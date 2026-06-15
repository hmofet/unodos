# Changelog

All notable changes to UnoDOS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [snes: milestone 1 — tile desktop, window manager, apps, soft keyboard] - 2026-06-14

The SNES port grows a real desktop. The Genesis M1 surface
(genesis/kernel.asm + softkbd.i) is re-expressed in 65816 on the SNES
shadow+DMA architecture: a cell renderer over the WRAM tilemap shadow, a
window manager, a hardware-sprite cursor, pad-as-pointer + a soft keyboard,
and the SysInfo + Clock apps.

- **kernel.asm** (~1.5k lines): cell primitives (`fill_cells` / `draw_str`
  / `draw_char`) writing the tilemap shadow with four UI palette schemes
  (NORM/INV/ACC/KEY as Mode-1 palette lines 0-3); the window manager
  (z-order list, raise/close, title-bar drag with cell snapping, close box,
  window chrome); `launch_app` + an app-definition table; the event queue;
  pad-as-pointer input (d-pad accel cursor, A=click/drag, B=soft keyboard,
  Y=Enter, X=Backspace, Start=Esc, Select=Space, L/R=turbo) decoded from the
  auto-joypad word; the cursor as two OAM sprites flushed each vblank.
- **softkbd.inc**: the soft keyboard re-laid-out for 32 cells (Genesis is
  40) - layout table, hit-test, sticky shift, hover highlight, posts EV_KEY
  with the Amiga/Genesis raw codes.
- **SysInfo** (CPU/RAM/region/input) + **Clock** (a real 60 Hz NMI tick →
  HH:MM:SS), refreshed once a second.
- **NMI** runs on its own direct page ($0100) so its input/flush scratch
  never collides with the main loop's ($0000).
- **Verified in Mesen2** (build/m1.png): desktop + menu bar + two overlapping
  windows (correct z-order, chrome, white/cyan palettes) + a live advancing
  clock; VRAM proven byte-correct via CPU read-back.

Three traps fixed along the way (all in kernel.asm / HANDOFF.md): calling
the 16-bit cell routines with an 8-bit accumulator (garbage tiles); outer
loop counters clobbered by the draw routines they call (use the dedicated
LC0/LC1 slots); `FlushOAM` invoked with the wrong A width in the NMI.

**Known caveat (rig, not ROM):** under this headless/RDP host only Mesen's
*software* renderer is screen-capturable, and it drops the BG palette bits
to 0 for screen rows below ~scanline 160 (the soft-keyboard panel's bottom
rows render blue instead of cyan). The tilemap VRAM is proven byte-identical
at the top and bottom of the screen, so this is a renderer/rig artifact, not
a ROM bug - a reference (hardware-renderer) capture isn't obtainable in this
environment. Documented in snes/README.md.

## [snes: milestone 0 — LoROM skeleton boots to the splash] - 2026-06-14

The SNES port opens: a 65816 LoROM cartridge boots in Mesen2 to the
"UnoDOS 3" tile splash and reacts to the joypad. This is the Genesis port's
twin, re-expressed in 65816 (HANDOFF.md), and it stands up the shared cc65
toolchain (ca65/ld65) and the foundation every later milestone hangs off.

- **`kernel.asm`** — native-mode bring-up (`clc/xce`, `rep #$38`), forced-
  blank PPU init, DMA upload of the tile blob + palette, Mode 1 / BG1, and
  the **shadow + DMA** render architecture (HANDOFF §2): the main loop
  writes a WRAM tilemap shadow at `$7E:1000`, the **vblank NMI** DMAs it to
  VRAM, acks, waits out the auto-joypad read, and latches `JOY1` — no
  app/WM logic in the ISR (PORT-SPEC §6 rule 2). The live controller word
  renders as `PAD:xxxx`.
- **`mkdata.py`** — the shared `kernel/font8x8.asm` → SNES **4bpp planar**
  tiles (32 B/tile) + a **BGR555** palette (entries 0–4 = the UnoDOS UI
  colours). 256×224 ⇒ **32×28 cells**, the documented narrower-by-8 metric
  vs. Genesis's 40×28.
- **`build.sh` / `lorom.cfg`** — cc65 build to a checksum-patched 32 KB
  LoROM `.sfc`, with an `AUTOTEST` variant that self-injects a synthetic
  joypad value in the NMI.
- **Rig** (`setup_mesen.ps1` / `run_mesen.ps1`) — Mesen2 forced to its
  software renderer (PrintWindow can't grab the GPU surface on this
  headless desktop) + window capture; input verified by the AUTOTEST build
  (the Genesis fallback), since Mesen's CLI doesn't autoload Lua.
- **Verified in Mesen2:** `build/desktop.png` (interactive, `PAD:0000`) and
  `build/autotest.png` (`PAD:C0A0`, `* AUTOTEST *`) — the read → shadow →
  DMA → display pipeline end to end. Two traps recorded in HANDOFF.md for
  M1: ca65's parameterised-`.define` half-stride evaluation, and the
  write-twice BG scroll registers.

## [ps2: milestone 2 — memory-card storage on the EE] - 2026-06-14

The EE File Manager now persists to the **PS2 memory card** via libmc, so
Files/Notepad save and load on real hardware and across boots.

- `ee_platform.c` loads `rom0:MCMAN/MCSERV`, runs `mcInit`, and brings up
  `/UnoDOS` (formatting the card only when `mcMkDir` reports `sceMcResNoFormat`,
  so an already-written card is never wiped).
- `mac_io.c` (EE branch) uses the libmc file API — `mcOpen` with
  `sceMcFileCreateFile`, `mcRead`/`mcWrite`/`mcClose`/`mcDelete`, `mcGetDir` for
  the catalog. (The PS2 MC isn't a POSIX FS — `open(O_CREAT)` makes a non-
  round-tripping directory entry; `mcOpen` makes a real save-file.)
- **Verified in PCSX2** (`shots/m2_pcsx2_*.png`): a 55-byte Notepad doc writes
  to the card, reloads byte-for-byte after the buffer is wiped, and — in a
  separate no-save run after a force-kill — loads back from the card, proving
  persistence across power cycles. The host build keeps the `uno_disk/`
  directory backend; both share the same File Manager code path.

## [ps2: milestone 1 — the desktop arrives (C core + Mac-compat shim)] - 2026-06-14

The portable C core (`mac/unodos.c`, 4139 lines — 11 apps + window manager +
event model + scheduler + FAT12) now runs on the PS2 by **swapping the platform
layer**, not rewriting (HANDOFF §1 strategy).

- **Mac-compat shim** (`mac_compat.h`/`.c`): the ~40 Mac Toolbox calls the core
  uses, re-implemented over the software framebuffer `fb.*` — one implicit
  full-screen GrafPort, QuickDraw rect/oval/line/text (Bresenham `LineTo`, 8×8
  `DrawText` at the pen baseline, `PaintOval`), pen + fore/back colour +
  transfer-mode state, a platform-fed event queue, a deterministic `TickCount`
  call-clock, and `NewPtr`/`DisposePtr`.
- **File Manager + Sound** (`mac_io.c`): `FSOpen/Read/Write/Create/Delete` +
  `PBGetCatInfo` over a real directory tree (the M2 storage backend), and a
  square-wave `Snd*` channel model (silent on host; audsrv on EE is the
  remaining M3 piece).
- **`ps2/unodos.c`**: the core, copied from `mac/unodos.c`. Divergences: the
  dozen Toolbox headers collapse to one `#include "mac_compat.h"`; Pascal
  literals (`"\pNAME"`) become octal-length C strings (gcc has no `\p`); the
  68K coroutine scheduler (`ctx_switch` asm) is guarded under `__m68k__` with a
  portable **kernel-driven (poll-and-dispatch) scheduler** for PS2/host — the
  Apple II model, identical app semantics.
- **Two front ends, both verified.** `host_desktop.c` builds the whole desktop
  with WSL gcc → PPM (the fast inner loop); `./build.sh desktop [FEATURE]`
  renders `shots/m1_*.png` — desktop + all 11 apps + the FAT12 write/read
  round-trip into Notepad all confirmed. `ee_platform.c` is the real EE target:
  GS-presents `fb` each vsync and maps the DualShock 2 to UnoDOS key events;
  `./build.sh ee [FEATURE]` links a real R5900 ELF and the desktop + Pac-Man
  render on the emulated GS in PCSX2 (`shots/m1_pcsx2_pacman.png`).

So **M1** (desktop/WM/apps) is verified on the host *and* the emulated PS2;
**M2** (File Manager + FAT12) and **M3 Theme** (32-bit colour) come along
through the shim. (Memory-card storage landed in M2 below; remaining: EE audio
via audsrv, and a USB keyboard.)

## [ps2: milestone 0 — software-FB foundation + EE ELF builds] - 2026-06-14

First milestone of the **Sony PS2 port** (`ps2/`), strategy = port the
portable C core (`mac/unodos.c`) by swapping the platform layer.

- Software framebuffer (`fb.c`/`fb.h`): 640×448×32 in EE RAM + drawing
  primitives (fill/frame/invert-XOR/8×8 text/scaled text) over the 4-colour
  PORT-SPEC gamut — the layer the whole port draws through; the GS is reduced
  to a per-vsync textured-quad blit.
- Shared font → C array (`mkfont_c.py`); hello-GS splash (`uno_splash.c`).
- **Host shim** (`host_main.c` + `tools/ppm2png.py`): builds the FB code with
  a host compiler (WSL gcc) and renders `shots/m0_splash.png` — the splash is
  verified on the PC, and the EE target shares the FB/splash code verbatim.
- EE target (`main.c`: gsKit GS init + FB→GS blit + DualShock 2 via SIO2MAN/
  PADMAN). **Builds** to `build/unodos-ps2.elf` (real MIPS R5900 ELF) with the
  prebuilt ps2dev v2.0.0 toolchain under WSL, and **runs on the emulated GS**:
  PCSX2 v2.6.3 + a 4 MB PS2 BIOS boots the ELF and renders the splash through
  the real GS pipeline (`shots/m0_pcsx2.png`, captured by `tools/run_pcsx2.ps1`).
  Rig gotcha: PCSX2 v2.x needs `[UI] SettingsVersion = 1` in `PCSX2.ini` or it
  refuses the config and blocks the boot. M1 (the C-core desktop) is unblocked
  and host-shim-iterable.

## [apple2: milestone 3 — full app roster + feasibility verdicts] - 2026-06-14

The **Apple II port** (`apple2/`) reaches M3: a 10-icon desktop on the 1 MHz
6502, all harness-verified.

- New apps: **Theme** (6 dither presets over a mutable `pat_tab`), **Dostris**
  (10×20 puzzle), **Pac-Man** (the 1 MHz adaptation — 13×13 maze, two
  Manhattan-steer ghosts, tile-stepped 7px actors), **Music** (Canon in D on
  the `$C030` speaker, blocking square-wave staff player), **Tracker** (shared
  32×4 pattern format, single-voice playback, SONG.UNO save/load), **Paint**
  (32×34 fat-pixel cells, four dither inks, PAINT.UNO save/load).
- **OutLast** feasibility: ships as a ~4 fps prototype (28-band half-res road
  raster — measured, just under the 5 fps bar but steering-responsive).
- **Scheduler** verdict: stack-partitioning proven (40 cooperative switches,
  canaries intact), but the shipping kernel keeps poll-and-dispatch.
- `tests/m3.script` + 7 per-app scripts + scheduler proto, all green.
  Real-hw (AppleWin/FloppyEmu) pass still pending.

## [macplus: real Mac SE audio fix] - 2026-06-14

Sound was silent on the real SE: `snd.i` wrote the PWM buffer but never
enabled the hardware there and set "volume" on the wrong VIA port (PB0-2 are
the RTC lines; volume is Port A 0-2, enable is Port B bit 7). Added
`snd_hw_on` — interrupt-masked, ORs PA0-2 = 7 and ANDs PB7 = 0 once at boot,
never touching PA4 (overlay) or PB3-5/SR (ADB), so the SE input path is
undisturbed. Harness-verified (square wave reaches the buffer, PB7=0,
volume=7); needs a hardware ear-check on the SE.

## [macplus: validated on real Mac SE hardware] - 2026-06-13

**The standalone Mac OS now boots and runs on a real Mac SE** (via
FloppyEmu) — boot chain, desktop, and the ROM-assisted ADB input path all
live on hardware. Getting there meant fixing two faults the emulators never
surfaced; they boot through a far more initialised low-memory environment
than a bare ROM-assisted boot. Same `unodos_macplus.dsk` boots Plus and SE.

### Fixed: `FAULT @ 00403F72` / `ACCESS 00000005` on the SE
The first `_GetOSEvent` of each main-loop pass walked the ROM's
`EventQueue` ($14A), which is uninitialised on a System-less boot —
`qHead` held garbage `$FFFFFFFF`, so the ROM's queue scan computed
`$FFFFFFFF + 6 = $00000005` (odd) and address-errored inside ROM. We now
initialise the OS Event Manager in the ROM-assisted boot path: point
`SysEvtBuf` ($146) at a 20-record event pool (22 B each, free-marked with
`evtQWhat = $FFFF`), set `EvtBufCnt` ($154), and clear `EventQueue` to
empty. Buffer layout read directly from the SE ROM's `PostEvent`, so the
ADB also posts keys/mouse into it. Root-caused by disassembling the SE ROM
at the fault PC and verified against it in Unicorn (`qHead = 0` takes the
empty-queue branch and never reaches the faulting load).

### Fixed: ADB input dead on the SE (mouse + keyboard)
With the desktop up, ADB was silent. Two causes: (1) autopoll self-chains
through each transaction's completion interrupt, and our long
interrupt-masked boot breaks the chain — fixed by calling `_ADBReInit`
($A07B) once interrupts are enabled. (2) The mouse was read from
`RawMouse` ($82C), but the SE ADB mouse handler accumulates deltas into
`MTemp` ($828) and leaves the `MTemp → RawMouse` copy to the VBL cursor
task, which we disable to paint our own cursor — fixed by reading `MTemp`
directly with our own clamp + write-back. Keyboard latency also tightened:
the idle loop drains the ROM `EventQueue` ($14C) immediately rather than on
the once-per-second refresh. SE input low-mem map: button = `MBState`
($172), mouse = `MTemp` ($828), keys = `EventQueue` ($14A) via
`_GetOSEvent`.

### Preventive ROM-VBL hardening (ships alongside)
The chained ROM level-1 handler's per-tick work assumes System-level init.
Pre-empted so it can't fault once `_GetOSEvent` stops crashing and the VBL
actually runs: the ROM cursor task is disabled (`CrsrCouple`/`CrsrNew` at
$8CF/$8CE = 0 — we paint our own cursor) and the stack-into-heap sniffer is
satisfied by pointing `ApplZone` ($2AA) at a zeroed fake zone.

### Crash-dump fault screen
Bus/address/illegal faults now paint a full dump — PC, faulting access
address, SSW (bit 4 = read/write), the opcode word (IR), and all
D0-D7/A0-A7 — instead of just the PC. This is what made finding #2
diagnosable from hardware alone.

### Fixed earlier: Sad Mac `0F/00000001` at boot (BootDrive)
The boot block and `sony.i` hardcoded `ioVRefNum = 1`, but FloppyEmu on
the SE's external port enumerates as drive 2/3, so the read hit the empty
internal drive. Now honors low-mem `BootDrive` ($210) in both layers, and
the read-fail paths raise distinctive Sad Mac minors ($42 = kernel read
failed, $43 = `UDM1` magic missing) instead of the ambiguous `1`.

## [macplus milestone 3: FULL APP PARITY] - 2026-06-12

The standalone Mac OS now carries the complete shared UnoDOS app roster —
the same 11 apps as the Amiga, Genesis, hosted-Mac and x86 ports — plus
sound and the cooperative scheduler. Everything below is harness-verified
(4 regression scripts); the sound ear-check is a real-hardware item.

### Sound (snd.i)
The classic Mac pulse-width buffer: 370 words at MemTop−$300 scanned at
22.257 kHz, high byte per word = sample; the low bytes (the .Sony
variable-speed disk PWM) are never touched. Square synth converts the
shared Paula note periods (period/40); gm_* game-music sequencer with
PAL→60 Hz tempo rescale. Machine-gated: Plus = VIA PB7 + volume control,
SE = buffer-only, Mac II class = disabled (ASC).

### Games: Dostris (proc 5), Pac-Man (6), OutLast (7)
Verbatim logic, tables and AI from the Amiga port with deliberate 1-bit
schemes: Dostris pieces in alternating dithers; Pac-Man ghosts identified
by body fill density (solid/50%/25%) with hollow-shell frightened mode;
OutLast's white road over medium/light dither grass parity. Gravity and
physics run as task ticks; tick_wanted defeats the idle gate only while a
game (or song) is live.

### Paint (8), Music (9), Tracker (10), Theme (11)
Paint with the platform's true gamut — the four dither inks — and
byte-exact PAINT.UNO round-trips; Music plays Canon in D on the square
voice with background playback; Tracker edits the byte-identical shared
pattern format with leftmost-voice playback (the x86 PC-speaker model)
and SONG.UNO persistence; Theme selects dither schemes through the
now-mutable pat_tab — six presets including a full video invert, applied
live with a whole-screen repaint as the preview.

### Cooperative scheduler (scheduler.i)
Port of the Amiga scheduler with the Genesis key yield-retry: task 0 is
the kernel; every window runs its app proc in a private-2KB-stack task
(stacks at $3C000, below the disk-app region; StkLowPt cleared so the
ROM stack sniffer stays quiet in ROM-assisted mode). Keys post to the
focused task's one-slot mailbox; frame ticks drive the games in task
context. task_body re-derives the proc per event after a real bug: the
cached-register approach broke when Theme's repaint_all counted windows
in the same register.

### Kernel
Large buffers (Paint canvas, FAT caches, Notepad buffer, game state)
moved out of the image to fixed-RAM KBSS equates ($30000+), zeroed at
boot — image ~30 KB and every vars label safely pc-relative. pat_tab is
mutable (Theme); clear_screen derives the desktop fill from it.

## [macplus milestone 2: floppy filesystem + Files/Notepad + disk apps] - 2026-06-12

### macplus: the UnoDOS floppy filesystem (M2)

The standalone Mac OS gains storage. Past the kernel image, the 800K boot
floppy now carries a plain **FAT12 volume** (at sector 256) — the same
on-disk layout the x86 reference port uses, so the disk is PC-readable.
The kernel reads *and writes* it through the ROM .Sony driver via the same
`_Read`/`_Write` A-traps the boot blocks use (`sony.i`, a flat 512-byte
logical-sector device), driving the portable 68K FAT12 core (`fat12.i`)
shared with the Amiga port.

- **Files** (proc 2) lists the root directory — arrows select, Enter opens
  a file in Notepad, `r` re-mounts/refreshes. The volume is lazily mounted
  the first time the window paints.
- **Notepad** (proc 3) views and edits text with caret/line navigation and
  vertical scroll; **Clr** (the M0110A keypad clear, raw `$50`) saves the
  buffer back to the floppy via `fat_save_file` (a real `_Write`). Verified
  round-trip: edit → save → close → reopen reads the change back from disk.
- App keys now route to the topmost window's handler (`WPROC`), wired into
  the main loop's event drain.

### macplus: disk-loaded app binaries

The launcher can run an app *image* read off the floppy, exactly as the x86
launcher loads `.BIN`s. `DEMO.APP` is a standalone, position-independent
68K binary on the FAT12 volume; the kernel reads it into `$40000` and calls
its entry for each window event (`d0=0` draw / `d0=1` key), handing it
`a5` = a **ksys service table** (`draw_string`, `fill_rect`,
`fat_find_file`/`fat_read_file`, `get_ticks`, …) so the app holds no
absolute kernel addresses. `diskapp.i` is the loader + ABI glue,
`demo_app.asm` the sample app.

### macplus: harness + build

The ROM-free Unicorn harness now emulates `_Write` ($A003) as well as
`_Read`, so the entire filesystem path runs without an Apple ROM;
`tests/m2.script` is the M2 regression (mount, list, open, edit, save,
persist, then load the disk app and drive its key handler). `mkfs.py`
writes the FAT12 volume (the `disk/*.TXT` content plus the assembled
`DEMO.APP`) into the image after `mkdisk.py` packs the boot blocks + kernel.

## [Mac SE/II support + platform-authentic chrome] - 2026-06-12

### macplus: machine-adaptive input + Mac II geometry

The standalone Mac OS now runs across the whole compact-Mac span, not just
the Plus. At boot the kernel reads the ROM version word (`ROMBase+8`) and
picks its input strategy: Plus (`$75`) keeps the self-owned M0110 keyboard
and SCC quadrature mouse; **SE and later** switch to *ROM-assisted mode* —
chain the ROM's level-1 handler (its ADB stack stays alive) and mirror the
low-memory state it maintains (`Ticks`, `RawMouse`, `MBState`, the OS event
queue via `SysEvtMask` + `_GetOSEvent`). The Mac II class gets a 640×480
geometry build (`./build.sh mac2`); the default 512×342 image boots Plus
**and** SE unchanged. Validated in a self-built Mini vMac II with a real
IIcx ROM (boot, drag, full ADB keyboard) — the same path the SE will use.

### Platform-authentic window chrome

Each port's chrome now matches its native look:

- **Macs** (standalone + hosted System 7/Classic): System 7 chrome — drop
  shadows, pinstriped active-only title bar, square close box on a white
  patch, centered title.
- **Amiga**: Workbench look — blue drag bar with white left-aligned title
  when active / white bar when inactive, orange-centered close gadget.
- **x86**: already platform-authentic — the `widget_style` system renders
  Windows-3.x 3D bevels on VGA and a flat 4-color variant on CGA/8088.

Shared fix: opening a second window now repaints all windows so the
previously-active one loses its active-state title styling.

### Roadmap

Apple II and Apple IIGS ports added to the roadmap (TODO.md) — not started.

## [MacPlus standalone OS, milestone 1] - 2026-06-12

### New port: UnoDOS as a real OS on compact 68000 Macs (macplus/)

The Mac line is now a true operating system, not a Toolbox application.
Custom boot blocks ('LK'/$4418, position-independent) are bootstrapped by
the Mac ROM — the exact analog of the BIOS bootstrapping the x86 port —
and pull the kernel off raw floppy sectors with a single .Sony _Read,
the only ROM service UnoDOS uses. The kernel owns the machine: own
stack, own vector table, VIA CA1 tick, M0110/M0110A keyboard driver over
the VIA shift register (Instant-poll protocol, $79 prefix decoding),
SCC DCD quadrature mouse driver, 1-bit 512x342 renderer with dither
"colors", software save-under cursor, and fault screens. Milestone-1
scope: desktop, icons, full window manager (raise/drag/close/z-order,
incl. the tst-before-rts click guard), SysInfo + Clock.

Because Mini vMac/MAME need copyrighted Apple ROMs, the port ships a
ROM-free verification harness (macplus/harness.py): a Unicorn/QEMU 68000
core where the harness plays the ROM — Start Manager, A-line _Read
against the disk image, register-level VIA/SCC emulation, PNG
screenshots, scripted mouse/keyboard. The whole M1 surface is verified
through real injected input (tests/m1.script). Real-hardware validation
pending (quadrature polarity + keyboard cadence are the flagged
calibration points).

Fixed during bring-up: kb_byte dropped key events whose canonical raw
code had no ASCII (arrows) — `or.b` set flags on the low byte only and
the following beq bailed; same flags-vs-branch family as the
find_window_at click-through bug. An explicit tst.w now guards it.

## [Window-click fix + Paint polish] - 2026-06-12

- **Click-through bug fixed (Genesis + Amiga)**: `find_window_at`
  returned window HITS with stale condition flags (the final bounds
  compare is negative for any hit), so the caller's `bmi` sent every
  window click to the desktop. Latent since milestone 1 on both
  bare-metal 68K ports — the AUTOTESTs drive key handlers directly
  and only ever clicked a desktop icon, so the first real window
  click happened on physical Genesis hardware. Fixed with an
  explicit `tst.w d2` at the routine's exit; AUTOTEST_CLICK now also
  closes a window through its close box as the regression guard.
- Paint's canvas is now **white** (the MacPaint expectation) on
  Genesis, Amiga and x86; default pens adjusted to stay visible.
- Genesis pad-first controls and labels: Files X = delete, Tracker
  X = clear cell, Paint Y = next tool / X = next pen; footers name
  pad buttons (bare letters = soft-keyboard taps).

## [Cross-platform parity wave 2] - 2026-06-12

### Paint everywhere, Tracker everywhere, Mac multitasking + PC disks

- **Paint on all five targets** - the MacPaint-style editor (tool
  palette, drag-to-draw canvas with byte-per-pixel backing store,
  pencil/brush/eraser/line/rect/filled-rect/oval/filled-oval/flood
  fill/spray) with a per-platform "all the colors" selector: 256
  8-bit colors (Mac 7), authentic 1-bit dither patterns (Mac
  Classic), all 4096 OCS colors via live copper pen tuning (Amiga),
  all 512 colors via CRAM tuning (Genesis), and the active mode's
  full palette incl. a 256-color VGA picker (x86). One shared
  Bresenham/scanline-oval/flood design; a real e2-reuse Bresenham
  bug was caught by the Genesis AUTOTEST and fixed on every port.
- **Tracker on x86 + Mac** - every platform now has the 32x4 pattern
  editor with the byte-identical SONG.TRK format (PC speaker plays
  the leftmost voice; the Mac drives up to four Sound Manager square
  channels). QEMU-verified playback on x86.
- **Mac cooperative multitasking** (milestone 3): per-window tasks
  with a 68K asm context switch, heap stacks, one-slot mailboxes -
  both Mac targets, same semantics as the Amiga/Genesis schedulers.
- **Mac PC-compatible floppy**: FAT12 read/write core over an
  injectable block device (.Sony raw sectors on real SuperDrives; a
  RAM image under Executor); Files gains an HFS <-> PC disk volume
  toggle and Notepad round-trips files. The core's output image was
  verified byte-for-byte with an independent FAT12 parser.
- Genesis port validated on real hardware (2026-06-12).
- Known issue filed: the x86 launcher's 16-icon table includes its
  own Refresh slot, so only 15 apps fit; MOUSE.BIN/MKBOOT.BIN left
  off the default floppy as the workaround (still build from source).

## [Genesis milestones 3 + 5 + 6] - 2026-06-12

### Sega Genesis / Mega Drive: full Amiga-port parity (v0.2.0)

- **Theme app** (`genesis/theme.i`, proc 8): the 8 preset palettes
  shared with every other port, stored pre-converted to Genesis CRAM
  words ($0RGB → $0BGR, 3-bit channels — preset 1 reproduces the boot
  palette exactly), applied live by rewriting the themed entries of
  all four palette lines; r/g/b keys edit the active colors one 3-bit
  channel at a time, like the Amiga's 4-bit editor scaled to Genesis
  color depth. Game colors (entries 5-15) stay fixed.
- **Tracker** (`genesis/tracker.i`, proc 9): the Amiga 32-row ×
  4-channel pattern editor on the PSG — channels 1-3 are the square
  tone generators (PSG values = the ProTracker periods scaled by the
  clock ratio, so pitches match the Amiga within cents), channel 4 is
  the noise generator (the note picks the rate, hits decay per
  frame). The pattern format is byte-identical to the Amiga tracker,
  same demo song; `s`/`l` persist SONG.TRK in cartridge SRAM, `t`/`y`
  write/read the pattern over the tape interface (`tape.i` grew
  parameterized `tape_save_blk` / `tape_load_core` engines).
- **Sega CD backup RAM, Mode 1** (`genesis/bram.i`, milestone 5): the
  cartridge boots the CD attachment as a peripheral — expansion probe,
  Kosinski-decompress the Sub-CPU BIOS out of the main BIOS ROM
  ($415800/$416000/$41AD00/$40D500 candidates), upload a ~300-byte SP
  stub at $6000, and speak a LIST/READ/WRITE/DELETE RPC over the
  gate-array mailbox with file data staged through Word RAM. The stub
  calls the BIOS `_BURAM` traps, so files share the standard Sega
  directory with every other CD title and the console's own manager.
  Files app: `v` cycles SRAM ↔ BRAM; Notepad F1 saves to the active
  volume; UnoDOS 8.3 names normalize to 11-char BRAM names with the
  original name + byte length in a 14-byte payload header (foreign
  saves open raw). The transport is injectable per the port's
  standing rule: AUTOTEST_BRAM verifies the whole protocol + UI stack
  over a RAM-backed fake in BlastEm; the BIOS-trap path needs a
  CD-capable emulator or real hardware. Two emulator traps fixed
  along the way: BlastEm raises a 68000 **bus error** for $400000
  reads with no CD (real hardware returns open bus) — the probe arms
  a bus-error recovery vector that unwinds to the no-CD path; and the
  write-protect register write must be byte-wide to keep the
  bank/DMNA bits.
- **Cooperative scheduler** (`genesis/scheduler.i`, milestone 6 =
  the Amiga milestone 3): every window runs its app proc in its own
  task with a private 2KB stack ($FF4000 block) and a one-slot
  mailbox; the kernel task pumps input, drag, audio services and
  desktop. Keys post with a bounded yield-retry (`task_post_key`) so
  soft-keyboard and PS/2 bursts survive the single-slot mailbox —
  verified by the existing kbd/ps2 AUTOTESTs running unchanged
  through the task machinery, and by Dostris gravity ticking via the
  mailbox.
- Desktop grows to 10 icons (Theme reuses the x86 Settings art,
  Tracker the Music art); game/maze/actor tiles shifted +8.
- Genesis port version v0.1.0 → v0.2.0 ("Milestone 6"); test harness
  configs (WinUAE .uae files, autotest/snapwin scripts, Executor
  stage_run) re-pointed at the repo's current location.
- Verified in BlastEm 0.6.2: all 14 AUTOTEST builds re-run green
  (composite, notepad, music, kbd, ps2, click, dostris, outlast,
  pacman, sram, tape, bram, theme, tracker); Amiga AUTOTEST re-run
  green in WinUAE; Mac targets rebuilt clean (no source changes).

## [Genesis milestones 4 + 4.5] - 2026-06-11

### Sega Genesis / Mega Drive: storage — SRAM saves and tape/WAV

Storage architecture (all four tiers) documented in
docs/GENESIS-STORAGE.md; Sega CD backup RAM (Mode 1 + BIOS BURAM) and
SD-over-SPI are spec'd there for later milestones.

- **Cartridge SRAM** (`genesis/sram.i`): 8KB battery-backed, declared
  in the ROM header ("RA" $F8 $20, odd bytes at $200001), with the
  USV1 mini-filesystem: 8 directory entries + byte heap,
  save-by-name overwrite, delete with heap compaction. New **Files
  app** (proc 7, 8th desktop icon): list/open/delete; **Notepad F1**
  now saves for real (UNTITLED.TXT for new buffers, the source name
  for opened ones). Verified in BlastEm: save → wipe → reopen round
  trip (AUTOTEST_SRAM). Two traps: SRAM indexing must use `(a0,d0.w)`
  (a stale high word in a `.l` index wanders off-bus), and `$A130F1`
  is written once at boot — per-access toggling left SRAM unmapped
  under BlastEm (open-bus $FF reads that cascaded into a wild copy
  and a stack smash, diagnosed by the new on-screen exception dump).
- **Tape / WAV over audio** (`genesis/tape.i`): the classic 1-bit
  tape interface — the console has no ADC, so reads go through a
  one-comparator adapter (port 2 pin 1) and writes need no hardware
  at all (the PSG plays KCS 1200-baud AFSK out the headphone jack).
  Poll-count timebase, ~20s per 2KB. The bit/block decoder is an
  injectable pure routine, emulator-verified by AUTOTEST_TAPE
  ("HELLO FROM THE TAPE DECK" decoded through the real state
  machine). `genesis/mktape.py` is the PC tape deck: file→WAV encode,
  WAV→file decode (same state machine), selftest; a 2047-byte WAV
  round trip is byte-exact. Files app: `w` writes the Notepad buffer
  to tape, `r` loads a block back.
- `err` now renders the exception stack frame in hex on the bottom
  row (magenta-border beacon + faulting PC) — on-screen forensics
  for a platform with no debugger stdin.

## [Genesis milestone 2] - 2026-06-11

### Sega Genesis / Mega Drive: the game ports

- **Dostris + OutLast** (`genesis/games.i`) and **Pac-Man**
  (`genesis/pacman.i`): the same piece tables, track/curve table,
  maze, three-ghost AI (Blinky direct / Pinky 4-ahead / Clyde hybrid,
  scatter-chase schedule, frightened mode with the eat chain), scoring
  and physics as the x86 originals, rescaled from the donor Amiga
  ports' 50 Hz to the 60 Hz NTSC vblank.
- Cell rendering through `gcol`, a 32-entry map from the Amiga
  extended-palette indexes to (attr | solid-tile) name words on VDP
  palette lines 2/3 — converted channel-wise (Amiga `$0RGB` vs Genesis
  `$0BGR`). OutLast re-runs the donor's per-strip road math once per
  cell row in the same pixel space, so the curve/traffic/collision
  behavior is identical.
- **Pac-Man actors are hardware sprites** (1-4, chained after the
  cursor sprite): pixel-smooth motion over the cell maze with no
  repaint-under-actor logic at all — only eaten-dot cells redraw.
  Sprites park off-screen whenever the Pac-Man window isn't topmost.
- **Game music on PSG channel 1** (`gm_*`): Korobeiniki and Sunset
  Drive parsed from the x86 sources by `mkdata.py` into PSG tone
  values + 60 Hz durations; mutes (keeping position) when the owning
  game loses topmost, stops on close (PORT-SPEC §2).
- **Game-mode pad**: while a game window is topmost the d-pad posts
  arrow-key events (press + hold-repeat at ~15 Hz after 12 frames),
  A = Space, B = soft keyboard, C = Enter, Start = Esc, X = 'n',
  Y = 'p'. Desktop mouse behavior returns when a non-game window is
  topmost.
- Desktop grows to 7 icons in two rows (the game icons pulled from the
  x86 `.BIN` headers like the rest).
- Verified in BlastEm via three new AUTOTEST builds (dostris: six
  hard-drops through the key handler; outlast: 60 forced physics
  steps; pacman: 150 real AI steps).

## [Genesis milestone 1] - 2026-06-11

### Sega Genesis / Mega Drive: desktop, pad-mouse, soft keyboard, PS/2 wiring, Notepad + Music

- **Kernel** (`genesis/kernel.asm`): cell-based UnoDOS desktop on VDP
  plane A (H40, 40×28 cells; windows snap to the 8 px grid), four
  palette lines as the four themed UI attribute schemes, hardware-
  sprite cursor, window manager (drag/raise/close/z-order), 32-entry
  event queue + press-time click latch per PORT-SPEC §3/§6, app icons
  converted from the x86 `.BIN` headers. ISRs never touch the VDP
  except the status-read interrupt acknowledge.
- **Pad as mouse**: standard 3/6-button pad on port 1 — d-pad moves
  the cursor with held-time acceleration (Z = turbo), A = click/drag,
  B = soft keyboard, C = Enter, Start = Esc, X = Backspace, Y = Space.
- **Soft keyboard** (`genesis/softkbd.i`): kernel overlay (bottom 6
  cell rows) with full QWERTY, sticky Shift, F1, arrows, Esc; hover
  highlight; posts through the shared event queue with the Amiga-port
  raw codes, so the 68K apps stay byte-portable.
- **PS/2 drivers, wired for real hardware** (`genesis/ps2.i`):
  keyboard on port 2 (TH = CLK → EXT level-2 interrupt, 11-bit frame
  assembler, scancode set 2 with shift/break/E0), mouse on port 1
  (host-inhibit + per-vblank receive windows, boot-time `$F4` probe
  with pad fallback). Emulators don't model PS/2 on the control
  ports; the decode engines are injectable and the AUTOTEST_PS2 build
  verifies them end-to-end in BlastEm ("ps2 ok" typed, cursor moved
  by a synthetic stream packet).
- **Apps** (`genesis/apps.i`): SysInfo, Clock, Notepad (2 KB buffer,
  caret, line navigation with goal column, vertical scroll clamp,
  status bar), Music (PSG channel-0 square-wave sequencer, the shared
  Canon in D at 60 Hz, staff view with live note highlight).
- Verified in BlastEm 0.6.2 via AUTOTEST screenshot builds
  (composite/notepad/kbd/ps2/click); `genesis/README.md` documents
  controls, wiring, build matrix, and the real-hardware checklist.
- Bring-up traps recorded for posterity: the M0 vector table was off
  by two (the first vblank jumped to the error loop); vblank must be
  acknowledged with a VDP status read; a negative cell row shifted
  into a control word flips it into a CRAM write (the PROBE_GUARD
  build traps this and renders the caller PC on screen); a `movem`
  restore clobbering the just-found window slot index turned the
  z-list into a phantom-window CRAM sprayer.

## [Ports wave 3] - 2026-06-11

### Amiga: real storage + milestone 3

- **FAT12 driver, read + write** (`fdd.i`, `fat12.i`, `mkfat.py`):
  DF1 trackdisk DMA with Amiga-MFM sector decode and a track cache;
  full-track MFM encoder (headers, checksums, clock fixup) with
  one-revolution writes and a write-protect gate; FAT12 mount, root
  directory, chain reads, cluster alloc/free, dual-FAT flush, file
  create/overwrite. The 880KB data disk is plain-ADF-shaped (WinUAE
  serves it directly) and PC-interchangeable at file level via mtools.
  Files mounts DF1 ('m'); Notepad F1 saves to disk; Tracker s/l
  persists songs. Hardware notes: writes longer than one revolution
  wrap and destroy sector 0; the boot loader leaves DF0 selected with
  its motor on, which corrupts DF1 DMA until the driver quiesces all
  drives at init.
- **Milestone 3: cooperative scheduler** (`scheduler.i`): every open
  window runs its app proc as a task with a private 2KB stack;
  full-context task_yield, per-task event mailboxes (keys to the
  focused task, frame ticks to the topmost), spawn/kill tied to window
  create/close, ESC/close handled kernel-side. No app rewrites - the
  generic task body dispatches to the existing handlers. Verified:
  game gravity and the whole FAT12 save/reopen flow run through the
  task machinery.
- Mac milestone-3 scheduler remains open (needs C coroutines).

## [3.27.0] / [Ports wave 2] - 2026-06-11

One day after milestone 2, a parity wave across all three platforms.

### x86 (Build 406)

- **API 105 `theme_set_palette`**: 4 RGB entries (6-bit) into the VGA
  DAC via INT 10h AX=1010h; stored kernel-side and re-applied after
  every video mode switch (CGA mode 4 keeps its fixed palette).
- **Settings: "Theme (VGA)" section** - 8 preset palettes (Classic VGA,
  Midnight, Forest, Sunset, Ocean, Slate, Candy, Amber; shared with the
  68K ports) replacing the word-wrap demo.
- **Splash**: IBM PC art (CRT + desktop unit + keyboard), "UnoDOS 3" in
  the 8x14 font, "for IBM PC/XT/AT", ~2s minimum hold; progress
  segments narrowed so all 16 apps fit the bar.

### Amiga port (milestones 2.1-2.5)

- **32-color display**: 5 bitplanes (OCS lowres maximum), per-plane
  fill/char primitives, 32-entry copper palette - UI colors 0-3 stay
  theme-driven, 4-31 carry the extended game palette.
- **Theme app**: the 8 shared presets + per-channel custom RGB editing,
  applied live through the copper list.
- **Boot splash**: striped Amiga checkmark + "UnoDOS 3" at 2x.
- **Games**: Dostris, OutLast and Pac-Man ported from the x86 originals
  (same tables/physics/AI; Pac-Man uses incremental tile rendering).
  Game music: Korobeiniki + Sunset Drive on a Paula sequencer, parsed
  from the x86 sources by mkdata.
- **Tracker app**: write and play 4-channel MOD-style music -
  ProTracker periods, 32-row pattern editor, 4 chip-synthesized
  instruments, demo song. (.MOD file I/O lands with FAT12.)
- **Notepad**: up/down line navigation with goal-column memory +
  vertical scrolling.
- Desktop grows to 10 icons (two rows); games/tracker drive their own
  drawing (excluded from the 1 Hz app_ticks repaint).

### Mac ports (milestones 2.1-2.5)

- **True-color games** (color target): real RGB via 8-bit Color
  QuickDraw - the seven VGA piece colors in Dostris (16px cells),
  the full OutLast scenery palette in a 480x300 playfield (3/2 render
  scale over the faithful 320x200 game space), classic ghost
  identities in Pac-Man. Mono target keeps its 1-bit theme.
- **Theme app** (color target): the 8 shared presets + custom RGB.
- **Boot splash**: happy compact Mac + "UnoDOS 3" on both targets.
- **Games**: Dostris, OutLast, Pac-Man ported (same mechanics as x86);
  game music through the Sound Manager channel.
- **Files**: subdirectory navigation (PBGetCatInfo dirID walk,
  PBHSetVol current dir, ".." parent entry).

### Port-harness notes

- WinUAE/Executor autotest build variants play real moves at boot for
  screenshot verification (AUTOTEST_DOSTRIS/OUTLAST/PACMAN/TRACKER/
  THEME/NOTEPAD, UnoDOS7*Test targets).
- 68000 traps collected: divu needs a masked 32-bit dividend; addq is
  limited to 1-8; tst.w (pc) and (d16,An,Xn) are 68020+; transparent
  text over non-zero backdrops composes wrong colors.

## [Ports] - 2026-06-11

Platform ports, developed out-of-band from the x86 versioning (the x86
tree is unchanged). Spec: docs/PORT-SPEC.md; plan and feasibility:
docs/M68K-PORT-FEASIBILITY.md.

### Amiga port (amiga/, milestone 1-2)
- Bare-metal 68000 port for OCS/ECS A500-class machines: self-booting
  ADF (vasm + exe2adf), supervisor takeover, copper-driven 320x200x4 in
  the UnoDOS palette, hardware-sprite cursor, CIA keyboard + quadrature
  mouse into the focus-routed event queue (press-time click latch).
- Window manager: frames/title bars/close box, z-order with
  click-to-raise, clamped drag with self-erasing XOR outline.
- Apps: SysInfo, Clock, Files (boot ROM-disk browser), Notepad (caret
  editor, live Ln/Co/bytes status bar, F1 save-to-RAM), Music (Canon in
  D on a Paula square wave with staff view + playback highlight).
- Storage milestone stand-in: build-time ROM-disk from amiga/disk/
  (MFM/FAT12 driver is the next milestone).
- Verified in WinUAE with the built-in AROS ROM (no Kickstart needed).

### Mac ports (mac/, milestone 1-2)
- Two applications from one C codebase via Retro68: UnoDOS7 (System 7,
  Color QuickDraw, Mac II+, full UnoDOS palette) and UnoDOSClassic
  (System 1-6, 1-bit QuickDraw, Mac Plus/SE/Classic, authentic mono
  theme). Toolbox-based by design: one full-screen GrafPort, UnoDOS's
  own WM/widgets/theme inside it; ROM supplies screen, events, files,
  sound, ticks.
- Apps: SysInfo, Clock, Files (File Manager directory listing), Notepad
  (caret editor, live status bar, Cmd-S save via the File Manager),
  Music (Canon in D on the Sound Manager square-wave synth).
- Verified under the ROM-free Executor emulator (no Mac ROM or System
  install needed); runs from the .bin on real hardware / Mini vMac /
  Basilisk II.

## [3.26.0] - 2026-06-11

### 8088/8086 Compatibility + Audit Backlog Complete (Builds 404-405)

This release completes every item from the post-audit backlog
(docs/AUDIT-HANDOFF-2026-06.md SS5): the OS now genuinely targets the
Intel 8088/8086 it always advertised, the cursor hide/lock race and the
interrupted performance wave are finished, all confirmed-but-unfixed
medium findings are fixed, and the dynamic regression scenarios were
re-run green against the new build.

### 8088/8086 Support (the OS now runs on a real PC/XT)

- **Kernel, all 16 apps, and the floppy boot chain assemble under
  `cpu 8086`** - 1,150+ non-8086 instruction sites rewritten
  (pusha/popa -> PUSHA86/POPA86 macros, movzx -> mov+xor, multi-bit
  immediate shifts -> repeated/CL shifts, imul-imm -> shift/add,
  push-imm -> push m16, dword stores -> word pairs). New
  kernel/cpu8086.inc macros; `cpu 8086` directives make any future
  regression an assembly error.
- **FAT16/IDE hard-disk driver is explicitly bracketed `cpu 386` and
  runtime-gated**: fat16_mount detects a pre-286 CPU via the FLAGS
  12-15 signature and refuses to mount, so floppy-only 8088 systems
  degrade gracefully. The HD boot chain (mbr/vbr/stage2_hd) remains
  386+ by design, as documented.
- **INT 0x80 dispatcher bitmap tests rewritten 8086-safe** (movzx+bt
  removed) - previously the FIRST syscall on an 8088 died.
- Dead ide_read_sector removed (never called; wrote ES:DI while
  documenting ES:BX; 386-only).
- README example app and APP_DEVELOPMENT docs updated for 8086-safe
  register saving.
- Note: verified instruction-clean by the assembler and behaviorally in
  QEMU; real-8088 hardware validation still needs 86Box/PCem or a
  physical XT (QEMU cannot emulate an 8088).

### Performance (completing the audit's interrupted wave 4)

- **cga_pixel_calc MUL eliminated** - the ~120-cycle (on 8088) 16-bit
  MUL per plotted pixel replaced with a 200-byte row LUT; benefits all
  CGA text, lines, icons, and sprites.
- **Mouse cursor sprite fast path** - CGA cursor rows drawn/erased with
  2-3 byte XORs instead of 8 call/push-pop/address-calc round trips
  per row.
- **Floppy multi-sector reads** - fat12_read now reads runs of
  physically-consecutive clusters with one INT 13h per track chunk
  (DMA-boundary safe, 3 retries) instead of one call + bounce copy per
  512-byte cluster: a 20KB app load is ~4 BIOS calls instead of 40
  (up to a full disk revolution saved per sector on real hardware).
- fat12_read bounce copy word-widened (rep movsw).

### Fixed - Input & Window Manager

- **Cursor hide/lock race closed** (cursor_protect_begin): IRQ12 could
  redraw the cursor between mouse_cursor_hide and the cursor_locked
  increment, leaving XOR droppings / stale save-under rectangles. All
  36 sites now take the lock atomically.
- **IRQ12 no longer draws the cursor inside the interrupt handler** -
  no VRAM walking or VESA INT 0x10 bank switching at IF=0; the ISR sets
  a dirty flag and the redraw happens in task context (event_get /
  mouse_get_state), shrinking worst-case interrupt latency from
  potentially milliseconds to microseconds.
- **Keystrokes no longer leak across focus changes** - INT 9 stamps the
  focused task into each key event at PRESS time; stale keys whose
  target lost focus are discarded, and keys typed during an app launch
  no longer arrive in the new app. kbd_getchar (API 11) is focus-gated.
- **Mouse clicks land where they were CLICKED** - the IRQ latches
  press-time X/Y + a sequence number; API 28 returns them (SI/DI/AH/AL)
  and the launcher hit-tests the latch, fixing wrong-icon clicks during
  fast click-and-move and lost press+release pairs between polls.
- **EVENT_MOUSE posted only on button-state change and focus-routed** -
  motion no longer floods the queue, and background tasks can no longer
  steal the focused app's click events (Music lost clicks to the
  launcher).
- **Click-to-raise works on the window BODY** (was title-bar only) via
  a new z-aware hit test.
- **Window drag clamped** - the close button can no longer be dragged
  off-screen, and the desktop menu bar row stays visible.
- **Killing a task closes its file handles** (owner byte + reaper on
  all three kill paths) - the 16-entry file table can no longer leak to
  exhaustion, which blocked all app launching until reboot.
- **app_load validates file size** (0 / >=64KB / >0xFFE0 rejected, and
  short reads fail) instead of executing truncated or stale images.

### Fixed - Apps & Misc

- SysInfo uptime now measures time since BOOT (API 63 latches the BIOS
  tick counter at kernel entry) - it previously showed wall-clock
  derived values like "3054s" seconds after power-on.
- Notepad status bar (Ln/Col/byte count) updates while typing via a
  deferred dirty flag (no per-keystroke cost).
- FAT16 INT 13h extensions probed once at mount (AH=41h) and cached;
  CHS-only BIOSes skip the wasted AH=42h per sector, and stc before
  extended calls guards BIOSes that IRET without setting CF.
- PS/2 KBC fallback no longer pokes 8042 ports on pre-AT machines
  (BIOS model byte check) - saved ~3s of dead probing per boot on a
  real XT.
- Default make target now builds the bootable 1.44MB image (the 360KB
  image cannot boot: 1.44MB geometry is hardcoded).
- tools/qemu_test.py: Windows-native headless QEMU driver with
  position-tracked moveto (the old -2000 homing trick kills pointer
  motion on QEMU 11.x).

## [3.25.0] - 2026-06-11

### Full-System Audit & Stability Overhaul (Build 403)

A 116-agent audit (static analysis + adversarial verification + live QEMU
testing) produced 140 findings (97 confirmed, 25 observed dynamically). This
release fixes the confirmed critical/high findings across the scheduler,
window manager, event system, input drivers, graphics, and boot chain. Full
details: docs/AUDIT-HANDOFF-2026-06.md (handoff summary) and
docs/audit-2026-06-digest.md (every finding with verified patches).

### Fixed - Crashes & Memory Corruption

- **win_create drew its resize-grip pixels into the KERNEL CODE segment**
  (ES never set to video memory in win_draw_stub's grip block) — for some
  window geometries (e.g. 160x100) the four grip dots read-modify-wrote live
  kernel instructions; apps calling API 22 directly sprayed their own
  segment. Root cause of "random" crashes and corrupted window handles.
- **fat12_read popped one word too many** at .not_supported (7 pops for 6
  pushes) — any FAT12 read at file position != 0 returned through a
  corrupted stack and jumped to garbage. Also added a zero-byte/EOF fast
  path so read-until-0 loops terminate.
- **Kernel load was at 100% capacity with zero growth headroom** — kernel
  area expanded from 88 to 104 sectors (52KB) across the whole chain
  (stage2, BPB reserved sectors 94→110, add_floppy_fs.py, mkboot, fat12
  mount offsets — which were hardcoded, not BPB-derived). The kernel image
  pad now fails the build if the kernel outgrows the area.
- **ES was not part of the task context** — clobbered across every
  yield/context switch; cross-task segment corruption for any app holding
  ES across a syscall. Saved/restored on all five context paths.
- **Stale INT 0x80 dispatcher flags survived RETF app exit** — the next
  task resumed with the dead app's coordinate-translation/cursor-lock
  state, corrupting its registers. Flags now consumed one-shot.
- **mkboot wrote a stale filesystem size** (2810 sectors, three layouts
  old) — now derived from the layout constants.

### Fixed - Window Manager (create/destroy/z-order verified)

- **Z-order values drifted to 0 and collided** — every create/focus demoted
  all windows but destroy never renormalized, so after ~7 launch/close
  cycles hit-testing, painting, and promotion disagreed about stacking.
  Focus now demotes only windows above the raised one; destroy closes the
  z-gap. Invariant: visible windows hold dense distinct z {16-N..15}.
- **win_resize repainted only the desktop** — shrinking a window erased
  any window it overlapped. Now uses redraw_affected_windows like move.
- **win_focus (API 23) raised windows logically but never repainted** —
  stale title-bar states, stale pixels on top.
- **Resize-handle hit-test ignored occlusion** — clicking the body of a
  topmost window could start resizing a window underneath it.
- **Z-clipped WIN_REDRAW events left permanent holes** in background
  windows; **window titles overwrote the [X] close button** (now clipped).
- destroy_task_windows batches its repaint instead of a full
  promote/focus/redraw cycle per window.

### Fixed - Input & Events

- **post_event had no interrupt masking** — task-context posts raced
  IRQ1/IRQ12 posts and silently lost keystrokes/clicks exactly when window
  activity coincided with typing. Now pushf/cli...popf protected.
- **Single global event-queue head caused head-of-line blocking** — one
  task's pending event stalled keyboard/mouse for every task, then the
  31-slot queue filled and dropped input. event_get now forward-scans with
  tombstones; consecutive mouse events are coalesced at post time.
- **event_wait (API 10) / kbd_wait_key (API 12) busy-waited without
  yielding** — one blocked task froze the whole cooperative system.
- **Scancode table read out of bounds** for scancodes 0x60-0x7F.
- **XT (8255 PPI) keyboard acknowledge was missing** — on a real PC/XT the
  keyboard died after the first keystroke. Port 0x61 bit-7 pulse added.
- **Arrow/nav keys required E0 scancodes XT keyboards never send** —
  NumLock-aware routing of bare numpad codes 0x47-0x53 to the special-key
  map; NumLock toggle tracked, seeded from the BIOS flag byte.
- **IRQ12 swallowed keyboard bytes** when the KBC AUX bit was clear; mouse
  packet stream now self-heals after a lost byte (idle re-arm + sync-bit
  rejection); event_get no longer clobbers CX/DX on the no-event path.

### Fixed - Graphics (visual anomalies)

- **Default 8x8 font had a 12px advance** — all default text 50% wider
  than intended; primary cause of the boot-visible overlapping desktop
  icon labels (plus 10-char label truncation in launcher + kernel).
- **CGA scroll clear-all path fell through into VESA bank-switching code**
  — garbage fills + undefined INT 10h calls in the default video mode.
- **VESA scroll corrupted rows straddling 64KB bank boundaries**;
  vesa_fill_rect skipped a bank when a row started exactly on a boundary;
  vesa_set_bank now honors the VESA window granularity.
- **Glyphs bled up to 7px past window borders** — draw_char/draw_char_inverted
  now enforce the clip rect at row/pixel level (char & wrap APIs included).
- **gfx_blit_rect copied forward regardless of overlap** (smearing) and
  produced black fills in VESA/mode-12h (read_pixel unimplemented there).
- CGA fill/clear fast paths gained screen-bounds clamping; CGA scroll no
  longer smears up to 3 pixel columns outside non-4-aligned regions.
- VESA mode queries no longer clobber the system clipboard at 0x9000:0.

### Fixed - Desktop

- Kernel desktop icon table sized for the launcher's 40 icons (was 16) with
  bounds-checked registration; icon names NUL-terminated; label dirty-rects
  sized to real label width; icon 0 selected at boot.

### Performance

- gfx_fill_color CGA path: hybrid fill (masked edges + rep stosb middle)
  replaces per-pixel plotting for misaligned fills — ~10-40x faster window
  repaints in the default mode (partially applied; see handoff doc).
- 8px font advance removes the 4-gap-pixel-per-char fill (~33% fewer plots
  per character system-wide).

### Tooling & Tests

- tools/qemu_test.sh: headless QEMU driver (keyboard/mouse injection +
  screenshots) used for all regression testing.
- tools/to8086.py + kernel/cpu8086.inc: mechanical 186+/386+ → 8086
  instruction rewriter and macro library for the planned 8088 compatibility
  pass (audit found 1153 non-8086 sites; see handoff doc for the plan).

### Known Remaining Work

See docs/AUDIT-HANDOFF-2026-06.md — notably: the 8088 conversion has NOT
been applied yet (the OS still requires a 386+ despite the README claim),
the cursor hide/lock race fix (35 sites) is pending, and several confirmed
medium findings remain open.

## [3.24.0] - 2026-06-10

### Heap Allocator Overhaul (Builds 401-402)

The kernel heap was unusable since the kernel outgrew 16KB: the heap segment
(0x1400) overlapped the kernel image (0x1000:0000, now 44KB), so the first
`malloc` corrupted live kernel code — and three further bugs meant it never
successfully returned memory anyway. This release relocates the heap and
makes malloc/free actually work.

### Fixed (Build 401) - Heap Relocation

- **Heap overlapped the kernel image** — heap segment moved from 0x1400
  (linear 0x14000, inside the 44KB kernel at 0x10000-0x1AFFF) to a dedicated
  segment 0x8000 (linear 0x80000, 60KB). The kernel can now grow to its full
  64KB segment without colliding with the heap. New `HEAP_SEGMENT`/`HEAP_SIZE`
  constants in kernel/kernel.asm replace hardcoded values.
  - Root cause: v3.6.0 documented moving the heap to 0x1600 when the kernel
    grew past 16KB, but the code change was never applied; the kernel has
    since grown to 44KB (88 sectors), deepening the overlap.
- **First-fit size check used signed comparison** (`jge`) — the initial
  0xF000-byte (60KB) free block read as negative, so `mem_alloc` always
  returned NULL (while still corrupting the kernel via heap lazy-init).
  Changed to unsigned (`jae`).
- **`heap_initialized` flag read/written through the heap segment** — the
  flag is kernel data, but DS points at the heap when it is accessed; now
  uses `cs:` segment overrides.

### Changed (Build 401)

- **User app segment pool reduced from 6 to 5 slots** (0x3000-0x7000);
  segment 0x8000 is now the kernel heap. Max concurrent user apps: 5 + shell.
  This was the only conflict-free placement: 0x2000 is the shell, 0x9000 is
  the scratch/clipboard segment, low memory holds the kernel stack, and any
  segment below 0x2000 collides with kernel growth.

### Documentation (Build 401)

- Memory maps updated in README.md, docs/MEMORY_LAYOUT.md,
  docs/ARCHITECTURE.md, docs/FEATURES.md, docs/API_REFERENCE.md,
  docs/APP_DEVELOPMENT.md
- Corrected stale kernel size (28KB → 44KB, 56 → 88 sectors, disk layout
  sectors) in docs/ARCHITECTURE.md, docs/bootloader-architecture.md,
  docs/boot-debug-messages.md

### Verification

- New QEMU harness in test-artifacts/heap/: a test app (run as LAUNCHER.BIN)
  exercises INT 0x80 API 7/8, then run_heap_test.sh inspects guest memory via
  the QEMU monitor — heap block headers at linear 0x80000, and the old heap
  site 0x14000 compared byte-for-byte against build/kernel.bin to prove the
  kernel image is no longer modified.

## [3.19.0] - 2026-02-16

### Added (Builds 202-212) - FAT12 Write, GUI Toolkit, Settings Persistence

- **FAT12 Write Support** (Build 202)
  - `fs_create_stub` (API 45) — Create new file on FAT12 floppy
  - `fs_write_stub` (API 46) — Write data to open file
  - `fs_delete_stub` (API 47) — Delete file from FAT12 floppy
  - `fs_write_sector_stub` (API 44) — Write raw sector to disk
  - Full FAT12 cluster chain allocation for multi-cluster files

- **Boot Floppy Creator (MkBoot)** (Builds 202-204)
  - New app: creates bootable UnoDOS floppy from running system
  - Pre-reads apps to RAM, prompts for disk swap, writes boot+kernel+apps
  - Floppy-to-floppy copy workflow for users without build tools

- **GUI Toolkit Foundation** (Build 205)
  - Multi-font system: 4x6 small, 8x8 medium, 8x14 large fonts
  - `gfx_set_font` (API 48), `gfx_get_font_metrics` (API 49)
  - Word-wrap text drawing: `gfx_draw_string_wrap` (API 50)
  - Widget APIs: `widget_draw_button` (51), `widget_draw_radio` (52), `widget_hit_test` (53)
  - Clip rectangle system for constraining drawing operations

- **Settings App** (Builds 206-210)
  - Font selection (small/medium/large) with live preview
  - Color theme: text color, desktop background, window color (4 CGA colors)
  - Color swatch picker with radio buttons for font selection
  - Apply/OK/Defaults buttons
  - Settings persist to `SETTINGS.CFG` on boot floppy via FAT12 write APIs
  - Kernel loads settings at boot before launching apps

- **Color Theme System** (Builds 208-209)
  - `theme_set_colors` (API 54), `theme_get_colors` (API 55)
  - `draw_bg_color` for text background rendering
  - Desktop background color, window frame color, text color all configurable

### Fixed (Builds 202-212)

- **Disappearing Windows** (Build 211): Drawing APIs 0 (pixel), 1 (rect), 2 (filled rect) didn't hide mouse cursor during drawing. IRQ12 could XOR the cursor over window frame pixels between API calls, progressively corrupting borders. Fixed by adding cursor_hide/cursor_locked to all drawing APIs.
- **MkBoot Window Redraw** (Build 211): MkBoot defined `API_WIN_DRAW equ 30` but the correct value is 22. API 30 is `mouse_is_enabled`, so the window redraw call was silently doing nothing.
- **draw_bg_color Pollution** (Build 211): `draw_desktop_region` set `draw_bg_color` to desktop color but never restored it, leaking into subsequent window drawing operations.
- **fs_open_stub Mount Handle** (Build 210): Compared full 16-bit BX for mount handle routing, but callers set only BL. Dirty BH caused silent open failures. Fixed to compare BL only.
- **fs_readdir_stub Mount Handle** (Build 209): Same BX vs BL routing bug as fs_open_stub.
- **Button Text Overflow** (Build 210): `widget_draw_button` didn't clip label text to button bounds. Fixed by setting clip rectangle before drawing label.
- **Mouse Click Events** (Build 207): `event_get_stub` wasn't setting CF flag correctly for mouse events, causing click handlers to miss events.

### Changed (Builds 202-212)

- API table expanded from 44 to 56 function slots (APIs 44-55)
- CGA pixel plotting functions refactored: shared `cga_pixel_calc` helper saves ~100 bytes (Build 212)
- FAT12 stack cleanup comments updated from stale line numbers to descriptive labels (Build 212)
- File handle validation uses `FILE_MAX_HANDLES` constant instead of magic number 16 (Build 212)
- Coordinate translation in INT 0x80 handler now covers APIs 0-6 and 50-52 (widget APIs)

---

## [3.18.0] - 2026-02-16

### Added (Builds 194-201) - Splash Screen, Multitasking Fixes, Refresh Icon

- **Splash Screen with Logo** (Builds 196-201)
  - "U" logo drawn with white filled rectangles during boot
  - "UnoDOS 3" title and "Loading..." text displayed
  - Progress bar fills as apps are discovered from disk
  - Replaces blank/debug screen during launcher initialization
  - Fast CGA memory clear via REP STOSW (Build 200)

- **Floppy Refresh Icon** (Build 195)
  - Manual disk rescan icon appears as last desktop icon on floppy boot
  - 3.5" floppy disk shape (16x16 2bpp CGA bitmap)
  - Replaces automatic INT 13h AH=16h polling (caused constant floppy seeking)
  - Only shown when booted from floppy

- **Launch Error Feedback** (Build 195)
  - "Insert app disk" message for mount/file errors (codes 2, 3)
  - Error message auto-clears after ~2 seconds with desktop redraw

### Fixed (Builds 194-201)

- **File Browser HD Support** (Build 194): Browser now queries boot drive
  and saves mount handle, fixing blank listing on HD/CF/USB boot
- **Floppy Seeking Noise** (Build 195): Removed automatic floppy swap
  polling (INT 13h AH=16h) that caused audible seeking on IBM PS/2 L40
- **Music App Single Tone** (Build 197): App played one constant note
  instead of Fur Elise melody. Root cause: `app_yield_stub` didn't
  preserve general-purpose registers across context switches, so CX
  (note duration) was clobbered by the launcher. Fixed with pusha/popa.
- **App Launch Crash** (Build 198): Adding pusha/popa to yield broke
  new task startup — `popa` consumed return addresses instead of
  register values. Fixed by adding dummy pusha frame (8 zero words)
  to initial task stack built by `app_start_stub`.
- **Initial Context Switch Loop** (Build 201): `auto_load_launcher`
  did bare `ret` without `popa`, popping 0 from the dummy pusha frame
  instead of `int80_return_point`. This jumped to kernel entry (0x0000)
  in an infinite loop: boot → load launcher → ret to kernel → repeat.
  Fixed by adding `popa` before `ret` in both `auto_load_launcher` and
  `app_exit_common`.

### Changed

- Bootloader version updated from v0.2 to v3.18
- All boot diagnostic code removed (keypress wait, BIOS teletype,
  CGA white boxes, PRE/POST markers)
- Splash screen text uses transparent background (Build 199)
- Desktop/splash screen clear uses direct CGA REP STOSW (Build 200)

### Removed

- Kernel boot keypress wait
- Kernel BIOS teletype diagnostic output
- Kernel "PRE"/"POST" CGA diagnostic strings
- Launcher CGA diagnostic white boxes (marks 1-6)

---

## [3.17.0] - 2026-02-15

### Added (Builds 162-193) - Universal PS/2 Mouse via BIOS Services

- **BIOS PS/2 Mouse Driver** (Build 187+)
  - Uses BIOS INT 15h/C2xx services instead of direct KBC port I/O
  - INT 15h/C205 (init), C207 (set callback), C200 (enable)
  - Works with USB mice via BIOS legacy emulation (SMI-based)
  - FAR CALL callback handler (`mouse_bios_callback`) processes packets from BIOS
  - Falls back to direct KBC method if BIOS services unavailable

- **Robust KBC Mouse Init** (Build 185)
  - KBC output buffer flush (16-byte drain loop) before init
  - Keyboard interface disabled (0xAD) during mouse setup
  - Long timeout for ACK wait (~1 second via BIOS timer tick)
  - Mouse reset retried up to 3 times
  - Keyboard re-enabled (0xAE) on both success and failure

- **Boot Diagnostic** (Build 184+)
  - Mouse init result displayed at boot: B=BIOS, K=KBC, R/S/E=failure
  - Keypress wait after diagnostic for hardware verification

### Fixed (Builds 162-193)

- **USB Legacy Emulation Conflict** (Build 187): BIOS SMI handler was overriding
  direct KBC port writes, re-masking IRQ12 and disabling aux clock. Solved by
  switching to BIOS INT 15h/C2 services which work *with* the SMI handler.
- **BIOS Callback AH Corruption** (Build 188): `mov ah, bh` in X sign-extend
  overwrote the status byte stored in AH, breaking Y sign extension.
- **Callback Byte Order** (Build 192): Discovered via QEMU raw stack dump that
  BIOS pushes status,X,Y,0 before CALL FAR, making [BP+12]=status, [BP+10]=X,
  [BP+8]=Y, [BP+6]=padding. All previous convention assumptions were wrong.
- IRQ2 cascade unmask added for IRQ12 propagation (Build 186)
- Removed fragile auto-detection of callback byte conventions (Build 192)

### Changed

- Mouse driver architecture: BIOS services primary, direct KBC fallback
- Kernel alignment pad bumped (0x11A0 → 0x13A0) for new mouse init code

---

## [3.16.0] - 2026-02-14

### Added (Build 161) - Hard Drive Boot Support

- **Hard Drive Boot Verified** (Build 161)
  - Full MBR → VBR → Stage2_hd → Kernel boot chain tested
  - FAT16 filesystem on 64MB partition with all apps
  - Boots from hard drives, CF cards, and USB flash drives (via BIOS emulation)

- **Boot Drive Query API** (Build 161)
  - New API 43: get_boot_drive — returns boot drive number in AL
  - Enables apps to detect floppy (0x00) vs hard drive (0x80) boot

### Fixed (Build 161)

- Launcher now queries boot drive from kernel instead of hardcoding floppy
- Launcher uses correct mount handle for FAT12/FAT16 routing
- read_bin_header uses dynamic mount handle (was hardcoded FAT12)
- Floppy swap detection skipped when booted from hard drive
- fat16_read 32-bit arithmetic: sector calculation no longer truncates to 16 bits
- MUSIC.BIN added to HD image (was missing from create_hd_image.py)

### Changed

- API table expanded from 43 to 44 functions (get_boot_drive)
- Launcher detects boot media type automatically

---

## [3.15.0] - 2026-02-14

### Added (Builds 151-159) - Window Manager, Sound, Close Button

- **Window Close Button** (Build 152)
  - [X] button drawn at right side of title bar
  - Click to terminate app and destroy window
  - Speaker silenced on app exit (prevents stuck tones)
  - Works for both current and background tasks

- **PC Speaker Sound** (Build 152)
  - New APIs: speaker_tone (41), speaker_off (42)
  - PIT Channel 2 programming for frequency generation
  - Automatic speaker silence on task termination

- **Music Player App** (Build 152)
  - MUSIC.BIN - Beethoven's Fur Elise opening theme
  - Sequential note playback via PC speaker
  - BIOS tick counter timing (~18.2 Hz)
  - Musical note icon in app header

- **Outline Drag** (Build 156)
  - XOR rectangle outline during window drag (Windows 3.1 style)
  - Window moves once on mouse release, single clean repaint
  - Replaced pixel save/restore drag (~235 lines removed)

- **Z-Order Window Management** (Builds 155-159)
  - Background windows blocked from drawing over foreground
  - Topmost window bounds cache for O(1) clipping
  - Active/inactive title bar visual distinction
  - Active: filled white title bar with black text
  - Inactive: black title bar with white text outline
  - Automatic title bar style update on focus change

### Fixed (Builds 151-159)

- Build 153: Floppy read retry logic for reliable loading on real hardware
- Build 154: App load error code diagnostic in launcher
- Build 155: Background windows losing content due to overzealous z-order clipping
- Build 157: Post-drag z-order — desktop icons and background frames showing through moved window
- Build 158: Per-draw-call z-order clipping (point-inside-topmost check)
- Build 159: Simplified to full background draw blocking (fixes multi-pixel bleed-through)

### Changed

- API table expanded from 41 to 43 functions (speaker_tone, speaker_off)
- Window drag: outline-based instead of content save/restore
- Draw API calls from background windows silently dropped (apps repaint on focus)
- Title bar style differentiates active vs inactive windows

---

## [3.14.0] - 2026-02-13

### Added (Builds 144-150) - Desktop Icons, Multi-App, Multitasking

- **Desktop Icon System** (Build 144)
  - 4x2 icon grid with 16x16 2bpp CGA icon bitmaps on desktop
  - BIN file icon headers (80 bytes: JMP + "UI" magic + 12B name + 64B bitmap)
  - Automatic icon detection from BIN headers at boot
  - Default icon for legacy apps without headers
  - Mouse double-click to launch (~0.5s threshold)
  - Keyboard navigation (arrows/WASD + Enter)
  - New APIs: desktop_set_icon (37), desktop_clear_icons (38), gfx_draw_icon (39), fs_read_header (40)

- **Cooperative Multitasking** (Build 144)
  - Round-robin cooperative scheduler (app_yield, app_start)
  - Per-task draw_context save/restore
  - Per-task event filtering (KEY_PRESS to focused, WIN_REDRAW to owner)

- **Multi-App Concurrent Execution** (Build 149)
  - Dynamic segment pool: 6 user segments (0x3000-0x8000)
  - alloc_segment / free_segment kernel helpers
  - Up to 6 concurrent user apps + launcher
  - Scratch buffer moved from 0x5000 to 0x9000

- **Window Title Bar Text** (Build 150)
  - Fixed gfx_draw_string_inverted reading from wrong segment for titles

### Fixed (Builds 145-148)

- Build 145: Window drag content flicker
- Build 146: Desktop z-order, floppy detection, version display
- Build 147: Mouse test app, icon repaint, icon deselect
- Build 148: Double-ESC exit bug (event queue), hello window sizing

### Changed

- API table expanded from 34 to 41 functions
- Memory: 6 dynamic user segments (0x3000-0x8000) replace single 0x3000
- Scratch buffer relocated from 0x5000 to 0x9000
- Launcher rewritten as fullscreen desktop with icon grid

---

## [3.13.0] - 2026-02-11

### Fixed (Build 135) - Text Width Measurement

- **gfx_text_width returned wrong values** - Was reporting 8px per character but draw_char advances 12px (8px glyph + 4px gap). Fixed to return 12px per character, matching actual rendering.
- **Clock content overflowed window** - "00:00:00" = 96px (8×12) but was drawn at X=22 in 108px content area. Repositioned to X=6 for proper centering.
- **Launcher help text caused white boxes** - "W/S/Arrows: Select" = 216px (18×12), far too wide for window. Removed help text from launcher.

### Added (Builds 127-134) - Mouse Cursor, Window Dragging, Drawing Context

- **XOR Mouse Cursor** - 8x10 arrow sprite drawn with plot_pixel_xor (self-erasing)
  - Cursor hide/show with `cursor_locked` flag for flicker-free rendering
  - Visible on all backgrounds (white on black, black on white)

- **Window Title Bar Dragging** - Click and drag windows by title bar
  - Three-layer architecture: IRQ12 detection → drag state machine → deferred processing
  - `mouse_hittest_titlebar` checks all visible windows for click hits
  - `mouse_drag_update` tracks offset and target position
  - `mouse_process_drag` called from event_get_stub (safe from reentrancy)

- **OS-Managed Content Preservation** - Window content saved during drags
  - Scratch buffer at segment 0x5000 stores CGA pixel data
  - Byte-aligned save/restore with `min(old_bpr, new_bpr)` for cross-boundary moves
  - Apps don't need to redraw when their window is dragged

- **Window Drawing Context** (APIs 31-32) - Apps use window-relative coordinates
  - `win_begin_draw` activates context for a window handle
  - `win_end_draw` deactivates context
  - APIs 0-6 automatically translate BX/CX from (0,0)=content-top-left to absolute screen

- **Text Width Measurement** (API 33) - `gfx_text_width` returns string width in pixels

- **gfx_draw_string_inverted** (API 6) - Fixed to use caller_ds for string access
  - Was reading from kernel segment (DS=0x1000) instead of app's segment
  - Caused white garbage boxes when launcher drew help text

### Changed (Builds 127-134)

- API table moved from 0x0F00 to 0x0F80 (more code space)
- API count increased from 30 to 34 (functions 30-33)
- Mouse enabled by default at boot (was disabled)
- Launcher binary: 1304 → 1069 bytes (removed debug code and help text)
- Clock window: W=90 → W=110 with centered time display at X=6

### Added (Build 054) - Hard Drive / FAT16 Support

- **FAT16 Filesystem Driver** - Read-only support for hard drives
  - `fat16_mount` - Mount FAT16 partition from MBR/partition table
  - `fat16_open` - Open files from FAT16 root directory
  - `fat16_read` - Read file data following FAT16 cluster chains
  - `fat16_get_next_cluster` - 16-bit FAT entry reading (simpler than FAT12's 12-bit)
  - `fat16_read_sector` - Sector read with INT 13h LBA extensions + CHS fallback

- **IDE/ATA Direct Access Driver** - Fallback for BIOS issues
  - `ide_detect` - Detect IDE drive presence via IDENTIFY command
  - `ide_wait_ready` - Wait for drive ready (BSY clear, DRDY set)
  - `ide_read_sector` - Direct port I/O read (ports 0x1F0-0x1F7)
  - Supports LBA addressing mode

- **HD Boot Support** - Boot UnoDOS directly from hard drive
  - `boot/mbr.asm` - Master Boot Record with partition table parsing
  - `boot/vbr.asm` - Volume Boot Record with FAT16 BPB
  - `boot/stage2_hd.asm` - HD kernel loader (finds KERNEL.BIN on FAT16)
  - Standard MBR relocation to 0x0600 for VBR loading

- **HD Image Creation Tools**
  - `tools/create_hd_image.py` - Create 64MB FAT16 bootable HD image
  - `tools/hd.ps1` - PowerShell script to write HD image to CF cards
  - Apps automatically included: KERNEL.BIN, LAUNCHER.BIN, CLOCK.BIN, BROWSER.BIN, MOUSE.BIN, TEST.BIN

- **New Makefile Targets**
  - `make hd-image` - Build bootable FAT16 HD image
  - `make run-hd` - Test HD image in QEMU

### Changed

- Filesystem stubs now route by drive type:
  - Drive 0 (A:) -> FAT12 driver (mount handle 0)
  - Drive 0x80+ (HD) -> FAT16 driver (mount handle 1)
- Kernel size increased to accommodate FAT16/IDE drivers

### Technical Details (HD Driver - Build 054)

- **Partition Table Parsing**
  - MBR at sector 0, partition table at offset 0x1BE
  - Supports FAT16 partition types: 0x04, 0x06, 0x0E
  - Hidden sectors field used for partition-relative LBA

- **INT 13h Extensions**
  - Uses AH=42h (extended read) with disk address packet
  - Falls back to CHS conversion for older BIOSes
  - Drive geometry queried via AH=08h

- **IDE Port I/O Protocol**
  - Primary controller: 0x1F0-0x1F7
  - Status polling: Wait for BSY=0, DRDY=1
  - LBA mode via 0xE0 in drive/head register
  - 256-word (512-byte) sector transfer via REP INSW

---

## [3.12.0] - 2026-01-28

### Added (Build 053) - PS/2 Mouse Driver
- **PS/2 Mouse Driver** - Foundation 1.7 complete
  - INT 0x74 (IRQ12) mouse interrupt handler
  - 8042 keyboard controller interface (ports 0x60/0x64)
  - 3-byte packet protocol parsing with sync bit detection
  - Automatic mouse detection at boot
  - Position tracking clamped to screen (0-319 X, 0-199 Y)
  - Button state tracking (left, right, middle)
  - Posts EVENT_MOUSE (type 4) to event queue

- **New Mouse APIs (27-29)**
  - `mouse_get_state` (API 27) - Returns BX=X, CX=Y, DL=buttons, DH=enabled
  - `mouse_set_position` (API 28) - Sets cursor position
  - `mouse_is_enabled` (API 29) - Checks if mouse available

- **MOUSE.BIN Test Application** (578 bytes)
  - Window-based UI with mouse cursor tracking
  - Displays '+' cursor that follows mouse position
  - Shows '*' when button pressed
  - Displays X,Y coordinates
  - Gracefully handles "no mouse detected"
  - ESC to exit

### Added (Build 042) - Dynamic Discovery & Browser
- **fs_readdir API (Index 26)** - Kernel directory iteration
- **Dynamic App Discovery** - Launcher scans for .BIN files
- **BROWSER.BIN** - File browser showing all files with sizes (564 bytes)

### Fixed (Builds 042-052)
- Build 051: Browser ESC doesn't work - Added STI, use JC pattern
- Build 052: Cleanup - Removed debug code

### Added (Build 010-041) - Window Manager
- **Window Manager** - Second Core Services feature (v3.12.0)
  - `win_create_stub` (API 19) - Create new window with position, size, title, and flags
  - `win_destroy_stub` (API 20) - Destroy window and clear its area
  - `win_draw_stub` (API 21) - Redraw window frame (title bar and border)
  - `win_focus_stub` (API 22) - Bring window to front (set z_order to 15, demote others)
  - `win_move_stub` (API 23) - Move window to new position
  - `win_get_content_stub` (API 24) - Get content area bounds for app drawing
  - window_table structure tracks up to 16 windows (512 bytes)
  - Window structure (32 bytes): state, flags, x, y, width, height, z_order, owner_app, title

- **Window Visual Design**
  - 10-pixel white title bar with centered title text
  - 1-pixel white border around window
  - Content area calculation: accounts for title bar and borders
  - Window flags: WIN_FLAG_TITLE (show title bar), WIN_FLAG_BORDER (show border)

- **'W' key handler** in keyboard demo
  - Press 'W' to create a test window at (50, 30) with size 200x100
  - Displays "Window: OK" or "Window: FAIL" status message

### Fixed
- **gfx_clear_area_stub** - Was previously a no-op stub, now properly clears rectangular areas
  - Implements pixel-by-pixel clearing to background color
  - Required for window background clearing

### Changed
- API table expanded from 19 to 30 functions (Builds 010-053)
- API table padding increased from 0x0900 to 0x0B00 (Build 053 - mouse driver code size)
- Keyboard demo prompt updated to show W key option: "ESC=exit F=file L=app W=win:"

### Technical Details (PS/2 Mouse - Build 053)
- PS/2 mouse packet format:
  - Byte 0: YO XO YS XS 1 M R L (overflow, sign, sync bit, buttons)
  - Byte 1: X movement delta (8-bit, sign-extended with XS)
  - Byte 2: Y movement delta (8-bit, sign-extended with YS)
- IRQ12 requires EOI to both slave PIC (0xA0) and master PIC (0x20)
- Sync bit (bit 3) in first byte ensures packet alignment
- AUXB bit (0x20) in status port distinguishes mouse from keyboard data

### Technical Details (Window Manager)
- Window structure (32 bytes):
  - Offset 0: State (0=free, 1=visible, 2=hidden)
  - Offset 1: Flags (bit 0: has_title, bit 1: has_border)
  - Offset 2-3: X position (0-319)
  - Offset 4-5: Y position (0-199)
  - Offset 6-7: Width in pixels
  - Offset 8-9: Height in pixels
  - Offset 10: Z-order (0=bottom, 15=top)
  - Offset 11: Owner app handle (0xFF = kernel)
  - Offset 12-23: Title (11 chars + null)
  - Offset 24-31: Reserved

- Content area calculation:
  - Content X = Window X + 1 (border)
  - Content Y = Window Y + 10 (titlebar) + 1 (border)
  - Content Width = Window Width - 2 (borders)
  - Content Height = Window Height - 12 (titlebar + borders)

### Constraints (v3.12.0)
- No overlapping windows (windows must not overlap)
- No dragging (win_move is API-only, no mouse drag)
- No close button (destroy via API only)
- White only (title bar uses white, no inverse video)

### Future Enhancements
- v3.13.0: Mouse support for window interaction
- v3.14.0: Window dragging via mouse
- v3.15.0: Overlapping window redraw

---

## [3.11.0] - 2026-01-25

### Added
- **Application Loader** - First Core Services feature (v3.11.0)
  - `app_load_stub` (API 17) - Load .BIN applications from FAT12 into heap memory
  - `app_run_stub` (API 18) - Execute loaded applications via far CALL
  - app_table structure tracks up to 16 loaded applications (512 bytes)
  - Entry includes: state, priority, code segment/offset, stack (for future multitasking)
  - BIOS drive number support (0x00=A:, 0x01=B:, 0x80=C:, etc.)

- **Test Application Framework**
  - apps/hello.asm - Simple test app that draws 'H' pattern to verify loader
  - tools/create_app_test.py - Creates FAT12 floppy with HELLO.BIN
  - `make apps` target to build applications
  - `make test-app` target to test app loader in QEMU

- **'L' key handler** in keyboard demo
  - Press 'L' to trigger app loader test
  - Prompts to insert app disk
  - Loads and runs HELLO.BIN from disk

### Fixed (Build 008)
- **Keyboard ISR register corruption** - INT 09h handler was modifying DX register
  without saving/restoring it, causing display corruption on real hardware
  - Added push/pop dx to int_09_handler
- **Error code display position** - Error digit was drawn at X=100, overlapping with
  'I' in "FAIL". Moved to X=136 (after 11-character string)

### Fixed (Build 009)
- **App loader filename format** - fat12_open expects "HELLO.BIN" (with dot separator)
  but kernel was passing "HELLO   BIN" (raw FAT 8.3 format without dot)
  - Changed .app_filename to include dot separator
  - fat12_open parses the dot to split name/extension correctly
- **Dynamic build numbers** - Version and build strings are now generated from files
  - BUILD_NUMBER file contains current build number
  - VERSION file contains version string
  - Makefile generates kernel/build_info.inc before assembly
  - `make bump-build` increments build number for next build

### Changed
- API table expanded from 17 to 19 functions
- Keyboard demo prompt updated to show L key option

### Technical Details
- App calling convention:
  - Entry point at offset 0x0000 within loaded segment
  - Kernel calls app via far CALL
  - App returns via RETF with return code in AX
  - Apps can discover kernel API via INT 0x80 (returns ES:BX = API table pointer)

- Memory layout:
  - Kernel at 0x1000:0000 (28KB)
  - Heap at 0x1400:0000 (apps loaded here via mem_alloc)

- App table entry (32 bytes):
  - Offset 0: State (0=free, 1=loaded, 2=running, 3=suspended)
  - Offset 2: Code segment
  - Offset 4: Code offset (entry point)
  - Offset 6: Code size
  - Offset 8-10: Stack segment/pointer (for future multitasking)
  - Offset 12-22: Filename (8.3 format)

### Future Enhancements Prepared
- App table includes state field for cooperative multitasking
- Stack segment/pointer fields for context switching
- Priority field for future scheduler

## [3.10.1] - 2026-01-24

### Added
- **Multi-cluster file reading** - FAT12 driver now reads files larger than 512 bytes
  - get_next_cluster() function reads FAT12 entries and follows cluster chains
  - Handles 12-bit FAT entries with even/odd cluster logic
  - FAT sector caching (512-byte fat_cache buffer + fat_cache_sector tracker)
  - Reads end-of-chain markers (0xFF8-0xFFF) to detect file end

- **Enhanced test infrastructure**
  - tools/create_multicluster_test.py - generates 1024-byte test files
  - Test file spans 2 clusters with "CLUSTER 1:" and "CLUSTER 2:" markers
  - FAT chain validation: cluster 2 → cluster 3 → EOF
  - make test-fat12-multi target for testing
  - fs_read_buffer expanded from 512 to 1024 bytes

- **Hardware debugging documentation**
  - docs/FAT12_HARDWARE_DEBUG.md - complete debugging process documentation

### Fixed
- **Stack cleanup bug in fat12_open .found_file** (Critical)
  - DS register pushed during search loop wasn't being popped in .found_file path
  - Caused system hang after finding file
  - Fixed by adding `add sp, 2` for DS cleanup

- **LBA to CHS conversion in fat12_read** (Critical)
  - Original code used bitmasks instead of proper division
  - DH (head) was never calculated
  - ES segment wasn't set for INT 13h read
  - BX (buffer pointer) was clobbered during division
  - Fixed with proper formula matching fat12_open's working code

- **Simplified attribute reading in fat12_open**
  - Removed unnecessary ES segment override
  - DS is already 0x1000 in the search loop

### Changed
- **fat12_read() rewritten for multi-cluster support**
  - Now loops through cluster chain until EOF or all bytes read
  - Reads each cluster sequentially into bpb_buffer
  - Copies data to user buffer and advances pointer (ES:DI) automatically
  - Updates file position correctly for multi-cluster reads

- **Debug code removed for release**
  - Removed D:, S:, A:, F: debug output from fat12_open
  - Removed comparison result (=/!) debug output
  - Removed unused debug strings (.dbg_dir, .dbg_srch, etc.)
  - Build string changed from "debug11" to "release"

### Technical Details
- FAT12 cluster chain algorithm:
  - Calculate FAT offset: `(cluster × 3) / 2`
  - Determine FAT sector: `reserved_sectors + (offset / 512)`
  - Cache FAT sector if not already loaded
  - Read 2 bytes from FAT at offset
  - If cluster is even: `value = word & 0x0FFF`
  - If cluster is odd: `value = word >> 4`
  - Check for end-of-chain: `value >= 0xFF8`

- LBA to CHS conversion for 1.44MB floppy (18 sectors/track, 2 heads):
  - Sector: `(LBA % 18) + 1`
  - Head: `(LBA / 18) % 2`
  - Cylinder: `LBA / 36`

### Hardware Verified
- ✅ Tested on HP Omnibook 600C (486DX4-75)
- ✅ Mount: OK
- ✅ Open TEST.TXT: OK
- ✅ Read: OK (multi-cluster)
- ✅ C1:A C2:B displayed correctly

### Testing
```bash
make test-fat12-multi      # Test with 1024-byte file
# Boot, press F, swap to test floppy
# Expected output: Mount: OK, Open: OK, Read: OK, C1:A C2:B
```

### Notes
- This release marks completion of FAT12 filesystem on real hardware
- Multi-cluster support enables loading applications larger than 512 bytes
- Critical foundation for Application Loader (v3.11.0)

---

## [3.10.0] - 2026-01-23

### Added
- **Foundation 1.6: Filesystem Abstraction Layer + FAT12 Driver** (Complete)
  - Filesystem driver abstraction (VFS-like interface)
  - FAT12 filesystem driver (boot sector BPB parsing, directory search, file reading)
  - Filesystem API functions added to kernel API table:
    - fs_mount_stub() - Mount filesystem on drive (API offset 12)
    - fs_open_stub() - Open file by name (API offset 13)
    - fs_read_stub() - Read file contents (API offset 14)
    - fs_close_stub() - Close file handle (API offset 15)
    - fs_register_driver_stub() - Register loadable driver (API offset 16, reserved for Tier 2/3)
  - File handle table (16 handles, 32 bytes each = 512 bytes)
  - BPB cache for filesystem metadata (512 bytes)
  - 8.3 filename support (FAT12 directory entry parsing)
  - Root directory search (up to 224 entries on 360KB floppy)
  - Single-cluster file reading (512 bytes per cluster)
  - Error handling with error codes: FS_OK, FS_ERR_NOT_FOUND, FS_ERR_NO_DRIVER, FS_ERR_READ_ERROR, etc.

- **Three-Tier Architecture Design**
  - Tier 1: Boot and run from single 360KB floppy (FAT12 built-in)
  - Tier 2: Multi-floppy system with loadable drivers (FAT16/FAT32 modules)
  - Tier 3: HDD installation with installer tool and bootloader writer
  - Driver registration hooks for future loadable filesystem modules

- **Test Infrastructure**
  - FAT12 test floppy creation script (Python)
  - TEST.TXT file on FAT12 image for validation
  - test_filesystem() function demonstrates fs_mount/open/read/close

### Changed
- **Kernel expanded from 24KB to 28KB** (48 → 56 sectors)
  - Accommodates FAT12 driver implementation (~2.7 KB)
  - New size: 28,672 bytes (28KB)
  - Stage2 loader updated to load 56 sectors
  - Still 85%+ free space remaining (estimated ~3.6 KB used)

- **Kernel API table relocated**
  - Moved from offset 0x0500 (1280 bytes) to 0x0800 (2048 bytes)
  - Provides 768 bytes additional headroom for future code
  - API table now at 0x1000:0x0800
  - Function count expanded from 12 to 17 slots

- **Entry point modified**
  - Replaced keyboard_demo with test_filesystem() for v3.10.0 testing
  - Can be reverted to keyboard_demo after filesystem validation

### Technical Details
- FAT12 implementation:
  - Reads boot sector (sector 0) via BIOS INT 13h
  - Parses BPB (BIOS Parameter Block): bytes_per_sector, sectors_per_cluster, root_dir_entries, etc.
  - Calculates root_dir_start and data_area_start from BPB
  - Searches root directory (14 sectors on 360KB floppy)
  - Converts user filename to 8.3 FAT format (space-padded)
  - Finds matching directory entry, extracts starting cluster and file size
  - Reads file data from cluster (sector = data_start + (cluster-2) * sectors_per_cluster)
  - Allocates file handle from 16-entry table
  - Current limitations: read-only, single cluster per read, position=0 only

- Filesystem abstraction:
  - Driver structure with function pointers (mount, open, read, close, list_dir, etc.)
  - Driver registry for up to 4 filesystem drivers
  - Auto-detection mechanism (tries each registered driver's detect function)
  - Mount table (4 entries, 16 bytes each) for active filesystem mounts
  - File handle table (16 entries, 32 bytes each) for open files

- Size impact:
  - Abstraction layer: ~400 bytes
  - FAT12 driver: ~1,200 bytes
  - Data structures: ~1,088 bytes (mount table, file table, BPB cache, read buffer)
  - Total: ~2,688 bytes
  - Remaining kernel space: ~25 KB free (87% available)

### Implementation
- fat12_mount(): Reads boot sector, parses BPB, calculates layout
- fat12_open(): Searches root directory, converts filename to 8.3, allocates handle
- fat12_read(): Reads cluster data, copies to user buffer, updates position
- fat12_close(): Marks file handle as free
- fs_mount_stub(): Calls fat12_mount for drive 0, returns mount handle
- fs_open_stub(): Validates mount handle, calls fat12_open
- fs_read_stub(): Validates file handle, calls fat12_read
- fs_close_stub(): Validates file handle, calls fat12_close

### Foundation Layer Progress
- ✓ System Call Infrastructure (v3.3.0)
- ✓ Graphics API (v3.4.0)
- ✓ Memory Allocator (v3.5.0)
- ✓ Kernel Expansion to 24KB → 28KB (v3.6.0 → v3.10.0)
- ✓ Aggressive Optimization (v3.7.0)
- ✓ Keyboard Driver (v3.8.0)
- ✓ Event System (v3.9.0)
- ✓ **Filesystem Abstraction + FAT12 (v3.10.0) - JUST COMPLETED**

### What's Next
Foundation Layer complete! Next phases:
1. **Core Services (v3.11.0-v3.13.0)**: App Loader, Window Manager
2. **Standard Library (v3.14.0)**: graphics.lib, unodos.lib for C development
3. **Tier 2/3 Support**: Multi-floppy loading, HDD installation, FAT16/FAT32 drivers

### Known Limitations
- Read-only filesystem (no write support)
- Single cluster reads only (512 bytes max)
- No multi-cluster file spanning support
- No subdirectory support (root directory only)
- No long filename support (8.3 only)
- File position fixed at 0 (no seek support)

These limitations are acceptable for v3.10.0 foundation. Advanced features will be added in future versions.

## [3.9.0] - 2026-01-23

### Added
- **Foundation 1.5: Event System** (Complete)
  - Circular event queue (32 events, 3 bytes each = 96 bytes)
  - Event structure: type (byte) + data (word)
  - post_event() function for posting events to queue
  - event_get_stub() - Non-blocking event retrieval (API offset 8)
  - event_wait_stub() - Blocking event wait (API offset 9)
  - Event types: KEY_PRESS (1), KEY_RELEASE (2), TIMER (3), MOUSE (4)
  - Keyboard integration: INT 09h now posts KEY_PRESS events

### Changed
- **Keyboard Demo Updated to Use Event System**
  - Now uses event_wait_stub() instead of kbd_wait_key()
  - Demonstrates event-driven programming model
  - Updated instruction text: "Uses: Event System + Graphics API"
  - Updated exit message: "Event demo complete!"
  - Validates event system integration with keyboard driver

### Technical Details
- Event queue: 32-event circular buffer (96 bytes total)
- Each event: 1 byte type + 2 bytes data
- Queue management: head/tail pointers with wraparound at 32
- Keyboard events: ASCII character stored in data field
- Backward compatibility: kbd_getchar/kbd_wait_key still available
- Dual posting: Keys stored in both keyboard buffer and event queue
- Event types extensible for future timer, mouse, custom events

### Implementation
- post_event(): Adds event to tail of queue, advances tail pointer
- event_get_stub(): Removes event from head, returns type and data
- event_wait_stub(): Loops on event_get_stub() until event available
- INT 09h handler: Calls post_event() after storing key in buffer
- Variables: event_queue[96], event_queue_head, event_queue_tail

### Foundation Layer Progress
- ✓ System Call Infrastructure (v3.3.0)
- ✓ Graphics API (v3.4.0)
- ✓ Memory Allocator (v3.5.0)
- ✓ Kernel Expansion to 24KB (v3.6.0)
- ✓ Aggressive Optimization (v3.7.0)
- ✓ Keyboard Driver (v3.8.0)
- ✓ **Event System (v3.9.0) - JUST COMPLETED**

### What's Next
Foundation Layer is now complete! Next phase: Standard Library (graphics.lib, unodos.lib)

## [3.8.0] - 2026-01-23

### Added
- **Foundation 1.4: Keyboard Driver** (Complete)
  - INT 09h keyboard interrupt handler with proper PIC EOI signaling
  - Scan code to ASCII translation tables (normal and shifted)
  - Modifier key state tracking (Shift, Ctrl, Alt)
  - 16-byte circular buffer for keyboard input
  - Non-blocking kbd_getchar() function (API offset 10)
  - Blocking kbd_wait_key() function (API offset 11)
  - Support for alphanumeric keys, punctuation, and special keys
  - Proper handling of key press and release events

- **Interactive Keyboard Demo** (Foundation Layer Integration Test)
  - Tests Graphics API + Keyboard Driver integration
  - Real-time keyboard input echo to screen
  - Displays prompt and instructions on boot
  - Handles special keys: ESC (exit), Enter (newline), Backspace (cursor back)
  - Auto-wraps at screen edges
  - Demonstrates API table function calls working in practice

### Technical Details
- INT 09h handler chains to original BIOS handler after processing
- Two 96-byte scan code translation tables (normal and shifted)
- Circular buffer prevents key loss during high-frequency input
- API table expanded: 10 → 12 function slots
- Estimated size: ~800 bytes (keyboard driver code + translation tables)
- Variables added: old_int9_offset/segment, kbd_buffer[16], buffer pointers, modifier states
- Interrupts enabled via STI after keyboard initialization

### Implementation
- install_keyboard(): Saves original INT 9h vector, installs handler, initializes buffer
- int_09_handler(): Reads scan code (port 0x60), tracks modifiers, translates to ASCII, stores in buffer
- kbd_getchar(): Returns next character from buffer (0 if empty)
- kbd_wait_key(): Blocks until key available, returns character
- Translation supports: A-Z, 0-9, punctuation, Escape, Backspace, Tab, Enter, Space

### Foundation Layer Progress
- ✓ System Call Infrastructure (v3.3.0)
- ✓ Graphics API (v3.4.0)
- ✓ Memory Allocator (v3.5.0)
- ✓ Kernel Expansion to 24KB (v3.6.0)
- ✓ Aggressive Optimization (v3.7.0)
- ✓ Keyboard Driver (v3.8.0)
- ⏳ Event System (v3.9.0 - Next)

## [3.7.0] - 2026-01-23

### Changed
- **Aggressive Kernel Optimization (Pre-Foundation 1.4/1.5)**
  - Removed test functions (test_int_80, test_graphics_api): ~200 bytes freed
  - Removed 21 character alias definitions (char_W, char_E, etc.)
  - Optimized all graphics API functions: replaced pusha/popa with targeted register saves
  - Optimized gfx_draw_rect_stub: eliminated ~80 bytes of redundant push/pop operations
  - Optimized plot_pixel_white: removed variable storage (pixel_save_x/y), stack-only implementation
  - Optimized setup_graphics: tighter BIOS call sequence
  - Optimized install_int_80: minimal register preservation
  - Welcome message now uses gfx_draw_string (string-based) instead of individual char draws

### Technical Details
- Kernel code: 2436 → 2416 bytes (20 bytes from optimization, ~200 from removal)
- Total space gained: ~220 bytes
- Available in 24KB kernel: **22,160 bytes** (sufficient for Foundation 1.4 + 1.5 + future features)
- Removed variables: pixel_save_x, pixel_save_y
- Removed test chars: test_W_char, test_eq_char
- Optimized functions maintain identical behavior, purely size/speed improvements

### Rationale
- Maximize space for Foundation 1.4 (Keyboard Driver ~800B) and 1.5 (Event System ~400B)
- Eliminate production overhead from debug/test code
- Optimize frequently-called graphics primitives
- Prepare for remaining Foundation Layer implementation

## [3.6.0] - 2026-01-23

### Changed
- **Kernel Expansion: 16KB → 24KB**
  - Kernel size increased from 16384 bytes (32 sectors) to 24576 bytes (48 sectors)
  - Provides headroom for Foundation 1.4 (Keyboard Driver, ~800 bytes) and Foundation 1.5 (Event System, ~400 bytes)
  - Heap start moved from 0x1400:0000 to 0x1600:0000
  - Available heap reduced from 540KB to 532KB (loses 8KB)

### Technical Details
- Modified boot/stage2.asm: KERNEL_SECTORS 32 → 48
- Modified kernel/kernel.asm: Final padding 16384 → 24576
- New memory layout:
  * 0x1000:0x0000 - Kernel (24KB, was 16KB)
  * 0x1600:0x0000 - Heap start (was 0x1400:0x0000)
  * ~532KB available for applications (was 540KB)
- Kernel headroom: ~7KB for future Foundation Layer components

### Rationale
- v3.5.0 reached exact 16KB capacity with Memory Allocator
- Foundation 1.4 and 1.5 require additional ~1200 bytes minimum
- 24KB expansion is conservative, leaves room for future enhancements
- 8KB heap reduction is negligible (still 532KB for apps)
- See docs/MEMORY_LAYOUT.md for detailed analysis

## [3.5.0] - 2026-01-23

### Added
- **Memory Allocator (Foundation 1.3)**
  - malloc(size): Allocate memory dynamically
  - free(ptr): Free allocated memory
  - First-fit allocation algorithm
  - Heap at 0x1400:0000, extends to ~640KB limit
  - Block header structure (size + flags)
  - Integrated with API table (offsets 6, 7)

### Technical Details
- Memory block header: 4 bytes [size:2][flags:2]
  * size: Total block size including header
  * flags: 0x0000 (free) or 0xFFFF (allocated)
- First-fit search algorithm for allocation
- Automatic heap initialization on first malloc
- Initial heap block: ~60KB (0xF000 bytes)
- 4-byte aligned allocations

### Implementation
- malloc(AX=size) → AX=pointer (offset from 0x1400:0000), 0 if failed
- free(AX=pointer) → frees memory block
- Heap starts at segment 0x1400 (linear 0x14000)
- Applications use ES=0x1400 + offset for memory access

### Size Impact
- Memory allocator: ~600 bytes
- Kernel size: Still 16KB (16384 bytes exact)
- Remaining capacity: ~0 bytes (at maximum)

## [3.4.0] - 2026-01-23

### Added
- **Graphics API Abstraction (Foundation 1.2)**
  - gfx_draw_pixel: Wraps plot_pixel_white for API table access
  - gfx_draw_char: Character rendering with coordinate parameters
  - gfx_draw_string: Null-terminated string rendering
  - gfx_draw_rect: Rectangle outline drawing
  - gfx_draw_filled_rect: Filled rectangle drawing
  - gfx_clear_area: Clear rectangular area (stub for now)

### Fixed
- Welcome message typo: "WELLCOME" → "WELCOME"

### Technical Details
- All graphics functions accessible via kernel API table
- Register-based calling convention:
  * gfx_draw_pixel(CX=X, BX=Y, AL=color)
  * gfx_draw_char(BX=X, CX=Y, AL=ASCII)
  * gfx_draw_string(BX=X, CX=Y, SI=string_ptr)
  * gfx_draw_rect(BX=X, CX=Y, DX=width, SI=height)
  * gfx_draw_filled_rect(BX=X, CX=Y, DX=width, SI=height)
- Kernel size: Still within 16KB limit

### Hardware Testing
- Tested on HP Omnibook 600C (486DX4-75)
- INT 0x80 discovery working ✓
- "OK" indicator displays correctly ✓

## [3.3.0] - 2026-01-23

### Added
- **System Call Infrastructure (Foundation 1.1)**
  - INT 0x80 handler for system call discovery mechanism
  - Kernel API table at fixed address 0x1000:0x0500
  - Hybrid approach: INT 0x80 for discovery + Far Call Table for execution
  - API table header with magic number ('KA' = 0x4B41), version (1.0), function count
  - 10 stub functions for future implementation:
    * Graphics API (6 functions): draw_pixel, draw_rect, draw_filled_rect, draw_char, draw_string, clear_area
    * Memory management (2 functions): malloc, free
    * Event system (2 functions): get_event, wait_event
- Visual test for INT 0x80 - displays "OK" at bottom right if successful

### Technical Details
- API table positioned at exactly offset 0x0500 (verified in binary)
- Follows Windows 1.x/2.x, GEOS pattern for performance
- Far Call approach saves ~40 cycles per call vs pure INT approach (~9% CPU at 4.77MHz)
- Foundation for third-party application development
- Enables future protected mode transition via thunking

### Documentation
- Added docs/ARCHITECTURE_PLAN.md - Complete architectural analysis and roadmap
- Added docs/SYSCALL.md - System call performance analysis
- Updated README.md with phase-based feature roadmap
- Updated docs/SESSION_SUMMARY.md with architectural decisions

## [3.2.0] - 2026-01-22

### Changed
- **Major architectural change: Split kernel from stage2 loader**
  - Stage2 is now a minimal 2KB loader with progress indicator
  - Kernel is a separate 16KB binary loaded at 0x1000:0000 (64KB mark)
  - Enables kernel to grow beyond 8KB limit
  - Future-proof architecture for XT through 486 hardware
- Boot sequence now shows "Loading kernel" with dot progress bar
- Removed text-mode debug output from bootloader (cleaner boot)
- RAM display now shows correct memory usage (~20KB for loader+kernel)

### Added
- New kernel/ directory for OS code
- kernel/kernel.asm - Main operating system (16KB)
- Kernel signature verification ('UK' = 0x4B55)

### Technical Details
- Disk layout: Boot (1 sector) + Stage2 (4 sectors) + Kernel (32 sectors)
- Stage2 loads kernel sector-by-sector with progress indicator
- Kernel loaded at segment 0x1000 (linear address 0x10000)

## [3.1.7] - 2026-01-22

### Changed
- Character demo redesigned for ASCII verification
  - Displays all 95 printable ASCII characters (32-126) in a 2-row grid
  - Row 1: characters 32-79 (48 chars) at Y=160
  - Row 2: characters 80-126 (47 chars) at Y=168
  - All characters remain visible during long pause (~100 delay cycles)
  - Then clears and repeats for continuous verification
- Clear demo area expanded to 16 pixels height (Y=160-175) for 2 rows

## [3.1.6] - 2026-01-22

### Fixed
- Clock and character demo now visible on HP Omnibook 600C
  - Added ES segment initialization in draw_clock and char_demo_loop
  - ES must point to 0xB800 (CGA video memory) for pixel plotting
- Slowed down animations for 486/DSTN display compatibility
  - Increased delay_short to nested loop (~4 million iterations)
  - Character demo now visible on slow DSTN displays with ghosting

### Changed
- Clock moved to top-left corner (X=4, Y=4)
- Character demo moved below welcome box (Y=160)
- Removed MDA text mode support (CGA-only now)
- Simplified video detection (no longer tracks video_type)

### Added
- Comprehensive coordinate visibility testing
- Confirmed full 320x200 CGA area visible on Omnibook 600C
- New documentation: ARCHITECTURE.md, FEATURES.md
- Comprehensive README update

## [3.1.5] - 2026-01-22

### Fixed
- Fixed graphical corruption of version text caused by overlapping elements
  - Clock moved to Y=40 (above white box which starts at Y=50)
  - Character demo moved to Y=120 (inside box, below version at Y=106)
  - Demo now starts at X=65 to stay within box boundaries (X=60-260)

## [3.1.4] - 2026-01-22

### Fixed
- Clock and character demo now visible on HP Omnibook 600C
  - Clock moved to Y=60 (just above welcome message)
  - Character demo moved to Y=108 (just below version text)
  - Positions within known visible area (Y=4-106 confirmed visible)

### Changed
- Separated clock_loop as independent function
- Added main_loop to coordinate clock and demo updates

## [3.1.3] - 2026-01-22

### Fixed
- Character demo now visible on real hardware (moved from Y=165 to Y=130)
  - Accounts for display overscan on vintage hardware
  - Tested on HP Omnibook 600C

## [3.1.2] - 2026-01-22

### Added
- Real-time clock display in top left corner
  - Reads time from CMOS RTC via BIOS INT 1Ah, AH=02h
  - Displays HH:MM:SS format using 4x6 small font
  - Updates continuously during character demo loop
  - Falls back to "--:--:--" if RTC unavailable
- New functions: draw_clock, draw_bcd_small, clear_clock_area

## [3.1.1] - 2026-01-22

### Added
- Character demo at boot: cycles through all ASCII characters (32-126)
  - Displays characters horizontally at bottom of screen using 4x6 font
  - Clears and repeats in an infinite loop
  - Visual delay between characters for effect
- New functions: char_demo_loop, clear_demo_area, delay_short

### Changed
- Moved RAM status display from bottom right to top right corner

## [3.1.0] - 2026-01-22

### Added
- Complete ASCII bitmap font set (characters 32-126)
  - 8x8 font in boot/font8x8.asm (95 characters, 760 bytes)
  - 4x6 small font in boot/font4x6.asm (95 characters, 570 bytes)
- Generic text rendering functions for any ASCII string:
  - draw_string_8x8: Render null-terminated string with 8x8 font
  - draw_string_4x6: Render null-terminated string with 4x6 font
  - draw_ascii_8x8: Render single ASCII character with 8x8 font
  - draw_ascii_4x6: Render single ASCII character with 4x6 font
- Font tables accessible via font_8x8 and font_4x6 labels
- Legacy character aliases (char_H, char_E, etc.) maintained for compatibility

### Changed
- Font data moved from inline definitions to separate include files
- Makefile updated with NASM include path for font files

## [3.0.1] - 2026-01-22

### Added
- RAM status display in bottom right corner of screen
  - Shows total RAM (from BIOS INT 12h)
  - Shows estimated used memory (~10K for boot code)
  - Shows free memory (total - used)
- New 4x6 small character bitmaps: digits 1-9, R, A, M, K, U, F, s, e, d, r, colon
- draw_number_small function for rendering numbers with small font

## [3.0.0] - 2026-01-22

### Changed
- Major version bump to UnoDOS 3
- New startup message: "Welcome to UnoDOS 3!" with version number below
- Added 4x6 small font for version display
- New 8x8 character bitmaps: C, M, T, U, N, S, 3
- New 4x6 small character bitmaps: v, 3, 0, .

## [0.2.4] - 2026-01-22

### Fixed
- Graphics corruption bug: BIOS teletype output (INT 10h AH=0Eh) was being
  called after switching to CGA graphics mode, causing text to render as
  stray pixels in the top-left corner of the screen
- Removed post-graphics print_string call that caused the corruption
- Hello World graphics now display correctly with no stray pixels

## [0.2.3] - 2026-01-22

### Added
- Pre-built floppy images now included in repository
  - build/unodos.img (360KB)
  - build/unodos-144.img (1.44MB)

### Changed
- Updated .gitignore to track final images, ignore intermediate build files

## [0.2.2] - 2026-01-22

### Fixed
- QEMU boot compatibility: Changed machine type from `-machine pc` to `-M isapc`
  for proper PC/XT BIOS boot behavior
- Boot now works correctly in QEMU with graphical Hello World display

### Changed
- Makefile now uses `isapc` machine type for all QEMU targets

## [0.2.1] - 2026-01-22

### Added
- Windows floppy write utilities
  - tools/writeflop.bat - Batch script for Windows command prompt
  - tools/Write-Floppy.ps1 - PowerShell script with verification
  - Both support 360KB and 1.44MB images
  - Require Administrator privileges for raw disk access

## [0.2.0] - 2026-01-22

### Added
- Boot sector (boot/boot.asm) - 512-byte IBM PC compatible boot loader
  - Loads from floppy drive (BIOS INT 13h)
  - Debug messages during boot process
  - Loads 8KB second stage from sectors 2-17
  - Validates second stage signature before jumping
- Second stage loader (boot/stage2.asm)
  - Memory detection via INT 12h
  - Video adapter detection (MDA/CGA/EGA/VGA)
  - CGA 320x200 4-color graphics mode
  - Graphical "HELLO WORLD!" with custom 8x8 bitmap font
  - MDA fallback with text-mode box drawing
- Build system (Makefile)
  - `make` - Build 360KB floppy image
  - `make floppy144` - Build 1.44MB floppy image
  - `make run` / `make run144` - Test in QEMU
  - `make debug` - QEMU with monitor for debugging
  - `make sizes` - Show binary sizes
  - Dependency checking for nasm and qemu
- Floppy write utility (tools/writeflop.sh)
  - Write images to physical floppy disks
  - Supports both 360KB and 1.44MB formats
  - Verification after write
  - Safety checks to prevent accidental overwrites

## [0.1.0] - 2026-01-22

### Added
- Initial project setup
- Project documentation (CLAUDE.md) with target specifications
- Documentation structure (docs/, VERSION, CHANGELOG.md, README.md)
- Target hardware: Intel 8088, 128KB RAM, MDA/CGA displays, floppy drive
- Architecture defined: GUI-first OS with direct BIOS interaction, no DOS dependency
