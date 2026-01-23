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

**Resolution (v3.2.1):**
- ✓ FIXED: Stage2 bootloader was corrupting BX register during progress output
- ✓ BIOS int 0x10 modified BX, which held the buffer pointer for loading sectors
- ✓ Only first sector (512 bytes) of kernel was being loaded
- ✓ Fix: Added push/pop BX around BIOS int 0x10 in stage2.asm
- ✓ All 32 sectors of kernel now load correctly
- ✓ Font data fully accessible, text rendering works

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

---

## Session: 2026-01-23 (Continued) - Architectural Planning

### Session Goal
Define comprehensive architecture for UnoDOS as a third-party app development platform.

### Major Architectural Decision
**Deferred "live memory display" feature** in favor of building foundation layer first.

**Reasoning:**
- Live display as kernel feature creates monolithic design
- Blocking delay loop prevents event-driven architecture
- Doesn't build reusable components for future apps
- Should be implemented as loadable application after foundation exists

### Architecture Approved: Hybrid System Call Model

**Key Decision:** Use INT 0x80 + Far Call Table hybrid approach

**Performance Analysis:**
- INT 0x80: ~50-70 cycles per call
- CALL FAR: ~28-35 cycles per call
- Savings: ~40 cycles per call (~9% CPU at 4.77MHz for graphics-heavy apps)
- Critical for graphical OS with hundreds of calls per frame

**Pattern:** Follows Windows 1.x/2.x, GEOS, early Mac OS
- INT 0x80 for discovery (one-time setup)
- Far Call Table for execution (frequent calls)
- Enables future protected mode transition via thunking

### Implementation Roadmap Defined

**Phase 1: Foundation Layer (v3.3.0 - v3.4.0)**
1. System call infrastructure (INT 0x80 + API table) - ~300 bytes
2. Graphics API abstraction - ~500 bytes
3. Memory allocator (malloc/free) - ~600 bytes
4. Keyboard driver - ~800 bytes
5. Event system - ~400 bytes
Total: ~2.8 KB, 15-18 hours

**Phase 2: Standard Library (v3.5.0)**
1. graphics.lib (C wrappers)
2. unodos.lib (initialization)
Total: 5-7 hours

**Phase 3: Core Services (v3.6.0 - v3.7.0)**
1. Window manager - 6-8 hours
2. FAT12 + App loader - 6-8 hours
Total: 12-16 hours

**Phase 4: Demo Application (v3.8.0)**
1. Clock display as loadable app - 2-3 hours

### Documentation Created
- docs/ARCHITECTURE_PLAN.md - Complete architectural analysis and roadmap
- Referenced docs/SYSCALL.md for performance analysis

---

## Session: 2026-01-23 (Continued) - Foundation 1.1 Implementation

### Version 3.3.0 Completed

**Foundation 1.1: System Call Infrastructure**
- ✓ INT 0x80 handler implemented
- ✓ Kernel API table at 0x1000:0x0500
- ✓ 10 stub functions created
- ✓ Visual test (displays "OK" on success)

**Hardware Testing:**
- Tested on HP Omnibook 600C (486DX4-75)
- INT 0x80 discovery mechanism: ✓ WORKING
- "OK" indicator displays correctly at bottom right
- API table magic number verified in binary

**Bug Fixed:**
- Typo in welcome message: "WELLCOME" → "WELCOME" (removed duplicate L)

---

## Session: 2026-01-23 (Continued) - Foundation 1.2 Implementation

### Version 3.4.0 Completed

**Foundation 1.2: Graphics API Abstraction**
- ✓ gfx_draw_pixel - Wraps plot_pixel_white
- ✓ gfx_draw_char - Character rendering with coordinates
- ✓ gfx_draw_string - Null-terminated string rendering
- ✓ gfx_draw_rect - Rectangle outline drawing
- ✓ gfx_draw_filled_rect - Filled rectangle drawing
- ✓ gfx_clear_area - Stub (returns immediately)

**Implementation Details:**
- All functions use register-based calling convention
- Preserve registers with pusha/popa
- Functions accessible via API table offsets 0-5
- Total added: ~400 bytes to kernel

**Calling Conventions:**
```asm
gfx_draw_pixel(CX=X, BX=Y, AL=color)
gfx_draw_char(BX=X, CX=Y, AL=ASCII)
gfx_draw_string(BX=X, CX=Y, SI=string_ptr)
gfx_draw_rect(BX=X, CX=Y, DX=width, SI=height)
gfx_draw_filled_rect(BX=X, CX=Y, DX=width, SI=height)
gfx_clear_area(BX=X, CX=Y, DX=width, SI=height)
```

**Hardware Testing (HP Omnibook 600C):**
- ✓ Visual tests display correctly
- ✓ Filled rectangle at X=20, Y=160 (60x30)
- ✓ Rectangle outline at X=90, Y=160 (60x30)
- ✓ Small filled square at X=160, Y=165 (10x10)
- ✓ String "API" displays correctly
- ✓ Characters "OK" display correctly
- ✓ All 6 graphics API functions working on real hardware

#### Version 3.5.0 - Memory Allocator (Foundation 1.3) (2026-01-23)

**Memory Allocator Implementation:**
- malloc(AX=size) → AX=pointer (offset from 0x1400:0000), 0 if failed
- free(AX=pointer) → frees memory block
- First-fit allocation algorithm
- Heap at 0x1400:0000, extends to ~640KB limit
- Block header structure: 4 bytes [size:2][flags:2]
  * size: Total block size including header
  * flags: 0x0000 (free) or 0xFFFF (allocated)
- 4-byte aligned allocations
- Automatic heap initialization on first malloc
- Initial heap block: ~60KB (0xF000 bytes)

**API Integration:**
- Added to API table at offsets 6, 7
- mem_alloc_stub and mem_free_stub implemented
- Applications use ES=0x1400 + offset for memory access

**Size Impact:**
- Memory allocator: ~600 bytes
- Kernel size: 16384 bytes (exactly at 16KB limit)
- Remaining capacity: 0 bytes - **KERNEL AT MAXIMUM**

**Critical Status:**
- Foundation 1.4 (Keyboard ~800 bytes) and 1.5 (Event ~400 bytes) cannot fit
- Kernel expansion required before continuing Foundation Layer

#### Version 3.6.0 - Kernel Expansion (2026-01-23)

**Kernel Size Expansion: 16KB → 24KB**
- Modified boot/stage2.asm: KERNEL_SECTORS 32 → 48
- Modified kernel/kernel.asm: Final padding 16384 → 24576
- Kernel binary verified: exactly 24576 bytes (24KB)

**Memory Layout Changes:**
- Kernel: 0x1000:0x0000 - 0x15FF:0x000F (24KB, was 16KB)
- Heap start: 0x1600:0000 (was 0x1400:0000)
- Available heap: ~532KB (was 540KB, loses 8KB)

**Rationale:**
- v3.5.0 reached exact 16KB capacity
- Foundation 1.4 and 1.5 require ~1200 bytes minimum
- 24KB expansion provides ~7KB headroom for future components
- 8KB heap reduction is negligible (532KB still ample for apps)
- Conservative expansion, can expand to 32KB later if needed

**Testing:**
- ✓ Build successful
- ✓ Kernel binary exactly 24576 bytes
- ✓ QEMU boot test passed
- Ready for Foundation 1.4 (Keyboard Driver)

### Next Immediate Task
**Foundation 1.4: Keyboard Driver**
- Scan code reading (Port 60h or INT 9h)
- Scan code → ASCII translation
- Modifier keys (Shift, Ctrl, Alt)
- Key buffer (16 keys)
- API table integration

### Current State (Updated 2026-01-23 - v3.6.0)
- **Bootloader Version**: 3.2.1 ✓
- **Kernel Version**: 3.6.0 ✓
- **Kernel Size**: 24KB (expanded from 16KB)
- **Available Heap**: ~532KB (was 540KB)
- **Architecture**: Three-stage boot (boot sector → stage2 → kernel)
- **Boot loader**: Fully functional, BX register bug fixed
- **System calls**: ✓ INT 0x80 + API table working
- **Graphics API**: ✓ 6 functions implemented and hardware tested
- **Memory Allocator**: ✓ malloc/free with first-fit algorithm
- **Welcome message**: "WELCOME TO UNODOS 3!" displays correctly
- **Test hardware**: HP Omnibook 600C (486DX4-75) - All tests passing
- **Foundation Layer Progress**: 3/5 complete (1.1, 1.2, 1.3 done; 1.4, 1.5 pending)
- **Status**: Kernel expanded to 24KB, ready for Foundation 1.4 (Keyboard Driver)

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
