# UnoDOS `ppcmac` code audit — performance, bugs, security

Scope: the **ppcmac** world (PowerPC Macintosh, G3/G4-class 32-bit big-endian PowerPC,
booted as an Open Firmware / IEEE-1275 client program). Sources: `ppcmac/kernel.s`,
`ppcmac/apps.inc.s`, `ppcmac/dostris.inc.s`, plus `build.sh`. A from-scratch port on a
new ISA (GAS-PPC dialect), but the Dostris/app logic mirrors the AArch64 rpi core.

Threat model for the security section: freestanding 32-bit PowerPC, OF has RAM mapped,
**no NX, no guard pages, big-endian** — every out-of-bounds write is a direct corruption
primitive and an OOB read can fault. Runtime **untrusted input** is the **Open Firmware
console `read()` byte stream** (`read_keys` → `of_read`): one byte per frame. There is
**no filesystem/config parsing** (the payload is loaded by OF before `_start`); the OF
`getprop` framebuffer geometry is a trusted secondary input. So the realistic attack
surface is the serial/console byte stream — and unlike the ARM ports, it reaches a
**real memory-corruption bug** here (S1).

Line numbers are exact against the current tree (asm: label + line).

---

## 1. Performance

### P1 — Full-screen clear + repaint on every dirty event (biggest cost)
`kernel.s:897-918` (`full_redraw`) → `draw_launcher`/`draw_app`; every app path routes
through `draw_chrome`→`clear_screen` (`kernel.s:540-554`), which fills the **entire
640×480 = 307,200-pixel** framebuffer one 32-bit word at a time via `frect`
(`kernel.s:455-479`, inner store `kernel.s:471`). It fires on app entry, theme change,
and **every Dostris piece-lock** (`dostris.inc.s:212` sets `v_dirty`). **Fix:** cache the
static background / damage-rect; for Dostris clear only the board rectangle. The
`render_partials` path (`kernel.s:845-895`) already does incremental redraws — extend
that to the lock case.

### P2 — `wait_vblank` is an *uncalibrated* fixed busy-spin
`kernel.s:294-300`. Unlike the ARM ports (which pace off a real timer), this spins
`FRAME_SPIN = 0x10000` (65,536) `bdnz` iterations per frame. On a 300-500 MHz G3/G4 that
is well under 1 ms, so the loop runs at hundreds-to-thousands of fps: the software clock
(1 tick/frame, `apps.inc.s:149`), music tempo, and Dostris gravity all run far too fast,
and the CPU is pinned at 100%. **Fix:** pace off the PowerPC Time Base (`mftb`/`mftbu`)
to a real ~16.67 ms per frame, the PPC analogue of the ARM `cntpct`/system-timer loop.
(Also filed as a correctness bug — B1.)

### P3 — `frect`/`clear_screen` store one word per pixel
`kernel.s:470-473` writes a single `stw r7,0(r11)` per pixel; the full-screen clear pays
this 307,200×. **Fix:** on PowerPC use `dcbz` (zeroes a 32-byte cache line in one
instruction) for the palette-0 clear path, and a small unrolled store loop for coloured
fills — roughly an 8× win on `clear_screen`.

### P4 — Dostris repaints static walls + floor every redraw
`dostris.inc.s:466-545` (`dostris_draw`) redraws walls (`dd_walls`) and floor
(`dd_floor`) on every full redraw though they never move. **Fix:** paint the frame once
on entry; repaint only board cells + the live piece thereafter (subsumed by P1).

---

## 2. Security

### S1 — HIGH — signed bounds compare lets a piece walk off the left edge → OOB read + OOB write from the serial console
`ppcmac/dostris.inc.s`. `piece_collide` bounds-checks each occupied mask cell with
**signed** compares:
```
    cmpwi r16, BW        # dostris.inc.s:36   bx = (mask col) + g_tx
    bge   pc_yes         # dostris.inc.s:37   SIGNED >= : negative bx is NOT caught
    ...
    cmpwi r17, BH        # dostris.inc.s:42
    bge   pc_yes         # dostris.inc.s:43
    mulli r0, r17, BW    # dostris.inc.s:44   idx = by*BW + bx
    add   r0, r0, r16    # dostris.inc.s:45
    LA    r3, g_board    # dostris.inc.s:46
    lbzx  r0, r3, r0     # dostris.inc.s:47   read g_board[idx]  <-- idx can be negative
```
Repeated **Left** input (`'a'`/`'A'` over the OF console, or `PAD_L`) drives `g_px`
negative one step per frame via `ds_left` (`dostris.inc.s:69-87`). Once `g_tx` reaches
`-1`, the leftmost occupied cell has `bx = -1`. Under the **signed** `cmpwi/bge` at
`:37`, `-1 < BW`, so the bound does **not** fire; execution falls through and
`lbzx r0, r3, r0` (`:47`) reads `g_board[-1]` — an **out-of-bounds read** one byte below
the board. Because a zero byte there reads as "free," `piece_collide` returns 0, `ds_left`
commits `g_px = -1`, and the piece leaves the playfield. Each further frame `bx`
decreases again, so the OOB read walks progressively further below `g_board`
(`VARS+0x260`) into `numstr`/`clk_str`/`palette` and below.

When such an off-board piece finally locks (gravity collision), `dostris_lock` writes each
occupied cell:
```
    stbx  r3, r4, r0     # dostris.inc.s:276   g_board[by*BW + bx] = tile, bx < 0
```
with the same negative `bx` and **no bounds check** — an **out-of-bounds write** into the
VARS block. This is a memory-corruption primitive reachable purely from untrusted console
input.

**The ARM ports are NOT affected:** rpi/pinephone use **unsigned** `b.hs` at the
equivalent check (`rpi/dostris.inc.s:38`, `pinephone/dostris.inc.s:37`), so a negative
`bx` wraps to a huge unsigned value and is correctly rejected as a collision.

**Fix:** use **unsigned** compares for both bounds — `cmplwi r16, BW` / `bge pc_yes`
(`:36-37`) and `cmplwi r17, BH` / `bge` (`:42-43`) — matching the ARM semantics; as
defence-in-depth also add an explicit `0 <= bx < BW && 0 <= by < BH` guard in
`dostris_lock` (`dostris.inc.s:262-281`). This is also a gameplay bug (piece clips
through the left wall); see B2.

### S2 — LOW — Open Firmware framebuffer geometry used unvalidated (trusted)
`fb_init` (`kernel.s:227-266`) stores the `address` (`kernel.s:242`) and `linebytes`
(`kernel.s:250`) returned by OF `getprop` straight into `fb_base`/`fb_pitch`, and every
primitive then computes `fb_base + y*pitch + x*4` with no sanity check. The `getprop`
return size in `r3` is discarded. A wrong/short property → all framebuffer writes land at
an arbitrary address. Open Firmware is trusted boot firmware, so LOW; a `getprop size==4`
check plus a `pitch >= SCRW*4` clamp would harden it.

### Console `read()` — no reachable corruption beyond S1 (justified)
`read_keys` (`kernel.s:305-378`) asks OF `read()` for **one** byte into `key_buf`
(`of_read`, `kernel.s:269-290`, `len=1`), masks to 8 bits, and maps it through a fixed
`cmpwi`/`beq` ladder to one `PAD_*` constant (or 0). No length/offset/index is derived
from the byte, and the receive buffer is exactly the 1 byte requested. The only influence
the stream has is *which pad bit* is set per frame — which is precisely what reaches S1.

---

## 3. Correctness / robustness bugs

### B1 — MEDIUM — uncalibrated frame timing
`kernel.s:294-300` (cross-ref P2). The fixed `FRAME_SPIN` busy-loop does not produce the
~60 Hz the comment claims on real hardware; the clock/music/gravity all run at whatever
rate the CPU happens to spin. **Fix:** pace off `mftb` against a Time Base frequency to a
real 16.67 ms.

### B2 — signed left-bound compare (gameplay + memory safety)
`dostris.inc.s:36-37` (cross-ref S1). Independent of the corruption angle, the signed
compare is a plain logic bug: the falling piece slides through the left wall instead of
being blocked. Fixed by the same `cmplwi` change.

### B3 — LOW — `two_digits` only handles 0..99; `g_lines` is unbounded
`kernel.s:557-567`. For a value ≥100 the tens character overshoots `'0'..'9'`; the Dostris
lines counter `g_lines` (`dostris.inc.s:381-383`) is uncapped. No OOB (byte stays in the
font's printable range for realistic counts) — cosmetic. **Fix:** clamp to 99 or render
three digits.

### B4 — LOW — Dostris uses a fixed seed even in the interactive build
`dostris.inc.s:358-360` seeds `g_seed` with the constant `0x00012345` in `dostris_init`,
so the piece sequence is identical on every boot. The ARM ports seed from a live timer
(`cntpct_el0` / `SYS_TIMER_CLO`). Harmless for the AUTOTEST runs (which script the moves)
but makes interactive play deterministic. **Fix:** seed from `mftb` like the ARM ports
seed from their timers.

### B5 — LOW (environment-dependent) — `read_keys` assumes OF `read()` is non-blocking
`kernel.s:314-319`. The main loop calls `of_read(len=1)` every frame and treats a return
of `!=1` as "no key" (`rk_none`). IEEE-1275 `read` is specified non-blocking (returns 0
when no data), so this is correct on a conformant OF; on an implementation that blocks,
the whole loop — clock, music, gravity — would stall until a key arrives. Flagged as a
documented dependency, not a confirmed defect.

*Checked and clean:* navigation index math (`sel_*` wrap/clamp against `NICONS`,
`kernel.s:762-805`); palette/theme load (`v_theme` bounded by `NTHEMES` in `theme_input`,
16-word `load_palette` `kernel.s:523-537`); `music_load` (`m_idx` bounded by
`MUSIC_COUNT`, 4-byte-stride table per `mkdata.py`; `music_gen` rest-guarded at
`apps.inc.s:329`); clock rollover (`clock_advance` `apps.inc.s:149-195`);
`collapse_row`/`row_full` indices (`g_row` ∈ `0..BH-1`); the boot VARS clear (256 words
= 1024 B ≥ the 748-byte used span up to `g_board`+`BW*BH`); OF client-interface argument
marshalling (`of_finddevice`/`of_getprop`/`of_read`, fixed `nargs`/`nrets`, `ci_buf`
16-byte aligned); the `by`/right-`bx` bounds (positive values compare correctly even
under the signed op — only the negative-`bx` left case in S1 is wrong).

---

## Suggested order of work
1. **S1 / B2** — change the two collide bounds to unsigned `cmplwi` (one-line-each fix
   that closes the reachable OOB read+write and the wall-clip bug); add the
   `dostris_lock` guard.
2. **P2 / B1** — real Time-Base frame pacing (fixes both the 100% spin and the wrong
   clock/music/gravity speed).
3. **P1 + P3** — region-clear the redraws and use `dcbz` for the clear.
4. **S2** — sanity-check the OF framebuffer geometry.
5. **P4, B3, B4, B5** — static-chrome caching, `two_digits` clamp, live RNG seed, and the
   `read()` non-blocking assumption.

Build/test note: assembles with `powerpc-linux-gnu-as/ld` (big-endian, `-mregnames`) via
WSL (`build.sh`, driven by `harness.py`). That toolchain isn't installed here, so none of
the above was compiled — the fixes are proposed, not applied.
