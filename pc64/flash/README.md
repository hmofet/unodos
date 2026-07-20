# UnoDOS USB Installer (pc64)

A single self-contained **Windows** app that writes a bootable UnoDOS/pc64 image
to a USB drive - the same idea as the [Writer's Unlock](https://github.com/hmofet/writers-unlock)
flasher, adapted to pc64's UEFI-only world.

pc64 is **UEFI / x86-64 only**, so this flasher is deliberately simpler than the
Writer's Unlock one: there is no BIOS/Legacy build and no FAT32-vs-exFAT choice.

It does **not** clone a fixed-size image. It builds the volume on the target:
GPT, one EFI System Partition spanning the whole disk, formatted FAT32, with the
UnoDOS tree copied in. A 32 GB stick becomes a 32 GB UnoDOS drive - the old
raw-image clone left all but the first 512 MB unallocated, so the OS had nowhere
to save documents. The system files ride in the exe as a zipped `build/esp/`
tree rather than as a disk image.

## Use it (end user)
1. Download **`UnoDosFlasher.exe`** from the release. No install.
2. Run it. Windows prompts for Administrator (raw disk writes need it).
3. Pick your USB drive (smallest removable disk is auto-selected), click **Install**, confirm the erase.
4. Boot the target PC from the USB: enter firmware, turn **Secure Boot off**, pick the USB from the boot menu.

Everything on the selected drive is erased.

## Developer options
**Developer options...** on the main window adds two things to an install, for
putting test material on a fresh drive without a second copy step:

- **Copy a folder onto the drive** - the folder's *contents* land in the drive
  root. Defaults to the media test kit at
  `\\behemoth\unreplicated\unodos\pc64\testkit` (see its `README.TXT`).
- **Extract a .zip onto the drive** - pick any zip; optionally into a subfolder.

Settings live in **`%APPDATA%\UnoDOS\flasher.ini`**, deliberately *not* beside
the exe: `deploy-to-share.ps1` overwrites `UnoDosFlasher.exe` on the share after
every build, and people run it straight from `\\behemoth`, so anything kept with
the binary would be lost on the next update. Unknown keys in that file are
preserved on save, so an older flasher cannot silently drop a newer one's
settings.

If a configured folder or zip is missing at install time you get a warning and
the choice to continue - a stale developer path never blocks a flash.

## Self-update

`deploy-to-share.ps1` stages every new flasher on
`\\behemoth\unreplicated\unodos\pc64\` together with `flasher-version.txt`
(build stamp + sha256 of the exe). A locally-run flasher compares its own
embedded stamp against that file:

- **Automatically at startup** (the "Update automatically" checkbox, on by
  default, stored as `autoupdate=` in `flasher.ini`), and
- **on demand** via the **Check for updates** button.

The share is tried by name first, then by IP (`\\192.168.2.75\...`) in case DNS
is down; `updatepath=` in `flasher.ini` overrides the folder entirely. This LAN
share is the interim channel - an internet-based endpoint will join or replace
it later (`UnoUpdate.Check()` is the only piece that has to learn HTTP).

Updating downloads the staged exe next to the running one, verifies its sha256,
renames the running exe to `*.exe.old` (Windows allows renaming, not deleting, a
running exe), moves the new one into place and launches it; the new instance
deletes the `*.exe.old` as soon as the old process exits. Any failure rolls
back, so the exe you started with always survives.

Two cases never self-update: the copy on the share itself (that one is owned by
`deploy-to-share.ps1`), and a hand-compiled exe (its stamp is the `0-dev`
placeholder from the checked-in `UnoVersion.cs` - only `build-flasher.ps1`
generates a real stamp), so an update can't clobber work in progress.

Names are converted to 8.3 the way Windows does it, because the OS reads short
names only: `My Song.mp3` is stored as `MYSONG~1.MP3` with a long-name entry, so
Explorer shows the real name and UnoDOS sees the alias. A name that is already
8.3 apart from its case (`sample.uno`) keeps its short name and uses the FAT
lowercase flags instead, which is what `mtools` does when it builds the image.

## Build it (developer)

Needs, on this Windows box:
- **WSL** with the pc64 toolchain: `x86_64-w64-mingw32-gcc`, `sgdisk` (gdisk), `mtools`, `python3`.
- In-box **.NET Framework** `csc.exe` (ships with Windows; no SDK needed).

```powershell
# builds pc64 (BOOTX64.EFI + ESP), packs the disk image, embeds + compiles the exe
pc64\flash\build-flasher.ps1 -SizeMiB 512

# reuse an already-built build/esp/ (skip ./build.sh):
pc64\flash\build-flasher.ps1 -SkipBuild

# also build build/UnoDiskTest.exe, the headless volume builder used for testing:
pc64\flash\build-flasher.ps1 -SkipBuild -TestTool
```

Output: `pc64/build/UnoDosFlasher.exe` and the raw `pc64/build/unodos-uefi.img`.
The `.img` is still built because `deploy-to-share.ps1` publishes it for
Rufus / balenaEtcher / dd users and `mkiso.py` turns it into the hybrid ISO; the
flasher itself no longer uses it.

Regenerate the icon (needs Pillow): `python pc64/flash/make-icon.py`.

## Files
| File | Role |
|------|------|
| `UnoDosFlash.cs` | the GUI: drive scan (WMI), volume dismount, developer options |
| `UnoDisk.cs` | GPT + FAT32 builder and the file writer - all the on-disk format |
| `UnoSettings.cs` | developer settings in `%APPDATA%`, so they outlive the exe |
| `UnoUpdate.cs` | self-update against the staged flasher on the share |
| `UnoVersion.cs` | dev-placeholder build stamp; the real one is generated at build |
| `UnoDiskTest.cs` | builds a volume into an image FILE so real tools can check it |
| `app.manifest` | forces the Administrator (UAC) elevation the raw write needs |
| `build-flasher.ps1` | build.sh -> mkuefi.py -> zip the ESP -> csc, all in one |
| `make-icon.py` | generates `unodos.ico` (monitor + boot cursor, no font needed) |
| `publish-release.ps1` | attaches the exe + gzipped image to a `gh` release |

## Testing the volume builder
`UnoDisk.cs` writes through a plain `Stream`, so the exact code that formats a
USB stick can format a file. Verify with real tools instead of flashing a stick
and hoping:

```powershell
pc64\flash\build-flasher.ps1 -SkipBuild -TestTool
pc64\build\UnoDiskTest.exe out.img 8192 pc64\build\esp <extraFolderOrZip>|SUBDIR
```
```sh
sgdisk -v out.img                       # partition table
dd if=out.img of=/tmp/p.img bs=1M skip=1 conv=sparse
fsck.vfat -n -v /tmp/p.img              # the filesystem
mdir -i /tmp/p.img ::/                  # what actually landed on it

# boot the real image (GPT + FAT32) as USB mass storage under OVMF:
python3 tools/diskboot_test.py out.img mytag 30
```

`tools/diskboot_test.py` matters because `harness.py` boots
`-drive format=vvfat,file=fat:rw:build/esp`, which is QEMU *faking* a filesystem
from a host directory. That proves the OS works; it proves nothing about a
partition table or an on-disk FAT32, because in that path neither exists.

## The image builder
`pc64/tools/mkuefi.py` turns the `build/esp/` **directory** that `build.sh` emits
(which QEMU fakes with `-drive fat:rw:build/esp`) into a real **raw disk image**:
GPT + one EF00 EFI System Partition (FAT32) with the whole ESP tree copied in.
Real UEFI firmware needs the partition table; QEMU does not.

```sh
./build.sh                    # -> build/esp/
python3 tools/mkuefi.py 512   # -> build/unodos-uefi.img  (512 MiB)
```

Verify / boot-test the image directly:
```sh
qemu-system-x86_64 -machine q35 -m 256 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS_4M.fd \
  -drive format=raw,file=build/unodos-uefi.img
```

## Cross-platform note
Writer's Unlock also ships a macOS flasher (`mac/flasher/`, Swift + a small C
raw-writer). The same port for pc64 is straightforward - swap the branding and
the single embedded image - but is not built here (it needs a Mac to compile).
