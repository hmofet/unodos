# UnoDOS `nes` code audit — performance, bugs, security

Scope: the **nes** port (6502/2A03 @ 1.79 MHz, PPU 2C02, 2 KB work RAM), the
`build.sh` default target (`build/unodos.nes`, NROM-256). Files: `kernel.s`,
`apps.inc`, `dostris.inc`, `nes_data.inc` (generated). Threat model for the
security section: bare-metal cartridge, flat 2 KB RAM, **no memory protection /
no MMU** — any out-of-bounds store is a direct corruption primitive. But the
**only external input is the standard controller** ($4016): 8 bits of button
state per frame, folded to edges. There is no network, no filesystem, no
untrusted parser — so the security surface is essentially nil and the section
below is short by design, focused on latent index/pointer footguns.

Line numbers are exact against the current tree (asm: `label` + line).

---

## 1. Performance

The main loop (`kernel.s:188` `main`) is genuinely lean: it syncs to vblank,
flushes **small** partial nametable writes (`render_partials`, `kernel.s:375`),
then runs logic that touches no PPU. Big repaints happen only on transitions and
run with rendering OFF. There is little per-frame waste. The findings are minor.

### P1 — LOW — every Dostris piece-lock blanks and rewrites the whole nametable
`dostris.inc:214-222`. On a lock the game sets `v_dirty` (`dostris.inc:219`),
which drives `full_redraw` (`kernel.s:450`). `full_redraw` calls
`clear_nametable` (`kernel.s:489`, **1024 `PPUDATA` writes** across
`$2000-$23FF`) and then repaints the title bar, the entire board, walls, floor
and score — when only the ~120 board cells (really: the few cells the landed
piece and any cleared line touched) actually changed. It is inside the documented
"brief settle" model (rendering is off, so it is flicker-free), but it is the
single largest avoidable per-event cost in the port.
**Fix:** stamp the locked piece into the board with a small partial (like
`draw_piece_partial`, `dostris.inc:576`) and only fall back to a full redraw when
a line actually collapsed.

### P2 — micro — clock string rebuilt every wall-second regardless of app
`apps.inc:111` `clock_advance` runs each frame and, on every second rollover,
calls `clock_format` (`apps.inc:143`, three `two_digits` conversions + 9 stores)
and sets `pf_clk` — even when the Clock app is not open (the write is only
consumed when `v_app==1`, `kernel.s:401`). Cost is trivial (once per second), so
this is a note, not a fix-now: guard the format with `v_inapp`/`v_app==1` if you
ever want the launcher idle-loop to do literally nothing.

*Checked and clean:* the per-frame partials (`draw_highlight` `kernel.s:435`,
`draw_clock_time`, `draw_piece_partial`) each touch only a handful of cells and
fit in vblank; the NMI (`kernel.s:779`) does a tick + flag and **never** touches
the PPU, so there is no ISR/main-loop VRAM contention; glyphs are drawn straight
from CHR ROM (no per-frame rasterization).

---

## 2. Security — memory corruption / reachability

**No untrusted-input surface.** The only external input is the controller, read
in `read_pad` (`kernel.s:219`) as 8 button bits and reduced to pressed edges. All
strings, piece masks, palettes, the music tune and the app tables live in ROM
(`apps.inc`, `nes_data.inc`). Nothing is parsed from an attacker-controlled
source, so there is no reachable remote or device corruption primitive. The
review reduces to confirming the latent index/pointer math cannot be pushed
out of bounds by button input.

### Board write path — checked safe
`dostris_lock` (`dostris.inc:243`) stores the piece tile into `g_board`
(`$0400`, `BW*BH = 120` bytes) at `board_off` = `by*10 + bx` (`dostris.inc:91`)
**without an inline bounds check** — but every lock is preceded by a
`piece_collide` (`dostris.inc:53`) at the same origin that rejects any cell whose
`bx >= BW` or `by >= BH` via unsigned `cmp #BW`/`cmp #BH` (`dostris.inc:62,72`).
The negative-column case wraps mod 256 and is caught by the same unsigned
upper-bound test, and the wrap is applied **identically** in collide, lock and
render, so the effective column stays in `[0,BW)`. Net index is therefore always
`< 120`. No reachable OOB store. (This is the deliberate invariant noted in the
`dostris.inc:50-52` comment; it holds.)

*No HIGH/MEDIUM/LOW memory-safety findings.* Zero-page string scratch is sized
correctly (`numstr` 4 B at `$50`, `clk_str` 9 B at `$54`, non-overlapping), and
all of work RAM is zeroed at boot (`kernel.s:155-164`) so nothing is read
uninitialized.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `two_digits` only valid for 0..99; Dostris line count is unbounded
`kernel.s:760` `two_digits` converts `A` to two decimal digits by repeated
`sbc #10`; for `A >= 100` the tens loop runs past `'9'` and emits a garbage
glyph. Callers with bounded inputs (clock `HH<24`/`MM,SS<60`, music note `<=30`)
are fine, but `g_lines` (`dostris.inc:338` `inc g_lines`) is an unbounded byte
and is rendered through `two_digits` in `dostris_draw_score` (`dostris.inc:525`).
A game that clears >99 lines shows a corrupt tens digit. Cosmetic only (no memory
effect). **Fix:** cap the displayed value at 99, or render three digits.

### B2 — LOW — launcher diagonal press leaves two labels highlighted
`nav_dir` (`kernel.s:280`) tests Right, Left, Down, Up **sequentially**, each
calling a `sel_*` routine that sets `v_selp = current v_sel` before moving. On a
diagonal edge (e.g. Right+Down in the same frame) `sel_right` (`kernel.s:300`)
sets `v_selp = old`, then `sel_down` (`kernel.s:319`) overwrites `v_selp` with the
**intermediate** selection. `draw_highlight` (`kernel.s:435`) then un-inverts the
intermediate label instead of the original, so the original label stays inverted
— two icons look selected until the next single-direction move. Reachable with a
diagonal on the d-pad. **Fix:** accept only one direction per frame in `nav_dir`
(e.g. `beq` chain that `rts` after the first handled edge), or recompute the
highlight from one `(old,new)` pair captured before any move.

*Checked and clean:* the line-clear collapse (`collapse_row`, `dostris.inc:368`)
shifts rows and zeroes row 0 correctly; `sel_left`/`sel_right`/`sel_up`/`sel_down`
wrap/clamp within `[0,NICONS)`; `dostris_init` clears exactly `BW*BH` board bytes
(`dostris.inc:314`); the RNG/`mod7` reduce to `[0,6]`; RAM is fully cleared at
boot; the music note index wraps at `MUSIC_COUNT` (`apps.inc:314`).

---

## Suggested order of work
1. **B2** — the one player-visible defect (stuck highlight on diagonals).
2. **B1** — clamp the Dostris line count before display.
3. **P1** — partial board redraw on lock instead of a full-nametable settle.
4. **P2** — optional idle-loop micro-optimisation.

Build/test note: the port assembles with `dasm` (`build.sh`) into an iNES ROM
and is driven by the `AUTOTEST` scripts (`apps.inc:368`). No toolchain was run
here — findings are from source review only.
