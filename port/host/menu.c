/* The port's settings menu and FPS counter. See menu.h. */

#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the menu --------------------------------------------------------------- */

#define REPEAT_DELAY 20     /* updates (vblanks) before Up/Down auto-repeats */
#define REPEAT_EVERY  6

void menu_init(menu_state *m, int fps, int show_fps) {
    memset(m, 0, sizeof *m);
    m->fps = fps >= 120 ? 120 : fps == 60 ? 60 : 30;
    m->show_fps = show_fps != 0;
    m->res = RES_DEFAULT;
    for (int k = 0; k < VOL_N; k++) m->vol[k] = VOL_STEPS;   /* 100% */
    m->att = ATT_DEFAULT;
    m->touch_alpha = TOUCH_ALPHA_DEFAULT;
    m->rs_on = 1;
    m->hidden = MENU_BIT(MENU_TOUCH_COLOR) | MENU_BIT(MENU_TOUCH_ALPHA);   /* the touch host shows them */
}

static const char *const TOUCH_NAMES[TOUCH_COLORS] = { "DEFAULT", "WHITE", "BLACK", "YELLOW", "CYAN", "PINK" };

const char *menu_touch_color_name(int c) { return c >= 0 && c < TOUCH_COLORS ? TOUCH_NAMES[c] : "?"; }

int menu_touch_color_parse(const char *s) {
    for (int i = 0; i < TOUCH_COLORS; i++) {
        const char *a = TOUCH_NAMES[i], *b = s;
        while (*a && *b && (*a == *b || *a + 32 == *b)) { a++; b++; }
        if (!*a && (!*b || *b == '\r' || *b == '\n' || *b == ' ')) return i;
    }
    return -1;
}

static const struct { const char *name; int w, h; } RES[RES_MODES] = {
    { "960X544",   960,  544 }, { "1280X720", 1280,  720 }, { "1920X1080", 1920, 1080 },
    { "2560X1440", 2560, 1440 }, { "3840X2160", 3840, 2160 }, { "FULLSCREEN", 0, 0 },
};

const char *menu_res_name(int mode) { return mode >= 0 && mode < RES_MODES ? RES[mode].name : "?"; }

int menu_res_size(int mode, int *w, int *h) {
    if (mode < 0 || mode >= RES_MODES || !RES[mode].w) { *w = *h = 0; return 0; }
    *w = RES[mode].w; *h = RES[mode].h;
    return 1;
}

int menu_res_parse(const char *s) {
    for (int i = 0; i < RES_MODES; i++) {
        const char *a = RES[i].name, *b = s;
        while (*a && *b && (*a == *b || (*a >= 'A' && *a <= 'Z' && *a + 32 == *b))) { a++; b++; }
        if (!*a && (!*b || *b == '\r' || *b == '\n' || *b == ' ')) return i;
    }
    return -1;
}

int menu_att_db(int mode) { static const int DB[ATT_MODES] = { 0, -6, -12, -20 }; return mode >= 0 && mode < ATT_MODES ? DB[mode] : 0; }

int menu_att_parse(int db) {
    if (db >= 0) return ATT_OFF;
    int best = ATT_6DB;
    for (int i = ATT_6DB; i < ATT_MODES; i++)
        if (abs(menu_att_db(i) - db) < abs(menu_att_db(best) - db)) best = i;
    return best;
}

const char *menu_ren_key(int r) { return r == REN_GL ? "opengl" : r == REN_SOFTWARE ? "software" : "d3d11"; }

int menu_ren_parse(const char *s) {
    static const struct { const char *k; int r; } K[] = {
        { "d3d11", REN_D3D11 }, { "opengl", REN_GL }, { "gl", REN_GL }, { "software", REN_SOFTWARE } };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
        const char *a = K[i].k, *b = s;
        while (*a && *b && (*a == *b || *a - 32 == *b)) { a++; b++; }
        if (!*a && (!*b || *b == '\r' || *b == '\n' || *b == ' ')) return K[i].r;
    }
    return -1;
}

uint32_t menu_inputs_from_psp(uint32_t psp, uint8_t ax, uint8_t ay) {
    enum { P_START = 0x0008, P_UP = 0x0010, P_RIGHT = 0x0020, P_DOWN = 0x0040, P_LEFT = 0x0080,
           P_CIRCLE = 0x2000, P_CROSS = 0x4000 };
    uint32_t m = 0;
    if ((psp & P_UP) || ay < 64)     m |= MI_UP;
    if ((psp & P_DOWN) || ay > 192)  m |= MI_DOWN;
    if ((psp & P_LEFT) || ax < 64)   m |= MI_LEFT;
    if ((psp & P_RIGHT) || ax > 192) m |= MI_RIGHT;
    if (psp & (P_CIRCLE | P_START))  m |= MI_CONFIRM;
    if (psp & P_CROSS)               m |= MI_BACK;
    return m;
}

float menu_rs_speed(const menu_state *m) { return 1.0f + 0.25f * (float)m->rs_step; }

void menu_set_rs_speed(menu_state *m, float speed) {
    int k = (int)((speed - 1.0f) / 0.25f + 0.5f);
    m->rs_step = k < 0 ? 0 : k >= MENU_RS_STEPS ? MENU_RS_STEPS - 1 : k;
}

/* Change the selected value. dir: -1 Left, +1 Right, 0 Confirm. The two-way
 * values flip either way; the sensitivity steps down/up with Left/Right
 * (stopping at the ends) and Confirm cycles it. */
static int change(menu_state *m, int dir) {
    switch (m->sel) {
    case MENU_FRAME_RATE:                       /* 30 -> 60 -> 120 (Left: the other way) */
        m->fps = dir < 0 ? (m->fps == 30 ? 120 : m->fps == 120 ? 60 : 30) : (m->fps == 30 ? 60 : m->fps == 60 ? 120 : 30);
        return MFX_FPS_CHANGED;
    case MENU_FPS_COUNTER: m->show_fps = !m->show_fps;      return MFX_SHOW_FPS;
    case MENU_SEGA_SERVER: return dir == 0 ? MFX_EDIT_SEGA : 0;
    case MENU_ADHOC_SERVER: return dir == 0 ? MFX_EDIT_ADHOC : 0;
    case MENU_MODERN_SERVER: return dir == 0 ? MFX_EDIT_MODERN : 0;
    case MENU_ADHOC_MODE:  m->adhoc_mode = (m->adhoc_mode + (dir < 0 ? 2 : 1)) % 3; return MFX_ADHOC_MODE;
    case MENU_RENDERER:    m->renderer = (m->renderer + 1) % REN_MENU_CHOICES; return MFX_RENDERER;
    case MENU_MASTER_VOL: case MENU_MUSIC_VOL: case MENU_SFX_VOL: {
        int *v = &m->vol[m->sel - MENU_MASTER_VOL];
        const int old = *v;
        if (dir < 0)      { if (*v > 0) (*v)--; }
        else if (dir > 0) { if (*v < VOL_STEPS) (*v)++; }
        else              *v = (*v + 1) % (VOL_STEPS + 1);
        return *v != old ? MFX_AUDIO : 0;
    }
    case MENU_ATTENUATION:
        m->att = (m->att + (dir < 0 ? ATT_MODES - 1 : 1)) % ATT_MODES;
        return MFX_AUDIO;
    case MENU_SWAP_OC:     m->swap_oc = !m->swap_oc; return MFX_SWAP_OC;
    case MENU_RS_ON:       m->rs_on = !m->rs_on; return MFX_RS_ON;
    case MENU_TEST_TICKS:  m->test_ticks = !m->test_ticks; return MFX_TESTS;
    case MENU_TEST_LOGIC:  m->test_logic = !m->test_logic; return MFX_TESTS;
    case MENU_TOUCH_COLOR:
        m->touch_color = (m->touch_color + (dir < 0 ? TOUCH_COLORS - 1 : 1)) % TOUCH_COLORS;
        return MFX_TOUCH;
    case MENU_TOUCH_ALPHA: {
        const int old = m->touch_alpha;
        if (dir < 0)      { if (m->touch_alpha > TOUCH_ALPHA_MIN) m->touch_alpha--; }
        else if (dir > 0) { if (m->touch_alpha < 10) m->touch_alpha++; }
        else              m->touch_alpha = m->touch_alpha >= 10 ? TOUCH_ALPHA_MIN : m->touch_alpha + 1;
        return m->touch_alpha != old ? MFX_TOUCH : 0;
    }
    case MENU_FONT:
        if (m->font_n < 2) return 0;
        m->font = (m->font + (dir < 0 ? m->font_n - 1 : 1)) % m->font_n;
        return MFX_FONT;
    case MENU_RESOLUTION:
        m->res = (m->res + (dir < 0 ? RES_MODES - 1 : 1)) % RES_MODES;
        return MFX_RES_CHANGED;
    case MENU_RS_SPEED: {
        const int old = m->rs_step;
        if (dir < 0)      { if (m->rs_step > 0) m->rs_step--; }
        else if (dir > 0) { if (m->rs_step < MENU_RS_STEPS - 1) m->rs_step++; }
        else              m->rs_step = (m->rs_step + 1) % MENU_RS_STEPS;
        return m->rs_step != old ? MFX_RS_SPEED : 0;
    }
    default:               return 0;
    }
}

int menu_page_of(int item) {
    return item >= MENU_TEST_TICKS ? MENU_PAGE_TESTS : item >= MENU_RS_ON ? MENU_PAGE_CONTROLS : MENU_PAGE_MAIN;
}

/* The main page's item that opens a sub-page (where BACK returns to). */
static int page_item(int page) { return page == MENU_PAGE_TESTS ? MENU_TESTS : MENU_CONTROLS; }

/* On the page shown, and not left out by this host. */
static int shown(const menu_state *m, int item) {
    return !(m->hidden & MENU_BIT(item)) && menu_page_of(item) == m->page;
}

/* Show a page, with `sel` selected (-1: its first item). */
static void go_page(menu_state *m, int page, int sel) {
    m->page = page;
    m->hold = 0;
    if (sel >= 0) { m->sel = sel; return; }
    m->sel = 0;
    while (m->sel < MENU_ITEMS - 1 && !shown(m, m->sel)) m->sel++;
}

int menu_update(menu_state *m, uint32_t held, uint32_t latched) {
    const uint32_t press = (held & ~m->prev) | latched;
    m->prev = held;
    int fx = 0;

    if (!m->open) {
        if (press & MI_TOGGLE) { m->open = 1; go_page(m, MENU_PAGE_MAIN, -1); fx |= MFX_OPENED; }
        return fx;
    }
    if (press & MI_TOGGLE) { m->open = 0; return fx | MFX_CLOSED; }
    if (press & MI_BACK) {                      /* a sub-page backs out to the main page */
        if (m->page != MENU_PAGE_MAIN) { go_page(m, MENU_PAGE_MAIN, page_item(m->page)); return fx; }
        m->open = 0;
        return fx | MFX_CLOSED;
    }

    /* Up/Down: once per press, then auto-repeat while held. */
    int step = 0;
    if (press & MI_UP) step = -1;
    else if (press & MI_DOWN) step = 1;
    if (held & (MI_UP | MI_DOWN)) {
        if (++m->hold > REPEAT_DELAY && (m->hold - REPEAT_DELAY) % REPEAT_EVERY == 0)
            step = (held & MI_UP) ? -1 : 1;
    } else {
        m->hold = 0;
    }
    if (step)
        do m->sel = (m->sel + step + MENU_ITEMS) % MENU_ITEMS;
        while (!shown(m, m->sel));

    if (press & (MI_LEFT | MI_RIGHT | MI_CONFIRM)) {
        if (m->sel == MENU_CLOSE) {
            if (press & MI_CONFIRM) { m->open = 0; fx |= MFX_CLOSED; }
        } else if (m->sel == MENU_CONTROLS || m->sel == MENU_TESTS) {
            if (press & (MI_CONFIRM | MI_RIGHT)) go_page(m, m->sel == MENU_TESTS ? MENU_PAGE_TESTS : MENU_PAGE_CONTROLS, -1);
        } else if (m->sel == MENU_CONTROLS_BACK || m->sel == MENU_TESTS_BACK) {
            if (press & (MI_CONFIRM | MI_LEFT)) go_page(m, MENU_PAGE_MAIN, page_item(m->page));
        } else {
            fx |= change(m, (press & MI_LEFT) ? -1 : (press & MI_RIGHT) ? 1 : 0);
        }
    }
    return fx;
}

uint32_t menu_filter_game(menu_state *m, uint32_t psp_buttons) {
    if (m->open) { m->game_mask = psp_buttons; return 0; }
    m->game_mask &= psp_buttons;           /* released buttons are free again */
    return psp_buttons & ~m->game_mask;
}

/* ---- measured frame rate --------------------------------------------------- */

void fps_meter_init(fps_meter *fm) {
    memset(fm, 0, sizeof *fm);
    fm->value = -1.0;
}

void fps_meter_sample(fps_meter *fm, uint64_t now_us, uint64_t flips) {
    fm->t[fm->head] = now_us;
    fm->f[fm->head] = flips;
    fm->head = (fm->head + 1) % FPS_METER_SAMPLES;
    if (fm->n < FPS_METER_SAMPLES) fm->n++;
    if (fm->n < 2) { fm->refreshed = now_us; return; }
    if (now_us - fm->refreshed < FPS_METER_REFRESH_US) return;
    fm->refreshed = now_us;

    /* The oldest sample still inside the window. */
    int oldest = (fm->head - 1 + FPS_METER_SAMPLES) % FPS_METER_SAMPLES;
    for (int k = 1; k < fm->n; k++) {
        const int i = (fm->head - 1 - k + 2 * FPS_METER_SAMPLES) % FPS_METER_SAMPLES;
        if (now_us - fm->t[i] > FPS_METER_WINDOW_US) break;
        oldest = i;
    }
    const uint64_t dt = now_us - fm->t[oldest];
    if (dt) fm->value = (double)(flips - fm->f[oldest]) * 1e6 / (double)dt;
}

double fps_meter_value(const fps_meter *fm) { return fm->value; }

/* ---- the font ---------------------------------------------------------------
 * 5x7, one byte per row, bit 4 = leftmost column. */
static const struct { char c; uint8_t r[7]; } FONT[] = {
    { '0', { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E } }, { '1', { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E } },
    { '2', { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F } }, { '3', { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E } },
    { '4', { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 } }, { '5', { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E } },
    { '6', { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E } }, { '7', { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 } },
    { '8', { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E } }, { '9', { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C } },
    { 'A', { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 } }, { 'B', { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E } },
    { 'C', { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E } }, { 'D', { 0x1C,0x12,0x11,0x11,0x11,0x12,0x1C } },
    { 'E', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F } }, { 'F', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 } },
    { 'G', { 0x0E,0x11,0x10,0x17,0x11,0x11,0x0F } }, { 'H', { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 } },
    { 'I', { 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E } }, { 'J', { 0x07,0x02,0x02,0x02,0x02,0x12,0x0C } },
    { 'K', { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 } }, { 'L', { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F } },
    { 'M', { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 } }, { 'N', { 0x11,0x11,0x19,0x15,0x13,0x11,0x11 } },
    { 'O', { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E } }, { 'P', { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 } },
    { 'Q', { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D } }, { 'R', { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 } },
    { 'S', { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E } }, { 'T', { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 } },
    { 'U', { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E } }, { 'V', { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 } },
    { 'W', { 0x11,0x11,0x11,0x15,0x15,0x15,0x0A } }, { 'X', { 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 } },
    { 'Y', { 0x11,0x11,0x11,0x0A,0x04,0x04,0x04 } }, { 'Z', { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F } },
    { '.', { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C } }, { ':', { 0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00 } },
    { '-', { 0x00,0x00,0x00,0x1F,0x00,0x00,0x00 } }, { '/', { 0x00,0x01,0x02,0x04,0x08,0x10,0x00 } },
    { '(', { 0x02,0x04,0x08,0x08,0x08,0x04,0x02 } }, { ')', { 0x08,0x04,0x02,0x02,0x02,0x04,0x08 } },
    { '<', { 0x02,0x04,0x08,0x10,0x08,0x04,0x02 } }, { '>', { 0x08,0x04,0x02,0x01,0x02,0x04,0x08 } },
    { '=', { 0x00,0x00,0x1F,0x00,0x1F,0x00,0x00 } }, { '+', { 0x00,0x04,0x04,0x1F,0x04,0x04,0x00 } },
    { ',', { 0x00,0x00,0x00,0x00,0x0C,0x04,0x08 } }, { '%', { 0x18,0x19,0x02,0x04,0x08,0x13,0x03 } },
    /* '*' masks the sign-in password and '_' is the text cursor: without
     * them both drew as blanks, so a typed password looked like no input. */
    { '*', { 0x00,0x04,0x15,0x0E,0x15,0x04,0x00 } }, { '_', { 0x00,0x00,0x00,0x00,0x00,0x00,0x1F } },
    { '@', { 0x0E,0x11,0x17,0x15,0x17,0x10,0x0E } }, { '!', { 0x04,0x04,0x04,0x04,0x04,0x00,0x04 } },
    { '?', { 0x0E,0x11,0x01,0x02,0x04,0x00,0x04 } }, { '#', { 0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A } },
    { '[', { 0x0E,0x08,0x08,0x08,0x08,0x08,0x0E } }, { ']', { 0x0E,0x02,0x02,0x02,0x02,0x02,0x0E } },
    /* Lowercase, for typed text (the sign-in keyboard's Shift); other text
     * keeps drawing in capitals when written that way. */
    { 'a', { 0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F } }, { 'b', { 0x10,0x10,0x16,0x19,0x11,0x11,0x1E } },
    { 'c', { 0x00,0x00,0x0E,0x10,0x10,0x11,0x0E } }, { 'd', { 0x01,0x01,0x0D,0x13,0x11,0x11,0x0F } },
    { 'e', { 0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E } }, { 'f', { 0x06,0x09,0x08,0x1C,0x08,0x08,0x08 } },
    { 'g', { 0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E } }, { 'h', { 0x10,0x10,0x16,0x19,0x11,0x11,0x11 } },
    { 'i', { 0x04,0x00,0x0C,0x04,0x04,0x04,0x0E } }, { 'j', { 0x02,0x00,0x06,0x02,0x02,0x12,0x0C } },
    { 'k', { 0x10,0x10,0x12,0x14,0x18,0x14,0x12 } }, { 'l', { 0x0C,0x04,0x04,0x04,0x04,0x04,0x0E } },
    { 'm', { 0x00,0x00,0x1A,0x15,0x15,0x11,0x11 } }, { 'n', { 0x00,0x00,0x16,0x19,0x11,0x11,0x11 } },
    { 'o', { 0x00,0x00,0x0E,0x11,0x11,0x11,0x0E } }, { 'p', { 0x00,0x00,0x1E,0x11,0x1E,0x10,0x10 } },
    { 'q', { 0x00,0x00,0x0D,0x13,0x0F,0x01,0x01 } }, { 'r', { 0x00,0x00,0x16,0x19,0x10,0x10,0x10 } },
    { 's', { 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E } }, { 't', { 0x08,0x08,0x1C,0x08,0x08,0x09,0x06 } },
    { 'u', { 0x00,0x00,0x11,0x11,0x11,0x13,0x0D } }, { 'v', { 0x00,0x00,0x11,0x11,0x11,0x0A,0x04 } },
    { 'w', { 0x00,0x00,0x11,0x11,0x15,0x15,0x0A } }, { 'x', { 0x00,0x00,0x11,0x0A,0x04,0x0A,0x11 } },
    { 'y', { 0x00,0x00,0x11,0x11,0x0F,0x01,0x0E } }, { 'z', { 0x00,0x00,0x1F,0x02,0x04,0x08,0x1F } },
    { '$', { 0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04 } }, { '&', { 0x0C,0x12,0x14,0x08,0x15,0x12,0x0D } },
    { '~', { 0x00,0x00,0x08,0x15,0x02,0x00,0x00 } },
};

static const uint8_t *glyph(char c) {
    for (size_t i = 0; i < sizeof FONT / sizeof FONT[0]; i++)
        if (FONT[i].c == c) return FONT[i].r;
    return NULL;
}

#define CELL_W 6
#define CELL_H 9

int menu_text_width(const char *s) { return (int)strlen(s) * CELL_W; }

static void put(menu_image *img, int x, int y, uint32_t rgb) {
    if (x >= 0 && y >= 0 && x < img->w && y < img->h) img->px[y * img->w + x] = 0xFF000000u | rgb;
}

void menu_text(menu_image *img, int x, int y, const char *s, uint32_t rgb) {
    for (; *s; s++, x += CELL_W) {
        const uint8_t *g = glyph(*s);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c)) put(img, x + c, y + r, rgb);
    }
}

/* Darken a rectangle to 1/4 brightness, so text over any scene stays legible. */
static void shade(menu_image *img, int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= img->h) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= img->w) continue;
            uint32_t *p = &img->px[j * img->w + i];
            *p = 0xFF000000u | ((*p >> 2) & 0x003F3F3Fu);
        }
    }
}

static void frame(menu_image *img, int x, int y, int w, int h, uint32_t rgb) {
    for (int i = x; i < x + w; i++) { put(img, i, y, rgb); put(img, i, y + h - 1, rgb); }
    for (int j = y; j < y + h; j++) { put(img, x, j, rgb); put(img, x + w - 1, j, rgb); }
}

#define WHITE  0xFFFFFFu
#define GREY   0xA0A0A0u
#define YELLOW 0xFFD040u
#define ORANGE 0xFF9040u

/* The box: 261 pixels tall with 16 items shown, one row more or less per
 * item on the page. */
static int shown_items(const menu_state *m) {
    int n = 0;
    for (int i = 0; i < MENU_ITEMS; i++) n += shown(m, i);
    return n;
}
#define BOX_W 288
#define ROW_H (CELL_H + 2)
#define ROWS_Y (8 + CELL_H + 8)     /* the first item's row, from the box's top */
#define VALUE_X 130                 /* the values' column, from the box's left */

void menu_box(const menu_state *m, int w, int h, int *x, int *y, int *bw, int *bh) {
    *bw = BOX_W;
    *bh = 261 + ROW_H * (shown_items(m) - 16);
    *x = (w - *bw) / 2;
    *y = (h - *bh) / 2;
}

static int is_editor(int item) {
    return item == MENU_SEGA_SERVER || item == MENU_ADHOC_SERVER || item == MENU_MODERN_SERVER;
}

int menu_touch(menu_state *m, int w, int h, int x, int y) {
    if (!m->open) return 0;
    int x0, y0, bw, bh;
    menu_box(m, w, h, &x0, &y0, &bw, &bh);
    if (x < x0 || x >= x0 + bw || y < y0 || y >= y0 + bh) {    /* outside: back a page, or close */
        if (m->page != MENU_PAGE_MAIN) { go_page(m, MENU_PAGE_MAIN, page_item(m->page)); return 0; }
        m->open = 0;
        return MFX_CLOSED;
    }
    const int row = (y - (y0 + ROWS_Y - 1)) / ROW_H;
    if (y < y0 + ROWS_Y - 1 || row < 0) return 0;
    int item = -1;
    for (int i = 0, r = 0; i < MENU_ITEMS; i++) {
        if (!shown(m, i)) continue;
        if (r++ == row) { item = i; break; }
    }
    if (item < 0) return 0;
    m->sel = item;
    if (item == MENU_CLOSE) { m->open = 0; return MFX_CLOSED; }
    if (item == MENU_CONTROLS) { go_page(m, MENU_PAGE_CONTROLS, -1); return 0; }
    if (item == MENU_TESTS) { go_page(m, MENU_PAGE_TESTS, -1); return 0; }
    if (item == MENU_CONTROLS_BACK || item == MENU_TESTS_BACK) { go_page(m, MENU_PAGE_MAIN, page_item(m->page)); return 0; }
    if (is_editor(item)) return change(m, 0);
    return change(m, x < x0 + VALUE_X + 30 ? -1 : 1);
}

void menu_draw(const menu_state *m, menu_image *img, int game_fps, const char *hotkey) {
    if (!m->open) return;
    char line[96];
    int x0, y0, bw, bh;
    menu_box(m, img->w, img->h, &x0, &y0, &bw, &bh);
    shade(img, x0, y0, bw, bh);
    frame(img, x0, y0, bw, bh, GREY);

    int y = y0 + 8;
    menu_text(img, x0 + 10, y, m->page == MENU_PAGE_CONTROLS ? "SETTINGS > CONTROLS"
                             : m->page == MENU_PAGE_TESTS ? "SETTINGS > TESTS (60 / 120 FPS)" : "SETTINGS", WHITE);
    y += CELL_H + 8;

    static const char *const NAMES[MENU_ITEMS] = { "FRAME RATE", "FPS COUNTER", "SEGA SERVER", "ADHOC SERVER",
                                                   "ADHOC MODE", "MODERN SERVER", "RESOLUTION", "RENDERER", "MASTER VOLUME",
                                                   "MUSIC VOLUME", "SFX VOLUME", "ATTENUATION", "FONT", "CONTROLS", "TESTS",
                                                   "CLOSE", "RIGHT STICK", "SENSITIVITY", "CIRCLE/CROSS", "TOUCH COLOR",
                                                   "TOUCH OPACITY", "BACK", "TICK PACING", "30 HZ LOGIC", "BACK" };
    static const char *const ADHOC_MODES[3] = { "< PPSSPP DIRECT >", "< PPSSPP RELAY >", "< MODERN >" };
    char adhoc[32];
    snprintf(adhoc, sizeof adhoc, "%.25s", m->adhoc[0] ? m->adhoc : "NOT SET");
    char modern[32];
    if (!m->modern[0] || !strcmp(m->modern, "host")) snprintf(modern, sizeof modern, "HOST (THIS GAME)");
    else snprintf(modern, sizeof modern, "%.25s", m->modern);
    char vol[VOL_N][16], att[24];
    for (int k = 0; k < VOL_N; k++) snprintf(vol[k], sizeof vol[k], "< %d%% >", m->vol[k] * 10);
    if (m->att == ATT_OFF) snprintf(att, sizeof att, "< OFF >");
    else snprintf(att, sizeof att, "< %d DB >", menu_att_db(m->att));
    char ren[32];                                       /* the change applies at the next start */
    snprintf(ren, sizeof ren, "< %s >%s", m->renderer == REN_GL ? "OPENGL" : "D3D11", m->renderer != m->renderer_now ? " RESTART" : "");
    char res[24];
    snprintf(res, sizeof res, "< %s >", menu_res_name(m->res));
    char sega[32];
    if (m->sega[0]) snprintf(sega, sizeof sega, "%.25s", m->sega);
    else snprintf(sega, sizeof sega, "OFF");
    char rs[24];
    snprintf(rs, sizeof rs, "< %.2fX >", (double)menu_rs_speed(m));
    char font[32];
    if (m->font_n > 1) snprintf(font, sizeof font, "< %.18s >", m->font_name);
    else snprintf(font, sizeof font, "%.24s", m->font_name[0] ? m->font_name : "NONE");
    char tcol[24], talpha[24];
    snprintf(tcol, sizeof tcol, "< %s >", menu_touch_color_name(m->touch_color));
    snprintf(talpha, sizeof talpha, "< %d%% >", m->touch_alpha * 10);
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (!shown(m, i)) continue;
        const uint32_t col = i == m->sel ? YELLOW : WHITE;
        if (i == m->sel) menu_text(img, x0 + 10, y, ">", col);
        menu_text(img, x0 + 22, y, NAMES[i], col);
        const char *v = i == MENU_FRAME_RATE ? (m->fps == 120 ? "< 120 FPS >" : m->fps == 60 ? "< 60 FPS >" : "< 30 FPS >")
                      : i == MENU_FPS_COUNTER ? (m->show_fps ? "< ON >" : "< OFF >")
                      : i == MENU_TEST_TICKS ? (m->test_ticks ? "< ON >" : "< OFF >")
                      : i == MENU_TEST_LOGIC ? (m->test_logic ? "< ON >" : "< OFF >")
                      : i == MENU_RS_ON ? (m->rs_on ? "< ON >" : "< OFF >") : i == MENU_CONTROLS || i == MENU_TESTS ? ">"
                      : i == MENU_RS_SPEED ? rs : i == MENU_SEGA_SERVER ? sega : i == MENU_ADHOC_SERVER ? adhoc
                      : i == MENU_ADHOC_MODE ? ADHOC_MODES[m->adhoc_mode % 3]
                      : i == MENU_MODERN_SERVER ? modern
                      : i == MENU_RESOLUTION ? res : i == MENU_RENDERER ? ren
                      : i >= MENU_MASTER_VOL && i <= MENU_SFX_VOL ? vol[i - MENU_MASTER_VOL]
                      : i == MENU_ATTENUATION ? att : i == MENU_FONT ? font
                      : i == MENU_SWAP_OC ? (m->swap_oc ? "< SWAPPED >" : "< NORMAL >")
                      : i == MENU_TOUCH_COLOR ? tcol : i == MENU_TOUCH_ALPHA ? talpha : "";
        menu_text(img, x0 + VALUE_X, y, v, col);
        y += ROW_H;
    }
    y += 4;
    if (game_fps > 0) snprintf(line, sizeof line, "GAME TIMING NOW: %d FPS", game_fps);
    else              snprintf(line, sizeof line, "GAME TIMING NOW: NOT SET YET");
    menu_text(img, x0 + 10, y, line, GREY);
    y += CELL_H + 2;
    if (m->fps >= 60) {
        snprintf(line, sizeof line, "%d FPS IS EXPERIMENTAL", m->fps);
        menu_text(img, x0 + 10, y, line, ORANGE);
    }
    y += CELL_H + 4;
    menu_text(img, x0 + 10, y, m->swap_oc ? "CIRCLE (KEY Z): ACCEPT  CROSS (KEY X): BACK"
                                          : "CIRCLE (KEY X): ACCEPT  CROSS (KEY Z): BACK", GREY);
    y += CELL_H + 2;
    if (m->sel == MENU_MODERN_SERVER && m->share[0]) {
        snprintf(line, sizeof line, "JOIN AT %.38s", m->share);   /* what the others type */
        menu_text(img, x0 + 10, y, line, YELLOW);
    } else {
        snprintf(line, sizeof line, "%s / R3: CLOSE   D-PAD / ARROWS: MOVE", hotkey ? hotkey : "F1");
        menu_text(img, x0 + 10, y, line, GREY);
    }
}

void fps_draw(const fps_meter *fm, menu_image *img) {
    char line[32];
    const double v = fps_meter_value(fm);
    if (v < 0) snprintf(line, sizeof line, "ACTUAL --.- FPS");
    else       snprintf(line, sizeof line, "ACTUAL %4.1f FPS", v);
    const int w = menu_text_width(line) + 5, h = CELL_H + 2;
    shade(img, 2, 2, w, h);
    menu_text(img, 5, 4, line, WHITE);
}
