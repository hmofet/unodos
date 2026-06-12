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

Target: Mac Plus / SE / Classic class — 68000, 512x342x1 framebuffer,
>= 1MB RAM, M0110/M0110A keyboard.

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

## Building

```sh
./build.sh          # build/unodos_macplus.dsk      (800K bootable image)
./build.sh test     # build/unodos_macplus_test.dsk (AUTOTEST: opens both apps)
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

**Real-hardware status: not yet validated.** Two known calibration points
for first hardware/Mini vMac runs: (1) mouse quadrature polarity — if an
axis moves backwards, flip the `eor`/branch sense in `isr_lvl2`; (2) the
keyboard Instant-poll cadence (one command per tick) is well within the
M0110's spec but has only been exercised against the harness model. To
run in Mini vMac, drop a Mac Plus ROM dump at `macplus/vMac.ROM` and
point Mini vMac at `build/unodos_macplus.dsk`.

## Milestones

- **M1 (this)**: boot + desktop + window manager + SysInfo/Clock,
  keyboard/mouse drivers, software cursor, fault screens.
- **M2 (next)**: the UnoDOS floppy filesystem (shared layout with the
  x86 port) and **disk-loaded app binaries** — the launcher reads app
  images off the floppy via the .Sony BIOS layer like the x86 launcher
  reads .BINs; Files/Notepad land here.
- **M3+**: sound (the Plus pulse-width sound buffer), Theme equivalent
  (dither schemes), Tracker, games, scheduler, Paint — Amiga parity.
