/* pc64/quickjs/compat/sys/time.h - gettimeofday for the vendored QuickJS.
 * Implemented in qjs_port.c: RTC anchor + TSC delta (see the note there). */
#ifndef QJS_COMPAT_SYS_TIME_H
#define QJS_COMPAT_SYS_TIME_H

#include <time.h>

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

int gettimeofday(struct timeval *tv, void *tz);

#endif /* QJS_COMPAT_SYS_TIME_H */
