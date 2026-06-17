# UnoDOS / Sega Master System (Z80)

The **first UnoDOS port built fresh on the contract-driven + greenfield-window
architecture** (CONTRACT-ARCH §13). The SMS has a true **Z80**, so it consumes the
Contract's existing `gen/z80/` world (sjasmplus) directly — no new CPU dialect, no
new assembler emitter. Screen geometry and the window/event layout come from
`[world.sms]` in `unodef/unodef.toml` via unogen (`gen/sms/sys_gen.inc`), so they
can never drift from the Contract.

## Status — milestone 1 (boot to desktop) ✅

Emulator-verified in BlastEm (`build/desktop.png`): Z80 + VDP bring-up from
scratch, the shared 8×8 font + the x86 icon donors uploaded as VDP tiles, and a
rendered desktop — the inverted **"UnoDOS 3"** title bar and the labelled icon grid
(SysInfo · Clock · Notepad · Music · Files · Theme · Tracker · Dostris · OutLast ·
Pac-Man · Paint).

Next, mirroring the Genesis/SNES progression: M2 = window manager + d-pad cursor +
events; M3 = the app sweep.

## Hardware brought up (from scratch)

| Part | Detail |
|---|---|
| CPU | Z80 @ 3.58 MHz; `im 1`, `jp boot` over the RST/INT/NMI vectors |
| VDP | Sega 315-5124 **Mode 4**, 256×192 = 32×24 cells; data port `$BE`, control/status `$BF` |
| Tiles | 4bpp **planar** (4 bytes/row, one per bitplane), 32 bytes/tile, uploaded to VRAM `$0000` |
| Name table | `$3800`, 32×28 word entries (tile index + flags), cleared to the desktop tile |
| CRAM | 16-entry background palette (`%00BBGGRR`) from the four UnoDOS theme colours; duplicated into the sprite palette so the border is desktop-blue |
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
sh sms/build.sh                         # -> sms/build/unodos.sms (32 KB)
powershell -ExecutionPolicy Bypass -File sms/run.ps1   # BlastEm + screenshot
```

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
