# UnoDOS/MacPlus — standalone OS for compact 68000 Macs (milestone 3)

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

## Filesystem & apps (milestone 2)

Past the kernel image, the boot floppy carries a plain **FAT12 volume**
(starting at sector 256), the same on-disk layout the x86 UnoDOS port
uses — so the disk is readable on a PC (`mtools`, `tools/dump_fat12.py`).
The kernel reads and writes it through the ROM .Sony driver via the same
`_Read`/`_Write` A-traps the boot blocks use ([sony.i](sony.i)), with a
portable 68K FAT12 core ([fat12.i](fat12.i), shared with the Amiga port).

- **Files** lists the root directory; arrows select, Enter opens a file
  in Notepad, `r` refreshes.
- **Notepad** views/edits text; typing edits, arrows + Backspace navigate,
  and **Clr** (the M0110A keypad clear, raw `$50`) saves back to the
  floppy (`fat_save_file`, a real `_Write`).
## Disk-loaded apps — the kernel contains NO app code

Every windowed app except **SysInfo** and **Clock** (tiny, no filesystem use)
lives on the floppy's FAT12 volume as a separate `.APP` binary and is loaded at
runtime. The kernel image holds the window manager, 1-bit renderer, input
drivers, FAT12/`.Sony` storage, the audio sequencers, the cooperative
scheduler, and the loader — but no app UI.

- **Per-app resident slots.** Each app has a fixed 16 KB load region in free
  RAM (`$40000 + slot*0x4000`, below the stack). An app stays resident once
  loaded, so several apps can be open at once — the windowed multitasking UX is
  preserved. Files+Notepad ship as one `FILES.APP` (Files opens a file *into*
  Notepad, so they are coupled) serving procs 2 and 3 from one slot.
- **ABI** (mirrors the C64 port). An app is a raw 68K binary assembled at its
  slot org whose first four longs are a vector table: `draw / key / tick /
  click`. The window manager dispatches each window's events through the loaded
  app's vectors ([diskapp.i](diskapp.i): `app_disp_draw/key/tick/click` →
  `app_ensure` loads the slot on demand → `jsr (vector)`).
- **Linked by address.** Apps reference kernel routines and variables by
  *absolute address* via the generated [`build/kernel_api.inc`](build) — there
  is no `a5` service table. [mkapi.py](mkapi.py) extracts the addresses from the
  kernel's vasm symbol listing; [mkapp.py](mkapp.py) rewrites each app's
  kernel-symbol `(pc)` refs to absolute and promotes kernel `bsr/bra` to
  `jsr/jmp` (a PC-relative branch can't reach the kernel from a slot). Static
  equates (memory map, window record, key codes) come from
  [sysequ.i](sysequ.i). The build runs kernel → API table → each `.APP` →
  FAT12 pack in one pass, so apps never link against a stale table.
- **Apps:** `FILES.APP` (Files+Notepad), `DOSTRIS.APP`, `PACMAN.APP`,
  `OUTLAST.APP`, `PAINT.APP`, `MUSIC.APP`, `TRACKER.APP`, `THEME.APP`, and a
  sample `DEMO.APP`. The audio sequencers (game music, Music, Tracker playback)
  stay kernel-side ([appsvc.i](appsvc.i)) so a song keeps playing while its
  window is not topmost; the app holds only the draw/key UI.

## Hardware layer (everything else is the portable UnoDOS core)

| Subsystem | Implementation |
|---|---|
| Video | 1-bit linear framebuffer via low-mem ScrnBase ($824), 64 B/row. Logical colors 0-3 → white / 25% / 50% dither / black |
| Mouse | Quadrature: X1/Y1 = SCC DCD-A/DCD-B ext/status interrupts (level 2), X2/Y2 = VIA PB4/PB5, button = PB3 (active low) |
| Keyboard | M0110/M0110A over the VIA shift register: `Instant` ($14) poll per tick; response `(scan<<1)\|1`, bit 7 = key-up, `$79` keypad/arrow prefix, `$7B` null. Scan codes are translated to the canonical UnoDOS raw codes ($4C-$4F arrows etc.) |
| Tick | VIA CA1 vblank interrupt, 60.15 Hz |
| Cursor | Software arrow with save-under; erased around any main-loop pass that draws (ISRs never draw) |
| Faults | Bus/address/illegal vectors → black screen + full crash dump (PC, faulting access address, SSW, opcode IR, and all D0-D7/A0-A7) |

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
  state the ROM maintains: `Ticks` ($16A) for timing, `MBState` ($172) for
  the button, **`MTemp` ($828)** for the pointer, and the OS event queue
  for keys (`SysEvtMask` opened at boot, drained with `_GetOSEvent` from the
  main loop — the Toolbox analog of polling BIOS INT 16h).

  The mouse source is `MTemp`, **not** `RawMouse` ($82C): the ADB mouse
  handler accumulates deltas into `MTemp` and leaves it to the ROM's VBL
  cursor task to copy `MTemp → RawMouse` and clamp it — but we disable that
  task (we paint our own cursor), so we read `MTemp` directly and clamp it
  ourselves, writing the clamp back so deltas don't accumulate unbounded.

Because we boot without a System file, ROM-assisted mode must also stand
up the low-memory state the System normally would before any of those ROM
services are safe to call:

- **OS Event Manager** — the ROM's `_GetOSEvent` / `PostEvent` walk
  `EventQueue` ($14A), which is uninitialised at our boot (`qHead` is
  garbage). We hand the ROM a real event pool: `SysEvtBuf` ($146) points at
  a 20-record buffer (22 bytes each, `EvtBufCnt` at $154), every record
  marked free (`evtQWhat` = $FFFF), and the queue cleared to empty. Without
  this, the very first `_GetOSEvent` dereferences the garbage head and the
  ROM address-errors (the real-hardware fault below).
- **ADB restart** — autopoll self-chains through each transaction's
  completion interrupt, and our long interrupt-masked boot can break that
  chain, leaving ADB silent. We call `_ADBReInit` ($A07B) once interrupts
  are live to reset the bus, re-register devices and restart autopoll.
- **ROM VBL hardening** — the chained handler's per-tick work assumes more
  init than we do, so we pre-empt it: the cursor task is disabled
  (`CrsrCouple`/`CrsrNew` at $8CF/$8CE = 0; we paint our own cursor) and the
  stack-into-heap sniffer is satisfied by pointing `ApplZone` ($2AA) at a
  zeroed fake zone (we never use the ROM Memory Manager).

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
block's `ioReqCount` with the real kernel size; `mkfs.py` then writes the
FAT12 volume (the `disk/*.TXT` content plus every assembled `*.APP`).

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
./build.sh            # the plain image carries the FAT12 volume + every *.APP
python3 harness.py build/unodos_macplus.dsk shots < tests/m2.script
python3 harness.py build/unodos_macplus.dsk shots < tests/disk_full.script  # all disk apps
```

The harness plays the .Sony driver for **both** `_Read` and `_Write`
against the disk image, so the whole filesystem path is exercised ROM-free.

Verified in the harness (milestone 1): boot chain end-to-end, desktop +
icons, window raise on title/body click, title-bar drag with XOR outline,
close box, ESC close, double-click launch, arrow-key icon selection via
the real M0110A prefix protocol, Enter launch, 1-second topmost-only
refresh, software cursor save-under across all of it.

Verified in the harness (milestone 2, `tests/m2.script`): mount the FAT12
volume, list the root directory, open a file in Notepad, edit it, save it
back to the floppy (`_Write`), close + reopen and confirm the edit
persisted, then load `DEMO.APP` off the floppy and drive its key handler.

Verified in the harness (disk-loaded apps, `tests/disk_full.script`): each
app loads from its FAT12 `.APP` into its resident slot and renders — Files
lists the directory (including the `.APP`s themselves), Notepad opens a file
from Files, Dostris/Pac-Man/OutLast play, Music/Tracker show their UI, Theme
applies a dither scheme to the whole desktop, Paint draws. Multiple apps stay
resident at once (`tests/residency.script`).

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

## Real-hardware validation (Mac SE + FloppyEmu) — PASSED 2026-06-13

Validated on a real Mac SE booting `unodos_macplus.dsk` from a FloppyEmu:
boot chain, desktop, and the full ROM-assisted ADB input path — moving the
mouse, clicking and double-clicking icons, and keyboard navigation — all
live on hardware. Getting there took fixing several issues the emulators
could never surface, since they boot through a far more initialised
low-memory environment than a bare FloppyEmu boot.

- **Finding #1 — Sad Mac `0F/00000001` at boot.** The boot block and
  `sony.i` hardcoded `ioVRefNum = 1`, but FloppyEmu on the SE's external
  port enumerates as drive 2/3, so the read hit the empty internal drive.
  Fixed by honoring low-mem `BootDrive` ($210) in both layers; the read-fail
  paths now raise distinctive Sad Mac minors ($42 = kernel read failed,
  $43 = `UDM1` magic missing) instead of the ambiguous `1`.
- **Finding #2 — `FAULT @ 00403F72 / ACCESS 00000005`.** The first
  `_GetOSEvent` walked an uninitialised `EventQueue` (`qHead = $FFFFFFFF`);
  `$FFFFFFFF + 6` = `$00000005` (odd) → address error inside the ROM.
  Fixed by initialising the OS Event Manager at boot (see *Machine-adaptive
  input* above). Root-caused by disassembling the SE ROM at the fault PC and
  confirmed in Unicorn: with `qHead = 0` the ROM takes the empty-queue
  branch and never reaches the faulting load. The black-screen crash dump
  (PC / access / SSW / opcode / registers) is what made it diagnosable from
  hardware alone.
- **Finding #3 — ADB silent.** With the desktop up, neither mouse nor
  keyboard responded. SE ADB autopoll self-chains through each transaction's
  completion interrupt; our long interrupt-masked boot breaks the chain.
  Fixed by calling `_ADBReInit` ($A07B) after interrupts are enabled.
- **Finding #4 — mouse dead while keyboard worked.** An on-screen state
  overlay showed our handler running and the keyboard posting events, but
  `RawMouse` frozen. The SE ADB mouse handler accumulates deltas into
  `MTemp` ($828) and relies on the VBL cursor task (which we disable) to
  forward them to `RawMouse`. Fixed by reading `MTemp` directly with our own
  clamp/write-back (see *Machine-adaptive input*). Also tightened keyboard
  latency: the idle loop now drains the ROM `EventQueue` ($14C) immediately
  instead of waiting for the once-per-second refresh.

Still untested on real hardware (no Plus-specific risk on the SE/II
ROM-assisted path): (1) the Plus-only SCC DCD quadrature mouse — flip the
`eor` sense in `isr_lvl2` if an axis runs backwards; (2) genuine M0110A
arrow codes on a real Plus; (3) electrical timing of the Plus VIA keyboard
handshake. On a real IIci, set the monitor to **B&W (1-bit)** — this port
renders 1-bit on every supported machine.

## Milestones

- **M1 (this)**: boot + desktop + window manager + SysInfo/Clock,
  keyboard/mouse drivers, software cursor, fault screens. Machine-adaptive
  input (Plus M0110/SCC vs. SE/II ROM-assisted ADB) and the Mac II 640x480
  geometry variant. System 7-style window chrome (drop shadows, pinstriped
  active title bar, square close box).
- **M2**: the UnoDOS floppy filesystem (FAT12, shared layout with
  the x86 port) read **and written** through the .Sony BIOS layer; the
  **Files** and **Notepad** apps; and **disk-loaded app binaries** — the
  launcher reads an app image off the floppy into `$40000` and runs it via
  a position-independent ksys-table ABI, like the x86 launcher reads .BINs.
- **M3 (this) — full app parity** with the Amiga/Genesis ports:
  - **Sound** ([snd.i](snd.i)): the classic Mac pulse-width buffer (370
    words at MemTop−$300, high byte per word = sample at 22.257 kHz;
    the low bytes are the .Sony disk PWM and are never touched). Square
    synth from the shared Paula note periods; machine-gated — Plus gets
    full VIA control (PB7 enable + volume), the SE is buffer-only (its
    VIA belongs to the ROM ADB stack), the Mac II class is silent (ASC).
  - **Games**: Dostris, OutLast, Pac-Man — verbatim logic/tables, 1-bit
    rendering schemes (dither-density ghost identities, white road on
    dithered grass, hollow frightened shells).
  - **Paint**: the MacPaint-style editor with the platform's true gamut —
    the four dither inks; `PAINT.UNO` round-trips byte-exact to the floppy.
  - **Music** (square sequencer + staff view) and **Tracker** (shared
    byte-identical pattern format, leftmost-voice playback like the
    x86 PC-speaker port, `SONG.UNO` on the floppy).
  - **Theme**: 1-bit dither schemes through the mutable `pat_tab` —
    six presets including a full video invert, applied live.
  - **Cooperative scheduler** ([scheduler.i](scheduler.i)): every window
    runs its app in a private-2KB-stack task with one-slot mailboxes;
    keys post with a bounded yield-retry, game physics run as task ticks.
- **Next**: the 8bpp color milestone for the Mac II class (IIci).
