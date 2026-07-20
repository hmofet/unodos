/* freestanding <setjmp.h> - MicroPython NLR fallback over the GCC builtins
   (the same primitive ucc uses). Native x64 NLR is default; this backs
   MICROPY_NLR_SETJMP=1 if that is forced. */
#ifndef PC64_SETJMP_H
#define PC64_SETJMP_H
typedef void *jmp_buf[5];
#define setjmp(b)     __builtin_setjmp(b)
#define longjmp(b,v)  __builtin_longjmp((b),1)
#endif
