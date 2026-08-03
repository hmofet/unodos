/* ===========================================================================
 * uocalc_test - the host gate for UnoCalc's engine (OFFICE97-PLAN §6 §9).
 *
 * A FIXTURE TABLE of formulas with the answer Excel 97 gives, which is the
 * only kind of assertion worth making about a calculator: the expected
 * values are written from Excel's documented behaviour, NOT from what this
 * implementation happens to produce, so agreement means something.
 *
 * Then: operator precedence and associativity (Excel's ^ is LEFT-
 * associative, so 2^3^2 is 64 - the same trap unodoc's ptg decompiler
 * documents), recalculation order, circular-reference detection, the sparse
 * store's ordering, and the number-format language.
 * ======================================================================== */
#include "uocalc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail, g_checks;

static void fail(const char *what, const char *detail)
{ printf("  FAIL %s: %s\n", what, detail); g_fail++; }

static int near(double a, double b)
{
    double d = a - b;
    if (d < 0) d = -d;
    if (b < 0) b = -b;
    return d <= (b > 1 ? b * 1e-9 : 1e-9);
}

/* ---- the fixture table ------------------------------------------------------
 * Every row: a formula, and what Excel 97 answers.  `S` rows expect a
 * string, `E` rows an error, `N` rows a number. */
typedef struct { const char *f; char kind; double num; const char *str; } fx;

static const fx kFix[] = {
    /* arithmetic and precedence */
    { "=1+2*3",            'N', 7, 0 },
    { "=(1+2)*3",          'N', 9, 0 },
    { "=2^3^2",            'N', 64, 0 },     /* LEFT-associative in Excel   */
    { "=-2^2",             'N', 4, 0 },      /* unary minus binds first     */
    { "=10/4",             'N', 2.5, 0 },
    { "=10/0",             'E', UXL_E_DIV0, 0 },
    { "=50%",              'N', 0.5, 0 },
    { "=2+3%",             'N', 2.03, 0 },
    { "=1<2",              'N', 1, 0 },
    { "=2<=2",             'N', 1, 0 },
    { "=3<>3",             'N', 0, 0 },
    /* text */
    { "=\"a\"&\"b\"",      'S', 0, "ab" },
    { "=LEN(\"hello\")",   'N', 5, 0 },
    { "=UPPER(\"abc\")",   'S', 0, "ABC" },
    { "=LOWER(\"ABC\")",   'S', 0, "abc" },
    { "=LEFT(\"abcdef\",3)",  'S', 0, "abc" },
    { "=RIGHT(\"abcdef\",2)", 'S', 0, "ef" },
    { "=MID(\"abcdef\",2,3)", 'S', 0, "bcd" },
    { "=TRIM(\"  a  b  \")", 'S', 0, "a b" },
    { "=REPT(\"ab\",3)",   'S', 0, "ababab" },
    { "=CONCATENATE(\"a\",\"b\",\"c\")", 'S', 0, "abc" },
    { "=EXACT(\"a\",\"A\")", 'N', 0, 0 },
    { "=FIND(\"cd\",\"abcdef\")", 'N', 3, 0 },
    { "=VALUE(\"12.5\")",  'N', 12.5, 0 },
    { "=CHAR(65)",         'S', 0, "A" },
    { "=CODE(\"A\")",      'N', 65, 0 },
    /* maths */
    { "=ABS(-3)",          'N', 3, 0 },
    { "=SQRT(16)",         'N', 4, 0 },
    { "=SQRT(-1)",         'E', UXL_E_NUM, 0 },
    { "=POWER(2,10)",      'N', 1024, 0 },
    { "=MOD(10,3)",        'N', 1, 0 },
    { "=MOD(-10,3)",       'N', 2, 0 },      /* Excel's sign follows the divisor */
    { "=INT(-2.5)",        'N', -3, 0 },
    { "=ROUND(2.345,2)",   'N', 2.35, 0 },
    { "=ROUND(-2.345,2)",  'N', -2.35, 0 },
    { "=ROUNDDOWN(2.99,0)",'N', 2, 0 },
    { "=ROUNDUP(2.01,0)",  'N', 3, 0 },
    { "=SIGN(-7)",         'N', -1, 0 },
    { "=EXP(0)",           'N', 1, 0 },
    { "=LN(1)",            'N', 0, 0 },
    { "=LOG10(1000)",      'N', 3, 0 },
    { "=PI()",             'N', 3.14159265358979, 0 },
    /* logic */
    { "=IF(1>0,10,20)",    'N', 10, 0 },
    { "=IF(1>2,10,20)",    'N', 20, 0 },
    { "=AND(1,1,0)",       'N', 0, 0 },
    { "=OR(0,0,1)",        'N', 1, 0 },
    { "=NOT(0)",           'N', 1, 0 },
    { "=TRUE",             'N', 1, 0 },
    { "=ISNUMBER(1)",      'N', 1, 0 },
    { "=ISTEXT(\"a\")",    'N', 1, 0 },
    { "=NA()",             'E', UXL_E_NA, 0 },
    { "=ISNA(NA())",       'N', 1, 0 },
    /* unknown names are #NAME?, exactly as Excel says */
    { "=NOSUCHFUNC(1)",    'E', UXL_E_NAME, 0 },
    /* financial */
    { "=PMT(0,10,1000)",   'N', -100, 0 },
    { "=FV(0,10,-100)",    'N', 1000, 0 },
    { "=PV(0,10,-100)",    'N', 1000, 0 }
};
#define NFIX ((int)(sizeof kFix / sizeof kFix[0]))

/* ---- range fixtures, over a sheet the test lays out ------------------------- */
typedef struct { const char *f; char kind; double num; const char *str; } rfx;

static const rfx kRange[] = {
    { "=SUM(A1:A5)",         'N', 15, 0 },
    { "=AVERAGE(A1:A5)",     'N', 3, 0 },
    { "=MIN(A1:A5)",         'N', 1, 0 },
    { "=MAX(A1:A5)",         'N', 5, 0 },
    { "=COUNT(A1:A6)",       'N', 5, 0 },    /* the text cell is not counted */
    { "=COUNTA(A1:A6)",      'N', 6, 0 },    /* but it IS non-empty          */
    { "=COUNTBLANK(A1:A8)",  'N', 2, 0 },
    { "=PRODUCT(A1:A3)",     'N', 6, 0 },
    { "=MEDIAN(A1:A5)",      'N', 3, 0 },
    { "=SUM(A1:A5)/COUNT(A1:A5)", 'N', 3, 0 },
    { "=SUMIF(A1:A5,\">2\")",'N', 12, 0 },
    { "=COUNTIF(A1:A5,\">=3\")", 'N', 3, 0 },
    { "=LARGE(A1:A5,2)",     'N', 4, 0 },
    { "=SMALL(A1:A5,2)",     'N', 2, 0 },
    { "=RANK(4,A1:A5)",      'N', 2, 0 },
    { "=ROWS(A1:A5)",        'N', 5, 0 },
    { "=COLUMNS(A1:C1)",     'N', 3, 0 },
    { "=INDEX(A1:A5,3)",     'N', 3, 0 },
    { "=MATCH(4,A1:A5)",     'N', 4, 0 },
    { "=VLOOKUP(3,A1:B5,2)", 'S', 0, "three" },
    { "=STDEV(A1:A5)",       'N', 1.58113883008419, 0 },
    { "=VAR(A1:A5)",         'N', 2.5, 0 }
};
#define NRANGE ((int)(sizeof kRange / sizeof kRange[0]))

/* ---- number-format fixtures -------------------------------------------------- */
typedef struct { double v; const char *code; const char *want; } ffx;
static const ffx kFmt[] = {
    { 1234.5678, "General",    "1234.5678" },
    { 0.1,       "General",    "0.1" },
    { 1234.5678, "0",          "1235" },
    { 1234.5678, "0.00",       "1234.57" },
    { 1234.5678, "#,##0",      "1,235" },
    { 1234.5678, "#,##0.00",   "1,234.57" },
    { 0.5,       "0%",         "50%" },
    { 0.12345,   "0.00%",      "12.35%" },
    { 1234.5,    "$#,##0.00",  "$1,234.50" },
    { -1234.5,   "#,##0.00",   "-1,234.50" },
    { 0.995,     "0.00",       "1.00" },      /* rounds before it prints     */
    { 1000000,   "#,##0",      "1,000,000" },
    /* serial 35562: 1997-01-01 is 35431, +131 days = 12 May.  The first
     * version of this row said June - the engine was right and the
     * FIXTURE was wrong, which is the failure mode a fixture table is
     * supposed to have (it is checkable). */
    { 35562,     "d-mmm-yy",   "12-May-97" },
    { 0,         "General",    "0" }
};
#define NFMT ((int)(sizeof kFmt / sizeof kFmt[0]))

static void run_one(uxl_book *b, const char *f, char kind, double num,
                    const char *str, const char *where)
{
    uxl_val v;
    char b1[160];
    g_checks++;
    if (!uxl_set_formula(b, 0, 20, 0, f)) {
        sprintf(b1, "%s did not compile", f);
        fail(where, b1);
        return;
    }
    uxl_recalc(b);
    uxl_get(b, 0, 20, 0, &v);
    if (kind == 'N') {
        if (v.kind == UXL_ERR) {
            sprintf(b1, "%s gave %s, wanted %g", f, uxl_err_text(v.err), num);
            fail(where, b1);
        } else if (!near(v.num, num)) {
            sprintf(b1, "%s gave %.15g, wanted %.15g", f, v.num, num);
            fail(where, b1);
        }
    } else if (kind == 'E') {
        if (v.kind != UXL_ERR || v.err != (int)num) {
            sprintf(b1, "%s gave kind %d err %d, wanted %s",
                    f, v.kind, v.err, uxl_err_text((int)num));
            fail(where, b1);
        }
    } else {
        const char *got = (v.kind == UXL_STR) ? uxl_pool(b, v.str) : "<not text>";
        if (strcmp(got, str) != 0) {
            sprintf(b1, "%s gave \"%s\", wanted \"%s\"", f, got, str);
            fail(where, b1);
        }
    }
}

int main(void)
{
    uxl_book *b = uxl_new();
    int i;
    char out[128];

    printf("uocalc engine gate\n");
    printf("  %d functions implemented\n", uxl_func_count());

    /* ---- the sparse store -------------------------------------------- */
    uxl_set_num(b, 0, 5, 2, 42);
    uxl_set_num(b, 0, 1, 1, 7);
    uxl_set_num(b, 0, 9, 0, 1);
    {
        int r, c, n = uxl_count(b, 0), last = -1, bad = 0;
        uxl_val v;
        g_checks++;
        if (n != 3) fail("store", "three cells were set");
        for (i = 0; i < n; i++) {
            uxl_at(b, 0, i, &r, &c, &v);
            if (r * 256 + c <= last) bad++;
            last = r * 256 + c;
        }
        g_checks++;
        if (bad) fail("store", "the walk is not in row-major order");
        uxl_get(b, 0, 5, 2, &v);
        g_checks++;
        if (v.kind != UXL_NUM || v.num != 42) fail("store", "lookup lost a value");
        uxl_clear(b, 0, 5, 2);
        g_checks++;
        if (uxl_count(b, 0) != 2) fail("store", "clear did not remove it");
    }

    /* ---- A1 round-trip ------------------------------------------------ */
    {
        int r, c, ar, ac;
        g_checks++;
        if (!uxl_a1_parse("B3", &r, &c, &ar, &ac) || r != 2 || c != 1)
            fail("a1", "B3 is row 2 column 1");
        g_checks++;
        if (!uxl_a1_parse("$AA$100", &r, &c, &ar, &ac) || c != 26 || r != 99 ||
            !ar || !ac)
            fail("a1", "$AA$100 is absolute, column 26");
        uxl_a1_write(2, 1, 0, 0, out, (int)sizeof out);
        g_checks++;
        if (strcmp(out, "B3")) fail("a1", "write did not round-trip B3");
        uxl_a1_write(99, 26, 1, 1, out, (int)sizeof out);
        g_checks++;
        if (strcmp(out, "$AA$100")) fail("a1", "write lost the absolutes");
    }

    /* ---- the scalar fixture table ------------------------------------- */
    for (i = 0; i < NFIX; i++)
        run_one(b, kFix[i].f, kFix[i].kind, kFix[i].num, kFix[i].str, "formula");
    printf("  %d scalar formulas checked\n", NFIX);

    /* ---- ranges, over a laid-out sheet -------------------------------- */
    uxl_set_num(b, 0, 0, 0, 1);
    uxl_set_num(b, 0, 1, 0, 2);
    uxl_set_num(b, 0, 2, 0, 3);
    uxl_set_num(b, 0, 3, 0, 4);
    uxl_set_num(b, 0, 4, 0, 5);
    uxl_set_str(b, 0, 5, 0, "text");
    uxl_set_str(b, 0, 0, 1, "one");
    uxl_set_str(b, 0, 1, 1, "two");
    uxl_set_str(b, 0, 2, 1, "three");
    uxl_set_str(b, 0, 3, 1, "four");
    uxl_set_str(b, 0, 4, 1, "five");
    for (i = 0; i < NRANGE; i++)
        run_one(b, kRange[i].f, kRange[i].kind, kRange[i].num, kRange[i].str,
                "range");
    printf("  %d range formulas checked\n", NRANGE);

    /* ---- recalculation order ------------------------------------------ */
    {
        uxl_val v;
        uxl_set_num(b, 0, 30, 0, 2);                 /* A31 = 2            */
        uxl_set_formula(b, 0, 31, 0, "=A31*10");     /* A32 depends on A31 */
        uxl_set_formula(b, 0, 32, 0, "=A32+5");      /* A33 on A32         */
        uxl_recalc(b);
        uxl_get(b, 0, 32, 0, &v);
        g_checks++;
        if (!near(v.num, 25)) {
            sprintf(out, "A33 is %g, wanted 25", v.num);
            fail("recalc", out);
        }
        /* change the root: everything downstream must follow */
        uxl_set_num(b, 0, 30, 0, 3);
        uxl_recalc(b);
        uxl_get(b, 0, 32, 0, &v);
        g_checks++;
        if (!near(v.num, 35)) {
            sprintf(out, "after the edit A33 is %g, wanted 35", v.num);
            fail("recalc", out);
        }
        g_checks++;
        if (uxl_circular(b)) fail("recalc", "a clean sheet reported circular");
    }

    /* ---- a circular reference is detected, not a hang ----------------- */
    {
        uxl_book *c = uxl_new();
        int clean;
        uxl_set_formula(c, 0, 0, 0, "=B1+1");
        uxl_set_formula(c, 0, 0, 1, "=A1+1");
        clean = uxl_recalc(c);
        g_checks++;
        if (clean) fail("circular", "the cycle was not reported");
        g_checks++;
        if (!uxl_circular(c)) fail("circular", "the flag was not set");
    }

    /* ---- number formats ------------------------------------------------ */
    b = uxl_new();
    for (i = 0; i < NFMT; i++) {
        g_checks++;
        uxl_format(kFmt[i].v, kFmt[i].code, out, (int)sizeof out);
        if (strcmp(out, kFmt[i].want)) {
            char b1[400];
            sprintf(b1, "%.10g through \"%s\" gave \"%s\", wanted \"%s\"",
                    kFmt[i].v, kFmt[i].code, out, kFmt[i].want);
            fail("numfmt", b1);
        }
    }
    printf("  %d number formats checked\n", NFMT);

    /* a cell's DISPLAY goes through its format, not its value */
    {
        uxl_set_num(b, 0, 0, 0, 0.5);
        uxl_set_fmt(b, 0, 0, 0, UXL_FMT_PCT);
        uxl_text(b, 0, 0, 0, out, (int)sizeof out);
        g_checks++;
        if (strcmp(out, "50%")) {
            char b1[256];
            sprintf(b1, "0.5 with a percent format shows \"%s\"", out);
            fail("display", b1);
        }
    }

    /* ---- the shared pools --------------------------------------------------
     * The store keeps ONE cell pool and ONE token pool for the whole book
     * (uocalc.h: per-sheet arrays made the module 104 MB against a 4 MB
     * module arena).  Overwriting a formula leaks its tokens, so the pool
     * fills and is REBUILT by recompiling every live formula.  Rewrite one
     * cell far more times than the pool holds, then check that the book -
     * this cell AND its untouched neighbours - still computes. */
    {
        int k;
        uxl_val v;
        uxl_book *pb = uxl_new();
        uxl_set_num(pb, 0, 0, 0, 6);            /* A1 = 6                   */
        uxl_set_num(pb, 0, 1, 0, 7);            /* A2 = 7                   */
        uxl_set_formula(pb, 0, 2, 0, "=A1*A2");  /* A3, never touched again  */
        for (k = 0; k < 4000; k++) {            /* >> UXL_RPNPOOL tokens    */
            char f[64];
            sprintf(f, "=A1+A2+%d", k);
            uxl_set_formula(pb, 0, 3, 0, f);     /* A4, rewritten each time  */
        }
        uxl_recalc(pb);
        g_checks++;
        if (!uxl_get(pb, 0, 2, 0, &v) || v.kind != UXL_NUM || v.num != 42) {
            char b1[160];
            sprintf(b1, "A3 after %d rewrites is kind %d = %.10g, wanted 42",
                    4000, v.kind, v.num);
            fail("rpn pool rebuild", b1);
        }
        g_checks++;
        if (!uxl_get(pb, 0, 3, 0, &v) || v.kind != UXL_NUM || v.num != 13 + 3999) {
            char b1[160];
            sprintf(b1, "A4 is kind %d = %.10g, wanted %d",
                    v.kind, v.num, 13 + 3999);
            fail("rpn pool rebuild", b1);
        }
        /* the cell pool recycles: fill it, clear it, fill it again */
        for (k = 0; k < UXL_MAXCELL - 4; k++)
            uxl_set_num(pb, 1, k, 0, k);
        g_checks++;
        if (uxl_count(pb, 1) != UXL_MAXCELL - 4)
            fail("cell pool", "sheet 2 did not take its share of the pool");
        for (k = 0; k < UXL_MAXCELL - 4; k++) uxl_clear(pb, 1, k, 0);
        for (k = 0; k < UXL_MAXCELL - 4; k++) uxl_set_num(pb, 2, k, 0, k);
        g_checks++;
        if (uxl_count(pb, 2) != UXL_MAXCELL - 4)
            fail("cell pool", "cleared cells were not returned to the pool");
        printf("  shared cell + token pools: rebuild and recycle checked\n");
    }

    printf(g_fail ? "\nuocalc gate: %d FAILURE(S) in %d checks\n"
                  : "\nuocalc gate: GREEN (%d checks)\n",
           g_fail ? g_fail : g_checks, g_checks);
    return g_fail ? 1 : 0;
}
