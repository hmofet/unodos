/* ===========================================================================
 * doctest - the host gate for unodoc's .doc reader (OFFICE97-PLAN §4 phase 4).
 *
 *   text FILE          the document's reading text, for run_tests.py to
 *                      compare against LibreOffice's own text conversion
 *   info FILE          pieces, character count, and the raw control-character
 *                      histogram - what the piece table actually produced
 *   fuzz FILE SEED N   mutations through the container AND the document
 *                      reader.  Must never crash, never hang.
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

static ud_doc *open_doc(const unsigned char *buf, long len, ud_src *src, ud_cfb **cc)
{
    ud_cfb *c;
    ud_src_mem(src, buf, len);
    c = ud_cfb_open(src);
    *cc = c;
    return c ? ud_doc_open(c) : 0;
}

/* ===========================================================================
 * selftest: a Word document assembled by hand, with FOUR text pieces in a
 * deliberate order.
 *
 * Why this exists.  Phase 4a shipped with a hole in its evidence, recorded at
 * the time: every document LibreOffice can produce is single-piece, so the
 * multi-piece walk - the entire reason a piece table exists - was implemented
 * and unproven.  Nothing we can generate produces the layout a real
 * quick-saved Word file has.  So the document is built here byte by byte,
 * with two properties no generated file has:
 *
 *   - the runs sit in the stream in a DIFFERENT order from the one the piece
 *     table gives, so a reader that walks the file instead of the table gets
 *     the text scrambled rather than merely different;
 *   - the pieces ALTERNATE between 8-bit and UTF-16, so a reader that decides
 *     the encoding once gets half the document as mojibake.
 *
 * Both are the mistakes this phase exists to not make.
 * ======================================================================== */
typedef struct { unsigned char *p; long n, cap; } dbuf;

static void dneed(dbuf *b, long n)
{
    if (b->n + n <= b->cap) return;
    b->cap = (b->n + n) * 2 + 1024;
    b->p = (unsigned char *)realloc(b->p, (size_t)b->cap);
}
static void dput(dbuf *b, const void *d, long n)
{ dneed(b, n); memcpy(b->p + b->n, d, (size_t)n); b->n += n; }
static void d16(dbuf *b, unsigned v)
{ unsigned char c[2] = { (unsigned char)v, (unsigned char)(v >> 8) }; dput(b, c, 2); }
static void d32(dbuf *b, unsigned long v)
{ unsigned char c[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                         (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
  dput(b, c, 4); }
static void dzero(dbuf *b, long n)
{ dneed(b, n); memset(b->p + b->n, 0, (size_t)n); b->n += n; }

/* the four pieces, alternating encodings; \r is Word's paragraph mark */
static const char *PC[4] = {
    "First piece, eight bit. ",
    "Second piece, sixteen bit. ",
    "Third piece, eight bit again.\r",
    "Fourth piece, wide, with an accent \xE9.\r"
};
static const int PCWIDE[4] = { 0, 1, 0, 1 };
/* the order the runs are PLACED in the stream - deliberately not 0,1,2,3 */
static const int PCORDER[4] = { 2, 0, 3, 1 };

#define FIB_CSW   14
#define FIB_CSLW  22
#define FIB_CBFC  93
#define FIB_LEN   (32 + 2 + FIB_CSW * 2 + 2 + FIB_CSLW * 4 + 2 + FIB_CBFC * 8)
#define TEXT_AT   2048          /* where the runs start in WordDocument */

static int doc_selftest(void)
{
    dbuf wd, tbl;
    long at[4], ccp = 0, i;
    char want[512];
    unsigned char *img;
    long imglen = 0;
    ud_cfbw *w;
    ud_src src;
    ud_cfb *c;
    ud_doc *d;
    int bad = 0, whichtbl;

    for (whichtbl = 0; whichtbl < 2; whichtbl++) {
        memset(&wd, 0, sizeof wd);
        memset(&tbl, 0, sizeof tbl);
        ccp = 0;
        want[0] = 0;
        for (i = 0; i < 4; i++) ccp += (long)strlen(PC[i]);
        for (i = 0; i < 4; i++) strcat(want, PC[i]);

        /* ---- WordDocument: the FIB, then the runs in scrambled order ---- */
        d16(&wd, 0xA5EC);                       /* wIdent                   */
        d16(&wd, 0x00C1);                       /* nFib: Word 97            */
        dzero(&wd, 6);
        d16(&wd, whichtbl ? 0x0200 : 0x0000);   /* fWhichTblStm at 0x0A     */
        dzero(&wd, 32 - 12);
        d16(&wd, FIB_CSW);
        dzero(&wd, FIB_CSW * 2);
        d16(&wd, FIB_CSLW);
        for (i = 0; i < FIB_CSLW; i++) d32(&wd, i == 3 ? (unsigned long)ccp : 0);
        d16(&wd, FIB_CBFC);
        for (i = 0; i < FIB_CBFC * 2; i++) d32(&wd, 0);   /* patched below  */
        if (wd.n != FIB_LEN) {
            printf("FAIL doc selftest: FIB is %ld bytes, expected %d\n",
                   wd.n, FIB_LEN);
            return 1;
        }
        dzero(&wd, TEXT_AT - wd.n);
        for (i = 0; i < 4; i++) {
            int k = PCORDER[i];
            long len = (long)strlen(PC[k]), j;
            at[k] = wd.n;
            for (j = 0; j < len; j++) {
                if (PCWIDE[k]) d16(&wd, (unsigned char)PC[k][j]);
                else           dput(&wd, PC[k] + j, 1);
            }
        }

        /* ---- the table stream: a CLX holding a four-entry piece table --- */
        {
            long lcb = 4 * 5 + 8 * 4;           /* (n+1) CPs + n Pcds       */
            long cp = 0;
            tbl.n = 0;
            dzero(&tbl, 64);                    /* fcClx points past this   */
            dput(&tbl, "\002", 1);
            d32(&tbl, (unsigned long)lcb);
            for (i = 0; i < 4; i++) { d32(&tbl, (unsigned long)cp); cp += (long)strlen(PC[i]); }
            d32(&tbl, (unsigned long)cp);
            for (i = 0; i < 4; i++) {
                unsigned long fc = PCWIDE[i] ? (unsigned long)at[i]
                                             : ((unsigned long)at[i] * 2) | 0x40000000UL;
                d16(&tbl, 0);
                d32(&tbl, fc);
                d16(&tbl, 0);
            }
            /* patch rgFcLcb pair 33 = (fcClx, lcbClx) */
            {
                long rg = 32 + 2 + FIB_CSW * 2 + 2 + FIB_CSLW * 4 + 2;
                long p = rg + 33 * 8;
                unsigned long fcClx = 64, lcbClx = (unsigned long)(tbl.n - 64);
                wd.p[p + 0] = (unsigned char)fcClx;
                wd.p[p + 1] = (unsigned char)(fcClx >> 8);
                wd.p[p + 2] = (unsigned char)(fcClx >> 16);
                wd.p[p + 3] = (unsigned char)(fcClx >> 24);
                wd.p[p + 4] = (unsigned char)lcbClx;
                wd.p[p + 5] = (unsigned char)(lcbClx >> 8);
                wd.p[p + 6] = (unsigned char)(lcbClx >> 16);
                wd.p[p + 7] = (unsigned char)(lcbClx >> 24);
            }
        }

        w = ud_cfbw_new();
        ud_cfbw_stream(w, UD_CFB_ROOT_ID, "WordDocument", wd.p, wd.n);
        ud_cfbw_stream(w, UD_CFB_ROOT_ID, whichtbl ? "1Table" : "0Table",
                       tbl.p, tbl.n);
        img = ud_cfbw_serialize(w, &imglen);
        ud_cfbw_free(w);
        free(wd.p); free(tbl.p);
        if (!img) { printf("FAIL doc selftest: could not build the file\n"); return 1; }

        d = open_doc(img, imglen, &src, &c);
        if (!d) {
            printf("FAIL doc selftest (%sTable): %s\n", whichtbl ? "1" : "0", ud_error());
            bad = 1;
        } else {
            if (ud_doc_pieces(d) != 4) {
                printf("FAIL doc selftest: %d pieces, expected 4\n", ud_doc_pieces(d));
                bad = 1;
            }
            if (ud_doc_text_len(d) != ccp || strcmp(ud_doc_text(d), want) != 0) {
                printf("FAIL doc selftest (%sTable): text came back as\n  \"%s\"\n"
                       "  want \"%s\"\n", whichtbl ? "1" : "0", ud_doc_text(d), want);
                bad = 1;
            }
            ud_doc_close(d);
        }
        ud_cfb_close(c);
        ud_free(img);
    }
    if (!bad)
        printf("doctest: selftest OK - 4 pieces alternating 8-bit/UTF-16, "
               "stored out of document order, reassembled correctly from "
               "both 0Table and 1Table\n");
    return bad;
}

int main(int argc, char **argv)
{
    ud_set_alloc(t_alloc, free);

    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) return doc_selftest();

    if (argc >= 3 && (strcmp(argv[1], "text") == 0 || strcmp(argv[1], "info") == 0)) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        ud_src src;
        ud_cfb *c;
        ud_doc *d;
        if (!b) { printf("ERR: cannot read %s\n", argv[2]); return 2; }
        d = open_doc(b, n, &src, &c);
        if (!d) { printf("ERR: %s\n", ud_error()); free(b); ud_cfb_close(c); return 1; }
        if (strcmp(argv[1], "text") == 0) {
            fputs(ud_doc_plain(d), stdout);
        } else {
            const char *t = ud_doc_text(d);
            long i, ctrl = 0, para = 0, cell = 0, field = 0;
            for (i = 0; i < ud_doc_text_len(d); i++) {
                unsigned char ch = (unsigned char)t[i];
                if (ch == 0x0D) para++;
                else if (ch == 0x07) cell++;
                else if (ch == 0x13) field++;
                else if (ch < 0x20 && ch != '\t') ctrl++;
            }
            printf("pieces=%d chars=%ld paragraphs=%ld cells=%ld fields=%ld "
                   "other-controls=%ld\n",
                   ud_doc_pieces(d), ud_doc_text_len(d), para, cell, field, ctrl);
        }
        ud_doc_close(d);
        ud_cfb_close(c);
        free(b);
        return 0;
    }
    if (argc >= 5 && strcmp(argv[1], "fuzz") == 0) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        unsigned seed = (unsigned)strtoul(argv[3], 0, 10);
        long iters = strtol(argv[4], 0, 10), t, opened = 0;
        if (!b || n < 512) { printf("ERR: cannot read %s\n", argv[2]); free(b); return 2; }
        for (t = 0; t < iters; t++) {
            unsigned char *m = (unsigned char *)malloc((size_t)n);
            ud_src src;
            ud_cfb *c;
            ud_doc *d;
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
            d = open_doc(m, n, &src, &c);
            if (d) { opened++; (void)strlen(ud_doc_plain(d)); ud_doc_close(d); }
            ud_cfb_close(c);
            free(m);
        }
        printf("OK %ld mutations, %ld documents opened\n", iters, opened);
        free(b);
        return 0;
    }
    printf("usage: doctest text FILE | info FILE | fuzz FILE SEED N\n");
    return 2;
}
