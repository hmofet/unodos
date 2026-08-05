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

## 4. Phase 2 - real CSS under unoweb's layout (PLANNED; licence-reshaped)

**Licence ruling (user, 2026-08-05): no GPL code in the tree; MIT is ok.**
That resolves the old NS0 decision point immediately: the NetSurf *browser
core* (the layout engine proper) is GPLv2 - **excluded**. The NetSurf
project's *libraries* are MIT - eligible. So phase 2 is no longer "NetSurf
as a third painter"; it is **the MIT libraries upgrading unoweb in place**,
which the old plan already named as most of the visible quality win at a
fraction of the integration. unoweb keeps DOM, HTML parsing, layout and
paint ownership; the browser seams don't change.

What gets vendored (each MIT, verify per-release with
`tools/check_licenses.py` + licence roster entries like unomedia's):
`libparserutils`, `libwapcaplet`, `libcss`. (`libhubbub`/`libdom` are also
MIT but unoweb already owns an HTML parser and DOM - only pull them if
uw_html's gaps ever justify it.)

Branch series (each lands independently, AGENTS.md-shaped):

- **CS0 `libcss-vendor`: LANDED 2026-08-05** (branch `libcss-vendor` ->
  master `0c753fb7`). Vendored at repo-root `csslib/` (libcss 0.9.2 +
  libparserutils 0.2.5 + libwapcaplet 0.4.3), per-file audit: 265/267
  explicit MIT, 2 under project COPYING, 0 GPL. Two upstream build-time
  artifacts generated once at vendor time (aliases.inc + 119
  autogenerated_*.c) - regeneration recipe in `csslib/VENDOR.md`.
- **CS1 `libcss-port`: LANDED 2026-08-05** (same branch). Whole stack
  freestanding (320 objects) over four libc shims + `-D_ALIGNED=` (an
  upstream stray-global quirk that breaks -fno-common links) +
  `-DWITHOUT_ICONV_FILTER`. Host test 8/8 on mingw AND linux ASan/UBSan:
  parse -> cascade -> computed styles (colors, grouped selectors, cascade
  order, unmatched->initial, display, @media filtering). Finding that
  shapes CS2: unmatched properties compute INITIAL values, so the UA
  default sheet (browser text colour, margins, display types per element)
  is a required CS2 deliverable, not an option.
- **CS2 `libcss-cascade`** (next): libcss behind uw_css/uw_style - libcss
  consumes DOM nodes through client vtables, so unoweb's DOM feeds it
  directly and uw_layout consumes its computed styles. Deliverables: the
  css_select_handler over uw_dom (seed: the test scaffolding in
  `csslib/test/`), a UA default stylesheet (see the CS1 finding above),
  the build.sh block (mirror `csslib/test/build-host-test.sh`'s flag set
  verbatim), and a SPECTEST area. Old cascade stays compiled (two-painter
  rule) behind the existing `BROWSER_ENGINE` switch until the new one has
  been through real pages.
- **CS3 `libcss-wire`**: default flip + SPECTEST area + uno:engine page
  grows a layout section if a runtime toggle is wanted.

If real-page layout quality still disappoints after CS3, the remaining gap
is uw_layout itself (floats, tables, inline-block); that is unoweb-lane
work on its own plan, not a vendoring question - there is no MIT-licensed
drop-in layout engine worth taking (Blink/WebKit derivatives are not
extractable, NetSurf's is GPL, litehtml is C++).

## 5. How to resume this programme

Read this doc, then `pc64/quickjs/VENDOR.md`, then the claim entries in
`pc64/UNOAUTOMATE-REQUESTS.md` (2026-08-05). Phase-1 branch `qjs-engine`
(landed `a67fe2f1`); merge gate = both builds + spectest_qemu green
(S-JS-10..13 included) + the two host tests. Phase 2 starts at CS0; the
licence rule (no GPL, MIT ok) is standing - check every vendored file, not
just the project's top-level LICENSE.
