/* Tests for the settings menu, the FPS counter (host/menu.c) and run-time
 * frame-rate switching (host/framerate.c, against the psprecomp hook and
 * dispatch runtime with stand-ins for the game's functions).
 *
 *   cmake --build port/build --config Release --target test_menu
 *   port/build/Release/test_menu.exe        (or: ctest -C Release) */

#include "menu.h"
#include "framerate.h"
#include "camera.h"
#include "login.h"
#include "textedit.h"

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
    CHECK(m.sel == MENU_RS_SPEED);
    menu_update(&m, MI_DOWN, 0); menu_update(&m, 0, 0);
    CHECK(m.sel == MENU_SEGA_SERVER);
    CHECK(menu_update(&m, MI_CONFIRM, 0) == MFX_EDIT_SEGA && m.open);   /* Circle opens the editor */
    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, MI_RIGHT, 0) == 0);                           /* Left/Right do nothing there */
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

/* The game's conventions: Circle accepts, Cross backs out. */
static void test_buttons(void) {
    const uint32_t CIRCLE = 0x2000, CROSS = 0x4000, START = 0x0008, UP = 0x0010, DOWN = 0x0040,
                   LEFT = 0x0080, RIGHT = 0x0020, TRIANGLE = 0x1000, SQUARE = 0x8000;
    CHECK(menu_inputs_from_psp(CIRCLE, 128, 128) == MI_CONFIRM);
    CHECK(menu_inputs_from_psp(CROSS, 128, 128) == MI_BACK);
    CHECK(menu_inputs_from_psp(START, 128, 128) == MI_CONFIRM);
    CHECK(menu_inputs_from_psp(TRIANGLE | SQUARE, 128, 128) == 0);
    CHECK(menu_inputs_from_psp(UP | LEFT, 128, 128) == (MI_UP | MI_LEFT));
    CHECK(menu_inputs_from_psp(DOWN | RIGHT, 128, 128) == (MI_DOWN | MI_RIGHT));
    CHECK(menu_inputs_from_psp(0, 128, 0) == MI_UP);            /* stick past half way */
    CHECK(menu_inputs_from_psp(0, 255, 128) == MI_RIGHT);
    CHECK(menu_inputs_from_psp(0, 150, 100) == 0);              /* small deflection: nothing */

    /* A full interaction with PSP buttons, as main.c feeds it. */
    menu_state m;
    menu_init(&m, 30, 1);
    /* main.c's order each vblank: input changes reach the game filter first
     * (ctrl_commit from poll_controllers), then the menu steps. */
#define STEP(psp) (menu_filter_game(&m, (psp)), menu_update(&m, menu_inputs_from_psp((psp), 128, 128) | toggle, 0))
    uint32_t toggle = MI_TOGGLE;
    CHECK(STEP(0) == MFX_OPENED);
    toggle = 0;
    CHECK(STEP(0) == 0);
    CHECK(STEP(CIRCLE) == MFX_FPS_CHANGED && m.fps == 60);      /* Circle accepts */
    CHECK(STEP(CIRCLE) == 0 && m.fps == 60);                    /* held: no repeat */
    CHECK(STEP(0) == 0);
    CHECK(STEP(DOWN) == 0 && m.sel == MENU_FPS_COUNTER);
    CHECK(STEP(0) == 0);
    CHECK(STEP(CIRCLE) == MFX_SHOW_FPS && m.show_fps == 0);
    CHECK(STEP(0) == 0);
    CHECK(STEP(DOWN) == 0 && m.sel == MENU_RS_SPEED);
    CHECK(STEP(0) == 0);
    CHECK(STEP(DOWN) == 0 && m.sel == MENU_SEGA_SERVER);
    CHECK(STEP(0) == 0);
    CHECK(STEP(DOWN) == 0 && m.sel == MENU_CLOSE);
    CHECK(STEP(0) == 0);
    CHECK(STEP(CROSS) == MFX_CLOSED && !m.open);                /* Cross backs out */
    CHECK(menu_filter_game(&m, CROSS) == 0);                    /* ... without pressing Cross in game */
    CHECK(STEP(CROSS) == 0 && !m.open);                         /* still held: does not reopen or act */
    CHECK(menu_filter_game(&m, 0) == 0);
    CHECK(menu_filter_game(&m, CROSS) == CROSS);                /* a fresh press reaches the game */
    CHECK(STEP(0) == 0);

    toggle = MI_TOGGLE;
    CHECK(STEP(0) == MFX_OPENED);
    toggle = 0;
    STEP(0);
    STEP(UP); STEP(0);                                          /* to Close */
    CHECK(m.sel == MENU_CLOSE);
    CHECK(STEP(CIRCLE) == MFX_CLOSED);                          /* Circle on Close closes */
    CHECK(menu_filter_game(&m, CIRCLE) == 0);
    CHECK(STEP(CIRCLE) == 0 && !m.open);
#undef STEP
}

/* Right-stick sensitivity in the menu: 1.00x..2.00x, Left/Right step and stop
 * at the ends, Circle cycles; Cross still backs out from the item. */
static void test_rs_speed(void) {
    menu_state m;
    menu_init(&m, 30, 1);
    CHECK(menu_rs_speed(&m) == 1.0f);                   /* default 1.00x */
    menu_update(&m, MI_TOGGLE, 0); menu_update(&m, 0, 0);
    menu_update(&m, MI_DOWN, 0); menu_update(&m, 0, 0);
    menu_update(&m, MI_DOWN, 0); menu_update(&m, 0, 0);
    CHECK(m.sel == MENU_RS_SPEED);
    CHECK(menu_update(&m, MI_LEFT, 0) == 0 && menu_rs_speed(&m) == 1.0f);   /* already at the bottom */
    menu_update(&m, 0, 0);
    float want = 1.0f;
    for (int i = 0; i < 4; i++) {
        CHECK(menu_update(&m, MI_RIGHT, 0) == MFX_RS_SPEED);
        menu_update(&m, 0, 0);
        want += 0.25f;
        CHECK(menu_rs_speed(&m) == want);
    }
    CHECK(menu_rs_speed(&m) == 2.0f);
    CHECK(menu_update(&m, MI_RIGHT, 0) == 0 && menu_rs_speed(&m) == 2.0f);  /* stops at 2.00x */
    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, MI_CONFIRM, 0) == MFX_RS_SPEED && menu_rs_speed(&m) == 1.0f);  /* cycles */
    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, menu_inputs_from_psp(0x2000, 128, 128), 0) == MFX_RS_SPEED && menu_rs_speed(&m) == 1.25f);
    menu_update(&m, 0, 0);
    CHECK(menu_update(&m, menu_inputs_from_psp(0x4000, 128, 128), 0) == MFX_CLOSED);    /* Cross backs out */
    CHECK(menu_rs_speed(&m) == 1.25f);                  /* kept */
    menu_set_rs_speed(&m, 1.8f);  CHECK(menu_rs_speed(&m) == 1.75f);
    menu_set_rs_speed(&m, 9.0f);  CHECK(menu_rs_speed(&m) == 2.0f);
    menu_set_rs_speed(&m, 0.2f);  CHECK(menu_rs_speed(&m) == 1.0f);
}

/* The camera axis exactly as the cheat computes it: byte - 128, zero for
 * -8..7, then / 128 * speed. */
static void test_camera_axis(void) {
    CHECK(camera_axis(128, 1.0f) == 0.0f);
    CHECK(camera_axis(120, 1.0f) == 0.0f);              /* -8: inside the dead zone */
    CHECK(camera_axis(135, 1.0f) == 0.0f);              /* +7 */
    CHECK(camera_axis(136, 1.0f) == 8.0f / 128.0f);     /* +8 */
    CHECK(camera_axis(119, 1.0f) == -9.0f / 128.0f);    /* -9 */
    CHECK(camera_axis(255, 1.0f) == 127.0f / 128.0f);
    CHECK(camera_axis(0, 1.0f) == -1.0f);
    CHECK(camera_axis(255, 2.0f) == 2.0f * 127.0f / 128.0f);
    CHECK(camera_axis(0, 1.5f) == -1.5f);
}

/* The sign-in screen: Circle accepts, Cross backs out; the password is never
 * drawn; submitting needs both fields; a failure keeps the screen open. */
static void test_login(void) {
    login_state l;
    login_open(&l, "test.server", "remembered");
    CHECK(l.state == LOGIN_EDITING && !strcmp(l.user, "remembered") && l.sel == LOGIN_FIELD_PASS);
    login_type(&l, 's'); login_type(&l, 'e'); login_type(&l, 'c');
    CHECK(!strcmp(l.pass, "sec"));
    login_backspace(&l);
    CHECK(!strcmp(l.pass, "se"));
    /* the password is not drawn: render and look for its letters' absence is
     * impractical; check the field shows asterisks via the drawn width instead */
    static uint32_t px[480 * 272];
    menu_image img = { px, 480, 272 };
    login_draw(&l, &img);
    login_update(&l, 0, 0);
    login_update(&l, MI_DOWN, 0); login_update(&l, 0, 0);
    CHECK(l.sel == LOGIN_BTN_SIGNIN);
    CHECK(login_update(&l, MI_CONFIRM, 0x2000) == LOGIN_ACT_SUBMIT && l.state == LOGIN_WORKING);   /* Circle */
    login_finish(&l, 0, "sign-in refused: invalid password");
    CHECK(l.state == LOGIN_EDITING && l.error && l.pass[0] == 0);       /* stays open, password cleared */
    login_update(&l, 0, 0);
    CHECK(login_update(&l, MI_BACK, 0x4000) == LOGIN_ACT_CANCEL);       /* Cross */
    login_open(&l, "s", "");
    l.sel = LOGIN_BTN_SIGNIN;
    CHECK(login_update(&l, MI_CONFIRM, 0) == LOGIN_ACT_NONE && l.error);   /* empty fields refused */
    login_open(&l, "s", "");
    login_update(&l, 0, 0);
    CHECK(login_update(&l, MI_CONFIRM, 0) == LOGIN_ACT_NONE && l.osk);  /* Circle on a field: keyboard */
    login_update(&l, 0, 0);
    login_update(&l, MI_DOWN, 0); login_update(&l, 0, 0);              /* to row "abcdefghij" */
    login_update(&l, MI_CONFIRM, 0x2000);                               /* types 'a' */
    login_update(&l, 0, 0);
    CHECK(!strcmp(l.user, "a"));
    CHECK(login_update(&l, MI_BACK, 0) == LOGIN_ACT_NONE && !l.osk);    /* Cross closes only the keyboard */
    login_finish(&l, 1, "ok");
    CHECK(l.state == LOGIN_DONE_OK);
    int act = LOGIN_ACT_NONE, n = 0;
    while (act == LOGIN_ACT_NONE && n++ < 200) act = login_update(&l, 0, 0);
    CHECK(act == LOGIN_ACT_CLOSE_OK);
}

/* The password field shows one '*' per typed character (the font had no
 * '*', so typed passwords looked like no input). Field: x 166.., y 98..110. */
static int pass_field_lit(const login_state *l) {
    static uint32_t px[480 * 272];
    for (int i = 0; i < 480 * 272; i++) px[i] = 0xFF000000u;
    menu_image img = { px, 480, 272 };
    login_draw(l, &img);
    int lit = 0;
    for (int y = 101; y < 108; y++)
        for (int x = 168; x < 400; x++)
            if ((px[y * 480 + x] & 0xFFFFFF) == 0xFFFFFF) lit++;
    return lit;
}

static void test_password_visible(void) {
    login_state l;
    login_open(&l, "s", "user");
    l.sel = LOGIN_FIELD_USER;                       /* no cursor in the password field */
    const int empty = pass_field_lit(&l);
    for (const char *c = "ppsspp123"; *c; c++) { l.sel = LOGIN_FIELD_PASS; login_type(&l, *c); }
    CHECK(!strcmp(l.pass, "ppsspp123"));
    l.sel = LOGIN_FIELD_USER;
    const int typed = pass_field_lit(&l);
    CHECK(empty == 0);
    CHECK(typed > 0);                               /* something is shown for the password */
    /* exactly nine asterisks: the '*' glyph lights 11 pixels */
    CHECK(typed == 9 * 11);
    /* and the cursor is visible in the selected field */
    l.sel = LOGIN_FIELD_PASS;
    CHECK(pass_field_lit(&l) > typed);
}

/* The SEGA SERVER editor: Circle types, Start saves, Cross cancels. */
static void test_textedit(void) {
    textedit e;
    textedit_open(&e, "T", "H", "old");
    CHECK(e.open && !strcmp(e.text, "old"));
    CHECK(textedit_update(&e, MI_CONFIRM, 0x2000) == TE_NONE && !strcmp(e.text, "old"));  /* held from opening: not a press */
    textedit_update(&e, 0, 0);
    textedit_backspace(&e); textedit_backspace(&e); textedit_backspace(&e);
    textedit_type(&e, 'h'); textedit_type(&e, ':'); textedit_type(&e, ' ');
    CHECK(!strcmp(e.text, "h:"));                                       /* no spaces */
    CHECK(textedit_update(&e, MI_CONFIRM, 0x2000) == TE_NONE && !strcmp(e.text, "h:1"));   /* key (0,0) = '1' */
    textedit_update(&e, 0, 0);
    CHECK(textedit_update(&e, 0, 0x8000) == TE_NONE && !strcmp(e.text, "h:"));            /* Square deletes */
    textedit_update(&e, 0, 0);
    CHECK(textedit_update(&e, 0, 0x0008) == TE_SAVE && !e.open && !strcmp(e.text, "h:")); /* Start saves */
    textedit_open(&e, "T", "H", "keep");
    textedit_update(&e, 0, 0);
    CHECK(textedit_update(&e, MI_BACK, 0x4000) == TE_CANCEL && !e.open);                  /* Cross cancels */
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
    CHECK(changed_inside(96, 57, 384, 214));            /* the centred 288x157 box */
    CHECK(!changed_outside(96, 57, 384, 214));

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

/* The camera hooks against the hook runtime, with stand-ins for the game's
 * camera-input function (writes yaw/pitch through a1/a2) and its
 * "camera controlled" test. */
#define CAM_YAW   0x08900000u
#define CAM_PITCH 0x08900004u
static float g_game_yaw, g_game_pitch;
static uint32_t g_game_active;
static void orig_cam_input(void) { psp_write_f32(psp_cpu.r[5], g_game_yaw); psp_write_f32(psp_cpu.r[6], g_game_pitch); }
static void entry_cam_input(void) { psp_hook_fn h = psp_hook_find(0x08A16794u); if (h) h(orig_cam_input); else orig_cam_input(); }
static void orig_cam_active(void) { psp_cpu.r[31] = 0xDEAD; psp_cpu.r[2] = g_game_active; }
static void entry_cam_active(void) { psp_hook_fn h = psp_hook_find(0x08AFA530u); if (h) h(orig_cam_active); else orig_cam_active(); }
static void cam_frame(void) { psp_cpu.r[5] = CAM_YAW; psp_cpu.r[6] = CAM_PITCH; entry_cam_input(); }
static uint32_t cam_active(uint32_t ra) { psp_cpu.r[31] = ra; entry_cam_active(); return psp_cpu.r[2]; }

static void test_camera_hooks(void) {
    if (psp_mem_init() != 0) { CHECK(!"psp_mem_init"); return; }
    camera_init(1.0f);
    camera_set_stick(128, 128);
    g_game_yaw = g_game_pitch = 0.0f;
    cam_frame();
    CHECK(psp_read_f32(CAM_YAW) == 0.0f && psp_read_f32(CAM_PITCH) == 0.0f);   /* centred: untouched */

    camera_set_stick(255, 0);                           /* right and up */
    cam_frame();
    CHECK(psp_read_f32(CAM_YAW) == -(127.0f / 128.0f));                 /* yaw = -camX */
    CHECK(psp_read_f32(CAM_PITCH) == -1.0f);                            /* pitch = camY */

    g_game_yaw = 0.3f; g_game_pitch = -0.2f;            /* the game's own camera input wins */
    cam_frame();
    CHECK(psp_read_f32(CAM_YAW) == 0.3f && psp_read_f32(CAM_PITCH) == -0.2f);
    g_game_yaw = g_game_pitch = 0.0f;

    camera_set_speed(2.0f);                             /* sensitivity scales both axes */
    cam_frame();
    CHECK(psp_read_f32(CAM_YAW) == -2.0f * 127.0f / 128.0f && psp_read_f32(CAM_PITCH) == -2.0f);
    camera_set_speed(5.0f);  CHECK(camera_speed() == 2.0f);             /* clamped */
    camera_set_speed(0.5f);  CHECK(camera_speed() == 1.0f);

    camera_set_stick(131, 125);                         /* inside the dead zone */
    cam_frame();
    CHECK(psp_read_f32(CAM_YAW) == 0.0f && psp_read_f32(CAM_PITCH) == 0.0f);

    /* "Camera controlled": only the call the cheat patched (0x08AF8C48). */
    g_game_active = 0;
    camera_set_stick(128, 128);
    CHECK(cam_active(0x08AF8C50u) == 0);
    camera_set_stick(200, 128);
    CHECK(cam_active(0x08AF8C50u) == 1);
    CHECK(cam_active(0x08AF9000u) == 0);                /* other callers unchanged */
    g_game_active = 1;
    camera_set_stick(128, 128);
    CHECK(cam_active(0x08AF8C50u) == 1 && cam_active(0x08AF9000u) == 1);
    psp_mem_free();
}

int main(void) {
    test_open_close();
    test_values();
    test_repeat();
    test_filter();
    test_buttons();
    test_rs_speed();
    test_login();
    test_password_visible();
    test_textedit();
    test_camera_axis();
    test_meter();
    test_draw();
    test_framerate();
    test_camera_hooks();
    printf("test_menu: %d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
