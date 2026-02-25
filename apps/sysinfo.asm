; SYSINFO.BIN - System Information for UnoDOS
; Build 325: Bisection test — pre-window APIs + simple window draws + 2-cluster padding
;
; Tests: Do pre-window API calls (get_boot_drive, get_screen_info, BDA read)
; break subsequent window drawing? Same 3 draw_strings as working Build 322.

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
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_GET_BOOT_DRIVE      equ 43
API_GET_SCREEN_INFO     equ 82

EVENT_KEY_PRESS         equ 1

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    ; --- Pre-window API calls (same as Build 324) ---
    ; These are the APIs that Build 322 did NOT have.
    ; If this build freezes, one of these breaks window drawing.

    ; Query boot drive
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov [boot_drive], al

    ; Query screen info
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [screen_w], bx
    mov [screen_h], cx

    ; Read BDA memory
    push es
    mov ax, 0x0040
    mov es, ax
    mov ax, [es:0x0013]
    pop es
    mov [mem_kb], ax

    ; --- Window creation (identical to Build 322) ---
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
    jc .exit_no_win
    mov [wh], al

    ; Explicitly reload window handle (defensive)
    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; --- Draw 3 strings (identical to Build 322) ---
    mov bx, 4
    mov cx, 4
    mov si, str_version
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 4
    mov cx, 14
    mov si, str_build
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 4
    mov cx, 30
    mov si, str_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Main loop (identical to Build 322) ---
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
boot_drive:     db 0
screen_w:       dw 0
screen_h:       dw 0
mem_kb:         dw 0

str_version:    db 'UnoDOS v3.21.0', 0
str_build:      db 'Build 325', 0
str_esc:        db 'Press ESC to close', 0

; Pad to force 2 FAT12 clusters (> 512 bytes)
; This tests multi-cluster file reads
times 600 - ($ - $$) db 0x90
