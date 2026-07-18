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
typedef unsigned long long uword;   /* 64-bit word for bulk copies/fills */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    /* copy 8 bytes at a time once the pointers are word-aligned and in the same
       phase - the desktop background blit and per-frame scene restore move
       megabytes, so a byte-at-a-time loop was a real cost. */
    if ((((size_t)d ^ (size_t)s) & 7u) == 0) {
        while (n && ((size_t)d & 7u)) { *d++ = *s++; n--; }
        while (n >= 8) { *(uword *)d = *(const uword *)s; d += 8; s += 8; n -= 8; }
    }
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
    unsigned char b = (unsigned char)c;
    uword w = (uword)b;
    w |= w << 8; w |= w << 16; w |= w << 32;
    while (n && ((size_t)d & 7u)) { *d++ = b; n--; }
    while (n >= 8) { *(uword *)d = w; d += 8; n -= 8; }
    while (n--) *d++ = b;
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
    Blk *b, *w;
    if (!p) return;
    b = (Blk *)((unsigned char *)p - HDR);
    /* reject pointers that aren't inside the arena, and double-frees, rather
       than corrupting block metadata. */
    if ((unsigned char *)b < gHeap || (unsigned char *)b >= gHeap + HEAP_BYTES)
        return;
    if (!b->used) return;
    b->used = 0;
    /* coalesce ALL adjacent free runs. The free list stays address-ordered, so
       a single forward sweep merges both the just-freed block with its
       successor AND its predecessor with it (backward coalescing) - the old
       forward-only merge left long-run fragmentation. */
    for (w = gHead; w; w = w->next) {
        while (w->next && !w->used && !w->next->used &&
               (unsigned char *)w + HDR + w->size == (unsigned char *)w->next) {
            w->size += HDR + w->next->size;
            w->next = w->next->next;
        }
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
    size_t n;
    void *p;
    if (sz && nm > (size_t)-1 / sz) return 0;   /* multiply would overflow */
    n = nm * sz;
    p = malloc(n);
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
