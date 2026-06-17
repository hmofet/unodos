# UnoDOS / NEC PC Engine — TurboGrafx-16 (HuC6280 + HuC6270 VDC)

The **sixth** fresh contract-driven port (created sixth, before VIC-20 and
WonderSwan; it was the last of the round to be verified) and the **first on the
HuC6280** (a 65C02
superset; `ca65 --cpu huc6280`). The PC Engine screen is 256×224 = **32×28 BAT
cells** — like the NES nametable — so this reuses the NES's 4-column icon-grid
launcher and the shared 6502 app/Dostris logic, swapping the draw layer to the VDC.
MINIMAL profile (CONTRACT-ARCH §9): one full-screen app at a time, directional nav.

The HuC6270 VDC is a tile engine driven through an address/data port pair: write a
register number to `$2000`, then its data to `$2002/$2003`. Tiles are 8×8 **4bpp
planar** (32 bytes); the 32×32 BAT holds 16-bit entries `(palette<<12) | CG`, and a
tile's pattern lives at VRAM word `CG<<4`. This port uploads tiles to VRAM `$1000`
(CG base `$100`), so a BAT entry for tile N is `$0100 + N`. Colour is a 16-entry VCE
palette (9-bit `GGGBBBRRR`), and the Theme app recolours everything by rewriting it.
The HuC6280 **MMU** maps the 16-bit logical space through 8 MPR bank registers:
MPR0=`$F8` (8 KB RAM), MPR1=`$FF` (I/O at `$2000`), MPR2-6 = ROM banks 1-5
(`$4000`+), MPR7=`$00` (boot bank at `$E000` + the reset vector).

## Status — M1 · M2 · M3 all shipped ✅

Verified headlessly on a **ROM-free HuC6280 harness** (`pce/harness.py`): a py65
65C02 core extended with the few HuC6280 opcodes the kernel uses (`TAM` bank-mapping,
`CSH`/`CSL`) plus the MMU, a model of the VDC write path (MAWR/VWR + the status
vblank bit) and the VCE colour table, decoding the BAT to a PNG. This is the same
ROM-free-core approach the C64/Apple II/VIC-20 ports use (py65) and the GBA/
WonderSwan ports use (Unicorn) — chosen because Mesen renders the PCE through a GPU
surface that a GDI/PrintWindow grab reads as black and its F12 capture is focus-flaky
over RDP. The AUTOTEST ROMs drive the pad through the same input path; nothing about
the picture is faked (`build/*.png`):

- **M1 — launcher** (`build/desktop.png`): VDC bring-up, a 16-colour VCE palette, the
  inverted "UnoDOS 3" title bar, a 4-column grid of 16×16 icons.
- **M2 — navigation** (`build/nav.png`): the joypad on `$3000`, a VDC-vblank-polled
  frame loop, a directional selection highlight (the selected label inverts),
  **I** launches the app full-screen / **II** returns.
- **M3 — apps** (`build/{app,clock,theme,music,dostris}.png`): SysInfo, live Clock
  (HH:MM:SS off the frame counter), Notepad, Files, Theme (cycles the VCE palette —
  blue/green/red/grey desktops), Music (the PC Engine PSG), and **Dostris** — the
  falling-blocks game in colour. Tracker / OutLast / Pac-Man / Paint open framed
  placeholders.

## Hardware brought up (from scratch)

| Part | Detail |
|---|---|
| CPU | **HuC6280** (65C02 superset) @ 7.16 MHz; 8 MPR MMU banks |
| Video | **HuC6270 VDC**: 32×32 BAT, 8×8 4bpp tiles, 256×224; ports `$2000-$2003` |
| Colour | **HuC6260 VCE**: 16-colour palette, 9-bit `GGGBBBRRR`; ports `$2402/$2404` |
| Input | joypad `$3000` (2-read d-pad + buttons protocol) |
| Timing | the VDC status vblank bit (`$2000` read) |
| Audio | PC Engine PSG `$2800-$2806` |
| RAM | 8 KB internal (bank `$F8`); zero page + stack + work + `g_board` at `$0400` |
| Boot | bank 0 at `$E000` (the only bank mapped at reset) sets the MPRs, then jumps to the main bank; reset vector `$FFFE` |

## Build & run

```sh
sh pce/build.sh                                      # -> pce/build/unodos.pce (HuCard ROM)
sh pce/build.sh nav|app|clock|theme|music|dostris    # AUTOTEST builds
python pce/harness.py pce/build/unodos.pce pce/build/desktop.png
```

- `mkdata.py` bakes the tile set (font + inverted font + a white solid + 11 2×2
  icons + Dostris block solids, all 4bpp planar), the piece tables, the PSG tune, and
  the VCE theme palettes into `pce_data.inc`; the kernel uploads the tiles to VRAM at
  boot.
- `build.sh` runs mkdata, assembles `kernel.s` with **ca65 `--cpu huc6280`** (AUTOTEST
  via `-DAUTOTEST -DAT_*`), and links a HuCard `.pce` with **ld65** (`pce.cfg`).
- Source split: `kernel.s` (boot/MMU, VDC/VCE, input, nav, render dispatch, the BAT
  helpers), `apps.inc` (apps, clock/theme/music, AUTOTEST, strings), `dostris.inc`
  (the game). `pce_data.inc` is generated.
- `run.ps1` is the Mesen2 capture attempt (kept for local/interactive sessions); the
  harness is the RDP-safe verified path.

## Toolchain
- **ca65 / ld65** (`C:\Users\arin\snes-tools\bin`) — shared with the SNES port.
- **py65** (Python) — the 65C02 core the harness extends with the HuC6280 opcodes.

A real PC Engine (HuCard / flash cart) and audio-by-ear remain the tail; the harness
models the VDC/VCE/MMU and the reset path faithfully, but PSG output is judged on
hardware.
