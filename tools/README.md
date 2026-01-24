# UnoDOS Tools

Scripts for creating test floppies and writing UnoDOS to physical floppy disks.

## For Windows Users (No WSL Required!)

### Write UnoDOS to Floppy
```powershell
.\Write-Floppy-Quick.ps1
```

- **No build tools needed!**
- Pre-built images are included in git
- Just pull and write to floppy

### Create Test Floppy for FAT12 Testing
```powershell
.\Create-TestFloppy.ps1
```

Creates a FAT12 test floppy with TEST.TXT (1024 bytes, multi-cluster).

## For Linux Developers

### Build UnoDOS from Source
```bash
cd ../
make clean && make floppy144
```

### Commit Pre-Built Image
After making changes, build and commit the image so Windows users get it:

```bash
make clean && make floppy144
git add build/unodos-144.img
git commit -m "Update pre-built image for v3.10.x"
git push
```

## Scripts

### Write-Floppy-Quick.ps1
**Fast image writer with automatic updates**

- Pulls latest code from GitHub (includes pre-built image)
- Writes to floppy drive A:
- **No WSL or build tools required!**

**Usage:**
```powershell
.\Write-Floppy-Quick.ps1
```

**Options:**
```powershell
.\Write-Floppy-Quick.ps1 -SkipPull       # Don't pull from git
.\Write-Floppy-Quick.ps1 -DriveLetter B  # Use drive B: instead of A:
```

### Write-Floppy.ps1
**Image writer with verification**

- Slower but more reliable (verifies writes)
- Useful for troubleshooting bad floppies
- Also requires no build tools

**Usage:**
```powershell
.\Write-Floppy.ps1
```

### Create-TestFloppy.ps1
**Creates FAT12 test floppy for filesystem testing**

- Formats floppy as FAT12
- Creates TEST.TXT with multi-cluster content
- Volume label: "TEST"
- File size: 1024 bytes (2 clusters)

**Usage:**
```powershell
.\Create-TestFloppy.ps1
```

**Options:**
```powershell
.\Create-TestFloppy.ps1 -Drive B:         # Use drive B:
.\Create-TestFloppy.ps1 -FileSize 2048    # Create 2KB file
```

## Diagnostic Tools

### dump_fat12.py
**Python script to examine FAT12 floppy structure**

Shows directory entries with:
- Filename (8.3 format)
- Attributes (Volume, Directory, System, Hidden, Archive)
- Starting cluster
- File size

**Usage:**
```bash
python dump_fat12.py A:         # Windows (drive letter)
python dump_fat12.py /dev/fd0   # Linux (device)
```

**Example Output:**
```
Entry  0: 'TEST       ' attr=0x08 (VOLUME)
Entry  1-2: [VFAT LFN entries]
Entry  3: 'SYSTEM~1   ' attr=0x16 (HIDDEN, SYSTEM, DIR)
Entry  4: 'TEST    TXT' attr=0x20 (ARCHIVE) cluster=4 size=1024
```

## Requirements

### Windows Users
- **PowerShell** (included in Windows)
- **Git for Windows** (to pull updates)
- **Physical floppy drive** (typically A:)

### Linux Developers (for building)
- **nasm** - Assembler
- **make** - Build system
- **qemu-system-i386** (optional, for testing)

Install on Ubuntu/Debian:
```bash
sudo apt-get install nasm make qemu-system-i386
```

## Troubleshooting

### "Access denied" when writing floppy
- Run PowerShell as Administrator
- Right-click PowerShell → "Run as Administrator"

### "Drive A: not found"
- Ensure floppy disk is inserted
- Check BIOS settings (floppy drive enabled?)
- Try different drive letter if configured differently

### "Format failed" when creating test floppy
- Run as Administrator (required for format command)
- Floppy may be write-protected (check tab on disk)
- Try different floppy disk (may be damaged)

### Script says "Repository updated" but floppy doesn't work
- Check version display on boot (should show v3.10.1)
- If older version shows, the image didn't update
- Try: `git pull --force` then run script again

## Testing Procedure

See [WINDOWS_FLOPPY_TESTING.md](WINDOWS_FLOPPY_TESTING.md) for complete testing guide.

**Quick test:**
1. Run `.\Write-Floppy-Quick.ps1` (creates UnoDOS boot floppy)
2. Run `.\Create-TestFloppy.ps1` (creates test data floppy)
3. Boot from UnoDOS floppy
4. Press F for filesystem test
5. Swap to test floppy when prompted
6. Verify output shows "Mount: OK", "Dir: TEST     TXT", "Open TEST.TXT: OK"

## References

- [FAT12 Testing Status](../docs/FAT12_TESTING_STATUS.md) - Current test results
- [Windows Floppy Testing Guide](WINDOWS_FLOPPY_TESTING.md) - Detailed Windows instructions
- [Omnibook Test Procedure](../docs/OMNIBOOK_TEST_PROCEDURE.md) - Hardware test guide

---

**Last Updated:** 2026-01-23
**UnoDOS Version:** v3.10.1
