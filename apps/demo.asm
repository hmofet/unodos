; DEMO.BIN - UI Widget Demo for UnoDOS
; Showcases GUI widgets using small (4x6) font.
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
API_GFX_FILLED_RECT     equ 2
API_GFX_DRAW_RECT       equ 3
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_MOUSE_STATE         equ 28
API_WIN_BEGIN_DRAW      equ 31
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
API_GET_TICK            equ 63
API_DRAW_COMBOBOX       equ 65
API_DELAY_TICKS         equ 73
API_WIN_GET_CONTENT_SIZE equ 97

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Layout constants (designed for 4x6 font)
; Left column: group box with radios/checkbox, text field, combo
; Right column: list + scrollbar
; Bottom: progress bar, separator, buttons

; Group box
GRP_X       equ 4
GRP_Y       equ 2
GRP_W       equ 120
GRP_H       equ 40

; Radios inside group
RAD_X       equ 10
RAD_Y1      equ 14
RAD_Y2      equ 22

; Checkbox inside group
CHK_X       equ 10
CHK_Y       equ 32

; Text field
TF_LBL_X    equ 4
TF_Y        equ 46
TF_X        equ 30
TF_W        equ 94

; Combo box
CB_LBL_X    equ 4
CB_Y        equ 58
CB_X        equ 30
CB_W        equ 94

; List (right column)
LIST_X      equ 134
LIST_Y      equ 2
LIST_W      equ 100
LIST_VISIBLE equ 6
LIST_COUNT  equ 10

; Scrollbar
SB_X        equ 234
SB_Y        equ 2
SB_H        equ 36

; Progress bar
PROG_X      equ 4
PROG_LBL_Y  equ 74
PROG_BAR_Y  equ 82
PROG_W      equ 246

; Separator
SEP_X       equ 4
SEP_Y       equ 94
SEP_W       equ 246

; Buttons
BTN_OK_X    equ 160
BTN_OK_W    equ 40
BTN_CAN_X   equ 206
BTN_CAN_W   equ 46
BTN_Y       equ 100
BTN_H       equ 12

; Focus states
FOCUS_TF    equ 0
FOCUS_LIST  equ 1
FOCUS_COMBO equ 2

; ============================================================================
; Entry
; ============================================================================
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Set small font (4x6)
    mov al, 0
    mov ah, API_SET_FONT
    int 0x80

    ; Create window
    mov bx, 20
    mov cx, 5
    mov dx, 260
    mov si, 130
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit

    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

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
    je .key_esc
    cmp dl, 9                       ; Tab
    je .toggle_focus

    ; Combo open?
    cmp byte [combo_open], 0
    jne .combo_keys

    ; Route to focused widget
    cmp byte [focus], FOCUS_TF
    je .key_textfield
    cmp byte [focus], FOCUS_LIST
    je .key_list
    cmp byte [focus], FOCUS_COMBO
    je .key_combo_focus
    jmp .main_loop

.key_esc:
    cmp byte [combo_open], 0
    jne .close_combo
    jmp .exit

.toggle_focus:
    cmp byte [combo_open], 0
    jne .close_combo_tab
    inc byte [focus]
    cmp byte [focus], 3
    jb .focus_ok
    mov byte [focus], 0
.focus_ok:
    call draw_textfield
    call draw_list
    call draw_combobox
    jmp .main_loop

.close_combo_tab:
    call close_combo_popup
    jmp .toggle_focus

; --- Text field keys ---
.key_textfield:
    call handle_tf_key
    jmp .main_loop

; --- List keys ---
.key_list:
    cmp dh, 0x48                    ; Up
    je .list_up
    cmp dh, 0x50                    ; Down
    je .list_down
    jmp .main_loop

.list_up:
    cmp byte [list_cursor], 0
    je .main_loop
    dec byte [list_cursor]
    mov al, [list_cursor]
    cmp al, [scroll_off]
    jae .list_redraw
    dec byte [scroll_off]
.list_redraw:
    call draw_list
    call draw_scrollbar
    jmp .main_loop

.list_down:
    mov al, [list_cursor]
    cmp al, LIST_COUNT - 1
    jge .main_loop
    inc byte [list_cursor]
    mov al, [list_cursor]
    mov ah, [scroll_off]
    add ah, LIST_VISIBLE - 1
    cmp al, ah
    jle .list_redraw
    inc byte [scroll_off]
    jmp .list_redraw

; --- Combo keys (when dropdown is open) ---
.combo_keys:
    cmp dl, 27
    je .close_combo
    cmp dh, 0x48
    je .combo_up
    cmp dh, 0x50
    je .combo_down
    cmp dl, 13
    je .combo_select
    jmp .main_loop

.key_combo_focus:
    cmp dl, 13
    je .open_combo
    cmp dl, 32
    je .open_combo
    jmp .main_loop

.combo_up:
    cmp byte [combo_sel], 0
    je .main_loop
    dec byte [combo_sel]
    call draw_combo_dropdown
    jmp .main_loop

.combo_down:
    cmp byte [combo_sel], 3
    jge .main_loop
    inc byte [combo_sel]
    call draw_combo_dropdown
    jmp .main_loop

.combo_select:
    call close_combo_popup
    jmp .main_loop

.close_combo:
    call close_combo_popup
    jmp .main_loop

.open_combo:
    mov byte [combo_open], 1
    call draw_combo_dropdown
    jmp .main_loop

; ============================================================================
; Mouse
; ============================================================================
.check_mouse:
    mov ah, API_MOUSE_STATE
    int 0x80
    test dl, 1
    jz .mouse_up
    cmp byte [prev_btn], 0
    jne .main_loop
    mov byte [prev_btn], 1

    ; Combo dropdown open?
    cmp byte [combo_open], 0
    jne .click_combo_dd

    ; Radio A
    mov bx, RAD_X
    mov cx, RAD_Y1
    mov dx, 70
    mov si, 8
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_rad_a

    ; Radio B
    mov bx, RAD_X
    mov cx, RAD_Y2
    mov dx, 70
    mov si, 8
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_rad_b

    ; Checkbox
    mov bx, CHK_X
    mov cx, CHK_Y
    mov dx, 70
    mov si, 8
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_chk

    ; Text field
    mov bx, TF_X
    mov cx, TF_Y
    mov dx, TF_W
    mov si, 8
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_tf

    ; Combo header
    mov bx, CB_X
    mov cx, CB_Y
    mov dx, CB_W
    mov si, 8
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_combo

    ; List area
    call check_list_click

    ; Scrollbar up
    mov bx, SB_X
    mov cx, SB_Y
    mov dx, 8
    mov si, SB_H / 2
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_sb_up

    ; Scrollbar down
    mov bx, SB_X
    mov cx, SB_Y + SB_H / 2
    mov dx, 8
    mov si, SB_H / 2
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_sb_down

    ; OK button
    mov bx, BTN_OK_X
    mov cx, BTN_Y
    mov dx, BTN_OK_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .exit

    ; Cancel button
    mov bx, BTN_CAN_X
    mov cx, BTN_Y
    mov dx, BTN_CAN_W
    mov si, BTN_H
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

.click_tf:
    mov byte [focus], FOCUS_TF
    call draw_textfield
    call draw_list
    call draw_combobox
    jmp .main_loop

.click_combo:
    mov byte [focus], FOCUS_COMBO
    cmp byte [combo_open], 0
    jne .click_combo_close
    mov byte [combo_open], 1
    call draw_textfield
    call draw_list
    call draw_combo_dropdown
    jmp .main_loop
.click_combo_close:
    call close_combo_popup
    call draw_textfield
    call draw_list
    jmp .main_loop

.click_combo_dd:
    ; Hit test dropdown area
    mov bx, CB_X
    mov cx, CB_Y + 8
    mov dx, CB_W
    mov si, 4 * 6 + 2              ; 4 items * 6px + border
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jz .click_combo_dd_outside
    ; Find which item
    xor cx, cx
.cbd_loop:
    cmp cx, 4
    jge .close_combo
    push cx
    mov ax, cx
    mov bx, 6
    mul bx
    add ax, CB_Y + 9
    mov cx, ax
    mov bx, CB_X + 1
    mov dx, CB_W - 2
    mov si, 6
    mov ah, API_HIT_TEST
    int 0x80
    pop cx
    test al, al
    jnz .cbd_hit
    inc cx
    jmp .cbd_loop
.cbd_hit:
    mov [combo_sel], cl
    call draw_combo_dropdown
    mov cx, 4
    mov ah, API_DELAY_TICKS
    int 0x80
    call close_combo_popup
    jmp .main_loop
.click_combo_dd_outside:
    call close_combo_popup
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
    cmp al, LIST_COUNT - LIST_VISIBLE
    jge .main_loop
    inc byte [scroll_off]
    call draw_list
    call draw_scrollbar
    jmp .main_loop

; ============================================================================
; Exit - restore font, let app_exit_stub handle cleanup
; ============================================================================
.exit:
    mov al, 1                       ; Restore default font
    mov ah, API_SET_FONT
    int 0x80
    pop es
    pop ds
    popa
    retf

; ============================================================================
; draw_all
; ============================================================================
draw_all:
    pusha
    call draw_groupbox
    call draw_radios
    call draw_checkbox
    call draw_tf_label
    call draw_textfield
    call draw_cb_label
    call draw_combobox
    call draw_list
    call draw_scrollbar
    call draw_prog_label
    call draw_prog_bar
    call draw_separator
    call draw_buttons
    popa
    ret

; ============================================================================
; Widget draw functions
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
    mov si, rad_a_label
    mov al, 0
    cmp byte [radio_sel], 0
    jne .ra
    mov al, 1
.ra:
    mov ah, API_DRAW_RADIO
    int 0x80
    mov bx, RAD_X
    mov cx, RAD_Y2
    mov si, rad_b_label
    mov al, 0
    cmp byte [radio_sel], 1
    jne .rb
    mov al, 1
.rb:
    mov ah, API_DRAW_RADIO
    int 0x80
    ret

draw_checkbox:
    mov bx, CHK_X
    mov cx, CHK_Y
    mov si, chk_label
    mov al, [chk_state]
    mov ah, API_DRAW_CHECKBOX
    int 0x80
    ret

draw_tf_label:
    mov bx, TF_LBL_X
    mov cx, TF_Y + 1
    mov si, s_name
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ret

draw_textfield:
    mov bx, TF_X
    mov cx, TF_Y
    mov dx, TF_W
    mov si, tf_buffer
    mov di, [tf_cursor]
    mov al, 0
    cmp byte [focus], FOCUS_TF
    jne .tf_nf
    or al, 1
.tf_nf:
    mov ah, API_DRAW_TEXTFIELD
    int 0x80
    ret

draw_cb_label:
    mov bx, CB_LBL_X
    mov cx, CB_Y + 1
    mov si, s_style
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ret

draw_combobox:
    mov bx, CB_X
    mov cx, CB_Y
    mov dx, CB_W
    xor ax, ax
    mov al, [combo_sel]
    shl ax, 1
    mov si, ax
    add si, combo_ptrs
    mov si, [si]
    mov al, 0
    cmp byte [focus], FOCUS_COMBO
    jne .cb_nf
    or al, 1
.cb_nf:
    mov ah, API_DRAW_COMBOBOX
    int 0x80
    ret

draw_list:
    pusha
    xor cx, cx
.dl_loop:
    cmp cx, LIST_VISIBLE
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
    push ax
    ; Y = LIST_Y + cx * 6
    mov bx, cx
    shl bx, 1
    add bx, cx
    shl bx, 1                      ; bx = cx * 6
    add bx, LIST_Y
    mov cx, bx
    pop ax
    mov bx, LIST_X
    mov dx, LIST_W
    ; Selected = cursor match
    push ax
    mov ah, al
    mov al, 0
    cmp ah, [list_cursor]
    jne .dl_nosel
    or al, 1
.dl_nosel:
    ; Focused
    cmp byte [focus], FOCUS_LIST
    jne .dl_draw
    cmp ah, [list_cursor]
    jne .dl_draw
    or al, 2
.dl_draw:
    mov ah, API_DRAW_LISTITEM
    int 0x80
    pop ax
    pop cx
    inc cx
    jmp .dl_loop
.dl_blank:
    ; Clear empty row
    mov bx, cx
    shl bx, 1
    add bx, cx
    shl bx, 1
    add bx, LIST_Y
    mov cx, bx
    mov bx, LIST_X
    mov dx, LIST_W
    mov si, 6
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    pop cx
    inc cx
    jmp .dl_loop
.dl_done:
    popa
    ret

draw_scrollbar:
    mov bx, SB_X
    mov cx, SB_Y
    mov si, SB_H
    xor dx, dx
    mov dl, [scroll_off]
    mov di, LIST_COUNT - LIST_VISIBLE
    xor al, al
    mov ah, API_DRAW_SCROLLBAR
    int 0x80
    ret

draw_prog_label:
    mov bx, PROG_X
    mov cx, PROG_LBL_Y
    mov si, s_progress
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ret

draw_prog_bar:
    mov bx, PROG_X
    mov cx, PROG_BAR_Y
    mov dx, PROG_W
    mov si, [prog_value]
    mov al, 1
    mov ah, API_DRAW_PROGRESS
    int 0x80
    ret

draw_separator:
    mov bx, SEP_X
    mov cx, SEP_Y
    mov dx, SEP_W
    xor al, al
    mov ah, API_DRAW_SEPARATOR
    int 0x80
    ret

draw_buttons:
    mov ax, cs
    mov es, ax
    mov bx, BTN_OK_X
    mov cx, BTN_Y
    mov dx, BTN_OK_W
    mov si, BTN_H
    mov di, s_ok
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    mov ax, cs
    mov es, ax
    mov bx, BTN_CAN_X
    mov cx, BTN_Y
    mov dx, BTN_CAN_W
    mov si, BTN_H
    mov di, s_cancel
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    ret

; ============================================================================
; Text field key handler
; ============================================================================
handle_tf_key:
    cmp dl, 8
    je .tf_bs
    cmp dl, 32
    jb .tf_done
    cmp dl, 126
    ja .tf_done
    cmp byte [tf_len], 20
    jge .tf_done
    movzx bx, byte [tf_len]
    mov [tf_buffer + bx], dl
    inc bl
    mov byte [tf_buffer + bx], 0
    mov [tf_len], bl
    inc word [tf_cursor]
    call draw_textfield
.tf_done:
    ret
.tf_bs:
    cmp byte [tf_len], 0
    je .tf_done
    dec byte [tf_len]
    movzx bx, byte [tf_len]
    mov byte [tf_buffer + bx], 0
    cmp word [tf_cursor], 0
    je .tf_bs_draw
    dec word [tf_cursor]
.tf_bs_draw:
    call draw_textfield
    ret

; ============================================================================
; List click handler
; ============================================================================
check_list_click:
    pusha
    mov bx, LIST_X
    mov cx, LIST_Y
    mov dx, LIST_W
    mov si, LIST_VISIBLE * 6
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jz .lc_done
    xor cx, cx
.lc_loop:
    cmp cx, LIST_VISIBLE
    jge .lc_done
    push cx
    mov ax, cx
    mov bx, 6
    mul bx
    add ax, LIST_Y
    mov cx, ax
    mov bx, LIST_X
    mov dx, LIST_W
    mov si, 6
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
    mov byte [focus], FOCUS_LIST
    call draw_list
    call draw_scrollbar
    call draw_textfield
    call draw_combobox
.lc_done:
    popa
    ret

; ============================================================================
; Combo dropdown
; ============================================================================
draw_combo_dropdown:
    pusha
    ; Clear + border
    mov bx, CB_X
    mov cx, CB_Y + 8
    mov dx, CB_W
    mov si, 4 * 6 + 2
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    mov bx, CB_X
    mov cx, CB_Y + 8
    mov dx, CB_W
    mov si, 4 * 6 + 2
    mov ah, API_GFX_DRAW_RECT
    int 0x80
    ; Draw items
    xor cx, cx
.dcd_loop:
    cmp cx, 4
    jge .dcd_done
    push cx
    mov ax, cx
    mov bx, 6
    mul bx
    add ax, CB_Y + 9
    mov cx, ax
    mov bx, CB_X + 1
    mov dx, CB_W - 2
    pop ax
    push ax
    mov si, ax
    shl si, 1
    add si, combo_ptrs
    mov si, [si]
    cmp al, [combo_sel]
    mov al, 0
    jne .dcd_draw
    mov al, 1
.dcd_draw:
    mov ah, API_DRAW_LISTITEM
    int 0x80
    pop cx
    inc cx
    jmp .dcd_loop
.dcd_done:
    popa
    ret

close_combo_popup:
    mov byte [combo_open], 0
    ; Clear dropdown area
    mov bx, CB_X
    mov cx, CB_Y + 8
    mov dx, CB_W
    mov si, 4 * 6 + 2
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    call draw_combobox
    ; Repaint anything that was under dropdown
    call draw_prog_label
    call draw_prog_bar
    ret

; ============================================================================
; Progress bar animation
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
s_name:         db 'Name:', 0
s_style:        db 'Style:', 0
s_progress:     db 'Progress:', 0
s_ok:           db 'OK', 0
s_cancel:       db 'Cancel', 0

; Combo strings
combo_str_0:    db 'Normal', 0
combo_str_1:    db 'Bold', 0
combo_str_2:    db 'Italic', 0
combo_str_3:    db 'Underline', 0

combo_ptrs:
    dw combo_str_0
    dw combo_str_1
    dw combo_str_2
    dw combo_str_3

; List items
list_str_0:     db 'Documents', 0
list_str_1:     db 'Pictures', 0
list_str_2:     db 'Music', 0
list_str_3:     db 'Programs', 0
list_str_4:     db 'System', 0
list_str_5:     db 'Videos', 0
list_str_6:     db 'Desktop', 0
list_str_7:     db 'Downloads', 0
list_str_8:     db 'Fonts', 0
list_str_9:     db 'Network', 0

list_ptrs:
    dw list_str_0, list_str_1, list_str_2, list_str_3, list_str_4
    dw list_str_5, list_str_6, list_str_7, list_str_8, list_str_9

; State
prev_btn:       db 0
focus:          db 0
radio_sel:      db 0
chk_state:      db 0
list_cursor:    db 0
scroll_off:     db 0
combo_sel:      db 0
combo_open:     db 0
tf_cursor:      dw 0
tf_len:         db 0
tf_buffer:      times 22 db 0
prog_value:     dw 45
prog_dir:       db 0
anim_cnt:       dw 0
last_tick:      dw 0
