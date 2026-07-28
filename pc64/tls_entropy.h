/* ===========================================================================
 * UnoDOS/pc64 - the entropy source behind TLS.  FAILS CLOSED.
 *
 * tls.c used to seed BearSSL from a TSC-LCG that the code itself labelled "NOT
 * cryptographically strong" whenever RDRAND was absent, and handshake anyway -
 * so an RDRAND-less box silently negotiated real TLS on demo-grade keys and no
 * caller could tell.  That is the one thing in this stack that failed OPEN.
 *
 *   1. Nothing is injected unless a source we can defend produced it.  When
 *      none qualifies, tls_connect / tls_connect_ca refuse (TLS_ENOENTROPY)
 *      before a socket exists.  A loud refusal beats a quiet weak key.
 *   2. A CPU without RDRAND is not given up on: conditioned CPU timing jitter
 *      is a real source.  It counts only after passing an online health test,
 *      so a counter that does not actually move (a strict emulator, a pinned
 *      TSC) reports NO source rather than a repeatable seed.
 *
 * Kept in its own file, separate from the TLS glue, so it can be built and
 * exercised natively by tools/tls_entropy_test.sh - the health test is the
 * whole security argument and has to be testable without QEMU or UEFI.
 * ======================================================================== */
#ifndef PC64_TLS_ENTROPY_H
#define PC64_TLS_ENTROPY_H

/* sources, as reported by tls_entropy_source() */
#define TLS_ENT_NONE    0           /* no usable source -> TLS is refused     */
#define TLS_ENT_RDRAND  1           /* the CPU's hardware DRNG                */
#define TLS_ENT_JITTER  2           /* SHA-256-conditioned CPU timing jitter  */

int  tls_entropy_source(void);      /* TLS_ENT_* (probes once, then cached)   */
const char *tls_entropy_name(void); /* "rdrand" / "jitter" / "none"           */
int  tls_have_rdrand(void);         /* 1 iff a WORKING hardware RNG is live   */

/* Fill `out` with n bytes from the live source.  Returns 0 when no source
 * qualifies - the caller MUST then refuse, never inject what it did get. */
int  tls_entropy_get(unsigned char *out, int n);

/* Gate hook (SPECTEST S-TLS-11, tools/tls_entropy_test.c):
 *    1 = a source is live and two draws differed  (the healthy case)
 *    0 = no usable source - a refused handshake is then the CORRECT outcome
 *   -1 = a source claimed to be live handed back identical bytes twice, which
 *        is exactly what the health test exists to stop reaching a key.
 * Returns no entropy to the caller; its seeds are zeroed before it returns. */
int  tls_entropy_selftest(void);

#endif
