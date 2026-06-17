# UnoDOS / Sega Game Gear (Z80 + 315-5124 VDP)

The **fourth** fresh contract-driven port — and a study in **reuse**. The Game Gear
is SMS silicon: the same Z80 and the same 315-5124 VDP, so this port consumes the
same `gen/z80/` world as the [SMS port](../sms/README.md) and reuses its hardware
bring-up, its 4bpp tile data, its SN76489 PSG audio, and the Dostris algorithm.
But the GG's LCD shows only the **centre 160×144 of the 256×192 frame = 20×18 cells**
— which is exactly the [Game Boy's panel](../gb/README.md). So the Game Gear wears
the GB's **`minimal` layout** (CONTRACT-ARCH §9): a vertical mini-icon list launcher,
one full-screen app at a time, directional nav. It sits squarely between its two
cousins: **SMS hardware, Game-Boy-sized screen.**

Everything draws at a **(6,3) cell offset** into the visible window. The one real
hardware delta exercised vs. the SMS is **CRAM**: the GG stores 12-bit colour (2
bytes/entry) instead of the SMS's 6-bit single byte.

## Status — M1 · M2 · M3 all shipped ✅

Emulator-verified in Mesen2 (Game Gear mode), via AUTOTEST scripted-pad builds —
the ROM drives the *same* `$DC` pad path, nothing faked (`build/*.png`):

- **M1 — launcher** (`build/desktop.png`): Z80 + VDP bring-up (reused from the SMS),
  the shared font + the x86 icon donors (and 8×8 mini-icons), the inverted
  **"UnoDOS 3"** title bar + the labelled mini-icon list.
- **M2 — directional navigation** (`build/nav.png`): the pad on `$DC`, a frame-
  interrupt per-frame loop, an Up/Down SELECTION highlight (the selected label
  inverts), **button 1** launches the selected app full-screen, **button 2** returns.
- **M3 — full-screen apps** (`build/{app,clock,dostris,theme,music}.png`): one
  resident app at a time. **SysInfo**, live **Clock** (HH:MM:SS off the frame tick),
  **Notepad** / **Files** (text), **Theme** (cycles the 12-bit CRAM palette live),
  **Music** (the SN76489 PSG, *Ode to Joy*), and **Dostris** — the falling-blocks
  game, in full colour (the SMS palette gives green/cyan/yellow/orange/magenta
  pieces). Tracker / OutLast / Pac-Man / Paint open framed placeholders.

### Rendering model (the §9 minimal floor)
The frame ISR is minimal — a 60 Hz tick, and it **never touches VRAM**. The main
loop `halt`s to the frame interrupt, then does **small partial name-table writes**
(highlight, clock, falling piece, palette) during the vblank window → flicker-free.
Big screen changes (launcher↔app, the Dostris board on a lock) redraw with the
**display off** (R1 bit 6). The SAT is terminated at boot (`Y=$D0`) so the sprite-
less UI shows no garbage sprites. No sprites are used.

## Hardware brought up (reused from the SMS)

| Part | Detail |
|---|---|
| CPU | **Z80** @ 3.58 MHz; frame-interrupt vector `$0038` |
| VDP | **315-5124** Mode 4, name table at `$3800`; tiles 4bpp planar in VRAM `$0000` |
| Screen | 256×192 frame; the GG LCD shows the **centre 160×144** (20×18), drawn at a (6,3) offset |
| CRAM | **12-bit** colour (2 bytes/entry: low `%GGGGRRRR`, high `%0000BBBB`) — the one delta vs. SMS |
| Input | control pad `$DC` (active-low U/D/L/R + buttons 1/2) |
| Audio | **SN76489 PSG** via port `$7F` (reused from the SMS port) |
| RAM | 8 KB `$C000`–`$DFFF`; the Sega mapper |
| ROM | 32 KB `.gg` cartridge (`TMR SEGA` header, region = GG export) |

## Build & run

```sh
sh gg/build.sh                                       # -> gg/build/unodos.gg
sh gg/build.sh nav                                   # AUTOTEST: directional select
sh gg/build.sh app|clock|theme|music|dostris         # AUTOTEST: launch that app/game
powershell -ExecutionPolicy Bypass -File gg/run.ps1 -Rom build\unodos.gg -Out build\desktop.png
```

- `mkdata.py` reuses the SMS tile generator (4bpp planar font + inverted font + icons
  + game-block solids) and adds 8×8 mini-icons, the 12-bit GG theme palettes, the
  Dostris piece tables, and the PSG tune — emitted into `gen_data.inc`.
- `build.sh` runs mkdata, then assembles with **sjasmplus** (`--raw`, 32 KB). AUTOTEST
  switches are passed as `-DAUTOTEST=1 -DAT_*=1` (the SMS pattern), not a cfg file.
- Source split: `kernel.asm` (boot, frame ISR, pad, nav, render dispatch, VDP helpers),
  `apps.inc` (app draws, clock/theme/music, AUTOTEST + strings), `dostris.inc` (the
  game). `gen_data.inc` is generated.

### Capture note (RDP-aware)
Same Mesen2 rig as the NES/GB ports: `run.ps1` flips Mesen to its software renderer
(grabbable over RDP), sizes the window so the full height fits, captures with the
focus-independent helper, and restores the setting. Mesen renders the frame larger
than the window so the right ~5 columns sit past the edge — a capture cosmetic, so
app content + the Dostris board are laid out in the visible region.

## Toolchain
- **sjasmplus** (`C:\Users\arin\z80-tools\…`) — the same Z80 assembler the SMS port uses.
- **Mesen2** (`C:\Users\arin\snes-tools\mesen\Mesen.exe`) — runs the `.gg` in Game Gear
  mode (so the 12-bit colour path shows).

## Why a `minimal` port (vs. the SMS's window manager)?
The SMS port is `windowed` — a desktop with a tile WM — because its 256×192 screen has
room. The Game Gear runs the *same VDP*, but only the centre 160×144 reaches the LCD,
so a windowed desktop would spill off the panel. The honest fit is the GB's `minimal`
profile: one full-screen app, directional nav. Same Contract, same silicon as the SMS,
the layout chosen for the panel — which is the whole point.
