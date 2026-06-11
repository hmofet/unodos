# UnoDOS Roadmap

## Post-Audit Backlog (2026-06 — see docs/AUDIT-HANDOFF-2026-06.md §5)
- [ ] **8088 compatibility pass** — 1153 non-8086 instruction sites; the OS
      currently requires a 386+. Tooling ready: tools/to8086.py +
      kernel/cpu8086.inc. FAT16/IDE + HD boot chain need 16-bit LBA rewrite
      or runtime 386 gating.
- [ ] Cursor hide/lock race fix (cursor_protect_begin, ~35 sites — exact
      patch in docs/audit-2026-06-digest.md lines 1863-1890)
- [ ] Finish interrupted performance wave (draw_char MUL hoist, stosw fills,
      floppy multi-sector reads, dispatcher movzx/bt removal)
- [ ] Confirmed-but-unfixed findings: app_load size validation, file-handle
      leak on task kill, focus-time key routing, EVENT_MOUSE coordinates,
      IRQ12 VESA bank switching in ISR, 360KB make target broken, FAT16
      AH=41h check, SysInfo uptime, Notepad status bar, drag off-screen
- [ ] Re-run dynamic QEMU regression scenarios against Build 403+

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
- [ ] 8086-compatible boot for HP 200LX, Sharp PC-3100

## APIs
- [ ] Multi-byte-wide sprite support (>8px width)
- [ ] 2bpp color sprite API (like icons but variable size)
- [ ] Update API_REFERENCE.md for APIs 91-104

## Documentation
- [ ] App development tutorial / sample app walkthrough
- [ ] Screenshots for README
