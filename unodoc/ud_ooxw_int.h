/* ===========================================================================
 * The READ-BACK seam between the three model writers (ud_xlsw.c, ud_docw.c,
 * ud_pptw.c) and ud_ooxw.c, which serialises the same models as OOXML.
 * Internal; not part of the contract.
 *
 * WHY IT EXISTS.  The promise in unodoc.h is that "an app that can save .xls
 * can save .xlsx by calling a different serialiser" - the model is built once
 * and serialised twice.  That leaves the OOXML serialiser needing to READ a
 * model whose struct lives inside another translation unit.  The two ways out
 * are moving those structs into a shared header, which makes every field of
 * three writers public to the whole subsystem, or a small accessor seam that
 * exposes exactly what a serialiser needs.  This is the second.
 *
 * Everything here is read-only and returns pointers INTO the model: they are
 * valid until the writer is freed, and nothing here allocates.
 * ======================================================================== */
#ifndef UD_OOXW_INT_H
#define UD_OOXW_INT_H

/* ---- the workbook --------------------------------------------------------- */

/* One cell, flattened.  The number format arrives as its CODE rather than an
 * XF index because that is what .xlsx wants; the binary writer's XF table is
 * an encoding detail of .xls and has no counterpart here. */
typedef struct {
    int         row, col;
    int         kind;              /* UD_XV_*                                */
    double      num;               /* NUM, and the cached result of a formula */
    const char *str;               /* STR: the interned shared string        */
    int         err;               /* UD_XE_*                                */
    const char *fmt;               /* number-format code, NULL for General   */
    const char *formula;           /* "=..." as the caller gave it, or NULL  */
} ud_wcellview;

int         ud_xlsw_sheets    (const ud_xlsw *w);
const char *ud_xlsw_sheet_name(const ud_xlsw *w, int s);
int         ud_xlsw_is1904    (const ud_xlsw *w);
int         ud_xlsw_ncells    (const ud_xlsw *w, int s);
/* Cells come out in the order they were added, which is not row order; the
 * serialiser sorts, exactly as ud_xlsw_save does before emitting ROW/RK. */
int         ud_xlsw_cell_at   (const ud_xlsw *w, int s, int i, ud_wcellview *o);
int         ud_xlsw_nmerges   (const ud_xlsw *w, int s);
int         ud_xlsw_merge_at  (const ud_xlsw *w, int s, int i,
                               int *r0, int *c0, int *r1, int *c1);

/* ---- the document --------------------------------------------------------- */
int         ud_docw_nparas(const ud_docw *w);
/* Returns the paragraph text; bold/italic/align are filled when non-NULL. */
const char *ud_docw_para_at(const ud_docw *w, int i,
                            int *bold, int *italic, int *align);

/* ---- the presentation ----------------------------------------------------- */
int         ud_pptw_nslides (const ud_pptw *w);
const char *ud_pptw_title_at(const ud_pptw *w, int i);   /* NULL when unset */
const char *ud_pptw_body_at (const ud_pptw *w, int i);

#endif /* UD_OOXW_INT_H */
