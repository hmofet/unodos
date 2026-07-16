/* UnoDOS/pc64 freestanding <stddef.h> - see pc64_libc.c */
#ifndef PC64_STDDEF_H
#define PC64_STDDEF_H
typedef unsigned long long size_t;
typedef long long          ptrdiff_t;
typedef unsigned short     wchar_t;
#ifndef NULL
#define NULL ((void *)0)
#endif
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif
