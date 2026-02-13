# UnoDOS Window Manager

## Overview

The UnoDOS window manager provides GUI applications with windowed display, mouse-driven title bar dragging, and OS-managed content preservation. Apps draw using window-relative coordinates via the drawing context API.

**Status: COMPLETE (Build 135)**

## Current State (v3.13.0 Build 135)

**All Features Implemented:**
- Window create/destroy/draw/focus/move (APIs 20-25)
- Window drawing context for relative coordinates (APIs 31-32)
- Mouse-driven title bar dragging with deferred processing
- XOR mouse cursor (8x10 arrow sprite)
- OS-managed content preservation during drags (scratch buffer at 0x5000)
- Text width measurement (API 33) for proper window sizing
- Desktop Launcher with dynamic app discovery

**Tested on:** HP Omnibook 600C (486DX4-75, VGA, 1.44MB floppy), QEMU

---

## Window Structure (32 bytes)

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

## Window Visual Design

```
+--[Title Text]--------+  <- Title bar (10px, white background, black text)
|                      |  <- 1px top border
|    Content Area      |  <- App draws here (window-relative 0,0)
|                      |
+----------------------+  <- 1px bottom border
^                      ^
1px left              1px right
```

**Content area calculation:**
- Content X = Window X + 1 (border)
- Content Y = Window Y + 10 (titlebar)
- Content Width = Window Width - 2 (borders)
- Content Height = Window Height - 12 (titlebar + borders)

---

## Window Sizing with Text Measurement

Characters render at 8x8 pixels with a **12px advance** (8px glyph + 4px gap). Use `gfx_text_width` (API 33) to measure string width:

```asm
; Example: Size window to fit "00:00:00"
mov si, time_str
mov ah, 33                  ; API_GFX_TEXT_WIDTH
int 0x80                    ; DX = 96 (8 chars × 12px)

; Window width = text_width + padding + borders
; Window width = 96 + 12 + 2 = 110
```

---

## API Functions

| Index | Function | Description |
|-------|----------|-------------|
| 20 | win_create | Create window with position, size, title, flags |
| 21 | win_destroy | Destroy window and clear its screen area |
| 22 | win_draw | Redraw window frame (title bar + border) |
| 23 | win_focus | Set active window (z-order to top) |
| 24 | win_move | Move window to new position |
| 25 | win_get_content | Get content area bounds |
| 31 | win_begin_draw | Set drawing context (enables coordinate translation) |
| 32 | win_end_draw | Clear drawing context |

### Drawing Context

When active, APIs 0-6 automatically translate BX/CX from window-relative to absolute screen coordinates:

```
absolute_x = window_x + 1 + relative_x
absolute_y = window_y + 10 + relative_y
```

Apps call `win_begin_draw` once after creating a window, then draw at (0,0) = top-left of content.

---

## Mouse Cursor and Dragging

### XOR Cursor
- 8x10 pixel arrow sprite
- Drawn with `plot_pixel_xor` (self-erasing: draw once to show, draw again to erase)
- `cursor_locked` flag prevents cursor during window operations (no XOR artifacts)

### Title Bar Dragging
Three-layer architecture prevents reentrancy issues:

1. **IRQ12 Handler** - Detects button press on title bars, tracks drag offset
2. **Drag State Machine** - Sets `drag_needs_update` flag with target position
3. **event_get_stub** - Processes deferred move (safe INT 0x80 context)

```
Mouse IRQ12:
  cursor_hide → update position → drag_update → cursor_show → post event

event_get (INT 0x80):
  mouse_process_drag → win_move if drag_needs_update
```

### Content Preservation
During window moves, the OS saves/restores CGA pixel content:
1. Save current window content to scratch buffer (0x5000)
2. Clear old window position
3. Restore content at new position
4. Redraw window frame

Uses byte-aligned CGA operations with `min(old_bpr, new_bpr)` to handle position changes across byte boundaries.

---

## Completed Enhancements

- [x] Mouse cursor (XOR sprite, visible on all backgrounds)
- [x] Title bar hit testing (mouse click detection)
- [x] Window dragging via mouse (deferred processing)
- [x] Content preservation during drags
- [x] Drawing context (window-relative coordinates)
- [x] Text measurement API (gfx_text_width)

## Future Enhancements

- [ ] Overlapping window redraw (z-order based)
- [ ] Close button in title bar
- [ ] Window minimize/maximize
- [ ] Window resize via drag

---

*Plan created: 2026-01-25*
*Completed: 2026-02-11 (Build 135)*
*Status: COMPLETE*
