# UnoDOS 3 Features

## Hardware Target

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| CPU | Intel 8088 @ 4.77 MHz | 8086/80286+ |
| RAM | 128 KB | 256 KB+ |
| Display | CGA (320x200, 4-color) | CGA/EGA/VGA |
| Storage | 360KB floppy | 1.44MB floppy, HDD, CF card, USB |
| Input | PC/XT keyboard | PS/2 mouse (optional) |
| Audio | PC speaker (optional) | |

---

## Completed Features (v3.22.0)

### Boot System
- Three-stage floppy boot: boot.asm -> stage2.asm -> kernel (FAT12)
- Hard drive boot: mbr.asm -> vbr.asm -> stage2_hd.asm -> kernel (FAT16)
- Memory and video adapter detection
- Boot drive auto-detection (floppy, HDD, CF card, USB)
- Build number display for version verification

### Graphics System
- CGA 320x200 4-color mode
- Three bitmap fonts: 4x6 (small), 8x8 (default), 8x14 (large)
- Pixel plotting, rectangle drawing (outline and filled), area clear
- String rendering (normal, inverted, word-wrapped)
- Colored drawing: filled rect, outline rect, hline, vline, line (Bresenham's)
- Text width measurement
- 16x16 2bpp icon rendering
- Character-level clipping within window bounds
- Scroll area (vertical pixel scrolling)

### System Call Infrastructure
- INT 0x80 handler with 91 API functions (indices 0-90)
- Caller segment preservation for cross-segment string access
- Window-relative coordinate auto-translation
- Z-order clipping (only topmost window renders pixels)

### Memory Management
- First-fit heap allocator (malloc/free)
- Heap at segment 0x1400

### Input
- INT 09h keyboard driver with scan code translation, modifier tracking
- Keyboard modifier query API (Shift/Ctrl/Alt states)
- PS/2 mouse driver: BIOS INT 15h/C2 (primary) + direct KBC (fallback)
- USB mouse support via BIOS legacy emulation
- XOR sprite cursor (8x10 arrow, self-erasing)
- Boot-time mouse auto-detection

### Event System
- 32-event circular queue
- Event types: KEY_PRESS, MOUSE, WIN_MOVED, WIN_REDRAW
- Per-task keyboard event filtering (only focused app receives keys)

### Window Manager
- Create, destroy, draw, focus, move, resize windows (16 max)
- Close button [X] on title bar
- Mouse-driven outline drag (Windows 3.1 style)
- Z-order management with active/inactive title bars
- Drawing context API (window-relative coordinates)
- Automatic coordinate translation for all drawing APIs
- Window info query API

### Filesystem
- VFS-like abstraction layer routing FAT12/FAT16 by drive type
- FAT12 driver (floppy, read/write)
- FAT16 driver (hard drive, read/write)
- File operations: mount, open, read, write, create, delete, close, readdir, seek, rename
- File size query
- Multi-cluster file reading with FAT chain following

### Multitasking
- Cooperative round-robin scheduler
- Up to 6 concurrent user apps + launcher
- Dynamic segment allocation from pool (0x3000-0x8000)
- Automatic segment cleanup on task exit
- Task info query API

### Desktop
- Fullscreen desktop with 4x3 icon grid (12 icons)
- BIN file icon headers (80 bytes: JMP + "UI" magic + name + 16x16 bitmap)
- Automatic icon detection from BIN headers
- Mouse double-click or keyboard to launch apps
- Floppy disk swap detection

### Audio
- PC speaker tone generation via PIT Channel 2
- Automatic silence on task exit

### GUI Toolkit
- Button widget with pressed state
- Radio button widget
- Checkbox widget
- Text input field (with cursor and password mode)
- Scrollbar (vertical and horizontal)
- List item (with selection highlight)
- Progress bar (with percentage text)
- Group box with label
- Separator line (horizontal and vertical)
- Combo box dropdown
- Menu bar
- Hit test for mouse click detection
- Global color theme (text, background, window colors)

### System Clipboard
- Copy/paste/query APIs
- 4KB capacity at scratch segment 0x9000:0x0000

### Popup Menu System
- Generic popup menu open/close/hit-test APIs
- Used by context menus and file menus in apps

### File Dialog
- System-wide blocking file open dialog (API 90)
- Scrollable file list with keyboard and mouse navigation
- Open/Cancel buttons

### System APIs
- Real-time clock read/write (BCD format)
- Delay with cooperative yield
- Task info query
- Screen info query
- Tick counter

### Settings Persistence
- SETTINGS.CFG on boot drive (font, colors)
- Loaded at boot, saved by Settings app

### Applications
- **LAUNCHER.BIN** - Desktop launcher with 4x3 icon grid
- **CLOCK.BIN** - Analog clock face + digital time display
- **BROWSER.BIN** - File manager with delete, rename, copy operations
- **TEXT.BIN** - Notepad text editor with selection, undo, clipboard, context menu, file dialog
- **MOUSE.BIN** - Mouse test/demo
- **MUSIC.BIN** - Fur Elise music player
- **MKBOOT.BIN** - Boot floppy creator (floppy-to-floppy copy)
- **SETTINGS.BIN** - System settings (font, theme colors)
- **TETRIS.BIN** - Dostris (Tetris clone with Korobeiniki music)
- **DEMO.BIN** - UI widget showcase demo
- **APITEST.BIN** - API test suite
- **HELLO.BIN** - Hello World test application

---

## API Summary (91 Functions)

| Index | Function | Description |
|-------|----------|-------------|
| 0 | gfx_draw_pixel | Draw single pixel |
| 1 | gfx_draw_rect | Draw rectangle outline |
| 2 | gfx_draw_filled_rect | Draw filled rectangle |
| 3 | gfx_draw_char | Draw single character |
| 4 | gfx_draw_string | Draw string (foreground color) |
| 5 | gfx_clear_area | Clear rectangular area |
| 6 | gfx_draw_string_inverted | Draw string (inverted) |
| 7 | mem_alloc | Allocate memory |
| 8 | mem_free | Free memory |
| 9 | event_get | Get event (non-blocking) |
| 10 | event_wait | Wait for event (blocking) |
| 11 | kbd_getchar | Get key (deprecated) |
| 12 | kbd_wait_key | Wait for key (deprecated) |
| 13 | fs_mount | Mount filesystem |
| 14 | fs_open | Open file |
| 15 | fs_read | Read file |
| 16 | fs_close | Close file |
| 17 | fs_register_driver | Register FS driver (reserved) |
| 18 | app_load | Load application binary |
| 19 | app_run | Run application (blocking) |
| 20 | win_create | Create window |
| 21 | win_destroy | Destroy window |
| 22 | win_draw | Redraw window frame |
| 23 | win_focus | Bring window to front |
| 24 | win_move | Move window |
| 25 | win_get_content | Get content area bounds |
| 26 | register_shell | Register shell callback |
| 27 | fs_readdir | Read directory entry |
| 28 | mouse_get_state | Get mouse position/buttons |
| 29 | mouse_set_position | Set mouse cursor position |
| 30 | mouse_is_enabled | Check if mouse available |
| 31 | win_begin_draw | Set window drawing context |
| 32 | win_end_draw | Clear drawing context |
| 33 | gfx_text_width | Measure string width |
| 34 | app_yield | Yield to scheduler |
| 35 | app_start | Start app (non-blocking) |
| 36 | app_exit | Exit current task |
| 37 | desktop_set_icon | Register desktop icon |
| 38 | desktop_clear_icons | Clear all desktop icons |
| 39 | gfx_draw_icon | Draw 16x16 icon |
| 40 | fs_read_header | Read file header bytes |
| 41 | speaker_tone | Play tone (Hz) |
| 42 | speaker_off | Turn off speaker |
| 43 | get_boot_drive | Get boot drive number |
| 44 | fs_write_sector | Write raw sector |
| 45 | fs_create | Create new file |
| 46 | fs_write | Write to file |
| 47 | fs_delete | Delete file |
| 48 | gfx_set_font | Select active font |
| 49 | gfx_get_font_metrics | Get font metrics |
| 50 | gfx_draw_string_wrap | Draw string with word wrap |
| 51 | widget_draw_button | Draw button widget |
| 52 | widget_draw_radio | Draw radio button |
| 53 | widget_hit_test | Mouse hit test |
| 54 | theme_set_colors | Set color theme |
| 55 | theme_get_colors | Get color theme |
| 56 | widget_draw_checkbox | Draw checkbox |
| 57 | widget_draw_textfield | Draw text input field |
| 58 | widget_draw_scrollbar | Draw scrollbar |
| 59 | widget_draw_listitem | Draw list item |
| 60 | widget_draw_progress | Draw progress bar |
| 61 | widget_draw_groupbox | Draw group box |
| 62 | widget_draw_separator | Draw separator line |
| 63 | get_tick_count | Get BIOS tick counter |
| 64 | point_over_window | Check point over window |
| 65 | widget_draw_combobox | Draw combo box |
| 66 | widget_draw_menubar | Draw menu bar |
| 67 | gfx_draw_filled_rect_color | Draw filled rect with color |
| 68 | gfx_draw_rect_color | Draw rect outline with color |
| 69 | gfx_draw_hline | Draw horizontal line |
| 70 | gfx_draw_vline | Draw vertical line |
| 71 | gfx_draw_line | Draw line (Bresenham's) |
| 72 | get_rtc_time | Read real-time clock |
| 73 | delay_ticks | Delay with yield |
| 74 | get_task_info | Get task info |
| 75 | fs_seek | Seek file position |
| 76 | fs_get_file_size | Get file size |
| 77 | fs_rename | Rename file |
| 78 | win_resize | Resize window |
| 79 | win_get_info | Get window info |
| 80 | gfx_scroll_area | Scroll area vertically |
| 81 | set_rtc_time | Set real-time clock |
| 82 | get_screen_info | Get screen dimensions |
| 83 | get_key_modifiers | Get Shift/Ctrl/Alt state |
| 84 | clip_copy | Copy to clipboard |
| 85 | clip_paste | Paste from clipboard |
| 86 | clip_get_len | Get clipboard length |
| 87 | menu_open | Open popup menu |
| 88 | menu_close | Close popup menu |
| 89 | menu_hit | Hit-test popup menu |
| 90 | file_dialog_open | System file open dialog |

For full register-level details, see [API_REFERENCE.md](API_REFERENCE.md).

---

## Memory Layout

```
0x0000:0x0000   Interrupt Vector Table        1 KB
0x0000:0x0400   BIOS Data Area                256 bytes
0x0000:0x7C00   Boot sector                   512 bytes
0x0800:0x0000   Stage2 loader                 2 KB
0x1000:0x0000   Kernel                        28 KB
0x1400:0x0000   Heap (malloc pool)            ~64 KB
0x2000:0x0000   Shell/Launcher segment        64 KB (fixed)
0x3000-0x8000   User app slots 0-5            6 x 64 KB (dynamic)
0x9000:0x0000   Scratch buffer                64 KB
  0x0000-0x0FFF   System clipboard (4KB)
  0x1000+         File dialog list buffer
0xB800:0x0000   CGA video memory              16 KB
```

---

## BIN File Icon Header (80 bytes)

```
Offset  Size   Description
0x00    2      JMP short 0x50 (0xEB, 0x4E)
0x02    2      Magic: "UI" (0x55, 0x49)
0x04    12     App display name (null-padded)
0x10    64     16x16 icon bitmap (2bpp CGA, 4 bytes/row)
0x50    ...    Code entry point
```

Detection: `byte[0]==0xEB && byte[2]=='U' && byte[3]=='I'`

---

## Non-Goals

- Networking (no TCP/IP, no modem)
- Preemptive multitasking (cooperative only)
- Protected mode (real mode for XT compatibility)
- High resolution (CGA only)
- DOS compatibility (different API)

---

*v3.22.0 Build 351*
