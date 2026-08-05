# pc64/quickjs - the vendored QuickJS engine + its UnoDOS port layer

The browser's optional second JavaScript engine (see
`docs/BROWSER-ENGINE2-PLAN.md`; the seam is `js_run()` in `../js.h`, default
engine stays unojs). Layout: vendored files at this level, the port layer in
`compat/` + `qjs_port.c`, host smoke test in `test/`.

## Provenance

- Upstream: quickjs-ng, https://github.com/quickjs-ng/quickjs
- Version: **v0.16.1** (commit `954dc53628e36891f93c359aa60895c2ae3dac6b`)
- License: MIT (`LICENSE` here, shipped verbatim; surface it in the About
  dialog + `DOCS\LICENSES.MD` roster when the engine ships in a build, same
  as the unomedia codec licences)
- Vendored subset: the engine core only - `quickjs.[ch]`,
  `quickjs-atom.h`, `quickjs-opcode.h`, `quickjs-c-atomics.h`, `cutils.h`,
  `libregexp.*`, `libunicode.*`, `dtoa.*`, `list.h`, `builtin-*.h`.
  NOT vendored: `quickjs-libc.*` (POSIX/worker host library - qjsweb.c is
  our host layer), the CLI (`qjs.c`, `qjsc.c`), and the test runners.
- Vendored files are UNMODIFIED. Every port decision lives in the build
  flags and `compat/`; an upstream bump is copy-over + rerun the tests.

## How it builds (the port contract)

Compiled with the kernel's freestanding flags plus:

    -Iquickjs/compat (FIRST)  -D__wasi__  -U_WIN32 -U_WIN64
    -U__MINGW32__ -U__MINGW64__  -Dalloca=__builtin_alloca
    -DJS_NAN_BOXING=0  -w

- `__wasi__` is quickjs-ng's "single-threaded, no OS" personality: no
  pthreads, no stack-limit probing, plain gettimeofday/clock_gettime. It is
  exactly what pc64 is, so the vendor code needs no edits.
- The `-U`s hide mingw's identity: otherwise quickjs takes the `_WIN32`
  paths (winsock2, `_beginthread`, QueryPerformanceCounter).
- `JS_NAN_BOXING=0` pins the 64-bit JSValue layout. Do NOT drop this: with
  pc64's stdint lacking `INTPTR_MAX`, quickjs.h's `#if INTPTR_MAX <
  INT64_MAX` silently chose the 32-bit NaN-boxed layout (undefined macro =
  0 in `#if`) and crashed the first `js_dup` - `compat/stdint.h` fixes the
  limits AND the flag makes the choice explicit.
- `compat/` is first on the include path so quickjs sees a real double
  `math.h` (pc64's kernel math is float-only; the transcendentals come from
  unojs's ujs_math - the one double libm in the tree), a `stdlib.h` that
  declares `strtod` (implicit-int otherwise = every JSON number parses as
  0 through the wrong return register), and a `stdio.h` whose FILE surface
  is renamed `qjs_*` (a real symbol named `stdout` interposes the host
  libc's in the smoke test and SEGVs inside puts).

## Tests

    cd pc64 && sh quickjs/test/build-host-test.sh && ./build/qjs_host_test.exe

Links the SAME freestanding objects (+ qjs_port.c + unojs's real ujs_math.c
+ stubbed uno_native_* clocks) against the host CRT: 31 checks over Math /
dtoa / JSON / RegExp+unicode / Date-UTC / BigInt / Uint8ClampedArray.
Green on mingw (LLP64 - the kernel's compiler) and on linux gcc with
ASan+UBSan (swap CC=gcc and add the sanitizer flags; see git history for
the exact invocation). The QEMU spectest remains the in-OS gate.
