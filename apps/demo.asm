; DEMO.BIN - UI Widget Demo for UnoDOS
; Showcases GUI widgets. Uses default font.
; Clean rewrite using current API conventions.

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E
    db 'UI'
    db 'UI Demo', 0
    times (0x04 + 12) - ($ - $$) db 0

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    db 0x55, 0x55, 0x55, 0x55      ; Row 0
    db 0x40, 0x01, 0x40, 0x01      ; Row 1
    db 0x48, 0xA9, 0x42, 0x81      ; Row 2
    db 0x4A, 0xA9, 0x42, 0x81      ; Row 3
    db 0x48, 0xA9, 0x42, 0x81      ; Row 4
    db 0x40, 0x01, 0x40, 0x01      ; Row 5
    db 0x55, 0x55, 0x55, 0x55      ; Row 6
    db 0x40, 0x01, 0x40, 0x01      ; Row 7
    db 0x4A, 0xA9, 0x6A, 0xA1      ; Row 8
    db 0x48, 0x89, 0x6A, 0xA1      ; Row 9
    db 0x4A, 0xA9, 0x40, 0x01      ; Row 10
    db 0x40, 0x01, 0x40, 0x01      ; Row 11
    db 0x40, 0x01, 0x55, 0x55      ; Row 12
    db 0x40, 0x01, 0x6A, 0xA9      ; Row 13
    db 0x40, 0x01, 0x55, 0x55      ; Row 14
    db 0x55, 0x55, 0x55, 0x55      ; Row 15

    times 0x50 - ($ - $$) db 0

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_MOUSE_STATE         equ 28
API_WIN_BEGIN_DRAW      equ 31
API_APP_YIELD           equ 34
API_DRAW_BUTTON         equ 51
API_DRAW_RADIO          equ 52
API_HIT_TEST            equ 53
API_DRAW_CHECKBOX       equ 56
API_DRAW_SCROLLBAR      equ 58
API_DRAW_LISTITEM       equ 59
API_DRAW_PROGRESS       equ 60
API_DRAW_GROUPBOX       equ 61
API_DRAW_SEPARATOR      equ 62
API_GET_TICK            equ 63
API_WIN_GET_CONTENT_SIZE equ 97
API_GET_FONT_INFO       equ 93

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Layout constants (computed at runtime from font metrics)
LIST_COUNT  equ 8

; ============================================================================
; Entry
; ============================================================================
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Get font metrics
    mov ah, API_GET_FONT_INFO
    int 0x80
    ; BH=height, BL=width, CL=advance
    mov [font_h], bh
    mov [font_adv], cl

    ; row_h = font_h + 2
    movzx ax, bh
    add ax, 2
    mov [row_h], ax

    ; btn_h = font_h + 4
    movzx ax, bh
    add ax, 4
    mov [btn_h], ax

    ; grp_h = 3 * row_h + 10 (radios + checkbox + padding)
    mov ax, [row_h]
    mov dx, 3
    mul dx
    add ax, 10
    mov [grp_h], ax

    ; list_item_h = row_h
    ; list_h = LIST_COUNT * row_h (visible list items)
    mov ax, [row_h]
    mov dx, 6                       ; 6 visible items
    mul dx
    mov [list_h], ax

    ; Compute column widths from font advance
    ; Left column: 16 chars wide
    movzx ax, cl
    mov dx, 16
    mul dx
    add ax, 8                       ; Padding
    mov [left_w], ax

    ; Right column: 12 chars wide
    movzx ax, cl
    mov dx, 12
    mul dx
    add ax, 8
    mov [right_w], ax

    ; right_x = left_w + 8
    mov ax, [left_w]
    add ax, 8
    mov [right_x], ax

    ; sb_x = right_x + right_w
    mov ax, [right_x]
    add ax, [right_w]
    mov [sb_x], ax

    ; Total content_w = sb_x + 8 (scrollbar width)
    mov ax, [sb_x]
    add ax, 8
    mov [content_w], ax

    ; Compute Y positions
    ; grp_y = 2
    mov word [grp_y], 2

    ; Compute radio/checkbox Y from grp_y + row_h + 2 (label offset)
    mov ax, [grp_y]
    add ax, [row_h]
    add ax, 2
    mov [rad_y1], ax
    add ax, [row_h]
    mov [rad_y2], ax
    add ax, [row_h]
    mov [chk_y], ax

    ; list_y = grp_y (same top as group box)
    mov ax, [grp_y]
    mov [list_y], ax

    ; Below group box: progress bar, separator, buttons
    ; prog_lbl_y = grp_y + grp_h + 4
    mov ax, [grp_y]
    add ax, [grp_h]
    add ax, 4
    mov [prog_lbl_y], ax

    ; prog_bar_y = prog_lbl_y + row_h
    add ax, [row_h]
    mov [prog_bar_y], ax

    ; sep_y = prog_bar_y + 12
    add ax, 12
    mov [sep_y], ax

    ; btn_y = sep_y + 4
    add ax, 4
    mov [btn_y], ax

    ; Window size = content + borders + titlebar
    ; win_w = content_w + 4
    mov ax, [content_w]
    add ax, 4
    mov [win_w], ax

    ; win_h = btn_y + btn_h + 4 + 14 (titlebar+border)
    mov ax, [btn_y]
    add ax, [btn_h]
    add ax, 18
    mov [win_h], ax

    ; Create window
    mov bx, 10
    mov cx, 5
    mov dx, [win_w]
    mov si, [win_h]
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit

    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Get actual content size
    mov al, 0xFF
    mov ah, API_WIN_GET_CONTENT_SIZE
    int 0x80
    jc .skip_content
    mov [content_w], dx
    mov [content_h], si
.skip_content:

    call draw_all

; ============================================================================
; Main Loop
; ============================================================================
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    call animate_progress

    mov ah, API_EVENT_GET
    int 0x80
    jc .check_mouse

    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call draw_all
    jmp .main_loop

.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    cmp dl, 27
    je .exit
    jmp .main_loop

; --- Mouse ---
.check_mouse:
    mov ah, API_MOUSE_STATE
    int 0x80
    test dl, 1
    jz .mouse_up
    cmp byte [prev_btn], 0
    jne .main_loop
    mov byte [prev_btn], 1

    ; Radio A
    mov bx, 10
    mov cx, [rad_y1]
    mov dx, 70
    mov si, [row_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_rad_a

    ; Radio B
    mov bx, 10
    mov cx, [rad_y2]
    mov dx, 70
    mov si, [row_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_rad_b

    ; Checkbox
    mov bx, 10
    mov cx, [chk_y]
    mov dx, 70
    mov si, [row_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_chk

    ; List items
    call check_list_click

    ; Scrollbar up
    mov bx, [sb_x]
    mov cx, [list_y]
    mov dx, 8
    mov si, [list_h]
    shr si, 1
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_sb_up

    ; Scrollbar down
    mov bx, [sb_x]
    mov cx, [list_y]
    mov ax, [list_h]
    shr ax, 1
    add cx, ax
    mov dx, 8
    mov si, ax
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_sb_down

    ; OK button
    mov bx, [right_x]
    mov cx, [btn_y]
    mov dx, [right_w]
    mov si, [btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .exit
    jmp .main_loop

.mouse_up:
    mov byte [prev_btn], 0
    jmp .main_loop

.click_rad_a:
    mov byte [radio_sel], 0
    call draw_radios
    jmp .main_loop

.click_rad_b:
    mov byte [radio_sel], 1
    call draw_radios
    jmp .main_loop

.click_chk:
    xor byte [chk_state], 1
    call draw_checkbox
    jmp .main_loop

.click_sb_up:
    cmp byte [scroll_off], 0
    je .main_loop
    dec byte [scroll_off]
    call draw_list
    call draw_scrollbar
    jmp .main_loop

.click_sb_down:
    mov al, [scroll_off]
    cmp al, LIST_COUNT - 6          ; LIST_COUNT - visible
    jge .main_loop
    inc byte [scroll_off]
    call draw_list
    call draw_scrollbar
    jmp .main_loop

; ============================================================================
; Exit - let app_exit_stub handle cleanup
; ============================================================================
.exit:
    pop es
    pop ds
    popa
    retf

; ============================================================================
; draw_all
; ============================================================================
draw_all:
    pusha

    ; Clear content
    mov bx, 0
    mov cx, 0
    mov dx, [content_w]
    mov si, [content_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    call draw_groupbox
    call draw_radios
    call draw_checkbox
    call draw_list
    call draw_scrollbar
    call draw_prog_label
    call draw_prog_bar
    call draw_separator
    call draw_ok_btn
    popa
    ret

; ============================================================================
; Widget draw functions
; ============================================================================

draw_groupbox:
    pusha
    mov bx, 4
    mov cx, [grp_y]
    mov dx, [left_w]
    mov si, [grp_h]
    mov ax, cs
    mov es, ax
    mov di, grp_label
    xor al, al
    mov ah, API_DRAW_GROUPBOX
    int 0x80
    popa
    ret

draw_radios:
    pusha
    mov bx, 10
    mov cx, [rad_y1]
    mov si, rad_a_label
    mov al, 0
    cmp byte [radio_sel], 0
    jne .ra
    mov al, 1
.ra:
    mov ah, API_DRAW_RADIO
    int 0x80
    mov bx, 10
    mov cx, [rad_y2]
    mov si, rad_b_label
    mov al, 0
    cmp byte [radio_sel], 1
    jne .rb
    mov al, 1
.rb:
    mov ah, API_DRAW_RADIO
    int 0x80
    popa
    ret

draw_checkbox:
    pusha
    mov bx, 10
    mov cx, [chk_y]
    mov si, chk_label
    mov al, [chk_state]
    mov ah, API_DRAW_CHECKBOX
    int 0x80
    popa
    ret

draw_list:
    pusha
    xor cx, cx                      ; Visible row counter
.dl_loop:
    cmp cx, 6                       ; 6 visible rows
    jge .dl_done
    push cx
    mov al, [scroll_off]
    xor ah, ah
    add ax, cx
    cmp ax, LIST_COUNT
    jge .dl_blank
    ; Get string pointer
    push ax
    mov si, ax
    shl si, 1
    add si, list_ptrs
    mov si, [si]
    pop ax
    ; Y = list_y + cx * row_h
    push ax
    mov ax, cx
    mul word [row_h]
    add ax, [list_y]
    mov cx, ax
    pop ax
    mov bx, [right_x]
    mov dx, [right_w]
    ; Selected?
    push ax
    cmp al, [list_cursor]
    mov al, 0
    jne .dl_draw
    or al, 1
.dl_draw:
    mov ah, API_DRAW_LISTITEM
    int 0x80
    pop ax
    pop cx
    inc cx
    jmp .dl_loop
.dl_blank:
    ; Clear empty row
    mov ax, cx
    mul word [row_h]
    add ax, [list_y]
    mov cx, ax
    mov bx, [right_x]
    mov dx, [right_w]
    mov si, [row_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    pop cx
    inc cx
    jmp .dl_loop
.dl_done:
    popa
    ret

draw_scrollbar:
    pusha
    mov bx, [sb_x]
    mov cx, [list_y]
    mov si, [list_h]
    xor dx, dx
    mov dl, [scroll_off]
    mov di, LIST_COUNT - 6          ; max_range = total - visible
    xor al, al
    mov ah, API_DRAW_SCROLLBAR
    int 0x80
    popa
    ret

draw_prog_label:
    pusha
    mov bx, 4
    mov cx, [prog_lbl_y]
    mov si, s_progress
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    popa
    ret

draw_prog_bar:
    pusha
    mov bx, 4
    mov cx, [prog_bar_y]
    mov ax, [content_w]
    sub ax, 8
    mov dx, ax                      ; Width = content_w - 8
    mov si, [prog_value]
    mov al, 1
    mov ah, API_DRAW_PROGRESS
    int 0x80
    popa
    ret

draw_separator:
    pusha
    mov bx, 4
    mov cx, [sep_y]
    mov ax, [content_w]
    sub ax, 8
    mov dx, ax
    xor al, al
    mov ah, API_DRAW_SEPARATOR
    int 0x80
    popa
    ret

draw_ok_btn:
    pusha
    mov ax, cs
    mov es, ax
    mov bx, [right_x]
    mov cx, [btn_y]
    mov dx, [right_w]
    mov si, [btn_h]
    mov di, s_ok
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    popa
    ret

; ============================================================================
; List click
; ============================================================================
check_list_click:
    pusha
    mov bx, [right_x]
    mov cx, [list_y]
    mov dx, [right_w]
    mov ax, [row_h]
    mov si, 6
    mul si
    mov si, ax                      ; height = 6 * row_h
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jz .lc_done
    xor cx, cx
.lc_loop:
    cmp cx, 6
    jge .lc_done
    push cx
    mov ax, cx
    mul word [row_h]
    add ax, [list_y]
    mov cx, ax
    mov bx, [right_x]
    mov dx, [right_w]
    mov si, [row_h]
    mov ah, API_HIT_TEST
    int 0x80
    pop cx
    test al, al
    jnz .lc_hit
    inc cx
    jmp .lc_loop
.lc_hit:
    mov al, [scroll_off]
    add al, cl
    cmp al, LIST_COUNT
    jge .lc_done
    mov [list_cursor], al
    call draw_list
    call draw_scrollbar
.lc_done:
    popa
    ret

; ============================================================================
; Progress animation
; ============================================================================
animate_progress:
    mov ah, API_GET_TICK
    int 0x80
    cmp ax, [last_tick]
    je .ap_done
    mov [last_tick], ax
    inc word [anim_cnt]
    cmp word [anim_cnt], 18
    jb .ap_done
    mov word [anim_cnt], 0
    mov ax, [prog_value]
    cmp byte [prog_dir], 0
    jne .ap_dec
    inc ax
    cmp ax, 100
    jbe .ap_set
    mov ax, 100
    mov byte [prog_dir], 1
    jmp .ap_set
.ap_dec:
    dec ax
    jnz .ap_set
    xor ax, ax
    mov byte [prog_dir], 0
.ap_set:
    mov [prog_value], ax
    call draw_prog_bar
.ap_done:
    ret

; ============================================================================
; Data
; ============================================================================
window_title:   db 'UI Demo', 0
grp_label:      db 'Options', 0
rad_a_label:    db 'Option A', 0
rad_b_label:    db 'Option B', 0
chk_label:      db 'Enable', 0
s_progress:     db 'Progress:', 0
s_ok:           db 'OK', 0

; List items
list_str_0:     db 'Documents', 0
list_str_1:     db 'Pictures', 0
list_str_2:     db 'Music', 0
list_str_3:     db 'Programs', 0
list_str_4:     db 'System', 0
list_str_5:     db 'Videos', 0
list_str_6:     db 'Desktop', 0
list_str_7:     db 'Downloads', 0

list_ptrs:
    dw list_str_0, list_str_1, list_str_2, list_str_3
    dw list_str_4, list_str_5, list_str_6, list_str_7

; State
prev_btn:       db 0
radio_sel:      db 0
chk_state:      db 0
list_cursor:    db 0
scroll_off:     db 0
prog_value:     dw 45
prog_dir:       db 0
anim_cnt:       dw 0
last_tick:      dw 0

; Font metrics (computed at runtime)
font_h:         db 0
font_adv:       db 0

; Layout (computed at runtime)
row_h:          dw 0
btn_h:          dw 0
grp_h:          dw 0
grp_y:          dw 0
rad_y1:         dw 0
rad_y2:         dw 0
chk_y:          dw 0
left_w:         dw 0
right_w:        dw 0
right_x:        dw 0
sb_x:           dw 0
list_y:         dw 0
list_h:         dw 0
prog_lbl_y:     dw 0
prog_bar_y:     dw 0
sep_y:          dw 0
btn_y:          dw 0
win_w:          dw 0
win_h:          dw 0
content_w:      dw 200
content_h:      dw 150
