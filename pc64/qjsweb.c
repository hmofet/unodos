/* ===========================================================================
 * qjsweb.c - the QuickJS backend behind js_run() (engine dispatch in js.c).
 *
 * Mirrors the unojs backend's host surface exactly - document.write/writeln,
 * console.log/warn/error/info, window === globalThis - and its js_run
 * contract (rc 0/1/2/3, messages appended to `log`), so the browser cannot
 * tell which engine ran a page. The engine itself is the vendored quickjs-ng
 * under pc64/quickjs/ (see VENDOR.md there); this file is OUR code and is
 * the only thing that talks to its C API.
 *
 * Bounds, matching unojs's config one for one where quickjs has the knob:
 *   heap       4 MB     JS_SetMemoryLimit (unojs: cfg.heap_max)
 *   runtime    ~40M ops interrupt handler x 4000 firings (unojs: fuel_total;
 *              quickjs fires the handler about every 10k interpreter ops)
 *   C stack    64 KB    JS_SetMaxStackSize - quickjs RECURSES the C stack
 *              where unojs's VM is heap-based, and the kernel runs on the
 *              UEFI stack (>=128 KB guaranteed, no guard page), so a hostile
 *              deeply-recursive page must hit quickjs's own stack check
 *              before it hits real stack bottom.
 * ======================================================================== */
#include "js.h"
#include <string.h>
#include "quickjs.h"

/* ---- output sinks (same append-and-bound contract as js.c's) ------------- */
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

/* Stringify one argument into a sink; a throwing toString() aborts the call
 * with the exception left pending, same as the unojs backend. */
static int put_arg(JSContext *ctx, sink *s, JSValueConst v)
{
    size_t n;
    const char *b = JS_ToCStringLen(ctx, &n, v);
    if (!b) return 0;
    sink_put(s, b, (int)n);
    JS_FreeCString(ctx, b);
    return 1;
}

/* ---- the host surface ----------------------------------------------------- */
static JSValue h_doc_write(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    int i;
    (void)self;
    for (i = 0; i < argc; i++)
        if (!put_arg(ctx, &g_write, argv[i])) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue h_doc_writeln(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    JSValue r = h_doc_write(ctx, self, argc, argv);
    sink_str(&g_write, "\n");
    return r;
}

static JSValue h_console_log(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    int i;
    (void)self;
    for (i = 0; i < argc; i++) {
        if (i) sink_str(&g_log, " ");
        if (!put_arg(ctx, &g_log, argv[i])) { sink_str(&g_log, "\n"); return JS_EXCEPTION; }
    }
    sink_str(&g_log, "\n");
    return JS_UNDEFINED;
}

/* ---- runtime budget ------------------------------------------------------- */
static int g_interrupts;

static int on_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt; (void)opaque;
    return ++g_interrupts > 4000;        /* ~40M ops, unojs's fuel_total */
}

/* ---- error reporting (same shapes as the unojs backend) ------------------- */
static int report(JSContext *ctx)
{
    JSValue e = JS_GetException(ctx);
    int rc = 2;
    int syntax = 0, oom = 0;
    if (JS_IsObject(e)) {
        JSValue nm = JS_GetPropertyStr(ctx, e, "name");
        JSValue ms = JS_GetPropertyStr(ctx, e, "message");
        const char *nb = JS_ToCString(ctx, nm);
        const char *mb = JS_ToCString(ctx, ms);
        if (nb && !strcmp(nb, "SyntaxError")) syntax = 1;
        if (nb && !strcmp(nb, "InternalError") && mb && strstr(mb, "out of memory")) oom = 1;
        sink_str(&g_log, syntax ? "JS syntax error: " : "JS error: ");
        if (nb && !syntax) { sink_str(&g_log, nb); sink_str(&g_log, ": "); }
        sink_str(&g_log, mb ? mb : "(no message)");
        JS_FreeCString(ctx, nb);
        JS_FreeCString(ctx, mb);
        JS_FreeValue(ctx, nm);
        JS_FreeValue(ctx, ms);
    } else {
        const char *b = JS_ToCString(ctx, e);
        sink_str(&g_log, "JS error: ");
        sink_str(&g_log, b ? b : "(unprintable exception)");
        JS_FreeCString(ctx, b);
    }
    sink_str(&g_log, "\n");
    JS_FreeValue(ctx, e);
    if (syntax) rc = 1;
    if (oom)    rc = 3;
    return rc;
}

/* ---- js_run_qjs ----------------------------------------------------------- */
int js_run_qjs(const char *src, char *out, int outmax, char *log, int logmax)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue g, doc, con, v;
    int rc = 0;

    sink_init(&g_write, out, outmax);
    sink_init(&g_log, log, logmax);

    rt = JS_NewRuntime();
    if (!rt) { sink_str(&g_log, "JS: out of memory\n"); return 3; }
    JS_SetMemoryLimit(rt, 4u << 20);
    JS_SetMaxStackSize(rt, 64u << 10);
    g_interrupts = 0;
    JS_SetInterruptHandler(rt, on_interrupt, 0);

    ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); sink_str(&g_log, "JS: out of memory\n"); return 3; }

    g = JS_GetGlobalObject(ctx);

    doc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, doc, "write",   JS_NewCFunction(ctx, h_doc_write,   "write",   1));
    JS_SetPropertyStr(ctx, doc, "writeln", JS_NewCFunction(ctx, h_doc_writeln, "writeln", 1));
    JS_SetPropertyStr(ctx, g, "document", doc);

    con = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, con, "log",   JS_NewCFunction(ctx, h_console_log, "log",   1));
    JS_SetPropertyStr(ctx, con, "warn",  JS_NewCFunction(ctx, h_console_log, "warn",  1));
    JS_SetPropertyStr(ctx, con, "error", JS_NewCFunction(ctx, h_console_log, "error", 1));
    JS_SetPropertyStr(ctx, con, "info",  JS_NewCFunction(ctx, h_console_log, "info",  1));
    JS_SetPropertyStr(ctx, g, "console", con);

    /* window is the global, as in a browser (matches the unojs backend) */
    JS_SetPropertyStr(ctx, g, "window", JS_DupValue(ctx, g));
    JS_FreeValue(ctx, g);

    v = JS_Eval(ctx, src, strlen(src), "<page>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v))
        rc = report(ctx);
    JS_FreeValue(ctx, v);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}
