# unoui — the UnoDOS cross-platform UI toolkit

unoui is a small widget toolkit that lets an application's window UI be **written
once** and then rendered *and driven* on every UnoDOS target — with a unified
look by default, or a per-platform **theme** to make it look native. It lives in
[`unoui/`](../unoui/).

It is the same idea as [Uno3D](UNO3D.md), applied to look-and-feel instead of 3D:
a portable C core over the shared `fb.h` software framebuffer, plus a swappable
vtable. Uno3D swaps the rasteriser **backend**; unoui swaps the **theme**. And
just as Uno3D's geometry is platform-independent, unoui's *behaviour* is
platform-independent — it is a pure function of an abstract event stream, so the
only per-platform code an app needs is a tiny input adapter and the framebuffer
present.

> **Two different "GUI toolkits" in UnoDOS — don't confuse them.** The bare-metal
> x86 kernel has its *own* native widget library exposed through the `INT 0x80`
> API (see the "GUI Toolkit" section of the root [README](../README.md)). That
> one is hand-written assembly, lives in the kernel, and is what the shipping
> asm apps use. **unoui is a separate, portable C library** for the C-based ports
> (PS2, Dreamcast, the shared Mac/console core, …) and the host — the C analogue
> of the kernel toolkit, written once and themeable. See
> [§8](#8-relationship-to-the-kernel-native-widgets).

---

## 1. The idea: one widget tree, swappable theme, abstract input

unoui splits a UI program into parts that name no platform and parts that do:

```
  your app  (unoui_app.c)                 <- builds a widget tree once, names no platform
        |  unoui_* API  (unoui.h)
  portable core  (unoui.c)                <- layout, depth-aware drawing helpers,
        |                                     default painters, render dispatch
  portable input  (unoui_input.c)         <- hit-test, focus, drag, text editing,
        |                                     menus — a pure fn of unoui_event
        +--> theme vtable (unoui_theme.h)  <- palette + metrics + chrome painters
        +--> fb.h primitives               <- the shared software framebuffer
  ----------------------------------------------------------------------------
  per-platform glue (per port, tiny):
        * map native mouse/keyboard -> unoui_event, call unoui_handle()
        * call unoui_render_ui(), then upload `fb` to the display
```

The two halves of "write once, port with minimal effort":

1. **Looks the same / re-skins per platform** — a *theme* is colours you can
   change AND graphics you can override. The default theme gives a unified look
   everywhere; a platform theme reproduces a native look.
2. **Behaves the same everywhere** — all interaction (drag, focus, menus,
   multi-line text editing) derives from the abstract `unoui_event` stream. A
   port translates its hardware into those events; the resulting behaviour is
   identical on every platform.

---

## 2. Quick start

```c
#include "unoui_theme.h"

static unoui_ui     ui;
static unoui_window win;

void app_init(void)
{
    unoui_window_init(&win, "Hello", 40, 30, 240, 140);
    unoui_add_label (&win, 0, 0, "Name:");
    unoui_add_button(&win, 0, 40, 90, "OK", UI_F_DEFAULT);

    unoui_ui_init(&ui, &theme_unodos, FB_W, FB_H);
    unoui_ui_add(&ui, &win);
}

void app_frame(void)                 /* called once per video frame by the glue */
{
    unoui_event ev;
    while (port_poll_event(&ev)) {                 /* <- the only platform code */
        unoui_action a = unoui_handle(&ui, &ev);
        if (a.changed && a.kind == UI_BUTTON) on_ok();
    }
    unoui_render_ui(&ui);            /* desktop + windows + popups into `fb`     */
    port_present(fb);                              /* <- the only platform code */
}
```

Swap the look by passing a different theme — `unoui_ui_theme(&ui, &theme_win31)`
— with no other change.

Build the host proof: `cd unoui && ./build.sh` →
[`build/themes.png`](../unoui/build) (one window under every theme) and
`build/storyboard.png` (one scripted event stream driving the live app).

---

## 3. API reference (`unoui.h`, `unoui_theme.h`)

### 3.1 Building a window

`unoui_window_init(win, title, x, y, w, h)` then one `unoui_add_*` per widget.
Each builder returns the `unoui_widget*` so you can set `->id` (echoed back in
events) or tweak fields.

| Widget | Builder |
|---|---|
| Label | `unoui_add_label(win, x, y, text)` |
| Push button | `unoui_add_button(win, x, y, w, text, flags)` |
| Checkbox / radio | `unoui_add_check / _radio(win, x, y, text, on)` |
| Static field | `unoui_add_field(win, x, y, w, text, focus)` |
| **Editable field** | `unoui_add_edit(win, x, y, w, unoui_text*)` |
| **Multi-line text area** | `unoui_add_textarea(win, x, y, w, h, unoui_text*)` |
| Progress bar | `unoui_add_progress(win, x, y, w, value, max)` |
| Vertical / horizontal scrollbar | `unoui_add_vscroll / _hscroll(win, …, value, max)` |
| **Slider** | `unoui_add_slider(win, x, y, w, vmin, vmax, value)` |
| **Spinner** (numeric stepper) | `unoui_add_spinner(win, x, y, w, vmin, vmax, value)` |
| **Dropdown / combo** | `unoui_add_dropdown(win, x, y, w, items, n, sel)` |
| **Tab strip** | `unoui_add_tabs(win, x, y, w, items, n, sel)` |
| **Menu bar** | `unoui_add_menubar(win, unoui_menu*, n)` |
| List box | `unoui_add_list(win, x, y, w, h, items, n, sel)` |
| Group box / separator / desktop icon | `unoui_add_group / _sep / _icon(…)` |

Widget geometry is **relative to the window's content area**; the toolkit
translates it through `unoui_content_origin()`, the single source of truth that
both the painters and hit-testing use, so what is drawn is exactly what is
clickable. State is carried in `flags` (`UI_F_DEFAULT/PRESSED/FOCUS/DISABLED/
CHECKED`).

### 3.2 Editable text

```c
unoui_text body;  static char buf[512];
unoui_text_init(&body, buf, sizeof buf, /*multiline=*/1);
unoui_text_set(&body, "initial text");
unoui_add_textarea(&win, 0, 0, 300, 90, &body);
```

The app owns the `char` buffer; the toolkit edits it in place and tracks
`caret`, `sel` (selection anchor) and scroll offsets. `multiline` stores `\n` in
the buffer.

### 3.3 The event model — the portability contract

```c
typedef struct {
    ui_event_kind kind;   /* MOUSE_DOWN/UP/MOVE, KEY, CHAR, WHEEL, TICK */
    int x, y, button;     /* mouse                                       */
    int key;              /* UI_KEY_* virtual key for KEY                 */
    int ch;               /* ASCII for CHAR                              */
    int mods;             /* UI_MOD_SHIFT / _CTRL / _ALT                 */
    int wheel;            /* notches for WHEEL                           */
} unoui_event;
```

Virtual keys: `UI_KEY_LEFT/RIGHT/UP/DOWN/HOME/END/PGUP/PGDN/BACKSPACE/DELETE/
ENTER/TAB/ESC`. `UI_EV_CHAR` carries printable input separately from `UI_EV_KEY`,
so a port never has to disambiguate. `UI_EV_TICK` advances the caret blink.

### 3.4 Driving + rendering the UI

```c
unoui_ui_init (&ui, theme, screen_w, screen_h);
unoui_ui_add  (&ui, &window);            /* newest window = front = focused */
unoui_action a = unoui_handle(&ui, &ev); /* feed one event, get one result  */
unoui_render_ui(&ui);                    /* desktop + windows (z-order) + popups */
unoui_ui_theme(&ui, other_theme);        /* re-skin live, no other change    */
```

`unoui_handle` returns `unoui_action { changed, id, kind, value }` — `changed`
is nonzero when a widget activated or changed; `value` carries the new state
(toggle, slider/scroll position, selected index; for a menu pick,
`menu*256 + item`).

### 3.5 What the input layer handles uniformly (all platforms)

- Hover, click, and **window drag** (grab the title bar) with **click-to-front
  z-order**.
- Scrollbar arrows + **thumb drag** + mouse wheel; **slider knob drag**; list
  and tab selection; spinner steppers.
- **Menu bar** and **dropdown** popups — overlay drawn last, commit on click-in,
  dismiss on click-out or `Esc`.
- **Focus** and **Tab / Shift-Tab traversal**; `Enter` / `Space` activation;
  arrow-key navigation for sliders, spinners, tabs, lists.
- **Full multi-line text editing**: caret, mouse caret-placement and
  drag-select, `Shift`-arrow selection, `Home`/`End`, `Backspace`/`Delete`,
  insertion, and automatic horizontal/vertical scroll-to-caret. The painter and
  the hit-tester share `ui_text_index_at` / `ui_text_caret_xy`, so a click lands
  on exactly the glyph drawn.

`unoui_input.c` contains **zero platform code**.

---

## 4. Themes — colour AND graphics

A `unoui_theme` is three things ([`unoui_theme.h`](../unoui/unoui_theme.h)):

1. **`unoui_palette`** — semantic colour *roles* (`title_bg`, `button_face`,
   bevel `light`/`shadow`/`dark`, `accent`, `field_bg`, …), never literal
   colours. Swap the struct and every widget recolours. *(palette theming.)*
2. **`unoui_metrics`** — sizes (`title_h`, `frame_w`, `bevel`, `radius`,
   `shadow_off`, `title_center`) and the target colour **depth**
   (`UNOUI_DEPTH_FULL/8/4/1`).
3. **`const unoui_draw *draw`** — a vtable of chrome painters. Leave any entry
   `NULL` and the portable default is used; override one and that widget gets
   entirely custom **graphics**. `draw = NULL` entirely → an all-default,
   palette-only theme. *(graphics theming.)*

### Bit-depth handling

Shaded fills go through `ui_shade(x,y,w,h, theme, a, b, shade)` (5 shades). At
full depth it blends `a`→`b`; at 4-bit it snaps and lightly dithers; at 1-bit it
becomes a 4×4 ordered-dither **stipple**. So the same write-once chrome renders
correctly on a 1-bit Mac Plus *and* a 32-bit PS2 — the Mac Plus theme's iconic
50% grey desktop and the 4-bit themes' mid-tones all come out of this one call.
Helpers: `ui_px`, `ui_stipple`, `ui_bevel` (raised/sunken, returns the inset
rect), `ui_round_frame` / `ui_round_fill` (corner-clipped rounded rects),
`ui_text_in`.

Geometry derives from each window's rect and the theme's metrics, so any
resolution works (640×448 PS2, 640×480 Dreamcast, …).

### Shipped themes

| Theme | Depth | What it exercises |
|---|---|---|
| `theme_unodos`  | full  | the unified house look — **palette only**, all-default painters |
| `theme_macos7`  | full  | rounded white windows, pinstripe title, close/zoom boxes, rounded default ring |
| `theme_macplus` | 1-bit | strict B&W — the dither/stipple path, racing-stripe title, drop shadow |
| `theme_win31`   | 4-bit | grey 3D — blue caption bar with control/min/max boxes, double-bevel buttons |
| `theme_amiga`   | 4-bit | Workbench 1.x — palette + a title-bar override (depth gadget) |
| `theme_c64`     | 4-bit | two-blue VIC screen — **palette only** |
| `theme_apple2`  | 1-bit | green-phosphor mono — **palette only** |
| `theme_next`    | 8-bit | NeXTSTEP chiselled greyscale — palette + a wider bevel metric |

---

## 5. Writing an app

Build the widget tree once (it names no platform), then let the toolkit drive it.
[`unoui_app.c`](../unoui/unoui_app.c) is the worked example — two windows
exercising every widget. The skeleton:

```c
/* once */
unoui_window_init(&win, "Settings", 30, 24, 360, 280);
unoui_add_menubar (&win, menus, 3);
unoui_add_tabs    (&win, 0, 12, 330, tabs, 3, 0);
unoui_add_edit    (&win, 0, 40, 200, &name_text);
unoui_add_slider  (&win, 0, 64, 200, 0, 100, 60)->id = ID_VOLUME;
unoui_add_button  (&win, 0, 100, 90, "OK", UI_F_DEFAULT)->id = ID_OK;
unoui_ui_init(&ui, &theme_unodos, FB_W, FB_H);
unoui_ui_add(&ui, &win);

/* per frame */
unoui_event ev;
while (port_poll_event(&ev)) {
    unoui_action a = unoui_handle(&ui, &ev);
    if (a.changed && a.id == ID_VOLUME) set_volume(a.value);
    if (a.changed && a.id == ID_OK)     commit();
}
unoui_render_ui(&ui);
port_present(fb);
```

---

## 6. Adding a new theme

One file, no core or app edits — the same contract as adding a Uno3D backend:

```c
/* themes/theme_foo.c */
#include "../unoui_theme.h"
static void foo_titlebar(const unoui_theme *t, const unoui_window *w) { /* custom */ }
static const unoui_draw foo_draw = { 0, 0, foo_titlebar, 0, /* …NULL… */ };
const unoui_theme theme_foo = {
    "Foo", { /* palette */ }, { /* metrics + depth */ }, &foo_draw
};
```

Add `extern const unoui_theme theme_foo;` to `unoui_theme.h` and list it in the
build. NULL painters fall back to the defaults, so a palette-only theme is just
the palette + metrics with `draw = 0`.

---

## 7. Porting unoui to a platform

Because the toolkit is pure C over `fb.h` and behaviour is a function of
`unoui_event`, a port writes only two small things:

1. **Input adapter** — translate native mouse/keyboard into `unoui_event` and
   call `unoui_handle()`. Map buttons/movement to `MOUSE_*`, printable keys to
   `CHAR`, navigation/editing keys to `KEY` with the `UI_KEY_*` virtuals, and
   modifier state to `mods`. Optionally feed a `TICK` per frame for caret blink.
2. **Present** — after `unoui_render_ui()`, upload `fb` to the display (the port
   already does this for everything else: PS2 textures it to the GS, Dreamcast
   copies it to the PVR, the host writes a PNG).

That's the whole port. `unoui.c`, `unoui_input.c`, and `themes/*.c` compile in
unchanged. The host harnesses ([`host_unoui.c`](../unoui/host_unoui.c),
[`host_unoui_input.c`](../unoui/host_unoui_input.c)) are the reference adapters:
the first renders the static window under every theme; the second feeds a
*scripted* event stream and snapshots the states, which is exactly what a real
port produces from the same gestures.

---

## 8. Relationship to the kernel-native widgets

The bare-metal x86 UnoDOS kernel ships its own widget library in assembly,
exposed through `INT 0x80` (Button, Checkbox, Scrollbar, Menu Bar, File dialogs,
word-wrap, …) and used by the shipping `.asm` apps. unoui does **not** replace
it and is not compiled into the asm kernel — UnoDOS apps there are 16-bit `.BIN`
files, not C (the same reason the Uno3D *library* can't be used by the x86 game,
which is hand-written asm over the kernel API instead).

unoui is the **C-side** toolkit: the portable widget library for the ports that
*are* C (PS2, Dreamcast, the shared Mac/console core) and the host. It mirrors
the kernel toolkit's widget set so a UI looks and behaves consistently across
both worlds, and it adds the theming engine and the abstract-event input model
that make "write once, run on every C port" hold.

---

## 9. Limitations / not yet done

- Not yet wired into each port's glue `main()` — every port still needs its ~20-
  line `unoui_event` adapter and `fb` present hookup. This is the intended
  "minimal porting effort," not missing capability.
- Render-only verification so far is on the host (software `fb` → PNG); the same
  code is what the consoles link, as with Uno3D.
- No horizontal text wrapping in the text area (no-wrap + horizontal scroll); no
  rich text; one font (the shared 8×8). Windows don't resize (drag-move only).
- Theming covers chrome; the new widgets inherit the default painters under all
  eight themes (only the original widgets have per-theme custom graphics in a
  few themes).

---

## See also

- [`unoui/README.md`](../unoui/README.md) — the toolkit's own readme + images
- [docs/UNO3D.md](UNO3D.md) — the sibling write-once library (3D), same pattern
- [`unoui/unoui.h`](../unoui/unoui.h), [`unoui/unoui_theme.h`](../unoui/unoui_theme.h) — the full API
- root [README](../README.md) "GUI Toolkit" — the kernel-native asm widget set
