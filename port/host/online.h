/* Online support: configuration, PSN sign-in through an RPCN server, and
 * HTTP request logging. See online.c. */
#ifndef PSP2I_ONLINE_H
#define PSP2I_ONLINE_H

#include "login.h"

void online_init(const char *exe_dir, login_state *login);
void online_poll(void);                      /* once per vblank */
void online_log(const char *fmt, ...);

/* From the sign-in screen's actions. */
int  online_login_submit(void);
void online_login_cancel(void);
void online_login_closed_ok(void);

/* A short status line to show on screen for a few seconds, or NULL. */
const char *online_status_line(void);
const char *online_server(void);

/* The SEGA server redirect (psp2i_online.ini sega_server_redirect): "" = off. */
const char *online_sega_redirect(void);
const char *online_sega_host(void);
void        online_set_sega_redirect(const char *target);

#endif /* PSP2I_ONLINE_H */
