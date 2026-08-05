/* ===========================================================================
 * js.c - the browser's script hook.
 *
 * This used to be a 577-line tree-walking interpreter. It is now a thin shim
 * over `unojs`, the real engine (bytecode VM, GC heap, fuel-based preemption)
 * that lives in ../unojs/ and knows nothing about browsers. Everything
 * web-shaped - `document.write`, `console.log` - is projected in from HERE, as
 * host functions, which is exactly the separation the engine exists to keep:
 * see unojs/UNOJS.md and docs/WEB-ENGINE-DESIGN.md.
 *
 * js.h is unchanged, so pc64_browser.c and pc64_spectest.c did not have to
 * move. When the full DOM bindings land (M5) they replace this file with
 * webjs.c and the browser talks to unojs directly.
 * ======================================================================== */
#include "js.h"
#include "../unojs/unojs.h"
#include <string.h>

/* ---- output sinks --------------------------------------------------------
 * The caller owns both buffers and may already have content in them (the
 * browser accumulates console output across a page's <script> blocks), so
 * both sinks APPEND and are hard-bounded by the caller's capacity. */
typedef struct { char *buf; int max, n; } sink;
static sink g_write, g_log;

static void sink_init(sink *s, char *buf, int max)
{
    s->buf = buf; s->max = max;
    s->n = (buf && max > 0) ? (int)strlen(buf) : 0;
    if (s->n > max - 1) s->n = max > 0 ? max - 1 : 0;
}

static void sink_put(sink *s, const char *b, int n)
{
    if (!s->buf || s->max <= 0) return;
    if (n > s->max - 1 - s->n) n = s->max - 1 - s->n;
    if (n <= 0) return;
    memcpy(s->buf + s->n, b, (size_t)n);
    s->n += n;
    s->buf[s->n] = 0;
}

static void sink_str(sink *s, const char *z) { sink_put(s, z, (int)strlen(z)); }

/* Stringify one argument into a sink. Page scripts control these values, so a
 * throwing toString() must abort the call rather than be silently ignored. */
static int put_arg(ujs_vm *vm, sink *s, ujs_val v)
{
    ujs_val sv;
    const char *b;
    size_t n;
    if (ujs_to_string(vm, v, &sv) != UJS_OK) return 0;
    b = ujs_string_bytes(vm, sv, &n);
    if (b) sink_put(s, b, (int)n);
    return 1;
}

/* ---- the host surface: document.write / console.log ---------------------- */
static ujs_val h_doc_write(ujs_args *a)
{
    int i;
    for (i = 0; i < a->argc; i++)
        if (!put_arg(a->vm, &g_write, a->argv[i])) break;
    return ujs_undefined();
}

static ujs_val h_doc_writeln(ujs_args *a)
{
    ujs_val r = h_doc_write(a);
    sink_str(&g_write, "\n");
    return r;
}

static ujs_val h_console_log(ujs_args *a)
{
    int i;
    for (i = 0; i < a->argc; i++) {
        if (i) sink_str(&g_log, " ");
        if (!put_arg(a->vm, &g_log, a->argv[i])) break;
    }
    sink_str(&g_log, "\n");
    return ujs_undefined();
}

/* ---- error reporting ----------------------------------------------------- */
static void report(ujs_vm *vm, ujs_result r)
{
    ujs_val e = ujs_exception(vm), m;
    const char *b;
    size_t n;
    sink_str(&g_log, r == UJS_SYNTAX ? "JS syntax error: " : "JS error: ");
    if (ujs_is_object(e) && ujs_get(vm, e, "message", &m) == UJS_OK &&
        (b = ujs_string_bytes(vm, m, &n)) != 0 && n) {
        ujs_val nm;
        if (ujs_get(vm, e, "name", &nm) == UJS_OK) {
            size_t nn;
            const char *nb = ujs_string_bytes(vm, nm, &nn);
            if (nb && nn) { sink_put(&g_log, nb, (int)nn); sink_str(&g_log, ": "); }
        }
        sink_put(&g_log, b, (int)n);
    } else {
        char buf[192];
        sink_str(&g_log, ujs_describe(vm, e, buf, sizeof buf));
    }
    sink_str(&g_log, "\n");
}

/* ===========================================================================
 * The unojs adapter for webjs (M5). Everything below is unojs's value model
 * behind webjs.h's five-operation call frame; the DOM itself lives in
 * webjs.c and is written once for both engines.
 * ======================================================================== */
#include "webjs.h"

/* A native's frame while it runs. Argument strings must survive until the
 * native returns, so a coerced string is parked in a per-frame slot rather
 * than a VM temporary the GC could move under us. */
typedef struct {
    ujs_args *a;
    ujs_val   ret;
    char      sbuf[4][512];
    int       nsbuf;
} ujs_frame;

static int uf_argc(void *impl) { return ((ujs_frame *)impl)->a->argc; }

static const char *uf_arg_str(void *impl, int i)
{
    ujs_frame *f = (ujs_frame *)impl;
    ujs_val sv;
    const char *b;
    size_t n;
    char *dst;
    if (i >= f->a->argc || f->nsbuf >= 4) return "";
    if (ujs_to_string(f->a->vm, f->a->argv[i], &sv) != UJS_OK) return "";
    b = ujs_string_bytes(f->a->vm, sv, &n);
    if (!b) return "";
    dst = f->sbuf[f->nsbuf++];
    if (n > sizeof f->sbuf[0] - 1) n = sizeof f->sbuf[0] - 1;
    memcpy(dst, b, n);
    dst[n] = 0;
    return dst;
}

static int uf_arg_int(void *impl, int i)
{
    ujs_frame *f = (ujs_frame *)impl;
    if (i >= f->a->argc) return 0;
    return (int)ujs_to_number(f->a->vm, f->a->argv[i]);
}

static void uf_ret_int(void *impl, int v)
{ ((ujs_frame *)impl)->ret = ujs_number((double)v); }

static void uf_ret_str(void *impl, const char *s)
{
    ujs_frame *f = (ujs_frame *)impl;
    f->ret = s ? ujs_string(f->a->vm, s, (int)strlen(s)) : ujs_null();
}

static const webjs_argops UJS_OPS = {
    uf_argc, uf_arg_str, uf_arg_int, uf_ret_int, uf_ret_str
};

/* unojs has no per-function user pointer, so the native a given ujs_cfunc
 * must call is carried by a table: one trampoline per binding slot. The
 * table is filled in registration order by uja_def_fn. */
#define UJA_MAX 32
static webjs_native g_uja[UJA_MAX];
static int          g_nuja;

static ujs_val uja_call(ujs_args *a, int slot)
{
    ujs_frame f;
    webjs_args wa;
    if (slot >= g_nuja || !g_uja[slot]) return ujs_undefined();
    memset(&f, 0, sizeof f);
    f.a = a;
    f.ret = ujs_undefined();
    wa.ops = &UJS_OPS;
    wa.impl = &f;
    g_uja[slot](&wa);
    return f.ret;
}

/* The trampolines. One per slot, because unojs identifies a native only by
 * its function pointer - there is no closure data to hang a slot index on.
 * Twenty-five is the binding count plus headroom; def_fn refuses past it
 * rather than silently binding the wrong native. */
#define UJA_TRAMP(n) static ujs_val uja_##n(ujs_args *a) { return uja_call(a, n); }
UJA_TRAMP(0)  UJA_TRAMP(1)  UJA_TRAMP(2)  UJA_TRAMP(3)  UJA_TRAMP(4)
UJA_TRAMP(5)  UJA_TRAMP(6)  UJA_TRAMP(7)  UJA_TRAMP(8)  UJA_TRAMP(9)
UJA_TRAMP(10) UJA_TRAMP(11) UJA_TRAMP(12) UJA_TRAMP(13) UJA_TRAMP(14)
UJA_TRAMP(15) UJA_TRAMP(16) UJA_TRAMP(17) UJA_TRAMP(18) UJA_TRAMP(19)
UJA_TRAMP(20) UJA_TRAMP(21) UJA_TRAMP(22) UJA_TRAMP(23) UJA_TRAMP(24)
UJA_TRAMP(25) UJA_TRAMP(26) UJA_TRAMP(27) UJA_TRAMP(28) UJA_TRAMP(29)
UJA_TRAMP(30) UJA_TRAMP(31)
static const ujs_cfunc UJA_FN[UJA_MAX] = {
    uja_0,  uja_1,  uja_2,  uja_3,  uja_4,  uja_5,  uja_6,  uja_7,
    uja_8,  uja_9,  uja_10, uja_11, uja_12, uja_13, uja_14, uja_15,
    uja_16, uja_17, uja_18, uja_19, uja_20, uja_21, uja_22, uja_23,
    uja_24, uja_25, uja_26, uja_27, uja_28, uja_29, uja_30, uja_31
};

static void *uja_vm_new(void)
{
    ujs_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.heap_max = 4u << 20;
    /* A page VM outlives one script, so the per-slice fuel bounds a single
     * turn (a hostile handler cannot wedge the desktop) while fuel_total
     * stays unlimited - a page legitimately runs script all day. */
    cfg.fuel_per_slice = 2000000;
    g_nuja = 0;
    return ujs_new(&cfg);
}

static void uja_vm_free(void *vm) { ujs_free((ujs_vm *)vm); }

static int uja_def_fn(void *vm, const char *name, webjs_native fn, int nargs)
{
    ujs_vm *v = (ujs_vm *)vm;
    if (g_nuja >= UJA_MAX) return -1;
    g_uja[g_nuja] = fn;
    return ujs_set_fn(v, ujs_global(v), name, UJA_FN[g_nuja++], nargs);
}

static int uja_eval(void *vm, const char *src, int len, char *err, int errmax)
{
    ujs_vm *v = (ujs_vm *)vm;
    ujs_val out;
    ujs_result r = ujs_eval(v, src, len, &out);
    int guard = 0;
    while (r == UJS_YIELD && guard++ < 64) r = ujs_resume(v, &out);
    if (r == UJS_OK) return 0;
    if (err && errmax > 0) {
        ujs_val e = ujs_exception(v), m;
        size_t n = 0;
        const char *b = NULL;
        err[0] = 0;
        if (ujs_is_object(e) && ujs_get(v, e, "message", &m) == UJS_OK)
            b = ujs_string_bytes(v, m, &n);
        if (b && n) {
            if (n > (size_t)errmax - 12) n = (size_t)errmax - 12;
            memcpy(err, "JS error: ", 10);
            memcpy(err + 10, b, n);
            err[10 + n] = 0;
        } else {
            char buf[160];
            const char *d = ujs_describe(v, e, buf, sizeof buf);
            int k = 0;
            while (d[k] && k < errmax - 1) { err[k] = d[k]; k++; }
            err[k] = 0;
        }
    }
    /* CLEAR it. The page VM outlives this call, and a pending exception left
     * behind is reported again by the NEXT eval - which made a script fail
     * with the previous script's error message. Only a VM that dies at the
     * end of every run can get away without this, which is exactly what
     * changed in M5. */
    ujs_clear_exception(v);
    return r == UJS_SYNTAX ? 1 : 2;
}

static const webjs_engine UNOJS_ENGINE = {
    "unojs", uja_vm_new, uja_vm_free, uja_def_fn, uja_eval
};

/* ---- engine dispatch ------------------------------------------------------
 * The one runtime switch between the in-tree engine and the vendored
 * quickjs (qjsweb.c). Registry-style: a new engine appends a row. */
static const struct { const char *name;
                      int (*run)(const char *, char *, int, char *, int); }
g_engines[JS_ENGINE_COUNT] = {
    { "unojs",   js_run_ujs },
    { "quickjs", js_run_qjs },
};
static int g_engine = JS_ENGINE_UNOJS;

int  js_engine_get(void) { return g_engine; }
void js_engine_set(int engine)
{ if (engine >= 0 && engine < JS_ENGINE_COUNT) g_engine = engine; }
const char *js_engine_name(int engine)
{ return engine >= 0 && engine < JS_ENGINE_COUNT ? g_engines[engine].name : "?"; }

int js_run(const char *src, char *out, int outmax, char *log, int logmax)
{ return g_engines[g_engine].run(src, out, outmax, log, logmax); }

/* The webjs adapter for whichever engine is selected: the one place that maps
 * an engine choice onto a DOM binding.
 *
 * KNOWN ISSUE (2026-08-06): the quickjs adapter (QJS_WEBJS_ENGINE, qjsweb.c)
 * is written and correct as far as it has been tested - the DOM binding runs
 * clean under a native linux build - but the MINGW host test binary dies at
 * startup the moment quickjs is the selected DOM engine, before main's first
 * statement, with the same object set that runs fine when unojs is selected.
 * Not root-caused. Until it is, the DOM binding stays on unojs for every
 * page: shipping a binding that might take the browser down with it is worse
 * than shipping one engine's. js_run() is UNAFFECTED - the script-engine
 * switch on uno:engine still runs page scripts on either engine; it is only
 * the live-DOM layer that is pinned here. See docs/WEB-ENGINE-DESIGN.md M5. */
extern const webjs_engine QJS_WEBJS_ENGINE;

const webjs_engine *webjs_engine_current(void)
{
    (void)&QJS_WEBJS_ENGINE;
    return &UNOJS_ENGINE;
}

/* ---- the unojs backend ---------------------------------------------------- */
int js_run_ujs(const char *src, char *out, int outmax, char *log, int logmax)
{
    ujs_config cfg;
    ujs_vm *vm;
    ujs_val v, g, doc, con;
    ujs_result r;
    int rc = 0, guard = 0;

    sink_init(&g_write, out, outmax);
    sink_init(&g_log, log, logmax);

    memset(&cfg, 0, sizeof cfg);
    cfg.heap_max = 4u << 20;          /* a page's scripts, bounded             */
    /* This entry point is SYNCHRONOUS - the browser blocks on it - so the fuel
     * slice is large and the loop below simply drains it. What matters here is
     * fuel_total: it is the ceiling that turns a hostile `while(1)` into a slow
     * page instead of a dead desktop. Real per-frame yielding arrives with the
     * event loop in M5. */
    cfg.fuel_per_slice = 2000000;
    cfg.fuel_total     = 40000000;

    vm = ujs_new(&cfg);
    if (!vm) { sink_str(&g_log, "JS: out of memory\n"); return 3; }

    g = ujs_global(vm);
    doc = ujs_object_new(vm);
    ujs_set_fn(vm, doc, "write", h_doc_write, 1);
    ujs_set_fn(vm, doc, "writeln", h_doc_writeln, 1);
    ujs_set(vm, g, "document", doc);

    con = ujs_object_new(vm);
    ujs_set_fn(vm, con, "log", h_console_log, 1);
    ujs_set_fn(vm, con, "warn", h_console_log, 1);
    ujs_set_fn(vm, con, "error", h_console_log, 1);
    ujs_set_fn(vm, con, "info", h_console_log, 1);
    ujs_set(vm, g, "console", con);

    /* `window` is the global, as in a browser - enough for the feature checks
     * real pages open with. The DOM itself is M5. */
    ujs_set(vm, g, "window", g);

    r = ujs_eval(vm, src, -1, &v);
    while (r == UJS_YIELD && guard++ < 64) r = ujs_resume(vm, &v);

    if (r != UJS_OK) { report(vm, r); rc = (r == UJS_SYNTAX) ? 1 : 2; }
    ujs_free(vm);
    return rc;
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
