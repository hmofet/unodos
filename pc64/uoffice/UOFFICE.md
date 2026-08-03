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
| 7-8 | UnoWord | not started |
| 9-10 | UnoCalc | not started |
| 11-12 | UnoShow | not started |

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

## Build integration

There is **no `pc64/build.sh` block yet**. The kernel does not need uoffice
until the first Office app module lands (phase 8), and per `/AGENTS.md` §2 a
choke-point is touched only when it is actually needed, as an append. The apps
will ship PHOTOS-style: each module links its own private copy of uochrome plus
the unodoc halves it uses.

## Changelog

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
