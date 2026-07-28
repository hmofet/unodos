/* ===========================================================================
 * UnoDOS/pc64 - TLS client (BearSSL) over the pc64 TCP stack.
 *
 * Security is BearSSL's (constant-time, audited portable C) - we roll none of
 * our own crypto. Trust is a PINNED server public key (br_x509_knownkey), so
 * no CA store and no system clock are needed; that is a strong, simple model
 * for a fixed endpoint.
 *
 * Entropy FAILS CLOSED. RDRAND when the CPU has a working one; otherwise
 * conditioned CPU timing jitter, and only if it passes a health test. When
 * neither qualifies both connects refuse with TLS_ENOENTROPY rather than
 * handshake on a seed we cannot defend (this used to be a silent TSC-LCG).
 * ======================================================================== */
#ifndef PC64_TLS_H
#define PC64_TLS_H
#include "net.h"
#include "tls_entropy.h"   /* TLS_ENT_*, tls_entropy_source/name/get/selftest */

#define TLS_ENOENTROPY (-4)         /* tls_connect*: refused, no usable RNG   */

int  tls_connect(const u8 dst[4], u16 port, const char *sni);  /* 0 = handshake ok (pinned key) */
int  tls_connect_ca(const u8 dst[4], u16 port, const char *sni);  /* 0 = ok (CA-validated, HTTPS) */
int  tls_write(const void *data, int len);
int  tls_read(void *buf, int cap);
void tls_close(void);
int  tls_last_error(void);          /* BearSSL BR_ERR_* (0 = ok) */
unsigned tls_version(void);         /* 0x0303 = TLS 1.2 */
unsigned tls_cipher(void);          /* negotiated cipher suite id */

#endif
