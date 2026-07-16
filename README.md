# UnoDOS 3

A graphical operating system for IBM PC XT-compatible computers, written entirely in x86 assembly language.

![License](https://img.shields.io/badge/license-CC%20BY--NC%204.0-blue)

> **Two lines.** This repo now carries both:
> - **UnoDOS 3 Legacy** — the shipped, real-code-validated OS and its many ports
>   (branch `unodos-3-legacy`, tag `legacy-pre-3.1`). Stable, known-good.
> - **UnoDOS 3.1** — the forward, contract-driven redesign (branch `master`): one
>   machine-readable Contract (`unodef/`) every world is generated from or checked
>   against. All 7 reachable asm ports + x86 consume it byte-identically; the **3.1
>   window ABI** (a greenfield logical window model → per-platform derived layout,
>   `unodef/WMODEL.md`) is shipped on x86 (clean 16 B entry), validated on real
>   hardware + a cycle-accurate 8088. **Eleven ports** were then built **fresh on the
>   3.1 architecture** (not migrated): **Sega Master System** (Z80), **Nintendo NES**
>   (6502), **Game Boy / Color** (Sharp SM83 — a new `gbz80`/rgbds dialect), **Sega
>   Game Gear** (Z80), **Game Boy Advance** (ARM7TDMI — the first ARM world), **VIC-20**
>   (6502), **Bandai WonderSwan** (NEC V30MZ — the first x86 handheld), **NEC PC
>   Engine** (HuC6280), **Raspberry Pi** (ARM Cortex-A — the first **AArch64 /
>   64-bit** world, on the VideoCore mailbox framebuffer), **PinePhone** (Allwinner
>   A64 — the AArch64 core retargeted to the DE2 display engine, in portrait), and
>   **PowerPC Mac** (32-bit PowerPC — the first **big-endian PowerPC** world, booting
>   over **Open Firmware**) — each M1–M3 with a Dostris game + audio. The seven newest
>   are verified on **ROM-free instruction-level harnesses** (Unicorn ARM / py65 / Unicorn
>   x86 / py65+HuC6280 / Unicorn AArch64 / Unicorn PPC) where an emulator can't be captured headlessly under RDP. See
>   **[docs/UNODOS-3.1-MIGRATION.md](docs/UNODOS-3.1-MIGRATION.md)** for status, the
>   design, and next directions.

## Overview

UnoDOS 3 is a GUI-first operating system that boots directly into a windowed desktop environment. It runs on bare metal x86 hardware with no DOS dependency — just BIOS services and an Intel 8088 or later processor. The entire OS, including a ~46KB kernel (loaded from a 104-sector / 52KB reserved area) with 106 system calls, a window manager, two filesystems, cooperative multitasking, and 19 applications, fits on a single 1.44MB floppy disk.

### Philosophy

- **GUI-First**: No command line. The system boots directly into a graphical desktop with draggable icons, windows, and mouse support.
- **Bare Metal**: Runs on raw hardware using only BIOS services. No DOS, no runtime, no dependencies.
- **Vintage-Friendly**: Designed for the constraints of 1980s hardware — runs on an IBM PC/XT-class machine with a CGA card. (Verified on a cycle-accurate 8088: 256KB boots the desktop; 640KB enables the full 5-app multitasking envelope. The kernel itself loads in 128KB but the launcher does not fit below 192KB — see [docs/PORT-8088.md](docs/PORT-8088.md).)
- **Self-Contained**: The kernel, window manager, GUI toolkit, filesystem drivers, and the application set fit on one floppy disk.

### Non-Goals

- Networking (no TCP/IP, no modem)
- Preemptive multitasking (cooperative only)
- Protected mode (real mode for XT compatibility)
- DOS compatibility (different API)

## Ports

The x86 assembly OS above is the reference implementation. UnoDOS also
runs on Motorola 68K platforms, rewritten against the portable contract
in [docs/PORT-SPEC.md](docs/PORT-SPEC.md):

| Port | Target hardware | Approach | Status |
|---|---|---|---|
| [**Amiga**](amiga/) | A500-class, OCS/ECS, 68000, 512KB | Bare-metal: self-booting ADF, copper/bitplanes (32 colors), hardware-sprite cursor, 4-channel Paula audio | **Milestone 3+** — cooperative multitasking, writable FAT12 disks (DF1, PC-interchangeable), splash, 11 apps incl. the three games, Tracker and Paint. **Apps load from disk** — 9 `.APP` binaries on the DF1 FAT12 disk are loaded on-open into per-window `$50000` slots and call the kernel through a fixed API vector table at `$77000` (by ordinal); SysInfo/Clock stay kernel-resident, kernel hunk-exe 30620 → 22872 bytes (−25%) |
| [**Mac System 7**](mac/) | Mac II / LC / Quadra (68020+) | Toolbox-based: 8-bit Color QuickDraw (true-RGB game art), Event/File/Sound Managers | **Milestone 3** — cooperative multitasking, PC-compatible FAT12 floppies (data volumes), 11 apps incl. the three games, Tracker and Paint. **Apps load from storage** — the shared `unodos.c` core is now app-free (a runtime `AppInterface` table replaces the compile-time `switch(proc)`) and all 11 apps are separate loadable modules; host-shim-verified, with the native path build-wired to load modules off the FAT12 volume / CODE resource — on-device (Retro68+Executor) verification pending |
| [**Mac System 1–6**](mac/) | Mac Plus / SE / Classic (68000) | Toolbox-based: classic 1-bit QuickDraw, authentic mono theme | **Milestone 3** — same set minus the color-only Theme app (Paint uses the classic dither patterns); same app-free core + disk-loaded modules as the System 7 build |
| [**MacPlus (standalone OS)**](macplus/) | Mac Plus / SE / Classic (68000) + Mac II class (640×480) | Bare-metal: own boot blocks (ROM bootstrap = BIOS, like the x86 port), own vectors/drivers, 1-bit dither renderer, software cursor; **machine-adaptive input** (Plus M0110/SCC quadrature vs. SE/II ROM-assisted ADB); ROM-free Unicorn harness | **Milestone 3** — full app parity (11 apps incl. the three games, Paint, Music, Tracker, Theme), FAT12 filesystem + **disk-loaded apps** (9 `.APP` binaries on the .Sony disk, loaded into 16 KB slots on demand and multi-resident; SysInfo/Clock and the audio sequencers stay kernel-side; kernel 30932 → 16774 bytes, −46%), sound, cooperative scheduler, on top of the M1 desktop/window-manager (System 7 chrome); validated in Mini vMac (real Plus ROM) + Mini vMac II (real IIcx ROM) **and on a real Mac SE (FloppyEmu)** (see [macplus/README](macplus/README.md)) |
| [**Sega Genesis**](genesis/) | Mega Drive / Genesis (68000, 64KB) | Bare-metal cartridge ROM: VDP tile-cell desktop (Paint runs on unique tiles), hardware-sprite cursor + game actors, pad-as-mouse + soft keyboard, PS/2 on the control ports, PSG audio | **Milestone 6+** — 11 apps incl. the three games, Theme, Tracker and Paint, SRAM + tape/WAV + Sega CD backup-RAM storage, cooperative multitasking; **boots and runs on real hardware** (desktop + apps via flashcart, validated 2026-06-12; the PS/2, tape-comparator and Sega CD Mode-1 adapter paths are not yet exercised on metal) |
| [**Apple II**](apple2/) | Apple ][+ / //e (6502 @ 1MHz, 48–64KB), hi-res 280×192 (1-bit, 7px/byte) | Bare-metal: Disk II ROM autoload → own GCR 6-and-2 RWTS (read+write), hi-res software renderer, keyboard window manager, 1-bit `$C030` speaker | **Milestone 3** — desktop + 10 apps (Theme, Dostris, Pac-Man, Music, Tracker, Paint + SysInfo/Clock/Files/Notepad), mini-FS, blocking square-wave audio, OutLast (~4fps prototype) + cooperative-scheduler verdict. **Apps load from disk** — 8 app binaries are read off the disk through the GCR RWTS into a fixed `$6000` full-screen region (SysInfo + Clock stay small kernel-drawn launcher windows); kernel 14669 → 5194 bytes. ROM-free py65 harness-verified (the RWTS load path is the real one, working on hardware), real-hw (AppleWin/FloppyEmu) pending |
| [**Sony PS2**](ps2/) | PlayStation 2 (MIPS R5900 EE, 32MB, Graphics Synthesizer), FreeMcBoot ELF | Port the portable C core ([mac/unodos.c](mac/unodos.c)) over a software 640×448×32 framebuffer blitted to GS each vsync; DualShock 2 + USB keyboard/mouse | **Milestone 3** — the full desktop / WM / all 11 apps (+ FAT12 round-trip, 32-bit Theme) run via the Mac-compat shim over the software FB, verified on the host shim and the **emulated PS2 GS** in PCSX2 (`ps2/shots/m1_pcsx2_pacman.png`); **memory-card storage** persists Files/Notepad across boots via libmc (`ps2/shots/m2_pcsx2_*.png`); **USB keyboard + mouse** (embedded `usbd`/`ps2kbd`/`ps2mouse`, RAW-HID keymap, absolute mouse + GS cursor) and **SPU2 audio via audsrv** (embedded `audsrv.irx`, square-wave synth) — boot verified at 60 fps in PCSX2 with all of it loaded (`ps2/shots/m3_audio_boot.png`), USB/audio function is hardware-only. **Apps load from storage** — the shared `unodos.c` core is now app-free (the `switch(proc)` dispatch became a runtime `AppInterface` table) and all 11 apps are separate loadable modules; host-shim-verified (the app-free core `dlopen`s and renders all 11), with the native path build-wired to load `.uno` modules from `mc0:/UnoDOS/Apps/` — on-device PCSX2 verification pending. Remaining: real hardware |
| [**Sega Dreamcast**](dreamcast/) | Dreamcast (Hitachi SH-4, 16MB, PowerVR2), KallistiOS `.cdi` | Port the same portable C core over a software **640×480×32** framebuffer copied to the DC framebuffer (`vram_s`) as RGB565 each vblank; maple controller + keyboard + mouse, VMU storage, AICA square-wave audio | **At parity — emulator-verified.** The full desktop / WM / all 11 apps (+ 32-bit Theme), **VMU** save/load, and **AICA** audio all run via the Mac-compat shim at native 640×480. Built clean against KallistiOS (`sh-elf-gcc 15.2.0`, from source) into a real SH-4 ELF + bootable `.cdi` (mkdcdisc) and **verified booting at 60 fps in Flycast** — desktop, every app, and the VMU Notepad save→reload round-trip captured (`dreamcast/shots/dc_*.png`). Also host-shim-verified (`dreamcast/shots/m1_*.png`). **Apps load from storage** — the shared `unodos.c` core is now app-free (runtime `AppInterface` table, not a compile-time `switch`) and all 11 apps are separate loadable modules; host-shim-verified, with the native path build-wired to load `.uno` modules from `/cd/UNODOS/APPS/` (ISO9660) — on-device Flycast verification pending. Remaining: real hardware (incl. the audio ear-check) |
| [**Super Nintendo**](snes/) | SNES / Super Famicom (65816 @ 3.58MHz, 128KB WRAM, PPU + SPC700), LoROM cartridge | Bare-metal cartridge: the Genesis port's twin in 65816 on a **shadow + DMA** model (main loop writes a WRAM tilemap shadow, the vblank NMI DMAs it to VRAM), pad-as-pointer + soft keyboard, battery-SRAM mini-FS, **SPC700 audio** (an uploaded driver built by a tiny Python SPC700 assembler) | **Milestones M0–M3 done — emulator-verified.** Tile desktop / WM / cursor / soft keyboard, USV1 SRAM + Notepad/Files, the three games (Dostris, OutLast, Pac-Man), and the M3 set — **Music**, **Theme** (palette shadow → NMI CGRAM flush), **Tracker** (4 DSP voices), **Paint** (a per-pixel **canvas of unique tiles**), and a cooperative **tick-model scheduler** (the 65816 bank-0 stack constraint rules out per-task stacks — a documented verdict). The SPC700 driver is verified by its mailbox ack ("Audio: SPC700 OK"). Every milestone is captured via Mesen2's F12 framebuffer (`snes/build/m*.png`). Deviations + backlog in [snes/HANDOFF.md](snes/HANDOFF.md). **Hardware: 🟡 conditional pass** — boots + navigable on a SupaBoy Hyperbeach clone (via FXPak Pro), but desktop icons render text-only and audio doesn't play; authentic-SNES verification (icon-tile upload + SPC700 audio) pending |
| [**Apple IIGS**](iigs/) | Apple IIGS (65C816 @ 2.8MHz, 256KB–8MB), Super Hi-Res 320×200 (16 of 4096 colors), Ensoniq 5503 DOC, ADB, SmartPort 3.5″ | Bare-metal: ProDOS/SmartPort block-boot → native 65816, a 4bpp **Super Hi-Res** software renderer (kernel state in fast bank 0, the bank-`$E1` framebuffer reached via 24-bit pointers + long stores so DBR never moves), ADB mouse + keyboard, save-under software cursor, FAT12 over the firmware block driver, **Ensoniq DOC** audio | **Full app parity — harness-verified.** SHR desktop / WM / cursor, FAT12 (Files/Notepad, persistent across reboot), all 11 apps incl. the three games, **4096-colour Theme** (one palette poke recolours the whole desktop), **Ensoniq DOC** Music + 4-voice **Tracker**, and a cooperative tick-scheduler. **Apps load from disk** — 8 `.APP` binaries are read at runtime from the FAT12/SmartPort volume into per-app bank-0 slots and dispatched through their loaded JMP vectors, with multiple apps resident at once (3 verified ticking concurrently); SysInfo/Clock stay kernel-resident, kernel 11636 → 6335 bytes. Built and driven entirely by a **from-scratch Python 65C816 core** + ROM-free harness (no IIGS ROM or emulator needed) — 9 regression suites green (`iigs/shots/m3_*.png`). Remaining: real hardware (GSplus/KEGS/MAME by hand, then FloppyEmu SmartPort) + the audio ear-check. See [iigs/HANDOFF.md](iigs/HANDOFF.md) |
| [**Commodore 64**](c64/) | C64 (6510 @ ~1MHz, 64KB), VIC-II hi-res bitmap 320×200 (per-cell colour), SID, CIA | Bare-metal PRG (BASIC stub `SYS 2061` → takes over): banks the VIC into a **hi-res bitmap with per-cell colour from screen RAM** (genuinely 16-colour, not 1-bit), own renderer / WM, **CIA #1 keyboard-matrix** scanner, **CIA Time-of-Day** clock, SID, **PAL/NTSC auto-detect** from the raster; **larger apps load from disk** through a stable kernel API. No KERNAL calls (SEI + poll) | **Milestones M1–M3 — full 11-app parity, harness-verified.** Colour desktop / WM (SysInfo + Clock), USV1 byte-heap mini-FS with Files + Notepad (persisted across a power cycle), and the M3 set: **Theme** (re-tints the whole desktop), **Dostris** (colour tetrominoes), **Music** + **Tracker** on the **three real SID voices**, **Pac-Man**, **Paint** (16-colour canvas) and **OutLast** (colour-band pseudo-3D). **All apps are disk-loaded** — every one is a separate binary on the `.d64`, loaded to `$5000` via the `$DE00` loader port and linked to the kernel only by the API addresses `mkapi.py` extracts; the kernel holds no app code (`.prg` 8603 → 3775 bytes, ~56% smaller). Built with `dasm` → `.prg` + bootable `.d64`; driven by a **ROM-free py65 harness** that models the VIC raster, CIA matrix + TOD, SID, the FS and the app loader, rendering the bitmap to colour PNG (`c64/shots/*.png`). Remaining: real hardware (VICE, then the 1541/IEC drivers behind the FS + app loader, then SD2IEC). See [c64/HANDOFF.md](c64/HANDOFF.md) |

### Built fresh on the 3.1 contract-driven architecture

These **eight** are *not* legacy ports migrated to the Contract — they were written
from scratch against `unodef/` (the Contract generates their screen geometry,
window/event layout, and enums), proving a new target costs a small generated
surface. All are M1–M3 with a **Dostris** game + audio. The four newest are verified
on **ROM-free instruction-level harnesses** (running the real ROM on a CPU core) where
the emulator can't be captured headlessly under RDP.

| Port | Target hardware | Approach | Status |
|---|---|---|---|
| [**Sega Master System**](sms/) | SMS (Z80 @ 3.58MHz, VDP 315-5124, 8KB RAM) | Bare-metal cartridge: VDP Mode-4 tile desktop, hardware-sprite cursor, Sega mapper, control pad, SN76489 PSG. Consumes `gen/z80/` + `[world.sms]` (sjasmplus) | **M1–M3 + game + audio — BlastEm-verified.** Desktop + a tile **window manager** (sprite cursor, Contract event queue, create/draw/raise/**drag**/close, z-order), apps (SysInfo, live Clock, Notepad, Files, Theme-recolours-CRAM-live, Music), a playable **Dostris**, and **PSG audio**. See [sms/README.md](sms/README.md) |
| [**Nintendo NES**](nes/) | NES (6502/2A03 @ 1.79MHz, PPU 2C02, **2KB RAM**) | Bare-metal iNES NROM-256: PPU tile launcher, patterns in CHR-ROM. The Contract's **`minimal` profile** — no WM, directional nav. Consumes `gen/6502/` + `[world.nes]` (dasm) | **M1–M3 + game + audio — Mesen2-verified + ✅ validated on a real AV Famicom** (2A03 APU audio + `$4016` controller confirmed on hardware — the **first fresh-3.1 port on physical silicon**). Full-screen launcher, pad nav + selection highlight, apps (SysInfo, live Clock, Notepad, Files, Theme-palette, Music on the APU) + **Dostris**. See [nes/README.md](nes/README.md) |
| [**Game Boy / Color**](gb/) | Game Boy / Color (Sharp SM83 @ 4.19MHz, **8KB**) | Bare-metal 32K ROM: BG tile map, DMG greys / GBC palette. **First `gbz80` world** — a new rgbds dialect. `minimal`, vertical-list launcher. | **M1–M3 + game + audio — Mesen2/GBC-verified.** `$FF00` joypad nav, apps incl. live Clock + Theme-BG-palette + Music on the GB APU + **Dostris**; one ROM = DMG greyscale / GBC colour. See [gb/README.md](gb/README.md) |
| [**Sega Game Gear**](gg/) | Game Gear (Z80 @ 3.58MHz, 315-5124 VDP, **8KB**) | Bare-metal 32K ROM: SMS silicon (reuses `gen/z80/` + the SMS bring-up) with the GB's centre-160×144 `minimal` layout; 12-bit CRAM. | **M1–M3 + game + audio — Mesen2/GG-verified.** Mini-icon list, `$DC` pad nav, apps + **Dostris** in full colour + PSG. See [gg/README.md](gg/README.md) |
| [**Game Boy Advance**](gba/) | GBA (ARM7TDMI @ 16.78MHz, 256KB+32KB) | Bare-metal `0x08000000` ROM: a software **Mode-3 framebuffer** (no HW tiles). **First ARM world** — a new `arm`/GNU-as dialect. `minimal`, icon grid. | **M1–M3 + game + audio — Unicorn-ARM-verified.** `REG_KEYINPUT` nav, apps incl. live Clock + Theme-palette + Music (square channel) + **Dostris**. See [gba/README.md](gba/README.md) |
| [**Commodore VIC-20**](vic20/) | VIC-20 (6502 @ 1.02MHz, +8K expansion) | Bare-metal `.prg`: the VIC 22×23 character matrix + custom charset, per-cell colour. `minimal`, character-cell list. Consumes `gen/6502/` (dasm). | **M1–M3 + game + audio — py65-verified.** Joystick nav, apps incl. live Clock + Theme-bg + Music (VIC oscillator) + **Dostris** in colour. See [vic20/README.md](vic20/README.md) |
| [**Bandai WonderSwan**](ws/) | WonderSwan (NEC V30MZ @ 3.07MHz, **16KB**) | Bare-metal 64K ROM (reset vector → JMP FAR): an SCR1 32×32 tilemap, 8×8 2bpp tiles, 224×144 mono. **First x86 handheld** — nasm, the Contract's x86 surface. `minimal`. | **M1–M3 + game + audio — Unicorn-x86-verified.** Keypad nav, apps incl. live Clock + Theme-shade-pool + Music (sound channel 1) + **Dostris**. See [ws/README.md](ws/README.md) |
| [**NEC PC Engine**](pce/) | PC Engine / TG-16 (HuC6280 @ 7.16MHz, **8KB**) | Bare-metal HuCard: HuC6270 VDC 32×32 BAT, 8×8 4bpp tiles, 256×224, 9-bit VCE palette, 8-MPR MMU. **First HuC6280 world** — `ca65 --cpu huc6280`. `minimal`, icon grid. | **M1–M3 + game + audio — py65+HuC6280-verified.** Joypad nav, apps incl. live Clock + Theme-VCE-palette + Music (PC Engine PSG) + **Dostris** in colour. See [pce/README.md](pce/README.md) |
| [**Raspberry Pi**](rpi/) | Pi 3/4 (ARM Cortex-A, **AArch64**, 1GB+) | Bare-metal `kernel8.img` @ `0x80000`: a firmware-allocated **VideoCore mailbox framebuffer** (640×480 32bpp XRGB, no HW tiles). **First AArch64 / 64-bit world** — `aarch64`/GNU-as. `minimal`, 4-col icon grid. | **M1–M3 + game + audio — Unicorn-AArch64-verified.** System-timer-paced nav, apps incl. live Clock + Theme-palette + Music (PWM jack) + **Dostris**. See [rpi/README.md](rpi/README.md) |
| [**PinePhone**](pinephone/) | PinePhone (Allwinner A64, 4× A53, **AArch64**) | Bare-metal payload @ `0x40080000`: programs the **DE2 mixer UI layer** to scan out a 480×640 **portrait** 32bpp DRAM framebuffer. Reuses the Pi AArch64 core; `cntpct_el0` pacing. `minimal`, 4-col grid. | **M1–M3 + game + audio — Unicorn-AArch64-verified.** d-pad nav, apps incl. live Clock + Theme-palette + Music (UI) + **Dostris**. See [pinephone/README.md](pinephone/README.md) |
| [**PowerPC Mac**](ppcmac/) | G3/G4-class (32-bit **PowerPC**, big-endian) | **Open Firmware** client program: OF enters with the CI in r5; `finddevice "screen"` + `getprop` give a 640×480 32bpp framebuffer. **First PowerPC world** — new `ppc`/GNU-as dialect. `minimal`, 4-col grid. | **M1–M3 + game + audio — Unicorn-PPC-verified** (OF client interface emulated). d-pad nav, apps incl. live Clock + Theme-palette + Music (UI) + **Dostris**. See [ppcmac/README.md](ppcmac/README.md) |
| [**Modern PC**](pc64/) | Any x86-64 UEFI PC (~2007+) | Bare-metal **UEFI application** (PE32+ via mingw-w64, no EDK2/gnu-efi): GOP framebuffer + firmware keyboard/pointer, boot services alive — **UEFI-as-BIOS**, the Pi/ppcmac firmware pattern on PC silicon. **First 64-bit x86 world.** Default UI is the **unoui** toolkit as a full themed desktop shell; a `legacy` build keeps the `unodos.c` core + 14 apps. | **QEMU+OVMF-verified + ✅ real hardware (Lenovo X1 Carbon G8).** Default `./build.sh` boots the **unoui shell** (~76 KB, no legacy code): launcher + app windows (Control Panel/Editor/Files/System/Clock/Canvas), 8 live-switchable themes, fill-scaled resolutions, **canvas + fullscreen** toolkit modes, keyboard+pointer nav. `legacy` build adds the family's **first native driver + TCP/IP stack** — Intel **e1000** NIC under a from-scratch **ARP/IP/ICMP/UDP/TCP+DHCP** stack — **TLS 1.2** via BearSSL (pinned key, RDRAND), and **uno3d** 3D (soft rasteriser; Intel-iGPU scaffold). Metal-hardened: eDP SetMode blackout, SMM-trapped debug port, phantom keystrokes, LLP64 DMA truncation; native **I2C-HID trackpad** driver (opt-in). Driver tail: ExitBootServices, xHCI HID, NVMe/AHCI, real Intel-GPU 3D. See [pc64/README.md](pc64/README.md) |

A feature-by-feature comparison of the mature targets lives in
[docs/FEATURE-MATRIX.md](docs/FEATURE-MATRIX.md). The new-ports program
(Apple II ✓, Sony PS2 ✓, Sega Dreamcast ✓, Apple IIGS ✓, SNES ✓) is tracked in
[docs/PORTS-PLAN.md](docs/PORTS-PLAN.md); the standalone MacPlus OS port
joins the matrix at app parity. The Dreamcast port reuses the PS2 port's
portable-C-core approach almost verbatim — same core, same Mac-compat shim,
swapped present (KallistiOS framebuffer) and input (maple) layers.

### App loading

The x86 reference port has always loaded apps from disk: the launcher itself is
a disk file, and apps are flat binaries the kernel pulls in through **kernel API
#18** (load app) and runs. Every storage-equipped port now follows the same
principle — apps are **separate binaries on disk**, not code compiled into the
kernel, linked to the kernel only by an **extracted API table** (the C64's
`mkapi.py` → `kernel_api.inc` + JMP-vector contract, generalised across the
family). Kernels shrink accordingly (C64 −56%, Apple II to ~⅓, MacPlus −46%,
Amiga −25%, the IIGS roughly halved). Two models are used: RAM-tight 8-bit ports
(C64, Apple II) load **one full-screen app at a time** into a fixed region; the
windowed ports (IIGS, MacPlus, Amiga, and the shared Mac/PS2/Dreamcast C core)
load apps **on open into per-app regions** and keep several resident, dispatching
each window through the app's loaded vectors / `AppInterface` table.

The two **cartridge** consoles — **SNES and Genesis** — are deliberately left
with their apps built into the ROM, because cartridge ROM is the correct
delivery medium and there is no removable or writable code storage to load from.

### 3D graphics — Uno3D

UnoDOS has its own portable 3D graphics library, **Uno3D** ([uno3d/](uno3d/),
guide in [docs/UNO3D.md](docs/UNO3D.md)): a write-once 3D API with a swappable
per-platform rasteriser backend. The same 3D game ("UnoDOS Runner") runs on the
**software** rasteriser (any port), the **PlayStation 2 Graphics Synthesizer**,
and the **Dreamcast PowerVR2** — the two consoles using real hardware
acceleration at 60 fps. The bare-metal x86 OS gets its own native 3D app
([apps/runner3d.asm](apps/runner3d.asm)) that draws through the kernel's `INT
0x80` graphics API. The backend interface is built for more targets (PS3, a
GPU-equipped PC, GameCube, Xbox).

### Cross-platform UI toolkit — unoui

The C-based ports share a portable widget toolkit, **unoui** ([unoui/](unoui/),
guide in [docs/UNOUI.md](docs/UNOUI.md)) — the same write-once / swappable-vtable
idea as Uno3D, applied to look-and-feel. An app builds a window's widget tree
**once** (buttons, checkboxes, tabs, menu bar, slider, spinner, dropdown, list,
single- and multi-line text editors, scrollbars, …); a swappable **theme**
restyles all of it — colours you can change *and* graphics you can override —
and it is depth-aware (1/4/8/full-bit via ordered dither). Eight themes ship,
from the unified UnoDOS look to native reproductions of Mac OS 7, a 1-bit Mac
Plus, Windows 3.1, Amiga Workbench, the C64, the Apple II, and NeXTSTEP. All
interaction (window drag with z-order, focus/Tab, scrollbar and slider thumbs,
menus, multi-line text editing) is a pure function of an abstract event stream,
so a port writes only a tiny adapter mapping its native mouse/keyboard to
`unoui_event` plus the framebuffer present. Host-verified into a theme contact
sheet and a scripted-input storyboard. This is the C-side analogue of the
kernel-native asm widget set documented under **GUI Toolkit** below — see
[docs/UNOUI.md §8](docs/UNOUI.md#8-relationship-to-the-kernel-native-widgets).

The x86 reference build above now runs at **full feature parity on a genuine
Intel 8088 / IBM PC-XT** — for years it had only ever run on QEMU (a 486-class
CPU that hides all real 8088 behaviour). Verified on a cycle-accurate emulated
XT (8088 @ 4.77 MHz, CGA, ROM-free GLaBIOS): boot chain → kernel → CGA desktop
→ window manager + cooperative scheduler → the CGA app set (SysInfo, Settings,
Files, Paint, Clock, Notepad, Music, Tracker, Pac-Man) → FAT12 read/write →
keyboard (XT 8255 PPI) and a **Microsoft serial mouse on COM1** (a real XT has
no PS/2 port). It also **boots from a CompactFlash card on an XT-IDE adapter**
(a FAT12 "superfloppy" CF, since the FAT16 hard-disk path is 386-only — full
FAT16-on-8088 is a follow-up). VGA apps are out-of-envelope on a CGA machine,
and a physical-XT pass is the final hardware step. See
[docs/PORT-8088.md](docs/PORT-8088.md) and [tools/xt/](tools/xt/).

All ports boot through a platform-themed **"UnoDOS 3" splash** (striped
checkmark on Amiga, happy compact Mac, IBM PC art on x86) into the
UnoDOS desktop: window manager (z-order, drag, click-to-raise), the
focus-routed event model, and the shared app set — **Files** (with
subdirectory navigation on Mac), **Notepad** (caret editor, line
navigation, live Ln/Co/bytes status bar), **Music** (Canon in D),
**Theme** (8 shared preset palettes + custom colors on every
color-capable platform), **Tracker** (the 32x4 pattern editor with a
byte-identical song format on every platform), **Paint** (the
MacPaint-style editor whose color selector reaches each platform's
full gamut), and the game ports **Dostris**, **OutLast** and
**Pac-Man** with their music. Verified in WinUAE (built-in AROS ROM)
and the ROM-free Executor emulator — no proprietary ROMs needed to
try them.

## Screenshots

*Screenshots coming soon — the OS runs in CGA 320x200 (4-color) and VGA 320x200 (256-color) modes.*

## Features

### Display Modes

UnoDOS supports four video modes, switchable at runtime from the Settings app:

| Mode | Resolution | Colors | Bits/Pixel | Memory |
|------|-----------|--------|------------|--------|
| CGA Mode 4 | 320x200 | 4 | 2 (interlaced) | 0xB800 |
| VGA Mode 13h | 320x200 | 256 | 8 (linear) | 0xA000 |
| VGA Mode 12h | 640x480 | 16 | 4 (planar) | 0xA000 |
| VESA | 640x480 | 256 | 8 (banked) | 0xA000 |

All drawing APIs, widgets, and applications work across all four modes. Higher resolutions automatically scale window content 2x for readability.

### Kernel & System Calls

The kernel provides 106 API functions accessed via `INT 0x80` with the function index in AH. The API covers:

- **Graphics** (APIs 0-6, 67-71, 80, 94, 102-104): Pixel, rectangle, filled rectangle, character, string, inverted string, clear area, colored drawing, horizontal/vertical/Bresenham lines, scroll area, 1-bit sprite drawing, scaled sprite (nearest-neighbor), screen blit (region copy), pixel read
- **Fonts** (APIs 33, 48-49, 93): Three built-in bitmap fonts (4x6 small, 8x8 default, 8x14 large) with runtime selection, text width measurement, and font metrics query
- **Window Manager** (APIs 20-25, 31-32, 64, 78-79, 96-97): Create, destroy, draw, focus, move, resize windows; drawing context for window-relative coordinates; window info query; content area size; content scaling
- **Widgets** (APIs 50-62, 65-66, 87-89, 98-99): Button, radio button, checkbox, text input field, scrollbar (with drag hit-testing), list item, progress bar, group box, separator, combo box, menu bar, word-wrapped text, popup context menus, system file open/save dialogs
- **Events** (APIs 9-10): Non-blocking and blocking event retrieval from a 32-entry circular queue. Event types: KEY_PRESS, KEY_RELEASE, TIMER, MOUSE, WIN_MOVED, WIN_REDRAW
- **Filesystem** (APIs 13-16, 27, 40, 44-47, 75-77): Mount, open, read, write, create, delete, close, readdir, seek, rename, file size query, raw sector write, BIN header read — for both FAT12 (floppy) and FAT16 (hard drive)
- **Input** (APIs 28-30, 83, 101): Mouse state (position + buttons), mouse positioning, mouse detection, keyboard modifier state (Shift/Ctrl/Alt), mouse cursor visibility
- **Clipboard** (APIs 84-86): System-wide 4KB clipboard with copy, paste, and length query — shared between all running apps
- **Audio** (APIs 41-42): PC speaker tone generation (frequency in Hz) and silence, via PIT Channel 2
- **Multitasking** (APIs 18-19, 34-36, 74): Load app, run app (blocking), yield to scheduler, start app (non-blocking), exit task, task info query
- **System** (APIs 43, 63, 72-73, 81-82, 95): Boot drive detection, tick counter, RTC read/write (BCD), delay with yield, screen info, video mode switching
- **Themes** (APIs 54-55): Set/get color theme (text, desktop background, window frame colors)
- **Memory** (APIs 7-8): Heap allocation and free (first-fit allocator)
- **Desktop** (APIs 37-39): Desktop icon registration, icon clearing, 16x16 icon rendering

### Window Manager

- **16 concurrent windows** with z-ordered rendering and per-pixel clipping
- Title bars with app name, borders, and close button [X]
- **Outline drag** — XOR rectangle follows the mouse during drag, window moves on release (Windows 3.1 style)
- **Resize** — drag handle in the bottom-right corner (10x10 hit zone), minimum 60x40 pixels
- **Active/inactive distinction** — focused window gets a highlighted title bar; background windows are dimmed
- **Drawing context** — apps call `win_begin_draw` and then draw at (0,0) meaning top-left of the content area. The kernel auto-translates coordinates to absolute screen position for all drawing, widget, sprite, and menu APIs
- **Content scaling** — in 640x480 modes, windows auto-scale content 2x so apps designed for 320x200 remain usable
- **Z-order clipping** — drawing calls from background windows are silently blocked; apps repaint on focus via WIN_REDRAW events
- **Cursor protection** — the kernel automatically hides/shows the mouse cursor around all drawing operations to prevent XOR corruption from IRQ12

### GUI Toolkit

A complete widget library, all coordinate-translated through the drawing context:

| Widget | Description |
|--------|-------------|
| Button | Raised/pressed states with centered text label |
| Radio Button | Circular selector with fill indicator |
| Checkbox | Square toggle with check mark |
| Text Field | Text input with cursor, selection, and password mode |
| Scrollbar | Vertical/horizontal with draggable thumb, arrows, and track hit-testing |
| List Item | Row with text, optional selection highlight |
| Progress Bar | Fill indicator with percentage text |
| Group Box | Labeled frame for grouping controls |
| Separator | Horizontal or vertical divider line |
| Combo Box | Dropdown selector |
| Menu Bar | Horizontal menu items with padding and hit zones |
| Popup Menu | Context menu with item list and mouse hit-testing |
| File Open Dialog | Modal dialog with scrollable file list, keyboard/mouse nav, Open/Cancel |
| File Save Dialog | Modal dialog with filename text field, file list, overwrite confirmation |
| Word Wrap | Multi-line text rendering with automatic line breaking |

### Desktop

- **Icon grid** — apps discovered from disk, displayed with 16x16 bitmap icons (32x32 scaled in high-res modes)
- **Drag icons** — click and hold to reposition icons on the desktop
- **Lock/unlock** — prevent accidental icon rearrangement
- **Right-click context menu** — Auto Arrange, Sort A-Z, Sort Z-A, Lock/Unlock Icons, Refresh, Exit
- **Keyboard navigation** — arrow keys or WASD to select icons, Enter to launch
- **Double-click launch** — 0.5-second threshold using BIOS timer
- **Boot media auto-detection** — launcher queries the boot drive and mounts the correct filesystem (FAT12 for floppy, FAT16 for HD/CF/USB)

### Filesystem

Two filesystem drivers with a unified API that routes by mount handle:

**FAT12 (Floppy)**:
- 1.44MB floppy disk support
- 12-bit FAT entries, dual FAT copies
- Full read/write: mount, open, read, write, create, delete, rename, seek, readdir
- Settings persistence (SETTINGS.CFG loaded at boot)

**FAT16 (Hard Drive)**:
- 64MB partition with MBR and partition table
- 16-bit FAT entries (256 per sector), dual FAT copies
- Multi-sector cluster support
- LBA access with CHS fallback for older BIOSes (PCMCIA CF cards)
- Full read/write with same API as FAT12

### Multitasking

- **Cooperative round-robin scheduler** — apps yield control with `app_yield`, kernel switches to next runnable task
- **Up to 5 concurrent user apps** plus the launcher shell (6 total)
- **Dynamic segment allocation** — each app loads into its own 64KB segment from a pool (0x3000-0x7000)
- **Per-task state** — the kernel saves and restores registers, stack pointer, draw context, font selection, and caller segments on every context switch
- **Focus-aware input** — only the focused app receives keyboard events; mouse events go to the appropriate window owner
- **Automatic cleanup** — when an app exits, its windows are destroyed, speaker is silenced, and its segment is freed

### Input

**Keyboard:**
- Custom INT 09h handler (not BIOS INT 16h) with scan code translation
- 16-byte key buffer with per-task event delivery
- Modifier tracking: Shift, Ctrl, Alt states queryable via API

**Mouse:**
- Primary: BIOS INT 15h/C2 services (works with USB mice via BIOS legacy emulation)
- Fallback: Direct KBC port I/O with IRQ12 for BIOSes without INT 15h/C2 support
- **IBM PC/XT: Microsoft serial mouse on COM1** (IRQ4) — a real XT has no PS/2
  port; on a pre-AT machine the kernel programs the COM1 UART (1200/7N1),
  detects the mouse via its `'M'` identifier, and decodes the 3-byte protocol
  in an IRQ4 handler (verified on a cycle-accurate 8088 — see [docs/PORT-8088.md](docs/PORT-8088.md))
- XOR sprite cursor (8x8) with automatic hide/show during drawing
- Boot-time auto-detection with diagnostic letter: B (BIOS), K (KBC), C (COM serial), E (no mouse)

### Audio

PC speaker tone generation via PIT Channel 2 — specify frequency in Hz. The speaker is automatically silenced when an app exits. Used by the Music app (5 classical pieces) and Dostris (Korobeiniki background music).

### Boot Chain

**Floppy boot**: Boot sector (512 bytes) loads Stage 2 (2KB), which loads the kernel (104 sectors / 52KB reserved area) with a progress indicator. Each stage verifies a magic signature before transferring control.

**Hard drive boot**: MBR relocates to 0x0600, reads VBR from the first partition, VBR loads Stage 2, Stage 2 parses the FAT16 BPB and loads KERNEL.BIN from the filesystem. Supports both LBA (INT 13h extensions) and CHS fallback, with diagnostic output showing the read method and root directory LBA.

**Boot media**: 1.44MB floppy, hard drive, CompactFlash (PCMCIA or IDE), USB flash drive — anything the BIOS can boot from.

### Settings Persistence

The Settings app saves user preferences to SETTINGS.CFG on the boot drive:
- Font selection (4x6 / 8x8 / 8x14)
- Color theme (text color, desktop background, window frame color)
- Video mode (CGA / VGA 320x200 / VGA 640x480 / VESA 640x480)
- Real-time clock adjustment

Settings are loaded automatically at boot before the launcher starts.

### Fonts

Three built-in bitmap fonts, selectable per-app at runtime:

| Font | Size | Advance | Chars/Line (320px) | Use |
|------|------|---------|--------------------|-----|
| Font 0 | 4x6 | 6px | 53 | Small labels, status bars |
| Font 1 | 8x8 | 12px | 26 | Default body text |
| Font 2 | 8x14 | 12px | 26 | Large headings |

### Sprites & Blitting

- **1-bit sprite drawing** — transparent bitmap with caller-specified color, any width/height
- **Scaled sprite** — nearest-neighbor scaling to arbitrary destination size
- **Screen blit** — copy a rectangular region of the screen (used for smooth scrolling, animations)
- **Read pixel** — query the color value at any screen coordinate

## Applications (19 in the tree)

| App | Size | Description |
|-----|------|-------------|
| **Launcher** | 8KB | Desktop shell — 4x3 icon grid (8x5 in high-res), keyboard/mouse navigation, icon drag, right-click context menu (sort, arrange, lock), double-click launch, floppy refresh, boot media auto-detection |
| **Notepad** | 61KB | Full text editor — text selection, clipboard (Ctrl+C/V/X), undo (Ctrl+Z), context menu (Cut/Copy/Paste/Delete), File menu (New/Open/Save), word wrap, vertical scrolling, system file dialogs |
| **File Manager** | 25KB | File browser — scrollable file list with scrollbar, columns (name, size, date), delete with confirmation, rename dialog, file copy with progress, FAT12/FAT16 support, keyboard arrow navigation |
| **Dostris** | 47KB | Tetris clone (CGA) — 7 tetrominoes, rotation, levels with increasing speed, score tracking, Korobeiniki background music on PC speaker, game over screen |
| **Dostris VGA** | 48KB | Tetris clone (VGA 256-color) — same gameplay in VGA mode 13h with 256-color graphics |
| **OutLast** | 47KB | Driving game (CGA) — pseudo-3D perspective road with curves, traffic cars, speed/steering mechanics, crash detection, roadside scenery, score tracking |
| **OutLast VGA** | 60KB | Driving game (VGA 256-color) — same gameplay in VGA mode 13h with richer graphics, title screen with PC speaker music |
| **Clock** | 11KB | Analog clock with hour/minute/second hands and digital time readout, driven by the BIOS real-time clock |
| **Music** | 33KB | PC speaker music player — 5 classical songs with scrolling musical staff visualization, note rendering (C4-G5 range), play/pause and prev/next controls |
| **Settings** | 34KB | System configuration — font selector with preview, color theme picker (text/background/window, 4 swatches each), video mode selector (CGA/VGA/Mode12h/VESA), RTC time adjustment, defaults button, persists to SETTINGS.CFG |
| **MkBoot** | 24KB | Boot floppy creator — reads OS and apps from the boot drive, prompts for a blank floppy, writes a complete bootable UnoDOS floppy (boot sector + stage2 + kernel + FAT12 filesystem + all apps) |
| **SysInfo** | 11KB | System information — displays UnoDOS version, current video mode and resolution, number of running tasks and open windows, real-time clock, available memory, boot drive info |
| **Pac-Man** | 30KB | Arcade port (CGA) — full 28x25 maze, three-ghost AI with scatter/chase schedule, frightened mode with the 200-1600 chain |
| **Pac-Man VGA** | 31KB | Same gameplay in VGA mode 13h |
| **Tracker** | 2KB | Pattern music editor — 32 rows x 4 channels, the SONG.TRK pattern layout shared byte-for-byte with the Amiga/Mac/Genesis ports (the on-disk filename is `SONG.TRK` here, `SONG.UNO` on Amiga/Apple II); PC-speaker playback voices the leftmost channel |
| **Paint** | 31KB | MacPaint-style bitmap editor — pencil/brush/eraser/line/rect/oval/fill/spray, drag drawing, color picker covering the active mode's full palette (4 CGA / 256 VGA) |
| **Mouse Test** | 8KB | Mouse diagnostic — shows real-time cursor position and button states, useful for verifying PS/2/USB mouse support on hardware (not on the default floppy) |
| **Hello** | 3KB | Minimal windowed app — creates a window, draws "Hello, UnoDOS!", waits for ESC. Serves as a template for new app development |
| **Runner3D** | 2KB | Native 3D demo ("UnoDOS Runner") — draws a solid 3D corridor through the kernel's `INT 0x80` graphics API in painter's order (no FPU, no framebuffer poking); the x86 counterpart to the [Uno3D](docs/UNO3D.md) library's PS2/Dreamcast hardware backends |

## Target Hardware

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| CPU | Intel 8088 @ 4.77 MHz | 80286+ |
| RAM | 256 KB (desktop + one app) | 640 KB (full 5-app multitasking) |
| Display | CGA | VGA |
| Storage | 3.5" 1.44MB floppy | Hard drive / CF card |
| Input | PC/XT keyboard + Microsoft serial mouse | + PS/2 mouse |
| Audio | PC speaker (optional) | |

### Tested Hardware
- **HP Omnibook 600C** (486DX4-75, VGA, 1.44MB floppy, PCMCIA CF card)
- **IBM PS/2 L40** (386SX, B&W LCD, 1.44MB floppy)
- **ASUS Eee PC 1004** (Intel Atom, USB boot)
- **QEMU** (PC emulation)

## Memory Layout

```
0x0000:0000   IVT + BIOS Data Area           ~1.25 KB
0x0000:7C00   Boot Sector (temporary)         512 B
0x0800:0000   Stage 2 Loader                  2 KB
0x1000:0000   Kernel                          52 KB area (104 sectors, may grow to 64 KB)
0x2000:0000   Shell/Launcher (fixed)          64 KB
0x3000:0000   User app slot 0 (dynamic)       64 KB
0x4000:0000   User app slot 1                 64 KB
0x5000:0000   User app slot 2                 64 KB
0x6000:0000   User app slot 3                 64 KB
0x7000:0000   User app slot 4                 64 KB
0x8000:0000   Heap (malloc pool)              60 KB
0x9000:0000   Scratch (clipboard + dialogs)   64 KB
0xA000:0000   VGA/VESA video memory           64 KB
0xB800:0000   CGA video memory                16 KB
```

Total: 6 app segments (1 shell + 5 user) using 384KB of the 640KB real-mode address space.

## Building

### Requirements

**Linux (Ubuntu/Debian):**
```bash
sudo apt install nasm qemu-system-x86 make python3
```

### Build Commands

```bash
# Build 1.44MB floppy image (OS + all apps)
make floppy144

# Build app-only launcher floppy
make apps && make build/launcher-floppy.img

# Build 64MB hard drive image (FAT16)
make hd-image

# Run in QEMU (floppy)
make run144

# Run in QEMU (hard drive)
make run-hd

# Clean build artifacts
make clean
```

### Build Output

| File | Description |
|------|-------------|
| `build/unodos-144.img` | 1.44MB boot floppy (FAT12, OS + apps) |
| `build/launcher-floppy.img` | Apps-only floppy image |
| `build/unodos-hd.img` | 64MB bootable hard drive image (FAT16) |
| `build/*.bin` | Individual compiled binaries |

### Pre-built Images

Pre-built disk images are included in the `build/` directory for users who can't build from source. Just clone the repo and write the image to media.

## Writing to Physical Media

### Windows (PowerShell, Run as Administrator)

```powershell
# Interactive mode — select image and target drive:
.\tools\write.ps1

# Quick floppy write:
.\tools\write.ps1 -DriveLetter A

# Quick HD/CF/USB write:
.\tools\write.ps1 -DiskNumber 2
```

The write tool auto-detects available drives, excludes system drives for safety, and includes optional read-back verification (`-Verify` flag).

### Linux

```bash
# Floppy
sudo dd if=build/unodos-144.img of=/dev/fd0 bs=512

# USB/CF (replace sdX with your device)
sudo dd if=build/unodos-hd.img of=/dev/sdX bs=512
```

## Project Structure

```
unodos/
├── apps/                    # Applications (19 NASM source files)
│   ├── launcher.asm         # Desktop launcher / shell
│   ├── notepad.asm          # Text editor
│   ├── browser.asm          # File manager
│   ├── tetris.asm           # Dostris (CGA)
│   ├── tetrisv.asm          # Dostris VGA
│   ├── outlast.asm          # OutLast driving game (CGA)
│   ├── outlastv.asm         # OutLast VGA
│   ├── pacman.asm           # Pac-Man (CGA)
│   ├── pacmanv.asm          # Pac-Man VGA
│   ├── clock.asm            # Clock
│   ├── music.asm            # Music player
│   ├── tracker.asm          # Pattern music editor
│   ├── paint.asm            # MacPaint-style bitmap editor
│   ├── settings.asm         # System settings
│   ├── mkboot.asm           # Boot floppy creator
│   ├── sysinfo.asm          # System info
│   ├── mouse_test.asm       # Mouse diagnostic
│   ├── runner3d.asm         # Native 3D demo (INT 0x80 graphics)
│   └── hello.asm            # Hello World
├── boot/                    # Boot chain
│   ├── boot.asm             # Floppy boot sector (512 bytes)
│   ├── stage2.asm           # Floppy stage 2 loader
│   ├── mbr.asm              # Hard drive MBR
│   ├── vbr.asm              # Hard drive VBR
│   └── stage2_hd.asm        # HD stage 2 loader
├── kernel/
│   ├── kernel.asm           # Main OS kernel (~52KB compiled)
│   ├── font4x6.asm          # 4x6 small font
│   ├── font8x8.asm          # 8x8 default font
│   └── font8x12.asm         # 8x14 large font
├── build/                   # Compiled binaries and disk images
├── docs/                    # Technical documentation
├── tools/                   # Build and deployment scripts
├── Makefile
├── CHANGELOG.md             # Version history (425 builds)
├── CONTRIBUTING.md          # Contribution guidelines
├── LICENSE                  # CC BY-NC 4.0
└── TODO.md                  # Roadmap
```

## Documentation

- **[App Development Guide](docs/APP_DEVELOPMENT.md)** — How to write applications for UnoDOS (with complete working example)
- **[API Reference](docs/API_REFERENCE.md)** — Complete system call reference (106 functions with register-level detail)
- **[Architecture](docs/ARCHITECTURE.md)** — Boot process, physical memory map, kernel layout, segment pool architecture, CGA video format
- **[Feature Matrix](docs/FEATURE-MATRIX.md)** — Cross-port feature comparison across every target
- **[Storage](docs/STORAGE.md)** — Per-platform filesystem and persistence architecture
- **[Boot Debug Messages](docs/boot-debug-messages.md)** — Diagnostic output reference for hardware troubleshooting
- **[Bootloader Architecture](docs/bootloader-architecture.md)** — Floppy and HD boot chain details
- **[Changelog](CHANGELOG.md)** — Full version history spanning 425 builds

## Writing Your Own App

UnoDOS apps are flat `.BIN` binaries assembled with NASM. Each app runs in its own 64KB segment and communicates with the kernel through `INT 0x80` system calls. Here's a minimal windowed app:

```asm
[BITS 16]
[ORG 0x0000]

; 80-byte icon header (JMP + magic + name + 16x16 bitmap)
    db 0xEB, 0x4E                   ; JMP to code entry at 0x50
    db 'UI'                         ; Magic identifier
    db 'MyApp', 0                   ; Display name (12 bytes max)
    times (0x04 + 12) - ($ - $$) db 0
    times 64 db 0xFF               ; 16x16 icon (placeholder: solid white)
    times 0x50 - ($ - $$) db 0

; Code entry (offset 0x50)
entry:
    ; Save registers (8086-safe: PUSHA is a 186+ instruction and silently
    ; executes as JO on an 8088 - see kernel/cpu8086.inc for macros)
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    mov ax, cs
    mov ds, ax                      ; DS = our segment

    ; Create a window (200x60 at position 60,60)
    mov bx, 60
    mov cx, 60
    mov dx, 200
    mov si, 60
    mov ax, cs
    mov es, ax
    mov di, title
    mov al, 0x03                    ; Title bar + border
    mov ah, 20                      ; win_create
    int 0x80
    jc .exit                        ; CF=1 = no free window slots

    mov ah, 31                      ; win_begin_draw (window-relative coords)
    int 0x80

    ; Draw text at (10,10) inside the window
    mov bx, 10
    mov cx, 10
    mov si, msg
    mov ah, 4                       ; gfx_draw_string
    int 0x80

    ; Event loop — wait for ESC
.loop:
    sti                             ; Re-enable interrupts (INT clears IF)
    mov ah, 9                       ; event_get (non-blocking)
    int 0x80
    jc .loop                        ; No event, keep polling
    cmp al, 1                       ; KEY_PRESS?
    jne .loop
    cmp dl, 27                      ; ESC?
    jne .loop

.exit:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    xor ax, ax
    retf                            ; Return to kernel

title: db 'MyApp', 0
msg:   db 'Hello, UnoDOS!', 0
```

Assemble with `nasm -f bin -o MYAPP.BIN app.asm`, place in the FAT filesystem, and it appears on the desktop with its icon. See the [App Development Guide](docs/APP_DEVELOPMENT.md) for the full tutorial covering windows, events, mouse input, file I/O, widgets, fonts, and audio.

## Architecture Overview

```
┌──────────────────────────────────────────────┐
│              Applications (19)               │
│   5 concurrent user apps + launcher shell    │
│   Each in its own 64KB segment (ORG 0x0000)  │
├──────────────────────────────────────────────┤
│           INT 0x80 System Calls              │
│   106 API functions, bitmap-based dispatch   │
│   Auto-translate coords, cursor protection   │
├──────────────────────────────────────────────┤
│                  Kernel                      │
│                                              │
│  Window Manager    │  Graphics (4 modes)     │
│  16 windows, drag  │  3 fonts, sprites      │
│  resize, z-order   │  lines, fill, blit     │
│                    │                         │
│  Cooperative       │  FAT12 + FAT16         │
│  Scheduler         │  Read/Write/Create     │
│  6 app slots       │  Delete/Rename/Seek    │
│                    │                         │
│  GUI Toolkit       │  Event System          │
│  15 widget types   │  32-entry queue        │
│  File dialogs      │  Per-task filtering    │
│                    │                         │
│  PS/2 Mouse        │  Keyboard (INT 09h)    │
│  BIOS + KBC        │  Modifier tracking     │
│  XOR cursor        │  Focus-aware delivery  │
│                    │                         │
│  Clipboard (4KB)   │  PC Speaker audio      │
│  Theme system      │  Settings persistence  │
├──────────────────────────────────────────────┤
│              BIOS Services                   │
│  INT 13h (disk), INT 15h (mouse),           │
│  INT 10h (video), INT 1Ah (RTC)             │
├──────────────────────────────────────────────┤
│             x86 Hardware                     │
│   8088 / 8086 / 286 / 386 / 486             │
│   Real Mode (16-bit)                         │
└──────────────────────────────────────────────┘
```

## Version History

See [CHANGELOG.md](CHANGELOG.md) for the full history spanning 425 builds.

Current version: **v3.32.0** (Build 425)

## License

Copyright (c) 2026 Arin Bakht

This project is licensed under the [Creative Commons Attribution-NonCommercial 4.0 International License](LICENSE) (CC BY-NC 4.0).

- **Modification**: Allowed
- **Attribution**: Required — credit the original author and link to this repository
- **Commercial use**: Not permitted

---

*UnoDOS 3 — Because sometimes the old ways are the best ways.*
