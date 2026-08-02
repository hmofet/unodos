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

#endif /* UD_XLS_INT_H */
