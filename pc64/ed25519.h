/* ===========================================================================
 * UnoDOS/pc64 - Ed25519 signatures (RFC 8032).
 *
 * The one piece of crypto an SSH client needs that BearSSL does not have.
 * BearSSL carries no EdDSA at all, and its curve25519 code does not help: it
 * is Montgomery-ladder X25519 with no exported field arithmetic, while Ed25519
 * needs the twisted-Edwards form, point compression and SHA-512-derived
 * nonces. So this is a from-scratch implementation of RFC 8032, which is the
 * easiest kind to get right - fully specified, with official test vectors, and
 * testable on the host with no OS, no network and no hardware.
 *
 * SHA-512 comes from BearSSL. Nothing else is borrowed.
 *
 * `ssh-ed25519` has been OpenSSH's default key type since 2014, so without
 * this the client cannot log into anything we actually run.
 *
 * PROPERTIES. Signing and key generation are constant-time with respect to the
 * secret: the scalar multiply is a fixed 256-iteration ladder whose add is
 * selected by a masked cmov, never a branch, and the scalar reduction is a
 * fixed 512-step shift-subtract with a masked conditional subtract.
 * Verification handles only public data and does not try to be constant-time.
 *
 * NOT optimised. There is no windowing and no precomputed base-point table, so
 * a signature costs ~256 point doublings plus ~256 additions. That is a few
 * milliseconds on any machine pc64 runs on, and an SSH handshake does one.
 * ======================================================================== */
#ifndef PC64_ED25519_H
#define PC64_ED25519_H

/* Derive the public key from a 32-byte secret seed (RFC 8032's `sk`). */
void ed25519_pubkey(unsigned char pk[32], const unsigned char sk[32]);

/* Sign `mlen` bytes. `pk` must be the seed's own public key - passing another
 * key silently produces signatures that will not verify, exactly as the RFC's
 * construction implies, because the public key is hashed into the challenge. */
void ed25519_sign(unsigned char sig[64], const unsigned char *m, int mlen,
                  const unsigned char pk[32], const unsigned char sk[32]);

/* 1 = good signature, 0 = bad. Rejects a non-canonical S (S >= L) and any
 * point that is not on the curve, so a malformed signature is a clean 0
 * rather than an error deeper in the maths. */
int  ed25519_verify(const unsigned char sig[64], const unsigned char *m,
                    int mlen, const unsigned char pk[32]);

/* Self-test against the curve's own invariants (that the hard-coded d and
 * sqrt(-1) really are what they claim). Returns 1 on success. The RFC 8032
 * vectors live in tools/ed25519test.c; this is the cheap check a caller can
 * run on a machine that has no test harness. */
int  ed25519_selftest(void);

#endif
