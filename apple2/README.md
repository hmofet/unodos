# UnoDOS/AppleII — standalone OS for the 6502 Apple II (milestone 1)

Like the other bare-metal UnoDOS ports, this is **a real operating
system**, not a DOS 3.3 application. The Disk II boot ROM autoloads our
own boot code; from `$4000` on, UnoDOS owns the machine — its own hi-res
renderer, its own keyboard polling, its own desktop and window manager.
There is no DOS 3.3, no ProDOS, nothing else on the disk.

## The "UnoDOS Lite" envelope

A 1 MHz 6502 with a software-only hi-res renderer is the tightest
envelope of any UnoDOS port so far, so M1 is **UnoDOS Lite**: parity
adapted to what the hardware can actually do, with every deviation
documented rather than faked.

- **Hi-res 280x192, effectively 1-bit.** 7 pixels/byte, bit 0 = leftmost,
  bit 7 = palette (kept 0). Rows are the classic Apple II interleaved
  layout (`addr = $2000 + (y&7)*$400 + ((y>>3)&7)*$80 + (y>>6)*$28 + c`).
  Every window/icon dimension is in **byte-columns** (7px steps), not
  pixels — the visible deviation vs. the bitmap-precise 68K ports.
- **Keyboard-driven, II+ floor.** No mouse at M1 (AppleMouse II is
  backlog). Nav is left/right arrow (`$08`/`$15`) + Return + ESC — the
  Apple II+'s full keyboard, no up/down/Tab assumed.
- **No timer hardware.** The Clock app runs on a **calibrated soft
  tick**: the main loop counts passes and converts to seconds with
  `TICKS_PER_SEC` (kernel.s) — there is no real clock to read.
- **No firmware sector services.** The boot ROM autoloads track 0 only;
  everything past that — head stepping, GCR 6-and-2 decode, the DOS 3.3
  skew table — is our own RWTS in [boot.s](boot.s).

## Boot chain

1. The Disk II P5A controller ROM autoloads **all 16 sectors of track 0**
   (`$0800-$17FF`, boot.s byte 0 = `$10`) and jumps to `$0801` with
   `X = slot*16`.
2. [boot.s](boot.s) (`entry:`) builds the 6-and-2 decode table at `$1900`,
   then for tracks `1..KTRACKS` steps the head and denibblizes every
   sector (DOS 3.3 `skew` interleave) into `$4000+`.
3. `jmp $4000` into [kernel.s](kernel.s), which clears hi-res page 1,
   draws the desktop, and never returns to DOS.

## M1 desktop

40 byte-columns x 24 char-rows (8px) of hi-res page 1:

| Rows | Content |
|---|---|
| 0 | Menu bar (`UnoDOS` title + separator) |
| 1-8 | **SysInfo** window — machine ID (via `$FBB3`/`$FBC0`), CPU, RAM |
| 9-13 | **Clock** window — `HH:MM:SS` from the soft tick |
| 14-18 | Empty dithered desktop |
| 19-23 | Icon grid — SysInfo (col 2), Clock (col 14) |

Window manager: open/raise/close with a z-order list and focus tracking;
ESC closes the topmost window. Icon selection (left/right arrow) is shown
by inverting the icon's label band (EOR `$7F`); Return opens or raises
the selected app's window.

## Building

```sh
./build.sh        # build/unodos_apple2.dsk       (plain boot)
./build.sh test   # build/unodos_apple2_test.dsk  (AUTOTEST: opens SysInfo + Clock)
```

Needs `dasm` (6502 cross-assembler, `DASM` env var or
`C:\Users\arin\apple2-tools\dasm.exe`) and Python 3. `mkfont.py` converts
the shared font (`amiga/mkdata.py`'s output) to the hi-res 7px convention;
`mkdsk.py` packs `boot.bin` + `kernel.bin` into a 35-track DOS-order
140 KB `.dsk`, patching boot.s's `KTRACKS` placeholder to match the
kernel's size (currently 1 track).

## Testing without real hardware

Real Apple II emulation needs Disk II nibble-level timing that's
overkill for regression screenshots, so this port ships its own
ROM-free harness: [harness.py](harness.py) is a `py65`-based 6502
emulator that plays the boot ROM's part (autoloads track 0 from the
`.dsk` image), serves Disk II nibbles for `boot.s`'s RWTS, emulates the
`$C000`/`$C010`/`$C030` keyboard/speaker soft-switches, and de-interleaves
hi-res page 1 to PNG screenshots (`pip install py65`):

```sh
./build.sh test
python3 harness.py build/unodos_apple2_test.dsk shots < tests/m1.script
```

[tests/m1.script](tests/m1.script) drives the M1 surface: boot to the
AUTOTEST desktop (SysInfo + Clock open), toggle icon selection, launch
the selected app, close it, and let the soft clock advance — 4
screenshots (`m1_boot`, `m1_select`, `m1_launch`, `m1_clock`).

Two harness calibration constants matter for reproducibility:
- `TICK_INSTRS` (5000): 6502 instruction-steps per `wait` tick, tuned so
  `wait 60` clears AUTOTEST setup with the soft clock at ~2s.
- `KEY_INSTRS` (30000): instruction-steps a `key` press gets to run
  `handle_key` to completion — sized for the heaviest M1 redraw (opening
  a window, ~26000 steps) so a `shot` right after a `key` never catches a
  redraw mid-frame.

Verified in the harness (milestone 1): boot chain end-to-end (ROM
autoload -> RWTS -> `$4000`), hi-res desktop + menu bar, SysInfo (machine
ID via `$FBB3`/`$FBC0`, CPU, RAM) and Clock (soft-tick `HH:MM:SS`) windows,
arrow-key icon-selection highlighting, Return open/raise, ESC close, and
the soft clock advancing across `wait`s.

## Real-hardware path (AppleWin -> FloppyEmu)

1. **AppleWin first.** Boot `build/unodos_apple2.dsk` in AppleWin and
   confirm the desktop renders and keyboard nav works before touching
   metal — AppleWin's cycle-honest Disk II emulation is what proves the
   RWTS.
2. **Autoload-count risk (first thing to check).** `boot.s` byte 0 = `$10`
   assumes a 16-sector P5A-ROM autoload of track 0. If AppleWin (or a
   clone ROM on real hardware) only honors a 1-sector autoload, `boot0`
   needs to replicate the ROM's read loop itself — see HANDOFF.md §3.1.
3. **FloppyEmu on real Apple II+ (or compatible) hardware**, once AppleWin
   passes.

## Milestones

- **M1 (this)**: boot chain (ROM autoload + GCR RWTS-read), hi-res
  desktop + menu bar, keyboard-driven window manager, SysInfo + Clock
  (soft tick), shared 7px font, ROM-free harness + `tests/m1.script`.
- **M2**: RWTS write path, a track/sector mini-FS, Files + Notepad,
  optional paddle/joystick pointer, speaker beeps. See
  [HANDOFF-M2.md](HANDOFF-M2.md).
- **M3**: scaled apps (Dostris, Pac-Man), Paint, Music/Tracker as
  blocking speaker playback, OutLast feasibility at 1 MHz, Theme dither
  schemes, and an honest evaluation of a cooperative scheduler on the
  6502. See [HANDOFF-M3.md](HANDOFF-M3.md).
