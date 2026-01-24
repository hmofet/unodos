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

## Current Features (v3.11.0)

- Three-stage boot architecture (boot sector + stage2 loader + kernel)
- Separate 28KB kernel loaded at 64KB mark
- Boot progress indicator (dots during kernel load)
- CGA 320x200 4-color graphics mode
- Custom bitmap fonts (8x8 for titles, 4x6 for small text)
- Graphical welcome screen with bordered window
- System call infrastructure (INT 0x80 + API table with 19 functions)
- Graphics API (draw pixel, rectangle, character, string)
- Memory allocator (malloc/free)
- Keyboard driver (INT 09h with scan code translation)
- Event system (32-event circular queue)
- Filesystem abstraction layer + FAT12 driver (read-only)
- File operations (mount, open, read, close)
- Multi-cluster file reading (FAT chain following)
- **Application Loader** (load and run .BIN apps from FAT12)

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

# Show binary sizes
make sizes

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
# Quick write (no verification)
.\tools\Write-Floppy-Quick.ps1

# With verification
.\tools\Write-Floppy.ps1

# Specify drive letter
.\tools\Write-Floppy-Quick.ps1 -DriveLetter B
```

### Windows (Command Prompt, Run as Administrator)

```cmd
tools\writeflop.bat
```

## Project Structure

```
unodos/
├── apps/
│   └── hello.asm       # Test application for app loader
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
│   ├── hello.bin       # Compiled test application
│   ├── unodos.img      # 360KB floppy image
│   └── unodos-144.img  # 1.44MB floppy image
├── docs/
│   ├── ARCHITECTURE.md # Boot loader architecture
│   ├── FEATURES.md     # Planned features roadmap
│   ├── CLAUDE.md       # Development context
│   └── SESSION_SUMMARY.md
├── tools/
│   ├── writeflop.sh    # Linux floppy write utility
│   ├── writeflop.bat   # Windows CMD floppy write
│   ├── Write-Floppy.ps1       # PowerShell with verification
│   ├── Write-Floppy-Quick.ps1 # PowerShell quick write
│   └── create_app_test.py     # Create app test floppy image
├── Makefile
├── README.md
├── CHANGELOG.md
└── VERSION
```

## Documentation

- [ARCHITECTURE.md](docs/ARCHITECTURE.md) - Technical details of the boot process
- [ARCHITECTURE_PLAN.md](docs/ARCHITECTURE_PLAN.md) - System architecture and implementation roadmap
- [SYSCALL.md](docs/SYSCALL.md) - System call architecture analysis
- [FAT12_IMPLEMENTATION_SUMMARY.md](docs/FAT12_IMPLEMENTATION_SUMMARY.md) - FAT12 filesystem implementation
- [FAT12_HARDWARE_DEBUG.md](docs/FAT12_HARDWARE_DEBUG.md) - Hardware debugging and bug fixes
- [FEATURES.md](docs/FEATURES.md) - Planned features roadmap
- [CHANGELOG.md](CHANGELOG.md) - Version history and changes

## Development Status

### Completed
- [x] Three-stage boot architecture (boot + stage2 + kernel)
- [x] Separate kernel (24KB, loaded at 64KB mark)
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

**Foundation Layer Complete!** All core infrastructure is now in place.

### Completed (Core Services) ✅
- [x] Application loader (load .BIN from FAT12) - v3.11.0

### In Progress (Core Services - v3.12.0-v3.13.0)
- [ ] GUI window manager
- [ ] Bootloader installation tool (Tier 3 support)

### Planned (Standard Library - v3.14.0)
- [ ] graphics.lib - C-callable wrappers for Graphics API
- [ ] unodos.lib - Initialization and utility functions

### Planned (Applications - v3.8.0+)
- [ ] Clock display application
- [ ] Text editor
- [ ] File manager
- [ ] Calculator
- [ ] Mouse support (serial mouse)
- [ ] Sound support (PC speaker)

See [ARCHITECTURE_PLAN.md](docs/ARCHITECTURE_PLAN.md) for detailed roadmap.

## Version History

See [CHANGELOG.md](CHANGELOG.md) for detailed version history.

Current version: **3.11.0**

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
