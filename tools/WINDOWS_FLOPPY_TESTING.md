# Windows Floppy Testing Guide

This guide explains how to create a test floppy for UnoDOS FAT12 filesystem testing on Windows.

## Requirements

- Windows PC with a floppy drive (typically drive A:)
- Blank 3.5" floppy disk (360KB, 720KB, or 1.44MB)
- Administrator privileges (for formatting)

## Quick Start

### Option 1: PowerShell Script (Recommended)

1. **Open PowerShell as Administrator:**
   - Press `Win + X`
   - Select "Windows PowerShell (Admin)" or "Terminal (Admin)"
   - Or: Search for "PowerShell", right-click, "Run as Administrator"

2. **Navigate to UnoDOS tools directory:**
   ```powershell
   cd C:\path\to\unodos\tools
   ```

3. **Run the script:**
   ```powershell
   .\Create-TestFloppy.ps1
   ```

4. **Follow the prompts:**
   - Insert floppy disk
   - Type `YES` when prompted
   - Wait for formatting and file creation

### Option 2: Batch File (Simpler, but larger file)

1. **Open Command Prompt as Administrator:**
   - Press `Win + X`
   - Select "Command Prompt (Admin)" or use PowerShell
   - Or: Search for "cmd", right-click, "Run as Administrator"

2. **Navigate to UnoDOS tools directory:**
   ```cmd
   cd C:\path\to\unodos\tools
   ```

3. **Run the batch file:**
   ```cmd
   create-test-floppy.bat
   ```

4. **Follow the prompts:**
   - Insert floppy disk
   - Type `YES` when prompted
   - Wait for formatting and file creation

### Option 3: Manual Creation

If the scripts don't work, create the test floppy manually:

1. **Format the floppy:**
   ```cmd
   format A: /FS:FAT /V:TEST /Q
   ```
   (When prompted, type `Y` and press Enter)

2. **Create TEST.TXT:**
   - Open Notepad
   - Type or paste content that's at least 1024 bytes (for multi-cluster test):
     ```
     CLUSTER 1: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
     CLUSTER 2: BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB
     ```
   - Save as: `A:\TEST.TXT`
   - **IMPORTANT:** Use "Save As" → Change "Save as type" to "All Files (*.*)"
   - **IMPORTANT:** Filename MUST be exactly `TEST.TXT` (all uppercase, 8.3 format)

3. **Verify:**
   ```cmd
   dir A:\TEST.TXT
   ```
   Should show file size >= 1024 bytes

## What the Scripts Do

1. **Format the floppy as FAT12:**
   - Uses `format A: /FS:FAT` (FAT on floppy = FAT12 automatically)
   - Labels the volume as "TEST"
   - Quick format for speed

2. **Create TEST.TXT with multi-cluster content:**
   - Creates a file larger than 512 bytes
   - Content: "CLUSTER 1: AAA..." (500+ chars) + "CLUSTER 2: BBB..." (500+ chars)
   - Total size: 1024 bytes (spans 2 clusters)
   - Filename: TEST.TXT (8.3 format, uppercase - required for FAT12)

3. **Verify:**
   - Checks file exists
   - Confirms size is multi-cluster (>512 bytes)
   - Shows preview of content

## Testing on HP Omnibook 600C (or other hardware)

### Preparation

1. **Write UnoDOS to first floppy:**
   - Use `Write-Floppy-Quick.ps1` (in same tools directory)
   - Or use `dd` on Linux: `dd if=build/unodos-144.img of=/dev/fd0`

2. **Create test floppy using this guide** (second floppy)

### Test Procedure

1. **Insert UnoDOS boot floppy** in drive A:
2. **Power on** or reboot the Omnibook
3. **Wait** for blue screen with "Type text (ESC to exit, F for file test):"
4. **Press F** to start filesystem test
5. **Prompt appears:** "Insert test floppy (with TEST.TXT) - Press any key"
6. **Swap floppies:**
   - Remove UnoDOS boot floppy
   - Insert test floppy (created with this guide)
   - Wait 2-3 seconds for drive to spin up
7. **Press any key** to continue test
8. **Observe results:**
   ```
   Testing...
   Mount: OK
   Dir: TEST    TXT
   Open TEST.TXT: OK
   Read: OK - File contents:
   CLUSTER 1: AAAAA...
   CLUSTER 2: BBBBB...
   ```

### Expected Results

✅ **SUCCESS:**
- "Mount: OK" - FAT12 filesystem detected
- "Dir: TEST    TXT" - File found in directory
- "Open TEST.TXT: OK" - File opened successfully
- "Read: OK" - Multi-cluster read completed
- Both "CLUSTER 1:" and "CLUSTER 2:" text visible on screen

❌ **FAILURE - Mount:**
- "Mount: FAIL" - Not FAT12, or floppy not readable
- Try: Reformat with script, try different floppy disk

❌ **FAILURE - Open:**
- "Open TEST.TXT: FAIL (not found)" - Filename incorrect
- Try: Recreate file as exactly `TEST.TXT` (uppercase, 8.3 format)
- Check: "Dir:" line shows actual filename on disk

❌ **FAILURE - Read:**
- "Read: FAIL" - Disk read error
- Try: Different floppy disk (may be bad sectors)

## Troubleshooting

### "Drive A: not found"
- Ensure floppy disk is inserted
- Check BIOS settings (floppy drive enabled?)
- Try different drive letter if configured differently

### "Format failed"
- Run as Administrator (required for format command)
- Floppy may be write-protected (check tab on disk)
- Try different floppy disk (may be damaged)

### "Access denied"
- Run PowerShell or Command Prompt as Administrator
- Right-click → "Run as Administrator"

### File too small (< 512 bytes)
- PowerShell script creates exactly 1024 bytes
- Batch file may create slightly different size due to line endings
- Manually edit to ensure 512+ bytes

### "Open TEST.TXT: FAIL (not found)" even though file exists
- **Filename case:** Must be `TEST.TXT` (all uppercase)
- **File location:** Must be in root directory (not in a subfolder)
- **Filename format:** Must be 8.3 format (no long filenames)
- Check "Dir:" line on screen - shows actual filename UnoDOS sees

## Advanced: Creating Custom Test Files

To test with your own content:

1. Format floppy as FAT12 (using script or manual method)
2. Create any file named `TEST.TXT` in root directory
3. File must be:
   - Exactly named `TEST.TXT` (uppercase, 8.3 format)
   - At least 1 byte (preferably >512 bytes for multi-cluster test)
   - In the root directory
   - Plain text (ASCII) for readable output

## Script Customization

### PowerShell script parameters:

```powershell
# Use different drive
.\Create-TestFloppy.ps1 -Drive "B:"

# Create larger test file (e.g., 2048 bytes)
.\Create-TestFloppy.ps1 -FileSize 2048
```

### Batch file customization:

Edit the `.bat` file and change these lines:
```batch
set DRIVE=A:          REM Change to B: if needed
set FILENAME=TEST.TXT REM Must be 8.3 format, uppercase
```

## Why FAT12?

- FAT12 is the standard filesystem for 360KB, 720KB, and 1.44MB floppies
- Modern Windows `format A: /FS:FAT` automatically uses FAT12 for floppies
- FAT16 is used for larger media (hard drives)
- UnoDOS currently only supports FAT12 (FAT16/FAT32 planned for future)

## References

- UnoDOS Documentation: `docs/OMNIBOOK_TEST_PROCEDURE.md`
- Omnibook Fixes: `docs/OMNIBOOK_TEST_FIXES.md`
- FAT12 Implementation: `docs/FAT12_IMPLEMENTATION_SUMMARY.md`

---

*Last updated: 2026-01-23*
*UnoDOS v3.10.1 - Multi-cluster FAT12 support*
