/* csslib/compat/inttypes.h - the PRI macros the CSS stack uses; pc64's
 * mini-libc has no <inttypes.h>. mingw x86-64 is LLP64: 64-bit is `long
 * long`. (Same shape as pc64/quickjs/compat/inttypes.h; kept per-vendor so
 * each drop stays self-contained.) */
#ifndef CSSLIB_COMPAT_INTTYPES_H
#define CSSLIB_COMPAT_INTTYPES_H

#include <stdint.h>

#define PRId8   "d"
#define PRIu8   "u"
#define PRIx8   "x"
#define PRId16  "d"
#define PRIu16  "u"
#define PRIx16  "x"
#define PRId32  "d"
#define PRIi32  "i"
#define PRIu32  "u"
#define PRIx32  "x"
#define PRIX32  "X"
#define PRId64  "lld"
#define PRIi64  "lli"
#define PRIu64  "llu"
#define PRIx64  "llx"
#define PRIX64  "llX"
#define PRIdPTR "lld"
#define PRIuPTR "llu"
#define PRIxPTR "llx"

#endif /* CSSLIB_COMPAT_INTTYPES_H */
