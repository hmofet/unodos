/* ===========================================================================
 * uow_doc.c - UnoWord's document model (OFFICE97-PLAN §5 phase 7).
 *
 * A piece table over two buffers, two run lists for formatting, a style table
 * with based-on chains, and an undo stack of inverse commands.  See uoword.h
 * for why it is a piece table and not the Editor's parallel arrays.
 *
 * THE INVARIANT EVERYTHING ELSE RESTS ON: the char-run list and the para-run
 * list each cover exactly [0, len).  Every mutation adjusts both, and
 * runs_check() in the gate asserts it after each one.  A model whose runs
 * drift from its text produces formatting that slides along the document as
 * you type, which is the bug this shape exists to make impossible.
 * ======================================================================== */
#include "uoword.h"

static void w_memcpy(void *d, const void *s, long n)
{ char *a = (char *)d; const char *b = (const char *)s; long i;
  for (i = 0; i < n; i++) a[i] = b[i]; }
static void w_memset(void *d, int c, long n)
{ char *a = (char *)d; long i; for (i = 0; i < n; i++) a[i] = (char)c; }
static int w_strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
static void w_strcpy(char *d, const char *s, int cap)
{ int i = 0; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }

/* ---- the undo record -------------------------------------------------------
 * An inverse command, not a snapshot: offsets into the add buffer plus the
 * run slices the edit overwrote.  A 200-page document costs the same to undo
 * as a one-line one. */
enum { UND_INSERT = 1, UND_DELETE, UND_CHP, UND_PAP };

typedef struct {
    int  kind;
    long cp, n;
    long saved_off;                 /* deleted text, parked in `add`         */
    uow_crun crun[16]; int ncrun;   /* the char runs the edit replaced       */
    uow_prun prun[16]; int nprun;
    const char *name;
} uow_undo_rec;

struct uow_doc {
    char  orig[UOW_ORIGCAP];
    char  add[UOW_ADDCAP];
    long  addlen;

    uow_piece piece[UOW_MAXPIECE];
    int   npiece;
    long  cum[UOW_MAXPIECE + 1];    /* cumulative cp at each piece start     */
    long  len;

    uow_crun crun[UOW_MAXRUN]; int ncrun;
    uow_prun prun[UOW_MAXRUN]; int nprun;

    uow_style style[UOW_MAXSTYLE];
    int nstyle;
    uow_sect sect;

    uow_undo_rec undo[UOW_MAXUNDO];
    int nundo, undo_at;             /* undo_at = how many are "done"         */

    unsigned rev;
    int in_undo;                    /* suppresses recording while undoing    */
};

static uow_doc g_doc;               /* one document; MDI comes with the app  */

/* ---- pieces ---------------------------------------------------------------- */
static void recum(uow_doc *d)
{
    int i;
    long at = 0;
    for (i = 0; i < d->npiece; i++) { d->cum[i] = at; at += d->piece[i].len; }
    d->cum[d->npiece] = at;
    d->len = at;
}
static int piece_of(const uow_doc *d, long cp, long *off)
{
    int lo = 0, hi = d->npiece - 1, mid;
    if (d->npiece <= 0) { *off = 0; return -1; }
    if (cp >= d->len) { *off = d->piece[d->npiece-1].len; return d->npiece - 1; }
    while (lo < hi) {
        mid = (lo + hi + 1) / 2;
        if (d->cum[mid] <= cp) lo = mid; else hi = mid - 1;
    }
    *off = cp - d->cum[lo];
    return lo;
}
/* Split the piece containing `cp` so a boundary falls exactly there. */
static int split_at(uow_doc *d, long cp)
{
    long off;
    int i;
    if (cp <= 0) return 0;
    if (cp >= d->len) return d->npiece;
    i = piece_of(d, cp, &off);
    if (i < 0) return 0;
    if (off == 0) return i;
    if (d->npiece >= UOW_MAXPIECE) return -1;
    { int j;
      for (j = d->npiece; j > i + 1; j--) d->piece[j] = d->piece[j-1]; }
    d->piece[i + 1].buf = d->piece[i].buf;
    d->piece[i + 1].off = d->piece[i].off + off;
    d->piece[i + 1].len = d->piece[i].len - off;
    d->piece[i].len = off;
    d->npiece++;
    recum(d);
    return i + 1;
}

long uow_len(const uow_doc *d) { return d ? d->len : 0; }
unsigned uow_revision(const uow_doc *d) { return d ? d->rev : 0; }

long uow_read(const uow_doc *d, long cp, long n, char *out)
{
    long got = 0, off;
    int i;
    if (!d || !out || n <= 0 || cp < 0 || cp >= d->len) return 0;
    if (cp + n > d->len) n = d->len - cp;
    i = piece_of(d, cp, &off);
    while (i >= 0 && i < d->npiece && got < n) {
        const uow_piece *p = &d->piece[i];
        const char *src = (p->buf == 0) ? d->orig : d->add;
        long take = p->len - off;
        if (take > n - got) take = n - got;
        w_memcpy(out + got, src + p->off + off, take);
        got += take;
        off = 0;
        i++;
    }
    return got;
}
int uow_char_at(const uow_doc *d, long cp)
{
    char c;
    if (uow_read(d, cp, 1, &c) != 1) return -1;
    return (unsigned char)c;
}

/* ---- run lists -------------------------------------------------------------
 * Both cover [0, len).  These two helpers are where every edit's bookkeeping
 * actually happens, and they are deliberately identical in shape. */
#define RUNOPS(TYPE, FIELD, PROP)                                             \
static int uow_##FIELD##_index(const uow_doc *d, long cp, long *off)                \
{                                                                             \
    int i; long at = 0;                                                       \
    for (i = 0; i < d->n##FIELD; i++) {                                       \
        if (cp < at + d->FIELD[i].len) { *off = cp - at; return i; }          \
        at += d->FIELD[i].len;                                                \
    }                                                                         \
    *off = 0;                                                                 \
    return d->n##FIELD - 1;                                                   \
}                                                                             \
static int uow_##FIELD##_split(uow_doc *d, long cp)                                 \
{                                                                             \
    long off; int i, j;                                                       \
    if (cp <= 0) return 0;                                                    \
    if (cp >= d->len) return d->n##FIELD;                                     \
    i = uow_##FIELD##_index(d, cp, &off);                                           \
    if (i < 0) return 0;                                                      \
    if (off == 0) return i;                                                   \
    if (d->n##FIELD >= UOW_MAXRUN) return -1;                                 \
    for (j = d->n##FIELD; j > i + 1; j--) d->FIELD[j] = d->FIELD[j-1];        \
    d->FIELD[i+1] = d->FIELD[i];                                              \
    d->FIELD[i+1].len = d->FIELD[i].len - off;                                \
    d->FIELD[i].len = off;                                                    \
    d->n##FIELD++;                                                            \
    return i + 1;                                                             \
}                                                                             \
static void uow_##FIELD##_merge(uow_doc *d)                                         \
{                                                                             \
    int i = 0;                                                                \
    while (i + 1 < d->n##FIELD) {                                             \
        const char *a = (const char *)&d->FIELD[i].PROP;                      \
        const char *b = (const char *)&d->FIELD[i+1].PROP;                    \
        int k, same = 1;                                                      \
        for (k = 0; k < (int)sizeof d->FIELD[i].PROP; k++)                    \
            if (a[k] != b[k]) { same = 0; break; }                            \
        if (same || d->FIELD[i].len == 0) {                                   \
            int j;                                                            \
            d->FIELD[i].len += d->FIELD[i+1].len;                             \
            for (j = i + 1; j + 1 < d->n##FIELD; j++)                         \
                d->FIELD[j] = d->FIELD[j+1];                                  \
            d->n##FIELD--;                                                    \
        } else i++;                                                           \
    }                                                                         \
}
RUNOPS(uow_crun, crun, chp)
RUNOPS(uow_prun, prun, pap)

/* ---- undo ------------------------------------------------------------------ */
static void push_undo(uow_doc *d, int kind, long cp, long n, long saved,
                      const char *name)
{
    uow_undo_rec *r;
    if (d->in_undo) return;
    if (d->undo_at >= UOW_MAXUNDO) {          /* drop the oldest             */
        int i;
        for (i = 0; i + 1 < UOW_MAXUNDO; i++) d->undo[i] = d->undo[i+1];
        d->undo_at = UOW_MAXUNDO - 1;
    }
    r = &d->undo[d->undo_at];
    w_memset(r, 0, (long)sizeof *r);
    r->kind = kind; r->cp = cp; r->n = n; r->saved_off = saved; r->name = name;
    d->undo_at++;
    d->nundo = d->undo_at;                    /* a new edit kills the redos  */
}
/* Save the run slices an edit is about to overwrite, so undo can put them
 * back verbatim rather than guessing. */
static void save_runs(uow_doc *d, uow_undo_rec *r, long cp, long n)
{
    long at = 0;
    int i;
    r->ncrun = r->nprun = 0;
    for (i = 0; i < d->ncrun && r->ncrun < 16; i++) {
        long a = at, b = at + d->crun[i].len;
        at = b;
        if (b <= cp || a >= cp + n) continue;
        r->crun[r->ncrun++] = d->crun[i];
    }
    at = 0;
    for (i = 0; i < d->nprun && r->nprun < 16; i++) {
        long a = at, b = at + d->prun[i].len;
        at = b;
        if (b <= cp || a >= cp + n) continue;
        r->prun[r->nprun++] = d->prun[i];
    }
}

/* ---- construction ---------------------------------------------------------- */
static void seed_styles(uow_doc *d)
{
    static const struct { const char *name; int based; int size; int bold;
                          int align; int before; int after; } kB[] = {
        { "Normal",     -1, 20, 0, UOW_AL_LEFT,   0,   0 },
        { "Heading 1",   0, 28, 1, UOW_AL_LEFT, 240, 120 },
        { "Heading 2",   0, 24, 1, UOW_AL_LEFT, 240, 120 },
        { "Heading 3",   0, 22, 1, UOW_AL_LEFT, 240, 120 },
        { "Body Text",   0, 20, 0, UOW_AL_LEFT,   0, 120 },
        { "Title",       0, 32, 1, UOW_AL_CENTER, 0, 240 },
        { "List Bullet", 0, 20, 0, UOW_AL_LEFT,   0,   0 },
        { "Header",      0, 20, 0, UOW_AL_LEFT,   0,   0 },
        { "Footer",      0, 20, 0, UOW_AL_LEFT,   0,   0 },
        { "Caption",     0, 18, 1, UOW_AL_LEFT, 120, 120 }
    };
    int i;
    for (i = 0; i < (int)(sizeof kB / sizeof kB[0]); i++) {
        uow_style *s = &d->style[i];
        w_memset(s, 0, (long)sizeof *s);
        w_strcpy(s->name, kB[i].name, UOW_STYLENAME);
        s->based_on = kB[i].based;
        s->next = (i >= UOW_STY_H1 && i <= UOW_STY_H3) ? UOW_STY_BODY : i;
        s->chp.size = (unsigned short)kB[i].size;
        s->chp.bold = (unsigned char)kB[i].bold;
        s->pap.align = (unsigned char)kB[i].align;
        s->pap.before = (short)kB[i].before;
        s->pap.after  = (short)kB[i].after;
        s->pap.style  = (unsigned short)i;
        s->pap.widow  = 1;
        s->has_chp = s->has_pap = 1;
        s->used = 1;
    }
    d->nstyle = (int)(sizeof kB / sizeof kB[0]);
}

uow_doc *uow_new(void)
{
    uow_doc *d = &g_doc;
    w_memset(d, 0, (long)sizeof *d);
    seed_styles(d);

    /* An empty Word document is not empty: it holds one paragraph mark. */
    d->orig[0] = '\r';
    d->piece[0].buf = 0; d->piece[0].off = 0; d->piece[0].len = 1;
    d->npiece = 1;
    recum(d);

    /* THE DIRECT RUNS START EMPTY.  A run holds EXCEPTIONS, so seeding it
     * with Normal's own values would give every character an explicit size
     * that beats any style applied later - the style would resolve, then
     * lose. (It did, on the first run of the gate: applying Heading 1 left
     * the text at 10pt.)  Only the paragraph's style id is seeded. */
    d->crun[0].len = 1;
    w_memset(&d->crun[0].chp, 0, (long)sizeof d->crun[0].chp);
    d->ncrun = 1;
    d->prun[0].len = 1;
    w_memset(&d->prun[0].pap, 0, (long)sizeof d->prun[0].pap);
    d->prun[0].pap.style = UOW_STY_NORMAL;
    d->nprun = 1;

    d->sect.page_w = 12240; d->sect.page_h = 15840;      /* US Letter        */
    d->sect.margin_l = d->sect.margin_r = 1800;          /* 1.25in, Word 97  */
    d->sect.margin_t = d->sect.margin_b = 1440;          /* 1in              */
    d->sect.header_from = d->sect.footer_from = 720;
    d->sect.columns = 1;
    d->sect.column_gap = 720;
    d->rev = 1;
    return d;
}
void uow_free(uow_doc *d) { (void)d; }

uow_style *uow_style_at(uow_doc *d, int i)
{ return (d && i >= 0 && i < UOW_MAXSTYLE) ? &d->style[i] : 0; }
int uow_style_find(const uow_doc *d, const char *name)
{
    int i;
    if (!d || !name) return -1;
    for (i = 0; i < d->nstyle; i++)
        if (d->style[i].used && w_strcmp(d->style[i].name, name) == 0) return i;
    return -1;
}
int uow_style_add(uow_doc *d, const char *name, int based_on)
{
    uow_style *s;
    if (!d || d->nstyle >= UOW_MAXSTYLE) return -1;
    s = &d->style[d->nstyle];
    w_memset(s, 0, (long)sizeof *s);
    w_strcpy(s->name, name, UOW_STYLENAME);
    s->based_on = based_on;
    s->next = d->nstyle;
    s->used = 1;
    return d->nstyle++;
}
uow_sect *uow_section(uow_doc *d) { return d ? &d->sect : 0; }

/* ---- style resolution ------------------------------------------------------
 * Root-first down the based-on chain, then the direct run on top.  Get the
 * order backwards and direct formatting silently loses to the style it was
 * meant to override - the same trap unodoc's STSH reader documents. */
static void resolve_chain(const uow_doc *d, int sty, uow_chp *c, uow_pap *p)
{
    int chain[8], n = 0, i;
    while (sty >= 0 && sty < d->nstyle && n < 8) {
        chain[n++] = sty;
        sty = d->style[sty].based_on;
    }
    for (i = n - 1; i >= 0; i--) {
        const uow_style *s = &d->style[chain[i]];
        if (c && s->has_chp) *c = s->chp;
        if (p && s->has_pap) {
            unsigned short keep = p->style;
            *p = s->pap;
            p->style = keep;
        }
    }
}
void uow_chp_at(const uow_doc *d, long cp, uow_chp *out)
{
    long off;
    int i;
    uow_pap pp;
    if (!d || !out) return;
    w_memset(out, 0, (long)sizeof *out);
    uow_pap_at(d, cp, &pp);
    resolve_chain(d, pp.style, out, 0);
    i = uow_crun_index(d, cp, &off);
    if (i >= 0 && i < d->ncrun) {
        const uow_chp *r = &d->crun[i].chp;
        /* a run's non-zero fields are its direct exceptions */
        if (r->size)      out->size = r->size;
        if (r->face)      out->face = r->face;
        if (r->bold)      out->bold = r->bold;
        if (r->italic)    out->italic = r->italic;
        if (r->underline) out->underline = r->underline;
        if (r->strike)    out->strike = r->strike;
        if (r->caps)      out->caps = r->caps;
        if (r->smallcaps) out->smallcaps = r->smallcaps;
        if (r->super)     out->super = r->super;
        if (r->sub)       out->sub = r->sub;
        if (r->color)     out->color = r->color;
        if (r->highlight) out->highlight = r->highlight;
    }
    if (!out->size) out->size = 20;
}
void uow_pap_at(const uow_doc *d, long cp, uow_pap *out)
{
    long off;
    int i;
    if (!d || !out) return;
    w_memset(out, 0, (long)sizeof *out);
    i = uow_prun_index(d, cp, &off);
    if (i >= 0 && i < d->nprun) *out = d->prun[i].pap;
    {   /* the style supplies whatever the run leaves at zero */
        uow_pap base;
        w_memset(&base, 0, (long)sizeof base);
        base.style = out->style;
        resolve_chain(d, out->style, 0, &base);
        if (!out->align)    out->align = base.align;
        if (!out->before)   out->before = base.before;
        if (!out->after)    out->after = base.after;
        if (!out->linerule) out->linerule = base.linerule;
        if (!out->widow)    out->widow = base.widow;
    }
}

/* ---- paragraphs ------------------------------------------------------------ */
long uow_para_start(const uow_doc *d, long cp)
{
    long i;
    if (!d) return 0;
    if (cp > d->len) cp = d->len;
    for (i = cp - 1; i >= 0; i--)
        if (uow_char_at(d, i) == '\r') return i + 1;
    return 0;
}
long uow_para_end(const uow_doc *d, long cp)
{
    long i;
    if (!d) return 0;
    for (i = cp; i < d->len; i++)
        if (uow_char_at(d, i) == '\r') return i;
    return d->len;
}
int uow_para_count(const uow_doc *d)
{
    long i;
    int n = 0;
    if (!d) return 0;
    for (i = 0; i < d->len; i++) if (uow_char_at(d, i) == '\r') n++;
    return n ? n : 1;
}

/* ---- mutation -------------------------------------------------------------- */
static int do_insert(uow_doc *d, long cp, const char *s, long n, int record)
{
    int pi, i;
    long saved;
    if (!d || n <= 0 || cp < 0 || cp > d->len) return 0;
    if (d->addlen + n > UOW_ADDCAP) return 0;
    if (d->npiece + 2 > UOW_MAXPIECE) return 0;

    saved = d->addlen;
    w_memcpy(d->add + d->addlen, s, n);
    d->addlen += n;

    pi = split_at(d, cp);
    if (pi < 0) return 0;
    for (i = d->npiece; i > pi; i--) d->piece[i] = d->piece[i-1];
    d->piece[pi].buf = 1;
    d->piece[pi].off = saved;
    d->piece[pi].len = n;
    d->npiece++;
    recum(d);

    /* the inserted text takes the formatting of what it was typed into */
    {   long off;
        int ci = uow_crun_index(d, cp > 0 ? cp - 1 : 0, &off);
        int qi = uow_prun_index(d, cp > 0 ? cp - 1 : 0, &off);
        if (ci >= 0 && ci < d->ncrun) d->crun[ci].len += n;
        if (qi >= 0 && qi < d->nprun) d->prun[qi].len += n;
    }
    /* a new paragraph mark splits the paragraph run so the halves can differ */
    {   long k;
        for (k = 0; k < n; k++)
            if (s[k] == '\r') { uow_prun_split(d, cp + k + 1); break; }
    }
    if (record) push_undo(d, UND_INSERT, cp, n, saved, "Typing");
    d->rev++;
    return 1;
}

static int do_delete(uow_doc *d, long cp, long n, int record)
{
    long saved = d->addlen;
    int i, a, b;
    if (!d || n <= 0 || cp < 0 || cp + n > d->len) return 0;
    if (d->len - n < 1) return 0;              /* the last mark must stay    */
    if (d->addlen + n > UOW_ADDCAP) return 0;

    if (record) {
        uow_undo_rec *r;
        char tmp[512];
        long got = 0;
        while (got < n) {
            long take = n - got;
            if (take > (long)sizeof tmp) take = (long)sizeof tmp;
            uow_read(d, cp + got, take, tmp);
            w_memcpy(d->add + d->addlen + got, tmp, take);
            got += take;
        }
        d->addlen += n;
        push_undo(d, UND_DELETE, cp, n, saved, "Delete");
        r = &d->undo[d->undo_at - 1];
        save_runs(d, r, cp, n);
    }

    a = split_at(d, cp);
    if (a < 0) return 0;
    b = split_at(d, cp + n);
    if (b < 0) return 0;
    for (i = a; i + (b - a) < d->npiece; i++) d->piece[i] = d->piece[i + (b - a)];
    d->npiece -= (b - a);
    recum(d);

    /* trim both run lists by the same span */
    {   int lists;
        for (lists = 0; lists < 2; lists++) {
            long at = 0, left = n;
            int k = 0;
            while (k < (lists ? d->nprun : d->ncrun) && left > 0) {
                long *plen = lists ? &d->prun[k].len : &d->crun[k].len;
                long s0 = at, e0 = at + *plen;
                at = e0;
                if (e0 <= cp) { k++; continue; }
                if (s0 >= cp + n) break;
                {   long lo = (s0 > cp) ? s0 : cp;
                    long hi = (e0 < cp + n) ? e0 : cp + n;
                    long cut = hi - lo;
                    *plen -= cut;
                    left -= cut;
                    at -= cut;
                }
                if (*plen == 0) {
                    int j, *pn = lists ? &d->nprun : &d->ncrun;
                    for (j = k; j + 1 < *pn; j++) {
                        if (lists) d->prun[j] = d->prun[j+1];
                        else       d->crun[j] = d->crun[j+1];
                    }
                    (*pn)--;
                } else k++;
            }
        }
    }
    if (d->ncrun == 0) { d->crun[0].len = d->len; d->ncrun = 1; }
    if (d->nprun == 0) { d->prun[0].len = d->len; d->nprun = 1; }
    uow_crun_merge(d);
    uow_prun_merge(d);
    d->rev++;
    return 1;
}

int uow_insert(uow_doc *d, long cp, const char *s, long n)
{ return do_insert(d, cp, s, n, 1); }
int uow_delete(uow_doc *d, long cp, long n)
{ return do_delete(d, cp, n, 1); }

static int apply_chp(uow_doc *d, long cp, long n, const uow_chp *c, int record)
{
    int a, b, i;
    long at;
    if (!d || n <= 0 || cp < 0 || cp + n > d->len) return 0;
    if (record) {
        push_undo(d, UND_CHP, cp, n, 0, "Formatting");
        save_runs(d, &d->undo[d->undo_at - 1], cp, n);
    }
    a = uow_crun_split(d, cp);
    if (a < 0) return 0;
    b = uow_crun_split(d, cp + n);
    if (b < 0) return 0;
    at = 0;
    for (i = 0; i < d->ncrun; i++) {
        long s0 = at;
        at += d->crun[i].len;
        if (s0 >= cp && s0 < cp + n) d->crun[i].chp = *c;
    }
    uow_crun_merge(d);
    d->rev++;
    return 1;
}
int uow_format(uow_doc *d, long cp, long n, const uow_chp *c)
{ return apply_chp(d, cp, n, c, 1); }

static int apply_pap(uow_doc *d, long cp, long n, const uow_pap *p,
                     int style_only, int style, int record)
{
    int a, b, i;
    long at, s = uow_para_start(d, cp), e = uow_para_end(d, cp + (n ? n - 1 : 0));
    if (!d) return 0;
    if (e < d->len) e++;                       /* include the mark           */
    if (record) {
        push_undo(d, UND_PAP, s, e - s, 0,
                  style_only ? "Apply Style" : "Paragraph Formatting");
        save_runs(d, &d->undo[d->undo_at - 1], s, e - s);
    }
    a = uow_prun_split(d, s);
    if (a < 0) return 0;
    b = uow_prun_split(d, e);
    if (b < 0) return 0;
    at = 0;
    for (i = 0; i < d->nprun; i++) {
        long s0 = at;
        at += d->prun[i].len;
        if (s0 >= s && s0 < e) {
            if (style_only) d->prun[i].pap.style = (unsigned short)style;
            else            d->prun[i].pap = *p;
        }
    }
    uow_prun_merge(d);
    d->rev++;
    return 1;
}
int uow_format_para(uow_doc *d, long cp, long n, const uow_pap *p)
{ return apply_pap(d, cp, n, p, 0, 0, 1); }
int uow_set_style(uow_doc *d, long cp, long n, int style)
{ return apply_pap(d, cp, n, 0, 1, style, 1); }

/* ---- undo / redo ----------------------------------------------------------- */
static void restore_runs(uow_doc *d, const uow_undo_rec *r)
{
    int a, b, i, j;
    if (r->ncrun) {
        a = uow_crun_split(d, r->cp);
        b = uow_crun_split(d, r->cp + r->n);
        if (a >= 0 && b >= a) {
            for (i = a, j = 0; i < b && j < r->ncrun; i++, j++)
                d->crun[i].chp = r->crun[j].chp;
        }
        uow_crun_merge(d);
    }
    if (r->nprun) {
        a = uow_prun_split(d, r->cp);
        b = uow_prun_split(d, r->cp + r->n);
        if (a >= 0 && b >= a) {
            for (i = a, j = 0; i < b && j < r->nprun; i++, j++)
                d->prun[i].pap = r->prun[j].pap;
        }
        uow_prun_merge(d);
    }
}

int uow_can_undo(const uow_doc *d) { return d && d->undo_at > 0; }
int uow_can_redo(const uow_doc *d) { return d && d->undo_at < d->nundo; }
const char *uow_undo_name(const uow_doc *d)
{ return (d && d->undo_at > 0) ? d->undo[d->undo_at - 1].name : ""; }

int uow_undo(uow_doc *d)
{
    uow_undo_rec *r;
    if (!uow_can_undo(d)) return 0;
    r = &d->undo[d->undo_at - 1];
    d->in_undo = 1;
    switch (r->kind) {
    case UND_INSERT:
        do_delete(d, r->cp, r->n, 0);
        break;
    case UND_DELETE: {
        long got = 0;
        while (got < r->n) {
            long take = r->n - got;
            do_insert(d, r->cp + got, d->add + r->saved_off + got, take, 0);
            got += take;
        }
        restore_runs(d, r);
        break;
    }
    case UND_CHP:
    case UND_PAP:
        restore_runs(d, r);
        break;
    default: break;
    }
    d->in_undo = 0;
    d->undo_at--;
    d->rev++;
    return 1;
}

int uow_redo(uow_doc *d)
{
    uow_undo_rec *r;
    if (!uow_can_redo(d)) return 0;
    r = &d->undo[d->undo_at];
    d->in_undo = 1;
    switch (r->kind) {
    case UND_INSERT:
        /* the text is still parked where the original insert put it */
        do_insert(d, r->cp, d->add + r->saved_off, r->n, 0);
        break;
    case UND_DELETE:
        do_delete(d, r->cp, r->n, 0);
        break;
    default: break;                   /* format redo needs the new run kept */
    }
    d->in_undo = 0;
    d->undo_at++;
    d->rev++;
    return 1;
}
