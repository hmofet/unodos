/* csslib/compat/stdlib.h - pc64's stdlib plus bsearch, which the CSS
 * stack's charset tables use and the mini-libc omits. Implemented in
 * ../css_port.c. */
#ifndef CSSLIB_COMPAT_STDLIB_H
#define CSSLIB_COMPAT_STDLIB_H

#include_next <stdlib.h>

void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *));

#endif /* CSSLIB_COMPAT_STDLIB_H */
