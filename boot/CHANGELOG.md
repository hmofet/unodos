# Bootloader Changelog

All notable changes to the UnoDOS bootloader (boot.asm and stage2.asm) will be documented here.

## [3.2.1] - 2026-01-23

### Fixed
- **CRITICAL BUG**: Stage2 now preserves BX register around BIOS int 0x10 calls
- Previously, BIOS int 0x10 (teletype output) could corrupt BX, which holds the buffer pointer
- This caused only the first sector of the kernel to load correctly, corrupting all subsequent sectors
- Fix: Added push/pop BX around progress dot printing

### Technical Details
- The bug manifested as kernel data being accessible only up to offset ~0x200 (512 bytes = 1 sector)
- Font data and other kernel code beyond the first sector appeared as zeros in memory
- Root cause: BX register corruption during progress indicator output
- Impact: Kernel was partially loaded, causing text rendering and other features to fail

## [3.2.0] - 2026-01-22

### Changed
- Split from combined bootloader+OS into separate bootloader and kernel
- Stage 1 (boot.asm): Loads 4 sectors (2KB) for stage2
- Stage 2 (stage2.asm): Minimal loader with dot progress indicator
- Kernel loaded at 0x1000:0000 (64KB mark), 32 sectors (16KB)

### Technical Details
- Boot sector loads stage2 to 0x0800:0000
- Stage2 verifies 'UN' signature, loads kernel, verifies 'UK' signature
- Progress dots printed during kernel load
