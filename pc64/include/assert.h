/* freestanding <assert.h> - assertions compiled out */
#undef assert
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((void)0)
#endif
