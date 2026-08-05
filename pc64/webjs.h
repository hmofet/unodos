/* ===========================================================================
 * webjs - the LIVE DOM binding: page scripts that can see and change the
 * document, on EITHER script engine.
 *
 * Three things make this file the one place in the system that sees both
 * halves (docs/WEB-ENGINE-DESIGN.md M5):
 *
 *  1. unoweb knows nothing about JavaScript, by contract (UNOWEB.md).
 *  2. unojs and quickjs know nothing about the DOM, by contract.
 *  3. Their C APIs are nothing alike.
 *
 * So the binding is split three ways. The DOM OPERATIONS are engine-neutral
 * C over unoweb's public API (webjs.c). The ENGINE ADAPTER is a small vtable
 * each backend fills in with its own value types (js.c for unojs, qjsweb.c
 * for quickjs) - about forty lines each, and the only engine-specific code.
 * The ERGONOMICS are a JS PRELUDE evaluated before any page script, which
 * wraps integer node handles in real Element objects. It comes in two parts,
 * because the engines are not equally capable: a METHOD CORE both engines
 * run, and a PROPERTY layer (textContent, innerHTML, ...) that needs
 * Object.defineProperty and so lands on quickjs only. webjs_has_properties()
 * reports which a page got - measured, not assumed: unojs turns out to have
 * neither object-literal accessors nor defineProperty nor `arguments`.
 *
 * The VM now LIVES for the page rather than for one <script>: timers and
 * event handlers are callbacks the browser fires long after the last script
 * block ran, and a VM that was destroyed cannot hold them.
 * ======================================================================== */
#ifndef PC64_WEBJS_H
#define PC64_WEBJS_H

#include "../unoweb/unoweb.h"

/* ---- the engine adapter ---------------------------------------------------
 * A call frame is the backend's own value machinery behind a table of five
 * operations, so the natives in webjs.c are written once. `impl` is whatever
 * the backend needs (a ujs_args*, a JSContext+argv pair, ...). */
typedef struct {
    int         (*argc)(void *impl);
    const char *(*arg_str)(void *impl, int i);     /* "" when absent      */
    int         (*arg_int)(void *impl, int i);     /* 0 when absent       */
    void        (*ret_int)(void *impl, int v);
    void        (*ret_str)(void *impl, const char *s);  /* NULL = null    */
} webjs_argops;

typedef struct { const webjs_argops *ops; void *impl; } webjs_args;
typedef void (*webjs_native)(webjs_args *a);

const char *webjs_arg_str(webjs_args *a, int i);
int         webjs_arg_int(webjs_args *a, int i);
int         webjs_argc(webjs_args *a);
void        webjs_ret_int(webjs_args *a, int v);
void        webjs_ret_str(webjs_args *a, const char *s);

typedef struct {
    const char *name;
    void *(*vm_new)(void);
    void  (*vm_free)(void *vm);
    /* define a native as a GLOBAL function (the prelude hides these behind
     * document/window/Element, so their names never reach page authors) */
    int   (*def_fn)(void *vm, const char *name, webjs_native fn, int nargs);
    /* evaluate; on failure fill `err` and return non-zero */
    int   (*eval)(void *vm, const char *src, int len, char *err, int errmax);
} webjs_engine;

/* The engine js.c's dispatch currently selects, as an adapter. NULL when the
 * selected engine has no adapter compiled in. */
const webjs_engine *webjs_engine_current(void);

/* ---- page lifecycle -------------------------------------------------------
 * begin() builds a VM bound to `d`; every later call acts on that document.
 * end() tears it down. Both are idempotent and safe to call in any order,
 * because navigation can happen at any point including mid-script. */
int  webjs_page_begin(uw_doc *d);
void webjs_page_end(void);
int  webjs_page_active(void);

/* Run one <script> block in the page VM. Console output is appended to
 * `log`; returns 0 ok, 1 syntax error, 2 runtime error (same codes as
 * js_run, so the browser's existing handling is unchanged). */
int  webjs_run(const char *src, int len, char *log, int logmax);

/* Fire timers due at `now_ms`, then report whether anything ran - the
 * browser repaints when it did. Cheap when no timers exist. */
int  webjs_pump(unsigned now_ms, char *log, int logmax);

/* Dispatch `type` (e.g. "click") at `n`, walking up to the root the way
 * bubbling does. Returns 1 if any handler ran. */
int  webjs_event(uw_node *n, const char *type, char *log, int logmax);

/* 1 when the page's engine supports the property aliases (textContent,
 * innerHTML, ...) rather than only the method core - see the two preludes
 * in webjs.c. */
int  webjs_has_properties(void);

/* Why the last webjs_page_begin failed; "" when it succeeded. */
const char *webjs_last_error(void);

/* Did script touch the tree since this was last called? Clears the flag.
 * The browser restyles/relayouts when it returns 1 - which is what makes a
 * DOM change visible. */
int  webjs_take_dirty(void);

#endif
