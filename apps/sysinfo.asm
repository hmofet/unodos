; SYSINFO.BIN - System Information for UnoDOS
; Build 328: Direct CGA VRAM diagnostic markers to pinpoint freeze location.
; Writes pixels directly to 0xB800 before/after each API call.
; Each marker = 4 white pixels (0xFF) at row 0, spaced 8px apart.
; Count visible markers to find last successful operation.

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
; Direct VRAM writes bypass INT 0x80 — works even if kernel is stuck

API_GFX_DRAW_STRING     equ 4
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34

EVENT_KEY_PRESS         equ 1

; Macro: write marker N to CGA VRAM row 0
; Each marker = 0xFF at byte offset N*2, giving 4 white pixels with gaps
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

    ; ---- Marker 1: entry reached ----
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

    ; ---- Marker 2: after win_create ----
    MARKER 1

    jc .exit_no_win
    mov [wh], al

    ; ---- Marker 3: before win_begin_draw ----
    MARKER 2

    ; Begin draw
    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; ---- Marker 4: after win_begin_draw ----
    MARKER 3

    ; Draw string 1
    mov bx, 4
    mov cx, 4
    mov si, str_version
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; ---- Marker 5: after draw_string 1 ----
    MARKER 4

    ; Draw string 2
    mov bx, 4
    mov cx, 14
    mov si, str_build
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; ---- Marker 6: after draw_string 2 ----
    MARKER 5

    ; Draw string 3
    mov bx, 4
    mov cx, 30
    mov si, str_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; ---- Marker 7: after draw_string 3, entering main loop ----
    MARKER 6

    ; Main loop
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

str_version:    db 'UnoDOS v3.21.0', 0
str_build:      db 'Build 328', 0
str_esc:        db 'Press ESC to close', 0

; Pad to force 2 FAT12 clusters (> 512 bytes)
times 600 - ($ - $$) db 0x90
