/* ===========================================================================
 * webjs.c - engine-neutral DOM operations + the shared prelude.
 *
 * See webjs.h for why this file exists and how the three layers split. The
 * natives here speak only in INTEGER NODE HANDLES and strings, which is what
 * lets one implementation serve two engines whose value types have nothing
 * in common. The prelude at the bottom turns those handles back into objects
 * page authors can use.
 * ======================================================================== */
#include "webjs.h"
#include "js.h"
#include <string.h>
#include <stdlib.h>

/* ---- call-frame accessors -------------------------------------------------
 * One definition, dispatched to whichever backend built the frame. */
int         webjs_argc(webjs_args *a)            { return a->ops->argc(a->impl); }
const char *webjs_arg_str(webjs_args *a, int i)  { return a->ops->arg_str(a->impl, i); }
int         webjs_arg_int(webjs_args *a, int i)  { return a->ops->arg_int(a->impl, i); }
void        webjs_ret_int(webjs_args *a, int v)  { a->ops->ret_int(a->impl, v); }
void        webjs_ret_str(webjs_args *a, const char *s) { a->ops->ret_str(a->impl, s); }

/* ---- node handles ---------------------------------------------------------
 * A handle is an index into a per-page table, never a raw pointer: script is
 * untrusted, and a fabricated integer must be able to do nothing worse than
 * miss. 0 is "no node", so a handle is always index+1. Nodes live in the
 * document arena and die with the page, so the table needs no ownership. */
#define NODE_MAX 512

static uw_doc  *g_doc;
static void    *g_vm;
static uw_node *g_node[NODE_MAX];
static int      g_nnodes;
static int      g_dirty;

static int handle_of(uw_node *n)
{
    int i;
    if (!n) return 0;
    for (i = 0; i < g_nnodes; i++) if (g_node[i] == n) return i + 1;
    if (g_nnodes >= NODE_MAX) return 0;
    g_node[g_nnodes++] = n;
    return g_nnodes;
}

static uw_node *node_of(int h)
{
    if (h <= 0 || h > g_nnodes) return NULL;
    return g_node[h - 1];
}

static void qsa_invalidate(void);

/* Every mutation goes through here: a tree change that layout never hears
 * about is a change the user cannot see. */
static void touched(void) { g_dirty = 1; qsa_invalidate(); }

int webjs_take_dirty(void) { int d = g_dirty; g_dirty = 0; return d; }

/* ---- the natives ----------------------------------------------------------
 * Naming: __d_* so the prelude can hide every one of them. A page that pokes
 * at them directly gets handles and nothing more dangerous. */

static void n_get_by_id(webjs_args *a)
{
    webjs_ret_int(a, handle_of(uw_get_element_by_id(g_doc, webjs_arg_str(a, 0))));
}

/* querySelector / querySelectorAll over uw_matches - the same matcher the
 * built-in cascade uses, which is exactly why unoweb exposes it. */
static uw_node *sel_nth(const char *sel, int nth, int *total)
{
    uw_node *n, *hit = NULL;
    int count = 0;
    for (n = uw_next_in_order(uw_document(g_doc), uw_document(g_doc)); n;
         n = uw_next_in_order(n, uw_document(g_doc))) {
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        if (!uw_matches(g_doc, n, sel)) continue;
        if (count == nth) hit = n;
        count++;
    }
    if (total) *total = count;
    return hit;
}

static void n_query(webjs_args *a)
{ webjs_ret_int(a, handle_of(sel_nth(webjs_arg_str(a, 0), 0, NULL))); }

/* querySelectorAll's backing store. It used to cost a FULL document traversal
 * per index: n_query_count walked the whole tree to count, and then
 * n_query_at walked it again, from the top, for every i - O(N*matches), which
 * a page with a few hundred matched elements turned into a visible stall.
 * Collected once here, bounded, and then indexed. Invalidated on any DOM
 * mutation (touched) and at page teardown, so the snapshot a querySelectorAll
 * loop reads is always of the current tree. */
#define QSA_MAX 256
static uw_node *g_qsa[QSA_MAX];
static int      g_qsa_n;
static char     g_qsa_sel[128];
static int      g_qsa_valid;

static void qsa_invalidate(void) { g_qsa_valid = 0; }

static void qsa_build(const char *sel)
{
    uw_node *n;
    int k = 0;
    if (g_qsa_valid && !strcmp(g_qsa_sel, sel)) return;   /* reuse this call's snapshot */
    g_qsa_n = 0;
    for (n = uw_next_in_order(uw_document(g_doc), uw_document(g_doc)); n;
         n = uw_next_in_order(n, uw_document(g_doc))) {
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        if (!uw_matches(g_doc, n, sel)) continue;
        if (g_qsa_n >= QSA_MAX) break;    /* bounded: script cannot demand unbounded handles */
        g_qsa[g_qsa_n++] = n;
    }
    while (sel[k] && k < (int)sizeof g_qsa_sel - 1) { g_qsa_sel[k] = sel[k]; k++; }
    g_qsa_sel[k] = 0;
    g_qsa_valid = 1;
}

static void n_query_count(webjs_args *a)
{ qsa_build(webjs_arg_str(a, 0)); webjs_ret_int(a, g_qsa_n); }

static void n_query_at(webjs_args *a)
{
    int i = webjs_arg_int(a, 1);
    qsa_build(webjs_arg_str(a, 0));
    webjs_ret_int(a, (i >= 0 && i < g_qsa_n) ? handle_of(g_qsa[i]) : 0);
}

static void n_tag(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    webjs_ret_str(a, n ? uw_tag_name(g_doc, n) : "");
}

static void n_get_attr(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    const char *v = n ? uw_attr(g_doc, n, webjs_arg_str(a, 1)) : NULL;
    webjs_ret_str(a, v);                       /* NULL -> null, as in a browser */
}

static void n_set_attr(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    if (n) { uw_set_attr(g_doc, n, webjs_arg_str(a, 1), webjs_arg_str(a, 2)); touched(); }
}

static void n_remove_attr(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    if (n) { uw_remove_attr(g_doc, n, webjs_arg_str(a, 1)); touched(); }
}

/* textContent: the concatenated text of the subtree. Bounded - a page can
 * nest text arbitrarily deep and a script asking for document.body.textContent
 * must not be able to ask for unbounded memory. */
#define TEXT_MAX 8192
static char g_textbuf[TEXT_MAX];

static void text_collect(uw_node *n, int *at)
{
    uw_node *c;
    if (uw_type(n) == UW_NODE_TEXT) {
        int tl = 0;
        const char *t = uw_text(n, &tl);
        if (t) {
            if (tl > TEXT_MAX - 1 - *at) tl = TEXT_MAX - 1 - *at;
            if (tl > 0) { memcpy(g_textbuf + *at, t, (size_t)tl); *at += tl; }
        }
        return;
    }
    for (c = uw_first_child(n); c; c = uw_next_sibling(c)) text_collect(c, at);
}

static void n_text(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    int at = 0;
    if (n) text_collect(n, &at);
    g_textbuf[at] = 0;
    webjs_ret_str(a, g_textbuf);
}

static void n_set_text(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    const char *s = webjs_arg_str(a, 1);
    uw_node *c;
    if (!n) return;
    while ((c = uw_first_child(n)) != NULL) uw_remove(g_doc, c);
    {   uw_node *t = uw_create_text(g_doc, s, (int)strlen(s));
        if (t) uw_append(g_doc, n, t); }
    touched();
}

static void n_inner_html(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    if (!n) { webjs_ret_str(a, ""); return; }
    uw_serialize(g_doc, n, g_textbuf, TEXT_MAX);
    webjs_ret_str(a, g_textbuf);
}

static void n_set_inner_html(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    const char *s = webjs_arg_str(a, 1);
    if (!n) return;
    uw_parse_fragment(g_doc, n, s, (int)strlen(s));   /* clears children first */
    touched();
}

/* ---- tree navigation + mutation ------------------------------------------- */
static void n_parent(webjs_args *a)
{ uw_node *n = node_of(webjs_arg_int(a, 0)); webjs_ret_int(a, handle_of(n ? uw_parent(n) : 0)); }

static void n_child_count(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0)), *c;
    int k = 0;
    if (n) for (c = uw_first_child(n); c; c = uw_next_sibling(c))
        if (uw_type(c) == UW_NODE_ELEMENT) k++;
    webjs_ret_int(a, k);
}

static void n_child_at(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0)), *c;
    int want = webjs_arg_int(a, 1), k = 0;
    if (n) for (c = uw_first_child(n); c; c = uw_next_sibling(c)) {
        if (uw_type(c) != UW_NODE_ELEMENT) continue;
        if (k++ == want) { webjs_ret_int(a, handle_of(c)); return; }
    }
    webjs_ret_int(a, 0);
}

static void n_create(webjs_args *a)
{ webjs_ret_int(a, handle_of(uw_create_element(g_doc, webjs_arg_str(a, 0)))); }

static void n_create_text(webjs_args *a)
{
    const char *s = webjs_arg_str(a, 0);
    webjs_ret_int(a, handle_of(uw_create_text(g_doc, s, (int)strlen(s))));
}

static void n_append(webjs_args *a)
{
    uw_node *p = node_of(webjs_arg_int(a, 0)), *c = node_of(webjs_arg_int(a, 1));
    if (p && c) { uw_append(g_doc, p, c); touched(); }
}

static void n_remove(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    if (n) { uw_remove(g_doc, n); touched(); }
}

static void n_body(webjs_args *a)  { webjs_ret_int(a, handle_of(uw_body(g_doc))); }

/* ---- console --------------------------------------------------------------
 * The log sink belongs to the CALL, not the VM: js_run's contract appends to
 * the caller's buffer, and webjs_run/pump/event each get their own. */
static char *g_log; static int g_logmax;

static void log_str(const char *s)
{
    int at, n;
    if (!g_log || g_logmax <= 0) return;
    at = (int)strlen(g_log);
    n = (int)strlen(s);
    if (n > g_logmax - 1 - at) n = g_logmax - 1 - at;
    if (n > 0) { memcpy(g_log + at, s, (size_t)n); g_log[at + n] = 0; }
}

static void n_log(webjs_args *a)
{
    int i, argc = webjs_argc(a);
    for (i = 0; i < argc; i++) { if (i) log_str(" "); log_str(webjs_arg_str(a, i)); }
    log_str("\n");
}

/* ---- document.write --------------------------------------------------------
 * After parsing, document.write() appends to the body rather than splicing at
 * the insertion point: the parser is long gone by the time a timer or an
 * event handler runs, and appending is what browsers do for a write() after
 * load. During the initial parse the browser still uses the parser's own
 * uw_parse_insert path (that is document.write proper). */
static void n_write(webjs_args *a)
{
    uw_node *b = uw_body(g_doc);
    int i, argc = webjs_argc(a);
    if (!b) return;
    for (i = 0; i < argc; i++) {
        const char *s = webjs_arg_str(a, i);
        uw_node *tmp = uw_create_element(g_doc, "span");
        if (!tmp) return;
        uw_append(g_doc, b, tmp);
        uw_parse_fragment(g_doc, tmp, s, (int)strlen(s));
    }
    touched();
}

/* ---- timers ---------------------------------------------------------------
 * A timer is a callback id (the prelude's registry) plus a due time. Fired
 * in webjs_pump by evaluating a call - going through the engine's own eval
 * keeps the adapter down to one entry point instead of a call-a-function API
 * that every engine spells differently. */
#define TIMER_MAX 32
typedef struct { int id, cb; unsigned due, period; int live; } timer;
static timer   g_timer[TIMER_MAX];
static int     g_timer_seq;
static unsigned g_now;

static void n_set_timer(webjs_args *a)
{
    int cb = webjs_arg_int(a, 0), ms = webjs_arg_int(a, 1), rep = webjs_arg_int(a, 2), i;
    if (ms < 0) ms = 0;
    for (i = 0; i < TIMER_MAX; i++) if (!g_timer[i].live) {
        g_timer[i].live = 1;
        g_timer[i].id = ++g_timer_seq;
        g_timer[i].cb = cb;
        g_timer[i].due = g_now + (unsigned)ms;
        g_timer[i].period = rep ? (unsigned)(ms > 0 ? ms : 1) : 0;
        webjs_ret_int(a, g_timer[i].id);
        return;
    }
    webjs_ret_int(a, 0);                     /* table full: a no-op timer */
}

static void n_clear_timer(webjs_args *a)
{
    int id = webjs_arg_int(a, 0), i;
    for (i = 0; i < TIMER_MAX; i++)
        if (g_timer[i].live && g_timer[i].id == id) g_timer[i].live = 0;
}

/* ---- event listeners -------------------------------------------------------
 * The registry is C-side (node, type, callback id) so dispatch can walk the
 * ancestor chain in C and only enter JS for handlers that actually match. */
#define LISTEN_MAX 64
typedef struct { uw_node *n; char type[16]; int cb; } listener;
static listener g_listen[LISTEN_MAX];
static int      g_nlisten;

static void n_add_listener(webjs_args *a)
{
    uw_node *n = node_of(webjs_arg_int(a, 0));
    const char *type = webjs_arg_str(a, 1);
    int cb = webjs_arg_int(a, 2), k = 0;
    if (!n || g_nlisten >= LISTEN_MAX) return;
    while (type[k] && k < 15) { g_listen[g_nlisten].type[k] = type[k]; k++; }
    g_listen[g_nlisten].type[k] = 0;
    g_listen[g_nlisten].n = n;
    g_listen[g_nlisten].cb = cb;
    g_nlisten++;
}

/* ---- the prelude ----------------------------------------------------------
 * Evaluated once per page, before any page script. It is the ONLY reason the
 * natives above can be so blunt: handles and flat functions become Element
 * objects with accessors, on both engines, because both have prototypes and
 * getters/setters. Keeping it as SOURCE rather than more C is deliberate -
 * it is the half of the binding that is pure ergonomics, and every line of
 * it would otherwise have to be written twice, once per engine. */
/* PART 1, the CORE: methods only. Every construct here runs on both engines,
 * which was established by measurement rather than by reading UNOJS.md -
 * unojs turns out to have NO object-literal accessors, NO
 * Object.defineProperty and NO `arguments`, so the obvious ergonomic prelude
 * is a syntax error there. It also requires a `var` in a for-init: bare
 * `for (i = 0; ...)` does not parse. Everything below was checked against
 * both engines rather than against either one's documentation. */
static const char PRELUDE_CORE[] =
"var __cbs = [];\n"
"function __reg(f){ __cbs.push(f); return __cbs.length - 1; }\n"
"function __fire(i, ev){ var f = __cbs[i]; if (f) f(ev); }\n"
"function Element(h){ this.__h = h; }\n"
"function __wrap(h){ return h ? new Element(h) : null; }\n"
"Element.prototype.tag = function(){ return __d_tag(this.__h); };\n"
"Element.prototype.getText = function(){ return __d_text(this.__h); };\n"
"Element.prototype.setText = function(v){ __d_setText(this.__h, String(v)); };\n"
"Element.prototype.getHtml = function(){ return __d_html(this.__h); };\n"
"Element.prototype.setHtml = function(v){ __d_setHtml(this.__h, String(v)); };\n"
"Element.prototype.getAttribute = function(k){ return __d_getAttr(this.__h, String(k)); };\n"
"Element.prototype.setAttribute = function(k, v){ __d_setAttr(this.__h, String(k), String(v)); };\n"
"Element.prototype.removeAttribute = function(k){ __d_delAttr(this.__h, String(k)); };\n"
"Element.prototype.appendChild = function(c){ __d_append(this.__h, c.__h); return c; };\n"
"Element.prototype.removeChild = function(c){ __d_remove(c.__h); return c; };\n"
"Element.prototype.remove = function(){ __d_remove(this.__h); };\n"
"Element.prototype.parent = function(){ return __wrap(__d_parent(this.__h)); };\n"
"Element.prototype.childList = function(){\n"
"  var a = [], n = __d_childCount(this.__h);\n"
"  for (var i = 0; i < n; i++) a.push(__wrap(__d_childAt(this.__h, i)));\n"
"  return a; };\n"
"Element.prototype.setStyle = function(css){ __d_setAttr(this.__h, 'style', String(css)); };\n"
"Element.prototype.getStyle = function(){ return __d_getAttr(this.__h, 'style') || ''; };\n"
"Element.prototype.addEventListener = function(t, f){ __d_listen(this.__h, String(t), __reg(f)); };\n"
"var document = {};\n"
"document.getElementById = function(id){ return __wrap(__d_byId(String(id))); };\n"
"document.querySelector = function(s){ return __wrap(__d_query(String(s))); };\n"
"document.querySelectorAll = function(s){\n"
"  var a = [], n = __d_queryCount(String(s));\n"
"  for (var i = 0; i < n; i++) a.push(__wrap(__d_queryAt(String(s), i)));\n"
"  return a; };\n"
"document.createElement = function(t){ return __wrap(__d_create(String(t))); };\n"
"document.createTextNode = function(t){ return __wrap(__d_createText(String(t))); };\n"
"document.getBody = function(){ return __wrap(__d_body()); };\n"
"document.addEventListener = function(t, f){ __d_listen(__d_body(), String(t), __reg(f)); };\n"
/* fixed arity rather than `arguments`, which unojs does not have; four
 * covers every real document.write call and the rest are simply dropped */
"document.write = function(a, b, c, d){\n"
"  if (a !== undefined) __d_write(String(a));\n"
"  if (b !== undefined) __d_write(String(b));\n"
"  if (c !== undefined) __d_write(String(c));\n"
"  if (d !== undefined) __d_write(String(d)); };\n"
"document.writeln = function(a, b, c, d){ document.write(a, b, c, d); __d_write('\\n'); };\n"
"var console = { log: __d_log, warn: __d_log, error: __d_log, info: __d_log };\n"
"function setTimeout(f, ms){ return __d_timer(__reg(f), ms | 0, 0); }\n"
"function setInterval(f, ms){ return __d_timer(__reg(f), ms | 0, 1); }\n"
"function clearTimeout(id){ __d_clearTimer(id | 0); }\n"
"function clearInterval(id){ __d_clearTimer(id | 0); }\n"
"var window = this;\n";

/* PART 2, the PROPERTY layer: the names real pages actually use. Needs
 * Object.defineProperty, which quickjs has and unojs does not - so this is
 * evaluated OPTIONALLY and its failure is not an error. The core above is
 * what every engine is guaranteed; this is what a modern engine adds.
 * (Giving unojs the same names would take C-side accessors through
 * ujs_set_accessor - a real option, and the natural M5b.) */
static const char PRELUDE_PROPS[] =
"(function(){\n"
"  function def(o, k, g, s){ Object.defineProperty(o, k, { get: g, set: s }); }\n"
"  def(Element.prototype, 'tagName',     function(){ return this.tag(); });\n"
"  def(Element.prototype, 'textContent', function(){ return this.getText(); },\n"
"                                        function(v){ this.setText(v); });\n"
"  def(Element.prototype, 'innerHTML',   function(){ return this.getHtml(); },\n"
"                                        function(v){ this.setHtml(v); });\n"
"  def(Element.prototype, 'parentNode',  function(){ return this.parent(); });\n"
"  def(Element.prototype, 'children',    function(){ return this.childList(); });\n"
"  def(Element.prototype, 'style', function(){\n"
"    var e = this;\n"
"    return { setProperty: function(k, v){ e.setStyle(e.getStyle() + ';' + k + ':' + v); },\n"
"             get cssText(){ return e.getStyle(); },\n"
"             set cssText(v){ e.setStyle(v); } }; });\n"
"  def(document, 'body', function(){ return document.getBody(); });\n"
"})();\n";

/* ---- binding table --------------------------------------------------------
 * One row per native. Adding a DOM call is a row here plus a prelude line,
 * and BOTH engines get it - which is the whole point of the split. */
static const struct { const char *name; webjs_native fn; int argc; } BINDINGS[] = {
    { "__d_byId",        n_get_by_id,     1 },
    { "__d_query",       n_query,         1 },
    { "__d_queryCount",  n_query_count,   1 },
    { "__d_queryAt",     n_query_at,      2 },
    { "__d_tag",         n_tag,           1 },
    { "__d_getAttr",     n_get_attr,      2 },
    { "__d_setAttr",     n_set_attr,      3 },
    { "__d_delAttr",     n_remove_attr,   2 },
    { "__d_text",        n_text,          1 },
    { "__d_setText",     n_set_text,      2 },
    { "__d_html",        n_inner_html,    1 },
    { "__d_setHtml",     n_set_inner_html,2 },
    { "__d_parent",      n_parent,        1 },
    { "__d_childCount",  n_child_count,   1 },
    { "__d_childAt",     n_child_at,      2 },
    { "__d_create",      n_create,        1 },
    { "__d_createText",  n_create_text,   1 },
    { "__d_append",      n_append,        2 },
    { "__d_remove",      n_remove,        1 },
    { "__d_body",        n_body,          0 },
    { "__d_log",         n_log,           1 },
    { "__d_write",       n_write,         1 },
    { "__d_timer",       n_set_timer,     3 },
    { "__d_clearTimer",  n_clear_timer,   1 },
    { "__d_listen",      n_add_listener,  3 },
};

/* ---- lifecycle ------------------------------------------------------------ */
int webjs_page_active(void) { return g_vm != NULL; }

void webjs_page_end(void)
{
    const webjs_engine *e = webjs_engine_current();
    if (g_vm && e) e->vm_free(g_vm);
    g_vm = NULL;
    g_doc = NULL;
    g_nnodes = 0;
    g_nlisten = 0;
    memset(g_timer, 0, sizeof g_timer);
    g_dirty = 0;
    g_qsa_valid = 0; g_qsa_n = 0;    /* the snapshot belonged to the old tree */
}

/* Why the last page_begin failed - "" when it did not. A binding layer that
 * silently declines to exist is the worst possible failure mode, so the
 * reason is kept and shown (the browser puts it on the engine page). */
static char g_last_err[192];
const char *webjs_last_error(void) { return g_last_err; }

int webjs_page_begin(uw_doc *d)
{
    const webjs_engine *e = webjs_engine_current();
    char err[192];
    unsigned i;

    webjs_page_end();
    if (!e || !d) return -1;
    g_doc = d;
    g_now = 0;
    g_vm = e->vm_new();
    if (!g_vm) { g_doc = NULL; return -1; }

    for (i = 0; i < sizeof BINDINGS / sizeof BINDINGS[0]; i++)
        e->def_fn(g_vm, BINDINGS[i].name, BINDINGS[i].fn, BINDINGS[i].argc);

    err[0] = 0;
    g_last_err[0] = 0;
    if (e->eval(g_vm, PRELUDE_CORE, (int)(sizeof PRELUDE_CORE - 1), err, sizeof err)) {
        int k = 0;
        while (err[k] && k < (int)sizeof g_last_err - 1) { g_last_err[k] = err[k]; k++; }
        g_last_err[k] = 0;
        webjs_page_end();
        return -1;
    }
    /* optional by design: an engine without Object.defineProperty keeps the
     * method core and simply has no property aliases */
    e->eval(g_vm, PRELUDE_PROPS, (int)(sizeof PRELUDE_PROPS - 1), err, sizeof err);
    return 0;
}

/* Does this page's engine have the property aliases (textContent, innerHTML,
 * ...)? The browser says so on the engine page, because "your script works
 * or it does not" is exactly the kind of thing a user should not have to
 * discover by trial. */
int webjs_has_properties(void)
{
    const webjs_engine *e = webjs_engine_current();
    char err[64];
    if (!g_vm || !e) return 0;
    err[0] = 0;
    return e->eval(g_vm, "typeof Object.defineProperty === 'function' ||"
                         " (function(){throw 0;})();", -1, err, sizeof err) == 0;
}

int webjs_run(const char *src, int len, char *log, int logmax)
{
    const webjs_engine *e = webjs_engine_current();
    char err[192];
    int rc;
    if (!g_vm || !e) return 2;
    g_log = log; g_logmax = logmax;
    err[0] = 0;
    rc = e->eval(g_vm, src, len, err, sizeof err);
    if (rc) { log_str(err); log_str("\n"); }
    g_log = NULL;
    return rc;
}

/* Fire one callback by evaluating a call against the prelude's registry.
 * Going through eval keeps the adapter to a single entry point; the cost is
 * compiling a ~20 byte call, which is nothing beside the handler itself. */
static int fire_cb(int cb, const char *arg)
{
    const webjs_engine *e = webjs_engine_current();
    char call[96], err[192];
    int n = 0;
    if (!g_vm || !e) return 0;
    {   const char *pre = "__fire(";
        while (*pre) call[n++] = *pre++;
        {   char num[12]; int k = 0, v = cb;
            if (!v) num[k++] = '0';
            while (v) { num[k++] = (char)('0' + v % 10); v /= 10; }
            while (k) call[n++] = num[--k]; }
        call[n++] = ','; call[n++] = '\'';
        /* Escape into the single-quoted JS literal. The arg is 'timer' or an
         * event type today, but a quote/backslash/newline spliced in raw would
         * break out of the literal - so it is escaped rather than trusted, and
         * the callback always sees exactly the string that was passed. */
        while (*arg && n < (int)sizeof call - 6) {
            char c = *arg++;
            if (c == '\'' || c == '\\') { call[n++] = '\\'; call[n++] = c; }
            else if (c == '\n')         { call[n++] = '\\'; call[n++] = 'n'; }
            else if (c == '\r')         { call[n++] = '\\'; call[n++] = 'r'; }
            else                          call[n++] = c;
        }
        call[n++] = '\''; call[n++] = ')'; call[n] = 0;
    }
    err[0] = 0;
    if (e->eval(g_vm, call, n, err, sizeof err)) { log_str(err); log_str("\n"); return 0; }
    return 1;
}

int webjs_pump(unsigned now_ms, char *log, int logmax)
{
    int i, ran = 0;
    if (!g_vm) return 0;
    g_now = now_ms;
    g_log = log; g_logmax = logmax;
    for (i = 0; i < TIMER_MAX; i++) {
        if (!g_timer[i].live || g_timer[i].due > now_ms) continue;
        {   int cb = g_timer[i].cb;
            if (g_timer[i].period) g_timer[i].due = now_ms + g_timer[i].period;
            else g_timer[i].live = 0;
            ran |= fire_cb(cb, "timer");
        }
    }
    g_log = NULL;
    return ran;
}

int webjs_event(uw_node *n, const char *type, char *log, int logmax)
{
    int ran = 0;
    if (!g_vm || !n) return 0;
    g_log = log; g_logmax = logmax;
    /* bubble: the target first, then each ancestor, which is the order a
     * handler on <body> expects to see a click on a button inside it */
    for (; n; n = uw_parent(n)) {
        int i;
        for (i = 0; i < g_nlisten; i++)
            if (g_listen[i].n == n && !strcmp(g_listen[i].type, type))
                ran |= fire_cb(g_listen[i].cb, type);
    }
    g_log = NULL;
    return ran;
}
