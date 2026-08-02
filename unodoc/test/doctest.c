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

#define FKP_PAGE  512

static void wr32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* Append one STD to a style sheet under construction: the fixed header, an
 * empty name, then the property blobs (a paragraph style carries PAPX then
 * CHPX, a character style just CHPX). */
static void put_style(dbuf *b, int stk, int base,
                      const unsigned char *papx, long npapx,
                      const unsigned char *chpx, long nchpx)
{
    long lenat, start;
    int cupx = (stk == 1) ? 2 : 1;

    lenat = b->n;
    d16(b, 0);                                  /* cbStd, patched below     */
    start = b->n;
    d16(b, 0);                                  /* sti and its flags        */
    d16(b, (unsigned)(stk | (base << 4)));
    d16(b, (unsigned)cupx);                     /* cupx | istdNext << 4     */
    d16(b, 0);                                  /* bchUpe                   */
    d16(b, 0);                                  /* grfstd                   */
    d16(b, 0);                                  /* Xstz: an empty name...   */
    d16(b, 0);                                  /* ...and its terminator    */
    if (stk == 1) {
        d16(b, (unsigned)(npapx + 2));          /* UPXPapx leads with istd  */
        d16(b, 0);
        dput(b, papx, npapx);
        if ((b->n - start) & 1) dzero(b, 1);    /* ONE byte of padding      */
    }
    d16(b, (unsigned)nchpx);
    dput(b, chpx, nchpx);
    if ((b->n - start) & 1) dzero(b, 1);
    b->p[lenat] = (unsigned char)(b->n - start);
    b->p[lenat + 1] = (unsigned char)((b->n - start) >> 8);
}

/* write one (fc, lcb) pair of rgFcLcb, given the array's base offset */
static void patch_pair(unsigned char *fib, long rgbase, int pair,
                       long fc, long lcb)
{
    wr32(fib + rgbase + pair * 8, (unsigned long)fc);
    wr32(fib + rgbase + pair * 8 + 4, (unsigned long)lcb);
}

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
    long bte_lo = 0, bte_hi = 0, bte_chpx = 0, bte_papx = 0;
    long stsh_at = 0, stsh_len = 0;

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

        /* ---- formatting: one CHPX page and one PAPX page ----------------
         * The CHPX run deliberately covers only the bytes of piece 2, which
         * is the piece placed FIRST in the stream but appearing THIRD in the
         * document.  So if the reader looks formatting up by character
         * position instead of by file offset, the wrong text comes back
         * bold - which is the whole point of doing it this way. */
        {
            long chpx_page = 8, papx_page = 9;
            unsigned char fkp[FKP_PAGE];
            long lo = at[PCORDER[0]];           /* first run placed         */
            long mid = at[PCORDER[1]];
            long hi = wd.n;

            /* CHPX FKP: run 0 = [lo,mid) bold+italic+20pt, run 1 = default */
            memset(fkp, 0, sizeof fkp);
            wr32(fkp + 0, (unsigned long)lo);
            wr32(fkp + 4, (unsigned long)mid);
            wr32(fkp + 8, (unsigned long)hi);
            fkp[12] = 250;                      /* run 0's CHPX at 500      */
            fkp[13] = 0;                        /* run 1: no exception      */
            fkp[500] = 7;                       /* cb                       */
            fkp[501] = 0x30; fkp[502] = 0x4A;                 /* sprmCIstd  */
            fkp[503] = 2;    fkp[504] = 0;                    /* char style 2*/
            fkp[505] = 0x3E; fkp[506] = 0x2A; fkp[507] = 1;   /* underline  */
            fkp[FKP_PAGE - 1] = 2;              /* crun                     */
            dzero(&wd, chpx_page * FKP_PAGE - wd.n);
            dput(&wd, fkp, FKP_PAGE);

            /* PAPX FKP: one run over everything, centred with a left indent */
            memset(fkp, 0, sizeof fkp);
            wr32(fkp + 0, (unsigned long)lo);
            wr32(fkp + 4, (unsigned long)hi);
            fkp[8] = 240;                       /* PAPX at 480              */
            fkp[480] = 5;                       /* cb: grpprl is 2*5-1 = 9  */
            fkp[481] = 1; fkp[482] = 0;         /* istd: paragraph style 1  */
            fkp[483] = 0x03; fkp[484] = 0x24; fkp[485] = 1;   /* jc centre  */
            fkp[486] = 0x0F; fkp[487] = 0x84;                 /* dxaLeft    */
            fkp[488] = 0xD0; fkp[489] = 0x02;                 /* 720 twips  */
            fkp[FKP_PAGE - 1] = 1;
            dzero(&wd, papx_page * FKP_PAGE - wd.n);
            dput(&wd, fkp, FKP_PAGE);
            bte_lo = lo; bte_hi = hi;
            bte_chpx = chpx_page; bte_papx = papx_page;
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
            {
                long rg = 32 + 2 + FIB_CSW * 2 + 2 + FIB_CSLW * 4 + 2;
                long clxlen = tbl.n - 64;
                long btechpx, btepapx;

                /* the two formatting bin tables, right after the CLX */
                btechpx = tbl.n;
                d32(&tbl, (unsigned long)bte_lo);
                d32(&tbl, (unsigned long)bte_hi);
                d32(&tbl, (unsigned long)bte_chpx);
                btepapx = tbl.n;
                d32(&tbl, (unsigned long)bte_lo);
                d32(&tbl, (unsigned long)bte_hi);
                d32(&tbl, (unsigned long)bte_papx);

                /* A style sheet with a based-on chain and a character style.
                 * Three styles, so the layering has something to get wrong:
                 *   0  paragraph, based on nothing: 10pt, left aligned
                 *   1  paragraph, based on 0:       bold, centred
                 *   2  character:                   italic
                 * The paragraph names style 1; the run names style 2 through
                 * sprmCIstd and adds underline directly.  Correct resolution
                 * is size from 0, bold and centre from 1, italic from 2, and
                 * underline plus the indent direct - four layers, and any two
                 * of them applied in the wrong order loses a property. */
                {
                    static const unsigned char S0P[] = { 0x03, 0x24, 0 };
                    static const unsigned char S0C[] = { 0x43, 0x4A, 20, 0 };
                    static const unsigned char S1P[] = { 0x03, 0x24, 1 };
                    static const unsigned char S1C[] = { 0x35, 0x08, 1 };
                    static const unsigned char S2C[] = { 0x36, 0x08, 1 };
                    stsh_at = tbl.n;
                    d16(&tbl, 18);                       /* cbStshi         */
                    d16(&tbl, 3);                        /* cstd            */
                    d16(&tbl, 10);                       /* cbSTDBaseInFile */
                    dzero(&tbl, 14);                     /* the rest of it   */
                    put_style(&tbl, 1, 0x0FFF, S0P, sizeof S0P, S0C, sizeof S0C);
                    put_style(&tbl, 1, 0,      S1P, sizeof S1P, S1C, sizeof S1C);
                    put_style(&tbl, 2, 0x0FFF, 0, 0,          S2C, sizeof S2C);
                    stsh_len = tbl.n - stsh_at;
                }

                patch_pair(wd.p, rg, 33, 64, clxlen);            /* fcClx   */
                patch_pair(wd.p, rg, 12, btechpx, 12);           /* CHPX bte*/
                patch_pair(wd.p, rg, 13, btepapx, 12);           /* PAPX bte*/
                patch_pair(wd.p, rg,  1, stsh_at, stsh_len);     /* STSH    */
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
            /* Formatting is indexed by FILE offset, and the CHPX run covers
               only piece 2's bytes - the piece stored FIRST but reading
               THIRD.  So the bold text is the one late in the document, and
               a reader that indexed by character position would bold the
               wrong words. */
            {
                long cp_in_p2 = (long)(strlen(PC[0]) + strlen(PC[1]) + 2);
                ud_chp ch;
                ud_pap pa;

                /* All four layers at once: size from the base style, bold
                   from the style based on it, italic from the CHARACTER
                   style the run names, underline from the run itself. */
                ud_doc_chp_at(d, cp_in_p2, &ch);
                if (ch.size != 20 || !ch.bold || !ch.italic || !ch.underline) {
                    printf("FAIL doc selftest: the four style layers did not "
                           "all resolve - size=%d (want 20, from the base "
                           "style) bold=%d (want 1, from the derived style) "
                           "italic=%d (want 1, from the character style) "
                           "underline=%d (want 1, direct)\n",
                           ch.size, ch.bold, ch.italic, ch.underline);
                    bad = 1;
                }
                /* Piece 0 has no direct CHPX, so it keeps the paragraph
                   style's bold and size but NOT the character style's italic
                   or the direct underline - which is also how we know the
                   lookup followed file offset rather than character
                   position, since piece 0 reads FIRST but is stored second. */
                ud_doc_chp_at(d, 0, &ch);
                if (ch.size != 20 || !ch.bold || ch.italic || ch.underline) {
                    printf("FAIL doc selftest: piece 0 should inherit the "
                           "paragraph style only, got size=%d bold=%d "
                           "italic=%d underline=%d\n",
                           ch.size, ch.bold, ch.italic, ch.underline);
                    bad = 1;
                }
                ud_doc_pap_at(d, 0, &pa);
                if (pa.align != 1 || pa.left != 720) {
                    printf("FAIL doc selftest: paragraph should be centred with "
                           "a 720-twip indent, got align=%d left=%d\n",
                           pa.align, pa.left);
                    bad = 1;
                }
            }
            ud_doc_close(d);
        }
        ud_cfb_close(c);
        ud_free(img);
    }
    if (!bad)
        printf("doctest: selftest OK - 4 pieces alternating 8-bit/UTF-16, "
               "stored out of document order, reassembled from both 0Table "
               "and 1Table; CHPX/PAPX resolved by FILE offset; and four style "
               "layers (base style, derived style, character style, direct) "
               "resolving in the right order\n");
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
    /* For every ALLCAPS marker word in the document, report the formatting
     * in force at its first character.  The gate compares this against the
     * expectations mkcorpus.py wrote from the SOURCE styles - so the marker
     * text, not an offset we computed, is what ties the two together. */
    if (argc >= 3 && strcmp(argv[1], "fmt") == 0) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        ud_src src;
        ud_cfb *c;
        ud_doc *d;
        const char *t;
        long i, len;
        if (!b) { printf("ERR: cannot read %s\n", argv[2]); return 2; }
        d = open_doc(b, n, &src, &c);
        if (!d) { printf("ERR: %s\n", ud_error()); free(b); ud_cfb_close(c); return 1; }
        t = ud_doc_text(d);
        len = ud_doc_text_len(d);
        for (i = 0; i < len; i++) {
            long j = i;
            char word[64];
            int k = 0;
            while (j < len && t[j] >= 'A' && t[j] <= 'Z' && k < 63) word[k++] = t[j++];
            if (k < 5) { continue; }
            word[k] = 0;
            {
                ud_chp ch;
                ud_pap pa;
                ud_doc_chp_at(d, i, &ch);
                ud_doc_pap_at(d, i, &pa);
                printf("chp\t%s\tbold\t%d\n",      word, ch.bold);
                printf("chp\t%s\titalic\t%d\n",    word, ch.italic);
                printf("chp\t%s\tunderline\t%d\n", word, ch.underline);
                printf("chp\t%s\tstrike\t%d\n",    word, ch.strike);
                printf("chp\t%s\tcaps\t%d\n",      word, ch.caps);
                printf("chp\t%s\tsize\t%d\n",      word, ch.size);
                printf("pap\t%s\talign\t%d\n",     word, pa.align);
                printf("pap\t%s\tleft\t%d\n",      word, pa.left);
                printf("pap\t%s\tfirst\t%d\n",     word, pa.first);
                printf("pap\t%s\tbefore\t%d\n",    word, pa.before);
            }
            i = j;
        }
        ud_doc_close(d);
        ud_cfb_close(c);
        free(b);
        return 0;
    }
    /* write a document, read it back with our own reader, and (via
     * run_tests.py) hand it to LibreOffice - the half we cannot judge */
    if (argc >= 2 && strcmp(argv[1], "wtest") == 0) {
        static const char *P[4] = {
            "Plain first paragraph.",
            "This one is bold.",
            "This one is italic and centred.",
            "Back to plain, right aligned."
        };
        ud_docw *w = ud_docw_new();
        unsigned char *img;
        long n = 0;
        ud_src src;
        ud_cfb *c;
        ud_doc *d;
        int bad = 0, i;

        if (!w) { printf("ERR: %s\n", ud_error()); return 1; }
        ud_docw_para(w, P[0], 0, 0, 0);
        ud_docw_para(w, P[1], 1, 0, 0);
        ud_docw_para(w, P[2], 0, 1, 1);
        ud_docw_para(w, P[3], 0, 0, 2);
        img = ud_docw_save(w, &n);
        ud_docw_free(w);
        if (!img) { printf("FAIL doc write: %s\n", ud_error()); return 1; }
        if (argc >= 3) {                       /* also drop it on disk      */
            FILE *f = fopen(argv[2], "wb");
            if (f) { fwrite(img, 1, (size_t)n, f); fclose(f); }
        }
        d = open_doc(img, n, &src, &c);
        if (!d) {
            printf("FAIL doc write: our own reader will not open it: %s\n", ud_error());
            bad = 1;
        } else {
            const char *t = ud_doc_text(d);
            long at = 0;
            for (i = 0; i < 4; i++) {
                long ln = (long)strlen(P[i]);
                ud_chp ch;
                ud_pap pa;
                if (strncmp(t + at, P[i], (size_t)ln) != 0 || t[at + ln] != 0x0D) {
                    printf("FAIL doc write: paragraph %d came back wrong\n", i);
                    bad = 1;
                }
                ud_doc_chp_at(d, at, &ch);
                ud_doc_pap_at(d, at, &pa);
                if (ch.bold != (i == 1) || ch.italic != (i == 2)) {
                    printf("FAIL doc write: paragraph %d formatting: bold=%d "
                           "italic=%d\n", i, ch.bold, ch.italic);
                    bad = 1;
                }
                if (pa.align != (i == 2 ? 1 : (i == 3 ? 2 : 0))) {
                    printf("FAIL doc write: paragraph %d align=%d\n", i, pa.align);
                    bad = 1;
                }
                if (ch.size != 20) {
                    printf("FAIL doc write: paragraph %d size=%d, the Normal "
                           "style should have supplied 20\n", i, ch.size);
                    bad = 1;
                }
                at += ln + 1;
            }
            ud_doc_close(d);
        }
        ud_cfb_close(c);
        ud_free(img);
        if (!bad)
            printf("doctest: writer OK - 4 paragraphs with bold, italic and "
                   "alignment survive a save/reload, and 10pt arrives through "
                   "the Normal style we wrote\n");
        return bad;
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
