/* ===========================================================================
 * unodoc core - the pieces every document format shares: the registered
 * allocator, the error surface, the bounds-checked byte source, and the
 * CP-1252 <-> UTF-16 boundary that the container and all three formats sit
 * on.  Format dispatch lives in the per-format files (ud_cfb.c, ...), so a
 * build links only what it uses - the unomedia arrangement.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include <string.h>

/* ---- allocator ------------------------------------------------------------ */
static void *(*g_alloc)(unsigned long);
static void  (*g_free)(void *);

void ud_set_alloc(void *(*a)(unsigned long), void (*f)(void *))
{ g_alloc = a; g_free = f; }

void *ud_alloc(unsigned long n) { return g_alloc ? g_alloc(n) : 0; }
void  ud_free(void *p)          { if (p && g_free) g_free(p); }

/* ---- error surface -------------------------------------------------------- */
static const char *g_err = "";
const char *ud_error(void)              { return g_err; }
void        ud_set_error(const char *w) { g_err = w ? w : ""; }

/* ---- byte sources --------------------------------------------------------- */
static long mem_read(void *ctx, long off, unsigned char *dst, long n)
{
    /* ud_src_read has already clamped off/n into [0,size). */
    memcpy(dst, (const unsigned char *)ctx + off, (unsigned long)n);
    return n;
}

void ud_src_mem(ud_src *s, const void *buf, long len)
{
    if (!s) return;
    s->read = mem_read;
    s->size = len < 0 ? 0 : len;
    s->ctx  = (void *)buf;      /* the source only ever reads through it */
}

long ud_src_read(const ud_src *s, long off, void *dst, long n)
{
    if (!s || !s->read || !dst) return 0;
    if (off < 0 || n <= 0) return 0;
    if (off >= s->size) return 0;
    if (n > s->size - off) n = s->size - off;
    {
        long r = s->read(s->ctx, off, (unsigned char *)dst, n);
        return r < 0 ? 0 : r;
    }
}

/* ---- CP-1252 <-> UTF-16 ---------------------------------------------------
 * Only 0x80..0x9F differ from Latin-1.  The five slots CP-1252 leaves
 * undefined (0x81 0x8D 0x8F 0x90 0x9D) map to the matching C1 control so the
 * table is total and the round trip is exact. */
static const uint16_t cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

uint16_t ud_cp1252_to_uc(unsigned char b)
{
    if (b >= 0x80 && b <= 0x9F) return cp1252_hi[b - 0x80];
    return (uint16_t)b;
}

unsigned char ud_uc_to_cp1252(uint16_t uc)
{
    int i;
    if (uc < 0x80 || (uc >= 0xA0 && uc <= 0xFF)) return (unsigned char)uc;
    for (i = 0; i < 32; i++)
        if (cp1252_hi[i] == uc) return (unsigned char)(0x80 + i);
    return '?';
}

/* ---- UTF-16 uppercase, CFB flavour ----------------------------------------
 * CFB orders directory names by uppercased UTF-16 code units.  Office only
 * ever generates names out of ASCII plus the odd Latin-1 letter and the two
 * leading control bytes 0x01/0x05, so covering ASCII + Latin-1 + the cased
 * CP-1252 specials is exact for everything we will meet; anything else is
 * left alone, which is stable (a total order) even if it is not Unicode's
 * own casing.  UNODOC.md states this limit. */
uint16_t ud_upper16(uint16_t uc)
{
    if (uc >= 'a' && uc <= 'z')          return (uint16_t)(uc - 0x20);
    if (uc >= 0x00E0 && uc <= 0x00FE && uc != 0x00F7)
                                         return (uint16_t)(uc - 0x20);
    switch (uc) {
    case 0x00FF: return 0x0178;   /* y diaeresis  */
    case 0x00B5: return 0x039C;   /* micro sign   */
    case 0x0161: return 0x0160;   /* s caron      */
    case 0x017E: return 0x017D;   /* z caron      */
    case 0x0153: return 0x0152;   /* oe ligature  */
    case 0x0192: return 0x0191;   /* f with hook  */
    default:     return uc;
    }
}

/* ---- CFB directory ordering ----------------------------------------------
 * Shorter names sort first; equal lengths compare uppercased, code unit by
 * code unit.  unodoc names are CP-1252, one byte per code unit, so the byte
 * length IS the UTF-16 length. */
int ud_name_cmp(const char *a, const char *b)
{
    unsigned long la, lb, i;
    if (!a) a = "";
    if (!b) b = "";
    la = strlen(a); lb = strlen(b);
    if (la != lb) return la < lb ? -1 : 1;
    for (i = 0; i < la; i++) {
        uint16_t ca = ud_upper16(ud_cp1252_to_uc((unsigned char)a[i]));
        uint16_t cb = ud_upper16(ud_cp1252_to_uc((unsigned char)b[i]));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return 0;
}
