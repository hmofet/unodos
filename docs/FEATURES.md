# UnoDOS 3 Features Roadmap

This document outlines the feature status and roadmap for UnoDOS 3, designed for IBM PC XT-compatible hardware and tested on the HP Omnibook 600C.

## Hardware Target Summary

| Component | Specification |
|-----------|---------------|
| CPU | Intel 8088/8086 (4.77-10 MHz) to 486 |
| RAM | 128 KB minimum, 640 KB typical |
| Display | CGA 320x200 4-color (also works on VGA in CGA mode) |
| Storage | 360KB or 1.44MB floppy disk |
| Input | PC/XT keyboard, optional serial mouse |
| Audio | PC speaker (optional) |

---

## Completed Features (v3.11.0)

### Boot System
- [x] Three-stage boot loader (boot sector + stage2 + kernel)
- [x] Memory detection (BIOS INT 12h)
- [x] Video adapter detection (CGA/EGA/VGA)
- [x] Boot progress indicator (dots during kernel load)
- [x] Signature verification at each stage

### Graphics System
- [x] CGA 320x200 4-color mode
- [x] Custom 8x8 bitmap font (95 ASCII characters)
- [x] Custom 4x6 small font (95 ASCII characters)
- [x] Pixel plotting routines
- [x] Rectangle drawing (outline and filled)
- [x] String rendering

### System Call Infrastructure (Foundation 1.1)
- [x] INT 0x80 handler for API discovery
- [x] Kernel API table at fixed address (0x1000:0x0800)
- [x] Hybrid approach: INT for discovery, far call for execution
- [x] 19 API functions implemented

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

### Filesystem (Foundation 1.6)
- [x] Filesystem abstraction layer (VFS-like)
- [x] FAT12 driver (read-only)
- [x] fs_mount_stub() - Mount filesystem
- [x] fs_open_stub() - Open file by name
- [x] fs_read_stub() - Read file contents
- [x] fs_close_stub() - Close file handle
- [x] Multi-cluster file reading (FAT chain following)
- [x] 8.3 filename support

### Application Loader (Core Services 2.1)
- [x] app_load_stub() - Load .BIN from FAT12 to heap
- [x] app_run_stub() - Execute loaded application
- [x] App table (16 entries, 32 bytes each)
- [x] Far CALL/RETF calling convention
- [x] BIOS drive number support (A:, B:, HDD)
- [x] Test application framework (HELLO.BIN)

### User Interface
- [x] Graphical welcome screen with bordered window
- [x] Real-time clock display (RTC)
- [x] RAM status display
- [x] Interactive keyboard demo
- [x] 'L' key to trigger app loader test

---

## In Progress (v3.12.0+)

### Window Manager (Core Services 2.2)
- [ ] Window structure (position, size, title, content)
- [ ] Window drawing (title bar, borders, content area)
- [ ] Window stacking (Z-order)
- [ ] Active window highlighting
- [ ] Window moving/resizing

### App Management
- [ ] app_unload_stub() - Free app memory
- [ ] app_get_info_stub() - Query app state

---

## Planned Features

### Standard Library (v3.14.0)
- [ ] graphics.lib - C-callable wrappers for Graphics API
- [ ] unodos.lib - Initialization and utility functions
- [ ] C compiler support (Turbo C, OpenWatcom)

### Applications
- [ ] Clock display application
- [ ] Text editor
- [ ] File manager
- [ ] Calculator
- [ ] Settings/Control Panel

### Hardware Support
- [ ] Mouse driver (Microsoft serial mouse)
- [ ] Sound support (PC speaker beeps/tunes)
- [ ] FAT16 driver (for hard drives)
- [ ] Hard drive boot support

### Future Enhancements
- [ ] Cooperative multitasking (app_yield)
- [ ] Inter-app messaging
- [ ] File writing support
- [ ] Long filename support

---

## API Table Summary (v3.11.0)

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

---

## Memory Layout (v3.11.0)

```
0x0000:0x0000   Interrupt Vector Table        1 KB
0x0000:0x0400   BIOS Data Area                256 bytes
0x0000:0x7C00   Boot sector                   512 bytes
0x0800:0x0000   Stage2 loader                 2 KB
0x1000:0x0000   Kernel                        28 KB
  └─ 0x1000:0x0800  API table (256 bytes)
0x1400:0x0000   Heap (malloc pool)            ~532 KB
0xB800:0x0000   CGA video memory              16 KB
```

---

## Size Budget

| Component | Size |
|-----------|------|
| Boot sector | 512 bytes |
| Stage 2 loader | 2 KB |
| Kernel | 28 KB |
| Fonts (8x8 + 4x6) | ~1.5 KB (in kernel) |
| **Total boot code** | **~30.5 KB** |
| Available for apps (360KB) | ~329 KB |
| Available for apps (1.44MB) | ~1.41 MB |

---

## Hardware Compatibility

### Tested Hardware
- **HP Omnibook 600C** (486DX4-75, VGA with CGA emulation, 1.44MB floppy)
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

*Document version: 3.0 (2026-01-25) - Updated for v3.11.0*
