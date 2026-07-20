/* ===========================================================================
 * ucc backend - x86-64 machine code straight from the AST, and the .UNO
 * container writer.
 *
 * Model: classic accumulator/stack lowering (the chibicc shape).  Every
 * expression leaves its value in RAX; binary operators evaluate the RHS,
 * push it, evaluate the LHS, pop the RHS into RCX and combine.  Only the
 * volatile registers RAX/RCX/RDX/R8-R11 are ever touched, so nothing needs
 * saving beyond RBP, and the generated code is automatically safe to mix
 * with the -O2 mingw kernel it calls into.
 *
 * ABI: Microsoft x64 exactly (RCX/RDX/R8/R9, 32-byte shadow space, RSP
 * 16-aligned at every call) - the kernel is mingw-built, and the loader
 * calls the module's entry/vtable with the same convention.  The generator
 * counts its own transient pushes (G.depth) and pads each call site so the
 * alignment rule holds no matter how deep an expression the call sits in.
 *
 * Canonical register form: a value of type T sits in RAX truncated to
 * max(4, sizeof T) bytes; 8/16-bit values are kept 32-bit-extended per
 * their signedness.  All arithmetic on <=4-byte types uses 32-bit ALU ops
 * (so int wraparound is exact); widening to 8 bytes happens only through
 * the ND_CAST nodes the front end inserts.
 *
 * Position independence: all data access is RIP-relative; imports are
 * `call [rip+slot]` into .unoimp-format records; the only base relocations
 * emitted are for address constants in static initializers (pref_base 0),
 * which pc64_modload.c rebases exactly like mkuno.py output.
 * ======================================================================== */
#include "ucc_int.h"

/* ---- byte emission --------------------------------------------------------- */
static void eb(Cc *cc, int b)
{
    if (cc->codelen >= cc->codecap) ucc_fatal(cc, "code section overflow", 0);
    cc->code[cc->codelen++] = (u8)b;
}
static void e32(Cc *cc, u32 v)
{ eb(cc, (int)(v & 255)); eb(cc, (int)((v >> 8) & 255));
  eb(cc, (int)((v >> 16) & 255)); eb(cc, (int)(v >> 24)); }
static void e64(Cc *cc, u64 v) { e32(cc, (u32)v); e32(cc, (u32)(v >> 32)); }
static void ebytes(Cc *cc, const u8 *p, int n) { int i; for (i = 0; i < n; i++) eb(cc, p[i]); }
#define EB(...) do { static const u8 _b[] = { __VA_ARGS__ }; ebytes(cc, _b, (int)sizeof _b); } while (0)

/* ---- labels (function-local rel32 patching) --------------------------------- */
typedef struct LSite { int site; struct LSite *next; } LSite;
typedef struct Lbl   { int pos; LSite *sites; } Lbl;

static Lbl *lbl_new(Cc *cc)
{
    Lbl *l = ucc_alloc(cc, sizeof *l);
    l->pos = -1; l->sites = 0;
    return l;
}
static void lbl_ref(Cc *cc, Lbl *l)        /* emit a rel32 aimed at l */
{
    if (l->pos >= 0) { e32(cc, (u32)(l->pos - (cc->codelen + 4))); return; }
    { LSite *s = ucc_alloc(cc, sizeof *s);
      s->site = cc->codelen; s->next = l->sites; l->sites = s;
      e32(cc, 0); }
}
static void lbl_bind(Cc *cc, Lbl *l)
{
    LSite *s;
    l->pos = cc->codelen;
    for (s = l->sites; s; s = s->next)
        *(u32 *)(cc->code + s->site) = (u32)(l->pos - (s->site + 4));
    l->sites = 0;
}
static void jmp_lbl(Cc *cc, Lbl *l)           { eb(cc, 0xE9); lbl_ref(cc, l); }
static void jcc_lbl(Cc *cc, int code, Lbl *l) { eb(cc, 0x0F); eb(cc, code); lbl_ref(cc, l); }

/* ---- fixups (cross-section rel32, patched at layout time) ------------------- */
static void fixup(Cc *cc, int kind, int target, Sym *sym)
{
    Fixup *f = ucc_alloc(cc, sizeof *f);
    f->kind = (u8)kind; f->site = cc->codelen;
    f->target = target; f->targetsym = sym;
    f->next = cc->fixups; cc->fixups = f;
    e32(cc, 0);
}

/* ---- imports ----------------------------------------------------------------- */
static int import_name(Cc *cc, const char *name)
{
    Import *im;
    int n = (int)strlen(name);
    for (im = cc->imports; im; im = im->next)
        if (!strcmp(im->name, name)) return im->idx;
    if (n > 23) ucc_fatal(cc, "import name too long", name);
    if (!ucc_import_ok(name, n))
        ucc_fatal(cc, "not a kernel export", name);
    im = ucc_alloc(cc, sizeof *im);
    im->name = name;
    im->idx = cc->nimports++;
    im->next = cc->imports; cc->imports = im;
    return im->idx;
}
static void import_sym(Cc *cc, Sym *s)
{
    if (s->impno >= 0) return;
    s->impno = import_name(cc, s->name);
    s->cls = SYM_IMPORT;
}

/* ============================================================================
 * generator state + primitives
 * ========================================================================== */
typedef struct G {
    Cc  *cc;
    int  depth;              /* 8-byte pushes outstanding since the prologue */
    Lbl *ret;
    Lbl *brk, *cont;
} G;

static void gen_expr(G *g, Node *n);
static void gen_stmt(G *g, Node *n);

static void push_rax(G *g) { eb(g->cc, 0x50); g->depth++; }
static void pop_rcx(G *g)  { eb(g->cc, 0x59); g->depth--; }
static void pop_rdx(G *g)  { eb(g->cc, 0x5A); g->depth--; }
static void pop_r8(G *g)   { eb(g->cc, 0x41); eb(g->cc, 0x58); g->depth--; }
static void pop_r9(G *g)   { eb(g->cc, 0x41); eb(g->cc, 0x59); g->depth--; }

static int ty_w(const Type *t) { return t->size > 4; }   /* 1 = 64-bit op */

static void mov_rax_imm(Cc *cc, i64 v)
{
    if (v >= -2147483647L - 1 && v < 0) { EB(0x48, 0xC7, 0xC0); e32(cc, (u32)v); }
    else if ((u64)v <= 0xFFFFFFFFull)   { eb(cc, 0xB8); e32(cc, (u32)v); }
    else if (v >= -2147483647L - 1 && v <= 2147483647L)
                                        { EB(0x48, 0xC7, 0xC0); e32(cc, (u32)v); }
    else                                { EB(0x48, 0xB8); e64(cc, (u64)v); }
}

static void lea_rbp(Cc *cc, int off) { EB(0x48, 0x8D, 0x85); e32(cc, (u32)off); }

/* address of a symbol -> rax */
static void lea_sym(Cc *cc, Sym *s)
{
    if (s->cls == SYM_LOCAL) { lea_rbp(cc, s->lvoff); return; }
    if (s->cls == SYM_IMPORT) {
        EB(0x48, 0x8B, 0x05);                     /* mov rax,[rip+slot] */
        fixup(cc, FX_IMPORT, s->impno, 0);
        return;
    }
    if (s->cls == SYM_FUNC) {
        if (!s->defined && ucc_import_ok(s->name, (int)strlen(s->name))) {
            import_sym(cc, s);
            EB(0x48, 0x8B, 0x05);
            fixup(cc, FX_IMPORT, s->impno, 0);
            return;
        }
        EB(0x48, 0x8D, 0x05);                     /* lea rax,[rip+fn] */
        fixup(cc, FX_CODE, 0, s);
        return;
    }
    EB(0x48, 0x8D, 0x05);                         /* lea rax,[rip+global] */
    fixup(cc, s->sec == SEC_BSS ? FX_BSS : FX_DATA, s->secoff, 0);
}

/* value at [rax] -> rax, canonical form */
static void load_ty(Cc *cc, const Type *t)
{
    if (t->kind == TY_ARR || t->kind == TY_STRUCT || t->kind == TY_UNION ||
        t->kind == TY_FUNC)
        return;                                    /* the address is the value */
    switch (t->size) {
    case 1: if (t->is_uns) EB(0x0F, 0xB6, 0x00); else EB(0x0F, 0xBE, 0x00); break;
    case 2: if (t->is_uns) EB(0x0F, 0xB7, 0x00); else EB(0x0F, 0xBF, 0x00); break;
    case 4: EB(0x8B, 0x00); break;
    default: EB(0x48, 0x8B, 0x00); break;
    }
}

/* [rcx] = rax, width per type */
static void store_ty(Cc *cc, const Type *t)
{
    switch (t->size) {
    case 1: EB(0x88, 0x01); break;
    case 2: EB(0x66, 0x89, 0x01); break;
    case 4: EB(0x89, 0x01); break;
    default: EB(0x48, 0x89, 0x01); break;
    }
}

/* convert rax from 'from' canonical form to 'to' canonical form */
static void cast_rax(Cc *cc, const Type *from, const Type *to)
{
    if (to->kind == TY_VOID) return;
    if (to->size == 8) {
        if (from->size <= 4) {
            if (from->is_uns) EB(0x89, 0xC0);              /* mov eax,eax   */
            else              EB(0x48, 0x63, 0xC0);        /* movsxd rax,eax */
        }
        return;
    }
    switch (to->size) {
    case 1: if (to->is_uns) EB(0x0F, 0xB6, 0xC0); else EB(0x0F, 0xBE, 0xC0); break;
    case 2: if (to->is_uns) EB(0x0F, 0xB7, 0xC0); else EB(0x0F, 0xBF, 0xC0); break;
    default:
        if (from->size == 8) EB(0x89, 0xC0);               /* truncate to 32 */
        break;
    }
}

/* rax = rax OP rcx (width/signedness per type); shifts take the count in cl */
static void alu(Cc *cc, int ndkind, const Type *t)
{
    int w = ty_w(t);
    switch (ndkind) {
    case ND_ADD: if (w) EB(0x48, 0x01, 0xC8); else EB(0x01, 0xC8); break;
    case ND_SUB: if (w) EB(0x48, 0x29, 0xC8); else EB(0x29, 0xC8); break;
    case ND_MUL: if (w) EB(0x48, 0x0F, 0xAF, 0xC1); else EB(0x0F, 0xAF, 0xC1); break;
    case ND_AND: if (w) EB(0x48, 0x21, 0xC8); else EB(0x21, 0xC8); break;
    case ND_OR:  if (w) EB(0x48, 0x09, 0xC8); else EB(0x09, 0xC8); break;
    case ND_XOR: if (w) EB(0x48, 0x31, 0xC8); else EB(0x31, 0xC8); break;
    case ND_DIV:
    case ND_MOD:
        if (t->is_uns) {
            EB(0x31, 0xD2);                                /* xor edx,edx */
            if (w) EB(0x48, 0xF7, 0xF1); else EB(0xF7, 0xF1);
        } else {
            if (w) EB(0x48, 0x99); else EB(0x99);          /* cqo / cdq  */
            if (w) EB(0x48, 0xF7, 0xF9); else EB(0xF7, 0xF9);
        }
        if (ndkind == ND_MOD) {
            if (w) EB(0x48, 0x89, 0xD0); else EB(0x89, 0xD0);
        }
        break;
    case ND_SHL: if (w) EB(0x48, 0xD3, 0xE0); else EB(0xD3, 0xE0); break;
    case ND_SHR:
        if (t->is_uns) { if (w) EB(0x48, 0xD3, 0xE8); else EB(0xD3, 0xE8); }
        else           { if (w) EB(0x48, 0xD3, 0xF8); else EB(0xD3, 0xF8); }
        break;
    default:
        ucc_fatal(cc, "internal: bad alu op", 0);
    }
}

/* compare rax ? rcx -> rax = 0/1 */
static void cmp_set(Cc *cc, int kind, const Type *opty, int rhs_uns)
{
    int uns = opty->is_uns || opty->kind == TY_PTR || rhs_uns;
    if (ty_w(opty)) EB(0x48, 0x39, 0xC8); else EB(0x39, 0xC8);
    switch (kind) {
    case ND_EQ: EB(0x0F, 0x94, 0xC0); break;
    case ND_NE: EB(0x0F, 0x95, 0xC0); break;
    case ND_LT: if (uns) EB(0x0F, 0x92, 0xC0); else EB(0x0F, 0x9C, 0xC0); break;
    case ND_LE: if (uns) EB(0x0F, 0x96, 0xC0); else EB(0x0F, 0x9E, 0xC0); break;
    }
    EB(0x0F, 0xB6, 0xC0);                          /* movzx eax, al */
}

static void test_rax(Cc *cc, const Type *t)
{
    if (t && (ty_w(t) || t->kind == TY_PTR)) EB(0x48, 0x85, 0xC0);
    else                                     EB(0x85, 0xC0);
}

/* ---- lvalues ----------------------------------------------------------------- */
static void gen_addr(G *g, Node *n)
{
    Cc *cc = g->cc;
    switch (n->kind) {
    case ND_VAR: lea_sym(cc, n->sym); return;
    case ND_STR: EB(0x48, 0x8D, 0x05); fixup(cc, FX_DATA, n->stroff, 0); return;
    case ND_DEREF: gen_expr(g, n->lhs); return;
    case ND_MEMBER:
        gen_addr(g, n->lhs);
        if (n->val) { EB(0x48, 0x05); e32(cc, (u32)n->val); }   /* add rax,imm */
        return;
    default:
        ucc_err_tok(cc, n->tok, "not an lvalue", 0);
    }
}

/* ---- calls -------------------------------------------------------------------- */
/* one aligned MS-ABI call with everything already in place; leaves the
 * shadow/pad cleanup to the caller of this helper via the returned drop */
static void emit_call_direct(G *g, Sym *s)
{
    Cc *cc = g->cc;
    if (s->cls == SYM_IMPORT) {
        EB(0xFF, 0x15);
        fixup(cc, FX_IMPORT, s->impno, 0);
    } else {
        eb(cc, 0xE8);
        fixup(cc, FX_CODE, 0, s);
    }
}

static void gen_call(G *g, Node *n)
{
    Cc *cc = g->cc;
    Node *a;
    Node *argv[8];
    int nargs = (int)n->val, i, stackargs, pad;

    for (a = n->args, i = 0; a; a = a->next) argv[i++] = a;
    stackargs = nargs > 4 ? nargs - 4 : 0;
    pad = ((g->depth + stackargs) & 1) ? 1 : 0;
    if (pad) { EB(0x48, 0x83, 0xEC, 0x08); g->depth++; }     /* sub rsp,8 */

    for (i = nargs - 1; i >= 0; i--) {                       /* right to left */
        gen_expr(g, argv[i]);
        /* a small struct arg rides by value: rax holds its address, load
         * the bytes themselves (MS ABI register-class for sizes 1/2/4/8) */
        if (argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) {
            switch (argv[i]->ty->size) {
            case 1: EB(0x0F, 0xB6, 0x00); break;             /* movzx eax,[rax] */
            case 2: EB(0x0F, 0xB7, 0x00); break;
            case 4: EB(0x8B, 0x00); break;
            default: EB(0x48, 0x8B, 0x00); break;
            }
        }
        push_rax(g);
    }
    /* an indirect callee is evaluated BEFORE the argument registers load
     * (its expression may freely clobber rcx/rdx/r8/r9) and parked in r10 */
    if (!n->sym) {
        gen_expr(g, n->lhs);
        EB(0x49, 0x89, 0xC2);                                /* mov r10, rax */
    }
    if (nargs > 0) pop_rcx(g);
    if (nargs > 1) pop_rdx(g);
    if (nargs > 2) pop_r8(g);
    if (nargs > 3) pop_r9(g);
    /* stack args (if any) now sit exactly at [rsp], lowest first */

    if (n->sym) {
        Sym *s = n->sym;
        if (!s->defined && s->cls == SYM_FUNC &&
            ucc_import_ok(s->name, (int)strlen(s->name)))
            import_sym(cc, s);
        EB(0x48, 0x83, 0xEC, 0x20);                          /* sub rsp,32 */
        emit_call_direct(g, s);
    } else {
        EB(0x48, 0x83, 0xEC, 0x20);
        EB(0x41, 0xFF, 0xD2);                                /* call r10 */
    }
    {
        int drop = 32 + 8 * stackargs + (pad ? 8 : 0);
        EB(0x48, 0x81, 0xC4); e32(cc, (u32)drop);            /* add rsp, drop */
        g->depth -= stackargs + pad;
    }
    /* canonicalize small integer returns (the ABI leaves upper bits loose) */
    if (n->ty && ucc_type_is_int(n->ty) && n->ty->size < 4) {
        Type from; memset(&from, 0, sizeof from);
        from.kind = TY_INT; from.size = 4; from.align = 4;
        cast_rax(cc, &from, n->ty);
    }
}

/* struct assignment: dst address on stack top, src address in rax */
static void gen_struct_copy(G *g, int size)
{
    Cc *cc = g->cc;
    int pad;

    if (cc->memcpy_imp < 0) cc->memcpy_imp = import_name(cc, "memcpy");
    EB(0x48, 0x89, 0xC2);                        /* mov rdx, rax (src)  */
    pop_rcx(g);                                  /* dst                 */
    EB(0x41, 0xB8); e32(cc, (u32)size);          /* mov r8d, size       */
    pad = (g->depth & 1) ? 1 : 0;
    if (pad) { EB(0x48, 0x83, 0xEC, 0x08); g->depth++; }
    EB(0x48, 0x83, 0xEC, 0x20);                  /* sub rsp,32          */
    EB(0xFF, 0x15); fixup(cc, FX_IMPORT, cc->memcpy_imp, 0);
    EB(0x48, 0x83, 0xC4); eb(cc, 32 + (pad ? 8 : 0));   /* add rsp, imm8 */
    if (pad) g->depth--;
    /* memcpy returns dst in rax - the assignment's value */
}

/* ---- expressions ---------------------------------------------------------------*/
static void gen_cond_branch(G *g, Node *cond, Lbl *lfalse)
{
    gen_expr(g, cond);
    test_rax(g->cc, cond->ty);
    jcc_lbl(g->cc, 0x84, lfalse);
}

/* rhs value in rax scaled for `ptr += n` style updates */
static void scale_rhs_for_ptr(Cc *cc, Node *rhs, Type *pt)
{
    Type l8; memset(&l8, 0, sizeof l8);
    l8.kind = TY_LONG; l8.size = 8; l8.align = 8;
    cast_rax(cc, rhs->ty, &l8);
    EB(0x48, 0x69, 0xC0);                        /* imul rax, rax, imm32 */
    e32(cc, (u32)(pt->base->size ? pt->base->size : 1));
}

static void gen_expr(G *g, Node *n)
{
    Cc *cc = g->cc;

    switch (n->kind) {
    case ND_NUM: mov_rax_imm(cc, n->val); return;
    case ND_STR: EB(0x48, 0x8D, 0x05); fixup(cc, FX_DATA, n->stroff, 0); return;
    case ND_VAR: case ND_MEMBER:
        gen_addr(g, n);
        load_ty(cc, n->ty);
        return;
    case ND_DEREF:
        gen_expr(g, n->lhs);
        load_ty(cc, n->ty);
        return;
    case ND_ADDR:
        gen_addr(g, n->lhs);
        return;
    case ND_CAST:
        gen_expr(g, n->lhs);
        cast_rax(cc, n->lhs->ty, n->ty);
        return;
    case ND_NEG:
        gen_expr(g, n->lhs);
        cast_rax(cc, n->lhs->ty, n->ty);
        if (ty_w(n->ty)) EB(0x48, 0xF7, 0xD8); else EB(0xF7, 0xD8);
        return;
    case ND_BITNOT:
        gen_expr(g, n->lhs);
        cast_rax(cc, n->lhs->ty, n->ty);
        if (ty_w(n->ty)) EB(0x48, 0xF7, 0xD0); else EB(0xF7, 0xD0);
        return;
    case ND_NOT:
        gen_expr(g, n->lhs);
        test_rax(cc, n->lhs->ty);
        EB(0x0F, 0x94, 0xC0);
        EB(0x0F, 0xB6, 0xC0);
        return;
    case ND_LOGAND: {
        Lbl *lf = lbl_new(cc), *le = lbl_new(cc);
        gen_cond_branch(g, n->lhs, lf);
        gen_cond_branch(g, n->rhs, lf);
        mov_rax_imm(cc, 1);
        jmp_lbl(cc, le);
        lbl_bind(cc, lf);
        mov_rax_imm(cc, 0);
        lbl_bind(cc, le);
        return;
    }
    case ND_LOGOR: {
        Lbl *lt = lbl_new(cc), *le = lbl_new(cc);
        gen_expr(g, n->lhs);
        test_rax(cc, n->lhs->ty);
        jcc_lbl(cc, 0x85, lt);
        gen_expr(g, n->rhs);
        test_rax(cc, n->rhs->ty);
        jcc_lbl(cc, 0x85, lt);
        mov_rax_imm(cc, 0);
        jmp_lbl(cc, le);
        lbl_bind(cc, lt);
        mov_rax_imm(cc, 1);
        lbl_bind(cc, le);
        return;
    }
    case ND_COND: {
        Lbl *lf = lbl_new(cc), *le = lbl_new(cc);
        gen_cond_branch(g, n->cond, lf);
        gen_expr(g, n->then);
        if (ucc_type_is_int(n->ty) && ucc_type_is_int(n->then->ty))
            cast_rax(cc, n->then->ty, n->ty);
        jmp_lbl(cc, le);
        lbl_bind(cc, lf);
        gen_expr(g, n->els);
        if (ucc_type_is_int(n->ty) && ucc_type_is_int(n->els->ty))
            cast_rax(cc, n->els->ty, n->ty);
        lbl_bind(cc, le);
        return;
    }
    case ND_COMMA:
        gen_expr(g, n->lhs);
        gen_expr(g, n->rhs);
        return;
    case ND_CALL:
        gen_call(g, n);
        return;
    case ND_ASSIGN: {
        Type *lt = n->lhs->ty;
        if (lt->kind == TY_STRUCT || lt->kind == TY_UNION || lt->kind == TY_ARR) {
            gen_addr(g, n->lhs);
            push_rax(g);
            gen_addr(g, n->rhs);                 /* struct rvalues are lvalues here */
            gen_struct_copy(g, lt->size);
            return;
        }
        if (n->val) {                            /* compound assign */
            int op = (int)n->val;
            gen_addr(g, n->lhs);
            push_rax(g);                         /* addr */
            gen_expr(g, n->rhs);
            if (lt->kind == TY_PTR && (op == ND_ADD || op == ND_SUB))
                scale_rhs_for_ptr(cc, n->rhs, lt);
            else if (ucc_type_is_int(lt))
                cast_rax(cc, n->rhs->ty, lt);
            EB(0x48, 0x89, 0xC1);                /* mov rcx, rax (rhs)   */
            EB(0x48, 0x8B, 0x04, 0x24);          /* mov rax, [rsp] (addr) */
            load_ty(cc, lt);                     /* rax = old value       */
            alu(cc, op, lt);                     /* rax = old OP rhs      */
            pop_rcx(g);                          /* addr                  */
            store_ty(cc, lt);
            return;                              /* rax = new value       */
        }
        gen_addr(g, n->lhs);
        push_rax(g);
        gen_expr(g, n->rhs);
        if (ucc_type_is_int(lt) && ucc_type_is_int(n->rhs->ty))
            cast_rax(cc, n->rhs->ty, lt);
        pop_rcx(g);
        store_ty(cc, lt);
        return;
    }
    case ND_POSTINC:
    case ND_POSTDEC: {
        Type *t = n->ty;
        int delta = 1, w = ty_w(t);
        if (t->kind == TY_PTR) delta = t->base->size ? t->base->size : 1;
        gen_addr(g, n->lhs);
        push_rax(g);                             /* addr                  */
        load_ty(cc, t);                          /* [rsp]=addr; rax = old */
        push_rax(g);                             /* old                   */
        if (n->kind == ND_POSTINC) { if (w) EB(0x48, 0x05); else eb(cc, 0x05); }
        else                       { if (w) EB(0x48, 0x2D); else eb(cc, 0x2D); }
        e32(cc, (u32)delta);                     /* rax = old +- delta    */
        EB(0x48, 0x8B, 0x4C, 0x24, 0x08);        /* mov rcx, [rsp+8] addr */
        store_ty(cc, t);
        eb(cc, 0x58); g->depth--;                /* pop rax = old         */
        EB(0x48, 0x83, 0xC4, 0x08); g->depth--;  /* drop addr             */
        return;
    }
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
    case ND_AND: case ND_OR: case ND_XOR: case ND_SHL: case ND_SHR:
        gen_expr(g, n->rhs);
        push_rax(g);
        gen_expr(g, n->lhs);
        pop_rcx(g);
        alu(cc, n->kind, n->ty);
        return;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        gen_expr(g, n->rhs);
        push_rax(g);
        gen_expr(g, n->lhs);
        pop_rcx(g);
        cmp_set(cc, n->kind, n->lhs->ty, n->rhs->ty->is_uns);
        return;
    default:
        ucc_err_tok(cc, n->tok, "internal: bad expression node", 0);
    }
}

/* ============================================================================
 * statements
 * ========================================================================== */
static void gen_stmt(G *g, Node *n)
{
    Cc *cc = g->cc;

    switch (n->kind) {
    case ND_NULL: return;
    case ND_BLOCK: {
        Node *s;
        for (s = n->body; s; s = s->next) gen_stmt(g, s);
        return;
    }
    case ND_EXPRSTMT:
        gen_expr(g, n->lhs);
        return;
    case ND_IF: {
        Lbl *lf = lbl_new(cc), *le = lbl_new(cc);
        gen_cond_branch(g, n->cond, lf);
        gen_stmt(g, n->then);
        jmp_lbl(cc, le);
        lbl_bind(cc, lf);
        if (n->els) gen_stmt(g, n->els);
        lbl_bind(cc, le);
        return;
    }
    case ND_WHILE: {
        Lbl *lh = lbl_new(cc), *le = lbl_new(cc);
        Lbl *sb = g->brk, *sc = g->cont;
        lbl_bind(cc, lh);
        gen_cond_branch(g, n->cond, le);
        g->brk = le; g->cont = lh;
        gen_stmt(g, n->body);
        jmp_lbl(cc, lh);
        lbl_bind(cc, le);
        g->brk = sb; g->cont = sc;
        return;
    }
    case ND_DO: {
        Lbl *lh = lbl_new(cc), *lc = lbl_new(cc), *le = lbl_new(cc);
        Lbl *sb = g->brk, *sc = g->cont;
        lbl_bind(cc, lh);
        g->brk = le; g->cont = lc;
        gen_stmt(g, n->body);
        lbl_bind(cc, lc);
        gen_expr(g, n->cond);
        test_rax(cc, n->cond->ty);
        jcc_lbl(cc, 0x85, lh);
        lbl_bind(cc, le);
        g->brk = sb; g->cont = sc;
        return;
    }
    case ND_FOR: {
        Lbl *lh = lbl_new(cc), *lc = lbl_new(cc), *le = lbl_new(cc);
        Lbl *sb = g->brk, *sc = g->cont;
        if (n->init) gen_stmt(g, n->init);
        lbl_bind(cc, lh);
        if (n->cond) gen_cond_branch(g, n->cond, le);
        g->brk = le; g->cont = lc;
        gen_stmt(g, n->body);
        lbl_bind(cc, lc);
        if (n->inc) gen_expr(g, n->inc);
        jmp_lbl(cc, lh);
        lbl_bind(cc, le);
        g->brk = sb; g->cont = sc;
        return;
    }
    case ND_SWITCH: {
        Lbl *le = lbl_new(cc);
        Lbl *sb = g->brk;
        Node *c;
        gen_expr(g, n->cond);
        for (c = n->cases; c; c = c->cases) {
            Lbl *lc = lbl_new(cc);
            c->defcase = (Node *)lc;                    /* per-case label */
            if (ty_w(n->cond->ty)) {
                EB(0x48, 0xB9); e64(cc, (u64)c->val);   /* mov rcx, imm64 */
                EB(0x48, 0x39, 0xC8);                   /* cmp rax, rcx   */
            } else {
                eb(cc, 0x3D); e32(cc, (u32)c->val);     /* cmp eax, imm32 */
            }
            jcc_lbl(cc, 0x84, lc);
        }
        if (n->defcase) {
            Lbl *ld = lbl_new(cc);
            n->defcase->defcase = (Node *)ld;
            jmp_lbl(cc, ld);
        } else
            jmp_lbl(cc, le);
        g->brk = le;
        gen_stmt(g, n->body);
        lbl_bind(cc, le);
        g->brk = sb;
        return;
    }
    case ND_CASE:
        lbl_bind(cc, (Lbl *)n->defcase);
        gen_stmt(g, n->body);
        return;
    case ND_BREAK:
        if (!g->brk) ucc_err_tok(cc, n->tok, "break outside loop/switch", 0);
        jmp_lbl(cc, g->brk);
        return;
    case ND_CONT:
        if (!g->cont) ucc_err_tok(cc, n->tok, "continue outside loop", 0);
        jmp_lbl(cc, g->cont);
        return;
    case ND_RETURN:
        if (n->lhs) {
            gen_expr(g, n->lhs);
            if (ucc_type_is_int(n->lhs->ty) && cc->cur_ret &&
                ucc_type_is_int(cc->cur_ret))
                cast_rax(cc, n->lhs->ty, cc->cur_ret);
        }
        jmp_lbl(cc, g->ret);
        return;
    default:
        gen_expr(g, n);
        return;
    }
}

/* ============================================================================
 * functions + emission
 * ========================================================================== */
void ucc_gen_func(Cc *cc, Sym *fnsym, Node *body)
{
    G g;
    int frame;
    Param *p;
    int i, npar = 0;

    while (cc->codelen & 15) eb(cc, 0xCC);        /* pad to 16 */

    fnsym->secoff = cc->codelen;
    fnsym->funcno = cc->nfuncs;
    fnsym->defined = 1;
    {
        Func *f = ucc_alloc(cc, sizeof *f);
        f->sym = fnsym; f->codeoff = cc->codelen; f->next = 0;
        if (cc->funcs_tail) cc->funcs_tail->next = f; else cc->funcs = f;
        cc->funcs_tail = f;
        cc->nfuncs++;
    }

    g.cc = cc; g.depth = 0; g.ret = lbl_new(cc); g.brk = 0; g.cont = 0;

    frame = (cc->lvsize + 15) & ~15;
    EB(0x55);                                     /* push rbp        */
    EB(0x48, 0x89, 0xE5);                         /* mov rbp, rsp    */
    if (frame) { EB(0x48, 0x81, 0xEC); e32(cc, (u32)frame); }

    for (p = fnsym->ty->params; p; p = p->next) npar++;
    for (i = 0; i < npar && i < 4; i++) {         /* home the register args */
        static const u8 mr[4][3] = {
            {0x48, 0x89, 0x8D},                   /* mov [rbp+d], rcx */
            {0x48, 0x89, 0x95},                   /* rdx */
            {0x4C, 0x89, 0x85},                   /* r8  */
            {0x4C, 0x89, 0x8D},                   /* r9  */
        };
        ebytes(cc, mr[i], 3);
        e32(cc, (u32)(16 + 8 * i));
    }

    gen_stmt(&g, body);

    lbl_bind(cc, g.ret);
    EB(0xC9, 0xC3);                               /* leave; ret */
}

/* ---- final layout + .UNO write ------------------------------------------------ */
static u32 uno_crc32(const u8 *p, long n)
{
    u32 c = 0xFFFFFFFFu; long i; int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void sum_cat(char *dst, int *pos, const char *s)
{
    while (*s && *pos < 126) dst[(*pos)++] = *s++;
    dst[*pos] = 0;
}
static void sum_num(char *dst, int *pos, long v)
{
    char rev[24]; int n = 0, i;
    if (!v) rev[n++] = '0';
    while (v > 0) { rev[n++] = (char)('0' + v % 10); v /= 10; }
    for (i = n - 1; i >= 0; i--) if (*pos < 126) dst[(*pos)++] = rev[i];
    dst[*pos] = 0;
}

long ucc_emit_uno(Cc *cc, unsigned char *out, long outmax)
{
    int data_base, imp_base, file_size, bss_base, mem_size;
    int nrel = 0;
    Fixup *f;
    DReloc *r;
    Import *im;
    long total;

    {
        Sym *e = 0;
        Func *fn;
        for (fn = cc->funcs; fn; fn = fn->next)
            if (!strcmp(fn->sym->name, "uno_app_main")) e = fn->sym;
        if (!e) ucc_fatal(cc, "missing entry: uno_app_main", 0);
        cc->entry_code_off = e->secoff;
    }

    data_base = (cc->codelen + 15) & ~15;
    imp_base  = (data_base + cc->datalen + 7) & ~7;
    file_size = (imp_base + 32 * cc->nimports + 7) & ~7;
    bss_base  = (file_size + 15) & ~15;
    mem_size  = bss_base + cc->bsslen;
    if (mem_size < file_size) mem_size = file_size;

    for (f = cc->fixups; f; f = f->next) {
        int target_rva;
        switch (f->kind) {
        case FX_CODE: {
            Sym *s = f->targetsym;
            if (!s || !s->defined)
                ucc_fatal(cc, "undefined function", s ? s->name : 0);
            target_rva = s->secoff;
            break;
        }
        case FX_DATA:   target_rva = data_base + f->target; break;
        case FX_BSS:    target_rva = bss_base + f->target; break;
        default:        target_rva = imp_base + 32 * f->target + 24; break;
        }
        *(u32 *)(cc->code + f->site) = (u32)(target_rva - (f->site + 4));
    }

    for (r = cc->drelocs; r; r = r->next) nrel++;
    total = 48L + file_size + 4L * nrel;
    if (total > outmax) ucc_fatal(cc, "module too large for output buffer", 0);

    memset(out, 0, (unsigned long)(48 + file_size));
    memcpy(out + 48, cc->code, (unsigned long)cc->codelen);
    memcpy(out + 48 + data_base, cc->data, (unsigned long)cc->datalen);
    for (im = cc->imports; im; im = im->next) {
        u8 *rec = out + 48 + imp_base + 32 * im->idx;
        unsigned long nl = strlen(im->name);
        memcpy(rec, im->name, nl > 23 ? 23 : nl);
    }

    {
        u32 *rl = (u32 *)(out + 48 + file_size);
        int ri = 0, i, j;
        for (r = cc->drelocs; r; r = r->next) {
            int rva;
            if (r->sym) {
                Sym *s = r->sym;
                if (s->cls == SYM_FUNC || s->cls == SYM_IMPORT) {
                    if (!s->defined) ucc_fatal(cc, "undefined function", s->name);
                    rva = s->secoff;
                } else if (s->sec == SEC_BSS) rva = bss_base + s->secoff;
                else                          rva = data_base + s->secoff;
            } else
                rva = data_base + r->dataoff;
            *(u64 *)(out + 48 + data_base + r->off) = (u64)(u32)(rva + r->addend);
            rl[ri++] = (u32)(data_base + r->off);
        }
        for (i = 1; i < nrel; i++) {              /* keep mkuno's sorted order */
            u32 v = rl[i];
            for (j = i - 1; j >= 0 && rl[j] > v; j--) rl[j + 1] = rl[j];
            rl[j + 1] = v;
        }
    }

    {
        u8 *h = out;
        u32 crc = uno_crc32(out + 48, file_size + 4L * nrel);
        *(u32 *)(h + 0)  = 0x314F4E55u;           /* 'UNO1' */
        *(u16 *)(h + 4)  = 1;                     /* abi    */
        *(u16 *)(h + 6)  = 0;                     /* flags  */
        *(u32 *)(h + 8)  = (u32)cc->entry_code_off;
        *(u32 *)(h + 12) = (u32)mem_size;
        *(u32 *)(h + 16) = (u32)file_size;
        *(u32 *)(h + 20) = (u32)nrel;
        *(u32 *)(h + 24) = (u32)imp_base;
        *(u32 *)(h + 28) = (u32)cc->nimports;
        *(u64 *)(h + 32) = 0;                     /* pref_base */
        *(u32 *)(h + 40) = crc;
        *(u32 *)(h + 44) = 0;
    }

    {
        int pos = 0;
        sum_cat(cc->summary, &pos, "code=");    sum_num(cc->summary, &pos, cc->codelen);
        sum_cat(cc->summary, &pos, " data=");   sum_num(cc->summary, &pos, cc->datalen);
        sum_cat(cc->summary, &pos, " bss=");    sum_num(cc->summary, &pos, cc->bsslen);
        sum_cat(cc->summary, &pos, " imports="); sum_num(cc->summary, &pos, cc->nimports);
        sum_cat(cc->summary, &pos, " relocs="); sum_num(cc->summary, &pos, nrel);
        sum_cat(cc->summary, &pos, " bytes="); sum_num(cc->summary, &pos, total);
    }
    return total;
}
