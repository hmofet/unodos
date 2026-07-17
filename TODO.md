# UnoDOS Roadmap

*Updated 2026-07-17. This file tracks **open** work only. Completed milestones are
recorded in [CHANGELOG.md](CHANGELOG.md) and the per-port READMEs/HANDOFFs — they are
not repeated here.*

## Direction

The forward line is converging on two write-once, retarget-per-platform layers — the
**[`unodef/`](unodef/) Contract** (screen/window/event geometry every world generates
from) and the **[`unoui/`](unoui/) toolkit** (the whole UI as swappable-theme widgets).
The **[`pc64/`](pc64/) Modern PC world** (x86-64 / UEFI) is the furthest-developed
target and the proving ground for the modern stack (networking, TLS, a browser, 3D, USB);
its `unoui` shell is the template the other high-spec ports (PS2, Dreamcast, ARM/PPC) are
expected to adopt. The 8-bit/console ports keep a minimal native path.

---

## pc64 — Modern PC (x86-64 / UEFI) — the active frontier

Shipped and verified on real hardware (Lenovo X1 Carbon Gen 8): the `unoui` desktop
shell, e1000 NIC + from-scratch TCP/IP, TLS 1.2 / HTTPS (BearSSL), a JS-capable browser,
`uno3d` 3D, live resolution/theme switching, resizable windows. Remaining driver tail
(details in [pc64/README.md](pc64/README.md), metal items in
[pc64/METAL-CHECKLIST.md](pc64/METAL-CHECKLIST.md)):

- [ ] **AX88179 USB-Ethernet bulk driver** → `uno_nic_t`, so the net/TLS/HTTPS stack
      lights up on the X1 (which has no wired e1000; Intel Wi-Fi only). xHCI USB core
      (Phases A+B) is done; this is the class driver on top.
- [ ] **xHCI HID** (USB keyboard/mouse) class driver on the USB core.
- [ ] **USBLEGSUP** BIOS→OS handoff (xECP) — may be required for xHCI on real Intel
      controllers (QEMU works without it).
- [ ] **ExitBootServices** — take full ownership from firmware (currently UEFI-as-BIOS
      with boot services alive).
- [ ] **NVMe / AHCI block driver** — real disk storage (today: RAM-FAT + UEFI Simple FS).
- [ ] **Real Intel-GPU 3D backend** for `uno3d` (today: soft rasteriser + honest
      Intel-iGPU soft-fallback scaffold).
- [ ] **I2C-HID trackpad** on-metal bring-up — driver ships gated/self-configuring;
      needs the X1 diagnostic readout iterated (LPSS reset-release landed; parser tuning
      pending real report dumps).
- [ ] Metal re-test pass of the latest build (mouse-driven apps, PC-speaker audio, RTC
      set, HTTPS-needs-NIC) — see the checklist.

---

## Cross-platform features

### Music Player app (full design: [docs/MUSIC-PLAYER-PLAN.md](docs/MUSIC-PLAYER-PLAN.md))
A new app (alongside the built-in-tune **Music** app) that plays music **files**, routed
to the best sound hardware on each platform and scaled down gracefully.
- [ ] Formats: **WAV/AU** (PCM, every platform), **MIDI/SMF** (synth-capable HW), **MP3**
      (PS2 / Dreamcast only — fixed-point decoder), **console-native** (Amiga **MOD**,
      Genesis **VGM**, SNES **SPC**, C64 **SID**, PS2 **VAG**, DC **ADX**).
- [ ] PC sound cards: **AdLib OPL2/OPL3**, **SoundBlaster** (DSP + DMA PCM), **GUS**
      (wavetable). Probe SB → GUS → AdLib → PC-speaker.
- [ ] Native chips: Paula (Amiga), SPC700 (SNES), Ensoniq DOC (IIGS), SID (C64),
      PSG **+ the unused YM2612 FM** (Genesis), SPU2 (PS2), AICA (DC); 1-bit PWM fallback.
- [ ] Architecture: portable core (probe → decoder → sink) + per-device **audio-sink
      vtable** with `caps`; mirrors the Uno3D backend pattern.
- [ ] **Prereq — Music multi-song parity:** finish the remaining built-in Music apps
      (Amiga, MacPlus, Apple II, SNES, IIGS, C64). Done: x86 (10), Mac 7 / Mac 1-6 /
      PS2 / Dreamcast (8, shared C core), Genesis (8, PSG).

### GUI widget model
The C-side ports are covered by **`unoui`** (the pc64 shell + PS2/Dreamcast). The
remaining work is the **bare-metal asm ports**:
- [ ] Bring a shared, themeable widget model (buttons, stepper/arrow controls, lists,
      scrollbars) to the asm ports (Amiga, MacPlus, Genesis, Apple II, SNES, IIGS, C64),
      replacing ad-hoc text-char controls — authored once per concept, themed per platform.

### Kernel / Window Manager (x86 reference OS)
- [ ] Modal window flag (`WIN_FLAG_MODAL`) — block focus changes while modal.
- [ ] Window minimize / maximize.
- [ ] Preemptive multitasking / threading (currently cooperative).
- [ ] Animated sprite support (multi-frame sprite API).

### New apps (x86 reference OS)
- [ ] Calculator.
- [ ] Simple game (Minesweeper, Snake, …).

### App improvements (x86 reference OS)
- [ ] Notepad: Find/Replace.
- [ ] Notepad: Save-As via the system file dialog.
- [ ] File Manager: create new file / new folder.
- [ ] Dostris: 386+ performance path.
- [ ] Music: animated note playback (stems, beams, durations).

### File dialog
- [ ] File-type filter (e.g. show only `.TXT`).
- [ ] Show file sizes in the list.

### Filesystem
- [ ] Directory support (create / navigate subdirectories).
- [ ] Long filename support (LFN).

### APIs
- [ ] Multi-byte-wide sprite support (>8px width).
- [ ] 2bpp color sprite API (like icons, variable size).
- [ ] Update [docs/API_REFERENCE.md](docs/API_REFERENCE.md) for APIs 91–104 (and API 28's
      SI/DI/AH/AL press-latch returns, API 63 ticks-since-boot).

### Documentation
- [ ] App-development tutorial / sample-app walkthrough.
- [ ] Screenshots for the README.

---

## Per-port remaining work

### 8088 / IBM PC-XT (feature parity achieved on a cycle-accurate XT; see [docs/PORT-8088.md](docs/PORT-8088.md))
- [~] **FAT16-on-8088** (DOS-interchangeable CF): boot chain is 8086-clean and verified.
      Remaining — [ ] convert the kernel FAT16 driver (~104 sites across
      read/write/mount/open/cluster/FAT/alloc/readdir/create/delete/rename) to 8086 and
      drop the pre-286 mount gate → desktop + apps + save from a FAT16 CF.
- [ ] **Physical IBM PC/XT pass** (real INT 13h write timing, cross-boot floppy
      persistence) — the final real-hardware step.
- [ ] Optional: dirty-region fill fast path for full-screen game repaint at 4.77 MHz.
- [ ] Launcher `select_icon` draws over open windows (z-order, cosmetic, low priority).
- [ ] Background window content not repainted until raised (single-topmost clipping model).

### Known issue — root-dir entries past 16 break app launch
The low-res launcher has 16 icon slots and the last is its own Refresh icon, so only 15
apps fit; app #16+ collides with the refresh slot. **Workaround shipped** (MOUSE.BIN and
MKBOOT.BIN kept off the default image so Tracker + Paint fit; both still build via
`make apps`). Real fix: exclude the refresh slot from the count or page the icon grid.

### Amiga (bare-metal)
- [ ] FAT12 polish: delete, rename, free-space display, dir-full UX, Tracker `.MOD`
      export, write-verify pass.
- [ ] Blitter fast paths (text row-blit, fills) — the big OCS win.
- [ ] `TICKS_SEC` calibration (vblank pacing runs fast under the WinUAE test config) + NTSC detect.
- [ ] 640-wide OCS hires option (16 colors; needs WM-wide content scaling per PORT-SPEC).
- [ ] Workbench-style chrome (blue/orange/white gadget look).

### Mac System 1–7 (Toolbox, hosted)
- [ ] Offscreen GWorld double-buffering for flicker-free repaints.
- [ ] Audio ear-check on real hardware / sound-enabled emulator (sequencers are register-verified).
- [ ] Real-hardware smoke tests (A500; Mac Plus + Mac II-class).
- [ ] Executor visual pass for the milestone-3 features (blocked on a capture path; a
      Windows-native Executor build would let the rig drive it like BlastEm/WinUAE).

### MacPlus (standalone OS)
- [ ] Files launches `*.APP` entries directly (real multi-app launcher, not the fixed Demo
      icon); FAT12 delete/rename; free-space display; a proper Demo icon.
- [ ] Color milestone: 8bpp framebuffer for the IIci → full-color Theme/Paint/games there.
- [ ] Real-hardware items: sound ear-check, SE sound audibility; Mini vMac validation
      (needs a user-supplied Mac Plus ROM at `macplus/vMac.ROM`); mouse quadrature polarity,
      keyboard poll cadence, SCC write-recovery calibration.
- [ ] Harness: SE/Classic screen-base variants; bus-error injection for the fault screens.
- [ ] Known harness-only bug: ~1/1000 lazy-CCR misevaluation at an interrupt-restore
      boundary (root-caused to Unicorn's context restore; real hardware / Mini vMac unaffected).

### Genesis (hardware-validated)
- [ ] BRAM follow-ups: verify the Mode-1 BIOS path under Genesis Plus GX / Ares, `>8`-file
      listing (BRMDIR paging), Tracker save-to-BRAM.
- [ ] Deferred: SD card over bit-banged SPI + FAT16 ([docs/STORAGE.md](docs/STORAGE.md)
      tier 4; lands with the adapter PCB).
- [ ] Real-hardware adapters still to exercise: PS/2 wiring, tape comparator, Sega CD Mode-1.
- [ ] Console-flavored chrome (after weighing the real-hardware regression risk).

### Apple II (in progress)
- [ ] 6502/dasm/py65; Disk II boot + own GCR RWTS; 280×192 hi-res 1-bit; keyboard desktop.
      M1 landed (toolchain, `mkfont.py`, `boot.s` with 6-and-2 RWTS + multi-track loader).
      Remaining M1: `kernel.s`, `mkdsk.py`, `harness.py`, `build.sh`, `tests/m1.script`,
      README. M2: RWTS write + USV1 FS + Files/Notepad + paddle pointer. M3:
      Dostris/Pac-Man/Paint/Tracker/Theme; OutLast feasibility-gated at 1 MHz.
      Validation: py65 harness → AppleWin → FloppyEmu.

### PS2 (via FreeMcBoot)
- [ ] PS2SDK ELF launched by FMCB; port the portable C core over a gsKit/pad/mc/audsrv
      layer; pad-as-pointer + soft keyboard always, USB kbd/mouse when present; memory-card
      storage. Core + apps are host-shim/PCSX2-verified; remaining is on-device PS2 validation.

### SNES / Apple IIGS (emulator-verified — real-hardware remaining)
- [ ] SNES: real hardware (flashcart + SNES Mouse) + audio ear-check
      (backlog in [snes/HANDOFF.md](snes/HANDOFF.md)).
- [ ] IIGS: real hardware (GSplus/KEGS/MAME then FloppyEmu SmartPort) + audio ear-check
      (see [iigs/HANDOFF.md](iigs/HANDOFF.md)).

### Cross-platform chrome themes (color platforms)
Make window-decoration chrome a selectable style (Mac System 7 / Amiga Workbench /
Windows 3.x / Windows XP "Luna"), distinct from the color-palette "Theme" app. x86 VGA
already ships the Windows-3.x style; CGA ships the flat variant.
- [ ] Define a portable chrome-style id + shared spec in [docs/PORT-SPEC.md](docs/PORT-SPEC.md).
- [ ] Implement all styles in each color port's `draw_window` (x86 VGA, Amiga, mac/ hosted, Genesis).
- [ ] Windows XP "Luna" style (blue gradient title, rounded top, red close, 3D frame).
- [ ] Expose the picker in each port's Theme/Appearance app; persist with the theme settings.
