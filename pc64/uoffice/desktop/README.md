# UnoOffice for Windows, macOS and Linux

UnoWord, UnoCalc and UnoShow run as ordinary desktop applications, compiled
from **the same source files** pc64 links into `APPS\UO*.UNO`. Nothing under
`pc64/apps/` or `pc64/uoffice/` is forked or `#ifdef`'d for the desktop.
`[EXPERIMENTAL]`, part of the unoffice lane (see [`../UOFFICE.md`](../UOFFICE.md)).

## How it works

`uodesk.c` is a **shell**, a small stand-in for `pc64_uui.c`. It gives a module
what pc64 gives it:

| pc64 provides | the desktop provides |
|---|---|
| the GOP framebuffer, sized at runtime | `pc64/fb.c` unchanged, sized to the OS window, sent to SDL2 each frame |
| a unoui window with a title bar | the OS window. The app's unoui window stays in unoui's **fullscreen** mode, so its canvas gets the whole framebuffer and every event |
| `pc64_shell_*` services | `pc64_shell_dirty`, `_workarea_w/h` (= window size), `_fullscreen` (= the OS window going fullscreen, used by UnoShow's slide show) |
| HID keyboard as UEFI `(uni, scan, ctrl)` | SDL keys translated into the same codes. The module gets each key first, and whatever it doesn't handle goes to unoui, same order as `pc64_uui.c`. Cmd is treated as Ctrl on a Mac |
| `uno_fs_*` over FAT volumes | `uodesk_fs.c`: each **folder** is a volume (Documents, Desktop, home by default, or `--dir`) |
| TTF faces on the ESP | `fonts/` beside the executable (`Contents/Resources/fonts` in a Mac bundle) |

The text engine (`pc64_font.c` + stb_truetype), unoui, unodoc and um_inflate
are all linked unchanged, so the documents you save are the same `.doc` / `.xls`
/ `.ppt` bytes pc64 writes.

## Build

You need CMake 3.16+, a C99 compiler, SDL2 and Python 3 (Python generates the
8x8 fallback font header, the same way `pc64/build.sh` does).

```bash
cmake -S pc64/uoffice/desktop -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

- **Linux:** `apt install libsdl2-dev`. Produces `unoword`, `unocalc`, `unoshow`.
- **macOS:** `-DUODESK_FETCH_SDL=ON` builds SDL2 2.30.9 from source and links
  it statically, giving self-contained `UnoWord.app` / `UnoCalc.app` /
  `UnoShow.app`. Avoid Homebrew's `sdl2`: it is now the sdl2-compat shim
  over a dynamic SDL3, which can't be bundled and misbehaves.
- **Windows:** MSYS2 MINGW64 with `mingw-w64-x86_64-SDL2`, plus
  `-DUODESK_STATIC_SDL=ON -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc"`.
  That gives `UnoWord.exe` etc., each importing only system DLLs.
  To cross-compile from Linux, use `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
  -DCMAKE_PREFIX_PATH=<SDL2 mingw devel>/x86_64-w64-mingw32`.

To ship a build, copy the executable(s) along with the `fonts/` folder next to
them. The CI workflow (`.github/workflows/uoffice-desktop.yml`) builds and
smoke-tests all three platforms and uploads packaged artifacts.

## Options

```
--dir PATH       a folder for Open/Save (repeatable; replaces the defaults)
--size WxH       initial window size
--shot out.ppm   render one frame and exit
--script FILE    replay input (text / key / click / shot / frames / size), then exit
```

F11 or Alt+Enter toggles fullscreen.

## Test

```bash
tests/smoke.sh <build dir> [.exe]      # under xvfb-run on Linux
```

The smoke test drives each app through its real event pump: typing, saving,
reopening, a formula, and the slide show in and out. It then checks that
UnoWord and UnoCalc actually wrote compound-file documents. CI runs it on all
three OSes. On Linux it runs under ASan+UBSan.

## Known limits

These come from the apps or the pc64 contract they're written to. The
desktop shell doesn't cause them, and it shouldn't hide them either.

- **Open/Save shows one folder at a time, flat.** uofile lists a volume's
  root, holds at most 64 names and truncates names at 31 characters. The shell
  lists only Office documents and extensionless files, and skips names too long
  to open. Use `--dir` to point it somewhere else.
- **The file-name field isn't focused when the dialog opens.** Click it first;
  otherwise the letters you type act as dialog mnemonics.
- **ASCII text only.** The document model is single-byte, so non-ASCII typed
  characters are dropped instead of being inserted as raw UTF-8.
- **UnoWord:** the arrow keys don't move the caret, and Bold/Italic apply to a
  selection, not to text typed afterwards. The pc64 build behaves the same.
- **No unsaved-changes prompt** on close: the apps don't report whether a
  document is dirty.
- **Unsigned binaries.** macOS Gatekeeper needs right-click > Open the first
  time, and Windows SmartScreen will warn.
- **No HiDPI.** A pixel on a Retina screen is scaled by the OS, because
  the chrome's 16x16 icons and metrics are drawn at 1x.
- On Win64, the apps declare `malloc(unsigned long)`, which is 32-bit there.
  That's harmless for the allocation sizes involved, and pc64's own mingw
  build does the same.
