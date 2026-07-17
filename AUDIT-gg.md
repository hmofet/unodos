# UnoDOS `gg` code audit — performance, bugs, security

Scope: the **gg** port (Sega Game Gear, Z80 + VDP 315-5124, 8 KB work RAM), the
`build.sh` default target (`build/unodos.gg`, 32 KB cartridge). Files:
`kernel.asm`, `apps.inc`, `dostris.inc`, `gen_data.inc` (generated). The GG is SMS
silicon but its LCD shows only the centre 20×18 cells, so this port wears the
**minimal** profile: a vertical mini-icon list launcher, one full-screen app at a
time, directional nav — architecturally a clone of the NES port, not the windowed
SMS one. Everything draws at a `(6,3)` cell offset (`xy_to_vram`, `kernel.asm:290`).
Threat model for the security section: bare-metal cartridge, flat 8 KB RAM, **no
memory protection** — any OOB store corrupts directly. The **only external input
is the control pad** (`PORT_DC`); no network/filesystem/untrusted parser, so the
security surface is essentially nil and that section is short by design.

Line numbers are exact against the current tree (asm: `label` + line). This port
is near-identical to `sms` in hardware but shares the `nes` software model;
findings mirror the NES audit, not the SMS one.

---

## 1. Performance

The main loop (`kernel.asm:179`) is lean like the NES: per-frame it flushes
**small** partial tile writes (`render_partials`, `kernel.asm:618`) and runs logic
that touches no VDP; the only full repaints are transitions, and `full_redraw`
**correctly blanks the display** first (`$A0`→`$E0`, `kernel.asm:689,700`). Little
per-frame waste — findings are minor. (Note: unlike the sibling SMS port, GG does
*not* do a full-scene repaint on every drag/tick, because there is no window
manager or drag — so the SMS P1/P2/P3 lag findings do **not** apply here.)

### P1 — LOW — every Dostris piece-lock does a display-off full-screen redraw
`dostris.inc:218-224`. On a lock the game sets `v_dirty` (`dostris.inc:221`),
driving `full_redraw` (`kernel.asm:684`), which blanks the display, runs
`clear_names` (`kernel.asm:258`, the full 32×28 map) and repaints the title bar,
walls, floor, all board cells and the score — when only the ~120 board cells (in
practice the landed piece + any collapsed line) changed. A brief black settle per
lock. **Fix:** stamp the locked piece with a partial (like `draw_piece_partial`,
`dostris.inc:598`) and only full-redraw when a line actually collapsed.

### P2 — micro — clock string rebuilt every wall-second regardless of app
`clock_advance` (`apps.inc:86`) runs each frame and, on every second rollover,
calls `clock_format` (`apps.inc:122`) and sets `pf_clk` even when the Clock app is
not open (the write is only consumed when `v_app==1`, `kernel.asm:648`). Cost is
trivial (once per second); guard with `v_inapp`/`v_app==1` only if you want a
truly idle launcher loop.

*Checked and clean:* the frame ISR (`kernel.asm:123`) does a tick + ack and never
touches the VDP; the per-frame partials (`draw_highlight`, `draw_clock_time`,
`draw_music_status`, `draw_piece_partial`) touch only a few cells; the music
progress bar caps at 16+1 cells with a min of 1 (`apps.inc:308-313`).

---

## 2. Security — memory corruption / reachability

**No untrusted-input surface.** External input is the control pad only
(`read_pad`, `kernel.asm:499`): 6 bits reduced to edges. Strings, piece masks,
palettes, the tune and app tables are all ROM constants (`apps.inc`,
`gen_data.inc`). Nothing is parsed from an attacker source — no reachable
remote/device corruption primitive. The review confirms the internal index math
stays in bounds.

### Board write path — checked safe
`dostris_lock` (`dostris.inc:248`) stores the piece tile into `g_board`
(`$C044`, `BW*BH = 120` bytes) at `board_off` = `by*10 + bx` (`dostris.inc:41`)
with **no inline bound**, but each lock is preceded by `piece_collide`
(`dostris.inc:66`) at the same origin, which rejects any cell with `bx >= BW` or
`by >= BH` (unsigned `cp BW`/`cp BH`, `dostris.inc:78,88`) and catches
wrapped-negative columns with the same test — applied identically in collide,
lock and render (`draw_piece_cells`, `dostris.inc:560`, adds `BORG_COL` on top of
the same `bx`). Net index always `< 120`. No reachable OOB store — the same
invariant as the NES/SMS ports.

*No HIGH/MEDIUM/LOW memory-safety findings.* String scratch is sized and placed
correctly (`numstr` 4 B at `$C034`, `clk_str` 9 B at `$C038` ending `$C040`,
`g_board` at `$C044` — non-overlapping), and all of work RAM is zeroed at boot
(`kernel.asm:153-157`) so nothing is read uninitialized.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `two_digits` only valid for 0..99; Dostris line count is unbounded
`two_digits` (`kernel.asm:385`) converts `A` to two decimal digits via repeated
`sub 10`; for `A >= 100` the tens loop runs past `'0'` and emits a garbage glyph.
Bounded callers (clock `HH<24`/`MM,SS<60`, music note `<=30`, single-digit theme)
are fine, but `g_lines` (`dostris.inc:352` `inc (hl)`) is an unbounded byte
rendered through `two_digits` in `dostris_draw_score` (`dostris.inc:546`). >99
cleared lines → corrupt tens digit. Cosmetic only. **Fix:** cap at 99 before
display.

*(Note: unlike the NES port, GG's launcher is a 1-D Up/Down list, so there is
**no** analog of the NES diagonal double-highlight bug — Up and Down are mutually
exclusive on the d-pad, and `nav_input` (`kernel.asm:546`) handles at most one of
them per frame.)*

*Checked and clean:* `sel_up`/`sel_down` wrap/clamp within `[0,NICONS)`
(`kernel.asm:568`); `dostris_init` clears exactly `BW*BH` board bytes
(`dostris.inc:322`); `collapse_row` shifts rows and zeroes row 0 correctly
(`dostris.inc:383`); RNG/`mod7` reduce to `[0,6]`; the music note index wraps at
`MUSIC_COUNT` (`apps.inc:276`); `full_redraw` blanks then restores the display,
and RAM is fully cleared at boot.

---

## Suggested order of work
1. **B1** — clamp the Dostris line count before display (shared with nes/sms).
2. **P1** — partial board redraw on lock instead of a display-off full settle.
3. **P2** — optional idle-loop micro-optimisation.

Build/test note: assembles with `sjasmplus` (`build.sh`) to a raw 32 KB `.gg`;
`AUTOTEST` variants drive scripted pads (`apps.inc:327`). No toolchain was run
here — findings are from source review only.
