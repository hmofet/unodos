# Modern window management, implementation spec (worker brief)

Status: SPEC, 2026-07-31. Nothing implemented. The design rationale is in
[`WM-MODERN-PLAN.md`](WM-MODERN-PLAN.md); this document is the build order.
Where the two disagree, THIS file wins.

The claim is already filed: `pc64/UNOAUTOMATE-REQUESTS.md`, entry
"2026-07-31 - CLAIM (toolkits lane): unoui window-management modernization".
That entry also carries the one cross-lane request (usb stack: HID modifier
byte) and an FYI to the BIOS-boot lane. Do not re-file; do append your own
dated progress notes there as phases land.

## 0. Read first, in this order

1. `/AGENTS.md` (the working agreement; you are the **toolkits lane**)
2. `docs/WM-MODERN-PLAN.md` (the design and its reasoning)
3. `unoui/unoui.h`, `unoui/unoui_theme.h` (the toolkit contract)
4. `unoui/unoui_input.c` (mouse capture: `UI_CAP_WINDOW` at :655, the
   titlebar/close hit-test at :725-:762, `clamp_win`)
5. `unoui/unoui.c` (`unoui_render_ui` at :1129, `UNO_BG_CACHE` at :1104,
   the generic resize-grip painter at :1197 - your model for generic
   titlebar buttons)
6. `pc64/pc64_uui.c` (the shell: apps table ~:77, taskbar ~:1257,
   `open_app`/`close_focused` ~:1683/:1846, session save/load ~:1876,
   `pump_input` ~:2532, the frame loop + drag fast path ~:2900-:3080)
7. `pc64/uefi_main.c` input section only (`raw_push`/`uno_pc64_next_key`
   ~:1300, `map_key` ~:1369, `uno_pc64_scene_save/restore` :179)
8. `pc64/harness.py` (the QEMU gate you will extend)

## 1. Ground rules

- **Lane.** You own `unoui/*` and the pc64 shell (`pc64_uui*`, `pc64_write/
  files/...` untouched, `pc64_native.c` PS/2, the input section of
  `uefi_main.c`). You do NOT touch: `bios_entry.c`, `boot/`, `usbhid.*`
  (usb lane - the modifier byte is a filed request, work around it),
  `unoauto*`, or any per-port directory.
- **Branches.** One worktree + branch per phase, off `origin/master`:
  `git worktree add ../unodos-wm-<p> -b wm-<p> origin/master`. Rebase at
  session start. Land each phase (rebase-then-ff or squash), delete the
  branch + worktree the same day. Never merge one phase branch into another;
  if phase N+1 needs unlanded phase N, land N first.
- **Merge gate per phase**: builds `UNO_DEBUG=0` AND `UNO_DEBUG=1`
  (`cd pc64 && ./build.sh` / `UNO_DEBUG=1 ./build.sh`, in WSL); the unoui
  host build stays green (`cd unoui && ./build.sh` - it renders every theme
  to a contact sheet, catching layout breakage); the phase's harness scenario
  passes (§9); no choke-point restructure (append-only in `build.sh`'s file
  list, `pc64_modload.c` KX arrays if you export anything, etc.).
- **ABI discipline.** `unoui.c` + themes also build into the PS2/Dreamcast
  ports and the host demo. Every struct change is APPEND-AT-END. Theme
  structs use positional initializers, so appended `unoui_metrics` fields
  read 0 in every theme you do not edit; 0 must always mean "feature absent,
  behavior identical to today". Same rule for `unoui_ui` and `unoui_window`
  fields: zero-initialized state must reproduce current behavior exactly.
- **Style.** Match the surrounding code: C with `/* */` comments,
  declarations at block top, 4-space indent, `snake_case`, `g_` file-scope
  globals, ASCII only. Comments state constraints and invariants, not
  narration. No new libc dependencies.
- **Commits.** Small, one lane per commit; a shared-seam touch (e.g. the
  `build.sh` list) is its own `seam:`-prefixed commit.

## 2. New contract surface (summary table)

All names are final; do not rename.

| Addition | Where | Notes |
|---|---|---|
| `UI_MOD_GUI = 8` | `unoui.h` mods enum | append after ALT |
| `UI_ACT_MIN 9998`, `UI_ACT_MAX 9997` | `unoui.h` | same contract as `UI_ACT_CLOSE`: `value` = z-index |
| `UI_SNAP_NONE/MAX/L/R/TL/TR/BL/BR` (0..7) | `unoui.h` | enum |
| `unoui_window`: `unsigned char snap; unoui_rect restore_r;` | end of struct | |
| `unoui_ui`: `unoui_rect work; int live_drag; int snap_preview; unsigned last_press_ticks; int last_press_x, last_press_y;` | end of struct | `work` set by `unoui_ui_init` to the full screen |
| `unoui_rect unoui_snap_rect(const unoui_ui *, int snap)` | `unoui.c` | pure geometry over `work` |
| `void unoui_render_window(unoui_ui *, unoui_window *)` | `unoui.c` | one window, chrome + widgets, correct focus/hot state |
| `void unoui_snap_apply(unoui_ui *, unoui_window *, int snap)` | `unoui.c` | saves/restores `restore_r`, applies rect, reflows |
| `unoui_metrics`: `int minbox; int maxbox;` | end of struct | 0 = theme has no such button |
| `int uno_pc64_next_key2(int *scan, int *uni, int *mods)` | `uefi_main.c` / `mac_compat.h` | ring gains a mods field; `uno_pc64_next_key` becomes a wrapper (`ctrl = mods & UI_MOD_CTRL`) |
| `int uno_pc64_mods(void)` | `uefi_main.c` / `mac_compat.h` | LIVE modifier state (held-now), for Alt-release detection |

Everything else (minimize state, desktops, groups, MRU, persistence) is
shell-private in `pc64_uui.c`.

## 3. Phase A - live drag, work area, double-click, geometry persistence

**unoui:**

1. Add the `unoui_ui` fields (§2). `unoui_ui_init` sets
   `work = {0, 0, sw, sh}`, everything else 0.
2. `clamp_win` (unoui_input.c) clamps into `ui->work` instead of the raw
   screen. With the default `work` this is behavior-identical.
3. `UI_CAP_WINDOW` move handler: when `ui->live_drag`, update `win->r.x/.y`
   directly (keep the existing 48 px keep-on-screen margins, expressed
   against `work`) and do NOT set `drag_active`; mouse-up just releases
   capture. When `live_drag == 0`, the outline path is byte-for-byte today's.
4. `unoui_render_window(ui, win)`: factor the per-window body of
   `unoui_render_ui` (the chrome + widget loop, :1157-:1213) into a helper
   both call. No behavior change to `unoui_render_ui`.
5. Double-click: on `UI_EV_MOUSE_DOWN` in a titlebar (the same test that
   starts `UI_CAP_WINDOW`), if the previous press was < 24 ticks ago and
   within 4 px, emit `UI_ACT_MAX` instead of starting a drag. Track with
   `last_press_ticks/x/y`. (`ui->ticks` advances via `UI_EV_TICK`, which the
   pc64 loop feeds ~60 Hz, so 24 ticks is ~400 ms.)

**pc64 shell:**

6. Set `UI.live_drag = 1` and `UI.work = {0, 0, FB_W, FB_H - TASKH}` at
   startup AND whenever the theme/font changes (TASKH follows the font).
   Retire `clamp_to_workarea` in favor of the unoui clamp where possible;
   keep it as a thin wrapper if call sites are awkward.
7. Replace the drag fast path in the frame loop (:3011): on drag start,
   temporarily remove the dragged window from `UI.win[]` (preserve index),
   `unoui_render_ui`, `uno_pc64_scene_save()`, reinsert; per moved frame
   `uno_pc64_scene_restore()`, `unoui_render_window(&UI, win)`, present;
   on release, full repaint. Keep the outline branch compiled (it runs when
   `live_drag` is 0, e.g. if a port of the shell ever wants it).
8. `UI_ACT_MAX` handling: toggle maximize via `unoui_snap_apply(...,
   UI_SNAP_MAX)` / restore. (Full snap semantics arrive in C; A only needs
   MAX + restore. Land `unoui_snap_apply` + the window fields now.)
   Windows without `UI_WIN_RESIZE`: ignore `UI_ACT_MAX` entirely in A.
9. Geometry persistence: extend `SHELL.CFG` (§8) with per-app
   `geomN=x,y,w,h`; save on drag/resize/snap commit and on close (the
   existing `session_save()` call sites plus one new call from the shell's
   action handler); apply after `open_app` builds the window, before first
   present. Unknown keys must be skipped by the parser (they already are:
   `cfg_line_val` matches whole keys).

**Gate A:** harness scenario `wm_a`: open Editor, drag it 200 px (window
visibly moves DURING the drag - assert via a mid-drag screenshot differing
from the start frame at the window's new location), double-click titlebar
(fills work area), double-click again (restores), reboot QEMU, window reopens
at its dragged position. Debug-HUD drag-frame cycles within 1.5x of the
pre-change outline drag (capture a baseline number first and record it in
your landing note).

## 4. Phase B - titlebar minimize/maximize buttons

1. `unoui_metrics`: append `minbox`, `maxbox` (§2). Set them for the themes
   where they make sense (Aurora Light/Dark, UnoDOS, Win 3.1, NeXTSTEP:
   same size as their close affordance or 13 px; Mac Plus, Mac OS 7, C64,
   Apple II, Amiga: leave 0). Note Win 3.1's `closebox` is 0 because its
   titlebar painter draws its own chrome; give it min/max via the generic
   painter anyway (classic triangles are a nice-to-have, palette rects are
   acceptable).
2. Generic painter: after `PICK(titlebar)` in the render path (both
   `unoui_render_ui` and `unoui_render_window` - i.e. in the shared helper),
   draw the enabled boxes right-aligned in the titlebar: [min][max] with
   4 px gaps, vertically centered like the close box. Palette only
   (`face/light/shadow/dark/text`), pressed/hot via existing `UI_F_*`
   conventions. Maximize shows a "restore" glyph when `win->snap ==
   UI_SNAP_MAX`. A window without `UI_WIN_RESIZE` draws its maxbox in the
   disabled style.
3. Hit-test in `unoui_input.c`, mirroring the close-box block (:750): emit
   `UI_ACT_MIN` / `UI_ACT_MAX`. Order of precedence: close, then min, then
   max, then drag. `UI_WIN_BARE` windows have no buttons (unchanged).
4. Shell minimize: `g_min[NAPPS]`. On `UI_ACT_MIN`: `remove_win`, set flag,
   `rebuild_taskbar`, focus the next MRU window (§6; until D lands, the top
   non-bare window). Restore: clear flag, `unoui_ui_add`, raise, focus.
5. Taskbar chips: parked (minimized) apps draw dimmed (existing palette,
   e.g. the chip at half intensity / no accent underline). Chip click:
   focused app -> minimize; minimized or unfocused -> restore + raise
   (the modern toggle). `open_app` on a minimized app restores it.
6. Keyboard: `Ctrl-M` minimizes the focused window (available today,
   ctrl-only ring). Add to the `pump_input` accelerator block; skip when a
   text edit has focus... it must NOT fire while typing in a field: gate on
   `UI.focus_wi` not being an edit-carrying widget, same as the Install
   window gates its accelerators.
7. Session: persist `minN=` flags (§8).

**Gate B:** `wm_b`: open 3 apps, minimize one via its button (shot: window
gone, chip parked), restore via chip (shot), chip-click the focused app
(minimizes), maximize via button on a resizable window and verify the rect
equals the work area, and verify Paint (non-resizable) shows a disabled
maxbox that does nothing.

## 5. Phase C - snapping (pointer + restore semantics)

1. `unoui_snap_rect`: MAX = `work`; L/R = halves; TL/TR/BL/BR = quarters.
   All derived from `ui->work` at call time, so a taskbar-height change
   just re-derives.
2. `unoui_snap_apply(ui, win, snap)`: entering a snap from `UI_SNAP_NONE`
   saves `restore_r = win->r` (only then - re-snapping keeps the original
   restore rect); applies the target rect; `unoui_reflow_window`;
   `UI_SNAP_NONE` restores `restore_r`. Non-resizable windows: never resize;
   position-only (centered in the target rect), and `snap` stays
   `UI_SNAP_NONE` (they have no snap state, they were merely moved).
3. Drag interaction (live drag only): while `UI_CAP_WINDOW` is captured,
   compute the zone from the POINTER position: within 8 px of work-area top
   = MAX, left/right edges = L/R, the 24x24 corners = quarters, else none.
   Set `ui->snap_preview` to the zone (0 = none). On release with a zone
   active, commit via `unoui_snap_apply` instead of the plain move.
4. Preview rendering: in the shared render helper, after all windows, if
   `snap_preview` draw `fb_blend_rect` of `pal.accent` at alpha ~56 over
   `unoui_snap_rect(ui, snap_preview)` plus a 1 px `pal.accent` frame.
   (`fb_blend_rect` exists in both fb backends.) The pc64 drag fast path
   draws the same preview after `unoui_render_window`.
5. Dragging a snapped window: on drag start, if `win->snap != UI_SNAP_NONE`,
   un-snap first: restore the SIZE of `restore_r`, keep the pointer's
   relative x position inside the titlebar (standard un-snap grab), clear
   `snap`.
6. Keyboard (lands fully in D, but wire what ctrl allows now if trivial;
   otherwise defer the bindings to D): Alt+Up = maximize/restore toggle,
   Alt+Left/Right = cycle NONE -> half -> (repeat = stay), Alt+Down =
   restore, then minimize if already restored.

**Gate C:** `wm_c`: drag Editor to the left edge (mid-drag shot shows the
translucent preview), release (left half exactly); drag it off (size
restored, snap cleared); drag to top (maximized); drag Browser to a corner
(quarter). Repeat one snap under the Win 3.1 theme to prove the preview is
visible on a flat palette.

## 6. Phase D - modifier plumbing + Alt-Tab (parallel to A-C)

This phase touches `uefi_main.c` (input section ONLY) and `pc64_native.c`.

1. Ring: `gRawK` entries become `(scan, uni, mods)`. `raw_push` takes mods.
   `map_key` already RECEIVES a Mac-style `short mods` (the UEFI Ex reader
   passes real state at :1440; other callers pass `cmdKey`-or-0): translate
   to `UI_MOD_*` (cmd/ctrl -> CTRL, shift -> SHIFT, option -> ALT, add a
   GUI flag) at the `raw_push` call. `uno_pc64_next_key` stays as the
   compat wrapper (`*ctrl = (mods & UI_MOD_CTRL) != 0`) so
   `pc64_accounts.c` and stragglers build untouched; the shell migrates to
   `uno_pc64_next_key2`.
2. Live state: `uno_pc64_mods()` returns currently-held `UI_MOD_*`.
   Sources: the UEFI Ex `KeyState` path latches per keystroke (good
   enough attached: state refreshes on every event; document that Alt-held
   with no key events reads stale on pure-ConIn firmware); the PS/2 path
   (`pc64_native.c`) tracks make/break of 0x2A/0x36 (shift), 0x1D (ctrl),
   0x38 (alt), E0-5B/5C (GUI) directly, which is authoritative when native
   input owns the keyboard (the normal detached case). USB HID modifiers
   arrive only after the usb-lane request lands; until then `uno_pc64_mods`
   simply never reports ALT/GUI from USB keyboards - every Alt feature must
   therefore keep a ctrl-reachable fallback (they do: F2/Ctrl-Tab, Ctrl-M,
   Ctrl-F1..F4).
3. MRU: shell-side stack of app indices, updated on every focus change
   (fold into `open_app`, the taskbar chip path, and the `UI_EV_MOUSE_DOWN`
   focus change - one helper `wm_note_focus(a)`).
4. Alt-Tab switcher: a TOP bare window with one canvas, centered strip of
   icon+name cells for open (incl. minimized) apps in MRU order. Alt+Tab
   opens it / steps; Alt+Shift+Tab steps back; commit (raise+focus, restore
   if minimized) when `uno_pc64_mods()` drops ALT, polled in the frame
   loop; Esc cancels. F2 / Ctrl-Tab drive the SAME overlay and MRU order,
   committing on a ~0.8 s timer after the last step (they have no reliable
   release edge). This replaces `cycle_window()`'s blind rotation.
5. Snap keybindings from §5.6, plus Alt+D = minimize all ("show desktop",
   restores the same set on repeat), Alt+Ctrl+F1..F4 reserved for E.

**Gate D:** `wm_d`: QEMU sends Alt+Tab (QMP `send-key alt-tab` holds both -
use separate press/release events to hold Alt across two Tab steps), shot of
the overlay, release commits the right window; Alt+Left snaps; Ctrl-Tab
fallback cycles with the overlay visible. PS/2 mods verified in the same run
(OVMF routes QEMU keys through its ConIn/Ex - to exercise the PS/2 tracker,
run the scenario once against the detached/native-input boot the debug build
performs; the harness's existing detach flow covers this).

## 7. Phase E - virtual desktops

Shell-only. Four desktops, fixed.

1. State: `g_cur_desk`, `g_desk_of[NAPPS]` (assignment; new windows open on
   the current desktop), `g_dz[4][NAPPS]` per-desktop z-order (app indices,
   bottom-to-top, -1 terminated) captured on every switch-away.
2. Switch: capture current z-order; `remove_win` every non-bare app window;
   re-add the target's windows bottom-to-top (skipping minimized); focus the
   target's MRU window (fall back to top); close launcher/calendar
   popovers; if a fullscreen game is live, exit fullscreen first;
   `g_dirty = 1`. Desktops share the wallpaper/icon layer and taskbar.
3. Pager: `[1][2][3][4]` cells drawn in `taskbar_draw` between Start and
   the chips (current = accent, occupied = a 2 px dot), hit-tested in
   `taskbar_event`. Chips list ONLY the current desktop's apps; an app
   opened from Start/desktop-icon while assigned elsewhere switches to its
   desktop and raises (principle of least surprise for single-instance
   apps).
4. Keys: Ctrl+F1..F4 switch (EFI scans 0x0B..0x0E, matching the existing
   F2 = 0x0C convention). Alt+Ctrl+F1..F4 move the focused window to
   desktop N (and follow it).
5. Context menu (F) gets "Move to desktop N"; do the keybinding now.
6. Persist `deskN=` per app and `cur_desk=` (§8); restore reopens windows
   onto their desktops and lands on the saved current one.
7. Stress hooks: add `pc64_dbg_wm_desk(int n)` next to the existing
   `pc64_dbg_*` shims so the soak run switches desktops.

**Gate E:** `wm_e`: open Editor on 1, switch to 2 (empty desktop shot),
open Files, pager shows dots on 1+2, Ctrl+F1 back (Editor there, Files
not), Alt+Ctrl+F2 moves Editor to 2 (follows it, both apps there), reboot:
same layout, same current desktop.

## 8. SHELL.CFG v2 (consolidated format)

Line-oriented `key=value`, CRLF, additive; old builds ignore unknown keys,
new builds default absent keys to today's behavior. Written by
`session_save()` as one buffer (raise the buffer size accordingly; keep it
static). Only restorable apps (`app_restorable`) get per-app lines.

```
restore=1
open=0,2,14
cur_desk=1
geom0=40,20,520,380
snap0=0            ; UI_SNAP_* value
min0=0
desk0=1
```

`geom` applies before `snap` (snap re-derives from the live work area, so a
saved maximized window tracks a font-size change instead of restoring a
stale rect).

## 9. Phase F - groups, tiling commands, context menu, taskbar overflow

1. **Link groups**: `g_group[NAPPS]` (0 = none, 1..4 = group id). Semantics:
   drag one -> the set moves by the same delta (fast path snapshots the
   scene minus the whole set; each frame renders every member); raise one ->
   raise the set (preserving intra-set order, members directly above the
   grabbed one); minimize/restore/desktop-move apply to the set. Titlebar
   dot: 6 px, right of the buttons, accent-family color per id (generic
   painter; shell exposes the group id via a small query hook the painter
   calls - keep unoui ignorant of groups except that one optional
   badge hook: `extern int (*unoui_win_badge)(const unoui_window *)`,
   returning a palette index or -1, default NULL).
2. **Context menu**: right-click on a titlebar or a taskbar chip opens a
   shell popover (the `g_launch`/`launcher_at` pattern - a small TOP window
   with a list canvas; do NOT reuse `ui->popup_*`, it requires an owner
   widget). Items: Restore / Minimize / Maximize / Snap left / Snap right /
   Move to desktop 1-4 / Group: none,A,B / Close. Right-click enters
   `pump_input` where the desktop-launcher right-click already does; add
   the titlebar/chip hit tests BEFORE the desktop test.
3. **Tiling commands** (taskbar blank-area right-click menu + Start menu
   entries): Tile (1 window = maximize; 2 = halves; 3-4 = quarters; n > 4 =
   grid with `ceil(sqrt(n))` columns, row-major), Cascade (staggered 24 px
   steps from the work-area origin, resets `snap`), Minimize all. Tile uses
   `unoui_snap_apply` geometry for 1/2/4 and direct rects for grids;
   non-resizable windows center in their cell.
4. **Taskbar overflow**: when the next chip would cross into the tray,
   draw a final `>>` chip instead; clicking it opens a popover listing the
   remaining apps (icon + name + parked state, click = same as chip click).
   Removes the silent `break` truncation.

**Gate F:** `wm_f`: group Editor+Files via the menu, drag one (both move,
mid-drag shot), minimize one (both park), ungroup; open 4 apps, Tile
(quarters shot), Cascade (staggered shot); open enough apps to overflow
(the games make this easy), `>>` popover shot, activate one from it.

## 10. Testing infrastructure (build once, in phase A)

Add `wm` scenarios to `pc64/harness.py` as functions (`wm_a`..`wm_f`),
runnable individually (`python3 harness.py wm_a`) and together
(`python3 harness.py wm`). Conventions:

- Shots into `shots/wm_<phase>_<step>.png`, named for what they assert.
- The harness's `mouse_move` normalizes against 640x480; parameterize it
  against the actual GOP resolution (read the mode the boot log prints, or
  make the scale a constant the scenario sets) before writing drag tests -
  a mis-scaled drag silently misses the titlebar and everything downstream
  "passes" by dragging nothing. Verify with a deliberate miss-test once:
  a drag aimed at empty desktop must NOT move the window.
- Every scenario asserts by pixel-comparing regions between its own shots
  (same run), never against committed golden images (themes/fonts drift).
- Run the full set under BOTH `UNO_DEBUG` builds before landing a phase.

Keep `pc64_dbg_*` parity: every new WM verb the harness needs
(minimize/restore/snap/desk-switch) gets a `pc64_dbg_wm_*` shim next to the
existing ones (`pc64_uui.c` end, `#ifdef UNO_DEBUG`), so the stress driver
exercises the real machinery.

## 11. Non-goals (do not build these)

- Tabbed window frames (grouping v2) - link groups only.
- A modal auto-tiling layout - commands only.
- Multi-instance apps, or raising `UNOUI_MAX_WINDOWS`.
- Any edit to `usbhid.*` (request filed), `bios_entry.c`, `boot/`,
  `unoauto*` dispatch (if the harness needs a URC verb, use the weak-symbol
  pass-through pattern and note it in the requests file).
- Per-theme hand-drawn button artwork beyond the generic painter.
- Animations of any kind.

## 12. Reporting

Append a dated entry to `pc64/UNOAUTOMATE-REQUESTS.md` when each phase
lands (one line is fine: phase, commit, gates run). Update the Status header
of `docs/WM-MODERN-PLAN.md` and this file's phase table as phases complete.
If you discover the usb-lane modifier request has landed mid-work, wire
`uno_pc64_mods()` to it and mark the request DONE with the commit. If metal
findings contradict this spec (e.g. Ex KeyState unreliable on a real
firmware), record the finding in `pc64/METAL-FINDINGS.md` and adapt; the
spec's fallback-first keybinding rule exists precisely for that case.

## Phase table (update as you land)

| Phase | Contents | State |
|---|---|---|
| A | live drag, work area, double-click max, geometry persistence | NOT STARTED |
| B | min/max buttons, shell minimize, parked chips | NOT STARTED |
| C | pointer snap + previews + restore semantics | NOT STARTED |
| D | mods byte, `next_key2`/`uno_pc64_mods`, Alt-Tab MRU switcher | **DONE** 2026-07-31 |
| E | virtual desktops + pager + persistence | NOT STARTED |
| F | link groups, tile/cascade, context menu, taskbar overflow | NOT STARTED |
