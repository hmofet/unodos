/* ===========================================================================
 * ud_ptgc.c - the other direction: "=SUM(A1:A9)" into a BIFF8 ptg array.
 *
 * A recursive-descent parser that emits postfix directly - no intermediate
 * tree, because RPN is what the file wants and the recursion already encodes
 * the shape.  Each level of the grammar is one precedence rung, and an
 * operator's token is written AFTER its operands have written theirs.
 *
 * OPERAND CLASSES are the subtle half of writing formulas, and the plan says
 * so.  Every reference-ish token exists in three flavours - reference
 * (base+0x20), value (base+0x40) and array (base+0x60) - and Excel chooses
 * between them by how the operand is CONSUMED, not by what it is.  The rule
 * implemented here: a reference used as a direct function argument goes out
 * in reference class, because that is what lets SUM see a range rather than
 * a single dereferenced value; everywhere else - arithmetic, comparison, the
 * whole formula - it goes out in value class.  That covers everything this
 * build can construct, and is checked by writing files and reading them back
 * through LibreOffice.  It is NOT checked against real Excel, which the plan
 * reserves for a milestone with a VM; if a formula ever comes back wrong
 * there, this rule is the first place to look.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_xls_int.h"
#include <string.h>

#define CLS_REF   0x20
#define CLS_VAL   0x40

typedef struct {
    const char *s;
    long        at;
    int         bad;
    const ud_ptgc_env *env;
    unsigned char *out;
    long        n, cap;
} cstate;

/* ---- output ---------------------------------------------------------------- */
static int cgrow(cstate *c, long need)
{
    unsigned char *p;
    long cap = c->cap ? c->cap : 64;
    if (c->bad) return 0;
    if (c->n + need <= c->cap) return 1;
    while (cap < c->n + need) cap *= 2;
    p = (unsigned char *)ud_alloc((unsigned long)cap);
    if (!p) { c->bad = 1; return 0; }
    if (c->n) memcpy(p, c->out, (unsigned long)c->n);
    ud_free(c->out);
    c->out = p; c->cap = cap;
    return 1;
}
static void e8 (cstate *c, unsigned v) { if (cgrow(c, 1)) c->out[c->n++] = (unsigned char)v; }
static void e16(cstate *c, unsigned v)
{ if (cgrow(c, 2)) { ud_wr16(c->out + c->n, (uint16_t)v); c->n += 2; } }
static void edbl(cstate *c, double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    if (cgrow(c, 8)) { ud_wr64(c->out + c->n, b); c->n += 8; }
}

static void fail(cstate *c, const char *why)
{ if (!c->bad) { c->bad = 1; ud_set_error(why); } }

/* ---- lexing ---------------------------------------------------------------- */
static void skip_ws(cstate *c)
{ while (c->s[c->at] == ' ' || c->s[c->at] == '\t') c->at++; }

static int at(cstate *c, const char *lit)
{
    long n = (long)strlen(lit);
    skip_ws(c);
    return strncmp(c->s + c->at, lit, (unsigned long)n) == 0;
}
static int eat(cstate *c, const char *lit)
{
    if (!at(c, lit)) return 0;
    c->at += (long)strlen(lit);
    return 1;
}
static int is_alpha(char ch)
{ return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'; }
static int is_digit(char ch) { return ch >= '0' && ch <= '9'; }
static char upper(char ch) { return (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch; }

/* ---- references ------------------------------------------------------------
 * [$]COL[$]ROW, columns A..IV.  Returns 1 and consumes on success; on failure
 * the position is restored, because "A1" and a defined name called "A1B" are
 * only distinguishable by trying. */
static int parse_a1(cstate *c, int *row, int *col, int *rowabs, int *colabs)
{
    long save = c->at;
    int co = 0, ndig = 0, nlet = 0;
    long r = 0;

    skip_ws(c);
    *colabs = 0; *rowabs = 0;
    if (c->s[c->at] == '$') { *colabs = 1; c->at++; }
    while (is_alpha(c->s[c->at]) && c->s[c->at] != '_' && nlet < 2) {
        co = co * 26 + (upper(c->s[c->at]) - 'A' + 1);
        c->at++; nlet++;
    }
    if (!nlet) { c->at = save; return 0; }
    if (c->s[c->at] == '$') { *rowabs = 1; c->at++; }
    while (is_digit(c->s[c->at]) && ndig < 6) {
        r = r * 10 + (c->s[c->at] - '0');
        c->at++; ndig++;
    }
    if (!ndig || r < 1) { c->at = save; return 0; }
    /* a trailing letter or digit means this was an identifier, not a
       reference: "A1B" is a name, "SUM" is a function */
    if (is_alpha(c->s[c->at]) || is_digit(c->s[c->at])) { c->at = save; return 0; }
    if (co < 1 || co > UD_XLS_MAXCOL || r > UD_XLS_MAXROW) { c->at = save; return 0; }
    *col = co - 1;
    *row = (int)r - 1;
    return 1;
}

static unsigned grbit_of(int col, int colabs, int rowabs)
{
    unsigned g = (unsigned)col & 0x3FFF;
    if (!colabs) g |= 0x4000;
    if (!rowabs) g |= 0x8000;
    return g;
}

/* An identifier: a function name, a defined name, or a sheet name. */
static int parse_ident(cstate *c, char *out, int cap)
{
    int n = 0;
    skip_ws(c);
    if (!is_alpha(c->s[c->at])) return 0;
    while ((is_alpha(c->s[c->at]) || is_digit(c->s[c->at]) ||
            c->s[c->at] == '.') && n < cap - 1)
        out[n++] = c->s[c->at++];
    out[n] = 0;
    return n;
}

/* 'Some sheet' - single-quoted, with '' for a literal quote */
static int parse_quoted(cstate *c, char *out, int cap)
{
    int n = 0;
    skip_ws(c);
    if (c->s[c->at] != '\'') return 0;
    c->at++;
    while (c->s[c->at]) {
        if (c->s[c->at] == '\'') {
            if (c->s[c->at + 1] == '\'') { if (n < cap - 1) out[n++] = '\''; c->at += 2; continue; }
            c->at++;
            out[n] = 0;
            return 1;
        }
        if (n < cap - 1) out[n++] = c->s[c->at];
        c->at++;
    }
    return 0;
}

static void expr(cstate *c, int want_ref);

/* Emit a reference or area, optionally sheet-qualified.  Returns 1 if one was
 * found and emitted. */
static int emit_ref(cstate *c, int cls)
{
    long save = c->at;
    char sheet[64];
    int have_sheet = 0, ixti = -1;
    int r0, c0, ra0, ca0, r1, c1, ra1, ca1;

    skip_ws(c);
    /* an optional Sheet! or 'Sheet name'! prefix */
    if (c->s[c->at] == '\'') {
        if (parse_quoted(c, sheet, sizeof sheet) && eat(c, "!")) have_sheet = 1;
        else { c->at = save; return 0; }
    } else {
        long s2 = c->at;
        if (parse_ident(c, sheet, sizeof sheet) && at(c, "!")) { eat(c, "!"); have_sheet = 1; }
        else c->at = s2;
    }
    if (have_sheet) {
        ixti = c->env && c->env->sheet_index
               ? c->env->sheet_index(c->env->book, sheet) : -1;
        if (ixti < 0) { fail(c, "formula: no such sheet in this workbook"); return 0; }
    }
    if (!parse_a1(c, &r0, &c0, &ra0, &ca0)) { c->at = save; return 0; }

    if (at(c, ":")) {
        long s3 = c->at;
        eat(c, ":");
        if (!parse_a1(c, &r1, &c1, &ra1, &ca1)) { c->at = s3; }
        else {
            if (r1 < r0) { int t = r0; r0 = r1; r1 = t; }
            if (c1 < c0) { int t = c0; c0 = c1; c1 = t; }
            if (have_sheet) {                       /* PtgArea3d */
                e8(c, 0x1B | cls);
                e16(c, (unsigned)ixti);
            } else {                                /* PtgArea   */
                e8(c, 0x05 | cls);
            }
            e16(c, (unsigned)r0); e16(c, (unsigned)r1);
            e16(c, grbit_of(c0, ca0, ra0));
            e16(c, grbit_of(c1, ca1, ra1));
            return 1;
        }
    }
    if (have_sheet) {                               /* PtgRef3d */
        e8(c, 0x1A | cls);
        e16(c, (unsigned)ixti);
    } else {                                        /* PtgRef   */
        e8(c, 0x04 | cls);
    }
    e16(c, (unsigned)r0);
    e16(c, grbit_of(c0, ca0, ra0));
    return 1;
}

/* ---- the function table lookup --------------------------------------------
 * ud_ptg.c owns the table; this asks it by name so there is one copy. */
int  ud_ftab_find(const char *name, int *args);   /* in ud_ptg.c */

static void parse_call(cstate *c, const char *name)
{
    int nargs = 0, ift, arity;

    ift = ud_ftab_find(name, &arity);
    if (ift < 0) { fail(c, "formula: unknown function"); return; }
    if (!eat(c, "(")) { fail(c, "formula: expected ( after a function name"); return; }
    if (!at(c, ")")) {
        for (;;) {
            /* a direct argument is where a reference must stay a reference */
            expr(c, 1);
            nargs++;
            if (c->bad) return;
            if (eat(c, ",")) continue;
            break;
        }
    }
    if (!eat(c, ")")) { fail(c, "formula: expected ) closing a function call"); return; }
    if (nargs > 127) { fail(c, "formula: too many arguments"); return; }
    if (arity >= 0) {
        if (nargs != arity) { fail(c, "formula: wrong argument count for this function"); return; }
        e8(c, 0x01 | CLS_VAL);                      /* PtgFunc  */
        e16(c, (unsigned)ift);
    } else {
        e8(c, 0x02 | CLS_VAL);                      /* PtgFuncVar */
        e8(c, (unsigned)nargs);
        e16(c, (unsigned)ift);
    }
}

/* ---- literals --------------------------------------------------------------- */
static int parse_number(cstate *c)
{
    long save = c->at;
    double v = 0, frac = 0.1;
    int any = 0, neg_exp = 0, e = 0, has_exp = 0, has_dot = 0;

    skip_ws(c);
    while (is_digit(c->s[c->at])) { v = v * 10 + (c->s[c->at] - '0'); c->at++; any = 1; }
    if (c->s[c->at] == '.') {
        c->at++; has_dot = 1;
        while (is_digit(c->s[c->at])) { v += (c->s[c->at] - '0') * frac; frac /= 10; c->at++; any = 1; }
    }
    if (!any) { c->at = save; return 0; }
    if (c->s[c->at] == 'e' || c->s[c->at] == 'E') {
        long s2 = c->at;
        c->at++;
        if (c->s[c->at] == '+') c->at++;
        else if (c->s[c->at] == '-') { neg_exp = 1; c->at++; }
        if (!is_digit(c->s[c->at])) c->at = s2;
        else {
            while (is_digit(c->s[c->at])) { e = e * 10 + (c->s[c->at] - '0'); c->at++; }
            has_exp = 1;
        }
    }
    if (has_exp) {
        int i;
        for (i = 0; i < e; i++) { if (neg_exp) v /= 10.0; else v *= 10.0; }
    }
    /* PtgInt carries an unsigned 16-bit whole number and is what Excel emits
       for one; everything else is a full double */
    if (!has_dot && !has_exp && v >= 0 && v <= 65535 && (double)(long)v == v) {
        e8(c, 0x1E);
        e16(c, (unsigned)(long)v);
    } else {
        e8(c, 0x1F);
        edbl(c, v);
    }
    return 1;
}

static int parse_string(cstate *c)
{
    char buf[512];
    int n = 0;
    skip_ws(c);
    if (c->s[c->at] != '"') return 0;
    c->at++;
    while (c->s[c->at]) {
        if (c->s[c->at] == '"') {
            if (c->s[c->at + 1] == '"') { if (n < (int)sizeof buf - 1) buf[n++] = '"'; c->at += 2; continue; }
            c->at++;
            {
                int wide = 0, i;
                for (i = 0; i < n; i++)
                    if (ud_cp1252_to_uc((unsigned char)buf[i]) > 0xFF) wide = 1;
                e8(c, 0x17);
                e8(c, (unsigned)n);
                e8(c, wide ? 1 : 0);
                for (i = 0; i < n; i++) {
                    uint16_t u = ud_cp1252_to_uc((unsigned char)buf[i]);
                    if (wide) e16(c, u); else e8(c, u & 0xFF);
                }
            }
            return 1;
        }
        if (n < (int)sizeof buf - 1) buf[n++] = c->s[c->at];
        c->at++;
    }
    fail(c, "formula: unterminated string literal");
    return 1;
}

static const struct { const char *t; int v; } ERRS[] = {
    { "#NULL!", UD_XE_NULL }, { "#DIV/0!", UD_XE_DIV0 }, { "#VALUE!", UD_XE_VALUE },
    { "#REF!", UD_XE_REF }, { "#NAME?", UD_XE_NAME }, { "#NUM!", UD_XE_NUM },
    { "#N/A", UD_XE_NA }
};

static int parse_error(cstate *c)
{
    int i;
    skip_ws(c);
    if (c->s[c->at] != '#') return 0;
    for (i = 0; i < (int)(sizeof ERRS / sizeof ERRS[0]); i++)
        if (eat(c, ERRS[i].t)) { e8(c, 0x1C); e8(c, (unsigned)ERRS[i].v); return 1; }
    fail(c, "formula: unrecognised error literal");
    return 1;
}

/* ---- the grammar, one function per precedence rung -------------------------- */
static void primary(cstate *c, int want_ref)
{
    char id[64];
    long save;

    skip_ws(c);
    if (c->bad) return;
    if (eat(c, "(")) {
        expr(c, 0);
        if (!eat(c, ")")) { fail(c, "formula: expected )"); return; }
        e8(c, 0x15);                                /* PtgParen: the user's */
        return;
    }
    if (c->s[c->at] == '"') { parse_string(c); return; }
    if (c->s[c->at] == '#') { parse_error(c); return; }
    if (is_digit(c->s[c->at]) ||
        (c->s[c->at] == '.' && is_digit(c->s[c->at + 1]))) {
        if (parse_number(c)) return;
    }
    /* a reference beats a name, and a name beats nothing */
    if (emit_ref(c, want_ref ? CLS_REF : CLS_VAL)) return;
    if (c->bad) return;

    save = c->at;
    if (parse_ident(c, id, sizeof id)) {
        if (at(c, "(")) { parse_call(c, id); return; }
        if (strcmp(id, "TRUE") == 0)  { e8(c, 0x1D); e8(c, 1); return; }
        if (strcmp(id, "FALSE") == 0) { e8(c, 0x1D); e8(c, 0); return; }
        {
            int idx = c->env && c->env->name_index
                      ? c->env->name_index(c->env->book, id) : 0;
            if (idx > 0) {
                e8(c, 0x03 | (want_ref ? CLS_REF : CLS_VAL));   /* PtgName */
                e16(c, (unsigned)idx);
                e16(c, 0);
                return;
            }
        }
        c->at = save;
    }
    fail(c, "formula: expected a value, reference or function here");
}

static void postfix(cstate *c, int want_ref)
{
    primary(c, want_ref);
    while (!c->bad && eat(c, "%")) e8(c, 0x14);      /* PtgPercent */
}

static void unary(cstate *c, int want_ref)
{
    skip_ws(c);
    if (c->s[c->at] == '-' ) { c->at++; unary(c, 0); if (!c->bad) e8(c, 0x13); return; }
    if (c->s[c->at] == '+' ) { c->at++; unary(c, want_ref); return; }
    postfix(c, want_ref);
}

static void power(cstate *c, int want_ref)
{
    unary(c, want_ref);
    while (!c->bad && at(c, "^")) { eat(c, "^"); unary(c, 0); e8(c, 0x07); }
}
static void muldiv(cstate *c, int want_ref)
{
    power(c, want_ref);
    for (;;) {
        if (c->bad) return;
        if (at(c, "*"))      { eat(c, "*"); power(c, 0); e8(c, 0x05); }
        else if (at(c, "/")) { eat(c, "/"); power(c, 0); e8(c, 0x06); }
        else return;
    }
}
static void addsub(cstate *c, int want_ref)
{
    muldiv(c, want_ref);
    for (;;) {
        if (c->bad) return;
        if (at(c, "+"))      { eat(c, "+"); muldiv(c, 0); e8(c, 0x03); }
        else if (at(c, "-")) { eat(c, "-"); muldiv(c, 0); e8(c, 0x04); }
        else return;
    }
}
static void concat(cstate *c, int want_ref)
{
    addsub(c, want_ref);
    while (!c->bad && at(c, "&")) { eat(c, "&"); addsub(c, 0); e8(c, 0x08); }
}

static void expr(cstate *c, int want_ref)
{
    concat(c, want_ref);
    for (;;) {
        if (c->bad) return;
        /* the two-character comparisons must be tried before the one-character
           ones, or "<=" lexes as "<" and leaves "=" behind */
        if (at(c, "<>"))      { eat(c, "<>"); concat(c, 0); e8(c, 0x0E); }
        else if (at(c, "<=")) { eat(c, "<="); concat(c, 0); e8(c, 0x0A); }
        else if (at(c, ">=")) { eat(c, ">="); concat(c, 0); e8(c, 0x0C); }
        else if (at(c, "<"))  { eat(c, "<");  concat(c, 0); e8(c, 0x09); }
        else if (at(c, ">"))  { eat(c, ">");  concat(c, 0); e8(c, 0x0D); }
        else if (at(c, "="))  { eat(c, "=");  concat(c, 0); e8(c, 0x0B); }
        else return;
    }
}

unsigned char *ud_ptg_compile(const char *text, const ud_ptgc_env *env,
                              int base_row, int base_col, long *len)
{
    cstate c;
    (void)base_row; (void)base_col;

    if (len) *len = 0;
    if (!text) { ud_set_error("formula: no text"); return 0; }
    memset(&c, 0, sizeof c);
    c.s = text;
    c.env = env;
    ud_set_error("");
    skip_ws(&c);
    if (c.s[c.at] == '=') c.at++;

    expr(&c, 0);
    skip_ws(&c);
    if (!c.bad && c.s[c.at]) fail(&c, "formula: trailing text after the expression");
    if (c.bad || c.n == 0) {
        if (!c.bad) ud_set_error("formula: empty");
        ud_free(c.out);
        return 0;
    }
    if (len) *len = c.n;
    return c.out;
}
