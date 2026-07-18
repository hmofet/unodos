# UnoDOS `sms` code audit — performance, bugs, security

Scope: the **sms** port (Sega Master System, Z80 @ 3.58 MHz, VDP 315-5124 Mode 4,
8 KB work RAM), the `build.sh` default target (`build/unodos.sms`, 32 KB
cartridge). Files: `kernel.asm`, `wm_render.inc`, `dostris.inc`, `music.inc`,
`gen_data.inc` (generated). Unlike the NES/GG minimal ports this one has a real
tile **window manager** (create / draw / focus-raise / drag / close, z-ordered)
and a hardware-sprite cursor. Threat model for the security section: bare-metal
cartridge, flat 8 KB RAM (`$C000-$DFFF`), **no memory protection** — any OOB store
corrupts directly. The **only external input is the control pad** (`PORT_DC`),
read as 6 bits per frame; there is no network/filesystem/untrusted parser, so the
security surface is essentially nil and that section is short by design.

Line numbers are exact against the current tree (asm: `label` + line).

---

## 1. Performance

### The root cause
Rendering is **all-or-nothing**: any state change sets `v_dirty`, and
`maybe_redraw` (`wm_render.inc:10`) responds by calling `redraw_all`
(`wm_render.inc:18`), which:
- `clear_names` — blanks the **entire 32×28 name table** (`kernel.asm:271`,
  896 cells × 2 bytes = 1792 `OUT`s),
- `draw_desktop_base` — repaints the title bar + all 11 icons + labels
  (`wm_render.inc:43`),
- `draw_window` for **every** window in the z-list (`wm_render.inc:171`, a dozen
  `fill_cells` rects + title + content each).

There is **no damage tracking and no cached desktop**. This is the same shape as
the pc64 drag-lag: the cheapest visual change costs the most expensive paint.

### P1 — HIGH impact — dragging a window repaints the whole scene every frame

> **✅ FIXED (`6a5396a`)** — `drag_move` sets a damage clip to `union(old,new)`; `redraw_all` clears via a clipped fill; primitives clip to the window. Byte-identical (`build.sh test` drag, retro-shot).
`dispatch` (`kernel.asm:422`) → `drag_move` (`kernel.asm:524`) sets `v_dirty`
**every frame** while trigger-1 is held (`kernel.asm:573-574`). So while you drag
one window a few cells, the loop clears and repaints the full desktop and *all*
windows on every single frame. This is the port's headline cost.
**Fix:** damage-rect the drag — repaint only `union(old_win_rect, new_win_rect)`
plus the desktop cells that got revealed, restoring them from a cached desktop
base rather than `clear_names` + full desktop redraw.

### P2 — HIGH-ish — the same full repaint fires for one-cell changes

> **✅ FIXED (`f4aebe8`)** — `dirty_proc` clips music/Dostris content ticks to their own window (Theme left as a whole-screen CRAM recolour). Byte-identical (dostris + music autotests).
`v_dirty` is set for every trivial update: each **music note** advance
(`music.inc:132`), every **Dostris** move/gravity step (`dostris.inc:10`
`set_dirty`, called from `ds_left`/`ds_right`/`ds_rotate`/`ds_softdrop` and
`dostris_gravity`), and each theme cycle (`theme_cycle`→`v_dirty`). Advancing the
music progress bar by one cyan cell repaints the entire desktop and every open
window. **Fix:** a damage list the app tick appends to, or at minimum special-case
"only the focused window's content changed" to skip `clear_names` +
`draw_desktop_base`.

### P3 — MEDIUM — the full redraw runs with the display ON (raster race)
`redraw_all` never blanks the screen — there is **no R1 display-off** around it,
unlike the NES/GG `full_redraw`. And `maybe_redraw` is called *after* the VBlank
`halt` returns (`kernel.asm:205`), i.e. during active display. So the ~1800-byte
name-table rewrite streams to VRAM mid-frame, racing the raster — visible tearing
plus slow active-display VRAM access. **Fix:** blank the display for the duration
of a full redraw (as GG does, `$A0`→`$E0`), or shrink the write to a damage list
that fits in vblank.

### P4 — LOW — `clear_names` before `draw_desktop_base` is largely redundant
`redraw_all` blanks all 896 cells and then `draw_desktop_base` immediately
overwrites the title bar + icon rows again; only the cells vacated by moved/closed
windows genuinely needed clearing. Subsumed by P1/P2 once damage tracking exists.

*Checked and clean:* the VBlank ISR (`kernel.asm:132`) only ticks + sets a flag;
`cursor_to_sat` (`kernel.asm:404`) streams two sprite entries per frame (cheap);
the clock live-update (`app_ticks`, `wm_render.inc:558`) writes only the two
changed cells and only on a second rollover.

---

## 2. Security — memory corruption / reachability

**No untrusted-input surface.** External input is the control pad only
(`read_input`, `kernel.asm:315`): 6 bits, reduced to edges. Window geometry,
titles, icon labels, piece masks, the tune and the app-definition table are all
ROM constants (`wm_render.inc`, `music.inc`, `gen_data.inc`). Nothing is parsed
from an attacker source, so there is no reachable remote/device corruption
primitive. The review confirms the internal index math stays in bounds.

### S1 — LOW (defensive) — event ring has no full-guard and is never drained
`ev_post` (`kernel.asm:756`) appends at the tail and wraps at `EVQ_SIZE` (16)
with **no check that the ring is full**, and `ev_get` (`kernel.asm:784`) is
**never called** — the only producer is the `EV_MOUSE` post on a content click
(`kernel.asm:502`). Because tail is masked to `[0,EVQ_SIZE)` and each slot is 3
bytes, all writes stay inside `event_q` (`$C030`, 48 bytes, `$C030-$C05F`,
verified non-overlapping with `zlist`/`wintab`), so this is **bounded — not a
corruption bug** — but it silently overwrites/drops events and `ev_get` is dead
code. **Fix:** consume events (or delete the queue), and add a full-check if it is
kept.

### Board write path — checked safe
`dostris_lock` (`dostris.inc:283`) stores into `g_board` (`$C0A4`, 120 bytes) at
`by*10 + bx` **without an inline bound**, but each lock is preceded by
`piece_collide` (`dostris.inc:97`) at the same origin, which rejects any cell with
`bx >= BW` or `by >= BH` (unsigned `cp BW`/`cp BH`, `dostris.inc:113,124`) and
handles wrapped-negative columns with the same test. Net index `< 120`. Clean —
the same invariant as the NES/GG ports.

*Latent/defensive:* `drag_move`'s clamp computes `SCRCOLS - WW` / `SCRROWS - WH`
(`kernel.asm:540,566`); if a window's `WW>SCRCOLS` this underflows to a huge
clamp, but the largest `WW` in `app_def_tab` is 22 < 32, so unreachable.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `put2` only valid for 0..99; Dostris line count is unbounded
`put2` (`wm_render.inc:509`) emits two decimal digits via repeated `sub 10`; for
`A >= 100` the tens digit runs past `'9'`. Bounded callers (clock, music note) are
fine, but `g_lines` (`dostris.inc:393` `inc g_lines`) is an unbounded byte drawn
through `put2` in `dostris_draw_score` (`dostris.inc:588`). >99 cleared lines →
garbage tens glyph. Cosmetic, no memory effect. **Fix:** cap at 99 before display.

### B2 — LOW (latent/defensive) — window coords use only the low byte of word fields
The generated layout makes `WX`/`WY`/`WW`/`WH` **word** fields (`sys_gen.inc`:
`WX=2, WY=4, WW=6, WH=8`), but the code reads/writes them as single bytes
throughout (`ld a,(ix+WX)`, `ld (ix+WX),a`, e.g. `win_create` `kernel.asm:944`).
This is correct **today** only because every value is < 256 and RAM is zero-init
at boot (`kernel.asm:161-165`) so the high byte stays 0; it is a footgun if any
coordinate ever exceeds a byte. Plus the `drag_move` clamp underflow noted in S1.
**Fix:** either treat the fields as bytes in the Contract, or handle the full word.

*Checked and clean:* z-list `raise_window`/`close_window` shift+compact correctly
and keep `v_zcount` in range (`kernel.asm:657,694`); `find_window_at` walks
`zlist[0..zcount-1]` with correct bounds and hit-tests inclusive rects
(`kernel.asm:594`); `win_ptr` slot indices are bounded by the `MAXWIN` loop guards
in `launch_app`/`win_create`; `move_cursor` has underflow guards on all four
directions (`kernel.asm:336`); `event_q`/`zlist`/`wintab`/`g_board` RAM regions are
laid out without overlap.

---

## Suggested order of work
1. **P1 + P2** — the drag/redraw lag (damage tracking + cached desktop): the
   biggest felt improvement.
2. **P3** — blank the display around any remaining full redraw to stop tearing.
3. **B1** — clamp the Dostris line count.
4. **S1 / B2 / P4** — drain-or-delete the event queue, byte-vs-word hardening,
   redundant-clear cleanup.

Build/test note: assembles with `sjasmplus` (`build.sh`) to a raw 32 KB `.sms`;
`AUTOTEST` variants drive scripted pads (`wm_render.inc:626`). No toolchain was
run here — findings are from source review only.
