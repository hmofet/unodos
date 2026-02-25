; SYSINFO.BIN - System Information for UnoDOS
; Displays OS version, boot drive, video, memory, tasks, time, ticks
;
; Build: nasm -f bin -o sysinfo.bin sysinfo.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Sys Info', 0                ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; Monitor icon: white outline, cyan screen
    db 0x00, 0x00, 0x00, 0x00      ; Row 0:  ................
    db 0x15, 0x55, 0x55, 0x54      ; Row 1:  .ccccccccccccc..
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 2:  WWWWWWWWWWWWWWWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 3:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 4:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 5:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 6:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 7:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 8:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 9:  WW.ccccccccc.WWW
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 10: WWWWWWWWWWWWWWWW
    db 0x00, 0x3F, 0xFC, 0x00      ; Row 11: ....WWWWWWWW....
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 12: ......WWWW......
    db 0x00, 0x3F, 0xFC, 0x00      ; Row 13: ....WWWWWWWW....
    db 0x03, 0xFF, 0xFF, 0xC0      ; Row 14: ..WWWWWWWWWWWW..
    db 0x00, 0x00, 0x00, 0x00      ; Row 15: ................

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_STRING     equ 4
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    ; --- Create window ---
    mov bx, 55
    mov cx, 60
    mov dx, 210
    mov si, 80
    mov di, win_title
    mov al, 0x03                ; BORDER | TITLE
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_no_win
    mov [wh], al

    ; Set drawing context
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Draw minimal content
    call draw_content

    ; --- Main loop ---
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .main_loop

    cmp al, EVENT_KEY_PRESS
    jne .check_redraw
    cmp dl, 27
    je .exit_ok
    jmp .main_loop

.check_redraw:
    cmp al, EVENT_WIN_REDRAW
    jne .main_loop
    call draw_content
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
; draw_content - Just draw version + build
; ============================================================================
draw_content:
    pusha

    mov bx, 4
    mov cx, 2
    mov si, str_version
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 4
    mov cx, 12
    mov si, str_build
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 4
    mov cx, 30
    mov si, str_press_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; Data
; ============================================================================

win_title:      db 'System Info', 0
wh:             db 0

str_version:    db 'UnoDOS v3.21.0', 0
str_build:      db 'Build 322', 0
str_press_esc:  db 'Press ESC to close', 0
