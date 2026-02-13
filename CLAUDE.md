# Claude Development Guidelines for UnoDOS

This document contains important instructions for AI assistants working on UnoDOS during long debugging sessions.

## Build Number Requirement

**CRITICAL**: For any persistent debugging issue that requires multiple build-test cycles:

1. **Always display a unique build number** on the boot screen
2. Build number MUST change with every new build
3. Build number stored in BUILD_NUMBER file
4. Display format: `Build: XXX` (e.g., "Build: 135")

**Why**: During hardware testing sessions, the developer needs to verify they're testing the correct build. Static version numbers (v3.13.0) don't change between debug builds, making it impossible to confirm the right binary is running.

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

**Why**: The INT 0x80 handler auto-translates BX/CX for drawing APIs 0-6 when a draw context is active. This means apps don't need absolute screen coordinates and content is preserved correctly during window drags.

**Content preservation**: The OS automatically saves and restores window content during mouse-driven drags. Apps do NOT need to handle redraw events.

## Dual Segment App Architecture

**Memory Layout**:
```
0x0000 - 0x0FFF  : BIOS/IVT
0x1000 - 0x13FF  : Kernel code and data (API table at 0x0F80)
0x1400 - 0x1FFF  : Heap (kernel allocations)
0x2000 - 0x2FFF  : Shell/Launcher segment (persists during app execution)
0x3000 - 0x3FFF  : User app segment (Clock, Browser, etc.)
0x5000           : Scratch buffer (used by OS for drag content preservation)
0xB800           : CGA video memory
```

- **Shell (0x2000)**: Launcher loads here, survives while user apps run
- **User (0x3000)**: Apps launched from launcher load here
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

## Testing Procedure for Hardware

**Floppy Test**:
1. Build: `make clean && make floppy144 && make apps && make build/launcher-floppy.img`
2. Commit and push .img files
3. Developer runs: `.\tools\boot.ps1` (writes OS to A:)
4. Swap to blank floppy
5. Developer runs: `.\tools\launcher.ps1` (writes launcher floppy)
6. Boot from OS floppy
7. Press 'L' to load launcher, swap to launcher floppy when prompted
8. Test: navigate with W/S, launch apps with Enter, ESC to exit
9. Test mouse: cursor visible, drag windows by title bar

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
- `win_create_stub` uses `caller_es` for window title

## CGA Byte Alignment

**Problem**: CGA mode 4 stores 4 pixels per byte (2bpp). When saving/restoring window content during drags, byte boundaries don't align with pixel boundaries.

**Key insight**: Content save/restore operates on whole CGA bytes. When window position changes, the byte alignment can change, so `restore_drag_content` must calculate `new_bpr` (bytes per row) for the new position and use `min(saved_bpr, new_bpr)` to avoid writing past the window's right edge.

## Git Workflow

**Pre-built binaries are committed**: Unlike typical projects, build/*.img and build/*.bin files are checked into git so Windows users can pull and write directly.

**Always commit all images and binaries**:
```bash
git add BUILD_NUMBER apps/*.asm kernel/kernel.asm \
  build/unodos-144.img build/launcher-floppy.img \
  build/*.bin
git commit -m "Description (Build XXX)"
git push
```

---

**Last Updated**: 2026-02-13
**Current Version**: v3.14.0
**Current Build**: 144
