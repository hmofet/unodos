# UnoDOS/MacPlus — standalone OS for compact 68000 Macs (milestone 1)

This is the Mac port done the way the other UnoDOS ports are done: **a real
operating system**, not an application. The Mac ROM bootstraps our boot
blocks the same way a PC BIOS bootstraps the x86 reference port's boot
sector; from that point UnoDOS owns the machine — its own stack, its own
vector table and interrupt handlers, its own input drivers, its own
renderer. No System file, no Toolbox, no Mac OS on the disk at all.

(The older `mac/` tree — UnoDOS as a Toolbox application hosted on classic
Mac OS — remains as the *hosted* variant covering System 7 color machines.
This port is the OS.)

Targets: **Mac Plus, SE, Classic** (68000, 512x342x1) and the **Mac II
class** (IIcx/IIci/etc., 640x480x1). The kernel adapts to the machine at
boot — see *Machine-adaptive input* below.

| Build | Geometry | Machines |
|---|---|---|
| `./build.sh` → `unodos_macplus.dsk` | 512x342, 64 B/row | Plus, SE, Classic |
| `./build.sh mac2` → `unodos_mac2.dsk` | 640x480, 80 B/row | Mac II class (set the monitor to **B&W / 1-bit**) |

## Boot chain

1. The ROM Start Manager reads sectors 0-1 of the floppy, validates the
   `LK` signature + version `$4418` ("execute boot code") and jumps in.
2. Our boot blocks ([boot.asm](boot.asm), position-independent, 1024
   bytes) issue one `_Read` on the ROM .Sony driver (refNum -5) to pull
   the kernel image from raw sectors (offset 1024) to `$20000`, then
   `jmp` there. The ROM's A-line dispatcher + .Sony driver are the "BIOS
   services" — the only ROM facility UnoDOS uses, and only for disk I/O.
3. [kernel.asm](kernel.asm) masks interrupts, installs its own vectors,
   takes the VIA and SCC, and never returns.

## Hardware layer (everything else is the portable UnoDOS core)

| Subsystem | Implementation |
|---|---|
| Video | 1-bit linear framebuffer via low-mem ScrnBase ($824), 64 B/row. Logical colors 0-3 → white / 25% / 50% dither / black |
| Mouse | Quadrature: X1/Y1 = SCC DCD-A/DCD-B ext/status interrupts (level 2), X2/Y2 = VIA PB4/PB5, button = PB3 (active low) |
| Keyboard | M0110/M0110A over the VIA shift register: `Instant` ($14) poll per tick; response `(scan<<1)\|1`, bit 7 = key-up, `$79` keypad/arrow prefix, `$7B` null. Scan codes are translated to the canonical UnoDOS raw codes ($4C-$4F arrows etc.) |
| Tick | VIA CA1 vblank interrupt, 60.15 Hz |
| Cursor | Software arrow with save-under; erased around any main-loop pass that draws (ISRs never draw) |
| Faults | Bus/address/illegal vectors → black screen + PC dump |

Keyboard notes: the M0110 has no Escape key — **`` ` `` (backquote) is
ESC** (closes the topmost window). Arrows/keypad arrive via the `$79`
prefix page; `Clr` is reserved as F1/save for later milestones.

## Machine-adaptive input (Plus vs. SE/II)

Only the original Plus has the M0110 keyboard and quadrature mouse on the
VIA/SCC; the SE and later use ADB, and the Mac II class even relocates the
VIA. So at boot the kernel reads the ROM version word (`ROMBase+8`):

- **`$75` (Plus)** → the self-owned drivers above: we take the VIA and
  SCC outright and run our own M0110 + quadrature handlers.
- **anything newer (SE `$76`, II `$78`, …)** → *ROM-assisted mode*. We
  **chain** the ROM's level-1 interrupt handler (keeping its ADB stack
  alive) and never touch the VIA/SCC. Each tick we mirror the low-memory
  state the ROM maintains: `Ticks` ($16A) for timing, `RawMouse` ($82C)
  for the pointer, `MBState` ($172) for the button, and the OS event
  queue for keys (`SysEvtMask` opened at boot, drained with `_GetOSEvent`
  from the main loop — the Toolbox analog of polling BIOS INT 16h).

The same `unodos_macplus.dsk` therefore boots on Plus **and** SE. The
Mac II class needs the `mac2` geometry build and a B&W monitor setting.

## Building

```sh
./build.sh           # build/unodos_macplus.dsk       (Plus/SE/Classic, 512x342)
./build.sh test      # build/unodos_macplus_test.dsk  (AUTOTEST: opens both apps)
./build.sh mac2      # build/unodos_mac2.dsk          (Mac II class, 640x480)
./build.sh mac2test  # build/unodos_mac2_test.dsk     (AUTOTEST, 640x480)
```

Needs vasmm68k_mot (same toolchain as the Amiga/Genesis ports) and
Python 3. `mkdisk.py` packs boot blocks + kernel and patches the boot
block's `ioReqCount` with the real kernel size.

## Testing without an Apple ROM

Real Mac emulators (Mini vMac, MAME, Snow) need a copyrighted Apple ROM
dump, so this port ships its own ROM-free harness: [harness.py](harness.py)
wraps a Unicorn (QEMU) 68000 core and plays the ROM's part — Start
Manager, `_Read` A-line trap against the disk image, VIA/SCC/keyboard/
mouse emulation at register level, framebuffer→PNG screenshots, and
scripted input (`pip install unicorn`):

```sh
./build.sh test
python3 harness.py build/unodos_macplus_test.dsk shots < tests/m1.script
```

Verified in the harness (milestone 1): boot chain end-to-end, desktop +
icons, window raise on title/body click, title-bar drag with XOR outline,
close box, ESC close, double-click launch, arrow-key icon selection via
the real M0110A prefix protocol, Enter launch, 1-second topmost-only
refresh, software cursor save-under across all of it.

## Mini vMac validation (real Mac Plus ROM) — PASSED 2026-06-12

With a genuine Plus ROM, the whole M1 surface was exercised live in
Mini vMac 36.04: ROM Start Manager boot, .Sony kernel load, 4MB RAM
config (dynamic ScrnBase/MemTop), VIA tick, button, window raise/drag/
close, and the full keyboard path — Inquiry polling with the mode-6
"attention" pulse, `$79` prefix + Instant stash fetch, arrows, Enter,
backquote-as-ESC. Three emulator-vs-spec findings are baked in:

- Mini vMac injects host mouse motion via low-mem `RawMouse` ($82C)
  instead of quadrature pulses → the tick handler has an absolute-mouse
  fallback (inert on real hardware, where we own the SCC vectors).
- Wire bytes carry `scan<<1` with bit 0 = 0 (the "always 1" LSB in some
  docs is not universal); the decoder masks it out either way.
- Host arrows arrive as ADB/M0115-style codes $7B-$7E on the prefix
  page, not the M0110A's $42/$46/$48/$4D — the keymap maps **both**.

## Mini vMac II validation (real Mac II ROM) — PASSED 2026-06-12

The `mac2` build was exercised in a self-built Mini vMac **II** variation
(Macintosh II model) with a genuine IIcx ROM: boot, the 640x480 desktop,
window drag (RawMouse + MBState), and the full keyboard including ADB
arrows — i.e. the entire **ROM-assisted** input path the Mac SE also
uses. Because the SE shares that path, this is strong pre-hardware
coverage for the SE as well as the II class.

Building the Mini vMac II binary (no prebuilt exists): cross-compiled
from the upstream source with mingw-w64; the disk-format gate
(`NonDiskProtect`) was disabled so it mounts our raw images, and it was
built at 1-bit depth (`-depth 0`) because the II ROM otherwise starts the
video card at a color depth our 1-bit renderer doesn't match.

**Real-hardware status: pending (user testing on a Mac SE + Mac IIci via
FloppyEmu).** What the emulators could NOT exercise:
(1) the Plus-only SCC DCD quadrature mouse path (flip the `eor` sense in
`isr_lvl2` if an axis runs backwards); (2) genuine M0110A arrow codes on
a real Plus; (3) electrical timing of the Plus VIA keyboard handshake.
The SE/II ROM-assisted path has no such Plus-specific risks. On the IIci,
set the monitor to **B&W (1-bit)** until the color milestone.

## Milestones

- **M1 (this)**: boot + desktop + window manager + SysInfo/Clock,
  keyboard/mouse drivers, software cursor, fault screens. Machine-adaptive
  input (Plus M0110/SCC vs. SE/II ROM-assisted ADB) and the Mac II 640x480
  geometry variant. System 7-style window chrome (drop shadows, pinstriped
  active title bar, square close box).
- **M2 (next)**: the UnoDOS floppy filesystem (shared layout with the
  x86 port) and **disk-loaded app binaries** — the launcher reads app
  images off the floppy via the .Sony BIOS layer like the x86 launcher
  reads .BINs; Files/Notepad land here.
- **M3+**: sound (the Plus pulse-width sound buffer), Theme equivalent
  (dither schemes), Tracker, games, scheduler, Paint — Amiga parity.
