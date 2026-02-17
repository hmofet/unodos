; MOUSE_TEST.BIN - Mouse test application for UnoDOS
; PS/2 mouse demonstration with button state display
;
; Build: nasm -f bin -o mouse_test.bin mouse_test.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Mouse', 0                   ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; Mouse cursor: white arrow with cyan fill
    db 0xC0, 0x00, 0x00, 0x00      ; Row 0:  W...............
    db 0xF0, 0x00, 0x00, 0x00      ; Row 1:  WW..............
    db 0xD4, 0x00, 0x00, 0x00      ; Row 2:  WcW.............
    db 0xD5, 0x00, 0x00, 0x00      ; Row 3:  WccW............
    db 0xD5, 0x40, 0x00, 0x00      ; Row 4:  WcccW...........
    db 0xD5, 0x50, 0x00, 0x00      ; Row 5:  WccccW..........
    db 0xD5, 0x54, 0x00, 0x00      ; Row 6:  WcccccW.........
    db 0xD5, 0x55, 0x00, 0x00      ; Row 7:  WccccccW........
    db 0xD5, 0x40, 0x00, 0x00      ; Row 8:  WcccW...........
    db 0xF1, 0x40, 0x00, 0x00      ; Row 9:  WWcWcW..........
    db 0xC0, 0x50, 0x00, 0x00      ; Row 10: W...ccW.........
    db 0x00, 0x50, 0x00, 0x00      ; Row 11: ....ccW.........
    db 0x00, 0x14, 0x00, 0x00      ; Row 12: .....cW.........
    db 0x00, 0x14, 0x00, 0x00      ; Row 13: .....cW.........
    db 0x00, 0x00, 0x00, 0x00      ; Row 14
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API function indices (must match kernel_api_table in kernel.asm)
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_MOUSE_GET_STATE     equ 28
API_MOUSE_IS_ENABLED    equ 30
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32

; Multitasking
API_APP_YIELD           equ 34
API_GFX_SET_FONT        equ 48

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Entry point
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Set medium font (8x8) explicitly
    mov al, 1
    mov ah, API_GFX_SET_FONT
    int 0x80

    ; Create window
    mov bx, 50                      ; X position
    mov cx, 50                      ; Y position
    mov dx, 220                     ; Width
    mov si, 80                      ; Height
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03                    ; WIN_FLAG_TITLE | WIN_FLAG_BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al

    ; Set window drawing context - all draw calls are now relative to
    ; this window's content area (0,0 = top-left of content)
    mov al, [cs:win_handle]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

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

    ; Draw static labels
    call draw_labels

    ; Initialize tracking state
    mov word [cs:last_x], 0xFFFF
    mov byte [cs:last_btn], 0xFF

    ; Main loop
.main_loop:
    sti
    mov ah, API_APP_YIELD           ; Yield to other tasks
    int 0x80

    ; Get mouse state: BX=X, CX=Y, DL=buttons
    mov ah, API_MOUSE_GET_STATE
    int 0x80

    mov [cs:cur_x], bx
    mov [cs:cur_y], cx
    mov [cs:cur_btn], dl

    ; Check if position changed
    mov ax, [cs:last_x]
    cmp ax, bx
    jne .update_pos
    mov ax, [cs:last_y]
    cmp ax, cx
    jne .update_pos
    jmp .check_buttons

.update_pos:
    ; Update position display
    mov ax, [cs:cur_x]
    mov [cs:last_x], ax
    mov ax, [cs:cur_y]
    mov [cs:last_y], ax

    ; Clear position value area (window-relative)
    mov bx, 36
    mov cx, 4
    mov dx, 100
    mov si, 10
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Build position string
    mov di, str_buffer
    mov ax, [cs:cur_x]
    call word_to_decimal
    mov byte [cs:di], ','
    inc di
    mov ax, [cs:cur_y]
    call word_to_decimal
    mov byte [cs:di], 0

    ; Draw position (window-relative)
    mov bx, 36
    mov cx, 4
    mov si, str_buffer
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.check_buttons:
    ; Check if button state changed
    mov al, [cs:cur_btn]
    cmp al, [cs:last_btn]
    je .check_key
    mov [cs:last_btn], al

    ; Clear button area (window-relative)
    mov bx, 4
    mov cx, 32
    mov dx, 200
    mov si, 10
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Build button string: "L:ON  R:--  M:--"
    mov di, str_buffer
    mov al, [cs:cur_btn]

    ; Left button (bit 0)
    mov byte [cs:di], 'L'
    inc di
    mov byte [cs:di], ':'
    inc di
    test al, 0x01
    jz .l_off
    mov byte [cs:di], 'O'
    inc di
    mov byte [cs:di], 'N'
    inc di
    jmp .l_done
.l_off:
    mov byte [cs:di], '-'
    inc di
    mov byte [cs:di], '-'
    inc di
.l_done:
    mov byte [cs:di], ' '
    inc di
    mov byte [cs:di], ' '
    inc di

    ; Right button (bit 1)
    mov al, [cs:cur_btn]
    mov byte [cs:di], 'R'
    inc di
    mov byte [cs:di], ':'
    inc di
    test al, 0x02
    jz .r_off
    mov byte [cs:di], 'O'
    inc di
    mov byte [cs:di], 'N'
    inc di
    jmp .r_done
.r_off:
    mov byte [cs:di], '-'
    inc di
    mov byte [cs:di], '-'
    inc di
.r_done:
    mov byte [cs:di], ' '
    inc di
    mov byte [cs:di], ' '
    inc di

    ; Middle button (bit 2)
    mov al, [cs:cur_btn]
    mov byte [cs:di], 'M'
    inc di
    mov byte [cs:di], ':'
    inc di
    test al, 0x04
    jz .m_off
    mov byte [cs:di], 'O'
    inc di
    mov byte [cs:di], 'N'
    inc di
    jmp .m_done
.m_off:
    mov byte [cs:di], '-'
    inc di
    mov byte [cs:di], '-'
    inc di
.m_done:
    mov byte [cs:di], 0

    ; Draw button state (window-relative)
    mov bx, 4
    mov cx, 32
    mov si, str_buffer
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.check_key:
    mov ah, API_EVENT_GET
    int 0x80
    jc .delay
    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    ; Redraw static labels and force value refresh
    call draw_labels
    mov word [cs:last_x], 0xFFFF
    mov byte [cs:last_btn], 0xFF
    jmp .main_loop
.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .delay
    cmp dl, 27                      ; ESC?
    je .exit_ok

.delay:
    jmp .main_loop

.no_mouse:
    mov bx, 4                       ; Window-relative
    mov cx, 20
    mov si, no_mouse_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.wait_esc:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    mov ah, API_EVENT_GET
    int 0x80
    jc .wait_esc
    cmp al, EVENT_KEY_PRESS
    jne .wait_esc
    cmp dl, 27
    je .exit_ok
    jmp .wait_esc

.exit_ok:
    ; Clear draw context before destroying window
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

; ============================================================================
; draw_labels - Draw static labels (window-relative coordinates)
; ============================================================================
draw_labels:
    pusha

    mov bx, 4                       ; X=4 within content area
    mov cx, 4                       ; Y=4 within content area
    mov si, label_pos
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 4
    mov cx, 20
    mov si, label_buttons
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 4
    mov cx, 48
    mov si, help_msg
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

window_title:   db 'Mouse Test', 0
win_handle:     db 0

cur_x:          dw 0
cur_y:          dw 0
cur_btn:        db 0
last_x:         dw 0xFFFF
last_y:         dw 0xFFFF
last_btn:       db 0xFF

str_buffer:     times 24 db 0

label_pos:      db 'Pos: ', 0
label_buttons:  db 'Buttons:', 0
help_msg:       db 'ESC to exit', 0
no_mouse_msg:   db 'No mouse detected', 0
