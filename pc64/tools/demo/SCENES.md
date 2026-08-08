# SCENES.md - the demo-video scene driver (QEMU rehearsal)

`scenes.py` is the deterministic, rerunnable driver for the pc64 demo video's
QEMU rehearsal cut. It boots the DEBUG image **once**, then per scene records
one mp4 through **unostream** (`pc64/UNOSTREAM.md`): a fresh `stream_recv.py`
on its own host port, `stream start 10.0.2.2 <port> 30` over URC, the scene's
beat list at video pacing, `stream stop`, reset, next scene.

```
UNO_DEBUG=1 ./build.sh                      # from pc64/, WSL or Git Bash
cd tools/demo
python3 scenes.py --list                    # the spine
python3 scenes.py --scene s02               # one scene
python3 scenes.py --all                     # the whole spine, one boot
```

Run under WSL (`qemu-system-x86_64`, OVMF, `sgdisk`, mtools, `ffmpeg` - the
same toolchain every `tools/*_qemu.py` gate needs).

## `--metal`: recording real hardware

The final cut is filmed on the **X13 Yoga**, booting a stick built from master
with `DEBUG.CFG` = `nohud`, `nostress`, `noshutdown`, `ui-unlock`,
`remote=192.168.2.100:5101`. The box **dials OUT** to that address and re-dials
about 45 s after every reboot, so the driver has to run on **devbuntu**
(192.168.2.100) - a Windows dev box cannot accept the inbound connection.

```
sh tools/demo/deploy.sh                     # scp the driver + corpus to devbuntu
ssh devbuntu
  pkill -f '[w]atcher.py'                   # frees :5101 if a watcher holds it
  cd ~/demo && python3 scenes.py --metal --all
```

`--metal [PORT]` (default 5101) changes four things and nothing else:

- **No boot, no staging.** It binds `0.0.0.0:PORT` and waits up to 10 minutes
  for the box to dial in, so it can be started before the box is. `remote_qemu`
  is imported defensively, so the module loads on a host with no QEMU/OVMF.
- **The stream target is a LAN address**, derived from the live connection's
  peer (`_local_ip_toward`), not hardcoded. `--stream-host IP` overrides.
  The receiver binds `0.0.0.0` instead of loopback.
- **Assets are re-probed on the stick** (`probe_metal_assets`): the volume
  carrying `DOOM1.WAD` / `DOCS\` is found at runtime rather than trusting a
  QEMU-era index. The office documents are still pushed to the RAM volume, from
  `./corpus` (deploy.sh puts them there - the repo is not on devbuntu).
- **The box is never powered off** on teardown. s09 (appliance) is QEMU-only.

## Resolution

`--min-width` (default 1024) raises the desktop **once at session start**,
before any stream exists - a resolution change mid-stream makes the guest emit
a fresh hello, which stream_recv treats as a reset and splits the mp4 into
`-2.mp4`. QEMU boots at 640x400, which is too small to film; the raise takes it
to 1280x800. Pass `--min-width 0` to leave it alone.

It is driven entirely by keyboard, because the screen changes size underneath
the sequence and any coordinate read beforehand would be stale halfway through.
Three things about that flow are not what the layout suggests, and each one
failed *silently* before it was pinned down:

- A successful Apply **disables the Apply button** (`g_res_sel ==
  res_active_index()`, pc64_uui.c:1132) and unoui refuses focus to a disabled
  widget (`interactive()`, unoui_input.c:89), so **Tab skips it**. Any Tab count
  derived from the visible layout is one too many and lands on "Revert now".
- The rebuilt panel leaves focus **on Keep** already, so the confirm is a bare
  Enter - no walk at all. (Verified from a screenshot: Keep carries the focus
  ring, Apply is greyed.)
- The mode reads back as the NEW size for the whole 15 s probation window, so a
  prompt check **cannot tell "kept" from "about to revert"** and reports success
  either way. The verification therefore sleeps past `RES_CONFIRM_S` and asks
  again. Doing nothing is always safe: unconfirmed modes revert themselves.

## Outputs (per scene, into `out/`)

| file | what |
|---|---|
| `sNN.mp4` | the recording (stream_recv -> ffmpeg, 30 fps container) |
| `sNN.timing.jsonl` | one line per received frame: index, wall clock, bytes, type |
| `sNN.beats.jsonl` | one line per beat: `{"beat": name, "t": wall-clock}` |
| `sNN.png`, `sNN.stats.json` | final canvas + receiver counters |

`out/probe/` holds development screenshots (`Demo.shot`), not deliverables.

## The spine

| scene | what it shows | notes / guards |
|---|---|---|
| s02 | WM: Start menu rises, Files/Editor/Clock, title-bar drag to the right edge (snap tween), the F2 switcher, "To desktop 2" via the title-bar menu, desktop switch and back, close all | Alt-Tab is undrivable over URC (no ALT bit): F2 is the shell's own no-Alt switcher and is what the scene uses. The desktop move rides the title-bar right-click menu, not Alt+Ctrl+F2, same reason. |
| s03 | Themes: Aurora Dark -> Mac OS 7 -> Windows 3.1 -> C64 -> Aurora Light, ~1.5 s hold each; a wallpaper on and off; ends on the Aurora default | Driven by keyboard through Control Panel > Personalization (tab strip -> Theme dropdown; Down cycles live). |
| s04 | Office: Files shows the staged docs (RAM volume); UnoWord opens `fmt.doc`; UnoCalc opens `formulas.xls`, clicks a formula cell; UnoShow authors a titled slide | fmt.doc/formulas.xls are `put` onto the RAM volume at runtime and opened there - the shared Open dialog lists a volume's ROOT only (uofile.c) and cannot reach `DOCS\`. **UnoShow cannot open small.ppt in this build** - two source bugs, both outside this lane: (1) the native-FAT lister marks every file as a directory (`uno_fat_list_ex` returns >=0 for a file path), so the Open dialog appends `\` and never mirrors a filename into the File-name field; (2) UnoShow's dialog key-bridge (`apps/uoshow.c`) swallows Backspace (uni 8), so the stale name can't be cleared to type the real one either. The file is fine (`uno.read` returns 463 KB of valid CFB). The beat degrades to authoring a titled slide. UnoWord "scroll" is a select-all - UnoWord scrolls by mouse WHEEL only, which URC cannot inject. |
| s05 | Browser: `uno:script` (JS writes the page body), scroll; `uno:engine`; switch the script engine by navigating `uno:engine/quickjs`; `uno:script` again on QuickJS | Local pages only. `--with-net` is a stub: HTTP/HTTPS beats are metal-only and log-skip here. No built-in `uno:` page mutates the DOM on a timer/click; `uno:script`'s document.write output is the visible JS payoff. |
| s07 | Studio: File > New, type a small UnoC app live, Ctrl-S, Ctrl-B build, Ctrl-R run, the app opens, close | The typed program is `STUDIO_PROG` in scenes.py (AppInterface + text_at, the SAMPLE.C shape). |
| s08 | Duum: Files -> ESP -> `APPS\` -> `DUUM.UNO` -> Enter (a PYAPP is a document PYRT opens - no Start-menu row), walk + turn, close | Guard: no WAD in `pc64/wads/` = clean no-op with a log line. Never downloads. Falls back to `uno.run_app` over URC if the Files walk misses. |
| s09 | Appliances: vmgr Start, the Linux guest boots (BEFORE the stream - it gets ~4 ms/frame, boot is minutes of dead air), then on camera: `ls /` + `uname -a` into the console | Guards: needs `build/bzImage` + `initrd.gz` (vm_stage payload; `VMS.CFG` row is written by the driver - vm_stage.py doesn't), AND `UNO_DEMO_KVM=1`. Plain TCG can never host a guest (TCG drops vmx; `-m 512` is under the 1800 MB carve floor; eligibility needs a `UNO_DETACH=1` build) - the harnesses all use `-m 4096 -cpu host -enable-kvm`, so that is what `UNO_DEMO_KVM=1` boots. On an AMD host it stays skipped (the SVM backend has never completed a VMRUN). |
| s10 | System readout (hold), the log viewer raised to info level, browser navigations landing in the tail, close | The level goes up BEFORE the traffic (a dropped record is gone). |

s01/s06/s11 (boot, media/audio, outro) are deliberately absent - a different
pipeline owns them.

## Pacing rules (the point of the file)

- ~0.5-1.0 s settles between beats; long holds where the payoff is a still
  (a redressed theme, a device readout).
- Pointer travel is a **glide**: many `pointer` moves ~12 px apart, ~35 ms
  apart host-side, a 0.4 s breather every 20 moves (the injected-pointer
  queue is 32 deep with a 2-frame dwell - a burst overflows silently).
- A click is **three injections** (move, press, release): the shell samples
  pointer state per frame, so a press+release inside one sample cancels out.
- A drag holds 0.35 s after the grab and before the drop, so it reads as one.
- Launches go by **id** (`launch files`) except where the Start menu itself
  is the shot; the menu ends at Shut Down - never arrow past the bottom.
- The browser address bar has no select-all: End + 40 backspaces, then type.
- Between scenes: every desktop visited and emptied, back to desktop 1.

## DEBUG.CFG

The driver replaces the shipped DEBUG.CFG (via `remote_qemu.build_disk`, then
its own mcopy) with:

```
remote=10.0.2.2:5399
nonet
nostress          <- the fuzz driver's real off switch (it opens a random app
                     every few frames and would fight the choreography)
noshutdown        <- belt-and-braces on the stress auto power-off
```

## Coordinates and resolution

Every coordinate here was first read off a **640x400** probe shot, and metal
runs at 1280x800. They fall into three classes and only the third needed work:

1. **Window-relative, with a hardcoded window origin** - Control Panel (150,24),
   Studio (24,20), UnoCalc (24,20), UnoShow (20,16), Files (120,64), Editor
   (90,36). Menu bars, toolbars and grids are laid out from that corner, so
   these are resolution-independent and stay as literals:
   `STUDIO_FILE_XY`, `STUDIO_NEW_XY`, `UOF_MENU_FILE`, `UOF_MENU_OPEN`,
   `UOCALC_A2`, the Files volume dropdown, the Editor title bar.
2. **Already computed** from the live size - the snap edge is `d.w - 4`.
3. **Screen-centred or clamped**, i.e. they MOVE with resolution. Baked
   literals would have missed every one of these, so they are derived at
   runtime:
   - the shared **Open dialog** - `Demo.dlg()` applies `uod_open`'s own formula
     (`x=(sw-dw)/2`, `y=(sh-dh)/3`) to the live size. Verified: at 640x400 it
     predicts x0=173, which matched the probe shot to the pixel.
   - the **title-bar context menu** - `Demo.rclick_menu()` grabs before and
     after the right-click and takes the changed region as the popup, then
     indexes rows within it. The popup is anchored at the click but clamped
     against the taskbar, so its position is not predictable from the anchor.
   - **UnoShow's slide page** - `Demo.slide_rect()` finds the white page
     bounded by the grey mat on a live grab; `UOSHOW_TITLE_F` is a *fraction*
     of that page, not a pixel.

Re-measure after a theme/font/UI-scale change. Take probe shots liberally
(`Demo.shot`); read coordinates off screenshots, never compute them from
theory - menu bands move.
