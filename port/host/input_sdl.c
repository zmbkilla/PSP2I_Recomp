/* Game controllers and joysticks through SDL3. See input_sdl.h.
 *
 * ## Loading
 * Only SDL3.dll ships with the port (no SDL headers or import library), so
 * the handful of functions used here are declared with their SDL3 signatures
 * and resolved with GetProcAddress. The types involved are opaque pointers,
 * 32-bit ids, Sint16/Uint8 values and C bool, so no SDL structure layout is
 * assumed. The DLL is looked up next to the exe first, then on the normal
 * search path. Without it the port runs keyboard-only.
 *
 * ## Mapping
 * Devices SDL recognises as gamepads (its controller database: XInput pads,
 * DualShock/DualSense, Switch Pro, most DirectInput pads) are mapped by
 * button POSITION, as the PSP's face buttons are:
 *
 *     south -> Cross     east -> Circle     west -> Square    north -> Triangle
 *     D-pad -> D-pad     left/right shoulder or trigger (> 50%) -> L / R
 *     Back -> Select     Start -> Start     left stick -> analog stick
 *
 * The right stick, stick clicks and Guide have no PSP equivalent and are left
 * unmapped. The analog stick uses a radial dead zone (PSP2I_DEADZONE, percent,
 * default 20) rescaled so the edge of the zone is centre, then 0..255.
 *
 * Joysticks SDL does not recognise as gamepads get hat 0 as the D-pad, axes
 * 0/1 as the analog stick (which also drives the D-pad past 50%, as menus
 * read digital input), and buttons by a default table that
 * PSP2I_JOYSTICK_MAP overrides: "0:square,1:cross,2:circle,3:triangle,...".
 *
 * ## Diagnostics
 * Every connect/disconnect is logged on one line (name, gamepad or joystick,
 * SDL id). PSP2I_INPUT_TRACE=1 also logs the combined PSP state whenever it
 * changes. */

#include "input_sdl.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef uint32_t SDL_JoystickID;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Joystick SDL_Joystick;

#define SDL_INIT_JOYSTICK 0x00000200u
#define SDL_INIT_GAMEPAD  0x00002000u

/* SDL_GamepadButton / SDL_GamepadAxis values (SDL3). */
enum { GB_SOUTH, GB_EAST, GB_WEST, GB_NORTH, GB_BACK, GB_GUIDE, GB_START, GB_LSTICK, GB_RSTICK,
       GB_LSHOULDER, GB_RSHOULDER, GB_DPAD_UP, GB_DPAD_DOWN, GB_DPAD_LEFT, GB_DPAD_RIGHT };
enum { GA_LEFTX, GA_LEFTY, GA_RIGHTX, GA_RIGHTY, GA_LTRIGGER, GA_RTRIGGER };
#define SDL_HAT_UP    0x01
#define SDL_HAT_RIGHT 0x02
#define SDL_HAT_DOWN  0x04
#define SDL_HAT_LEFT  0x08

static struct {
    bool            (*Init)(uint32_t);
    void            (*Quit)(void);
    const char     *(*GetError)(void);
    int             (*GetVersion)(void);
    bool            (*SetHint)(const char *, const char *);
    void            (*UpdateGamepads)(void);
    void            (*FlushEvents)(uint32_t, uint32_t);
    SDL_JoystickID *(*GetJoysticks)(int *);
    bool            (*IsGamepad)(SDL_JoystickID);
    void            (*free)(void *);
    SDL_Gamepad    *(*OpenGamepad)(SDL_JoystickID);
    void            (*CloseGamepad)(SDL_Gamepad *);
    bool            (*GamepadConnected)(SDL_Gamepad *);
    const char     *(*GetGamepadName)(SDL_Gamepad *);
    bool            (*GetGamepadButton)(SDL_Gamepad *, int);
    int16_t         (*GetGamepadAxis)(SDL_Gamepad *, int);
    SDL_Joystick   *(*OpenJoystick)(SDL_JoystickID);
    void            (*CloseJoystick)(SDL_Joystick *);
    bool            (*JoystickConnected)(SDL_Joystick *);
    const char     *(*GetJoystickName)(SDL_Joystick *);
    int             (*GetNumJoystickButtons)(SDL_Joystick *);
    int             (*GetNumJoystickAxes)(SDL_Joystick *);
    int             (*GetNumJoystickHats)(SDL_Joystick *);
    bool            (*GetJoystickButton)(SDL_Joystick *, int);
    int16_t         (*GetJoystickAxis)(SDL_Joystick *, int);
    uint8_t         (*GetJoystickHat)(SDL_Joystick *, int);
} S;

static HMODULE g_dll;
static int     g_active;

/* PSP button bits. */
#define P_SELECT   0x0001u
#define P_START    0x0008u
#define P_UP       0x0010u
#define P_RIGHT    0x0020u
#define P_DOWN     0x0040u
#define P_LEFT     0x0080u
#define P_L        0x0100u
#define P_R        0x0200u
#define P_TRIANGLE 0x1000u
#define P_CIRCLE   0x2000u
#define P_CROSS    0x4000u
#define P_SQUARE   0x8000u

#define MAX_DEV 8
typedef struct {
    SDL_JoystickID id;
    SDL_Gamepad   *pad;      /* one of pad / joy is set */
    SDL_Joystick  *joy;
} device;
static device g_dev[MAX_DEV];
static int    g_ndev;

#define JOY_MAP_MAX 32
static uint32_t g_joymap[JOY_MAP_MAX];
static float    g_deadzone = 0.20f;
static int      g_trace;
static uint64_t g_polls;

static const struct { const char *name; uint32_t bit; } NAMES[] = {
    { "select", P_SELECT }, { "start", P_START }, { "up", P_UP }, { "right", P_RIGHT },
    { "down", P_DOWN }, { "left", P_LEFT }, { "l", P_L }, { "r", P_R },
    { "triangle", P_TRIANGLE }, { "circle", P_CIRCLE }, { "cross", P_CROSS }, { "square", P_SQUARE },
};

/* "0:square,1:cross,..." -- index:psp-button pairs; unknown names are
 * reported and skipped. */
static void parse_joymap(const char *spec) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int idx;
        char name[32];
        if (sscanf(tok, " %d : %31[a-z]", &idx, name) != 2 || idx < 0 || idx >= JOY_MAP_MAX) {
            fprintf(stderr, "input: PSP2I_JOYSTICK_MAP entry '%s' ignored (want index:button)\n", tok);
            continue;
        }
        uint32_t bit = 0;
        for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
            if (!strcmp(name, NAMES[i].name)) bit = NAMES[i].bit;
        if (!bit) { fprintf(stderr, "input: PSP2I_JOYSTICK_MAP button '%s' unknown\n", name); continue; }
        g_joymap[idx] = bit;
    }
}

static int load(void) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    if (n && n < sizeof path) {
        char *slash = strrchr(path, '\\');
        if (slash) { strcpy(slash + 1, "SDL3.dll"); g_dll = LoadLibraryA(path); }
    }
    if (!g_dll) g_dll = LoadLibraryA("SDL3.dll");
    if (!g_dll) return -1;
#define GET(field, name) do { *(FARPROC *)&S.field = GetProcAddress(g_dll, name); \
        if (!S.field) { fprintf(stderr, "input: SDL3.dll lacks %s\n", name); return -1; } } while (0)
    GET(Init, "SDL_Init");                 GET(Quit, "SDL_Quit");
    GET(GetError, "SDL_GetError");         GET(GetVersion, "SDL_GetVersion");
    GET(SetHint, "SDL_SetHint");           GET(UpdateGamepads, "SDL_UpdateGamepads");
    GET(FlushEvents, "SDL_FlushEvents");   GET(GetJoysticks, "SDL_GetJoysticks");
    GET(IsGamepad, "SDL_IsGamepad");       GET(free, "SDL_free");
    GET(OpenGamepad, "SDL_OpenGamepad");   GET(CloseGamepad, "SDL_CloseGamepad");
    GET(GamepadConnected, "SDL_GamepadConnected");
    GET(GetGamepadName, "SDL_GetGamepadName");
    GET(GetGamepadButton, "SDL_GetGamepadButton");
    GET(GetGamepadAxis, "SDL_GetGamepadAxis");
    GET(OpenJoystick, "SDL_OpenJoystick"); GET(CloseJoystick, "SDL_CloseJoystick");
    GET(JoystickConnected, "SDL_JoystickConnected");
    GET(GetJoystickName, "SDL_GetJoystickName");
    GET(GetNumJoystickButtons, "SDL_GetNumJoystickButtons");
    GET(GetNumJoystickAxes, "SDL_GetNumJoystickAxes");
    GET(GetNumJoystickHats, "SDL_GetNumJoystickHats");
    GET(GetJoystickButton, "SDL_GetJoystickButton");
    GET(GetJoystickAxis, "SDL_GetJoystickAxis");
    GET(GetJoystickHat, "SDL_GetJoystickHat");
#undef GET
    return 0;
}

int input_sdl_init(void) {
    if (g_active) return 0;
    if (load() != 0) {
        fprintf(stderr, "input: SDL3.dll not available; keyboard only\n");
        if (g_dll) { FreeLibrary(g_dll); g_dll = NULL; }
        return -1;
    }
    /* The window is Win32, not SDL: keep reading pads while it is not the
     * foreground window too (a console or debugger often is). */
    S.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    if (!S.Init(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        fprintf(stderr, "input: SDL_Init failed: %s; keyboard only\n", S.GetError());
        FreeLibrary(g_dll);
        g_dll = NULL;
        return -1;
    }
    const int v = S.GetVersion();
    fprintf(stderr, "input: SDL %d.%d.%d loaded; gamepads and joysticks enabled\n",
            v / 1000000, (v / 1000) % 1000, v % 1000);

    static const uint32_t def[12] = { P_SQUARE, P_CROSS, P_CIRCLE, P_TRIANGLE, P_L, P_R, P_L, P_R,
                                      P_SELECT, P_START, 0, 0 };
    memcpy(g_joymap, def, sizeof def);
    const char *m = getenv("PSP2I_JOYSTICK_MAP");
    if (m) parse_joymap(m);
    const char *dz = getenv("PSP2I_DEADZONE");
    if (dz) { float f = (float)atof(dz) / 100.0f; if (f >= 0.0f && f < 0.95f) g_deadzone = f; }
    g_trace = getenv("PSP2I_INPUT_TRACE") != NULL;
    g_active = 1;
    return 0;
}

static void close_dev(int i, const char *why) {
    device *d = &g_dev[i];
    fprintf(stderr, "input: %s %u %s\n", d->pad ? "gamepad" : "joystick", (unsigned)d->id, why);
    if (d->pad) S.CloseGamepad(d->pad);
    if (d->joy) S.CloseJoystick(d->joy);
    g_dev[i] = g_dev[--g_ndev];
}

/* Bring the open set in line with what is connected. */
static void rescan(void) {
    for (int i = g_ndev - 1; i >= 0; i--) {
        const int ok = g_dev[i].pad ? S.GamepadConnected(g_dev[i].pad) : S.JoystickConnected(g_dev[i].joy);
        if (!ok) close_dev(i, "disconnected");
    }
    int n = 0;
    SDL_JoystickID *ids = S.GetJoysticks(&n);
    if (!ids) return;
    for (int k = 0; k < n; k++) {
        int have = 0;
        for (int i = 0; i < g_ndev; i++) if (g_dev[i].id == ids[k]) have = 1;
        if (have || g_ndev == MAX_DEV) continue;
        device d;
        memset(&d, 0, sizeof d);
        d.id = ids[k];
        if (S.IsGamepad(ids[k])) d.pad = S.OpenGamepad(ids[k]);
        else d.joy = S.OpenJoystick(ids[k]);
        if (!d.pad && !d.joy) {
            fprintf(stderr, "input: device %u could not be opened: %s\n", (unsigned)ids[k], S.GetError());
            continue;
        }
        if (d.pad) {
            const char *name = S.GetGamepadName(d.pad);
            fprintf(stderr, "input: gamepad %u connected: %s (positional mapping)\n",
                    (unsigned)d.id, name ? name : "?");
        } else {
            const char *name = S.GetJoystickName(d.joy);
            fprintf(stderr, "input: joystick %u connected: %s (%d buttons, %d axes, %d hats; "
                            "generic mapping, see PSP2I_JOYSTICK_MAP)\n",
                    (unsigned)d.id, name ? name : "?", S.GetNumJoystickButtons(d.joy),
                    S.GetNumJoystickAxes(d.joy), S.GetNumJoystickHats(d.joy));
        }
        g_dev[g_ndev++] = d;
    }
    S.free(ids);
}

/* Radial dead zone, rescaled; x/y in -1..1, out in -1..1. */
static void deadzone(float *x, float *y) {
    float m = sqrtf(*x * *x + *y * *y);
    if (m <= g_deadzone) { *x = *y = 0.0f; return; }
    float s = (m - g_deadzone) / (1.0f - g_deadzone) / m;
    if (m > 1.0f) s = 1.0f / m;           /* corners of square gates */
    *x *= s; *y *= s;
}

void input_sdl_shutdown(void) {
    if (!g_active) return;
    while (g_ndev) close_dev(g_ndev - 1, "closed");
    S.Quit();
    FreeLibrary(g_dll);
    g_dll = NULL;
    g_active = 0;
}

int input_sdl_poll(uint32_t *buttons, uint8_t *ax, uint8_t *ay) {
    *buttons = 0; *ax = 128; *ay = 128;
    if (!g_active) return 0;
    S.UpdateGamepads();                    /* also detects added/removed devices */
    if (g_polls++ % 30 == 0) rescan();     /* twice a second is prompt enough */
    S.FlushEvents(0, 0xFFFF);              /* nothing reads SDL's queue; keep it empty */

    uint32_t bits = 0;
    float bx = 0.0f, by = 0.0f;            /* the most deflected stick wins */
    int16_t rx = 0, ry = 0;                /* its raw SDL values, for the trace */
    for (int i = 0; i < g_ndev; i++) {
        device *d = &g_dev[i];
        float x = 0.0f, y = 0.0f;
        int16_t sx = 0, sy = 0;
        if (d->pad) {
            SDL_Gamepad *p = d->pad;
            static const struct { int b; uint32_t bit; } MAP[] = {
                { GB_SOUTH, P_CROSS }, { GB_EAST, P_CIRCLE }, { GB_WEST, P_SQUARE }, { GB_NORTH, P_TRIANGLE },
                { GB_BACK, P_SELECT }, { GB_START, P_START }, { GB_LSHOULDER, P_L }, { GB_RSHOULDER, P_R },
                { GB_DPAD_UP, P_UP }, { GB_DPAD_DOWN, P_DOWN }, { GB_DPAD_LEFT, P_LEFT }, { GB_DPAD_RIGHT, P_RIGHT },
            };
            for (size_t k = 0; k < sizeof MAP / sizeof MAP[0]; k++)
                if (S.GetGamepadButton(p, MAP[k].b)) bits |= MAP[k].bit;
            if (S.GetGamepadAxis(p, GA_LTRIGGER) > 16384) bits |= P_L;
            if (S.GetGamepadAxis(p, GA_RTRIGGER) > 16384) bits |= P_R;
            sx = S.GetGamepadAxis(p, GA_LEFTX);
            sy = S.GetGamepadAxis(p, GA_LEFTY);
            x = (float)sx / 32767.0f;
            y = (float)sy / 32767.0f;
        } else {
            SDL_Joystick *j = d->joy;
            const int nb = S.GetNumJoystickButtons(j);
            for (int b = 0; b < nb && b < JOY_MAP_MAX; b++)
                if (g_joymap[b] && S.GetJoystickButton(j, b)) bits |= g_joymap[b];
            if (S.GetNumJoystickHats(j) > 0) {
                const uint8_t h = S.GetJoystickHat(j, 0);
                if (h & SDL_HAT_UP) bits |= P_UP;
                if (h & SDL_HAT_DOWN) bits |= P_DOWN;
                if (h & SDL_HAT_LEFT) bits |= P_LEFT;
                if (h & SDL_HAT_RIGHT) bits |= P_RIGHT;
            }
            if (S.GetNumJoystickAxes(j) >= 2) {
                sx = S.GetJoystickAxis(j, 0);
                sy = S.GetJoystickAxis(j, 1);
                x = (float)sx / 32767.0f;
                y = (float)sy / 32767.0f;
                if (x < -0.5f) bits |= P_LEFT;
                if (x > 0.5f) bits |= P_RIGHT;
                if (y < -0.5f) bits |= P_UP;
                if (y > 0.5f) bits |= P_DOWN;
            }
        }
        deadzone(&x, &y);
        if (x * x + y * y > bx * bx + by * by) { bx = x; by = y; rx = sx; ry = sy; }
    }
    /* PSP: 0 = left/up, 255 = right/down, 128 = centre (SDL's y also grows
     * downward). */
    int ix = 128 + (int)(bx * 127.5f), iy = 128 + (int)(by * 127.5f);
    *ax = (uint8_t)(ix < 0 ? 0 : ix > 255 ? 255 : ix);
    *ay = (uint8_t)(iy < 0 ? 0 : iy > 255 ? 255 : iy);
    *buttons = bits;

    if (g_trace) {
        static uint32_t lb = 0xFFFFFFFFu;
        static uint8_t lx, ly;
        if (bits != lb || abs((int)*ax - lx) > 8 || abs((int)*ay - ly) > 8) {
            /* raw: SDL's axes as read; |raw|: their magnitude (1.0 = full
             * deflection on a circular gate); psp: what the game reads. */
            const float rm = sqrtf((float)rx * rx + (float)ry * ry) / 32767.0f;
            fprintf(stderr, "input: pad buttons 0x%04X raw %6d,%6d |raw| %.3f psp %3u,%3u\n",
                    bits, rx, ry, rm, *ax, *ay);
            lb = bits; lx = *ax; ly = *ay;
        }
    }
    return 1;
}

#else  /* !_WIN32 */
int  input_sdl_init(void) { return -1; }
void input_sdl_shutdown(void) {}
int  input_sdl_poll(uint32_t *buttons, uint8_t *ax, uint8_t *ay) { *buttons = 0; *ax = *ay = 128; return 0; }
#endif
