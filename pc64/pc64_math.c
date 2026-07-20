/* ===========================================================================
 * UnoDOS/pc64 - freestanding float math for uno3d (no libm).
 *
 * uno3d's portable pipeline needs sinf/cosf/tanf (rotation, projection) and
 * the software rasteriser needs floorf/ceilf. A demo-grade Taylor sine after
 * range reduction is plenty for a solid-shaded 3D corridor; sqrtf via one
 * Newton step over an SSE reciprocal-estimate-free bit hack. x86-64 has SSE,
 * which OVMF enables, so plain float ops compile to xmm.
 * ======================================================================== */
#include <math.h>

#define PI_F   3.14159265358979f
#define TWOPI  6.28318530717959f
#define HALFPI 1.57079632679490f

/* Guard the int cast: |x| beyond INT range (or NaN) is UB as (int)x, so return
   the input unchanged for those - callers only feed small demo 3D/font values,
   but a stray large/NaN value must not invoke UB. */
float floorf(float x)
{
    int i;
    if (!(x > -2147483000.0f && x < 2147483000.0f)) return x;
    i = (int)x;
    if ((float)i > x) i--;
    return (float)i;
}
float ceilf(float x)
{
    int i;
    if (!(x > -2147483000.0f && x < 2147483000.0f)) return x;
    i = (int)x;
    if ((float)i < x) i++;
    return (float)i;
}
float fabsf(float x) { return x < 0.0f ? -x : x; }

float sinf(float x)
{
    float x2, x3, x5, x7;
    /* range-reduce to [-pi, pi] */
    while (x >  PI_F) x -= TWOPI;
    while (x < -PI_F) x += TWOPI;
    x2 = x * x; x3 = x2 * x; x5 = x3 * x2; x7 = x5 * x2;
    /* Taylor: x - x^3/6 + x^5/120 - x^7/5040 */
    return x - x3 * 0.16666667f + x5 * 0.00833333f - x7 * 0.00019841f;
}
float cosf(float x) { return sinf(x + HALFPI); }

float tanf(float x)
{
    float c = cosf(x);
    if (c < 1e-6f && c > -1e-6f) c = (c < 0.0f) ? -1e-6f : 1e-6f;
    return sinf(x) / c;
}

float sqrtf(float x)
{
    float g;
    int i;
    if (x <= 0.0f) return 0.0f;
    g = x;                                   /* Newton-Raphson from a rough seed */
    for (i = 0; i < 12; i++) g = 0.5f * (g + x / g);
    return g;
}

/* ===========================================================================
 * Extended single-precision libm for the MicroPython port (PYRT.UNO).
 * Demo/game-grade minimax + range reduction; ~1e-5 accuracy, enough for the
 * `math` module and Duum's renderer trig. No double libm on this target.
 * ======================================================================== */
#define LN2_F  0.69314718056f
#define LG2E   1.44269504089f       /* 1/ln2 */

float copysignf(float x, float y) { float a = fabsf(x); return y < 0.0f ? -a : a; }
float truncf(float x) { return x < 0.0f ? ceilf(x) : floorf(x); }
float roundf(float x) { return x < 0.0f ? ceilf(x - 0.5f) : floorf(x + 0.5f); }
float fmodf(float x, float y)
{
    float q;
    if (y == 0.0f) return 0.0f;
    q = truncf(x / y);
    return x - q * y;
}

/* exp2f: 2^x via integer split + degree-5 poly on the fraction in [0,1). */
static float exp2f_(float x)
{
    int e; float f, r; union { float f; unsigned u; } bits;
    if (x > 127.0f)  return 3.4e38f;
    if (x < -126.0f) return 0.0f;
    e = (int)floorf(x);
    f = x - (float)e;                        /* 0 <= f < 1 */
    /* minimax 2^f on [0,1] */
    r = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f +
        f * (0.0096181f + f * 0.0013333f))));
    bits.u = (unsigned)((e + 127) & 0xFF) << 23;   /* 2^e */
    return r * bits.f;
}
float expf(float x)  { return exp2f_(x * LG2E); }
float exp2f(float x) { return exp2f_(x); }

/* logf: ln(x) = (exp-127)*ln2 + ln(mantissa in [1,2)). */
float logf(float x)
{
    union { float f; unsigned u; } b;
    int e; float m, t, p;
    if (x <= 0.0f) return -3.4e38f;
    b.f = x;
    e = (int)((b.u >> 23) & 0xFF) - 127;
    b.u = (b.u & 0x007FFFFF) | 0x3F800000;   /* mantissa in [1,2) */
    m = b.f;
    t = (m - 1.0f) / (m + 1.0f);             /* atanh series for ln */
    p = t * t;
    return (float)e * LN2_F + 2.0f * t * (1.0f + p * (0.333333f + p * (0.2f + p * 0.142857f)));
}
float log10f(float x) { return logf(x) * 0.43429448f; }
float log2f(float x)  { return logf(x) * LG2E; }

float powf(float x, float y)
{
    if (y == 0.0f) return 1.0f;
    if (x == 0.0f) return 0.0f;
    if (x < 0.0f) {                          /* real pow needs integer y */
        float r = expf(y * logf(-x));
        int iy = (int)y;
        return ((iy & 1) ? -r : r);
    }
    return expf(y * logf(x));
}
float cbrtf(float x) { return x < 0.0f ? -powf(-x, 0.3333333f) : powf(x, 0.3333333f); }

/* atan family: atanf via range reduction to [-1,1] + minimax. */
float atanf(float x)
{
    float a = fabsf(x), r, z;
    int inv = 0;
    if (a > 1.0f) { a = 1.0f / a; inv = 1; }
    z = a * a;
    r = a * (0.9998660f + z * (-0.3302995f + z * (0.1801410f + z * (-0.0851330f + z * 0.0208351f))));
    if (inv) r = HALFPI - r;
    return x < 0.0f ? -r : r;
}
float atan2f(float y, float x)
{
    if (x > 0.0f) return atanf(y / x);
    if (x < 0.0f) return atanf(y / x) + (y < 0.0f ? -PI_F : PI_F);
    return y > 0.0f ? HALFPI : (y < 0.0f ? -HALFPI : 0.0f);
}
float asinf(float x)
{
    if (x >= 1.0f)  return HALFPI;
    if (x <= -1.0f) return -HALFPI;
    return atanf(x / sqrtf(1.0f - x * x));
}
float acosf(float x) { return HALFPI - asinf(x); }

float sinhf(float x) { float e = expf(x); return 0.5f * (e - 1.0f / e); }
float coshf(float x) { float e = expf(x); return 0.5f * (e + 1.0f / e); }
float tanhf(float x) { float e = expf(2.0f * x); return (e - 1.0f) / (e + 1.0f); }
float expm1f(float x) { return expf(x) - 1.0f; }
float log1pf(float x) { return logf(1.0f + x); }

float ldexpf(float x, int e)
{
    while (e > 30)  { x *= 1073741824.0f; e -= 30; }
    while (e < -30) { x *= (1.0f / 1073741824.0f); e += 30; }
    { union { float f; unsigned u; } b; b.u = (unsigned)((e + 127) & 0xFF) << 23; return x * b.f; }
}
float frexpf(float x, int *e)
{
    union { float f; unsigned u; } b; int ex;
    if (x == 0.0f) { *e = 0; return 0.0f; }
    b.f = x;
    ex = (int)((b.u >> 23) & 0xFF) - 126;
    b.u = (b.u & 0x807FFFFF) | 0x3F000000;   /* mantissa in [0.5,1) */
    *e = ex;
    return b.f;
}
float modff(float x, float *ip) { float t = truncf(x); *ip = t; return x - t; }

float nearbyintf(float x) { return roundf(x); }   /* round-half-away is fine here */
float rintf(float x) { return roundf(x); }
