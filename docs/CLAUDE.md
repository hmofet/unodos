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

## Versioning (IMPORTANT - Claude MUST follow)

This project uses **Semantic Versioning** (MAJOR.MINOR.PATCH):
- **MAJOR**: Breaking changes or major new features
- **MINOR**: New features, backward compatible
- **PATCH**: Bug fixes, small improvements

**Claude MUST do the following with EVERY code change:**

1. **Update VERSION file** - Increment the appropriate version number
2. **Update CHANGELOG.md** - Add an entry documenting the changes
3. **Use versioned commit message** - Format: `vX.Y.Z: summary of change`

## Changelog (IMPORTANT - Claude MUST maintain)

## Documentation (IMPORTANT - Claude MUST follow)

### Document Locations

**Root directory (only these 3):**
- `README.md` - End-user/developer setup guide, nicely formatted for GitHub
- `CHANGELOG.md` - Extensive changelog, updated with every deployment and in between
- `VERSION` - Semantic version number

**docs/ directory:**
- `docs/SESSION_SUMMARY.md` - Session context for continuity between sessions
- `docs/CLAUDE.md` - Project context, workflow, preferences, long-running journal
- Other docs as needed (architecture, transcripts, technical summaries)

### When to Update

**CHANGELOG.md** - Most frequently updated. Update automatically as changes are made.

**docs/SESSION_SUMMARY.md** and **docs/CLAUDE.md** - Update automatically when:
- Major changes are made
- User says "update the docs", "I'm done for the night", "we'll pick this up later", or similar

**README.md** - Only when there are user-facing changes (features, usage, setup)

### "Update the docs" means:
1. `CHANGELOG.md` - Add any new changes
2. `docs/SESSION_SUMMARY.md` - Update with current session state
3. `docs/CLAUDE.md` - Update if significant project-level changes
4. Any docs in `docs/` that have significant topic updates

### Additional Documents
User may request architectural docs, technical summaries, or chat transcripts.
- Create/update these in `docs/` folder only
- Do not create automatically - only when requested

## Deploy Workflow (IMPORTANT)

When deploying changes, **always use a commit message**:

**Claude MUST provide:**
- `-m "vX.Y.Z: summary"` - Version number + short summary (REQUIRED)
- `-d "description"` - Detailed bullet-point description of all changes (REQUIRED)

**Before running deploy.sh, Claude MUST:**
1. Update `VERSION` file with the new version number
2. Update `CHANGELOG.md` with all changes made




The `CHANGELOG.md` file tracks all changes. Claude MUST update it with every change.

**Format:**
```markdown
## [X.Y.Z] - YYYY-MM-DD

### Added
- New features

### Changed
- Changes to existing features

### Fixed
- Bug fixes
```

**CRITICAL: Claude MUST always provide commit messages when deploying.**
- NEVER use `--no-commit` for final deployments
- ALWAYS use `-m "vX.Y.Z: summary"` with descriptive message
- ALWAYS use `-d "description"` with detailed bullet points

---

*Project initialized: 2026-01-22*