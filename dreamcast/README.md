# UnoDOS/Dreamcast — Sega Dreamcast port (milestone 1, M2 storage)

UnoDOS/Dreamcast is a KallistiOS program that boots straight into the UnoDOS
desktop. It follows the same strategy as the [PS2 port](../ps2/README.md)
([../docs/PORTS-PLAN.md](../docs/PORTS-PLAN.md) §4): **port the C core** in
[../mac/unodos.c](../mac/unodos.c) — the complete UnoDOS (11 apps, window
manager, focus-routed event model, cooperative scheduler, device-abstracted
FAT12) — by swapping the platform layer, not rewriting.

**Status: M1 host-verified; the DC target is written but UNVERIFIED on a
Dreamcast.** The whole desktop / window manager / all 11 apps render through the
**host shim** at the Dreamcast's native **640×480** (`build.sh desktop`,
`shots/m1_*.png`). The port is [../mac/unodos.c](../mac/unodos.c) copied to
[unodos.c](unodos.c) over the same **Mac-compat shim**
([mac_compat.h](mac_compat.h)/[mac_compat.c](mac_compat.c) + [mac_io.c](mac_io.c))
the PS2 port uses, re-implementing the ~40 Toolbox calls the core needs over the
software framebuffer [fb.*](fb.h). The Dreamcast-specific layer is one file,
[dc_main.c](dc_main.c) (KallistiOS video present + maple input); **M2 storage**
persists Files/Notepad/Tracker/Paint to the **VMU** via the KOS VFS, and M3
Theme (32-bit colour) comes along through the shim. Remaining: AICA audio, and a
real-hardware / emulator run — see [Next](#next).

## Platform design: software framebuffer, the DC framebuffer as the blitter

All UnoDOS drawing happens in software against a **640×480×32 framebuffer** in
main RAM (~1.2 MB of 16 MB), via the plain-C primitives in [fb.c](fb.c)/[fb.h](fb.h)
(`fb_fill_rect`/`fb_frame_rect`/`fb_invert_rect`/`fb_text`/`fb_big_text` + the
4-colour PORT-SPEC palette). Each vblank [dc_main.c](dc_main.c) converts the
buffer to **RGB565** and copies it into the Dreamcast framebuffer (`vram_s`),
then overlays the arrow cursor — the PowerVR2 is left idle.

Why not use the PVR's 3D pipeline? `unodos.c` draws *incrementally* (event-driven
partial repaints, XOR drag outlines, invert highlights). A software FB preserves
those semantics exactly (`uno_invert` is a real XOR), and the per-vblank copy is
the simplest, most portable KOS present path — the same "GPU/FB as a blitter"
decision the PS2 port made for the GS. (A PVR-textured-quad present is a possible
later optimisation; it changes nothing above [fb.c](fb.c).)

This is also why the Dreamcast runs at full **640×480** while the PS2 port uses
640×448: the core derives all geometry from `gScreen = qd.screenBits.bounds`,
seeded from `FB_W × FB_H` at init, so [fb.h](fb.h) is the only file that names
the resolution. No letterboxing — the desktop fills the screen.

## Input (maple bus)

[dc_main.c](dc_main.c) reads the maple peripherals each frame and posts into the
shim's event queue, so the core's normal `GetNextEvent`/`GetMouse`/`StillDown`
loop consumes them unchanged:

- **Controller** — d-pad → arrow keys (desktop icon nav / in-app movement),
  A/B → Return (launch/confirm), Start → Esc (close window). The **left analog
  stick** moves the pointer and the **right trigger** clicks it.
- **Dreamcast mouse** (if connected) — relative motion drives the pointer,
  left button → `mouseDown`/`mouseUp` edges (clicks + title-bar drags).
- **Dreamcast keyboard** (if connected) — typed characters into Notepad, plus
  Return / Esc / Backspace / Tab, drained from the KOS cooked key queue
  (`kbd_queue_pop`, xlat). *Note:* arrow-key navigation routes through the
  controller d-pad in M1; routing the keyboard's own arrows is a small follow-up.

The arrow **cursor** is drawn as an overlay into the RGB565 framebuffer *after*
the fb copy, so it never touches the software framebuffer (keeping the core's
XOR/incremental drawing pristine — the same rule the PS2 port follows by drawing
its cursor as a GS overlay).

## Storage (VMU, M2)

[mac_io.c](mac_io.c)'s DC backend persists files to the **VMU** in port A slot 1
(`/vmu/a1`) through the KOS POSIX VFS. UnoDOS only ever Creates then writes a
whole file in one `FSWrite`, and opens then reads a whole file in one `FSRead`
(no mid-file seeks — the only seeking path is the `.Sony` Mac floppy, which we
deliberately fail so the RAM FAT12 volume is the working path), so each handle
owns a **flush-on-close RAM buffer**: writes accumulate in RAM and hit the card
once on `FSClose`; reads slurp the file at `FSOpen`. This is the VMU-safe shape —
it sidesteps the VMU VFS's lack of an update (`r+b`) mode and its block
granularity. `opendir`/`readdir` over `/vmu/a1` powers the Files listing. So
Files/Notepad/Tracker/Paint persist across power cycles on real hardware.

The HOST build of [mac_io.c](mac_io.c) keeps the PS2 port's stdio-over-`uno_disk/`
backend **byte-identical**, so the PC inner loop round-trips files exactly the
same way.

## Audio (M3, stubbed)

The Sound Manager shim ([mac_io.c](mac_io.c)) is a square-wave channel model that
links and runs but is currently silent; wiring it to the **AICA** via KOS's sound
API (`snd_stream` / a square-wave generator) is the M3 task. Music and Tracker
already link and run.

## Toolchain status on this dev machine

**No Dreamcast toolchain or emulator is installed here** (same starting point the
PS2 port had before ps2dev was unpacked). The host shim — the family's fastest
inner loop — needs only WSL `gcc` + `python3`, both present, and is fully
verified. The DC ELF needs **KallistiOS** (`sh-elf-gcc` + `libkos`); building it
is a one-time source build of the cross-compiler (see below). [dc_main.c](dc_main.c)
is therefore written and reviewed but **not yet compiled or run** — exactly the
state the PS2 `main.c`/`ee_platform.c` shipped in before the toolchain arrived.
Everything it shares with the host shim ([fb.c](fb.c), [mac_compat.c](mac_compat.c),
[mac_io.c](mac_io.c), [unodos.c](unodos.c)) **is** verified on the PC.

## Building

```sh
# --- M0: the splash (reference) --------------------------------------------
./build.sh host            # software-FB splash -> shots/m0_splash.png  (VERIFIED)

# --- M1: the full desktop (host shim, VERIFIED) ----------------------------
# The whole UnoDOS over the Mac-compat shim, via WSL gcc, rendered at 640x480.
# FEATURE bakes in a UNO_AUTOTEST_* app so the screenshot is self-driving
# (PACMAN/PAINT/THEME/DOSTRIS/TRACKER/FILES/OUTLAST/FAT12, or "stack"; empty =
# the bare desktop).
./build.sh desktop                 # -> shots/m1_desktop.png
./build.sh desktop PACMAN          # -> shots/m1_pacman.png

# --- the Dreamcast ELF (needs KallistiOS; UNVERIFIED here) ------------------
./build.sh dc                      # -> build/unodos-dc.elf   (interactive desktop)
./build.sh dc PACMAN               # self-driving screenshot variant
./build.sh cdi                     # + build/unodos-dc.cdi    (bootable CD image)
```

`build.sh` first runs `../amiga/mkdata.py` + [mkfont_c.py](mkfont_c.py) to emit
`build/font_data.h` (the shared 8×8 font as a C array — the same font every other
port consumes). [tools/ppm2png.py](tools/ppm2png.py) converts the host shim's PPM
dump to PNG with only the stdlib.

### Installing the toolchain (KallistiOS)

```sh
git clone https://github.com/KallistiOS/KallistiOS.git
cd KallistiOS/utils/kos-chain
cp Makefile.dreamcast.cfg Makefile.cfg
make                       # builds sh-elf binutils + gcc + newlib (long, one-time)
cd ../..                   # back to KOS_BASE
cp doc/environ.sh.sample environ.sh    # edit if your prefix differs
source environ.sh          # sets KOS_BASE, kos-cc on PATH, KOS_LIBS
make                       # build libkos
```

Then `source <KOS>/environ.sh` and run `./build.sh dc`. `build.sh dc` also tries
to source `environ.sh` from `/opt/toolchains/dc/kos`, `~/KallistiOS`, or `~/dc/kos`
if `$KOS_BASE` is unset.

### Running it (once built)

Boot `build/unodos-dc.cdi` in an emulator — **Flycast**, **lxdream**, or
**redream** — or burn it to CD-R for real hardware. `make run` uses `$KOS_LOADER`
(dc-tool over the coder's-cable / BBA) if you have one.

## Files

| File | Role | Shared with |
|---|---|---|
| [unodos.c](unodos.c) | the portable UnoDOS core (+ `#ifdef UNO_DC` hooks) | copied from `mac/unodos.c` (= ps2/unodos.c + DC hooks) |
| [fb.c](fb.c)/[fb.h](fb.h) | software framebuffer + primitives (640×480) | ps2 (resolution differs) |
| [mac_compat.c](mac_compat.c)/[.h](mac_compat.h) | Mac Toolbox → fb.* shim | identical to ps2 |
| [mac_io.c](mac_io.c) | File Manager (HOST stdio / **DC VMU**) + Sound stub | HOST branch identical to ps2 |
| [dc_main.c](dc_main.c) | **KallistiOS** video present + maple input | DC-only |
| [uno_splash.c](uno_splash.c) | M0 hello-PVR splash (reference) | adapted from ps2 |
| [host_main.c](host_main.c)/[host_desktop.c](host_desktop.c) | host-shim present → PPM | identical to ps2 |
| [Makefile](Makefile)/[build.sh](build.sh) | KOS build + host inner loop | DC-specific |

## Next

- **AICA audio** (M3): wire the Sound Manager shim to KOS's sound API.
- **Real run**: build with KallistiOS, boot the `.cdi` in Flycast/lxdream/redream,
  capture the desktop + apps; then real hardware (CD-R or dc-tool).
- **PVR present** (optional): a textured-quad present for hardware-accelerated
  scaling/vsync — purely below [fb.c](fb.c).
- **Keyboard arrows**: route the Dreamcast keyboard's own arrow keys (M1 uses the
  controller d-pad).
