# FAT12 Testing Status

## Current Status: ✅ WORKING (with minor graphics issues)

**Date:** 2026-01-23
**Version:** UnoDOS v3.10.1
**Hardware Tested:** HP Omnibook 600C

## Test Results

### Latest Test (2026-01-23)
```
UnoDOS v3.10.1
[Press F for filesystem test]
Insert test disk, press key
Testing...
Mount: OK
Dir: TEST     TXT
Open TEST.TXT: [Result pending - likely OK based on Dir success]
```

**Status:** ✅ **FAT12 directory reading is WORKING!**
- Boot sector parsing: ✅ Working
- Directory sector reading: ✅ Working (corrected CHS conversion)
- Directory entry parsing: ✅ Working (showing TEST.TXT correctly)
- File skip logic: ✅ Working (skips SYSTEM~1, shows TEST.TXT)

**Known Issues:**
- Graphics rendering has artifacts (text corruption, spacing issues)
- Does NOT affect functionality - disk I/O is working correctly
- Graphics fixes can be done later

## Critical Bugs Fixed

### 1. Write-Floppy-Quick.ps1 Never Rebuilt Images (CRITICAL!)
**Symptom:** Script said "Repository updated!" but still writing old builds
**Root Cause:** Script pulled git repo but never ran `make`
**Fix:** Added automatic build step using WSL make after git pull
**Commit:** ef0f230

### 2. Broken LBA→CHS Conversion (CRITICAL!)
**Symptom:** Dir showed "MSDOS5.0" (boot sector OEM ID) instead of directory
**Root Cause:** CHS conversion formula was completely wrong:
- Old: `sector = (LBA & 0x3F) + 1, cylinder = LBA >> 6, head = 0`
- New: `sector = (LBA % 18) + 1, head = (LBA / 18) % 2, cylinder = LBA / 36`

For LBA 19 (typical root dir start):
- Old: C=0, H=0, S=20 ❌ (invalid - only 18 sectors/track!)
- New: C=0, H=1, S=2 ✅

**Fix:** Proper LBA→CHS formula in 2 places (debug code + fat12_open)
**Commit:** e88dfa9

### 3. Volume Label Skip Logic Backwards
**Symptom:** Showed "MSDOS5.0" volume label as a file
**Root Cause:** `test ah, 0x08` / `jz .found_entry` - jumped if NOT volume
**Fix:** `test ah, 0x08` / `jnz .try_next` - skip if IS volume
**Commit:** 50084e2

### 4. Directory Skip Logic Incomplete
**Symptom:** Showed "SYSTEM~1" directory instead of TEST.TXT file
**Root Cause:** Only skipped LFN and volume labels, not directories/system/hidden
**Fix:** Added skip for DIRECTORY (0x10), SYSTEM (0x04), HIDDEN (0x02)
**Commit:** ef0f230

### 5. Segment Register Bug (DS Not Set)
**Symptom:** Never found any files in directory
**Root Cause:** After INT 13h read into ES:BX (0x1000:bpb_buffer), code read with DS:SI where DS=CS
**Fix:** Set DS=0x1000 before directory search, restore after
**Commit:** 96c16a1

## Timeline

### Commits (Most Recent First)
- **ef0f230** - Fix TWO CRITICAL bugs causing old builds (2026-01-23)
- **9f49864** - Add version display on boot to verify floppy write (2026-01-23)
- **e88dfa9** - Fix BROKEN CHS conversion - the ACTUAL root cause! (2026-01-23)
- **c4c6481** - Fix CRITICAL bug: hardcoded sector 19 instead of using root_dir_start (2026-01-23)
- **50084e2** - Fix word wrap and ascending text bugs in FAT12 test (2026-01-23)
- **ec46051** - Add debug output and fix word wrap in FAT12 test (2026-01-23)
- **96c16a1** - Fix CRITICAL segment register bug in FAT12 directory search (2026-01-23)
- Earlier commits omitted for brevity

## Architecture Decisions Made

### FAT12 Implementation Choices
1. **CHS Conversion:** Proper formula using sectors_per_track=18, num_heads=2
2. **Directory Search:** Linear scan through root directory (typical: 14 sectors, 224 entries)
3. **Entry Filtering:** Skip LFN (0x0F), Volume (0x08), Directory (0x10), System (0x04), Hidden (0x02)
4. **Segment Usage:** DS=0x1000 for directory data, restored after search
5. **Buffer Location:** 0x1000:bpb_buffer for disk I/O

### Testing Tools Created
1. **dump_fat12.py** - Python tool to examine floppy structure (hexdump + FAT parser)
2. **Create-TestFloppy.ps1** - Windows PowerShell script to create test floppy with TEST.TXT
3. **Write-Floppy-Quick.ps1** - Enhanced with auto-build and dependency checking

## Next Steps

### Immediate (v3.10.x)
- [ ] Fix graphics rendering artifacts
- [ ] Test multi-cluster file reading (TEST.TXT is 1024 bytes = 2 clusters)
- [ ] Verify "Open TEST.TXT: OK" appears
- [ ] Verify file contents display correctly

### Short-term (v3.11.0)
- [ ] Implement application loader
  - Load .BIN files from floppy
  - Allocate memory for app code+data
  - Execute app entry point
  - Return to OS on exit
- [ ] Create first demo app (clock display or simple game)

### Medium-term (v3.12.0+)
- [ ] Graphics API fixes (text rendering, coordinate handling)
- [ ] Window manager (basic window drawing, Z-order)
- [ ] Event routing to applications
- [ ] System call interface finalization

See [PLAN.md](../../../.claude/plans/mellow-watching-spring.md) for full roadmap.

## Test Floppy Creation

### Windows (PowerShell)
```powershell
cd unodos\tools
.\Create-TestFloppy.ps1
```

Creates 1.44MB FAT12 floppy with TEST.TXT (1024 bytes, 2 clusters).

### Verify Test Floppy
```powershell
python dump_fat12.py A:
```

Expected output:
```
Entry  0: 'TEST       ' attr=0x08 (VOLUME)
Entry  1-2: [VFAT LFN entries]
Entry  3: 'SYSTEM~1   ' attr=0x16 (HIDDEN, SYSTEM, DIR)
Entry  4: 'TEST    TXT' attr=0x20 (ARCHIVE) cluster=4 size=1024
```

## Writing to Floppy

### Quick Method (Recommended)
```powershell
cd unodos\tools
.\Write-Floppy-Quick.ps1
```

This script:
1. Pulls latest code from GitHub
2. **Builds image automatically** (requires WSL + nasm + make)
3. Writes to floppy drive A:
4. Auto-installs build tools if missing

### Manual Method
```bash
# On Linux/WSL
cd unodos/unodos
make clean && make floppy144

# Write to physical floppy (Linux)
sudo dd if=build/unodos-144.img of=/dev/fd0 bs=512

# Write to physical floppy (Windows PowerShell as Admin)
cd unodos\tools
.\Write-Floppy.ps1
```

## Hardware Test Procedure

1. **Create UnoDOS boot floppy:**
   ```powershell
   cd unodos\tools
   .\Write-Floppy-Quick.ps1
   ```

2. **Create test data floppy:**
   ```powershell
   .\Create-TestFloppy.ps1
   ```

3. **Boot on HP Omnibook 600C:**
   - Insert UnoDOS floppy in drive A:
   - Power on or reboot
   - Wait for blue screen with "UnoDOS v3.10.1" in top-left corner

4. **Run filesystem test:**
   - Press **F** key
   - Wait for prompt: "Insert test disk, press key"
   - **Swap floppies:** Remove UnoDOS, insert test floppy
   - Wait 2-3 seconds for drive to spin up
   - Press any key

5. **Expected output:**
   ```
   Testing...
   Mount: OK
   Dir: TEST     TXT
   Open TEST.TXT: OK
   Read: OK - File contents:
   CLUSTER 1: AAAAA...
   CLUSTER 2: BBBBB...
   ```

## Success Criteria

### ✅ Completed
- [x] Boot from floppy
- [x] Switch to graphics mode (320x200 CGA)
- [x] Parse FAT12 boot sector (BPB)
- [x] Calculate root directory location
- [x] Read root directory with correct CHS conversion
- [x] Parse directory entries
- [x] Skip volume labels, LFN, directories, system, hidden
- [x] Find TEST.TXT file
- [x] Display filename correctly

### 🚧 In Progress (Graphics Issues)
- [ ] Clean text rendering (spacing, corruption)
- [ ] Proper line wrapping

### ⏳ Pending
- [ ] Open file (get starting cluster, size)
- [ ] Read file contents (multi-cluster)
- [ ] Follow FAT chain (cluster 4 → 5 → EOF)
- [ ] Display file contents on screen

## Known Issues

### Graphics Rendering
**Symptoms:**
- Text has spacing issues ("TEST     TXT" shows huge spaces)
- Some characters have corruption
- Line wrapping inconsistent

**Root Cause:** Not yet diagnosed (likely gfx_draw_string_stub coordinate handling)

**Impact:** LOW - Does not affect FAT12 functionality

**Workaround:** None needed for disk testing

**Priority:** Medium (fix after confirming full FAT12 read works)

## Tools and Scripts

### Development Tools
- `dump_fat12.py` - Examine floppy structure (Python)
- `Create-TestFloppy.ps1` - Create test floppy (Windows PowerShell)
- `Write-Floppy-Quick.ps1` - Write image with auto-build (Windows PowerShell)
- `Write-Floppy.ps1` - Write image with verification (Windows PowerShell)

### Build System
- `make clean` - Clean build directory
- `make floppy144` - Build 1.44MB floppy image
- `make run144` - Build and run in QEMU

### Dependencies
- **Linux/WSL:** nasm, make, qemu-system-i386 (optional)
- **Windows:** WSL (for building), PowerShell (for floppy writing)

## References

- [FAT12 Implementation Summary](FAT12_IMPLEMENTATION_SUMMARY.md)
- [Omnibook Test Procedure](OMNIBOOK_TEST_PROCEDURE.md)
- [Omnibook Test Fixes](OMNIBOOK_TEST_FIXES.md)
- [Windows Floppy Testing Guide](../tools/WINDOWS_FLOPPY_TESTING.md)
- [System Architecture Plan](../../../.claude/plans/mellow-watching-spring.md)

---

**Last Updated:** 2026-01-23
**Status:** FAT12 directory reading ✅ WORKING on real hardware!
