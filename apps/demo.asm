; DEMO.BIN - Widget Toolkit Demo for UnoDOS
; Showcases all GUI widget types (Build 235)
;
; Build: nasm -f bin -o demo.bin demo.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Demo', 0                    ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; Grid/widgets icon: cyan border, magenta detail
    db 0x55, 0x55, 0x55, 0x55      ; Row 0:  cyan top border
    db 0x40, 0x01, 0x40, 0x01      ; Row 1:  cyan frame
    db 0x48, 0xA9, 0x42, 0x81      ; Row 2:  checkbox + text
    db 0x4A, 0xA9, 0x42, 0x81      ; Row 3:  checkbox + text
    db 0x48, 0xA9, 0x42, 0x81      ; Row 4:  checkbox + text
    db 0x40, 0x01, 0x40, 0x01      ; Row 5:  cyan frame
    db 0x55, 0x55, 0x55, 0x55      ; Row 6:  cyan divider
    db 0x40, 0x01, 0x40, 0x01      ; Row 7:  cyan frame
    db 0x4A, 0xA9, 0x6A, 0xA1      ; Row 8:  radio + bar
    db 0x48, 0x89, 0x6A, 0xA1      ; Row 9:  radio + bar
    db 0x4A, 0xA9, 0x40, 0x01      ; Row 10: radio + blank
    db 0x40, 0x01, 0x40, 0x01      ; Row 11: cyan frame
    db 0x40, 0x01, 0x55, 0x55      ; Row 12: blank + button
    db 0x40, 0x01, 0x6A, 0xA9      ; Row 13: blank + button text
    db 0x40, 0x01, 0x55, 0x55      ; Row 14: blank + button
    db 0x55, 0x55, 0x55, 0x55      ; Row 15: cyan bottom border

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_MOUSE_STATE         equ 28
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_SET_FONT            equ 48
API_DRAW_BUTTON         equ 51
API_DRAW_RADIO          equ 52
API_HIT_TEST            equ 53
API_DRAW_CHECKBOX       equ 56
API_DRAW_TEXTFIELD      equ 57
API_DRAW_SCROLLBAR      equ 58
API_DRAW_LISTITEM       equ 59
API_DRAW_PROGRESS       equ 60
API_DRAW_GROUPBOX       equ 61
API_DRAW_SEPARATOR      equ 62

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Layout
WIN_X       equ 20
WIN_Y       equ 5
WIN_W       equ 280
WIN_H       equ 185

; Group box
GRP_X       equ 4
GRP_Y       equ 2
GRP_W       equ 130
GRP_H       equ 50

; Radio buttons (inside group box)
RAD_X       equ 10
RAD_Y1      equ 14
RAD_Y2      equ 26

; Checkbox
CHK_X       equ 10
CHK_Y       equ 38

; Text field
TF_LBL_X    equ 4
TF_Y        equ 58
TF_X        equ 36
TF_W        equ 100

; List items
LIST_X      equ 142
LIST_Y      equ 2
LIST_W      equ 112
LIST_COUNT  equ 5

; Scrollbar
SB_X        equ 256
SB_Y        equ 2
SB_H        equ 36

; Progress bar
PROG_X      equ 142
PROG_Y      equ 46
PROG_W      equ 125

; Separator
SEP_X       equ 4
SEP_Y       equ 76
SEP_W       equ 265

; Buttons
BTN_OK_X    equ 168
BTN_OK_W    equ 50
BTN_CAN_X   equ 224
BTN_CAN_W   equ 50
BTN_Y       equ 82
BTN_H       equ 14

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Use small font (4x6)
    mov al, 0
    mov ah, API_SET_FONT
    int 0x80

    ; Create window
    mov bx, WIN_X
    mov cx, WIN_Y
    mov dx, WIN_W
    mov si, WIN_H
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al

    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    call draw_all

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; Animate progress bar
    call animate_progress

    ; Check events
    mov ah, API_EVENT_GET
    int 0x80
    jc .check_mouse
    cmp al, EVENT_KEY_PRESS
    jne .check_redraw

    ; Key press
    cmp dl, 27                      ; ESC
    je .exit_ok
    cmp dl, 9                       ; Tab - switch focus
    je .toggle_focus
    ; If text field focused, handle typing
    cmp byte [cs:focus], 0
    jne .check_list_keys
    call handle_textfield_key
    jmp .main_loop

.check_list_keys:
    ; List focused - handle arrow keys
    cmp dh, 0x48                    ; Up arrow scancode
    je .list_up
    cmp dh, 0x50                    ; Down arrow scancode
    je .list_down
    jmp .main_loop

.list_up:
    cmp byte [cs:list_sel], 0
    je .main_loop
    dec byte [cs:list_sel]
    call draw_list
    call draw_scrollbar
    jmp .main_loop

.list_down:
    mov al, [cs:list_sel]
    cmp al, LIST_COUNT - 1
    jge .main_loop
    inc byte [cs:list_sel]
    call draw_list
    call draw_scrollbar
    jmp .main_loop

.toggle_focus:
    xor byte [cs:focus], 1
    call draw_textfield
    call draw_list
    jmp .main_loop

.check_redraw:
    cmp al, EVENT_WIN_REDRAW
    jne .main_loop
    call draw_all
    jmp .main_loop

.check_mouse:
    mov ah, API_MOUSE_STATE
    int 0x80
    test dl, 1                      ; Left button
    jz .mouse_up
    cmp byte [cs:prev_btn], 0
    jne .main_loop
    mov byte [cs:prev_btn], 1

    ; Hit test radio buttons
    mov bx, RAD_X
    mov cx, RAD_Y1
    mov dx, 70
    mov si, 10
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_radio_a

    mov bx, RAD_X
    mov cx, RAD_Y2
    mov dx, 70
    mov si, 10
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_radio_b

    ; Hit test checkbox
    mov bx, CHK_X
    mov cx, CHK_Y
    mov dx, 80
    mov si, 8
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_checkbox

    ; Hit test OK button
    mov bx, BTN_OK_X
    mov cx, BTN_Y
    mov dx, BTN_OK_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .exit_ok

    ; Hit test Cancel button
    mov bx, BTN_CAN_X
    mov cx, BTN_Y
    mov dx, BTN_CAN_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .exit_ok

    ; Hit test list items
    call check_list_click
    jmp .main_loop

.mouse_up:
    mov byte [cs:prev_btn], 0
    jmp .main_loop

.click_radio_a:
    mov byte [cs:radio_sel], 0
    call draw_radios
    jmp .main_loop

.click_radio_b:
    mov byte [cs:radio_sel], 1
    call draw_radios
    jmp .main_loop

.click_checkbox:
    xor byte [cs:chk_state], 1
    call draw_checkbox
    jmp .main_loop

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80

.exit_fail:
    ; Restore font to default (8x8)
    mov al, 1
    mov ah, API_SET_FONT
    int 0x80

    pop es
    pop ds
    popa
    retf

; ============================================================================
; Draw all UI elements
; ============================================================================
draw_all:
    call draw_groupbox
    call draw_radios
    call draw_checkbox
    call draw_textfield
    call draw_list
    call draw_scrollbar
    call draw_progress
    call draw_separator
    call draw_buttons
    ret

; ============================================================================
; Individual draw functions
; ============================================================================

draw_groupbox:
    mov bx, GRP_X
    mov cx, GRP_Y
    mov dx, GRP_W
    mov si, GRP_H
    mov ax, cs
    mov es, ax
    mov di, grp_label
    xor al, al
    mov ah, API_DRAW_GROUPBOX
    int 0x80
    ret

draw_radios:
    mov bx, RAD_X
    mov cx, RAD_Y1
    mov si, radio_a_label
    mov al, 0
    cmp byte [cs:radio_sel], 0
    jne .rad_a_draw
    mov al, 1
.rad_a_draw:
    mov ah, API_DRAW_RADIO
    int 0x80

    mov bx, RAD_X
    mov cx, RAD_Y2
    mov si, radio_b_label
    mov al, 0
    cmp byte [cs:radio_sel], 1
    jne .rad_b_draw
    mov al, 1
.rad_b_draw:
    mov ah, API_DRAW_RADIO
    int 0x80
    ret

draw_checkbox:
    mov bx, CHK_X
    mov cx, CHK_Y
    mov si, chk_label
    mov al, [cs:chk_state]
    mov ah, API_DRAW_CHECKBOX
    int 0x80
    ret

draw_textfield:
    ; Label
    mov bx, TF_LBL_X
    mov cx, TF_Y
    add cx, 2                       ; Vertically center label
    mov si, tf_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Text field
    mov bx, TF_X
    mov cx, TF_Y
    mov dx, TF_W
    mov si, tf_buffer
    xor di, di
    mov di, [cs:tf_cursor]
    mov al, 0
    cmp byte [cs:focus], 0
    jne .tf_no_focus
    or al, 1                        ; Focused = show cursor
.tf_no_focus:
    mov ah, API_DRAW_TEXTFIELD
    int 0x80
    ret

draw_list:
    ; Draw each list item
    xor cx, cx                      ; item counter
.list_loop:
    cmp cx, LIST_COUNT
    jge .list_done
    push cx
    ; Calculate Y = LIST_Y + item * font_height (6 for 4x6 font)
    mov ax, cx
    mov bx, 6                       ; font_height for 4x6
    mul bx
    add ax, LIST_Y
    mov cx, ax                      ; CX = Y
    mov bx, LIST_X
    mov dx, LIST_W

    ; Get string pointer for this item
    pop ax                          ; item index
    push ax
    mov si, ax
    shl si, 1                       ; index * 2 for word lookup
    add si, list_ptrs
    mov si, [cs:si]                 ; SI = string pointer

    ; Check if selected
    pop ax
    push ax
    cmp al, [cs:list_sel]
    mov al, 0
    jne .list_draw
    mov al, 1                       ; Selected flag
.list_draw:
    mov ah, API_DRAW_LISTITEM
    int 0x80
    pop cx
    inc cx
    jmp .list_loop
.list_done:
    ret

draw_scrollbar:
    mov bx, SB_X
    mov cx, SB_Y
    mov si, SB_H
    xor dx, dx
    mov dl, [cs:list_sel]           ; Position = selected item
    mov di, LIST_COUNT - 1          ; Max range
    xor al, al                      ; Vertical
    mov ah, API_DRAW_SCROLLBAR
    int 0x80
    ret

draw_progress:
    ; Label
    mov bx, PROG_X
    mov cx, PROG_Y
    mov si, prog_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Progress bar
    mov bx, PROG_X
    mov cx, PROG_Y
    add cx, 8
    mov dx, PROG_W
    xor si, si
    mov si, [cs:prog_value]
    mov al, 1                       ; Show percentage
    mov ah, API_DRAW_PROGRESS
    int 0x80
    ret

draw_separator:
    mov bx, SEP_X
    mov cx, SEP_Y
    mov dx, SEP_W
    xor al, al                      ; Horizontal
    mov ah, API_DRAW_SEPARATOR
    int 0x80
    ret

draw_buttons:
    ; OK button
    mov bx, BTN_OK_X
    mov cx, BTN_Y
    mov dx, BTN_OK_W
    mov si, BTN_H
    mov ax, cs
    mov es, ax
    mov di, btn_ok_label
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    ; Cancel button
    mov bx, BTN_CAN_X
    mov cx, BTN_Y
    mov dx, BTN_CAN_W
    mov si, BTN_H
    mov ax, cs
    mov es, ax
    mov di, btn_cancel_label
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    ret

; ============================================================================
; Text field key handling
; ============================================================================
handle_textfield_key:
    ; DL = ASCII char, DH = scancode
    cmp dl, 8                       ; Backspace
    je .tf_backspace
    cmp dl, 32
    jb .tf_done                     ; Ignore control chars
    cmp dl, 126
    ja .tf_done                     ; Ignore extended chars
    ; Insert character at cursor
    mov al, [cs:tf_len]
    cmp al, 20                      ; Max length
    jge .tf_done
    ; Append char at cursor position
    xor bx, bx
    mov bl, al                      ; BL = current length
    mov [cs:tf_buffer + bx], dl
    inc bl
    mov byte [cs:tf_buffer + bx], 0 ; Null terminate
    mov [cs:tf_len], bl
    inc word [cs:tf_cursor]
    call draw_textfield
.tf_done:
    ret
.tf_backspace:
    cmp byte [cs:tf_len], 0
    je .tf_done
    dec byte [cs:tf_len]
    xor bx, bx
    mov bl, [cs:tf_len]
    mov byte [cs:tf_buffer + bx], 0
    cmp word [cs:tf_cursor], 0
    je .tf_bs_draw
    dec word [cs:tf_cursor]
.tf_bs_draw:
    call draw_textfield
    ret

; ============================================================================
; List click handling
; ============================================================================
check_list_click:
    xor cx, cx
.lc_loop:
    cmp cx, LIST_COUNT
    jge .lc_done
    push cx
    ; Y = LIST_Y + item * 6
    mov ax, cx
    mov bx, 6
    mul bx
    add ax, LIST_Y
    mov cx, ax
    mov bx, LIST_X
    mov dx, LIST_W
    mov si, 6                       ; height = font_height
    mov ah, API_HIT_TEST
    int 0x80
    pop cx
    test al, al
    jnz .lc_hit
    inc cx
    jmp .lc_loop
.lc_hit:
    mov [cs:list_sel], cl
    call draw_list
    call draw_scrollbar
.lc_done:
    ret

; ============================================================================
; Progress bar animation
; ============================================================================
animate_progress:
    ; Simple counter-based animation
    inc word [cs:anim_counter]
    cmp word [cs:anim_counter], 8
    jb .anim_done
    mov word [cs:anim_counter], 0
    ; Advance progress value
    mov ax, [cs:prog_value]
    cmp byte [cs:prog_dir], 0
    jne .anim_dec
    inc ax
    cmp ax, 100
    jbe .anim_set
    mov ax, 100
    mov byte [cs:prog_dir], 1
    jmp .anim_set
.anim_dec:
    dec ax
    jnz .anim_set
    xor ax, ax
    mov byte [cs:prog_dir], 0
.anim_set:
    mov [cs:prog_value], ax
    call draw_progress
.anim_done:
    ret

; ============================================================================
; Data
; ============================================================================

window_title:   db 'Widget Demo', 0
grp_label:      db 'Options', 0
radio_a_label:  db 'Option A', 0
radio_b_label:  db 'Option B', 0
chk_label:      db 'Enable', 0
tf_label:       db 'Name:', 0
prog_label:     db 'Progress:', 0
btn_ok_label:   db 'OK', 0
btn_cancel_label: db 'Cancel', 0

; List item strings
list_str_0:     db 'Documents', 0
list_str_1:     db 'Pictures', 0
list_str_2:     db 'Music', 0
list_str_3:     db 'Programs', 0
list_str_4:     db 'System', 0

list_ptrs:
    dw list_str_0
    dw list_str_1
    dw list_str_2
    dw list_str_3
    dw list_str_4

; State variables
win_handle:     db 0
prev_btn:       db 0
focus:          db 0                ; 0=textfield, 1=list
radio_sel:      db 0                ; 0=A, 1=B
chk_state:      db 0                ; 0=unchecked, 1=checked
list_sel:       db 0                ; Selected list item (0-4)
tf_cursor:      dw 0                ; Text field cursor position
tf_len:         db 0                ; Text field length
tf_buffer:      times 22 db 0       ; Text field buffer (20 chars + null + spare)
prog_value:     dw 45               ; Progress bar value (0-100)
prog_dir:       db 0                ; 0=increasing, 1=decreasing
anim_counter:   dw 0                ; Animation tick counter
