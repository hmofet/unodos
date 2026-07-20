# UnoDOS / NEC PC Engine — TurboGrafx-16 (HuC6280 + HuC6270 VDC)

The **sixth** fresh contract-driven port (created sixth, before VIC-20 and
WonderSwan; it was the last of the round to be verified) and the **first on the
HuC6280** (a 65C02
superset; `ca65 --cpu huc6280`). The PC Engine screen is 256×224 = **32×28 BAT
cells** — like the NES nametable — so this reuses the NES's 4-column icon-grid
launcher and the shared 6502 app/Dostris logic, swapping the draw layer to the VDC.
MINIMAL profile (CONTRACT-ARCH §9): one full-screen app at a time, directional nav.

The HuC6270 VDC is a tile engine driven through an address/data port pair: write a
register number to `$0000`, then its data to `$0002/$0003`. Tiles are 8×8 **4bpp
planar** (32 bytes); the 32×32 BAT holds 16-bit entries `(palette<<12) | CG`, and a
tile's pattern lives at VRAM word `CG<<4`. This port uploads tiles to VRAM `$1000`
(CG base `$100`), so a BAT entry for tile N is `$0100 + N`. Colour is a 16-entry VCE
palette (9-bit `GGGRRRBBB`), and the Theme app recolours everything by rewriting it.

The HuC6280 **MMU** maps the 16-bit logical space through 8 MPR bank registers. The
critical constraint: the HuC6280 **hard-maps the zero page to logical `$2000` and the
stack to `$2100`**, so the 8 KB work RAM (bank `$F8`) MUST sit at **MPR1 (`$2000`)** and
the hardware I/O page (bank `$FF`) at **MPR0 (`$0000`)** — VDC `$0000`, VCE `$0400`, PSG
`$0800`, joypad `$1000`. MPR2-6 = ROM banks 1-5 (`$4000`+), MPR7=`$00` (boot bank at
`$E000` + the reset vector). Because the VDC registers (`$0000-$0003`) are below `$100`,
every VDC access uses the **`a:` prefix** (`sta a:VDC_AR`) to force absolute addressing —
otherwise ca65 folds the store to zero page, which the HuC6280 redirects to `$2000`
(RAM), and the write silently never reaches the VDC. (Getting this map backwards — RAM at
`$0000`, I/O at `$2000` — is what black-screened the port on real hardware; see below.)

## Status — M1 · M2 · M3 shipped, real-hardware validated ✅

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
- **M2 — navigation** (`build/nav.png`): the joypad on `$1000`, a VDC-vblank-polled
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
| Video | **HuC6270 VDC**: 32×32 BAT, 8×8 4bpp tiles, 256×224; ports `$0000-$0003` (accessed `a:`-absolute) |
| Colour | **HuC6260 VCE**: 16-colour palette, 9-bit `GGGRRRBBB`; ports `$0402/$0404` |
| Input | joypad `$1000` (2-read d-pad + buttons protocol) |
| Timing | the VDC status vblank bit (`$0000` read, needs CR bit 3 set) |
| Audio | PC Engine PSG `$0800-$0806` |
| RAM | 8 KB internal (bank `$F8`) at **MPR1 `$2000`**; zero page `$2000` + stack `$2100` + work + `g_board` at `$2400` |
| Boot | bank 0 at `$E000` (the only bank mapped at reset) disables IRQs, sets the MPRs (I/O→MPR0, RAM→MPR1), clears work RAM, then jumps to the main bank; reset vector `$FFFE` |

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

## Real hardware — validated on a Turbo EverDrive v2.5 ✅

The port **boots on a real PC Engine** via a Krikzz **Turbo EverDrive v2.5**, with
**controller input and PSG sound working** — the full milestone, judged on hardware, not
just the harness.

Getting there surfaced two bugs the harness could never catch, because py65 keeps the
zero page and stack at `$0000/$0100` while the real HuC6280 hard-maps them to
`$2000/$2100` (the relocation lives in the CPU's addressing-mode logic, not the memory
bus, so no memory model can mimic it):

1. **RAM/I/O map was swapped** — RAM at MPR0 (`$0000`), I/O at MPR1 (`$2000`). Every
   `jsr`/`rts` and zero-page access hit I/O → instant black screen. Fixed: I/O→MPR0,
   RAM→MPR1 (see the MMU note above).
2. **VDC writes folded to zero page** — `$0000-$0003` is below `$100`, so ca65 emitted
   zero-page stores that the HuC6280 sent to `$2000` (RAM) instead of the VDC. Fixed with
   the `a:` absolute prefix on every VDC access.

(Both are verified in **Mednafen** — the gold-standard PCE emulator — via savestate +
F9 framebuffer snaps, since Mesen's GPU surface reads black under RDP. Tooling lives
outside the repo at `C:\Users\arin\mednafen-dl`.)

### Running it on a TED v2 — use TEOS, not the stock Krikzz OS

The stock Krikzz TED v2 OS rejects this (homebrew) ROM with **"Error 32"** during
"Game loading…". This is **not** a ROM, firmware, size, or format problem — our
`TBED/OS.PCE` is byte-identical to the official `turbo-os-v2.zip`, and 48 KB / 128 KB /
fresh-FAT32 all fail under it. The fix is to run **TEOS** (the open-source replacement OS
for the TED v2), which has a more robust loader/SD-FAT stack and runs the exact same
ROM. To build a card:

1. Format a card **FAT32**.
2. Copy the **TEOS** `TBED/` folder to the root (so `os.pce` is TEOS, not the stock
   Krikzz OS).
3. Copy **`build/unodos.pce`** (the 48 KB ROM — no padding needed) to the root.

Making the ROM boot under the *stock* Krikzz OS as well is an open, optional item (cause
still unknown — both ROM sizes failed, so it is not simple padding).
