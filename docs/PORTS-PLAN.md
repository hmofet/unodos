# UnoDOS new-ports program: Apple II, Apple IIGS, SNES, Sony PS2, Sega Dreamcast

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

## 2. Apple IIGS — ✅ FULL APP PARITY (M0–M3, 2026-06-15, build 420)

**Shipped.** All four milestones done and verified headlessly: the SHR
desktop + window manager, ADB mouse/keyboard + save-under software cursor,
FAT12-over-SmartPort with persistent Files/Notepad, and **all 11 apps** —
Theme (4096-colour), Music + Tracker (Ensoniq DOC), Dostris, Pac-Man,
OutLast, Paint, plus SysInfo/Clock/Files/Notepad — over a cooperative
tick-scheduler. **The M0 harness wildcard was resolved by writing a
from-scratch Python 65C816 core** (`iigs/cpu65816.py`, self-testing) +
ROM-free firmware shim (`iigs/harness.py`: block-0 autoload, ProDOS driver
via a WDM trap, `$C0xx`/DOC register model, SHR→PNG); no IIGS ROM or
emulator needed, fully CI-able. 9 regression suites green. The 65816
toolchain it stands up is reused by the SNES port. Detail + the reusable
app pattern + traps in [../iigs/HANDOFF.md](../iigs/HANDOFF.md).
**Remaining:** real-hardware validation (GSplus/KEGS/MAME by hand, then
FloppyEmu SmartPort 3.5″) + audio-by-ear (DOC sound isn't harness-
reproducible; the oscillator-programming path is asserted via the DOC
register log).


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

## 3. SNES (in progress)

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
- M0 (DONE): ca65 LoROM skeleton boots in Mesen2 to the "UnoDOS 3" tile
  splash (shared 8x8 font → SNES 4bpp planar tiles + BGR555 palette via
  snes/mkdata.py) and reacts to the joypad — auto-joypad read in the NMI,
  rendered live as `PAD:xxxx`. The shadow+DMA architecture (HANDOFF SS2) is
  in from line one: the main loop writes a WRAM tilemap shadow, the vblank
  NMI DMAs it to VRAM and samples input. Build = cc65 (snes/build.sh →
  LoROM .sfc, checksum-patched). Rig: Mesen2 forced to its **software
  renderer** (PrintWindow can't grab the GPU surface on this headless
  desktop — the Genesis "software-under-RDP" lesson) + PrintWindow capture
  (snes/run_mesen.ps1); input verified by the **AUTOTEST** self-injecting
  build (synthetic joypad in the NMI), the Genesis fallback pattern, since
  Mesen's CLI does not autoload Lua. Verified in Mesen2 (PAD:0000
  interactive, PAD:C0A0 AUTOTEST). See snes/README.md.
- M1 (DONE): tile desktop + WM (z-order/drag/chrome) + OAM cursor +
  pad-as-pointer + 32-cell soft keyboard + SysInfo/Clock (live 60 Hz clock),
  verified in Mesen2 via the F12 PPU-framebuffer screenshot (full desktop +
  cyan soft keyboard; VRAM also byte-correct by CPU read-back). SNES Mouse
  detection deferred to M2 backlog. (The capture rig now uses Mesen's own
  F12 screenshot — the GPU surface is black through PrintWindow on this
  headless host, and the software-renderer grab had a bottom-row palette
  artifact.)
- M2 (DONE): USV1 SRAM mini-FS at $70:0000 (byte-addressable) + Notepad
  (append editor, F1 save) + Files (list/open/delete) + all three shared
  games (Dostris, OutLast, Pac-Man), 7 desktop icons; each verified in
  Mesen2 (save -> directory -> listing round-trip; each game rendering and
  running). Deviations (in HANDOFF): OutLast uses a linear-perspective road
  (no 16/16 divide on the 65816); Pac-Man uses cell-grid BG-tile actors
  (the full ghost AI intact) rather than OAM sprites; Notepad is an append
  editor. Backlog: Notepad caret nav, OutLast HDMA raster, SNES-Mouse detect.
- M3 (DONE): SPC700 driver (a Python SPC700 assembler builds the uploaded
  engine + mailbox; IPL upload verified by ack) for Music (voice 0) and the
  Tracker (4 voices); Theme over CGRAM (palette shadow + NMI flush, since
  CGRAM is vblank-only); Paint as a per-pixel unique-tile canvas (dirty
  tiles DMA'd by the NMI). Scheduler = cooperative-by-ticks (verdict: the
  65816 bank-0 stack constraint leaves no room for per-task stacks; every
  app's *_tick runs from the main loop). Deviations in HANDOFF: square-wave-
  only audio, Tracker tone-not-noise 4th channel, Paint pencil-only/fixed
  palette, tick-model scheduler. Backlog: game-music hooks, DSP instruments,
  Paint tools + live colour, SNES-Mouse probe.
- Real hardware: flashcart (confirm which one the user owns), SNES Mouse
  if available; audio-by-ear pass.

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

**Milestones.** (M0–M2 DONE + verified on the emulated PS2 in PCSX2 as of
2026-06-14; USB keyboard + mouse and SPU2 audio added 2026-06-15; see
[../ps2/HANDOFF.md](../ps2/HANDOFF.md) and the ps2 CHANGELOG entries. All
features are in — only a real-hardware run + the USB/audio function check
remain, which need a physical PS2.)
- M0 (DONE — splash on the emulated GS): the
  software-framebuffer platform layer (`ps2/fb.c` — 640×448×32 + fill/frame/
  invert/text over the 4-colour gamut), the shared font as a C array
  (`mkfont_c.py`), and the hello-GS splash (`uno_splash.c`) all built +
  **screenshotted on the PC via the host shim** (`./build.sh host`, WSL gcc)
  — the verified inner loop the EE target shares verbatim. Design decided:
  software FB, GS as a blitter (gsKit), so gsKit-vs-raw-GIF is low-stakes.
  **Toolchain installed** (prebuilt ps2dev v2.0.0 under WSL; Docker was
  unavailable) and `./build.sh ee` links a real MIPS R5900 ELF
  (`build/unodos-ps2.elf`, gsKit/libpad). The EE ELF now **runs on the
  emulated GS** (PCSX2 v2.6.3 + a 4 MB PS2 BIOS; the earlier 512 KB dumps were
  PS1 BIOSes). Rig: `ps2/tools/run_pcsx2.ps1` (note the `SettingsVersion=1`
  ini gotcha).
- M1 (DONE — desktop on host + emulated GS): the C core `mac/unodos.c` ported
  to `ps2/unodos.c` over a Mac-compat shim (`mac_compat.*`/`mac_io.c`) — full
  desktop + WM + all 11 apps + pad-as-pointer, the host shim being the fast
  inner loop and the EE target (`ee_platform.c`) GS-presenting each vsync.
- M2 (DONE — memory-card storage + USB input): the EE File Manager persists
  Files/Notepad to the **PS2 memory card** via libmc, verified to survive a
  power cycle in PCSX2. (Trivial next to GCR floppies, as predicted.) **USB
  keyboard + mouse DONE** (`ps2/ee_usb.c`): the `usbd`/`ps2kbd`/`ps2mouse` IRX
  are embedded into the ELF via `bin2c` and loaded with `SifExecModuleBuffer`;
  keyboard in RAW HID mode (full US keymap incl. arrows/modifiers), mouse in ABS
  mode with a GS-overlay cursor — both feed the same shim event queue as the
  pad. Device init runs on a background EE thread so a USB-less boot (e.g. PCSX2,
  which has no USB HLE) still reaches the desktop; boot verified in PCSX2, the
  USB function itself is hardware-only to confirm.
- M3 (DONE): full 32-bit-colour Theme + the cooperative scheduler come along
  through the shim (scheduler kept cooperative, not EE threads); **SPU2 audio via
  audsrv** (`ps2/ee_audio.c`) realises the Sound Manager square-wave channels —
  `audsrv.irx` embedded with `bin2c`, `rom0:LIBSD` for the SPU2, an 8-voice
  phase-accumulator synth pumped per-frame from `SndDoImmediate`. Bring-up shares
  the USB I/O thread + SIF lock (the audsrv RPC init hangs under PCSX2 fastboot
  just like the HID inits). The sound itself awaits a hardware ear-check; boot is
  verified at 60 fps in PCSX2 with audsrv + USB loaded.
- Real hardware: FMCB memory card; document the BOOT.ELF install path.

---

## 5. IBM PC/XT 8088 (native build — hardware-fidelity, in progress)

Not a CPU rewrite: the x86 reference build already *is* the 8088 target. This
is the **real-hardware-fidelity milestone** for the native build — proving and
fixing UnoDOS on a genuine Intel 8088 / IBM PC-XT, following the same
real-emulator → real-hardware method. The 2026-06 audit's `cpu 8086` pass made
the floppy boot chain + kernel + apps 8086-clean, but it was only ever *run* on
QEMU (a 486-class CPU that hides all 8088/XT-specific behaviour).

**Rig.** MartyPC (cycle-accurate 8088, validated against real silicon) booting
open GLaBIOS — ROM-free, the house rule. See [PORT-8088.md](PORT-8088.md) and
[../tools/xt/README.md](../tools/xt/README.md).

**Milestones.**
- M0 (DONE 2026-06-14): the cycle-accurate XT rig + the primary
  `build/unodos-144.img` booting end-to-end on an emulated IBM PC/XT (8088 @
  4.77MHz, CGA) — boot chain → 104-sector kernel load → CGA desktop →
  keyboard-launched SysInfo (window manager + cooperative scheduler running on
  real 8088 silicon). First run outside a 486-class QEMU. Findings: the README
  "128KB minimum" is wrong (the launcher at 0x2000 alone needs ~192K; the full
  feature set with heap/clipboard at 0x8000/0x9000 needs 640K); the serial
  mouse is undriven (cursor static — AT-only INT 15h/C2 + KBC paths).
- M1: CGA-only reality, XT 8255 PPI keyboard ack verification, the RAM floor,
  PIT/timing; exercise the keyboard-driven WM + the full CGA app set.
- M2: serial mouse on COM1, real INT 13h floppy timing/retry, CGA snow; VGA
  apps documented as out-of-envelope on a CGA 5150/5160 (a real deviation).
- M3: the draw_char CGA row-blit fast path (~10× text at 4.77MHz), then a
  physical IBM PC/XT.

---

## 6. Sega Dreamcast (KallistiOS) — DONE, at parity (emulator-verified)

**Envelope.** Hitachi SH-4 @ 200 MHz, 16 MB RAM, PowerVR2 GPU, AICA
(ARM7) sound, VMU memory cards, maple bus (controller / keyboard /
mouse). KallistiOS (KOS) is a mature open SDK with newlib + a POSIX VFS;
homebrew boots from a self-made GD-ROM image (`.cdi`/`.gdi`). Like the PS2,
practically "firmware-hosted bare-metal".

**Strategy: reuse the PS2 port almost verbatim.** Same `mac/unodos.c`
core over the same Mac-compat shim (`mac_compat.*`, `mac_io.c`) and the
same software framebuffer (`fb.*`); only the present + input + storage
layers are swapped. The DC layer is one file, `dreamcast/dc_main.c`:
a 640×480 RGB565 framebuffer copied to `vram_s` each vblank (PVR idle —
"FB as the blitter"), maple input, and an AICA square-wave synth via
`snd_sfx`. Native 640×480 (vs the PS2's 640×448) since the core derives
geometry from `gScreen`.

**Toolchain & rigs.** KOS toolchain built from source under WSL
(`utils/kos-chain` → `sh-elf-gcc 15.2.0` + newlib + libkos); `build.sh
dc`/`cdi` link the SH-4 ELF and wrap it with **mkdcdisc**. Validation rig:
**Flycast** under Xvfb + Mesa llvmpipe (no GPU, REIOS HLE BIOS — no Sega
ROM), with the existing host-shim inner loop. Rig gotchas (see
`../dreamcast/README.md`): Flycast needs a real disc format (not a bare
`.iso`), `rend.EmulateFramebuffer=yes`, and `rend.vsync=no` (Xvfb's 0 Hz
otherwise gates frame swaps → black).

**Milestones — all DONE, emulator-verified** (see
[../dreamcast/HANDOFF.md](../dreamcast/HANDOFF.md)):
- M0/M1: software-FB splash + the full desktop / WM / all 11 apps, host-shim
  verified (`m1_*.png`) and **booted in Flycast at 640×480/60 fps**
  (`dc_*.png` — desktop, Pac-Man, Dostris, Tracker, Paint, Theme, Files,
  OutLast, the Music/Files/Notepad stack).
- M2 storage: VMU (`/vmu/a1`) via the KOS VFS with a flush-on-close buffer;
  the Notepad save→reload round-trip is captured in-emulator (`dc_vmu.png`).
- M3: 32-bit Theme + cooperative scheduler through the shim; **AICA audio**
  (looping square-wave `snd_sfx` pitched per MIDI note) for Music / Tracker /
  Dostris — boots with audio live, the sound an ear-check like the other ports.
- Real hardware (CD-R / dc-tool) is the only remaining step.

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
- PS2: [../ps2/HANDOFF.md](../ps2/HANDOFF.md) (M0–M3 done + verified in PCSX2;
  Theme/scheduler/USB/audio all in — real-hardware function check pending)
- Dreamcast: [../dreamcast/HANDOFF.md](../dreamcast/HANDOFF.md) (at parity, booted
  in Flycast; VMU + AICA in — real-hardware run + audio ear-check pending)

## Sequencing & checkpoints

1. **Apple II M1-M3** — DONE (harness-verified); AppleWin/FloppyEmu real-hw
   pass still pending. Real-hw images now built: `mkwoz.py` emits `.woz`
   (WOZ 2.0, self-describing 5.25"/16-sector, bit-exact vs the `.dsk`) +
   `.nib` for FloppyEmu. NB FloppyEmu must run in **5.25" mode** — a 140 KB
   UnoDOS image is rejected by its 3.5"/Smartport (800 KB ProDOS) modes.
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
- Apple II: target machine is the **IIc** (confirmed). On the IIc a 5.25"
  FloppyEmu is the external drive (slot 6, drive 2) and the IIc auto-boots
  drive 1, so the open item is the exact per-ROM drive-2 boot procedure for
  UnoDOS — TBD on metal.
