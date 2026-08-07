/* qjs_host_test.c - host smoke test for the QuickJS port layer.
 *
 * Links the SAME freestanding-compiled quickjs objects + qjs_port.c +
 * unojs's real ujs_math.c against the host CRT, with the uno_native_* time
 * sources stubbed below. This proves the engine + the derived double math +
 * the UTC time surface actually run before anything touches the kernel; the
 * QEMU spectest remains the real gate (a host harness tests different code -
 * see the UnoAmp lesson).
 *
 * Build (WSL, from pc64/): see quickjs/test/build-host-test.sh
 */
#include <stdio.h>
#include "../quickjs.h"

/* ---- uno_native_* stubs: a frozen clock ---------------------------------- */
/* 1 = read OK. This returned 0 while qjs_port.c's rtc_epoch_s treated non-zero
 * as the failure, so stub and caller were wrong in the same direction and the
 * date checks passed over a clock that was never actually read. Correcting the
 * caller made this stub fail them, which is the gate doing its job. */
int uno_native_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s)
{ *y = 2026; *mo = 8; *d = 5; *h = 12; *mi = 0; *s = 0; return 1; }
unsigned long long uno_native_rdtsc(void) { static unsigned long long t; return t += 1000; }
unsigned long long uno_native_tsc_per_us(void) { return 0; }   /* RTC fallback path */

static int g_pass, g_fail;

static void check(JSContext *ctx, const char *name, const char *expr)
{
    JSValue v = JS_Eval(ctx, expr, strlen(expr), name, JS_EVAL_TYPE_GLOBAL);
    int ok = 0;
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        printf("FAIL %-28s threw: %s\n", name, msg ? msg : "?");
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
    } else {
        ok = JS_ToBool(ctx, v) == 1;
        printf("%s %-28s\n", ok ? "pass" : "FAIL", name);
    }
    JS_FreeValue(ctx, v);
    if (ok) g_pass++; else g_fail++;
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    check(ctx, "arith",        "1 + 2 * 3 === 7");
    check(ctx, "string",       "('uno' + 'dos').toUpperCase() === 'UNODOS'");
    check(ctx, "closure",      "(() => { let a = 0; for (let i = 1; i <= 10; i++) a += i; return a; })() === 55");
    check(ctx, "class",        "class A { f() { return 42; } } new A().f() === 42");

    /* the derived double math, against identities with tolerance */
    check(ctx, "math.pow",     "Math.abs(Math.pow(2, 10) - 1024) < 1e-9");
    check(ctx, "math.sqrt",    "Math.sqrt(81) === 9");
    check(ctx, "math.explog",  "Math.abs(Math.log(Math.exp(3)) - 3) < 1e-12");
    check(ctx, "math.trig",    "Math.abs(Math.sin(1)**2 + Math.cos(1)**2 - 1) < 1e-12");
    check(ctx, "math.asin",    "Math.abs(Math.asin(Math.sin(0.5)) - 0.5) < 1e-12");
    check(ctx, "math.atan2",   "Math.abs(Math.atan2(1, 1) - Math.PI / 4) < 1e-12");
    check(ctx, "math.cbrt",    "Math.abs(Math.cbrt(27) - 3) < 1e-12");
    check(ctx, "math.hypot",   "Math.abs(Math.hypot(3, 4) - 5) < 1e-12");
    check(ctx, "math.log2",    "Math.abs(Math.log2(4096) - 12) < 1e-9");
    check(ctx, "math.tanh",    "Math.abs(Math.tanh(100) - 1) < 1e-12");
    check(ctx, "math.round",   "Math.round(2.5) === 3 && Math.round(-2.5) === -2");
    check(ctx, "math.trunc",   "Math.trunc(-3.9) === -3 && Math.trunc(3.9) === 3");
    check(ctx, "math.sign0",   "Object.is(Math.cbrt(-0), -0) && Math.cbrt(-8) === -2");

    /* number formatting + parsing (dtoa, js_atod, the JSON strtod) */
    check(ctx, "num.print",    "(0.1).toString() === '0.1' && (1e21).toString() === '1e+21'");
    check(ctx, "num.radix",    "(255).toString(16) === 'ff' && parseInt('ff', 16) === 255");
    check(ctx, "num.frexp",    "(0.5).toString(2) === '0.1'");
    check(ctx, "json",         "JSON.parse('{\"a\":0.1,\"b\":1.5e300}').b === 1.5e300");
    check(ctx, "json.rt",      "JSON.stringify(JSON.parse('[0.1,2,3]')) === '[0.1,2,3]'");

    /* regexp incl. unicode property classes (libregexp + libunicode) */
    check(ctx, "regexp",       "/a(b+)c/.exec('xabbbc')[1] === 'bbb'");
    check(ctx, "regexp.uni",   "/\\p{Letter}+/u.test('caf\\u00e9')");

    /* Date over the UTC-only RTC surface (stub: 2026-08-05T12:00:00Z) */
    check(ctx, "date.iso",     "new Date(0).toISOString() === '1970-01-01T00:00:00.000Z'");
    check(ctx, "date.utc",     "Date.UTC(2026, 7, 5, 12, 0, 0) === 1785931200000");
    check(ctx, "date.tz0",     "new Date().getTimezoneOffset() === 0");
    check(ctx, "date.now",     "Math.abs(Date.now() - 1785931200000) < 86400000");
    check(ctx, "date.parts",   "(d => d.getUTCFullYear() === 2026 && d.getUTCMonth() === 7 && d.getUTCDay() === 3)(new Date())");

    /* bigint, typed arrays (the lrint path is Uint8ClampedArray) */
    check(ctx, "bigint",       "(2n ** 100n).toString() === '1267650600228229401496703205376'");
    check(ctx, "u8clamped",    "(a => { a[0] = 2.5; a[1] = 3.5; a[2] = -1; a[3] = 300; return a[0] === 2 && a[1] === 4 && a[2] === 0 && a[3] === 255; })(new Uint8ClampedArray(4))");

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return g_fail != 0;
}
