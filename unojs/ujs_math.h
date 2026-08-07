/* unojs/ujs_math.h - unojs's double-precision mini-libm, as a PUBLIC surface.
 *
 * [STABLE] as of 2026-08-06. Requested by the quickjs port, which had been
 * declaring these fourteen symbols by hand in `pc64/quickjs/compat/math.h`
 * because the only declaration in the tree lived in `ujs_int.h`, which is
 * unojs-internal (UNOAUTOMATE-REQUESTS.md, 2026-08-05).
 *
 * Why this exists at all: pc64's kernel math is float-only by design, and JS
 * numbers are IEEE doubles. unojs therefore carries its own double math
 * (`ujs_math.c`) rather than linking a host libm, and runs the same code on
 * the host tests and on metal. Any freestanding consumer that needs real
 * doubles - a second JS engine, a layout engine doing sub-pixel arithmetic -
 * should consume THIS rather than assume a libm.
 *
 * Contract: each function has the semantics of its C99 <math.h> namesake for
 * finite inputs, including the sign and NaN/infinity edge cases the ECMAScript
 * Math object requires. Accuracy is "good enough for JS number formatting and
 * Math.*", not correctly-rounded; do not rely on the last ulp.
 *
 * On a hosted build (`UJS_USE_LIBM`, i.e. the host test suites) these fold to
 * the platform's libm, so a consumer including this header gets the right
 * thing in both worlds and must not declare the symbols itself. */
#ifndef UJS_MATH_H
#define UJS_MATH_H

#ifdef UJS_USE_LIBM
#include <math.h>
#define ujs_fabs  fabs
#define ujs_floor floor
#define ujs_ceil  ceil
#define ujs_fmod  fmod
#define ujs_sqrt  sqrt
#define ujs_pow   pow
#define ujs_exp   exp
#define ujs_log   log
#define ujs_log10 log10
#define ujs_sin   sin
#define ujs_cos   cos
#define ujs_tan   tan
#define ujs_atan  atan
#define ujs_atan2 atan2
#else
double ujs_fabs(double x);
double ujs_floor(double x);
double ujs_ceil(double x);
double ujs_fmod(double x, double y);
double ujs_sqrt(double x);
double ujs_pow(double x, double y);
double ujs_exp(double x);
double ujs_log(double x);
double ujs_log10(double x);
double ujs_sin(double x);
double ujs_cos(double x);
double ujs_tan(double x);
double ujs_atan(double x);
double ujs_atan2(double y, double x);
#endif

#endif /* UJS_MATH_H */
