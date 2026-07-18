/* ===========================================================================
 * UnoDOS/pc64 - a small tree-walking JavaScript interpreter (see js.h).
 *
 * Pipeline: lexer -> recursive-descent parser (AST in a bump arena) -> tree
 * walk with lexical scopes. Values: undefined/null/boolean/number(double)/
 * string/array/function + a few builtin objects. Supports var/let, the usual
 * operators (arithmetic, comparison, logical, ternary, assignment, ++/--),
 * if/else, while, for(;;), blocks, function declarations + expressions with
 * recursion, array literals/index/length/push, and the builtins console.log,
 * document.write, Math.*, String, Number, parseInt. No closures over mutated
 * scopes beyond the parent chain, no prototypes, objects or exceptions - a
 * pragmatic subset for browser page scripts, not a spec engine.
 * ======================================================================== */
#include "js.h"
#include <string.h>

/* -------- arena (all nodes / strings / values / scopes; reset per run) ----- */
#define ARENA_SZ (512 * 1024)
static char  g_arena[ARENA_SZ];
static long  g_used;
static int   g_oom;
static void *ar_alloc(long n)
{
    void *p; n = (n + 7) & ~7L;
    if (g_used + n > ARENA_SZ) { g_oom = 1; return &g_arena[0]; }
    p = &g_arena[g_used]; g_used += n; return p;
}
static char *ar_strn(const char *s, int n)
{ char *d = (char *)ar_alloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }

/* -------- values ----------------------------------------------------------- */
enum { JV_UNDEF, JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_FUN, JV_OBJ, JV_BUILTIN };
struct node;
typedef struct jval {
    unsigned char t;
    double n;                 /* number / bool                 */
    char  *s;                 /* string                        */
    struct jval **el; int elen, ecap;  /* array                */
    struct node *fn; struct scope *clo;/* function              */
    int obj; int bid; struct jval *recv;  /* builtin object/fn  */
} jval;

static jval *mk(unsigned char t) { jval *v = (jval *)ar_alloc(sizeof(jval)); memset(v, 0, sizeof *v); v->t = t; return v; }
static jval *jnum(double x) { jval *v = mk(JV_NUM); v->n = x; return v; }
static jval *jbool(int b)   { jval *v = mk(JV_BOOL); v->n = b ? 1 : 0; return v; }
static jval *jstr(char *s)  { jval *v = mk(JV_STR); v->s = s; return v; }
static jval  g_undef_v = { JV_UNDEF, 0,0,0,0,0,0,0,0,0,0 };
static jval *judef(void)    { return &g_undef_v; }

/* -------- output sinks ----------------------------------------------------- */
static char *g_out; static int g_outmax, g_outn;
static char *g_log; static int g_logmax, g_logn;
static void emit(char *dst, int *n, int max, const char *s)
{ while (*s && *n < max - 1) dst[(*n)++] = *s++; dst[*n] = 0; }

/* number -> string (integers without a decimal, else up to 6 trimmed) */
static void num_str(double x, char *out)
{
    char *p = out; long i; double f; int neg = 0, k;
    char tmp[32]; int tn = 0;
    if (x != x) { strcpy(out, "NaN"); return; }
    if (x < 0) { neg = 1; x = -x; }
    if (x > 1e15) { strcpy(out, neg ? "-1e15+" : "1e15+"); return; }
    i = (long)x; f = x - (double)i;
    if (neg) *p++ = '-';
    if (i == 0) tmp[tn++] = '0';
    while (i > 0) { tmp[tn++] = (char)('0' + i % 10); i /= 10; }
    while (tn) *p++ = tmp[--tn];
    if (f > 1e-9) {
        int digits = 0; *p++ = '.';
        while (f > 1e-9 && digits < 6) { f *= 10; k = (int)f; *p++ = (char)('0' + k); f -= k; digits++; }
        while (p[-1] == '0') p--;                 /* trim trailing zeros */
        if (p[-1] == '.') p--;
    }
    *p = 0;
}

/* -------- to-string / to-number / truthiness ------------------------------ */
static char *val_str(jval *v)
{
    char b[40];
    switch (v->t) {
    case JV_UNDEF: return "undefined";
    case JV_NULL:  return "null";
    case JV_BOOL:  return v->n ? "true" : "false";
    case JV_NUM:   num_str(v->n, b); return ar_strn(b, (int)strlen(b));
    case JV_STR:   return v->s ? v->s : "";
    case JV_ARR: {
        char *acc = (char *)ar_alloc(2048); int an = 0, i;
        for (i = 0; i < v->elen; i++) { char *e = val_str(v->el[i]);
            while (*e && an < 2046) acc[an++] = *e++; if (i < v->elen-1 && an < 2046) acc[an++] = ','; }
        acc[an] = 0; return acc; }
    case JV_FUN: case JV_BUILTIN: return "function";
    default: return "[object]";
    }
}
static double val_num(jval *v)
{
    switch (v->t) {
    case JV_NUM: return v->n;
    case JV_BOOL: return v->n;
    case JV_NULL: return 0;
    case JV_STR: { const char *s = v->s; double r = 0, sign = 1, frac; int any = 0;
        while (*s == ' ') s++; if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9') { r = r*10 + (*s - '0'); s++; any = 1; }
        if (*s == '.') { s++; frac = 0.1; while (*s >= '0' && *s <= '9') { r += (*s-'0')*frac; frac *= 0.1; s++; any = 1; } }
        return any ? sign * r : (0.0/0.0); }
    default: return 0;
    }
}
static int truthy(jval *v)
{
    switch (v->t) {
    case JV_UNDEF: case JV_NULL: return 0;
    case JV_BOOL: case JV_NUM: return v->n != 0;
    case JV_STR: return v->s && v->s[0];
    default: return 1;
    }
}

/* ======================= lexer ============================================ */
enum { T_EOF, T_NUM, T_STR, T_ID, T_PUNCT, T_KW };
typedef struct { int t, op; double num; char *str; int kw; } tok;
static const char *L; static tok cur, ahead; static int have_ahead;
static int g_err; static const char *g_errmsg;

enum { KW_VAR, KW_LET, KW_CONST, KW_IF, KW_ELSE, KW_WHILE, KW_FOR, KW_FUNCTION,
       KW_RETURN, KW_TRUE, KW_FALSE, KW_NULL, KW_UNDEF, KW_NONE };
static int kw_of(const char *s, int n)
{
    struct { const char *k; int v; } t[] = {
        {"var",KW_VAR},{"let",KW_LET},{"const",KW_CONST},{"if",KW_IF},{"else",KW_ELSE},
        {"while",KW_WHILE},{"for",KW_FOR},{"function",KW_FUNCTION},{"return",KW_RETURN},
        {"true",KW_TRUE},{"false",KW_FALSE},{"null",KW_NULL},{"undefined",KW_UNDEF},{0,0} };
    int i; for (i = 0; t[i].k; i++) if ((int)strlen(t[i].k) == n && !strncmp(t[i].k, s, n)) return t[i].v;
    return KW_NONE;
}
static int is_id0(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'; }
static int is_id(char c)  { return is_id0(c) || (c>='0'&&c<='9'); }

static tok lex(void)
{
    tok k; memset(&k, 0, sizeof k);
    for (;;) {
        while (*L==' '||*L=='\t'||*L=='\n'||*L=='\r') L++;
        if (L[0]=='/'&&L[1]=='/') { while (*L && *L!='\n') L++; continue; }
        if (L[0]=='/'&&L[1]=='*') { L+=2; while (*L && !(L[0]=='*'&&L[1]=='/')) L++; if (*L) L+=2; continue; }
        break;
    }
    if (!*L) { k.t = T_EOF; return k; }
    if (*L>='0'&&*L<='9') {
        double v = 0, f; k.t = T_NUM;
        while (*L>='0'&&*L<='9') { v = v*10 + (*L-'0'); L++; }
        if (*L=='.') { L++; f = 0.1; while (*L>='0'&&*L<='9') { v += (*L-'0')*f; f*=0.1; L++; } }
        k.num = v; return k;
    }
    if (*L=='"' || *L=='\'') {
        char q = *L++; char buf[1024]; int n = 0;
        while (*L && *L!=q && n < 1023) {
            char c = *L++;
            if (c=='\\' && *L) { char e=*L++; c = e=='n'?'\n':e=='t'?'\t':e=='r'?'\r':e=='\\'?'\\':e=='0'?0:e; }
            buf[n++] = c;
        }
        if (*L==q) L++;
        k.t = T_STR; k.str = ar_strn(buf, n); return k;
    }
    if (is_id0(*L)) {
        const char *s = L; while (is_id(*L)) L++;
        { int n = (int)(L - s), w = kw_of(s, n);
          if (w != KW_NONE) { k.t = T_KW; k.kw = w; } else { k.t = T_ID; k.str = ar_strn(s, n); } }
        return k;
    }
    /* punctuation / operators (2-char first) */
    k.t = T_PUNCT;
    { const char *two[] = {"==","!=","<=",">=","&&","||","+=","-=","*=","/=","++","--","===","!==",0};
      int i;
      if ((L[0]=='='&&L[1]=='='&&L[2]=='=')||(L[0]=='!'&&L[1]=='='&&L[2]=='=')) { k.op = (L[0]=='=')?'E':'N'; L+=3; return k; }
      for (i = 0; two[i]; i++) if (L[0]==two[i][0] && L[1]==two[i][1]) {
          static const char m[] = { 0 };
          (void)m;
          if (!strncmp(two[i],"==",2)) k.op='e'; else if(!strncmp(two[i],"!=",2)) k.op='n';
          else if(!strncmp(two[i],"<=",2)) k.op='l'; else if(!strncmp(two[i],">=",2)) k.op='g';
          else if(!strncmp(two[i],"&&",2)) k.op='a'; else if(!strncmp(two[i],"||",2)) k.op='o';
          else if(!strncmp(two[i],"++",2)) k.op='I'; else if(!strncmp(two[i],"--",2)) k.op='D';
          else if(!strncmp(two[i],"+=",2)) k.op='P'; else if(!strncmp(two[i],"-=",2)) k.op='M';
          else if(!strncmp(two[i],"*=",2)) k.op='T'; else if(!strncmp(two[i],"/=",2)) k.op='V';
          if (k.op) { L += 2; return k; }
      } }
    k.op = *L++; return k;
}
static void lx_init(const char *src) { L = src; have_ahead = 0; cur = lex(); }
static tok *peek(void) { return &cur; }
static tok *peek2(void) { if (!have_ahead) { ahead = lex(); have_ahead = 1; } return &ahead; }
static void adv(void) { if (have_ahead) { cur = ahead; have_ahead = 0; } else cur = lex(); }
static int is_punct(int op) { return cur.t == T_PUNCT && cur.op == op; }
static int eat_punct(int op) { if (is_punct(op)) { adv(); return 1; } return 0; }
static void perr(const char *m) { if (!g_err) { g_err = 1; g_errmsg = m; } }

/* Recursion guard. The parser (nested parens/arrays/unary) and the evaluator
 * (recursive calls) descend the native C stack with no per-frame counter; on
 * bare metal there is no guard page, so an overflow corrupts adjacent RAM
 * rather than faulting. Measure actual stack use against a base captured at the
 * top-level entry and trip an error well before the ~128 KB UEFI stack runs
 * out. One cheap check per recursive hub covers every cycle. */
static char *g_stkbase;
#define JS_STACK_LIMIT (48 * 1024)
static int too_deep(void)
{
    char probe;
    long used = g_stkbase - &probe;         /* stack grows down */
    if (used < 0) used = -used;
    if (used > JS_STACK_LIMIT) { perr("nesting too deep"); return 1; }
    return 0;
}

/* ======================= AST ============================================== */
enum { N_NUM, N_STR, N_BOOL, N_NULL, N_UNDEF, N_ID, N_BIN, N_UN, N_ASSIGN,
       N_CALL, N_INDEX, N_MEMBER, N_ARR, N_TERN, N_VAR, N_IF, N_WHILE, N_FOR,
       N_BLOCK, N_FUNC, N_RET, N_EXPR, N_PREPOST };
typedef struct node {
    int k, op;
    double num; char *str;
    struct node *a, *b, *c, *d;
    struct node **list; int nlist;
    int pre;                         /* ++/-- prefix vs postfix */
} node;
static node *nn(int k) { node *n = (node *)ar_alloc(sizeof(node)); memset(n, 0, sizeof *n); n->k = k; return n; }

static node *parse_expr(void);
static node *parse_stmt(void);

static node *parse_args(node *callee)
{
    node *c = nn(N_CALL); c->a = callee; c->list = (node **)ar_alloc(sizeof(node*) * 32); c->nlist = 0;
    adv();  /* '(' */
    if (!is_punct(')')) for (;;) {
        if (c->nlist < 32) c->list[c->nlist++] = parse_expr();
        if (eat_punct(',')) continue; break;
    }
    if (!eat_punct(')')) perr("expected )");
    return c;
}
static node *parse_primary(void)
{
    node *n;
    if (too_deep()) return nn(N_UNDEF);
    if (cur.t == T_NUM) { n = nn(N_NUM); n->num = cur.num; adv(); return n; }
    if (cur.t == T_STR) { n = nn(N_STR); n->str = cur.str; adv(); return n; }
    if (cur.t == T_ID)  { n = nn(N_ID); n->str = cur.str; adv(); return n; }
    if (cur.t == T_KW) {
        int w = cur.kw; adv();
        if (w==KW_TRUE) { n = nn(N_BOOL); n->num = 1; return n; }
        if (w==KW_FALSE){ n = nn(N_BOOL); n->num = 0; return n; }
        if (w==KW_NULL) return nn(N_NULL);
        if (w==KW_UNDEF)return nn(N_UNDEF);
        if (w==KW_FUNCTION) {                     /* function expression */
            n = nn(N_FUNC);
            if (cur.t == T_ID) { n->str = cur.str; adv(); }
            n->list = (node **)ar_alloc(sizeof(node*) * 16); n->nlist = 0;
            if (!eat_punct('(')) perr("expected (");
            if (!is_punct(')')) for (;;) { if (cur.t==T_ID && n->nlist<16) { n->list[n->nlist++] = nn(N_ID); n->list[n->nlist-1]->str = cur.str; adv(); } if (eat_punct(',')) continue; break; }
            if (!eat_punct(')')) perr("expected )");
            n->a = parse_stmt(); return n;
        }
        perr("unexpected keyword"); return nn(N_UNDEF);
    }
    if (eat_punct('(')) { n = parse_expr(); if (!eat_punct(')')) perr("expected )"); return n; }
    if (is_punct('[')) {                          /* array literal */
        adv(); n = nn(N_ARR); n->list = (node **)ar_alloc(sizeof(node*) * 64); n->nlist = 0;
        if (!is_punct(']')) for (;;) { if (n->nlist < 64) n->list[n->nlist++] = parse_expr(); if (eat_punct(',')) continue; break; }
        if (!eat_punct(']')) perr("expected ]");
        return n;
    }
    perr("unexpected token"); adv(); return nn(N_UNDEF);
}
static node *parse_postfix(void)
{
    node *n = parse_primary();
    for (;;) {
        if (is_punct('(')) n = parse_args(n);
        else if (is_punct('.')) { adv(); { node *m = nn(N_MEMBER); m->a = n; m->str = cur.str; if (cur.t==T_ID) adv(); else perr("expected name"); n = m; } }
        else if (is_punct('[')) { adv(); { node *m = nn(N_INDEX); m->a = n; m->b = parse_expr(); if (!eat_punct(']')) perr("expected ]"); n = m; } }
        else if (is_punct('I') || is_punct('D')) { node *u = nn(N_PREPOST); u->op = cur.op; u->a = n; u->pre = 0; adv(); n = u; }
        else break;
    }
    return n;
}
static node *parse_unary(void)
{
    if (too_deep()) return nn(N_UNDEF);
    if (is_punct('!') || is_punct('-') || is_punct('+')) { int op = cur.op; adv(); { node *u = nn(N_UN); u->op = op; u->a = parse_unary(); return u; } }
    if (is_punct('I') || is_punct('D')) { int op = cur.op; adv(); { node *u = nn(N_PREPOST); u->op = op; u->pre = 1; u->a = parse_unary(); return u; } }
    return parse_postfix();
}
static int bin_prec(int op)
{
    switch (op) {
    case '*': case '/': case '%': return 7;
    case '+': case '-': return 6;
    case '<': case '>': case 'l': case 'g': return 5;
    case 'e': case 'n': case 'E': case 'N': return 4;
    case 'a': return 3;
    case 'o': return 2;
    default: return -1;
    }
}
static node *parse_binrhs(int minp, node *lhs)
{
    for (;;) {
        int op = (cur.t==T_PUNCT) ? cur.op : 0, p = bin_prec(op);
        if (p < minp) return lhs;
        adv();
        { node *rhs = parse_unary();
          int nop = (cur.t==T_PUNCT) ? cur.op : 0, np = bin_prec(nop);
          if (np > p) rhs = parse_binrhs(p + 1, rhs);
          { node *b = nn(N_BIN); b->op = op; b->a = lhs; b->b = rhs; lhs = b; } }
    }
}
static node *parse_ternary(void)
{
    node *c = parse_binrhs(0, parse_unary());
    if (is_punct('?')) { adv(); { node *t = nn(N_TERN); t->a = c; t->b = parse_expr(); if (!eat_punct(':')) perr("expected :"); t->c = parse_expr(); return t; } }
    return c;
}
static node *parse_expr(void)
{
    node *l = parse_ternary();
    if (is_punct('=') || is_punct('P') || is_punct('M') || is_punct('T') || is_punct('V')) {
        int op = cur.op; adv(); { node *a = nn(N_ASSIGN); a->op = op; a->a = l; a->b = parse_expr(); return a; }
    }
    return l;
}
static node *parse_block(void)
{
    node *b = nn(N_BLOCK); b->list = (node **)ar_alloc(sizeof(node*) * 256); b->nlist = 0;
    adv();  /* '{' */
    while (!is_punct('}') && cur.t != T_EOF && !g_err) { node *st = parse_stmt(); if (b->nlist < 256) b->list[b->nlist++] = st; }
    if (!eat_punct('}')) perr("expected }");
    return b;
}
static node *parse_var(void)
{
    node *v = nn(N_VAR); v->list = (node **)ar_alloc(sizeof(node*) * 16); v->nlist = 0;
    adv();  /* var/let/const */
    for (;;) {
        node *d = nn(N_ID); d->str = cur.str; if (cur.t==T_ID) adv(); else perr("expected name");
        if (eat_punct('=')) d->a = parse_expr();
        if (v->nlist < 16) v->list[v->nlist++] = d;
        if (eat_punct(',')) continue; break;
    }
    eat_punct(';');
    return v;
}
static node *parse_stmt(void)
{
    if (too_deep()) return nn(N_UNDEF);
    if (is_punct('{')) return parse_block();
    if (cur.t == T_KW) {
        int w = cur.kw;
        if (w==KW_VAR||w==KW_LET||w==KW_CONST) return parse_var();
        if (w==KW_IF) { node *n = nn(N_IF); adv(); if(!eat_punct('('))perr("("); n->a=parse_expr(); if(!eat_punct(')'))perr(")"); n->b=parse_stmt(); if (cur.t==T_KW&&cur.kw==KW_ELSE){adv(); n->c=parse_stmt();} return n; }
        if (w==KW_WHILE){ node *n = nn(N_WHILE); adv(); if(!eat_punct('('))perr("("); n->a=parse_expr(); if(!eat_punct(')'))perr(")"); n->b=parse_stmt(); return n; }
        if (w==KW_FOR)  { node *n = nn(N_FOR); adv(); if(!eat_punct('('))perr("(");
            if (!is_punct(';')) n->a = (cur.t==T_KW&&(cur.kw==KW_VAR||cur.kw==KW_LET||cur.kw==KW_CONST))?parse_var():parse_expr();
            eat_punct(';');
            if (!is_punct(';')) n->b = parse_expr(); eat_punct(';');
            if (!is_punct(')')) n->c = parse_expr(); if(!eat_punct(')'))perr(")");
            n->d = parse_stmt(); return n; }
        if (w==KW_FUNCTION) { node *n = nn(N_FUNC); adv(); n->str = cur.str; if(cur.t==T_ID)adv(); else perr("name");
            n->list=(node**)ar_alloc(sizeof(node*)*16); n->nlist=0; if(!eat_punct('('))perr("(");
            if (!is_punct(')')) for(;;){ if(cur.t==T_ID&&n->nlist<16){n->list[n->nlist]=nn(N_ID);n->list[n->nlist++]->str=cur.str;adv();} if(eat_punct(','))continue; break; }
            if(!eat_punct(')'))perr(")"); n->a = parse_stmt(); return n; }
        if (w==KW_RETURN){ node *n = nn(N_RET); adv(); if (!is_punct(';') && !is_punct('}') && cur.t!=T_EOF) n->a = parse_expr(); eat_punct(';'); return n; }
    }
    { node *e = nn(N_EXPR); e->a = parse_expr(); eat_punct(';'); return e; }
}

/* ======================= scopes + eval ==================================== */
typedef struct binding { const char *name; jval *val; } binding;
typedef struct scope { binding b[96]; int n; struct scope *parent; } scope;
static scope *sc_new(scope *parent) { scope *s = (scope *)ar_alloc(sizeof(scope)); s->n = 0; s->parent = parent; return s; }
static jval **sc_find(scope *s, const char *name)
{ for (; s; s = s->parent) { int i; for (i = 0; i < s->n; i++) if (!strcmp(s->b[i].name, name)) return &s->b[i].val; } return 0; }
static void sc_def(scope *s, const char *name, jval *v)
{ int i; for (i=0;i<s->n;i++) if(!strcmp(s->b[i].name,name)){s->b[i].val=v;return;} if(s->n<96){s->b[s->n].name=name;s->b[s->n++].val=v;} }

/* builtin ids */
enum { OB_CONSOLE=1, OB_DOCUMENT, OB_MATH };
enum { BT_LOG=1, BT_WRITE, BT_FLOOR, BT_CEIL, BT_ABS, BT_MAX, BT_MIN, BT_SQRT, BT_ROUND,
       BT_RANDOM, BT_PUSH, BT_STRING, BT_NUMBER, BT_PARSEINT };
static unsigned long g_rng = 2463534242UL;

static int g_ret; static jval *g_retval;
static jval *ev(node *n, scope *s);

static jval *call_builtin(int bid, jval *recv, jval **a, int na)
{
    char b[40];
    switch (bid) {
    case BT_LOG: { int i; for (i=0;i<na;i++){ emit(g_log,&g_logn,g_logmax,val_str(a[i])); if(i<na-1) emit(g_log,&g_logn,g_logmax," "); } emit(g_log,&g_logn,g_logmax,"\n"); return judef(); }
    case BT_WRITE: { int i; for (i=0;i<na;i++) emit(g_out,&g_outn,g_outmax,val_str(a[i])); return judef(); }
    case BT_FLOOR: { double x=na?val_num(a[0]):0; long i=(long)x; if(x<0&&(double)i!=x)i--; return jnum((double)i); }
    case BT_CEIL:  { double x=na?val_num(a[0]):0; long i=(long)x; if(x>0&&(double)i!=x)i++; return jnum((double)i); }
    case BT_ROUND: { double x=na?val_num(a[0]):0; return jnum((double)(long)(x+(x<0?-0.5:0.5))); }
    case BT_ABS:   { double x=na?val_num(a[0]):0; return jnum(x<0?-x:x); }
    case BT_SQRT:  { double x=na?val_num(a[0]):0, r=x, i; if(x<=0)return jnum(0); for(i=0;i<24;i++) r=(r+x/r)/2; return jnum(r); }
    case BT_MAX:   { double m=-1e300; int i; for(i=0;i<na;i++){double x=val_num(a[i]); if(x>m)m=x;} return jnum(na?m:0); }
    case BT_MIN:   { double m=1e300; int i; for(i=0;i<na;i++){double x=val_num(a[i]); if(x<m)m=x;} return jnum(na?m:0); }
    case BT_RANDOM:{ g_rng ^= g_rng<<13; g_rng ^= g_rng>>17; g_rng ^= g_rng<<5; return jnum((double)(g_rng & 0xFFFFFF)/(double)0x1000000); }
    case BT_STRING:{ return jstr(na?ar_strn(val_str(a[0]),(int)strlen(val_str(a[0]))):ar_strn("",0)); }
    case BT_NUMBER:{ return jnum(na?val_num(a[0]):0); }
    case BT_PARSEINT:{ double x=na?val_num(a[0]):0; long i=(long)x; return jnum((double)i); }
    case BT_PUSH:  { int i; if (recv && recv->t==JV_ARR) for(i=0;i<na;i++){ if(recv->elen<recv->ecap){recv->el[recv->elen++]=a[i];} } return jnum(recv?recv->elen:0); }
    default: (void)b; return judef();
    }
}
static jval *member(jval *o, const char *name)
{
    if (o->t == JV_OBJ) {
        int b = 0;
        if (o->obj==OB_CONSOLE && !strcmp(name,"log")) b = BT_LOG;
        else if (o->obj==OB_DOCUMENT && !strcmp(name,"write")) b = BT_WRITE;
        else if (o->obj==OB_MATH) { b = !strcmp(name,"floor")?BT_FLOOR:!strcmp(name,"ceil")?BT_CEIL:!strcmp(name,"round")?BT_ROUND:
                                        !strcmp(name,"abs")?BT_ABS:!strcmp(name,"sqrt")?BT_SQRT:!strcmp(name,"max")?BT_MAX:
                                        !strcmp(name,"min")?BT_MIN:!strcmp(name,"random")?BT_RANDOM:0; }
        if (b) { jval *v = mk(JV_BUILTIN); v->bid = b; return v; }
    }
    if (o->t == JV_STR) { if (!strcmp(name,"length")) return jnum(o->s?(double)strlen(o->s):0); }
    if (o->t == JV_ARR) {
        if (!strcmp(name,"length")) return jnum(o->elen);
        if (!strcmp(name,"push")) { jval *v = mk(JV_BUILTIN); v->bid = BT_PUSH; v->recv = o; return v; }
    }
    return judef();
}
static jval *do_assign_target(node *t, jval *val, scope *s)   /* returns val */
{
    if (t->k == N_ID) { jval **slot = sc_find(s, t->str); if (slot) *slot = val; else sc_def(s, t->str, val); return val; }
    if (t->k == N_INDEX) { jval *o = ev(t->a, s), *idx = ev(t->b, s);
        if (o->t==JV_ARR) { int i=(int)val_num(idx); if(i>=0&&i<o->ecap){ if(i>=o->elen)o->elen=i+1; o->el[i]=val; } } return val; }
    return val;
}
static jval *ev(node *n, scope *s)
{
    if (!n || g_err || g_oom) return judef();
    if (too_deep()) return judef();
    switch (n->k) {
    case N_NUM: return jnum(n->num);
    case N_STR: return jstr(n->str);
    case N_BOOL: return jbool((int)n->num);
    case N_NULL: return mk(JV_NULL);
    case N_UNDEF: return judef();
    case N_ID: { jval **slot = sc_find(s, n->str); return slot ? *slot : judef(); }
    case N_ARR: { jval *v = mk(JV_ARR); v->ecap = n->nlist<8?16:n->nlist+8; v->el = (jval**)ar_alloc(sizeof(jval*)*v->ecap);
                  { int i; for (i=0;i<n->nlist;i++) v->el[v->elen++] = ev(n->list[i], s); } return v; }
    case N_MEMBER: return member(ev(n->a, s), n->str);
    case N_INDEX: { jval *o = ev(n->a, s), *idx = ev(n->b, s);
        if (o->t==JV_ARR){ int i=(int)val_num(idx); return (i>=0&&i<o->elen)?o->el[i]:judef(); }
        if (o->t==JV_STR){ int i=(int)val_num(idx); if(o->s&&i>=0&&i<(int)strlen(o->s)){char c[2]={o->s[i],0}; return jstr(ar_strn(c,1));} }
        return judef(); }
    case N_UN: { jval *a = ev(n->a, s);
        if (n->op=='!') return jbool(!truthy(a));
        if (n->op=='-') return jnum(-val_num(a));
        return jnum(val_num(a)); }
    case N_PREPOST: { node *t = n->a; jval *old = ev(t, s); double v = val_num(old) + (n->op=='I'?1:-1);
        do_assign_target(t, jnum(v), s); return n->pre ? jnum(v) : jnum(val_num(old)); }
    case N_TERN: return truthy(ev(n->a, s)) ? ev(n->b, s) : ev(n->c, s);
    case N_ASSIGN: {
        jval *rhs = ev(n->b, s);
        if (n->op!='=') { jval *cur_ = ev(n->a, s); double l = val_num(cur_), r = val_num(rhs);
            if (n->op=='P') { if (cur_->t==JV_STR||rhs->t==JV_STR){ char *ls=val_str(cur_),*rs=val_str(rhs); char *c=(char*)ar_alloc(strlen(ls)+strlen(rs)+1); strcpy(c,ls); strcat(c,rs); rhs=jstr(c);} else rhs=jnum(l+r); }
            else rhs = jnum(n->op=='M'?l-r:n->op=='T'?l*r:l/r); }
        return do_assign_target(n->a, rhs, s); }
    case N_BIN: {
        int op = n->op;
        if (op=='a') { jval *l = ev(n->a,s); return truthy(l)?ev(n->b,s):l; }
        if (op=='o') { jval *l = ev(n->a,s); return truthy(l)?l:ev(n->b,s); }
        { jval *l = ev(n->a,s), *r = ev(n->b,s);
          if (op=='+') { if (l->t==JV_STR||r->t==JV_STR){ char *ls=val_str(l),*rs=val_str(r); char *c=(char*)ar_alloc(strlen(ls)+strlen(rs)+1); strcpy(c,ls); strcat(c,rs); return jstr(c);} return jnum(val_num(l)+val_num(r)); }
          if (op=='-') return jnum(val_num(l)-val_num(r));
          if (op=='*') return jnum(val_num(l)*val_num(r));
          if (op=='/') return jnum(val_num(l)/val_num(r));
          if (op=='%') { long a=(long)val_num(l),b=(long)val_num(r); return jnum(b?(double)(a%b):(0.0/0.0)); }
          if (op=='<') return jbool(val_num(l)<val_num(r));
          if (op=='>') return jbool(val_num(l)>val_num(r));
          if (op=='l') return jbool(val_num(l)<=val_num(r));
          if (op=='g') return jbool(val_num(l)>=val_num(r));
          if (op=='e'||op=='E') { if (l->t==JV_STR&&r->t==JV_STR) return jbool(!strcmp(val_str(l),val_str(r))); return jbool(val_num(l)==val_num(r)); }
          if (op=='n'||op=='N') { if (l->t==JV_STR&&r->t==JV_STR) return jbool(!!strcmp(val_str(l),val_str(r))); return jbool(val_num(l)!=val_num(r)); }
          return judef(); } }
    case N_CALL: {
        jval *args[32]; int na = n->nlist>32?32:n->nlist, i;
        jval *callee, *recv = 0;
        for (i=0;i<na;i++) args[i] = ev(n->list[i], s);
        if (n->a->k==N_MEMBER) { jval *o = ev(n->a->a, s); recv = o; callee = member(o, n->a->str); }
        else callee = ev(n->a, s);
        if (callee->t==JV_BUILTIN) return call_builtin(callee->bid, callee->recv?callee->recv:recv, args, na);
        if (callee->t==JV_FUN) {
            long mark = g_used;                 /* reclaim the call frame on return */
            scope *fs = sc_new(callee->clo ? callee->clo : s); node *f = callee->fn; int j;
            for (j=0;j<f->nlist;j++) sc_def(fs, f->list[j]->str, j<na?args[j]:judef());
            g_ret = 0; g_retval = judef(); ev(f->a, fs);
            jval *rv = g_ret ? g_retval : judef(); g_ret = 0;
            /* If the return value is self-contained (no references into the frame
             * we're about to free), copy it below the mark and release the frame.
             * This turns deep recursion from O(calls) memory into O(depth). Arrays/
             * objects/functions may alias frame memory, so those keep their frame. */
            if (!g_oom && !g_err &&
                (rv->t==JV_UNDEF||rv->t==JV_NULL||rv->t==JV_BOOL||rv->t==JV_NUM||rv->t==JV_STR)) {
                unsigned char t = rv->t; double num = rv->n;
                char tmp[1024]; int haveS = 0;
                if (t==JV_STR && rv->s) { int L=(int)strlen(rv->s);
                    if (L < (int)sizeof(tmp)) { memcpy(tmp, rv->s, L+1); haveS = 1; } }
                if (t!=JV_STR || haveS) {                 /* safe to relocate */
                    g_used = mark;
                    { jval *o = mk(t); o->n = num;
                      if (haveS) o->s = ar_strn(tmp, (int)strlen(tmp));
                      return o; }
                }
            }
            return rv; }
        return judef(); }
    case N_FUNC: { jval *v = mk(JV_FUN); v->fn = n; v->clo = s; if (n->str) sc_def(s, n->str, v); return v; }
    case N_VAR: { int i; for (i=0;i<n->nlist;i++){ node *d=n->list[i]; sc_def(s, d->str, d->a?ev(d->a,s):judef()); } return judef(); }
    case N_EXPR: return ev(n->a, s);
    case N_BLOCK: { int i; for (i=0;i<n->nlist && !g_ret && !g_err;i++) ev(n->list[i], s); return judef(); }
    case N_IF: if (truthy(ev(n->a,s))) ev(n->b,s); else if (n->c) ev(n->c,s); return judef();
    case N_WHILE: { int guard=0; while (truthy(ev(n->a,s)) && !g_ret && !g_err && guard++<1000000) ev(n->b,s); return judef(); }
    case N_FOR: { int guard=0; if (n->a) ev(n->a,s);
        while ((!n->b || truthy(ev(n->b,s))) && !g_ret && !g_err && guard++<1000000) { ev(n->d,s); if (n->c) ev(n->c,s); } return judef(); }
    case N_RET: g_retval = n->a?ev(n->a,s):judef(); g_ret = 1; return g_retval;
    default: return judef();
    }
}

/* ======================= entry ============================================ */
static void def_globals(scope *g)
{
    jval *c = mk(JV_OBJ); c->obj = OB_CONSOLE; sc_def(g, "console", c);
    { jval *d = mk(JV_OBJ); d->obj = OB_DOCUMENT; sc_def(g, "document", d); }
    { jval *m = mk(JV_OBJ); m->obj = OB_MATH; sc_def(g, "Math", m); }
    { jval *v = mk(JV_BUILTIN); v->bid = BT_STRING; sc_def(g, "String", v); }
    { jval *v = mk(JV_BUILTIN); v->bid = BT_NUMBER; sc_def(g, "Number", v); }
    { jval *v = mk(JV_BUILTIN); v->bid = BT_PARSEINT; sc_def(g, "parseInt", v); }
}
int js_run(const char *src, char *out, int outmax, char *log, int logmax)
{
    scope *g; node *prog; int i;
    char base;
    g_stkbase = &base;                    /* recursion-guard reference point */
    g_used = 0; g_oom = 0; g_err = 0; g_errmsg = 0; g_ret = 0;
    g_out = out; g_outmax = outmax; g_outn = (int)strlen(out);
    g_log = log; g_logmax = logmax; g_logn = (int)strlen(log);
    lx_init(src);
    prog = nn(N_BLOCK); prog->list = (node **)ar_alloc(sizeof(node*) * 512); prog->nlist = 0;
    while (cur.t != T_EOF && !g_err) { node *st = parse_stmt(); if (prog->nlist < 512) prog->list[prog->nlist++] = st; }
    if (g_err) { emit(log, &g_logn, g_logmax, "JS parse error: "); emit(log,&g_logn,g_logmax, g_errmsg?g_errmsg:"?"); emit(log,&g_logn,g_logmax,"\n"); return 1; }
    g = sc_new(0); def_globals(g);
    /* hoist top-level function declarations */
    for (i = 0; i < prog->nlist; i++) if (prog->list[i]->k == N_FUNC) ev(prog->list[i], g);
    for (i = 0; i < prog->nlist && !g_err; i++) if (prog->list[i]->k != N_FUNC) ev(prog->list[i], g);
    if (g_oom) { emit(log,&g_logn,g_logmax,"JS: out of memory\n"); return 2; }
    return 0;
}

#ifdef JS_TEST
#include <stdio.h>
int main(int argc, char **argv)
{
    static char out[8192], log[8192]; char src[16384]; int n;
    FILE *f = argc>1 ? fopen(argv[1],"rb") : stdin;
    out[0]=0; log[0]=0;
    n = (int)fread(src, 1, sizeof src - 1, f); src[n]=0;
    js_run(src, out, sizeof out, log, sizeof log);
    printf("--- document.write ---\n%s\n--- console.log ---\n%s", out, log);
    return 0;
}
#endif
