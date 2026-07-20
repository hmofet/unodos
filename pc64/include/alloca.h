/* freestanding <alloca.h> */
#ifndef PC64_ALLOCA_H
#define PC64_ALLOCA_H
#define alloca(n) __builtin_alloca(n)
#endif
