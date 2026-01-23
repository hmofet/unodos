# Changelog

All notable changes to UnoDOS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [3.8.0] - 2026-01-23

### Added
- **Foundation 1.4: Keyboard Driver** (Complete)
  - INT 09h keyboard interrupt handler with proper PIC EOI signaling
  - Scan code to ASCII translation tables (normal and shifted)
  - Modifier key state tracking (Shift, Ctrl, Alt)
  - 16-byte circular buffer for keyboard input
  - Non-blocking kbd_getchar() function (API offset 10)
  - Blocking kbd_wait_key() function (API offset 11)
  - Support for alphanumeric keys, punctuation, and special keys
  - Proper handling of key press and release events

- **Interactive Keyboard Demo** (Foundation Layer Integration Test)
  - Tests Graphics API + Keyboard Driver integration
  - Real-time keyboard input echo to screen
  - Displays prompt and instructions on boot
  - Handles special keys: ESC (exit), Enter (newline), Backspace (cursor back)
  - Auto-wraps at screen edges
  - Demonstrates API table function calls working in practice

### Technical Details
- INT 09h handler chains to original BIOS handler after processing
- Two 96-byte scan code translation tables (normal and shifted)
- Circular buffer prevents key loss during high-frequency input
- API table expanded: 10 → 12 function slots
- Estimated size: ~800 bytes (keyboard driver code + translation tables)
- Variables added: old_int9_offset/segment, kbd_buffer[16], buffer pointers, modifier states
- Interrupts enabled via STI after keyboard initialization

### Implementation
- install_keyboard(): Saves original INT 9h vector, installs handler, initializes buffer
- int_09_handler(): Reads scan code (port 0x60), tracks modifiers, translates to ASCII, stores in buffer
- kbd_getchar(): Returns next character from buffer (0 if empty)
- kbd_wait_key(): Blocks until key available, returns character
- Translation supports: A-Z, 0-9, punctuation, Escape, Backspace, Tab, Enter, Space

### Foundation Layer Progress
- ✓ System Call Infrastructure (v3.3.0)
- ✓ Graphics API (v3.4.0)
- ✓ Memory Allocator (v3.5.0)
- ✓ Kernel Expansion to 24KB (v3.6.0)
- ✓ Aggressive Optimization (v3.7.0)
- ✓ Keyboard Driver (v3.8.0)
- ⏳ Event System (v3.9.0 - Next)

## [3.7.0] - 2026-01-23

### Changed
- **Aggressive Kernel Optimization (Pre-Foundation 1.4/1.5)**
  - Removed test functions (test_int_80, test_graphics_api): ~200 bytes freed
  - Removed 21 character alias definitions (char_W, char_E, etc.)
  - Optimized all graphics API functions: replaced pusha/popa with targeted register saves
  - Optimized gfx_draw_rect_stub: eliminated ~80 bytes of redundant push/pop operations
  - Optimized plot_pixel_white: removed variable storage (pixel_save_x/y), stack-only implementation
  - Optimized setup_graphics: tighter BIOS call sequence
  - Optimized install_int_80: minimal register preservation
  - Welcome message now uses gfx_draw_string (string-based) instead of individual char draws

### Technical Details
- Kernel code: 2436 → 2416 bytes (20 bytes from optimization, ~200 from removal)
- Total space gained: ~220 bytes
- Available in 24KB kernel: **22,160 bytes** (sufficient for Foundation 1.4 + 1.5 + future features)
- Removed variables: pixel_save_x, pixel_save_y
- Removed test chars: test_W_char, test_eq_char
- Optimized functions maintain identical behavior, purely size/speed improvements

### Rationale
- Maximize space for Foundation 1.4 (Keyboard Driver ~800B) and 1.5 (Event System ~400B)
- Eliminate production overhead from debug/test code
- Optimize frequently-called graphics primitives
- Prepare for remaining Foundation Layer implementation

## [3.6.0] - 2026-01-23

### Changed
- **Kernel Expansion: 16KB → 24KB**
  - Kernel size increased from 16384 bytes (32 sectors) to 24576 bytes (48 sectors)
  - Provides headroom for Foundation 1.4 (Keyboard Driver, ~800 bytes) and Foundation 1.5 (Event System, ~400 bytes)
  - Heap start moved from 0x1400:0000 to 0x1600:0000
  - Available heap reduced from 540KB to 532KB (loses 8KB)

### Technical Details
- Modified boot/stage2.asm: KERNEL_SECTORS 32 → 48
- Modified kernel/kernel.asm: Final padding 16384 → 24576
- New memory layout:
  * 0x1000:0x0000 - Kernel (24KB, was 16KB)
  * 0x1600:0x0000 - Heap start (was 0x1400:0x0000)
  * ~532KB available for applications (was 540KB)
- Kernel headroom: ~7KB for future Foundation Layer components

### Rationale
- v3.5.0 reached exact 16KB capacity with Memory Allocator
- Foundation 1.4 and 1.5 require additional ~1200 bytes minimum
- 24KB expansion is conservative, leaves room for future enhancements
- 8KB heap reduction is negligible (still 532KB for apps)
- See docs/MEMORY_LAYOUT.md for detailed analysis

## [3.5.0] - 2026-01-23

### Added
- **Memory Allocator (Foundation 1.3)**
  - malloc(size): Allocate memory dynamically
  - free(ptr): Free allocated memory
  - First-fit allocation algorithm
  - Heap at 0x1400:0000, extends to ~640KB limit
  - Block header structure (size + flags)
  - Integrated with API table (offsets 6, 7)

### Technical Details
- Memory block header: 4 bytes [size:2][flags:2]
  * size: Total block size including header
  * flags: 0x0000 (free) or 0xFFFF (allocated)
- First-fit search algorithm for allocation
- Automatic heap initialization on first malloc
- Initial heap block: ~60KB (0xF000 bytes)
- 4-byte aligned allocations

### Implementation
- malloc(AX=size) → AX=pointer (offset from 0x1400:0000), 0 if failed
- free(AX=pointer) → frees memory block
- Heap starts at segment 0x1400 (linear 0x14000)
- Applications use ES=0x1400 + offset for memory access

### Size Impact
- Memory allocator: ~600 bytes
- Kernel size: Still 16KB (16384 bytes exact)
- Remaining capacity: ~0 bytes (at maximum)

## [3.4.0] - 2026-01-23

### Added
- **Graphics API Abstraction (Foundation 1.2)**
  - gfx_draw_pixel: Wraps plot_pixel_white for API table access
  - gfx_draw_char: Character rendering with coordinate parameters
  - gfx_draw_string: Null-terminated string rendering
  - gfx_draw_rect: Rectangle outline drawing
  - gfx_draw_filled_rect: Filled rectangle drawing
  - gfx_clear_area: Clear rectangular area (stub for now)

### Fixed
- Welcome message typo: "WELLCOME" → "WELCOME"

### Technical Details
- All graphics functions accessible via kernel API table
- Register-based calling convention:
  * gfx_draw_pixel(CX=X, BX=Y, AL=color)
  * gfx_draw_char(BX=X, CX=Y, AL=ASCII)
  * gfx_draw_string(BX=X, CX=Y, SI=string_ptr)
  * gfx_draw_rect(BX=X, CX=Y, DX=width, SI=height)
  * gfx_draw_filled_rect(BX=X, CX=Y, DX=width, SI=height)
- Kernel size: Still within 16KB limit

### Hardware Testing
- Tested on HP Omnibook 600C (486DX4-75)
- INT 0x80 discovery working ✓
- "OK" indicator displays correctly ✓

## [3.3.0] - 2026-01-23

### Added
- **System Call Infrastructure (Foundation 1.1)**
  - INT 0x80 handler for system call discovery mechanism
  - Kernel API table at fixed address 0x1000:0x0500
  - Hybrid approach: INT 0x80 for discovery + Far Call Table for execution
  - API table header with magic number ('KA' = 0x4B41), version (1.0), function count
  - 10 stub functions for future implementation:
    * Graphics API (6 functions): draw_pixel, draw_rect, draw_filled_rect, draw_char, draw_string, clear_area
    * Memory management (2 functions): malloc, free
    * Event system (2 functions): get_event, wait_event
- Visual test for INT 0x80 - displays "OK" at bottom right if successful

### Technical Details
- API table positioned at exactly offset 0x0500 (verified in binary)
- Follows Windows 1.x/2.x, GEOS pattern for performance
- Far Call approach saves ~40 cycles per call vs pure INT approach (~9% CPU at 4.77MHz)
- Foundation for third-party application development
- Enables future protected mode transition via thunking

### Documentation
- Added docs/ARCHITECTURE_PLAN.md - Complete architectural analysis and roadmap
- Added docs/SYSCALL.md - System call performance analysis
- Updated README.md with phase-based feature roadmap
- Updated docs/SESSION_SUMMARY.md with architectural decisions

## [3.2.0] - 2026-01-22

### Changed
- **Major architectural change: Split kernel from stage2 loader**
  - Stage2 is now a minimal 2KB loader with progress indicator
  - Kernel is a separate 16KB binary loaded at 0x1000:0000 (64KB mark)
  - Enables kernel to grow beyond 8KB limit
  - Future-proof architecture for XT through 486 hardware
- Boot sequence now shows "Loading kernel" with dot progress bar
- Removed text-mode debug output from bootloader (cleaner boot)
- RAM display now shows correct memory usage (~20KB for loader+kernel)

### Added
- New kernel/ directory for OS code
- kernel/kernel.asm - Main operating system (16KB)
- Kernel signature verification ('UK' = 0x4B55)

### Technical Details
- Disk layout: Boot (1 sector) + Stage2 (4 sectors) + Kernel (32 sectors)
- Stage2 loads kernel sector-by-sector with progress indicator
- Kernel loaded at segment 0x1000 (linear address 0x10000)

## [3.1.7] - 2026-01-22

### Changed
- Character demo redesigned for ASCII verification
  - Displays all 95 printable ASCII characters (32-126) in a 2-row grid
  - Row 1: characters 32-79 (48 chars) at Y=160
  - Row 2: characters 80-126 (47 chars) at Y=168
  - All characters remain visible during long pause (~100 delay cycles)
  - Then clears and repeats for continuous verification
- Clear demo area expanded to 16 pixels height (Y=160-175) for 2 rows

## [3.1.6] - 2026-01-22

### Fixed
- Clock and character demo now visible on HP Omnibook 600C
  - Added ES segment initialization in draw_clock and char_demo_loop
  - ES must point to 0xB800 (CGA video memory) for pixel plotting
- Slowed down animations for 486/DSTN display compatibility
  - Increased delay_short to nested loop (~4 million iterations)
  - Character demo now visible on slow DSTN displays with ghosting

### Changed
- Clock moved to top-left corner (X=4, Y=4)
- Character demo moved below welcome box (Y=160)
- Removed MDA text mode support (CGA-only now)
- Simplified video detection (no longer tracks video_type)

### Added
- Comprehensive coordinate visibility testing
- Confirmed full 320x200 CGA area visible on Omnibook 600C
- New documentation: ARCHITECTURE.md, FEATURES.md
- Comprehensive README update

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
