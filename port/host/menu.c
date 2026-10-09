/* The port's settings menu and FPS counter. See menu.h. */

#include "menu.h"

#include <stdio.h>
#include <string.h>

/* ---- the menu --------------------------------------------------------------- */

#define REPEAT_DELAY 20     /* updates (vblanks) before Up/Down auto-repeats */
#define REPEAT_EVERY  6

void menu_init(menu_state *m, int fps, int show_fps) {
    memset(m, 0, sizeof *m);
    m->fps = fps == 60 ? 60 : 30;
    m->show_fps = show_fps != 0;
    m->res = RES_DEFAULT;
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
    case MENU_FRAME_RATE:  m->fps = m->fps == 60 ? 30 : 60; return MFX_FPS_CHANGED;
    case MENU_FPS_COUNTER: m->show_fps = !m->show_fps;      return MFX_SHOW_FPS;
    case MENU_SEGA_SERVER: return dir == 0 ? MFX_EDIT_SEGA : 0;
    case MENU_ADHOC_SERVER: return dir == 0 ? MFX_EDIT_ADHOC : 0;
    case MENU_ADHOC_MODE:  m->adhoc_mode = (m->adhoc_mode + (dir < 0 ? 2 : 1)) % 3; return MFX_ADHOC_MODE;
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

int menu_update(menu_state *m, uint32_t held, uint32_t latched) {
    const uint32_t press = (held & ~m->prev) | latched;
    m->prev = held;
    int fx = 0;

    if (!m->open) {
        if (press & MI_TOGGLE) { m->open = 1; m->sel = MENU_FRAME_RATE; m->hold = 0; fx |= MFX_OPENED; }
        return fx;
    }
    if (press & (MI_TOGGLE | MI_BACK)) { m->open = 0; return fx | MFX_CLOSED; }

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
    if (step) m->sel = (m->sel + step + MENU_ITEMS) % MENU_ITEMS;

    if (press & (MI_LEFT | MI_RIGHT | MI_CONFIRM)) {
        if (m->sel == MENU_CLOSE) {
            if (press & MI_CONFIRM) { m->open = 0; fx |= MFX_CLOSED; }
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
};

static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
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

void menu_draw(const menu_state *m, menu_image *img, int game_fps, const char *hotkey) {
    if (!m->open) return;
    char line[96];
    const int bw = 288, bh = 196;
    const int x0 = (img->w - bw) / 2, y0 = (img->h - bh) / 2;
    shade(img, x0, y0, bw, bh);
    frame(img, x0, y0, bw, bh, GREY);

    int y = y0 + 8;
    menu_text(img, x0 + 10, y, "SETTINGS", WHITE);
    y += CELL_H + 8;

    static const char *const NAMES[MENU_ITEMS] = { "FRAME RATE", "FPS COUNTER", "RIGHT STICK", "SEGA SERVER", "ADHOC SERVER",
                                                   "ADHOC MODE", "RESOLUTION", "CLOSE" };
    static const char *const ADHOC_MODES[3] = { "< PPSSPP DIRECT >", "< PPSSPP RELAY >", "< MODERN >" };
    char adhoc[32];
    snprintf(adhoc, sizeof adhoc, "%.25s", m->adhoc[0] ? m->adhoc : "NOT SET");
    char res[24];
    snprintf(res, sizeof res, "< %s >", menu_res_name(m->res));
    char sega[32];
    if (m->sega[0]) snprintf(sega, sizeof sega, "%.25s", m->sega);
    else snprintf(sega, sizeof sega, "OFF");
    char rs[24];
    snprintf(rs, sizeof rs, "< %.2fX >", (double)menu_rs_speed(m));
    for (int i = 0; i < MENU_ITEMS; i++) {
        const uint32_t col = i == m->sel ? YELLOW : WHITE;
        if (i == m->sel) menu_text(img, x0 + 10, y, ">", col);
        menu_text(img, x0 + 22, y, NAMES[i], col);
        const char *v = i == MENU_FRAME_RATE ? (m->fps == 60 ? "< 60 FPS >" : "< 30 FPS >")
                      : i == MENU_FPS_COUNTER ? (m->show_fps ? "< ON >" : "< OFF >")
                      : i == MENU_RS_SPEED ? rs : i == MENU_SEGA_SERVER ? sega : i == MENU_ADHOC_SERVER ? adhoc
                      : i == MENU_ADHOC_MODE ? ADHOC_MODES[m->adhoc_mode % 3]
                      : i == MENU_RESOLUTION ? res : "";
        menu_text(img, x0 + 130, y, v, col);
        y += CELL_H + 4;
    }
    y += 4;
    if (game_fps > 0) snprintf(line, sizeof line, "GAME TIMING NOW: %d FPS", game_fps);
    else              snprintf(line, sizeof line, "GAME TIMING NOW: NOT SET YET");
    menu_text(img, x0 + 10, y, line, GREY);
    y += CELL_H + 2;
    if (m->fps == 60) menu_text(img, x0 + 10, y, "60 FPS IS EXPERIMENTAL", ORANGE);
    y += CELL_H + 4;
    menu_text(img, x0 + 10, y, "CIRCLE (KEY X): ACCEPT  CROSS (KEY Z): BACK", GREY);
    y += CELL_H + 2;
    snprintf(line, sizeof line, "%s / R3: CLOSE   D-PAD / ARROWS: MOVE", hotkey ? hotkey : "F1");
    menu_text(img, x0 + 10, y, line, GREY);
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
