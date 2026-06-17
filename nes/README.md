# UnoDOS / Nintendo Entertainment System (6502 / 2A03)

The NES is the Contract's **`minimal` profile** flagship (CONTRACT-ARCH §9): 2 KB
RAM, no mouse, no window manager — one full-screen launcher with directional
navigation. That makes it a deliberate contrast to the windowed tile-console ports
(SMS/Genesis/SNES): the *other* end of the profile spectrum the Contract scales
across. It reuses the already-proven **dasm** 6502 toolchain (shared with the C64
and Apple II ports) and the Contract's `gen/6502/` world; the screen geometry comes
from `[world.nes]` via unogen (`gen/nes/sys_gen.inc`).

## Status — milestone 1 (boot to launcher) ✅

Emulator-verified in Mesen2 (`build/desktop.png`): 6502 + PPU bring-up from scratch,
the shared 8×8 font and the x86 icon donors baked into CHR-ROM, and a rendered
full-screen launcher — the inverted **"UnoDOS 3"** title bar + the labelled icon
grid (SysInfo · Clock · Notepad · Music · Files · Theme · Tracker · Dostris ·
OutLast · Pac-Man · Paint).

Next (the `minimal` model, not a WM): M2 = directional-focus navigation (d-pad moves
the selected icon, A launches it full-screen, B returns — the §8 pointer-less input
model) + the controller on `$4016`; M3 = full-screen apps.

## Hardware brought up (from scratch)

| Part | Detail |
|---|---|
| CPU | 6502 / 2A03 @ 1.79 MHz; reset/NMI/IRQ vectors at `$FFFA`–`$FFFF` |
| PPU | 2C02, 256×240, 32×30 tile nametable at `$2000`; regs `$2000`–`$2007` |
| Tiles | 2bpp planar in **CHR-ROM** (16 B/tile) — the PPU reads patterns directly, no runtime upload |
| Palette | one 4-colour background palette at `$3F00` ({blue, white, cyan, magenta}); attribute table all zeros |
| RAM | 2 KB `$0000`–`$07FF` (cleared at boot); stack page `$0100` |
| ROM | iNES **NROM-256** (mapper 0): 16 B header + 32 KB PRG + 8 KB CHR |

### The NES backdrop quirk
PPU colour-index 0 *always* renders as the universal backdrop (`$3F00`), in every
palette — so "inverted" title-bar text can't be a palette swap (its white
background would be index 0 = the blue backdrop). The data generator emits a real
inverted font whose foreground is index 0 (blue) on an index-1 (white) background.

## Build & run

```sh
sh nes/build.sh                                       # -> nes/build/unodos.nes (NROM-256)
powershell -ExecutionPolicy Bypass -File nes/run.ps1  # Mesen2 + screenshot -> build/desktop.png
```

- `mkdata.py` bakes `build/chr.bin` (font + icon tiles) + emits `nes_data.inc`
  (tile equates + palette) from the shared assets (`kernel/font8x8.asm`,
  `build/*.bin` icon donors — run `make floppy144` first if they're missing).
- `build.sh` runs mkdata, assembles `kernel.s` with **dasm** (`-f3`, 32 KB PRG),
  then `mkines.py` packs the iNES ROM.

### Capture note (RDP-aware)
Mesen renders through a GPU surface that a GDI/PrintWindow grab reads as **black**
(like BlastEm's GL surface), and its own F12 screenshot is focus-flaky over RDP for
some ROMs. So `run.ps1` temporarily flips Mesen to its **software** renderer (which
the focus-independent helper *can* grab), captures, then restores the setting. Mesen
letterboxes the frame to the window, so a column may sit at the window edge — a
capture cosmetic, not a render defect (the launcher renders the full 256×240).

## Toolchain

- **dasm** (`C:\Users\arin\apple2-tools\dasm.exe`) — the same 6502 assembler the C64
  and Apple II ports use.
- **Mesen2** (`C:\Users\arin\snes-tools\mesen\Mesen.exe`) — multi-system; runs the
  iNES ROM in NES mode.

## Why NES (vs Game Boy)
The NES uses a 6502 core, so it reuses `gen/6502/` and dasm as-is. The Game Boy is a
Sharp **LR35902** (a Z80 relative) needing rgbds + a new `gbz80` unogen dialect — a
separate port, deferred until that toolchain is installed.
