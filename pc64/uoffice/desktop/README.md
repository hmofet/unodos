# UnoOffice for Windows, macOS and Linux

UnoWord, UnoCalc and UnoShow run as ordinary desktop applications, compiled
from **the same source files** pc64 links into `APPS\UO*.UNO`. Nothing under
`pc64/apps/` or `pc64/uoffice/` is forked or `#ifdef`'d for the desktop.
`[EXPERIMENTAL]`, part of the unoffice lane (see [`../UOFFICE.md`](../UOFFICE.md)).

They're **standalone**. Each app is a single native program with its UnoDOS
host built in. There's no UnoDOS, emulator or VM involved, no third-party
toolkit, and nothing else to install besides the app.

## Install

| OS | package | what you get |
|---|---|---|
| Windows 10/11 x64 | `UnoOffice-<ver>-windows-x64.exe` | an installer: Program Files, Start-menu entries, uninstaller in *Installed apps* |
| | `UnoOffice-<ver>-windows-x64.zip` | the same, portable: unzip anywhere and run |
| macOS 11+ (Apple silicon and Intel) | `UnoOffice-<ver>-macos-universal.dmg` | drag the three apps to Applications |
| Debian / Ubuntu 22.04+ | `unooffice_<ver>_amd64.deb` | `sudo apt install ./unooffice_*.deb` |
| Fedora / RHEL / openSUSE | `unooffice-<ver>-1.x86_64.rpm` | `sudo dnf install ./unooffice-*.rpm` |
| other Linux | `UnoOffice-<ver>-linux-x86_64.tar.gz` | unpack; `bin/unoword` etc. |

Every installer registers the apps for their documents: `.doc`/`.docx`
open in UnoWord, `.xls`/`.xlsx` in UnoCalc, `.ppt`/`.pptx` in UnoShow.
Windows 10 and 11 let only the user choose a default, so there the apps
appear under *Open with* and in *Settings > Default apps* without taking
the files away from whatever already opens them. macOS and Linux desktops
offer them the same way (*Open With*, the file manager's application list).

The builds aren't code-signed (the Mac ones are ad-hoc signed), so the
first launch needs one extra click. On Windows, SmartScreen: *More info >
Run anyway*. On macOS: right-click the app > *Open*, or *System Settings >
Privacy & Security > Open Anyway*.

Releases are published by pushing a tag `uoffice-v<version>` that matches
`project(VERSION)` in `CMakeLists.txt`. The `uoffice-release` workflow builds
every package, installs each one on its own OS, launches the installed apps,
and only then attaches the packages to a GitHub release.

## How it works

Two layers, with one rule between them:

- **Ours, identical on every OS:** the window's *contents* and the place they
  are drawn. `pc64/fb.c`'s software framebuffer, painted by unoui, the Office
  97 chrome and the apps.
- **The OS's own API, used directly:** everything that belongs to the OS.
  That's the top-level window that shows our pixels, keyboard and mouse
  input, the title bar, fullscreen, and the File › Open / Save As dialogs.

The seam is [`uodesk_plat.h`](uodesk_plat.h), with one backend per OS:

| | Windows `plat_win32.c` | macOS `plat_cocoa.m` | Linux `plat_x11.c` |
|---|---|---|---|
| window + blit | `CreateWindowExW` + `SetDIBitsToDevice` | `NSWindow` + an `NSBitmapImageRep` over the frame | `XCreateWindow` + `XPutImage` |
| keyboard | `WM_KEYDOWN` / `WM_CHAR` | `keyDown:` | `KeyPress` via XIM (dead keys, compose) |
| mouse | `WM_*BUTTON*`, `WM_MOUSEWHEEL` | `mouseDown:` ..., `scrollWheel:` | `ButtonPress`, `MotionNotify` |
| title / fullscreen | `SetWindowTextW` / borderless monitor-sized window | `title` / `toggleFullScreen:` (its own Space) | `_NET_WM_NAME` / `_NET_WM_STATE_FULLSCREEN` |
| Open / Save As | `GetOpenFileNameW` / `GetSaveFileNameW` | `NSOpenPanel` / `NSSavePanel` | the XDG desktop portal over D-Bus (GNOME's or KDE's own dialog), else zenity / kdialog |
| clipboard | `CF_UNICODETEXT` | the general pasteboard | the `CLIPBOARD` selection, served by the app |
| a document to open | `"%1"` on the command line (UTF-16, via `CommandLineToArgvW`) | the open-document Apple event (`application:openURLs:`) | `%F` on the command line |
| HiDPI | per-monitor DPI aware (v2), `WM_DPICHANGED` | the backing scale (Retina), `windowDidChangeBackingProperties:` | `GDK_SCALE`, else `Xft.dpi` |
| links against | user32, gdi32, comdlg32, shell32 | Cocoa | libX11, libdbus-1 |

`uodesk.c` is a **shell**, a small stand-in for `pc64_uui.c`. It gives a module
what pc64 gives it:

| pc64 provides | the desktop provides |
|---|---|
| the GOP framebuffer, sized at runtime | `pc64/fb.c` unchanged, sized to the OS window, handed to the backend each frame |
| a unoui window with a title bar | the OS window. The app's unoui window stays in unoui's **fullscreen** mode, so its canvas gets the whole framebuffer and every event. The app's title ("UnoWord - Document1") goes to the OS title bar |
| `pc64_shell_*` services | `pc64_shell_dirty`, `_workarea_w/h` (= window size), `_fullscreen` (= the OS window going fullscreen, used by UnoShow's slide show) |
| HID keyboard as UEFI `(uni, scan, ctrl)` | the backend's keys translated into the same codes. The module gets each key first, and whatever it doesn't handle goes to unoui, same order as `pc64_uui.c`. Cmd is treated as Ctrl on a Mac |
| the Office 97 Open / Save As dialog | the OS dialog, through `uof_set_native()` (see `../uofile.h`). The chosen file's folder becomes a volume, so the app still addresses it as (volume, name), and a plain Save goes back to that folder. If the OS has no picker, the Office 97 dialog is drawn as before |
| `uno_fs_*` over FAT volumes | `uodesk_fs.c`: each **folder** is a volume (Documents, Desktop and home by default, or `--dir`, plus every folder the picker opens) |
| TTF faces on the ESP | `fonts/` beside the executable (`Contents/Resources/fonts` in a Mac bundle, `share/unooffice/fonts` in the Linux packages) |
| Settings > UI scale | the display's scale: the frame is in device pixels, and the UI is drawn at the matching scale (`uno_font_set_ui_scale` + `uoc_set_scale`, the same two calls pc64's setting makes), so text is sharp at 150% and on Retina |
| (nothing: pc64's shell cannot ask a module these) | the host seam in [`../uoapp.h`](../uoapp.h): the close box asks the app, which puts up "Do you want to save the changes...?" when there are any; a file the OS hands over is opened through the same prompt; and the app's Cut/Copy/Paste go to the OS clipboard |

The text engine (`pc64_font.c` + stb_truetype), unoui, unodoc and um_inflate
are all linked unchanged, so the documents you save are the same `.doc` / `.xls`
/ `.ppt` bytes pc64 writes.

## Build

You need CMake 3.16+, a C99 compiler, and Python 3 (Python generates the 8x8
fallback font header, the same way `pc64/build.sh` does). There are no other
dependencies beyond the OS's own development files:

```bash
cmake -S pc64/uoffice/desktop -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

- **Linux:** `apt install libx11-dev libdbus-1-dev` (Fedora: `libX11-devel
  dbus-devel`). Produces `unoword`, `unocalc`, `unoshow`. Without libdbus the
  picker falls back to zenity/kdialog.
- **macOS:** Xcode's command-line tools. Add
  `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` for universal apps.
- **Windows:** MSYS2 MINGW64 (`mingw-w64-x86_64-gcc`, `-cmake`, `-python`),
  plus `-DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc"` so each `.exe`
  imports only Windows' own DLLs. To cross-compile from Linux, use
  `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake`.

Then, in the build directory:

```bash
cpack                  # Windows: NSIS + ZIP (NSIS must be installed)
                       # macOS:   DMG
                       # Linux:   DEB + RPM (needs rpmbuild) + TGZ
```

None of this needs virtualization: a Windows installer builds on Windows
(MSYS2 + NSIS), a DMG needs a Mac, and the Linux packages build on any Linux.
CI (`.github/workflows/uoffice-desktop.yml`) does all three and
install-tests them.

The app icons in `packaging/icons/` are rendered from the UnoDOS shell's own
emblems (`pc64/pc64_icons.c`) by `packaging/mkicons.c`. Regenerate them with
`cmake --build build --target uodesk_mkicons && build/uodesk_mkicons packaging/icons`
(needs zlib).

## Options

```
unoword [options] [FILE]

FILE             a document to open (what a double click passes)
--dir PATH       a folder for the drawn Open/Save dialog (repeatable)
--size WxH       initial window size, in points
--scale PCT      the UI scale, 100-200 (default: the display's; also
                 the UNOOFFICE_SCALE environment variable)
--shot out.ppm   render one frame and exit
--script FILE    replay input, then exit; see uodesk.c for the commands
                 (text, key, click, shot, frames, sleep, pick, open, quit,
                 clip, expect title/dirty/clip, passed)
```

F11 or Alt+Enter toggles fullscreen. F10 or Alt+letter opens the menus.

## Test

```bash
tests/smoke.sh <build dir> [.exe]      # under xvfb-run on Linux
```

The smoke test drives each app through the shell's real dispatch path:
typing, saving, a formula, and the slide show in and out. It opens a document
from a *second* folder, edits it and plain-saves it, and checks the save went
back to that folder, and opens one from the command line. It drives the
unsaved-changes prompt down all four paths (Cancel, No, Yes in place, Yes
through Save As), the clipboard in both directions, UnoWord's caret keys,
text beyond ASCII through `.doc`, `.docx` and `.xls` and back, and the UI at
150% and 200%. The OS file dialog can't be scripted, so `pick` supplies its
answer; everything after the dialog is the real code. CI runs the test on all
three OSes, and under ASan+UBSan on Linux and macOS, then installs each
package and checks that the OS opens a document with it.

## Known limits

These come from the apps or the pc64 contract they're written to. The
desktop shell doesn't cause them, and it shouldn't hide them either.

- **Western European text only.** The documents are CP-1252, the encoding
  unodoc reads and writes for all six formats, so accents, the euro sign and
  curly quotes work everywhere, and a character CP-1252 has no byte for
  (Greek, Cyrillic, CJK) is typed and pasted as `?`. Lifting that is a wider
  internal encoding in unodoc, not something the apps can do alone.
- **UnoWord saves character formatting per paragraph.** unodoc's Word writer
  takes bold, italic and alignment for a whole paragraph (`ud_docw_para`), so
  a bold word inside a plain paragraph is written with the paragraph's first
  character's formatting. The editor itself keeps it per character.
- **UnoCalc's Cut** clears the cells straight away rather than moving them on
  Paste, and a paste from another program types the values in (formulas are
  shifted only when the copy came from UnoCalc).
- **UnoShow's clipboard is text and whole shapes**; there is no selection
  inside a text box.
- **The UI scale stops at 200%**, the font engine's ceiling. A 250% or 300%
  display draws the UI at 200%, which is small but sharp.
- **Unsigned binaries** (see *Install*). Proper signing needs an Apple
  Developer ID ($99/yr, plus notarization) and a Windows code-signing
  certificate.
- **One document per window.** Opening a file while one is open replaces it
  (after the save prompt); on Windows and Linux a second double click starts
  a second window, on macOS it comes to the running one.
- On Win64, the apps declare `malloc(unsigned long)`, which is 32-bit there.
  That's harmless for the allocation sizes involved, and pc64's own mingw
  build does the same.
