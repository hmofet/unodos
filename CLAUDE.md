# UnoDOS - GUI Operating System for PC XT

## Project Summary

UnoDOS is a graphical operating system designed for IBM PC XT-compatible computers. Unlike traditional DOS-based systems, UnoDOS interacts directly with BIOS and the Intel 8088 processor, providing a GUI-first experience without any command-line interface.

## Target Hardware Specifications

- **CPU**: Intel 8088 (4.77 MHz)
- **RAM**: 128 KB minimum
- **Display**: MDA (Monochrome Display Adapter) and CGA (Color Graphics Adapter)
- **Storage**: 5.25" Floppy Drive (360KB) - serves as both installation and runtime media
- **BIOS**: IBM PC/XT compatible

## Architecture Principles

1. **GUI-First Design**: No command line - the system boots directly into a graphical shell
2. **Direct Hardware Access**: Uses BIOS interrupts and direct 8088 instructions, not DOS
3. **Minimal Footprint**: Designed to run efficiently in 128KB RAM
4. **Self-Contained**: Boots and runs entirely from floppy disk

## Project Structure

```
/boot           - Boot loader (512-byte boot sector)
/kernel         - Core kernel code
/drivers        - Hardware drivers (display, keyboard, floppy)
/gui            - Graphical shell and window manager
/apps           - Built-in applications
/tools          - Build tools and utilities
```

## Development Status

- [ ] Boot loader
- [ ] Kernel initialization
- [ ] Memory management
- [ ] Display drivers (MDA/CGA)
- [ ] Keyboard driver
- [ ] Floppy driver
- [ ] GUI shell
- [ ] File system
- [ ] Applications

## Build Requirements

- NASM (Netwide Assembler) for 8086/8088 assembly
- QEMU with 8086 emulation for testing
- dd or similar for floppy image creation

## Technical Notes

### Memory Map (128KB System)
```
0x00000 - 0x003FF  Interrupt Vector Table (1KB)
0x00400 - 0x004FF  BIOS Data Area (256 bytes)
0x00500 - 0x07BFF  Free conventional memory (~30KB)
0x07C00 - 0x07DFF  Boot sector load address (512 bytes)
0x07E00 - 0x1FFFF  Free conventional memory (~96KB)
```

### Display Modes
- **MDA**: 80x25 text mode, monochrome, no graphics
- **CGA**: 320x200 4-color, 640x200 2-color, 80x25/40x25 text

### Key BIOS Interrupts
- INT 10h - Video services
- INT 13h - Disk services
- INT 16h - Keyboard services
- INT 19h - Bootstrap loader

---

*Project initialized: 2026-01-22*
