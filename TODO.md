# UnoDOS Roadmap

*Updated 2026-08-06. This file tracks **open** work only. Completed milestones are
recorded in [CHANGELOG.md](CHANGELOG.md) and the per-port READMEs/HANDOFFs, they are
not repeated here. Per-programme detail lives in the plan docs linked below; the
async channel between agents is [pc64/UNOAUTOMATE-REQUESTS.md](pc64/UNOAUTOMATE-REQUESTS.md),
and the process every agent follows is [AGENTS.md](AGENTS.md).*

## Direction

The forward line is converging on two write-once, retarget-per-platform layers, the
**[`unodef/`](unodef/) Contract** (screen/window/event geometry every world generates
from) and the **[`unoui/`](unoui/) toolkit** (the whole UI as swappable-theme widgets).
The **[`pc64/`](pc64/) Modern PC world** (x86-64 / UEFI) is the furthest-developed
target and the proving ground for the modern stack (networking, TLS, a browser, 3D, USB);
its `unoui` shell is the template the other high-spec ports (PS2, Dreamcast, ARM/PPC) are
expected to adopt. The 8-bit/console ports keep a minimal native path.

---

## pc64, Modern PC (x86-64 / UEFI), the active frontier

Shipped and verified on real hardware: the `unoui` desktop shell and modern window
manager, e1000/e1000e/igb/r8169 wired NICs, AX201 Wi-Fi, AX88179 USB-Ethernet,
from-scratch TCP/IP + TLS/HTTPS, a JS-capable browser with two script engines and two
CSS cascades, AHCI/NVMe/SDHCI block drivers, xHCI + HID + mass storage, firmware detach
(ExitBootServices, ZimaBlade-confirmed), the installer, URC remote control, unolog, an
Office 97 suite, and `uno3d`.

**The 2026-07-17 driver tail is closed** apart from the items below (details in
[pc64/README.md](pc64/README.md), metal items in
[pc64/METAL-CHECKLIST.md](pc64/METAL-CHECKLIST.md)):

- [ ] **USBLEGSUP BIOS→OS handoff (xECP)** is still not implemented. `pc64/xhci.c`
      instead rides out the handoff race by retrying HCRST + re-init up to five times,
      which has been enough on every machine tested so far; a real extended-capability
      walk remains the correct fix if a controller ever needs it.
- [ ] **Real Intel-GPU 3D backend** for `uno3d`. `uno3d/uno3d_intel.c` probes PCI for
      the iGPU and then honestly delegates every draw to the software rasteriser; the
      command-streamer path (GTT, batch buffers, 3DPRIMITIVE, display-engine flip) is
      unwritten, and only the four vtable hooks change when it lands.
- [ ] **I2C-HID trackpad on metal.** Root-caused and fixed in source (`pc64/i2c_hid.c`:
      the LPSS input clock is 216 MHz on Cannon/Comet/Ice/Tiger Lake, not the 133 MHz
      the DesignWare timing table assumed), but the fix has never been run on the X1.
- [ ] **DNS fails on a production build** while a debug build resolves fine on the same
      host (filed 2026-08-06 against the unonet lane). The lease succeeds and the
      resolver still cannot ask anybody, so the suspect is the DNS server address out of
      the lease. Blocks regenerating the manual's network figures.
- [ ] **iwlwifi RX decryption on the Surface Go**: association and the 4-way handshake
      complete, no DHCP lease follows. The next move needs one metal boot producing the
      `post-join diag` line, then most likely a station remove/add instead of the live
      retarget (see the 2026-08-05 entries in the requests file).
- [ ] Metal re-test pass of the latest build, see the checklist.

### Programmes in flight (pc64)

Each has its own plan doc; this is the index, not the detail.

- [ ] **Virtualization** (branch `unovirt`, not yet on master): A0 through A6 committed
      and pushed, a Linux guest boots under UnoDOS and says so. Active.
- [ ] **BIOS boot** ([docs/BIOS-BOOT-PLAN.md](docs/BIOS-BOOT-PLAN.md)): A-E green in
      QEMU including the installer and the flasher; **two metal runs, neither
      conclusive** (Acer Revo RL100, Asus Eee PC 1005). Run 2 has the cleaner next step.
- [ ] **Detach completion** ([docs/DETACH-COMPLETION-PLAN.md](docs/DETACH-COMPLETION-PLAN.md)):
      phase B is metal-pending on the X1, which is the machine that can falsify it; B3
      (Surface keyboard) is deliberately unanswered; phase D fleet validation is
      outstanding on the Yoga, X1 and Surface.
- [ ] **Web engine** ([docs/WEB-ENGINE-DESIGN.md](docs/WEB-ENGINE-DESIGN.md)): **forms**
      (M6: no text input, no submit, `pc64_http` is GET-only) and **parallel TLS** (M7,
      blocked on per-socket TLS from unonet). The unoweb renderer stays non-default by
      user ruling.
- [ ] **Second engine** ([docs/BROWSER-ENGINE2-PLAN.md](docs/BROWSER-ENGINE2-PLAN.md)):
      the default-cascade flip is the user's call, and the quickjs DOM adapter is
      written but pinned off pending a mingw-only startup crash.
- [ ] **UnoMail** ([docs/OFFICE97-PLAN.md](docs/OFFICE97-PLAN.md)): the fourth Office
      app, deferred on the OAuth / web sign-on question. Word, Excel and PowerPoint are
      done.
- [ ] **SSH client** ([docs/SSH-CLIENT-PLAN.md](docs/SSH-CLIENT-PLAN.md)): PROPOSAL
      only, nothing started. The spec is the build order.
- [ ] **`cfg-parse-window`**: three commits (read the whole DEBUG.CFG, and say so when
      it does not fit) sitting unlanded on their branch. Land or delete per
      [AGENTS.md](AGENTS.md) §3.

---

## Cross-platform features

### Aurora rollout (plan: [PLAN-aurora-rollout.md](PLAN-aurora-rollout.md), state: [HANDOFF-aurora.md](HANDOFF-aurora.md))
Bring the unoui Aurora theme to the ports. Phase 0 (the `UNO_BG_CACHE` compositing
foundation + the `unoui_aurora_lite` full/static split) and phase 1 (**ps2** and
**dreamcast**, both render-verified in PCSX2 and flycast) are on master.
- [ ] **Phase 2, rpi → pinephone → ppcmac**: stand a freestanding C world beside each
      asm kernel, point an `fb.c` at the framebuffer the asm bring-up already
      establishes, add a timer/input shim, then unoui + Aurora FULL.
- [ ] **Phase 4 mac (dithered)**: a QuickDraw `fb` backend, Aurora at `DEPTH_8` on a
      256-color Mac. The easiest remaining win (already hosted C).
- [ ] **Phase 3 gba / x86 (STATIC toggle)**: gba gets a build-time-baked wallpaper
      behind the existing asm UI; x86 needs a VESA LFB truecolor mode plus a pc64-style
      C+unoui world, the biggest single lift.
- [ ] **Phase 4 amiga** (conditional spike: vbcc/gcc-m68k + chunky→planar `c2p`, 020/AGA
      only). **iigs is excluded** from unoui-Aurora; native faux-aurora only if wanted.

### Cross-port performance pass (state: [HANDOFF-perf.md](HANDOFF-perf.md))
The work-list is every `AUDIT-<port>.md` §1 plus cross-cutting pattern #1 in
[AUDIT-INDEX.md](AUDIT-INDEX.md): no damage-rect means a full-scene repaint on drag and
redraw. amiga, c64, dreamcast, genesis, iigs, mac, ps2, sms and snes each carry at least
one **✅ FIXED** finding.
- [ ] The remaining ports have no fixes recorded yet (pc64, x86, apple2, gb, gg, gba,
      macplus, nes, pce, pinephone, ppcmac, rpi, vic20, ws). x86 (full damage-rect),
      amiga (XOR outline) and macplus (damage-min) are the models to copy.
- [ ] **Hard rule, unchanged:** every fix is a redraw refactor, so render-verify it. A
      correct refactor is byte-identical against a baseline built from the pre-change
      source.

### Music Player app (full design: [docs/MUSIC-PLAYER-PLAN.md](docs/MUSIC-PLAYER-PLAN.md))
A new app (alongside the built-in-tune **Music** app) that plays music **files**, routed
to the best sound hardware on each platform and scaled down gracefully.
- [ ] Formats: **WAV/AU** (PCM, every platform), **MIDI/SMF** (synth-capable HW), **MP3**
      (PS2 / Dreamcast only, fixed-point decoder), **console-native** (Amiga **MOD**,
      Genesis **VGM**, SNES **SPC**, C64 **SID**, PS2 **VAG**, DC **ADX**).
- [ ] PC sound cards: **AdLib OPL2/OPL3**, **SoundBlaster** (DSP + DMA PCM), **GUS**
      (wavetable). Probe SB → GUS → AdLib → PC-speaker.
- [ ] Native chips: Paula (Amiga), SPC700 (SNES), Ensoniq DOC (IIGS), SID (C64),
      PSG **+ the unused YM2612 FM** (Genesis), SPU2 (PS2), AICA (DC); 1-bit PWM fallback.
- [ ] Architecture: portable core (probe → decoder → sink) + per-device **audio-sink
      vtable** with `caps`; mirrors the Uno3D backend pattern.
- [ ] **Prereq, Music multi-song parity:** finish the remaining built-in Music apps
      (Amiga, MacPlus, Apple II, SNES, IIGS, C64). Done: x86 (10), Mac 7 / Mac 1-6 /
      PS2 / Dreamcast (8, shared C core), Genesis (8, PSG).

### GUI widget model
The C-side ports are covered by **`unoui`** (the pc64 shell + PS2/Dreamcast). The
remaining work is the **bare-metal asm ports**:
- [ ] Bring a shared, themeable widget model (buttons, stepper/arrow controls, lists,
      scrollbars) to the asm ports (Amiga, MacPlus, Genesis, Apple II, SNES, IIGS, C64),
      replacing ad-hoc text-char controls, authored once per concept, themed per platform.

### Kernel / Window Manager (x86 reference OS)
- [ ] Modal window flag (`WIN_FLAG_MODAL`), block focus changes while modal.
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
- [ ] Update [docs/API_REFERENCE.md](docs/API_REFERENCE.md) for APIs 91-104 (and API 28's
      SI/DI/AH/AL press-latch returns, API 63 ticks-since-boot).

### Documentation
- [ ] App-development tutorial / sample-app walkthrough.
- [ ] Screenshots for the README.

---

## Per-port remaining work

### Fresh-3.1 console tier (app parity + storage)
The 2026-07-19 audit's conclusion that this work was lost was wrong; it landed on
`parity-push-fresh-ports` and is on master. **sms, nes, gba, rpi, pinephone and ppcmac
are at 11 of 11 apps** (Tracker, OutLast, Pac-Man and Paint are real and wired into
dispatch). See [docs/PARITY-HANDOFF.md](docs/PARITY-HANDOFF.md).
- [ ] **gb, gg, vic20, ws, pce still ship 7 of 11.** Those four apps are launcher
      placeholders on each.
- [ ] **Storage persistence is outstanding across the whole fresh tier.**
- [ ] `docs/FEATURE-MATRIX.md` is stale: no C64 column, the pc64 storage row predates
      the native block drivers, and the fresh-port rows now understate six ports.
- [ ] `parity-wip` (`b2e40c1`, does not build by design) is fully superseded by master
      and holds nothing worth recovering. Do not merge it; it is a deletion candidate.

### 8088 / IBM PC-XT (feature parity achieved on a cycle-accurate XT; see [docs/PORT-8088.md](docs/PORT-8088.md))
- [~] **FAT16-on-8088** (DOS-interchangeable CF): boot chain is 8086-clean and verified.
      Remaining, [ ] convert the kernel FAT16 driver (~104 sites across
      read/write/mount/open/cluster/FAT/alloc/readdir/create/delete/rename) to 8086 and
      drop the pre-286 mount gate → desktop + apps + save from a FAT16 CF.
- [ ] **Physical IBM PC/XT pass** (real INT 13h write timing, cross-boot floppy
      persistence), the final real-hardware step.
- [ ] Optional: dirty-region fill fast path for full-screen game repaint at 4.77 MHz.
- [ ] Launcher `select_icon` draws over open windows (z-order, cosmetic, low priority).
- [ ] Background window content not repainted until raised (single-topmost clipping model).

### Known issue, root-dir entries past 16 break app launch
The low-res launcher has 16 icon slots and the last is its own Refresh icon, so only 15
apps fit; app #16+ collides with the refresh slot. **Workaround shipped** (MOUSE.BIN and
MKBOOT.BIN kept off the default image so Tracker + Paint fit; both still build via
`make apps`). Real fix: exclude the refresh slot from the count or page the icon grid.

### Amiga (bare-metal)
- [ ] FAT12 polish: delete, rename, free-space display, dir-full UX, Tracker `.MOD`
      export, write-verify pass.
- [ ] Blitter fast paths (text row-blit, fills), the big OCS win.
- [ ] `TICKS_SEC` calibration (vblank pacing runs fast under the WinUAE test config) + NTSC detect.
- [ ] 640-wide OCS hires option (16 colors; needs WM-wide content scaling per PORT-SPEC).
- [ ] Workbench-style chrome (blue/orange/white gadget look).

### Mac System 1-7 (Toolbox, hosted)
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

### Apple II (M1-M3 shipped; real hardware remaining)
M1 (desktop + boot/RWTS path), M2 (write RWTS + USV1 mini-FS + Files/Notepad) and M3
(the disk-loaded apps, audio, Theme, and the cooperative-scheduler verdict) are all
shipped and py65-harness-verified. The as-built behavior is in
[apple2/README.md](apple2/README.md); the open questions below are from
[apple2/HANDOFF.md](apple2/HANDOFF.md) §11.
- [ ] **AppleWin pass**: only an emulator can prove the *absolute* sector
      skew/interleave convention matches real DOS-order `.dsk` semantics (the harness
      selftest only proves it is self-consistent), and it settles the 16-sector autoload
      assumption on a clone P5A ROM.
- [ ] **Real hardware** via FloppyEmu (WOZ imaging path is documented), plus an audio
      ear-check.
- [ ] Confirm the target machine: M1 targets the II+ floor, and a IIe later unlocks
      up/down plus 80 columns. The `$FBB3/$FBC0` SysInfo seeds are `$E0/$E0`, not a real
      machine pair, and want fixing to II+ (`$EA/$EA`) alongside SysInfo's decode.

### PS2 (via FreeMcBoot)
- [ ] PS2SDK ELF launched by FMCB; port the portable C core over a gsKit/pad/mc/audsrv
      layer; pad-as-pointer + soft keyboard always, USB kbd/mouse when present; memory-card
      storage. Core + apps are host-shim/PCSX2-verified; remaining is on-device PS2 validation.

### SNES / Apple IIGS (real-hardware remaining)
- [ ] SNES: **conditional pass on a clone** (SupaBoy Hyperbeach + FXPak Pro): the OS
      boots and is navigable, but desktop icons render as text labels only and there is
      no audio. Genuine SNES silicon is pending, and the two things to re-check there
      are the icon-tile upload (shadow→VRAM DMA) and SPC700 audio. Backlog in
      [snes/HANDOFF.md](snes/HANDOFF.md).
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
