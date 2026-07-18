# Fresh-port parity push (2026-06-17)

Goal: bring the **11 fresh contract-driven ports** up to the full app roster
(SysInfo, Clock, Notepad, Files, Theme, Music, Dostris **+ the 4 that were
missing: Tracker, Pac-Man, OutLast, Paint**) and persistent storage, *where the
hardware allows*; plus (separately) refactor the C-core ports to consume the
contract. Best-ROI tier order: framebuffer ports first, then tile ports.

The 4 missing apps + storage were the same gap on every fresh port. The
algorithms are settled (debugged once on rpi): Pac-Man maze + ghost AI (from
`genesis/pacman.i`), OutLast band model (from `c64/outlast.s`), the USV1 byte-heap
FS (from `c64/fs.i`), and the tracker pattern. New ports re-express these in the
native ISA — they are not redesigns.

## Status

### Tier A — framebuffer ports: COMPLETE (4 apps + storage each, all verified)
| Port | ISA | Notes | Verify |
|---|---|---|---|
| rpi | AArch64 | 640×480 mailbox FB; PWM audio | `rpi/harness.py` → PNG + FS round-trip |
| pinephone | AArch64 | portrait 480×640 DE2; I2S audio; cntpct seed | `pinephone/harness.py` |
| ppcmac | PowerPC (BE) | 640×480 OF FB; codec PCM; PPC translation | `ppcmac/harness.py` |
| gba | ARM7TDMI | 240×160 Mode-3; SND1 tone; IWRAM FS | `gba/harness.py` |

Each port gained `paint`, `pacman`, `outlast`, `tracker`, and `fs` (USV1 FS +
Files browser + Notepad save), wired through its kernel dispatch/launch/draw, with
the harness persisting the FS region via `--storage=<sidecar>`. On real hardware
the FS medium (SD / eMMC / cart SRAM) is the documented hardware tail; the FS
logic + read/write/persist are fully exercised in the harness.

### SMS (Z80, Sega VDP windowed WM): 3 of 4 apps
Paint, Tracker (SN76489 PSG), OutLast (C64 band model fitted to a window) — all
BlastEm-verified (`sms/run.ps1` → PrintWindow capture). New files
`sms/{paint,tracker,outlast}.inc`, wired into the WM input/draw/launch path.

### NES (6502/2A03, `minimal` profile): COMPLETE — all 4 apps (2026-06-18)
Tracker, OutLast, Pac-Man, Paint — full 11-app parity. Each is its own `.inc`
(`nes/{tracker,outlast,pacman,paint}.inc`) wired into the kernel's
`draw_app`/`up_disp`/`rp_app`/`enter_app` dispatch at app ids 6/8/9/10. The minimal
profile has **no sprite layer**, so everything is background tiles:
- **Tracker** — 14×4 note grid, auto-loop playhead on APU pulse 1, `>` marker as a
  vblank partial; inverted-cell cursor.
- **OutLast** — the C64 band racer with its **own** `$3F00` palette (grass backdrop
  / white road / yellow line / red car); grass = the cleared backdrop.
- **Pac-Man** — the shared 13×13 pillar maze + greedy-chase ghosts (from
  `c64/pacman.s`) as background tiles under a per-app palette; each step repaints
  only the cells actors vacated/entered (flicker-free). 4 new CHR tiles.
- **Paint** — a *cell* canvas + swatch row + hollow-box cursor; NROM CHR is ROM, so
  this is the cell-canvas ceiling (vs the SNES RAM-CHR per-pixel Paint).

Verified on a **new headless py65 PPU harness** (`nes/harness.py`, companion to
`c64/harness.py`): tracks `$2006`/`$2007` → nametable, fires 1 NMI/frame, composes
nametable + CHR-ROM + `$3F00` → a full **256×240 PNG** — deterministic, no
Mesen/GPU/RDP dependency, and it captures the *whole* frame (vs Mesen's 21×17 crop).
This harness + the per-app-palette + background-tile patterns transfer directly to
the remaining tile ports (PCE/VIC-20/GB). Storage: NROM has none (the hardware tail).

### Remaining
- **SMS Pac-Man** — needs a screen-fitted maze (28×25 > 32×24) + the Z80 AI port.
- **SMS battery-SRAM** storage (hardware-tail item).
- **GG, PCE, VIC-20, GB, WS** — tile-model ports; constrained ones (VIC-20 22×23)
  get the feasible subset with the ceiling documented. (NES done — see above.)
- **C-core refactor** — PS2/DC/Mac consume `gen/c/unodef.h` (thin: the ports
  legitimately diverge — MAXWIN=6, TBAR_H=18 are intentional).

## Verify-loop bug classes caught (per ISA)
- Caller-saved register clobbered across a helper call (rpi/gba `pm_walkable`).
- Dropped dispatch case during consolidation (pinephone Notepad).
- Register exhaustion in the steer arg-min (gba — reload tile coords from memory).
- `fill_cells` advances `dp_row`, so road/stripe fills need a per-fill row reset (SMS).

## Real-hardware status (fresh ports)
- **NES** ✅ real AV Famicom (base 7 + Dostris; the 4 parity apps are emulator-only).
- **Raspberry Pi** ✅ boots to desktop on a real Pi 3 (open: brown background =
  XRGB/BGR pixel-order swap; USB-HID input — currently UART-serial only).
- **PC Engine** ✅ real Turbo EverDrive v2.5 — boots + input + sound **under TEOS**
  (the stock Krikzz v2 OS throws "Error 32" on the ROM).
- **PinePhone** ❌ does not boot on the real phone (suspect: U-Boot `go`
  cache-coherency + DE2/DSI bring-up — needs a serial-console debug pass).

The earlier multi-agent real-hardware bootability work on Pi + PinePhone is now
complete; those dirs are no longer being edited concurrently. Full resumable state
is in the agent memory note `unodos-parity-push`.
