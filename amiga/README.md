# UnoDOS/68K — Amiga port (milestone 2.5)

A bare-metal Motorola 68000 port of UnoDOS for A500-class Amigas
(OCS/ECS, 512 KB chip RAM), per the platform contract in
[`docs/PORT-SPEC.md`](../docs/PORT-SPEC.md). It boots from a
self-booting ADF, takes over the machine (no Kickstart libraries used at
runtime), and runs the UnoDOS desktop with in-kernel apps.

## What it does

- **Boot splash** — striped Amiga-checkmark art with "UnoDOS 3" rendered
  at 2× and "for Commodore Amiga" (~2 s, raster-timed).
- **Display** — 320×200 in **5 bitplanes (32 colors)**, the OCS lowres
  maximum. Copper-driven: palette entries 0–3 are the themed UI colors,
  4–31 a fixed extended palette for the games (17–19 double as the
  hardware-sprite cursor registers). Per-plane software fill/text
  primitives; hardware-sprite mouse cursor.
- **Window manager** — frames, title bars, close box, z-order with
  click-to-raise, clamped XOR-outline drag; the focus-routed,
  press-time-stamped event model from the spec.
- **Audio** — 4-channel Paula: square-wave sequencers for the Music app
  and game music, all four DMA channels in the Tracker.

## Apps (10 desktop icons, two rows)

| App | Notes |
|---|---|
| **Sys Info** | machine/uptime panel |
| **Clock** | live clock |
| **Files** | ROM-disk browser; Enter opens in Notepad |
| **Notepad** | caret editor, left/right/up/down with goal-column memory, vertical scroll, live `Ln/Co/bytes` status bar, F1 save (RAM until FAT12) |
| **Music** | Canon in D on Paula (same arrangement as `apps/music.asm`) |
| **Theme** | the 8 shared preset palettes + per-channel custom RGB editing of UI colors 0–3, applied live through the copper list |
| **Dostris** | port of `apps/tetris.asm` — same piece tables/scoring/speed curve, the seven VGA piece colors, Korobeiniki on Paula |
| **OutLast** | port of `apps/outlast.asm` — same track/perspective/traffic/physics, full-color scenery, Sunset Drive on Paula |
| **Pac-Man** | port of `apps/pacman.asm` — full 28×25 maze, three-ghost AI (Blinky/Pinky/Clyde), scatter/chase schedule, frightened mode; incremental tile rendering |
| **Tracker** | write + play 4-channel MOD-style music: ProTracker periods (C-2..B-3), 32-row pattern editor, 4 chip-synthesized instruments (square/saw/triangle/noise), demo song, instant edit preview. Keys: arrows move, q/w note, e instrument, x clear, d demo, Space play/stop |

## Build

Needs `vasmm68k_mot` and `exe2adf` (both shipped inside the
[amiga-debug VS Code extension](https://github.com/BartmanAbyss/vscode-amiga-debug)
VSIX), plus Python 3 and a built x86 tree (`make floppy144`) for the
shared assets:

```sh
cd amiga
./build.sh          # -> build/unodos68k.adf
./build.sh test     # -> build/unodos68k_test.adf (auto-launches apps)
```

`mkdata.py` regenerates `gen_data.i` from the x86 tree: the 8×8 font and
app icons byte-identical from the x86 binaries, the Amiga keymap, the
ROM-disk files, and the music tables (Canon in D plus the game songs,
parsed straight out of `apps/tetris.asm` / `apps/outlast.asm` and
converted to Paula periods).

### Autotest variants

WinUAE blocks host→guest key injection, so verification builds drive the
real key handlers at boot and the harness screenshots the window:

```sh
vasmm68k_mot -Fhunkexe -nosym -DAUTOTEST=1 -DAUTOTEST_<X>=1 -o out kernel.asm
```

with `<X>` one of `NOTEPAD` (demo text + six up-arrows), `THEME` (applies
the Sunset preset), `DOSTRIS` (hard-drops six pieces), `OUTLAST` (60
physics steps), `PACMAN` (150 game steps), `TRACKER` (demo song +
playback). Plain `-DAUTOTEST=1` launches the Files/Notepad/Music stack.

## Run

WinUAE with the built-in AROS ROM (no Kickstart needed) — config in
`uae/unodos.uae`; `uae/autotest.ps1` and `uae/snapwin.ps1` drive
headless screenshot runs.

## Architecture

`kernel.asm` plus includes: `apps_m2.i` (Files/Notepad/Music + shared
helpers), `theme.i` (theme engine + splash), `games.i` (Dostris,
OutLast, game-music sequencer), `pacman.i`, `tracker.i`, and the
generated `gen_data.i`. The event queue, window manager, scheduler
scaffolding and app procs follow PORT-SPEC; the Amiga-specific layer
(copper, bitplane primitives, CIA/sprite ISRs, Paula) is the future HAL.

Chip-RAM map: 5 contiguous bitplanes at `$60000`, copper list `$76000`,
sprite data `$76800`, square-wave + tracker instrument samples
`$76A00`/`$76B00`, supervisor stack to `$7C000`.

## Known limitations (milestone 2.5)

- Storage is the boot ROM-disk (build-time files, RAM-editable); the
  portable FAT12 core + an MFM track reader are the next milestone, and
  unlock Notepad/Tracker file save and PC-interchangeable disks.
- Single cooperative context (the scheduler is scaffolding); real
  multitasking over the window/app tables is milestone 3.
- Notepad: 2 KB buffer; long lines clip (no horizontal scroll).
- Tracker: one 32-row pattern, fixed instrument volumes, no effect
  column yet; .MOD import/export needs FAT12.
- The vblank tick runs fast under WinUAE's default pacing for this
  config (uptime advances ~4× wall-clock) — a `TICKS_SEC` calibration
  item, not a logic bug.
- Text/fills are CPU RMW (no blitter yet — the big OCS win). Game
  windows repaint visibly; Pac-Man already uses dirty-tile rendering.
- 640-wide OCS hires (16 colors max) would need PORT-SPEC content
  scaling through the WM; lowres 32-color is the current ceiling.
- Audio is register-verified (test configs run sound off); real-hardware
  ear-check pending.
