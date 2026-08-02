/* ===========================================================================
 * ud_doc.c - the Word 97 binary document [MS-DOC], read side, phase 4a:
 * the FIB, the piece table, and the text.
 *
 * A .doc does not store its text in one place or one encoding.  The
 * WordDocument stream holds runs of characters wherever a quick-save happened
 * to leave them, and a PIECE TABLE in one of the two table streams says which
 * run supplies which part of the document.  Three things follow, and all
 * three are where naive readers go wrong:
 *
 *   - DOCUMENT ORDER IS NOT FILE ORDER.  The pieces must be walked in the
 *     order the table gives, not the order they appear in the stream.
 *   - EACH PIECE CHOOSES ITS OWN ENCODING.  Bit 30 of a piece's offset means
 *     "this run is 8-bit", and the real offset is then the remaining bits
 *     halved.  One document mixes 8-bit and UTF-16 runs freely - the same
 *     shape as BIFF8's shared strings, which is why ud_xls.c met it first.
 *   - WHICH TABLE STREAM holds the piece table is a single bit in the FIB
 *     (fWhichTblStm), naming "0Table" or "1Table".  Both may exist.
 *
 * Phase 4a stops at text.  Character and paragraph formatting (CHPX/PAPX
 * through the bin tables, sprms over the style hierarchy) is 4b; writing is
 * 4c.  Splitting it here means the piece table gets its own gate, which is
 * the part everything else depends on.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include <string.h>

#define FIB_IDENT      0xA5EC
#define NFIB_WORD97    0x00C1

/* FIB fixed-part offsets */
#define F_IDENT        0x00
#define F_NFIB         0x02
#define F_FLAGS        0x0A
#define F_FCMIN        0x18
#define F_CSW          0x20

#define FL_COMPLEX     0x0004    /* fComplex: a real piece table          */
#define FL_ENCRYPTED   0x0100
#define FL_WHICHTBL    0x0200
#define FL_CRYPTO      0x8000

#define FCLB_STSHF      1        /* pairs of rgFcLcb97 we use              */
#define FCLB_BTECHPX   12
#define FCLB_BTEPAPX   13
#define FCLB_CLX       33

#define ISTD_NONE      0x0FFF    /* "this style is based on nothing"       */
#define STK_PARA       1
#define STK_CHAR       2
#define MAX_STYLES     32768
#define STYLE_DEPTH    16        /* based-on chains are shallow; cycles are
                                    not, so the walk is capped              */

#define FKP_SIZE       512

#define MAX_TEXT       (64L * 1024 * 1024)
#define MAX_PIECES     1000000

typedef struct {
    long cp0, cp1;         /* document character range this piece supplies */
    long fc;               /* byte offset in WordDocument                  */
    int  wide;             /* 0 = 8-bit CP-1252, 1 = UTF-16LE             */
} ud_piece;

/* a bin table: which 512-byte FKP page covers which stretch of the file */
typedef struct {
    uint32_t *fc;      /* n+1 file offsets                                 */
    uint32_t *pn;      /* n page numbers                                   */
    long      n;
} ud_bte;

/* One entry of the style sheet.  The grpprl pointers refer into the STSH
 * blob, which the document keeps for its lifetime rather than copying each
 * style out. */
typedef struct {
    int   stk;                   /* STK_PARA / STK_CHAR                    */
    int   base;                  /* istdBase, ISTD_NONE = based on nothing */
    const unsigned char *papx; long npapx;
    const unsigned char *chpx; long nchpx;
} ud_style;

struct ud_doc {
    ud_cfb   *cfb;
    int       wd;                    /* WordDocument stream id            */
    int       tbl;                   /* the table stream id               */
    long      ccp;                   /* characters of body text           */
    ud_piece *pc;  int npc;
    char     *text;                  /* the body, CP-1252, NUL-terminated */
    char     *plain;                 /* reading text (built on demand)    */
    ud_bte    chpbte, papbte;
    unsigned char *stsh;         /* the style sheet, kept for its grpprls  */
    ud_style *style; int nstyle;
};

/* ---- the FIB ---------------------------------------------------------------
 * Everything after offset 0x20 is variable length: three counted arrays, one
 * after another.  Walk them rather than assuming offsets, because getting
 * this wrong silently reads the wrong table. */
/* fclcb[i] receives the (fc, lcb) pair numbered WANTED[i] of rgFcLcb */
static const int WANTED[4] = { FCLB_CLX, FCLB_BTECHPX, FCLB_BTEPAPX,
                               FCLB_STSHF };

static int fib_parse(ud_doc *d, const unsigned char *f, long n,
                     int *whichtbl, long fclcb[4][2])
{
    long csw, cslw, cbfc, off;

    if (n < 0x60) { ud_set_error("not a Word document (truncated FIB)"); return 0; }
    if (ud_rd16(f + F_IDENT) != FIB_IDENT) {
        ud_set_error("not a Word document (bad FIB signature)");
        return 0;
    }
    {
        unsigned flags = ud_rd16(f + F_FLAGS);
        if (flags & (FL_ENCRYPTED | FL_CRYPTO)) {
            ud_set_error("this document is password-protected - not opened");
            return 0;
        }
        *whichtbl = (flags & FL_WHICHTBL) ? 1 : 0;
    }
    if (ud_rd16(f + F_NFIB) < NFIB_WORD97) {
        ud_set_error("Word 6/95 document - not decoded in this build");
        return 0;
    }

    csw = (long)ud_rd16(f + F_CSW);
    off = F_CSW + 2 + csw * 2;
    if (off + 2 > n) { ud_set_error("Word: FIB rgsw runs off the end"); return 0; }
    cslw = (long)ud_rd16(f + off);
    off += 2;
    /* rglw[3] is ccpText, the length of the body in characters */
    if (cslw < 4 || off + cslw * 4 > n) {
        ud_set_error("Word: FIB rglw runs off the end");
        return 0;
    }
    d->ccp = (long)(int32_t)ud_rd32(f + off + 3 * 4);
    off += cslw * 4;
    if (off + 2 > n) { ud_set_error("Word: FIB rgFcLcb missing"); return 0; }
    cbfc = (long)ud_rd16(f + off);
    off += 2;
    if (cbfc <= FCLB_CLX || off + cbfc * 8 > n) {
        ud_set_error("Word: FIB does not reach the piece table pointer");
        return 0;
    }
    {
        int i;
        for (i = 0; i < 4; i++) {
            fclcb[i][0] = (long)ud_rd32(f + off + WANTED[i] * 8);
            fclcb[i][1] = (long)ud_rd32(f + off + WANTED[i] * 8 + 4);
        }
    }
    if (d->ccp < 0 || d->ccp > MAX_TEXT) {
        ud_set_error("Word: implausible document length");
        return 0;
    }
    return 1;
}

/* ---- the piece table --------------------------------------------------------
 * The CLX is a run of Prc blocks (0x01, a length, a property blob we skip)
 * followed by the Pcdt (0x02, a length, then the PlcPcd itself). */
static int clx_pieces(ud_doc *d, const unsigned char *clx, long n)
{
    long at = 0, lcb = 0, npc, i;
    const unsigned char *plc;

    while (at < n && clx[at] == 0x01) {          /* skip the Prc blocks     */
        long cb;
        if (at + 3 > n) break;
        cb = (long)ud_rd16(clx + at + 1);
        if (cb < 0 || at + 3 + cb > n) break;
        at += 3 + cb;
    }
    if (at >= n || clx[at] != 0x02) {
        ud_set_error("Word: no piece table in the CLX");
        return 0;
    }
    if (at + 5 > n) { ud_set_error("Word: truncated piece table"); return 0; }
    lcb = (long)ud_rd32(clx + at + 1);
    at += 5;
    if (lcb < 4 || at + lcb > n) { ud_set_error("Word: piece table overruns"); return 0; }
    plc = clx + at;

    /* a PLC of n entries is (n+1) CPs then n 8-byte descriptors */
    npc = (lcb - 4) / 12;
    if (npc <= 0 || npc > MAX_PIECES) {
        ud_set_error("Word: implausible piece count");
        return 0;
    }
    d->pc = (ud_piece *)ud_alloc((unsigned long)npc * sizeof(ud_piece));
    if (!d->pc) { ud_set_error("out of memory (pieces)"); return 0; }

    for (i = 0; i < npc; i++) {
        long cp0 = (long)ud_rd32(plc + i * 4);
        long cp1 = (long)ud_rd32(plc + (i + 1) * 4);
        const unsigned char *pcd = plc + (npc + 1) * 4 + i * 8;
        uint32_t fc = ud_rd32(pcd + 2);
        ud_piece *p = &d->pc[d->npc];

        if (cp1 < cp0 || cp0 < 0) continue;      /* damaged: drop the piece */
        p->cp0 = cp0;
        p->cp1 = cp1;
        /* Bit 30 says the run is 8-bit, and then the real offset is the
           remaining bits HALVED - Word stores it doubled so that one field
           can address either encoding. */
        if (fc & 0x40000000u) { p->wide = 0; p->fc = (long)(fc & 0x3FFFFFFFu) / 2; }
        else                  { p->wide = 1; p->fc = (long)(fc & 0x3FFFFFFFu); }
        d->npc++;
    }
    return d->npc > 0;
}

/* ---- the text ---------------------------------------------------------------
 * Assembled in DOCUMENT order, which is the order of the piece table, not the
 * order the runs happen to sit in the stream. */
static int build_text(ud_doc *d)
{
    long i;

    d->text = (char *)ud_alloc((unsigned long)d->ccp + 1);
    if (!d->text) { ud_set_error("out of memory (text)"); return 0; }
    memset(d->text, ' ', (unsigned long)d->ccp);
    d->text[d->ccp] = 0;

    for (i = 0; i < d->npc; i++) {
        ud_piece *p = &d->pc[i];
        long want = p->cp1 - p->cp0, k;
        unsigned char buf[512];

        if (p->cp0 >= d->ccp) continue;
        if (p->cp0 + want > d->ccp) want = d->ccp - p->cp0;
        for (k = 0; k < want; ) {
            long chunk = want - k;
            long got, j;
            if (chunk > (long)(sizeof buf) / (p->wide ? 2 : 1))
                chunk = (long)(sizeof buf) / (p->wide ? 2 : 1);
            got = ud_cfb_read(d->cfb, d->wd,
                              p->fc + k * (p->wide ? 2 : 1),
                              buf, chunk * (p->wide ? 2 : 1));
            if (got <= 0) break;
            got /= (p->wide ? 2 : 1);
            for (j = 0; j < got; j++) {
                uint16_t u = p->wide ? ud_rd16(buf + j * 2)
                                     : ud_cp1252_to_uc(buf[j]);
                d->text[p->cp0 + k + j] = (char)ud_uc_to_cp1252(u);
            }
            k += got;
            if (got < chunk) break;
        }
    }
    return 1;
}

/* ===========================================================================
 * formatting: bin table -> FKP page -> CHPX/PAPX -> sprms
 * ======================================================================== */
static int bte_load(ud_doc *d, ud_bte *b, long fc, long lcb)
{
    unsigned char *raw;
    long n, i;

    if (lcb < 4 || lcb > 4L * 1024 * 1024) return 0;
    n = (lcb - 4) / 8;                     /* (n+1) FCs then n page numbers */
    if (n <= 0) return 0;
    raw = (unsigned char *)ud_alloc((unsigned long)lcb);
    if (!raw) return 0;
    if (ud_cfb_read(d->cfb, d->tbl, fc, raw, lcb) != lcb) { ud_free(raw); return 0; }
    b->fc = (uint32_t *)ud_alloc((unsigned long)(n + 1) * 4);
    b->pn = (uint32_t *)ud_alloc((unsigned long)n * 4);
    if (!b->fc || !b->pn) { ud_free(raw); return 0; }
    for (i = 0; i <= n; i++) b->fc[i] = ud_rd32(raw + i * 4);
    for (i = 0; i < n; i++)
        b->pn[i] = ud_rd32(raw + (n + 1) * 4 + i * 4) & 0x003FFFFFu;
    b->n = n;
    ud_free(raw);
    return 1;
}

/* Which FKP page covers file offset `fc`?  The table is sorted, so this is a
 * binary search - and the bounds check is what stops a damaged table sending
 * us to page 4 billion. */
static long bte_page(const ud_bte *b, long fc)
{
    long lo = 0, hi = b->n - 1;
    if (!b->n) return -1;
    if ((uint32_t)fc < b->fc[0] || (uint32_t)fc >= b->fc[b->n]) return -1;
    while (lo <= hi) {
        long mid = lo + (hi - lo) / 2;
        if ((uint32_t)fc < b->fc[mid]) hi = mid - 1;
        else if ((uint32_t)fc >= b->fc[mid + 1]) lo = mid + 1;
        else return (long)b->pn[mid];
    }
    return -1;
}

/* ---- the sprm walker --------------------------------------------------------
 * A sprm is a u16 opcode whose top three bits say how big its operand is.
 * Getting that table wrong desynchronises the whole run, so it is the one
 * piece of this that has to be exactly right. */
static long sprm_operand_len(uint16_t sprm, const unsigned char *p, long avail)
{
    switch ((sprm >> 13) & 7) {
    case 0: case 1: return 1;
    case 2: case 4: case 5: return 2;
    case 3: return 4;
    case 7: return 3;
    case 6:                              /* variable: a leading length byte */
        if (avail < 1) return -1;
        return 1 + (long)p[0];
    default: return -1;
    }
}

/* Word's toggle operands: 0 off, 1 on, 128 leave alone, 129 invert. */
static int toggle(int cur, int v)
{
    if (v == 128) return cur;
    if (v == 129) return !cur;
    return v ? 1 : 0;
}

static void apply_chp(ud_chp *c, const unsigned char *g, long n)
{
    long at = 0;
    while (at + 2 <= n) {
        uint16_t sprm = ud_rd16(g + at);
        long olen = sprm_operand_len(sprm, g + at + 2, n - at - 2);
        const unsigned char *o = g + at + 2;
        if (olen < 0 || at + 2 + olen > n) return;
        switch (sprm) {
        case 0x0835: c->bold      = toggle(c->bold, o[0]); break;
        case 0x0836: c->italic    = toggle(c->italic, o[0]); break;
        case 0x0837: c->strike    = toggle(c->strike, o[0]); break;
        case 0x083A: c->smallcaps = toggle(c->smallcaps, o[0]); break;
        case 0x083B: c->caps      = toggle(c->caps, o[0]); break;
        case 0x2A3E: c->underline = o[0] != 0; break;
        case 0x2A42: c->color     = o[0]; break;
        case 0x4A43: c->size      = (int)ud_rd16(o); break;
        case 0x4A4F: c->font      = (int)ud_rd16(o); break;
        case 0x2A44: c->super = (o[0] == 1); c->sub = (o[0] == 2); break;
        default: break;
        }
        at += 2 + olen;
    }
}

static void apply_pap(ud_pap *p, const unsigned char *g, long n)
{
    long at = 0;
    while (at + 2 <= n) {
        uint16_t sprm = ud_rd16(g + at);
        long olen = sprm_operand_len(sprm, g + at + 2, n - at - 2);
        const unsigned char *o = g + at + 2;
        if (olen < 0 || at + 2 + olen > n) return;
        switch (sprm) {
        case 0x2403: p->align  = o[0]; break;
        case 0x2406: p->keep_next = o[0] != 0; break;
        case 0x2407: p->page_break_before = o[0] != 0; break;
        case 0x840F: p->left   = (int)(int16_t)ud_rd16(o); break;
        case 0x840E: p->right  = (int)(int16_t)ud_rd16(o); break;
        case 0x8411: p->first  = (int)(int16_t)ud_rd16(o); break;
        case 0xA413: p->before = (int)ud_rd16(o); break;
        case 0xA414: p->after  = (int)ud_rd16(o); break;
        default: break;
        }
        at += 2 + olen;
    }
}

/* ---- the style sheet ([MS-DOC] STSH) ---------------------------------------
 * A count, then one variable-length STD per style.  Each STD is a small fixed
 * header, a UTF-16 name, and then `cupx` property blobs: a paragraph style
 * carries two (its PAPX then its CHPX), a character style carries one.  The
 * blobs are the SAME sprm grammar the direct formatting uses, which is what
 * makes resolution a matter of applying them in the right order rather than a
 * second interpreter.
 *
 * Every offset here is walked, never assumed: the name's length decides where
 * the first blob starts, and each blob's length decides the next.  Get one
 * wrong and every style after it is garbage. */
static int stsh_load(ud_doc *d, long fc, long lcb)
{
    long at, i;
    int cstd, cbbase, cbstshi;

    if (lcb < 6 || lcb > 8L * 1024 * 1024) return 0;
    d->stsh = (unsigned char *)ud_alloc((unsigned long)lcb);
    if (!d->stsh) return 0;
    if (ud_cfb_read(d->cfb, d->tbl, fc, d->stsh, lcb) != lcb) return 0;

    cbstshi = (int)ud_rd16(d->stsh);
    if (cbstshi < 4 || 2 + cbstshi > lcb) return 0;
    cstd   = (int)ud_rd16(d->stsh + 2);
    cbbase = (int)ud_rd16(d->stsh + 4);
    if (cstd <= 0 || cstd > MAX_STYLES || cbbase < 6) return 0;

    d->style = (ud_style *)ud_alloc((unsigned long)cstd * sizeof(ud_style));
    if (!d->style) return 0;
    memset(d->style, 0, (unsigned long)cstd * sizeof(ud_style));
    d->nstyle = cstd;
    for (i = 0; i < cstd; i++) d->style[i].base = ISTD_NONE;

    at = 2 + cbstshi;
    for (i = 0; i < cstd; i++) {
        long cbStd, p;
        const unsigned char *std;
        int cupx, k;

        if (at + 2 > lcb) break;
        cbStd = (long)ud_rd16(d->stsh + at);
        at += 2;
        if (cbStd == 0) continue;                 /* an unused style slot   */
        if (cbStd < 6 || at + cbStd > lcb) break;
        std = d->stsh + at;

        d->style[i].stk  = (int)(ud_rd16(std + 2) & 0x000F);
        d->style[i].base = (int)((ud_rd16(std + 2) >> 4) & 0x0FFF);
        cupx             = (int)(ud_rd16(std + 4) & 0x000F);

        p = cbbase;
        if (p + 2 <= cbStd) {                     /* Xstz: a counted name   */
            long cch = (long)ud_rd16(std + p);
            p += 2 + cch * 2 + 2;                 /* + its NUL terminator   */
        }
        if (p & 1) p++;
        for (k = 0; k < cupx && p + 2 <= cbStd; k++) {
            long cb = (long)ud_rd16(std + p);
            p += 2;
            if (cb < 0 || p + cb > cbStd) break;
            if (d->style[i].stk == STK_PARA && k == 0) {
                /* a paragraph style's PAPX blob leads with its own istd */
                if (cb >= 2) { d->style[i].papx = std + p + 2; d->style[i].npapx = cb - 2; }
            } else if ((d->style[i].stk == STK_PARA && k == 1) ||
                       (d->style[i].stk == STK_CHAR && k == 0)) {
                d->style[i].chpx = std + p;
                d->style[i].nchpx = cb;
            }
            p += cb;
            if (p & 1) p++;
        }
        at += cbStd;
    }
    return 1;
}

/* Apply a style's chain, ROOT FIRST, so a style overrides what it is based
 * on rather than the other way round. */
static void style_chp(ud_doc *d, int istd, ud_chp *c, int depth)
{
    ud_style *s;
    if (depth > STYLE_DEPTH || !d->style || istd < 0 || istd >= d->nstyle) return;
    s = &d->style[istd];
    if (s->base != ISTD_NONE && s->base != istd) style_chp(d, s->base, c, depth + 1);
    if (s->chpx) apply_chp(c, s->chpx, s->nchpx);
}

static void style_pap(ud_doc *d, int istd, ud_pap *p, int depth)
{
    ud_style *s;
    if (depth > STYLE_DEPTH || !d->style || istd < 0 || istd >= d->nstyle) return;
    s = &d->style[istd];
    if (s->base != ISTD_NONE && s->base != istd) style_pap(d, s->base, p, depth + 1);
    if (s->papx) apply_pap(p, s->papx, s->npapx);
}

/* A run's CHPX may name a CHARACTER style (sprmCIstd) whose properties sit
 * between the paragraph style's and the run's own direct formatting. */
static int chpx_cstyle(const unsigned char *g, long n)
{
    long at = 0;
    while (at + 2 <= n) {
        uint16_t sprm = ud_rd16(g + at);
        long olen = sprm_operand_len(sprm, g + at + 2, n - at - 2);
        if (olen < 0 || at + 2 + olen > n) break;
        if (sprm == 0x4A30 && olen >= 2) return (int)ud_rd16(g + at + 2);
        at += 2 + olen;
    }
    return -1;
}

/* Which file offset does document character `cp` live at?  The piece table
 * again - formatting is indexed by FILE position, not by character. */
static long cp_to_fc(ud_doc *d, long cp, int *wide)
{
    long i;
    for (i = 0; i < d->npc; i++)
        if (cp >= d->pc[i].cp0 && cp < d->pc[i].cp1) {
            *wide = d->pc[i].wide;
            return d->pc[i].fc + (cp - d->pc[i].cp0) * (d->pc[i].wide ? 2 : 1);
        }
    return -1;
}

/* Locate the direct CHPX for `fc`, if any.  Returns 1 and points g and glen
 * into `fkp` (which the caller owns) when one exists, 0 when the run has no
 * exception at all, -1 when the lookup failed. */
static int find_chpx(ud_doc *d, long fc, unsigned char *fkp,
                     const unsigned char **g, long *glen)
{
    long page, crun, j;

    *g = 0; *glen = 0;
    page = bte_page(&d->chpbte, fc);
    if (page < 0) return -1;
    if (ud_cfb_read(d->cfb, d->wd, page * FKP_SIZE, fkp, FKP_SIZE) != FKP_SIZE)
        return -1;
    crun = fkp[FKP_SIZE - 1];
    if (crun < 1 || 4 * (crun + 1) + crun > FKP_SIZE - 1) return -1;
    for (j = 0; j < crun; j++) {
        uint32_t f0 = ud_rd32(fkp + j * 4), f1 = ud_rd32(fkp + (j + 1) * 4);
        long off, cb;
        if ((uint32_t)fc < f0 || (uint32_t)fc >= f1) continue;
        off = (long)fkp[4 * (crun + 1) + j] * 2;
        if (off == 0) return 0;              /* no exception on this run   */
        if (off + 1 > FKP_SIZE) return -1;
        cb = fkp[off];
        if (off + 1 + cb > FKP_SIZE) return -1;
        *g = fkp + off + 1;
        *glen = cb;
        return 1;
    }
    return -1;
}

/* Character formatting resolves in three layers, outermost first: the
 * PARAGRAPH's style chain, then any CHARACTER style the run names, then the
 * run's own direct exceptions.  Get the order backwards and direct formatting
 * silently loses to the style it was meant to override. */
int ud_doc_chp_at(ud_doc *d, long cp, ud_chp *out)
{
    unsigned char fkp[FKP_SIZE];
    const unsigned char *g = 0;
    long fc, glen = 0;
    int wide = 0, found, cs;
    ud_pap pa;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!d) return 0;
    fc = cp_to_fc(d, cp, &wide);
    if (fc < 0) return 0;

    ud_doc_pap_at(d, cp, &pa);               /* for the paragraph's istd   */
    style_chp(d, pa.style, out, 0);

    found = find_chpx(d, fc, fkp, &g, &glen);
    if (found < 0) return 0;
    if (found == 1) {
        cs = chpx_cstyle(g, glen);
        if (cs >= 0) style_chp(d, cs, out, 0);
        apply_chp(out, g, glen);
    }
    return 1;
}

int ud_doc_pap_at(ud_doc *d, long cp, ud_pap *out)
{
    unsigned char fkp[FKP_SIZE];
    long fc, page, crun, j;
    int wide = 0;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!d) return 0;
    fc = cp_to_fc(d, cp, &wide);
    if (fc < 0) return 0;
    page = bte_page(&d->papbte, fc);
    if (page < 0) return 0;
    if (ud_cfb_read(d->cfb, d->wd, page * FKP_SIZE, fkp, FKP_SIZE) != FKP_SIZE)
        return 0;
    crun = fkp[FKP_SIZE - 1];
    /* a PAPX bin entry is 13 bytes, not 1 - the extra 12 are the BX */
    if (crun < 1 || 4 * (crun + 1) + crun * 13 > FKP_SIZE - 1) return 0;
    for (j = 0; j < crun; j++) {
        uint32_t f0 = ud_rd32(fkp + j * 4), f1 = ud_rd32(fkp + (j + 1) * 4);
        long off, cb, glen;
        if ((uint32_t)fc < f0 || (uint32_t)fc >= f1) continue;
        off = (long)fkp[4 * (crun + 1) + j * 13] * 2;
        if (off == 0 || off + 2 > FKP_SIZE) return 1;
        /* PapxInFkp: a word count, and if it is zero the REAL count is the
           next byte and the blob starts one further in.  Miss that and every
           long paragraph's properties come out as garbage. */
        cb = fkp[off];
        if (cb) { glen = cb * 2 - 1; off += 1; }
        else    { cb = fkp[off + 1]; glen = cb * 2; off += 2; }
        if (glen < 2 || off + glen > FKP_SIZE) return 0;
        out->style = (int)ud_rd16(fkp + off);       /* istd leads the blob */
        /* the style chain first, then the paragraph's own exceptions */
        style_pap(d, out->style, out, 0);
        out->style = (int)ud_rd16(fkp + off);
        apply_pap(out, fkp + off + 2, glen - 2);
        return 1;
    }
    return 0;
}

ud_doc *ud_doc_open(ud_cfb *c)
{
    ud_doc *d;
    int tbl, tid;
    long fclcb[4][2], fcClx, lcbClx, wdlen;
    /* The FIB's three counted arrays make it variable length.  Word 97 runs
       to about 900 bytes, but a file written by a later Word - or by
       LibreOffice, which stamps nFib 0x0101 and 136 rgFcLcb pairs - reaches
       1242.  4 KB covers every version that still says nFib, and the bounds
       checks in fib_parse report honestly rather than guessing if one ever
       does not fit. */
    unsigned char fib[4096];
    unsigned char *clx;

    ud_set_error("");
    if (!c) { ud_set_error("no container"); return 0; }
    d = (ud_doc *)ud_alloc(sizeof(ud_doc));
    if (!d) { ud_set_error("out of memory"); return 0; }
    memset(d, 0, sizeof *d);
    d->cfb = c;
    d->wd = ud_cfb_find(c, "/WordDocument");
    if (d->wd == UD_CFB_NONE) {
        ud_set_error("not a Word document (no WordDocument stream)");
        ud_doc_close(d);
        return 0;
    }
    wdlen = ud_cfb_size(c, d->wd);
    memset(fclcb, 0, sizeof fclcb);
    { long n = ud_cfb_read(c, d->wd, 0, fib, (long)sizeof fib);
      if (!fib_parse(d, fib, n, &tbl, fclcb)) { ud_doc_close(d); return 0; } }
    fcClx = fclcb[0][0]; lcbClx = fclcb[0][1];
    (void)wdlen;

    tid = ud_cfb_find(c, tbl ? "/1Table" : "/0Table");
    if (tid == UD_CFB_NONE) {
        /* the bit named a stream that is not there: try the other one rather
           than failing, since the document is otherwise readable */
        tid = ud_cfb_find(c, tbl ? "/0Table" : "/1Table");
    }
    if (tid == UD_CFB_NONE || lcbClx <= 0) {
        ud_set_error("Word: the piece table's stream is missing");
        ud_doc_close(d);
        return 0;
    }
    if (lcbClx > MAX_TEXT) { ud_set_error("Word: implausible CLX"); ud_doc_close(d); return 0; }
    clx = (unsigned char *)ud_alloc((unsigned long)lcbClx);
    if (!clx) { ud_set_error("out of memory (CLX)"); ud_doc_close(d); return 0; }
    if (ud_cfb_read(c, tid, fcClx, clx, lcbClx) != lcbClx) {
        ud_free(clx);
        ud_set_error("Word: could not read the piece table");
        ud_doc_close(d);
        return 0;
    }
    d->tbl = tid;
    if (!clx_pieces(d, clx, lcbClx) || !build_text(d)) {
        ud_free(clx);
        ud_doc_close(d);
        return 0;
    }
    ud_free(clx);
    /* The formatting bin tables are optional as far as opening goes: a
       document with no direct formatting still reads, and one whose tables
       are damaged should still give up its text. */
    bte_load(d, &d->chpbte, fclcb[1][0], fclcb[1][1]);
    bte_load(d, &d->papbte, fclcb[2][0], fclcb[2][1]);
    stsh_load(d, fclcb[3][0], fclcb[3][1]);
    return d;
}

void ud_doc_close(ud_doc *d)
{
    if (!d) return;
    ud_free(d->pc);
    ud_free(d->text);
    ud_free(d->plain);
    ud_free(d->chpbte.fc); ud_free(d->chpbte.pn);
    ud_free(d->papbte.fc); ud_free(d->papbte.pn);
    ud_free(d->stsh); ud_free(d->style);
    ud_free(d);
}

long        ud_doc_text_len(const ud_doc *d) { return d ? d->ccp : 0; }
const char *ud_doc_text(const ud_doc *d)     { return d && d->text ? d->text : ""; }
int         ud_doc_pieces(const ud_doc *d)   { return d ? d->npc : 0; }

/* ---- reading text -----------------------------------------------------------
 * The body text is full of in-band control characters.  This turns it into
 * something a person would recognise: paragraph and line marks become
 * newlines, cell marks become tabs, and a field's CODE is dropped while its
 * CACHED RESULT is kept - which is why a page number in a Word file shows up
 * as a number here rather than as "PAGE". */
const char *ud_doc_plain(ud_doc *d)
{
    long i, k = 0;
    int in_code = 0;

    if (!d) return "";
    if (d->plain) return d->plain;
    d->plain = (char *)ud_alloc((unsigned long)d->ccp + 1);
    if (!d->plain) return "";

    for (i = 0; i < d->ccp; i++) {
        unsigned char ch = (unsigned char)d->text[i];
        switch (ch) {
        case 0x13: in_code = 1; continue;     /* field begin: code follows */
        case 0x14: in_code = 0; continue;     /* separator: result follows */
        case 0x15: in_code = 0; continue;     /* field end                 */
        default: break;
        }
        if (in_code) continue;
        switch (ch) {
        case 0x07:                            /* cell / row mark           */
            d->plain[k++] = '\t'; break;
        case 0x0D: case 0x0B: case 0x0C:      /* paragraph, line, page     */
            d->plain[k++] = '\n'; break;
        case 0x01: case 0x08:                 /* picture / drawn object    */
        case 0x02: case 0x03: case 0x05:      /* footnote / annotation     */
            break;
        case 0x1E: d->plain[k++] = '-'; break;    /* non-breaking hyphen   */
        case 0x1F: break;                         /* optional hyphen       */
        case 0xA0: d->plain[k++] = ' '; break;    /* non-breaking space    */
        default:
            if (ch >= 0x20 || ch == '\t') d->plain[k++] = (char)ch;
            break;
        }
    }
    d->plain[k] = 0;
    return d->plain;
}
