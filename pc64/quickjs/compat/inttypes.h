/* pc64/quickjs/compat/inttypes.h - just the PRI macros the vendored QuickJS
 * uses. mingw x86-64 is LLP64: the 64-bit types are `long long`. */
#ifndef QJS_COMPAT_INTTYPES_H
#define QJS_COMPAT_INTTYPES_H

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

#endif /* QJS_COMPAT_INTTYPES_H */
