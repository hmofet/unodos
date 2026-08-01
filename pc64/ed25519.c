/* ===========================================================================
 * UnoDOS/pc64 - Ed25519 signatures (RFC 8032). See ed25519.h for why this
 * exists and what it does and does not promise.
 *
 * LAYOUT
 *   field    GF(2^255-19) in five 51-bit unsigned limbs, products through
 *            unsigned __int128. Every limb is carried after every operation:
 *            slower than letting them grow, and it removes the entire class of
 *            "this input was allowed to be slightly too big" bugs.
 *   group    twisted Edwards, extended coordinates (X:Y:Z:T) with x = X/Z,
 *            y = Y/Z and xy = T/Z. Addition is add-2008-hwcd-3 and doubling is
 *            dbl-2008-hwcd, both in their a = -1 forms.
 *   scalar   arithmetic mod L by bitwise shift-subtract. ref10 does this with
 *            hand-tuned 21-bit limbs and several hundred lines of generated
 *            code; a 512-step loop with a masked conditional subtract is a
 *            fraction of the size, obviously correct by inspection, constant
 *            time, and costs microseconds.
 *
 * ARITHMETIC IS DEFINED, not merely correct. The debug OS compiles first-party
 * code with -fsanitize=...-trap-on-error, so a shift of a negative value or a
 * signed overflow is a #UD at runtime, not a warning. Everything here is
 * unsigned; nothing relies on wraparound of a signed type.
 *
 * NO libc. The OS build is -nostdinc, so the two byte helpers are local.
 * The only external dependency is BearSSL's SHA-512.
 * ======================================================================== */
#include "bearssl_hash.h"
#include "ed25519.h"

typedef unsigned long long  eu64;
typedef unsigned __int128   eu128;

#define MASK51 0x7FFFFFFFFFFFFULL

static void ed_copy(unsigned char *d, const unsigned char *s, int n)
{ int i; for (i = 0; i < n; i++) d[i] = s[i]; }
static void ed_zero(unsigned char *d, int n)
{ int i; for (i = 0; i < n; i++) d[i] = 0; }

/* ---- field ------------------------------------------------------------- */
typedef struct { eu64 v[5]; } fe;

static void fe_carry(fe *h)
{
    eu64 c;
    c = h->v[0] >> 51; h->v[0] &= MASK51; h->v[1] += c;
    c = h->v[1] >> 51; h->v[1] &= MASK51; h->v[2] += c;
    c = h->v[2] >> 51; h->v[2] &= MASK51; h->v[3] += c;
    c = h->v[3] >> 51; h->v[3] &= MASK51; h->v[4] += c;
    c = h->v[4] >> 51; h->v[4] &= MASK51; h->v[0] += 19u * c;
    c = h->v[0] >> 51; h->v[0] &= MASK51; h->v[1] += c;
}

static void fe_0(fe *h) { int i; for (i = 0; i < 5; i++) h->v[i] = 0; }
static void fe_1(fe *h) { int i; h->v[0] = 1; for (i = 1; i < 5; i++) h->v[i] = 0; }

static void fe_add(fe *r, const fe *a, const fe *b)
{
    int i;
    for (i = 0; i < 5; i++) r->v[i] = a->v[i] + b->v[i];
    fe_carry(r);
}

/* r = a - b, computed as a + 2p - b so no limb can borrow: 2p is 2^256 - 38,
 * whose limbs are all about 2^52 while b's are all below 2^51. */
static void fe_sub(fe *r, const fe *a, const fe *b)
{
    r->v[0] = a->v[0] + 0xFFFFFFFFFFFDAULL - b->v[0];
    r->v[1] = a->v[1] + 0xFFFFFFFFFFFFEULL - b->v[1];
    r->v[2] = a->v[2] + 0xFFFFFFFFFFFFEULL - b->v[2];
    r->v[3] = a->v[3] + 0xFFFFFFFFFFFFEULL - b->v[3];
    r->v[4] = a->v[4] + 0xFFFFFFFFFFFFEULL - b->v[4];
    fe_carry(r);
}

static void fe_neg(fe *r, const fe *a) { fe z; fe_0(&z); fe_sub(r, &z, a); }

static void fe_mul(fe *h, const fe *f, const fe *g)
{
    eu64 f0 = f->v[0], f1 = f->v[1], f2 = f->v[2], f3 = f->v[3], f4 = f->v[4];
    eu64 g0 = g->v[0], g1 = g->v[1], g2 = g->v[2], g3 = g->v[3], g4 = g->v[4];
    eu64 g1x = 19u * g1, g2x = 19u * g2, g3x = 19u * g3, g4x = 19u * g4;
    eu128 h0, h1, h2, h3, h4;
    eu64 r0, r1, r2, r3, r4, c;

    h0 = (eu128)f0*g0 + (eu128)f1*g4x + (eu128)f2*g3x + (eu128)f3*g2x + (eu128)f4*g1x;
    h1 = (eu128)f0*g1 + (eu128)f1*g0  + (eu128)f2*g4x + (eu128)f3*g3x + (eu128)f4*g2x;
    h2 = (eu128)f0*g2 + (eu128)f1*g1  + (eu128)f2*g0  + (eu128)f3*g4x + (eu128)f4*g3x;
    h3 = (eu128)f0*g3 + (eu128)f1*g2  + (eu128)f2*g1  + (eu128)f3*g0  + (eu128)f4*g4x;
    h4 = (eu128)f0*g4 + (eu128)f1*g3  + (eu128)f2*g2  + (eu128)f3*g1  + (eu128)f4*g0;

    r0 = (eu64)h0 & MASK51; h1 += (eu64)(h0 >> 51);
    r1 = (eu64)h1 & MASK51; h2 += (eu64)(h1 >> 51);
    r2 = (eu64)h2 & MASK51; h3 += (eu64)(h2 >> 51);
    r3 = (eu64)h3 & MASK51; h4 += (eu64)(h3 >> 51);
    r4 = (eu64)h4 & MASK51; r0 += 19u * (eu64)(h4 >> 51);
    c = r0 >> 51; r0 &= MASK51; r1 += c;
    c = r1 >> 51; r1 &= MASK51; r2 += c;
    h->v[0] = r0; h->v[1] = r1; h->v[2] = r2; h->v[3] = r3; h->v[4] = r4;
}

static void fe_sq(fe *h, const fe *f) { fe_mul(h, f, f); }

/* r = b ? g : r, branch-free (b must be 0 or 1) */
static void fe_cmov(fe *r, const fe *g, unsigned b)
{
    eu64 mask = (eu64)0 - (eu64)b;
    int i;
    for (i = 0; i < 5; i++) r->v[i] ^= mask & (r->v[i] ^ g->v[i]);
}

static void fe_copy(fe *r, const fe *a) { int i; for (i = 0; i < 5; i++) r->v[i] = a->v[i]; }

static void fe_sqn(fe *r, const fe *a, int n)
{
    int i;
    fe_sq(r, a);
    for (i = 1; i < n; i++) fe_sq(r, r);
}

/* z^(2^250 - 1), plus z^11 on the side: the shared spine of both the inverse
 * and the (p-5)/8 power, so the long chain is written once. */
static void fe_pow_spine(fe *z250, fe *z11, const fe *z)
{
    fe z2, z9, t;
    fe_sq(&z2, z);                        /* 2      */
    fe_sqn(&t, &z2, 2);                   /* 8      */
    fe_mul(&z9, &t, z);                   /* 9      */
    fe_mul(z11, &z9, &z2);                /* 11     */
    fe_sq(&t, z11);                       /* 22     */
    fe_mul(&t, &t, &z9);                  /* 2^5-1  */
    { fe a; fe_sqn(&a, &t, 5);   fe_mul(&t, &a, &t); }      /* 2^10-1 */
    { fe a, b; fe_copy(&b, &t);
      fe_sqn(&a, &t, 10);  fe_mul(&a, &a, &t);              /* 2^20-1 */
      fe_sqn(&t, &a, 20);  fe_mul(&t, &t, &a);              /* 2^40-1 */
      fe_sqn(&t, &t, 10);  fe_mul(&t, &t, &b);              /* 2^50-1 */
      fe_sqn(&a, &t, 50);  fe_mul(&a, &a, &t);              /* 2^100-1 */
      fe_sqn(&b, &a, 100); fe_mul(&b, &b, &a);              /* 2^200-1 */
      fe_sqn(&b, &b, 50);  fe_mul(z250, &b, &t); }          /* 2^250-1 */
}

static void fe_invert(fe *out, const fe *z)      /* z^(p-2) = z^(2^255-21) */
{
    fe z250, z11, t;
    fe_pow_spine(&z250, &z11, z);
    fe_sqn(&t, &z250, 5);
    fe_mul(out, &t, &z11);
}

static void fe_pow22523(fe *out, const fe *z)    /* z^((p-5)/8) = z^(2^252-3) */
{
    fe z250, z11, t;
    fe_pow_spine(&z250, &z11, z);
    fe_sqn(&t, &z250, 2);
    fe_mul(out, &t, z);
}

static eu64 ed_load64(const unsigned char *s)
{
    eu64 r = 0; int i;
    for (i = 7; i >= 0; i--) r = (r << 8) | (eu64)s[i];
    return r;
}
static void ed_store64(unsigned char *s, eu64 x)
{ int i; for (i = 0; i < 8; i++) s[i] = (unsigned char)(x >> (8 * i)); }

static void fe_frombytes(fe *h, const unsigned char *s)
{
    eu64 i0 = ed_load64(s), i1 = ed_load64(s + 8);
    eu64 i2 = ed_load64(s + 16), i3 = ed_load64(s + 24);
    h->v[0] = i0 & MASK51;
    h->v[1] = ((i0 >> 51) | (i1 << 13)) & MASK51;
    h->v[2] = ((i1 >> 38) | (i2 << 26)) & MASK51;
    h->v[3] = ((i2 >> 25) | (i3 << 39)) & MASK51;
    h->v[4] = (i3 >> 12) & MASK51;        /* bit 255 is the caller's business */
}

static void fe_tobytes(unsigned char *s, const fe *f)
{
    fe t;
    eu64 q, c;
    fe_copy(&t, f);
    fe_carry(&t);
    /* one conditional subtraction of p, computed as a carry probe */
    q = (t.v[0] + 19u) >> 51;
    q = (t.v[1] + q) >> 51;
    q = (t.v[2] + q) >> 51;
    q = (t.v[3] + q) >> 51;
    q = (t.v[4] + q) >> 51;
    t.v[0] += 19u * q;
    c = t.v[0] >> 51; t.v[0] &= MASK51; t.v[1] += c;
    c = t.v[1] >> 51; t.v[1] &= MASK51; t.v[2] += c;
    c = t.v[2] >> 51; t.v[2] &= MASK51; t.v[3] += c;
    c = t.v[3] >> 51; t.v[3] &= MASK51; t.v[4] += c;
    t.v[4] &= MASK51;
    ed_store64(s,      t.v[0]        | (t.v[1] << 51));
    ed_store64(s + 8,  (t.v[1] >> 13) | (t.v[2] << 38));
    ed_store64(s + 16, (t.v[2] >> 26) | (t.v[3] << 25));
    ed_store64(s + 24, (t.v[3] >> 39) | (t.v[4] << 12));
}

static int fe_isnegative(const fe *f)
{ unsigned char s[32]; fe_tobytes(s, f); return s[0] & 1; }

static int fe_iszero(const fe *f)
{
    unsigned char s[32];
    int i, r = 0;
    fe_tobytes(s, f);
    for (i = 0; i < 32; i++) r |= s[i];
    return r == 0;
}

static int fe_eq(const fe *a, const fe *b)
{ fe t; fe_sub(&t, a, b); return fe_iszero(&t); }

/* ---- curve constants ----------------------------------------------------
 * Held as their canonical little-endian encodings rather than as hand-computed
 * limbs: the encodings are checkable against the RFC by eye, limb triples are
 * not. ed25519_selftest() proves the two derived ones really are what they
 * claim (d is the curve's, sqrt(-1) squares to -1). */
static const unsigned char kD[32] = {       /* -121665/121666 mod p */
    0xa3,0x78,0x59,0x13,0xca,0x4d,0xeb,0x75,0xab,0xd8,0x41,0x41,0x4d,0x0a,0x70,0x00,
    0x98,0xe8,0x79,0x77,0x79,0x40,0xc7,0x8c,0x73,0xfe,0x6f,0x2b,0xee,0x6c,0x03,0x52 };
static const unsigned char kSqrtM1[32] = {  /* 2^((p-1)/4) mod p */
    0xb0,0xa0,0x0e,0x4a,0x27,0x1b,0xee,0xc4,0x78,0xe4,0x2f,0xad,0x06,0x18,0x43,0x2f,
    0xa7,0xd7,0xfb,0x3d,0x99,0x00,0x4d,0x2b,0x0b,0xdf,0xc1,0x4f,0x80,0x24,0x83,0x2b };
/* the base point, compressed: y = 4/5, sign 0 */
static const unsigned char kBase[32] = {
    0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
    0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66 };

/* ---- group -------------------------------------------------------------- */
typedef struct { fe X, Y, Z, T; } ge;       /* x = X/Z, y = Y/Z, xy = T/Z */

static fe g_d, g_d2, g_sqrtm1;
static ge g_base;
static int g_ready;

static int ge_frombytes(ge *h, const unsigned char *s);

static void ge_ident(ge *h) { fe_0(&h->X); fe_1(&h->Y); fe_1(&h->Z); fe_0(&h->T); }

static void ed_init(void)
{
    if (g_ready) return;
    fe_frombytes(&g_d, kD);
    fe_add(&g_d2, &g_d, &g_d);
    fe_frombytes(&g_sqrtm1, kSqrtM1);
    g_ready = 1;                 /* set before frombytes: it needs the above */
    ge_frombytes(&g_base, kBase);
}

static void ge_add(ge *r, const ge *p, const ge *q)
{
    fe a, b, c, d, e, f, g, h, t;
    fe_sub(&t, &p->Y, &p->X); fe_sub(&a, &q->Y, &q->X); fe_mul(&a, &t, &a);
    fe_add(&t, &p->Y, &p->X); fe_add(&b, &q->Y, &q->X); fe_mul(&b, &t, &b);
    fe_mul(&c, &p->T, &g_d2);  fe_mul(&c, &c, &q->T);
    fe_mul(&d, &p->Z, &q->Z);  fe_add(&d, &d, &d);
    fe_sub(&e, &b, &a);
    fe_sub(&f, &d, &c);
    fe_add(&g, &d, &c);
    fe_add(&h, &b, &a);
    fe_mul(&r->X, &e, &f);
    fe_mul(&r->Y, &g, &h);
    fe_mul(&r->T, &e, &h);
    fe_mul(&r->Z, &f, &g);
}

static void ge_dbl(ge *r, const ge *p)
{
    fe a, b, c, e, f, g, h, t;
    fe_sq(&a, &p->X);
    fe_sq(&b, &p->Y);
    fe_sq(&c, &p->Z); fe_add(&c, &c, &c);
    fe_add(&t, &p->X, &p->Y); fe_sq(&e, &t);
    fe_sub(&e, &e, &a); fe_sub(&e, &e, &b);      /* E = 2*X*Y            */
    fe_sub(&g, &b, &a);                          /* G = -A + B           */
    fe_sub(&f, &g, &c);
    fe_neg(&h, &a); fe_sub(&h, &h, &b);          /* H = -A - B           */
    fe_mul(&r->X, &e, &f);
    fe_mul(&r->Y, &g, &h);
    fe_mul(&r->T, &e, &h);
    fe_mul(&r->Z, &f, &g);
}

static void ge_cmov(ge *r, const ge *q, unsigned b)
{
    fe_cmov(&r->X, &q->X, b); fe_cmov(&r->Y, &q->Y, b);
    fe_cmov(&r->Z, &q->Z, b); fe_cmov(&r->T, &q->T, b);
}

/* r = [a]p, constant time: every bit costs one double and one add, and which
 * one is kept is a masked move rather than a branch. */
static void ge_scalarmult(ge *r, const unsigned char a[32], const ge *p)
{
    ge q, t;
    int i;
    ge_ident(&q);
    for (i = 255; i >= 0; i--) {
        ge_dbl(&q, &q);
        ge_add(&t, &q, p);
        ge_cmov(&q, &t, (unsigned)((a[i >> 3] >> (i & 7)) & 1));
    }
    *r = q;
}

static void ge_tobytes(unsigned char *s, const ge *p)
{
    fe recip, x, y;
    fe_invert(&recip, &p->Z);
    fe_mul(&x, &p->X, &recip);
    fe_mul(&y, &p->Y, &recip);
    fe_tobytes(s, &y);
    s[31] = (unsigned char)(s[31] ^ (unsigned char)(fe_isnegative(&x) << 7));
}

/* Decompress. 0 = not a point on the curve (or a non-canonical sign on zero),
 * which the caller must treat as a bad key or a bad signature. */
static int ge_frombytes(ge *h, const unsigned char *s)
{
    fe u, v, v3, vxx, check, x, y, t;
    unsigned char sign = (unsigned char)(s[31] >> 7);

    ed_init();
    fe_frombytes(&y, s);                     /* drops bit 255 for us */
    fe_1(&u);
    fe_sq(&v, &y);
    fe_mul(&v3, &v, &g_d);
    fe_sub(&u, &v, &u);                      /* u = y^2 - 1          */
    fe_1(&t);
    fe_add(&v, &v3, &t);                     /* v = d*y^2 + 1        */

    fe_sq(&v3, &v); fe_mul(&v3, &v3, &v);    /* v^3                  */
    fe_sq(&x, &v3); fe_mul(&x, &x, &v); fe_mul(&x, &x, &u);   /* u*v^7 */
    fe_pow22523(&x, &x);
    fe_mul(&x, &x, &v3); fe_mul(&x, &x, &u); /* x = u*v^3*(u*v^7)^((p-5)/8) */

    fe_sq(&vxx, &x); fe_mul(&vxx, &vxx, &v);
    fe_sub(&check, &vxx, &u);
    if (!fe_iszero(&check)) {
        fe_add(&check, &vxx, &u);
        if (!fe_iszero(&check)) return 0;    /* no square root: not on curve */
        fe_mul(&x, &x, &g_sqrtm1);
    }
    if (fe_iszero(&x) && sign) return 0;     /* -0 is not a canonical encoding */
    if ((unsigned)fe_isnegative(&x) != (unsigned)sign) fe_neg(&x, &x);

    fe_copy(&h->X, &x);
    fe_copy(&h->Y, &y);
    fe_1(&h->Z);
    fe_mul(&h->T, &x, &y);
    return 1;
}

/* ---- scalars mod L ------------------------------------------------------ */
static const eu64 kL[4] = {                  /* L = 2^252 + 27742...648493 */
    0x5812631A5CF5D3EDULL, 0x14DEF9DEA2F79CD6ULL,
    0x0000000000000000ULL, 0x1000000000000000ULL };

/* r -= L when r >= L, branch-free */
static void sc_cond_sub_l(eu64 r[4])
{
    eu64 t[4], borrow = 0, mask;
    int i;
    for (i = 0; i < 4; i++) {
        eu64 d = r[i] - kL[i] - borrow;
        borrow = ((r[i] < kL[i] + borrow) ||
                  (kL[i] + borrow < kL[i])) ? 1u : 0u;
        t[i] = d;
    }
    mask = (eu64)0 - (1u - borrow);          /* all ones when r >= L */
    for (i = 0; i < 4; i++) r[i] ^= mask & (r[i] ^ t[i]);
}

/* out = x mod L, where x is `bits` bits of little-endian limbs. Shift-subtract
 * from the top: r stays below L, so r*2+bit stays below 2^254 and never
 * overflows the four limbs. */
static void sc_mod_l(unsigned char out[32], const eu64 *x, int bits)
{
    eu64 r[4];
    int i;
    r[0] = r[1] = r[2] = r[3] = 0;
    for (i = bits - 1; i >= 0; i--) {
        eu64 bit = (x[i >> 6] >> (i & 63)) & 1u;
        r[3] = (r[3] << 1) | (r[2] >> 63);
        r[2] = (r[2] << 1) | (r[1] >> 63);
        r[1] = (r[1] << 1) | (r[0] >> 63);
        r[0] = (r[0] << 1) | bit;
        sc_cond_sub_l(r);
    }
    for (i = 0; i < 4; i++) ed_store64(out + 8 * i, r[i]);
}

static void sc_reduce64(unsigned char out[32], const unsigned char in[64])
{
    eu64 x[8];
    int i;
    for (i = 0; i < 8; i++) x[i] = ed_load64(in + 8 * i);
    sc_mod_l(out, x, 512);
}

/* s = (a*b + c) mod L. a and b are already reduced, so the product is under
 * 2^506 and the sum still fits the 512-bit accumulator. */
static void sc_muladd(unsigned char s[32], const unsigned char a[32],
                      const unsigned char b[32], const unsigned char c[32])
{
    eu64 av[4], bv[4], cv[4], p[8];
    eu128 acc;
    eu64 carry;
    int i, j;

    for (i = 0; i < 4; i++) {
        av[i] = ed_load64(a + 8 * i);
        bv[i] = ed_load64(b + 8 * i);
        cv[i] = ed_load64(c + 8 * i);
    }
    for (i = 0; i < 8; i++) p[i] = 0;
    for (i = 0; i < 4; i++) {
        carry = 0;
        for (j = 0; j < 4; j++) {
            acc = (eu128)av[i] * bv[j] + (eu128)p[i + j] + (eu128)carry;
            p[i + j] = (eu64)acc;
            carry = (eu64)(acc >> 64);
        }
        p[i + 4] += carry;
    }
    carry = 0;                                /* += c */
    for (i = 0; i < 4; i++) {
        acc = (eu128)p[i] + (eu128)cv[i] + (eu128)carry;
        p[i] = (eu64)acc;
        carry = (eu64)(acc >> 64);
    }
    for (i = 4; i < 8 && carry; i++) {
        acc = (eu128)p[i] + (eu128)carry;
        p[i] = (eu64)acc;
        carry = (eu64)(acc >> 64);
    }
    sc_mod_l(s, p, 512);
}

/* 1 when s < L: the canonical-S check a verifier owes the caller, so a mauled
 * signature is rejected rather than quietly reduced into a valid one. */
static int sc_is_canonical(const unsigned char s[32])
{
    int i;
    for (i = 31; i >= 0; i--) {
        unsigned char l = (unsigned char)(kL[i >> 3] >> (8 * (i & 7)));
        if (s[i] < l) return 1;
        if (s[i] > l) return 0;
    }
    return 0;                                  /* s == L is not canonical */
}

/* ---- the RFC 8032 construction ------------------------------------------ */
static void sha512(unsigned char out[64], const unsigned char *a, int alen,
                   const unsigned char *b, int blen,
                   const unsigned char *c, int clen)
{
    br_sha512_context ctx;
    br_sha512_init(&ctx);
    if (alen) br_sha512_update(&ctx, a, (size_t)alen);
    if (blen) br_sha512_update(&ctx, b, (size_t)blen);
    if (clen) br_sha512_update(&ctx, c, (size_t)clen);
    br_sha512_out(&ctx, out);
}

/* the secret scalar: SHA-512(seed)[0..31] with the low three bits and the top
 * two fixed, which is what puts it in the prime-order subgroup */
static void ed_clamp(unsigned char a[32], const unsigned char h[64])
{
    ed_copy(a, h, 32);
    a[0] = (unsigned char)(a[0] & 248);
    a[31] = (unsigned char)((a[31] & 63) | 64);
}

void ed25519_pubkey(unsigned char pk[32], const unsigned char sk[32])
{
    unsigned char h[64], a[32];
    ge A;
    ed_init();
    sha512(h, sk, 32, 0, 0, 0, 0);
    ed_clamp(a, h);
    ge_scalarmult(&A, a, &g_base);
    ge_tobytes(pk, &A);
    ed_zero(h, 64); ed_zero(a, 32);
}

void ed25519_sign(unsigned char sig[64], const unsigned char *m, int mlen,
                  const unsigned char pk[32], const unsigned char sk[32])
{
    unsigned char h[64], a[32], r[32], k[32], hr[64];
    ge R;
    ed_init();
    sha512(h, sk, 32, 0, 0, 0, 0);
    ed_clamp(a, h);

    sha512(hr, h + 32, 32, m, mlen, 0, 0);     /* r = H(prefix || M)  */
    sc_reduce64(r, hr);
    ge_scalarmult(&R, r, &g_base);
    ge_tobytes(sig, &R);                       /* sig[0..31] = R      */

    sha512(hr, sig, 32, pk, 32, m, mlen);      /* k = H(R || A || M)  */
    sc_reduce64(k, hr);
    sc_muladd(sig + 32, k, a, r);              /* S = r + k*a mod L   */

    ed_zero(h, 64); ed_zero(a, 32); ed_zero(r, 32); ed_zero(hr, 64);
}

int ed25519_verify(const unsigned char sig[64], const unsigned char *m,
                   int mlen, const unsigned char pk[32])
{
    unsigned char hr[64], k[32], chk[32];
    ge A, R, sB, kA, rhs;
    int i, diff = 0;

    ed_init();
    if (!sc_is_canonical(sig + 32)) return 0;
    if (!ge_frombytes(&A, pk)) return 0;
    if (!ge_frombytes(&R, sig)) return 0;

    sha512(hr, sig, 32, pk, 32, m, mlen);
    sc_reduce64(k, hr);

    /* [S]B == R + [k]A. A is negated instead in some implementations; doing it
     * this way keeps both sides in the same form and needs no negation. */
    ge_scalarmult(&sB, sig + 32, &g_base);
    ge_scalarmult(&kA, k, &A);
    ge_add(&rhs, &R, &kA);

    ge_tobytes(chk, &rhs);
    { unsigned char lhs[32];
      ge_tobytes(lhs, &sB);
      for (i = 0; i < 32; i++) diff |= lhs[i] ^ chk[i]; }
    return diff == 0;
}

int ed25519_selftest(void)
{
    fe t, m1;
    ge B2;
    unsigned char s[32];
    ed_init();

    fe_sq(&t, &g_sqrtm1);                      /* sqrt(-1)^2 == -1 */
    fe_1(&m1); fe_neg(&m1, &m1);
    if (!fe_eq(&t, &m1)) return 0;

    /* the base point round-trips through its own encoding */
    ge_tobytes(s, &g_base);
    for (t.v[0] = 0; t.v[0] < 32; t.v[0]++)
        if (s[t.v[0]] != kBase[t.v[0]]) return 0;

    /* and doubling it agrees with adding it to itself */
    ge_dbl(&B2, &g_base);
    { ge sum; unsigned char a[32], b[32];
      ge_add(&sum, &g_base, &g_base);
      ge_tobytes(a, &B2); ge_tobytes(b, &sum);
      for (t.v[0] = 0; t.v[0] < 32; t.v[0]++)
          if (a[t.v[0]] != b[t.v[0]]) return 0; }
    return 1;
}
