/* ===========================================================================
 * uw_cascade.c - libcss AS unoweb's style pass.
 *
 * Registered through uw_cascade_set() (the additive seam in uw_style.c);
 * when active it replaces matching + cascade + inheritance wholesale and
 * fills each element's fixed uw_style record via uw_style_store(). Layout,
 * paint and the browser are untouched - they read uw_computed() as ever.
 *
 * Sheet model per pass: the SHARED UA stylesheet text (uw_ua_css(), so the
 * two cascades cannot drift) parsed once and cached for the process, plus
 * every <style> element's text as an author sheet, plus each element's
 * style="" attribute as a libcss inline sheet. That is exactly the built-in
 * cascade's input set today; <link> sheets join when the fetch queue lands.
 *
 * Inheritance is css_computed_style_compose() against the parent's COMPOSED
 * style, parent-before-child down the tree, NetSurf's own pattern. Units
 * resolve through css_unit_len2css_px (em/rem/vw against the pass's
 * css_unit_ctx), then land in uw_style as px - except percentages, which
 * stay symbolic (UW_LEN_PCT) because uw_layout resolves them against the
 * containing block, same as the built-in cascade.
 *
 * Any failure makes the PASS return non-zero, and uw_style_document falls
 * back to the built-in cascade: a broken stylesheet degrades to the old
 * renderer, never to an unstyled page.
 * ======================================================================== */
#include <stdlib.h>
#include <string.h>
#include "uw_bridge.h"

/* ---- pass pool (class arrays; see uw_bridge.h) ---------------------------- */
void *uwx_pool_alloc(uwx_ctx *cx, size_t n)
{
    uwx_pool_chunk *c = malloc(sizeof *c + n);
    if (!c) return NULL;
    c->next = cx->pool;
    cx->pool = c;
    return c + 1;
}

void uwx_pool_free_all(uwx_ctx *cx)
{
    while (cx->pool) {
        uwx_pool_chunk *c = cx->pool;
        cx->pool = c->next;
        free(c);
    }
}

/* ---- diagnostics ---------------------------------------------------------- */
static const char *g_status = "";
const char *uwx_libcss_status(void) { return g_status; }

/* ---- stylesheet helpers --------------------------------------------------- */
static css_error resolve_url(void *pw,
        const char *base, lwc_string *rel, lwc_string **abs)
{
    (void)pw; (void)base;
    *abs = lwc_string_ref(rel);       /* no fetch queue yet: keep it symbolic */
    return CSS_OK;
}

static css_stylesheet *parse_sheet(const char *data, size_t len, int is_inline)
{
    css_stylesheet_params params;
    css_stylesheet *sheet;

    memset(&params, 0, sizeof params);
    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_21;
    params.charset = "UTF-8";
    params.url = "";
    params.title = "";
    params.inline_style = is_inline != 0;
    params.resolve = resolve_url;

    if (css_stylesheet_create(&params, &sheet) != CSS_OK) return NULL;
    {   /* CSS_NEEDDATA just means "feed me more"; this is a one-shot feed */
        css_error e = css_stylesheet_append_data(sheet,
                (const uint8_t *)data, len);
        if (e != CSS_OK && e != CSS_NEEDDATA) {
            css_stylesheet_destroy(sheet);
            return NULL;
        }
    }
    if (css_stylesheet_data_done(sheet) != CSS_OK) {
        css_stylesheet_destroy(sheet);
        return NULL;
    }
    return sheet;
}

/* the UA sheet is process-lifetime: parsed once, appended to every pass */
static css_stylesheet *ua_sheet(void)
{
    static css_stylesheet *ua;
    if (!ua) {
        const char *txt = uw_ua_css();
        ua = parse_sheet(txt, strlen(txt), 0);
    }
    return ua;
}

/* ---- computed -> uw_style mapping ----------------------------------------- */
static int to_px(const css_computed_style *st, const css_unit_ctx *uc,
                 css_fixed v, css_unit u)
{ return FIXTOINT(css_unit_len2css_px(st, uc, v, u)); }

static uw_color to_uw_color(css_color c)          /* css_color is AARRGGBB */
{
    uw_color r;
    r.a = (unsigned char)(c >> 24);
    r.r = (unsigned char)(c >> 16);
    r.g = (unsigned char)(c >> 8);
    r.b = (unsigned char)(c);
    return r;
}

static uw_len to_uw_len(const css_computed_style *st, const css_unit_ctx *uc,
                        uint8_t type, uint8_t set_val, uint8_t auto_val,
                        css_fixed v, css_unit u)
{
    uw_len l;
    if (type == auto_val || type == 0) { l.unit = UW_LEN_AUTO; l.v = 0; return l; }
    (void)set_val;
    if (u == CSS_UNIT_PCT) { l.unit = UW_LEN_PCT; l.v = FIXTOINT(v); return l; }
    l.unit = UW_LEN_PX;
    l.v = to_px(st, uc, v, u);
    return l;
}

static unsigned char map_display(uint8_t d)
{
    switch (d) {
    case CSS_DISPLAY_NONE:         return UW_DISP_NONE;
    case CSS_DISPLAY_INLINE:       return UW_DISP_INLINE;
    case CSS_DISPLAY_INLINE_BLOCK: return UW_DISP_INLINE_BLOCK;
    case CSS_DISPLAY_LIST_ITEM:    return UW_DISP_LIST_ITEM;
    default:                       return UW_DISP_BLOCK;
    /* the table/flex display types collapse to BLOCK, which is exactly the
     * built-in UA sheet's treatment of tables today */
    }
}

static void map_style(const css_computed_style *st, const css_unit_ctx *uc,
                      int is_root, uw_style *out)
{
    css_color col;
    css_fixed v;
    css_unit u;
    uint8_t t;
    lwc_string **names;
    int i;

    memset(out, 0, sizeof *out);

    out->display = map_display(css_computed_display(st, is_root != 0));

    css_computed_color(st, &col);
    out->color = to_uw_color(col);

    t = css_computed_background_color(st, &col);
    out->background_color = to_uw_color(col);
    out->has_bg = (out->background_color.a != 0);
    (void)t;

    /* font-size is absolute after compose; resolve whatever unit remains */
    css_computed_font_size(st, &v, &u);
    out->font_size = to_px(st, uc, v, u);
    if (out->font_size <= 0) out->font_size = 14;

    switch (css_computed_font_weight(st)) {
    case CSS_FONT_WEIGHT_BOLD: case CSS_FONT_WEIGHT_BOLDER:
    case CSS_FONT_WEIGHT_600: case CSS_FONT_WEIGHT_700:
    case CSS_FONT_WEIGHT_800: case CSS_FONT_WEIGHT_900:
        out->font_weight = 700; break;
    default:
        out->font_weight = 400; break;
    }

    out->font_style = css_computed_font_style(st) == CSS_FONT_STYLE_ITALIC;

    switch (css_computed_font_family(st, &names)) {
    case CSS_FONT_FAMILY_MONOSPACE: out->font_family = UW_FF_MONO;  break;
    case CSS_FONT_FAMILY_SERIF:     out->font_family = UW_FF_SERIF; break;
    default:                        out->font_family = UW_FF_SANS;  break;
    }

    switch (css_computed_text_align(st)) {
    case CSS_TEXT_ALIGN_CENTER:  out->text_align = UW_ALIGN_CENTER;  break;
    case CSS_TEXT_ALIGN_RIGHT:   out->text_align = UW_ALIGN_RIGHT;   break;
    case CSS_TEXT_ALIGN_JUSTIFY: out->text_align = UW_ALIGN_JUSTIFY; break;
    default:                     out->text_align = UW_ALIGN_LEFT;    break;
    }

    switch (css_computed_white_space(st)) {
    case CSS_WHITE_SPACE_PRE:
    case CSS_WHITE_SPACE_PRE_WRAP:  out->white_space = UW_WS_PRE;    break;
    case CSS_WHITE_SPACE_NOWRAP:    out->white_space = UW_WS_NOWRAP; break;
    default:                        out->white_space = UW_WS_NORMAL; break;
    }

    {   css_fixed vv; css_unit vu;
        switch (css_computed_vertical_align(st, &vv, &vu)) {
        case CSS_VERTICAL_ALIGN_TOP:
        case CSS_VERTICAL_ALIGN_TEXT_TOP:    out->vertical_align = UW_VA_TOP;    break;
        case CSS_VERTICAL_ALIGN_MIDDLE:      out->vertical_align = UW_VA_MIDDLE; break;
        case CSS_VERTICAL_ALIGN_BOTTOM:
        case CSS_VERTICAL_ALIGN_TEXT_BOTTOM: out->vertical_align = UW_VA_BOTTOM; break;
        case CSS_VERTICAL_ALIGN_SUB:         out->vertical_align = UW_VA_SUB;    break;
        case CSS_VERTICAL_ALIGN_SUPER:       out->vertical_align = UW_VA_SUPER;  break;
        default:                             out->vertical_align = UW_VA_BASELINE; break;
        }
    }

    out->underline =
        (css_computed_text_decoration(st) & CSS_TEXT_DECORATION_UNDERLINE) != 0;

    switch (css_computed_list_style_type(st)) {
    case CSS_LIST_STYLE_TYPE_DISC:
    case CSS_LIST_STYLE_TYPE_CIRCLE:
    case CSS_LIST_STYLE_TYPE_SQUARE:  out->list_bullet = 1; break;
    case CSS_LIST_STYLE_TYPE_DECIMAL:
    case CSS_LIST_STYLE_TYPE_DECIMAL_LEADING_ZERO:
    case CSS_LIST_STYLE_TYPE_LOWER_ALPHA:
    case CSS_LIST_STYLE_TYPE_LOWER_ROMAN:
    case CSS_LIST_STYLE_TYPE_UPPER_ALPHA:
    case CSS_LIST_STYLE_TYPE_UPPER_ROMAN: out->list_bullet = 2; break;
    default:                          out->list_bullet = 0; break;
    }

    t = css_computed_line_height(st, &v, &u);
    if (t == CSS_LINE_HEIGHT_DIMENSION)
        out->line_height = u == CSS_UNIT_PCT
            ? out->font_size * FIXTOINT(v) / 100
            : to_px(st, uc, v, u);
    else if (t == CSS_LINE_HEIGHT_NUMBER)
        out->line_height = (int)(FIXTOFLT(v) * (float)out->font_size);
    else
        out->line_height = 0;          /* normal: layout derives from size */

    t = css_computed_margin_top(st, &v, &u);
    out->margin[UW_TOP] = to_uw_len(st, uc, t, CSS_MARGIN_SET, CSS_MARGIN_AUTO, v, u);
    t = css_computed_margin_right(st, &v, &u);
    out->margin[UW_RIGHT] = to_uw_len(st, uc, t, CSS_MARGIN_SET, CSS_MARGIN_AUTO, v, u);
    t = css_computed_margin_bottom(st, &v, &u);
    out->margin[UW_BOTTOM] = to_uw_len(st, uc, t, CSS_MARGIN_SET, CSS_MARGIN_AUTO, v, u);
    t = css_computed_margin_left(st, &v, &u);
    out->margin[UW_LEFT] = to_uw_len(st, uc, t, CSS_MARGIN_SET, CSS_MARGIN_AUTO, v, u);

    /* padding has no auto; unset means 0 */
    t = css_computed_padding_top(st, &v, &u);
    out->padding[UW_TOP] = to_uw_len(st, uc, t, CSS_PADDING_SET, 0xff, v, u);
    t = css_computed_padding_right(st, &v, &u);
    out->padding[UW_RIGHT] = to_uw_len(st, uc, t, CSS_PADDING_SET, 0xff, v, u);
    t = css_computed_padding_bottom(st, &v, &u);
    out->padding[UW_BOTTOM] = to_uw_len(st, uc, t, CSS_PADDING_SET, 0xff, v, u);
    t = css_computed_padding_left(st, &v, &u);
    out->padding[UW_LEFT] = to_uw_len(st, uc, t, CSS_PADDING_SET, 0xff, v, u);
    for (i = 0; i < 4; i++)
        if (out->padding[i].unit == UW_LEN_AUTO)
            { out->padding[i].unit = UW_LEN_PX; out->padding[i].v = 0; }
    for (i = 0; i < 4; i++)
        if (out->margin[i].unit == UW_LEN_AUTO)
            { out->margin[i].unit = UW_LEN_PX; out->margin[i].v = 0; }
    /* NOTE: margin auto-vs-0: libcss reports UNSET margins as SET 0 after
     * compose, and true `margin: auto` as AUTO - but uw_layout's auto
     * centring only reads width, so collapsing auto margins to 0 px matches
     * the built-in cascade's current behaviour exactly. Revisit with
     * layout's margin-auto support. */

    {   static uint8_t (*const wfn[4])(const css_computed_style *,
                                       css_fixed *, css_unit *) = {
            css_computed_border_top_width, css_computed_border_right_width,
            css_computed_border_bottom_width, css_computed_border_left_width };
        static uint8_t (*const sfn[4])(const css_computed_style *) = {
            css_computed_border_top_style, css_computed_border_right_style,
            css_computed_border_bottom_style, css_computed_border_left_style };
        static uint8_t (*const cfn[4])(const css_computed_style *,
                                       css_color *) = {
            css_computed_border_top_color, css_computed_border_right_color,
            css_computed_border_bottom_color, css_computed_border_left_color };
        for (i = 0; i < 4; i++) {
            uint8_t bs = sfn[i](st);
            if (bs == CSS_BORDER_STYLE_NONE || bs == CSS_BORDER_STYLE_HIDDEN) {
                out->border_style[i] = UW_BS_NONE;
                out->border_width[i] = 0;
            } else {
                out->border_style[i] = UW_BS_SOLID;   /* every visible style
                                                       * paints solid today */
                wfn[i](st, &v, &u);
                out->border_width[i] = to_px(st, uc, v, u);
            }
            cfn[i](st, &col);
            out->border_color[i] = to_uw_color(col);
        }
    }

    t = css_computed_width(st, &v, &u);
    out->width = to_uw_len(st, uc, t, CSS_WIDTH_SET, CSS_WIDTH_AUTO, v, u);
    t = css_computed_height(st, &v, &u);
    out->height = to_uw_len(st, uc, t, CSS_HEIGHT_SET, CSS_HEIGHT_AUTO, v, u);
}

/* ---- the pass ------------------------------------------------------------- */
typedef struct {
    uwx_ctx       cx;
    css_select_ctx *sel;
    css_unit_ctx   units;
    css_media      media;
    int            failed;
} pass;

static void style_rec(pass *ps, uw_node *n,
                      const css_computed_style *parent_st)
{
    uw_node *c;
    const css_computed_style *down = parent_st;
    css_select_results *results = NULL;
    css_computed_style *composed = NULL;

    if (ps->failed) return;

    if (uw_type(n) == UW_NODE_ELEMENT) {
        css_stylesheet *inl = NULL;
        const char *inl_txt = uw_attr(ps->cx.doc, n, "style");
        uw_style uwst;
        int is_root = uw_parent(n) &&
                      uw_type(uw_parent(n)) != UW_NODE_ELEMENT;

        if (inl_txt && *inl_txt)
            inl = parse_sheet(inl_txt, strlen(inl_txt), 1);

        if (css_select_style(ps->sel, n, &ps->units, &ps->media, inl,
                             &uwx_select_handler, &ps->cx,
                             &results) != CSS_OK) {
            if (inl) css_stylesheet_destroy(inl);
            g_status = "select failed";
            ps->failed = 1;
            return;
        }
        if (inl) css_stylesheet_destroy(inl);

        if (parent_st) {
            if (css_computed_style_compose(parent_st,
                    results->styles[CSS_PSEUDO_ELEMENT_NONE],
                    &ps->units, &composed) != CSS_OK) {
                css_select_results_destroy(results);
                g_status = "compose failed";
                ps->failed = 1;
                return;
            }
            down = composed;
        } else {
            down = results->styles[CSS_PSEUDO_ELEMENT_NONE];
        }

        map_style(down, &ps->units, is_root, &uwst);
        if (uw_style_store(ps->cx.doc, n, &uwst) != 0) {
            g_status = "store failed";
            ps->failed = 1;
        }
    }

    for (c = uw_first_child(n); c && !ps->failed; c = uw_next_sibling(c))
        style_rec(ps, c, down);

    /* lifetimes: `results` must outlive the subtree when the root element's
     * own style (no compose) was passed down */
    if (composed) css_computed_style_destroy(composed);
    if (results) css_select_results_destroy(results);
}

static int uwx_cascade(void *user, uw_doc *d, int vw, int vh)
{
    pass ps;
    css_stylesheet *authors[16];
    int nauthors = 0, i, rc = -1;
    uw_node *n;

    (void)user;
    g_status = "";

    memset(&ps, 0, sizeof ps);
    ps.cx.doc = d;
    ps.media.type = CSS_MEDIA_SCREEN;
    ps.units.viewport_width  = INTTOFIX(vw);
    ps.units.viewport_height = INTTOFIX(vh);
    ps.units.font_size_default = INTTOFIX(14);   /* the built-in default   */
    ps.units.font_size_minimum = INTTOFIX(6);
    ps.units.device_dpi        = INTTOFIX(96);

    if (!ua_sheet()) { g_status = "ua sheet failed"; return -1; }
    if (css_select_ctx_create(&ps.sel) != CSS_OK) {
        g_status = "ctx failed";
        return -1;
    }
    if (css_select_ctx_append_sheet(ps.sel, ua_sheet(),
                                    CSS_ORIGIN_UA, NULL) != CSS_OK) {
        g_status = "ua append failed";
        goto out;
    }

    /* every <style> element, in document order (same set the built-in
     * cascade collects via uw_add_inline_sheets) */
    for (n = uw_next_in_order(uw_document(d), uw_document(d)); n;
         n = uw_next_in_order(n, uw_document(d))) {
        uw_node *t;
        const char *txt;
        int tl = 0;
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        if (strcmp(uw_tag_name(d, n), "style")) continue;
        t = uw_first_child(n);
        txt = t ? uw_text(t, &tl) : NULL;
        if (!txt || !tl) continue;
        if (nauthors < (int)(sizeof authors / sizeof authors[0])) {
            css_stylesheet *s = parse_sheet(txt, (size_t)tl, 0);
            if (s) {
                if (css_select_ctx_append_sheet(ps.sel, s, CSS_ORIGIN_AUTHOR,
                                                NULL) == CSS_OK)
                    authors[nauthors++] = s;
                else
                    css_stylesheet_destroy(s);
            }
        }
    }

    style_rec(&ps, uw_document(d), NULL);
    rc = ps.failed ? -1 : 0;

out:
    css_select_ctx_destroy(ps.sel);
    for (i = 0; i < nauthors; i++)
        css_stylesheet_destroy(authors[i]);
    uwx_nodedata_drop_all(&ps.cx);       /* before the pool: entries live there */
    uwx_pool_free_all(&ps.cx);
    return rc;
}

/* ---- registration ---------------------------------------------------------- */
void uwx_libcss_register(void)   { uw_cascade_set(uwx_cascade, NULL); }
void uwx_libcss_unregister(void) { uw_cascade_set(NULL, NULL); }
