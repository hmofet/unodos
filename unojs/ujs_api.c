/* ===========================================================================
 * unojs public API - the surface declared in unojs.h, implemented over the
 * internals. Everything here is embedder-facing and must be defensive: a
 * binding layer written against this header is the boundary between the host
 * and untrusted page script.
 * ======================================================================== */
#include "ujs_int.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- value constructors -------------------------------------------------- */
ujs_val ujs_undefined(void) { return ujs_mkbits(UJS_NANISH | UJS_TAG_UNDEF); }
ujs_val ujs_null(void)      { return ujs_mkbits(UJS_NANISH | UJS_TAG_NULL); }
ujs_val ujs_bool(int b)     { return ujs_mkbits(UJS_NANISH | (b ? UJS_TAG_TRUE : UJS_TAG_FALSE)); }

ujs_val ujs_number(double d)
{
    ujs_val v;
    if (d != d) return ujs_mkbits(UJS_NANISH);      /* canonical NaN: payload 0
                                                     * keeps the box test sound */
    memcpy(&v.bits, &d, 8);
    return v;
}

ujs_val ujs_string(ujs_vm *vm, const char *s, int n)
{ return ujs_str_val(ujs_str_new(vm, s, n)); }

ujs_val ujs_object_new(ujs_vm *vm)
{ return ujs_obj_val(ujs_obj_new(vm, C_PLAIN, vm->obj_proto)); }

ujs_val ujs_array_new(ujs_vm *vm)
{ return ujs_obj_val(ujs_obj_new(vm, C_ARRAY, vm->arr_proto)); }

/* ---- predicates ---------------------------------------------------------- */
ujs_type ujs_typeof(ujs_val v)
{
    if (IS_DOUBLE(v)) return UJS_TYPE_NUMBER;
    if (IS_PTR(v)) {
        ujs_hdr *h = (ujs_hdr *)PTR_OF(v);
        return h->type == H_STR ? UJS_TYPE_STRING : UJS_TYPE_OBJECT;
    }
    switch ((u32)(VBITS(v) & 0xFF)) {
    case UJS_TAG_NULL: return UJS_TYPE_NULL;
    case UJS_TAG_TRUE: case UJS_TAG_FALSE: return UJS_TYPE_BOOL;
    default: return UJS_TYPE_UNDEFINED;
    }
}

int ujs_is_undefined(ujs_val v) { return ujs_typeof(v) == UJS_TYPE_UNDEFINED; }
int ujs_is_null(ujs_val v)      { return ujs_typeof(v) == UJS_TYPE_NULL; }
int ujs_is_number(ujs_val v)    { return IS_DOUBLE(v); }
int ujs_is_string(ujs_val v)    { return ujs_typeof(v) == UJS_TYPE_STRING; }
int ujs_is_object(ujs_val v)    { return ujs_typeof(v) == UJS_TYPE_OBJECT; }

int ujs_is_array(ujs_vm *vm, ujs_val v)
{ (void)vm; return ujs_is_object(v) && ujs_val_obj(v)->cls == C_ARRAY; }

int ujs_is_function(ujs_vm *vm, ujs_val v)
{ (void)vm; if (!ujs_is_object(v)) return 0;
  { int c = ujs_val_obj(v)->cls; return c == C_FUNC || c == C_CFUNC; } }

double ujs_to_number(ujs_vm *vm, ujs_val v) { return ujs_num_of(vm, v); }
int    ujs_to_bool(ujs_val v)               { return ujs_truthy(v); }

const char *ujs_string_bytes(ujs_vm *vm, ujs_val v, size_t *len)
{
    (void)vm;
    if (!ujs_is_string(v)) { if (len) *len = 0; return NULL; }
    { ujs_str *s = ujs_val_str(v); if (len) *len = s->len; return s->b; }
}

ujs_result ujs_to_string(ujs_vm *vm, ujs_val v, ujs_val *out)
{
    ujs_str *s = ujs_tostr(vm, v);
    if (!s) return vm->has_exception ? UJS_THROW : UJS_OOM;
    if (out) *out = ujs_str_val(s);
    return UJS_OK;
}

const char *ujs_describe(ujs_vm *vm, ujs_val v, char *buf, size_t buflen)
{
    (void)vm;
    if (!buf || buflen < 8) return "";
    switch (ujs_typeof(v)) {
    case UJS_TYPE_UNDEFINED: snprintf(buf, buflen, "undefined"); break;
    case UJS_TYPE_NULL:      snprintf(buf, buflen, "null"); break;
    case UJS_TYPE_BOOL:      snprintf(buf, buflen, "%s", ujs_truthy(v) ? "true" : "false"); break;
    case UJS_TYPE_NUMBER: { char t[48]; ujs_num_to_str(ujs_dbl(v), t, sizeof t);
                            snprintf(buf, buflen, "%s", t); break; }
    case UJS_TYPE_STRING: { ujs_str *s = ujs_val_str(v);
                            snprintf(buf, buflen, "\"%.*s\"", (int)(s->len > buflen-4 ? buflen-4 : s->len), s->b);
                            break; }
    default: { ujs_obj *o = ujs_val_obj(v);
               snprintf(buf, buflen, "%s", o->cls == C_ARRAY ? "[array]" :
                        (o->cls == C_FUNC || o->cls == C_CFUNC) ? "[function]" :
                        o->cls == C_HOST ? "[host]" : "[object]"); break; }
    }
    return buf;
}

/* ---- handle scopes + roots ----------------------------------------------- */
void ujs_scope_open(ujs_vm *vm, ujs_scope *s) { s->base = vm->nscope; }

static int scope_push(ujs_vm *vm, ujs_val v)
{
    if (vm->nscope == vm->scopecap) {
        int nc = vm->scopecap ? vm->scopecap * 2 : 64;
        ujs_val *nv = (ujs_val *)ujs_alloc_raw(vm, (size_t)nc * sizeof *nv);
        if (!nv) return 0;
        if (vm->scope_vals) { memcpy(nv, vm->scope_vals, (size_t)vm->nscope * sizeof *nv);
                              ujs_free_raw(vm, vm->scope_vals, (size_t)vm->scopecap * sizeof *nv); }
        vm->scope_vals = nv; vm->scopecap = nc;
    }
    vm->scope_vals[vm->nscope++] = v;
    return 1;
}

ujs_val ujs_scope_close(ujs_vm *vm, ujs_scope *s, ujs_val keep)
{
    vm->nscope = s->base;
    if (IS_PTR(keep)) scope_push(vm, keep);      /* re-root in the outer scope */
    return keep;
}

int ujs_root(ujs_vm *vm, ujs_val v)
{
    if (!IS_PTR(v)) return 0;
    if (vm->nroots >= UJS_MAX_ROOTS) return -1;
    vm->roots[vm->nroots++] = v;
    return 0;
}

void ujs_unroot(ujs_vm *vm, ujs_val v)
{
    int i;
    for (i = 0; i < vm->nroots; i++)
        if (VBITS(vm->roots[i]) == VBITS(v)) {
            vm->roots[i] = vm->roots[--vm->nroots];
            return;
        }
}

/* ---- properties ---------------------------------------------------------- */
/* Property access on a non-object is a TypeError in JS for null/undefined and
 * a primitive-wrapper lookup otherwise. v1 handles strings (length + methods
 * off the string prototype) and rejects the rest cleanly. */
static ujs_obj *prop_target(ujs_vm *vm, ujs_val obj, const char *what)
{
    if (ujs_is_object(obj)) return ujs_val_obj(obj);
    if (ujs_is_string(obj)) return vm->str_proto;
    if (IS_DOUBLE(obj))     return vm->num_proto;
    ujs_throwf(vm, "TypeError", "cannot %s property of %s", what,
               ujs_is_null(obj) ? "null" : "undefined");
    return NULL;
}

ujs_result ujs_get(ujs_vm *vm, ujs_val obj, const char *name, ujs_val *out)
{
    ujs_obj *o;
    u32 a;
    if (out) *out = ujs_undefined();
    /* string .length is intrinsic, not a prototype property */
    if (ujs_is_string(obj) && !strcmp(name, "length")) {
        if (out) *out = ujs_number((double)ujs_val_str(obj)->len);
        return UJS_OK;
    }
    o = prop_target(vm, obj, "read");
    if (!o) return UJS_THROW;
    a = ujs_atom(vm, name, -1);
    { ujs_val v; int r = ujs_obj_get(vm, o, a, &v);
      if (r == 2) return ujs_call_value(vm, v, obj, 0, NULL, out, 0);  /* getter */
      if (out) *out = v;
      return UJS_OK; }
}

ujs_result ujs_set(ujs_vm *vm, ujs_val obj, const char *name, ujs_val v)
{
    ujs_obj *o;
    if (!ujs_is_object(obj)) {
        if (ujs_is_null(obj) || ujs_is_undefined(obj)) {
            ujs_throwf(vm, "TypeError", "cannot set property of %s",
                       ujs_is_null(obj) ? "null" : "undefined");
            return UJS_THROW;
        }
        return UJS_OK;                            /* writes to primitives vanish */
    }
    o = ujs_val_obj(obj);
    {   u32 a = ujs_atom(vm, name, -1);
        ujs_prop *p = ujs_obj_find(o, a);
        if (p && (p->flags & P_ACCESSOR)) {
            if (ujs_is_undefined(p->setter)) return UJS_OK;
            return ujs_call_value(vm, p->setter, obj, 1, &v, NULL, 0);
        }
        if (!ujs_obj_put(vm, o, a, v, P_DEFAULT)) {
            ujs_throw_error(vm, "RangeError", "out of memory");
            return UJS_OOM;
        }
    }
    return UJS_OK;
}

ujs_result ujs_get_index(ujs_vm *vm, ujs_val obj, unsigned i, ujs_val *out)
{
    if (out) *out = ujs_undefined();
    if (ujs_is_string(obj)) {
        ujs_str *s = ujs_val_str(obj);
        if (i < s->len && out) *out = ujs_string(vm, s->b + i, 1);
        return UJS_OK;
    }
    if (!ujs_is_object(obj)) return UJS_OK;
    { ujs_obj *o = ujs_val_obj(obj);
      if (o->elems && i < o->nelems) { if (out) *out = o->elems[i]; return UJS_OK; }
      { char nb[24]; snprintf(nb, sizeof nb, "%u", i); return ujs_get(vm, obj, nb, out); } }
}

ujs_result ujs_set_index(ujs_vm *vm, ujs_val obj, unsigned i, ujs_val v)
{
    if (!ujs_is_object(obj)) return UJS_OK;
    ujs_arr_set(vm, ujs_val_obj(obj), i, v);
    return UJS_OK;
}

int ujs_has(ujs_vm *vm, ujs_val obj, const char *name)
{
    ujs_val tmp;
    if (!ujs_is_object(obj)) return 0;
    return ujs_obj_get(vm, ujs_val_obj(obj), ujs_atom(vm, name, -1), &tmp) != 0;
}

int ujs_delete(ujs_vm *vm, ujs_val obj, const char *name)
{
    if (!ujs_is_object(obj)) return 0;
    return ujs_obj_del(vm, ujs_val_obj(obj), ujs_atom(vm, name, -1));
}

unsigned ujs_array_length(ujs_vm *vm, ujs_val arr)
{ (void)vm; return ujs_is_object(arr) ? ujs_val_obj(arr)->nelems : 0; }

ujs_result ujs_array_push(ujs_vm *vm, ujs_val arr, ujs_val v)
{
    if (!ujs_is_object(arr)) return UJS_OK;
    { ujs_obj *o = ujs_val_obj(arr); ujs_arr_set(vm, o, o->nelems, v); }
    return UJS_OK;
}

/* ---- calling ------------------------------------------------------------- */
ujs_result ujs_call(ujs_vm *vm, ujs_val fn, ujs_val self,
                    int argc, const ujs_val *argv, ujs_val *out)
{ return ujs_call_value(vm, fn, self, argc, argv, out, 0); }

/* ---- host functions ------------------------------------------------------ */
ujs_val ujs_function_new(ujs_vm *vm, ujs_cfunc fn, const char *name, int nargs)
{
    ujs_obj *o = ujs_obj_new(vm, C_CFUNC, vm->fun_proto);
    if (!o) return ujs_undefined();
    o->u.cfn.fn = fn;
    o->u.cfn.nargs = nargs;
    ujs_obj_put(vm, o, ujs_atom(vm, "name", -1),
                ujs_string(vm, name ? name : "", -1), P_CONFIG);
    ujs_obj_put(vm, o, ujs_atom(vm, "length", -1), ujs_number(nargs), P_CONFIG);
    return ujs_obj_val(o);
}

ujs_val ujs_host_new(ujs_vm *vm, void *user, ujs_finalizer fin)
{
    ujs_obj *o = ujs_obj_new(vm, C_HOST, vm->obj_proto);
    if (!o) return ujs_undefined();
    o->u.host.user = user;
    o->u.host.fin = fin;
    return ujs_obj_val(o);
}

void *ujs_host_user(ujs_vm *vm, ujs_val v)
{
    (void)vm;
    if (!ujs_is_object(v)) return NULL;
    { ujs_obj *o = ujs_val_obj(v); return o->cls == C_HOST ? o->u.host.user : NULL; }
}

int ujs_set_fn(ujs_vm *vm, ujs_val obj, const char *name, ujs_cfunc fn, int nargs)
{
    ujs_val f = ujs_function_new(vm, fn, name, nargs);
    if (ujs_is_undefined(f) || !ujs_is_object(obj)) return -1;
    return ujs_obj_put(vm, ujs_val_obj(obj), ujs_atom(vm, name, -1), f, P_WRITE | P_CONFIG) ? 0 : -1;
}

int ujs_set_accessor(ujs_vm *vm, ujs_val obj, const char *name,
                     ujs_cfunc getter, ujs_cfunc setter)
{
    ujs_obj *o;
    ujs_prop *p;
    u32 a;
    if (!ujs_is_object(obj)) return -1;
    o = ujs_val_obj(obj);
    a = ujs_atom(vm, name, -1);
    if (!ujs_obj_put(vm, o, a, ujs_function_new(vm, getter, name, 0), P_ACCESSOR | P_CONFIG))
        return -1;
    p = ujs_obj_find(o, a);
    if (!p) return -1;
    p->flags |= P_ACCESSOR;
    p->setter = setter ? ujs_function_new(vm, setter, name, 1) : ujs_undefined();
    return 0;
}

ujs_val ujs_global(ujs_vm *vm) { return ujs_obj_val(vm->global); }

/* ---- exceptions ---------------------------------------------------------- */
ujs_val ujs_throw(ujs_vm *vm, ujs_val err)
{
    vm->exception = err;
    vm->has_exception = 1;
    return ujs_undefined();
}

ujs_val ujs_throw_error(ujs_vm *vm, const char *kind, const char *msg)
{
    ujs_obj *e = ujs_obj_new(vm, C_ERROR, vm->err_proto);
    if (!e) { vm->has_exception = 1; vm->exception = ujs_string(vm, msg, -1); return ujs_undefined(); }
    ujs_obj_put(vm, e, ujs_atom(vm, "name", -1), ujs_string(vm, kind, -1), P_DEFAULT);
    ujs_obj_put(vm, e, ujs_atom(vm, "message", -1), ujs_string(vm, msg, -1), P_DEFAULT);
    return ujs_throw(vm, ujs_obj_val(e));
}

ujs_val ujs_exception(ujs_vm *vm)
{ return vm->has_exception ? vm->exception : ujs_undefined(); }

void ujs_clear_exception(ujs_vm *vm)
{ vm->has_exception = 0; vm->exception = ujs_undefined(); }

/* ---- fuel + gc ----------------------------------------------------------- */
unsigned long ujs_fuel_used(ujs_vm *vm) { return vm->fuel_used; }
void ujs_fuel_reset(ujs_vm *vm)         { vm->fuel_used = 0; }
size_t ujs_heap_used(ujs_vm *vm)        { return vm->heap_used; }

/* ---- lifecycle ----------------------------------------------------------- */
ujs_vm *ujs_new(const ujs_config *cfg)
{
    ujs_config c;
    ujs_vm *vm;
    memset(&c, 0, sizeof c);
    if (cfg) c = *cfg;
    vm = (ujs_vm *)(c.alloc ? c.alloc(c.alloc_user, sizeof *vm) : calloc(1, sizeof *vm));
    if (!vm) return NULL;
    memset(vm, 0, sizeof *vm);
    vm->cfg = c;
    vm->heap_max = c.heap_max ? c.heap_max : (8u << 20);
    vm->gc_threshold = 256 * 1024;
    if (vm->gc_threshold > vm->heap_max) vm->gc_threshold = vm->heap_max;
    vm->fuel_slice = c.fuel_per_slice;
    vm->fuel_total = c.fuel_total;
    vm->exception = ujs_undefined();

    vm->stack  = (ujs_val *)ujs_alloc_raw(vm, UJS_STACK_SIZE * sizeof(ujs_val));
    vm->frames = (ujs_frame *)ujs_alloc_raw(vm, UJS_MAX_FRAMES * sizeof(ujs_frame));
    if (!vm->stack || !vm->frames) { ujs_free(vm); return NULL; }

    /* Bootstrap: the prototype objects must exist before anything that would
     * reference them, and no GC may run while the graph is half-built. */
    vm->gc_disabled = 1;
    vm->obj_proto  = ujs_obj_new(vm, C_PLAIN, NULL);
    vm->fun_proto  = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
    vm->arr_proto  = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
    vm->str_proto  = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
    vm->num_proto  = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
    vm->bool_proto = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
    vm->err_proto  = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
    vm->global     = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
    if (!vm->global) { vm->gc_disabled = 0; ujs_free(vm); return NULL; }
    ujs_lib_init(vm);
    vm->gc_disabled = 0;
    return vm;
}

void ujs_free(ujs_vm *vm)
{
    ujs_hdr *h, *n;
    if (!vm) return;
    /* drop every root so the sweep below reclaims the whole graph */
    vm->global = NULL; vm->obj_proto = NULL; vm->fun_proto = NULL;
    vm->arr_proto = NULL; vm->str_proto = NULL; vm->num_proto = NULL;
    vm->bool_proto = NULL; vm->err_proto = NULL;
    vm->nroots = 0; vm->nscope = 0; vm->sp = 0; vm->nframes = 0;
    vm->has_exception = 0; vm->natoms = 0;
    for (h = vm->objects; h; h = n) {
        n = h->next;
        if (h->type == H_OBJ) {
            ujs_obj *o = (ujs_obj *)h;
            if (o->cls == C_HOST && o->u.host.fin) o->u.host.fin(o->u.host.user);
            if (o->props) ujs_free_raw(vm, o->props, (size_t)o->propcap * sizeof *o->props);
            if (o->elems) ujs_free_raw(vm, o->elems, (size_t)o->elemcap * sizeof *o->elems);
        } else if (h->type == H_CODE) {
            ujs_code *c = (ujs_code *)h;
            if (c->bc)     ujs_free_raw(vm, c->bc, c->nbc);
            if (c->consts) ujs_free_raw(vm, c->consts, (size_t)c->nconsts * sizeof *c->consts);
            if (c->lines)  ujs_free_raw(vm, c->lines, (size_t)c->nlines * sizeof *c->lines);
        }
        ujs_free_raw(vm, h, h->size);
    }
    if (vm->atoms)      ujs_free_raw(vm, vm->atoms, (size_t)vm->atomcap * sizeof *vm->atoms);
    if (vm->atom_hash)  ujs_free_raw(vm, vm->atom_hash, (size_t)vm->atom_hashcap * sizeof *vm->atom_hash);
    if (vm->scope_vals) ujs_free_raw(vm, vm->scope_vals, (size_t)vm->scopecap * sizeof *vm->scope_vals);
    if (vm->handlers)   ujs_free_raw(vm, vm->handlers, (size_t)vm->handlercap * sizeof *vm->handlers);
    if (vm->stack)      ujs_free_raw(vm, vm->stack, UJS_STACK_SIZE * sizeof(ujs_val));
    if (vm->frames)     ujs_free_raw(vm, vm->frames, UJS_MAX_FRAMES * sizeof(ujs_frame));
    if (vm->cfg.free) vm->cfg.free(vm->cfg.alloc_user, vm);
    else free(vm);
}
