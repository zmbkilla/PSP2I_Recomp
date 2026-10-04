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
enum { MENU_FRAME_RATE, MENU_FPS_COUNTER, MENU_CLOSE, MENU_ITEMS };

/* What menu_update() changed, for the caller to act on. */
enum {
    MFX_OPENED      = 1 << 0,
    MFX_CLOSED      = 1 << 1,
    MFX_FPS_CHANGED = 1 << 2,   /* fps is the new frame-rate choice */
    MFX_SHOW_FPS    = 1 << 3,   /* show_fps changed */
};

typedef struct {
    int      open;
    int      sel;          /* MENU_* */
    int      fps;          /* 30 or 60 */
    int      show_fps;     /* the FPS counter */
    uint32_t prev;         /* MI_* held last update, for press detection */
    int      hold;         /* updates Up/Down has been held, for auto-repeat */
    uint32_t game_mask;    /* PSP buttons held through a close, kept from the game until released */
} menu_state;

void menu_init(menu_state *m, int fps, int show_fps);

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
