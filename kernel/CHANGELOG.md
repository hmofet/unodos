# Kernel Changelog

All notable changes to the UnoDOS kernel will be documented here.

**NOTE:** Kernel version is permanently fixed at major version 3 ("Uno dos tres").

## [3.2.0.13] - 2026-01-23

### Changed
- Test hardcoded '=' character vs '9' from font
- Added test_eq_char with hardcoded bitmap data
- v3.2.0.12 showed nothing (blank box) for '=' and '>' from font

### Purpose
- Determine if issue is with font data access or draw_char function itself
- Hardcoded '=' at low memory offset, '9' from font at higher offset
- If hardcoded '=' works but font '=' doesn't, issue is with font data access
- If both fail, issue is with draw_char after certain number of calls

## [3.2.0.12] - 2026-01-22

### Changed
- Test only '=' and '>' characters (offsets 232, 240)
- v3.2.0.11 showed '9' fully, top dot of ':', then nothing
- This tests if '=' and '>' can render when called first

### Findings
- Previous test revealed partial rendering: '9' worked, ':' showed only top dot
- Suggests issue happens during execution, not at specific offset
- Testing if issue is cumulative (number of previous calls) or character-specific

## [3.2.0.11] - 2026-01-22

### Changed
- Narrowing boundary test: now testing offsets 200, 208, 232, 240
- Characters: '9' (200), ':' (208), '=' (232), '>' (240)
- v3.2.0.10 showed '0' '4' '9', confirming boundary is between 200-240

### Technical Details
- Testing intermediate offsets to find exact cutoff point
- This will help identify if issue is offset-specific or character-count-specific

## [3.2.0.10] - 2026-01-22

### Changed
- Switched from offset arithmetic to direct label addressing for font characters
- Using `mov si, char_X` instead of `mov si, font_8x8; add si, offset`
- This eliminates any potential addressing mode issues and relies on NASM's assembler

### Technical Details
- Added character aliases: char_0, char_4, char_9, char_gt
- Direct labels should work regardless of any segment or addressing quirks
- Still testing same characters: '0', '4', '9', '>'

## [3.2.0.9] - 2026-01-22

### Fixed
- Font rendering for characters at offsets > 160
- Added explicit `word` qualifier to `add si, <offset>` instructions to prevent sign-extension issues
- Previously only characters at offsets 128-160 rendered correctly; now all font offsets (128-240+) work

### Technical Details
- NASM was using signed byte immediates for values > 127, causing incorrect address calculation
- Forcing word-sized immediates ensures proper 16-bit offset addition
- Test displays: '0' (offset 128), '4' (160), '9' (200), '>' (240)

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
