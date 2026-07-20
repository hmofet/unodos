/* freestanding <stdio.h> - just the printf-family prototypes MicroPython
   references (definitions in pc64_libc.c). No FILE streams. */
#ifndef PC64_STDIO_H
#define PC64_STDIO_H
#include <stddef.h>
#include <stdarg.h>
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int printf(const char *fmt, ...);
#endif
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
