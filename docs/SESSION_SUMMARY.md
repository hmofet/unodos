# UnoDOS Development Session Summary
**Last Updated:** 2026-01-28
**Current Version:** v3.12.0
**Current Build:** 052
**Status:** Dynamic App Discovery & File Browser COMPLETE

---

## Latest Session (2026-01-28) - Dynamic Discovery & Browser

### Features Implemented

1. **fs_readdir API (API Index 26)** - Kernel directory iteration
   - Iterates FAT12 root directory entries
   - State-based: CX encodes sector/entry position
   - Returns 32-byte directory entries to caller buffer
   - Skips deleted entries, LFN, volume labels, directories

2. **Dynamic App Discovery** - Launcher scans for .BIN files
   - No hardcoded menu - discovers apps at runtime
   - Skips LAUNCHER.BIN (doesn't show itself)
   - Converts FAT "CLOCK   BIN" to "CLOCK.BIN" for loading

3. **File Browser App** - Shows all files with sizes
   - Window-based UI listing directory contents
   - Displays "FILENAME.EXT  12345" format
   - ESC to exit back to launcher

### Bug Fixes (Builds 042-052)

| Build | Issue | Root Cause | Fix |
|-------|-------|------------|-----|
| 042 | fs_readdir API | New feature | Added API index 26 to kernel |
| 043 | Clock fails to launch | FAT format mismatch | get_selected_filename converts format |
| 044-050 | Browser keyboard not working | Multiple issues | Debug with port I/O |
| 051 | Browser ESC doesn't work | Interrupts disabled + event handling | Added STI, use JC pattern like Clock |
| 052 | Cleanup | Unused debug code | Removed loop_counter |

### Key Discovery: Browser Keyboard Fix (Build 051)

The browser wasn't responding to keyboard because:
1. **Interrupts were disabled** - INT 0x80 clears IF, needs STI to re-enable
2. **Wrong event handling pattern** - Clock uses `jc .no_event` which works

Fix: Add `sti` at start of main loop and use same pattern as Clock:
```asm
.main_loop:
    sti                             ; Enable keyboard IRQ
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .no_event
    cmp dl, 27                      ; ESC?
    je .exit_ok
```

---

## Testing Procedure

```powershell
git pull
.\tools\boot.ps1           # Write UnoDOS to floppy A:
# Swap to blank floppy
.\tools\launcher.ps1       # Write launcher floppy
```

### Test Steps
1. Boot from UnoDOS floppy, verify "Build: 052"
2. Press 'L' to load launcher
3. Swap to launcher floppy when prompted
4. Launcher shows: CLOCK, TEST, BROWSER (dynamically discovered)
5. Select BROWSER, press Enter
6. Browser shows all files with sizes
7. Press ESC → returns to launcher
8. Select CLOCK, press Enter → clock works
9. Press ESC → returns to launcher

---

## Files on Launcher Floppy

| File | Size | Description |
|------|------|-------------|
| LAUNCHER.BIN | 1061 bytes | Desktop launcher with dynamic discovery |
| CLOCK.BIN | 249 bytes | Real-time clock display |
| TEST.BIN | 112 bytes | Hello test app |
| BROWSER.BIN | 564 bytes | File browser |

---

## API Table (v3.12.0)

| Index | Function | Description |
|-------|----------|-------------|
| 0-5 | gfx_* | Graphics API |
| 6-7 | mem_* | Memory allocation |
| 8-9 | event_* | Event system |
| 10-11 | kbd_* | Keyboard input |
| 12-16 | fs_* | Filesystem (mount, open, read, close, stat) |
| 17-18 | app_* | App loader (load, run) |
| 19-24 | win_* | Window manager |
| 25 | register_shell | Shell registration |
| **26** | **fs_readdir** | **Directory iteration (NEW)** |

---

## Memory Map

```
0x0000 - 0x0FFF  : BIOS/IVT
0x1000 - 0x13FF  : Kernel code and data
0x1400 - 0x1FFF  : Heap (kernel allocations)
0x2000 - 0x2FFF  : Shell/Launcher segment
0x3000 - 0x3FFF  : User app segment
```

---

## Previous Sessions

### Build 041: Desktop Launcher Complete
- W/S navigation, Enter to launch, ESC to exit
- Event drain at startup to prevent auto-launch

### Builds 025-028: Clock App
- RTC time reading, display updates

### Build 019: App Loader Segment Fix
- Apps with [ORG 0x0000] load at segment offset 0

---

*Dynamic App Discovery: COMPLETE*
*File Browser: COMPLETE*
*Ready for next feature*
