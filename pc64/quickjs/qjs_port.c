/* qjs_port.c - the freestanding-UnoDOS port layer under the vendored QuickJS.
 *
 * Two halves:
 *
 *   1. The double-precision math that compat/math.h routes here (qjs_*).
 *      Everything is DERIVED from unojs's double core (ujs_exp/ujs_log/
 *      ujs_pow/ujs_sqrt/ujs_atan2...), so there is exactly one double
 *      transcendental implementation in the tree. The derivations lose a few
 *      ulps against a real libm (expm1/log1p near 0 especially); that is the
 *      accepted trade for not vendoring a second libm, and the upgrade path
 *      if it ever matters is swapping this file's tail for musl's.
 *
 *   2. The POSIX-shaped time surface (gettimeofday / clock_gettime / mktime
 *      family) over the CMOS RTC + calibrated TSC. UTC-only: UnoDOS has no
 *      timezone database, so localtime IS gmtime and Date's tz offset is 0.
 *      Wall time anchors the RTC once and advances on the TSC delta; if the
 *      TSC is not calibrated yet it falls back to re-reading the RTC, which
 *      only costs sub-second Date.now() resolution before calibration.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include "dtoa.h"
#include "../pc64_native.h"

/* ---- FILE-shaped stdio (compat/stdio.h) ----------------------------------
 * Only quickjs's debug/dump paths reach these; everything lands in pc64's
 * printf, i.e. the kernel log. The FILE pointers are pure sentinels, and
 * everything is a qjs_* symbol - see the interposition note in
 * compat/stdio.h. (The macros there rename this file's own definitions
 * back to these names; the explicit qjs_ spelling below is for grep.) */
struct qjs_FILE { int unused; };
static qjs_FILE g_stdout, g_stderr;
qjs_FILE *qjs_stdout = &g_stdout;
qjs_FILE *qjs_stderr = &g_stderr;

int qjs_vfprintf(qjs_FILE *f, const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    (void)f;
    printf("%s", buf);
    return n;
}

int qjs_fprintf(qjs_FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = qjs_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int qjs_fputs(const char *s, qjs_FILE *f) { (void)f; printf("%s", s); return 0; }
int qjs_fputc(int c, qjs_FILE *f) { (void)f; printf("%c", c); return c; }
int qjs_putchar(int c) { printf("%c", c); return c; }
int qjs_fflush(qjs_FILE *f) { (void)f; return 0; }

size_t qjs_fwrite(const void *p, size_t sz, size_t n, qjs_FILE *f)
{
    char buf[256];
    size_t total = sz * n;
    size_t take = total < sizeof buf - 1 ? total : sizeof buf - 1;
    (void)f;
    memcpy(buf, p, take);
    buf[take] = 0;
    printf("%s", buf);
    return n;
}

/* ---- bit views ---------------------------------------------------------- */
static uint64_t d2b(double x) { union { double d; uint64_t b; } u; u.d = x; return u.b; }
static double   b2d(uint64_t b) { union { double d; uint64_t b; } u; u.b = b; return u.d; }

#define D_NAN  (__builtin_nan(""))
#define D_INF  (__builtin_inf())

/* ---- rounding-family ----------------------------------------------------- */
double qjs_trunc(double x)
{
    if (!isfinite(x)) return x;
    return x < 0 ? ujs_ceil(x) : ujs_floor(x);
}

/* C round(): halfway cases away from zero (Math.round's half-up is quickjs's
 * own job on top of this). */
double qjs_round(double x)
{
    if (!isfinite(x)) return x;
    return x < 0 ? ujs_ceil(x - 0.5) : ujs_floor(x + 0.5);
}

long qjs_lrint(double x)
{
    /* SSE cvtsd2si: round-to-nearest-even in the kernel's default MXCSR.
     * quickjs's call sites are pre-clamped to small ranges, so 32-bit is
     * exact (mingw long is 32-bit). */
    int r;
    __asm__("cvtsd2si %1, %0" : "=r"(r) : "x"(x));
    return r;
}

double qjs_copysign(double x, double y)
{ return b2d((d2b(x) & ~(1ull << 63)) | (d2b(y) & (1ull << 63))); }

/* ---- exponent surgery ---------------------------------------------------- */
double qjs_scalbn(double x, int n)
{
    /* multiply by 2^n in exponent-safe chunks; the extremes flush to inf/0
     * through the multiplies themselves, denormals included. */
    double p;
    while (n != 0) {
        int step = n > 1023 ? 1023 : n < -1022 ? -1022 : n;
        p = b2d((uint64_t)(step + 1023) << 52);       /* exact power of two */
        x *= p;
        n -= step;
        if (x == 0.0 || !isfinite(x)) break;
    }
    return x;
}

double qjs_frexp(double x, int *e)
{
    uint64_t b = d2b(x);
    int be = (int)((b >> 52) & 0x7ff);
    *e = 0;
    if (be == 0x7ff || x == 0.0) return x;            /* inf/nan/0: e stays 0 */
    if (be == 0) {                                    /* subnormal: normalize */
        x *= 0x1p64;
        b = d2b(x);
        be = (int)((b >> 52) & 0x7ff);
        *e = -64;
    }
    *e += be - 1022;
    return b2d((b & ~(0x7ffull << 52)) | (1022ull << 52));
}

/* ---- derived transcendentals --------------------------------------------- */
double qjs_log2(double x)  { return ujs_log(x) * 1.4426950408889634074; }
double qjs_log1p(double x) { return ujs_log(1.0 + x); }
double qjs_expm1(double x) { return ujs_exp(x) - 1.0; }

double qjs_cbrt(double x)
{
    double r;
    if (x == 0.0 || !isfinite(x)) return x;           /* keeps -0 and +-inf */
    r = ujs_pow(ujs_fabs(x), 1.0 / 3.0);
    r = r - (r - ujs_fabs(x) / (r * r)) / 3.0;        /* one Newton polish   */
    return x < 0 ? -r : r;
}

double qjs_hypot(double x, double y)
{
    double a = ujs_fabs(x), b = ujs_fabs(y), t;
    if (isinf(a) || isinf(b)) return D_INF;
    if (a < b) { t = a; a = b; b = t; }
    if (a == 0.0) return 0.0;
    t = b / a;
    return a * ujs_sqrt(1.0 + t * t);
}

double qjs_asin(double x)
{
    if (isnan(x) || x > 1.0 || x < -1.0) return D_NAN;
    return ujs_atan2(x, ujs_sqrt((1.0 - x) * (1.0 + x)));
}

double qjs_acos(double x)
{
    if (isnan(x) || x > 1.0 || x < -1.0) return D_NAN;
    return ujs_atan2(ujs_sqrt((1.0 - x) * (1.0 + x)), x);
}

double qjs_sinh(double x)
{
    double e = ujs_exp(ujs_fabs(x)), r = (e - 1.0 / e) * 0.5;
    return x < 0 ? -r : r;
}

double qjs_cosh(double x)
{
    double e = ujs_exp(ujs_fabs(x));
    return (e + 1.0 / e) * 0.5;
}

double qjs_tanh(double x)
{
    double e2;
    if (isnan(x)) return x;
    if (ujs_fabs(x) > 20.0) return x < 0 ? -1.0 : 1.0;
    e2 = ujs_exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}

double qjs_asinh(double x)
{
    double a = ujs_fabs(x), r;
    if (!isfinite(x)) return x;
    r = ujs_log(a + ujs_sqrt(a * a + 1.0));
    return x < 0 ? -r : r;
}

double qjs_acosh(double x)
{
    if (isnan(x) || x < 1.0) return D_NAN;
    return ujs_log(x + ujs_sqrt(x * x - 1.0));
}

double qjs_atanh(double x)
{
    if (isnan(x) || x > 1.0 || x < -1.0) return D_NAN;
    if (x == 1.0)  return D_INF;
    if (x == -1.0) return -D_INF;
    return 0.5 * ujs_log((1.0 + x) / (1.0 - x));
}

/* ---- strtod (the JSON lexer's one call) ----------------------------------
 * quickjs parses every other number itself through js_atod; JSON reaches for
 * libc strtod, so route it to the same correctly-rounding js_atod. */
double strtod(const char *s, char **end)
{
    JSATODTempMem tmp;
    return js_atod(s, (const char **)end, 10, 0, &tmp);
}

/* ---- wall clock / monotonic clock ---------------------------------------- */
static long long civil_days(int y, int m, int d)   /* Hinnant, m 1-12 */
{
    long long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;              /* days since 1970-01-01 */
}

static long long rtc_epoch_s(void)
{
    int y, mo, d, h, mi, s;
    /* rtc_read returns 1 on SUCCESS (its only 0 is the UIP spin timing out),
     * so this test used to be inverted: every machine with a working clock
     * took the "dead CMOS" arm and Date was permanently at the epoch. Filed
     * by the unolog lane 2026-08-06, alongside the same inversion in
     * pc64_cache.c / pc64_cookie.c (fixed there in 3c8d5a86). */
    if (!uno_native_rtc_read(&y, &mo, &d, &h, &mi, &s))
        return 0;                                    /* dead CMOS: epoch 0    */
    return civil_days(y, mo, d) * 86400ll + h * 3600ll + mi * 60ll + s;
}

static long long g_base_us;                          /* wall us at the anchor */
static unsigned long long g_base_tsc;
static int g_anchored;

int gettimeofday(struct timeval *tv, void *tz)
{
    long long us;
    unsigned long long per_us = uno_native_tsc_per_us();
    (void)tz;
    if (per_us && !g_anchored) {
        g_base_us  = rtc_epoch_s() * 1000000ll;
        g_base_tsc = uno_native_rdtsc();
        g_anchored = 1;
    }
    if (g_anchored && per_us)
        us = g_base_us + (long long)((uno_native_rdtsc() - g_base_tsc) / per_us);
    else
        us = rtc_epoch_s() * 1000000ll;              /* pre-calibration       */
    tv->tv_sec  = us / 1000000ll;
    tv->tv_usec = (long)(us % 1000000ll);
    return 0;
}

int clock_gettime(int clk, struct timespec *ts)
{
    unsigned long long per_us = uno_native_tsc_per_us();
    if (clk == CLOCK_MONOTONIC && per_us) {
        unsigned long long us = uno_native_rdtsc() / per_us;
        ts->tv_sec  = (time_t)(us / 1000000ull);
        ts->tv_nsec = (long)(us % 1000000ull) * 1000;
        return 0;
    }
    {
        struct timeval tv;
        gettimeofday(&tv, 0);
        ts->tv_sec  = tv.tv_sec;
        ts->tv_nsec = tv.tv_usec * 1000;
    }
    return 0;
}

time_t time(time_t *t)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    if (t) *t = tv.tv_sec;
    return tv.tv_sec;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

/* ---- UTC civil-time conversions ------------------------------------------ */
time_t mktime(struct tm *tm)                         /* input is UTC here     */
{
    return civil_days(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday) * 86400ll
         + tm->tm_hour * 3600ll + tm->tm_min * 60ll + tm->tm_sec;
}

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    long long z = *t / 86400, rem = *t % 86400;
    long long era, doe, yoe, doy, mp;
    int y, m, d;
    if (rem < 0) { rem += 86400; z--; }
    era = (z >= -719468 ? z + 719468 : z + 719468 - 146096) / 146097;
    doe = (z + 719468) - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y   = (int)(yoe + era * 400);
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    d   = (int)(doy - (153 * mp + 2) / 5 + 1);
    m   = (int)(mp + (mp < 10 ? 3 : -9));
    y  += m <= 2;
    out->tm_year  = y - 1900;
    out->tm_mon   = m - 1;
    out->tm_mday  = d;
    out->tm_hour  = (int)(rem / 3600);
    out->tm_min   = (int)(rem % 3600 / 60);
    out->tm_sec   = (int)(rem % 60);
    out->tm_wday  = (int)((z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6));
    out->tm_yday  = 0;                               /* nothing reads it      */
    out->tm_isdst = 0;
    out->tm_gmtoff = 0;
    return out;
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{ return gmtime_r(t, out); }                         /* UTC-only port         */
