/* csslib/compat/strings.h - the POSIX <strings.h> subset the CSS stack
 * uses (case-insensitive compares; pc64's mini-libc has neither).
 * Implemented in ../css_port.c. */
#ifndef CSSLIB_COMPAT_STRINGS_H
#define CSSLIB_COMPAT_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif /* CSSLIB_COMPAT_STRINGS_H */
