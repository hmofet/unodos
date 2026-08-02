/* ===========================================================================
 * ud_xlsw.c - writing a BIFF8 workbook [MS-XLS].
 *
 * Same rule as the container: NEVER in place.  The caller builds a model and
 * this serialises a fresh workbook stream from it, which ud_xlsw_save then
 * wraps in a compound file.
 *
 * The globals preamble is written FROM THE SPEC rather than frozen as a
 * canned byte blob.  The plan allowed for canning one, and it is the usual
 * trick, but the records Excel actually insists on are few enough to emit
 * honestly - four FONTs (index 4 is skipped, which is the single strangest
 * thing about BIFF8 font numbering), the sixteen style XFs, the default cell
 * XF at 15, a Normal STYLE - and a blob nobody can read is a blob nobody can
 * fix.  Everything here is a record this file constructs field by field.
 *
 * Two things the writer has to get right that the reader taught us:
 *   - The SST is split at the 8224-byte record ceiling with CONTINUE, and a
 *     string cut mid-character has to restate its encoding flag in the new
 *     block.  We emit that restatement (see sst_emit), which is the mirror
 *     image of the trap ud_xls.c reads.
 *   - A string is written 8-bit only when every character fits in a byte of
 *     UTF-16 - CP-1252's 0x80-0x9F map to code points above 255 (the euro
 *     sign is U+20AC), so those strings must go out wide or come back wrong.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_xls_int.h"
#include <string.h>

#define W_MAXREC   8224          /* the BIFF record payload ceiling         */

/* ---- a growable byte buffer ------------------------------------------------ */
typedef struct { unsigned char *p; long n, cap; int bad; } wbuf;

static int wneed(wbuf *b, long n)
{
    unsigned char *np;
    long cap;
    if (b->bad) return 0;
    if (b->n + n <= b->cap) return 1;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->n + n) cap *= 2;
    np = (unsigned char *)ud_alloc((unsigned long)cap);
    if (!np) { b->bad = 1; return 0; }
    if (b->n) memcpy(np, b->p, (unsigned long)b->n);
    ud_free(b->p);
    b->p = np; b->cap = cap;
    return 1;
}
static void w8 (wbuf *b, unsigned v)
{ if (wneed(b, 1)) b->p[b->n++] = (unsigned char)v; }
static void w16(wbuf *b, unsigned v)
{ if (wneed(b, 2)) { ud_wr16(b->p + b->n, (uint16_t)v); b->n += 2; } }
static void w32(wbuf *b, unsigned long v)
{ if (wneed(b, 4)) { ud_wr32(b->p + b->n, (uint32_t)v); b->n += 4; } }
static void wdbl(wbuf *b, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    if (wneed(b, 8)) { ud_wr64(b->p + b->n, bits); b->n += 8; }
}
/* open a record header, remembering where its length field is */
static long rec_open(wbuf *b, unsigned type)
{
    long at;
    w16(b, type);
    at = b->n;
    w16(b, 0);
    return at;
}
static void rec_close(wbuf *b, long at)
{
    if (b->bad || at + 2 > b->n) return;
    ud_wr16(b->p + at, (uint16_t)(b->n - at - 2));
}

/* ---- strings ---------------------------------------------------------------
 * 8-bit only when every character fits in one byte of UTF-16. */
static int str_wide(const char *s)
{
    for (; s && *s; s++)
        if (ud_cp1252_to_uc((unsigned char)*s) > 0xFF) return 1;
    return 0;
}
static void wchars(wbuf *b, const char *s, long from, long to, int wide)
{
    long i;
    for (i = from; i < to; i++) {
        uint16_t u = ud_cp1252_to_uc((unsigned char)s[i]);
        if (wide) w16(b, u);
        else      w8 (b, u & 0xFF);
    }
}
/* XLUnicodeString with a 1-byte or 2-byte character count */
static void wstr(wbuf *b, const char *s, int wide_len)
{
    long n = s ? (long)strlen(s) : 0;
    int wide = str_wide(s);
    if (wide_len) w16(b, (unsigned)n); else w8(b, (unsigned)n);
    w8(b, wide ? 1 : 0);
    wchars(b, s, 0, n, wide);
}

/* ===========================================================================
 * the model
 * ======================================================================== */
typedef struct {
    uint32_t key;              /* row * UD_XLS_MAXCOL + col                 */
    int      kind;             /* UD_XV_*                                   */
    double   num;
    int      sst;              /* index into the shared string table        */
    int      err;
    int      xf;               /* 15 = the default cell XF                  */
    /* When set, this cell is a FORMULA: the token stream plus the cached
       result above, which is what the kind/num/sst/err fields then mean. */
    unsigned char *ptg;
    long           ptgn;
} wcell;

typedef struct {
    char     *name;
    wcell    *cell; int ncell, ccap;
    uint16_t *merge; int nmerge, mcap;
    long      bofpos;          /* backpatched into BOUNDSHEET               */
} wsheet;

struct ud_xlsw {
    wsheet  *sh;    int nsh, shcap;
    char   **sst;   int nsst, sstcap;    /* the shared strings, interned    */
    int     *sstbk; int sstbkn;          /* hash buckets -> sst index + 1   */
    char   **fmt;   int nfmt, fmtcap;    /* custom format codes             */
    int     *fmtid;                      /* the ifmt each one was given     */
    /* One cell XF per distinct number format a cell actually uses, from
       XF_FIRST up.  Built-in format ids need an XF just as much as custom
       ones do - the XF is what a cell points at, the FORMAT record only
       defines codes Excel does not already know. */
    int     *xfmt;  int nxfmt, xfmtcap;
    int      date1904;
    int      bad;
};

static int xf_slot(ud_xlsw *w, int ifmt)
{
    int i;
    for (i = 0; i < w->nxfmt; i++) if (w->xfmt[i] == ifmt) return i;
    if (w->nxfmt == w->xfmtcap) {
        int nc = w->xfmtcap ? w->xfmtcap * 2 : 16;
        int *n = (int *)ud_alloc((unsigned long)nc * sizeof(int));
        if (!n) { w->bad = 1; return -1; }
        if (w->nxfmt) memcpy(n, w->xfmt, (unsigned long)w->nxfmt * sizeof(int));
        ud_free(w->xfmt);
        w->xfmt = n; w->xfmtcap = nc;
    }
    w->xfmt[w->nxfmt] = ifmt;
    return w->nxfmt++;
}

/* ---- string interning ------------------------------------------------------ */
static unsigned str_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static int sst_intern(ud_xlsw *w, const char *s)
{
    unsigned h;
    int i, n;

    if (!s) s = "";
    /* grow the bucket array before it gets dense enough to cluster */
    if (w->sstbkn == 0 || w->nsst * 2 >= w->sstbkn) {
        int nb = w->sstbkn ? w->sstbkn * 2 : 256;
        int *bk = (int *)ud_alloc((unsigned long)nb * sizeof(int));
        if (!bk) { w->bad = 1; return -1; }
        memset(bk, 0, (unsigned long)nb * sizeof(int));
        for (i = 0; i < w->nsst; i++) {
            unsigned k = str_hash(w->sst[i]) & (unsigned)(nb - 1);
            while (bk[k]) k = (k + 1) & (unsigned)(nb - 1);
            bk[k] = i + 1;
        }
        ud_free(w->sstbk);
        w->sstbk = bk; w->sstbkn = nb;
    }
    h = str_hash(s) & (unsigned)(w->sstbkn - 1);
    while (w->sstbk[h]) {
        if (strcmp(w->sst[w->sstbk[h] - 1], s) == 0) return w->sstbk[h] - 1;
        h = (h + 1) & (unsigned)(w->sstbkn - 1);
    }
    if (w->nsst == w->sstcap) {
        int nc = w->sstcap ? w->sstcap * 2 : 64;
        char **ns = (char **)ud_alloc((unsigned long)nc * sizeof(char *));
        if (!ns) { w->bad = 1; return -1; }
        if (w->nsst) memcpy(ns, w->sst, (unsigned long)w->nsst * sizeof(char *));
        ud_free(w->sst);
        w->sst = ns; w->sstcap = nc;
    }
    n = (long)strlen(s) > 32767 ? 32767 : (int)strlen(s);
    w->sst[w->nsst] = (char *)ud_alloc((unsigned long)n + 1);
    if (!w->sst[w->nsst]) { w->bad = 1; return -1; }
    memcpy(w->sst[w->nsst], s, (unsigned long)n);
    w->sst[w->nsst][n] = 0;
    w->sstbk[h] = w->nsst + 1;
    return w->nsst++;
}

/* ---- number formats --------------------------------------------------------
 * A code that matches one of Excel's built-ins reuses its id, so a plain
 * "0.00" costs no FORMAT record.  Anything else gets an id from 164 up. */
static const struct { int id; const char *code; } BUILTIN[] = {
    { 0, "General" }, { 1, "0" }, { 2, "0.00" }, { 3, "#,##0" },
    { 4, "#,##0.00" }, { 9, "0%" }, { 10, "0.00%" }, { 11, "0.00E+00" },
    { 14, "m/d/yy" }, { 15, "d-mmm-yy" }, { 16, "d-mmm" }, { 17, "mmm-yy" },
    { 18, "h:mm AM/PM" }, { 19, "h:mm:ss AM/PM" }, { 20, "h:mm" },
    { 21, "h:mm:ss" }, { 22, "m/d/yy h:mm" }, { 45, "mm:ss" },
    { 46, "[h]:mm:ss" }, { 47, "mmss.0" }, { 48, "##0.0E+0" }, { 49, "@" }
};
#define NBUILTIN ((int)(sizeof BUILTIN / sizeof BUILTIN[0]))

static int fmt_intern(ud_xlsw *w, const char *code)
{
    int i;
    if (!code || !code[0]) return 0;
    for (i = 0; i < NBUILTIN; i++)
        if (strcmp(BUILTIN[i].code, code) == 0) return BUILTIN[i].id;
    for (i = 0; i < w->nfmt; i++)
        if (strcmp(w->fmt[i], code) == 0) return w->fmtid[i];
    if (w->nfmt == w->fmtcap) {
        int nc = w->fmtcap ? w->fmtcap * 2 : 16;
        char **nf = (char **)ud_alloc((unsigned long)nc * sizeof(char *));
        int   *ni = (int   *)ud_alloc((unsigned long)nc * sizeof(int));
        if (!nf || !ni) { ud_free(nf); ud_free(ni); w->bad = 1; return 0; }
        if (w->nfmt) {
            memcpy(nf, w->fmt,   (unsigned long)w->nfmt * sizeof(char *));
            memcpy(ni, w->fmtid, (unsigned long)w->nfmt * sizeof(int));
        }
        ud_free(w->fmt); ud_free(w->fmtid);
        w->fmt = nf; w->fmtid = ni; w->fmtcap = nc;
    }
    {
        long n = (long)strlen(code);
        char *c = (char *)ud_alloc((unsigned long)n + 1);
        if (!c) { w->bad = 1; return 0; }
        memcpy(c, code, (unsigned long)n + 1);
        w->fmt[w->nfmt] = c;
        w->fmtid[w->nfmt] = 164 + w->nfmt;
        return w->fmtid[w->nfmt++];
    }
}

/* ---- model building --------------------------------------------------------
 * XF numbering: 0-14 are the style XFs Excel requires, 15 is the default
 * cell XF, and each distinct number format used adds one cell XF from 16 up.
 * The cell stores that index, and the serialiser emits the table to match. */
#define XF_STYLES   15
#define XF_DEFAULT  15
#define XF_FIRST    16

ud_xlsw *ud_xlsw_new(void)
{
    ud_xlsw *w = (ud_xlsw *)ud_alloc(sizeof(ud_xlsw));
    if (!w) { ud_set_error("out of memory"); return 0; }
    memset(w, 0, sizeof *w);
    return w;
}

void ud_xlsw_free(ud_xlsw *w)
{
    int i;
    if (!w) return;
    for (i = 0; i < w->nsh; i++) {
        int k;
        for (k = 0; k < w->sh[i].ncell; k++) ud_free(w->sh[i].cell[k].ptg);
        ud_free(w->sh[i].name);
        ud_free(w->sh[i].cell);
        ud_free(w->sh[i].merge);
    }
    for (i = 0; i < w->nsst; i++) ud_free(w->sst[i]);
    for (i = 0; i < w->nfmt; i++) ud_free(w->fmt[i]);
    ud_free(w->sh); ud_free(w->sst); ud_free(w->sstbk);
    ud_free(w->fmt); ud_free(w->fmtid); ud_free(w->xfmt);
    ud_free(w);
}

int ud_xlsw_sheet(ud_xlsw *w, const char *name)
{
    long n;
    if (!w || !name || !name[0]) { ud_set_error("xls write: bad sheet name"); return -1; }
    n = (long)strlen(name);
    if (n > 31) { ud_set_error("xls write: sheet name over 31 characters"); return -1; }
    if (w->nsh == w->shcap) {
        int nc = w->shcap ? w->shcap * 2 : 4;
        wsheet *ns = (wsheet *)ud_alloc((unsigned long)nc * sizeof(wsheet));
        if (!ns) { ud_set_error("out of memory"); return -1; }
        memset(ns, 0, (unsigned long)nc * sizeof(wsheet));
        if (w->nsh) memcpy(ns, w->sh, (unsigned long)w->nsh * sizeof(wsheet));
        ud_free(w->sh);
        w->sh = ns; w->shcap = nc;
    }
    memset(&w->sh[w->nsh], 0, sizeof(wsheet));
    w->sh[w->nsh].name = (char *)ud_alloc((unsigned long)n + 1);
    if (!w->sh[w->nsh].name) { ud_set_error("out of memory"); return -1; }
    memcpy(w->sh[w->nsh].name, name, (unsigned long)n + 1);
    return w->nsh++;
}

/* find-or-create the record for one cell; writing twice replaces */
static wcell *cell_at(ud_xlsw *w, int s, int row, int col)
{
    wsheet *sh;
    uint32_t key;
    int i;

    if (!w || s < 0 || s >= w->nsh) { ud_set_error("xls write: bad sheet"); return 0; }
    if (row < 0 || row >= UD_XLS_MAXROW || col < 0 || col >= UD_XLS_MAXCOL) {
        ud_set_error("xls write: cell outside the 65536x256 grid");
        return 0;
    }
    sh = &w->sh[s];
    key = (uint32_t)row * UD_XLS_MAXCOL + (uint32_t)col;
    for (i = sh->ncell - 1; i >= 0; i--)          /* the common case is the */
        if (sh->cell[i].key == key) return &sh->cell[i];   /* one just added */
    if (sh->ncell == sh->ccap) {
        int nc = sh->ccap ? sh->ccap * 2 : 256;
        wcell *n = (wcell *)ud_alloc((unsigned long)nc * sizeof(wcell));
        if (!n) { ud_set_error("out of memory"); return 0; }
        if (sh->ncell) memcpy(n, sh->cell, (unsigned long)sh->ncell * sizeof(wcell));
        ud_free(sh->cell);
        sh->cell = n; sh->ccap = nc;
    }
    memset(&sh->cell[sh->ncell], 0, sizeof(wcell));
    sh->cell[sh->ncell].key = key;
    sh->cell[sh->ncell].xf = XF_DEFAULT;
    return &sh->cell[sh->ncell++];
}

int ud_xlsw_num(ud_xlsw *w, int s, int row, int col, double v)
{
    wcell *c = cell_at(w, s, row, col);
    if (!c) return 0;
    c->kind = UD_XV_NUM; c->num = v;
    return 1;
}
int ud_xlsw_str(ud_xlsw *w, int s, int row, int col, const char *t)
{
    wcell *c = cell_at(w, s, row, col);
    int i;
    if (!c) return 0;
    i = sst_intern(w, t);
    if (i < 0) return 0;
    c->kind = UD_XV_STR; c->sst = i;
    return 1;
}
int ud_xlsw_bool(ud_xlsw *w, int s, int row, int col, int v)
{
    wcell *c = cell_at(w, s, row, col);
    if (!c) return 0;
    c->kind = UD_XV_BOOL; c->num = v ? 1 : 0;
    return 1;
}
int ud_xlsw_err(ud_xlsw *w, int s, int row, int col, int err)
{
    wcell *c = cell_at(w, s, row, col);
    if (!c) return 0;
    c->kind = UD_XV_ERR; c->err = err;
    return 1;
}
int ud_xlsw_blank(ud_xlsw *w, int s, int row, int col)
{
    wcell *c = cell_at(w, s, row, col);
    if (!c) return 0;
    c->kind = UD_XV_EMPTY;
    return 1;
}

int ud_xlsw_format(ud_xlsw *w, int s, int row, int col, const char *code)
{
    wcell *c = cell_at(w, s, row, col);
    int ifmt;
    if (!c) return 0;
    ifmt = fmt_intern(w, code);
    if (ifmt == 0) { c->xf = XF_DEFAULT; return 1; }
    {
        int slot = xf_slot(w, ifmt);
        if (slot < 0) return 0;
        c->xf = XF_FIRST + slot;
    }
    return 1;
}

int ud_xlsw_merge(ud_xlsw *w, int s, int r0, int c0, int r1, int c1)
{
    wsheet *sh;
    if (!w || s < 0 || s >= w->nsh) return 0;
    sh = &w->sh[s];
    if (r0 > r1 || c0 > c1 || r0 < 0 || c0 < 0 ||
        r1 >= UD_XLS_MAXROW || c1 >= UD_XLS_MAXCOL) return 0;
    if (sh->nmerge == sh->mcap) {
        int nc = sh->mcap ? sh->mcap * 2 : 16;
        uint16_t *n = (uint16_t *)ud_alloc((unsigned long)nc * 4 * sizeof(uint16_t));
        if (!n) return 0;
        if (sh->nmerge) memcpy(n, sh->merge,
                               (unsigned long)sh->nmerge * 4 * sizeof(uint16_t));
        ud_free(sh->merge);
        sh->merge = n; sh->mcap = nc;
    }
    sh->merge[sh->nmerge * 4 + 0] = (uint16_t)r0;
    sh->merge[sh->nmerge * 4 + 1] = (uint16_t)r1;
    sh->merge[sh->nmerge * 4 + 2] = (uint16_t)c0;
    sh->merge[sh->nmerge * 4 + 3] = (uint16_t)c1;
    sh->nmerge++;
    return 1;
}

int ud_xlsw_date1904(ud_xlsw *w, int on)
{ if (!w) return 0; w->date1904 = on ? 1 : 0; return 1; }

/* ---- formulas (phase 3b) ---------------------------------------------------
 * The text is compiled straight away rather than at save time, so a syntax
 * error is reported to the caller who wrote it, at the cell they wrote it
 * for, instead of surfacing much later as a failed save. */
static int env_sheet_index(void *book, const char *name)
{
    ud_xlsw *w = (ud_xlsw *)book;
    int i;
    for (i = 0; i < w->nsh; i++) {
        const char *a = w->sh[i].name, *b = name;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
            if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
            if (ca != cb) break;
            a++; b++;
        }
        if (!*a && !*b) return i;
    }
    return -1;
}
static int env_name_index(void *book, const char *name)
{ (void)book; (void)name; return 0; }   /* defined names: not written yet */

int ud_xlsw_formula(ud_xlsw *w, int s, int row, int col, const char *text,
                    const ud_xcell *cached)
{
    ud_ptgc_env env;
    unsigned char *ptg;
    long n = 0;
    wcell *c;

    if (!w || !text) { ud_set_error("xls write: no formula text"); return 0; }
    env.book = w;
    env.sheet_index = env_sheet_index;
    env.name_index = env_name_index;
    ptg = ud_ptg_compile(text, &env, row, col, &n);
    if (!ptg) return 0;                      /* ud_error already says why  */

    c = cell_at(w, s, row, col);
    if (!c) { ud_free(ptg); return 0; }
    ud_free(c->ptg);
    c->ptg = ptg;
    c->ptgn = n;
    c->kind = cached ? cached->kind : UD_XV_NUM;
    c->num  = cached ? cached->num : 0;
    c->err  = cached ? cached->err : 0;
    if (c->kind == UD_XV_STR) {
        int i = sst_intern(w, cached && cached->str ? cached->str : "");
        if (i < 0) return 0;
        c->sst = i;                          /* the STRING record uses it  */
    }
    return 1;
}

/* ===========================================================================
 * serialisation
 * ======================================================================== */
static void cellsort_sift(wcell *a, int lo, int n)
{
    int root = lo;
    while (root * 2 + 1 < n) {
        int c = root * 2 + 1;
        if (c + 1 < n && a[c].key < a[c + 1].key) c++;
        if (a[root].key >= a[c].key) return;
        { wcell t = a[root]; a[root] = a[c]; a[c] = t; }
        root = c;
    }
}
static void cellsort(wcell *a, int n)
{
    int i;
    for (i = n / 2 - 1; i >= 0; i--) cellsort_sift(a, i, n);
    for (i = n - 1; i > 0; i--) {
        wcell t = a[0]; a[0] = a[i]; a[i] = t;
        cellsort_sift(a, 0, i);
    }
}

static void emit_bof(wbuf *b, unsigned dt)
{
    long at = rec_open(b, 0x0809);
    w16(b, 0x0600);            /* BIFF8                                     */
    w16(b, dt);
    w16(b, 0x0DBB);            /* rupBuild / rupYear: an Excel 97 build     */
    w16(b, 0x07CC);
    w32(b, 0x00000041);
    w32(b, 0x00000006);
    rec_close(b, at);
}
static void emit_eof(wbuf *b)
{ long at = rec_open(b, 0x000A); rec_close(b, at); }

static void emit_font(wbuf *b, int bold, const char *name)
{
    long at = rec_open(b, 0x0031);
    w16(b, 200);               /* 10pt, in twips                            */
    w16(b, 0);
    w16(b, 0x7FFF);            /* automatic colour                          */
    w16(b, bold ? 700 : 400);
    w16(b, 0);
    w8 (b, 0); w8 (b, 0); w8 (b, 0); w8 (b, 0);
    wstr(b, name, 0);
    rec_close(b, at);
}

static void emit_xf(wbuf *b, int ifmt, int style)
{
    long at = rec_open(b, 0x00E0);
    w16(b, 0);                                  /* ifnt                     */
    w16(b, (unsigned)ifmt);
    /* fLocked, plus fStyle and a 0xFFF parent for the style XFs */
    w16(b, style ? 0xFFF5 : 0x0001);
    w8 (b, 0);                                  /* alignment: general       */
    w8 (b, 0);                                  /* rotation                 */
    w8 (b, 0);                                  /* indent                   */
    /* usedAttrib: for a cell XF that carries its own number format, say so
       (fAtrNum), otherwise inherit everything from the parent style */
    w8 (b, (!style && ifmt) ? 0x04 : 0x00);
    w32(b, 0); w16(b, 0); w16(b, 0);            /* borders and fill         */
    rec_close(b, at);
}

/* the shared string table, split at the record ceiling ---------------------- */
static void sst_emit(wbuf *b, ud_xlsw *w, long total_refs)
{
    long at = rec_open(b, 0x00FC);
    long i;

    w32(b, (unsigned long)total_refs);
    w32(b, (unsigned long)w->nsst);
    for (i = 0; i < w->nsst; i++) {
        const char *s = w->sst[i];
        long len = (long)strlen(s), done = 0;
        int wide = str_wide(s);
        int per = wide ? 2 : 1;

        /* header: it is legal to split anywhere EXCEPT inside these three
           bytes, so start a CONTINUE first if they would not fit */
        if (b->n - at - 2 + 3 > W_MAXREC) {
            rec_close(b, at);
            at = rec_open(b, 0x003C);
        }
        w16(b, (unsigned)len);
        w8 (b, wide ? 1 : 0);
        while (done < len) {
            long room = (W_MAXREC - (b->n - at - 2)) / per;
            long take;
            if (room <= 0) {
                /* The split lands mid-string: the continuation block opens
                   with the encoding flag restated.  This is the writer half
                   of the trap ud_xls.c reads - a reader that ignores it gets
                   mojibake from here on. */
                rec_close(b, at);
                at = rec_open(b, 0x003C);
                w8(b, wide ? 1 : 0);
                room = (W_MAXREC - (b->n - at - 2)) / per;
            }
            take = len - done;
            if (take > room) take = room;
            wchars(b, s, done, done + take, wide);
            done += take;
        }
    }
    rec_close(b, at);
}

static long build_stream(ud_xlsw *w, wbuf *b)
{
    int i, k;
    long *boundpatch;
    long total_refs = 0;

    boundpatch = (long *)ud_alloc((unsigned long)(w->nsh ? w->nsh : 1) * sizeof(long));
    if (!boundpatch) { b->bad = 1; return 0; }

    for (i = 0; i < w->nsh; i++) {
        cellsort(w->sh[i].cell, w->sh[i].ncell);
        for (k = 0; k < w->sh[i].ncell; k++)
            if (w->sh[i].cell[k].kind == UD_XV_STR) total_refs++;
    }

    /* ---- globals ---------------------------------------------------------- */
    emit_bof(b, 0x0005);
    { long at = rec_open(b, 0x0042); w16(b, 0x04B0); rec_close(b, at); }   /* CODEPAGE 1200 */
    { long at = rec_open(b, 0x0022); w16(b, (unsigned)w->date1904); rec_close(b, at); }
    /* Four FONT records.  BIFF8 numbers fonts 0,1,2,3 then SKIPS 4 - an XF's
       ifnt of 4 means the fifth record.  We only use font 0, so the quirk
       costs nothing here, but the four records are required. */
    for (i = 0; i < 4; i++) emit_font(b, i == 1, "Arial");
    for (i = 0; i < w->nfmt; i++) {
        long at = rec_open(b, 0x041E);
        w16(b, (unsigned)w->fmtid[i]);
        wstr(b, w->fmt[i], 1);
        rec_close(b, at);
    }
    for (i = 0; i < XF_STYLES; i++) emit_xf(b, 0, 1);   /* the style XFs     */
    emit_xf(b, 0, 0);                                   /* XF 15: default    */
    for (i = 0; i < w->nxfmt; i++) emit_xf(b, w->xfmt[i], 0);
    { long at = rec_open(b, 0x0293);                    /* STYLE: Normal     */
      w16(b, 0x8000); w8(b, 0); w8(b, 0xFF); rec_close(b, at); }
    for (i = 0; i < w->nsh; i++) {
        long at = rec_open(b, 0x0085);
        boundpatch[i] = b->n;
        w32(b, 0);                                      /* lbPlyPos, patched */
        w8 (b, 0);                                      /* visible           */
        w8 (b, 0);                                      /* worksheet         */
        wstr(b, w->sh[i].name, 0);
        rec_close(b, at);
    }
    /* SUPBOOK + EXTERNSHEET: a 3-D reference names a sheet through an index
       into this table, not by name, so it has to exist before any formula
       can use one.  One internal SUPBOOK (this file), then one XTI per sheet
       so ixti and the sheet index are the same number. */
    { long at = rec_open(b, 0x01AE);
      w16(b, (unsigned)w->nsh); w16(b, 0x0401); rec_close(b, at); }
    { long at = rec_open(b, 0x0017);
      w16(b, (unsigned)w->nsh);
      for (i = 0; i < w->nsh; i++) { w16(b, 0); w16(b, (unsigned)i); w16(b, (unsigned)i); }
      rec_close(b, at); }
    if (w->nsst) sst_emit(b, w, total_refs);
    { long at = rec_open(b, 0x00FF); w16(b, 8); rec_close(b, at); }   /* EXTSST */
    emit_eof(b);

    /* ---- one substream per sheet ------------------------------------------ */
    for (i = 0; i < w->nsh; i++) {
        wsheet *sh = &w->sh[i];
        int rmax = 0, cmax = 0;
        if (b->bad) break;
        if (boundpatch[i] + 4 <= b->n) ud_wr32(b->p + boundpatch[i], (uint32_t)b->n);
        for (k = 0; k < sh->ncell; k++) {
            int r = (int)(sh->cell[k].key / UD_XLS_MAXCOL);
            int c = (int)(sh->cell[k].key % UD_XLS_MAXCOL);
            if (r + 1 > rmax) rmax = r + 1;
            if (c + 1 > cmax) cmax = c + 1;
        }
        emit_bof(b, 0x0010);
        { long at = rec_open(b, 0x0200);                /* DIMENSIONS        */
          w32(b, 0); w32(b, (unsigned long)rmax);
          w16(b, 0); w16(b, (unsigned)cmax); w16(b, 0);
          rec_close(b, at); }
        for (k = 0; k < sh->ncell; k++) {
            wcell *c = &sh->cell[k];
            int r = (int)(c->key / UD_XLS_MAXCOL);
            int col = (int)(c->key % UD_XLS_MAXCOL);
            long at;
            if (c->ptg) {
                /* FORMULA carries the expression AND its cached result, so a
                   reader that does not calculate still shows the right thing.
                   The result is either a raw double, or a tagged 8 bytes
                   ending 0xFFFF for the non-numeric kinds. */
                at = rec_open(b, 0x0006);
                w16(b, (unsigned)r); w16(b, (unsigned)col); w16(b, (unsigned)c->xf);
                switch (c->kind) {
                case UD_XV_STR:
                    w8(b, 0); w8(b, 0); w8(b, 0); w8(b, 0);
                    w8(b, 0); w8(b, 0); w16(b, 0xFFFF); break;
                case UD_XV_BOOL:
                    w8(b, 1); w8(b, 0); w8(b, c->num != 0); w8(b, 0);
                    w8(b, 0); w8(b, 0); w16(b, 0xFFFF); break;
                case UD_XV_ERR:
                    w8(b, 2); w8(b, 0); w8(b, (unsigned)c->err); w8(b, 0);
                    w8(b, 0); w8(b, 0); w16(b, 0xFFFF); break;
                case UD_XV_EMPTY:
                    w8(b, 3); w8(b, 0); w8(b, 0); w8(b, 0);
                    w8(b, 0); w8(b, 0); w16(b, 0xFFFF); break;
                default:
                    wdbl(b, c->num); break;
                }
                w16(b, 0);                            /* grbit             */
                w32(b, 0);                            /* chn               */
                w16(b, (unsigned)c->ptgn);
                if (wneed(b, c->ptgn)) {
                    memcpy(b->p + b->n, c->ptg, (unsigned long)c->ptgn);
                    b->n += c->ptgn;
                }
                rec_close(b, at);
                if (c->kind == UD_XV_STR) {           /* the text result   */
                    at = rec_open(b, 0x0207);
                    wstr(b, c->sst < w->nsst ? w->sst[c->sst] : "", 1);
                    rec_close(b, at);
                }
                continue;      /* not `break`: this is the cell loop, not a switch */
            }
            switch (c->kind) {
            case UD_XV_NUM:
                at = rec_open(b, 0x0203);
                w16(b, (unsigned)r); w16(b, (unsigned)col); w16(b, (unsigned)c->xf);
                wdbl(b, c->num);
                rec_close(b, at);
                break;
            case UD_XV_STR:
                at = rec_open(b, 0x00FD);
                w16(b, (unsigned)r); w16(b, (unsigned)col); w16(b, (unsigned)c->xf);
                w32(b, (unsigned long)c->sst);
                rec_close(b, at);
                break;
            case UD_XV_BOOL:
                at = rec_open(b, 0x0205);
                w16(b, (unsigned)r); w16(b, (unsigned)col); w16(b, (unsigned)c->xf);
                w8(b, c->num != 0); w8(b, 0);
                rec_close(b, at);
                break;
            case UD_XV_ERR:
                at = rec_open(b, 0x0205);
                w16(b, (unsigned)r); w16(b, (unsigned)col); w16(b, (unsigned)c->xf);
                w8(b, (unsigned)c->err); w8(b, 1);
                rec_close(b, at);
                break;
            default:
                at = rec_open(b, 0x0201);
                w16(b, (unsigned)r); w16(b, (unsigned)col); w16(b, (unsigned)c->xf);
                rec_close(b, at);
                break;
            }
        }
        if (sh->nmerge) {
            long at = rec_open(b, 0x00E5);
            w16(b, (unsigned)sh->nmerge);
            for (k = 0; k < sh->nmerge; k++) {
                w16(b, sh->merge[k * 4 + 0]); w16(b, sh->merge[k * 4 + 1]);
                w16(b, sh->merge[k * 4 + 2]); w16(b, sh->merge[k * 4 + 3]);
            }
            rec_close(b, at);
        }
        { long at = rec_open(b, 0x023E);                /* WINDOW2           */
          w16(b, 0x06B6); w16(b, 0); w16(b, 0);
          w32(b, 0x00000040); w16(b, 0); w16(b, 0); w32(b, 0);
          rec_close(b, at); }
        emit_eof(b);
    }
    ud_free(boundpatch);
    return b->bad ? 0 : 1;
}

unsigned char *ud_xlsw_save(ud_xlsw *w, long *len)
{
    wbuf b;
    ud_cfbw *c;
    unsigned char *img = 0;

    if (len) *len = 0;
    if (!w) { ud_set_error("no writer"); return 0; }
    if (!w->nsh) { ud_set_error("xls write: a workbook needs a sheet"); return 0; }
    memset(&b, 0, sizeof b);
    if (!build_stream(w, &b) || b.bad) {
        ud_free(b.p);
        ud_set_error("xls write: out of memory");
        return 0;
    }
    c = ud_cfbw_new();
    if (c) {
        if (ud_cfbw_stream(c, UD_CFB_ROOT_ID, "Workbook", b.p, b.n) != UD_CFB_NONE)
            img = ud_cfbw_serialize(c, len);
        ud_cfbw_free(c);
    }
    ud_free(b.p);
    return img;
}
