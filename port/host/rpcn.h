/* RPCN client (sign-in and auth tickets). See rpcn.c. */
#ifndef PSP2I_RPCN_H
#define PSP2I_RPCN_H

#include <stddef.h>
#include <stdint.h>

#define RPCN_PROTOCOL_VERSION 27
#define RPCN_DEFAULT_PORT     31313

enum { RPCN_OK = 0, RPCN_ERR_CONNECT = -1, RPCN_ERR_PROTOCOL = -2, RPCN_ERR_REFUSED = -3 };

typedef struct rpcn rpcn;

/* TLS connect and read the server's ServerInfo (protocol version check).
 * cert_sha256 receives the certificate fingerprint -- the caller pins it. */
int  rpcn_connect(rpcn **out, const char *host, int port, int timeout_ms, uint8_t cert_sha256[32], char *err, size_t cap);
uint32_t rpcn_server_version(const rpcn *r);
int  rpcn_login(rpcn *r, const char *user, const char *password, const char *token,
                char *online_name, size_t name_cap, char *err, size_t cap);
int  rpcn_request_ticket(rpcn *r, const char *service_id, const uint8_t *cookie, uint32_t cookie_len,
                         uint8_t **ticket, uint32_t *ticket_len, char *err, size_t cap);
/* NP matching 2 on a signed-in session; com_id is "NPWRnnnnn_nn". */
int  rpcn_get_server_list(rpcn *r, const char *com_id, uint16_t *ids, int max, int *count, char *err, size_t cap);
int  rpcn_get_world_list(rpcn *r, const char *com_id, uint16_t server_id, uint32_t *ids, int max, int *count, char *err, size_t cap);
void rpcn_close(rpcn *r);
const char *rpcn_error_name(int code);

#endif /* PSP2I_RPCN_H */
