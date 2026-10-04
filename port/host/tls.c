/* A TLS client over a TCP socket, on Windows' own TLS stack (Schannel).
 * See tls.h. */

#include "tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

struct tls_conn {
    SOCKET s;
    CredHandle cred;
    CtxtHandle ctx;
    int have_cred, have_ctx;
    SecPkgContext_StreamSizes sizes;
    uint8_t *in;                /* received, not yet decrypted */
    size_t in_len, in_cap;
    uint8_t *plain;             /* decrypted, not yet handed out */
    size_t plain_len, plain_off;
};

static void set_err(char *err, size_t cap, const char *fmt, long code) {
    if (err && cap) snprintf(err, cap, fmt, code);
}

static int send_all(SOCKET s, const void *p, size_t n) {
    const char *c = (const char *)p;
    while (n) {
        const int k = send(s, c, (int)(n > 65536 ? 65536 : n), 0);
        if (k <= 0) return -1;
        c += k; n -= (size_t)k;
    }
    return 0;
}

static int read_more(tls_conn *t) {
    if (t->in_cap - t->in_len < 16384) {
        const size_t cap = t->in_cap ? t->in_cap * 2 : 65536;
        uint8_t *n = (uint8_t *)realloc(t->in, cap);
        if (!n) return -1;
        t->in = n; t->in_cap = cap;
    }
    const int k = recv(t->s, (char *)t->in + t->in_len, (int)(t->in_cap - t->in_len), 0);
    if (k <= 0) return -1;
    t->in_len += (size_t)k;
    return 0;
}

static SOCKET tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t cap) {
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { set_err(err, cap, "could not resolve the server name (%ld)", WSAGetLastError()); return INVALID_SOCKET; }
    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo *a = res; a && s == INVALID_SOCKET; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, a->ai_addr, (int)a->ai_addrlen);
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(0, NULL, &w, &e, &tv) != 1 || FD_ISSET(s, &e)) { closesocket(s); s = INVALID_SOCKET; continue; }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD to = (DWORD)timeout_ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) set_err(err, cap, "could not connect to the server (timed out or refused)%.0ld", 0);
    return s;
}

void tls_close(tls_conn *t) {
    if (!t) return;
    if (t->have_ctx) DeleteSecurityContext(&t->ctx);
    if (t->have_cred) FreeCredentialsHandle(&t->cred);
    if (t->s != INVALID_SOCKET) closesocket(t->s);
    free(t->in);
    free(t->plain);
    free(t);
}

tls_conn *tls_connect(const char *host, int port, int timeout_ms, uint8_t cert_sha256[32], char *err, size_t errcap) {
    static int wsa;
    if (!wsa) { WSADATA w; wsa = WSAStartup(MAKEWORD(2, 2), &w) == 0; }
    tls_conn *t = (tls_conn *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->s = tcp_connect(host, port, timeout_ms, err, errcap);
    if (t->s == INVALID_SOCKET) { tls_close(t); return NULL; }

    SCHANNEL_CRED sc;
    memset(&sc, 0, sizeof sc);
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    /* The certificate is checked by the caller (pinned fingerprint): the
     * test server's is self-signed, which the system store rejects. */
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    TimeStamp ts;
    if (AcquireCredentialsHandleA(NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &t->cred, &ts) != SEC_E_OK) {
        set_err(err, errcap, "TLS is unavailable on this system (%ld)", (long)GetLastError());
        tls_close(t);
        return NULL;
    }
    t->have_cred = 1;

    const DWORD want = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                       ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    DWORD got = 0;
    SECURITY_STATUS st;
    int first = 1;
    for (;;) {
        SecBuffer inb[2], outb[1];
        SecBufferDesc ind = { SECBUFFER_VERSION, 2, inb }, outd = { SECBUFFER_VERSION, 1, outb };
        inb[0].BufferType = SECBUFFER_TOKEN; inb[0].pvBuffer = t->in; inb[0].cbBuffer = (unsigned long)t->in_len;
        inb[1].BufferType = SECBUFFER_EMPTY; inb[1].pvBuffer = NULL; inb[1].cbBuffer = 0;
        outb[0].BufferType = SECBUFFER_TOKEN; outb[0].pvBuffer = NULL; outb[0].cbBuffer = 0;
        st = InitializeSecurityContextA(&t->cred, first ? NULL : &t->ctx, (SEC_CHAR *)host, want, 0, 0,
                                        first ? NULL : &ind, 0, first ? &t->ctx : NULL, &outd, &got, &ts);
        if (first) t->have_ctx = 1;
        first = 0;
        if (outb[0].cbBuffer && outb[0].pvBuffer) {
            const int bad = send_all(t->s, outb[0].pvBuffer, outb[0].cbBuffer) != 0;
            FreeContextBuffer(outb[0].pvBuffer);
            if (bad) { set_err(err, errcap, "the connection was closed during the TLS handshake%.0ld", 0); tls_close(t); return NULL; }
        }
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            if (read_more(t) != 0) { set_err(err, errcap, "the server closed the connection during the TLS handshake%.0ld", 0); tls_close(t); return NULL; }
            continue;
        }
        /* Keep any bytes past this handshake record. */
        if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer) {
            memmove(t->in, t->in + t->in_len - inb[1].cbBuffer, inb[1].cbBuffer);
            t->in_len = inb[1].cbBuffer;
        } else if (st != SEC_I_CONTINUE_NEEDED || t->in_len) {
            t->in_len = 0;
        }
        if (st == SEC_E_OK) break;
        if (st == SEC_I_CONTINUE_NEEDED) {
            if (t->in_len == 0 && read_more(t) != 0) { set_err(err, errcap, "the server closed the connection during the TLS handshake%.0ld", 0); tls_close(t); return NULL; }
            continue;
        }
        set_err(err, errcap, "TLS handshake failed (0x%08lX)", (long)st);
        tls_close(t);
        return NULL;
    }
    QueryContextAttributesA(&t->ctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes);

    PCCERT_CONTEXT cert = NULL;
    if (QueryContextAttributesA(&t->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert) {
        set_err(err, errcap, "the server sent no certificate%.0ld", 0);
        tls_close(t);
        return NULL;
    }
    DWORD hl = 32;
    const BOOL ok = CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, NULL, cert->pbCertEncoded, cert->cbCertEncoded, cert_sha256, &hl);
    CertFreeCertificateContext(cert);
    if (!ok || hl != 32) { set_err(err, errcap, "could not fingerprint the server certificate%.0ld", 0); tls_close(t); return NULL; }
    return t;
}

int tls_send(tls_conn *t, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    const size_t max = t->sizes.cbMaximumMessage ? t->sizes.cbMaximumMessage : 16384;
    uint8_t *buf = (uint8_t *)malloc(t->sizes.cbHeader + max + t->sizes.cbTrailer);
    if (!buf) return -1;
    while (len) {
        const size_t n = len > max ? max : len;
        memcpy(buf + t->sizes.cbHeader, p, n);
        SecBuffer b[4];
        b[0].BufferType = SECBUFFER_STREAM_HEADER;  b[0].pvBuffer = buf;                                b[0].cbBuffer = t->sizes.cbHeader;
        b[1].BufferType = SECBUFFER_DATA;           b[1].pvBuffer = buf + t->sizes.cbHeader;            b[1].cbBuffer = (unsigned long)n;
        b[2].BufferType = SECBUFFER_STREAM_TRAILER; b[2].pvBuffer = buf + t->sizes.cbHeader + n;        b[2].cbBuffer = t->sizes.cbTrailer;
        b[3].BufferType = SECBUFFER_EMPTY;          b[3].pvBuffer = NULL;                               b[3].cbBuffer = 0;
        SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
        if (EncryptMessage(&t->ctx, 0, &d, 0) != SEC_E_OK ||
            send_all(t->s, buf, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer) != 0) { free(buf); return -1; }
        p += n; len -= n;
    }
    free(buf);
    return 0;
}

/* Decrypt whatever is buffered into the plaintext queue; read the socket
 * when a record is incomplete. Returns -1 on error or close. */
static int fill_plain(tls_conn *t) {
    for (;;) {
        if (t->in_len) {
            SecBuffer b[4];
            b[0].BufferType = SECBUFFER_DATA; b[0].pvBuffer = t->in; b[0].cbBuffer = (unsigned long)t->in_len;
            b[1].BufferType = b[2].BufferType = b[3].BufferType = SECBUFFER_EMPTY;
            b[1].pvBuffer = b[2].pvBuffer = b[3].pvBuffer = NULL;
            b[1].cbBuffer = b[2].cbBuffer = b[3].cbBuffer = 0;
            SecBufferDesc d = { SECBUFFER_VERSION, 4, b };
            const SECURITY_STATUS st = DecryptMessage(&t->ctx, &d, 0, NULL);
            if (st == SEC_E_OK) {
                size_t extra = 0;
                for (int i = 1; i < 4; i++) {
                    if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer) {
                        uint8_t *n = (uint8_t *)realloc(t->plain, t->plain_len + b[i].cbBuffer);
                        if (!n) return -1;
                        t->plain = n;
                        memcpy(t->plain + t->plain_len, b[i].pvBuffer, b[i].cbBuffer);
                        t->plain_len += b[i].cbBuffer;
                    }
                    if (b[i].BufferType == SECBUFFER_EXTRA) extra = b[i].cbBuffer;
                }
                if (extra) memmove(t->in, t->in + t->in_len - extra, extra);
                t->in_len = extra;
                if (t->plain_len > t->plain_off) return 0;
                continue;
            }
            if (st == SEC_I_CONTEXT_EXPIRED) return -1;
            if (st != SEC_E_INCOMPLETE_MESSAGE) return -1;
        }
        if (read_more(t) != 0) return -1;
    }
}

int tls_recv(tls_conn *t, void *out, size_t len) {
    uint8_t *o = (uint8_t *)out;
    size_t got = 0;
    while (got < len) {
        if (t->plain_off >= t->plain_len) {
            t->plain_off = t->plain_len = 0;
            if (fill_plain(t) != 0) return -1;
        }
        size_t n = t->plain_len - t->plain_off;
        if (n > len - got) n = len - got;
        memcpy(o + got, t->plain + t->plain_off, n);
        t->plain_off += n;
        got += n;
    }
    return 0;
}

#else  /* !_WIN32 */
struct tls_conn { int unused; };
tls_conn *tls_connect(const char *host, int port, int timeout_ms, uint8_t sha[32], char *err, size_t cap) {
    (void)host; (void)port; (void)timeout_ms; (void)sha;
    if (err && cap) snprintf(err, cap, "TLS is only implemented on Windows so far");
    return NULL;
}
int  tls_send(tls_conn *t, const void *d, size_t n) { (void)t; (void)d; (void)n; return -1; }
int  tls_recv(tls_conn *t, void *o, size_t n) { (void)t; (void)o; (void)n; return -1; }
void tls_close(tls_conn *t) { (void)t; }
#endif
