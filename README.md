# UnoDOS

A graphical operating system for IBM PC XT-compatible computers.

## Overview

UnoDOS is a GUI-first operating system that runs on original PC XT hardware (or compatible emulators). Unlike traditional systems, it does not use DOS—instead, it interacts directly with BIOS and the Intel 8088 processor.

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

- **NASM** - Netwide Assembler for 8086/8088 assembly
- **QEMU** - For testing (with `-machine pc` for XT emulation)
- **make** - Build automation

### Build Commands

```bash
# Build floppy image
make

# Run in QEMU
make run

# Clean build artifacts
make clean
```

## Project Status

UnoDOS is in early development. See [CHANGELOG.md](CHANGELOG.md) for progress.

## License

TBD
