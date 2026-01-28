# UnoDOS 3 Features Roadmap

This document outlines the feature status and roadmap for UnoDOS 3, designed for IBM PC XT-compatible hardware and tested on the HP Omnibook 600C and Asus EeePC.

## Hardware Target Summary

| Component | Specification |
|-----------|---------------|
| CPU | Intel 8088/8086 (4.77-10 MHz) to 486 |
| RAM | 128 KB minimum, 640 KB typical |
| Display | CGA 320x200 4-color (also works on VGA in CGA mode) |
| Storage | 360KB or 1.44MB floppy disk |
| Input | PC/XT keyboard, PS/2 mouse (optional) |
| Audio | PC speaker (optional) |

---

## Completed Features (v3.12.0 Build 053)

### Boot System
- [x] Three-stage boot loader (boot sector + stage2 + kernel)
- [x] Memory detection (BIOS INT 12h)
- [x] Video adapter detection (CGA/EGA/VGA)
- [x] Boot progress indicator (dots during kernel load)
- [x] Signature verification at each stage
- [x] Build number display for debugging

### Graphics System
- [x] CGA 320x200 4-color mode
- [x] Custom 8x8 bitmap font (95 ASCII characters)
- [x] Custom 4x6 small font (95 ASCII characters)
- [x] Pixel plotting routines
- [x] Rectangle drawing (outline and filled)
- [x] String rendering

### System Call Infrastructure (Foundation 1.1)
- [x] INT 0x80 handler for API dispatch
- [x] Kernel API table at fixed address (0x1000:0x0B00)
- [x] 30 API functions implemented (indices 0-29)
- [x] Caller segment preservation for string parameters

### Graphics API (Foundation 1.2)
- [x] gfx_draw_pixel - Plot single pixel
- [x] gfx_draw_rect - Rectangle outline
- [x] gfx_draw_filled_rect - Filled rectangle
- [x] gfx_draw_char - Single character
- [x] gfx_draw_string - Null-terminated string
- [x] gfx_clear_area - Clear rectangular region

### Memory Allocator (Foundation 1.3)
- [x] malloc(size) - Allocate memory dynamically
- [x] free(ptr) - Free allocated memory
- [x] First-fit allocation algorithm
- [x] Heap at 0x1400:0000 (~532KB available)

### Keyboard Driver (Foundation 1.4)
- [x] INT 09h keyboard interrupt handler
- [x] Scan code to ASCII translation (normal + shifted)
- [x] Modifier key tracking (Shift, Ctrl, Alt)
- [x] 16-byte circular buffer
- [x] kbd_getchar() - Non-blocking input
- [x] kbd_wait_key() - Blocking input

### Event System (Foundation 1.5)
- [x] Circular event queue (32 events)
- [x] Event types: KEY_PRESS, KEY_RELEASE, TIMER, MOUSE
- [x] event_get_stub() - Non-blocking event retrieval
- [x] event_wait_stub() - Blocking event wait
- [x] Keyboard integration (posts KEY_PRESS events)
- [x] Mouse integration (posts MOUSE events)

### PS/2 Mouse Driver (Foundation 1.7) - NEW Build 053
- [x] INT 0x74 (IRQ12) mouse interrupt handler
- [x] 8042 keyboard controller interface (ports 0x60/0x64)
- [x] 3-byte packet protocol parsing
- [x] Automatic mouse detection at boot
- [x] Position tracking (0-319 X, 0-199 Y)
- [x] Button state tracking (left, right, middle)
- [x] mouse_get_state() - Get position and buttons
- [x] mouse_set_position() - Set cursor position
- [x] mouse_is_enabled() - Check mouse availability
- [x] Graceful degradation when no mouse present

### Filesystem (Foundation 1.6)
- [x] Filesystem abstraction layer (VFS-like)
- [x] FAT12 driver (read-only)
- [x] fs_mount_stub() - Mount filesystem
- [x] fs_open_stub() - Open file by name
- [x] fs_read_stub() - Read file contents
- [x] fs_close_stub() - Close file handle
- [x] **fs_readdir_stub() - Directory iteration (NEW Build 042)**
- [x] Multi-cluster file reading (FAT chain following)
- [x] 8.3 filename support

### Application Loader (Core Services 2.1)
- [x] app_load_stub() - Load .BIN from FAT12
- [x] app_run_stub() - Execute loaded application
- [x] Dual segment architecture (shell 0x2000, user 0x3000)
- [x] Far CALL/RETF calling convention
- [x] Apps survive shell execution and return

### Window Manager (Core Services 2.2)
- [x] win_create_stub() - Create window with title/border
- [x] win_destroy_stub() - Destroy window
- [x] win_draw_stub() - Draw window frame
- [x] win_focus_stub() - Set active window
- [x] win_move_stub() - Move window
- [x] win_get_content_stub() - Get content area bounds

### Desktop Launcher (Core Services 2.3)
- [x] **Dynamic app discovery - scans floppy for .BIN files (NEW Build 042)**
- [x] Window-based menu UI
- [x] W/S navigation, Enter to launch, ESC to exit
- [x] Automatic return after app exits
- [x] Skips LAUNCHER.BIN in menu display

### Applications
- [x] **LAUNCHER.BIN** - Desktop launcher (1061 bytes)
- [x] **CLOCK.BIN** - Real-time clock display (249 bytes)
- [x] **BROWSER.BIN** - File browser showing files with sizes (564 bytes)
- [x] **TEST.BIN** - Hello test application (112 bytes)
- [x] **MOUSE.BIN** - Mouse test/demo application (578 bytes) - NEW Build 053

---

## API Table Summary (v3.12.0)

| Index | Function | Description |
|-------|----------|-------------|
| 0 | gfx_draw_pixel | Plot single pixel |
| 1 | gfx_draw_rect | Draw rectangle outline |
| 2 | gfx_draw_filled_rect | Draw filled rectangle |
| 3 | gfx_draw_char | Draw single character |
| 4 | gfx_draw_string | Draw null-terminated string |
| 5 | gfx_clear_area | Clear rectangular area |
| 6 | mem_alloc | Allocate memory |
| 7 | mem_free | Free memory |
| 8 | event_get | Get event (non-blocking) |
| 9 | event_wait | Wait for event (blocking) |
| 10 | kbd_getchar | Get key (non-blocking) |
| 11 | kbd_wait_key | Wait for key (blocking) |
| 12 | fs_mount | Mount filesystem |
| 13 | fs_open | Open file |
| 14 | fs_read | Read file |
| 15 | fs_close | Close file |
| 16 | fs_register_driver | Register filesystem driver |
| 17 | app_load | Load application |
| 18 | app_run | Run application |
| 19 | win_create | Create window |
| 20 | win_destroy | Destroy window |
| 21 | win_draw | Draw window |
| 22 | win_focus | Focus window |
| 23 | win_move | Move window |
| 24 | win_get_content | Get content area |
| 25 | register_shell | Register shell callback |
| 26 | fs_readdir | Read directory entry |
| **27** | **mouse_get_state** | **Get mouse position/buttons (NEW)** |
| **28** | **mouse_set_position** | **Set mouse cursor position (NEW)** |
| **29** | **mouse_is_enabled** | **Check if mouse available (NEW)** |

---

## Memory Layout (v3.12.0)

```
0x0000:0x0000   Interrupt Vector Table        1 KB
0x0000:0x0400   BIOS Data Area                256 bytes
0x0000:0x7C00   Boot sector                   512 bytes
0x0800:0x0000   Stage2 loader                 2 KB
0x1000:0x0000   Kernel                        28 KB
  +-- 0x1000:0x0B00  API table (256 bytes)
0x1400:0x0000   Heap (malloc pool)            ~532 KB
0x2000:0x0000   Shell/Launcher segment        64 KB
0x3000:0x0000   User app segment              64 KB
0xB800:0x0000   CGA video memory              16 KB
```

---

## Planned Features

### Standard Library (v3.14.0)
- [ ] graphics.lib - C-callable wrappers for Graphics API
- [ ] unodos.lib - Initialization and utility functions
- [ ] C compiler support (Turbo C, OpenWatcom)

### Additional Applications
- [ ] Text editor
- [ ] Calculator
- [ ] Settings/Control Panel
- [ ] Simple games

### Hardware Support
- [x] PS/2 mouse driver (COMPLETE - Build 053)
- [ ] Serial mouse driver (Microsoft compatible)
- [ ] Sound support (PC speaker beeps/tunes)
- [ ] FAT16 driver (for hard drives)
- [ ] Hard drive boot support

### Future Enhancements
- [ ] Cooperative multitasking (app_yield)
- [ ] Inter-app messaging
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
| Available for apps (360KB) | ~329 KB |
| Available for apps (1.44MB) | ~1.41 MB |

---

## Hardware Compatibility

### Tested Hardware
- **HP Omnibook 600C** (486DX4-75, VGA with CGA emulation, 1.44MB floppy)
- **Asus EeePC 1005PEB** (Atom, external USB floppy)
- **QEMU** (PC/XT emulation mode)

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
- **Preemptive Multitasking**: Single app or cooperative only
- **Protected Mode**: Stays in real mode for XT compatibility
- **High Resolution**: Sticks to CGA modes for compatibility
- **Modern File Systems**: No FAT32, NTFS, ext4
- **DOS Compatibility**: Not a DOS clone; different API

---

*Document version: 3.2 (2026-01-28) - Updated for v3.12.0 Build 053 - PS/2 Mouse Driver*
