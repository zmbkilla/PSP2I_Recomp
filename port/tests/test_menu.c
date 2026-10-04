/* Tests for the settings menu, the FPS counter (host/menu.c) and run-time
 * frame-rate switching (host/framerate.c, against the psprecomp hook and
 * dispatch runtime with stand-ins for the game's functions).
 *
 *   cmake --build port/build --config Release --target test_menu
 *   port/build/Release/test_menu.exe        (or: ctest -C Release) */

#include "menu.h"
#include "framerate.h"

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/mem.h>

#include <stdio.h>
#include <string.h>

static int g_fail, g_run;
#define CHECK(c) do { g_run++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- menu ------------------------------------------------------------------ */

static void test_open_close(void) {
    menu_state m;
    menu_init(&m, 30, 1);
    CHECK(!m.open && m.fps == 30 && m.show_fps == 1);

    CHECK(menu_update(&m, MI_DOWN, 0) == 0);           /* closed: navigation does nothing */
    CHECK(menu_update(&m, 0, 0) == 0);
    CHECK(menu_update(&m, MI_TOGGLE, 0) == MFX_OPENED);
    CHECK(m.open && m.sel == MENU_FRAME_RATE);
    CHECK(menu_update(&m, MI_TOGGLE, 0) == 0);         /* still held: no second toggle */
    CHECK(m.open);
    CHECK(menu_update(&m, 0, 0) == 0);
    CHECK(menu_update(&m, MI_TOGGLE, 0) == MFX_CLOSED); /* the same hotkey closes */
    CHECK(!m.open);

    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, 0, MI_TOGGLE) == MFX_OPENED); /* a tap between samples counts */
    CHECK(menu_update(&m, MI_BACK, 0) == MFX_CLOSED);   /* cancel/back closes */
    menu_update(&m, 0, 0);

    /* Confirm on Close closes; Left/Right on Close do not. */
    menu_update(&m, MI_TOGGLE, 0); menu_update(&m, 0, 0);
    menu_update(&m, MI_UP, 0); menu_update(&m, 0, 0);    /* wraps to Close */
    CHECK(m.sel == MENU_CLOSE);
    CHECK(menu_update(&m, MI_RIGHT, 0) == 0 && m.open);
    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, MI_CONFIRM, 0) == MFX_CLOSED && !m.open);
}

static void test_values(void) {
    menu_state m;
    menu_init(&m, 30, 1);
    menu_update(&m, MI_TOGGLE, 0); menu_update(&m, 0, 0);

    CHECK(menu_update(&m, MI_RIGHT, 0) == MFX_FPS_CHANGED && m.fps == 60);
    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, MI_CONFIRM, 0) == MFX_FPS_CHANGED && m.fps == 30);
    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, MI_LEFT, 0) == MFX_FPS_CHANGED && m.fps == 60);
    menu_update(&m, 0, 0);

    CHECK(menu_update(&m, MI_DOWN, 0) == 0 && m.sel == MENU_FPS_COUNTER);
    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, MI_RIGHT, 0) == MFX_SHOW_FPS && m.show_fps == 0);
    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, MI_RIGHT, 0) == MFX_SHOW_FPS && m.show_fps == 1);
    menu_update(&m, 0, 0);

    menu_update(&m, MI_DOWN, 0); menu_update(&m, 0, 0);
    CHECK(m.sel == MENU_CLOSE);
    menu_update(&m, MI_DOWN, 0); menu_update(&m, 0, 0);
    CHECK(m.sel == MENU_FRAME_RATE);                    /* wraps */

    /* Values survive closing and reopening; reopening starts at the top. */
    menu_update(&m, MI_DOWN, 0); menu_update(&m, 0, 0);
    menu_update(&m, MI_TOGGLE, 0); menu_update(&m, 0, 0);
    menu_update(&m, MI_TOGGLE, 0); menu_update(&m, 0, 0);
    CHECK(m.open && m.sel == MENU_FRAME_RATE && m.fps == 60 && m.show_fps == 1);

    menu_state d;
    menu_init(&d, 45, 0);                               /* anything but 60 is 30 */
    CHECK(d.fps == 30 && d.show_fps == 0);
}

static void test_repeat(void) {
    menu_state m;
    menu_init(&m, 30, 1);
    menu_update(&m, MI_TOGGLE, 0); menu_update(&m, 0, 0);
    int moves = 0, sel = m.sel;
    for (int i = 0; i < 20 + 6 * 3; i++) {             /* 1 press + 3 repeats */
        menu_update(&m, MI_DOWN, 0);
        if (m.sel != sel) { moves++; sel = m.sel; }
    }
    CHECK(moves == 4);
    menu_update(&m, 0, 0);
    CHECK(m.hold == 0);
}

static void test_filter(void) {
    menu_state m;
    menu_init(&m, 30, 1);
    const uint32_t CROSS = 0x4000, UP = 0x0010, CIRCLE = 0x2000;
    CHECK(menu_filter_game(&m, CROSS | UP) == (CROSS | UP));   /* closed: everything passes */

    menu_update(&m, MI_TOGGLE, 0);
    CHECK(menu_filter_game(&m, CROSS | UP) == 0);              /* open: the game sees nothing */
    CHECK(menu_filter_game(&m, CIRCLE) == 0);

    menu_update(&m, MI_BACK, 0);                               /* closed with Circle held */
    CHECK(!m.open);
    CHECK(menu_filter_game(&m, CIRCLE) == 0);                  /* held through the close: hidden */
    CHECK(menu_filter_game(&m, CIRCLE | UP) == UP);            /* a new press passes */
    CHECK(menu_filter_game(&m, UP) == UP);                     /* Circle released ... */
    CHECK(menu_filter_game(&m, CIRCLE | UP) == (CIRCLE | UP)); /* ... and pressed again: passes */
}

/* ---- FPS meter -------------------------------------------------------------- */

static void test_meter(void) {
    fps_meter fm;
    fps_meter_init(&fm);
    CHECK(fps_meter_value(&fm) < 0);

    /* 30 frames a second, sampled at 60 Hz (vblank = 16683 us). */
    uint64_t t = 1000, flips = 0;
    const uint64_t VB = 16683;
    for (int i = 0; i < 20; i++) { fps_meter_sample(&fm, t, flips); t += VB; if (i & 1) flips++; }
    CHECK(fps_meter_value(&fm) < 0);                    /* under half a second: no value yet */
    for (int i = 20; i < 180; i++) { fps_meter_sample(&fm, t, flips); t += VB; if (i & 1) flips++; }
    double v = fps_meter_value(&fm);
    CHECK(v > 29.0 && v < 31.0);

    /* Switch to 60 frames a second: within the 1 s window plus one refresh
     * the value reaches 60, and in between it is a blend (no jumps past). */
    double seen_mid = 0;
    for (int i = 0; i < 120; i++) {
        flips++;
        fps_meter_sample(&fm, t, flips); t += VB;
        const double x = fps_meter_value(&fm);
        if (x > 31.0 && x < 59.0) seen_mid = x;
        CHECK(x < 61.0);
    }
    v = fps_meter_value(&fm);
    CHECK(v > 59.0 && v < 61.0);
    CHECK(seen_mid > 0);

    /* The value only changes at refreshes (every 0.5 s): readable, not jittery. */
    int changes = 0;
    double last = fps_meter_value(&fm);
    for (int i = 0; i < 120; i++) {                     /* 2 s, alternating 1 and 0 flips */
        flips += (uint64_t)(i & 1);
        fps_meter_sample(&fm, t, flips); t += VB;
        if (fps_meter_value(&fm) != last) { changes++; last = fps_meter_value(&fm); }
    }
    CHECK(changes <= 4);

    /* A stall (no flips for 2 s) reads 0, not the last value. */
    for (int i = 0; i < 120; i++) { fps_meter_sample(&fm, t, flips); t += VB; }
    CHECK(fps_meter_value(&fm) < 0.5);
}

/* ---- drawing ------------------------------------------------------------------ */

static uint32_t g_img[480 * 272];

static int changed_outside(int x0, int y0, int x1, int y1) {
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            if ((x < x0 || x >= x1 || y < y0 || y >= y1) && g_img[y * 480 + x] != 0xFF808080u) return 1;
    return 0;
}
static int changed_inside(int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (g_img[y * 480 + x] != 0xFF808080u) return 1;
    return 0;
}

static void test_draw(void) {
    menu_image img = { g_img, 480, 272 };
    menu_state m;
    fps_meter fm;
    menu_init(&m, 30, 1);
    fps_meter_init(&fm);

    for (int i = 0; i < 480 * 272; i++) g_img[i] = 0xFF808080u;
    menu_draw(&m, &img, 30, "F1");                      /* closed: draws nothing */
    CHECK(!changed_inside(0, 0, 480, 272));

    m.open = 1;
    menu_draw(&m, &img, 30, "F1");
    CHECK(changed_inside(96, 72, 384, 200));            /* the centred 288x128 box */
    CHECK(!changed_outside(96, 72, 384, 200));

    for (int i = 0; i < 480 * 272; i++) g_img[i] = 0xFF808080u;
    fps_draw(&fm, &img);                                /* top-left corner only */
    CHECK(changed_inside(0, 0, 120, 16));
    CHECK(!changed_outside(0, 0, 120, 16));

    CHECK(menu_text_width("ACTUAL 60.0 FPS") == 15 * 6);
    for (int i = 0; i < 480 * 272; i++) g_img[i] = 0xFF808080u;
    menu_text(&img, 470, 268, "WW", 0xFFFFFF);          /* clipped at the edges, no overrun */
    CHECK(!changed_outside(470, 268, 480, 272));
}

/* ---- run-time frame-rate switching -------------------------------------------- */

#define FN_SET  0x08B81890u
#define FN_FRAME 0x08B85AB0u
static uint32_t g_orig_saw;          /* $a0 the original setFrameRate received */
static int      g_set_calls, g_frame_calls;
static uint32_t g_frame_saw_a0, g_frame_saw_sp;

/* Stand-ins for the game's functions, entered as the generated code enters a
 * hooked function. */
static void orig_set(void) {
    g_orig_saw = psp_cpu.r[4];
    g_set_calls++;
    psp_write32(0x08EDA558u, psp_cpu.r[4]);            /* the timing state's fps */
    psp_cpu.r[4] = 0xDEAD;                              /* clobbers, as real code does */
    psp_cpu.r[2] = 0xBEEF;
}
static void entry_set(void) {
    psp_hook_fn h = psp_hook_find(FN_SET);
    if (h) h(orig_set); else orig_set();
}
static void orig_frame(void) { g_frame_calls++; g_frame_saw_a0 = psp_cpu.r[4]; g_frame_saw_sp = psp_cpu.r[29]; }
static void entry_frame(void) {
    psp_hook_fn h = psp_hook_find(FN_FRAME);
    if (h) h(orig_frame); else orig_frame();
}
static void game_asks(uint32_t fps) { psp_cpu.r[4] = fps; entry_set(); }
static void game_frame(void) { psp_cpu.r[4] = 0x1234; psp_cpu.r[29] = 0x09F00000u; psp_cpu.r[2] = 7; entry_frame(); }

static void test_framerate(void) {
    if (psp_mem_init() != 0) { CHECK(!"psp_mem_init"); return; }
    psp_register(FN_SET, entry_set);
    psp_register(FN_FRAME, entry_frame);

    framerate_init(30);
    CHECK(framerate_target() == 30 && framerate_game_fps() == 0);
    game_frame();                                       /* nothing pending: no extra call */
    CHECK(g_set_calls == 0 && g_frame_calls == 1);

    game_asks(30); CHECK(g_orig_saw == 30);             /* at 30, everything passes unchanged */
    game_asks(20); CHECK(g_orig_saw == 20);
    game_asks(30); CHECK(g_orig_saw == 30 && framerate_game_fps() == 30);

    /* To 60 at run time: applied at the next frame, through setFrameRate. */
    const int before = g_set_calls;
    framerate_set(60);
    CHECK(framerate_target() == 60 && g_set_calls == before);   /* not from the host thread */
    game_frame();
    CHECK(g_set_calls == before + 1 && g_orig_saw == 60 && framerate_game_fps() == 60);
    CHECK(g_frame_saw_a0 == 0x1234 && g_frame_saw_sp == 0x09F00000u);  /* registers restored */
    CHECK(psp_cpu.r[2] == 7);
    game_frame();
    CHECK(g_set_calls == before + 1);                   /* once only */

    /* The game's own requests at 60. */
    game_asks(20); CHECK(g_orig_saw == 60);             /* the heavy-scene fallback too */
    game_asks(15); CHECK(g_orig_saw == 15);             /* slow modes as asked */
    game_asks(30); CHECK(g_orig_saw == 60);

    /* Back to 30: re-issues the game's last request unmapped. */
    game_asks(20); CHECK(g_orig_saw == 60);
    framerate_set(30);
    game_frame();
    CHECK(g_orig_saw == 20 && framerate_game_fps() == 20);

    /* Setting the current target again does nothing. */
    const int n = g_set_calls;
    framerate_set(30); game_frame();
    CHECK(g_set_calls == n);

    psp_mem_free();
}

int main(void) {
    test_open_close();
    test_values();
    test_repeat();
    test_filter();
    test_meter();
    test_draw();
    test_framerate();
    printf("test_menu: %d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
