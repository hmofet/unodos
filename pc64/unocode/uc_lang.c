/*
 * VENDORED FILE - DO NOT EDIT HERE.
 *
 * UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
 * An edit made here is lost at the next sync, and until then it silently
 * forks the editor away from the tree the desktop builds are cut from.
 *
 * Change it there; bring it back with pc64/tools/sync_unocode.py.
 * See pc64/UNOCODE-UPSTREAM.md.
 */
/* ===========================================================================
 * uc_lang.c - languages, grammars and the highlighter.
 *
 * THE GRAMMAR MODEL is TextMate's, cut down to what a 4 MB module can carry
 * and an author can hand-write.  A grammar is a list of patterns; a pattern is
 * either a one-line `match` regex, or a `begin`/`end` pair with its own nested
 * `patterns`, or an `include` of a named rule set from `repository` (or of
 * `$self`).  `captures` / `beginCaptures` / `endCaptures` assign scopes to
 * capture groups.  Scopes are dotted names ("keyword.control.c") and the theme
 * resolves them by longest dotted prefix, so a grammar written for VS Code's
 * default themes is coloured correctly here without being rewritten.
 *
 * CROSS-LINE STATE IS A STACK (UCD-28), and still one 16-bit number per line.
 * It used to be a single open rule, which meant a rule closing on a later line
 * dropped the editor to the top level rather than back into whatever contained
 * it.  The stack is INTERNED - each distinct nesting becomes an id in a pool -
 * so the per-line cost is unchanged and the per-document cost is the number of
 * nestings that actually occur, which is small even in a large file.  See the
 * block above hl_state_push().
 *
 * OUTPUT IS PER CHARACTER, not per token run.  The painter wants a colour for
 * every column anyway, and writing scopes into a per-character array makes an
 * overlapping capture scope simply overwrite its parent's - no merge pass, no
 * ordering rules, no chance of two runs disagreeing about a column.
 * ======================================================================== */
#include "unocode.h"

/* ---- scope interning ------------------------------------------------------
 * Scope strings are compared by the theme on every character, so they are
 * interned once and passed around as ids.  A grammar that names a scope the
 * theme has never heard of simply resolves to the default foreground. */
#define UC_SCOPES_MAX 256
static char  g_scope[UC_SCOPES_MAX][52];
static int   g_nscope;

int uc_scope_id(const char *name)
{
    int i;
    if (!name || !name[0]) return 0;
    for (i = 1; i < g_nscope; i++) if (!strcmp(g_scope[i], name)) return i;
    if (g_nscope == 0) { g_scope[0][0] = 0; g_nscope = 1; }
    if (g_nscope >= UC_SCOPES_MAX) return 0;
    uc_scpy(g_scope[g_nscope], name, sizeof g_scope[0]);
    return g_nscope++;
}

const char *uc_scope_name(int id)
{
    if (id <= 0 || id >= g_nscope) return "";
    return g_scope[id];
}

/* ---- built-in grammars ----------------------------------------------------
 * One row per pattern.  `sub_*` is a single nested rule, which is all a
 * begin/end pattern in these languages needs (an escape inside a string).  A
 * grammar that wants more structure is a JSON file - which is the point. */
typedef struct {
    const char *name;
    const char *match;
    const char *begin, *end;
    const char *content;
    const char *cap1, *cap2, *cap3;
    const char *sub_name, *sub_match;
} UcGramRow;

#define KW_C "auto break case char const continue default do double else enum " \
             "extern float for goto if inline int long register restrict return " \
             "short signed sizeof static struct switch typedef union unsigned " \
             "void volatile while NULL true false include define ifdef ifndef " \
             "endif pragma"

static const UcGramRow kGramC[] = {
{ "comment.block.c", 0, "/\\*", "\\*/", 0, 0,0,0, 0,0 },
{ "comment.line.double-slash.c", "//.*", 0,0,0, 0,0,0, 0,0 },
{ "meta.preprocessor.c", 0, "^\\s*#\\s*[a-z_]+", "$", 0, 0,0,0,
  "string.quoted.other.include.c", "<[^>]*>" },
{ "string.quoted.double.c", 0, "\"", "\"", 0, 0,0,0,
  "constant.character.escape.c", "\\\\." },
{ "string.quoted.single.c", 0, "'", "'", 0, 0,0,0,
  "constant.character.escape.c", "\\\\." },
{ "constant.numeric.c", "\\b(0[xX][0-9a-fA-F]+[uUlL]*|[0-9]+\\.?[0-9]*([eE][-+]?[0-9]+)?[uUlLfF]*)\\b", 0,0,0, 0,0,0, 0,0 },
{ "keyword.control.c", "\\b(if|else|for|while|do|switch|case|default|break|continue|return|goto)\\b", 0,0,0, 0,0,0, 0,0 },
{ "storage.type.c", "\\b(void|char|short|int|long|float|double|signed|unsigned|struct|union|enum|typedef|const|volatile|static|extern|register|inline|auto|sizeof)\\b", 0,0,0, 0,0,0, 0,0 },
{ "constant.language.c", "\\b(NULL|true|false)\\b", 0,0,0, 0,0,0, 0,0 },
{ "", "\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\(", 0,0,0, "entity.name.function.c", 0,0, 0,0 },
{ "entity.name.type.c", "\\b[A-Z][A-Za-z0-9_]*[a-z][A-Za-z0-9_]*\\b", 0,0,0, 0,0,0, 0,0 },
{ "keyword.operator.c", "[-+*/%=!<>&|^~?]+", 0,0,0, 0,0,0, 0,0 },
{ 0,0,0,0,0,0,0,0,0,0 }
};

#define KW_PY "and as assert async await break class continue def del elif else " \
              "except False finally for from global if import in is lambda None " \
              "nonlocal not or pass raise return True try while with yield self " \
              "print len range str int float list dict set tuple open enumerate " \
              "zip map filter sorted sum min max abs"

static const UcGramRow kGramPy[] = {
{ "comment.line.number-sign.python", "#.*", 0,0,0, 0,0,0, 0,0 },
{ "string.quoted.triple.python", 0, "\"\"\"", "\"\"\"", 0, 0,0,0, 0,0 },
{ "string.quoted.triple.python", 0, "'''", "'''", 0, 0,0,0, 0,0 },
{ "string.quoted.double.python", 0, "\"", "\"", 0, 0,0,0,
  "constant.character.escape.python", "\\\\." },
{ "string.quoted.single.python", 0, "'", "'", 0, 0,0,0,
  "constant.character.escape.python", "\\\\." },
{ "entity.name.function.decorator.python", "@[A-Za-z_][A-Za-z0-9_.]*", 0,0,0, 0,0,0, 0,0 },
{ "", "\\b(def|class)\\s+([A-Za-z_][A-Za-z0-9_]*)", 0,0,0,
  "storage.type.function.python", "entity.name.function.python", 0, 0,0 },
{ "constant.numeric.python", "\\b(0[xXbBoO][0-9a-fA-F_]+|[0-9][0-9_]*\\.?[0-9_]*([eE][-+]?[0-9]+)?[jJ]?)\\b", 0,0,0, 0,0,0, 0,0 },
{ "keyword.control.python", "\\b(if|elif|else|for|while|break|continue|return|yield|pass|raise|try|except|finally|with|as|assert|import|from|in|is|not|and|or|lambda|global|nonlocal|del|async|await)\\b", 0,0,0, 0,0,0, 0,0 },
{ "storage.type.python", "\\b(def|class)\\b", 0,0,0, 0,0,0, 0,0 },
{ "constant.language.python", "\\b(True|False|None)\\b", 0,0,0, 0,0,0, 0,0 },
{ "variable.language.python", "\\bself\\b", 0,0,0, 0,0,0, 0,0 },
{ "support.function.python", "\\b(print|len|range|str|int|float|list|dict|set|tuple|open|abs|min|max|sum|enumerate|zip|map|filter|sorted|isinstance|getattr|setattr|hasattr)\\b", 0,0,0, 0,0,0, 0,0 },
{ "keyword.operator.python", "[-+*/%=!<>&|^~]+", 0,0,0, 0,0,0, 0,0 },
{ 0,0,0,0,0,0,0,0,0,0 }
};

#define KW_JS "async await break case catch class const continue debugger default " \
              "delete do else export extends false finally for function if import " \
              "in instanceof let new null of return super switch this throw true " \
              "try typeof var void while with yield console require module exports"

static const UcGramRow kGramJs[] = {
{ "comment.block.js", 0, "/\\*", "\\*/", 0, 0,0,0, 0,0 },
{ "comment.line.double-slash.js", "//.*", 0,0,0, 0,0,0, 0,0 },
{ "string.template.js", 0, "`", "`", 0, 0,0,0,
  "constant.character.escape.js", "\\\\." },
{ "string.quoted.double.js", 0, "\"", "\"", 0, 0,0,0,
  "constant.character.escape.js", "\\\\." },
{ "string.quoted.single.js", 0, "'", "'", 0, 0,0,0,
  "constant.character.escape.js", "\\\\." },
{ "constant.numeric.js", "\\b(0[xX][0-9a-fA-F]+|[0-9]+\\.?[0-9]*([eE][-+]?[0-9]+)?)\\b", 0,0,0, 0,0,0, 0,0 },
{ "", "\\b(function)\\s+([A-Za-z_$][A-Za-z0-9_$]*)", 0,0,0,
  "storage.type.function.js", "entity.name.function.js", 0, 0,0 },
{ "keyword.control.js", "\\b(if|else|for|while|do|switch|case|default|break|continue|return|throw|try|catch|finally|new|delete|typeof|instanceof|in|of|yield|await|import|export|from|as)\\b", 0,0,0, 0,0,0, 0,0 },
{ "storage.type.js", "\\b(var|let|const|function|class|extends|async|static|get|set)\\b", 0,0,0, 0,0,0, 0,0 },
{ "constant.language.js", "\\b(true|false|null|undefined|NaN|Infinity)\\b", 0,0,0, 0,0,0, 0,0 },
{ "variable.language.js", "\\b(this|super|arguments)\\b", 0,0,0, 0,0,0, 0,0 },
{ "support.class.js", "\\b(console|Math|JSON|Object|Array|String|Number|Promise|vscode|require|module|exports)\\b", 0,0,0, 0,0,0, 0,0 },
{ "keyword.operator.js", "[-+*/%=!<>&|^~?]+", 0,0,0, 0,0,0, 0,0 },
{ 0,0,0,0,0,0,0,0,0,0 }
};

static const UcGramRow kGramJson[] = {
{ "comment.block.json", 0, "/\\*", "\\*/", 0, 0,0,0, 0,0 },
{ "comment.line.double-slash.json", "//.*", 0,0,0, 0,0,0, 0,0 },
{ "", "(\"(\\\\.|[^\"\\\\])*\")\\s*:", 0,0,0, "support.type.property-name.json", 0,0, 0,0 },
{ "string.quoted.double.json", 0, "\"", "\"", 0, 0,0,0,
  "constant.character.escape.json", "\\\\." },
{ "constant.numeric.json", "-?\\b[0-9]+\\.?[0-9]*([eE][-+]?[0-9]+)?\\b", 0,0,0, 0,0,0, 0,0 },
{ "constant.language.json", "\\b(true|false|null)\\b", 0,0,0, 0,0,0, 0,0 },
{ "punctuation.definition.json", "[{}\\[\\],:]", 0,0,0, 0,0,0, 0,0 },
{ 0,0,0,0,0,0,0,0,0,0 }
};

static const UcGramRow kGramMd[] = {
{ "markup.raw.block.markdown", 0, "^```", "^```", 0, 0,0,0, 0,0 },
{ "markup.heading.markdown", "^#{1,6}\\s.*", 0,0,0, 0,0,0, 0,0 },
{ "markup.quote.markdown", "^>\\s?.*", 0,0,0, 0,0,0, 0,0 },
{ "markup.list.markdown", "^\\s*([-*+]|[0-9]+\\.)\\s", 0,0,0, 0,0,0, 0,0 },
{ "markup.inline.raw.markdown", "`[^`]*`", 0,0,0, 0,0,0, 0,0 },
{ "markup.bold.markdown", "\\*\\*[^*]+\\*\\*", 0,0,0, 0,0,0, 0,0 },
{ "markup.italic.markdown", "\\*[^*]+\\*", 0,0,0, 0,0,0, 0,0 },
{ "", "(\\[[^\\]]*\\])(\\([^)]*\\))", 0,0,0,
  "string.other.link.title.markdown", "markup.underline.link.markdown", 0, 0,0 },
{ "markup.heading.setext.markdown", "^(=+|-{2,})$", 0,0,0, 0,0,0, 0,0 },
{ 0,0,0,0,0,0,0,0,0,0 }
};

static const UcGramRow kGramHtml[] = {
{ "comment.block.html", 0, "<!--", "-->", 0, 0,0,0, 0,0 },
{ "", "(</?)([A-Za-z][A-Za-z0-9-]*)", 0,0,0,
  "punctuation.definition.tag.html", "entity.name.tag.html", 0, 0,0 },
{ "string.quoted.double.html", 0, "\"", "\"", 0, 0,0,0, 0,0 },
{ "entity.other.attribute-name.html", "\\b[A-Za-z-]+(?=\\s*=)", 0,0,0, 0,0,0, 0,0 },
{ "constant.character.entity.html", "&[a-zA-Z#0-9]+;", 0,0,0, 0,0,0, 0,0 },
{ "punctuation.definition.tag.html", "[<>/]", 0,0,0, 0,0,0, 0,0 },
{ 0,0,0,0,0,0,0,0,0,0 }
};

static const UcGramRow kGramCss[] = {
{ "comment.block.css", 0, "/\\*", "\\*/", 0, 0,0,0, 0,0 },
{ "string.quoted.double.css", 0, "\"", "\"", 0, 0,0,0, 0,0 },
{ "", "([-a-zA-Z]+)\\s*:", 0,0,0, "support.type.property-name.css", 0,0, 0,0 },
{ "constant.numeric.css", "-?\\b[0-9]+(\\.[0-9]+)?(px|em|rem|%|vh|vw|pt|s|ms)?\\b", 0,0,0, 0,0,0, 0,0 },
{ "constant.other.color.css", "#[0-9a-fA-F]{3,8}\\b", 0,0,0, 0,0,0, 0,0 },
{ "entity.name.tag.css", "^\\s*[.#]?[-A-Za-z0-9_]+(?=[^:]*\\{)", 0,0,0, 0,0,0, 0,0 },
{ "keyword.control.at-rule.css", "@[-a-z]+", 0,0,0, 0,0,0, 0,0 },
{ 0,0,0,0,0,0,0,0,0,0 }
};

/* ---- language table -------------------------------------------------------- */
static UcLang g_lang[UC_LANG_MAX];
static int    g_nlang;

int     uc_lang_count(void) { return g_nlang; }
UcLang *uc_lang_at(int i) { return (i >= 0 && i < g_nlang) ? &g_lang[i] : 0; }

int uc_lang_by_id(const char *id)
{
    int i;
    for (i = 0; i < g_nlang; i++) if (!strcmp(g_lang[i].id, id)) return i;
    return -1;
}

int uc_lang_for_file(const char *name)
{
    int i, j;
    const char *dot = 0, *p;
    if (!name) return 0;
    for (p = name; *p; p++) if (*p == '.') dot = p;
    /* a name with no extension: MAKEFILE and friends are matched whole */
    for (i = 0; i < g_nlang; i++)
        for (j = 0; j < g_lang[i].next; j++)
            if (g_lang[i].ext[j][0] != '.' && uc_ieq(g_lang[i].ext[j], name))
                return i;
    if (!dot) return 0;
    for (i = 0; i < g_nlang; i++)
        for (j = 0; j < g_lang[i].next; j++)
            if (g_lang[i].ext[j][0] == '.' && uc_ieq(g_lang[i].ext[j], dot))
                return i;
    return 0;
}

/* ---- grammar construction -------------------------------------------------- */
static UcGrammar *gram_new(const char *scope, int cap)
{
    UcGrammar *g = (UcGrammar *)malloc(sizeof(UcGrammar));
    if (!g) return 0;
    memset(g, 0, sizeof *g);
    g->pat = (UcPattern *)malloc((unsigned long)cap * sizeof(UcPattern));
    if (!g->pat) { free(g); return 0; }
    memset(g->pat, 0, (unsigned long)cap * sizeof(UcPattern));
    g->pcap = cap;
    uc_scpy(g->scope, scope, sizeof g->scope);
    return g;
}

/* n contiguous slots.  The pool GROWS (UCD-28): it used to be a fixed 288 and
 * a published grammar wants thousands - `#name` includes are inlined by COPY,
 * so a repository entry used in twenty places costs twenty copies.  Running out
 * was silent, and what it silently did was drop every rule after the one that
 * hit the cap, which is how a grammar could load, report success and colour
 * nothing.
 *
 * A GROWING POOL MOVES.  Patterns address their children by INDEX (`sub`,
 * `nsub`), so nothing stored survives a move badly - but a CALLER holding a
 * `UcPattern *` across a call that can take more slots is holding a dangling
 * pointer, and both builders did.  They work on a local and write it back at
 * a freshly computed address; see build_one().  This is the failure that
 * turned "the pool is too small" into a segfault the moment it stopped
 * being too small. */
static int gram_take(UcGrammar *g, int n)      /* -1 = out of memory or too big */
{
    int at = g->npat;
    if (at + n > g->pcap) {
        int ncap = g->pcap ? g->pcap * 2 : 64;
        UcPattern *np;
        while (ncap < at + n) ncap *= 2;
        /* `sub`/`nsub` are `short`, so the pool cannot exceed what they can
         * address.  A grammar past that is refused rather than truncated. */
        if (ncap > 32000) { g->pool_full = 1; return -1; }
        np = (UcPattern *)malloc((unsigned long)ncap * sizeof(UcPattern));
        if (!np) { g->pool_full = 1; return -1; }
        if (g->pat) {
            memcpy(np, g->pat, (unsigned long)g->npat * sizeof(UcPattern));
            free(g->pat);
        }
        memset(np + g->npat, 0, (unsigned long)(ncap - g->npat) * sizeof(UcPattern));
        g->pat = np;
        g->pcap = ncap;
    }
    g->npat += n;
    return at;
}

/* The reason the LAST failed compile failed, and how many have.  The error
 * string was being written into a stack buffer and thrown away at every call
 * site, so a grammar could lose half its rules without a word (UCD-28). */
static char g_rx_err[80];
static int  g_rx_bad;

static UcRx *rx_or_null(const char *pat)
{
    char err[80];
    UcRx *rx;
    if (!pat || !pat[0]) return 0;
    rx = uc_rx_compile(pat, 0, err, sizeof err);
    if (!rx) {
        g_rx_bad++;
        uc_scpy(g_rx_err, err[0] ? err : "regex did not compile", sizeof g_rx_err);
    }
    return rx;
}

static UcGrammar *gram_from_rows(const char *scope, const UcGramRow *rows)
{
    int n = 0, i, base;
    UcGrammar *g;
    while (rows[n].name || rows[n].match || rows[n].begin) n++;
    g = gram_new(scope, n * 2 + 4);
    if (!g) return 0;
    base = gram_take(g, n);
    g->top = (short)base;
    g->ntop = (short)n;
    for (i = 0; i < n; i++) {
        UcPattern tmp;
        UcPattern *p = &tmp;          /* a LOCAL: gram_take() below may move
                                       * the pool out from under a pointer
                                       * into it */
        memset(&tmp, 0, sizeof tmp);
        uc_scpy(p->name, rows[i].name ? rows[i].name : "", sizeof p->name);
        p->match = rx_or_null(rows[i].match);
        p->begin = rx_or_null(rows[i].begin);
        p->end   = rx_or_null(rows[i].end);
        uc_scpy(p->content, rows[i].content ? rows[i].content : "", sizeof p->content);
        if (rows[i].cap1) uc_scpy(p->cap[1], rows[i].cap1, sizeof p->cap[0]);
        if (rows[i].cap2) uc_scpy(p->cap[2], rows[i].cap2, sizeof p->cap[0]);
        if (rows[i].cap3) uc_scpy(p->cap[3], rows[i].cap3, sizeof p->cap[0]);
        if (rows[i].sub_match) {
            int s = gram_take(g, 1);
            if (s >= 0) {
                uc_scpy(g->pat[s].name, rows[i].sub_name ? rows[i].sub_name : "",
                        sizeof g->pat[s].name);
                g->pat[s].match = rx_or_null(rows[i].sub_match);
                p->sub = (short)s;
                p->nsub = 1;
            }
        }
        g->pat[base + i] = tmp;       /* address computed AFTER any growth */
    }
    g->ok = 1;
    return g;
}

/* ---- grammars from JSON ---------------------------------------------------- */
/* A repository entry is built ONCE and referred to, not copied (UCD-28).
 *
 * `#name` used to be inlined by copy: every include of `#expression` produced
 * its own copy of that rule set and its own freshly compiled regexes.  For a
 * hand-written grammar with four repository entries that is merely wasteful.
 * For a published one it is fatal - TypeScript's entries reference each other
 * densely, so copying expands combinatorially: the grammar filled the pattern
 * pool and then recursed until the C stack ran out.  The old fixed 288-slot
 * pool had been hiding that, which is why simply growing the pool turned a
 * silent truncation into a segfault.
 *
 * Referring instead makes an include an index, makes a cycle finite, and
 * compiles each rule's regex exactly once. */
#define UC_REPO_MAX        256
#define UC_GRAM_BUILD_MAX  32      /* reference-chain depth while building  */

typedef struct {
    UcGrammar *g;
    UcJson    *root;
    UcJson    *repo;
    int        depth;
    const char *repo_name[UC_REPO_MAX];
    short       repo_first[UC_REPO_MAX], repo_n[UC_REPO_MAX];
    int         nrepo;
} GramBuild;

static void build_list(GramBuild *b, UcJson *arr, int *out_first, int *out_n);
static int  build_one(GramBuild *b, UcJson *rule, int slot);

/* The slice for `#name`, built the first time it is asked for. */
static int repo_slice(GramBuild *b, const char *name, int *first, int *n)
{
    UcJson *r, *pats;
    int i, cnt, base;
    *first = 0; *n = 0;
    for (i = 0; i < b->nrepo; i++)
        if (!strcmp(b->repo_name[i], name)) {
            *first = b->repo_first[i];
            *n = b->repo_n[i];
            return *n > 0;
        }
    if (!b->repo || b->nrepo >= UC_REPO_MAX) return 0;
    r = uc_json_member(b->repo, name);
    if (!r) return 0;
    pats = uc_json_member(r, "patterns");
    cnt = (pats && pats->type == UJ_ARR && pats->n > 0) ? pats->n : 1;
    base = gram_take(b->g, cnt);
    if (base < 0) return 0;

    /* CACHED BEFORE IT IS BUILT.  An entry that references itself, or a cycle
     * through several, has to find the slice already reserved and point at it.
     * Building first and caching after is exactly how a cycle becomes an
     * infinite expansion. */
    i = b->nrepo++;
    b->repo_name[i] = name;
    b->repo_first[i] = (short)base;
    b->repo_n[i] = (short)cnt;

    if (pats && pats->type == UJ_ARR && pats->n > 0) {
        UcJson *e;
        int k;
        for (k = 0, e = pats->child; e && k < cnt; e = e->next, k++) {
            if (build_one(b, e, base + k)) b->g->nbuilt++;
            else b->g->ndropped++;
        }
    } else {
        if (build_one(b, r, base)) b->g->nbuilt++;
        else b->g->ndropped++;
    }
    *first = base;
    *n = cnt;
    return 1;
}

static void captures_into(UcPattern *p, UcJson *caps)
{
    UcJson *m;
    if (!caps) return;
    for (m = caps->child; m; m = m->next) {
        int idx;
        const char *nm;
        if (!m->key) continue;
        idx = (int)strtol(m->key, 0, 10);
        if (idx < 0 || idx >= UC_RX_CAPS) continue;
        nm = uc_json_str(m, "name", 0);
        if (nm) uc_scpy(p->cap[idx], nm, sizeof p->cap[0]);
    }
}

/* Fill one pattern slot from a JSON rule.  Returns 0 if the rule is one we
 * cannot represent, in which case the slot is left inert (it matches nothing)
 * rather than silently becoming a different rule. */
/* Build one rule into a LOCAL, then write it into the pool at an address
 * computed afterwards.  Anything in here can call gram_take() - an `include`
 * expands into fresh slots - and gram_take() can move the pool. */
static int build_one_into(GramBuild *b, UcJson *rule, UcPattern *p);

static int build_one(GramBuild *b, UcJson *rule, int slot)
{
    UcPattern tmp;
    int ok = build_one_into(b, rule, &tmp);
    b->g->pat[slot] = tmp;
    return ok;
}

static int build_one_into(GramBuild *b, UcJson *rule, UcPattern *p)
{
    const char *inc, *m, *bg, *en, *nm;
    char err[64];
    memset(p, 0, sizeof *p);
    inc = uc_json_str(rule, "include", 0);
    if (inc) {
        if (!strcmp(inc, "$self")) { p->self = 1; return 1; }
        if (inc[0] == '#' && b->repo && b->depth < UC_GRAM_BUILD_MAX) {
            int first = 0, n = 0;
            b->depth++;
            repo_slice(b, inc + 1, &first, &n);
            b->depth--;
            p->sub = (short)first;
            p->nsub = (short)n;
            return n > 0;
        }
        return 0;                      /* include of another grammar: skipped */
    }
    nm = uc_json_str(rule, "name", 0);
    if (nm) uc_scpy(p->name, nm, sizeof p->name);
    nm = uc_json_str(rule, "contentName", 0);
    if (nm) uc_scpy(p->content, nm, sizeof p->content);

    m  = uc_json_str(rule, "match", 0);
    bg = uc_json_str(rule, "begin", 0);
    en = uc_json_str(rule, "end", 0);
    if (m) {
        p->match = uc_rx_compile(m, 0, err, sizeof err);
        captures_into(p, uc_json_member(rule, "captures"));
        return p->match != 0;
    }
    if (bg && en) {
        UcJson *pats;
        p->begin = uc_rx_compile(bg, 0, err, sizeof err);
        p->end   = uc_rx_compile(en, 0, err, sizeof err);
        captures_into(p, uc_json_member(rule, "beginCaptures"));
        if (!p->cap[0][0]) captures_into(p, uc_json_member(rule, "captures"));
        pats = uc_json_member(rule, "patterns");
        if (pats && b->depth < UC_GRAM_DEPTH) {
            int first = 0, n = 0;
            b->depth++;
            build_list(b, pats, &first, &n);
            b->depth--;
            p->sub = (short)first;
            p->nsub = (short)n;
        }
        return p->begin && p->end;
    }
    /* a bare { "patterns": [...] } group */
    {
        UcJson *pats = uc_json_member(rule, "patterns");
        if (pats && b->depth < UC_GRAM_DEPTH) {
            int first = 0, n = 0;
            b->depth++;
            build_list(b, pats, &first, &n);
            b->depth--;
            p->sub = (short)first;
            p->nsub = (short)n;
            return n > 0;
        }
    }
    return 0;
}

/* A patterns array becomes a CONTIGUOUS slice of the pool: the slots are
 * reserved first, then filled, so a child rule appended during the fill lands
 * after the slice instead of inside it. */
static void build_list(GramBuild *b, UcJson *arr, int *out_first, int *out_n)
{
    int n, base, i;
    UcJson *e;
    *out_first = 0; *out_n = 0;
    if (!arr || arr->type != UJ_ARR || arr->n <= 0) return;
    n = arr->n;
    base = gram_take(b->g, n);
    if (base < 0) return;
    *out_first = base;
    *out_n = n;
    for (i = 0, e = arr->child; e && i < n; e = e->next, i++) {
        if (build_one(b, e, base + i)) b->g->nbuilt++;
        else b->g->ndropped++;
    }
}

static void gram_free(UcGrammar *g)
{
    int i;
    if (!g) return;
    /* Any interned tokenizer state that names this grammar is now a pattern
     * index into freed memory.  Bumping the generation retires them all
     * without having to find them (UCD-28). */
    uc_hl_state_invalidate();
    for (i = 0; i < g->npat; i++) {
        uc_rx_free(g->pat[i].match);
        uc_rx_free(g->pat[i].begin);
        uc_rx_free(g->pat[i].end);
    }
    free(g->pat);
    free(g);
}

int uc_lang_load_grammar(int lang, int vol, const char *path)
{
    char *src = 0;
    long len = 0;
    char err[80];
    UcJson *root;
    GramBuild b;
    UcGrammar *g;
    int first = 0, n = 0;
    if (lang < 0 || lang >= g_nlang) return 0;
    if (!uc_read_file(vol, path, &src, &len)) return 0;
    root = uc_json_parse(src, (int)len, err, sizeof err);
    free(src);
    if (!root) return 0;
    g = gram_new(uc_json_str(root, "scopeName", "source.unknown"), UC_GRAM_PATTERNS * 3);
    if (!g) { uc_json_free(root); return 0; }
    /* ZEROED, not field-by-field.  It grew a repository cache (UCD-28) and the
     * old initialisation set four fields by name, so `nrepo` was whatever was
     * on the stack and the very first lookup walked off the end of the array. */
    memset(&b, 0, sizeof b);
    b.g = g; b.root = root; b.repo = uc_json_member(root, "repository");
    g_rx_bad = 0;
    g_rx_err[0] = 0;
    build_list(&b, uc_json_member(root, "patterns"), &first, &n);
    uc_json_free(root);
    if (n <= 0) { gram_free(g); return 0; }
    g->top = (short)first;
    g->ntop = (short)n;
    g->nregex_bad = g_rx_bad;
    g->ok = 1;
    /* SAY SO.  A grammar that lost rules used to load in silence, and silence
     * is why nobody knew that the two biggest published grammars in the world
     * were arriving as a third of themselves. */
    {
        int ch = uc_output_channel("Log");
        char msg[200], num[16];
        uc_scpy(msg, "grammar ", sizeof msg);
        uc_scat(msg, g->scope, sizeof msg);
        uc_scat(msg, ": ", sizeof msg);
        uc_itoa(num, g->nbuilt);
        uc_scat(msg, num, sizeof msg);
        uc_scat(msg, " rules", sizeof msg);
        if (g->ndropped) {
            uc_itoa(num, g->ndropped);
            uc_scat(msg, ", ", sizeof msg);
            uc_scat(msg, num, sizeof msg);
            uc_scat(msg, " dropped", sizeof msg);
        }
        if (g->nregex_bad) {
            uc_itoa(num, g->nregex_bad);
            uc_scat(msg, " (", sizeof msg);
            uc_scat(msg, num, sizeof msg);
            uc_scat(msg, " bad regex, last: ", sizeof msg);
            uc_scat(msg, g_rx_err, sizeof msg);
            uc_scat(msg, ")", sizeof msg);
        }
        if (g->pool_full) uc_scat(msg, " - PATTERN POOL EXHAUSTED", sizeof msg);
        { char nl[2]; nl[0] = 0x0a; nl[1] = 0; uc_scat(msg, nl, sizeof msg); }
        uc_output_write(ch, msg);
    }
    if (g_lang[lang].gram) gram_free(g_lang[lang].gram);
    g_lang[lang].gram = g;
    return 1;
}

/* ---- language registration -------------------------------------------------- */
static int lang_add(const char *id, const char *name, const char *exts,
                    const char *lc, const char *bo, const char *bc,
                    const char *keywords, const UcGramRow *rows,
                    const char *scope)
{
    UcLang *L;
    int i = g_nlang;
    const char *p = exts;
    if (i >= UC_LANG_MAX) return -1;
    L = &g_lang[i];
    memset(L, 0, sizeof *L);
    uc_scpy(L->id, id, sizeof L->id);
    uc_scpy(L->name, name, sizeof L->name);
    while (*p && L->next < UC_LANG_EXTS) {
        int n = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < (int)sizeof L->ext[0] - 1) L->ext[L->next][n++] = *p++;
        L->ext[L->next][n] = 0;
        if (n) L->next++;
    }
    uc_scpy(L->line_comment, lc ? lc : "", sizeof L->line_comment);
    uc_scpy(L->block_open, bo ? bo : "", sizeof L->block_open);
    uc_scpy(L->block_close, bc ? bc : "", sizeof L->block_close);
    L->keywords = keywords;
    L->tabsize = 4;
    L->ext_index = -1;
    if (rows) L->gram = gram_from_rows(scope, rows);
    g_nlang++;
    return i;
}

int uc_lang_register(const UcLang *proto)
{
    int i;
    if (g_nlang >= UC_LANG_MAX || !proto) return -1;
    i = uc_lang_by_id(proto->id);
    if (i >= 0) return i;                     /* already known: reuse the slot */
    i = g_nlang++;
    g_lang[i] = *proto;
    g_lang[i].gram = 0;
    return i;
}

void uc_lang_init(void)
{
    g_nscope = 1;
    g_scope[0][0] = 0;
    g_nlang = 0;
    /* index 0 must be plaintext: uc_lang_for_file() returns it for anything
     * it does not recognise, and every caller relies on that being valid */
    lang_add("plaintext", "Plain Text", ".TXT .LOG .MD5", 0, 0, 0, 0, 0, 0);
    lang_add("c", "C", ".C .H .CPP .HPP .CC", "//", "/*", "*/", KW_C, kGramC, "source.c");
    lang_add("python", "Python", ".PY .PYI", "#", 0, 0, KW_PY, kGramPy, "source.python");
    lang_add("javascript", "JavaScript", ".JS .MJS", "//", "/*", "*/", KW_JS, kGramJs, "source.js");
    lang_add("json", "JSON", ".JSN .JSON .CFG .MFT", "//", "/*", "*/",
             "true false null", kGramJson, "source.json");
    lang_add("markdown", "Markdown", ".MD .MARKDOWN", 0, 0, 0, 0, kGramMd, "text.html.markdown");
    lang_add("html", "HTML", ".HTM .HTML", 0, "<!--", "-->", 0, kGramHtml, "text.html.basic");
    lang_add("css", "CSS", ".CSS", 0, "/*", "*/", 0, kGramCss, "source.css");
}

/* ===========================================================================
 * The highlighter.
 * ======================================================================== */
typedef struct {
    UcGrammar  *g;
    const char *s;
    int         len;
    short      *out;
    int         steps;
    unsigned    gen;            /* which line the match cache belongs to     */
} HlCtx;

/* ---- the per-line match cache ----------------------------------------------
 * THE ONE OPTIMISATION THIS TOKENIZER CANNOT DO WITHOUT.
 *
 * scan() walks a line position by position and, at each position, asks every
 * reachable rule where it next matches.  A real grammar has hundreds of rules
 * and a line has dozens of positions, so that is tens of thousands of regex
 * executions per line - Microsoft's TypeScript grammar took 32 seconds to
 * colour a 1300-line file, which is not a slow editor, it is a broken one.
 *
 * The observation that fixes it: `uc_rx_exec(rx, s, to, pos)` returns the
 * LEFTMOST match at or after `pos`.  If a rule was already found to match at
 * `at`, and `at >= pos`, that is still the answer - the text has not changed.
 * And if a rule was found not to match from some earlier position, it cannot
 * match from a later one either.  So each rule runs at most once per line,
 * plus once more each time the scan passes the position it had matched at.
 *
 * The cache is keyed by pattern-pool index and validated by a generation
 * number, so a new line costs an integer bump rather than a memset of an
 * array with one entry per rule in the grammar. */
typedef struct {
    unsigned gen;
    int      from;              /* the position the search started from      */
    int      at;                /* where it matched, or -1 for "nowhere"     */
    int      end;
    int      caps[UC_RX_CAPS * 2];
} HlHit;

static HlHit  *g_hit;
static int     g_hitcap;
static unsigned g_hitgen;

/* How many regexes the tokenizer has actually executed.  Exposed because the
 * cost of this loop is the thing that regresses, and it regresses by ORDERS -
 * a missing cache took a real grammar from a millisecond a line to twenty-five
 * - while looking identical in every screenshot and every scope assertion.
 * Counting executions is a measure of the machine's work that does not depend
 * on which machine it is. */
static unsigned long g_rx_calls;
unsigned long uc_hl_rx_calls(void) { return g_rx_calls; }

static int hit_reserve(int n)
{
    HlHit *p;
    if (n <= g_hitcap) return 1;
    p = (HlHit *)malloc((unsigned long)n * sizeof(HlHit));
    if (!p) return 0;
    memset(p, 0, (unsigned long)n * sizeof(HlHit));
    if (g_hit) free(g_hit);
    g_hit = p;
    g_hitcap = n;
    g_hitgen++;                 /* every old entry is now stale */
    return 1;
}

/* Where rule `idx` next matches at or after `pos`, through the cache. */
static int rule_match(HlCtx *h, int idx, UcRx *rx, int pos, int to, int *caps)
{
    HlHit *c;
    /* A \G rule is NOT cacheable: it anchors at the position the search was
     * told to start from, so "matches nowhere" determined from column 0 says
     * nothing about column 8 - where it may well match.  Everything else is
     * position-independent in the way the cache needs. */
    if (idx < 0 || idx >= g_hitcap || uc_rx_ganchored(rx)) {
        g_rx_calls++;
        return uc_rx_exec(rx, h->s, to, pos, pos == 0, caps);
    }
    c = &g_hit[idx];
    if (c->gen == h->gen && c->from <= pos) {
        if (c->at < 0) return 0;                       /* nothing, ever again */
        if (c->at >= pos) {
            memcpy(caps, c->caps, sizeof c->caps);
            return 1;
        }
    }
    if (++h->steps > 40000) return 0;
    g_rx_calls++;
    if (!uc_rx_exec(rx, h->s, to, pos, pos == 0, caps)) {
        c->gen = h->gen; c->from = pos; c->at = -1;
        return 0;
    }
    c->gen = h->gen; c->from = pos; c->at = caps[0]; c->end = caps[1];
    memcpy(c->caps, caps, sizeof c->caps);
    return 1;
}

/* ---- the cross-line state, which is a STACK (UCD-28) -----------------------
 * A line can begin inside a comment inside a template string inside a function
 * body.  The state carried from one line to the next used to be ONE pattern
 * index - the innermost rule left open - so closing that rule dropped the
 * editor back to the top level rather than back into whatever contained it.
 * A block comment's closing delimiter, appearing inside a string that was
 * itself inside something, ended the string and dropped everything after it
 * to plain text.  Every real grammar nests, so this was not an edge case.
 *
 * THE STACK IS INTERNED RATHER THAN STORED.  UcDoc keeps one `unsigned short`
 * per line and there are tens of thousands of lines; a stack per line would be
 * a heap allocation per line, invalidated on every keystroke.  Instead each
 * distinct stack becomes an id in a pool, and a stack is a chain of
 * (parent, pattern) links - so the per-line cost stays two bytes and the
 * per-DOCUMENT cost is the number of distinct nestings that actually occur,
 * which is small even in a large file.
 *
 * A state records the grammar it belongs to AND a generation, because ids
 * outlive grammars: changing a document's language, or an extension reloading
 * its grammar, leaves old ids in a document's cache and the pattern index
 * inside them would then address a different rule.  A stale id reads as "no
 * state", which is exactly the safe answer.
 * ======================================================================== */
#define UC_HLSTATE_MAX 4096
typedef struct {
    unsigned short parent;      /* 0 = the bottom of the stack               */
    short          pat;         /* index into the grammar's pattern pool     */
    const UcGrammar *g;
    unsigned       gen;
} HlState;

static HlState g_hlstate[UC_HLSTATE_MAX];
static int     g_nhlstate = 1;  /* id 0 is "nothing open" and is never stored */
static unsigned g_gram_gen = 1;

/* Called whenever a grammar is loaded or freed.  Old ids do not have to be
 * hunted down - they simply stop validating. */
void uc_hl_state_invalidate(void) { g_gram_gen++; }

static int hl_state_valid(const UcGrammar *g, int id)
{
    return id > 0 && id < g_nhlstate &&
           g_hlstate[id].g == g && g_hlstate[id].gen == g_gram_gen;
}

static int hl_state_push(const UcGrammar *g, int parent, int pat)
{
    int i;
    if (pat < 0) return parent;
    for (i = 1; i < g_nhlstate; i++)
        if (g_hlstate[i].parent == parent && g_hlstate[i].pat == pat &&
            g_hlstate[i].g == g && g_hlstate[i].gen == g_gram_gen)
            return i;
    /* Out of pool.  DEGRADE to the caller's own state rather than fail: the
     * line still colours, it just forgets one level of nesting - which is what
     * every line did before this existed. */
    if (g_nhlstate >= UC_HLSTATE_MAX) return parent;
    g_hlstate[g_nhlstate].parent = (unsigned short)parent;
    g_hlstate[g_nhlstate].pat = (short)pat;
    g_hlstate[g_nhlstate].g = g;
    g_hlstate[g_nhlstate].gen = g_gram_gen;
    return g_nhlstate++;
}

static void paint(HlCtx *h, int a, int b, int scope)
{
    int i;
    if (a < 0) a = 0;
    if (b > h->len) b = h->len;
    for (i = a; i < b; i++) h->out[i] = (short)scope;
}

/* Paint one match's own scope plus any capture scopes over the top.  Capture
 * scopes are applied AFTER the whole-match scope, which is what makes
 * "(def)\s+(name)" colour its two halves differently without a merge pass. */
static void paint_match(HlCtx *h, UcPattern *p, const int *caps, int use_name)
{
    int k;
    if (use_name && p->name[0]) paint(h, caps[0], caps[1], uc_scope_id(p->name));
    for (k = 0; k < UC_RX_CAPS; k++) {
        if (!p->cap[k][0]) continue;
        if (caps[k * 2] < 0) continue;
        paint(h, caps[k * 2], caps[k * 2 + 1], uc_scope_id(p->cap[k]));
    }
}

/* Scan [from,to) against the pattern slice [first,first+n), honouring an
 * enclosing begin/end rule when `encl` is non-NULL.
 *
 * Returns the interned STATE open when the line ends - which is `encl_state`
 * if nothing deeper opened, and something deeper if it did - or the sentinel
 * -2 to say the enclosing rule CLOSED, in which case *closed_at receives the
 * position just past its end match.
 *
 * Carrying the position back is new (UCD-28) and is not a tidiness: the old
 * -2 said only "it closed", so both callers re-ran the end regex to find out
 * where.  That is one wasted regex execution per closed block per line, and
 * two places that had to agree about a zero-width end match. */
/* The leftmost match among the rules in [first,first+n), following group and
 * $self includes TO ANY DEPTH.
 *
 * A GROUP IS NOT A RULE.  `{"include": "#expression"}` resolves to a repository
 * entry that is itself a list of includes, which resolve to more lists.  The
 * old code expanded exactly one level and skipped anything deeper - fine for a
 * hand-written grammar, fatal for a published one.  Microsoft's TypeScript
 * grammar is nothing BUT includes of includes: one level of expansion found no
 * rules at all, so the file loaded, reported success, and came out entirely
 * plain.  That is the "or mis-colour" half of this task, and it survived the
 * whole regex rewrite because the two failures look identical from outside.
 *
 * `seen` is a cycle guard, not an optimisation: a repository entry that
 * includes itself - directly, or through $self - must not be a hang.
 * Depth-first in declaration order, so the first rule listed still wins a tie,
 * which is TextMate's rule. */
#define UC_GRAM_SEEN 24
static void best_in(HlCtx *h, int first, int n, int pos, int to,
                    int *best, int *best_at, int *best_end, int *best_caps,
                    short *seen, int *nseen, int depth)
{
    int i;
    if (depth > UC_GRAM_DEPTH) return;
    for (i = 0; i < n; i++) {
        UcPattern *p = &h->g->pat[first + i];
        UcRx *rx = p->match ? p->match : p->begin;
        int caps[UC_RX_CAPS * 2];
        if (p->self || (!rx && p->nsub > 0)) {
            int sf = p->self ? h->g->top : p->sub;
            int sn = p->self ? h->g->ntop : p->nsub;
            int k, dup = 0;
            for (k = 0; k < *nseen; k++) if (seen[k] == (short)sf) { dup = 1; break; }
            if (dup || *nseen >= UC_GRAM_SEEN) continue;
            seen[(*nseen)++] = (short)sf;
            best_in(h, sf, sn, pos, to, best, best_at, best_end, best_caps,
                    seen, nseen, depth + 1);
            (*nseen)--;
            continue;
        }
        if (!rx) continue;
        if (h->steps > 40000) return;
        if (!rule_match(h, first + i, rx, pos, to, caps)) continue;
        if (caps[0] < *best_at) {
            *best_at = caps[0];
            *best = first + i;
            *best_end = caps[1];
            memcpy(best_caps, caps, sizeof caps);
        }
    }
}

static int scan(HlCtx *h, int first, int n, int from, int to, UcPattern *encl,
                int encl_state, int depth, int *closed_at)
{
    int pos = from;
    if (depth > UC_GRAM_DEPTH) return encl_state;
    while (pos < to) {
        int best = -1, best_at = to + 1, best_end = 0;
        int best_caps[UC_RX_CAPS * 2], caps[UC_RX_CAPS * 2];
        int end_at = -1, end_to = 0;

        best_caps[0] = -1;

        if (++h->steps > 40000) break;      /* a pathological grammar stops here */

        /* the enclosing rule's end pattern competes with its children */
        g_rx_calls += (encl && encl->end) ? 1 : 0;
        if (encl && encl->end &&
            uc_rx_exec(encl->end, h->s, to, pos, pos == 0, caps)) {
            end_at = caps[0];
            end_to = caps[1];
            if (end_to == end_at) end_to = end_at + 1;   /* never stall */
        }

        {
            short seen[UC_GRAM_SEEN];
            int nseen = 0;
            best_in(h, first, n, pos, to, &best, &best_at, &best_end,
                    best_caps, seen, &nseen, 0);
        }

        /* the enclosing end wins if it starts no later than the best child */
        if (end_at >= 0 && (best < 0 || end_at <= best_at)) {
            if (encl->content[0]) paint(h, pos, end_at, uc_scope_id(encl->content));
            else if (encl->name[0]) paint(h, pos, end_at, uc_scope_id(encl->name));
            if (encl->name[0]) paint(h, end_at, end_to, uc_scope_id(encl->name));
            if (closed_at) *closed_at = end_to;
            return -2;                          /* the rule closed on this line */
        }
        if (best < 0) break;

        {
            UcPattern *p = &h->g->pat[best];
            /* the run before the match keeps the enclosing scope */
            if (encl) {
                int sc = encl->content[0] ? uc_scope_id(encl->content)
                                          : (encl->name[0] ? uc_scope_id(encl->name) : 0);
                paint(h, pos, best_at, sc);
            }
            if (p->match) {
                paint_match(h, p, best_caps, 1);
                pos = best_end > best_at ? best_end : best_at + 1;
                continue;
            }
            /* begin/end */
            paint_match(h, p, best_caps, 1);
            {
                int inner_from = best_end > best_at ? best_end : best_at + 1;
                int child = hl_state_push(h->g, encl_state, best);
                int cl = to;
                int r = scan(h, p->nsub ? p->sub : 0, p->nsub, inner_from, to,
                             p, child, depth + 1, &cl);
                if (r == -2) { pos = cl > pos ? cl : pos + 1; continue; }
                /* Whatever the CHILD left open, not merely this rule.  The old
                 * code recorded `best` here and threw `r` away, and that one
                 * line is the whole of "the state is not a stack": a rule left
                 * open inside another was forgotten, so the next line resumed
                 * one level too shallow. */
                return r;
            }
        }
    }
    if (pos < to && encl) {
        int sc = encl->content[0] ? uc_scope_id(encl->content)
                                  : (encl->name[0] ? uc_scope_id(encl->name) : 0);
        paint(h, pos, to, sc);
    }
    return encl_state;
}

int uc_tokenize(int lang, const char *line, int len, int state_in,
                short *scope_out, int *state_out)
{
    HlCtx h;
    UcLang *L = uc_lang_at(lang);
    int i, open, pos = 0, st, depth;
    if (state_out) *state_out = 0;
    if (!scope_out || len < 0) return 0;
    for (i = 0; i < len; i++) scope_out[i] = 0;
    if (!L || !L->gram || !L->gram->ok) return 0;
    if (len > UC_HL_MAXLINE) return 0;

    h.g = L->gram; h.s = line; h.len = len; h.out = scope_out; h.steps = 0;
    if (!hit_reserve(h.g->npat)) return 0;
    h.gen = ++g_hitgen;

    /* A state from another grammar - a changed language, a reloaded extension -
     * reads as "nothing open" rather than as an index into a pool it does not
     * belong to. */
    st = hl_state_valid(h.g, state_in) ? state_in : 0;

    /* how deep this line starts, so UC_GRAM_DEPTH still bounds the recursion */
    depth = 0;
    { int k = st; while (k > 0 && depth <= UC_GRAM_DEPTH) { depth++; k = g_hlstate[k].parent; } }

    /* UNWIND OUTWARDS.  The line begins inside a stack of open rules; scan the
     * innermost, and each time one closes carry on in the one that contained
     * it, from where the close left off.  The old code could only ever resume
     * one level and, when that level closed, jumped straight to the top - so a
     * comment ending inside a string put the rest of the string in plain text. */
    for (;;) {
        UcPattern *p;
        int cl = len, r;
        if (st <= 0) { open = scan(&h, h.g->top, h.g->ntop, pos, len, 0, 0, 0, &cl); break; }
        p = &h.g->pat[g_hlstate[st].pat];
        r = scan(&h, p->nsub ? p->sub : 0, p->nsub, pos, len, p, st, depth, &cl);
        if (r != -2) { open = r; break; }
        pos = cl > pos ? cl : pos + 1;
        st = g_hlstate[st].parent;
        if (depth > 0) depth--;
        if (pos >= len) { open = st; break; }
    }
    if (state_out) *state_out = open > 0 ? open : 0;
    return 1;
}
