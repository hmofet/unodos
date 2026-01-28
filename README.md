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
| BIOS | IBM PC/XT compatible | Any PC-compatible BIOS |

### Tested Hardware

- **HP Omnibook 600C** (486DX4-75, VGA with CGA emulation, 1.44MB floppy)
- **QEMU** (PC/XT emulation mode)

## Current Features (v3.12.0 Build 053)

- Three-stage boot architecture (boot sector + stage2 loader + kernel)
- Separate 28KB kernel loaded at 64KB mark
- Boot progress indicator (dots during kernel load)
- CGA 320x200 4-color graphics mode
- Custom bitmap fonts (8x8 for titles, 4x6 for small text)
- Graphical welcome screen with bordered window
- System call infrastructure (INT 0x80 + API table with 30 functions)
- Graphics API (draw pixel, rectangle, character, string)
- Memory allocator (malloc/free)
- Keyboard driver (INT 09h with scan code translation)
- **PS/2 Mouse driver (IRQ12/INT 0x74)** - NEW
- Event system (32-event circular queue with KEY and MOUSE events)
- Filesystem abstraction layer + FAT12 driver (read-only)
- File operations (mount, open, read, close, readdir)
- Multi-cluster file reading (FAT chain following)
- **Application Loader** (load and run .BIN apps from FAT12)
- **Window Manager** (create, destroy, draw windows)
- **Desktop Launcher** with dynamic app discovery

## Building

### Requirements

**Linux (Ubuntu/Debian):**
```bash
sudo apt install nasm qemu-system-x86 make
```

**Linux (Fedora):**
```bash
sudo dnf install nasm qemu-system-x86 make
```

**Linux (Arch):**
```bash
sudo pacman -S nasm qemu make
```

**Windows:**
- Install [NASM](https://nasm.us/) and add to PATH
- Install [QEMU](https://www.qemu.org/download/) (optional, for testing)
- Use WSL or MinGW for `make`

### Build Commands

```bash
# Build both 360KB and 1.44MB floppy images
make

# Build only 1.44MB image (for modern testing)
make floppy144

# Run in QEMU (360KB image)
make run

# Run in QEMU (1.44MB image)
make run144

# Debug mode with QEMU monitor
make debug

# Build test applications
make apps

# Test app loader in QEMU (two floppy drives)
make test-app

# Show binary sizes
make sizes

# Increment build number for next build
make bump-build

# Clean build artifacts
make clean
```

### Build Output

After building, you'll find:
- `build/unodos.img` - 360KB floppy image (for vintage 5.25" drives)
- `build/unodos-144.img` - 1.44MB floppy image (for modern 3.5" drives)

## Writing to Physical Floppy

### Linux

```bash
# Write to 5.25" floppy drive
sudo ./tools/writeflop.sh /dev/fd0

# Write 1.44MB image to 3.5" floppy
sudo ./tools/writeflop.sh -1 /dev/fd0
```

### Windows (PowerShell, Run as Administrator)

```powershell
# Write boot floppy (pulls latest from git first)
.\tools\boot.ps1

# Write app test floppy (for testing app loader)
.\tools\app-test.ps1

# Specify drive letter
.\tools\boot.ps1 -DriveLetter B
```

### Windows (Command Prompt, Run as Administrator)

```cmd
tools\writeflop.bat
```

### Testing Launcher on Real Hardware

1. Write boot floppy: `.\tools\boot.ps1`
2. Write launcher floppy: `.\tools\launcher.ps1`
3. Boot from boot floppy (verify Build: 053)
4. Press **L** key to load launcher
5. When prompted, swap to launcher floppy
6. Launcher shows apps: CLOCK, TEST, BROWSER, MOUSE
7. Navigate with W/S keys, press Enter to launch
8. ESC returns to launcher

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
│   ├── boot.bin        # Compiled boot sector
│   ├── stage2.bin      # Compiled stage2 loader
│   ├── kernel.bin      # Compiled kernel
│   ├── launcher.bin    # Desktop launcher application
│   ├── clock.bin       # Clock application
│   ├── browser.bin     # File browser application
│   ├── mouse_test.bin  # Mouse test application
│   ├── unodos.img      # 360KB floppy image
│   ├── unodos-144.img  # 1.44MB boot floppy image
│   └── launcher-floppy.img # Launcher + apps floppy image
├── docs/
│   ├── ARCHITECTURE.md # Boot loader architecture
│   ├── ARCHITECTURE_PLAN.md # System architecture roadmap
│   ├── FEATURES.md     # Features and API reference
│   ├── SESSION_SUMMARY.md # Latest session summary
│   └── ...             # Additional technical docs
├── tools/
│   ├── writeflop.sh    # Linux floppy write utility
│   ├── boot.ps1        # PowerShell - write boot floppy
│   ├── launcher.ps1    # PowerShell - write launcher floppy
│   └── create_app_test.py # Create app floppy images
├── Makefile
├── README.md
├── CHANGELOG.md
├── CLAUDE.md         # Development guidelines
├── VERSION           # Version string (3.12.0)
└── BUILD_NUMBER      # Build number (053)
```

## Documentation

- [ARCHITECTURE.md](docs/ARCHITECTURE.md) - Technical details of the boot process
- [ARCHITECTURE_PLAN.md](docs/ARCHITECTURE_PLAN.md) - System architecture and implementation roadmap
- [SYSCALL.md](docs/SYSCALL.md) - System call architecture analysis
- [FAT12_IMPLEMENTATION_SUMMARY.md](docs/FAT12_IMPLEMENTATION_SUMMARY.md) - FAT12 filesystem implementation
- [FAT12_HARDWARE_DEBUG.md](docs/FAT12_HARDWARE_DEBUG.md) - Hardware debugging and bug fixes
- [FEATURES.md](docs/FEATURES.md) - Planned features roadmap
- [SESSION_2026-01-25.md](docs/SESSION_2026-01-25.md) - Latest development session
- [CHANGELOG.md](CHANGELOG.md) - Version history and changes

## Development Status

### Completed
- [x] Three-stage boot architecture (boot + stage2 + kernel)
- [x] Separate kernel (28KB, loaded at 64KB mark)
- [x] Boot progress indicator
- [x] Memory detection (BIOS INT 12h)
- [x] Video adapter detection (CGA/EGA/VGA)
- [x] CGA 320x200 4-color graphics
- [x] Custom bitmap fonts (8x8, 4x6)
- [x] Graphical welcome screen
- [x] Real-time clock display
- [x] RAM status display
- [x] Character demonstration

### Completed (Foundation Layer) ✅
- [x] System call infrastructure (INT 0x80 + Far Call Table) - v3.3.0
- [x] Graphics API abstraction layer - v3.4.0
- [x] Memory allocator (malloc/free) - v3.5.0
- [x] Kernel expansion (16KB → 24KB → 28KB) - v3.6.0 → v3.10.0
- [x] Aggressive kernel optimization - v3.7.0
- [x] Keyboard input handling (INT 09h driver) - v3.8.0
- [x] Event system (circular queue, event-driven architecture) - v3.9.0
- [x] Filesystem abstraction layer + FAT12 driver - v3.10.0
- [x] **PS/2 Mouse driver (IRQ12/INT 0x74)** - v3.12.0 Build 053

**Foundation Layer Complete!** All core infrastructure is now in place.

### Completed (Core Services) ✅
- [x] Application loader (load .BIN from FAT12) - v3.11.0
- [x] Window Manager (create, destroy, draw, focus, move) - v3.12.0
- [x] Desktop Launcher with dynamic app discovery - v3.12.0 Build 042
- [x] Directory iteration (fs_readdir API) - v3.12.0 Build 042

### Completed (Applications) ✅
- [x] LAUNCHER.BIN - Desktop launcher with dynamic discovery (1061 bytes)
- [x] CLOCK.BIN - Real-time clock display (249 bytes)
- [x] TEST.BIN - Hello test application (112 bytes)
- [x] BROWSER.BIN - File browser showing files with sizes (564 bytes)
- [x] MOUSE.BIN - PS/2 mouse test/demo application (578 bytes)

### Planned (Standard Library - v3.14.0)
- [ ] graphics.lib - C-callable wrappers for Graphics API
- [ ] unodos.lib - Initialization and utility functions

### Planned (Future)
- [ ] Text editor
- [ ] Calculator
- [ ] Serial mouse support (Microsoft compatible)
- [ ] Sound support (PC speaker)

See [ARCHITECTURE_PLAN.md](docs/ARCHITECTURE_PLAN.md) for detailed roadmap.

## Version History

See [CHANGELOG.md](CHANGELOG.md) for detailed version history.

Current version: **3.12.0** (Build 053)

## Contributing

This project is in active development. See the documentation in `docs/` for technical details.

## License

UnoDOS License - Copyright (c) 2026 Arin Bakht

- **Distribution**: Allowed in original, unmodified form with attribution
- **Modification**: NOT permitted (no derivatives)
- **Attribution**: Required - credit original author and link to source

See [LICENSE.md](../LICENSE.md) for full terms.

---

*UnoDOS 3 - Because sometimes the old ways are the best ways.*
