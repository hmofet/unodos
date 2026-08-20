/* ===========================================================================
 * ud_docw.c - writing a Word 97 document [MS-DOC], the minimal accepted path.
 *
 * Reading a .doc means coping with every layout Word has ever emitted.
 * WRITING one means picking the single simplest layout that Word and
 * LibreOffice both accept, and emitting it exactly:
 *
 *   - ONE text piece, 8-bit, so the piece table is a formality rather than
 *     the puzzle the reader has to solve;
 *   - one paragraph exception page and one character exception page, so the
 *     bin tables have exactly one entry each;
 *   - a style sheet with a single Normal style, because a document with no
 *     STSH at all is rejected;
 *   - one section, with no section properties of its own.
 *
 * The awkward part is not any single structure, it is that the FIB has to be
 * written twice: once to reserve its space, and again at the end once every
 * other structure's offset and length is known.  Everything here is laid out
 * first and patched afterwards, which is why the fclcb table exists.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_ooxw_int.h"
#include <string.h>

#define W_CSW      14
#define W_CSLW     22
#define W_CBFC     93
#define W_FIBLEN   (32 + 2 + W_CSW * 2 + 2 + W_CSLW * 4 + 2 + W_CBFC * 8)
#define W_RGBASE   (32 + 2 + W_CSW * 2 + 2 + W_CSLW * 4 + 2)
#define W_TEXT_AT  1024          /* fcMin: page-aligned, clear of the FIB   */
#define W_PAGE     512

/* the rgFcLcb pairs a minimal document must fill in */
#define P_STSHF     1
#define P_PLCFSED   6
#define P_BTECHPX  12
#define P_BTEPAPX  13
#define P_CLX      33

typedef struct { unsigned char *p; long n, cap; int bad; } dwbuf;

static int dwneed(dwbuf *b, long n)
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
static void dwput(dwbuf *b, const void *d, long n)
{ if (dwneed(b, n)) { memcpy(b->p + b->n, d, (unsigned long)n); b->n += n; } }
static void dw8 (dwbuf *b, unsigned v)
{ if (dwneed(b, 1)) b->p[b->n++] = (unsigned char)v; }
static void dw16(dwbuf *b, unsigned v)
{ if (dwneed(b, 2)) { ud_wr16(b->p + b->n, (uint16_t)v); b->n += 2; } }
static void dw32(dwbuf *b, unsigned long v)
{ if (dwneed(b, 4)) { ud_wr32(b->p + b->n, (uint32_t)v); b->n += 4; } }
static void dwzero(dwbuf *b, long n)
{ if (dwneed(b, n)) { memset(b->p + b->n, 0, (unsigned long)n); b->n += n; } }
static void dwpad(dwbuf *b, long to)
{ if (b->n < to) dwzero(b, to - b->n); }

/* ---- the model ------------------------------------------------------------- */
typedef struct { char *text; int bold, italic, align; } dwpara;

struct ud_docw {
    dwpara *para; int npara, pcap;
};

ud_docw *ud_docw_new(void)
{
    ud_docw *w = (ud_docw *)ud_alloc(sizeof(ud_docw));
    if (!w) { ud_set_error("out of memory"); return 0; }
    memset(w, 0, sizeof *w);
    return w;
}

void ud_docw_free(ud_docw *w)
{
    int i;
    if (!w) return;
    for (i = 0; i < w->npara; i++) ud_free(w->para[i].text);
    ud_free(w->para);
    ud_free(w);
}

int ud_docw_para(ud_docw *w, const char *text, int bold, int italic, int align)
{
    long n;
    char *c;
    if (!w) { ud_set_error("no writer"); return 0; }
    if (!text) text = "";
    n = (long)strlen(text);
    if (w->npara == w->pcap) {
        int nc = w->pcap ? w->pcap * 2 : 16;
        dwpara *np = (dwpara *)ud_alloc((unsigned long)nc * sizeof(dwpara));
        if (!np) { ud_set_error("out of memory"); return 0; }
        memset(np, 0, (unsigned long)nc * sizeof(dwpara));
        if (w->npara) memcpy(np, w->para, (unsigned long)w->npara * sizeof(dwpara));
        ud_free(w->para);
        w->para = np; w->pcap = nc;
    }
    c = (char *)ud_alloc((unsigned long)n + 1);
    if (!c) { ud_set_error("out of memory"); return 0; }
    memcpy(c, text, (unsigned long)n + 1);
    w->para[w->npara].text   = c;
    w->para[w->npara].bold   = bold ? 1 : 0;
    w->para[w->npara].italic = italic ? 1 : 0;
    w->para[w->npara].align  = align;
    w->npara++;
    return 1;
}

/* ---- the style sheet: one Normal style ------------------------------------- */
static void put_stsh(dwbuf *t)
{
    long lenat, start;

    dw16(t, 18);                       /* cbStshi                           */
    dw16(t, 1);                        /* cstd: just Normal                 */
    dw16(t, 10);                       /* cbSTDBaseInFile                   */
    dwzero(t, 14);                     /* the rest of the STSHI             */

    lenat = t->n;
    dw16(t, 0);                        /* cbStd, patched below              */
    start = t->n;
    dw16(t, 0);                        /* sti 0 = Normal, no flags          */
    dw16(t, 1);                        /* stk = paragraph, istdBase = none  */
    dw16(t, 2);                        /* cupx = 2 (a PAPX and a CHPX)      */
    dw16(t, 0);                        /* bchUpe                            */
    dw16(t, 0);                        /* grfstd                            */
    dw16(t, 0);                        /* Xstz: an empty name...            */
    dw16(t, 0);                        /* ...and its terminator             */
    dw16(t, 2);                        /* UPXPapx: just the istd            */
    dw16(t, 0);
    dw16(t, 4);                        /* UPXChpx: 10pt                     */
    dw16(t, 0x4A43); dw16(t, 20);
    ud_wr16(t->p + lenat, (uint16_t)(t->n - start));
    /* istdBase is stored in the high 12 bits, so "based on nothing" is
       0x0FFF shifted up over the stk nibble */
    ud_wr16(t->p + start + 2, (uint16_t)(1 | (0x0FFF << 4)));
}

/* ---- the exception pages ---------------------------------------------------
 * One FKP each.  The character page carries a CHPX per formatted run and the
 * paragraph page a PAPX per paragraph, both keyed by the file offsets the
 * text actually occupies. */
static void put_chpx_fkp(unsigned char *fkp, ud_docw *w, const long *pstart,
                         long fcMac)
{
    int i, crun = w->npara;
    long blob = W_PAGE - 1;

    memset(fkp, 0, W_PAGE);
    for (i = 0; i < crun; i++) ud_wr32(fkp + i * 4, (uint32_t)pstart[i]);
    ud_wr32(fkp + crun * 4, (uint32_t)fcMac);
    for (i = 0; i < crun; i++) {
        unsigned char g[6];
        long cb = 0;
        if (w->para[i].bold)   { g[cb++] = 0x35; g[cb++] = 0x08; g[cb++] = 1; }
        if (w->para[i].italic) { g[cb++] = 0x36; g[cb++] = 0x08; g[cb++] = 1; }
        if (!cb) { fkp[4 * (crun + 1) + i] = 0; continue; }
        blob -= cb + 1;
        blob &= ~1L;                          /* CHPX offsets are halved    */
        if (blob <= 4 * (crun + 1) + crun) { fkp[4 * (crun + 1) + i] = 0; continue; }
        fkp[blob] = (unsigned char)cb;
        memcpy(fkp + blob + 1, g, (unsigned long)cb);
        fkp[4 * (crun + 1) + i] = (unsigned char)(blob / 2);
    }
    fkp[W_PAGE - 1] = (unsigned char)crun;
}

static void put_papx_fkp(unsigned char *fkp, ud_docw *w, const long *pstart,
                         long fcMac)
{
    int i, crun = w->npara;
    long blob = W_PAGE - 1;

    memset(fkp, 0, W_PAGE);
    for (i = 0; i < crun; i++) ud_wr32(fkp + i * 4, (uint32_t)pstart[i]);
    ud_wr32(fkp + crun * 4, (uint32_t)fcMac);
    for (i = 0; i < crun; i++) {
        unsigned char g[8];
        long cb = 0, need, cw;
        g[cb++] = 0; g[cb++] = 0;             /* istd 0 = Normal            */
        if (w->para[i].align) {
            g[cb++] = 0x03; g[cb++] = 0x24; g[cb++] = (unsigned char)w->para[i].align;
        }
        /* PapxInFkp: a word count whose blob is 2*cw-1 bytes, so the grpprl
           is padded to an odd length */
        cw = (cb + 1 + 1) / 2;
        need = cw * 2 - 1;
        blob -= need + 1;
        blob &= ~1L;
        if (blob <= 4 * (crun + 1) + crun * 13) { fkp[4 * (crun + 1) + i * 13] = 0; continue; }
        fkp[blob] = (unsigned char)cw;
        memset(fkp + blob + 1, 0, (unsigned long)need);
        memcpy(fkp + blob + 1, g, (unsigned long)cb);
        fkp[4 * (crun + 1) + i * 13] = (unsigned char)(blob / 2);
    }
    fkp[W_PAGE - 1] = (unsigned char)crun;
}

/* ---- serialise -------------------------------------------------------------- */
unsigned char *ud_docw_save(ud_docw *w, long *len)
{
    dwbuf wd, tb;
    long fclcb[W_CBFC][2];       /* one slot per rgFcLcb pair, not fewer */
    long *pstart = 0;
    long ccp = 0, fcMin = W_TEXT_AT, fcMac, chpx_pg, papx_pg, i;
    unsigned char *img = 0;
    ud_cfbw *c;

    if (len) *len = 0;
    if (!w) { ud_set_error("no writer"); return 0; }
    if (!w->npara) { ud_set_error("doc write: a document needs a paragraph"); return 0; }
    memset(&wd, 0, sizeof wd);
    memset(&tb, 0, sizeof tb);
    memset(fclcb, 0, sizeof fclcb);
    pstart = (long *)ud_alloc((unsigned long)w->npara * sizeof(long));
    if (!pstart) { ud_set_error("out of memory"); return 0; }

    /* ---- the text: one 8-bit piece, every paragraph closed with 0x0D ---- */
    dwzero(&wd, W_TEXT_AT);
    for (i = 0; i < w->npara; i++) {
        long n = (long)strlen(w->para[i].text), k;
        pstart[i] = wd.n;
        for (k = 0; k < n; k++) {
            unsigned char ch = (unsigned char)w->para[i].text[k];
            /* the control range is Word's own; keep the text out of it */
            dw8(&wd, (ch < 0x20 && ch != '\t') ? ' ' : ch);
        }
        dw8(&wd, 0x0D);
        ccp += n + 1;
    }
    fcMac = wd.n;

    /* ---- the two exception pages, each on a 512-byte boundary ----------- */
    {
        unsigned char fkp[W_PAGE];
        dwpad(&wd, (wd.n + W_PAGE - 1) / W_PAGE * W_PAGE);
        chpx_pg = wd.n / W_PAGE;
        put_chpx_fkp(fkp, w, pstart, fcMac);
        dwput(&wd, fkp, W_PAGE);
        papx_pg = wd.n / W_PAGE;
        put_papx_fkp(fkp, w, pstart, fcMac);
        dwput(&wd, fkp, W_PAGE);
    }

    /* ---- the table stream ------------------------------------------------ */
    fclcb[P_STSHF][0] = tb.n;
    put_stsh(&tb);
    fclcb[P_STSHF][1] = tb.n - fclcb[P_STSHF][0];

    fclcb[P_CLX][0] = tb.n;                   /* the piece table            */
    dw8(&tb, 0x02);
    dw32(&tb, 4 * 2 + 8);                     /* two CPs, one Pcd           */
    dw32(&tb, 0);
    dw32(&tb, (unsigned long)ccp);
    dw16(&tb, 0);
    dw32(&tb, ((unsigned long)fcMin * 2) | 0x40000000UL);   /* 8-bit        */
    dw16(&tb, 0);
    fclcb[P_CLX][1] = tb.n - fclcb[P_CLX][0];

    fclcb[P_PLCFSED][0] = tb.n;               /* one section, no properties */
    dw32(&tb, 0);
    dw32(&tb, (unsigned long)ccp);
    dw16(&tb, 0); dw16(&tb, 0);
    dw32(&tb, 0xFFFFFFFFUL);
    dw16(&tb, 0); dw16(&tb, 0);
    dw32(&tb, 0xFFFFFFFFUL);
    fclcb[P_PLCFSED][1] = tb.n - fclcb[P_PLCFSED][0];

    fclcb[P_BTECHPX][0] = tb.n;
    dw32(&tb, (unsigned long)fcMin);
    dw32(&tb, (unsigned long)fcMac);
    dw32(&tb, (unsigned long)chpx_pg);
    fclcb[P_BTECHPX][1] = tb.n - fclcb[P_BTECHPX][0];

    fclcb[P_BTEPAPX][0] = tb.n;
    dw32(&tb, (unsigned long)fcMin);
    dw32(&tb, (unsigned long)fcMac);
    dw32(&tb, (unsigned long)papx_pg);
    fclcb[P_BTEPAPX][1] = tb.n - fclcb[P_BTEPAPX][0];

    /* ---- now the FIB, whose every offset is finally known ---------------- */
    if (!wd.bad && !tb.bad) {
        unsigned char *f = wd.p;
        memset(f, 0, W_FIBLEN);
        ud_wr16(f + 0x00, 0xA5EC);            /* wIdent                     */
        ud_wr16(f + 0x02, 0x00C1);            /* nFib: Word 97              */
        ud_wr16(f + 0x0A, 0x0200);            /* fWhichTblStm = 1Table      */
        ud_wr16(f + 0x0C, 0x00BF);            /* nFibBack                   */
        ud_wr32(f + 0x18, (uint32_t)fcMin);
        ud_wr32(f + 0x1C, (uint32_t)fcMac);
        ud_wr16(f + 0x20, W_CSW);
        ud_wr16(f + 0x20 + 2 + W_CSW * 2, W_CSLW);
        {
            long rglw = 0x20 + 2 + W_CSW * 2 + 2;
            ud_wr32(f + rglw + 0 * 4, (uint32_t)wd.n);      /* cbMac        */
            ud_wr32(f + rglw + 3 * 4, (uint32_t)ccp);       /* ccpText      */
        }
        ud_wr16(f + W_RGBASE - 2, W_CBFC);
        for (i = 0; i < W_CBFC; i++) {
            ud_wr32(f + W_RGBASE + i * 8,     (uint32_t)fclcb[i][0]);
            ud_wr32(f + W_RGBASE + i * 8 + 4, (uint32_t)fclcb[i][1]);
        }
    }

    if (wd.bad || tb.bad) { ud_set_error("doc write: out of memory"); goto done; }

    c = ud_cfbw_new();
    if (c) {
        if (ud_cfbw_stream(c, UD_CFB_ROOT_ID, "WordDocument", wd.p, wd.n) != UD_CFB_NONE &&
            ud_cfbw_stream(c, UD_CFB_ROOT_ID, "1Table", tb.p, tb.n) != UD_CFB_NONE)
            img = ud_cfbw_serialize(c, len);
        ud_cfbw_free(c);
    }
done:
    ud_free(pstart);
    ud_free(wd.p);
    ud_free(tb.p);
    return img;
}

/* ---- the read-back seam (ud_ooxw_int.h) ------------------------------------
 * ud_ooxw.c serialises this same model as .docx. */
int ud_docw_nparas(const ud_docw *w) { return w ? w->npara : 0; }

const char *ud_docw_para_at(const ud_docw *w, int i,
                            int *bold, int *italic, int *align)
{
    if (!w || i < 0 || i >= w->npara) return 0;
    if (bold)   *bold   = w->para[i].bold;
    if (italic) *italic = w->para[i].italic;
    if (align)  *align  = w->para[i].align;
    return w->para[i].text ? w->para[i].text : "";
}
