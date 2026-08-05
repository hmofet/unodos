/* csslib/compat/string.h - pc64's string.h plus strdup, which libwapcaplet
 * uses and the mini-libc omits. Implemented in ../css_port.c. */
#ifndef CSSLIB_COMPAT_STRING_H
#define CSSLIB_COMPAT_STRING_H

#include_next <string.h>

char *strdup(const char *s);

#endif /* CSSLIB_COMPAT_STRING_H */
