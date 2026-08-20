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
 * THE ONE DOCUMENTED DEVIATION.  Cross-line state is ONE open begin/end rule,
 * not a stack.  Within a line, nesting is arbitrary; across a line break, only
 * the OUTERMOST rule left open is remembered.  That covers block comments and
 * multi-line strings - everything real code leaves open at a newline - and it
 * is what lets the per-line state be a single 16-bit number stored beside the
 * line index, which is what makes scrolling a 6000-line file cost nothing.
 * Anything deeper re-syncs at the next line rather than being coloured wrongly
 * forever, which is the failure mode to prefer.
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

static int gram_take(UcGrammar *g, int n)      /* n contiguous slots, -1 = full */
{
    int at = g->npat;
    if (at + n > g->pcap) return -1;
    g->npat += n;
    return at;
}

static UcRx *rx_or_null(const char *pat)
{
    char err[64];
    if (!pat || !pat[0]) return 0;
    return uc_rx_compile(pat, 0, err, sizeof err);
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
        UcPattern *p = &g->pat[base + i];
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
    }
    g->ok = 1;
    return g;
}

/* ---- grammars from JSON ---------------------------------------------------- */
typedef struct {
    UcGrammar *g;
    UcJson    *root;
    UcJson    *repo;
    int        depth;
} GramBuild;

static void build_list(GramBuild *b, UcJson *arr, int *out_first, int *out_n);

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
static int build_one(GramBuild *b, UcJson *rule, UcPattern *p)
{
    const char *inc, *m, *bg, *en, *nm;
    char err[64];
    memset(p, 0, sizeof *p);
    inc = uc_json_str(rule, "include", 0);
    if (inc) {
        if (!strcmp(inc, "$self")) { p->self = 1; return 1; }
        if (inc[0] == '#' && b->repo && b->depth < UC_GRAM_DEPTH) {
            UcJson *r = uc_json_member(b->repo, inc + 1);
            UcJson *pats;
            int first = 0, n = 0;
            if (!r) return 0;
            pats = uc_json_member(r, "patterns");
            b->depth++;
            if (pats) build_list(b, pats, &first, &n);
            else {
                /* a repository entry that is itself a single rule */
                int slot = gram_take(b->g, 1);
                if (slot >= 0 && build_one(b, r, &b->g->pat[slot])) { first = slot; n = 1; }
            }
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
    for (i = 0, e = arr->child; e && i < n; e = e->next, i++)
        build_one(b, e, &b->g->pat[base + i]);
}

static void gram_free(UcGrammar *g)
{
    int i;
    if (!g) return;
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
    b.g = g; b.root = root; b.repo = uc_json_member(root, "repository"); b.depth = 0;
    build_list(&b, uc_json_member(root, "patterns"), &first, &n);
    uc_json_free(root);
    if (n <= 0) { gram_free(g); return 0; }
    g->top = (short)first;
    g->ntop = (short)n;
    g->ok = 1;
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
} HlCtx;

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
 * enclosing begin/end rule when `encl` is non-NULL.  Returns the index of a
 * pattern left OPEN at the end of the line, or -1. */
static int scan(HlCtx *h, int first, int n, int from, int to, UcPattern *encl,
                int depth)
{
    int pos = from, open = -1;
    if (depth > UC_GRAM_DEPTH) return -1;
    while (pos < to) {
        int best = -1, best_at = to + 1, best_end = 0;
        int best_caps[UC_RX_CAPS * 2], caps[UC_RX_CAPS * 2];
        int end_at = -1, end_to = 0, i;

        best_caps[0] = -1;

        if (++h->steps > 40000) break;      /* a pathological grammar stops here */

        /* the enclosing rule's end pattern competes with its children */
        if (encl && encl->end &&
            uc_rx_exec(encl->end, h->s, to, pos, pos == 0, caps)) {
            end_at = caps[0];
            end_to = caps[1];
            if (end_to == end_at) end_to = end_at + 1;   /* never stall */
        }

        for (i = 0; i < n; i++) {
            UcPattern *p = &h->g->pat[first + i];
            UcRx *rx = p->match ? p->match : p->begin;
            if (p->self || (!rx && p->nsub > 0)) {
                /* a group / $self include: its children compete directly */
                int sf = p->self ? h->g->top : p->sub;
                int sn = p->self ? h->g->ntop : p->nsub;
                int j;
                for (j = 0; j < sn; j++) {
                    UcPattern *q = &h->g->pat[sf + j];
                    UcRx *qrx = q->match ? q->match : q->begin;
                    if (!qrx) continue;
                    if (!uc_rx_exec(qrx, h->s, to, pos, pos == 0, caps)) continue;
                    if (caps[0] < best_at) {
                        best_at = caps[0]; best = sf + j; best_end = caps[1];
                        memcpy(best_caps, caps, sizeof caps);
                    }
                }
                continue;
            }
            if (!rx) continue;
            if (!uc_rx_exec(rx, h->s, to, pos, pos == 0, caps)) continue;
            if (caps[0] < best_at) {
                best_at = caps[0]; best = first + i; best_end = caps[1];
                memcpy(best_caps, caps, sizeof caps);
            }
        }

        /* the enclosing end wins if it starts no later than the best child */
        if (end_at >= 0 && (best < 0 || end_at <= best_at)) {
            if (encl->content[0]) paint(h, pos, end_at, uc_scope_id(encl->content));
            else if (encl->name[0]) paint(h, pos, end_at, uc_scope_id(encl->name));
            if (encl->name[0]) paint(h, end_at, end_to, uc_scope_id(encl->name));
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
                int r = scan(h, p->nsub ? p->sub : 0, p->nsub, inner_from, to,
                             p, depth + 1);
                if (r == -2) {
                    /* the sub-scan consumed up to and including the end match;
                     * it does not report where, so re-find it to continue */
                    int c2[UC_RX_CAPS * 2];
                    if (uc_rx_exec(p->end, h->s, to, inner_from, 0, c2))
                        pos = c2[1] > c2[0] ? c2[1] : c2[0] + 1;
                    else pos = to;
                    continue;
                }
                open = best;
                pos = to;
                break;
            }
        }
    }
    if (pos < to && encl) {
        int sc = encl->content[0] ? uc_scope_id(encl->content)
                                  : (encl->name[0] ? uc_scope_id(encl->name) : 0);
        paint(h, pos, to, sc);
    }
    return open;
}

int uc_tokenize(int lang, const char *line, int len, int state_in,
                short *scope_out, int *state_out)
{
    HlCtx h;
    UcLang *L = uc_lang_at(lang);
    int i, open;
    if (state_out) *state_out = 0;
    if (!scope_out || len < 0) return 0;
    for (i = 0; i < len; i++) scope_out[i] = 0;
    if (!L || !L->gram || !L->gram->ok) return 0;
    if (len > UC_HL_MAXLINE) return 0;

    h.g = L->gram; h.s = line; h.len = len; h.out = scope_out; h.steps = 0;

    if (state_in > 0 && state_in <= h.g->npat) {
        /* the line opens inside a begin/end rule that started earlier */
        UcPattern *p = &h.g->pat[state_in - 1];
        int r = scan(&h, p->nsub ? p->sub : 0, p->nsub, 0, len, p, 1);
        if (r == -2) {
            int c2[UC_RX_CAPS * 2];
            int pos = 0;
            if (uc_rx_exec(p->end, line, len, 0, 1, c2)) pos = c2[1] > c2[0] ? c2[1] : c2[0] + 1;
            open = scan(&h, h.g->top, h.g->ntop, pos, len, 0, 0);
        } else {
            open = state_in - 1;                 /* still open at end of line */
        }
    } else {
        open = scan(&h, h.g->top, h.g->ntop, 0, len, 0, 0);
    }
    if (state_out) *state_out = (open >= 0) ? open + 1 : 0;
    return 1;
}
