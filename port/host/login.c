/* The PSN sign-in screen. See login.h.
 *
 * Keyboard: type into the selected field, Backspace deletes, Up/Down/Tab
 * move, Enter on a button presses it, Escape goes back (letter keys type,
 * they do not act as PSP buttons while this screen is open).
 * Controller: D-pad moves, Circle on a text field opens an on-screen
 * keyboard (D-pad picks a character, Circle types it, Triangle is Shift --
 * capitals and symbols --, Square deletes, Start or Cross closes it), Circle on a button presses it, Cross goes back.
 * The password is shown as asterisks and never leaves this struct except to
 * be sent, by online.c, to the configured server. */

#include "login.h"

#include <stdio.h>
#include <string.h>

static const char OSK[2][OSK_ROWS][OSK_COLS + 1] = {
    { "1234567890", "abcdefghij", "klmnopqrst", "uvwxyz-_.@" },
    { "!#$%&*+=?~", "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ()/:" },   /* Shift */
};

char login_osk_char(int x, int y, int shift) {
    if (x < 0 || y < 0 || x >= OSK_COLS || y >= OSK_ROWS) return 0;
    return OSK[shift ? 1 : 0][y][x];
}

static const char *const SIGNIN_HINT = "Enter your test account. This is not real PSN. New here? REGISTER.";

void login_open(login_state *l, const char *server, const char *user) {
    memset(l, 0, sizeof *l);
    l->state = LOGIN_EDITING;
    l->page = LOGIN_PAGE_SIGNIN;
    snprintf(l->server, sizeof l->server, "%s", server ? server : "");
    snprintf(l->user, sizeof l->user, "%s", user ? user : "");
    l->sel = l->user[0] ? LOGIN_FIELD_PASS : LOGIN_FIELD_USER;
    snprintf(l->status, sizeof l->status, "%s", SIGNIN_HINT);
}

void login_close(login_state *l) {
    memset(l->pass, 0, sizeof l->pass);
    l->state = LOGIN_CLOSED;
    l->osk = 0;
}

void login_set_page(login_state *l, int page) {
    l->page = page;
    l->osk = 0;
    l->error = 0;
    l->sel = !l->user[0] ? LOGIN_FIELD_USER : !l->pass[0] ? LOGIN_FIELD_PASS : page == LOGIN_PAGE_REGISTER ? LOGIN_FIELD_EXTRA : LOGIN_BTN_OK;
    snprintf(l->status, sizeof l->status, "%s", page == LOGIN_PAGE_REGISTER
             ? "A new account on this server: a username, a password and your email."
             : SIGNIN_HINT);
}

static char *field(login_state *l, size_t *cap) {
    switch (l->sel) {
    case LOGIN_FIELD_USER: *cap = LOGIN_TEXT_MAX; return l->user;
    case LOGIN_FIELD_PASS: *cap = LOGIN_TEXT_MAX; return l->pass;
    case LOGIN_FIELD_EXTRA:
        if (l->page == LOGIN_PAGE_REGISTER) { *cap = LOGIN_EMAIL_MAX; return l->email; }
        return NULL;
    default: return NULL;
    }
}

void login_type(login_state *l, char c) {
    size_t cap;
    char *f = field(l, &cap);
    if (l->state != LOGIN_EDITING || !f || c < 32 || c > 126) return;
    const size_t n = strlen(f);
    if (n < cap) { f[n] = c; f[n + 1] = '\0'; }
}

void login_backspace(login_state *l) {
    size_t cap;
    char *f = field(l, &cap);
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

void login_register_finish(login_state *l, int ok, const char *msg) {
    l->state = LOGIN_EDITING;
    if (ok) {                                  /* to SIGN IN, the new account filled in */
        l->page = LOGIN_PAGE_SIGNIN;
        l->sel = LOGIN_BTN_OK;
    } else l->sel = LOGIN_FIELD_USER;
    login_set_status(l, msg, !ok);
}

static int shown(const login_state *l, int item) {
    if (l->page == LOGIN_PAGE_REGISTER) return item != LOGIN_BTN_REGISTER;
    return item != LOGIN_FIELD_EXTRA;                                    /* SIGN IN: username, password */
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
        if (ppress & 0x1000) l->osk_shift = !l->osk_shift;               /* Triangle: Shift */
        if (ppress & 0x0008) { l->osk = 0; return LOGIN_ACT_NONE; }      /* Start: done */
        if (press & MI_BACK) { l->osk = 0; return LOGIN_ACT_NONE; }      /* Cross: close the keyboard */
        if ((press & MI_CONFIRM) && !(ppress & 0x0008)) login_type(l, login_osk_char(l->osk_x, l->osk_y, l->osk_shift));
        return LOGIN_ACT_NONE;
    }

    if (press & MI_UP)   do l->sel = (l->sel + LOGIN_ITEMS - 1) % LOGIN_ITEMS; while (!shown(l, l->sel));
    if (press & MI_DOWN) do l->sel = (l->sel + 1) % LOGIN_ITEMS; while (!shown(l, l->sel));
    if ((press & (MI_LEFT | MI_RIGHT)) && l->sel >= LOGIN_BTN_OK) {      /* along the buttons */
        const int step = (press & MI_LEFT) ? -1 : 1;
        do l->sel = LOGIN_BTN_OK + (l->sel - LOGIN_BTN_OK + step + 3) % 3; while (!shown(l, l->sel));
    }
    if (press & MI_BACK) {                                               /* Cross: back out */
        if (l->page == LOGIN_PAGE_REGISTER) { login_set_page(l, LOGIN_PAGE_SIGNIN); return LOGIN_ACT_NONE; }
        return LOGIN_ACT_CANCEL;
    }
    if (press & MI_CONFIRM) {
        switch (l->sel) {
        case LOGIN_FIELD_USER: case LOGIN_FIELD_PASS: case LOGIN_FIELD_EXTRA: l->osk = 1; break;
        case LOGIN_BTN_OK:
            if (l->page == LOGIN_PAGE_REGISTER) {
                if (!l->user[0] || !l->pass[0] || !strchr(l->email, '@')) {
                    login_set_status(l, "Enter a username, a password and your email.", 1);
                    break;
                }
                l->state = LOGIN_WORKING;
                login_set_status(l, "Registering...", 0);
                return LOGIN_ACT_REGISTER;
            }
            if (!l->user[0] || !l->pass[0]) { login_set_status(l, "Enter both a username and a password.", 1); break; }
            l->state = LOGIN_WORKING;
            login_set_status(l, "Connecting to the server...", 0);
            return LOGIN_ACT_SUBMIT;
        case LOGIN_BTN_REGISTER: login_set_page(l, LOGIN_PAGE_REGISTER); break;
        case LOGIN_BTN_CANCEL:
            if (l->page == LOGIN_PAGE_REGISTER) { login_set_page(l, LOGIN_PAGE_SIGNIN); break; }
            return LOGIN_ACT_CANCEL;
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
    const int reg = l->page == LOGIN_PAGE_REGISTER;
    const int nf = reg ? 3 : 2;                                          /* fields */
    const int bw = 340, bh = reg ? 216 : 196, x0 = (img->w - bw) / 2, y0 = (img->h - bh) / 2;
    shade(img, x0, y0, bw, bh);
    frame(img, x0, y0, bw, bh, GREY);
    fill(img, x0 + 1, y0 + 1, bw - 2, 14, BLUE);
    menu_text(img, x0 + 8, y0 + 4, reg ? "PLAYSTATION NETWORK: REGISTER" : "PLAYSTATION NETWORK SIGN-IN", WHITE);
    char line[96];
    snprintf(line, sizeof line, "SERVER: %.44s", l->server);
    menu_text(img, x0 + 8, y0 + 22, line, GREY);

    const int fx = x0 + 96, fw = bw - 108, maxc = (fw - 6) / 6 - 1;
    const char *const LABEL[3] = { "USERNAME", "PASSWORD", "EMAIL" };
    for (int i = 0; i < nf; i++) {
        const int y = y0 + 40 + i * 20;
        const int sel = l->sel == i && l->state == LOGIN_EDITING;
        menu_text(img, x0 + 10, y + 3, LABEL[i], sel ? YELLOW : WHITE);
        frame(img, fx, y, fw, 13, sel ? YELLOW : GREY);
        char shown_text[LOGIN_EMAIL_MAX + 2];
        const char *v = i == 0 ? l->user : i == 2 ? l->email : NULL;
        if (v) snprintf(shown_text, sizeof shown_text, "%s", v);
        else { size_t n = strlen(l->pass); memset(shown_text, '*', n); shown_text[n] = '\0'; }
        if (sel && !l->osk) { size_t n = strlen(shown_text); if (n < LOGIN_EMAIL_MAX + 1) { shown_text[n] = '_'; shown_text[n + 1] = '\0'; } }
        const size_t n = strlen(shown_text);
        menu_text(img, fx + 3, y + 3, n > (size_t)maxc ? shown_text + n - (size_t)maxc : shown_text, WHITE);   /* long: its end */
    }
    /* buttons: SIGN IN / REGISTER / CANCEL, or CREATE / BACK */
    const int by = y0 + 46 + nf * 20;
    const int items[3] = { LOGIN_BTN_OK, LOGIN_BTN_REGISTER, LOGIN_BTN_CANCEL };
    const char *const BTN[3] = { reg ? "CREATE" : "SIGN IN", "REGISTER", reg ? "BACK" : "CANCEL" };
    for (int i = 0, k = 0; i < 3; i++) {
        if (!shown(l, items[i])) continue;
        const int bx = reg ? x0 + 70 + k * 110 : x0 + 20 + k * 105;
        k++;
        const int sel = l->sel == items[i] && l->state == LOGIN_EDITING;
        frame(img, bx, by, 90, 15, sel ? YELLOW : GREY);
        menu_text(img, bx + 45 - menu_text_width(BTN[i]) / 2, by + 4, BTN[i], sel ? YELLOW : WHITE);
    }
    /* status: wrap at the box width */
    const uint32_t sc = l->error ? RED : l->state == LOGIN_DONE_OK ? GREEN : WHITE;
    const int wrap = (bw - 16) / 6;
    const char *s = l->status;
    for (int row = 0; row < 3 && *s; row++) {
        char part[64];
        int n = (int)strlen(s);
        if (n > wrap) { n = wrap; while (n > 0 && s[n] != ' ') n--; if (n == 0) n = wrap; }
        snprintf(part, sizeof part, "%.*s", n, s);
        menu_text(img, x0 + 8, by + 24 + row * 10, part, sc);
        s += n;
        while (*s == ' ') s++;
    }

    if (l->osk) {
        const int kx = x0 + 40, ky = by + 56;
        for (int r = 0; r < OSK_ROWS; r++)
            for (int c = 0; c < OSK_COLS; c++) {
                const int sel = r == l->osk_y && c == l->osk_x;
                char ch[2] = { login_osk_char(c, r, l->osk_shift), 0 };
                if (sel) frame(img, kx + c * 26 - 3, ky + r * 11 - 2, 13, 11, YELLOW);
                menu_text(img, kx + c * 26, ky + r * 11, ch, sel ? YELLOW : WHITE);
            }
        menu_text(img, x0 + 8, ky + OSK_ROWS * 11 + 1,
                  l->osk_shift ? "TRIANGLE: SHIFT (ON)  SQUARE: DELETE  START: DONE"
                               : "TRIANGLE: SHIFT  SQUARE: DELETE  START: DONE", GREY);
    } else {
        menu_text(img, x0 + 8, by + 64, "CIRCLE / ENTER: ACCEPT   CROSS / ESC: BACK", GREY);
        menu_text(img, x0 + 8, by + 74, "KEYBOARD: TYPE INTO THE FIELD, TAB / ARROWS MOVE", GREY);
        menu_text(img, x0 + 8, by + 86, "PAD: CIRCLE ON A FIELD OPENS A KEYBOARD", GREY);
    }
}
