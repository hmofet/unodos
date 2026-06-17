# UnoDOS / Bandai WonderSwan (NEC V30MZ)

The **eighth** fresh contract-driven port — and the **first x86 handheld**. The
WonderSwan's CPU is an **NEC V30MZ**, an 80186-class x86, i.e. the same
instruction family as the reference kernel (`kernel/kernel.asm`). So this port is
written in **nasm** (16-bit real mode) and consumes the Contract's x86 surface
directly. MINIMAL profile (CONTRACT-ARCH §9): a tile-list launcher, one full-screen
app at a time, directional nav via the X-pad.

The WonderSwan is a **hardware tile** machine: 8×8 2bpp planar tiles live in the
16 KB internal RAM (`0x2000`), and a 32×32 SCR1 tilemap (`0x0800`) of 16-bit
entries (`tile# | palette<<9 | flips`) selects them; the LCD shows the top-left
**28×18** tiles (224×144). "Drawing" is a single word store into the map. Colour is
the per-tile **palette** resolving through the mono **shade pool** (ports
`0x1C-0x1F`), so the Theme app recolours the entire screen by rewriting the pool —
no tile data changes. Tile index == ASCII for the font, so the kernel just writes a
character's code into the map.

## Status — M1 · M2 · M3 all shipped ✅

Verified headlessly on a **Unicorn x86 core** (`ws/harness.py`) running the real
WonderSwan ROM in 16-bit real mode — exactly as the GBA port is verified on a
Unicorn ARM core and the MacPlus port on a Unicorn 68K. The harness maps the
cartridge ROM at the top of the 1 MB space so the V30MZ **reset vector `0xFFFF0`**
lands in it (the genuine JMP-FAR boot path), services the I/O ports the kernel
touches, and renders the SCR1 tilemap to a PNG. The AUTOTEST ROMs drive the keypad
through the same input path; nothing is faked (`build/*.png`):

- **M1 — launcher** (`build/desktop.png`): boot, tile/palette upload, the inverted
  "UnoDOS 3" title bar, an 11-item icon list.
- **M2 — navigation** (`build/nav.png`): the keypad on port `0xB5` (group-select),
  a line-counter-synced (`0x03`) frame loop, an Up/Down selection highlight (the
  selected row inverts), **A** launches the app full-screen / **B** returns.
- **M3 — apps** (`build/{app,clock,theme,music,dostris}.png`): SysInfo, live Clock
  (HH:MM:SS off the frame counter), Notepad, Files, Theme (cycles the shade pool),
  Music (the WonderSwan sound channel), and **Dostris** — the falling-blocks game
  with a tile well. Tracker / OutLast / Pac-Man / Paint open framed placeholders.

## Hardware brought up (from scratch)

| Part | Detail |
|---|---|
| CPU | **NEC V30MZ** (80186-class x86) @ ~3.072 MHz |
| Video | tile engine: 8×8 2bpp planar tiles at `0x2000`, SCR1 32×32 tilemap at `0x0800`, 224×144 mono LCD (16 greys) |
| Colour | per-tile palette (ports `0x20-0x3F`) → mono shade pool (ports `0x1C-0x1F`) |
| Display ctrl | port `0x00` (SCR1 enable), `0x07` (map base), `0x10/0x11` (scroll), `0x14` (LCD on) |
| Input | keypad port `0xB5`: write a group select (`0x20`=X-pad, `0x40`=buttons), read the low nibble |
| Timing | the line counter `0x03` (≥144 = vblank) |
| Audio | sound channel 1 — wavetable at `0x0040`, period `0x80/0x81`, ctrl `0x90/0x91` |
| RAM | 16 KB internal: vars `0x0100`, stack `→0x2000`, tilemap `0x0800`, tiles `0x2000` |
| Boot | 64 KB ROM mapped flush to `0xFFFFF`; reset vector `0xFFFF0` = JMP FAR to `start`; 16-byte footer header (mono flag, checksum) |

## Build & run

```sh
sh ws/build.sh                                       # -> ws/build/unodos.ws
sh ws/build.sh nav|app|clock|theme|music|dostris     # AUTOTEST builds
python ws/harness.py ws/build/unodos.ws ws/build/desktop.png
```

- `mkdata.py` bakes the tile set (ASCII font + a solid block + 11 launcher icons,
  all 2bpp planar), the Dostris piece tables, the tone tune, and the theme
  shade-pool presets into `ws_data.inc` (+ assemble-time `ws_equ.inc`); the kernel
  DMA-copies the tiles to `0x2000` at boot.
- `build.sh` runs mkdata, assembles `kernel.asm` with **nasm** (AUTOTEST via
  `-DAUTOTEST`/`-DSCENE`), and patches the WonderSwan header checksum.
- Source split: `kernel.asm` (boot, video/palette init, keypad, nav, render
  dispatch, the tile-map helpers), `apps.inc` (apps, clock/theme/music, the
  AUTOTEST scripts, strings), `dostris.inc` (the game). `ws_data.inc` is generated.
- `min.asm` is a tiny standalone boot test (stripes) that de-risks the
  reset-path + tile-render pipeline.

## Toolchain
- **nasm** (`C:\Users\arin\nasm-tools\…\nasm.exe`) — the same x86 assembler the
  reference `kernel/` uses.
- **Unicorn** (Python) — the CPU core for the ROM-free headless harness.

A real WonderSwan (or an accurate emulator such as Mednafen) is the remaining tail:
the harness models the display registers + keypad + reset path faithfully, but the
sound output is square-wave audio best judged on hardware by ear.
