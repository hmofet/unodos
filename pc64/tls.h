/* ===========================================================================
 * UnoDOS/pc64 - TLS client (BearSSL) over the pc64 TCP stack.
 *
 * Security is BearSSL's (constant-time, audited portable C) - we roll none of
 * our own crypto. Trust is a PINNED server public key (br_x509_knownkey), so
 * no CA store and no system clock are needed; that is a strong, simple model
 * for a fixed endpoint. Entropy comes from RDRAND when the CPU has it.
 * ======================================================================== */
#ifndef PC64_TLS_H
#define PC64_TLS_H
#include "net.h"

int  tls_connect(const u8 dst[4], u16 port, const char *sni);  /* 0 = handshake ok */
int  tls_write(const void *data, int len);
int  tls_read(void *buf, int cap);
void tls_close(void);
int  tls_last_error(void);          /* BearSSL BR_ERR_* (0 = ok) */
unsigned tls_version(void);         /* 0x0303 = TLS 1.2 */
unsigned tls_cipher(void);          /* negotiated cipher suite id */
int  tls_have_rdrand(void);         /* 1 if hardware RNG seeded the PRNG */

#endif
