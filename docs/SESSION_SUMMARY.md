# Session Summary

## Current Session: 2026-01-22

### Session Goal
Build a GUI operating system for PC XT-class machines with direct BIOS interaction.

### Completed This Session
- Created GitHub repository: https://github.com/hmofet/unodos
- Set up project documentation structure
- Defined target hardware specifications
- Established versioning and changelog workflow
- **Implemented boot loader (boot/boot.asm)**
  - 512-byte boot sector
  - Loads second stage from floppy via INT 13h
  - Debug messages during boot
  - Signature validation before jump
- **Implemented second stage loader (boot/stage2.asm)**
  - Memory detection (INT 12h)
  - Video adapter detection (MDA/CGA/EGA/VGA)
  - CGA 320x200 graphics mode
  - Custom 8x8 bitmap font for main text
  - Custom 4x6 small bitmap font for version display
  - MDA text-mode fallback
- **Created build system (Makefile)**
  - Builds 360KB and 1.44MB floppy images
  - QEMU integration for testing (using `-M isapc`)
  - Debug mode with QEMU monitor
- **Created floppy write utilities**
  - tools/writeflop.sh (Linux)
  - tools/writeflop.bat (Windows CMD)
  - tools/Write-Floppy.ps1 (Windows PowerShell)
- **Fixed graphics corruption bug (v0.2.4)**
  - BIOS teletype output was rendering as stray pixels after graphics mode switch
- **Major version bump to UnoDOS 3 (v3.0.0)**
  - New welcome message: "WELCOME TO UNODOS 3!"
  - Version number displayed in smaller font below

### Next Steps
- Add keyboard input handling
- Begin GUI shell implementation
- File system support

### Current State
- Version: 3.0.0
- Boot loader complete with graphical welcome screen
- Tested working in QEMU

### Key Decisions Made
1. GUI-first design (no command line)
2. Direct BIOS/8088 interaction (no DOS)
3. Target: 128KB RAM, MDA/CGA, floppy boot
4. Semantic versioning with CHANGELOG.md tracking
5. Two-stage boot loader (512-byte boot + 8KB stage2)
6. CGA 320x200 as primary graphics mode, MDA text fallback
7. Major version 3.x.y series (user approval required for v4.0.0+)
