# UnoDOS USB Installer (pc64)

A single self-contained **Windows** app that writes a bootable UnoDOS/pc64 image
to a USB drive - the same idea as the [Writer's Unlock](https://github.com/hmofet/writers-unlock)
flasher, adapted to pc64's UEFI-only world.

pc64 is **UEFI / x86-64 only**, so this flasher is deliberately simpler than the
Writer's Unlock one: there is no BIOS/Legacy build and no FAT32-vs-exFAT choice.
It bundles one image - a GPT disk with an EFI System Partition (FAT32) holding
`/EFI/BOOT/BOOTX64.EFI` plus the sample docs and TrueType fonts.

## Use it (end user)
1. Download **`UnoDosFlasher.exe`** from the release. No install.
2. Run it. Windows prompts for Administrator (raw disk writes need it).
3. Pick your USB drive (smallest removable disk is auto-selected), click **Install**, confirm the erase.
4. Boot the target PC from the USB: enter firmware, turn **Secure Boot off**, pick the USB from the boot menu.

Everything on the selected drive is erased.

## Build it (developer)

Needs, on this Windows box:
- **WSL** with the pc64 toolchain: `x86_64-w64-mingw32-gcc`, `sgdisk` (gdisk), `mtools`, `python3`.
- In-box **.NET Framework** `csc.exe` (ships with Windows; no SDK needed).

```powershell
# builds pc64 (BOOTX64.EFI + ESP), packs the disk image, embeds + compiles the exe
pc64\flash\build-flasher.ps1 -SizeMiB 512

# reuse an already-built build/esp/ (skip ./build.sh):
pc64\flash\build-flasher.ps1 -SkipBuild
```

Output: `pc64/build/UnoDosFlasher.exe` and the raw `pc64/build/unodos-uefi.img`.

Regenerate the icon (needs Pillow): `python pc64/flash/make-icon.py`.

## Files
| File | Role |
|------|------|
| `UnoDosFlash.cs` | the flasher: drive scan (WMI), volume dismount, raw gzip-streamed write |
| `app.manifest` | forces the Administrator (UAC) elevation the raw write needs |
| `build-flasher.ps1` | build.sh -> mkuefi.py -> gzip -> csc, all in one |
| `make-icon.py` | generates `unodos.ico` (monitor + boot cursor, no font needed) |
| `publish-release.ps1` | attaches the exe + gzipped image to a `gh` release |

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
