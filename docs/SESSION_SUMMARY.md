# Session Summary

## Current Session: 2026-01-23

### Session Goal
Debug and fix font rendering issues in kernel after bootloader/kernel separation.

### Test Hardware
- **HP Omnibook 600C**: 486DX4-75 MHz, VGA (640x480 DSTN), 1.44MB floppy
- CGA mode emulated via VGA, confirmed full 320x200 visible

### Completed This Session

#### Version 3.2.0 - Kernel Separation & Font Debugging (2026-01-23)

**Major Architecture Change (v3.2.0):**
- Split kernel from stage2 bootloader
- Stage2 now minimal 2KB loader with progress indicator
- Kernel is separate 16KB binary loaded at 0x1000:0000 (64KB mark)
- Enables kernel to grow beyond previous 8KB limit

**Font Rendering Issue Discovered:**
After kernel separation, text rendering stopped working for characters beyond a certain offset in the font data.

**Systematic Debugging (v3.2.0.8 - v3.2.0.14):**
1. **v3.2.0.8**: Discovered boundary at offset 160-200
2. **v3.2.0.9**: Attempted fix with `word` qualifier (no change)
3. **v3.2.0.10**: Switched to direct label addressing (progress: boundary moved to 200-240)
4. **v3.2.0.11**: Critical clue - ':' character rendered partially (top dot only)
5. **v3.2.0.12**: Confirmed '=' and '>' at offsets 232/240 cannot be accessed
6. **v3.2.0.13**: **BREAKTHROUGH** - Hardcoded '=' works, font '=' fails
7. **v3.2.0.14**: Documentation and analysis

**Key Findings:**
- Font data IS present in binary (verified with hexdump)
- NASM addressing IS correct (verified in listings)
- `draw_char` function works correctly (hardcoded data renders)
- Font data accessible up to offset ~200
- Font data beyond offset ~208 NOT accessible at runtime
- Issue is with accessing included font data, not rendering logic

**Documentation Created:**
- docs/GRAPHICS_DEBUG.md - Complete debugging analysis
- Updated Write-Floppy-Quick.ps1 with git retry logic

**Current Status:**
- Root cause identified: Font data access issue beyond offset ~208
- Next step: Investigate why included font data not accessible at higher offsets

---

### Previous Session Accomplishments

#### Boot Loader (v0.2.0 - v3.0.0)
- Created GitHub repository: https://github.com/hmofet/unodos
- Implemented two-stage boot loader
  - Stage 1: 512-byte boot sector, loads Stage 2 via INT 13h
  - Stage 2: 8KB second stage with graphics and detection
- Memory detection (INT 12h)
- Video adapter detection (INT 11h equipment list)
- CGA 320x200 4-color graphics mode
- Custom bitmap fonts (8x8 and 4x6)
- MDA text-mode fallback (later removed)

#### Version 3.0.0 - 3.1.0
- Major version bump to UnoDOS 3
- Welcome screen: "WELCOME TO UNODOS 3!"
- Complete ASCII font set (characters 32-126)
- Generic text rendering functions

#### Version 3.1.1 - 3.1.2
- Character demonstration loop (cycles through ASCII)
- Real-time clock display (RTC via INT 1Ah)
- RAM status display (total/used/free)

#### Version 3.1.3 - 3.1.6 (Today's Focus)
- Debugging display visibility on HP Omnibook 600C
- Fixed ES segment register issues in clock/demo functions
- Confirmed full 320x200 CGA area visible on test hardware
- Slowed animations for 486/DSTN display compatibility
- Removed MDA support (CGA-only now)
- Clock at top-left (X=4, Y=4)
- Character demo below welcome box (Y=160)

#### Documentation
- Comprehensive README update
- Created ARCHITECTURE.md (boot process documentation)
- Created FEATURES.md (planned features roadmap)
- Updated CHANGELOG with all version history

### Build Tools Created
- Makefile with QEMU integration
- Linux: tools/writeflop.sh
- Windows: tools/writeflop.bat, Write-Floppy.ps1, Write-Floppy-Quick.ps1

### Known Issues
- Character demo runs too fast even with increased delays
- Need to tune animation speed for DSTN display ghosting

### Next Steps
1. Fix character demo animation (runs across screen, not just single character)
2. Implement keyboard input handling
3. Begin GUI window manager

### Current State
- **Version**: 3.2.0.14
- **Architecture**: Three-stage boot (boot sector → stage2 → kernel)
- **Boot loader**: Fully functional
- **Graphics**: CGA 320x200 working, welcome box displays
- **Text rendering**: DEBUGGING - font access issue beyond offset ~208
- **Known issue**: Cannot access included font data at higher offsets
- **Test hardware**: HP Omnibook 600C (486DX4-75)

### Key Decisions Made
1. GUI-first design (no command line)
2. Direct BIOS/8088 interaction (no DOS)
3. CGA 320x200 as primary graphics mode
4. Two-stage boot loader (512-byte boot + 8KB stage2)
5. Removed MDA support to simplify codebase
6. Target both vintage XT and modern 486 machines

### Hardware Findings (Omnibook 600C)
- VGA chip: Chips & Technologies 65545 (1MB VRAM)
- CGA emulation: Full 320x200 visible (no overscan issues)
- Display: DSTN with significant ghosting
- CPU: Fast enough that delays need to be very long
- Floppy: 1.44MB, works with Write-Floppy-Quick.ps1
