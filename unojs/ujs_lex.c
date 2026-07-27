/* ===========================================================================
 * unojs lexer. Hand-written scanner over a byte range; no allocation per
 * token (the payload goes into one growable scratch buffer that the compiler
 * consumes before the next ujs_lex_next call).
 * ======================================================================== */
#include "ujs_lex.h"
#include <stdio.h>

double ujs_strtod_impl(const char *s, const char **end);

static const struct { const char *w; int t; } kw[] = {
    {"var",T_VAR},{"let",T_LET},{"const",T_CONST},{"function",T_FUNCTION},
    {"return",T_RETURN},{"if",T_IF},{"else",T_ELSE},{"while",T_WHILE},{"do",T_DO},
    {"for",T_FOR},{"break",T_BREAK},{"continue",T_CONTINUE},{"new",T_NEW},
    {"delete",T_DELETE},{"typeof",T_TYPEOF},{"instanceof",T_INSTANCEOF},{"in",T_IN},
    {"this",T_THIS},{"null",T_NULL},{"true",T_TRUE},{"false",T_FALSE},
    {"throw",T_THROW},{"try",T_TRY},{"catch",T_CATCH},{"finally",T_FINALLY},
    {"switch",T_SWITCH},{"case",T_CASE},{"default",T_DEFAULT},{"void",T_VOID},
    {"of",T_OF},
    {NULL,0}
};

void ujs_lex_init(ujs_lexer *lx, ujs_vm *vm, const char *src, int len)
{
    memset(lx, 0, sizeof *lx);
    lx->vm = vm; lx->src = src; lx->len = len < 0 ? (int)strlen(src) : len;
    lx->line = 1;
}

void ujs_lex_free(ujs_lexer *lx)
{
    if (lx->buf) ujs_free_raw(lx->vm, lx->buf, (size_t)lx->bufcap);
    lx->buf = NULL; lx->bufcap = 0;
}

static int bufneed(ujs_lexer *lx, int n)
{
    if (n <= lx->bufcap) return 1;
    {   int nc = lx->bufcap ? lx->bufcap : 64;
        char *nb;
        while (nc < n) nc *= 2;
        nb = (char *)ujs_alloc_raw(lx->vm, (size_t)nc);
        if (!nb) return 0;
        if (lx->buf) { memcpy(nb, lx->buf, (size_t)lx->bufcap);
                       ujs_free_raw(lx->vm, lx->buf, (size_t)lx->bufcap); }
        lx->buf = nb; lx->bufcap = nc;
        return 1;
    }
}

void ujs_lex_save(ujs_lexer *lx, ujs_lexpos *p)
{ p->pos = lx->pos; p->line = lx->line; p->tok = lx->tok;
  p->start = lx->start; p->nl_before = lx->nl_before; p->num = lx->num; }

/* Rewind to the START of the saved token and re-scan it, rather than just
 * restoring the position fields. The token PAYLOAD (lx->text) lives in one
 * shared scratch buffer that later tokens overwrite, so a restore that did not
 * re-lex would leave `tok` correct but `text` pointing at some unrelated
 * identifier - which silently miscompiles every rewind the compiler does
 * (arrow-function lookahead, the hoisting pre-scan, the hoist prologue). */
void ujs_lex_restore(ujs_lexer *lx, const ujs_lexpos *p)
{
    lx->line = p->line;
    if (p->tok == T_EOF || p->tok == T_ERROR) {
        lx->pos = p->pos; lx->tok = p->tok; lx->start = p->start;
        lx->nl_before = p->nl_before; lx->num = p->num;
        return;
    }
    lx->pos = p->start;
    ujs_lex_next(lx);
    lx->nl_before = p->nl_before;      /* recomputed as 0 by the rescan */
}

static int at(ujs_lexer *lx, int off)
{ int i = lx->pos + off; return i < lx->len ? (u8)lx->src[i] : 0; }

static int ident_start(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'||c>=0x80; }
static int ident_part(int c)  { return ident_start(c) || (c>='0'&&c<='9'); }

static int err(ujs_lexer *lx, const char *m)
{ snprintf(lx->errmsg, sizeof lx->errmsg, "%s (line %d)", m, lx->line); return lx->tok = T_ERROR; }

/* \uXXXX / \xXX / \n ... into the scratch buffer; returns bytes written */
static int esc_utf8(char *out, unsigned cp)
{
    if (cp < 0x80)   { out[0] = (char)cp; return 1; }
    if (cp < 0x800)  { out[0] = (char)(0xC0|(cp>>6)); out[1] = (char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000){ out[0] = (char)(0xE0|(cp>>12)); out[1] = (char)(0x80|((cp>>6)&0x3F));
                       out[2] = (char)(0x80|(cp&0x3F)); return 3; }
    out[0] = (char)(0xF0|(cp>>18)); out[1] = (char)(0x80|((cp>>12)&0x3F));
    out[2] = (char)(0x80|((cp>>6)&0x3F)); out[3] = (char)(0x80|(cp&0x3F)); return 4;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* scan a quoted string (quote already consumed at lx->pos-1) */
static int scan_string(ujs_lexer *lx, int quote)
{
    int n = 0;
    for (;;) {
        int c = at(lx, 0);
        if (!c) return err(lx, "unterminated string");
        lx->pos++;
        if (c == quote) break;
        if (c == '\n') return err(lx, "newline in string");
        if (c == '\\') {
            int e = at(lx, 0); lx->pos++;
            if (!bufneed(lx, n + 8)) return err(lx, "out of memory");
            switch (e) {
            case 'n': lx->buf[n++] = '\n'; break;
            case 't': lx->buf[n++] = '\t'; break;
            case 'r': lx->buf[n++] = '\r'; break;
            case 'b': lx->buf[n++] = '\b'; break;
            case 'f': lx->buf[n++] = '\f'; break;
            case 'v': lx->buf[n++] = '\v'; break;
            case '0': lx->buf[n++] = '\0'; break;
            case '\n': lx->line++; break;                   /* line continuation */
            case 'x': { int h1 = hexval(at(lx,0)), h2 = hexval(at(lx,1));
                        if (h1 < 0 || h2 < 0) return err(lx, "bad \\x escape");
                        lx->pos += 2; n += esc_utf8(lx->buf + n, (unsigned)(h1*16+h2)); break; }
            case 'u': { unsigned cp = 0; int k;
                        if (at(lx,0) == '{') {              /* \u{...} */
                            lx->pos++;
                            while (hexval(at(lx,0)) >= 0) { cp = cp*16 + (unsigned)hexval(at(lx,0)); lx->pos++; }
                            if (at(lx,0) != '}') return err(lx, "bad \\u{} escape");
                            lx->pos++;
                        } else {
                            for (k = 0; k < 4; k++) { int h = hexval(at(lx,0));
                                if (h < 0) return err(lx, "bad \\u escape");
                                cp = cp*16 + (unsigned)h; lx->pos++; }
                        }
                        n += esc_utf8(lx->buf + n, cp); break; }
            default:  lx->buf[n++] = (char)e; break;
            }
            continue;
        }
        if (!bufneed(lx, n + 2)) return err(lx, "out of memory");
        lx->buf[n++] = (char)c;
    }
    if (!bufneed(lx, n + 1)) return err(lx, "out of memory");
    lx->buf[n] = 0;
    lx->text = lx->buf; lx->textlen = n;
    return lx->tok = T_STR;
}

int ujs_lex_next(ujs_lexer *lx)
{
    int c;
    lx->nl_before = 0;
    /* whitespace + comments */
    for (;;) {
        c = at(lx, 0);
        if (c == '\n') { lx->line++; lx->nl_before = 1; lx->pos++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { lx->pos++; continue; }
        if (c == '/' && at(lx, 1) == '/') { while (at(lx,0) && at(lx,0) != '\n') lx->pos++; continue; }
        if (c == '/' && at(lx, 1) == '*') {
            lx->pos += 2;
            for (;;) {
                if (!at(lx,0)) return err(lx, "unterminated comment");
                if (at(lx,0) == '\n') { lx->line++; lx->nl_before = 1; }
                if (at(lx,0) == '*' && at(lx,1) == '/') { lx->pos += 2; break; }
                lx->pos++;
            }
            continue;
        }
        break;
    }
    lx->start = lx->pos;
    c = at(lx, 0);
    if (!c) return lx->tok = T_EOF;

    /* numbers */
    if ((c >= '0' && c <= '9') || (c == '.' && at(lx,1) >= '0' && at(lx,1) <= '9')) {
        const char *e;
        if (c == '0' && (at(lx,1) == 'x' || at(lx,1) == 'X')) {
            u64 v = 0; lx->pos += 2;
            while (hexval(at(lx,0)) >= 0) { v = v*16 + (u64)hexval(at(lx,0)); lx->pos++; }
            lx->num = (double)v; return lx->tok = T_NUM;
        }
        if (c == '0' && (at(lx,1) == 'b' || at(lx,1) == 'B')) {
            u64 v = 0; lx->pos += 2;
            while (at(lx,0) == '0' || at(lx,0) == '1') { v = v*2 + (u64)(at(lx,0)-'0'); lx->pos++; }
            lx->num = (double)v; return lx->tok = T_NUM;
        }
        if (c == '0' && (at(lx,1) == 'o' || at(lx,1) == 'O')) {
            u64 v = 0; lx->pos += 2;
            while (at(lx,0) >= '0' && at(lx,0) <= '7') { v = v*8 + (u64)(at(lx,0)-'0'); lx->pos++; }
            lx->num = (double)v; return lx->tok = T_NUM;
        }
        lx->num = ujs_strtod_impl(lx->src + lx->pos, &e);
        lx->pos = (int)(e - lx->src);
        return lx->tok = T_NUM;
    }

    /* identifiers + keywords */
    if (ident_start(c)) {
        int s = lx->pos, n, i;
        while (ident_part(at(lx,0))) lx->pos++;
        n = lx->pos - s;
        if (!bufneed(lx, n + 1)) return err(lx, "out of memory");
        memcpy(lx->buf, lx->src + s, (size_t)n); lx->buf[n] = 0;
        lx->text = lx->buf; lx->textlen = n;
        for (i = 0; kw[i].w; i++)
            if ((int)strlen(kw[i].w) == n && !memcmp(kw[i].w, lx->buf, (size_t)n))
                return lx->tok = kw[i].t;
        return lx->tok = T_IDENT;
    }

    /* strings */
    if (c == '"' || c == '\'') { lx->pos++; return scan_string(lx, c); }

    /* template literal: no interpolation in v1 - `${` is reported so the
     * compiler can give a clear error rather than mis-parsing. */
    if (c == '`') {
        int n = 0;
        lx->pos++;
        for (;;) {
            int d = at(lx, 0);
            if (!d) return err(lx, "unterminated template literal");
            if (d == '`') { lx->pos++; break; }
            if (d == '\n') lx->line++;
            if (d == '\\') {
                lx->pos++;
                { int e2 = at(lx,0); lx->pos++;
                  if (!bufneed(lx, n + 8)) return err(lx, "out of memory");
                  switch (e2) {
                  case 'n': lx->buf[n++]='\n'; break; case 't': lx->buf[n++]='\t'; break;
                  case 'r': lx->buf[n++]='\r'; break; case '`': lx->buf[n++]='`'; break;
                  case '\\': lx->buf[n++]='\\'; break; case '$': lx->buf[n++]='$'; break;
                  default: lx->buf[n++]=(char)e2; break; } }
                continue;
            }
            lx->pos++;
            if (!bufneed(lx, n + 2)) return err(lx, "out of memory");
            lx->buf[n++] = (char)d;
        }
        if (!bufneed(lx, n + 1)) return err(lx, "out of memory");
        lx->buf[n] = 0; lx->text = lx->buf; lx->textlen = n;
        return lx->tok = T_TEMPLATE;
    }

    /* punctuation, longest match first */
    lx->pos++;
    switch (c) {
    case '(': return lx->tok = T_LPAREN;
    case ')': return lx->tok = T_RPAREN;
    case '{': return lx->tok = T_LBRACE;
    case '}': return lx->tok = T_RBRACE;
    case '[': return lx->tok = T_LBRACKET;
    case ']': return lx->tok = T_RBRACKET;
    case ';': return lx->tok = T_SEMI;
    case ',': return lx->tok = T_COMMA;
    case ':': return lx->tok = T_COLON;
    case '?': return lx->tok = T_QUESTION;
    case '~': return lx->tok = T_TILDE;
    case '.':
        if (at(lx,0)=='.' && at(lx,1)=='.') { lx->pos += 2; return lx->tok = T_SPREAD; }
        return lx->tok = T_DOT;
    case '=':
        if (at(lx,0)=='=' && at(lx,1)=='=') { lx->pos += 2; return lx->tok = T_SEQ; }
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_EQ; }
        if (at(lx,0)=='>') { lx->pos++; return lx->tok = T_ARROW; }
        return lx->tok = T_ASSIGN;
    case '!':
        if (at(lx,0)=='=' && at(lx,1)=='=') { lx->pos += 2; return lx->tok = T_SNE; }
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_NE; }
        return lx->tok = T_BANG;
    case '+':
        if (at(lx,0)=='+') { lx->pos++; return lx->tok = T_PLUSPLUS; }
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_PLUSEQ; }
        return lx->tok = T_PLUS;
    case '-':
        if (at(lx,0)=='-') { lx->pos++; return lx->tok = T_MINUSMINUS; }
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_MINUSEQ; }
        return lx->tok = T_MINUS;
    case '*':
        if (at(lx,0)=='*') { lx->pos++; return lx->tok = T_STARSTAR; }
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_STAREQ; }
        return lx->tok = T_STAR;
    case '/':
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_SLASHEQ; }
        return lx->tok = T_SLASH;
    case '%':
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_PERCENTEQ; }
        return lx->tok = T_PERCENT;
    case '<':
        if (at(lx,0)=='<' && at(lx,1)=='=') { lx->pos += 2; return lx->tok = T_SHLEQ; }
        if (at(lx,0)=='<') { lx->pos++; return lx->tok = T_SHL; }
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_LE; }
        return lx->tok = T_LT;
    case '>':
        if (at(lx,0)=='>' && at(lx,1)=='>' && at(lx,2)=='=') { lx->pos += 3; return lx->tok = T_USHREQ; }
        if (at(lx,0)=='>' && at(lx,1)=='>') { lx->pos += 2; return lx->tok = T_USHR; }
        if (at(lx,0)=='>' && at(lx,1)=='=') { lx->pos += 2; return lx->tok = T_SHREQ; }
        if (at(lx,0)=='>') { lx->pos++; return lx->tok = T_SHR; }
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_GE; }
        return lx->tok = T_GT;
    case '&':
        if (at(lx,0)=='&') { lx->pos++; return lx->tok = T_ANDAND; }
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_AMPEQ; }
        return lx->tok = T_AMP;
    case '|':
        if (at(lx,0)=='|') { lx->pos++; return lx->tok = T_OROR; }
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_PIPEEQ; }
        return lx->tok = T_PIPE;
    case '^':
        if (at(lx,0)=='=') { lx->pos++; return lx->tok = T_CARETEQ; }
        return lx->tok = T_CARET;
    default: break;
    }
    return err(lx, "unexpected character");
}

const char *ujs_tok_name(int t)
{
    static const char *n[T__COUNT];
    static int init = 0;
    if (!init) {
        int i; for (i = 0; i < T__COUNT; i++) n[i] = "?";
        n[T_EOF]="end of input"; n[T_NUM]="number"; n[T_STR]="string";
        n[T_IDENT]="identifier"; n[T_TEMPLATE]="template";
        n[T_LPAREN]="("; n[T_RPAREN]=")"; n[T_LBRACE]="{"; n[T_RBRACE]="}";
        n[T_LBRACKET]="["; n[T_RBRACKET]="]"; n[T_SEMI]=";"; n[T_COMMA]=",";
        n[T_DOT]="."; n[T_COLON]=":"; n[T_ASSIGN]="="; n[T_ARROW]="=>";
        n[T_PLUS]="+"; n[T_MINUS]="-"; n[T_STAR]="*"; n[T_SLASH]="/";
        n[T_PERCENT]="%"; n[T_STARSTAR]="**"; n[T_PLUSPLUS]="++";
        n[T_MINUSMINUS]="--"; n[T_EQ]="=="; n[T_NE]="!="; n[T_SEQ]="===";
        n[T_SNE]="!=="; n[T_LT]="<"; n[T_LE]="<="; n[T_GT]=">"; n[T_GE]=">=";
        n[T_ANDAND]="&&"; n[T_OROR]="||"; n[T_BANG]="!"; n[T_AMP]="&";
        n[T_PIPE]="|"; n[T_CARET]="^"; n[T_TILDE]="~"; n[T_SHL]="<<";
        n[T_SHR]=">>"; n[T_USHR]=">>>"; n[T_QUESTION]="?"; n[T_SPREAD]="...";
        n[T_PLUSEQ]="+="; n[T_MINUSEQ]="-="; n[T_STAREQ]="*=";
        n[T_SLASHEQ]="/="; n[T_PERCENTEQ]="%="; n[T_AMPEQ]="&=";
        n[T_PIPEEQ]="|="; n[T_CARETEQ]="^="; n[T_SHLEQ]="<<=";
        n[T_SHREQ]=">>="; n[T_USHREQ]=">>>="; n[T_ERROR]="bad input";
        for (i = 0; kw[i].w; i++) n[kw[i].t] = kw[i].w;
        init = 1;
    }
    return (t >= 0 && t < T__COUNT) ? n[t] : "?";
}
