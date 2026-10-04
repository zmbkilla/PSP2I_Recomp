/* The PSN sign-in screen. See login.h.
 *
 * Keyboard: type into the selected field, Backspace deletes, Up/Down/Tab
 * move, Enter on Sign In submits, Escape cancels (letter keys type, they do
 * not act as PSP buttons while this screen is open).
 * Controller: D-pad moves, Circle on a text field opens an on-screen
 * keyboard (D-pad picks a character, Circle types it, Square deletes, Start
 * or Cross closes it), Circle on Sign In submits, Cross cancels.
 * The password is shown as asterisks and never leaves this struct except to
 * be sent, by online.c, to the configured server. */

#include "login.h"

#include <stdio.h>
#include <string.h>

static const char OSK[OSK_ROWS][OSK_COLS + 1] = {
    "1234567890",
    "abcdefghij",
    "klmnopqrst",
    "uvwxyz-_.@",
};

char login_osk_char(int x, int y) {
    if (x < 0 || y < 0 || x >= OSK_COLS || y >= OSK_ROWS) return 0;
    return OSK[y][x];
}

void login_open(login_state *l, const char *server, const char *user) {
    memset(l, 0, sizeof *l);
    l->state = LOGIN_EDITING;
    snprintf(l->server, sizeof l->server, "%s", server ? server : "");
    snprintf(l->user, sizeof l->user, "%s", user ? user : "");
    l->sel = l->user[0] ? LOGIN_FIELD_PASS : LOGIN_FIELD_USER;
    snprintf(l->status, sizeof l->status, "Enter your test account. This is not real PSN.");
}

void login_close(login_state *l) {
    memset(l->pass, 0, sizeof l->pass);
    l->state = LOGIN_CLOSED;
    l->osk = 0;
}

static char *field(login_state *l) {
    return l->sel == LOGIN_FIELD_USER ? l->user : l->sel == LOGIN_FIELD_PASS ? l->pass : NULL;
}

void login_type(login_state *l, char c) {
    char *f = field(l);
    if (l->state != LOGIN_EDITING || !f || c < 32 || c > 126) return;
    const size_t n = strlen(f);
    if (n < LOGIN_TEXT_MAX) { f[n] = c; f[n + 1] = '\0'; }
}

void login_backspace(login_state *l) {
    char *f = field(l);
    if (l->state != LOGIN_EDITING || !f) return;
    const size_t n = strlen(f);
    if (n) f[n - 1] = '\0';
}

void login_set_status(login_state *l, const char *msg, int is_error) {
    snprintf(l->status, sizeof l->status, "%s", msg);
    l->error = is_error;
}

void login_finish(login_state *l, int ok, const char *msg) {
    login_set_status(l, msg, !ok);
    memset(l->pass, 0, sizeof l->pass);
    if (ok) { l->state = LOGIN_DONE_OK; l->done_timer = 90; }
    else    { l->state = LOGIN_EDITING; l->sel = LOGIN_FIELD_PASS; }   /* stay: retry or cancel */
}

int login_update(login_state *l, uint32_t held, uint32_t psp) {
    const uint32_t press = held & ~l->prev;
    l->prev = held;
    static uint32_t prev_psp;
    const uint32_t ppress = psp & ~prev_psp;
    prev_psp = psp;

    if (l->state == LOGIN_DONE_OK) return --l->done_timer <= 0 ? LOGIN_ACT_CLOSE_OK : LOGIN_ACT_NONE;
    if (l->state == LOGIN_WORKING) return (press & MI_BACK) ? LOGIN_ACT_CANCEL : LOGIN_ACT_NONE;
    if (l->state != LOGIN_EDITING) return LOGIN_ACT_NONE;

    if (l->osk) {
        if (press & MI_UP)    l->osk_y = (l->osk_y + OSK_ROWS - 1) % OSK_ROWS;
        if (press & MI_DOWN)  l->osk_y = (l->osk_y + 1) % OSK_ROWS;
        if (press & MI_LEFT)  l->osk_x = (l->osk_x + OSK_COLS - 1) % OSK_COLS;
        if (press & MI_RIGHT) l->osk_x = (l->osk_x + 1) % OSK_COLS;
        if (ppress & 0x8000) login_backspace(l);                         /* Square */
        if (ppress & 0x0008) { l->osk = 0; return LOGIN_ACT_NONE; }      /* Start: done */
        if (press & MI_BACK) { l->osk = 0; return LOGIN_ACT_NONE; }      /* Cross: close the keyboard */
        if ((press & MI_CONFIRM) && !(ppress & 0x0008)) login_type(l, login_osk_char(l->osk_x, l->osk_y));
        return LOGIN_ACT_NONE;
    }

    if (press & MI_UP)   l->sel = (l->sel + LOGIN_ITEMS - 1) % LOGIN_ITEMS;
    if (press & MI_DOWN) l->sel = (l->sel + 1) % LOGIN_ITEMS;
    if ((press & (MI_LEFT | MI_RIGHT)) && l->sel >= LOGIN_BTN_SIGNIN)
        l->sel = l->sel == LOGIN_BTN_SIGNIN ? LOGIN_BTN_CANCEL : LOGIN_BTN_SIGNIN;
    if (press & MI_BACK) return LOGIN_ACT_CANCEL;                        /* Cross: back out */
    if (press & MI_CONFIRM) {
        switch (l->sel) {
        case LOGIN_FIELD_USER: case LOGIN_FIELD_PASS: l->osk = 1; break;
        case LOGIN_BTN_SIGNIN:
            if (!l->user[0] || !l->pass[0]) { login_set_status(l, "Enter both a username and a password.", 1); break; }
            l->state = LOGIN_WORKING;
            login_set_status(l, "Connecting to the server...", 0);
            return LOGIN_ACT_SUBMIT;
        case LOGIN_BTN_CANCEL: return LOGIN_ACT_CANCEL;
        }
    }
    return LOGIN_ACT_NONE;
}

/* ---- drawing ------------------------------------------------------------------------- */

#define WHITE  0xFFFFFFu
#define GREY   0xA0A0A0u
#define YELLOW 0xFFD040u
#define RED    0xFF6060u
#define GREEN  0x60FF80u
#define BLUE   0x2A3A5Au

static void put(menu_image *img, int x, int y, uint32_t rgb) {
    if (x >= 0 && y >= 0 && x < img->w && y < img->h) img->px[y * img->w + x] = 0xFF000000u | rgb;
}
static void shade(menu_image *img, int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++)
        if (i >= 0 && j >= 0 && i < img->w && j < img->h) {
            uint32_t *p = &img->px[j * img->w + i];
            *p = 0xFF000000u | ((*p >> 2) & 0x003F3F3Fu);
        }
}
static void frame(menu_image *img, int x, int y, int w, int h, uint32_t rgb) {
    for (int i = x; i < x + w; i++) { put(img, i, y, rgb); put(img, i, y + h - 1, rgb); }
    for (int j = y; j < y + h; j++) { put(img, x, j, rgb); put(img, x + w - 1, j, rgb); }
}
static void fill(menu_image *img, int x, int y, int w, int h, uint32_t rgb) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) put(img, i, j, rgb);
}

void login_draw(const login_state *l, menu_image *img) {
    if (l->state == LOGIN_CLOSED) return;
    const int bw = 340, bh = 196, x0 = (img->w - bw) / 2, y0 = (img->h - bh) / 2;
    shade(img, x0, y0, bw, bh);
    frame(img, x0, y0, bw, bh, GREY);
    fill(img, x0 + 1, y0 + 1, bw - 2, 14, BLUE);
    menu_text(img, x0 + 8, y0 + 4, "PLAYSTATION NETWORK SIGN-IN", WHITE);
    char line[96];
    snprintf(line, sizeof line, "SERVER: %.44s", l->server);
    menu_text(img, x0 + 8, y0 + 22, line, GREY);

    const int fx = x0 + 96, fw = bw - 108;
    static const char *const LABEL[2] = { "USERNAME", "PASSWORD" };
    for (int i = 0; i < 2; i++) {
        const int y = y0 + 40 + i * 20;
        const int sel = l->sel == i && l->state == LOGIN_EDITING;
        menu_text(img, x0 + 10, y + 3, LABEL[i], sel ? YELLOW : WHITE);
        frame(img, fx, y, fw, 13, sel ? YELLOW : GREY);
        char shown[LOGIN_TEXT_MAX + 2];
        if (i == 0) snprintf(shown, sizeof shown, "%s", l->user);
        else { size_t n = strlen(l->pass); memset(shown, '*', n); shown[n] = '\0'; }
        if (sel && !l->osk) { size_t n = strlen(shown); if (n < LOGIN_TEXT_MAX + 1) { shown[n] = '_'; shown[n + 1] = '\0'; } }
        menu_text(img, fx + 3, y + 3, shown, WHITE);
    }
    static const char *const BTN[2] = { "SIGN IN", "CANCEL" };
    for (int i = 0; i < 2; i++) {
        const int bx = x0 + 70 + i * 110, by = y0 + 86;
        const int sel = l->sel == LOGIN_BTN_SIGNIN + i && l->state == LOGIN_EDITING;
        frame(img, bx, by, 90, 15, sel ? YELLOW : GREY);
        menu_text(img, bx + 45 - menu_text_width(BTN[i]) / 2, by + 4, BTN[i], sel ? YELLOW : WHITE);
    }
    /* status: wrap at the box width */
    const uint32_t sc = l->error ? RED : l->state == LOGIN_DONE_OK ? GREEN : WHITE;
    const int maxc = (bw - 16) / 6;
    const char *s = l->status;
    for (int row = 0; row < 3 && *s; row++) {
        char part[64];
        int n = (int)strlen(s);
        if (n > maxc) { n = maxc; while (n > 0 && s[n] != ' ') n--; if (n == 0) n = maxc; }
        snprintf(part, sizeof part, "%.*s", n, s);
        menu_text(img, x0 + 8, y0 + 110 + row * 10, part, sc);
        s += n;
        while (*s == ' ') s++;
    }

    if (l->osk) {
        const int kx = x0 + 40, ky = y0 + 142;
        for (int r = 0; r < OSK_ROWS; r++)
            for (int c = 0; c < OSK_COLS; c++) {
                const int sel = r == l->osk_y && c == l->osk_x;
                char ch[2] = { login_osk_char(c, r), 0 };
                if (sel) frame(img, kx + c * 26 - 3, ky + r * 11 - 2, 13, 11, YELLOW);
                menu_text(img, kx + c * 26, ky + r * 11, ch, sel ? YELLOW : WHITE);
            }
    } else {
        menu_text(img, x0 + 8, y0 + 150, "CIRCLE / ENTER: ACCEPT   CROSS / ESC: BACK", GREY);
        menu_text(img, x0 + 8, y0 + 160, "KEYBOARD: TYPE INTO THE FIELD, TAB / ARROWS MOVE", GREY);
        menu_text(img, x0 + 8, y0 + 172, "PAD: CIRCLE ON A FIELD OPENS A KEYBOARD", GREY);
    }
}
