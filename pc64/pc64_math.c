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

float floorf(float x)
{
    int i = (int)x;
    if ((float)i > x) i--;
    return (float)i;
}
float ceilf(float x)
{
    int i = (int)x;
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
