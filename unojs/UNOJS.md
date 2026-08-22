# unojs, subsystem contract

**Owner:** the unojs row in [`/AGENTS.md`](../AGENTS.md) §1.
**Public surface:** [`unojs.h`](unojs.h). Everything else in this directory is
internal and may change without notice.
**Contract version:** 0.1, `[EXPERIMENTAL]` until M1 lands on `master`.

unojs is a standalone JavaScript engine: a bytecode VM with a garbage-collected
heap and fuel-based preemption. It is the JS half of the engine split described
in [`docs/WEB-ENGINE-DESIGN.md`](../docs/WEB-ENGINE-DESIGN.md); `unoweb` (DOM /
HTML / CSS) is the other half and lands in M2.

## The rule that defines this subsystem

**unojs knows nothing about its consumers.** No HTML, no DOM, no CSS, no
`console`, no timers, no file system, no network. `unojs.h` contains no web
vocabulary, and the test suite builds and passes with nothing linked but a
minimal libc - not even libm.

An embedder projects its own world in through `ujs_host_new`, `ujs_function_new`
and `ujs_set_fn`. That is how `document` and `window` will be built in
`pc64/webjs.c` (M5) without unojs ever learning what a DOM node is. If you find
yourself wanting to add a web-shaped call to `unojs.h`, that is the signal that
it belongs in the binding layer instead.

## Building

Seven `.c` files, C99. No libc beyond `<string.h>` and `<stdio.h>` (`snprintf`
for diagnostics), and **no libm**: unojs carries its own double-precision math
in `ujs_math.c`, because pc64's is float-only and the number formatter needs
double exactness. One implementation, identical on the host tests and on metal.

```
unojs/ujs_core.c ujs_math.c ujs_lex.c ujs_comp.c ujs_vm.c ujs_lib.c ujs_api.c
```

It does need the **compiler runtime** (`-lgcc` on the pc64 link): the decimal
conversions use `__int128`, which lowers to `__udivti3` / `__floatuntidf`.
PYRT links it for the same reason.

### `ujs_math.h`, the double-math surface: **[STABLE]** as of 2026-08-06

That mini-libm is now a **public** surface, not an unojs internal. The fourteen
functions `ujs_fabs floor ceil fmod sqrt pow exp log log10 sin cos tan atan
atan2` are declared in [`ujs_math.h`](ujs_math.h) and any freestanding consumer
in the tree may include it. Blessing it was a request from the quickjs port
(`pc64/quickjs/compat/math.h`), which needed real doubles for the same reason
unojs does and had been hand-declaring the symbols against `ujs_int.h`, a
header it has no business reading.

The contract, and its limit: each has the semantics of its C99 namesake for
finite inputs including the sign and NaN/infinity cases ECMAScript's `Math`
requires, and the accuracy is "good enough for JS number formatting", not
correctly-rounded. Under `UJS_USE_LIBM` (the host tests) they fold to the
platform's libm, so include the header rather than declaring the symbols and
you get the right one in both worlds. Breaking any of this bumps a version
marker here per AGENTS.md §6.

Host tests:

```bash
cd unojs/test && make test
```

## The one rule an embedder must not break

Every `ujs_val` the embedder holds is GC-visible **only** while inside an open
handle scope, or while explicitly rooted:

```c
ujs_scope s;
ujs_scope_open(vm, &s);
ujs_val o = ujs_object_new(vm);
/* ... build it up, calling into the VM freely ... */
kept = ujs_scope_close(vm, &s, o);     /* o survives; the rest is garbage */
```

A raw `ujs_val` held across an allocation with no scope may be collected. The
collector is non-moving, so pointers stay valid, but a value that nothing
roots is *freed*.

This is not a theoretical hazard. The engine's own VM had this bug in every
arithmetic, comparison and property opcode: operands were popped off the
operand stack (which is precisely the root set) before conversions that
allocate. The tests all passed; AddressSanitizer found it. Treat the rule as
load-bearing.

## Execution model

`ujs_eval` compiles and runs a script. It returns:

| Result | Meaning |
|---|---|
| `UJS_OK` | finished; `*out` is the completion value (the last expression statement's value) |
| `UJS_SYNTAX` | compile failed; `ujs_exception()` describes it |
| `UJS_THROW` | an uncaught exception; `ujs_exception()` has it |
| `UJS_YIELD` | the fuel slice ran out mid-run; call `ujs_resume()` |
| `UJS_OOM` | the heap ceiling was hit and collection could not free enough |

### Jobs, and where an `await` continues (UCD-21)

`ujs_run_jobs(vm)` drains the microtask queue: every Promise reaction and every
suspended `await` whose value has arrived. `ujs_eval` and `ujs_resume` call it
before they return, so a script is finished when they are.

**A host that settles promises from C must call it once per frame.** That call
is where the continuation of every `await` in an embedder actually runs - a
dialog closing, a request completing, a file arriving. Without it those
continuations are queued and never run - the promise is settled and the code
after the `await` simply never runs, which looks like a hang with no error.

It is bounded per call and refuses re-entry: a promise chain that queues itself
for ever slows down rather than taking the machine with it.

```c
ujs_val p = ujs_promise(vm);          /* hand this to JS                    */
...
ujs_promise_resolve(vm, p, value);    /* later; reactions run on the next   */
ujs_run_jobs(vm);                     /* ...call to this                    */
```

`ujs_function_set_data()` binds private state to a C function, arriving as
`a->data`. A C function has no closure, and `self` is whatever the call site
passed - which is `undefined` for a plain `f(x)`, and is why the `resolve` and
`reject` pair handed to a `new Promise` executor needs it.

### Fuel, why it exists

UnoDOS runs page scripts in ring 0 on a single-threaded OS. A `while(1)` in an
untrusted page must not be able to wedge the desktop. The VM therefore charges
fuel on every branch, call and loop back-edge; when a slice is exhausted it
saves nothing to the C stack (all state lives in `ujs_vm`) and returns
`UJS_YIELD`. The host resumes it on the next frame tick.

```c
ujs_config cfg = {0};
cfg.fuel_per_slice = 20000;      /* ~a frame's worth */
cfg.fuel_total     = 5000000;    /* then "script ran too long" */
```

`fuel_per_slice = 0` means unlimited, which is right for host tools and **wrong
for page scripts**. A script that exceeds `fuel_total` is killed with a
catchable `Error`.

Caveat: the VM cannot yield while a host C function is on the stack (there is a
live C frame to preserve). Such runs keep going instead of yielding; keep host
callbacks short.

### Memory

| Knob | Default | Behaviour at the limit |
|---|---|---|
| `heap_max` | 8 MB | collect, then raise a JS `RangeError`: never grow |
| call depth | 256 frames | `RangeError: maximum call depth exceeded` |
| operand stack | 4096 slots | `RangeError: stack overflow` |

No allocation in the engine is unbounded.

## Language coverage (v0.1)

**Supported.** All statements and operators; `var`/`let`/`const`; closures and
recursion; function declarations (hoisted, value and all) and expressions;
arrow functions (concise and block bodies); objects and object literals;
arrays and array literals; property/index access and assignment, including
compound (`o.k += v`, `a[i] *= 2`); `this`; `new`, prototypes, `instanceof`;
getters/setters; `try`/`catch`/`throw`/`finally`, including `finally` running
on the `return` path; `switch` with fallthrough; `for`, `for-in`, `for-of`,
`while`, `do-while`, `break`, `continue`; `typeof` (including on undeclared
names); template literals **without** interpolation; `Object`, `Array`,
`String`, `Number`, `Boolean`, `Math`, `JSON.stringify`, `Error`/`TypeError`/
`RangeError`, `parseInt`/`parseFloat`/`isNaN`/`isFinite`.

**Known gaps, the honest list.** These are deliberate v0.1 omissions, each
scheduled or explicitly out of scope. None of them is a silent failure: they
either work in a documented reduced way or raise a clear error.

1. **Block scoping.** `let`/`const` are function-scoped; there is no TDZ and no
   per-iteration binding for `for (let i ...)`. A closure made inside such a
   loop sees the final value. **M1b.**
2. **`RegExp`.** Not implemented; a `/re/` literal is a syntax error and
   `String.prototype.replace` takes string patterns only. **M1c.**
3. **Template interpolation.** `` `a${b}` `` lexes as a plain string; the
   `${...}` is not substituted. **M1b.**
4. ~~**`Promise` / microtasks / `async`.** Absent.~~ **DONE (UCD-21.)**
   `Promise` with `.then`/`.catch`/`.finally` and `resolve`/`reject`/`all`, a
   microtask queue drained by `ujs_run_jobs()`, and `async`/`await` on a real
   suspension: an `await` lifts its function's frames, stack slice and
   handlers out of the machine and splices them back when the awaited value
   settles, so the call returns its promise at the moment it suspends.
   Two things to know:
   - **`async` and `await` are KEYWORDS here, not contextual ones**, so a
     script using either as a variable name will not compile. Real code does
     not, and a contextual keyword costs a lookahead on every identifier.
   - `.then`/`.catch`/`.finally` live on `Object.prototype`, because a promise
     is a plain object in this engine. Nothing else defines those names.
5. **Named function *expressions*** cannot refer to themselves by name
   (`var g = function f(){ f(); }`). Function *declarations* recurse normally,
   because their name is a binding in the enclosing scope.
6. **`arguments`**, spread/rest, destructuring, default parameters, classes,
   generators: absent. **M1b/M1c.**
7. **Atoms are never collected.** Property-name strings live for the VM's
   lifetime. Integer-like keys bypass atoms entirely (they become dense array
   elements), which bounds the common patterns; a script that manufactures
   unbounded *distinct string* keys grows the atom table until `heap_max` stops
   it. Acceptable because a document's VM is destroyed on navigation.
8. **One `finally` slot.** A `finally` nested inside another `finally`'s
   exception or return path loses the outer pending completion. Rare; a real
   completion stack is M1b.
9. **`ToPrimitive` is simplified.** An object operand of `+` stringifies rather
   than consulting `valueOf` first in every spec-mandated order.
10. **No hidden classes / inline caches.** Property lookup is a linear scan of
    a small dense array. Correctness first; measure before optimizing.
11. **Environment per call.** Every call heap-allocates its scope, even when
    nothing captures it. Escape analysis to keep non-capturing locals on the
    stack is the first planned perf change.
12. **Transcendental accuracy is ~1ulp**, not correctly rounded. `Math.exp(1)`
    and `Math.log(10)` differ from glibc/V8 in the last bit, so they print with
    an extra digit (`2.7182818284590446` where V8 says `2.718281828459045`).
    The algebraic operations, `Math.sqrt`, integer `Math.pow` and all decimal
    conversion ARE exact - those are what page arithmetic and number printing
    depend on. Correctly-rounded transcendentals are a large lift for
    essentially no browser value; if it ever matters, `UJS_USE_LIBM` switches
    to the host's.
13. **Decimal exponents beyond +-22** lose a few ulp on parse (the 128-bit
    exact path only covers that range; past it the value is built from chunked
    powers of ten). `1.7976931348623157e308` reads back as
    `1.7976931348623139e308`. Ordinary page numbers are unaffected.

## Deliberate deviations from the design doc

`docs/WEB-ENGINE-DESIGN.md` §3.1 sketched `source → AST → bytecode`. There is
**no AST**: the compiler emits bytecode in one pass, with a token-level
pre-scan that hoists `var` and `function` declarations. Rationale: nothing in
v1 consumes a tree (no optimizer, no source maps), an AST would add several
thousand lines and a second traversal, and the pre-scan handles the one thing
single-pass compilers classically get wrong. If an optimizer or source-map
consumer ever arrives, an AST goes in then.

## Testing

`unojs/test/` builds with plain gcc and is the merge gate for this subsystem:

- 71 script cases covering the language and library, each asserting exact output.
- C-API cases: host values/objects, syntax-error recovery, fuel yielding, the
  cumulative fuel kill, the heap ceiling, GC churn under 20 000 allocations,
  and deep recursion raising rather than smashing the C stack.

**Sanitizers are part of the gate**, not an optional extra:

```bash
gcc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
    -o /tmp/ujs_asan test/run_tests.c ujs_*.c -lm && /tmp/ujs_asan
```

The collector is hand-written and non-moving, so use-after-free and leaks are
real defects that ordinary passing tests will not reveal. `make valgrind` is
equivalent where valgrind is available.

## Numeric conformance

Number printing is the part of a JS engine users see most directly, so it is
held to the shortest-round-trip rule and checked against V8. `num-roundtrip`
and `num-parse-exact` in the test suite pin these:

| value | unojs | V8 |
|---|---|---|
| `Math.sqrt(2)` | `1.4142135623730951` | same |
| `1/3` | `0.3333333333333333` | same |
| `0.1+0.2` | `0.30000000000000004` | same |
| `1e-7` | `1e-7` | same |
| `Math.PI` | `3.141592653589793` | same |
| `5e-324` | `5e-324` | same |
| `123456789012345678` | `123456789012345680` | same |

Getting there took three real bugs worth recording, because each is a trap the
obvious implementation falls into:

1. **Digit extraction must be exact.** A 17-significant-digit value exceeds
   2^53, so scaling in double arithmetic snaps it to its neighbour *before*
   rounding: `sqrt(2)` printed as `1.4142135623730952`. Digits now come from
   `m * 10^p * 2^e` evaluated in 128-bit integers.
2. **Parsing must not collapse adjacent doubles.** `(double)mantissa * 10^e`
   loses the low bit of a >2^53 mantissa, so two neighbouring doubles parsed
   to the same value - which silently breaks the shortest-round-trip search,
   since it uses the parser as its oracle.
3. **128-bit division must be normalized first.** Shifting a fixed 64 places
   gives 64 guard bits only if the mantissa is already wide; for `1e-7`
   (mantissa 1) the quotient had ~41 bits and the value came back as
   `9.999999999994822e-8`.

## Changelog

- **0.2** (2026-08-21) Promises, a microtask queue, and `async`/`await`
  (UCD-21). `await` is a real suspension: the async function's frames, its
  slice of the value stack and its handlers are lifted out of the VM and
  spliced back when the awaited value settles. C functions gained bound data
  (`ujs_function_set_data`), and `ujs_call_value` now unwinds the frames it
  pushed when an exception escapes - invisible while every caller re-threw,
  and corrupting the moment one swallowed it, as a Promise executor must.
- **0.1** (2026-07-27), first cut. Engine core, library, host test suite.
  Replaces `pc64/js.c` (a 577-line tree-walking subset) via a `js_run()`
  compatibility shim. `[EXPERIMENTAL]`: the surface may still move before M1
  lands on `master`.
