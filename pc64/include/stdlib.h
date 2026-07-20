/* UnoDOS/pc64 freestanding <stdlib.h> - definitions in pc64_libc.c */
#ifndef PC64_STDLIB_H
#define PC64_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void  free(void *p);
void *realloc(void *p, size_t n);
void *calloc(size_t nm, size_t sz);
int   abs(int v);
long long llabs(long long v);
/* number parsers + misc the MicroPython port needs (pc64_libc.c) */
long           strtol(const char *s, char **end, int base);
unsigned long  strtoul(const char *s, char **end, int base);
long long      strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
int            atoi(const char *s);
void           abort(void);
void           qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
#endif
