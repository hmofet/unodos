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

#define UXL_MAXRPN 96

typedef struct {
    unsigned short row, col;
    short          fmt;
    uxl_val        v;                 /* the cached result                  */
    char           fml[UXL_FMLLEN];   /* the source text, "" when a literal */
    uxl_tok        rpn[UXL_MAXRPN];
    short          nrpn;
    unsigned char  mark;              /* recalc visit state                 */
    unsigned char  gen;               /* which recalc generation cached it  */
} uxl_cell;

typedef struct {
    char     name[32];
    uxl_cell cell[UXL_MAXCELL];
    int      ncell;
} uxl_sheet;

struct uxl_book {
    uxl_sheet sheet[UXL_MAXSHEET];
    int       nsheet;
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
uxl_cell *uxl_slot(uxl_book *b, int s, int r, int c, int make);
int       uxl_intern(uxl_book *b, const char *t);

/* the compiler, used by uxl_set_formula */
int uxl_compile(uxl_book *b, const char *text, int home_s,
                uxl_tok *out, int cap);

#endif /* UXL_INT_H */
