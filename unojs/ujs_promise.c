/* ===========================================================================
 * ujs_promise.c - Promises, the microtask queue, and the machinery that makes
 * `await` a real suspension rather than a shape (UCD-21).
 *
 * A PROMISE IS A PLAIN OBJECT with two hidden properties: state and value.
 * Not a new heap class, because a new class means new cases in the collector,
 * in typeof, in property lookup and in every switch that already handles the
 * five that exist.  The hidden state property is also what ujs_is_promise()
 * asks about, so the test and the storage are the same fact.
 *
 * REACTIONS LIVE IN C, on vm->reactions, rather than on the object.  A
 * reaction is EITHER a pair of JS callbacks (from .then) OR a suspended
 * coroutine (from await), and a coroutine is not expressible as a property
 * value.  One list handles both, which is why await and .then cannot drift
 * apart in their ordering.
 *
 * NOTHING RUNS EAGERLY.  Settling queues reactions as jobs; jobs run in
 * ujs_run_jobs().  That is the entire difference between a promise and a
 * callback, and the reason `p.then(f); print(1)` prints 1 first, always -
 * including when p was already resolved before .then was called.
 * ======================================================================== */
#include "ujs_int.h"

#define PROM_PENDING  0
#define PROM_OK       1
#define PROM_REJECTED 2

static u32 a_state(ujs_vm *vm) { return ujs_atom(vm, "__ps", -1); }
static u32 a_value(ujs_vm *vm) { return ujs_atom(vm, "__pv", -1); }

int ujs_is_promise(ujs_vm *vm, ujs_val v)
{
    ujs_val s;
    if (!ujs_is_object(v)) return 0;
    return ujs_obj_get(vm, ujs_val_obj(v), a_state(vm), &s);
}

ujs_val ujs_promise_new(ujs_vm *vm)
{
    ujs_obj *o = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
    if (!o) return ujs_undefined();
    /* NOT enumerable: JSON.stringify and for-in must not see the plumbing */
    ujs_obj_put(vm, o, a_state(vm), ujs_number(PROM_PENDING), P_WRITE);
    ujs_obj_put(vm, o, a_value(vm), ujs_undefined(), P_WRITE);
    return ujs_obj_val(o);
}

/* ---- the job queue -------------------------------------------------------- */
int ujs_job_push(ujs_vm *vm, ujs_val fn, ujs_val arg)
{
    if (!vm->jobs) {
        vm->jobcap = 256;
        vm->jobs = (ujs_val *)ujs_alloc_raw(vm,
                        sizeof(ujs_val) * (size_t)vm->jobcap * 2);
        if (!vm->jobs) { vm->jobcap = 0; return 0; }
    }
    if (vm->njobs >= vm->jobcap) { vm->jobs_dropped++; return 0; }
    {   int slot = (vm->jobhead + vm->njobs) % vm->jobcap;
        vm->jobs[slot * 2] = fn;
        vm->jobs[slot * 2 + 1] = arg;
        vm->njobs++;
    }
    return 1;
}

/* ---- reactions ------------------------------------------------------------ */

/* Move every reaction waiting on `p` onto the job queue.  Called when a
 * promise settles, and when a reaction is attached to one that already has. */
static void promise_flush(ujs_vm *vm, ujs_val p, ujs_val v, int rejected)
{
    ujs_reaction **pp = &vm->reactions;
    while (*pp) {
        ujs_reaction *r = *pp;
        if (!ujs_strict_eq(r->promise, p)) { pp = &r->next; continue; }
        *pp = r->next;
        if (r->co) {
            /* A suspended await.  It resumes as a JOB, never here: resuming
             * inside settle() would run the rest of an async function in the
             * middle of whatever code settled the promise. */
            ujs_obj *h = ujs_obj_new(vm, C_HOST, vm->obj_proto);
            if (h) {
                h->u.host.user = r->co;
                h->u.host.fin = 0;
                ujs_obj_put(vm, h, ujs_atom(vm, "__cv", -1), v, P_WRITE);
                ujs_obj_put(vm, h, ujs_atom(vm, "__cr", -1),
                            ujs_number(rejected ? 1 : 0), P_WRITE);
                ujs_job_push(vm, ujs_undefined(), ujs_obj_val(h));
            }
        } else {
            ujs_val cb = rejected ? r->onerr : r->onok;
            if (ujs_is_object(cb)) {
                /* the callback runs as a job, and its RESULT settles the
                 * derived promise - which is what makes .then chain */
                ujs_obj *rec = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
                if (rec) {
                    ujs_obj_put(vm, rec, ujs_atom(vm, "__jv", -1), v, P_WRITE);
                    ujs_obj_put(vm, rec, ujs_atom(vm, "__jo", -1), r->out, P_WRITE);
                    ujs_obj_put(vm, rec, ujs_atom(vm, "__jr", -1),
                                ujs_number(rejected ? 1 : 0), P_WRITE);
                    ujs_job_push(vm, cb, ujs_obj_val(rec));
                }
            } else if (ujs_is_object(r->out)) {
                /* no handler for THIS outcome: it passes straight through to
                 * the derived promise, which is how .then(f) forwards an
                 * error to a .catch further down the chain */
                ujs_promise_settle(vm, r->out, v, rejected);
            }
        }
        ujs_free_raw(vm, r, sizeof *r);
    }
}

void ujs_promise_settle(ujs_vm *vm, ujs_val p, ujs_val v, int rejected)
{
    ujs_obj *o;
    ujs_val st;
    if (!ujs_is_object(p)) return;
    o = ujs_val_obj(p);
    if (!ujs_obj_get(vm, o, a_state(vm), &st)) return;
    if ((int)ujs_num_of(vm, st) != PROM_PENDING) return;      /* settle once */

    /* Resolving WITH a promise ADOPTS it: this one settles when that one
     * does.  It is what makes `return somePromise` inside a .then behave the
     * way everybody expects instead of handing back a promise for a promise. */
    if (!rejected && ujs_is_promise(vm, v)) {
        ujs_reaction *r = (ujs_reaction *)ujs_alloc_raw(vm, sizeof *r);
        if (r) {
            memset(r, 0, sizeof *r);
            r->promise = v;
            r->onok = r->onerr = ujs_undefined();
            r->out = p;
            r->next = vm->reactions;
            vm->reactions = r;
            /* if v has ALREADY settled, nothing will flush it later */
            {   ujs_val vst, vv;
                ujs_obj_get(vm, ujs_val_obj(v), a_state(vm), &vst);
                if ((int)ujs_num_of(vm, vst) != PROM_PENDING) {
                    ujs_obj_get(vm, ujs_val_obj(v), a_value(vm), &vv);
                    promise_flush(vm, v, vv,
                                  (int)ujs_num_of(vm, vst) == PROM_REJECTED);
                }
            }
        }
        return;
    }
    ujs_obj_put(vm, o, a_state(vm),
                ujs_number(rejected ? PROM_REJECTED : PROM_OK), P_WRITE);
    ujs_obj_put(vm, o, a_value(vm), v, P_WRITE);
    promise_flush(vm, p, v, rejected);
}

static void react_add(ujs_vm *vm, ujs_val p, ujs_val onok, ujs_val onerr,
                      ujs_val out, ujs_coro *co)
{
    ujs_val st, v;
    ujs_obj *o;
    if (!ujs_is_object(p)) return;
    o = ujs_val_obj(p);
    if (!ujs_obj_get(vm, o, a_state(vm), &st)) return;
    {   ujs_reaction *r = (ujs_reaction *)ujs_alloc_raw(vm, sizeof *r);
        if (!r) return;
        memset(r, 0, sizeof *r);
        r->promise = p;
        r->onok = onok;
        r->onerr = onerr;
        r->out = out;
        r->co = co;
        r->next = vm->reactions;
        vm->reactions = r;
    }
    /* ALREADY settled: flush now - which still means "as a job".  Without
     * this, an await on a resolved promise would wait for ever. */
    if ((int)ujs_num_of(vm, st) != PROM_PENDING) {
        ujs_obj_get(vm, o, a_value(vm), &v);
        promise_flush(vm, p, v, (int)ujs_num_of(vm, st) == PROM_REJECTED);
    }
}

void ujs_promise_react(ujs_vm *vm, ujs_val p, ujs_val onok, ujs_val onerr,
                       ujs_val out)
{ react_add(vm, p, onok, onerr, out, 0); }

void ujs_promise_await(ujs_vm *vm, ujs_val p, ujs_coro *co)
{ react_add(vm, p, ujs_undefined(), ujs_undefined(), ujs_undefined(), co); }

/* ---- running the queue ---------------------------------------------------- */
int ujs_run_jobs(ujs_vm *vm)
{
    int ran = 0;
    if (!vm || vm->in_jobs) return 0;
    vm->in_jobs = 1;
    /* Bounded: a job may queue another, and a promise chain that queues
     * itself for ever must not take the machine with it.  What is left stays
     * queued for the next call. */
    while (vm->njobs > 0 && ran < 4096) {
        ujs_val fn = vm->jobs[vm->jobhead * 2];
        ujs_val arg = vm->jobs[vm->jobhead * 2 + 1];
        vm->jobhead = (vm->jobhead + 1) % vm->jobcap;
        vm->njobs--;
        ran++;

        if (ujs_is_object(arg) && ujs_val_obj(arg)->cls == C_HOST) {
            /* a coroutine resumption */
            ujs_coro *co = (ujs_coro *)ujs_val_obj(arg)->u.host.user;
            ujs_val v = ujs_undefined(), rj = ujs_undefined();
            ujs_obj_get(vm, ujs_val_obj(arg), ujs_atom(vm, "__cv", -1), &v);
            ujs_obj_get(vm, ujs_val_obj(arg), ujs_atom(vm, "__cr", -1), &rj);
            ujs_val_obj(arg)->u.host.user = 0;   /* one resume per job       */
            if (co) ujs_coro_resume(vm, co, v, (int)ujs_num_of(vm, rj) != 0);
            continue;
        }
        if (ujs_is_object(fn) && ujs_is_object(arg)) {
            ujs_val v = ujs_undefined(), out = ujs_undefined(), rj = ujs_undefined();
            ujs_val r = ujs_undefined();
            ujs_obj *rec = ujs_val_obj(arg);
            ujs_obj_get(vm, rec, ujs_atom(vm, "__jv", -1), &v);
            ujs_obj_get(vm, rec, ujs_atom(vm, "__jo", -1), &out);
            ujs_obj_get(vm, rec, ujs_atom(vm, "__jr", -1), &rj);
            if (ujs_call_value(vm, fn, ujs_undefined(), 1, &v, &r, 0) == UJS_THROW) {
                ujs_val e = vm->exception;
                vm->has_exception = 0;
                vm->exception = ujs_undefined();
                if (ujs_is_object(out)) ujs_promise_settle(vm, out, e, 1);
            } else if (ujs_is_object(out)) {
                /* a .finally handler passes the ORIGINAL outcome through
                 * rather than replacing it with its own return value */
                ujs_promise_settle(vm, out, r, 0);
            }
        }
    }
    vm->in_jobs = 0;
    return ran;
}

/* ---- the JS surface ------------------------------------------------------- */
static ujs_val p_then(ujs_args *a)
{
    ujs_val out = ujs_promise_new(a->vm);
    ujs_promise_react(a->vm, a->self,
                      a->argc > 0 ? a->argv[0] : ujs_undefined(),
                      a->argc > 1 ? a->argv[1] : ujs_undefined(), out);
    return out;
}

static ujs_val p_catch(ujs_args *a)
{
    ujs_val out = ujs_promise_new(a->vm);
    ujs_promise_react(a->vm, a->self, ujs_undefined(),
                      a->argc > 0 ? a->argv[0] : ujs_undefined(), out);
    return out;
}

static ujs_val p_finally(ujs_args *a)
{
    /* the same callback on both paths */
    ujs_val out = ujs_promise_new(a->vm);
    ujs_val cb = a->argc > 0 ? a->argv[0] : ujs_undefined();
    ujs_promise_react(a->vm, a->self, cb, cb, out);
    return out;
}

static ujs_val p_resolve(ujs_args *a)
{
    ujs_val v = a->argc > 0 ? a->argv[0] : ujs_undefined();
    ujs_val p;
    if (ujs_is_promise(a->vm, v)) return v;
    p = ujs_promise_new(a->vm);
    ujs_promise_settle(a->vm, p, v, 0);
    return p;
}

static ujs_val p_reject(ujs_args *a)
{
    ujs_val p = ujs_promise_new(a->vm);
    ujs_promise_settle(a->vm, p, a->argc > 0 ? a->argv[0] : ujs_undefined(), 1);
    return p;
}

/* `resolve` / `reject` handed to a Promise executor.  A C function here has no
 * closure, so the promise travels on the function object as a property. */
static ujs_val p_res_fn(ujs_args *a)
{
    ujs_promise_settle(a->vm, a->data,
                       a->argc > 0 ? a->argv[0] : ujs_undefined(), 0);
    return ujs_undefined();
}

static ujs_val p_rej_fn(ujs_args *a)
{
    ujs_promise_settle(a->vm, a->data,
                       a->argc > 0 ? a->argv[0] : ujs_undefined(), 1);
    return ujs_undefined();
}

/* Promise.all: one counter, one result array, first rejection wins. */
static ujs_val p_all_step(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_val st = ujs_undefined(), out = ujs_undefined(), arr = ujs_undefined();
    ujs_val idx = ujs_undefined(), left = ujs_undefined();
    ujs_val v = a->argc > 0 ? a->argv[0] : ujs_undefined();
    if (!ujs_is_object(a->data)) return ujs_undefined();
    ujs_obj_get(vm, ujs_val_obj(a->data), ujs_atom(vm, "__pa", -1), &st);
    ujs_obj_get(vm, ujs_val_obj(a->data), ujs_atom(vm, "__pi", -1), &idx);
    if (!ujs_is_object(st)) return ujs_undefined();
    ujs_obj_get(vm, ujs_val_obj(st), ujs_atom(vm, "out", -1), &out);
    ujs_obj_get(vm, ujs_val_obj(st), ujs_atom(vm, "arr", -1), &arr);
    ujs_obj_get(vm, ujs_val_obj(st), ujs_atom(vm, "left", -1), &left);
    if (ujs_is_object(arr))
        ujs_arr_set(vm, ujs_val_obj(arr), (u32)ujs_num_of(vm, idx), v);
    {   int n = (int)ujs_num_of(vm, left) - 1;
        ujs_obj_put(vm, ujs_val_obj(st), ujs_atom(vm, "left", -1),
                    ujs_number((double)n), P_WRITE);
        if (n <= 0) ujs_promise_settle(vm, out, arr, 0);
    }
    return ujs_undefined();
}

static ujs_val p_all_fail(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_val st = ujs_undefined(), out = ujs_undefined();
    if (!ujs_is_object(a->data)) return ujs_undefined();
    ujs_obj_get(vm, ujs_val_obj(a->data), ujs_atom(vm, "__pa", -1), &st);
    if (!ujs_is_object(st)) return ujs_undefined();
    ujs_obj_get(vm, ujs_val_obj(st), ujs_atom(vm, "out", -1), &out);
    ujs_promise_settle(vm, out, a->argc > 0 ? a->argv[0] : ujs_undefined(), 1);
    return ujs_undefined();
}

static ujs_val p_all(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_val out = ujs_promise_new(vm);
    ujs_val list = a->argc > 0 ? a->argv[0] : ujs_undefined();
    ujs_obj *arr, *st;
    u32 n, i;

    if (!ujs_is_object(list)) { ujs_promise_settle(vm, out, list, 0); return out; }
    n = ujs_val_obj(list)->nelems;
    arr = ujs_obj_new(vm, C_ARRAY, vm->arr_proto);
    st = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
    if (!arr || !st) return out;
    ujs_obj_put(vm, st, ujs_atom(vm, "out", -1), out, P_WRITE);
    ujs_obj_put(vm, st, ujs_atom(vm, "arr", -1), ujs_obj_val(arr), P_WRITE);
    ujs_obj_put(vm, st, ujs_atom(vm, "left", -1), ujs_number((double)n), P_WRITE);
    if (!n) { ujs_promise_settle(vm, out, ujs_obj_val(arr), 0); return out; }

    for (i = 0; i < n; i++) {
        ujs_val e = ujs_arr_get(ujs_val_obj(list), i);
        ujs_val ok = ujs_function_new(vm, p_all_step, "", 1);
        ujs_val no = ujs_function_new(vm, p_all_fail, "", 1);
        ujs_val p = e;
        if (!ujs_is_promise(vm, e)) {
            p = ujs_promise_new(vm);
            ujs_promise_settle(vm, p, e, 0);
        }
        {   /* each element's callbacks carry the shared state AND their own
             * index as bound data - a C function has no closure to hold it */
            ujs_obj *b = ujs_obj_new(vm, C_PLAIN, vm->obj_proto);
            if (b) {
                ujs_obj_put(vm, b, ujs_atom(vm, "__pa", -1), ujs_obj_val(st), P_WRITE);
                ujs_obj_put(vm, b, ujs_atom(vm, "__pi", -1),
                            ujs_number((double)i), P_WRITE);
                ujs_function_set_data(vm, ok, ujs_obj_val(b));
                ujs_function_set_data(vm, no, ujs_obj_val(b));
            }
        }
        ujs_promise_react(vm, p, ok, no, ujs_undefined());
    }
    return out;
}

static ujs_val p_ctor(ujs_args *a)
{
    ujs_vm *vm = a->vm;
    ujs_val p = ujs_promise_new(vm);
    ujs_val ex = a->argc > 0 ? a->argv[0] : ujs_undefined();
    if (ujs_is_object(ex)) {
        ujs_val res = ujs_function_new(vm, p_res_fn, "", 1);
        ujs_val rej = ujs_function_new(vm, p_rej_fn, "", 1);
        ujs_val args[2], r;
        ujs_function_set_data(vm, res, p);
        ujs_function_set_data(vm, rej, p);
        args[0] = res; args[1] = rej;
        /* The executor runs SYNCHRONOUSLY, as the standard requires; only the
         * reactions are deferred. */
        if (ujs_call_value(vm, ex, ujs_undefined(), 2, args, &r, 0) == UJS_THROW) {
            ujs_val e = vm->exception;
            vm->has_exception = 0;
            vm->exception = ujs_undefined();
            ujs_promise_settle(vm, p, e, 1);
        }
    }
    return p;
}

void ujs_promise_init(ujs_vm *vm)
{
    ujs_obj *g = vm->global;
    ujs_val ctor = ujs_function_new(vm, p_ctor, "Promise", 1);
    if (!ujs_is_object(ctor)) return;
    ujs_set_fn(vm, ctor, "resolve", p_resolve, 1);
    ujs_set_fn(vm, ctor, "reject", p_reject, 1);
    ujs_set_fn(vm, ctor, "all", p_all, 1);
    ujs_obj_put(vm, g, ujs_atom(vm, "Promise", -1), ctor, P_DEFAULT);

    /* then/catch/finally hang off Object.prototype: a promise here is a plain
     * object, so this is where every promise can see them.  The names are
     * distinctive enough that shadowing an ordinary object is not a concern
     * this engine has - and it is stated in UNOJS.md rather than assumed. */
    ujs_set_fn(vm, ujs_obj_val(vm->obj_proto), "then", p_then, 2);
    ujs_set_fn(vm, ujs_obj_val(vm->obj_proto), "catch", p_catch, 1);
    ujs_set_fn(vm, ujs_obj_val(vm->obj_proto), "finally", p_finally, 1);
}
