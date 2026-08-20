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
 * Supported: literals, `.`, `[...]` with ranges, negation and class escapes,
 * `\w \W \d \D \s \S \b \B`, `^ $`, `* + ? {n} {n,} {n,m}` greedy and lazy,
 * `|`, `(...)`, `(?:...)`.  Rejected at COMPILE time (never silently
 * mis-matched): backreferences and lookaround.
 * ======================================================================== */
#include "unocode.h"

enum {
    RXI_CHAR = 0, RXI_ANY, RXI_CLASS, RXI_SPLIT, RXI_JMP, RXI_SAVE,
    RXI_MATCH, RXI_BOL, RXI_EOL, RXI_WORDB, RXI_NWORDB
};

typedef struct {
    unsigned char op;
    unsigned char set;      /* RXI_CLASS: index into rx->sets              */
    short x, y;             /* RELATIVE targets for SPLIT/JMP; slot for SAVE */
    unsigned char ch;       /* RXI_CHAR (already case-folded if icase)     */
} RxIns;

#define RX_MAXINS   1024
#define RX_MAXSETS  48
#define RX_MAXBT    2048
#define RX_MAXSTEPS 120000
#define RX_MAXREP   48

struct UcRx {
    RxIns        *ins;
    int           nins, cap;
    unsigned char sets[RX_MAXSETS][32];
    int           nsets;
    int           ngroup;
    int           icase;
};

typedef struct {
    const char *p;
    UcRx       *rx;
    char       *err;
    int         errcap;
    int         failed;
    int         group;
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
                if (c->rx->icase) { rx_setbit(s, rx_lower(lo)); rx_setbit(s, rx_upper(lo)); }
                lo++;
            }
            continue;
        }
        rx_setbit(s, lo);
        if (c->rx->icase) { rx_setbit(s, rx_lower(lo)); rx_setbit(s, rx_upper(lo)); }
    }
    if (*c->p == ']') c->p++;
    else rx_err(c, "unterminated character class");
    *set_out = set;
    *neg_out = neg;
}

/* one atom, emitted at the current end of the program.  Returns its start. */
static int rx_atom(RxC *c)
{
    int at = c->rx->nins;
    char ch = *c->p;

    if (ch == '(') {
        int cap = -1;
        c->p++;
        if (c->p[0] == '?') {
            if (c->p[1] == ':') c->p += 2;
            else if (c->p[1] == '=' || c->p[1] == '!' || c->p[1] == '<') {
                rx_err(c, "lookaround is not supported");
                return at;
            } else if (c->p[1] == '#') {          /* a comment group */
                while (*c->p && *c->p != ')') c->p++;
                if (*c->p) c->p++;
                return at;
            } else c->p += 1;                     /* (?i) and friends: ignore */
        } else {
            cap = ++c->group;
            if (cap >= UC_RX_CAPS) cap = -1;      /* beyond what we report */
        }
        if (cap >= 0) rx_emit(c, RXI_SAVE, cap * 2, 0, 0, 0);
        rx_alt(c);
        if (cap >= 0) rx_emit(c, RXI_SAVE, cap * 2 + 1, 0, 0, 0);
        if (*c->p != ')') { rx_err(c, "missing ')'"); return at; }
        c->p++;
        return at;
    }
    if (ch == '[') {
        int set, neg;
        rx_bracket(c, &set, &neg);
        rx_emit(c, RXI_CLASS, 0, 0, neg, set);
        return at;
    }
    if (ch == '.') { c->p++; rx_emit(c, RXI_ANY, 0, 0, 0, 0); return at; }
    if (ch == '^') { c->p++; rx_emit(c, RXI_BOL, 0, 0, 0, 0); return at; }
    if (ch == '$') { c->p++; rx_emit(c, RXI_EOL, 0, 0, 0, 0); return at; }
    if (ch == '\\') {
        int e = c->p[1];
        if (!e) { rx_err(c, "trailing backslash"); return at; }
        c->p += 2;
        if (e >= '1' && e <= '9') { rx_err(c, "backreferences are not supported"); return at; }
        if (e == 'b') { rx_emit(c, RXI_WORDB, 0, 0, 0, 0); return at; }
        if (e == 'B') { rx_emit(c, RXI_NWORDB, 0, 0, 0, 0); return at; }
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
            int set = rx_new_set(c);
            if (c->failed) return at;
            rx_class_escape(c->rx->sets[set], rx_lower(e));
            rx_emit(c, RXI_CLASS, 0, 0, (e >= 'A' && e <= 'Z'), set);
            return at;
        }
        if (e == 'A') { rx_emit(c, RXI_BOL, 0, 0, 0, 0); return at; }
        if (e == 'z' || e == 'Z') { rx_emit(c, RXI_EOL, 0, 0, 0, 0); return at; }
        rx_emit(c, RXI_CHAR, 0, 0,
                c->rx->icase ? rx_lower(rx_escape_char(e)) : rx_escape_char(e), 0);
        return at;
    }
    c->p++;
    rx_emit(c, RXI_CHAR, 0, 0, c->rx->icase ? rx_lower((unsigned char)ch)
                                            : (unsigned char)ch, 0);
    return at;
}

/* wrap the atom at [at, end) in its quantifier, if one follows */
static void rx_quant(RxC *c, int at)
{
    UcRx *rx = c->rx;
    int len = rx->nins - at, lazy = 0;
    char q = *c->p;
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
    while (*c->p && *c->p != '|' && *c->p != ')' && !c->failed) {
        int at = rx_atom(c);
        rx_quant(c, at);
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
    free(rx);
}

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

/* Try to match starting exactly at `start`.  Returns 1 + fills caps. */
static int rx_match_at(UcRx *rx, const char *s, int len, int start, int bol0,
                       int *caps, RxBt *bt, int btmax, long *budget)
{
    int pc = 0, sp = start, nbt = 0, i;
    for (i = 0; i < UC_RX_CAPS * 2; i++) caps[i] = -1;
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
            if (rx->icase) c = rx_lower(c);
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
            if (rx->icase && !hit)
                hit = rx_getbit(rx->sets[in->set], rx_lower(c)) ||
                      rx_getbit(rx->sets[in->set], rx_upper(c));
            if (in->ch) hit = !hit;                  /* negated class */
            if (!hit) { ok = 0; break; }
            sp++; pc++;
            break;
        }
        case RXI_BOL:
            if (!((sp == 0 && bol0) || (sp > 0 && s[sp-1] == '\n'))) { ok = 0; break; }
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

int uc_rx_exec(UcRx *rx, const char *s, int len, int from, int bol, int *caps)
{
    static RxBt *bt;
    long budget = RX_MAXSTEPS;
    int at;
    if (!rx || !s || from < 0 || from > len) return 0;
    if (!bt) {
        bt = (RxBt *)malloc((unsigned long)RX_MAXBT * sizeof(RxBt));
        if (!bt) return 0;
    }
    for (at = from; at <= len; at++) {
        if (rx_match_at(rx, s, len, at, at == from ? bol : (at > 0 && s[at-1] == '\n'),
                        caps, bt, RX_MAXBT, &budget))
            return 1;
        if (budget < 0) return 0;
    }
    return 0;
}
