# UnoDOS `rpi` code audit — performance, bugs, security

Scope: the **rpi** world (Raspberry Pi 3/4 bare-metal, BCM283x, ARM Cortex-A /
AArch64), the `build.sh` default target (interactive M1 boot) and its AUTOTEST
variants. Sources: `rpi/kernel.s`, `rpi/apps.inc.s`, `rpi/dostris.inc.s`, plus
`build.sh`. This is the AArch64 core the pinephone port later reused.

Threat model for the security section: freestanding AArch64, firmware-mapped RAM,
**MMU flat, no NX, no guard pages** — every out-of-bounds write is a direct corruption
primitive, an OOB read can fault. The only runtime **untrusted input** is the **PL011
UART serial console** (`read_keys`): one received byte per frame. Note on the SD/FAT
surface: **our kernel does not parse FAT32 or `config.txt`.** The VideoCore GPU firmware
reads `config.txt` and loads `kernel8.img` from the card *before* `_start`; our code only
issues a mailbox property call for the framebuffer (`fb_init`) and reads back a base +
pitch from the firmware response. So the realistic attack surface is the serial byte
stream (handled safely, §2), with the firmware-supplied framebuffer geometry a trusted
secondary input.

Line numbers are exact against the current tree.

---

## 1. Performance

### P1 — Full-screen clear + repaint on every dirty event (biggest cost)
`kernel.s:834-851` (`full_redraw`) → `draw_launcher`/`draw_app`; every app path routes
through `draw_chrome`→`clear_screen` (`kernel.s:481-491`), which fills the **entire
640×480 = 307,200-pixel** framebuffer one 32-bit word at a time via `frect`
(`kernel.s:397-420`, inner store `kernel.s:413`). It fires on app entry, theme change,
and **every Dostris piece-lock** (`dostris.inc.s:230` sets `v_dirty`) — roughly once a
second of play — each time wiping and repainting the whole screen. **Fix:** cache the
static background and restore only the damaged region; for Dostris clear only the board
rectangle. The `render_partials` path already does this correctly for the clock,
highlight, and live piece — extend it to the lock redraw.

### P2 — `wait_vblank` busy-spins the core at 100%
`kernel.s:245-254` polls `SYS_TIMER_CLO` (1 MHz) in a tight loop for `FRAME_US` (16667
= 16.67 ms) every frame. Time-accurate but never sleeps, so the core is pinned at 100%
even when idle. **Fix:** use the BCM system-timer compare + IRQ (or the ARM generic
timer) and `WFI` between frames.

### P3 — `frect`/`clear_screen` store one word per pixel
`kernel.s:412-415` writes a single `str w4,[x6],#4` per pixel; the full-screen clear
pays this 307,200×. **Fix:** `stp`/NEON stores for the aligned bulk of each row — a
4-8× win on `clear_screen` and large fills.

### P4 — Dostris repaints static walls + floor every redraw
`dostris.inc.s:490-574` (`dostris_draw`) redraws walls (`dd_walls`) and floor
(`dd_floor`) on every full redraw though they never move. **Fix:** paint the frame once
on app entry; thereafter repaint only board cells + the live piece (subsumed by P1).

*(Already good: `render_partials` (`kernel.s:780-831`) is incremental for highlight,
clock tick, music status, and the Dostris piece.)*

---

## 2. Security

### S1 — LOW (latent) — `dostris_lock` writes the board with no bounds check
`dostris.inc.s:269-318`. `dostris_lock` writes `g_board[by*BW+bx]` via
`strb w2,[x1,w0,uxtw]` (`:309`) with **no range check on `bx`/`by`**. Safe today only
because it is always preceded by `piece_collide`, which uses **unsigned** compares
(`cmp w21,#BW; b.hs pc_yes` at `:38`, and `:45`): a negative/overflowing `bx` wraps to a
huge unsigned value and is rejected as a collision, so committed poses — and therefore
lock indices — are always in `[0,BW*BH)`. **The ppcmac port uses *signed* compares at the
same spot and this becomes a live OOB write.** **Fix (hardening):** add an explicit
`bx<BW && by<BH` guard inside `dostris_lock`.

### S2 — LOW — firmware framebuffer geometry used unvalidated (trusted)
`fb_init` (`kernel.s:162-242`) reads the allocated base (`kernel.s:234-237`, masked
`& 0x3FFFFFFF`) and pitch (`kernel.s:238-240`) from the mailbox response and stores them
into `fb_base`/`fb_pitch`, then every primitive computes addresses as
`fb_base + y*pitch + x*4` with no sanity check. A malformed firmware response (wrong
pitch/base, or a failed allocation returning garbage) would send all draws to an
arbitrary address. The VideoCore firmware is trusted boot code, so LOW; a cheap
`pitch >= SCRW*4 && base != 0` clamp would harden it.

### Serial console — no reachable corruption (justified)
`read_keys` (`kernel.s:260-334`) reads **one** byte from `UART0_DR`, masks to 8 bits, and
maps it through a fixed `cmp`/`b.eq` ladder to one `PAD_*` constant (or 0). No length,
offset, or array index is derived from the byte; there is no receive buffer. An attacker
controlling the serial stream can only choose which of 8 pad bits is set per frame —
which drives UI/game state but cannot by itself corrupt memory. No FS parsing exists in
our code (see threat model), and there is no network/USB input.

---

## 3. Correctness / robustness bugs

### B1 — LOW — `two_digits` only handles 0..99; `g_lines` is unbounded
`kernel.s:493-504`. For a value ≥100 the tens character overshoots `'9'`. The Dostris
lines counter `g_lines` (`dostris.inc.s:410-413`) is uncapped, so a long game prints a
garbled tens glyph. No OOB (byte stays in the font's printable range) — cosmetic.
**Fix:** clamp to 99 or render three digits.

### B2 — LOW (latent) — unchecked `dostris_lock` board write
Same site as S1 (`dostris.inc.s:309`) — the lock depends on an external invariant rather
than a local bound. Tracked here as a robustness gap.

### B3 — LOW — no fallback if the mailbox framebuffer request fails
`fb_init` (`kernel.s:219-240`) never inspects the response status word (drained and
discarded at `kernel.s:230-231`) or checks that `fb_base` is non-zero before drawing. On
a firmware/allocation failure the first `frect` writes to a bogus pointer. Trusted path,
so LOW. **Fix:** verify the `0x80000000` success bit / non-zero base before `draw_launcher`.

*Checked and clean:* navigation index math (`sel_*` wrap/clamp against `NICONS`,
`kernel.s:695-744`); palette/theme load (`v_theme` bounded by `NTHEMES`, 16-word
`load_palette` `kernel.s:465-478`); `music_load` (`m_idx` bounded by `MUSIC_COUNT`,
4-byte stride; PWM divisor `udiv` guarded against a rest by `cbz w2,ml_rest`
`kernel.s:309`); clock rollover (`clock_advance` `apps.inc.s:143-185`);
`collapse_row`/`row_full` indices (`g_row` ∈ `0..BH-1`); the boot VARS clear (256 words
= 1024 B ≥ the 748-byte used span); Dostris board render stays on-screen
(`BORG_X=224,BORG_Y=64`, max cell `x≈400`, `y≈304` within 640×480).

---

## Suggested order of work
1. **P1** — region-clear the Dostris lock redraw and app repaints.
2. **P2 + P3** — stop the 100% busy-spin; vectorize the clear.
3. **S1 / B2** — local bound in `dostris_lock`.
4. **S2 / B3** — sanity-check the firmware framebuffer response.
5. **P4, B1** — static-chrome caching and the `two_digits` clamp.

Build/test note: assembles with `aarch64-linux-gnu-as/ld` via WSL (`build.sh`,
`harness.py` under QEMU/an emulator). That toolchain isn't installed here, so none of the
above was compiled — the fixes are proposed, not applied.
