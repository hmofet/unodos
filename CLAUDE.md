# UnoDOS — agent instructions

## Fresh-port parity — state as of 2026-07-20

The 2026-07-19 audit concluded the June parity work was "never committed and is
lost". **That was wrong.** It was committed and pushed on
`parity-push-fresh-ports`, a branch the survey missed, and it is now **merged to
master**.

- **sms, nes, gba, rpi, pinephone, ppcmac are at 11 of 11 apps** — Tracker,
  OutLast, Pac-Man and Paint are real and wired into dispatch. All six build.
- **gb, gg, vic20, ws, pce still ship 7 of 11** (those four are launcher
  placeholders). Storage persistence is outstanding across the whole fresh tier.
- `parity-wip` (`b2e40c1`, does not build by design) is now **fully superseded**
  by master and holds nothing worth recovering. Do not merge it; it is a
  deletion candidate.
- `docs/FEATURE-MATRIX.md` is stale (no C64 column, pc64 storage row predates
  the native drivers, fresh-port rows now understate six ports) — fix it as
  parity lands.

[`docs/PARITY-HANDOFF.md`](docs/PARITY-HANDOFF.md) carries the full history,
including the correction above; read it before resuming parity work.

**The procedural lesson:** before concluding work is lost, check every branch
and every remote, not just the mainline.

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
