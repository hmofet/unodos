/* css_port.c - the freestanding-UnoDOS port layer under the vendored CSS
 * stack. Tiny on purpose: the stack is plain C99 and the ONE non-C99
 * dependency (iconv in parserutils' input filter) is compile-gated off
 * with -DWITHOUT_ICONV_FILTER. What's left is the four libc routines the
 * mini-libc omits: the POSIX case-insensitive compares (compat/strings.h),
 * bsearch (compat/stdlib.h) and strdup (compat/string.h). */
#include <strings.h>
#include <string.h>
#include <stdlib.h>

void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *))
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *p = (const char *)base + mid * sz;
        int c = cmp(key, p);
        if (c == 0) return (void *)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int strcasecmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; n--; }
    return n ? lower((unsigned char)*a) - lower((unsigned char)*b) : 0;
}
