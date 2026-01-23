# Graphics Display Debugging - Font Rendering Issue

## Problem Summary

After separating the kernel from the bootloader (v3.2.0), text rendering stopped working correctly. Characters from the included font file beyond a certain offset would not display.

## Test Hardware

- **HP Omnibook 600C**: Intel 486DX4-75 MHz, VGA (CGA mode), 1.44MB floppy
- CGA 320x200 4-color mode
- Font data included via `%include "font8x8.asm"` with `-I boot/` flag

## Debugging Timeline

### v3.2.0.8: Initial Problem Discovery
- **Test**: Characters at offsets 128, 160, 200, 240
- **Expected**: `0 4 9 >`
- **Actual**: Only `0` and `4` displayed
- **Conclusion**: Boundary between offset 160 (works) and 200 (fails)

### v3.2.0.9: Attempt to Fix with `word` Qualifier
- **Change**: Added explicit `word` qualifier to `add si, <offset>` instructions
- **Reasoning**: Suspected signed byte vs word immediate encoding issue
- **Result**: No change - still only `0` and `4`
- **Analysis**: Machine code showed correct encoding, so not an assembly issue

### v3.2.0.10: Switch to Direct Label Addressing
- **Change**: Used `mov si, char_9` instead of `mov si, font_8x8; add si, 200`
- **Reasoning**: Eliminate any offset calculation issues
- **Result**: Progress! Now saw `0 4 9` - boundary moved to 200-240
- **Conclusion**: Direct addressing helped but didn't solve the core issue

### v3.2.0.11: Critical Clue - Partial Rendering
- **Test**: Characters at offsets 200, 208, 232, 240 (`9 : = >`)
- **Expected**: `9 : = >`
- **Actual**: `9` fully rendered, `:` showed only TOP DOT, nothing after
- **BREAKTHROUGH**: Partial rendering of `:` proved issue occurs DURING execution
- **Conclusion**: Not a simple offset boundary - something corrupts mid-render

### v3.2.0.12: Isolation Test
- **Test**: Only `=` and `>` (offsets 232, 240) without prior characters
- **Expected**: `= >`
- **Actual**: BLANK (just white rectangle, no text)
- **Conclusion**: Characters at offsets 232+ cannot be accessed, even when called first

### v3.2.0.13: Hardcoded vs Font Data Test ✓
- **Test**: Hardcoded `=` character + font `9` character
- **Expected**: `= 9`
- **Actual**: `= 9` (SUCCESS!)
- **CRITICAL FINDING**:
  - Hardcoded `=` at low memory offset: ✓ Works
  - Font `9` at offset 200: ✓ Works
  - Font `=` at offset 232: ✗ Fails
  - Font `>` at offset 240: ✗ Fails

## Root Cause Analysis

### Findings
1. **Font data IS present** in the binary at correct offsets (verified with hexdump)
2. **NASM addressing IS correct** (verified in listing files)
3. **draw_char function works** (hardcoded data renders fine)
4. **Font data up to ~offset 200 works** (characters '0' through '9' accessible)
5. **Font data beyond ~offset 208 fails** (characters after '9' not accessible)

### Current Hypothesis

The issue is likely related to how the font file is being included or accessed:

**Possible causes:**
1. **64KB segment boundary**: Font data might cross a segment boundary
2. **Include file truncation**: Font file might not be fully included after a certain size
3. **DS register issue**: Data segment might not cover all font data
4. **Memory layout conflict**: Font data might overlap with something else

### Memory Layout

```
Kernel at 0x1000:0000 (linear 0x10000)
├─ Entry point: 0x0002
├─ Code section
├─ font_8x8 label: ~0x0120 (offset 288 from kernel start)
│   ├─ Character ' ' (32): offset 0x0120 + 0 = 0x0120
│   ├─ Character '0' (48): offset 0x0120 + 128 = 0x01A0 ✓ Works
│   ├─ Character '9' (57): offset 0x0120 + 200 = 0x0208 ✓ Works
│   ├─ Character ':' (58): offset 0x0120 + 208 = 0x0210 ⚠ Partial
│   ├─ Character '=' (61): offset 0x0120 + 232 = 0x0240 ✗ Fails
│   └─ Character '>' (62): offset 0x0120 + 240 = 0x0248 ✗ Fails
└─ Padding to 16KB
```

**Font size**: 95 characters × 8 bytes = 760 bytes (0x2F8)
**Font range**: 0x0120 to 0x0418

### Segment Boundary Analysis

**NOT a 256-byte boundary issue:**
- Character at offset 200 (0xC8) works
- Character at offset 208 (0xD0) partially works
- Character at offset 232 (0xE8) fails

The boundary is somewhere between offset 208-232 in the font data.

## Next Steps

### Immediate Actions
1. Check if full font data made it into the kernel binary
2. Verify DS register points to correct segment during font access
3. Test accessing font data beyond offset 208 directly (not through draw_char)
4. Check for any 512-byte or other boundaries at failure point

### Potential Solutions
1. **Copy font to different location**: Load font data to known-good memory region
2. **Access font differently**: Use far pointers or different segment
3. **Split font file**: Put later characters in separate data block
4. **Simplify include**: Instead of including font file, embed font data directly in kernel

## Test Results Summary

| Version | Test | Result | Insight |
|---------|------|--------|---------|
| 3.2.0.8 | Offsets 128,160,200,240 | `0 4` only | Boundary at 160-200 |
| 3.2.0.9 | With `word` qualifier | `0 4` only | Not encoding issue |
| 3.2.0.10 | Direct labels | `0 4 9` | Boundary moved to 200-240 |
| 3.2.0.11 | Offsets 200,208,232,240 | `9` + top dot of `:` | Execution corruption |
| 3.2.0.12 | Only 232,240 | Blank | Can't access high offsets |
| 3.2.0.13 | Hardcoded `=` + font `9` | `= 9` ✓ | Font access issue confirmed |

## Conclusion

The issue is definitively with accessing included font data beyond offset ~200-208. The `draw_char` function works correctly, and lower font offsets work. The problem lies in how the font file data is included, stored, or accessed at runtime when using offsets beyond this boundary.

---

*Last updated: 2026-01-23*
*Current version: 3.2.0.13*
