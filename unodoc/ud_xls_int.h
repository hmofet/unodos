/* ===========================================================================
 * The seam between ud_xls.c (which owns the workbook) and ud_ptg.c (which
 * owns formula decompilation).  Internal; not part of the contract.
 *
 * The decompiler needs three things only the workbook knows - what a 3-D
 * reference index names, and what a defined name is called - so they come in
 * as callbacks rather than ud_ptg.c reaching into the workbook struct.
 * ======================================================================== */
#ifndef UD_XLS_INT_H
#define UD_XLS_INT_H

typedef struct {
    void *book;
    /* "Sheet2", or "Sheet1:Sheet3" for a spanning reference; NULL if the
       index is out of range (rendered as #REF). */
    const char *(*xti_name)(void *book, int ixti);
    const char *(*name_of)(void *book, int idx);              /* NAME       */
    const char *(*extname_of)(void *book, int ixti, int idx); /* NAMEX      */
} ud_ptg_env;

/* Decompile a ptg array to "=..." in one ud_alloc'd string, or NULL if the
 * token stream did not add up (a damaged file, or a token this build does
 * not render - the caller then reports the cached value with no text).
 * base_row/base_col are the cell that owns the formula, needed by the
 * relative PtgRefN/PtgAreaN tokens shared formulas are built from.
 * `extra` is the rgbExtra bytes following the ptg array (array constants). */
char *ud_ptg_text(const unsigned char *pt, long n, const ud_ptg_env *env,
                  int base_row, int base_col,
                  const unsigned char *extra, long extra_n);

/* 1 if the array is nothing but a PtgExp pointing at a shared formula. */
int ud_ptg_is_exp(const unsigned char *pt, long n, int *row, int *col);

/* ---- the other direction: text -> ptgs (phase 3b) -------------------------
 * What the compiler needs from the workbook it is compiling INTO: which
 * sheet a name refers to (for a 3-D reference) and which defined name. */
typedef struct {
    void *book;
    int (*sheet_index)(void *book, const char *name);  /* -1 if no such sheet */
    int (*name_index)(void *book, const char *name);   /* 1-based, 0 = none   */
} ud_ptgc_env;

/* Compile "=SUM(A1:A9)" (the leading '=' is optional) into a ptg array in one
 * ud_alloc'd buffer.  NULL on a syntax error, with ud_error() saying what and
 * roughly where.  base_row/base_col are the cell the formula lives in; they
 * are not used for absolute references but are what a future relative-token
 * mode would need. */
unsigned char *ud_ptg_compile(const char *text, const ud_ptgc_env *env,
                              int base_row, int base_col, long *len);

/* ---- the builder seam: ud_xlsx.c fills the SAME workbook ------------------
 * An OOXML workbook and a BIFF8 workbook differ only in how the bytes are
 * spelled.  Rather than a second cell model with a second set of accessors -
 * and a second place for a bug to live - ud_xlsx.c constructs `struct ud_xls`
 * through these, so ud_xls_cell_at(), the merges, the number formats and every
 * consumer above them work on either without knowing which they have.
 *
 * Internal, not part of the contract.  The rule is the same one the BIFF
 * parser follows: add sheets in order, add cells in any order, call
 * ud_xls_built() once at the end (it sorts the cells, which every accessor
 * assumes). */
ud_xls   *ud_xls_blank(void);
int       ud_xls_b_sheet(ud_xls *x, const char *name, int visible);
/* A cell to fill in; NULL if the position is out of range or the sheet is
 * full.  The pointer is into the sheet's array and is invalidated by the next
 * ud_xls_b_cell() on that sheet, so write it before adding another. */
ud_xcell *ud_xls_b_cell(ud_xls *x, int sheet, int row, int col);
/* Copy `s` into the workbook and return the copy, which lives until close -
 * how a cell's `str` and `ftext` get storage they do not own. */
const char *ud_xls_b_str(ud_xls *x, const char *s);
int       ud_xls_b_merge(ud_xls *x, int sheet, int r0, int c0, int r1, int c1);
/* Define XF `xf` as using number-format id `ifmt`, and (optionally) give that
 * id a format code.  The two are separate because a file states them
 * separately and either may be absent. */
int       ud_xls_b_xf(ud_xls *x, int xf, int ifmt);
int       ud_xls_b_fmt(ud_xls *x, int ifmt, const char *code);
void      ud_xls_b_date1904(ud_xls *x, int on);
void      ud_xls_built(ud_xls *x);

#endif /* UD_XLS_INT_H */
