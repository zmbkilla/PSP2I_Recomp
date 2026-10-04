/* The PSN sign-in screen: shown when the game asks to sign in
 * (sceUtilityPsnInitStart). Logic and drawing only; online.c does the
 * network work. Circle accepts, Cross goes back, as in the engine menu. */
#ifndef PSP2I_LOGIN_H
#define PSP2I_LOGIN_H

#include <stdint.h>
#include "menu.h"

enum { LOGIN_FIELD_USER, LOGIN_FIELD_PASS, LOGIN_BTN_SIGNIN, LOGIN_BTN_CANCEL, LOGIN_ITEMS };

/* What the screen is doing. */
enum { LOGIN_CLOSED = 0, LOGIN_EDITING, LOGIN_WORKING, LOGIN_DONE_OK, LOGIN_DONE_FAILED, LOGIN_DONE_CANCELLED };

/* What login_update asks the caller to do. */
enum { LOGIN_ACT_NONE = 0, LOGIN_ACT_SUBMIT, LOGIN_ACT_CANCEL, LOGIN_ACT_CLOSE_OK };

#define LOGIN_TEXT_MAX 32
#define OSK_COLS 10
#define OSK_ROWS 4

typedef struct {
    int      state;
    int      sel;                         /* LOGIN_* item */
    char     user[LOGIN_TEXT_MAX + 1];
    char     pass[LOGIN_TEXT_MAX + 1];    /* never drawn, logged or stored */
    char     server[128];                 /* shown, e.g. "bl00d3dg3.xyz (RPCN test server)" */
    char     status[160];                 /* progress / error line */
    int      error;                       /* status is an error */
    int      osk;                         /* the on-screen keyboard is open */
    int      osk_x, osk_y;
    uint32_t prev;
    int      done_timer;                  /* frames to show the result before closing */
} login_state;

void login_open(login_state *l, const char *server, const char *remembered_user);
void login_close(login_state *l);

/* One input sample: held MI_* (menu_inputs_from_psp + host keys) plus
 * PSP-style extra buttons for the keyboard (square = backspace). Returns
 * LOGIN_ACT_*. */
int  login_update(login_state *l, uint32_t held, uint32_t psp_buttons);

/* Keyboard text entry into the selected field (printable ASCII), and
 * Backspace. */
void login_type(login_state *l, char c);
void login_backspace(login_state *l);

/* Progress from the network side. */
void login_set_status(login_state *l, const char *msg, int is_error);
void login_finish(login_state *l, int ok, const char *msg);

void login_draw(const login_state *l, menu_image *img);

/* Characters the on-screen keyboard offers (row-major). */
char login_osk_char(int x, int y);

#endif /* PSP2I_LOGIN_H */
