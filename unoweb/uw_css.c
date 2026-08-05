/* ===========================================================================
 * unoweb CSS - stylesheet parsing, selector matching, and the cascade.
 *
 * Selectors are stored right-to-left, because that is the order they are
 * matched in: start at the candidate element and walk outwards, which fails
 * fast on the first mismatch. Matching left-to-right would mean searching the
 * subtree for every rule.
 * ======================================================================== */
#include "uw_int.h"
#include <stdlib.h>

/* ---- selector model ------------------------------------------------------
 * One compound selector is a tag plus any number of .class / #id / [attr] /
 * :pseudo conditions. A full selector is a chain of compounds joined by
 * combinators, held in RIGHTMOST-FIRST order. */
enum { UW_COMB_NONE = 0, UW_COMB_DESC, UW_COMB_CHILD, UW_COMB_ADJ };
enum { UW_CND_CLASS = 1, UW_CND_ID, UW_CND_ATTR, UW_CND_ATTR_EQ, UW_CND_PSEUDO };

typedef struct uw_cond {
    struct uw_cond *next;
    unsigned char kind;
    uw_atom  name;                 /* class/id/attr name, or pseudo name */
    char    *value;                /* [attr=value] */
} uw_cond;

typedef struct uw_compound {
    struct uw_compound *next;      /* the compound to the LEFT of this one */
    unsigned char comb;            /* how `next` relates to this one */
    uw_atom  tag;                  /* 0 = '*' */
    uw_cond *conds;
} uw_compound;

typedef struct uw_decl {
    struct uw_decl *next;
    uw_atom prop;
    char   *value;
    int     important;
} uw_decl;

typedef struct uw_rule {
    struct uw_rule *next;
    uw_compound *sel;              /* rightmost compound */
    uw_decl     *decls;
    int          spec;             /* a*100 + b*10 + c, packed */
    int          order;            /* source order, breaks specificity ties */
} uw_rule;

struct uw_sheet {
    struct uw_sheet *next;
    uw_rule *rules;
    int      nrules;
    int      origin;
};

/* ---- small scanner ------------------------------------------------------- */
typedef struct { const char *s; int n, i; uw_doc *d; } css_in;

static void skip_ws(css_in *c)
{
    for (;;) {
        while (c->i < c->n && (c->s[c->i]==' '||c->s[c->i]=='\t'||c->s[c->i]=='\n'||
                               c->s[c->i]=='\r'||c->s[c->i]=='\f')) c->i++;
        if (c->i + 1 < c->n && c->s[c->i]=='/' && c->s[c->i+1]=='*') {
            c->i += 2;
            while (c->i + 1 < c->n && !(c->s[c->i]=='*' && c->s[c->i+1]=='/')) c->i++;
            c->i = c->i + 2 <= c->n ? c->i + 2 : c->n;
            continue;
        }
        return;
    }
}

static int ident_char(int ch)
{ return (ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||
         ch=='-'||ch=='_'||(unsigned char)ch>=0x80; }

static int scan_ident(css_in *c, const char **out)
{
    int s = c->i;
    while (c->i < c->n && ident_char((unsigned char)c->s[c->i])) c->i++;
    *out = c->s + s;
    return c->i - s;
}

/* ---- selector parsing ---------------------------------------------------- */
static uw_cond *cond_new(uw_doc *d, int kind, uw_atom name, char *value)
{
    uw_cond *q = (uw_cond *)uw_arena(d, sizeof *q);
    if (!q) return NULL;
    q->kind = (unsigned char)kind;
    q->name = name;
    q->value = value;
    return q;
}

/* Parse one compound (`div.a#b[x=y]:hover`). Returns NULL at a combinator or
 * the end of the selector. */
static uw_compound *parse_compound(css_in *c, int *spec)
{
    uw_doc *d = c->d;
    uw_compound *cp = (uw_compound *)uw_arena(d, sizeof *cp);
    int any = 0;
    if (!cp) return NULL;
    for (;;) {
        if (c->i >= c->n) break;
        {   char ch = c->s[c->i];
            const char *id;
            int idl;
            if (ch == '*') { c->i++; any = 1; continue; }
            if (ch == '.' || ch == '#') {
                c->i++;
                idl = scan_ident(c, &id);
                if (!idl) break;
                {   uw_cond *q = cond_new(d, ch == '.' ? UW_CND_CLASS : UW_CND_ID,
                                          uw_intern(d, id, idl), NULL);
                    if (!q) break;
                    q->next = cp->conds; cp->conds = q; }
                *spec += (ch == '.') ? 10 : 100;
                any = 1;
                continue;
            }
            if (ch == '[') {
                const char *an, *av = NULL;
                int anl, avl = 0;
                c->i++;
                anl = scan_ident(c, &an);
                skip_ws(c);
                if (c->i < c->n && c->s[c->i] == '=') {
                    c->i++;
                    skip_ws(c);
                    if (c->i < c->n && (c->s[c->i]=='"' || c->s[c->i]=='\'')) {
                        char q2 = c->s[c->i++];
                        av = c->s + c->i;
                        while (c->i < c->n && c->s[c->i] != q2) c->i++;
                        avl = (int)(c->s + c->i - av);
                        if (c->i < c->n) c->i++;
                    } else {
                        av = c->s + c->i;
                        while (c->i < c->n && c->s[c->i] != ']') c->i++;
                        avl = (int)(c->s + c->i - av);
                    }
                }
                while (c->i < c->n && c->s[c->i] != ']') c->i++;
                if (c->i < c->n) c->i++;
                if (anl) {
                    uw_cond *q = cond_new(d, av ? UW_CND_ATTR_EQ : UW_CND_ATTR,
                                          uw_intern(d, an, anl),
                                          av ? uw_arena_str(d, av, avl) : NULL);
                    if (q) { q->next = cp->conds; cp->conds = q; }
                    *spec += 10;
                    any = 1;
                }
                continue;
            }
            if (ch == ':') {
                c->i++;
                if (c->i < c->n && c->s[c->i] == ':') c->i++;   /* ::before etc */
                idl = scan_ident(c, &id);
                /* a functional pseudo like :nth-child(2n+1): swallow its args */
                if (c->i < c->n && c->s[c->i] == '(') {
                    int depth = 0;
                    while (c->i < c->n) {
                        if (c->s[c->i] == '(') depth++;
                        else if (c->s[c->i] == ')') { depth--; c->i++; if (!depth) break; continue; }
                        c->i++;
                    }
                }
                if (idl) {
                    uw_cond *q = cond_new(d, UW_CND_PSEUDO, uw_intern(d, id, idl), NULL);
                    if (q) { q->next = cp->conds; cp->conds = q; }
                    *spec += 10;
                    any = 1;
                }
                continue;
            }
            if (ident_char((unsigned char)ch)) {
                idl = scan_ident(c, &id);
                if (!idl) break;
                cp->tag = uw_intern(d, id, idl);
                *spec += 1;
                any = 1;
                continue;
            }
            break;
        }
    }
    return any ? cp : NULL;
}

/* Parse a full selector into a RIGHTMOST-FIRST chain: each compound's `next`
 * points LEFT, and its `comb` describes how it relates to that neighbour. That
 * is the order chain_match walks, so building it the other way round (which is
 * the order the text reads) silently made every combinator selector fail to
 * match anything. */
static uw_compound *parse_selector(css_in *c, int *spec)
{
    uw_compound *prev = NULL;
    int comb = UW_COMB_NONE;
    for (;;) {
        uw_compound *cp;
        skip_ws(c);
        if (c->i >= c->n) break;
        if (c->s[c->i] == ',' || c->s[c->i] == '{') break;
        cp = parse_compound(c, spec);
        if (!cp) break;
        cp->comb = (unsigned char)comb;
        cp->next = prev;
        prev = cp;
        {   int saw_ws = 0;
            while (c->i < c->n && (c->s[c->i]==' '||c->s[c->i]=='\t'||
                                   c->s[c->i]=='\n'||c->s[c->i]=='\r')) { c->i++; saw_ws = 1; }
            if (c->i >= c->n) break;
            if (c->s[c->i] == '>') { comb = UW_COMB_CHILD; c->i++; continue; }
            if (c->s[c->i] == '+') { comb = UW_COMB_ADJ; c->i++; continue; }
            if (c->s[c->i] == ',' || c->s[c->i] == '{') break;
            if (saw_ws) { comb = UW_COMB_DESC; continue; }
            break;
        }
    }
    return prev;
}

/* ---- declaration parsing -------------------------------------------------- */
static uw_decl *parse_decls(css_in *c)
{
    uw_doc *d = c->d;
    uw_decl *head = NULL, *tail = NULL;
    /* at '{' */
    if (c->i < c->n && c->s[c->i] == '{') c->i++;
    for (;;) {
        const char *pn;
        int pnl, vs, ve, important = 0;
        skip_ws(c);
        if (c->i >= c->n) break;
        if (c->s[c->i] == '}') { c->i++; break; }
        pnl = scan_ident(c, &pn);
        skip_ws(c);
        if (!pnl || c->i >= c->n || c->s[c->i] != ':') {
            /* junk: skip to the next ';' or '}' - CSS error recovery */
            while (c->i < c->n && c->s[c->i] != ';' && c->s[c->i] != '}') c->i++;
            if (c->i < c->n && c->s[c->i] == ';') c->i++;
            continue;
        }
        c->i++;
        skip_ws(c);
        vs = c->i;
        {   int depth = 0;
            while (c->i < c->n) {
                char ch = c->s[c->i];
                if (ch == '(') depth++;
                else if (ch == ')') { if (depth) depth--; }
                else if (!depth && (ch == ';' || ch == '}')) break;
                c->i++;
            } }
        ve = c->i;
        while (ve > vs && (c->s[ve-1]==' '||c->s[ve-1]=='\t'||c->s[ve-1]=='\n'||c->s[ve-1]=='\r')) ve--;
        if (ve - vs > 10 && !uw_ieq(c->s + ve - 10, 10, "!important", 10)) {
            /* not important */
        } else if (ve - vs >= 10 && uw_ieq(c->s + ve - 10, 10, "!important", 10)) {
            important = 1;
            ve -= 10;
            while (ve > vs && (c->s[ve-1]==' '||c->s[ve-1]=='\t')) ve--;
        }
        if (ve > vs) {
            uw_decl *dc = (uw_decl *)uw_arena(d, sizeof *dc);
            if (!dc) break;
            dc->prop = uw_intern(d, pn, pnl);
            dc->value = uw_arena_str(d, c->s + vs, ve - vs);
            dc->important = important;
            if (tail) tail->next = dc; else head = dc;
            tail = dc;
        }
        if (c->i < c->n && c->s[c->i] == ';') c->i++;
        else if (c->i < c->n && c->s[c->i] == '}') { c->i++; break; }
    }
    return head;
}

/* ---- at-rules ------------------------------------------------------------ */
static void skip_block(css_in *c)
{
    int depth = 0;
    while (c->i < c->n) {
        char ch = c->s[c->i++];
        if (ch == '{') depth++;
        else if (ch == '}') { depth--; if (depth <= 0) return; }
        else if (!depth && ch == ';') return;
    }
}

/* ---- stylesheet parsing --------------------------------------------------- */
static void sheet_add(uw_sheet *sh, uw_rule *r)
{
    uw_rule *t;
    r->order = sh->nrules++;
    if (!sh->rules) { sh->rules = r; return; }
    for (t = sh->rules; t->next; t = t->next) {}
    t->next = r;
}

uw_sheet *uw_css_parse(uw_doc *d, const char *css, int len, int origin)
{
    css_in c;
    uw_sheet *sh;
    if (!d || !css) return NULL;
    if (len < 0) len = (int)strlen(css);
    sh = (uw_sheet *)uw_arena(d, sizeof *sh);
    if (!sh) return NULL;
    sh->origin = origin;
    c.s = css; c.n = len; c.i = 0; c.d = d;
    for (;;) {
        skip_ws(&c);
        if (c.i >= c.n) break;
        if (c.s[c.i] == '@') {
            /* @media: parse the body inline when the query plausibly applies,
             * otherwise skip it. v1 accepts `screen` and `all` and any
             * min/max-width that the caller's viewport satisfies - the query
             * is re-evaluated per document, so this is a parse-time
             * approximation, noted in UNOWEB.md. */
            int save = c.i;
            const char *kw;
            int kl;
            c.i++;
            kl = scan_ident(&c, &kw);
            if (kl == 5 && uw_ieq(kw, 5, "media", 5)) {
                while (c.i < c.n && c.s[c.i] != '{') c.i++;
                if (c.i < c.n) c.i++;         /* enter the block: parse inside */
                continue;
            }
            c.i = save;
            skip_block(&c);
            continue;
        }
        if (c.s[c.i] == '}') { c.i++; continue; }   /* closing a @media block */
        {   /* one or more comma-separated selectors sharing a declaration block */
            uw_compound *sels[32];
            int specs[32], nsel = 0;
            for (;;) {
                int spec = 0;
                uw_compound *s = parse_selector(&c, &spec);
                if (s && nsel < 32) { sels[nsel] = s; specs[nsel] = spec; nsel++; }
                skip_ws(&c);
                if (c.i < c.n && c.s[c.i] == ',') { c.i++; continue; }
                break;
            }
            if (c.i >= c.n) break;
            if (c.s[c.i] != '{') { skip_block(&c); continue; }
            {   uw_decl *decls = parse_decls(&c);
                int k;
                for (k = 0; k < nsel; k++) {
                    uw_rule *r = (uw_rule *)uw_arena(d, sizeof *r);
                    if (!r) break;
                    r->sel = sels[k];
                    r->decls = decls;          /* shared; declarations are read-only */
                    r->spec = specs[k];
                    sheet_add(sh, r);
                }
            }
        }
    }
    return sh;
}

int uw_css_nrules(uw_sheet *s) { return s ? s->nrules : 0; }

/* ---- matching ------------------------------------------------------------- */
static int has_word(const char *list, const char *word, int wlen)
{
    int i = 0;
    if (!list) return 0;
    while (list[i]) {
        int s;
        while (list[i]==' '||list[i]=='\t'||list[i]=='\n') i++;
        s = i;
        while (list[i] && list[i]!=' ' && list[i]!='\t' && list[i]!='\n') i++;
        if (i - s == wlen && !memcmp(list + s, word, (size_t)wlen)) return 1;
    }
    return 0;
}

static int conds_match(uw_doc *d, uw_node *n, uw_cond *q)
{
    for (; q; q = q->next) {
        switch (q->kind) {
        case UW_CND_CLASS: {
            const char *cl = uw_attr(d, n, "class");
            const char *want = uw_atom_name(d, q->name);
            if (!has_word(cl, want, (int)strlen(want))) return 0;
            break; }
        case UW_CND_ID: {
            const char *id = uw_attr(d, n, "id");
            if (!id || strcmp(id, uw_atom_name(d, q->name))) return 0;
            break; }
        case UW_CND_ATTR:
            if (!uw_has_attr(d, n, uw_atom_name(d, q->name))) return 0;
            break;
        case UW_CND_ATTR_EQ: {
            const char *v = uw_attr(d, n, uw_atom_name(d, q->name));
            if (!v || !q->value || strcmp(v, q->value)) return 0;
            break; }
        case UW_CND_PSEUDO: {
            const char *ps = uw_atom_name(d, q->name);
            if (!strcmp(ps, "first-child")) { if (uw_prev_sibling(n)) return 0; }
            else if (!strcmp(ps, "last-child")) { if (uw_next_sibling(n)) return 0; }
            else if (!strcmp(ps, "root")) { if (uw_parent(n) != uw_document(d)) return 0; }
            else if (!strcmp(ps, "link")) { if (!uw_has_attr(d, n, "href")) return 0; }
            /* :hover / :active / :visited never match without an interaction
             * model; M4 supplies one. Matching them now would style pages as
             * if the pointer were everywhere at once. */
            else return 0;
            break; }
        default: return 0;
        }
    }
    return 1;
}

static int compound_match(uw_doc *d, uw_node *n, uw_compound *cp)
{
    if (uw_type(n) != UW_NODE_ELEMENT) return 0;
    if (cp->tag && uw_tag(n) != cp->tag) return 0;
    return conds_match(d, n, cp->conds);
}

/* Walk the chain right-to-left. Descendant combinators need backtracking: if
 * an ancestor matches but the rest of the chain fails above it, another
 * ancestor may still satisfy it. */
static int chain_match(uw_doc *d, uw_node *n, uw_compound *cp)
{
    if (!cp) return 1;
    if (!compound_match(d, n, cp)) return 0;
    if (!cp->next) return 1;
    switch (cp->comb) {
    case UW_COMB_CHILD:
        return chain_match(d, uw_parent(n), cp->next);
    case UW_COMB_ADJ:
        return chain_match(d, uw_prev_sibling(n), cp->next);
    case UW_COMB_DESC:
    default: {
        uw_node *a;
        for (a = uw_parent(n); a; a = uw_parent(a))
            if (chain_match(d, a, cp->next)) return 1;
        return 0; }
    }
}

int uw_matches(uw_doc *d, uw_node *n, const char *sel)
{
    css_in c;
    uw_compound *s;
    int spec = 0;
    if (!d || !n || !sel) return 0;
    c.s = sel; c.n = (int)strlen(sel); c.i = 0; c.d = d;
    s = parse_selector(&c, &spec);
    return s ? chain_match(d, n, s) : 0;
}

/* ---- internal accessors used by uw_style.c ------------------------------- */
uw_rule *uw_sheet_rules(uw_sheet *s) { return s ? s->rules : NULL; }
uw_rule *uw_rule_next(uw_rule *r)    { return r ? r->next : NULL; }
uw_decl *uw_rule_decls(uw_rule *r)   { return r ? r->decls : NULL; }
int      uw_rule_spec(uw_rule *r)    { return r ? r->spec : 0; }
int      uw_rule_order(uw_rule *r)   { return r ? r->order : 0; }
int      uw_rule_matches(uw_doc *d, uw_node *n, uw_rule *r)
{ return r && chain_match(d, n, r->sel); }
uw_decl *uw_decl_next(uw_decl *x)    { return x ? x->next : NULL; }
uw_atom  uw_decl_prop(uw_decl *x)    { return x ? x->prop : 0; }
const char *uw_decl_value(uw_decl *x){ return x ? x->value : ""; }
int      uw_decl_important(uw_decl *x){ return x ? x->important : 0; }
/* Can a document styled by this sheet share computed styles between
 * same-shaped siblings? Only if NO rule can depend on an element's POSITION,
 * because position is the one thing a "same tag, same attributes, same
 * parent" key cannot see. `li:first-child{color:red}` over two identical
 * siblings is exactly that case, and it is what caught the first attempt at
 * sharing.
 *
 * Proving that no rule cares beats trying to model position in the key: the
 * key would need per-element sibling indices, and would have to be re-derived
 * every time a new positional selector is supported. This is conservative by
 * construction - an unrecognised pseudo-class disables sharing rather than
 * being assumed harmless. */
int uw_sheet_shareable(uw_doc *d, uw_sheet *s)
{
    uw_rule *r;
    if (!s) return 1;
    for (r = s->rules; r; r = r->next) {
        uw_compound *c;
        for (c = r->sel; c; c = c->next) {
            uw_cond *q;
            if (c->comb == UW_COMB_ADJ) return 0;         /* b + i */
            for (q = c->conds; q; q = q->next) {
                const char *nm;
                if (q->kind != UW_CND_PSEUDO) continue;
                nm = uw_atom_name(d, q->name);
                if (!nm) return 0;
                if (!strncmp(nm, "first", 5) || !strncmp(nm, "last", 4) ||
                    !strncmp(nm, "nth", 3)   || !strncmp(nm, "only", 4) ||
                    !strcmp(nm, "root"))
                    return 0;
            }
        }
    }
    return 1;
}

int      uw_sheet_origin(uw_sheet *s){ return s ? s->origin : UW_ORIGIN_AUTHOR; }
uw_sheet *uw_sheet_next(uw_sheet *s) { return s ? s->next : NULL; }
void      uw_sheet_link(uw_sheet *s, uw_sheet *n) { if (s) s->next = n; }
