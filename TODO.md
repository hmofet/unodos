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
- [ ] Amiga FAT12 WRITE path: MFM encode + track write + verify, FAT/
      dir updates - unlocks Notepad F1 save to disk, Tracker .MOD
      save, file create/delete
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
- [ ] Both: cooperative scheduler over the window/app tables (milestone
      3 — the single event loop is scaffolding)
- [ ] Real-hardware smoke tests (A500; Mac Plus + Mac II-class)
- [ ] Sega Genesis showpiece port (per feasibility report SS3.3 —
      optional, after the portable core exists)

## Game Ports (2026-06-11) - DONE
- [x] Dostris on Amiga + Mac (same piece tables/scoring/speed curve as
      apps/dostris.asm; windowed; verified WinUAE + Executor)
- [x] OutLast on Amiga + Mac (same track/perspective/traffic/physics as
      apps/outlast.asm; verified WinUAE + Executor)
- [ ] Game music on the 68K ports (Korobeiniki / driving songs)
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
