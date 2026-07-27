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

/* ---- js_run -------------------------------------------------------------- */
int js_run(const char *src, char *out, int outmax, char *log, int logmax)
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
