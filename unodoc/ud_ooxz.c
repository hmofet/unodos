/* ===========================================================================
 * ud_ooxz.c - the OOXML package writer: a stored zip of XML parts.
 *
 * Shared by ud_xlsxw.c, ud_docxw.c and ud_pptxw.c; see ud_ooxz.h for why the
 * three formats are three files over this one rather than one file.
 *
 * PARTS ARE WRITTEN STORED.  A zip entry may be uncompressed and the package
 * is still valid OOXML - the format requires a zip container, not a compressed
 * one.  That means unodoc needs no DEFLATE compressor to write, only
 * unomedia's decompressor to read, which is a whole encoder that does not have
 * to exist or be maintained.  The cost is file size on a format whose bulk is
 * repetitive XML, and it is paid by a file the user is about to open in Excel
 * rather than by anything long-lived.
 *
 * THE CRC IS COMPUTED BIT BY BIT, no 256-entry table.  A table is several
 * times quicker and costs a kilobyte of the module's data segment
 * permanently, to speed up an operation that runs once per save on data the
 * process has just finished building.  On the arithmetic of a loadable module
 * that is the wrong way round.
 *
 * TEXT GOES OUT AS UTF-8.  unodoc's models hold CP-1252, because that is what
 * both binary formats store; an XML part is UTF-8 by declaration, so every
 * string is transcoded on the way out.  Skipping that step writes a file that
 * reads back byte-identical through a tolerant parser and mojibake through a
 * correct one - the same bug the .xlsx READER had in the other direction.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_ooxz.h"
#include <string.h>

/* ===========================================================================
 * a growable byte buffer
 * ======================================================================== */
static int zneed(zbuf *b, long n)
{
    unsigned char *np;
    long cap = b->cap ? b->cap : 4096;
    if (b->bad) return 0;
    if (b->n + n <= b->cap) return 1;
    while (cap < b->n + n) cap *= 2;
    np = (unsigned char *)ud_alloc((unsigned long)cap);
    if (!np) { b->bad = 1; return 0; }
    if (b->n) memcpy(np, b->p, (unsigned long)b->n);
    ud_free(b->p);
    b->p = np; b->cap = cap;
    return 1;
}

void zput(zbuf *b, const void *d, long n)
{ if (n > 0 && zneed(b, n)) { memcpy(b->p + b->n, d, (unsigned long)n); b->n += n; } }

void zs(zbuf *b, const char *s) { zput(b, s, (long)strlen(s)); }

void z8 (zbuf *b, unsigned v) { if (zneed(b, 1)) b->p[b->n++] = (unsigned char)v; }
void z16(zbuf *b, unsigned v) { z8(b, v); z8(b, v >> 8); }
void z32(zbuf *b, unsigned long v)
{ z16(b, (unsigned)(v & 0xFFFF)); z16(b, (unsigned)((v >> 16) & 0xFFFF)); }

void zint(zbuf *b, long v)
{
    char t[24];
    int i = 0, neg = v < 0;
    unsigned long u = (unsigned long)(neg ? -v : v);
    if (!u) { z8(b, '0'); return; }
    while (u) { t[i++] = (char)('0' + (int)(u % 10)); u /= 10; }
    if (neg) z8(b, '-');
    while (i) z8(b, (unsigned char)t[--i]);
}

/* A number for an XML `<v>`.  ud_num_text is Excel's own display convention -
 * at most 15 significant digits, trailing zeros trimmed - which is the same
 * rendering a formula literal gets, and the right one here for the same
 * reason: it is what a value that came out of a spreadsheet went in as. */
void znum(zbuf *b, double v)
{
    char t[32];
    zput(b, t, ud_num_text(v, t));
}

/* ---- XML escaping, CP-1252 in and UTF-8 out --------------------------------
 * `&`, `<` and `>` are escaped everywhere rather than only where each is
 * strictly required: `>` is legal in content, and a reader that mishandles
 * `]]>` is a reader this file has no way to hear about.  In an attribute the
 * quote goes too, which is why `quot` is a parameter. */
void zxml(zbuf *b, const char *s, int quot)
{
    long i;
    if (!s) return;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        uint16_t uc;
        if (c == '&') { zs(b, "&amp;");  continue; }
        if (c == '<') { zs(b, "&lt;");   continue; }
        if (c == '>') { zs(b, "&gt;");   continue; }
        if (c == '"' && quot) { zs(b, "&quot;"); continue; }
        /* XML 1.0 has no way to spell most control characters at all, not even
           as a numeric reference, so they are dropped rather than written as
           something a parser must then reject. */
        if (c < 0x20 && c != '\t' && c != '\n') continue;
        if (c < 0x80) { z8(b, c); continue; }
        uc = ud_cp1252_to_uc(c);
        if (uc < 0x800) {
            z8(b, 0xC0 | (uc >> 6));
            z8(b, 0x80 | (uc & 0x3F));
        } else {
            z8(b, 0xE0 | (uc >> 12));
            z8(b, 0x80 | ((uc >> 6) & 0x3F));
            z8(b, 0x80 | (uc & 0x3F));
        }
    }
}

/* ===========================================================================
 * the stored-zip writer
 * ======================================================================== */
static unsigned long crc32_of(const unsigned char *p, long n)
{
    unsigned long c = 0xFFFFFFFFuL;
    long i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320uL & (unsigned long)(-(long)(c & 1)));
    }
    return c ^ 0xFFFFFFFFuL;
}

/* A DOS timestamp of 1980-01-01 00:00.  unodoc has no clock - it is a document
 * library, not a filesystem - and a fixed epoch makes two saves of the same
 * model byte-identical, which is what makes a round-trip test meaningful. */
#define DOSDATE 0x0021u
#define DOSTIME 0x0000u

static void zw_add(zwrite *z, const char *name, const unsigned char *data, long n)
{
    long namelen = (long)strlen(name);
    if (z->n >= UD_OOXZ_MAXPART || namelen >= (long)sizeof z->e[0].name) {
        z->out.bad = 1;
        return;
    }
    z->e[z->n].off  = z->out.n;
    z->e[z->n].crc  = crc32_of(data, n);
    z->e[z->n].size = (unsigned long)n;
    memcpy(z->e[z->n].name, name, (unsigned long)namelen + 1);

    zs (&z->out, "PK\003\004");
    z16(&z->out, 20);                 /* version needed: 2.0, stored/deflate */
    z16(&z->out, 0);                  /* no flags: no descriptor, no crypto  */
    z16(&z->out, 0);                  /* method 0 = stored                   */
    z16(&z->out, DOSTIME);
    z16(&z->out, DOSDATE);
    z32(&z->out, z->e[z->n].crc);
    z32(&z->out, (unsigned long)n);   /* compressed == uncompressed          */
    z32(&z->out, (unsigned long)n);
    z16(&z->out, (unsigned)namelen);
    z16(&z->out, 0);                  /* no extra field                      */
    zput(&z->out, name, namelen);
    zput(&z->out, data, n);
    z->n++;
}

void zw_part(zwrite *z, const char *name, zbuf *body)
{
    if (body->bad) { z->out.bad = 1; return; }
    zw_add(z, name, body->p, body->n);
    ud_free(body->p);
    memset(body, 0, sizeof *body);
}

unsigned char *zw_finish(zwrite *z, long *len)
{
    long cdoff = z->out.n, cdlen;
    int i;
    unsigned char *out;

    for (i = 0; i < z->n; i++) {
        long namelen = (long)strlen(z->e[i].name);
        zs (&z->out, "PK\001\002");
        z16(&z->out, 20);             /* version made by                      */
        z16(&z->out, 20);             /* version needed                       */
        z16(&z->out, 0);
        z16(&z->out, 0);              /* stored                               */
        z16(&z->out, DOSTIME);
        z16(&z->out, DOSDATE);
        z32(&z->out, z->e[i].crc);
        z32(&z->out, z->e[i].size);
        z32(&z->out, z->e[i].size);
        z16(&z->out, (unsigned)namelen);
        z16(&z->out, 0);              /* extra                                */
        z16(&z->out, 0);              /* comment                              */
        z16(&z->out, 0);              /* disk                                 */
        z16(&z->out, 0);              /* internal attributes                  */
        z32(&z->out, 0);              /* external attributes                  */
        z32(&z->out, (unsigned long)z->e[i].off);
        zput(&z->out, z->e[i].name, namelen);
    }
    cdlen = z->out.n - cdoff;

    zs (&z->out, "PK\005\006");
    z16(&z->out, 0); z16(&z->out, 0);
    z16(&z->out, (unsigned)z->n);
    z16(&z->out, (unsigned)z->n);
    z32(&z->out, (unsigned long)cdlen);
    z32(&z->out, (unsigned long)cdoff);
    z16(&z->out, 0);                  /* no archive comment                   */

    if (z->out.bad) {
        ud_free(z->out.p);
        memset(z, 0, sizeof *z);
        ud_set_error("out of memory writing package");
        return 0;
    }
    out = z->out.p;
    if (len) *len = z->out.n;
    memset(z, 0, sizeof *z);
    return out;
}

/* Every part starts the same way. */
void xml_head(zbuf *b)
{
    zs(b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n");
}


/* The package-level `_rels/.rels`, which is the same three lines for all three
 * formats bar the part it points at. */
void root_rels(zwrite *z, const char *target)
{
    zbuf b;
    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<Relationships xmlns=\"" NS_PR "\">"
           "<Relationship Id=\"rId1\" Type=\"" RT "officeDocument\" Target=\"");
    zs(&b, target);
    zs(&b, "\"/></Relationships>");
    zw_part(z, "_rels/.rels", &b);
}

/* "sheet" + 12 + ".xml".  unodoc carries no printf, and three call sites
 * open-coding the same digit loop is three chances to get the bounds wrong. */
void zw_name(char *out, const char *prefix, int n, const char *suffix)
{
    long at = (long)strlen(prefix);
    char t[12];
    int k = 0;
    memcpy(out, prefix, (unsigned long)at);
    if (n <= 0) t[k++] = '0';
    while (n > 0) { t[k++] = (char)('0' + n % 10); n /= 10; }
    while (k) out[at++] = t[--k];
    memcpy(out + at, suffix, strlen(suffix) + 1);
}
