# UnoDOS / Sega Master System (Z80)

The **first UnoDOS port built fresh on the contract-driven + greenfield-window
architecture** (CONTRACT-ARCH §13). The SMS has a true **Z80**, so it consumes the
Contract's existing `gen/z80/` world (sjasmplus) directly — no new CPU dialect, no
new assembler emitter. Screen geometry and the window/event layout come from
`[world.sms]` in `unodef/unodef.toml` via unogen (`gen/sms/sys_gen.inc`), so they
can never drift from the Contract.

## Status — M1 · M2 · M3 ✅ (emulator-verified)

- **M1 — desktop** (`build/desktop.png`): Z80 + VDP bring-up from scratch, the
  shared 8×8 font + the x86 icon donors uploaded as VDP tiles, the inverted
  **"UnoDOS 3"** title bar and the labelled icon grid, plus the hardware-sprite
  cursor.
- **M2 — window manager + input + events** (`build/wm.png`): a VBlank-driven
  cooperative loop; a d-pad-steered hardware-sprite cursor; the Contract event
  queue (`ev_post`/`ev_get`, `EV_MOUSE`); and a tile window manager — create,
  draw (title bar + X close box + border chrome), focus-raise, **drag** by the
  title bar, close, all z-ordered — reached by clicking the desktop icons.
- **M3 — app sweep** (`build/wm.png`): **SysInfo** (hardware readout), **Clock**
  (live `HH:MM:SS`, ticked by the COOP scheduler floor), **Notepad**, **Files**,
  and **Theme** (cycles the desktop palette live by reprogramming CRAM) have real
  content; the remaining icons open framed placeholder windows.

`wm.png` is one AUTOTEST screenshot exercising launch + z-order + the live Clock +
the Theme re-theme + a window drag together.

### Window manager design
Windows live in the Contract's 16-byte `win_entry` (`WSTATE/WPROC/WX/WY/WW/WH/
WTITLE`, `MAXWIN=4`) in cell coordinates; z-order is a side list (`zlist`,
top..bottom). The whole scene is re-composed on the name table whenever something
changes (`v_dirty`), back-to-front; the cursor is sprite 0 so it floats above the
cell world without a redraw. Per the PORT-SPEC §6 audit rules the VBlank ISR only
ticks + sets a flag — the main loop owns every VDP access.

## Hardware brought up (from scratch)

| Part | Detail |
|---|---|
| CPU | Z80 @ 3.58 MHz; `im 1`, `jp boot` over the RST/INT/NMI vectors |
| VDP | Sega 315-5124 **Mode 4**, 256×192 = 32×24 cells; data port `$BE`, control/status `$BF` |
| Tiles | 4bpp **planar** (4 bytes/row, one per bitplane), 32 bytes/tile, uploaded to VRAM `$0000` |
| Name table | `$3800`, 32×28 word entries (tile index + flags), cleared to the desktop tile |
| CRAM | 16-entry background palette (`%00BBGGRR`) from the four UnoDOS theme colours; duplicated into the sprite palette so the border is desktop-blue |
| Interrupts | VBlank frame interrupt (`im 1`, R1 bit5); the ISR only ticks `v_ticks` + sets a flag |
| Sprites | cursor = sprite 0, 8×16 (R1 bit1), pattern base `$0000`, SAT at `$3F00` |
| Input | control pad on port `$DC` (active-low); d-pad → cursor, trigger 1 = click, trigger 2 = close |
| Mapper | Sega `$FFFC–$FFFF` paging (banks 0/1/2); code lives in banks 0–1 |
| RAM | 8 KB work RAM `$C000–$DFFF` (cleared at boot); stack at `$DFF0` |
| ROM | 32 KB `.sms` image with the `TMR SEGA` header at `$7FF0` |

## Display model

Everything composes on the name table as `(tile)` cells snapped to the 8px grid,
exactly like the Genesis/SNES cousins. Two font tile sets are generated: a normal
set (white-on-blue) for body text and an **inverted** set (blue-on-white) for the
title bar. Icons are 16×16 = 2×2 tiles, pulled from the x86 `.BIN` headers (the same
donors Genesis uses) and re-mapped onto the SMS palette.

## Build & run

```sh
sh sms/build.sh                         # -> sms/build/unodos.sms      (interactive, 32 KB)
sh sms/build.sh test                    # -> sms/build/unodos_test.sms (AUTOTEST: scripted pad)
powershell -ExecutionPolicy Bypass -File sms/run.ps1                                   # boot desktop -> build/desktop.png
powershell -ExecutionPolicy Bypass -File sms/run.ps1 -Rom build\unodos_test.sms -Out build\wm.png  # WM proof
```

The AUTOTEST variant drives a scripted pad sequence into the *same* input path
(`auto_input` replaces the live `$DC` read), so one screenshot proves the WM end to
end — the ROM simulates the pad, nothing is faked.

- `mkdata.py` generates `gen_data.inc` (tiles + palette) from the shared assets
  (`kernel/font8x8.asm`, `build/*.bin` icon donors — run `make floppy144` first if
  the donors are missing).
- `build.sh` runs mkdata then `sjasmplus --raw` into a contiguous 32 KB ROM.
- `run.ps1` launches BlastEm with the SDL **software** renderer (so the
  focus-independent PrintWindow grab works over RDP — see `~/.claude/CLAUDE.md`) and
  saves `build/desktop.png`.

## Toolchain

- **sjasmplus 1.23.1** (`C:\Users\arin\z80-tools\sjasmplus-1.23.1.win`).
- **BlastEm 0.6.2** (`C:\Users\arin\genesis-tools\blastem-win32-0.6.2`) — boots
  `.sms` in Master System mode; capture needs `SDL_RENDER_DRIVER=software`.

## Why SMS (not Game Boy)

The SMS is a *true Z80*, so it reuses `gen/z80/` as-is. The Game Boy is a Sharp
**LR35902** (a Z80 relative, not a Z80) and would need rgbds + a new `gbz80`
dialect — a separate port.
