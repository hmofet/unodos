# Flasher screenshot pipeline (macOS)

Headlessly screenshot the UnoDOS macOS USB flasher, re-runnable per release, for
the [user manual](https://hmofet.github.io/unodos/getting-started.html). The
macOS twin of `pc64/flash/capture/` (Windows).

macOS gates screen/window capture behind **Screen Recording** permission (TCC),
and a plain SSH process cannot capture the GUI session. The fix is a tiny
**capture app bundle** (`UnoDosFlasherCapture.app`) with its own bundle identity:
grant *it* Screen Recording once, then `open` it (even from SSH) and it captures
the flasher in the login session. This is the macOS analog of the Windows
scheduled-task-with-admin approval.

It **never clicks Install**, so no drive is written; the downloaded `.app` is
deleted after each run.

## One-time setup (on the Mac, no admin needed)

```bash
./build-capture-app.sh          # assembles + ad-hoc signs UnoDosFlasherCapture.app
```

Then grant it Screen Recording:
**System Settings > Privacy & Security > Screen Recording > +**, press
**Cmd+Shift+G**, paste the full path to `UnoDosFlasherCapture.app`, and turn its
switch on.

## Capture (any time, per release)

```bash
./run-capture-mac.sh            # or: open UnoDosFlasherCapture.app
```

Output: `~/unodos-flasher-shots/flasher-macos.png` (+ `capture.log`). Copy it into
`docs/assets/img/` and reference it from the manual.

## Files

| File | Role |
|------|------|
| `capture.swift` | the app's compiled executable: download latest, open, find the window (`CGWindowListCopyWindowInfo`), capture it **in-process** via ScreenCaptureKit, trim transparent margins, quit, clean up |
| `Info.plist` | bundle metadata (`LSUIElement`, so no Dock icon) |
| `build-capture-app.sh` | compile `capture.swift` into the `.app` + ad-hoc sign |
| `run-capture-mac.sh` | fire a capture and wait for the PNG |

## Notes

- Capture is **in-process** (ScreenCaptureKit) on purpose: a shelled-out
  `/usr/sbin/screencapture` is evaluated by TCC as its own binary and stays
  denied even when the `.app` is granted.
- The `.app` is ad-hoc signed, so its TCC identity is its code hash. If you edit
  `capture.swift` and rebuild, re-grant Screen Recording
  (`tccutil reset ScreenCapture org.unodos.flashercapture`, then re-add it).
  Normal per-release captures reuse the same `.app`, so no re-grant is needed.
- The login user must have an active aqua session for the capture to render,
  same as the Windows pipeline needs an interactive session.
