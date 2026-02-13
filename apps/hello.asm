; HELLO.BIN - Hello World application for UnoDOS
; Displays a message in a draggable window
;
; Build: nasm -f bin -o hello.bin hello.asm

[BITS 16]
[ORG 0x0000]

; API function indices (must match kernel_api_table in kernel.asm)
API_GFX_DRAW_STRING     equ 4
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_MOVED         equ 5

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

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .check_win_moved
    cmp dl, 27                      ; ESC key?
    je .exit_ok
    jmp .no_event

.check_win_moved:
    cmp al, EVENT_WIN_MOVED
    jne .no_event
    call draw_content

.no_event:
    mov cx, 0x1000
.delay:
    loop .delay
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
