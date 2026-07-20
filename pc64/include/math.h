/* UnoDOS/pc64 freestanding <math.h> - single-precision only (pc64_math.c).
   uno3d uses sin/cos/tan/floor/ceil/sqrt; the MicroPython port needs the rest. */
#ifndef PC64_MATH_H
#define PC64_MATH_H

float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float expf(float x);
float exp2f(float x);
float expm1f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float log1pf(float x);
float powf(float x, float y);
float sqrtf(float x);
float cbrtf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float fabsf(float x);
float fmodf(float x, float y);
float copysignf(float x, float y);
float ldexpf(float x, int e);
float frexpf(float x, int *e);
float modff(float x, float *ip);
float nearbyintf(float x);
float rintf(float x);

/* MicroPython's modmath.c references the double names via MICROPY_FLOAT_C_FUN;
   under MICROPY_FLOAT_IMPL_FLOAT it appends 'f', so only the *f names are used.
   NAN/INFINITY for completeness. */
#define nanf(s)   __builtin_nanf(s)
#define nan(s)    __builtin_nan(s)
#define NAN       (__builtin_nanf(""))
#define INFINITY  (__builtin_inff())
#define HUGE_VALF INFINITY
#define HUGE_VAL  ((double)INFINITY)
#define M_PI      3.14159265358979f
#define M_E       2.71828182845905f

/* classification as compiler builtins (inline, so no imported functions) */
#define isfinite(x)  __builtin_isfinite(x)
#define isnan(x)     __builtin_isnan(x)
#define isinf(x)     __builtin_isinf(x)
#define signbit(x)   __builtin_signbit(x)
#define isnormal(x)  __builtin_isnormal(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN,FP_INFINITE,FP_NORMAL,FP_SUBNORMAL,FP_ZERO,x)
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_NORMAL 2
#define FP_SUBNORMAL 3
#define FP_ZERO 4

#endif
