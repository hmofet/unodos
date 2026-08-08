# Platform status

Every target UnoDOS builds for, what has actually been verified, and where it
has not. This file is the claims ledger: if something is stated in the README,
on the website or in a talk, it should be checkable here first.

**The honest headline: 22 platforms build and ship a binary. Six are verified
booting on real hardware without caveats. Two pass with real, named limitations.
One is confirmed NOT to boot on its hardware. The rest are verified only in
emulators or instruction-level harnesses.**

"Emulator-verified" and "runs on the real machine" are different claims, and the
distance between them is where operating systems actually break. They are kept
in separate columns here for that reason.

Last reconciled: 2026-08-08, against release
[v3.32.0](https://github.com/hmofet/unodos/releases/tag/v3.32.0). Every binary
in that release was built from this tree in one pass on that date.

## Legend

| | Meaning |
|---|---|
| ✅ | Verified on the physical machine, no known caveats |
| ⚠️ | Runs on the physical machine with stated limitations |
| ❌ | Confirmed **not** working on the physical machine |
| ⏳ | Not yet attempted on the physical machine |

App parity is out of the 11-app reference set (SysInfo, Clock, Notepad, Files,
Settings, Dostris, Tracker, OutLast, Pac-Man, Paint, Browser/Theme depending on
the port's profile).

---

## Modern PC

| Platform | CPU | Emulator / harness | Real hardware | Parity | Known gaps |
|---|---|---|---|---|---|
| **Modern PC (pc64)** | x86-64 | QEMU + OVMF (`harness.py`, `nettest.py`) | ✅ Lenovo ThinkPad X1 Carbon Gen 8; ZimaBlade (boots from USB, runs detached from firmware) | full + net/TLS/browser/3D | Wi-Fi firmware not redistributable, so published images ship without it (see below). AMD/SVM hypervisor backend written but never completed a VMRUN. |

## Home computers

| Platform | CPU | Emulator / harness | Real hardware | Parity | Known gaps |
|---|---|---|---|---|---|
| **IBM PC / XT and later** | 8086/8088+ | QEMU, scripted scenarios | ✅ tested across 8088→486, incl. PS/2 L40 and an Eee PC | reference implementation | The 8088 cycle-accurate build below is tracked separately. |
| **IBM PC/XT (8088 fidelity build)** | 8088 | MartyPC + GLaBIOS, cycle-accurate | ⏳ physical XT pending | M0-M2 done, M3 mostly | Never run on a real XT. |
| **Commodore 64** | 6510 | Custom harness; VICE (`x64sc`) | ⏳ pending | 11 of 11 | Harness-verified only. Untested via SD2IEC or 1541 Ultimate. |
| **Commodore VIC-20** | 6502 | py65 ROM-free harness | ⏳ pending | 7 of 11 | Tracker, OutLast, Pac-Man and Paint are launcher placeholders. |
| **Commodore Amiga** | 68000 | WinUAE + AROS ROM (AUTOTEST builds) | ⏳ A500 smoke test pending | M3+ | Never run on real Amiga hardware. |
| **Apple II** | 6502 | py65 ROM-free harness; `.woz`/`.nib` built | ⏳ AppleWin / Floppy Emu (IIc) pending | M1-M3 | Reduced "UnoDOS Lite" profile: a 1 MHz 6502 with 1-bit software graphics cannot carry the full desktop. |
| **Apple IIgs** | 65C816 | From-scratch py65816 core, 9 suites green | ⏳ GSplus / KEGS / MAME pending | full | Audio never verified by ear. |
| **Macintosh Plus (bare-metal OS)** | 68000 | Unicorn harness; Mini vMac | ✅ real Macintosh SE via Floppy Emu | full | — |
| **Macintosh System 7 (hosted)** | 68K | Executor (ROM-free) | ⏳ Mac II-class pending | M3 | — |
| **Macintosh System 1-6 (hosted)** | 68K | Executor | ⏳ Mac Plus pending | M3 minus colour Theme | **No binary in v3.32.0.** The Retro68 toolchain on the build machine is incomplete; source builds once Retro68 is installed. |
| **PowerPC Macintosh** | PowerPC 32 | Unicorn PPC32 big-endian core, Open Firmware client | ⏳ real Mac pending | 11 of 11 | Native ADB input and codec audio delivery unproven. |

## Consoles

| Platform | CPU | Emulator / harness | Real hardware | Parity | Known gaps |
|---|---|---|---|---|---|
| **Nintendo NES / Famicom** | 6502 (2A03) | Headless py65 PPU harness; Mesen2 | ✅ real AV Famicom (2A03 APU and `$4016` input confirmed) | 11 of 11 | Hardware run covered the base 7 apps plus Dostris. The four parity apps added later are emulator-verified only; hardware re-test pending. |
| **Super Nintendo** | 65816 | Mesen2 captures | ⚠️ conditional pass on a SupaBoy clone with an FXPak Pro: boots and is navigable, but **icons render text-only and there is no audio** | full | Never tested on an authentic SNES. The two faults may be clone-specific or real. |
| **Sega Master System** | Z80 | BlastEm, AUTOTEST scripted-pad | ⏳ pending | 11 of 11 | Audio never verified by ear. |
| **Sega Genesis / Mega Drive** | 68000 + Z80 | BlastEm, 15 AUTOTEST builds | ⚠️ boots from a flashcart (2026-06-12); PS/2, tape and Sega CD adapters **not** exercised | M6+ | The peripheral support is emulator-only. |
| **Sega Dreamcast** | SH-4 | Flycast at 60fps; VMU round-trip | ⏳ CD-R / dc-tool pending | full | Audio never verified by ear. |
| **Sony PlayStation 2** | MIPS R5900 | PCSX2, boots at 60fps | ⏳ real PS2 pending | full | USB and audio are written but cannot be exercised in the emulator. |
| **NEC PC Engine / TurboGrafx-16** | HuC6280 | Mednafen savestate/framebuffer | ✅ real Turbo EverDrive v2.5: boots, input and sound confirmed | 7 of 11 | Boots **under TEOS only**. The stock Krikzz v2 OS rejects the ROM with "Error 32"; stock-OS compatibility is unresolved. Four apps are launcher placeholders. |

## Handhelds

| Platform | CPU | Emulator / harness | Real hardware | Parity | Known gaps |
|---|---|---|---|---|---|
| **Game Boy / Game Boy Color** | Sharp SM83 | Mesen2/GBC, AUTOTEST scripted-pad | ⏳ real DMG/GBC pending | 7 of 11 | Four apps are launcher placeholders. |
| **Game Boy Advance** | ARM7TDMI | Unicorn ARM7TDMI ROM-free harness | ⏳ real GBA pending | 11 of 11 | Audio never verified by ear. |
| **Sega Game Gear** | Z80 | Mesen2/GG, AUTOTEST scripted-pad | ⏳ real GG pending | 7 of 11 | Four apps are launcher placeholders. |
| **Bandai WonderSwan** | NEC V30MZ | Unicorn V30MZ ROM-free harness | ⏳ pending | 7 of 11 | Four apps are launcher placeholders. Audio never verified by ear. |

## Single-board and mobile

| Platform | CPU | Emulator / harness | Real hardware | Parity | Known gaps |
|---|---|---|---|---|---|
| **Raspberry Pi** | ARM Cortex-A (AArch64) | Unicorn AArch64 core; PWM→WAV reconstruction | ✅ boots to the desktop on a **real Pi 3** (2026-06-17) | 11 of 11 | Two open faults on hardware: the background renders **brown** (an XRGB/BGR pixel-order swap), and input is **serial-only** (no USB HID). |
| **PinePhone** | Allwinner A64 | Unicorn AArch64 core; DE2 sink, I2S PCM→WAV | ❌ **does not boot on a real PinePhone** (2026-06-17) | 11 of 11 | Unresolved. Prime suspects are the U-Boot `go` cache-coherency caveat and DE2/DSI display bring-up. Needs a serial-console debugging pass. The published image is emulator-verified only and **should be treated as untested on hardware.** |

---

## Wi-Fi firmware

Published images ship **without** Intel wireless firmware. UnoDOS drives those
adapters, but the firmware is Intel's and is not ours to redistribute, so every
released image is built `UNO_NOFW=1`.

This is enforced, not remembered: `tools/mkrelease.sh` refuses to stage a
release if `pc64/build/esp` contains any firmware blob. That gate has caught a
contaminated tree twice, because `build/esp` is populated incrementally and a
default build leaves firmware behind for the next one to inherit.

Wired Ethernet works without any of this, and so does a virtual machine's
emulated NIC. To enable Wi-Fi on real hardware, use
`unodos-wifi-firmware-tool.zip` from the release.

## How to check any of this yourself

Every row above corresponds to a downloadable binary in
[v3.32.0](https://github.com/hmofet/unodos/releases/tag/v3.32.0), and the
emulator named in each row is the one that was used. Load the file and see. If
something here is wrong, that is a bug in this file and worth reporting.

## Where this file comes from

Reconciled from `docs/FEATURE-MATRIX.md` (the per-port verification table,
which is the primary record), `docs/PARITY-HANDOFF.md` and `CLAUDE.md` (which
corrects the matrix's stale parity counts for the fresh 3.1 ports), the
per-port `README.md` files, and a full build of every port on 2026-08-08.

Known limitations of this reconciliation, stated so nobody over-trusts it:

- `docs/FEATURE-MATRIX.md` was last updated 2026-07-17 and has no C64 row; the
  C64 entry above is taken from `c64/README.md`, which records it as
  harness-verified only.
- Hardware dates are as recorded at the time of the test. Nothing above was
  re-tested on physical hardware for this release; the binaries were rebuilt,
  and a rebuilt binary is not a re-verified one.
