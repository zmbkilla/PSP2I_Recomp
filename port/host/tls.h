/* A minimal TLS client on the host's own TLS implementation (Schannel on
 * Windows): connect, exchange bytes, close. The server certificate is NOT
 * validated against the system store here -- the caller checks the returned
 * SHA-256 fingerprint (pinning), which is how a self-signed test server is
 * trusted safely. */
#ifndef PSP2I_TLS_H
#define PSP2I_TLS_H

#include <stddef.h>
#include <stdint.h>

typedef struct tls_conn tls_conn;

/* NULL on failure, with a readable reason in err. cert_sha256 receives the
 * server certificate's fingerprint. */
tls_conn *tls_connect(const char *host, int port, int timeout_ms, uint8_t cert_sha256[32], char *err, size_t errcap);
int       tls_send(tls_conn *t, const void *data, size_t len);     /* 0 / -1 */
int       tls_recv(tls_conn *t, void *out, size_t len);            /* exactly len bytes: 0 / -1 */
void      tls_close(tls_conn *t);

#endif /* PSP2I_TLS_H */
