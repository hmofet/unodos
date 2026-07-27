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

/* ---- inline content: greedy word wrap into line boxes --------------------
 * This is NOT full inline formatting (nested inline boxes carrying their own
 * borders across line breaks, vertical-align, justification - all M4). It is
 * enough to give a block a REAL height, without which every auto height above
 * it would be a guess and the golden geometry would mean nothing. */
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
} inline_ctx;

static void line_break(inline_ctx *ic)
{
    if (ic->line) {
        ic->line->h = ic->line_h;
        ic->line->w = ic->x;
    }
    ic->y += ic->line_h;
    ic->x = 0;
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
    tb->text = t; tb->tlen = len;
    tb->x = ic->content_x + ic->x;
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
    if (ic->x > 0 && ic->x + iw > ic->avail) line_break(ic);
    ensure_line(ic, s);
    if (!ic->line) return;
    b = box_new(ic->d, UW_BOX_IMAGE, n, s);
    if (!b) return;
    b->image = handle;
    b->x = ic->content_x + ic->x;
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
static void layout_block(uw_doc *d, uw_box *b, int cb_width, int x, int y);

static uw_box *build_block(uw_doc *d, uw_node *n, const uw_style *s)
{
    uw_box *b = box_new(d, UW_BOX_BLOCK, n, s);
    return b;
}

static void layout_children(uw_doc *d, uw_box *b, int content_x, int content_y,
                            int content_w, int *out_h)
{
    uw_node *c;
    int cy = content_y;
    int prev_margin = 0;            /* for collapsing with the next sibling */
    int first = 1;
    int any_block = 0;

    for (c = uw_first_child(b->node); c; c = uw_next_sibling(c)) {
        const uw_style *cs;
        if (uw_type(c) != UW_NODE_ELEMENT) continue;
        cs = uw_computed(c);
        if (!cs || cs->display == UW_DISP_NONE) continue;
        if (cs->display != UW_DISP_BLOCK && cs->display != UW_DISP_LIST_ITEM) continue;
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
            layout_block(d, cb, content_w, content_x, cy);
            cy = cb->y + cb->h;
            prev_margin = len_px(cs->margin[UW_BOTTOM], content_w);
            cy += prev_margin;
            any_block = 1;
        }
    }
    if (any_block) { *out_h = cy - content_y; return; }

    /* no block children: this box establishes an inline formatting context */
    {   inline_ctx ic;
        memset(&ic, 0, sizeof ic);
        ic.d = d; ic.block = b; ic.avail = content_w > 0 ? content_w : 1;
        ic.content_x = content_x; ic.content_y = content_y;
        ic.cur_elem = b->node;
        flow_inline(&ic, b->node, b->style);
        if (ic.line) { ic.line->h = ic.line_h; ic.line->w = ic.x; ic.y += ic.line_h; }
        *out_h = ic.y;
    }
}

static void layout_block(uw_doc *d, uw_box *b, int cb_width, int x, int y)
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

    layout_children(d, b, b->x + b->bl + b->pl, b->y + b->bt + b->pt,
                    content_w, &content_h);

    if (s->height.unit != UW_LEN_AUTO) content_h = len_px(s->height, 0);
    b->h = content_h + b->bt + b->bb + b->pt + b->pb;
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
                 len_px(bs->margin[UW_TOP], width));
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
    p->v[p->n++] = *c;
    return 0;
}

static void paint_box(uw_doc *d, uw_box *b)
{
    uw_paint_cmd c;
    const uw_style *s = b->style;
    uw_box *k;
    memset(&c, 0, sizeof c);

    if (b->type == UW_BOX_IMAGE) {
        c.cmd = UW_CMD_IMAGE;
        c.x = b->x; c.y = b->y; c.w = b->w; c.h = b->h;
        c.image = b->image;
        c.style = s;
        pl_push(d, &c);
        return;
    }
    if (b->type == UW_BOX_TEXT) {
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
