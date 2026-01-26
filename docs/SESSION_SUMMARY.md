# UnoDOS Development Session Summary
**Last Updated:** 2026-01-26
**Current Version:** v3.12.0
**Current Build:** 019
**Status:** Clock app fix ready for hardware testing

---

## Latest Session (2026-01-26)

### Clock App Crash Fixed (Build 019)

**Problem:** Clock app crashed when loaded. Title bar showed garbage "SQRMU" instead of "Clock", no time displayed.

**Root Cause:** Apps use `[ORG 0x0000]`, meaning all data references are relative to offset 0. But the app loader was using the heap allocator which returned non-zero offsets (e.g., 0x1400:4). When the app referenced `window_title` at offset 0xDC, it pointed to wrong memory.

**Fix:** Changed app loader to use dedicated segment 0x2000 and always load at offset 0:
- `app_load_stub` now uses segment 0x2000 (not heap)
- Apps always loaded at offset 0 within that segment
- This matches the `[ORG 0x0000]` directive in app source files

**Memory Map (Updated):**
```
0x0000 - 0x0FFF  : BIOS/IVT
0x1000 - 0x13FF  : Kernel code and data
0x1400 - 0x1FFF  : Heap (for kernel allocations)
0x2000 - 0x2FFF  : App code segment (apps loaded at offset 0)
```

### Testing Procedure

```powershell
git pull
.\tools\boot.ps1
```

Boot from floppy, press 'L', swap to clock floppy, press key. Clock app should show "Clock" title and display time.

---

## Previous Session (2026-01-25)

### Window Manager Completed (Builds 010-014)

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

### Clock Application Created

First real windowed application:
- apps/clock.asm - Displays RTC time (HH:MM:SS) in a window
- Uses Window Manager API via INT 0x80
- Reads BIOS RTC via INT 1Ah
- Responds to ESC key to exit
- Size: 300 bytes

### INT 0x80 System Call Interface

Apps call kernel functions via INT 0x80:
```asm
mov ah, 19              ; API_WIN_CREATE
mov bx, 100             ; X position
mov cx, 60              ; Y position
mov dx, 120             ; Width
mov si, 50              ; Height
mov al, 0x03            ; Flags
int 0x80                ; Create window
jc error                ; CF set on error
mov [win_handle], ax    ; AX = window handle
```

---

## Foundation Complete (2026-01-23)

### v3.10.0 - Foundation Layer

All components complete:
- System Call Infrastructure (INT 0x80 + API dispatch, 25 functions)
- Graphics API (6 drawing functions + inverted text)
- Memory Allocator (malloc/free)
- Keyboard Driver (INT 09h)
- Event System (32-event queue)
- FAT12 Filesystem (mount, open, read, close)
- Application Loader
- Window Manager

### Kernel Metrics
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
- Testing via floppy disk swap

---

## Build History (Recent)

| Build | Date | Changes |
|-------|------|---------|
| 019 | 2026-01-26 | Fix app loader to use segment 0x2000 offset 0 |
| 018 | 2026-01-26 | Add caller_ds/caller_es segment tracking |
| 014 | 2026-01-25 | INT 0x80 dispatch, clock app working |
| 013 | 2026-01-25 | Inverted text for title bars |
| 012 | 2026-01-25 | Title segment fix |
| 011 | 2026-01-25 | Coordinate swap fix |
| 010 | 2026-01-25 | Initial Window Manager |

---

## Next Steps

1. **Test Build 019** on HP Omnibook - verify clock app works
2. **Mouse Support** (v3.13.0) - PS/2 mouse driver
3. **Window Dragging** (v3.14.0) - Mouse drag to move windows
4. **More Applications** - File manager, text editor

---

*Foundation Layer: COMPLETE*
*Core Services: IN PROGRESS (App Loader + Window Manager done)*
