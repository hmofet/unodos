# UnoDOS / Nintendo Entertainment System (6502 / 2A03)

The NES is the Contract's **`minimal` profile** flagship (CONTRACT-ARCH §9): 2 KB
RAM, no mouse, no window manager — one full-screen launcher with directional
navigation. That makes it a deliberate contrast to the windowed tile-console ports
(SMS/Genesis/SNES): the *other* end of the profile spectrum the Contract scales
across. It reuses the already-proven **dasm** 6502 toolchain (shared with the C64
and Apple II ports) and the Contract's `gen/6502/` world; the screen geometry comes
from `[world.nes]` via unogen (`gen/nes/sys_gen.inc`).

## Status — M1 · M2 · M3 all shipped ✅ — **validated on real hardware** 🎮

**Validated on a real AV Famicom** (HVC-101): the desktop boots, the `$4016`
controller drives navigation, and the 2A03 APU audio plays — confirmed on physical
silicon. This is the **first of the fresh-3.1 ports to run on real hardware**.

Also emulator-verified in Mesen2 (the `build/*.png` shots, via AUTOTEST scripted-pad
builds — the ROM drives the *same* `$4016` input path, nothing faked):

- **M1 — launcher** (`build/desktop.png`): 6502 + PPU bring-up from scratch, the
  shared 8×8 font + the x86 icon donors baked into CHR-ROM, and the full-screen
  launcher — inverted **"UnoDOS 3"** title bar + the labelled icon grid (SysInfo ·
  Clock · Notepad · Music · Files · Theme · Tracker · Dostris · OutLast · Pac-Man ·
  Paint).
- **M2 — directional navigation** (`build/nav.png`): the standard pad on `$4016`
  (strobe, then 8 bits A·B·Sel·Start·U·D·L·R), a vblank-NMI per-frame loop, a
  d-pad SELECTION highlight (the selected icon's label inverts), **A** launches the
  selected app full-screen, **B** returns. The §8 pointer-less model — *no* WM.
- **M3 — full-screen apps** (`build/{app,clock,dostris,theme,music}.png`): one
  resident app at a time, dispatched by the launcher. **SysInfo** (hardware text),
  live **Clock** (HH:MM:SS off the NMI 60 Hz tick), **Notepad** / **Files** (text),
  **Theme** (cycles the `$3F00` palette live), **Music** (a 2A03 APU pulse tune,
  *Ode to Joy*), and **Dostris** — a from-scratch falling-blocks game (the SMS
  port's algorithm ported to 6502: 7 tetrominoes, collision, rotation, gravity,
  line-clears).
- **M3 apps — full 11-app parity** (`build/{tracker,outlast,pacman,paint}.png`):
  the four remaining apps, each re-expressed for the minimal profile (no WM, no
  sprite layer — everything is background tiles):
  - **Tracker** — an `NT_ROWS×NT_CH` note grid that auto-loops, playing the
    leftmost note of the playhead row on the APU pulse 1; the d-pad moves an
    inverted-cell cursor, A cycles the note (`-- C4…C5`), a `>` marker steps the
    rows. The playhead is a small vblank partial; cursor/note edits redraw.
  - **OutLast** — the C64/SMS pseudo-3D "band" racer: a road span per band whose
    half-width grows toward the camera and whose centre snakes, a steerable car,
    off-road = CRASH, distance = score. Installs its **own** palette (grass-green
    backdrop / white road / yellow centre line / red car) — grass is the cleared
    backdrop, so only the road, line and car are drawn.
  - **Pac-Man** — the shared 13×13 pillar maze + greedy-chase ghosts (the C64
    algorithm): blue walls, white dots, yellow power pellets, a yellow Pac-Man and
    two white ghosts; queued-turn steering, eat all dots to WIN, a ghost on your
    tile = OVER. Maze + actors are background tiles under Pac-Man's own palette;
    each game step repaints only the few cells the actors vacated/entered (a vblank
    partial — flicker-free).
  - **Paint** — a cell canvas + swatch row (erase / white / cyan / magenta) with a
    hollow-box cursor; A paints (or picks a swatch). Because the NES CHR is **ROM**
    (NROM-256), this is a *cell* canvas, not the per-pixel canvas the RAM-CHR ports
    (SNES) can do — the documented minimal-profile ceiling.

  Deviations forced by the 4-colour background palette and CHR-ROM: each game
  installs a per-app `$3F00` palette (the launcher restores the default on exit),
  and Pac-Man's actors use white/yellow rather than the arcade colours (no spare
  palette index for distinct ghost hues in the minimal model).

### Rendering model (the §9 minimal floor)
The NMI is deliberately minimal — a 60 Hz tick + a vblank flag, and it **never
touches the PPU**, so it can never race the main loop's VRAM writes (the PORT-SPEC
ISR rule). The main loop syncs to vblank, then does **small partial nametable
writes** (highlight move, clock tick, falling piece, palette) that fit inside
vblank → flicker-free. Big screen changes (launcher↔app, the Dostris board on a
lock) redraw the whole nametable with **rendering off** (a brief settle). No
sprites and no OAM DMA are needed — every update is cell-stepped tiles.

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
sh nes/build.sh                                       # -> nes/build/unodos.nes (interactive)
sh nes/build.sh nav                                   # AUTOTEST: directional select  -> unodos_nav.nes
sh nes/build.sh app|clock|theme|music|dostris         # AUTOTEST: launch that app/game
sh nes/build.sh tracker|outlast|pacman|paint          # AUTOTEST: launch a parity app
powershell -ExecutionPolicy Bypass -File nes/run.ps1 -Rom build\unodos.nes -Out build\desktop.png
printf 'frames 120\nshot pacman\nquit\n' | python nes/harness.py build/unodos_pacman.nes build  # headless
```

- `mkdata.py` bakes `build/chr.bin` (font + inverted font + icon tiles + Dostris
  block tiles) and emits `nes_data.inc` (tile equates, palette, the Dostris piece
  tables, and the APU tune) from the shared assets (`kernel/font8x8.asm`, `build/*.bin`
  icon donors — run `make floppy144` first if they're missing).
- `build.sh` runs mkdata, writes `build/cfg.inc` (the AUTOTEST switches), assembles
  `kernel.s` with **dasm** (`-f3`, 32 KB PRG), then `mkines.py` packs the iNES ROM.
- Source split: `kernel.s` (boot, NMI, input, nav, render dispatch, PPU helpers),
  `apps.inc` (app draws, clock/theme/music, the AUTOTEST script + strings),
  `dostris.inc`, `tracker.inc`, `outlast.inc`, `pacman.inc`, `paint.inc` (one game
  each). `nes_data.inc` is generated; don't hand-edit it.

### Headless harness (`harness.py`) — the primary verify loop
`harness.py` is a ROM-free py65 PPU emulator (the companion to `c64/harness.py`):
it tracks `$2006`/`$2007` writes into a nametable, fires one NMI per emulated
frame (the kernel's `wait_vbl` cadence), and composes the nametable + CHR-ROM +
`$3F00` palette to a **256×240 PNG** — the *full* frame, deterministic, and with
no Mesen/GPU/RDP dependency. Drive an AUTOTEST build with a tiny stdin script
(`frames N` / `shot NAME` / `assert apu>0` / `quit`). This is what the four parity
shots were verified with; `run.ps1` (Mesen) remains for an on-silicon-style check.

### AUTOTEST (scripted pad)
`build.sh <target>` generates `build/cfg.inc` with one `AT_*` switch set. The kernel
then replaces the live `$4016` read with a scripted `{frames, pad}` table driven into
the *same* input path, so one Mesen screenshot proves nav / launch / an app — the
ROM simulates the pad, nothing is faked. When the script ends it idles the pad and
freezes Dostris gravity for a stable shot.

### Capture note (RDP-aware)
Mesen renders through a GPU surface that a GDI/PrintWindow grab reads as **black**
(like BlastEm's GL surface), and its own F12 screenshot is focus-flaky over RDP for
some ROMs. So `run.ps1` temporarily flips Mesen to its **software** renderer (which
the focus-independent helper *can* grab), captures, then restores the setting. On
this 1280×720 host Mesen renders at 3× anchored top-left, so the grab shows the
top-left ~21×17 cells (the launcher's rightmost icon column sits past the window
edge) — a capture cosmetic, not a render defect (the PPU composes the full 256×240).
The apps and the Dostris board are laid out in that visible region so each shot
proves them in full.

## Toolchain

- **dasm** (`C:\Users\arin\apple2-tools\dasm.exe`) — the same 6502 assembler the C64
  and Apple II ports use.
- **Mesen2** (`C:\Users\arin\snes-tools\mesen\Mesen.exe`) — multi-system; runs the
  iNES ROM in NES mode.

## Why NES (vs Game Boy)
The NES uses a 6502 core, so it reuses `gen/6502/` and dasm as-is. The Game Boy is a
Sharp **LR35902** (a Z80 relative) needing rgbds + a new `gbz80` unogen dialect — a
separate port, deferred until that toolchain is installed.
