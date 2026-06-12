# UnoDOS feature matrix

How the five targets compare, feature by feature. x86 is the reference
implementation; the 68K ports are rewrites against
[PORT-SPEC.md](PORT-SPEC.md). Updated 2026-06-12 (x86 v3.27.0 build
406; Amiga M3+; Mac M3; Genesis M6+, real-hardware validated).

## Platform / kernel

| | **x86 PC** | **Amiga** | **Mac System 7** | **Mac System 1–6** | **Sega Genesis** |
|---|---|---|---|---|---|
| Target hardware | IBM PC/XT+, 8088+, 128KB | A500-class, 68000, 512KB | Mac II/LC/Quadra, 68020+ | Mac Plus/SE/Classic, 68000 | Mega Drive/Genesis, 68000, 64KB |
| Approach | bare metal, BIOS only | bare metal, custom chips | Toolbox application | Toolbox application | bare metal, cartridge ROM |
| Boot medium | 1.44MB floppy / HD / CF / USB | self-booting ADF | .APPL / 800K dsk | .APPL / 800K dsk | 64KB cartridge ROM (TMSS-safe) |
| Display | CGA 320×200×4 → VESA 640×480×256, runtime-switchable | 320×256, 32 colors (5 bitplanes) | 640×480, 8-bit Color QuickDraw | 512×342, 1-bit | 320×224 VDP tiles, 4×16-color palette lines |
| Mouse cursor | XOR software sprite | hardware sprite | system cursor | system cursor | hardware sprite |
| Splash | IBM PC art | striped checkmark | happy compact Mac | happy compact Mac | text title card |
| Multitasking | cooperative, 5 apps + shell | cooperative, per-window tasks (2KB stacks) | cooperative, per-window tasks | cooperative, per-window tasks | cooperative, per-window tasks (2KB stacks) |
| Max windows | 16 (move + resize) | 6 (move) | 6 (move) | 6 (move) | 6 (move) |
| Widget toolkit / file dialogs / clipboard | full set (15 widgets, open/save dialogs, 4KB clipboard, undo) | — | — | — | — |
| Public API | 106 syscalls (INT 0x80) | internal | internal | internal | internal |

## Input

| | **x86 PC** | **Amiga** | **Mac 7** | **Mac 1–6** | **Genesis** |
|---|---|---|---|---|---|
| Keyboard | PC/XT (custom INT 09h) | native | Toolbox events | Toolbox events | on-screen soft keyboard + PS/2 on port 2 (real-hw wiring) |
| Mouse | PS/2 / USB-legacy / KBC | native | Toolbox | Toolbox | 3/6-button pad-as-mouse (accel + turbo) + PS/2 on port 1 |
| Game-mode controls | n/a | n/a | n/a | n/a | pad remaps to arrows/action when a game is topmost |

## Storage

| | **x86 PC** | **Amiga** | **Mac 7** | **Mac 1–6** | **Genesis** |
|---|---|---|---|---|---|
| Filesystem(s) | FAT12 floppy + FAT16 HD, full R/W, rename/copy | FAT12 on DF1 (PC-interchangeable), read + write/create | HFS + **PC FAT12 floppy R/W** | HFS + **PC FAT12 floppy R/W** | USV1 mini-FS in 8KB battery SRAM |
| Extra media | settings persistence (SETTINGS.CFG) | — | subdirectory navigation | subdirectory navigation | tape/WAV (1-bit AFSK via PSG + comparator); **Sega CD backup RAM** (Mode-1 Sub-CPU + BIOS BURAM, shared Sega directory) |
| Save UX | system save dialogs | Notepad F1, Tracker/Paint s/l | Cmd-S (HFS or the PC volume), Files `V` toggle | Cmd-S, Files `V` toggle | Notepad F1 to active volume, Files `v` toggle, Tracker s/l + t/y, Paint t/y |
| Planned | — | FAT12 delete/rename polish | — | — | SD card over bit-banged SPI (adapter PCB) |

## Audio

| | **x86 PC** | **Amiga** | **Mac 7** | **Mac 1–6** | **Genesis** |
|---|---|---|---|---|---|
| Hardware | PC speaker (PIT ch 2) | Paula, 4ch sample playback | Sound Manager square synth | Sound Manager square synth | PSG: 3 squares + noise |
| Music app | 5 classical pieces, staff view | Canon in D | Canon in D | Canon in D | Canon in D |
| Game music | Korobeiniki, Sunset Drive | both | both | both | both (PSG ch 1) |
| Tracker (pattern editor) | **yes** — PC speaker (leftmost voice) | **yes** — 4ch Paula, 4 synth instruments | **yes** — 4 square channels | **yes** | **yes** — 3 squares + noise; pattern format byte-identical everywhere |

## Applications

| App | **x86 PC** | **Amiga** | **Mac 7** | **Mac 1–6** | **Genesis** |
|---|---|---|---|---|---|
| SysInfo | ✓ | ✓ | ✓ | ✓ | ✓ |
| Clock | ✓ (analog + RTC) | ✓ (uptime) | ✓ (uptime) | ✓ (uptime) | ✓ (uptime) |
| Files | ✓ (columns, copy, rename) | ✓ | ✓ (subdirs) | ✓ (subdirs) | ✓ (multi-volume) |
| Notepad | ✓ (selection, clipboard, undo, dialogs) | ✓ (caret, line-nav, status bar) | ✓ | ✓ | ✓ |
| Music | ✓ | ✓ | ✓ | ✓ | ✓ |
| Theme | ✓ (Settings app + API 105) | ✓ | ✓ | — (1-bit display) | ✓ |
| Tracker | ✓ (PC spkr, 1 voice) | ✓ | ✓ | ✓ | ✓ |
| Dostris | ✓ (+ VGA variant) | ✓ | ✓ | ✓ | ✓ |
| OutLast | ✓ (+ VGA variant) | ✓ | ✓ | ✓ | ✓ |
| Pac-Man | ✓ (+ VGA variant) | ✓ | ✓ | ✓ | ✓ (hardware-sprite actors) |
| Paint (MacPaint-style; platform-gamut colors) | ✓ (4 CGA / 256 VGA) | ✓ (4096 via pen tuning) | ✓ (256 8-bit) | ✓ (1-bit + dithers) | ✓ (512 via CRAM tuning) |
| Settings / MkBoot / Mouse Test / Hello | ✓ | — | — | — | — |

The 8 theme preset palettes (Classic VGA, Midnight, Forest, Sunset,
Ocean, Slate, Candy, Amber) are shared across every color-capable
platform; the Dostris/OutLast/Pac-Man ports share the x86 originals'
piece tables, track/physics and ghost AI byte-for-byte where the
platform allows.

## Verification & hardware status

| | **x86 PC** | **Amiga** | **Mac 7** | **Mac 1–6** | **Genesis** |
|---|---|---|---|---|---|
| Emulator harness | QEMU (scripted scenarios) | WinUAE + AROS ROM (AUTOTEST builds) | Executor (ROM-free) | Executor | BlastEm (15 AUTOTEST builds) |
| Real hardware | **tested** (8088→486, PS/2 L40, Eee PC) | pending (A500 smoke test) | pending (Mac II-class) | pending (Mac Plus) | **tested, works** (2026-06-12); PS/2 wiring, tape comparator and Sega CD Mode-1 adapters still to be exercised |
