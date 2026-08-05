/* uw_select.c - the css_select_handler over unoweb's PUBLIC dom API.
 *
 * `node` is a uw_node*; `pw` is the pass's uwx_ctx. Everything reaches the
 * tree through unoweb.h accessors only - the bridge consumes unoweb as a
 * neutral API and must keep working when unoweb's internals move.
 *
 * Ownership rules learned from libcss's select.c (they are NOT in the
 * header docs): node_name/node_id/node_classes return STRONG lwc refs that
 * libcss unrefs; the classes ARRAY is never freed by libcss and may be live
 * twice (style-sharing candidates compare two nodes' arrays), so arrays
 * come from the pass pool. Element-name compares are ASCII-caseless, like
 * HTML. */
#include <string.h>
#include <strings.h>
#include "uw_bridge.h"

#define UNUSED(x) ((x) = (x))

/* ---- small helpers -------------------------------------------------------- */
static int ieq(const char *a, const char *b)
{ return a && b && strcasecmp(a, b) == 0; }

static lwc_string *intern(const char *s, size_t n)
{
    lwc_string *r = NULL;
    if (lwc_intern_string(s, n, &r) != lwc_error_ok) return NULL;
    return r;
}

static const char *node_tag(uwx_ctx *cx, uw_node *n)
{ return uw_tag_name(cx->doc, n); }

static uw_node *parent_element(uw_node *n)
{
    uw_node *p = uw_parent(n);
    return (p && uw_type(p) == UW_NODE_ELEMENT) ? p : NULL;
}

static uw_node *prev_element(uw_node *n)
{
    uw_node *p = uw_prev_sibling(n);
    while (p && uw_type(p) != UW_NODE_ELEMENT) p = uw_prev_sibling(p);
    return p;
}

static int qname_is(uwx_ctx *cx, uw_node *n, const css_qname *q)
{
    const char *tag = node_tag(cx, n);
    return tag && q->name &&
           strncasecmp(tag, lwc_string_data(q->name),
                       lwc_string_length(q->name)) == 0 &&
           tag[lwc_string_length(q->name)] == 0;
}

/* does the whitespace-separated list `list` contain `tok` (caseless)? */
static int list_contains(const char *list, const char *tok, size_t toklen)
{
    const char *p = list;
    while (*p) {
        const char *b;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        b = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if ((size_t)(p - b) == toklen && strncasecmp(b, tok, toklen) == 0)
            return 1;
    }
    return 0;
}

/* ---- names, classes, ids -------------------------------------------------- */
static css_error node_name(void *pw, void *node, css_qname *qname)
{
    uwx_ctx *cx = pw;
    const char *tag = node_tag(cx, (uw_node *)node);
    qname->name = intern(tag, strlen(tag));
    return qname->name ? CSS_OK : CSS_NOMEM;
}

static css_error node_classes(void *pw, void *node,
        lwc_string ***classes, uint32_t *n_classes)
{
    uwx_ctx *cx = pw;
    const char *cls = uw_attr(cx->doc, (uw_node *)node, "class");
    lwc_string **arr;
    uint32_t count = 0, cap = 0;
    const char *p;

    *classes = NULL;
    *n_classes = 0;
    if (!cls || !*cls) return CSS_OK;

    for (p = cls; *p; ) {                       /* count tokens first */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        cap++;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    }
    if (!cap) return CSS_OK;

    arr = uwx_pool_alloc(cx, cap * sizeof *arr);
    if (!arr) return CSS_NOMEM;

    for (p = cls; *p; ) {
        const char *b;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        b = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        arr[count] = intern(b, (size_t)(p - b));
        if (!arr[count]) {                      /* unref what we made */
            while (count) lwc_string_unref(arr[--count]);
            return CSS_NOMEM;
        }
        count++;
    }
    *classes = arr;                             /* strings: refs transferred */
    *n_classes = count;
    return CSS_OK;
}

static css_error node_id(void *pw, void *node, lwc_string **id)
{
    uwx_ctx *cx = pw;
    const char *v = uw_attr(cx->doc, (uw_node *)node, "id");
    *id = NULL;
    if (v && *v) {
        *id = intern(v, strlen(v));
        if (!*id) return CSS_NOMEM;
    }
    return CSS_OK;
}

/* ---- tree relations ------------------------------------------------------- */
static css_error named_ancestor_node(void *pw, void *node,
        const css_qname *qname, void **ancestor)
{
    uwx_ctx *cx = pw;
    uw_node *p = parent_element((uw_node *)node);
    while (p && !qname_is(cx, p, qname)) p = parent_element(p);
    *ancestor = p;
    return CSS_OK;
}

static css_error named_parent_node(void *pw, void *node,
        const css_qname *qname, void **parent)
{
    uwx_ctx *cx = pw;
    uw_node *p = parent_element((uw_node *)node);
    *parent = (p && qname_is(cx, p, qname)) ? p : NULL;
    return CSS_OK;
}

static css_error named_sibling_node(void *pw, void *node,
        const css_qname *qname, void **sibling)
{
    uwx_ctx *cx = pw;
    uw_node *p = prev_element((uw_node *)node);
    *sibling = (p && qname_is(cx, p, qname)) ? p : NULL;
    return CSS_OK;
}

static css_error named_generic_sibling_node(void *pw, void *node,
        const css_qname *qname, void **sibling)
{
    uwx_ctx *cx = pw;
    uw_node *p = prev_element((uw_node *)node);
    while (p && !qname_is(cx, p, qname)) p = prev_element(p);
    *sibling = p;
    return CSS_OK;
}

static css_error parent_node(void *pw, void *node, void **parent)
{
    UNUSED(pw);
    *parent = parent_element((uw_node *)node);
    return CSS_OK;
}

static css_error sibling_node(void *pw, void *node, void **sibling)
{
    UNUSED(pw);
    *sibling = prev_element((uw_node *)node);
    return CSS_OK;
}

/* ---- predicates ----------------------------------------------------------- */
static css_error node_has_name(void *pw, void *node,
        const css_qname *qname, bool *match)
{
    uwx_ctx *cx = pw;
    *match = qname_is(cx, (uw_node *)node, qname);
    return CSS_OK;
}

static css_error node_has_class(void *pw, void *node,
        lwc_string *name, bool *match)
{
    uwx_ctx *cx = pw;
    const char *cls = uw_attr(cx->doc, (uw_node *)node, "class");
    *match = cls && list_contains(cls, lwc_string_data(name),
                                  lwc_string_length(name));
    return CSS_OK;
}

static css_error node_has_id(void *pw, void *node,
        lwc_string *name, bool *match)
{
    uwx_ctx *cx = pw;
    const char *v = uw_attr(cx->doc, (uw_node *)node, "id");
    *match = v && strncasecmp(v, lwc_string_data(name),
                              lwc_string_length(name)) == 0 &&
             v[lwc_string_length(name)] == 0;
    return CSS_OK;
}

static const char *attr_of(uwx_ctx *cx, void *node, const css_qname *qname)
{
    char name[64];
    size_t n = lwc_string_length(qname->name);
    if (n >= sizeof name) return NULL;
    memcpy(name, lwc_string_data(qname->name), n);
    name[n] = 0;
    return uw_attr(cx->doc, (uw_node *)node, name);
}

static css_error node_has_attribute(void *pw, void *node,
        const css_qname *qname, bool *match)
{
    uwx_ctx *cx = pw;
    *match = attr_of(cx, node, qname) != NULL;
    return CSS_OK;
}

static css_error node_has_attribute_equal(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    uwx_ctx *cx = pw;
    const char *v = attr_of(cx, node, qname);
    *match = v && strncasecmp(v, lwc_string_data(value),
                              lwc_string_length(value)) == 0 &&
             v[lwc_string_length(value)] == 0;
    return CSS_OK;
}

static css_error node_has_attribute_dashmatch(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    uwx_ctx *cx = pw;
    const char *v = attr_of(cx, node, qname);
    size_t n = lwc_string_length(value);
    *match = v && strncasecmp(v, lwc_string_data(value), n) == 0 &&
             (v[n] == 0 || v[n] == '-');
    return CSS_OK;
}

static css_error node_has_attribute_includes(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    uwx_ctx *cx = pw;
    const char *v = attr_of(cx, node, qname);
    *match = v && list_contains(v, lwc_string_data(value),
                                lwc_string_length(value));
    return CSS_OK;
}

static css_error node_has_attribute_prefix(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    uwx_ctx *cx = pw;
    const char *v = attr_of(cx, node, qname);
    size_t n = lwc_string_length(value);
    *match = v && n > 0 && strncasecmp(v, lwc_string_data(value), n) == 0;
    return CSS_OK;
}

static css_error node_has_attribute_suffix(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    uwx_ctx *cx = pw;
    const char *v = attr_of(cx, node, qname);
    size_t n = lwc_string_length(value), vl;
    *match = false;
    if (v && n > 0 && (vl = strlen(v)) >= n)
        *match = strncasecmp(v + vl - n, lwc_string_data(value), n) == 0;
    return CSS_OK;
}

static css_error node_has_attribute_substring(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    uwx_ctx *cx = pw;
    const char *v = attr_of(cx, node, qname);
    size_t n = lwc_string_length(value);
    *match = false;
    if (v && n > 0) {
        size_t vl = strlen(v), i;
        for (i = 0; !*match && i + n <= vl; i++)
            *match = strncasecmp(v + i, lwc_string_data(value), n) == 0;
    }
    return CSS_OK;
}

/* ---- pseudo-classes ------------------------------------------------------- */
static css_error node_is_root(void *pw, void *node, bool *match)
{
    UNUSED(pw);
    *match = parent_element((uw_node *)node) == NULL;
    return CSS_OK;
}

static css_error node_count_siblings(void *pw, void *node,
        bool same_name, bool after, int32_t *count)
{
    uwx_ctx *cx = pw;
    uw_node *n = (uw_node *)node;
    const char *tag = node_tag(cx, n);
    int32_t c = 0;
    uw_node *s = after ? uw_next_sibling(n) : uw_prev_sibling(n);
    while (s) {
        if (uw_type(s) == UW_NODE_ELEMENT &&
            (!same_name || ieq(node_tag(cx, s), tag)))
            c++;
        s = after ? uw_next_sibling(s) : uw_prev_sibling(s);
    }
    *count = c;
    return CSS_OK;
}

static css_error node_is_empty(void *pw, void *node, bool *match)
{
    uw_node *c;
    UNUSED(pw);
    *match = true;
    for (c = uw_first_child((uw_node *)node); c; c = uw_next_sibling(c)) {
        int tl = 0;
        if (uw_type(c) == UW_NODE_ELEMENT) { *match = false; break; }
        if (uw_type(c) == UW_NODE_TEXT && uw_text(c, &tl) && tl > 0)
            { *match = false; break; }
    }
    return CSS_OK;
}

static css_error node_is_link(void *pw, void *node, bool *match)
{
    uwx_ctx *cx = pw;
    *match = ieq(node_tag(cx, (uw_node *)node), "a") &&
             uw_attr(cx->doc, (uw_node *)node, "href") != NULL;
    return CSS_OK;
}

/* the interaction pseudo-classes: never true in a static style pass */
static css_error no_match(void *pw, void *node, bool *match)
{ UNUSED(pw); UNUSED(node); *match = false; return CSS_OK; }

static css_error node_is_lang(void *pw, void *node,
        lwc_string *lang, bool *match)
{ UNUSED(pw); UNUSED(node); UNUSED(lang); *match = false; return CSS_OK; }

/* ---- hints and node data -------------------------------------------------- */
static css_error node_presentational_hint(void *pw, void *node,
        uint32_t *nhints, css_hint **hints)
{
    UNUSED(pw); UNUSED(node);
    *nhints = 0;
    *hints = NULL;
    return CSS_OK;
}

/* Only `color` gets a UA fallback here; every real default lives in the
 * SHARED UA stylesheet text (uw_ua_css) so the two cascades cannot drift. */
static css_error ua_default_for_property(void *pw, uint32_t property,
        css_hint *hint)
{
    UNUSED(pw);
    if (property == CSS_PROP_COLOR) {
        hint->data.color = 0xFF000000;
        hint->status = CSS_COLOR_COLOR;
    } else if (property == CSS_PROP_FONT_FAMILY) {
        hint->data.strings = NULL;
        hint->status = CSS_FONT_FAMILY_SANS_SERIF;
    } else if (property == CSS_PROP_QUOTES) {
        hint->data.strings = NULL;
        hint->status = CSS_QUOTES_NONE;
    } else if (property == CSS_PROP_VOICE_FAMILY) {
        hint->data.strings = NULL;
        hint->status = 0;
    } else {
        return CSS_INVALID;
    }
    return CSS_OK;
}

/* Per-pass node-data map - see the lifetime note in uw_bridge.h. Deleting
 * eagerly (the upstream example's pattern) is a use-after-free on any tree
 * deeper than one node. */
static unsigned nd_hash(void *p)
{ return (unsigned)(((size_t)p >> 4) & (UWX_ND_BUCKETS - 1)); }

static css_error set_libcss_node_data(void *pw, void *n, void *libcss_node_data)
{
    uwx_ctx *cx = pw;
    unsigned h = nd_hash(n);
    uwx_nodedata *e;
    for (e = cx->nd[h]; e; e = e->next)
        if (e->node == (uw_node *)n) {
            if (e->data && e->data != libcss_node_data)
                css_libcss_node_data_handler(&uwx_select_handler,
                        CSS_NODE_DELETED, pw, n, NULL, e->data);
            e->data = libcss_node_data;
            return CSS_OK;
        }
    e = uwx_pool_alloc(cx, sizeof *e);
    if (!e) {   /* can't track it: delete now rather than leak */
        css_libcss_node_data_handler(&uwx_select_handler, CSS_NODE_DELETED,
                pw, n, NULL, libcss_node_data);
        return CSS_NOMEM;
    }
    e->node = (uw_node *)n;
    e->data = libcss_node_data;
    e->next = cx->nd[h];
    cx->nd[h] = e;
    return CSS_OK;
}

static css_error get_libcss_node_data(void *pw, void *n, void **libcss_node_data)
{
    uwx_ctx *cx = pw;
    uwx_nodedata *e;
    *libcss_node_data = NULL;
    for (e = cx->nd[nd_hash(n)]; e; e = e->next)
        if (e->node == (uw_node *)n) { *libcss_node_data = e->data; break; }
    return CSS_OK;
}

void uwx_nodedata_drop_all(uwx_ctx *cx)
{
    int i;
    for (i = 0; i < UWX_ND_BUCKETS; i++) {
        uwx_nodedata *e;
        for (e = cx->nd[i]; e; e = e->next)
            if (e->data)
                css_libcss_node_data_handler(&uwx_select_handler,
                        CSS_NODE_DELETED, cx, e->node, NULL, e->data);
        cx->nd[i] = NULL;       /* entries themselves live in the pool */
    }
}

/* ---- the table ------------------------------------------------------------ */
css_select_handler uwx_select_handler = {
    CSS_SELECT_HANDLER_VERSION_1,

    node_name,
    node_classes,
    node_id,
    named_ancestor_node,
    named_parent_node,
    named_sibling_node,
    named_generic_sibling_node,
    parent_node,
    sibling_node,
    node_has_name,
    node_has_class,
    node_has_id,
    node_has_attribute,
    node_has_attribute_equal,
    node_has_attribute_dashmatch,
    node_has_attribute_includes,
    node_has_attribute_prefix,
    node_has_attribute_suffix,
    node_has_attribute_substring,
    node_is_root,
    node_count_siblings,
    node_is_empty,
    node_is_link,
    no_match,                       /* visited */
    no_match,                       /* hover   */
    no_match,                       /* active  */
    no_match,                       /* focus   */
    no_match,                       /* enabled */
    no_match,                       /* disabled*/
    no_match,                       /* checked */
    no_match,                       /* target  */
    node_is_lang,
    node_presentational_hint,
    ua_default_for_property,
    set_libcss_node_data,
    get_libcss_node_data,
};
