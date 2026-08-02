/* ===========================================================================
 * xlstest - the host gate for unodoc's BIFF8 reader (OFFICE97-PLAN §4
 * phase 2).  Built with build.sh's sanitizer set plus ASan, like cfbtest.
 *
 *   dump FILE          canonical TSV of everything the reader extracted;
 *                      run_tests.py diffs it against the fixture mkcorpus.py
 *                      wrote from the source document
 *   fuzz FILE SEED N   N mutations of FILE through the container AND the
 *                      workbook reader.  Must never crash, never hang.
 * ======================================================================== */
#include "unodoc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *t_alloc(unsigned long n) { return malloc(n ? n : 1); }

static unsigned char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    *len = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }
    b = (unsigned char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return 0; }
    fclose(f);
    *len = n;
    return b;
}

/* TSV-safe.  unodoc text is CP-1252, and the output is emitted as CP-1252
 * bytes for the runner to decode: 0xA0-0xFF go out raw (they are exactly the
 * printable half of CP-1252), while control bytes AND the five CP-1252
 * undefined slots in 0x7F-0x9F are escaped, so what reaches the runner is
 * always decodable and a real difference can never hide behind a
 * replacement character. */
static void put_escaped(const char *s)
{
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\t')      fputs("\\t", stdout);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\\') fputs("\\\\", stdout);
        else if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) printf("\\x%02x", c);
        else                putchar((int)c);
    }
}

static void dump(ud_xls *x)
{
    int s;
    printf("book\tdate1904=%d\tsheets=%d\n", ud_xls_date1904(x),
           ud_xls_sheets(x));
    for (s = 0; s < ud_xls_sheets(x); s++) {
        int i, n = ud_xls_cell_count(x, s);
        printf("sheet\t%d\t", s);
        put_escaped(ud_xls_sheet_name(x, s));
        printf("\tvisible=%d\trows=%d\tcols=%d\tcells=%d\n",
               ud_xls_sheet_visible(x, s), ud_xls_rows(x, s),
               ud_xls_cols(x, s), n);
        for (i = 0; i < n; i++) {
            int row = 0, col = 0;
            ud_xcell c;
            if (!ud_xls_cell_at(x, s, i, &row, &col, &c)) continue;
            printf("cell\t%d\t%d\t%d\t", s, row, col);
            switch (c.kind) {
            case UD_XV_NUM:  printf("NUM\t%.15g", c.num); break;
            case UD_XV_BOOL: printf("BOOL\t%d", c.num != 0); break;
            case UD_XV_ERR:  printf("ERR\t%s", ud_xls_err_text(c.err)); break;
            case UD_XV_STR:  printf("STR\t"); put_escaped(c.str); break;
            default:         printf("EMPTY\t"); break;
            }
            printf("\tf=%d\tfmt=", c.formula);
            put_escaped(ud_xls_xf_format(x, c.xf));
            printf("\n");
            /* the random-access path must agree with the iteration path */
            {
                ud_xcell d;
                if (!ud_xls_cell(x, s, row, col, &d) || d.kind != c.kind)
                    printf("!! lookup disagrees at %d,%d\n", row, col);
            }
        }
        for (i = 0; i < ud_xls_merges(x, s); i++) {
            int r0, c0, r1, c1;
            if (ud_xls_merge(x, s, i, &r0, &c0, &r1, &c1))
                printf("merge\t%d\t%d\t%d\t%d\t%d\n", s, r0, c0, r1, c1);
        }
    }
}

/* open the container and the workbook inside it; 1 if the workbook opened */
static int walk(const unsigned char *buf, long len, int print)
{
    ud_src src;
    ud_cfb *c;
    ud_xls *x;
    int ok = 0;

    ud_src_mem(&src, buf, len);
    c = ud_cfb_open(&src);
    if (!c) {
        if (print) printf("ERR: %s\n", ud_error());
        return 0;
    }
    x = ud_xls_open(c);
    if (x) {
        ok = 1;
        if (print) dump(x);
        else {
            /* still touch everything, so the fuzzer exercises the readers */
            int s;
            for (s = 0; s < ud_xls_sheets(x); s++) {
                int i, n = ud_xls_cell_count(x, s);
                for (i = 0; i < n; i++) {
                    ud_xcell cc;
                    int r = 0, cl = 0;
                    ud_xls_cell_at(x, s, i, &r, &cl, &cc);
                    if (cc.kind == UD_XV_STR && cc.str) (void)strlen(cc.str);
                    (void)ud_xls_xf_format(x, cc.xf);
                }
            }
        }
        ud_xls_close(x);
    } else if (print) {
        printf("ERR: %s\n", ud_error());
    }
    ud_cfb_close(c);
    return ok;
}

/* ===========================================================================
 * selftest: a hand-assembled workbook that splits shared strings across
 * CONTINUE boundaries AND CHANGES THE ENCODING at the split.
 *
 * Why this exists.  The generated corpus does produce mid-string splits (54
 * of them in sst.xls, measured), but LibreOffice always restates the SAME
 * flag, so the case where a string starts 8-bit and finishes UTF-16 - which
 * [MS-XLS] permits and Excel emits - is never reached by any file we can
 * generate.  It is also precisely the case that silently corrupts a naive
 * reader.  So the workbook is built here by hand, byte by byte, with the
 * split placed on purpose.  Writing .xls properly is phase 3; this is just
 * enough BIFF to put the reader in front of the shape that matters.
 * ======================================================================== */
typedef struct { unsigned char *p; long n, cap; } buf;

static void bneed(buf *b, long n)
{
    if (b->n + n <= b->cap) return;
    b->cap = (b->n + n) * 2 + 256;
    b->p = (unsigned char *)realloc(b->p, (size_t)b->cap);
}
static void bput(buf *b, const void *d, long n)
{ bneed(b, n); memcpy(b->p + b->n, d, (size_t)n); b->n += n; }
static void bu8 (buf *b, unsigned v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
static void bu16(buf *b, unsigned v) { unsigned char c[2] = { (unsigned char)v,
                                       (unsigned char)(v >> 8) }; bput(b, c, 2); }
static void bu32(buf *b, unsigned long v)
{ unsigned char c[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                         (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
  bput(b, c, 4); }

/* append `s`'s characters in the chosen width */
static void bchars(buf *b, const char *s, long from, long to, int wide)
{
    long i;
    for (i = from; i < to; i++) {
        if (wide) bu16(b, (unsigned char)s[i]);   /* CP-1252 == Latin-1 here */
        else      bu8 (b, (unsigned char)s[i]);
    }
}

/* the four strings, and how each is split: {first half wide?, second wide?} */
static const char *SS[4] = {
    "plain-ascii-string-that-is-cut-in-half-right-here-ok",
    "wide-all-the-way-through-with-an-accent-\xE9-in-it-too",
    "starts-narrow-then-turns-WIDE-at-the-split-\xE0-here",
    "starts-wide-then-turns-narrow-at-the-split-\xFC-here"
};
static const int W0[4] = { 0, 1, 0, 1 };
static const int W1[4] = { 0, 1, 1, 0 };

static int selftest(void)
{
    buf blocks[8];
    int nblk = 0, i, bad = 0;
    buf strm;
    ud_cfbw *w;
    unsigned char *img;
    long imglen = 0, sheet_at;
    ud_src src;
    ud_cfb *c;
    ud_xls *x;

    memset(blocks, 0, sizeof blocks);
    memset(&strm, 0, sizeof strm);

    /* ---- the SST payload, cut mid-string once per string --------------- */
    bu32(&blocks[0], 4);                       /* cstTotal                  */
    bu32(&blocks[0], 4);                       /* cstUnique                 */
    for (i = 0; i < 4; i++) {
        long len = (long)strlen(SS[i]), half = len / 2;
        buf *cur = &blocks[nblk];
        bu16(cur, (unsigned)len);
        bu8 (cur, (unsigned)W0[i]);            /* grbit: just fHighByte     */
        bchars(cur, SS[i], 0, half, W0[i]);
        cur = &blocks[++nblk];                 /* <-- the CONTINUE boundary */
        bu8 (cur, (unsigned)W1[i]);            /* the RESTATED flag         */
        bchars(cur, SS[i], half, len, W1[i]);
    }
    nblk++;

    /* ---- globals substream --------------------------------------------- */
    { unsigned char bof[16];
      memset(bof, 0, sizeof bof);
      bof[0] = 0x00; bof[1] = 0x06;            /* vers = BIFF8              */
      bof[2] = 0x05; bof[3] = 0x00;            /* dt = globals              */
      bu16(&strm, 0x0809); bu16(&strm, 16); bput(&strm, bof, 16); }

    /* BOUNDSHEET, with lbPlyPos backpatched once the sheet is placed */
    { long patch;
      bu16(&strm, 0x0085); bu16(&strm, 4 + 1 + 1 + 2 + 5);
      patch = strm.n;
      bu32(&strm, 0);                          /* lbPlyPos - patched below  */
      bu8(&strm, 0); bu8(&strm, 0);            /* visible, worksheet        */
      bu8(&strm, 5); bu8(&strm, 0);            /* cch = 5, 8-bit            */
      bput(&strm, "Sheet", 5);

      /* SST + its CONTINUEs */
      bu16(&strm, 0x00FC); bu16(&strm, (unsigned)blocks[0].n);
      bput(&strm, blocks[0].p, blocks[0].n);
      for (i = 1; i < nblk; i++) {
          bu16(&strm, 0x003C); bu16(&strm, (unsigned)blocks[i].n);
          bput(&strm, blocks[i].p, blocks[i].n);
      }
      bu16(&strm, 0x000A); bu16(&strm, 0);     /* EOF of globals            */

      /* ---- the sheet substream --------------------------------------- */
      sheet_at = strm.n;
      strm.p[patch + 0] = (unsigned char)sheet_at;
      strm.p[patch + 1] = (unsigned char)(sheet_at >> 8);
      strm.p[patch + 2] = (unsigned char)(sheet_at >> 16);
      strm.p[patch + 3] = (unsigned char)(sheet_at >> 24);
    }
    { unsigned char bof[16];
      memset(bof, 0, sizeof bof);
      bof[0] = 0x00; bof[1] = 0x06;
      bof[2] = 0x10; bof[3] = 0x00;            /* dt = worksheet            */
      bu16(&strm, 0x0809); bu16(&strm, 16); bput(&strm, bof, 16); }
    for (i = 0; i < 4; i++) {                  /* LABELSST at A1..A4        */
        bu16(&strm, 0x00FD); bu16(&strm, 10);
        bu16(&strm, (unsigned)i); bu16(&strm, 0); bu16(&strm, 15);
        bu32(&strm, (unsigned long)i);
    }
    bu16(&strm, 0x000A); bu16(&strm, 0);

    /* ---- wrap it in a container and read it back ----------------------- */
    w = ud_cfbw_new();
    ud_cfbw_stream(w, UD_CFB_ROOT_ID, "Workbook", strm.p, strm.n);
    img = ud_cfbw_serialize(w, &imglen);
    ud_cfbw_free(w);
    for (i = 0; i < nblk; i++) free(blocks[i].p);
    free(strm.p);
    if (!img) { printf("FAIL selftest: could not build the workbook\n"); return 1; }

    ud_src_mem(&src, img, imglen);
    c = ud_cfb_open(&src);
    x = c ? ud_xls_open(c) : 0;
    if (!x) {
        printf("FAIL selftest: %s\n", ud_error());
        ud_cfb_close(c); ud_free(img);
        return 1;
    }
    for (i = 0; i < 4; i++) {
        ud_xcell cell;
        if (!ud_xls_cell(x, 0, i, 0, &cell) || cell.kind != UD_XV_STR ||
            strcmp(cell.str, SS[i]) != 0) {
            printf("FAIL selftest: string %d (%d->%d bit) came back as \"",
                   i, W0[i] ? 16 : 8, W1[i] ? 16 : 8);
            put_escaped(cell.kind == UD_XV_STR ? cell.str : "<not a string>");
            printf("\"\n  want \"");
            put_escaped(SS[i]);
            printf("\"\n");
            bad = 1;
        }
    }
    ud_xls_close(x);
    ud_cfb_close(c);
    ud_free(img);
    if (!bad)
        printf("xlstest: selftest OK - 4 shared strings split mid-character "
               "across CONTINUE, including 8->16 and 16->8 bit switches\n");
    return bad;
}

int main(int argc, char **argv)
{
    ud_set_alloc(t_alloc, free);

    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) return selftest();

    if (argc >= 3 && strcmp(argv[1], "dump") == 0) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        int ok;
        if (!b) { printf("ERR: cannot read %s\n", argv[2]); return 2; }
        ok = walk(b, n, 1);
        free(b);
        return ok ? 0 : 1;
    }
    if (argc >= 5 && strcmp(argv[1], "fuzz") == 0) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        unsigned seed = (unsigned)strtoul(argv[3], 0, 10);
        long iters = strtol(argv[4], 0, 10), t, opened = 0;
        if (!b || n < 512) { printf("ERR: cannot read %s\n", argv[2]); free(b); return 2; }
        for (t = 0; t < iters; t++) {
            unsigned char *m = (unsigned char *)malloc((size_t)n);
            int k, nmut;
            memcpy(m, b, (size_t)n);
            seed = seed * 1103515245u + 12345u;
            nmut = (int)((seed >> 16) % 12u) + 1;
            for (k = 0; k < nmut; k++) {
                long off;
                seed = seed * 1103515245u + 12345u;
                off = (long)((seed >> 8) % (unsigned long)n);
                seed = seed * 1103515245u + 12345u;
                m[off] = (unsigned char)(seed >> 16);
            }
            opened += walk(m, n, 0);
            free(m);
        }
        printf("OK %ld mutations, %ld workbooks opened\n", iters, opened);
        free(b);
        return 0;
    }
    printf("usage: xlstest dump FILE | fuzz FILE SEED N\n");
    return 2;
}
