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
 * uc_rx.c - the regular-expression engine behind grammars and Find/Replace.
 *
 * WHY THIS EXISTS.  A TextMate grammar is a tree of regexes; a syntax
 * highlighter built from hand-written scanners cannot be extended by a JSON
 * file dropped on the disk, and being extensible by a dropped file is the
 * whole point of UnoCode's grammar support.  Find & Replace gets regex mode
 * out of the same engine for nothing.
 *
 * SHAPE.  Pattern -> a small bytecode program -> an ITERATIVE backtracking VM.
 * Iterative, not recursive, on purpose: a recursive matcher's stack depth is a
 * function of the SUBJECT length (`.*` over a 4 KB line is 4 KB of frames) and
 * this runs on a kernel stack in a loadable module, where blowing the stack is
 * a triple fault rather than a segfault.  The explicit backtrack stack is
 * bounded and a step budget kills catastrophic patterns instead of hanging the
 * desktop - both are failures that must degrade to "no match", never to "the
 * machine stopped".
 *
 * Jump operands are RELATIVE, which is what makes {n,m} a memcpy of the atom's
 * program instead of a relocation pass.
 *
 * Supported: literals, `.`, `[...]` with ranges, negation, class escapes and
 * POSIX names, `\w \W \d \D \s \S \b \B \A \z \G`, `^ $`, `* + ? {n} {n,}
 * {n,m}` greedy and lazy, `|`, `(...)`, `(?:...)`, `(?<name>...)`, inline
 * `(?i)` and `(?i:...)`, backreferences `\1`-`\9`, and lookaround in all four
 * forms.
 *
 * WHY LOOKAROUND IS NOT OPTIONAL (UCD-28).  It reads like an exotic corner of
 * regex and it is not: measured over the two grammars this editor most needs to
 * load, Microsoft's own TypeScript grammar uses lookahead in 49% of its
 * patterns and lookbehind in 39%, and the C++ grammar uses negative lookahead
 * in 48%.  A rule whose regex fails to compile is silently dropped from the
 * grammar (uc_lang.c leaves the slot inert), so before this the two biggest
 * real grammars in the world loaded as a third of themselves.  Backreferences,
 * named in the same breath by the roadmap, appear in under 1% of either - they
 * are supported here because they are nearly free once captures survive
 * backtracking, not because they were holding anything up.
 *
 * LOOKAROUND IS THE ONE PLACE THIS RECURSES, and the header's no-recursion rule
 * still holds.  That rule exists because a recursive matcher's depth is a
 * function of the SUBJECT length - `.*` over a 4 KB line is 4 KB of frames -
 * and this runs on a kernel stack where that is a triple fault.  A lookaround's
 * depth is a function of PATTERN NESTING instead, which is small, fixed before
 * the match starts, and capped here at RX_MAXLOOK.  The sub-match borrows the
 * unused tail of the caller's backtrack stack rather than allocating.
 * ======================================================================== */
#include "unocode.h"

enum {
    RXI_CHAR = 0, RXI_ANY, RXI_CLASS, RXI_SPLIT, RXI_JMP, RXI_SAVE,
    RXI_MATCH, RXI_BOL, RXI_EOL, RXI_WORDB, RXI_NWORDB,
    /* UCD-28 */
    RXI_LOOK,      /* zero-width sub-match; x = past the body, ch = flags   */
    RXI_BACKREF,   /* x = group number, y = icase                           */
    RXI_GANCHOR    /* \G: where this search was told to start               */
};

/* RXI_LOOK's `ch` */
#define RXL_NEG    1
#define RXL_BEHIND 2

typedef struct {
    unsigned char op;
    unsigned char set;      /* RXI_CLASS: index into rx->sets              */
    short x, y;             /* RELATIVE targets for SPLIT/JMP; slot for SAVE.
                             * For CHAR/CLASS/BACKREF, y is the case-fold
                             * flag - it has to be PER INSTRUCTION now that
                             * (?i:...) can turn folding on for part of a
                             * pattern.  For LOOK behind, y is the widest
                             * the body can be, or -1 for "cannot tell". */
    unsigned char ch;       /* RXI_CHAR (already case-folded if icase)     */
} RxIns;

/* Raised for UCD-28, and both because a REAL grammar asked.  Microsoft's C++
 * grammar has 52 patterns over the old 1024-instruction cap and 20 over the
 * old 48-class one; TypeScript has 29 over the class cap.  These are runaway
 * guards, not budgets, and they were set where they were because nothing had
 * yet handed this engine a pattern written by a machine. */
#define RX_MAXINS   8192
/* 1024 because a real one needed 571: TypeScript's grammar contains 8 KB
 * machine-written patterns with that many bracket expressions in a single
 * regex.  A number chosen from what a person would write is the wrong number
 * here. */
#define RX_MAXSETS  1024
#define RX_MAXBT    2048
#define RX_MAXSTEPS 120000
#define RX_MAXREP   48
#define RX_MAXLOOK  8       /* nested lookarounds; a C-stack depth cap     */
/* How far back an unbounded lookbehind will look.  A bounded one is measured
 * exactly (rx_block_maxw); this is only for a body containing a loop, which no
 * published grammar's lookbehind does. */
#define RX_LOOKBACK 32

struct UcRx {
    RxIns        *ins;
    int           nins, cap;
    /* GROWN, not inline.  A fixed `sets[512][32]` would be 16 KB in every
     * compiled regex, and a grammar is thousands of them - the C++ one would
     * have wanted 50 MB of character classes that no rule uses.  Most patterns
     * use one class or none. */
    unsigned char (*sets)[32];
    int           nsets, setcap;
    int           ngroup;
    int           icase;
    /* Contains \G, whose meaning depends on WHERE the search was told to
     * start.  A caller that caches "this rule matches at X" or "this rule
     * matches nowhere" keyed only on the pattern would be wrong for these the
     * moment the start moves - see uc_lang.c's per-line match cache. */
    int           ganchored;
};

typedef struct {
    const char *p;
    UcRx       *rx;
    char       *err;
    int         errcap;
    int         failed;
    int         group;
    int         icase;      /* the CURRENT fold state; (?i) moves it       */
    int         xmode;      /* (?x): whitespace and # comments are ignored */
    int         depth;      /* open groups, so runaway nesting is caught   */
} RxC;

/* ---- small helpers -------------------------------------------------------- */
static int rx_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int rx_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static void rx_err(RxC *c, const char *m)
{
    if (c->failed) return;
    c->failed = 1;
    if (c->err && c->errcap > 0) uc_scpy(c->err, m, c->errcap);
}

static int rx_emit(RxC *c, int op, int x, int y, int ch, int set)
{
    UcRx *rx = c->rx;
    int at = rx->nins;
    if (rx->nins >= rx->cap) {
        int ncap = rx->cap ? rx->cap * 2 : 64;
        RxIns *n;
        if (ncap > RX_MAXINS) { rx_err(c, "pattern too complex"); return at; }
        n = (RxIns *)malloc((unsigned long)ncap * sizeof(RxIns));
        if (!n) { rx_err(c, "out of memory"); return at; }
        if (rx->ins) { memcpy(n, rx->ins, (unsigned long)rx->nins * sizeof(RxIns)); free(rx->ins); }
        rx->ins = n; rx->cap = ncap;
    }
    rx->ins[at].op  = (unsigned char)op;
    rx->ins[at].x   = (short)x;
    rx->ins[at].y   = (short)y;
    rx->ins[at].ch  = (unsigned char)ch;
    rx->ins[at].set = (unsigned char)set;
    rx->nins++;
    return at;
}

static int rx_isalnum_ascii(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void rx_setbit(unsigned char *s, int c) { s[(c >> 3) & 31] |= (unsigned char)(1 << (c & 7)); }
static int  rx_getbit(const unsigned char *s, int c) { return (s[(c >> 3) & 31] >> (c & 7)) & 1; }

static void rx_class_escape(unsigned char *s, int e)
{
    int i;
    switch (e) {
    case 'd': for (i = '0'; i <= '9'; i++) rx_setbit(s, i); break;
    case 'w': for (i = '0'; i <= '9'; i++) rx_setbit(s, i);
              for (i = 'a'; i <= 'z'; i++) rx_setbit(s, i);
              for (i = 'A'; i <= 'Z'; i++) rx_setbit(s, i);
              rx_setbit(s, '_'); break;
    case 's': rx_setbit(s, ' '); rx_setbit(s, '\t'); rx_setbit(s, '\n');
              rx_setbit(s, '\r'); rx_setbit(s, '\f'); rx_setbit(s, '\v'); break;
    default: break;
    }
}

static int rx_escape_char(int e)
{
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    default:  return e;
    }
}

/* ---- parser --------------------------------------------------------------- */
static void rx_alt(RxC *c);

/* EXTENDED MODE, `(?x)`.  Whitespace outside a character class is not part of
 * the pattern and `#` runs to the end of the line.
 *
 * This had to be implemented rather than accepted-and-ignored, and the
 * difference matters more than it looks.  Accepting the flag and then treating
 * the layout as literal text compiles a pattern that will never match anything
 * - a silently WRONG rule, which is worse than a dropped one, because a
 * dropped rule at least leaves the text uncoloured rather than coloured by the
 * next rule down.  Microsoft's TypeScript grammar writes 42 of its patterns
 * this way, laid out over a dozen lines each with comments in them. */
static void rx_skipx(RxC *c)
{
    if (!c->xmode) return;
    for (;;) {
        char ch = *c->p;
        if (ch == ' '  || ch == 0x09 || ch == 0x0a ||
            ch == 0x0d || ch == 0x0c || ch == 0x0b) { c->p++; continue; }
        if (ch == '#') {
            while (*c->p && *c->p != 0x0a) c->p++;
            continue;
        }
        return;
    }
}

/* Duplicate the program block [from,to) at the end - jumps are relative, so a
 * straight copy relocates itself.  This is how {n,m} is expanded. */
static int rx_dup(RxC *c, int from, int to)
{
    UcRx *rx = c->rx;
    int i, at = rx->nins;
    for (i = from; i < to && !c->failed; i++) {
        RxIns s = rx->ins[i];
        rx_emit(c, s.op, s.x, s.y, s.ch, s.set);
    }
    return at;
}

static int rx_new_set(RxC *c)
{
    UcRx *rx = c->rx;
    if (rx->nsets >= RX_MAXSETS) { rx_err(c, "too many character classes"); return 0; }
    if (rx->nsets >= rx->setcap) {
        int ncap = rx->setcap ? rx->setcap * 2 : 4;
        unsigned char (*n)[32];
        if (ncap > RX_MAXSETS) ncap = RX_MAXSETS;
        n = (unsigned char (*)[32])malloc((unsigned long)ncap * 32);
        if (!n) { rx_err(c, "out of memory"); return 0; }
        if (rx->sets) {
            memcpy(n, rx->sets, (unsigned long)rx->nsets * 32);
            free(rx->sets);
        }
        rx->sets = n;
        rx->setcap = ncap;
    }
    memset(rx->sets[rx->nsets], 0, 32);
    return rx->nsets++;
}

/* a bracketed class; `*neg` receives the negation flag via a spare bit in the
 * instruction's `ch` field */
static void rx_bracket(RxC *c, int *set_out, int *neg_out)
{
    int set = rx_new_set(c), neg = 0, first = 1;
    unsigned char *s;
    if (c->failed) { *set_out = 0; *neg_out = 0; return; }
    s = c->rx->sets[set];
    c->p++;                                   /* '[' */
    if (*c->p == '^') { neg = 1; c->p++; }
    while (*c->p && (*c->p != ']' || first)) {
        int lo;
        first = 0;
        /* POSIX names, `[[:alpha:]]`.  Rare in hand-written patterns and not
         * rare in published grammars - TypeScript's uses them in 3% of its
         * rules, and one unrecognised name costs the whole rule. */
        if (c->p[0] == '[' && c->p[1] == ':') {
            static const struct { const char *name; const char *set; } kPosix[] = {
                { "alpha:]",  "azAZ" },  { "digit:]",  "09" },
                { "alnum:]",  "azAZ09" },{ "upper:]",  "AZ" },
                { "lower:]",  "az" },    { "xdigit:]", "09afAF" },
                { "space:]",  "" },      { "word:]",   "azAZ09" },
                { "punct:]",  "" },      { "cntrl:]",  "" },
                { "print:]",  "" },      { "graph:]",  "" },
                { "blank:]",  "" },      { 0, 0 }
            };
            int k;
            for (k = 0; kPosix[k].name; k++) {
                unsigned long n = strlen(kPosix[k].name);
                if (strncmp(c->p + 2, kPosix[k].name, n)) continue;
                {
                    const char *r = kPosix[k].set;
                    int i;
                    while (r[0] && r[1]) {
                        for (i = (unsigned char)r[0]; i <= (unsigned char)r[1]; i++)
                            rx_setbit(s, i);
                        r += 2;
                    }
                    if (!strncmp(kPosix[k].name, "word", 4)) rx_setbit(s, '_');
                    if (!strncmp(kPosix[k].name, "space", 5)) rx_class_escape(s, 's');
                    if (!strncmp(kPosix[k].name, "blank", 5)) { rx_setbit(s, ' '); rx_setbit(s, '\t'); }
                    if (!strncmp(kPosix[k].name, "cntrl", 5))
                        for (i = 0; i < 32; i++) rx_setbit(s, i);
                    if (!strncmp(kPosix[k].name, "print", 5))
                        for (i = 32; i < 127; i++) rx_setbit(s, i);
                    if (!strncmp(kPosix[k].name, "graph", 5))
                        for (i = 33; i < 127; i++) rx_setbit(s, i);
                    if (!strncmp(kPosix[k].name, "punct", 5))
                        for (i = 33; i < 127; i++)
                            if (!rx_isalnum_ascii(i)) rx_setbit(s, i);
                }
                c->p += 2 + (int)strlen(kPosix[k].name);
                break;
            }
            if (kPosix[k].name) continue;
            /* an unknown [: name - fall through and treat '[' as a literal */
        }
        if (*c->p == '\\' && c->p[1]) {
            int e = c->p[1];
            c->p += 2;
            if (e == 'd' || e == 'w' || e == 's') { rx_class_escape(s, e); continue; }
            if (e == 'D' || e == 'W' || e == 'S') {
                unsigned char t[32]; int i;
                memset(t, 0, 32);
                rx_class_escape(t, rx_lower(e));
                for (i = 0; i < 256; i++) if (!rx_getbit(t, i)) rx_setbit(s, i);
                continue;
            }
            lo = rx_escape_char(e);
        } else {
            lo = (unsigned char)*c->p++;
        }
        if (c->p[0] == '-' && c->p[1] && c->p[1] != ']') {
            int hi;
            c->p++;
            if (*c->p == '\\' && c->p[1]) { hi = rx_escape_char(c->p[1]); c->p += 2; }
            else hi = (unsigned char)*c->p++;
            if (hi < lo) { int t = lo; lo = hi; hi = t; }
            while (lo <= hi) {
                rx_setbit(s, lo);
                if (c->icase) { rx_setbit(s, rx_lower(lo)); rx_setbit(s, rx_upper(lo)); }
                lo++;
            }
            continue;
        }
        rx_setbit(s, lo);
        if (c->icase) { rx_setbit(s, rx_lower(lo)); rx_setbit(s, rx_upper(lo)); }
    }
    if (*c->p == ']') c->p++;
    else rx_err(c, "unterminated character class");
    *set_out = set;
    *neg_out = neg;
}

/* The widest string the program block [a,b) can consume, or -1 when that
 * cannot be bounded (UCD-28).
 *
 * A block with no BACKWARD jump is a DAG: every instruction can run at most
 * once, so the count of consuming instructions is an exact upper bound, however
 * much alternation there is.  One backward jump means a loop and the answer
 * becomes "cannot tell", which is the honest result - and never happens in
 * practice, because a lookbehind with a `*` in it is not something any
 * published grammar writes. */
static int rx_block_maxw(UcRx *rx, int a, int b)
{
    int i, w = 0;
    for (i = a; i < b; i++) {
        RxIns *in = &rx->ins[i];
        if (in->op == RXI_SPLIT && (in->x < 0 || in->y < 0)) return -1;
        if (in->op == RXI_JMP && in->x < 0) return -1;
        if (in->op == RXI_BACKREF) return -1;       /* width is runtime data */
        if (in->op == RXI_CHAR || in->op == RXI_ANY || in->op == RXI_CLASS) w++;
    }
    return w;
}

/* `(?i)`, `(?i:`, `(?-i)`, `(?im-sx:` ... - parse the letters, apply what we
 * have.  Only `i` means anything here; the rest are accepted and ignored
 * rather than refused, because refusing turns one flag nobody needs into a
 * dropped grammar rule. */
/* `(?imsx-imsx)` or `(?imsx-imsx:`.
 *
 * STRICT about what it will consume, and that is the point.  A loop that ate
 * everything up to `)` or `:` turned `(?>abc)` - an atomic group - into a flag
 * group whose "flags" were `>abc`, and then compiled the whole thing to
 * nothing.  `(?>abc)d` matched "d".  A parser that swallows a construct it does
 * not know is worse than one that refuses it, because the refusal is visible
 * and the swallowing is not.  Returns 0 if it met something that is not a
 * flag. */
static int rx_flags(RxC *c, int *saved_icase, int *saved_x)
{
    int on = 1;
    *saved_icase = c->icase;
    *saved_x = c->xmode;
    while (*c->p && *c->p != ')' && *c->p != ':') {
        char f = *c->p;
        if (f == '-') on = 0;
        else if (f == 'i') c->icase = on;
        else if (f == 'x') c->xmode = on;
        /* m, s, u, a, d: accepted and ignored.  `m` and `s` describe how `.`
         * and `^`/`$` treat a newline, and this engine is already multiline
         * and already excludes `
` from `.` - the flags ask for behaviour it
         * has, or for behaviour no grammar depends on. */
        else if (f != 'm' && f != 's' && f != 'u' && f != 'a' && f != 'd') return 0;
        c->p++;
    }
    return 1;
}

/* one atom, emitted at the current end of the program.  Returns its start. */
static int rx_atom(RxC *c)
{
    int at, ch;
    rx_skipx(c);
    at = c->rx->nins;
    ch = *c->p;
    if (!ch) return at;

    if (ch == '(') {
        int cap = -1, look = 0, behind = 0, neg = 0;
        int restore_i = -1, restore_x = -1;
        c->p++;
        if (c->p[0] == '?') {
            if (c->p[1] == ':') c->p += 2;
            else if (c->p[1] == '=' || c->p[1] == '!') {
                look = 1;
                neg = (c->p[1] == '!');
                c->p += 2;
            } else if (c->p[1] == '<' && (c->p[2] == '=' || c->p[2] == '!')) {
                look = 1;
                behind = 1;
                neg = (c->p[2] == '!');
                c->p += 3;
            } else if (c->p[1] == '<' || c->p[1] == 'P') {
                /* a NAMED group, (?<name>...) or (?P<name>...).  It captures;
                 * the name is skipped, because nothing here addresses a group
                 * by name and refusing would drop the rule. */
                c->p += (c->p[1] == 'P') ? 3 : 2;
                while (*c->p && *c->p != '>') c->p++;
                if (*c->p == '>') c->p++;
                cap = ++c->group;
                if (cap >= UC_RX_CAPS) cap = -1;
            } else if (c->p[1] == '>') {
                /* An ATOMIC group, `(?>...)`.  Compiled as a plain
                 * non-capturing group: atomicity forbids backtracking back
                 * INTO the group, which is a performance promise and, in the
                 * rare pattern that leans on it, a narrowing one.  Dropping it
                 * can only make a pattern match in more places, never in the
                 * wrong place - and the alternative was eating the body. */
                c->p += 2;
            } else if (c->p[1] == '#') {          /* a comment group */
                while (*c->p && *c->p != ')') c->p++;
                if (*c->p) c->p++;
                return at;
            } else {
                int si, sx;
                c->p++;                            /* past '?' */
                if (!rx_flags(c, &si, &sx)) {
                    c->icase = si; c->xmode = sx;
                    rx_err(c, "unsupported group type");
                    return at;
                }
                if (*c->p == ':') { c->p++; restore_i = si; restore_x = sx; }
                else {
                    /* `(?i)` with no body: the flag runs to the end of the
                     * enclosing group, which the caller's save/restore around
                     * this group's rx_alt() already gives us.  At the top
                     * level there is no group, so it runs to the end of the
                     * pattern - which is exactly what `(?x)` at the start of a
                     * multi-line grammar pattern means. */
                    if (*c->p == ')') c->p++;
                    return at;
                }
            }
        } else {
            cap = ++c->group;
            if (cap >= UC_RX_CAPS) cap = -1;      /* beyond what we report */
        }

        if (++c->depth > 40) { rx_err(c, "pattern nested too deeply"); c->depth--; return at; }
        if (look) {
            int hdr = rx_emit(c, RXI_LOOK, 0, 0,
                              (neg ? RXL_NEG : 0) | (behind ? RXL_BEHIND : 0), 0);
            int body = c->rx->nins;
            int si = c->icase, sx = c->xmode;
            rx_alt(c);
            c->icase = si;
            c->xmode = sx;
            rx_emit(c, RXI_MATCH, 0, 0, 0, 0);     /* terminates the sub-run */
            if (!c->failed) {
                c->rx->ins[hdr].x = (short)(c->rx->nins - hdr);
                c->rx->ins[hdr].y = behind
                    ? (short)rx_block_maxw(c->rx, body, c->rx->nins - 1) : 0;
            }
        } else {
            int si = c->icase, sx = c->xmode;
            if (cap >= 0) rx_emit(c, RXI_SAVE, cap * 2, 0, 0, 0);
            rx_alt(c);
            if (cap >= 0) rx_emit(c, RXI_SAVE, cap * 2 + 1, 0, 0, 0);
            /* a flag set inside a group dies with the group */
            c->icase = (restore_i >= 0) ? restore_i : si;
            c->xmode = (restore_x >= 0) ? restore_x : sx;
        }
        c->depth--;
        if (*c->p != ')') { rx_err(c, "missing ')'"); return at; }
        c->p++;
        return at;
    }
    if (ch == '[') {
        int set, neg;
        rx_bracket(c, &set, &neg);
        rx_emit(c, RXI_CLASS, 0, c->icase, neg, set);
        return at;
    }
    if (ch == '.') { c->p++; rx_emit(c, RXI_ANY, 0, 0, 0, 0); return at; }
    if (ch == '^') { c->p++; rx_emit(c, RXI_BOL, 0, 0, 0, 0); return at; }
    if (ch == '$') { c->p++; rx_emit(c, RXI_EOL, 0, 0, 0, 0); return at; }
    if (ch == '\\') {
        int e = c->p[1];
        if (!e) { rx_err(c, "trailing backslash"); return at; }
        c->p += 2;
        if (e >= '1' && e <= '9') {
            /* A BACKREFERENCE, and only to a group that has already been
             * opened.  A forward reference is still an error: it can never
             * match anything, and accepting it would turn a typo in a grammar
             * into a rule that quietly never fires. */
            int n = e - '0';
            if (n > c->group) { rx_err(c, "backreference to a group that does not exist"); return at; }
            if (n >= UC_RX_CAPS) { rx_err(c, "backreference beyond group 9"); return at; }
            rx_emit(c, RXI_BACKREF, n, c->icase, 0, 0);
            return at;
        }
        if (e == 'b') { rx_emit(c, RXI_WORDB, 0, 0, 0, 0); return at; }
        if (e == 'B') { rx_emit(c, RXI_NWORDB, 0, 0, 0, 0); return at; }
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
            int set = rx_new_set(c);
            if (c->failed) return at;
            rx_class_escape(c->rx->sets[set], rx_lower(e));
            rx_emit(c, RXI_CLASS, 0, 0, (e >= 'A' && e <= 'Z'), set);
            return at;
        }
        if (e == 'h') {                            /* oniguruma: a hex digit */
            int set = rx_new_set(c);
            int i;
            if (c->failed) return at;
            for (i = '0'; i <= '9'; i++) rx_setbit(c->rx->sets[set], i);
            for (i = 'a'; i <= 'f'; i++) rx_setbit(c->rx->sets[set], i);
            for (i = 'A'; i <= 'F'; i++) rx_setbit(c->rx->sets[set], i);
            rx_emit(c, RXI_CLASS, 0, 0, 0, set);
            return at;
        }
        if (e == 'A') { rx_emit(c, RXI_BOL, 0, 0, 0, 0); return at; }
        if (e == 'z' || e == 'Z') { rx_emit(c, RXI_EOL, 0, 0, 0, 0); return at; }
        /* `\G` anchors at the position the SEARCH was told to start from, which
         * for a grammar is where the previous rule left off.  It is how a
         * TextMate rule says "only immediately after the last match", and
         * without it those rules match anywhere on the line. */
        if (e == 'G') {
            c->rx->ganchored = 1;
            rx_emit(c, RXI_GANCHOR, 0, 0, 0, 0);
            return at;
        }
        rx_emit(c, RXI_CHAR, 0, c->icase,
                c->icase ? rx_lower(rx_escape_char(e)) : rx_escape_char(e), 0);
        return at;
    }
    /* In extended mode a `)` or `|` can only get here if rx_seq already
     * decided it was not a terminator, which it cannot: the skip runs first.
     * Everything else is a literal. */
    c->p++;
    rx_emit(c, RXI_CHAR, 0, c->icase, c->icase ? rx_lower((unsigned char)ch)
                                               : (unsigned char)ch, 0);
    return at;
}

/* wrap the atom at [at, end) in its quantifier, if one follows */
static void rx_quant(RxC *c, int at)
{
    UcRx *rx = c->rx;
    int len = rx->nins - at, lazy = 0;
    char q;
    /* `a *` is `a*` in extended mode: the quantifier binds through layout */
    rx_skipx(c);
    q = *c->p;
    if (len <= 0) return;
    if (q != '*' && q != '+' && q != '?' && q != '{') return;
    if (q == '{') {
        /* a lone '{' that is not a valid bound is a literal brace, which is
         * what real-world grammars (and every C-ish pattern) rely on */
        const char *save = c->p;
        int lo = 0, hi = -1, digits = 0;
        c->p++;
        while (*c->p >= '0' && *c->p <= '9') { lo = lo * 10 + (*c->p++ - '0'); digits++; }
        if (!digits) { c->p = save; return; }
        if (*c->p == ',') {
            c->p++;
            if (*c->p >= '0' && *c->p <= '9') { hi = 0; while (*c->p >= '0' && *c->p <= '9') hi = hi * 10 + (*c->p++ - '0'); }
        } else hi = lo;
        if (*c->p != '}') { c->p = save; return; }
        c->p++;
        if (*c->p == '?') { c->p++; lazy = 1; }
        else if (*c->p == '+') c->p++;      /* possessive: see below */
        if (lo > RX_MAXREP) lo = RX_MAXREP;
        if (hi > RX_MAXREP) hi = RX_MAXREP;
        {
            int body_from = at, body_to = rx->nins, i;
            if (lo == 0) {
                /* drop the single copy we already emitted and rebuild */
                RxIns *tmp = (RxIns *)malloc((unsigned long)len * sizeof(RxIns));
                if (!tmp) { rx_err(c, "out of memory"); return; }
                memcpy(tmp, rx->ins + at, (unsigned long)len * sizeof(RxIns));
                rx->nins = at;
                if (hi < 0) {                        /* {0,} == * */
                    rx_emit(c, RXI_SPLIT, lazy ? len + 2 : 1, lazy ? 1 : len + 2, 0, 0);
                    for (i = 0; i < len; i++) rx_emit(c, tmp[i].op, tmp[i].x, tmp[i].y, tmp[i].ch, tmp[i].set);
                    rx_emit(c, RXI_JMP, -(len + 1), 0, 0, 0);
                } else {
                    int k;
                    for (k = 0; k < hi; k++) {
                        int remaining = (hi - k) * (len + 1);
                        rx_emit(c, RXI_SPLIT, lazy ? remaining : 1, lazy ? 1 : remaining, 0, 0);
                        for (i = 0; i < len; i++) rx_emit(c, tmp[i].op, tmp[i].x, tmp[i].y, tmp[i].ch, tmp[i].set);
                    }
                }
                free(tmp);
                return;
            }
            for (i = 1; i < lo; i++) rx_dup(c, body_from, body_to);
            if (hi < 0) {
                /* A{n,} is A{n} followed by A* - NOT n copies with the last
                 * one looping, which would silently mean A{n+1,}. */
                rx_emit(c, RXI_SPLIT, lazy ? len + 2 : 1, lazy ? 1 : len + 2, 0, 0);
                rx_dup(c, body_from, body_to);
                rx_emit(c, RXI_JMP, -(len + 1), 0, 0, 0);
            } else {
                int k;
                for (k = lo; k < hi; k++) {
                    int remaining = (hi - k) * (len + 1);
                    rx_emit(c, RXI_SPLIT, lazy ? remaining : 1, lazy ? 1 : remaining, 0, 0);
                    rx_dup(c, body_from, body_to);
                }
            }
        }
        return;
    }
    c->p++;
    if (*c->p == '?') { c->p++; lazy = 1; }
    /* A POSSESSIVE quantifier - `\s*+`, `[^*]++` - compiled as greedy.
     * Possessiveness forbids giving back what the quantifier took, which is
     * a promise about backtracking cost rather than about what matches; the
     * step budget is what actually protects this engine from a runaway
     * pattern.  Not consuming the `+` was the real hazard: it became a
     * LITERAL plus, so Microsoft's C++ grammar - which writes `\s*+`
     * throughout - had rules demanding a '+' character that is not there. */
    else if (*c->p == '+') c->p++;
    if (q == '?') {
        /* SPLIT then the atom: shift the atom up one slot */
        RxIns *tmp = (RxIns *)malloc((unsigned long)len * sizeof(RxIns));
        int i;
        if (!tmp) { rx_err(c, "out of memory"); return; }
        memcpy(tmp, rx->ins + at, (unsigned long)len * sizeof(RxIns));
        rx->nins = at;
        rx_emit(c, RXI_SPLIT, lazy ? len + 1 : 1, lazy ? 1 : len + 1, 0, 0);
        for (i = 0; i < len; i++) rx_emit(c, tmp[i].op, tmp[i].x, tmp[i].y, tmp[i].ch, tmp[i].set);
        free(tmp);
        return;
    }
    if (q == '+') {
        rx_emit(c, RXI_SPLIT, lazy ? 1 : -len, lazy ? -len : 1, 0, 0);
        return;
    }
    /* '*' */
    {
        RxIns *tmp = (RxIns *)malloc((unsigned long)len * sizeof(RxIns));
        int i;
        if (!tmp) { rx_err(c, "out of memory"); return; }
        memcpy(tmp, rx->ins + at, (unsigned long)len * sizeof(RxIns));
        rx->nins = at;
        rx_emit(c, RXI_SPLIT, lazy ? len + 2 : 1, lazy ? 1 : len + 2, 0, 0);
        for (i = 0; i < len; i++) rx_emit(c, tmp[i].op, tmp[i].x, tmp[i].y, tmp[i].ch, tmp[i].set);
        rx_emit(c, RXI_JMP, -(len + 1), 0, 0, 0);
        free(tmp);
    }
}

static void rx_seq(RxC *c)
{
    /* The skip goes HERE as well as inside rx_atom, because in extended mode
     * the thing that ends a sequence - `|`, `)`, the end of the pattern - can
     * be preceded by layout, and a loop that tested the raw character would
     * emit a literal space and then decide the sequence had not ended. */
    rx_skipx(c);
    while (*c->p && *c->p != '|' && *c->p != ')' && !c->failed) {
        int at = rx_atom(c);
        rx_quant(c, at);
        rx_skipx(c);
    }
}

/* alternation: each branch is `SPLIT here, else` ... `JMP end` */
static void rx_alt(RxC *c)
{
    int start = c->rx->nins;
    rx_seq(c);
    while (*c->p == '|' && !c->failed) {
        UcRx *rx = c->rx;
        int len = rx->nins - start, i, jmp_at;
        RxIns *tmp = (RxIns *)malloc((unsigned long)(len ? len : 1) * sizeof(RxIns));
        if (!tmp) { rx_err(c, "out of memory"); return; }
        if (len) memcpy(tmp, rx->ins + start, (unsigned long)len * sizeof(RxIns));
        rx->nins = start;
        /* SPLIT: branch A at +1, branch B after A's JMP */
        rx_emit(c, RXI_SPLIT, 1, len + 2, 0, 0);
        for (i = 0; i < len; i++) rx_emit(c, tmp[i].op, tmp[i].x, tmp[i].y, tmp[i].ch, tmp[i].set);
        free(tmp);
        jmp_at = rx_emit(c, RXI_JMP, 0, 0, 0, 0);   /* patched below */
        c->p++;                                     /* '|' */
        rx_seq(c);
        if (c->failed) return;
        rx->ins[jmp_at].x = (short)(rx->nins - jmp_at);
    }
}

/* ---- compile -------------------------------------------------------------- */
UcRx *uc_rx_compile(const char *pat, int icase, char *err, int errcap)
{
    RxC c;
    UcRx *rx;
    if (err && errcap > 0) err[0] = 0;
    if (!pat) return 0;
    rx = (UcRx *)malloc(sizeof(UcRx));
    if (!rx) return 0;
    memset(rx, 0, sizeof *rx);
    rx->icase = icase ? 1 : 0;
    memset(&c, 0, sizeof c);
    c.p = pat; c.rx = rx; c.err = err; c.errcap = errcap;
    /* the flag the caller asked for is only the STARTING state now; `(?i)`
     * moves it, and every CHAR and CLASS records the state it was emitted in */
    c.icase = rx->icase;

    rx_emit(&c, RXI_SAVE, 0, 0, 0, 0);
    rx_alt(&c);
    if (*c.p == ')') rx_err(&c, "unbalanced ')'");
    rx_emit(&c, RXI_SAVE, 1, 0, 0, 0);
    rx_emit(&c, RXI_MATCH, 0, 0, 0, 0);
    if (c.failed) { uc_rx_free(rx); return 0; }
    rx->ngroup = c.group < UC_RX_CAPS - 1 ? c.group : UC_RX_CAPS - 1;
    return rx;
}

void uc_rx_free(UcRx *rx)
{
    if (!rx) return;
    if (rx->ins) free(rx->ins);
    if (rx->sets) free(rx->sets);
    free(rx);
}

/* 1 when the pattern contains \G, so a caller that caches match positions
 * knows this one's answer depends on where the search began. */
int uc_rx_ganchored(const UcRx *rx) { return rx ? rx->ganchored : 0; }

int uc_rx_ngroups(const UcRx *rx) { return rx ? rx->ngroup : 0; }

/* ---- the VM --------------------------------------------------------------- */
typedef struct {
    int pc, sp;
    int caps[UC_RX_CAPS * 2];
} RxBt;

static int rx_isword(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Run the program from `pc0` until it reaches RXI_MATCH.
 *
 * `want_end` >= 0 means the match must finish EXACTLY there, which is what
 * turns a forward matcher into a lookbehind: the body is tried at each start
 * position going back and only an ending flush against the current point
 * counts.  Reaching RXI_MATCH somewhere else is treated as a failure and
 * backtracked out of, so alternatives of different widths all get their turn.
 *
 * `gpos` is what `\G` anchors to.  `caps` is NOT reset here - a lookaround runs
 * with the caller's captures visible, which is what makes `(?<=(\w+)\.)\1`
 * mean anything.
 *
 * `depth` counts nested lookarounds only. */
static int rx_run(UcRx *rx, const char *s, int len, int start, int bolz,
                  int gpos, int *caps, RxBt *bt, int btmax, long *budget,
                  int pc0, int want_end, int depth)
{
    int pc = pc0, sp = start, nbt = 0;
    for (;;) {
        RxIns *in;
        int ok = 1;
        if (--*budget < 0) return 0;
        in = &rx->ins[pc];
        switch (in->op) {
        case RXI_CHAR: {
            int c;
            if (sp >= len) { ok = 0; break; }
            c = (unsigned char)s[sp];
            if (in->y) c = rx_lower(c);
            if (c != in->ch) { ok = 0; break; }
            sp++; pc++;
            break;
        }
        case RXI_ANY:
            if (sp >= len || s[sp] == '\n') { ok = 0; break; }
            sp++; pc++;
            break;
        case RXI_CLASS: {
            int c, hit;
            if (sp >= len) { ok = 0; break; }
            c = (unsigned char)s[sp];
            hit = rx_getbit(rx->sets[in->set], c);
            if (in->y && !hit)
                hit = rx_getbit(rx->sets[in->set], rx_lower(c)) ||
                      rx_getbit(rx->sets[in->set], rx_upper(c));
            if (in->ch) hit = !hit;                  /* negated class */
            if (!hit) { ok = 0; break; }
            sp++; pc++;
            break;
        }
        case RXI_GANCHOR:
            if (sp != gpos) { ok = 0; break; }
            pc++;
            break;
        case RXI_BACKREF: {
            /* The text group n matched, again.  A group that never
             * participated fails rather than matching the empty string:
             * `(a)?\1` against "b" must not succeed, or every optional group
             * in a grammar becomes a wildcard. */
            int a = caps[in->x * 2], b = caps[in->x * 2 + 1], n, k;
            if (a < 0 || b < a) { ok = 0; break; }
            n = b - a;
            if (sp + n > len) { ok = 0; break; }
            for (k = 0; k < n; k++) {
                int u = (unsigned char)s[a + k], v = (unsigned char)s[sp + k];
                if (in->y) { u = rx_lower(u); v = rx_lower(v); }
                if (u != v) break;
            }
            if (k < n) { ok = 0; break; }
            sp += n; pc++;
            break;
        }
        case RXI_LOOK: {
            /* A zero-width sub-match.  On success sp does NOT move; the only
             * lasting effect is whether we continue, and (for a positive
             * lookaround) any groups the body captured. */
            int neg = (in->ch & RXL_NEG) != 0;
            int behind = (in->ch & RXL_BEHIND) != 0;
            int body = pc + 1, hit = 0;
            int save[UC_RX_CAPS * 2];
            memcpy(save, caps, sizeof save);
            if (depth >= RX_MAXLOOK) {
                /* Out of C stack budget.  Fail the lookaround rather than
                 * ignore it: ignoring turns `(?!x)` into "always", which is a
                 * silently WRONG match, and a missing match is merely a
                 * missing one. */
                ok = 0;
                break;
            }
            if (!behind) {
                hit = rx_run(rx, s, len, sp, bolz, gpos, caps, bt + nbt,
                             btmax - nbt, budget, body, -1, depth + 1);
            } else {
                int maxw = in->y >= 0 ? in->y : RX_LOOKBACK;
                int w;
                if (maxw > RX_LOOKBACK) maxw = RX_LOOKBACK;
                for (w = 0; w <= maxw && w <= sp; w++) {
                    memcpy(caps, save, sizeof save);
                    if (rx_run(rx, s, len, sp - w, bolz, gpos, caps, bt + nbt,
                               btmax - nbt, budget, body, sp, depth + 1)) {
                        hit = 1;
                        break;
                    }
                }
            }
            if (hit == neg) {
                memcpy(caps, save, sizeof save);
                ok = 0;
                break;
            }
            /* A NEGATIVE lookaround leaves no captures behind: it succeeded by
             * NOT matching, so anything its body happened to capture on the
             * way to failing is not something the pattern saw. */
            if (neg) memcpy(caps, save, sizeof save);
            pc += in->x;
            break;
        }
        case RXI_BOL:
            /* `bolz` is about INDEX 0 OF THE SUBJECT, not about wherever this
             * attempt started.  It used to be the latter, which was harmless
             * only because the flag can never be consulted anywhere but index
             * 0 when matching FORWARDS - and stopped being harmless the moment
             * a lookbehind began a sub-match at a lower position than its
             * caller.  `(?<=^a)b` could not match "ab": the sub-run was told
             * index 0 was not a line start, because the value it was handed
             * described index 1. */
            if (!(sp == 0 ? bolz : s[sp-1] == '\n')) { ok = 0; break; }
            pc++;
            break;
        case RXI_EOL:
            if (!(sp == len || s[sp] == '\n')) { ok = 0; break; }
            pc++;
            break;
        case RXI_WORDB:
        case RXI_NWORDB: {
            int a = sp > 0 ? rx_isword((unsigned char)s[sp-1]) : 0;
            int b = sp < len ? rx_isword((unsigned char)s[sp]) : 0;
            int at_boundary = (a != b);
            if ((in->op == RXI_WORDB) != at_boundary) { ok = 0; break; }
            pc++;
            break;
        }
        case RXI_SAVE:
            if (in->x < UC_RX_CAPS * 2) {
                /* the old value has to be restored on backtrack, and the
                 * backtrack entries carry a full capture snapshot, so simply
                 * writing here is correct */
                caps[in->x] = sp;
            }
            pc++;
            break;
        case RXI_SPLIT:
            if (nbt < btmax) {
                bt[nbt].pc = pc + in->y;
                bt[nbt].sp = sp;
                memcpy(bt[nbt].caps, caps, sizeof bt[nbt].caps);
                nbt++;
            }
            pc += in->x;
            break;
        case RXI_JMP:
            pc += in->x;
            break;
        case RXI_MATCH:
            /* A lookbehind demands an exact landing.  Anything else here is a
             * near miss, and backtracking may still find the right width. */
            if (want_end >= 0 && sp != want_end) { ok = 0; break; }
            return 1;
        default:
            ok = 0;
            break;
        }
        if (!ok) {
            if (nbt == 0) return 0;
            nbt--;
            pc = bt[nbt].pc;
            sp = bt[nbt].sp;
            memcpy(caps, bt[nbt].caps, sizeof bt[nbt].caps);
        }
    }
}

/* Try to match starting exactly at `start`.  Returns 1 + fills caps. */
static int rx_match_at(UcRx *rx, const char *s, int len, int start, int bolz,
                       int gpos, int *caps, RxBt *bt, int btmax, long *budget)
{
    int i;
    for (i = 0; i < UC_RX_CAPS * 2; i++) caps[i] = -1;
    return rx_run(rx, s, len, start, bolz, gpos, caps, bt, btmax, budget, 0, -1, 0);
}

int uc_rx_exec(UcRx *rx, const char *s, int len, int from, int bol, int *caps)
{
    static RxBt *bt;
    long budget = RX_MAXSTEPS;
    int at, bolz;
    if (!rx || !s || from < 0 || from > len) return 0;
    if (!bt) {
        bt = (RxBt *)malloc((unsigned long)RX_MAXBT * sizeof(RxBt));
        if (!bt) return 0;
    }
    /* ONE flag for the whole scan, describing INDEX 0.  The per-start-position
     * computation this replaces was dead arithmetic: `^` is only ever tested
     * against the flag when the position IS index 0, so every other value it
     * produced was discarded.  Saying so plainly is also what lets a lookbehind
     * sub-match, which starts at a LOWER position than its caller, get the
     * right answer - see RXI_BOL. */
    bolz = (from == 0) ? (bol != 0) : 1;
    for (at = from; at <= len; at++) {
        if (rx_match_at(rx, s, len, at, bolz, from, caps, bt, RX_MAXBT, &budget))
            return 1;
        if (budget < 0) return 0;
    }
    return 0;
}
