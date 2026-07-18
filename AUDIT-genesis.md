# UnoDOS `genesis` code audit — performance, bugs, security

Scope: the **genesis** world (Sega Mega Drive / Genesis, 68000 @ 7.67 MHz, VDP
tile graphics, 64 KB work RAM), the `./build.sh` cartridge target — `kernel.asm`
plus its includes (`apps.i`, `games.i`, `pacman.i`, `sram.i`, `tape.i`, `bram.i`,
`theme.i`, `tracker.i`, `paint.i`, `softkbd.i`, `ps2.i`, `scheduler.i`) and the
generated Contract includes (`sysabi_gen.i`, `window.i`).

Threat model for the security section: bare-metal 68000 cartridge, **flat RAM,
no MMU / no NX / no guard pages**, so every out-of-bounds write is direct
corruption and even OOB reads can bus-error the console. But the reachability
picture is the opposite of pc64: a cartridge/console has **essentially no
untrusted external input**. There is no network stack, no filesystem parser fed
by remote data, no JS engine. The only externally-influenced bytes are the
controller pad, PS/2 keyboard/mouse (real-hardware-only), battery SRAM save data
(a crafted `.sav` on a flashcart), tape/WAV audio, and foreign Sega-CD BRAM
files. Every one of those paths was checked and is **bounds-clamped** (see §2),
so the Security section is short and correctly N-A for memory-corruption: it
lists only latent/defensive items. The bugs that matter here are performance
and correctness (§1, §3).

Three areas, most-actionable first. Line numbers are exact against the current tree.

---

## 1. Performance — the window drag rebuilds the whole desktop every step

> **✅ P1 FIXED (`facd5f8`)** — `handle_drag` now clips `repaint_all` to `union(old,new)` window rects via a clip window on `fill_cells`/`draw_str`/`draw_icon_cell`; render-verified byte-identical (`build.sh drag`, retro-shot genesis_plus_gx).

### The root cause
Dragging a window is **cell-snapped** (the window commits to a new 8-px cell as
the cursor crosses cell boundaries — `handle_drag`, `kernel.asm:1083`). The header
comment on `handle_drag` says *"cell-snapped live drag (tile repaints are cheap)"*
(`kernel.asm:1081`), but the repaint it triggers is not cheap:

- Each time the target cell changes, `handle_drag` writes the new `WX/WY` and calls
  **`repaint_all`** (`kernel.asm:1120-1122`).
- `repaint_all` (`kernel.asm:1544-1560`) does an **unconditional full-scene
  rebuild**: `clear_screen` (every one of the 40×28 = 1120 plane-A cells,
  `kernel.asm:1546`), then `draw_desktop` (menu bar, two footers, and all 11 icons),
  then **every window bottom-to-top at full content** (`kernel.asm:1549-1555`), then
  the soft-keyboard overlay.
- `draw_window` (`kernel.asm:1577`) ends by calling `app_draw_content`
  (`kernel.asm:1648`), so **every open window repaints its entire body**, not just
  its chrome — including any full-screen game or Paint window that did not move.

So each drag frame that crosses a cell re-executes the most expensive paint in the
system. With a Pac-Man window open that is the full 28×25 = 700-cell maze
(`pacman_draw` `.trow`/`.tcol`, `pacman.i:800-810`) re-issued to the VDP; with an
OutLast/Dostris/Tracker/Paint window it is that app's whole content pass — every
drag step, for a window that isn't even the one being dragged.

| Cost per drag cell-step | Where |
|---|---|
| `clear_screen` — 1120 single-word VDP writes | `kernel.asm:1546`, `1824-1834` |
| `draw_desktop` — menu + footers + **11 icons** (`divu` in `icon_pos`, 4 tiles + a `draw_str` label each) | `kernel.asm:1199-1205`, `draw_icon_cell 1224` |
| **Every** window's full body via `app_draw_content` (e.g. 700 maze cells) | `kernel.asm:1553`, `1646-1648` |
| `fill_cells` re-points the VDP (`cell_addr`, a `move.l` to `VDP_CTRL`) once per row | `kernel.asm:1764-1781` |

The same full-scene `repaint_all` is also on `raise_window` (`kernel.asm:1474`),
`close_window` (`kernel.asm:1503`) and `softkbd_hide` (`softkbd.i:30`) — one-shot
there, so less visible, but the same cost.

> Note: the per-app *content* refreshes are already optimized to the topmost window
> (`redraw_topmost`, `kernel.asm:1562`; `music_tick`/`gm_tick`/`tracker_tick`/
> `dostris_tick`/`outlast_tick`/`pacman_tick` all early-out unless they own the top
> window). The drag path is the one place that still goes full-scene.

### Fixes, highest impact first

**P1 — Damage-rect the drag instead of `repaint_all` (direct fix for the symptom).**
While `v_drag_active`, the only thing that changed is `union(old_rect, new_rect)`
of the moved window. Replace the `repaint_all` at `kernel.asm:1122` with: repaint
the desktop cells the window *vacated* (the old-rect minus new-rect band) and
`draw_window` the moved window at its new spot; only re-`draw_window` other windows
whose rectangles intersect that union. Even the coarse version — "repaint the band
of cell rows between the old and new `WY`, plus the moved window" — removes the
per-step full-screen churn and the redraw of unrelated game/Paint bodies.

**P2 — Don't repaint every window's *content* on a chrome-only rebuild.**

> **✅ FIXED (`f845166`)** — `draw_window` early-outs when the window rect doesn't intersect the `v_clip` window, so during a drag the unmoved windows' full chrome+content passes (already clipped to zero VDP writes by P1) are skipped entirely. Byte-identical on `build.sh drag`/`test` (retro-shot). Note: the live-game dostris autotest is cycle-timing-sensitive (state at frame N depends on per-frame render cost), so any `draw_window` edit — even NOPs — shifts it; the shipped P1 `facd5f8` does the same. Verify draw-path changes on the settled drag/test scenes, not dostris.

`repaint_all`/`draw_window` unconditionally call `app_draw_content`
(`kernel.asm:1648`). For a full-screen game or Paint window this is the dominant
cost and is pure waste when the window merely got re-blitted at the same size for a
neighbour's move. Split `draw_window` into `draw_window_chrome` + `draw_content`,
and in the bottom-up loop only redraw content for windows whose body cells were
actually clobbered (with P1's damage set, usually just the top/moved one).

**P3 — Skip the redundant `clear_screen` pass.**
`repaint_all` clears all 1120 cells to tile 0 (`kernel.asm:1546`) and then
`draw_desktop` + the windows immediately overwrite most of them. Only the desktop
*background* cells not covered by the menu/footers/icons/windows need clearing.
With damage tracking (P1) the clear collapses to "the vacated band," and the
full-screen clear disappears from the interactive path.

**P4 — Stop re-drawing the 11 static icons on every rebuild.**
`draw_desktop` redraws all icons (`kernel.asm:1199-1205`), each doing a `divu`
(`icon_pos`, `kernel.asm:1209-1221`), four tile writes and a `draw_str` label
(`draw_icon_cell`, `kernel.asm:1224`). Icons only change on selection
(`select_icon` already does the minimal 2-icon repaint). During a drag they never
change; damage-rect (P1) makes `draw_desktop` unnecessary except for the vacated
band.

**P5 — Use a VDP DMA fill for `clear_screen`.**
`clear_screen`/`fill_cells` write the whole plane one 16-bit word at a time through
`VDP_DATA` (`kernel.asm:1776`). Plane A is contiguous in VRAM, so a VDP **DMA fill**
of `$0000` clears it far faster than 1120 CPU writes and frees the 68000 during
vblank. (Rectangular window fills can't use DMA fill — the plane is 64 cells wide so
a window's rows aren't contiguous — but the full-screen clear can.)

Expected result: P1+P2 take a drag from "rebuild the entire desktop + every app
body each cell-step" to "repaint a few rows plus the moving window," which is the
difference between visibly chunky and smooth, exactly as on the other ports.

---

## 2. Security — reachable memory corruption

**There is no untrusted-input attack surface that reaches a memory-corruption
primitive.** This is a cartridge with no network, no remote parser, and no
scripting. The externally-influenced input paths were each traced to a bounds
clamp:

- **Tape / WAV decode** (`tape.i`, the closest thing to attacker-controlled data —
  a user plays an arbitrary WAV into the comparator). The block length is read from
  the stream but clamped against `v_tp_max` before any payload is accepted
  (`tape_rx_byte` `.lenlo`, `tape.i:392-396`), and the data write index is
  `bstate-18 ∈ [0,len)` with `len ≤ max` (`.data`, `tape.i:397-398`). Destinations
  are sized to the cap in every caller (Notepad `NBUF-1` into `NBUF`;
  `tape.i:178`; Tracker `TK_PATLEN`, `tracker.i:446`; Paint `PT_W*PT_H` into the
  exact-size canvas, `paint.i:917`). The name write is `bstate 4..15 → offset 0..11`
  into a ≥12-byte buffer (`.name`, `tape.i:384-386`). Bounded.
- **PS/2 keyboard** — the scancode→ASCII lookup is guarded unsigned before indexing:
  `cmp.b #132,d0 / bhs .out` (`ps2.i:408-409`), and `ps2map`/`ps2map_sh` are exactly
  132 bytes (`gen_data.i:463-482`). Frame shifters and prefixes are bounded. **PS/2
  mouse** packets assemble into the 4-byte `v_ps2m_pkt` at index `pktn<3`
  (`ps2.i:250-253`). Bounded.
- **Battery SRAM mini-FS** (`sram.i`) — a crafted `.sav` can supply arbitrary
  count/size/offset fields, but `sram_read` clamps the read to the caller's `max`
  (`sram.i:166-168`) and callers pass `NBUF-1`, so the RAM destination is safe; a
  bogus source offset only reads elsewhere inside the mapped SRAM window (no write).
  See S1 for the DoS-shaped residue.
- **Sega-CD BRAM foreign files** (`bram.i`) — `bram_read` clamps delivered bytes to
  the caller's `max` (`.clamp`, `bram.i:477-480`) and validates the UnoDOS header
  length against `blocks*64-14` before trusting it (`bram.i:460-465`). Bounded.
- **Controller pad** — fixed bit tests, no indexing. Trusted.

So the list below is **latent / defensive only**; none is a currently-reachable
corruption bug.

### S1 — LOW — corrupt SRAM directory count drives unbounded (but read-only) scans
`sram_find` loops `cmp d4,d3` where `d4` is the file count read straight from SRAM
(`sram.i:191-193`), and `files_draw` draws `store_count` rows (`sram.i:406`). A
crafted `.sav` with `count = $FFFF` makes these iterate 65535 times over
out-of-directory SRAM (read-only, and `files_draw` would issue off-screen VDP
writes with no `cell_addr` guard — cosmetic, cells wrap within the 64-wide plane).
No RAM is corrupted; worst case is a slow frame / garbled Files list.
**Fix:** clamp the SRAM count to `SRD_MAX` (8) on read, the way `bram_refresh`
already clamps to `BR_MAXDIR` (`bram.i:356-358`).

### S2 — LOW — `cell_addr` has no coordinate bound except under `PROBE_GUARD`
`cell_addr` (`kernel.asm:1745-1762`) range-checks `cx/cy` only when assembled with
`-DPROBE_GUARD` (`kernel.asm:1746-1751`); the shipping build computes the VRAM
address with no guard. Every drawing coordinate today is derived from clamped
window positions (`handle_drag` clamps to `[0,SCRW_C-WW]`/`[MENUBAR_C,SCRH_C-1]`,
`kernel.asm:1099-1114`) or fixed layout tables, so this is not reachable — but it is
the one spot where a future off-by-one in a caller becomes a wild VDP write instead
of a contained glitch. **Fix (defensive):** keep the `PROBE_GUARD` clamp compiled in
(even as a silent clamp-to-range) for release builds, or bound-check the app-supplied
draw origins.

### S3 — LOW — `sram_name` writes 13 bytes into the 12-byte `v_np_name`
Also tracked as **B2** below (it is a robustness boundary write, harmless today).

*Checked and clean:* the tape decoder length/name clamps (`tape.i:384-398`), the
PS/2 keyboard map bound (`ps2.i:408`) and mouse packet assembler (`ps2.i:241-255`),
`sram_read`/`bram_read` size clamps (`sram.i:166-168`, `bram.i:477-480`), the BRAM
UnoDOS-header sanity check (`bram.i:460-465`), the fake BRAM store's heap/dir caps
(`brfk_used`/`BRFK_BLKS` and `BR_MAXDIR` guards, `bram.i:641-648`), Paint's
flood-fill stack (guarded by `PT_STKN` on every push, `paint.i:462`, `476`) and
clipped `pt_px`/`pt_get` (`paint.i:61-116`), the Kosinski decompressor writing only
into cleared PRG-RAM (`bram.i:189-257`, CD-only path), Notepad insert/backspace
(guarded by `NBUF-1`, `apps.i:522-544`), Dostris/Pac-Man board indexing (masked /
`DT_COLS`/`DT_ROWS`/`PM_COLS`/`PM_ROWS`-bounded), the window table (`MAXWIN`=6 with
`v_wintab` = 6×16 and `v_zlist` = 6), and the scheduler task stacks
(`$FF4000..$FF7800`, non-overlapping with the Paint canvas `$FF0000..$FF3C00` and
`VARS` at `$FF8000`).

---

## 3. Correctness / robustness bugs

### B1 — LOW — `divu` overflow garbles Clock / uptime after ~18 h of runtime
`clock_draw` (`apps.i:112-119`) and `sysinfo_draw` (`apps.i:79-80`) convert the
32-bit vblank tick counter with `divu #TICKS_SEC` / `divu #60` on the full `long`.
`divu` on the 68000 requires the quotient to fit 16 bits; once `v_ticks/60`
exceeds 65535 — i.e. after `60*65536` ticks ≈ **18.2 hours** of uptime — the divide
overflows (V set, destination left unchanged), and the subsequent `swap`/mask
produce garbage, so the Clock window and the SysInfo uptime display corrupt.
No memory effect (values stay in registers and are drawn as digits). **Fix:**
reduce the tick count before the word divide (e.g. keep a separate seconds counter
incremented once per 60 ticks, or pre-divide the high word), so the operand handed
to `divu` is always < 2²⁰.

### B2 — LOW — `sram_name` over-writes `v_np_name` by one byte (into a reserved pad)
`sram_name` copies 12 name bytes and then `clr.b (a0)` — 13 bytes total — per its
"13-byte dest" contract (`sram.i:130-139`). But `files_key` `.open` calls it with
the **12-byte** `v_np_name` (`sram.i:527-529`). The 13th byte (the NUL) lands in the
1-byte pad declared right after `v_np_name` (`kernel.asm:270-271`), which is zeroed
at boot and otherwise unused, so it is correct today — but it is a genuine
one-past-the-field write kept safe only by that adjacent pad. (The BRAM path is fine:
`bram_name` writes 11 + NUL = 12 bytes, `bram.i:386-399`.) **Fix:** give `v_np_name`
13 bytes (drop the separate pad), or have `sram_name` accept and respect a length so
the terminator isn't forced.

*Checked and clean:* Notepad edit-buffer insert/backspace (both guarded by `NBUF-1`
and shifting within `v_npbuf`, `apps.i:497-546`); `fmt_dec`/`put2dig` (16-bit-masked,
≤6 bytes into ≥12-byte buffers, `apps.i:11-46`); the OutLast/Dostris/Pac-Man HUD
strings built into `v_npstat` (48 B, worst case ~31 chars); Dostris shape/rotation
indexing (`dt_shapes` = 224 B, `lsl` indices bounded, `games.i:1315-1322`) and
`dt_interval`'s `mulu/divu` remainder correctly discarded by `ext.l`
(`games.i:207-215`, `games.i:702-704`); Pac-Man mode-duration index clamped to 7
before ×2 into the 8-entry `pm_modedur` (`pacman.i:357-363`), ghost table sized
exactly `3*GSIZE` (`pacman.i:107-133`), maze template 700 B into `v_pm_maze` (700 B,
`pacman.i:87-104`); the Tracker demo song 256 B into `v_tkpat` (256 B) and cell
formatting bounds (`tracker.i:167-198`, `481-489`); soft-keyboard key-table indexing
(`KB_NKEYS`=55, 10-byte entries, `softkbd.i`) and hit-test row/col bounds
(`softkbd.i:112-141`); the event-queue ring (power-of-two `EVQ_SIZE`=32 masking,
`kernel.asm:2261-2303`); z-order raise/close shifts (byte-indexed within `v_zlist`,
`kernel.asm:1457-1512`); and the tape block-length signedness — a `len` in the
negative-word range is accepted by the `ble` clamp but then writes nothing because
`d3 < len` is immediately false (`tape.i:359-398`), so it is incidentally safe.

---

## Suggested order of work
1. **P1 + P2** — damage-rect the drag and stop repainting every window's content on
   a chrome rebuild. This is the reported "drag feels heavy" symptom and the single
   biggest win.
2. **P3 + P4 + P5** — collapse the redundant full-screen clear and static-icon
   redraw into the damage set, and DMA-fill the remaining full clears.
3. **B1** — fix the ~18 h `divu` overflow in Clock / uptime (cheap, one counter).
4. **B2 / S3** — size `v_np_name` to 13 bytes (closes the one-byte over-write).
5. **S1, S2** — clamp the SRAM directory count, and keep the `cell_addr` range guard
   compiled into release builds (defensive hardening for the flashcart-`.sav` and
   future-caller cases).

Building/testing note: the target assembles with `vasmm68k_mot` (`build.sh`, same
binary as the Amiga port) and runs under BlastEm; that toolchain isn't invoked here,
so all of the above is proposed, not applied. The audit did not modify any source.
