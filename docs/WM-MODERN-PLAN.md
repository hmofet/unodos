# Modern window management for the unoui shell (pc64 first)

Status: PROPOSAL, 2026-07-31. Phases A, B, C and D are landed. The claim is filed in
`pc64/UNOAUTOMATE-REQUESTS.md`; the implementation brief a worker follows is
[`WM-MODERN-SPEC.md`](WM-MODERN-SPEC.md), which wins on any disagreement.
Lane: toolkits (unoui / `pc64_uui*`), per `/AGENTS.md`. One out-of-lane
dependency is called out in §10 (USB HID modifier byte, a request to the usb
stack owner).

## 1. Where we are

The window manager is split across two layers:

- **unoui (portable core, `unoui/`)**: a z-ordered window list
  (`UNOUI_MAX_WINDOWS` 24), flags `UI_WIN_BARE/BOTTOM/TOP/RESIZE`, focus,
  mouse capture, one titlebar control (the close box, emitted as
  `UI_ACT_CLOSE`), live resize with widget reflow, and **rubber-band outline
  drag**: `UI_CAP_WINDOW` moves only `drag_x/y` and the window commits its
  rect on mouse-up (`unoui_input.c:773`).
- **pc64 shell (`pc64/pc64_uui.c`)**: one static `unoui_window` per app
  (single instance), `g_open[]`, a taskbar canvas (Start button, one chip per
  open app, tray), a Start menu, desktop icons, `clamp_to_workarea()`, and
  session restore via `SHELL.CFG` (restore flag + open set only, no
  geometry).

What a window can do today: **close, move (outline), resize**. No minimize,
no maximize, no snapping, no desktops, no grouping, no Alt-Tab (F2/Ctrl-Tab
cycles blindly with no visual).

Why the outline existed, and why it can go: dragging used to re-run the full
alpha-blend scene painter per mouse move, which lagged. Two fixes since then
changed the economics:

- `UNO_BG_CACHE` (`unoui.c:1111`): the aurora backdrop is painted once and
  blitted thereafter.
- The scene snapshot fast path (`pc64_uui.c:3011`, `uno_pc64_scene_save/
  restore` in `uefi_main.c:179`): at drag start the shell renders the scene
  once, snapshots the framebuffer, and each drag frame is memcpy-restore +
  outline + present.

The same snapshot machinery supports an **opaque** drag at almost the same
cost: snapshot the scene *without* the dragged window, then per frame
restore + draw one window + present. That is the unlock for both live drag
and drag-edge snap previews.

Input constraint to know about: the raw key ring (`uno_pc64_next_key`) carries
`(scan, uni, ctrl)` only. No Shift for non-printables, no Alt, no Win/GUI.
Several modern keybindings need a mods byte (§10).

Known wart this plan also fixes: taskbar chips that do not fit before the tray
are silently dropped (`taskbar_draw`, `if (x + cw > tray_x - 4) break;`).

## 2. Design principles

1. **Mechanism in unoui, policy in the shell.** Everything a Genesis or host
   build could reuse (buttons, actions, snap geometry helpers, live drag)
   goes in unoui behind small additive APIs. Everything about *when* (keys,
   taskbar UI, persistence, desktops-as-a-concept) stays in `pc64_uui.c`.
2. **Additive, seam-friendly.** New window flags, new `UI_ACT_*` codes, new
   fields at the END of `unoui_window` / `unoui_ui`. No renames, no
   reordering; existing ports must not need edits to keep building.
3. **Degrade by capability.** Live drag needs an fb-size snapshot buffer;
   ports that cannot afford it keep the outline (same gate philosophy as
   `UNO_BG_CACHE`). Fixed-layout bridge apps (no `UI_WIN_RESIZE`) cannot
   snap-resize; they get move-snap only and a disabled maximize button.
4. **Theme-consistent, theme-cheap.** Ten themes exist. Titlebar buttons are
   drawn by ONE generic painter using theme palette + metrics (the same way
   the resize grip already is, `unoui.c:1197`), not ten hand-edited titlebar
   painters. A theme can opt out per button via metrics.

## 3. Live opaque drag (retire the outline as the default)

**unoui**: add an opt-in flag on the context, off by default so every other
port keeps today's behavior:

```c
int live_drag;   /* unoui_ui: 1 = UI_CAP_WINDOW moves win->r each frame */
```

With `live_drag`, `UI_CAP_WINDOW` mouse-move updates `win->r` directly
(clamped as today) and never sets `drag_active`; mouse-up is just capture
release. The outline path stays intact for ports that keep `live_drag == 0`.

**pc64 fast path** (keeps drags cheap at 4K-ish panels): generalize the
existing drag branch in the frame loop:

- Drag begins: temporarily hide the dragged window (swap it out of
  `ui->win[]`), `unoui_render_ui`, `uno_pc64_scene_save`, swap it back.
- Each moved frame: `uno_pc64_scene_restore`, draw just the dragged window
  (new entry point below), present.
- Drag ends: full repaint (commit shadows/occlusion).

New unoui entry point so the shell can draw one window with correct chrome
and focus state:

```c
void unoui_render_window(unoui_ui *, unoui_window *);  /* chrome + widgets */
```

Cost per drag frame: one memcpy + one window paint, versus today's memcpy +
outline. Measured risk is low; the debug HUD already times render/present, so
the QEMU gate can assert drag-frame cycles stay within budget.

Snap previews (§5) draw on top of the restored snapshot in the same pass, so
they are free riders on this path.

## 4. Titlebar buttons: minimize, maximize/restore

**unoui metrics** (additive fields at the end of `unoui_metrics`):

```c
int minbox;   /* minimize box size, 0 = theme has none */
int maxbox;   /* maximize box size, 0 = theme has none */
```

Draw generically after `PICK(titlebar)` (exactly like the resize grip):
right-aligned in the title bar, palette colors, pressed/hot states from the
existing `UI_F_*` machinery. Classic themes keep their look: Mac Plus / Mac 7
can set `minbox = 0` (System 7 had no minimize) or map it to a zoom-box
glyph; Win 3.1 gets the classic down/up triangles; Aurora gets flat glyphs.
Hit-testing mirrors the close-box test in `unoui_input.c:750` and emits:

```c
#define UI_ACT_MIN 9998   /* value = z-index, same contract as UI_ACT_CLOSE */
#define UI_ACT_MAX 9997
```

unoui does NOT implement minimize itself (it has no taskbar concept); the
shell handles the action:

- **Minimize** (`g_min[a]` in the shell): `remove_win(&g_win[a])`, keep
  `g_open[a] = 1`. The taskbar chip stays, drawn in a distinct "parked"
  style. Chip click on a minimized app restores + raises; chip click on the
  focused app minimizes (standard modern toggle). Restore re-adds the window
  at its old rect.
- **Maximize** is a snap state (§5) targeting the full work area.
- **Double-click titlebar = maximize/restore.** unoui gains a tick-based
  double-click detector (it already carries `ui->ticks`); threshold in the
  theme metrics is unnecessary, a constant ~350 ms is fine.

## 5. Work area, maximize, snapping

**Work area.** unoui currently clamps to the full screen and the shell
re-clamps to "screen minus taskbar" in its own helper. Promote the concept:

```c
unoui_rect work;   /* unoui_ui: the area windows tile/snap/maximize into */
```

Set by the shell (`0,0,FB_W,FB_H-TASKH`); `clamp_win`, snap and maximize all
use it. Default = full screen so other ports are unaffected.

**Snap states.** Per window, at the end of `unoui_window`:

```c
unsigned char snap;      /* UI_SNAP_NONE/MAX/L/R/TL/TR/BL/BR */
unoui_rect    restore_r; /* the pre-snap rect to give back */
```

Geometry is pure arithmetic on `work` (halves and quarters). Entering a snap
saves `restore_r` once; leaving (un-maximize, or dragging the titlebar off a
snap, which is the standard gesture) restores it. `unoui_reflow_window`
already handles the widget side.

**Pointer snapping** (needs live drag): while dragging, when the pointer
enters an edge zone (top = maximize, left/right = half, corners = quarter,
zone width ~8 px), draw a translucent preview of the target rect
(`fb_blend_rect` of accent at low alpha) over the restored snapshot; release
inside the zone commits the snap. Release outside commits the move as today.

**Keyboard** (after the mods work in §10): `Alt+Up` maximize/restore,
`Alt+Left/Right` cycle none/half-left/half-right, `Alt+Down`
restore-then-minimize. (Chosen over Win-key bindings because Alt is
detectable on every input path including pre-detach UEFI; Win/GUI arrives
only on native paths. If GUI proves reliable on metal we can alias Win+arrows
later.)

**Non-resizable windows** (bridge apps with fixed pixel layouts): maximize
button drawn disabled, snap zones only *position* them (centered in the half)
without resizing. This keeps Paint/Tracker honest instead of stretching them.

## 6. Alt-Tab switcher

`cycle_window()` exists but is invisible and last-used-order-blind. Replace
with a proper switcher, shell-side (it is pure policy):

- Maintain an MRU stack of app indices (updated on every focus change).
- Alt+Tab: overlay a centered strip (icon + name per open app, current
  selection highlighted) drawn as a TOP bare window; Tab steps, Shift+Tab
  steps back, Alt-release commits, Esc cancels. Requires Alt up/down edges
  from §10; until then F2/Ctrl-Tab keeps working but drives the same MRU
  logic and shows the same overlay (commit on key release of Ctrl, or after
  a short timeout for F2).

## 7. Virtual desktops

Four fixed desktops, shell-owned. unoui needs zero changes: a desktop switch
is remove-set / add-set on the existing z-list.

- **State**: `g_desk_of[NAPPS]` (assignment), `g_zorder[4][NAPPS]`
  (per-desktop z-order as app indices, rebuilt on switch), `g_cur_desk`.
  Shell chrome (desktop icons, taskbar) is shared across desktops. The
  launcher/calendar popovers close on switch. A fullscreen game pins its
  desktop (switching away exits fullscreen first, simplest correct rule).
- **Switch**: remove current set, add target set bottom-to-top from its saved
  z-order, focus its MRU window, `g_dirty = 1`. All O(nwin), no redraw cost
  beyond one frame.
- **Taskbar pager**: `[1][2][3][4]` buttons between Start and the chips
  (current desktop highlighted, a dot on desktops with windows). Chips show
  the current desktop's windows; opening an app opens it on the current
  desktop; the Start menu is desktop-agnostic. Clicking a chip for an app on
  another desktop switches there (chips-scope option can come later).
- **Keys**: `Ctrl+F1..F4` switch (works TODAY, ctrl is already in the ring);
  `Alt+Ctrl+F1..F4` (post-§10) or the context menu (§9) moves the focused
  window to desktop N.
- **Window count**: unchanged, still the same single-instance windows split
  across sets, so `UNOUI_MAX_WINDOWS` 24 still holds with headroom.

## 8. Grouping and tiling commands

**Grouping v1, link groups** (cheap, ships with desktops): a group id per
window, shell-side (`g_group[a]`, 0 = ungrouped). Group members:

- move together (dragging one drags the linked set, the live-drag fast path
  snapshots the scene minus the whole set),
- raise together (raise one brings the set above other apps, preserving
  intra-set order),
- minimize/restore together,
- switch desktops together.

A small colored dot in the titlebar (generic painter, palette accent per
group id) marks membership. Create/dissolve via the window context menu (§9):
"Group with next click", "Ungroup". This is the classic "window set" model;
it delivers the *behavior* of grouping without restructuring unoui.

**Grouping v2, tabbed frames** (explicitly future, not in this plan's
phases): windows docked as tabs in one frame. Needs a container concept in
unoui (a window hosting other windows' widget lists) and per-widget origin
remapping; the payoff is real but it should wait until some app actually has
multi-window workflows. Nothing in v1 paints us into a corner: a tab frame
can adopt the group id namespace later.

**Tiling commands** (not a modal tiling WM): taskbar right-click menu (blank
area) plus Start-menu entries:

- **Tile** current desktop's non-minimized windows: 1 window = maximize, 2 =
  halves, 3-4 = quarters, more = grid `ceil(sqrt(n))` columns. Resizable
  windows get the cell; fixed ones center in it.
- **Cascade**: staggered `restore_r` reset, the classic escape hatch for a
  lost window.
- **Minimize all / Show desktop** (`Alt+D` post-§10).

A modal auto-tiling mode (master+stack, retile on open/close) is a natural
later toggle per desktop, worth doing only if the commands see use.

## 9. Window context menu and taskbar overflow

**Titlebar right-click** (also chip right-click): reuse the existing popup
machinery (`ui->popup_*`, already global-clip aware): Restore / Minimize /
Maximize / Snap left / Snap right / Move to desktop 1-4 / Group with... /
Ungroup / Close. One new right-button path in the shell's `pump_input`
(right-click currently only serves the desktop launcher; a titlebar/chip hit
test slots in before that).

**Taskbar overflow**: when chips exceed the space before the tray, draw the
first N-1 plus a `»` chip opening a popup listing the rest (same popup
machinery). Fixes today's silent truncation.

## 10. Input: a real modifier byte

Extend the raw ring entry to `(scan, uni, mods)` with
`UI_MOD_SHIFT/CTRL/ALT/GUI`, plus Alt key up/down edge events for the
Alt-Tab hold-release model.

- **Additive seam**: keep `uno_pc64_next_key(scan,uni,ctrl)` as a wrapper
  (ctrl = mods & CTRL) so `pc64_accounts.c` and existing call sites build
  unchanged; add `uno_pc64_next_key2(scan,uni,mods)` and migrate the shell.
- **Sources**: UEFI `SimpleTextInputEx` key state exposes shift/alt/logo
  (attached path, `map_key` funnel in `uefi_main.c`); PS/2 scan codes 0x38 /
  0xE0 0x5B track Alt/Win in `pc64_native.c` (our lane); **USB HID boot
  report byte 0 already carries all eight modifier bits, but `usbhid.*` is
  the usb-stack lane: file a request** ("expose HID modifier byte alongside
  the key stream") in `pc64/UNOAUTOMATE-REQUESTS.md` and fall back to
  ctrl-only bindings on USB keyboards until it lands.

Keybinding summary once mods exist: Alt+Tab switcher, Alt+arrows snap,
Ctrl+F1..F4 desktops (works pre-mods), Alt+Ctrl+F1..F4 move-to-desktop,
Alt+D show desktop, Ctrl+W close (existing), Ctrl+Esc Start (existing).

## 11. Persistence: SHELL.CFG v2

Additive `key=value` lines (parser already line-oriented; unknown keys are
ignored by old builds, so the file stays forward/backward compatible):

```
restore=1
open=0,2,14
geom0=40,20,520,380,S0     ; x,y,w,h,snap-state per restorable app
desk0=1                    ; desktop assignment
min0=0                     ; minimized flag
cur_desk=1
```

Windows finally reopen where the user left them, on the desktop they were
on. Geometry saves piggyback the existing `session_save()` call sites (open/
close/drag-commit/snap-commit).

## 12. Ownership, coordination, phasing

- Everything above is the **toolkits lane** (`unoui/*`, `pc64_uui*`,
  `pc64_native.c` PS/2, the `map_key` funnel) except the USB HID modifier
  request (usb lane, §10). File the claim line ("taking unoui window
  management: live drag, min/max, snap, desktops, grouping") in
  `pc64/UNOAUTOMATE-REQUESTS.md` before starting.
- **BIOS agent**: no file overlap (`bios_entry.c`, `boot/`). The one shared
  surface is `uefi_main.c`; our edits there are confined to the input
  section (`map_key`, raw ring), not boot wiring. If the BIOS path grows its
  own key source it should feed the same raw ring, which the mods byte makes
  a cleaner contract anyway. Note it in the requests file so they see it at
  their next rebase.
- **Stress/debug hooks**: extend `pc64_dbg_*` with minimize/restore, snap,
  desktop-switch so the existing stress driver exercises the new machinery
  (same philosophy as `pc64_dbg_open_app`).

Phases, each a short-lived branch off `master` per AGENTS.md §3, each
landing green on both `UNO_DEBUG` builds and the QEMU gate:

| Phase | Contents | Depends on |
|---|---|---|
| A | live drag (`live_drag` + `unoui_render_window` + shell fast path), work area in unoui, double-click maximize, geometry persistence | - — **LANDED 2026-07-31** |
| B | min/max titlebar buttons + `UI_ACT_MIN/MAX`, shell minimize + parked chips, chip toggle, maximize-as-snap | A — **LANDED 2026-07-31** |
| C | pointer snap zones + previews, snap restore-on-drag, non-resizable policy | A, B — **LANDED 2026-07-31** |
| D | mods byte (`next_key2`, UEFI + PS/2 sources), Alt-Tab MRU switcher, Alt keybindings; USB HID request filed early so it can land in parallel | - (parallel to A-C) — **LANDED 2026-07-31** |
| E | virtual desktops + pager + Ctrl+F1..F4 + persistence | B |
| F | link groups, tile/cascade/show-desktop commands, window context menu, taskbar overflow | B, E |

Test plan per phase: `host_unoui` build for WM-logic unit checks (snap
geometry, MRU, desktop set-swap are pure functions there), `tools/
qemu_test.py` screenshot gates (drag a window opaquely, snap left, minimize/
restore, switch desktop, tile 3 windows), debug-HUD cycle budgets to prove
drag frames did not regress, and a stress-driver extension randomly
minimizing/snapping/switching for the soak run.
