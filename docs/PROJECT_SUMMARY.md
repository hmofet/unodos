# UnoDOS 3 - Project Summary

UnoDOS 3 is a graphical operating system written entirely in x86 assembly language, designed to run on IBM PC/XT-compatible hardware from the 1980s through 486-era machines. It boots from floppy disk or hard drive with no DOS dependency, providing a windowed GUI with mouse support, cooperative multitasking, and a full application ecosystem — all in under 30KB of kernel code.

## Architecture

The system runs in CGA mode 4 (320x200, 4 colors, 2 bits per pixel) with an interlaced framebuffer at segment 0xB800. The kernel loads at segment 0x1000 via a two-stage boot process: a 512-byte boot sector loads a 4-sector stage2 loader, which loads the 56-sector kernel. For hard drives, an MBR/VBR/stage2 chain handles FAT16 partition boot.

Applications communicate with the kernel through INT 0x80, using AH as a function index into a 56-entry API dispatch table. The handler saves caller segments, sets DS to the kernel segment (0x1000), and dispatches. For drawing APIs (0-6, 50-52), the handler auto-translates BX/CX coordinates relative to the active window's content area, so apps draw at (0,0) without knowing their window's screen position.

## Memory and Multitasking

The launcher occupies a fixed segment at 0x2000, while up to six user applications load dynamically into segments 0x3000-0x8000. A cooperative scheduler round-robins between tasks on `app_yield` calls, saving and restoring each task's draw context, caller segments, and general-purpose registers. Tasks own windows and receive filtered events — keyboard input goes to the focused task, redraw events to the window owner.

## Windowing and Graphics

The window manager supports 16 windows with z-ordered rendering, title bars, borders, close buttons, and outline-based dragging (XOR rectangle during drag, single repaint on release). Background windows are blocked from drawing over foreground ones. A mouse cursor (8x10 XOR sprite) is managed through IRQ12 with a cursor_locked mechanism that prevents XOR corruption during drawing operations.

Three built-in bitmap fonts (4x6, 8x8, 8x14) support the GUI toolkit, which provides buttons, radio buttons, hit testing, word-wrapped text, and clipping rectangles. A color theme system lets users configure text color, desktop background, and window color from the four CGA palette entries.

## Filesystem

A read/write FAT12 driver handles floppy disks: mount, open, read, create, write, delete, and directory listing. A read-only FAT16 driver supports hard drive boot with 64MB partitions. File handles (16 max, 32 bytes each) track open files, and mount handles route operations to the correct driver. The kernel loads a SETTINGS.CFG file at boot to restore user preferences.

## Applications

Eight applications ship with the OS:

- **Launcher** — Desktop shell with 16x16 icon grid, keyboard/mouse navigation, floppy refresh
- **Clock** — Real-time clock display reading CMOS RTC via BIOS
- **Browser** — File listing with sizes for FAT12/FAT16 volumes
- **Mouse** — Mouse test showing cursor position and button state
- **Music** — PC speaker player (Fur Elise via PIT Channel 2)
- **Hello** — Test app for loader verification
- **Settings** — Font and color theme configuration with floppy persistence
- **MkBoot** — Boot floppy creator (copies OS + apps to blank floppy)

Applications use a BIN format with an 80-byte header containing a JMP instruction, "UI" magic, 12-byte display name, and 64-byte icon bitmap. Code entry is at offset 0x50. Each app runs in its own segment with `[ORG 0x0000]`.

## Hardware Support

The OS runs on the original IBM PC (8088, 4.77 MHz) through 486 systems. It has been tested on an HP Omnibook 600C (486DX4-75) with real floppy hardware. PS/2 mouse support uses BIOS INT 15h/C2xx services (primary) with a direct KBC port I/O fallback, enabling USB mice through BIOS legacy emulation. Hard drive boot works with CF cards and USB drives via BIOS LBA extensions.

## Development Process

The project spans 212 builds across roughly a month of development. A Linux build machine runs NASM and creates floppy/HD images that are committed to git as pre-built binaries — the developer tests on Windows by pulling the repo and writing images to physical media with PowerShell scripts. Every build increments a visible build number on the splash screen for hardware verification.

Key debugging challenges included: the BP/SS segment default (BP-relative addressing uses SS, not DS, causing silent data corruption when SS differs from DS), mouse cursor XOR races (IRQ12 firing between unprotected drawing calls), and FAT12 mount handle routing (comparing 16-bit BX instead of 8-bit BL causing silent failures with dirty upper bytes).

## Current State (Build 212, v3.19.0)

The OS boots, displays a splash screen with progress bar, launches a desktop with application icons, and supports full windowed multitasking with mouse interaction. Settings persist across reboots. The MkBoot floppy creator can write boot sectors and kernel but has a remaining bug preventing app file copying. The kernel uses approximately 25KB of its 28KB allocation, with the API table at 56 functions and room to grow.
