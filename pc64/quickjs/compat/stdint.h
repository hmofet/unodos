/* pc64/quickjs/compat/stdint.h - pc64's stdint plus the pointer-width limit
 * macros it omits. These are not decoration: quickjs.h picks its JSValue
 * representation with `#if INTPTR_MAX < INT64_MAX`, and an UNDEFINED macro
 * is 0 in #if context - which silently selected NaN-boxing (the 32-bit
 * layout, pointers truncated to the NaN payload) on this 64-bit build and
 * crashed the first js_dup. The build also passes -DJS_NAN_BOXING=0 so the
 * choice no longer rides on a header sniff at all. */
#ifndef QJS_COMPAT_STDINT_H
#define QJS_COMPAT_STDINT_H

#include_next <stdint.h>

#ifndef INTPTR_MAX
#define INTPTR_MAX  INT64_MAX
#define INTPTR_MIN  INT64_MIN
#define UINTPTR_MAX UINT64_MAX
#endif
#ifndef PTRDIFF_MAX
#define PTRDIFF_MAX INT64_MAX
#define PTRDIFF_MIN INT64_MIN
#endif
#ifndef INT32_MIN
#define INT32_MIN (-2147483647-1)
#endif

#endif /* QJS_COMPAT_STDINT_H */
