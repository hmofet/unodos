/* ===========================================================================
 * ud_zip.c - the ZIP container, which is what an OOXML document is.
 *
 * A .docx / .xlsx / .pptx is a ZIP holding XML parts, exactly as a .doc /
 * .xls / .ppt is a CFB holding binary streams.  This file is to OOXML what
 * ud_cfb.c is to the 97 formats, and it deliberately mirrors that API: open a
 * container over a ud_src, find a part by name, load it into one buffer.
 *
 * WE READ THE CENTRAL DIRECTORY, not the local headers, and the difference
 * matters.  A local header may carry zeroed sizes with the real ones in a
 * trailing data descriptor (the streaming writers do this, and Word is one of
 * them); the central directory always has the true sizes.  The local header is
 * still read - but only for its two length fields, because the name and extra
 * fields there may be longer than the central copy and the payload starts
 * after them.
 *
 * DEFLATE COMES FROM unomedia.  um_inflate() is PNG's engine, exported by
 * unomedia.h as "a general facility future formats will want"; this is that
 * future format.  Reimplementing 500 lines of Huffman decoding here to keep
 * unodoc dependency-free would be the wrong trade - AGENTS.md §2 says consume
 * a neighbour's public API rather than duplicate it - so a consumer of the
 * OOXML readers links unomedia too and registers BOTH allocators.  That is
 * stated in UNODOC.md because it is the one new obligation on callers.
 *
 * WHAT IS NOT SUPPORTED, refused rather than mis-read: encryption (a bit in
 * the flags), ZIP64 beyond the 4 GB fields, and any compression method other
 * than 0 (stored) and 8 (deflate).  Office writes neither of the first two for
 * an ordinary document and nothing but those two methods.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "../unomedia/unomedia.h"
#include <string.h>
#include <stdint.h>

#define EOCD_SIG   0x06054b50u
#define EOCD64_SIG 0x06064b50u
#define EOCDL_SIG  0x07064b50u
#define CD_SIG     0x02014b50u
#define LF_SIG     0x04034b50u

#define EOCD_MIN   22            /* an end-of-central-directory with no comment */
#define ZIP_MAXCOM 65535         /* the comment field is a u16 length           */
#define ZIP_MAXENT 4096          /* parts in one document; Office writes ~30    */
#define ZIP_MAXPART (32L * 1024 * 1024)   /* one inflated part                  */

typedef struct {
    char     name[128];          /* the part name, "xl/worksheets/sheet1.xml"  */
    uint16_t method;
    uint32_t csize, usize;       /* compressed / uncompressed                  */
    uint32_t lhoff;              /* local file header offset                   */
} ud_zent;

struct ud_zip {
    ud_src   src;
    ud_zent *e;
    int      n;
};

/* ---- little-endian reads out of a scratch buffer --------------------------- */
static uint16_t z16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t z32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int src_read(const ud_src *s, long off, void *dst, long n)
{
    if (off < 0 || n < 0 || off + n > s->size) return 0;
    return s->read(s->ctx, off, dst, n) == n;
}

/* ---- the end-of-central-directory record -----------------------------------
 * It is at the END of the file, after a variable-length comment, so it is
 * found by scanning backwards for its signature.  A comment that happens to
 * contain the signature would fool a naive scan, so the candidate is checked:
 * its own comment length must reach exactly the end of the file. */
static int find_eocd(const ud_src *src, long *at)
{
    long span = src->size < (long)(EOCD_MIN + ZIP_MAXCOM)
              ? src->size : (long)(EOCD_MIN + ZIP_MAXCOM);
    unsigned char *buf;
    long base = src->size - span, i;
    int found = 0;

    if (src->size < EOCD_MIN) { ud_set_error("not a zip (too short)"); return 0; }
    buf = (unsigned char *)ud_alloc((unsigned long)span);
    if (!buf) { ud_set_error("out of memory (zip directory scan)"); return 0; }
    if (!src_read(src, base, buf, span)) {
        ud_free(buf);
        ud_set_error("zip: short read scanning for the directory");
        return 0;
    }
    for (i = span - EOCD_MIN; i >= 0; i--) {
        if (z32(buf + i) != EOCD_SIG) continue;
        if ((long)z16(buf + i + 20) != span - i - EOCD_MIN) continue;
        *at = base + i;
        found = 1;
        break;
    }
    ud_free(buf);
    if (!found) ud_set_error("not a zip (no end-of-central-directory record)");
    return found;
}

/* ---- opening ---------------------------------------------------------------- */
ud_zip *ud_zip_open(const ud_src *src)
{
    unsigned char hdr[46];
    ud_zip *z;
    long eocd, cd, at;
    int n, i;

    if (!src || !src->read || src->size <= 0) {
        ud_set_error("zip: no source");
        return 0;
    }
    if (!find_eocd(src, &eocd)) return 0;
    if (!src_read(src, eocd, hdr, EOCD_MIN)) {
        ud_set_error("zip: short read at the directory record");
        return 0;
    }
    n  = (int)z16(hdr + 10);              /* entries in this disk             */
    cd = (long)z32(hdr + 16);             /* central directory offset         */
    if (z16(hdr + 4) || z16(hdr + 6)) {
        ud_set_error("zip: split archives are not supported");
        return 0;
    }
    /* ZIP64 marks its counts as 0xFFFF/0xFFFFFFFF and puts the real ones in a
     * separate record.  An Office document never needs it, so say so plainly
     * rather than reading a 4 GB offset as -1. */
    if (n == 0xFFFF || (uint32_t)cd == 0xFFFFFFFFu || z32(hdr + 12) == 0xFFFFFFFFu) {
        ud_set_error("zip: ZIP64 is not supported");
        return 0;
    }
    if (n < 0 || n > ZIP_MAXENT) { ud_set_error("zip: too many entries"); return 0; }
    if (cd < 0 || cd > src->size)  { ud_set_error("zip: bad directory offset"); return 0; }

    z = (ud_zip *)ud_alloc(sizeof(ud_zip));
    if (!z) { ud_set_error("out of memory (zip)"); return 0; }
    memset(z, 0, sizeof *z);
    z->src = *src;
    z->e = (ud_zent *)ud_alloc((unsigned long)(n ? n : 1) * sizeof(ud_zent));
    if (!z->e) { ud_free(z); ud_set_error("out of memory (zip entries)"); return 0; }
    memset(z->e, 0, (unsigned long)(n ? n : 1) * sizeof(ud_zent));

    at = cd;
    for (i = 0; i < n; i++) {
        ud_zent *e = &z->e[z->n];
        int nlen, xlen, clen;
        uint16_t flags;
        if (!src_read(src, at, hdr, 46) || z32(hdr) != CD_SIG) {
            ud_set_error("zip: damaged central directory");
            ud_zip_close(z);
            return 0;
        }
        flags     = z16(hdr + 8);
        e->method = z16(hdr + 10);
        e->csize  = z32(hdr + 20);
        e->usize  = z32(hdr + 24);
        nlen      = (int)z16(hdr + 28);
        xlen      = (int)z16(hdr + 30);
        clen      = (int)z16(hdr + 32);
        e->lhoff  = z32(hdr + 42);
        at += 46;
        if (flags & 1) {
            ud_set_error("zip: the document is encrypted");
            ud_zip_close(z);
            return 0;
        }
        if (nlen > 0 && nlen < (int)sizeof e->name) {
            if (!src_read(src, at, e->name, nlen)) {
                ud_set_error("zip: short read on a part name");
                ud_zip_close(z);
                return 0;
            }
            e->name[nlen] = 0;
            /* a part name is stored with forward slashes; normalise anyway so
             * a writer that used backslashes still resolves */
            { int k; for (k = 0; k < nlen; k++) if (e->name[k] == '\\') e->name[k] = '/'; }
            z->n++;
        }
        /* a name longer than the buffer is skipped rather than truncated: a
         * truncated name could collide with a real part */
        at += nlen + xlen + clen;
        if (at > src->size) {
            ud_set_error("zip: directory runs past the end of the file");
            ud_zip_close(z);
            return 0;
        }
    }
    return z;
}

void ud_zip_close(ud_zip *z)
{
    if (!z) return;
    ud_free(z->e);
    ud_free(z);
}

int ud_zip_parts(const ud_zip *z) { return z ? z->n : 0; }

const char *ud_zip_name(const ud_zip *z, int i)
{
    if (!z || i < 0 || i >= z->n) return "";
    return z->e[i].name;
}

int ud_zip_find(const ud_zip *z, const char *name)
{
    int i;
    if (!z || !name) return -1;
    /* A leading slash is how the CFB half of unodoc spells a path, and it is
     * the mistake a caller ported from ud_cfb_find() will make.  Accept it. */
    if (*name == '/') name++;
    for (i = 0; i < z->n; i++) if (!strcmp(z->e[i].name, name)) return i;
    /* OOXML part names are not case sensitive in practice and some writers
     * differ on "xl/SharedStrings.xml"; fall back to a fold-insensitive match
     * rather than reporting a missing part. */
    for (i = 0; i < z->n; i++) {
        const char *a = z->e[i].name, *b = name;
        while (*a && *b) {
            int ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
            int cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*a && !*b) return i;
    }
    return -1;
}

long ud_zip_size(const ud_zip *z, int i)
{
    if (!z || i < 0 || i >= z->n) return -1;
    return (long)z->e[i].usize;
}

/* ---- inflating one part ------------------------------------------------------
 * um_inflate pulls compressed bytes through a callback and pushes decompressed
 * runs through another, so both sides are a few lines of cursor arithmetic. */
typedef struct {
    const ud_src *src;
    long at, left;
} zin;

static long zin_read(void *ctx, unsigned char *dst, long max)
{
    zin *s = (zin *)ctx;
    long n = s->left < max ? s->left : max;
    if (n <= 0) return 0;
    if (!src_read(s->src, s->at, dst, n)) return 0;
    s->at += n;
    s->left -= n;
    return n;
}

typedef struct {
    unsigned char *out;
    long cap, n;
    int  over;
} zout;

static int zout_write(void *ctx, const unsigned char *p, long n)
{
    zout *o = (zout *)ctx;
    if (n < 0 || o->n + n > o->cap) { o->over = 1; return 0; }
    memcpy(o->out + o->n, p, (unsigned long)n);
    o->n += n;
    return 1;
}

unsigned char *ud_zip_load(ud_zip *z, int i, long *len)
{
    unsigned char lh[30];
    ud_zent *e;
    unsigned char *out;
    long data;

    if (len) *len = 0;
    if (!z || i < 0 || i >= z->n) { ud_set_error("zip: no such part"); return 0; }
    e = &z->e[i];
    if (e->usize > ZIP_MAXPART) { ud_set_error("zip: part too large"); return 0; }

    /* The payload starts after the LOCAL header, whose name and extra fields
     * may be longer than the central directory's copies - so their lengths are
     * read from the local header and nothing else is. */
    if (!src_read(&z->src, (long)e->lhoff, lh, 30) || z32(lh) != LF_SIG) {
        ud_set_error("zip: damaged local header");
        return 0;
    }
    data = (long)e->lhoff + 30 + (long)z16(lh + 26) + (long)z16(lh + 28);
    if (data < 0 || data + (long)e->csize > z->src.size) {
        ud_set_error("zip: part runs past the end of the file");
        return 0;
    }

    out = (unsigned char *)ud_alloc((unsigned long)e->usize + 1);
    if (!out) { ud_set_error("out of memory (zip part)"); return 0; }
    out[e->usize] = 0;                 /* every part here is XML: NUL-terminate */

    if (e->method == 0) {
        if (e->csize != e->usize ||
            !src_read(&z->src, data, out, (long)e->usize)) {
            ud_free(out);
            ud_set_error("zip: damaged stored part");
            return 0;
        }
    } else if (e->method == 8) {
        zin in;
        zout o;
        in.src = &z->src; in.at = data; in.left = (long)e->csize;
        o.out = out; o.cap = (long)e->usize; o.n = 0; o.over = 0;
        if (!um_inflate(zin_read, &in, zout_write, &o, 0) ||
            o.n != (long)e->usize) {
            ud_free(out);
            /* The two failures are different problems and the difference is
             * the whole diagnosis: a size mismatch is a truncated or lying
             * directory, a decode failure is corruption. */
            ud_set_error(o.over || o.n != (long)e->usize
                         ? "zip: part does not match its recorded size"
                         : "zip: could not inflate a part");
            return 0;
        }
    } else {
        ud_free(out);
        ud_set_error("zip: unsupported compression method");
        return 0;
    }
    if (len) *len = (long)e->usize;
    return out;
}

/* ---- what kind of document is this? ------------------------------------------
 * By the parts it carries, never by the file name.  An extension is a hint the
 * user typed; the container is the fact - and the three OOXML formats differ
 * only in their parts. */
int ud_zip_kind(const ud_zip *z)
{
    if (!z) return UD_K_UNKNOWN;
    if (ud_zip_find(z, "xl/workbook.xml") >= 0 ||
        ud_zip_find(z, "xl/workbook.bin") >= 0)      return UD_K_XLSX;
    if (ud_zip_find(z, "word/document.xml") >= 0)    return UD_K_DOCX;
    if (ud_zip_find(z, "ppt/presentation.xml") >= 0) return UD_K_PPTX;
    return UD_K_UNKNOWN;
}

/* ---- the front door ----------------------------------------------------------
 * One look at the first bytes, so an app does not have to guess from the file
 * name which of the two containers it is holding. */
int ud_sniff(const ud_src *src)
{
    unsigned char sig[8];
    static const unsigned char CFB[8] =
        { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    if (!src || !src->read || src->size < 8) return UD_C_UNKNOWN;
    if (!src_read(src, 0, sig, 8)) return UD_C_UNKNOWN;
    if (!memcmp(sig, CFB, 8)) return UD_C_CFB;
    /* "PK\3\4" is a local header; "PK\5\6" is an empty archive's EOCD. */
    if (sig[0] == 'P' && sig[1] == 'K' &&
        ((sig[2] == 3 && sig[3] == 4) || (sig[2] == 5 && sig[3] == 6) ||
         (sig[2] == 7 && sig[3] == 8)))
        return UD_C_ZIP;
    return UD_C_UNKNOWN;
}
