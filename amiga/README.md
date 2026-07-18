# UnoDOS/68K — Amiga port (milestone 3)

A bare-metal Motorola 68000 port of UnoDOS for A500-class Amigas
(OCS/ECS, 512 KB chip RAM), per the platform contract in
[`docs/PORT-SPEC.md`](../docs/PORT-SPEC.md). It boots from a
self-booting ADF, takes over the machine (no Kickstart libraries used at
runtime), and runs the UnoDOS desktop with in-kernel apps.

## What it does

- **Boot splash** — striped Amiga-checkmark art with "UnoDOS 3" rendered
  at 2× and "for Commodore Amiga" (~2 s, raster-timed).
- **Display** — 320×200 in **5 bitplanes (32 colors)**, the OCS lowres
  maximum. Copper-driven: palette entries 0–3 are the themed UI colors,
  4–31 a fixed extended palette for the games (17–19 double as the
  hardware-sprite cursor registers). Per-plane software fill/text
  primitives; hardware-sprite mouse cursor.
- **Window manager** — frames, title bars, close box, z-order with
  click-to-raise, clamped XOR-outline drag; the focus-routed,
  press-time-stamped event model from the spec.
- **Audio** — 4-channel Paula: square-wave sequencers for the Music app
  and game music, all four DMA channels in the Tracker.

## Apps (11 desktop icons, two rows)

| App | Notes |
|---|---|
| **Sys Info** | machine/uptime panel |
| **Clock** | live clock |
| **Files** | ROM-disk browser; Enter opens in Notepad |
| **Notepad** | caret editor, left/right/up/down with goal-column memory, vertical scroll, live `Ln/Co/bytes` status bar, F1 save (RAM until FAT12) |
| **Music** | Canon in D on Paula (same arrangement as `apps/music.asm`) |
| **Theme** | the 8 shared preset palettes + per-channel custom RGB editing of UI colors 0–3, applied live through the copper list |
| **Dostris** | port of `apps/tetris.asm` — same piece tables/scoring/speed curve, the seven VGA piece colors, Korobeiniki on Paula |
| **OutLast** | port of `apps/outlast.asm` — same track/perspective/traffic/physics, full-color scenery, Sunset Drive on Paula |
| **Pac-Man** | port of `apps/pacman.asm` — full 28×25 maze, three-ghost AI (Blinky/Pinky/Clyde), scatter/chase schedule, frightened mode; incremental tile rendering |
| **Tracker** | write + play 4-channel MOD-style music: ProTracker periods (C-2..B-3), 32-row pattern editor, 4 chip-synthesized instruments (square/saw/triangle/noise), demo song, instant edit preview. Keys: arrows move, q/w note, e instrument, x clear, d demo, Space play/stop |
| **Paint** | MacPaint-style editor: pencil/brush/eraser/line/rect/filled rect/oval/filled oval/flood fill/spray, white canvas, rubber-band shape preview. The pen strip shows all 32 hardware pens; r/g/b tune the selected pen's 12-bit channels live through the copper, so **all 4096 OCS colors** are reachable (UI pens 0–3 locked; the game palette restores on close). s/l saves PAINT.UNO to the DF1 FAT12 disk |

## Build

Needs `vasmm68k_mot` and `exe2adf` (both shipped inside the
[amiga-debug VS Code extension](https://github.com/BartmanAbyss/vscode-amiga-debug)
VSIX), plus Python 3 and a built x86 tree (`make floppy144`) for the
shared assets:

```sh
cd amiga
./build.sh          # -> build/unodos68k.adf
./build.sh test     # -> build/unodos68k_test.adf (auto-launches apps)
```

`mkdata.py` regenerates `gen_data.i` from the x86 tree: the 8×8 font and
app icons byte-identical from the x86 binaries, the Amiga keymap, the
ROM-disk files, and the music tables (Canon in D plus the game songs,
parsed straight out of `apps/tetris.asm` / `apps/outlast.asm` and
converted to Paula periods).

### Autotest variants

WinUAE blocks host→guest key injection, so verification builds drive the
real key handlers at boot and the harness screenshots the window:

```sh
vasmm68k_mot -Fhunkexe -nosym -DAUTOTEST=1 -DAUTOTEST_<X>=1 -o out kernel.asm
```

with `<X>` one of `NOTEPAD` (demo text + six up-arrows), `THEME` (applies
the Sunset preset), `DOSTRIS` (hard-drops six pieces), `OUTLAST` (60
physics steps), `PACMAN` (150 game steps), `TRACKER` (demo song +
playback), `FAT` (mounts DF1 and FAT-chain-reads CHAIN.TXT into
Notepad), `FATW` (saves the demo text to DF1 and reopens it from disk),
`MFM` (in-RAM encoder/decoder self-test). Plain `-DAUTOTEST=1` launches
the Files/Notepad/Music stack.

## Run

WinUAE with the built-in AROS ROM (no Kickstart needed) — config in
`uae/unodos.uae`; `uae/autotest.ps1` and `uae/snapwin.ps1` drive
headless screenshot runs.

### Deterministic headless render (`uae/render.py`)

WinUAE screenshots are **not reproducible** run-to-run (wall-clock boot timing +
a live desktop clock), so they cannot byte-verify a redraw refactor. `render.py`
is a frame-deterministic renderer built on a Unicorn 68000 core (the same
approach as `macplus/harness.py`): it hunk-loads `build/UnoDOS68K_test`, models
just enough of the hardware (a `jmp (a5)` Supervisor stub, a free-running beam so
the splash's `splash_wait_frames` terminates, TBE-ready serial, and an
auto-advancing `ticks` so `fdd_delay` returns — the cursor it also drives is a
hardware sprite, absent from the bitplanes), runs the boot + the synchronous
autotest scene, and halts at the `UNODOS68K: ATDONE` serial marker (emitted right
before `main_loop`, so the framebuffer is final and the tick-driven main loop
hasn't started animating). It then reads the 5 bitplanes at the fixed chip
address `$60000` and the palette from the copper list at `$76000`, and writes a
320×200 RGB PNG.

```
python3 uae/render.py build/UnoDOS68K_test out.png
```

Two runs of the same build are byte-identical, so a redraw refactor is verified
by rendering `AUTOTEST` builds from the pre- and post-change source and `cmp`-ing
the PNGs — the discipline used for genesis/snes/c64. No display, GPU, or RDP
session required.

## Architecture

`kernel.asm` plus includes: `apps_m2.i` (Files/Notepad/Music + shared
helpers), `theme.i` (theme engine + splash), `games.i` (Dostris,
OutLast, game-music sequencer), `pacman.i`, `tracker.i`, and the
generated `gen_data.i`. The event queue, window manager, scheduler
scaffolding and app procs follow PORT-SPEC; the Amiga-specific layer
(copper, bitplane primitives, CIA/sprite ISRs, Paula) is the future HAL.

Chip-RAM map: 5 contiguous bitplanes at `$60000`, copper list `$76000`,
sprite data `$76800`, square-wave + tracker instrument samples
`$76A00`/`$76B00`, supervisor stack to `$7C000`.

## Disk-loaded apps (FAT12 on DF1)

Four apps — **Files, Theme, Dostris, Pac-Man** — are no longer assembled
into the kernel. They are separate raw binaries (`*_app.asm`, built
`-Fbin` at a fixed slot) that the kernel reads off the DF1 FAT12 disk at
runtime (via the existing `fat_read_file`) into per-window slot regions
at `$50000`, then dispatches each window's draw/key/tick through the
app's 4-entry JMP table (`open/draw/key/tick`). Several app windows can
be open at once — the windowed multitasking UX is preserved; a window's
app code is loaded when it opens (load-on-open).

Because the kernel is a hunk executable that AmigaDOS relocates to an
unknown address (on the AROS rig, into bogomem ~`$C39xxx`), apps cannot
link to absolute kernel addresses. Instead the kernel publishes a
fixed-address **API vector table** (`APIVEC` at `$77000`, filled at boot
by `apivec_init`) holding the runtime addresses of every exported
routine and shared data block; apps call/read through it by ordinal
(`KCALL`/`KDATA` macros in `sysabi.i`). `mkapi.py` reads the kernel
listing to (a) verify every exported symbol exists and (b) emit
`build/kernel_api.inc` with `VO_<field>` offsets for the shared `vars`
block, so the apps can never drift out of sync with the kernel. The
remaining apps (SysInfo, Clock, Notepad, Music, OutLast, Tracker, Paint)
are still in-kernel; converting them is the same mechanical pass.

The `.APP` binaries are written onto the DF1 FAT12 disk by `mkfat.py`
(binary mode) and appear in the Files listing alongside the data files.
Verify with `./build.sh test <APP>` and the two-floppy
`uae/unodos_diskapp.uae` config (DF0 = kernel ADF, DF1 = data disk).

## Storage (FAT12 on DF1)

`fdd.i` + `fat12.i`: the data drive DF1 holds an 880 KB **FAT12** disk
(plain ADF-shaped image, built by `mkfat.py`; PC-interchangeable at the
file level via mtools). Raw MFM track DMA with software sector
decode/encode, track cache, one-revolution writes, write-protect gate.
Files mounts it with `m`; Notepad `F1` saves to it; Tracker `s`/`l`
persists songs. The boot ROM-disk remains as a fallback listing.

## Milestone 3: cooperative multitasking

`scheduler.i`: every open window runs its app proc as a cooperative
task with a private 2 KB stack. The kernel task pumps input and posts
keys to the focused window's task mailbox and frame ticks to the
topmost; `task_wait`/`task_yield` switch full register contexts; window
create/close spawn/kill the task (ESC stays kernel-side). The app procs
themselves are unchanged - the generic task body dispatches to them.

## Known limitations (milestone 3)

- FAT12: no delete/rename yet; root-dir only; 16-file listing cap; no
  write-verify pass; Tracker saves its own format (.MOD export later).
- Notepad: 2 KB buffer; long lines clip (no horizontal scroll).
- Tracker: one 32-row pattern, fixed instrument volumes, no effect
  column yet; .MOD import/export needs FAT12.
- The vblank tick runs fast under WinUAE's default pacing for this
  config (uptime advances ~4× wall-clock) — a `TICKS_SEC` calibration
  item, not a logic bug.
- Text/fills are CPU RMW (no blitter yet — the big OCS win). Game
  windows repaint visibly; Pac-Man already uses dirty-tile rendering.
- 640-wide OCS hires (16 colors max) would need PORT-SPEC content
  scaling through the WM; lowres 32-color is the current ceiling.
- Audio is register-verified (test configs run sound off); real-hardware
  ear-check pending.
- Paint: shapes preview as a bounding-band; the flood-fill work stack
  caps at ~500 spans (very intricate regions may fill partially).
