/* pc64/quickjs/compat/time.h - the time surface the vendored QuickJS needs.
 * UnoDOS has no timezone database, so this port is UTC-ONLY by contract:
 * localtime_r IS gmtime_r and getTimezoneOffset() is 0. Implementations are
 * in qjs_port.c over the CMOS RTC + calibrated TSC (pc64_native.h). */
#ifndef QJS_COMPAT_TIME_H
#define QJS_COMPAT_TIME_H

typedef long long time_t;

struct tm {
    int tm_sec, tm_min, tm_hour;
    int tm_mday, tm_mon, tm_year;    /* month 0-11, year since 1900 */
    int tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;                  /* always 0: UTC-only port */
};

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

time_t     time(time_t *t);
double     difftime(time_t a, time_t b);
time_t     mktime(struct tm *tm);            /* input treated as UTC */
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime_r(const time_t *t, struct tm *out);
int        clock_gettime(int clk, struct timespec *ts);

#endif /* QJS_COMPAT_TIME_H */
