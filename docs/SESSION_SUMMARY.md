# UnoDOS Development Session Summary
**Last Updated:** 2026-01-27
**Current Version:** v3.12.0
**Current Build:** 041
**Status:** Desktop Launcher WORKING - Ready for next feature

---

## Latest Session (2026-01-27) - Desktop Launcher Complete

### Milestone Achieved
Desktop launcher is fully functional:
- Launch apps from menu (Clock works)
- Return to launcher with ESC
- W/S navigation working
- Enter to launch selected app

### Bug Fix Summary (Builds 036-041)

| Build | Issue | Root Cause | Fix |
|-------|-------|------------|-----|
| 036 | W/S keys not responding | Actually working, added debug | Debug output confirmed events received |
| 037 | App load error code display | Error digit positioned wrong | Display on separate line "Code: X" |
| 038 | File not found (error 3) | app_load_stub read filename_off after changing DS | Read filename_off before changing DS |
| 039 | Auto-launch bug (skips launcher) | Leftover Enter key from disk swap in event queue | Drain event queue at startup |
| 040 | Hello Test crashes | HELLO.BIN was launcher itself (recursive load) | Renamed to TEST.BIN, added actual test app |
| 041 | Cleaner naming | HELLO.BIN confusing | Renamed to LAUNCHER.BIN, kernel loads by name |

### Key Bug Fix (Build 038)

```asm
; WRONG - reads .filename_off with wrong DS:
mov ax, [.filename_seg]
mov ds, ax                    ; DS now caller's segment
mov si, [.filename_off]       ; BUG: local var in kernel segment!

; CORRECT - read before changing DS:
mov si, [.filename_off]       ; Read while DS is still kernel
mov ax, [.filename_seg]
mov ds, ax
```

### Known Issues
- **TEST.BIN crashes:** Low priority - the hello test app doesn't use proper API calls, writes directly to video memory. Clock app works correctly.

---

## Desktop Launcher Architecture (Complete)

**Dual Segment Design:**
- **Shell segment (0x2000):** Launcher loads here, persists while user apps run
- **User segment (0x3000):** User apps (Clock) load here
- Launcher survives app execution and regains control after app exits

**Files on Launcher Floppy:**
- `LAUNCHER.BIN` - Desktop launcher (735 bytes)
- `CLOCK.BIN` - Clock app (249 bytes)
- `TEST.BIN` - Hello test app (112 bytes) - crashes, low priority

**Launcher Features:**
- Window-based menu UI
- W/S keys for navigation
- Enter to launch selected app
- ESC to exit launcher
- Automatic return after app exits (via RETF)

---

## Testing Procedure

### Streamlined Test Workflow

```powershell
.\tools\test-all.ps1
```

Or manually:
```powershell
git pull
.\tools\boot.ps1       # Write UnoDOS to floppy A:
# Swap to blank floppy
.\tools\launcher.ps1   # Write launcher floppy
```

### Test Steps
1. Boot from UnoDOS floppy, verify "Build: 041"
2. Press 'L' to load launcher
3. Swap to launcher floppy when prompted, press any key
4. Launcher menu appears with Clock and Test App
5. Use W/S to select Clock, press Enter
6. Clock displays real-time with RTC
7. Press ESC in clock → returns to launcher
8. Can select and launch Clock again

---

## Previous Sessions

### Builds 030-035: App Loading Debug
6 bug fixes to get launcher loading apps correctly (see previous entries below).

### Builds 025-028: Clock App
Fixed INT 0x80 return values, RTC time reading, display updates.

### Build 019: App Loader Segment Fix
Apps with `[ORG 0x0000]` must load at offset 0 in dedicated segment.

### Builds 010-014: Window Manager
6 window functions: create, destroy, draw, focus, move, get_content.

### Foundation Complete (2026-01-23)
All core components: INT 0x80 API, Graphics, Memory, Keyboard, Events, FAT12, App Loader, Window Manager.

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

## Hardware Platforms

**Primary:** HP Omnibook 600C (486DX4-75)
**Secondary:** Asus EeePC 1005PEB (Atom)

Both use 1.44MB floppy, CGA 320x200 graphics mode.

---

## Files Modified This Session

- `kernel/kernel.asm` - LAUNCHER.BIN filename, app_load_stub DS fix
- `apps/launcher.asm` - Event drain, error display, TEST.BIN naming
- `apps/clock.asm` - Working clock app
- `tools/launcher.ps1` - Updated messaging
- `tools/test-all.ps1` - Streamlined test workflow
- `Makefile` - launcher-floppy.img target with 3 apps

---

*Foundation Layer: COMPLETE*
*Core Services: Desktop Launcher COMPLETE (Build 041)*
*Next: Ready for new feature*
