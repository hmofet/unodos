/* ===========================================================================
 * unojs standard library - the built-in objects every script assumes exist.
 *
 * Note what is NOT here: console, timers, document, fetch. Those are HOST
 * concerns, and unojs deliberately knows nothing about them - an embedder
 * projects them in with ujs_set_fn / ujs_host_new. Keeping the split honest
 * is what lets the engine build and pass its tests with no consumer linked.
 * ======================================================================== */
#include "ujs_int.h"
#include <stdio.h>

/* ---- argument helpers ---------------------------------------------------- */
static ujs_val arg(ujs_args *a, int i)
{ return i < a->argc ? a->argv[i] : ujs_undefined(); }

static double argnum(ujs_args *a, int i) { return ujs_num_of(a->vm, arg(a, i)); }

static ujs_str *argstr(ujs_args *a, int i) { return ujs_tostr(a->vm, arg(a, i)); }

static ujs_str *selfstr(ujs_args *a) { return ujs_tostr(a->vm, a->self); }

/* ---- Math ---------------------------------------------------------------- */
static ujs_val m_floor(ujs_args *a) { return ujs_number(ujs_floor(argnum(a,0))); }
static ujs_val m_ceil (ujs_args *a) { return ujs_number(ujs_ceil(argnum(a,0))); }
static ujs_val m_abs  (ujs_args *a) { return ujs_number(ujs_fabs(argnum(a,0))); }
static ujs_val m_sqrt (ujs_args *a) { return ujs_number(ujs_sqrt(argnum(a,0))); }
static ujs_val m_sin  (ujs_args *a) { return ujs_number(ujs_sin(argnum(a,0))); }
static ujs_val m_cos  (ujs_args *a) { return ujs_number(ujs_cos(argnum(a,0))); }
static ujs_val m_tan  (ujs_args *a) { return ujs_number(ujs_tan(argnum(a,0))); }
static ujs_val m_atan2(ujs_args *a) { return ujs_number(ujs_atan2(argnum(a,0), argnum(a,1))); }
static ujs_val m_log  (ujs_args *a) { return ujs_number(ujs_log(argnum(a,0))); }
static ujs_val m_exp  (ujs_args *a) { return ujs_number(ujs_exp(argnum(a,0))); }
static ujs_val m_pow  (ujs_args *a) { return ujs_number(ujs_pow(argnum(a,0), argnum(a,1))); }

static ujs_val m_round(ujs_args *a)
{   /* JS rounds halves toward +Infinity, which is not C's round() for -0.5 */
    double d = argnum(a, 0);
    if (d != d) return ujs_number(d);
    return ujs_number(ujs_floor(d + 0.5));
}

static ujs_val m_min(ujs_args *a)
{
    double r = 1.0/0.0; int i;
    for (i = 0; i < a->argc; i++) {
        double d = argnum(a, i);
        if (d != d) return ujs_number(0.0/0.0);
        if (d < r) r = d;
    }
    return ujs_number(r);
}

static ujs_val m_max(ujs_args *a)
{
    double r = -1.0/0.0; int i;
    for (i = 0; i < a->argc; i++) {
        double d = argnum(a, i);
        if (d != d) return ujs_number(0.0/0.0);
        if (d > r) r = d;
    }
    return ujs_number(r);
}

/* A deterministic LCG. Deterministic is a FEATURE here: golden tests and the
 * screenshot harness must reproduce, and no page should be relying on
 * cryptographic quality from Math.random anyway. */
static ujs_val m_random(ujs_args *a)
{
    static unsigned long long seed = 0x2545F4914F6CDD1DULL;
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    (void)a;
    return ujs_number((double)((seed >> 11) & 0x1FFFFFFFFFFFFFULL) / 9007199254740992.0);
}

/* ---- global functions ---------------------------------------------------- */
double ujs_strtod_impl(const char *s, const char **end);

static ujs_val g_parseInt(ujs_args *a)
{
    ujs_str *s = argstr(a, 0);
    int radix = a->argc > 1 ? (int)argnum(a, 1) : 10;
    const char *p;
    int neg = 0;
    double r = 0;
    int any = 0;
    if (!s) return ujs_number(0.0/0.0);
    p = s->b;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
    if (!radix) radix = 10;
    if ((radix == 16 || radix == 10) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; radix = 16; }
    for (;;) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= radix) break;
        r = r * radix + d; any = 1; p++;
    }
    if (!any) return ujs_number(0.0/0.0);
    return ujs_number(neg ? -r : r);
}

static ujs_val g_parseFloat(ujs_args *a)
{
    ujs_str *s = argstr(a, 0);
    const char *e, *p;
    double d;
    if (!s) return ujs_number(0.0/0.0);
    p = s->b;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    d = ujs_strtod_impl(p, &e);
    return ujs_number(e == p ? 0.0/0.0 : d);
}

static ujs_val g_isNaN(ujs_args *a) { double d = argnum(a,0); return ujs_bool(d != d); }
static ujs_val g_isFinite(ujs_args *a)
{ double d = argnum(a,0); return ujs_bool(d == d && d < 1.0/0.0 && d > -1.0/0.0); }

static ujs_val g_String(ujs_args *a)
{ ujs_str *s = a->argc ? argstr(a, 0) : ujs_str_new(a->vm, "", 0);
  return s ? ujs_str_val(s) : ujs_undefined(); }

static ujs_val g_Number(ujs_args *a)
{ return ujs_number(a->argc ? argnum(a, 0) : 0); }

static ujs_val g_Boolean(ujs_args *a) { return ujs_bool(ujs_truthy(arg(a, 0))); }

/* ---- Object -------------------------------------------------------------- */
static ujs_val o_keys(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_val ov = arg(a, 0);
    ujs_obj *r = ujs_obj_new(vm, C_ARRAY, vm->arr_proto);
    if (!r) return ujs_undefined();
    if (ujs_is_object(ov)) {
        ujs_obj *o = ujs_val_obj(ov);
        u32 i;
        for (i = 0; i < o->nelems; i++) {
            char b[24]; snprintf(b, sizeof b, "%u", i);
            ujs_arr_set(vm, r, r->nelems, ujs_string(vm, b, -1));
        }
        for (i = 0; i < o->nprops; i++) {
            ujs_str *nm;
            if (!o->props[i].atom || !(o->props[i].flags & P_ENUM)) continue;
            nm = ujs_atom_str(vm, o->props[i].atom);
            if (nm) ujs_arr_set(vm, r, r->nelems, ujs_str_val(nm));
        }
    }
    return ujs_obj_val(r);
}

static ujs_val o_values(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_val ov = arg(a, 0);
    ujs_obj *r = ujs_obj_new(vm, C_ARRAY, vm->arr_proto);
    if (!r) return ujs_undefined();
    if (ujs_is_object(ov)) {
        ujs_obj *o = ujs_val_obj(ov);
        u32 i;
        for (i = 0; i < o->nelems; i++) ujs_arr_set(vm, r, r->nelems, o->elems[i]);
        for (i = 0; i < o->nprops; i++) {
            if (!o->props[i].atom || !(o->props[i].flags & P_ENUM)) continue;
            if (o->props[i].flags & P_ACCESSOR) continue;
            ujs_arr_set(vm, r, r->nelems, o->props[i].v);
        }
    }
    return ujs_obj_val(r);
}

static ujs_val o_assign(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_val dst = arg(a, 0);
    int k;
    if (!ujs_is_object(dst)) return dst;
    for (k = 1; k < a->argc; k++) {
        ujs_val sv = arg(a, k);
        ujs_obj *s;
        u32 i;
        if (!ujs_is_object(sv)) continue;
        s = ujs_val_obj(sv);
        for (i = 0; i < s->nelems; i++) ujs_arr_set(vm, ujs_val_obj(dst), i, s->elems[i]);
        for (i = 0; i < s->nprops; i++) {
            if (!s->props[i].atom || !(s->props[i].flags & P_ENUM)) continue;
            ujs_obj_put(vm, ujs_val_obj(dst), s->props[i].atom, s->props[i].v, P_DEFAULT);
        }
    }
    return dst;
}

static ujs_val o_hasOwn(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_str *k;
    if (!ujs_is_object(a->self)) return ujs_bool(0);
    k = argstr(a, 0);
    if (!k) return ujs_bool(0);
    {   ujs_obj *o = ujs_val_obj(a->self);
        u32 idx = 0, i;
        int isnum = 1;
        for (i = 0; i < k->len; i++) if (k->b[i] < '0' || k->b[i] > '9') { isnum = 0; break; }
        if (isnum && k->len) { for (i = 0; i < k->len; i++) idx = idx*10 + (u32)(k->b[i]-'0');
                               return ujs_bool(idx < o->nelems); }
        return ujs_bool(ujs_obj_find(o, ujs_atom(vm, k->b, (int)k->len)) != NULL);
    }
}

/* ---- Array.prototype ----------------------------------------------------- */
static ujs_obj *self_arr(ujs_args *a)
{ return ujs_is_object(a->self) ? ujs_val_obj(a->self) : NULL; }

static ujs_val a_push(ujs_args *a)
{
    ujs_obj *o = self_arr(a); int i;
    if (!o) return ujs_number(0);
    for (i = 0; i < a->argc; i++) ujs_arr_set(a->vm, o, o->nelems, a->argv[i]);
    return ujs_number((double)o->nelems);
}

static ujs_val a_pop(ujs_args *a)
{
    ujs_obj *o = self_arr(a);
    if (!o || !o->nelems) return ujs_undefined();
    return o->elems[--o->nelems];
}

static ujs_val a_shift(ujs_args *a)
{
    ujs_obj *o = self_arr(a);
    ujs_val r;
    u32 i;
    if (!o || !o->nelems) return ujs_undefined();
    r = o->elems[0];
    for (i = 1; i < o->nelems; i++) o->elems[i-1] = o->elems[i];
    o->nelems--;
    return r;
}

static ujs_val a_unshift(ujs_args *a)
{
    ujs_obj *o = self_arr(a);
    int k;
    u32 i;
    if (!o) return ujs_number(0);
    for (k = a->argc - 1; k >= 0; k--) {
        ujs_arr_set(a->vm, o, o->nelems, ujs_undefined());
        for (i = o->nelems - 1; i > 0; i--) o->elems[i] = o->elems[i-1];
        o->elems[0] = a->argv[k];
    }
    return ujs_number((double)o->nelems);
}

static ujs_val a_join(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_obj *o = self_arr(a);
    ujs_str *sep = a->argc ? argstr(a, 0) : ujs_str_new(vm, ",", 1);
    ujs_str *acc = ujs_str_new(vm, "", 0);
    u32 i;
    if (!o || !sep || !acc) return ujs_undefined();
    for (i = 0; i < o->nelems; i++) {
        if (i) { acc = ujs_str_cat(vm, acc, sep); if (!acc) return ujs_undefined(); }
        if (ujs_is_undefined(o->elems[i]) || ujs_is_null(o->elems[i])) continue;
        { ujs_str *p = ujs_tostr(vm, o->elems[i]);
          if (!p) return ujs_undefined();
          acc = ujs_str_cat(vm, acc, p);
          if (!acc) return ujs_undefined(); }
    }
    return ujs_str_val(acc);
}

static ujs_val a_indexOf(ujs_args *a)
{
    ujs_obj *o = self_arr(a);
    u32 i, from = 0;
    if (!o) return ujs_number(-1);
    /* fromIndex, for the same reason s_indexOf honours it now: a scanner
     * that resumes from its last hit must be able to */
    if (a->argc > 1 && !ujs_is_undefined(arg(a, 1))) {
        double d = argnum(a, 1);
        if (d != d || d < 0) d = 0;
        from = d > (double)o->nelems ? o->nelems : (u32)d;
    }
    for (i = from; i < o->nelems; i++)
        if (ujs_strict_eq(o->elems[i], arg(a, 0))) return ujs_number((double)i);
    return ujs_number(-1);
}

static ujs_val a_slice(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_obj *o = self_arr(a), *r;
    double s, e;
    u32 i;
    if (!o) return ujs_undefined();
    r = ujs_obj_new(vm, C_ARRAY, vm->arr_proto);
    if (!r) return ujs_undefined();
    s = a->argc > 0 ? argnum(a, 0) : 0;
    e = a->argc > 1 && !ujs_is_undefined(arg(a,1)) ? argnum(a, 1) : (double)o->nelems;
    if (s < 0) s += o->nelems;
    if (s < 0) s = 0;
    if (e < 0) e += o->nelems;
    if (e > o->nelems) e = o->nelems;
    for (i = (u32)s; i < (u32)e && i < o->nelems; i++) ujs_arr_set(vm, r, r->nelems, o->elems[i]);
    return ujs_obj_val(r);
}

static ujs_val a_concat(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_obj *o = self_arr(a), *r;
    int k;
    u32 i;
    if (!o) return ujs_undefined();
    r = ujs_obj_new(vm, C_ARRAY, vm->arr_proto);
    if (!r) return ujs_undefined();
    for (i = 0; i < o->nelems; i++) ujs_arr_set(vm, r, r->nelems, o->elems[i]);
    for (k = 0; k < a->argc; k++) {
        ujs_val v = a->argv[k];
        if (ujs_is_array(vm, v)) {
            ujs_obj *s = ujs_val_obj(v);
            for (i = 0; i < s->nelems; i++) ujs_arr_set(vm, r, r->nelems, s->elems[i]);
        } else ujs_arr_set(vm, r, r->nelems, v);
    }
    return ujs_obj_val(r);
}

static ujs_val a_reverse(ujs_args *a)
{
    ujs_obj *o = self_arr(a);
    u32 i;
    if (!o) return ujs_undefined();
    for (i = 0; i < o->nelems / 2; i++) {
        ujs_val t = o->elems[i];
        o->elems[i] = o->elems[o->nelems-1-i];
        o->elems[o->nelems-1-i] = t;
    }
    return a->self;
}

/* map/filter/forEach/reduce all call back into JS, so they must handle a
 * callee that throws: bail out immediately and let the exception propagate. */
static ujs_val a_forEach(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_obj *o = self_arr(a);
    u32 i;
    if (!o) return ujs_undefined();
    for (i = 0; i < o->nelems; i++) {
        ujs_val args[3], r;
        args[0] = o->elems[i]; args[1] = ujs_number((double)i); args[2] = a->self;
        if (ujs_call_value(vm, arg(a,0), arg(a,1), 3, args, &r, 0) != UJS_OK) return ujs_undefined();
    }
    return ujs_undefined();
}

static ujs_val a_map(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_obj *o = self_arr(a), *out;
    u32 i;
    if (!o) return ujs_undefined();
    out = ujs_obj_new(vm, C_ARRAY, vm->arr_proto);
    if (!out) return ujs_undefined();
    for (i = 0; i < o->nelems; i++) {
        ujs_val args[3], r;
        args[0] = o->elems[i]; args[1] = ujs_number((double)i); args[2] = a->self;
        if (ujs_call_value(vm, arg(a,0), arg(a,1), 3, args, &r, 0) != UJS_OK) return ujs_undefined();
        ujs_arr_set(vm, out, out->nelems, r);
    }
    return ujs_obj_val(out);
}

static ujs_val a_filter(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_obj *o = self_arr(a), *out;
    u32 i;
    if (!o) return ujs_undefined();
    out = ujs_obj_new(vm, C_ARRAY, vm->arr_proto);
    if (!out) return ujs_undefined();
    for (i = 0; i < o->nelems; i++) {
        ujs_val args[3], r;
        args[0] = o->elems[i]; args[1] = ujs_number((double)i); args[2] = a->self;
        if (ujs_call_value(vm, arg(a,0), arg(a,1), 3, args, &r, 0) != UJS_OK) return ujs_undefined();
        if (ujs_truthy(r)) ujs_arr_set(vm, out, out->nelems, o->elems[i]);
    }
    return ujs_obj_val(out);
}

static ujs_val a_reduce(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_obj *o = self_arr(a);
    ujs_val acc;
    u32 i = 0;
    if (!o) return ujs_undefined();
    if (a->argc > 1) acc = a->argv[1];
    else {
        if (!o->nelems) return ujs_throw_error(vm, "TypeError", "reduce of empty array with no initial value");
        acc = o->elems[0]; i = 1;
    }
    for (; i < o->nelems; i++) {
        ujs_val args[4], r;
        args[0] = acc; args[1] = o->elems[i];
        args[2] = ujs_number((double)i); args[3] = a->self;
        if (ujs_call_value(vm, arg(a,0), ujs_undefined(), 4, args, &r, 0) != UJS_OK)
            return ujs_undefined();
        acc = r;
    }
    return acc;
}

/* insertion sort: n is small in practice and it is stable, which matters for
 * the comparator contract. A throwing comparator aborts the sort. */
static ujs_val a_sort(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_obj *o = self_arr(a);
    ujs_val cmp = arg(a, 0);
    u32 i, j;
    if (!o) return ujs_undefined();
    for (i = 1; i < o->nelems; i++) {
        ujs_val key = o->elems[i];
        j = i;
        while (j > 0) {
            int before;
            if (ujs_is_function(vm, cmp)) {
                ujs_val args[2], r;
                args[0] = o->elems[j-1]; args[1] = key;
                if (ujs_call_value(vm, cmp, ujs_undefined(), 2, args, &r, 0) != UJS_OK)
                    return ujs_undefined();
                before = ujs_num_of(vm, r) > 0;
            } else {
                ujs_str *x = ujs_tostr(vm, o->elems[j-1]), *y = ujs_tostr(vm, key);
                if (!x || !y) return ujs_undefined();
                {   u32 n = x->len < y->len ? x->len : y->len;
                    int c = memcmp(x->b, y->b, n);
                    if (!c) c = x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
                    before = c > 0; }
            }
            if (!before) break;
            o->elems[j] = o->elems[j-1];
            j--;
        }
        o->elems[j] = key;
    }
    return a->self;
}

static ujs_val a_isArray(ujs_args *a) { return ujs_bool(ujs_is_array(a->vm, arg(a,0))); }

/* ---- String.prototype ---------------------------------------------------- */
static ujs_val s_charAt(ujs_args *a)
{
    ujs_str *s = selfstr(a);
    int i = (int)argnum(a, 0);
    if (!s) return ujs_undefined();
    if (i < 0 || i >= (int)s->len) return ujs_string(a->vm, "", 0);
    return ujs_string(a->vm, s->b + i, 1);
}

static ujs_val s_charCodeAt(ujs_args *a)
{
    ujs_str *s = selfstr(a);
    int i = (int)argnum(a, 0);
    if (!s || i < 0 || i >= (int)s->len) return ujs_number(0.0/0.0);
    return ujs_number((double)(u8)s->b[i]);
}

static ujs_val s_indexOf(ujs_args *a)
{
    ujs_str *s = selfstr(a), *n = argstr(a, 0);
    u32 i, from = 0;
    if (!s || !n) return ujs_number(-1);
    /* fromIndex.  Dropping it looked harmless and was not: every scanner
     * written as `i = s.indexOf(x, i) + 1` finds the SAME first hit for
     * ever, which turns a three-line tokenizer into an infinite loop that
     * grinds its whole fuel budget before the first frame paints. */
    if (a->argc > 1 && !ujs_is_undefined(arg(a, 1))) {
        double d = argnum(a, 1);
        if (d != d || d < 0) d = 0;
        from = d > (double)s->len ? s->len : (u32)d;
    }
    if (!n->len) return ujs_number((double)from);
    if (n->len > s->len) return ujs_number(-1);
    for (i = from; i + n->len <= s->len; i++)
        if (!memcmp(s->b + i, n->b, n->len)) return ujs_number((double)i);
    return ujs_number(-1);
}

static ujs_val s_lastIndexOf(ujs_args *a)
{
    ujs_str *s = selfstr(a), *n = argstr(a, 0);
    u32 i;
    if (!s || !n || n->len > s->len) return ujs_number(-1);
    for (i = s->len - n->len + 1; i > 0; i--)
        if (!memcmp(s->b + i - 1, n->b, n->len)) return ujs_number((double)(i - 1));
    return ujs_number(-1);
}

static ujs_val s_slice(ujs_args *a)
{
    ujs_str *s = selfstr(a);
    double st, e;
    if (!s) return ujs_undefined();
    st = a->argc > 0 ? argnum(a, 0) : 0;
    e  = (a->argc > 1 && !ujs_is_undefined(arg(a,1))) ? argnum(a, 1) : (double)s->len;
    if (st < 0) st += s->len;
    if (st < 0) st = 0;
    if (st > s->len) st = s->len;
    if (e < 0) e += s->len;
    if (e < 0) e = 0;
    if (e > s->len) e = s->len;
    if (e <= st) return ujs_string(a->vm, "", 0);
    return ujs_string(a->vm, s->b + (int)st, (int)(e - st));
}

static ujs_val s_substring(ujs_args *a)
{
    ujs_str *s = selfstr(a);
    double st, e, t;
    if (!s) return ujs_undefined();
    st = a->argc > 0 ? argnum(a, 0) : 0;
    e  = (a->argc > 1 && !ujs_is_undefined(arg(a,1))) ? argnum(a, 1) : (double)s->len;
    if (st < 0 || st != st) st = 0;
    if (st > s->len) st = s->len;
    if (e < 0 || e != e) e = 0;
    if (e > s->len) e = s->len;
    if (st > e) { t = st; st = e; e = t; }
    return ujs_string(a->vm, s->b + (int)st, (int)(e - st));
}

static ujs_val s_toUpper(ujs_args *a)
{
    ujs_str *s = selfstr(a);
    ujs_str *r;
    u32 i;
    if (!s) return ujs_undefined();
    r = ujs_str_new(a->vm, s->b, (int)s->len);
    if (!r) return ujs_undefined();
    for (i = 0; i < r->len; i++) if (r->b[i] >= 'a' && r->b[i] <= 'z') r->b[i] -= 32;
    return ujs_str_val(r);
}

static ujs_val s_toLower(ujs_args *a)
{
    ujs_str *s = selfstr(a);
    ujs_str *r;
    u32 i;
    if (!s) return ujs_undefined();
    r = ujs_str_new(a->vm, s->b, (int)s->len);
    if (!r) return ujs_undefined();
    for (i = 0; i < r->len; i++) if (r->b[i] >= 'A' && r->b[i] <= 'Z') r->b[i] += 32;
    return ujs_str_val(r);
}

static ujs_val s_trim(ujs_args *a)
{
    ujs_str *s = selfstr(a);
    u32 b, e;
    if (!s) return ujs_undefined();
    b = 0; e = s->len;
    while (b < e && (s->b[b]==' '||s->b[b]=='\t'||s->b[b]=='\n'||s->b[b]=='\r')) b++;
    while (e > b && (s->b[e-1]==' '||s->b[e-1]=='\t'||s->b[e-1]=='\n'||s->b[e-1]=='\r')) e--;
    return ujs_string(a->vm, s->b + b, (int)(e - b));
}

static ujs_val s_split(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_str *s = selfstr(a), *sep;
    ujs_obj *r;
    u32 i, last = 0;
    if (!s) return ujs_undefined();
    r = ujs_obj_new(vm, C_ARRAY, vm->arr_proto);
    if (!r) return ujs_undefined();
    if (!a->argc || ujs_is_undefined(arg(a,0))) {
        ujs_arr_set(vm, r, 0, ujs_str_val(s));
        return ujs_obj_val(r);
    }
    sep = argstr(a, 0);
    if (!sep) return ujs_undefined();
    if (!sep->len) {                                  /* "" splits into chars */
        for (i = 0; i < s->len; i++) ujs_arr_set(vm, r, r->nelems, ujs_string(vm, s->b+i, 1));
        return ujs_obj_val(r);
    }
    for (i = 0; i + sep->len <= s->len; ) {
        if (!memcmp(s->b + i, sep->b, sep->len)) {
            ujs_arr_set(vm, r, r->nelems, ujs_string(vm, s->b + last, (int)(i - last)));
            i += sep->len; last = i;
        } else i++;
    }
    ujs_arr_set(vm, r, r->nelems, ujs_string(vm, s->b + last, (int)(s->len - last)));
    return ujs_obj_val(r);
}

static ujs_val s_replace(ujs_args *a)
{
    /* string-pattern replace only (no RegExp until M1c): first occurrence. */
    ujs_vm *vm = a->vm;
    ujs_str *s = selfstr(a), *pat = argstr(a, 0), *rep = argstr(a, 1);
    u32 i;
    if (!s || !pat || !rep) return ujs_undefined();
    if (pat->len && pat->len <= s->len) {
        for (i = 0; i + pat->len <= s->len; i++) {
            if (memcmp(s->b + i, pat->b, pat->len)) continue;
            {   ujs_str *head = ujs_str_new(vm, s->b, (int)i);
                ujs_str *tail = ujs_str_new(vm, s->b + i + pat->len,
                                            (int)(s->len - i - pat->len));
                if (!head || !tail) return ujs_undefined();
                head = ujs_str_cat(vm, head, rep);
                if (!head) return ujs_undefined();
                head = ujs_str_cat(vm, head, tail);
                return head ? ujs_str_val(head) : ujs_undefined(); }
        }
    }
    return ujs_str_val(s);
}

static ujs_val s_includes(ujs_args *a)
{ ujs_val r = s_indexOf(a); return ujs_bool(ujs_num_of(a->vm, r) >= 0); }

static ujs_val s_startsWith(ujs_args *a)
{
    ujs_str *s = selfstr(a), *n = argstr(a, 0);
    if (!s || !n || n->len > s->len) return ujs_bool(0);
    return ujs_bool(!memcmp(s->b, n->b, n->len));
}

static ujs_val s_endsWith(ujs_args *a)
{
    ujs_str *s = selfstr(a), *n = argstr(a, 0);
    if (!s || !n || n->len > s->len) return ujs_bool(0);
    return ujs_bool(!memcmp(s->b + s->len - n->len, n->b, n->len));
}

static ujs_val s_repeat(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_str *s = selfstr(a), *r;
    int n = (int)argnum(a, 0), i;
    if (!s) return ujs_undefined();
    if (n < 0) return ujs_throw_error(vm, "RangeError", "invalid repeat count");
    if ((double)n * s->len > 16.0 * 1024 * 1024)
        return ujs_throw_error(vm, "RangeError", "repeat result too large");
    r = ujs_str_new(vm, "", 0);
    for (i = 0; i < n && r; i++) r = ujs_str_cat(vm, r, s);
    return r ? ujs_str_val(r) : ujs_undefined();
}

static ujs_val s_concat(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_str *r = selfstr(a);
    int i;
    for (i = 0; i < a->argc && r; i++) {
        ujs_str *p = argstr(a, i);
        if (!p) return ujs_undefined();
        r = ujs_str_cat(vm, r, p);
    }
    return r ? ujs_str_val(r) : ujs_undefined();
}

static ujs_val s_fromCharCode(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_str *r = ujs_str_new(vm, "", 0);
    int i;
    for (i = 0; i < a->argc && r; i++) {
        char c = (char)(int)argnum(a, i);
        ujs_str *p = ujs_str_new(vm, &c, 1);
        if (!p) return ujs_undefined();
        r = ujs_str_cat(vm, r, p);
    }
    return r ? ujs_str_val(r) : ujs_undefined();
}

/* ---- Number.prototype ---------------------------------------------------- */
static ujs_val n_toFixed(ujs_args *a)
{
    double d = ujs_num_of(a->vm, a->self);
    int p = (int)argnum(a, 0), i;
    char buf[64];
    double scale = 1;
    if (p < 0 || p > 20) return ujs_throw_error(a->vm, "RangeError", "toFixed digits out of range");
    for (i = 0; i < p; i++) scale *= 10;
    d = (d < 0 ? -1 : 1) * ujs_floor(ujs_fabs(d) * scale + 0.5) / scale;
    if (!p) { ujs_num_to_str(d, buf, sizeof buf); return ujs_string(a->vm, buf, -1); }
    {   /* print with exactly p fractional digits */
        double ad = ujs_fabs(d);
        double ip = ujs_floor(ad);
        double fr = ujs_floor((ad - ip) * scale + 0.5);
        char ib[32], fb[32];
        int n = 0, k;
        u64 iv = (u64)ip, fv = (u64)fr;
        if (!iv) ib[n++] = '0';
        while (iv) { ib[n++] = (char)('0' + (int)(iv % 10)); iv /= 10; }
        k = 0;
        for (i = 0; i < p; i++) { fb[k++] = (char)('0' + (int)(fv % 10)); fv /= 10; }
        {   int o = 0;
            if (d < 0) buf[o++] = '-';
            while (n) buf[o++] = ib[--n];
            buf[o++] = '.';
            while (k) buf[o++] = fb[--k];
            buf[o] = 0; }
    }
    return ujs_string(a->vm, buf, -1);
}

static ujs_val n_toString(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    double d = ujs_num_of(vm, a->self);
    int radix = a->argc ? (int)argnum(a, 0) : 10;
    char buf[80];
    if (radix == 10 || !a->argc) { ujs_num_to_str(d, buf, sizeof buf); return ujs_string(vm, buf, -1); }
    if (radix < 2 || radix > 36) return ujs_throw_error(vm, "RangeError", "radix out of range");
    {   int neg = d < 0, n = 0;
        u64 iv = (u64)(neg ? -d : d);
        char t[72];
        if (!iv) t[n++] = '0';
        while (iv) { int dg = (int)(iv % (u64)radix);
                     t[n++] = (char)(dg < 10 ? '0' + dg : 'a' + dg - 10); iv /= (u64)radix; }
        {   int o = 0;
            if (neg) buf[o++] = '-';
            while (n) buf[o++] = t[--n];
            buf[o] = 0; }
    }
    return ujs_string(vm, buf, -1);
}

/* ---- Object.prototype / Function.prototype ------------------------------- */
static ujs_val op_toString(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    if (ujs_is_array(vm, a->self)) return a_join(a);
    return ujs_string(vm, "[object Object]", -1);
}

static ujs_val fp_call(ujs_args *a)
{
    ujs_val r;
    if (ujs_call_value(a->vm, a->self, arg(a, 0),
                       a->argc > 1 ? a->argc - 1 : 0,
                       a->argc > 1 ? a->argv + 1 : NULL, &r, 0) != UJS_OK)
        return ujs_undefined();
    return r;
}

static ujs_val fp_apply(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_val list = arg(a, 1), r;
    if (ujs_is_array(vm, list)) {
        ujs_obj *l = ujs_val_obj(list);
        if (ujs_call_value(vm, a->self, arg(a,0), (int)l->nelems, l->elems, &r, 0) != UJS_OK)
            return ujs_undefined();
        return r;
    }
    if (ujs_call_value(vm, a->self, arg(a,0), 0, NULL, &r, 0) != UJS_OK) return ujs_undefined();
    return r;
}

/* ---- Error --------------------------------------------------------------- */
static ujs_val e_toString(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_val nm, msg;
    ujs_str *sn, *sm;
    if (!ujs_is_object(a->self)) return ujs_string(vm, "Error", -1);
    ujs_obj_get(vm, ujs_val_obj(a->self), ujs_atom(vm, "name", -1), &nm);
    ujs_obj_get(vm, ujs_val_obj(a->self), ujs_atom(vm, "message", -1), &msg);
    sn = ujs_tostr(vm, ujs_is_undefined(nm) ? ujs_string(vm, "Error", -1) : nm);
    sm = ujs_tostr(vm, ujs_is_undefined(msg) ? ujs_string(vm, "", 0) : msg);
    if (!sn || !sm) return ujs_undefined();
    if (!sm->len) return ujs_str_val(sn);
    {   ujs_str *r = ujs_str_cat(vm, sn, ujs_str_new(vm, ": ", 2));
        if (!r) return ujs_undefined();
        r = ujs_str_cat(vm, r, sm);
        return r ? ujs_str_val(r) : ujs_undefined(); }
}

/* Error constructors are host functions used with `new`: the VM has already
 * made the object and passed it as `this`, so they just fill in `message`. */
static ujs_val err_ctor_generic(ujs_args *a, const char *name)
{
    ujs_vm *vm = a->vm;
    ujs_val target = ujs_is_object(a->self) ? a->self
                   : ujs_obj_val(ujs_obj_new(vm, C_ERROR, vm->err_proto));
    if (!ujs_is_object(target)) return ujs_undefined();
    ujs_obj_put(vm, ujs_val_obj(target), ujs_atom(vm, "name", -1),
                ujs_string(vm, name, -1), P_DEFAULT);
    if (a->argc) {
        ujs_str *m = argstr(a, 0);
        if (m) ujs_obj_put(vm, ujs_val_obj(target), ujs_atom(vm, "message", -1),
                           ujs_str_val(m), P_DEFAULT);
    }
    return target;
}
static ujs_val e_Error     (ujs_args *a) { return err_ctor_generic(a, "Error"); }
static ujs_val e_TypeError (ujs_args *a) { return err_ctor_generic(a, "TypeError"); }
static ujs_val e_RangeError(ujs_args *a) { return err_ctor_generic(a, "RangeError"); }

/* ---- JSON ---------------------------------------------------------------- */
static ujs_str *json_str(ujs_vm *vm, ujs_val v, int depth);

static ujs_str *json_quote(ujs_vm *vm, ujs_str *s)
{
    ujs_str *r = ujs_str_new(vm, "\"", 1);
    u32 i;
    for (i = 0; i < s->len && r; i++) {
        char c = s->b[i];
        const char *esc = NULL;
        char one[2];
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\t': esc = "\\t"; break;
        case '\r': esc = "\\r"; break;
        default: one[0] = c; one[1] = 0; break;
        }
        r = ujs_str_cat(vm, r, esc ? ujs_str_new(vm, esc, 2) : ujs_str_new(vm, one, 1));
    }
    return r ? ujs_str_cat(vm, r, ujs_str_new(vm, "\"", 1)) : NULL;
}

static ujs_str *json_str(ujs_vm *vm, ujs_val v, int depth)
{
    char buf[64];
    if (depth > 32) return ujs_str_new(vm, "null", 4);      /* cycle guard */
    switch (ujs_typeof(v)) {
    case UJS_TYPE_UNDEFINED: return NULL;                   /* omitted */
    case UJS_TYPE_NULL:  return ujs_str_new(vm, "null", 4);
    case UJS_TYPE_BOOL:  return ujs_str_new(vm, ujs_truthy(v) ? "true" : "false", -1);
    case UJS_TYPE_NUMBER: { double d = ujs_dbl(v);
                            if (d != d || d > 1.7e308 || d < -1.7e308) return ujs_str_new(vm, "null", 4);
                            ujs_num_to_str(d, buf, sizeof buf);
                            return ujs_str_new(vm, buf, -1); }
    case UJS_TYPE_STRING: return json_quote(vm, ujs_val_str(v));
    default: break;
    }
    {   ujs_obj *o = ujs_val_obj(v);
        ujs_str *r;
        u32 i;
        int first = 1;
        if (o->cls == C_FUNC || o->cls == C_CFUNC) return NULL;
        if (o->cls == C_ARRAY) {
            r = ujs_str_new(vm, "[", 1);
            for (i = 0; i < o->nelems && r; i++) {
                ujs_str *e = json_str(vm, o->elems[i], depth + 1);
                if (!e) e = ujs_str_new(vm, "null", 4);
                if (!first) r = ujs_str_cat(vm, r, ujs_str_new(vm, ",", 1));
                first = 0;
                if (r) r = ujs_str_cat(vm, r, e);
            }
            return r ? ujs_str_cat(vm, r, ujs_str_new(vm, "]", 1)) : NULL;
        }
        r = ujs_str_new(vm, "{", 1);
        for (i = 0; i < o->nelems && r; i++) {
            char nb[24];
            ujs_str *e = json_str(vm, o->elems[i], depth + 1);
            if (!e) continue;
            snprintf(nb, sizeof nb, "%u", i);
            if (!first) r = ujs_str_cat(vm, r, ujs_str_new(vm, ",", 1));
            first = 0;
            if (r) r = ujs_str_cat(vm, r, json_quote(vm, ujs_str_new(vm, nb, -1)));
            if (r) r = ujs_str_cat(vm, r, ujs_str_new(vm, ":", 1));
            if (r) r = ujs_str_cat(vm, r, e);
        }
        for (i = 0; i < o->nprops && r; i++) {
            ujs_str *nm, *e;
            if (!o->props[i].atom || !(o->props[i].flags & P_ENUM)) continue;
            if (o->props[i].flags & P_ACCESSOR) continue;
            e = json_str(vm, o->props[i].v, depth + 1);
            if (!e) continue;
            nm = ujs_atom_str(vm, o->props[i].atom);
            if (!nm) continue;
            if (!first) r = ujs_str_cat(vm, r, ujs_str_new(vm, ",", 1));
            first = 0;
            if (r) r = ujs_str_cat(vm, r, json_quote(vm, nm));
            if (r) r = ujs_str_cat(vm, r, ujs_str_new(vm, ":", 1));
            if (r) r = ujs_str_cat(vm, r, e);
        }
        return r ? ujs_str_cat(vm, r, ujs_str_new(vm, "}", 1)) : NULL;
    }
}

static ujs_val j_stringify(ujs_args *a)
{
    ujs_str *r = json_str(a->vm, arg(a, 0), 0);
    return r ? ujs_str_val(r) : ujs_undefined();
}

/* ---- installation -------------------------------------------------------- */
static void def(ujs_vm *vm, ujs_obj *o, const char *name, ujs_cfunc fn, int nargs)
{ ujs_set_fn(vm, ujs_obj_val(o), name, fn, nargs); }

static void defval(ujs_vm *vm, ujs_obj *o, const char *name, ujs_val v)
{ ujs_obj_put(vm, o, ujs_atom(vm, name, -1), v, P_WRITE | P_CONFIG); }

void ujs_lib_init(ujs_vm *vm)
{
    ujs_obj *g = vm->global;

    /* global values */
    defval(vm, g, "undefined", ujs_undefined());
    defval(vm, g, "NaN", ujs_number(0.0/0.0));
    defval(vm, g, "Infinity", ujs_number(1.0/0.0));
    defval(vm, g, "globalThis", ujs_obj_val(g));

    def(vm, g, "parseInt", g_parseInt, 2);
    def(vm, g, "parseFloat", g_parseFloat, 1);
    def(vm, g, "isNaN", g_isNaN, 1);
    def(vm, g, "isFinite", g_isFinite, 1);

    /* Object.prototype */
    def(vm, vm->obj_proto, "toString", op_toString, 0);
    def(vm, vm->obj_proto, "hasOwnProperty", o_hasOwn, 1);
    def(vm, vm->obj_proto, "valueOf", op_toString, 0);

    /* Function.prototype */
    def(vm, vm->fun_proto, "call", fp_call, 1);
    def(vm, vm->fun_proto, "apply", fp_apply, 2);

    /* Object */
    {   ujs_obj *O = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
        def(vm, O, "keys", o_keys, 1);
        def(vm, O, "values", o_values, 1);
        def(vm, O, "assign", o_assign, 2);
        defval(vm, g, "Object", ujs_obj_val(O)); }

    /* Array + Array.prototype */
    def(vm, vm->arr_proto, "push", a_push, 1);
    def(vm, vm->arr_proto, "pop", a_pop, 0);
    def(vm, vm->arr_proto, "shift", a_shift, 0);
    def(vm, vm->arr_proto, "unshift", a_unshift, 1);
    def(vm, vm->arr_proto, "join", a_join, 1);
    def(vm, vm->arr_proto, "indexOf", a_indexOf, 1);
    def(vm, vm->arr_proto, "slice", a_slice, 2);
    def(vm, vm->arr_proto, "concat", a_concat, 1);
    def(vm, vm->arr_proto, "reverse", a_reverse, 0);
    def(vm, vm->arr_proto, "forEach", a_forEach, 1);
    def(vm, vm->arr_proto, "map", a_map, 1);
    def(vm, vm->arr_proto, "filter", a_filter, 1);
    def(vm, vm->arr_proto, "reduce", a_reduce, 2);
    def(vm, vm->arr_proto, "sort", a_sort, 1);
    def(vm, vm->arr_proto, "toString", a_join, 0);
    {   ujs_obj *A = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
        def(vm, A, "isArray", a_isArray, 1);
        defval(vm, A, "prototype", ujs_obj_val(vm->arr_proto));
        defval(vm, g, "Array", ujs_obj_val(A)); }

    /* String + String.prototype */
    def(vm, vm->str_proto, "charAt", s_charAt, 1);
    def(vm, vm->str_proto, "charCodeAt", s_charCodeAt, 1);
    def(vm, vm->str_proto, "indexOf", s_indexOf, 1);
    def(vm, vm->str_proto, "lastIndexOf", s_lastIndexOf, 1);
    def(vm, vm->str_proto, "slice", s_slice, 2);
    def(vm, vm->str_proto, "substring", s_substring, 2);
    def(vm, vm->str_proto, "substr", s_slice, 2);
    def(vm, vm->str_proto, "toUpperCase", s_toUpper, 0);
    def(vm, vm->str_proto, "toLowerCase", s_toLower, 0);
    def(vm, vm->str_proto, "trim", s_trim, 0);
    def(vm, vm->str_proto, "split", s_split, 2);
    def(vm, vm->str_proto, "replace", s_replace, 2);
    def(vm, vm->str_proto, "includes", s_includes, 1);
    def(vm, vm->str_proto, "startsWith", s_startsWith, 1);
    def(vm, vm->str_proto, "endsWith", s_endsWith, 1);
    def(vm, vm->str_proto, "repeat", s_repeat, 1);
    def(vm, vm->str_proto, "concat", s_concat, 1);
    {   ujs_obj *S = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
        ujs_val sv = ujs_function_new(vm, g_String, "String", 1);
        if (ujs_is_object(sv)) {
            S = ujs_val_obj(sv);
            def(vm, S, "fromCharCode", s_fromCharCode, 1);
            defval(vm, S, "prototype", ujs_obj_val(vm->str_proto));
        }
        defval(vm, g, "String", sv); }

    /* Number */
    def(vm, vm->num_proto, "toFixed", n_toFixed, 1);
    def(vm, vm->num_proto, "toString", n_toString, 1);
    {   ujs_val nv = ujs_function_new(vm, g_Number, "Number", 1);
        if (ujs_is_object(nv)) {
            ujs_obj *N = ujs_val_obj(nv);
            defval(vm, N, "prototype", ujs_obj_val(vm->num_proto));
            defval(vm, N, "MAX_SAFE_INTEGER", ujs_number(9007199254740991.0));
            defval(vm, N, "MIN_SAFE_INTEGER", ujs_number(-9007199254740991.0));
            defval(vm, N, "MAX_VALUE", ujs_number(1.7976931348623157e308));
            def(vm, N, "isNaN", g_isNaN, 1);
            def(vm, N, "isFinite", g_isFinite, 1);
            def(vm, N, "parseFloat", g_parseFloat, 1);
            def(vm, N, "parseInt", g_parseInt, 2);
        }
        defval(vm, g, "Number", nv); }

    defval(vm, g, "Boolean", ujs_function_new(vm, g_Boolean, "Boolean", 1));

    /* Math */
    {   ujs_obj *M = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
        def(vm, M, "floor", m_floor, 1);  def(vm, M, "ceil", m_ceil, 1);
        def(vm, M, "round", m_round, 1);  def(vm, M, "abs", m_abs, 1);
        def(vm, M, "sqrt", m_sqrt, 1);    def(vm, M, "pow", m_pow, 2);
        def(vm, M, "min", m_min, 2);      def(vm, M, "max", m_max, 2);
        def(vm, M, "sin", m_sin, 1);      def(vm, M, "cos", m_cos, 1);
        def(vm, M, "tan", m_tan, 1);      def(vm, M, "atan2", m_atan2, 2);
        def(vm, M, "log", m_log, 1);      def(vm, M, "exp", m_exp, 1);
        def(vm, M, "random", m_random, 0);
        defval(vm, M, "PI", ujs_number(3.14159265358979323846));
        defval(vm, M, "E", ujs_number(2.71828182845904523536));
        defval(vm, g, "Math", ujs_obj_val(M)); }

    /* JSON */
    {   ujs_obj *J = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
        def(vm, J, "stringify", j_stringify, 2);
        defval(vm, g, "JSON", ujs_obj_val(J)); }

    /* Error hierarchy */
    def(vm, vm->err_proto, "toString", e_toString, 0);
    ujs_obj_put(vm, vm->err_proto, ujs_atom(vm, "name", -1),
                ujs_string(vm, "Error", -1), P_DEFAULT);
    ujs_obj_put(vm, vm->err_proto, ujs_atom(vm, "message", -1),
                ujs_string(vm, "", 0), P_DEFAULT);
    {   struct { const char *n; ujs_cfunc f; } es[3] = {
            { "Error", e_Error }, { "TypeError", e_TypeError }, { "RangeError", e_RangeError } };
        int i;
        for (i = 0; i < 3; i++) {
            ujs_val cv = ujs_function_new(vm, es[i].f, es[i].n, 1);
            if (ujs_is_object(cv))
                defval(vm, ujs_val_obj(cv), "prototype", ujs_obj_val(vm->err_proto));
            defval(vm, g, es[i].n, cv);
        } }
}
