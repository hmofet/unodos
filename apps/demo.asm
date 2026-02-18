; DEMO.BIN - UI Demo for UnoDOS
; Showcases all GUI widget types (Build 245)
;
; Build: nasm -f bin -o demo.bin demo.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'UI Demo', 0                 ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
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
API_GFX_DRAW_PIXEL      equ 0
API_GFX_FILLED_RECT     equ 2
API_GFX_DRAW_RECT       equ 3
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_GFX_DRAW_STRING_INV equ 6
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
API_GET_TICK            equ 63
API_DRAW_COMBOBOX       equ 65
API_DRAW_MENUBAR        equ 66

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Layout constants
WIN_X       equ 20
WIN_Y       equ 5
WIN_W       equ 280
WIN_H       equ 185

; Menu bar
MENU_Y      equ 0
MENU_W      equ 273
MENU_COUNT  equ 3
MENU_H      equ 8

; Group box (left column)
GRP_X       equ 4
GRP_Y       equ 10
GRP_W       equ 130
GRP_H       equ 40

; Radio buttons
RAD_X       equ 10
RAD_Y1      equ 22
RAD_Y2      equ 30

; Checkbox
CHK_X       equ 10
CHK_Y       equ 38

; Text field (below group box)
TF_LBL_X    equ 4
TF_Y        equ 54
TF_X        equ 32
TF_W        equ 100

; Combo box (below text field)
CB_LBL_X    equ 4
CB_Y        equ 66
CB_X        equ 32
CB_W        equ 100
CB_ITEMS    equ 4

; List (right column)
LIST_X      equ 142
LIST_Y      equ 10
LIST_W      equ 106
LIST_VISIBLE equ 6
LIST_COUNT  equ 12

; Scrollbar (next to list)
SB_X        equ 248
SB_Y        equ 10
SB_H        equ 36

; Multi-select checkbox (below list)
MSEL_X      equ 142
MSEL_Y      equ 54

; Multiline text box (below combo dropdown area)
ML_LBL_X    equ 4
ML_LBL_Y    equ 78
ML_X        equ 4
ML_Y        equ 84
ML_W        equ 130
ML_H        equ 26
ML_MAX_COL  equ 31
ML_MAX_LINE equ 3

; Progress bar
PROG_X      equ 4
PROG_LBL_Y  equ 112
PROG_BAR_Y  equ 120
PROG_W      equ 265

; Separator
SEP_X       equ 4
SEP_Y       equ 132
SEP_W       equ 265

; Buttons
BTN_OK_X    equ 170
BTN_OK_W    equ 46
BTN_CAN_X   equ 222
BTN_CAN_W   equ 46
BTN_Y       equ 140
BTN_H       equ 14

; Menu dropdown geometry
MFILE_X     equ 5
MFILE_W     equ 36
MFILE_CNT   equ 4
MEDIT_X     equ 27
MEDIT_W     equ 36
MEDIT_CNT   equ 3
MHELP_X     equ 49
MHELP_W     equ 36
MHELP_CNT   equ 1

; Focus states
FOCUS_TF    equ 0
FOCUS_LIST  equ 1
FOCUS_COMBO equ 2
FOCUS_MULTI equ 3

; ============================================================================
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Small font (4x6)
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

; ============================================================================
; Main event loop
; ============================================================================
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    call animate_progress

    mov ah, API_EVENT_GET
    int 0x80
    jc .check_mouse
    cmp al, EVENT_KEY_PRESS
    jne .check_redraw

    ; --- Key handling ---
    cmp dl, 27                      ; ESC
    je .handle_esc

    ; If a popup is open, handle keys for it
    cmp byte [cs:menu_open], 0xFF
    jne .menu_keys
    cmp byte [cs:combo_open], 0
    jne .combo_keys

    cmp dl, 9                       ; Tab
    je .toggle_focus

    ; Route to focused widget
    cmp byte [cs:focus], FOCUS_TF
    je .key_textfield
    cmp byte [cs:focus], FOCUS_LIST
    je .key_list
    cmp byte [cs:focus], FOCUS_COMBO
    je .key_combo_focus
    cmp byte [cs:focus], FOCUS_MULTI
    je .key_multiline
    jmp .main_loop

.handle_esc:
    cmp byte [cs:menu_open], 0xFF
    jne .close_menu
    cmp byte [cs:combo_open], 0
    jne .close_combo
    jmp .exit_ok

.key_textfield:
    call handle_textfield_key
    jmp .main_loop

.key_multiline:
    call handle_multiline_key
    jmp .main_loop

.key_list:
    cmp dh, 0x48                    ; Up arrow
    je .list_up
    cmp dh, 0x50                    ; Down arrow
    je .list_down
    cmp dl, 32                      ; Space - toggle in multiselect
    je .list_toggle
    jmp .main_loop

.key_combo_focus:
    cmp dl, 13
    je .open_combo
    cmp dl, 32
    je .open_combo
    jmp .main_loop

.menu_keys:
    cmp dl, 27
    je .close_menu
    cmp dh, 0x48
    je .menu_up
    cmp dh, 0x50
    je .menu_down
    cmp dl, 13
    je .menu_select
    jmp .main_loop

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

; --- List navigation with scrolling ---
.list_up:
    cmp byte [cs:list_cursor], 0
    je .main_loop
    dec byte [cs:list_cursor]
    cmp byte [cs:multisel], 0
    jne .list_up_scroll
    call list_single_select
.list_up_scroll:
    mov al, [cs:list_cursor]
    cmp al, [cs:scroll_off]
    jge .list_redraw
    dec byte [cs:scroll_off]
.list_redraw:
    call draw_list
    call draw_scrollbar
    jmp .main_loop

.list_down:
    mov al, [cs:list_cursor]
    cmp al, LIST_COUNT - 1
    jge .main_loop
    inc byte [cs:list_cursor]
    cmp byte [cs:multisel], 0
    jne .list_down_scroll
    call list_single_select
.list_down_scroll:
    mov al, [cs:list_cursor]
    mov ah, [cs:scroll_off]
    add ah, LIST_VISIBLE - 1
    cmp al, ah
    jle .list_redraw
    inc byte [cs:scroll_off]
    jmp .list_redraw

.list_toggle:
    cmp byte [cs:multisel], 0
    je .main_loop
    xor bx, bx
    mov bl, [cs:list_cursor]
    mov ax, 1
    mov cx, bx
    jcxz .lt_no_shift
.lt_shift:
    shl ax, 1
    loop .lt_shift
.lt_no_shift:
    xor [cs:list_mask], ax
    call draw_list
    jmp .main_loop

.toggle_focus:
    cmp byte [cs:menu_open], 0xFF
    jne .close_menu_then_tab
    cmp byte [cs:combo_open], 0
    jne .close_combo_then_tab
.do_toggle:
    inc byte [cs:focus]
    cmp byte [cs:focus], 4
    jb .focus_ok
    mov byte [cs:focus], 0
.focus_ok:
    call draw_textfield_widget
    call draw_list
    call draw_combobox
    call draw_multiline
    jmp .main_loop

.close_menu_then_tab:
    call close_menu_popup
    jmp .do_toggle
.close_combo_then_tab:
    call close_combo_popup
    jmp .do_toggle

; --- Menu dropdown navigation ---
.menu_up:
    cmp byte [cs:menu_sel], 0
    je .main_loop
    dec byte [cs:menu_sel]
    call draw_menu_dropdown
    jmp .main_loop

.menu_down:
    mov al, [cs:menu_open]
    call get_menu_count
    dec al
    cmp [cs:menu_sel], al
    jge .main_loop
    inc byte [cs:menu_sel]
    call draw_menu_dropdown
    jmp .main_loop

.menu_select:
    call delay_short                ; Brief visual feedback
    cmp byte [cs:menu_open], 0
    jne .close_menu
    cmp byte [cs:menu_sel], 3
    je .exit_ok
    jmp .close_menu

.close_menu:
    call close_menu_popup
    jmp .main_loop

; --- Combo dropdown navigation ---
.combo_up:
    cmp byte [cs:combo_sel], 0
    je .main_loop
    dec byte [cs:combo_sel]
    call draw_combo_dropdown
    jmp .main_loop

.combo_down:
    cmp byte [cs:combo_sel], CB_ITEMS - 1
    jge .main_loop
    inc byte [cs:combo_sel]
    call draw_combo_dropdown
    jmp .main_loop

.combo_select:
    call delay_short
    call close_combo_popup
    jmp .main_loop

.close_combo:
    call close_combo_popup
    jmp .main_loop

.open_combo:
    mov byte [cs:combo_open], 1
    call draw_combo_dropdown
    jmp .main_loop

; --- Window redraw ---
.check_redraw:
    cmp al, EVENT_WIN_REDRAW
    jne .main_loop
    call draw_all
    jmp .main_loop

; ============================================================================
; Mouse click handling
; ============================================================================
.check_mouse:
    mov ah, API_MOUSE_STATE
    int 0x80
    test dl, 1
    jz .mouse_up
    cmp byte [cs:prev_btn], 0
    jne .main_loop
    mov byte [cs:prev_btn], 1

    ; If menu dropdown open
    cmp byte [cs:menu_open], 0xFF
    jne .click_menu_dropdown

    ; If combo dropdown open
    cmp byte [cs:combo_open], 0
    jne .click_combo_dropdown

    ; Hit test menu bar
    mov bx, 0
    mov cx, MENU_Y
    mov dx, MENU_W
    mov si, MENU_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_menubar

    ; Hit test radios
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

    ; Hit test multiselect checkbox
    mov bx, MSEL_X
    mov cx, MSEL_Y
    mov dx, 90
    mov si, 8
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_multisel

    ; Hit test text field
    mov bx, TF_X
    mov cx, TF_Y
    mov dx, TF_W
    mov si, 10
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_textfield

    ; Hit test combo header
    mov bx, CB_X
    mov cx, CB_Y
    mov dx, CB_W
    mov si, 10
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_combobox

    ; Hit test multiline text area
    mov bx, ML_X
    mov cx, ML_Y
    mov dx, ML_W
    mov si, ML_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_multiline

    ; Hit test scrollbar (up arrow = top half, down arrow = bottom half)
    mov bx, SB_X
    mov cx, SB_Y
    mov dx, 8
    mov si, SB_H / 2
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_sb_up

    mov bx, SB_X
    mov cx, SB_Y + SB_H / 2
    mov dx, 8
    mov si, SB_H / 2
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .click_sb_down

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

; --- Scrollbar clicks ---
.click_sb_up:
    cmp byte [cs:scroll_off], 0
    je .main_loop
    dec byte [cs:scroll_off]
    cmp byte [cs:multisel], 0
    jne .sb_redraw
    cmp byte [cs:list_cursor], 0
    je .sb_redraw
    dec byte [cs:list_cursor]
    call list_single_select
.sb_redraw:
    call draw_list
    call draw_scrollbar
    jmp .main_loop

.click_sb_down:
    mov al, [cs:scroll_off]
    cmp al, LIST_COUNT - LIST_VISIBLE
    jge .main_loop
    inc byte [cs:scroll_off]
    cmp byte [cs:multisel], 0
    jne .sb_redraw
    mov al, [cs:list_cursor]
    cmp al, LIST_COUNT - 1
    jge .sb_redraw
    inc byte [cs:list_cursor]
    call list_single_select
    jmp .sb_redraw

; --- Menu bar click ---
.click_menubar:
    ; Hit zones match kernel MENUBAR_PAD=6, 4x6 font
    mov bx, 0
    mov cx, MENU_Y
    mov dx, 26
    mov si, MENU_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .open_file_menu

    mov bx, 26
    mov cx, MENU_Y
    mov dx, 24
    mov si, MENU_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .open_edit_menu

    mov bx, 50
    mov cx, MENU_Y
    mov dx, 24
    mov si, MENU_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .open_help_menu
    jmp .main_loop

.open_file_menu:
    mov byte [cs:menu_open], 0
    mov byte [cs:menu_sel], 0
    call draw_menubar
    call draw_menu_dropdown
    jmp .main_loop

.open_edit_menu:
    mov byte [cs:menu_open], 1
    mov byte [cs:menu_sel], 0
    call draw_menubar
    call draw_menu_dropdown
    jmp .main_loop

.open_help_menu:
    mov byte [cs:menu_open], 2
    mov byte [cs:menu_sel], 0
    call draw_menubar
    call draw_menu_dropdown
    jmp .main_loop

; --- Menu dropdown click ---
.click_menu_dropdown:
    mov al, [cs:menu_open]
    call get_menu_geometry
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jz .click_menu_outside
    ; Find which item was clicked
    mov al, [cs:menu_open]
    call get_menu_count
    xor di, di
.mdd_loop:
    cmp di, ax
    jge .close_menu_jmp
    push ax
    push di
    mov al, [cs:menu_open]
    call get_menu_geometry
    inc bx
    sub dx, 2
    ; CX = MENU_H + 1 + di * 6
    mov ax, di
    mov cx, ax
    shl cx, 1
    add cx, ax
    shl cx, 1
    add cx, MENU_H + 1
    mov si, 6
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    pop di
    pop ax
    jnz .mdd_hit
    inc di
    jmp .mdd_loop
.mdd_hit:
    mov ax, di
    mov [cs:menu_sel], al
    call draw_menu_dropdown         ; Show selected item highlighted
    call delay_short                ; Brief visual feedback
    cmp byte [cs:menu_open], 0
    jne .close_menu_jmp
    cmp byte [cs:menu_sel], 3
    je .exit_ok
.close_menu_jmp:
    jmp .close_menu

.click_menu_outside:
    mov bx, 0
    mov cx, MENU_Y
    mov dx, MENU_W
    mov si, MENU_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .switch_menu
    jmp .close_menu

.switch_menu:
    call close_menu_popup
    jmp .click_menubar

; --- Combo click ---
.click_combobox:
    mov byte [cs:focus], FOCUS_COMBO
    cmp byte [cs:combo_open], 0
    jne .close_combo_click
    mov byte [cs:combo_open], 1
    call draw_textfield_widget
    call draw_list
    call draw_multiline
    call draw_combo_dropdown
    jmp .main_loop
.close_combo_click:
    call close_combo_popup
    call draw_textfield_widget
    call draw_list
    call draw_multiline
    jmp .main_loop

.click_combo_dropdown:
    mov bx, CB_X
    mov cx, CB_Y + 10
    mov dx, CB_W
    mov si, CB_ITEMS * 6 + 2
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jz .close_combo_outside
    ; Find item
    xor cx, cx
.cbd_loop:
    cmp cx, CB_ITEMS
    jge .close_combo_jmp
    push cx
    mov ax, cx
    mov bx, 6
    mul bx
    add ax, CB_Y + 11
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
    mov [cs:combo_sel], cl
    call draw_combo_dropdown        ; Show selected item highlighted
    call delay_short                ; Brief visual feedback
    call close_combo_popup
    jmp .main_loop
.close_combo_outside:
    mov bx, CB_X
    mov cx, CB_Y
    mov dx, CB_W
    mov si, 10
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .close_combo_jmp
.close_combo_jmp:
    jmp .close_combo

; --- Widget clicks ---
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

.click_multisel:
    xor byte [cs:multisel], 1
    cmp byte [cs:multisel], 0
    jne .ms_draw
    call list_single_select
.ms_draw:
    call draw_multisel_chk
    call draw_list
    jmp .main_loop

.click_textfield:
    mov byte [cs:focus], FOCUS_TF
    call draw_textfield_widget
    call draw_list
    call draw_combobox
    call draw_multiline
    jmp .main_loop

.click_multiline:
    mov byte [cs:focus], FOCUS_MULTI
    call draw_textfield_widget
    call draw_list
    call draw_combobox
    call draw_multiline
    jmp .main_loop

; ============================================================================
; Exit
; ============================================================================
.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80

.exit_fail:
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
    call draw_menubar
    call draw_groupbox
    call draw_radios
    call draw_checkbox
    call draw_tf_label
    call draw_textfield_widget
    call draw_cb_label
    call draw_combobox
    call draw_list
    call draw_scrollbar
    call draw_multisel_chk
    call draw_ml_label
    call draw_multiline
    call draw_prog_label
    call draw_prog_bar
    call draw_separator
    call draw_buttons
    ret

; ============================================================================
; Individual draw functions
; ============================================================================

draw_menubar:
    mov bx, 0
    mov cx, MENU_Y
    mov dx, MENU_W
    mov si, menu_strings
    mov di, MENU_COUNT
    mov al, [cs:menu_open]
    mov ah, API_DRAW_MENUBAR
    int 0x80
    ret

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
    jne .ra
    mov al, 1
.ra:
    mov ah, API_DRAW_RADIO
    int 0x80
    mov bx, RAD_X
    mov cx, RAD_Y2
    mov si, radio_b_label
    mov al, 0
    cmp byte [cs:radio_sel], 1
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
    mov al, [cs:chk_state]
    mov ah, API_DRAW_CHECKBOX
    int 0x80
    ret

draw_multisel_chk:
    mov bx, MSEL_X
    mov cx, MSEL_Y
    mov si, msel_label
    mov al, [cs:multisel]
    mov ah, API_DRAW_CHECKBOX
    int 0x80
    ret

draw_tf_label:
    mov bx, TF_LBL_X
    mov cx, TF_Y + 2
    mov si, tf_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ret

draw_textfield_widget:
    mov bx, TF_X
    mov cx, TF_Y
    mov dx, TF_W
    mov si, tf_buffer
    mov di, [cs:tf_cursor]
    mov al, 0
    cmp byte [cs:focus], FOCUS_TF
    jne .tf_nf
    or al, 1
.tf_nf:
    mov ah, API_DRAW_TEXTFIELD
    int 0x80
    ret

draw_cb_label:
    mov bx, CB_LBL_X
    mov cx, CB_Y + 2
    mov si, cb_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ret

draw_combobox:
    mov bx, CB_X
    mov cx, CB_Y
    mov dx, CB_W
    xor ax, ax
    mov al, [cs:combo_sel]
    shl ax, 1
    mov si, ax
    add si, combo_ptrs
    mov si, [cs:si]
    mov al, 0
    cmp byte [cs:focus], FOCUS_COMBO
    jne .cb_nf
    or al, 1
.cb_nf:
    mov ah, API_DRAW_COMBOBOX
    int 0x80
    ret

draw_list:
    xor cx, cx
.dl_loop:
    cmp cx, LIST_VISIBLE
    jge .dl_done
    push cx
    mov al, [cs:scroll_off]
    xor ah, ah
    add ax, cx
    cmp ax, LIST_COUNT
    jge .dl_blank
    push ax
    mov si, ax
    shl si, 1
    add si, list_ptrs
    mov si, [cs:si]
    pop ax
    push ax
    mov ax, cx
    mov bx, 6
    mul bx
    add ax, LIST_Y
    mov cx, ax
    pop ax
    mov bx, LIST_X
    mov dx, LIST_W
    push ax
    call item_is_selected
    mov al, 0
    jnc .dl_no_sel
    or al, 1
.dl_no_sel:
    pop dx
    cmp byte [cs:focus], FOCUS_LIST
    jne .dl_draw
    cmp dl, [cs:list_cursor]
    jne .dl_draw
    or al, 2
.dl_draw:
    mov dx, LIST_W
    mov ah, API_DRAW_LISTITEM
    int 0x80
    pop cx
    inc cx
    jmp .dl_loop
.dl_blank:
    mov ax, cx
    mov bx, 6
    mul bx
    add ax, LIST_Y
    mov cx, ax
    mov bx, LIST_X
    mov dx, LIST_W
    mov si, 6
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    pop cx
    inc cx
    jmp .dl_loop
.dl_done:
    ret

item_is_selected:
    push bx
    push cx
    cmp byte [cs:multisel], 0
    jne .iis_multi
    pop cx
    pop bx
    cmp al, [cs:list_cursor]
    je .iis_yes
    clc
    ret
.iis_yes:
    stc
    ret
.iis_multi:
    xor bx, bx
    mov bl, al
    mov ax, 1
    mov cx, bx
    jcxz .iis_bit0
.iis_shift:
    shl ax, 1
    loop .iis_shift
.iis_bit0:
    test [cs:list_mask], ax
    pop cx
    pop bx
    jz .iis_no
    stc
    ret
.iis_no:
    clc
    ret

list_single_select:
    push ax
    push cx
    xor bx, bx
    mov bl, [cs:list_cursor]
    mov ax, 1
    mov cx, bx
    jcxz .lss_done
.lss_shift:
    shl ax, 1
    loop .lss_shift
.lss_done:
    mov [cs:list_mask], ax
    pop cx
    pop ax
    ret

draw_scrollbar:
    mov bx, SB_X
    mov cx, SB_Y
    mov si, SB_H
    xor dx, dx
    mov dl, [cs:scroll_off]
    mov di, LIST_COUNT - LIST_VISIBLE
    xor al, al
    mov ah, API_DRAW_SCROLLBAR
    int 0x80
    ret

draw_ml_label:
    mov bx, ML_LBL_X
    mov cx, ML_LBL_Y
    mov si, ml_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ret

draw_multiline:
    ; Draw border rect
    mov bx, ML_X
    mov cx, ML_Y
    mov dx, ML_W
    mov si, ML_H
    mov ah, API_GFX_DRAW_RECT
    int 0x80
    ; Clear interior
    mov bx, ML_X + 1
    mov cx, ML_Y + 1
    mov dx, ML_W - 2
    mov si, ML_H - 2
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    ; Draw each line of text
    xor cx, cx
.dml_loop:
    cmp cx, 4
    jge .dml_cursor
    push cx
    ; SI = ml_ptrs[cx]
    mov si, cx
    shl si, 1
    add si, ml_ptrs
    mov si, [cs:si]
    ; Y = ML_Y + 1 + cx * 6
    mov ax, cx
    shl ax, 1
    add ax, cx
    shl ax, 1
    add ax, ML_Y + 1
    mov cx, ax
    mov bx, ML_X + 2
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    pop cx
    inc cx
    jmp .dml_loop
.dml_cursor:
    ; Draw cursor if focused
    cmp byte [cs:focus], FOCUS_MULTI
    jne .dml_done
    ; X = ML_X + 2 + cur_col * 4
    xor ax, ax
    mov al, [cs:ml_cur_col]
    shl ax, 2                      ; * 4 (font advance)
    add ax, ML_X + 2
    mov bx, ax
    ; Y = ML_Y + 1 + cur_line * 6
    xor ax, ax
    mov al, [cs:ml_cur_line]
    mov cx, ax
    shl cx, 1
    add cx, ax
    shl cx, 1
    add cx, ML_Y + 1
    mov dx, 1
    mov si, 6
    mov ah, API_GFX_FILLED_RECT
    int 0x80
.dml_done:
    ret

draw_prog_label:
    mov bx, PROG_X
    mov cx, PROG_LBL_Y
    mov si, prog_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ret

draw_prog_bar:
    mov bx, PROG_X
    mov cx, PROG_BAR_Y
    mov dx, PROG_W
    mov si, [cs:prog_value]
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
    cmp dl, 8
    je .tf_bs
    cmp dl, 32
    jb .tf_done
    cmp dl, 126
    ja .tf_done
    mov al, [cs:tf_len]
    cmp al, 20
    jge .tf_done
    xor bx, bx
    mov bl, al
    mov [cs:tf_buffer + bx], dl
    inc bl
    mov byte [cs:tf_buffer + bx], 0
    mov [cs:tf_len], bl
    inc word [cs:tf_cursor]
    call draw_textfield_widget
.tf_done:
    ret
.tf_bs:
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
    call draw_textfield_widget
    ret

; ============================================================================
; Multiline text box key handling
; ============================================================================
handle_multiline_key:
    cmp dl, 8
    je .ml_bs
    cmp dl, 13                      ; Enter = new line
    je .ml_enter
    cmp dl, 32
    jb .ml_done
    cmp dl, 126
    ja .ml_done
    ; Printable char: add to current line if not full
    mov al, [cs:ml_cur_col]
    cmp al, ML_MAX_COL
    jge .ml_done
    ; Get pointer to current line buffer
    xor bx, bx
    mov bl, [cs:ml_cur_line]
    shl bx, 1
    mov si, [cs:ml_ptrs + bx]
    xor cx, cx
    mov cl, [cs:ml_cur_col]
    add si, cx
    mov [cs:si], dl
    mov byte [cs:si + 1], 0
    inc byte [cs:ml_cur_col]
    call draw_multiline
.ml_done:
    ret
.ml_enter:
    mov al, [cs:ml_cur_line]
    cmp al, ML_MAX_LINE
    jge .ml_done
    inc byte [cs:ml_cur_line]
    mov byte [cs:ml_cur_col], 0
    call draw_multiline
    ret
.ml_bs:
    cmp byte [cs:ml_cur_col], 0
    jne .ml_bs_col
    ; At start of line: go to previous line end
    cmp byte [cs:ml_cur_line], 0
    je .ml_done
    dec byte [cs:ml_cur_line]
    ; Find length of previous line
    xor bx, bx
    mov bl, [cs:ml_cur_line]
    shl bx, 1
    mov si, [cs:ml_ptrs + bx]
    xor cx, cx
.ml_bs_findlen:
    cmp byte [cs:si], 0
    je .ml_bs_setlen
    inc si
    inc cx
    jmp .ml_bs_findlen
.ml_bs_setlen:
    mov [cs:ml_cur_col], cl
    call draw_multiline
    ret
.ml_bs_col:
    dec byte [cs:ml_cur_col]
    xor bx, bx
    mov bl, [cs:ml_cur_line]
    shl bx, 1
    mov si, [cs:ml_ptrs + bx]
    xor cx, cx
    mov cl, [cs:ml_cur_col]
    add si, cx
    mov byte [cs:si], 0
    call draw_multiline
    ret

; ============================================================================
; List click handling (with scroll viewport + multiselect)
; ============================================================================
check_list_click:
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
    mov al, [cs:scroll_off]
    add al, cl
    cmp al, LIST_COUNT
    jge .lc_done
    mov [cs:list_cursor], al
    cmp byte [cs:multisel], 0
    je .lc_single
    push ax
    xor bx, bx
    mov bl, al
    mov ax, 1
    mov cx, bx
    jcxz .lc_no_shift
.lc_shift:
    shl ax, 1
    loop .lc_shift
.lc_no_shift:
    xor [cs:list_mask], ax
    pop ax
    jmp .lc_update
.lc_single:
    call list_single_select
.lc_update:
    mov byte [cs:focus], FOCUS_LIST
    call draw_list
    call draw_scrollbar
    call draw_textfield_widget
    call draw_combobox
    call draw_multiline
.lc_done:
    ret

; ============================================================================
; Menu dropdown helpers
; ============================================================================
get_menu_count:
    cmp al, 0
    je .gmc_f
    cmp al, 1
    je .gmc_e
    mov ax, MHELP_CNT
    ret
.gmc_f:
    mov ax, MFILE_CNT
    ret
.gmc_e:
    mov ax, MEDIT_CNT
    ret

get_menu_geometry:
    push ax
    cmp al, 0
    je .gmg_f
    cmp al, 1
    je .gmg_e
    mov bx, MHELP_X
    mov dx, MHELP_W
    mov ax, MHELP_CNT
    jmp .gmg_c
.gmg_f:
    mov bx, MFILE_X
    mov dx, MFILE_W
    mov ax, MFILE_CNT
    jmp .gmg_c
.gmg_e:
    mov bx, MEDIT_X
    mov dx, MEDIT_W
    mov ax, MEDIT_CNT
.gmg_c:
    mov cx, MENU_H
    mov si, 6
    push dx
    mul si
    pop dx
    add ax, 2
    mov si, ax
    pop ax
    ret

get_menu_items:
    cmp al, 0
    je .gmi_f
    cmp al, 1
    je .gmi_e
    mov si, menu_help_items
    ret
.gmi_f:
    mov si, menu_file_items
    ret
.gmi_e:
    mov si, menu_edit_items
    ret

draw_menu_dropdown:
    mov al, [cs:menu_open]
    cmp al, 0xFF
    je .dmd_ret
    call get_menu_geometry
    push bx
    push cx
    push dx
    push si
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    pop si
    pop dx
    pop cx
    pop bx
    push bx
    push cx
    push dx
    push si
    mov ah, API_GFX_DRAW_RECT
    int 0x80
    pop si
    pop dx
    pop cx
    pop bx
    mov al, [cs:menu_open]
    push bx
    push cx
    call get_menu_items
    call get_menu_count
    pop cx
    pop bx
    inc cx
    inc bx
    sub dx, 2
    xor di, di
.dmd_loop:
    cmp di, ax
    jge .dmd_ret
    push ax
    push bx
    push cx
    push dx
    push di
    mov ax, di
    cmp al, [cs:menu_sel]
    mov al, 0
    jne .dmd_draw
    mov al, 1
.dmd_draw:
    mov ah, API_DRAW_LISTITEM
    int 0x80
    push ds
    mov ax, cs
    mov ds, ax
.dmd_skip:
    lodsb
    test al, al
    jnz .dmd_skip
    pop ds
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    add cx, 6
    inc di
    jmp .dmd_loop
.dmd_ret:
    ret

close_menu_popup:
    mov al, [cs:menu_open]
    cmp al, 0xFF
    je .cmp_skip
    call get_menu_geometry
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
.cmp_skip:
    mov byte [cs:menu_open], 0xFF
    mov byte [cs:menu_sel], 0
    call draw_all
    ret

; ============================================================================
; Combo dropdown helpers
; ============================================================================
draw_combo_dropdown:
    mov bx, CB_X
    mov cx, CB_Y + 10
    mov dx, CB_W
    mov si, CB_ITEMS * 6 + 2
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    mov bx, CB_X
    mov cx, CB_Y + 10
    mov dx, CB_W
    mov si, CB_ITEMS * 6 + 2
    mov ah, API_GFX_DRAW_RECT
    int 0x80
    xor cx, cx
.dcd_loop:
    cmp cx, CB_ITEMS
    jge .dcd_done
    push cx
    mov ax, cx
    mov bx, 6
    mul bx
    add ax, CB_Y + 11
    mov cx, ax
    mov bx, CB_X + 1
    mov dx, CB_W - 2
    pop ax
    push ax
    mov si, ax
    shl si, 1
    add si, combo_ptrs
    mov si, [cs:si]
    pop ax
    push ax
    cmp al, [cs:combo_sel]
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
    ret

close_combo_popup:
    mov byte [cs:combo_open], 0
    mov bx, CB_X
    mov cx, CB_Y + 10
    mov dx, CB_W
    mov si, CB_ITEMS * 6 + 2
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    call draw_combobox
    ; Repaint multiline area if overlapped by dropdown
    call draw_ml_label
    call draw_multiline
    ret

; ============================================================================
; Brief delay (~220ms) for UI feedback
; ============================================================================
delay_short:
    mov ah, API_GET_TICK
    int 0x80
    add ax, 4
    push ax
.ds_wait:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    mov ah, API_GET_TICK
    int 0x80
    pop cx
    push cx
    cmp ax, cx
    jb .ds_wait
    pop cx
    ret

; ============================================================================
; Progress bar animation
; ============================================================================
animate_progress:
    mov ah, API_GET_TICK
    int 0x80
    cmp ax, [cs:last_tick]
    je .ap_done
    mov [cs:last_tick], ax
    inc word [cs:anim_counter]
    cmp word [cs:anim_counter], 18
    jb .ap_done
    mov word [cs:anim_counter], 0
    mov ax, [cs:prog_value]
    cmp byte [cs:prog_dir], 0
    jne .ap_dec
    inc ax
    cmp ax, 100
    jbe .ap_set
    mov ax, 100
    mov byte [cs:prog_dir], 1
    jmp .ap_set
.ap_dec:
    dec ax
    jnz .ap_set
    xor ax, ax
    mov byte [cs:prog_dir], 0
.ap_set:
    mov [cs:prog_value], ax
    call draw_prog_bar
.ap_done:
    ret

; ============================================================================
; Data
; ============================================================================

window_title:   db 'UI Demo', 0
grp_label:      db 'Options', 0
radio_a_label:  db 'Option A', 0
radio_b_label:  db 'Option B', 0
chk_label:      db 'Enable', 0
msel_label:     db 'Multi-sel', 0
tf_label:       db 'Name:', 0
cb_label:       db 'Style:', 0
ml_label:       db 'Notes:', 0
prog_label:     db 'Progress:', 0
btn_ok_label:   db 'OK', 0
btn_cancel_label: db 'Cancel', 0

; Menu strings
menu_strings:   db 'File', 0, 'Edit', 0, 'Help', 0
menu_file_items: db 'New', 0, 'Open', 0, 'Save', 0, 'Exit', 0
menu_edit_items: db 'Cut', 0, 'Copy', 0, 'Paste', 0
menu_help_items: db 'About', 0

; Combo options
combo_str_0:    db 'Normal', 0
combo_str_1:    db 'Bold', 0
combo_str_2:    db 'Italic', 0
combo_str_3:    db 'Underline', 0

combo_ptrs:
    dw combo_str_0
    dw combo_str_1
    dw combo_str_2
    dw combo_str_3

; List items (12)
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
list_str_10:    db 'Trash', 0
list_str_11:    db 'Config', 0

list_ptrs:
    dw list_str_0
    dw list_str_1
    dw list_str_2
    dw list_str_3
    dw list_str_4
    dw list_str_5
    dw list_str_6
    dw list_str_7
    dw list_str_8
    dw list_str_9
    dw list_str_10
    dw list_str_11

; Multiline text buffers (4 lines x 32 bytes each)
ml_line0:       times 32 db 0
ml_line1:       times 32 db 0
ml_line2:       times 32 db 0
ml_line3:       times 32 db 0

ml_ptrs:
    dw ml_line0
    dw ml_line1
    dw ml_line2
    dw ml_line3

; State variables
win_handle:     db 0
prev_btn:       db 0
focus:          db 0                ; 0=TF, 1=list, 2=combo, 3=multi
radio_sel:      db 0
chk_state:      db 0
multisel:       db 0
list_cursor:    db 0
list_mask:      dw 0x0001
scroll_off:     db 0
tf_cursor:      dw 0
tf_len:         db 0
tf_buffer:      times 22 db 0
combo_sel:      db 0
combo_open:     db 0
menu_open:      db 0xFF
menu_sel:       db 0
ml_cur_line:    db 0
ml_cur_col:     db 0
prog_value:     dw 45
prog_dir:       db 0
anim_counter:   dw 0
last_tick:      dw 0
