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
| `winid.swift` | prints the flasher window id via `CGWindowListCopyWindowInfo` (no permission needed for the id/geometry) |
| `capture` | the app's executable: download latest, open, `screencapture -l`, quit, clean up |
| `Info.plist` | bundle metadata (`LSUIElement`, so no Dock icon) |
| `build-capture-app.sh` | assemble + ad-hoc sign the `.app` |
| `run-capture-mac.sh` | fire a capture and wait for the PNG |

## Notes

- The `.app` is ad-hoc signed for a stable TCC identity. If you edit `capture`
  and rebuild, the signature changes and you may need to re-grant Screen
  Recording.
- The login user must be logged in (an active aqua session) for the capture to
  render, same as the Windows pipeline needs an interactive session.
