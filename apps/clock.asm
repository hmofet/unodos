; CLOCK.BIN - Clock application for UnoDOS v3.12.0
; Simplified debug version - Build 023
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
    mov [cs:win_handle], ax

    ; SKIP win_get_content - use hardcoded values to test
    ; Window at X=100, Y=60, title bar=10px
    ; Content should be at X=101, Y=71
    mov word [cs:content_x], 105
    mov word [cs:content_y], 75

    ; DEBUG: Draw marker at FIXED position (X=110, Y=75)
    ; This is inside the window content area if window is at X=100, Y=60+10(titlebar)=70
    push es
    mov ax, 0xB800
    mov es, ax
    ; Calculate offset for Y=75: (75/2)*80 = 37*80 = 2960 = 0x0B90
    ; For X=110: 110/4 = 27
    mov di, 0x0B90 + 27             ; Y=75 (odd), need to add 0x2000
    add di, 0x2000                  ; Odd scanline
    mov byte [es:di], 0xFF          ; White marker
    mov byte [es:di+1], 0xFF
    mov byte [es:di+2], 0xFF
    pop es

    ; DEBUG: Also draw marker at content_x, content_y to compare
    push es
    mov ax, 0xB800
    mov es, ax
    mov ax, [cs:content_y]
    shr ax, 1
    mov bx, 80
    mul bx
    mov di, ax
    mov ax, [cs:content_x]
    shr ax, 1
    shr ax, 1
    add di, ax
    mov ax, [cs:content_y]
    test ax, 1
    jz .even
    add di, 0x2000
.even:
    mov byte [es:di], 0xAA          ; Pink marker at content coords
    mov byte [es:di+1], 0xAA
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
    ; Small delay
    mov cx, 0x8000
.delay:
    loop .delay
    jmp .main_loop

.win_create_failed:
    ; Draw red marker at Y=10 to show win_create failed
    push es
    mov ax, 0xB800
    mov es, ax
    mov di, 0x0140                  ; Y=10
    mov byte [es:di], 0x55
    mov byte [es:di+1], 0x55
    pop es
    jmp .exit_fail

.get_content_failed:
    ; Draw blue marker at Y=15 to show get_content failed
    push es
    mov ax, 0xB800
    mov es, ax
    mov di, 0x0258                  ; Y=15 = (15/2)*80 = 600 = 0x258
    add di, 0x2000                  ; Odd line
    mov byte [es:di], 0xAA
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
