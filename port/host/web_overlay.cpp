/* A web page over the game window. See web_overlay.h.
 *
 * The webview library (MIT, cmake/FetchWebView.cmake) wraps each system's own
 * engine; on Windows that is WebView2 (the Edge engine, part of Windows 10/11),
 * loaded by webview's built-in loader, so nothing is shipped with the game.
 * webview_create() with the game window as parent embeds the page as a child
 * window filling it; webview_run() then pumps the thread's messages (the game
 * window's too) until webview_terminate().
 *
 * Closing: a script added to every page draws a "Back to the game" bar and
 * maps Esc; the error page shown for an unreachable site runs no scripts, so
 * Esc and the controller (Cross, Start, Guide) are also polled natively on a
 * timer. Built without the library (offline configure), the page is opened in
 * the default browser instead. */

#include "web_overlay.h"

extern "C" {
#include "input_sdl.h"
}

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef PSP2I_WEBVIEW
#include "webview/webview.h"

static webview_t g_view;
#ifdef _WIN32
static HWND     g_parent;
static LONG_PTR g_style;               /* the game window's style before the overlay */
#endif

void web_overlay_resize(void) {
#ifdef _WIN32
    if (!g_view || !g_parent) return;
    HWND widget = (HWND)webview_get_native_handle(g_view, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET);
    RECT r;
    if (widget && GetClientRect(g_parent, &r))
        SetWindowPos(widget, HWND_TOP, 0, 0, r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW);
#endif
}

static const char CLOSE_JS[] =
    "(function(){if(window.__psp2i)return;window.__psp2i=1;"
    "function close(){if(window.psp2iClose)window.psp2iClose();}"
    "document.addEventListener('keydown',function(e){if(e.key==='Escape'){e.preventDefault();close();}},true);"
    "function bar(){if(document.getElementById('__psp2i_bar'))return;"
    "var b=document.createElement('div');b.id='__psp2i_bar';"
    "b.textContent='\\u2715  Back to the game  (Esc / Cross)';"
    "b.style.cssText='position:fixed;top:0;right:0;z-index:2147483647;padding:6px 12px;"
    "background:rgba(0,0,0,.78);color:#fff;font:14px sans-serif;cursor:pointer;border-bottom-left-radius:6px';"
    "b.onclick=close;(document.body||document.documentElement).appendChild(b);}"
    "if(document.readyState!=='loading')bar();else document.addEventListener('DOMContentLoaded',bar);"
    "})();";

static int g_by_player;                /* closed by the player (not web_overlay_close) */
static int g_modal;                    /* web_overlay_show (its own loop) vs web_overlay_open (the game's) */
static int g_close_req;                /* modeless: closing asked; web_overlay_poll does it */
#ifdef _WIN32
static UINT_PTR g_timer;
#endif

/* Shown while the game starts again: the page fades to this. */
static const char RETURN_HTML[] =
    "<!doctype html><html><head><meta charset='utf-8'><style>"
    "html,body{margin:0;height:100%;background:#000;color:#cfd8e3;font:18px 'Segoe UI',sans-serif;overflow:hidden}"
    "body{display:flex;align-items:center;justify-content:center;animation:in .35s ease-out}"
    "@keyframes in{from{opacity:0}to{opacity:1}}"
    ".b{display:flex;align-items:center;gap:14px;opacity:.9}"
    ".s{width:18px;height:18px;border:2px solid #cfd8e3;border-top-color:transparent;border-radius:50%;"
    "animation:r .9s linear infinite}@keyframes r{to{transform:rotate(360deg)}}"
    "</style></head><body><div class='b'><div class='s'></div>Returning to the game&hellip;</div></body></html>";

/* The player (or the window) wants it closed: the modal loop ends; a
 * modeless page is taken down at the next web_overlay_poll. */
static void request_close(int by_player) {
    if (!g_view) return;
    if (by_player) g_by_player = 1;
    if (g_modal) webview_terminate(g_view);
    else g_close_req = 1;
}

static void on_close(const char *id, const char *req, void *arg) {
    (void)req; (void)arg;
    webview_return(g_view, id, 0, "");
    request_close(1);
}

#ifdef _WIN32
/* The controller as a mouse over the page (the real pointer, so the page
 * sees ordinary mouse input): left stick / D-pad move it, Circle clicks (held:
 * drags), right stick scrolls. Only while the game window is in front. */
static void virtual_mouse(uint32_t b, uint8_t ax, uint8_t ay) {
    enum { P_UP = 0x0010, P_RIGHT = 0x0020, P_DOWN = 0x0040, P_LEFT = 0x0080, P_CIRCLE = 0x2000 };
    static float fx, fy;                    /* sub-pixel remainder */
    static int held;
    if (!g_parent || GetForegroundWindow() != g_parent) return;
    const UINT dpi = GetDpiForWindow(g_parent);
    const float scale = dpi ? (float)dpi / 96.0f : 1.0f;
    float dx = 0, dy = 0;
    const int sx = (int)ax - 128, sy = (int)ay - 128;
    if (sx * sx + sy * sy > 24 * 24) {      /* outside the dead zone: speed grows with the push */
        dx = (float)sx / 127.0f;
        dy = (float)sy / 127.0f;
        const float m = dx * dx + dy * dy;
        dx *= 4.0f + 14.0f * m;
        dy *= 4.0f + 14.0f * m;
    }
    if (b & P_LEFT)  dx -= 6.0f;
    if (b & P_RIGHT) dx += 6.0f;
    if (b & P_UP)    dy -= 6.0f;
    if (b & P_DOWN)  dy += 6.0f;
    if (dx != 0 || dy != 0) {
        POINT p;
        GetCursorPos(&p);
        fx += dx * scale; fy += dy * scale;
        const int mx = (int)fx, my = (int)fy;
        fx -= (float)mx; fy -= (float)my;
        RECT r;                              /* kept inside the game window */
        GetClientRect(g_parent, &r);
        MapWindowPoints(g_parent, NULL, (POINT *)&r, 2);
        p.x += mx; p.y += my;
        if (p.x < r.left) p.x = r.left;
        if (p.x > r.right - 1) p.x = r.right - 1;
        if (p.y < r.top) p.y = r.top;
        if (p.y > r.bottom - 1) p.y = r.bottom - 1;
        SetCursorPos(p.x, p.y);
    }
    const int down = (b & P_CIRCLE) != 0;
    if (down != held) {
        INPUT in;
        memset(&in, 0, sizeof in);
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof in);
        held = down;
    }
    uint8_t rx = 128, ry = 128;
    input_sdl_right_stick(&rx, &ry);
    const int ws = (int)ry - 128;
    if (ws * ws > 32 * 32) {
        INPUT in;
        memset(&in, 0, sizeof in);
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)(-ws * 40 / 127);        /* up = scroll up */
        SendInput(1, &in, sizeof in);
    }
}

/* Each frame: the virtual mouse, and closing -- Esc or Cross / Start /
 * Guide, whatever page (or error page) is showing. */
static void CALLBACK poll_input(HWND, UINT, UINT_PTR, DWORD) {
    static int armed;                       /* only after everything is released (no carry-over press) */
    enum { P_START = 0x0008, P_CROSS = 0x4000 };
    uint32_t b = 0;
    uint8_t ax = 128, ay = 128;
    input_sdl_poll(&b, &ax, &ay);
    virtual_mouse(b, ax, ay);
    const int esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const int down = esc || (b & (P_START | P_CROSS)) || (input_sdl_host_buttons() & INPUT_HOST_GUIDE);
    if (!armed) { if (!down) armed = 1; return; }
    if (down && g_view) { armed = 0; request_close(1); }
}
#endif

/* The page, embedded in `parent` and sized to it; -1 (and the default
 * browser) without a web engine. */
static int create_view(void *parent, const char *url, int modal) {
#ifdef _WIN32
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* WebView2 needs an STA; harmless if already set */
#endif
    g_by_player = 0;
    g_close_req = 0;
    g_modal = modal;
    g_view = webview_create(0, parent);
    if (!g_view) {
        fprintf(stderr, "web: no web engine (WebView2 missing?); opening %s in the default browser\n", url);
#ifdef _WIN32
        ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#endif
        return -1;
    }
    webview_bind(g_view, "psp2iClose", on_close, NULL);
    webview_init(g_view, CLOSE_JS);
    webview_navigate(g_view, url);
#ifdef _WIN32
    /* Embedded, webview creates its widget at 0 x 0 and only follows a window
     * it owns: size it to the game window ourselves (its own WM_SIZE then
     * sizes the page), keep it on top, and keep the game's drawing off it. */
    g_parent = (HWND)parent;
    g_style = GetWindowLongPtrW(g_parent, GWL_STYLE);
    SetWindowLongPtrW(g_parent, GWL_STYLE, g_style | WS_CLIPCHILDREN);
    web_overlay_resize();
    g_timer = SetTimer(NULL, 0, 16, poll_input);
#endif
    fprintf(stderr, "web: showing %s%s\n", url, modal ? "" : " (over the running game)");
    return 0;
}

static void destroy_view(void) {
#ifdef _WIN32
    if (g_timer) KillTimer(NULL, g_timer);
    g_timer = 0;
#endif
    webview_destroy(g_view);
    g_view = NULL;
#ifdef _WIN32
    SetWindowLongPtrW(g_parent, GWL_STYLE, g_style);
    InvalidateRect(g_parent, NULL, FALSE);
    g_parent = NULL;
#endif
    fprintf(stderr, "web: closed%s\n", g_by_player ? " by the player" : "");
}

int web_overlay_show(void *parent, const char *url, void (*after_close)(void)) {
    if (create_view(parent, url, 1) != 0) return -1;
    webview_run(g_view);
    const int by_player = g_by_player;
    if (by_player && after_close) {
        webview_set_html(g_view, RETURN_HTML);
        after_close();                       /* pumps messages: the screen above shows meanwhile */
    }
    destroy_view();
    return by_player;
}

int web_overlay_open(void *parent, const char *url) {
    if (g_view) return 0;
    return create_view(parent, url, 0);
}

int web_overlay_poll(void) {
    if (!g_view || g_modal) return g_view != NULL;
    if (!g_close_req) return 1;
    destroy_view();
    return 0;
}

void web_overlay_close(void) { request_close(0); }
int  web_overlay_active(void) { return g_view != NULL; }

#else  /* built without the webview library */

int web_overlay_show(void *parent, const char *url, void (*after_close)(void)) {
    (void)parent; (void)after_close;
    fprintf(stderr, "web: built without webview; opening %s in the default browser\n", url);
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#endif
    return -1;
}
int web_overlay_open(void *parent, const char *url) { return web_overlay_show(parent, url, NULL); }
int web_overlay_poll(void) { return 0; }
void web_overlay_close(void) {}
int  web_overlay_active(void) { return 0; }
void web_overlay_resize(void) {}

#endif
