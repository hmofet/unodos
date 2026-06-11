# UnoDOS/Mac — classic Mac OS ports (milestone 2)

Two Macintosh ports of the UnoDOS desktop, from **one C codebase**
(`unodos.c`), built with the [Retro68](https://github.com/autc04/Retro68)
toolchain and verified running under the ROM-free
[Executor](https://github.com/autc04/executor) emulator:

| App | Era | Hardware | Graphics |
|---|---|---|---|
| **UnoDOS7** | System 7 | Mac II / LC / Quadra (68020+) | **Color QuickDraw**, full UnoDOS palette |
| **UnoDOSClassic** | System 1–6 | Mac Plus / SE / Classic (68000) | classic 1-bit QuickDraw, authentic black-on-white |

### System 7, color (UnoDOS7)
![color](build/color_desktop.png)

### System 1–6, monochrome (UnoDOSClassic)
![mono](build/mono_desktop.png)

Both screenshots are the `test` builds (apps auto-launched) running under
Executor: **Notepad** topmost with demo text, a visible caret, and the live
`Ln/Co/bytes` status bar; the **Files** directory listing stacked behind
(real File Manager contents); **Music** playing Canon in D underneath. The
color build renders the literal UnoDOS palette; the mono build is the
canonical 1-bit Mac look.

## Milestone 2 — the app trio

- **Files** — volume directory listing via `PBGetCatInfo` (name + size,
  `<DIR>` markers), arrow-key/click selection with a themed selection bar,
  scrolling, `R` refresh. Enter or double-click opens the file in Notepad.
- **Notepad** — text editor: caret, insert/backspace/return, arrow-key
  navigation (incl. up/down with column memory), vertical scrolling, and
  the **live status bar** (`Ln 3  Co 25  67 B *`) that updates on every
  keystroke — the x86 audit's stale-status fix is law here. `Cmd-S` saves
  through the File Manager (create/write/flush); files opened from Files
  keep their name, new text saves as `UNTITLED.TXT`.
- **Music** — the same Canon in D arrangement as `apps/music.asm`, played
  on the Sound Manager square-wave synth (`noteSynth`/`noteCmd`), with a
  staff view, per-note playback highlight, and Space to play/stop. If the
  Sound Manager is unavailable the app runs visual-only.

## Strategy

Per `docs/PORT-SPEC.md` and the Toolbox-based plan in
`docs/M68K-PORT-FEASIBILITY.md`: UnoDOS owns **one full-screen GrafPort**
and runs its **own** window manager, widgets, and theme inside it. The ROM
Toolbox supplies the screen, the Event Manager (mouse + keyboard, already
press-time stamped — PORT-SPEC §3 for free), QuickDraw / Color QuickDraw
primitives, and `TickCount`. The window manager, event routing, desktop,
and the SysInfo/Clock apps are the same model as the x86 and Amiga ports.

The **only** difference between the two apps is the theme layer
(`#if UNO_COLOR`): Color QuickDraw `RGBForeColor`/`PaintRect` vs. classic
1-bit patterns. Everything else — the WM, z-order, drag with clamping,
click-to-raise, the press-time click latch, the apps — is shared. System
1–6 had no Color QuickDraw, so the mono build avoids those calls entirely
and targets the 68000.

## Build

Needs the Retro68 toolchain (a GCC cross-compiler for classic Mac):

```sh
# one-time: build Retro68 (Linux/macOS/WSL). ~30-45 min.
git clone --recursive https://github.com/autc04/Retro68.git
mkdir Retro68-build && cd Retro68-build
../Retro68/build-toolchain.bash --no-ppc --no-carbon

# build both apps
cd unodos/mac
R68=/path/to/Retro68-build ./build.sh
```

`build.sh` produces, for each app, a `.bin` (MacBinary), `.APPL`
(application), and `.dsk` (bootable 800K HFS disk image) in `build/`.

## Run

Under **Executor** (ROM-free, reimplements the Toolbox — no Mac ROM or
System install needed):

```sh
EXEC=/path/to/executor ./run.sh color      # UnoDOS7
EXEC=/path/to/executor ./run.sh mono        # UnoDOSClassic
./run.sh color test                          # auto-launch the apps
```

Point Executor at the **`.APPL`**, not the `.dsk` — the `.APPL`
auto-launches our app, while the `.dsk` opens Executor's Browser (and
trips a PBClose hang on the color disk). For the GUI window you need a
display (WSLg, X, or Wayland).

On **real hardware** or a ROM-based emulator (Mini vMac for mono, Basilisk
II for color — both need a user-supplied Mac ROM and System), copy the
`.bin` over and it runs as a normal application.

## Architecture

`unodos.c` is one file, structured along the portable-core boundary the
feasibility plan calls for: the theme layer, window manager, event/desktop
logic, and app procs are cleanly separated, with `#if UNO_COLOR` confined
to the theme. `CMakeLists.txt` emits both shipping apps plus `*Test`
variants (`-DUNO_AUTOTEST`) that auto-launch the apps for screenshot
verification without host→guest input injection.

## Known limitations (milestone 2)

- Single cooperative event loop (the scheduler is scaffolding); the
  WM/app tables already support more.
- Notepad: no horizontal scroll (long lines clip), 4 KB buffer, no
  find/replace; Files: 24-entry listing cap.
- Uses our own full-screen GrafPort and chrome rather than real Mac
  windows — intentional (PORT-SPEC: one GrafPort, our WM), so the look is
  identical across color/mono and matches the other ports.
- Drawing uses QuickDraw primitives directly; no offscreen GWorld
  double-buffering yet (the topmost-only repaint keeps flicker low).
