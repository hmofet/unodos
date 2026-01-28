; MOUSE_TEST.BIN - Mouse test application for UnoDOS v3.12.0
; Build 053 - PS/2 mouse demonstration
;
; Build: nasm -f bin -o mouse_test.bin mouse_test.asm

[BITS 16]
[ORG 0x0000]

; API function indices
API_GFX_DRAW_PIXEL      equ 0
API_GFX_DRAW_CHAR       equ 3
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 8
API_WIN_CREATE          equ 19
API_WIN_DESTROY         equ 20
API_WIN_GET_CONTENT     equ 24
API_MOUSE_GET_STATE     equ 27
API_MOUSE_IS_ENABLED    equ 29

; Event types
EVENT_KEY_PRESS         equ 1

; Entry point
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create window
    mov bx, 60                      ; X position
    mov cx, 40                      ; Y position
    mov dx, 200                     ; Width
    mov si, 100                     ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al

    ; Get content area
    mov ah, API_WIN_GET_CONTENT
    int 0x80
    jc .exit_fail
    mov [cs:content_x], bx
    mov [cs:content_y], cx
    mov [cs:content_w], dx
    mov [cs:content_h], si

    ; Drain pending events
.drain_events:
    mov ah, API_EVENT_GET
    int 0x80
    test al, al
    jnz .drain_events

    ; Check if mouse is available
    mov ah, API_MOUSE_IS_ENABLED
    int 0x80
    test al, al
    jz .no_mouse

    ; Display instructions
    mov bx, [cs:content_x]
    add bx, 4
    mov cx, [cs:content_y]
    add cx, 4
    mov si, instructions_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Initialize last position
    mov word [cs:last_x], 0xFFFF
    mov word [cs:last_y], 0xFFFF

    ; Main loop
.main_loop:
    sti                             ; Enable interrupts for mouse IRQ

    ; Get mouse state: BX=X, CX=Y, DL=buttons, DH=enabled
    mov ah, API_MOUSE_GET_STATE
    int 0x80

    ; Save state
    mov [cs:cur_x], bx
    mov [cs:cur_y], cx
    mov [cs:cur_buttons], dl

    ; Only redraw if position changed
    mov ax, [cs:last_x]
    cmp ax, bx
    jne .need_redraw
    mov ax, [cs:last_y]
    cmp ax, cx
    je .check_key

.need_redraw:
    ; Erase old cursor (if valid)
    mov ax, [cs:last_x]
    cmp ax, 0xFFFF
    je .draw_new
    mov bx, ax
    mov cx, [cs:last_y]
    call erase_cursor

.draw_new:
    ; Draw new cursor
    mov bx, [cs:cur_x]
    mov cx, [cs:cur_y]

    ; Choose character based on button state
    mov al, [cs:cur_buttons]
    test al, 0x01                   ; Left button?
    jnz .draw_star
    mov al, '+'                     ; Normal cursor
    jmp .draw_cursor
.draw_star:
    mov al, '*'                     ; Button pressed
.draw_cursor:
    mov [cs:cursor_char], al
    call draw_cursor

    ; Update last position
    mov ax, [cs:cur_x]
    mov [cs:last_x], ax
    mov ax, [cs:cur_y]
    mov [cs:last_y], ax

    ; Display coordinates
    call show_coordinates

.check_key:
    ; Check for ESC key
    mov ah, API_EVENT_GET
    int 0x80
    jc .delay
    cmp al, EVENT_KEY_PRESS
    jne .delay
    cmp dl, 27                      ; ESC?
    je .exit_ok

.delay:
    ; Small delay
    mov cx, 0x800
.delay_loop:
    loop .delay_loop
    jmp .main_loop

.no_mouse:
    ; Display "No mouse detected"
    mov bx, [cs:content_x]
    add bx, 4
    mov cx, [cs:content_y]
    add cx, 30
    mov si, no_mouse_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Wait for ESC
.wait_esc:
    sti
    mov ah, API_EVENT_GET
    int 0x80
    jc .wait_delay
    cmp al, EVENT_KEY_PRESS
    jne .wait_delay
    cmp dl, 27
    je .exit_ok
.wait_delay:
    mov cx, 0x1000
.wait_delay_loop:
    loop .wait_delay_loop
    jmp .wait_esc

.exit_ok:
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

; ============================================================================
; draw_cursor - Draw cursor character at BX,CX
; ============================================================================
draw_cursor:
    push ax
    push bx
    push cx
    mov al, [cs:cursor_char]
    mov ah, API_GFX_DRAW_CHAR
    int 0x80
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; erase_cursor - Erase cursor at BX,CX (draw space)
; ============================================================================
erase_cursor:
    push ax
    push bx
    push cx
    mov al, ' '
    mov ah, API_GFX_DRAW_CHAR
    int 0x80
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; show_coordinates - Display X,Y coordinates
; ============================================================================
show_coordinates:
    pusha

    ; Clear coordinate area
    mov bx, [cs:content_x]
    add bx, 4
    mov cx, [cs:content_y]
    add cx, 60
    mov dx, 100
    mov si, 12
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Build coordinate string "X:nnn Y:nnn"
    mov di, coord_buffer

    ; "X:"
    mov byte [cs:di], 'X'
    inc di
    mov byte [cs:di], ':'
    inc di

    ; X value
    mov ax, [cs:cur_x]
    call word_to_decimal

    ; " Y:"
    mov byte [cs:di], ' '
    inc di
    mov byte [cs:di], 'Y'
    inc di
    mov byte [cs:di], ':'
    inc di

    ; Y value
    mov ax, [cs:cur_y]
    call word_to_decimal

    ; Null terminate
    mov byte [cs:di], 0

    ; Draw it
    mov bx, [cs:content_x]
    add bx, 4
    mov cx, [cs:content_y]
    add cx, 60
    mov si, coord_buffer
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; word_to_decimal - Convert AX to decimal string at CS:DI
; ============================================================================
word_to_decimal:
    push ax
    push bx
    push cx
    push dx

    test ax, ax
    jnz .not_zero
    mov byte [cs:di], '0'
    inc di
    jmp .done

.not_zero:
    mov cx, 0
    mov bx, 10

.div_loop:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .div_loop

.store_loop:
    pop ax
    add al, '0'
    mov [cs:di], al
    inc di
    loop .store_loop

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Data Section
; ============================================================================

window_title:       db 'Mouse Test', 0
win_handle:         db 0
content_x:          dw 0
content_y:          dw 0
content_w:          dw 0
content_h:          dw 0

cur_x:              dw 0
cur_y:              dw 0
cur_buttons:        db 0
last_x:             dw 0xFFFF
last_y:             dw 0xFFFF
cursor_char:        db '+'

coord_buffer:       times 20 db 0

instructions_msg:   db 'Move mouse, ESC to exit', 0
no_mouse_msg:       db 'No mouse detected', 0
