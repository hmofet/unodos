# UnoDOS feature matrix

How the targets compare, feature by feature. x86 is the reference
implementation; every other target is a port — the 68K/6502/65816 ports
are native rewrites against [PORT-SPEC.md](PORT-SPEC.md), and the PS2 /
Dreamcast ports reuse the portable C core ([../mac/unodos.c](../mac/unodos.c))
over a Mac-compat shim. Updated 2026-06-15 (x86 v3.31.0 build 420; the
new-ports program — Apple II, IIGS, SNES, PS2, Dreamcast — and the
standalone MacPlus OS are all at app parity).

Column key: **x86** = IBM PC reference (incl. the 8088 hardware-fidelity
build), **Amg** = Amiga, **M7** = Mac System 7 (hosted Toolbox), **M1-6** =
Mac System 1–6 (hosted Toolbox), **MacP** = MacPlus standalone OS
(bare-metal), **Gen** = Sega Genesis, **A2** = Apple II, **IIGS** = Apple
IIGS, **SNES** = Super Nintendo, **PS2** = Sony PlayStation 2, **DC** =
Sega Dreamcast.

## Maturity & verification

| Port | Status | Harness / emulator | Real hardware |
|---|---|---|---|
| **x86 PC** | reference, feature-complete | QEMU (scripted scenarios) | ✅ tested (8088→486, PS/2 L40, Eee PC) |
| **x86 8088/XT** | M0–M2 done, M3 mostly | MartyPC + GLaBIOS (cycle-accurate 8088) | ⏳ physical XT pending |
| **Amiga** | M3+ | WinUAE + AROS ROM (AUTOTEST builds) | ⏳ A500 smoke test pending |
| **Mac System 7** | M3 | Executor (ROM-free) | ⏳ Mac II-class pending |
| **Mac System 1–6** | M3 (minus color Theme) | Executor | ⏳ Mac Plus pending |
| **MacPlus (OS)** | M3, full app parity | Unicorn harness + Mini vMac / vMac II | ✅ real Mac SE (FloppyEmu) |
| **Sega Genesis** | M6+ | BlastEm (15 AUTOTEST builds) | ⚠️ boots on flashcart (2026-06-12); PS/2 / tape / Sega CD adapters not yet exercised |
| **Apple II** | M1–M3 done | py65 ROM-free harness; `.woz`/`.nib` built | ⏳ AppleWin/FloppyEmu (IIc) pending |
| **Apple IIGS** | M0–M3, full parity | from-scratch py65816 core, 9 suites green | ⏳ GSplus/KEGS/MAME + audio-ear pending |
| **Super Nintendo** | M0–M3 done | Mesen2 F12 captures | ⏳ flashcart + audio-ear pending |
| **Sony PS2** | M0–M3 done | PCSX2 (boot @60fps) | ⏳ real PS2; USB+audio coded but not emulator-exercisable |
| **Sega Dreamcast** | at parity | Flycast @60fps + VMU round-trip | ⏳ CD-R / dc-tool + audio-ear pending |
| **Sega Master System** *(3.1-fresh)* | M1–M3 + Dostris game + PSG audio | BlastEm (AUTOTEST scripted-pad builds) | ⏳ real SMS + audio-ear pending |
| **Nintendo NES** *(3.1-fresh)* | M1–M3 + Dostris + APU audio (`minimal` profile) | Mesen2 (software-render grab, AUTOTEST scripted-pad) | ⏳ real NES pending |
| **Game Boy / Color** *(3.1-fresh)* | M1–M3 + Dostris + APU audio (`minimal`, DMG+GBC colour) | Mesen2/GBC (software-render grab, AUTOTEST scripted-pad) | ⏳ real DMG/GBC pending |
| **Sega Game Gear** *(3.1-fresh)* | M1–M3 + Dostris + PSG audio (`minimal`, 12-bit colour) | Mesen2/GG (software-render grab, AUTOTEST scripted-pad) | ⏳ real GG pending |
| **Game Boy Advance** *(3.1-fresh)* | M1–M3 + Dostris + APU audio (`minimal`, Mode-3 framebuffer) | Unicorn ARM7TDMI core (ROM-free harness, AUTOTEST scripted-pad) | ⏳ real GBA + audio-ear pending |
| **Commodore VIC-20** *(3.1-fresh)* | M1–M3 + Dostris + VIC audio (`minimal`, 22×23 char cells) | py65 ROM-free harness (AUTOTEST scripted-joystick) | ⏳ real VIC-20 / VICE pending |
| **Bandai WonderSwan** *(3.1-fresh)* | M1–M3 + Dostris + sound channel (`minimal`, 224×144 mono tiles) | Unicorn x86/V30MZ core (ROM-free harness, AUTOTEST scripted-pad) | ⏳ real WonderSwan + audio-ear pending |
| **NEC PC Engine** *(3.1-fresh)* | M1–M3 + Dostris + PSG audio (`minimal`, 256×224 4bpp tiles) | py65+HuC6280 core (ROM-free harness, AUTOTEST scripted-pad) | ⏳ real PC Engine + audio-ear pending |
| **Raspberry Pi** *(3.1-fresh)* | M1–M3 + Dostris + PWM audio + **PL011 UART input** (`minimal`, 640×480 32bpp mailbox FB) | Unicorn AArch64 core (ROM-free harness emulating the mailbox + system timer + PL011 RX; AUTOTEST + live serial input) | ⏳ real Pi (USB-HID + audio-ear) pending |
| **PinePhone** *(3.1-fresh)* | M1–M3 + Dostris + **A64 UART input** (`minimal`, 480×640 portrait 32bpp DE2 FB; audio UI-only) | Unicorn AArch64 core (ROM-free harness; DE2 sink + `cntpct_el0` + 16550 RX; AUTOTEST + live serial input) | ⏳ real PinePhone (touch + AC200 audio) pending |
| **PowerPC Mac** *(3.1-fresh)* | M1–M3 + Dostris + **Open-Firmware keyboard input** (`minimal`, 640×480 32bpp OF FB; audio UI-only) | Unicorn PPC32 big-endian core (ROM-free harness; OF client interface incl. `read`; AUTOTEST + live console input) | ⏳ real Mac (native ADB + audio) pending |

The last eleven are built **fresh on the 3.1 contract-driven architecture** (not
legacy ports): SMS is a windowed Z80 port; NES is the `minimal`-profile 6502
launcher; Game Boy is the `minimal`-profile SM83 port — the first to add a *new*
generator dialect (`gbz80`/rgbds); Game Gear is `minimal` on SMS silicon, reusing
`gen/z80/` with the GB's 20×18 layout; **Game Boy Advance** is the first **ARM**
world (a new `arm`/GNU-as dialect), drawing a software Mode-3 framebuffer;
**VIC-20** reuses the `gen/6502/` + dasm path as a 22×23 character-cell launcher;
**WonderSwan** is the first **x86 handheld** (NEC V30MZ ≈ 80186, nasm), a
hardware-tile launcher; **PC Engine** is the first **HuC6280** world (a 65C02
superset, `ca65 --cpu huc6280`), a VDC tile launcher; **Raspberry Pi** is the
first **AArch64 / 64-bit** world (`aarch64`/GNU-as), drawing a software framebuffer
the VideoCore firmware allocates over the mailbox; **PinePhone** reuses that
AArch64 core on the Allwinner A64, programming the DE2 mixer UI layer to scan out a
portrait framebuffer; and **PowerPC Mac** is the first big-endian **PowerPC** world
(a new `ppc`/GNU-as dialect), the first to boot over **Open Firmware** — it makes OF
client calls to get its framebuffer. The seven newest are each
verified on a **ROM-free instruction-level harness** (Unicorn ARM / py65 / Unicorn
x86 / py65+HuC6280 / Unicorn AArch64 / Unicorn PPC) where a focus-independent emulator
capture is impractical. They are not yet in the per-feature grids below (which cover
the mature legacy targets); see [../sms/README.md](../sms/README.md),
[../nes/README.md](../nes/README.md), [../gb/README.md](../gb/README.md),
[../gg/README.md](../gg/README.md), [../gba/README.md](../gba/README.md),
[../vic20/README.md](../vic20/README.md), [../ws/README.md](../ws/README.md),
[../pce/README.md](../pce/README.md), [../rpi/README.md](../rpi/README.md),
[../pinephone/README.md](../pinephone/README.md), and
[../ppcmac/README.md](../ppcmac/README.md).

All retro/console ports flag **audio as an "ear-check"** pending real
hardware: the control path (SPC700 mailbox ack, Ensoniq DOC register log,
AICA/SPU2 synth) is asserted, but the actual sound output is verified on
metal, not in the harness.

## Platform / kernel

| | **x86** | **Amg** | **M7** | **M1-6** | **MacP** | **Gen** | **A2** | **IIGS** | **SNES** | **PS2** | **DC** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Approach | bare metal, BIOS only | bare metal, custom chips | Toolbox app | Toolbox app | bare metal | bare-metal cartridge | bare metal | bare metal | bare-metal cartridge | C core + shim | C core + shim |
| Boot medium | floppy / HD / CF / USB | self-booting ADF | .APPL / 800K dsk | .APPL / 800K dsk | own boot blocks | 64KB cart ROM | Disk II GCR | ProDOS/SmartPort block | LoROM cart | FreeMcBoot ELF | KallistiOS `.cdi` |
| Display | CGA 320×200×4 → VESA 640×480×256 | 320×256, 32 colors | 640×480, 8-bit | 512×342, 1-bit | 1-bit (Plus/SE/II) | 320×224 VDP tiles | 280×192, 1-bit | SHR 320×200, 16/4096 | tiles, BGR555 | 640×448×32 (sw FB) | 640×480×32 (sw FB) |
| Mouse cursor | XOR sw sprite | HW sprite | system | system | sw save-under | HW sprite | (keyboard-driven) | sw save-under | OAM sprite | GS overlay | sw |
| Multitasking | cooperative, 5 apps + shell | cooperative, per-task 2KB stacks | cooperative, per-window | cooperative, per-window | cooperative, 2KB stacks | cooperative, 2KB stacks | poll-and-dispatch¹ | cooperative tick | cooperative tick² | cooperative (shim) | cooperative (shim) |
| Max windows | 16 (move + resize) | 6 (move) | 6 (move) | 6 (move) | WM (move) | 6 (move) | WM (kbd) | WM | WM | WM | WM |
| Widgets / dialogs / clipboard | full set (15 widgets, open/save dialogs, 4KB clipboard, undo) | core | core | core | core | core | core | core | core | full set (via core) | full set (via core) |
| Public API | 106 syscalls (INT 0x80) | internal | internal | internal | internal | internal | internal | internal | internal | internal (C) | internal (C) |

¹ Apple II ships poll-and-dispatch; a per-task cooperative scheduler was
prototyped and proven (`scheduler.i`, `-DSCHED_PROTO=1`) but the
full-screen single-app model doesn't need a live scheduler.
² SNES is cooperative-by-ticks — a documented verdict: the 65816 bank-0
stack constraint leaves no room for per-task stacks, so every app's
`*_tick` runs from the main loop.

## Input

| | **x86** | **Amg** | **M7** | **M1-6** | **MacP** | **Gen** | **A2** | **IIGS** | **SNES** | **PS2** | **DC** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Keyboard | PC/XT (custom INT 09h) | native | Toolbox | Toolbox | M0110 / ADB (machine-adaptive) | soft kbd + PS/2 on port 2 | native | ADB (firmware) | soft kbd | pad + USB HID | maple |
| Mouse | PS/2 / USB-legacy / KBC / COM1 serial (XT) | native | Toolbox | Toolbox | M0110/SCC quadrature / ADB | pad-as-mouse + PS/2 on port 1 | (keyboard nav) | ADB (firmware) | pad-as-pointer (+ SNES Mouse backlog) | pad + USB | maple |
| Game-mode controls | n/a | n/a | n/a | n/a | n/a | pad remaps to arrows/action when a game is topmost | keyboard | n/a | pad | pad / USB | maple |

## Storage

| | **x86** | **Amg** | **M7** | **M1-6** | **MacP** | **Gen** | **A2** | **IIGS** | **SNES** | **PS2** | **DC** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Filesystem(s) | FAT12 floppy + FAT16 HD, full R/W | FAT12 on DF1 (PC-interchangeable) | HFS + PC FAT12 floppy R/W | HFS + PC FAT12 floppy R/W | FAT12 + disk-loaded apps | USV1 mini-FS in 8KB battery SRAM | mini-FS (track/sector, GCR — FAT12 doesn't fit) | FAT12 over SmartPort blocks (persistent) | USV1 SRAM mini-FS | memory card (libmc) | VMU (KOS VFS) |
| Extra media | SETTINGS.CFG persistence | — | subdir nav | subdir nav | — | tape/WAV (1-bit AFSK via PSG); Sega CD backup RAM (Mode-1) | — | — | — | — | flush-on-close buffer |

## Audio

| | **x86** | **Amg** | **M7** | **M1-6** | **MacP** | **Gen** | **A2** | **IIGS** | **SNES** | **PS2** | **DC** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Hardware | PC speaker (PIT ch 2) | Paula, 4ch samples | Sound Manager | Sound Manager | sound | PSG: 3 squares + noise | 1-bit `$C030` click | Ensoniq DOC (32-osc wavetable) | SPC700 (uploaded driver) | SPU2 (audsrv)³ | AICA (`snd_sfx`)³ |
| Music app | 5 classical, staff view | Canon in D | Canon in D | Canon in D | Canon in D | Canon in D | Canon in D | ✓ DOC | ✓ (voice 0) | ✓ | ✓ |
| Tracker | ✓ (PC spkr, 1 voice) | ✓ (4ch Paula) | ✓ (4 square) | ✓ | ✓ | ✓ (3 squares + noise) | ✓ (1 voice) | ✓ (4-voice DOC) | ✓ (4 DSP voices) | ✓ | ✓ |

³ Coded and loaded; PS2 SPU2 / DC AICA output is the hardware ear-check
(PCSX2 has no USB HLE and audsrv RPC hangs under fastboot; Flycast boots
with audio live).

## Applications

`✓` = present; `—` = N/A for the platform.

| App | **x86** | **Amg** | **M7** | **M1-6** | **MacP** | **Gen** | **A2** | **IIGS** | **SNES** | **PS2** | **DC** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| SysInfo | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Clock | ✓ (analog + RTC) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ (60 Hz) | ✓ | ✓ |
| Files | ✓ (columns, copy, rename) | ✓ | ✓ (subdirs) | ✓ (subdirs) | ✓ | ✓ (multi-volume) | ✓ | ✓ | ✓ | ✓ | ✓ |
| Notepad | ✓ (selection, clipboard, undo) | ✓ (caret, status bar) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ (append) | ✓ | ✓ |
| Music | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Theme | ✓ (8 presets) | ✓ (4096) | ✓ (256) | — (1-bit) | ✓ (1-bit dither schemes) | ✓ (512) | ✓ (dither) | ✓ (4096) | ✓ (CGRAM) | ✓ (32-bit) | ✓ (32-bit) |
| Tracker | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Dostris | ✓ (+ VGA) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| OutLast | ✓ (+ VGA) | ✓ | ✓ | ✓ | ✓ | ✓ | ⚠️ ~4fps proto | ✓ | ✓ (linear road) | ✓ | ✓ |
| Pac-Man | ✓ (+ VGA) | ✓ | ✓ | ✓ | ✓ | ✓ (HW-sprite actors) | ✓ (1MHz adaptation) | ✓ | ✓ (BG-tile actors) | ✓ | ✓ |
| Paint | ✓ (4 CGA / 256 VGA) | ✓ (4096) | ✓ (256) | ✓ (1-bit + dithers) | ✓ (1-bit) | ✓ (512) | ✓ (dither) | ✓ (4096) | ✓ (pencil, fixed palette) | ✓ (32-bit) | ✓ (32-bit) |
| Settings / MkBoot / Mouse Test / Hello / Runner3D | ✓ | — | — | — | — | — | — | — | — | — | — |

The 8 theme preset palettes (Classic VGA, Midnight, Forest, Sunset,
Ocean, Slate, Candy, Amber) are shared across every color-capable
platform; the Dostris/OutLast/Pac-Man ports share the x86 originals'
piece tables, track/physics and ghost AI byte-for-byte where the
platform allows. The Tracker pattern (32×4) is byte-identical on every
platform, though the on-disk filename varies (`SONG.TRK` on x86/Mac/
Genesis; `SONG.UNO` on Amiga/Apple II).

## 3D — Uno3D

A separate write-once 3D library ([../uno3d/](../uno3d/),
[UNO3D.md](UNO3D.md)) with a swappable per-platform rasteriser backend.
Three backends ship: **soft** (CPU → framebuffer, universal), **ps2-gs**
(GS hardware via gsKit, 60 fps in PCSX2), **dc-pvr** (PowerVR2 via KOS,
verified in Flycast). The x86 OS gets its own native 3D app
([../apps/runner3d.asm](../apps/runner3d.asm)) that draws through the
kernel's `INT 0x80` graphics API instead of the C library. Backend slots
for PS3 / PC / GameCube / Xbox are planned (comments only, not yet
implemented).

## UI toolkit — unoui

A separate write-once widget toolkit ([../unoui/](../unoui/),
[UNOUI.md](UNOUI.md)) for the C-based ports + host — the look-and-feel
analogue of Uno3D: a portable core over `fb.h` plus a swappable **theme**
vtable (palette + metrics + chrome painters, per-painter NULL-fallback).
~20 widgets including menu bar, tabs, slider, spinner, dropdown, and a
multi-line text editor; depth-aware (1/4/8/full-bit via `ui_shade`
ordered dither). Eight themes ship (unodos, macos7, macplus [1-bit],
win31, amiga, c64, apple2, next), host-verified into `themes.png`.
Interaction is a pure function of an abstract `unoui_event` stream
(drag + z-order, focus/Tab, scrollbar/slider thumbs, menus, multi-line
text editing), so a port writes only a small event adapter + `fb`
present; verified into the scripted `storyboard.png`. Distinct from the
kernel-native asm widget set (the "Widgets / dialogs / clipboard" row
above): unoui is the C-side library, not compiled into the asm kernel.
Not yet wired into the port glue `main()`s.
