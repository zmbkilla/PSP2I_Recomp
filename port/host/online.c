/* Online support for the port: configuration, the PSN sign-in backend (RPCN),
 * and the HTTP transport that logs what the game sends to its servers.
 *
 * Configuration: psp2i_online.ini next to the exe (created with defaults):
 *
 *   rpcn_host=bl00d3dg3.xyz      server for sign-in and tickets (RPCN protocol)
 *   rpcn_port=31313
 *   rpcn_cert_sha256=            pinned certificate fingerprint; empty = trust
 *                                on first use and record it here; a later
 *                                mismatch refuses to connect
 *   rpcn_user=                   remembered username (the password is never
 *                                stored)
 *   http_mode=log                log  : log the game's requests, answer
 *                                       "server unreachable" (default; the
 *                                       game's own servers are offline)
 *                                stub : answer from http_stub_dir if a file
 *                                       exists for the request, else as log
 *                                live : send with Windows HTTP (WinHTTP,
 *                                       system TLS) and log both sides
 *   http_stub_dir=http_stubs     stub files: <dir>/<host>/<path> (query string
 *                                and "/" become "_"); the file's bytes are the
 *                                body, status 200
 *
 *   SEGA server redirect -- the game's own server (offline since 2014) pointed
 *   at a replacement:
 *   sega_server_host=game.psp2infinity.jp,game.revurb.us   the name(s) the
 *                                game uses (comma-separated): the original
 *                                EBOOT's, and the community-patched EBOOT's
 *   sega_server_redirect=        host or host:port of the replacement; empty =
 *                                no redirect. When set, the game's lookups of
 *                                those names resolve to it, and its HTTP(S)
 *                                requests to them are sent to it (WinHTTP,
 *                                system TLS) whatever http_mode says
 *   sega_server_scheme=          empty = the game's own scheme; or http / https
 *   sega_server_ignore_cert=0    1 = accept the replacement's certificate even
 *                                if self-signed or for another name (test
 *                                servers only)
 *
 * Logs: online_log.txt next to the exe -- network events, sign-in progress
 * (never the password), and every HTTP request with the game's handling of
 * the response. */

#include "online.h"
#include "rpcn.h"

#include <psprecomp/net.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#endif

/* ---- configuration ---------------------------------------------------------------- */

static char g_dir[600];
static char g_host[128] = "bl00d3dg3.xyz";
static int  g_port = RPCN_DEFAULT_PORT;
static char g_pin[65];
static char g_user[64];
static char g_http_mode[16] = "log";
static char g_stub_dir[256] = "http_stubs";
/* The game's server names: the original EBOOT uses game.psp2infinity.jp; the
 * community-patched EBOOT (as in FinalBuild) uses game.revurb.us. */
#define SEGA_HOSTS_DEFAULT "game.psp2infinity.jp,game.revurb.us"
static char g_sega_host[256] = SEGA_HOSTS_DEFAULT;
static char g_sega_redirect[256];
static char g_sega_scheme[16];
static int  g_sega_ignore_cert;
static int  g_in_redirect;               /* live_send is sending a redirected request */
static FILE *g_log;

static void cfg_path(char *out, size_t cap) { snprintf(out, cap, "%s/psp2i_online.ini", g_dir); }

static void cfg_save(void) {
    char p[700];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "; psp2i online settings (see port/HANDOFF.md). The password is never stored.\n");
    fprintf(f, "rpcn_host=%s\nrpcn_port=%d\nrpcn_cert_sha256=%s\nrpcn_user=%s\nhttp_mode=%s\nhttp_stub_dir=%s\n",
            g_host, g_port, g_pin, g_user, g_http_mode, g_stub_dir);
    fprintf(f, "; SEGA server redirect: the game's server name(s) and where to send them instead\n"
               "; (host or host:port; empty = no redirect). See port/HANDOFF.md.\n");
    fprintf(f, "sega_server_host=%s\nsega_server_redirect=%s\nsega_server_scheme=%s\nsega_server_ignore_cert=%d\n",
            g_sega_host, g_sega_redirect, g_sega_scheme, g_sega_ignore_cert);
    fclose(f);
}

static void cfg_load(void) {
    char p[700], line[512];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) { cfg_save(); return; }
    int seen = 0;
    while (fgets(line, sizeof line, f)) {
        char *e = strchr(line, '=');
        if (!e || line[0] == ';' || line[0] == '#') continue;
        *e++ = '\0';
        e[strcspn(e, "\r\n")] = '\0';
        if (!strcmp(line, "rpcn_host")) { snprintf(g_host, sizeof g_host, "%s", e); seen++; continue; }
        if (!strcmp(line, "rpcn_port")) { g_port = atoi(e) > 0 ? atoi(e) : RPCN_DEFAULT_PORT; seen++; continue; }
        if (!strcmp(line, "rpcn_cert_sha256")) { snprintf(g_pin, sizeof g_pin, "%s", e); seen++; continue; }
        if (!strcmp(line, "rpcn_user")) { snprintf(g_user, sizeof g_user, "%s", e); seen++; continue; }
        if (!strcmp(line, "http_mode")) snprintf(g_http_mode, sizeof g_http_mode, "%s", e);
        else if (!strcmp(line, "http_stub_dir")) snprintf(g_stub_dir, sizeof g_stub_dir, "%s", e);
        else if (!strcmp(line, "sega_server_host")) snprintf(g_sega_host, sizeof g_sega_host, "%s", e);
        else if (!strcmp(line, "sega_server_redirect")) snprintf(g_sega_redirect, sizeof g_sega_redirect, "%s", e);
        else if (!strcmp(line, "sega_server_scheme")) snprintf(g_sega_scheme, sizeof g_sega_scheme, "%s", e);
        else if (!strcmp(line, "sega_server_ignore_cert")) g_sega_ignore_cert = atoi(e) != 0;
        else continue;
        seen++;
    }
    fclose(f);
    /* An ini written with the earlier single-name default: add the patched
     * EBOOT's name, or a redirect would miss it. */
    if (!strcmp(g_sega_host, "game.psp2infinity.jp")) { snprintf(g_sega_host, sizeof g_sega_host, "%s", SEGA_HOSTS_DEFAULT); seen = 0; }
    if (seen < 10) cfg_save();          /* add any keys a older file lacks */
}

/* ---- log --------------------------------------------------------------------------- */

void online_log(const char *fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "online: %s\n", line);
    if (!g_log) {
        char p[700];
        snprintf(p, sizeof p, "%s/online_log.txt", g_dir);
        g_log = fopen(p, "a");
        if (g_log) {
            time_t t = time(NULL);
            fprintf(g_log, "\n==== session %s", ctime(&t));
        }
    }
    if (g_log) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        fprintf(g_log, "[%02d:%02d:%02d] %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, line);
        fflush(g_log);
    }
}

static void net_line(const char *line) { online_log("net: %s", line); }

/* ---- status shown on screen -------------------------------------------------------- */

static char g_status[160];
static int  g_status_frames;
static void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    va_end(ap);
    g_status_frames = 60 * 6;
    online_log("status: %s", g_status);
}
const char *online_status_line(void) { return g_status_frames > 0 ? g_status : NULL; }

/* ---- the worker: one network job at a time --------------------------------------------- */

#ifdef _WIN32
static CRITICAL_SECTION g_lock;
static CRITICAL_SECTION g_rpcn_lock;     /* one request at a time on the session */
#define LOCK()   EnterCriticalSection(&g_lock)
#define UNLOCK() LeaveCriticalSection(&g_lock)
#define RPCN_LOCK()   EnterCriticalSection(&g_rpcn_lock)
#define RPCN_UNLOCK() LeaveCriticalSection(&g_rpcn_lock)
#else
#define LOCK()
#define UNLOCK()
#define RPCN_LOCK()
#define RPCN_UNLOCK()
#endif

enum { JOB_NONE, JOB_SIGNIN, JOB_TICKET };
static struct {
    int  kind, busy, done, ok;
    char user[64], pass[64];
    int  slot;
    char service[64];
    uint8_t cookie[256];
    uint32_t cookie_len;
    char msg[200];
} g_job;

static rpcn *g_session;                  /* signed in */
static char  g_online_name[64];
static int   g_signin_state = PSP_NP_SIGNIN_NONE;
static struct { int state; uint8_t *data; uint32_t len; uint32_t err; } g_tickets[4];

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static int pin_matches(const uint8_t fp[32]) {
    if (strlen(g_pin) != 64) return -1;                       /* nothing pinned */
    for (int i = 0; i < 32; i++)
        if (((hexval(g_pin[2 * i]) << 4) | hexval(g_pin[2 * i + 1])) != fp[i]) return 0;
    return 1;
}

static void run_signin(void) {
    char err[200] = "";
    uint8_t fp[32];
    rpcn *r = NULL;
    online_log("signing in: connecting to %s:%d", g_host, g_port);
    int rc = rpcn_connect(&r, g_host, g_port, 8000, fp, err, sizeof err);
    if (rc != RPCN_OK) {
        snprintf(g_job.msg, sizeof g_job.msg, rc == RPCN_ERR_PROTOCOL ? "Incompatible server: %s" : "Could not reach the server: %s", err);
        return;
    }
    char fphex[65];
    for (int i = 0; i < 32; i++) snprintf(fphex + 2 * i, 3, "%02x", fp[i]);
    const int pm = pin_matches(fp);
    if (pm == 0) {
        rpcn_close(r);
        snprintf(g_job.msg, sizeof g_job.msg, "Server certificate changed (expected %.16s..., got %.16s...). Not connecting.", g_pin, fphex);
        return;
    }
    if (pm < 0) {
        snprintf(g_pin, sizeof g_pin, "%s", fphex);           /* trust on first use */
        online_log("pinned the server certificate (first use): sha256 %s", fphex);
    }
    online_log("connected: TLS, RPCN protocol %u, certificate sha256 %s", rpcn_server_version(r), fphex);
    char name[64] = "";
    rc = rpcn_login(r, g_job.user, g_job.pass, "", name, sizeof name, err, sizeof err);
    memset(g_job.pass, 0, sizeof g_job.pass);
    if (rc != RPCN_OK) {
        rpcn_close(r);
        snprintf(g_job.msg, sizeof g_job.msg, "Connected to the server, but %s.", err);
        online_log("sign-in failed: %s", err);
        return;
    }
    LOCK();
    if (g_session) rpcn_close(g_session);
    g_session = r;
    snprintf(g_online_name, sizeof g_online_name, "%s", name[0] ? name : g_job.user);
    snprintf(g_user, sizeof g_user, "%s", g_job.user);
    UNLOCK();
    cfg_save();
    g_job.ok = 1;
    snprintf(g_job.msg, sizeof g_job.msg, "Signed in to %s as %s (RPCN test account, not PSN).", g_host, g_online_name);
    online_log("signed in as %s", g_online_name);
}

static void run_ticket(void) {
    char err[200] = "";
    uint8_t *t = NULL;
    uint32_t n = 0;
    const int slot = g_job.slot;
    if (!g_session) { snprintf(g_job.msg, sizeof g_job.msg, "not signed in"); return; }
    online_log("requesting an auth ticket for service %s", g_job.service);
    RPCN_LOCK();
    const int rc = rpcn_request_ticket(g_session, g_job.service, g_job.cookie, g_job.cookie_len, &t, &n, err, sizeof err);
    RPCN_UNLOCK();
    LOCK();
    if (rc == RPCN_OK) {
        free(g_tickets[slot].data);
        g_tickets[slot].data = t; g_tickets[slot].len = n; g_tickets[slot].state = 1;
        g_job.ok = 1;
        snprintf(g_job.msg, sizeof g_job.msg, "Auth ticket received from %s (%u bytes)", g_host, n);
    } else {
        g_tickets[slot].state = -1;
        g_tickets[slot].err = 0x80550401u;      /* SCE_NP_AUTH_ERROR_SERVICE_DOWN */
        snprintf(g_job.msg, sizeof g_job.msg, "Ticket request failed: %s", err);
    }
    UNLOCK();
    online_log("%s", g_job.msg);
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    if (g_job.kind == JOB_SIGNIN) run_signin();
    else if (g_job.kind == JOB_TICKET) run_ticket();
    LOCK();
    g_job.busy = 0;
    g_job.done = 1;
    UNLOCK();
    return 0;
}
#endif

static int start_job(int kind) {
#ifdef _WIN32
    if (g_job.busy) return -1;
    g_job.kind = kind; g_job.busy = 1; g_job.done = 0; g_job.ok = 0; g_job.msg[0] = '\0';
    HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!h) { g_job.busy = 0; return -1; }
    CloseHandle(h);
    return 0;
#else
    (void)kind;
    return -1;
#endif
}

/* ---- the sign-in screen and the NP backend ---------------------------------------------- */

static login_state *g_login;

int online_login_submit(void) {
    if (!g_login) return -1;
    snprintf(g_job.user, sizeof g_job.user, "%s", g_login->user);
    snprintf(g_job.pass, sizeof g_job.pass, "%s", g_login->pass);
    memset(g_login->pass, 0, sizeof g_login->pass);
    online_log("sign-in submitted for user %s (password not logged)", g_job.user);
    if (start_job(JOB_SIGNIN) != 0) { login_finish(g_login, 0, "Busy -- try again."); return -1; }
    return 0;
}

void online_login_cancel(void) {
    online_log("sign-in cancelled by the user");
    g_signin_state = PSP_NP_SIGNIN_CANCELLED;
    if (g_login) login_close(g_login);
}

void online_login_closed_ok(void) {
    g_signin_state = PSP_NP_SIGNIN_OK;
    status("Signed in to %s as %s (RPCN, not PSN)", g_host, g_online_name);
    if (g_login) login_close(g_login);
}

static void be_signin_begin(void) {
    online_log("game requested PSN sign-in: showing the sign-in screen (server %s:%d)", g_host, g_port);
    if (g_session) {                      /* already signed in this session */
        g_signin_state = PSP_NP_SIGNIN_OK;
        online_log("already signed in as %s", g_online_name);
        return;
    }
    g_signin_state = PSP_NP_SIGNIN_BUSY;
    char server[128];
    snprintf(server, sizeof server, "%s (RPCN TEST SERVER)", g_host);
    if (g_login) login_open(g_login, server, g_user);
}
static int  be_signin_state(void) { return g_signin_state; }
static void be_signin_cancel(void) { if (g_login && g_login->state != LOGIN_CLOSED) online_login_cancel(); }
static const char *be_online_id(void) { return g_online_name; }

static void be_ticket_begin(int slot, const char *service, const uint8_t *cookie, uint32_t cookie_len) {
    LOCK();
    free(g_tickets[slot].data);
    memset(&g_tickets[slot], 0, sizeof g_tickets[slot]);
    UNLOCK();
    g_job.slot = slot;
    snprintf(g_job.service, sizeof g_job.service, "%s", service);
    g_job.cookie_len = cookie_len > sizeof g_job.cookie ? (uint32_t)sizeof g_job.cookie : cookie_len;
    if (g_job.cookie_len) memcpy(g_job.cookie, cookie, g_job.cookie_len);
    status("Requesting an auth ticket for %s from %s...", service, g_host);
    if (start_job(JOB_TICKET) != 0) { g_tickets[slot].state = -1; g_tickets[slot].err = 0x80550306u; }
}

static int be_ticket_state(int slot, const uint8_t **data, uint32_t *len, uint32_t *err) {
    LOCK();
    const int s = g_tickets[slot].state;
    if (s > 0) { *data = g_tickets[slot].data; *len = g_tickets[slot].len; }
    if (s < 0) *err = g_tickets[slot].err;
    UNLOCK();
    return s;
}
static void be_ticket_cancel(int slot) {
    LOCK();
    free(g_tickets[slot].data);
    memset(&g_tickets[slot], 0, sizeof g_tickets[slot]);
    UNLOCK();
}

/* NP matching 2: server and world lists from RPCN, on the signed-in session.
 * Called from the game's thread and blocking for one round trip. */
#define M2_SERVER_ERROR_SERVICE_UNAVAILABLE 0x80550D02u
#define M2_ERROR_TIMEDOUT                   0x80550C3Cu

static int be_m2_server_list(const char *com_id, uint16_t *ids, int max, uint32_t *err) {
    char e[200] = "";
    int n = 0, rc = RPCN_ERR_CONNECT;
    online_log("matching: RPCN GetServerList for %s on %s", com_id, g_host);
    RPCN_LOCK();
    if (g_session) {
        rc = rpcn_get_server_list(g_session, com_id, ids, max, &n, e, sizeof e);
        /* a server may create a game's entries on first request: ask once more */
        if (rc == RPCN_OK && n == 0) rc = rpcn_get_server_list(g_session, com_id, ids, max, &n, e, sizeof e);
    } else snprintf(e, sizeof e, "not signed in");
    RPCN_UNLOCK();
    if (rc != RPCN_OK) {
        online_log("matching: GetServerList failed: %s", e);
        status("Matching server list failed: %s", e);
        *err = rc == RPCN_ERR_CONNECT && g_session ? M2_ERROR_TIMEDOUT : M2_SERVER_ERROR_SERVICE_UNAVAILABLE;
        return -1;
    }
    char list[128] = "";
    for (int i = 0; i < n; i++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%u", i ? "," : "", ids[i]);
    online_log("matching: %s has %d server(s) on %s%s%s", com_id, n, g_host, n ? ": " : "", list);
    if (!n) status("RPCN %s has no matching servers for %s", g_host, com_id);
    return n;
}

static int be_m2_world_list(const char *com_id, uint16_t server_id, uint32_t *ids, int max, uint32_t *err) {
    char e[200] = "";
    int n = 0, rc = RPCN_ERR_CONNECT;
    online_log("matching: RPCN GetWorldList for %s, server %u", com_id, server_id);
    RPCN_LOCK();
    if (g_session) {
        rc = rpcn_get_world_list(g_session, com_id, server_id, ids, max, &n, e, sizeof e);
        if (rc == RPCN_OK && n == 0) rc = rpcn_get_world_list(g_session, com_id, server_id, ids, max, &n, e, sizeof e);
    } else snprintf(e, sizeof e, "not signed in");
    RPCN_UNLOCK();
    if (rc != RPCN_OK) {
        online_log("matching: GetWorldList failed: %s", e);
        status("Matching world list failed: %s", e);
        *err = rc == RPCN_ERR_CONNECT && g_session ? M2_ERROR_TIMEDOUT : M2_SERVER_ERROR_SERVICE_UNAVAILABLE;
        return -1;
    }
    char list[160] = "";
    for (int i = 0; i < n; i++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%u", i ? "," : "", ids[i]);
    online_log("matching: server %u has %d world(s)%s%s", server_id, n, n ? ": " : "", list);
    if (!n) status("RPCN %s has no worlds for %s server %u", g_host, com_id, server_id);
    return n;
}

#ifdef _WIN32
int game_stack(char *out, size_t cap, int max_frames);     /* main.c */
#endif

/* NP library log lines; a matching call also names where in the game it came from. */
static void np_line(const char *line) {
#ifdef _WIN32
    if (!strncmp(line, "np: m2 ", 7) && strncmp(line, "np: m2 ->", 9) && strncmp(line, "np: m2   ", 9)) {
        char where[512];
        if (game_stack(where, sizeof where, 12) > 0) { online_log("%s   [game: %s]", line, where); return; }
    }
#endif
    online_log("%s", line);
}

static const psp_np_backend NP_BACKEND = {
    be_signin_begin, be_signin_state, be_signin_cancel, be_online_id,
    be_ticket_begin, be_ticket_state, be_ticket_cancel,
    be_m2_server_list, be_m2_world_list, np_line,
};

/* Once per vblank: hand finished jobs to the screen and the status line. */
void online_poll(void) {
    if (g_status_frames > 0) g_status_frames--;
    int done = 0, ok = 0, kind = 0;
    char msg[200];
    LOCK();
    if (g_job.done) { done = 1; ok = g_job.ok; kind = g_job.kind; snprintf(msg, sizeof msg, "%s", g_job.msg); g_job.done = 0; }
    UNLOCK();
    if (done && kind == JOB_SIGNIN && g_login) {
        login_finish(g_login, ok, msg);
        if (!ok) online_log("sign-in failed: %s", msg);
    }
    if (done && kind == JOB_TICKET) status("%s", msg);
    psp_np_poll();
}

/* ---- the SEGA server redirect -------------------------------------------------------------- */

/* Is `host` one of the game's server names (sega_server_host, comma-separated)? */
static int is_sega_host(const char *host) {
    if (!host || !host[0]) return 0;
    const char *p = g_sega_host;
    while (*p) {
        const char *e = strchr(p, ',');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && strlen(host) == n && !_strnicmp(host, p, n)) return 1;
        if (!e) break;
        p = e + 1;
        while (*p == ' ') p++;
    }
    return 0;
}

/* sega_server_redirect split into host and port (0 = keep the game's). */
static void redirect_target(char *host, size_t cap, uint32_t *port) {
    snprintf(host, cap, "%s", g_sega_redirect);
    *port = 0;
    char *c = strrchr(host, ':');
    if (c && !strchr(c + 1, ']')) { *port = (uint32_t)atoi(c + 1); *c = '\0'; }
}

/* The redirect target's IPv4 address as text: the game resolves the server
 * name (to this, through resolve_redirect) and then connects by address. */
static int is_redirect_address(const char *host) {
    static char cached_for[256], addr[64];
    if (!g_sega_redirect[0] || !host || !host[0]) return 0;
    char target[256];
    uint32_t port;
    redirect_target(target, sizeof target, &port);
    if (!_stricmp(host, target)) return 1;
    if (strcmp(cached_for, target)) {
        snprintf(cached_for, sizeof cached_for, "%s", target);
        addr[0] = '\0';
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        if (getaddrinfo(target, NULL, &hints, &res) == 0 && res) {
            const unsigned char *b = (const unsigned char *)&((struct sockaddr_in *)res->ai_addr)->sin_addr;
            snprintf(addr, sizeof addr, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            freeaddrinfo(res);
        }
    }
    return addr[0] && !strcmp(host, addr);
}

static const char *resolve_redirect(const char *host) {
    static char target[256];
    if (!g_sega_redirect[0] || !is_sega_host(host)) return NULL;
    uint32_t port;
    redirect_target(target, sizeof target, &port);
    return target;
}

/* ---- HTTP: log, stub or live ---------------------------------------------------------------- */

/* HTTP log lines about a request also say where in the game the call came
 * from: the chain of game functions (PSP addresses, innermost first). */
static void http_line(const char *line) {
#ifdef _WIN32
    if (!strncmp(line, "request ", 8) || !strncmp(line, "connection ", 11)) {
        char where[512];
        if (game_stack(where, sizeof where, 16) > 0) { online_log("http: %s   [game: %s]", line, where); return; }
    }
#endif
    online_log("http: %s", line);
}

static void log_body(const char *what, const uint8_t *p, uint32_t n) {
    if (!n) { online_log("http:   %s: (empty)", what); return; }
    online_log("http:   %s: %u bytes", what, n);
    char hex[16 * 3 + 1], asc[17];
    for (uint32_t i = 0; i < n && i < 4096; i += 16) {
        int k = 0;
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < n) { snprintf(hex + 3 * j, 4, "%02x ", p[i + j]); asc[k++] = p[i + j] >= 32 && p[i + j] < 127 ? (char)p[i + j] : '.'; }
            else memcpy(hex + 3 * j, "   ", 4);
        }
        asc[k] = '\0';
        online_log("http:     %04x  %s %s", i, hex, asc);
    }
    if (n > 4096) online_log("http:     ... (%u more bytes)", n - 4096);
}

static int stub_answer(const psp_http_request *q, psp_http_response *r) {
    char rel[1024], path[1700];
    snprintf(rel, sizeof rel, "%s", q->path[0] == '/' ? q->path + 1 : q->path);
    for (char *c = rel; *c; c++) if (*c == '/' || *c == '?' || *c == '&' || *c == '=' || *c == ':') *c = '_';
    snprintf(path, sizeof path, "%s/%s/%s/%s", g_dir, g_stub_dir, q->host, rel[0] ? rel : "_index");
    FILE *f = fopen(path, "rb");
    if (!f) { online_log("http:   no stub at %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    r->body = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    r->len = r->body ? (uint32_t)fread(r->body, 1, (size_t)n, f) : 0;
    fclose(f);
    r->status = 200;
    online_log("http:   answered from stub %s", path);
    return 0;
}

#ifdef _WIN32
static int live_send(const psp_http_request *q, psp_http_response *r) {
    wchar_t host[256], path[1024], ua[256], verb[8];
    MultiByteToWideChar(CP_UTF8, 0, q->host, -1, host, 256);
    MultiByteToWideChar(CP_UTF8, 0, q->path[0] ? q->path : "/", -1, path, 1024);
    MultiByteToWideChar(CP_UTF8, 0, q->user_agent, -1, ua, 256);
    MultiByteToWideChar(CP_UTF8, 0, q->method, -1, verb, 8);
    const int https = !strcmp(q->scheme, "https");
    HINTERNET s = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, (INTERNET_PORT)q->port, 0) : NULL;
    HINTERNET h = c ? WinHttpOpenRequest(c, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0) : NULL;
    int rc = (int)0x80431063u;
    if (h && https && g_sega_ignore_cert && g_in_redirect) {
        DWORD fl = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(h, WINHTTP_OPTION_SECURITY_FLAGS, &fl, sizeof fl);
    }
    if (h && WinHttpSendRequest(h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)q->body, q->body_len, q->body_len, 0) &&
        WinHttpReceiveResponse(h, NULL)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        r->status = (int)code;
        for (;;) {
            DWORD avail = 0, got = 0;
            if (!WinHttpQueryDataAvailable(h, &avail) || !avail) break;
            uint8_t *nb = (uint8_t *)realloc(r->body, r->len + avail);
            if (!nb) break;
            r->body = nb;
            if (!WinHttpReadData(h, r->body + r->len, avail, &got) || !got) break;
            r->len += got;
        }
        rc = 0;
    } else {
        online_log("http:   live request failed (WinHTTP error %lu)", (unsigned long)GetLastError());
    }
    if (h) WinHttpCloseHandle(h);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return rc;
}
#endif

static int http_send(const psp_http_request *q, psp_http_response *r) {
    online_log("http: ---- %s %s://%s:%u%s", q->method, q->scheme, q->host, q->port, q->path);
    online_log("http:   User-Agent: %s", q->user_agent);
    log_body("request body", q->body, q->body_len);
    int rc = -1;
    if (g_sega_redirect[0] && (is_sega_host(q->host) || is_redirect_address(q->host))) {
        /* The game's own server, pointed at the replacement in the settings file. */
        char host[256];
        uint32_t port;
        redirect_target(host, sizeof host, &port);
        psp_http_request rq = *q;
        rq.host = host;
        if (port) rq.port = port;
        if (g_sega_scheme[0]) {
            rq.scheme = g_sega_scheme;
            if (!port) rq.port = !strcmp(g_sega_scheme, "https") ? 443 : 80;
        }
        online_log("http:   SEGA server redirect: sending to %s://%s:%u%s", rq.scheme, rq.host, rq.port, rq.path);
        status("Redirecting the game's server request to %s", host);
#ifdef _WIN32
        g_in_redirect = 1;
        rc = live_send(&rq, r);
        g_in_redirect = 0;
#endif
        if (rc != 0) {
            online_log("http:   the redirect target did not answer: reporting the server unreachable");
            return rc ? rc : (int)0x80431063u;
        }
        online_log("http:   response status %d", r->status);
        log_body("response body", r->body, r->len);
        return 0;
    }
    if (!strcmp(g_http_mode, "stub")) rc = stub_answer(q, r);
#ifdef _WIN32
    else if (!strcmp(g_http_mode, "live")) rc = live_send(q, r);
#endif
    if (rc != 0) {
        online_log("http:   not sent (http_mode=%s): reporting the server unreachable (SCE_HTTP_ERROR_NETWORK)", g_http_mode);
        status("The game contacted %s -- logged in online_log.txt (servers offline)", q->host);
        return (int)0x80431063u;
    }
    online_log("http:   response status %d", r->status);
    log_body("response body", r->body, r->len);
    return 0;
}

static const psp_http_transport HTTP_TRANSPORT = { http_send, http_line };

/* ---- setup ------------------------------------------------------------------------------- */

void online_init(const char *exe_dir, login_state *login) {
    snprintf(g_dir, sizeof g_dir, "%s", exe_dir);
#ifdef _WIN32
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_rpcn_lock);
#endif
    cfg_load();
    g_login = login;
    psp_net_set_log(net_line);
    psp_net_set_resolve_redirect(resolve_redirect);
    psp_http_set_transport(&HTTP_TRANSPORT);
    psp_np_set_backend(&NP_BACKEND);
    online_log("online: sign-in server %s:%d, http_mode=%s", g_host, g_port, g_http_mode);
    if (g_sega_redirect[0]) online_log("online: SEGA server %s redirected to %s", g_sega_host, g_sega_redirect);
    else online_log("online: SEGA server %s not redirected (sega_server_redirect is empty)", g_sega_host);
}

const char *online_server(void) { return g_host; }

const char *online_sega_redirect(void) { return g_sega_redirect; }
const char *online_sega_host(void) { return g_sega_host; }

/* From the settings menu: set (or clear, with "") the redirect and save it. */
void online_set_sega_redirect(const char *target) {
    char t[256];
    snprintf(t, sizeof t, "%s", target ? target : "");
    /* trim spaces */
    char *b = t; while (*b == ' ') b++;
    size_t n = strlen(b); while (n && b[n - 1] == ' ') b[--n] = '\0';
    snprintf(g_sega_redirect, sizeof g_sega_redirect, "%s", b);
    cfg_save();
    if (g_sega_redirect[0]) online_log("settings: SEGA server %s now redirected to %s (saved)", g_sega_host, g_sega_redirect);
    else online_log("settings: SEGA server redirect cleared (saved)");
}
