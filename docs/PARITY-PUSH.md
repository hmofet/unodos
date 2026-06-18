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

### Remaining
- **SMS Pac-Man** — needs a screen-fitted maze (28×25 > 32×24) + the Z80 AI port.
- **SMS battery-SRAM** storage (hardware-tail item).
- **GG, PCE, NES, VIC-20, GB, WS** — tile-model ports; constrained ones (NES 2 KB,
  VIC-20 22×23) get the feasible subset with the ceiling documented.
- **C-core refactor** — PS2/DC/Mac consume `gen/c/unodef.h` (thin: the ports
  legitimately diverge — MAXWIN=6, TBAR_H=18 are intentional).

## Verify-loop bug classes caught (per ISA)
- Caller-saved register clobbered across a helper call (rpi/gba `pm_walkable`).
- Dropped dispatch case during consolidation (pinephone Notepad).
- Register exhaustion in the steer arg-min (gba — reload tile coords from memory).
- `fill_cells` advances `dp_row`, so road/stripe fills need a per-fill row reset (SMS).

## Coordination
Pi + PinePhone may also be edited by other agents for real-hardware bootability
(e.g. rpi `PERIPH`-base Pi 4 support was layered around the app code). Re-read
those dirs before touching them. Full resumable state is also in the agent memory
note `unodos-parity-push`.
