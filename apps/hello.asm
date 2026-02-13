; HELLO.BIN - Hello World application for UnoDOS
; Displays a message in a draggable window
;
; Build: nasm -f bin -o hello.bin hello.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Hello', 0                   ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; Speech bubble with "Hi" text inside
    db 0x00, 0x00, 0x00, 0x00      ; Row 0
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 1
    db 0x30, 0x00, 0x00, 0x30      ; Row 2
    db 0x33, 0x30, 0xF0, 0x30      ; Row 3
    db 0x33, 0x30, 0x30, 0x30      ; Row 4
    db 0x33, 0xF0, 0x30, 0x30      ; Row 5
    db 0x33, 0x30, 0x30, 0x30      ; Row 6
    db 0x33, 0x30, 0xFC, 0x30      ; Row 7
    db 0x30, 0x00, 0x00, 0x30      ; Row 8
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 9
    db 0x00, 0x30, 0x00, 0x00      ; Row 10
    db 0x00, 0xC0, 0x00, 0x00      ; Row 11
    db 0x03, 0x00, 0x00, 0x00      ; Row 12
    db 0x00, 0x00, 0x00, 0x00      ; Row 13
    db 0x00, 0x00, 0x00, 0x00      ; Row 14
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API function indices (must match kernel_api_table in kernel.asm)
API_GFX_DRAW_STRING     equ 4
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32

; Multitasking
API_APP_YIELD           equ 34

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Entry point - called by kernel via far CALL
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create window at X=80, Y=70, W=160, H=50
    mov bx, 80                      ; X position
    mov cx, 70                      ; Y position
    mov dx, 160                     ; Width
    mov si, 50                      ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al

    ; Set window drawing context
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Draw content
    call draw_content

    ; Event loop
.main_loop:
    sti
    mov ah, API_APP_YIELD           ; Yield to other tasks
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call draw_content
    jmp .main_loop
.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .no_event
    cmp dl, 27                      ; ESC key?
    je .exit_ok

.no_event:
    jmp .main_loop

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80

    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80
    xor ax, ax
    jmp .exit

.exit_fail:
    mov ax, 1

.exit:
    pop es
    pop ds
    popa
    retf

; Draw window content (window-relative coordinates)
draw_content:
    pusha

    mov bx, 20                      ; X within content area
    mov cx, 8                       ; Y within content area
    mov si, msg_hello
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 16
    mov cx, 22
    mov si, msg_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; Data Section
window_title:   db 'Hello', 0
win_handle:     db 0
msg_hello:      db 'Hello, World!', 0
msg_esc:        db 'Press ESC to exit', 0
