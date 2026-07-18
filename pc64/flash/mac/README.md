# UnoDOS USB Installer — macOS (pc64)

A single self-contained **macOS** app that writes the bootable UnoDOS/pc64 UEFI
image to a USB drive — the macOS twin of the Windows flasher in
[`pc64/flash/`](../), ported from the Writer's Unlock mac flasher and simplified
to pc64's UEFI-only, single-image world (no BIOS/Legacy build, no FAT32-vs-exFAT
choice).

## Use it (end user)
1. Open **`UnoDosFlasher.app`**.
2. Pick your USB drive (the smallest removable disk is auto-selected), click **Install**, confirm the erase.
3. macOS prompts for an administrator password (raw disk writes need root).
4. Boot the target PC from the USB: enter firmware, turn **Secure Boot off**, pick the USB from the boot menu.

Everything on the selected drive is erased.

## Build it (developer)

On a Mac with:
- **Swift** toolchain (Xcode or the Swift CLI).
- The pc64 image toolchain: `x86_64-w64-mingw32-gcc` (`brew install mingw-w64`),
  `sgdisk` (`brew install gptfdisk`), `mtools` (`brew install mtools`), `python3`.
- Optional: the `Slate Local Signing` keychain identity (a stable local
  code-signing cert). Without it the app is ad-hoc signed.

```sh
# builds pc64 (BOOTX64.EFI + ESP), packs + gzips the disk image, builds the
# universal .app, compiles the privileged writer, signs it
pc64/flash/mac/build-app.sh

# reuse an already-built pc64/build/unodos-uefi.img (skip build.sh + mkuefi.py):
pc64/flash/mac/build-app.sh release --skip-build
```

If your GCC is new enough to reject the repo's implicit declarations (GCC 14+),
pass the flags through: `UNO_EXTRA="-std=gnu17 -Wno-implicit-function-declaration" build-app.sh`.

Output: `pc64/flash/mac/UnoDosFlasher.app` (universal arm64 + x86_64).

## Files
| File | Role |
|------|------|
| `Sources/UnoDosFlasher/App.swift` | the SwiftUI window + drive picker |
| `Sources/UnoDosFlasher/Model.swift` | `diskutil`-based external-disk enumeration |
| `Sources/UnoDosFlasher/FlashModel.swift` | flash flow: confirm → unmount → privileged write → progress |
| `Sources/CFlash/` | C shim over Authorization Services (root exec = the macOS UAC analogue) |
| `writer.c` | the privileged raw writer: gzip-stream the image to `/dev/rdiskN` |
| `build-app.sh` | build.sh → mkuefi.py → gzip → swift build → assemble + sign the `.app` |
| `Package.swift` | SwiftPM manifest (CFlash + UnoDosFlasher, links Security.framework) |

## How it works
The app embeds one gzipped image (`Resources/images/unodos.img.gz`, the gzip of
`pc64/build/unodos-uefi.img` from [`tools/mkuefi.py`](../../tools/mkuefi.py)). On
Install it `diskutil unmountDisk`s the target, then runs the bundled `uno-writer`
as root via Authorization Services; the writer streams the decompressed image to
the raw char device (`/dev/rdiskN`) in sector-aligned 1 MiB blocks and reports
`P done total` / `DONE` / `ERR` lines the GUI turns into a progress bar.
