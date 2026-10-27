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

#include "postfx.h"

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

/* Menu items, top to bottom: the main page, then the CONTROLS, TESTS and
 * GRAPHICS pages (opened from their items; BACK, or Cross, returns to the
 * main page), then KEYBOARD (opened from CONTROLS, and back to it). */
enum { MENU_GRAPHICS,
       MENU_MASTER_VOL, MENU_MUSIC_VOL, MENU_SFX_VOL, MENU_ATTENUATION, MENU_FONT, MENU_CONTROLS, MENU_TESTS, MENU_EXTRACT_MODELS, MENU_LOAD_MODELS, MENU_QUIT, MENU_CLOSE,
       MENU_RS_ON, MENU_RS_SPEED, MENU_DEADZONE, MENU_MOUSE_CAM, MENU_KEYS, MENU_SWAP_OC, MENU_TOUCH_COLOR, MENU_TOUCH_ALPHA, MENU_CONTROLS_BACK,
       MENU_TEST_TICKS, MENU_TEST_LOGIC, MENU_TESTS_BACK,
       MENU_FRAME_RATE, MENU_FPS_COUNTER, MENU_RESOLUTION, MENU_RENDERER, MENU_DRAW_DIST, MENU_VERTEX_WORK,
       MENU_SCALING, MENU_FXAA, MENU_SHARPEN, MENU_SCANLINES, MENU_BRIGHTNESS, MENU_SATURATION, MENU_GRAPHICS_BACK,
       MENU_KEY_FIRST, MENU_KEYS_RESET = MENU_KEY_FIRST + 16, MENU_KEYS_BACK,
       MENU_ITEMS };
enum { MENU_PAGE_MAIN, MENU_PAGE_CONTROLS, MENU_PAGE_TESTS, MENU_PAGE_GRAPHICS, MENU_PAGE_KEYS };

/* KEYBOARD/MOUSE: the PSP controls a key or mouse button can be bound to, in
 * the page's order (item MENU_KEY_FIRST + KB_*). The mouse's movement is not
 * bound here: it turns the camera (MOUSE CAMERA), as the right stick. */
enum { KB_UP, KB_DOWN, KB_LEFT, KB_RIGHT, KB_TRIANGLE, KB_CIRCLE, KB_CROSS, KB_SQUARE, KB_L, KB_R,
       KB_START, KB_SELECT, KB_STICK_UP, KB_STICK_DOWN, KB_STICK_LEFT, KB_STICK_RIGHT, KB_N };
int menu_page_of(int item);                      /* MENU_PAGE_* */
#define MENU_BIT(i) (1ull << (i))

/* Touch controls (the SDL host): colour presets and opacity in tenths. */
enum { TOUCH_DEFAULT, TOUCH_WHITE, TOUCH_BLACK, TOUCH_YELLOW, TOUCH_CYAN, TOUCH_PINK, TOUCH_COLORS };
#define TOUCH_ALPHA_MIN     2
#define TOUCH_ALPHA_DEFAULT 3
const char *menu_touch_color_name(int c);        /* "DEFAULT", "WHITE", ... */
int  menu_touch_color_parse(const char *s);      /* name (any case) -> TOUCH_*, -1 if unknown */

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
/* Controller stick dead zone: DEFAULT (the game's own), then 5% .. 30%. */
#define MENU_DZ_STEPS 7

/* What menu_update() changed, for the caller to act on. */
enum {
    MFX_OPENED      = 1 << 0,
    MFX_CLOSED      = 1 << 1,
    MFX_FPS_CHANGED = 1 << 2,   /* fps is the new frame-rate choice */
    MFX_SHOW_FPS    = 1 << 3,   /* show_fps changed */
    MFX_RS_SPEED    = 1 << 4,   /* rs_step changed: menu_rs_speed() */
    MFX_RES_CHANGED = 1 << 6,   /* res changed: resize the window / go fullscreen */
    MFX_RENDERER    = 1 << 9,   /* renderer changed: save it (used from the next start) */
    MFX_AUDIO       = 1 << 10,  /* a volume or the attenuation changed: apply and save */
    MFX_FONT        = 1 << 12,  /* font changed: the caller applies it and sets font_name */
    MFX_SWAP_OC     = 1 << 13,  /* swap_oc changed: Circle and Cross exchanged (keys and controllers) */
    MFX_TOUCH       = 1 << 14,  /* touch_color or touch_alpha changed */
    MFX_RS_ON       = 1 << 15,  /* rs_on changed: the right stick turns the camera, or nothing */
    MFX_TESTS       = 1 << 16,  /* test_ticks or test_logic changed (framerate_set_tests) */
    MFX_DRAW_DIST   = 1 << 17,  /* draw_dist changed (port/switches.txt switch 0) */
    MFX_POSTFX      = 1 << 18,  /* a post-processing value changed: postfx_set(&post) */
    MFX_VERTEX      = 1 << 19,  /* gpu_vertex changed (psp_gpu_set_gpu_transform) */
    MFX_QUIT        = 1 << 20,  /* QUIT GAME: the caller ends the game (as closing the window) */
    MFX_LOAD_MODELS = 1 << 22,  /* load_models changed: models in Data/ replace the game's (modelx.c) */
    MFX_EXTRACT     = 1 << 21,  /* EXTRACT MODELS: the caller writes the loaded models (modelx.c), sets model_note */
    MFX_DEADZONE    = 1 << 23,  /* deadzone changed: menu_deadzone_pct() */
    MFX_MOUSE_CAM   = 1 << 24,  /* mouse_cam changed: the mouse turns the camera, or not */
    MFX_KEY_WAIT    = 1 << 25,  /* a KEYBOARD row chosen: key_wait is set; the caller binds the next key
                                   (menu_key_done), Escape cancels */
    MFX_KEYS_RESET  = 1 << 26,  /* KEYBOARD > DEFAULT KEYS: the caller restores the bindings and key_name */
};

typedef struct {
    int      open;
    int      sel;          /* MENU_* */
    int      page;         /* MENU_PAGE_*: the page shown */
    int      fps;          /* 30, 60 or 120 */
    int      show_fps;     /* the FPS counter */
    int      rs_step;      /* right-stick sensitivity, 0..MENU_RS_STEPS-1 (1.00x + 0.25x each) */
    int      rs_on;        /* the right stick turns the camera (off: it does nothing) */
    int      deadzone;     /* STICK DEADZONE: 0..MENU_DZ_STEPS-1 (0: the game's own, else 5% each) */
    int      mouse_cam;    /* MOUSE CAMERA: the mouse turns the camera (the cursor held in the window) */
    int      key_wait;     /* KEYBOARD: the row waiting for a key (MENU_KEY_FIRST + KB_*), 0 = none */
    char     key_name[KB_N][16]; /* the keys bound, shown; set by the caller ("" = none) */
    int      draw_dist;    /* DRAW DISTANCE: the longer far distances (switches.txt 0) */
    int      gpu_vertex;   /* VERTEX WORK: 3D vertices transformed and lit on the GPU (1) or the CPU */
    postfx   post;         /* GRAPHICS: scaling, FXAA, sharpen, scanlines, brightness, saturation */
    int      test_ticks;   /* TESTS > TICK PACING: the game's 1/60 s ticks paced as at 30 at 60 too (always above 60) */
    int      test_logic;   /* TESTS > 30 HZ LOGIC: frame-counted functions on 30 Hz frames only */
    int      res;          /* RES_*: output resolution */
    int      renderer;     /* REN_D3D11 / REN_GL: the saved choice */
    int      renderer_now; /* REN_*: the renderer running now (set by the caller) */
    int      vol[VOL_N];   /* 0..VOL_STEPS, tenths */
    int      att;          /* ATT_* */
    int      font;         /* the game's text font, 0..font_n-1; the caller's list (system, PSP, fonts/) */
    int      font_n;       /* choices; under 2 the item only shows font_name */
    char     font_name[40];/* the chosen font, shown; set by the caller */
    int      load_models;  /* LOAD MODELS: models in Data/ replace the game's as they load */
    char     model_note[32];/* EXTRACT MODELS: the last result, shown; set by the caller */
    int      swap_oc;      /* Circle and Cross exchanged (the help line follows) */
    int      touch_color;  /* TOUCH_*: the touch controls' colour */
    int      touch_alpha;  /* their opacity in tenths, TOUCH_ALPHA_MIN..10 */
    uint64_t hidden;       /* MENU_BIT(item)s this host leaves out (not drawn, skipped);
                              menu_init hides the touch items */
    uint32_t prev;         /* MI_* held last update, for press detection */
    int      hold;         /* updates Up/Down has been held, for auto-repeat */
    uint32_t game_mask;    /* PSP buttons held through a close, kept from the game until released */
} menu_state;

void menu_init(menu_state *m, int fps, int show_fps);

/* Right-stick sensitivity as a multiplier (1.00..2.00), and setting it from
 * one (rounded to the nearest step, clamped). */
float menu_rs_speed(const menu_state *m);
int   menu_deadzone_pct(const menu_state *m);    /* 0 (DEFAULT) or 5..30 */
void  menu_set_deadzone_pct(menu_state *m, int pct);
void  menu_set_rs_speed(menu_state *m, float speed);

/* KEYBOARD: the waiting row got its key (or was cancelled); the caller has
 * updated key_name. */
void  menu_key_done(menu_state *m);

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

/* Where menu_draw puts the box in an image of w x h (to copy just that part). */
void menu_box(const menu_state *m, int w, int h, int *x, int *y, int *bw, int *bh);

/* A tap at (x, y) in the image coordinates menu_draw used: on an item, it is
 * selected and changed (left of the value's middle steps down, right steps
 * up; the server items open their editor); on CLOSE or outside the box, the
 * menu closes. Returns MFX_* flags. */
int menu_touch(menu_state *m, int w, int h, int x, int y);
void fps_draw(const fps_meter *fm, menu_image *img);

/* Text in the built-in 5x7 font (ASCII, both cases; unknown characters as
 * blanks), one image pixel per font pixel, 6x9 cells. */
void menu_text(menu_image *img, int x, int y, const char *s, uint32_t rgb);
int  menu_text_width(const char *s);

#endif /* PSP2I_MENU_H */
