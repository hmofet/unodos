# UnoOffice: a Microsoft Office 97 clone for UnoDOS pc64 — implementation plan

Status: PLANNED, 2026-08-01. This is the implementation document for Opus
workers. Its sibling `docs/OFFICE97-SPEC.md` is the conformance yardstick the
finished product is compared against; this file is how to get there. Read
[`/AGENTS.md`](../AGENTS.md) first — every worker on this plan is bound by it.

## 0. The directive, and the three rulings baked into this plan

The directive: a full clone of Office 97 — **Word, Excel, PowerPoint** — with
look, feel and functionality as close to identical as practical, plus
**native support for Office 2003 files** (= the 97-2003 *binary* formats:
`.doc`, `.xls`, `.ppt` — the OOXML `.docx` tier is Office 2007 and out of
scope). An **Outlook clone** was conditional on OAuth / web sign-on being
straightforward.

Three rulings from the research (sources: the four 2026-08-01 research
reports summarized in §11):

1. **Outlook is DEFERRED to phase M (last, optional).** The OAuth research
   verdict is split: Gmail OAuth is *not* straightforward (device flow bans
   Gmail scopes; browser flows need a browser Google will refuse; production
   needs an annual paid CASA audit) — but Gmail app passwords still work, and
   Microsoft's device-code flow (the one OAuth an Outlook.com client cannot
   avoid) is genuinely small: two HTTP POSTs + polling over the TLS stack we
   already have. So when phase M runs it is: generic IMAP/SMTP + app
   passwords first, Microsoft device flow as the only OAuth. Per the
   directive's own condition, none of the mail work gates anything else.
2. **The file formats are a LIBRARY, not app code.** `unodoc/` — the
   long-planned document-format sibling of `unomedia` (named in
   `unomedia/README.md` "What unomedia is NOT") — is where CFB, `.doc`,
   `.xls`, `.ppt` and the shared Escher drawing layer live. Apps consume it.
3. **The Office chrome is OUR canvas, not unoui surgery.** Office 97's menus
   and toolbars are owner-drawn "command bars", not native controls — which
   is lucky, because unoui's menubar is flat (no submenus, separators,
   icons, checkmarks) and the 64-widget/window cap cannot host Office
   toolbars anyway. So the suite draws its own pixel-faithful command-bar
   engine (`uochrome`) inside one UI_CANVAS, exactly as UnoAmp draws Winamp.
   unoui stays a neutral API we consume; zero choke-point fights.

## 1. Subsystems, lanes, and workers

Two new registry rows (added to `/AGENTS.md` with this commit):

| Subsystem | Contract | Root files | Worker |
|---|---|---|---|
| unodoc (CFB + doc/xls/ppt + Escher R/W) | `unodoc/UNODOC.md` | `unodoc/*` | worker A |
| unoffice (uochrome + UnoWord/UnoCalc/UnoShow apps) | this file + `OFFICE97-SPEC.md` | `pc64/uoffice/*`, `pc64/apps/uo*.c` | workers B, C, D |

Rules of engagement (AGENTS.md, condensed):
- One worktree + branch per slice off `origin/master`; rebase at session
  start; land small via rebase-then-ff; delete branch+worktree the day it
  lands. Claim your slice in `pc64/UNOAUTOMATE-REQUESTS.md` first.
- Merge gate: builds `UNO_DEBUG=0` and `=1`, the relevant host/QEMU gate
  green, no choke-point restructuring. Choke-points touched by this plan are
  all APPEND-only: `build.sh` file list, `kExports[]` in `pc64_modload.c`,
  the `EX_*` app slots in `pc64_uui.c`.
- Everything from scratch. Specs are open ([MS-CFB], [MS-DOC], [MS-XLS],
  [MS-PPT], [MS-ODRAW] on learn.microsoft.com; patents under the Microsoft
  Open Specification Promise). Study Apache POI (Apache-2.0) and LibreOffice
  (MPL-2.0) freely; **do not read GPL sources** (Antiword, wvWare, catdoc,
  Gnumeric importer). Any spec-mandated third-party data goes through the
  THIRD-PARTY manifest rule (see `unomedia/`).

Worker map: **A** owns unodoc end to end (phases 1-5). **B** owns uochrome +
UnoWord (6-8). **C** owns UnoCalc (9-10). **D** owns UnoShow (11-12).
A starts alone; B can start uochrome (phase 6) in parallel from day one; C
and D fork off after 6 lands. Mail (M) is unassigned until the user asks.

## 2. What already exists (build on it, do not rebuild it)

From the 2026-08-01 capability survey — the load-bearing facts:

- **`pc64/pc64_write.c`** is a working WordPad-class editor: per-character
  style words (bold/italic/underline, 4 faces, 8 sizes, per-paragraph
  alignment), greedy word-wrap layout (`wr_layout`), styled clipboard,
  find/replace, its own UWD container. UnoWord *extends this design*, it
  does not start from zero — but needs a real run/paragraph model, undo, and
  a page (not window) layout target.
- **TTF engine** `pc64/pc64_font.c`: kerned, fractional-pen, styled draw
  with a measure guaranteed to match the draw (`uno_font_draw_styled` /
  `uno_font_text_w_styled` / `height_px` / `baseline_px`), all exported to
  modules. Limits to fix in OUR lane's request file, not by editing:
  px clamp 8..40 (PowerPoint titles need 72+), ASCII-only glyph cache.
- **`.UNO` module system**: apps are PE32+-flattened modules resolved by
  name against `kExports[]`; `PHOTOS.UNO` is the exact template for a module
  carrying a library inside it (it links unomedia's image half privately).
  Each Office app ships as a module carrying unodoc. Module `.bss` is
  charged to the 4 MB module arena — **document buffers go on the kernel
  heap via `malloc`** (32 MB, exported), not in module `.bss`.
- **unoui**: MDI container (`unoui_add_mdi`, 12 children), tabs with public
  geometry, UI_CANVAS + `unoui_fullscreen`, the input layer. The suite is
  one UI_CANVAS per app window; MDI hosts multiple documents.
- **unomedia**: 9 image decode formats emitting `fb_px`-native pixels —
  picture insertion is solved on the read side. Remember the allocator rule:
  **every unomedia/unodoc consumer calls `um_set_alloc(malloc, free)` (and
  unodoc's equivalent) before first use; it is idempotent.**
- **`um_inflate`** is a standalone raw-deflate decoder = ZIP method 8; the
  hardened central-directory ZIP walk in `pc64/unoamp_skin.c` (2026-08-01)
  is the pattern for archive parsing done right.
- **fb primitives**: rects, lines, gradients, rounded rects, blend, clip.
  **No polygon/ellipse/bezier fill** — phase 4b builds `uo_geom` (scanline
  polygon fill + ellipse + quad bezier flattening) inside the unoffice lane;
  charts, pie wedges, autoshapes and Escher rendering all sit on it.
- **Fonts**: ship metric-compatible substitutes, never Microsoft's fonts:
  **Liberation Serif → "Times New Roman", Liberation Sans → "Arial",
  Liberation Mono → "Courier New"** (SIL OFL — license text into
  `THIRD-PARTY.md` + `DOCS\LICENSES.MD` per the unomedia precedent). Word's
  default is Times New Roman 10pt, Excel's Arial 10 — with Liberation
  metrics, 97-era documents lay out to the same line breaks.

Known platform gaps that are REQUESTS to other lanes (file them, use the
nearest primitive meanwhile, do not block on them):
- **unofs**: FAT long filenames (8.3 today — real-world `.doc` names will
  not round-trip); a streaming/append write path (today `uno_fs_write` is
  whole-file-from-one-buffer, so saves materialize the full file in RAM —
  acceptable at Office-97 document sizes, just budget for it).
- **shell**: a bigger multi-format clipboard (512-byte plain-text today).
  Until granted: the suite keeps a private rich clipboard (the Editor
  precedent) and mirrors plain text into the shell clipboard.
- **font lane**: raise the styled-draw px clamp to 8..96; (later) CP-1252
  glyph coverage beyond ASCII.
- **Printing does not exist in the OS.** Not in scope for v1; the spec marks
  every Print menu item as present-but-stubbed (Print Preview IS in scope —
  it is just layout). A later `unoprint` (PostScript/IPP to the LAN bridge)
  is noted in §10.

## 3. Phase plan overview

Sequencing follows the formats research: CFB first (everything sits in it),
BIFF8 second (flat records shake out CFB with the least coupling), then
.doc, then Escher (before PPT — PPT writing cannot avoid it), then .ppt.
The UI track runs in parallel. Every phase = one or more landed slices, each
with a host gate that runs without booting the OS, plus a QEMU gate where an
app is involved.

```
worker A: 1 CFB → 2 XLS-r → 3 XLS-w+formulas → 4 DOC → 5 Escher+PPT
worker B: 6 uochrome → 7 UnoWord model+layout → 8 UnoWord app (uses 4)
worker C:              9 UnoCalc grid+engine → 10 UnoCalc app (uses 2,3)
worker D:              11 UnoShow canvas     → 12 UnoShow app (uses 5)
later:    M UnoMail
```

## 4. Worker A — unodoc (phases 1-5)

Create `unodoc/` on the unomedia playbook: `unodoc.h` (core: allocator,
error strings, `ud_src` random-access byte source like `um_src`),
per-format `ud_*.c`, `UNODOC.md` contract with `[EXPERIMENTAL]` markers and
a changelog, `unodoc/test/` host harness. Build integration: one appended
`for f in ud_...` block in `pc64/build.sh` (compile into the kernel only
what the kernel needs; apps link their halves privately, PHOTOS-style).

### Phase 1 — CFB container, read AND write (`ud_cfb.c`, ~3-4k LOC)
- Read: header (v3, 512-byte sectors), DIFAT (109 header entries + chained
  sectors), FAT chain walker with cycle/bounds defense (cap chain length at
  filesize/512; every sector index validated — this parser is attack
  surface number one), directory tree (ignore red-black colors on read),
  miniFAT + mini stream (4096 cutoff), stream extraction by path.
- Write: NEVER in place. Serialize fresh from an in-memory model: lay out
  streams, mini vs regular by the 4096 cutoff, build miniFAT/FAT/DIFAT/
  directory, emit. Directory: emit a balanced BST, all-black, ordered by
  (name length, uppercased UTF-16 code units) — the ordering is what Office
  actually checks.
- Gate `unodoc/test/cfbtest.c`: round-trip (build → write → reread →
  compare), plus open every `.doc/.xls/.ppt` in `unodoc/test/corpus/` (seed
  it with files saved from LibreOffice headless; grow it with a govdocs1
  subset — see §9). Fuzz list in the harness from day one (malformed
  headers, looped chains, out-of-range sectors).

### Phase 2 — `.xls` read (BIFF8, `ud_xls.c`, ~8-12k LOC)
- Record stream walker with `Continue` handling; globals substream (FONT —
  mind the index-4 skip, FORMAT, XF, STYLE, BOUNDSHEET, SST + EXTSST) and
  per-sheet substreams (ROW, LABELSST, NUMBER, RK, MULRK/MULBLANK, BLANK,
  BOOLERR, FORMULA + STRING, MERGEDCELLS, COLINFO, WINDOW2).
- **The SST/Continue trap is the number-one BIFF8 bug**: a string may split
  at any character across a Continue boundary and the continuation restates
  the 8-bit/UTF-16 flag, so one string can switch encodings mid-way. Write
  the dual-encoding string reader once, as a shared helper — the same trap
  reappears in .doc (piece table fc bit 30) and .ppt (TextBytesAtom vs
  TextCharsAtom). CP-1252 fold to the internal 8-bit text for v1.
- Formula READ = ptg RPN decompiler with precedence-aware parenthesization
  (operands PtgInt/Num/Str/Bool/Ref/Area/Ref3d/Area3d/Name, operators,
  PtgFunc/FuncVar against the embedded ~400-entry iftab, PtgAttr jumps,
  shared formulas via ShrFmla/PtgExp — mandatory, Excel writes them for
  fill-downs). 1900/1904 date epochs; detect FILEPASS (encrypted) and
  refuse cleanly. Skip chart/pivot substreams cleanly by BOF dt.
- Gate: value+formula extraction matches a fixture table for the corpus;
  decompiled formula text re-parses to the same ptgs once phase 3 lands.

### Phase 3 — `.xls` write + the formula compiler (~9-16k LOC)
- Writer: globals preamble (the canonical 16-XF/5-FONT/formats block —
  build it once from a real Office-saved file dump and freeze it as a byte
  array, the "canned blob" trick), BOUNDSHEET offset backpatching (the
  serializer wants a fixup list from day one), SST with correct
  continuation, cells, MERGEDCELLS, COLINFO, DIMENSIONS/ROW bookkeeping.
- Formula WRITE = tokenizer → precedence parser → ptg compiler **with
  operand class assignment** (ref/value/array flavors chosen by consumption
  context — the subtle half; Excel is picky on read-back). Budget it as its
  own component with its own test suite. Cheap milestone on the way: emit
  cached results with fAlwaysCalc for a whitelist of shapes.
- Gate: written files open in LibreOffice headless with correct values AND
  formulas; POI-based structural diff in CI where available; round-trip
  read(write(model)) == model property test.

### Phase 4 — `.doc` read + minimal writer (~8-13k LOC)
- Read: FIB → piece table (CLX/PlcPcd in the table stream chosen by
  `fWhichTblStm`; fc bit 30 = compressed 8-bit vs UTF-16LE, pieces switch
  mid-document; document order ≠ file order — walk it, no shortcut) →
  CHPX/PAPX FKPs via bin tables → sprm delta interpreter (~50 common sprms:
  bold/italic/underline/size/font/color/justification/indents/spacing) over
  the STSH style hierarchy → sections (PlcfSed/SEPX: page size, margins,
  columns) → tables as in-band text (0x07 cell marks, sprmT* geometry on
  row ends) → fields (emit cached results between 0x14 and 0x15) → embedded
  pictures (PICF in Data stream → Escher blob → hand to phase 5; render as
  placeholder until then).
- Write, the well-trodden minimal path: single-piece piece table, text +
  0x0D, freshly built FKPs, **canned STSH blob** (Normal + Heading 1-9 +
  defaults, frozen from a real file), SttbfFfn (Times New Roman, Symbol,
  Arial), one-section PlcfSed, default Dop. Word and LibreOffice both
  accept this. Formatting written as sprm deltas from the canned styles.
- Gate: text+formatting extraction fixtures over the corpus; written files
  open clean (no repair prompt) in LibreOffice; UnoWord round-trip.

### Phase 5 — Escher ([MS-ODRAW]) + `.ppt` (~10-18k LOC)
- `ud_escher.c` standalone with host-callback seams for the three
  host-specific parts (anchors, client data, BLIP byte location) — it
  serves .ppt now and .doc/.xls drawing layers later. Read: record walker,
  FOPT property decoder (~30 common properties), BLIP store (PNG/JPEG near
  raw → unomedia decodes them), geometry for ~10 shape types (rect,
  roundrect, ellipse, line, arrow, textbox + bounding-box placeholder for
  the rest). Write: DggContainer with correct shape-ID accounting, per-page
  DgContainer, SpContainers for rect/textbox/picture.
- `.ppt` read: Current User → UserEditAtom chain → fold persist directories
  newest-wins → live DocumentContainer → slides/masters; text from
  SlideListWithText OR the slide's Escher client data (exactly one of the
  two — check both); StyleTextPropAtom runs with TxMasterStyleAtom
  inheritance.
- `.ppt` write: single UserEdit, canned MainMaster blob (with required
  TxMasterStyleAtoms), N SlideContainers each with a valid PPDrawing
  skeleton, PersistDirectoryAtom + UserEditAtom + consistent
  CurrentUserAtom. PowerPoint is the strictest of the three about internal
  consistency — the round-trip gate is load-bearing here.
- Gate: slide/text/shape extraction fixtures; written decks open clean in
  LibreOffice Impress headless.

## 5. Worker B — uochrome (phase 6) and UnoWord (phases 7-8)

### Phase 6 — `pc64/uoffice/uochrome.c`: the Office 97 shell, on one canvas
The shared chrome all three apps render through. Everything drawn from a
theme table of Office 97 constants (`#C0C0C0` face, `#808080` shadow,
`#FFFFFF` highlight, `#000080` selection, MS-Sans-Serif-8-alike via the
shipped fonts at 100% scale) so the whole suite re-tunes from one place.
- **Command bars**: menu bar + dockable/floatable toolbars; flat buttons
  that raise on hover, sink when pressed/toggled; drag handles; combo boxes
  in toolbars (Style/Font/Size/Zoom); dropdown-split buttons with tear-off
  palettes (border/highlight/color pickers); full static menus with 16x16
  icons, separators, submenus, checkmarks, disabled gray, accelerator
  column. Keyboard: Alt-mnemonics, F10, arrows, Esc.
- **Dialog engine**: modal-within-the-canvas dialog frames with tabs,
  group boxes, previews — the ~30 shared dialog layouts (Font, Paragraph,
  Tabs, Borders and Shading, Page Setup, Format Cells, ...) are data tables
  over this engine, not bespoke code. Includes the message-box (with the
  "?" title-bar help affordance) and the Office file Open/Save dialog
  (look-in combo, list/details toggle, type filter, MRU) over `uno_fs_*`.
- **Icon sheet**: one BMP sprite atlas, 16x16 cells, drawn in the 16-color
  VGA palette look (own artwork, Office-97-*style*, never copied bitmaps).
- **Status bars, rulers** (Word's tab-stop/indent ruler; PPT's), zoom.
- The Office Assistant is IN scope as a stub character ("Uno", our own
  artwork — not Clippit's likeness): balloon with query box wired to the
  spec's help topics, idle animation, closable, off by default. Fidelity of
  the *frame*, not of Microsoft's characters.
- Gate `pc64/tools/uochrome_test.c` (host, links fb.c like unoui's harness):
  scripted event stream over a demo window → PPM storyboard asserting
  hover/press/open-menu/submenu/tear-off/dialog-tab states pixel-stable.

### Phase 7 — UnoWord's document model + page layout (`pc64/uoffice/uow_*.c`)
Model (`uow_doc.c`): piece-table text storage (the .doc lesson applied
in-memory: edits append, pieces map order — this is also what makes undo
cheap), character-run formatting (font/size/bold/italic/underline/color/
highlight/super-sub), paragraph properties (style id, alignment incl.
justify, indents, spacing, tabs, borders/shading, list level), named styles
with based-on chains (the ~90 built-ins from the spec, seeded from a
table), section properties (page size/margins/columns/headers-footers),
tables (rows/cells with widths, merges, borders), inline+floating objects
(pictures via unomedia, text boxes), fields (PAGE/NUMPAGES/DATE/TIME/
FILENAME + cached-result storage for the rest), bookmarks, footnotes.
**Undo/redo from day one** — command objects over the piece table; nothing
mutates the model except through them.

Layout (`uow_layout.c`): paginated flow — the window is a viewport onto
pages on a gray pasteboard, not the wrap target (the one thing
`wr_layout` cannot be taught): line breaking with justification, page
breaking with widow/orphan control, keep-with-next, columns with
balancing, headers/footers per section (first/odd/even), footnotes at page
bottom, tables splitting across pages (row-atomic v1), floating-object
wrap (square/top-bottom v1). Uses `uno_font_*_styled` metrics exclusively.
Incremental: relayout from the damaged paragraph forward, stop when line
starts re-converge — typing must stay under a frame at Carbon speeds.

Gate `uow_layout_test.c` (host): fixture documents → line-break/pagination
tables + PPM renders; a justification-correctness assertion (sum of gaps ==
line slack); a typing-latency micro-bench.

### Phase 8 — UnoWord the app (`pc64/apps/uoword.c` → `UOWORD.UNO`)
Views Normal/Page Layout (+ Outline, Print Preview), the spec's menu tree
and Standard/Formatting toolbars wired to real commands, find/replace with
options, spelling with red squiggles (wordlist port of a permissive
dictionary — SCOWL, license into THIRD-PARTY), AutoCorrect (two initial
caps, capitalize first letter, replacement table), bullets/numbering,
tables UI (insert/draw, merge/split, autoformat presets), columns,
headers/footers editing mode, page setup, styles dropdown + dialog,
Document Map, word count, zoom. File I/O: `.doc` native via unodoc, UWD
import (one-way, from the Editor), `.TXT`, `.RTF` write (cheap and useful).
New `EX_UOWORD` slot + `KX` appends. Gate: SPECTEST additions S-UOW-*
(open-fixture-assert-model, edit-save-reopen, QEMU screenshot storyboard).

## 6. Worker C — UnoCalc (phases 9-10)

### Phase 9 — grid + calculation engine (`pc64/uoffice/uoc_*.c`)
- Sheet store (`uoc_sheet.c`): 65536x256 per sheet, sparse — column-keyed
  row segment arrays on the kernel heap; cell = tagged value (double, RC
  string-table index, bool, error) + XF-style format index + optional
  formula. Selection model (ranges, multi-range, fill handle semantics).
- Formula engine (`uoc_calc.c`): parser to an internal RPN mirroring ptg
  semantics (so unodoc phase 2/3 conversion is mechanical), evaluator,
  dependency graph with dirty propagation + natural-order recalc +
  circular detection (iteration setting), A1/R1C1, 3-D refs, named ranges.
- **Functions: the full built-in set from the spec (~230), plus the
  Analysis ToolPak set behaving as installed** — the spec file carries the
  authoritative list; implement by category with shared numeric kernels
  (the stats/financial iterators are the bulk). Every function lands with
  fixture rows (input → Excel-97 result) in `uoc_calc_test.c`.
- Number formatting (`uoc_numfmt.c`): the full custom format-code language
  (sections, #/0/?/,/%, date-time pictures, [Red], conditions) — Excel's
  display fidelity lives or dies here.
- Gate: host harness runs the function fixture table (thousands of rows),
  recalc-order and circular tests, format-code fixtures.

### Phase 10 — UnoCalc the app (`pc64/apps/uocalc.c` → `UOCALC.UNO`)
Grid canvas (virtualized draw of visible cells only; headers, gridlines,
freeze panes, split, in-cell editing overlay + formula bar with Name Box
and the 97 Formula Palette, Range Finder colored borders, marching-ants
marquee, fill handle with right-drag menu, AutoComplete, AutoCalculate
status well), Format Cells (all 6 tabs), conditional formatting (3
conditions), AutoFilter dropdowns, sort dialog, data validation,
subtotals, merged cells, comments, multiple sheets with tab strip,
Page Break Preview (draggable breaks), page setup with repeat rows/fit-to.
**Charts v1**: the 2D core types (column/bar/line/pie/XY/area/doughnut)
via Chart Wizard's 4 steps, rendered on `uo_geom` (§2), embedded objects
on the sheet's drawing layer; 3-D chart types and pivot tables are v2
(menu present, wizard stubbed with an honest "not in this build").
File I/O: `.xls` native (unodoc), CSV/TXT import-export.
Gate: S-UOC-* SPECTESTs + QEMU storyboard; the LibreOffice cross-check
(save → open in Calc headless → value diff) runs in the host gate.

## 7. Worker D — UnoShow (phases 11-12)

### Phase 11 — slide model + shape canvas (`pc64/uoffice/uos_*.c`)
Model: presentation = slide size + masters (slide/title/notes/handout) +
color schemes (the 8 scheme roles) + slides (layout id, background
override, shape list); shape = frame (pos/size/rotation) + geometry id +
fill (solid/gradient/pattern — Fill Effects) + line + shadow + text body
(paragraphs with the 5 indent levels, bullets, per-run formatting) +
placeholder role; z-order, grouping. ~20 autoshape geometries v1 on
`uo_geom` (rect, roundrect, ellipse, triangle, diamond, arrows, star,
callouts...), adjustment handles where cheap.
Renderer: slide → framebuffer at arbitrary scale (editing zoom, sorter
thumbnails, show mode full-screen); B&W view mode.
Gate: host PPM fixtures per layout/scheme/shape battery.

### Phase 12 — UnoShow the app (`pc64/apps/uoshow.c` → `UOSHOW.UNO`)
Views: Slide, Outline (promote/demote drives titles/bodies), Slide Sorter
(transition icons, drag reorder), Notes Page; the 24 AutoLayouts; design
templates (a handful of own-artwork `.pot`-equivalents seeded from scheme
tables); Apply Design; masters editing; Header and Footer dialog.
**Slide Show mode**: full-screen via `unoui_fullscreen`, click/arrow
advance, the right-click show menu, pen annotation, black screen,
hidden slides, **transitions v1** (cut, wipe 4-dir, blinds, box, dissolve,
random bars, cover/uncover — frame-loop animated on the shell tick) and
**builds v1** (appear, fly-from, wipe, dissolve; by paragraph level;
after-animation dim). Timings/rehearse/kiosk loop v2.
File I/O: `.ppt` native via unodoc.
Gate: S-UOS-* + QEMU storyboard incl. a scripted show run.

## 8. Phase M — UnoMail (deferred, runs only on explicit go)

Per the ruling in §0: IMAP4rev1 subset (LOGIN/AUTHENTICATE PLAIN + XOAUTH2,
LIST/SELECT/FETCH/STORE/APPEND, UIDs), SMTP submission on 465/587, over the
existing TLS stack. Auth matrix v1: app passwords (Gmail w/ 2FA, iCloud,
Fastmail, generic) + **Microsoft device-code flow** for Outlook.com (two
POSTs + poll + refresh-token store; scopes `IMAP.AccessAsUser.All`,
`SMTP.Send`, `offline_access`; needs a one-time free Azure app
registration — the registration id is configuration, not code). No Gmail
OAuth: documented as "use an app password", revisit only if Google ever
allows mail scopes on the device flow. UI: Outlook-97 shell (Outlook Bar,
folder list, table views, reading pane off by default) reusing uochrome;
Calendar/Contacts/Tasks local-only v1 in a CFB-based `.pst`-like store —
scoped fully in a future doc before work starts.

## 9. Cross-cutting: corpus, oracles, and honesty rules

- **Oracle**: LibreOffice headless on the WSL side (`soffice --headless
  --convert-to txt/csv/pdf`) is the always-available smoke test; "opens
  with no repair prompt" in real Office (a VM on this LAN has Office 97 and
  modern Office available when arin stages one) is the strict gate, run at
  milestones, not per-commit.
- **Corpus**: `unodoc/test/corpus/` seeded from LibreOffice-authored files
  + a downloaded govdocs1 subset (freely redistributable; fetch script, not
  committed binaries) + poi test files where license-clean. Fuzz the
  readers continuously in the host harness (the OS-resident parser rule).
- **Honesty**: menu items whose guts are out of scope (VBA/Macros, Pack and
  Go, Presentation Conference, pivot v1...) exist, look right, and say
  plainly "not in this build" — the spec tracks three states per item:
  `full / chrome-only / absent`. No silent fakes.
- **The sanitizer lesson** (UnoAmp EQ, 2026-07-31): every host harness
  builds with build.sh's set `-fsanitize=signed-integer-overflow,bounds,
  shift,integer-divide-by-zero,null` — a harness without the OS's flags
  tests different code.

## 10. Later / explicitly out of v1

OOXML (.docx/.xlsx/.pptx — needs a deflate *compressor*; note the ZIP
reader half now exists in hardened form), printing (`unoprint`:
PostScript → the C:\IPPBridge LAN printer), VBA, pivot tables, 3-D charts,
Escher full autoshape geometry (200 shapes), OLE in-place activation
between our apps (v1 embeds render as pictures with edit-by-open),
password-protected files ([MS-OFFCRYPTO]), non-Latin scripts, Office
Binder/Shortcut Bar/Find Fast/Map.

## 11. Research provenance

Four reports, 2026-08-01, produced for this plan (summaries live in this
repo's history with this commit; re-run the research before contradicting
it): (1) Office 97 feature/UI inventory — full menu trees, toolbars,
dialogs, function list, defaults; folded into `OFFICE97-SPEC.md`.
(2) Binary formats deep-dive — [MS-CFB]/[MS-DOC]/[MS-XLS]/[MS-PPT]/
[MS-ODRAW] structure, effort table (40-70k LOC for the coherent R/W
subset), canned-blob strategy, corpus/oracle plan; folded into §4.
(3) Email auth feasibility — the Gmail/Microsoft/iCloud matrix behind §8's
ruling. (4) UnoDOS capability survey — what exists, the gaps table behind
§2. Spec PDFs: learn.microsoft.com openspecs (one-shot zip:
OfficeFileFormatsProtocols.zip); keep local copies under
`unodoc/specs/` (gitignored).
