/* ===========================================================================
 * uxl_sheet.c - UnoCalc's sparse workbook store (OFFICE97-PLAN §6 phase 9).
 *
 * A 65536 x 256 grid is 16.7 million cells and a real worksheet holds a few
 * hundred, so the store is a SORTED ARRAY of live cells per sheet, binary
 * searched by (row, col).  That is the same reasoning BIFF8 applies when it
 * stores rows as records rather than as a rectangle, and it means "walk
 * every cell" is O(live) rather than O(grid).
 * ======================================================================== */
#include "uocalc.h"
#include "uxl_int.h"

void *malloc(unsigned long);
void  free(void *);

static void x_memset(void *d, int c, long n)
{ char *p = (char *)d; long i; for (i = 0; i < n; i++) p[i] = (char)c; }
static int x_streq(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void x_cpy(char *d, const char *s, int cap)
{ int i = 0; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }

const char *uxl_err_text(int err)
{
    switch (err) {
    case UXL_E_NULL:  return "#NULL!";
    case UXL_E_DIV0:  return "#DIV/0!";
    case UXL_E_VALUE: return "#VALUE!";
    case UXL_E_REF:   return "#REF!";
    case UXL_E_NAME:  return "#NAME?";
    case UXL_E_NUM:   return "#NUM!";
    case UXL_E_NA:    return "#N/A";
    default:          return "#ERR";
    }
}

/* ---- the book -------------------------------------------------------------- */
static uxl_book g_book;

uxl_book *uxl_new(void)
{
    uxl_book *b = &g_book;
    x_memset(b, 0, (long)sizeof *b);
    uxl_sheet_add(b, "Sheet1");
    uxl_sheet_add(b, "Sheet2");
    uxl_sheet_add(b, "Sheet3");
    return b;
}
void uxl_free(uxl_book *b) { (void)b; }

int uxl_sheet_add(uxl_book *b, const char *name)
{
    if (!b || b->nsheet >= UXL_MAXSHEET) return -1;
    x_cpy(b->sheet[b->nsheet].name, name, 32);
    b->sheet[b->nsheet].ncell = 0;
    return b->nsheet++;
}
int uxl_sheets(const uxl_book *b) { return b ? b->nsheet : 0; }
const char *uxl_sheet_name(const uxl_book *b, int s)
{ return (b && s >= 0 && s < b->nsheet) ? b->sheet[s].name : ""; }
int uxl_sheet_find(const uxl_book *b, const char *name)
{
    int i;
    if (!b || !name) return -1;
    for (i = 0; i < b->nsheet; i++)
        if (x_streq(b->sheet[i].name, name)) return i;
    return -1;
}

/* ---- the string pool ------------------------------------------------------- */
int uxl_intern(uxl_book *b, const char *t)
{
    int i;
    if (!b || !t) return -1;
    for (i = 0; i < b->npool; i++)
        if (x_streq(b->pool[i], t)) return i;
    if (b->npool >= UXL_MAXSTR) return -1;
    x_cpy(b->pool[b->npool], t, UXL_STRLEN);
    return b->npool++;
}
const char *uxl_pool(const uxl_book *b, int idx)
{ return (b && idx >= 0 && idx < b->npool) ? b->pool[idx] : ""; }

/* ---- lookup ----------------------------------------------------------------
 * Cells are kept sorted by (row, col) so the search is a bisection and the
 * walk is already in the order a painter wants. */
static long key_of(int r, int c) { return (long)r * UXL_COLS + c; }

/* The cell pool: a bump half plus a free stack of cells uxl_clear released.
 * Sheets hold indices, so a cell never moves and a pointer into the pool
 * stays valid across inserts - which the calculator relies on. */
static int cell_alloc(uxl_book *b)
{
    if (b->nfree > 0)            return b->freecell[--b->nfree];
    if (b->nalloc < UXL_MAXCELL) return b->nalloc++;
    return -1;
}
static void cell_release(uxl_book *b, int ci)
{
    if (b->nfree < UXL_MAXCELL) b->freecell[b->nfree++] = (unsigned short)ci;
}

uxl_cell *uxl_nth(uxl_book *b, int s, int i)
{
    if (!b || s < 0 || s >= b->nsheet || i < 0 || i >= b->sheet[s].ncell) return 0;
    return &b->cell[b->sheet[s].idx[i]];
}
static const uxl_cell *nth_c(const uxl_book *b, int s, int i)
{ return &b->cell[b->sheet[s].idx[i]]; }

int uxl_find(const uxl_book *b, int s, int r, int c)
{
    const uxl_sheet *sh;
    long k = key_of(r, c);
    int lo, hi, mid;
    if (!b || s < 0 || s >= b->nsheet) return -1;
    sh = &b->sheet[s];
    lo = 0; hi = sh->ncell - 1;
    while (lo <= hi) {
        const uxl_cell *p;
        long km;
        mid = (lo + hi) / 2;
        p = nth_c(b, s, mid);
        km = key_of(p->row, p->col);
        if (km == k) return mid;
        if (km < k) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

uxl_cell *uxl_slot(uxl_book *b, int s, int r, int c, int make)
{
    uxl_sheet *sh;
    long k;
    int lo, hi, mid, at, i, ci;
    if (!b || s < 0 || s >= b->nsheet) return 0;
    if (r < 0 || r >= UXL_ROWS || c < 0 || c >= UXL_COLS) return 0;
    sh = &b->sheet[s];
    k = key_of(r, c);
    lo = 0; hi = sh->ncell - 1; at = sh->ncell;
    while (lo <= hi) {
        uxl_cell *p;
        long km;
        mid = (lo + hi) / 2;
        p = &b->cell[sh->idx[mid]];
        km = key_of(p->row, p->col);
        if (km == k) return p;
        if (km < k) lo = mid + 1; else { at = mid; hi = mid - 1; }
    }
    if (!make) return 0;
    if (sh->ncell >= UXL_MAXCELL) return 0;
    ci = cell_alloc(b);
    if (ci < 0) return 0;
    if (lo > at) at = lo;
    for (i = sh->ncell; i > at; i--) sh->idx[i] = sh->idx[i - 1];
    sh->idx[at] = (unsigned short)ci;
    x_memset(&b->cell[ci], 0, (long)sizeof b->cell[ci]);
    b->cell[ci].row = (unsigned short)r;
    b->cell[ci].col = (unsigned short)c;
    sh->ncell++;
    return &b->cell[ci];
}

/* ---- the token pool ---------------------------------------------------------
 * Setting a formula bump-allocates; overwriting one leaks the old tokens.
 * That is deliberate: the source text is kept in the cell, so when the pool
 * fills, every live formula can simply be RECOMPILED into a fresh pool.  A
 * rebuild costs one compile per formula and needs no back-pointers, free
 * list or compaction pass to go wrong. */
static int g_rebuilding;

static int rpn_rebuild(uxl_book *b)
{
    static uxl_tok tmp[UXL_MAXRPN];
    int s, i, j, n, ok = 1;
    g_rebuilding = 1;
    b->nrpnpool = 0;
    for (s = 0; s < b->nsheet; s++)
        for (i = 0; i < b->sheet[s].ncell; i++) {
            uxl_cell *p = uxl_nth(b, s, i);
            if (!p->nrpn) continue;
            n = uxl_compile(b, p->fml, s, tmp, UXL_MAXRPN);
            if (n < 0 || b->nrpnpool + n > UXL_RPNPOOL) {
                p->nrpn = 0; ok = 0; continue;   /* visibly a literal now   */
            }
            p->rpn_at = b->nrpnpool;
            for (j = 0; j < n; j++) b->rpn[b->nrpnpool + j] = tmp[j];
            b->nrpnpool += n;
            p->nrpn = (short)n;
        }
    g_rebuilding = 0;
    return ok;
}

int uxl_rpn_store(uxl_book *b, uxl_cell *p, const uxl_tok *t, int n)
{
    int i;
    if (!b || !p) return 0;
    if (n <= 0) { p->nrpn = 0; p->rpn_at = 0; return 1; }
    if (b->nrpnpool + n > UXL_RPNPOOL && !g_rebuilding) {
        p->nrpn = 0;               /* this cell is mid-set: don't rebuild it */
        rpn_rebuild(b);
    }
    if (b->nrpnpool + n > UXL_RPNPOOL) return 0;
    p->rpn_at = b->nrpnpool;
    for (i = 0; i < n; i++) b->rpn[b->nrpnpool + i] = t[i];
    b->nrpnpool += n;
    p->nrpn = (short)n;
    return 1;
}

/* ---- setters ---------------------------------------------------------------- */
static void bump(uxl_book *b) { b->rev++; b->dirty = 1; }

int uxl_set_num(uxl_book *b, int s, int r, int c, double v)
{
    uxl_cell *p = uxl_slot(b, s, r, c, 1);
    if (!p) return 0;
    p->v.kind = UXL_NUM; p->v.num = v;
    p->fml[0] = 0; p->nrpn = 0;
    bump(b);
    return 1;
}
int uxl_set_str(uxl_book *b, int s, int r, int c, const char *t)
{
    uxl_cell *p = uxl_slot(b, s, r, c, 1);
    int idx;
    if (!p) return 0;
    idx = uxl_intern(b, t);
    if (idx < 0) return 0;
    p->v.kind = UXL_STR; p->v.str = idx;
    p->fml[0] = 0; p->nrpn = 0;
    bump(b);
    return 1;
}
int uxl_set_bool(uxl_book *b, int s, int r, int c, int on)
{
    uxl_cell *p = uxl_slot(b, s, r, c, 1);
    if (!p) return 0;
    p->v.kind = UXL_BOOL; p->v.num = on ? 1 : 0;
    p->fml[0] = 0; p->nrpn = 0;
    bump(b);
    return 1;
}
int uxl_set_err(uxl_book *b, int s, int r, int c, int err)
{
    uxl_cell *p = uxl_slot(b, s, r, c, 1);
    if (!p) return 0;
    p->v.kind = UXL_ERR; p->v.err = err;
    p->fml[0] = 0; p->nrpn = 0;
    bump(b);
    return 1;
}
int uxl_clear(uxl_book *b, int s, int r, int c)
{
    uxl_sheet *sh;
    int i = uxl_find(b, s, r, c), j;
    if (i < 0) return 0;
    sh = &b->sheet[s];
    cell_release(b, sh->idx[i]);
    for (j = i; j + 1 < sh->ncell; j++) sh->idx[j] = sh->idx[j + 1];
    sh->ncell--;
    bump(b);
    return 1;
}

int uxl_get(const uxl_book *b, int s, int r, int c, uxl_val *out)
{
    int i = uxl_find(b, s, r, c);
    if (!out) return 0;
    if (i < 0) { x_memset(out, 0, (long)sizeof *out); return 0; }
    *out = nth_c(b, s, i)->v;
    return 1;
}
const char *uxl_formula(const uxl_book *b, int s, int r, int c)
{
    int i = uxl_find(b, s, r, c);
    return (i >= 0 && nth_c(b, s, i)->fml[0]) ? nth_c(b, s, i)->fml : 0;
}
int uxl_count(const uxl_book *b, int s)
{ return (b && s >= 0 && s < b->nsheet) ? b->sheet[s].ncell : 0; }
int uxl_at(const uxl_book *b, int s, int i, int *r, int *c, uxl_val *out)
{
    const uxl_cell *p;
    if (!b || s < 0 || s >= b->nsheet || i < 0 || i >= b->sheet[s].ncell) return 0;
    p = nth_c(b, s, i);
    if (r) *r = p->row;
    if (c) *c = p->col;
    if (out) *out = p->v;
    return 1;
}
int uxl_fmt(const uxl_book *b, int s, int r, int c)
{
    int i = uxl_find(b, s, r, c);
    return i >= 0 ? nth_c(b, s, i)->fmt : UXL_FMT_GENERAL;
}
void uxl_set_fmt(uxl_book *b, int s, int r, int c, int fmt)
{
    uxl_cell *p = uxl_slot(b, s, r, c, 1);
    if (p) { p->fmt = (short)fmt; b->rev++; }
}

/* ---- defined names ---------------------------------------------------------- */
int uxl_name_set(uxl_book *b, const char *name, int s, int r, int c)
{
    int i;
    if (!b || !name) return 0;
    for (i = 0; i < b->nname; i++)
        if (x_streq(b->name[i].name, name)) {
            b->name[i].s = s; b->name[i].r = r; b->name[i].c = c;
            return 1;
        }
    if (b->nname >= UXL_MAXNAME) return 0;
    x_cpy(b->name[b->nname].name, name, 32);
    b->name[b->nname].s = s;
    b->name[b->nname].r = r;
    b->name[b->nname].c = c;
    b->nname++;
    return 1;
}
int uxl_name_find(const uxl_book *b, const char *name, int *s, int *r, int *c)
{
    int i;
    if (!b || !name) return 0;
    for (i = 0; i < b->nname; i++)
        if (x_streq(b->name[i].name, name)) {
            if (s) *s = b->name[i].s;
            if (r) *r = b->name[i].r;
            if (c) *c = b->name[i].c;
            return 1;
        }
    return 0;
}

/* ---- A1 --------------------------------------------------------------------- */
int uxl_a1_parse(const char *s, int *row, int *col, int *abs_r, int *abs_c)
{
    int c = 0, r = 0, ac = 0, ar = 0, got = 0;
    if (!s) return 0;
    if (*s == '$') { ac = 1; s++; }
    while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
        int d = (*s >= 'a') ? *s - 'a' : *s - 'A';
        c = c * 26 + d + 1;
        s++; got = 1;
    }
    if (!got) return 0;
    if (*s == '$') { ar = 1; s++; }
    got = 0;
    while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; got = 1; }
    if (!got || *s) return 0;
    c--; r--;
    if (c < 0 || c >= UXL_COLS || r < 0 || r >= UXL_ROWS) return 0;
    if (row) *row = r;
    if (col) *col = c;
    if (abs_r) *abs_r = ar;
    if (abs_c) *abs_c = ac;
    return 1;
}
int uxl_a1_write(int row, int col, int abs_r, int abs_c, char *out, int cap)
{
    char letters[4];
    int n = 0, k = 0, v = col + 1, i;
    if (!out || cap < 8) return 0;
    if (abs_c) out[n++] = '$';
    while (v > 0 && k < 4) { letters[k++] = (char)('A' + (v - 1) % 26); v = (v - 1) / 26; }
    for (i = k - 1; i >= 0; i--) out[n++] = letters[i];
    if (abs_r) out[n++] = '$';
    {
        char d[8];
        int m = 0, q = row + 1;
        do { d[m++] = (char)('0' + q % 10); q /= 10; } while (q && m < 8);
        while (m) out[n++] = d[--m];
    }
    out[n] = 0;
    return n;
}

/* ---- display ---------------------------------------------------------------- */
int uxl_text(const uxl_book *b, int s, int r, int c, char *out, int cap)
{
    uxl_val v;
    int i = uxl_find(b, s, r, c), fmt;
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (i < 0) return 0;
    v = nth_c(b, s, i)->v;
    fmt = nth_c(b, s, i)->fmt;
    switch (v.kind) {
    case UXL_NUM:
        return uxl_format(v.num, uxl_fmt_code(fmt), out, cap);
    case UXL_BOOL: {
        const char *t = v.num ? "TRUE" : "FALSE";
        int n = 0;
        while (t[n] && n < cap - 1) { out[n] = t[n]; n++; }
        out[n] = 0;
        return n;
    }
    case UXL_ERR: {
        const char *t = uxl_err_text(v.err);
        int n = 0;
        while (t[n] && n < cap - 1) { out[n] = t[n]; n++; }
        out[n] = 0;
        return n;
    }
    case UXL_STR: {
        const char *t = uxl_pool(b, v.str);
        return uxl_format_text(t, uxl_fmt_code(fmt), out, cap);
    }
    default: return 0;
    }
}
