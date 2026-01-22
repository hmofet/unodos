# UnoDOS 3 - Development Context

This document provides context for Claude (AI assistant) when working on UnoDOS 3.

## Project Summary

UnoDOS 3 is a graphical operating system designed for IBM PC XT-compatible computers. Unlike traditional DOS-based systems, UnoDOS interacts directly with BIOS and the Intel 8088 processor, providing a GUI-first experience without any command-line interface.

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

## Architecture Principles

1. **GUI-First Design**: No command line - boots directly into graphical shell
2. **Direct Hardware Access**: Uses BIOS interrupts, not DOS
3. **Minimal Footprint**: Designed for 128KB RAM minimum
4. **Self-Contained**: Boots and runs entirely from floppy disk
5. **CGA-Only Graphics**: 320x200 4-color mode for maximum compatibility

## Current Project Structure

```
unodos/
├── boot/
│   ├── boot.asm        # Stage 1: 512-byte boot sector
│   ├── stage2.asm      # Stage 2: 2KB minimal loader
│   ├── font8x8.asm     # 8x8 bitmap font (95 chars)
│   └── font4x6.asm     # 4x6 small font (95 chars)
├── kernel/
│   └── kernel.asm      # Main OS kernel (16KB)
├── build/
│   ├── boot.bin        # Compiled boot sector
│   ├── stage2.bin      # Compiled stage2 loader
│   ├── kernel.bin      # Compiled kernel
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

### Completed (v3.2.0)
- [x] Three-stage boot architecture (boot + stage2 + kernel)
- [x] Separate 16KB kernel loaded at 64KB mark
- [x] Boot progress indicator (dots during kernel load)
- [x] Memory detection (INT 12h)
- [x] Video adapter detection (CGA/EGA/VGA)
- [x] CGA 320x200 4-color graphics
- [x] Custom bitmap fonts (8x8, 4x6)
- [x] Graphical welcome screen
- [x] Real-time clock display
- [x] RAM status display
- [x] Character demonstration

### Next Priority
- [ ] Keyboard input handling
- [ ] Basic window manager
- [ ] FAT12 file system (read)

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

### Memory Map
- 0x0000:7C00 - Boot sector (512 bytes)
- 0x0800:0000 - Stage2 loader (2KB)
- 0x1000:0000 - Kernel (16KB, expandable)
- 0xB800:0000 - CGA video memory (16KB)

### Display Compatibility Notes
- HP Omnibook 600C: Full 320x200 visible in CGA mode
- DSTN displays have ghosting - avoid fast animations
- 486 is much faster than 8088 - delays must be long

---

## Versioning (IMPORTANT)

This project uses a modified **Semantic Versioning** (3.MINOR.PATCH.HOTFIX):

**CRITICAL: The MAJOR version is permanently fixed at 3** ("Uno dos tres" - Spanish for 1, 2, 3). Never increment the major version without explicit user request.

- **MAJOR (3)**: Fixed at 3 - do NOT increment
- **MINOR**: Treat as major - increment for breaking changes, major new features, architectural changes
- **PATCH**: New features, bug fixes, improvements
- **HOTFIX** (optional): Small bug fixes, typos, minor corrections

**Claude MUST do the following with EVERY code change:**

1. **Update VERSION file** - Increment the appropriate version number
2. **Update CHANGELOG.md** - Add an entry documenting the changes
3. **Use versioned commit message** - Format: `vX.Y.Z: summary of change`

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

## Known Hardware Quirks

### HP Omnibook 600C
- DSTN display has significant ghosting
- Very fast compared to target 8088
- Full CGA 320x200 visible (no overscan)
- RTC works via INT 1Ah

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

*Last updated: 2026-01-22 (v3.2.0)*
