#ifndef CONFIG_H__
#define CONFIG_H__
/* UnoDOS/pc64 freestanding BearSSL config: portable C only. No CPU
   intrinsics (they would pull <intrin.h>/<immintrin.h> into a -nostdinc
   build), no int128 path, no OS entropy, no system clock. tls.c seeds the
   PRNG itself and pins the server key (br_x509_knownkey), so no CA store or
   time source is needed. All security-bearing code stays BearSSL's own
   audited portable implementation - we only drop platform accel + glue. */
#define BR_INT128            0
#define BR_UMUL128           0
#define BR_RDRAND            0
#define BR_AES_X86NI         0
#define BR_SSE2              0
#define BR_POWER8            0
#define BR_ARMEL_CORTEXM_GCC 0
#define BR_USE_GETENTROPY    0
#define BR_USE_URANDOM       0
#define BR_USE_WIN32_RAND    0
#define BR_USE_UNIX_TIME     0
#define BR_USE_WIN32_TIME    0
#endif
