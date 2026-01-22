# Changelog

All notable changes to UnoDOS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.3] - 2026-01-22

### Added
- Pre-built floppy images now included in repository
  - build/unodos.img (360KB)
  - build/unodos-144.img (1.44MB)

### Changed
- Updated .gitignore to track final images, ignore intermediate build files

## [0.2.2] - 2026-01-22

### Fixed
- QEMU boot compatibility: Changed machine type from `-machine pc` to `-M isapc`
  for proper PC/XT BIOS boot behavior
- Boot now works correctly in QEMU with graphical Hello World display

### Changed
- Makefile now uses `isapc` machine type for all QEMU targets

## [0.2.1] - 2026-01-22

### Added
- Windows floppy write utilities
  - tools/writeflop.bat - Batch script for Windows command prompt
  - tools/Write-Floppy.ps1 - PowerShell script with verification
  - Both support 360KB and 1.44MB images
  - Require Administrator privileges for raw disk access

## [0.2.0] - 2026-01-22

### Added
- Boot sector (boot/boot.asm) - 512-byte IBM PC compatible boot loader
  - Loads from floppy drive (BIOS INT 13h)
  - Debug messages during boot process
  - Loads 8KB second stage from sectors 2-17
  - Validates second stage signature before jumping
- Second stage loader (boot/stage2.asm)
  - Memory detection via INT 12h
  - Video adapter detection (MDA/CGA/EGA/VGA)
  - CGA 320x200 4-color graphics mode
  - Graphical "HELLO WORLD!" with custom 8x8 bitmap font
  - MDA fallback with text-mode box drawing
- Build system (Makefile)
  - `make` - Build 360KB floppy image
  - `make floppy144` - Build 1.44MB floppy image
  - `make run` / `make run144` - Test in QEMU
  - `make debug` - QEMU with monitor for debugging
  - `make sizes` - Show binary sizes
  - Dependency checking for nasm and qemu
- Floppy write utility (tools/writeflop.sh)
  - Write images to physical floppy disks
  - Supports both 360KB and 1.44MB formats
  - Verification after write
  - Safety checks to prevent accidental overwrites

## [0.1.0] - 2026-01-22

### Added
- Initial project setup
- Project documentation (CLAUDE.md) with target specifications
- Documentation structure (docs/, VERSION, CHANGELOG.md, README.md)
- Target hardware: Intel 8088, 128KB RAM, MDA/CGA displays, floppy drive
- Architecture defined: GUI-first OS with direct BIOS interaction, no DOS dependency
