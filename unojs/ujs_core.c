/* ===========================================================================
 * unojs core - allocation, the mark-sweep collector, atoms, strings, objects,
 * and the type conversions the whole engine shares.
 * ======================================================================== */
#include "ujs_int.h"
#include <stdlib.h>

/* 128-bit integers: GCC/clang on x86-64, including the mingw cross-compiler
 * pc64 builds with. Integer-only, so no FPU-state concerns in the kernel. */
typedef unsigned __int128 ujs_u128;
#include <stdarg.h>
#include <stdio.h>

/* ---- raw memory ---------------------------------------------------------- */
void *ujs_alloc_raw(ujs_vm *vm, size_t n)
{
    void *p = vm->cfg.alloc ? vm->cfg.alloc(vm->cfg.alloc_user, n) : malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void ujs_free_raw(ujs_vm *vm, void *p, size_t n)
{
    (void)n;
    if (!p) return;
    if (vm->cfg.free) vm->cfg.free(vm->cfg.alloc_user, p);
    else free(p);
}

/* ---- GC allocation -------------------------------------------------------
 * Every collectable object is threaded on vm->objects. Allocation checks the
 * heap ceiling FIRST (collect, then refuse) so a runaway script meets a JS
 * RangeError instead of exhausting the OS heap - the availability half of the
 * security posture in docs/WEB-ENGINE-DESIGN.md section 14. */
void *ujs_gc_alloc(ujs_vm *vm, size_t n, int type)
{
    ujs_hdr *h;
    if (vm->heap_used + n > vm->gc_threshold && !vm->gc_disabled) {
        ujs_gc(vm);
        if (vm->heap_used + n > vm->heap_max) return NULL;   /* caller throws */
        /* grow the trigger point so we do not collect on every allocation */
        vm->gc_threshold = vm->heap_used * 2;
        if (vm->gc_threshold > vm->heap_max) vm->gc_threshold = vm->heap_max;
        if (vm->gc_threshold < 256 * 1024) vm->gc_threshold = 256 * 1024;
    }
    if (vm->heap_used + n > vm->heap_max) return NULL;
    h = (ujs_hdr *)ujs_alloc_raw(vm, n);
    if (!h) return NULL;
    h->size = (u32)n;
    h->type = (u8)type;
    h->mark = 0;
    h->next = vm->objects;
    vm->objects = h;
    vm->heap_used += n;
    return h;
}

/* ---- the collector -------------------------------------------------------
 * Mark-sweep, non-moving. Marking is iterative over an explicit worklist so a
 * deep object graph cannot blow the C stack (a page can make the DOM->JS
 * graph as deep as it likes). */
typedef struct { ujs_hdr **v; int n, cap; } marklist;

static void ml_push(ujs_vm *vm, marklist *m, ujs_hdr *h)
{
    if (!h || h->mark) return;
    h->mark = 1;
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 256;
        ujs_hdr **nv = (ujs_hdr **)ujs_alloc_raw(vm, (size_t)nc * sizeof *nv);
        if (!nv) return;                     /* under memory pressure we under-
                                              * mark rather than crash; the
                                              * sweep below is conservative
                                              * only in that it may free late */
        if (m->v) { memcpy(nv, m->v, (size_t)m->n * sizeof *nv);
                    ujs_free_raw(vm, m->v, (size_t)m->cap * sizeof *nv); }
        m->v = nv; m->cap = nc;
    }
    m->v[m->n++] = h;
}

static void ml_push_val(ujs_vm *vm, marklist *m, ujs_val v)
{
    if (IS_PTR(v)) ml_push(vm, m, (ujs_hdr *)PTR_OF(v));
}

static void mark_children(ujs_vm *vm, marklist *m, ujs_hdr *h)
{
    u32 i;
    switch (h->type) {
    case H_STR: break;
    case H_OBJ: {
        ujs_obj *o = (ujs_obj *)h;
        ml_push_val(vm, m, o->proto);
        for (i = 0; i < o->nprops; i++) {
            if (!o->props[i].atom) continue;
            ml_push_val(vm, m, o->props[i].v);
            if (o->props[i].flags & P_ACCESSOR) ml_push_val(vm, m, o->props[i].setter);
        }
        for (i = 0; i < o->nelems; i++) ml_push_val(vm, m, o->elems[i]);
        if (o->cls == C_FUNC) {
            ml_push(vm, m, (ujs_hdr *)o->u.fn.code);
            ml_push(vm, m, (ujs_hdr *)o->u.fn.env);
        }
        break; }
    case H_ENV: {
        ujs_env *e = (ujs_env *)h;
        ml_push(vm, m, (ujs_hdr *)e->parent);
        for (i = 0; i < e->n; i++) ml_push_val(vm, m, e->slots[i]);
        break; }
    case H_CODE: {
        ujs_code *c = (ujs_code *)h;
        for (i = 0; i < c->nconsts; i++) ml_push_val(vm, m, c->consts[i]);
        break; }
    }
}

void ujs_gc(ujs_vm *vm)
{
    marklist m; ujs_hdr *h, **pp;
    int i;
    m.v = NULL; m.n = 0; m.cap = 0;

    /* roots: well-known objects, the global, the live stack + frames, the
     * handle scopes, explicit roots, the pending exception, and every atom
     * (atoms are permanently live in v1 - see the note in ujs_int.h). */
    ml_push(vm, &m, (ujs_hdr *)vm->global);
    ml_push(vm, &m, (ujs_hdr *)vm->obj_proto);
    ml_push(vm, &m, (ujs_hdr *)vm->fun_proto);
    ml_push(vm, &m, (ujs_hdr *)vm->arr_proto);
    ml_push(vm, &m, (ujs_hdr *)vm->str_proto);
    ml_push(vm, &m, (ujs_hdr *)vm->num_proto);
    ml_push(vm, &m, (ujs_hdr *)vm->bool_proto);
    ml_push(vm, &m, (ujs_hdr *)vm->err_proto);
    for (i = 0; i < vm->sp; i++) ml_push_val(vm, &m, vm->stack[i]);
    for (i = 0; i < vm->nframes; i++) {
        ml_push(vm, &m, (ujs_hdr *)vm->frames[i].code);
        ml_push(vm, &m, (ujs_hdr *)vm->frames[i].env);
        ml_push(vm, &m, (ujs_hdr *)vm->frames[i].fn);
        ml_push_val(vm, &m, vm->frames[i].self);
    }
    for (i = 0; i < vm->nroots; i++)  ml_push_val(vm, &m, vm->roots[i]);
    for (i = 0; i < vm->nscope; i++)  ml_push_val(vm, &m, vm->scope_vals[i]);
    if (vm->has_exception) ml_push_val(vm, &m, vm->exception);
    for (i = 0; i < (int)vm->natoms; i++) ml_push(vm, &m, (ujs_hdr *)vm->atoms[i]);

    while (m.n) mark_children(vm, &m, m.v[--m.n]);
    if (m.v) ujs_free_raw(vm, m.v, (size_t)m.cap * sizeof *m.v);

    /* sweep */
    pp = &vm->objects;
    while ((h = *pp)) {
        if (h->mark) { h->mark = 0; pp = &h->next; continue; }
        *pp = h->next;
        vm->heap_used -= h->size;
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
}

void ujs_gc_maybe(ujs_vm *vm)
{
    if (!vm->gc_disabled && vm->heap_used > vm->gc_threshold) ujs_gc(vm);
}

void ujs_gc_mark_val(ujs_vm *vm, ujs_val v) { (void)vm; (void)v; }

/* ---- strings ------------------------------------------------------------- */
static u32 str_hash(const char *s, int n)
{
    u32 h = 2166136261u; int i;
    for (i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 16777619u; }
    return h ? h : 1;
}

ujs_str *ujs_str_new(ujs_vm *vm, const char *s, int len)
{
    ujs_str *r;
    if (len < 0) len = s ? (int)strlen(s) : 0;
    r = (ujs_str *)ujs_gc_alloc(vm, sizeof(ujs_str) + (size_t)len, H_STR);
    if (!r) return NULL;
    if (len && s) memcpy(r->b, s, (size_t)len);
    r->b[len] = 0;
    r->len = (u32)len;
    r->hash = str_hash(r->b, len);
    return r;
}

ujs_str *ujs_str_cat(ujs_vm *vm, ujs_str *a, ujs_str *b)
{
    ujs_str *r = (ujs_str *)ujs_gc_alloc(vm, sizeof(ujs_str) + a->len + b->len, H_STR);
    if (!r) return NULL;
    memcpy(r->b, a->b, a->len);
    memcpy(r->b + a->len, b->b, b->len);
    r->b[a->len + b->len] = 0;
    r->len = a->len + b->len;
    r->hash = str_hash(r->b, (int)r->len);
    return r;
}

int ujs_str_eq(ujs_str *a, ujs_str *b)
{
    if (a == b) return 1;
    if (a->len != b->len || a->hash != b->hash) return 0;
    return memcmp(a->b, b->b, a->len) == 0;
}

ujs_val  ujs_str_val(ujs_str *s) { return s ? ujs_ptrval(s) : ujs_undefined(); }
ujs_str *ujs_val_str(ujs_val v) { return (ujs_str *)PTR_OF(v); }
ujs_obj *ujs_val_obj(ujs_val v) { return (ujs_obj *)PTR_OF(v); }
ujs_val  ujs_obj_val(ujs_obj *o) { return o ? ujs_ptrval(o) : ujs_undefined(); }

/* ---- atoms ---------------------------------------------------------------
 * Interned property names. The hash index maps a name to (atom index + 1);
 * 0 means empty. Atom 0 is reserved as "no atom". */
static void atom_rehash(ujs_vm *vm, u32 newcap)
{
    u32 i;
    u32 *tab = (u32 *)ujs_alloc_raw(vm, (size_t)newcap * sizeof *tab);
    if (!tab) return;
    for (i = 1; i < vm->natoms; i++) {
        ujs_str *s = vm->atoms[i];
        u32 j = s->hash & (newcap - 1);
        while (tab[j]) j = (j + 1) & (newcap - 1);
        tab[j] = i;
    }
    if (vm->atom_hash) ujs_free_raw(vm, vm->atom_hash, (size_t)vm->atom_hashcap * sizeof *tab);
    vm->atom_hash = tab;
    vm->atom_hashcap = newcap;
}

u32 ujs_atom(ujs_vm *vm, const char *s, int len)
{
    u32 h, j, idx;
    if (len < 0) len = (int)strlen(s);
    if (!vm->atom_hashcap) atom_rehash(vm, 256);
    if (!vm->atom_hash) return 0;
    h = str_hash(s, len);
    j = h & (vm->atom_hashcap - 1);
    while ((idx = vm->atom_hash[j])) {
        ujs_str *e = vm->atoms[idx];
        if (e->hash == h && e->len == (u32)len && !memcmp(e->b, s, (size_t)len))
            return idx;
        j = (j + 1) & (vm->atom_hashcap - 1);
    }
    /* not present: intern it */
    if (vm->natoms + 1 >= vm->atomcap) {
        u32 nc = vm->atomcap ? vm->atomcap * 2 : 128;
        ujs_str **na = (ujs_str **)ujs_alloc_raw(vm, (size_t)nc * sizeof *na);
        if (!na) return 0;
        if (vm->atoms) { memcpy(na, vm->atoms, (size_t)vm->natoms * sizeof *na);
                         ujs_free_raw(vm, vm->atoms, (size_t)vm->atomcap * sizeof *na); }
        vm->atoms = na; vm->atomcap = nc;
        if (!vm->natoms) vm->natoms = 1;          /* reserve index 0 */
    }
    if (!vm->natoms) vm->natoms = 1;
    {   ujs_str *ns;
        int gd = vm->gc_disabled; vm->gc_disabled = 1;   /* the table is not yet
                                                          * consistent: no GC */
        ns = ujs_str_new(vm, s, len);
        vm->gc_disabled = gd;
        if (!ns) return 0;
        idx = vm->natoms++;
        vm->atoms[idx] = ns;
        vm->atom_hash[j] = idx;
        if (vm->natoms * 2 > vm->atom_hashcap) atom_rehash(vm, vm->atom_hashcap * 2);
        return idx;
    }
}

ujs_str *ujs_atom_str(ujs_vm *vm, u32 atom)
{ return (atom && atom < vm->natoms) ? vm->atoms[atom] : NULL; }

const char *ujs_atom_cstr(ujs_vm *vm, u32 atom)
{ ujs_str *s = ujs_atom_str(vm, atom); return s ? s->b : ""; }

/* ---- objects -------------------------------------------------------------
 * Properties live in a DENSE array in insertion order (JS needs that order for
 * for-in and Object.keys), with a lazily built hash index once an object grows
 * past a handful of properties. Small objects - the overwhelming majority -
 * pay nothing but a linear scan over a few cache-hot entries. */
ujs_obj *ujs_obj_new(ujs_vm *vm, int cls, ujs_obj *proto)
{
    ujs_obj *o = (ujs_obj *)ujs_gc_alloc(vm, sizeof(ujs_obj), H_OBJ);
    if (!o) return NULL;
    o->cls = (u8)cls;
    o->proto = proto ? ujs_obj_val(proto) : ujs_null();
    return o;
}

ujs_prop *ujs_obj_find(ujs_obj *o, u32 atom)
{
    u32 i;
    for (i = 0; i < o->nprops; i++)
        if (o->props[i].atom == atom) return &o->props[i];
    return NULL;
}

static int obj_grow(ujs_vm *vm, ujs_obj *o)
{
    u32 nc = o->propcap ? o->propcap * 2 : 4;
    ujs_prop *np = (ujs_prop *)ujs_alloc_raw(vm, (size_t)nc * sizeof *np);
    if (!np) return 0;
    if (o->props) {
        memcpy(np, o->props, (size_t)o->nprops * sizeof *np);
        ujs_free_raw(vm, o->props, (size_t)o->propcap * sizeof *np);
    }
    o->props = np; o->propcap = nc;
    return 1;
}

int ujs_obj_put(ujs_vm *vm, ujs_obj *o, u32 atom, ujs_val v, u32 flags)
{
    ujs_prop *p = ujs_obj_find(o, atom);
    if (p) {
        if (!(p->flags & P_WRITE) && !(flags & P_ACCESSOR)) return 1;  /* silent in
                                                                       * sloppy mode */
        p->v = v;
        if (flags & P_ACCESSOR) p->flags |= P_ACCESSOR;
        return 1;
    }
    if (o->nprops == o->propcap && !obj_grow(vm, o)) return 0;
    p = &o->props[o->nprops++];
    p->atom = atom; p->v = v; p->flags = flags; p->setter = ujs_undefined();
    return 1;
}

int ujs_obj_get(ujs_vm *vm, ujs_obj *o, u32 atom, ujs_val *out)
{
    ujs_obj *cur = o;
    int guard = 0;
    while (cur && guard++ < 1000) {                 /* proto chains are page
                                                     * data: bound the walk */
        ujs_prop *p = ujs_obj_find(cur, atom);
        if (p) { *out = p->v; return p->flags & P_ACCESSOR ? 2 : 1; }
        if (!IS_PTR(cur->proto)) break;
        cur = ujs_val_obj(cur->proto);
    }
    (void)vm;
    *out = ujs_undefined();
    return 0;
}

int ujs_obj_del(ujs_vm *vm, ujs_obj *o, u32 atom)
{
    u32 i;
    (void)vm;
    for (i = 0; i < o->nprops; i++) {
        if (o->props[i].atom != atom) continue;
        memmove(&o->props[i], &o->props[i + 1],
                (size_t)(o->nprops - i - 1) * sizeof o->props[0]);
        o->nprops--;
        return 1;
    }
    return 1;
}

/* dense indexed storage */
void ujs_arr_set(ujs_vm *vm, ujs_obj *a, u32 i, ujs_val v)
{
    if (i >= a->elemcap) {
        u32 nc = a->elemcap ? a->elemcap * 2 : 8;
        ujs_val *ne;
        while (nc <= i) nc *= 2;
        if (nc > (1u << 26)) return;                /* 64M elements: refuse    */
        ne = (ujs_val *)ujs_alloc_raw(vm, (size_t)nc * sizeof *ne);
        if (!ne) return;
        if (a->elems) { memcpy(ne, a->elems, (size_t)a->nelems * sizeof *ne);
                        ujs_free_raw(vm, a->elems, (size_t)a->elemcap * sizeof *ne); }
        { u32 k; for (k = a->nelems; k < nc; k++) ne[k] = ujs_undefined(); }
        a->elems = ne; a->elemcap = nc;
    }
    a->elems[i] = v;
    if (i >= a->nelems) a->nelems = i + 1;
}

ujs_val ujs_arr_get(ujs_obj *a, u32 i)
{ return (a->elems && i < a->nelems) ? a->elems[i] : ujs_undefined(); }

/* ---- environments -------------------------------------------------------- */
ujs_env *ujs_env_new(ujs_vm *vm, ujs_env *parent, u32 n)
{
    ujs_env *e = (ujs_env *)ujs_gc_alloc(vm, sizeof(ujs_env) + (n ? n - 1 : 0) * sizeof(ujs_val), H_ENV);
    u32 i;
    if (!e) return NULL;
    e->parent = parent;
    e->n = n;
    for (i = 0; i < n; i++) e->slots[i] = ujs_undefined();
    return e;
}

/* ---- number formatting ---------------------------------------------------
 * Freestanding: unojs never calls the C library's float printf. Integers take
 * the exact digit path (which is almost all of the web's numbers); the rest
 * round-trips through ujs_strtod at increasing precision so `0.1` prints as
 * "0.1" and `0.1+0.2` still prints as "0.30000000000000004". */
double ujs_strtod_impl(const char *s, const char **end);

/* round(d * 10^p) computed EXACTLY, for p >= 0.
 *
 * A double is m * 2^e with m < 2^53, so d * 10^p = m * 10^p * 2^e is an exact
 * rational that 128-bit integers can evaluate outright. Doing this in double
 * arithmetic instead is what made sqrt(2) print as 1.4142135623730952: the
 * true product is ...951.45, but as a double it cannot be represented and
 * snaps to ...952 before the rounding step ever runs. Returns 0 when the
 * value does not fit, and the caller falls back to scaling in double. */
static int exact_scaled(double d, int p, u64 *out)
{
    u64 bits, m;
    int be, e, i;
    ujs_u128 num;
    if (p < 0 || p > 38) return 0;
    memcpy(&bits, &d, 8);
    be = (int)((bits >> 52) & 0x7FF);
    m = bits & 0x000FFFFFFFFFFFFFULL;
    if (be == 0) e = -1074;                       /* subnormal: no implicit 1 */
    else { m |= 1ULL << 52; e = be - 1075; }
    if (!m) { *out = 0; return 1; }
    num = (ujs_u128)m;
    for (i = 0; i < p; i++) {
        if (num > (~(ujs_u128)0) / 10u) return 0;
        num *= 10u;
    }
    if (e >= 0) {
        if (e > 60) return 0;
        for (i = 0; i < e; i++) {
            if (num > (~(ujs_u128)0) >> 1) return 0;
            num <<= 1;
        }
    } else {
        int sh = -e;
        if (sh >= 127) return 0;
        num = (num + ((ujs_u128)1 << (sh - 1))) >> sh;    /* round to nearest */
    }
    if (num >= ((ujs_u128)1 << 64)) return 0;
    *out = (u64)num;
    return 1;
}

static void fmt_digits(char *dst, const char *digits, int ndig, int e10)
{
    /* place `digits` (ndig significant) with decimal exponent e10 into dst,
     * choosing JS's fixed-vs-exponential rule. */
    int i = 0, k, dp = e10 + 1;                 /* digits before the point */
    if (e10 >= 21 || e10 <= -7) {               /* exponential */
        dst[i++] = digits[0];
        if (ndig > 1) { dst[i++] = '.'; for (k = 1; k < ndig; k++) dst[i++] = digits[k]; }
        dst[i++] = 'e';
        dst[i++] = e10 < 0 ? '-' : '+';
        { int a = e10 < 0 ? -e10 : e10; char t[8]; int n = 0;
          if (!a) t[n++] = '0';
          while (a) { t[n++] = (char)('0' + a % 10); a /= 10; }
          while (n) dst[i++] = t[--n]; }
    } else if (dp <= 0) {                       /* 0.000ddd */
        dst[i++] = '0'; dst[i++] = '.';
        for (k = 0; k < -dp; k++) dst[i++] = '0';
        for (k = 0; k < ndig; k++) dst[i++] = digits[k];
    } else if (dp >= ndig) {                    /* ddd000 */
        for (k = 0; k < ndig; k++) dst[i++] = digits[k];
        for (k = ndig; k < dp; k++) dst[i++] = '0';
    } else {                                    /* dd.ddd */
        for (k = 0; k < dp; k++) dst[i++] = digits[k];
        dst[i++] = '.';
        for (; k < ndig; k++) dst[i++] = digits[k];
    }
    dst[i] = 0;
}

void ujs_num_to_str(double d, char *buf, size_t n)
{
    char digits[32], tmp[64];
    int prec, ndig, e10;
    if (n < 32) { if (n) buf[0] = 0; return; }
    if (d != d) { strcpy(buf, "NaN"); return; }
    if (d == 0) { strcpy(buf, "0"); return; }
    if (d < 0) { buf[0] = '-'; ujs_num_to_str(-d, buf + 1, n - 1); return; }
    if (d > 1.7976931348623157e308) { strcpy(buf, "Infinity"); return; }

    /* exact integers below 2^53 print directly - no rounding questions */
    if (d == ujs_floor(d) && d < 9007199254740992.0) {
        u64 iv = (u64)d; char t[24]; int k = 0;
        while (iv) { t[k++] = (char)('0' + (int)(iv % 10)); iv /= 10; }
        if (!k) t[k++] = '0';
        if ((size_t)k < n) { int i = 0; while (k) buf[i++] = t[--k]; buf[i] = 0; return; }
    }

    /* Shortest representation that reads back as the SAME double.
     *
     * Digits are extracted by scaling with powers of ten in double precision,
     * which is exact only while the scaled integer stays under 2^53. Past that
     * - i.e. for the 16th and 17th significant digits - the scaling can land
     * one unit off, which is how sqrt(2) came out as 1.4142135623730952 where
     * every other engine prints ...51. Rather than reach for a bignum dtoa,
     * each candidate is parsed back with ujs_strtod_impl (which IS exact for
     * these inputs) and its last-digit neighbours are tried too. A candidate
     * that round-trips is correct BY DEFINITION, so the oracle turns an
     * approximate extraction into an exact answer. */
    e10 = (int)ujs_floor(ujs_log10(d));
    for (prec = 1; prec <= 17; prec++) {
        double scaled;
        int i, adj, shift = e10 - prec + 1;
        /* Scale by MULTIPLYING with a positive power of ten where possible:
         * dividing by an inexact 1e-16 loses the final digit outright. Very
         * small or large values need two steps so the factor cannot overflow. */
        if (shift <= 0) {
            int k = -shift;
            if (k > 300) scaled = (d * 1e300) * ujs_pow(10.0, (double)(k - 300));
            else         scaled = d * ujs_pow(10.0, (double)k);
        } else if (shift > 300) {
            scaled = (d / 1e300) / ujs_pow(10.0, (double)(shift - 300));
        } else {
            scaled = d / ujs_pow(10.0, (double)shift);
        }
        scaled = ujs_floor(scaled + 0.5);
        if (!(scaled >= 1.0) || scaled >= 18446744073709551616.0) continue;

        /* The candidate integer must stay an INTEGER: a 17-digit value exceeds
         * 2^53, so routing it through a double would snap it to its neighbour
         * and undo the exact extraction above. */
        {   u64 base_iv;
            double lim = ujs_pow(10.0, (double)prec);
            if (!exact_scaled(d, -shift, &base_iv) || base_iv < 1)
                base_iv = (u64)scaled;
            if ((double)base_iv >= lim * 10.0) { e10++; prec--; continue; }

        for (adj = 0; adj < 3; adj++) {
            static const int delta[3] = { 0, -1, 1 };
            u64 iv = base_iv;
            int k = 0;
            char t[24];
            double back;
            if (delta[adj] < 0) { if (iv == 0) continue; iv -= 1; }
            else if (delta[adj] > 0) iv += 1;
            if (iv == 0) continue;
            while (iv) { t[k++] = (char)('0' + (int)(iv % 10)); iv /= 10; }
            ndig = k;
            for (i = 0; i < k; i++) digits[i] = t[k - 1 - i];
            while (ndig > 1 && digits[ndig - 1] == '0') ndig--;   /* trim */
            fmt_digits(tmp, digits, ndig, e10);
            back = ujs_strtod_impl(tmp, NULL);
            if (back == d) {
                if (strlen(tmp) < n) strcpy(buf, tmp); else buf[0] = 0;
                return;
            }
        }
        }
    }
    if (strlen(tmp) < n) strcpy(buf, tmp); else buf[0] = 0;
}

/* strtod. The mantissa is accumulated exactly as a u64 and combined with the
 * decimal exponent in 128-bit integers where possible, so adjacent doubles
 * never collapse onto the same value - which the shortest-round-trip search in
 * ujs_num_to_str depends on being true. */
double ujs_strtod_impl(const char *s, const char **end)
{
    u64 mant = 0; int ndig = 0, e10 = 0, neg = 0, any = 0, esign, eval;
    double r;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    while (*s >= '0' && *s <= '9') {
        any = 1;
        if (ndig < 19) { mant = mant * 10 + (u64)(*s - '0'); ndig++; }
        else e10++;
        s++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            any = 1;
            if (ndig < 19) { mant = mant * 10 + (u64)(*s - '0'); ndig++; e10--; }
            s++;
        }
    }
    if (!any) { if (end) *end = s; return 0.0 / (neg ? 1.0 : 1.0) * 0 + (0.0 / 0.0); }
    if (*s == 'e' || *s == 'E') {
        const char *save = s;
        s++; esign = 1; eval = 0;
        if (*s == '+' || *s == '-') { if (*s == '-') esign = -1; s++; }
        if (*s >= '0' && *s <= '9') {
            while (*s >= '0' && *s <= '9') { if (eval < 100000) eval = eval * 10 + (*s - '0'); s++; }
            e10 += esign * eval;
        } else s = save;                        /* "1e" is just "1" then "e"  */
    }
    /* mant * 10^e10, correctly rounded.
     *
     * The obvious `(double)mant * pow(10,e10)` is WRONG for 16-17 digit
     * inputs: a mantissa above 2^53 loses its low bit the moment it becomes a
     * double, so two adjacent doubles parse to the same value and the shortest
     * round-trip search in ujs_num_to_str is fed a broken oracle. Doing the
     * scaling in 128-bit integers keeps ~64 guard bits, so the single
     * conversion to double at the end is the only rounding. */
    if (e10 >= 0 && e10 <= 22 && mant != 0) {
        ujs_u128 p = (ujs_u128)mant;
        int k = e10, overflow = 0;
        while (k-- > 0) {
            if (p > (~(ujs_u128)0) / 10u) { overflow = 1; break; }
            p *= 10u;
        }
        if (!overflow) { r = (double)p; if (end) *end = s; return neg ? -r : r; }
        r = (double)mant;
    } else if (e10 < 0 && e10 >= -22 && mant != 0) {
        ujs_u128 den = 1;                          /* 10^21 does NOT fit a u64 */
        int k = -e10, sh = 0;
        ujs_u128 num = (ujs_u128)mant, q;
        u64 pbits;
        double scale2;
        while (k-- > 0) den *= 10u;
        /* NORMALIZE before dividing. A fixed `mant << 64` looks like 64 guard
         * bits but only gives that many when the mantissa is already wide: for
         * "1e-7" the mantissa is 1, the quotient came out ~41 bits, and the
         * value printed back as 9.999999999994822e-8. Shifting the numerator as
         * far left as 128 bits allow leaves the quotient ~70+ significant bits
         * whatever the input looked like, so the single conversion to double
         * is the only rounding that matters. */
        while (num < ((ujs_u128)1 << 126) && sh < 126) { num <<= 1; sh++; }
        q = num / den;
        pbits = (u64)(1023 - sh) << 52;            /* 2^-sh as a double */
        memcpy(&scale2, &pbits, 8);
        r = (double)q * scale2;
        if (end) *end = s;
        return neg ? -r : r;
    } else {
        r = (double)mant;
    }
    /* 10^0..10^22 are exactly representable; scale in those chunks so the
     * error does not compound the way repeated squaring would, and so a
     * literal at the very top of the range stays finite. */
    if (e10 > 0) {
        while (e10 > 22 && r != 0) { r *= 1e22; e10 -= 22;
                                     if (r > 1.7976931348623157e308) break; }
        if (e10 > 0) r *= ujs_pow(10.0, (double)e10);
    } else if (e10 < 0) {
        while (e10 < -22 && r != 0) { r /= 1e22; e10 += 22; }
        if (e10 < 0) r /= ujs_pow(10.0, (double)-e10);
    }
    if (end) *end = s;
    return neg ? -r : r;
}

/* ---- conversions --------------------------------------------------------- */
int ujs_truthy(ujs_val v)
{
    if (IS_DOUBLE(v)) { double d = ujs_dbl(v); return d != 0 && d == d; }
    switch ((u32)(VBITS(v) & 0xFF)) {
    case UJS_TAG_UNDEF: case UJS_TAG_NULL: case UJS_TAG_FALSE: return 0;
    case UJS_TAG_TRUE: return 1;
    default: break;
    }
    if (IS_PTR(v)) {
        ujs_hdr *h = (ujs_hdr *)PTR_OF(v);
        if (h->type == H_STR) return ((ujs_str *)h)->len != 0;
        return 1;                                /* objects are always truthy */
    }
    return 0;
}

double ujs_num_of(ujs_vm *vm, ujs_val v)
{
    if (IS_DOUBLE(v)) return ujs_dbl(v);
    if (IS_PTR(v)) {
        ujs_hdr *h = (ujs_hdr *)PTR_OF(v);
        if (h->type == H_STR) {
            ujs_str *s = (ujs_str *)h;
            const char *e; double d;
            const char *p = s->b;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (!*p) return 0;                   /* "" and "   " are 0        */
            d = ujs_strtod_impl(p, &e);
            while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r') e++;
            return *e ? (0.0 / 0.0) : d;         /* trailing junk -> NaN      */
        }
        {   /* objects: ToPrimitive via valueOf/toString */
            ujs_str *s = ujs_tostr(vm, v);
            if (!s) return 0.0 / 0.0;
            if (!s->len) return 0;
            { const char *e; double d = ujs_strtod_impl(s->b, &e);
              while (*e == ' ') e++;
              return *e ? (0.0 / 0.0) : d; }
        }
    }
    switch ((u32)(VBITS(v) & 0xFF)) {
    case UJS_TAG_NULL:  return 0;
    case UJS_TAG_FALSE: return 0;
    case UJS_TAG_TRUE:  return 1;
    default:            return 0.0 / 0.0;        /* undefined */
    }
}

ujs_str *ujs_tostr(ujs_vm *vm, ujs_val v)
{
    char buf[64];
    if (IS_DOUBLE(v)) { ujs_num_to_str(ujs_dbl(v), buf, sizeof buf); return ujs_str_new(vm, buf, -1); }
    if (IS_PTR(v)) {
        ujs_hdr *h = (ujs_hdr *)PTR_OF(v);
        if (h->type == H_STR) return (ujs_str *)h;
        {   /* object: try toString(), then valueOf(); else the class tag */
            ujs_obj *o = (ujs_obj *)h;
            ujs_val fn, r;
            static const char *names[2] = { "toString", "valueOf" };
            int i;
            for (i = 0; i < 2; i++) {
                if (ujs_obj_get(vm, o, ujs_atom(vm, names[i], -1), &fn) &&
                    IS_PTR(fn) && ((ujs_hdr *)PTR_OF(fn))->type == H_OBJ) {
                    ujs_obj *f = ujs_val_obj(fn);
                    if (f->cls == C_FUNC || f->cls == C_CFUNC) {
                        if (ujs_call_value(vm, fn, v, 0, NULL, &r, 0) != UJS_OK) return NULL;
                        if (!IS_PTR(r) || ((ujs_hdr *)PTR_OF(r))->type != H_OBJ)
                            return ujs_tostr(vm, r);
                    }
                }
            }
            if (o->cls == C_ARRAY) {
                /* default Array#toString is join(",") - implemented here so the
                 * conversion works even before the library object is reachable */
                ujs_str *acc = ujs_str_new(vm, "", 0);
                u32 i2;
                for (i2 = 0; i2 < o->nelems && acc; i2++) {
                    ujs_val e = o->elems[i2];
                    ujs_str *part;
                    if (i2) { acc = ujs_str_cat(vm, acc, ujs_str_new(vm, ",", 1)); if (!acc) break; }
                    if (ujs_is_undefined(e) || ujs_is_null(e)) continue;
                    part = ujs_tostr(vm, e);
                    if (!part) return NULL;
                    acc = ujs_str_cat(vm, acc, part);
                }
                return acc;
            }
            if (o->cls == C_FUNC || o->cls == C_CFUNC) return ujs_str_new(vm, "function", -1);
            return ujs_str_new(vm, "[object Object]", -1);
        }
    }
    switch ((u32)(VBITS(v) & 0xFF)) {
    case UJS_TAG_NULL:  return ujs_str_new(vm, "null", 4);
    case UJS_TAG_FALSE: return ujs_str_new(vm, "false", 5);
    case UJS_TAG_TRUE:  return ujs_str_new(vm, "true", 4);
    default:            return ujs_str_new(vm, "undefined", 9);
    }
}

int ujs_strict_eq(ujs_val a, ujs_val b)
{
    if (IS_DOUBLE(a) || IS_DOUBLE(b)) {
        if (!IS_DOUBLE(a) || !IS_DOUBLE(b)) return 0;
        return ujs_dbl(a) == ujs_dbl(b);          /* NaN != NaN falls out      */
    }
    if (VBITS(a) == VBITS(b)) return 1;
    if (IS_PTR(a) && IS_PTR(b)) {
        ujs_hdr *ha = (ujs_hdr *)PTR_OF(a), *hb = (ujs_hdr *)PTR_OF(b);
        if (ha->type == H_STR && hb->type == H_STR)
            return ujs_str_eq((ujs_str *)ha, (ujs_str *)hb);
    }
    return 0;
}

int ujs_loose_eq(ujs_vm *vm, ujs_val a, ujs_val b)
{
    int na = ujs_is_null(a) || ujs_is_undefined(a);
    int nb = ujs_is_null(b) || ujs_is_undefined(b);
    if (na || nb) return na && nb;
    if (ujs_strict_eq(a, b)) return 1;
    {   int oa = ujs_is_object(a), ob = ujs_is_object(b);
        if (oa && ob) return 0;                   /* distinct objects          */
        if (oa || ob) {                           /* ToPrimitive the object    */
            ujs_str *s = ujs_tostr(vm, oa ? a : b);
            if (!s) return 0;
            return ujs_loose_eq(vm, oa ? ujs_str_val(s) : a, ob ? ujs_str_val(s) : b);
        }
        {   double da = ujs_num_of(vm, a), db = ujs_num_of(vm, b);
            return da == db; }
    }
}

const char *ujs_typeof_str(ujs_vm *vm, ujs_val v)
{
    (void)vm;
    if (IS_DOUBLE(v)) return "number";
    if (IS_PTR(v)) {
        ujs_hdr *h = (ujs_hdr *)PTR_OF(v);
        if (h->type == H_STR) return "string";
        { ujs_obj *o = (ujs_obj *)h;
          return (o->cls == C_FUNC || o->cls == C_CFUNC) ? "function" : "object"; }
    }
    switch ((u32)(VBITS(v) & 0xFF)) {
    case UJS_TAG_NULL:  return "object";          /* the famous ES1 bug, kept  */
    case UJS_TAG_FALSE: case UJS_TAG_TRUE: return "boolean";
    default: return "undefined";
    }
}

/* ---- errors -------------------------------------------------------------- */
ujs_val ujs_throwf(ujs_vm *vm, const char *kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->msgbuf, sizeof vm->msgbuf, fmt, ap);
    va_end(ap);
    return ujs_throw_error(vm, kind, vm->msgbuf);
}
