/* ===========================================================================
 * UnoDOS/pc64 - SAE (Simultaneous Authentication of Equals), the WPA3
 * replacement for the WPA2 pre-shared key.
 *
 * WPA2 turned the passphrase into the PMK with PBKDF2 and nothing else: the
 * PMK is a pure function of (SSID, passphrase), so anyone who captures a
 * 4-way handshake can grind the passphrase offline at their leisure.  SAE
 * replaces that step with a balanced PAKE over NIST P-256 (finite cyclic
 * group 19): both sides prove they know the password without either the
 * password or anything derived from it crossing the air, and the PMK that
 * falls out is fresh per association.  Everything DOWNSTREAM of the PMK is
 * unchanged - the same EAPOL 4-way handshake in wifi_wpa.c installs the same
 * CCMP keys - so this file's whole job is: password in, 32-byte PMK out.
 *
 * Why it matters here: an Eero Pro 6E (and most 2024+ consumer APs) defaults
 * its main SSID to WPA3 or WPA2/WPA3-transition with management-frame
 * protection, and a 6 GHz SSID is WPA3-only by regulation.  A supplicant that
 * advertises AKM 00-0F-AC-02 and nothing else is simply not offered a way in.
 * That is what kept SKYNET unjoinable while the (WPA2-only) guest network
 * NimmuNet joined first time.
 *
 * Two ways to turn the password into the password element (PWE) exist and
 * both are implemented:
 *
 *   - HASH-TO-ELEMENT (H2E, 802.11-2020 12.4.4.3.2).  A fixed amount of work,
 *     no secret-dependent branches: SSWU maps two hashed field elements onto
 *     the curve and the result is scaled by a MAC-derived value.  PREFERRED,
 *     and mandatory on 6 GHz.  Used whenever the AP sets the H2E bit in its
 *     RSN Extension element.
 *   - HUNTING-AND-PECKING (HnP, 802.11-2020 12.4.4.3.3).  The original
 *     method: hash, test whether the result is a valid x-coordinate, repeat.
 *     Kept because it is the universally supported baseline, but it is the
 *     method the Dragonblood papers attacked - the number of iterations
 *     before success is password-dependent.  We run the full 40-iteration
 *     loop unconditionally (switching to a dummy password once found, as the
 *     reference supplicant does) so the ITERATION COUNT leaks nothing; the
 *     residual per-iteration timing signal is why H2E is preferred, not a
 *     fallback we are happy with.
 *
 * Pure computation over BearSSL primitives plus a small self-contained P-256
 * field arithmetic layer - no hardware, no I/O - so the whole exchange is
 * exercised on the host by tools/sae_test.sh with no QEMU and no radio.
 *
 * ENTROPY FAILS CLOSED.  SAE's security rests entirely on `rand` and `mask`
 * being unpredictable; a box with no defensible entropy source must not
 * "authenticate anyway" on a TSC counter.  sae_init() therefore draws from
 * tls_entropy_get() and returns SAE_ENOENTROPY when it refuses - the same
 * contract tls.c already holds itself to.
 * ======================================================================== */
#ifndef WIFI_SAE_H
#define WIFI_SAE_H

#define SAE_GROUP_P256   19      /* the only group we implement (mandatory)  */

/* sae_* return codes.  0 = success; negative = give up on this AP. */
#define SAE_OK            0
#define SAE_EINVAL       -1      /* malformed frame / bad parameter          */
#define SAE_EREJECT      -2      /* peer's commit or confirm did not verify  */
#define SAE_ENOENTROPY   -3      /* no defensible RNG - we refuse to join    */
#define SAE_EGROUP       -4      /* peer wants a group we do not implement   */
#define SAE_ESTATE       -5      /* frame arrived out of order               */

enum { SAE_ST_INIT = 0, SAE_ST_COMMITTED, SAE_ST_CONFIRMED, SAE_ST_ACCEPTED,
       SAE_ST_FAILED };

typedef struct {
    int  state;
    int  h2e;                    /* 1 = hash-to-element, 0 = hunt-and-peck   */
    unsigned char aa[6], spa[6]; /* authenticator (AP) / supplicant (us)     */
    unsigned char pwe[65];       /* 0x04 || x || y - the password element    */
    unsigned char rnd[32], mask[32];
    unsigned char own_scalar[32], own_elem[64];
    unsigned char peer_scalar[32], peer_elem[64];
    unsigned char kck[32];       /* SAE KCK - confirms only, NOT the EAPOL KCK */
    unsigned char pmk[32];       /* >>> the output: feed this to wpa_sm_init */
    unsigned char pmkid[16];     /* PMKID for the RSN IE in the assoc request */
    unsigned char token[256];
    int  token_len;
    unsigned short send_confirm;
} sae_t;

/* Derive the PWE and draw rand/mask.  `password` is the WPA passphrase;
 * `ssid` is needed only by H2E (it salts the HKDF).  `h2e` selects the
 * method - pass the AP's advertised capability, not a preference.
 * Returns SAE_OK, or SAE_ENOENTROPY when no usable RNG exists. */
int sae_init(sae_t *s, const char *ssid, int ssid_len,
             const char *password,
             const unsigned char own_mac[6], const unsigned char ap_mac[6],
             int h2e);

/* Body of an SAE Commit authentication frame - everything AFTER the 6-byte
 * (algorithm, sequence, status) header: group id, anti-clogging token if one
 * was demanded, scalar, element.  Returns the length, or <0.
 * The status code the caller must put in the header is sae_commit_status(). */
int sae_build_commit(sae_t *s, unsigned char *out, int cap);

/* The status code that belongs in OUR commit's header: 0 for hunting-and-
 * pecking, 126 (SAE_HASH_TO_ELEMENT) for H2E. */
int sae_commit_status(const sae_t *s);

/* Consume the AP's Commit body (same offset).  On SAE_OK the shared secret,
 * PMK, PMKID and KCK are computed and the state advances to COMMITTED.
 * An AP that answers with status 76 instead sends a token rather than a
 * commit; the caller feeds that to sae_token_from_reject() and re-sends. */
int sae_rx_commit(sae_t *s, const unsigned char *body, int len);

/* Record an anti-clogging token to include in the next commit.  The commit is
 * otherwise IDENTICAL - same scalar, same element; regenerating them would
 * restart the exchange and some APs answer that with a deauthentication. */
int sae_set_token(sae_t *s, const unsigned char *tok, int len);

/* Extract the token from a status-76 Commit body (group id, then token). */
int sae_token_from_reject(sae_t *s, const unsigned char *body, int len);

/* Body of an SAE Confirm frame: send-confirm counter, then the confirm hash. */
int sae_build_confirm(sae_t *s, unsigned char *out, int cap);

/* Verify the AP's Confirm body.  SAE_OK advances to ACCEPTED, at which point
 * s->pmk is authenticated and the association may proceed. */
int sae_rx_confirm(sae_t *s, const unsigned char *body, int len);

/* Wipe every secret in the state (called on failure and on disconnect). */
void sae_clear(sae_t *s);

/* ---- exposed for the host gate (tools/sae_test.c) ------------------------
 * Not part of the supplicant's own interface; they exist so the arithmetic
 * can be checked against an independent implementation rather than only
 * against itself. */
int  sae_pwe_for_test(const char *ssid, int ssid_len, const char *password,
                      const unsigned char a[6], const unsigned char b[6],
                      int h2e, unsigned char pwe_out[65]);

#endif
