# UnoDOS Remote Desktop client (`unoremote`)

A Windows GUI client for driving a running UnoDOS pc64 machine over **URC** (the
unoautomate remote channel — see [`../REMOTE.md`](../REMOTE.md)). It wraps URC in
a GUI: a **live view** of the device screen, **mouse + keyboard forwarding**,
**session recording**, a **log pane** fed by the URC LOG stream, and a raw
command box.

Remote desktop is just two URC halves working together: `key`/`pointer` (input,
IN) and the `screen` verb (framebuffer out, OUT). This client polls `screen grab
delta` VNC-style — the device sends only the tiles that changed since the last
grab — composites them onto a persistent canvas, and maps clicks/keystrokes back
to framebuffer coordinates. (It seeds the canvas with one full `screen grab` on
connect, then streams deltas; a scale change or a big change transparently comes
back as a full keyframe.)

## Build

```powershell
pc64\remote\build-remote.ps1                      # -> build\UnoRemote.exe
pc64\remote\build-remote.ps1 -Ffmpeg C:\ff\ffmpeg.exe   # bundle ffmpeg for MP4
```

WinForms + the in-box .NET Framework 4.x `csc` — a single self-contained
`winexe`, exactly like the flasher (`pc64/flash/`). No MSBuild, no XAML compiler,
no NuGet. (WPF was the original intent but would force MSBuild/XAML compilation
and break the `csc` single-exe model; WinForms keeps the repo's toolchain.)

## Use

pc64 dials **out** to a listener, so the client **listens** and you point the
device at it:

1. Run `UnoRemote.exe`, set the port (default 5099), click **Listen**.
2. On the device stick's `DEBUG.CFG` (formerly `STRESS.CFG`), set `remote=<this-pc-ip>:5099`
   (QEMU SLIRP guest: `remote=10.0.2.2:5099`) and boot a **debug** build.
3. The desktop appears. Click and type on the view to drive the device. Use
   **Scale** (1x…4x) to trade resolution for bandwidth on a busy/hi-res screen.
4. **Record** captures the session to `Videos\UnoRemote\`: an `.mp4` if ffmpeg
   is found (beside the exe, or on PATH), otherwise a timestamped PNG frame
   folder ("still lands video" either way).
5. The command box sends any raw URC line (`probe`, `vols`, `launch 0`,
   `py print(6*7)`, `reboot`; `/msg …` for a free-form message).

URC is **plaintext, LAN-only, UNO_DEBUG-only** — do not expose the listener to an
untrusted network.

## Files

| File | Role |
|---|---|
| `Urc.cs` | C# port of `UnoAutoLink` (`../tools/unoauto_remote.py`): TCP listen, HELLO, CMD/RSP correlation, verb wrappers, `screen` grab reassembly (full + `ScreenGrabDelta`) |
| `Qoi.cs` | QOI decoder → `Bitmap`, matched to the encoder in `../unoauto_screen.c` |
| `Recorder.cs` | Session recording: ffmpeg (raw BGRA → MP4/H.264) or a PNG frame sequence. Used for both live client-side recording and replaying a server-side `screen record` capture |
| `RemoteMain.cs` | WinForms UI: live view, input mapping, log pane, command box, record toggle |
| `build-remote.ps1` | csc build → `build/UnoRemote.exe` |
| `app.manifest` | Per-monitor DPI aware so the live view stays crisp |

## Follow-up slices

- ~~Dirty-rect / delta frame streaming for higher FPS.~~ **Done** — `screen grab
  delta` sends only changed tiles; the client composites onto a persistent canvas.
- ~~Server-side capture (record on the device itself).~~ **Done** — the **"on
  device"** box records on the device tick at a steady fps (its own snapshot);
  Stop pulls the ring and reconstructs the frames into an MP4 / PNG sequence.
- ~~A clickable command-GUI for the URC verbs, growing from the raw command
  box.~~ **Done** — a verb bar under the log runs read-only verbs immediately,
  prefills the box for verbs that take an argument (`launch…`, `guard…`, …), and
  confirms `reboot`/`poweroff`. The raw box remains for anything else.
- A macOS client (Avalonia) reusing `Urc.cs` / `Qoi.cs`.
