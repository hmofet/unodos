/* The pc64 browser's script hook - now an ENGINE DISPATCH.
 *
 * js_run() keeps its historic contract: run one script; document.write()
 * output appends to `out`, console output to `log`; returns 0 ok, 1 syntax
 * error, 2 runtime error, 3 out of memory (errors leave a short message in
 * `log`). The browser and spectest call ONLY js_run and never care which
 * engine ran.
 *
 * Underneath there are two engines, runtime-switchable:
 *   JS_ENGINE_UNOJS    unojs, the in-tree engine (js.c). The default.
 *   JS_ENGINE_QUICKJS  vendored quickjs-ng (qjsweb.c over pc64/quickjs/).
 * Both are always compiled so neither can rot (the browser's two-painter
 * rule). Adding an engine = new backend file + append an entry in js.c's
 * table; nothing here reorders. */
#ifndef PC64_JS_H
#define PC64_JS_H

enum {
    JS_ENGINE_UNOJS = 0,
    JS_ENGINE_QUICKJS = 1,
    JS_ENGINE_COUNT
};

int  js_run(const char *src, char *out, int outmax, char *log, int logmax);

int         js_engine_get(void);
void        js_engine_set(int engine);        /* out of range: ignored */
const char *js_engine_name(int engine);       /* "unojs" / "quickjs"   */

/* backends (one per engine; the table in js.c is the only caller) */
int js_run_ujs(const char *src, char *out, int outmax, char *log, int logmax);
int js_run_qjs(const char *src, char *out, int outmax, char *log, int logmax);

#endif
