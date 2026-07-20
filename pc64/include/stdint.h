/* UnoDOS/pc64 freestanding <stdint.h> - see pc64_libc.c */
#ifndef PC64_STDINT_H
#define PC64_STDINT_H
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;
typedef long long          intptr_t;
typedef unsigned long long uintptr_t;
#endif

/* limits the MicroPython port references */
#ifndef INT8_MAX
#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647
#define INT64_MAX  9223372036854775807LL
#define UINT8_MAX  255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295u
#define UINT64_MAX 18446744073709551615ull
#define INT8_MIN   (-128)
#define INT16_MIN  (-32768)
#define INT32_MIN  (-2147483647-1)
#define INT64_MIN  (-9223372036854775807LL-1)
#define SIZE_MAX   UINT64_MAX
#endif
