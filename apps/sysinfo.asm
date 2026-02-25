; SYSINFO.BIN - System Information for UnoDOS
; Build 330: Pinpoint freeze location in gfx_draw_string.
; Tests: empty string, 1-char string, full string — all without draw context.
; If empty string works but others don't, issue is in character loop.
; If empty string freezes, issue is in function setup (mouse_cursor_hide).

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Sys Info', 0                ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    db 0x00, 0x00, 0x00, 0x00
    db 0x15, 0x55, 0x55, 0x54
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0x00, 0x3F, 0xFC, 0x00
    db 0x00, 0x0F, 0xF0, 0x00
    db 0x00, 0x3F, 0xFC, 0x00
    db 0x03, 0xFF, 0xFF, 0xC0
    db 0x00, 0x00, 0x00, 0x00

    times 0x50 - ($ - $$) db 0

; --- Code Entry (offset 0x50) ---

API_GFX_DRAW_STRING     equ 4
API_GFX_SET_PIXEL       equ 0
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34

EVENT_KEY_PRESS         equ 1

; Macro: write marker N to CGA VRAM row 0
%macro MARKER 1
    push es
    push bx
    mov bx, 0xB800
    mov es, bx
    mov byte [es:%1*2], 0xFF
    pop bx
    pop es
%endmacro

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    ; ---- Marker 0: entry reached ----
    MARKER 0

    ; Create window
    mov bx, 55
    mov cx, 43
    mov dx, 210
    mov si, 114
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80

    ; ---- Marker 1: after win_create ----
    MARKER 1

    jc .exit_no_win
    mov [wh], al

    ; ---- TEST 1: Draw EMPTY string (just null terminator) ----
    ; Tests if function setup (mouse_cursor_hide etc.) hangs
    mov bx, 100
    mov cx, 80
    mov si, str_empty
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; ---- Marker 2: empty string succeeded ----
    MARKER 2

    ; ---- TEST 2: Draw single character "A" ----
    ; Tests if first character processing hangs
    mov bx, 100
    mov cx, 90
    mov si, str_one
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; ---- Marker 3: single char succeeded ----
    MARKER 3

    ; ---- TEST 3: Draw 3-char string "Uno" ----
    ; Tests if loop continuation hangs
    mov bx, 100
    mov cx, 100
    mov si, str_three
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; ---- Marker 4: 3-char string succeeded ----
    MARKER 4

    ; ---- TEST 4: Draw full string ----
    ; The original failing case
    mov bx, 100
    mov cx, 110
    mov si, str_version
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; ---- Marker 5: full string succeeded ----
    MARKER 5

    ; Skip to main loop (no win_begin_draw needed for this test)
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .main_loop

    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    cmp dl, 27
    je .exit_ok
    jmp .main_loop

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:wh]
    mov ah, API_WIN_DESTROY
    int 0x80

.exit_no_win:
    pop es
    pop ds
    popa
    retf

; ============================================================================
; Data
; ============================================================================

win_title:      db 'System Info', 0
wh:             db 0

str_empty:      db 0                        ; Empty string (just null)
str_one:        db 'A', 0                   ; Single character
str_three:      db 'Uno', 0                 ; 3 characters
str_version:    db 'UnoDOS v3.21.0', 0
str_build:      db 'Build 330', 0
str_esc:        db 'Press ESC to close', 0

; Pad to force 2 FAT12 clusters (> 512 bytes)
times 600 - ($ - $$) db 0x90
