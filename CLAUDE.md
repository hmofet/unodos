# Claude Development Guidelines for UnoDOS

This document contains important instructions for AI assistants working on UnoDOS during long debugging sessions.

## Build Number Requirement

**CRITICAL**: For any persistent debugging issue that requires multiple build-test cycles:

1. **Always display a unique build number** on the boot screen
2. Build number MUST change with every new build
3. Build number stored in BUILD_NUMBER file
4. Display format: `Build: XXX` (e.g., "Build: 052")

**Why**: During hardware testing sessions, the developer needs to verify they're testing the correct build. Static version numbers (v3.12.0) don't change between debug builds, making it impossible to confirm the right binary is running.

**Location**: Display build number below the version string at coordinates (4, 14)

## Pre-Built Binary Workflow

**IMPORTANT**: The developer cannot install WSL or build tools on their Windows machine.

**Workflow**:
1. Make code changes on Linux development machine
2. Run `make clean && make floppy144 && make apps && make hd-image` to build all images
3. **Commit and push** all .img files to git
4. Developer pulls the repo and uses PowerShell scripts to write to media
5. No build step on developer's machine - they get pre-built binaries from git

**Images built**:
- `build/unodos-144.img` - Floppy with OS + Launcher
- `build/unodos-hd.img` - HDD/USB with OS + Launcher + All Apps

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

## Dual Segment App Architecture

**Memory Layout**:
```
0x0000 - 0x0FFF  : BIOS/IVT
0x1000 - 0x13FF  : Kernel code and data
0x1400 - 0x1FFF  : Heap (kernel allocations)
0x2000 - 0x2FFF  : Shell/Launcher segment (persists during app execution)
0x3000 - 0x3FFF  : User app segment (Clock, Browser, etc.)
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

## Testing Procedure for Hardware

**Floppy Test**:
1. Build: `make clean && make floppy144`
2. Commit and push .img file
3. Developer runs: `.\tools\floppy.ps1` (writes UnoDOS + Launcher to A:)
4. Boot from floppy - launcher auto-loads!
5. Test: navigate with W/S, launch apps with Enter, ESC to exit

**HDD/USB Test**:
1. Build: `make apps && make hd-image`
2. Commit and push .img file
3. Developer runs: `.\tools\hd.ps1 -ImagePath build\unodos-hd.img -DiskNumber N`
4. Boot from HDD/USB - launcher auto-loads!
5. All apps available immediately (Clock, Browser, Mouse, Test)

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
- `gfx_draw_string_stub` uses `caller_ds` for string access
- `win_create_stub` uses `caller_es` for window title

## Git Workflow

**Pre-built binaries are committed**: Unlike typical projects, build/*.img files are checked into git so Windows users can pull and write directly.

**Always commit all images**:
```bash
git add BUILD_NUMBER apps/*.asm build/unodos-144.img build/unodos-hd.img
git commit -m "Description (Build XXX)"
git push
```

---

**Last Updated**: 2026-01-28
**Current Version**: v3.13.0
**Current Build**: 073
