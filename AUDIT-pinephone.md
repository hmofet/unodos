# UnoDOS `pinephone` code audit — performance, bugs, security

Scope: the **pinephone** world (Allwinner A64, ARM Cortex-A53 / AArch64), the
`build.sh` default target (interactive M1 boot) and its AUTOTEST variants. Sources:
`pinephone/kernel.s`, `pinephone/apps.inc.s`, `pinephone/dostris.inc.s`, plus
`boot.cmd`/`build.sh`. The port reuses the rpi AArch64 core almost verbatim,
retargeted to the A64 DE2 mixer and a 480×640 portrait panel.

Threat model for the security section: freestanding AArch64, DRAM identity-mapped
by the SPL/U-Boot chain, caches disabled (`boot.cmd` `dcache off`), **MMU effectively
flat, no NX, no guard pages** — every out-of-bounds write is a direct corruption
primitive and an OOB read can fault the bus. The only runtime **untrusted input** is
the **A64 UART0 serial console** (`read_keys`): one received byte per frame. There is
**no filesystem/config parsing in our code** — `boot.cmd` `fatload` runs in U-Boot,
before us, and the payload is a flat image. So the realistic attack surface is the
serial byte stream, which (see §2) is handled safely.

Line numbers are exact against the current tree.

---

## 1. Performance

### P1 — Full-screen clear + repaint on every dirty event (biggest cost)
`kernel.s:780-797` (`full_redraw`) calls `draw_launcher`/`draw_app`, and every app
path routes through `draw_chrome`→`clear_screen` (`kernel.s:435-445`), which fills the
**entire 480×640 = 307,200-pixel** framebuffer one 32-bit word at a time via `frect`
(`kernel.s:356-379`, inner store `kernel.s:372`). This fires not just on app entry and
theme change but on **every Dostris piece-lock** (`dostris.inc.s:230` sets `v_dirty`),
i.e. roughly once per second of play, each time wiping and repainting the whole screen.
**Fix:** cache the static desktop/background and restore only the damaged region; for
Dostris, clear only the board rectangle (`BORG_X..BORG_X+BW*CELL`, `BORG_Y..`) instead
of the whole panel. The incremental `render_partials` path already does the right thing
for the clock/highlight/piece — extend that discipline to the lock redraw.

### P2 — `wait_vblank` busy-spins the core at 100%
`kernel.s:209-217` polls `cntpct_el0` in a tight loop for `FRAME_TICKS` (400000 @ 24 MHz
= 16.67 ms) every frame. It is time-accurate (good) but never sleeps, so core0 is pinned
at 100% even when idle. **Fix:** program a generic-timer compare (`cntp_cval_el0`) and
`WFE`/`WFI` until it fires; wake, then continue.

### P3 — `frect`/`clear_screen` store one word per pixel
`kernel.s:371-374` writes a single `str w4,[x6],#4` per pixel with no unrolling; the
full-screen clear pays this 307,200×. **Fix:** use `stp` (two words) or a NEON
`st1 {v0.4s}` inner loop for the aligned bulk of each row, byte/word tail for the
remainder — a 4-8× win on `clear_screen` and large `frect`s.

### P4 — Dostris repaints static walls + floor every redraw
`dostris.inc.s:489-573` (`dostris_draw`) redraws the left/right walls (`dd_walls`,
`BH` iterations × 2 `dcell`) and floor (`dd_floor`, `BW+2` cells) on every full redraw,
though they never move after board init. **Fix:** paint the frame/walls once on app
entry and only repaint board cells + the live piece thereafter (subsumed by P1's
damage-rect approach).

*(Already good: `render_partials` (`kernel.s:727-778`) does incremental redraws for the
highlight, clock tick, music status, and the Dostris piece — text/pieces are not
re-rasterized from a full-scene repaint each frame.)*

---

## 2. Security

### S1 — LOW (latent) — `dostris_lock` writes the board with no bounds check
`dostris.inc.s:269-318`. `dostris_lock` computes `idx = by*BW + bx` from the piece mask
and `g_tx/g_ty` and does `strb w2,[x1,w0,uxtw]` (`:309`) with **no range check on
`bx`/`by`**. It is safe *today* only because it is always preceded by a `piece_collide`
that validated the pose, and `piece_collide` uses **unsigned** compares
(`cmp w21,#BW; b.hs pc_yes` at `:37`, and `:44`), so a negative or overflowing `bx`
wraps to a huge unsigned value and is correctly rejected as a collision. The lock write
therefore always lands in `g_board[0..BW*BH)`. **Note the contrast with the ppcmac port,
where the same code uses *signed* compares and this becomes a reachable OOB write.**
**Fix (hardening):** add an explicit `bx<BW && by<BH` guard in `dostris_lock` so the
invariant is enforced locally, not just by the caller.

### Serial console — no reachable corruption (justified)
`read_keys` (`kernel.s:222-296`) reads **one** byte from `UART0_RBR`, masks it to 8 bits,
and maps it through a fixed `cmp`/`b.eq` ladder to exactly one `PAD_*` constant (or 0)
stored in `v_pad`. No length, no offset, and no array index is ever derived from the
input byte; there is no receive buffer to overflow. An attacker with full control of the
serial stream can only choose which of 8 d-pad/button bits is set per frame. That drives
game/UI state (including the ppcmac-style left-walk in §3/ppcmac) but on this port cannot
by itself corrupt memory. No other untrusted input exists (no FS, no network, no USB).

---

## 3. Correctness / robustness bugs

### B1 — LOW — `two_digits` only handles 0..99; `g_lines` is unbounded
`kernel.s:447-457` (`two_digits`) subtracts 10 until `<10`, so for a value ≥100 the
"tens" character overshoots `'9'` (e.g. 100→`':'`, 120→`'<'`). The Dostris lines-cleared
counter `g_lines` (`dostris.inc.s:411`) has no cap, so a long game prints a garbled tens
glyph. No OOB — the produced byte stays within the font's printable range for any
realistic count — purely cosmetic. **Fix:** clamp the display to 99, or render three
digits.

### B2 — LOW (latent) — unchecked `dostris_lock` board write
Same site as S1 (`dostris.inc.s:309`): correctness-wise the lock relies on an external
invariant. Guarded today by the unsigned collide check; listed here so the missing local
bound is tracked as a robustness gap, not only a security one.

*Checked and clean:* navigation index math (`sel_*` wrap/clamp against `NICONS`,
`kernel.s:642-691`); palette/theme load (`v_theme` bounded by `NTHEMES` in `theme_input`,
16-word copy in `load_palette` `kernel.s:420-433`); `music_load` (`m_idx` bounded by
`MUSIC_COUNT`, 4-byte-stride table matches `mkdata.py`); clock rollover
(`clock_advance` `apps.inc.s:143-185`, all fields wrapped); `collapse_row`/`row_full`
indices (`g_row` ∈ `0..BH-1`); the boot VARS clear (256 words = 1024 B ≥ the 748-byte
used span up to `g_board`+`BW*BH`); PCM square-wave synthesis (`music_gen`
`apps.inc.s:306-331`, fixed `AUD_PERF` sample count to a fixed FIFO port).

---

## Suggested order of work
1. **P1** — damage-rect / region-clear the Dostris lock redraw and app repaints (the
   one recurring, whole-screen cost).
2. **P2 + P3** — stop the 100% busy-spin and vectorize the clear.
3. **S1 / B2** — add the local bound in `dostris_lock` (cheap; also pre-empts the
   ppcmac-class bug if this core is retargeted again).
4. **P4, B1** — static-chrome caching and the `two_digits` clamp.

Build/test note: this port assembles with `aarch64-linux-gnu-as/ld` via WSL
(`build.sh`, driven by `harness.py` under an emulator). That toolchain isn't installed
here, so none of the above was compiled — the fixes are proposed, not applied.
