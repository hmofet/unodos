# UnoDOS Tools

Scripts for writing UnoDOS images to physical media.

## PowerShell Scripts (Windows)

### floppy.ps1
Writes UnoDOS to a 1.44MB floppy disk.

**What it does:**
- Pulls latest code from GitHub
- Writes `build/unodos-144.img` to floppy drive
- Includes OS + Launcher on the same disk

**Usage:**
```powershell
.\tools\floppy.ps1          # Write to A:
.\tools\floppy.ps1 B        # Write to B:
```

**Requirements:**
- Run as Administrator
- 1.44MB floppy disk in drive

**What you get:**
- Bootable UnoDOS floppy
- Launcher auto-loads on boot

---

### hd.ps1
Writes UnoDOS to a hard drive, CF card, or USB drive.

**What it does:**
- Writes `build/unodos-hd.img` to physical disk
- Includes OS + Launcher + All Apps (Clock, Browser, Mouse, Test)
- Creates bootable FAT16 partition

**Usage:**
```powershell
# First, find your disk number:
Get-Disk

# Then write (WARNING: ERASES ALL DATA):
.\tools\hd.ps1 -ImagePath build\unodos-hd.img -DiskNumber N
```

**Requirements:**
- Run as Administrator
- Target disk (CF card, USB, or HDD)

---

## New Unified Boot Flow (Build 073+)

**Before:** Boot floppy → Press 'L' → Swap floppy → Launcher loads
**Now:** Boot floppy/HDD → Launcher auto-loads!

No disk swapping needed!
