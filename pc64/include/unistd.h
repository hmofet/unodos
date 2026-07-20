/* freestanding <unistd.h> - minimal (MicroPython stream.c only needs the SEEK_* / ssize_t types via other headers) */
#ifndef PC64_UNISTD_H
#define PC64_UNISTD_H
#include <stddef.h>
typedef long ssize_t;
#endif
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
