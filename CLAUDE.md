# UnoDOS — agent instructions

## Unfinished work waiting on branch `parity-wip` (picked up later)

The 2026-07-19 family parity program was cut short mid-flight. Its finished
parts are on `master`; the **interrupted parts are parked on the branch
`parity-wip` (commit `b2e40c1`), which does NOT build by design** — six ports
were caught mid-edit.

**Read [`docs/PARITY-HANDOFF.md`](docs/PARITY-HANDOFF.md) before resuming
anything parity-related.** It has the audit findings, what landed with evidence,
a per-port table of exactly what each parked port is missing, and the ordered
backlog.

The short version:

- Every "3.1-fresh" port (sms, nes, gb, gg, vic20, ws, pce, gba, rpi, pinephone,
  ppcmac) ships **7 of 11 apps** — Tracker, OutLast, Pac-Man and Paint are
  launcher placeholders, and none has storage persistence. An earlier June
  attempt at this was **never committed and is lost**, so treat any doc or note
  claiming fresh-port parity as false until the source says otherwise.
- `parity-wip` holds new per-app sources for nes/vic20/gba/rpi/ppcmac/pinephone.
  rpi and vic20 build (dispatch unwired); ppcmac needs a `t_tracker` entry; gba
  needs `outlast.inc.s`; pinephone needs `paint.inc.s`; nes is mid-wire.
- **Do not merge `parity-wip` to master as-is.** Finish + harness-verify a port,
  then merge it forward.
- `docs/FEATURE-MATRIX.md` is stale (overstates fresh-port parity, has no C64
  column, pc64 storage row predates the native drivers) — fix it as parity lands.

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
