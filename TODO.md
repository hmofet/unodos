# UnoDOS TODO List

## Kernel / Window Manager
- [ ] Modal window flag (WIN_FLAG_MODAL) — block focus changes when modal is active
- [ ] Window minimize/maximize
- [ ] Preemptive multitasking / threading
- [ ] Serial mouse support
- [ ] Animated sprite support (multi-frame sprite API)

## Apps — New
- [ ] Calculator
- [ ] Paint / drawing tool (could use sprite API)
- [ ] Simple game (Minesweeper, Snake, etc.)

## Apps — Improvements
- [ ] Notepad: Find/Replace
- [ ] Notepad: Save As uses system file dialog (currently text-input)
- [ ] File Manager: create new file / new folder
- [ ] Dostris: performance improvements for 386+
- [ ] MkBoot: validate "Copied 0 files" counter fix (Build 210)
- [ ] Music: animated note playback (stems, beams, note duration visuals)
- [x] Music: 5 songs with visual staff playback + prev/next buttons (Build 279-280)
- [x] Sysinfo: dynamic font-aware layout + proper window sizing (Build 278-280)
- [x] File Manager: scrollbar + dynamic font layout + arrow key navigation (Build 278-280)
- [x] Clock: dynamic font-aware digital time centering (Build 278)

## File Dialog
- [ ] File type filter (e.g. show only .TXT)
- [ ] Show file sizes in list
- [ ] Save dialog variant (with filename input field)

## Filesystem
- [ ] Directory support (create, navigate subdirectories)
- [ ] Long filename support (LFN)

## Boot / Hardware
- [ ] 8086-compatible boot for HP 200LX, Sharp PC-3100
- [x] VGA mode 13h support — 320x200 256-color, switchable in Settings (Build 281)

## APIs (Kernel)
- [x] API 91-92: Utility string conversion (Build 277)
- [x] API 93: Get current font info (Build 278)
- [x] API 94: Sprite drawing — 1-bit transparent bitmap (Build 279)
- [ ] Multi-byte-wide sprite support (>8px width)
- [ ] 2bpp color sprite API (like icons but variable size)

## Documentation
- [x] Update API_REFERENCE.md to cover APIs 44-90 (Build 275)
- [ ] Update API_REFERENCE.md for APIs 91-95
- [ ] App development tutorial / sample app walkthrough
