# Kernel Changelog

All notable changes to the UnoDOS kernel will be documented here.

**NOTE:** Kernel version is permanently fixed at major version 3 ("Uno dos tres").

## [3.2.0] - 2026-01-22

### Added
- Initial kernel separated from bootloader
- CGA 320x200 4-color graphics mode setup
- Welcome box with white border
- Text rendering with 8x8 font (currently debugging)

### Technical Details
- Kernel loaded at segment 0x1000 (linear 0x10000)
- Signature: 'UK' (0x4B55) at offset 0
- Entry point at offset 0x0002
