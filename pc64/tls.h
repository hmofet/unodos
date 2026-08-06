/* ===========================================================================
 * UnoDOS/pc64 - TLS client (BearSSL) over the pc64 TCP stack.
 *
 * Security is BearSSL's (constant-time, audited portable C) - we roll none of
 * our own crypto. Trust is either a PINNED server public key (br_x509_knownkey,
 * no CA store and no system clock needed - a strong, simple model for a fixed
 * endpoint) or the bundled CA roots, for HTTPS to arbitrary hosts.
 *
 * Entropy FAILS CLOSED. RDRAND when the CPU has a working one; otherwise
 * conditioned CPU timing jitter, and only if it passes a health test. When
 * neither qualifies both connects refuse with TLS_ENOENTROPY rather than
 * handshake on a seed we cannot defend (this used to be a silent TSC-LCG).
 *
 * TWO SURFACES, one engine:
 *
 *   tls_conn *   [PREFERRED]  a HANDLE. Each handle owns its own netsock
 *                socket, BearSSL engine and record buffer, so several TLS
 *                sessions exist at once, and every call is NON-BLOCKING: you
 *                pump them from wherever you already call net_poll(), and one
 *                slow peer cannot stall the others. This is what the browser's
 *                parallel fetch is built on.
 *
 *   tls_connect  [LEGACY, unchanged] the original module-scoped, blocking API.
 *   tls_write    Still exactly one session, still exported to .UNO modules,
 *   tls_read     still what apps/network.c and apps/studio_ai.c use. It is now
 *   tls_close    a thin blocking wrapper over one internally-held handle, so
 *                its semantics, deadlines and error codes are as they were.
 *                Mixing it with handles is fine; it is just one more session.
 * ======================================================================== */
#ifndef PC64_TLS_H
#define PC64_TLS_H
#include "net.h"
#include "tls_entropy.h"   /* TLS_ENT_*, tls_entropy_source/name/get/selftest */

#define TLS_ENOENTROPY (-4)         /* tls_connect*: refused, no usable RNG   */

/* ---- handles (non-blocking, many at once) -------------------------------- */

typedef struct tls_conn tls_conn;

enum {                              /* what to trust the peer's certificate on */
    TLS_TRUST_PINNED = 0,           /* the built-in pinned P-256 key           */
    TLS_TRUST_CA     = 1            /* the bundled roots + SNI + validity date  */
};

enum {                              /* tls_poll() */
    TLS_PENDING = 0,                /* connecting or handshaking; call again    */
    TLS_READY   = 1,                /* handshake done; send/recv are live       */
    TLS_EOF     = 2                 /* peer closed; drain recv, then tls_free   */
};

/* Start a session to dst:port. Returns a handle immediately - the TCP connect
 * and the handshake have NOT happened yet; drive them with tls_poll(). NULL if
 * the machine has no usable entropy (TLS_ENOENTROPY), no socket, or no memory;
 * tls_open_error() says which. `sni` is the server name (also the CA-validated
 * name when trust is TLS_TRUST_CA) and is copied. */
tls_conn *tls_open(const u8 dst[4], u16 port, const char *sni, int trust);
int tls_open_error(void);           /* why the last tls_open() returned NULL   */

/* Advance the session. Non-blocking: it moves whatever bytes the socket will
 * give or take right now and returns. Returns TLS_PENDING / TLS_READY /
 * TLS_EOF, or <0 on failure (tls_conn_error() carries BearSSL's BR_ERR_*).
 * Call net_poll() yourself; this does not, so one pump can drive many. */
int tls_poll(tls_conn *c);

/* Queue application data. Returns the bytes ACCEPTED, which may be fewer than
 * asked (call again with the rest) or 0 if the session cannot take any right
 * now. <0 = failed/closed. Never blocks. */
int tls_send(tls_conn *c, const void *data, int len);

/* Take decrypted application data. Returns bytes copied, 0 if none has
 * arrived yet (NOT an error - poll again), <0 on close/failure. Never blocks. */
int tls_recv(tls_conn *c, void *buf, int cap);

/* Close politely if the transport still allows it, then free the handle and
 * its socket. Safe on NULL. */
void tls_free(tls_conn *c);

int  tls_conn_error(tls_conn *c);   /* BearSSL BR_ERR_* (0 = ok)              */
unsigned tls_conn_version(tls_conn *c);
unsigned tls_conn_cipher(tls_conn *c);
int  tls_conn_sock(tls_conn *c);    /* the netsock id, for diagnostics        */

/* ---- the legacy single-session API (blocking) ---------------------------- */

int  tls_connect(const u8 dst[4], u16 port, const char *sni);  /* 0 = handshake ok (pinned key) */
int  tls_connect_ca(const u8 dst[4], u16 port, const char *sni);  /* 0 = ok (CA-validated, HTTPS) */
int  tls_write(const void *data, int len);
int  tls_read(void *buf, int cap);
void tls_close(void);
int  tls_last_error(void);          /* BearSSL BR_ERR_* (0 = ok) */
unsigned tls_version(void);         /* 0x0303 = TLS 1.2 */
unsigned tls_cipher(void);          /* negotiated cipher suite id */

#endif
