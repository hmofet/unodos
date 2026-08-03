/* ===========================================================================
 * uxl_int.h - UnoCalc internals, shared between the store, the calculator
 * and the formatter.  Not part of the contract; uocalc.h is.
 * ======================================================================== */
#ifndef UXL_INT_H
#define UXL_INT_H

#include "uocalc.h"

/* ---- the compiled formula ---------------------------------------------------
 * POSTFIX, and shaped like Excel's ptgs on purpose: operand tokens carry a
 * value or a reference, operator tokens carry an arity.  Keeping the shape
 * makes unodoc's ptg conversion mechanical in both directions rather than a
 * translation layer nobody can check. */
enum {
    RPN_NUM = 1, RPN_STR, RPN_BOOL, RPN_ERR,
    RPN_REF,          /* a single cell                                      */
    RPN_RANGE,        /* a rectangular area                                 */
    RPN_OP,           /* a binary or unary operator, `op` says which        */
    RPN_FUNC          /* a function call, `idx` names it, `argc` its arity  */
};

/* operators, in the precedence Excel gives them */
enum {
    OP_ADD = 1, OP_SUB, OP_MUL, OP_DIV, OP_POW, OP_CAT,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
    OP_NEG, OP_PCT
};

typedef struct {
    unsigned char kind;      /* RPN_*                                       */
    unsigned char op;        /* OP_* for RPN_OP                             */
    unsigned char argc;      /* RPN_FUNC                                    */
    short         idx;       /* string index, or the function's table slot  */
    double        num;
    /* references: sheet -1 means "this cell's own sheet" */
    short  s, r1, c1, r2, c2;
} uxl_tok;

#define UXL_MAXRPN  96             /* tokens in ONE formula                 */
#define UXL_RPNPOOL 2048           /* tokens in the whole workbook          */

/* A cell's compiled form lives in the workbook's token pool, not in the cell:
 * inline it was 96 tokens x 32 bytes = 3 KB of a 3.2 KB cell, i.e. 94% of the
 * store, paid by every literal too.  rpn_at is a pool index and nrpn its
 * length; a literal has nrpn == 0. */
typedef struct {
    unsigned short row, col;
    short          fmt;
    short          nrpn;
    int            rpn_at;            /* index into book.rpn[]              */
    uxl_val        v;                 /* the cached result                  */
    char           fml[UXL_FMLLEN];   /* the source text, "" when a literal */
    unsigned char  mark;              /* recalc visit state                 */
    unsigned char  gen;               /* which recalc generation cached it  */
} uxl_cell;

/* A sheet owns no cells, only a sorted view of the shared pool. */
typedef struct {
    char           name[32];
    unsigned short idx[UXL_MAXCELL];  /* sorted by (row,col), into b->cell  */
    int            ncell;
} uxl_sheet;

struct uxl_book {
    uxl_sheet sheet[UXL_MAXSHEET];
    int       nsheet;

    uxl_cell  cell[UXL_MAXCELL];      /* the shared cell pool               */
    int       nalloc;                 /* high-water mark of the bump half   */
    unsigned short freecell[UXL_MAXCELL];
    int       nfree;                  /* cells returned by uxl_clear        */

    uxl_tok   rpn[UXL_RPNPOOL];       /* the shared token pool              */
    int       nrpnpool;               /* bump; reclaimed by uxl_rpn_rebuild */

    char      pool[UXL_MAXSTR][UXL_STRLEN];
    int       npool;
    struct { char name[32]; int s, r, c; } name[UXL_MAXNAME];
    int       nname;
    unsigned  rev;
    int       dirty;
    int       circular;
    unsigned char gen;
};

/* the store's internals the calculator needs */
int       uxl_find(const uxl_book *b, int s, int r, int c);
uxl_cell *uxl_nth(uxl_book *b, int s, int i);       /* the i'th live cell   */
int       uxl_rpn_store(uxl_book *b, uxl_cell *p, const uxl_tok *t, int n);
uxl_cell *uxl_slot(uxl_book *b, int s, int r, int c, int make);
int       uxl_intern(uxl_book *b, const char *t);

/* the compiler, used by uxl_set_formula */
int uxl_compile(uxl_book *b, const char *text, int home_s,
                uxl_tok *out, int cap);

#endif /* UXL_INT_H */
