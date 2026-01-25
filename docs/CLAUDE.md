# UnoDOS 3 - Development Context (v3.12.0)

This document provides context for Claude (AI assistant) when working on UnoDOS 3.

**Last Updated:** 2026-01-25
**Current Version:** 3.12.0 Build 014 (Window Manager Complete)

## Project Summary

UnoDOS 3 is a graphical operating system designed for IBM PC XT-compatible computers. Unlike traditional DOS-based systems, UnoDOS interacts directly with BIOS and the Intel 8088 processor, providing a GUI-first experience without any command-line interface.

**Current Status:** Foundation Layer complete with working FAT12 filesystem driver. Can boot, display graphics, handle keyboard input, manage memory, and read files from floppy disks.

## Target Hardware Specifications

### Minimum (IBM PC XT)
- **CPU**: Intel 8088 (4.77 MHz)
- **RAM**: 128 KB
- **Display**: CGA (Color Graphics Adapter)
- **Storage**: 5.25" Floppy Drive (360KB)
- **BIOS**: IBM PC/XT compatible

### Test Hardware (HP Omnibook 600C)
- **CPU**: Intel 486DX4-75 MHz
- **RAM**: 8-40 MB
- **Display**: VGA 640x480 DSTN (CGA emulated)
- **Storage**: 1.44MB floppy drive
- **Graphics Chip**: Chips & Technologies 65545
- **Status**: Tested successfully, hardware compatibility fixes applied

## Architecture Principles

1. **GUI-First Design**: No command line - boots directly into graphical shell
2. **Direct Hardware Access**: Uses BIOS interrupts, not DOS
3. **Minimal Footprint**: Designed for 128KB RAM minimum
4. **Self-Contained**: Boots and runs entirely from floppy disk
5. **CGA-Only Graphics**: 320x200 4-color mode for maximum compatibility
6. **Event-Driven**: Circular event queue with keyboard integration
7. **Modular Architecture**: Pluggable filesystem drivers for future expansion

## Current Project Structure

```
unodos/
├── boot/
│   ├── boot.asm        # Stage 1: 512-byte boot sector
│   ├── stage2.asm      # Stage 2: 2KB minimal loader (loads 56 sectors)
│   ├── font8x8.asm     # 8x8 bitmap font (95 chars)
│   └── font4x6.asm     # 4x6 small font (95 chars)
├── kernel/
│   ├── kernel.asm      # Main OS kernel (28KB, expanded for FAT12)
│   └── VERSION         # Current version: 3.10.0
├── build/
│   ├── boot.bin        # Compiled boot sector
│   ├── stage2.bin      # Compiled stage2 loader
│   ├── kernel.bin      # Compiled kernel (28KB)
│   ├── unodos.img      # 360KB floppy image
│   └── unodos-144.img  # 1.44MB floppy image
├── docs/
│   ├── ARCHITECTURE.md # Boot process documentation
│   ├── FEATURES.md     # Planned features roadmap
│   ├── CLAUDE.md       # This file
│   └── SESSION_SUMMARY.md
├── tools/
│   ├── writeflop.sh    # Linux floppy writer
│   ├── writeflop.bat   # Windows CMD writer
│   ├── Write-Floppy.ps1       # PowerShell with verify
│   └── Write-Floppy-Quick.ps1 # PowerShell quick write
├── Makefile
├── README.md
├── CHANGELOG.md
└── VERSION
```

## Development Status

### Foundation Layer Complete (v3.3.0 - v3.10.0) ✅

**All core infrastructure is now in place:**

- [x] Three-stage boot architecture (boot + stage2 + kernel)
- [x] Separate 28KB kernel loaded at 64KB mark (expanded from 16KB → 24KB → 28KB)
- [x] Boot progress indicator (dots during kernel load)
- [x] Memory detection (INT 12h)
- [x] Video adapter detection (CGA/EGA/VGA)
- [x] CGA 320x200 4-color graphics
- [x] Custom bitmap fonts (8x8, 4x6)
- [x] Graphical welcome screen
- [x] **System Call Infrastructure (v3.3.0)** - INT 0x80 + API dispatch
- [x] **Graphics API (v3.4.0)** - 6 functions + inverted text
- [x] **Memory Allocator (v3.5.0)** - malloc/free with free list management
- [x] **Kernel Optimization (v3.7.0)** - Aggressive optimization, ~220 bytes freed
- [x] **Keyboard Driver (v3.8.0)** - INT 09h handler, scan code translation, circular buffer
- [x] **Event System (v3.9.0)** - 32-event circular queue, event-driven architecture
- [x] **Filesystem Abstraction + FAT12 (v3.10.0)** - VFS-like interface, complete FAT12 driver
- [x] **Hardware compatibility** - Tested on HP Omnibook 600C, fixes applied

### Core Services (v3.11.0 - v3.12.0) ✅

- [x] **Application Loader (v3.11.0)** - Load/run .BIN files from FAT12
- [x] **Window Manager (v3.12.0)** - Create windows with title bar and border
- [x] **INT 0x80 Dispatch (v3.12.0)** - Apps call kernel via interrupt

### Applications

- [x] hello.bin - Minimal test app (112 bytes)
- [x] clock.bin - Clock display app (300 bytes) - READY FOR TESTING

### Next Steps (v3.13.0+)
- [ ] **Mouse Support (v3.13.0)** - PS/2 mouse driver
- [ ] **Window Dragging (v3.14.0)** - Mouse drag to move windows
- [ ] **Enhanced Filesystem (v3.15.0)** - Multi-cluster reads, subdirectories, seek
- [ ] **Standard Library (v3.16.0)** - graphics.lib, unodos.lib for C development

## Technical Notes

### CGA Memory Layout
- Video memory at 0xB800:0000
- Mode 4: 320x200, 4 colors
- Interlaced: even lines at 0x0000, odd at 0x2000
- 2 bits per pixel, 4 pixels per byte

### Segment Register Convention
- ES must be 0xB800 when calling pixel/drawing functions
- DS is 0x1000 (Kernel data segment)
- Stack at SS:SP = 0x0000:0x7C00
- Kernel API table at fixed address 0x1000:0x0800

### Memory Map (v3.12.0)
```
0x0000:0x0000  IVT, BIOS Data                    1 KB
0x0000:0x0500  Free/Temporary                    ~30 KB
0x0000:0x7C00  Boot sector                       512 bytes
0x0800:0x0000  Stage2 loader                     2 KB (loads 56 sectors)
0x1000:0x0000  Kernel signature + code           ~2.3 KB (to 0x900)
0x1000:0x0900  Kernel API table (fixed address)  256 bytes (25 functions)
0x1000:0x0A00  Kernel code continues             ~22 KB available
0x1400:0x0000  Heap start (malloc pool)          ~608 KB
0xB800:0000    CGA video memory                  16 KB
```

### Kernel Size Budget
- **Total allocation:** 28,672 bytes (28 KB, 56 sectors)
- **Currently used:** ~5,100 bytes (18%)
- **Free space:** ~23,600 bytes (82%)
- **Safe headroom:** Plenty of room for Core Services

### API Table (v3.12.0)
**Location:** 0x1000:0x0900 (fixed address, moved from 0x800 in Build 014)
**Discovery:** Apps use INT 0x80 with AH=0 to get table pointer
**Dispatch:** Apps use INT 0x80 with AH=function_index to call functions
**Functions (25 total):**
- 0-5: Graphics API (pixel, rect, filled_rect, char, string, clear)
- 6-7: Memory Management (malloc, free)
- 8-9: Event System (get_event, wait_event)
- 10-11: Keyboard Input (getchar, wait_key)
- 12-16: Filesystem (mount, open, read, close, register_driver)
- 17-18: Application Loader (load, run)
- 19-24: Window Manager (create, destroy, draw, focus, move, get_content)

### Display Compatibility Notes
- HP Omnibook 600C: Full 320x200 visible in CGA mode ✅ TESTED
- DSTN displays have ghosting - avoid fast animations
- 486 is much faster than 8088 - delays must be long
- Hardware compatibility fixes applied for floppy drives (disk reset, spin-up delays, retries)

### FAT12 Filesystem (v3.10.0)

**Current Capabilities:**
- Mount FAT12 filesystems (360KB and 1.44MB floppies)
- Open files by name (8.3 filename format: "FILENAME.EXT")
- Read file contents (single cluster = 512 bytes per read)
- Close files properly
- Root directory only (no subdirectories)
- Drive A: support

**Implementation Details:**
- BPB (BIOS Parameter Block) parsing
- Boot sector validation (0xAA55 signature, 512-byte sectors)
- Root directory search (up to 224 entries on 360KB floppy)
- Cluster-to-sector address translation
- File handle table (16 simultaneous open files)
- Mount table (4 filesystem mounts)
- Hardware retry logic (3 attempts with delays)
- Disk system reset on floppy swap

**Known Limitations (intentional for v3.10.0):**
- ⚠️ **Read-only** - No write/create/delete support (Future: v4.0.0+)
- ⚠️ **Single cluster** - Max 512 bytes per read (Future: v3.13.0)
- ⚠️ **No seek** - Always reads from position 0 (Future: v3.13.0)
- ⚠️ **Root directory only** - No subdirectories (Future: v3.13.0)
- ⚠️ **8.3 filenames** - No long filename support (Future: v4.1.0+)
- ⚠️ **Drive A: only** - Single drive (Future: v3.11.0)

**Interactive Testing:**
- Boot UnoDOS → Keyboard demo
- Press 'F' or 'f' → Filesystem test starts
- Prompt to insert test floppy with TEST.TXT
- System waits for keypress (allows floppy swap)
- Displays mount/open/read status
- Shows file contents on screen

**Three-Tier Architecture (Planned):**
1. **Tier 1:** Single floppy boot (current) - FAT12 built-in
2. **Tier 2:** Multi-floppy system - Load FAT16/FAT32 drivers from Floppy 2
3. **Tier 3:** HDD installation - Install OS to hard disk with installer tool

---

## Versioning (IMPORTANT)

This project maintains **separate version numbers** for the bootloader and kernel:

- **Bootloader version**: `boot/VERSION` - For boot.asm and stage2.asm changes
- **Kernel version**: `kernel/VERSION` - For kernel.asm changes
- **Changelogs**: `boot/CHANGELOG.md` and `kernel/CHANGELOG.md`

### Version Format

Uses **Semantic Versioning** (MAJOR.MINOR.PATCH.HOTFIX):

**Bootloader:** Standard semver - can go above major version 3

**Kernel:** MAJOR version permanently fixed at 3 ("Uno dos tres" - Spanish for 1, 2, 3). Never increment kernel major version without explicit user request.

- **MAJOR**: Bootloader can increment; Kernel fixed at 3
- **MINOR**: Treat as major for kernel - increment for breaking changes, major new features, architectural changes
- **PATCH**: New features, bug fixes, improvements
- **HOTFIX** (optional): Small bug fixes, typos, minor corrections

### Claude MUST do the following with code changes:

**For bootloader changes (boot.asm, stage2.asm):**
1. Update `boot/VERSION`
2. Update `boot/CHANGELOG.md`

**For kernel changes (kernel.asm):**
1. Update `kernel/VERSION`
2. Update `kernel/CHANGELOG.md`

**Commit message format:** `[boot|kernel] vX.Y.Z: summary of change`

---

## Documentation Guidelines

### Document Locations

**Root directory:**
- `README.md` - User/developer setup guide
- `CHANGELOG.md` - Version history (update with every change)
- `VERSION` - Semantic version number

**docs/ directory:**
- `ARCHITECTURE.md` - Technical boot process documentation
- `FEATURES.md` - Planned features roadmap
- `SESSION_SUMMARY.md` - Session context for continuity
- `CLAUDE.md` - This file (development context)

### When to Update

- **CHANGELOG.md**: Every code change
- **VERSION**: Every release
- **SESSION_SUMMARY.md**: End of session or major milestones
- **CLAUDE.md**: When project-level decisions change
- **README.md**: User-facing feature changes

---

## Commit Message Format

```
vX.Y.Z: Short summary

- Detailed change 1
- Detailed change 2

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
```

---

## Testing Workflow

**IMPORTANT**: When the user needs to test on hardware (HP Omnibook 600C), Claude MUST:
1. Build the project (`make clean && make && make floppy144`)
2. Commit and push to GitHub so binaries are available for download
3. The user will download from GitHub and write to floppy for testing

Testing always happens on the Omnibook unless otherwise specified. Always push before asking the user to test.

## Windows Development Constraints

**CRITICAL**: The Windows machine used for writing floppies **cannot build images**.
- No NASM or Python installed on Windows
- All `.img` files must be **pre-built here** and pushed to Git
- PowerShell scripts (e.g., `boot.ps1`, `clock.ps1`) pull from Git and write to floppy
- **Never** create a Windows script that tries to build - it will fail
- Always ensure `build/*.img` files are committed and pushed before testing

---

## Known Hardware Quirks

### HP Omnibook 600C
- DSTN display has significant ghosting
- Very fast compared to target 8088
- Full CGA 320x200 visible (no overscan)
- RTC works via INT 1Ah
- **IMPORTANT: Only has drive A: (floppy) and drive C: (HDD) - NO drive B:**
  - Apps must be loaded from drive A: after disk swap
  - The 'L' key loads HELLO.BIN from drive A:
  - For testing apps, boot from UnoDOS, press 'L', swap disk, press key

### IBM PC XT (Target)
- 4.77 MHz is very slow
- May have CGA snow on original cards
- 360KB floppy is primary storage
- May not have RTC (show --:--)

---

## License

This project uses the **UnoDOS License** (see `/LICENSE.md` in project root):
- **Distribution allowed**: Original, unmodified form only
- **No modifications**: Derivatives and alterations are NOT permitted
- **Attribution required**: Must credit original author (Arin Bakht) and link to source

---

## Recent Issues (RESOLVED)

### Font Rendering Problem (v3.2.0 - v3.2.1) - FIXED ✓

**Issue**: After separating kernel from bootloader (v3.2.0), font rendering only worked for characters at offset 0-200. Characters beyond offset ~208 appeared as zeros in memory.

**Root Cause**: Critical bug in stage2 bootloader - BIOS int 0x10 (teletype output for progress dots) was corrupting the BX register, which held the buffer pointer for loading kernel sectors. This caused only the first sector (512 bytes) of the kernel to load correctly.

**Fix**: Bootloader v3.2.1 now preserves BX register with push/pop around all BIOS int 0x10 calls.

**See**: [docs/GRAPHICS_DEBUG.md](GRAPHICS_DEBUG.md) for complete debugging analysis and investigation timeline.

**Status**: ✓ RESOLVED in bootloader v3.2.1 (2026-01-23)

---

*Last updated: 2026-01-25 (kernel v3.12.0 Build 014)*
