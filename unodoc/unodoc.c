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

/* ---- number to text -------------------------------------------------------
 * Written out because unodoc links no libc beyond mem-/str-, and a formula
 * literal has to be rendered somehow.  Integer-only apart from the scaling
 * multiply, and no libm: the powers of ten are a table. */
int ud_int_text(long v, char *out)
{
    char tmp[24];
    int n = 0, len = 0;
    unsigned long u;

    if (v < 0) { out[len++] = '-'; u = (unsigned long)(-(v + 1)) + 1; }
    else        u = (unsigned long)v;
    do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    while (n) out[len++] = tmp[--n];
    out[len] = 0;
    return len;
}

/* 1e0 .. 1e22 are all exactly representable; beyond that the table is the
 * nearest double, which is what any scaling would land on anyway. */
static double pow10_of(int e)
{
    static const double P[23] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10,
        1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
    };
    double r = 1.0;
    int neg = e < 0;
    if (neg) e = -e;
    while (e > 22) { r *= 1e22; e -= 22; }
    r *= P[e];
    return neg ? 1.0 / r : r;
}

#define SIGDIG 15

int ud_num_text(double v, char *out)
{
    int len = 0, exp10 = 0, i, ndig;
    unsigned long long m;
    char dig[SIGDIG + 2];
    double a;

    if (v != v) { memcpy(out, "#NUM!", 6); return 5; }      /* NaN          */
    if (v == 0) { memcpy(out, "0", 2); return 1; }
    if (v < 0) { out[len++] = '-'; v = -v; }
    if (v > 1.7e308) { memcpy(out + len, "#NUM!", 6); return len + 5; }

    /* Scale into [1e14, 1e15) so 15 significant digits land in a uint64
       exactly (1e15 < 2^53), then read them off.  The exponent is estimated
       from the binary one (log10(2) as a ratio, no libm) so the scaling is a
       SINGLE multiply for any magnitude - repeated division by ten would
       accumulate a rounding error into the digits we are about to print. */
    {
        uint64_t bits;
        int be, est;
        memcpy(&bits, &v, 8);
        be = (int)((bits >> 52) & 0x7FF) - 1023;
        est = (int)(((long)be * 30103L) / 100000L);
        exp10 = 14 - est;
        a = v * pow10_of(exp10);
        exp10 = -exp10;
    }
    while (a >= 1e15) { a /= 10.0; exp10++; }   /* at most a step or two    */
    while (a <  1e14) { a *= 10.0; exp10--; }
    m = (unsigned long long)(a + 0.5);
    if (m >= 1000000000000000ULL) { m /= 10; exp10++; }     /* rounding up  */

    for (i = SIGDIG - 1; i >= 0; i--) { dig[i] = (char)('0' + (m % 10)); m /= 10; }
    ndig = SIGDIG;
    while (ndig > 1 && dig[ndig - 1] == '0') ndig--;        /* trim zeros   */

    /* decimal exponent of the leading digit */
    exp10 += SIGDIG - 1;

    if (exp10 >= -5 && exp10 < SIGDIG) {                    /* plain form   */
        if (exp10 >= 0) {
            for (i = 0; i <= exp10; i++)
                out[len++] = i < ndig ? dig[i] : '0';
            if (ndig > exp10 + 1) {
                out[len++] = '.';
                for (i = exp10 + 1; i < ndig; i++) out[len++] = dig[i];
            }
        } else {
            out[len++] = '0'; out[len++] = '.';
            for (i = 0; i < -exp10 - 1; i++) out[len++] = '0';
            for (i = 0; i < ndig; i++) out[len++] = dig[i];
        }
    } else {                                                /* E notation   */
        out[len++] = dig[0];
        if (ndig > 1) {
            out[len++] = '.';
            for (i = 1; i < ndig; i++) out[len++] = dig[i];
        }
        out[len++] = 'E';
        out[len++] = exp10 < 0 ? '-' : '+';
        len += ud_int_text(exp10 < 0 ? -exp10 : exp10, out + len);
        return len;
    }
    out[len] = 0;
    return len;
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
