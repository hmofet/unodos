/* ===========================================================================
 * uxl_calc.c - UnoCalc's formula compiler and evaluator (phase 9).
 *
 * A recursive-descent parser emitting POSTFIX directly (no intermediate
 * tree: RPN is what the file format wants and the recursion already encodes
 * the shape - the same argument uodoc's ptg compiler makes), then a stack
 * evaluator over it.
 *
 * RECALC ORDER FALLS OUT OF RECURSION.  A formula that needs a cell computes
 * that cell first, memoised per generation, so nothing has to topologically
 * sort a dependency graph.  A cell asked for while it is already being
 * computed is a CIRCULAR REFERENCE, and Excel's answer is zero plus the CIRC
 * indicator rather than a hang - so that is this engine's answer too.
 * ======================================================================== */
#include "uocalc.h"
#include "uxl_int.h"

static int  c_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void c_cpy(char *d, const char *s, int cap)
{ int i = 0; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int  c_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int  c_ieq(const char *a, const char *b)
{ while (*a && c_upper(*a) == c_upper(*b)) { a++; b++; }
  return c_upper(*a) == c_upper(*b); }
static int  is_alpha(int c)
{ return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static int  is_digit(int c) { return c >= '0' && c <= '9'; }

/* ---- maths without libm ----------------------------------------------------
 * unodoc's lane refuses libc; so does this one.  These are the handful the
 * function library actually needs, each written to the accuracy a
 * spreadsheet shows (15 significant digits at most). */
static double x_floor(double v)
{ double t = (double)(long long)v; return (v < 0 && t != v) ? t - 1 : t; }
static double x_fabs(double v) { return v < 0 ? -v : v; }
static double x_sqrt(double v)
{
    double x, p;
    int i;
    if (v < 0) return -1;
    if (v == 0) return 0;
    x = v > 1 ? v : 1;
    for (i = 0; i < 60; i++) { p = x; x = 0.5 * (x + v / x); if (x == p) break; }
    return x;
}
static double x_exp(double v)
{
    double t = 1, s = 1;
    int i, neg = v < 0;
    long k;
    if (neg) v = -v;
    k = (long)x_floor(v);              /* e^v = e^k * e^frac                */
    v -= (double)k;
    for (i = 1; i < 24; i++) { t *= v / i; s += t; }
    while (k-- > 0) s *= 2.718281828459045235;
    return neg ? 1.0 / s : s;
}
static double x_ln(double v)
{
    double y, e;
    int i, k = 0;
    if (v <= 0) return 0;
    while (v > 2) { v /= 2; k++; }
    while (v < 0.5) { v *= 2; k--; }
    y = v - 1;
    for (i = 0; i < 80; i++) { e = x_exp(y); y += 2 * (v - e) / (v + e); }
    return y + k * 0.6931471805599453094;
}
static double x_pow(double a, double b)
{
    long n;
    if (b == 0) return 1;
    n = (long)b;
    if ((double)n == b) {             /* integer powers exactly             */
        double r = 1, base = a;
        long k = n < 0 ? -n : n;
        while (k) { if (k & 1) r *= base; base *= base; k >>= 1; }
        return n < 0 ? 1.0 / r : r;
    }
    if (a <= 0) return 0;
    return x_exp(b * x_ln(a));
}
static double x_round(double v, int places)
{
    /* Excel rounds half AWAY FROM ZERO on the decimal the user sees, but
     * 2.345 * 100 is 234.49999999999997 in binary, so a bare floor(t + 0.5)
     * answers 2.34 and every accountant notices.  A relative nudge puts the
     * value back on the decimal it was typed as before the split. */
    double m = x_pow(10, places), t = v * m;
    double eps = (t < 0 ? -t : t) * 1e-12;
    t = (t < 0) ? -x_floor(-t + 0.5 + eps) : x_floor(t + 0.5 + eps);
    return t / m;
}
static double x_pi(void) { return 3.14159265358979323846; }
static double x_sin(double v)
{
    double t, s;
    int i;
    while (v > x_pi()) v -= 2 * x_pi();
    while (v < -x_pi()) v += 2 * x_pi();
    t = v; s = v;
    for (i = 1; i < 12; i++) { t *= -v * v / ((2 * i) * (2 * i + 1)); s += t; }
    return s;
}
static double x_cos(double v) { return x_sin(v + x_pi() / 2); }

/* ---- the evaluation stack --------------------------------------------------- */
typedef struct {
    uxl_val v;
    /* a reference operand keeps its extent so SUM(A1:A9) can iterate it */
    int is_ref, s, r1, c1, r2, c2;
} uxl_opnd;

static void set_err(uxl_opnd *o, int e)
{ o->is_ref = 0; o->v.kind = UXL_ERR; o->v.err = e; }
static void set_num(uxl_opnd *o, double d)
{ o->is_ref = 0; o->v.kind = UXL_NUM; o->v.num = d; }

static int eval_cell(uxl_book *b, int s, int r, int c, uxl_val *out);

/* Dereference an operand to a single value - what arithmetic needs. */
static void deref(uxl_book *b, uxl_opnd *o)
{
    if (!o->is_ref) return;
    {   uxl_val v;
        eval_cell(b, o->s, o->r1, o->c1, &v);
        o->is_ref = 0;
        o->v = v;
    }
}
static double as_num(uxl_book *b, uxl_opnd *o, int *err)
{
    deref(b, o);
    if (o->v.kind == UXL_ERR) { *err = o->v.err; return 0; }
    if (o->v.kind == UXL_STR) { *err = UXL_E_VALUE; return 0; }
    return o->v.num;                  /* EMPTY is 0, as Excel treats it     */
}

/* ---- functions --------------------------------------------------------------
 * A table, so the app's Paste Function dialog and the gate can both
 * enumerate what is actually implemented rather than what was intended. */
typedef struct {
    const char *name;
    const char *cat;
    int minargs, maxargs;             /* -1 = unlimited                     */
    int id;
} uxl_fdef;

enum {
    F_SUM = 1, F_AVERAGE, F_COUNT, F_COUNTA, F_MIN, F_MAX, F_PRODUCT,
    F_IF, F_AND, F_OR, F_NOT, F_TRUE, F_FALSE,
    F_ABS, F_SQRT, F_POWER, F_MOD, F_INT, F_ROUND, F_ROUNDUP, F_ROUNDDOWN,
    F_SIGN, F_EXP, F_LN, F_LOG10, F_PI, F_SIN, F_COS, F_TAN, F_RAND,
    F_SUMIF, F_COUNTIF, F_COUNTBLANK, F_MEDIAN, F_STDEV, F_VAR, F_LARGE,
    F_SMALL, F_RANK,
    F_LEN, F_LEFT, F_RIGHT, F_MID, F_UPPER, F_LOWER, F_TRIM, F_CONCATENATE,
    F_REPT, F_EXACT, F_FIND, F_VALUE, F_TEXT, F_CHAR, F_CODE,
    F_VLOOKUP, F_HLOOKUP, F_INDEX, F_MATCH, F_CHOOSE, F_ROW, F_COLUMN,
    F_ROWS, F_COLUMNS,
    F_ISBLANK, F_ISNUMBER, F_ISTEXT, F_ISERROR, F_ISNA, F_NA,
    F_PMT, F_FV, F_PV, F_NPV,
    F_NCOUNT
};

static const uxl_fdef kFn[] = {
    { "SUM",       "Math & Trig",  1, -1, F_SUM },
    { "AVERAGE",   "Statistical",  1, -1, F_AVERAGE },
    { "COUNT",     "Statistical",  1, -1, F_COUNT },
    { "COUNTA",    "Statistical",  1, -1, F_COUNTA },
    { "MIN",       "Statistical",  1, -1, F_MIN },
    { "MAX",       "Statistical",  1, -1, F_MAX },
    { "PRODUCT",   "Math & Trig",  1, -1, F_PRODUCT },
    { "IF",        "Logical",      2,  3, F_IF },
    { "AND",       "Logical",      1, -1, F_AND },
    { "OR",        "Logical",      1, -1, F_OR },
    { "NOT",       "Logical",      1,  1, F_NOT },
    { "TRUE",      "Logical",      0,  0, F_TRUE },
    { "FALSE",     "Logical",      0,  0, F_FALSE },
    { "ABS",       "Math & Trig",  1,  1, F_ABS },
    { "SQRT",      "Math & Trig",  1,  1, F_SQRT },
    { "POWER",     "Math & Trig",  2,  2, F_POWER },
    { "MOD",       "Math & Trig",  2,  2, F_MOD },
    { "INT",       "Math & Trig",  1,  1, F_INT },
    { "ROUND",     "Math & Trig",  2,  2, F_ROUND },
    { "ROUNDUP",   "Math & Trig",  2,  2, F_ROUNDUP },
    { "ROUNDDOWN", "Math & Trig",  2,  2, F_ROUNDDOWN },
    { "SIGN",      "Math & Trig",  1,  1, F_SIGN },
    { "EXP",       "Math & Trig",  1,  1, F_EXP },
    { "LN",        "Math & Trig",  1,  1, F_LN },
    { "LOG10",     "Math & Trig",  1,  1, F_LOG10 },
    { "PI",        "Math & Trig",  0,  0, F_PI },
    { "SIN",       "Math & Trig",  1,  1, F_SIN },
    { "COS",       "Math & Trig",  1,  1, F_COS },
    { "TAN",       "Math & Trig",  1,  1, F_TAN },
    { "RAND",      "Math & Trig",  0,  0, F_RAND },
    { "SUMIF",     "Math & Trig",  2,  3, F_SUMIF },
    { "COUNTIF",   "Math & Trig",  2,  2, F_COUNTIF },
    { "COUNTBLANK","Statistical",  1,  1, F_COUNTBLANK },
    { "MEDIAN",    "Statistical",  1, -1, F_MEDIAN },
    { "STDEV",     "Statistical",  1, -1, F_STDEV },
    { "VAR",       "Statistical",  1, -1, F_VAR },
    { "LARGE",     "Statistical",  2,  2, F_LARGE },
    { "SMALL",     "Statistical",  2,  2, F_SMALL },
    { "RANK",      "Statistical",  2,  3, F_RANK },
    { "LEN",       "Text",         1,  1, F_LEN },
    { "LEFT",      "Text",         1,  2, F_LEFT },
    { "RIGHT",     "Text",         1,  2, F_RIGHT },
    { "MID",       "Text",         3,  3, F_MID },
    { "UPPER",     "Text",         1,  1, F_UPPER },
    { "LOWER",     "Text",         1,  1, F_LOWER },
    { "TRIM",      "Text",         1,  1, F_TRIM },
    { "CONCATENATE","Text",        1, -1, F_CONCATENATE },
    { "REPT",      "Text",         2,  2, F_REPT },
    { "EXACT",     "Text",         2,  2, F_EXACT },
    { "FIND",      "Text",         2,  3, F_FIND },
    { "VALUE",     "Text",         1,  1, F_VALUE },
    { "TEXT",      "Text",         2,  2, F_TEXT },
    { "CHAR",      "Text",         1,  1, F_CHAR },
    { "CODE",      "Text",         1,  1, F_CODE },
    { "VLOOKUP",   "Lookup",       3,  4, F_VLOOKUP },
    { "HLOOKUP",   "Lookup",       3,  4, F_HLOOKUP },
    { "INDEX",     "Lookup",       2,  3, F_INDEX },
    { "MATCH",     "Lookup",       2,  3, F_MATCH },
    { "CHOOSE",    "Lookup",       2, -1, F_CHOOSE },
    { "ROW",       "Lookup",       0,  1, F_ROW },
    { "COLUMN",    "Lookup",       0,  1, F_COLUMN },
    { "ROWS",      "Lookup",       1,  1, F_ROWS },
    { "COLUMNS",   "Lookup",       1,  1, F_COLUMNS },
    { "ISBLANK",   "Information",  1,  1, F_ISBLANK },
    { "ISNUMBER",  "Information",  1,  1, F_ISNUMBER },
    { "ISTEXT",    "Information",  1,  1, F_ISTEXT },
    { "ISERROR",   "Information",  1,  1, F_ISERROR },
    { "ISNA",      "Information",  1,  1, F_ISNA },
    { "NA",        "Information",  0,  0, F_NA },
    { "PMT",       "Financial",    3,  5, F_PMT },
    { "FV",        "Financial",    3,  5, F_FV },
    { "PV",        "Financial",    3,  5, F_PV },
    { "NPV",       "Financial",    2, -1, F_NPV }
};
#define NFN ((int)(sizeof kFn / sizeof kFn[0]))

int uxl_func_count(void) { return NFN; }
const char *uxl_func_name(int i)
{ return (i >= 0 && i < NFN) ? kFn[i].name : ""; }
const char *uxl_func_category(int i)
{ return (i >= 0 && i < NFN) ? kFn[i].cat : ""; }

static int fn_lookup(const char *name)
{
    int i;
    for (i = 0; i < NFN; i++) if (c_ieq(kFn[i].name, name)) return i;
    return -1;
}

/* ---- the parser ------------------------------------------------------------- */
typedef struct {
    const char *p;
    uxl_book   *b;
    uxl_tok    *out;
    int         n, cap, err, home_s;
} uxl_ps;

static void skip(uxl_ps *ps) { while (*ps->p == ' ' || *ps->p == '\t') ps->p++; }
static int emit(uxl_ps *ps, const uxl_tok *t)
{
    if (ps->n >= ps->cap) { ps->err = 1; return 0; }
    ps->out[ps->n++] = *t;
    return 1;
}
static void emit_op(uxl_ps *ps, int op)
{ uxl_tok t; int i; for (i=0;i<(int)sizeof t;i++) ((char*)&t)[i]=0;
  t.kind = RPN_OP; t.op = (unsigned char)op; emit(ps, &t); }

static int parse_expr(uxl_ps *ps);

/* a name: a function call, a defined name, or a cell/range reference */
static int parse_name(uxl_ps *ps)
{
    char id[40];
    int n = 0;
    uxl_tok t;
    int i;
    for (i = 0; i < (int)sizeof t; i++) ((char *)&t)[i] = 0;

    while ((is_alpha(*ps->p) || is_digit(*ps->p) || *ps->p == '$' ||
            *ps->p == '.') && n < 39)
        id[n++] = *ps->p++;
    id[n] = 0;
    skip(ps);

    if (*ps->p == '(') {              /* a function call                    */
        int f = fn_lookup(id), argc = 0;
        ps->p++;
        skip(ps);
        if (*ps->p != ')') {
            for (;;) {
                if (!parse_expr(ps)) return 0;
                argc++;
                skip(ps);
                if (*ps->p == ',') { ps->p++; skip(ps); continue; }
                break;
            }
        }
        if (*ps->p != ')') { ps->err = 1; return 0; }
        ps->p++;
        if (f < 0) {                  /* an unknown function is #NAME?      */
            t.kind = RPN_ERR; t.idx = UXL_E_NAME;
            return emit(ps, &t);
        }
        if (argc < kFn[f].minargs ||
            (kFn[f].maxargs >= 0 && argc > kFn[f].maxargs)) {
            ps->err = 1; return 0;
        }
        t.kind = RPN_FUNC; t.idx = (short)f; t.argc = (unsigned char)argc;
        return emit(ps, &t);
    }

    /* TRUE / FALSE are literals, not zero-argument calls, when bare */
    if (c_ieq(id, "TRUE") || c_ieq(id, "FALSE")) {
        t.kind = RPN_BOOL;
        t.num = c_ieq(id, "TRUE") ? 1 : 0;
        return emit(ps, &t);
    }

    /* a cell reference, possibly the start of a range */
    {
        int r, c, ar, ac;
        if (uxl_a1_parse(id, &r, &c, &ar, &ac)) {
            skip(ps);
            if (*ps->p == ':') {
                char id2[40];
                int m = 0, r2, c2, ar2, ac2;
                ps->p++;
                skip(ps);
                while ((is_alpha(*ps->p) || is_digit(*ps->p) || *ps->p == '$')
                       && m < 39) id2[m++] = *ps->p++;
                id2[m] = 0;
                if (!uxl_a1_parse(id2, &r2, &c2, &ar2, &ac2)) { ps->err = 1; return 0; }
                t.kind = RPN_RANGE; t.s = (short)ps->home_s;
                t.r1 = (short)(r < r2 ? r : r2); t.c1 = (short)(c < c2 ? c : c2);
                t.r2 = (short)(r > r2 ? r : r2); t.c2 = (short)(c > c2 ? c : c2);
                return emit(ps, &t);
            }
            t.kind = RPN_REF; t.s = (short)ps->home_s;
            t.r1 = t.r2 = (short)r; t.c1 = t.c2 = (short)c;
            return emit(ps, &t);
        }
    }

    /* a defined name */
    {
        int s, r, c;
        if (uxl_name_find(ps->b, id, &s, &r, &c)) {
            t.kind = RPN_REF; t.s = (short)s;
            t.r1 = t.r2 = (short)r; t.c1 = t.c2 = (short)c;
            return emit(ps, &t);
        }
    }
    t.kind = RPN_ERR; t.idx = UXL_E_NAME;
    return emit(ps, &t);
}

static int parse_atom(uxl_ps *ps)
{
    uxl_tok t;
    int i;
    for (i = 0; i < (int)sizeof t; i++) ((char *)&t)[i] = 0;
    skip(ps);

    if (*ps->p == '(') {
        ps->p++;
        if (!parse_expr(ps)) return 0;
        skip(ps);
        if (*ps->p != ')') { ps->err = 1; return 0; }
        ps->p++;
        return 1;
    }
    if (*ps->p == '-') { ps->p++; if (!parse_atom(ps)) return 0;
                         emit_op(ps, OP_NEG); return 1; }
    if (*ps->p == '+') { ps->p++; return parse_atom(ps); }
    if (*ps->p == '"') {
        char buf[UXL_STRLEN];
        int n = 0;
        ps->p++;
        while (*ps->p && n < UXL_STRLEN - 1) {
            if (*ps->p == '"') {
                if (ps->p[1] == '"') { buf[n++] = '"'; ps->p += 2; continue; }
                break;
            }
            buf[n++] = *ps->p++;
        }
        if (*ps->p != '"') { ps->err = 1; return 0; }
        ps->p++;
        buf[n] = 0;
        t.kind = RPN_STR;
        t.idx = (short)uxl_intern(ps->b, buf);
        return emit(ps, &t);
    }
    if (is_digit(*ps->p) || (*ps->p == '.' && is_digit(ps->p[1]))) {
        double v = 0, frac = 0.1;
        while (is_digit(*ps->p)) { v = v * 10 + (*ps->p - '0'); ps->p++; }
        if (*ps->p == '.') {
            ps->p++;
            while (is_digit(*ps->p)) { v += (*ps->p - '0') * frac; frac /= 10; ps->p++; }
        }
        if (*ps->p == 'e' || *ps->p == 'E') {
            int sgn = 1, ex = 0;
            ps->p++;
            if (*ps->p == '+') ps->p++;
            else if (*ps->p == '-') { sgn = -1; ps->p++; }
            while (is_digit(*ps->p)) { ex = ex * 10 + (*ps->p - '0'); ps->p++; }
            v *= x_pow(10, sgn * ex);
        }
        t.kind = RPN_NUM; t.num = v;
        return emit(ps, &t);
    }
    if (is_alpha(*ps->p) || *ps->p == '$') return parse_name(ps);
    ps->err = 1;
    return 0;
}

/* postfix % binds tighter than anything but a reference */
static int parse_postfix(uxl_ps *ps)
{
    if (!parse_atom(ps)) return 0;
    skip(ps);
    while (*ps->p == '%') { ps->p++; emit_op(ps, OP_PCT); skip(ps); }
    return 1;
}
/* ^ is LEFT-associative in Excel: 2^3^2 is 64, not 512 */
static int parse_pow(uxl_ps *ps)
{
    if (!parse_postfix(ps)) return 0;
    for (;;) {
        skip(ps);
        if (*ps->p != '^') return 1;
        ps->p++;
        if (!parse_postfix(ps)) return 0;
        emit_op(ps, OP_POW);
    }
}
static int parse_mul(uxl_ps *ps)
{
    if (!parse_pow(ps)) return 0;
    for (;;) {
        skip(ps);
        if (*ps->p == '*')      { ps->p++; if (!parse_pow(ps)) return 0; emit_op(ps, OP_MUL); }
        else if (*ps->p == '/') { ps->p++; if (!parse_pow(ps)) return 0; emit_op(ps, OP_DIV); }
        else return 1;
    }
}
static int parse_add(uxl_ps *ps)
{
    if (!parse_mul(ps)) return 0;
    for (;;) {
        skip(ps);
        if (*ps->p == '+')      { ps->p++; if (!parse_mul(ps)) return 0; emit_op(ps, OP_ADD); }
        else if (*ps->p == '-') { ps->p++; if (!parse_mul(ps)) return 0; emit_op(ps, OP_SUB); }
        else return 1;
    }
}
static int parse_cat(uxl_ps *ps)
{
    if (!parse_add(ps)) return 0;
    for (;;) {
        skip(ps);
        if (*ps->p != '&') return 1;
        ps->p++;
        if (!parse_add(ps)) return 0;
        emit_op(ps, OP_CAT);
    }
}
static int parse_expr(uxl_ps *ps)
{
    if (!parse_cat(ps)) return 0;
    for (;;) {
        int op = 0;
        skip(ps);
        if (ps->p[0] == '<' && ps->p[1] == '=') { op = OP_LE; ps->p += 2; }
        else if (ps->p[0] == '>' && ps->p[1] == '=') { op = OP_GE; ps->p += 2; }
        else if (ps->p[0] == '<' && ps->p[1] == '>') { op = OP_NE; ps->p += 2; }
        else if (ps->p[0] == '<') { op = OP_LT; ps->p++; }
        else if (ps->p[0] == '>') { op = OP_GT; ps->p++; }
        else if (ps->p[0] == '=') { op = OP_EQ; ps->p++; }
        else return 1;
        if (!parse_cat(ps)) return 0;
        emit_op(ps, op);
    }
}

int uxl_compile(uxl_book *b, const char *text, int home_s,
                uxl_tok *out, int cap)
{
    uxl_ps ps;
    ps.p = text; ps.b = b; ps.out = out; ps.n = 0; ps.cap = cap;
    ps.err = 0; ps.home_s = home_s;
    if (*ps.p == '=') ps.p++;
    if (!parse_expr(&ps)) return -1;
    skip(&ps);
    if (*ps.p || ps.err) return -1;
    return ps.n;
}

int uxl_set_formula(uxl_book *b, int s, int r, int c, const char *text)
{
    uxl_tok rpn[UXL_MAXRPN];
    int n;
    uxl_cell *p;
    if (!b || !text) return 0;
    n = uxl_compile(b, text, s, rpn, UXL_MAXRPN);
    if (n < 0) return 0;
    p = uxl_slot(b, s, r, c, 1);
    if (!p) return 0;
    c_cpy(p->fml, text, UXL_FMLLEN);
    if (!uxl_rpn_store(b, p, rpn, n)) return 0;
    p->v.kind = UXL_EMPTY;
    b->rev++; b->dirty = 1;
    return 1;
}

/* ---- evaluation -------------------------------------------------------------- */
static int cmp_vals(uxl_book *b, uxl_opnd *a, uxl_opnd *c, int *err)
{
    deref(b, a); deref(b, c);
    if (a->v.kind == UXL_ERR) { *err = a->v.err; return 0; }
    if (c->v.kind == UXL_ERR) { *err = c->v.err; return 0; }
    if (a->v.kind == UXL_STR && c->v.kind == UXL_STR) {
        const char *x = uxl_pool(b, a->v.str), *y = uxl_pool(b, c->v.str);
        while (*x && c_upper(*x) == c_upper(*y)) { x++; y++; }
        return c_upper(*x) - c_upper(*y);
    }
    if (a->v.kind == UXL_STR) return 1;      /* text sorts above numbers    */
    if (c->v.kind == UXL_STR) return -1;
    return a->v.num < c->v.num ? -1 : (a->v.num > c->v.num ? 1 : 0);
}

static void to_text(uxl_book *b, uxl_opnd *o, char *out, int cap)
{
    deref(b, o);
    out[0] = 0;
    if (o->v.kind == UXL_STR) c_cpy(out, uxl_pool(b, o->v.str), cap);
    else if (o->v.kind == UXL_NUM) uxl_general(o->v.num, out, cap);
    else if (o->v.kind == UXL_BOOL) c_cpy(out, o->v.num ? "TRUE" : "FALSE", cap);
    else if (o->v.kind == UXL_ERR) c_cpy(out, uxl_err_text(o->v.err), cap);
}

/* Iterate an operand's cells: a range walks its rectangle, anything else is
 * itself.  `numeric_only` is the COUNT vs COUNTA distinction. */
typedef void (*uxl_iter)(uxl_book *, const uxl_val *, void *);
static void iterate(uxl_book *b, uxl_opnd *o, uxl_iter fn, void *ctx)
{
    if (o->is_ref) {
        int r, c;
        for (r = o->r1; r <= o->r2; r++)
            for (c = o->c1; c <= o->c2; c++) {
                uxl_val v;
                eval_cell(b, o->s, r, c, &v);
                fn(b, &v, ctx);
            }
        return;
    }
    fn(b, &o->v, ctx);
}

typedef struct {
    double sum, sum2, minv, maxv, prod;
    int n, na, blank, err;
    double vals[256];
    int nvals;
} uxl_acc;

static void acc_one(uxl_book *b, const uxl_val *v, void *ctx)
{
    uxl_acc *a = (uxl_acc *)ctx;
    (void)b;
    if (v->kind == UXL_ERR) { if (!a->err) a->err = v->err; return; }
    if (v->kind == UXL_EMPTY) { a->blank++; return; }
    a->na++;
    if (v->kind == UXL_STR) return;
    if (a->n == 0) { a->minv = a->maxv = v->num; a->prod = 1; }
    if (v->num < a->minv) a->minv = v->num;
    if (v->num > a->maxv) a->maxv = v->num;
    a->sum += v->num;
    a->sum2 += v->num * v->num;
    a->prod *= v->num;
    if (a->nvals < 256) a->vals[a->nvals++] = v->num;
    a->n++;
}
static void acc_init(uxl_acc *a)
{ int i; for (i = 0; i < (int)sizeof *a; i++) ((char *)a)[i] = 0; a->prod = 1; }

static void sort_vals(double *v, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        double t = v[i];
        for (j = i - 1; j >= 0 && v[j] > t; j--) v[j + 1] = v[j];
        v[j + 1] = t;
    }
}

static void do_func(uxl_book *b, int f, uxl_opnd *arg, int argc, uxl_opnd *res)
{
    int err = 0, i;
    uxl_acc a;
    int id = kFn[f].id;

    switch (id) {
    case F_TRUE:  res->is_ref = 0; res->v.kind = UXL_BOOL; res->v.num = 1; return;
    case F_FALSE: res->is_ref = 0; res->v.kind = UXL_BOOL; res->v.num = 0; return;
    case F_PI:    set_num(res, x_pi()); return;
    case F_RAND:  set_num(res, 0.5); return;   /* deterministic: a gate must
                                                * be able to assert it      */
    case F_NA:    set_err(res, UXL_E_NA); return;
    default: break;
    }

    /* the accumulating family */
    if (id == F_SUM || id == F_AVERAGE || id == F_COUNT || id == F_COUNTA ||
        id == F_MIN || id == F_MAX || id == F_PRODUCT || id == F_MEDIAN ||
        id == F_STDEV || id == F_VAR || id == F_COUNTBLANK) {
        acc_init(&a);
        for (i = 0; i < argc; i++) iterate(b, &arg[i], acc_one, &a);
        if (a.err) { set_err(res, a.err); return; }
        switch (id) {
        case F_SUM:        set_num(res, a.sum); return;
        case F_PRODUCT:    set_num(res, a.n ? a.prod : 0); return;
        case F_COUNT:      set_num(res, a.n); return;
        case F_COUNTA:     set_num(res, a.na); return;
        case F_COUNTBLANK: set_num(res, a.blank); return;
        case F_AVERAGE:
            if (!a.n) { set_err(res, UXL_E_DIV0); return; }
            set_num(res, a.sum / a.n); return;
        case F_MIN: set_num(res, a.n ? a.minv : 0); return;
        case F_MAX: set_num(res, a.n ? a.maxv : 0); return;
        case F_MEDIAN:
            if (!a.n) { set_err(res, UXL_E_NUM); return; }
            sort_vals(a.vals, a.nvals);
            set_num(res, (a.nvals & 1) ? a.vals[a.nvals / 2]
                       : (a.vals[a.nvals / 2 - 1] + a.vals[a.nvals / 2]) / 2);
            return;
        case F_STDEV:
        case F_VAR: {
            double mean, ss = 0;
            if (a.n < 2) { set_err(res, UXL_E_DIV0); return; }
            mean = a.sum / a.n;
            for (i = 0; i < a.nvals; i++)
                ss += (a.vals[i] - mean) * (a.vals[i] - mean);
            ss /= (a.n - 1);
            set_num(res, id == F_VAR ? ss : x_sqrt(ss));
            return;
        }
        default: break;
        }
    }

    switch (id) {
    case F_IF: {
        double cond = as_num(b, &arg[0], &err);
        if (err) { set_err(res, err); return; }
        if (cond != 0) { deref(b, &arg[1]); *res = arg[1]; }
        else if (argc >= 3) { deref(b, &arg[2]); *res = arg[2]; }
        else { res->is_ref = 0; res->v.kind = UXL_BOOL; res->v.num = 0; }
        res->is_ref = 0;
        return;
    }
    case F_AND: case F_OR: {
        int all = (id == F_AND), any = 0;
        for (i = 0; i < argc; i++) {
            double v = as_num(b, &arg[i], &err);
            if (err) { set_err(res, err); return; }
            if (v != 0) any = 1; else all = 0;
        }
        res->is_ref = 0; res->v.kind = UXL_BOOL;
        res->v.num = (id == F_AND) ? (all ? 1 : 0) : (any ? 1 : 0);
        return;
    }
    case F_NOT: {
        double v = as_num(b, &arg[0], &err);
        if (err) { set_err(res, err); return; }
        res->is_ref = 0; res->v.kind = UXL_BOOL; res->v.num = v ? 0 : 1;
        return;
    }
    case F_ABS: case F_SQRT: case F_INT: case F_SIGN: case F_EXP:
    case F_LN: case F_LOG10: case F_SIN: case F_COS: case F_TAN: {
        double v = as_num(b, &arg[0], &err);
        if (err) { set_err(res, err); return; }
        switch (id) {
        case F_ABS:   set_num(res, x_fabs(v)); return;
        case F_SQRT:  if (v < 0) { set_err(res, UXL_E_NUM); return; }
                      set_num(res, x_sqrt(v)); return;
        case F_INT:   set_num(res, x_floor(v)); return;
        case F_SIGN:  set_num(res, v > 0 ? 1 : (v < 0 ? -1 : 0)); return;
        case F_EXP:   set_num(res, x_exp(v)); return;
        case F_LN:    if (v <= 0) { set_err(res, UXL_E_NUM); return; }
                      set_num(res, x_ln(v)); return;
        case F_LOG10: if (v <= 0) { set_err(res, UXL_E_NUM); return; }
                      set_num(res, x_ln(v) / 2.302585092994046); return;
        case F_SIN:   set_num(res, x_sin(v)); return;
        case F_COS:   set_num(res, x_cos(v)); return;
        case F_TAN:   { double cs = x_cos(v);
                        if (cs == 0) { set_err(res, UXL_E_DIV0); return; }
                        set_num(res, x_sin(v) / cs); return; }
        }
        return;
    }
    case F_POWER: case F_MOD: case F_ROUND: case F_ROUNDUP: case F_ROUNDDOWN: {
        double x = as_num(b, &arg[0], &err), y = as_num(b, &arg[1], &err);
        if (err) { set_err(res, err); return; }
        switch (id) {
        case F_POWER: set_num(res, x_pow(x, y)); return;
        case F_MOD:
            if (y == 0) { set_err(res, UXL_E_DIV0); return; }
            set_num(res, x - y * x_floor(x / y));   /* Excel's sign rule    */
            return;
        case F_ROUND: set_num(res, x_round(x, (int)y)); return;
        case F_ROUNDUP: {
            double m = x_pow(10, (int)y), t = x * m;
            t = (t < 0) ? -x_floor(-t + 0.9999999999) : x_floor(t + 0.9999999999);
            set_num(res, t / m);
            return;
        }
        case F_ROUNDDOWN: {
            double m = x_pow(10, (int)y), t = x * m;
            t = (t < 0) ? -x_floor(-t) : x_floor(t);
            set_num(res, t / m);
            return;
        }
        }
        return;
    }
    case F_LEN: case F_UPPER: case F_LOWER: case F_TRIM: case F_VALUE:
    case F_CODE: {
        char s[UXL_STRLEN];
        to_text(b, &arg[0], s, (int)sizeof s);
        switch (id) {
        case F_LEN: set_num(res, c_len(s)); return;
        case F_CODE: set_num(res, (unsigned char)s[0]); return;
        case F_VALUE: {
            double v = 0, frac = 0.1;
            int i2 = 0, neg = 0;
            if (s[i2] == '-') { neg = 1; i2++; }
            while (is_digit(s[i2])) { v = v * 10 + (s[i2] - '0'); i2++; }
            if (s[i2] == '.') { i2++;
                while (is_digit(s[i2])) { v += (s[i2] - '0') * frac; frac /= 10; i2++; } }
            if (s[i2]) { set_err(res, UXL_E_VALUE); return; }
            set_num(res, neg ? -v : v);
            return;
        }
        default: {
            char o[UXL_STRLEN];
            int n = 0, j = 0;
            if (id == F_TRIM) {
                int k = 0;
                while (s[k] == ' ') k++;
                while (s[k]) {
                    if (s[k] == ' ' && (n == 0 || o[n-1] == ' ')) { k++; continue; }
                    o[n++] = s[k++];
                }
                while (n && o[n-1] == ' ') n--;
            } else {
                for (j = 0; s[j] && n < UXL_STRLEN - 1; j++)
                    o[n++] = (char)(id == F_UPPER ? c_upper(s[j])
                                  : (s[j] >= 'A' && s[j] <= 'Z' ? s[j] + 32 : s[j]));
            }
            o[n] = 0;
            res->is_ref = 0; res->v.kind = UXL_STR;
            res->v.str = uxl_intern(b, o);
            return;
        }
        }
    }
    case F_LEFT: case F_RIGHT: case F_MID: case F_REPT: case F_CHAR: {
        char s[UXL_STRLEN], o[UXL_STRLEN];
        int n = 0;
        if (id == F_CHAR) {
            double v = as_num(b, &arg[0], &err);
            if (err) { set_err(res, err); return; }
            o[0] = (char)(int)v; o[1] = 0;
            res->is_ref = 0; res->v.kind = UXL_STR;
            res->v.str = uxl_intern(b, o);
            return;
        }
        to_text(b, &arg[0], s, (int)sizeof s);
        if (id == F_LEFT || id == F_RIGHT) {
            int k = (argc >= 2) ? (int)as_num(b, &arg[1], &err) : 1;
            int L = c_len(s), i2;
            if (err) { set_err(res, err); return; }
            if (k < 0) { set_err(res, UXL_E_VALUE); return; }
            if (k > L) k = L;
            for (i2 = 0; i2 < k; i2++)
                o[n++] = (id == F_LEFT) ? s[i2] : s[L - k + i2];
        } else if (id == F_MID) {
            int st = (int)as_num(b, &arg[1], &err);
            int k = (int)as_num(b, &arg[2], &err);
            int L = c_len(s), i2;
            if (err) { set_err(res, err); return; }
            if (st < 1 || k < 0) { set_err(res, UXL_E_VALUE); return; }
            for (i2 = st - 1; i2 < L && n < k; i2++) o[n++] = s[i2];
        } else {                       /* REPT */
            int k = (int)as_num(b, &arg[1], &err), i2, j;
            int L = c_len(s);
            if (err) { set_err(res, err); return; }
            for (i2 = 0; i2 < k; i2++)
                for (j = 0; j < L && n < UXL_STRLEN - 1; j++) o[n++] = s[j];
        }
        o[n] = 0;
        res->is_ref = 0; res->v.kind = UXL_STR;
        res->v.str = uxl_intern(b, o);
        return;
    }
    case F_CONCATENATE: {
        char o[UXL_STRLEN], s[UXL_STRLEN];
        int n = 0, j;
        for (i = 0; i < argc; i++) {
            to_text(b, &arg[i], s, (int)sizeof s);
            for (j = 0; s[j] && n < UXL_STRLEN - 1; j++) o[n++] = s[j];
        }
        o[n] = 0;
        res->is_ref = 0; res->v.kind = UXL_STR;
        res->v.str = uxl_intern(b, o);
        return;
    }
    case F_EXACT: {
        char s1[UXL_STRLEN], s2[UXL_STRLEN];
        int j = 0;
        to_text(b, &arg[0], s1, (int)sizeof s1);
        to_text(b, &arg[1], s2, (int)sizeof s2);
        while (s1[j] && s1[j] == s2[j]) j++;
        res->is_ref = 0; res->v.kind = UXL_BOOL;
        res->v.num = (s1[j] == s2[j]) ? 1 : 0;
        return;
    }
    case F_FIND: {
        char needle[UXL_STRLEN], hay[UXL_STRLEN];
        int st = 1, j, k;
        to_text(b, &arg[0], needle, (int)sizeof needle);
        to_text(b, &arg[1], hay, (int)sizeof hay);
        if (argc >= 3) st = (int)as_num(b, &arg[2], &err);
        if (err) { set_err(res, err); return; }
        for (j = st - 1; hay[j]; j++) {
            for (k = 0; needle[k] && hay[j + k] == needle[k]; k++) ;
            if (!needle[k]) { set_num(res, j + 1); return; }
        }
        set_err(res, UXL_E_VALUE);
        return;
    }
    case F_TEXT: {
        char code[UXL_STRLEN], o[UXL_STRLEN];
        double v = as_num(b, &arg[0], &err);
        if (err) { set_err(res, err); return; }
        to_text(b, &arg[1], code, (int)sizeof code);
        uxl_format(v, code, o, (int)sizeof o);
        res->is_ref = 0; res->v.kind = UXL_STR;
        res->v.str = uxl_intern(b, o);
        return;
    }
    case F_ISBLANK: case F_ISNUMBER: case F_ISTEXT: case F_ISERROR:
    case F_ISNA: {
        int k;
        deref(b, &arg[0]);
        switch (id) {
        case F_ISBLANK:  k = arg[0].v.kind == UXL_EMPTY; break;
        case F_ISNUMBER: k = arg[0].v.kind == UXL_NUM; break;
        case F_ISTEXT:   k = arg[0].v.kind == UXL_STR; break;
        case F_ISERROR:  k = arg[0].v.kind == UXL_ERR; break;
        default:         k = arg[0].v.kind == UXL_ERR &&
                             arg[0].v.err == UXL_E_NA; break;
        }
        res->is_ref = 0; res->v.kind = UXL_BOOL; res->v.num = k;
        return;
    }
    case F_ROW: case F_COLUMN:
        if (argc >= 1 && arg[0].is_ref)
            set_num(res, id == F_ROW ? arg[0].r1 + 1 : arg[0].c1 + 1);
        else set_err(res, UXL_E_VALUE);
        return;
    case F_ROWS: case F_COLUMNS:
        if (arg[0].is_ref)
            set_num(res, id == F_ROWS ? arg[0].r2 - arg[0].r1 + 1
                                      : arg[0].c2 - arg[0].c1 + 1);
        else set_num(res, 1);
        return;
    case F_CHOOSE: {
        int k = (int)as_num(b, &arg[0], &err);
        if (err) { set_err(res, err); return; }
        if (k < 1 || k >= argc) { set_err(res, UXL_E_VALUE); return; }
        deref(b, &arg[k]);
        *res = arg[k];
        res->is_ref = 0;
        return;
    }
    case F_LARGE: case F_SMALL: {
        int k;
        acc_init(&a);
        iterate(b, &arg[0], acc_one, &a);
        k = (int)as_num(b, &arg[1], &err);
        if (err) { set_err(res, err); return; }
        if (k < 1 || k > a.nvals) { set_err(res, UXL_E_NUM); return; }
        sort_vals(a.vals, a.nvals);
        set_num(res, id == F_SMALL ? a.vals[k - 1] : a.vals[a.nvals - k]);
        return;
    }
    case F_RANK: {
        double v = as_num(b, &arg[0], &err);
        int desc = 1, rank = 1;
        acc_init(&a);
        iterate(b, &arg[1], acc_one, &a);
        if (argc >= 3) desc = as_num(b, &arg[2], &err) == 0;
        if (err) { set_err(res, err); return; }
        for (i = 0; i < a.nvals; i++)
            if (desc ? (a.vals[i] > v) : (a.vals[i] < v)) rank++;
        set_num(res, rank);
        return;
    }
    case F_SUMIF: case F_COUNTIF: {
        /* the criterion is a value or a "<10" style string */
        char crit[UXL_STRLEN];
        double want = 0, total = 0;
        int cnt = 0, cop = OP_EQ, r, c, k = 0;
        to_text(b, &arg[1], crit, (int)sizeof crit);
        if (crit[0] == '<' && crit[1] == '=') { cop = OP_LE; k = 2; }
        else if (crit[0] == '>' && crit[1] == '=') { cop = OP_GE; k = 2; }
        else if (crit[0] == '<' && crit[1] == '>') { cop = OP_NE; k = 2; }
        else if (crit[0] == '<') { cop = OP_LT; k = 1; }
        else if (crit[0] == '>') { cop = OP_GT; k = 1; }
        else if (crit[0] == '=') { cop = OP_EQ; k = 1; }
        {   double v = 0, frac = 0.1;
            int j = k, neg = 0;
            if (crit[j] == '-') { neg = 1; j++; }
            while (is_digit(crit[j])) { v = v * 10 + (crit[j] - '0'); j++; }
            if (crit[j] == '.') { j++;
                while (is_digit(crit[j])) { v += (crit[j]-'0')*frac; frac/=10; j++; } }
            want = neg ? -v : v;
        }
        if (!arg[0].is_ref) { set_err(res, UXL_E_VALUE); return; }
        for (r = arg[0].r1; r <= arg[0].r2; r++)
            for (c = arg[0].c1; c <= arg[0].c2; c++) {
                uxl_val v;
                int hit = 0;
                eval_cell(b, arg[0].s, r, c, &v);
                if (v.kind != UXL_NUM) continue;
                switch (cop) {
                case OP_LT: hit = v.num <  want; break;
                case OP_LE: hit = v.num <= want; break;
                case OP_GT: hit = v.num >  want; break;
                case OP_GE: hit = v.num >= want; break;
                case OP_NE: hit = v.num != want; break;
                default:    hit = v.num == want; break;
                }
                if (!hit) continue;
                cnt++;
                if (id == F_SUMIF) {
                    if (argc >= 3 && arg[2].is_ref) {
                        uxl_val sv;
                        eval_cell(b, arg[2].s,
                                  arg[2].r1 + (r - arg[0].r1),
                                  arg[2].c1 + (c - arg[0].c1), &sv);
                        if (sv.kind == UXL_NUM) total += sv.num;
                    } else total += v.num;
                }
            }
        set_num(res, id == F_COUNTIF ? cnt : total);
        return;
    }
    case F_VLOOKUP: case F_HLOOKUP: {
        int idx = (int)as_num(b, &arg[2], &err), r, c;
        int exact = (argc >= 4) ? (as_num(b, &arg[3], &err) == 0) : 0;
        uxl_opnd key = arg[0];
        if (err) { set_err(res, err); return; }
        if (!arg[1].is_ref) { set_err(res, UXL_E_VALUE); return; }
        deref(b, &key);
        if (id == F_VLOOKUP) {
            int best = -1;
            for (r = arg[1].r1; r <= arg[1].r2; r++) {
                uxl_opnd cell;
                int cmp;
                int e2 = 0;
                cell.is_ref = 1; cell.s = arg[1].s;
                cell.r1 = cell.r2 = (short)r;
                cell.c1 = cell.c2 = arg[1].c1;
                { uxl_opnd k2 = key; cmp = cmp_vals(b, &k2, &cell, &e2); }
                if (cmp == 0) { best = r; break; }
                if (!exact && cmp > 0) best = r;
            }
            if (best < 0) { set_err(res, UXL_E_NA); return; }
            { uxl_val v;
              eval_cell(b, arg[1].s, best, arg[1].c1 + idx - 1, &v);
              res->is_ref = 0; res->v = v; }
            return;
        }
        {   int best = -1;
            for (c = arg[1].c1; c <= arg[1].c2; c++) {
                uxl_opnd cell;
                int cmp, e2 = 0;
                cell.is_ref = 1; cell.s = arg[1].s;
                cell.r1 = cell.r2 = arg[1].r1;
                cell.c1 = cell.c2 = (short)c;
                { uxl_opnd k2 = key; cmp = cmp_vals(b, &k2, &cell, &e2); }
                if (cmp == 0) { best = c; break; }
                if (!exact && cmp > 0) best = c;
            }
            if (best < 0) { set_err(res, UXL_E_NA); return; }
            { uxl_val v;
              eval_cell(b, arg[1].s, arg[1].r1 + idx - 1, best, &v);
              res->is_ref = 0; res->v = v; }
        }
        return;
    }
    case F_INDEX: {
        int rr = (int)as_num(b, &arg[1], &err);
        int cc = (argc >= 3) ? (int)as_num(b, &arg[2], &err) : 1;
        if (err) { set_err(res, err); return; }
        if (!arg[0].is_ref) { set_err(res, UXL_E_VALUE); return; }
        if (arg[0].r1 == arg[0].r2 && argc < 3) { cc = rr; rr = 1; }
        { uxl_val v;
          eval_cell(b, arg[0].s, arg[0].r1 + rr - 1, arg[0].c1 + cc - 1, &v);
          res->is_ref = 0; res->v = v; }
        return;
    }
    case F_MATCH: {
        int type = (argc >= 3) ? (int)as_num(b, &arg[2], &err) : 1;
        int r, c, n = 0, best = -1;
        uxl_opnd key = arg[0];
        if (err) { set_err(res, err); return; }
        if (!arg[1].is_ref) { set_err(res, UXL_E_VALUE); return; }
        deref(b, &key);
        for (r = arg[1].r1; r <= arg[1].r2; r++)
            for (c = arg[1].c1; c <= arg[1].c2; c++) {
                uxl_opnd cell;
                int cmp, e2 = 0;
                n++;
                cell.is_ref = 1; cell.s = arg[1].s;
                cell.r1 = cell.r2 = (short)r; cell.c1 = cell.c2 = (short)c;
                { uxl_opnd k2 = key; cmp = cmp_vals(b, &k2, &cell, &e2); }
                if (cmp == 0) { set_num(res, n); return; }
                if (type == 1 && cmp > 0) best = n;
                if (type == -1 && cmp < 0) best = n;
            }
        if (best < 0) { set_err(res, UXL_E_NA); return; }
        set_num(res, best);
        return;
    }
    case F_PMT: case F_FV: case F_PV: {
        double rate = as_num(b, &arg[0], &err);
        double nper = as_num(b, &arg[1], &err);
        double v3   = as_num(b, &arg[2], &err);
        double extra = (argc >= 4) ? as_num(b, &arg[3], &err) : 0;
        double f;
        if (err) { set_err(res, err); return; }
        f = x_pow(1 + rate, nper);
        if (id == F_PMT) {
            if (rate == 0) { set_num(res, -(v3 + extra) / nper); return; }
            set_num(res, -(v3 * f + extra) * rate / (f - 1));
            return;
        }
        if (id == F_FV) {
            if (rate == 0) { set_num(res, -(extra + v3 * nper)); return; }
            set_num(res, -(extra * f + v3 * (f - 1) / rate));
            return;
        }
        if (rate == 0) { set_num(res, -(extra + v3 * nper)); return; }
        set_num(res, -(extra + v3 * (f - 1) / rate) / f);
        return;
    }
    case F_NPV: {
        double rate = as_num(b, &arg[0], &err), total = 0;
        int k = 1;
        if (err) { set_err(res, err); return; }
        for (i = 1; i < argc; i++) {
            if (arg[i].is_ref) {
                int r, c;
                for (r = arg[i].r1; r <= arg[i].r2; r++)
                    for (c = arg[i].c1; c <= arg[i].c2; c++) {
                        uxl_val v;
                        eval_cell(b, arg[i].s, r, c, &v);
                        if (v.kind == UXL_NUM)
                            total += v.num / x_pow(1 + rate, k++);
                    }
            } else {
                double v = as_num(b, &arg[i], &err);
                total += v / x_pow(1 + rate, k++);
            }
        }
        set_num(res, total);
        return;
    }
    default: break;
    }
    set_err(res, UXL_E_VALUE);
}

/* ---- the RPN machine --------------------------------------------------------- */
static void run_rpn(uxl_book *b, const uxl_tok *rpn, int n, int home_s,
                    uxl_val *out)
{
    uxl_opnd st[32];
    int sp = 0, i;
    (void)home_s;
    for (i = 0; i < n; i++) {
        const uxl_tok *t = &rpn[i];
        if (sp < 0 || sp > 30) break;
        switch (t->kind) {
        case RPN_NUM:  st[sp].is_ref = 0; st[sp].v.kind = UXL_NUM;
                       st[sp].v.num = t->num; sp++; break;
        case RPN_STR:  st[sp].is_ref = 0; st[sp].v.kind = UXL_STR;
                       st[sp].v.str = t->idx; sp++; break;
        case RPN_BOOL: st[sp].is_ref = 0; st[sp].v.kind = UXL_BOOL;
                       st[sp].v.num = t->num; sp++; break;
        case RPN_ERR:  st[sp].is_ref = 0; st[sp].v.kind = UXL_ERR;
                       st[sp].v.err = t->idx; sp++; break;
        case RPN_REF:
        case RPN_RANGE:
            st[sp].is_ref = 1; st[sp].s = t->s;
            st[sp].r1 = t->r1; st[sp].c1 = t->c1;
            st[sp].r2 = t->r2; st[sp].c2 = t->c2;
            st[sp].v.kind = UXL_EMPTY;
            sp++;
            break;
        case RPN_OP: {
            int err = 0;
            if (t->op == OP_NEG || t->op == OP_PCT) {
                double v;
                if (sp < 1) return;
                v = as_num(b, &st[sp-1], &err);
                if (err) { set_err(&st[sp-1], err); break; }
                set_num(&st[sp-1], t->op == OP_NEG ? -v : v / 100);
                break;
            }
            if (sp < 2) return;
            sp--;
            if (t->op == OP_CAT) {
                char a1[UXL_STRLEN], a2[UXL_STRLEN], o[UXL_STRLEN * 2];
                int k = 0, j;
                to_text(b, &st[sp-1], a1, (int)sizeof a1);
                to_text(b, &st[sp],   a2, (int)sizeof a2);
                for (j = 0; a1[j] && k < UXL_STRLEN - 1; j++) o[k++] = a1[j];
                for (j = 0; a2[j] && k < UXL_STRLEN - 1; j++) o[k++] = a2[j];
                o[k] = 0;
                st[sp-1].is_ref = 0; st[sp-1].v.kind = UXL_STR;
                st[sp-1].v.str = uxl_intern(b, o);
                break;
            }
            if (t->op >= OP_LT && t->op <= OP_NE) {
                int cmp = cmp_vals(b, &st[sp-1], &st[sp], &err), r;
                if (err) { set_err(&st[sp-1], err); break; }
                switch (t->op) {
                case OP_LT: r = cmp <  0; break;
                case OP_LE: r = cmp <= 0; break;
                case OP_GT: r = cmp >  0; break;
                case OP_GE: r = cmp >= 0; break;
                case OP_EQ: r = cmp == 0; break;
                default:    r = cmp != 0; break;
                }
                st[sp-1].is_ref = 0; st[sp-1].v.kind = UXL_BOOL;
                st[sp-1].v.num = r;
                break;
            }
            {
                double a1 = as_num(b, &st[sp-1], &err);
                double a2 = as_num(b, &st[sp], &err);
                if (err) { set_err(&st[sp-1], err); break; }
                switch (t->op) {
                case OP_ADD: set_num(&st[sp-1], a1 + a2); break;
                case OP_SUB: set_num(&st[sp-1], a1 - a2); break;
                case OP_MUL: set_num(&st[sp-1], a1 * a2); break;
                case OP_DIV:
                    if (a2 == 0) set_err(&st[sp-1], UXL_E_DIV0);
                    else set_num(&st[sp-1], a1 / a2);
                    break;
                case OP_POW: set_num(&st[sp-1], x_pow(a1, a2)); break;
                default: set_err(&st[sp-1], UXL_E_VALUE); break;
                }
            }
            break;
        }
        case RPN_FUNC: {
            uxl_opnd res;
            int k;
            if (sp < t->argc) return;
            sp -= t->argc;
            for (k = 0; k < (int)sizeof res; k++) ((char *)&res)[k] = 0;
            do_func(b, t->idx, &st[sp], t->argc, &res);
            st[sp] = res;
            sp++;
            break;
        }
        default: break;
        }
    }
    if (sp >= 1) {
        deref(b, &st[sp-1]);
        *out = st[sp-1].v;
    } else {
        out->kind = UXL_EMPTY;
    }
}

/* Evaluate one cell, memoised for this generation.  A cell reached while it
 * is already being computed is circular. */
static int eval_cell(uxl_book *b, int s, int r, int c, uxl_val *out)
{
    int i;
    uxl_cell *p;
    out->kind = UXL_EMPTY; out->num = 0; out->str = 0; out->err = 0;
    if (!b || s < 0 || s >= b->nsheet) { out->kind = UXL_ERR; out->err = UXL_E_REF; return 0; }
    i = uxl_find(b, s, r, c);
    if (i < 0) return 1;                      /* empty is not an error      */
    p = uxl_nth(b, s, i);
    if (!p->nrpn) { *out = p->v; return 1; }
    if (p->mark == 1) {                       /* already computing: circular */
        b->circular = 1;
        p->v.kind = UXL_NUM; p->v.num = 0;
        *out = p->v;
        return 0;
    }
    if (p->mark == 2 && p->gen == b->gen) { *out = p->v; return 1; }
    p->mark = 1;
    run_rpn(b, &b->rpn[p->rpn_at], p->nrpn, s, &p->v);
    p->mark = 2;
    p->gen = b->gen;
    *out = p->v;
    return 1;
}

int uxl_recalc(uxl_book *b)
{
    int s, i;
    if (!b) return 0;
    b->gen++;
    b->circular = 0;
    for (s = 0; s < b->nsheet; s++)
        for (i = 0; i < b->sheet[s].ncell; i++) uxl_nth(b, s, i)->mark = 0;
    for (s = 0; s < b->nsheet; s++)
        for (i = 0; i < b->sheet[s].ncell; i++) {
            uxl_cell *p = uxl_nth(b, s, i);
            uxl_val v;
            if (p->nrpn) eval_cell(b, s, p->row, p->col, &v);
        }
    b->dirty = 0;
    return !b->circular;
}
int uxl_circular(const uxl_book *b) { return b ? b->circular : 0; }
