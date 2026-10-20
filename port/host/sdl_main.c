/* PSP2i on Android (and other SDL3 platforms): the SDL3 host.
 *
 * The Windows host (main.c) is Win32 throughout; this one is SDL3 throughout
 * and carries only what playing needs:
 *
 *   - the window and an OpenGL ES 3.0 context (desktop GL 3.3 elsewhere),
 *     with the GE rendered on the GPU by gl_ge.c, the frame scaled to fit;
 *   - touch controls drawn over the picture (an analog stick, the D-pad, the
 *     face buttons, L/R, START/SELECT), hidden while a game controller is in
 *     use; controllers through SDL's gamepad API, the right stick turning the
 *     camera (camera.c);
 *   - sound: each PSP audio channel on its own SDL audio stream; ATRAC music
 *     through libpsp2i_atrac.so;
 *   - frame rate (framerate.c): 30 as the game ships, or 60.
 *
 * Files, on Android in the data folder the player chooses at the first start,
 * where any file manager and USB reach them (PSP2iActivity.dataFolder: asks
 * for storage access and the folder, and copies over what older versions kept
 * in Android/data/<package>/files or PSP2i/; quitting there closes the app);
 * elsewhere next to the executable or in $PSP2I_DATA:
 *
 *   the game           an ISO: on Android picked on first start (remembered),
 *                      elsewhere psp2i.ini iso=<path>; or the extracted files
 *                      in GameData/disc/ (PSP_GAME/...)
 *   GameData/flash/    the PSP fonts (on Android copied in from a folder the
 *                      user picks, once)
 *   EBOOT.BIN          optional: a decrypted EBOOT, when the disc's is encrypted
 *   GameData/ms/       the memory stick (saves), created on first save
 *   psp2i.ini          the settings menu's choices (written when one changes): fps=,
 *                      fps_counter=, right_stick=on/off, rs_speed=, volume_master= / _music= / _sfx= (0-100),
 *                      swap_circle_cross=on (keys X/Z and controllers; the touch buttons
 *                      keep their symbols), touch_color=, touch_opacity= (10-100); ad hoc
 *                      multiplayer: adhoc_mode=modern (default) | ppsspp_direct |
 *                      ppsspp_relay, modern_server=host (default) or the hosting player's
 *                      address (IPv4/IPv6), adhoc_server= (PPSSPP modes); by hand only:
 *                      touch=off, nickname=, iso=
 *   logs/psp2i_log.txt the log of the last run
 *
 * The settings menu (menu.c, as on Windows): the Back button, R3 or Guide on
 * a controller, the MENU touch button, or F1 on a desktop; items this host
 * cannot do are left out.
 *
 * Optional PSP2I_LIBAV supplies FFmpeg movie video on desktop builds.
 * Not here yet: online play, movie video without that
 * backend (movies play black with sound), and save encryption (saves are written unencrypted, so
 * they do not move to and from a PSP or the Windows build). */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/net.h>

#include "atrac_at3.h"
#ifdef PSP2I_LIBAV
#include "h264_libav.h"
#endif
#include "camera.h"
#include "framerate.h"
#include "gl_ge.h"
#include "gl_load.h"
#include "menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SCREEN_W 480
#define SCREEN_H 272
#define USER_PARTITION_TOP 0x0A000000u

void psp_recomp_register(void);          /* the generated code (port/gen), or gen_stub.c */

static char g_base[1024];                /* where the files live */
static SDL_Window *g_win;
static SDL_GLContext g_gl;

static void fail(const char *msg) {
    fprintf(stderr, "psp2i: %s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PSP2i", msg, g_win);
}

/* ---- the game's ELF ------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf32_Ehdr;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } Elf32_Phdr;
typedef struct { uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; } Elf32_Shdr;
#pragma pack(pop)

typedef struct { uint32_t entry, gp, load_hi; char name[29]; } module_info;

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

/* Place the PT_LOAD segments; $gp and the name from .rodata.sceModuleInfo. */
/* (The ELF's bytes are taken and freed.) -2 if it is not a decrypted ELF. */
static int load_elf_buf(uint8_t *b, size_t len, module_info *mi) {
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)b;
    if (len < sizeof *eh || memcmp(eh->ident, "\x7F" "ELF", 4) != 0 || eh->machine != 8) { free(b); return -2; }
    memset(mi, 0, sizeof *mi);
    mi->entry = eh->entry;
    for (unsigned i = 0; i < eh->phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(b + eh->phoff + i * eh->phentsize);
        if (ph->type != 1) continue;
        if ((size_t)ph->offset + ph->filesz > len || psp_mem_write_block(ph->vaddr, b + ph->offset, ph->filesz) != 0) { free(b); return -2; }
        for (uint32_t a = ph->vaddr + ph->filesz; a < ph->vaddr + ph->memsz; a++) psp_write8(a, 0);
        if (ph->vaddr + ph->memsz > mi->load_hi) mi->load_hi = ph->vaddr + ph->memsz;
    }
    if (eh->shoff && eh->shstrndx < eh->shnum) {
        const Elf32_Shdr *sh = (const Elf32_Shdr *)(b + eh->shoff);
        const char *strtab = (const char *)(b + sh[eh->shstrndx].offset);
        for (unsigned i = 0; i < eh->shnum; i++) {
            if (strcmp(strtab + sh[i].name, ".rodata.sceModuleInfo") != 0) continue;
            memcpy(mi->name, b + sh[i].offset + 4, 28);
            memcpy(&mi->gp, b + sh[i].offset + 32, 4);
        }
    }
    free(b);
    return mi->gp ? 0 : -2;
}

/* ---- hooks the generated code calls ------------------------------------- */

void psp_syscall(uint32_t id) {
    fprintf(stderr, "psp2i: raw syscall 0x%05X reached\n", id);
    psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED);
}

void psp_unimplemented(uint32_t addr, const char *what) {
    static uint32_t seen[256];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == addr) return;
    if (n < 256) seen[n++] = addr;
    fprintf(stderr, "psp2i: untranslated instruction at 0x%08X: %s\n", addr, what ? what : "?");
}

/* ---- sound ---------------------------------------------------------------- */

#define AUDIO_CHANNELS 8
static SDL_AudioStream *g_stream[AUDIO_CHANNELS];

static float g_master = 1.0f;            /* the menu's MASTER VOLUME, on every stream */

static void audio_sink(int ch, const int16_t *pcm, uint32_t frames) {
    if (ch < 0 || ch >= AUDIO_CHANNELS) return;
    if (!g_stream[ch]) {
        const SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, 44100 };
        g_stream[ch] = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!g_stream[ch]) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return; }
        SDL_SetAudioStreamGain(g_stream[ch], g_master);
        SDL_ResumeAudioStreamDevice(g_stream[ch]);
    }
    /* the runtime paces the game to real time; a long queue means a stall */
    if (SDL_GetAudioStreamQueued(g_stream[ch]) > 44100 * 4 / 4) SDL_ClearAudioStream(g_stream[ch]);
    SDL_PutAudioStreamData(g_stream[ch], pcm, (int)(frames * 4));
}

/* ---- input: touch controls and game controllers ---------------------------- */

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

enum { SH_DISC, SH_RING, SH_RECT };
typedef struct {
    int      shape;
    uint32_t bit;            /* 0: the analog stick */
    float    cx, cy, rx, ry; /* pixels, from the top left */
    float    r, g, b;
} control;

enum { C_STICK, C_UP, C_DOWN, C_LEFT, C_RIGHT, C_TRI, C_CIR, C_CRO, C_SQU, C_L, C_R, C_SELECT, C_START, C_MENU, C_COUNT };
static control g_ctl[C_COUNT];
static int g_lay_w, g_lay_h;

static void layout(int w, int h) {
    if (w == g_lay_w && h == g_lay_h) return;
    g_lay_w = w; g_lay_h = h;
    const float u = (float)(h < w ? h : w);
    const float lx = 0.22f * u, fx = (float)w - 0.24f * u;
    #define SET(i, sh, bt, x, y, sx, sy, R, G, B) g_ctl[i] = (control){ sh, bt, x, y, sx, sy, R, G, B }
    SET(C_STICK, SH_RING, 0, lx, (float)h - 0.25f * u, 0.15f * u, 0.15f * u, 0.9f, 0.9f, 0.9f);
    const float dy = (float)h - 0.64f * u, d = 0.085f * u, k = 0.045f * u;
    SET(C_UP,    SH_RECT, P_UP,    lx, dy - d, k, k, 0.9f, 0.9f, 0.9f);
    SET(C_DOWN,  SH_RECT, P_DOWN,  lx, dy + d, k, k, 0.9f, 0.9f, 0.9f);
    SET(C_LEFT,  SH_RECT, P_LEFT,  lx - d, dy, k, k, 0.9f, 0.9f, 0.9f);
    SET(C_RIGHT, SH_RECT, P_RIGHT, lx + d, dy, k, k, 0.9f, 0.9f, 0.9f);
    const float fy = (float)h - 0.32f * u, o = 0.1f * u, br = 0.052f * u;
    SET(C_TRI, SH_DISC, P_TRIANGLE, fx, fy - o, br, br, 0.35f, 0.85f, 0.6f);
    SET(C_CIR, SH_DISC, P_CIRCLE,   fx + o, fy, br, br, 0.95f, 0.4f, 0.45f);
    SET(C_CRO, SH_DISC, P_CROSS,    fx, fy + o, br, br, 0.5f, 0.65f, 1.0f);
    SET(C_SQU, SH_DISC, P_SQUARE,   fx - o, fy, br, br, 0.95f, 0.55f, 0.85f);
    SET(C_L, SH_RECT, P_L, 0.17f * u, 0.08f * u, 0.13f * u, 0.05f * u, 0.9f, 0.9f, 0.9f);
    SET(C_R, SH_RECT, P_R, (float)w - 0.17f * u, 0.08f * u, 0.13f * u, 0.05f * u, 0.9f, 0.9f, 0.9f);
    SET(C_SELECT, SH_RECT, P_SELECT, (float)w * 0.5f - 0.1f * u, (float)h - 0.06f * u, 0.065f * u, 0.028f * u, 0.9f, 0.9f, 0.9f);
    SET(C_START,  SH_RECT, P_START,  (float)w * 0.5f + 0.1f * u, (float)h - 0.06f * u, 0.065f * u, 0.028f * u, 0.9f, 0.9f, 0.9f);
    SET(C_MENU,   SH_RECT, 0,        (float)w * 0.5f, 0.05f * u, 0.07f * u, 0.03f * u, 0.9f, 0.9f, 0.9f);   /* the settings menu */
    #undef SET
}

#define MAX_FINGERS 10
static struct { SDL_FingerID id; float x, y; int used, stick, menu; } g_finger[MAX_FINGERS];
static int g_touch_on = 1;               /* psp2i.ini touch=off hides them for good */
static int g_fps_on = 1;                 /* psp2i.ini fps_counter=off hides the counter */
static int g_swap_oc;                    /* psp2i.ini swap_circle_cross=on: Circle <-> Cross */
static fps_meter g_fm;
static int g_touch_shown = 1;            /* hidden while a controller is used */
static uint32_t g_touch_bits;
static float g_knob_x, g_knob_y;         /* -1..1 */
static menu_state g_menu;                /* the settings menu */
static uint32_t g_menu_latched;          /* MI_TOGGLE from Back / F1 / the MENU button since the last vblank */
static int g_menu_fx;                    /* MFX_* from taps on the open menu */
static int g_vx, g_vy, g_vw, g_vh;       /* the picture's rectangle in the window */

static int hit(const control *c, float x, float y) {
    const float sx = c->rx * 1.35f, sy = c->ry * 1.35f;   /* generous: thumbs are wide */
    if (c->shape == SH_RECT) return fabsf(x - c->cx) <= sx && fabsf(y - c->cy) <= sy;
    const float dx = (x - c->cx) / sx, dy = (y - c->cy) / sy;
    return dx * dx + dy * dy <= 1.0f;
}

static void touch_event(const SDL_TouchFingerEvent *e, int down, int up) {
    const float x = e->x * (float)g_lay_w, y = e->y * (float)g_lay_h;
    int i = 0;
    while (i < MAX_FINGERS && !(g_finger[i].used && g_finger[i].id == e->fingerID)) i++;
    if (i == MAX_FINGERS) {
        if (!down) return;
        for (i = 0; i < MAX_FINGERS && g_finger[i].used; i++) {}
        if (i == MAX_FINGERS) return;
        g_finger[i].used = 1;
        g_finger[i].id = e->fingerID;
        const control *s = &g_ctl[C_STICK];
        const float dx = (x - s->cx) / (s->rx * 1.6f), dy = (y - s->cy) / (s->ry * 1.6f);
        g_finger[i].stick = dx * dx + dy * dy <= 1.0f;   /* this finger drives the stick until it lifts */
        g_finger[i].menu = 0;
        if (g_menu.open) {                               /* a tap on the menu, in its 480x272 picture */
            g_finger[i].stick = 0;
            g_finger[i].menu = 1;
            if (g_vw > 0 && g_vh > 0)
                g_menu_fx |= menu_touch(&g_menu, SCREEN_W, SCREEN_H, (int)((x - (float)g_vx) * SCREEN_W / (float)g_vw),
                                        (int)((y - (float)g_vy) * SCREEN_H / (float)g_vh));
        } else if (g_touch_on && g_touch_shown && hit(&g_ctl[C_MENU], x, y)) {
            g_finger[i].stick = 0;
            g_finger[i].menu = 1;
            g_menu_latched |= MI_TOGGLE;
        }
        g_touch_shown = g_touch_on;
    }
    g_finger[i].x = x;
    g_finger[i].y = y;
    if (up) g_finger[i].used = 0;
}

static void touch_state(uint8_t *ax, uint8_t *ay) {
    uint32_t bits = 0;
    float kx = 0.0f, ky = 0.0f;
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!g_finger[i].used || g_finger[i].menu) continue;    /* a finger on the menu is not the game's */
        if (g_finger[i].stick) {
            const control *s = &g_ctl[C_STICK];
            kx = (g_finger[i].x - s->cx) / s->rx;
            ky = (g_finger[i].y - s->cy) / s->ry;
            const float m = sqrtf(kx * kx + ky * ky);
            if (m > 1.0f) { kx /= m; ky /= m; }
            continue;
        }
        for (int c = C_UP; c < C_MENU; c++) if (hit(&g_ctl[c], g_finger[i].x, g_finger[i].y)) bits |= g_ctl[c].bit;
    }
    g_touch_bits = g_touch_on ? bits : 0;
    g_knob_x = kx; g_knob_y = ky;
    *ax = (uint8_t)(128 + (int)(kx * 127.0f));
    *ay = (uint8_t)(128 + (int)(ky * 127.0f));
}

#define MAX_PADS 4
static SDL_Gamepad *g_pad[MAX_PADS];

static uint8_t stick_byte(int16_t v) { return (uint8_t)((v + 32768) >> 8); }
static int g_pad_toggle;                 /* R3 or Guide held on a controller: the settings menu */

/* Buttons and the left stick of every controller; the most deflected stick wins. */
static uint32_t pad_state(uint8_t *ax, uint8_t *ay, uint8_t *rx, uint8_t *ry) {
    static const struct { SDL_GamepadButton b; uint32_t bit; } MAP[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH, P_CROSS }, { SDL_GAMEPAD_BUTTON_EAST, P_CIRCLE },
        { SDL_GAMEPAD_BUTTON_WEST, P_SQUARE }, { SDL_GAMEPAD_BUTTON_NORTH, P_TRIANGLE },
        { SDL_GAMEPAD_BUTTON_BACK, P_SELECT }, { SDL_GAMEPAD_BUTTON_START, P_START },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, P_L }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, P_R },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, P_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, P_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, P_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, P_RIGHT },
    };
    uint32_t bits = 0;
    g_pad_toggle = 0;
#ifndef SDL_PLATFORM_ANDROID
    const bool *keys = SDL_GetKeyboardState(NULL);
    if (SDL_GetKeyboardFocus() == g_win) {
        static const struct { SDL_Scancode key; uint32_t bit; } keyboard[] = {
            {SDL_SCANCODE_UP, P_UP}, {SDL_SCANCODE_DOWN, P_DOWN},
            {SDL_SCANCODE_LEFT, P_LEFT}, {SDL_SCANCODE_RIGHT, P_RIGHT},
            {SDL_SCANCODE_X, P_CIRCLE}, {SDL_SCANCODE_Z, P_CROSS},     /* as the Windows build */
            {SDL_SCANCODE_A, P_SQUARE}, {SDL_SCANCODE_S, P_TRIANGLE},
            {SDL_SCANCODE_Q, P_L}, {SDL_SCANCODE_E, P_R},
            {SDL_SCANCODE_RETURN, P_START}, {SDL_SCANCODE_RSHIFT, P_SELECT}
        };
        for (size_t k = 0; k < sizeof keyboard / sizeof keyboard[0]; ++k)
            if (keys[keyboard[k].key]) bits |= keyboard[k].bit;
        *ax = 128 + 127 * (keys[SDL_SCANCODE_L] - keys[SDL_SCANCODE_J]);
        *ay = 128 + 127 * (keys[SDL_SCANCODE_K] - keys[SDL_SCANCODE_I]);
    }
#endif
    int best = 6000, rbest = 6000;          /* a dead zone */
    for (int i = 0; i < MAX_PADS; i++) {
        SDL_Gamepad *p = g_pad[i];
        if (!p) continue;
        for (size_t k = 0; k < sizeof MAP / sizeof MAP[0]; k++) if (SDL_GetGamepadButton(p, MAP[k].b)) bits |= MAP[k].bit;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) bits |= P_L;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) bits |= P_R;
        const int16_t x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
        if (abs(x) + abs(y) > best) { best = abs(x) + abs(y); *ax = stick_byte(x); *ay = stick_byte(y); }
        const int16_t u = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), v = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY);
        if (abs(u) + abs(v) > rbest) { rbest = abs(u) + abs(v); *rx = stick_byte(u); *ry = stick_byte(v); }
        if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_STICK) || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_GUIDE))
            g_pad_toggle = 1;
    }
    if (bits || best > 6000 || rbest > 6000) g_touch_shown = 0;   /* a controller is in use */
    if (g_swap_oc && ((bits & P_CIRCLE) != 0) != ((bits & P_CROSS) != 0)) bits ^= P_CIRCLE | P_CROSS;
    return bits;
}

static void pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: psp_request_exit(); break;
        case SDL_EVENT_KEY_DOWN:                         /* Android's Back, or F1: the settings menu */
            if (!e.key.repeat && (e.key.key == SDLK_AC_BACK || e.key.key == SDLK_F1)) g_menu_latched |= MI_TOGGLE;
            break;
        case SDL_EVENT_FINGER_DOWN:   touch_event(&e.tfinger, 1, 0); break;
        case SDL_EVENT_FINGER_MOTION: touch_event(&e.tfinger, 0, 0); break;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: touch_event(&e.tfinger, 0, 1); break;
        case SDL_EVENT_GAMEPAD_ADDED:
            for (int i = 0; i < MAX_PADS; i++) if (!g_pad[i]) { g_pad[i] = SDL_OpenGamepad(e.gdevice.which); break; }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (int i = 0; i < MAX_PADS; i++)
                if (g_pad[i] && SDL_GetGamepadID(g_pad[i]) == e.gdevice.which) { SDL_CloseGamepad(g_pad[i]); g_pad[i] = NULL; }
            break;
        default: break;
        }
    }
}

/* ---- drawing the touch controls ----------------------------------------------- */

static GLuint g_oprog, g_ovao;
static GLint u_rect, u_col, u_shape;

static const char *OVS =
GLSL_HEADER
"uniform vec4 R;\n"                              /* NDC x0, y0, x1, y1 */
"out vec2 v_l;\n"
"void main() {\n"
"  int i = gl_VertexID;\n"
"  vec2 c = vec2((i == 1 || i == 2 || i == 4) ? 1.0 : 0.0, (i == 2 || i == 4 || i == 5) ? 1.0 : 0.0);\n"
"  v_l = c * 2.0 - 1.0;\n"
"  gl_Position = vec4(mix(R.xy, R.zw, c), 0.0, 1.0);\n"
"}\n";
static const char *OFS =
GLSL_HEADER
"uniform vec4 C; uniform vec4 S;\n"              /* colour; shape, aspect, ring width */
"in vec2 v_l; out vec4 o_c;\n"
"void main() {\n"
"  float a;\n"
"  if (S.x < 0.5) a = 1.0 - smoothstep(0.88, 1.0, length(v_l));\n"
"  else if (S.x < 1.5) { float d = length(v_l); a = (1.0 - smoothstep(0.9, 1.0, d)) * smoothstep(0.9 - S.z, 1.0 - S.z, d); }\n"
"  else { vec2 q = abs(v_l) * vec2(S.y, 1.0); vec2 b = vec2(S.y, 1.0) - 0.5;\n"
"         a = 1.0 - smoothstep(0.8, 1.0, length(max(q - b, 0.0)) / 0.5); }\n"
"  o_c = vec4(C.rgb, C.a * a);\n"
"}\n";

static void draw_shape(int cw, int ch, int shape, float cx, float cy, float rx, float ry,
                       float r, float g, float b, float a) {
    glUniform4f(u_rect, (cx - rx) / (float)cw * 2.0f - 1.0f, 1.0f - (cy + ry) / (float)ch * 2.0f,
                (cx + rx) / (float)cw * 2.0f - 1.0f, 1.0f - (cy - ry) / (float)ch * 2.0f);
    glUniform4f(u_col, r, g, b, a);
    glUniform4f(u_shape, (float)shape, rx / ry, 0.12f, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* "FPS nn.n" (frames the game completed, on the host clock) in the corner of
 * the picture, in the settings menu's 5x7 font, scaled with the picture. */
static void draw_fps(int x, int y, int vw, int cw, int ch) {
    if (!g_fps_on) return;
    fps_meter_sample(&g_fm, SDL_GetTicksNS() / 1000u, psp_display_flips());
    enum { W = 64, H = 11 };
    static uint32_t px[W * H];
    char line[16];
    const double v = fps_meter_value(&g_fm);
    if (v < 0) snprintf(line, sizeof line, "FPS --.-");
    else       snprintf(line, sizeof line, "FPS %4.1f", v);
    for (int i = 0; i < W * H; i++) px[i] = 0xFF000000u;
    menu_image img = { px, W, H };
    menu_text(&img, 2, 2, line, 0xFFFFFF);
    const int s = vw / 480 > 1 ? vw / 480 : 1;           /* whole multiples of the PSP's pixels */
    gl_draw_overlay(px, W, H, x + s * 2, y + s * 2, W * s, H * s, cw, ch);
}

static void draw_controls(int cw, int ch) {
    if (!g_touch_on || !g_touch_shown) return;
    if (!g_oprog) {
        g_oprog = gl_program("touch", OVS, OFS);
        if (!g_oprog) { g_touch_on = 0; return; }
        u_rect = glGetUniformLocation(g_oprog, "R");
        u_col = glGetUniformLocation(g_oprog, "C");
        u_shape = glGetUniformLocation(g_oprog, "S");
        glGenVertexArrays(1, &g_ovao);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, cw, ch);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glColorMask(1, 1, 1, 1);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glUseProgram(g_oprog);
    glBindVertexArray(g_ovao);
    /* the menu's TOUCH COLOR (DEFAULT: each button its own) and TOUCH OPACITY */
    static const float PRESET[TOUCH_COLORS][3] = {
        { 0, 0, 0 }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.85f, 0.1f }, { 0.2f, 0.9f, 1.0f }, { 1.0f, 0.4f, 0.75f } };
    const int pc = g_menu.touch_color > TOUCH_DEFAULT && g_menu.touch_color < TOUCH_COLORS ? g_menu.touch_color : TOUCH_DEFAULT;
    const float base = (float)g_menu.touch_alpha / 10.0f, pressed = base * 2.1f > 1.0f ? 1.0f : base * 2.1f;
    for (int i = 0; i < C_COUNT; i++) {
        const control *c = &g_ctl[i];
        if (i == C_MENU && g_menu.open) continue;
        const float a = (c->bit && (g_touch_bits & c->bit)) ? pressed : base;
        if (pc) draw_shape(cw, ch, c->shape, c->cx, c->cy, c->rx, c->ry, PRESET[pc][0], PRESET[pc][1], PRESET[pc][2], a);
        else    draw_shape(cw, ch, c->shape, c->cx, c->cy, c->rx, c->ry, c->r, c->g, c->b, a);
    }
    const control *s = &g_ctl[C_STICK];
    const float kr = pc ? PRESET[pc][0] : 0.9f, kg = pc ? PRESET[pc][1] : 0.9f, kb = pc ? PRESET[pc][2] : 0.9f;
    draw_shape(cw, ch, SH_DISC, s->cx + g_knob_x * s->rx * 0.6f, s->cy + g_knob_y * s->ry * 0.6f,
               s->rx * 0.45f, s->ry * 0.45f, kr, kg, kb, base * 1.4f > 1.0f ? 1.0f : base * 1.4f);
    glDisable(GL_BLEND);
}

/* The settings menu over the picture: drawn at the PSP's 480x272 like on
 * Windows, and just its box scaled up with the picture (solid: the overlay is
 * not blended). */
static void draw_menu(void) {
    if (!g_menu.open || g_vw <= 0) return;
    static uint32_t px[SCREEN_W * SCREEN_H], box[SCREEN_W * SCREEN_H];
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) px[i] = 0xFF000000u;
    menu_image img = { px, SCREEN_W, SCREEN_H };
#ifdef SDL_PLATFORM_ANDROID
    menu_draw(&g_menu, &img, framerate_game_fps(), "BACK");
#else
    menu_draw(&g_menu, &img, framerate_game_fps(), "F1");
#endif
    int bx, by, bw, bh;
    menu_box(&g_menu, SCREEN_W, SCREEN_H, &bx, &by, &bw, &bh);
    if (bx < 0) { bw += bx; bx = 0; }
    if (by < 0) { bh += by; by = 0; }
    if (bx + bw > SCREEN_W) bw = SCREEN_W - bx;
    if (by + bh > SCREEN_H) bh = SCREEN_H - by;
    if (bw <= 0 || bh <= 0) return;
    for (int y = 0; y < bh; y++) memcpy(box + y * bw, px + (by + y) * SCREEN_W + bx, (size_t)bw * 4);
    int cw = 0, ch = 0;
    SDL_GetWindowSizeInPixels(g_win, &cw, &ch);
    gl_draw_overlay(box, bw, bh, g_vx + bx * g_vw / SCREEN_W, g_vy + by * g_vh / SCREEN_H,
                    bw * g_vw / SCREEN_W, bh * g_vh / SCREEN_H, cw, ch);
}

/* ---- the picture ---------------------------------------------------------------- */

static uint32_t g_frame[SCREEN_W * SCREEN_H];    /* 0xAARRGGBB */
static int g_have_frame;

/* The displayed framebuffer straight from VRAM (when it is current). */
static int grab_frame(uint32_t addr, uint32_t stride, uint32_t fmt) {
    const int bpp = fmt == 3 ? 4 : 2;
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            const uint32_t at = addr + (uint32_t)(y * (int)stride + x) * (uint32_t)bpp;
            const uint32_t p = bpp == 4 ? psp_read32(at) : psp_read16(at);
            uint32_t r, g, b;
            switch (fmt) {
            case 0:  r = (p & 0x1F) * 255 / 31; g = ((p >> 5) & 0x3F) * 255 / 63; b = ((p >> 11) & 0x1F) * 255 / 31; break;
            case 1:  r = (p & 0x1F) * 255 / 31; g = ((p >> 5) & 0x1F) * 255 / 31; b = ((p >> 10) & 0x1F) * 255 / 31; break;
            case 2:  r = (p & 0xF) * 17; g = ((p >> 4) & 0xF) * 17; b = ((p >> 8) & 0xF) * 17; break;
            default: r = p & 0xFF; g = (p >> 8) & 0xFF; b = (p >> 16) & 0xFF; break;
            }
            g_frame[y * SCREEN_W + x] = 0xFF000000u | r << 16 | g << 8 | b;
        }
    return 1;
}

static void present(void) {
    uint32_t addr, stride, fmt;
    psp_display_get(&addr, &stride, &fmt);
    int cw = 0, ch = 0;
    SDL_GetWindowSizeInPixels(g_win, &cw, &ch);
    if (cw <= 0 || ch <= 0) return;
    layout(cw, ch);
    /* the largest 480:272 rectangle that fits, centred */
    int vw = cw, vh = cw * SCREEN_H / SCREEN_W;
    if (vh > ch) { vh = ch; vw = ch * SCREEN_W / SCREEN_H; }
    /* the GPU's frame straight to the window when it is the current one (no
     * readback and re-upload: that round trip dominated the frame on phones) */
    if (!(addr && gl_ge_present_direct(addr, stride, (int)fmt, SCREEN_W, SCREEN_H,
                                       (cw - vw) / 2, (ch - vh) / 2, vw, vh, cw, ch))) {
        const int p = addr ? gl_ge_present(addr, stride, (int)fmt, g_frame, SCREEN_W, SCREEN_H) : 0;
        if (p == 1) g_have_frame = 1;
        else if (p == 0 && addr) g_have_frame = grab_frame(addr, stride, fmt);
        if (!g_have_frame) memset(g_frame, 0, sizeof g_frame);
        gl_blit_frame(g_frame, SCREEN_W, SCREEN_H, (cw - vw) / 2, (ch - vh) / 2, vw, vh, cw, ch);
    }
    g_vx = (cw - vw) / 2; g_vy = (ch - vh) / 2; g_vw = vw; g_vh = vh;
    draw_fps((cw - vw) / 2, (ch - vh) / 2, vw, cw, ch);
    draw_controls(cw, ch);
    draw_menu();
    SDL_GL_SwapWindow(g_win);
}

/* Once per vblank, on the scheduler's clock. */
static void menu_apply(int fx);

static void on_vblank(void) {
    pump_events();
    uint8_t ax = 128, ay = 128, rx = 128, ry = 128, tx, ty;
    const uint32_t pad = pad_state(&ax, &ay, &rx, &ry);
    const uint8_t pax = ax, pay = ay;                    /* the controllers' stick, for the menu */
    touch_state(&tx, &ty);
    if (tx != 128 || ty != 128) { ax = tx; ay = ty; }
    const uint32_t held = menu_inputs_from_psp(pad, pax, pay) | (g_pad_toggle ? MI_TOGGLE : 0);
    const int fx = menu_update(&g_menu, held, g_menu_latched) | g_menu_fx;
    g_menu_latched = 0;
    g_menu_fx = 0;
    menu_apply(fx);
    const uint32_t game = menu_filter_game(&g_menu, pad | g_touch_bits);
    if (g_menu.open) { ax = ay = rx = ry = 128; }
    if (!g_menu.rs_on) rx = ry = 128;                    /* CONTROLS > RIGHT STICK off: no camera */
    psp_ctrl_set(game, ax, ay);
    camera_set_stick(rx, ry);
    present();
}

/* ---- start-up ------------------------------------------------------------------ */

static void *gl_get(const char *name) { return (void *)SDL_GL_GetProcAddress(name); }

#ifdef SDL_PLATFORM_ANDROID
static int android_data_folder(char *out, size_t cap);
#endif

static void find_base(void) {
    const char *env = getenv("PSP2I_DATA");
#ifdef SDL_PLATFORM_ANDROID
    char folder[1024];
    const char *p = env && *env ? env : android_data_folder(folder, sizeof folder) ? folder : NULL;
    if (!p) { SDL_Quit(); exit(0); }            /* the player quit choosing the data folder: the app closes */
#else
    const char *p = env && *env ? env : SDL_GetBasePath();
#endif
    snprintf(g_base, sizeof g_base, "%s", p ? p : ".");
    const size_t n = strlen(g_base);
    if (n > 1 && (g_base[n - 1] == '/' || g_base[n - 1] == '\\')) g_base[n - 1] = '\0';
}

static char g_iso_path[1024];            /* psp2i.ini iso= (desktop; Android asks) */
/* ad hoc multiplayer (psp2i.ini) */
static int  g_adhoc_mode = PSP_ADHOC_MODE_MODERN;
static char g_modern_server[256] = "host";
static char g_adhoc_server[256] = "socom.cc";
static char g_nick[64] = "PSP2i";

static float g_rs_speed = 1.0f;          /* the menu's CONTROLS > SENSITIVITY */
static int  g_rs_on = 1;                 /* CONTROLS > RIGHT STICK */
static int  g_vol[VOL_N] = { VOL_STEPS, VOL_STEPS, VOL_STEPS };
static int  g_touch_color = TOUCH_DEFAULT, g_touch_alpha = TOUCH_ALPHA_DEFAULT;

/* "0".."100" (percent) as tenths. */
static int pct_step(const char *s) {
    const int v = (atoi(s) + 5) / 10;
    return v < 0 ? 0 : v > VOL_STEPS ? VOL_STEPS : v;
}

static int read_ini(int *fps) {
    char path[1100], line[1100];
    snprintf(path, sizeof path, "%s/psp2i.ini", g_base);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "fps=", 4)) *fps = atoi(line + 4) >= 60 ? 60 : 30;
        else if (!strncmp(line, "touch=", 6)) g_touch_on = strncmp(line + 6, "off", 3) != 0;
        else if (!strncmp(line, "fps_counter=", 12)) g_fps_on = strncmp(line + 12, "off", 3) != 0;
        else if (!strncmp(line, "swap_circle_cross=", 18)) g_swap_oc = !strncmp(line + 18, "on", 2);
        else if (!strncmp(line, "iso=", 4)) snprintf(g_iso_path, sizeof g_iso_path, "%s", line + 4);
        else if (!strncmp(line, "adhoc_mode=", 11))
            g_adhoc_mode = !strcmp(line + 11, "ppsspp_direct") ? PSP_ADHOC_MODE_PPSSPP_DIRECT
                         : !strcmp(line + 11, "ppsspp_relay") ? PSP_ADHOC_MODE_PPSSPP_RELAY : PSP_ADHOC_MODE_MODERN;
        else if (!strncmp(line, "modern_server=", 14)) snprintf(g_modern_server, sizeof g_modern_server, "%s", line[14] ? line + 14 : "host");
        else if (!strncmp(line, "adhoc_server=", 13)) { if (line[13]) snprintf(g_adhoc_server, sizeof g_adhoc_server, "%s", line + 13); }
        else if (!strncmp(line, "nickname=", 9)) { if (line[9]) snprintf(g_nick, sizeof g_nick, "%s", line + 9); }
        else if (!strncmp(line, "rs_speed=", 9)) g_rs_speed = (float)atof(line + 9);
        else if (!strncmp(line, "right_stick=", 12)) g_rs_on = strncmp(line + 12, "off", 3) != 0;
        else if (!strncmp(line, "volume_master=", 14)) g_vol[VOL_MASTER] = pct_step(line + 14);
        else if (!strncmp(line, "volume_music=", 13)) g_vol[VOL_MUSIC] = pct_step(line + 13);
        else if (!strncmp(line, "volume_sfx=", 11)) g_vol[VOL_SFX] = pct_step(line + 11);
        else if (!strncmp(line, "touch_color=", 12)) { const int c = menu_touch_color_parse(line + 12); if (c >= 0) g_touch_color = c; }
        else if (!strncmp(line, "touch_opacity=", 14)) {
            const int a = pct_step(line + 14);
            g_touch_alpha = a < TOUCH_ALPHA_MIN ? TOUCH_ALPHA_MIN : a;
        }
    }
    fclose(f);
    return 1;
}

static int path_exists(const char *p) { return SDL_GetPathInfo(p, NULL); }

/* ad hoc / server messages, into psp2i_log.txt */
static void net_line(const char *line) { fprintf(stderr, "net: %s\n", line); }

#ifdef SDL_PLATFORM_ANDROID
#include <jni.h>

/* PSP2iActivity's static helpers (they show Android's pickers and wait). */
static jclass activity_class(JNIEnv **env) {
    *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!*env || !act) return NULL;
    jclass c = (**env)->GetObjectClass(*env, act);
    (**env)->DeleteLocalRef(*env, act);
    return c;
}

/* A file descriptor for the game's ISO, asking for it when `choose` is set
 * or none is remembered; -1 if the user declined. */
static int android_open_iso(int choose) {
    JNIEnv *env;
    jclass c = activity_class(&env);
    if (!c) return -1;
    jmethodID m = (*env)->GetStaticMethodID(env, c, "openGameImage", "(Z)I");
    const int fd = m ? (*env)->CallStaticIntMethod(env, c, m, (jboolean)(choose != 0)) : -1;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, c);
    return fd;
}

/* The modern multiplayer server, asked in a dialog (no settings menu here). */
static void android_ask_modern_server(char *out, size_t cap) {
    JNIEnv *env;
    jclass c = activity_class(&env);
    if (!c) return;
    /* this device's addresses, for the players who join it */
    char v4[64] = "", v6[128] = "", here[256];
    psp_adhoc_host_addresses(v4, sizeof v4, v6, sizeof v6);
    snprintf(here, sizeof here, "%s%s%s", v4, v4[0] && v6[0] ? "  or  " : "", v6);
    jstring jhere = (*env)->NewStringUTF(env, here);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "askModernServer", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring s = m ? (jstring)(*env)->CallStaticObjectMethod(env, c, m, jhere) : NULL;
    if (jhere) (*env)->DeleteLocalRef(env, jhere);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (s) {
        const char *t = (*env)->GetStringUTFChars(env, s, NULL);
        if (t && t[0]) snprintf(out, cap, "%s", t);
        if (t) (*env)->ReleaseStringUTFChars(env, s, t);
        (*env)->DeleteLocalRef(env, s);
    }
    (*env)->DeleteLocalRef(env, c);
}

/* PSP2iActivity.askText: a one-line text dialog (the menu's server items).
 * 1 with the text in out if OK was pressed, 0 if cancelled. */
static int android_ask_text(const char *title, const char *message, const char *value, char *out, size_t cap) {
    JNIEnv *env;
    jclass c = activity_class(&env);
    if (!c) return 0;
    jstring jt = (*env)->NewStringUTF(env, title), jm = (*env)->NewStringUTF(env, message), jv = (*env)->NewStringUTF(env, value);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "askText",
                                            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jstring s = m ? (jstring)(*env)->CallStaticObjectMethod(env, c, m, jt, jm, jv) : NULL;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); s = NULL; }
    if (jt) (*env)->DeleteLocalRef(env, jt);
    if (jm) (*env)->DeleteLocalRef(env, jm);
    if (jv) (*env)->DeleteLocalRef(env, jv);
    int ok = 0;
    if (s) {
        const char *t = (*env)->GetStringUTFChars(env, s, NULL);
        if (t) { snprintf(out, cap, "%s", t); ok = 1; (*env)->ReleaseStringUTFChars(env, s, t); }
        (*env)->DeleteLocalRef(env, s);
    }
    (*env)->DeleteLocalRef(env, c);
    return ok;
}

/* Copy the PSP fonts (flash/) from a folder the user picks into the app. */
static int android_copy_fonts(void) {
    JNIEnv *env;
    jclass c = activity_class(&env);
    if (!c) return 0;
    jmethodID m = (*env)->GetStaticMethodID(env, c, "copyFonts", "()Z");
    const int ok = m ? (*env)->CallStaticBooleanMethod(env, c, m) : 0;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, c);
    return ok;
}

/* PSP2iActivity.dataFolder(): the folder the player chose (asked for, with
 * storage access, the first time). 0 if the player quit instead. */
static int android_data_folder(char *out, size_t cap) {
    JNIEnv *env;
    jclass c = activity_class(&env);
    if (!c) return 0;
    jmethodID m = (*env)->GetStaticMethodID(env, c, "dataFolder", "()Ljava/lang/String;");
    jstring s = m ? (jstring)(*env)->CallStaticObjectMethod(env, c, m) : NULL;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); s = NULL; }
    int ok = 0;
    if (s) {
        const char *u = (*env)->GetStringUTFChars(env, s, NULL);
        if (u) {
            snprintf(out, cap, "%s", u);
            ok = out[0] != '\0';
            (*env)->ReleaseStringUTFChars(env, s, u);
        }
        (*env)->DeleteLocalRef(env, s);
    }
    (*env)->DeleteLocalRef(env, c);
    return ok;
}
#endif

/* ---- the settings menu's choices ----------------------------------------------- */

/* The ad hoc settings to the runtime (at start, and when the menu changes them). */
static void adhoc_apply(void) {
    char host[256];
    snprintf(host, sizeof host, "%s", g_adhoc_server);
    uint16_t port = 0;
    char *c = strrchr(host, ':');
    if (c) { port = (uint16_t)atoi(c + 1); *c = '\0'; }
    psp_adhoc_config ac;
    memset(&ac, 0, sizeof ac);
    ac.server = host;
    ac.server_port = port;
    ac.mode = g_adhoc_mode;
    ac.port_offset = 10000;
    ac.nickname = g_nick;
    ac.modern_server = g_modern_server;
    psp_adhoc_configure(&ac);
    fprintf(stderr, "adhoc: mode %d, modern server %s, ad hoc server %s, nickname %s\n", g_adhoc_mode, g_modern_server,
            g_adhoc_server, g_nick);
}

static void audio_apply(void) {
    g_master = (float)g_menu.vol[VOL_MASTER] / VOL_STEPS;
    for (int i = 0; i < AUDIO_CHANNELS; i++) if (g_stream[i]) SDL_SetAudioStreamGain(g_stream[i], g_master);
    atrac_at3_set_gain((float)g_menu.vol[VOL_MUSIC] / VOL_STEPS);
    psp_sas_set_gain((float)g_menu.vol[VOL_SFX] / VOL_STEPS);
}

/* psp2i.ini with the menu's keys rewritten; every other line (touch=, iso=,
 * nickname=, comments) kept as it was. */
static void write_ini(void) {
    static const char *const OURS[] = { "fps=", "fps_counter=", "right_stick=", "rs_speed=", "volume_master=", "volume_music=", "volume_sfx=",
                                        "swap_circle_cross=", "touch_color=", "touch_opacity=", "adhoc_mode=",
                                        "adhoc_server=", "modern_server=" };
    char path[1100], tmp[1110], line[1100];
    snprintf(path, sizeof path, "%s/psp2i.ini", g_base);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *out = fopen(tmp, "w");
    if (!out) { fprintf(stderr, "menu: cannot write %s\n", tmp); return; }
    FILE *in = fopen(path, "r");
    if (in) {
        while (fgets(line, sizeof line, in)) {
            int ours = 0;
            for (size_t k = 0; k < sizeof OURS / sizeof OURS[0] && !ours; k++) ours = !strncmp(line, OURS[k], strlen(OURS[k]));
            if (ours || !line[0]) continue;
            fputs(line, out);
            if (line[strlen(line) - 1] != '\n') fputc('\n', out);
        }
        fclose(in);
    }
    static const char *const MODES[3] = { "ppsspp_direct", "ppsspp_relay", "modern" };
    fprintf(out, "fps=%d\nfps_counter=%s\nright_stick=%s\nrs_speed=%.2f\n", g_menu.fps, g_menu.show_fps ? "on" : "off",
            g_menu.rs_on ? "on" : "off", (double)menu_rs_speed(&g_menu));
    fprintf(out, "volume_master=%d\nvolume_music=%d\nvolume_sfx=%d\n", g_menu.vol[VOL_MASTER] * 10, g_menu.vol[VOL_MUSIC] * 10,
            g_menu.vol[VOL_SFX] * 10);
    fprintf(out, "swap_circle_cross=%s\ntouch_color=%s\ntouch_opacity=%d\n", g_menu.swap_oc ? "on" : "off",
            menu_touch_color_name(g_menu.touch_color), g_menu.touch_alpha * 10);
    fprintf(out, "adhoc_mode=%s\nadhoc_server=%s\nmodern_server=%s\n", MODES[g_adhoc_mode % 3], g_adhoc_server, g_modern_server);
    fclose(out);
    remove(path);
    if (rename(tmp, path) != 0) fprintf(stderr, "menu: cannot replace %s\n", path);
}

/* What the others type to join while this device hosts modern ad hoc. */
static void menu_share(void) {
    char v4[64] = "", v6[64] = "";
    psp_adhoc_host_addresses(v4, sizeof v4, v6, sizeof v6);
    if (!g_modern_server[0] || !strcmp(g_modern_server, "host"))
        snprintf(g_menu.share, sizeof g_menu.share, "%s%s%s", v6[0] ? v6 : v4, v6[0] && v4[0] ? " / " : "", v6[0] ? v4 : "");
    else g_menu.share[0] = '\0';
}

/* Act on what menu_update / menu_touch changed; changes are saved at once. */
static void menu_apply(int fx) {
    if (!fx) return;
    int save = 0;
    if (fx & MFX_OPENED) menu_share();
    if (fx & MFX_FPS_CHANGED) { framerate_set(g_menu.fps); save = 1; }
    if (fx & MFX_SHOW_FPS) { g_fps_on = g_menu.show_fps; save = 1; }
    if (fx & MFX_RS_SPEED) { camera_set_speed(menu_rs_speed(&g_menu)); save = 1; }
    if (fx & MFX_RS_ON) save = 1;                       /* on_vblank reads g_menu.rs_on */
    if (fx & MFX_AUDIO) { audio_apply(); save = 1; }
    if (fx & MFX_SWAP_OC) { g_swap_oc = g_menu.swap_oc; save = 1; }
    if (fx & MFX_TOUCH) save = 1;
    if (fx & MFX_ADHOC_MODE) { g_adhoc_mode = g_menu.adhoc_mode; adhoc_apply(); save = 1; }
#ifdef SDL_PLATFORM_ANDROID
    char text[256];
    if ((fx & MFX_EDIT_ADHOC) && android_ask_text("Ad hoc server", "The PPSSPP ad hoc server: host or host:port (default port 27312).",
                                                  g_adhoc_server, text, sizeof text)) {
        if (text[0]) snprintf(g_adhoc_server, sizeof g_adhoc_server, "%s", text);
        snprintf(g_menu.adhoc, sizeof g_menu.adhoc, "%.63s", g_adhoc_server);
        adhoc_apply();
        save = 1;
    }
    if ((fx & MFX_EDIT_MODERN) && android_ask_text("Modern server", "host to host on this device, or the address of the player "
                                                   "hosting (IPv4 or IPv6).", g_modern_server, text, sizeof text)) {
        snprintf(g_modern_server, sizeof g_modern_server, "%s", text[0] ? text : "host");
        snprintf(g_menu.modern, sizeof g_menu.modern, "%.63s", g_modern_server);
        menu_share();
        adhoc_apply();
        save = 1;
    }
#endif
    if (save) write_ini();
}

/* Where the game comes from. With GameData/disc/ in the files folder: the
 * extracted files, as on Windows. Otherwise a disc image -- on Android the
 * ISO the user picks (remembered), elsewhere psp2i.ini iso=. A decrypted
 * EBOOT.BIN in the files folder overrides the disc's. Returns the EBOOT's
 * bytes (malloc'd) or NULL, having told the user why. */
static uint8_t *find_game(const char *root, size_t *len) {
    char p[1300], msg[1600];
    snprintf(p, sizeof p, "%s/disc/PSP_GAME", root);
    const int extracted = path_exists(p);
    if (!extracted) {
#ifdef SDL_PLATFORM_ANDROID
        for (int attempt = 0; ; attempt++) {
            const int fd = android_open_iso(attempt > 0);
            if (fd < 0) { fail("No game ISO was chosen."); return NULL; }
            FILE *f = fdopen(fd, "rb");
            if (f && psp_io_set_disc_image(f) == 0) break;
            if (f) fclose(f);
            if (attempt >= 3) { fail("That file is not a PSP disc image (.iso)."); return NULL; }
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "PSP2i", "That file is not a PSP disc image (.iso). Choose the game's ISO.", g_win);
        }
#else
        if (!g_iso_path[0] || psp_io_open_disc_image(g_iso_path) != 0) {
            snprintf(msg, sizeof msg, "No game found.\n\nPut iso=<path to the game's .iso> in %s/psp2i.ini, "
                                      "or the extracted files in %s/disc/.", g_base, root);
            fail(msg);
            return NULL;
        }
#endif
    }
    snprintf(p, sizeof p, "%s/EBOOT.BIN", g_base);
    size_t n = 0;
    uint8_t *b = read_file(p, &n);
    if (b) fprintf(stderr, "eboot: %s\n", p);
    if (!b && extracted) {
        snprintf(p, sizeof p, "%s/disc/PSP_GAME/SYSDIR/EBOOT.BIN", root);
        b = read_file(p, &n);
    }
    if (!b && !extracted) {
        uint32_t l = 0;
        b = psp_io_disc_file("PSP_GAME/SYSDIR/EBOOT.BIN", &l);
        n = l;
    }
    if (!b) { fail("The disc has no PSP_GAME/SYSDIR/EBOOT.BIN."); return NULL; }
    if (n < 4 || memcmp(b, "\x7F" "ELF", 4) != 0) {
        snprintf(msg, sizeof msg, "The disc's EBOOT.BIN is encrypted. Put a decrypted EBOOT.BIN in:\n%s", g_base);
        fail(msg);
        free(b);
        return NULL;
    }

    /* The PSP's fonts: GameData/flash/ (on Android, copied in once). */
    snprintf(p, sizeof p, "%s/flash/font", root);
#ifdef SDL_PLATFORM_ANDROID
    if (!path_exists(p) && !android_copy_fonts())
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "PSP2i",
            "The PSP fonts were not found (GameData/flash/font). The game's text may not show.", g_win);
#else
    if (!path_exists(p)) fprintf(stderr, "psp2i: no fonts in %s; the game's text may not show\n", p);
#endif
    *len = n;
    return b;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");       /* Back must not quit the game mid-save */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "psp2i: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    find_base();
    {
        char log[1100];
        snprintf(log, sizeof log, "%s/logs", g_base);
        mkdir(log, 0777);                                       /* logs/, as on Windows */
        snprintf(log, sizeof log, "%s/logs/psp2i_log.txt", g_base);
        if (freopen(log, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
        if (freopen(log, "a", stdout)) setvbuf(stdout, NULL, _IOLBF, 0);
    }
    fprintf(stderr, "psp2i: files in %s\n", g_base);
    int fps = 30;
    read_ini(&fps);

#ifdef PSP2I_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
#ifdef SDL_PLATFORM_ANDROID
    const SDL_WindowFlags wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    const SDL_WindowFlags wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    g_win = SDL_CreateWindow("Phantasy Star Portable 2 Infinity Recompiled", SCREEN_W * 2, SCREEN_H * 2, wflags);
    g_gl = g_win ? SDL_GL_CreateContext(g_win) : NULL;
    if (!g_gl) { fprintf(stderr, "psp2i: no OpenGL context: %s\n", SDL_GetError()); fail("This device has no OpenGL ES 3.0."); return 1; }
    SDL_GL_MakeCurrent(g_win, g_gl);
    SDL_GL_SetSwapInterval(0);                       /* the scheduler paces frames, not the swap */
    if (gl_load(gl_get) != 0 || gl_ge_init() != 0) { fail("OpenGL ES 3.0 setup failed; see logs/psp2i_log.txt."); return 1; }

    char root[1100];
    snprintf(root, sizeof root, "%s/GameData", g_base);    /* fonts (flash/), saves (ms/), extracted disc */
    if (psp_mem_init() != 0) { fail("Out of memory."); return 1; }
    psp_cpu_reset();
    size_t elf_len = 0;
    uint8_t *elf = find_game(root, &elf_len);
    if (!elf) return 1;
#ifdef PSP2I_NO_GAME_CODE
    /* The setup above still runs, so the game files can be checked with this build. */
    fail("The game files are set up and usable.\n\nThis build has no game code, though: generate port/gen "
         "from your own EBOOT.BIN and build the app yourself (see port/android/README.md).");
    return 1;
#endif
    module_info mi;
    if (load_elf_buf(elf, elf_len, &mi) != 0) { fail("The EBOOT could not be loaded (see logs/psp2i_log.txt)."); return 1; }
    fprintf(stderr, "module '%s': entry 0x%08X, gp 0x%08X\n", mi.name, mi.entry, mi.gp);

    psp_hle_init();
    psp_io_set_root(root);
    psp_net_set_log(net_line);
#ifdef SDL_PLATFORM_ANDROID
    android_ask_modern_server(g_modern_server, sizeof g_modern_server);   /* ad hoc multiplayer: asked at every start */
#endif
    adhoc_apply();
    psp_sysmem_set_heap(mi.load_hi, USER_PARTITION_TOP);
    psp_recomp_register();
    psp_audio_set_sink(audio_sink);
    atrac_at3_init(SDL_GetBasePath());
#ifdef PSP2I_LIBAV
    if (h264_libav_init() != 0) fprintf(stderr, "video: FFmpeg H.264 unavailable; movies will be black\n");
#endif
    framerate_init(fps);
    fps_meter_init(&g_fm);
    /* the settings menu, with psp2i.ini's choices; what this host cannot do is left out */
    menu_init(&g_menu, fps, g_fps_on);
    g_menu.hidden = MENU_BIT(MENU_SEGA_SERVER) | MENU_BIT(MENU_RESOLUTION) | MENU_BIT(MENU_RENDERER)
                  | MENU_BIT(MENU_ATTENUATION) | MENU_BIT(MENU_FONT) | MENU_BIT(MENU_TESTS);
#ifndef SDL_PLATFORM_ANDROID
    g_menu.hidden |= MENU_BIT(MENU_ADHOC_SERVER) | MENU_BIT(MENU_MODERN_SERVER)          /* no text entry here */
                   | MENU_BIT(MENU_TOUCH_COLOR) | MENU_BIT(MENU_TOUCH_ALPHA);
#endif
    menu_set_rs_speed(&g_menu, g_rs_speed);
    g_menu.rs_on = g_rs_on;
    for (int k = 0; k < VOL_N; k++) g_menu.vol[k] = g_vol[k];
    g_menu.swap_oc = g_swap_oc;
    g_menu.touch_color = g_touch_color;
    g_menu.touch_alpha = g_touch_alpha;
    g_menu.adhoc_mode = g_adhoc_mode;
    snprintf(g_menu.adhoc, sizeof g_menu.adhoc, "%.63s", g_adhoc_server);
    snprintf(g_menu.modern, sizeof g_menu.modern, "%.63s", g_modern_server);
    audio_apply();
    camera_init(menu_rs_speed(&g_menu));
    psp_sched_set_vblank_hook(on_vblank);

    static const char boot_path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    const uint32_t argp = psp_sysmem_alloc(0x100, 1);
    psp_mem_write_block(argp, boot_path, (uint32_t)sizeof boot_path);
    fprintf(stderr, "starting module_start at 0x%08X (%d fps)\n", mi.entry, fps);
    const int rc = psp_sched_run(mi.entry, (uint32_t)sizeof boot_path, argp, 0x20, 0x40000, mi.gp);
    fprintf(stderr, "psp2i: the game ended (%d)\n", rc);

    for (int i = 0; i < AUDIO_CHANNELS; i++) if (g_stream[i]) SDL_DestroyAudioStream(g_stream[i]);
    for (int i = 0; i < MAX_PADS; i++) if (g_pad[i]) SDL_CloseGamepad(g_pad[i]);
    SDL_GL_DestroyContext(g_gl);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    psp_mem_free();
    return rc;
}
