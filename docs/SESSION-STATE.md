# UnoDOS Development Session State

**Last Updated:** 2026-01-28
**Current Build:** 072
**Branch:** master

## Current Status: USB Boot Almost Working

### What Works
- MBR loads and finds bootable partition
- VBR loads stage2 (using LBA fallback for USB)
- Stage2 loads kernel from FAT16 filesystem
- Kernel entry point reached
- Text mode output works correctly
- INT 0x80 syscall handler installed
- Keyboard wait using BIOS INT 16h (before our handler install)

### What's Being Tested (Build 072)
- **Keyboard wait fix**: Moved keyboard handler installation to AFTER INT 16h keypress wait
- **Cleaner boot output**: Removed verbose debug messages
- **Expected output:**
  ```
  MV Loading stage2...
  Loading UnoDOS kernel...... OK

  Kernel: .UnoDOS v3.13.0 (Build: 072)
  Press any key...
  ```
- After keypress: Should switch to blue graphics screen

### Known Issues (Fixed or Bypassed)
1. **Mouse init hangs** (Build 067) - BYPASSED: Skipped mouse initialization
   - 8042 keyboard controller doesn't respond properly on USB boot
   - `install_mouse` commented out, `mouse_enabled` set to 0

2. **Garbled graphics text** (Build 068) - FIXED: Initialize `caller_ds` before draw calls
   - `gfx_draw_string_stub` uses `caller_ds` for string segment
   - Was uninitialized during kernel boot

3. **INT 16h keyboard wait hangs** (Build 072) - FIXED: Reorder init
   - Our keyboard handler fills kernel buffer, not BIOS buffer
   - INT 16h reads BIOS buffer (empty forever)
   - Solution: Install keyboard handler AFTER INT 16h calls

### Next Steps (If Build 072 Works)
1. Test that keypress continues to graphics mode
2. Verify graphics text renders correctly on blue screen
3. Test keyboard input in graphics mode (keyboard_demo)
4. Re-enable mouse init with proper timeout/fallback
5. Test app loading from FAT16

### Next Steps (If Build 072 Still Has Issues)
1. If keypress still doesn't work:
   - Check if interrupts need to be enabled (STI)
   - Try polling keyboard port directly (IN AL, 0x60)

2. If graphics text still garbled:
   - Verify font_8x8 data at correct offset
   - Check ES segment in plot_pixel_white

### Files Modified Recently
- `kernel/kernel.asm` - Main changes for boot sequence
- `boot/vbr.asm` - Removed debug output
- `boot/stage2_hd.asm` - Removed debug output
- `docs/boot-debug-messages.md` - Debug reference (NEW)
- `docs/bootloader-architecture.md` - Architecture doc (NEW)

### Debug Output Reference (Verbose Mode - Pre-Build 072)
If you need to re-enable debug output for troubleshooting:
```
M                    - MBR started
(XX/YY)              - Drive geometry (sectors/heads in hex)
L                    - Using LBA mode (CHS failed)
[XX]                 - First byte of stage2 loaded
Loading stage2...    - VBR message
Loading UnoDOS kernel - Stage2 message
RAAAABB              - Root dir at LBA AAAA, first byte BB
....                 - Sector loading progress
OK                   - Kernel loaded, signature verified
JCCCCCCCC            - Pre-jump debug (first 4 bytes at entry)
K                    - Kernel entry reached
1234                 - Init phases completed
```

### Key Technical Details
- **Kernel load address:** 0x1000:0x0000
- **Kernel entry:** 0x1000:0x0002 (after 2-byte signature)
- **Kernel signature:** 0x4B55 ("UK")
- **Stage2 signature:** 0x5355 ("US")
- **Video memory:** 0xB800:0x0000 (CGA mode 4)
- **Graphics mode:** Mode 4 (320x200, 4-color CGA)

### Build Commands
```bash
# Build HD image
make hd-image

# Build floppy image
make floppy144

# Run in QEMU
make run-hd

# Bump build number
echo XX > BUILD_NUMBER
```

### Git State
```bash
git log --oneline -5
# 2910872 Add bootloader docs, fix keyboard wait, clean up output (Build 072)
# 99796ca Add char-by-char text mode test at kernel entry (Build 071)
# 5dd373f Add keypress pause after text-mode debug output (Build 070)
# 06cf6c6 Add text-mode debug: print version string before graphics (Build 069)
# 4f1fa4f Fix garbled text: initialize caller_ds before draw calls (Build 068)
```
