/* pc64/quickjs/compat/stdio.h - pc64's stdio plus the FILE-shaped surface
 * quickjs's debug/dump paths expect.
 *
 * Every FILE-shaped name is RENAMED to a qjs_* symbol (object-like macros,
 * so `stdout` as a bare data reference renames too). This matters: defining
 * a real symbol named `stdout` INTERPOSES the host C library's when the
 * same objects are linked into the host smoke test, and glibc's puts then
 * dereferences our dummy - an instant pre-output SEGV. qjs_* names collide
 * with nothing anywhere. Nothing here buffers: every write lands in pc64's
 * printf (the kernel log). */
#ifndef QJS_COMPAT_STDIO_H
#define QJS_COMPAT_STDIO_H

#include_next <stdio.h>          /* pc64/include/stdio.h: *printf family */
#include <stddef.h>
#include <stdarg.h>

typedef struct qjs_FILE qjs_FILE;
extern qjs_FILE *qjs_stdout, *qjs_stderr;

int    qjs_fprintf(qjs_FILE *f, const char *fmt, ...);
int    qjs_vfprintf(qjs_FILE *f, const char *fmt, va_list ap);
int    qjs_fputs(const char *s, qjs_FILE *f);
int    qjs_fputc(int c, qjs_FILE *f);
int    qjs_putchar(int c);
size_t qjs_fwrite(const void *p, size_t sz, size_t n, qjs_FILE *f);
int    qjs_fflush(qjs_FILE *f);

#define FILE     qjs_FILE
#define stdout   qjs_stdout
#define stderr   qjs_stderr
#define fprintf  qjs_fprintf
#define vfprintf qjs_vfprintf
#define fputs    qjs_fputs
#define fputc    qjs_fputc
#define putchar  qjs_putchar
#define fwrite   qjs_fwrite
#define fflush   qjs_fflush

#endif /* QJS_COMPAT_STDIO_H */
