# Session Summary - HDD App Scanning Fix (Builds 074-077)

## Problem Statement
HDD bootloader worked, launcher auto-loaded, but showed "No apps found" even though apps existed in FAT16 filesystem.

## Root Cause Analysis

### Issue 1 - Launcher Drive Hardcoded (Build 074)
**Problem**: Launcher hardcoded to scan drive A: (0x00)
**Location**: [apps/launcher.asm:198](apps/launcher.asm#L198)
**Fix**: Try HDD (0x80) first, fall back to floppy (0x00)
**Result**: Mount succeeded, but still no apps found

### Issue 2 - Missing FAT16 Readdir (Build 076)
**Problem**: `fs_readdir_stub` only supported FAT12, not FAT16
**Location**: [kernel/kernel.asm:2382-2418](kernel/kernel.asm#L2382-L2418)
**Fix**:
- Added routing for mount handle 0 (FAT12) vs 1 (FAT16)
- Implemented `fat16_readdir()` function at line 4121-4229
- Reads FAT16 root directory, skips deleted/volume entries
**Result**: Kernel could scan FAT16, but launcher still didn't call it!

### Issue 3 - Launcher Mount Handle Hardcoded (Build 077)
**Problem**: Launcher saved mount handle but passed hardcoded 0 to readdir
**Location**: [apps/launcher.asm:227](apps/launcher.asm#L227)
**Fix**:
- Save mount handle (BX) from `fs_mount` along with drive number
- Pass `mount_handle` to `API_FS_READDIR` instead of hardcoded 0
**Result**: Should finally work!

## Changes by Build

### Build 074
- Launcher tries HDD (0x80) first, then floppy (0x00)
- Saves `mounted_drive` variable
- Uses saved drive for app loading

### Build 075
- Added version/build display in bottom-right for hardware verification
- (Later removed due to screen overflow)

### Build 076
- Implemented FAT16 readdir support in kernel
- Added `fat16_readdir()` function
- `fs_readdir_stub` routes based on mount handle

### Build 077 (FINAL)
- Launcher saves `mount_handle` from `fs_mount` return value
- Passes correct mount handle to `API_FS_READDIR`
- Removed duplicate version displays (kept top-left only)

## Technical Details

### FAT12 vs FAT16 Mount Handles
- FAT12 (floppy): Drive 0x00, mount handle 0
- FAT16 (HDD): Drive 0x80, mount handle 1
- `fs_mount` returns mount handle in BX
- `fs_readdir` expects mount handle in AL

### Launcher Flow (Build 077)
```asm
1. Try mount HDD (0x80) → Returns handle 1 in BX → Save both
2. Call readdir with AL=1 → Routes to fat16_readdir → Success!
3. Scan .BIN files, display in launcher window
4. Load selected app from correct drive
```

### File Locations
- Launcher: `apps/launcher.asm`
- Kernel: `kernel/kernel.asm`
- FAT16 functions: Lines 3403-4229 in kernel.asm
- HDD image creation: `tools/create_hd_image.py`

## Testing Status
- Build 077 compiled successfully
- Needs QEMU testing before hardware deployment
- Version/build display: Top-left only

### Build 078 (CRITICAL FIX)
**Problem**: FAT16 readdir attribute check was wrong!
**Location**: [kernel/kernel.asm:4186](kernel/kernel.asm#L4186)
**Bug**: Used `test al, 0x0F` which would skip ANY file with read-only, hidden, system, or volume bits
**Fix**:
- Check for exact match 0x0F (long filename)
- Check for volume label (0x08) but not directory (0x10)
- Allow all other entries including Archive (0x20)

**Why it seemed to work**: Our .BIN files have attribute 0x20 (Archive), and 0x20 & 0x0F = 0, so they passed the test. But the logic was still wrong and could fail with other attribute combinations.

## Summary of All Fixes

Build 074: Launcher drive detection (try 0x80 then 0x00)
Build 076: Kernel FAT16 readdir implementation
Build 077: Launcher mount handle usage (pass handle to readdir)
Build 078: FAT16 attribute filtering (proper LFN/volume skip)

## Ready for Testing
Build 078 should now correctly:
1. Mount HDD drive 0x80 → Get handle 1
2. Call readdir with handle 1 → Route to fat16_readdir
3. Read FAT16 root directory
4. Skip volume label and LFN entries
5. Return .BIN file entries to launcher
6. Display apps in launcher window
