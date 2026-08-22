/* ===========================================================================
 * unojs - a small, embeddable JavaScript engine.
 *
 * CONTRACT VERSION 0.1  [EXPERIMENTAL until M1 lands; see UNOJS.md changelog]
 *
 * A bytecode VM with a garbage-collected heap and fuel-based preemption.
 * Written as a freestanding C99 library: it allocates through one host hook,
 * makes no OS calls, and has NO knowledge of HTML, CSS, the DOM, or any other
 * consumer. That separation is deliberate and load-bearing - the web engine's
 * DOM bindings live entirely in the embedder (pc64/webjs.c), and unojs must
 * always build and pass its test suite with no web code linked at all.
 *
 * Concurrency: none. One ujs_vm is single-threaded and reentrant only through
 * the documented callback points (ujs_cfunc bodies may call back into the VM).
 *
 * Memory safety model: every ujs_val handed to the embedder is a GC-visible
 * root ONLY while inside an open handle scope (ujs_scope_open/close). A raw
 * ujs_val held across an allocation with no scope may be collected. This is
 * the single rule an embedder must not break.
 * ======================================================================== */
#ifndef UNOJS_H
#define UNOJS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UJS_VERSION_MAJOR 0
#define UJS_VERSION_MINOR 1

/* ---- values -------------------------------------------------------------
 * A NaN-boxed 64-bit value: IEEE doubles are themselves; everything else
 * lives in the payload of a quiet NaN. Pass by value freely; it is one word.
 * The representation is PRIVATE - use the accessors below, never .bits. */
typedef struct { unsigned long long bits; } ujs_val;

typedef enum {
    UJS_TYPE_UNDEFINED = 0,
    UJS_TYPE_NULL,
    UJS_TYPE_BOOL,
    UJS_TYPE_NUMBER,
    UJS_TYPE_STRING,
    UJS_TYPE_OBJECT,        /* plain object, array, function, host object */
} ujs_type;

/* ---- result codes -------------------------------------------------------- */
typedef enum {
    UJS_OK = 0,
    UJS_THROW,              /* a JS exception is pending; ujs_exception()     */
    UJS_YIELD,              /* fuel exhausted mid-run; ujs_resume() continues */
    UJS_OOM,                /* heap cap hit and GC could not free enough      */
    UJS_SYNTAX,             /* compile failed; ujs_exception() has the detail */
} ujs_result;

typedef struct ujs_vm ujs_vm;

/* ---- configuration ------------------------------------------------------- */
typedef struct {
    /* Allocator. Both may be NULL to use the C library's malloc/free. The VM
     * calls these for its own arenas only; it never allocates per JS object. */
    void *(*alloc)(void *user, size_t n);
    void  (*free)(void *user, void *p);
    void  *alloc_user;

    /* Hard ceiling on the JS heap in bytes. A collection runs when the heap
     * would exceed it; if that does not free enough, allocation raises a JS
     * RangeError rather than growing. 0 = the default (8 MB). */
    size_t heap_max;

    /* Fuel granted per ujs_eval/ujs_resume call, in abstract steps (roughly
     * one step per branch, call, and loop back-edge). When it runs out the
     * call returns UJS_YIELD with all VM state intact. 0 = unlimited, which
     * is appropriate for host tools and NOT for untrusted page scripts. */
    unsigned long fuel_per_slice;

    /* Cumulative fuel across all slices of one script before it is killed
     * outright (UJS_THROW with a "script ran too long" error). 0 = unlimited. */
    unsigned long fuel_total;
} ujs_config;

/* ---- lifecycle ----------------------------------------------------------- */

/* Create a VM. `cfg` may be NULL for all defaults. Returns NULL on OOM. */
ujs_vm *ujs_new(const ujs_config *cfg);
void    ujs_free(ujs_vm *vm);

/* Compile and run `src` as a top-level script. `len` may be -1 for strlen.
 * On UJS_OK, *out (if non-NULL) receives the completion value. On UJS_YIELD
 * call ujs_resume() until it returns something else. */
ujs_result ujs_eval(ujs_vm *vm, const char *src, int len, ujs_val *out);

/* Continue a run that returned UJS_YIELD, granting another fuel slice. */
ujs_result ujs_resume(ujs_vm *vm, ujs_val *out);

/* The pending exception (after UJS_THROW / UJS_SYNTAX), or undefined. */
ujs_val ujs_exception(ujs_vm *vm);
/* Clear it. Until cleared, entering the VM again is a programming error. */
void    ujs_clear_exception(ujs_vm *vm);

/* A short human-readable rendering of `v` for diagnostics (never throws).
 * Writes into `buf` and returns it. Not JSON, not String(v) - a debug view. */
const char *ujs_describe(ujs_vm *vm, ujs_val v, char *buf, size_t buflen);

/* ---- handle scopes -------------------------------------------------------
 * Open a scope before creating or fetching values you will hold across any
 * further VM call; close it when done. Values created inside are rooted until
 * the close. To keep one past the close, pass it through ujs_scope_close.
 *
 *      ujs_scope s;
 *      ujs_scope_open(vm, &s);
 *      ujs_val o = ujs_object_new(vm);
 *      ... build it up, calling into the VM freely ...
 *      keep = ujs_scope_close(vm, &s, o);   // o survives; the rest is garbage
 */
typedef struct { int base; } ujs_scope;
void    ujs_scope_open(ujs_vm *vm, ujs_scope *s);
ujs_val ujs_scope_close(ujs_vm *vm, ujs_scope *s, ujs_val keep);

/* Pin a value as a permanent GC root (for globals the embedder holds forever).
 * Returns 0 on success. Unpin with ujs_unroot. */
int  ujs_root(ujs_vm *vm, ujs_val v);
void ujs_unroot(ujs_vm *vm, ujs_val v);

/* ---- constructing values ------------------------------------------------- */
ujs_val ujs_undefined(void);
ujs_val ujs_null(void);
ujs_val ujs_bool(int b);
ujs_val ujs_number(double d);
/* Copies `n` bytes of UTF-8 (n = -1 for strlen). Returns undefined on OOM. */
ujs_val ujs_string(ujs_vm *vm, const char *s, int n);
ujs_val ujs_object_new(ujs_vm *vm);
ujs_val ujs_array_new(ujs_vm *vm);

/* ---- inspecting values --------------------------------------------------- */
ujs_type ujs_typeof(ujs_val v);
int      ujs_is_undefined(ujs_val v);
int      ujs_is_null(ujs_val v);
int      ujs_is_number(ujs_val v);
int      ujs_is_string(ujs_val v);
int      ujs_is_object(ujs_val v);      /* includes arrays and functions */
int      ujs_is_array(ujs_vm *vm, ujs_val v);
int      ujs_is_function(ujs_vm *vm, ujs_val v);

double   ujs_to_number(ujs_vm *vm, ujs_val v);   /* ToNumber, may not throw */
int      ujs_to_bool(ujs_val v);                 /* ToBoolean               */
/* Bytes of a string value. NUL-terminated for convenience; `len` (optional)
 * gets the true byte length. Returns NULL if `v` is not a string - this does
 * NOT coerce (use ujs_to_string first) so that an accidental object never
 * silently stringifies inside a binding. */
const char *ujs_string_bytes(ujs_vm *vm, ujs_val v, size_t *len);
/* ToString. May run user code (toString), so it can throw: returns UJS_OK or
 * UJS_THROW and writes the result to *out. */
ujs_result ujs_to_string(ujs_vm *vm, ujs_val v, ujs_val *out);

/* ---- properties ---------------------------------------------------------- */
/* All property calls may run user code (accessors), hence a result code.
 * `name` is UTF-8, NUL-terminated, and interned internally. */
ujs_result ujs_get(ujs_vm *vm, ujs_val obj, const char *name, ujs_val *out);
ujs_result ujs_set(ujs_vm *vm, ujs_val obj, const char *name, ujs_val v);
ujs_result ujs_get_index(ujs_vm *vm, ujs_val obj, unsigned i, ujs_val *out);
ujs_result ujs_set_index(ujs_vm *vm, ujs_val obj, unsigned i, ujs_val v);
int        ujs_has(ujs_vm *vm, ujs_val obj, const char *name);
int        ujs_delete(ujs_vm *vm, ujs_val obj, const char *name);
/* Array length, or 0 for a non-array. */
unsigned   ujs_array_length(ujs_vm *vm, ujs_val arr);
ujs_result ujs_array_push(ujs_vm *vm, ujs_val arr, ujs_val v);

/* ---- calling ------------------------------------------------------------- */
/* Call `fn` with `self` and argv[argc]. Returns UJS_OK/THROW/YIELD/OOM.
 * A UJS_YIELD from inside a host call is resumed by ujs_resume like any other. */
ujs_result ujs_call(ujs_vm *vm, ujs_val fn, ujs_val self,
                    int argc, const ujs_val *argv, ujs_val *out);

/* ---- host functions and objects ------------------------------------------
 * This is how an embedder projects its own world into JS without unojs
 * knowing anything about it. A host object carries an opaque `user` pointer
 * and an optional finalizer run when the GC collects it. */
typedef struct {
    ujs_vm  *vm;
    ujs_val  self;
    int      argc;
    const ujs_val *argv;
    /* Whatever ujs_function_set_data() bound to THIS function object.  A C
     * function has no closure, so without this the only way to give one
     * private state is `self` - which is whatever the call site happened to
     * pass, and is undefined for a plain `f(x)`.  Promise's resolve/reject
     * pair is exactly that shape (UCD-21). */
    ujs_val  data;
} ujs_args;

/* Return the result. To throw, call ujs_throw() and return undefined. */
typedef ujs_val (*ujs_cfunc)(ujs_args *a);
typedef void    (*ujs_finalizer)(void *user);

ujs_val ujs_function_new(ujs_vm *vm, ujs_cfunc fn, const char *name, int nargs);
ujs_val ujs_host_new(ujs_vm *vm, void *user, ujs_finalizer fin);
/* The user pointer of a host object, or NULL if `v` is not one. Bindings must
 * treat NULL as "wrong receiver" and throw a TypeError. */
void   *ujs_host_user(ujs_vm *vm, ujs_val v);

/* Convenience: define a method / accessor pair on an object. 0 on success. */
int ujs_set_fn(ujs_vm *vm, ujs_val obj, const char *name, ujs_cfunc fn, int nargs);
/* Bind private state to a C function; it arrives as `a->data` on every call. */
void ujs_function_set_data(ujs_vm *vm, ujs_val fn, ujs_val data);
int ujs_set_accessor(ujs_vm *vm, ujs_val obj, const char *name,
                     ujs_cfunc getter, ujs_cfunc setter);

/* The global object (`window` in a browser embedding, but unojs does not
 * name it that - the embedder does). */
ujs_val ujs_global(ujs_vm *vm);

/* ---- throwing ------------------------------------------------------------ */
/* Set the pending exception. Always returns undefined so a ujs_cfunc can
 * `return ujs_throw(...)`. `msg` is a plain string, not a format. */
ujs_val ujs_throw(ujs_vm *vm, ujs_val err);
ujs_val ujs_throw_error(ujs_vm *vm, const char *kind, const char *msg);
/* kind is one of: "Error" "TypeError" "RangeError" "SyntaxError" "ReferenceError" */

/* ---- jobs (UCD-21) --------------------------------------------------------
 * Drain the microtask queue: every Promise reaction and every resumed `await`
 * that is ready.  ujs_eval() and ujs_resume() call it before they return, so a
 * script is complete when they are; a HOST that settles promises from C - a
 * request finishing, a file arriving - calls it once per frame, and that call
 * is where the continuation of every `await` in the extension host actually
 * runs.  Returns the number of jobs run.
 *
 * Bounded per call, and re-entry is refused: a promise chain that queues
 * itself forever slows down rather than taking the machine with it. */
int ujs_run_jobs(ujs_vm *vm);

/* A promise the HOST settles.  This is how an embedder hands JS something it
 * will answer later - a dialog the user has not closed, a request in flight -
 * and get `await` on the other side of it for free.  Settling runs the
 * reactions on the next ujs_run_jobs(), never inside the settle call. */
ujs_val ujs_promise(ujs_vm *vm);
void    ujs_promise_resolve(ujs_vm *vm, ujs_val p, ujs_val v);
void    ujs_promise_reject(ujs_vm *vm, ujs_val p, ujs_val v);

/* ---- fuel ---------------------------------------------------------------- */
/* Steps consumed since the current script started. */
unsigned long ujs_fuel_used(ujs_vm *vm);
/* Reset the cumulative counter (call between independent scripts). */
void          ujs_fuel_reset(ujs_vm *vm);

/* ---- GC ------------------------------------------------------------------ */
void   ujs_gc(ujs_vm *vm);           /* force a full collection */
size_t ujs_heap_used(ujs_vm *vm);

#ifdef __cplusplus
}
#endif
#endif /* UNOJS_H */
