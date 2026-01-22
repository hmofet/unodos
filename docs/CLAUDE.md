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
│   ├── stage2.asm      # Stage 2: 8KB main loader
│   ├── font8x8.asm     # 8x8 bitmap font (95 chars)
│   └── font4x6.asm     # 4x6 small font (95 chars)
├── build/
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

### Completed (v3.1.6)
- [x] Two-stage boot loader
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
- DS is 0x0800 (Stage 2 data segment)
- Stack at SS:SP = 0x0000:0x7C00

### Display Compatibility Notes
- HP Omnibook 600C: Full 320x200 visible in CGA mode
- DSTN displays have ghosting - avoid fast animations
- 486 is much faster than 8088 - delays must be long

---

## Versioning (IMPORTANT)

This project uses **Semantic Versioning** (MAJOR.MINOR.PATCH):
- **MAJOR**: Breaking changes or major new features
- **MINOR**: New features, backward compatible
- **PATCH**: Bug fixes, small improvements

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

*Last updated: 2026-01-22 (v3.1.6)*
