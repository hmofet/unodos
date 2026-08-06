# The second-engine programme: real-world engines behind the browser's seams

**Status: BOTH PHASES LANDED ON MASTER, 2026-08-05/06.** Phase 1 (QuickJS) via
`qjs-engine`; phase 2 reshaped by the licence ruling from "NetSurf layout" into
"the MIT libraries upgrading unoweb in place", delivered as CS0 through CS3.
The programme's build work is done. What remains is one decision, one bug that
is not root-caused, and one verification waiting on a networking fix (§5):

- **The default-cascade flip** (CS3) is the user's call and has not been taken.
- **The quickjs DOM adapter is written but pinned off** (`pc64/js.c`, 2026-08-06):
  it runs clean under a native linux build, and the mingw host test binary dies
  at startup before `main` the moment quickjs is the selected DOM engine. Not
  root-caused. `js_run()` is unaffected, so the script-engine switch on
  `uno:engine` still works on either engine; it is only the live-DOM layer that
  is pinned.

The ask this answers: "port Blink/V8 as a switchable option next to our own
engine, same front end." This doc records why that exact ask is unbuildable,
what delivered its value instead, and what is left.

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
targets). *The NetSurf half of that sentence was overtaken the same day by
the no-GPL ruling: what shipped is NetSurf's MIT libraries under unoweb's
own layout, not its layout engine. See §4.*

## 2. The seams (why "same front end" already works)

- **Scripts:** `js_run()` in `pc64/js.h` - the browser's ONLY script entry.
  Now an engine dispatch: `JS_ENGINE_UNOJS` (default) / `JS_ENGINE_QUICKJS`,
  runtime-switchable via `js_engine_set()`; the `uno:engine` internal page
  is the user-facing switch. Registry-style: an engine is a backend file +
  an appended table row in js.c.
- **Layout/paint:** the browser switches between the DOM-walk flow painter and
  the unoweb pipeline. **This became a RUNTIME switch on 2026-08-06** (`g_renderer`
  in `pc64_browser.c`, third choice on `uno:engine`): build.sh always compiled
  unoweb, quickjs and csslib into every kernel, so `-DUW_ENGINE` was only choosing
  which of two in-image paths this one file called, and one of them was
  unreachable. `UW_ENGINE` survives as the INITIAL selection, so
  `BROWSER_ENGINE=uw` still means what the gates expect, and the flow painter
  stays the default. Switching clears the parsed tree and any focused form
  control (the two paths disagree about who runs `<script>`).
- **Cascade:** the third switch, added by CS3: built-in cascade or libcss, live
  on `uno:engine`, restyle forced on toggle.

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

Since landed, and the update this section wanted: **unoweb M5's `webjs.c`
arrived 2026-08-06** with the live DOM (get/querySelector, text/attrs/innerHTML,
create/append/remove), events with bubbling, timers and mutation-driven restyle.
The DOM itself is written once for both engines behind `webjs.h`'s five-operation
call frame, and quickjs's adapter (`QJS_WEBJS_ENGINE` in `qjsweb.c`) exists, but
`webjs_engine_current()` returns the unojs one unconditionally, per the pin
described at the top of this doc. Un-pinning it is the next quickjs slice, and it
starts by root-causing the mingw-only startup death.

Still not in scope: per-frame yielding via the interrupt handler (the browser
blocks on `js_run` today either way), ES module loading over `pc64_http`.

## 4. Phase 2 - real CSS under unoweb's layout (LANDED CS0-CS3; licence-reshaped)

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
- **CS2 `libcss-cascade`: LANDED 2026-08-05** (-> master `2137c285`).
  unoweb gained the alternate-cascade seam (`uw_cascade_set` /
  `uw_style_store` / `uw_ua_css`; a failing pass falls back to the
  built-in cascade). The bridge (`csslib/uw_select.c` + `uw_cascade.c`,
  embedder surface `csslib/uwx.h`) implements the full select handler over
  unoweb's public DOM, shared-UA + <style> + style= sheets, compose-based
  inheritance, and the computed->uw_style mapping. The UA default text
  colour moved INTO the shared sheet (html{color}) so parity doesn't ride
  on a hardcoded root default. Tests: css_cascade_test (29 checks - both
  cascades over one document, per-engine expectations + full-tree parity),
  green mingw + linux ASan/UBSan; SPECTEST S-CSS-01..06 in QEMU (77/0/7).
  Two handler ownership rules libcss does not document are recorded in
  csslib/VENDOR.md (classes-array pool; node data MUST be stored - eager
  delete is a use-after-free on real trees).
- **CS3 `libcss-wire`: LANDED 2026-08-05** (-> master `d54378cc`). The
  uno:engine page (now titled "Engines") grew the layout-cascade switch
  (built-in / libcss, plain-link pattern, restyle forced on toggle), and
  `tools/browser_cascade_urc.py` drove the comparison in a
  BROWSER_ENGINE=uw QEMU boot: uno:welcome / uno:sample / uno:script
  rendered under both cascades differ by **0.000-0.009% of pixels** with
  shell chrome masked - pixel-identical page content. Side find: every
  BROWSER_ENGINE=uw build had been link-broken since 2026-08-03 (um_inflate
  compiled twice after UnoAmp's skin work); fixed in build.sh.
  **Open decision - the default flip**: the evidence supports making libcss
  the default cascade in uw builds (and eventually retiring the built-in
  one to CONSUME-only), but the flip is deliberately NOT taken here; the
  built-in cascade remains default until the user calls it.
- **Update 2026-08-06:** the blocker this bullet named is gone. unoweb M4
  landed the subresource fetch queue (`pc64_fetch.c`), so network `<img>`
  and `<link>` sheets are fetched, and M7 verified framing, keep-alive and
  progressive render against a real server. What blocks a real-network
  comparison run now is a different bug: **DNS fails on a production
  build** (filed the same day against the unonet lane), so a live page run
  wants a debug image until that is fixed.

If real-page layout quality still disappoints after CS3, the remaining gap
is uw_layout itself (floats, tables, inline-block); that is unoweb-lane
work on its own plan, not a vendoring question - there is no MIT-licensed
drop-in layout engine worth taking (Blink/WebKit derivatives are not
extractable, NetSurf's is GPL, litehtml is C++).

## 5. What is left, and how to pick it up

Both phases are on master, so there is no branch series to start. Three
things remain, in the order they are worth doing:

1. **The default-cascade flip (user's call, no engineering left).** CS3's
   pixel evidence is 0.000-0.009% difference with shell chrome masked. If
   the user calls it, the flip is a default change plus retiring the
   built-in cascade to CONSUME-only over time.
2. **Un-pin the quickjs DOM adapter.** Root-cause why the mingw host test
   binary dies before `main` with quickjs selected as the DOM engine, when
   the same object set is fine under linux and when unojs is selected.
   Until then `webjs_engine_current()` stays hardcoded to unojs, which is
   the honest choice: a binding that might take the browser down is worse
   than one engine's binding.
3. **A real-network cascade comparison**, once the production DNS bug is
   fixed, using `tools/browser_cascade_urc.py` against live pages rather
   than `uno:` internal ones.

To pick any of it up: read this doc, then `pc64/quickjs/VENDOR.md` and
`csslib/VENDOR.md`, then the 2026-08-05/06 entries in
`pc64/UNOAUTOMATE-REQUESTS.md`. Merge gate for anything in this lane =
both builds + spectest_qemu green (S-JS-10..13 and S-CSS-01..06 included)
+ the host tests. Two traps recorded from this programme: a
`BROWSER_ENGINE=uw` build was silently link-broken for two days (um_inflate
compiled twice), so **wipe `build/` between variants**, and the licence
rule (no GPL, MIT ok) is standing - check every vendored file, not just the
project's top-level LICENSE.
