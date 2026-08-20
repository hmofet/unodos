/* SAE / WPA3 host gate.  Seconds, no QEMU, no radio - run it after every edit
 * to wifi_sae.c or the AKM-aware half of wifi_wpa.c.
 *
 * Three kinds of check, in increasing order of what they can catch:
 *
 *   1. SELF-CONSISTENCY.  Two supplicant instances run the exchange against
 *      each other and must land on the same PMK.  This catches state-machine
 *      and framing bugs, and NOTHING else: a field arithmetic layer that is
 *      wrong in the same way on both sides agrees with itself perfectly.
 *   2. STRUCTURAL.  The PWE must be a point ON the curve, the confirm must
 *      reject a flipped bit, a reflected commit must be refused.
 *   3. CROSS-IMPLEMENTATION.  Mode "vectors" prints the derived values for a
 *      fixed input; tools/sae_test.py recomputes them from the 802.11 text
 *      with Python's own bignums and diffs.  That is the only check here that
 *      can catch "my P-256 is self-consistently wrong", which is exactly the
 *      failure mode a hand-rolled Montgomery layer has.
 *
 * Built WITH build.sh's sanitizer set, deliberately - a harness compiled
 * without them tests different code from the one the debug OS runs. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wifi_sae.h"
#include "wifi_wpa.h"

typedef unsigned char u8;

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

/* ---- the entropy the supplicant draws from ------------------------------
 * wifi_sae.c calls tls_entropy_get() and REFUSES when it returns 0, which is
 * the behaviour a gate has to be able to drive both ways.  Rather than link
 * the real source (which pulls in the network stack's frame counters), supply
 * a deterministic stream here: reproducible failures beat realistic ones, and
 * "no entropy" becomes a case we can actually test. */
static unsigned long long g_rngstate = 0x0123456789abcdefULL;
static int g_entropy_dead;
int tls_entropy_get(unsigned char *out, int n);
int tls_entropy_get(unsigned char *out, int n)
{
    int i;
    if (g_entropy_dead) return 0;
    for (i = 0; i < n; i++) {
        g_rngstate = g_rngstate * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = (u8)(g_rngstate >> 33);
    }
    return 1;
}

static void hex(const char *tag, const u8 *b, int n)
{
    int i;
    printf("%s ", tag);
    for (i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

/* ---- P-256 curve membership, computed independently of wifi_sae.c --------
 * Long multiplication over 32-bit limbs with a schoolbook mod-p reduction; it
 * is far too slow for the supplicant and completely different code from the
 * Montgomery layer under test, which is the point. */
#define LN 16                             /* 512-bit scratch */
typedef unsigned int  u32;
typedef unsigned long long u64;

static void big_from_be(u32 *z, const u8 *b, int nbytes)
{
    int i;
    memset(z, 0, LN * 4);
    for (i = 0; i < nbytes; i++) z[(nbytes - 1 - i) / 4] |= (u32)b[i] << (8 * ((nbytes - 1 - i) % 4));
}
static int big_cmp(const u32 *a, const u32 *b)
{ int i; for (i = LN - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1; return 0; }
static void big_sub(u32 *a, const u32 *b)
{ int i; u64 c = 0; for (i = 0; i < LN; i++) { u64 v = (u64)a[i] - b[i] - c; a[i] = (u32)v; c = (v >> 32) ? 1 : 0; } }
static void big_add(u32 *a, const u32 *b)
{ int i; u64 c = 0; for (i = 0; i < LN; i++) { u64 v = (u64)a[i] + b[i] + c; a[i] = (u32)v; c = v >> 32; } }
static void big_shl1(u32 *a)
{ int i; u32 c = 0; for (i = 0; i < LN; i++) { u32 v = a[i]; a[i] = (v << 1) | c; c = v >> 31; } }

static const u8 PBE[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static const u8 BBE[32] = {
    0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
    0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b };

static void mod_p(u32 *a, const u32 *p)
{ while (big_cmp(a, p) >= 0) big_sub(a, p); }

/* z = a*b mod p, by shift-and-add - O(256) additions, ~microseconds. */
static void mulmod(u32 *z, const u32 *a, const u32 *b, const u32 *p)
{
    u32 acc[LN], t[LN];
    int i;
    memset(acc, 0, sizeof acc);
    memcpy(t, a, sizeof t);
    mod_p(t, p);
    for (i = 0; i < 256; i++) {
        if ((b[i / 32] >> (i % 32)) & 1) { big_add(acc, t); mod_p(acc, p); }
        big_shl1(t); mod_p(t, p);
    }
    memcpy(z, acc, LN * 4);
}

/* 1 iff the 65-byte uncompressed point satisfies y^2 == x^3 - 3x + b (mod p). */
static int point_on_curve(const u8 pt[65])
{
    u32 p[LN], x[LN], y[LN], b[LN], lhs[LN], rhs[LN], t[LN], three[LN], tx[LN];
    if (pt[0] != 0x04) return 0;
    big_from_be(p, PBE, 32);
    big_from_be(b, BBE, 32);
    big_from_be(x, pt + 1, 32);
    big_from_be(y, pt + 33, 32);
    if (big_cmp(x, p) >= 0 || big_cmp(y, p) >= 0) return 0;
    mulmod(lhs, y, y, p);                       /* y^2            */
    mulmod(t, x, x, p);
    mulmod(rhs, t, x, p);                       /* x^3            */
    memset(three, 0, sizeof three); three[0] = 3;
    mulmod(tx, three, x, p);                    /* 3x             */
    if (big_cmp(rhs, tx) < 0) big_add(rhs, p);
    big_sub(rhs, tx);                           /* x^3 - 3x       */
    big_add(rhs, b); mod_p(rhs, p);             /* + b            */
    return big_cmp(lhs, rhs) == 0;
}

/* ===========================================================================
 * a complete exchange between two instances of the supplicant
 * ======================================================================== */
static int run_pair(int h2e, const char *ssid, const char *pw,
                    u8 pmk_a[32], u8 pmk_b[32])
{
    sae_t a, b;
    static const u8 mac_a[6] = { 0x02,0x00,0x00,0x00,0x00,0x01 };
    static const u8 mac_b[6] = { 0x02,0x00,0x00,0x00,0x00,0x02 };
    u8 ca[256], cb[256], fa[64], fb[64];
    int na, nb, rc;

    rc = sae_init(&a, ssid, (int)strlen(ssid), pw, mac_a, mac_b, h2e);
    if (rc != SAE_OK) { printf("  sae_init(a) -> %d\n", rc); return rc; }
    rc = sae_init(&b, ssid, (int)strlen(ssid), pw, mac_b, mac_a, h2e);
    if (rc != SAE_OK) { printf("  sae_init(b) -> %d\n", rc); return rc; }

    /* The PWE is a function of the password and the UNORDERED MAC pair, so
     * both peers must derive the identical point.  If this differs, nothing
     * downstream can agree and every later check is meaningless. */
    CHECK(!memcmp(a.pwe, b.pwe, 65), "the two peers derived different PWEs (h2e=%d)", h2e);
    CHECK(point_on_curve(a.pwe), "PWE is not on P-256 (h2e=%d)", h2e);

    na = sae_build_commit(&a, ca, sizeof ca);
    nb = sae_build_commit(&b, cb, sizeof cb);
    CHECK(na == 98 && nb == 98, "commit body should be 2+32+64 bytes, got %d/%d", na, nb);
    rc = sae_rx_commit(&a, cb, nb); CHECK(rc == SAE_OK, "a.rx_commit -> %d", rc);
    rc = sae_rx_commit(&b, ca, na); CHECK(rc == SAE_OK, "b.rx_commit -> %d", rc);
    CHECK(!memcmp(a.pmk, b.pmk, 32), "PMK mismatch (h2e=%d)", h2e);
    CHECK(!memcmp(a.pmkid, b.pmkid, 16), "PMKID mismatch (h2e=%d)", h2e);

    na = sae_build_confirm(&a, fa, sizeof fa);
    nb = sae_build_confirm(&b, fb, sizeof fb);
    CHECK(na == 34 && nb == 34, "confirm body should be 34 bytes, got %d/%d", na, nb);
    rc = sae_rx_confirm(&a, fb, nb); CHECK(rc == SAE_OK, "a.rx_confirm -> %d", rc);
    rc = sae_rx_confirm(&b, fa, na); CHECK(rc == SAE_OK, "b.rx_confirm -> %d", rc);
    CHECK(a.state == SAE_ST_ACCEPTED && b.state == SAE_ST_ACCEPTED,
          "states %d/%d, expected ACCEPTED", a.state, b.state);

    memcpy(pmk_a, a.pmk, 32);
    memcpy(pmk_b, b.pmk, 32);
    return SAE_OK;
}

static void t_exchange(void)
{
    u8 pa[32], pb[32], qa[32], qb[32];
    printf("-- full exchange, hunting-and-pecking\n");
    run_pair(0, "SKYNET", "correct horse battery", pa, pb);
    printf("-- full exchange, hash-to-element\n");
    run_pair(1, "SKYNET", "correct horse battery", qa, qb);
    /* The two PWE derivations are different functions of the same password,
     * so they MUST produce different PMKs.  If they matched, one of them
     * would not be doing what it claims. */
    CHECK(memcmp(pa, qa, 32) != 0, "HnP and H2E produced the same PMK - one of them is a no-op");
}

static void t_wrong_password(void)
{
    sae_t a, b;
    static const u8 ma[6] = { 0x02,0,0,0,0,1 }, mb[6] = { 0x02,0,0,0,0,2 };
    u8 ca[256], cb[256], fa[64], fb[64];
    int na, nb, rc;
    printf("-- a wrong passphrase is rejected at CONFIRM, not earlier\n");
    sae_init(&a, "SKYNET", 6, "the right one", ma, mb, 1);
    sae_init(&b, "SKYNET", 6, "the WRONG one", mb, ma, 1);
    na = sae_build_commit(&a, ca, sizeof ca);
    nb = sae_build_commit(&b, cb, sizeof cb);
    /* Commit always succeeds: SAE deliberately leaks nothing about the
     * password until the confirm, which is why a WPA3 network cannot be
     * probed for its password the way a WPA2 one can. */
    rc = sae_rx_commit(&a, cb, nb); CHECK(rc == SAE_OK, "commit should still verify, got %d", rc);
    rc = sae_rx_commit(&b, ca, na); CHECK(rc == SAE_OK, "commit should still verify, got %d", rc);
    CHECK(memcmp(a.pmk, b.pmk, 32) != 0, "different passwords produced the same PMK");
    sae_build_confirm(&a, fa, sizeof fa);
    nb = sae_build_confirm(&b, fb, sizeof fb);
    rc = sae_rx_confirm(&a, fb, nb);
    CHECK(rc == SAE_EREJECT, "confirm from a wrong-password peer should be EREJECT, got %d", rc);
    CHECK(a.state == SAE_ST_FAILED, "state after a bad confirm should be FAILED, got %d", a.state);
}

static void t_tamper(void)
{
    sae_t a, b;
    static const u8 ma[6] = { 0x02,0,0,0,0,1 }, mb[6] = { 0x02,0,0,0,0,2 };
    u8 ca[256], cb[256], fb[64];
    int na, nb, rc, bit;
    printf("-- every single bit of the confirm is covered by the MAC\n");
    sae_init(&a, "SKYNET", 6, "pw", ma, mb, 1);
    sae_init(&b, "SKYNET", 6, "pw", mb, ma, 1);
    na = sae_build_commit(&a, ca, sizeof ca);
    nb = sae_build_commit(&b, cb, sizeof cb);
    sae_rx_commit(&a, cb, nb);
    sae_rx_commit(&b, ca, na);
    nb = sae_build_confirm(&b, fb, sizeof fb);
    for (bit = 0; bit < 34 * 8; bit++) {
        sae_t t = a;
        u8 f[64];
        memcpy(f, fb, (size_t)nb);
        f[bit / 8] ^= (u8)(1 << (bit % 8));
        rc = sae_rx_confirm(&t, f, nb);
        if (rc == SAE_OK) { CHECK(0, "a confirm with bit %d flipped was ACCEPTED", bit); break; }
    }
    printf("-- a reflected commit is refused\n");
    { sae_t t;
      sae_init(&t, "SKYNET", 6, "pw", ma, mb, 1);
      na = sae_build_commit(&t, ca, sizeof ca);
      rc = sae_rx_commit(&t, ca, na);
      CHECK(rc == SAE_EREJECT, "our own commit echoed back should be EREJECT, got %d", rc); }
    printf("-- an out-of-range scalar is refused\n");
    { sae_t t;
      sae_init(&t, "SKYNET", 6, "pw", ma, mb, 1);
      nb = sae_build_commit(&b, cb, sizeof cb);
      memset(cb + 2, 0, 32);                /* scalar = 0 */
      rc = sae_rx_commit(&t, cb, nb);
      CHECK(rc == SAE_EREJECT, "a zero scalar should be EREJECT, got %d", rc);
      sae_init(&t, "SKYNET", 6, "pw", ma, mb, 1);
      nb = sae_build_commit(&b, cb, sizeof cb);
      memset(cb + 34, 0xff, 64);            /* element coordinates >= p */
      rc = sae_rx_commit(&t, cb, nb);
      CHECK(rc == SAE_EREJECT, "an off-curve element should be EREJECT, got %d", rc); }
    printf("-- a commit naming a group we do not implement is refused\n");
    { sae_t t;
      sae_init(&t, "SKYNET", 6, "pw", ma, mb, 1);
      nb = sae_build_commit(&b, cb, sizeof cb);
      cb[0] = 20;                            /* group 20 = P-384 */
      rc = sae_rx_commit(&t, cb, nb);
      CHECK(rc == SAE_EGROUP, "group 20 should be EGROUP, got %d", rc); }
}

static void t_no_entropy(void)
{
    sae_t s;
    static const u8 ma[6] = { 0x02,0,0,0,0,1 }, mb[6] = { 0x02,0,0,0,0,2 };
    int rc;
    printf("-- with no RNG, SAE REFUSES rather than joining on a guessable secret\n");
    g_entropy_dead = 1;
    rc = sae_init(&s, "SKYNET", 6, "pw", ma, mb, 1);
    g_entropy_dead = 0;
    CHECK(rc == SAE_ENOENTROPY, "expected SAE_ENOENTROPY, got %d", rc);
}

/* ---- the RSN element we put on the air ---------------------------------- */
static void t_rsn(void)
{
    u8 ie[64];
    wpa_rsn_cfg_t cfg;
    wpa_ap_sec_t ap;
    u8 pmkid[16];
    int n;
    printf("-- the RSN element\n");
    memset(pmkid, 0xA5, sizeof pmkid);

    memset(&cfg, 0, sizeof cfg); cfg.akm = WPA_AKM_PSK; cfg.mfp = WPA_MFP_OFF;
    n = wpa_build_rsn_ie(ie, sizeof ie, &cfg);
    /* The WPA2 element must stay byte-identical to the 22-byte one that has
     * been joining networks for a year; this is the regression guard on the
     * whole parameterisation. */
    CHECK(n == 22, "WPA2 RSNE should still be 22 bytes, got %d", n);
    CHECK(ie[0] == 0x30 && ie[1] == 20, "WPA2 RSNE header changed: %02x %02x", ie[0], ie[1]);
    CHECK(ie[19] == 0x02, "WPA2 AKM should be 00-0F-AC-02, got ..%02x", ie[19]);
    CHECK(ie[20] == 0 && ie[21] == 0, "WPA2 RSN caps should be 0000");

    memset(&cfg, 0, sizeof cfg);
    cfg.akm = WPA_AKM_SAE; cfg.mfp = WPA_MFP_REQUIRED; cfg.pmkid = pmkid;
    n = wpa_build_rsn_ie(ie, sizeof ie, &cfg);
    CHECK(n == 44, "SAE RSNE with a PMKID should be 44 bytes, got %d", n);
    CHECK(ie[19] == 0x08, "SAE AKM should be 00-0F-AC-08, got ..%02x", ie[19]);
    CHECK(ie[20] == 0xC0 && ie[21] == 0x00, "MFPC|MFPR should be 00c0, got %02x%02x", ie[21], ie[20]);
    CHECK(ie[22] == 0x01 && ie[23] == 0x00, "PMKID count should be 1");
    CHECK(!memcmp(ie + 24, pmkid, 16), "PMKID not echoed");
    CHECK(ie[43] == 0x06, "group mgmt cipher should be BIP-CMAC-128, got ..%02x", ie[43]);

    /* PMF with no PMKID still needs the zero count, or the AP reads the BIP
     * suite as a PMKID count and rejects the association. */
    memset(&cfg, 0, sizeof cfg); cfg.akm = WPA_AKM_SAE; cfg.mfp = WPA_MFP_REQUIRED;
    n = wpa_build_rsn_ie(ie, sizeof ie, &cfg);
    CHECK(n == 28, "SAE RSNE without a PMKID should be 28 bytes, got %d", n);
    CHECK(ie[22] == 0 && ie[23] == 0, "a zero PMKID count must still be present");
    CHECK(ie[27] == 0x06, "group mgmt cipher must follow it");

    /* ...and we must be able to read back what we write. */
    CHECK(wpa_parse_rsn_ie(ie, n, &ap), "our own SAE RSNE did not parse");
    CHECK(ap.akm == WPA_AKM_SAE, "round-trip AKM %02x", ap.akm);
    CHECK(ap.mfpc && ap.mfpr, "round-trip MFP c=%d r=%d", ap.mfpc, ap.mfpr);
    CHECK(ap.group_mgmt == 6, "round-trip group mgmt %d", ap.group_mgmt);

    n = wpa_build_rsnxe(ie, sizeof ie, 1);
    CHECK(n == 3 && ie[0] == 0xF4 && ie[1] == 1 && ie[2] == 0x20,
          "RSNXE should be f4 01 20, got %02x %02x %02x (n=%d)", ie[0], ie[1], ie[2], n);
    memset(&ap, 0, sizeof ap);
    wpa_parse_rsnxe(ie, n, &ap);
    CHECK(ap.h2e == 1, "our own RSNXE did not read back as H2E");
    CHECK(wpa_build_rsnxe(ie, sizeof ie, 0) == 0, "no RSNXE when H2E is off");
}

/* An Eero-shaped beacon: WPA2/WPA3 transition mode with PMF capable, which is
 * the configuration that locked the old supplicant out. */
static void t_transition_ap(void)
{
    static const u8 beacon_rsne[] = {
        0x30, 0x18,                         /* RSNE, 24 bytes                */
        0x01, 0x00,                         /* version 1                     */
        0x00,0x0F,0xAC,0x04,                /* group: CCMP                   */
        0x01,0x00, 0x00,0x0F,0xAC,0x04,     /* pairwise: CCMP                */
        0x02,0x00, 0x00,0x0F,0xAC,0x02,     /* AKM: PSK ...                  */
                   0x00,0x0F,0xAC,0x08,     /*      ... and SAE              */
        0x80,0x00                           /* caps: MFPC                    */
    };
    static const u8 rsnxe[] = { 0xF4, 0x01, 0x20 };
    static const u8 wpa2_only[] = {
        0x30, 0x14, 0x01, 0x00, 0x00,0x0F,0xAC,0x04,
        0x01,0x00, 0x00,0x0F,0xAC,0x04, 0x01,0x00, 0x00,0x0F,0xAC,0x02, 0x00,0x00
    };
    wpa_ap_sec_t ap;
    printf("-- an Eero-shaped transition-mode beacon\n");
    CHECK(wpa_parse_rsn_ie(beacon_rsne, sizeof beacon_rsne, &ap), "transition RSNE did not parse");
    wpa_parse_rsnxe(rsnxe, sizeof rsnxe, &ap);
    CHECK(ap.akm == (WPA_AKM_PSK | WPA_AKM_SAE), "expected PSK|SAE, got %02x", ap.akm);
    CHECK(ap.mfpc == 1 && ap.mfpr == 0, "expected MFPC only, got c=%d r=%d", ap.mfpc, ap.mfpr);
    CHECK(ap.h2e == 1, "expected H2E advertised");
    CHECK(wpa_pick_akm(&ap) == WPA_AKM_SAE, "we should choose SAE on a transition AP");
    CHECK(wpa_pick_mfp(&ap, WPA_AKM_SAE) == WPA_MFP_REQUIRED, "SAE must claim MFPC+MFPR");

    printf("-- a WPA2-only AP still takes the proven path unchanged\n");
    CHECK(wpa_parse_rsn_ie(wpa2_only, sizeof wpa2_only, &ap), "WPA2 RSNE did not parse");
    CHECK(wpa_pick_akm(&ap) == WPA_AKM_PSK, "expected PSK on a WPA2-only AP");
    CHECK(wpa_pick_mfp(&ap, WPA_AKM_PSK) == WPA_MFP_OFF, "PMF must stay off for plain WPA2");

    printf("-- an enterprise (802.1X) AP is refused rather than half-attempted\n");
    { u8 ent[sizeof wpa2_only];
      memcpy(ent, wpa2_only, sizeof ent);
      ent[19] = 0x01;                                   /* AKM 00-0F-AC-01 */
      CHECK(wpa_parse_rsn_ie(ent, sizeof ent, &ap), "802.1X RSNE did not parse");
      CHECK(wpa_pick_akm(&ap) == 0, "802.1X should be unjoinable, got %02x", wpa_pick_akm(&ap)); }

    printf("-- a truncated RSN element does not read as permissive\n");
    { u8 trunc[6];
      memcpy(trunc, wpa2_only, 6); trunc[1] = 4;
      CHECK(wpa_parse_rsn_ie(trunc, sizeof trunc, &ap), "truncated RSNE should still parse");
      CHECK(wpa_pick_akm(&ap) == 0, "a truncated element must offer nothing"); }
}

/* ---- the 802.11 SHA-256 KDF --------------------------------------------- */
static void t_kdf(void)
{
    u8 out[64], again[64];
    printf("-- the 802.11 KDF is deterministic and length-separated\n");
    wpa_kdf_sha256((const u8 *)"key", 3, "label", (const u8 *)"ctx", 3, out, 256);
    wpa_kdf_sha256((const u8 *)"key", 3, "label", (const u8 *)"ctx", 3, again, 256);
    CHECK(!memcmp(out, again, 32), "the KDF is not deterministic");
    /* The output length is hashed IN, so a 512-bit draw does not merely extend
     * a 256-bit one.  Getting this wrong makes SAE's KCK equal the first half
     * of something else and nothing visibly breaks. */
    wpa_kdf_sha256((const u8 *)"key", 3, "label", (const u8 *)"ctx", 3, again, 512);
    CHECK(memcmp(out, again, 32) != 0, "KDF output does not depend on the requested length");
}

/* Print the values tools/sae_test.py recomputes independently. */
static void vectors(void)
{
    static const u8 ma[6] = { 0x02,0x11,0x22,0x33,0x44,0x55 };
    static const u8 mb[6] = { 0x02,0xaa,0xbb,0xcc,0xdd,0xee };
    u8 pwe[65], out[64];
    sae_pwe_for_test("SKYNET", 6, "a test passphrase", ma, mb, 0, pwe);
    hex("pwe_hnp", pwe, 65);
    sae_pwe_for_test("SKYNET", 6, "a test passphrase", ma, mb, 1, pwe);
    hex("pwe_h2e", pwe, 65);
    wpa_kdf_sha256((const u8 *)"0123456789abcdef", 16, "SAE KCK and PMK",
                   (const u8 *)"0123456789abcdef0123456789abcdef", 32, out, 512);
    hex("kdf512", out, 64);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "vectors")) { vectors(); return 0; }
    t_kdf();
    t_rsn();
    t_transition_ap();
    t_exchange();
    t_wrong_password();
    t_tamper();
    t_no_entropy();
    if (fails) { printf("\n%d FAILURE(S)\n", fails); return 1; }
    printf("\nall SAE / WPA3 checks passed\n");
    return 0;
}
