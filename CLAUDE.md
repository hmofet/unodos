# UnoDOS — agent instructions

## Standing rule: keep the shared pc64 flasher current

**Whenever you produce a new build of the pc64 OS** (i.e. you run `pc64/build.sh`
and `build/BOOTX64.EFI` / `build/esp/` is regenerated), you must also rebuild the
USB flasher and publish it to the network share, so it can be flashed from any
computer on the LAN and never lags the OS:

```powershell
pc64\flash\deploy-to-share.ps1        # rebuilds the flasher + image, copies to the share
```

This rebuilds `UnoDosFlasher.exe` (with the fresh image embedded), then copies it
and `unodos-pc64-uefi.img.gz` to **`\\behemoth\unreplicated\unodos\pc64\`** and
updates that share's `MANIFEST.txt` + a `BUILD.txt` stamp.

- The flasher's embedded image IS the OS, so a stale flasher ships a stale OS —
  don't skip this after meaningful pc64 changes.
- If you already have `build/UnoDosFlasher.exe` + `build/unodos-uefi.img` from the
  same build and just want to (re)publish, use `deploy-to-share.ps1 -SkipBuild`.
- If `\\behemoth` is offline, the script stops with a clear error — deploy later
  when the share is reachable; don't treat it as a build failure.
- Needs WSL (mingw + sgdisk + mtools) and the in-box .NET `csc`, same as the
  flasher build. See `pc64/flash/README.md`.
