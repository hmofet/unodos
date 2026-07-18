# Flasher screenshot pipeline (Windows)

Headlessly screenshot the UnoDOS Windows USB flasher, re-runnable on every
release, for the [user manual](https://hmofet.github.io/unodos/getting-started.html).

The flasher requires Administrator, and an interactive UAC prompt cannot be
automated. The fix is a **scheduled task registered to run with highest
privileges** (approved once), which then runs the capture headlessly with no UAC
prompt. Capture uses the Win32 `PrintWindow` API, which paints the window into a
bitmap even when the physical display is off or the machine is locked, so it
works on a headless build box.

**It never clicks Install**, so no drive is ever written. The script only opens
the flasher, captures its main window, and closes it.

## One-time setup (run once, as administrator)

```powershell
# in an elevated PowerShell:
powershell -ExecutionPolicy Bypass -File setup-capture-task.ps1
```

This registers the scheduled task `UnoDOSFlasherCapture` (runs
`capture-flasher.ps1` as the current user, highest privileges, interactive
session, on demand).

## Capture (any time, no elevation)

```powershell
powershell -ExecutionPolicy Bypass -File run-capture.ps1
# or:  Start-ScheduledTask -TaskName UnoDOSFlasherCapture
```

Output: `%USERPROFILE%\unodos-flasher-shots\flasher-windows.png` (+ `capture.log`).

## Per release

`capture-flasher.ps1` downloads the **latest** released `UnoDosFlasher.exe`
(`releases/latest/download/UnoDosFlasher.exe`), so a fresh run always reflects
the current flasher. Re-run `run-capture.ps1` after publishing a release, then
copy `flasher-windows.png` into `docs/assets/img/` and rebuild the manual.

## Files

| File | Role |
|------|------|
| `capture-flasher.ps1` | download latest exe, launch, `PrintWindow` capture, clean up |
| `setup-capture-task.ps1` | register the highest-privilege scheduled task (run once, elevated) |
| `run-capture.ps1` | fire the task and wait for the PNG (no elevation) |

## Notes

- The downloaded exe is deleted after each run (no build artifacts kept).
- macOS: the `.app` capture would need Screen Recording permission granted once
  in System Settings to a launchd agent using `screencapture`; not yet wired up.
  The macOS flasher UI mirrors the Windows one.
