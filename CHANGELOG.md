# Changelog

All notable changes to UnoDOS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [3.1.5] - 2026-01-22

### Fixed
- Fixed graphical corruption of version text caused by overlapping elements
  - Clock moved to Y=40 (above white box which starts at Y=50)
  - Character demo moved to Y=120 (inside box, below version at Y=106)
  - Demo now starts at X=65 to stay within box boundaries (X=60-260)

## [3.1.4] - 2026-01-22

### Fixed
- Clock and character demo now visible on HP Omnibook 600C
  - Clock moved to Y=60 (just above welcome message)
  - Character demo moved to Y=108 (just below version text)
  - Positions within known visible area (Y=4-106 confirmed visible)

### Changed
- Separated clock_loop as independent function
- Added main_loop to coordinate clock and demo updates

## [3.1.3] - 2026-01-22

### Fixed
- Character demo now visible on real hardware (moved from Y=165 to Y=130)
  - Accounts for display overscan on vintage hardware
  - Tested on HP Omnibook 600C

## [3.1.2] - 2026-01-22

### Added
- Real-time clock display in top left corner
  - Reads time from CMOS RTC via BIOS INT 1Ah, AH=02h
  - Displays HH:MM:SS format using 4x6 small font
  - Updates continuously during character demo loop
  - Falls back to "--:--:--" if RTC unavailable
- New functions: draw_clock, draw_bcd_small, clear_clock_area

## [3.1.1] - 2026-01-22

### Added
- Character demo at boot: cycles through all ASCII characters (32-126)
  - Displays characters horizontally at bottom of screen using 4x6 font
  - Clears and repeats in an infinite loop
  - Visual delay between characters for effect
- New functions: char_demo_loop, clear_demo_area, delay_short

### Changed
- Moved RAM status display from bottom right to top right corner

## [3.1.0] - 2026-01-22

### Added
- Complete ASCII bitmap font set (characters 32-126)
  - 8x8 font in boot/font8x8.asm (95 characters, 760 bytes)
  - 4x6 small font in boot/font4x6.asm (95 characters, 570 bytes)
- Generic text rendering functions for any ASCII string:
  - draw_string_8x8: Render null-terminated string with 8x8 font
  - draw_string_4x6: Render null-terminated string with 4x6 font
  - draw_ascii_8x8: Render single ASCII character with 8x8 font
  - draw_ascii_4x6: Render single ASCII character with 4x6 font
- Font tables accessible via font_8x8 and font_4x6 labels
- Legacy character aliases (char_H, char_E, etc.) maintained for compatibility

### Changed
- Font data moved from inline definitions to separate include files
- Makefile updated with NASM include path for font files

## [3.0.1] - 2026-01-22

### Added
- RAM status display in bottom right corner of screen
  - Shows total RAM (from BIOS INT 12h)
  - Shows estimated used memory (~10K for boot code)
  - Shows free memory (total - used)
- New 4x6 small character bitmaps: digits 1-9, R, A, M, K, U, F, s, e, d, r, colon
- draw_number_small function for rendering numbers with small font

## [3.0.0] - 2026-01-22

### Changed
- Major version bump to UnoDOS 3
- New startup message: "Welcome to UnoDOS 3!" with version number below
- Added 4x6 small font for version display
- New 8x8 character bitmaps: C, M, T, U, N, S, 3
- New 4x6 small character bitmaps: v, 3, 0, .

## [0.2.4] - 2026-01-22

### Fixed
- Graphics corruption bug: BIOS teletype output (INT 10h AH=0Eh) was being
  called after switching to CGA graphics mode, causing text to render as
  stray pixels in the top-left corner of the screen
- Removed post-graphics print_string call that caused the corruption
- Hello World graphics now display correctly with no stray pixels

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
