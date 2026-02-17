# UnoDOS Tools

Scripts for writing UnoDOS images to physical media.

## write.ps1 — Unified Disk Image Writer

Interactive TUI that writes any UnoDOS image to any target drive. Replaces the old `floppy.ps1`, `hd.ps1`, and `apps.ps1` scripts.

**What it does:**
- Pulls latest code from GitHub
- Shows available images (floppy, HD, apps-only) with version/build info
- Detects floppy drives and removable/fixed disks
- Filters out boot/system drives for safety
- Warns on drives >256 GB (requires double confirmation)
- Writes with progress bar and optional verification

**Usage:**
```powershell
# Full interactive mode (recommended):
.\tools\write.ps1

# Quick floppy write:
.\tools\write.ps1 -DriveLetter A

# Quick HD/CF write to specific disk:
.\tools\write.ps1 -DiskNumber 2

# Explicit image + drive:
.\tools\write.ps1 -ImagePath build\unodos-hd.img -DiskNumber 2

# With read-back verification:
.\tools\write.ps1 -Verify

# Skip git pull:
.\tools\write.ps1 -NoGitPull
```

**Requirements:**
- Run as Administrator
- Target media (floppy, CF card, USB drive, or HDD)

**Safety features:**
- Boot and system drives are excluded from the drive list
- Drives >256 GB are greyed out and require a second "Are you REALLY sure?" confirmation
- Default confirmation button is NO
- Drives too small for the selected image are blocked

---

## Legacy Wrappers

The old scripts still work — they now call `write.ps1` internally:

| Script | Equivalent |
|--------|-----------|
| `floppy.ps1 [letter]` | `write.ps1 -ImagePath build\unodos-144.img -DriveLetter A` |
| `hd.ps1` | `write.ps1 -ImagePath build\unodos-hd.img` |
| `apps.ps1 [letter]` | `write.ps1 -ImagePath build\launcher-floppy.img -DriveLetter A` |

---

## Boot Flow

**Floppy:** Boot from `unodos-144.img` → Launcher auto-loads, all apps included.

**Hard Drive / CF:** Boot from `unodos-hd.img` → Launcher and all apps immediately available.

**Apps-only floppy:** Boot from OS floppy, then swap to `launcher-floppy.img` for different app selection.
