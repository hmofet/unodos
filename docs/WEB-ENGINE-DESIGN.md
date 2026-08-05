# UnoDOS web engine, full HTML/CSS/JS design

Status: DESIGN (2026-07-27). Nothing here is built. The current browser
(`pc64/pc64_browser.c` + `pc64/js.c`) stays in production until milestone M5
flips the default.

This is the plan for replacing the immediate-mode markdown/HTML flow renderer
with a real engine: DOM tree, CSS cascade, box layout, display-list paint, and
a garbage-collected bytecode JavaScript VM. The organizing rule, and the reason
the design splits the way it does: **the JS interpreter is a separate library
from HTML/CSS.** Neither knows the other's types; a thin binding layer joins
them inside the browser app.

---

## 1. Goals and non-goals

**Goals**

- Render real-world *text-first* pages correctly: docs, wikis, blogs, READMEs,
  news articles, package indexes, man pages. That means: real HTML error
  recovery, the CSS block/inline formatting model, author stylesheets, images,
  links, basic forms.
- Run enough JavaScript that "the page's own scripts built the content" pages
  show their content: DOM construction, `document.write`, events, timers,
  `innerHTML`, class toggling.
- Stay inside the UnoDOS idiom: portable C99 libraries over narrow shims
  (the `unomedia` / `acpipower` pattern), host-testable off-OS, painted with
  `fb.h` + `uno_font`, shipped as a `.UNO` app module.
- Every layer independently testable; the JS engine reusable with no web
  stack linked at all.

**Non-goals (explicit, permanent or long-horizon)**

- Web-app parity (Gmail, Figma): no JIT, no WebGL/canvas, no workers, no
  WebSockets, no video. Google's homepage rendering *its logo and search box*
  is in scope; Google Docs is not.
- No process isolation: JS runs ring-0 like everything else on this OS. The
  defense is budgets and bounds discipline (§14), not sandboxing.
- No cookies/localStorage in v1 (privacy + persistence complexity); no cache
  beyond the page lifetime; no HTTP/2.

---

## 2. Architecture overview

Three deliverables, one seam:

```
   pc64_browser.c ── the APP: chrome (address bar, tabs later), navigation,
        │            fetch queue, event-loop integration, history
        │
        ├── webjs.c ──── the BINDING layer: window/document/Element wrappers.
        │     │   │      The ONLY file that includes both unojs.h and unoweb.h.
        │     │   │
        │  unojs/       unoweb/
        │  JS engine    web core: DOM + HTML parser + CSS + layout + paint
        │  (lib)        (lib)
        │  knows        knows NOTHING of JS. Emits "script element complete"
        │  NOTHING      callbacks; exposes a C mutation/query API.
        │  of HTML/CSS.
        │
        └── pc64_http.c (+ streaming variant) ── over unonet (consumed)

   unoweb paints through fb.h + uno_font; images through unomedia (consumed).
```

- **`unojs/`**: a standalone ECMAScript engine. Public header `unojs.h` has
  zero web vocabulary: values, objects, functions, evaluation, host objects,
  GC roots, interrupts. It must link and pass its test suite with no unoweb
  present. (Side benefit: it becomes available to any other consumer, e.g.
  Studio's console, as a neutral API, without this project owning those uses.)
- **`unoweb/`**: DOM store, HTML parser, CSS engine, layout, paint. Public
  header `unoweb.h` has zero JS vocabulary. It must render a page completely
  with no JS engine linked (`<script>` contents simply go unexecuted, the
  NoScript build, which is also the fast path for M2-M4 development).
- **`webjs.c`**: the glue. Implements the DOM bindings as unojs host objects
  calling unoweb C functions. Replaceable/deletable without touching either lib.

**Packaging.** The whole engine ships as `APPS/BROWSER.UNO` (the `STUDIO.UNO`
precedent: it already imports `net_dns_query`, `tls_*`, `pc64_net_up` through
`KX()` thunks, proving the seam carries networking). The kernel keeps only the
`KX()` exports; kernel size does not grow. The current built-in browser remains
until M5, then is deleted from the kernel image.

**Subsystem registration (AGENTS.md §1).** When implementation starts, TWO new
registry rows are added (in the first commit of each): `unojs` (contract
`unojs/UNOJS.md`, files `unojs/*`) and `unoweb` (contract `unoweb/UNOWEB.md`,
files `unoweb/*`, `pc64/webjs.c`). The existing "browser" row keeps
`pc64_browser.*`, `pc64_http.*`, and (until M1 retires it) `js.*`.

---

## 3. unojs, the JavaScript engine

Replaces `js.c` (577-line tree-walker, statement subset) with a real VM.

### 3.1 Execution pipeline

```
source ──lexer──▶ tokens ──parser──▶ AST ──compiler──▶ bytecode ──VM──▶ result
                                     (arena)           (function objects, GC heap)
```

- **Bytecode stack VM**, not a tree-walker. ~90 opcodes, `u8` op + immediate
  operands, per-function constant pool. Locals resolved to stack slots at
  compile time; variables captured by closures get environment records
  (Lua-style upvalues: open on the stack, closed on escape).
- **Values: NaN-boxed 64-bit.** Doubles are themselves; pointers/int32/
  bool/null/undefined live in the NaN payload (48-bit pointers are safe under
  UEFI's identity map). One `ujs_val` type, pass-by-value everywhere.
- **Exceptions** via an in-VM handler stack (`try/catch/finally`), no setjmp.
- **Interrupts / fuel.** The VM decrements a fuel counter on every branch and
  call; at zero it returns `UJS_YIELD` with all state intact in the VM object,
  resumable by `ujs_resume()`. This is *the* mechanism that makes untrusted JS
  survivable in a single-threaded ring-0 OS: the frame loop gives each page a
  per-frame slice, and a hot `while(1)` becomes a slow page, not a dead OS.
  A cumulative budget (default 60 s of slices) kills the script with a banner.

### 3.2 Heap and GC

- **Mark-sweep, non-moving** (embedder-friendly: raw `ujs_val`s in C locals are
  pinned via a handle scope API, `ujs_scope_open/close`, V8-style).
- Allocation: size-class free lists over 64 KB chunks obtained from the OS
  `malloc`. Per-VM cap (default 8 MB), hitting it triggers GC, then a JS
  `RangeError`, never OS heap exhaustion.
- Strings immutable UTF-8, hashed; atoms (interned strings) for property names,
  shared table with the compiler.
- Objects: open-addressing hash of atom→value plus a dense element vector for
  arrays. No hidden classes in v1 (measure first; shapes are an optimization
  milestone, not architecture).

### 3.3 Language level

ES5.1 essentially complete, plus the ES2015 subset real pages break without:

| In v1 | Deferred | Never (documented) |
|---|---|---|
| all statements/operators, closures, prototypes, `Function`, getters/setters | classes (sugar), generators | JIT |
| `Object` / `Array` / `String` / `Number` / `Math` / `JSON` / `Date` | `Proxy`, `Reflect`, typed arrays | workers/Atomics |
| `let`/`const`, arrow functions, template literals, `for-of`, spread (call/array) | modules (`import`) | `eval` of page-supplied strings is IN (it's just the pipeline re-entered) |
| `Map`/`Set`, `Promise` + microtask queue | `async/await` (compiles onto promises later) | |
| `RegExp`: backtracking interpreter, the common syntax (classes, groups, anchors, quantifiers, alternation, flags `gim`) | lookbehind, unicode classes | |

### 3.4 Embedder API (`unojs.h`, the whole surface)

```c
ujs_vm  *ujs_new(const ujs_config *);            /* heap cap, fuel defaults */
void     ujs_free(ujs_vm *);
int      ujs_eval(ujs_vm *, const char *src, int len, ujs_val *out);
int      ujs_resume(ujs_vm *);                   /* after UJS_YIELD          */
ujs_val  ujs_call(ujs_vm *, ujs_val fn, ujs_val self, int argc, ujs_val *argv);
/* host objects: how webjs.c builds `document` without unojs knowing DOM */
ujs_val  ujs_host_new(ujs_vm *, void *user, ujs_finalizer fin);
void    *ujs_host_user(ujs_val);
void     ujs_set(ujs_vm *, ujs_val obj, const char *name, ujs_val v);
void     ujs_set_fn(ujs_vm *, ujs_val obj, const char *name, ujs_cfunc f);
void     ujs_set_accessor(ujs_vm *, ujs_val obj, const char *name,
                          ujs_cfunc get, ujs_cfunc set);
ujs_val  ujs_get(ujs_vm *, ujs_val obj, const char *name);
/* conversions, handle scopes, error inspection, fuel control ... */
```

Return codes: `UJS_OK / UJS_THROW / UJS_YIELD / UJS_OOM`. `js_run()` from js.h
is reimplemented over this in M1 as a 30-line compat shim so `pc64_browser.c`
and `pc64_spectest.c` don't change on day one.

Size estimate: ~13 k LOC C. Host-tested (§15) against a curated test262 subset.

---

## 4. unoweb, DOM core

- **Node** = 48-byte arena record: type (document/element/text/comment/
  doctype), parent / first-child / last-child / prev / next pointers, and a
  per-type payload pointer. Element payload: interned tag atom, attribute
  vector, `computed_style*`, `box*`, `ujs wrapper` slot (opaque `void*`: the
  DOM does not know what a wrapper is; webjs.c uses it).
- **Arena per document.** Bump allocation; navigation frees the whole arena in
  O(1). Detached nodes are *not* reclaimed until navigation, bounded by the
  arena cap (default 16 MB → OOM error page). This is a deliberate trade:
  no per-node free lists, no dangling-pointer class of bugs. A DOM-churning
  SPA eventually hits the cap and gets the error page; acceptable for this
  browser's mission, and documented in the contract.
- Atomized tag/attribute/class names, one interner shared by HTML parser, CSS
  matcher, and `getElementsByTagName`.
- Indexes: `id → node` hash maintained on mutation (for `getElementById` and
  `#fragment` scrolling).
- Mutation API (`uw_create_element`, `uw_insert_before`, `uw_remove`,
  `uw_set_attr`, `uw_set_text` …) sets dirty bits: `RESTYLE_SELF`,
  `RESTYLE_SUBTREE`, `RELAYOUT`. The engine never restyles eagerly; the frame
  tick batches (§12).

---

## 5. HTML parser

Spec-shaped, subset-sized. Two stages, both **push/streaming** (bytes arrive
from the network incrementally):

- **Tokenizer**: the WHATWG state machine's core states, data, tag open/name,
  attributes (all quote states), self-closing, comments (proper `-->` logic),
  doctype (swallowed), RCDATA (`<title>`, `<textarea>`), RAWTEXT (`<style>`),
  script data (with `</script>` detection incl. the escaped states real pages
  hit). Character references: full named-entity table (generated, ~2.2 k
  entries, sorted for binary search) + numeric refs.
- **Tree builder**: the insertion-mode algorithm reduced to what error recovery
  actually requires: implied `<html>/<head>/<body>`; `<p>`/`<li>`/`<dt>`/`<dd>`
  /heading auto-close; table modes with foster-parenting of stray text; a
  simplified active-formatting-elements list (reconstruct `<b>/<i>/<a>` across
  block boundaries; the full adoption-agency algorithm is NOT implemented -
  mis-nested formatting degrades gracefully instead of matching the spec
  tree exactly).
- **Script handling**: on `</script>`, the builder emits
  `UW_EV_SCRIPT(node, src, len)` through the host-callback table and pauses
  (classic blocking-script semantics). `document.write` during that callback
  feeds `uw_parser_insert()`: an insertion-point stack splices written text
  into the input stream, spec-style. If no callback is registered (NoScript
  build), the script text is simply dropped and parsing continues.
- **Fragment mode** (`uw_parse_fragment`) for `innerHTML`.
- Charset: UTF-8 assumed; windows-1252 fallback when a `<meta charset>` or
  header says so (single-table transcode at the tokenizer mouth).

Size estimate: ~3.5 k LOC + generated entity table.

---

## 6. CSS engine

- **Parser** (css-syntax subset): qualified rules, declarations, `!important`,
  `@media screen/(min|max-)width` (evaluated against the canvas), `@import`
  (fetch-queued like `<link>`), unknown at-rules skipped by block matching.
  Values: colors (named/hex/rgb()), lengths `px em rem %` (+`vw vh`), keywords,
  the shorthands that matter expanded at parse time (`margin`, `padding`,
  `border`, `font`, `background`, `list-style`).
- **Selectors**: tag / `.class` / `#id` / `[attr]`, `*`, compound; descendant,
  child `>`, adjacent `+` combinators; `:link :visited :hover :active
  :first-child :last-child :nth-child(odd|even|n)`. Matched right-to-left.
  Rules indexed by rightmost simple selector (hash on id/class/tag) so
  matching is per-bucket, not per-stylesheet.
- **Cascade**: UA sheet (compiled-in table of the html5 default styles) <
  author sheets < inline `style=""`, ordered by specificity then source order,
  `!important` inverting author/UA as per spec.
- **Computed style = fixed struct** (~200 bytes, arena): every supported
  property has a slot; no property maps. Inherited properties copied from the
  parent's struct; lengths resolved to px at computed-value time except `%`
  (kept symbolic, resolved in layout). Cheap to compare (`memcmp`) for
  restyle-damage detection.
- Property set v1: `display (block | inline | inline-block | none |
  list-item)`, `color`, `background-color`, `font-family/-size/-weight/-style`,
  `line-height`, `text-align/-decoration/-transform/-indent`, `white-space`,
  `letter-spacing`, margins/paddings/borders (width/style/color per side),
  `width height min-* max-*`, `box-sizing`, `overflow (visible|hidden)`,
  `vertical-align (baseline|top|middle|bottom)`, `visibility`,
  `list-style-type`. M6 adds `float/clear`, `position (relative|absolute;
  fixed→absolute)`, `z-index`, `background-image`. M7 considers single-line
  flexbox and `border-radius`.
- `font-family` maps generic families onto the bundled TTFs
  (sans → SANS.TTF, monospace → MONO.TTF, serif → UBUNTU.TTF or the closest
  we ship); everything else falls to sans.

Size estimate: ~4.5 k LOC.

---

## 7. Layout

- **Box tree** generated from DOM × computed style: `display:none` pruned,
  anonymous block boxes wrap stray inline runs (the spec's fixup), list-item
  markers synthesized.
- Geometry: **int32 device pixels** (fb is integer; `uno_font` measures in
  int px, no fixed-point until proven needed).
- **Block formatting**: top-down width resolution (auto/px/%, `box-sizing`),
  bottom-up height; margin collapsing between vertical siblings (the simple
  adjacent case only, no clearance interaction until M6 floats land).
- **Inline formatting**: line boxes built greedily; words measured via
  `uno_font_text_w_styled` (bold/italic/size from computed style); nested
  inline boxes carry border/padding fragments across line breaks;
  `white-space: pre/nowrap` honored; `text-align: left|center|right|justify`
  (justify = inter-word); baseline alignment with `vertical-align` offsets;
  `line-height` per spec (half-leading).
- **Replaced elements**: `<img>` via **unomedia** (`um_image_open/frame`: decoders already exist and are licensed); intrinsic size from
  `um_image_info`, scaled by CSS w/h (nearest-neighbor blit v1). `<input>`,
  `<button>`, `<textarea>`, `<select>` are replaced boxes drawn by the engine
  (unoui-look) with focus/editing handled in the app layer (M6).
- Incremental relayout: dirty-bit driven, subtree relayout when the subtree's
  containing block size is unchanged, else full. (Full layout of a big page
  at this scale is milliseconds; correctness first.)

Size estimate: ~5 k LOC.

---

## 8. Paint

- Layout emits a **display list**: `FILL_RECT`, `BORDER`, `TEXT_RUN(font,
  px, style, atom-or-buffer)`, `IMAGE(um handle, src/dst rects)`,
  `CLIP_PUSH/POP`, in paint order (background → borders → children → inline
  content), stacking contexts by tree order until `z-index` lands in M6.
- The canvas draw replays the list translated by scroll offset, clipped to the
  viewport, **scrolling never relayouts**, it re-replays. Full-viewport
  replay per dirty frame (the current app already full-repaints; fb fills and
  glyph blits are cheap at 1280×800).
- Hit testing walks the same list backwards (topmost first) mapping a point to
  the owning DOM node, one geometry source for paint and events.
- Links: `TEXT_RUN`s carry the nearest `<a>` ancestor for underline/color from
  CSS and for the hit-test → navigation path.

Size estimate: ~1 k LOC.

---

## 9. The binding layer (`webjs.c`), the seam

The only component that sees both worlds. ~3 k LOC of mechanical glue:

- `window` (global object): `document`, `location`, `navigator` (honest UA:
  `"UnoDOS-pc64"`), `setTimeout/clearTimeout/setInterval`, `alert` (status-bar
  banner), `addEventListener`.
- `document`: `getElementById`, `querySelector(All)` (delegates to the CSS
  selector matcher, one selector engine, two consumers), `createElement`,
  `createTextNode`, `write` (parser insertion point), `body/head/title`,
  `cookie` (reads `""`, writes ignored, logged).
- `Element` wrappers created **lazily** on first JS touch, cached in the DOM
  node's wrapper slot, registered as GC roots until the node's document dies
  (nodes are arena-owned, so wrapper lifetime ≤ document lifetime, no cycles
  to collect across the boundary). Surface: `innerHTML` (get: serializer;
  set: fragment parse + subtree replace), `textContent`, `className/classList`,
  `style.x` (writes into an inline-declaration block → RESTYLE), `getAttribute/
  setAttribute`, tree accessors, `offsetWidth/Height` (forces layout flush -
  the classic sync-layout point, documented), `addEventListener`.
- **Events**: capture → target → bubble; `Event` objects with
  `preventDefault/stopPropagation`; wired from unoui input in the app layer.
  Default actions (link navigation, form submit, checkbox toggle) run after
  dispatch unless prevented.
- Everything crossing the seam is bounds-checked and re-validated: page JS is
  hostile input to unoweb, and unoweb content is hostile input to JS strings
  (the `REQ_PUT` discipline, everywhere).

---

## 10. Networking and resource loading

- `pc64_http` (browser-owned) grows a **streaming API**:
  `pc64_http_open(url) → h`, `pc64_http_poll(h, sink_cb)`, header access, and
  the existing redirect-following moves inside it. The current
  buffer-the-world `pc64_http_get` remains for small fetches (and the REQUESTS
  file gets a note that it's now a convenience wrapper).
- **Fetch queue** over `netsock` (NSOCK=12): the document connection plus up
  to 3 parallel subresource connections (stylesheets first, first layout
  blocks on pending CSS or a 3 s timeout, then images in viewport order,
  lazily as scrolling reveals them).
- **Constraint (today):** `tls.c` wraps the single legacy TCP slot, so only
  ONE HTTPS stream can be live at a time. v1 rule: HTTPS pages fetch
  subresources sequentially on the one TLS slot; HTTP pages parallelize.
  A `per-socket TLS context` request goes to the unonet owner via
  `UNOAUTOMATE-REQUESTS.md`-style claim (their lane, not ours); the fetch
  queue is written against `tls_connect(sock)` so it upgrades transparently.
- `Connection: close` per fetch in v1; keep-alive is a later request for the
  same reason (needs per-socket TLS to matter).
- Limits: 2 MB HTML, 4 MB per image, 512 KB per stylesheet/script, 16 MB
  arena, 8 MB JS heap, each overflow degrades (truncate parse / skip
  resource / OOM page), never crashes.

---

## 11. Navigation model

- `location.href` set, link click, or address bar → the app tears down the
  document (frees arena, frees VM, closes sockets), pushes history, starts the
  fetch. Back/forward = re-fetch (no cache in v1).
- `#fragment` navigation scrolls via the id index, no teardown.
- History: ring of 32 URLs + scroll offsets in the app.

---

## 12. Event loop, one frame tick

Integrated into the unoui frame loop (the browser canvas's tick):

```
per frame:
  1. net_poll(); pump fetch queue → feed parser (bounded bytes/frame)
  2. run due timers + microtasks + resumed scripts, under this frame's
     JS fuel slice (default ~2 ms worth)
  3. dispatch queued input events (hit test → DOM dispatch → default actions)
  4. if RESTYLE dirty  → recompute styles (damaged subtrees)
     if RELAYOUT dirty → layout → rebuild display list
  5. if display list or scroll changed → replay paint → present
```

Everything is cooperative and bounded; a busy page degrades to fewer
bytes/frame and slower scripts, never to a frozen desktop. The `UJS_YIELD`
fuel mechanism (§3.1) is what makes step 2 safely preemptible.

---

## 13. Memory model summary

| Pool | Discipline | Cap (default) | On exhaustion |
|---|---|---|---|
| Document arena | bump, freed on navigation | 16 MB | OOM error page |
| JS heap | mark-sweep GC, size classes | 8 MB | GC then `RangeError` |
| Parser scratch | per-feed, reset each pump | 64 KB | truncate token |
| Display list | rebuilt per layout, arena | counted in arena | drop tail + banner |
| Fetch buffers | streaming, fixed rings | 16 KB/socket | backpressure (stop reading) |

No unbounded allocation exists anywhere in the engine; every buffer's owner
and cap is named in the contract docs.

---

## 14. Security posture (read this twice)

This is a ring-0, no-MMU-protection OS executing untrusted programs (page JS)
against untrusted data (page bytes). There is no sandbox to hide behind, so
the design leans on:

1. **Fuel + heap caps** (§3.1, §3.2), availability: scripts cannot hang the
   OS or exhaust the OS heap.
2. **No ambient authority in the bindings**: webjs.c exposes exactly the DOM
   of *this* document. No file system, no `uno_fs`, no URC, no exec, no raw
   sockets. `fetch()`/XHR are absent in v1 (when added: same-origin only).
3. **Every length checked**: parser, decoder, and binding code treat all page
   content as adversarial (the existing `REQ_PUT` precedent is the house
   style). Fuzzing the tokenizer/parser on the host build (§15) is a merge
   gate for parser changes.
4. **unosecure audit hook**: navigation events logged through the existing
   audit surface (their API, consumed not modified) so network egress from
   the browser is visible.

The honest statement for the contract doc: UnoDOS treats browsing as
*courtesy, not containment*, a malicious page exploiting an engine bug owns
the machine, exactly like a malicious .UNO app. The mitigations above raise
the bar; they do not create a security boundary.

---

## 15. Testing strategy

Layered, mostly OFF the OS (the `acpipower`/`unomedia` host-test pattern):

- **unojs host tests** (`unojs/test/`): builds with plain gcc; a curated
  test262 subset + regression files; valgrind-clean is a merge gate. The fuel
  mechanism is tested with hostile loops.
- **unoweb host tests** (`unoweb/test/`): builds with a stub fb that records
  draw calls. Three golden layers: (1) HTML → DOM-tree dumps, (2) DOM+CSS →
  box-geometry dumps, (3) box tree → display-list dumps. Goldens are tiny
  hand-written pages; failures print diffs. Tokenizer/parser fuzz harness
  (afl-style file corpus in-repo, minutes not hours).
- **In-OS**: `pc64_spectest.c` gains S-WEB checks (golden pages on the ESP,
  layout invariants asserted in-engine); `harness.py` grows a `webtest`
  scenario, boot QEMU + SLIRP, drive the browser to local golden pages served
  over `guestfwd`, screenshot-diff. Live-site smoke (google.com redirect
  chain, example.com) stays a manual/documented check since the internet
  isn't deterministic.
- Merge gate per AGENTS.md: both `UNO_DEBUG` builds + host suites green.

---

## 16. Milestones, landable slices (AGENTS.md-sized)

Each milestone is one or a few short branches; master stays green throughout;
the shipping browser is untouched until M4's flag flip.

| M | Deliverable | Exit criteria |
|---|---|---|
| **M1** | `unojs/` engine + host suite; `js_run()` compat shim replaces `js.c` | spectest S-JS green; Script.html demo identical; js.c deleted |
| **M2** | `unoweb/` DOM + HTML parser (NoScript build) | host golden DOM dumps green; browser renders via a bridge that walks the DOM with the OLD flow painter, screenshots match current pages |
| **M3** | CSS parse + cascade + block layout + paint list | golden box/display-list dumps; demo pages pixel-compared in QEMU |
| **M4** DONE 2026-08-06 | Inline formatting, images (unomedia), links/hit-test; app wired behind `BROWSER_ENGINE=uw` build flag | real article-class pages render; old renderer still default. Landed: subresource fetch queue (network `<img>` + `<link>` sheets, `pc64_fetch.c`), line-close alignment (`text-align`, `vertical-align`, line box = max ascent + max descent) |
| **M5** PARTIAL 2026-08-06 | webjs.c bindings, events, timers, innerHTML | LANDED: live DOM (get/querySelector, text/attrs/innerHTML, create/append/remove), events with bubbling, setTimeout/setInterval, mutation-driven restyle; 17 host checks + SPECTEST S-WJS-01..10. NOT DONE: **the flag does NOT flip** - the flow painter stays the default renderer by user ruling (2026-08-06), and the quickjs DOM adapter is written but pinned off (see js.c) |
| **M6** PARTIAL 2026-08-06 | forms (text input, submit → GET; POST added to pc64_http), tables, floats, position, z-index | LANDED: **tables** (rows through row groups, text-proportional columns, cells stretched to row height), **floats + clear** (context passed down the block tree so a float in body shortens later paragraphs), **position + z-index** (relative shifts the subtree and leaves its space; absolute/fixed out of flow; stable z sort of the display list). NOT DONE: **forms** - no text input, no submit, pc64_http is still GET-only |
| **M7** PARTIAL 2026-08-06 | progressive render during fetch, style sharing, keep-alive + parallel TLS, perf pass | LANDED: **response framing** (Content-Length + chunked), **keep-alive** (one connection across a page's subresources, HTTP/1.1, retry-once on a dropped reuse), **progressive render** (the transport offers the body every ~6 KB and the browser paints it mid-fetch). All three are **UNVERIFIED against a live server** - the gate's network area runs on a null NIC, so a real page load is the real test; failure modes are bounded (a wrong keep decision costs the existing ~3 s idle timeout, not a hang). NOT DONE: **style sharing** - ATTEMPTED AND REVERTED, the key (tag/class/id/style/parent style) computed a WRONG COLOUR for an `li` in the golden suite; find what it misses before retrying. Also open: parallel TLS needs per-socket TLS from unonet, which does not exist. |

Rough sizing: unojs ~13 k, unoweb ~14 k, webjs ~3 k, app/net rework ~2 k LOC.
`BROWSER.UNO` estimated 300-400 KB, well inside the module loader's range
(PYRT.UNO is 310 KB today).

---

## 17. Risks and open questions

- **RegExp completeness** is the classic long tail once frameworks run;
  time-boxed per milestone, with `UJS_YIELD` fuel protecting against
  catastrophic backtracking.
- **Adoption-agency simplification** will mis-tree some mis-nested formatting;
  accepted, revisit only if golden real-world pages show it mattering.
- **Detached-node arena growth** (§4) on DOM-churny pages: mitigated by cap +
  error page; a compacting "copy live DOM to fresh arena" GC is sketched as a
  contingency, not planned.
- **Single-slot TLS** serializes HTTPS subresources until unonet grows
  per-socket TLS, filed as a request when M4 starts, not blocked on.
- **Font coverage**: no font fallback chain yet for glyphs the bundled TTFs
  lack; boxes render as `uno_font`'s notdef. Acceptable v1.
- Open: whether `unojs` should also back `unoscript`'s JS tier someday, out
  of scope here; the neutral-API split makes it *possible* without deciding it.
