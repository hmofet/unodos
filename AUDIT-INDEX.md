# UnoDOS cross-port code audit — index

Per-port audits of every UnoDOS target, in the same three-section format as
[`AUDIT-pc64.md`](AUDIT-pc64.md): **1. Performance** (P#), **2. Security** (S#,
HIGH/MEDIUM/LOW), **3. Correctness/robustness** (B#), each finding with an exact
`file:line` and a concrete fix. One file per port: `AUDIT-<port>.md`.

Threat-model reality check: **most retro consoles have no untrusted-input surface
at all** — the only runtime inputs are the controller and project-authored ROM
data, so their Security sections are honestly short/latent. Reachable memory
corruption concentrates in two places: ports that **load from mutable media**
(disk / SRAM / memory-card) and ports whose **console/serial input reaches a
signed-index path**.

## Per-port summary

| Port | P / S / B | Highest-severity finding |
|------|-----------|--------------------------|
| [pc64](AUDIT-pc64.md) | 7 / 9 / 6 | **S1 HIGH** — `net.c` IP total-length trusted → remote info-leak/OOB (see file) |
| [amiga](AUDIT-amiga.md) | 5 / 3 / 3 | **S1 HIGH** — `loader.i:209` `.APP` read budget = untrusted dir size, no slot clamp → overflow slots + bitplanes |
| [apple2](AUDIT-apple2.md) | 6 / 3 / 4 | **S1 HIGH** — `fs.i:219`→`fs_read_sectors` catalog `size` drives unbounded sector write across the whole address space |
| [ppcmac](AUDIT-ppcmac.md) | 4 / 2 / 5 | **S1 HIGH** — `dostris.inc.s:36` **signed** `cmpwi/bge` bounds → held-Left over the OF console drives OOB read + `stbx` write |
| [ps2](AUDIT-ps2.md) | 4 / 3 / 4 | **S1 MED-reach / HIGH-impact** — `ee_modload.c:413` relocation `r_offset` unbounded → arbitrary 32-bit write from an `mc0:` module |
| [c64](AUDIT-c64.md) | 4 / 2 / 3 | **S1 MEDIUM** — `paint.s:374`/`fs.i:221` unclamped `fs_read` → Notepad-grown `PAINT.UNO` overflows canvas into running code |
| [snes](AUDIT-snes.md) | 4 / 2 / 3 | **S1 MEDIUM** — `sram.inc:108` unvalidated `sram_count` → tilemap index wrap → WRAM corruption from a crafted `.srm` |
| [iigs](AUDIT-iigs.md) | 6 / 2 / 3 | **B1 MEDIUM** — `kernel.s:2043` drag Y-clamp omits window height → OOB write past SHR fb into SCB/palette |
| [sms](AUDIT-sms.md) | 4 / 1 / 2 | **P1 HIGH-impact** — full repaint every drag frame (pc64 drag-lag shape) + mid-frame raster tearing |
| [mac](AUDIT-mac.md) | 6 / 3 / 6 | **P1** — Clock full-redraws every main-loop iteration → 100% CPU spin |
| [genesis](AUDIT-genesis.md) | 5 / 3 / 2 | **P1** — `repaint_all` full-scene rebuild on every drag cell-step |
| [rpi](AUDIT-rpi.md) | 4 / 2 / 3 | **S2 LOW** — firmware-mailbox framebuffer base/pitch used unvalidated |
| [pinephone](AUDIT-pinephone.md) | 4 / 1 / 2 | **S1 LOW/latent** — `dostris_lock` board write relies solely on `piece_collide` (unsigned → safe here) |
| [macplus](AUDIT-macplus.md) | 4 / 1 / 3 | **B1 LOW** — 32-bit tick `divu` overflows at ~18 h (Clock/uptime); no HIGH (trusted disk only) |
| [x86](AUDIT-x86.md) | 4 / 3 / 5 | all **LOW/latent** — more hardened than pc64 (damage-rect, aligned `rep stos`, XOR drag, FAT cache); P1 = unaligned CGA blit |
| [dreamcast](AUDIT-dreamcast.md) | 4 / 3 / 4 | **B1** functional — storage silently no-ops on real HW; no untrusted-media surface |
| [gb](AUDIT-gb.md) | 3 / 1 / 2 | **P1** — per-lock LCD-off `full_redraw` when a board-only path exists |
| [pce](AUDIT-pce.md) | 3 / 1 / 2 | **P1** — per-lock full redraw pushes all 1024 BAT entries through the single VDC port |
| [vic20](AUDIT-vic20.md) | 3 / 2 / 2 | **P1** — per-lock full clear+repaint tears on the live matrix (no display-off) |
| [ws](AUDIT-ws.md) | 3 / 1 / 2 | **P1** — per-lock `full_redraw`; otherwise the reference-clean retro port (gates clock, signed-safe compares) |
| [gba](AUDIT-gba.md) | 3 / 2 / 2 | **P1** — `frect` per-pixel: `clear_screen` = 38 400 `strh` every full redraw; S latent (cart ROM, no media) |
| [nes](AUDIT-nes.md) | 2 / 0 / 2 | **B2 LOW** — diagonal launcher press double-selects; no security surface |
| [gg](AUDIT-gg.md) | 2 / 0 / 1 | **P1 LOW** — display-off full redraw for a ~120-cell change |

*(pc64 counts summarize the existing `AUDIT-pc64.md`.)*

## Cross-cutting patterns — fix once in shared code, not per port

These recur across many ports because the ports share source (the `dostris`/app
sources, the 6502 `fs.i`, the 68k `kernel.asm` line). Fixing the shared origin
fixes every port at once.

1. **No damage-rect / no cached background → full-scene repaint on drag & redraw.**
   The pc64 P1/P2 lesson repeats on genesis, iigs, sms, mac, gb, pce, vic20, ws,
   gg, dreamcast. Ports that already do minimal-damage drawing (amiga XOR outline,
   macplus damage-min, **x86** full damage-rect) are the models to copy.

2. **Dostris `dostris_lock` / `draw_piece_cells` trust `piece_collide` with no
   independent index clamp.** Latent S1 across *every* retro port — and it became a
   reachable **HIGH on ppcmac** purely because that port's `piece_collide` used a
   **signed** compare (`cmpwi/bge`) where the ARM ports used unsigned (`b.hs`). Fix:
   add an independent board-index clamp in the shared lock/draw path, and make every
   port's collision bound compare unsigned.

3. **`two_digits`/`put2` assume ≤ 99 but `g_lines` is uncapped** — shared cosmetic
   **B1** on nes/sms/gg/gb/pce/vic20/ws/gba/snes. Cap the value or widen the
   formatter once.

4. **32-bit tick counter `divu` overflows at ~18 h** — macplus and genesis (shared
   68k `kernel.asm`). Use a 32/32 long divide or reduce before dividing.

5. **Unclamped `fs_read` + trusted "USV1" catalog fields** — apple2 (**HIGH**) and
   c64 (**MEDIUM**) share the 6502 `fs.i`; iigs has the FAT12 analogue (latent).
   Clamp the destination to the buffer size and validate catalog `size`/`count`/
   `start` on load. The disk/catalog parser is *the* corruption surface on every
   media-loading port.

6. **Module/app loaders don't bound the load against the destination slot** —
   ps2 ELF relocation (HIGH-impact), amiga `.APP` (HIGH), apple2/c64/iigs `.APP`
   slot. The x86 loader is the counter-example (validates size + bytes-read before
   executing) — copy its checks.

7. **`clock`/time formatting runs regardless of the visible app** — mac (P1),
   gb/pce/vic20 (P2). **ws already gates this correctly** — use it as the pattern.

8. **`fmt_u tmp[12]` (pc64 B1) does NOT reproduce** on ps2/dreamcast/mac — their
   ABIs make `long` 32-bit (≤ 10 digits). It's a pc64/host-64-bit-build issue only;
   don't "fix" it on the 32-bit-long targets.

## Notes
- Audits are static source review; nothing was assembled or booted (same caveat as
  the pc64 reference). Line numbers are exact against the current tree.
- No source was modified — every finding is a proposed fix, not an applied one.
- Shared subsystems compiled into pc64 (net/TLS/USB/HID/JS/unoui) were covered by
  `AUDIT-pc64.md`; this sweep covers the per-port code and the retro shared cores.
