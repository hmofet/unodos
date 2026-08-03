/* ===========================================================================
 * uos_model.c - the presentation store: slides, shapes, text bodies, masters,
 * colour schemes and the 24 AutoLayouts.  (OFFICE97-PLAN §7 phase 11.)
 *
 * THREE POOLS, SHARED BY THE WHOLE PRESENTATION - shapes, paragraphs and
 * characters - and a slide holds nothing but indices into the first.  Giving
 * each slide its own arrays multiplies the worst case by UOS_MAXSLIDE whether
 * the slides exist or not, which is exactly the mistake UnoCalc made and paid
 * for at link time with a 104 MB module against a 4 MB arena.
 *
 * THE POOL INVARIANT, which compaction depends on:
 *
 *   A shape's paragraph run is CONTIGUOUS, its text is CONTIGUOUS, and
 *   sorting shapes by para_at also sorts them by their text offset.
 *
 * Both pools are bump-allocated together in uos_text_set, so that holds by
 * construction - and uos_para_add, which appends to an existing body, re-homes
 * the run's TEXT as well as its paragraphs to keep it holding.  One sort then
 * compacts both pools in a single pass.  Break the invariant and compaction
 * silently shuffles one body's text into another's.
 * ======================================================================== */
#include "uoshow.h"

/* ---- tiny local helpers (freestanding: no libc) ---------------------------- */
static void m_zero(void *d, long n)
{ char *p = (char *)d; long i; for (i = 0; i < n; i++) p[i] = 0; }
static void m_str(char *d, const char *s, int cap)
{ int i = 0; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int  m_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

/* ---- the AutoLayout tables ---------------------------------------------------
 * Frames in slide points on the 720 x 540 plane, with 0.75in margins - the
 * geometry PowerPoint's own layouts use.  A layout is DATA, so "apply a
 * layout" is "re-frame the placeholders and add the ones that are missing",
 * which is what PowerPoint does to a slide that already has content. */
#define T_X  54
#define T_Y  54
#define T_W 612
#define T_H  86
#define B_X  54
#define B_Y 175
#define B_W 612
#define B_H 310
#define L_X  54                    /* left half  */
#define R_X 368                    /* right half */
#define H_W 298
#define TOP_H 148                  /* upper band of a split body */
#define BOT_Y 331

typedef struct { unsigned char ph; short x, y, w, h; } uos_lay_ph;
typedef struct { const uos_lay_ph *ph; int n; } uos_layout;

static const uos_lay_ph kL_title[] = {
    { UOS_PH_CTRTITLE, 54, 170, 612, 110 }, { UOS_PH_SUBTITLE, 108, 300, 504, 120 } };
static const uos_lay_ph kL_bullets[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H }, { UOS_PH_BODY, B_X, B_Y, B_W, B_H } };
static const uos_lay_ph kL_2col[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H },
    { UOS_PH_BODY,  L_X, B_Y, H_W, B_H }, { UOS_PH_BODY2, R_X, B_Y, H_W, B_H } };
static const uos_lay_ph kL_obj[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H }, { UOS_PH_OBJECT, B_X, B_Y, B_W, B_H } };
static const uos_lay_ph kL_text_obj[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H },
    { UOS_PH_BODY,  L_X, B_Y, H_W, B_H }, { UOS_PH_OBJECT, R_X, B_Y, H_W, B_H } };
static const uos_lay_ph kL_obj_text[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H },
    { UOS_PH_OBJECT, L_X, B_Y, H_W, B_H }, { UOS_PH_BODY, R_X, B_Y, H_W, B_H } };
static const uos_lay_ph kL_title_only[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H } };
static const uos_lay_ph kL_obj_over[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H },
    { UOS_PH_OBJECT, B_X, B_Y, B_W, TOP_H }, { UOS_PH_BODY, B_X, BOT_Y, B_W, TOP_H } };
static const uos_lay_ph kL_text_over[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H },
    { UOS_PH_BODY, B_X, B_Y, B_W, TOP_H }, { UOS_PH_OBJECT, B_X, BOT_Y, B_W, TOP_H } };
static const uos_lay_ph kL_4obj[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H },
    { UOS_PH_OBJECT, L_X, B_Y,   H_W, TOP_H }, { UOS_PH_OBJECT, R_X, B_Y,   H_W, TOP_H },
    { UOS_PH_OBJECT, L_X, BOT_Y, H_W, TOP_H }, { UOS_PH_OBJECT, R_X, BOT_Y, H_W, TOP_H } };
static const uos_lay_ph kL_2obj_text[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H },
    { UOS_PH_OBJECT, L_X, B_Y, H_W, TOP_H }, { UOS_PH_OBJECT, L_X, BOT_Y, H_W, TOP_H },
    { UOS_PH_BODY,   R_X, B_Y, H_W, B_H } };
static const uos_lay_ph kL_text_2obj[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H },
    { UOS_PH_BODY,   L_X, B_Y, H_W, B_H },
    { UOS_PH_OBJECT, R_X, B_Y, H_W, TOP_H }, { UOS_PH_OBJECT, R_X, BOT_Y, H_W, TOP_H } };
static const uos_lay_ph kL_2col_obj[] = {
    { UOS_PH_TITLE, T_X, T_Y, T_W, T_H },
    { UOS_PH_BODY,  L_X, B_Y, H_W, TOP_H }, { UOS_PH_BODY2, R_X, B_Y, H_W, TOP_H },
    { UOS_PH_OBJECT, B_X, BOT_Y, B_W, TOP_H } };

#define LAY(t) { t, (int)(sizeof t / sizeof t[0]) }
static const uos_layout kLayouts[UOS_AL_COUNT] = {
    LAY(kL_title),      LAY(kL_bullets),  LAY(kL_2col),      LAY(kL_obj),
    LAY(kL_text_obj),   LAY(kL_obj_text), LAY(kL_obj),       LAY(kL_obj),
    LAY(kL_text_obj),   LAY(kL_obj_text), LAY(kL_title_only), { 0, 0 },
    LAY(kL_text_obj),   LAY(kL_obj_text), LAY(kL_obj),       LAY(kL_text_obj),
    LAY(kL_obj_over),   LAY(kL_text_over), LAY(kL_4obj),     LAY(kL_2obj_text),
    LAY(kL_2col_obj),   LAY(kL_text_2obj), LAY(kL_obj),      LAY(kL_bullets)
};

/* ---- the presentation --------------------------------------------------------
 * Masters live in the same slide array, past the numbered slides, so every
 * function that walks a slide works on a master unchanged. */
#define SL_TOTAL (UOS_MAXSLIDE + UOS_M_COUNT)

typedef struct {
    unsigned short idx[UOS_MAXPERSLIDE];
    int  n;
    unsigned char layout, hidden, omit_master, has_bg;
    uos_fill bg;
} uos_slide;

struct uos_pres {
    uos_shape shape[UOS_MAXSHAPE];
    int       nshape;                       /* bump high-water mark          */
    unsigned short freeshape[UOS_MAXSHAPE];
    int       nfreeshape;

    uos_para  para[UOS_MAXPARA];
    int       npara;                        /* bump; compacted when full     */
    char      text[UOS_TEXTPOOL];
    int       ntext;

    uos_slide slide[SL_TOTAL];
    int       nslide;
    uos_scheme scheme;
    uos_hf    hf;
    int       dirty;
    unsigned char nextgroup;
};

static uos_pres g_pres;                     /* one deck, like uxl's one book */

/* ---- validity ---------------------------------------------------------------- */
static int sl_ok(const uos_pres *p, int i)
{
    if (!p) return 0;
    if (i >= 0 && i < p->nslide) return 1;
    return i >= UOS_MAXSLIDE && i < SL_TOTAL;   /* a master */
}
static uos_slide *sl(uos_pres *p, int i)
{ return sl_ok(p, i) ? &p->slide[i] : 0; }
static const uos_slide *sl_c(const uos_pres *p, int i)
{ return sl_ok(p, i) ? &p->slide[i] : 0; }

int uos_master(const uos_pres *p, int which)
{ return (p && which >= 0 && which < UOS_M_COUNT) ? UOS_MAXSLIDE + which : -1; }

/* ---- default text formatting -------------------------------------------------
 * PowerPoint 97's own ladder: a title is 44pt, and body levels step
 * 32/28/24/20/20.  Colours are SCHEME ROLES, which is what makes Apply Design
 * a one-table swap instead of a walk over every run. */
static uos_chp default_chp(int ph, int level)
{
    uos_chp c;
    static const unsigned short kBody[UOS_MAXLEVEL] = { 32, 28, 24, 20, 20 };
    m_zero(&c, (long)sizeof c);
    if (level < 0) level = 0;
    if (level >= UOS_MAXLEVEL) level = UOS_MAXLEVEL - 1;
    if (ph == UOS_PH_TITLE || ph == UOS_PH_CTRTITLE) {
        c.size = 44; c.color = UOS_SCHEME_COLOR(UOS_C_TITLE);
    } else if (ph == UOS_PH_SUBTITLE) {
        c.size = 32; c.color = UOS_SCHEME_COLOR(UOS_C_TEXT);
    } else if (ph == UOS_PH_NUMBER || ph == UOS_PH_DATE || ph == UOS_PH_FOOTER) {
        c.size = 12; c.color = UOS_SCHEME_COLOR(UOS_C_TEXT);
    } else {
        c.size = kBody[level]; c.color = UOS_SCHEME_COLOR(UOS_C_TEXT);
    }
    return c;
}
static int ph_is_text(int ph)
{ return ph == UOS_PH_TITLE || ph == UOS_PH_CTRTITLE || ph == UOS_PH_SUBTITLE
      || ph == UOS_PH_BODY  || ph == UOS_PH_BODY2; }

/* ---- shape pool -------------------------------------------------------------- */
static int shape_alloc(uos_pres *p)
{
    if (p->nfreeshape > 0)      return p->freeshape[--p->nfreeshape];
    if (p->nshape < UOS_MAXSHAPE) return p->nshape++;
    return -1;
}
static void shape_release(uos_pres *p, int si)
{ if (p->nfreeshape < UOS_MAXSHAPE) p->freeshape[p->nfreeshape++] = (unsigned short)si; }

uos_shape *uos_shape_at(uos_pres *p, int slide, int z)
{
    const uos_slide *s = sl_c(p, slide);
    if (!s || z < 0 || z >= s->n) return 0;
    return &p->shape[s->idx[z]];
}
const uos_shape *uos_shape_at_c(const uos_pres *p, int slide, int z)
{
    const uos_slide *s = sl_c(p, slide);
    if (!s || z < 0 || z >= s->n) return 0;
    return &p->shape[s->idx[z]];
}
int uos_shapes(const uos_pres *p, int slide)
{ const uos_slide *s = sl_c(p, slide); return s ? s->n : 0; }

int uos_shape_add(uos_pres *p, int slide, int geom, int x, int y, int w, int h)
{
    uos_slide *s = sl(p, slide);
    uos_shape *sh;
    int si;
    if (!s || s->n >= UOS_MAXPERSLIDE) return -1;
    si = shape_alloc(p);
    if (si < 0) return -1;
    sh = &p->shape[si];
    m_zero(sh, (long)sizeof *sh);
    sh->x = (short)x; sh->y = (short)y; sh->w = (short)w; sh->h = (short)h;
    sh->geom = (unsigned char)((geom >= 0 && geom < UOS_G_COUNT) ? geom : UOS_G_RECT);
    sh->adj = 0;                                   /* 0 = the shape default  */
    sh->fill.kind = UOS_F_SOLID;
    sh->fill.c1 = UOS_SCHEME_COLOR(UOS_C_FILL);
    sh->line.kind = UOS_L_SOLID;
    sh->line.width = 1;
    sh->line.c = UOS_SCHEME_COLOR(UOS_C_TEXT);
    s->idx[s->n] = (unsigned short)si;
    p->dirty = 1;
    return s->n++;
}

int uos_shape_delete(uos_pres *p, int slide, int z)
{
    uos_slide *s = sl(p, slide);
    int i;
    if (!s || z < 0 || z >= s->n) return 0;
    shape_release(p, s->idx[z]);
    for (i = z; i + 1 < s->n; i++) s->idx[i] = s->idx[i + 1];
    s->n--;
    p->dirty = 1;
    return 1;
}

static void z_move(uos_slide *s, int from, int to)
{
    unsigned short v = s->idx[from];
    int i;
    if (from == to) return;
    if (from < to) for (i = from; i < to; i++) s->idx[i] = s->idx[i + 1];
    else           for (i = from; i > to; i--) s->idx[i] = s->idx[i - 1];
    s->idx[to] = v;
}
int uos_shape_raise(uos_pres *p, int slide, int z, int to_top)
{
    uos_slide *s = sl(p, slide);
    if (!s || z < 0 || z >= s->n || z == s->n - 1) return 0;
    z_move(s, z, to_top ? s->n - 1 : z + 1);
    p->dirty = 1;
    return 1;
}
int uos_shape_lower(uos_pres *p, int slide, int z, int to_bottom)
{
    uos_slide *s = sl(p, slide);
    if (!s || z < 0 || z >= s->n || z == 0) return 0;
    z_move(s, z, to_bottom ? 0 : z - 1);
    p->dirty = 1;
    return 1;
}

int uos_shape_group(uos_pres *p, int slide, const int *zs, int n)
{
    uos_slide *s = sl(p, slide);
    int i, g;
    if (!s || !zs || n < 2) return 0;
    if (p->nextgroup >= 254) return 0;
    g = ++p->nextgroup;
    for (i = 0; i < n; i++) {
        if (zs[i] < 0 || zs[i] >= s->n) return 0;
        p->shape[s->idx[zs[i]]].group = (unsigned char)g;
    }
    p->dirty = 1;
    return g;
}
int uos_shape_ungroup(uos_pres *p, int slide, int z)
{
    uos_slide *s = sl(p, slide);
    int g, i, n = 0;
    if (!s || z < 0 || z >= s->n) return 0;
    g = p->shape[s->idx[z]].group;
    if (!g) return 0;
    for (i = 0; i < s->n; i++)
        if (p->shape[s->idx[i]].group == g) { p->shape[s->idx[i]].group = 0; n++; }
    p->dirty = 1;
    return n;
}

int uos_placeholder(const uos_pres *p, int slide, int role)
{
    const uos_slide *s = sl_c(p, slide);
    int i;
    if (!s) return -1;
    for (i = 0; i < s->n; i++)
        if (p->shape[s->idx[i]].ph == role) return i;
    return -1;
}

/* ---- the paragraph and character pools --------------------------------------
 * Bump-allocated, and rewriting a body leaks the old run.  When either pool
 * fills, compact: sort the live shapes by para_at (which the pool invariant
 * says also sorts them by text offset) and slide every run down.  The sort is
 * an insertion sort over at most UOS_MAXSHAPE entries and runs only when a
 * pool is full, so its worst case is cheaper than the allocation it saves. */
static void pool_compact(uos_pres *p)
{
    static unsigned short order[UOS_MAXSHAPE];
    int n = 0, i, j, pcur = 0, tcur = 0;

    for (i = 0; i < SL_TOTAL; i++) {
        const uos_slide *s = &p->slide[i];
        if (i >= p->nslide && i < UOS_MAXSLIDE) continue;   /* unused slot   */
        for (j = 0; j < s->n; j++)
            if (p->shape[s->idx[j]].para_n > 0 && n < UOS_MAXSHAPE)
                order[n++] = s->idx[j];
    }
    /* insertion sort by para_at */
    for (i = 1; i < n; i++) {
        unsigned short v = order[i];
        int at = p->shape[v].para_at;
        j = i - 1;
        while (j >= 0 && p->shape[order[j]].para_at > at) { order[j + 1] = order[j]; j--; }
        order[j + 1] = v;
    }
    for (i = 0; i < n; i++) {
        uos_shape *sh = &p->shape[order[i]];
        int k;
        for (k = 0; k < sh->para_n; k++) {
            uos_para *pa = &p->para[sh->para_at + k];
            if (pa->n > 0 && pa->at != tcur) {
                int b;
                for (b = 0; b < pa->n; b++) p->text[tcur + b] = p->text[pa->at + b];
            }
            pa->at = tcur;
            tcur += pa->n;
            p->para[pcur + k] = *pa;
        }
        sh->para_at = pcur;
        pcur += sh->para_n;
    }
    p->npara = pcur;
    p->ntext = tcur;
}

static int pool_room(uos_pres *p, int npara, int nbytes)
{
    if (p->npara + npara <= UOS_MAXPARA && p->ntext + nbytes <= UOS_TEXTPOOL)
        return 1;
    pool_compact(p);
    return p->npara + npara <= UOS_MAXPARA && p->ntext + nbytes <= UOS_TEXTPOOL;
}

/* How many paragraphs and bytes `text` needs (it splits on '\n'). */
static void text_shape_of(const char *t, int *np, int *nb)
{
    int lines = 1, bytes = 0, i;
    for (i = 0; t && t[i]; i++) { if (t[i] == '\n') lines++; else bytes++; }
    *np = lines; *nb = bytes;
}

int uos_text_set(uos_pres *p, int slide, int z, const char *text)
{
    uos_shape *sh = uos_shape_at(p, slide, z);
    int np, nb, i, at, first;
    if (!sh) return 0;
    if (!text || !*text) { sh->para_at = 0; sh->para_n = 0; p->dirty = 1; return 1; }
    text_shape_of(text, &np, &nb);
    if (!pool_room(p, np, nb)) return 0;

    first = p->npara;
    at = 0;
    for (i = 0; i < np; i++) {
        uos_para *pa = &p->para[p->npara++];
        int start = p->ntext;
        m_zero(pa, (long)sizeof *pa);
        while (text[at] && text[at] != '\n') p->text[p->ntext++] = text[at++];
        if (text[at] == '\n') at++;
        pa->at = start;
        pa->n  = p->ntext - start;
        pa->level = 0;
        pa->align = (sh->ph == UOS_PH_CTRTITLE || sh->ph == UOS_PH_SUBTITLE)
                    ? UOS_AL_CENTER : UOS_AL_LEFT;
        pa->bullet = (sh->ph == UOS_PH_BODY || sh->ph == UOS_PH_BODY2) ? 0x95 : 0;
        pa->chp = default_chp(sh->ph, 0);
    }
    sh->para_at = first;
    sh->para_n  = np;
    p->dirty = 1;
    return 1;
}

int uos_text_paras(const uos_pres *p, int slide, int z)
{ const uos_shape *sh = uos_shape_at_c(p, slide, z); return sh ? sh->para_n : 0; }

uos_para *uos_para_at(uos_pres *p, int slide, int z, int i)
{
    uos_shape *sh = uos_shape_at(p, slide, z);
    if (!sh || i < 0 || i >= sh->para_n) return 0;
    return &p->para[sh->para_at + i];
}
const char *uos_para_text(const uos_pres *p, int slide, int z, int i, int *len)
{
    const uos_shape *sh = uos_shape_at_c(p, slide, z);
    const uos_para *pa;
    if (len) *len = 0;
    if (!sh || i < 0 || i >= sh->para_n) return "";
    pa = &p->para[sh->para_at + i];
    if (len) *len = pa->n;
    return &p->text[pa->at];
}

/* Appending re-homes the WHOLE run - paragraphs and their text - to the end of
 * both pools.  That is what keeps the pool invariant (see the file header)
 * true, and it is why compaction can be one sorted pass. */
int uos_para_add(uos_pres *p, int slide, int z, const char *text, int level)
{
    uos_shape *sh = uos_shape_at(p, slide, z);
    int nb, i, oldat, oldn, newat, newtext;
    if (!sh) return 0;
    if (!sh->para_n) {
        if (!uos_text_set(p, slide, z, text ? text : "")) return 0;
        if (level > 0) uos_para_set_level(p, slide, z, 0, level);
        return 1;
    }
    nb = m_len(text);
    { int extra = 0, k;
      for (k = 0; k < sh->para_n; k++) extra += p->para[sh->para_at + k].n;
      if (!pool_room(p, sh->para_n + 1, extra + nb)) return 0; }

    oldat = sh->para_at; oldn = sh->para_n;
    newat = p->npara;
    for (i = 0; i < oldn; i++) {
        uos_para src = p->para[oldat + i];
        uos_para *dst = &p->para[p->npara++];
        int start = p->ntext, b;
        for (b = 0; b < src.n; b++) p->text[p->ntext++] = p->text[src.at + b];
        *dst = src;
        dst->at = start;
    }
    newtext = p->ntext;
    for (i = 0; i < nb; i++) p->text[p->ntext++] = text[i];
    {
        uos_para *pa = &p->para[p->npara++];
        m_zero(pa, (long)sizeof *pa);
        pa->at = newtext; pa->n = nb;
        pa->level = (unsigned char)(level < 0 ? 0 :
                    (level >= UOS_MAXLEVEL ? UOS_MAXLEVEL - 1 : level));
        pa->align = p->para[newat].align;
        pa->bullet = p->para[newat].bullet;
        pa->chp = default_chp(sh->ph, pa->level);
    }
    sh->para_at = newat;
    sh->para_n  = oldn + 1;
    p->dirty = 1;
    return 1;
}

int uos_para_set_level(uos_pres *p, int slide, int z, int i, int level)
{
    uos_para *pa = uos_para_at(p, slide, z, i);
    const uos_shape *sh = uos_shape_at_c(p, slide, z);
    if (!pa || !sh) return 0;
    if (level < 0) level = 0;
    if (level >= UOS_MAXLEVEL) level = UOS_MAXLEVEL - 1;
    pa->level = (unsigned char)level;
    pa->chp.size = default_chp(sh->ph, level).size;
    p->dirty = 1;
    return 1;
}

/* ---- layouts ------------------------------------------------------------------
 * Applying a layout REFRAMES the placeholders that are already there and adds
 * the ones that are not, so a slide with a title keeps its title when you move
 * it from Bulleted List to Two Column Text.  Anything the user drew stays
 * exactly where it is: only shapes with a placeholder role are touched. */
static void layout_apply(uos_pres *p, int slide, int lay)
{
    const uos_layout *L;
    uos_slide *s = sl(p, slide);
    int i, j;
    unsigned char taken[UOS_MAXPERSLIDE];
    if (!s || lay < 0 || lay >= UOS_AL_COUNT) return;
    L = &kLayouts[lay];
    s->layout = (unsigned char)lay;
    m_zero(taken, (long)sizeof taken);

    for (i = 0; i < L->n; i++) {
        int z = -1;
        for (j = 0; j < s->n; j++) {
            if (taken[j]) continue;
            if (p->shape[s->idx[j]].ph == L->ph[i].ph) { z = j; break; }
        }
        if (z < 0) {
            z = uos_shape_add(p, slide, UOS_G_RECT, L->ph[i].x, L->ph[i].y,
                              L->ph[i].w, L->ph[i].h);
            if (z < 0) continue;
            {
                uos_shape *sh = &p->shape[s->idx[z]];
                sh->ph = L->ph[i].ph;
                sh->fill.kind = UOS_F_NONE;      /* a placeholder is invisible */
                sh->line.kind = UOS_L_NONE;      /* until it holds something   */
            }
        } else {
            uos_shape *sh = &p->shape[s->idx[z]];
            sh->x = L->ph[i].x; sh->y = L->ph[i].y;
            sh->w = L->ph[i].w; sh->h = L->ph[i].h;
        }
        taken[z] = 1;
    }

    /* Placeholders the NEW layout has no use for go, but only if they are
     * EMPTY - PowerPoint keeps content when you change layout and drops the
     * frame when there is nothing in it.  Without this a Title Slide turned
     * into a Bulleted List keeps its centre-title and subtitle holders
     * underneath the new ones, and the slide shows two overlapping "Click to
     * add text" prompts.  Walk downwards so the indices behind stay valid. */
    for (j = s->n - 1; j >= 0; j--) {
        const uos_shape *sh = &p->shape[s->idx[j]];
        int wanted = 0, k;
        if (sh->ph == UOS_PH_NONE || sh->para_n > 0) continue;
        for (k = 0; k < L->n; k++) if (L->ph[k].ph == sh->ph) { wanted = 1; break; }
        if (!wanted) uos_shape_delete(p, slide, j);
    }
}

int uos_slide_layout(const uos_pres *p, int i)
{ const uos_slide *s = sl_c(p, i); return s ? s->layout : 0; }
int uos_slide_set_layout(uos_pres *p, int i, int lay)
{
    if (!sl_ok(p, i) || lay < 0 || lay >= UOS_AL_COUNT) return 0;
    layout_apply(p, i, lay);
    p->dirty = 1;
    return 1;
}

/* ---- slides -------------------------------------------------------------------- */
int uos_slides(const uos_pres *p) { return p ? p->nslide : 0; }

int uos_slide_insert(uos_pres *p, int at, int layout)
{
    int i;
    if (!p || p->nslide >= UOS_MAXSLIDE) return -1;
    if (at < 0) at = 0;
    if (at > p->nslide) at = p->nslide;
    for (i = p->nslide; i > at; i--) p->slide[i] = p->slide[i - 1];
    m_zero(&p->slide[at], (long)sizeof p->slide[at]);
    p->nslide++;
    layout_apply(p, at, layout);
    p->dirty = 1;
    return at;
}
int uos_slide_add(uos_pres *p, int layout)
{ return uos_slide_insert(p, p ? p->nslide : 0, layout); }

int uos_slide_delete(uos_pres *p, int i)
{
    int j;
    if (!p || i < 0 || i >= p->nslide) return 0;
    while (p->slide[i].n > 0) uos_shape_delete(p, i, p->slide[i].n - 1);
    for (j = i; j + 1 < p->nslide; j++) p->slide[j] = p->slide[j + 1];
    p->nslide--;
    m_zero(&p->slide[p->nslide], (long)sizeof p->slide[0]);
    p->dirty = 1;
    return 1;
}

int uos_slide_move(uos_pres *p, int from, int to)
{
    uos_slide tmp;
    int i;
    if (!p || from < 0 || from >= p->nslide || to < 0 || to >= p->nslide) return 0;
    if (from == to) return 1;
    tmp = p->slide[from];
    if (from < to) for (i = from; i < to; i++) p->slide[i] = p->slide[i + 1];
    else           for (i = from; i > to; i--) p->slide[i] = p->slide[i - 1];
    p->slide[to] = tmp;
    p->dirty = 1;
    return 1;
}

int  uos_slide_hidden(const uos_pres *p, int i)
{ const uos_slide *s = sl_c(p, i); return s ? s->hidden : 0; }
void uos_slide_hide(uos_pres *p, int i, int on)
{ uos_slide *s = sl(p, i); if (s) { s->hidden = (unsigned char)(on != 0); p->dirty = 1; } }

void uos_slide_bg(uos_pres *p, int i, const uos_fill *f)
{
    uos_slide *s = sl(p, i);
    if (!s) return;
    if (f) { s->bg = *f; s->has_bg = 1; } else { s->has_bg = 0; }
    p->dirty = 1;
}
void uos_slide_omit_master(uos_pres *p, int i, int on)
{ uos_slide *s = sl(p, i); if (s) { s->omit_master = (unsigned char)(on != 0); p->dirty = 1; } }

/* Reading the parts the renderer needs without exposing uos_slide. */
int uos_slide_has_bg(const uos_pres *p, int i, uos_fill *out)
{
    const uos_slide *s = sl_c(p, i);
    if (!s || !s->has_bg) return 0;
    if (out) *out = s->bg;
    return 1;
}
int uos_slide_omits_master(const uos_pres *p, int i)
{ const uos_slide *s = sl_c(p, i); return s ? s->omit_master : 0; }

/* ---- scheme, header/footer, housekeeping ------------------------------------- */
void uos_set_scheme(uos_pres *p, const uos_scheme *s)
{ if (p && s) { p->scheme = *s; p->dirty = 1; } }
const uos_scheme *uos_get_scheme(const uos_pres *p)
{ return p ? &p->scheme : uos_scheme_at(0); }

fb_px uos_color(const uos_pres *p, fb_px c)
{
    if (!UOS_IS_SCHEME(c)) return c;
    return (p ? &p->scheme : uos_scheme_at(0))->c[UOS_SCHEME_ROLE(c)];
}

void uos_set_hf(uos_pres *p, const uos_hf *hf) { if (p && hf) { p->hf = *hf; p->dirty = 1; } }
const uos_hf *uos_get_hf(const uos_pres *p) { return p ? &p->hf : 0; }

int  uos_dirty(const uos_pres *p) { return p ? p->dirty : 0; }
void uos_set_dirty(uos_pres *p, int on) { if (p) p->dirty = (on != 0); }

/* ---- construction --------------------------------------------------------------
 * A new presentation is what File > New gives you: one Title Slide, the
 * default scheme, and four masters carrying nothing but their placeholders. */
uos_pres *uos_new(void)
{
    uos_pres *p = &g_pres;
    int i;
    m_zero(p, (long)sizeof *p);
    p->scheme = *uos_scheme_at(0);
    p->npara = 1;                    /* index 0 is the "no body" sentinel     */
    p->ntext = 0;
    m_str(p->hf.date, "", (int)sizeof p->hf.date);
    for (i = 0; i < UOS_M_COUNT; i++) {
        int m = UOS_MAXSLIDE + i;
        m_zero(&p->slide[m], (long)sizeof p->slide[m]);
        layout_apply(p, m, i == UOS_M_TITLE ? UOS_AL_TITLE : UOS_AL_BULLETS);
    }
    uos_slide_add(p, UOS_AL_TITLE);
    p->dirty = 0;
    return p;
}
void uos_free(uos_pres *p) { (void)p; }

/* Exposed for the app: what a fresh placeholder should be formatted as. */
uos_chp uos_default_chp(int ph, int level) { return default_chp(ph, level); }
int     uos_ph_is_text(int ph) { return ph_is_text(ph); }
