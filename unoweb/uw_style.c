/* ===========================================================================
 * unoweb style - the UA stylesheet, value parsing, and the cascade.
 *
 * A computed style is a FIXED struct, so inheritance is a struct copy and
 * damage detection will be a memcmp. Nothing here allocates per property.
 * ======================================================================== */
#include "uw_int.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/* ---- the UA stylesheet ---------------------------------------------------
 * The browser's own defaults, expressed as CSS and run through the same
 * parser as author sheets. Keeping it as text rather than hand-built structs
 * means the defaults and the cascade cannot drift apart. */
static const char UA_CSS[] =
"html,body,div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,pre,blockquote,section,article,"
"header,footer,nav,main,aside,figure,figcaption,table,form,hr,dl,dt,dd"
"{display:block}"
"head,style,script,title,meta,link{display:none}"
/* colour on html AND body: the default text colour must come from the SHEET,
 * not a cascade's hardcoded root default - the libcss engine computes the
 * spec initial (black) for anything the sheet leaves unset, and parity
 * between the two cascades is asserted by csslib/test/css_cascade_test.c */
"html{color:#1e2028}"
"body{margin:8px;color:#1e2028;font-size:14px}"
"h1{font-size:28px;font-weight:700;margin:14px 0;color:#14285a}"
"h2{font-size:21px;font-weight:700;margin:12px 0;color:#14285a}"
"h3{font-size:17px;font-weight:700;margin:10px 0;color:#14285a}"
"h4,h5,h6{font-size:14px;font-weight:700;margin:9px 0;color:#14285a}"
"p{margin:9px 0}"
"ul,ol{margin:9px 0;padding:0 0 0 24px}"
"li{display:list-item;list-style-type:disc}"
"ol>li{list-style-type:decimal}"
"b,strong{font-weight:700}"
"i,em{font-style:italic}"
"a{color:#285ad2;text-decoration:underline}"
"code,tt,kbd,samp{font-family:monospace;color:#962828;background:#ebebe6}"
"pre{font-family:monospace;color:#962828;background:#ebebe6;"
     "white-space:pre;margin:9px 0}"
"blockquote{margin:9px 0;padding:0 0 0 12px;color:#5a6478;"
           "border-left:2px solid #c8c8c3}"
"hr{margin:8px 0;border-top:1px solid #c8c8c3}"
"dd{margin:0 0 0 24px}"
/* Real table formatting (M6). Before this, td,th were display:block and
 * every table on the web rendered as one column of stacked cells. */
"table{display:table;margin:6px 0;border-spacing:2px}"
"thead,tbody,tfoot{display:table-row-group}"
"tr{display:table-row}"
"td,th{display:table-cell;padding:2px 4px}"
"th{font-weight:700;text-align:center}";

/* ---- value helpers -------------------------------------------------------- */
static int is_ws(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }

static const char *skipws(const char *s) { while (*s && is_ws((unsigned char)*s)) s++; return s; }

static int tok_at(const char *s, const char **end)
{
    const char *b = skipws(s);
    const char *e = b;
    int depth = 0;
    while (*e && (depth || !is_ws((unsigned char)*e))) {
        if (*e == '(') depth++;
        else if (*e == ')') { if (depth) depth--; }
        e++;
    }
    *end = e;
    return (int)(e - b);
}

/* CSS numbers, parsed here rather than with strtod: pc64's libc has no
 * strtod, and borrowing unojs's would make the web core depend on the JS
 * engine - exactly the coupling this subsystem exists to avoid. CSS numbers
 * are simple (optional sign, digits, optional fraction); exponent notation is
 * legal in the grammar but does not occur in real stylesheets. */
static double uw_strtod(const char *s, char **end)
{
    const char *p = s;
    double v = 0.0, frac = 0.0, scale = 1.0;
    int neg = 0, any = 0;
    while (*p == ' ' || *p == '	') p++;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10.0 + (*p - '0'); p++; any = 1; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') { frac = frac * 10.0 + (*p - '0'); scale *= 10.0; p++; any = 1; }
        v += frac / scale;
    }
    if (end) *end = (char *)(any ? p : s);
    return neg ? -v : v;
}

static int keyword_is(const char *s, int len, const char *kw)
{ return uw_ieq(s, len, kw, (int)strlen(kw)); }

static int hexv(int c)
{ if (c>='0'&&c<='9') return c-'0';
  if (c>='a'&&c<='f') return c-'a'+10;
  if (c>='A'&&c<='F') return c-'A'+10;
  return -1; }

static const struct { const char *n; unsigned char r,g,b; } NAMED[] = {
    {"black",0,0,0},{"white",255,255,255},{"red",255,0,0},{"green",0,128,0},
    {"blue",0,0,255},{"gray",128,128,128},{"grey",128,128,128},
    {"silver",192,192,192},{"maroon",128,0,0},{"olive",128,128,0},
    {"lime",0,255,0},{"aqua",0,255,255},{"cyan",0,255,255},{"teal",0,128,128},
    {"navy",0,0,128},{"fuchsia",255,0,255},{"magenta",255,0,255},
    {"purple",128,0,128},{"yellow",255,255,0},{"orange",255,165,0},
    {"pink",255,192,203},{"brown",165,42,42},{"gold",255,215,0},
    {"transparent",0,0,0},
    {NULL,0,0,0}
};

static int parse_color(const char *s, int len, uw_color *out)
{
    int i;
    s = skipws(s);
    if (len <= 0) len = (int)strlen(s);
    if (*s == '#') {
        int n = 0, v[8];
        for (i = 1; i < len && n < 8; i++) {
            int h = hexv((unsigned char)s[i]);
            if (h < 0) break;
            v[n++] = h;
        }
        if (n >= 6) { out->r=(unsigned char)(v[0]*16+v[1]);
                      out->g=(unsigned char)(v[2]*16+v[3]);
                      out->b=(unsigned char)(v[4]*16+v[5]); out->a=255; return 1; }
        if (n >= 3) { out->r=(unsigned char)(v[0]*17); out->g=(unsigned char)(v[1]*17);
                      out->b=(unsigned char)(v[2]*17); out->a=255; return 1; }
        return 0;
    }
    if (len > 4 && uw_ieq(s, 4, "rgb(", 4)) {
        int c[4] = {0,0,0,255}, k = 0;
        const char *p = s + 4;
        while (*p && k < 4) {
            while (*p && !((*p>='0'&&*p<='9') || *p=='.')) { if (*p==')') break; p++; }
            if (!*p || *p==')') break;
            c[k++] = atoi(p);
            while (*p && ((*p>='0'&&*p<='9')||*p=='.')) p++;
        }
        out->r=(unsigned char)c[0]; out->g=(unsigned char)c[1];
        out->b=(unsigned char)c[2]; out->a=255;
        return 1;
    }
    for (i = 0; NAMED[i].n; i++)
        if (keyword_is(s, len, NAMED[i].n)) {
            out->r = NAMED[i].r; out->g = NAMED[i].g; out->b = NAMED[i].b;
            out->a = keyword_is(s, len, "transparent") ? 0 : 255;
            return 1;
        }
    return 0;
}

/* A length. `em` resolves against the element's own font size, `rem` against
 * the root's - both known by the time this runs, so they collapse to px here
 * and never reach layout. Percentages do NOT: they need a containing block. */
static int parse_len(const char *s, int len, int em_px, int root_px, uw_len *out)
{
    double v;
    const char *p = skipws(s);
    char *endp;
    if (len <= 0) len = (int)strlen(p);
    if (keyword_is(p, len, "auto")) { out->unit = UW_LEN_AUTO; out->v = 0; return 1; }
    if (!((*p>='0'&&*p<='9') || *p=='-' || *p=='+' || *p=='.')) return 0;
    v = uw_strtod(p, &endp);
    if (endp == p) return 0;
    p = skipws(endp);
    if (!strncmp(p, "px", 2) || !*p || is_ws((unsigned char)*p) || *p==';') {
        out->unit = UW_LEN_PX; out->v = (int)(v + (v < 0 ? -0.5 : 0.5)); return 1;
    }
    if (*p == '%') { out->unit = UW_LEN_PCT; out->v = (int)(v + 0.5); return 1; }
    if (!strncmp(p, "em", 2)) { out->unit = UW_LEN_PX; out->v = (int)(v * em_px + 0.5); return 1; }
    if (!strncmp(p, "rem", 3)) { out->unit = UW_LEN_PX; out->v = (int)(v * root_px + 0.5); return 1; }
    if (!strncmp(p, "pt", 2)) { out->unit = UW_LEN_PX; out->v = (int)(v * 96.0 / 72.0 + 0.5); return 1; }
    /* an unknown unit is not a length; leaving it unset is better than
     * guessing a magnitude that would lay the page out wrongly */
    return 0;
}

/* Expand a 1-4 value box shorthand (margin/padding) into the four sides. */
static void parse_box(const char *v, int em, int root, uw_len out[4])
{
    uw_len parts[4];
    int n = 0;
    const char *p = v, *e;
    while (n < 4) {
        int l = tok_at(p, &e);
        if (!l) break;
        if (!parse_len(skipws(p), l, em, root, &parts[n])) { parts[n].unit = UW_LEN_PX; parts[n].v = 0; }
        n++;
        p = e;
    }
    if (!n) return;
    if (n == 1) { out[0]=out[1]=out[2]=out[3]=parts[0]; return; }
    if (n == 2) { out[UW_TOP]=out[UW_BOTTOM]=parts[0]; out[UW_RIGHT]=out[UW_LEFT]=parts[1]; return; }
    if (n == 3) { out[UW_TOP]=parts[0]; out[UW_RIGHT]=out[UW_LEFT]=parts[1];
                  out[UW_BOTTOM]=parts[2]; return; }
    out[UW_TOP]=parts[0]; out[UW_RIGHT]=parts[1]; out[UW_BOTTOM]=parts[2]; out[UW_LEFT]=parts[3];
}

/* `border: 1px solid #ccc` in any order. */
static void parse_border(const char *v, int em, int root, uw_style *st, int side_mask)
{
    const char *p = v, *e;
    int width = -1, style = -1, i;
    uw_color col;
    int have_col = 0;
    for (;;) {
        int l = tok_at(p, &e);
        const char *t = skipws(p);
        if (!l) break;
        if (keyword_is(t, l, "none") || keyword_is(t, l, "hidden")) style = UW_BS_NONE;
        else if (keyword_is(t, l, "solid") || keyword_is(t, l, "dashed") ||
                 keyword_is(t, l, "dotted") || keyword_is(t, l, "double")) style = UW_BS_SOLID;
        else { uw_len L;
               if (parse_len(t, l, em, root, &L) && L.unit == UW_LEN_PX) width = L.v;
               else if (parse_color(t, l, &col)) have_col = 1; }
        p = e;
    }
    for (i = 0; i < 4; i++) {
        if (!(side_mask & (1 << i))) continue;
        if (style >= 0) st->border_style[i] = (unsigned char)style;
        if (width >= 0) st->border_width[i] = width;
        else if (style == UW_BS_SOLID && st->border_width[i] == 0) st->border_width[i] = 1;
        if (have_col) st->border_color[i] = col;
        if (style == UW_BS_NONE) st->border_width[i] = 0;
    }
}

/* ---- applying one declaration -------------------------------------------- */
static void apply_decl(uw_doc *d, uw_style *st, const uw_style *parent,
                       const char *prop, const char *v, int root_px)
{
    int em = st->font_size ? st->font_size : 14;
    int vlen = (int)strlen(v);
    (void)parent;
    if (!strcmp(prop, "display")) {
        if (keyword_is(v, vlen, "block")) st->display = UW_DISP_BLOCK;
        else if (keyword_is(v, vlen, "inline")) st->display = UW_DISP_INLINE;
        else if (keyword_is(v, vlen, "inline-block")) st->display = UW_DISP_INLINE_BLOCK;
        else if (keyword_is(v, vlen, "list-item")) st->display = UW_DISP_LIST_ITEM;
        else if (keyword_is(v, vlen, "none")) st->display = UW_DISP_NONE;
        else if (keyword_is(v, vlen, "table")) st->display = UW_DISP_TABLE;
        else if (keyword_is(v, vlen, "inline-table")) st->display = UW_DISP_TABLE;
        else if (keyword_is(v, vlen, "table-row")) st->display = UW_DISP_TABLE_ROW;
        else if (keyword_is(v, vlen, "table-cell")) st->display = UW_DISP_TABLE_CELL;
        else if (keyword_is(v, vlen, "table-row-group") ||
                 keyword_is(v, vlen, "table-header-group") ||
                 keyword_is(v, vlen, "table-footer-group"))
            st->display = UW_DISP_TABLE_ROW_GROUP;
        return;
    }
    if (!strcmp(prop, "color")) { parse_color(v, vlen, &st->color); return; }
    if (!strcmp(prop, "background-color") || !strcmp(prop, "background")) {
        if (parse_color(v, vlen, &st->background_color)) st->has_bg = st->background_color.a != 0;
        return;
    }
    if (!strcmp(prop, "font-size")) {
        uw_len L;
        if (keyword_is(v, vlen, "smaller")) { st->font_size = em * 5 / 6; return; }
        if (keyword_is(v, vlen, "larger"))  { st->font_size = em * 6 / 5; return; }
        /* font-size:em resolves against the PARENT's size, not this element's */
        if (parse_len(v, vlen, parent ? parent->font_size : em, root_px, &L)) {
            if (L.unit == UW_LEN_PX) st->font_size = L.v > 0 ? L.v : 1;
            else if (L.unit == UW_LEN_PCT && parent) st->font_size = parent->font_size * L.v / 100;
        }
        return;
    }
    if (!strcmp(prop, "font-weight")) {
        if (keyword_is(v, vlen, "bold") || keyword_is(v, vlen, "bolder")) st->font_weight = 700;
        else if (keyword_is(v, vlen, "normal") || keyword_is(v, vlen, "lighter")) st->font_weight = 400;
        else { int w = atoi(v); if (w >= 100 && w <= 900) st->font_weight = w >= 600 ? 700 : 400; }
        return;
    }
    if (!strcmp(prop, "font-style")) {
        st->font_style = (unsigned char)(keyword_is(v, vlen, "italic") ||
                                         keyword_is(v, vlen, "oblique"));
        return;
    }
    if (!strcmp(prop, "font-family")) {
        /* map to the three faces the system actually ships */
        if (strstr(v, "monospace") || strstr(v, "Courier") || strstr(v, "mono"))
            st->font_family = UW_FF_MONO;
        else if (strstr(v, "serif") && !strstr(v, "sans-serif"))
            st->font_family = UW_FF_SERIF;
        else st->font_family = UW_FF_SANS;
        return;
    }
    if (!strcmp(prop, "line-height")) {
        uw_len L;
        char *endp;
        double num = uw_strtod(skipws(v), &endp);
        if (endp != skipws(v) && (!*skipws(endp))) { st->line_height = (int)(num * st->font_size + 0.5); return; }
        if (parse_len(v, vlen, em, root_px, &L)) {
            if (L.unit == UW_LEN_PX) st->line_height = L.v;
            else if (L.unit == UW_LEN_PCT) st->line_height = st->font_size * L.v / 100;
        }
        return;
    }
    if (!strcmp(prop, "text-align")) {
        st->text_align = (unsigned char)(keyword_is(v, vlen, "center") ? UW_ALIGN_CENTER :
                                         keyword_is(v, vlen, "right")  ? UW_ALIGN_RIGHT :
                                         keyword_is(v, vlen, "justify")? UW_ALIGN_JUSTIFY :
                                                                         UW_ALIGN_LEFT);
        return;
    }
    if (!strcmp(prop, "vertical-align")) {
        /* NOT inherited (CSS says so), and the length/percentage forms fall
         * back to baseline rather than pretending to shift by a value the
         * line box cannot honour yet. */
        st->vertical_align = (unsigned char)(
            keyword_is(v, vlen, "top")         ? UW_VA_TOP :
            keyword_is(v, vlen, "middle")      ? UW_VA_MIDDLE :
            keyword_is(v, vlen, "bottom")      ? UW_VA_BOTTOM :
            keyword_is(v, vlen, "sub")         ? UW_VA_SUB :
            keyword_is(v, vlen, "super")       ? UW_VA_SUPER :
            keyword_is(v, vlen, "text-top")    ? UW_VA_TOP :
            keyword_is(v, vlen, "text-bottom") ? UW_VA_BOTTOM :
                                                 UW_VA_BASELINE);
        return;
    }
    if (!strcmp(prop, "text-decoration") || !strcmp(prop, "text-decoration-line")) {
        st->underline = (unsigned char)(strstr(v, "underline") != NULL);
        return;
    }
    if (!strcmp(prop, "white-space")) {
        st->white_space = (unsigned char)(keyword_is(v, vlen, "pre") ||
                                          keyword_is(v, vlen, "pre-wrap") ? UW_WS_PRE :
                                          keyword_is(v, vlen, "nowrap") ? UW_WS_NOWRAP : UW_WS_NORMAL);
        return;
    }
    if (!strcmp(prop, "list-style-type") || !strcmp(prop, "list-style")) {
        st->list_bullet = (unsigned char)(keyword_is(v, vlen, "none") ? 0 :
                                          strstr(v, "decimal") ? 2 : 1);
        return;
    }
    if (!strcmp(prop, "margin"))  { parse_box(v, em, root_px, st->margin); return; }
    if (!strcmp(prop, "padding")) { parse_box(v, em, root_px, st->padding); return; }
    {   static const char *const sides[4] = { "top", "right", "bottom", "left" };
        int i;
        for (i = 0; i < 4; i++) {
            char nm[24];
            snprintf(nm, sizeof nm, "margin-%s", sides[i]);
            if (!strcmp(prop, nm)) { parse_len(v, vlen, em, root_px, &st->margin[i]); return; }
            snprintf(nm, sizeof nm, "padding-%s", sides[i]);
            if (!strcmp(prop, nm)) { parse_len(v, vlen, em, root_px, &st->padding[i]); return; }
            snprintf(nm, sizeof nm, "border-%s", sides[i]);
            if (!strcmp(prop, nm)) { parse_border(v, em, root_px, st, 1 << i); return; }
            snprintf(nm, sizeof nm, "border-%s-width", sides[i]);
            if (!strcmp(prop, nm)) { uw_len L;
                if (parse_len(v, vlen, em, root_px, &L) && L.unit == UW_LEN_PX)
                    st->border_width[i] = L.v;
                return; }
            snprintf(nm, sizeof nm, "border-%s-color", sides[i]);
            if (!strcmp(prop, nm)) { parse_color(v, vlen, &st->border_color[i]); return; }
        }
    }
    if (!strcmp(prop, "border")) { parse_border(v, em, root_px, st, 0xF); return; }
    if (!strcmp(prop, "border-width")) {
        uw_len b[4]; int i;
        for (i = 0; i < 4; i++) { b[i].unit = UW_LEN_PX; b[i].v = st->border_width[i]; }
        parse_box(v, em, root_px, b);
        for (i = 0; i < 4; i++) st->border_width[i] = b[i].unit == UW_LEN_PX ? b[i].v : 0;
        return;
    }
    if (!strcmp(prop, "border-color")) {
        uw_color c; int i;
        if (parse_color(v, vlen, &c)) for (i = 0; i < 4; i++) st->border_color[i] = c;
        return;
    }
    if (!strcmp(prop, "border-style")) {
        int i, s = keyword_is(v, vlen, "none") ? UW_BS_NONE : UW_BS_SOLID;
        for (i = 0; i < 4; i++) {
            st->border_style[i] = (unsigned char)s;
            if (s == UW_BS_NONE) st->border_width[i] = 0;
        }
        return;
    }
    if (!strcmp(prop, "width"))  { parse_len(v, vlen, em, root_px, &st->width); return; }
    if (!strcmp(prop, "height")) { parse_len(v, vlen, em, root_px, &st->height); return; }
    (void)d;
    /* Anything else is simply not supported. Silently ignoring it is correct:
     * CSS is designed so an unknown declaration is skipped, and the page keeps
     * working with what did apply. */
}

/* ---- the cascade ---------------------------------------------------------- */
typedef struct { uw_rule *r; int origin, important, spec, order; } match;

static void sort_matches(match *m, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        match key = m[i];
        for (j = i; j > 0; j--) {
            match *p = &m[j-1];
            /* ascending priority: the last one applied wins */
            int a = p->important * 4 + (p->important ? (p->origin == UW_ORIGIN_UA ? 1 : 0)
                                                     : (p->origin == UW_ORIGIN_UA ? 0 : 1));
            int b = key.important * 4 + (key.important ? (key.origin == UW_ORIGIN_UA ? 1 : 0)
                                                       : (key.origin == UW_ORIGIN_UA ? 0 : 1));
            if (a < b) break;
            if (a == b && p->spec < key.spec) break;
            if (a == b && p->spec == key.spec && p->order <= key.order) break;
            m[j] = m[j-1];
        }
        m[j] = key;
    }
}

static void inherit(uw_style *st, const uw_style *parent)
{
    /* Only the inherited properties come from the parent; everything else
     * starts from the initial value (the struct is zeroed by the caller). */
    st->color = parent->color;
    st->font_size = parent->font_size;
    st->font_weight = parent->font_weight;
    st->font_style = parent->font_style;
    st->font_family = parent->font_family;
    st->line_height = parent->line_height;
    st->text_align = parent->text_align;
    st->white_space = parent->white_space;
    st->underline = parent->underline;
    st->list_bullet = parent->list_bullet;
}

static void style_element(uw_doc *d, uw_node *n, const uw_style *parent, int root_px)
{
    uw_style *st = (uw_style *)n->style;
    match m[128];
    int nm = 0;
    uw_sheet *sh;
    if (!st) {
        st = (uw_style *)uw_arena(d, sizeof *st);
        if (!st) return;
        n->style = st;
    }
    memset(st, 0, sizeof *st);
    if (parent) inherit(st, parent);
    else { st->color.r = 0x1e; st->color.g = 0x20; st->color.b = 0x28; st->color.a = 255;
           st->font_size = 14; st->font_weight = 400; }
    /* The zeroed struct makes every uw_len AUTO, but that is only the correct
     * initial value for width/height: margins, padding and borders start at
     * zero LENGTH. Left as-is, every element reported padding=auto. */
    {   int i;
        for (i = 0; i < 4; i++) {
            st->margin[i].unit = UW_LEN_PX;  st->margin[i].v = 0;
            st->padding[i].unit = UW_LEN_PX; st->padding[i].v = 0;
        } }
    st->width.unit = UW_LEN_AUTO;
    st->height.unit = UW_LEN_AUTO;

    for (sh = d->sheets; sh; sh = uw_sheet_next(sh)) {
        uw_rule *r;
        for (r = uw_sheet_rules(sh); r; r = uw_rule_next(r)) {
            if (nm >= 128) break;
            if (!uw_rule_matches(d, n, r)) continue;
            m[nm].r = r;
            m[nm].origin = uw_sheet_origin(sh);
            m[nm].important = 0;
            m[nm].spec = uw_rule_spec(r);
            m[nm].order = uw_rule_order(r);
            nm++;
        }
    }
    sort_matches(m, nm);
    {   int i;
        /* two passes: normal declarations, then !important ones, which is the
         * cheap way to honour importance without splitting every rule */
        int pass;
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < nm; i++) {
                uw_decl *dc;
                for (dc = uw_rule_decls(m[i].r); dc; dc = uw_decl_next(dc))
                    if (uw_decl_important(dc) == pass)
                        apply_decl(d, st, parent, uw_atom_name(d, uw_decl_prop(dc)),
                                   uw_decl_value(dc), root_px);
            }
    }
    /* the style="" attribute outranks every author rule of equal importance */
    {   const char *inl = uw_attr(d, n, "style");
        if (inl && *inl) {
            uw_sheet *tmp;
            char wrap[2048];
            snprintf(wrap, sizeof wrap, "*{%s}", inl);
            tmp = uw_css_parse(d, wrap, -1, UW_ORIGIN_AUTHOR);
            if (tmp) {
                uw_rule *r = uw_sheet_rules(tmp);
                for (; r; r = uw_rule_next(r)) {
                    uw_decl *dc;
                    for (dc = uw_rule_decls(r); dc; dc = uw_decl_next(dc))
                        apply_decl(d, st, parent, uw_atom_name(d, uw_decl_prop(dc)),
                                   uw_decl_value(dc), root_px);
                }
            }
        }
    }
    if (!st->line_height) st->line_height = st->font_size * 4 / 3;
}

static void style_tree(uw_doc *d, uw_node *n, const uw_style *parent, int root_px)
{
    uw_node *c;
    if (uw_type(n) == UW_NODE_ELEMENT) {
        style_element(d, n, parent, root_px);
        parent = (const uw_style *)n->style;
    }
    for (c = uw_first_child(n); c; c = uw_next_sibling(c))
        style_tree(d, c, parent, root_px);
}

int uw_add_sheet(uw_doc *d, uw_sheet *s)
{
    if (!d || !s) return -1;
    if (d->sheets_tail) { uw_sheet_link(d->sheets_tail, s); d->sheets_tail = s; }
    else { d->sheets = d->sheets_tail = s; }
    d->styled = 0;
    return 0;
}

int uw_add_inline_sheets(uw_doc *d)
{
    uw_node *n;
    int count = 0;
    if (!d) return 0;
    for (n = uw_next_in_order(d->document, d->document); n;
         n = uw_next_in_order(n, d->document)) {
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        if (strcmp(uw_tag_name(d, n), "style")) continue;
        {   uw_node *t = uw_first_child(n);
            int tl = 0;
            const char *txt = t ? uw_text(t, &tl) : NULL;
            if (txt && tl) {
                uw_sheet *s = uw_css_parse(d, txt, tl, UW_ORIGIN_AUTHOR);
                if (s) { uw_add_sheet(d, s); count++; }
            } }
    }
    return count;
}

/* ---- alternate cascade engine (see unoweb.h) ------------------------------ */
static uw_cascade_fn g_cascade;
static void         *g_cascade_user;

void uw_cascade_set(uw_cascade_fn fn, void *user)
{ g_cascade = fn; g_cascade_user = user; }

int uw_cascade_active(void) { return g_cascade != NULL; }

const char *uw_ua_css(void) { return UA_CSS; }

int uw_style_store(uw_doc *d, uw_node *n, const uw_style *s)
{
    uw_style *st;
    if (!d || !n || !s || uw_type(n) != UW_NODE_ELEMENT) return -1;
    st = (uw_style *)n->style;
    if (!st) {
        st = (uw_style *)uw_arena(d, sizeof *st);
        if (!st) return -1;
        n->style = st;
    }
    *st = *s;
    return 0;
}

int uw_style_document(uw_doc *d, int viewport_w, int viewport_h)
{
    if (!d) return -1;
    d->vw = viewport_w > 0 ? viewport_w : 800;
    d->vh = viewport_h > 0 ? viewport_h : 600;
    /* the external engine, when registered, IS the style pass; a non-zero
     * return degrades to the built-in cascade below rather than leaving the
     * tree unstyled */
    if (g_cascade && g_cascade(g_cascade_user, d, d->vw, d->vh) == 0) {
        d->styled = 1;
        return 0;
    }
    /* the UA sheet is installed once, ahead of every author sheet */
    if (!d->sheets || uw_sheet_origin(d->sheets) != UW_ORIGIN_UA) {
        uw_sheet *ua = uw_css_parse(d, UA_CSS, -1, UW_ORIGIN_UA);
        if (!ua) return -1;
        uw_sheet_link(ua, d->sheets);
        d->sheets = ua;
        if (!d->sheets_tail) d->sheets_tail = ua;
    }
    style_tree(d, d->document, NULL, 14);
    d->styled = 1;
    return 0;
}

const uw_style *uw_computed(uw_node *n)
{
    if (!n || uw_type(n) != UW_NODE_ELEMENT) return NULL;
    return (const uw_style *)n->style;
}

/* ---- golden dump ---------------------------------------------------------- */
typedef struct { char *b; int max, n; } sob;

static void sp(sob *o, const char *fmt, ...)
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
        if (room > 0) { int k = len < room ? len : room;
                        memcpy(o->b + o->n, tmp, (size_t)k); }
        o->b[o->n + len < o->max - 1 ? o->n + len : o->max - 1] = 0;
    }
    o->n += len;
}

static const char *disp_name(int d2)
{
    switch (d2) {
    case UW_DISP_BLOCK: return "block";
    case UW_DISP_INLINE_BLOCK: return "inline-block";
    case UW_DISP_LIST_ITEM: return "list-item";
    case UW_DISP_NONE: return "none";
    default: return "inline";
    }
}

static void lenstr(uw_len l, char *out, size_t n)
{
    if (l.unit == UW_LEN_AUTO) snprintf(out, n, "auto");
    else if (l.unit == UW_LEN_PCT) snprintf(out, n, "%d%%", l.v);
    else snprintf(out, n, "%d", l.v);
}

static void dump_style(uw_doc *d, uw_node *n, int depth, sob *o)
{
    uw_node *c;
    if (uw_type(n) == UW_NODE_ELEMENT) {
        const uw_style *s = uw_computed(n);
        int i;
        char w[24], h[24];
        for (i = 0; i < depth; i++) sp(o, "  ");
        sp(o, "%s", uw_tag_name(d, n));
        if (!s) { sp(o, " (unstyled)\n"); }
        else {
            lenstr(s->width, w, sizeof w);
            lenstr(s->height, h, sizeof h);
            sp(o, " display=%s font=%d/%d%s%s color=#%02x%02x%02x",
               disp_name(s->display), s->font_size, s->font_weight,
               s->font_style ? "i" : "",
               s->font_family == UW_FF_MONO ? "m" : s->font_family == UW_FF_SERIF ? "s" : "",
               s->color.r, s->color.g, s->color.b);
            if (s->underline) sp(o, " underline");
            if (s->has_bg) sp(o, " bg=#%02x%02x%02x", s->background_color.r,
                              s->background_color.g, s->background_color.b);
            {   char m4[4][16], p4[4][16];
                for (i = 0; i < 4; i++) { lenstr(s->margin[i], m4[i], 16);
                                          lenstr(s->padding[i], p4[i], 16); }
                if (strcmp(m4[0],"0")||strcmp(m4[1],"0")||strcmp(m4[2],"0")||strcmp(m4[3],"0"))
                    sp(o, " margin=%s,%s,%s,%s", m4[0], m4[1], m4[2], m4[3]);
                if (strcmp(p4[0],"0")||strcmp(p4[1],"0")||strcmp(p4[2],"0")||strcmp(p4[3],"0"))
                    sp(o, " padding=%s,%s,%s,%s", p4[0], p4[1], p4[2], p4[3]);
            }
            for (i = 0; i < 4; i++)
                if (s->border_width[i])
                    sp(o, " border%d=%dpx#%02x%02x%02x", i, s->border_width[i],
                       s->border_color[i].r, s->border_color[i].g, s->border_color[i].b);
            if (strcmp(w, "auto")) sp(o, " width=%s", w);
            if (strcmp(h, "auto")) sp(o, " height=%s", h);
            if (s->white_space == UW_WS_PRE) sp(o, " pre");
            if (s->text_align) sp(o, " align=%d", s->text_align);
            if (s->list_bullet) sp(o, " bullet=%d", s->list_bullet);
            sp(o, "\n");
        }
        depth++;
    }
    for (c = uw_first_child(n); c; c = uw_next_sibling(c)) dump_style(d, c, depth, o);
}

int uw_style_dump(uw_doc *d, uw_node *n, char *out, int max)
{
    sob o;
    o.b = out; o.max = max; o.n = 0;
    if (max > 0) out[0] = 0;
    if (!d) return 0;
    if (!n) n = d->document;
    dump_style(d, n, 0, &o);
    return o.n;
}
