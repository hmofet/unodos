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

## Current Features (v3.1.6)

- Two-stage boot loader (512-byte boot sector + 8KB second stage)
- CGA 320x200 4-color graphics mode
- Custom bitmap fonts (8x8 for titles, 4x6 for small text)
- Graphical welcome screen with bordered window
- Real-time clock display (reads from RTC via BIOS)
- Memory detection and display
- Character set demonstration

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
├── boot/
│   ├── boot.asm        # First stage boot loader (512 bytes)
│   ├── stage2.asm      # Second stage loader (8KB)
│   ├── font8x8.asm     # 8x8 bitmap font data
│   └── font4x6.asm     # 4x6 small font data
├── build/
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
│   └── Write-Floppy-Quick.ps1 # PowerShell quick write
├── Makefile
├── README.md
├── CHANGELOG.md
└── VERSION
```

## Documentation

- [ARCHITECTURE.md](docs/ARCHITECTURE.md) - Technical details of the boot process
- [FEATURES.md](docs/FEATURES.md) - Planned features and roadmap
- [CHANGELOG.md](CHANGELOG.md) - Version history and changes

## Development Status

### Completed
- [x] Boot sector with debug output
- [x] Two-stage boot loader
- [x] Memory detection (BIOS INT 12h)
- [x] Video adapter detection (CGA/EGA/VGA)
- [x] CGA 320x200 4-color graphics
- [x] Custom bitmap fonts (8x8, 4x6)
- [x] Graphical welcome screen
- [x] Real-time clock display
- [x] RAM status display
- [x] Character demonstration

### In Progress
- [ ] Character demo animation tuning

### Planned
- [ ] Keyboard input handling
- [ ] Mouse support (serial mouse)
- [ ] GUI window manager
- [ ] File system (FAT12)
- [ ] Built-in applications
- [ ] Sound support (PC speaker)

## Version History

See [CHANGELOG.md](CHANGELOG.md) for detailed version history.

Current version: **3.1.6**

## Contributing

This project is in active development. See the documentation in `docs/` for technical details.

## License

TBD

---

*UnoDOS 3 - Because sometimes the old ways are the best ways.*
