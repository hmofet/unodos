# UnoDOS 3

A graphical operating system for IBM PC XT-compatible computers.

## Overview

UnoDOS 3 is a GUI-first operating system designed for vintage PC hardware. Unlike traditional systems that boot into a command line, UnoDOS boots directly into a graphical environment. It does not depend on MS-DOS or any other operating system—it interacts directly with the BIOS and Intel 8088/8086 processor.

### Philosophy

- **GUI-First**: No command line interface. The system boots directly into a graphical shell.
- **Minimal Dependencies**: Runs on bare metal using only BIOS services.
- **Vintage-Friendly**: Designed for the constraints of 1980s hardware.
- **Self-Contained**: Boots and runs entirely from a single floppy disk.

## Target Hardware

| Component | Minimum Specification | Recommended |
|-----------|----------------------|-------------|
| CPU | Intel 8088 @ 4.77 MHz | Intel 8086/80286 |
| RAM | 128 KB | 256 KB+ |
| Display | CGA (Color Graphics Adapter) | CGA or EGA |
| Storage | 5.25" 360KB Floppy Drive | 3.5" 1.44MB Floppy |
| Input | PC/XT keyboard | PS/2 mouse (optional) |
| BIOS | IBM PC/XT compatible | Any PC-compatible BIOS |

### Tested Hardware

- **HP Omnibook 600C** (486DX4-75, VGA with CGA emulation, 1.44MB floppy)
- **IBM PS/2 L40** (386SX, B&W display, 1.44MB floppy)
- **QEMU** (PC/XT emulation mode)

## Current Features (v3.18.0 Build 196)

- **Boots from floppy disk, hard drive, CF card, or USB flash drive**
- Three-stage boot architecture (boot sector + stage2 loader + kernel)
- Separate 28KB kernel loaded at 64KB mark
- CGA 320x200 4-color graphics mode
- Custom bitmap fonts (8x8 for titles, 4x6 for small text)
- System call infrastructure (INT 0x80 + API table with 44 functions)
- Graphics API (pixel, rectangle, character, string, text measurement, icons)
- Memory allocator (malloc/free)
- Keyboard driver (INT 09h with scan code translation)
- PS/2 Mouse driver (BIOS services + KBC fallback) with visible XOR cursor, USB mouse support
- Event system (32-event circular queue, per-task filtering)
- FAT12 filesystem (floppy) and FAT16 filesystem (hard drive) — read-only
- Application Loader with dynamic segment allocation
- **Cooperative multitasking** - up to 6 concurrent user apps + launcher
- **Window Manager** with close button, outline drag, z-order management (16 windows max)
- **Active/inactive title bars** - visual distinction for foreground window
- **Window Drawing Context** (apps use window-relative coordinates)
- **PC Speaker sound** - tone generation API + Fur Elise music player app
- **Desktop Launcher** with icon grid, double-click launch, auto boot media detection

## Building

### Requirements

**Linux (Ubuntu/Debian):**
```bash
sudo apt install nasm qemu-system-x86 make python3
```

### Build Commands

```bash
# Build 1.44MB floppy image with OS + apps
make floppy144

# Build launcher floppy image
make apps && make build/launcher-floppy.img

# Build 64MB hard drive image (FAT16)
make hd-image

# Run in QEMU (1.44MB floppy)
make run144

# Run in QEMU (hard drive)
make run-hd

# Clean build artifacts
make clean
```

### Build Output

After building, you'll find:
- `build/unodos-144.img` - 1.44MB floppy image (OS + apps, FAT12)
- `build/launcher-floppy.img` - Launcher + apps floppy image
- `build/unodos-hd.img` - 64MB hard drive image (OS + apps, FAT16)
- `build/*.bin` - Individual compiled binaries

## Writing to Physical Floppy

### Windows (PowerShell, Run as Administrator)

```powershell
# Write boot floppy
.\tools\boot.ps1

# Write launcher floppy
.\tools\launcher.ps1
```

### Linux

```bash
sudo dd if=build/unodos-144.img of=/dev/fd0 bs=512
```

### Testing on Real Hardware

1. Write boot floppy: `.\tools\boot.ps1`
2. Write launcher floppy: `.\tools\launcher.ps1`
3. Boot from boot floppy (verify Build number on screen)
4. Press **L** key to load launcher
5. When prompted, swap to launcher floppy
6. Launcher shows apps: CLOCK, TEST, BROWSER, MOUSE, MUSIC
7. Navigate with W/S keys, press Enter to launch
8. ESC returns to launcher
9. Mouse cursor visible; drag windows by title bar

## Project Structure

```
unodos/
├── apps/
│   ├── hello.asm       # Test application (TEST.BIN)
│   ├── clock.asm       # Real-time clock display
│   ├── launcher.asm    # Desktop launcher
│   ├── browser.asm     # File browser
│   ├── mouse_test.asm  # PS/2 mouse test application
│   └── music.asm       # Fur Elise music player
├── boot/
│   ├── boot.asm        # First stage boot loader (512 bytes)
│   └── stage2.asm      # Second stage loader (2KB)
├── kernel/
│   ├── kernel.asm      # Main OS kernel (28KB)
│   ├── font8x8.asm     # 8x8 bitmap font data
│   └── font4x6.asm     # 4x6 small font data
├── build/
│   ├── *.bin            # Compiled binaries
│   ├── unodos-144.img   # 1.44MB boot floppy image (FAT12)
│   ├── unodos-hd.img    # 64MB hard drive image (FAT16)
│   └── launcher-floppy.img # Launcher + apps floppy
├── docs/                # Technical documentation
├── tools/               # Build and deployment utilities
├── Makefile
├── README.md
├── CHANGELOG.md
├── CLAUDE.md            # AI development guidelines
├── VERSION              # Version string (3.18.0)
└── BUILD_NUMBER         # Build number (193)
```

## Documentation

- [ARCHITECTURE.md](docs/ARCHITECTURE.md) - Boot process and system architecture
- [FEATURES.md](docs/FEATURES.md) - Features roadmap and API reference
- [WINDOW_MANAGER_PLAN.md](docs/WINDOW_MANAGER_PLAN.md) - Window manager design
- [MEMORY_LAYOUT.md](docs/MEMORY_LAYOUT.md) - Memory layout analysis
- [CHANGELOG.md](CHANGELOG.md) - Version history and changes

## Development Status

### Completed (Foundation Layer)
- [x] System call infrastructure (INT 0x80 + Far Call Table)
- [x] Graphics API (7 functions including text measurement)
- [x] Memory allocator (malloc/free)
- [x] Keyboard driver (INT 09h)
- [x] PS/2 Mouse driver (BIOS INT 15h/C2 + KBC fallback) with XOR cursor
- [x] Event system (32-event circular queue)
- [x] FAT12 filesystem (floppy, read-only)
- [x] FAT16 filesystem (hard drive, read-only)

### Completed (Core Services)
- [x] Application loader with dynamic segment allocation
- [x] Cooperative multitasking (6 concurrent user apps + launcher)
- [x] Window Manager (create, destroy, draw, focus, move, close button, outline drag)
- [x] Z-order management (background draw blocking, active/inactive title bars)
- [x] Window drawing context (window-relative coordinates)
- [x] Desktop Launcher with icon grid, app discovery, auto boot media detection
- [x] Mouse cursor (XOR sprite, title bar hit testing, drag state machine, USB mouse via BIOS)
- [x] Desktop icons (16x16 2bpp CGA, auto-detected from BIN headers)
- [x] PC Speaker sound (tone generation + silence APIs)
- [x] Hard drive boot (MBR → VBR → Stage2_hd, FAT16 partition)

### Completed (Applications)
- [x] LAUNCHER.BIN - Desktop launcher with icon grid (2922 bytes)
- [x] CLOCK.BIN - Real-time clock display (330 bytes)
- [x] TEST.BIN - Hello World window (251 bytes)
- [x] BROWSER.BIN - File browser with file sizes (569 bytes)
- [x] MOUSE.BIN - Mouse test/demo (745 bytes)
- [x] MUSIC.BIN - Fur Elise music player (649 bytes)

### Planned (Future)
- [ ] Text editor
- [ ] Calculator
- [ ] Window resize

## Version History

See [CHANGELOG.md](CHANGELOG.md) for detailed version history.

Current version: **3.18.0** (Build 196)

## License

Copyright (c) 2026 Arin Bakht

This project is licensed under the [Creative Commons Attribution-NonCommercial 4.0 International License](LICENSE) (CC BY-NC 4.0).

- **Modification**: Allowed
- **Attribution**: Required — credit the original author and link to this repository
- **Commercial use**: Not permitted

---

*UnoDOS 3 - Because sometimes the old ways are the best ways.*
