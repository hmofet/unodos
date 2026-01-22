# UnoDOS 3

A graphical operating system for IBM PC XT-compatible computers.

## Overview

UnoDOS 3 is a GUI-first operating system that runs on original PC XT hardware (or compatible emulators). Unlike traditional systems, it does not use DOS—instead, it interacts directly with BIOS and the Intel 8088 processor.

## Target Hardware

| Component | Specification |
|-----------|---------------|
| CPU | Intel 8088 @ 4.77 MHz |
| RAM | 128 KB minimum |
| Display | MDA or CGA |
| Storage | 5.25" Floppy Drive (360KB) |
| BIOS | IBM PC/XT compatible |

## Building

### Requirements

```bash
# Ubuntu/Debian
sudo apt install nasm qemu-system-x86

# Fedora
sudo dnf install nasm qemu-system-x86

# Arch
sudo pacman -S nasm qemu
```

### Build Commands

```bash
# Build 360KB floppy image (target platform)
make

# Build 1.44MB floppy image (for modern testing)
make floppy144

# Run in QEMU
make run

# Run with 1.44MB image
make run144

# Debug mode (QEMU monitor)
make debug

# Show binary sizes
make sizes

# Clean build artifacts
make clean
```

### Writing to Real Floppy

```bash
# Write to floppy drive (auto-detects image)
./tools/writeflop.sh /dev/fd0

# Write 1.44MB image to USB floppy
./tools/writeflop.sh -1 /dev/sdb
```

## Current Status

- [x] Boot sector with debug output
- [x] Second stage loader
- [x] Memory detection
- [x] Video adapter detection (MDA/CGA/EGA)
- [x] CGA graphics mode (320x200)
- [x] Graphical welcome screen ("Welcome to UnoDOS 3!")
- [x] Custom 8x8 and 4x6 bitmap fonts
- [ ] Keyboard input
- [ ] GUI shell
- [ ] File system

## Project Status

UnoDOS 3 is in early development. See [CHANGELOG.md](CHANGELOG.md) for progress.

## License

TBD
