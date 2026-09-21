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
--dir PATH       a folder for the drawn Open/Save dialog (repeatable)
--size WxH       initial window size
--shot out.ppm   render one frame and exit
--script FILE    replay input (text / key / click / shot / frames / pick), then exit
```

F11 or Alt+Enter toggles fullscreen.

## Test

```bash
tests/smoke.sh <build dir> [.exe]      # under xvfb-run on Linux
```

The smoke test drives each app through the shell's real dispatch path:
typing, saving, a formula, and the slide show in and out. It opens a document
from a *second* folder, edits it and plain-saves it, and checks the save went
back to that folder. It then checks that UnoWord and UnoCalc wrote
compound-file documents. The OS file dialog can't be scripted, so `pick`
supplies its answer; everything after the dialog is the real code. CI runs the
test on all three OSes, and under ASan+UBSan on Linux and macOS.

## Known limits

These come from the apps or the pc64 contract they're written to. The
desktop shell doesn't cause them, and it shouldn't hide them either.

- **ASCII text only.** The document model is single-byte, so non-ASCII typed
  characters are dropped instead of being inserted as raw UTF-8.
- **No clipboard.** The apps' Cut/Copy/Paste aren't implemented yet, so there's
  nothing to connect to the OS clipboard.
- **UnoWord:** the arrow keys don't move the caret, and Bold/Italic apply to a
  selection, not to text typed afterwards. The pc64 build behaves the same.
- **No unsaved-changes prompt** on close: the apps don't report whether a
  document is dirty.
- **Unsigned binaries** (see *Install*). Proper signing needs an Apple
  Developer ID ($99/yr, plus notarization) and a Windows code-signing
  certificate.
- **No file associations** yet: the apps can't open a file passed on the
  command line, so double-clicking a `.doc` won't launch UnoWord.
- **No HiDPI.** The chrome's 16x16 icons and metrics are drawn at 1x, so a
  framebuffer pixel is one point: macOS doubles it crisply on Retina
  (nearest-neighbour), while Windows at 150-200% scales it with some softness.
- On Win64, the apps declare `malloc(unsigned long)`, which is 32-bit there.
  That's harmless for the allocation sizes involved, and pc64's own mingw
  build does the same.
