/* UnoDOS/pc64 freestanding <stdlib.h> - definitions in pc64_libc.c */
#ifndef PC64_STDLIB_H
#define PC64_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void  free(void *p);
void *realloc(void *p, size_t n);
void *calloc(size_t nm, size_t sz);
int   abs(int v);
#endif
