# UnoDOS TODO List

## Kernel / Window Manager
- [ ] Modal window flag (WIN_FLAG_MODAL) — block focus changes when modal is active
- [ ] Window minimize/maximize
- [ ] Preemptive multitasking / threading
- [ ] Serial mouse support

## Apps — New
- [ ] Calculator
- [ ] Paint / drawing tool
- [ ] System monitor (memory, tasks, tick count)
- [ ] Simple game (Minesweeper, Snake, etc.)

## Apps — Improvements
- [ ] Notepad: Find/Replace
- [ ] Notepad: Save As uses system file dialog (currently text-input)
- [ ] File Manager: create new file / new folder
- [ ] Dostris: performance improvements for 386+
- [ ] MkBoot: fix "Copied 0 files" counter bug

## File Dialog
- [ ] File type filter (e.g. show only .TXT)
- [ ] Show file sizes in list
- [ ] Save dialog variant (with filename input field)

## Filesystem
- [ ] Directory support (create, navigate subdirectories)
- [ ] Long filename support (LFN)

## Boot / Hardware
- [ ] 8086-compatible boot for HP 200LX, Sharp PC-3100
- [ ] VGA mode support (640x480 or 320x200 256-color)

## Documentation
- [x] Update API_REFERENCE.md to cover APIs 44-90 (Build 275)
- [ ] App development tutorial / sample app walkthrough
