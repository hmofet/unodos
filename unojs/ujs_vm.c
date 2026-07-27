/* ===========================================================================
 * unojs virtual machine - the bytecode interpreter.
 *
 * All execution state lives in ujs_vm (frames, operand stack, handler stack,
 * instruction pointer), never in C locals across an instruction boundary.
 * That is what lets the loop return UJS_YIELD to the host when its fuel runs
 * out and pick up exactly where it stopped on the next ujs_resume() - the
 * mechanism that keeps a hostile `while(1)` from wedging a single-threaded
 * ring-0 OS (docs/WEB-ENGINE-DESIGN.md §3.1, §14).
 * ======================================================================== */
#include "ujs_int.h"
#include <math.h>
#include <stdio.h>

/* ---- small helpers ------------------------------------------------------- */
static int push(ujs_vm *vm, ujs_val v)
{
    if (vm->sp >= UJS_STACK_SIZE) {
        ujs_throw_error(vm, "RangeError", "stack overflow");
        return 0;
    }
    vm->stack[vm->sp++] = v;
    return 1;
}

static ujs_val pop(ujs_vm *vm)
{ return vm->sp > 0 ? vm->stack[--vm->sp] : ujs_undefined(); }

static int to_int32(ujs_vm *vm, ujs_val v)
{
    double d = ujs_num_of(vm, v);
    if (d != d || d == 0 || d > 1.7e308 || d < -1.7e308) return 0;
    { double m = fmod(floor(fabs(d)) * (d < 0 ? -1 : 1), 4294967296.0);
      if (m < 0) m += 4294967296.0;
      if (m >= 2147483648.0) m -= 4294967296.0;
      return (int)m; }
}

static unsigned to_uint32(ujs_vm *vm, ujs_val v)
{
    double d = ujs_num_of(vm, v);
    if (d != d || d == 0 || d > 1.7e308 || d < -1.7e308) return 0;
    { double m = fmod(floor(fabs(d)) * (d < 0 ? -1 : 1), 4294967296.0);
      if (m < 0) m += 4294967296.0;
      return (unsigned)m; }
}

/* "123" as a property name means the dense element 123 */
/* Only arrays use dense element storage. A plain object given o[3] must get a
 * real "3" property: otherwise it grows holes 0..2 that Object.keys, for-in,
 * `in` and JSON.stringify would all report as existing members. */
static int dense_target(ujs_val o)
{ return ujs_is_object(o) && ujs_val_obj(o)->cls == C_ARRAY; }

static int index_of_val(ujs_vm *vm, ujs_val k, u32 *out)
{
    if (IS_DOUBLE(k)) {
        double d = ujs_dbl(k);
        if (d >= 0 && d < 4294967295.0 && d == floor(d)) { *out = (u32)d; return 1; }
        return 0;
    }
    if (ujs_is_string(k)) {
        ujs_str *s = ujs_val_str(k);
        u32 v = 0, i;
        if (!s->len || s->len > 9) return 0;
        for (i = 0; i < s->len; i++) {
            if (s->b[i] < '0' || s->b[i] > '9') return 0;
            v = v * 10 + (u32)(s->b[i] - '0');
        }
        if (s->len > 1 && s->b[0] == '0') return 0;      /* "01" is a name    */
        *out = v; return 1;
    }
    (void)vm;
    return 0;
}

static u32 atom_of_key(ujs_vm *vm, ujs_val k)
{
    ujs_str *s = ujs_tostr(vm, k);
    return s ? ujs_atom(vm, s->b, (int)s->len) : 0;
}

/* ---- generic property get/put used by the opcodes ------------------------ */
static int prop_get(ujs_vm *vm, ujs_val obj, u32 atom, ujs_val *out, int *is_getter)
{
    ujs_obj *o;
    *is_getter = 0;
    if (ujs_is_string(obj)) {
        ujs_str *s = ujs_val_str(obj);
        if (atom == ujs_atom(vm, "length", -1)) { *out = ujs_number((double)s->len); return 1; }
        o = vm->str_proto;
    } else if (IS_DOUBLE(obj)) {
        o = vm->num_proto;
    } else if (ujs_is_object(obj)) {
        o = ujs_val_obj(obj);
        if (o->cls == C_ARRAY && atom == ujs_atom(vm, "length", -1)) {
            *out = ujs_number((double)o->nelems); return 1;
        }
    } else {
        ujs_throwf(vm, "TypeError", "cannot read '%s' of %s", ujs_atom_cstr(vm, atom),
                   ujs_is_null(obj) ? "null" : "undefined");
        return 0;
    }
    { int r = ujs_obj_get(vm, o, atom, out);
      if (r == 2) *is_getter = 1;
      return 1; }
}

static int prop_put(ujs_vm *vm, ujs_val obj, u32 atom, ujs_val v)
{
    ujs_obj *o;
    if (!ujs_is_object(obj)) {
        if (ujs_is_null(obj) || ujs_is_undefined(obj)) {
            ujs_throwf(vm, "TypeError", "cannot set '%s' of %s", ujs_atom_cstr(vm, atom),
                       ujs_is_null(obj) ? "null" : "undefined");
            return 0;
        }
        return 1;                                  /* silently dropped */
    }
    o = ujs_val_obj(obj);
    if (o->cls == C_ARRAY && atom == ujs_atom(vm, "length", -1)) {
        u32 n = (u32)ujs_num_of(vm, v);
        if (n < o->nelems) o->nelems = n;
        else { u32 i; for (i = o->nelems; i < n; i++) ujs_arr_set(vm, o, i, ujs_undefined()); }
        return 1;
    }
    {   ujs_prop *p = ujs_obj_find(o, atom);
        if (p && (p->flags & P_ACCESSOR)) {
            if (ujs_is_undefined(p->setter)) return 1;
            return ujs_call_value(vm, p->setter, obj, 1, &v, NULL, 0) == UJS_OK;
        }
    }
    if (!ujs_obj_put(vm, o, atom, v, P_DEFAULT)) {
        ujs_throw_error(vm, "RangeError", "out of memory");
        return 0;
    }
    return 1;
}

/* ---- iterator state ------------------------------------------------------
 * One array object: elems[0] is the cursor, elems[1..] the items to visit. */
static ujs_val make_iter(ujs_vm *vm, ujs_val target, int values)
{
    ujs_obj *it = ujs_obj_new(vm, C_ARRAY, NULL);
    if (!it) return ujs_undefined();
    ujs_arr_set(vm, it, 0, ujs_number(0));
    if (ujs_is_string(target) && values) {
        ujs_str *s = ujs_val_str(target);
        u32 i;
        for (i = 0; i < s->len; i++)
            ujs_arr_set(vm, it, it->nelems, ujs_string(vm, s->b + i, 1));
        return ujs_obj_val(it);
    }
    if (!ujs_is_object(target)) return ujs_obj_val(it);
    {   ujs_obj *o = ujs_val_obj(target);
        u32 i;
        for (i = 0; i < o->nelems; i++) {
            if (values) ujs_arr_set(vm, it, it->nelems, o->elems[i]);
            else { char b[24]; snprintf(b, sizeof b, "%u", i);
                   ujs_arr_set(vm, it, it->nelems, ujs_string(vm, b, -1)); }
        }
        if (!values) {
            for (i = 0; i < o->nprops; i++) {
                ujs_str *nm;
                if (!o->props[i].atom || !(o->props[i].flags & P_ENUM)) continue;
                nm = ujs_atom_str(vm, o->props[i].atom);
                if (nm) ujs_arr_set(vm, it, it->nelems, ujs_str_val(nm));
            }
        } else if (o->cls != C_ARRAY) {
            ujs_throw_error(vm, "TypeError", "value is not iterable");
            return ujs_undefined();
        }
        return ujs_obj_val(it);
    }
}

/* Binary numeric operators. The operands are PEEKED, not popped: ToNumber on
 * an object calls user code and allocates, and anything not reachable from the
 * operand stack at that moment is garbage. Popping first is how a collector
 * frees a value that is still about to be used - the bug class ASan caught in
 * OP_ADD, fixed here for every arithmetic and comparison opcode. */
#define BINNUM(EXPR) do {         ujs_val b = vm->stack[vm->sp-1], a = vm->stack[vm->sp-2];         double x, y;         fr->ip = ip;         x = ujs_num_of(vm, a); y = ujs_num_of(vm, b);         if (vm->has_exception) continue;         vm->sp -= 2;         if (!push(vm, ujs_number(EXPR))) goto oom;     } while (0)

/* ---- calling ------------------------------------------------------------- */
static ujs_result run(ujs_vm *vm, int base, ujs_val *out);

static int push_frame(ujs_vm *vm, ujs_obj *fn, ujs_val self,
                      int argc, const ujs_val *argv, int is_ctor)
{
    ujs_code *code = fn->u.fn.code;
    ujs_env *env;
    ujs_frame *fr;
    int i;
    if (vm->nframes >= UJS_MAX_FRAMES) {
        ujs_throw_error(vm, "RangeError", "maximum call depth exceeded");
        return 0;
    }
    env = ujs_env_new(vm, fn->u.fn.env, code->nslots);
    if (!env) { ujs_throw_error(vm, "RangeError", "out of memory"); return 0; }
    for (i = 0; i < (int)code->nparams && i < argc; i++) env->slots[i] = argv[i];
    fr = &vm->frames[vm->nframes++];
    fr->code = code;
    fr->env = env;
    fr->fn = fn;
    /* an arrow function has no `this` of its own: it keeps the one from the
     * frame that created it, which the closure captured at definition time. */
    fr->self = self;
    fr->ip = 0;
    fr->sp_base = vm->sp;
    fr->h_base = vm->nhandlers;
    fr->is_ctor = is_ctor;
    return 1;
}

ujs_result ujs_call_value(ujs_vm *vm, ujs_val fnv, ujs_val self,
                          int argc, const ujs_val *argv, ujs_val *out, int is_new)
{
    ujs_obj *fn;
    if (out) *out = ujs_undefined();
    if (!ujs_is_object(fnv)) {
        ujs_throw_error(vm, "TypeError", "value is not a function");
        return UJS_THROW;
    }
    fn = ujs_val_obj(fnv);
    if (fn->cls == C_CFUNC) {
        ujs_args a;
        ujs_val r;
        a.vm = vm; a.self = self; a.argc = argc; a.argv = argv;
        vm->c_depth++;
        r = fn->u.cfn.fn(&a);
        vm->c_depth--;
        if (vm->has_exception) return UJS_THROW;
        if (out) *out = r;
        return UJS_OK;
    }
    if (fn->cls != C_FUNC) {
        ujs_throw_error(vm, "TypeError", "value is not a function");
        return UJS_THROW;
    }
    {   int base = vm->nframes;
        if (!push_frame(vm, fn, self, argc, argv, is_new)) return UJS_THROW;
        vm->c_depth++;                       /* a nested run cannot yield out */
        {   ujs_result r = run(vm, base, out);
            vm->c_depth--;
            return r;
        }
    }
}

/* ---- the interpreter ----------------------------------------------------- */
#define RD8()  (code->bc[ip++])
#define RD16() (ip += 2, (u32)code->bc[ip-2] | ((u32)code->bc[ip-1] << 8))
#define RD32() (ip += 4, (u32)code->bc[ip-4] | ((u32)code->bc[ip-3] << 8) | \
                         ((u32)code->bc[ip-2] << 16) | ((u32)code->bc[ip-1] << 24))

/* Unwind to the nearest handler at or above `base`. Returns 1 if execution
 * should continue (a catch or finally took it), 0 if it escapes this run. */
static int unwind(ujs_vm *vm, int base)
{
    while (vm->nhandlers > 0) {
        ujs_handler *h = &vm->handlers[vm->nhandlers - 1];
        if (h->frame < base) break;
        vm->nhandlers--;
        vm->nframes = h->frame + 1;
        vm->sp = h->sp;
        if (h->catch_ip) {
            ujs_val e = vm->exception;
            vm->has_exception = 0;
            vm->exception = ujs_undefined();
            if (!push(vm, e)) return 0;
            vm->frames[vm->nframes - 1].ip = h->catch_ip;
            return 1;
        }
        if (h->finally_ip) {
            vm->finally_exc = vm->exception;
            vm->finally_rethrow = 1;
            vm->has_exception = 0;
            vm->exception = ujs_undefined();
            vm->frames[vm->nframes - 1].ip = h->finally_ip;
            return 1;
        }
    }
    return 0;
}

static ujs_result run(ujs_vm *vm, int base, ujs_val *out)
{
    for (;;) {
        ujs_frame *fr = &vm->frames[vm->nframes - 1];
        ujs_code *code = fr->code;
        u32 ip = fr->ip;
        u8 op;

        if (vm->has_exception) {
            fr->ip = ip;
            if (!unwind(vm, base)) return UJS_THROW;
            continue;
        }

        op = code->bc[ip++];

        /* fuel: charged on the operations that can form an unbounded loop */
        if (vm->fuel_slice && (op == OP_JMP || op == OP_JT || op == OP_JF ||
                               op == OP_CALL || op == OP_NEW || op == OP_ITER_NEXT)) {
            vm->fuel_used++;
            if (vm->fuel_total && vm->fuel_used > vm->fuel_total) {
                fr->ip = ip - 1;
                ujs_throw_error(vm, "Error", "script ran too long");
                continue;
            }
            if (vm->fuel == 0) {
                fr->ip = ip - 1;                  /* re-execute this op later */
                if (vm->c_depth == 0) { vm->running = 1; return UJS_YIELD; }
                vm->fuel = vm->fuel_slice;        /* nested in C: cannot yield */
            }
            vm->fuel--;
        }

        switch (op) {
        case OP_NOP: break;
        case OP_CONST: { u32 k = RD16(); if (!push(vm, code->consts[k])) goto oom;
        break; }
        case OP_UNDEF: if (!push(vm, ujs_undefined())) goto oom;
        break;
        case OP_NULL:  if (!push(vm, ujs_null())) goto oom;
        break;
        case OP_TRUE:  if (!push(vm, ujs_bool(1))) goto oom;
        break;
        case OP_FALSE: if (!push(vm, ujs_bool(0))) goto oom;
        break;
        case OP_ZERO:  if (!push(vm, ujs_number(0))) goto oom;
        break;
        case OP_ONE:   if (!push(vm, ujs_number(1))) goto oom;
        break;
        case OP_DUP:   if (!push(vm, vm->stack[vm->sp-1])) goto oom;
        break;
        case OP_POP:   pop(vm); break;
        case OP_SWAP:  { ujs_val a = vm->stack[vm->sp-1];
                         vm->stack[vm->sp-1] = vm->stack[vm->sp-2];
                         vm->stack[vm->sp-2] = a; break; }
        case OP_ROT3:  { ujs_val c3 = vm->stack[vm->sp-1], b = vm->stack[vm->sp-2],
                                 a = vm->stack[vm->sp-3];
                         vm->stack[vm->sp-3] = c3; vm->stack[vm->sp-2] = a;
                         vm->stack[vm->sp-1] = b; break; }
        case OP_OVER_DUP: if (!push(vm, vm->stack[vm->sp-2])) goto oom;
        break;

        case OP_GETLOC: { int d = RD8(); u32 s = RD16(); ujs_env *e = fr->env;
                          while (d-- > 0 && e) e = e->parent;
                          if (!push(vm, (e && s < e->n) ? e->slots[s] : ujs_undefined())) goto oom;
                          break; }
        case OP_SETLOC: { int d = RD8(); u32 s = RD16(); ujs_env *e = fr->env;
                          while (d-- > 0 && e) e = e->parent;
                          if (e && s < e->n) e->slots[s] = vm->stack[vm->sp-1];
                          break; }
        case OP_GETGLOB: { u32 a = RD32(); ujs_val v;
                           if (!ujs_obj_get(vm, vm->global, a, &v)) {
                               fr->ip = ip;
                               ujs_throwf(vm, "ReferenceError", "%s is not defined",
                                          ujs_atom_cstr(vm, a));
                               continue;
                           }
                           if (!push(vm, v)) goto oom;
                           break; }
        case OP_SETGLOB: { u32 a = RD32();
                           if (!ujs_obj_put(vm, vm->global, a, vm->stack[vm->sp-1], P_DEFAULT))
                               goto oom;
                           break; }
        case OP_DECLGLOB: { u32 a = RD32();
                            if (!ujs_obj_find(vm->global, a))
                                ujs_obj_put(vm, vm->global, a, ujs_undefined(), P_DEFAULT);
                            break; }

        case OP_GETPROP: {
            u32 a = RD32(); ujs_val o = vm->stack[vm->sp-1], v; int g;
            fr->ip = ip;
            if (!prop_get(vm, o, a, &v, &g)) continue;
            if (g) { ujs_val r;
                     if (ujs_call_value(vm, v, o, 0, NULL, &r, 0) != UJS_OK) continue;
                     v = r; }
            vm->stack[vm->sp-1] = v;
            break; }
        case OP_GETPROP_KEEP: {
            /* compound assignment (`o.k += v`): leave the RECEIVER on the
             * stack under the loaded value so SETPROP can store back. */
            u32 a = RD32(); ujs_val o = vm->stack[vm->sp-1], v; int g;
            fr->ip = ip;
            if (!prop_get(vm, o, a, &v, &g)) continue;
            if (g) { ujs_val r;
                     if (ujs_call_value(vm, v, o, 0, NULL, &r, 0) != UJS_OK) continue;
                     v = r; }
            if (!push(vm, v)) goto oom;
            break; }
        case OP_GETIDX_KEEP: {
            ujs_val o = vm->stack[vm->sp-2], k = vm->stack[vm->sp-1], v = ujs_undefined();
            u32 idx;
            int done = 0;
            fr->ip = ip;
            if (dense_target(o) && index_of_val(vm, k, &idx)) {
                ujs_obj *oo = ujs_val_obj(o);
                v = idx < oo->nelems ? oo->elems[idx] : ujs_undefined();
                done = 1;
            }
            if (!done) {
                int g; u32 a = atom_of_key(vm, k);
                if (vm->has_exception) continue;
                if (!prop_get(vm, o, a, &v, &g)) continue;
                if (g) { ujs_val r;
                         if (ujs_call_value(vm, v, o, 0, NULL, &r, 0) != UJS_OK) continue;
                         v = r; }
            }
            if (!push(vm, v)) goto oom;
            break; }
        case OP_GETGLOB_SOFT: {
            /* `typeof x` where x was never declared: undefined, not a throw */
            u32 a = RD32(); ujs_val v;
            ujs_obj_get(vm, vm->global, a, &v);
            if (!push(vm, v)) goto oom;
            break; }
        case OP_SETRES: vm->completion = pop(vm); break;
        case OP_GETRES:
            if (!push(vm, vm->completion)) goto oom;
            break;

        case OP_SETPROP: {
            u32 a = RD32();
            ujs_val v = vm->stack[vm->sp-1], o = vm->stack[vm->sp-2];
            fr->ip = ip;
            if (!prop_put(vm, o, a, v)) continue;
            vm->stack[vm->sp-2] = v; vm->sp--;
            break; }
        case OP_GETIDX: {
            ujs_val o = vm->stack[vm->sp-2], k = vm->stack[vm->sp-1], v = ujs_undefined();
            u32 idx;
            int done = 0;
            fr->ip = ip;
            if (dense_target(o) && index_of_val(vm, k, &idx)) {
                ujs_obj *oo = ujs_val_obj(o);
                v = idx < oo->nelems ? oo->elems[idx] : ujs_undefined();
                done = 1;
            } else if (ujs_is_string(o) && index_of_val(vm, k, &idx)) {
                ujs_str *st = ujs_val_str(o);
                v = idx < st->len ? ujs_string(vm, st->b + idx, 1) : ujs_undefined();
                done = 1;
            }
            if (!done) {
                int g; u32 a = atom_of_key(vm, k);
                if (vm->has_exception) continue;
                if (!prop_get(vm, o, a, &v, &g)) continue;
                if (g) { ujs_val r;
                         if (ujs_call_value(vm, v, o, 0, NULL, &r, 0) != UJS_OK) continue;
                         v = r; }
            }
            vm->sp -= 2;
            if (!push(vm, v)) goto oom;
            break; }
        case OP_SETIDX: {
            ujs_val v = vm->stack[vm->sp-1], k = vm->stack[vm->sp-2], o = vm->stack[vm->sp-3];
            u32 idx;
            fr->ip = ip;
            if (dense_target(o) && index_of_val(vm, k, &idx)) {
                ujs_arr_set(vm, ujs_val_obj(o), idx, v);
            } else {
                u32 a = atom_of_key(vm, k);
                if (vm->has_exception) continue;
                if (!prop_put(vm, o, a, v)) continue;
            }
            vm->stack[vm->sp-3] = v; vm->sp -= 2;
            break; }
        case OP_DELPROP: { u32 a = RD32(); ujs_val o = pop(vm);
                           if (ujs_is_object(o)) ujs_obj_del(vm, ujs_val_obj(o), a);
                           if (!push(vm, ujs_bool(1))) goto oom;
                           break; }
        case OP_DELIDX: {
            ujs_val o = vm->stack[vm->sp-2], k = vm->stack[vm->sp-1];
            u32 idx;
            fr->ip = ip;
            if (ujs_is_object(o)) {
                ujs_obj *oo = ujs_val_obj(o);
                if (oo->cls == C_ARRAY && index_of_val(vm, k, &idx)) {
                    if (idx < oo->nelems) oo->elems[idx] = ujs_undefined();
                } else {
                    u32 a = atom_of_key(vm, k);
                    if (vm->has_exception) continue;
                    ujs_obj_del(vm, oo, a);
                }
            }
            vm->sp -= 2;
            if (!push(vm, ujs_bool(1))) goto oom;
            break; }

        case OP_ADD: {
            ujs_val b = vm->stack[vm->sp-1], a = vm->stack[vm->sp-2];
            fr->ip = ip;
            if (ujs_is_string(a) || ujs_is_string(b) ||
                ujs_is_object(a) || ujs_is_object(b)) {
                ujs_str *sa, *sb, *r;
                sa = ujs_tostr(vm, a);
                if (!sa) continue;
                vm->stack[vm->sp-2] = ujs_str_val(sa);   /* keep it rooted */
                sb = ujs_tostr(vm, b);
                if (!sb) continue;
                vm->stack[vm->sp-1] = ujs_str_val(sb);
                r = ujs_str_cat(vm, sa, sb);
                if (!r) goto oom;
                vm->sp -= 2;
                if (!push(vm, ujs_str_val(r))) goto oom;
                break;
            }
            BINNUM(x + y);
            break; }
        case OP_SUB: BINNUM(x - y); break;
        case OP_MUL: BINNUM(x * y); break;
        case OP_DIV: BINNUM(x / y); break;
        case OP_MOD: BINNUM(fmod(x, y)); break;
        case OP_POW: BINNUM(pow(x, y)); break;
        case OP_NEG: { ujs_val a = pop(vm);
                       if (!push(vm, ujs_number(-ujs_num_of(vm,a)))) goto oom;
                       break; }
        case OP_POS: { ujs_val a = pop(vm);
                       if (!push(vm, ujs_number(ujs_num_of(vm,a)))) goto oom;
                       break; }
        case OP_NOT: { ujs_val a = pop(vm);
                       if (!push(vm, ujs_bool(!ujs_truthy(a)))) goto oom;
                       break; }
        case OP_BITNOT: { ujs_val a = pop(vm);
                          if (!push(vm, ujs_number((double)(~to_int32(vm,a))))) goto oom;
                          break; }
        case OP_AND: { int xi, yi; fr->ip = ip;
                     xi = to_int32(vm, vm->stack[vm->sp-2]);
                     yi = to_int32(vm, vm->stack[vm->sp-1]);
                     if (vm->has_exception) continue;
                     vm->sp -= 2;
                     if (!push(vm, ujs_number((double)(xi & yi)))) goto oom;
                     break; }
        case OP_OR: { int xi, yi; fr->ip = ip;
                     xi = to_int32(vm, vm->stack[vm->sp-2]);
                     yi = to_int32(vm, vm->stack[vm->sp-1]);
                     if (vm->has_exception) continue;
                     vm->sp -= 2;
                     if (!push(vm, ujs_number((double)(xi | yi)))) goto oom;
                     break; }
        case OP_XOR: { int xi, yi; fr->ip = ip;
                     xi = to_int32(vm, vm->stack[vm->sp-2]);
                     yi = to_int32(vm, vm->stack[vm->sp-1]);
                     if (vm->has_exception) continue;
                     vm->sp -= 2;
                     if (!push(vm, ujs_number((double)(xi ^ yi)))) goto oom;
                     break; }
        case OP_SHL: { int xi; unsigned yu; fr->ip = ip;
                     xi = to_int32(vm, vm->stack[vm->sp-2]);
                     yu = to_uint32(vm, vm->stack[vm->sp-1]);
                     if (vm->has_exception) continue;
                     vm->sp -= 2;
                     if (!push(vm, ujs_number((double)(xi << (yu & 31))))) goto oom;
                     break; }
        case OP_SHR: { int xi; unsigned yu; fr->ip = ip;
                     xi = to_int32(vm, vm->stack[vm->sp-2]);
                     yu = to_uint32(vm, vm->stack[vm->sp-1]);
                     if (vm->has_exception) continue;
                     vm->sp -= 2;
                     if (!push(vm, ujs_number((double)(xi >> (yu & 31))))) goto oom;
                     break; }
        case OP_USHR: { unsigned xu, yu; fr->ip = ip;
                        xu = to_uint32(vm, vm->stack[vm->sp-2]);
                        yu = to_uint32(vm, vm->stack[vm->sp-1]);
                        if (vm->has_exception) continue;
                        vm->sp -= 2;
                        if (!push(vm, ujs_number((double)(xu >> (yu & 31))))) goto oom;
                        break; }

        case OP_EQ: { int r; fr->ip = ip;
                   r = ujs_loose_eq(vm, vm->stack[vm->sp-2], vm->stack[vm->sp-1]);
                   if (vm->has_exception) continue;
                   vm->sp -= 2;
                   if (!push(vm, ujs_bool(r))) goto oom;
                   break; }
        case OP_NE: { int r; fr->ip = ip;
                   r = ujs_loose_eq(vm, vm->stack[vm->sp-2], vm->stack[vm->sp-1]);
                   if (vm->has_exception) continue;
                   vm->sp -= 2;
                   if (!push(vm, ujs_bool(!r))) goto oom;
                   break; }
        case OP_SEQ: { ujs_val b = pop(vm), a = pop(vm);
                       if (!push(vm, ujs_bool(ujs_strict_eq(a,b)))) goto oom;
                       break; }
        case OP_SNE: { ujs_val b = pop(vm), a = pop(vm);
                       if (!push(vm, ujs_bool(!ujs_strict_eq(a,b)))) goto oom;
                       break; }
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            ujs_val b = vm->stack[vm->sp-1], a = vm->stack[vm->sp-2];
            int r;
            fr->ip = ip;
            if (ujs_is_string(a) && ujs_is_string(b)) {          /* lexicographic */
                ujs_str *sa = ujs_val_str(a), *sb = ujs_val_str(b);
                u32 n = sa->len < sb->len ? sa->len : sb->len;
                int cmp = memcmp(sa->b, sb->b, n);
                if (!cmp) cmp = sa->len < sb->len ? -1 : sa->len > sb->len ? 1 : 0;
                r = op == OP_LT ? cmp <  0 : op == OP_LE ? cmp <= 0
                  : op == OP_GT ? cmp >  0 : cmp >= 0;
            } else {
                double da = ujs_num_of(vm, a), db = ujs_num_of(vm, b);
                if (vm->has_exception) continue;
                if (da != da || db != db) r = 0;                 /* NaN: all false */
                else r = op == OP_LT ? da <  db : op == OP_LE ? da <= db
                       : op == OP_GT ? da >  db : da >= db;
            }
            vm->sp -= 2;
            if (!push(vm, ujs_bool(r))) goto oom;
            break; }
        case OP_IN: {
            ujs_val o = vm->stack[vm->sp-1], k = vm->stack[vm->sp-2];
            u32 idx; int has = 0;
            fr->ip = ip;
            if (ujs_is_object(o)) {
                ujs_obj *oo = ujs_val_obj(o);
                if (oo->cls == C_ARRAY && index_of_val(vm, k, &idx)) has = idx < oo->nelems;
                else { ujs_val t; u32 a = atom_of_key(vm, k);
                       if (vm->has_exception) continue;
                       has = ujs_obj_get(vm, oo, a, &t) != 0; }
            }
            vm->sp -= 2;
            if (!push(vm, ujs_bool(has))) goto oom;
            break; }
        case OP_INSTANCEOF: {
            ujs_val ctor = pop(vm), o = pop(vm);
            int r = 0;
            fr->ip = ip;
            if (!ujs_is_object(ctor)) {
                ujs_throw_error(vm, "TypeError", "right side of instanceof is not callable");
                continue;
            }
            if (ujs_is_object(o)) {
                ujs_val proto;
                ujs_obj_get(vm, ujs_val_obj(ctor), ujs_atom(vm, "prototype", -1), &proto);
                { ujs_val cur = ujs_val_obj(o)->proto; int guard = 0;
                  while (ujs_is_object(cur) && guard++ < 1000) {
                      if (VBITS(cur) == VBITS(proto)) { r = 1; break; }
                      cur = ujs_val_obj(cur)->proto;
                  } }
            }
            if (!push(vm, ujs_bool(r))) goto oom;
            break; }
        case OP_TYPEOF: { ujs_val a = pop(vm);
                          if (!push(vm, ujs_string(vm, ujs_typeof_str(vm, a), -1))) goto oom;
                          break; }

        case OP_JMP: { u32 t = RD32(); ip = t; break; }
        case OP_JT:  { u32 t = RD32(); if (ujs_truthy(pop(vm))) ip = t; break; }
        case OP_JF:  { u32 t = RD32(); if (!ujs_truthy(pop(vm))) ip = t; break; }
        case OP_JT_KEEP: { u32 t = RD32(); if (ujs_truthy(vm->stack[vm->sp-1])) ip = t; break; }
        case OP_JF_KEEP: { u32 t = RD32(); if (!ujs_truthy(vm->stack[vm->sp-1])) ip = t; break; }

        case OP_CALL: {
            int argc = RD8();
            int fi = vm->sp - argc - 2;
            ujs_val fnv, selfv;
            fr->ip = ip;
            if (fi < 0) { ujs_throw_error(vm, "Error", "corrupt call frame"); continue; }
            fnv = vm->stack[fi]; selfv = vm->stack[fi+1];
            if (!ujs_is_object(fnv)) {
                ujs_throw_error(vm, "TypeError", "value is not a function");
                continue;
            }
            {   ujs_obj *fn = ujs_val_obj(fnv);
                if (fn->cls == C_CFUNC) {
                    ujs_args a; ujs_val r;
                    a.vm = vm; a.self = selfv; a.argc = argc;
                    a.argv = argc ? &vm->stack[fi+2] : NULL;
                    vm->c_depth++;
                    r = fn->u.cfn.fn(&a);
                    vm->c_depth--;
                    vm->sp = fi;
                    if (vm->has_exception) continue;
                    if (!push(vm, r)) goto oom;
                    break;
                }
                if (fn->cls != C_FUNC) {
                    ujs_throw_error(vm, "TypeError", "value is not a function");
                    continue;
                }
                /* a real JS call: push a frame and keep interpreting. The
                 * arguments stay on the stack until push_frame copies them. */
                {   ujs_val self2 = fn->u.fn.code->is_arrow
                                  ? vm->frames[vm->nframes-1].self : selfv;
                    if (!push_frame(vm, fn, self2, argc,
                                    argc ? &vm->stack[fi+2] : NULL, 0)) continue;
                    vm->frames[vm->nframes-1].sp_base = fi;   /* result lands here */
                    vm->sp = fi;
                }
            }
            break; }

        case OP_NEW: {
            int argc = RD8();
            int fi = vm->sp - argc - 1;
            ujs_val fnv, proto, objv;
            ujs_obj *newo;
            fr->ip = ip;
            if (fi < 0) { ujs_throw_error(vm, "Error", "corrupt call frame"); continue; }
            fnv = vm->stack[fi];
            if (!ujs_is_object(fnv)) { ujs_throw_error(vm, "TypeError", "not a constructor"); continue; }
            ujs_obj_get(vm, ujs_val_obj(fnv), ujs_atom(vm, "prototype", -1), &proto);
            newo = ujs_obj_new(vm, C_PLAIN, ujs_is_object(proto) ? ujs_val_obj(proto) : vm->obj_proto);
            if (!newo) goto oom;
            objv = ujs_obj_val(newo);
            /* park the new object above the arguments so it stays reachable
             * for the whole constructor call (which allocates freely) */
            if (!push(vm, objv)) goto oom;
            {   ujs_val r;
                ujs_result rc = ujs_call_value(vm, fnv, objv, argc,
                                               argc ? &vm->stack[fi+1] : NULL, &r, 1);
                vm->sp = fi;
                if (rc != UJS_OK) continue;
                if (!push(vm, ujs_is_object(r) ? r : objv)) goto oom;
            }
            break; }
        case OP_RET:
            /* If this frame is still inside a try that has a `finally`, the
             * return value waits while the finally body runs; OP_FINALLY_END
             * then completes the return. */
            if (vm->nhandlers > 0) {
                ujs_handler *h = &vm->handlers[vm->nhandlers-1];
                if (h->frame == vm->nframes-1 && h->finally_ip) {
                    vm->pending_ret = pop(vm);
                    vm->ret_pending = 1;
                    vm->nhandlers--;
                    vm->sp = h->sp;
                    ip = h->finally_ip;
                    break;
                }
            }
        do_return: {
            ujs_val rv = pop(vm);
            int sp_base = fr->sp_base;
            vm->nframes--;
            while (vm->nhandlers > 0 && vm->handlers[vm->nhandlers-1].frame >= vm->nframes)
                vm->nhandlers--;
            vm->sp = sp_base;
            if (vm->nframes <= base) { if (out) *out = rv; return UJS_OK; }
            if (!push(vm, rv)) goto oom;
            break; }

        case OP_CLOSURE: {
            u32 k = RD16();
            ujs_code *cc = (ujs_code *)PTR_OF(code->consts[k]);
            ujs_obj *f = ujs_obj_new(vm, C_FUNC, vm->fun_proto);
            if (!f) goto oom;
            f->u.fn.code = cc;
            f->u.fn.env = fr->env;
            if (!push(vm, ujs_obj_val(f))) goto oom;   /* rooted while filled */
            ujs_obj_put(vm, f, ujs_atom(vm, "length", -1), ujs_number(cc->nparams), P_CONFIG);
            if (cc->name_atom) {
                ujs_str *nm = ujs_atom_str(vm, cc->name_atom);
                ujs_obj_put(vm, f, ujs_atom(vm, "name", -1),
                            nm ? ujs_str_val(nm) : ujs_string(vm, "", 0), P_CONFIG);
            }
            /* every non-arrow function is a potential constructor, so it needs
             * a .prototype whose .constructor points back */
            if (!cc->is_arrow) {
                ujs_obj *pr = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
                if (!pr) goto oom;
                if (!push(vm, ujs_obj_val(pr))) goto oom;
                ujs_obj_put(vm, pr, ujs_atom(vm, "constructor", -1), ujs_obj_val(f), P_WRITE|P_CONFIG);
                ujs_obj_put(vm, f, ujs_atom(vm, "prototype", -1), ujs_obj_val(pr), P_WRITE);
                vm->sp--;                              /* drop the prototype   */
            }
            break; }
        case OP_ARRAY: {
            u32 n = RD16(), i;
            ujs_obj *a = ujs_obj_new(vm, C_ARRAY, vm->arr_proto);
            if (!a) goto oom;
            if (!push(vm, ujs_obj_val(a))) goto oom;   /* rooted while filled */
            for (i = 0; i < n; i++) ujs_arr_set(vm, a, i, vm->stack[vm->sp - 1 - n + i]);
            vm->stack[vm->sp - 1 - n] = ujs_obj_val(a);
            vm->sp -= (int)n;
            break; }
        case OP_OBJECT: {
            u32 n = RD16(), i;
            ujs_obj *o = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
            if (!o) goto oom;
            fr->ip = ip;
            if (!push(vm, ujs_obj_val(o))) goto oom;   /* rooted while filled */
            for (i = 0; i < n; i++) {
                ujs_val k = vm->stack[vm->sp - 1 - 2*n + 2*i];
                ujs_val v = vm->stack[vm->sp - 1 - 2*n + 2*i + 1];
                ujs_obj_put(vm, o, atom_of_key(vm, k), v, P_DEFAULT);
                if (vm->has_exception) break;
            }
            if (vm->has_exception) continue;
            vm->stack[vm->sp - 1 - 2*n] = ujs_obj_val(o);
            vm->sp -= (int)(2*n);
            break; }
        case OP_THIS: if (!push(vm, fr->self)) goto oom;
        break;

        case OP_THROW: { ujs_val e = pop(vm); fr->ip = ip; ujs_throw(vm, e); continue; }

        case OP_TRY: {
            u32 cip = RD32(), fip = RD32();
            if (vm->nhandlers == vm->handlercap) {
                int nc = vm->handlercap ? vm->handlercap * 2 : 16;
                ujs_handler *nh = (ujs_handler *)ujs_alloc_raw(vm, (size_t)nc * sizeof *nh);
                if (!nh) goto oom;
                if (vm->handlers) { memcpy(nh, vm->handlers, (size_t)vm->nhandlers * sizeof *nh);
                                    ujs_free_raw(vm, vm->handlers, (size_t)vm->handlercap * sizeof *nh); }
                vm->handlers = nh; vm->handlercap = nc;
            }
            {   ujs_handler *h = &vm->handlers[vm->nhandlers++];
                h->frame = vm->nframes - 1;
                h->catch_ip = cip; h->finally_ip = fip;
                h->sp = vm->sp; h->env_depth = 0; }
            break; }
        case OP_ENDTRY: if (vm->nhandlers > 0) vm->nhandlers--; break;
        case OP_FINALLY_END:
            if (vm->finally_rethrow) {
                vm->finally_rethrow = 0;
                fr->ip = ip;
                ujs_throw(vm, vm->finally_exc);
                continue;
            }
            if (vm->ret_pending) {
                vm->ret_pending = 0;
                if (!push(vm, vm->pending_ret)) goto oom;
                goto do_return;
            }
            break;

        case OP_ITER: case OP_ITER_OF: {
            ujs_val it;
            fr->ip = ip;
            it = make_iter(vm, vm->stack[vm->sp-1], op == OP_ITER_OF);
            if (vm->has_exception) continue;
            vm->stack[vm->sp-1] = it;
            break; }
        case OP_ITER_NEXT: {
            u32 done = RD32();
            ujs_val itv = vm->stack[vm->sp-1];
            ujs_obj *it = ujs_val_obj(itv);
            u32 cur = (u32)ujs_num_of(vm, ujs_arr_get(it, 0));
            if (cur + 1 >= it->nelems) { ip = done; break; }
            ujs_arr_set(vm, it, 0, ujs_number((double)(cur + 1)));
            if (!push(vm, ujs_arr_get(it, cur + 1))) goto oom;
            break; }

        case OP_ENV_PUSH: { u32 n = RD16(); (void)n; break; }   /* block scope: M1b */
        case OP_ENV_POP:  break;
        case OP_LINE:     { u32 l = RD32(); (void)l; break; }

        default:
            fr->ip = ip;
            ujs_throwf(vm, "Error", "bad opcode %d", (int)op);
            continue;
        }
        /* Store the instruction pointer back into the frame we were RUNNING,
         * not into whatever is on top now: OP_CALL pushes a callee frame, and
         * writing `ip` there would corrupt the callee's entry point. */
        fr->ip = ip;
        continue;
    oom:
        ujs_throw_error(vm, "RangeError", "out of memory");
        fr->ip = ip;
    }
}

/* ---- public entry points ------------------------------------------------- */
ujs_result ujs_eval(ujs_vm *vm, const char *src, int len, ujs_val *out)
{
    ujs_code *code;
    char err[192];
    ujs_obj *fn;
    if (out) *out = ujs_undefined();
    if (vm->has_exception) return UJS_THROW;
    err[0] = 0;
    code = ujs_compile(vm, src, len, err, sizeof err);
    if (!code) {
        ujs_throw_error(vm, "SyntaxError", err[0] ? err : "compile failed");
        return UJS_SYNTAX;
    }
    /* wrap the script body in a function object so the normal frame machinery
     * runs it; its environment is the (empty) top-level one. */
    fn = ujs_obj_new(vm, C_FUNC, vm->fun_proto);
    if (!fn) { ujs_throw_error(vm, "RangeError", "out of memory"); return UJS_OOM; }
    fn->u.fn.code = code;
    fn->u.fn.env = ujs_env_new(vm, NULL, 0);
    if (!fn->u.fn.env) { ujs_throw_error(vm, "RangeError", "out of memory"); return UJS_OOM; }

    vm->sp = 0; vm->nframes = 0; vm->nhandlers = 0;
    vm->fuel = vm->fuel_slice;
    vm->fuel_used = 0;
    vm->completion = ujs_undefined();
    vm->ret_pending = 0; vm->finally_rethrow = 0;
    if (!push_frame(vm, fn, ujs_obj_val(vm->global), 0, NULL, 0)) return UJS_THROW;
    return run(vm, 0, out);
}

ujs_result ujs_resume(ujs_vm *vm, ujs_val *out)
{
    if (out) *out = ujs_undefined();
    if (!vm->running) return UJS_OK;
    vm->fuel = vm->fuel_slice;
    {   ujs_result r = run(vm, 0, out);
        if (r != UJS_YIELD) vm->running = 0;
        return r;
    }
}
