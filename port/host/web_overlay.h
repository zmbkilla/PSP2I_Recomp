/* A web page over the game window (PSP2i's "open website"), through the
 * webview library: WebView2 on Windows, WebKit on Linux / macOS. See
 * web_overlay.cpp. */
#ifndef PSP2I_WEB_OVERLAY_H
#define PSP2I_WEB_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show `url` filling `parent` (an HWND on Windows) until the player closes it
 * (Esc, the page's "Back to the game" bar, Cross / Start / Guide on a
 * controller) or web_overlay_close() is called. Blocks; the parent window's
 * messages keep being handled meanwhile. When the player closed it and
 * `after_close` is set, a "Returning to the game..." screen replaces the page
 * while after_close runs (it should keep pumping the thread's messages), so
 * the hand-over is not a flash of an empty window. Returns 1 if the player
 * closed it, 0 if web_overlay_close() did, -1 when no web engine is available
 * (the page was opened in the default browser instead, if possible). */
int  web_overlay_show(void *parent, const char *url, void (*after_close)(void));

/* The same page over the RUNNING game (a PSP firmware dialog drawn over the
 * game, like the PS Store): returns at once; the game's own message pumping
 * keeps it alive. Poll web_overlay_poll() once per frame: 1 while it is open,
 * 0 once the player closed it (it is then taken down). web_overlay_open
 * returns 0, or -1 without a web engine (the page went to the default
 * browser; nothing to poll). */
int  web_overlay_open(void *parent, const char *url);
int  web_overlay_poll(void);

/* Close the overlay from elsewhere on its thread (e.g. the window's close
 * button); nothing when none is open. */
void web_overlay_close(void);

/* Whether one is open now. */
int  web_overlay_active(void);

/* Fit the open page to its parent again (call on the parent's WM_SIZE). */
void web_overlay_resize(void);

#ifdef __cplusplus
}
#endif

#endif /* PSP2I_WEB_OVERLAY_H */
