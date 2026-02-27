# UnoDOS API Reference

All system calls are invoked via `INT 0x80` with the function index in `AH`. The kernel dispatches to the appropriate handler and returns results in registers. The carry flag (`CF`) indicates success (`CF=0`) or error (`CF=1`) for most functions.

## Calling Convention

```asm
mov ah, <function_index>   ; Set API function number
; ... set other registers as required ...
int 0x80                   ; Call kernel
; Check CF for error, read return registers
```

**Important notes:**

- `INT 0x80` clears the interrupt flag (IF). Apps must call `STI` to re-enable hardware interrupts (keyboard, mouse) after each system call.
- The kernel saves the caller's DS and ES segments internally. String pointers passed in DS:SI are read from the app's original DS. Window titles passed in ES:DI are read from the app's original ES.
- When a drawing context is active (API 31), drawing APIs 0-6, 50-52, 56-62, 65-71, 80, and 87 auto-translate BX/CX from window-relative to absolute screen coordinates.
- Only the topmost window (z-order 15) can draw pixels. Drawing calls from background windows are silently dropped.

## Color Values

CGA Mode 4, 320x200, 4 colors:

| Value | Color |
|-------|-------|
| 0 | Black |
| 1 | Green/Cyan |
| 2 | Red/Magenta |
| 3 | White |

---

## Graphics (0-6)

### API 0: gfx_draw_pixel

Draw a single pixel.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 0 |
| BX | In | X coordinate (0-319) |
| CX | In | Y coordinate (0-199) |
| AL | In | Color (0-3) |

### API 1: gfx_draw_rect

Draw a rectangle outline in white.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 1 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| SI | In | Height |

### API 2: gfx_draw_filled_rect

Draw a filled rectangle in white.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 2 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| SI | In | Height |

### API 3: gfx_draw_char

Draw a single character using the current font.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 3 |
| BX | In | X position |
| CX | In | Y position |
| AL | In | ASCII character (32-126) |

### API 4: gfx_draw_string

Draw a null-terminated string using the current foreground color.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 4 |
| BX | In | X position |
| CX | In | Y position |
| DS:SI | In | Pointer to null-terminated string |

Reads string from the caller's DS segment.

### API 5: gfx_clear_area

Clear a rectangular area to black.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 5 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| SI | In | Height |

### API 6: gfx_draw_string_inverted

Draw a string with inverted colors (black text on white background).

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 6 |
| BX | In | X position |
| CX | In | Y position |
| DS:SI | In | Pointer to null-terminated string |

---

## Memory (7-8)

### API 7: mem_alloc

Allocate a block of memory from the kernel heap.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 7 |
| AX | In | Size in bytes |
| AX | Out | Pointer to allocated block (offset in heap segment 0x1400) |
| CF | Out | 0 = success, 1 = out of memory (AX=0) |

Blocks are rounded up to 4-byte boundaries. Each block has a 4-byte header.

### API 8: mem_free

Free a previously allocated memory block.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 8 |
| AX | In | Pointer returned by mem_alloc |

---

## Events (9-10)

### API 9: event_get

Non-blocking event retrieval from the event queue.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 9 |
| AL | Out | Event type (see Event Types below) |
| DL | Out | Event data low byte |
| DH | Out | Event data high byte |
| CF | Out | 0 = event available, 1 = queue empty |

**Event Types:**

| Value | Name | DL | DH |
|-------|------|----|----|
| 0 | EVENT_NONE | - | - |
| 1 | EVENT_KEY_PRESS | ASCII code | Scan code |
| 4 | EVENT_MOUSE | Button state | - |
| 5 | EVENT_WIN_MOVED | - | - |
| 6 | EVENT_WIN_REDRAW | Window handle | - |

Only the focused task receives KEY_PRESS events.

### API 10: event_wait

Blocking event wait. Loops internally until an event is available.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 10 |
| AL | Out | Event type |
| DL | Out | Event data low byte |
| DH | Out | Event data high byte |

---

## Keyboard (11-12) - Deprecated

### API 11: kbd_getchar

Non-blocking key read. **Deprecated** - use event_get (API 9) instead.

### API 12: kbd_wait_key

Blocking key wait. **Deprecated** - use event_wait (API 10) instead.

---

## Filesystem (13-17, 27, 40, 44-47)

### API 13: fs_mount

Mount a filesystem from a drive.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 13 |
| AL | In | Drive number (0x00 = floppy A:, 0x80 = first HDD) |
| BX | Out | Mount handle (0 = FAT12, 1 = FAT16) |
| CF | Out | 0 = success, 1 = error |

### API 14: fs_open

Open a file by name.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 14 |
| BX | In | Mount handle |
| DS:SI | In | Filename in dot format, e.g. `CLOCK.BIN` |
| AX | Out | File handle (0-15) |
| CF | Out | 0 = success, 1 = error |

### API 15: fs_read

Read bytes from an open file.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 15 |
| AX | In | File handle |
| BX | In | Bytes to read |
| ES:DI | In | Destination buffer |
| AX | Out | Bytes actually read |
| CF | Out | 0 = success, 1 = error |

### API 16: fs_close

Close a file handle.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 16 |
| AX | In | File handle |
| CF | Out | 0 = success |

### API 17: fs_register_driver

Register a filesystem driver. **Reserved / not used by applications.**

### API 27: fs_readdir

Read the next directory entry.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 27 |
| BX | In | Mount handle |
| AX | In | Directory handle |
| ES:DI | Out | Pointer to 32-byte FAT directory entry |
| CF | Out | 0 = success, 1 = end of directory |

Returns standard FAT directory entries with 11-byte names in `8.3` space-padded format (no dot).

### API 40: fs_read_header

Read the first N bytes of a file (convenience wrapper for open/read/close).

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 40 |
| BX | In | Mount handle |
| DS:SI | In | Filename |
| ES:DI | In | Destination buffer |
| CX | In | Bytes to read |
| AX | Out | Bytes read |
| CF | Out | 0 = success, 1 = error |

### API 44: fs_write_sector

Write a raw 512-byte sector to disk.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 44 |
| BX | In | Mount handle |
| CX:DX | In | LBA sector number |
| ES:DI | In | Source buffer (512 bytes) |
| CF | Out | 0 = success, 1 = error |

### API 45: fs_create

Create a new file.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 45 |
| BX | In | Mount handle |
| DS:SI | In | Filename |
| AX | Out | File handle |
| CF | Out | 0 = success, 1 = error |

### API 46: fs_write

Write bytes to an open file.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 46 |
| AX | In | File handle |
| BX | In | Bytes to write |
| DS:SI | In | Source buffer |
| CF | Out | 0 = success, 1 = error |

### API 47: fs_delete

Delete a file.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 47 |
| BX | In | Mount handle |
| DS:SI | In | Filename |
| CF | Out | 0 = success, 1 = error |

**Filesystem Error Codes** (returned in AX on error):

| Code | Name |
|------|------|
| 1 | FS_ERR_NOT_FOUND |
| 2 | FS_ERR_NO_DRIVER |
| 3 | FS_ERR_READ_ERROR |
| 4 | FS_ERR_INVALID_HANDLE |
| 5 | FS_ERR_NO_HANDLES |
| 6 | FS_ERR_END_OF_DIR |
| 7 | FS_ERR_WRITE_ERROR |
| 8 | FS_ERR_DISK_FULL |
| 9 | FS_ERR_DIR_FULL |

---

## Application Loading (18-19)

### API 18: app_load

Load a .BIN file from disk into a memory segment.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 18 |
| BX | In | Mount handle |
| AL | In | Drive number (0 = boot drive) |
| DH | In | Target segment hint (0 = auto-allocate, 0x20 = shell) |
| DS:SI | In | Filename in dot format |
| AX | Out | Loaded segment address |
| CF | Out | 0 = success, 1 = error |

When DH=0, the kernel auto-allocates a segment from the dynamic pool (0x3000-0x8000).

### API 19: app_run

Execute a loaded application (blocking).

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 19 |
| AX | In | Application segment |
| AX | Out | Exit code |
| CF | Out | Status |

Performs a far call to `segment:0x0050` (the code entry point after the BIN header).

---

## Window Manager (20-26)

### API 20: win_create

Create a new window.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 20 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width (pixels) |
| SI | In | Height (pixels) |
| ES:DI | In | Window title (null-terminated, max 11 chars) |
| AL | In | Flags: 0x01 = title bar, 0x02 = border (use 0x03 for both) |
| AL | Out | Window handle (0-15) |
| CF | Out | 0 = success, 1 = no free slots |

The new window becomes topmost and receives keyboard focus.

### API 21: win_destroy

Destroy a window and free its slot.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 21 |
| AL | In | Window handle |
| CF | Out | 0 = success |

### API 22: win_draw

Redraw a window's frame and title bar.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 22 |
| AL | In | Window handle |

### API 23: win_focus

Bring a window to the front (raise z-order to topmost).

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 23 |
| AL | In | Window handle |
| CF | Out | 0 = success |

### API 24: win_move

Move a window to a new position.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 24 |
| AL | In | Window handle |
| BX | In | New X position |
| CX | In | New Y position |
| CF | Out | 0 = success |

### API 25: win_get_content

Get the content area bounds of a window (inside border and title bar).

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 25 |
| AL | In | Window handle |
| BX | Out | Content X |
| CX | Out | Content Y |
| DX | Out | Content width |
| SI | Out | Content height |

### API 26: register_shell

Register the current application as the system shell.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 26 |
| AX | In | Application segment |

Used by the launcher to register itself for auto-return on app exit.

---

## Mouse (28-30)

### API 28: mouse_get_state

Get current mouse position and button state.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 28 |
| BX | Out | X position (0-319) |
| CX | Out | Y position (0-199) |
| AL | Out | Buttons (bit 0 = left, bit 1 = right, bit 2 = middle) |

### API 29: mouse_set_position

Set the mouse cursor position.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 29 |
| BX | In | X position |
| CX | In | Y position |

### API 30: mouse_is_enabled

Check if a mouse was detected at boot.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 30 |
| AL | Out | 1 = mouse available, 0 = no mouse |

---

## Drawing Context (31-32)

### API 31: win_begin_draw

Activate drawing context for a window. All subsequent drawing APIs (0-6, 50-52) will use window-relative coordinates where (0,0) is the top-left of the content area.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 31 |
| AL | In | Window handle |
| CF | Out | 0 = success, 1 = invalid handle |

Also enables clipping to the window bounds.

### API 32: win_end_draw

Clear the drawing context and return to fullscreen (absolute) coordinates.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 32 |

---

## Text Measurement (33)

### API 33: gfx_text_width

Measure the pixel width of a string using the current font.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 33 |
| DS:SI | In | Pointer to null-terminated string |
| DX | Out | Width in pixels |

---

## Multitasking (34-36)

### API 34: app_yield

Yield the CPU to the cooperative scheduler, allowing other tasks to run.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 34 |

### API 35: app_start

Start a loaded application as a concurrent task (non-blocking).

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 35 |
| AX | In | Application segment |

### API 36: app_exit

Exit the current task.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 36 |

---

## Desktop Icons (37-39)

### API 37: desktop_set_icon

Register a desktop icon with the kernel.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 37 |
| AL | In | Slot number (0-7) |
| BX | In | X screen position |
| CX | In | Y screen position |
| DS:SI | In | Icon name string |
| ES:DI | In | Icon bitmap (64 bytes, 16x16 2bpp) |
| CF | Out | 0 = success |

### API 38: desktop_clear_icons

Clear all 8 desktop icon slots.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 38 |

### API 39: gfx_draw_icon

Draw a 16x16 icon bitmap to the screen.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 39 |
| BX | In | X position (should be divisible by 4) |
| CX | In | Y position |
| ES:DI | In | Icon bitmap (64 bytes, 2bpp CGA format) |

---

## PC Speaker (41-42)

### API 41: speaker_tone

Play a continuous tone on the PC speaker.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 41 |
| BX | In | Frequency in Hz (0 = turn off) |

### API 42: speaker_off

Turn off the PC speaker.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 42 |

The speaker is automatically silenced when a task exits.

---

## System Info (43)

### API 43: get_boot_drive

Get the BIOS drive number that the system booted from.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 43 |
| AL | Out | Drive number (0x00 = floppy A:, 0x80 = first HDD) |

---

## Font Management (48-49)

### API 48: gfx_set_font

Select the active font for all text rendering.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 48 |
| AL | In | Font index (see table below) |
| CF | Out | 0 = success, 1 = invalid index |

**Available Fonts:**

| Index | Size | Width | Height | Advance | Description |
|-------|------|-------|--------|---------|-------------|
| 0 | 4x6 | 4px | 6px | 6px | Small text |
| 1 | 8x8 | 8px | 8px | 12px | Default, window titles |
| 2 | 8x12 | 8px | 12px | 12px | Large text |

### API 49: gfx_get_font_metrics

Get metrics for a font.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 49 |
| AL | In | Font index |
| BL | Out | Width |
| BH | Out | Height |
| CL | Out | Advance (pixels between characters) |
| CF | Out | 0 = success, 1 = invalid index |

---

## GUI Toolkit (50-53)

### API 50: gfx_draw_string_wrap

Draw a string with automatic word wrapping.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 50 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Wrap width (pixels) |
| DS:SI | In | Pointer to string |
| CX | Out | Final Y position after wrapped text |

### API 51: widget_draw_button

Draw a button widget.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 51 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| SI | In | Height |
| ES:DI | In | Button label (null-terminated) |
| AL | In | Flags (bit 0 = pressed state) |

Draws a white filled rectangle with a border and centered text.

### API 52: widget_draw_radio

Draw a radio button.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 52 |
| BX | In | X position |
| CX | In | Y position |
| AL | In | State (0 = unchecked, 1 = checked) |

### API 53: widget_hit_test

Test if a point (typically mouse position) is inside a rectangle.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 53 |
| BX | In | Rectangle X |
| CX | In | Rectangle Y |
| DX | In | Rectangle width |
| SI | In | Rectangle height |
| AL | Out | 1 = hit, 0 = miss |

Coordinates are auto-translated when a drawing context is active.

---

## Theme (54-55)

### API 54: theme_set_colors

Set the global color theme.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 54 |
| AL | In | Text color (0-3) |
| BL | In | Desktop background color (0-3) |
| CL | In | Window border color (0-3) |

### API 55: theme_get_colors

Get the current color theme.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 55 |
| AL | Out | Text color |
| BL | Out | Desktop background color |
| CL | Out | Window border color |

### API 56: widget_draw_checkbox

Draw a checkbox widget with label.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 56 |
| BX | In | X position |
| CX | In | Y position |
| DS:SI | In | Label string |
| AL | In | Flags (bit 0 = checked) |

### API 57: widget_draw_textfield

Draw a text input field with optional cursor.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 57 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| DS:SI | In | Text string |
| DI | In | Cursor position |
| AL | In | Flags (bit 0 = focused, bit 1 = password mode) |

### API 58: widget_draw_scrollbar

Draw a scrollbar with thumb and arrows.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 58 |
| BX | In | X position |
| CX | In | Y position |
| SI | In | Track height |
| DX | In | Position |
| DI | In | Max range |
| AL | In | Flags (bit 0 = horizontal) |

### API 59: widget_draw_listitem

Draw a list item row with optional selection highlight.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 59 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| DS:SI | In | Text string |
| AL | In | Flags (bit 0 = selected, bit 1 = cursor marker) |

### API 60: widget_draw_progress

Draw a progress bar (8px fixed height).

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 60 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| SI | In | Value (0-100) |
| AL | In | Flags (bit 0 = show percentage text) |

### API 61: widget_draw_groupbox

Draw a group box frame with label.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 61 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| SI | In | Height |
| ES:DI | In | Label string |
| AL | In | Flags |

### API 62: widget_draw_separator

Draw a separator line.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 62 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Length |
| AL | In | Flags (bit 0 = vertical, else horizontal) |

---

## System Info (63-64)

### API 63: get_tick_count

Get the BIOS timer tick count (18.2 Hz).

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 63 |
| AX | Out | Tick count (low 16 bits, wraps at 65536) |

### API 64: point_over_window

Check if a screen point is over any visible window.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 64 |
| BX | In | X position |
| CX | In | Y position |
| AL | Out | Window handle (if hit) |
| CF | Out | 0 = over window, 1 = not over any window |

---

## Extended Widgets (65-66)

### API 65: widget_draw_combobox

Draw a dropdown combo box.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 65 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| DS:SI | In | Text string |
| AL | In | Flags (bit 0 = focused, bit 1 = open/pressed) |

### API 66: widget_draw_menubar

Draw a horizontal menu bar.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 66 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Bar width |
| DS:SI | In | Items (consecutive null-terminated strings) |
| DI | In | Item count |
| AL | In | Selected index (0xFF = none) |

---

## Colored Drawing (67-71)

### API 67: gfx_draw_filled_rect_color

Draw a filled rectangle with a specific color.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 67 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| SI | In | Height |
| AL | In | Color (0-3) |

### API 68: gfx_draw_rect_color

Draw a rectangle outline with a specific color.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 68 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| SI | In | Height |
| AL | In | Color (0-3) |

### API 69: gfx_draw_hline

Draw a horizontal line with color.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 69 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Length |
| AL | In | Color (0-3) |

### API 70: gfx_draw_vline

Draw a vertical line with color.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 70 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Height |
| AL | In | Color (0-3) |

### API 71: gfx_draw_line

Draw a line using Bresenham's algorithm.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 71 |
| BX | In | X1 |
| CX | In | Y1 |
| DX | In | X2 |
| SI | In | Y2 |
| AL | In | Color (0-3) |

---

## System APIs (72-74, 81-83)

### API 72: get_rtc_time

Read the real-time clock.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 72 |
| CH | Out | Hours (BCD) |
| CL | Out | Minutes (BCD) |
| DH | Out | Seconds (BCD) |

### API 73: delay_ticks

Delay with cooperative yield to scheduler.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 73 |
| CX | In | Ticks to wait (1 tick ~ 55ms at 18.2 Hz) |

### API 74: get_task_info

Get current task and focus information.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 74 |
| AL | Out | Current task ID |
| BL | Out | Focused task ID |
| CL | Out | Running task count |

### API 81: set_rtc_time

Set the real-time clock.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 81 |
| CH | In | Hours (BCD) |
| CL | In | Minutes (BCD) |
| DH | In | Seconds (BCD) |

### API 82: get_screen_info

Get screen dimensions and mode.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 82 |
| BX | Out | Width (320) |
| CX | Out | Height (200) |
| AL | Out | Mode (4) |
| AH | Out | Colors (4) |

### API 83: get_key_modifiers

Get keyboard modifier key states.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 83 |
| AL | Out | Shift state (1 = pressed) |
| AH | Out | Ctrl state (1 = pressed) |
| DL | Out | Alt state (1 = pressed) |

---

## Filesystem (75-77)

### API 75: fs_seek

Seek to a position in an open file.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 75 |
| AL | In | File handle |
| CX | In | Position high word |
| DX | In | Position low word |
| CF | Out | 0 = success, 1 = error |

### API 76: fs_get_file_size

Get the size of an open file.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 76 |
| AL | In | File handle |
| DX | Out | Size high word |
| AX | Out | Size low word |
| CF | Out | 0 = success, 1 = error |

### API 77: fs_rename

Rename a file.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 77 |
| BL | In | Mount handle |
| DS:SI | In | Old filename |
| ES:DI | In | New filename |
| CF | Out | 0 = success, 1 = error |

---

## Window Manager (78-79)

### API 78: win_resize

Resize an existing window.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 78 |
| AL | In | Window handle |
| DX | In | New width |
| SI | In | New height |
| CF | Out | 0 = success, 1 = error |

### API 79: win_get_info

Query window properties.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 79 |
| AL | In | Window handle |
| BX | Out | X position |
| CX | Out | Y position |
| DX | Out | Width |
| SI | Out | Height |
| DI | Out | Flags (high byte) and state (low byte) |
| CF | Out | 0 = success, 1 = error |

---

## Scroll (80)

### API 80: gfx_scroll_area

Scroll a rectangular region vertically.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 80 |
| BX | In | X position |
| CX | In | Y position |
| DX | In | Width |
| SI | In | Height |
| DI | In | Scroll pixels (positive = scroll up) |

---

## Clipboard (84-86)

### API 84: clip_copy

Copy data to the system clipboard.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 84 |
| DS:SI | In | Source data (caller's DS segment) |
| CX | In | Byte count |
| CF | Out | 0 = success, 1 = too large |

Clipboard capacity is 4KB (0x9000:0x0000-0x0FFF).

### API 85: clip_paste

Read data from the system clipboard.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 85 |
| ES:DI | In | Destination buffer (caller's ES segment) |
| CX | In | Max bytes to read |
| CX | Out | Actual bytes copied |
| CF | Out | 0 = success, 1 = clipboard empty |

### API 86: clip_get_len

Get the clipboard content length.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 86 |
| CX | Out | Clipboard length (0 = empty) |

---

## Popup Menu (87-89)

### API 87: menu_open

Open a popup menu at a position.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 87 |
| BX | In | X position (auto-translated by draw_context) |
| CX | In | Y position (auto-translated by draw_context) |
| DS:SI | In | String table (consecutive null-terminated strings) |
| DL | In | Item count |
| DH | In | Menu width (pixels) |

### API 88: menu_close

Close the active popup menu and repaint the area underneath.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 88 |

### API 89: menu_hit

Hit-test the active popup menu against the current mouse position.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 89 |
| AL | Out | Item index (0-based), or 0xFF if mouse is outside the menu |

---

## File Dialog (90)

### API 90: file_dialog_open

Open a blocking modal file picker dialog.

| Register | Direction | Description |
|----------|-----------|-------------|
| AH | In | 90 |
| BL | In | Mount handle (0 = FAT12 floppy, 1 = FAT16 hard drive) |
| ES:DI | In | Destination buffer for filename (13+ bytes) |
| CF | Out | 0 = file selected (filename at ES:DI), 1 = cancelled |

Creates a modal dialog window with a scrollable file list. Supports keyboard navigation (Up/Down/Enter/ESC) and mouse (click to select, click again or Open button to confirm, Cancel button to dismiss). The call blocks until the user selects a file or cancels.

---

## Quick Reference Table

| Index | Function | Category |
|-------|----------|----------|
| 0 | gfx_draw_pixel | Graphics |
| 1 | gfx_draw_rect | Graphics |
| 2 | gfx_draw_filled_rect | Graphics |
| 3 | gfx_draw_char | Graphics |
| 4 | gfx_draw_string | Graphics |
| 5 | gfx_clear_area | Graphics |
| 6 | gfx_draw_string_inverted | Graphics |
| 7 | mem_alloc | Memory |
| 8 | mem_free | Memory |
| 9 | event_get | Events |
| 10 | event_wait | Events |
| 11 | kbd_getchar | Keyboard (deprecated) |
| 12 | kbd_wait_key | Keyboard (deprecated) |
| 13 | fs_mount | Filesystem |
| 14 | fs_open | Filesystem |
| 15 | fs_read | Filesystem |
| 16 | fs_close | Filesystem |
| 17 | fs_register_driver | Filesystem (reserved) |
| 18 | app_load | App Loading |
| 19 | app_run | App Loading |
| 20 | win_create | Window Manager |
| 21 | win_destroy | Window Manager |
| 22 | win_draw | Window Manager |
| 23 | win_focus | Window Manager |
| 24 | win_move | Window Manager |
| 25 | win_get_content | Window Manager |
| 26 | register_shell | App Loading |
| 27 | fs_readdir | Filesystem |
| 28 | mouse_get_state | Mouse |
| 29 | mouse_set_position | Mouse |
| 30 | mouse_is_enabled | Mouse |
| 31 | win_begin_draw | Drawing Context |
| 32 | win_end_draw | Drawing Context |
| 33 | gfx_text_width | Graphics |
| 34 | app_yield | Multitasking |
| 35 | app_start | Multitasking |
| 36 | app_exit | Multitasking |
| 37 | desktop_set_icon | Desktop |
| 38 | desktop_clear_icons | Desktop |
| 39 | gfx_draw_icon | Graphics |
| 40 | fs_read_header | Filesystem |
| 41 | speaker_tone | Audio |
| 42 | speaker_off | Audio |
| 43 | get_boot_drive | System |
| 44 | fs_write_sector | Filesystem |
| 45 | fs_create | Filesystem |
| 46 | fs_write | Filesystem |
| 47 | fs_delete | Filesystem |
| 48 | gfx_set_font | Graphics |
| 49 | gfx_get_font_metrics | Graphics |
| 50 | gfx_draw_string_wrap | GUI Toolkit |
| 51 | widget_draw_button | GUI Toolkit |
| 52 | widget_draw_radio | GUI Toolkit |
| 53 | widget_hit_test | GUI Toolkit |
| 54 | theme_set_colors | Theme |
| 55 | theme_get_colors | Theme |
| 56 | widget_draw_checkbox | GUI Toolkit |
| 57 | widget_draw_textfield | GUI Toolkit |
| 58 | widget_draw_scrollbar | GUI Toolkit |
| 59 | widget_draw_listitem | GUI Toolkit |
| 60 | widget_draw_progress | GUI Toolkit |
| 61 | widget_draw_groupbox | GUI Toolkit |
| 62 | widget_draw_separator | GUI Toolkit |
| 63 | get_tick_count | System |
| 64 | point_over_window | Window Manager |
| 65 | widget_draw_combobox | GUI Toolkit |
| 66 | widget_draw_menubar | GUI Toolkit |
| 67 | gfx_draw_filled_rect_color | Colored Drawing |
| 68 | gfx_draw_rect_color | Colored Drawing |
| 69 | gfx_draw_hline | Colored Drawing |
| 70 | gfx_draw_vline | Colored Drawing |
| 71 | gfx_draw_line | Colored Drawing |
| 72 | get_rtc_time | System |
| 73 | delay_ticks | System |
| 74 | get_task_info | System |
| 75 | fs_seek | Filesystem |
| 76 | fs_get_file_size | Filesystem |
| 77 | fs_rename | Filesystem |
| 78 | win_resize | Window Manager |
| 79 | win_get_info | Window Manager |
| 80 | gfx_scroll_area | Graphics |
| 81 | set_rtc_time | System |
| 82 | get_screen_info | System |
| 83 | get_key_modifiers | System |
| 84 | clip_copy | Clipboard |
| 85 | clip_paste | Clipboard |
| 86 | clip_get_len | Clipboard |
| 87 | menu_open | Popup Menu |
| 88 | menu_close | Popup Menu |
| 89 | menu_hit | Popup Menu |
| 90 | file_dialog_open | File Dialog |

---

*UnoDOS v3.22.0 Build 351 - 96 API functions (indices 0-95)*
