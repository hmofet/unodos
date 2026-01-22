# Bootloader Changelog

All notable changes to the UnoDOS bootloader (boot.asm and stage2.asm) will be documented here.

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
