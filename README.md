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
- **QEMU** (PC/XT emulation mode)

## Current Features (v3.13.0 Build 135)

- Three-stage boot architecture (boot sector + stage2 loader + kernel)
- Separate 28KB kernel loaded at 64KB mark
- CGA 320x200 4-color graphics mode
- Custom bitmap fonts (8x8 for titles, 4x6 for small text)
- System call infrastructure (INT 0x80 + API table with 34 functions)
- Graphics API (pixel, rectangle, character, string, text measurement)
- Memory allocator (malloc/free)
- Keyboard driver (INT 09h with scan code translation)
- PS/2 Mouse driver (IRQ12/INT 0x74) with visible XOR cursor
- Event system (32-event circular queue)
- FAT12 filesystem (read-only: mount, open, read, close, readdir)
- Application Loader (load and run .BIN apps from FAT12)
- **Window Manager** with mouse-driven title bar dragging
- **Window Drawing Context** (apps use window-relative coordinates)
- **OS-managed content preservation** during window drags
- **Desktop Launcher** with dynamic app discovery

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

# Run in QEMU (1.44MB image)
make run144

# Clean build artifacts
make clean
```

### Build Output

After building, you'll find:
- `build/unodos-144.img` - 1.44MB floppy image (OS + apps)
- `build/launcher-floppy.img` - Launcher + apps floppy image
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
6. Launcher shows apps: CLOCK, TEST, BROWSER, MOUSE
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
│   └── mouse_test.asm  # PS/2 mouse test application
├── boot/
│   ├── boot.asm        # First stage boot loader (512 bytes)
│   └── stage2.asm      # Second stage loader (2KB)
├── kernel/
│   ├── kernel.asm      # Main OS kernel (28KB)
│   ├── font8x8.asm     # 8x8 bitmap font data
│   └── font4x6.asm     # 4x6 small font data
├── build/
│   ├── *.bin            # Compiled binaries
│   ├── unodos-144.img   # 1.44MB boot floppy image
│   └── launcher-floppy.img # Launcher + apps floppy
├── docs/                # Technical documentation
├── tools/               # Build and deployment utilities
├── Makefile
├── README.md
├── CHANGELOG.md
├── CLAUDE.md            # AI development guidelines
├── VERSION              # Version string (3.13.0)
└── BUILD_NUMBER         # Build number (134)
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
- [x] PS/2 Mouse driver (IRQ12/INT 0x74) with XOR cursor
- [x] Event system (32-event circular queue)
- [x] FAT12 filesystem (read-only)

### Completed (Core Services)
- [x] Application loader (load .BIN from FAT12)
- [x] Window Manager (create, destroy, draw, focus, move, dragging)
- [x] Window drawing context (window-relative coordinates)
- [x] OS-managed content preservation during window drags
- [x] Desktop Launcher with dynamic app discovery
- [x] Mouse cursor (XOR sprite, title bar hit testing, drag state machine)

### Completed (Applications)
- [x] LAUNCHER.BIN - Desktop launcher (1069 bytes)
- [x] CLOCK.BIN - Real-time clock display (238 bytes)
- [x] TEST.BIN - Hello World window (159 bytes)
- [x] BROWSER.BIN - File browser with file sizes (477 bytes)
- [x] MOUSE.BIN - Mouse test/demo (634 bytes)

### Planned (Future)
- [ ] Text editor
- [ ] Calculator
- [ ] Overlapping window redraw
- [ ] Sound support (PC speaker)

## Version History

See [CHANGELOG.md](CHANGELOG.md) for detailed version history.

Current version: **3.13.0** (Build 135)

## License

UnoDOS License - Copyright (c) 2026 Arin Bakht

- **Distribution**: Allowed in original, unmodified form with attribution
- **Modification**: NOT permitted (no derivatives)
- **Attribution**: Required - credit original author and link to source

---

*UnoDOS 3 - Because sometimes the old ways are the best ways.*
