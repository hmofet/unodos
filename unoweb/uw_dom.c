/* ===========================================================================
 * unoweb DOM store - the arena, interned names, nodes, attributes, mutation,
 * queries and serialization.
 * ======================================================================== */
#include "uw_int.h"
#include <stdlib.h>
#include <stdio.h>

/* ---- arena --------------------------------------------------------------- */
static void *raw_alloc(uw_doc *d, size_t n)
{
    void *p = d->cfg.alloc ? d->cfg.alloc(d->cfg.alloc_user, n) : malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

static void raw_free(uw_doc *d, void *p)
{
    if (!p) return;
    if (d->cfg.free) d->cfg.free(d->cfg.alloc_user, p);
    else free(p);
}

void *uw_arena(uw_doc *d, size_t n)
{
    uw_chunk *c = d->chunks;
    n = (n + 7u) & ~(size_t)7u;                    /* 8-byte alignment */
    if (c && c->cap - c->used >= n) {
        void *p = c->data + c->used;
        c->used += n;
        return p;
    }
    {   size_t want = n + sizeof(uw_chunk);
        size_t cap = want > UW_CHUNK_MIN ? want : UW_CHUNK_MIN;
        uw_chunk *nc;
        /* soft_max is the PARSE's share of the arena (see uw_parse_begin): the
         * tree must not be allowed to spend everything, because style, layout
         * and paint each need more than it does and the arena never frees. */
        {   size_t lim = (d->soft_max && d->soft_max < d->max) ? d->soft_max : d->max;
            if (d->used + cap > lim) { d->truncated = 1; return NULL; } }
        nc = (uw_chunk *)raw_alloc(d, cap);
        if (!nc) { d->truncated = 1; return NULL; }
        nc->cap = cap - sizeof(uw_chunk);
        nc->used = n;
        nc->next = d->chunks;
        d->chunks = nc;
        d->used += cap;
        return nc->data;
    }
}

char *uw_arena_str(uw_doc *d, const char *s, int len)
{
    char *p;
    if (len < 0) len = s ? (int)strlen(s) : 0;
    p = (char *)uw_arena(d, (size_t)len + 1);
    if (!p) return NULL;
    if (len && s) memcpy(p, s, (size_t)len);
    p[len] = 0;
    return p;
}

/* ---- ASCII helpers ------------------------------------------------------- */
int uw_lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int uw_ieq(const char *a, int alen, const char *b, int blen)
{
    int i;
    if (alen != blen) return 0;
    for (i = 0; i < alen; i++) if (uw_lc((u8)a[i]) != uw_lc((u8)b[i])) return 0;
    return 1;
}

/* ---- atoms ---------------------------------------------------------------
 * Names are folded to lower case on the way in: HTML tag and attribute names
 * are ASCII case-insensitive, so `<DIV CLASS=x>` and `<div class=x>` must
 * intern to the same atom and compare as pointers-equal thereafter. */
static u32 name_hash(const char *s, int n)
{
    u32 h = 2166136261u; int i;
    for (i = 0; i < n; i++) { h ^= (u8)uw_lc((u8)s[i]); h *= 16777619u; }
    return h ? h : 1;
}

static void atom_rehash(uw_doc *d, u32 newcap)
{
    u32 *tab = (u32 *)raw_alloc(d, (size_t)newcap * sizeof *tab);
    u32 i;
    if (!tab) return;
    for (i = 1; i < d->natoms; i++) {
        u32 j = d->atoms[i].hash & (newcap - 1);
        while (tab[j]) j = (j + 1) & (newcap - 1);
        tab[j] = i;
    }
    raw_free(d, d->atom_hash);
    d->atom_hash = tab;
    d->atom_hashcap = newcap;
}

uw_atom uw_intern(uw_doc *d, const char *s, int len)
{
    u32 h, j, idx;
    if (!s) return 0;
    if (len < 0) len = (int)strlen(s);
    if (!len) return 0;
    if (!d->atom_hashcap) atom_rehash(d, 256);
    if (!d->atom_hash) return 0;
    h = name_hash(s, len);
    j = h & (d->atom_hashcap - 1);
    while ((idx = d->atom_hash[j])) {
        if (d->atoms[idx].hash == h && (int)d->atoms[idx].len == len &&
            uw_ieq(d->atoms[idx].s, len, s, len))
            return idx;
        j = (j + 1) & (d->atom_hashcap - 1);
    }
    if (d->natoms + 1 >= d->atomcap) {
        u32 nc = d->atomcap ? d->atomcap * 2 : 128;
        void *na = raw_alloc(d, (size_t)nc * sizeof *d->atoms);
        if (!na) return 0;
        if (d->atoms) {
            memcpy(na, d->atoms, (size_t)d->natoms * sizeof *d->atoms);
            raw_free(d, d->atoms);
        }
        d->atoms = na;
        d->atomcap = nc;
        if (!d->natoms) d->natoms = 1;              /* reserve index 0 */
    }
    if (!d->natoms) d->natoms = 1;
    {   char *cp = (char *)uw_arena(d, (size_t)len + 1);
        int i;
        if (!cp) return 0;
        for (i = 0; i < len; i++) cp[i] = (char)uw_lc((u8)s[i]);
        cp[len] = 0;
        idx = d->natoms++;
        d->atoms[idx].s = cp;
        d->atoms[idx].len = (u32)len;
        d->atoms[idx].hash = h;
        d->atom_hash[j] = idx;
        if (d->natoms * 2 > d->atom_hashcap) atom_rehash(d, d->atom_hashcap * 2);
        return idx;
    }
}

const char *uw_atom_name(uw_doc *d, uw_atom a)
{ return (a && a < d->natoms) ? d->atoms[a].s : ""; }

/* ---- tag classification --------------------------------------------------
 * Looked up by name once per intern would be nicer, but the sets are tiny and
 * the comparison is an atom-to-string check only on element creation. */
static int name_in(uw_doc *d, uw_atom a, const char *const *list)
{
    const char *n = uw_atom_name(d, a);
    int i;
    for (i = 0; list[i]; i++) if (!strcmp(n, list[i])) return 1;
    return 0;
}

int uw_is_void(uw_doc *d, uw_atom tag)
{
    static const char *const v[] = { "area","base","br","col","embed","hr","img",
        "input","link","meta","param","source","track","wbr", NULL };
    return name_in(d, tag, v);
}

int uw_raw_kind(uw_doc *d, uw_atom tag)
{
    const char *n = uw_atom_name(d, tag);
    if (!strcmp(n, "script")) return 3;
    if (!strcmp(n, "style")) return 2;
    if (!strcmp(n, "title") || !strcmp(n, "textarea")) return 1;
    return 0;
}

/* ---- documents ----------------------------------------------------------- */
uw_doc *uw_doc_new(const uw_config *cfg)
{
    uw_doc *d;
    uw_config c;
    memset(&c, 0, sizeof c);
    if (cfg) c = *cfg;
    d = (uw_doc *)(c.alloc ? c.alloc(c.alloc_user, sizeof *d) : calloc(1, sizeof *d));
    if (!d) return NULL;
    memset(d, 0, sizeof *d);
    d->cfg = c;
    d->max = c.arena_max ? c.arena_max : (16u << 20);
    d->max_depth = c.max_depth ? c.max_depth : 256;
    d->document = uw_node_new(d, UW_NODE_DOCUMENT);
    if (!d->document) { uw_doc_free(d); return NULL; }
    uw_paint_reserve(d);             /* before the tree can eat the arena */
    return d;
}

void uw_doc_free(uw_doc *d)
{
    uw_chunk *c, *n;
    if (!d) return;
    for (c = d->chunks; c; c = n) { n = c->next; raw_free(d, c); }
    raw_free(d, d->atoms);
    raw_free(d, d->atom_hash);
    raw_free(d, d->ids);
    if (d->cfg.free) d->cfg.free(d->cfg.alloc_user, d);
    else free(d);
}

int    uw_doc_truncated(uw_doc *d) { return d ? d->truncated : 0; }
size_t uw_doc_used(uw_doc *d)      { return d ? d->used : 0; }
uw_node *uw_document(uw_doc *d)    { return d ? d->document : NULL; }
uw_node *uw_root(uw_doc *d)        { return d ? d->html : NULL; }
uw_node *uw_head(uw_doc *d)        { return d ? d->head : NULL; }
uw_node *uw_body(uw_doc *d)        { return d ? d->body : NULL; }

/* ---- nodes --------------------------------------------------------------- */
uw_node *uw_node_new(uw_doc *d, int type)
{
    uw_node *n = (uw_node *)uw_arena(d, sizeof(uw_node));
    if (!n) return NULL;
    n->type = (u8)type;
    return n;
}

uw_node_type uw_type(uw_node *n)      { return n ? (uw_node_type)n->type : 0; }
uw_atom      uw_tag(uw_node *n)       { return n ? n->tag : 0; }
uw_node     *uw_parent(uw_node *n)    { return n ? n->parent : NULL; }
uw_node     *uw_first_child(uw_node *n){ return n ? n->first : NULL; }
uw_node     *uw_last_child(uw_node *n){ return n ? n->last : NULL; }
uw_node     *uw_next_sibling(uw_node *n){ return n ? n->next : NULL; }
uw_node     *uw_prev_sibling(uw_node *n){ return n ? n->prev : NULL; }
unsigned     uw_dirty(uw_node *n)     { return n ? n->dirty : 0; }

const char *uw_tag_name(uw_doc *d, uw_node *n)
{ return (n && n->type == UW_NODE_ELEMENT) ? uw_atom_name(d, n->tag) : ""; }

const char *uw_text(uw_node *n, int *len)
{
    if (!n || (n->type != UW_NODE_TEXT && n->type != UW_NODE_COMMENT &&
               n->type != UW_NODE_DOCTYPE)) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = (int)n->tlen;
    return n->text ? n->text : "";
}

void uw_mark_dirty(uw_node *n, unsigned bits)
{
    uw_node *p;
    if (!n) return;
    n->dirty |= (u16)bits;
    for (p = n->parent; p; p = p->parent) p->dirty |= UW_DIRTY_SUBTREE | UW_DIRTY_LAYOUT;
}

static void clear_dirty_walk(uw_node *n)
{
    for (; n; n = n->next) { n->dirty = 0; clear_dirty_walk(n->first); }
}

void uw_clear_dirty(uw_doc *d) { if (d) clear_dirty_walk(d->document); }

/* ---- tree mutation ------------------------------------------------------- */
static void unlink_node(uw_node *n)
{
    if (!n->parent) return;
    if (n->prev) n->prev->next = n->next; else n->parent->first = n->next;
    if (n->next) n->next->prev = n->prev; else n->parent->last = n->prev;
    n->parent = n->prev = n->next = NULL;
}

int uw_insert_before(uw_doc *d, uw_node *parent, uw_node *child, uw_node *ref)
{
    if (!parent || !child || child == parent) return -1;
    if (parent->flags & UW_F_VOID) return -1;
    if (ref && ref->parent != parent) return -1;
    /* a node may not become its own ancestor */
    {   uw_node *p;
        for (p = parent; p; p = p->parent) if (p == child) return -1; }
    unlink_node(child);
    child->parent = parent;
    if (ref) {
        child->prev = ref->prev;
        child->next = ref;
        if (ref->prev) ref->prev->next = child; else parent->first = child;
        ref->prev = child;
    } else {
        child->prev = parent->last;
        child->next = NULL;
        if (parent->last) parent->last->next = child; else parent->first = child;
        parent->last = child;
    }
    uw_mark_dirty(child, UW_DIRTY_STYLE | UW_DIRTY_LAYOUT);
    (void)d;
    return 0;
}

int uw_append(uw_doc *d, uw_node *parent, uw_node *child)
{ return uw_insert_before(d, parent, child, NULL); }

int uw_remove(uw_doc *d, uw_node *n)
{
    uw_node *p;
    if (!n || !n->parent) return -1;
    p = n->parent;
    unlink_node(n);
    uw_mark_dirty(p, UW_DIRTY_LAYOUT | UW_DIRTY_SUBTREE);
    (void)d;
    return 0;
}

uw_node *uw_create_element(uw_doc *d, const char *tag)
{
    uw_node *n;
    if (!d || !tag) return NULL;
    n = uw_node_new(d, UW_NODE_ELEMENT);
    if (!n) return NULL;
    n->tag = uw_intern(d, tag, -1);
    if (uw_is_void(d, n->tag)) n->flags |= UW_F_VOID;
    return n;
}

uw_node *uw_create_text(uw_doc *d, const char *text, int len)
{
    uw_node *n;
    if (!d) return NULL;
    if (len < 0) len = text ? (int)strlen(text) : 0;
    n = uw_node_new(d, UW_NODE_TEXT);
    if (!n) return NULL;
    n->text = uw_arena_str(d, text, len);
    if (!n->text) return NULL;
    n->tlen = (u32)len;
    return n;
}

int uw_set_text(uw_doc *d, uw_node *n, const char *text, int len)
{
    if (!n) return -1;
    if (len < 0) len = text ? (int)strlen(text) : 0;
    if (n->type == UW_NODE_ELEMENT) {              /* textContent = ... */
        uw_node *t;
        while (n->first) uw_remove(d, n->first);
        t = uw_create_text(d, text, len);
        if (!t) return -1;
        return uw_append(d, n, t);
    }
    n->text = uw_arena_str(d, text, len);
    if (!n->text) return -1;
    n->tlen = (u32)len;
    uw_mark_dirty(n, UW_DIRTY_LAYOUT);
    return 0;
}

/* ---- attributes ---------------------------------------------------------- */
static uw_attr_ent *attr_find(uw_node *n, uw_atom name)
{
    uw_attr_ent *a;
    if (!n) return NULL;
    for (a = n->attrs; a; a = a->next) if (a->name == name) return a;
    return NULL;
}

int uw_set_attr(uw_doc *d, uw_node *n, const char *name, const char *value)
{
    uw_atom a;
    uw_attr_ent *e;
    int vlen;
    if (!d || !n || n->type != UW_NODE_ELEMENT || !name) return -1;
    a = uw_intern(d, name, -1);
    if (!a) return -1;
    vlen = value ? (int)strlen(value) : 0;
    e = attr_find(n, a);
    if (!e) {
        e = (uw_attr_ent *)uw_arena(d, sizeof *e);
        if (!e) return -1;
        e->name = a;
        /* append, so attribute ORDER matches the source - the golden dumps
         * and innerHTML serialization both depend on it being stable */
        if (!n->attrs) n->attrs = e;
        else { uw_attr_ent *t = n->attrs; while (t->next) t = t->next; t->next = e; }
    }
    e->value = uw_arena_str(d, value ? value : "", vlen);
    if (!e->value) return -1;
    e->vlen = (u32)vlen;
    if (!strcmp(uw_atom_name(d, a), "id")) uw_index_id(d, n, e->value, vlen);
    uw_mark_dirty(n, UW_DIRTY_STYLE | UW_DIRTY_SUBTREE | UW_DIRTY_LAYOUT);
    return 0;
}

int uw_remove_attr(uw_doc *d, uw_node *n, const char *name)
{
    uw_atom a;
    uw_attr_ent **pp;
    if (!d || !n || !name) return -1;
    a = uw_intern(d, name, -1);
    for (pp = &n->attrs; *pp; pp = &(*pp)->next)
        if ((*pp)->name == a) { *pp = (*pp)->next;
                                uw_mark_dirty(n, UW_DIRTY_STYLE | UW_DIRTY_LAYOUT);
                                return 0; }
    return -1;
}

const char *uw_attr(uw_doc *d, uw_node *n, const char *name)
{
    uw_attr_ent *e;
    if (!d || !n || !name) return NULL;
    e = attr_find(n, uw_intern(d, name, -1));
    return e ? e->value : NULL;
}

int uw_has_attr(uw_doc *d, uw_node *n, const char *name)
{ return uw_attr(d, n, name) != NULL; }

int uw_nattrs(uw_node *n)
{
    int c = 0;
    uw_attr_ent *a;
    if (!n) return 0;
    for (a = n->attrs; a; a = a->next) c++;
    return c;
}

int uw_attr_at(uw_doc *d, uw_node *n, int i, const char **name, const char **value)
{
    uw_attr_ent *a;
    if (!n || i < 0) return -1;
    for (a = n->attrs; a && i; a = a->next) i--;
    if (!a) return -1;
    if (name)  *name  = uw_atom_name(d, a->name);
    if (value) *value = a->value;
    return 0;
}

/* ---- id index ------------------------------------------------------------ */
void uw_index_id(uw_doc *d, uw_node *n, const char *id, int len)
{
    uw_atom a = uw_intern(d, id, len);
    u32 i;
    if (!a) return;
    for (i = 0; i < d->nids; i++)
        if (d->ids[i].id == a) { d->ids[i].n = n; return; }
    if (d->nids == d->idcap) {
        u32 nc = d->idcap ? d->idcap * 2 : 32;
        void *ni = raw_alloc(d, (size_t)nc * sizeof *d->ids);
        if (!ni) return;
        if (d->ids) { memcpy(ni, d->ids, (size_t)d->nids * sizeof *d->ids);
                      raw_free(d, d->ids); }
        d->ids = ni; d->idcap = nc;
    }
    d->ids[d->nids].id = a;
    d->ids[d->nids].n = n;
    d->nids++;
}

uw_node *uw_get_element_by_id(uw_doc *d, const char *id)
{
    uw_atom a;
    u32 i;
    if (!d || !id) return NULL;
    a = uw_intern(d, id, -1);
    for (i = 0; i < d->nids; i++)
        if (d->ids[i].id == a) {
            /* an indexed node may since have been detached; the index is a
             * cache, the tree is the truth */
            uw_node *n = d->ids[i].n, *p;
            for (p = n; p; p = p->parent) if (p == d->document) return n;
            return NULL;
        }
    return NULL;
}

/* ---- traversal ----------------------------------------------------------- */
uw_node *uw_next_in_order(uw_node *n, uw_node *root)
{
    if (!n) return NULL;
    if (n->first) return n->first;
    while (n && n != root) {
        if (n->next) return n->next;
        n = n->parent;
    }
    return NULL;
}

int uw_elements_by_tag(uw_doc *d, uw_node *root, const char *tag,
                       uw_node **out, int max)
{
    uw_atom a;
    uw_node *n;
    int count = 0, all;
    if (!d || !tag) return 0;
    all = !strcmp(tag, "*");
    a = all ? 0 : uw_intern(d, tag, -1);
    if (!root) root = d->document;
    for (n = uw_next_in_order(root, root); n; n = uw_next_in_order(n, root)) {
        if (n->type != UW_NODE_ELEMENT) continue;
        if (!all && n->tag != a) continue;
        if (out && count < max) out[count] = n;
        count++;
    }
    return count;
}

/* ---- output helpers ------------------------------------------------------
 * Every writer follows the same rule: append what fits, keep counting what
 * does not, and return the FULL length so a caller can size a buffer and try
 * again. Truncation therefore never looks like success. */
typedef struct { char *b; int max, n; } outbuf;

static void ob_put(outbuf *o, const char *s, int len)
{
    int room;
    if (len < 0) len = (int)strlen(s);
    room = o->max - 1 - o->n;
    if (room > 0) {
        int k = len < room ? len : room;
        memcpy(o->b + o->n, s, (size_t)k);
    }
    o->n += len;
    if (o->max > 0) o->b[o->n < o->max - 1 ? o->n : o->max - 1] = 0;
}

static void ob_esc(outbuf *o, const char *s, int len, int in_attr)
{
    int i, start = 0;
    for (i = 0; i < len; i++) {
        const char *r = NULL;
        switch (s[i]) {
        case '&': r = "&amp;"; break;
        case '<': r = in_attr ? NULL : "&lt;"; break;
        case '>': r = in_attr ? NULL : "&gt;"; break;
        case '"': r = in_attr ? "&quot;" : NULL; break;
        default: break;
        }
        if (!r) continue;
        if (i > start) ob_put(o, s + start, i - start);
        ob_put(o, r, -1);
        start = i + 1;
    }
    if (len > start) ob_put(o, s + start, len - start);
}

static void serialize_node(uw_doc *d, uw_node *n, outbuf *o);

static void serialize_children(uw_doc *d, uw_node *n, outbuf *o)
{ uw_node *c; for (c = n->first; c; c = c->next) serialize_node(d, c, o); }

static void serialize_node(uw_doc *d, uw_node *n, outbuf *o)
{
    switch (n->type) {
    case UW_NODE_TEXT: {
        uw_node *p = n->parent;
        /* inside <script>/<style> the content is raw, not escaped */
        if (p && p->type == UW_NODE_ELEMENT && uw_raw_kind(d, p->tag) >= 2)
            ob_put(o, n->text, (int)n->tlen);
        else
            ob_esc(o, n->text, (int)n->tlen, 0);
        break; }
    case UW_NODE_COMMENT:
        ob_put(o, "<!--", 4); ob_put(o, n->text, (int)n->tlen); ob_put(o, "-->", 3);
        break;
    case UW_NODE_DOCTYPE:
        ob_put(o, "<!DOCTYPE ", 10); ob_put(o, n->text, (int)n->tlen); ob_put(o, ">", 1);
        break;
    case UW_NODE_ELEMENT: {
        uw_attr_ent *a;
        ob_put(o, "<", 1);
        ob_put(o, uw_atom_name(d, n->tag), -1);
        for (a = n->attrs; a; a = a->next) {
            ob_put(o, " ", 1);
            ob_put(o, uw_atom_name(d, a->name), -1);
            ob_put(o, "=\"", 2);
            ob_esc(o, a->value, (int)a->vlen, 1);
            ob_put(o, "\"", 1);
        }
        ob_put(o, ">", 1);
        if (n->flags & UW_F_VOID) break;
        serialize_children(d, n, o);
        ob_put(o, "</", 2);
        ob_put(o, uw_atom_name(d, n->tag), -1);
        ob_put(o, ">", 1);
        break; }
    default:
        serialize_children(d, n, o);
        break;
    }
}

int uw_serialize(uw_doc *d, uw_node *n, char *out, int max)
{
    outbuf o;
    o.b = out; o.max = max; o.n = 0;
    if (max > 0) out[0] = 0;
    if (!d || !n) return 0;
    serialize_children(d, n, &o);
    return o.n;
}

/* ---- tree dump (the golden-test format) ---------------------------------- */
static void dump_node(uw_doc *d, uw_node *n, int depth, outbuf *o)
{
    int i;
    for (i = 0; i < depth; i++) ob_put(o, "  ", 2);
    switch (n->type) {
    case UW_NODE_DOCUMENT: ob_put(o, "#document", -1); break;
    case UW_NODE_DOCTYPE:  ob_put(o, "<!DOCTYPE ", -1);
                           ob_put(o, n->text, (int)n->tlen);
                           ob_put(o, ">", 1); break;
    case UW_NODE_COMMENT:  ob_put(o, "<!-- ", -1);
                           ob_put(o, n->text, (int)n->tlen);
                           ob_put(o, " -->", -1); break;
    case UW_NODE_TEXT:     ob_put(o, "\"", 1);
                           ob_put(o, n->text, (int)n->tlen);
                           ob_put(o, "\"", 1); break;
    case UW_NODE_ELEMENT: {
        uw_attr_ent *a;
        ob_put(o, uw_atom_name(d, n->tag), -1);
        for (a = n->attrs; a; a = a->next) {
            ob_put(o, " ", 1);
            ob_put(o, uw_atom_name(d, a->name), -1);
            ob_put(o, "=\"", 2);
            ob_put(o, a->value, (int)a->vlen);
            ob_put(o, "\"", 1);
        }
        break; }
    default: break;
    }
    ob_put(o, "\n", 1);
    { uw_node *c; for (c = n->first; c; c = c->next) dump_node(d, c, depth + 1, o); }
}

int uw_dump(uw_doc *d, uw_node *n, char *out, int max)
{
    outbuf o;
    o.b = out; o.max = max; o.n = 0;
    if (max > 0) out[0] = 0;
    if (!d) return 0;
    if (!n) n = d->document;
    dump_node(d, n, 0, &o);
    return o.n;
}

/* ---- textContent --------------------------------------------------------- */
int uw_text_content(uw_doc *d, uw_node *n, char *out, int max)
{
    outbuf o;
    uw_node *c;
    o.b = out; o.max = max; o.n = 0;
    if (max > 0) out[0] = 0;
    if (!d || !n) return 0;
    if (n->type == UW_NODE_TEXT) { ob_put(&o, n->text, (int)n->tlen); return o.n; }
    for (c = uw_next_in_order(n, n); c; c = uw_next_in_order(c, n))
        if (c->type == UW_NODE_TEXT) ob_put(&o, c->text, (int)c->tlen);
    return o.n;
}
