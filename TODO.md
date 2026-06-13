# UnoDOS Roadmap

## Post-Audit Backlog (2026-06) — COMPLETE as of v3.26.0 / Build 405
All items from docs/AUDIT-HANDOFF-2026-06.md §5 are done:
- [x] 8088 compatibility pass — kernel + all apps + floppy boot chain
      assemble under `cpu 8086`; FAT16/IDE region is `cpu 386`-bracketed
      and runtime-gated in fat16_mount; HD boot chain stays 386+ by design
- [x] Cursor hide/lock race fix (cursor_protect_begin, 36 sites)
- [x] Performance wave (cga_pixel_calc row LUT, cursor sprite byte-XOR
      fast path, floppy multi-sector reads, stosw fills, dispatcher
      movzx/bt removal)
- [x] Confirmed-but-unfixed findings (app_load size validation, file-handle
      reaping on task kill, press-time key/click routing, EVENT_MOUSE
      edge-only + focus routing, deferred IRQ12 cursor, 360KB make target,
      FAT16 AH=41h probe, XT KBC gate, SysInfo uptime, Notepad status bar,
      drag clamping, body click-to-raise)
- [x] Dynamic QEMU regression scenarios re-run green against Build 405

### Known issue (2026-06-12): root-dir entries past 16 break app launch
The low-res launcher has 16 icon slots (MAX_ICONS_LO) and the LAST one
is its own Refresh icon - so only 15 apps fit. App #16+ collides with
the refresh slot: blank icon, launch becomes a disk rescan (or fails).
Repro: add a 16th .BIN to the floppy144 list and Enter-launch the last
icons. Fix direction: exclude the refresh slot from MAX_ICONS_LO math
or page the icon grid. Workaround shipped: MOUSE.BIN and MKBOOT.BIN
(diagnostic/utility) left off the default image so Tracker + Paint fit
in the 15-app envelope; both still build via 'make apps'.

### Remaining 8088 follow-ups
- [ ] Real-hardware validation on an 8088 (86Box/PCem or physical XT) —
      QEMU cannot emulate an 8088; current builds are assembler-verified
      (`cpu 8086`) and QEMU-behavior-verified only
- [ ] draw_char CGA row-blit fast path (audit digest "Stage 2", ~10x text
      speedup on real 8088 hardware; Stage 1 MUL removal is done)
- [ ] Launcher select_icon draws over open windows (z-order violation,
      cosmetic — known audit finding, low priority)
- [ ] Background window content not repainted until raised (single-topmost
      clipping model; visible when a covered window is exposed by a drag)

## Platform Ports (68K) — milestone 2 shipped 2026-06-11
Done: Amiga (bare-metal, amiga/) + Mac System 7 color + Mac System 1-6
mono (Toolbox-based, mac/) all boot to the desktop with the WM and run
SysInfo, Clock, Files, Notepad, Music. Spec: docs/PORT-SPEC.md.

Next steps:
- [x] Amiga: MFM track reader + portable FAT12 core, READ path
      (fdd.i + fat12.i): DF1 trackdisk DMA + Amiga-MFM sector decode
      with track cache; FAT12 mount/root-dir/chain-walk; Files mounts
      the DF1 data disk ('m') and opens files in Notepad. mkfat.py
      builds the 880KB FAT12 data image (PC interchange via mtools).
      Verified in WinUAE: multi-cluster CHAIN.TXT read end-to-end.
- [x] Amiga FAT12 WRITE path: MFM track encoder (sector headers,
      checksums, clock-fixup), one-revolution track writes with
      write-protect gate, track-granular RMW sector writes; FAT
      alloc/free/flush, root-dir create/overwrite. Notepad F1 saves to
      DF1 (FAT files + new UNTITLEDTXT), Tracker s/l persists the song.
      Verified in WinUAE: byte-exact image deltas + on-screen re-read.
- [ ] FAT12 polish: delete, rename, free-space display, dir-full UX,
      Tracker .MOD-format export, write-verify pass
- [x] Amiga: Notepad up/down line navigation (goal-column memory) +
      vertical scroll (caret-follow clamp) — verified in WinUAE via the
      AUTOTEST_NOTEPAD build
- [ ] Amiga: blitter fast paths (text row-blit, fills) — the big OCS win
- [ ] Amiga: TICKS_SEC calibration (vblank pacing runs fast under the
      WinUAE test config) + NTSC detection
- [x] Mac: Files subdirectory navigation (PBGetCatInfo ioDrDirID walk,
      PBHSetVol current dir, ".." parent entry) — verified under Executor
      via the UnoDOS7FTest build (enter dir, "..", open file in subdir)
- [ ] Mac: offscreen GWorld double-buffering for flicker-free repaints
- [ ] Both: audio ear-check on real hardware / sound-enabled emulator
      (sequencers are register-verified; test configs run sound off)
- [x] Amiga: MILESTONE 3 cooperative scheduler (scheduler.i): every
      window runs its app proc in its own task with a private 2KB
      stack; task_yield context switch, per-task event mailboxes,
      keys + frame ticks posted by the kernel task, spawn/kill tied
      to window create/close. Verified in WinUAE: game gravity and
      the FAT12 write flow both run through the task machinery.
- [x] Mac: milestone 3 scheduler (2026-06-12): per-window cooperative
      tasks in unodos.c - 68K asm context switch (movem + SP swap),
      heap stacks, one-slot mailboxes with key yield-retry, StkLowPt
      cleared per the Thread Manager convention; both targets
- [x] Mac Tracker (2026-06-12): the 32x4 pattern editor on up to four
      Sound Manager square channels, byte-identical pattern format,
      SONG.TRK via the File Manager
- [x] Mac PC floppy (2026-06-12): FAT12 R/W core in C over an
      injectable block device (.Sony raw driver on SuperDrives; RAM
      image under Executor); Files 'v' volume toggle, Notepad
      round-trip; PC-interchange verified byte-for-byte by an
      independent parser (mac/test_fat12.py)
- [x] Paint on ALL FIVE targets (2026-06-12): the MacPaint-style
      editor (pencil/brush/eraser/line/rect/frect/oval/foval/flood/
      spray) with platform-gamut color selectors - 256 8-bit colors
      on Mac 7, 1-bit dither patterns on Classic, 4096 via copper
      pens on Amiga, 512 via CRAM line-3 tuning on Genesis, full
      mode palette on x86 incl. the 256-color VGA picker
- [x] x86 Tracker + Paint (2026-06-12): apps/tracker.asm (PC speaker,
      leftmost-voice playback, shared SONG.TRK format - QEMU-verified
      with the demo song playing) and apps/paint.asm (QEMU-verified:
      drag strokes, filled shapes, spray, CGA all-colors picker)
- [x] Window click routing fix (2026-06-12): find_window_at returned
      hits with stale flags on Genesis AND Amiga - every window click
      fell through to the desktop (found on real Genesis hardware;
      AUTOTEST_CLICK now guards it by closing a window's close box)
- [ ] Real-hardware smoke tests (A500; Mac Plus + Mac II-class)
- [ ] Mac: Executor visual pass for the milestone-3 features (blocked
      on a capture path; a Windows-native Executor build would let the
      automated rig drive it like BlastEm/WinUAE)

## Genesis Port (2026-06-12) - MILESTONES 1-6 DONE (Amiga parity)
- [x] M0 boot PoC (TMSS, VDP init, font tiles, splash) — verified in
      BlastEm
- [x] M1 kernel: cell-based desktop on plane A (40x28, H40), WM with
      drag/raise/close/z-order, event queue + click latch, sprite
      cursor, icons from the x86 .BIN art (genesis/kernel.asm)
- [x] M1 input: 6-button pad as mouse (d-pad accel + Z turbo, A=click,
      B=soft kbd, C/Start/X/Y synth keys; 3-button safe) + full soft
      keyboard overlay (QWERTY, sticky Shift, F1, arrows)
- [x] M1 PS/2 drivers wired for real hardware: port 2 kbd on the EXT
      interrupt (set-2 decode), port 1 mouse via host-inhibit vblank
      windows + boot probe; decode paths emulator-verified via
      synthetic streams (AUTOTEST_PS2)
- [x] M1 apps: SysInfo, Clock, Notepad (caret/line-nav/scroll/status),
      Music (PSG ch0 Canon in D, staff view)
- [x] M2 game ports (2026-06-11): Dostris + OutLast + Pac-Man, same
      tables/physics/AI as x86, cell rendering via the gcol map
      (Amiga $0RGB -> Genesis $0BGR!), Pac-Man actors as hardware
      sprites, Korobeiniki/Sunset Drive on PSG ch1, game-mode pad
      (d-pad = arrows w/ hold-repeat, A=Space, X=new, Y=pause);
      verified in BlastEm (dostris/outlast/pacman AUTOTESTs)
- [x] M4 SRAM storage (2026-06-11): 8KB battery SRAM + USV1 mini-FS
      (8 files, overwrite-by-name, delete-compaction), Files app,
      Notepad F1-save; BlastEm-verified round trip. $A130F1 once at
      boot only (toggling unmaps it in BlastEm).
- [x] M4.5 tape/WAV storage (2026-06-11): KCS 1200-baud AFSK, PSG
      write path (record the headphone jack), comparator-on-port-2
      read path, injectable decoder (AUTOTEST_TAPE), mktape.py
      WAV encode/decode round trip; Files w/r keys. Comparator
      hardware = real-hw checklist.
- [x] M3 (2026-06-12): Theme over CRAM (8 shared presets + 3-bit RGB
      custom editing, themed entries rewritten live) and Tracker over
      PSG (3 square channels + noise, byte-identical pattern format
      to the Amiga tracker, demo song, edit preview, s/l to SRAM and
      t/y to tape) — BlastEm-verified (AUTOTEST_THEME/_TRACKER)
- [x] M5 (2026-06-12): Sega CD backup RAM via Mode-1 (genesis/bram.i):
      expansion probe (DISK bit + bus-error recovery — BlastEm raises
      a bus error on $400000 with no CD), Kosinski sub-BIOS
      decompress + Sub-CPU boot, ~300-byte SP stub calling the BIOS
      _BURAM traps, LIST/READ/WRITE/DELETE mailbox RPC with Word-RAM
      staging, Files volume toggle ('v') + Notepad F1 to the active
      volume, names normalized 8.3 -> 11 chars with the original name
      in the payload header. RPC/UI emulator-verified through the
      injectable fake transport (AUTOTEST_BRAM); the BIOS-trap path
      itself is a CD-emulator/real-hardware item
- [x] M6 (2026-06-12): cooperative scheduler (genesis/scheduler.i,
      port of amiga/scheduler.i): per-window tasks with private 2KB
      stacks, one-slot mailboxes (keys yield-retry so bursts survive),
      kernel task 0 pumps input/audio; BlastEm-verified (soft-kbd
      typing, PS/2, game gravity all through the task machinery)
- [ ] BRAM follow-ups: verify the Mode-1 BIOS path under Genesis Plus
      GX / Ares (BlastEm has no CD), listing >8 files (BRMDIR paging),
      Tracker save-to-BRAM
- [ ] Deferred: SD card over bit-banged SPI + FAT16 (spec:
      docs/GENESIS-STORAGE.md; lands with the adapter PCB)
- [x] Real hardware (2026-06-12): the cartridge boots and runs on a
      physical console — first new port validated on metal
- [ ] Real-hardware adapters still to exercise: PS/2 wiring, tape
      comparator, Sega CD Mode-1 end-to-end

## Game Ports (2026-06-11) - DONE
- [x] Dostris on Amiga + Mac (same piece tables/scoring/speed curve as
      apps/dostris.asm; windowed; verified WinUAE + Executor)
- [x] OutLast on Amiga + Mac (same track/perspective/traffic/physics as
      apps/outlast.asm; verified WinUAE + Executor)
- [x] Game music on the 68K ports (Korobeiniki + Sunset Drive; Paula + Sound Manager sequencers)
- [x] Pac-Man on the 68K ports (full maze + 3-ghost AI on both;
      Amiga uses incremental tile rendering; verified WinUAE + Executor)

## Tracker (2026-06-11) - v1 DONE
- [x] Amiga Tracker app: write + play 4-channel Paula music (MOD-style
      ProTracker periods, 32-row pattern editor, 4 chip-synthesized
      instruments, demo song, edit preview)
- [ ] Tracker: load/save real .MOD files (needs FAT12), more patterns,
      volume/effect columns, sample import

## Color/Resolution Upgrade (2026-06-11) - DONE
- [x] Amiga: 5 bitplanes / 32 colors (OCS lowres max); themed UI colors
      0-3 + fixed extended game palette 4-31; per-plane primitives
- [x] Mac color targets: true-RGB game art (8-bit QuickDraw), larger
      playfields (Dostris 16px cells, OutLast 480x300 at 3/2 scale)
- [ ] Amiga 640-wide OCS hires option (16 colors max; needs WM-wide
      content scaling per PORT-SPEC)

## Theming & Splash (2026-06-11) - DONE
- [x] 8 shared preset palettes on every color-capable platform (x86 VGA
      via new kernel API 105 + Settings buttons; Amiga Theme app via
      copper; Mac Theme app via mutable kPalette). Preset 1 = Classic
      VGA. Custom colors: per-channel RGB editing on Amiga + Mac;
      element-color mapping (existing) on x86 CGA.
- [x] Per-platform boot splash stating "UnoDOS 3" with platform art:
      IBM PC (CRT/unit/keyboard) on x86, striped checkmark on Amiga,
      happy compact Mac on the Mac targets.

## Kernel / Window Manager
- [ ] Modal window flag (WIN_FLAG_MODAL) — block focus changes when modal is active
- [ ] Window minimize/maximize
- [ ] Preemptive multitasking / threading
- [ ] Serial mouse support
- [ ] Animated sprite support (multi-frame sprite API)

## New Apps
- [ ] Calculator
- [ ] Paint / drawing tool
- [ ] Simple game (Minesweeper, Snake, etc.)

## App Improvements
- [ ] Notepad: Find/Replace
- [ ] Notepad: Save As uses system file dialog
- [ ] File Manager: create new file / new folder
- [ ] Dostris: performance improvements for 386+
- [ ] Music: animated note playback (stems, beams, note duration visuals)

## File Dialog
- [ ] File type filter (e.g. show only .TXT)
- [ ] Show file sizes in list

## Filesystem
- [ ] Directory support (create, navigate subdirectories)
- [ ] Long filename support (LFN)

## Boot / Hardware
- [ ] 8086-compatible boot for HP 200LX, Sharp PC-3100 (kernel itself is
      now 8086-clean; needs 720KB media/geometry support — see audit digest
      ~line 4552 for the design notes)

## APIs
- [ ] Multi-byte-wide sprite support (>8px width)
- [ ] 2bpp color sprite API (like icons but variable size)
- [ ] Update API_REFERENCE.md for APIs 91-104 (and API 28's new
      SI/DI/AH/AL press-latch returns, API 63 ticks-since-boot)

## Documentation
- [ ] App development tutorial / sample app walkthrough
- [ ] Screenshots for README

## MacPlus standalone OS port (macplus/, after M1 2026-06-12)
- [x] M2 (2026-06-12): UnoDOS floppy filesystem (FAT12, shared layout with
      the x86 port) read + written through the .Sony BIOS layer
      (sony.i over _Read/_Write A-traps + the portable fat12.i core from
      the Amiga port); Files + Notepad apps (in-kernel procs, key routing
      via the topmost window's WPROC); disk-loaded app binaries — the
      launcher reads DEMO.APP off the floppy into $40000 and runs it as a
      windowed app through a position-independent ksys-table ABI
      (diskapp.i + demo_app.asm), like the x86 launcher loads .BINs.
      Harness extended to emulate _Write; verified end-to-end in
      tests/m2.script (list/open/edit/save/persist + disk-app load+keys).
- [ ] M2 follow-ups: Files should launch *.APP entries directly (a real
      multi-app launcher rather than the single fixed Demo icon); FAT12
      delete/rename; free-space display; a proper Demo icon (reuses
      icon_paint today)
- [ ] M2 real-hardware risk: .Sony _Read/_Write *after* the kernel has
      taken the VIA/SCC is harness- and spec-validated but unproven on
      metal (the boot-time _Read path is). Watch this on the SE/IIci run.
- [x] REAL-HW FIX (2026-06-12, first SE run): Sad Mac 0F/00000001 - the
      boot block + sony.i hardcoded drive 1; a FloppyEmu on the external
      port boots as drive 2/3. Both layers now honor BootDrive ($210);
      distinctive fail codes ($42 read failed / $43 kernel magic missing);
      harness boots from drive 2 and asserts the drive number end-to-end.
      Sad Mac decode for future runs: 0F/xx = SysError xx during boot;
      0F/0001 from a current build = a REAL bus error (our codes are
      $42/$43).
### M3: full app parity - COMPLETE 2026-06-12
macplus now carries the entire shared app roster (11 apps + the
disk-loaded Demo): SysInfo, Clock, Files, Notepad, Dostris, Pac-Man,
OutLast, Paint, Music, Tracker, Theme.
- [x] Sound foundation (snd.i): the Plus pulse-width buffer (370 words at
      MemTop-$300, high bytes only - low bytes are the .Sony disk PWM),
      Paula-period square synth, machine-gated (Plus full VIA control /
      SE buffer-only / II disabled). gm_* sequencer with 50->60Hz tempo.
- [x] Cooperative scheduler (scheduler.i): per-window tasks, private 2KB
      stacks at $3C000, one-slot mailboxes with the Genesis bounded key
      yield-retry; StkLowPt cleared (ROM stack sniffer). task_body
      re-derives the proc per event (registers are app-clobberable -
      found via Theme's repaint_all using d7).
- [x] Music (square sequencer + staff view, background playback)
- [x] Tracker (byte-identical pattern format; leftmost-voice playback
      like the x86 PC-speaker port; SONG.UNO on the FAT12 floppy)
- [x] Theme - 1-bit dither schemes through the now-mutable pat_tab,
      6 presets incl. full video invert, live whole-screen preview
- [x] Paint (4-dither-ink variant; PAINT.UNO byte-exact round-trip)
- [x] Games: Dostris, OutLast, Pac-Man (verbatim tables/AI; 1-bit
      rendering schemes; gravity/physics through task ticks)
- [ ] Color milestone (orthogonal): 8bpp framebuffer for the IIci, which
      then unlocks full-color Theme/Paint/games on that machine
- [ ] M3 real-hardware items: sound ear-check (synthesis is
      buffer-verified only), SE sound audibility (no VIA writes there)
- [ ] Real-hardware / Mini vMac validation (needs a user-supplied Mac
      Plus ROM dump at macplus/vMac.ROM). Calibration points: mouse
      quadrature polarity (flip eor sense in isr_lvl2 if an axis is
      reversed), keyboard Instant-poll cadence, SCC write-recovery
      delays (nops may need widening on real silicon)
- [ ] Harness: SE/Classic variants (screen base differences), bus-error
      injection to exercise the kernel fault screens
- [ ] macplus harness: rare (~1/1000 injections) lazy-CCR misevaluation at
      an interrupt-injection/restore boundary - a conditional branch right
      at the boundary can misread stale flags. ROOT-CAUSED 2026-06-12 via
      Paint: polls returned exactly the clamp constants (255/139) when the
      clamp's ble misfired; widening the pre-step to flag-producing
      instructions does NOT change the rate (measured), so the fault is in
      Unicorn's context restore, not the boundary choice. Manifests as
      1px clamp spikes in long Paint drags and the 2-char label gap.
      Harness-only; Mini vMac / real hardware unaffected.

## Cross-platform chrome themes (2026-06-12 direction, AFTER macplus parity)
Make the window-decoration LOOK a selectable theme on the COLOR platforms,
not a hardcoded per-port style. So an Amiga can wear the Mac System 7 look,
a PC the Amiga Workbench look, etc. This is a distinct axis from the
color-palette "Theme" app (slot palettes) - it is the draw_window chrome
style. Scope (user 2026-06-12): color platforms only - x86 VGA, Amiga,
mac/ hosted, Genesis; macplus (1-bit) and x86 CGA keep their native look.
- [ ] Define a portable chrome-style id (0=Mac System 7, 1=Amiga Workbench,
      2=Windows 3.x, 3=Windows XP) + a shared spec in docs/PORT-SPEC.md
- [ ] Refactor each color port's draw_window to branch on the style and
      implement ALL styles: kernel/ (x86 VGA), amiga/, mac/ (hosted),
      genesis/
- [ ] NEW Windows XP "Luna" style: blue gradient title bar, rounded top
      corners, the red close button, 3D raised frame (gradient -> CRAM
      ramp on Genesis, banded gradient on 32-color Amiga)
- [ ] Expose the picker in each port's Theme/Appearance app; persist the
      choice with the palette/theme settings

## New ports program (2026-06-12; full plan: docs/PORTS-PLAN.md)
Order: Apple II -> (chrome themes) -> Apple IIGS -> SNES -> PS2. Each
milestone ships with a scripted regression rig + screenshots, macplus-style.
- [ ] Apple II (IN PROGRESS): 6502/dasm/py65; Disk II boot via T0S0
      autoload + own GCR read-RWTS; 280x192 hi-res 1-bit; keyboard-driven
      "UnoDOS Lite" desktop. M1 done so far: toolchain, mkfont.py, boot.s
      (boot0 + 6-and-2 RWTS + multi-track loader). Remaining M1: kernel.s,
      mkdsk.py, harness.py (py65), build.sh, tests/m1.script, README.
      Validation: py65 harness -> AppleWin (proves RWTS) -> FloppyEmu.
      M2: RWTS write + USV1-style FS + Files/Notepad + paddle pointer.
      M3: Dostris/Pac-Man/Paint/Tracker(blocking speaker)/Theme-dithers;
      OutLast feasibility-gated at 1MHz.
- [ ] Apple IIGS: 65C816/ca65; firmware block boot (the macplus model:
      SmartPort = the .Sony equivalent); SHR 320x200x16 desktop; ADB
      firmware-state input; FAT12 in the 800K block space; Ensoniq
      wavetable Music/Tracker. Harness decision at M0 (py65816 vs GSplus
      scripting). FloppyEmu covers real-hw media.
- [ ] SNES: 65816 toolchain shared with IIGS; the GENESIS architecture
      re-expressed (tile desktop, sprite cursor, pad-as-pointer + soft
      keyboard, SNES Mouse, SRAM USV1 FS); SPC700 uploaded audio driver
      is the hardest novel piece. Rig: Mesen2 (the BlastEm role).
- [ ] PS2 via FreeMcBoot: PS2SDK ELF launched by FMCB; port the PORTABLE
      C CORE from mac/unodos.c over a gsKit/pad/mc/audsrv platform layer;
      pad-as-pointer + soft keyboard always, USB kbd/mouse when present;
      memory-card file storage. Rig: PCSX2 boots ELFs directly.
      Open Qs: SNES flashcart + Mouse on hand? PS2 USB kbd? MC vs USB
      storage primary? Apple II target machine (II+/IIe/IIc)?

## Platform-authentic chrome (2026-06-12 direction)
- [x] Mac ports: classic Mac / System 7 look (shadows, pinstriped active
      title, square close box)
- [ ] Amiga: Workbench-style chrome (blue/orange/white gadget look)
- [ ] x86 VGA (486): Windows 3.x-style chrome (3D bevels, solid title bar)
- [ ] x86 CGA (8088): same design language reduced to 4 colors
- [ ] Genesis: console-flavored chrome (after real-hardware regression risk
      is weighed - port is hardware-validated as-is)
- [ ] x86 chrome status (2026-06-12): VGA already ships the Windows-3.x
      3D widget_style; CGA ships the flat variant - platform-authentic
      as-is. Follow-ups: (a) qemu_test.py key/click injection didn't land
      this session (boot+render fine) - re-check the QEMU input workaround;
      (b) optional VGA window drop shadows need the dirty-rect/move-erase
      interaction solved first
