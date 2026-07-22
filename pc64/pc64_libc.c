#include <stdarg.h>
#include <stdint.h>
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
#define HEAP_BYTES (32u * 1024u * 1024u)   /* Studio's compile arena + editor
                                              buffers live here; QEMU runs
                                              -m 256 so 32 MB of .bss is cheap */

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

#ifdef UNO_DEBUG
#include "uno_debug.h"
#include "unoauto.h"
/* debug build: walkable heap stats for the HUD + crash reports, and the
 * fail-every-Nth-malloc OOM injection the stress driver drives */
void uno_heap_stats(unsigned long *used, unsigned long *free_,
                    unsigned long *largest)
{
    Blk *b;
    unsigned long u = 0, f = 0, l = 0;
    for (b = gHead; b; b = b->next) {
        if (b->used) u += (unsigned long)b->size;
        else { f += (unsigned long)b->size;
               if (b->size > l) l = (unsigned long)b->size; }
    }
    if (used) *used = u;
    if (free_) *free_ = f;
    if (largest) *largest = l;
}
#else
void uno_heap_stats(unsigned long *used, unsigned long *free_,
                    unsigned long *largest)
{ if (used) *used = 0; if (free_) *free_ = 0; if (largest) *largest = 0; }
#endif

void *malloc(size_t n)
{
    Blk *b;
    if (!gHead) heap_init();
#ifdef UNO_DEBUG
    /* unoautomate tap: a hook on "libc.malloc" can trace every allocation or
     * force this one to fail (scriptable OOM). Guarded so a hook fn that
     * allocates cannot recurse into its own fire. */
    { static int in_hook;
      if (!in_hook) {
          UnoAutoAllocEv ev; ev.size = (unsigned long)n; ev.fail = 0;
          in_hook = 1; unoauto_hook_fire("libc.malloc", &ev); in_hook = 0;
          if (ev.fail) {
              uno_dbg_log("hook-inject: malloc(%lu) forced to fail", (unsigned long)n);
              return 0;
          }
      } }
    if (uno_dbg_oom_tick()) {
        uno_dbg_log("oom-inject: malloc(%lu) forced to fail", (unsigned long)n);
        return 0;
    }
#endif
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

/* ===========================================================================
 * Extended libc for the MicroPython port (PYRT.UNO): number parsers, a few
 * string helpers, and a compact snprintf/vsnprintf.  Shared with C apps.
 * ======================================================================== */
long long llabs(long long v) { return v < 0 ? -v : v; }
#ifdef UNO_DEBUG
void uno_dbg_panic(const char *why);
void abort(void) { uno_dbg_panic("abort() called"); for (;;) { } }
#else
void abort(void) { for (;;) { } }
#endif

char *strrchr(const char *s, int c)
{ const char *last = 0; do { if (*s == (char)c) last = s; } while (*s++); return (char *)last; }

char *strstr(const char *hay, const char *needle)
{
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return 0;
}
void *memchr(const void *s, int c, size_t n)
{ const unsigned char *p = s; while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; } return 0; }
size_t strspn(const char *s, const char *set)
{ size_t n = 0; for (; s[n]; n++) { const char *t = set; while (*t && *t != s[n]) t++; if (!*t) break; } return n; }

static int dval(int c)
{ if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10; return 99; }

unsigned long long strtoull(const char *s, char **end, int base)
{
    unsigned long long v = 0; int d;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') { base = 8; }
    else if (base == 0) base = 10;
    while ((d = dval((unsigned char)*s)) < base) { v = v * (unsigned)base + (unsigned)d; s++; }
    if (end) *end = (char *)s;
    return v;
}
long long strtoll(const char *s, char **end, int base)
{
    int neg = 0;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    { unsigned long long v = strtoull(s, end, base); return neg ? -(long long)v : (long long)v; }
}
long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtoll(s, 0, 10); }

/* simple insertion+quicksort hybrid; MicroPython's own sort is internal, this
   is only for stray libc qsort references */
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    char *a = base, tmp;
    size_t i, j, k;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && cmp(a + j * sz, a + (j - 1) * sz) < 0; j--)
            for (k = 0; k < sz; k++) { tmp = a[j*sz+k]; a[j*sz+k] = a[(j-1)*sz+k]; a[(j-1)*sz+k] = tmp; }
}

/* ---- compact vsnprintf: %d/i/u/x/X/o/c/s/p/%, l/ll/z length, width, 0/-/+ ---
   Float (%f/%g/%e) is deferred to MicroPython's own float formatter; a bare
   %f prints "<flt>" rather than pulling in a dtoa. */
static int u64_str(unsigned long long v, int base, int upper, char *out)
{
    char t[24]; int n = 0, i; const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) t[n++] = '0';
    while (v) { t[n++] = dig[v % (unsigned)base]; v /= (unsigned)base; }
    for (i = 0; i < n; i++) out[i] = t[n - 1 - i];
    return n;
}
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    size_t o = 0;
    /* Evaluate `ch` EXACTLY ONCE, before the space check - callers pass a
     * side-effecting argument (PUT(*s++)), and gating the whole statement on
     * "o+1<cap" used to skip the s++ once the buffer filled, so a TRUNCATED
     * "%s" spun `while(*s)` forever on the same byte (the S-LIBC-06 hang). */
    #define PUT(ch) do { char c_ = (char)(ch); if (o + 1 < cap) buf[o] = c_; o++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        { int left = 0, zero = 0, plus = 0, width = 0, lng = 0, i, neg = 0; char nb[24]; int nn;
          unsigned long long uv; long long sv;
          while (*fmt=='-'||*fmt=='0'||*fmt=='+'||*fmt==' '||*fmt=='#') {
              if (*fmt=='-') left=1; else if (*fmt=='0') zero=1; else if (*fmt=='+') plus=1; fmt++; }
          while (*fmt >= '0' && *fmt <= '9') width = width*10 + (*fmt++ - '0');
          if (*fmt=='.') { fmt++; while (*fmt>='0'&&*fmt<='9') fmt++; }  /* precision parsed, ignored for ints */
          while (*fmt=='l'||*fmt=='z'||*fmt=='h'||*fmt=='j'||*fmt=='t') { if (*fmt=='l') lng++; if (*fmt=='z'||*fmt=='j'||*fmt=='t') lng=2; fmt++; }
          switch (*fmt) {
          case 'd': case 'i':
              sv = lng>=2 ? va_arg(ap,long long) : (long long)va_arg(ap,int);
              if (sv < 0) { neg = 1; uv = (unsigned long long)(-sv); } else uv = (unsigned long long)sv;
              nn = u64_str(uv,10,0,nb); goto emit_num;
          case 'u': uv = lng>=2 ? va_arg(ap,unsigned long long) : (unsigned long long)va_arg(ap,unsigned); nn=u64_str(uv,10,0,nb); goto emit_num;
          case 'x': uv = lng>=2 ? va_arg(ap,unsigned long long) : (unsigned long long)va_arg(ap,unsigned); nn=u64_str(uv,16,0,nb); goto emit_num;
          case 'X': uv = lng>=2 ? va_arg(ap,unsigned long long) : (unsigned long long)va_arg(ap,unsigned); nn=u64_str(uv,16,1,nb); goto emit_num;
          case 'o': uv = lng>=2 ? va_arg(ap,unsigned long long) : (unsigned long long)va_arg(ap,unsigned); nn=u64_str(uv,8,0,nb); goto emit_num;
          case 'p': PUT('0'); PUT('x'); uv=(unsigned long long)(uintptr_t)va_arg(ap,void*); nn=u64_str(uv,16,0,nb); for(i=0;i<nn;i++) PUT(nb[i]); break;
          case 'c': PUT((char)va_arg(ap,int)); break;
          case 's': { const char *s = va_arg(ap,const char*); int pad; if(!s) s="(null)";
                      { int sl=0; while(s[sl]) sl++; pad=width-sl;
                        if(!left) while(pad-->0) PUT(' ');
                        while(*s) PUT(*s++);
                        if(left) while(pad-->0) PUT(' '); } } break;
          case 'f': case 'g': case 'e': { double dd = va_arg(ap,double); (void)dd; const char *m="<flt>"; while(*m) PUT(*m++); } break;
          case '%': PUT('%'); break;
          default: PUT('%'); if (*fmt) PUT(*fmt); break;
          }
          continue;
          emit_num:
          { int total = nn + (neg||plus?1:0), pad = width - total;
            if (!left && !zero) while (pad-- > 0) PUT(' ');
            if (neg) PUT('-'); else if (plus) PUT('+');
            if (!left && zero) while (pad-- > 0) PUT('0');
            for (i = 0; i < nn; i++) PUT(nb[i]);
            if (left) while (pad-- > 0) PUT(' '); }
        }
    }
    if (cap) buf[o < cap ? o : cap - 1] = 0;
    #undef PUT
    return (int)o;
}
int snprintf(char *buf, size_t n, const char *fmt, ...)
{ va_list ap; int r; va_start(ap,fmt); r = vsnprintf(buf,n,fmt,ap); va_end(ap); return r; }
int printf(const char *fmt, ...) { (void)fmt; return 0; }   /* no stdout stream here */
