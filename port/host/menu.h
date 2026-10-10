/* The port's settings menu and FPS counter: logic and drawing, with no
 * platform dependencies, so tests/test_menu.c can drive them directly.
 *
 * main.c feeds the menu one input sample per vblank (logical buttons, below),
 * applies what menu_update() reports (a new frame rate goes to
 * framerate_set()), filters the game's input through menu_filter_game() and
 * draws the overlay into the presented image with menu_draw()/fps_draw(). */
#ifndef PSP2I_MENU_H
#define PSP2I_MENU_H

#include <stdint.h>

/* Logical menu inputs (held state; menu_update finds the presses). */
enum {
    MI_TOGGLE  = 1 << 0,   /* the hotkey: opens, and closes */
    MI_UP      = 1 << 1,
    MI_DOWN    = 1 << 2,
    MI_LEFT    = 1 << 3,
    MI_RIGHT   = 1 << 4,
    MI_CONFIRM = 1 << 5,   /* change the selected value / activate Close */
    MI_BACK    = 1 << 6,   /* cancel: closes */
};

/* Menu items, top to bottom. */
enum { MENU_FRAME_RATE, MENU_FPS_COUNTER, MENU_RS_SPEED, MENU_SEGA_SERVER, MENU_ADHOC_SERVER, MENU_ADHOC_MODE,
       MENU_MODERN_SERVER, MENU_RESOLUTION, MENU_RENDERER, MENU_MASTER_VOL, MENU_MUSIC_VOL, MENU_SFX_VOL, MENU_ATTENUATION,
       MENU_CLOSE, MENU_ITEMS };

/* Volumes in tenths (0..10 = 0..100%); attenuation while other programs play
 * sound: ATT_* (OFF, -6 dB, -12 dB, -20 dB). */
enum { VOL_MASTER, VOL_MUSIC, VOL_SFX, VOL_N };
#define VOL_STEPS 10
enum { ATT_OFF, ATT_6DB, ATT_12DB, ATT_20DB, ATT_MODES };
#define ATT_DEFAULT ATT_12DB
int  menu_att_db(int mode);                          /* 0, -6, -12, -20 */
int  menu_att_parse(int db);                         /* dB -> ATT_* (nearest; 0 = off) */

/* Output resolution: the window's client size (the 480x272 picture is scaled
 * to fit it, aspect kept), or borderless fullscreen on the current monitor. */
enum { RES_960x544, RES_1280x720, RES_1920x1080, RES_2560x1440, RES_3840x2160, RES_FULLSCREEN, RES_MODES };
#define RES_DEFAULT RES_1920x1080
const char *menu_res_name(int mode);                 /* "1920X1080", "FULLSCREEN" */
int  menu_res_size(int mode, int *w, int *h);        /* client size; 0 for fullscreen */
int  menu_res_parse(const char *s);                  /* "1920x1080" / "fullscreen" -> mode, -1 if unknown */

/* Renderer for the GE (used from the next start): Direct3D 11, OpenGL 3.3, or
 * the software reference (command line only, not in the menu). */
enum { REN_D3D11, REN_GL, REN_SOFTWARE, REN_MENU_CHOICES = 2 };
const char *menu_ren_key(int r);                     /* "d3d11", "opengl", "software" */
int  menu_ren_parse(const char *s);                  /* "d3d11" / "opengl" / "gl" -> REN_*, -1 if unknown */

/* Right-stick camera sensitivity: 1.00x .. 2.00x in 0.25 steps. */
#define MENU_RS_STEPS 5

/* What menu_update() changed, for the caller to act on. */
enum {
    MFX_OPENED      = 1 << 0,
    MFX_CLOSED      = 1 << 1,
    MFX_FPS_CHANGED = 1 << 2,   /* fps is the new frame-rate choice */
    MFX_SHOW_FPS    = 1 << 3,   /* show_fps changed */
    MFX_RS_SPEED    = 1 << 4,   /* rs_step changed: menu_rs_speed() */
    MFX_EDIT_SEGA   = 1 << 5,   /* Circle on SEGA SERVER: the caller opens its text editor */
    MFX_RES_CHANGED = 1 << 6,   /* res changed: resize the window / go fullscreen */
    MFX_EDIT_ADHOC  = 1 << 7,   /* Circle on ADHOC SERVER: the caller opens its text editor */
    MFX_ADHOC_MODE  = 1 << 8,   /* adhoc_mode changed */
    MFX_RENDERER    = 1 << 9,   /* renderer changed: save it (used from the next start) */
    MFX_AUDIO       = 1 << 10,  /* a volume or the attenuation changed: apply and save */
    MFX_EDIT_MODERN = 1 << 11,  /* Circle on MODERN SERVER: the caller opens its text editor */
};

typedef struct {
    int      open;
    int      sel;          /* MENU_* */
    int      fps;          /* 30 or 60 */
    int      show_fps;     /* the FPS counter */
    int      rs_step;      /* right-stick sensitivity, 0..MENU_RS_STEPS-1 (1.00x + 0.25x each) */
    int      res;          /* RES_*: output resolution */
    char     sega[64];     /* the SEGA server redirect, shown ("" = off); set by the caller */
    char     adhoc[64];    /* the ad hoc server host[:port], shown; set by the caller */
    int      adhoc_mode;   /* PSP_ADHOC_MODE_*: 0 PPSSPP direct, 1 PPSSPP relay, 2 modern; set by the caller */
    char     modern[64];   /* the modern server: "host" or an address; set by the caller */
    char     share[96];    /* while hosting modern: the addresses others type; set by the caller */
    int      renderer;     /* REN_D3D11 / REN_GL: the saved choice */
    int      renderer_now; /* REN_*: the renderer running now (set by the caller) */
    int      vol[VOL_N];   /* 0..VOL_STEPS, tenths */
    int      att;          /* ATT_* */
    uint32_t prev;         /* MI_* held last update, for press detection */
    int      hold;         /* updates Up/Down has been held, for auto-repeat */
    uint32_t game_mask;    /* PSP buttons held through a close, kept from the game until released */
} menu_state;

void menu_init(menu_state *m, int fps, int show_fps);

/* Right-stick sensitivity as a multiplier (1.00..2.00), and setting it from
 * one (rounded to the nearest step, clamped). */
float menu_rs_speed(const menu_state *m);
void  menu_set_rs_speed(menu_state *m, float speed);

/* PSP buttons (and the analog stick, 0..255) as menu inputs, following the
 * game's own conventions: Circle accepts, Cross goes back; the D-pad, or the
 * stick past half way, moves; Start also accepts. With the default keyboard
 * mapping that is X = accept, Z = back. */
uint32_t menu_inputs_from_psp(uint32_t psp, uint8_t ax, uint8_t ay);

/* One input sample. held: MI_* currently down. latched: MI_* pressed and
 * released since the last call (a tap shorter than one vblank), counted as
 * presses too. Returns MFX_* flags. */
int menu_update(menu_state *m, uint32_t held, uint32_t latched);

/* The PSP buttons the game should see. While the menu is open: none. After it
 * closes, buttons that were still held (the Cross or Circle that closed it)
 * stay hidden until released, so closing the menu does not also press them
 * in the game. */
uint32_t menu_filter_game(menu_state *m, uint32_t psp_buttons);

/* ---- measured frame rate --------------------------------------------------
 * Samples of (host time, the game's flip count), one per vblank. The value is
 * flips / elapsed host time over the most recent window of up to
 * FPS_METER_WINDOW_US, recomputed every FPS_METER_REFRESH_US so the number is
 * readable. It measures frames the game actually produced, against the host
 * clock -- not the target, and not the emulated vblank clock. */
#define FPS_METER_SAMPLES    256
#define FPS_METER_WINDOW_US  1000000u
#define FPS_METER_REFRESH_US  500000u

typedef struct {
    uint64_t t[FPS_METER_SAMPLES], f[FPS_METER_SAMPLES];
    int      head, n;
    uint64_t refreshed;    /* host time of the last recompute */
    double   value;        /* the displayed value; < 0 until the first window */
} fps_meter;

void   fps_meter_init(fps_meter *fm);
void   fps_meter_sample(fps_meter *fm, uint64_t now_us, uint64_t flips);
double fps_meter_value(const fps_meter *fm);

/* ---- drawing into a 0xAARRGGBB image -------------------------------------- */
typedef struct { uint32_t *px; int w, h; } menu_image;

/* game_fps: the frame rate the game's own timing state holds (what the
 * setting reached), 0 if unknown. hotkey: the keyboard hotkey's name. */
void menu_draw(const menu_state *m, menu_image *img, int game_fps, const char *hotkey);
void fps_draw(const fps_meter *fm, menu_image *img);

/* Text in the built-in 5x7 font (ASCII; lower case drawn as upper case;
 * unknown characters as blanks), one image pixel per font pixel, 6x9 cells. */
void menu_text(menu_image *img, int x, int y, const char *s, uint32_t rgb);
int  menu_text_width(const char *s);

#endif /* PSP2I_MENU_H */
