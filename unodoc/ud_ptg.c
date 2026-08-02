/* ===========================================================================
 * ud_ptg.c - the BIFF8 formula decompiler: a ptg array back to "=SUM(A1:A9)".
 *
 * Excel stores a formula as reverse-Polish "parse thing" tokens, not text.
 * Rebuilding the text means running the RPN back through a stack of
 * fragments, each carrying the precedence of its top-level operator so that
 * parentheses can be re-inserted where - and only where - the tree requires
 * them.  Excel also records the parentheses the USER typed as an explicit
 * PtgParen, so "=(1+2)*3" comes back with its parentheses in the same place
 * the author put them rather than merely somewhere valid.
 *
 * Two things here are easy to get subtly wrong and are called out where they
 * happen: right-associativity (Excel's ^ is LEFT associative, so 2^3^2 is 64,
 * not 512, and the right operand of ^ - / needs parens at equal precedence),
 * and PtgRefN/PtgAreaN, whose row and column are OFFSETS from the cell that
 * owns the formula, not absolute positions - that is how one shared formula
 * serves a whole filled-down column.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_xls_int.h"
#include <string.h>

/* ---- precedence, low binds loosest ---------------------------------------- */
#define P_CMP     1     /* = <> <= >= < >          */
#define P_CAT     2     /* &                       */
#define P_ADD     3     /* + -                     */
#define P_MUL     4     /* * /                     */
#define P_POW     5     /* ^                       */
#define P_PCT     6     /* postfix %               */
#define P_NEG     7     /* unary - +               */
#define P_UNION   8     /* ,                       */
#define P_ISECT   9     /* space                   */
#define P_RANGE  10     /* :                       */
#define P_ATOM   20

#define STACKMAX 128

typedef struct { char *s; int prec; } frag;

typedef struct {
    frag  st[STACKMAX];
    int   sp;
    int   bad;                    /* something did not add up; give up      */
    const ud_ptg_env *env;
    int   base_row, base_col;     /* the cell owning the formula            */
    const unsigned char *extra;   /* rgbExtra, for PtgArray constants       */
    long  extra_n, extra_at;
} pstate;

/* ---- string helpers (no libc beyond mem-/str-) ---------------------------- */
static char *sdup(const char *a, long na, const char *b, long nb,
                  const char *c, long nc)
{
    char *r = (char *)ud_alloc((unsigned long)(na + nb + nc + 1));
    if (!r) return 0;
    if (na) memcpy(r, a, (unsigned long)na);
    if (nb) memcpy(r + na, b, (unsigned long)nb);
    if (nc) memcpy(r + na + nb, c, (unsigned long)nc);
    r[na + nb + nc] = 0;
    return r;
}
static char *s1(const char *a) { return sdup(a, (long)strlen(a), 0, 0, 0, 0); }

static void push(pstate *p, char *s, int prec)
{
    if (!s) { p->bad = 1; return; }
    if (p->sp >= STACKMAX) { p->bad = 1; ud_free(s); return; }
    p->st[p->sp].s = s;
    p->st[p->sp].prec = prec;
    p->sp++;
}

static int pop(pstate *p, frag *out)
{
    if (p->sp <= 0) { p->bad = 1; out->s = 0; out->prec = P_ATOM; return 0; }
    *out = p->st[--p->sp];
    return 1;
}

/* wrap `f` in parentheses if its top operator binds looser than `need` */
static char *paren_if(frag f, int need)
{
    char *r;
    if (!f.s) return 0;
    if (f.prec >= need) return f.s;
    r = sdup("(", 1, f.s, (long)strlen(f.s), ")", 1);
    ud_free(f.s);
    return r;
}

static void binop(pstate *p, const char *op, int prec, int right_needs_more)
{
    /* Initialised because a malformed token stream can underflow the stack:
       if the FIRST pop fails the second never runs, and the cleanup below
       would otherwise free whatever happened to be on the C stack.  Found by
       the workbook fuzzer, 2026-08-01. */
    frag a = { 0, P_ATOM }, b = { 0, P_ATOM };
    char *ls, *rs, *r;
    if (!pop(p, &b) || !pop(p, &a)) { ud_free(a.s); ud_free(b.s); return; }
    ls = paren_if(a, prec);
    /* -, /, ^ and the comparisons are left-associative, so an equal-
       precedence RIGHT operand really is parenthesised in the source:
       a-(b-c) is a different tree from a-b-c and must render differently. */
    rs = paren_if(b, right_needs_more ? prec + 1 : prec);
    if (!ls || !rs) { ud_free(ls); ud_free(rs); p->bad = 1; return; }
    r = sdup(ls, (long)strlen(ls), op, (long)strlen(op), rs, (long)strlen(rs));
    ud_free(ls); ud_free(rs);
    push(p, r, prec);
}

/* ---- A1-style references --------------------------------------------------- */
static int col_letters(int col, char *out)
{
    char tmp[4];
    int n = 0, len = 0;
    if (col < 0) col = 0;
    do { tmp[n++] = (char)('A' + (col % 26)); col = col / 26 - 1; }
    while (col >= 0 && n < 4);
    while (n) out[len++] = tmp[--n];
    return len;
}

/* One cell reference.  `rel` tokens (PtgRefN/PtgAreaN, which only appear in
 * shared formulas) carry SIGNED OFFSETS from the owning cell instead of
 * absolute positions. */
static int ref_text(pstate *p, int row, unsigned grbit, int rel_base, char *out)
{
    int col   = (int)(grbit & 0x3FFF);
    int colrel = (grbit & 0x4000) != 0;
    int rowrel = (grbit & 0x8000) != 0;
    int len = 0;

    if (rel_base) {
        if (colrel) col = p->base_col + (int)(signed char)(col & 0xFF);
        if (rowrel) row = p->base_row + (int)(short)(unsigned short)row;
    }
    if (row < 0 || col < 0 || row >= UD_XLS_MAXROW || col >= UD_XLS_MAXCOL) {
        memcpy(out, "#REF!", 5);
        out[5] = 0;
        return 5;
    }
    if (!colrel) out[len++] = '$';
    len += col_letters(col, out + len);
    if (!rowrel) out[len++] = '$';
    len += ud_int_text((long)row + 1, out + len);
    out[len] = 0;
    return len;
}

/* Quote a sheet name the way Excel does when it is not a bare identifier. */
static char *sheet_prefix(const char *name)
{
    long n, i;
    int need = 0;
    char *r;
    if (!name || !name[0]) return s1("");
    n = (long)strlen(name);
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              (unsigned char)c >= 0x80)) { need = 1; break; }
    }
    if (name[0] >= '0' && name[0] <= '9') need = 1;
    if (!need) return sdup(name, n, "!", 1, 0, 0);
    /* single quotes double inside a quoted sheet name */
    {
        long extra = 0;
        char *q;
        for (i = 0; i < n; i++) if (name[i] == '\'') extra++;
        q = (char *)ud_alloc((unsigned long)(n + extra + 4));
        if (!q) return 0;
        { long k = 0;
          q[k++] = '\'';
          for (i = 0; i < n; i++) {
              if (name[i] == '\'') q[k++] = '\'';
              q[k++] = name[i];
          }
          q[k++] = '\''; q[k++] = '!'; q[k] = 0; }
        r = q;
    }
    return r;
}

/* ---- the function table ([MS-XLS] Ftab) ------------------------------------
 * name, and the arity Excel uses when it emits the FIXED-argument form
 * (PtgFunc).  -1 = variable, which can only appear as PtgFuncVar, where the
 * count is in the token itself.  Gaps are the macro-sheet-only slots. */
typedef struct { const char *name; signed char args; } ftab_e;

static const ftab_e FTAB[] = {
/*  0 */ {"COUNT",-1},{"IF",-1},{"ISNA",1},{"ISERROR",1},{"SUM",-1},
/*  5 */ {"AVERAGE",-1},{"MIN",-1},{"MAX",-1},{"ROW",-1},{"COLUMN",-1},
/* 10 */ {"NA",0},{"NPV",-1},{"STDEV",-1},{"DOLLAR",-1},{"FIXED",-1},
/* 15 */ {"SIN",1},{"COS",1},{"TAN",1},{"ATAN",1},{"PI",0},
/* 20 */ {"SQRT",1},{"EXP",1},{"LN",1},{"LOG10",1},{"ABS",1},
/* 25 */ {"INT",1},{"SIGN",1},{"ROUND",2},{"LOOKUP",-1},{"INDEX",-1},
/* 30 */ {"REPT",2},{"MID",3},{"LEN",1},{"VALUE",1},{"TRUE",0},
/* 35 */ {"FALSE",0},{"AND",-1},{"OR",-1},{"NOT",1},{"MOD",2},
/* 40 */ {"DCOUNT",3},{"DSUM",3},{"DAVERAGE",3},{"DMIN",3},{"DMAX",3},
/* 45 */ {"DSTDEV",3},{"VAR",-1},{"DVAR",3},{"TEXT",2},{"LINEST",-1},
/* 50 */ {"TREND",-1},{"LOGEST",-1},{"GROWTH",-1},{"GOTO",-1},{"HALT",-1},
/* 55 */ {"RETURN",-1},{"PV",-1},{"FV",-1},{"NPER",-1},{"PMT",-1},
/* 60 */ {"RATE",-1},{"MIRR",3},{"IRR",-1},{"RAND",0},{"MATCH",-1},
/* 65 */ {"DATE",3},{"TIME",3},{"DAY",1},{"MONTH",1},{"YEAR",1},
/* 70 */ {"WEEKDAY",-1},{"HOUR",1},{"MINUTE",1},{"SECOND",1},{"NOW",0},
/* 75 */ {"AREAS",1},{"ROWS",1},{"COLUMNS",1},{"OFFSET",-1},{"ABSREF",2},
/* 80 */ {"RELREF",2},{"ARGUMENT",-1},{"SEARCH",-1},{"TRANSPOSE",1},{"ERROR",-1},
/* 85 */ {"STEP",0},{"TYPE",1},{"ECHO",-1},{"SET.NAME",-1},{"CALLER",0},
/* 90 */ {"DEREF",1},{"WINDOWS",-1},{"SERIES",4},{"DOCUMENTS",-1},{"ACTIVE.CELL",0},
/* 95 */ {"SELECTION",0},{"RESULT",-1},{"ATAN2",2},{"ASIN",1},{"ACOS",1},
/*100 */ {"CHOOSE",-1},{"HLOOKUP",-1},{"VLOOKUP",-1},{"LINKS",-1},{"INPUT",-1},
/*105 */ {"ISREF",1},{"GET.FORMULA",1},{"GET.NAME",-1},{"SET.VALUE",2},{"LOG",-1},
/*110 */ {"EXEC",-1},{"CHAR",1},{"LOWER",1},{"UPPER",1},{"PROPER",1},
/*115 */ {"LEFT",-1},{"RIGHT",-1},{"EXACT",2},{"TRIM",1},{"REPLACE",4},
/*120 */ {"SUBSTITUTE",-1},{"CODE",1},{"NAMES",-1},{"DIRECTORY",-1},{"FIND",-1},
/*125 */ {"CELL",-1},{"ISERR",1},{"ISTEXT",1},{"ISNUMBER",1},{"ISBLANK",1},
/*130 */ {"T",1},{"N",1},{"FOPEN",-1},{"FCLOSE",1},{"FSIZE",1},
/*135 */ {"FREADLN",1},{"FREAD",2},{"FWRITELN",2},{"FWRITE",2},{"FPOS",-1},
/*140 */ {"DATEVALUE",1},{"TIMEVALUE",1},{"SLN",3},{"SYD",4},{"DDB",-1},
/*145 */ {"GET.DEF",-1},{"REFTEXT",-1},{"TEXTREF",-1},{"INDIRECT",-1},{"REGISTER",-1},
/*150 */ {"CALL",-1},{"ADD.BAR",-1},{"ADD.MENU",-1},{"ADD.COMMAND",-1},{"ENABLE.COMMAND",-1},
/*155 */ {"CHECK.COMMAND",-1},{"RENAME.COMMAND",-1},{"SHOW.BAR",-1},{"DELETE.MENU",-1},{"DELETE.COMMAND",-1},
/*160 */ {"GET.CHART.ITEM",-1},{"DIALOG.BOX",1},{"CLEAN",1},{"MDETERM",1},{"MINVERSE",1},
/*165 */ {"MMULT",2},{"FILES",-1},{"IPMT",-1},{"PPMT",-1},{"COUNTA",-1},
/*170 */ {"CANCEL.KEY",-1},{"FOR",-1},{"WHILE",1},{"BREAK",0},{"NEXT",0},
/*175 */ {"INITIATE",2},{"REQUEST",2},{"POKE",3},{"EXECUTE",2},{"TERMINATE",1},
/*180 */ {"RESTART",-1},{"HELP",-1},{"GET.BAR",-1},{"PRODUCT",-1},{"FACT",1},
/*185 */ {"GET.CELL",-1},{"GET.WORKSPACE",1},{"GET.WINDOW",-1},{"GET.DOCUMENT",-1},{"DPRODUCT",3},
/*190 */ {"ISNONTEXT",1},{"GET.NOTE",-1},{"NOTE",-1},{"STDEVP",-1},{"VARP",-1},
/*195 */ {"DSTDEVP",3},{"DVARP",3},{"TRUNC",-1},{"ISLOGICAL",1},{"DCOUNTA",3},
/*200 */ {"DELETE.BAR",1},{"UNREGISTER",1},{0,-1},{0,-1},{"USDOLLAR",-1},
/*205 */ {"FINDB",-1},{"SEARCHB",-1},{"REPLACEB",4},{"LEFTB",-1},{"RIGHTB",-1},
/*210 */ {"MIDB",3},{"LENB",1},{"ROUNDUP",2},{"ROUNDDOWN",2},{"ASC",1},
/*215 */ {"DBCS",1},{"RANK",-1},{0,-1},{0,-1},{"ADDRESS",-1},
/*220 */ {"DAYS360",-1},{"TODAY",0},{"VDB",-1},{0,-1},{0,-1},
/*225 */ {0,-1},{0,-1},{"MEDIAN",-1},{"SUMPRODUCT",-1},{"SINH",1},
/*230 */ {"COSH",1},{"TANH",1},{"ASINH",1},{"ACOSH",1},{"ATANH",1},
/*235 */ {"DGET",3},{"CREATE.OBJECT",-1},{"VOLATILE",-1},{"LAST.ERROR",0},{"CUSTOM.UNDO",-1},
/*240 */ {"CUSTOM.REPEAT",-1},{"FORMULA.CONVERT",-1},{"GET.LINK.INFO",-1},{"TEXT.BOX",-1},{"INFO",1},
/*245 */ {"GROUP",0},{"GET.OBJECT",-1},{"DB",-1},{"PAUSE",-1},{0,-1},
/*250 */ {0,-1},{"RESUME",-1},{"FREQUENCY",2},{"ADD.TOOLBAR",-1},{"DELETE.TOOLBAR",-1},
/*255 */ {0,-1},  /* 255 = user-defined: the NAME is the first argument */
/*256 */ {"RESET.TOOLBAR",1},{"EVALUATE",1},{"GET.TOOLBAR",-1},{"GET.TOOL",-1},
/*260 */ {"SPELLING.CHECK",-1},{"ERROR.TYPE",1},{"APP.TITLE",-1},{"WINDOW.TITLE",-1},{"SAVE.TOOLBAR",-1},
/*265 */ {"ENABLE.TOOL",3},{"PRESS.TOOL",3},{"REGISTER.ID",-1},{"GET.WORKBOOK",-1},{"AVEDEV",-1},
/*270 */ {"BETADIST",-1},{"GAMMALN",1},{"BETAINV",-1},{"BINOMDIST",4},{"CHIDIST",2},
/*275 */ {"CHIINV",2},{"COMBIN",2},{"CONFIDENCE",3},{"CRITBINOM",3},{"EVEN",1},
/*280 */ {"EXPONDIST",3},{"FDIST",3},{"FINV",3},{"FISHER",1},{"FISHERINV",1},
/*285 */ {"FLOOR",2},{"GAMMADIST",4},{"GAMMAINV",3},{"CEILING",2},{"HYPGEOMDIST",4},
/*290 */ {"LOGNORMDIST",3},{"LOGINV",3},{"NEGBINOMDIST",3},{"NORMDIST",4},{"NORMSDIST",1},
/*295 */ {"NORMINV",3},{"NORMSINV",1},{"STANDARDIZE",3},{"ODD",1},{"PERMUT",2},
/*300 */ {"POISSON",3},{"TDIST",3},{"WEIBULL",4},{"SUMXMY2",2},{"SUMX2MY2",2},
/*305 */ {"SUMX2PY2",2},{"CHITEST",2},{"CORREL",2},{"COVAR",2},{"FORECAST",3},
/*310 */ {"FTEST",2},{"INTERCEPT",2},{"PEARSON",2},{"RSQ",2},{"STEYX",2},
/*315 */ {"SLOPE",2},{"TTEST",4},{"PROB",-1},{"DEVSQ",-1},{"GEOMEAN",-1},
/*320 */ {"HARMEAN",-1},{"SUMSQ",-1},{"KURT",-1},{"SKEW",-1},{"ZTEST",-1},
/*325 */ {"LARGE",2},{"SMALL",2},{"QUARTILE",2},{"PERCENTILE",2},{"PERCENTRANK",-1},
/*330 */ {"MODE",-1},{"TRIMMEAN",2},{"TINV",2},{0,-1},{"MOVIE.COMMAND",-1},
/*335 */ {"GET.MOVIE",-1},{"CONCATENATE",-1},{"POWER",2},{"PIVOT.ADD.DATA",-1},{"GET.PIVOT.TABLE",-1},
/*340 */ {"GET.PIVOT.FIELD",-1},{"GET.PIVOT.ITEM",-1},{"RADIANS",1},{"DEGREES",1},{"SUBTOTAL",-1},
/*345 */ {"SUMIF",-1},{"COUNTIF",2},{"COUNTBLANK",1},{"SCENARIO.GET",-1},{"OPTIONS.LISTS.GET",1},
/*350 */ {"ISPMT",4},{"DATEDIF",3},{"DATESTRING",1},{"NUMBERSTRING",2},{"ROMAN",-1},
/*355 */ {"OPEN.DIALOG",-1},{"SAVE.DIALOG",-1},{"VIEW.GET",-1},{"GETPIVOTDATA",-1},{"HYPERLINK",-1},
/*360 */ {"PHONETIC",1},{"AVERAGEA",-1},{"MAXA",-1},{"MINA",-1},{"STDEVPA",-1},
/*365 */ {"VARPA",-1},{"STDEVA",-1},{"VARA",-1}
};
#define NFTAB ((int)(sizeof FTAB / sizeof FTAB[0]))

static const char *ftab_name(int i)
{
    if (i >= 0 && i < NFTAB && FTAB[i].name) return FTAB[i].name;
    return 0;
}

/* Pop `n` fragments and render name(a,b,c).  The deepest is the first arg. */
static void call_func(pstate *p, const char *name, int n, int name_is_arg)
{
    frag args[64];
    long total, i;
    char *r;
    int k;

    if (n < 0 || n > 64) { p->bad = 1; return; }
    for (k = n - 1; k >= 0; k--)
        if (!pop(p, &args[k])) {
            for (k++; k < n; k++) ud_free(args[k].s);
            return;
        }
    if (name_is_arg) {
        /* iftab 255: the first argument IS the function name */
        if (n < 1) { p->bad = 1; return; }
        name = args[0].s ? args[0].s : "";
    }
    total = (long)strlen(name) + 2;
    for (i = 0; i < n; i++) total += (args[i].s ? (long)strlen(args[i].s) : 0) + 1;
    r = (char *)ud_alloc((unsigned long)total + 1);
    if (!r) { p->bad = 1; goto out; }
    {
        long at = (long)strlen(name);
        memcpy(r, name, (unsigned long)at);
        r[at++] = '(';
        for (i = name_is_arg ? 1 : 0; i < n; i++) {
            long ln = args[i].s ? (long)strlen(args[i].s) : 0;
            if (i > (name_is_arg ? 1 : 0)) r[at++] = ',';
            if (ln) { memcpy(r + at, args[i].s, (unsigned long)ln); at += ln; }
        }
        r[at++] = ')';
        r[at] = 0;
    }
    push(p, r, P_ATOM);
out:
    for (i = 0; i < n; i++) ud_free(args[i].s);
}

/* ---- array constants, which live in rgbExtra after the ptg array ---------- */
static char *array_const(pstate *p)
{
    long cols, rows, r, c;
    char *out = s1("{");
    const unsigned char *e = p->extra;

    if (!out) return 0;
    if (p->extra_at + 3 > p->extra_n) { p->bad = 1; return out; }
    cols = (long)e[p->extra_at] + 1;
    rows = (long)ud_rd16(e + p->extra_at + 1) + 1;
    p->extra_at += 3;
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            char buf[40];
            char *piece = 0, *joined;
            unsigned char t;
            if (p->extra_at >= p->extra_n) { p->bad = 1; return out; }
            t = e[p->extra_at++];
            if (t == 0x01) {                      /* number                */
                uint64_t bits;
                double d;
                if (p->extra_at + 8 > p->extra_n) { p->bad = 1; return out; }
                bits = ud_rd64(e + p->extra_at);
                p->extra_at += 8;
                memcpy(&d, &bits, 8);
                ud_num_text(d, buf);
                piece = s1(buf);
            } else if (t == 0x02) {               /* string                */
                long cch, i;
                int wide;
                if (p->extra_at + 3 > p->extra_n) { p->bad = 1; return out; }
                cch  = (long)ud_rd16(e + p->extra_at);
                wide = e[p->extra_at + 2] & 1;
                p->extra_at += 3;
                piece = (char *)ud_alloc((unsigned long)cch + 3);
                if (piece) {
                    long k = 0;
                    piece[k++] = '"';
                    for (i = 0; i < cch; i++) {
                        uint16_t u;
                        if (p->extra_at + (wide ? 2 : 1) > p->extra_n) break;
                        if (wide) { u = ud_rd16(e + p->extra_at); p->extra_at += 2; }
                        else      { u = ud_cp1252_to_uc(e[p->extra_at]); p->extra_at++; }
                        piece[k++] = (char)ud_uc_to_cp1252(u);
                    }
                    piece[k++] = '"';
                    piece[k] = 0;
                }
            } else if (t == 0x04) {               /* boolean               */
                if (p->extra_at + 8 > p->extra_n) { p->bad = 1; return out; }
                piece = s1(e[p->extra_at] ? "TRUE" : "FALSE");
                p->extra_at += 8;
            } else if (t == 0x10) {               /* error                 */
                if (p->extra_at + 8 > p->extra_n) { p->bad = 1; return out; }
                piece = s1(ud_xls_err_text(e[p->extra_at]));
                p->extra_at += 8;
            } else {                              /* empty                 */
                if (p->extra_at + 8 > p->extra_n) { p->bad = 1; return out; }
                piece = s1("");
                p->extra_at += 8;
            }
            joined = sdup(out, (long)strlen(out),
                          c ? "," : "", c ? 1 : 0,
                          piece ? piece : "", piece ? (long)strlen(piece) : 0);
            ud_free(out); ud_free(piece);
            out = joined;
            if (!out) { p->bad = 1; return 0; }
        }
        if (r + 1 < rows) {
            char *j = sdup(out, (long)strlen(out), ";", 1, 0, 0);
            ud_free(out); out = j;
            if (!out) { p->bad = 1; return 0; }
        }
    }
    { char *j = sdup(out, (long)strlen(out), "}", 1, 0, 0);
      ud_free(out); return j; }
}

/* ---- the walk --------------------------------------------------------------
 * Two passes are NOT needed: rgbExtra is consumed in ptg order, so a single
 * forward walk keeps both cursors in step. */
char *ud_ptg_text(const unsigned char *pt, long n, const ud_ptg_env *env,
                  int base_row, int base_col,
                  const unsigned char *extra, long extra_n)
{
    pstate P;
    long at = 0;
    char *result = 0;

    memset(&P, 0, sizeof P);
    P.env = env;
    P.base_row = base_row;
    P.base_col = base_col;
    P.extra = extra;
    P.extra_n = extra_n;

    while (at < n && !P.bad) {
        unsigned char t = pt[at++];
        unsigned char base = (unsigned char)(t >= 0x20 ? (t & 0x3F) | 0x20 : t);
        char buf[64];

        /* the classified tokens (value/reference/array flavours) share one
           encoding; the class does not change the TEXT, only how Excel
           evaluates it, so they are folded together here */
        switch (base) {
        case 0x03: binop(&P, "+", P_ADD, 0); break;
        case 0x04: binop(&P, "-", P_ADD, 1); break;
        case 0x05: binop(&P, "*", P_MUL, 0); break;
        case 0x06: binop(&P, "/", P_MUL, 1); break;
        case 0x07: binop(&P, "^", P_POW, 1); break;   /* left-associative   */
        case 0x08: binop(&P, "&", P_CAT, 0); break;
        case 0x09: binop(&P, "<",  P_CMP, 1); break;
        case 0x0A: binop(&P, "<=", P_CMP, 1); break;
        case 0x0B: binop(&P, "=",  P_CMP, 1); break;
        case 0x0C: binop(&P, ">=", P_CMP, 1); break;
        case 0x0D: binop(&P, ">",  P_CMP, 1); break;
        case 0x0E: binop(&P, "<>", P_CMP, 1); break;
        case 0x0F: binop(&P, " ",  P_ISECT, 0); break;
        case 0x10: binop(&P, ",",  P_UNION, 0); break;
        case 0x11: binop(&P, ":",  P_RANGE, 0); break;
        case 0x12: case 0x13: {                        /* unary + and -    */
            frag a;
            char *s, *r;
            if (!pop(&P, &a)) break;
            s = paren_if(a, P_NEG);
            if (!s) { P.bad = 1; break; }
            r = sdup(base == 0x12 ? "+" : "-", 1, s, (long)strlen(s), 0, 0);
            ud_free(s);
            push(&P, r, P_NEG);
            break;
        }
        case 0x14: {                                   /* postfix %        */
            frag a;
            char *s, *r;
            if (!pop(&P, &a)) break;
            s = paren_if(a, P_PCT);
            if (!s) { P.bad = 1; break; }
            r = sdup(s, (long)strlen(s), "%", 1, 0, 0);
            ud_free(s);
            push(&P, r, P_PCT);
            break;
        }
        case 0x15: {                                   /* the user's parens */
            frag a;
            char *r;
            if (!pop(&P, &a)) break;
            if (!a.s) { P.bad = 1; break; }
            r = sdup("(", 1, a.s, (long)strlen(a.s), ")", 1);
            ud_free(a.s);
            push(&P, r, P_ATOM);
            break;
        }
        case 0x16: push(&P, s1(""), P_ATOM); break;    /* missing argument */
        case 0x17: {                                   /* string literal   */
            long cch, i, k = 0;
            int wide;
            char *s;
            if (at + 2 > n) { P.bad = 1; break; }
            cch  = pt[at];
            wide = pt[at + 1] & 1;
            at += 2;
            s = (char *)ud_alloc((unsigned long)cch * 2 + 3);
            if (!s) { P.bad = 1; break; }
            s[k++] = '"';
            for (i = 0; i < cch; i++) {
                uint16_t u;
                if (at + (wide ? 2 : 1) > n) { P.bad = 1; break; }
                if (wide) { u = ud_rd16(pt + at); at += 2; }
                else      { u = ud_cp1252_to_uc(pt[at]); at++; }
                /* a quote inside a literal is doubled in the source text */
                if (u == '"') s[k++] = '"';
                s[k++] = (char)ud_uc_to_cp1252(u);
            }
            s[k++] = '"';
            s[k] = 0;
            push(&P, s, P_ATOM);
            break;
        }
        case 0x19: {                                   /* PtgAttr          */
            unsigned char fl;
            unsigned data;
            if (at + 3 > n) { P.bad = 1; break; }
            fl   = pt[at];
            data = ud_rd16(pt + at + 1);
            at += 3;
            if (fl & 0x04) at += ((long)data + 1) * 2;  /* CHOOSE jump table */
            if (fl & 0x10) {                            /* the one-arg SUM  */
                frag a;
                char *r;
                if (!pop(&P, &a)) break;
                if (!a.s) { P.bad = 1; break; }
                r = sdup("SUM(", 4, a.s, (long)strlen(a.s), ")", 1);
                ud_free(a.s);
                push(&P, r, P_ATOM);
            }
            break;                                      /* space/if/goto:  */
        }                                               /* no text effect  */
        case 0x1C: {                                   /* error literal    */
            if (at >= n) { P.bad = 1; break; }
            push(&P, s1(ud_xls_err_text(pt[at])), P_ATOM);
            at++;
            break;
        }
        case 0x1D:
            if (at >= n) { P.bad = 1; break; }
            push(&P, s1(pt[at] ? "TRUE" : "FALSE"), P_ATOM);
            at++;
            break;
        case 0x1E:
            if (at + 2 > n) { P.bad = 1; break; }
            ud_int_text((long)ud_rd16(pt + at), buf);
            at += 2;
            push(&P, s1(buf), P_ATOM);
            break;
        case 0x1F: {
            uint64_t bits;
            double d;
            if (at + 8 > n) { P.bad = 1; break; }
            bits = ud_rd64(pt + at);
            at += 8;
            memcpy(&d, &bits, 8);
            ud_num_text(d, buf);
            push(&P, s1(buf), P_ATOM);
            break;
        }
        case 0x20: {                                   /* array constant   */
            if (at + 7 > n) { P.bad = 1; break; }
            at += 7;
            push(&P, array_const(&P), P_ATOM);
            break;
        }
        case 0x21: {                                   /* fixed-arity call */
            int ift;
            const char *nm;
            if (at + 2 > n) { P.bad = 1; break; }
            ift = (int)ud_rd16(pt + at);
            at += 2;
            nm = ftab_name(ift);
            if (!nm || FTAB[ift].args < 0) { P.bad = 1; break; }
            call_func(&P, nm, FTAB[ift].args, 0);
            break;
        }
        case 0x22: {                                   /* variable call    */
            int cp, ift;
            const char *nm;
            if (at + 3 > n) { P.bad = 1; break; }
            cp  = pt[at] & 0x7F;
            ift = (int)(ud_rd16(pt + at + 1) & 0x7FFF);
            at += 3;
            if (ift == 255) { call_func(&P, "", cp, 1); break; }
            nm = ftab_name(ift);
            if (!nm) { P.bad = 1; break; }
            call_func(&P, nm, cp, 0);
            break;
        }
        case 0x23: {                                   /* defined name     */
            int idx;
            const char *nm;
            if (at + 4 > n) { P.bad = 1; break; }
            idx = (int)ud_rd16(pt + at);
            at += 4;
            nm = env && env->name_of ? env->name_of(env->book, idx - 1) : 0;
            push(&P, s1(nm ? nm : "#NAME?"), P_ATOM);
            break;
        }
        case 0x24: case 0x2C: {                        /* PtgRef / PtgRefN */
            int rel = (base == 0x2C);
            if (at + 4 > n) { P.bad = 1; break; }
            ref_text(&P, (int)ud_rd16(pt + at), ud_rd16(pt + at + 2), rel, buf);
            at += 4;
            push(&P, s1(buf), P_ATOM);
            break;
        }
        case 0x25: case 0x2D: {                        /* area / areaN     */
            char a[32], b[32];
            int rel = (base == 0x2D);
            if (at + 8 > n) { P.bad = 1; break; }
            ref_text(&P, (int)ud_rd16(pt + at),     ud_rd16(pt + at + 4), rel, a);
            ref_text(&P, (int)ud_rd16(pt + at + 2), ud_rd16(pt + at + 6), rel, b);
            at += 8;
            push(&P, sdup(a, (long)strlen(a), ":", 1, b, (long)strlen(b)), P_ATOM);
            break;
        }
        case 0x26: case 0x27: case 0x28:               /* Mem* markers:    */
            at += 6;                                   /* the subexpression*/
            break;                                     /* follows inline   */
        case 0x29:
            at += 2;
            break;
        case 0x2A:                                     /* deleted ref      */
            at += 4;
            push(&P, s1("#REF!"), P_ATOM);
            break;
        case 0x2B:
            at += 8;
            push(&P, s1("#REF!"), P_ATOM);
            break;
        case 0x39: {                                   /* external name    */
            int ixti, idx;
            const char *nm;
            if (at + 6 > n) { P.bad = 1; break; }
            ixti = (int)ud_rd16(pt + at);
            idx  = (int)ud_rd16(pt + at + 2);
            at += 6;
            nm = env && env->extname_of ? env->extname_of(env->book, ixti, idx - 1) : 0;
            push(&P, s1(nm ? nm : "#NAME?"), P_ATOM);
            break;
        }
        case 0x3A: case 0x3C: {                        /* 3-D cell ref     */
            const char *sh;
            char *pre, *r;
            if (at + 6 > n) { P.bad = 1; break; }
            sh = env && env->xti_name
                 ? env->xti_name(env->book, (int)ud_rd16(pt + at)) : 0;
            if (base == 0x3C) { memcpy(buf, "#REF!", 6); }
            else ref_text(&P, (int)ud_rd16(pt + at + 2),
                          ud_rd16(pt + at + 4), 0, buf);
            at += 6;
            pre = sheet_prefix(sh ? sh : "#REF");
            if (!pre) { P.bad = 1; break; }
            r = sdup(pre, (long)strlen(pre), buf, (long)strlen(buf), 0, 0);
            ud_free(pre);
            push(&P, r, P_ATOM);
            break;
        }
        case 0x3B: case 0x3D: {                        /* 3-D area ref     */
            const char *sh;
            char a[32], b[32], *pre, *r;
            if (at + 10 > n) { P.bad = 1; break; }
            sh = env && env->xti_name
                 ? env->xti_name(env->book, (int)ud_rd16(pt + at)) : 0;
            if (base == 0x3D) { memcpy(a, "#REF!", 6); memcpy(b, "#REF!", 6); }
            else {
                ref_text(&P, (int)ud_rd16(pt + at + 2), ud_rd16(pt + at + 6), 0, a);
                ref_text(&P, (int)ud_rd16(pt + at + 4), ud_rd16(pt + at + 8), 0, b);
            }
            at += 10;
            pre = sheet_prefix(sh ? sh : "#REF");
            if (!pre) { P.bad = 1; break; }
            r = sdup(pre, (long)strlen(pre), a, (long)strlen(a), ":", 1);
            ud_free(pre);
            if (r) {
                char *r2 = sdup(r, (long)strlen(r), b, (long)strlen(b), 0, 0);
                ud_free(r);
                r = r2;
            }
            push(&P, r, P_ATOM);
            break;
        }
        case 0x01:                                     /* PtgExp: shared   */
        case 0x02:                                     /* PtgTbl           */
            /* handled by the caller before we ever get here */
            P.bad = 1;
            break;
        default:
            P.bad = 1;
            break;
        }
    }

    if (!P.bad && P.sp == 1 && P.st[0].s) {
        result = sdup("=", 1, P.st[0].s, (long)strlen(P.st[0].s), 0, 0);
    }
    { int i; for (i = 0; i < P.sp; i++) ud_free(P.st[i].s); }
    return result;
}

/* Is this ptg array just a pointer at a shared formula?  ([MS-XLS]: a
 * fill-down column stores the expression ONCE and every cell after the first
 * carries only this.) */
int ud_ptg_is_exp(const unsigned char *pt, long n, int *row, int *col)
{
    if (n < 5 || pt[0] != 0x01) return 0;
    if (row) *row = (int)ud_rd16(pt + 1);
    if (col) *col = (int)ud_rd16(pt + 3);
    return 1;
}
