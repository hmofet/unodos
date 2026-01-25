# Claude Development Guidelines for UnoDOS

This document contains important instructions for AI assistants working on UnoDOS during long debugging sessions.

## Build Number Requirement

**CRITICAL**: For any persistent debugging issue that requires multiple build-test cycles:

1. **Always display a unique build number** on the boot screen
2. Build number MUST change with every new build
3. Use git commit hash as the build identifier
4. Display format: `Build: <git-hash>` (e.g., "Build: f24fabc")

**Why**: During hardware testing sessions, the developer needs to verify they're testing the correct build. Static version numbers (v3.10.1) don't change between debug builds, making it impossible to confirm the right binary is running.

**Implementation**:
- Run `./build-version.sh` before each build to update kernel/BUILD
- kernel/BUILD contains the git commit hash
- kernel/kernel.asm includes this as `build_string` and displays it on boot

**Location**: Display build number below the version string at coordinates (4, 14)

## Pre-Built Binary Workflow

**IMPORTANT**: The developer cannot install WSL or build tools on their Windows machine.

**Workflow**:
1. Make code changes on Linux development machine
2. Run `make clean && make floppy144` to build binaries
3. **Commit and push** the build/unodos-144.img file to git
4. Developer pulls the repo and uses Write-Floppy-Quick.ps1 to write to floppy
5. No build step on developer's machine - they get pre-built binaries from git

**Never**: Don't tell the developer to "run make" - they can't. Always build and commit for them.

## Stack Management in x86 Assembly

**Common Bug**: Mismatched push/pop causing stack corruption and crashes

**Rules**:
- Every `push` must have exactly one matching `pop`
- Watch for modified registers in loops (e.g., `loop` modifies CX)
- Split cleanup paths when code can exit a function through multiple routes
- Use comments to track stack state: `; Stack: [CX] [AX] [filename]`

**Example Bug** (from fat12_open):
```asm
; Wrong - double cleanup
.search_loop:
    push cx
    ; ... code ...
    pop cx
    dec cx
    jnz .search_loop
    ; Falls through
.cleanup:
    add sp, 2  ; ERROR: CX already popped above!
```

**Fix**:
```asm
; Correct - separate cleanup paths
.search_loop:
    push cx
    ; ... code ...
    pop cx
    dec cx
    jnz .search_loop
    ; Falls through - CX already popped
    add sp, 11  ; Clean only filename
    ret

.cleanup_from_jump:
    ; Jumped here - CX still on stack
    add sp, 2   ; Clean CX
    add sp, 11  ; Clean filename
    ret
```

## Testing Procedure for Hardware

**Two-Floppy FAT12 Test**:
1. Build UnoDOS boot floppy: `make clean && make floppy144`
2. Commit and push: `git add build/unodos-144.img && git commit && git push`
3. Developer runs: `.\tools\Write-Floppy-Quick.ps1` (writes UnoDOS to A:)
4. Developer creates test floppy: `.\tools\Create-TestFloppy.ps1` (creates FAT12 with TEST.TXT)
5. Boot from UnoDOS floppy
6. Press F to start filesystem test
7. When prompted, swap to test floppy
8. Verify output: "Mount: OK" → "Open TEST.TXT: OK" → "Read: OK" → file contents

**Always verify**: Developer should report the build number displayed on screen to confirm they're testing the right binary.

## Word Wrap Issues

**Problem**: CGA 320x200 mode with 8x8 font = 40 characters per line

**Solution**: Keep status messages short (< 35 chars) or manually wrap with multiple draw_string calls at different Y coordinates.

**Example**:
```asm
; Wrong - wraps mid-word
mov si, .msg
call gfx_draw_string_stub
.msg: db 'Press any key to continue with filesystem test', 0

; Right - manual wrap
mov si, .msg1
call gfx_draw_string_stub
mov cx, 80  ; Next line
mov si, .msg2
call gfx_draw_string_stub
.msg1: db 'Press any key to', 0
.msg2: db 'continue test', 0
```

## Application Loading (ORG 0 Requirement)

**CRITICAL**: Apps using `[ORG 0x0000]` must be loaded at offset 0 in their segment.

**Problem** (Build 018 crash): The clock app used `[ORG 0x0000]`, but the app loader was using the heap allocator which returned a non-zero offset (e.g., 0x1400:4). When the app referenced `window_title` at offset 0xDC, it pointed to 0x1400:0xDC instead of the correct 0x1400:(4+0xDC).

**Solution**: Apps are now loaded at segment 0x2000 offset 0:
- `app_load_stub` uses segment 0x2000 (dedicated app code segment)
- Apps always loaded at offset 0 within that segment
- This matches the `[ORG 0x0000]` directive in app source files

**Memory Map**:
```
0x0000 - 0x0FFF  : BIOS/IVT
0x1000 - 0x13FF  : Kernel code and data
0x1400 - 0x1FFF  : Heap (for kernel allocations)
0x2000 - 0x2FFF  : App code segment (apps loaded here at offset 0)
```

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

**Important**: `caller_ds` and `caller_es` are initialized to 0x1000 so direct kernel calls (not via INT 0x80) work correctly during boot.

## Git Workflow

**Pre-built binaries are committed**: Unlike typical projects, build/unodos-144.img is checked into git so Windows users can pull and write directly.

**.gitignore**:
```
# Build artifacts (intermediate files)
build/*.bin
build/*.o

# Keep final floppy images
!build/*.img
```

**Commit message format**:
```
Add build number display (Build: f24fabc)

- Created build-version.sh to generate git commit hash
- Display build number below version on boot screen
- Helps verify correct binary during hardware testing
```

---

**Last Updated**: 2026-01-25
**Current Version**: v3.12.0
**Current Build**: 019
