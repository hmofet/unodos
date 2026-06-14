# UnoDOS new-ports program: Apple II, Apple IIGS, SNES, Sony PS2

Direction (2026-06-12): bring UnoDOS to four new platforms, in this order.
The order is chosen so each port feeds the next: the Apple II establishes
the 6502 toolchain and the keyboard-driven input adaptation; the IIGS
introduces the 65816 toolchain that the SNES reuses; the SNES reuses the
Genesis architecture nearly verbatim; the PS2 reuses the portable C core
from the hosted Mac port. Every port follows the house method: ROM-free
scripted harness first, real emulator second, real hardware last, with
committed + regression-tested milestones (the macplus M1->M3 shape).

Parity definition per port = the 11 shared apps (SysInfo, Clock, Files,
Notepad, Dostris, OutLast, Pac-Man, Paint, Music, Tracker, Theme) plus
storage, sound, and the cooperative scheduler — adapted to the platform's
real envelope (documented deviations, like macplus's 1-bit gamut, rather
than fake parity).

The cross-platform chrome-themes work (color ports + Windows XP style)
remains queued from the earlier directive and slots naturally after
Apple II M1, since it touches no new-port code.

---

## 1. Apple II (in progress)

**Envelope.** 6502 @ 1 MHz, 48-64 KB RAM, hi-res 280x192 effectively
1-bit (7 px/byte, LSB-left, interleaved rows), keyboard only as standard
input (no timer interrupts, no vblank IRQ), 1-bit speaker click at $C030,
Disk II 140 KB GCR floppy with NO firmware sector services — the boot ROM
loads exactly one sector; everything past that is our own RWTS.

**The honest constraint.** At 1 MHz with a software-only renderer this is
"UnoDOS Lite": byte-aligned (7 px) window columns, full repaints take
visible fractions of a second, and the M1 desktop is keyboard-driven
(arrows/Return/Tab/ESC). It is still a real OS booting from its own disk.

**Boot strategy.** T0S0 byte-0 autoload protocol (the 16-sector P5A ROM
loads all 16 track-0 sectors to $0800-$17FF, then jumps $0801) → our
read-RWTS (GCR 6-and-2 denibble + head stepping) loads the kernel from
tracks 1+ to $4000 → jmp. Risk: if a clone ROM only honors 1 sector, fall
back to replicating the ROM read loop inside boot0 — first thing to check
in AppleWin.

**Toolchain & rigs.** dasm 2.20 (acquired, C:\Users\arin\apple2-tools) +
py65 (installed) for the ROM-free harness (plays the boot firmware,
keyboard softswitch, hi-res de-interleave → PNG, scripted input — the
macplus harness pattern). AppleWin for real-emulator validation (its
cycle-honest Disk II emulation is what proves the RWTS before metal).
Real hardware: the user's FloppyEmu does Disk II emulation.

**Milestones.**
- M1 (DONE): boot chain + RWTS-read, hi-res desktop + menu bar, window
  manager (open/raise/close, keyboard-driven), SysInfo (machine detect via
  $FBB3/$FBC0) + Clock (calibrated soft tick — no timer hardware), 7-px
  shared font, ROM-free py65 harness + tests/m1.script (4 screenshots,
  verified). TICK_INSTRS=5000 / KEY_INSTRS=30000 calibration documented in
  apple2/README.md. Real-hardware (AppleWin/FloppyEmu) pass still pending.
- M2 (DONE): RWTS write path (rwts.i, GCR 6-and-2 encoder); a track/sector
  mini-FS (USV1-style catalog on tracks 20-34, FS_SECTORS=240 — FAT12
  doesn't fit GCR sector space sensibly); Files + Notepad (Ctrl-S save);
  speaker beeps (beep_click on launch/save). Paddle/joystick pointer
  option not implemented (flagged optional in HANDOFF-M2). ROM-free harness
  extended with a write path + `--writeback`; tests/m2.script (5
  screenshots, RWTS+FS self-test asserts PASS, beep counter > 0) and
  tests/m2_persist.script (2 screenshots, verifies an edit survives a
  simulated power cycle via `--writeback` + re-boot) both verified.
  Real-hardware write-timing pass (AppleWin) still pending — see
  apple2/README.md's RWTS write-timing caveat.
- M3 (DONE): the scaled app roster on a 10-icon / 3-row desktop. Theme
  (theme.i, 6 dither presets over a mutable pat_tab), Dostris (dostris.i),
  Pac-Man (pacman.i — the 1 MHz adaptation: 13x13 maze, two Manhattan-steer
  ghosts, tile-stepped 7px actors), Music (music.i — Canon in D, blocking
  square-wave staff player) and Tracker (tracker.i — shared 32x4 pattern
  format, single-voice leftmost-channel playback, SONG.UNO save/load), Paint
  (paint.i — MacPaint-style on 32x34 byte-aligned fat-pixel cells, four
  dither inks, keyboard cursor, PAINT.UNO save/load). Note tables via
  mknotes.py → build/notes7.s. tests/m3.script + per-app scripts (sound apps
  assert beep>0; Tracker/Paint assert FS round trips), all harness-verified.
  **OutLast feasibility — SHIPS marginal:** the cheapest honest variant
  (28-band half-vertical-res road raster) measured ~75k instr/frame ≈ ~4 fps
  at 1 MHz — just under the 5 fps bar but with responsive steering; ships as
  a playable prototype, dirty-band repaint identified as the >5 fps path
  (outlast.i). **Scheduler — option 1 PROVEN, ships option 3:** the
  stack-partitioning prototype (scheduler.i, ./build.sh sched) ran 40
  cooperative context switches with both slice canaries intact, so option 1
  is feasible; but the shipping kernel keeps option 3 (poll-and-dispatch)
  because the full-screen single-app model needs no live scheduler. Real-hw
  (AppleWin/FloppyEmu) pass still pending, as for M1/M2.
- Real-hw validation: AppleWin first (boot + RWTS + input), then FloppyEmu
  on a real machine. AppleMouse II card support = stretch/backlog.

## 2. Apple IIGS

**Envelope.** 65C816 @ 2.8 MHz, 256 KB-8 MB RAM, Super Hi-Res 320x200
with 16 colors/scanline from 4096 (or 640x200), ADB keyboard + mouse with
FIRMWARE support, Ensoniq DOC 32-oscillator wavetable audio (the richest
sound chip in the whole UnoDOS family), 800 KB 3.5" disks behind
SmartPort firmware BLOCK services — a true ".Sony equivalent".

**Why second.** It is the macplus story replayed on a new CPU: firmware
bootstraps us (block 0 → $800), firmware gives us block I/O and
ADB-maintained input state to mirror — our proven "ROM-assisted" model.
And it forces the 65816 toolchain into existence for the SNES.

**Boot strategy.** ProDOS-style block boot: firmware loads block 0, our
boot stage reads the kernel via SmartPort block calls (firmware entry in
the slot ROM), kernel takes over in native 65816 mode with shadowed SHR.

**Toolchain & rigs.** ca65/ld65 (cc65 suite — also gives the Apple II a
second assembler option); harness strategy decided at M0: either a Python
65816 core (py65816 if viable, else extend py65) or lean directly on a
scriptable emulator (GSplus/KEGS) the way Genesis leans on BlastEm —
GSplus has debugger scripting; screenshot via the existing snapscreen
rig. Real hardware: FloppyEmu supports SmartPort/800K for the IIGS.

**Milestones.**
- M0: toolchain + boot PoC (block boot → SHR splash) + harness decision.
- M1: SHR 320x200x16 desktop + WM + ADB mouse/keyboard (firmware-state
  mirroring) + SysInfo/Clock. Real pointer from day one.
- M2: storage — FAT12 inside the 800 KB block space (the fat12 layout is
  CPU-portable; write a 65816 core mirroring the 68K one) + Files/Notepad
  + disk-loaded apps (the macplus ksys-table ABI, 65816 flavor).
- M3: parity — full-color games/Paint/Theme (16-color palettes!), Ensoniq
  audio engine (Music/Tracker map to real wavetable voices), scheduler.

## 3. SNES

**Envelope.** 65C816 @ 3.58 MHz (same CPU as IIGS), 128 KB WRAM, tile/
sprite PPU (Mode 1: two 16-color BG layers + sprites), SPC700 audio
coprocessor (own 64 KB RAM + 8-voice DSP — a driver must be UPLOADED to
it), controllers + the SNES Mouse (widely supported), battery SRAM on
cartridge (8-32 KB), boots from cartridge ROM (LoROM) on a flashcart.

**Why third.** It is the Genesis port's twin: cell-based desktop on BG
tiles, sprite cursor, pad-as-pointer + soft keyboard, SRAM mini-FS —
genesis/kernel.asm, scheduler.i, and the USV1 FS are the direct templates,
re-expressed in 65816 (toolchain already standing from the IIGS).

**Milestones.**
- M0: ca65 LoROM skeleton boots in Mesen2 (the BlastEm-role emulator —
  Lua scripting + screenshots for the automated rig) — splash + joypad.
- M1: tile desktop + WM + sprite cursor + pad-as-pointer + soft keyboard
  (Genesis input model verbatim) + SNES Mouse support + SysInfo/Clock.
- M2: SRAM storage (USV1 port) + Files/Notepad; games (Dostris/Pac-Man/
  OutLast — the PPU makes these EASIER than Genesis).
- M3: SPC700 driver (uploaded engine + mailbox protocol — the hardest
  novel piece on this port) for Music/Tracker/game audio; Theme over
  CGRAM palettes; scheduler.
- Real hardware: flashcart (confirm which one the user owns), SNES Mouse
  if available.

## 4. Sony PS2 (FreeMcBoot)

**Envelope.** MIPS R5900 EE @ 295 MHz, 32 MB RAM, GS framebuffer
(512x448), IOP coprocessor runs I/O modules (IRX), USB keyboard + mouse
possible, DualShock 2, 8 MB memory cards with a file API, SPU2 audio.
FreeMcBoot launches a homebrew ELF from the memory card — so UnoDOS/PS2
is an ELF with full hardware access. Practically "firmware-hosted
bare-metal": the richest target in the family by orders of magnitude.

**Strategy: port the C core.** mac/unodos.c already factors UnoDOS as
portable C over a platform layer (QuickDraw there). The PS2 layer:
gsKit (or direct GIF packets) for the framebuffer, padlib + ps2kbd/usbd
IRX modules for input (pad-as-pointer + soft keyboard as the always-works
path, USB kbd/mouse when plugged), mcman/mcserv for memory-card files
(libmc), audsrv for sound, newlib via PS2SDK.

**Toolchain & rigs.** PS2SDK (ps2dev) — prebuilt toolchain via the ps2dev
Docker image or Windows release binaries (decide at M0; Docker is already
proven on this machine per the control-repo fixtures). PCSX2 boots ELFs
directly (no FMCB needed in the emulator) = the validation rig, with the
existing screenshot automation. Real hardware: PS2 + FMCB card; ELF on MC
or USB stick.

**Milestones.**
- M0: PS2SDK builds a hello-GS ELF that runs in PCSX2; decide gsKit vs
  raw GIF; module loading (sio2man/padman/mcman) proven.
- M1: framebuffer desktop + WM + pad-as-pointer + soft keyboard +
  SysInfo/Clock — mostly the C core with a new platform layer.
- M2: memory-card storage (file API — trivial next to GCR floppies),
  USB keyboard/mouse modules, Files/Notepad, disk(card)-loaded apps.
- M3: parity — full-color everything, audsrv Music/Tracker, Theme with
  true 32-bit color, scheduler (or real threads — the EE kernel has them;
  decide whether authenticity or capability wins).
- Real hardware: FMCB memory card; document the BOOT.ELF install path.

---

## Implementation handoffs (build-level companions to this plan)

Each port has a Sonnet-ready handoff capturing the contracts, file-by-
file work, reference map and risks. Read the relevant one before
touching code; update it (and this plan) when a milestone closes.

- Apple II: [../apple2/HANDOFF.md](../apple2/HANDOFF.md) (M1 done),
  [HANDOFF-M2.md](../apple2/HANDOFF-M2.md),
  [HANDOFF-M3.md](../apple2/HANDOFF-M3.md)
- Apple IIGS: [../iigs/HANDOFF.md](../iigs/HANDOFF.md) (M0–M3 phased)
- SNES: [../snes/HANDOFF.md](../snes/HANDOFF.md) (M0–M3 phased)
- PS2: [../ps2/HANDOFF.md](../ps2/HANDOFF.md) (M0–M3 phased)

## Sequencing & checkpoints

1. **Apple II M1-M3** — DONE (harness-verified); AppleWin/FloppyEmu real-hw
   pass still pending.
2. **Chrome themes** (queued directive) — natural slot while Apple II
   real-hw feedback is pending; touches only existing color ports.
3. **IIGS M0-M1** next (Apple II M3 complete).
4. **IIGS M2-M3**, then **SNES M0-M3** (toolchain shared).
5. **PS2 M0-M3** (independent toolchain; can start anytime if priorities
   shift — nothing upstream feeds it except the C core, which is done).

Each milestone lands as: code + harness/emulator regression script +
screenshots + commit + README/TODO updates — same bar as macplus.

## Open questions (not blocking; answer when convenient)

- SNES: which flashcart is on hand for real-hardware runs? Is there a
  SNES Mouse?
- PS2: USB keyboard/mouse available? Memory card vs USB stick as the
  primary storage story?
- Apple II: which machine will FloppyEmu target (II+/IIe/IIc)? IIe makes
  up/down arrows + 80-col/aux memory available; II+ is the floor.
