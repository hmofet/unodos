# Changelog

All notable changes to UnoDOS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [3.19.0] - 2026-02-16

### Added (Builds 202-212) - FAT12 Write, GUI Toolkit, Settings Persistence

- **FAT12 Write Support** (Build 202)
  - `fs_create_stub` (API 45) — Create new file on FAT12 floppy
  - `fs_write_stub` (API 46) — Write data to open file
  - `fs_delete_stub` (API 47) — Delete file from FAT12 floppy
  - `fs_write_sector_stub` (API 44) — Write raw sector to disk
  - Full FAT12 cluster chain allocation for multi-cluster files

- **Boot Floppy Creator (MkBoot)** (Builds 202-204)
  - New app: creates bootable UnoDOS floppy from running system
  - Pre-reads apps to RAM, prompts for disk swap, writes boot+kernel+apps
  - Floppy-to-floppy copy workflow for users without build tools

- **GUI Toolkit Foundation** (Build 205)
  - Multi-font system: 4x6 small, 8x8 medium, 8x14 large fonts
  - `gfx_set_font` (API 48), `gfx_get_font_metrics` (API 49)
  - Word-wrap text drawing: `gfx_draw_string_wrap` (API 50)
  - Widget APIs: `widget_draw_button` (51), `widget_draw_radio` (52), `widget_hit_test` (53)
  - Clip rectangle system for constraining drawing operations

- **Settings App** (Builds 206-210)
  - Font selection (small/medium/large) with live preview
  - Color theme: text color, desktop background, window color (4 CGA colors)
  - Color swatch picker with radio buttons for font selection
  - Apply/OK/Defaults buttons
  - Settings persist to `SETTINGS.CFG` on boot floppy via FAT12 write APIs
  - Kernel loads settings at boot before launching apps

- **Color Theme System** (Builds 208-209)
  - `theme_set_colors` (API 54), `theme_get_colors` (API 55)
  - `draw_bg_color` for text background rendering
  - Desktop background color, window frame color, text color all configurable

### Fixed (Builds 202-212)

- **Disappearing Windows** (Build 211): Drawing APIs 0 (pixel), 1 (rect), 2 (filled rect) didn't hide mouse cursor during drawing. IRQ12 could XOR the cursor over window frame pixels between API calls, progressively corrupting borders. Fixed by adding cursor_hide/cursor_locked to all drawing APIs.
- **MkBoot Window Redraw** (Build 211): MkBoot defined `API_WIN_DRAW equ 30` but the correct value is 22. API 30 is `mouse_is_enabled`, so the window redraw call was silently doing nothing.
- **draw_bg_color Pollution** (Build 211): `draw_desktop_region` set `draw_bg_color` to desktop color but never restored it, leaking into subsequent window drawing operations.
- **fs_open_stub Mount Handle** (Build 210): Compared full 16-bit BX for mount handle routing, but callers set only BL. Dirty BH caused silent open failures. Fixed to compare BL only.
- **fs_readdir_stub Mount Handle** (Build 209): Same BX vs BL routing bug as fs_open_stub.
- **Button Text Overflow** (Build 210): `widget_draw_button` didn't clip label text to button bounds. Fixed by setting clip rectangle before drawing label.
- **Mouse Click Events** (Build 207): `event_get_stub` wasn't setting CF flag correctly for mouse events, causing click handlers to miss events.

### Changed (Builds 202-212)

- API table expanded from 44 to 56 function slots (APIs 44-55)
- CGA pixel plotting functions refactored: shared `cga_pixel_calc` helper saves ~100 bytes (Build 212)
- FAT12 stack cleanup comments updated from stale line numbers to descriptive labels (Build 212)
- File handle validation uses `FILE_MAX_HANDLES` constant instead of magic number 16 (Build 212)
- Coordinate translation in INT 0x80 handler now covers APIs 0-6 and 50-52 (widget APIs)

---

## [3.18.0] - 2026-02-16

### Added (Builds 194-201) - Splash Screen, Multitasking Fixes, Refresh Icon

- **Splash Screen with Logo** (Builds 196-201)
  - "U" logo drawn with white filled rectangles during boot
  - "UnoDOS 3" title and "Loading..." text displayed
  - Progress bar fills as apps are discovered from disk
  - Replaces blank/debug screen during launcher initialization
  - Fast CGA memory clear via REP STOSW (Build 200)

- **Floppy Refresh Icon** (Build 195)
  - Manual disk rescan icon appears as last desktop icon on floppy boot
  - 3.5" floppy disk shape (16x16 2bpp CGA bitmap)
  - Replaces automatic INT 13h AH=16h polling (caused constant floppy seeking)
  - Only shown when booted from floppy

- **Launch Error Feedback** (Build 195)
  - "Insert app disk" message for mount/file errors (codes 2, 3)
  - Error message auto-clears after ~2 seconds with desktop redraw

### Fixed (Builds 194-201)

- **File Browser HD Support** (Build 194): Browser now queries boot drive
  and saves mount handle, fixing blank listing on HD/CF/USB boot
- **Floppy Seeking Noise** (Build 195): Removed automatic floppy swap
  polling (INT 13h AH=16h) that caused audible seeking on IBM PS/2 L40
- **Music App Single Tone** (Build 197): App played one constant note
  instead of Fur Elise melody. Root cause: `app_yield_stub` didn't
  preserve general-purpose registers across context switches, so CX
  (note duration) was clobbered by the launcher. Fixed with pusha/popa.
- **App Launch Crash** (Build 198): Adding pusha/popa to yield broke
  new task startup — `popa` consumed return addresses instead of
  register values. Fixed by adding dummy pusha frame (8 zero words)
  to initial task stack built by `app_start_stub`.
- **Initial Context Switch Loop** (Build 201): `auto_load_launcher`
  did bare `ret` without `popa`, popping 0 from the dummy pusha frame
  instead of `int80_return_point`. This jumped to kernel entry (0x0000)
  in an infinite loop: boot → load launcher → ret to kernel → repeat.
  Fixed by adding `popa` before `ret` in both `auto_load_launcher` and
  `app_exit_common`.

### Changed

- Bootloader version updated from v0.2 to v3.18
- All boot diagnostic code removed (keypress wait, BIOS teletype,
  CGA white boxes, PRE/POST markers)
- Splash screen text uses transparent background (Build 199)
- Desktop/splash screen clear uses direct CGA REP STOSW (Build 200)

### Removed

- Kernel boot keypress wait
- Kernel BIOS teletype diagnostic output
- Kernel "PRE"/"POST" CGA diagnostic strings
- Launcher CGA diagnostic white boxes (marks 1-6)

---

## [3.17.0] - 2026-02-15

### Added (Builds 162-193) - Universal PS/2 Mouse via BIOS Services

- **BIOS PS/2 Mouse Driver** (Build 187+)
  - Uses BIOS INT 15h/C2xx services instead of direct KBC port I/O
  - INT 15h/C205 (init), C207 (set callback), C200 (enable)
  - Works with USB mice via BIOS legacy emulation (SMI-based)
  - FAR CALL callback handler (`mouse_bios_callback`) processes packets from BIOS
  - Falls back to direct KBC method if BIOS services unavailable

- **Robust KBC Mouse Init** (Build 185)
  - KBC output buffer flush (16-byte drain loop) before init
  - Keyboard interface disabled (0xAD) during mouse setup
  - Long timeout for ACK wait (~1 second via BIOS timer tick)
  - Mouse reset retried up to 3 times
  - Keyboard re-enabled (0xAE) on both success and failure

- **Boot Diagnostic** (Build 184+)
  - Mouse init result displayed at boot: B=BIOS, K=KBC, R/S/E=failure
  - Keypress wait after diagnostic for hardware verification

### Fixed (Builds 162-193)

- **USB Legacy Emulation Conflict** (Build 187): BIOS SMI handler was overriding
  direct KBC port writes, re-masking IRQ12 and disabling aux clock. Solved by
  switching to BIOS INT 15h/C2 services which work *with* the SMI handler.
- **BIOS Callback AH Corruption** (Build 188): `mov ah, bh` in X sign-extend
  overwrote the status byte stored in AH, breaking Y sign extension.
- **Callback Byte Order** (Build 192): Discovered via QEMU raw stack dump that
  BIOS pushes status,X,Y,0 before CALL FAR, making [BP+12]=status, [BP+10]=X,
  [BP+8]=Y, [BP+6]=padding. All previous convention assumptions were wrong.
- IRQ2 cascade unmask added for IRQ12 propagation (Build 186)
- Removed fragile auto-detection of callback byte conventions (Build 192)

### Changed

- Mouse driver architecture: BIOS services primary, direct KBC fallback
- Kernel alignment pad bumped (0x11A0 → 0x13A0) for new mouse init code

---

## [3.16.0] - 2026-02-14

### Added (Build 161) - Hard Drive Boot Support

- **Hard Drive Boot Verified** (Build 161)
  - Full MBR → VBR → Stage2_hd → Kernel boot chain tested
  - FAT16 filesystem on 64MB partition with all apps
  - Boots from hard drives, CF cards, and USB flash drives (via BIOS emulation)

- **Boot Drive Query API** (Build 161)
  - New API 43: get_boot_drive — returns boot drive number in AL
  - Enables apps to detect floppy (0x00) vs hard drive (0x80) boot

### Fixed (Build 161)

- Launcher now queries boot drive from kernel instead of hardcoding floppy
- Launcher uses correct mount handle for FAT12/FAT16 routing
- read_bin_header uses dynamic mount handle (was hardcoded FAT12)
- Floppy swap detection skipped when booted from hard drive
- fat16_read 32-bit arithmetic: sector calculation no longer truncates to 16 bits
- MUSIC.BIN added to HD image (was missing from create_hd_image.py)

### Changed

- API table expanded from 43 to 44 functions (get_boot_drive)
- Launcher detects boot media type automatically

---

## [3.15.0] - 2026-02-14

### Added (Builds 151-159) - Window Manager, Sound, Close Button

- **Window Close Button** (Build 152)
  - [X] button drawn at right side of title bar
  - Click to terminate app and destroy window
  - Speaker silenced on app exit (prevents stuck tones)
  - Works for both current and background tasks

- **PC Speaker Sound** (Build 152)
  - New APIs: speaker_tone (41), speaker_off (42)
  - PIT Channel 2 programming for frequency generation
  - Automatic speaker silence on task termination

- **Music Player App** (Build 152)
  - MUSIC.BIN - Beethoven's Fur Elise opening theme
  - Sequential note playback via PC speaker
  - BIOS tick counter timing (~18.2 Hz)
  - Musical note icon in app header

- **Outline Drag** (Build 156)
  - XOR rectangle outline during window drag (Windows 3.1 style)
  - Window moves once on mouse release, single clean repaint
  - Replaced pixel save/restore drag (~235 lines removed)

- **Z-Order Window Management** (Builds 155-159)
  - Background windows blocked from drawing over foreground
  - Topmost window bounds cache for O(1) clipping
  - Active/inactive title bar visual distinction
  - Active: filled white title bar with black text
  - Inactive: black title bar with white text outline
  - Automatic title bar style update on focus change

### Fixed (Builds 151-159)

- Build 153: Floppy read retry logic for reliable loading on real hardware
- Build 154: App load error code diagnostic in launcher
- Build 155: Background windows losing content due to overzealous z-order clipping
- Build 157: Post-drag z-order — desktop icons and background frames showing through moved window
- Build 158: Per-draw-call z-order clipping (point-inside-topmost check)
- Build 159: Simplified to full background draw blocking (fixes multi-pixel bleed-through)

### Changed

- API table expanded from 41 to 43 functions (speaker_tone, speaker_off)
- Window drag: outline-based instead of content save/restore
- Draw API calls from background windows silently dropped (apps repaint on focus)
- Title bar style differentiates active vs inactive windows

---

## [3.14.0] - 2026-02-13

### Added (Builds 144-150) - Desktop Icons, Multi-App, Multitasking

- **Desktop Icon System** (Build 144)
  - 4x2 icon grid with 16x16 2bpp CGA icon bitmaps on desktop
  - BIN file icon headers (80 bytes: JMP + "UI" magic + 12B name + 64B bitmap)
  - Automatic icon detection from BIN headers at boot
  - Default icon for legacy apps without headers
  - Mouse double-click to launch (~0.5s threshold)
  - Keyboard navigation (arrows/WASD + Enter)
  - New APIs: desktop_set_icon (37), desktop_clear_icons (38), gfx_draw_icon (39), fs_read_header (40)

- **Cooperative Multitasking** (Build 144)
  - Round-robin cooperative scheduler (app_yield, app_start)
  - Per-task draw_context save/restore
  - Per-task event filtering (KEY_PRESS to focused, WIN_REDRAW to owner)

- **Multi-App Concurrent Execution** (Build 149)
  - Dynamic segment pool: 6 user segments (0x3000-0x8000)
  - alloc_segment / free_segment kernel helpers
  - Up to 6 concurrent user apps + launcher
  - Scratch buffer moved from 0x5000 to 0x9000

- **Window Title Bar Text** (Build 150)
  - Fixed gfx_draw_string_inverted reading from wrong segment for titles

### Fixed (Builds 145-148)

- Build 145: Window drag content flicker
- Build 146: Desktop z-order, floppy detection, version display
- Build 147: Mouse test app, icon repaint, icon deselect
- Build 148: Double-ESC exit bug (event queue), hello window sizing

### Changed

- API table expanded from 34 to 41 functions
- Memory: 6 dynamic user segments (0x3000-0x8000) replace single 0x3000
- Scratch buffer relocated from 0x5000 to 0x9000
- Launcher rewritten as fullscreen desktop with icon grid

---

## [3.13.0] - 2026-02-11

### Fixed (Build 135) - Text Width Measurement

- **gfx_text_width returned wrong values** - Was reporting 8px per character but draw_char advances 12px (8px glyph + 4px gap). Fixed to return 12px per character, matching actual rendering.
- **Clock content overflowed window** - "00:00:00" = 96px (8×12) but was drawn at X=22 in 108px content area. Repositioned to X=6 for proper centering.
- **Launcher help text caused white boxes** - "W/S/Arrows: Select" = 216px (18×12), far too wide for window. Removed help text from launcher.

### Added (Builds 127-134) - Mouse Cursor, Window Dragging, Drawing Context

- **XOR Mouse Cursor** - 8x10 arrow sprite drawn with plot_pixel_xor (self-erasing)
  - Cursor hide/show with `cursor_locked` flag for flicker-free rendering
  - Visible on all backgrounds (white on black, black on white)

- **Window Title Bar Dragging** - Click and drag windows by title bar
  - Three-layer architecture: IRQ12 detection → drag state machine → deferred processing
  - `mouse_hittest_titlebar` checks all visible windows for click hits
  - `mouse_drag_update` tracks offset and target position
  - `mouse_process_drag` called from event_get_stub (safe from reentrancy)

- **OS-Managed Content Preservation** - Window content saved during drags
  - Scratch buffer at segment 0x5000 stores CGA pixel data
  - Byte-aligned save/restore with `min(old_bpr, new_bpr)` for cross-boundary moves
  - Apps don't need to redraw when their window is dragged

- **Window Drawing Context** (APIs 31-32) - Apps use window-relative coordinates
  - `win_begin_draw` activates context for a window handle
  - `win_end_draw` deactivates context
  - APIs 0-6 automatically translate BX/CX from (0,0)=content-top-left to absolute screen

- **Text Width Measurement** (API 33) - `gfx_text_width` returns string width in pixels

- **gfx_draw_string_inverted** (API 6) - Fixed to use caller_ds for string access
  - Was reading from kernel segment (DS=0x1000) instead of app's segment
  - Caused white garbage boxes when launcher drew help text

### Changed (Builds 127-134)

- API table moved from 0x0F00 to 0x0F80 (more code space)
- API count increased from 30 to 34 (functions 30-33)
- Mouse enabled by default at boot (was disabled)
- Launcher binary: 1304 → 1069 bytes (removed debug code and help text)
- Clock window: W=90 → W=110 with centered time display at X=6

### Added (Build 054) - Hard Drive / FAT16 Support

- **FAT16 Filesystem Driver** - Read-only support for hard drives
  - `fat16_mount` - Mount FAT16 partition from MBR/partition table
  - `fat16_open` - Open files from FAT16 root directory
  - `fat16_read` - Read file data following FAT16 cluster chains
  - `fat16_get_next_cluster` - 16-bit FAT entry reading (simpler than FAT12's 12-bit)
  - `fat16_read_sector` - Sector read with INT 13h LBA extensions + CHS fallback

- **IDE/ATA Direct Access Driver** - Fallback for BIOS issues
  - `ide_detect` - Detect IDE drive presence via IDENTIFY command
  - `ide_wait_ready` - Wait for drive ready (BSY clear, DRDY set)
  - `ide_read_sector` - Direct port I/O read (ports 0x1F0-0x1F7)
  - Supports LBA addressing mode

- **HD Boot Support** - Boot UnoDOS directly from hard drive
  - `boot/mbr.asm` - Master Boot Record with partition table parsing
  - `boot/vbr.asm` - Volume Boot Record with FAT16 BPB
  - `boot/stage2_hd.asm` - HD kernel loader (finds KERNEL.BIN on FAT16)
  - Standard MBR relocation to 0x0600 for VBR loading

- **HD Image Creation Tools**
  - `tools/create_hd_image.py` - Create 64MB FAT16 bootable HD image
  - `tools/hd.ps1` - PowerShell script to write HD image to CF cards
  - Apps automatically included: KERNEL.BIN, LAUNCHER.BIN, CLOCK.BIN, BROWSER.BIN, MOUSE.BIN, TEST.BIN

- **New Makefile Targets**
  - `make hd-image` - Build bootable FAT16 HD image
  - `make run-hd` - Test HD image in QEMU

### Changed

- Filesystem stubs now route by drive type:
  - Drive 0 (A:) -> FAT12 driver (mount handle 0)
  - Drive 0x80+ (HD) -> FAT16 driver (mount handle 1)
- Kernel size increased to accommodate FAT16/IDE drivers

### Technical Details (HD Driver - Build 054)

- **Partition Table Parsing**
  - MBR at sector 0, partition table at offset 0x1BE
  - Supports FAT16 partition types: 0x04, 0x06, 0x0E
  - Hidden sectors field used for partition-relative LBA

- **INT 13h Extensions**
  - Uses AH=42h (extended read) with disk address packet
  - Falls back to CHS conversion for older BIOSes
  - Drive geometry queried via AH=08h

- **IDE Port I/O Protocol**
  - Primary controller: 0x1F0-0x1F7
  - Status polling: Wait for BSY=0, DRDY=1
  - LBA mode via 0xE0 in drive/head register
  - 256-word (512-byte) sector transfer via REP INSW

---

## [3.12.0] - 2026-01-28

### Added (Build 053) - PS/2 Mouse Driver
- **PS/2 Mouse Driver** - Foundation 1.7 complete
  - INT 0x74 (IRQ12) mouse interrupt handler
  - 8042 keyboard controller interface (ports 0x60/0x64)
  - 3-byte packet protocol parsing with sync bit detection
  - Automatic mouse detection at boot
  - Position tracking clamped to screen (0-319 X, 0-199 Y)
  - Button state tracking (left, right, middle)
  - Posts EVENT_MOUSE (type 4) to event queue

- **New Mouse APIs (27-29)**
  - `mouse_get_state` (API 27) - Returns BX=X, CX=Y, DL=buttons, DH=enabled
  - `mouse_set_position` (API 28) - Sets cursor position
  - `mouse_is_enabled` (API 29) - Checks if mouse available

- **MOUSE.BIN Test Application** (578 bytes)
  - Window-based UI with mouse cursor tracking
  - Displays '+' cursor that follows mouse position
  - Shows '*' when button pressed
  - Displays X,Y coordinates
  - Gracefully handles "no mouse detected"
  - ESC to exit

### Added (Build 042) - Dynamic Discovery & Browser
- **fs_readdir API (Index 26)** - Kernel directory iteration
- **Dynamic App Discovery** - Launcher scans for .BIN files
- **BROWSER.BIN** - File browser showing all files with sizes (564 bytes)

### Fixed (Builds 042-052)
- Build 051: Browser ESC doesn't work - Added STI, use JC pattern
- Build 052: Cleanup - Removed debug code

### Added (Build 010-041) - Window Manager
- **Window Manager** - Second Core Services feature (v3.12.0)
  - `win_create_stub` (API 19) - Create new window with position, size, title, and flags
  - `win_destroy_stub` (API 20) - Destroy window and clear its area
  - `win_draw_stub` (API 21) - Redraw window frame (title bar and border)
  - `win_focus_stub` (API 22) - Bring window to front (set z_order to 15, demote others)
  - `win_move_stub` (API 23) - Move window to new position
  - `win_get_content_stub` (API 24) - Get content area bounds for app drawing
  - window_table structure tracks up to 16 windows (512 bytes)
  - Window structure (32 bytes): state, flags, x, y, width, height, z_order, owner_app, title

- **Window Visual Design**
  - 10-pixel white title bar with centered title text
  - 1-pixel white border around window
  - Content area calculation: accounts for title bar and borders
  - Window flags: WIN_FLAG_TITLE (show title bar), WIN_FLAG_BORDER (show border)

- **'W' key handler** in keyboard demo
  - Press 'W' to create a test window at (50, 30) with size 200x100
  - Displays "Window: OK" or "Window: FAIL" status message

### Fixed
- **gfx_clear_area_stub** - Was previously a no-op stub, now properly clears rectangular areas
  - Implements pixel-by-pixel clearing to background color
  - Required for window background clearing

### Changed
- API table expanded from 19 to 30 functions (Builds 010-053)
- API table padding increased from 0x0900 to 0x0B00 (Build 053 - mouse driver code size)
- Keyboard demo prompt updated to show W key option: "ESC=exit F=file L=app W=win:"

### Technical Details (PS/2 Mouse - Build 053)
- PS/2 mouse packet format:
  - Byte 0: YO XO YS XS 1 M R L (overflow, sign, sync bit, buttons)
  - Byte 1: X movement delta (8-bit, sign-extended with XS)
  - Byte 2: Y movement delta (8-bit, sign-extended with YS)
- IRQ12 requires EOI to both slave PIC (0xA0) and master PIC (0x20)
- Sync bit (bit 3) in first byte ensures packet alignment
- AUXB bit (0x20) in status port distinguishes mouse from keyboard data

### Technical Details (Window Manager)
- Window structure (32 bytes):
  - Offset 0: State (0=free, 1=visible, 2=hidden)
  - Offset 1: Flags (bit 0: has_title, bit 1: has_border)
  - Offset 2-3: X position (0-319)
  - Offset 4-5: Y position (0-199)
  - Offset 6-7: Width in pixels
  - Offset 8-9: Height in pixels
  - Offset 10: Z-order (0=bottom, 15=top)
  - Offset 11: Owner app handle (0xFF = kernel)
  - Offset 12-23: Title (11 chars + null)
  - Offset 24-31: Reserved

- Content area calculation:
  - Content X = Window X + 1 (border)
  - Content Y = Window Y + 10 (titlebar) + 1 (border)
  - Content Width = Window Width - 2 (borders)
  - Content Height = Window Height - 12 (titlebar + borders)

### Constraints (v3.12.0)
- No overlapping windows (windows must not overlap)
- No dragging (win_move is API-only, no mouse drag)
- No close button (destroy via API only)
- White only (title bar uses white, no inverse video)

### Future Enhancements
- v3.13.0: Mouse support for window interaction
- v3.14.0: Window dragging via mouse
- v3.15.0: Overlapping window redraw

---

## [3.11.0] - 2026-01-25

### Added
- **Application Loader** - First Core Services feature (v3.11.0)
  - `app_load_stub` (API 17) - Load .BIN applications from FAT12 into heap memory
  - `app_run_stub` (API 18) - Execute loaded applications via far CALL
  - app_table structure tracks up to 16 loaded applications (512 bytes)
  - Entry includes: state, priority, code segment/offset, stack (for future multitasking)
  - BIOS drive number support (0x00=A:, 0x01=B:, 0x80=C:, etc.)

- **Test Application Framework**
  - apps/hello.asm - Simple test app that draws 'H' pattern to verify loader
  - tools/create_app_test.py - Creates FAT12 floppy with HELLO.BIN
  - `make apps` target to build applications
  - `make test-app` target to test app loader in QEMU

- **'L' key handler** in keyboard demo
  - Press 'L' to trigger app loader test
  - Prompts to insert app disk
  - Loads and runs HELLO.BIN from disk

### Fixed (Build 008)
- **Keyboard ISR register corruption** - INT 09h handler was modifying DX register
  without saving/restoring it, causing display corruption on real hardware
  - Added push/pop dx to int_09_handler
- **Error code display position** - Error digit was drawn at X=100, overlapping with
  'I' in "FAIL". Moved to X=136 (after 11-character string)

### Fixed (Build 009)
- **App loader filename format** - fat12_open expects "HELLO.BIN" (with dot separator)
  but kernel was passing "HELLO   BIN" (raw FAT 8.3 format without dot)
  - Changed .app_filename to include dot separator
  - fat12_open parses the dot to split name/extension correctly
- **Dynamic build numbers** - Version and build strings are now generated from files
  - BUILD_NUMBER file contains current build number
  - VERSION file contains version string
  - Makefile generates kernel/build_info.inc before assembly
  - `make bump-build` increments build number for next build

### Changed
- API table expanded from 17 to 19 functions
- Keyboard demo prompt updated to show L key option

### Technical Details
- App calling convention:
  - Entry point at offset 0x0000 within loaded segment
  - Kernel calls app via far CALL
  - App returns via RETF with return code in AX
  - Apps can discover kernel API via INT 0x80 (returns ES:BX = API table pointer)

- Memory layout:
  - Kernel at 0x1000:0000 (28KB)
  - Heap at 0x1400:0000 (apps loaded here via mem_alloc)

- App table entry (32 bytes):
  - Offset 0: State (0=free, 1=loaded, 2=running, 3=suspended)
  - Offset 2: Code segment
  - Offset 4: Code offset (entry point)
  - Offset 6: Code size
  - Offset 8-10: Stack segment/pointer (for future multitasking)
  - Offset 12-22: Filename (8.3 format)

### Future Enhancements Prepared
- App table includes state field for cooperative multitasking
- Stack segment/pointer fields for context switching
- Priority field for future scheduler

## [3.10.1] - 2026-01-24

### Added
- **Multi-cluster file reading** - FAT12 driver now reads files larger than 512 bytes
  - get_next_cluster() function reads FAT12 entries and follows cluster chains
  - Handles 12-bit FAT entries with even/odd cluster logic
  - FAT sector caching (512-byte fat_cache buffer + fat_cache_sector tracker)
  - Reads end-of-chain markers (0xFF8-0xFFF) to detect file end

- **Enhanced test infrastructure**
  - tools/create_multicluster_test.py - generates 1024-byte test files
  - Test file spans 2 clusters with "CLUSTER 1:" and "CLUSTER 2:" markers
  - FAT chain validation: cluster 2 → cluster 3 → EOF
  - make test-fat12-multi target for testing
  - fs_read_buffer expanded from 512 to 1024 bytes

- **Hardware debugging documentation**
  - docs/FAT12_HARDWARE_DEBUG.md - complete debugging process documentation

### Fixed
- **Stack cleanup bug in fat12_open .found_file** (Critical)
  - DS register pushed during search loop wasn't being popped in .found_file path
  - Caused system hang after finding file
  - Fixed by adding `add sp, 2` for DS cleanup

- **LBA to CHS conversion in fat12_read** (Critical)
  - Original code used bitmasks instead of proper division
  - DH (head) was never calculated
  - ES segment wasn't set for INT 13h read
  - BX (buffer pointer) was clobbered during division
  - Fixed with proper formula matching fat12_open's working code

- **Simplified attribute reading in fat12_open**
  - Removed unnecessary ES segment override
  - DS is already 0x1000 in the search loop

### Changed
- **fat12_read() rewritten for multi-cluster support**
  - Now loops through cluster chain until EOF or all bytes read
  - Reads each cluster sequentially into bpb_buffer
  - Copies data to user buffer and advances pointer (ES:DI) automatically
  - Updates file position correctly for multi-cluster reads

- **Debug code removed for release**
  - Removed D:, S:, A:, F: debug output from fat12_open
  - Removed comparison result (=/!) debug output
  - Removed unused debug strings (.dbg_dir, .dbg_srch, etc.)
  - Build string changed from "debug11" to "release"

### Technical Details
- FAT12 cluster chain algorithm:
  - Calculate FAT offset: `(cluster × 3) / 2`
  - Determine FAT sector: `reserved_sectors + (offset / 512)`
  - Cache FAT sector if not already loaded
  - Read 2 bytes from FAT at offset
  - If cluster is even: `value = word & 0x0FFF`
  - If cluster is odd: `value = word >> 4`
  - Check for end-of-chain: `value >= 0xFF8`

- LBA to CHS conversion for 1.44MB floppy (18 sectors/track, 2 heads):
  - Sector: `(LBA % 18) + 1`
  - Head: `(LBA / 18) % 2`
  - Cylinder: `LBA / 36`

### Hardware Verified
- ✅ Tested on HP Omnibook 600C (486DX4-75)
- ✅ Mount: OK
- ✅ Open TEST.TXT: OK
- ✅ Read: OK (multi-cluster)
- ✅ C1:A C2:B displayed correctly

### Testing
```bash
make test-fat12-multi      # Test with 1024-byte file
# Boot, press F, swap to test floppy
# Expected output: Mount: OK, Open: OK, Read: OK, C1:A C2:B
```

### Notes
- This release marks completion of FAT12 filesystem on real hardware
- Multi-cluster support enables loading applications larger than 512 bytes
- Critical foundation for Application Loader (v3.11.0)

---

## [3.10.0] - 2026-01-23

### Added
- **Foundation 1.6: Filesystem Abstraction Layer + FAT12 Driver** (Complete)
  - Filesystem driver abstraction (VFS-like interface)
  - FAT12 filesystem driver (boot sector BPB parsing, directory search, file reading)
  - Filesystem API functions added to kernel API table:
    - fs_mount_stub() - Mount filesystem on drive (API offset 12)
    - fs_open_stub() - Open file by name (API offset 13)
    - fs_read_stub() - Read file contents (API offset 14)
    - fs_close_stub() - Close file handle (API offset 15)
    - fs_register_driver_stub() - Register loadable driver (API offset 16, reserved for Tier 2/3)
  - File handle table (16 handles, 32 bytes each = 512 bytes)
  - BPB cache for filesystem metadata (512 bytes)
  - 8.3 filename support (FAT12 directory entry parsing)
  - Root directory search (up to 224 entries on 360KB floppy)
  - Single-cluster file reading (512 bytes per cluster)
  - Error handling with error codes: FS_OK, FS_ERR_NOT_FOUND, FS_ERR_NO_DRIVER, FS_ERR_READ_ERROR, etc.

- **Three-Tier Architecture Design**
  - Tier 1: Boot and run from single 360KB floppy (FAT12 built-in)
  - Tier 2: Multi-floppy system with loadable drivers (FAT16/FAT32 modules)
  - Tier 3: HDD installation with installer tool and bootloader writer
  - Driver registration hooks for future loadable filesystem modules

- **Test Infrastructure**
  - FAT12 test floppy creation script (Python)
  - TEST.TXT file on FAT12 image for validation
  - test_filesystem() function demonstrates fs_mount/open/read/close

### Changed
- **Kernel expanded from 24KB to 28KB** (48 → 56 sectors)
  - Accommodates FAT12 driver implementation (~2.7 KB)
  - New size: 28,672 bytes (28KB)
  - Stage2 loader updated to load 56 sectors
  - Still 85%+ free space remaining (estimated ~3.6 KB used)

- **Kernel API table relocated**
  - Moved from offset 0x0500 (1280 bytes) to 0x0800 (2048 bytes)
  - Provides 768 bytes additional headroom for future code
  - API table now at 0x1000:0x0800
  - Function count expanded from 12 to 17 slots

- **Entry point modified**
  - Replaced keyboard_demo with test_filesystem() for v3.10.0 testing
  - Can be reverted to keyboard_demo after filesystem validation

### Technical Details
- FAT12 implementation:
  - Reads boot sector (sector 0) via BIOS INT 13h
  - Parses BPB (BIOS Parameter Block): bytes_per_sector, sectors_per_cluster, root_dir_entries, etc.
  - Calculates root_dir_start and data_area_start from BPB
  - Searches root directory (14 sectors on 360KB floppy)
  - Converts user filename to 8.3 FAT format (space-padded)
  - Finds matching directory entry, extracts starting cluster and file size
  - Reads file data from cluster (sector = data_start + (cluster-2) * sectors_per_cluster)
  - Allocates file handle from 16-entry table
  - Current limitations: read-only, single cluster per read, position=0 only

- Filesystem abstraction:
  - Driver structure with function pointers (mount, open, read, close, list_dir, etc.)
  - Driver registry for up to 4 filesystem drivers
  - Auto-detection mechanism (tries each registered driver's detect function)
  - Mount table (4 entries, 16 bytes each) for active filesystem mounts
  - File handle table (16 entries, 32 bytes each) for open files

- Size impact:
  - Abstraction layer: ~400 bytes
  - FAT12 driver: ~1,200 bytes
  - Data structures: ~1,088 bytes (mount table, file table, BPB cache, read buffer)
  - Total: ~2,688 bytes
  - Remaining kernel space: ~25 KB free (87% available)

### Implementation
- fat12_mount(): Reads boot sector, parses BPB, calculates layout
- fat12_open(): Searches root directory, converts filename to 8.3, allocates handle
- fat12_read(): Reads cluster data, copies to user buffer, updates position
- fat12_close(): Marks file handle as free
- fs_mount_stub(): Calls fat12_mount for drive 0, returns mount handle
- fs_open_stub(): Validates mount handle, calls fat12_open
- fs_read_stub(): Validates file handle, calls fat12_read
- fs_close_stub(): Validates file handle, calls fat12_close

### Foundation Layer Progress
- ✓ System Call Infrastructure (v3.3.0)
- ✓ Graphics API (v3.4.0)
- ✓ Memory Allocator (v3.5.0)
- ✓ Kernel Expansion to 24KB → 28KB (v3.6.0 → v3.10.0)
- ✓ Aggressive Optimization (v3.7.0)
- ✓ Keyboard Driver (v3.8.0)
- ✓ Event System (v3.9.0)
- ✓ **Filesystem Abstraction + FAT12 (v3.10.0) - JUST COMPLETED**

### What's Next
Foundation Layer complete! Next phases:
1. **Core Services (v3.11.0-v3.13.0)**: App Loader, Window Manager
2. **Standard Library (v3.14.0)**: graphics.lib, unodos.lib for C development
3. **Tier 2/3 Support**: Multi-floppy loading, HDD installation, FAT16/FAT32 drivers

### Known Limitations
- Read-only filesystem (no write support)
- Single cluster reads only (512 bytes max)
- No multi-cluster file spanning support
- No subdirectory support (root directory only)
- No long filename support (8.3 only)
- File position fixed at 0 (no seek support)

These limitations are acceptable for v3.10.0 foundation. Advanced features will be added in future versions.

## [3.9.0] - 2026-01-23

### Added
- **Foundation 1.5: Event System** (Complete)
  - Circular event queue (32 events, 3 bytes each = 96 bytes)
  - Event structure: type (byte) + data (word)
  - post_event() function for posting events to queue
  - event_get_stub() - Non-blocking event retrieval (API offset 8)
  - event_wait_stub() - Blocking event wait (API offset 9)
  - Event types: KEY_PRESS (1), KEY_RELEASE (2), TIMER (3), MOUSE (4)
  - Keyboard integration: INT 09h now posts KEY_PRESS events

### Changed
- **Keyboard Demo Updated to Use Event System**
  - Now uses event_wait_stub() instead of kbd_wait_key()
  - Demonstrates event-driven programming model
  - Updated instruction text: "Uses: Event System + Graphics API"
  - Updated exit message: "Event demo complete!"
  - Validates event system integration with keyboard driver

### Technical Details
- Event queue: 32-event circular buffer (96 bytes total)
- Each event: 1 byte type + 2 bytes data
- Queue management: head/tail pointers with wraparound at 32
- Keyboard events: ASCII character stored in data field
- Backward compatibility: kbd_getchar/kbd_wait_key still available
- Dual posting: Keys stored in both keyboard buffer and event queue
- Event types extensible for future timer, mouse, custom events

### Implementation
- post_event(): Adds event to tail of queue, advances tail pointer
- event_get_stub(): Removes event from head, returns type and data
- event_wait_stub(): Loops on event_get_stub() until event available
- INT 09h handler: Calls post_event() after storing key in buffer
- Variables: event_queue[96], event_queue_head, event_queue_tail

### Foundation Layer Progress
- ✓ System Call Infrastructure (v3.3.0)
- ✓ Graphics API (v3.4.0)
- ✓ Memory Allocator (v3.5.0)
- ✓ Kernel Expansion to 24KB (v3.6.0)
- ✓ Aggressive Optimization (v3.7.0)
- ✓ Keyboard Driver (v3.8.0)
- ✓ **Event System (v3.9.0) - JUST COMPLETED**

### What's Next
Foundation Layer is now complete! Next phase: Standard Library (graphics.lib, unodos.lib)

## [3.8.0] - 2026-01-23

### Added
- **Foundation 1.4: Keyboard Driver** (Complete)
  - INT 09h keyboard interrupt handler with proper PIC EOI signaling
  - Scan code to ASCII translation tables (normal and shifted)
  - Modifier key state tracking (Shift, Ctrl, Alt)
  - 16-byte circular buffer for keyboard input
  - Non-blocking kbd_getchar() function (API offset 10)
  - Blocking kbd_wait_key() function (API offset 11)
  - Support for alphanumeric keys, punctuation, and special keys
  - Proper handling of key press and release events

- **Interactive Keyboard Demo** (Foundation Layer Integration Test)
  - Tests Graphics API + Keyboard Driver integration
  - Real-time keyboard input echo to screen
  - Displays prompt and instructions on boot
  - Handles special keys: ESC (exit), Enter (newline), Backspace (cursor back)
  - Auto-wraps at screen edges
  - Demonstrates API table function calls working in practice

### Technical Details
- INT 09h handler chains to original BIOS handler after processing
- Two 96-byte scan code translation tables (normal and shifted)
- Circular buffer prevents key loss during high-frequency input
- API table expanded: 10 → 12 function slots
- Estimated size: ~800 bytes (keyboard driver code + translation tables)
- Variables added: old_int9_offset/segment, kbd_buffer[16], buffer pointers, modifier states
- Interrupts enabled via STI after keyboard initialization

### Implementation
- install_keyboard(): Saves original INT 9h vector, installs handler, initializes buffer
- int_09_handler(): Reads scan code (port 0x60), tracks modifiers, translates to ASCII, stores in buffer
- kbd_getchar(): Returns next character from buffer (0 if empty)
- kbd_wait_key(): Blocks until key available, returns character
- Translation supports: A-Z, 0-9, punctuation, Escape, Backspace, Tab, Enter, Space

### Foundation Layer Progress
- ✓ System Call Infrastructure (v3.3.0)
- ✓ Graphics API (v3.4.0)
- ✓ Memory Allocator (v3.5.0)
- ✓ Kernel Expansion to 24KB (v3.6.0)
- ✓ Aggressive Optimization (v3.7.0)
- ✓ Keyboard Driver (v3.8.0)
- ⏳ Event System (v3.9.0 - Next)

## [3.7.0] - 2026-01-23

### Changed
- **Aggressive Kernel Optimization (Pre-Foundation 1.4/1.5)**
  - Removed test functions (test_int_80, test_graphics_api): ~200 bytes freed
  - Removed 21 character alias definitions (char_W, char_E, etc.)
  - Optimized all graphics API functions: replaced pusha/popa with targeted register saves
  - Optimized gfx_draw_rect_stub: eliminated ~80 bytes of redundant push/pop operations
  - Optimized plot_pixel_white: removed variable storage (pixel_save_x/y), stack-only implementation
  - Optimized setup_graphics: tighter BIOS call sequence
  - Optimized install_int_80: minimal register preservation
  - Welcome message now uses gfx_draw_string (string-based) instead of individual char draws

### Technical Details
- Kernel code: 2436 → 2416 bytes (20 bytes from optimization, ~200 from removal)
- Total space gained: ~220 bytes
- Available in 24KB kernel: **22,160 bytes** (sufficient for Foundation 1.4 + 1.5 + future features)
- Removed variables: pixel_save_x, pixel_save_y
- Removed test chars: test_W_char, test_eq_char
- Optimized functions maintain identical behavior, purely size/speed improvements

### Rationale
- Maximize space for Foundation 1.4 (Keyboard Driver ~800B) and 1.5 (Event System ~400B)
- Eliminate production overhead from debug/test code
- Optimize frequently-called graphics primitives
- Prepare for remaining Foundation Layer implementation

## [3.6.0] - 2026-01-23

### Changed
- **Kernel Expansion: 16KB → 24KB**
  - Kernel size increased from 16384 bytes (32 sectors) to 24576 bytes (48 sectors)
  - Provides headroom for Foundation 1.4 (Keyboard Driver, ~800 bytes) and Foundation 1.5 (Event System, ~400 bytes)
  - Heap start moved from 0x1400:0000 to 0x1600:0000
  - Available heap reduced from 540KB to 532KB (loses 8KB)

### Technical Details
- Modified boot/stage2.asm: KERNEL_SECTORS 32 → 48
- Modified kernel/kernel.asm: Final padding 16384 → 24576
- New memory layout:
  * 0x1000:0x0000 - Kernel (24KB, was 16KB)
  * 0x1600:0x0000 - Heap start (was 0x1400:0x0000)
  * ~532KB available for applications (was 540KB)
- Kernel headroom: ~7KB for future Foundation Layer components

### Rationale
- v3.5.0 reached exact 16KB capacity with Memory Allocator
- Foundation 1.4 and 1.5 require additional ~1200 bytes minimum
- 24KB expansion is conservative, leaves room for future enhancements
- 8KB heap reduction is negligible (still 532KB for apps)
- See docs/MEMORY_LAYOUT.md for detailed analysis

## [3.5.0] - 2026-01-23

### Added
- **Memory Allocator (Foundation 1.3)**
  - malloc(size): Allocate memory dynamically
  - free(ptr): Free allocated memory
  - First-fit allocation algorithm
  - Heap at 0x1400:0000, extends to ~640KB limit
  - Block header structure (size + flags)
  - Integrated with API table (offsets 6, 7)

### Technical Details
- Memory block header: 4 bytes [size:2][flags:2]
  * size: Total block size including header
  * flags: 0x0000 (free) or 0xFFFF (allocated)
- First-fit search algorithm for allocation
- Automatic heap initialization on first malloc
- Initial heap block: ~60KB (0xF000 bytes)
- 4-byte aligned allocations

### Implementation
- malloc(AX=size) → AX=pointer (offset from 0x1400:0000), 0 if failed
- free(AX=pointer) → frees memory block
- Heap starts at segment 0x1400 (linear 0x14000)
- Applications use ES=0x1400 + offset for memory access

### Size Impact
- Memory allocator: ~600 bytes
- Kernel size: Still 16KB (16384 bytes exact)
- Remaining capacity: ~0 bytes (at maximum)

## [3.4.0] - 2026-01-23

### Added
- **Graphics API Abstraction (Foundation 1.2)**
  - gfx_draw_pixel: Wraps plot_pixel_white for API table access
  - gfx_draw_char: Character rendering with coordinate parameters
  - gfx_draw_string: Null-terminated string rendering
  - gfx_draw_rect: Rectangle outline drawing
  - gfx_draw_filled_rect: Filled rectangle drawing
  - gfx_clear_area: Clear rectangular area (stub for now)

### Fixed
- Welcome message typo: "WELLCOME" → "WELCOME"

### Technical Details
- All graphics functions accessible via kernel API table
- Register-based calling convention:
  * gfx_draw_pixel(CX=X, BX=Y, AL=color)
  * gfx_draw_char(BX=X, CX=Y, AL=ASCII)
  * gfx_draw_string(BX=X, CX=Y, SI=string_ptr)
  * gfx_draw_rect(BX=X, CX=Y, DX=width, SI=height)
  * gfx_draw_filled_rect(BX=X, CX=Y, DX=width, SI=height)
- Kernel size: Still within 16KB limit

### Hardware Testing
- Tested on HP Omnibook 600C (486DX4-75)
- INT 0x80 discovery working ✓
- "OK" indicator displays correctly ✓

## [3.3.0] - 2026-01-23

### Added
- **System Call Infrastructure (Foundation 1.1)**
  - INT 0x80 handler for system call discovery mechanism
  - Kernel API table at fixed address 0x1000:0x0500
  - Hybrid approach: INT 0x80 for discovery + Far Call Table for execution
  - API table header with magic number ('KA' = 0x4B41), version (1.0), function count
  - 10 stub functions for future implementation:
    * Graphics API (6 functions): draw_pixel, draw_rect, draw_filled_rect, draw_char, draw_string, clear_area
    * Memory management (2 functions): malloc, free
    * Event system (2 functions): get_event, wait_event
- Visual test for INT 0x80 - displays "OK" at bottom right if successful

### Technical Details
- API table positioned at exactly offset 0x0500 (verified in binary)
- Follows Windows 1.x/2.x, GEOS pattern for performance
- Far Call approach saves ~40 cycles per call vs pure INT approach (~9% CPU at 4.77MHz)
- Foundation for third-party application development
- Enables future protected mode transition via thunking

### Documentation
- Added docs/ARCHITECTURE_PLAN.md - Complete architectural analysis and roadmap
- Added docs/SYSCALL.md - System call performance analysis
- Updated README.md with phase-based feature roadmap
- Updated docs/SESSION_SUMMARY.md with architectural decisions

## [3.2.0] - 2026-01-22

### Changed
- **Major architectural change: Split kernel from stage2 loader**
  - Stage2 is now a minimal 2KB loader with progress indicator
  - Kernel is a separate 16KB binary loaded at 0x1000:0000 (64KB mark)
  - Enables kernel to grow beyond 8KB limit
  - Future-proof architecture for XT through 486 hardware
- Boot sequence now shows "Loading kernel" with dot progress bar
- Removed text-mode debug output from bootloader (cleaner boot)
- RAM display now shows correct memory usage (~20KB for loader+kernel)

### Added
- New kernel/ directory for OS code
- kernel/kernel.asm - Main operating system (16KB)
- Kernel signature verification ('UK' = 0x4B55)

### Technical Details
- Disk layout: Boot (1 sector) + Stage2 (4 sectors) + Kernel (32 sectors)
- Stage2 loads kernel sector-by-sector with progress indicator
- Kernel loaded at segment 0x1000 (linear address 0x10000)

## [3.1.7] - 2026-01-22

### Changed
- Character demo redesigned for ASCII verification
  - Displays all 95 printable ASCII characters (32-126) in a 2-row grid
  - Row 1: characters 32-79 (48 chars) at Y=160
  - Row 2: characters 80-126 (47 chars) at Y=168
  - All characters remain visible during long pause (~100 delay cycles)
  - Then clears and repeats for continuous verification
- Clear demo area expanded to 16 pixels height (Y=160-175) for 2 rows

## [3.1.6] - 2026-01-22

### Fixed
- Clock and character demo now visible on HP Omnibook 600C
  - Added ES segment initialization in draw_clock and char_demo_loop
  - ES must point to 0xB800 (CGA video memory) for pixel plotting
- Slowed down animations for 486/DSTN display compatibility
  - Increased delay_short to nested loop (~4 million iterations)
  - Character demo now visible on slow DSTN displays with ghosting

### Changed
- Clock moved to top-left corner (X=4, Y=4)
- Character demo moved below welcome box (Y=160)
- Removed MDA text mode support (CGA-only now)
- Simplified video detection (no longer tracks video_type)

### Added
- Comprehensive coordinate visibility testing
- Confirmed full 320x200 CGA area visible on Omnibook 600C
- New documentation: ARCHITECTURE.md, FEATURES.md
- Comprehensive README update

## [3.1.5] - 2026-01-22

### Fixed
- Fixed graphical corruption of version text caused by overlapping elements
  - Clock moved to Y=40 (above white box which starts at Y=50)
  - Character demo moved to Y=120 (inside box, below version at Y=106)
  - Demo now starts at X=65 to stay within box boundaries (X=60-260)

## [3.1.4] - 2026-01-22

### Fixed
- Clock and character demo now visible on HP Omnibook 600C
  - Clock moved to Y=60 (just above welcome message)
  - Character demo moved to Y=108 (just below version text)
  - Positions within known visible area (Y=4-106 confirmed visible)

### Changed
- Separated clock_loop as independent function
- Added main_loop to coordinate clock and demo updates

## [3.1.3] - 2026-01-22

### Fixed
- Character demo now visible on real hardware (moved from Y=165 to Y=130)
  - Accounts for display overscan on vintage hardware
  - Tested on HP Omnibook 600C

## [3.1.2] - 2026-01-22

### Added
- Real-time clock display in top left corner
  - Reads time from CMOS RTC via BIOS INT 1Ah, AH=02h
  - Displays HH:MM:SS format using 4x6 small font
  - Updates continuously during character demo loop
  - Falls back to "--:--:--" if RTC unavailable
- New functions: draw_clock, draw_bcd_small, clear_clock_area

## [3.1.1] - 2026-01-22

### Added
- Character demo at boot: cycles through all ASCII characters (32-126)
  - Displays characters horizontally at bottom of screen using 4x6 font
  - Clears and repeats in an infinite loop
  - Visual delay between characters for effect
- New functions: char_demo_loop, clear_demo_area, delay_short

### Changed
- Moved RAM status display from bottom right to top right corner

## [3.1.0] - 2026-01-22

### Added
- Complete ASCII bitmap font set (characters 32-126)
  - 8x8 font in boot/font8x8.asm (95 characters, 760 bytes)
  - 4x6 small font in boot/font4x6.asm (95 characters, 570 bytes)
- Generic text rendering functions for any ASCII string:
  - draw_string_8x8: Render null-terminated string with 8x8 font
  - draw_string_4x6: Render null-terminated string with 4x6 font
  - draw_ascii_8x8: Render single ASCII character with 8x8 font
  - draw_ascii_4x6: Render single ASCII character with 4x6 font
- Font tables accessible via font_8x8 and font_4x6 labels
- Legacy character aliases (char_H, char_E, etc.) maintained for compatibility

### Changed
- Font data moved from inline definitions to separate include files
- Makefile updated with NASM include path for font files

## [3.0.1] - 2026-01-22

### Added
- RAM status display in bottom right corner of screen
  - Shows total RAM (from BIOS INT 12h)
  - Shows estimated used memory (~10K for boot code)
  - Shows free memory (total - used)
- New 4x6 small character bitmaps: digits 1-9, R, A, M, K, U, F, s, e, d, r, colon
- draw_number_small function for rendering numbers with small font

## [3.0.0] - 2026-01-22

### Changed
- Major version bump to UnoDOS 3
- New startup message: "Welcome to UnoDOS 3!" with version number below
- Added 4x6 small font for version display
- New 8x8 character bitmaps: C, M, T, U, N, S, 3
- New 4x6 small character bitmaps: v, 3, 0, .

## [0.2.4] - 2026-01-22

### Fixed
- Graphics corruption bug: BIOS teletype output (INT 10h AH=0Eh) was being
  called after switching to CGA graphics mode, causing text to render as
  stray pixels in the top-left corner of the screen
- Removed post-graphics print_string call that caused the corruption
- Hello World graphics now display correctly with no stray pixels

## [0.2.3] - 2026-01-22

### Added
- Pre-built floppy images now included in repository
  - build/unodos.img (360KB)
  - build/unodos-144.img (1.44MB)

### Changed
- Updated .gitignore to track final images, ignore intermediate build files

## [0.2.2] - 2026-01-22

### Fixed
- QEMU boot compatibility: Changed machine type from `-machine pc` to `-M isapc`
  for proper PC/XT BIOS boot behavior
- Boot now works correctly in QEMU with graphical Hello World display

### Changed
- Makefile now uses `isapc` machine type for all QEMU targets

## [0.2.1] - 2026-01-22

### Added
- Windows floppy write utilities
  - tools/writeflop.bat - Batch script for Windows command prompt
  - tools/Write-Floppy.ps1 - PowerShell script with verification
  - Both support 360KB and 1.44MB images
  - Require Administrator privileges for raw disk access

## [0.2.0] - 2026-01-22

### Added
- Boot sector (boot/boot.asm) - 512-byte IBM PC compatible boot loader
  - Loads from floppy drive (BIOS INT 13h)
  - Debug messages during boot process
  - Loads 8KB second stage from sectors 2-17
  - Validates second stage signature before jumping
- Second stage loader (boot/stage2.asm)
  - Memory detection via INT 12h
  - Video adapter detection (MDA/CGA/EGA/VGA)
  - CGA 320x200 4-color graphics mode
  - Graphical "HELLO WORLD!" with custom 8x8 bitmap font
  - MDA fallback with text-mode box drawing
- Build system (Makefile)
  - `make` - Build 360KB floppy image
  - `make floppy144` - Build 1.44MB floppy image
  - `make run` / `make run144` - Test in QEMU
  - `make debug` - QEMU with monitor for debugging
  - `make sizes` - Show binary sizes
  - Dependency checking for nasm and qemu
- Floppy write utility (tools/writeflop.sh)
  - Write images to physical floppy disks
  - Supports both 360KB and 1.44MB formats
  - Verification after write
  - Safety checks to prevent accidental overwrites

## [0.1.0] - 2026-01-22

### Added
- Initial project setup
- Project documentation (CLAUDE.md) with target specifications
- Documentation structure (docs/, VERSION, CHANGELOG.md, README.md)
- Target hardware: Intel 8088, 128KB RAM, MDA/CGA displays, floppy drive
- Architecture defined: GUI-first OS with direct BIOS interaction, no DOS dependency
