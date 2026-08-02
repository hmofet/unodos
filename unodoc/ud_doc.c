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

#define FCLB_CLX       33        /* fcClx is pair 33 of rgFcLcb97          */

#define MAX_TEXT       (64L * 1024 * 1024)
#define MAX_PIECES     1000000

typedef struct {
    long cp0, cp1;         /* document character range this piece supplies */
    long fc;               /* byte offset in WordDocument                  */
    int  wide;             /* 0 = 8-bit CP-1252, 1 = UTF-16LE             */
} ud_piece;

struct ud_doc {
    ud_cfb   *cfb;
    int       wd;                    /* WordDocument stream id            */
    long      ccp;                   /* characters of body text           */
    ud_piece *pc;  int npc;
    char     *text;                  /* the body, CP-1252, NUL-terminated */
    char     *plain;                 /* reading text (built on demand)    */
};

/* ---- the FIB ---------------------------------------------------------------
 * Everything after offset 0x20 is variable length: three counted arrays, one
 * after another.  Walk them rather than assuming offsets, because getting
 * this wrong silently reads the wrong table. */
static int fib_parse(ud_doc *d, const unsigned char *f, long n,
                     int *whichtbl, long *fcClx, long *lcbClx)
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
    *fcClx  = (long)ud_rd32(f + off + FCLB_CLX * 8);
    *lcbClx = (long)ud_rd32(f + off + FCLB_CLX * 8 + 4);
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

ud_doc *ud_doc_open(ud_cfb *c)
{
    ud_doc *d;
    int tbl, tid;
    long fcClx = 0, lcbClx = 0, wdlen;
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
    { long n = ud_cfb_read(c, d->wd, 0, fib, (long)sizeof fib);
      if (!fib_parse(d, fib, n, &tbl, &fcClx, &lcbClx)) { ud_doc_close(d); return 0; } }
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
    if (!clx_pieces(d, clx, lcbClx) || !build_text(d)) {
        ud_free(clx);
        ud_doc_close(d);
        return 0;
    }
    ud_free(clx);
    return d;
}

void ud_doc_close(ud_doc *d)
{
    if (!d) return;
    ud_free(d->pc);
    ud_free(d->text);
    ud_free(d->plain);
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
