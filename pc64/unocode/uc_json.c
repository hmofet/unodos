/* ===========================================================================
 * uc_json.c - JSON with comments, the format every UnoCode config file uses.
 *
 * VS Code's settings.json, keybindings.json, launch.json, tasks.json, colour
 * themes and package.json are all JSONC: line and block comments are legal and
 * a trailing comma before a closing brace is legal.  A strict JSON parser
 * rejects the files people actually write - every default settings.json VS
 * Code has ever shipped opens with a comment - so this parser accepts them.
 *
 * ONE ARENA PER DOCUMENT.  Nodes and strings are bump-allocated out of a chain
 * of blocks hanging off the root, so a whole tree is freed by freeing the
 * root, no node owns anything, and a parse that runs out of memory half way
 * leaks nothing.  The cost is that a child pointer must never outlive its
 * root - which is exactly the discipline the callers already want.
 * ======================================================================== */
#include "unocode.h"

#define UCJ_BLOCK 8192
#define UCJ_MAXDEPTH 40

typedef struct UcjBlock {
    struct UcjBlock *next;
    unsigned long    used, cap;
    /* payload follows */
} UcjBlock;

/* The root node is placed at the head of the FIRST block, so uc_json_free()
 * can find the chain from the root alone. */
typedef struct {
    UcjBlock *first;
    UcJson    root;
} UcjDoc;

/* Where `root` sits inside UcjDoc.  It is the LAST member and nothing follows
 * it, so its offset is the struct size minus its own size - which is exact
 * here (both members are pointer-aligned) and needs no offsetof, and no cast
 * from a pointer to an integer, which is what the first version did and what
 * the compiler was right to complain about. */
#define UCJ_ROOT_OFF ((unsigned long)(sizeof(UcjDoc) - sizeof(UcJson)))

typedef struct {
    const char *s;
    int         len, pos, depth;
    UcjBlock   *head, *cur;
    char       *err;
    int         errcap;
    int         failed;
} UcjP;

/* ---- arena ---------------------------------------------------------------- */
static UcjBlock *ucj_block(unsigned long need)
{
    unsigned long cap = UCJ_BLOCK;
    UcjBlock *b;
    while (cap < need + sizeof(UcjBlock)) cap *= 2;
    b = (UcjBlock *)malloc(cap);
    if (!b) return 0;
    b->next = 0;
    b->cap  = cap;
    b->used = sizeof(UcjBlock);
    return b;
}

static void *ucj_alloc(UcjP *p, unsigned long n)
{
    UcjBlock *b = p->cur;
    char *ret;
    n = (n + 7u) & ~7u;
    if (!b || b->used + n > b->cap) {
        UcjBlock *nb = ucj_block(n);
        if (!nb) { p->failed = 1; return 0; }
        if (b) b->next = nb; else p->head = nb;
        p->cur = b = nb;
    }
    ret = (char *)b + b->used;
    b->used += n;
    memset(ret, 0, n);
    return ret;
}

static UcJson *ucj_node(UcjP *p, int type)
{
    UcJson *v = (UcJson *)ucj_alloc(p, sizeof(UcJson));
    if (v) v->type = (unsigned char)type;
    return v;
}

/* ---- lexing --------------------------------------------------------------- */
static void ucj_fail(UcjP *p, const char *msg)
{
    if (p->failed) return;
    p->failed = 1;
    if (p->err && p->errcap > 0) {
        int line = 1, i;
        char num[16];
        for (i = 0; i < p->pos && i < p->len; i++)
            if (p->s[i] == '\n') line++;
        uc_scpy(p->err, "line ", p->errcap);
        uc_itoa(num, line);
        uc_scat(p->err, num, p->errcap);
        uc_scat(p->err, ": ", p->errcap);
        uc_scat(p->err, msg, p->errcap);
    }
}

/* whitespace AND comments - the JSONC part */
static void ucj_ws(UcjP *p)
{
    for (;;) {
        while (p->pos < p->len) {
            char c = p->s[p->pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p->pos++;
            else break;
        }
        if (p->pos + 1 < p->len && p->s[p->pos] == '/' && p->s[p->pos+1] == '/') {
            p->pos += 2;
            while (p->pos < p->len && p->s[p->pos] != '\n') p->pos++;
            continue;
        }
        if (p->pos + 1 < p->len && p->s[p->pos] == '/' && p->s[p->pos+1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->len &&
                   !(p->s[p->pos] == '*' && p->s[p->pos+1] == '/')) p->pos++;
            p->pos = (p->pos + 1 < p->len) ? p->pos + 2 : p->len;
            continue;
        }
        return;
    }
}

static int ucj_hex4(const char *s)
{
    int i, v = 0;
    for (i = 0; i < 4; i++) {
        int c = s[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v * 16 + d;
    }
    return v;
}

/* Decode a JSON string literal into arena memory.  UTF-16 escapes become
 * UTF-8; a lone surrogate becomes U+FFFD rather than a truncated sequence. */
static char *ucj_string(UcjP *p)
{
    int start, n = 0, i;
    char *out;
    if (p->pos >= p->len || p->s[p->pos] != '"') { ucj_fail(p, "expected a string"); return 0; }
    p->pos++;
    start = p->pos;
    /* pass 1: measure */
    for (i = start; i < p->len && p->s[i] != '"'; i++) {
        if (p->s[i] == '\\') {
            if (i + 1 >= p->len) break;
            if (p->s[i+1] == 'u') { n += 3; i += 5; }   /* worst case 3 bytes */
            else { n += 1; i += 1; }
        } else n++;
    }
    if (i >= p->len) { ucj_fail(p, "unterminated string"); return 0; }
    out = (char *)ucj_alloc(p, (unsigned long)n + 1);
    if (!out) return 0;
    /* pass 2: decode */
    n = 0;
    for (i = start; i < p->len && p->s[i] != '"'; i++) {
        char c = p->s[i];
        if (c != '\\') { out[n++] = c; continue; }
        i++;
        if (i >= p->len) break;
        switch (p->s[i]) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': out[n++] = '\r'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case '/': out[n++] = '/';  break;
        case '"': out[n++] = '"';  break;
        case '\\': out[n++] = '\\'; break;
        case 'u': {
            int cp;
            if (i + 4 >= p->len) { i = p->len; break; }
            cp = ucj_hex4(p->s + i + 1);
            i += 4;
            if (cp < 0) cp = 0xFFFD;
            /* a high surrogate followed by \uDC00..\uDFFF is one code point */
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < p->len &&
                p->s[i+1] == '\\' && p->s[i+2] == 'u') {
                int lo = ucj_hex4(p->s + i + 3);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            if (cp < 0x80) out[n++] = (char)cp;
            else if (cp < 0x800) {
                out[n++] = (char)(0xC0 | (cp >> 6));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out[n++] = (char)(0xE0 | (cp >> 12));
                out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            } else {
                /* Above the BMP we would need four bytes, and the measuring
                 * pass budgeted three.  The fonts are 8-bit anyway, so this
                 * is the honest place to stop: emit the replacement glyph
                 * rather than overrun the buffer we sized. */
                out[n++] = (char)0xEF; out[n++] = (char)0xBF; out[n++] = (char)0xBD;
            }
            break;
        }
        default: out[n++] = p->s[i]; break;
        }
    }
    out[n] = 0;
    p->pos = i + 1;      /* past the closing quote */
    return out;
}

static double ucj_number(UcjP *p)
{
    double v = 0, frac = 0, scale = 1;
    int neg = 0, any = 0;
    if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+')) {
        neg = p->s[p->pos] == '-';
        p->pos++;
    }
    while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
        v = v * 10 + (p->s[p->pos] - '0');
        p->pos++; any = 1;
    }
    if (p->pos < p->len && p->s[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            frac = frac * 10 + (p->s[p->pos] - '0');
            scale *= 10;
            p->pos++; any = 1;
        }
        v += frac / scale;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        int eneg = 0, e = 0;
        p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '-' || p->s[p->pos] == '+')) {
            eneg = p->s[p->pos] == '-';
            p->pos++;
        }
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            e = e * 10 + (p->s[p->pos] - '0');
            p->pos++;
        }
        while (e-- > 0) { if (eneg) v /= 10; else v *= 10; }
    }
    if (!any) ucj_fail(p, "expected a number");
    return neg ? -v : v;
}

static UcJson *ucj_value(UcjP *p);

static UcJson *ucj_object(UcjP *p)
{
    UcJson *o = ucj_node(p, UJ_OBJ), *tail = 0;
    if (!o) return 0;
    p->pos++;                                  /* '{' */
    for (;;) {
        UcJson *m;
        char *key;
        ucj_ws(p);
        if (p->pos >= p->len) { ucj_fail(p, "unterminated object"); return 0; }
        if (p->s[p->pos] == '}') { p->pos++; return o; }
        key = ucj_string(p);
        if (p->failed) return 0;
        ucj_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') { ucj_fail(p, "expected ':'"); return 0; }
        p->pos++;
        m = ucj_value(p);
        if (p->failed || !m) return 0;
        m->key = key;
        if (tail) tail->next = m; else o->child = m;
        tail = m;
        o->n++;
        ucj_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        ucj_ws(p);
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return o; }
        ucj_fail(p, "expected ',' or '}'");
        return 0;
    }
}

static UcJson *ucj_array(UcjP *p)
{
    UcJson *a = ucj_node(p, UJ_ARR), *tail = 0;
    if (!a) return 0;
    p->pos++;                                  /* '[' */
    for (;;) {
        UcJson *e;
        ucj_ws(p);
        if (p->pos >= p->len) { ucj_fail(p, "unterminated array"); return 0; }
        if (p->s[p->pos] == ']') { p->pos++; return a; }
        e = ucj_value(p);
        if (p->failed || !e) return 0;
        if (tail) tail->next = e; else a->child = e;
        tail = e;
        a->n++;
        ucj_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        ucj_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return a; }
        ucj_fail(p, "expected ',' or ']'");
        return 0;
    }
}

static UcJson *ucj_value(UcjP *p)
{
    UcJson *v;
    ucj_ws(p);
    if (p->failed) return 0;
    if (p->pos >= p->len) { ucj_fail(p, "unexpected end of input"); return 0; }
    if (++p->depth > UCJ_MAXDEPTH) { ucj_fail(p, "nested too deeply"); p->depth--; return 0; }
    switch (p->s[p->pos]) {
    case '{': v = ucj_object(p); break;
    case '[': v = ucj_array(p);  break;
    case '"':
        v = ucj_node(p, UJ_STR);
        if (v) { v->str = ucj_string(p); if (!v->str) v = 0; }
        break;
    case 't':
        if (p->pos + 4 <= p->len && !strncmp(p->s + p->pos, "true", 4)) {
            p->pos += 4; v = ucj_node(p, UJ_BOOL); if (v) v->bval = 1;
        } else { ucj_fail(p, "expected a value"); v = 0; }
        break;
    case 'f':
        if (p->pos + 5 <= p->len && !strncmp(p->s + p->pos, "false", 5)) {
            p->pos += 5; v = ucj_node(p, UJ_BOOL);
        } else { ucj_fail(p, "expected a value"); v = 0; }
        break;
    case 'n':
        if (p->pos + 4 <= p->len && !strncmp(p->s + p->pos, "null", 4)) {
            p->pos += 4; v = ucj_node(p, UJ_NULL);
        } else { ucj_fail(p, "expected a value"); v = 0; }
        break;
    default:
        v = ucj_node(p, UJ_NUM);
        if (v) v->num = ucj_number(p);
        break;
    }
    p->depth--;
    return v;
}

/* ---- public --------------------------------------------------------------- */
UcJson *uc_json_parse(const char *src, int len, char *err, int errcap)
{
    UcjP p;
    UcjDoc *doc;
    UcJson *v;
    if (err && errcap > 0) err[0] = 0;
    if (!src) return 0;
    memset(&p, 0, sizeof p);
    p.s = src;
    p.len = len < 0 ? (int)strlen(src) : len;
    p.err = err; p.errcap = errcap;

    /* The document header lives in the first block so the free chain hangs
     * off the returned root and the caller needs no second handle. */
    doc = (UcjDoc *)ucj_alloc(&p, sizeof(UcjDoc));
    if (!doc) { if (err && errcap) uc_scpy(err, "out of memory", errcap); return 0; }

    v = ucj_value(&p);
    if (p.failed || !v) {
        UcjBlock *b = p.head, *nb;
        while (b) { nb = b->next; free(b); b = nb; }
        if (err && errcap > 0 && !err[0]) uc_scpy(err, "parse failed", errcap);
        return 0;
    }
    /* trailing junk is tolerated: a settings file with a stray brace at the
     * end still yields the object above it, which is more use than nothing */
    doc->first = p.head;
    doc->root  = *v;
    return &doc->root;
}

void uc_json_free(UcJson *root)
{
    UcjDoc *doc;
    UcjBlock *b, *nb;
    if (!root) return;
    /* The root node is the `root` member of the UcjDoc at the head of the
     * first block, so the block chain is recoverable from the root pointer
     * alone - which is what lets uc_json_parse() return one pointer and
     * uc_json_free() take one pointer. */
    doc = (UcjDoc *)((char *)root - UCJ_ROOT_OFF);
    b = doc->first;
    while (b) { nb = b->next; free(b); b = nb; }
}

UcJson *uc_json_member(const UcJson *o, const char *key)
{
    UcJson *c;
    if (!o || o->type != UJ_OBJ) return 0;
    for (c = o->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return 0;
}

UcJson *uc_json_at(const UcJson *a, int i)
{
    UcJson *c;
    if (!a || i < 0) return 0;
    for (c = a->child; c; c = c->next)
        if (i-- == 0) return c;
    return 0;
}

const char *uc_json_str(const UcJson *o, const char *key, const char *dflt)
{
    UcJson *m = uc_json_member(o, key);
    return (m && m->type == UJ_STR && m->str) ? m->str : dflt;
}

double uc_json_num(const UcJson *o, const char *key, double dflt)
{
    UcJson *m = uc_json_member(o, key);
    return (m && m->type == UJ_NUM) ? m->num : dflt;
}

int uc_json_bool(const UcJson *o, const char *key, int dflt)
{
    UcJson *m = uc_json_member(o, key);
    if (!m) return dflt;
    if (m->type == UJ_BOOL) return m->bval;
    if (m->type == UJ_NUM)  return m->num != 0;
    return dflt;
}

/* "editor.minimap.enabled" resolves against BOTH spellings, because VS Code
 * settings files use the flat dotted key and contributed configuration uses
 * the nested object, and a reader that only understood one would silently
 * ignore half the files it is handed. */
UcJson *uc_json_path(const UcJson *root, const char *dotted)
{
    const UcJson *node = root;
    char part[64];
    const char *p = dotted;
    UcJson *flat;
    if (!root || !dotted) return 0;
    flat = uc_json_member(root, dotted);
    if (flat) return flat;
    for (;;) {
        int n = 0;
        UcJson *m;
        while (*p && *p != '.' && n < (int)sizeof part - 1) part[n++] = *p++;
        part[n] = 0;
        m = uc_json_member(node, part);
        if (!m) return 0;
        if (*p != '.') return m;
        p++;
        node = m;
    }
}

int uc_json_esc(char *out, int cap, const char *s)
{
    int n = 0;
    if (cap <= 0) return 0;
    while (*s && n < cap - 7) {
        unsigned char c = (unsigned char)*s++;
        switch (c) {
        case '"':  out[n++] = '\\'; out[n++] = '"';  break;
        case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
        case '\n': out[n++] = '\\'; out[n++] = 'n';  break;
        case '\t': out[n++] = '\\'; out[n++] = 't';  break;
        case '\r': out[n++] = '\\'; out[n++] = 'r';  break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
                out[n++] = hex[(c >> 4) & 15]; out[n++] = hex[c & 15];
            } else out[n++] = (char)c;
        }
    }
    out[n] = 0;
    return n;
}
