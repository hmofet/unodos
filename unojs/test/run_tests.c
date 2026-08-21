/* ===========================================================================
 * unojs host test runner. Builds with plain gcc, no OS, no web code - which
 * is itself part of what is being tested: unojs must stand alone.
 *
 *   make && ./run_tests          run everything
 *   ./run_tests <substring>      run only matching cases
 * ======================================================================== */
#include "../unojs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char g_out[65536];
static int  g_outn;

static void emit(const char *s, int n)
{ if (g_outn + n < (int)sizeof g_out - 1) { memcpy(g_out + g_outn, s, (size_t)n); g_outn += n; }
  g_out[g_outn] = 0; }

/* print(x, ...) - the tests' only channel to the outside. Deliberately a HOST
 * function: unojs itself has no console, by design. */
static ujs_val h_print(ujs_args *a)
{
    int i;
    for (i = 0; i < a->argc; i++) {
        ujs_val s;
        const char *b;
        size_t n;
        if (i) emit(" ", 1);
        if (ujs_to_string(a->vm, a->argv[i], &s) != UJS_OK) return ujs_undefined();
        b = ujs_string_bytes(a->vm, s, &n);
        if (b) emit(b, (int)n);
    }
    emit("\n", 1);
    return ujs_undefined();
}

typedef struct { const char *name, *src, *want; } tcase;

static const tcase cases[] = {

/* ---- literals, numbers, the formatter ---------------------------------- */
{ "num-basic", "print(1); print(2+3*4); print(10/4); print(7%3);", "1\n14\n2.5\n1\n" },
{ "num-format", "print(0.1); print(0.1+0.2); print(1e21); print(1/3);",
  "0.1\n0.30000000000000004\n1e+21\n0.3333333333333333\n" },
{ "num-special", "print(1/0); print(-1/0); print(0/0); print(-0);",
  "Infinity\n-Infinity\nNaN\n0\n" },
{ "num-int", "print(9007199254740991); print(-42); print(1e3);",
  "9007199254740991\n-42\n1000\n" },
{ "num-hex", "print(0xff); print(0b1010); print(0o17);", "255\n10\n15\n" },
/* Shortest round-trip formatting, verified against V8. These are the cases
 * that broke while digits were extracted in double arithmetic: a 17-digit
 * value exceeds 2^53, so the scaling snapped to the neighbouring integer and
 * sqrt(2) printed as ...952. Extraction is exact integer arithmetic now and
 * these lock it down. */
{ "num-roundtrip",
  "print(Math.sqrt(2)); print(1/3); print(2/3); print(0.1+0.2); print(1e-7);"
  "print(Math.PI); print(5e-324); print(123456789012345678); print(1e21); print(1e-21);",
  "1.4142135623730951\n0.3333333333333333\n0.6666666666666666\n"
  "0.30000000000000004\n1e-7\n3.141592653589793\n5e-324\n123456789012345680\n"
  "1e+21\n1e-21\n" },
{ "num-parse-exact",
  /* adjacent doubles must NOT collapse onto one value when parsed */
  "print(1.4142135623730951 === 1.4142135623730952);"
  "print(0.1 + 0.2 === 0.3); print(9007199254740993 === 9007199254740992);",
  "true\nfalse\ntrue\n" },

/* ---- strings ------------------------------------------------------------ */
{ "str-cat", "print('a'+'b'); print('n='+5); print(1+'2');", "ab\nn=5\n12\n" },
{ "str-esc", "print('a\\tb'); print(\"q\\\"q\"); print('\\u0041');", "a\tb\nq\"q\nA\n" },
{ "str-methods",
  "var s='Hello World';"
  "print(s.length); print(s.toUpperCase()); print(s.indexOf('World'));"
  "print(s.slice(0,5)); print(s.split(' ').join('-')); print('  x '.trim());",
  "11\nHELLO WORLD\n6\nHello\nHello-World\nx\n" },
{ "str-index", "var s='abc'; print(s[0]); print(s[2]); print(s.charAt(1));", "a\nc\nb\n" },
{ "str-indexof-from",
  /* the second argument is where a SCANNER lives: i = s.indexOf(x, i) + 1
   * must advance, or every loop written that way spins for ever */
  "var s='a.b.c';"
  "print(s.indexOf('.', 2)); print(s.indexOf('.', 4)); print(s.indexOf('.', 99));"
  "print(s.indexOf('', 3)); print([7,8,7].indexOf(7, 1));",
  "3\n-1\n-1\n3\n2\n" },
{ "str-replace", "print('a-b-c'.replace('-','+')); print('xy'.repeat(3));", "a+b-c\nxyxyxy\n" },

/* ---- operators ---------------------------------------------------------- */
{ "ops-cmp", "print(1<2); print('a'<'b'); print(2=='2'); print(2==='2'); print(null==undefined);",
  "true\ntrue\ntrue\nfalse\ntrue\n" },
{ "ops-logic", "print(1&&2); print(0||'x'); print(!0); print(!'');", "2\nx\ntrue\ntrue\n" },
{ "ops-bit", "print(5&3); print(5|3); print(5^3); print(~5); print(1<<4); print(-8>>1); print(-1>>>28);",
  "1\n7\n6\n-6\n16\n-4\n15\n" },
{ "ops-typeof", "print(typeof 1); print(typeof 'a'); print(typeof undefined);"
                "print(typeof null); print(typeof {}); print(typeof print);",
  "number\nstring\nundefined\nobject\nobject\nfunction\n" },
{ "ops-ternary", "print(1?'y':'n'); print(0?'y':'n');", "y\nn\n" },

/* ---- variables + control flow ------------------------------------------ */
{ "var-assign", "var a=1; a+=2; a*=3; print(a); var b=10; b-=4; print(b);", "9\n6\n" },
{ "incdec", "var i=0; print(i++); print(i); print(++i); print(i--); print(i);",
  "0\n1\n2\n2\n1\n" },
{ "if-else", "var x=5; if(x>3) print('big'); else print('small');"
             "if(x>10) print('no'); else if(x>4) print('mid');", "big\nmid\n" },
{ "while", "var i=0,s=0; while(i<5){s+=i;i++;} print(s);", "10\n" },
{ "do-while", "var i=0; do{ i++; }while(i<3); print(i);", "3\n" },
{ "for", "var s=''; for(var i=0;i<4;i++) s+=i; print(s);", "0123\n" },
{ "for-expr-init",
  /* a PLAIN ASSIGNMENT in the head, no var: the for-in probe's rewind used
   * to skip the first token, handing the expression parser `= 0` */
  "var s='', j; for (j = 0; j < 3; j++) s += j; print(s); print(j);",
  "012\n3\n" },
{ "for-empty-head", "var n=0; for(;;){ n++; if(n===3) break; } print(n);", "3\n" },
{ "for-var-is-local",
  /* the same rewind bug dropped `var`, so a loop variable inside a function
   * silently assigned a GLOBAL - correct output, wrong scope */
  "var i='outer'; function f(){ for(var i=0;i<2;i++){} } f(); print(i);",
  "outer\n" },
{ "break-continue",
  "var s=''; for(var i=0;i<10;i++){ if(i%2==0) continue; if(i>6) break; s+=i; } print(s);",
  "135\n" },
{ "nested-loops",
  "var s=''; for(var i=0;i<3;i++){ for(var j=0;j<3;j++){ if(j==1) continue; s+=i+''+j+' '; } } print(s);",
  "00 02 10 12 20 22 \n" },

/* ---- switch, including fallthrough ------------------------------------- */
{ "switch", "function f(x){ var r=''; switch(x){ case 1: r+='one'; break;"
            "case 2: r+='two'; case 3: r+='three'; break; default: r+='other'; } return r; }"
            "print(f(1)); print(f(2)); print(f(3)); print(f(9));",
  "one\ntwothree\nthree\nother\n" },

/* ---- functions, closures, recursion ------------------------------------ */
{ "fn-basic", "function add(a,b){ return a+b; } print(add(2,3)); print(add(2));",
  "5\nNaN\n" },
{ "fn-recursion", "function fib(n){ return n<2 ? n : fib(n-1)+fib(n-2); } print(fib(10));",
  "55\n" },
{ "fn-closure",
  "function counter(){ var n=0; return function(){ n++; return n; }; }"
  "var c=counter(); c(); c(); print(c()); var d=counter(); print(d());",
  "3\n1\n" },
{ "fn-expr", "var f=function(x){return x*2;}; print(f(21));", "42\n" },
{ "fn-arrow", "var f=x=>x*3; print(f(5)); var g=(a,b)=>a+b; print(g(1,2));"
              "var h=(x)=>{ return x+1; }; print(h(9));", "15\n3\n10\n" },
{ "fn-hoist", "print(typeof later); function later(){ return 1; }", "function\n" },
{ "fn-callback", "function apply2(f,v){ return f(v); } print(apply2(function(x){return x+1;}, 41));",
  "42\n" },

/* ---- objects ------------------------------------------------------------ */
{ "obj-literal", "var o={a:1,b:'two',c:true}; print(o.a); print(o.b); print(o['c']);",
  "1\ntwo\ntrue\n" },
{ "obj-assign", "var o={}; o.x=1; o['y']=2; print(o.x+o.y); o.x+=10; print(o.x);", "3\n11\n" },
{ "obj-nested", "var o={p:{q:{r:7}}}; print(o.p.q.r); o.p.q.r=8; print(o.p.q.r);", "7\n8\n" },
{ "obj-method", "var o={n:5, get:function(){ return this.n; }}; print(o.get());", "5\n" },
{ "obj-keys", "var o={a:1,b:2}; print(Object.keys(o).join(',')); print(JSON.stringify(o));",
  "a,b\n{\"a\":1,\"b\":2}\n" },
{ "obj-delete", "var o={a:1,b:2}; delete o.a; print(Object.keys(o).join(',')); print(o.a);",
  "b\nundefined\n" },
{ "obj-in", "var o={a:1}; print('a' in o); print('z' in o);", "true\nfalse\n" },

/* ---- arrays ------------------------------------------------------------- */
{ "arr-basic", "var a=[1,2,3]; print(a.length); print(a[0]); a[3]=4; print(a.length); print(a.join('-'));",
  "3\n1\n4\n1-2-3-4\n" },
{ "arr-stack", "var a=[]; a.push(1); a.push(2,3); print(a.join()); print(a.pop()); print(a.join());",
  "1,2,3\n3\n1,2\n" },
{ "arr-hof",
  "var a=[1,2,3,4];"
  "print(a.map(function(x){return x*2;}).join(','));"
  "print(a.filter(function(x){return x%2==0;}).join(','));"
  "print(a.reduce(function(s,x){return s+x;},0));",
  "2,4,6,8\n2,4\n10\n" },
{ "arr-sort", "var a=[3,1,2]; print(a.sort(function(x,y){return x-y;}).join(','));"
              "print([10,9,1].sort().join(','));", "1,2,3\n1,10,9\n" },
{ "arr-misc", "var a=[1,2,3]; print(a.slice(1).join()); print(a.concat([4]).join());"
              "print(a.indexOf(2)); print(a.reverse().join()); print(Array.isArray(a));",
  "2,3\n1,2,3,4\n1\n3,2,1\ntrue\n" },
{ "arr-nested", "var m=[[1,2],[3,4]]; print(m[1][0]); print(JSON.stringify(m));",
  "3\n[[1,2],[3,4]]\n" },

/* ---- this, new, prototypes ---------------------------------------------- */
{ "new-ctor",
  "function P(n){ this.n=n; } P.prototype.greet=function(){ return 'hi '+this.n; };"
  "var p=new P('bob'); print(p.n); print(p.greet()); print(p instanceof P);",
  "bob\nhi bob\ntrue\n" },
{ "proto-chain",
  "function A(){} A.prototype.x=function(){return 'A';};"
  "var a=new A(); print(a.x()); print(typeof a.nope);", "A\nundefined\n" },

/* ---- exceptions --------------------------------------------------------- */
{ "try-catch", "try { throw 'boom'; } catch(e) { print('caught '+e); } print('after');",
  "caught boom\nafter\n" },
{ "try-finally", "function f(){ try { return 'r'; } finally { print('fin'); } }"
                 "print(f());", "fin\nr\n" },
{ "throw-error", "try { null.x; } catch(e) { print(e.name); } print('ok');",
  "TypeError\nok\n" },
{ "throw-object", "try { throw new Error('msg'); } catch(e){ print(e.message); print(e.name); }",
  "msg\nError\n" },
{ "catch-nested",
  "function f(){ try { g(); } catch(e){ return 'inner:'+e; } } function g(){ throw 'x'; }"
  "print(f());", "inner:x\n" },
{ "ref-error", "try { nosuchvar; } catch(e){ print(e.name); }", "ReferenceError\n" },

/* ---- iteration ---------------------------------------------------------- */
{ "for-in-obj", "var o={a:1,b:2}; var s=''; for(var k in o) s+=k+'='+o[k]+' '; print(s);",
  "a=1 b=2 \n" },
{ "for-in-arr", "var a=['x','y']; var s=''; for(var i in a) s+=i+':'+a[i]+' '; print(s);",
  "0:x 1:y \n" },
{ "for-of", "var s=''; for(var v of [10,20,30]) s+=v+' '; print(s);", "10 20 30 \n" },

/* ---- builtins ----------------------------------------------------------- */
{ "math", "print(Math.floor(3.7)); print(Math.ceil(3.2)); print(Math.round(2.5));"
          "print(Math.abs(-4)); print(Math.sqrt(16)); print(Math.max(1,9,3)); print(Math.min(1,9,3));"
          "print(Math.pow(2,10));",
  "3\n4\n3\n4\n4\n9\n1\n1024\n" },
{ "parse", "print(parseInt('42')); print(parseInt('0x1f')); print(parseFloat('3.14abc'));"
           "print(isNaN(parseInt('zz')));", "42\n31\n3.14\ntrue\n" },
{ "json", "print(JSON.stringify({a:[1,'s',null],b:{c:true}}));",
  "{\"a\":[1,\"s\",null],\"b\":{\"c\":true}}\n" },
{ "number-fmt", "print((3.14159).toFixed(2)); print((255).toString(16)); print((0.5).toFixed(0));",
  "3.14\nff\n1\n" },
{ "string-ctor", "print(String(42)); print(Number('17')); print(Boolean(0));", "42\n17\nfalse\n" },
{ "fn-call-apply",
  "function f(a,b){ return this.p+a+b; } var o={p:1};"
  "print(f.call(o,2,3)); print(f.apply(o,[2,3]));", "6\n6\n" },

/* ---- integration -------------------------------------------------------- */
{ "integration-1",
  "function Stack(){ this.items=[]; }"
  "Stack.prototype.push=function(v){ this.items.push(v); return this; };"
  "Stack.prototype.sum=function(){ return this.items.reduce(function(a,b){return a+b;},0); };"
  "var s=new Stack(); s.push(1).push(2).push(3); print(s.sum()); print(s.items.length);",
  "6\n3\n" },
{ "integration-2",
  "var words='the quick brown fox'.split(' ');"
  "var byLen={};"
  "words.forEach(function(w){ var k=w.length; if(!byLen[k]) byLen[k]=[]; byLen[k].push(w); });"
  "print(JSON.stringify(byLen[3])); print(Object.keys(byLen).sort().join(','));",
  "[\"the\",\"fox\"]\n3,5\n" },
{ "integration-3",
  "function memo(f){ var c={}; return function(n){ if(n in c) return c[n];"
  "var r=f(n); c[n]=r; return r; }; }"
  "var calls=0; var slow=function(n){ calls++; return n*n; }; var fast=memo(slow);"
  "fast(4); fast(4); fast(5); print(calls); print(fast(4));", "2\n16\n" },
};

/* ---- extra checks that need the C API, not just script ------------------ */
static int api_tests(int *nrun)
{
    int fails = 0;
    ujs_vm *vm;
    ujs_val v;
    char buf[64];

    /* host objects + the embedder API */
    (*nrun)++;
    vm = ujs_new(NULL);
    {   ujs_val g = ujs_global(vm);
        int dummy = 7;
        ujs_set(vm, g, "hostval", ujs_number(99));
        ujs_set(vm, g, "hostobj", ujs_host_new(vm, &dummy, NULL));
        if (ujs_eval(vm, "hostval + 1", -1, &v) != UJS_OK || ujs_to_number(vm, v) != 100) {
            printf("  FAIL api/host-value\n"); fails++;
        }
        if (ujs_eval(vm, "typeof hostobj", -1, &v) != UJS_OK) { printf("  FAIL api/host-obj\n"); fails++; }
    }
    ujs_free(vm);

    /* a syntax error must report, not crash */
    (*nrun)++;
    vm = ujs_new(NULL);
    if (ujs_eval(vm, "function ( {", -1, &v) != UJS_SYNTAX) { printf("  FAIL api/syntax\n"); fails++; }
    ujs_clear_exception(vm);
    if (ujs_eval(vm, "1+1", -1, &v) != UJS_OK || ujs_to_number(vm, v) != 2) {
        printf("  FAIL api/recover-after-syntax\n"); fails++;
    }
    ujs_free(vm);

    /* FUEL: an infinite loop must yield, repeatedly, and never hang */
    (*nrun)++;
    {   ujs_config cfg;
        int slices = 0;
        ujs_result r;
        memset(&cfg, 0, sizeof cfg);
        cfg.fuel_per_slice = 2000;
        vm = ujs_new(&cfg);
        r = ujs_eval(vm, "var i=0; while(true){ i++; }", -1, &v);
        while (r == UJS_YIELD && slices < 50) { r = ujs_resume(vm, &v); slices++; }
        if (r != UJS_YIELD || slices < 10) {
            printf("  FAIL api/fuel-yield (r=%d slices=%d)\n", (int)r, slices); fails++;
        }
        ujs_free(vm);
    }

    /* FUEL: a cumulative budget kills the script instead of yielding forever */
    (*nrun)++;
    {   ujs_config cfg;
        ujs_result r;
        int guard = 0;
        memset(&cfg, 0, sizeof cfg);
        cfg.fuel_per_slice = 500;
        cfg.fuel_total = 5000;
        vm = ujs_new(&cfg);
        r = ujs_eval(vm, "while(true){}", -1, &v);
        while (r == UJS_YIELD && guard++ < 100) r = ujs_resume(vm, &v);
        if (r != UJS_THROW) { printf("  FAIL api/fuel-total (r=%d)\n", (int)r); fails++; }
        ujs_free(vm);
    }

    /* HEAP CAP: allocation past the ceiling raises, and does not grow the heap */
    (*nrun)++;
    {   ujs_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.heap_max = 1 << 20;
        vm = ujs_new(&cfg);
        ujs_eval(vm, "var a=[]; for(var i=0;i<200000;i++) a.push('xxxxxxxxxxxxxxxxxxxx'+i);", -1, &v);
        if (ujs_heap_used(vm) > cfg.heap_max) {
            printf("  FAIL api/heap-cap (used=%lu)\n", (unsigned long)ujs_heap_used(vm)); fails++;
        }
        ujs_free(vm);
    }

    /* GC: a big churn of garbage must not grow the heap without bound, and the
     * program must keep running correctly across collections */
    (*nrun)++;
    vm = ujs_new(NULL);
    if (ujs_eval(vm,
        "function make(n){ var o={v:n, s:'pad pad pad pad'+n, a:[n,n+1,n+2]}; return o.v; }"
        "var t=0; for(var i=0;i<20000;i++) t+=make(i);"
        "t", -1, &v) != UJS_OK) {
        printf("  FAIL api/gc-churn (exception)\n"); fails++;
    } else if (ujs_to_number(vm, v) != 199990000.0) {
        printf("  FAIL api/gc-churn value=%s\n", ujs_describe(vm, v, buf, sizeof buf)); fails++;
    } else if (ujs_heap_used(vm) > (4u << 20)) {
        printf("  FAIL api/gc-churn heap=%lu\n", (unsigned long)ujs_heap_used(vm)); fails++;
    }
    ujs_free(vm);

    /* deep recursion must throw RangeError, not smash the C stack */
    (*nrun)++;
    vm = ujs_new(NULL);
    if (ujs_eval(vm, "function r(n){ return r(n+1); } r(0);", -1, &v) != UJS_THROW) {
        printf("  FAIL api/deep-recursion\n"); fails++;
    }
    ujs_free(vm);

    return fails;
}

int main(int argc, char **argv)
{
    int i, n = (int)(sizeof cases / sizeof cases[0]);
    int fails = 0, run = 0;
    const char *filter = argc > 1 ? argv[1] : NULL;

    for (i = 0; i < n; i++) {
        ujs_vm *vm;
        ujs_val v;
        ujs_result r;
        if (filter && !strstr(cases[i].name, filter)) continue;
        run++;
        g_outn = 0; g_out[0] = 0;
        vm = ujs_new(NULL);
        if (!vm) { printf("  FAIL %s: no vm\n", cases[i].name); fails++; continue; }
        ujs_set_fn(vm, ujs_global(vm), "print", h_print, 1);
        r = ujs_eval(vm, cases[i].src, -1, &v);
        if (r != UJS_OK) {
            char b[192];
            printf("  FAIL %-16s rc=%d exc=%s\n", cases[i].name, (int)r,
                   ujs_describe(vm, ujs_exception(vm), b, sizeof b));
            if (ujs_is_object(ujs_exception(vm))) {
                ujs_val m;
                if (ujs_get(vm, ujs_exception(vm), "message", &m) == UJS_OK) {
                    size_t ml; const char *mb = ujs_string_bytes(vm, m, &ml);
                    if (mb) printf("                   message: %s\n", mb);
                }
            }
            printf("                   output so far: %s\n", g_out);
            fails++;
            ujs_free(vm);
            continue;
        }
        if (strcmp(g_out, cases[i].want)) {
            printf("  FAIL %-16s\n    want: %s    got:  %s", cases[i].name, cases[i].want, g_out);
            if (g_outn && g_out[g_outn-1] != '\n') printf("\n");
            fails++;
        }
        ujs_free(vm);
    }

    if (!filter) fails += api_tests(&run);

    printf("%s: %d cases, %d failures\n", fails ? "FAIL" : "PASS", run, fails);
    return fails ? 1 : 0;
}
