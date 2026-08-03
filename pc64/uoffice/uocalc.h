/* ===========================================================================
 * uocalc - UnoCalc's workbook, calculation engine and number formats.
 *                                                          [EXPERIMENTAL]
 * (OFFICE97-PLAN §6 phase 9; conformance items OFFICE97-SPEC.md S-UOC-04,
 * S-UOC-05 and S-UOC-06.)
 *
 * ONE DEVIATION FROM THE PLAN, on purpose: the plan reserved `uoc_*` for
 * UnoCalc, but uochrome landed first and owns that prefix (46 uses in its
 * header alone).  UnoCalc is `uxl_*` - Uno's eXceL - and the files are
 * uxl_sheet.c / uxl_calc.c / uxl_numfmt.c.  Renaming the chrome instead
 * would have been a shared-surface churn for no gain.
 *
 * THE MODEL MIRRORS BIFF8 DELIBERATELY.  A cell is a tagged value plus an
 * optional formula, exactly as unodoc's .xls reader hands one back, and the
 * formula compiles to a postfix token stream shaped like Excel's ptgs.  That
 * is not decoration: it makes the unodoc conversion mechanical in both
 * directions, which is the whole reason the spreadsheet lane can open and
 * save real workbooks at all.
 * ======================================================================== */
#ifndef UOCALC_H
#define UOCALC_H

/* ---- values ----------------------------------------------------------------
 * Excel's five: a number (IEEE double, dates are serials), a string, a
 * boolean, an error, and empty - which is NOT zero, because COUNT and an
 * empty cell in an average both depend on the difference. */
enum { UXL_EMPTY = 0, UXL_NUM, UXL_STR, UXL_BOOL, UXL_ERR };

/* Excel 97's seven error values, in its own order. */
enum { UXL_E_NULL = 1, UXL_E_DIV0, UXL_E_VALUE, UXL_E_REF,
       UXL_E_NAME, UXL_E_NUM, UXL_E_NA };

typedef struct {
    int    kind;      /* UXL_*                                              */
    double num;       /* UXL_NUM, and UXL_BOOL as 0/1                       */
    int    str;       /* UXL_STR: an index into the book's string pool      */
    int    err;       /* UXL_ERR: a UXL_E_*                                 */
} uxl_val;

const char *uxl_err_text(int err);        /* "#DIV/0!" etc.                 */

/* ---- the workbook ---------------------------------------------------------- */
#define UXL_MAXSHEET 8
/* Live cells are drawn from ONE pool shared by every sheet, and each sheet
 * keeps a sorted array of indices into it.  Per-sheet cell arrays looked
 * tidier and cost UXL_MAXSHEET times the memory whether the sheets were used
 * or not - at 4096 cells a sheet that was a 104 MB module, against a 4 MB
 * module arena (pc64_modload.c MOD_ARENA_PAGES).  Sharing the pool means one
 * sheet may hold all of it, which is also how people actually use a workbook. */
#define UXL_MAXCELL  2048          /* live cells per WORKBOOK                */
#define UXL_MAXSTR   768
#define UXL_STRLEN   48
#define UXL_FMLLEN   96
#define UXL_MAXNAME  32            /* defined names                          */
#define UXL_ROWS     65536         /* Excel 97's grid, and the reason        */
#define UXL_COLS     256           /* the store has to be sparse             */

typedef struct uxl_book uxl_book;

uxl_book *uxl_new(void);
void      uxl_free(uxl_book *b);

int         uxl_sheet_add(uxl_book *b, const char *name);
int         uxl_sheets(const uxl_book *b);
const char *uxl_sheet_name(const uxl_book *b, int s);
int         uxl_sheet_find(const uxl_book *b, const char *name);

/* ---- cells -----------------------------------------------------------------
 * The store is SPARSE: a sorted array of live cells per sheet, binary
 * searched by (row, col).  A 65536 x 256 grid is 16.7 million cells and a
 * real worksheet has a few hundred, so anything dense is a non-starter -
 * that is the same reason BIFF8 stores rows as records rather than as a
 * rectangle. */
int  uxl_set_num  (uxl_book *b, int s, int r, int c, double v);
int  uxl_set_str  (uxl_book *b, int s, int r, int c, const char *t);
int  uxl_set_bool (uxl_book *b, int s, int r, int c, int on);
int  uxl_set_err  (uxl_book *b, int s, int r, int c, int err);
/* `text` includes the leading '=' or not; it is compiled immediately, so a
 * syntax error is reported here rather than at the next recalc. */
int  uxl_set_formula(uxl_book *b, int s, int r, int c, const char *text);
int  uxl_clear    (uxl_book *b, int s, int r, int c);

int  uxl_get(const uxl_book *b, int s, int r, int c, uxl_val *out);
const char *uxl_formula(const uxl_book *b, int s, int r, int c);
const char *uxl_pool(const uxl_book *b, int idx);
int  uxl_count(const uxl_book *b, int s);
/* Walk the live cells of a sheet in row-major order - the only way to
 * enumerate, because there is no dense grid to scan. */
int  uxl_at(const uxl_book *b, int s, int i, int *r, int *c, uxl_val *out);

/* number-format id per cell: 0 = General, then the built-ins, then customs */
int  uxl_fmt(const uxl_book *b, int s, int r, int c);
void uxl_set_fmt(uxl_book *b, int s, int r, int c, int fmt);

/* ---- defined names --------------------------------------------------------- */
int  uxl_name_set(uxl_book *b, const char *name, int s, int r, int c);
int  uxl_name_find(const uxl_book *b, const char *name, int *s, int *r, int *c);

/* ---- recalculation ---------------------------------------------------------
 * Natural order falls out of evaluating recursively: a formula that needs a
 * cell computes that cell first.  A cell already being computed when it is
 * asked for again is a CIRCULAR REFERENCE, and Excel's answer is a zero plus
 * the CIRC indicator, not a hang - so that is this engine's answer too. */
int  uxl_recalc(uxl_book *b);          /* 1 = clean, 0 = a circular ref      */
int  uxl_circular(const uxl_book *b);

/* ---- display ----------------------------------------------------------------
 * What the cell SHOWS, which is not what it holds: 0.5 with a percent format
 * is "50%", and a serial with a date format is a date. */
int  uxl_text(const uxl_book *b, int s, int r, int c, char *out, int cap);

/* ---- number formats (uxl_numfmt.c) -----------------------------------------
 * Excel's format-code language, the part its display fidelity lives in:
 * up to four sections (positive; negative; zero; text), the # 0 ? , % and .
 * placeholders, literal runs, [Red] and friends, and date-time pictures. */
enum {                                /* the built-in ids Excel reserves     */
    UXL_FMT_GENERAL = 0, UXL_FMT_INT, UXL_FMT_2DP, UXL_FMT_THOUS,
    UXL_FMT_THOUS2, UXL_FMT_PCT, UXL_FMT_PCT2, UXL_FMT_SCI,
    UXL_FMT_CURRENCY, UXL_FMT_DATE, UXL_FMT_TIME, UXL_FMT_DATETIME,
    UXL_FMT_TEXT, UXL_FMT_NBUILTIN
};
const char *uxl_fmt_code(int id);      /* the built-in's code string         */
/* Render `v` through `code` into `out`.  Returns the length.  `is_text` uses
 * the fourth section.  A NULL or empty code means General. */
int  uxl_format(double v, const char *code, char *out, int cap);
int  uxl_format_text(const char *s, const char *code, char *out, int cap);
/* General: Excel's own "as many digits as fit, at most 15 significant". */
int  uxl_general(double v, char *out, int cap);

/* ---- A1 references ---------------------------------------------------------- */
int  uxl_a1_parse(const char *s, int *row, int *col, int *abs_r, int *abs_c);
int  uxl_a1_write(int row, int col, int abs_r, int abs_c, char *out, int cap);

/* ---- functions ---------------------------------------------------------------
 * The roster this build implements, for the app's Paste Function dialog and
 * for the gate to enumerate.  SPEC S-UOC-06 carries the full Excel 97 list;
 * this reports what is actually here. */
int         uxl_func_count(void);
const char *uxl_func_name(int i);
const char *uxl_func_category(int i);

#endif /* UOCALC_H */
