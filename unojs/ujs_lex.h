/* unojs lexer - tokens and the scanner state shared with the compiler. */
#ifndef UJS_LEX_H
#define UJS_LEX_H
#include "ujs_int.h"

enum {
    T_EOF = 0, T_ERROR,
    T_NUM, T_STR, T_TEMPLATE, T_IDENT,
    /* keywords */
    T_VAR, T_LET, T_CONST, T_FUNCTION, T_RETURN, T_IF, T_ELSE, T_WHILE, T_DO,
    T_FOR, T_BREAK, T_CONTINUE, T_NEW, T_DELETE, T_TYPEOF, T_INSTANCEOF, T_IN,
    T_THIS, T_NULL, T_TRUE, T_FALSE, T_THROW, T_TRY, T_CATCH, T_FINALLY,
    T_SWITCH, T_CASE, T_DEFAULT, T_VOID, T_OF,
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_SEMI, T_COMMA, T_DOT, T_COLON, T_QUESTION, T_ARROW, T_SPREAD,
    T_ASSIGN, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_STARSTAR,
    T_PLUSPLUS, T_MINUSMINUS,
    T_EQ, T_NE, T_SEQ, T_SNE, T_LT, T_LE, T_GT, T_GE,
    T_ANDAND, T_OROR, T_BANG,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_SHL, T_SHR, T_USHR,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_AMPEQ, T_PIPEEQ, T_CARETEQ, T_SHLEQ, T_SHREQ, T_USHREQ,
    T__COUNT
};

typedef struct {
    const char *src;
    int         len;
    int         pos;
    int         line;
    /* current token */
    int         tok;
    int         start;          /* byte offset of the token                  */
    int         nl_before;      /* a line terminator preceded this token
                                 * (automatic semicolon insertion needs it)  */
    double      num;
    char       *text;           /* T_STR/T_IDENT/T_TEMPLATE payload          */
    int         textlen;
    char        errmsg[128];
    ujs_vm     *vm;
    /* scratch buffer for the token payload, grown as needed */
    char       *buf;
    int         bufcap;
} ujs_lexer;

void ujs_lex_init(ujs_lexer *lx, ujs_vm *vm, const char *src, int len);
void ujs_lex_free(ujs_lexer *lx);
int  ujs_lex_next(ujs_lexer *lx);              /* advances; returns lx->tok  */
/* Save/restore for the hoisting pre-scan and for arrow-function backtracking. */
typedef struct { int pos, line, tok, start, nl_before; double num; } ujs_lexpos;
void ujs_lex_save(ujs_lexer *lx, ujs_lexpos *p);
void ujs_lex_restore(ujs_lexer *lx, const ujs_lexpos *p);
const char *ujs_tok_name(int t);

#endif
