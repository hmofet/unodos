; CLOCK.BIN - Clock application for UnoDOS v3.12.0
; Build 025 - Test win_get_content fix
;
; Build: nasm -f bin -o clock.bin clock.asm

[BITS 16]
[ORG 0x0000]

; API function indices
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 8
API_WIN_CREATE          equ 19
API_WIN_GET_CONTENT     equ 24

; Event types
EVENT_KEY_PRESS         equ 1

; Entry point - called by kernel via far CALL
entry:
    pusha
    push ds
    push es

    ; Save our code segment
    mov ax, cs
    mov ds, ax

    ; Create clock window at X=100, Y=60, W=120, H=50
    mov bx, 100                     ; X position
    mov cx, 60                      ; Y position
    mov dx, 120                     ; Width
    mov si, 50                      ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title            ; Title pointer (ES:DI)
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .win_create_failed
    mov [cs:win_handle], al         ; Save handle (only low byte needed)

    ; DEBUG: Draw marker at VERY TOP of screen (Y=0) to show we got here
    push es
    mov ax, 0xB800
    mov es, ax
    mov di, 0
    mov byte [es:di], 0xFF          ; White at top-left
    mov byte [es:di+1], 0xFF
    mov byte [es:di+2], 0xFF
    pop es

    ; NOW TEST win_get_content with the REAL handle
    mov al, [cs:win_handle]         ; Window handle in AL
    mov ah, API_WIN_GET_CONTENT
    int 0x80
    jc .get_content_failed

    ; win_get_content returns:
    ; BX = Content X, CX = Content Y, DX = Content Width, SI = Content Height
    mov [cs:content_x], bx
    mov [cs:content_y], cx
    mov [cs:content_w], dx
    mov [cs:content_h], si

    ; Draw marker at returned content coords using direct video memory
    push es
    mov ax, 0xB800
    mov es, ax
    ; Calculate video offset for content_y, content_x
    mov ax, [cs:content_y]
    mov bx, ax                      ; Save Y for odd/even check
    shr ax, 1                       ; Y/2
    mov cx, 80
    mul cx                          ; AX = (Y/2) * 80
    mov di, ax
    mov ax, [cs:content_x]
    shr ax, 1
    shr ax, 1                       ; X/4
    add di, ax
    test bx, 1                      ; Was Y odd?
    jz .even_line
    add di, 0x2000                  ; Odd scanline offset
.even_line:
    ; Draw GREEN marker (0x55 pattern in CGA = green-ish)
    mov byte [es:di], 0x55
    mov byte [es:di+1], 0x55
    mov byte [es:di+2], 0x55
    pop es

    ; Also draw at row 2 on screen to show win_get_content succeeded
    push es
    mov ax, 0xB800
    mov es, ax
    mov di, 80                      ; Y=2 (row 1)
    mov byte [es:di], 0xFF
    mov byte [es:di+1], 0xFF
    pop es

    ; Main loop - just wait for ESC
.main_loop:
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event
    cmp al, EVENT_KEY_PRESS
    jne .no_event
    cmp dl, 27                      ; ESC key?
    je .exit_ok

.no_event:
    mov cx, 0x8000
.delay:
    loop .delay
    jmp .main_loop

.win_create_failed:
    ; Draw marker at Y=10 to show win_create failed
    push es
    mov ax, 0xB800
    mov es, ax
    mov di, 5*80                    ; Y=10
    mov byte [es:di], 0xAA          ; Pink
    mov byte [es:di+1], 0xAA
    pop es
    jmp .exit_fail

.get_content_failed:
    ; Draw marker at Y=14 to show get_content failed
    push es
    mov ax, 0xB800
    mov es, ax
    mov di, 7*80                    ; Y=14
    mov byte [es:di], 0xAA          ; Pink
    mov byte [es:di+1], 0xAA
    pop es
    jmp .exit_fail

.exit_ok:
    xor ax, ax
    jmp .exit

.exit_fail:
    mov ax, 1

.exit:
    pop es
    pop ds
    popa
    retf

; Data Section
window_title:   db 'Clock', 0
win_handle:     dw 0
content_x:      dw 0
content_y:      dw 0
content_w:      dw 0
content_h:      dw 0
