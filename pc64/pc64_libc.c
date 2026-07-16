/* ===========================================================================
 * UnoDOS/pc64 - freestanding mini-libc.
 *
 * The portable core (unodos.c + mac_compat.c + the 11 app modules) uses a
 * deliberately tiny libc surface: the mem- and str- functions plus
 * malloc/free/realloc behind NewPtr/DisposePtr and the RAM-disk buffers.
 * On a UEFI bare-metal build there
 * is no C runtime at all (-ffreestanding -nostdlib), so this file IS the libc.
 *
 * The allocator is a first-fit free-list over one static arena in .bss - the
 * same shape as the x86 kernel's heap (API 7/8, first-fit) and, like the rest
 * of the port, deliberately boring: UnoDOS allocates a few KB per open window
 * and the RAM-disk file buffers, nothing else.
 * ===========================================================================
 */
#include <stddef.h>

/* ---- mem* / str* (gcc also emits calls to these for struct copies) ------ */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (n--) { if (*p != *q) return (int)*p - (int)*q; p++; q++; }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && *src) { *d++ = *src++; n--; }
    while (n--) *d++ = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++) != 0) ;
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

int abs(int v) { return v < 0 ? -v : v; }

/* ---- first-fit free-list allocator over a static arena ------------------ */
#define HEAP_BYTES (8u * 1024u * 1024u)

typedef struct Blk {
    size_t       size;              /* payload bytes */
    int          used;
    struct Blk  *next;
} Blk;

static unsigned char gHeap[HEAP_BYTES] __attribute__((aligned(16)));
static Blk *gHead = 0;

#define ALIGN16(n) (((n) + 15u) & ~(size_t)15u)
#define HDR        ALIGN16(sizeof(Blk))

static void heap_init(void)
{
    gHead = (Blk *)gHeap;
    gHead->size = HEAP_BYTES - HDR;
    gHead->used = 0;
    gHead->next = 0;
}

void *malloc(size_t n)
{
    Blk *b;
    if (!gHead) heap_init();
    if (n == 0) n = 1;
    n = ALIGN16(n);
    for (b = gHead; b; b = b->next) {
        if (b->used || b->size < n) continue;
        if (b->size >= n + HDR + 16) {          /* split the tail off */
            Blk *t = (Blk *)((unsigned char *)b + HDR + n);
            t->size = b->size - n - HDR;
            t->used = 0;
            t->next = b->next;
            b->size = n;
            b->next = t;
        }
        b->used = 1;
        return (unsigned char *)b + HDR;
    }
    return 0;
}

void free(void *p)
{
    Blk *b;
    if (!p) return;
    b = (Blk *)((unsigned char *)p - HDR);
    b->used = 0;
    /* coalesce forward runs of free blocks */
    while (b->next && !b->next->used &&
           (unsigned char *)b + HDR + b->size == (unsigned char *)b->next) {
        b->size += HDR + b->next->size;
        b->next = b->next->next;
    }
}

void *realloc(void *p, size_t n)
{
    Blk *b;
    void *np;
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    b = (Blk *)((unsigned char *)p - HDR);
    if (b->size >= n) return p;
    np = malloc(n);
    if (!np) return 0;
    memcpy(np, p, b->size);
    free(p);
    return np;
}

void *calloc(size_t nm, size_t sz)
{
    size_t n = nm * sz;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

/* gcc emits a stack probe call for frames > 4KB on windows targets; the UEFI
   stack (128KB, fully committed by the firmware) needs no guard-page touch,
   so a plain ret - which clobbers nothing - satisfies the reference. */
__asm__(
    ".globl ___chkstk_ms\n"
    "___chkstk_ms:\n"
    "    ret\n"
);
