# The second-engine programme: real-world engines behind the browser's seams

**Status: phase 1 (QuickJS) LANDED on branch `qjs-engine`, 2026-08-05.
Phase 2 (NetSurf layout) is PLANNED - branch series not started.**

The ask this answers: "port Blink/V8 as a switchable option next to our own
engine, same front end." This doc records why that exact ask is unbuildable,
what delivers its value instead, and the plan for the remaining phase.

## 1. Why not Blink/V8 (the feasibility finding, 2026-08-05)

pc64 is freestanding C (`x86_64-w64-mingw32-gcc -ffreestanding -nostdinc`
against `pc64/include`'s mini-libc): no C++ runtime, no threads, no POSIX,
one address space.

- **Blink is not portable as a component.** There is no standalone Blink
  embedding API; it hard-depends on Chromium's //base, Mojo IPC, the cc
  compositor, Skia, ICU, HarfBuzz/FreeType, PartitionAlloc, and a
  multi-process sandbox. The smallest thing that runs Blink is ~all of
  Chromium (~25M lines). Fuchsia needed a funded team, years, and a POSIX
  layer to get there. Not a matter of effort here - a different project.
- **V8 alone is conceivable but the prerequisite is the real cost:** C++20 +
  libc++/libc++abi, pthreads, TLS, VM-permission games (JITless mode avoids
  W^X only), a new platform port inside GN. That is "build a POSIX
  personality + C++ runtime for UnoDOS" - months, tens of MB, still no
  Blink at the end.

**The decided substitution (user, 2026-08-05):** QuickJS-ng as the second
JS engine + NetSurf's stack as the second layout engine. Both are C, both
are the proven "real engine on a small/hobby OS" lineage (NetSurf shipped
on RISC OS / Amiga / Atari; quickjs-ng builds for wasi/DJGPP-class
targets).

## 2. The seams (why "same front end" already works)

- **Scripts:** `js_run()` in `pc64/js.h` - the browser's ONLY script entry.
  Now an engine dispatch: `JS_ENGINE_UNOJS` (default) / `JS_ENGINE_QUICKJS`,
  runtime-switchable via `js_engine_set()`; the `uno:engine` internal page
  is the user-facing switch. Registry-style: an engine is a backend file +
  an appended table row in js.c.
- **Layout/paint:** `BROWSER_ENGINE=uw` (build flag `UW_ENGINE`) already
  switches the browser between the DOM-walk flow painter and the unoweb
  pipeline (`pc64_browser.c` ~line 371). Phase 2 adds a third path behind
  the same pattern. Both compiled always, "so neither can rot".

## 3. Phase 1 - QuickJS (LANDED; this is the record)

Files: vendored core in `pc64/quickjs/` (v0.16.1, unmodified - provenance
+ port contract in `pc64/quickjs/VENDOR.md`), port layer in
`pc64/quickjs/compat/` + `qjs_port.c`, backend `pc64/qjsweb.c`, dispatch in
`pc64/js.c`, build seam in `pc64/build.sh` (QJSCF block).

The port decisions that matter (full detail in VENDOR.md):

- `__DJGPP` personality (single-threaded, no OS), NOT `__wasi__`, which
  would compile out the C-stack overflow check the kernel needs.
- `-DJS_NAN_BOXING=0` + compat `stdint.h`: pc64's stdint omits
  `INTPTR_MAX`, and quickjs.h's `#if INTPTR_MAX < INT64_MAX` silently chose
  the 32-bit NaN-boxed JSValue on a 64-bit build (undefined = 0 in `#if`).
- Double math comes from unojs's `ujs_math` (kernel math is float-only);
  the derivable remainder is `qjs_port.c`. Request to bless that surface is
  filed in `pc64/UNOAUTOMATE-REQUESTS.md`.
- compat `stdlib.h` declares `strtod` (implicit-int had every JSON number
  parse as 0 via the wrong return register).
- The FILE-shaped stdio is renamed `qjs_*` (a real `stdout` symbol
  interposes the host libc's under the host tests).
- Bounds match unojs: 4MB heap, ~40M-op interrupt budget, 64KB C-stack.

Tests: `pc64/quickjs/test/` - engine-core smoke (31 checks) + dispatch
seam (12 checks, same scripts byte-identical on both engines, hostile
loop + hostile recursion contained), green on mingw AND linux ASan/UBSan;
SPECTEST gained S-JS-10..13 (quickjs on the kernel allocator/clock in
QEMU).

Not in scope yet (candidates for later slices): DOM bindings beyond the
document.write surface (arrives with unoweb M5's webjs.c - quickjs should
get the same bindings behind the same dispatch), per-frame yielding via the
interrupt handler (the browser blocks on js_run today either way), ES
module loading over pc64_http.

## 4. Phase 2 - NetSurf as the second layout engine (PLANNED)

Goal: real-page CSS + layout quality behind the existing painter switch,
keeping the browser chrome, tabs, history, pc64_http and the fb untouched.
NetSurf is the right donor because it is C99, single-threaded around a
cooperative scheduler, and its rendering core is DESIGNED for pluggable
frontends (the framebuffer frontend is how it shipped on RISC OS-class
systems).

What gets vendored (all NetSurf-project C libraries): `libparserutils`,
`libwapcaplet`, `libhubbub` (HTML), `libdom`, `libcss`, `libnsutils`,
`libnsfb` only as reference (we plot straight to fb.h). Plus the netsurf
core (content/handlers/html = the layout engine proper).

Branch series (each lands independently, AGENTS.md-shaped):

- **NS0 `netsurf-vendor`**: vendor drop + VENDOR.md provenance (pin one
  release), no build wiring. Licence roster entries (GPLv2 with the
  MIT-licensed libs - CHECK the exact mix per component before shipping it
  in a build; the libs are MIT, the netsurf core is GPLv2+link exception
  territory. If the core's licence is unacceptable for the tree, the
  fallback is libcss+libdom under unoweb's own layout - decide at NS0).
- **NS1 `netsurf-libs`**: the support libs freestanding under the QJSCF
  pattern (their own compat dir where needed) + a host test running
  libcss's selection engine on real stylesheets. These libs alone already
  let unoweb upgrade its cascade (libcss consumes DOM-ish nodes through
  vtables, no NetSurf core needed).
- **NS2 `netsurf-core-host`**: netsurf core compiled hosted with a stub
  frontend: feed HTML bytes, get plot calls, assert against golden pages.
  This is where the effort risk lives; timebox it before committing to NS3.
- **NS3 `netsurf-fe`**: the UnoDOS frontend - plotters over fb.h, fetcher
  over pc64_http, scheduler pumped from the browser tick, images via
  unomedia behind its content handlers.
- **NS4 `netsurf-wire`**: third entry in the painter switch (compile-time
  `BROWSER_ENGINE=ns` first, runtime toggle on the uno:engine page once
  stable) + SPECTEST area.

Decision points recorded now: (1) NS0 licence check gates the whole shape;
(2) if NS2's timebox blows, land NS1 anyway - libcss under unoweb is most
of the visible quality win at a fraction of the integration.

## 5. How to resume this programme

Read this doc, then `pc64/quickjs/VENDOR.md`, then the claim entries in
`pc64/UNOAUTOMATE-REQUESTS.md` (2026-08-05). Phase-1 branch `qjs-engine`;
merge gate = both builds + spectest_qemu green (S-JS-10..13 included) +
the two host tests. Phase 2 starts at NS0 with the licence check.
