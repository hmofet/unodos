# UnoDOS 3 (x86) — run on real hardware

The x86 port is the first full UnoDOS port on the contract-driven architecture:
every constant, struct, FAT12 geometry, and now the **window-manager addressing**
is generated from the single Contract (`unodef/`), and the build is byte-identical
and **QEMU-verified** (boots to the desktop; window manager opens/closes windows
and launches apps). This is the image to test on real hardware.

## The image
- **`build/unodos-144.img`** — a raw 1.44 MB bootable floppy image (FAT12, with the
  desktop, window manager, and all bundled apps).
- Build it yourself: `make floppy144` (needs `nasm` on PATH; the repo's is at
  `~/AppData/Local/bin/NASM`).

## Requirements
- A **386 or later** PC (the kernel runs in 16-bit real mode, 320×200 VGA mode 13h).
- Mouse: **PS/2** or **Microsoft serial (COM1)** — both supported.
- A way to boot a floppy image (see below).

## Booting it

### A. Real floppy + a PC with a floppy drive
Write the raw image to a physical 1.44 MB diskette, then boot the PC from drive A:.
- Windows: **Rufus** (Image → DD mode) or **Win32DiskImager**, target the floppy.
- Linux/macOS: `dd if=build/unodos-144.img of=/dev/fdX bs=512` (use your floppy device).

### B. Floppy emulator (recommended for vintage PCs)
A **Gotek** (FlashFloppy) or similar: copy `unodos-144.img` to the USB stick, select
it, boot the PC from A:. This is the most reliable real-hardware path today.

### C. Bootable USB on a modern PC
Many BIOSes can boot a 1.44 MB image via **floppy emulation**. Tools like Rufus
(select the .img, "DD"/"floppy" mode) can write it; in the BIOS pick the USB as a
floppy/removable device. (UEFI-only machines without CSM won't boot a real-mode
floppy — use a machine with legacy/CSM boot, or option D.)

### D. QEMU (to reproduce the verification, no hardware needed)
```
qemu-system-i386 -drive file=build/unodos-144.img,format=raw,if=floppy -boot a
```
(On this machine QEMU is at `C:\Program Files\qemu\qemu-system-i386.exe`.)
Headless screenshot harness: `python tools/qemu_test.py build/unodos-144.img <outdir> 0`
then feed it `wait 6 / shot name / quit` on stdin.

## What you should see
The "UnoDOS 3" desktop with an icon grid (3D Runner, Sys Info, Tracker, Paint,
Clock, Files, Music, Settings, Dostris, Tetris, Notepad, OutLast, Pac-Man, …),
a mouse cursor, and a version/build line. Double-click an icon to open its window;
the titlebar **X** or the **OK** button closes it.

## What "on the new architecture" means here
- All kernel constants/structs/FAT12 geometry come from `unodef/` via `unogen`
  (the trust anchor asserts they match; rebuild is byte-identical).
- The window-manager **index→address arithmetic** is generated from the greenfield
  window model (`unodef/wmgen.py`, `[wmodel.platform.x86nasm]` → the `win_entry_addr`
  macro) — the same model that drives the other five windowing ports. Wiring it was
  proven byte-identical (`e433e02b…`), so this image is the known-good behavior,
  now generated from the single source.
- The window-entry **layout** is now the **UnoDOS 3.1 clean layout**: the Contract's
  `[struct] win_entry` is the compact **16 B** entry (down from 32 B) — `owner` joins
  the hot prefix, the **title is a pointer** into a kernel title pool (no more 12-byte
  inline buffer or 11-char limit), and `content_scale`/`flags` move to the tail.
  z-order stays an in-entry rank byte (a valid per-platform realization of the z
  relation). `kernel.asm` needed **no code edits** — it consumes the offsets and the
  `win_entry_addr` stride macro symbolically, so regenerating the Contract flipped the
  whole layout. This is behavior-changing vs the legacy 32 B entry, and is **QEMU-
  verified** (boot, open/close/reopen windows, drag, z-order, three distinct titles).
  This is the first port shipping the 3.1 clean window layout — the very thing to
  exercise on real hardware. (`file_handle`/directory entries stay 32 B; only the
  window entry changed.)
