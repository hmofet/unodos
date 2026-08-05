/* qjs_dispatch_test.c - the browser-visible seam, end to end on host.
 *
 * Runs the same scripts through js_run() with each engine selected and
 * checks (a) the dispatch switches, (b) both engines honour the sink
 * contract, (c) outputs AGREE where the engines' feature sets overlap,
 * (d) the rc mapping matches for the error classes. Build via
 * build-host-test.sh alongside the engine-core test. */
#include <stdio.h>
#include <string.h>
#include "../../js.h"

/* uno_native_* stubs for qjs_port's clocks */
int uno_native_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s)
{ *y = 2026; *mo = 8; *d = 5; *h = 12; *mi = 0; *s = 0; return 0; }
unsigned long long uno_native_rdtsc(void) { static unsigned long long t; return t += 1000; }
unsigned long long uno_native_tsc_per_us(void) { return 0; }

static int g_pass, g_fail;

static void note(int ok, const char *name)
{ printf("%s %s\n", ok ? "pass" : "FAIL", name); if (ok) g_pass++; else g_fail++; }

/* run `src` on `engine`; returns rc, output in out/log */
static int run_on(int engine, const char *src, char *out, int om, char *log, int lm)
{
    out[0] = 0; log[0] = 0;
    js_engine_set(engine);
    return js_run(src, out, om, log, lm);
}

int main(void)
{
    static char o1[4096], l1[4096], o2[4096], l2[4096];
    int rc1, rc2, i;

    /* scripts BOTH engines must run identically (the overlap set: unojs is
     * a subset engine, so nothing exotic here) */
    static const char *agree[] = {
        "document.write('hi ', 1 + 2)",
        "for (var i = 0; i < 3; i++) console.log('n', i)",
        "function f(n) { return n <= 1 ? 1 : n * f(n - 1); } document.write(f(6))",
        "console.log('str', ('abc' + 'def').length)",
    };

    note(js_engine_get() == JS_ENGINE_UNOJS, "default engine is unojs");
    js_engine_set(99);
    note(js_engine_get() == JS_ENGINE_UNOJS, "out-of-range set ignored");
    note(!strcmp(js_engine_name(JS_ENGINE_QUICKJS), "quickjs"), "engine name");

    for (i = 0; i < (int)(sizeof agree / sizeof agree[0]); i++) {
        rc1 = run_on(JS_ENGINE_UNOJS,   agree[i], o1, sizeof o1, l1, sizeof l1);
        rc2 = run_on(JS_ENGINE_QUICKJS, agree[i], o2, sizeof o2, l2, sizeof l2);
        char name[64];
        snprintf(name, sizeof name, "agree[%d] rc(%d,%d)", i, rc1, rc2);
        note(rc1 == 0 && rc2 == 0 && !strcmp(o1, o2) && !strcmp(l1, l2), name);
        if (strcmp(o1, o2) || strcmp(l1, l2))
            printf("   unojs out=%s log=%s\n   qjs   out=%s log=%s\n", o1, l1, o2, l2);
    }

    /* quickjs-only ground: real ES in the SAME sinks */
    rc2 = run_on(JS_ENGINE_QUICKJS,
                 "class P { constructor(n) { this.n = n; } get d() { return this.n * 2; } }"
                 "document.write(JSON.stringify({v: new P(21).d}))",
                 o2, sizeof o2, l2, sizeof l2);
    note(rc2 == 0 && !strcmp(o2, "{\"v\":42}"), "quickjs ES class+JSON");

    /* error-class rc mapping on the quickjs backend */
    rc2 = run_on(JS_ENGINE_QUICKJS, "syntax error here(", o2, sizeof o2, l2, sizeof l2);
    note(rc2 == 1 && strstr(l2, "JS syntax error:") == l2, "qjs syntax rc=1");
    rc2 = run_on(JS_ENGINE_QUICKJS, "null.x", o2, sizeof o2, l2, sizeof l2);
    note(rc2 == 2 && strstr(l2, "JS error: TypeError") == l2, "qjs runtime rc=2");

    /* the runtime budget: a hostile loop must come back, not hang */
    rc2 = run_on(JS_ENGINE_QUICKJS, "for(;;);", o2, sizeof o2, l2, sizeof l2);
    note(rc2 == 2, "qjs infinite loop interrupted");

    /* the C-stack guard: hostile recursion must be caught by the engine */
    rc2 = run_on(JS_ENGINE_QUICKJS, "function r() { return r(); } r()",
                 o2, sizeof o2, l2, sizeof l2);
    note(rc2 == 2 && strstr(l2, "stack") != 0, "qjs deep recursion caught");

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail != 0;
}
