/* A one-line text editor for the settings menu. See textedit.h. */

#include "textedit.h"

#include <stdio.h>
#include <string.h>

static const char KEYS[TE_ROWS][TE_COLS + 1] = {
    "1234567890",
    "abcdefghij",
    "klmnopqrst",
    "uvwxyz.-_:",
    "/<<<<<   >",       /* '/' ; "<" = delete, ' ' = space-free filler, '>' = save */
};

char textedit_key(int x, int y) {
    if (x < 0 || y < 0 || x >= TE_COLS || y >= TE_ROWS) return 0;
    return KEYS[y][x];
}

void textedit_open(textedit *e, const char *title, const char *hint, const char *initial) {
    memset(e, 0, sizeof *e);
    e->open = 1;
    snprintf(e->title, sizeof e->title, "%s", title);
    snprintf(e->hint, sizeof e->hint, "%s", hint);
    snprintf(e->text, sizeof e->text, "%s", initial ? initial : "");
    e->prev = 0xFFFFFFFFu;                 /* the press that opened it is not a press here */
    e->prev_psp = 0xFFFFFFFFu;
}

void textedit_type(textedit *e, char c) {
    if (!e->open || c < 33 || c > 126) return;         /* no spaces in an address */
    const size_t n = strlen(e->text);
    if (n < TEXTEDIT_MAX) { e->text[n] = c; e->text[n + 1] = '\0'; }
}

void textedit_backspace(textedit *e) {
    const size_t n = strlen(e->text);
    if (e->open && n) e->text[n - 1] = '\0';
}

int textedit_update(textedit *e, uint32_t held, uint32_t psp) {
    if (!e->open) return TE_NONE;
    const uint32_t press = held & ~e->prev, ppress = psp & ~e->prev_psp;
    e->prev = held;
    e->prev_psp = psp;
    if (press & MI_BACK) { e->open = 0; return TE_CANCEL; }               /* Cross / Escape */
    if (ppress & 0x0008) { e->open = 0; return TE_SAVE; }                 /* Start */
    if (press & MI_UP)    e->ky = (e->ky + TE_ROWS - 1) % TE_ROWS;
    if (press & MI_DOWN)  e->ky = (e->ky + 1) % TE_ROWS;
    if (press & MI_LEFT)  e->kx = (e->kx + TE_COLS - 1) % TE_COLS;
    if (press & MI_RIGHT) e->kx = (e->kx + 1) % TE_COLS;
    if (ppress & 0x8000) textedit_backspace(e);                            /* Square */
    if (press & MI_CONFIRM) {                                              /* Circle / Enter */
        const char k = textedit_key(e->kx, e->ky);
        if (k == '>') { e->open = 0; return TE_SAVE; }
        if (k == '<') textedit_backspace(e);
        else if (k != ' ') textedit_type(e, k);
    }
    return TE_NONE;
}

#define WHITE  0xFFFFFFu
#define GREY   0xA0A0A0u
#define YELLOW 0xFFD040u

static void put(menu_image *img, int x, int y, uint32_t rgb) {
    if (x >= 0 && y >= 0 && x < img->w && y < img->h) img->px[y * img->w + x] = 0xFF000000u | rgb;
}

void textedit_draw(const textedit *e, menu_image *img) {
    if (!e->open) return;
    const int bw = 300, bh = 128, x0 = (img->w - bw) / 2, y0 = (img->h - bh) / 2;
    for (int j = y0; j < y0 + bh; j++) for (int i = x0; i < x0 + bw; i++)
        if (i >= 0 && j >= 0 && i < img->w && j < img->h) {
            uint32_t *p = &img->px[j * img->w + i];
            *p = 0xFF000000u | ((*p >> 3) & 0x001F1F1Fu);
        }
    for (int i = x0; i < x0 + bw; i++) { put(img, i, y0, GREY); put(img, i, y0 + bh - 1, GREY); }
    for (int j = y0; j < y0 + bh; j++) { put(img, x0, j, GREY); put(img, x0 + bw - 1, j, GREY); }
    menu_text(img, x0 + 8, y0 + 6, e->title, WHITE);
    menu_text(img, x0 + 8, y0 + 18, e->hint, GREY);
    /* the text, showing its end if it is long */
    char shown[64];
    const size_t n = strlen(e->text), maxc = 45;
    snprintf(shown, sizeof shown, "%s_", n > maxc ? e->text + n - maxc : e->text);
    for (int i = x0 + 6; i < x0 + bw - 6; i++) { put(img, i, y0 + 30, YELLOW); put(img, i, y0 + 42, YELLOW); }
    menu_text(img, x0 + 9, y0 + 33, shown, WHITE);
    for (int r = 0; r < TE_ROWS; r++)
        for (int c = 0; c < TE_COLS; c++) {
            const char k = textedit_key(c, r);
            const char *label = k == '<' ? "DEL" : k == '>' ? "OK" : NULL;
            if (k == '<' && c > 1) continue;                   /* one DEL key drawn over cells 1-5 */
            if (k == ' ') continue;
            char ch[2] = { k, 0 };
            const int x = x0 + 30 + c * 24, y = y0 + 52 + r * 12;
            const int sel = r == e->ky && (c == e->kx || (k == '<' && e->kx >= 1 && e->kx <= 5));
            menu_text(img, x, y, label ? label : ch, sel ? YELLOW : WHITE);
        }
    menu_text(img, x0 + 8, y0 + 114, "CIRCLE TYPE  SQUARE DEL  START SAVE  CROSS BACK", GREY);
}
