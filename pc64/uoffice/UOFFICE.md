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
| 6b | floating/docking, combo lists, split buttons + tear-off palettes, icon artwork | not started |
| 6c | the dialog engine (tabs, group boxes, message box) | not started |
| 6d | file Open/Save dialog, status bar, rulers, the Assistant | not started |
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

### What 6a does NOT do

Floating and docking (toolbars are drawn docked, in declaration order), combo
**lists** (the field and its drop button are drawn and their space reserved;
clicking one does nothing yet), split buttons and tear-off palettes, ScreenTips,
the Customize dialog, and any dialog at all. Those are 6b-6d.

## The host gate

```bash
cd pc64/uoffice && ./build.sh
```

Builds `pc64/tools/uochrome_test.c` against the same `uochrome.c` the module
compiles freestanding, linked to the shared software framebuffer
(`ps2/fb.c` + `fb_aa.c`) exactly as unoui's harness is, and drives a **scripted
`unoui_event` stream** - the same event stream pc64 feeds. Sixteen frames land
in `build/uoc_*.ppm` plus a contact sheet.

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

- **2026-08-02 - phase 6a.** `uochrome.{h,c}`: the menu bar, full static menus
  (16x16 icon gutter, separators, submenus, checkmarks and radio bullets,
  disabled items with the Windows 95 white emboss, a right-aligned accelerator
  column, always-underlined mnemonics - Windows 95 showed them unconditionally;
  hiding them until Alt is a Windows 2000 behaviour and would be wrong here),
  docked toolbars of flat buttons that raise on hover and sink when pressed or
  toggled, grippers, separators and combo fields, and the whole keyboard model
  (F10, Alt+mnemonic, arrows, Enter, Esc). All surface `[EXPERIMENTAL]`.
  Gate: 16 storyboard frames, green.
