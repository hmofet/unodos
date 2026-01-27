# UnoDOS Development Session Summary
**Last Updated:** 2026-01-26
**Current Version:** v3.12.0
**Current Build:** 035
**Status:** Desktop Launcher with app loading - awaiting test

---

## Latest Session (2026-01-26) - Desktop Launcher

### Launcher App Loading Fix (Build 033)

**Fixed:** Filename format mismatch causing "Load failed!" error.

**Root Cause:** The launcher's menu_data used raw 8.3 FAT format ('CLOCK   BIN') but
`fat12_open` expects dot-separated format ('CLOCK.BIN\0'). The kernel's FAT12 code
converts "NAME.EXT" to 8.3 internally.

**Fix:** Changed launcher menu_data from:
```asm
db 'CLOCK   BIN'                ; Wrong: raw 8.3 format
```
To:
```asm
db 'CLOCK.BIN', 0               ; Correct: dot-separated, null-terminated
```

### Previous Fixes (Builds 030-032)

| Build | Issue | Root Cause | Fix |
|-------|-------|------------|-----|
| 030 | Menu not showing | draw_menu stack corruption | Rewrite with memory variables |
| 031 | App load failed | app_load_stub used DS after INT 0x80 changed it | Use caller_ds variable |
| 032 | App doesn't run | app_run_stub used AX (AH=18 function#) as handle | Clear AH before validation |
| 033 | Load failed | Filename format mismatch | Use 'NAME.EXT\0' format |
| 034 | Load failed | fat12_open accessed kernel vars with wrong DS | Set DS=0x1000 after filename copy |
| 035 | Load failed | app_load_stub passed drive in DL not AL | Use AL for fs_mount_stub call |

### Desktop Launcher (Build 029)

**Implemented:** Full desktop launcher with dual app segment architecture.

**Architecture:**
- **Shell segment (0x2000):** Launcher app loads here, persists while user apps run
- **User segment (0x3000):** User apps (Clock, Hello) load here
- Launcher can load apps without overwriting itself

**Kernel Changes:**
- Added `APP_SEGMENT_SHELL` (0x2000) and `APP_SEGMENT_USER` (0x3000) constants
- Modified `app_load_stub` to accept DH parameter for target segment
- Added `shell_handle` variable for future auto-return support
- Added `register_shell_stub` (API 25) for shell registration

**New Files:**
- `apps/launcher.asm` - Desktop launcher application (616 bytes)

**Launcher Features:**
- Window-based menu UI
- W/S keys for navigation
- Enter to launch selected app
- ESC to exit
- Returns to launcher after app exits

**Testing:**
```
git pull
.\tools\boot.ps1       # Write UnoDOS to floppy A:
.\tools\launcher.ps1   # Write launcher+clock to floppy B:
```
Boot, press L, swap floppy, press key. Launcher window should appear with Clock and Hello menu items.

---

### Previous: Clock App Working (Builds 025-028)

**Resolved Issues:**
- Build 025: Fixed INT 0x80 destroying AX return values
- Build 026: Implemented RTC time reading
- Build 027: Fixed time updates (save RTC values immediately)
- Build 028: Added clear-before-draw to prevent ghosting

Clock app now shows real-time updating clock with slight flicker (optimization TODO).

### Fixes Applied This Session

**INT 0x80 Carry Flag Preservation:**
```asm
; Before IRET, update FLAGS on stack to preserve CF
mov bp, sp
mov ax, [bp+4]          ; Get saved FLAGS
jc .set_carry
and ax, 0xFFFE          ; Clear CF
jmp .store_flags
.set_carry:
or ax, 0x0001           ; Set CF
.store_flags:
mov [bp+4], ax          ; Store modified FLAGS
```

**Window Handle Extraction:**
Window functions were receiving AX but AH contains function number. Fixed by adding `xor ah, ah` to use only AL as handle.

**Filename Mismatch:**
Kernel looks for HELLO.BIN but Makefile was creating CLOCK.BIN. Fixed Makefile to pass HELLO.BIN to create_app_test.py.

---

## Previous Session (2026-01-26)

### Clock App Crash Fixed (Build 019)

**Problem:** Clock app crashed when loaded. Title bar showed garbage "SQRMU" instead of "Clock".

**Root Cause:** Apps use `[ORG 0x0000]`, meaning all data references are relative to offset 0. But the app loader was using the heap allocator which returned non-zero offsets (e.g., 0x1400:4). When the app referenced `window_title` at offset 0xDC, it pointed to wrong memory.

**Fix:** Changed app loader to use dedicated segment 0x2000 and always load at offset 0:
- `app_load_stub` now uses segment 0x2000 (not heap)
- Apps always loaded at offset 0 within that segment
- This matches the `[ORG 0x0000]` directive in app source files

**Memory Map:**
```
0x0000 - 0x0FFF  : BIOS/IVT
0x1000 - 0x13FF  : Kernel code and data
0x1400 - 0x1FFF  : Heap (for kernel allocations)
0x2000 - 0x2FFF  : App code segment (apps loaded at offset 0)
```

---

## Testing Procedure

```powershell
git pull
.\tools\boot.ps1    # Write UnoDOS to floppy A:
.\tools\clock.ps1   # Write clock app to floppy B: (second USB floppy)
```

Boot from floppy A:, press 'L', swap to clock floppy, press key. Clock app should show "Clock" title and display time.

---

## Previous Sessions

### Window Manager Completed (2026-01-25, Builds 010-014)

**Window Manager API (6 functions):**
- win_create_stub (API 19) - Create window with title and border
- win_destroy_stub (API 20) - Destroy window
- win_draw_stub (API 21) - Redraw window frame
- win_focus_stub (API 22) - Bring to front
- win_move_stub (API 23) - Move position
- win_get_content_stub (API 24) - Get content area bounds

**Bugs Fixed:**
| Bug | Symptom | Root Cause | Fix | Build |
|-----|---------|------------|-----|-------|
| Coordinate swap | Vertical title bar | BX/CX swap needed | xchg bx, cx | 011 |
| Title segment | No title text | Read after segment change | Read before pop ds | 012 |
| White on white | Title invisible | Same color as bar | Inverted text | 013 |
| Far call crash | App crashes | near ret vs far call | INT 0x80 dispatch | 014 |

### Foundation Complete (2026-01-23)

All components complete:
- System Call Infrastructure (INT 0x80 + API dispatch, 25 functions)
- Graphics API (6 drawing functions + inverted text)
- Memory Allocator (malloc/free)
- Keyboard Driver (INT 09h)
- Event System (32-event queue)
- FAT12 Filesystem (mount, open, read, close)
- Application Loader
- Window Manager

**Kernel Metrics:**
- **Total:** 28 KB (56 sectors)
- **Used:** ~5.1 KB (18%)
- **Free:** ~23 KB (82%)

---

## API Table (v3.12.0)

| Index | Function | Description |
|-------|----------|-------------|
| 0 | gfx_draw_pixel_stub | Draw single pixel |
| 1 | gfx_draw_rect_stub | Draw rectangle outline |
| 2 | gfx_draw_filled_rect_stub | Draw filled rectangle |
| 3 | gfx_draw_char_stub | Draw single character |
| 4 | gfx_draw_string_stub | Draw null-terminated string |
| 5 | gfx_clear_area_stub | Clear rectangular area |
| 6 | mem_alloc_stub | Allocate memory |
| 7 | mem_free_stub | Free memory |
| 8 | event_get_stub | Get event (non-blocking) |
| 9 | event_wait_stub | Wait for event |
| 10 | kbd_getchar | Get keyboard character |
| 11 | kbd_wait_key | Wait for keypress |
| 12 | fs_mount_stub | Mount filesystem |
| 13 | fs_open_stub | Open file |
| 14 | fs_read_stub | Read file |
| 15 | fs_close_stub | Close file |
| 16 | fs_register_driver_stub | Register filesystem driver |
| 17 | app_load_stub | Load application |
| 18 | app_run_stub | Run application |
| 19 | win_create_stub | Create window |
| 20 | win_destroy_stub | Destroy window |
| 21 | win_draw_stub | Redraw window frame |
| 22 | win_focus_stub | Bring window to front |
| 23 | win_move_stub | Move window |
| 24 | win_get_content_stub | Get content area bounds |

---

## Hardware Platform

**Primary Test Machine:** HP Omnibook 600C
- CPU: 486DX4-75
- Display: VGA (CGA 320x200 mode used)
- Storage: 1.44MB floppy (A:), HDD (C:) - NO drive B:
- Testing via USB floppy drives and disk swap

---

## Build History (Recent)

| Build | Date | Changes |
|-------|------|---------|
| 024 | 2026-01-26 | Debug: Y=0 marker to verify app execution |
| 023 | 2026-01-26 | Debug: Simplified app, hardcoded coords |
| 022 | 2026-01-26 | Debug: Direct video memory marker |
| 021 | 2026-01-26 | Debug: Added TEST string draw |
| 020 | 2026-01-26 | Fixed INT 0x80 CF preservation, window handle |
| 019 | 2026-01-26 | Fix app loader to use segment 0x2000 offset 0 |
| 018 | 2026-01-26 | Add caller_ds/caller_es segment tracking |
| 014 | 2026-01-25 | INT 0x80 dispatch, clock app working |

---

## Current Debug State (Build 024)

**Clock app (apps/clock.asm) contains:**
1. Create window at X=100, Y=60, W=120, H=50
2. Draw white marker at Y=0, X=0 (very top-left) - **KEY TEST**
3. Hardcoded content coords X=105, Y=75
4. Draw pink marker at content coords
5. Draw white markers at fixed position
6. Main loop waits for ESC to exit

**What to look for:**
- White pixels at top-left corner = App runs after win_create
- No white pixels = App crashes/stuck at win_create

---

*Foundation Layer: COMPLETE*
*Core Services: IN PROGRESS (Debugging clock app content display)*
