# UnoDOS/Mac — classic Mac OS ports (milestone 3)

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

## Apps

Eleven apps from one codebase (the color-only **Theme** app makes it
eleven on UnoDOS7, ten icons on UnoDOSClassic):

- **Sys Info**, **Clock** — system panels.
- **Files**, **Notepad**, **Music** — the milestone-2 trio (below).
- **Theme** (color targets) — the 8 preset palettes shared with the x86
  and Amiga ports, plus per-channel custom RGB editing of the UI colors.
- **Dostris** — port of `apps/tetris.asm`: same piece tables, scoring
  and speed curve; the seven VGA piece colors with bevel highlights at
  16 px cells; Korobeiniki through the Sound Manager.
- **OutLast** — port of `apps/outlast.asm`: same track, perspective,
  traffic and physics; full `outlastv`-style scenery in a 480×300
  playfield (gameplay math stays in the faithful 320×200 virtual space,
  rendered at 3/2); Sunset Drive music.
- **Pac-Man** — port of `apps/pacman.asm`: full 28×25 maze, three-ghost
  AI (Blinky red, Pinky pink, Clyde orange), scatter/chase schedule,
  frightened mode with the 200→1600 chain.
- **Tracker** — the shared 32-row × 4-channel pattern editor
  (byte-identical SONG.TRK format) on up to four Sound Manager
  square-wave channels; the noise column plays a low thump. s/l save
  and load through the File Manager.
- **Paint** — the MacPaint-style editor: tool palette (pencil, brush,
  eraser, line, rect, filled rect, oval, filled oval, flood fill,
  spray), drag drawing with rubber-band shapes, white canvas with a
  byte-per-pixel backing store. Color selector: the full 8-bit gamut
  (a 256-swatch picker) on UnoDOS7; authentic 1-bit dither patterns
  on UnoDOSClassic. PAINT.UNO save/load.

A platform-themed splash (a happy compact Mac + "UnoDOS 3") shows on
boot on both targets.

On the color targets, game art draws in **true RGB** through 8-bit
Color QuickDraw (`fill_rgb`, nearest of the 256 system colors); each
color carries a 4-slot fallback so the mono target keeps its authentic
1-bit look.

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

## Milestone 3 — multitasking + PC floppies (2026-06-12)

- **Cooperative scheduler**: every window's app proc runs in its own
  task with a private heap stack and a one-slot event mailbox — the
  same model as the Amiga/Genesis schedulers. The context switch is
  ~6 lines of 68K asm (callee-saved `movem` + SP swap); `StkLowPt`
  is cleared so the classic stack sniffer accepts the task stacks
  (the Thread Manager's own convention). Keys post with a bounded
  yield-retry so typing bursts survive the single mailbox slot.
- **PC-compatible floppies (FAT12 read/write)**: a portable FAT12
  core in C over an injectable 512-byte block device. The Files app
  cycles volumes with `V` (HFS ↔ PC disk); opening a FAT file loads
  it into Notepad and Cmd-S writes it back to the volume it came
  from. The real device is raw `.Sony` driver sector access; under
  Executor (no floppy emulation) a RAM-backed FAT12 image stands in
  so the whole stack stays emulator-testable. PC interchange is
  verified by `mac/test_fat12.py`, which compiles the same core
  host-side, writes an image through it, and re-reads it with an
  independent FAT12 parser byte-for-byte.

**Booting vs. reading PC disks**: the Mac targets are Toolbox
*applications*, not a bootable OS — the machine boots classic Mac OS
from an HFS volume (Macs cannot boot from FAT media at all), and
UnoDOS launches as an app. PC floppies are **data volumes only**,
mounted from inside the running app via Files → `V`. Hardware note:
only SuperDrive (FDHD) machines can physically read 1.44 MB MFM PC
disks; the 800K GCR-only drives in the Mac Plus and early SE cannot,
regardless of software.

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
variants that auto-launch the apps for screenshot verification without
host→guest input injection: `UnoDOS7Test`/`UnoDOSClassicTest` (app
stack), `UnoDOS7FTest` (Files enters a subdirectory), `UnoDOS7ThTest`
(applies the Sunset theme), `UnoDOS7SpTest`/`UnoDOSClassicSpTest`
(long-hold splash), `UnoDOS7DtTest`/`UnoDOS7OlTest`/`UnoDOS7PmTest`
(the games play real moves at boot).

## Known limitations (milestone 3)

- Notepad: no horizontal scroll (long lines clip), 4 KB buffer, no
  find/replace; Files: 24-entry listing cap (16 on the PC volume).
- The FAT12 `.Sony` raw-sector path is real-hardware-only (Executor
  has no floppy); the RAM-image fallback covers the logic in CI.
- Tracker polyphony depends on the Sound Manager mixing multiple
  square-wave channels (System 6.0.7+/real hardware; Executor may
  voice fewer).
- The milestone-3 features are build- and host-test-verified; an
  Executor visual pass is pending (no automated capture path in the
  current rig — a Windows-native Executor would fix that).
- Executor quirk: TickCount() advances much faster than 60 Hz, so the
  ~2s splash hold races by under Executor; timing is correct on real
  hardware. UnoDOS7SpTest holds long for screenshot runs.
- Uses our own full-screen GrafPort and chrome rather than real Mac
  windows — intentional (PORT-SPEC: one GrafPort, our WM), so the look is
  identical across color/mono and matches the other ports.
- Drawing uses QuickDraw primitives directly; no offscreen GWorld
  double-buffering yet (the topmost-only repaint keeps flicker low).
