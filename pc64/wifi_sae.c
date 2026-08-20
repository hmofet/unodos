/* SAE / WPA3 authentication over NIST P-256 (see wifi_sae.h).
 *
 * References: IEEE 802.11-2020 12.4 (SAE) and 12.7.1.6 (the KDF), RFC 5869
 * (HKDF, used by hash-to-element), RFC 6090 (curve arithmetic).
 *
 * Curve operations come from BearSSL's br_ec_impl - point multiplication and
 * the two-point multiply-add - which also gives us peer-point validation for
 * free: br_ec's decode rejects anything not on the P-256 subgroup, and its
 * muladd reports the point at infinity as an error.  What BearSSL does NOT
 * expose is arithmetic in the coordinate field, and SAE needs plenty of it:
 * both PWE derivations have to solve y^2 = x^3 + ax + b, which means modular
 * square roots, quadratic-residue tests and inverses mod p.  Rather than
 * reach into bearssl/src/inner.h (a private header, and one the pc64 compile
 * flags do not even put on the include path), this file carries its own
 * 256-bit Montgomery layer.  It is ~150 lines, it is checked against an
 * independent Python implementation by tools/sae_test.sh, and it keeps the
 * BearSSL vendor tree a black box we only CONSUME.
 *
 * Every constant below is derived at first use from p, r and b - there are no
 * transcribed Montgomery magic numbers to get wrong. */
#include "wifi_sae.h"
#include "wifi_wpa.h"
#include "tls_entropy.h"
#include <string.h>
#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include "bearssl_ec.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

#define NL 8                       /* 8 x 32-bit limbs = 256 bits           */
typedef u32 fe[NL];                /* little-endian limb order              */

/* ===========================================================================
 * 1. 256-bit modular arithmetic
 * ======================================================================== */

static const u8 P256_P[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static const u8 P256_R[32] = {   /* the subgroup order n */
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51 };
static const u8 P256_B[32] = {
    0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
    0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b };

/* Everything below is filled in once by fld_init(). */
static fe FP;                 /* p                                          */
static fe FR;                 /* n (subgroup order)                         */
static fe MONT1;              /* R mod p        (1 in Montgomery form)      */
static fe MONT_R2;            /* R^2 mod p      (the to-Montgomery factor)  */
static fe E_SQRT;             /* (p+1)/4        exponent for square roots   */
static fe E_LEG;              /* (p-1)/2        exponent for the QR test    */
static fe E_INV;              /* p-2            exponent for inversion      */
static fe MB, MA, MZ;         /* b, a = -3, z = -10, all in Montgomery form */
static fe M_SSWU_C1;          /* b / (z*a)                                  */
static fe M_SSWU_C2;          /* -b / a                                     */
static u32 FP_N0;             /* -p^-1 mod 2^32                             */
static int g_fld_ready;

static void fe_zero(u32 *z)                 { memset(z, 0, NL * 4); }
static void fe_copy(u32 *z, const u32 *a)   { memcpy(z, a, NL * 4); }

static void fe_from_be(u32 *z, const u8 *b)
{
    int i;
    for (i = 0; i < NL; i++) {
        const u8 *q = b + (NL - 1 - i) * 4;
        z[i] = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
    }
}
static void fe_to_be(u8 *b, const u32 *z)
{
    int i;
    for (i = 0; i < NL; i++) {
        u8 *q = b + (NL - 1 - i) * 4;
        q[0] = (u8)(z[i] >> 24); q[1] = (u8)(z[i] >> 16);
        q[2] = (u8)(z[i] >> 8);  q[3] = (u8)z[i];
    }
}

static int fe_is_zero(const u32 *a)
{ int i; u32 v = 0; for (i = 0; i < NL; i++) v |= a[i]; return v == 0; }

/* 1 iff a <= 1 - the "commit-scalar must exceed 1" test. */
static int fe_le_one(const u32 *a)
{ int i; u32 v = 0; for (i = 1; i < NL; i++) v |= a[i]; return v == 0 && a[0] <= 1; }

static int fe_cmp(const u32 *a, const u32 *b)
{
    int i;
    for (i = NL - 1; i >= 0; i--) { if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1; }
    return 0;
}

/* z = a - b (mod m), for a,b < m */
static void fe_sub(u32 *z, const u32 *a, const u32 *b, const u32 *m)
{
    u64 c = 0; int i; u32 t[NL]; u32 borrow;
    for (i = 0; i < NL; i++) { u64 v = (u64)a[i] - b[i] - c; t[i] = (u32)v; c = (v >> 32) ? 1 : 0; }
    borrow = (u32)c;
    c = 0;
    for (i = 0; i < NL; i++) { u64 v = (u64)t[i] + (m[i] & (u32)(0 - borrow)) + c; z[i] = (u32)v; c = v >> 32; }
}

/* z = a + b (mod m), for a,b < m */
static void fe_add(u32 *z, const u32 *a, const u32 *b, const u32 *m)
{
    u64 c = 0; int i; u32 t[NL]; u32 top, ge;
    for (i = 0; i < NL; i++) { u64 v = (u64)a[i] + b[i] + c; t[i] = (u32)v; c = v >> 32; }
    top = (u32)c;
    ge = top;
    if (!ge) { for (i = NL - 1; i >= 0; i--) { if (t[i] != m[i]) { ge = t[i] > m[i]; break; } if (i == 0) ge = 1; } }
    c = 0;
    for (i = 0; i < NL; i++) { u64 v = (u64)t[i] - (m[i] & (u32)(0 - ge)) - c; z[i] = (u32)v; c = (v >> 32) ? 1 : 0; }
}

/* Montgomery multiplication mod p: z = a*b*R^-1 mod p (CIOS, Koc et al). */
static void mont_mul(u32 *z, const u32 *a, const u32 *b)
{
    u32 t[NL + 2];
    int i, j;
    memset(t, 0, sizeof t);
    for (i = 0; i < NL; i++) {
        u64 c = 0, s;
        u32 mi;
        for (j = 0; j < NL; j++) {
            s = (u64)a[j] * b[i] + t[j] + c;
            t[j] = (u32)s; c = s >> 32;
        }
        s = (u64)t[NL] + c;
        t[NL] = (u32)s; t[NL + 1] = (u32)(s >> 32);
        mi = t[0] * FP_N0;
        c = 0;
        s = (u64)mi * FP[0] + t[0];
        c = s >> 32;                          /* the low word is discarded: it is 0 */
        for (j = 1; j < NL; j++) {
            s = (u64)mi * FP[j] + t[j] + c;
            t[j - 1] = (u32)s; c = s >> 32;
        }
        s = (u64)t[NL] + c;
        t[NL - 1] = (u32)s;
        t[NL] = t[NL + 1] + (u32)(s >> 32);
    }
    /* t[0..NL] < 2p; one conditional subtraction brings it below p. */
    {
        u32 ge = t[NL];
        u64 c = 0;
        if (!ge) { for (j = NL - 1; j >= 0; j--) { if (t[j] != FP[j]) { ge = t[j] > FP[j]; break; } if (j == 0) ge = 1; } }
        for (j = 0; j < NL; j++) { u64 v = (u64)t[j] - (FP[j] & (u32)(0 - ge)) - c; z[j] = (u32)v; c = (v >> 32) ? 1 : 0; }
    }
}

static void mont_sqr(u32 *z, const u32 *a) { mont_mul(z, a, a); }
static void to_mont(u32 *z, const u32 *a)  { mont_mul(z, a, MONT_R2); }
static void from_mont(u32 *z, const u32 *a){ fe one; fe_zero(one); one[0] = 1; mont_mul(z, a, one); }

/* z = base^e mod p, base and z in Montgomery form.  Square-and-multiply: the
 * exponents here are (p+1)/4, (p-1)/2 and p-2 - all PUBLIC constants - so the
 * key-dependent-branch objection to this shape does not apply. */
static void mont_pow(u32 *z, const u32 *base, const u32 *e)
{
    fe acc, b;
    int i;
    fe_copy(acc, MONT1);
    fe_copy(b, base);
    for (i = NL * 32 - 1; i >= 0; i--) {
        mont_sqr(acc, acc);
        if ((e[i >> 5] >> (i & 31)) & 1) mont_mul(acc, acc, b);
    }
    fe_copy(z, acc);
}

static void mont_inv(u32 *z, const u32 *a)  { mont_pow(z, a, E_INV); }
static void mont_sqrt(u32 *z, const u32 *a) { mont_pow(z, a, E_SQRT); }

/* 1 iff a is a non-zero quadratic residue mod p. */
static int mont_is_qr(const u32 *a)
{
    fe t;
    if (fe_is_zero(a)) return 0;
    mont_pow(t, a, E_LEG);
    return fe_cmp(t, MONT1) == 0;
}

/* gx = x^3 + a*x + b, everything in Montgomery form. */
static void curve_rhs(u32 *gx, const u32 *x)
{
    fe t;
    mont_sqr(t, x);
    mont_mul(t, t, x);                    /* x^3            */
    mont_mul(gx, MA, x);                  /* a*x            */
    fe_add(t, t, gx, FP);
    fe_add(gx, t, MB, FP);
}

/* r = x mod m, where x is xlen bytes big-endian.  Shift-and-subtract: xlen is
 * at most 48 here, so 384 iterations, once or twice per handshake. */
static void mp_mod_be(u32 *r, const u8 *x, int xlen, const u32 *m)
{
    u32 acc[NL + 1];
    int i, j;
    memset(acc, 0, sizeof acc);
    for (i = 0; i < xlen * 8; i++) {
        u32 carry = (u32)((x[i >> 3] >> (7 - (i & 7))) & 1);
        u32 ge;
        u64 c;
        for (j = 0; j <= NL; j++) { u32 v = acc[j]; acc[j] = (v << 1) | carry; carry = v >> 31; }
        ge = acc[NL];
        if (!ge) { for (j = NL - 1; j >= 0; j--) { if (acc[j] != m[j]) { ge = acc[j] > m[j]; break; } if (j == 0) ge = 1; } }
        c = 0;
        for (j = 0; j < NL; j++) { u64 v = (u64)acc[j] - (m[j] & (u32)(0 - ge)) - c; acc[j] = (u32)v; c = (v >> 32) ? 1 : 0; }
        acc[NL] -= (u32)c;
    }
    memcpy(r, acc, NL * 4);
}

static void fld_init(void)
{
    fe one, t;
    int i;
    if (g_fld_ready) return;
    fe_from_be(FP, P256_P);
    fe_from_be(FR, P256_R);
    fe_zero(one); one[0] = 1;

    /* n0 = -p^-1 mod 2^32, by Newton iteration on the low limb. */
    { u32 y = 1; for (i = 0; i < 5; i++) y *= 2u - FP[0] * y; FP_N0 = (u32)(0 - y); }

    /* R mod p = 2^256 mod p, and R^2 mod p, both by repeated doubling from 1.
     * Deriving them beats transcribing them: a mistyped Montgomery constant
     * produces arithmetic that is wrong only for some inputs. */
    fe_copy(MONT1, one);
    for (i = 0; i < 256; i++) fe_add(MONT1, MONT1, MONT1, FP);
    fe_copy(MONT_R2, MONT1);
    for (i = 0; i < 256; i++) fe_add(MONT_R2, MONT_R2, MONT_R2, FP);

    /* exponents: (p+1)/4, (p-1)/2, p-2.  p ends in 0xffffffff and p > 2^255,
     * so p+1 does not overflow 256 bits and p-1 needs no borrow. */
    { fe pp1; u32 carry = 1;
      for (i = 0; i < NL; i++) { u64 v = (u64)FP[i] + carry; pp1[i] = (u32)v; carry = (u32)(v >> 32); }
      for (i = 0; i < NL; i++) E_SQRT[i] = (pp1[i] >> 2) | (i + 1 < NL ? (pp1[i + 1] << 30) : 0); }
    { fe pm1; fe_copy(pm1, FP); pm1[0] -= 1;
      for (i = 0; i < NL; i++) E_LEG[i] = (pm1[i] >> 1) | (i + 1 < NL ? (pm1[i + 1] << 31) : 0); }
    fe_copy(E_INV, FP); E_INV[0] -= 2;

    /* curve constants in Montgomery form: b, a = p-3, z = p-10 */
    fe_from_be(t, P256_B);       to_mont(MB, t);
    { fe three; fe_zero(three); three[0] = 3; fe_sub(t, FP, three, FP); to_mont(MA, t); }
    { fe ten;   fe_zero(ten);   ten[0]   = 10; fe_sub(t, FP, ten, FP);  to_mont(MZ, t); }

    /* SSWU constants: c1 = b/(z*a), c2 = -b/a */
    { fe za, inv;
      mont_mul(za, MZ, MA); mont_inv(inv, za); mont_mul(M_SSWU_C1, MB, inv); }
    { fe inv, nb;
      mont_inv(inv, MA);
      fe_sub(nb, FP, MB, FP);                        /* -b */
      mont_mul(M_SSWU_C2, nb, inv); }
    g_fld_ready = 1;
}

/* ===========================================================================
 * 2. hashes: HMAC-SHA256, the RFC 5869 HKDF, and the 802.11 KDF
 * ======================================================================== */

static void hmac256(const u8 *key, int klen, const u8 *m1, int l1,
                    const u8 *m2, int l2, u8 out[32])
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha256_vtable, key, (size_t)klen);
    br_hmac_init(&hc, &kc, 0);
    if (l1) br_hmac_update(&hc, m1, (size_t)l1);
    if (l2) br_hmac_update(&hc, m2, (size_t)l2);
    br_hmac_out(&hc, out);
}

/* HKDF-Extract(salt, ikm) = HMAC-SHA256(salt, ikm) */
static void hkdf_extract(const u8 *salt, int slen, const u8 *ikm, int ilen, u8 prk[32])
{ hmac256(salt, slen, ikm, ilen, 0, 0, prk); }

/* HKDF-Expand(prk, info, L) - RFC 5869, SHA-256, L <= 255*32. */
static void hkdf_expand(const u8 prk[32], const char *info, u8 *out, int outlen)
{
    br_hmac_key_context kc;
    u8 t[32];
    int pos = 0, tlen = 0;
    u8 ctr = 1;
    int ilen = (int)strlen(info);
    br_hmac_key_init(&kc, &br_sha256_vtable, prk, 32);
    while (pos < outlen) {
        br_hmac_context hc;
        int n;
        br_hmac_init(&hc, &kc, 0);
        if (tlen) br_hmac_update(&hc, t, (size_t)tlen);
        br_hmac_update(&hc, info, (size_t)ilen);
        br_hmac_update(&hc, &ctr, 1);
        br_hmac_out(&hc, t);
        tlen = 32; ctr++;
        n = outlen - pos; if (n > 32) n = 32;
        memcpy(out + pos, t, (size_t)n); pos += n;
    }
}

/* ===========================================================================
 * 3. the two password-element derivations
 * ======================================================================== */

/* Order the two MAC addresses as SAE requires: MAX || MIN. */
static void mac_pair(const u8 a[6], const u8 b[6], u8 out[12])
{
    const u8 *hi = memcmp(a, b, 6) >= 0 ? a : b;
    const u8 *lo = (hi == a) ? b : a;
    memcpy(out, hi, 6); memcpy(out + 6, lo, 6);
}

/* Given x (Montgomery form) known to be a valid abscissa, finish the point:
 * y = sqrt(x^3+ax+b), flipped so that LSB(y) == want_lsb.  Writes the 65-byte
 * uncompressed encoding. */
static void finish_point(const u32 *mx, int want_lsb, u8 pwe[65])
{
    fe gx, my, ny, nx;
    curve_rhs(gx, mx);
    mont_sqrt(my, gx);
    from_mont(ny, my);
    if ((int)(ny[0] & 1) != want_lsb) { fe_sub(my, FP, my, FP); from_mont(ny, my); }
    from_mont(nx, mx);
    pwe[0] = 0x04;
    fe_to_be(pwe + 1, nx);
    fe_to_be(pwe + 33, ny);
}

/* ---- hunting-and-pecking (802.11-2020 12.4.4.3.3) ----------------------- */
static int pwe_hunt_peck(const char *password,
                         const u8 a[6], const u8 b[6], u8 pwe[65])
{
    u8 macs[12], seed[32], value[32];
    fe mx, x, gx, savex;
    int counter, found = 0, save_lsb = 0;
    int plen = (int)strlen(password);
    const char *pw = password;
    /* The dummy the loop switches to once a candidate is found, so that the
     * remaining iterations do the same work on a different secret.  Its exact
     * value is irrelevant; that it is a DIFFERENT length from the real
     * password is deliberate - hostapd does the same. */
    static const char dummy[] = "UnoDOS SAE dummy password";

    mac_pair(a, b, macs);
    fe_zero(savex);
    /* 40 iterations unconditionally: stopping at the first success is exactly
     * the leak Dragonblood turned into a password oracle. */
    for (counter = 1; counter <= 40; counter++) {
        u8 c = (u8)counter;
        u8 pbuf[128];
        int n = plen; if (n > (int)sizeof pbuf - 1) n = (int)sizeof pbuf - 1;
        memcpy(pbuf, pw, (size_t)n); pbuf[n] = c;
        /* pwd-seed = H(MAX(A,B) || MIN(A,B), password || counter) */
        hmac256(macs, 12, pbuf, n + 1, 0, 0, seed);
        /* pwd-value = KDF-256(pwd-seed, "SAE Hunting and Pecking", p) */
        wpa_kdf_sha256(seed, 32, "SAE Hunting and Pecking", P256_P, 32, value, 256);
        fe_from_be(x, value);
        if (fe_cmp(x, FP) >= 0) continue;         /* not a field element      */
        to_mont(mx, x);
        curve_rhs(gx, mx);
        if (mont_is_qr(gx) && !found) {
            fe_copy(savex, mx);
            save_lsb = seed[31] & 1;
            found = 1;
            pw = dummy; plen = (int)sizeof dummy - 1;
        }
    }
    memset(seed, 0, sizeof seed);
    if (!found) return SAE_EINVAL;                /* ~2^-40; retry is useless */
    finish_point(savex, save_lsb, pwe);
    return SAE_OK;
}

/* ---- hash-to-element (802.11-2020 12.4.4.3.2) --------------------------- */
/* Simplified Shallue-van de Woestijne-Ulas: map a field element onto the
 * curve in constant time.  z = -10 for group 19. */
static void sswu(const u8 uval[32], u8 point[65])
{
    fe u, mu, mu2, zu2, t1, m, t, x1, x2, gx1, gx2, x, v, y, ny, nx, one_m;
    int l;

    fe_from_be(u, uval);
    to_mont(mu, u);
    mont_sqr(mu2, mu);                   /* u^2                     */
    mont_mul(zu2, MZ, mu2);              /* z*u^2                   */
    mont_sqr(t1, zu2);                   /* z^2*u^4                 */
    fe_add(m, t1, zu2, FP);              /* m = z^2u^4 + zu^2       */
    mont_inv(t, m);                      /* inverse(m); 0 when m==0 */

    fe_copy(one_m, MONT1);
    fe_add(t1, one_m, t, FP);            /* 1 + t                   */
    mont_mul(x1, M_SSWU_C2, t1);         /* (-b/a)*(1+t)            */
    if (fe_is_zero(m)) fe_copy(x1, M_SSWU_C1);   /* the m==0 branch: b/(z*a) */

    curve_rhs(gx1, x1);
    mont_mul(x2, zu2, x1);
    curve_rhs(gx2, x2);

    l = mont_is_qr(gx1);
    if (l) { fe_copy(v, gx1); fe_copy(x, x1); }
    else   { fe_copy(v, gx2); fe_copy(x, x2); }

    mont_sqrt(y, v);
    from_mont(ny, y);
    if ((ny[0] & 1) != (u[0] & 1)) { fe_sub(y, FP, y, FP); from_mont(ny, y); }
    from_mont(nx, x);
    point[0] = 0x04;
    fe_to_be(point + 1, nx);
    fe_to_be(point + 33, ny);
}

/* PT = SSWU(u1) + SSWU(u2), a value that depends only on (ssid, password). */
static int pwe_h2e_pt(const char *ssid, int ssid_len, const char *password,
                      u8 pt[65])
{
    const br_ec_impl *ec = br_ec_get_default();
    u8 prk[32], wide[48], u1[32], u2[32], p1[65], p2[65];
    static const u8 one[1] = { 1 };
    fe m;

    /* pwd-seed = HKDF-Extract(ssid, password) */
    hkdf_extract((const u8 *)ssid, ssid_len,
                 (const u8 *)password, (int)strlen(password), prk);
    /* len = olen(p) + ceil(olen(p)/2) = 48; u = pwd-value mod p */
    hkdf_expand(prk, "SAE Hash to Element u1 P1", wide, 48);
    mp_mod_be(m, wide, 48, FP); fe_to_be(u1, m);
    hkdf_expand(prk, "SAE Hash to Element u2 P2", wide, 48);
    mp_mod_be(m, wide, 48, FP); fe_to_be(u2, m);
    memset(prk, 0, sizeof prk); memset(wide, 0, sizeof wide);

    sswu(u1, p1);
    sswu(u2, p2);
    /* PT = P1 + P2, as 1*P1 + 1*P2.  BearSSL's muladd handles the P1==P2
     * doubling case internally and reports infinity as an error. */
    if (!ec->muladd(p1, p2, 65, one, 1, one, 1, BR_EC_secp256r1)) return SAE_EINVAL;
    memcpy(pt, p1, 65);
    return SAE_OK;
}

/* PWE = scalar-op(val, PT) where val = H(0^32, MAX||MIN) mod (n-1) + 1. */
static int pwe_h2e(const char *ssid, int ssid_len, const char *password,
                   const u8 a[6], const u8 b[6], u8 pwe[65])
{
    const br_ec_impl *ec = br_ec_get_default();
    u8 zero[32], macs[12], val[32], valbe[32];
    fe v, rm1, one;
    int rc = pwe_h2e_pt(ssid, ssid_len, password, pwe);
    if (rc != SAE_OK) return rc;
    memset(zero, 0, sizeof zero);
    mac_pair(a, b, macs);
    hmac256(zero, 32, macs, 12, 0, 0, val);
    fe_zero(one); one[0] = 1;
    fe_sub(rm1, FR, one, FR);                    /* n - 1 */
    mp_mod_be(v, val, 32, rm1);
    fe_add(v, v, one, FR);                       /* +1: never zero */
    fe_to_be(valbe, v);
    if (!ec->mul(pwe, 65, valbe, 32, BR_EC_secp256r1)) return SAE_EINVAL;
    return SAE_OK;
}

int sae_pwe_for_test(const char *ssid, int ssid_len, const char *password,
                     const u8 a[6], const u8 b[6], int h2e, u8 pwe_out[65])
{
    fld_init();
    return h2e ? pwe_h2e(ssid, ssid_len, password, a, b, pwe_out)
               : pwe_hunt_peck(password, a, b, pwe_out);
}

/* ===========================================================================
 * 4. the exchange
 * ======================================================================== */

/* A scalar in [2, n-1], by rejection sampling.  Rejection is the right shape
 * here: n is within 2^-32 of 2^256, so a draw is rejected with probability
 * about 2^-32 and "reduce a wider draw" would only add a division we do not
 * otherwise need. */
static int rand_scalar(u8 out[32])
{
    int tries;
    for (tries = 0; tries < 16; tries++) {
        fe v, two;
        if (!tls_entropy_get(out, 32)) return SAE_ENOENTROPY;
        fe_from_be(v, out);
        fe_zero(two); two[0] = 2;
        if (fe_cmp(v, FR) < 0 && fe_cmp(v, two) >= 0) return SAE_OK;
    }
    return SAE_ENOENTROPY;
}

void sae_clear(sae_t *s) { memset(s, 0, sizeof *s); }

int sae_init(sae_t *s, const char *ssid, int ssid_len, const char *password,
             const u8 own_mac[6], const u8 ap_mac[6], int h2e)
{
    const br_ec_impl *ec;
    u8 pt[65];
    fe rv, mv, sc;
    int rc;

    fld_init();
    ec = br_ec_get_default();
    memset(s, 0, sizeof *s);
    memcpy(s->spa, own_mac, 6);
    memcpy(s->aa, ap_mac, 6);
    s->h2e = h2e ? 1 : 0;

    rc = h2e ? pwe_h2e(ssid, ssid_len, password, own_mac, ap_mac, s->pwe)
             : pwe_hunt_peck(password, own_mac, ap_mac, s->pwe);
    if (rc != SAE_OK) { sae_clear(s); s->state = SAE_ST_FAILED; return rc; }

    /* rand and mask are THE secrets of the exchange; a predictable pair hands
     * an eavesdropper the PMK.  No entropy source means no association. */
    do {
        rc = rand_scalar(s->rnd);
        if (rc != SAE_OK) { sae_clear(s); s->state = SAE_ST_FAILED; return rc; }
        rc = rand_scalar(s->mask);
        if (rc != SAE_OK) { sae_clear(s); s->state = SAE_ST_FAILED; return rc; }
        /* commit-scalar = (rand + mask) mod n, and it must exceed 1. */
        fe_from_be(rv, s->rnd);
        fe_from_be(mv, s->mask);
        fe_add(sc, rv, mv, FR);
    } while (fe_le_one(sc));
    fe_to_be(s->own_scalar, sc);

    /* commit-element = inverse(mask * PWE): the same point negated. */
    memcpy(pt, s->pwe, 65);
    if (!ec->mul(pt, 65, s->mask, 32, BR_EC_secp256r1))
        { sae_clear(s); s->state = SAE_ST_FAILED; return SAE_EINVAL; }
    {
        fe y, ny;
        fe_from_be(y, pt + 33);
        fe_sub(ny, FP, y, FP);
        memcpy(s->own_elem, pt + 1, 32);
        fe_to_be(s->own_elem + 32, ny);
    }
    s->state = SAE_ST_INIT;
    s->send_confirm = 0;
    return SAE_OK;
}

int sae_commit_status(const sae_t *s) { return s->h2e ? 126 : 0; }

int sae_build_commit(sae_t *s, u8 *out, int cap)
{
    int n = 0;
    if (cap < 2 + s->token_len + 96) return SAE_EINVAL;
    out[n++] = SAE_GROUP_P256; out[n++] = 0;          /* group id, LE16 */
    if (s->token_len) { memcpy(out + n, s->token, (size_t)s->token_len); n += s->token_len; }
    memcpy(out + n, s->own_scalar, 32); n += 32;
    memcpy(out + n, s->own_elem, 64);   n += 64;
    return n;
}

int sae_set_token(sae_t *s, const u8 *tok, int len)
{
    if (len < 0 || len > (int)sizeof s->token) return SAE_EINVAL;
    memcpy(s->token, tok, (size_t)len);
    s->token_len = len;
    return SAE_OK;
}

int sae_token_from_reject(sae_t *s, const u8 *body, int len)
{
    if (len < 2) return SAE_EINVAL;
    if (body[0] != SAE_GROUP_P256 || body[1] != 0) return SAE_EGROUP;
    return sae_set_token(s, body + 2, len - 2);
}

int sae_rx_commit(sae_t *s, const u8 *body, int len)
{
    const br_ec_impl *ec = br_ec_get_default();
    static const u8 one[1] = { 1 };
    u8 kpt[65], k[32], keyseed[32], ctx[32], out[64], zero[32];
    fe ps, pe_x, pe_y, own, sum;

    if (s->state != SAE_ST_INIT) return SAE_ESTATE;
    if (len < 2 + 96) return SAE_EINVAL;
    if (body[0] != SAE_GROUP_P256 || body[1] != 0) return SAE_EGROUP;
    memcpy(s->peer_scalar, body + 2, 32);
    memcpy(s->peer_elem,   body + 34, 64);

    /* Validate before doing any arithmetic with it.  1 < scalar < n, both
     * element coordinates below p, and the element not the identity; being ON
     * the curve is checked by BearSSL's decode inside muladd below. */
    fe_from_be(ps, s->peer_scalar);
    { fe two; fe_zero(two); two[0] = 2;
      if (fe_cmp(ps, two) < 0 || fe_cmp(ps, FR) >= 0) return SAE_EREJECT; }
    fe_from_be(pe_x, s->peer_elem);
    fe_from_be(pe_y, s->peer_elem + 32);
    if (fe_cmp(pe_x, FP) >= 0 || fe_cmp(pe_y, FP) >= 0) return SAE_EREJECT;
    if (fe_is_zero(pe_x) && fe_is_zero(pe_y)) return SAE_EREJECT;
    /* Reflection attack: an attacker who simply echoes our own commit back at
     * us would otherwise have us complete a handshake with ourselves. */
    if (!memcmp(s->peer_scalar, s->own_scalar, 32) &&
        !memcmp(s->peer_elem, s->own_elem, 64)) return SAE_EREJECT;

    /* K = rand * (peer-scalar * PWE + PEER-ELEMENT); k = K.x */
    memcpy(kpt, s->pwe, 65);
    { u8 pe[65]; pe[0] = 0x04; memcpy(pe + 1, s->peer_elem, 64);
      if (!ec->muladd(kpt, pe, 65, s->peer_scalar, 32, one, 1, BR_EC_secp256r1))
          return SAE_EREJECT; }
    if (!ec->mul(kpt, 65, s->rnd, 32, BR_EC_secp256r1)) return SAE_EREJECT;
    memcpy(k, kpt + 1, 32);
    memset(kpt, 0, sizeof kpt);

    /* keyseed = H(<0>32, k); KCK || PMK = KDF-512(keyseed, "SAE KCK and PMK",
     * (commit-scalar + peer-commit-scalar) mod n) */
    memset(zero, 0, sizeof zero);
    hmac256(zero, 32, k, 32, 0, 0, keyseed);
    memset(k, 0, sizeof k);
    fe_from_be(own, s->own_scalar);
    fe_add(sum, own, ps, FR);
    fe_to_be(ctx, sum);
    wpa_kdf_sha256(keyseed, 32, "SAE KCK and PMK", ctx, 32, out, 512);
    memset(keyseed, 0, sizeof keyseed);
    memcpy(s->kck, out, 32);
    memcpy(s->pmk, out + 32, 32);
    memset(out, 0, sizeof out);
    /* PMKID = L((commit-scalar + peer-commit-scalar) mod n, 0, 128) */
    memcpy(s->pmkid, ctx, 16);
    s->state = SAE_ST_COMMITTED;
    return SAE_OK;
}

/* CN(key, counter, s1, e1, s2, e2) = HMAC-SHA256 over the concatenation. */
static void sae_cn(const u8 kck[32], u16 counter,
                   const u8 *s1, const u8 *e1, const u8 *s2, const u8 *e2,
                   u8 out[32])
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    u8 c[2];
    c[0] = (u8)counter; c[1] = (u8)(counter >> 8);      /* little-endian */
    br_hmac_key_init(&kc, &br_sha256_vtable, kck, 32);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, c, 2);
    br_hmac_update(&hc, s1, 32);
    br_hmac_update(&hc, e1, 64);
    br_hmac_update(&hc, s2, 32);
    br_hmac_update(&hc, e2, 64);
    br_hmac_out(&hc, out);
}

int sae_build_confirm(sae_t *s, u8 *out, int cap)
{
    if (s->state != SAE_ST_COMMITTED && s->state != SAE_ST_CONFIRMED) return SAE_ESTATE;
    if (cap < 34) return SAE_EINVAL;
    out[0] = (u8)s->send_confirm; out[1] = (u8)(s->send_confirm >> 8);
    sae_cn(s->kck, s->send_confirm,
           s->own_scalar, s->own_elem, s->peer_scalar, s->peer_elem, out + 2);
    if (s->state == SAE_ST_COMMITTED) s->state = SAE_ST_CONFIRMED;
    return 34;
}

int sae_rx_confirm(sae_t *s, const u8 *body, int len)
{
    u8 want[32];
    u16 psc;
    int diff, i;
    if (s->state != SAE_ST_CONFIRMED && s->state != SAE_ST_COMMITTED) return SAE_ESTATE;
    if (len < 34) return SAE_EINVAL;
    psc = (u16)(body[0] | (body[1] << 8));
    sae_cn(s->kck, psc,
           s->peer_scalar, s->peer_elem, s->own_scalar, s->own_elem, want);
    /* Constant-time compare: a byte-at-a-time memcmp on a MAC turns a forgery
     * attempt into a 256-guess-per-byte search. */
    for (diff = 0, i = 0; i < 32; i++) diff |= want[i] ^ body[2 + i];
    memset(want, 0, sizeof want);
    if (diff) { s->state = SAE_ST_FAILED; return SAE_EREJECT; }
    s->state = SAE_ST_ACCEPTED;
    return SAE_OK;
}
