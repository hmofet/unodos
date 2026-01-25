# UnoDOS v3.12.0 - Window Manager Implementation Plan

## Overview

Implement a minimal Window Manager for UnoDOS, building on the completed Foundation Layer and Application Loader. This enables GUI applications to create and manage windows.

## Current State (v3.11.0 Build 009)

**Completed:**
- System Call Infrastructure (INT 0x80 + API table with 19 functions)
- Graphics API (pixel, rect, filled_rect, char, string, clear_area)
- Memory Allocator (malloc/free at 0x1400:0000)
- Keyboard Driver (INT 09h with scan code translation)
- Event System (32-event circular queue)
- FAT12 Filesystem (mount, open, read, close)
- Application Loader (load, run .BIN files)

**Tested on:** HP Omnibook 600C (486DX4-75, VGA, 1.44MB floppy)

---

## Window Structure (32 bytes)

Following established kernel patterns (app_table, file_table):

```
Offset  Size  Field        Description
------  ----  -----        -----------
  0       1   state        0=free, 1=visible, 2=hidden
  1       1   flags        Bit 0: has_title, Bit 1: has_border
  2       2   x            X position (0-319)
  4       2   y            Y position (0-199)
  6       2   width        Width in pixels
  8       2   height       Height in pixels
 10       1   z_order      0=bottom, 15=top (stacking order)
 11       1   owner_app    App handle that owns window (0xFF=kernel)
 12      12   title        Window title (11 chars + null)
 24       8   reserved     Future use
```

**Constants:**
```asm
WIN_STATE_FREE      equ 0
WIN_STATE_VISIBLE   equ 1
WIN_STATE_HIDDEN    equ 2

WIN_FLAG_TITLE      equ 0x01
WIN_FLAG_BORDER     equ 0x02

WIN_MAX_COUNT       equ 16
WIN_ENTRY_SIZE      equ 32
WIN_TITLEBAR_HEIGHT equ 10
```

---

## API Functions to Add (6 new functions)

| Index | Function | Description |
|-------|----------|-------------|
| 19 | `win_create_stub` | Create new window |
| 20 | `win_destroy_stub` | Destroy window |
| 21 | `win_draw_stub` | Redraw window frame |
| 22 | `win_focus_stub` | Bring window to front |
| 23 | `win_move_stub` | Move window position |
| 24 | `win_get_content_stub` | Get content area bounds |

### Function Signatures

**win_create_stub**
```asm
; Input:  BX = X, CX = Y, DX = Width, SI = Height
;         DI = Pointer to title string (max 11 chars)
;         AL = Flags
; Output: AX = Window handle (0-15), CF=0 success
```

**win_destroy_stub**
```asm
; Input:  AX = Window handle
; Output: CF=0 success
```

**win_draw_stub**
```asm
; Input:  AX = Window handle
; Output: CF=0 success (redraws title bar and border only)
```

**win_focus_stub**
```asm
; Input:  AX = Window handle
; Output: CF=0 success (sets z_order to 15, demotes others)
```

**win_move_stub**
```asm
; Input:  AX = Window handle, BX = New X, CX = New Y
; Output: CF=0 success
```

**win_get_content_stub**
```asm
; Input:  AX = Window handle
; Output: BX = Content X, CX = Content Y
;         DX = Content Width, SI = Content Height
```

---

## Window Visual Design

```
+--[Title Text]--------+  <- Title bar (10px, white)
|                      |  <- 1px top border
|    Content Area      |  <- App draws here
|                      |
+----------------------+  <- 1px bottom border
^                      ^
1px left              1px right
```

**Content area calculation:**
- Content X = Window X + 1 (border)
- Content Y = Window Y + 10 (titlebar) + 1 (border)
- Content Width = Window Width - 2 (borders)
- Content Height = Window Height - 12 (titlebar + borders)

---

## Implementation Steps

### Step 1: Fix gfx_clear_area_stub
Currently a no-op. Must implement to clear window backgrounds:
```asm
gfx_clear_area_stub:
    ; Fill rectangle with background color (color 0)
    ; Same algorithm as gfx_draw_filled_rect but with color 0
```

### Step 2: Add Window Table Data
Location: After `app_table` (around line 2700)
```asm
window_table: times (WIN_MAX_COUNT * WIN_ENTRY_SIZE) db 0
```

### Step 3: Implement Helper Functions
```asm
alloc_window_handle   ; Find free slot in window_table
draw_window_titlebar  ; Draw filled rect + title text
draw_window_border    ; Draw rect outline
```

### Step 4: Implement API Functions
Order:
1. `win_create_stub` - Most critical
2. `win_destroy_stub` - Clear and free
3. `win_draw_stub` - Redraw frame
4. `win_get_content_stub` - Calculate bounds
5. `win_focus_stub` - Z-order management
6. `win_move_stub` - Position update

### Step 5: Update API Table
- Increment function count from 19 to 25
- Add 6 new function pointers at indices 19-24

### Step 6: Add Demo Trigger
Modify keyboard_demo to create test window on 'W' key press.

---

## Files to Modify

1. **[kernel/kernel.asm](kernel/kernel.asm)** - Main kernel
   - Add window_table data structure
   - Fix gfx_clear_area_stub (implement background clear)
   - Add 6 window management functions
   - Update kernel_api_table (count + 6 new pointers)
   - Add 'W' key handler in keyboard_demo

2. **[VERSION](VERSION)** - Update to 3.12.0

3. **[BUILD_NUMBER](BUILD_NUMBER)** - Increment (run `make bump-build`)

4. **[CHANGELOG.md](CHANGELOG.md)** - Document new features

---

## Size Budget

| Component | Size |
|-----------|------|
| window_table | 512 bytes |
| gfx_clear_area_stub fix | ~100 bytes |
| win_create_stub | ~200 bytes |
| win_destroy_stub | ~80 bytes |
| win_draw_stub | ~150 bytes |
| win_focus_stub | ~100 bytes |
| win_move_stub | ~100 bytes |
| win_get_content_stub | ~50 bytes |
| Helper functions | ~200 bytes |
| **Total** | **~1.5 KB** |

Current kernel: ~28KB. Well within limit.

---

## Constraints (v3.12.0)

1. **No overlapping windows** - Windows must not overlap
2. **No dragging** - win_move is API-only, no mouse drag
3. **No close button** - Destroy via API only
4. **White only** - Title bar uses white, no inverse video

These constraints can be lifted in future versions.

---

## Verification

1. **Build test:**
   ```bash
   make bump-build
   make clean && make floppy144
   ```

2. **QEMU test:**
   ```bash
   make run144
   ```
   - Press 'W' to create test window
   - Verify window appears with title bar and border

3. **Hardware test (HP Omnibook 600C):**
   - Write to floppy
   - Boot, press 'W'
   - Verify window draws correctly

---

## Future Enhancements (Not in v3.12.0)

- **v3.13.0**: Mouse support for window interaction
- **v3.14.0**: Window dragging via mouse
- **v3.15.0**: Overlapping window redraw (complex)
- **Future**: Close button, minimize, resize

---

*Plan created: 2026-01-25*
*Target version: v3.12.0*
