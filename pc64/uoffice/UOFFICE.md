# unoffice, the Office 97 suite

UnoWord, UnoCalc and UnoShow, and the shared chrome all three are drawn with.
This file is the **contract** for the lane, per [`/AGENTS.md`](../../AGENTS.md)
§6: re-read it and the changelog after every pull. The plan that produced it is
[`docs/OFFICE97-PLAN.md`](../../docs/OFFICE97-PLAN.md) §5-§7; the yardstick the
result is measured against is
[`docs/OFFICE97-SPEC.md`](../../docs/OFFICE97-SPEC.md).

The formats live next door in [`unodoc/`](../../unodoc/UNODOC.md) and are
consumed from here as a neutral API.

Owner: the unoffice lane (workers B, C, D).

## Status

| Phase | Surface | State |
|---|---|---|
| 6a | uochrome: menu bar, static menus, docked toolbars | **landed**, `[EXPERIMENTAL]` |
| 6b | docking on four edges + floating, combo lists, split buttons + tear-off palettes, ScreenTips, icon artwork | **landed**, `[EXPERIMENTAL]` |
| 6c | uodlg: the dialog engine (tabs, group boxes, message box) | **landed**, `[EXPERIMENTAL]` |
| 6d | uobars + uofile: Open/Save dialog, status bar, ruler, the Assistant | **landed**, `[EXPERIMENTAL]` |
| 7 | UnoWord: document model + page layout (`uoword.h`, `uow_*.c`) | **landed**, `[EXPERIMENTAL]` |
| 8 | UnoWord the app (`pc64/apps/uoword.c` -> `APPS\UOWORD.UNO`) | **landed**, `[EXPERIMENTAL]` |
| 9 | UnoCalc: workbook store, calculator, number formats (`uocalc.h`, `uxl_*.c`) | **landed**, `[EXPERIMENTAL]` |
| 10 | UnoCalc the app (`pc64/apps/uocalc.c` -> `APPS\UOCALC.UNO`) | **landed**, `[EXPERIMENTAL]` |
| 11 | UnoShow: presentation model, autoshape geometry, slide renderer (`uoshow.h`, `uos_*.c`) | **landed**, `[EXPERIMENTAL]` |
| 12 | UnoShow the app (`pc64/apps/uoshow.c` -> `APPS\UOSHOW.UNO`) | **landed**, `[EXPERIMENTAL]` |

## Why the chrome is ours and not unoui

Office 97's menus and toolbars are **owner-drawn command bars**, not native
controls - which is the one lucky thing about cloning this era, because we are
supposed to draw them ourselves. We would have had to anyway: unoui's menubar
is flat (no submenus, separators, icons, checkmarks or accelerator column) and
its 64-widget-per-window ceiling could not hold an Office toolbar row.

So the suite draws its own chrome inside **one `UI_CANVAS`**, exactly as UnoAmp
draws Winamp's window, and consumes unoui as a neutral API. Nothing in this
lane edits a shared choke-point to exist.

## uochrome (phase 6a)

```c
#include "uochrome.h"

static const uoc_item kFile[] = {
    { "&New...\tCtrl+N", C_NEW,  0, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },                        /* a separator            */
    { "Sen&d To",        0,     -1, 0, kSendTo, 3 }   /* a submenu          */
};
static const uoc_menu kMenus[] = { { "&File", kFile, 3 }, ... };

uoc_ui chrome;
uoc_init(&chrome, kMenus, NMENU, kBars, NBAR, x, y, w);
...
if (uoc_handle(&chrome, &ev, &cmd)) { if (cmd) do_command(cmd); return 1; }
uoc_render(&chrome);          /* menus paint last, over the document       */
```

- **Menus are static data.** `'&'` marks the mnemonic, `'\t'` splits the label
  from the accelerator drawn in its own right-aligned column, and an item with
  `text == NULL` is a separator. Submenus nest to `UOC_MAXDEPTH`.
- **`uoc_height()`** is where the app's client area starts.
- **Toggle state lives here** (`uoc_toggle_set` / `uoc_toggle`) so a Bold
  button can be drawn from the caret's run without the app re-declaring its
  tables.
- **The icon atlas is an installation seam** (`uoc_set_icons`). Until phase 6b
  installs artwork every icon draws as a deterministic placeholder derived
  from its index, so layout and behaviour are testable before a pixel of art
  exists.

### Two rules this file lives by

1. **Geometry is computed once**, by the same functions the painter and the
   hit-tester both call. unoui learned this the expensive way (its
   `unoui_content_origin` comment records it): test a click against arithmetic
   that merely *resembles* what was drawn and the two drift until buttons stop
   working where they look.
2. **Metrics derive from the font**, never from a pixel count. The host
   harness draws with an 8x8 bitmap; pc64 draws with the kerned TTF engine at
   whatever size and UI scale the user picked. Everything is `fb_text_h()` /
   `fb_text_w()` plus the look table's paddings, so both lay out correctly.

### The look table

`uoc_look_97()` holds every colour and gap the chrome draws with - the
Windows 95 / NT4 system colours Office 97 shipped against - so the whole suite
re-tunes from one place when the pixel-check against a real Office 97 install
happens. Items the SPEC tags *(verify)* are exactly the ones that check will
settle.

## Docking, palettes and icons (phase 6b)

A toolbar is a strip in a **band**: a row at the top or bottom, a column at
the left or right, with two bars sharing a band sitting side by side in
declaration order. Dragging a bar's gripper (or a floating bar's title bar)
shows the Windows dashed drop outline; releasing within a bar-and-a-half of a
frame edge docks it there as a new band, anywhere else floats it.
`uoc_client_rect()` reports what is left for the document, so an app never
computes a band height itself.

All of that arrived without re-deriving a coordinate, because 6a had already
put every toolbar position behind `tb_origin()` / `tb_btn_rect()`. That is
rule 1 above paying for itself one phase later.

Also here: combo boxes that drop real lists and remember their selection
(`uoc_combo` / `uoc_combo_set`), split buttons that drop colour or icon
palettes, **tear-off palettes** (drag the move bar across a dropped palette's
top and it floats away, swatches still live — the Office 97 gesture people
remember), and ScreenTips on a hover dwell driven by `UI_EV_TICK`.

`uoicons.c` fills the atlas seam with 35 16x16 icons in the VGA palette,
drawn from shape primitives. **Our own artwork**: no Microsoft bitmap was
copied or traced. They are code rather than a data blob for the same reason
unodoc refused a canned STSH — a blob nobody can read is a blob nobody can
fix — and the shapes that are not rectangles are string art anyone can edit
by counting.

**A trap worth keeping.** `fb_set_clip` does NOT clip text. `fb.c`'s
`fb_text` clips to the screen only; the settable clip window lives in
`fb_aa.c` and governs the alpha primitives, and fb.c's own comment notes the
two domains merely "agree in practice" because unoui sizes widgets to fit. A
combo field is where they do not agree — "Times New Roman" overflowed across
the buttons beside it and then showed through the transparent parts of their
icons. Text that must fit a control is **truncated**, not clipped.

## The dialog engine (phase 6c)

`uodlg.h`. Office's ~30 shared dialogs are the same dozen controls arranged
differently, so this is one engine and they are **data tables** over it. An
app declares a `uod_dlg` of `uod_item`s at dialog-relative coordinates;
nothing in `uodlg.c` knows what a font is.

Controls: label, push button (default ring, focus ring), check, radio
(exclusive within its `group`), edit, list, drop-down combo, spinner with a
clamped range, etched group box with the caption punched out of its top edge,
and a preview well the app paints into after `uod_render` via
`uod_preview_rect`. Tabbed pages, where an item on page `-1` shows on every
page. Keyboard: Tab/Shift+Tab, arrows for lists, combos, spinners and radio
groups, a mnemonic anywhere jumps to that control and fires it, Enter presses
the default, Esc cancels.

**Modal within the canvas.** unoui has no dialog primitive at all — apps fake
one with a second window and nothing blocks the parent — so the suite draws
its dialogs over the document inside its own `UI_CANVAS` and swallows every
event while one is up. That is what Office 97 looked like too: a dialog was a
window, not an overlay panel. It drags by its title bar, and its close box and
its "?" button report **distinct** results rather than both meaning cancel.

`uod_msgbox` builds its dialog rather than taking one, because "Save changes
to Document1?" is not anybody's data table.

## The status bar, ruler, Assistant and file dialog (phase 6d)

`uobars.h` carries three independent pieces, each a pure function of its own
model plus the event stream:

- **The status bar**: Word's page and position cells, the four mode cells
  (REC/TRK/EXT/OVR, greyed when off) and the spelling book. `uob_status_hit`
  names the cell under the pointer so double-click-to-toggle has a target.
- **The ruler**: the white text column over grey margins, ticks, the
  first-line and hanging indent markers, the square below the hanging one
  that drags **both** while preserving their gap, the right indent, tab stops,
  and the selector at the far left that cycles left/centre/right/decimal.
  Clicking the bare ruler sets a stop of whatever type the selector shows.
- **The Assistant**: the balloon, the query box, the numbered blue-bullet
  answers, the Close button — and **"Uno"**, our own character, deliberately
  not a paperclip, a dog, a cat or a wizard. The SPEC asks for the fidelity of
  the frame, not for anyone's likeness. Off by default.

`uofile.h` is the Open / Save As dialog, which Office 97 shipped itself
rather than taking from the common dialog. It is a `uodlg` like any other;
what it adds is that **its list is not static data**. Contents arrive through
a filesystem seam (`uof_set_fs`): pc64 installs one wrapping `uno_fs_*`, the
host gate installs a fake, and `uofile.c` never learns which — the same trick
as unodoc's `ud_src`, and what makes it testable without booting the OS.
`uof_sync()` sits deliberately OUTSIDE `uod_handle`, so the dialog engine
stays a pure control layer that knows nothing about files.

### What phase 6 does NOT do

The Customize dialog (drag a command onto a bar, Large icons, menu
animations), right-click-a-bar's toolbar checklist, list/details/preview view
buttons in the file dialog and its MRU, real Assistant help content behind the
query box, and the Large-icons doubling. Those wait for the apps that need
them.

## The host gate

```bash
cd pc64/uoffice && ./build.sh
```

Builds three harnesses - `pc64/tools/uochrome_test.c`, `uodlg_test.c` and
`uobars_test.c` - against the same sources the module compiles freestanding,
linked to the shared software framebuffer (`ps2/fb.c` + `fb_aa.c`) exactly as
unoui's harness is, and drives a **scripted `unoui_event` stream**: the same
event stream pc64 feeds. 45 frames land in `build/uo{c,d,b}_*.ppm` plus three
contact sheets.

It asserts three kinds of thing, and the mix is the point:

- **Behaviour**: which menu is open, which item is hot, what command fired,
  that a disabled item never highlights and never fires, that keyboard Down
  skips both the disabled item and the separator after it.
- **Pixels**: that what was drawn matches what the state says. A model that is
  right while the painter is wrong is precisely the failure a behaviour-only
  test cannot see - so the navy of an open title, the bright edge of a hovered
  button, the dark edge of a pressed one and the sunken edge a toggle keeps
  after the mouse leaves are all sampled out of the framebuffer. Those four are
  SPEC S-OFF-01's central claim about command bars.
- **Determinism**: rendering the same state twice must produce byte-identical
  frames. A painter that accumulates rather than drawing from scratch passes
  every single-shot check and then drifts in use.

Built with `build.sh`'s own sanitizer set plus ASan and
`-fno-sanitize-recover=all`, per the UnoAmp EQ lesson: a harness built without
the OS's flags is testing different code.

## UnoWord (phases 7 and 8)

`uoword.h` is the document model and the page layout; `pc64/apps/uoword.c` is
the app. See uoword.h's header for why the model is a **piece table** and why
the layout wraps to the **page** rather than the window - the one thing pc64's
Editor cannot be taught. Font metrics arrive through a `uow_metrics` seam, so
the layout engine is gated on the host without booting the OS.

### Three things a module build teaches you

1. **`FB_W` / `FB_H` are variables on pc64** (`uno_fb_w` / `uno_fb_h`), and a
   `.UNO` module can only import **functions** - the loader turns every
   undefined symbol into a jmp thunk. The lane calls the exported
   `fb_width()` / `fb_height()` and never the macros; the host harness gets
   them from `host_fbdim.c` rather than an `#ifdef` in every file.
2. **A stack frame over 4 KB pulls in `___chkstk_ms`** on mingw - a probe
   that walks Windows' guard page, which this OS does not have and cannot
   provide. It is a host artifact, not a safety net, so the module is built
   `-mno-stack-arg-probe`. (unodoc's `.doc` reader keeps the 4 KB FIB on the
   stack, which is what tripped it.)
3. **`build.sh`'s kExports check earns its keep**: it caught all of the above
   at build time, rather than as a module that loads and then jumps into
   nothing.
4. **A module's BSS comes out of a 4 MB arena shared by every module**
   (`pc64_modload.c`, `MOD_ARENA_PAGES`), and `mkuno` prints the figure on
   every build. UnoCalc's first link read `mem=104036K`: a per-sheet cell
   array where each cell inlined 96 RPN tokens. It compiled, it passed every
   host gate, and it could never have loaded on the machine. **Read the
   `mem=` line** - it is the only place that number is visible before metal.

## UnoCalc (phases 9 and 10)

`uocalc.h` is the contract; `uxl_sheet.c` is the store, `uxl_calc.c` the
formula compiler and evaluator, `uxl_numfmt.c` the number-format language, and
`pc64/apps/uocalc.c` the app.

- **The store is sparse.** Excel 97's grid is 65536 x 256 = 16.7 million
  cells and a real sheet holds a few hundred, so live cells are a sorted
  array searched by bisection - the same reasoning BIFF8 applies when it
  stores rows as records rather than as a rectangle.
- **One pool, not one per sheet.** Cells come from a single workbook-wide
  pool and each sheet keeps a sorted array of **indices** into it. Per-sheet
  arrays cost `UXL_MAXSHEET` times the memory whether the sheets were used or
  not; sharing means one sheet may hold the lot, which is also how people use
  a workbook.
- **Compiled formulas live in a token pool**, not in the cell: inline, 96
  tokens x 32 bytes was 94% of the store and every literal paid it. Setting a
  formula bump-allocates and overwriting one leaks - deliberately, because
  the **source text is kept in the cell**, so a full pool is reclaimed by
  recompiling every live formula into a fresh one. No back-pointers, no free
  list, no compaction pass to get wrong. The gate rewrites one cell 4000
  times and then checks that an untouched neighbour still computes.
- **The RPN is shaped like Excel's ptgs on purpose** (operands carry a value
  or a reference, operators an arity), so unodoc's ptg conversion is
  mechanical in both directions rather than a translation layer nobody can
  check.
- **Recalc is memoised depth-first with a generation counter**, and a cell
  reached while it is already computing is the circular reference - which is
  reported, not hung on.
- **73 functions** across maths, statistics, text, logic, lookup and dates,
  and the number-format language (`#,##0.00`, sections for
  positive;negative;zero;text, dates, fractions, scientific). Excel's 1900
  leap-year bug is reproduced, because a date serial that disagrees with
  Excel's is worse than no date at all.

Two engine bugs the gate caught are worth repeating. `ROUND(2.345,2)` gave
2.34 because 2.345*100 is 234.4999... in binary, so rounding takes a relative
epsilon nudge. And a date fixture that read "12-Jun-97" was simply **wrong**:
serial 35562 is 12 May, and the engine was right - a fixture is only an oracle
when it comes from one.

### The trap phase 10 found: paint order

An app whose content sits **below** the chrome must paint
`uoc_render_bars()`, then its own content, then `uoc_render_popups()` LAST.
Calling `uoc_render()` in that position paints an open dropdown and then
overpaints it: UnoCalc's File menu came out as two items and a cut edge,
clipped to the toolbar band, and **UnoWord had the same bug** and nobody had
noticed because a two-item File menu still looks like a menu.

## UnoShow (phases 11 and 12)

`uoshow.h` is the contract; `uos_geom.c` holds the autoshape paths, colour
schemes, AutoLayouts and page setups, `uos_model.c` the store, `uos_render.c`
the renderer, and `pc64/apps/uoshow.c` the app.

- **One renderer, five destinations.** The editing zoom, a sorter thumbnail,
  the notes page, the handout grid and the full-screen show are `uos_render()`
  with a different rectangle. Everything is in SLIDE POINTS (72/inch, the
  on-screen show is 720 x 540) so that stays true, and the gate asserts it:
  the same slide drawn at full size and at thumbnail size must put the slide's
  centre at the centre of both rectangles. A shape that is only right at 100%
  is a shape the sorter shows wrong twelve times.
- **Autoshapes are paths in a 1000 x 1000 box**, mapped onto the shape's
  frame - one description for every size. The three shapes a polygon
  describes badly (ellipse, round rectangle, line) say so through
  `uos_geom_kind()` instead of being approximated with 64 vertices, which
  looks like 64 vertices at show size and costs 64 divisions at thumbnail
  size. The pentagon's and the star's trigonometry is precomputed: a constant
  table has no business being recomputed 60 times a second.
- **Spans carry the fill.** The scanline filler emits one horizontal run at a
  time and hands it to a span painter, so solid, two gradients and six
  patterns are four span painters over ONE geometry walk. Everything is drawn
  from `fb_fill_rect` / `fb_hline` / `fb_pixel` / `fb_text` and nothing else,
  so UOSHOW.UNO imports the same short list UOWORD.UNO does.
- **The renderer clips to the slide.** A shape half-dragged off the edge is
  normal in PowerPoint and harmless full-screen; in the sorter the overhang
  lands on the next thumbnail. It cannot use `fb_set_clip` (see the trap
  above - that governs `fb_aa`'s alpha primitives, not fb's), so every span
  and every line passes through the file's own clip rect. The gate renders
  a slide whose bottom row deliberately hangs over the edge and counts the
  pixels outside the rectangle: the answer must be zero.
- **Three pools, shared by the whole presentation** - shapes, paragraphs and
  characters - with slides holding indices. This is the UnoCalc lesson
  applied at design time rather than after a 104 MB link. The invariant that
  makes compaction work is stated in `uos_model.c`'s header: a shape's
  paragraph run and its text are both contiguous, and sorting shapes by
  `para_at` sorts them by text offset too. `uos_para_add` re-homes the run's
  text as well as its paragraphs precisely to keep that true.

### The app

Four views (Slide, Outline, Slide Sorter, Notes Page), the 24 AutoLayouts as
a picker, Apply Design / Colour Scheme over the eight scheme roles, text
typed straight into a placeholder with Tab and Shift+Tab demoting and
promoting, autoshape insert, and **the slide show full-screen** - F5, click
or Space or Right to advance, Left to go back, `B` for a black screen, Esc
out, hidden slides skipped.

**Builds (Custom Animation) do not exist.** OFFICE97-PLAN §7 asks for builds
v1 - appear, fly-from, wipe, dissolve, grouped by paragraph level, with
after-animation dim - and none of it is implemented. What follows is about
slide TRANSITIONS, which are a different feature; the gap is filed as a
request for an animation facility in unoui, since a tween clock is not
UnoShow's to own.

**Transitions v1 are the ones a bounded number of renders per frame can
express**: Cut, Cut Through Black, Wipe x4, Box In/Out, Split H/V, Cover x4,
Blinds H/V, Random Bars H/V. There is no second framebuffer, so a transition
cannot cross-fade two bitmaps - it draws the outgoing slide and then the
incoming one through a moving window (`uos_clip`). Dissolve and Checkerboard
need a per-cell mask over the whole slide, which is 192 renders a frame, so
they are honestly absent rather than approximated by something that stutters.

### Four traps this phase found, three of them not UnoShow's

1. **`uod_open` takes the frame's SIZE, not a position.** It centres the
   dialog in a `sw` x `sh` box. Passing a top-left corner puts the dialog at
   a quarter of the screen with its list somewhere else again.
2. **The OK/Cancel row is not automatic** - a dialog declares its own
   buttons, as uoword's Font dialog does. A picker without them opens, looks
   finished, and can only be dismissed with Esc.
3. **Scan codes are the FIRMWARE's**, not PS/2 set 1: Up 0x01, Down 0x02,
   Right 0x03, Left 0x04, PageUp 0x09, PageDown 0x0A, F5 0x0F, Esc 0x17. A
   PS/2 table compiles, looks plausible, and every key silently does nothing.
4. **`uno_font_*_styled` needs a scalable TTF slot, and there may not be
   one.** The system font is slot -1 (the 8x8 bitmap) until the user picks a
   face, and a styled call with -1 falls back to `fb_text`, which has exactly
   one size - so a 44pt title and a 20pt bullet draw identically and the
   full-screen show draws text the size the editor does. Slot 0 is no better:
   it is CHICAGO.TTF, a bitmap-style face pinned to a 15px grid, which also
   ignores the size it is asked for. Both apps now choose the face by
   MEASURING (the first slot whose width answer changes with px) and cache
   it. **UnoWord had this bug too** - its zoom moved the page and never the
   text - and it is fixed in the same commit.

## File I/O

All three apps read and write their native Office 97 format through unodoc,
and each module links only its own half of the library - UnoWord carries
`ud_doc`/`ud_docw`, UnoCalc `ud_xls`/`ud_xlsw` plus the ptg compiler, UnoShow
`ud_ppt`/`ud_pptw` plus Escher - so none of them pays for another's format.

The mapping in each app is deliberately **symmetric: what save writes, open
reads back**. That is the property the round-trip test checks, and it is the
one a writer and a reader that are each self-consistent can quietly fail.

- **UnoCalc**: values by kind, and a **formula round-trips as its TEXT** -
  unodoc decompiles the ptg array on the way in and recompiles it on the way
  out, with the cached result carried alongside so a reader that does not
  calculate still shows the right number. An expression unodoc's compiler
  refuses does not lose the cell: the value it produced is written instead.
  Excel's on-disk error codes and `uxl`'s are two enumerations of the same
  seven values and are mapped, never cast.
- **UnoShow**: unodoc writes a slide as a title frame and a body frame with
  `\n` between paragraphs, so the first paragraph is the title and the rest
  are the body. Shapes drawn by hand do **not** survive - the writer is
  title-and-body only.
- **UnoWord**: plain text through the piece table (`.doc` styles on write are
  a standing request to unodoc).

### The bug the round-trip found

`uos_slide_set_layout` added the new layout's placeholders and never removed
the old ones. A Title Slide reopened as a Bulleted List kept its centre-title
and subtitle frames underneath the new ones, so the slide showed two
overlapping "Click to add text" prompts. Applying a layout now deletes
placeholders the new layout has no use for **when they are empty** - which is
what PowerPoint does: it keeps content and drops the frame.

### The test

`pc64/tools/uofile_urc.py` types content, saves it, clears the document with
File > New, reopens the file and reads the content back off the screen. It is
the only test that catches a writer and a reader that agree with themselves
and not with each other, and its first cut is worth remembering: it assumed
File > Save was the menu item at y=109, which is Open. Every step after that
cascaded - it "saved" by opening a file that did not exist, and then reported
a successful round-trip of data that had never left the grid. **Read the
coordinates off a screenshot.**

## Build integration

`pc64/build.sh` gains a **UOWORD.UNO block beside PHOTOS'** - the app plus the
whole uoffice lane plus unodoc's Word half, statically linked into the module,
so the kernel gains no document code. `pc64_modload.c` gained one `KX()` line
(`uno_fs_isdir`, which the file dialog needs to tell a folder from a document)
and `pc64_uui.c` an `EX_UOWORD` slot in the same shape as `EX_PHOTOS`. All
three are appends, which is how those choke-points are meant to grow.

## Changelog

- **2026-08-03 - file I/O.** UnoCalc reads and writes `.xls` (values, number
  formats and formulas as text, through unodoc's ptg compiler and
  decompiler); UnoShow reads and writes `.ppt`. Before this, UnoCalc's Save
  said "not in this build yet" and **UnoShow's File > Open and Save had no
  handler at all** - they fell through to `default:` and silently did
  nothing. Gate: `pc64/tools/uofile_urc.py`, which round-trips both on a
  booted machine, plus the six host gates and prod/debug builds. It found one
  model bug (empty placeholders surviving a layout change, above). Two gaps
  are now filed rather than left implicit: builds/Custom Animation, and
  printing.
- **2026-08-03 - phases 11 and 12.** `uoshow.h` + `uos_geom.c` + `uos_model.c`
  + `uos_render.c` (20 autoshapes, 8 colour schemes, the 24 AutoLayouts, the
  pooled store and the one renderer) and `pc64/apps/uoshow.c` ->
  `APPS\UOSHOW.UNO`. Gates: a sixth host harness at 66 checks and 4
  storyboard frames, production and debug pc64 builds, and **the app driven
  on screen over URC** (`pc64/tools/uoshow_urc.py`): a title typed into a
  placeholder, New Slide picked from the AutoLayout dialog, the Slide Sorter
  showing both slides, and the show taking the whole screen and coming back.
  One shared-code addition: `pc64_shell_fullscreen()` +
  `pc64_shell_is_fullscreen()`, because `unoui_fullscreen()` takes the
  shell's own `UI` and a module has no way to name it - the same append
  UnoWord made for `uno_fs_isdir`. Four traps recorded above; the font-slot
  one was a live bug in UnoWord as well.
- **2026-08-03 - phases 9 and 10.** `uocalc.h` + `uxl_sheet.c` + `uxl_calc.c`
  + `uxl_numfmt.c` (the sparse store, the RPN compiler and evaluator, 73
  functions, the number-format language) and `pc64/apps/uocalc.c` ->
  `APPS\UOCALC.UNO`. Gates: a fifth host harness at 110 checks, production
  and debug pc64 builds, and **the app driven on screen over URC**
  (`pc64/tools/uocalc_urc.py`): typing into cells, `=A1*A2` computing to 42,
  the formula bar showing the source text back, three menus opened by mouse
  and the About box dismissed. Two things this phase changed for everyone
  else: the chrome paint is now split (`uoc_render_bars` /
  `uoc_render_popups`, see the trap above, which also fixed UnoWord), and
  `urcui.py` resolves an app by the **title of the window it opens**
  (`launch_named`) rather than by slot index - `uoword_urc.py` had silently
  driven UnoCalc the moment UnoCalc took the last slot.
- **2026-08-03 - phases 7 and 8.** `uoword.h` + `uow_doc.c` + `uow_layout.c`
  (the piece-table model, run-list formatting, styles with based-on chains,
  undo/redo, and paginated layout with justification and widow control), and
  `pc64/apps/uoword.c` -> `APPS\UOWORD.UNO`. Gates: a fourth host harness
  plus production and debug pc64 builds and the QEMU diskboot gate. Three
  bugs the layout gate caught are worth repeating: direct runs seeded with
  Normal's own values BEAT every style applied later; justification spread
  slack between FORMATTING runs, so uniform text never justified at all; and
  the pagination fixture fitted on one page, which is a correct answer to the
  wrong question. **Not yet verified: the app driven on screen.**
- **2026-08-03 - phase 6d.** `uobars.c` (status bar, ruler, Assistant) and
  `uofile.c` (Open / Save As over a filesystem seam). Gate: 11 frames over a
  fake volume set. The gate's first cut assumed the filesystem's raw order
  and was corrected by the engine: directories sort first, wear a trailing
  backslash, and picking one does not fill the name field.
- **2026-08-03 - phase 6c.** `uodlg.c`: the dialog engine, and with it the
  claim that Office's thirty dialogs are one engine and thirty tables. Gate:
  11 frames driving Word's Font dialog, declared as a data table.
- **2026-08-03 - phase 6b.** Docking on four edges and floating, combo lists,
  split buttons with tear-off palettes, ScreenTips, and `uoicons.c`. Gate
  grew to 23 frames. Two bugs it caught: the atlas was sized for 32 icons and
  UBSan found the overrun at 35 (it is derived from `UOI_COUNT` now), and
  combo text was not clipped - see the fb_set_clip trap above.
- **2026-08-02 - phase 6a.** `uochrome.{h,c}`: the menu bar, full static menus
  (16x16 icon gutter, separators, submenus, checkmarks and radio bullets,
  disabled items with the Windows 95 white emboss, a right-aligned accelerator
  column, always-underlined mnemonics - Windows 95 showed them unconditionally;
  hiding them until Alt is a Windows 2000 behaviour and would be wrong here),
  docked toolbars of flat buttons that raise on hover and sink when pressed or
  toggled, grippers, separators and combo fields, and the whole keyboard model
  (F10, Alt+mnemonic, arrows, Enter, Esc). All surface `[EXPERIMENTAL]`.
  Gate: 16 storyboard frames, green.
