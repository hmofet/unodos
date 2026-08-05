/* ===========================================================================
 * unoweb layout and paint - the box tree, block formatting, and the display
 * list.
 *
 * Two passes, as the CSS box model wants them: widths resolve TOP-DOWN (a
 * child's containing block is known before the child is laid out) and heights
 * resolve BOTTOM-UP (a parent's auto height is the sum of what its children
 * turned out to be). Trying to do both in one walk is the classic way to get
 * percentage widths and auto heights subtly wrong.
 *
 * Geometry is in DOCUMENT coordinates. The paint pass translates by the scroll
 * offset at replay time, which is what makes scrolling free.
 * ======================================================================== */
#include "uw_int.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

struct uw_box {
    struct uw_box *parent, *first, *last, *prev, *next;
    uw_node        *node;
    const uw_style *style;
    unsigned char   type;
    int x, y, w, h;                  /* border box, document coords */
    int mt, mr, mb, ml;              /* used margins */
    int bt, br, bb, bl;              /* border widths */
    int pt, pr, pb, pl;              /* padding */
    const char *text; int tlen;      /* UW_BOX_TEXT */
    void *image;                     /* UW_BOX_IMAGE */
    unsigned char inline_bg;         /* run came from a nested inline element */
};

struct uw_paint_list {
    uw_paint_cmd *v;
    int n, cap;
};

/* ---- default metrics -----------------------------------------------------
 * If the embedder supplies none, assume a monospace-ish 0.55em advance. The
 * numbers do not matter for correctness of the ALGORITHM, only for the pixels,
 * and every consumer that cares passes real metrics. */
static int def_text_width(void *u, const uw_style *s, const char *t, int len)
{ (void)u; (void)t; return len * (s->font_size * 55 / 100); }
static int def_line_height(void *u, const uw_style *s)
{ (void)u; return s->line_height ? s->line_height : s->font_size * 4 / 3; }

static int measure(uw_doc *d, const uw_style *s, const char *t, int len)
{ return d->metrics.text_width ? d->metrics.text_width(d->metrics.user, s, t, len)
                               : def_text_width(NULL, s, t, len); }
static int lineh(uw_doc *d, const uw_style *s)
{ return d->metrics.line_height ? d->metrics.line_height(d->metrics.user, s)
                                : def_line_height(NULL, s); }

/* ---- box construction ----------------------------------------------------- */
static uw_box *box_new(uw_doc *d, int type, uw_node *n, const uw_style *s)
{
    uw_box *b = (uw_box *)uw_arena(d, sizeof *b);
    if (!b) return NULL;
    b->type = (unsigned char)type;
    b->node = n;
    b->style = s;
    return b;
}

static void box_add(uw_box *parent, uw_box *child)
{
    child->parent = parent;
    child->prev = parent->last;
    if (parent->last) parent->last->next = child; else parent->first = child;
    parent->last = child;
}

static int len_px(uw_len l, int base)
{
    if (l.unit == UW_LEN_PX) return l.v;
    if (l.unit == UW_LEN_PCT) return base * l.v / 100;
    return 0;
}

/* ---- floats ---------------------------------------------------------------
 * A float is taken out of the normal flow, pinned to one side of its
 * containing block, and everything after it flows AROUND it. Two pieces are
 * needed: a record of what is currently floating (so a line knows how much
 * width is left at its own y), and `clear`, which pushes a later block below
 * them.
 *
 * The list is per-containing-block and lives for that block's layout. Floats
 * do not escape their block here - a float taller than its parent still ends
 * at the parent's edge - which is the simplification worth naming, because
 * the real rule (floats leak out of an auto-height parent unless it
 * establishes a new formatting context) is a large part of why CSS float
 * layout is famously fiddly.
 */
#define FLOAT_MAX 16
typedef struct floatrec_s {
    int side;                        /* UW_FLOAT_LEFT / RIGHT */
    int x, y, w, h;                  /* margin box, document coords */
} floatrec;

struct floatctx {
    floatrec f[FLOAT_MAX];
    int n;
    int left, right;                 /* the containing block's content edges */
};
typedef struct floatctx floatctx;

/* Width available for a line at `y` of height `h`, and where it starts. */
static void float_band(floatctx *fc, int y, int h, int *out_x, int *out_w)
{
    int lo = fc->left, hi = fc->right, i;
    for (i = 0; i < fc->n; i++) {
        floatrec *f = &fc->f[i];
        if (y + h <= f->y || y >= f->y + f->h) continue;   /* no overlap */
        if (f->side == UW_FLOAT_LEFT) { if (f->x + f->w > lo) lo = f->x + f->w; }
        else                          { if (f->x < hi)        hi = f->x; }
    }
    if (hi < lo) hi = lo;
    *out_x = lo;
    *out_w = hi - lo;
}

/* The first y at or below `y` where a box of width `w` fits between floats. */
static int float_fit(floatctx *fc, int y, int w, int h)
{
    int guard = 0;
    for (; guard < FLOAT_MAX + 1; guard++) {
        int bx, bw, i, next = 0;
        float_band(fc, y, h, &bx, &bw);
        if (bw >= w) return y;
        for (i = 0; i < fc->n; i++) {          /* drop past the shallowest */
            int bottom = fc->f[i].y + fc->f[i].h;
            if (bottom > y && (!next || bottom < next)) next = bottom;
        }
        if (!next) return y;
        y = next;
    }
    return y;
}

/* y below every float the `clear` value names */
static int float_clear_y(floatctx *fc, int y, int clear)
{
    int i;
    if (clear == UW_CLEAR_NONE) return y;
    for (i = 0; i < fc->n; i++) {
        int side = fc->f[i].side, bottom = fc->f[i].y + fc->f[i].h;
        if (clear == UW_CLEAR_BOTH ||
            (clear == UW_CLEAR_LEFT  && side == UW_FLOAT_LEFT) ||
            (clear == UW_CLEAR_RIGHT && side == UW_FLOAT_RIGHT))
            if (bottom > y) y = bottom;
    }
    return y;
}

static int float_bottom(floatctx *fc)
{
    int i, b = 0;
    for (i = 0; i < fc->n; i++)
        if (fc->f[i].y + fc->f[i].h > b) b = fc->f[i].y + fc->f[i].h;
    return b;
}

/* ---- inline content: greedy word wrap into line boxes --------------------
 * The emitter below is greedy word wrap; ALIGNMENT (text-align,
 * vertical-align) happens in line_close(), because neither the line's final
 * width nor its tallest item is known until the line ends. Still not full
 * inline formatting: nested inline boxes do not carry their own borders
 * across a line break. */
typedef struct {
    uw_doc *d;
    uw_box *block;
    int     avail;               /* content width */
    int     x, y;                /* pen, relative to the block's content box */
    int     line_h;
    int     content_x, content_y;
    int     pending_space;       /* a space is owed before the next word */
    uw_node *cur_elem;           /* the element whose text is being flowed */
    uw_box *line;
    struct floatctx *floats;     /* NULL when no float is in play */
    int     line_x0;             /* band offset of the current line          */
    int     avail_full;          /* the block's width, before any float      */
} inline_ctx;

/* Ascent of a box: how far its top sits above the line's baseline. Text asks
 * the embedder (fonts are the embedder's business - see uw_metrics); anything
 * else, an image included, sits ON the baseline, which is what CSS means by
 * an inline replaced box defaulting to vertical-align: baseline. */
static int box_ascent(inline_ctx *ic, uw_box *b)
{
    if (b->type == UW_BOX_TEXT && ic->d->metrics.baseline)
        return ic->d->metrics.baseline(ic->d->metrics.user, b->style);
    if (b->type == UW_BOX_TEXT)
        return b->h * 4 / 5;     /* no hook: the usual ~80% of the line box */
    return b->h;                 /* replaced content rests on the baseline  */
}

/* Close the current line: align its children vertically against a shared
 * baseline, then horizontally per text-align.
 *
 * Doing BOTH here rather than at emit time is the point. Neither the line's
 * final width nor its tallest item is known until the line ends, and both
 * alignments need exactly that - which is why the greedy emitter simply
 * stacks boxes at the pen and this function puts them where they belong.
 * `last` suppresses justification on a block's final line, per CSS. */
static void line_close(inline_ctx *ic, int last)
{
    uw_box *c;
    int ascent = 0, top, slack, nchild = 0, ngap;

    if (!ic->line) return;
    ic->line->w = ic->x;
    top = ic->line->y;

    /* A line box is max(ascent) + max(descent), NOT max(height): once items
     * share a baseline instead of a common top, the tallest ascender and the
     * deepest descender can come from different items, and sizing by height
     * alone lets a descender hang into the next line. */
    {   int descent = 0;
        for (c = ic->line->first; c; c = c->next) {
            int a = box_ascent(ic, c), dsc = c->h - a;
            if (a > ascent) ascent = a;
            if (dsc > descent) descent = dsc;
            nchild++;
        }
        if (ascent + descent > ic->line_h) ic->line_h = ascent + descent;
    }
    ic->line->h = ic->line_h;
    for (c = ic->line->first; c; c = c->next) {
        switch (c->style ? c->style->vertical_align : UW_VA_BASELINE) {
        case UW_VA_TOP:    c->y = top; break;
        case UW_VA_BOTTOM: c->y = top + ic->line_h - c->h; break;
        case UW_VA_MIDDLE: c->y = top + (ic->line_h - c->h) / 2; break;
        case UW_VA_SUB:    c->y = top + ascent - box_ascent(ic, c) + c->h / 5; break;
        case UW_VA_SUPER:  c->y = top + ascent - box_ascent(ic, c) - c->h / 3; break;
        default:           c->y = top + ascent - box_ascent(ic, c); break;
        }
    }

    slack = ic->avail - ic->x;
    if (slack > 0 && nchild) {
        int align = ic->line->style ? ic->line->style->text_align : UW_ALIGN_LEFT;
        if (align == UW_ALIGN_CENTER)
            for (c = ic->line->first; c; c = c->next) c->x += slack / 2;
        else if (align == UW_ALIGN_RIGHT)
            for (c = ic->line->first; c; c = c->next) c->x += slack;
        else if (align == UW_ALIGN_JUSTIFY && !last && (ngap = nchild - 1) > 0) {
            /* spread the slack across the inter-box gaps; the remainder goes
             * to the leftmost gaps so the right edge lands exactly flush */
            int i = 0, rem = slack % ngap, step = slack / ngap, shift = 0;
            for (c = ic->line->first; c; c = c->next, i++) {
                c->x += shift;
                if (i < ngap) shift += step + (i < rem ? 1 : 0);
            }
            ic->line->w = ic->avail;
        }
    }
}

static void line_break(inline_ctx *ic)
{
    line_close(ic, 0);
    ic->y += ic->line_h;
    ic->x = 0;
    ic->line_x0 = 0;
    if (ic->floats) ic->avail = ic->avail_full;   /* re-measured by ensure_line */
    ic->line_h = 0;
    ic->pending_space = 0;       /* a space at a line end is not drawn */
    ic->line = NULL;
}

static void ensure_line(inline_ctx *ic, const uw_style *s)
{
    if (ic->line) return;
    ic->line = box_new(ic->d, UW_BOX_LINE, NULL, s);
    if (!ic->line) return;
    ic->line->x = ic->content_x;
    ic->line->y = ic->content_y + ic->y;
    /* A line beside a float is narrower, and starts further in when the float
     * is on the left. This is the whole of "text flows around an image". */
    if (ic->floats && ic->floats->n) {
        int bx, bw, lh = ic->line_h > 0 ? ic->line_h : lineh(ic->d, s);
        float_band(ic->floats, ic->line->y, lh, &bx, &bw);
        ic->line->x = bx;
        ic->line_x0 = bx - ic->content_x;
        ic->avail = bw;
    }
    box_add(ic->block, ic->line);
}

static void emit_word(inline_ctx *ic, const uw_style *s, const char *t, int len)
{
    int w = measure(ic->d, s, t, len);
    int lh = lineh(ic->d, s);
    int sp = (ic->pending_space && ic->x > 0) ? measure(ic->d, s, " ", 1) : 0;
    uw_box *tb;
    /* The separating space is applied HERE, before the word, not after the
     * previous one - otherwise a line that wraps carries its trailing space
     * into the line box width and every right edge is one space too far. */
    if (ic->x > 0 && ic->x + sp + w > ic->avail) { line_break(ic); sp = 0; }
    ic->x += sp;
    ic->pending_space = 0;
    ensure_line(ic, s);
    if (!ic->line) return;
    tb = box_new(ic->d, UW_BOX_TEXT, NULL, s);
    if (!tb) return;
    tb->node = ic->cur_elem;     /* so hit testing can name the <a>, not the <p> */
    /* Only a run belonging to a NESTED inline element paints its own
     * background. A run that is simply the block's own text already sits on
     * the block's background - painting it again would double-draw it and,
     * worse, draw it only under the words rather than across the box. */
    tb->inline_bg = (unsigned char)(ic->cur_elem && ic->block &&
                                    ic->cur_elem != ic->block->node);
    tb->text = t; tb->tlen = len;
    tb->x = ic->content_x + ic->line_x0 + ic->x;
    tb->y = ic->content_y + ic->y;
    tb->w = w;
    tb->h = lh;
    box_add(ic->line, tb);
    ic->x += w;
    if (lh > ic->line_h) ic->line_h = lh;
}

static void flow_inline(inline_ctx *ic, uw_node *n, const uw_style *inherited);

/* <img> is a REPLACED box: it occupies intrinsic size (overridden by CSS
 * width/height) and paints opaquely. unoweb decodes nothing - the embedder's
 * resolve hook supplies the size and an opaque handle. No hook, or a failed
 * resolve, means zero size, which is what a broken image should be. */
static void emit_image(inline_ctx *ic, uw_node *n, const uw_style *s)
{
    int iw = 0, ih = 0;
    void *handle = NULL;
    uw_box *b;
    const char *src = uw_attr(ic->d, n, "src");
    if (ic->d->images.resolve && src)
        ic->d->images.resolve(ic->d->images.user, src, &iw, &ih, &handle);
    if (s->width.unit == UW_LEN_PX) iw = s->width.v;
    if (s->height.unit == UW_LEN_PX) ih = s->height.v;
    if (iw <= 0 && ih <= 0) return;
    if (iw > ic->avail && ic->avail > 0) {
        ih = (int)((long)ih * ic->avail / iw);
        iw = ic->avail;
        if (ih < 1) ih = 1;
    }
    if (ic->x > 0 && ic->x + iw > ic->avail) line_break(ic);
    ensure_line(ic, s);
    if (!ic->line) return;
    b = box_new(ic->d, UW_BOX_IMAGE, n, s);
    if (!b) return;
    b->image = handle;
    b->x = ic->content_x + ic->line_x0 + ic->x;
    b->y = ic->content_y + ic->y;
    b->w = iw; b->h = ih;
    box_add(ic->line, b);
    ic->x += iw;
    ic->pending_space = 0;
    if (ih > ic->line_h) ic->line_h = ih;
}

static void flow_text(inline_ctx *ic, const char *t, int len, const uw_style *s)
{
    int i = 0;
    if (s->white_space == UW_WS_PRE) {
        int start = 0;
        for (i = 0; i <= len; i++) {
            if (i == len || t[i] == '\n') {
                if (i > start) emit_word(ic, s, t + start, i - start);
                if (i < len) { if (!ic->line) ensure_line(ic, s);
                               if (!ic->line_h) ic->line_h = lineh(ic->d, s);
                               line_break(ic); }
                start = i + 1;
            }
        }
        return;
    }
    /* Leading whitespace still separates words ACROSS an element boundary:
     * "<code>span</code> here" is one text node ending at "span" and another
     * beginning with a space, and dropping it rendered "spanhere". */
    if (len && (t[0]==' '||t[0]=='\t'||t[0]=='\n'||t[0]=='\r') && ic->x > 0)
        ic->pending_space = 1;
    while (i < len) {
        int start;
        while (i < len && (t[i]==' '||t[i]=='\t'||t[i]=='\n'||t[i]=='\r')) i++;
        start = i;
        while (i < len && !(t[i]==' '||t[i]=='\t'||t[i]=='\n'||t[i]=='\r')) i++;
        if (i > start) {
            emit_word(ic, s, t + start, i - start);
            if (i < len) ic->pending_space = 1;
        }
    }
}

static void flow_inline(inline_ctx *ic, uw_node *n, const uw_style *inherited)
{
    uw_node *c;
    for (c = uw_first_child(n); c; c = uw_next_sibling(c)) {
        if (uw_type(c) == UW_NODE_TEXT) {
            int tl = 0;
            const char *t = uw_text(c, &tl);
            if (t && tl) flow_text(ic, t, tl, inherited);
            continue;
        }
        if (uw_type(c) != UW_NODE_ELEMENT) continue;
        {   const uw_style *s = uw_computed(c);
            if (!s || s->display == UW_DISP_NONE) continue;
            if (!strcmp(uw_tag_name(ic->d, c), "img")) { emit_image(ic, c, s); continue; }
            if (!strcmp(uw_tag_name(ic->d, c), "br")) {
                if (!ic->line) ensure_line(ic, s);
                if (!ic->line_h) ic->line_h = lineh(ic->d, s);
                line_break(ic);
                continue;
            }
            {   uw_node *save = ic->cur_elem;
                ic->cur_elem = c;
                flow_inline(ic, c, s);
                ic->cur_elem = save; }
        }
    }
}

/* ---- block layout ---------------------------------------------------------
 * Pass 1 (this function) resolves this box's width and position, lays out its
 * children, then resolves its own height from them. */
static void layout_block(uw_doc *d, uw_box *b, int cb_width, int x, int y,
                         floatctx *fc);

static uw_box *build_block(uw_doc *d, uw_node *n, const uw_style *s)
{
    uw_box *b = box_new(d, UW_BOX_BLOCK, n, s);
    return b;
}

static uw_box *build_block(uw_doc *d, uw_node *n, const uw_style *s);
static void layout_block(uw_doc *d, uw_box *b, int cb_width, int x, int y,
                         floatctx *fc);

/* ---- tables ---------------------------------------------------------------
 * Auto table layout, the useful subset. Rows come from the table's children,
 * descending THROUGH row groups (tbody/thead/tfoot exist in the tree but own
 * no box - a table's rows are its rows wherever the parser put them).
 *
 * Column widths are proportional to how much text each column holds, floored
 * so a column can never vanish. Equal columns would be simpler and are what a
 * first attempt reaches for, but they make the common table - a narrow index
 * column beside a wide description - unreadable, and the text length is
 * already in hand. This is not the CSS auto-table algorithm (which needs
 * min/max content widths per cell, i.e. laying every cell out twice); it is
 * the cheap approximation that gets the common shapes right.
 *
 * A row's height is its tallest cell, and every cell in the row is stretched
 * to it, which is what makes a table look like a table rather than a set of
 * independently-sized boxes.
 */
#define TBL_MAXCOL 16
#define TBL_MINCOL 24                    /* px: a column never disappears */

static int cell_text_weight(uw_doc *d, uw_node *cell)
{
    uw_node *n;
    int w = 0;
    for (n = cell; n; n = uw_next_in_order(n, cell)) {
        int tl = 0;
        if (uw_type(n) != UW_NODE_TEXT) continue;
        if (uw_text(n, &tl)) w += tl;
    }
    (void)d;
    return w;
}

/* Append every row of `parent` to `rows`, looking through row groups. */
static void table_rows(uw_doc *d, uw_node *parent, uw_node **rows, int *nrows, int max)
{
    uw_node *c;
    for (c = uw_first_child(parent); c && *nrows < max; c = uw_next_sibling(c)) {
        const uw_style *cs;
        if (uw_type(c) != UW_NODE_ELEMENT) continue;
        cs = uw_computed(c);
        if (!cs || cs->display == UW_DISP_NONE) continue;
        if (cs->display == UW_DISP_TABLE_ROW_GROUP) table_rows(d, c, rows, nrows, max);
        else if (cs->display == UW_DISP_TABLE_ROW) rows[(*nrows)++] = c;
    }
}

static void layout_table(uw_doc *d, uw_box *b, int content_x, int content_y,
                         int content_w, int *out_h)
{
#define TBL_MAXROW 64
    uw_node *rows[TBL_MAXROW];
    int colw[TBL_MAXCOL], weight[TBL_MAXCOL];
    int nrows = 0, ncols = 0, i, c, cy = content_y;
    long total = 0;
    int spacing = 2;                       /* border-spacing, fixed for now */

    table_rows(d, b->node, rows, &nrows, TBL_MAXROW);
    if (!nrows) { *out_h = 0; return; }

    for (i = 0; i < TBL_MAXCOL; i++) weight[i] = 0;
    for (i = 0; i < nrows; i++) {
        uw_node *cell;
        int k = 0;
        for (cell = uw_first_child(rows[i]); cell; cell = uw_next_sibling(cell)) {
            const uw_style *cs;
            int wgt;
            if (uw_type(cell) != UW_NODE_ELEMENT) continue;
            cs = uw_computed(cell);
            if (!cs || cs->display == UW_DISP_NONE) continue;
            if (k >= TBL_MAXCOL) break;
            wgt = cell_text_weight(d, cell);
            if (wgt > weight[k]) weight[k] = wgt;
            k++;
        }
        if (k > ncols) ncols = k;
    }
    if (!ncols) { *out_h = 0; return; }

    for (i = 0; i < ncols; i++) { if (weight[i] < 1) weight[i] = 1; total += weight[i]; }
    {   int avail = content_w - spacing * (ncols + 1), used = 0;
        if (avail < ncols * TBL_MINCOL) avail = ncols * TBL_MINCOL;
        for (i = 0; i < ncols; i++) {
            colw[i] = (int)((long)avail * weight[i] / (total ? total : 1));
            if (colw[i] < TBL_MINCOL) colw[i] = TBL_MINCOL;
            used += colw[i];
        }
        /* rounding remainder goes to the widest column, so the table's right
         * edge lands exactly on the content edge */
        if (used < avail) {
            int widest = 0;
            for (i = 1; i < ncols; i++) if (colw[i] > colw[widest]) widest = i;
            colw[widest] += avail - used;
        }
    }

    cy += spacing;
    for (i = 0; i < nrows; i++) {
        uw_node *cell;
        uw_box *rowbox = box_new(d, UW_BOX_BLOCK, rows[i], uw_computed(rows[i]));
        int cx = content_x + spacing, k = 0, rowh = 0;
        if (!rowbox) continue;
        rowbox->x = content_x; rowbox->y = cy; rowbox->w = content_w;
        box_add(b, rowbox);
        for (cell = uw_first_child(rows[i]); cell; cell = uw_next_sibling(cell)) {
            const uw_style *cs;
            uw_box *cb;
            if (uw_type(cell) != UW_NODE_ELEMENT) continue;
            cs = uw_computed(cell);
            if (!cs || cs->display == UW_DISP_NONE) continue;
            if (k >= ncols) break;
            cb = build_block(d, cell, cs);
            if (!cb) { k++; continue; }
            box_add(rowbox, cb);
            /* a cell is a block laid out in a containing block of exactly its
             * column width; margins do not apply to cells, so cb_width IS the
             * column and the cell fills it */
            layout_block(d, cb, colw[k], cx, cy, NULL);
            cb->w = colw[k];
            if (cb->h > rowh) rowh = cb->h;
            cx += colw[k] + spacing;
            k++;
        }
        /* every cell stretches to the row height - the thing that makes a
         * table read as a grid rather than as ragged boxes */
        {   uw_box *cb;
            for (cb = rowbox->first; cb; cb = cb->next) cb->h = rowh; }
        rowbox->h = rowh;
        cy += rowh + spacing;
    }
    *out_h = cy - content_y;
#undef TBL_MAXROW
}

/* `inherited` is the float context of the block formatting context this box
 * belongs to. Floats are NOT scoped to the block that declared them - a float
 * in <body> shortens the lines of every paragraph after it, which are
 * separate blocks - so the context is passed down rather than rebuilt per
 * block. NULL starts a new one (the root, and anything that would establish
 * its own BFC once that concept exists here). */
static void layout_children(uw_doc *d, uw_box *b, int content_x, int content_y,
                            int content_w, int *out_h, floatctx *inherited)
{
    uw_node *c;
    int cy = content_y;
    int prev_margin = 0;            /* for collapsing with the next sibling */
    int first = 1;
    int any_block = 0;
    floatctx own;
    floatctx *fcp = inherited;

    if (!fcp) { own.n = 0; own.left = content_x; own.right = content_x + content_w; fcp = &own; }

    /* FLOATS first: they are out of flow, so their geometry has to exist
     * before any in-flow content asks how much room is left beside them. A
     * float is placed at the current flow position, pushed down until it
     * fits, and recorded; everything after flows around what is recorded. */
    for (c = uw_first_child(b->node); c; c = uw_next_sibling(c)) {
        const uw_style *cs;
        uw_box *fb;
        int fw, fy;
        if (uw_type(c) != UW_NODE_ELEMENT) continue;
        cs = uw_computed(c);
        if (!cs || cs->display == UW_DISP_NONE) continue;
        if (cs->cssfloat == UW_FLOAT_NONE) continue;
        if (fcp->n >= FLOAT_MAX) continue;
        fb = build_block(d, c, cs);
        if (!fb) continue;
        box_add(b, fb);
        /* a float shrinks to its width if given one, else to half the
         * containing block - a float with no width is not "as wide as the
         * block", which would leave nothing to flow beside it */
        fw = cs->width.unit == UW_LEN_AUTO ? content_w / 2 : len_px(cs->width, content_w);
        if (fw < 1) fw = 1;
        if (fw > content_w) fw = content_w;
        layout_block(d, fb, fw, content_x, content_y, fcp);
        fb->w = fw;
        fy = float_fit(fcp, content_y, fw, fb->h);
        fb->y = fy;
        if (cs->cssfloat == UW_FLOAT_RIGHT) {
            int bx, bw;
            float_band(fcp, fy, fb->h, &bx, &bw);
            fb->x = bx + bw - fw;
        }
        fcp->f[fcp->n].side = cs->cssfloat;
        fcp->f[fcp->n].x = fb->x;  fcp->f[fcp->n].y = fb->y;
        fcp->f[fcp->n].w = fb->w;  fcp->f[fcp->n].h = fb->h;
        fcp->n++;
    }

    for (c = uw_first_child(b->node); c; c = uw_next_sibling(c)) {
        const uw_style *cs;
        if (uw_type(c) != UW_NODE_ELEMENT) continue;
        cs = uw_computed(c);
        if (!cs || cs->display == UW_DISP_NONE) continue;
        if (cs->cssfloat != UW_FLOAT_NONE) continue;     /* already placed */
        if (cs->display != UW_DISP_BLOCK && cs->display != UW_DISP_LIST_ITEM &&
            cs->display != UW_DISP_TABLE) continue;
        {   uw_box *cb = build_block(d, c, cs);
            int mt;
            if (!cb) continue;
            box_add(b, cb);
            mt = len_px(cs->margin[UW_TOP], content_w);
            /* Adjacent vertical margins collapse to the LARGER of the two.
             * Without this every paragraph in a document would be separated by
             * the sum of both margins and the page would read as double-spaced. */
            if (first) { cy += mt; first = 0; }
            else cy += (mt > prev_margin ? mt : prev_margin) - prev_margin;
            cy = float_clear_y(fcp, cy, cs->clear);
            layout_block(d, cb, content_w, content_x, cy, fcp);
            /* absolute and fixed are OUT OF FLOW: the boxes after them must
             * lay out as though they were not there, so the flow position
             * does not advance past one. */
            if (cs->position == UW_POS_ABSOLUTE || cs->position == UW_POS_FIXED)
                continue;
            cy = cb->y + cb->h;
            if (cs->position == UW_POS_RELATIVE) {   /* space stays behind */
                if (cs->offset[UW_TOP].unit != UW_LEN_AUTO)
                    cy -= len_px(cs->offset[UW_TOP], content_w);
                else if (cs->offset[UW_BOTTOM].unit != UW_LEN_AUTO)
                    cy += len_px(cs->offset[UW_BOTTOM], content_w);
            }
            prev_margin = len_px(cs->margin[UW_BOTTOM], content_w);
            cy += prev_margin;
            any_block = 1;
        }
    }
    if (any_block) {
        int fb2 = inherited ? 0 : float_bottom(fcp);
        if (fb2 > cy) cy = fb2;      /* the parent contains its floats */
        *out_h = cy - content_y;
        return;
    }

    /* no block children: this box establishes an inline formatting context */
    {   inline_ctx ic;
        memset(&ic, 0, sizeof ic);
        ic.d = d; ic.block = b; ic.avail = content_w > 0 ? content_w : 1;
        ic.avail_full = ic.avail;
        ic.content_x = content_x; ic.content_y = content_y;
        ic.cur_elem = b->node;
        if (fcp->n) ic.floats = fcp;
        flow_inline(&ic, b->node, b->style);
        /* the block's LAST line: aligned like the rest, but never justified
         * (CSS leaves a final line ragged - stretching it is the classic
         * broken-justification look) */
        if (ic.line) { line_close(&ic, 1); ic.y += ic.line_h; }
        if (!inherited) {                  /* the BFC root contains its floats */
            int fb2 = float_bottom(fcp) - content_y;
            if (fb2 > ic.y) ic.y = fb2; }
        *out_h = ic.y;
    }
}

static void layout_block(uw_doc *d, uw_box *b, int cb_width, int x, int y,
                         floatctx *fc)
{
    const uw_style *s = b->style;
    int avail, content_w, content_h = 0;

    b->ml = len_px(s->margin[UW_LEFT], cb_width);
    b->mr = len_px(s->margin[UW_RIGHT], cb_width);
    b->mt = len_px(s->margin[UW_TOP], cb_width);
    b->mb = len_px(s->margin[UW_BOTTOM], cb_width);
    b->bl = s->border_width[UW_LEFT];   b->br = s->border_width[UW_RIGHT];
    b->bt = s->border_width[UW_TOP];    b->bb = s->border_width[UW_BOTTOM];
    b->pl = len_px(s->padding[UW_LEFT], cb_width);
    b->pr = len_px(s->padding[UW_RIGHT], cb_width);
    b->pt = len_px(s->padding[UW_TOP], cb_width);
    b->pb = len_px(s->padding[UW_BOTTOM], cb_width);

    avail = cb_width - b->ml - b->mr;
    if (avail < 0) avail = 0;
    if (s->width.unit == UW_LEN_AUTO) {
        b->w = avail;                                  /* fill the line */
        content_w = b->w - b->bl - b->br - b->pl - b->pr;
    } else {
        content_w = len_px(s->width, cb_width);
        b->w = content_w + b->bl + b->br + b->pl + b->pr;
    }
    if (content_w < 0) content_w = 0;

    b->x = x + b->ml;
    b->y = y;
    /* `relative` shifts BEFORE the children are laid out, so the whole
     * subtree moves with the box. Applying it afterwards moves the border
     * box and leaves its own text behind, which is what the first version
     * did. The space the box occupied in the flow is unaffected - the parent
     * advances from the unshifted position (see layout_children). */
    if (s->position == UW_POS_RELATIVE) {
        if (s->offset[UW_LEFT].unit != UW_LEN_AUTO) b->x += len_px(s->offset[UW_LEFT], cb_width);
        else if (s->offset[UW_RIGHT].unit != UW_LEN_AUTO) b->x -= len_px(s->offset[UW_RIGHT], cb_width);
        if (s->offset[UW_TOP].unit != UW_LEN_AUTO) b->y += len_px(s->offset[UW_TOP], cb_width);
        else if (s->offset[UW_BOTTOM].unit != UW_LEN_AUTO) b->y -= len_px(s->offset[UW_BOTTOM], cb_width);
    }

    if (s->display == UW_DISP_TABLE)
        layout_table(d, b, b->x + b->bl + b->pl, b->y + b->bt + b->pt,
                     content_w, &content_h);
    else
        layout_children(d, b, b->x + b->bl + b->pl, b->y + b->bt + b->pt,
                        content_w, &content_h, fc);

    if (s->height.unit != UW_LEN_AUTO) content_h = len_px(s->height, 0);
    b->h = content_h + b->bt + b->bb + b->pt + b->pb;

    /* position: applied AFTER the box has its normal-flow geometry, which is
     * exactly what CSS means - `relative` offsets from where the box would
     * have been, and leaves the space it occupied behind. `absolute` and
     * `fixed` are placed against their containing block instead; here that
     * is the nearest positioned ancestor's padding box, approximated by the
     * containing block this call was given. Fixed uses the same path and is
     * pinned to the viewport by the PAINT pass, since only paint knows the
     * scroll. */
    if (s->position == UW_POS_ABSOLUTE || s->position == UW_POS_FIXED) {
        if (s->offset[UW_LEFT].unit != UW_LEN_AUTO)
            b->x = x + len_px(s->offset[UW_LEFT], cb_width);
        else if (s->offset[UW_RIGHT].unit != UW_LEN_AUTO)
            b->x = x + cb_width - b->w - len_px(s->offset[UW_RIGHT], cb_width);
        if (s->offset[UW_TOP].unit != UW_LEN_AUTO)
            b->y = y + len_px(s->offset[UW_TOP], cb_width);
        else if (s->offset[UW_BOTTOM].unit != UW_LEN_AUTO)
            b->y = y - b->h - len_px(s->offset[UW_BOTTOM], cb_width);
    }
}

int uw_layout(uw_doc *d, int width, int height, const uw_metrics *m)
{
    uw_node *body;
    const uw_style *bs;
    if (!d) return -1;
    if (m) d->metrics = *m; else memset(&d->metrics, 0, sizeof d->metrics);
    if (!d->styled) uw_style_document(d, width, height);
    body = uw_body(d);
    if (!body) return -1;
    bs = uw_computed(body);
    if (!bs) return -1;
    d->layout_w = width;
    d->layout_h = height;
    d->layout_root = build_block(d, body, bs);
    if (!d->layout_root) return -1;
    /* layout_block positions a BORDER box: the caller owns the margin, because
     * sibling collapsing is resolved by the parent. The root has no parent, so
     * its top margin is applied here or it is silently lost. */
    layout_block(d, d->layout_root, width, 0,
                 len_px(bs->margin[UW_TOP], width), NULL);
    return d->layout_root->h + d->layout_root->mt + d->layout_root->mb;
}

uw_box *uw_layout_root(uw_doc *d) { return d ? d->layout_root : NULL; }
int     uw_box_type(uw_box *b)    { return b ? b->type : 0; }
uw_node *uw_box_node(uw_box *b)   { return b ? b->node : NULL; }
const uw_style *uw_box_style(uw_box *b) { return b ? b->style : NULL; }
uw_box *uw_box_first_child(uw_box *b)   { return b ? b->first : NULL; }
uw_box *uw_box_next(uw_box *b)          { return b ? b->next : NULL; }

void uw_box_rect(uw_box *b, int *x, int *y, int *w, int *h)
{
    if (!b) return;
    if (x) *x = b->x;
    if (y) *y = b->y;
    if (w) *w = b->w;
    if (h) *h = b->h;
}

const char *uw_box_text(uw_box *b, int *len)
{
    if (!b || b->type != UW_BOX_TEXT) { if (len) *len = 0; return NULL; }
    if (len) *len = b->tlen;
    return b->text;
}

/* ---- the display list ----------------------------------------------------- */
static int g_paint_z;             /* z of the box currently emitting */

static int pl_push(uw_doc *d, const uw_paint_cmd *c)
{
    struct uw_paint_list *p = d->paint;
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        uw_paint_cmd *nv = (uw_paint_cmd *)uw_arena(d, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        if (p->v) memcpy(nv, p->v, (size_t)p->n * sizeof *nv);
        p->v = nv; p->cap = nc;
    }
    p->v[p->n] = *c;
    p->v[p->n].z = g_paint_z;      /* stamped here so no emitter can forget */
    p->n++;
    return 0;
}

static void paint_box(uw_doc *d, uw_box *b)
{
    uw_paint_cmd c;
    const uw_style *s = b->style;
    uw_box *k;
    int save_z = g_paint_z;
    memset(&c, 0, sizeof c);
    /* A positioned box with a z-index carries it to everything it and its
     * descendants emit: z applies to a whole subtree, not to one rectangle. */
    if (s && s->position != UW_POS_STATIC && s->z_index) g_paint_z = s->z_index;

    if (b->type == UW_BOX_IMAGE) {
        c.cmd = UW_CMD_IMAGE;
        c.x = b->x; c.y = b->y; c.w = b->w; c.h = b->h;
        c.image = b->image;
        c.style = s;
        pl_push(d, &c);
        return;
    }
    if (b->type == UW_BOX_TEXT) {
        /* An INLINE background paints behind the run itself. Blocks get theirs
         * from the block branch below, but <code> and friends are inline and
         * have no box of their own - without this their background-color would
         * simply never appear. */
        if (s && s->has_bg && b->inline_bg) {
            c.cmd = UW_CMD_RECT;
            c.x = b->x - 1; c.y = b->y - 1; c.w = b->w + 2; c.h = b->h + 2;
            c.color = s->background_color;
            pl_push(d, &c);
            memset(&c, 0, sizeof c);
        }
        c.cmd = UW_CMD_TEXT;
        c.x = b->x; c.y = b->y; c.w = b->w; c.h = b->h;
        c.color = s->color;
        c.text = b->text; c.len = b->tlen;
        c.style = s;
        pl_push(d, &c);
        return;
    }
    if (b->type == UW_BOX_BLOCK && s) {
        /* backgrounds and borders paint before children, so content lands on
         * top of them - that ordering IS the stacking rule for M3 */
        if (s->has_bg) {
            c.cmd = UW_CMD_RECT;
            c.x = b->x; c.y = b->y; c.w = b->w; c.h = b->h;
            c.color = s->background_color;
            pl_push(d, &c);
        }
        {   int i;
            static const int sides[4] = { UW_TOP, UW_RIGHT, UW_BOTTOM, UW_LEFT };
            for (i = 0; i < 4; i++) {
                int sd = sides[i];
                if (!s->border_width[sd]) continue;
                memset(&c, 0, sizeof c);
                c.cmd = UW_CMD_BORDER;
                c.color = s->border_color[sd];
                switch (sd) {
                case UW_TOP:    c.x=b->x; c.y=b->y; c.w=b->w; c.h=s->border_width[sd]; break;
                case UW_BOTTOM: c.x=b->x; c.y=b->y+b->h-s->border_width[sd];
                                c.w=b->w; c.h=s->border_width[sd]; break;
                case UW_LEFT:   c.x=b->x; c.y=b->y; c.w=s->border_width[sd]; c.h=b->h; break;
                default:        c.x=b->x+b->w-s->border_width[sd]; c.y=b->y;
                                c.w=s->border_width[sd]; c.h=b->h; break;
                }
                pl_push(d, &c);
            }
        }
        if (s->display == UW_DISP_LIST_ITEM && s->list_bullet) {
            memset(&c, 0, sizeof c);
            c.cmd = UW_CMD_BULLET;
            c.x = b->x - 12; c.y = b->y + s->font_size / 2;
            c.w = 3; c.h = 3;
            c.color = s->color;
            pl_push(d, &c);
        }
    }
    for (k = b->first; k; k = k->next) paint_box(d, k);
    g_paint_z = save_z;
}

int uw_paint(uw_doc *d)
{
    if (!d || !d->layout_root) return 0;
    if (!d->paint) {
        d->paint = (struct uw_paint_list *)uw_arena(d, sizeof *d->paint);
        if (!d->paint) return 0;
    }
    d->paint->n = 0;
    paint_box(d, d->layout_root);

    /* z-index: a STABLE sort of the finished list by the z of the box each
     * command came from. Sorting the list rather than ordering the walk keeps
     * paint_box a plain tree recursion, and stability is what preserves
     * document order among equal z - which is the rule for everything that
     * has no z-index of its own, i.e. almost every box on a page.
     * Insertion sort: the list is nearly always already ordered (z-index is
     * rare), so this is a linear scan in the common case. */
    {   int i, j;
        for (i = 1; i < d->paint->n; i++) {
            uw_paint_cmd tmp = d->paint->v[i];
            int z = tmp.z;
            for (j = i - 1; j >= 0 && d->paint->v[j].z > z; j--)
                d->paint->v[j + 1] = d->paint->v[j];
            d->paint->v[j + 1] = tmp;
        }
    }
    return d->paint->n;
}

int uw_paint_count(uw_doc *d) { return (d && d->paint) ? d->paint->n : 0; }

const uw_paint_cmd *uw_paint_at(uw_doc *d, int i)
{
    if (!d || !d->paint || i < 0 || i >= d->paint->n) return NULL;
    return &d->paint->v[i];
}

void uw_set_images(uw_doc *d, const uw_images *im)
{
    if (!d) return;
    if (im) d->images = *im;
    else memset(&d->images, 0, sizeof d->images);
}

/* ---- hit testing ----------------------------------------------------------
 * Walk the boxes in reverse paint order and take the first one containing the
 * point, then attribute it to a DOM node. Text and image boxes carry no node
 * of their own (they belong to whatever block flowed them), so the answer
 * comes from the nearest ancestor box that does - which is how a click on a
 * word inside <a> resolves to the <a>. */
static uw_node *hit_box(uw_box *b, int x, int y, uw_node *inherited)
{
    uw_box *k;
    uw_node *best = NULL;
    if (x < b->x || y < b->y || x >= b->x + b->w || y >= b->y + b->h) {
        /* a line box can be narrower than its block, so keep descending
         * through boxes that do not themselves contain the point only when
         * they are structural (blocks); a miss on a leaf really is a miss */
        if (b->type != UW_BOX_BLOCK) return NULL;
    }
    if (b->node) inherited = b->node;
    for (k = b->first; k; k = k->next) {
        uw_node *r = hit_box(k, x, y, inherited);
        if (r) best = r;                     /* later siblings paint on top */
    }
    if (best) return best;
    if (x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h)
        return inherited;
    return NULL;
}

uw_node *uw_hit_test(uw_doc *d, int x, int y)
{
    if (!d || !d->layout_root) return NULL;
    return hit_box(d->layout_root, x, y, NULL);
}

uw_node *uw_link_at(uw_doc *d, uw_node *n)
{
    for (; n; n = uw_parent(n)) {
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        if (!strcmp(uw_tag_name(d, n), "a") && uw_has_attr(d, n, "href")) return n;
    }
    return NULL;
}

/* ---- golden dumps ---------------------------------------------------------- */
typedef struct { char *b; int max, n; } lob;

static void lp(lob *o, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int len;
    va_start(ap, fmt);
    len = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (len < 0) return;
    if (o->max > 0) {
        int room = o->max - 1 - o->n;
        if (room > 0) { int k = len < room ? len : room; memcpy(o->b + o->n, tmp, (size_t)k); }
        o->b[o->n + len < o->max - 1 ? o->n + len : o->max - 1] = 0;
    }
    o->n += len;
}

static void dump_box(uw_doc *d, uw_box *b, int depth, lob *o)
{
    uw_box *k;
    int i;
    for (i = 0; i < depth; i++) lp(o, "  ");
    switch (b->type) {
    case UW_BOX_BLOCK:
        lp(o, "block %s (%d,%d %dx%d)\n",
           b->node ? uw_tag_name(d, b->node) : "?", b->x, b->y, b->w, b->h);
        break;
    case UW_BOX_LINE:
        lp(o, "line (%d,%d %dx%d)\n", b->x, b->y, b->w, b->h);
        break;
    case UW_BOX_TEXT:
        lp(o, "text (%d,%d %dx%d) \"%.*s\"\n", b->x, b->y, b->w, b->h, b->tlen, b->text);
        break;
    case UW_BOX_IMAGE:
        lp(o, "image %s (%d,%d %dx%d)\n",
           b->node ? uw_tag_name(d, b->node) : "?", b->x, b->y, b->w, b->h);
        break;
        break;
    default:
        lp(o, "box (%d,%d %dx%d)\n", b->x, b->y, b->w, b->h);
        break;
    }
    for (k = b->first; k; k = k->next) dump_box(d, k, depth + 1, o);
}

int uw_layout_dump(uw_doc *d, char *out, int max)
{
    lob o;
    o.b = out; o.max = max; o.n = 0;
    if (max > 0) out[0] = 0;
    if (!d || !d->layout_root) return 0;
    dump_box(d, d->layout_root, 0, &o);
    return o.n;
}

int uw_paint_dump(uw_doc *d, char *out, int max)
{
    lob o;
    int i;
    o.b = out; o.max = max; o.n = 0;
    if (max > 0) out[0] = 0;
    if (!d || !d->paint) return 0;
    for (i = 0; i < d->paint->n; i++) {
        const uw_paint_cmd *c = &d->paint->v[i];
        switch (c->cmd) {
        case UW_CMD_RECT:
            lp(&o, "rect (%d,%d %dx%d) #%02x%02x%02x\n", c->x, c->y, c->w, c->h,
               c->color.r, c->color.g, c->color.b);
            break;
        case UW_CMD_BORDER:
            lp(&o, "border (%d,%d %dx%d) #%02x%02x%02x\n", c->x, c->y, c->w, c->h,
               c->color.r, c->color.g, c->color.b);
            break;
        case UW_CMD_BULLET:
            lp(&o, "bullet (%d,%d)\n", c->x, c->y);
            break;
        case UW_CMD_IMAGE:
            lp(&o, "image (%d,%d %dx%d)\n", c->x, c->y, c->w, c->h);
            break;
        case UW_CMD_TEXT:
            lp(&o, "text (%d,%d %dx%d) #%02x%02x%02x %d/%d%s \"%.*s\"\n",
               c->x, c->y, c->w, c->h, c->color.r, c->color.g, c->color.b,
               c->style->font_size, c->style->font_weight,
               c->style->underline ? "u" : "", c->len, c->text);
            break;
        default: break;
        }
    }
    return o.n;
}
