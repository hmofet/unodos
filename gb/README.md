# UnoDOS / Nintendo Game Boy + Game Boy Color (Sharp SM83)

The **third** fresh contract-driven port (after SMS and NES) and the **first on the
Sharp SM83** — a genuinely new unogen dialect (`gbz80`, assembled with **rgbds**).
Like the NES it is the Contract's **`minimal` profile** (CONTRACT-ARCH §9): 8 KB
RAM, no mouse, no window manager — one full-screen app at a time with directional
navigation. The launcher is a **vertical list** (the small 160×144 LCD suits a list
better than a grid), a deliberate demonstration that the same Contract scales to a
different layout. One ROM runs on both machines: **greyscale on a DMG, colour on a
Game Boy Color** (the UnoDOS blue/white/cyan/magenta theme), detected at boot.

## Status — M1 · M2 · M3 all shipped ✅

Emulator-verified in Mesen2 (GBC mode), via AUTOTEST scripted-pad builds — the ROM
drives the *same* `$FF00` joypad path, nothing faked (`build/*.png`):

- **M1 — launcher** (`build/desktop.png`): SM83 + LCD bring-up from scratch, the
  shared 8×8 font + the x86 icon donors (and 8×8 mini-icons) baked into the ROM and
  uploaded to VRAM, the inverted **"UnoDOS 3"** title bar + the labelled mini-icon
  list (SysInfo · Clock · Notepad · Music · Files · Theme · Tracker · Dostris ·
  OutLast · Pac-Man · Paint).
- **M2 — directional navigation** (`build/nav.png`): the joypad on `$FF00` (select
  d-pad / buttons, read the low nibble), a vblank-interrupt per-frame loop, an
  Up/Down SELECTION highlight (the selected item's label inverts), **A** launches the
  selected app full-screen, **B** returns. The §8 pointer-less model — *no* WM.
- **M3 — full-screen apps** (`build/{app,clock,dostris,theme,music}.png`): one
  resident app at a time. **SysInfo** (hardware text), live **Clock** (HH:MM:SS off
  the vblank 60 Hz tick), **Notepad** / **Files** (text), **Theme** (cycles the BG
  palette live — BGP on DMG, BG palette RAM on GBC), **Music** (a GB APU pulse tune,
  *Ode to Joy*), and **Dostris** — the falling-blocks game (the NES/SMS algorithm
  ported to SM83). Tracker / OutLast / Pac-Man / Paint open framed placeholders.

### Rendering model (the §9 minimal floor)
The vblank ISR is deliberately minimal — a 60 Hz tick, and it **never touches VRAM**,
so it can never race the main loop. The main loop `halt`s to vblank, then does
**small partial tile-map writes** (highlight move, clock tick, falling piece,
palette) during the vblank window → flicker-free. Big screen changes (launcher↔app,
the Dostris board on a lock) redraw with the **LCD off**. No sprites / no OAM DMA —
every update is cell-stepped BG tiles.

## Hardware brought up (from scratch)

| Part | Detail |
|---|---|
| CPU | Sharp **SM83** (LR35902) @ 4.19 MHz; vblank interrupt vector `$0040` |
| LCD | 160×144, BG tile map at `$9800` (32×32, 20×18 visible), tiles at `$8000` |
| Tiles | 2bpp **interleaved** (16 B/tile: per row, low-plane byte then high-plane byte) — uploaded to VRAM at runtime (no CHR-ROM) |
| Palette | DMG: `BGP` (`$FF47`, 4 greys). GBC: BG palette 0 via `BCPS`/`BCPD` (BGR555) — the UnoDOS theme. Detected at boot (`A=$11` ⇒ GBC) |
| Input | joypad `$FF00` (write to select d-pad / buttons, read the low nibble; active-low) |
| Audio | GB APU pulse channel 1 (`$FF10`–`$FF14`, `$FF24`–`$FF26`) |
| RAM | 8 KB WRAM `$C000`–`$DFFF`; stack at `$FFFE` |
| ROM | 32 KB, no MBC; **CGB-compatible** header (runs on DMG + GBC) |

## Build & run

```sh
sh gb/build.sh                                       # -> gb/build/unodos.gb (CGB-compatible)
sh gb/build.sh nav                                   # AUTOTEST: directional select
sh gb/build.sh app|clock|theme|music|dostris         # AUTOTEST: launch that app/game
powershell -ExecutionPolicy Bypass -File gb/run.ps1 -Rom build\unodos.gb -Out build\desktop.png
```

- `mkdata.py` bakes `build/tiles.bin` (font + inverted font + icons + 8×8 mini-icons +
  Dostris block tiles) and emits `gb_equ.inc` (tile/`NICONS`/`NTHEMES` constants —
  rgbds needs EQUs defined before use) + `gb_data.inc` (theme palettes, Dostris piece
  tables, the APU tune) from the shared assets (`kernel/font8x8.asm`, `build/*.bin`
  icon donors — run `make floppy144` first if they're missing).
- `build.sh` runs mkdata, writes `build/cfg.inc` (the AUTOTEST switches), assembles
  with **rgbasm**, links with **rgblink**, and fixes the header (logo + checksums +
  CGB flag) with **rgbfix**.
- Source split: `kernel.asm` (boot, vblank ISR, joypad, nav, render dispatch, LCD/VRAM
  helpers), `apps.inc` (app draws, clock/theme/music, AUTOTEST script + strings),
  `dostris.inc` (the game). `gb_equ.inc` / `gb_data.inc` are generated.

### AUTOTEST (scripted pad)
`build.sh <target>` generates `build/cfg.inc` with one `AT_*` switch set; the kernel
then replaces the live `$FF00` read with a scripted `{frames, pad}` table driven into
the *same* input path, so one Mesen screenshot proves nav / launch / an app — the ROM
simulates the pad, nothing is faked. The script end idles the pad and freezes Dostris
gravity for a stable shot.

### Capture note (RDP-aware)
Mesen renders through a GPU surface a GDI/PrintWindow grab reads as **black**, so
`run.ps1` flips Mesen to its **software** renderer (grabbable), captures, and restores
it. On this 1280×720 host Mesen renders the 160×144 frame at a large scale anchored
top-left; `run.ps1` sizes the window so the full height fits, and the right ~5 columns
sit past the window edge — a capture cosmetic, so app content + the Dostris board are
laid out in the visible region (the LCD composes the full 160×144).

## Toolchain
- **rgbds 1.0.1** (`C:\Users\arin\gb-tools\` — rgbasm/rgblink/rgbfix) — the Game Boy
  assembler; note rgbds requires `DEF NAME EQU value` (bare `NAME EQU` is rejected).
- **Mesen2** (`C:\Users\arin\snes-tools\mesen\Mesen.exe`) — multi-system; runs the
  `.gb` in GBC mode (so the colour path shows).

## Why a list launcher (vs the NES grid)?
The NES (256×240) has room for a 4-column icon grid; the Game Boy's 160×144 (20×18
cells) does not. Rather than cramp the grid, the GB launcher is a vertical list with
8×8 mini-icons and Up/Down navigation — the same Contract + the same `minimal` profile,
a layout chosen to fit the panel. That contrast is the point: the boundary is fixed,
the presentation is the port's.
