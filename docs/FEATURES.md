# UnoDOS 3 Features Roadmap

This document outlines the feature status and roadmap for UnoDOS 3, designed for IBM PC XT-compatible hardware and tested on the HP Omnibook 600C.

## Hardware Target Summary

| Component | Specification |
|-----------|---------------|
| CPU | Intel 8088/8086 (4.77-10 MHz) to 486 |
| RAM | 128 KB minimum, 256 KB recommended |
| Display | CGA 320x200 4-color (also works on VGA in CGA mode) |
| Storage | 360KB or 1.44MB floppy disk |
| Input | PC/XT keyboard, PS/2 mouse (optional) |
| Audio | PC speaker (optional) |

---

## Completed Features (v3.14.0 Build 151)

### Boot System
- [x] Three-stage boot loader (boot sector + stage2 + kernel)
- [x] Memory detection (BIOS INT 12h)
- [x] Video adapter detection (CGA/EGA/VGA)
- [x] Boot progress indicator (dots during kernel load)
- [x] Signature verification at each stage
- [x] Build number display for debugging

### Graphics System
- [x] CGA 320x200 4-color mode
- [x] Custom 8x8 bitmap font (95 ASCII characters, 12px advance)
- [x] Custom 4x6 small font (95 ASCII characters)
- [x] Pixel plotting routines (white, black, XOR)
- [x] Rectangle drawing (outline and filled)
- [x] String rendering (normal and inverted/white)
- [x] Text width measurement

### System Call Infrastructure
- [x] INT 0x80 handler for API dispatch
- [x] Kernel API table at fixed address (0x1000:0x1060)
- [x] 41 API functions implemented (indices 0-40)
- [x] Caller segment preservation (caller_ds/caller_es) for string parameters
- [x] Window-relative coordinate translation for APIs 0-6

### Graphics API
- [x] gfx_draw_pixel (0) - Plot single pixel
- [x] gfx_draw_rect (1) - Rectangle outline
- [x] gfx_draw_filled_rect (2) - Filled rectangle
- [x] gfx_draw_char (3) - Single character
- [x] gfx_draw_string (4) - Null-terminated string (black on background)
- [x] gfx_clear_area (5) - Clear rectangular region
- [x] gfx_draw_string_inverted (6) - Null-terminated string (white/inverted)

### Memory Allocator
- [x] malloc(size) - Allocate memory dynamically
- [x] free(ptr) - Free allocated memory
- [x] First-fit allocation algorithm
- [x] Heap at 0x1400:0000 (~532KB available)

### Keyboard Driver
- [x] INT 09h keyboard interrupt handler
- [x] Scan code to ASCII translation (normal + shifted)
- [x] Modifier key tracking (Shift, Ctrl, Alt)
- [x] Arrow key support (Up=128, Down=129)
- [x] 16-byte circular buffer
- [x] kbd_getchar() - Non-blocking input
- [x] kbd_wait_key() - Blocking input

### Event System
- [x] Circular event queue (32 events)
- [x] Event types: KEY_PRESS, KEY_RELEASE, TIMER, MOUSE
- [x] event_get_stub() - Non-blocking event retrieval
- [x] event_wait_stub() - Blocking event wait
- [x] Keyboard integration (posts KEY_PRESS events)
- [x] Mouse integration (posts MOUSE events)

### PS/2 Mouse Driver
- [x] INT 0x74 (IRQ12) mouse interrupt handler
- [x] 8042 keyboard controller interface (ports 0x60/0x64)
- [x] 3-byte packet protocol parsing
- [x] Automatic mouse detection at boot
- [x] Position tracking (0-319 X, 0-199 Y)
- [x] Button state tracking (left, right, middle)
- [x] XOR sprite cursor (8x10 arrow, self-erasing)
- [x] Cursor hide/show with lock flag for flicker-free rendering
- [x] mouse_get_state() - Get position and buttons
- [x] mouse_set_position() - Set cursor position
- [x] mouse_is_enabled() - Check mouse availability

### Window Manager
- [x] win_create_stub() - Create window with title/border
- [x] win_destroy_stub() - Destroy window and clear area
- [x] win_draw_stub() - Redraw window frame
- [x] win_focus_stub() - Set active window (z-order)
- [x] win_move_stub() - Move window to new position
- [x] win_get_content_stub() - Get content area bounds
- [x] Mouse-driven title bar dragging (deferred via event_get)
- [x] OS-managed content preservation during drags (scratch buffer at 0x9000)
- [x] Window drawing context (apps use window-relative 0,0 coordinates)
- [x] Automatic coordinate translation for APIs 0-6

### Filesystem
- [x] Filesystem abstraction layer (VFS-like)
- [x] FAT12 driver (read-only)
- [x] fs_mount_stub() - Mount filesystem
- [x] fs_open_stub() - Open file by name
- [x] fs_read_stub() - Read file contents
- [x] fs_close_stub() - Close file handle
- [x] fs_readdir_stub() - Directory iteration
- [x] Multi-cluster file reading (FAT chain following)
- [x] 8.3 filename support

### Application Loader & Multitasking
- [x] app_load_stub() - Load .BIN from FAT12 with dynamic segment allocation
- [x] app_run_stub() - Execute loaded application
- [x] app_start_stub() - Start app non-blocking (cooperative multitasking)
- [x] Multi-app segment pool (shell 0x2000 fixed, user 0x3000-0x8000 dynamic)
- [x] Up to 6 concurrent user apps + launcher
- [x] Dynamic segment allocation (alloc_segment / free_segment)
- [x] Automatic segment cleanup on task exit
- [x] Far CALL/RETF calling convention
- [x] Cooperative multitasking with round-robin scheduler

### Desktop & Icons
- [x] Fullscreen desktop (no window, draws directly to screen)
- [x] 4x2 icon grid with 16x16 2bpp CGA icon bitmaps
- [x] BIN file icon headers (80 bytes: JMP + "UI" magic + name + bitmap)
- [x] Automatic icon detection from BIN headers at boot
- [x] Default icon for legacy apps without headers
- [x] Mouse double-click to launch apps (~0.5s threshold)
- [x] Keyboard icon navigation (arrows/WASD + Enter)
- [x] Icon selection highlight (white rectangle border)
- [x] Floppy disk swap detection (~2 second polling via INT 13h)
- [x] Kernel-managed desktop repaint during window operations
- [x] Desktop icon registration API for kernel-level repaint support

### Desktop Launcher
- [x] Dynamic app discovery - scans floppy for .BIN files
- [x] Reads icon headers from BIN files (API 40: fs_read_header)
- [x] Icon grid display with app names
- [x] Arrow/WASD navigation, Enter or double-click to launch
- [x] Skips LAUNCHER.BIN in icon display
- [x] Auto-rescans disk on floppy swap

### Applications
- [x] **LAUNCHER.BIN** - Desktop launcher with icon grid (2769 bytes)
- [x] **CLOCK.BIN** - Real-time clock display (330 bytes, icon: clock face)
- [x] **BROWSER.BIN** - File browser showing files with sizes (565 bytes, icon: folder)
- [x] **TEST.BIN** - Hello test application (247 bytes, icon: speech bubble)
- [x] **MOUSE.BIN** - Mouse test/demo application (741 bytes, icon: arrow cursor)

---

## API Table Summary (v3.14.0 Build 151)

| Index | Function | Description |
|-------|----------|-------------|
| 0 | gfx_draw_pixel | Plot single pixel |
| 1 | gfx_draw_rect | Draw rectangle outline |
| 2 | gfx_draw_filled_rect | Draw filled rectangle |
| 3 | gfx_draw_char | Draw single character |
| 4 | gfx_draw_string | Draw null-terminated string (black) |
| 5 | gfx_clear_area | Clear rectangular area |
| 6 | gfx_draw_string_inverted | Draw string (white/inverted) |
| 7 | mem_alloc | Allocate memory |
| 8 | mem_free | Free memory |
| 9 | event_get | Get event (non-blocking) |
| 10 | event_wait | Wait for event (blocking) |
| 11 | kbd_getchar | Get key (non-blocking) |
| 12 | kbd_wait_key | Wait for key (blocking) |
| 13 | fs_mount | Mount filesystem |
| 14 | fs_open | Open file |
| 15 | fs_read | Read file |
| 16 | fs_close | Close file |
| 17 | fs_register_driver | Register filesystem driver |
| 18 | app_load | Load application |
| 19 | app_run | Run application (blocking) |
| 20 | win_create | Create window |
| 21 | win_destroy | Destroy window |
| 22 | win_draw | Draw window frame |
| 23 | win_focus | Focus window |
| 24 | win_move | Move window |
| 25 | win_get_content | Get content area |
| 26 | register_shell | Register shell callback |
| 27 | fs_readdir | Read directory entry |
| 28 | mouse_get_state | Get mouse position/buttons |
| 29 | mouse_set_position | Set mouse cursor position |
| 30 | mouse_is_enabled | Check if mouse available |
| 31 | win_begin_draw | Set window drawing context |
| 32 | win_end_draw | Clear drawing context |
| 33 | gfx_text_width | Measure string width in pixels |
| 34 | app_yield | Yield to scheduler (cooperative multitasking) |
| 35 | app_start | Start loaded app (non-blocking) |
| 36 | app_get_state | Get app running state |
| 37 | desktop_set_icon | Register desktop icon with kernel |
| 38 | desktop_clear_icons | Clear all registered desktop icons |
| 39 | gfx_draw_icon | Draw 16x16 2bpp icon bitmap |
| 40 | fs_read_header | Read first N bytes from a file |

### New API Details (Build 144)

**API 34: app_yield** - Yield to scheduler
- Input: none
- Output: none
- Description: Yields CPU to the cooperative scheduler, allowing other tasks to run.

**API 35: app_start** - Start loaded app (non-blocking)
- Input: AL = app handle (from app_load)
- Output: CF=0 success, CF=1 error
- Description: Starts a loaded application as a cooperative task. The caller continues execution.

**API 37: desktop_set_icon** - Register desktop icon
- Input: AL = slot (0-7), BX = X screen position, CX = Y screen position, SI -> 76 bytes (64B bitmap + 12B name) in caller's DS
- Output: CF=0 success, CF=1 invalid slot
- Description: Registers an icon with the kernel for automatic desktop repaint during window operations.

**API 38: desktop_clear_icons** - Clear all icons
- Input: none
- Output: none
- Description: Zeros all 8 icon slots and resets the icon count. Call before rescanning disk.

**API 39: gfx_draw_icon** - Draw 16x16 icon bitmap
- Input: BX = X position (should be divisible by 4), CX = Y position, SI -> 64-byte bitmap in caller's DS
- Output: none
- Description: Draws a 16x16 2bpp icon to CGA video memory, handling even/odd row interlacing.

**API 40: fs_read_header** - Read file header bytes
- Input: BX = mount handle, SI -> filename (null-terminated, in caller's segment), ES:DI = destination buffer, CX = bytes to read
- Output: CF=0 success (AX = bytes read), CF=1 error
- Description: Opens a file, reads CX bytes, closes it. Designed for reading BIN icon headers without manual open/read/close.

---

## BIN File Icon Header Format

Applications can embed a 16x16 icon and display name in an 80-byte header at the start of the BIN file. The kernel and desktop launcher use this header to display app icons.

### Header Layout (80 bytes, 0x00-0x4F)

```
Offset  Size   Description
0x00    2      JMP short 0x50 (bytes: 0xEB, 0x4E) - skip to code entry
0x02    2      Magic: "UI" (0x55, 0x49) - identifies icon header
0x04    12     App display name (null-padded ASCII, e.g. "Clock\0\0...")
0x10    64     Icon bitmap: 16x16 pixels, 2 bits per pixel, CGA format
               16 rows x 4 bytes/row, top-to-bottom, MSB first
0x50    ...    Code entry point (pusha, push ds, push es, ...)
```

### Detection Algorithm

Read first 4 bytes of each BIN file:
- If `byte[0] == 0xEB` AND `byte[2] == 'U'` AND `byte[3] == 'I'` -> has icon header
- Otherwise -> legacy app (use default icon, derive name from FAT filename)

Old apps start with `0x60` (pusha), never `0xEB`, so detection is safe.

### Icon Bitmap Format

The 64-byte bitmap is a 16x16 image in CGA 2bpp format:
- 4 bytes per row, 16 rows
- Each byte contains 4 pixels (2 bits each)
- Bit layout per byte: `[px0:7-6] [px1:5-4] [px2:3-2] [px3:1-0]`
- MSB = leftmost pixel

CGA Palette 1 color values:
- `00` = Black (background/transparent)
- `01` = Cyan
- `10` = Magenta
- `11` = White

### Example Header (NASM)

```asm
[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'MyApp', 0                   ; App name (12 bytes, null-padded)
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    db 0xFF, 0xFF, 0xFF, 0xFF      ; Row 0:  all white pixels
    db 0xC0, 0x00, 0x00, 0x03      ; Row 1:  white border, black inside
    ; ... 14 more rows (4 bytes each) ...
    db 0xFF, 0xFF, 0xFF, 0xFF      ; Row 15: all white pixels

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---
entry:
    pusha
    push ds
    push es
    ; ... app code here ...
    pop es
    pop ds
    popa
    retf
```

### Icon Design Guidelines

- Use white (color 3) for outlines and main shapes - most visible on black desktop
- Use black (color 0) for background/transparent areas
- Cyan and magenta can be used for accents but are less visible
- Keep designs simple - 16x16 at 2bpp is very low resolution
- Icon X position should be divisible by 4 for byte alignment (the grid handles this)

---

## Memory Layout (v3.14.0 Build 149+)

```
0x0000:0x0000   Interrupt Vector Table        1 KB
0x0000:0x0400   BIOS Data Area                256 bytes
0x0000:0x7C00   Boot sector                   512 bytes
0x0800:0x0000   Stage2 loader                 2 KB
0x1000:0x0000   Kernel                        28 KB
  +-- 0x1000:0x1060  API table (41 functions)
0x1400:0x0000   Heap (malloc pool)            ~64 KB
0x2000:0x0000   Shell/Launcher segment        64 KB (fixed)
0x3000:0x0000   User app slot 0               64 KB (dynamic pool)
0x4000:0x0000   User app slot 1               64 KB
0x5000:0x0000   User app slot 2               64 KB
0x6000:0x0000   User app slot 3               64 KB
0x7000:0x0000   User app slot 4               64 KB
0x8000:0x0000   User app slot 5               64 KB
0x9000:0x0000   Scratch buffer (drag content) 64 KB
0xB800:0x0000   CGA video memory              16 KB
```

---

## Planned Features

### Additional Applications
- [ ] Text editor
- [ ] Calculator
- [ ] Settings/Control Panel

### Window Manager Enhancements
- [x] Overlapping window redraw (z-order clipping)
- [ ] Close button
- [ ] Window resize

### Hardware Support
- [ ] Sound support (PC speaker beeps/tunes)
- [ ] Serial mouse driver (Microsoft compatible)

### Future Enhancements
- [x] Cooperative multitasking (app_yield, app_start, 6 concurrent apps)
- [ ] File writing support
- [ ] Long filename support

---

## Size Budget

| Component | Size |
|-----------|------|
| Boot sector | 512 bytes |
| Stage 2 loader | 2 KB |
| Kernel | ~28 KB |
| Fonts (8x8 + 4x6) | ~1.5 KB (in kernel) |
| **Total boot code** | **~30.5 KB** |
| Available for apps (1.44MB) | ~1.41 MB |

---

## Hardware Compatibility

### Tested Hardware
- **HP Omnibook 600C** (486DX4-75, VGA with CGA emulation, 1.44MB floppy)
- **QEMU** (PC/XT emulation mode, `-M isapc`)

### Display Compatibility
CGA 320x200 mode works on:
- Original CGA cards
- EGA cards (backward compatible)
- VGA cards (backward compatible)
- Modern VGA implementations

### Color Palette (Palette 1)
- Background: Blue (configurable)
- Color 1: Cyan
- Color 2: Magenta
- Color 3: White

---

## Non-Goals

The following are explicitly out of scope for UnoDOS 3:

- **Networking**: No TCP/IP, no modem support
- **Preemptive Multitasking**: Cooperative only (apps must yield)
- **Protected Mode**: Stays in real mode for XT compatibility
- **High Resolution**: Sticks to CGA modes for compatibility
- **Modern File Systems**: No FAT32, NTFS, ext4
- **DOS Compatibility**: Not a DOS clone; different API

---

*Document version: 6.0 (2026-02-13) - Updated for v3.14.0 Build 151*
