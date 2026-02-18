# Claude Development Guidelines for UnoDOS

This document contains important instructions for AI assistants working on UnoDOS during long debugging sessions.

## Build Number Requirement

**CRITICAL**: For any persistent debugging issue that requires multiple build-test cycles:

1. **Always display a unique build number** on the boot screen
2. Build number MUST change with every new build
3. Build number stored in BUILD_NUMBER file
4. Display format: `Build: XXX` (e.g., "Build: 135")

**Why**: During hardware testing sessions, the developer needs to verify they're testing the correct build. Static version numbers (v3.15.0) don't change between debug builds, making it impossible to confirm the right binary is running.

**Location**: Display build number below the version string at coordinates (4, 14)

## Pre-Built Binary Workflow

**IMPORTANT**: The developer cannot install WSL or build tools on their Windows machine.

**Workflow**:
1. Make code changes on Linux development machine
2. Run `make clean && make floppy144` to build OS floppy (includes apps)
3. Run `make apps && make build/launcher-floppy.img` to build launcher floppy
4. **Commit and push** all .img and .bin files to git
5. Developer pulls the repo and uses PowerShell scripts to write to media
6. No build step on developer's machine - they get pre-built binaries from git

**Images built**:
- `build/unodos-144.img` - 1.44MB floppy with OS + apps (FAT12)
- `build/launcher-floppy.img` - Separate apps-only floppy

**Never**: Don't tell the developer to "run make" - they can't. Always build and commit for them.

## App Keyboard Handling

**CRITICAL**: Apps MUST use this pattern for keyboard input to work:

```asm
.main_loop:
    sti                             ; RE-ENABLE INTERRUPTS (INT 0x80 clears IF)
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event                    ; CF set = no event
    cmp al, EVENT_KEY_PRESS
    jne .no_event
    cmp dl, 27                      ; ESC?
    je .exit_ok
```

**Why**: INT 0x80 (and all interrupts) automatically clears the IF flag. Without STI, no keyboard IRQs fire and no events are queued.

## Window Drawing Context API

**CRITICAL**: Apps MUST use `WIN_BEGIN_DRAW` / `WIN_END_DRAW` for window-relative drawing:

```asm
    ; After win_create, set drawing context
    mov ah, API_WIN_BEGIN_DRAW      ; Function 31
    int 0x80

    ; Now all drawing calls (functions 0-6) use window-relative coordinates
    ; BX=0, CX=0 is the top-left of the content area (inside border/title)
    mov bx, 10                      ; X relative to content area
    mov cx, 5                       ; Y relative to content area
    mov si, my_string
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Before destroying window, clear context
    mov ah, API_WIN_END_DRAW        ; Function 32
    int 0x80
```

**Why**: The INT 0x80 handler auto-translates BX/CX for drawing APIs 0-6 AND 50-52 (widget APIs) when a draw context is active. This means apps don't need absolute screen coordinates and content is preserved correctly during window drags.

**Window dragging**: Outline drag (XOR rectangle) — window moves on mouse release. Apps receive WIN_REDRAW to repaint content. Background window draws are silently blocked; apps repaint on focus.

## Multi-App Segment Architecture (Build 149+)

**Memory Layout**:
```
0x0000 - 0x0FFF  : BIOS/IVT
0x1000 - 0x16FF  : Kernel code and data (28KB)
0x1400 - 0x1FFF  : Heap (kernel allocations)
0x2000 - 0x2FFF  : Shell/Launcher segment (fixed, persists always)
0x3000 - 0x3FFF  : User app slot 0 (dynamic pool)
0x4000 - 0x4FFF  : User app slot 1
0x5000 - 0x5FFF  : User app slot 2
0x6000 - 0x6FFF  : User app slot 3
0x7000 - 0x7FFF  : User app slot 4
0x8000 - 0x8FFF  : User app slot 5
0x9000           : Scratch buffer (reserved)
0xB800           : CGA video memory
```

- **Shell (0x2000)**: Launcher loads here, survives while user apps run
- **User (0x3000-0x8000)**: Up to 6 concurrent user apps, each in its own segment
- Segments allocated dynamically from pool by `alloc_segment` / freed by `free_segment`
- `DH > 0x20` in app_load triggers auto-allocation; `DH = 0x20` = shell (fixed)
- Apps use `[ORG 0x0000]` and are loaded at offset 0 in their segment

## Stack Management in x86 Assembly

**Common Bug**: Mismatched push/pop causing stack corruption and crashes

**Rules**:
- Every `push` must have exactly one matching `pop`
- Watch for modified registers in loops (e.g., `loop` modifies CX)
- Split cleanup paths when code can exit a function through multiple routes
- Use comments to track stack state: `; Stack: [CX] [AX] [filename]`

## BP/SS Segment Bug

**CRITICAL**: In x86 real mode, `[bp + displacement]` defaults to the SS segment, NOT DS!

Since the kernel uses DS=0x1000 but SS=0x0000, any `[bp + offset]` access reads from the wrong segment. Always use explicit `[ds:bp + offset]` override when accessing kernel data via BP.

## CHS LBA-to-CHS Conversion Bug Pattern (Build 231+)

**CRITICAL**: When converting LBA to CHS using two successive `div` instructions, **never use ECX as the divisor** because CL holds the sector number between divisions.

**The bug**: `movzx ecx, byte [heads]` clobbers CL (sector number stored earlier), causing every CHS read to use the wrong sector.

**Correct pattern** (use EBX for divisor):
```asm
    push ebx
    movzx ebx, byte [spt]
    div ebx                     ; EAX = LBA / SPT, EDX = remainder
    inc dl
    mov cl, dl                  ; CL = sector (safe — EBX doesn't touch CL)

    xor edx, edx
    movzx ebx, byte [heads]    ; EBX, not ECX!
    div ebx                     ; EAX = cylinder, EDX = head
    mov dh, dl
    mov ch, al
    shl ah, 6
    or cl, ah                   ; CL = sector | (cyl_high << 6)
    pop ebx
```

**Affected locations** (both fixed in Builds 231-232):
- `boot/stage2_hd.asm` — `read_sector_to_esbx` CHS fallback
- `kernel/kernel.asm` — `fat16_read_sector` CHS fallback

**The MBR (`boot/mbr.asm`) was always correct** — it used EBX from the start.

## Testing Procedure for Hardware

**Writing images** (unified `write.ps1` script):
- Developer runs: `.\tools\write.ps1` (interactive TUI — selects image + target drive)
- Or quick mode: `.\tools\write.ps1 -DriveLetter A` (floppy) or `.\tools\write.ps1 -DiskNumber 2` (CF/USB)
- Legacy wrappers still work: `floppy.ps1`, `hd.ps1`, `apps.ps1`

**Floppy Test**:
1. Build: `make clean && make floppy144 && make apps && make build/launcher-floppy.img`
2. Commit and push .img files
3. Developer runs: `.\tools\write.ps1` → selects `unodos-144.img` → selects floppy drive
4. Boot from floppy — launcher auto-loads
5. Test: navigate with W/S, launch apps with Enter, ESC to exit
6. Test mouse: cursor visible, drag windows by title bar

**HD / CF Card Test**:
1. Build: `make clean && make floppy144 && make apps && make build/launcher-floppy.img && make hd-image`
2. Commit and push .img files
3. Developer runs: `.\tools\write.ps1` → selects `unodos-hd.img` → selects CF/USB drive
4. Boot from CF card
5. Stage2 diagnostic shows: `L0144` or `C0144` (L=LBA, C=CHS, 0144=root_start_lba hex)
6. Launcher should auto-load from FAT16 partition

**Always verify**: Developer should report the build number displayed on screen to confirm they're testing the right binary.

## FAT12 Filename Format

**Two formats in use**:
- **FAT format**: `CLOCK   BIN` (8+3, space-padded, no dot)
- **Dot format**: `CLOCK.BIN` (human-readable)

The `fs_readdir` API returns FAT format. The `app_load` API expects dot format. Use `get_selected_filename` in launcher to convert.

## Word Wrap Issues

**Problem**: CGA 320x200 mode with 8x8 font = 40 characters per line

**Solution**: Keep status messages short (< 35 chars) or manually wrap with multiple draw_string calls at different Y coordinates.

## Segment Handling for App API Calls

**Problem**: When apps call kernel APIs via INT 0x80, the kernel sets DS=0x1000 (kernel segment). But apps pass string pointers in their own segment (ES:DI for titles, DS:SI for strings).

**Solution**: The INT 0x80 handler saves caller's segments:
```asm
.dispatch_function:
    mov [cs:caller_ds], ds      ; Save app's DS
    mov [cs:caller_es], es      ; Save app's ES
    push ds
    mov ax, 0x1000
    mov ds, ax                  ; Set kernel DS
```

API functions that need app strings use these saved values:
- `gfx_draw_string_stub` (function 4) uses `caller_ds` for string access
- `gfx_draw_string_inverted` (function 6) uses `caller_ds` for string access
- `gfx_text_width` (function 33) uses `caller_ds` for string access
- `gfx_draw_string_wrap` (function 50) uses `caller_ds` for string access
- `win_create_stub` uses `caller_es` for window title

## CGA Byte Alignment

**Problem**: CGA mode 4 stores 4 pixels per byte (2bpp). When saving/restoring window content during drags, byte boundaries don't align with pixel boundaries.

**Key insight**: Content save/restore operates on whole CGA bytes. When window position changes, the byte alignment can change, so `restore_drag_content` must calculate `new_bpr` (bytes per row) for the new position and use `min(saved_bpr, new_bpr)` to avoid writing past the window's right edge.

## BIOS PS/2 Mouse Architecture (Build 192+)

**Primary method**: BIOS INT 15h/C2xx services (works with USB legacy emulation)
**Fallback**: Direct KBC port I/O (for BIOSes without INT 15h/C2 support)

**BIOS callback stack layout** (verified via QEMU SeaBIOS raw dump `00 FB 0A 28`):
```
BIOS pushes: status, X, Y, 0, then CALL FAR handler
After push bp / mov bp, sp:
  [BP+6]  = 0 (padding)
  [BP+8]  = Y delta (0-255, sign in status bit 5)
  [BP+10] = X delta (0-255, sign in status bit 4)
  [BP+12] = status byte (YO XO YS XS 1 M R L)
```

**Boot diagnostic**: Letter printed before CGA mode switch + keypress wait:
- `B` = BIOS method succeeded
- `K` = KBC direct method succeeded
- `R` = Mouse reset failed
- `S` = Mouse stream enable failed
- `E` = No mouse detected

## Git Workflow

**Pre-built binaries are committed**: Unlike typical projects, build/*.img and build/*.bin files are checked into git so Windows users can pull and write directly.

**Always commit all images and binaries**:
```bash
make clean && make floppy144 && make apps && make build/launcher-floppy.img && make hd-image
git add BUILD_NUMBER kernel/kernel.asm apps/*.asm boot/*.asm \
  build/unodos-144.img build/launcher-floppy.img build/unodos-hd.img \
  build/*.bin
git commit -m "Description (Build XXX)"
git push
```

## Filesystem Write Support (Build 201+ FAT12, Build 258+ FAT16)

**APIs for writing files (work on both FAT12 and FAT16)**:
- `fs_write_sector_stub` (API 44) — Write raw sector to disk
- `fs_create_stub` (API 45) — Create new file (BL=mount handle, SI=filename)
- `fs_write_stub` (API 46) — Write to open file (AL=handle, ES:BX=data, CX=size)
- `fs_delete_stub` (API 47) — Delete file (BL=mount handle, SI=filename)
- `fs_rename_stub` (API 77) — Rename file (BL=mount handle, SI=old, ES:DI=new)

**Mount handle routing**: All filesystem stubs route by mount handle byte (BL):
- `BL=0` → FAT12 (floppy) — `fat12_create`, `fat12_write`, `fat12_delete`, `fat12_rename`
- `BL=1` → FAT16 (hard drive) — `fat16_create`, `fat16_write`, `fat16_delete`, `fat16_rename`
- `fs_write_stub` routes by file handle's mount_handle field (offset 1), not BL
- Always compare BL (byte), never BX (word) — dirty BH causes silent failures.

**FAT16 internals** (Build 258):
- `fat16_write_sector` — INT 13h AH=43h (LBA write) with AH=03h (CHS) fallback
- `fat16_set_fat_entry` — 16-bit FAT entries (256 per sector), writes to both FAT copies
- `fat16_alloc_cluster` — Scans from cluster 2, marks as EOC (0xFFF8)
- Multi-sector clusters: `fat16_sects_per_clust * 512` bytes per cluster

## GUI Toolkit (Build 205+)

**Multi-font system** with 3 built-in fonts:
- Font 0: 4x6 small font
- Font 1: 8x8 medium font (default)
- Font 2: 8x14 large font

**APIs**:
- `gfx_set_font` (API 48) — Set current font (AL=index 0-2)
- `gfx_get_font_metrics` (API 49) — Get font metrics (returns char width/height)
- `gfx_draw_string_wrap` (API 50) — Draw string with word wrap
- `widget_draw_button` (API 51) — Draw button with label and clip bounds
- `widget_draw_radio` (API 52) — Draw radio button
- `widget_hit_test` (API 53) — Hit test rectangle (BX,CX=point, DX,SI=rect)
- `theme_set_colors` (API 54) — Set theme colors (AL=text, BL=bg, CL=win)
- `theme_get_colors` (API 55) — Get current theme colors

**Coordinate translation**: APIs 50-52 are auto-translated by draw_context (like APIs 0-6).

## Settings Persistence (Build 210+)

**SETTINGS.CFG** on boot floppy: 5-byte file (magic 0xA5 + font + text_color + bg_color + win_color).
- Kernel `load_settings` reads at boot (after filesystem mount, before launcher start)
- Settings app saves via FAT12 write APIs on Apply/OK

## HD Boot Chain (Build 228+)

**Boot sequence**: MBR (0x7C00 → relocated 0x0600) → VBR (0x7C00) → Stage2 (0x0800:0x0000) → Kernel (0x1000:0x0000)

**Key design decisions**:
- MBR uses `[ORG 0x0600]` so all data/code lives in relocated region, safe from VBR overwrite at 0x7C00
- Stage2 parses BPB from VBR (0x7C00) **before** any INT 13h calls — old BIOSes may use 0x7C00 as scratch
- All boot components use static DAP (not stack-built `push dword`) for INT 13h AH=42h
- DAP pointer uses DS=0 with linear address conversion for BIOS compatibility
- CHS fallback uses BIOS-queried geometry (INT 13h AH=08h), not hardcoded values
- PCMCIA CF cards (e.g. Omnibook 600C) may lack INT 13h extension support, requiring CHS fallback

**Stage2 boot diagnostic** (printed before kernel load):
- `L` or `C` = read method (LBA or CHS)
- `0144` = root_start_lba in hex (expected value for standard 64MB image)

**HD image tool** (`tools/hd.ps1`): Interactive TUI with arrow-key drive selector, Y/N overwrite confirmation (defaults to No for safety).

## Cursor Protection for Drawing APIs

**All drawing APIs (0-6, 51) must hide the mouse cursor** before drawing and restore after. The pattern:
```asm
    call mouse_cursor_hide
    inc byte [cursor_locked]
    ; ... drawing code ...
    dec byte [cursor_locked]
    call mouse_cursor_show
```
Without this, IRQ12 can XOR the cursor over drawing pixels between API calls, causing progressive corruption of window frames and UI elements.

---

**Last Updated**: 2026-02-18
**Current Version**: v3.21.0
**Current Build**: 258
