# UnoDOS 3

A graphical operating system for IBM PC XT-compatible computers, written entirely in x86 assembly language.

![License](https://img.shields.io/badge/license-CC%20BY--NC%204.0-blue)

## Overview

UnoDOS 3 is a GUI-first operating system designed for vintage PC hardware. It boots directly into a graphical environment with windowed multitasking, mouse support, and a full application ecosystem — all running on bare metal with no DOS dependency.

### Philosophy

- **GUI-First**: No command line. Boots directly into a graphical desktop with icons.
- **Bare Metal**: Runs on raw hardware using only BIOS services. No DOS, no runtime.
- **Vintage-Friendly**: Designed for the constraints of 1980s hardware (8088 CPU, 128KB RAM, CGA graphics).
- **Self-Contained**: Everything fits on a single 1.44MB floppy disk.

## Screenshots

*Screenshots coming soon — the OS runs in CGA 320x200 (4-color) and VGA 320x200 (256-color) modes.*

## Features

### System
- Three-stage boot chain (floppy: boot sector + stage2 + kernel; HDD: MBR + VBR + stage2 + kernel)
- Boots from floppy disk, hard drive, CF card, or USB flash drive
- 45KB kernel with 105 system call APIs via `INT 0x80`
- Cooperative multitasking — up to 6 concurrent apps + launcher
- Dynamic segment allocation (each app gets its own 64KB segment)
- FAT12 (floppy) and FAT16 (hard drive) filesystem support with full read/write
- Settings persistence across reboots

### Graphics
- CGA 320x200 4-color mode (default)
- VGA 320x200 256-color mode
- VGA 640x480 16-color mode (Mode 12h)
- VESA 640x480 256-color mode
- Three bitmap fonts: 4x6, 8x8, 8x14 with runtime selection
- Color theme system with user-configurable palette

### Window Manager
- Up to 16 windows with z-ordered rendering
- Title bars, borders, close buttons
- Mouse-driven outline drag (Windows 3.1 style)
- Window resize with drag handle
- Active/inactive title bar distinction
- Drawing context API — apps use window-relative coordinates

### GUI Toolkit
- Buttons, radio buttons, checkboxes
- Text input fields (with cursor and password mode)
- Scrollbars (vertical, horizontal, draggable)
- List items, progress bars, group boxes
- Combo boxes, menu bars, popup context menus
- System file open/save dialogs
- System clipboard (copy/paste between apps)

### Input
- PS/2 keyboard driver with modifier tracking (Shift/Ctrl/Alt)
- PS/2 mouse driver (BIOS INT 15h + KBC fallback)
- USB mouse support via BIOS legacy emulation

### Audio
- PC speaker tone generation via PIT Channel 2

### Applications (14 included)
| App | Description |
|-----|-------------|
| **Launcher** | Desktop shell with 4x3 icon grid, keyboard and mouse navigation |
| **Notepad** | Text editor with selection, undo, clipboard, context menu, file dialogs |
| **File Manager** | Scrollable file list with delete, rename, copy (FAT12/FAT16) |
| **Dostris** | Tetris clone with Korobeiniki music (CGA version) |
| **Dostris VGA** | Tetris clone (VGA 256-color version) |
| **OutLast** | Top-down driving game with traffic (CGA version) |
| **OutLast VGA** | Driving game (VGA 256-color version) |
| **Clock** | Analog clock face + digital time display |
| **Music** | PC speaker music player (Fur Elise, 5 songs with visual staff) |
| **Settings** | Font selection, color themes, video mode switching |
| **MkBoot** | Boot floppy creator (floppy-to-floppy copy) |
| **SysInfo** | System information display (CPU, RAM, drives) |
| **Mouse Test** | Mouse diagnostic tool |
| **Hello** | Minimal test application |

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

The write tool auto-detects available drives, excludes system drives for safety, and includes optional read-back verification.

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
│   ├── launcher.asm         # Desktop launcher
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
├── CHANGELOG.md             # Version history
├── LICENSE                  # CC BY-NC 4.0
└── TODO.md                  # Roadmap
```

## Documentation

- **[App Development Guide](docs/APP_DEVELOPMENT.md)** — How to write applications for UnoDOS
- **[API Reference](docs/API_REFERENCE.md)** — Complete system call reference (105 functions)
- **[Architecture](docs/ARCHITECTURE.md)** — Boot process and system architecture
- **[Features](docs/FEATURES.md)** — Detailed feature list
- **[Memory Layout](docs/MEMORY_LAYOUT.md)** — Memory map and segment architecture
- **[Boot Debug Messages](docs/boot-debug-messages.md)** — Diagnostic output reference
- **[Bootloader Architecture](docs/bootloader-architecture.md)** — Floppy and HD boot chains
- **[Changelog](CHANGELOG.md)** — Full version history

## Writing Your Own App

UnoDOS apps are flat `.BIN` binaries assembled with NASM. Each app runs in its own 64KB segment and communicates with the kernel through `INT 0x80` system calls. Here's a minimal example:

```asm
[BITS 16]
[ORG 0x0000]

; 80-byte icon header
    db 0xEB, 0x4E                   ; JMP to code entry
    db 'UI'                         ; Magic
    db 'MyApp', 0                   ; Display name
    times (0x04 + 12) - ($ - $$) db 0
    times 64 db 0xFF               ; 16x16 icon placeholder
    times 0x50 - ($ - $$) db 0

; Code entry (offset 0x50)
entry:
    pusha
    push ds
    push es
    mov ax, cs
    mov ds, ax                      ; DS = our segment

    ; Create a window
    mov bx, 60
    mov cx, 60
    mov dx, 200
    mov si, 60
    mov ax, cs
    mov es, ax
    mov di, title
    mov al, 0x03                    ; Title + border
    mov ah, 20                      ; win_create
    int 0x80
    jc .exit

    mov ah, 31                      ; win_begin_draw
    int 0x80

    ; Draw text
    mov bx, 10
    mov cx, 10
    mov si, msg
    mov ah, 4                       ; gfx_draw_string
    int 0x80

    ; Event loop
.loop:
    sti
    mov ah, 9                       ; event_get
    int 0x80
    jc .loop
    cmp al, 1                       ; KEY_PRESS?
    jne .loop
    cmp dl, 27                      ; ESC?
    jne .loop

.exit:
    xor ax, ax
    pop es
    pop ds
    popa
    retf

title: db 'MyApp', 0
msg:   db 'Hello, UnoDOS!', 0
```

See the [App Development Guide](docs/APP_DEVELOPMENT.md) for the full tutorial.

## Architecture Overview

```
┌─────────────────────────────────────────┐
│              Applications               │
│  (6 concurrent apps, each 64KB segment) │
├─────────────────────────────────────────┤
│          INT 0x80 System Calls          │
│        (105 API functions)              │
├─────────────────────────────────────────┤
│               Kernel                    │
│  Window Manager │ Graphics │ Filesystem │
│  Scheduler      │ Events   │ GUI Toolkit│
│  Mouse Driver   │ Keyboard │ Clipboard  │
├─────────────────────────────────────────┤
│          BIOS Services                  │
├─────────────────────────────────────────┤
│          x86 Hardware                   │
│   8088/8086/286/386/486 Real Mode       │
└─────────────────────────────────────────┘
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
