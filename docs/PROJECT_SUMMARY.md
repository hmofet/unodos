# UnoDOS 3 - Project Summary

UnoDOS 3 is a graphical operating system written entirely in x86 assembly language, designed to run on IBM PC/XT-compatible hardware from the 1980s through 486-era machines. It boots from floppy disk or hard drive with no DOS dependency, providing a windowed GUI with mouse support, cooperative multitasking, and a full application ecosystem — all in under 30KB of kernel code.

## Architecture

The system runs in CGA mode 4 (320x200, 4 colors, 2 bits per pixel) with an interlaced framebuffer at segment 0xB800. The kernel loads at segment 0x1000 via a two-stage boot process: a 512-byte boot sector loads a 4-sector stage2 loader, which loads the 56-sector kernel. For hard drives, an MBR/VBR/stage2 chain handles FAT16 partition boot.

Applications communicate with the kernel through INT 0x80, using AH as a function index into a 91-entry API dispatch table. The handler saves caller segments, sets DS to the kernel segment (0x1000), and dispatches. For drawing APIs, the handler auto-translates BX/CX coordinates relative to the active window's content area, so apps draw at (0,0) without knowing their window's screen position.

## Memory and Multitasking

The launcher occupies a fixed segment at 0x2000, while up to six user applications load dynamically into segments 0x3000-0x8000. A cooperative scheduler round-robins between tasks on `app_yield` calls, saving and restoring each task's draw context, caller segments, and general-purpose registers. Tasks own windows and receive filtered events — keyboard input goes to the focused task, redraw events to the window owner.

## Windowing and Graphics

The window manager supports 16 windows with z-ordered rendering, title bars, borders, close buttons, and outline-based dragging (XOR rectangle during drag, single repaint on release). Background windows are blocked from drawing over foreground ones. A mouse cursor (8x10 XOR sprite) is managed through IRQ12 with a cursor_locked mechanism that prevents XOR corruption during drawing operations.

Three built-in bitmap fonts (4x6, 8x8, 8x14) support a comprehensive GUI toolkit: buttons, radio buttons, checkboxes, text input fields, scrollbars, list items, progress bars, group boxes, separators, combo boxes, and menu bars. A color theme system lets users configure text color, desktop background, and window color from the four CGA palette entries. Colored drawing APIs support lines, rectangles, and Bresenham's line algorithm.

## Filesystem

A read/write FAT12 driver handles floppy disks and a read/write FAT16 driver supports hard drive partitions (64MB). Both support mount, open, read, write, create, delete, close, readdir, seek, rename, and file size query. File handles (16 max, 32 bytes each) track open files, and mount handles route operations to the correct driver. The kernel loads a SETTINGS.CFG file at boot to restore user preferences.

## System Services

A system-wide clipboard (4KB at scratch segment 0x9000) enables copy/paste between applications. A popup menu system provides context menus and file menus. A blocking file open dialog lets any app show a browsable file list with keyboard and mouse navigation — the kernel handles the modal window and event loop, returning the selected filename to the caller.

## Applications

Twelve applications ship with the OS:

- **Launcher** — Desktop shell with 4x3 icon grid, keyboard/mouse navigation, floppy refresh
- **Clock** — Analog clock face with hands + digital time display, RTC-based
- **File Manager** — Scrollable file list with delete, rename, copy operations (FAT12/FAT16)
- **Notepad** — Text editor with selection, undo, clipboard, context menu, file menu, system file dialog
- **Mouse** — Mouse test showing cursor position and button state
- **Music** — PC speaker player (Fur Elise via PIT Channel 2)
- **Dostris** — Tetris clone with Korobeiniki music and toolkit buttons
- **Settings** — Font and color theme configuration with persistence
- **MkBoot** — Boot floppy creator (copies OS + apps to blank floppy)
- **UI Demo** — Showcases all GUI toolkit widgets
- **API Test** — Tests extended APIs with pass/fail display
- **Hello** — Test app for loader verification

Applications use a BIN format with an 80-byte header containing a JMP instruction, "UI" magic, 12-byte display name, and 64-byte icon bitmap. Code entry is at offset 0x50. Each app runs in its own segment with `[ORG 0x0000]`.

## Hardware Support

The OS runs on the original IBM PC (8088, 4.77 MHz) through 486 systems. It has been tested on an HP Omnibook 600C (486DX4-75) with real floppy and CF card hardware. PS/2 mouse support uses BIOS INT 15h/C2xx services (primary) with a direct KBC port I/O fallback, enabling USB mice through BIOS legacy emulation. Hard drive boot works with CF cards and USB drives via BIOS LBA extensions with CHS fallback.

## Development Process

The project spans 351 builds. A Linux build machine runs NASM and creates floppy/HD images that are committed to git as pre-built binaries — the developer tests on Windows by pulling the repo and writing images to physical media with PowerShell scripts. Every build increments a visible build number on the splash screen for hardware verification.

## Current State (Build 351, v3.22.0)

The OS boots, displays a splash screen with progress bar, launches a desktop with application icons, and supports full windowed multitasking with mouse interaction. Twelve applications are included, with Notepad providing a full text editor with clipboard, undo, context menus, and a system file open dialog. Settings persist across reboots. The kernel uses approximately 27KB of its 28KB allocation, with the API table at 91 functions.
