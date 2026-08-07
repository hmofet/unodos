/* ===========================================================================
 * unoweb HTML parser - a streaming tokenizer plus a tree builder that does
 * the error recovery real pages depend on.
 *
 * STREAMING STRATEGY. The tokenizer is not a resumable state machine; it is a
 * scanner over a pending buffer that only ever consumes COMPLETE tokens and
 * leaves a partial tail for the next feed. That is a deliberate trade: a
 * resumable machine needs every state duplicated with a "we ran out of input
 * here" exit, which is where hand-written HTML parsers accumulate their bugs.
 * Buffering costs memory bounded by UW_PENDING_MAX and buys a scanner that is
 * obviously correct at every call. It also makes document.write fall out for
 * free - uw_parse_insert just splices text in at the read cursor, which IS the
 * spec's "insertion point".
 * ======================================================================== */
#include "uw_int.h"
#include <stdlib.h>

#define UW_MAX_ATTRS   64
#define UW_PENDING_MAX (4u * 1024u * 1024u)

/* ---- tokens -------------------------------------------------------------- */
enum { T_NONE = 0, T_TEXT, T_START, T_END, T_COMMENT, T_DOCTYPE };

typedef struct {
    int kind;
    const char *name; int nlen;
    int self_closing;
    struct { const char *n; int nlen; const char *v; int vlen; } at[UW_MAX_ATTRS];
    int nat;
    const char *text; int tlen;
} uw_tok;

struct uw_parser {
    uw_doc  *d;
    uw_hooks hooks;
    int      have_hooks;

    char    *buf;
    int      blen, bcap, pos;

    uw_node **open;
    int       nopen, opencap;

    int       in_head_only;      /* nothing has forced <body> open yet */
    uw_atom   raw_tag;           /* inside RCDATA/RAWTEXT/script, else 0 */
    int       raw_kind;
    int       in_script_hook;
    int       stopped;
};

/* ---- entity decoding -----------------------------------------------------
 * The named set is the practical subset, not the full 2231-entry table: these
 * are what actually appears in prose, and an unknown reference is left alone
 * (which is also what a browser does with `&foo;`). The full generated table
 * is an M2b item - see UNOWEB.md. */
static const struct { const char *name; const char *utf8; } g_ents[] = {
    {"amp","&"},{"lt","<"},{"gt",">"},{"quot","\""},{"apos","'"},
    {"nbsp","\xC2\xA0"},{"copy","\xC2\xA9"},{"reg","\xC2\xAE"},
    {"trade","\xE2\x84\xA2"},{"hellip","\xE2\x80\xA6"},
    {"mdash","\xE2\x80\x94"},{"ndash","\xE2\x80\x93"},
    {"lsquo","\xE2\x80\x98"},{"rsquo","\xE2\x80\x99"},
    {"ldquo","\xE2\x80\x9C"},{"rdquo","\xE2\x80\x9D"},
    {"bull","\xE2\x80\xA2"},{"middot","\xC2\xB7"},{"sect","\xC2\xA7"},
    {"para","\xC2\xB6"},{"dagger","\xE2\x80\xA0"},{"permil","\xE2\x80\xB0"},
    {"times","\xC3\x97"},{"divide","\xC3\xB7"},{"plusmn","\xC2\xB1"},
    {"minus","\xE2\x88\x92"},{"deg","\xC2\xB0"},{"micro","\xC2\xB5"},
    {"frac12","\xC2\xBD"},{"frac14","\xC2\xBC"},{"frac34","\xC2\xBE"},
    {"sup2","\xC2\xB2"},{"sup3","\xC2\xB3"},
    {"euro","\xE2\x82\xAC"},{"pound","\xC2\xA3"},{"yen","\xC2\xA5"},
    {"cent","\xC2\xA2"},{"curren","\xC2\xA4"},
    {"laquo","\xC2\xAB"},{"raquo","\xC2\xBB"},
    {"larr","\xE2\x86\x90"},{"rarr","\xE2\x86\x92"},
    {"uarr","\xE2\x86\x91"},{"darr","\xE2\x86\x93"},{"harr","\xE2\x86\x94"},
    {"ne","\xE2\x89\xA0"},{"le","\xE2\x89\xA4"},{"ge","\xE2\x89\xA5"},
    {"asymp","\xE2\x89\x88"},{"infin","\xE2\x88\x9E"},{"radic","\xE2\x88\x9A"},
    {"sum","\xE2\x88\x91"},{"prod","\xE2\x88\x8F"},{"int","\xE2\x88\xAB"},
    {"alpha","\xCE\xB1"},{"beta","\xCE\xB2"},{"gamma","\xCE\xB3"},
    {"delta","\xCE\xB4"},{"epsilon","\xCE\xB5"},{"theta","\xCE\xB8"},
    {"lambda","\xCE\xBB"},{"mu","\xCE\xBC"},{"pi","\xCF\x80"},
    {"sigma","\xCF\x83"},{"tau","\xCF\x84"},{"phi","\xCF\x86"},
    {"omega","\xCF\x89"},{"Omega","\xCE\xA9"},{"Delta","\xCE\x94"},
    {"Sigma","\xCE\xA3"},{"Pi","\xCE\xA0"},
    {"ensp","\xE2\x80\x82"},{"emsp","\xE2\x80\x83"},{"thinsp","\xE2\x80\x89"},
    {"shy","\xC2\xAD"},{"iexcl","\xC2\xA1"},{"iquest","\xC2\xBF"},
    {"aacute","\xC3\xA1"},{"eacute","\xC3\xA9"},{"iacute","\xC3\xAD"},
    {"oacute","\xC3\xB3"},{"uacute","\xC3\xBA"},{"ntilde","\xC3\xB1"},
    {"agrave","\xC3\xA0"},{"egrave","\xC3\xA8"},{"ccedil","\xC3\xA7"},
    {"auml","\xC3\xA4"},{"ouml","\xC3\xB6"},{"uuml","\xC3\xBC"},
    {"szlig","\xC3\x9F"},{"aring","\xC3\xA5"},{"oslash","\xC3\xB8"},
    {NULL,NULL}
};

static int cp_to_utf8(unsigned cp, char *out)
{
    if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0|(cp>>6)); out[1] = (char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0|(cp>>12)); out[1] = (char)(0x80|((cp>>6)&0x3F));
                        out[2] = (char)(0x80|(cp&0x3F)); return 3; }
    out[0] = (char)(0xF0|(cp>>18)); out[1] = (char)(0x80|((cp>>12)&0x3F));
    out[2] = (char)(0x80|((cp>>6)&0x3F)); out[3] = (char)(0x80|(cp&0x3F));
    return 4;
}

int uw_decode_entities(const char *src, int len, char *dst)
{
    int i = 0, o = 0;
    while (i < len) {
        int j, semi;
        if (src[i] != '&') { dst[o++] = src[i++]; continue; }
        /* find the terminating ';' within a sane distance */
        semi = -1;
        for (j = i + 1; j < len && j < i + 34; j++) {
            if (src[j] == ';') { semi = j; break; }
            if (src[j] == '&' || src[j] == ' ' || src[j] == '<') break;
        }
        if (semi < 0) { dst[o++] = src[i++]; continue; }
        if (src[i+1] == '#') {                       /* numeric reference */
            unsigned cp = 0;
            int k = i + 2, hex = (src[k] == 'x' || src[k] == 'X'), ok = 0;
            if (hex) k++;
            for (; k < semi; k++) {
                int c = src[k], dv;
                if (c >= '0' && c <= '9') dv = c - '0';
                else if (hex && c >= 'a' && c <= 'f') dv = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') dv = c - 'A' + 10;
                else { ok = 0; break; }
                cp = cp * (hex ? 16u : 10u) + (unsigned)dv;
                ok = 1;
                if (cp > 0x10FFFF) { cp = 0xFFFD; }
            }
            if (ok) {
                /* the decoded form must never be longer than "&#...;" was */
                char tmp[4];
                int n = cp_to_utf8(cp, tmp);
                if (n <= semi - i + 1) { memcpy(dst + o, tmp, (size_t)n); o += n; i = semi + 1; continue; }
            }
            dst[o++] = src[i++];
            continue;
        }
        {   int nl = semi - i - 1, k, found = 0;
            for (k = 0; g_ents[k].name; k++) {
                int el = (int)strlen(g_ents[k].name);
                if (el != nl || memcmp(g_ents[k].name, src + i + 1, (size_t)nl)) continue;
                {   int vl = (int)strlen(g_ents[k].utf8);
                    if (vl <= nl + 2) { memcpy(dst + o, g_ents[k].utf8, (size_t)vl); o += vl; }
                    else { memcpy(dst + o, src + i, (size_t)(semi - i + 1)); o += semi - i + 1; } }
                i = semi + 1;
                found = 1;
                break;
            }
            if (found) continue;
        }
        dst[o++] = src[i++];
    }
    return o;
}

/* ---- tokenizer -----------------------------------------------------------
 * Every scan_* returns 1 having consumed a complete token, or 0 meaning "not
 * enough input yet" - unless `eof`, in which case what is there is taken as
 * final. Nothing ever half-consumes. */
static int is_space(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }
static int is_alpha(int c) { c = uw_lc(c); return c >= 'a' && c <= 'z'; }

static int find3(const char *b, int from, int len, const char *pat, int plen)
{
    int i;
    for (i = from; i + plen <= len; i++) if (!memcmp(b + i, pat, (size_t)plen)) return i;
    return -1;
}

/* scan a tag starting at b[p] == '<' */
static int scan_tag(uw_parser *pr, int p, int eof, uw_tok *t)
{
    const char *b = pr->buf;
    int len = pr->blen, i = p + 1, end;
    int is_end = 0;
    if (i < len && b[i] == '/') { is_end = 1; i++; }
    if (i >= len) return eof ? (t->kind = T_NONE, pr->pos = len, 1) : 0;
    if (!is_alpha((u8)b[i])) {
        /* not a tag after all: emit the '<' as text */
        t->kind = T_TEXT; t->text = b + p; t->tlen = 1;
        pr->pos = p + 1;
        return 1;
    }
    /* find the closing '>' honouring quoted attribute values */
    {   int q = 0;
        for (end = i; end < len; end++) {
            char c = b[end];
            if (q) { if (c == q) q = 0; continue; }
            if (c == '"' || c == '\'') { q = c; continue; }
            if (c == '>') break;
        }
        if (end >= len) {
            if (!eof) return 0;
            end = len;                              /* unterminated at EOF */
        }
    }
    t->kind = is_end ? T_END : T_START;
    t->self_closing = 0;
    t->nat = 0;
    t->name = b + i;
    while (i < end && !is_space((u8)b[i]) && b[i] != '/' && b[i] != '>') i++;
    t->nlen = (int)(b + i - t->name);
    /* attributes */
    while (i < end) {
        const char *an, *av = NULL;
        int anl, avl = 0;
        while (i < end && (is_space((u8)b[i]) || b[i] == '/')) i++;
        if (i >= end) break;
        an = b + i;
        while (i < end && !is_space((u8)b[i]) && b[i] != '=' && b[i] != '/' && b[i] != '>') i++;
        anl = (int)(b + i - an);
        if (!anl) { i++; continue; }
        while (i < end && is_space((u8)b[i])) i++;
        if (i < end && b[i] == '=') {
            i++;
            while (i < end && is_space((u8)b[i])) i++;
            if (i < end && (b[i] == '"' || b[i] == '\'')) {
                char q = b[i++];
                av = b + i;
                while (i < end && b[i] != q) i++;
                avl = (int)(b + i - av);
                if (i < end) i++;
            } else {
                av = b + i;
                while (i < end && !is_space((u8)b[i]) && b[i] != '>') i++;
                avl = (int)(b + i - av);
            }
        }
        if (t->nat < UW_MAX_ATTRS) {
            t->at[t->nat].n = an; t->at[t->nat].nlen = anl;
            t->at[t->nat].v = av; t->at[t->nat].vlen = avl;
            t->nat++;
        }
    }
    if (end > p && b[end - 1] == '/') t->self_closing = 1;
    pr->pos = end < len ? end + 1 : len;
    return 1;
}

static int next_token(uw_parser *pr, int eof, uw_tok *t)
{
    const char *b = pr->buf;
    int len = pr->blen, p = pr->pos;
    memset(t, 0, sizeof *t);
    if (p >= len) return 0;

    /* RCDATA / RAWTEXT / script data: everything up to the matching end tag */
    if (pr->raw_tag) {
        const char *name = uw_atom_name(pr->d, pr->raw_tag);
        int nl = (int)strlen(name), i;
        for (i = p; i + 2 + nl <= len; i++) {
            if (b[i] != '<' || b[i+1] != '/') continue;
            if (!uw_ieq(b + i + 2, nl, name, nl)) continue;
            {   int after = i + 2 + nl;
                if (after < len && !is_space((u8)b[after]) && b[after] != '>') continue;
                if (find3(b, after, len, ">", 1) < 0 && !eof) return 0;
            }
            if (i > p) { t->kind = T_TEXT; t->text = b + p; t->tlen = i - p; pr->pos = i; return 1; }
            /* at the end tag itself: leave raw mode and let scan_tag run */
            pr->raw_tag = 0; pr->raw_kind = 0;
            return next_token(pr, eof, t);
        }
        if (!eof) return 0;
        t->kind = T_TEXT; t->text = b + p; t->tlen = len - p; pr->pos = len;
        return 1;
    }

    if (b[p] != '<') {                               /* text run */
        int i = p;
        while (i < len && b[i] != '<') i++;
        t->kind = T_TEXT; t->text = b + p; t->tlen = i - p;
        pr->pos = i;
        return 1;
    }

    /* markup declarations */
    if (p + 3 < len && b[p+1] == '!' && b[p+2] == '-' && b[p+3] == '-') {
        int e = find3(b, p + 4, len, "-->", 3);
        if (e < 0) {
            if (!eof) return 0;
            t->kind = T_COMMENT; t->text = b + p + 4; t->tlen = len - p - 4;
            pr->pos = len;
            return 1;
        }
        t->kind = T_COMMENT; t->text = b + p + 4; t->tlen = e - (p + 4);
        pr->pos = e + 3;
        return 1;
    }
    if (p + 1 < len && b[p+1] == '!') {
        int e = find3(b, p + 2, len, ">", 1);
        if (e < 0) { if (!eof) return 0; e = len; }
        {   int s = p + 2;
            if (e - s >= 7 && uw_ieq(b + s, 7, "doctype", 7)) {
                s += 7;
                while (s < e && is_space((u8)b[s])) s++;
                t->kind = T_DOCTYPE; t->text = b + s; t->tlen = e - s;
            } else {
                t->kind = T_COMMENT; t->text = b + s; t->tlen = e - s;
            } }
        pr->pos = e < len ? e + 1 : len;
        return 1;
    }
    if (p + 1 < len && b[p+1] == '?') {              /* bogus comment */
        int e = find3(b, p + 2, len, ">", 1);
        if (e < 0) { if (!eof) return 0; e = len; }
        t->kind = T_COMMENT; t->text = b + p + 1; t->tlen = e - p - 1;
        pr->pos = e < len ? e + 1 : len;
        return 1;
    }
    return scan_tag(pr, p, eof, t);
}

/* ---- the open-element stack ---------------------------------------------- */
static uw_node *cur(uw_parser *p)
{ return p->nopen ? p->open[p->nopen - 1] : p->d->document; }

static int push_open(uw_parser *p, uw_node *n)
{
    if (p->nopen >= p->d->max_depth) { p->d->truncated = 1; return -1; }
    if (p->nopen == p->opencap) {
        int nc = p->opencap ? p->opencap * 2 : 32;
        uw_node **nv = (uw_node **)(p->d->cfg.alloc
            ? p->d->cfg.alloc(p->d->cfg.alloc_user, (size_t)nc * sizeof *nv)
            : calloc((size_t)nc, sizeof *nv));
        if (!nv) return -1;
        if (p->open) {
            memcpy(nv, p->open, (size_t)p->nopen * sizeof *nv);
            if (p->d->cfg.free) p->d->cfg.free(p->d->cfg.alloc_user, p->open);
            else free(p->open);
        }
        p->open = nv; p->opencap = nc;
    }
    p->open[p->nopen++] = n;
    return 0;
}

static int in_open(uw_parser *p, uw_atom tag)
{ int i; for (i = p->nopen - 1; i >= 0; i--) if (p->open[i]->tag == tag) return i; return -1; }

static void pop_to(uw_parser *p, int idx) { if (idx >= 0 && idx < p->nopen) p->nopen = idx; }

static int tag_in(uw_doc *d, uw_atom a, const char *const *list)
{
    const char *n = uw_atom_name(d, a);
    int i;
    for (i = 0; list[i]; i++) if (!strcmp(n, list[i])) return 1;
    return 0;
}

/* ---- implied structure ---------------------------------------------------- */
static uw_node *ensure_html(uw_parser *p)
{
    uw_doc *d = p->d;
    if (!d->html) {
        d->html = uw_create_element(d, "html");
        if (!d->html) return NULL;
        uw_append(d, d->document, d->html);
        push_open(p, d->html);
    }
    return d->html;
}

static uw_node *ensure_head(uw_parser *p)
{
    uw_doc *d = p->d;
    if (!ensure_html(p)) return NULL;
    if (!d->head) {
        d->head = uw_create_element(d, "head");
        if (!d->head) return NULL;
        uw_append(d, d->html, d->head);
    }
    return d->head;
}

static uw_node *ensure_body(uw_parser *p)
{
    uw_doc *d = p->d;
    if (!ensure_head(p)) return NULL;
    if (!d->body) {
        d->body = uw_create_element(d, "body");
        if (!d->body) return NULL;
        uw_append(d, d->html, d->body);
        /* leaving <head>: everything after this belongs to the body */
        {   int i = in_open(p, d->head->tag);
            if (i >= 0) pop_to(p, i); }
        push_open(p, d->body);
    }
    p->in_head_only = 0;
    return d->body;
}

/* Where a new node goes. Text and elements that stray directly into a table
 * are foster-parented BEFORE the table, which is what keeps a stray label from
 * disappearing inside a table's structure. */
static void insert_node(uw_parser *p, uw_node *n, int is_text)
{
    uw_doc *d = p->d;
    uw_node *parent = cur(p);
    static const char *const tbl[] = { "table","tbody","thead","tfoot","tr", NULL };
    if (is_text && parent->type == UW_NODE_ELEMENT && tag_in(d, parent->tag, tbl)) {
        int i;
        for (i = p->nopen - 1; i >= 0; i--)
            if (!strcmp(uw_atom_name(d, p->open[i]->tag), "table")) {
                uw_node *t = p->open[i];
                if (t->parent) { uw_insert_before(d, t->parent, n, t); return; }
                break;
            }
    }
    uw_append(d, parent, n);
}

static void insert_text(uw_parser *p, const char *s, int len, int decode)
{
    uw_doc *d = p->d;
    uw_node *parent, *last;
    char *buf = NULL;
    if (len <= 0) return;
    if (decode) {
        buf = (char *)uw_arena(d, (size_t)len + 1);
        if (!buf) return;
        len = uw_decode_entities(s, len, buf);
        buf[len] = 0;
        s = buf;
        if (!len) return;
    }
    parent = cur(p);
    last = parent->last;
    /* Merge with a preceding text node. Browsers produce one text node per run
     * of character data, and the golden dumps are much easier to read for it. */
    if (last && last->type == UW_NODE_TEXT) {
        char *joined = (char *)uw_arena(d, (size_t)last->tlen + (size_t)len + 1);
        if (!joined) return;
        memcpy(joined, last->text, last->tlen);
        memcpy(joined + last->tlen, s, (size_t)len);
        joined[last->tlen + len] = 0;
        last->text = joined;
        last->tlen += (u32)len;
        return;
    }
    {   uw_node *t = uw_node_new(d, UW_NODE_TEXT);
        if (!t) return;
        t->text = decode ? buf : uw_arena_str(d, s, len);
        if (!t->text) return;
        t->tlen = (u32)len;
        insert_node(p, t, 1);
    }
}

/* ---- auto-closing --------------------------------------------------------
 * The subset of the spec's "generate implied end tags" that unclosed real-world
 * markup actually needs. */
static void close_p(uw_parser *p)
{
    uw_atom pa = uw_intern(p->d, "p", 1);
    int i = in_open(p, pa);
    if (i >= 0) pop_to(p, i);
}

static void auto_close_for(uw_parser *p, uw_atom tag)
{
    uw_doc *d = p->d;
    const char *n = uw_atom_name(d, tag);
    static const char *const blocks[] = { "address","article","aside","blockquote",
        "div","dl","fieldset","figcaption","figure","footer","form","h1","h2","h3",
        "h4","h5","h6","header","hr","main","nav","ol","p","pre","section","table",
        "ul", NULL };
    if (tag_in(d, tag, blocks)) close_p(p);
    if (!strcmp(n, "li")) {
        int i = in_open(p, tag);
        /* only close an <li> in the same list, not one further out */
        if (i >= 0) {
            int j, ok = 1;
            for (j = i + 1; j < p->nopen; j++)
                if (tag_in(d, p->open[j]->tag, blocks)) { ok = 0; break; }
            if (ok) pop_to(p, i);
        }
    }
    if (!strcmp(n, "dt") || !strcmp(n, "dd")) {
        int i = in_open(p, uw_intern(d, "dt", 2));
        int j = in_open(p, uw_intern(d, "dd", 2));
        int k = i > j ? i : j;
        if (k >= 0) pop_to(p, k);
    }
    if (!strcmp(n, "option")) { int i = in_open(p, tag); if (i >= 0) pop_to(p, i); }
    if (!strcmp(n, "tr")) {
        int i = in_open(p, tag);
        if (i >= 0) pop_to(p, i);
        else {
            int td = in_open(p, uw_intern(d, "td", 2));
            int th = in_open(p, uw_intern(d, "th", 2));
            int k = td > th ? td : th;
            if (k >= 0) pop_to(p, k);
        }
    }
    if (!strcmp(n, "td") || !strcmp(n, "th")) {
        int td = in_open(p, uw_intern(d, "td", 2));
        int th = in_open(p, uw_intern(d, "th", 2));
        int k = td > th ? td : th;
        if (k >= 0) pop_to(p, k);
    }
}

/* ---- the tree builder ---------------------------------------------------- */
static void handle_start(uw_parser *p, uw_tok *t)
{
    uw_doc *d = p->d;
    uw_node *el;
    uw_atom tag = uw_intern(d, t->name, t->nlen);
    const char *n = uw_atom_name(d, tag);
    static const char *const headonly[] = { "base","link","meta","title","style",
        "noscript", NULL };
    int i;

    if (!strcmp(n, "html")) { ensure_html(p); el = d->html; goto attrs; }
    if (!strcmp(n, "head")) { ensure_head(p);
                              if (in_open(p, d->head->tag) < 0) push_open(p, d->head);
                              el = d->head; goto attrs; }
    if (!strcmp(n, "body")) { ensure_body(p); el = d->body; goto attrs; }

    if (p->in_head_only && tag_in(d, tag, headonly)) {
        uw_node *h = ensure_head(p);
        if (!h) return;
        if (in_open(p, h->tag) < 0) push_open(p, h);
    } else if (!strcmp(n, "script") && p->in_head_only) {
        uw_node *h = ensure_head(p);
        if (!h) return;
        if (in_open(p, h->tag) < 0) push_open(p, h);
    } else {
        if (!ensure_body(p)) return;
    }

    auto_close_for(p, tag);

    el = uw_node_new(d, UW_NODE_ELEMENT);
    if (!el) return;
    el->tag = tag;
    if (uw_is_void(d, tag)) el->flags |= UW_F_VOID;
    insert_node(p, el, 0);

attrs:
    for (i = 0; i < t->nat; i++) {
        char vbuf[1024];
        const char *v = t->at[i].v;
        int vl = t->at[i].vlen;
        uw_atom an = uw_intern(d, t->at[i].n, t->at[i].nlen);
        uw_attr_ent *e;
        if (!an) continue;
        /* first declaration of an attribute wins, per the spec */
        { uw_attr_ent *x; int dup = 0;
          for (x = el->attrs; x; x = x->next) if (x->name == an) { dup = 1; break; }
          if (dup) continue; }
        if (v && vl > 0 && vl < (int)sizeof vbuf) {
            vl = uw_decode_entities(v, vl, vbuf);
            v = vbuf;
        } else if (!v) { v = ""; vl = 0; }
        e = (uw_attr_ent *)uw_arena(d, sizeof *e);
        if (!e) break;
        e->name = an;
        e->value = uw_arena_str(d, v, vl);
        if (!e->value) break;
        e->vlen = (u32)vl;
        if (!el->attrs) el->attrs = e;
        else { uw_attr_ent *x = el->attrs; while (x->next) x = x->next; x->next = e; }
        if (!strcmp(uw_atom_name(d, an), "id")) uw_index_id(d, el, e->value, vl);
    }

    if (el == d->html || el == d->head || el == d->body) return;
    if ((el->flags & UW_F_VOID) || t->self_closing) {
        if (p->have_hooks && p->hooks.resource) {
            const char *src = uw_attr(d, el, "src");
            const char *href = uw_attr(d, el, "href");
            if (!strcmp(n, "img") && src) p->hooks.resource(p->hooks.user, el, src, UW_RES_IMAGE);
            else if (!strcmp(n, "link") && href) {
                const char *rel = uw_attr(d, el, "rel");
                if (rel && uw_ieq(rel, (int)strlen(rel), "stylesheet", 10))
                    p->hooks.resource(p->hooks.user, el, href, UW_RES_STYLESHEET);
            }
        }
        return;
    }
    push_open(p, el);
    {   int rk = uw_raw_kind(d, tag);
        if (rk) { p->raw_tag = tag; p->raw_kind = rk; } }
}

static void handle_end(uw_parser *p, uw_tok *t)
{
    uw_doc *d = p->d;
    uw_atom tag = uw_intern(d, t->name, t->nlen);
    int i;
    if (!tag) return;
    if (uw_is_void(d, tag)) return;                  /* </br> and friends */
    i = in_open(p, tag);
    if (i < 0) return;                               /* stray end tag: ignore */
    /* Never pop past <body>/<html> on a stray close: content after it must
     * still land somewhere sane. */
    if (d->body && p->open[i] == d->body && i == 0) return;
    pop_to(p, i);
}

static void handle_token(uw_parser *p, uw_tok *t)
{
    uw_doc *d = p->d;
    switch (t->kind) {
    case T_TEXT: {
        /* Text inside a raw/RCDATA element belongs to THAT element, wherever it
         * sits. Letting it fall through to the head/body logic below made
         * <title>T</title> force <body> open and drop T into it. RCDATA (kind
         * 1: title, textarea) decodes entities; RAWTEXT and script data do not. */
        if (p->raw_kind) { insert_text(p, t->text, t->tlen, p->raw_kind == 1); break; }
        if (p->in_head_only) {
            int i, ws = 1;
            for (i = 0; i < t->tlen; i++) if (!is_space((u8)t->text[i])) { ws = 0; break; }
            if (ws) break;                           /* leading whitespace: drop */
            if (!ensure_body(p)) break;
        }
        insert_text(p, t->text, t->tlen, 1);
        break; }
    case T_START:   handle_start(p, t); break;
    case T_END: {
        uw_atom tag = uw_intern(d, t->name, t->nlen);
        /* leaving a <script> hands its text to the embedder */
        if (p->have_hooks && p->hooks.script &&
            !strcmp(uw_atom_name(d, tag), "script")) {
            int i = in_open(p, tag);
            if (i >= 0) {
                uw_node *el = p->open[i];
                const char *src = "";
                int slen = 0;
                if (el->first && el->first->type == UW_NODE_TEXT) {
                    src = el->first->text; slen = (int)el->first->tlen;
                }
                p->in_script_hook = 1;
                p->hooks.script(p->hooks.user, p, el, src, slen);
                p->in_script_hook = 0;
            }
        }
        handle_end(p, t);
        break; }
    case T_COMMENT: {
        uw_node *c = uw_node_new(d, UW_NODE_COMMENT);
        if (!c) break;
        c->text = uw_arena_str(d, t->text, t->tlen);
        c->tlen = (u32)t->tlen;
        insert_node(p, c, 0);
        break; }
    case T_DOCTYPE: {
        uw_node *c;
        if (d->html) break;                          /* too late to matter */
        c = uw_node_new(d, UW_NODE_DOCTYPE);
        if (!c) break;
        c->text = uw_arena_str(d, t->text, t->tlen);
        c->tlen = (u32)t->tlen;
        uw_append(d, d->document, c);
        break; }
    default: break;
    }
}

/* ---- the push interface --------------------------------------------------- */
uw_parser *uw_parse_begin(uw_doc *d, const uw_hooks *hooks)
{
    uw_parser *p;
    if (!d) return NULL;
    p = (uw_parser *)(d->cfg.alloc ? d->cfg.alloc(d->cfg.alloc_user, sizeof *p)
                                   : calloc(1, sizeof *p));
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    p->d = d;
    p->in_head_only = 1;
    if (hooks) { p->hooks = *hooks; p->have_hooks = 1; }
    /* The parse gets HALF the arena, and uw_parse_end gives the rest back.
     *
     * Without this the tree could spend everything, and because the arena never
     * frees, that is terminal: style, layout and paint then allocate nothing at
     * all and the page renders BLANK - worse than the same page rendered short
     * (UNOAUTOMATE-REQUESTS 2026-08-06, "an exhausted arena draws NOTHING, not
     * less"). Truncating the parse instead means fewer elements, all of which
     * are styled, laid out and painted.
     *
     * Half is deliberately generous to the later phases, and it costs nothing
     * on a document that fits: the parse is by far the CHEAPEST phase - 4,000
     * elements parse inside 1 MB and need 16 MB to paint, i.e. the tree is
     * about 6% of the pipeline's peak - so a document whose parse alone wants
     * more than half the arena was never going to render under the old rule
     * either. It would have parsed, then drawn nothing. */
    if (!d->soft_max) d->soft_max = d->max / 2;
    return p;
}

static int buf_reserve(uw_parser *p, int extra)
{
    uw_doc *d = p->d;
    if (p->blen + extra + 1 <= p->bcap) return 0;
    {   int nc = p->bcap ? p->bcap : 4096;
        char *nb;
        while (nc < p->blen + extra + 1) nc *= 2;
        if ((unsigned)nc > UW_PENDING_MAX) { d->truncated = 1; return -1; }
        nb = (char *)(d->cfg.alloc ? d->cfg.alloc(d->cfg.alloc_user, (size_t)nc)
                                   : calloc((size_t)nc, 1));
        if (!nb) return -1;
        if (p->buf) {
            memcpy(nb, p->buf, (size_t)p->blen);
            if (d->cfg.free) d->cfg.free(d->cfg.alloc_user, p->buf); else free(p->buf);
        }
        p->buf = nb; p->bcap = nc;
    }
    return 0;
}

/* Drop what has already been consumed, so a long document does not keep the
 * whole byte stream alive in the pending buffer. */
static void buf_compact(uw_parser *p)
{
    if (p->pos <= 0) return;
    if (p->pos < p->blen) memmove(p->buf, p->buf + p->pos, (size_t)(p->blen - p->pos));
    p->blen -= p->pos;
    p->pos = 0;
    if (p->buf) p->buf[p->blen] = 0;
}

static int run(uw_parser *p, int eof)
{
    uw_tok t;
    while (!p->stopped && next_token(p, eof, &t)) {
        if (t.kind == T_NONE) break;
        handle_token(p, &t);
        if (p->d->truncated) { p->stopped = 1; return -1; }
    }
    buf_compact(p);
    return 0;
}

int uw_parse_feed(uw_parser *p, const char *bytes, int n)
{
    if (!p || p->stopped) return -1;
    if (n < 0) n = bytes ? (int)strlen(bytes) : 0;
    if (n) {
        if (buf_reserve(p, n) < 0) { p->stopped = 1; return -1; }
        memcpy(p->buf + p->blen, bytes, (size_t)n);
        p->blen += n;
        p->buf[p->blen] = 0;
    }
    return run(p, 0);
}

int uw_parse_insert(uw_parser *p, const char *bytes, int n)
{
    /* document.write: splice at the READ CURSOR, which is the spec's insertion
     * point - the written markup is parsed before whatever follows it. */
    if (!p || p->stopped) return -1;
    if (n < 0) n = bytes ? (int)strlen(bytes) : 0;
    if (!n) return 0;
    if (buf_reserve(p, n) < 0) { p->stopped = 1; return -1; }
    memmove(p->buf + p->pos + n, p->buf + p->pos, (size_t)(p->blen - p->pos));
    memcpy(p->buf + p->pos, bytes, (size_t)n);
    p->blen += n;
    p->buf[p->blen] = 0;
    return 0;
}

int uw_parse_end(uw_parser *p)
{
    uw_doc *d;
    int rc;
    if (!p) return -1;
    d = p->d;
    rc = run(p, 1);
    /* Hand the parse's reserved half back: everything from here - the cascade,
     * the box tree, the display list - draws on the full arena. */
    d->soft_max = 0;
    /* A document always has html/head/body, even if the source had none. This
     * used to be skipped on a truncated parse, which turned "some of the page"
     * into "none of it": uw_layout starts at uw_body() and returns -1 without
     * one, so a document that ran out of room mid-parse had no body, no boxes
     * and nothing to paint. It is a handful of nodes and the half-arena above
     * is there to pay for them. */
    ensure_body(p);
    if (p->open) { if (d->cfg.free) d->cfg.free(d->cfg.alloc_user, p->open); else free(p->open); }
    if (p->buf)  { if (d->cfg.free) d->cfg.free(d->cfg.alloc_user, p->buf);  else free(p->buf); }
    if (d->cfg.free) d->cfg.free(d->cfg.alloc_user, p); else free(p);
    return rc;
}

/* ---- convenience wrappers ------------------------------------------------- */
uw_doc *uw_parse_string(const char *html, int n, const uw_config *cfg)
{
    uw_doc *d = uw_doc_new(cfg);
    uw_parser *p;
    if (!d) return NULL;
    p = uw_parse_begin(d, NULL);
    if (!p) { uw_doc_free(d); return NULL; }
    uw_parse_feed(p, html, n);
    uw_parse_end(p);
    return d;
}

int uw_parse_fragment(uw_doc *d, uw_node *ctx, const char *html, int n)
{
    uw_parser *p;
    if (!d || !ctx) return -1;
    while (ctx->first) uw_remove(d, ctx->first);
    p = uw_parse_begin(d, NULL);
    if (!p) return -1;
    /* Parse directly into the context element: no implied html/head/body, and
     * the fragment's own root is whatever `ctx` already is. */
    p->in_head_only = 0;
    push_open(p, ctx);
    if (n < 0) n = html ? (int)strlen(html) : 0;
    if (n) {
        if (buf_reserve(p, n) < 0) { p->stopped = 1; }
        else {
            memcpy(p->buf + p->blen, html, (size_t)n);
            p->blen += n;
            p->buf[p->blen] = 0;
        }
    }
    run(p, 1);
    if (p->open) { if (d->cfg.free) d->cfg.free(d->cfg.alloc_user, p->open); else free(p->open); }
    if (p->buf)  { if (d->cfg.free) d->cfg.free(d->cfg.alloc_user, p->buf);  else free(p->buf); }
    if (d->cfg.free) d->cfg.free(d->cfg.alloc_user, p); else free(p);
    uw_mark_dirty(ctx, UW_DIRTY_STYLE | UW_DIRTY_SUBTREE | UW_DIRTY_LAYOUT);
    return 0;
}
