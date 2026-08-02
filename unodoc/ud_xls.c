/* ===========================================================================
 * ud_xls.c - the BIFF8 workbook [MS-XLS], read side.
 *
 * A .xls is a flat record stream - (u16 type, u16 length, payload) - living
 * in the container's "Workbook" stream.  First a globals substream (the
 * sheet directory, the shared string table, formats and cell formats), then
 * one substream per sheet, each a run of cell records bracketed by BOF/EOF.
 * Sheets are not scanned for: BOUNDSHEET gives each one's absolute offset,
 * so we seek.
 *
 * THE TRAP THIS FILE EXISTS TO GET RIGHT.  A record may not exceed 8224
 * bytes, so longer ones are split with CONTINUE records.  The shared string
 * table is routinely hundreds of KB, so it is always split - and a string
 * may be cut at ANY character, with the continuation block starting with a
 * fresh option byte that restates whether the text is 8-bit or UTF-16.  One
 * string can therefore change encoding halfway through.  Everything else
 * about BIFF8 is bookkeeping; this is the part that silently corrupts every
 * naive reader, so the record layer keeps the continuation boundaries and
 * the string reader consults them per character.  The same shape recurs in
 * .doc (piece table, fc bit 30) and .ppt (TextBytesAtom vs TextCharsAtom),
 * which is why it is written once, here, as sst_string().
 *
 * Defensive posture is ud_cfb.c's: every length is checked against the
 * record actually read, every seek against the stream size, every index
 * against the table it indexes.  A truncated or hostile workbook yields
 * fewer cells, never a crash and never a hang.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_xls_int.h"
#include <string.h>

/* ---- record types we act on ----------------------------------------------- */
#define R_FORMULA      0x0006
#define R_EOF          0x000A
#define R_CALCCOUNT    0x000C
#define R_DATEMODE     0x0022
#define R_FILEPASS     0x002F
#define R_FONT         0x0031
#define R_EXTERNSHEET  0x0017
#define R_NAME         0x0018
#define R_CONTINUE     0x003C
#define R_CODEPAGE     0x0042
#define R_COLINFO      0x007D
#define R_BOUNDSHEET   0x0085
#define R_MULRK        0x00BD
#define R_MULBLANK     0x00BE
#define R_XF           0x00E0
#define R_MERGEDCELLS  0x00E5
#define R_SST          0x00FC
#define R_LABELSST     0x00FD
#define R_BLANK        0x0201
#define R_NUMBER       0x0203
#define R_LABEL        0x0204
#define R_BOOLERR      0x0205
#define R_STRING       0x0207
#define R_ROW          0x0208
#define R_DIMENSIONS   0x0200
#define R_WINDOW2      0x023E
#define R_RK           0x027E
#define R_STYLE        0x0293
#define R_FORMAT       0x041E
#define R_SHRFMLA      0x04BC
#define R_ARRAY        0x0221
#define R_SUPBOOK      0x01AE
#define R_BOF          0x0809

/* BOF substream kinds */
#define DT_GLOBALS     0x0005
#define DT_WORKSHEET   0x0010

#define BIFF8          0x0600

/* Bounds we refuse to grow past, so a fuzzed length cannot ask for the moon */
#define MAX_RECORD     (32L * 1024 * 1024)
#define MAX_SST        1000000
#define MAX_CELLS      4000000
#define MAX_SHEETS     4096

/* ===========================================================================
 * a bounds-checked cursor over a byte range
 * ======================================================================== */
typedef struct {
    const unsigned char *p;
    long n, at;
    int  ok;                 /* cleared for good by the first overrun */
} ud_bs;

static void bs_init(ud_bs *b, const unsigned char *p, long n)
{ b->p = p; b->n = n; b->at = 0; b->ok = 1; }

static int bs_want(ud_bs *b, long n)
{
    if (!b->ok || n < 0 || b->at + n > b->n) { b->ok = 0; return 0; }
    return 1;
}
static uint8_t  bs_u8 (ud_bs *b) { if (!bs_want(b,1)) return 0;
                                   return b->p[b->at++]; }
static uint16_t bs_u16(ud_bs *b) { uint16_t v; if (!bs_want(b,2)) return 0;
                                   v = ud_rd16(b->p + b->at); b->at += 2; return v; }
static uint32_t bs_u32(ud_bs *b) { uint32_t v; if (!bs_want(b,4)) return 0;
                                   v = ud_rd32(b->p + b->at); b->at += 4; return v; }
static double   bs_dbl(ud_bs *b)
{
    uint64_t bits;
    double d = 0;
    if (!bs_want(b, 8)) return 0;
    bits = ud_rd64(b->p + b->at);
    b->at += 8;
    memcpy(&d, &bits, 8);        /* byte order already normalised by ud_rd64 */
    return d;
}
static void bs_skip(ud_bs *b, long n) { if (bs_want(b, n)) b->at += n; }

/* ===========================================================================
 * the record stream: one logical record at a time, CONTINUEs folded in and
 * their boundaries remembered
 * ======================================================================== */
typedef struct {
    ud_cfb        *cfb;
    int            sid;
    long           size;          /* the Workbook stream's length          */
    long           pos;           /* offset of the next record header      */
    uint16_t       type;
    unsigned char *buf;           /* the current record, continues appended*/
    long           len, cap;
    long          *bnd;           /* offsets in buf where a CONTINUE began */
    int            nbnd, bndcap, bndi;   /* bndi = forward scan cursor     */
} ud_xr;

static int xr_grow(ud_xr *r, long need)
{
    unsigned char *nb;
    long cap = r->cap ? r->cap : 8192;
    if (need > MAX_RECORD) return 0;
    while (cap < need) cap *= 2;
    if (cap <= r->cap) return 1;
    nb = (unsigned char *)ud_alloc((unsigned long)cap);
    if (!nb) return 0;
    if (r->len) memcpy(nb, r->buf, (unsigned long)r->len);
    ud_free(r->buf);
    r->buf = nb; r->cap = cap;
    return 1;
}

static int xr_bnd_add(ud_xr *r, long off)
{
    if (r->nbnd == r->bndcap) {
        int nc = r->bndcap ? r->bndcap * 2 : 16;
        long *nb = (long *)ud_alloc((unsigned long)nc * sizeof(long));
        if (!nb) return 0;
        if (r->nbnd) memcpy(nb, r->bnd, (unsigned long)r->nbnd * sizeof(long));
        ud_free(r->bnd);
        r->bnd = nb; r->bndcap = nc;
    }
    r->bnd[r->nbnd++] = off;
    return 1;
}

/* peek the type of the record at `off`, or 0xFFFF if there is none */
static uint16_t xr_peek(ud_xr *r, long off)
{
    unsigned char h[4];
    if (off + 4 > r->size) return 0xFFFF;
    if (ud_cfb_read(r->cfb, r->sid, off, h, 4) != 4) return 0xFFFF;
    return ud_rd16(h);
}

/* Read the next logical record.  1 = got one, 0 = end of stream / broken. */
static int xr_next(ud_xr *r)
{
    unsigned char h[4];
    long blen;

    r->len = 0; r->nbnd = 0; r->bndi = 0;
    if (r->pos + 4 > r->size) return 0;
    if (ud_cfb_read(r->cfb, r->sid, r->pos, h, 4) != 4) return 0;
    r->type = ud_rd16(h);
    blen    = (long)ud_rd16(h + 2);
    if (r->pos + 4 + blen > r->size) return 0;
    if (!xr_grow(r, blen + 1)) return 0;
    if (blen && ud_cfb_read(r->cfb, r->sid, r->pos + 4, r->buf, blen) != blen)
        return 0;
    r->len = blen;
    r->pos += 4 + blen;

    /* fold in every CONTINUE that follows, remembering where each began -
       the string reader needs those offsets to spot a mid-string split */
    while (xr_peek(r, r->pos) == R_CONTINUE) {
        long clen;
        if (ud_cfb_read(r->cfb, r->sid, r->pos, h, 4) != 4) break;
        clen = (long)ud_rd16(h + 2);
        if (r->pos + 4 + clen > r->size) break;
        if (!xr_grow(r, r->len + clen + 1)) break;
        if (clen && ud_cfb_read(r->cfb, r->sid, r->pos + 4,
                                r->buf + r->len, clen) != clen) break;
        if (!xr_bnd_add(r, r->len)) break;
        r->len += clen;
        r->pos += 4 + clen;
    }
    return 1;
}

static void xr_free(ud_xr *r) { ud_free(r->buf); ud_free(r->bnd); }

/* Is `at` exactly a continuation boundary?  Called per character while
 * reading string data, which is strictly forward, so the scan cursor makes
 * it O(1) amortised rather than O(boundaries) each time. */
static int xr_at_bnd(ud_xr *r, long at)
{
    while (r->bndi < r->nbnd && r->bnd[r->bndi] < at) r->bndi++;
    return r->bndi < r->nbnd && r->bnd[r->bndi] == at;
}

/* ===========================================================================
 * the workbook
 * ======================================================================== */
typedef struct {
    uint32_t  key;            /* row * UD_XLS_MAXCOL + col, for sorting     */
    ud_xcell  v;
} ud_xcellrec;

typedef struct {
    char        *name;
    long         pos;         /* lbPlyPos: where this sheet's BOF lives     */
    int          visible;
    int          worksheet;   /* 0 for chart / macro sheets                 */
    ud_xcellrec *cell;
    int          ncell, ccap;
    int          rows, cols;
    uint16_t    *merge;       /* 4 per range: r0 r1 c0 c1                   */
    int          nmerge, mcap;
} ud_xsheet;

typedef struct { uint16_t ifmt; } ud_xxf;

typedef struct { uint16_t sup, first, last; } ud_xti;

/* A shared formula: the expression stored ONCE for a filled-down block, with
 * relative PtgRefN tokens each member cell re-bases against itself. */
typedef struct {
    int            r0, c0;
    unsigned char *ptg;
    long           n;
} ud_xshr;

/* A cell that carried only a PtgExp: it points at a shared formula whose
 * SHRFMLA record has not been read yet (it follows the FIRST member), so the
 * text is resolved at end of sheet. */
typedef struct { int cell, er, ec, row, col; } ud_xpend;

struct ud_xls {
    ud_cfb    *cfb;
    int        sid;
    int        date1904;
    char     **sst;    int nsst;
    ud_xsheet *sh;     int nsh, shcap;
    ud_xxf    *xf;     int nxf, xfcap;
    /* number-format codes, sparse by ifmt id */
    char     **fmt;    int nfmt;
    char     **owned;  int nowned, ocap;   /* every string we allocated     */
    ud_xti    *xti;    int nxti, xticap;   /* EXTERNSHEET: 3-D ref targets  */
    unsigned char *supint; int nsup, supcap; /* which SUPBOOKs are this file */
    char     **name;   int nname, namecap; /* defined names                 */
    char       xtibuf[96];                 /* scratch for xti_name()        */
};

/* ---- string ownership: one list, freed at close --------------------------- */
static char *own(ud_xls *x, char *s)
{
    if (!s) return 0;
    if (x->nowned == x->ocap) {
        int nc = x->ocap ? x->ocap * 2 : 64;
        char **n = (char **)ud_alloc((unsigned long)nc * sizeof(char *));
        if (!n) { ud_free(s); return 0; }
        if (x->nowned) memcpy(n, x->owned, (unsigned long)x->nowned * sizeof(char *));
        ud_free(x->owned);
        x->owned = n; x->ocap = nc;
    }
    x->owned[x->nowned++] = s;
    return s;
}

/* ---- the string readers ----------------------------------------------------
 * BIFF8 text is UTF-16 or 8-bit-per-character, chosen by an option byte, and
 * folded here to CP-1252 (the v1 internal encoding - see UNODOC.md). */

/* A plain XLUnicodeString: u16 cch, u8 grbit, characters.  `wide_len` picks
 * the 2-byte (LABEL, FORMAT) or 1-byte (BOUNDSHEET) count field. */
static char *plain_string(ud_xls *x, ud_bs *b, int wide_len)
{
    long cch = wide_len ? (long)bs_u16(b) : (long)bs_u8(b);
    int  grbit = bs_u8(b);
    char *out;
    long i;

    if (!b->ok || cch < 0 || cch > 65535) return 0;
    out = (char *)ud_alloc((unsigned long)cch + 1);
    if (!out) return 0;
    for (i = 0; i < cch; i++) {
        uint16_t u = (grbit & 1) ? bs_u16(b) : (uint16_t)bs_u8(b);
        out[i] = (char)ud_uc_to_cp1252((grbit & 1) ? u
                                        : ud_cp1252_to_uc((unsigned char)u));
    }
    out[cch] = 0;
    if (!b->ok) { out[0] = 0; }
    return own(x, out);
}

/* An SST entry: XLUnicodeRichExtendedString, read THROUGH continuation
 * boundaries.  This is the one that matters (see the file header). */
static char *sst_string(ud_xls *x, ud_xr *r, ud_bs *b)
{
    long cch, i, nrun = 0, cbext = 0;
    int  grbit, rich, ext, wide;
    char *out;

    cch   = (long)bs_u16(b);
    grbit = bs_u8(b);
    if (!b->ok || cch < 0) return 0;
    wide = grbit & 0x01;
    ext  = grbit & 0x04;
    rich = grbit & 0x08;
    if (rich) nrun  = (long)bs_u16(b);
    if (ext)  cbext = (long)bs_u32(b);
    if (!b->ok || nrun < 0 || cbext < 0) return 0;

    out = (char *)ud_alloc((unsigned long)cch + 1);
    if (!out) return 0;
    for (i = 0; i < cch; i++) {
        uint16_t u;
        /* A split lands here: the continuation restates the width flag for
           the REST of this string.  Miss this and every string after the
           first split comes out as mojibake or half-length. */
        if (xr_at_bnd(r, b->at)) {
            int g = bs_u8(b);
            wide = g & 0x01;
        }
        u = wide ? bs_u16(b) : ud_cp1252_to_uc((unsigned char)bs_u8(b));
        if (!b->ok) break;
        out[i] = (char)ud_uc_to_cp1252(u);
    }
    out[i] = 0;
    /* the formatting runs and the phonetic blob are skipped, but they can
       also straddle a boundary - skipping by length is still correct because
       CONTINUE only restates the flag for CHARACTER data */
    bs_skip(b, nrun * 4);
    bs_skip(b, cbext);
    return own(x, out);
}

/* ---- RK: Excel's packed number --------------------------------------------
 * 30 significant bits, either a signed integer or the top half of a double,
 * optionally divided by 100.  Written with unsigned arithmetic only so no
 * step is implementation-defined under the sanitizers. */
static double rk_value(uint32_t rk)
{
    double d;
    uint32_t u = rk & 0xFFFFFFFCu;
    if (rk & 0x02) {
        if (u & 0x80000000u) d = -(double)(((~u) + 1u) / 4u);
        else                 d =  (double)(u / 4u);
    } else {
        uint64_t bits = (uint64_t)u << 32;
        memcpy(&d, &bits, 8);
    }
    if (rk & 0x01) d /= 100.0;
    return d;
}

const char *ud_xls_err_text(int err)
{
    switch (err) {
    case UD_XE_NULL:  return "#NULL!";
    case UD_XE_DIV0:  return "#DIV/0!";
    case UD_XE_VALUE: return "#VALUE!";
    case UD_XE_REF:   return "#REF!";
    case UD_XE_NAME:  return "#NAME?";
    case UD_XE_NUM:   return "#NUM!";
    case UD_XE_NA:    return "#N/A";
    default:          return "#ERR?";
    }
}

/* ---- what ud_ptg.c is allowed to ask the workbook -------------------------- */
static const char *env_xti_name(void *book, int ixti)
{
    ud_xls *x = (ud_xls *)book;
    const char *a, *b;
    unsigned long la, lb;

    if (ixti < 0 || ixti >= x->nxti) return 0;
    {
        ud_xti *t = &x->xti[ixti];
        if (t->sup < x->nsup && !x->supint[t->sup]) return 0;  /* external */
        if (t->first >= x->nsh) return 0;
        a = x->sh[t->first].name ? x->sh[t->first].name : "";
        if (t->last == t->first || t->last >= x->nsh) return a;
        b = x->sh[t->last].name ? x->sh[t->last].name : "";
        la = strlen(a); lb = strlen(b);
        if (la + lb + 2 > sizeof x->xtibuf) return a;
        memcpy(x->xtibuf, a, la);
        x->xtibuf[la] = ':';
        memcpy(x->xtibuf + la + 1, b, lb);
        x->xtibuf[la + 1 + lb] = 0;
        return x->xtibuf;
    }
}

static const char *env_name_of(void *book, int idx)
{
    ud_xls *x = (ud_xls *)book;
    if (idx < 0 || idx >= x->nname) return 0;
    return x->name[idx];
}

static const char *env_extname_of(void *book, int ixti, int idx)
{
    (void)ixti;
    return env_name_of(book, idx);
}

static void env_init(ud_xls *x, ud_ptg_env *e)
{
    e->book = x;
    e->xti_name = env_xti_name;
    e->name_of = env_name_of;
    e->extname_of = env_extname_of;
}

/* ---- cell collection ------------------------------------------------------- */
static ud_xcell *add_cell(ud_xsheet *s, int row, int col)
{
    ud_xcellrec *rec;
    if (row < 0 || row >= UD_XLS_MAXROW || col < 0 || col >= UD_XLS_MAXCOL)
        return 0;
    if (s->ncell >= MAX_CELLS) return 0;
    if (s->ncell == s->ccap) {
        int nc = s->ccap ? s->ccap * 2 : 256;
        ud_xcellrec *n = (ud_xcellrec *)ud_alloc((unsigned long)nc *
                                                 sizeof(ud_xcellrec));
        if (!n) return 0;
        if (s->ncell) memcpy(n, s->cell,
                             (unsigned long)s->ncell * sizeof(ud_xcellrec));
        ud_free(s->cell);
        s->cell = n; s->ccap = nc;
    }
    rec = &s->cell[s->ncell++];
    memset(rec, 0, sizeof *rec);
    rec->key = (uint32_t)row * UD_XLS_MAXCOL + (uint32_t)col;
    if (row + 1 > s->rows) s->rows = row + 1;
    if (col + 1 > s->cols) s->cols = col + 1;
    return &rec->v;
}

static void cells_sift(ud_xcellrec *a, int lo, int n)
{
    int root = lo;
    while (root * 2 + 1 < n) {
        int c = root * 2 + 1;
        if (c + 1 < n && a[c].key < a[c + 1].key) c++;
        if (a[root].key >= a[c].key) return;
        { ud_xcellrec t = a[root]; a[root] = a[c]; a[c] = t; }
        root = c;
    }
}

static void cells_sort(ud_xcellrec *a, int n)
{
    int i;
    for (i = n / 2 - 1; i >= 0; i--) cells_sift(a, i, n);
    for (i = n - 1; i > 0; i--) {
        ud_xcellrec t = a[0]; a[0] = a[i]; a[i] = t;
        cells_sift(a, 0, i);
    }
}

/* ---- number formats --------------------------------------------------------
 * Excel's built-in ids carry no FORMAT record; a file only stores the ones it
 * defines.  This is the 97 built-in table ([MS-XLS] 2.4.126); the gaps (5-8,
 * 23-26, 29-30, 33-36, 41-44, 50-58) are locale-dependent in real Excel and
 * are left empty rather than guessed at. */
static const char *builtin_fmt(int id)
{
    switch (id) {
    case 0:  return "General";
    case 1:  return "0";
    case 2:  return "0.00";
    case 3:  return "#,##0";
    case 4:  return "#,##0.00";
    case 9:  return "0%";
    case 10: return "0.00%";
    case 11: return "0.00E+00";
    case 12: return "# ?/?";
    case 13: return "# ?\?/??";   /* the escape dodges the ??/ trigraph */
    case 14: return "m/d/yy";
    case 15: return "d-mmm-yy";
    case 16: return "d-mmm";
    case 17: return "mmm-yy";
    case 18: return "h:mm AM/PM";
    case 19: return "h:mm:ss AM/PM";
    case 20: return "h:mm";
    case 21: return "h:mm:ss";
    case 22: return "m/d/yy h:mm";
    case 37: return "#,##0 ;(#,##0)";
    case 38: return "#,##0 ;[Red](#,##0)";
    case 39: return "#,##0.00;(#,##0.00)";
    case 40: return "#,##0.00;[Red](#,##0.00)";
    case 45: return "mm:ss";
    case 46: return "[h]:mm:ss";
    case 47: return "mmss.0";
    case 48: return "##0.0E+0";
    case 49: return "@";
    default: return "";
    }
}

#define NFMT 512      /* ifmt ids are u16 but Excel keeps them well under this */

int ud_xls_xf_format_id(const ud_xls *x, int xf)
{
    if (!x || xf < 0 || xf >= x->nxf) return 0;
    return x->xf[xf].ifmt;
}

const char *ud_xls_xf_format(const ud_xls *x, int xf)
{
    int id = ud_xls_xf_format_id(x, xf);
    if (x && x->fmt && id >= 0 && id < x->nfmt && x->fmt[id]) return x->fmt[id];
    return builtin_fmt(id);
}

/* ===========================================================================
 * parsing
 * ======================================================================== */
static ud_xsheet *add_sheet(ud_xls *x)
{
    if (x->nsh >= MAX_SHEETS) return 0;
    if (x->nsh == x->shcap) {
        int nc = x->shcap ? x->shcap * 2 : 8;
        ud_xsheet *n = (ud_xsheet *)ud_alloc((unsigned long)nc * sizeof(ud_xsheet));
        if (!n) return 0;
        memset(n, 0, (unsigned long)nc * sizeof(ud_xsheet));
        if (x->nsh) memcpy(n, x->sh, (unsigned long)x->nsh * sizeof(ud_xsheet));
        ud_free(x->sh);
        x->sh = n; x->shcap = nc;
    }
    memset(&x->sh[x->nsh], 0, sizeof(ud_xsheet));
    return &x->sh[x->nsh++];
}

static int parse_globals(ud_xls *x, ud_xr *r)
{
    int seen_bof = 0;

    x->fmt = (char **)ud_alloc(NFMT * sizeof(char *));
    if (!x->fmt) { ud_set_error("out of memory (formats)"); return 0; }
    memset(x->fmt, 0, NFMT * sizeof(char *));
    x->nfmt = NFMT;

    while (xr_next(r)) {
        ud_bs b;
        bs_init(&b, r->buf, r->len);

        switch (r->type) {
        case R_BOF: {
            uint16_t vers, dt;
            if (seen_bof) return 1;              /* the sheets start here   */
            vers = bs_u16(&b); dt = bs_u16(&b);
            if (dt != DT_GLOBALS) {
                ud_set_error("not a workbook (first substream is not globals)");
                return 0;
            }
            if (vers != BIFF8) {
                ud_set_error("BIFF5/BIFF7 workbook - not decoded in this build");
                return 0;
            }
            seen_bof = 1;
            break;
        }
        case R_FILEPASS:
            /* Encrypted.  Refuse plainly rather than emitting garbage - the
               spec's own conformance item, OFFICE97-SPEC S-OFF-06. */
            ud_set_error("this workbook is password-protected - not opened");
            return 0;

        case R_DATEMODE:
            x->date1904 = bs_u16(&b) ? 1 : 0;
            break;

        case R_BOUNDSHEET: {
            ud_xsheet *s = add_sheet(x);
            uint32_t pos = bs_u32(&b);
            int hs = bs_u8(&b), dt = bs_u8(&b);
            if (!s) break;
            s->pos       = (long)pos;
            s->visible   = (hs == 0);
            s->worksheet = (dt == 0);
            s->name      = plain_string(x, &b, 0);   /* 1-byte length       */
            if (!s->name) s->name = own(x, (char *)ud_alloc(1));
            if (s->name && !b.ok) s->name[0] = 0;
            break;
        }
        case R_FORMAT: {
            int id = (int)bs_u16(&b);
            char *code = plain_string(x, &b, 1);
            if (id >= 0 && id < x->nfmt && code) x->fmt[id] = code;
            break;
        }
        case R_XF: {
            if (x->nxf == x->xfcap) {
                int nc = x->xfcap ? x->xfcap * 2 : 32;
                ud_xxf *n = (ud_xxf *)ud_alloc((unsigned long)nc * sizeof(ud_xxf));
                if (!n) break;
                if (x->nxf) memcpy(n, x->xf, (unsigned long)x->nxf * sizeof(ud_xxf));
                ud_free(x->xf);
                x->xf = n; x->xfcap = nc;
            }
            bs_u16(&b);                           /* ifnt                   */
            x->xf[x->nxf].ifmt = bs_u16(&b);
            x->nxf++;
            break;
        }
        case R_SUPBOOK: {
            /* A 4-byte SUPBOOK whose second word is 0x0401 is this workbook
               referring to its own sheets; anything else is an external
               workbook, which v1 renders as #REF rather than guessing. */
            int internal = (r->len == 4 && ud_rd16(r->buf + 2) == 0x0401);
            if (x->nsup == x->supcap) {
                int nc = x->supcap ? x->supcap * 2 : 8;
                unsigned char *n = (unsigned char *)ud_alloc((unsigned long)nc);
                if (!n) break;
                memset(n, 0, (unsigned long)nc);
                if (x->nsup) memcpy(n, x->supint, (unsigned long)x->nsup);
                ud_free(x->supint);
                x->supint = n; x->supcap = nc;
            }
            x->supint[x->nsup++] = (unsigned char)internal;
            break;
        }
        case R_EXTERNSHEET: {
            int cnt = (int)bs_u16(&b), k;
            for (k = 0; k < cnt; k++) {
                uint16_t sup = bs_u16(&b), f = bs_u16(&b), l = bs_u16(&b);
                if (!b.ok) break;
                if (x->nxti == x->xticap) {
                    int nc = x->xticap ? x->xticap * 2 : 16;
                    ud_xti *n = (ud_xti *)ud_alloc((unsigned long)nc * sizeof(ud_xti));
                    if (!n) break;
                    if (x->nxti) memcpy(n, x->xti,
                                        (unsigned long)x->nxti * sizeof(ud_xti));
                    ud_free(x->xti);
                    x->xti = n; x->xticap = nc;
                }
                x->xti[x->nxti].sup = sup;
                x->xti[x->nxti].first = f;
                x->xti[x->nxti].last = l;
                x->nxti++;
            }
            break;
        }
        case R_NAME: {
            /* fixed header, then the name as an XLUnicodeString with its
               length already given by cch, then the definition ptgs */
            int cch;
            char *nm;
            bs_u16(&b);                       /* grbit                     */
            bs_u8(&b);                        /* chKey                     */
            cch = (int)bs_u8(&b);
            bs_u16(&b);                       /* cce                       */
            bs_u16(&b);                       /* ixals (unused)            */
            bs_u16(&b);                       /* itab                      */
            bs_skip(&b, 4);                   /* the four description cchs */
            {
                int grbit = bs_u8(&b), i;
                nm = (char *)ud_alloc((unsigned long)(cch > 0 ? cch : 0) + 1);
                if (!nm) break;
                for (i = 0; i < cch; i++) {
                    uint16_t u = (grbit & 1) ? bs_u16(&b)
                                 : ud_cp1252_to_uc((unsigned char)bs_u8(&b));
                    nm[i] = (char)ud_uc_to_cp1252(u);
                }
                nm[cch > 0 ? cch : 0] = 0;
                if (!b.ok) nm[0] = 0;
                own(x, nm);
            }
            if (x->nname == x->namecap) {
                int nc = x->namecap ? x->namecap * 2 : 8;
                char **n = (char **)ud_alloc((unsigned long)nc * sizeof(char *));
                if (!n) break;
                memset(n, 0, (unsigned long)nc * sizeof(char *));
                if (x->nname) memcpy(n, x->name,
                                     (unsigned long)x->nname * sizeof(char *));
                ud_free(x->name);
                x->name = n; x->namecap = nc;
            }
            x->name[x->nname++] = nm;
            break;
        }
        case R_SST: {
            long total, uniq, i;
            total = (long)bs_u32(&b); (void)total;
            uniq  = (long)bs_u32(&b);
            if (!b.ok || uniq < 0) break;
            if (uniq > MAX_SST) uniq = MAX_SST;
            /* A well-formed workbook has exactly one SST, but a damaged one
               can carry two - and silently overwriting the pointer leaks the
               first table.  Found by the workbook fuzzer, 2026-08-01. */
            ud_free(x->sst);
            x->sst = 0;
            x->nsst = 0;
            x->sst = (char **)ud_alloc((unsigned long)(uniq ? uniq : 1) *
                                       sizeof(char *));
            if (!x->sst) { ud_set_error("out of memory (SST)"); return 0; }
            memset(x->sst, 0, (unsigned long)(uniq ? uniq : 1) * sizeof(char *));
            for (i = 0; i < uniq; i++) {
                char *s = sst_string(x, r, &b);
                if (!b.ok) break;                 /* truncated table        */
                x->sst[i] = s;
            }
            x->nsst = (int)i;
            break;
        }
        case R_EOF:
            return 1;
        default:
            break;
        }
    }
    return 1;
}

/* The cached result packed into a FORMULA record's 8 result bytes.  Returns
 * 1 if the value is a non-empty string, which arrives in a following STRING
 * record - note that kind 3 is the EMPTY string and no STRING follows it, so
 * the two cannot be told apart by looking at c->str afterwards. */
static int formula_result(ud_xcell *c, const unsigned char *v)
{
    if (ud_rd16(v + 6) == 0xFFFF) {
        switch (v[0]) {
        case 0: c->kind = UD_XV_STR;  c->str = ""; return 1;
        case 1: c->kind = UD_XV_BOOL; c->num = v[2] ? 1 : 0; break;
        case 2: c->kind = UD_XV_ERR;  c->err = v[2]; break;
        case 3: c->kind = UD_XV_STR;  c->str = ""; break;   /* empty string   */
        default: c->kind = UD_XV_EMPTY; break;
        }
        return 0;
    }
    {
        uint64_t bits = ud_rd64(v);
        double d;
        memcpy(&d, &bits, 8);
        c->kind = UD_XV_NUM;
        c->num  = d;
    }
    return 0;
}

/* keep a copy of a ptg array; the record buffer is reused by the next read */
static unsigned char *keep(const unsigned char *p, long n)
{
    unsigned char *c = (unsigned char *)ud_alloc((unsigned long)(n ? n : 1));
    if (c && n) memcpy(c, p, (unsigned long)n);
    return c;
}

static void parse_sheet(ud_xls *x, ud_xr *r, ud_xsheet *s)
{
    int depth = 0;
    ud_xcell *pending_str = 0;      /* a formula whose result is a STRING   */
    ud_ptg_env env;
    ud_xshr  *shr = 0;   int nshr = 0, shrcap = 0;
    ud_xpend *pnd = 0;   int npnd = 0, pndcap = 0;
    int last_formula_cell = -1, last_fr = 0, last_fc = 0;

    env_init(x, &env);

    if (s->pos < 0 || s->pos >= r->size) return;
    r->pos = s->pos;

    while (xr_next(r)) {
        ud_bs b;
        bs_init(&b, r->buf, r->len);

        if (r->type == R_BOF) {
            /* an embedded chart or macro substream - skip it whole */
            if (depth++ > 0) continue;
            {
                uint16_t vers = bs_u16(&b), dt = bs_u16(&b);
                (void)vers;
                if (dt != DT_WORKSHEET) { s->worksheet = 0; }
            }
            continue;
        }
        if (r->type == R_EOF) {
            if (--depth <= 0) break;
            continue;
        }
        if (depth != 1) continue;            /* inside a nested substream   */

        switch (r->type) {
        case R_BLANK: {
            int row = bs_u16(&b), col = bs_u16(&b), xf = bs_u16(&b);
            ud_xcell *c = b.ok ? add_cell(s, row, col) : 0;
            if (c) { c->kind = UD_XV_EMPTY; c->xf = xf; }
            break;
        }
        case R_MULBLANK: {
            int row = bs_u16(&b), c0 = bs_u16(&b), k;
            long avail = (r->len - b.at - 2) / 2;
            for (k = 0; k < avail; k++) {
                int xf = bs_u16(&b);
                ud_xcell *c = b.ok ? add_cell(s, row, c0 + k) : 0;
                if (c) { c->kind = UD_XV_EMPTY; c->xf = xf; }
            }
            break;
        }
        case R_NUMBER: {
            int row = bs_u16(&b), col = bs_u16(&b), xf = bs_u16(&b);
            double d = bs_dbl(&b);
            ud_xcell *c = b.ok ? add_cell(s, row, col) : 0;
            if (c) { c->kind = UD_XV_NUM; c->num = d; c->xf = xf; }
            break;
        }
        case R_RK: {
            int row = bs_u16(&b), col = bs_u16(&b), xf = bs_u16(&b);
            uint32_t rk = bs_u32(&b);
            ud_xcell *c = b.ok ? add_cell(s, row, col) : 0;
            if (c) { c->kind = UD_XV_NUM; c->num = rk_value(rk); c->xf = xf; }
            break;
        }
        case R_MULRK: {
            int row = bs_u16(&b), c0 = bs_u16(&b), k;
            long avail = (r->len - b.at - 2) / 6;
            for (k = 0; k < avail; k++) {
                int xf = bs_u16(&b);
                uint32_t rk = bs_u32(&b);
                ud_xcell *c = b.ok ? add_cell(s, row, c0 + k) : 0;
                if (c) { c->kind = UD_XV_NUM; c->num = rk_value(rk); c->xf = xf; }
            }
            break;
        }
        case R_LABELSST: {
            int row = bs_u16(&b), col = bs_u16(&b), xf = bs_u16(&b);
            uint32_t isst = bs_u32(&b);
            ud_xcell *c = b.ok ? add_cell(s, row, col) : 0;
            if (c) {
                c->kind = UD_XV_STR;
                c->xf   = xf;
                c->str  = ((long)isst < x->nsst && x->sst[isst])
                          ? x->sst[isst] : "";
            }
            break;
        }
        case R_LABEL: {                       /* rare in BIFF8, legal       */
            int row = bs_u16(&b), col = bs_u16(&b), xf = bs_u16(&b);
            char *t = plain_string(x, &b, 1);
            ud_xcell *c = add_cell(s, row, col);
            if (c) { c->kind = UD_XV_STR; c->xf = xf; c->str = t ? t : ""; }
            break;
        }
        case R_BOOLERR: {
            int row = bs_u16(&b), col = bs_u16(&b), xf = bs_u16(&b);
            int v = bs_u8(&b), iserr = bs_u8(&b);
            ud_xcell *c = b.ok ? add_cell(s, row, col) : 0;
            if (c) {
                c->xf = xf;
                if (iserr) { c->kind = UD_XV_ERR;  c->err = v; }
                else       { c->kind = UD_XV_BOOL; c->num = v ? 1 : 0; }
            }
            break;
        }
        case R_FORMULA: {
            int row = bs_u16(&b), col = bs_u16(&b), xf = bs_u16(&b);
            ud_xcell *c;
            if (!bs_want(&b, 8)) break;
            c = add_cell(s, row, col);
            if (!c) break;
            c->xf = xf;
            c->formula = 1;
            pending_str = formula_result(c, b.p + b.at) ? c : 0;
            bs_skip(&b, 8);
            bs_u16(&b);                       /* grbit                     */
            bs_u32(&b);                       /* chn                       */
            {
                long cce = (long)bs_u16(&b);
                long avail = r->len - b.at;
                int er, ec;
                if (!b.ok || cce < 0 || cce > avail) break;
                last_formula_cell = s->ncell - 1;
                last_fr = row; last_fc = col;
                if (ud_ptg_is_exp(b.p + b.at, cce, &er, &ec)) {
                    /* points at a shared formula whose SHRFMLA record has
                       not arrived yet - resolve at end of sheet */
                    if (npnd == pndcap) {
                        int nc = pndcap ? pndcap * 2 : 32;
                        ud_xpend *n = (ud_xpend *)ud_alloc((unsigned long)nc *
                                                           sizeof(ud_xpend));
                        if (!n) break;
                        if (npnd) memcpy(n, pnd, (unsigned long)npnd * sizeof(ud_xpend));
                        ud_free(pnd);
                        pnd = n; pndcap = nc;
                    }
                    pnd[npnd].cell = s->ncell - 1;
                    pnd[npnd].er = er; pnd[npnd].ec = ec;
                    pnd[npnd].row = row; pnd[npnd].col = col;
                    npnd++;
                } else {
                    char *t = ud_ptg_text(b.p + b.at, cce, &env, row, col,
                                          b.p + b.at + cce, avail - cce);
                    if (t) c->ftext = own(x, t);
                }
            }
            break;
        }
        case R_SHRFMLA: {
            long cce;
            unsigned char *cp;
            bs_u16(&b); bs_u16(&b);           /* rowFirst, rowLast         */
            bs_u8(&b);  bs_u8(&b);            /* colFirst, colLast         */
            bs_u16(&b);                       /* reserved / cUse           */
            cce = (long)bs_u16(&b);
            if (!b.ok || cce < 0 || cce > r->len - b.at) break;
            if (last_formula_cell < 0) break;
            if (nshr == shrcap) {
                int nc = shrcap ? shrcap * 2 : 16;
                ud_xshr *n = (ud_xshr *)ud_alloc((unsigned long)nc * sizeof(ud_xshr));
                if (!n) break;
                if (nshr) memcpy(n, shr, (unsigned long)nshr * sizeof(ud_xshr));
                ud_free(shr);
                shr = n; shrcap = nc;
            }
            cp = keep(b.p + b.at, cce);
            if (!cp) break;
            /* keyed on the FIRST member cell, which is what a PtgExp names */
            shr[nshr].r0 = last_fr;
            shr[nshr].c0 = last_fc;
            shr[nshr].ptg = cp;
            shr[nshr].n = cce;
            nshr++;
            break;
        }
        case R_STRING: {
            char *t = plain_string(x, &b, 1);
            if (pending_str) { pending_str->str = t ? t : ""; pending_str = 0; }
            break;
        }
        case R_MERGEDCELLS: {
            int n = bs_u16(&b), k;
            for (k = 0; k < n; k++) {
                uint16_t r0 = bs_u16(&b), r1 = bs_u16(&b);
                uint16_t c0 = bs_u16(&b), c1 = bs_u16(&b);
                if (!b.ok) break;
                if (s->nmerge == s->mcap) {
                    int nc = s->mcap ? s->mcap * 2 : 16;
                    uint16_t *nm = (uint16_t *)ud_alloc((unsigned long)nc * 4 *
                                                        sizeof(uint16_t));
                    if (!nm) break;
                    if (s->nmerge) memcpy(nm, s->merge, (unsigned long)s->nmerge *
                                          4 * sizeof(uint16_t));
                    ud_free(s->merge);
                    s->merge = nm; s->mcap = nc;
                }
                s->merge[s->nmerge * 4 + 0] = r0;
                s->merge[s->nmerge * 4 + 1] = r1;
                s->merge[s->nmerge * 4 + 2] = c0;
                s->merge[s->nmerge * 4 + 3] = c1;
                s->nmerge++;
            }
            break;
        }
        default:
            break;
        }
    }
    /* Resolve the cells that only carried a PtgExp, now that every SHRFMLA
       for this sheet has been read.  Each one re-bases the shared
       expression's relative tokens against its OWN position - that is the
       whole point of a shared formula. */
    {
        int i, k;
        for (i = 0; i < npnd; i++) {
            for (k = 0; k < nshr; k++) {
                if (shr[k].r0 != pnd[i].er || shr[k].c0 != pnd[i].ec) continue;
                if (pnd[i].cell < 0 || pnd[i].cell >= s->ncell) break;
                {
                    char *t = ud_ptg_text(shr[k].ptg, shr[k].n, &env,
                                          pnd[i].row, pnd[i].col, 0, 0);
                    if (t) s->cell[pnd[i].cell].v.ftext = own(x, t);
                }
                break;
            }
        }
        for (k = 0; k < nshr; k++) ud_free(shr[k].ptg);
        ud_free(shr);
        ud_free(pnd);
    }
    cells_sort(s->cell, s->ncell);
}

/* ===========================================================================
 * open / close / accessors
 * ======================================================================== */
ud_xls *ud_xls_open(ud_cfb *c)
{
    ud_xls *x;
    ud_xr   r;
    int     sid, i;

    ud_set_error("");
    if (!c) { ud_set_error("no container"); return 0; }
    sid = ud_cfb_find(c, "/Workbook");
    if (sid == UD_CFB_NONE) sid = ud_cfb_find(c, "/Book");
    if (sid == UD_CFB_NONE || ud_cfb_type(c, sid) != UD_ENT_STREAM) {
        ud_set_error("not a workbook (no Workbook stream)");
        return 0;
    }
    x = (ud_xls *)ud_alloc(sizeof(ud_xls));
    if (!x) { ud_set_error("out of memory"); return 0; }
    memset(x, 0, sizeof *x);
    x->cfb = c;
    x->sid = sid;

    memset(&r, 0, sizeof r);
    r.cfb  = c;
    r.sid  = sid;
    r.size = ud_cfb_size(c, sid);

    if (!parse_globals(x, &r)) { xr_free(&r); ud_xls_close(x); return 0; }
    for (i = 0; i < x->nsh; i++)
        if (x->sh[i].worksheet) parse_sheet(x, &r, &x->sh[i]);
    xr_free(&r);
    return x;
}

void ud_xls_close(ud_xls *x)
{
    int i;
    if (!x) return;
    for (i = 0; i < x->nsh; i++) {
        ud_free(x->sh[i].cell);
        ud_free(x->sh[i].merge);
    }
    for (i = 0; i < x->nowned; i++) ud_free(x->owned[i]);
    ud_free(x->owned);
    ud_free(x->sh);
    ud_free(x->sst);
    ud_free(x->xf);
    ud_free(x->fmt);
    ud_free(x->xti);
    ud_free(x->supint);
    ud_free(x->name);
    ud_free(x);
}

static const ud_xsheet *sheet(const ud_xls *x, int s)
{ return (x && s >= 0 && s < x->nsh) ? &x->sh[s] : 0; }

int ud_xls_sheets(const ud_xls *x) { return x ? x->nsh : 0; }
int ud_xls_date1904(const ud_xls *x) { return x ? x->date1904 : 0; }

const char *ud_xls_sheet_name(const ud_xls *x, int s)
{ const ud_xsheet *p = sheet(x, s); return p && p->name ? p->name : ""; }
int ud_xls_sheet_visible(const ud_xls *x, int s)
{ const ud_xsheet *p = sheet(x, s); return p ? p->visible : 0; }
int ud_xls_rows(const ud_xls *x, int s)
{ const ud_xsheet *p = sheet(x, s); return p ? p->rows : 0; }
int ud_xls_cols(const ud_xls *x, int s)
{ const ud_xsheet *p = sheet(x, s); return p ? p->cols : 0; }
int ud_xls_cell_count(const ud_xls *x, int s)
{ const ud_xsheet *p = sheet(x, s); return p ? p->ncell : 0; }

int ud_xls_cell_at(const ud_xls *x, int s, int i, int *row, int *col,
                   ud_xcell *out)
{
    const ud_xsheet *p = sheet(x, s);
    if (out) memset(out, 0, sizeof *out);
    if (!p || i < 0 || i >= p->ncell) return 0;
    if (row) *row = (int)(p->cell[i].key / UD_XLS_MAXCOL);
    if (col) *col = (int)(p->cell[i].key % UD_XLS_MAXCOL);
    if (out) *out = p->cell[i].v;
    return 1;
}

int ud_xls_cell(const ud_xls *x, int s, int row, int col, ud_xcell *out)
{
    const ud_xsheet *p = sheet(x, s);
    uint32_t key;
    int lo, hi;

    if (out) memset(out, 0, sizeof *out);
    if (!p || row < 0 || col < 0 || col >= UD_XLS_MAXCOL) return 0;
    key = (uint32_t)row * UD_XLS_MAXCOL + (uint32_t)col;
    lo = 0; hi = p->ncell - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->cell[mid].key == key) {
            if (out) *out = p->cell[mid].v;
            return 1;
        }
        if (p->cell[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

int ud_xls_merges(const ud_xls *x, int s)
{ const ud_xsheet *p = sheet(x, s); return p ? p->nmerge : 0; }

int ud_xls_merge(const ud_xls *x, int s, int i,
                 int *row0, int *col0, int *row1, int *col1)
{
    const ud_xsheet *p = sheet(x, s);
    if (!p || i < 0 || i >= p->nmerge) return 0;
    if (row0) *row0 = p->merge[i * 4 + 0];
    if (row1) *row1 = p->merge[i * 4 + 1];
    if (col0) *col0 = p->merge[i * 4 + 2];
    if (col1) *col1 = p->merge[i * 4 + 3];
    return 1;
}
