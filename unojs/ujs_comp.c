/* ===========================================================================
 * unojs compiler - recursive descent straight to bytecode, one pass over the
 * token stream per function body (plus a cheap pre-scan that hoists `var` and
 * `function` declarations, which JS semantics require to exist before the
 * statements that use them).
 *
 * DEVIATION from docs/WEB-ENGINE-DESIGN.md §3.1, recorded deliberately: the
 * design sketched source -> AST -> bytecode. There is no AST here. Nothing in
 * v1 needs a tree (no optimizer, no source maps), an AST would be several
 * thousand lines of node types and a second traversal, and the pre-scan covers
 * the one thing single-pass compilers classically get wrong. If an optimizer
 * or a source-map consumer ever arrives, an AST goes in then.
 *
 * SCOPING (v1): `var`, `let` and `const` are all FUNCTION-scoped. Block scope
 * and the per-iteration binding of `for (let i ...)` are M1b - see UNOJS.md's
 * known-gaps list. Everything else about closures is correct: each call gets a
 * heap environment, and a nested function captures its definition environment.
 * ======================================================================== */
#include "ujs_lex.h"
#include <stdio.h>
#include <stdlib.h>

/* Instruction lengths. Declared in ujs_int.h; defined here, next to the
 * emitter that produces them. */
const u8 ujs_op_len[OP__COUNT] = {
    /* NOP */            1,
    /* CONST */          3,
    /* UNDEF NULL TRUE FALSE */ 1, 1, 1, 1,
    /* ZERO ONE */       1, 1,
    /* DUP POP SWAP */   1, 1, 1,
    /* ROT3 OVER_DUP */  1, 1,
    /* GETLOC SETLOC */  4, 4,
    /* GETGLOB SETGLOB DECLGLOB */ 5, 5, 5,
    /* GETPROP_KEEP GETIDX_KEEP */ 5, 1,
    /* GETGLOB_SOFT */    5,
    /* SETRES GETRES */  1, 1,
    /* GETPROP SETPROP */ 5, 5,
    /* GETIDX SETIDX */  1, 1,
    /* DELPROP DELIDX */ 5, 1,
    /* ADD SUB MUL DIV MOD POW */ 1, 1, 1, 1, 1, 1,
    /* NEG POS NOT BITNOT */ 1, 1, 1, 1,
    /* AND OR XOR SHL SHR USHR */ 1, 1, 1, 1, 1, 1,
    /* EQ NE SEQ SNE LT LE GT GE */ 1, 1, 1, 1, 1, 1, 1, 1,
    /* IN INSTANCEOF TYPEOF */ 1, 1, 1,
    /* JMP JT JF JT_KEEP JF_KEEP */ 5, 5, 5, 5, 5,
    /* CALL NEW */       2, 2,
    /* RET */            1,
    /* CLOSURE */        3,
    /* ARRAY OBJECT */   3, 3,
    /* THIS */           1,
    /* THROW */          1,
    /* TRY */            9,
    /* ENDTRY FINALLY_END */ 1, 1,
    /* ENV_PUSH ENV_POP */ 3, 1,
    /* ITER ITER_OF */   1, 1,
    /* ITER_NEXT */      5,
    /* LINE */           5
};

typedef struct patchlist { int *v; int n, cap; } patchlist;

typedef struct loopctx {
    struct loopctx *prev;
    int       continue_target;      /* -1 until known (for-loop update) */
    int       is_switch;            /* `break` targets it, `continue` does not */
    patchlist breaks, continues;
} loopctx;

typedef struct hoistfn { u32 atom; ujs_lexpos pos; } hoistfn;

typedef struct fnscope {
    struct fnscope *parent;
    hoistfn  *hoists;  u32 nhoists, hoistcap;
    u8       *bc;      u32 nbc, bccap;
    ujs_val  *consts;  u32 nconsts, constcap;
    u32      *locals;  u32 nlocals, localcap;    /* atom per slot */
    u32       nparams;
    int       is_global;                          /* top level: vars are globals */
    int       is_arrow;
    loopctx  *loops;
    u32       name_atom;
} fnscope;

typedef struct {
    ujs_vm    *vm;
    ujs_lexer  lx;
    fnscope   *fn;
    int        haderr;
    char       err[192];
} compiler;

/* ---- diagnostics --------------------------------------------------------- */
static void cerr(compiler *c, const char *fmt, const char *a)
{
    if (c->haderr) return;
    c->haderr = 1;
    if (a) snprintf(c->err, sizeof c->err, "%s '%s' (line %d)", fmt, a, c->lx.line);
    else   snprintf(c->err, sizeof c->err, "%s (line %d)", fmt, c->lx.line);
}

/* ---- emit ---------------------------------------------------------------- */
static void emit(compiler *c, u8 b)
{
    fnscope *f = c->fn;
    if (c->haderr) return;
    if (f->nbc == f->bccap) {
        u32 nc = f->bccap ? f->bccap * 2 : 128;
        u8 *nb = (u8 *)ujs_alloc_raw(c->vm, nc);
        if (!nb) { cerr(c, "out of memory", NULL); return; }
        if (f->bc) { memcpy(nb, f->bc, f->nbc); ujs_free_raw(c->vm, f->bc, f->bccap); }
        f->bc = nb; f->bccap = nc;
    }
    f->bc[f->nbc++] = b;
}

static void emit_u16(compiler *c, u32 v) { emit(c, (u8)(v & 0xFF)); emit(c, (u8)((v >> 8) & 0xFF)); }
static void emit_u32(compiler *c, u32 v)
{ emit(c, (u8)(v & 0xFF)); emit(c, (u8)((v >> 8) & 0xFF));
  emit(c, (u8)((v >> 16) & 0xFF)); emit(c, (u8)((v >> 24) & 0xFF)); }

static int emit_jump(compiler *c, u8 op)
{ int at; emit(c, op); at = (int)c->fn->nbc; emit_u32(c, 0); return at; }

static void patch_to(compiler *c, int at, u32 target)
{
    fnscope *f = c->fn;
    if (c->haderr || at < 0 || (u32)at + 4 > f->nbc) return;
    f->bc[at]   = (u8)(target & 0xFF);
    f->bc[at+1] = (u8)((target >> 8) & 0xFF);
    f->bc[at+2] = (u8)((target >> 16) & 0xFF);
    f->bc[at+3] = (u8)((target >> 24) & 0xFF);
}

static void patch_here(compiler *c, int at) { patch_to(c, at, c->fn->nbc); }

static u32 add_const(compiler *c, ujs_val v)
{
    fnscope *f = c->fn;
    u32 i;
    if (c->haderr) return 0;
    for (i = 0; i < f->nconsts; i++)                  /* small pools: dedupe */
        if (VBITS(f->consts[i]) == VBITS(v)) return i;
    if (f->nconsts == f->constcap) {
        u32 nc = f->constcap ? f->constcap * 2 : 16;
        ujs_val *nv = (ujs_val *)ujs_alloc_raw(c->vm, (size_t)nc * sizeof *nv);
        if (!nv) { cerr(c, "out of memory", NULL); return 0; }
        if (f->consts) { memcpy(nv, f->consts, (size_t)f->nconsts * sizeof *nv);
                         ujs_free_raw(c->vm, f->consts, (size_t)f->constcap * sizeof *nv); }
        f->consts = nv; f->constcap = nc;
    }
    f->consts[f->nconsts] = v;
    return f->nconsts++;
}

static void emit_const(compiler *c, ujs_val v)
{ u32 k = add_const(c, v); emit(c, OP_CONST); emit_u16(c, k); }

/* ---- patch lists (break / continue) -------------------------------------- */
static void pl_add(compiler *c, patchlist *p, int at)
{
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 8;
        int *nv = (int *)ujs_alloc_raw(c->vm, (size_t)nc * sizeof *nv);
        if (!nv) { cerr(c, "out of memory", NULL); return; }
        if (p->v) { memcpy(nv, p->v, (size_t)p->n * sizeof *nv);
                    ujs_free_raw(c->vm, p->v, (size_t)p->cap * sizeof *nv); }
        p->v = nv; p->cap = nc;
    }
    p->v[p->n++] = at;
}

static void pl_patch(compiler *c, patchlist *p, u32 target)
{ int i; for (i = 0; i < p->n; i++) patch_to(c, p->v[i], target);
  if (p->v) ujs_free_raw(c->vm, p->v, (size_t)p->cap * sizeof *p->v);
  p->v = NULL; p->n = p->cap = 0; }

/* ---- locals -------------------------------------------------------------- */
static int local_find(fnscope *f, u32 atom)
{ u32 i; for (i = 0; i < f->nlocals; i++) if (f->locals[i] == atom) return (int)i; return -1; }

static int local_add(compiler *c, u32 atom)
{
    fnscope *f = c->fn;
    int e = local_find(f, atom);
    if (e >= 0) return e;                          /* re-declaration is a no-op */
    if (f->nlocals == f->localcap) {
        u32 nc = f->localcap ? f->localcap * 2 : 8;
        u32 *nv = (u32 *)ujs_alloc_raw(c->vm, (size_t)nc * sizeof *nv);
        if (!nv) { cerr(c, "out of memory", NULL); return 0; }
        if (f->locals) { memcpy(nv, f->locals, (size_t)f->nlocals * sizeof *nv);
                         ujs_free_raw(c->vm, f->locals, (size_t)f->localcap * sizeof *nv); }
        f->locals = nv; f->localcap = nc;
    }
    f->locals[f->nlocals] = atom;
    return (int)f->nlocals++;
}

/* Resolve `atom` to (depth, slot) walking out through enclosing FUNCTION
 * scopes. Returns 0 when it is not a local anywhere - i.e. a global. */
static int resolve(compiler *c, u32 atom, int *depth, int *slot)
{
    fnscope *f = c->fn;
    int d = 0;
    while (f && !f->is_global) {
        int i = local_find(f, atom);
        if (i >= 0) { *depth = d; *slot = i; return 1; }
        f = f->parent; d++;
    }
    return 0;
}

static void emit_load_name(compiler *c, u32 atom)
{
    int d, s;
    if (resolve(c, atom, &d, &s)) { emit(c, OP_GETLOC); emit(c, (u8)d); emit_u16(c, (u32)s); }
    else { emit(c, OP_GETGLOB); emit_u32(c, atom); }
}

static void emit_store_name(compiler *c, u32 atom)
{
    int d, s;
    if (resolve(c, atom, &d, &s)) { emit(c, OP_SETLOC); emit(c, (u8)d); emit_u16(c, (u32)s); }
    else { emit(c, OP_SETGLOB); emit_u32(c, atom); }
}

/* ---- token plumbing ------------------------------------------------------ */
static void next(compiler *c)
{
    if (c->haderr) return;
    ujs_lex_next(&c->lx);
    if (c->lx.tok == T_ERROR) { c->haderr = 1;
        snprintf(c->err, sizeof c->err, "%s", c->lx.errmsg); }
}

static int check(compiler *c, int t) { return c->lx.tok == t; }

static int accept(compiler *c, int t)
{ if (check(c, t)) { next(c); return 1; } return 0; }

static void expect(compiler *c, int t, const char *what)
{ if (!accept(c, t)) cerr(c, what, NULL); }

static u32 tok_atom(compiler *c)
{ return ujs_atom(c->vm, c->lx.text, c->lx.textlen); }

/* automatic semicolon insertion: a statement ends at `;`, `}`, EOF, or a
 * newline. Anything else is a syntax error. */
static void semicolon(compiler *c)
{
    if (accept(c, T_SEMI)) return;
    if (check(c, T_RBRACE) || check(c, T_EOF) || c->lx.nl_before) return;
    cerr(c, "expected ';'", NULL);
}

/* ---- forward declarations ------------------------------------------------ */
static void expression(compiler *c);          /* full expression, incl. comma */
static void assign_expr(compiler *c);         /* no comma operator            */
static void statement(compiler *c);
static void block(compiler *c);
static ujs_code *compile_function(compiler *c, u32 name_atom, int is_arrow,
                                  int arrow_single_param, u32 single_param_atom);

static void hoist_add(compiler *c, u32 atom, const ujs_lexpos *pos)
{
    fnscope *f = c->fn;
    if (f->nhoists == f->hoistcap) {
        u32 nc = f->hoistcap ? f->hoistcap * 2 : 8;
        hoistfn *nv = (hoistfn *)ujs_alloc_raw(c->vm, (size_t)nc * sizeof *nv);
        if (!nv) return;
        if (f->hoists) { memcpy(nv, f->hoists, (size_t)f->nhoists * sizeof *nv);
                         ujs_free_raw(c->vm, f->hoists, (size_t)f->hoistcap * sizeof *nv); }
        f->hoists = nv; f->hoistcap = nc;
    }
    f->hoists[f->nhoists].atom = atom;
    f->hoists[f->nhoists].pos = *pos;
    f->nhoists++;
}

/* ---- the hoisting pre-scan ----------------------------------------------
 * Runs over the tokens of the body about to be compiled, at brace depth,
 * declaring every `var`/`let`/`const`/`function` name it finds. Nested
 * function bodies are skipped so their vars stay theirs. The lexer is
 * restored afterwards, so this costs one extra tokenization of the body. */
static void prescan_body(compiler *c, int stop_at_brace)
{
    ujs_lexpos save;
    int depth = 0, prev = T_SEMI;      /* start of a body == statement position */
    ujs_lex_save(&c->lx, &save);
    for (;;) {
        int t = c->lx.tok, wasprev = prev;
        if (t == T_EOF || t == T_ERROR) break;
        prev = t;
        if (t == T_LBRACE) depth++;
        else if (t == T_RBRACE) { if (stop_at_brace && depth == 0) break; depth--; }
        else if (t == T_VAR || t == T_LET || t == T_CONST) {
            next(c);
            while (check(c, T_IDENT)) {
                local_add(c, tok_atom(c));
                next(c);
                /* skip the initializer: stop at a comma at THIS paren depth */
                { int pd = 0;
                  while (!c->haderr) {
                      int u = c->lx.tok;
                      if (u == T_EOF) break;
                      if (u == T_LPAREN || u == T_LBRACKET) pd++;
                      else if (u == T_RPAREN || u == T_RBRACKET) { if (pd == 0) break; pd--; }
                      else if (pd == 0 && (u == T_SEMI || u == T_RBRACE)) break;
                      else if (pd == 0 && u == T_COMMA) { next(c); break; }
                      else if (u == T_LBRACE) { pd++; }
                      else if (u == T_RBRACE) { if (pd == 0) break; pd--; }
                      next(c);
                  } }
                if (!check(c, T_IDENT)) break;
            }
            prev = T_SEMI;                  /* back at statement position */
            continue;
        }
        else if (t == T_FUNCTION) {
            ujs_lexpos fpos;
            ujs_lex_save(&c->lx, &fpos);          /* the `function` token itself */
            next(c);
            /* `function f(){}` in STATEMENT position is a declaration and
             * hoists; `var g = function f(){}` is an expression and does not.
             * The preceding token tells them apart. */
            if (check(c, T_IDENT) &&
                (wasprev == T_SEMI || wasprev == T_LBRACE || wasprev == T_RBRACE)) {
                u32 fa = tok_atom(c);
                local_add(c, fa);
                hoist_add(c, fa, &fpos);
                next(c);
            }
            /* skip params + body */
            { int pd = 0, bd = 0, started = 0;
              while (!c->haderr && c->lx.tok != T_EOF) {
                  int u = c->lx.tok;
                  if (u == T_LPAREN) pd++;
                  else if (u == T_RPAREN) pd--;
                  else if (u == T_LBRACE) { bd++; started = 1; }
                  else if (u == T_RBRACE) { bd--; if (started && bd == 0) { next(c); break; } }
                  next(c);
              } }
            prev = T_RBRACE;                /* a function body ends with '}' */
            continue;
        }
        next(c);
    }
    c->haderr = 0;                       /* the pre-scan never reports errors;
                                          * the real pass will hit them properly */
    ujs_lex_restore(&c->lx, &save);
}

/* Compile every hoisted function declaration up front, so a call that appears
 * BEFORE the declaration in source order still finds a real function. Each was
 * recorded with the lexer position of its `function` keyword, so this just
 * rewinds, compiles, and binds - then puts the lexer back. */
static void emit_hoisted(compiler *c)
{
    fnscope *f = c->fn;
    ujs_lexpos save;
    u32 i;
    if (!f->nhoists) return;
    ujs_lex_save(&c->lx, &save);
    for (i = 0; i < f->nhoists && !c->haderr; i++) {
        u32 name = f->hoists[i].atom;
        ujs_code *code;
        ujs_lex_restore(&c->lx, &f->hoists[i].pos);   /* tok == `function` */
        next(c);                                      /* -> the name       */
        if (check(c, T_IDENT)) next(c);               /* -> '('            */
        code = compile_function(c, name, 0, 0, 0);
        if (!code) break;
        { u32 k = add_const(c, ujs_ptrval(code)); emit(c, OP_CLOSURE); emit_u16(c, k); }
        if (f->is_global) { emit(c, OP_DECLGLOB); emit_u32(c, name); }
        emit_store_name(c, name);
        emit(c, OP_POP);
    }
    ujs_lex_restore(&c->lx, &save);
}

/* Skip a function declaration's tokens: emit_hoisted already compiled it. */
static void skip_function_decl(compiler *c)
{
    int pd = 0, bd = 0, started = 0;
    next(c);                                          /* past `function`   */
    if (check(c, T_IDENT)) next(c);
    while (!c->haderr && !check(c, T_EOF)) {
        int t = c->lx.tok;
        if (t == T_LPAREN) pd++;
        else if (t == T_RPAREN) pd--;
        else if (t == T_LBRACE) { bd++; started = 1; }
        else if (t == T_RBRACE) { bd--; if (started && bd == 0) { next(c); return; } }
        next(c);
    }
    if (!c->haderr) cerr(c, "unterminated function declaration", NULL);
}

/* ---- primary expressions ------------------------------------------------- */
static void object_literal(compiler *c)
{
    u32 n = 0;
    /* '{' consumed */
    while (!check(c, T_RBRACE) && !c->haderr) {
        /* key */
        if (check(c, T_IDENT) || (c->lx.tok >= T_VAR && c->lx.tok <= T_OF)) {
            emit_const(c, ujs_string(c->vm, c->lx.text, c->lx.textlen)); next(c);
        } else if (check(c, T_STR)) {
            emit_const(c, ujs_string(c->vm, c->lx.text, c->lx.textlen)); next(c);
        } else if (check(c, T_NUM)) {
            char b[48]; ujs_num_to_str(c->lx.num, b, sizeof b);
            emit_const(c, ujs_string(c->vm, b, -1)); next(c);
        } else { cerr(c, "bad property name in object literal", NULL); return; }
        expect(c, T_COLON, "expected ':' in object literal");
        assign_expr(c);
        n++;
        if (!accept(c, T_COMMA)) break;
    }
    expect(c, T_RBRACE, "expected '}' closing object literal");
    emit(c, OP_OBJECT); emit_u16(c, n);
}

static void array_literal(compiler *c)
{
    u32 n = 0;
    while (!check(c, T_RBRACKET) && !c->haderr) {
        if (check(c, T_COMMA)) { emit(c, OP_UNDEF); n++; next(c); continue; }  /* hole */
        assign_expr(c);
        n++;
        if (!accept(c, T_COMMA)) break;
    }
    expect(c, T_RBRACKET, "expected ']' closing array literal");
    emit(c, OP_ARRAY); emit_u16(c, n);
}

/* Is the parenthesised run starting at the current '(' an arrow parameter
 * list? Scan forward to the matching ')' and look for '=>'. */
static int looks_like_arrow(compiler *c)
{
    ujs_lexpos save;
    int depth = 0, res = 0;
    ujs_lex_save(&c->lx, &save);
    for (;;) {
        int t = c->lx.tok;
        if (t == T_EOF || t == T_ERROR) break;
        if (t == T_LPAREN) depth++;
        else if (t == T_RPAREN) { depth--; if (!depth) { next(c); res = check(c, T_ARROW); break; } }
        next(c);
    }
    c->haderr = 0;
    ujs_lex_restore(&c->lx, &save);
    return res;
}

static void primary(compiler *c)
{
    if (c->haderr) return;
    switch (c->lx.tok) {
    case T_NUM:   emit_const(c, ujs_number(c->lx.num)); next(c); return;
    case T_STR: case T_TEMPLATE:
                  emit_const(c, ujs_string(c->vm, c->lx.text, c->lx.textlen)); next(c); return;
    case T_TRUE:  emit(c, OP_TRUE);  next(c); return;
    case T_FALSE: emit(c, OP_FALSE); next(c); return;
    case T_NULL:  emit(c, OP_NULL);  next(c); return;
    case T_THIS:  emit(c, OP_THIS);  next(c); return;
    case T_IDENT: {
        u32 a = tok_atom(c);
        ujs_lexpos save;
        ujs_lex_save(&c->lx, &save);
        next(c);
        if (check(c, T_ARROW)) {                 /* single-parameter arrow */
            ujs_code *code;
            next(c);
            code = compile_function(c, 0, 1, 1, a);
            if (code) { u32 k = add_const(c, ujs_ptrval(code)); emit(c, OP_CLOSURE); emit_u16(c, k); }
            return;
        }
        ujs_lex_restore(&c->lx, &save);
        next(c);
        emit_load_name(c, a);
        return; }
    case T_FUNCTION: {
        u32 name = 0;
        ujs_code *code;
        next(c);
        if (check(c, T_IDENT)) { name = tok_atom(c); next(c); }
        code = compile_function(c, name, 0, 0, 0);
        if (code) { u32 k = add_const(c, ujs_ptrval(code)); emit(c, OP_CLOSURE); emit_u16(c, k); }
        return; }
    case T_LPAREN:
        if (looks_like_arrow(c)) {
            ujs_code *code = compile_function(c, 0, 1, 0, 0);
            if (code) { u32 k = add_const(c, ujs_ptrval(code)); emit(c, OP_CLOSURE); emit_u16(c, k); }
            return;
        }
        next(c);
        expression(c);
        expect(c, T_RPAREN, "expected ')'");
        return;
    case T_LBRACE:   next(c); object_literal(c); return;
    case T_LBRACKET: next(c); array_literal(c);  return;
    default:
        cerr(c, "unexpected token", ujs_tok_name(c->lx.tok));
        return;
    }
}

/* member access, calls, `new` - the tightest-binding suffixes */
static void call_suffix(compiler *c, int allow_call);

static void new_expr(compiler *c)
{
    u32 argc = 0;
    next(c);                                     /* 'new' */
    if (check(c, T_NEW)) new_expr(c); else { primary(c); call_suffix(c, 0); }
    if (accept(c, T_LPAREN)) {
        while (!check(c, T_RPAREN) && !c->haderr) {
            assign_expr(c); argc++;
            if (!accept(c, T_COMMA)) break;
        }
        expect(c, T_RPAREN, "expected ')' after constructor arguments");
    }
    emit(c, OP_NEW); emit(c, (u8)argc);
}

static void call_suffix(compiler *c, int allow_call)
{
    for (;;) {
        if (c->haderr) return;
        if (accept(c, T_DOT)) {
            if (!check(c, T_IDENT) && !(c->lx.tok >= T_VAR && c->lx.tok <= T_OF)) {
                cerr(c, "expected property name after '.'", NULL); return; }
            emit(c, OP_GETPROP); emit_u32(c, tok_atom(c));
            next(c);
            continue;
        }
        if (accept(c, T_LBRACKET)) {
            expression(c);
            expect(c, T_RBRACKET, "expected ']'");
            emit(c, OP_GETIDX);
            continue;
        }
        if (allow_call && check(c, T_LPAREN)) {
            u32 argc = 0;
            next(c);
            /* the callee is on the stack; a plain call has undefined `this` */
            emit(c, OP_UNDEF);
            emit(c, OP_SWAP);                     /* -> fn self  (see note)   */
            while (!check(c, T_RPAREN) && !c->haderr) {
                assign_expr(c); argc++;
                if (!accept(c, T_COMMA)) break;
            }
            expect(c, T_RPAREN, "expected ')' after arguments");
            emit(c, OP_CALL); emit(c, (u8)argc);
            continue;
        }
        return;
    }
}

/* A member expression that keeps the receiver for a method call. Compiles
 * `a.b.c(x)` so that `this` inside c is `a.b`. */
static void postfix_chain(compiler *c)
{
    for (;;) {
        if (c->haderr) return;
        if (accept(c, T_DOT)) {
            u32 a;
            if (!check(c, T_IDENT) && !(c->lx.tok >= T_VAR && c->lx.tok <= T_OF)) {
                cerr(c, "expected property name after '.'", NULL); return; }
            a = tok_atom(c);
            next(c);
            if (check(c, T_LPAREN)) {             /* method call: keep receiver */
                u32 argc = 0;
                next(c);
                emit(c, OP_DUP);                  /* obj obj                   */
                emit(c, OP_GETPROP); emit_u32(c, a);  /* obj fn                */
                emit(c, OP_SWAP);                 /* fn obj                    */
                while (!check(c, T_RPAREN) && !c->haderr) {
                    assign_expr(c); argc++;
                    if (!accept(c, T_COMMA)) break;
                }
                expect(c, T_RPAREN, "expected ')' after arguments");
                emit(c, OP_CALL); emit(c, (u8)argc);
                continue;
            }
            emit(c, OP_GETPROP); emit_u32(c, a);
            continue;
        }
        if (accept(c, T_LBRACKET)) {
            expression(c);
            expect(c, T_RBRACKET, "expected ']'");
            if (check(c, T_LPAREN)) {             /* obj[k](...) */
                u32 argc = 0;
                next(c);
                /* stack: obj key -> need fn self. GETIDX consumes both, so
                 * duplicate the receiver under the key first. */
                emit(c, OP_SWAP);                 /* key obj                   */
                emit(c, OP_DUP);                  /* key obj obj               */
                emit(c, OP_ROT3);                 /* obj obj key               */
                emit(c, OP_GETIDX);               /* obj fn                    */
                emit(c, OP_SWAP);                 /* fn obj                    */
                while (!check(c, T_RPAREN) && !c->haderr) {
                    assign_expr(c); argc++;
                    if (!accept(c, T_COMMA)) break;
                }
                expect(c, T_RPAREN, "expected ')' after arguments");
                emit(c, OP_CALL); emit(c, (u8)argc);
                continue;
            }
            emit(c, OP_GETIDX);
            continue;
        }
        if (check(c, T_LPAREN)) {
            u32 argc = 0;
            next(c);
            emit(c, OP_UNDEF);                    /* fn undefined              */
            while (!check(c, T_RPAREN) && !c->haderr) {
                assign_expr(c); argc++;
                if (!accept(c, T_COMMA)) break;
            }
            expect(c, T_RPAREN, "expected ')' after arguments");
            emit(c, OP_CALL); emit(c, (u8)argc);
            continue;
        }
        return;
    }
}

static void unary(compiler *c);

static void postfix(compiler *c)
{
    fnscope *f = c->fn;
    u32 start;
    if (check(c, T_NEW)) { new_expr(c); postfix_chain(c); return; }
    start = f->nbc;
    primary(c);
    postfix_chain(c);
    /* `x++` / `x--`: the expression's value is the OLD one, so duplicate it
     * before the increment and drop the store's result.
     *   load  -> [old]      DUP -> [old old]     ONE ADD -> [old new]
     *   store -> [old new]  POP -> [old]                                     */
    if ((check(c, T_PLUSPLUS) || check(c, T_MINUSMINUS)) && !c->lx.nl_before) {
        int isinc = check(c, T_PLUSPLUS);
        u8 op;
        u32 last = start, i = start;
        while (i < f->nbc) { u8 o = f->bc[i]; last = i;
                             i += (o < OP__COUNT && ujs_op_len[o]) ? ujs_op_len[o] : 1; }
        op = f->bc[last];
        if (op != OP_GETLOC && op != OP_GETGLOB) {
            cerr(c, "postfix ++/-- is only supported on a variable", NULL);
            return;
        }
        {   u8 depth = 0; u32 slot = 0, atom = 0;
            if (op == OP_GETLOC) { depth = f->bc[last+1];
                                   slot = (u32)f->bc[last+2] | ((u32)f->bc[last+3] << 8); }
            else atom = (u32)f->bc[last+1] | ((u32)f->bc[last+2] << 8)
                      | ((u32)f->bc[last+3] << 16) | ((u32)f->bc[last+4] << 24);
            next(c);
            emit(c, OP_DUP);
            emit(c, OP_ONE);
            emit(c, isinc ? OP_ADD : OP_SUB);
            if (op == OP_GETLOC) { emit(c, OP_SETLOC); emit(c, depth); emit_u16(c, slot); }
            else { emit(c, OP_SETGLOB); emit_u32(c, atom); }
            emit(c, OP_POP);
        }
    }
}

/* ---- unary / binary ladder ----------------------------------------------- */
static void unary(compiler *c)
{
    if (c->haderr) return;
    switch (c->lx.tok) {
    case T_BANG:   next(c); unary(c); emit(c, OP_NOT); return;
    case T_TILDE:  next(c); unary(c); emit(c, OP_BITNOT); return;
    case T_MINUS:  next(c); unary(c); emit(c, OP_NEG); return;
    case T_PLUS:   next(c); unary(c); emit(c, OP_POS); return;
    case T_TYPEOF: {
        /* `typeof undeclaredThing` is defined to be "undefined", NOT a
         * ReferenceError - and real pages feature-detect with it constantly.
         * When the operand compiled to exactly one global load, soften it. */
        u32 start = c->fn->nbc;
        next(c);
        unary(c);
        if (c->fn->nbc == start + ujs_op_len[OP_GETGLOB] && c->fn->bc[start] == OP_GETGLOB)
            c->fn->bc[start] = OP_GETGLOB_SOFT;
        emit(c, OP_TYPEOF);
        return; }
    case T_VOID:   next(c); unary(c); emit(c, OP_POP); emit(c, OP_UNDEF); return;
    case T_DELETE: {
        next(c);
        /* delete o.k / delete o[k]: compile the object, then the key */
        primary(c);
        for (;;) {
            if (accept(c, T_DOT)) {
                u32 a;
                if (!check(c, T_IDENT)) { cerr(c, "expected property name", NULL); return; }
                a = tok_atom(c); next(c);
                if (check(c, T_DOT) || check(c, T_LBRACKET)) { emit(c, OP_GETPROP); emit_u32(c, a); continue; }
                emit(c, OP_DELPROP); emit_u32(c, a);
                return;
            }
            if (accept(c, T_LBRACKET)) {
                expression(c);
                expect(c, T_RBRACKET, "expected ']'");
                if (check(c, T_DOT) || check(c, T_LBRACKET)) { emit(c, OP_GETIDX); continue; }
                emit(c, OP_DELIDX);
                return;
            }
            emit(c, OP_POP); emit(c, OP_TRUE);    /* delete of a non-reference */
            return;
        }
    }
    case T_PLUSPLUS: case T_MINUSMINUS: {
        int isinc = check(c, T_PLUSPLUS);
        u32 a;
        next(c);
        if (!check(c, T_IDENT)) { cerr(c, "expected a variable after ++/--", NULL); return; }
        a = tok_atom(c); next(c);
        emit_load_name(c, a);
        emit(c, OP_ONE);
        emit(c, isinc ? OP_ADD : OP_SUB);
        emit_store_name(c, a);                    /* leaves the new value      */
        return; }
    default: postfix(c); return;
    }
}

/* precedence climbing over the binary operators */
static int binprec(int t)
{
    switch (t) {
    case T_STARSTAR: return 11;
    case T_STAR: case T_SLASH: case T_PERCENT: return 10;
    case T_PLUS: case T_MINUS: return 9;
    case T_SHL: case T_SHR: case T_USHR: return 8;
    case T_LT: case T_LE: case T_GT: case T_GE:
    case T_IN: case T_INSTANCEOF: return 7;
    case T_EQ: case T_NE: case T_SEQ: case T_SNE: return 6;
    case T_AMP: return 5;
    case T_CARET: return 4;
    case T_PIPE: return 3;
    case T_ANDAND: return 2;
    case T_OROR: return 1;
    default: return 0;
    }
}

static u8 binop(int t)
{
    switch (t) {
    case T_PLUS: return OP_ADD;   case T_MINUS: return OP_SUB;
    case T_STAR: return OP_MUL;   case T_SLASH: return OP_DIV;
    case T_PERCENT: return OP_MOD; case T_STARSTAR: return OP_POW;
    case T_SHL: return OP_SHL;    case T_SHR: return OP_SHR;
    case T_USHR: return OP_USHR;
    case T_LT: return OP_LT;      case T_LE: return OP_LE;
    case T_GT: return OP_GT;      case T_GE: return OP_GE;
    case T_EQ: return OP_EQ;      case T_NE: return OP_NE;
    case T_SEQ: return OP_SEQ;    case T_SNE: return OP_SNE;
    case T_AMP: return OP_AND;    case T_CARET: return OP_XOR;
    case T_PIPE: return OP_OR;
    case T_IN: return OP_IN;      case T_INSTANCEOF: return OP_INSTANCEOF;
    default: return OP_NOP;
    }
}

static void binary(compiler *c, int minprec)
{
    unary(c);
    for (;;) {
        int t = c->lx.tok, p = binprec(t);
        if (c->haderr || p == 0 || p < minprec) return;
        next(c);
        if (t == T_ANDAND || t == T_OROR) {       /* short-circuit             */
            int j = emit_jump(c, t == T_ANDAND ? OP_JF_KEEP : OP_JT_KEEP);
            emit(c, OP_POP);
            binary(c, p + 1);
            patch_here(c, j);
            continue;
        }
        /* ** is right-associative; the rest are left */
        binary(c, t == T_STARSTAR ? p : p + 1);
        emit(c, binop(t));
    }
}

static void conditional(compiler *c)
{
    binary(c, 1);
    if (accept(c, T_QUESTION)) {
        int jf, jend;
        jf = emit_jump(c, OP_JF);
        assign_expr(c);
        jend = emit_jump(c, OP_JMP);
        patch_here(c, jf);
        expect(c, T_COLON, "expected ':' in conditional");
        assign_expr(c);
        patch_here(c, jend);
    }
}

/* Assignment needs the target's shape, which we only learn after parsing the
 * left side. Rather than build an AST, remember where the left side's code
 * began and what its last instruction was: a GETLOC/GETGLOB/GETPROP/GETIDX is
 * a valid target and is rewritten into the matching store. */
static void assign_expr(compiler *c)
{
    fnscope *f = c->fn;
    u32 start = f->nbc;
    int t;
    conditional(c);
    t = c->lx.tok;
    if (t != T_ASSIGN && !(t >= T_PLUSEQ && t <= T_USHREQ)) return;
    if (c->haderr) return;

    /* find the instruction that produced the value: it must be the last one */
    {   u32 last = start, i = start;
        u8 op;
        /* walk the emitted range to find the final instruction boundary */
        while (i < f->nbc) {
            u8 o = f->bc[i];
            last = i;
            i += (o < OP__COUNT && ujs_op_len[o]) ? ujs_op_len[o] : 1;
        }
        op = f->bc[last];
        next(c);                                   /* consume the operator     */

        if (op == OP_GETLOC || op == OP_GETGLOB) {
            /* compound: reload, combine, then store */
            u8 depth = 0; u32 slot = 0, atom = 0;
            if (op == OP_GETLOC) { depth = f->bc[last+1]; slot = (u32)f->bc[last+2] | ((u32)f->bc[last+3] << 8); }
            else atom = (u32)f->bc[last+1] | ((u32)f->bc[last+2] << 8)
                      | ((u32)f->bc[last+3] << 16) | ((u32)f->bc[last+4] << 24);
            if (t != T_ASSIGN) {
                assign_expr(c);                    /* value on top of the load  */
                emit(c, binop(t == T_PLUSEQ ? T_PLUS : t == T_MINUSEQ ? T_MINUS :
                              t == T_STAREQ ? T_STAR : t == T_SLASHEQ ? T_SLASH :
                              t == T_PERCENTEQ ? T_PERCENT : t == T_AMPEQ ? T_AMP :
                              t == T_PIPEEQ ? T_PIPE : t == T_CARETEQ ? T_CARET :
                              t == T_SHLEQ ? T_SHL : t == T_SHREQ ? T_SHR : T_USHR));
            } else {
                f->nbc = last;                     /* drop the load entirely    */
                assign_expr(c);
            }
            if (op == OP_GETLOC) { emit(c, OP_SETLOC); emit(c, depth); emit_u16(c, slot); }
            else { emit(c, OP_SETGLOB); emit_u32(c, atom); }
            return;
        }
        if (op == OP_GETPROP) {
            u32 atom = (u32)f->bc[last+1] | ((u32)f->bc[last+2] << 8)
                     | ((u32)f->bc[last+3] << 16) | ((u32)f->bc[last+4] << 24);
            if (t != T_ASSIGN) {
                /* `o.k += v` needs the RECEIVER still on the stack under the
                 * old value. Rewriting the load's opcode in place does that
                 * without shifting any bytecode (the KEEP form is the same
                 * length), so no jump target can be invalidated. */
                f->bc[last] = OP_GETPROP_KEEP;
                assign_expr(c);
                emit(c, binop(t == T_PLUSEQ ? T_PLUS : t == T_MINUSEQ ? T_MINUS :
                              t == T_STAREQ ? T_STAR : t == T_SLASHEQ ? T_SLASH :
                              t == T_PERCENTEQ ? T_PERCENT : t == T_AMPEQ ? T_AMP :
                              t == T_PIPEEQ ? T_PIPE : t == T_CARETEQ ? T_CARET :
                              t == T_SHLEQ ? T_SHL : t == T_SHREQ ? T_SHR : T_USHR));
            } else {
                f->nbc = last;                     /* leaves the object on top  */
                assign_expr(c);
            }
            emit(c, OP_SETPROP); emit_u32(c, atom);
            return;
        }
        if (op == OP_GETIDX) {
            if (t != T_ASSIGN) {
                f->bc[last] = OP_GETIDX_KEEP;      /* obj key -> obj key old   */
                assign_expr(c);
                emit(c, binop(t == T_PLUSEQ ? T_PLUS : t == T_MINUSEQ ? T_MINUS :
                              t == T_STAREQ ? T_STAR : t == T_SLASHEQ ? T_SLASH :
                              t == T_PERCENTEQ ? T_PERCENT : t == T_AMPEQ ? T_AMP :
                              t == T_PIPEEQ ? T_PIPE : t == T_CARETEQ ? T_CARET :
                              t == T_SHLEQ ? T_SHL : t == T_SHREQ ? T_SHR : T_USHR));
            } else {
                f->nbc = last;                     /* leaves obj + key on top   */
                assign_expr(c);
            }
            emit(c, OP_SETIDX);
            return;
        }
        cerr(c, "invalid assignment target", NULL);
    }
}

static void expression(compiler *c)
{
    assign_expr(c);
    while (accept(c, T_COMMA)) { emit(c, OP_POP); assign_expr(c); }
}

/* ---- statements ---------------------------------------------------------- */
static void var_decl(compiler *c, int is_global_scope)
{
    for (;;) {
        u32 a;
        if (!check(c, T_IDENT)) { cerr(c, "expected a variable name", NULL); return; }
        a = tok_atom(c);
        next(c);
        if (is_global_scope) { emit(c, OP_DECLGLOB); emit_u32(c, a); }
        else local_add(c, a);
        if (accept(c, T_ASSIGN)) {
            assign_expr(c);
            emit_store_name(c, a);
            emit(c, OP_POP);
        }
        if (!accept(c, T_COMMA)) break;
    }
}

static void loop_begin(compiler *c, loopctx *L)
{ memset(L, 0, sizeof *L); L->continue_target = -1; L->prev = c->fn->loops; c->fn->loops = L; }

static void loop_end(compiler *c, loopctx *L, u32 cont_target)
{
    pl_patch(c, &L->breaks, c->fn->nbc);
    pl_patch(c, &L->continues, cont_target);
    c->fn->loops = L->prev;
}

static void if_stmt(compiler *c)
{
    int jf, jend;
    expect(c, T_LPAREN, "expected '(' after if");
    expression(c);
    expect(c, T_RPAREN, "expected ')' after the if condition");
    jf = emit_jump(c, OP_JF);
    statement(c);
    if (accept(c, T_ELSE)) {
        jend = emit_jump(c, OP_JMP);
        patch_here(c, jf);
        statement(c);
        patch_here(c, jend);
    } else patch_here(c, jf);
}

static void while_stmt(compiler *c)
{
    loopctx L;
    u32 top = c->fn->nbc;
    int jf;
    loop_begin(c, &L);
    expect(c, T_LPAREN, "expected '(' after while");
    expression(c);
    expect(c, T_RPAREN, "expected ')' after the while condition");
    jf = emit_jump(c, OP_JF);
    statement(c);
    { int j = emit_jump(c, OP_JMP); patch_to(c, j, top); }
    patch_here(c, jf);
    loop_end(c, &L, top);
}

static void do_stmt(compiler *c)
{
    loopctx L;
    u32 top = c->fn->nbc, cond;
    loop_begin(c, &L);
    statement(c);
    cond = c->fn->nbc;
    expect(c, T_WHILE, "expected 'while' after a do body");
    expect(c, T_LPAREN, "expected '(' after while");
    expression(c);
    expect(c, T_RPAREN, "expected ')'");
    { int j = emit_jump(c, OP_JT); patch_to(c, j, top); }
    semicolon(c);
    loop_end(c, &L, cond);
}

static void for_stmt(compiler *c)
{
    loopctx L;
    expect(c, T_LPAREN, "expected '(' after for");
    /* for-in / for-of: `for (var? x in|of expr)` */
    {   ujs_lexpos save;
        int isdecl = check(c, T_VAR) || check(c, T_LET) || check(c, T_CONST);
        ujs_lex_save(&c->lx, &save);
        if (isdecl) next(c);
        if (check(c, T_IDENT)) {
            u32 a = tok_atom(c);
            next(c);
            if (check(c, T_IN) || check(c, T_OF)) {
                int isof = check(c, T_OF);
                u32 top;
                int jdone;
                next(c);
                if (isdecl && !c->fn->is_global) local_add(c, a);
                else if (isdecl) { emit(c, OP_DECLGLOB); emit_u32(c, a); }
                expression(c);
                expect(c, T_RPAREN, "expected ')' after the for-in/of head");
                emit(c, isof ? OP_ITER_OF : OP_ITER);
                loop_begin(c, &L);
                top = c->fn->nbc;
                jdone = emit_jump(c, OP_ITER_NEXT);
                emit_store_name(c, a);
                emit(c, OP_POP);
                statement(c);
                { int j = emit_jump(c, OP_JMP); patch_to(c, j, top); }
                patch_here(c, jdone);
                emit(c, OP_POP);                   /* drop the iterator state  */
                loop_end(c, &L, top);
                return;
            }
        }
        ujs_lex_restore(&c->lx, &save);
        next(c);
    }
    /* classic three-clause for */
    if (!check(c, T_SEMI)) {
        if (accept(c, T_VAR) || accept(c, T_LET) || accept(c, T_CONST))
            var_decl(c, c->fn->is_global);
        else { expression(c); emit(c, OP_POP); }
    }
    expect(c, T_SEMI, "expected ';' in the for head");
    {   u32 top = c->fn->nbc, upd;
        int jf = -1, jbody, jcond;
        loop_begin(c, &L);
        if (!check(c, T_SEMI)) { expression(c); jf = emit_jump(c, OP_JF); }
        expect(c, T_SEMI, "expected ';' in the for head");
        jbody = emit_jump(c, OP_JMP);              /* skip the update the first
                                                    * time round               */
        upd = c->fn->nbc;
        if (!check(c, T_RPAREN)) { expression(c); emit(c, OP_POP); }
        jcond = emit_jump(c, OP_JMP); patch_to(c, jcond, top);
        expect(c, T_RPAREN, "expected ')' after the for head");
        patch_here(c, jbody);
        statement(c);
        { int j = emit_jump(c, OP_JMP); patch_to(c, j, upd); }
        if (jf >= 0) patch_here(c, jf);
        loop_end(c, &L, upd);
    }
}

static void try_stmt(compiler *c)
{
    int at_try, jafter;
    u32 catch_ip = 0, finally_ip = 0;
    at_try = (int)c->fn->nbc;
    emit(c, OP_TRY); emit_u32(c, 0); emit_u32(c, 0);
    expect(c, T_LBRACE, "expected '{' after try");
    block(c);
    emit(c, OP_ENDTRY);
    jafter = emit_jump(c, OP_JMP);
    if (accept(c, T_CATCH)) {
        u32 a = 0;
        catch_ip = c->fn->nbc;
        if (accept(c, T_LPAREN)) {
            if (!check(c, T_IDENT)) cerr(c, "expected a catch parameter", NULL);
            else { a = tok_atom(c); next(c); }
            expect(c, T_RPAREN, "expected ')' after the catch parameter");
        }
        /* the VM pushes the thrown value; bind it (or drop it) */
        if (a) {
            if (c->fn->is_global) { emit(c, OP_DECLGLOB); emit_u32(c, a); }
            else local_add(c, a);
            emit_store_name(c, a);
        }
        emit(c, OP_POP);
        expect(c, T_LBRACE, "expected '{' after catch");
        block(c);
    }
    patch_here(c, jafter);
    if (accept(c, T_FINALLY)) {
        finally_ip = c->fn->nbc;
        expect(c, T_LBRACE, "expected '{' after finally");
        block(c);
        emit(c, OP_FINALLY_END);
    }
    /* patch the handler targets into the OP_TRY operands */
    patch_to(c, at_try + 1, catch_ip);
    patch_to(c, at_try + 5, finally_ip);
    /* a try with a finally runs it on the normal path too */
    if (finally_ip) {
        /* the normal path fell through to the finally body already because it
         * is emitted right after jafter's target - nothing more to do. */
    }
}

/* switch, laid out so that FALLTHROUGH still works in one pass:
 *
 *   test_i : DUP <expr> SEQ  JT->body_i   JMP->test_{i+1}
 *   body_i : <statements>    JMP->body_{i+1}
 *
 * Each test's "no match" jump is patched when the NEXT test begins (so a
 * `default:` body sitting between them is skipped by the test chain), and each
 * body's trailing jump is patched when the next body begins - which is exactly
 * what makes a case without `break` fall into the following one. */
static void switch_stmt(compiler *c)
{
    loopctx L;
    int pending_test = -1, pending_body = -1, default_body = -1;
    expect(c, T_LPAREN, "expected '(' after switch");
    expression(c);
    expect(c, T_RPAREN, "expected ')' after the switch subject");
    expect(c, T_LBRACE, "expected '{' after switch");
    loop_begin(c, &L);
    L.is_switch = 1;
    while ((check(c, T_CASE) || check(c, T_DEFAULT)) && !c->haderr) {
        if (accept(c, T_CASE)) {
            int jt;
            if (pending_test >= 0) { patch_here(c, pending_test); pending_test = -1; }
            emit(c, OP_DUP);
            assign_expr(c);
            emit(c, OP_SEQ);
            jt = emit_jump(c, OP_JT);
            pending_test = emit_jump(c, OP_JMP);
            expect(c, T_COLON, "expected ':' after a case label");
            patch_here(c, jt);                        /* body starts here */
        } else {
            next(c);
            expect(c, T_COLON, "expected ':' after default");
            default_body = (int)c->fn->nbc;
        }
        if (pending_body >= 0) { patch_here(c, pending_body); pending_body = -1; }
        while (!check(c, T_CASE) && !check(c, T_DEFAULT) && !check(c, T_RBRACE)
               && !check(c, T_EOF) && !c->haderr)
            statement(c);
        pending_body = emit_jump(c, OP_JMP);
    }
    expect(c, T_RBRACE, "expected '}' closing switch");
    if (pending_test >= 0) {
        if (default_body >= 0) patch_to(c, pending_test, (u32)default_body);
        else patch_here(c, pending_test);
    }
    if (pending_body >= 0) patch_here(c, pending_body);
    loop_end(c, &L, c->fn->nbc);                      /* `break` lands here */
    emit(c, OP_POP);                                  /* drop the subject   */
}

static void block(compiler *c)
{
    while (!check(c, T_RBRACE) && !check(c, T_EOF) && !c->haderr) statement(c);
    expect(c, T_RBRACE, "expected '}'");
}

static void statement(compiler *c)
{
    if (c->haderr) return;
    switch (c->lx.tok) {
    case T_SEMI: next(c); return;
    case T_LBRACE: next(c); block(c); return;
    case T_VAR: case T_LET: case T_CONST:
        next(c); var_decl(c, c->fn->is_global); semicolon(c); return;
    case T_IF: next(c); if_stmt(c); return;
    case T_WHILE: next(c); while_stmt(c); return;
    case T_DO: next(c); do_stmt(c); return;
    case T_FOR: next(c); for_stmt(c); return;
    case T_SWITCH: next(c); switch_stmt(c); return;
    case T_TRY: next(c); try_stmt(c); return;
    case T_RETURN:
        next(c);
        if (check(c, T_SEMI) || check(c, T_RBRACE) || check(c, T_EOF) || c->lx.nl_before)
            emit(c, OP_UNDEF);
        else expression(c);
        semicolon(c);
        emit(c, OP_RET);
        return;
    case T_THROW:
        next(c);
        expression(c);
        semicolon(c);
        emit(c, OP_THROW);
        return;
    case T_BREAK: {
        loopctx *L = c->fn->loops;
        next(c); semicolon(c);
        if (!L) { cerr(c, "'break' outside a loop", NULL); return; }
        pl_add(c, &L->breaks, emit_jump(c, OP_JMP));
        return; }
    case T_CONTINUE: {
        loopctx *L = c->fn->loops;
        next(c); semicolon(c);
        while (L && L->is_switch) L = L->prev;    /* a switch is not a loop */
        if (!L) { cerr(c, "'continue' outside a loop", NULL); return; }
        pl_add(c, &L->continues, emit_jump(c, OP_JMP));
        return; }
    case T_FUNCTION:
        /* already compiled and bound by emit_hoisted(); just consume it */
        skip_function_decl(c);
        return;
    default:
        expression(c);
        semicolon(c);
        /* At the top level the value of the last expression statement is the
         * script's completion value - what ujs_eval() hands back. Inside a
         * function nobody can observe it, so it is simply dropped. */
        emit(c, c->fn->is_global ? OP_SETRES : OP_POP);
        return;
    }
}

/* ---- function compilation ------------------------------------------------ */
static ujs_code *finish(compiler *c, fnscope *f)
{
    ujs_code *code;
    if (c->haderr) return NULL;
    code = (ujs_code *)ujs_gc_alloc(c->vm, sizeof(ujs_code), H_CODE);
    if (!code) { cerr(c, "out of memory", NULL); return NULL; }
    code->bc = f->bc; code->nbc = f->nbc;
    code->consts = f->consts; code->nconsts = f->nconsts;
    code->nparams = f->nparams;
    code->nslots = f->nlocals;
    code->name_atom = f->name_atom;
    code->is_arrow = f->is_arrow;
    f->bc = NULL; f->consts = NULL;                /* ownership moved to code  */
    return code;
}

static void scope_dispose(compiler *c, fnscope *f)
{
    if (f->bc)     ujs_free_raw(c->vm, f->bc, f->bccap);
    if (f->consts) ujs_free_raw(c->vm, f->consts, (size_t)f->constcap * sizeof *f->consts);
    if (f->locals) ujs_free_raw(c->vm, f->locals, (size_t)f->localcap * sizeof *f->locals);
    if (f->hoists) ujs_free_raw(c->vm, f->hoists, (size_t)f->hoistcap * sizeof *f->hoists);
}

static ujs_code *compile_function(compiler *c, u32 name_atom, int is_arrow,
                                  int arrow_single_param, u32 single_param_atom)
{
    fnscope f;
    fnscope *saved = c->fn;
    ujs_code *code;
    memset(&f, 0, sizeof f);
    f.parent = saved;
    f.name_atom = name_atom;
    f.is_arrow = is_arrow;
    c->fn = &f;

    /* Parameters occupy slots 0..nparams-1; the VM copies arguments straight
     * into them, so nothing may be declared ahead of them. (A named function
     * EXPRESSION therefore cannot see its own name inside itself - a v1 gap in
     * UNOJS.md. Function DECLARATIONS are unaffected: their name is a local of
     * the ENCLOSING scope, so ordinary recursion works.) */
    if (arrow_single_param) {
        local_add(c, single_param_atom);
        f.nparams = 1;
    } else {
        expect(c, T_LPAREN, "expected '(' before the parameter list");
        while (!check(c, T_RPAREN) && !c->haderr) {
            if (!check(c, T_IDENT)) { cerr(c, "expected a parameter name", NULL); break; }
            local_add(c, tok_atom(c));
            f.nparams++;
            next(c);
            if (!accept(c, T_COMMA)) break;
        }
        expect(c, T_RPAREN, "expected ')' after the parameter list");
        if (is_arrow) expect(c, T_ARROW, "expected '=>'");
    }

    if (is_arrow && !check(c, T_LBRACE)) {         /* concise body: `x => expr` */
        assign_expr(c);
        emit(c, OP_RET);
    } else {
        expect(c, T_LBRACE, "expected '{' before the function body");
        prescan_body(c, 1);
        emit_hoisted(c);
        block(c);
        emit(c, OP_UNDEF);
        emit(c, OP_RET);
    }
    code = finish(c, &f);
    scope_dispose(c, &f);
    c->fn = saved;
    return code;
}

/* ---- entry point --------------------------------------------------------- */
ujs_code *ujs_compile(ujs_vm *vm, const char *src, int len, char *err, size_t errn)
{
    compiler c;
    fnscope f;
    ujs_code *code;
    memset(&c, 0, sizeof c);
    memset(&f, 0, sizeof f);
    c.vm = vm;
    f.is_global = 1;
    c.fn = &f;
    ujs_lex_init(&c.lx, vm, src, len);
    next(&c);
    prescan_body(&c, 0);          /* top-level `function` declarations hoist too */
    emit_hoisted(&c);
    while (!check(&c, T_EOF) && !c.haderr) statement(&c);
    emit(&c, OP_GETRES);          /* the script's completion value */
    emit(&c, OP_RET);
    code = finish(&c, &f);
    if (!code && err && errn) snprintf(err, errn, "%s", c.err[0] ? c.err : "compile failed");
    scope_dispose(&c, &f);
    ujs_lex_free(&c.lx);
    return code;
}
