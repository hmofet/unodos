# UnoDOS Development Session Summary
**Last Updated:** 2026-01-27
**Current Version:** v3.12.0
**Current Build:** 036
**Status:** Debugging W/S key navigation in launcher

---

## Latest Session (2026-01-27) - Keyboard Navigation Debug

### Current Issue
User reports W/S keys don't work for menu navigation in the launcher.
- Launcher window displays correctly
- Menu draws with items visible
- Key navigation not responding

### Build 036 - Debug Output
Added visual debug display to launcher to diagnose keyboard issue:
- Shows event type (hex digit) at right side of window content area
- Shows key character when key press event received
- Helps identify if events are being received or if the issue is elsewhere

**Test procedure:**
1. Boot from UnoDOS floppy (verify "Build: 036")
2. Press L to load launcher
3. Swap to launcher floppy, press any key
4. Launcher should appear with debug area on right
5. Press W, S, or any key - should see event type "1" and key character displayed

If no debug output appears when pressing keys, the event system isn't reaching the launcher.
If debug shows correct keys, the issue is in the menu update logic.

---

## Previous Session - Launcher App Loading Debug

### Bug Fix Summary (Builds 030-035)

The desktop launcher implementation required 6 bug fixes to get app loading working:

| Build | Issue | Root Cause | Fix |
|-------|-------|------------|-----|
| 030 | Menu not showing | draw_menu stack corruption | Rewrite with memory variables |
| 031 | App load failed | app_load_stub used DS after INT 0x80 changed it | Use caller_ds variable |
| 032 | App doesn't run | app_run_stub used AX (AH=18 function#) as handle | Clear AH before validation |
| 033 | Load failed | Filename format mismatch | Use 'NAME.EXT\0' format |
| 034 | Load failed | fat12_open accessed kernel vars with wrong DS | Set DS=0x1000 after filename copy |
| 035 | Load failed | app_load_stub passed drive in DL not AL | Use AL for fs_mount_stub call |

### Key Bug Details

**Build 033 - Filename Format:**
```asm
; Wrong: raw 8.3 format (launcher menu_data)
db 'CLOCK   BIN'

; Correct: dot-separated, null-terminated (fat12_open expects this)
db 'CLOCK.BIN', 0
```

**Build 034 - DS Segment in fat12_open:**
```asm
.name_done:
    ; After filename copied to stack, set DS to kernel for variable access
    mov ax, 0x1000
    mov ds, ax
```

**Build 035 - Wrong Register for fs_mount:**
```asm
; Wrong:
mov dl, [.drive]
call fs_mount_stub

; Correct (fs_mount_stub expects AL):
mov al, [.drive]
xor ah, ah
call fs_mount_stub
```

---

## Desktop Launcher Architecture (Build 029)

**Dual Segment Design:**
- **Shell segment (0x2000):** Launcher app loads here, persists while user apps run
- **User segment (0x3000):** User apps (Clock, Hello) load here
- Launcher can load apps without overwriting itself

**Kernel Changes:**
- Added `APP_SEGMENT_SHELL` (0x2000) and `APP_SEGMENT_USER` (0x3000) constants
- Modified `app_load_stub` to accept DH parameter for target segment
- Added `shell_handle` variable for future auto-return support

**Launcher Features:**
- Window-based menu UI with Clock and Hello Test entries
- W/S keys for navigation
- Enter to launch selected app
- ESC to exit
- Returns to launcher after app exits

---

## Testing Procedure

```powershell
git pull
.\tools\boot.ps1       # Write UnoDOS to floppy A:
.\tools\launcher.ps1   # Write launcher+clock to same floppy (swap required)
```

1. Boot from UnoDOS floppy, see "Build: 035" on screen
2. Press 'L' to load app
3. Swap to launcher floppy when prompted, press any key
4. Launcher menu appears with Clock and Hello Test
5. Use W/S to select Clock, press Enter
6. Clock should launch showing time
7. Press ESC in clock to return to launcher

---

## Previous Sessions

### Clock App Working (Builds 025-028)

- Build 025: Fixed INT 0x80 destroying AX return values
- Build 026: Implemented RTC time reading
- Build 027: Fixed time updates (save RTC values immediately)
- Build 028: Added clear-before-draw to prevent ghosting

Clock app displays real-time updating clock with slight flicker (optimization TODO).

### App Loader Fixed (Build 019)

Apps use `[ORG 0x0000]` so they must load at offset 0. Changed app loader to use dedicated segment 0x2000 instead of heap allocator.

### Window Manager Completed (Builds 010-014)

6 window functions: create, destroy, draw, focus, move, get_content

### Foundation Complete (2026-01-23)

All core components: INT 0x80 API, Graphics, Memory, Keyboard, Events, FAT12, App Loader, Window Manager

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

## API Table (v3.12.0)

| Index | Function | Description |
|-------|----------|-------------|
| 0-5 | gfx_* | Graphics API (pixel, rect, char, string, clear) |
| 6-7 | mem_* | Memory allocation |
| 8-9 | event_* | Event system |
| 10-11 | kbd_* | Keyboard input |
| 12-16 | fs_* | Filesystem (mount, open, read, close) |
| 17-18 | app_* | App loader (load, run) |
| 19-24 | win_* | Window manager |
| 25 | register_shell | Shell registration (future) |

---

## Hardware Platform

**Primary Test Machine:** HP Omnibook 600C
- CPU: 486DX4-75
- Display: VGA (CGA 320x200 mode)
- Storage: 1.44MB floppy (A:) - single drive, requires disk swap
- Testing via USB floppy on Windows host

---

## Files Modified This Session

- `kernel/kernel.asm` - Fixes for fat12_open DS, app_load_stub register
- `apps/launcher.asm` - Desktop launcher with menu UI
- `apps/clock.asm` - Clock app (working)
- `tools/create_app_test.py` - Multi-file FAT12 floppy creator
- `Makefile` - Launcher build targets

---

*Foundation Layer: COMPLETE*
*Core Services: Desktop Launcher debugging (Build 035)*
