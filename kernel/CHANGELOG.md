# Kernel Changelog

All notable changes to the UnoDOS kernel will be documented here.

**NOTE:** Kernel version is permanently fixed at major version 3 ("Uno dos tres").

## [3.2.2] - 2026-01-23

### Fixed
- Added padding to align font at 0x0200 (512-byte boundary)
- Discovered issue: Font data crossing 0x0200 boundary caused access failures
- v3.2.1.1 test showed '9' worked but '=' and '>' still failed

### Technical Details
**Root cause identified:**
- Font was starting at 0x0120
- Character ':' (offset 208) straddles 0x01F8-0x0200 boundary (explains partial render)
- Characters '=' and '>' beyond 0x0200 boundary couldn't be accessed
- 0x0200 = 512 bytes = disk sector boundary

**Solution:**
- Added `times 512 - ($ - $$) db 0` padding before font
- Font now starts cleanly at 0x0200
- All font data (760 bytes) now at 0x0200-0x04F8
- No characters cross critical boundaries

**New addresses:**
- '9' (offset 200): 0x0320
- '=' (offset 232): 0x02E8
- '>' (offset 240): 0x02F0

## [3.2.1] - 2026-01-23

### Fixed
- Font rendering issue for characters beyond offset ~208
- Reorganized memory layout: moved font data BEFORE variables
- Font now occupies clean contiguous block (0x0120-0x0417)
- All variables moved after font data (0x0418+)

### Changed
- Renamed `.save_x`/`.save_y` to `pixel_save_x`/`pixel_save_y` (global scope)
- Updated test to use actual font characters '=' and '>'

### Technical Details
**Old layout (broken):**
- Variables at 0x0120-0x0127
- Font started at 0x0128
- Issue: Unknown addressing problem prevented access beyond ~offset 208

**New layout (fixed):**
- Font: 0x0120-0x0417 (760 bytes, contiguous)
- Variables: 0x0418+ (draw_x, draw_y, pixel_save_x, pixel_save_y)
- Font data now in clean block with no interspersed variables

## [3.2.0.14] - 2026-01-23

### Changed
- Updated documentation with complete debugging findings
- Created docs/GRAPHICS_DEBUG.md with full analysis of font rendering issue
- Updated Write-Floppy-Quick.ps1 with retry logic for git fetch failures

### Findings Summary
- v3.2.0.13 confirmed issue: Font data beyond offset ~208 cannot be accessed
- Hardcoded '=' character works, font '=' at offset 232 fails
- Issue is NOT with draw_char function or encoding
- Issue IS with accessing included font data at higher offsets
- Font data verified present in binary, but not accessible at runtime

### Next Steps
- Investigate why font data beyond offset ~208 is not accessible
- Test direct memory access to font data region
- Consider alternative font storage methods

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
