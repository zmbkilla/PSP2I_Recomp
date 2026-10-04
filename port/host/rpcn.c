/* RPCN client: the subset needed to sign in and obtain an auth ticket.
 *
 * RPCN (the PSN replacement written for RPCS3) speaks a binary protocol over
 * TLS on TCP (default port 31313). Every packet starts with a 15-byte
 * little-endian header -- u8 type (0 request, 1 reply, 2 notification,
 * 3 server info), u16 command, u32 total size including the header, u64
 * packet id -- followed by the payload. On connect the server sends a
 * ServerInfo packet whose payload is its protocol version (u32); this client
 * speaks version 27. A reply's payload starts with an error byte (0 = none).
 *
 *   Login (command 0):          user\0 password\0 token\0
 *                                -> online name\0 avatar URL\0 u64 user id ...
 *   RequestTicket (command 27): service id\0 u32 cookie length, cookie bytes
 *                                -> u32 ticket length, ticket bytes
 *
 * Verified against bl00d3dg3.xyz (protocol 27, self-signed certificate) --
 * see port/HANDOFF.md. Passwords are sent only to the configured server, over
 * TLS, and are never logged or stored. */

#include "rpcn.h"
#include "tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR 15
enum { PT_REQUEST = 0, PT_REPLY = 1, PT_NOTIFICATION = 2, PT_SERVER_INFO = 3 };
enum { CMD_LOGIN = 0, CMD_REQUEST_TICKET = 27 };   /* RPCN CommandType: Login 0 ... SendRoomMessage 25, RequestSignalingInfos 26, RequestTicket 27 */

struct rpcn {
    tls_conn *t;
    uint64_t next_id;
    uint32_t server_version;
};

static const char *const RPCN_ERRORS[] = {
    "no error", "malformed request", "invalid request for this stage", "invalid input", "too soon",
    "login error", "already logged in", "invalid username", "invalid password", "invalid token (account not activated)",
    "account creation error", "username exists", "banned email provider", "email already registered",
    "room missing", "room already joined", "room full", "room password mismatch", "room password missing",
    "room group: no join label", "room group full", "room group: join label not found", "room group: slot mismatch",
    "unauthorized", "server database failure", "server email failure", "not found", "blocked", "already friends",
    "score not best", "score invalid", "score has data", "condition failed", "unsupported",
};

const char *rpcn_error_name(int code) {
    return code >= 0 && code < (int)(sizeof RPCN_ERRORS / sizeof RPCN_ERRORS[0]) ? RPCN_ERRORS[code] : "unknown error";
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

/* Read one packet. *payload is malloc'd. */
static int read_packet(rpcn *r, uint8_t *type, uint16_t *cmd, uint64_t *id, uint8_t **payload, uint32_t *len) {
    uint8_t h[HDR];
    if (tls_recv(r->t, h, HDR) != 0) return -1;
    *type = h[0];
    *cmd = (uint16_t)(h[1] | h[2] << 8);
    const uint32_t size = get32(h + 3);
    *id = get64(h + 7);
    if (size < HDR || size > 16u << 20) return -1;
    *len = size - HDR;
    *payload = (uint8_t *)malloc(*len ? *len : 1);
    if (!*payload) return -1;
    if (*len && tls_recv(r->t, *payload, *len) != 0) { free(*payload); return -1; }
    return 0;
}

static int send_request(rpcn *r, uint16_t cmd, const uint8_t *data, uint32_t len, uint64_t *id) {
    uint8_t *p = (uint8_t *)malloc(HDR + len);
    if (!p) return -1;
    *id = ++r->next_id;
    p[0] = PT_REQUEST;
    put16(p + 1, cmd);
    put32(p + 3, HDR + len);
    put64(p + 7, *id);
    if (len) memcpy(p + HDR, data, len);
    const int rc = tls_send(r->t, p, HDR + len);
    free(p);
    return rc;
}

/* The reply to request `id` (notifications in between are skipped). */
static int await_reply(rpcn *r, uint64_t id, uint8_t **payload, uint32_t *len) {
    for (;;) {
        uint8_t type; uint16_t cmd; uint64_t pid;
        if (read_packet(r, &type, &cmd, &pid, payload, len) != 0) return -1;
        if (type == PT_REPLY && pid == id) return 0;
        free(*payload);
    }
}

void rpcn_close(rpcn *r) {
    if (!r) return;
    tls_close(r->t);
    free(r);
}

int rpcn_connect(rpcn **out, const char *host, int port, int timeout_ms, uint8_t cert_sha256[32], char *err, size_t cap) {
    *out = NULL;
    rpcn *r = (rpcn *)calloc(1, sizeof *r);
    if (!r) return RPCN_ERR_CONNECT;
    char terr[160] = "";
    r->t = tls_connect(host, port, timeout_ms, cert_sha256, terr, sizeof terr);
    if (!r->t) { snprintf(err, cap, "%s", terr); free(r); return RPCN_ERR_CONNECT; }
    uint8_t type; uint16_t cmd; uint64_t id; uint8_t *p = NULL; uint32_t len = 0;
    if (read_packet(r, &type, &cmd, &id, &p, &len) != 0) {
        snprintf(err, cap, "the server sent nothing after the TLS handshake (not an RPCN server?)");
        rpcn_close(r);
        return RPCN_ERR_PROTOCOL;
    }
    if (type != PT_SERVER_INFO || len < 4) {
        snprintf(err, cap, "unexpected first packet (type %u, %u bytes): not an RPCN server", type, len);
        free(p);
        rpcn_close(r);
        return RPCN_ERR_PROTOCOL;
    }
    r->server_version = get32(p);
    free(p);
    if (r->server_version != RPCN_PROTOCOL_VERSION) {
        snprintf(err, cap, "server speaks RPCN protocol %u, this client %u", r->server_version, RPCN_PROTOCOL_VERSION);
        rpcn_close(r);
        return RPCN_ERR_PROTOCOL;
    }
    *out = r;
    return RPCN_OK;
}

uint32_t rpcn_server_version(const rpcn *r) { return r ? r->server_version : 0; }

int rpcn_login(rpcn *r, const char *user, const char *password, const char *token,
               char *online_name, size_t name_cap, char *err, size_t cap) {
    const size_t a = strlen(user) + 1, b = strlen(password) + 1, c = strlen(token) + 1;
    uint8_t *d = (uint8_t *)malloc(a + b + c);
    if (!d) return RPCN_ERR_CONNECT;
    memcpy(d, user, a); memcpy(d + a, password, b); memcpy(d + a + b, token, c);
    uint64_t id;
    const int sent = send_request(r, CMD_LOGIN, d, (uint32_t)(a + b + c), &id);
    memset(d, 0, a + b + c);                       /* do not leave the password in freed memory */
    free(d);
    uint8_t *p = NULL; uint32_t len = 0;
    if (sent != 0 || await_reply(r, id, &p, &len) != 0) { snprintf(err, cap, "the connection was lost while signing in"); return RPCN_ERR_CONNECT; }
    if (len < 1) { free(p); snprintf(err, cap, "empty login reply"); return RPCN_ERR_PROTOCOL; }
    if (p[0] != 0) {
        snprintf(err, cap, "sign-in refused: %s", rpcn_error_name(p[0]));
        free(p);
        return RPCN_ERR_REFUSED;
    }
    /* online name\0 ... */
    size_t n = 0;
    while (1 + n < len && p[1 + n]) n++;
    if (online_name && name_cap) {
        const size_t k = n < name_cap - 1 ? n : name_cap - 1;
        memcpy(online_name, p + 1, k);
        online_name[k] = '\0';
    }
    free(p);
    return RPCN_OK;
}

int rpcn_request_ticket(rpcn *r, const char *service_id, const uint8_t *cookie, uint32_t cookie_len,
                        uint8_t **ticket, uint32_t *ticket_len, char *err, size_t cap) {
    *ticket = NULL; *ticket_len = 0;
    const size_t s = strlen(service_id) + 1;
    uint8_t *d = (uint8_t *)malloc(s + 4 + cookie_len);
    if (!d) return RPCN_ERR_CONNECT;
    memcpy(d, service_id, s);
    put32(d + s, cookie_len);
    if (cookie_len) memcpy(d + s + 4, cookie, cookie_len);
    uint64_t id;
    const int sent = send_request(r, CMD_REQUEST_TICKET, d, (uint32_t)(s + 4 + cookie_len), &id);
    free(d);
    uint8_t *p = NULL; uint32_t len = 0;
    if (sent != 0 || await_reply(r, id, &p, &len) != 0) { snprintf(err, cap, "the connection was lost while requesting a ticket"); return RPCN_ERR_CONNECT; }
    if (len < 1) { free(p); snprintf(err, cap, "empty ticket reply"); return RPCN_ERR_PROTOCOL; }
    if (p[0] != 0) {
        snprintf(err, cap, "ticket refused: %s", rpcn_error_name(p[0]));
        free(p);
        return RPCN_ERR_REFUSED;
    }
    /* u32 length + bytes */
    if (len < 5 || get32(p + 1) != len - 5) {
        snprintf(err, cap, "malformed ticket reply (%u bytes)", len);
        free(p);
        return RPCN_ERR_PROTOCOL;
    }
    *ticket_len = len - 5;
    *ticket = (uint8_t *)malloc(*ticket_len);
    if (*ticket) memcpy(*ticket, p + 5, *ticket_len);
    free(p);
    return *ticket ? RPCN_OK : RPCN_ERR_CONNECT;
}
