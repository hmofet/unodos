/* ===========================================================================
 * unojs double-precision math.
 *
 * unojs promises to be freestanding, and UnoDOS makes that promise load-bearing:
 * pc64's libm is FLOAT only (floorf/powf/...), and float precision is not
 * enough - the number formatter round-trips values through powers of ten, so a
 * 7-digit pow() would print 0.1 as 0.100000001. Rather than depend on a host
 * libm that differs between the test build and the OS build, unojs carries its
 * own. One implementation, identical behaviour everywhere, which is what makes
 * the host golden tests mean anything on metal.
 *
 * Accuracy targets: floor/ceil/fabs/fmod exact; sqrt correctly rounded for
 * perfect squares and ~1ulp elsewhere; integer powers exact via binary
 * exponentiation; exp/log ~1e-15 relative; sin/cos/tan/atan good to ~1e-14
 * over the ranges a document will ever ask for.
 *
 * Define UJS_USE_LIBM to fall back to the C library instead (useful only for
 * differential testing against glibc).
 * ======================================================================== */
#include "ujs_int.h"

#ifndef UJS_USE_LIBM

#define INF_D  (1.0e308 * 10.0)
#define NAN_D  (INF_D - INF_D)

static int is_nan(double x) { return x != x; }
static int is_inf(double x) { return x > 1.7976931348623157e308 || x < -1.7976931348623157e308; }

double ujs_fabs(double x) { return x < 0 ? -x : x; }

double ujs_floor(double x)
{
    double t;
    if (is_nan(x) || is_inf(x) || x == 0) return x;
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0) return x;  /* already integral */
    t = (double)(long long)x;                 /* truncates toward zero */
    return (x < 0 && t != x) ? t - 1.0 : t;
}

double ujs_ceil(double x)
{
    double t;
    if (is_nan(x) || is_inf(x) || x == 0) return x;
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0) return x;
    t = (double)(long long)x;
    return (x > 0 && t != x) ? t + 1.0 : t;
}

double ujs_fmod(double x, double y)
{
    double r;
    if (is_nan(x) || is_nan(y) || is_inf(x) || y == 0) return NAN_D;
    if (is_inf(y)) return x;
    r = x - y * (double)(long long)(x / y);
    /* the quotient may not fit a long long; fall back to repeated scaling */
    if (ujs_fabs(x / y) >= 9007199254740992.0) {
        double a = ujs_fabs(x), b = ujs_fabs(y);
        while (a >= b) {
            double s = b, prev = b;
            while (s <= a / 2.0) { prev = s; s *= 2.0; if (s == prev) break; }
            a -= s;
        }
        r = x < 0 ? -a : a;
    }
    return r;
}

double ujs_sqrt(double x)
{
    double r, last;
    int i;
    if (is_nan(x) || x < 0) return NAN_D;
    if (x == 0 || is_inf(x)) return x;
    /* seed from the exponent: halve it, which lands within a factor of ~2 */
    {   u64 b;
        memcpy(&b, &x, 8);
        b = (b >> 1) + (0x1FF80000ULL << 32);     /* classic bit-hack seed */
        memcpy(&r, &b, 8);
    }
    for (i = 0; i < 6; i++) {                     /* Newton: quadratic */
        last = r;
        r = 0.5 * (r + x / r);
        if (r == last) break;
    }
    /* nail perfect squares: Math.sqrt(16) must be exactly 4 */
    {   double n = ujs_floor(r + 0.5);
        if (n * n == x) return n; }
    /* Newton in plain doubles can still land 1ulp off, and that is VISIBLE:
     * Math.sqrt(2) would print 1.414213562373095 where every other engine
     * prints 1.4142135623730951. One more step using an EXACT residual fixes
     * it. r*r is computed in two pieces by Dekker's split so that (x - r*r)
     * is not swamped by its own rounding error. */
    {   const double SPLIT = 134217729.0;         /* 2^27 + 1 */
        double c = SPLIT * r, hi = c - (c - r), lo = r - hi;
        double p = r * r;
        double e = ((hi * hi - p) + 2.0 * hi * lo) + lo * lo;
        double resid = (x - p) - e;
        r += resid / (2.0 * r);
    }
    return r;
}

/* exp: range-reduce to [-ln2/2, ln2/2] then a Taylor series. */
static const double LN2   = 0.69314718055994530942;
static const double LOG2E = 1.44269504088896340736;

double ujs_exp(double x)
{
    int k, i;
    double r, term, sum, two_k;
    if (is_nan(x)) return x;
    if (x > 709.78) return INF_D;
    if (x < -745.2) return 0.0;
    k = (int)ujs_floor(x * LOG2E + 0.5);
    r = x - (double)k * LN2;
    sum = 1.0; term = 1.0;
    for (i = 1; i < 18; i++) {
        term *= r / (double)i;
        sum += term;
        if (term < 1e-18 && term > -1e-18) break;
    }
    two_k = 1.0;
    if (k > 0)      { for (i = 0; i < k; i++) { two_k *= 2.0; if (is_inf(two_k)) return INF_D; } }
    else if (k < 0) { for (i = 0; i < -k; i++) two_k *= 0.5; }
    return sum * two_k;
}

/* log: reduce to the mantissa in [1,2) via the exponent field, then atanh. */
double ujs_log(double x)
{
    int e = 0;
    double m, z, z2, sum, p;
    int i;
    if (is_nan(x) || x < 0) return NAN_D;
    if (x == 0) return -INF_D;
    if (is_inf(x)) return x;
    {   u64 b;
        double scaled_extra = 0.0;
        memcpy(&b, &x, 8);
        if (((b >> 52) & 0x7FF) == 0) {           /* subnormal: no implicit 1 */
            x *= 18446744073709551616.0;          /* 2^64 */
            scaled_extra = -64.0 * LN2;
            memcpy(&b, &x, 8);
            e = (int)((b >> 52) & 0x7FF) - 1023;
            b = (b & 0x800FFFFFFFFFFFFFULL) | (1023ULL << 52);
            memcpy(&m, &b, 8);
            if (m > 1.4142135623730951) { m *= 0.5; e++; }
            z = (m - 1.0) / (m + 1.0); z2 = z * z; sum = 0.0; p = z;
            for (i = 1; i < 40; i += 2) {
                double t = p / (double)i;
                sum += t;
                if (t < 1e-19 && t > -1e-19) break;
                p *= z2;
            }
            return 2.0 * sum + (double)e * LN2 + scaled_extra;
        }
        e = (int)((b >> 52) & 0x7FF) - 1023;
        b = (b & 0x800FFFFFFFFFFFFFULL) | (1023ULL << 52);   /* mantissa in [1,2) */
        memcpy(&m, &b, 8);
    }
    if (m > 1.4142135623730951) { m *= 0.5; e++; }            /* centre on 1 */
    z = (m - 1.0) / (m + 1.0);                                /* |z| small   */
    z2 = z * z;
    sum = 0.0; p = z;
    for (i = 1; i < 40; i += 2) {
        double t = p / (double)i;
        sum += t;
        if (t < 1e-19 && t > -1e-19) break;
        p *= z2;
    }
    return 2.0 * sum + (double)e * LN2;
}

double ujs_pow(double x, double y)
{
    if (y == 0) return 1.0;                        /* including pow(NaN, 0) */
    if (is_nan(x) || is_nan(y)) return NAN_D;
    if (y == 1.0) return x;
    /* Integer exponents take an EXACT binary-exponentiation path: Math.pow(2,10)
     * must be 1024 on the nose, and the number formatter leans on exact powers
     * of ten. exp(y*log(x)) would give 1023.9999999999999. */
    if (y == ujs_floor(y) && ujs_fabs(y) <= 1024.0) {
        double base = x, acc = 1.0;
        long long n = (long long)(y < 0 ? -y : y);
        while (n) {
            if (n & 1) acc *= base;
            base *= base;
            n >>= 1;
            if (is_inf(acc)) break;
        }
        return y < 0 ? 1.0 / acc : acc;
    }
    if (x == 0) return y > 0 ? 0.0 : INF_D;
    if (x < 0) return NAN_D;                       /* non-integer power of a negative */
    return ujs_exp(y * ujs_log(x));
}

/* sin/cos: reduce mod pi/2 into an octant, then a Taylor series. */
static const double PI_2 = 1.57079632679489661923;

static void sincos_core(double r, double *s, double *c)
{
    double r2 = r * r, term, sum;
    int i;
    sum = r; term = r;
    for (i = 1; i < 12; i++) {
        term *= -r2 / (double)((2*i) * (2*i + 1));
        sum += term;
    }
    *s = sum;
    sum = 1.0; term = 1.0;
    for (i = 1; i < 12; i++) {
        term *= -r2 / (double)((2*i - 1) * (2*i));
        sum += term;
    }
    *c = sum;
}

static void sincos(double x, double *sout, double *cout)
{
    double q, r, s, c;
    int k;
    if (is_nan(x) || is_inf(x)) { *sout = NAN_D; *cout = NAN_D; return; }
    q = ujs_floor(x / PI_2 + 0.5);
    r = x - q * PI_2;
    k = (int)ujs_fmod(q, 4.0);
    if (k < 0) k += 4;
    sincos_core(r, &s, &c);
    switch (k) {
    case 0: *sout =  s; *cout =  c; break;
    case 1: *sout =  c; *cout = -s; break;
    case 2: *sout = -s; *cout = -c; break;
    default:*sout = -c; *cout =  s; break;
    }
}

double ujs_sin(double x) { double s, c; sincos(x, &s, &c); return s; }
double ujs_cos(double x) { double s, c; sincos(x, &s, &c); return c; }
double ujs_tan(double x) { double s, c; sincos(x, &s, &c); return c == 0 ? INF_D : s / c; }

/* atan via argument reduction onto [0, tan(pi/12)] then a series. */
double ujs_atan(double x)
{
    int neg = 0, inv = 0;
    double add = 0.0, z, z2, sum, p;
    int i;
    if (is_nan(x)) return x;
    if (is_inf(x)) return x > 0 ? PI_2 : -PI_2;
    if (x < 0) { x = -x; neg = 1; }
    if (x > 1.0) { x = 1.0 / x; inv = 1; }
    if (x > 0.26794919243112270647) {              /* tan(pi/12) */
        /* atan(x) = pi/6 + atan((x*sqrt3 - 1)/(x + sqrt3)) */
        const double SQRT3 = 1.73205080756887729353;
        add = PI_2 / 3.0;                          /* pi/6 */
        x = (x * SQRT3 - 1.0) / (x + SQRT3);
    }
    z = x; z2 = z * z; sum = 0.0; p = z;
    for (i = 1; i < 60; i += 2) {
        double t = p / (double)i;
        sum += (i % 4 == 1) ? t : -t;
        if (t < 1e-19 && t > -1e-19) break;
        p *= z2;
    }
    sum += add;
    if (inv) sum = PI_2 - sum;
    return neg ? -sum : sum;
}

double ujs_atan2(double y, double x)
{
    const double PI = 3.14159265358979323846;
    if (is_nan(x) || is_nan(y)) return NAN_D;
    if (x == 0) return y > 0 ? PI_2 : y < 0 ? -PI_2 : 0.0;
    {   double a = ujs_atan(y / x);
        if (x > 0) return a;
        return y >= 0 ? a + PI : a - PI; }
}

double ujs_log10(double x) { return ujs_log(x) * 0.43429448190325182765; }

#endif /* !UJS_USE_LIBM */
