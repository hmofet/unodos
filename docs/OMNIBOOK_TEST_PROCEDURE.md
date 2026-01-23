# HP Omnibook 600C Testing Procedure (v3.10.0)

## Overview

This document describes how to test UnoDOS v3.10.0 on the HP Omnibook 600C with the FAT12 filesystem driver.

## What You'll Need

1. **HP Omnibook 600C** with 3.5" floppy drive
2. **Two 3.5" floppy disks:**
   - Disk 1: UnoDOS boot disk (write `unodos-144.img`)
   - Disk 2: Test data disk (create your own with TEST.TXT file)

## Preparation

### Step 1: Build UnoDOS Floppy Image

On your Linux development machine:

```bash
cd /home/arin/unodos/unodos
make clean
make floppy144
```

This creates `build/unodos-144.img` (1.44MB floppy image).

### Step 2: Write UnoDOS to Floppy

**On Linux:**
```bash
# Insert blank floppy into drive
sudo ./tools/writeflop.sh -1 /dev/fd0
```

The script will:
- Detect your floppy drive
- Write `build/unodos-144.img` to the floppy
- Verify the write (optional)

**On Windows (PowerShell as Administrator):**
```powershell
.\tools\Write-Floppy-Quick.ps1
```

### Step 3: Create Test Data Floppy

You can create your own test floppy with any FAT12-formatted disk:

**Option A: Format blank floppy (Linux):**
```bash
# Insert second floppy
sudo mkfs.vfat -F 12 -n "TESTDATA" /dev/fd0
```

**Option B: Format blank floppy (Windows):**
```cmd
format A: /FS:FAT /V:TESTDATA
```

**Then add TEST.TXT file:**

Create a text file with any content you like. For example:
```
Hello from the Omnibook!
This is a test of the FAT12 driver.
UnoDOS v3.10.0 can read this file.
```

Copy it to the floppy:
- **Linux:** `sudo mount /dev/fd0 /mnt && sudo cp test.txt /mnt/TEST.TXT && sudo umount /mnt`
- **Windows:** Just copy the file as `TEST.TXT` to the floppy drive

**Important:** The filename MUST be `TEST.TXT` (uppercase, 8.3 format).

## Testing on HP Omnibook 600C

### Boot Sequence

1. **Insert UnoDOS boot floppy** into the Omnibook's drive
2. **Power on** the Omnibook (or press Ctrl+Alt+Del to reboot)
3. **Wait for boot:**
   - Stage2 loader displays: "Loading kernel.."
   - Kernel loads (28KB = 56 sectors)
   - Screen switches to CGA 320x200 graphics mode (blue background)
   - Welcome screen displays: "WELCOME / TO / UNODOS 3!"

### Interactive Test

**The keyboard demo starts automatically:**

```
Type text (ESC to exit, F for file test):
Event System + Graphics API
```

**What you can do:**
- Type any text → characters appear on screen
- Press Enter → move to next line
- Press Backspace → cursor moves back
- Press ESC → exit demo, system halts
- **Press F or f → start filesystem test** ⬅️ This is what we're testing!

### Filesystem Test Procedure

**When you press 'F':**

1. **Prompt appears:**
   ```
   FAT12 Filesystem Test
   Insert test floppy (with TEST.TXT) - Press any key
   ```

2. **Swap floppies:**
   - Remove UnoDOS boot floppy
   - Insert test data floppy (with TEST.TXT)
   - Press any key to continue

3. **Test runs:**
   ```
   Testing...
   Mount: OK
   Open TEST.TXT: OK
   Read: OK - File contents:
   [Your file contents display here]
   ```

4. **System halts** after displaying results

### Expected Results

✅ **Success case:**
- "Mount: OK" → FAT12 filesystem detected
- "Open TEST.TXT: OK" → File found in root directory
- "Read: OK - File contents:" → File read successfully
- Your TEST.TXT contents displayed on screen (up to ~200 characters)

❌ **Failure cases:**

| Display | Meaning | Possible Cause |
|---------|---------|----------------|
| "Mount: FAIL" | Couldn't read boot sector | Bad floppy, wrong format, not FAT12 |
| "Open TEST.TXT: FAIL (not found)" | File not on disk | TEST.TXT missing or wrong name |
| "Read: FAIL" | Disk read error | Corrupted floppy, bad sectors |

### Testing Without Floppy Swap

If you only have one floppy drive, you can test by adding TEST.TXT to the UnoDOS boot floppy itself:

**Before writing to floppy:**
```bash
# Mount the image
sudo mkdir -p /mnt/unodos
sudo mount -o loop build/unodos-144.img /mnt/unodos

# Create TEST.TXT
echo "Hello from UnoDOS!" | sudo tee /mnt/unodos/TEST.TXT

# Unmount
sudo umount /mnt/unodos

# Now write to floppy
sudo ./tools/writeflop.sh -1 /dev/fd0
```

Then you don't need to swap floppies - just press F and any key.

## What This Tests

### Foundation Layer Components

✅ **Graphics API** - Drawing text messages
✅ **Event System** - Keyboard event processing
✅ **Keyboard Driver** - Detecting 'F' key press
✅ **Filesystem Abstraction** - fs_mount, fs_open, fs_read, fs_close
✅ **FAT12 Driver** - BPB parsing, directory search, file reading
✅ **BIOS INT 13h** - Sector reading from floppy

### What Gets Validated

1. **Boot Process:** 3-stage boot works on real hardware
2. **CGA Graphics:** VGA with CGA emulation works correctly
3. **Keyboard Input:** Real keyboard scan codes translate properly
4. **Floppy I/O:** BIOS INT 13h works with Omnibook's floppy controller
5. **FAT12 Parsing:** Boot sector reading and BPB parsing
6. **Directory Search:** Root directory traversal and filename matching
7. **File Reading:** Cluster-to-sector conversion and data retrieval

## Troubleshooting

### Boot Issues

**"Disk error!" during boot:**
- Floppy may be bad or unreadable
- Try different floppy disk
- Clean floppy drive heads

**"Bad kernel!" message:**
- Kernel didn't write correctly
- Rewrite floppy image
- Try different floppy disk

**Black screen after boot:**
- CGA emulation may not be working
- Check Omnibook BIOS settings for video mode
- Try VGA-compatible mode if available

### Filesystem Test Issues

**"Mount: FAIL":**
- Test floppy not FAT12 formatted
- Reformat floppy as FAT12: `mkfs.vfat -F 12`
- Try different floppy

**"Open TEST.TXT: FAIL":**
- File not named exactly "TEST.TXT" (must be uppercase)
- File not in root directory (subdirectories not supported yet)
- File may be corrupted

**"Read: FAIL":**
- Bad sectors on floppy
- Try different floppy
- Reduce file size (current limit: 200 bytes)

### Known Limitations (v3.10.0)

- **Single cluster reads:** Files must fit in one cluster (512 bytes for FAT12)
- **Root directory only:** No subdirectory support
- **8.3 filenames only:** No long filename support
- **Read-only:** Cannot write files (yet)
- **Display limit:** Shows first ~200 characters of file

## What to Report

If you find issues, please note:

1. **Boot stage** where failure occurred (boot sector, stage2, kernel, demo)
2. **Exact error message** displayed
3. **Floppy disk details** (brand, capacity, format)
4. **Omnibook model** and specs (RAM, BIOS version if known)
5. **Success/failure** of each test stage (mount, open, read)

## Next Steps

After validating the FAT12 driver on real hardware, we can proceed with:

- **v3.11.0: Application Loader** - Load and run .BIN programs from floppy
- **v3.12.0: Window Manager** - GUI windows and event routing
- **v3.13.0: Multi-floppy Support** - Load additional drivers from Disk 2

---

*Document created: 2026-01-23*
*UnoDOS v3.10.0 - FAT12 Filesystem Testing on Real Hardware*
