/* pc64/quickjs/compat/math.h - the double-precision <math.h> the vendored
 * QuickJS objects see INSTEAD of pc64's (this dir is first on the include
 * path for quickjs compiles only; nothing else in the tree sees it).
 *
 * pc64's kernel math is float-only by design (build.sh), and JS numbers are
 * IEEE doubles, so every function here must be a real double implementation.
 * The core transcendentals are unojs's own double math (unojs/ujs_math.c),
 * consumed symbol-by-symbol as the nearest existing primitive per AGENTS.md
 * S4; the derivable remainder lives in qjs_port.c under qjs_* names.
 *
 * All renames are FUNCTION-LIKE macros on purpose: quickjs routes every
 * libm call through its js_math_* wrappers as `name(arg)` calls and never
 * takes a libm function's address, so `name(` is the only shape that must
 * match - and a function-like macro cannot mangle `int exp;`-style locals
 * the way an object-like `#define exp` would. */
#ifndef QJS_COMPAT_MATH_H
#define QJS_COMPAT_MATH_H

#define NAN       (__builtin_nanf(""))
#define INFINITY  (__builtin_inff())
#define HUGE_VAL  (__builtin_inf())

/* real symbol in qjs_port.c (one cvtsd2si): __builtin_lrint just emits a
 * libcall here, and quickjs's two call sites are both small-range, so a
 * 32-bit `long` result is exact. */
long qjs_lrint(double x);
#define lrint(x)    qjs_lrint(x)

#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

/* unojs's double math. These used to be declared by hand right here, because
 * the only declaration in the tree was in unojs-internal `ujs_int.h`. The
 * unojs owner blessed the set as a public [STABLE] surface on 2026-08-06, so
 * consume it: one declaration, and the hosted/freestanding split handled
 * there rather than assumed here. */
#include "ujs_math.h"

/* the remainder, derived in qjs_port.c */
double qjs_trunc(double x);
double qjs_round(double x);
double qjs_log2(double x);
double qjs_log1p(double x);
double qjs_expm1(double x);
double qjs_cbrt(double x);
double qjs_hypot(double x, double y);
double qjs_copysign(double x, double y);
double qjs_scalbn(double x, int n);
double qjs_frexp(double x, int *e);
double qjs_asin(double x);
double qjs_acos(double x);
double qjs_sinh(double x);
double qjs_cosh(double x);
double qjs_tanh(double x);
double qjs_asinh(double x);
double qjs_acosh(double x);
double qjs_atanh(double x);

#define fabs(x)        ujs_fabs(x)
#define floor(x)       ujs_floor(x)
#define ceil(x)        ujs_ceil(x)
#define fmod(x, y)     ujs_fmod(x, y)
#define sqrt(x)        ujs_sqrt(x)
#define exp(x)         ujs_exp(x)
#define log(x)         ujs_log(x)
#define log10(x)       ujs_log10(x)
#define pow(x, y)      ujs_pow(x, y)
#define sin(x)         ujs_sin(x)
#define cos(x)         ujs_cos(x)
#define tan(x)         ujs_tan(x)
#define atan(x)        ujs_atan(x)
#define atan2(y, x)    ujs_atan2(y, x)

#define trunc(x)       qjs_trunc(x)
#define round(x)       qjs_round(x)
#define log2(x)        qjs_log2(x)
#define log1p(x)       qjs_log1p(x)
#define expm1(x)       qjs_expm1(x)
#define cbrt(x)        qjs_cbrt(x)
#define hypot(x, y)    qjs_hypot(x, y)
#define copysign(x, y) qjs_copysign(x, y)
#define scalbn(x, n)   qjs_scalbn(x, n)
#define frexp(x, e)    qjs_frexp(x, e)
#define asin(x)        qjs_asin(x)
#define acos(x)        qjs_acos(x)
#define sinh(x)        qjs_sinh(x)
#define cosh(x)        qjs_cosh(x)
#define tanh(x)        qjs_tanh(x)
#define asinh(x)       qjs_asinh(x)
#define acosh(x)       qjs_acosh(x)
#define atanh(x)       qjs_atanh(x)

#endif /* QJS_COMPAT_MATH_H */
