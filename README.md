# UnoDOS 3

A graphical operating system for IBM PC XT-compatible computers, written entirely in x86 assembly language.

![License](https://img.shields.io/badge/license-CC%20BY--NC%204.0-blue)

## Overview

UnoDOS 3 is a GUI-first operating system that boots directly into a windowed desktop environment. It runs on bare metal x86 hardware with no DOS dependency — just BIOS services and an Intel 8088 or later processor. The entire OS, including a 45KB kernel with 105 system calls, a window manager, two filesystems, cooperative multitasking, and 14 applications, fits on a single 1.44MB floppy disk.

### Philosophy

- **GUI-First**: No command line. The system boots directly into a graphical desktop with draggable icons, windows, and mouse support.
- **Bare Metal**: Runs on raw hardware using only BIOS services. No DOS, no runtime, no dependencies.
- **Vintage-Friendly**: Designed for the constraints of 1980s hardware — runs on an original IBM PC with 128KB RAM and a CGA card.
- **Self-Contained**: The kernel, window manager, GUI toolkit, filesystem drivers, and all 14 applications fit on one floppy disk.

## Screenshots

*Screenshots coming soon — the OS runs in CGA 320x200 (4-color) and VGA 320x200 (256-color) modes.*

## Features

### Display Modes

UnoDOS supports four video modes, switchable at runtime from the Settings app:

| Mode | Resolution | Colors | Bits/Pixel | Memory |
|------|-----------|--------|------------|--------|
| CGA Mode 4 | 320x200 | 4 | 2 (interlaced) | 0xB800 |
| VGA Mode 13h | 320x200 | 256 | 8 (linear) | 0xA000 |
| VGA Mode 12h | 640x480 | 16 | 4 (planar) | 0xA000 |
| VESA | 640x480 | 256 | 8 (banked) | 0xA000 |

All drawing APIs, widgets, and applications work across all four modes. Higher resolutions automatically scale window content 2x for readability.

### Kernel & System Calls

The kernel provides 105 API functions accessed via `INT 0x80` with the function index in AH. The API covers:

- **Graphics** (APIs 0-6, 67-71, 80, 94, 102-104): Pixel, rectangle, filled rectangle, character, string, inverted string, clear area, colored drawing, horizontal/vertical/Bresenham lines, scroll area, 1-bit sprite drawing, scaled sprite (nearest-neighbor), screen blit (region copy), pixel read
- **Fonts** (APIs 33, 48-49, 93): Three built-in bitmap fonts (4x6 small, 8x8 default, 8x14 large) with runtime selection, text width measurement, and font metrics query
- **Window Manager** (APIs 20-25, 31-32, 64, 78-79, 96-97): Create, destroy, draw, focus, move, resize windows; drawing context for window-relative coordinates; window info query; content area size; content scaling
- **Widgets** (APIs 50-62, 65-66, 87-89, 98-99): Button, radio button, checkbox, text input field, scrollbar (with drag hit-testing), list item, progress bar, group box, separator, combo box, menu bar, word-wrapped text, popup context menus, system file open/save dialogs
- **Events** (APIs 9-10): Non-blocking and blocking event retrieval from a 32-entry circular queue. Event types: KEY_PRESS, KEY_RELEASE, TIMER, MOUSE, WIN_MOVED, WIN_REDRAW
- **Filesystem** (APIs 13-16, 27, 40, 44-47, 75-77): Mount, open, read, write, create, delete, close, readdir, seek, rename, file size query, raw sector write, BIN header read — for both FAT12 (floppy) and FAT16 (hard drive)
- **Input** (APIs 28-30, 83, 101): Mouse state (position + buttons), mouse positioning, mouse detection, keyboard modifier state (Shift/Ctrl/Alt), mouse cursor visibility
- **Clipboard** (APIs 84-86): System-wide 4KB clipboard with copy, paste, and length query — shared between all running apps
- **Audio** (APIs 41-42): PC speaker tone generation (frequency in Hz) and silence, via PIT Channel 2
- **Multitasking** (APIs 18-19, 34-36, 74): Load app, run app (blocking), yield to scheduler, start app (non-blocking), exit task, task info query
- **System** (APIs 43, 63, 72-73, 81-82, 95): Boot drive detection, tick counter, RTC read/write (BCD), delay with yield, screen info, video mode switching
- **Themes** (APIs 54-55): Set/get color theme (text, desktop background, window frame colors)
- **Memory** (APIs 7-8): Heap allocation and free (first-fit allocator)
- **Desktop** (APIs 37-39): Desktop icon registration, icon clearing, 16x16 icon rendering

### Window Manager

- **16 concurrent windows** with z-ordered rendering and per-pixel clipping
- Title bars with app name, borders, and close button [X]
- **Outline drag** — XOR rectangle follows the mouse during drag, window moves on release (Windows 3.1 style)
- **Resize** — drag handle in the bottom-right corner (10x10 hit zone), minimum 60x40 pixels
- **Active/inactive distinction** — focused window gets a highlighted title bar; background windows are dimmed
- **Drawing context** — apps call `win_begin_draw` and then draw at (0,0) meaning top-left of the content area. The kernel auto-translates coordinates to absolute screen position for all drawing, widget, sprite, and menu APIs
- **Content scaling** — in 640x480 modes, windows auto-scale content 2x so apps designed for 320x200 remain usable
- **Z-order clipping** — drawing calls from background windows are silently blocked; apps repaint on focus via WIN_REDRAW events
- **Cursor protection** — the kernel automatically hides/shows the mouse cursor around all drawing operations to prevent XOR corruption from IRQ12

### GUI Toolkit

A complete widget library, all coordinate-translated through the drawing context:

| Widget | Description |
|--------|-------------|
| Button | Raised/pressed states with centered text label |
| Radio Button | Circular selector with fill indicator |
| Checkbox | Square toggle with check mark |
| Text Field | Text input with cursor, selection, and password mode |
| Scrollbar | Vertical/horizontal with draggable thumb, arrows, and track hit-testing |
| List Item | Row with text, optional selection highlight |
| Progress Bar | Fill indicator with percentage text |
| Group Box | Labeled frame for grouping controls |
| Separator | Horizontal or vertical divider line |
| Combo Box | Dropdown selector |
| Menu Bar | Horizontal menu items with padding and hit zones |
| Popup Menu | Context menu with item list and mouse hit-testing |
| File Open Dialog | Modal dialog with scrollable file list, keyboard/mouse nav, Open/Cancel |
| File Save Dialog | Modal dialog with filename text field, file list, overwrite confirmation |
| Word Wrap | Multi-line text rendering with automatic line breaking |

### Desktop

- **Icon grid** — apps discovered from disk, displayed with 16x16 bitmap icons (32x32 scaled in high-res modes)
- **Drag icons** — click and hold to reposition icons on the desktop
- **Lock/unlock** — prevent accidental icon rearrangement
- **Right-click context menu** — Auto Arrange, Sort A-Z, Sort Z-A, Lock/Unlock Icons, Refresh, Exit
- **Keyboard navigation** — arrow keys or WASD to select icons, Enter to launch
- **Double-click launch** — 0.5-second threshold using BIOS timer
- **Boot media auto-detection** — launcher queries the boot drive and mounts the correct filesystem (FAT12 for floppy, FAT16 for HD/CF/USB)

### Filesystem

Two filesystem drivers with a unified API that routes by mount handle:

**FAT12 (Floppy)**:
- 1.44MB floppy disk support
- 12-bit FAT entries, dual FAT copies
- Full read/write: mount, open, read, write, create, delete, rename, seek, readdir
- Settings persistence (SETTINGS.CFG loaded at boot)

**FAT16 (Hard Drive)**:
- 64MB partition with MBR and partition table
- 16-bit FAT entries (256 per sector), dual FAT copies
- Multi-sector cluster support
- LBA access with CHS fallback for older BIOSes (PCMCIA CF cards)
- Full read/write with same API as FAT12

### Multitasking

- **Cooperative round-robin scheduler** — apps yield control with `app_yield`, kernel switches to next runnable task
- **Up to 6 concurrent user apps** plus the launcher shell (7 total)
- **Dynamic segment allocation** — each app loads into its own 64KB segment from a pool (0x3000-0x8000)
- **Per-task state** — the kernel saves and restores registers, stack pointer, draw context, font selection, and caller segments on every context switch
- **Focus-aware input** — only the focused app receives keyboard events; mouse events go to the appropriate window owner
- **Automatic cleanup** — when an app exits, its windows are destroyed, speaker is silenced, and its segment is freed

### Input

**Keyboard:**
- Custom INT 09h handler (not BIOS INT 16h) with scan code translation
- 16-byte key buffer with per-task event delivery
- Modifier tracking: Shift, Ctrl, Alt states queryable via API

**Mouse:**
- Primary: BIOS INT 15h/C2 services (works with USB mice via BIOS legacy emulation)
- Fallback: Direct KBC port I/O with IRQ12 for BIOSes without INT 15h/C2 support
- XOR sprite cursor (8x8) with automatic hide/show during drawing
- Boot-time auto-detection with diagnostic letter: B (BIOS), K (KBC), E (no mouse)

### Audio

PC speaker tone generation via PIT Channel 2 — specify frequency in Hz. The speaker is automatically silenced when an app exits. Used by the Music app (5 classical pieces) and Dostris (Korobeiniki background music).

### Boot Chain

**Floppy boot**: Boot sector (512 bytes) loads Stage 2 (2KB), which loads the kernel (45KB) with a progress indicator. Each stage verifies a magic signature before transferring control.

**Hard drive boot**: MBR relocates to 0x0600, reads VBR from the first partition, VBR loads Stage 2, Stage 2 parses the FAT16 BPB and loads KERNEL.BIN from the filesystem. Supports both LBA (INT 13h extensions) and CHS fallback, with diagnostic output showing the read method and root directory LBA.

**Boot media**: 1.44MB floppy, hard drive, CompactFlash (PCMCIA or IDE), USB flash drive — anything the BIOS can boot from.

### Settings Persistence

The Settings app saves user preferences to SETTINGS.CFG on the boot drive:
- Font selection (4x6 / 8x8 / 8x14)
- Color theme (text color, desktop background, window frame color)
- Video mode (CGA / VGA 320x200 / VGA 640x480 / VESA 640x480)
- Real-time clock adjustment

Settings are loaded automatically at boot before the launcher starts.

### Fonts

Three built-in bitmap fonts, selectable per-app at runtime:

| Font | Size | Advance | Chars/Line (320px) | Use |
|------|------|---------|--------------------|-----|
| Font 0 | 4x6 | 6px | 53 | Small labels, status bars |
| Font 1 | 8x8 | 12px | 26 | Default body text |
| Font 2 | 8x14 | 12px | 26 | Large headings |

### Sprites & Blitting

- **1-bit sprite drawing** — transparent bitmap with caller-specified color, any width/height
- **Scaled sprite** — nearest-neighbor scaling to arbitrary destination size
- **Screen blit** — copy a rectangular region of the screen (used for smooth scrolling, animations)
- **Read pixel** — query the color value at any screen coordinate

## Applications (14 included)

| App | Size | Description |
|-----|------|-------------|
| **Launcher** | 8KB | Desktop shell — 4x3 icon grid (8x5 in high-res), keyboard/mouse navigation, icon drag, right-click context menu (sort, arrange, lock), double-click launch, floppy refresh, boot media auto-detection |
| **Notepad** | 61KB | Full text editor — text selection, clipboard (Ctrl+C/V/X), undo (Ctrl+Z), context menu (Cut/Copy/Paste/Delete), File menu (New/Open/Save), word wrap, vertical scrolling, system file dialogs |
| **File Manager** | 25KB | File browser — scrollable file list with scrollbar, columns (name, size, date), delete with confirmation, rename dialog, file copy with progress, FAT12/FAT16 support, keyboard arrow navigation |
| **Dostris** | 47KB | Tetris clone (CGA) — 7 tetrominoes, rotation, levels with increasing speed, score tracking, Korobeiniki background music on PC speaker, game over screen |
| **Dostris VGA** | 48KB | Tetris clone (VGA 256-color) — same gameplay in VGA mode 13h with 256-color graphics |
| **OutLast** | 47KB | Driving game (CGA) — pseudo-3D perspective road with curves, traffic cars, speed/steering mechanics, crash detection, roadside scenery, score tracking |
| **OutLast VGA** | 60KB | Driving game (VGA 256-color) — same gameplay in VGA mode 13h with richer graphics, title screen with PC speaker music |
| **Clock** | 11KB | Analog clock with hour/minute/second hands and digital time readout, driven by the BIOS real-time clock |
| **Music** | 33KB | PC speaker music player — 5 classical songs with scrolling musical staff visualization, note rendering (C4-G5 range), play/pause and prev/next controls |
| **Settings** | 34KB | System configuration — font selector with preview, color theme picker (text/background/window, 4 swatches each), video mode selector (CGA/VGA/Mode12h/VESA), RTC time adjustment, defaults button, persists to SETTINGS.CFG |
| **MkBoot** | 24KB | Boot floppy creator — reads OS and apps from the boot drive, prompts for a blank floppy, writes a complete bootable UnoDOS floppy (boot sector + stage2 + kernel + FAT12 filesystem + all apps) |
| **SysInfo** | 11KB | System information — displays UnoDOS version, current video mode and resolution, number of running tasks and open windows, real-time clock, available memory, boot drive info |
| **Mouse Test** | 8KB | Mouse diagnostic — shows real-time cursor position and button states, useful for verifying PS/2/USB mouse support on hardware |
| **Hello** | 3KB | Minimal windowed app — creates a window, draws "Hello, UnoDOS!", waits for ESC. Serves as a template for new app development |

## Target Hardware

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| CPU | Intel 8088 @ 4.77 MHz | 80286+ |
| RAM | 128 KB | 256 KB+ |
| Display | CGA | VGA |
| Storage | 3.5" 1.44MB floppy | Hard drive / CF card |
| Input | PC/XT keyboard | + PS/2 mouse |
| Audio | PC speaker (optional) | |

### Tested Hardware
- **HP Omnibook 600C** (486DX4-75, VGA, 1.44MB floppy, PCMCIA CF card)
- **IBM PS/2 L40** (386SX, B&W LCD, 1.44MB floppy)
- **ASUS Eee PC 1004** (Intel Atom, USB boot)
- **QEMU** (PC emulation)

## Memory Layout

```
0x0000:0000   IVT + BIOS Data Area           ~1.25 KB
0x0000:7C00   Boot Sector (temporary)         512 B
0x0800:0000   Stage 2 Loader                  2 KB
0x1000:0000   Kernel                          45 KB
0x1400:0000   Heap (malloc pool)              ~48 KB
0x2000:0000   Shell/Launcher (fixed)          64 KB
0x3000:0000   User app slot 0 (dynamic)       64 KB
0x4000:0000   User app slot 1                 64 KB
0x5000:0000   User app slot 2                 64 KB
0x6000:0000   User app slot 3                 64 KB
0x7000:0000   User app slot 4                 64 KB
0x8000:0000   User app slot 5                 64 KB
0x9000:0000   Scratch (clipboard + dialogs)   64 KB
0xA000:0000   VGA/VESA video memory           64 KB
0xB800:0000   CGA video memory                16 KB
```

Total: 7 app segments (1 shell + 6 user) using 448KB of the 640KB real-mode address space.

## Building

### Requirements

**Linux (Ubuntu/Debian):**
```bash
sudo apt install nasm qemu-system-x86 make python3
```

### Build Commands

```bash
# Build 1.44MB floppy image (OS + all apps)
make floppy144

# Build app-only launcher floppy
make apps && make build/launcher-floppy.img

# Build 64MB hard drive image (FAT16)
make hd-image

# Run in QEMU (floppy)
make run144

# Run in QEMU (hard drive)
make run-hd

# Clean build artifacts
make clean
```

### Build Output

| File | Description |
|------|-------------|
| `build/unodos-144.img` | 1.44MB boot floppy (FAT12, OS + apps) |
| `build/launcher-floppy.img` | Apps-only floppy image |
| `build/unodos-hd.img` | 64MB bootable hard drive image (FAT16) |
| `build/*.bin` | Individual compiled binaries |

### Pre-built Images

Pre-built disk images are included in the `build/` directory for users who can't build from source. Just clone the repo and write the image to media.

## Writing to Physical Media

### Windows (PowerShell, Run as Administrator)

```powershell
# Interactive mode — select image and target drive:
.\tools\write.ps1

# Quick floppy write:
.\tools\write.ps1 -DriveLetter A

# Quick HD/CF/USB write:
.\tools\write.ps1 -DiskNumber 2
```

The write tool auto-detects available drives, excludes system drives for safety, and includes optional read-back verification (`-Verify` flag).

### Linux

```bash
# Floppy
sudo dd if=build/unodos-144.img of=/dev/fd0 bs=512

# USB/CF (replace sdX with your device)
sudo dd if=build/unodos-hd.img of=/dev/sdX bs=512
```

## Project Structure

```
unodos/
├── apps/                    # Applications (14 NASM source files)
│   ├── launcher.asm         # Desktop launcher / shell
│   ├── notepad.asm          # Text editor
│   ├── browser.asm          # File manager
│   ├── tetris.asm           # Dostris (CGA)
│   ├── tetrisv.asm          # Dostris VGA
│   ├── outlast.asm          # OutLast driving game (CGA)
│   ├── outlastv.asm         # OutLast VGA
│   ├── clock.asm            # Clock
│   ├── music.asm            # Music player
│   ├── settings.asm         # System settings
│   ├── mkboot.asm           # Boot floppy creator
│   ├── sysinfo.asm          # System info
│   ├── mouse_test.asm       # Mouse diagnostic
│   └── hello.asm            # Hello World
├── boot/                    # Boot chain
│   ├── boot.asm             # Floppy boot sector (512 bytes)
│   ├── stage2.asm           # Floppy stage 2 loader
│   ├── mbr.asm              # Hard drive MBR
│   ├── vbr.asm              # Hard drive VBR
│   └── stage2_hd.asm        # HD stage 2 loader
├── kernel/
│   ├── kernel.asm           # Main OS kernel (45KB compiled)
│   ├── font4x6.asm          # 4x6 small font
│   ├── font8x8.asm          # 8x8 default font
│   └── font8x12.asm         # 8x14 large font
├── build/                   # Compiled binaries and disk images
├── docs/                    # Technical documentation
├── tools/                   # Build and deployment scripts
├── Makefile
├── CHANGELOG.md             # Version history (397 builds)
├── CONTRIBUTING.md          # Contribution guidelines
├── LICENSE                  # CC BY-NC 4.0
└── TODO.md                  # Roadmap
```

## Documentation

- **[App Development Guide](docs/APP_DEVELOPMENT.md)** — How to write applications for UnoDOS (with complete working example)
- **[API Reference](docs/API_REFERENCE.md)** — Complete system call reference (105 functions with register-level detail)
- **[Architecture](docs/ARCHITECTURE.md)** — Boot process, memory map, segment architecture, CGA video format
- **[Features](docs/FEATURES.md)** — Detailed feature list and API summary table
- **[Memory Layout](docs/MEMORY_LAYOUT.md)** — Physical memory map, kernel layout, segment pool architecture
- **[Boot Debug Messages](docs/boot-debug-messages.md)** — Diagnostic output reference for hardware troubleshooting
- **[Bootloader Architecture](docs/bootloader-architecture.md)** — Floppy and HD boot chain details
- **[Changelog](CHANGELOG.md)** — Full version history spanning 397 builds

## Writing Your Own App

UnoDOS apps are flat `.BIN` binaries assembled with NASM. Each app runs in its own 64KB segment and communicates with the kernel through `INT 0x80` system calls. Here's a minimal windowed app:

```asm
[BITS 16]
[ORG 0x0000]

; 80-byte icon header (JMP + magic + name + 16x16 bitmap)
    db 0xEB, 0x4E                   ; JMP to code entry at 0x50
    db 'UI'                         ; Magic identifier
    db 'MyApp', 0                   ; Display name (12 bytes max)
    times (0x04 + 12) - ($ - $$) db 0
    times 64 db 0xFF               ; 16x16 icon (placeholder: solid white)
    times 0x50 - ($ - $$) db 0

; Code entry (offset 0x50)
entry:
    pusha
    push ds
    push es
    mov ax, cs
    mov ds, ax                      ; DS = our segment

    ; Create a window (200x60 at position 60,60)
    mov bx, 60
    mov cx, 60
    mov dx, 200
    mov si, 60
    mov ax, cs
    mov es, ax
    mov di, title
    mov al, 0x03                    ; Title bar + border
    mov ah, 20                      ; win_create
    int 0x80
    jc .exit                        ; CF=1 = no free window slots

    mov ah, 31                      ; win_begin_draw (window-relative coords)
    int 0x80

    ; Draw text at (10,10) inside the window
    mov bx, 10
    mov cx, 10
    mov si, msg
    mov ah, 4                       ; gfx_draw_string
    int 0x80

    ; Event loop — wait for ESC
.loop:
    sti                             ; Re-enable interrupts (INT clears IF)
    mov ah, 9                       ; event_get (non-blocking)
    int 0x80
    jc .loop                        ; No event, keep polling
    cmp al, 1                       ; KEY_PRESS?
    jne .loop
    cmp dl, 27                      ; ESC?
    jne .loop

.exit:
    xor ax, ax
    pop es
    pop ds
    popa
    retf                            ; Return to kernel

title: db 'MyApp', 0
msg:   db 'Hello, UnoDOS!', 0
```

Assemble with `nasm -f bin -o MYAPP.BIN app.asm`, place in the FAT filesystem, and it appears on the desktop with its icon. See the [App Development Guide](docs/APP_DEVELOPMENT.md) for the full tutorial covering windows, events, mouse input, file I/O, widgets, fonts, and audio.

## Architecture Overview

```
┌──────────────────────────────────────────────┐
│               Applications (14)              │
│   6 concurrent user apps + launcher shell    │
│   Each in its own 64KB segment (ORG 0x0000)  │
├──────────────────────────────────────────────┤
│           INT 0x80 System Calls              │
│   105 API functions, bitmap-based dispatch   │
│   Auto-translate coords, cursor protection   │
├──────────────────────────────────────────────┤
│                  Kernel                      │
│                                              │
│  Window Manager    │  Graphics (4 modes)     │
│  16 windows, drag  │  3 fonts, sprites      │
│  resize, z-order   │  lines, fill, blit     │
│                    │                         │
│  Cooperative       │  FAT12 + FAT16         │
│  Scheduler         │  Read/Write/Create     │
│  6 app slots       │  Delete/Rename/Seek    │
│                    │                         │
│  GUI Toolkit       │  Event System          │
│  15 widget types   │  32-entry queue        │
│  File dialogs      │  Per-task filtering    │
│                    │                         │
│  PS/2 Mouse        │  Keyboard (INT 09h)    │
│  BIOS + KBC        │  Modifier tracking     │
│  XOR cursor        │  Focus-aware delivery  │
│                    │                         │
│  Clipboard (4KB)   │  PC Speaker audio      │
│  Theme system      │  Settings persistence  │
├──────────────────────────────────────────────┤
│              BIOS Services                   │
│  INT 13h (disk), INT 15h (mouse),           │
│  INT 10h (video), INT 1Ah (RTC)             │
├──────────────────────────────────────────────┤
│             x86 Hardware                     │
│   8088 / 8086 / 286 / 386 / 486             │
│   Real Mode (16-bit)                         │
└──────────────────────────────────────────────┘
```

## Version History

See [CHANGELOG.md](CHANGELOG.md) for the full history spanning 397 builds.

Current version: **v3.23.0** (Build 397)

## License

Copyright (c) 2026 Arin Bakht

This project is licensed under the [Creative Commons Attribution-NonCommercial 4.0 International License](LICENSE) (CC BY-NC 4.0).

- **Modification**: Allowed
- **Attribution**: Required — credit the original author and link to this repository
- **Commercial use**: Not permitted

---

*UnoDOS 3 — Because sometimes the old ways are the best ways.*
