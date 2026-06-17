# UnoDOS / Commodore VIC-20 (MOS 6502 + VIC 6560/6561)

The **seventh** fresh contract-driven port — 6502 again (like the NES), reusing
`gen/6502/` + dasm. MINIMAL profile (CONTRACT-ARCH §9): a character-cell list
launcher, one full-screen app at a time, directional nav via the joystick.

The VIC is a **character-cell** display: a 22×23 screen matrix at `$1E00` (char
codes), colour RAM at `$9600` (a per-cell nibble = 8 fg colours over the global
background), and a custom 8×8 1bpp char set copied to `$3000` at boot. So
"drawing" is just a store to `$1E00 + row*22 + col` (plus a colour to `$9600+…`)
— the simplest display layer of any UnoDOS port. An **inverted** char set
(complement bitmaps) gives the title-bar and selection bars.

## Status — M1 · M2 · M3 all shipped ✅

Verified headlessly on a **py65 6502 core** (`vic20/harness.py`) that renders the
character matrix — the same approach as the C64/Apple II ports. The AUTOTEST ROMs
drive the joystick through the same input path; nothing is faked (`build/*.png`):

- **M1 — launcher** (`build/desktop.png`): VIC bring-up, a mini-icon list, the
  inverted "UnoDOS 3" title bar.
- **M2 — navigation** (`build/nav.png`): the joystick on `$9111`/`$9120`, a
  raster-synced (`$9004`) frame loop, an Up/Down selection highlight (the selected
  label inverts), **Fire** launches the app full-screen / returns from it.
- **M3 — apps** (`build/{app,clock,dostris,theme,music}.png`): SysInfo, live Clock
  (HH:MM:SS off the frame counter), Notepad, Files, Theme (cycles the background
  colour `$900F`), Music (a VIC oscillator tune), and **Dostris** — the
  falling-blocks game in colour. Tracker / OutLast / Pac-Man / Paint open
  framed placeholders.

## Hardware brought up (from scratch)

| Part | Detail |
|---|---|
| CPU | **MOS 6502** @ ~1 MHz |
| Video | **VIC** 6560/6561: 22×23 char matrix at `$1E00`, colour RAM `$9600`, 8×8 1bpp char set at `$3000`; registers `$9000`–`$900F` |
| Colour | per-cell fg nibble (8 colours) over the global background (`$900F`) |
| Input | joystick — `$9111` (up/down/left/fire) + `$9120` bit 7 (right), active-low |
| Timing | the VIC raster `$9004` (frame sync) |
| Audio | VIC tone oscillator `$900B` + volume `$900E` |
| RAM | targets a **+8K** expanded VIC-20: PRG at `$1201`, code/data through `$2FFF`, char set `$3000` |

## Build & run

```sh
sh vic20/build.sh                                     # -> vic20/build/unodos.prg
sh vic20/build.sh nav|app|clock|theme|music|dostris   # AUTOTEST builds
python vic20/harness.py vic20/build/unodos.prg vic20/build/desktop.png
```

- `mkdata.py` bakes the custom char set (font + inverted font + 8×8 mini-icons +
  blocks), the Dostris piece tables, the tune, and a row-offset table, into
  `vic_data.inc`; the kernel copies the char set to `$3000` at boot.
- `build.sh` runs mkdata, assembles `kernel.s` with **dasm** (AUTOTEST via `-D`
  switches), and prepends the `$1201` load address to make a CBM `.prg`.
- Source split: `kernel.s` (boot, VIC init, input, nav, render dispatch, the
  char-matrix helpers), `apps.inc` (app draws, clock/theme/music, AUTOTEST +
  strings), `dostris.inc` (the game). `vic_data.inc` is generated.

## Toolchain
- **dasm** (`C:\Users\arin\apple2-tools\dasm.exe`) — the same 6502 assembler the
  NES / C64 / Apple II ports use.
- **py65** (Python) — the ROM-free 6502 core for the headless harness.

A real VIC-20 needs the `$9005` video-matrix/char-base nibble set precisely for
the chosen `$1E00`/`$3000` layout; the kernel sets a best-effort value, and the
py65 harness reads the configured locations directly (the same modelling the C64/
Apple II harnesses use). On-hardware verification (VICE / a real VIC-20) is the
remaining tail.
