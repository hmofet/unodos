# Writing Applications for UnoDOS

This guide explains how to write, build, and run applications for UnoDOS 3.

## Overview

UnoDOS apps are flat .BIN binaries assembled with NASM. Each app runs in its own 64KB memory segment and communicates with the kernel via `INT 0x80` system calls. Apps can create windows, draw graphics, handle keyboard and mouse input, play sounds, and read/write files.

## Minimal Example

Here is the smallest possible windowed application:

```asm
[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes) ---
    db 0xEB, 0x4E               ; JMP short to offset 0x50
    db 'UI'                     ; Magic identifier
    db 'Hello', 0               ; App name (12 bytes, null-padded)
    times (0x04 + 12) - ($ - $$) db 0

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA)
    ; Each row is 4 bytes. Color 3 = white, 0 = black.
    times 64 db 0xFF            ; Solid white square placeholder

    times 0x50 - ($ - $$) db 0 ; Pad to code entry

; --- Code Entry (offset 0x50) ---
entry:
    pusha
    push ds
    push es
    mov ax, cs
    mov ds, ax                  ; DS = our segment (for local data)

    ; Create a window
    mov bx, 60                  ; X
    mov cx, 60                  ; Y
    mov dx, 200                 ; Width
    mov si, 60                  ; Height
    mov ax, cs
    mov es, ax
    mov di, title_str           ; ES:DI = title
    mov al, 0x03                ; Flags: title + border
    mov ah, 20                  ; API: win_create
    int 0x80
    jc .fail                    ; CF=1 means no free window slots
    mov [cs:handle], al

    ; Activate drawing context (window-relative coordinates)
    mov ah, 31                  ; API: win_begin_draw
    int 0x80

    ; Draw text at (10, 10) inside the window
    mov bx, 10
    mov cx, 10
    mov si, msg
    mov ah, 4                   ; API: gfx_draw_string
    int 0x80

    ; Main event loop
.loop:
    sti                         ; CRITICAL: re-enable interrupts
    mov ah, 9                   ; API: event_get
    int 0x80
    jc .loop                    ; No event, keep polling

    cmp al, 1                   ; EVENT_KEY_PRESS?
    jne .check_redraw
    cmp dl, 27                  ; ESC key?
    je .exit
    jmp .loop

.check_redraw:
    cmp al, 6                   ; EVENT_WIN_REDRAW?
    jne .loop
    ; Repaint content
    mov bx, 10
    mov cx, 10
    mov si, msg
    mov ah, 4
    int 0x80
    jmp .loop

.exit:
    mov ah, 32                  ; API: win_end_draw
    int 0x80
    mov al, [cs:handle]
    mov ah, 21                  ; API: win_destroy
    int 0x80

.fail:
    xor ax, ax                  ; Exit code 0
    pop es
    pop ds
    popa
    retf                        ; Return to kernel

; --- Data ---
handle:    db 0
title_str: db 'Hello', 0
msg:       db 'Hello, UnoDOS!', 0
```

## BIN File Format

Every UnoDOS application is a flat binary with an optional 80-byte icon header at the start. The kernel loads the entire file into a segment at offset 0 and calls the entry point at offset `0x0050`.

### Header Layout (0x00 - 0x4F)

```
Offset  Size    Content
0x00    2       JMP short 0x50 (bytes: 0xEB, 0x4E)
0x02    2       Magic: "UI" (0x55, 0x49)
0x04    12      App display name (null-padded ASCII)
0x10    64      Icon bitmap (16x16 pixels, 2bpp CGA format)
0x50    ...     Code entry point
```

The icon header is detected by checking bytes 0-3. If the header is missing (legacy apps), the kernel derives the name from the FAT filename and uses a default icon.

### Icon Bitmap

The 64-byte bitmap is a 16x16 image in CGA 2bpp format:
- 4 bytes per row, 16 rows, top-to-bottom
- Each byte holds 4 pixels: bits 7-6 = leftmost, bits 1-0 = rightmost
- Colors: 0 = black, 1 = green, 2 = red, 3 = white

Design tips:
- Use white (3) for outlines - most visible on the black desktop
- Use black (0) for transparent/empty areas
- Keep shapes simple at 16x16 resolution

## Application Lifecycle

1. **Kernel loads** the .BIN file into a free segment (0x3000-0x8000)
2. **Far CALL** to `segment:0x0050` - your entry point runs
3. **App initializes**: save registers, set DS, create window, set drawing context
4. **Event loop**: poll for events, draw content, handle input
5. **Cleanup**: end drawing context, destroy window
6. **RETF** returns to kernel, segment is freed

## Segment and Register Rules

### On Entry

| Register | Value |
|----------|-------|
| CS | Your app's segment (0x3000-0x8000) |
| DS | Unknown - **you must set it** |
| ES | Unknown - set as needed |
| SS:SP | Kernel stack |

**Always do this first:**

```asm
entry:
    pusha                       ; Save all GP registers
    push ds
    push es
    mov ax, cs
    mov ds, ax                  ; DS = CS = our segment
```

### On Exit

Restore everything and return with `RETF`:

```asm
    xor ax, ax                  ; Exit code in AX (0 = success)
    pop es
    pop ds
    popa
    retf
```

### Data Access

Your app's code and data share the same segment (CS = DS after setup). Reference local variables with `[cs:variable]` or just `[variable]` after setting DS = CS.

**Important:** When passing string pointers to kernel APIs, DS:SI is read from your segment automatically (the kernel saves your DS). No special handling needed.

For window titles, the kernel reads from ES:DI. Set ES to your segment:

```asm
    mov ax, cs
    mov es, ax
    mov di, my_title            ; ES:DI = title in our segment
```

## The Event Loop

Every interactive app needs an event loop. The pattern is always the same:

```asm
.loop:
    sti                         ; RE-ENABLE INTERRUPTS (mandatory!)

    ; Optional: yield to let other apps run
    mov ah, 34                  ; API: app_yield
    int 0x80

    ; Check for events
    mov ah, 9                   ; API: event_get
    int 0x80
    jc .no_event                ; CF=1 = no event queued

    ; Dispatch by event type (in AL)
    cmp al, 1                   ; EVENT_KEY_PRESS
    je .handle_key
    cmp al, 4                   ; EVENT_MOUSE
    je .handle_mouse
    cmp al, 6                   ; EVENT_WIN_REDRAW
    je .handle_redraw
    jmp .loop

.handle_key:
    ; DL = ASCII code, DH = scan code
    cmp dl, 27                  ; ESC?
    je .exit
    ; ... handle other keys ...
    jmp .loop

.handle_mouse:
    ; DL = button state (bit 0=left, 1=right, 2=middle)
    ; Use API 28 (mouse_get_state) to get position
    jmp .loop

.handle_redraw:
    ; DL = window handle
    ; Repaint your entire window content
    call draw_content
    jmp .loop

.no_event:
    ; Nothing to do - loop back
    jmp .loop
```

**Why STI is mandatory:** `INT 0x80` (like all x86 interrupts) clears the interrupt flag. Without `STI`, no keyboard or mouse IRQs fire, and the event queue stays empty forever.

## Window Drawing

### Creating a Window

```asm
    mov bx, 50                  ; X position
    mov cx, 30                  ; Y position
    mov dx, 220                 ; Width in pixels
    mov si, 100                 ; Height in pixels
    mov ax, cs
    mov es, ax
    mov di, my_title            ; ES:DI = window title
    mov al, 0x03                ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, 20                  ; API: win_create
    int 0x80
    jc .error                   ; CF=1 = out of window slots (16 max)
    mov [cs:win_handle], al     ; Save handle for later
```

### Drawing Context

After creating a window, activate the drawing context so all coordinates are relative to the window's content area:

```asm
    mov al, [cs:win_handle]
    mov ah, 31                  ; API: win_begin_draw
    int 0x80
```

Now `(0, 0)` refers to the top-left pixel of the content area (inside the border and below the title bar). The content area is:
- Width: window width - 2 pixels (1px border on each side)
- Height: window height - 11 pixels (10px title bar + 1px border)

### Handling Redraws

When the user drags another window over yours and then moves it away, the kernel sends `EVENT_WIN_REDRAW`. Your app must repaint its content from scratch:

```asm
.handle_redraw:
    call draw_content
    jmp .loop

draw_content:
    ; The drawing context is still active from win_begin_draw
    mov bx, 5
    mov cx, 5
    mov si, some_text
    mov ah, 4                   ; gfx_draw_string
    int 0x80
    ret
```

### Cleanup

Always destroy your window before exiting:

```asm
    mov ah, 32                  ; API: win_end_draw
    int 0x80
    mov al, [cs:win_handle]
    mov ah, 21                  ; API: win_destroy
    int 0x80
```

## Drawing API

All drawing APIs (0-6, 50-52) respect the active drawing context. Coordinates are window-relative when a context is set.

### Text

```asm
    ; Draw white text
    mov bx, 10                  ; X
    mov cx, 20                  ; Y
    mov si, my_string           ; DS:SI = string
    mov ah, 4                   ; gfx_draw_string
    int 0x80

    ; Draw inverted text (black on white)
    mov ah, 6                   ; gfx_draw_string_inverted
    int 0x80

    ; Word-wrapped text
    mov bx, 5                   ; X
    mov cx, 5                   ; Y
    mov dx, 180                 ; Max width before wrap
    mov si, long_text
    mov ah, 50                  ; gfx_draw_string_wrap
    int 0x80
    ; CX now contains the Y position after the last line
```

### Shapes

```asm
    ; Draw a rectangle outline
    mov bx, 10                  ; X
    mov cx, 10                  ; Y
    mov dx, 50                  ; Width
    mov si, 30                  ; Height
    mov ah, 1                   ; gfx_draw_rect
    int 0x80

    ; Draw a filled rectangle
    mov ah, 2                   ; gfx_draw_filled_rect
    int 0x80

    ; Clear an area to black
    mov ah, 5                   ; gfx_clear_area
    int 0x80

    ; Draw a single pixel
    mov bx, 100                 ; X
    mov cx, 50                  ; Y
    mov al, 3                   ; Color: white
    mov ah, 0                   ; gfx_draw_pixel
    int 0x80
```

### Fonts

Three fonts are available. The default is font 1 (8x8).

```asm
    ; Switch to small font
    mov al, 0                   ; Font 0: 4x6
    mov ah, 48                  ; gfx_set_font
    int 0x80

    ; Measure text width
    mov si, my_string
    mov ah, 33                  ; gfx_text_width
    int 0x80
    ; DX = width in pixels
```

| Font | Size | Advance | Chars per 320px line |
|------|------|---------|---------------------|
| 0 | 4x6 | 6px | 53 |
| 1 | 8x8 | 12px | 26 |
| 2 | 8x12 | 12px | 26 |

## GUI Widgets

### Buttons

```asm
    ; Draw a button
    mov bx, 10                  ; X
    mov cx, 40                  ; Y
    mov dx, 80                  ; Width
    mov si, 16                  ; Height
    mov ax, cs
    mov es, ax
    mov di, btn_label           ; ES:DI = label text
    mov al, 0                   ; 0 = normal, 1 = pressed
    mov ah, 51                  ; widget_draw_button
    int 0x80
```

### Hit Testing

Check if the mouse cursor is inside a rectangle (useful for button clicks):

```asm
    ; Is mouse inside the button area?
    mov bx, 10                  ; Button X
    mov cx, 40                  ; Button Y
    mov dx, 80                  ; Button width
    mov si, 16                  ; Button height
    mov ah, 53                  ; widget_hit_test
    int 0x80
    cmp al, 1
    je .button_clicked
```

## Mouse Input

```asm
    ; Check if mouse is available
    mov ah, 30                  ; mouse_is_enabled
    int 0x80
    cmp al, 0
    je .no_mouse

    ; Get mouse state
    mov ah, 28                  ; mouse_get_state
    int 0x80
    ; BX = X, CX = Y, AL = buttons

    ; Check left button
    test al, 1                  ; Bit 0 = left button
    jnz .left_clicked
```

## PC Speaker

```asm
    ; Play middle C (262 Hz)
    mov bx, 262
    mov ah, 41                  ; speaker_tone
    int 0x80

    ; Wait...

    ; Silence
    mov ah, 42                  ; speaker_off
    int 0x80
```

The speaker is automatically silenced when your app exits.

## File I/O

### Reading Files

```asm
    ; Mount the boot drive
    mov ah, 43                  ; get_boot_drive
    int 0x80
    ; AL = drive number

    mov ah, 13                  ; fs_mount
    int 0x80
    jc .mount_error
    mov [cs:mount_handle], bx

    ; Open a file
    mov bx, [cs:mount_handle]
    mov si, filename            ; "DATA.TXT"
    mov ah, 14                  ; fs_open
    int 0x80
    jc .open_error
    mov [cs:file_handle], ax

    ; Read 512 bytes
    mov ax, [cs:file_handle]
    mov bx, 512
    mov cx, cs
    mov es, cx
    mov di, buffer
    mov ah, 15                  ; fs_read
    int 0x80
    ; AX = bytes actually read

    ; Close
    mov ax, [cs:file_handle]
    mov ah, 16                  ; fs_close
    int 0x80

; Data
filename:     db 'DATA.TXT', 0
mount_handle: dw 0
file_handle:  dw 0
buffer:       times 512 db 0
```

## Building

Assemble with NASM as a flat binary:

```bash
nasm -f bin -o MYAPP.BIN apps/myapp.asm
```

To include on the OS floppy, place the .BIN file in the FAT12 filesystem of `build/unodos-144.img`. The Makefile handles this for apps in the `apps/` directory.

## Screen Constraints

| Metric | Value |
|--------|-------|
| Resolution | 320 x 200 pixels |
| Colors | 4 (black, green, red, white) |
| Bits per pixel | 2 |
| Max windows | 16 |
| Max concurrent apps | 6 (+ launcher) |
| App segment size | 64 KB |
| Default font | 8x8, 12px advance |
| Title bar height | 10 px |
| Window border | 1 px |

## Common Pitfalls

1. **Forgetting STI** - The most common bug. Without `STI` after `INT 0x80`, your event loop hangs because hardware interrupts are disabled.

2. **Not handling EVENT_WIN_REDRAW** - If you don't repaint when you receive this event, your window content disappears after another window is dragged over it.

3. **Not setting DS** - DS is undefined on entry. Always set `DS = CS` before accessing local variables.

4. **Stack corruption** - Every `PUSH` must have a matching `POP`. Mismatched pushes/pops cause crashes on `RETF`.

5. **Using BP for data pointers** - `[BP + offset]` defaults to the SS segment, not DS. This is a known x86 quirk. If you must use BP for data, write `[ds:bp + offset]`.

6. **FAT filename format** - `fs_readdir` returns space-padded 11-byte names (`CLOCK   BIN`). `fs_open` expects dot format (`CLOCK.BIN`). You must convert between them.

---

*For the complete API register reference (91 functions, APIs 0-90), see [API_REFERENCE.md](API_REFERENCE.md).*
*For a concise SDK reference with code patterns, see [UNODOS_APP_SDK.md](UNODOS_APP_SDK.md).*

*v3.21.0 Build 275*
