/* ===========================================================================
 * ucc internals - shared between ucc.c (front end) and ucc_x64.c (backend).
 *
 * Everything lives in the caller-supplied work arena (bump allocation, no
 * frees); a compile either finishes or longjmps out through Cc.jb.  All
 * state hangs off the one Cc context so the compiler is re-entrant per
 * call (though not concurrently - the pc64 world is single-threaded).
 * ======================================================================== */
#ifndef UCC_INT_H
#define UCC_INT_H

#include "ucc.h"

/* freestanding-safe libc slice: these resolve to pc64_libc in the kernel
 * build, to kernel-export thunks in the module build, to glibc on host */
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
int   memcmp(const void *a, const void *b, unsigned long n);
unsigned long strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, unsigned long n);
char *strcpy(char *d, const char *s);

typedef long long          i64;
typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* ---- tokens --------------------------------------------------------------- */
enum {
    TK_EOF, TK_IDENT, TK_NUM, TK_STR, TK_PUNCT, TK_KW,
};
/* keywords */
enum {
    KW_VOID, KW_CHAR, KW_SHORT, KW_INT, KW_LONG, KW_UNSIGNED, KW_SIGNED,
    KW_CONST, KW_STATIC, KW_EXTERN, KW_STRUCT, KW_UNION, KW_ENUM, KW_TYPEDEF,
    KW_IF, KW_ELSE, KW_WHILE, KW_DO, KW_FOR, KW_SWITCH, KW_CASE, KW_DEFAULT,
    KW_BREAK, KW_CONTINUE, KW_RETURN, KW_SIZEOF, KW_NKW,
};
/* punctuators (single id per spelling) */
enum {
    P_LPAR, P_RPAR, P_LBRACE, P_RBRACE, P_LBRACK, P_RBRACK,
    P_SEMI, P_COMMA, P_DOT, P_ARROW, P_QUEST, P_COLON, P_ELLIPSIS,
    P_PLUS, P_MINUS, P_STAR, P_SLASH, P_PCT, P_AMP, P_PIPE, P_CARET,
    P_TILDE, P_BANG, P_LT, P_GT, P_LE, P_GE, P_EQ, P_NE, P_SHL, P_SHR,
    P_ANDAND, P_OROR, P_INC, P_DEC, P_ASSIGN,
    P_ADDA, P_SUBA, P_MULA, P_DIVA, P_MODA, P_ANDA, P_ORA, P_XORA,
    P_SHLA, P_SHRA, P_NPUNCT,
};

typedef struct Tok Tok;
struct Tok {
    u8   kind;
    u8   id;                    /* KW_* or P_* */
    u16  file;                  /* index into Cc.files[] */
    int  seq;                   /* lexing order (macro visibility window) */
    int  line, col;
    const char *p; int len;     /* identifier spelling (arena copy)        */
    i64  val;                   /* TK_NUM value                            */
    u8   num_uns, num_long;     /* numeric literal suffix info             */
    const char *str; int slen;  /* TK_STR decoded bytes (excl. NUL)        */
    Tok *next;
};

/* ---- types ---------------------------------------------------------------- */
enum {
    TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_LONG,
    TY_PTR, TY_ARR, TY_FUNC, TY_STRUCT, TY_UNION,
};

typedef struct Type   Type;
typedef struct Member Member;
typedef struct Param  Param;

struct Member { const char *name; Type *ty; int off; Member *next; };
struct Param  { const char *name; Type *ty; Param *next; };

struct Type {
    u8  kind;
    u8  is_uns;
    int size, align;
    Type   *base;               /* PTR/ARR target, FUNC return             */
    int     arr_len;            /* ARR; -1 = incomplete                    */
    Member *members;            /* STRUCT/UNION; 0 = incomplete tag        */
    Param  *params; int nparams;/* FUNC                                    */
};

/* ---- symbols -------------------------------------------------------------- */
enum { SYM_LOCAL, SYM_GLOBAL, SYM_FUNC, SYM_TYPEDEF, SYM_ENUMC, SYM_IMPORT };

typedef struct Sym Sym;
struct Sym {
    const char *name;
    Type *ty;
    u8    cls;                  /* SYM_*                                   */
    u8    is_static;
    u8    defined;              /* function has a body / global emitted    */
    int   lvoff;                /* SYM_LOCAL: negative rbp offset          */
    i64   enum_val;
    int   sec, secoff;          /* SYM_GLOBAL: SEC_* + offset              */
    int   funcno;               /* SYM_FUNC: index into Cc.funcs           */
    int   impno;                /* SYM_IMPORT: import record index, -1 yet */
    Sym  *next;                 /* scope chain                             */
};

typedef struct Scope Scope;
struct Scope { Sym *syms; Sym *tags; Scope *up; };

/* struct/union/enum tags live in Scope.tags as Syms whose ty is the tag'd
 * type (enum tags carry TY_INT) */

/* ---- AST ------------------------------------------------------------------ */
enum {
    /* expressions */
    ND_NUM, ND_VAR, ND_STR, ND_DEREF, ND_ADDR, ND_MEMBER, ND_ASSIGN,
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_AND, ND_OR, ND_XOR,
    ND_SHL, ND_SHR, ND_EQ, ND_NE, ND_LT, ND_LE, ND_NOT, ND_BITNOT,
    ND_NEG, ND_LOGAND, ND_LOGOR, ND_COND, ND_COMMA, ND_CALL, ND_CAST,
    ND_POSTINC, ND_POSTDEC,
    /* statements */
    ND_BLOCK, ND_IF, ND_WHILE, ND_DO, ND_FOR, ND_SWITCH, ND_CASE,
    ND_BREAK, ND_CONT, ND_RETURN, ND_EXPRSTMT, ND_NULL,
};

typedef struct Node Node;
struct Node {
    u8    kind;
    Type *ty;
    Tok  *tok;                  /* for diagnostics                         */
    Node *lhs, *rhs;            /* operands                                */
    Node *cond, *then, *els;    /* if/for/while/cond                       */
    Node *init, *inc;           /* for                                     */
    Node *body;                 /* block/loop body, next case chain        */
    Node *next;                 /* statement / argument list link          */
    Node *args;                 /* ND_CALL                                 */
    Sym  *sym;                  /* ND_VAR / direct ND_CALL target          */
    i64   val;                  /* ND_NUM; ND_MEMBER offset                */
    int   stroff;               /* ND_STR: rodata offset                   */
    /* switch backend bookkeeping */
    Node *cases; Node *defcase; int caseno;
};

/* ---- sections / output ----------------------------------------------------- */
enum { SEC_CODE, SEC_DATA, SEC_BSS };

/* code fixups, resolved after layout */
enum { FX_CODE, FX_DATA, FX_BSS, FX_IMPORT };
typedef struct Fixup Fixup;
struct Fixup {
    u8  kind;                   /* what the rel32 at site refers to        */
    int site;                   /* offset in code of the rel32 field       */
    int target;                 /* data/bss off / import index             */
    Sym *targetsym;             /* FX_CODE: the function (fwd refs ok)     */
    Fixup *next;
};

/* u64 address cells in .data that need a base reloc (initializers holding
 * &global / function / string addresses).  Resolved at emit time: value
 * written = target RVA (+ addend), cell offset listed in the reloc table.
 * sym != 0 -> symbol target (global/function); else raw data-section off. */
typedef struct DReloc DReloc;
struct DReloc { int off; Sym *sym; int dataoff; int addend; DReloc *next; };

typedef struct Func Func;
struct Func {
    Sym  *sym;
    int   codeoff;              /* offset of prologue in code section      */
    Func *next;
};

/* ---- imports ---------------------------------------------------------------*/
typedef struct Import Import;
struct Import { const char *name; int slot_off; Import *next; int idx; };

/* ---- the compile context --------------------------------------------------- */
#define UCC_MAXFILES 17
#define UCC_LOCALS_MAX (1 << 20)  /* sanity cap on a frame */

typedef struct Cc Cc;
struct Cc {
    /* arena */
    u8  *wk; long wkcap, wkuse;
    /* diagnostics */
    UccDiag *diag; int maxdiag, ndiag;
    void *jb[5];                    /* __builtin_setjmp buffer */
    /* files */
    const char *files[UCC_MAXFILES]; int nfiles;
    /* token stream */
    Tok *tok;                       /* parse cursor */
    /* macro table */
    struct Macro *macros;
    /* scopes */
    Scope *scope;
    Scope  gscope;
    /* current function state (front) */
    Type *cur_ret;
    int   lvsize;                   /* running frame size */
    Node *cur_sw;                   /* innermost switch (case collection) */
    /* statics uniquifier */
    int   statno;
    int   lexseq;                   /* token sequence counter */
    int   dry;                      /* sizing pass for unsized-array inits:
                                       parse but write/reserve nothing */
    /* string literals: interned into data section image */
    /* section images */
    u8 *code; int codelen, codecap;
    u8 *data; int datalen, datacap; /* rodata+data, bss appended virtually */
    int bsslen;
    Fixup  *fixups;
    DReloc *drelocs; int ndreloc;
    Func   *funcs, *funcs_tail; int nfuncs;
    Import *imports; int nimports;
    int     memcpy_imp;             /* cached "memcpy" import index, -1 */
    /* max call args seen in current function (outgoing area) */
    int  entry_code_off;            /* uno_app_main */
    char summary[128];
};

/* ---- shared helpers (ucc.c) ------------------------------------------------ */
void *ucc_alloc(Cc *cc, long n);
void  ucc_err_tok(Cc *cc, Tok *t, const char *m1, const char *m2); /* no return */
void  ucc_fatal(Cc *cc, const char *m1, const char *m2);           /* no return */
i64   ucc_const_expr(Cc *cc, Node *n, int *ok); /* fold; also addr-const helper */

/* backend entry (ucc_x64.c): lower every parsed function + emit the .UNO */
void ucc_gen_func(Cc *cc, Sym *fnsym, Node *body);
long ucc_emit_uno(Cc *cc, unsigned char *out, long outmax);

/* front-end services the backend needs */
int  ucc_type_is_int(const Type *t);
int  ucc_import_ok(const char *name, int len);   /* against baked kexports */

#endif /* UCC_INT_H */
