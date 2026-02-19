; NOTEPAD.BIN - Text Editor for UnoDOS
; Text editor with selection, clipboard, undo, and context menu.
; Build 267
;
; Build: nasm -f bin -o notepad.bin notepad.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Notepad', 0                 ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    db 0xFF, 0xFC, 0x00, 0x00      ; Row 0:  white top edge
    db 0xC0, 0x0F, 0x00, 0x00      ; Row 1:  white sides + fold
    db 0xC0, 0x03, 0xC0, 0x00      ; Row 2:  fold corner
    db 0xC0, 0x00, 0xF0, 0x00      ; Row 3:  fold
    db 0xC5, 0x55, 0x40, 0x00      ; Row 4:  text line (cyan)
    db 0xC0, 0x00, 0x00, 0x00      ; Row 5:  blank
    db 0xC5, 0x55, 0x50, 0x00      ; Row 6:  text line
    db 0xC0, 0x00, 0x00, 0x00      ; Row 7:  blank
    db 0xC5, 0x54, 0x00, 0x00      ; Row 8:  short text line
    db 0xC0, 0x00, 0x00, 0x00      ; Row 9:  blank
    db 0xC5, 0x55, 0x40, 0x00      ; Row 10: text line
    db 0xC0, 0x00, 0x00, 0x00      ; Row 11: blank
    db 0xC5, 0x50, 0x00, 0x00      ; Row 12: short text
    db 0xC0, 0x00, 0x00, 0x00      ; Row 13: blank
    db 0xFF, 0xFF, 0xF0, 0x00      ; Row 14: bottom edge
    db 0x00, 0x00, 0x00, 0x00      ; Row 15: empty

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_GFX_DRAW_STRING_INV equ 6
API_EVENT_GET           equ 9
API_FS_MOUNT            equ 13
API_FS_OPEN             equ 14
API_FS_READ             equ 15
API_FS_CLOSE            equ 16
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_MOUSE_STATE         equ 28
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_GFX_TEXT_WIDTH      equ 33
API_APP_YIELD           equ 34
API_GET_BOOT_DRIVE      equ 43
API_FS_CREATE           equ 45
API_FS_WRITE            equ 46
API_FS_DELETE           equ 47
API_DRAW_BUTTON         equ 51
API_HIT_TEST            equ 53
API_DRAW_TEXTFIELD      equ 57
API_FILLED_RECT_COLOR   equ 67
API_RECT_COLOR          equ 68
API_DRAW_HLINE          equ 69
API_WIN_GET_INFO        equ 79
API_GET_KEY_MODIFIERS   equ 83

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Modes
MODE_EDIT               equ 0
MODE_OPEN               equ 1
MODE_SAVE               equ 2
MODE_CONTEXT_MENU       equ 3
MODE_FILE_MENU          equ 4

; Layout (content-relative, total window 318x198, content 316x186)
WIN_W                   equ 318
WIN_H                   equ 198
CONTENT_W               equ 316
CONTENT_H               equ 186
MENUBAR_Y               equ 0
MENUBAR_H               equ 10
SEP1_Y                  equ 11
TEXT_X                   equ 2
TEXT_Y                  equ 13
TEXT_W                  equ 312
TEXT_H                  equ 155
SEP2_Y                  equ 170
STATUS_Y                equ 173
TITLEBAR_HEIGHT         equ 10

; Menu bar layout
FILE_LABEL_X            equ 4
FILE_LABEL_W            equ 30
FNAME_X                 equ 80
BTN_H                   equ 10

; Dialog button
BTN_OK_X                equ 260
BTN_OK_W                equ 30

; Buffer sizes
TEXT_MAX                 equ 16384      ; 16KB text buffer
CLIP_MAX                equ 4096       ; 4KB clipboard

; Menu shared constants
MENU_ITEM_H             equ 10

; Context menu
CTX_MENU_W              equ 80
CTX_MENU_ITEMS          equ 5

; File menu
FILE_MENU_X             equ 2
FILE_MENU_Y             equ 11         ; Below menu bar
FILE_MENU_W             equ 72
FILE_MENU_ITEMS         equ 4

; ============================================================================
; Entry Point
; ============================================================================
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Initialize selection/clipboard/undo state
    mov word [cs:sel_anchor], 0xFFFF
    mov word [cs:clip_len], 0
    mov byte [cs:undo_valid], 0
    mov byte [cs:undo_saved_for_edit], 0
    mov byte [cs:mouse_selecting], 0
    mov byte [cs:prev_right_btn], 0
    mov byte [cs:shift_held], 0

    ; Get boot drive and mount filesystem
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov ah, API_FS_MOUNT
    int 0x80
    jc .exit_fail
    mov [cs:mount_handle], bl

    ; Compute font metrics for layout
    call compute_layout

    ; Create window (nearly fullscreen)
    mov bx, 1                          ; X
    mov cx, 1                          ; Y
    mov dx, WIN_W                      ; Width (total)
    mov si, WIN_H                      ; Height (total)
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03                        ; TITLE | BORDER
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al

    ; Set draw context
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Initialize empty buffer
    call do_new_file

    ; Draw initial UI
    call draw_ui

; ============================================================================
; Main Loop
; ============================================================================
.main_loop:
    ; Deferred redraw: process all queued events FIRST, then redraw once
    cmp byte [cs:needs_redraw], 0
    je .no_deferred
    cmp byte [cs:needs_redraw], 1
    je .do_line_redraw
    ; needs_redraw >= 2: full text area redraw
    mov byte [cs:needs_redraw], 0
    call update_after_edit
    jmp .no_deferred
.do_line_redraw:
    mov byte [cs:needs_redraw], 0
    call draw_current_line
.no_deferred:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; --- Mouse ---
    mov ah, API_MOUSE_STATE
    int 0x80
    ; BX=X, CX=Y, DL=buttons
    mov [cs:mouse_abs_x], bx
    mov [cs:mouse_abs_y], cx
    mov [cs:mouse_buttons], dl

    ; --- Mouse drag selection ---
    cmp byte [cs:mouse_selecting], 0
    je .no_drag
    test byte [cs:mouse_buttons], 1     ; Left button still held?
    jnz .do_drag
    ; Released: end drag
    mov byte [cs:mouse_selecting], 0
    jmp .no_drag
.do_drag:
    call mouse_to_offset
    jc .no_drag
    cmp ax, [cs:cursor_pos]
    je .check_event                     ; No movement
    mov [cs:cursor_pos], ax
    call update_selection_bounds
    mov byte [cs:needs_redraw], 2
    jmp .check_event
.no_drag:

    ; --- Left click ---
    test byte [cs:mouse_buttons], 1
    jz .left_up
    cmp byte [cs:prev_btn], 0
    jne .check_right_click
    mov byte [cs:prev_btn], 1

    ; Left click dispatch by mode
    cmp byte [cs:mode], MODE_CONTEXT_MENU
    je .click_menu
    cmp byte [cs:mode], MODE_FILE_MENU
    je .click_menu
    cmp byte [cs:mode], MODE_EDIT
    je .click_edit
    jmp .click_dialog

.left_up:
    mov byte [cs:prev_btn], 0
.check_right_click:
    ; --- Right click ---
    test byte [cs:mouse_buttons], 2
    jz .right_up
    cmp byte [cs:prev_right_btn], 0
    jne .check_event
    mov byte [cs:prev_right_btn], 1

    ; Right-click: open context menu (only in edit mode)
    cmp byte [cs:mode], MODE_EDIT
    jne .check_event
    jmp .open_context_menu

.right_up:
    mov byte [cs:prev_right_btn], 0

.check_event:
    mov ah, API_EVENT_GET
    int 0x80
    jc .main_loop
    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call compute_layout
    call draw_ui
    mov byte [cs:needs_redraw], 0
    jmp .main_loop
.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    ; DL=ASCII/special, DH=scan code
    cmp byte [cs:mode], MODE_CONTEXT_MENU
    je .key_dismiss_menu
    cmp byte [cs:mode], MODE_FILE_MENU
    je .key_dismiss_menu
    cmp byte [cs:mode], MODE_EDIT
    je .key_edit
    jmp .key_dialog

; ============================================================================
; Edit Mode Click Handling
; ============================================================================
.click_edit:
    ; File menu label hit-test
    mov bx, FILE_LABEL_X
    mov cx, MENUBAR_Y
    mov dx, FILE_LABEL_W
    mov si, MENUBAR_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .open_file_menu

    ; Text area click — position cursor
    call mouse_to_offset
    jc .check_event                     ; Outside text area

    ; Check shift for selection extension
    push ax
    mov ah, API_GET_KEY_MODIFIERS
    int 0x80
    mov [cs:shift_held], al
    pop ax

    cmp byte [cs:shift_held], 0
    jne .click_extend_sel

    ; Normal click: set cursor, clear selection, start potential drag
    call clear_selection_silent         ; Clear old selection first
    mov [cs:cursor_pos], ax
    mov [cs:sel_anchor], ax             ; Then set new anchor for drag
    mov byte [cs:mouse_selecting], 1
    mov byte [cs:undo_saved_for_edit], 0
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.click_extend_sel:
    ; Shift+click: extend selection from anchor
    cmp word [cs:sel_anchor], 0xFFFF
    jne .click_ext_have_anchor
    mov bx, [cs:cursor_pos]
    mov [cs:sel_anchor], bx
.click_ext_have_anchor:
    mov [cs:cursor_pos], ax
    call update_selection_bounds
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.start_open:
    mov byte [cs:mode], MODE_OPEN
    mov byte [cs:input_buf], 0
    mov byte [cs:input_len], 0
    call draw_status
    jmp .check_event

.start_save:
    cmp byte [cs:filename_buf], 0
    je .start_save_as
    call do_save_file
    call draw_status
    jmp .check_event

.do_new:
    call do_new_file
    call draw_ui
    jmp .main_loop

; ============================================================================
; Edit Mode Key Handling (restructured for unified INT 9)
; ============================================================================
.key_edit:
    ; Get modifier state first
    push dx
    mov ah, API_GET_KEY_MODIFIERS
    int 0x80
    mov [cs:shift_held], al             ; AL=shift state
    mov [cs:ctrl_held], ah              ; AH=ctrl state
    pop dx

    ; 1. ESC
    cmp dl, 27
    je .exit_ok

    ; 2. Ctrl+shortcuts (DL=1-26 from Ctrl+letter mapping)
    cmp dl, 1                           ; Ctrl+A = Select All
    je .do_select_all
    cmp dl, 3                           ; Ctrl+C = Copy
    je .do_copy_key
    cmp dl, 14                          ; Ctrl+N = New
    je .do_new
    cmp dl, 15                          ; Ctrl+O = Open
    je .start_open
    cmp dl, 19                          ; Ctrl+S = Save
    je .start_save
    cmp dl, 22                          ; Ctrl+V = Paste
    je .do_paste_key
    cmp dl, 24                          ; Ctrl+X = Cut
    je .do_cut_key
    cmp dl, 26                          ; Ctrl+Z = Undo
    je .do_undo_key

    ; 3. Control chars
    cmp dl, 8
    je .do_backspace
    cmp dl, 13
    je .do_enter
    cmp dl, 9
    je .do_tab

    ; 4. Special codes from unified INT 9 (DL=128-136)
    cmp dl, 128
    je .do_cursor_up
    cmp dl, 129
    je .do_cursor_down
    cmp dl, 130
    je .do_cursor_left
    cmp dl, 131
    je .do_cursor_right
    cmp dl, 132
    je .do_home
    cmp dl, 133
    je .do_end
    cmp dl, 134
    je .do_delete
    cmp dl, 135
    je .do_pgup
    cmp dl, 136
    je .do_pgdn

    ; 5. Fallback DH scan codes (BIOS INT 16h path, DL=0)
    test dl, dl
    jnz .check_printable
    cmp dh, 0x48
    je .do_cursor_up
    cmp dh, 0x50
    je .do_cursor_down
    cmp dh, 0x4B
    je .do_cursor_left
    cmp dh, 0x4D
    je .do_cursor_right
    cmp dh, 0x47
    je .do_home
    cmp dh, 0x4F
    je .do_end
    cmp dh, 0x53
    je .do_delete
    cmp dh, 0x49
    je .do_pgup
    cmp dh, 0x51
    je .do_pgdn
    jmp .main_loop

    ; 6. Printable (32-126) — reject if Ctrl held (prevents Ctrl+letter from typing)
.check_printable:
    cmp byte [cs:ctrl_held], 0
    jne .main_loop                      ; Ctrl held → don't insert character
    cmp dl, 32
    jb .main_loop
    cmp dl, 126
    ja .main_loop

    ; If selection active, delete it first (replace selection with typed char)
    cmp word [cs:sel_anchor], 0xFFFF
    je .type_no_sel
    call maybe_save_undo
    call delete_selection
    jmp .type_insert
.type_no_sel:
    call maybe_save_undo
.type_insert:
    call buf_insert_char
    inc word [cs:cursor_col]

    ; Check if typing at end of line (fast path)
    mov bx, [cs:cursor_pos]
    cmp bx, [cs:text_len]
    jae .type_at_eol
    cmp byte [cs:text_buf + bx], 0x0A
    je .type_at_eol

    ; Mid-line insertion: line redraw
    jmp .set_line_redraw

.type_at_eol:
    call draw_typed_char
    jmp .check_event

; --- Edit operations ---
.do_backspace:
    cmp word [cs:sel_anchor], 0xFFFF
    je .bs_no_sel
    call maybe_save_undo
    call delete_selection
    mov byte [cs:needs_redraw], 2
    jmp .check_event
.bs_no_sel:
    cmp word [cs:cursor_pos], 0
    je .main_loop
    call maybe_save_undo
    call buf_delete_char
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_enter:
    call maybe_save_undo
    cmp word [cs:sel_anchor], 0xFFFF
    je .enter_insert
    call delete_selection
.enter_insert:
    mov dl, 0x0A
    call buf_insert_char
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_tab:
    call maybe_save_undo
    cmp word [cs:sel_anchor], 0xFFFF
    je .tab_insert
    call delete_selection
.tab_insert:
    mov dl, ' '
    call buf_insert_char
    call buf_insert_char
    call buf_insert_char
    call buf_insert_char
    jmp .set_line_redraw

.do_delete:
    cmp word [cs:sel_anchor], 0xFFFF
    je .del_no_sel
    call maybe_save_undo
    call delete_selection
    mov byte [cs:needs_redraw], 2
    jmp .check_event
.del_no_sel:
    mov ax, [cs:cursor_pos]
    cmp ax, [cs:text_len]
    jae .main_loop
    call maybe_save_undo
    call buf_delete_fwd
    mov byte [cs:needs_redraw], 2
    jmp .check_event

; --- Ctrl+key operations ---
.do_select_all:
    mov word [cs:sel_anchor], 0
    mov ax, [cs:text_len]
    mov [cs:cursor_pos], ax
    call update_selection_bounds
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_copy_key:
    call do_copy
    jmp .check_event

.do_cut_key:
    call do_cut
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_paste_key:
    call do_paste
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_undo_key:
    call do_undo
    mov byte [cs:undo_saved_for_edit], 0
    mov byte [cs:needs_redraw], 2
    jmp .check_event

; --- Cursor movement (selection-aware) ---
.do_cursor_left:
    cmp byte [cs:shift_held], 0
    jne .cursor_left_shift
    ; No shift: collapse selection or move
    cmp word [cs:sel_anchor], 0xFFFF
    je .cursor_left_move
    mov ax, [cs:sel_start]
    mov [cs:cursor_pos], ax
    call clear_selection_silent
    jmp .cursor_moved
.cursor_left_move:
    cmp word [cs:cursor_pos], 0
    je .main_loop
    dec word [cs:cursor_pos]
    jmp .cursor_moved
.cursor_left_shift:
    call handle_shift_for_move
    cmp word [cs:cursor_pos], 0
    je .cursor_moved_sel
    dec word [cs:cursor_pos]
    jmp .cursor_moved_sel

.do_cursor_right:
    cmp byte [cs:shift_held], 0
    jne .cursor_right_shift
    cmp word [cs:sel_anchor], 0xFFFF
    je .cursor_right_move
    mov ax, [cs:sel_end]
    mov [cs:cursor_pos], ax
    call clear_selection_silent
    jmp .cursor_moved
.cursor_right_move:
    mov ax, [cs:cursor_pos]
    cmp ax, [cs:text_len]
    jae .main_loop
    inc word [cs:cursor_pos]
    jmp .cursor_moved
.cursor_right_shift:
    call handle_shift_for_move
    mov ax, [cs:cursor_pos]
    cmp ax, [cs:text_len]
    jae .cursor_moved_sel
    inc word [cs:cursor_pos]
    jmp .cursor_moved_sel

.do_cursor_up:
    cmp byte [cs:shift_held], 0
    jne .cursor_up_shift
    cmp word [cs:sel_anchor], 0xFFFF
    je .cursor_up_move
    mov ax, [cs:sel_start]
    mov [cs:cursor_pos], ax
    call clear_selection_silent
    jmp .cursor_moved
.cursor_up_move:
    call cursor_up
    jmp .cursor_moved
.cursor_up_shift:
    call handle_shift_for_move
    call cursor_up
    jmp .cursor_moved_sel

.do_cursor_down:
    cmp byte [cs:shift_held], 0
    jne .cursor_down_shift
    cmp word [cs:sel_anchor], 0xFFFF
    je .cursor_down_move
    mov ax, [cs:sel_end]
    mov [cs:cursor_pos], ax
    call clear_selection_silent
    jmp .cursor_moved
.cursor_down_move:
    call cursor_down
    jmp .cursor_moved
.cursor_down_shift:
    call handle_shift_for_move
    call cursor_down
    jmp .cursor_moved_sel

.do_home:
    cmp byte [cs:shift_held], 0
    jne .home_shift
    call clear_selection_silent
    call cursor_home
    jmp .cursor_moved
.home_shift:
    call handle_shift_for_move
    call cursor_home
    jmp .cursor_moved_sel

.do_end:
    cmp byte [cs:shift_held], 0
    jne .end_shift
    call clear_selection_silent
    call cursor_end
    jmp .cursor_moved
.end_shift:
    call handle_shift_for_move
    call cursor_end
    jmp .cursor_moved_sel

.do_pgup:
    cmp byte [cs:shift_held], 0
    jne .pgup_shift
    call clear_selection_silent
    call cursor_pgup
    jmp .cursor_moved
.pgup_shift:
    call handle_shift_for_move
    call cursor_pgup
    jmp .cursor_moved_sel

.do_pgdn:
    cmp byte [cs:shift_held], 0
    jne .pgdn_shift
    call clear_selection_silent
    call cursor_pgdn
    jmp .cursor_moved
.pgdn_shift:
    call handle_shift_for_move
    call cursor_pgdn
    jmp .cursor_moved_sel

; Common exit for cursor movement
.cursor_moved:
    mov byte [cs:undo_saved_for_edit], 0
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.cursor_moved_sel:
    call update_selection_bounds
    mov byte [cs:undo_saved_for_edit], 0
    mov byte [cs:needs_redraw], 2
    jmp .check_event

; Helper: set line-only redraw (don't downgrade from full redraw)
.set_line_redraw:
    cmp byte [cs:needs_redraw], 2
    jae .check_event
    mov byte [cs:needs_redraw], 1
    jmp .check_event

; ============================================================================
; Menu System (shared by File menu and Context menu)
; ============================================================================
.open_context_menu:
    call mouse_to_content_rel
    jc .check_event

    ; Clamp position within content area
    mov ax, [cs:mouse_rel_x]
    mov bx, CONTENT_W
    sub bx, CTX_MENU_W
    cmp ax, bx
    jbe .ctx_x_ok
    mov ax, bx
.ctx_x_ok:
    mov [cs:menu_x], ax

    movzx bx, byte [cs:active_menu_h]
    mov ax, [cs:mouse_rel_y]
    push bx
    mov bx, CONTENT_H
    sub bx, CTX_MENU_ITEMS * MENU_ITEM_H
    cmp ax, bx
    jbe .ctx_y_ok
    mov ax, bx
.ctx_y_ok:
    pop bx
    mov [cs:menu_y], ax

    ; Set active menu state
    mov word [cs:active_menu_strings], ctx_menu_strings
    mov byte [cs:active_menu_count], CTX_MENU_ITEMS
    mov byte [cs:active_menu_w], CTX_MENU_W
    mov byte [cs:mode], MODE_CONTEXT_MENU
    call draw_menu
    jmp .check_event

.open_file_menu:
    mov word [cs:menu_x], FILE_MENU_X
    mov word [cs:menu_y], FILE_MENU_Y
    mov word [cs:active_menu_strings], file_menu_strings
    mov byte [cs:active_menu_count], FILE_MENU_ITEMS
    mov byte [cs:active_menu_w], FILE_MENU_W
    mov byte [cs:mode], MODE_FILE_MENU
    call draw_menu
    jmp .check_event

.click_menu:
    ; Update mouse_rel coords to current click position
    call mouse_to_content_rel

    ; Check if click is within menu bounds (X)
    mov ax, [cs:mouse_rel_x]
    cmp ax, [cs:menu_x]
    jb .dismiss_menu
    mov bx, [cs:menu_x]
    movzx cx, byte [cs:active_menu_w]
    add bx, cx
    cmp ax, bx
    jae .dismiss_menu

    ; Check Y bounds
    mov ax, [cs:mouse_rel_y]
    cmp ax, [cs:menu_y]
    jb .dismiss_menu
    mov bx, [cs:menu_y]
    movzx cx, byte [cs:active_menu_count]
    imul cx, MENU_ITEM_H
    add bx, cx
    cmp ax, bx
    jae .dismiss_menu

    ; Compute item index
    sub ax, [cs:menu_y]
    xor dx, dx
    mov bx, MENU_ITEM_H
    div bx
    cmp al, [cs:active_menu_count]
    jae .dismiss_menu

    ; Dispatch by menu type
    cmp byte [cs:mode], MODE_FILE_MENU
    je .file_menu_dispatch

    ; --- Context menu dispatch ---
    mov byte [cs:mode], MODE_EDIT
    cmp al, 0
    je .menu_cut
    cmp al, 1
    je .menu_copy
    cmp al, 2
    je .menu_paste
    cmp al, 3
    je .menu_undo
    cmp al, 4
    je .menu_sel_all
    jmp .dismiss_menu

.file_menu_dispatch:
    mov byte [cs:mode], MODE_EDIT
    mov byte [cs:needs_redraw], 2
    cmp al, 0
    je .do_new
    cmp al, 1
    je .start_open
    cmp al, 2
    je .start_save
    cmp al, 3
    je .start_save_as
    jmp .dismiss_menu

.menu_cut:
    call do_cut
    mov byte [cs:needs_redraw], 2
    jmp .check_event
.menu_copy:
    call do_copy
    mov byte [cs:needs_redraw], 2
    jmp .check_event
.menu_paste:
    call do_paste
    mov byte [cs:needs_redraw], 2
    jmp .check_event
.menu_undo:
    call do_undo
    mov byte [cs:undo_saved_for_edit], 0
    mov byte [cs:needs_redraw], 2
    jmp .check_event
.menu_sel_all:
    mov word [cs:sel_anchor], 0
    mov ax, [cs:text_len]
    mov [cs:cursor_pos], ax
    call update_selection_bounds
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.start_save_as:
    mov byte [cs:mode], MODE_SAVE
    mov byte [cs:input_buf], 0
    mov byte [cs:input_len], 0
    call draw_status
    jmp .check_event

.dismiss_menu:
    mov byte [cs:mode], MODE_EDIT
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.key_dismiss_menu:
    ; ESC or any key dismisses any open menu
    mov byte [cs:mode], MODE_EDIT
    mov byte [cs:needs_redraw], 2
    jmp .check_event

; ============================================================================
; Dialog Mode (Open/Save filename input)
; ============================================================================
.click_dialog:
    ; [OK] button
    mov bx, BTN_OK_X
    mov cx, STATUS_Y
    mov dx, BTN_OK_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .dialog_submit
    jmp .check_event

.key_dialog:
    ; ESC - cancel dialog
    cmp dl, 27
    je .dialog_cancel
    ; Backspace
    cmp dl, 8
    je .dialog_backspace
    ; Enter - submit
    cmp dl, 13
    je .dialog_submit
    ; Printable char (32-126)
    cmp dl, 32
    jb .main_loop
    cmp dl, 126
    ja .main_loop
    ; Max 12 chars
    cmp byte [cs:input_len], 12
    jae .main_loop
    ; Auto-uppercase
    cmp dl, 'a'
    jb .dialog_store
    cmp dl, 'z'
    ja .dialog_store
    sub dl, 32
.dialog_store:
    movzx bx, byte [cs:input_len]
    mov [cs:input_buf + bx], dl
    inc byte [cs:input_len]
    movzx bx, byte [cs:input_len]
    mov byte [cs:input_buf + bx], 0
    call draw_status
    jmp .main_loop

.dialog_backspace:
    cmp byte [cs:input_len], 0
    je .main_loop
    dec byte [cs:input_len]
    movzx bx, byte [cs:input_len]
    mov byte [cs:input_buf + bx], 0
    call draw_status
    jmp .main_loop

.dialog_submit:
    cmp byte [cs:input_len], 0
    je .main_loop
    ; Copy input_buf to filename_buf
    mov si, input_buf
    mov di, filename_buf
    xor cx, cx
.ds_copy:
    mov al, [cs:si]
    mov [cs:di], al
    test al, al
    jz .ds_copied
    inc si
    inc di
    inc cl
    cmp cl, 13
    jb .ds_copy
.ds_copied:
    mov byte [cs:di], 0
    cmp byte [cs:mode], MODE_OPEN
    je .do_open_submit
    ; MODE_SAVE
    call do_save_file
    mov byte [cs:mode], MODE_EDIT
    call draw_ui
    jmp .main_loop
.do_open_submit:
    call do_open_file
    mov byte [cs:mode], MODE_EDIT
    call draw_ui
    jmp .main_loop

.dialog_cancel:
    mov byte [cs:mode], MODE_EDIT
    call draw_status
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
; update_after_edit - After insert/delete, ensure cursor visible and redraw
; ============================================================================
update_after_edit:
    pusha
    call cursor_to_line_col
    call ensure_cursor_visible
    call draw_text_area
    call draw_status
    popa
    ret

; ============================================================================
; draw_current_line - Fast redraw of only the cursor's line
; ============================================================================
draw_current_line:
    pusha
    call cursor_to_line_col

    ; Check if cursor is visible without scrolling
    mov ax, [cs:cursor_line]
    cmp ax, [cs:scroll_row]
    jb .dcl_full
    mov bx, [cs:scroll_row]
    add bx, [cs:vis_rows]
    cmp ax, bx
    jae .dcl_full

    ; Cursor is visible — compute screen row
    sub ax, [cs:scroll_row]

    ; Calculate Y = TEXT_Y + screen_row * row_h
    movzx dx, byte [cs:row_h]
    mul dx
    add ax, TEXT_Y
    mov [cs:.dcl_y], ax

    ; Clear just this line's strip
    mov bx, TEXT_X
    mov cx, ax
    mov dx, TEXT_W
    movzx si, byte [cs:row_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Find byte offset for cursor_line
    mov cx, [cs:cursor_line]
    call find_line_start                ; BX = start of line

    ; Copy line to line_buf
    mov si, bx
    xor di, di
.dcl_copy:
    cmp di, [cs:vis_cols]
    jae .dcl_line_end
    cmp si, [cs:text_len]
    jae .dcl_line_end
    mov al, [cs:text_buf + si]
    cmp al, 0x0A
    je .dcl_line_end
    mov [cs:line_buf + di], al
    inc si
    inc di
    jmp .dcl_copy
.dcl_line_end:
    mov byte [cs:line_buf + di], 0

    ; Draw line text
    mov bx, TEXT_X
    mov cx, [cs:.dcl_y]
    mov si, line_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw cursor only if no selection
    cmp word [cs:sel_anchor], 0xFFFF
    jne .dcl_no_cursor

    ; Draw cursor (inverted char at cursor_col)
    mov ax, [cs:cursor_col]
    cmp ax, [cs:vis_cols]
    jae .dcl_no_cursor
    movzx dx, byte [cs:font_adv]
    mul dx
    add ax, TEXT_X
    mov bx, ax
    mov cx, [cs:.dcl_y]

    push bx
    mov bx, [cs:cursor_pos]
    cmp bx, [cs:text_len]
    jae .dcl_cursor_space
    mov al, [cs:text_buf + bx]
    cmp al, 0x0A
    je .dcl_cursor_space
    cmp al, 32
    jb .dcl_cursor_space
    mov [cs:cursor_char_buf], al
    jmp .dcl_cursor_got
.dcl_cursor_space:
    mov byte [cs:cursor_char_buf], ' '
.dcl_cursor_got:
    mov byte [cs:cursor_char_buf + 1], 0
    pop bx
    mov si, cursor_char_buf
    mov ah, API_GFX_DRAW_STRING_INV
    int 0x80

.dcl_no_cursor:
    popa
    ret

.dcl_full:
    call ensure_cursor_visible
    call draw_text_area
    popa
    ret

.dcl_y: dw 0

; ============================================================================
; draw_typed_char - Ultra-fast: draw just the typed char + cursor (2 API calls)
; ============================================================================
draw_typed_char:
    pusha

    ; Check if cursor is visible
    mov ax, [cs:cursor_line]
    cmp ax, [cs:scroll_row]
    jb .dtc_full
    mov bx, [cs:scroll_row]
    add bx, [cs:vis_rows]
    cmp ax, bx
    jae .dtc_full

    ; Check cursor_col is visible
    mov ax, [cs:cursor_col]
    cmp ax, [cs:vis_cols]
    jae .dtc_done

    ; Calculate Y
    mov ax, [cs:cursor_line]
    sub ax, [cs:scroll_row]
    movzx dx, byte [cs:row_h]
    mul dx
    add ax, TEXT_Y
    mov [cs:.dtc_y], ax

    ; Draw the typed char at cursor_col - 1
    mov ax, [cs:cursor_col]
    dec ax
    movzx dx, byte [cs:font_adv]
    mul dx
    add ax, TEXT_X
    mov bx, ax
    mov cx, [cs:.dtc_y]

    push bx
    mov bx, [cs:cursor_pos]
    dec bx
    mov al, [cs:text_buf + bx]
    mov [cs:cursor_char_buf], al
    mov byte [cs:cursor_char_buf + 1], 0
    pop bx
    mov si, cursor_char_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw cursor at cursor_col
    mov ax, [cs:cursor_col]
    cmp ax, [cs:vis_cols]
    jae .dtc_done
    movzx dx, byte [cs:font_adv]
    mul dx
    add ax, TEXT_X
    mov bx, ax
    mov cx, [cs:.dtc_y]
    mov byte [cs:cursor_char_buf], ' '
    mov byte [cs:cursor_char_buf + 1], 0
    mov si, cursor_char_buf
    mov ah, API_GFX_DRAW_STRING_INV
    int 0x80

.dtc_done:
    popa
    ret

.dtc_full:
    call ensure_cursor_visible
    call draw_text_area
    call draw_status
    popa
    ret

.dtc_y: dw 0

; ============================================================================
; Selection Helpers
; ============================================================================

; clear_selection_silent - Clear selection without redraw
clear_selection_silent:
    mov word [cs:sel_anchor], 0xFFFF
    ret

; update_selection_bounds - Compute sel_start/sel_end from anchor and cursor_pos
update_selection_bounds:
    push ax
    push bx
    mov ax, [cs:sel_anchor]
    mov bx, [cs:cursor_pos]
    cmp ax, bx
    jbe .usb_ordered
    xchg ax, bx
.usb_ordered:
    mov [cs:sel_start], ax
    mov [cs:sel_end], bx
    pop bx
    pop ax
    ret

; handle_shift_for_move - Set anchor if shift held and no anchor yet
handle_shift_for_move:
    cmp word [cs:sel_anchor], 0xFFFF
    jne .hsfm_done
    mov ax, [cs:cursor_pos]
    mov [cs:sel_anchor], ax
.hsfm_done:
    ret

; delete_selection - Remove selected text, set cursor to sel_start, clear selection
delete_selection:
    pusha
    cmp word [cs:sel_anchor], 0xFFFF
    je .dsel_done

    call update_selection_bounds
    mov ax, [cs:sel_start]
    mov bx, [cs:sel_end]
    cmp ax, bx
    je .dsel_clear

    ; Shift text_buf[sel_end..text_len) left to sel_start
    mov cx, [cs:text_len]
    sub cx, bx                          ; CX = bytes after sel_end
    jcxz .dsel_no_shift

    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, text_buf
    add si, bx                          ; SI = &text_buf[sel_end]
    mov di, text_buf
    add di, ax                          ; DI = &text_buf[sel_start]
    cld
    rep movsb
    pop es
    pop ds

.dsel_no_shift:
    ; Update text_len
    mov cx, [cs:sel_end]
    sub cx, [cs:sel_start]
    sub [cs:text_len], cx

    ; Cursor to sel_start
    mov ax, [cs:sel_start]
    mov [cs:cursor_pos], ax

.dsel_clear:
    mov word [cs:sel_anchor], 0xFFFF

.dsel_done:
    popa
    ret

; ============================================================================
; Clipboard Operations
; ============================================================================

; do_copy - Copy selected text to clipboard
do_copy:
    pusha
    cmp word [cs:sel_anchor], 0xFFFF
    je .dcopy_done

    call update_selection_bounds
    mov cx, [cs:sel_end]
    sub cx, [cs:sel_start]
    jcxz .dcopy_done

    cmp cx, CLIP_MAX
    jbe .dcopy_ok
    mov cx, CLIP_MAX
.dcopy_ok:
    mov [cs:clip_len], cx

    ; Copy text_buf[sel_start..] to clip_buf
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, text_buf
    add si, [cs:sel_start]
    mov di, clip_buf
    cld
    rep movsb
    pop es
    pop ds

.dcopy_done:
    popa
    ret

; do_cut - Copy selection to clipboard, then delete it
do_cut:
    pusha
    cmp word [cs:sel_anchor], 0xFFFF
    je .dcut_done
    call save_undo
    mov byte [cs:undo_saved_for_edit], 1
    call do_copy
    call delete_selection
.dcut_done:
    popa
    ret

; do_paste - Insert clipboard at cursor (delete selection first if active)
do_paste:
    pusha
    mov cx, [cs:clip_len]
    test cx, cx
    jz .dpaste_done

    ; Check room
    mov ax, [cs:text_len]
    cmp word [cs:sel_anchor], 0xFFFF
    je .dpaste_no_sel_check
    ; Account for selection deletion
    push bx
    call update_selection_bounds
    mov bx, [cs:sel_end]
    sub bx, [cs:sel_start]
    sub ax, bx
    pop bx
.dpaste_no_sel_check:
    add ax, cx
    cmp ax, TEXT_MAX
    ja .dpaste_done

    call save_undo
    mov byte [cs:undo_saved_for_edit], 1

    ; Delete selection if active
    cmp word [cs:sel_anchor], 0xFFFF
    je .dpaste_no_del
    call delete_selection
.dpaste_no_del:

    ; Shift text right by clip_len at cursor_pos
    mov cx, [cs:text_len]
    sub cx, [cs:cursor_pos]            ; CX = bytes to shift right
    jcxz .dpaste_no_shift

    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, text_buf
    add si, [cs:text_len]
    dec si                              ; SI = &text_buf[text_len-1]
    mov di, si
    add di, [cs:clip_len]              ; DI = SI + clip_len
    std
    rep movsb
    cld
    pop es
    pop ds

.dpaste_no_shift:
    ; Copy clip_buf into gap at cursor_pos
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, clip_buf
    mov di, text_buf
    add di, [cs:cursor_pos]
    mov cx, [cs:clip_len]
    cld
    rep movsb
    pop es
    pop ds

    ; Update text_len and cursor_pos
    mov ax, [cs:clip_len]
    add [cs:text_len], ax
    add [cs:cursor_pos], ax

.dpaste_done:
    popa
    ret

; ============================================================================
; Undo Operations
; ============================================================================

; maybe_save_undo - Save undo snapshot if not already saved for this edit group
maybe_save_undo:
    cmp byte [cs:undo_saved_for_edit], 0
    jne .msu_done
    call save_undo
    mov byte [cs:undo_saved_for_edit], 1
.msu_done:
    ret

; save_undo - Snapshot text_buf to undo_buf
save_undo:
    pusha
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, text_buf
    mov di, undo_buf
    mov cx, [cs:text_len]
    jcxz .su_no_copy
    cld
    rep movsb
.su_no_copy:
    pop es
    pop ds

    mov ax, [cs:text_len]
    mov [cs:undo_len], ax
    mov ax, [cs:cursor_pos]
    mov [cs:undo_cursor], ax
    mov ax, [cs:scroll_row]
    mov [cs:undo_scroll], ax
    mov byte [cs:undo_valid], 1

    popa
    ret

; do_undo - Swap text_buf and undo_buf (toggle undo/redo)
do_undo:
    pusha
    cmp byte [cs:undo_valid], 0
    je .du_done

    ; Find max length to swap
    mov cx, [cs:text_len]
    cmp cx, [cs:undo_len]
    jae .du_got_max
    mov cx, [cs:undo_len]
.du_got_max:
    jcxz .du_swap_meta

    ; Byte-by-byte swap
    xor bx, bx
.du_swap_loop:
    mov al, [cs:text_buf + bx]
    mov dl, [cs:undo_buf + bx]
    mov [cs:text_buf + bx], dl
    mov [cs:undo_buf + bx], al
    inc bx
    dec cx
    jnz .du_swap_loop

.du_swap_meta:
    ; Swap text_len / undo_len
    mov ax, [cs:text_len]
    mov dx, [cs:undo_len]
    mov [cs:text_len], dx
    mov [cs:undo_len], ax

    ; Swap cursor_pos / undo_cursor
    mov ax, [cs:cursor_pos]
    mov dx, [cs:undo_cursor]
    mov [cs:cursor_pos], dx
    mov [cs:undo_cursor], ax

    ; Swap scroll_row / undo_scroll
    mov ax, [cs:scroll_row]
    mov dx, [cs:undo_scroll]
    mov [cs:scroll_row], dx
    mov [cs:undo_scroll], ax

    ; Clear selection
    mov word [cs:sel_anchor], 0xFFFF

.du_done:
    popa
    ret

; ============================================================================
; Cursor Movement Functions
; ============================================================================

cursor_up:
    pusha
    call cursor_to_line_col
    cmp word [cs:cursor_line], 0
    je .cu_done

    mov cx, [cs:cursor_line]
    dec cx
    call find_line_start
    push bx
    call find_line_end
    pop ax
    sub bx, ax

    mov dx, [cs:cursor_col]
    cmp dx, bx
    jbe .cu_col_ok
    mov dx, bx
.cu_col_ok:
    add ax, dx
    mov [cs:cursor_pos], ax
.cu_done:
    popa
    ret

cursor_down:
    pusha
    call cursor_to_line_col

    mov cx, [cs:cursor_line]
    inc cx
    call find_line_start
    cmp bx, [cs:text_len]
    ja .cd_done

    push bx
    call find_line_end
    pop ax
    sub bx, ax

    mov dx, [cs:cursor_col]
    cmp dx, bx
    jbe .cd_col_ok
    mov dx, bx
.cd_col_ok:
    add ax, dx
    mov [cs:cursor_pos], ax
.cd_done:
    popa
    ret

cursor_home:
    pusha
    call cursor_to_line_col
    mov cx, [cs:cursor_line]
    call find_line_start
    mov [cs:cursor_pos], bx
    popa
    ret

cursor_end:
    pusha
    call cursor_to_line_col
    mov cx, [cs:cursor_line]
    call find_line_start
    call find_line_end
    mov [cs:cursor_pos], bx
    popa
    ret

cursor_pgup:
    pusha
    call cursor_to_line_col
    mov ax, [cs:cursor_line]
    mov bx, [cs:vis_rows]
    cmp ax, bx
    jae .pgup_sub
    xor ax, ax
    jmp .pgup_go
.pgup_sub:
    sub ax, bx
.pgup_go:
    ; Move to target line, same column
    mov cx, ax
    call find_line_start
    push bx
    call find_line_end
    pop ax
    sub bx, ax

    mov dx, [cs:cursor_col]
    cmp dx, bx
    jbe .pgup_col_ok
    mov dx, bx
.pgup_col_ok:
    add ax, dx
    mov [cs:cursor_pos], ax
    popa
    ret

cursor_pgdn:
    pusha
    call cursor_to_line_col
    mov ax, [cs:cursor_line]
    add ax, [cs:vis_rows]

    ; Move to target line, same column
    mov cx, ax
    call find_line_start
    cmp bx, [cs:text_len]
    ja .pgdn_clamp
    push bx
    call find_line_end
    pop ax
    sub bx, ax

    mov dx, [cs:cursor_col]
    cmp dx, bx
    jbe .pgdn_col_ok
    mov dx, bx
.pgdn_col_ok:
    add ax, dx
    mov [cs:cursor_pos], ax
    popa
    ret

.pgdn_clamp:
    ; Past end of text - go to text_len
    mov ax, [cs:text_len]
    mov [cs:cursor_pos], ax
    popa
    ret

; ============================================================================
; Buffer Operations
; ============================================================================

buf_insert_char:
    pusha
    mov ax, [cs:text_len]
    cmp ax, TEXT_MAX
    jae .bi_done

    mov cx, ax
    sub cx, [cs:cursor_pos]
    jcxz .bi_no_shift

    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, text_buf
    add si, [cs:text_len]
    dec si
    mov di, si
    inc di
    std
    rep movsb
    cld
    pop es
    pop ds

.bi_no_shift:
    mov bx, [cs:cursor_pos]
    mov [cs:text_buf + bx], dl
    inc word [cs:text_len]
    inc word [cs:cursor_pos]

.bi_done:
    popa
    ret

buf_delete_char:
    pusha
    cmp word [cs:cursor_pos], 0
    je .bd_done

    mov cx, [cs:text_len]
    sub cx, [cs:cursor_pos]

    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, text_buf
    add si, [cs:cursor_pos]
    mov di, si
    dec di
    cld
    rep movsb
    pop es
    pop ds

    dec word [cs:text_len]
    dec word [cs:cursor_pos]

.bd_done:
    popa
    ret

buf_delete_fwd:
    pusha
    mov ax, [cs:cursor_pos]
    cmp ax, [cs:text_len]
    jae .bf_done

    mov cx, [cs:text_len]
    dec cx
    sub cx, [cs:cursor_pos]
    jcxz .bf_no_shift

    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, text_buf
    add si, [cs:cursor_pos]
    inc si
    mov di, si
    dec di
    cld
    rep movsb
    pop es
    pop ds

.bf_no_shift:
    dec word [cs:text_len]

.bf_done:
    popa
    ret

; ============================================================================
; Mouse Helpers
; ============================================================================

; mouse_to_content_rel - Convert saved mouse coords to content-relative
; Output: mouse_rel_x, mouse_rel_y set. CF set if can't compute.
mouse_to_content_rel:
    push ax
    push bx
    push cx
    push dx
    push si

    mov al, [cs:win_handle]
    mov ah, API_WIN_GET_INFO
    int 0x80
    ; BX=win_x, CX=win_y

    mov ax, [cs:mouse_abs_x]
    sub ax, bx
    dec ax                              ; -1 for border
    mov [cs:mouse_rel_x], ax

    mov ax, [cs:mouse_abs_y]
    sub ax, cx
    sub ax, TITLEBAR_HEIGHT
    mov [cs:mouse_rel_y], ax

    clc
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; mouse_to_offset - Convert saved mouse coords to text buffer offset
; Output: AX = byte offset, CF set if outside text area
mouse_to_offset:
    push bx
    push cx
    push dx
    push si

    call mouse_to_content_rel

    ; Check text area bounds
    mov ax, [cs:mouse_rel_x]
    cmp ax, TEXT_X
    jb .mto_outside
    cmp ax, TEXT_X + TEXT_W
    jae .mto_outside
    mov ax, [cs:mouse_rel_y]
    cmp ax, TEXT_Y
    jb .mto_outside
    cmp ax, TEXT_Y + TEXT_H
    jae .mto_outside

    ; Compute column
    mov ax, [cs:mouse_rel_x]
    sub ax, TEXT_X
    xor dx, dx
    movzx bx, byte [cs:font_adv]
    div bx
    push ax                             ; Save column

    ; Compute line number
    mov ax, [cs:mouse_rel_y]
    sub ax, TEXT_Y
    xor dx, dx
    movzx bx, byte [cs:row_h]
    div bx
    add ax, [cs:scroll_row]            ; AX = absolute line number

    ; Find byte offset for this line
    mov cx, ax
    call find_line_start                ; BX = start of line
    push bx                             ; Save line start
    call find_line_end                  ; BX = end of line
    pop ax                              ; AX = line start
    sub bx, ax                          ; BX = line length

    ; Column = min(mouse_col, line_length)
    pop cx                              ; CX = mouse column
    cmp cx, bx
    jbe .mto_col_ok
    mov cx, bx
.mto_col_ok:
    add ax, cx                          ; AX = byte offset

    clc
    pop si
    pop dx
    pop cx
    pop bx
    ret

.mto_outside:
    stc
    pop si
    pop dx
    pop cx
    pop bx
    ret

; ============================================================================
; Context Menu Drawing
; ============================================================================

; draw_menu - Draw the currently active menu (context or file)
; Uses: menu_x, menu_y, active_menu_strings, active_menu_count, active_menu_w
draw_menu:
    pusha

    ; Compute menu height
    movzx si, byte [cs:active_menu_count]
    imul si, MENU_ITEM_H

    ; White filled rect
    mov bx, [cs:menu_x]
    mov cx, [cs:menu_y]
    movzx dx, byte [cs:active_menu_w]
    mov al, 3                           ; White
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Black border
    mov bx, [cs:menu_x]
    mov cx, [cs:menu_y]
    movzx dx, byte [cs:active_menu_w]
    movzx si, byte [cs:active_menu_count]
    imul si, MENU_ITEM_H
    mov al, 0                           ; Black
    mov ah, API_RECT_COLOR
    int 0x80

    ; Draw items
    xor cx, cx                          ; CX = item index
.dm_loop:
    cmp cl, [cs:active_menu_count]
    jae .dm_done

    ; Compute item text Y = menu_y + index * MENU_ITEM_H + 2
    mov ax, cx
    mov bx, MENU_ITEM_H
    mul bx
    add ax, [cs:menu_y]
    add ax, 2
    push cx                             ; Save index
    mov [cs:.dm_item_y], ax

    ; Get string pointer from active string table
    mov bx, cx
    shl bx, 1
    add bx, [cs:active_menu_strings]
    mov si, [cs:bx]

    ; Text X = menu_x + 4
    mov bx, [cs:menu_x]
    add bx, 4
    mov cx, [cs:.dm_item_y]
    mov ah, API_GFX_DRAW_STRING_INV     ; Black text on white
    int 0x80

    pop cx
    inc cx
    jmp .dm_loop

.dm_done:
    popa
    ret

.dm_item_y: dw 0

; ============================================================================
; compute_layout - Measure current font and compute visible cols/rows
; ============================================================================
compute_layout:
    pusha
    mov si, test_char
    mov ah, API_GFX_TEXT_WIDTH
    int 0x80
    mov [cs:font_adv], dl

    cmp dl, 8
    jae .large_font
    mov al, dl
    inc al
    mov [cs:row_h], al
    jmp .calc_grid
.large_font:
    mov al, dl
    mov [cs:row_h], al

.calc_grid:
    mov ax, TEXT_W
    xor dx, dx
    movzx bx, byte [cs:font_adv]
    div bx
    mov [cs:vis_cols], ax

    mov ax, TEXT_H
    xor dx, dx
    movzx bx, byte [cs:row_h]
    div bx
    mov [cs:vis_rows], ax

    popa
    ret

; ============================================================================
; draw_ui - Full UI redraw
; ============================================================================
draw_ui:
    pusha
    mov bx, 0
    mov cx, 0
    mov dx, CONTENT_W
    mov si, CONTENT_H
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    call draw_menubar

    ; Separators
    mov bx, 0
    mov cx, SEP1_Y
    mov dx, CONTENT_W
    mov al, 3
    mov ah, API_DRAW_HLINE
    int 0x80
    mov bx, 0
    mov cx, SEP2_Y
    mov dx, CONTENT_W
    mov al, 3
    mov ah, API_DRAW_HLINE
    int 0x80

    call draw_text_area
    call draw_status

    popa
    ret

; ============================================================================
; draw_menubar - Draw menu bar with "File" label and filename
; ============================================================================
draw_menubar:
    pusha

    ; "File" label
    mov bx, FILE_LABEL_X
    mov cx, MENUBAR_Y + 2
    mov si, str_file
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Filename display
    mov bx, FNAME_X
    mov cx, MENUBAR_Y + 2
    cmp byte [cs:filename_buf], 0
    je .no_fname
    mov si, filename_buf
    jmp .draw_fname
.no_fname:
    mov si, str_untitled
.draw_fname:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; draw_text_area - Draw visible text lines with selection highlighting
; ============================================================================
draw_text_area:
    pusha
    ; Clear text area
    mov bx, TEXT_X
    mov cx, TEXT_Y
    movzx si, byte [cs:row_h]
    mov ax, [cs:vis_rows]
    mul si
    mov si, ax
    mov dx, TEXT_W
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Compute cursor line/col
    call cursor_to_line_col

    ; Pre-compute selection bounds if active
    cmp word [cs:sel_anchor], 0xFFFF
    je .dta_no_sel_init
    call update_selection_bounds
.dta_no_sel_init:

    ; Find byte offset for scroll_row
    mov cx, [cs:scroll_row]
    call find_line_start
    ; BX = offset of first visible line

    mov word [cs:draw_row], 0
    mov [cs:line_offset], bx

.dta_row_loop:
    mov ax, [cs:draw_row]
    cmp ax, [cs:vis_rows]
    jae .dta_rows_done
    mov bx, [cs:line_offset]
    cmp bx, [cs:text_len]
    ja .dta_rows_done

    ; Copy line to line_buf (up to vis_cols chars)
    mov si, bx
    xor di, di
.dta_copy_char:
    cmp di, [cs:vis_cols]
    jae .dta_line_end
    cmp si, [cs:text_len]
    jae .dta_line_end
    mov al, [cs:text_buf + si]
    cmp al, 0x0A
    je .dta_line_end
    mov [cs:line_buf + di], al
    inc si
    inc di
    jmp .dta_copy_char
.dta_line_end:
    mov byte [cs:line_buf + di], 0
    mov [cs:line_char_count], di

    ; Calculate Y for this row
    mov ax, [cs:draw_row]
    movzx dx, byte [cs:row_h]
    mul dx
    add ax, TEXT_Y
    mov [cs:draw_y], ax

    ; Check if selection overlaps this line
    cmp word [cs:sel_anchor], 0xFFFF
    je .dta_draw_normal

    ; Compute selection column range on this line
    ; sel_col_start = max(0, sel_start - line_offset) clamped to line_char_count
    mov ax, [cs:sel_start]
    mov bx, [cs:line_offset]
    cmp ax, bx
    jbe .dta_scs_zero
    sub ax, bx
    cmp ax, [cs:line_char_count]
    jbe .dta_scs_ok
    mov ax, [cs:line_char_count]
    jmp .dta_scs_ok
.dta_scs_zero:
    xor ax, ax
.dta_scs_ok:
    mov [cs:sel_col_s], ax

    ; sel_col_end = max(0, sel_end - line_offset) clamped to line_char_count
    mov ax, [cs:sel_end]
    mov bx, [cs:line_offset]
    cmp ax, bx
    jbe .dta_sce_zero
    sub ax, bx
    cmp ax, [cs:line_char_count]
    jbe .dta_sce_ok
    mov ax, [cs:line_char_count]
    jmp .dta_sce_ok
.dta_sce_zero:
    xor ax, ax
.dta_sce_ok:
    mov [cs:sel_col_e], ax

    ; If no visible selection on this line, draw normally
    mov ax, [cs:sel_col_s]
    cmp ax, [cs:sel_col_e]
    jae .dta_draw_normal

    ; --- Draw line with selection (up to 3 segments) ---

    ; Segment 1: before selection (0..sel_col_s)
    mov ax, [cs:sel_col_s]
    test ax, ax
    jz .dta_seg2

    ; Null-terminate at sel_col_s
    mov bx, [cs:sel_col_s]
    mov al, [cs:line_buf + bx]
    mov [cs:saved_char], al
    mov byte [cs:line_buf + bx], 0

    mov bx, TEXT_X
    mov cx, [cs:draw_y]
    mov si, line_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Restore
    mov bx, [cs:sel_col_s]
    mov al, [cs:saved_char]
    mov [cs:line_buf + bx], al

.dta_seg2:
    ; Segment 2: selected text (sel_col_s..sel_col_e)
    mov bx, [cs:sel_col_e]
    mov al, [cs:line_buf + bx]
    mov [cs:saved_char], al
    mov byte [cs:line_buf + bx], 0

    ; X = TEXT_X + sel_col_s * font_adv
    mov ax, [cs:sel_col_s]
    movzx dx, byte [cs:font_adv]
    mul dx
    add ax, TEXT_X
    mov bx, ax
    mov cx, [cs:draw_y]
    mov si, line_buf
    add si, [cs:sel_col_s]
    mov ah, API_GFX_DRAW_STRING_INV
    int 0x80

    ; Restore
    mov bx, [cs:sel_col_e]
    mov al, [cs:saved_char]
    mov [cs:line_buf + bx], al

    ; Segment 3: after selection (sel_col_e..end)
    mov ax, [cs:sel_col_e]
    cmp ax, [cs:line_char_count]
    jae .dta_skip_cursor

    ; X = TEXT_X + sel_col_e * font_adv
    movzx dx, byte [cs:font_adv]
    mul dx
    add ax, TEXT_X
    mov bx, ax
    mov cx, [cs:draw_y]
    mov si, line_buf
    add si, [cs:sel_col_e]
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    jmp .dta_skip_cursor                ; No cursor block when selection active

.dta_draw_normal:
    ; Draw line text normally
    mov bx, TEXT_X
    mov cx, [cs:draw_y]
    mov si, line_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Check if cursor is on this line (only when no selection)
    cmp word [cs:sel_anchor], 0xFFFF
    jne .dta_skip_cursor

    mov ax, [cs:draw_row]
    add ax, [cs:scroll_row]
    cmp ax, [cs:cursor_line]
    jne .dta_skip_cursor

    ; Draw cursor (inverted char at cursor_col)
    mov ax, [cs:cursor_col]
    cmp ax, [cs:vis_cols]
    jae .dta_skip_cursor
    movzx dx, byte [cs:font_adv]
    mul dx
    add ax, TEXT_X
    mov bx, ax

    mov ax, [cs:draw_row]
    movzx dx, byte [cs:row_h]
    mul dx
    add ax, TEXT_Y
    mov cx, ax

    push bx
    mov bx, [cs:cursor_pos]
    cmp bx, [cs:text_len]
    jae .dta_cursor_space
    mov al, [cs:text_buf + bx]
    cmp al, 0x0A
    je .dta_cursor_space
    cmp al, 32
    jb .dta_cursor_space
    mov [cs:cursor_char_buf], al
    jmp .dta_cursor_got
.dta_cursor_space:
    mov byte [cs:cursor_char_buf], ' '
.dta_cursor_got:
    mov byte [cs:cursor_char_buf + 1], 0
    pop bx
    mov si, cursor_char_buf
    mov ah, API_GFX_DRAW_STRING_INV
    int 0x80

.dta_skip_cursor:
    ; Advance past this line in text_buf
    mov bx, [cs:line_offset]
.dta_skip_line:
    cmp bx, [cs:text_len]
    jae .dta_advance_row
    cmp byte [cs:text_buf + bx], 0x0A
    je .dta_found_nl
    inc bx
    jmp .dta_skip_line
.dta_found_nl:
    inc bx
.dta_advance_row:
    mov [cs:line_offset], bx
    inc word [cs:draw_row]
    jmp .dta_row_loop

.dta_rows_done:
    popa
    ret

; ============================================================================
; draw_status - Draw status bar or dialog
; ============================================================================
draw_status:
    pusha
    mov bx, 0
    mov cx, STATUS_Y - 1
    mov dx, CONTENT_W
    mov si, CONTENT_H - STATUS_Y + 2
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    cmp byte [cs:mode], MODE_EDIT
    je .status_normal
    jmp .status_dialog

.status_normal:
    call cursor_to_line_col

    mov bx, 4
    mov cx, STATUS_Y
    mov si, str_ln
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [cs:cursor_line]
    inc ax
    mov di, num_buf
    call word_to_decimal
    mov byte [cs:di], 0
    mov bx, 22
    mov cx, STATUS_Y
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 60
    mov cx, STATUS_Y
    mov si, str_col
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [cs:cursor_col]
    inc ax
    mov di, num_buf
    call word_to_decimal
    mov byte [cs:di], 0
    mov bx, 84
    mov cx, STATUS_Y
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Byte count on the right
    mov ax, [cs:text_len]
    mov di, num_buf
    call word_to_decimal
    mov byte [cs:di], ' '
    inc di
    mov byte [cs:di], 'B'
    inc di
    mov byte [cs:di], 0
    mov bx, 240
    mov cx, STATUS_Y
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Status message
    cmp byte [cs:status_msg], 0
    je .status_done
    mov bx, 130
    mov cx, STATUS_Y
    mov si, status_msg
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov byte [cs:status_msg], 0
    jmp .status_done

.status_dialog:
    cmp byte [cs:mode], MODE_OPEN
    je .dialog_open_label
    mov si, str_save_as
    jmp .dialog_draw_label
.dialog_open_label:
    mov si, str_open_file
.dialog_draw_label:
    mov bx, 4
    mov cx, STATUS_Y
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 70
    mov cx, STATUS_Y
    mov dx, 140
    mov si, input_buf
    movzx di, byte [cs:input_len]
    mov al, 1
    mov ah, API_DRAW_TEXTFIELD
    int 0x80

    mov ax, cs
    mov es, ax
    mov bx, BTN_OK_X
    mov cx, STATUS_Y
    mov dx, BTN_OK_W
    mov si, BTN_H
    mov di, str_ok
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

.status_done:
    popa
    ret

; ============================================================================
; File I/O
; ============================================================================

do_open_file:
    pusha
    mov si, filename_buf
    movzx bx, byte [cs:mount_handle]
    mov ah, API_FS_OPEN
    int 0x80
    jc .of_fail
    mov [cs:file_handle], al

    push cs
    pop es
    mov di, text_buf
    mov cx, TEXT_MAX
    mov al, [cs:file_handle]
    mov ah, API_FS_READ
    int 0x80
    jc .of_close_fail
    mov [cs:text_len], ax

    mov al, [cs:file_handle]
    mov ah, API_FS_CLOSE
    int 0x80

    call strip_cr

    mov word [cs:cursor_pos], 0
    mov word [cs:scroll_row], 0
    mov word [cs:cursor_line], 0
    mov word [cs:cursor_col], 0
    mov word [cs:sel_anchor], 0xFFFF
    mov byte [cs:undo_valid], 0
    mov byte [cs:undo_saved_for_edit], 0

    mov si, str_opened
    mov di, status_msg
    call copy_str

    popa
    ret

.of_close_fail:
    mov al, [cs:file_handle]
    mov ah, API_FS_CLOSE
    int 0x80
.of_fail:
    mov si, str_err_open
    mov di, status_msg
    call copy_str
    popa
    ret

do_save_file:
    pusha
    cmp byte [cs:filename_buf], 0
    je .sf_fail

    mov si, filename_buf
    mov bl, [cs:mount_handle]
    mov ah, API_FS_DELETE
    int 0x80

    mov si, filename_buf
    mov bl, [cs:mount_handle]
    mov ah, API_FS_CREATE
    int 0x80
    jc .sf_fail
    mov [cs:file_handle], al

    mov ax, cs
    mov es, ax
    mov bx, text_buf
    mov cx, [cs:text_len]
    mov al, [cs:file_handle]
    mov ah, API_FS_WRITE
    int 0x80

    mov al, [cs:file_handle]
    mov ah, API_FS_CLOSE
    int 0x80

    mov si, str_saved
    mov di, status_msg
    call copy_str

    popa
    ret

.sf_fail:
    mov si, str_err_save
    mov di, status_msg
    call copy_str
    popa
    ret

do_new_file:
    pusha
    mov word [cs:text_len], 0
    mov word [cs:cursor_pos], 0
    mov word [cs:scroll_row], 0
    mov word [cs:cursor_line], 0
    mov word [cs:cursor_col], 0
    mov byte [cs:filename_buf], 0
    mov byte [cs:mode], MODE_EDIT
    mov byte [cs:status_msg], 0
    mov word [cs:sel_anchor], 0xFFFF
    mov word [cs:clip_len], 0
    mov byte [cs:undo_valid], 0
    mov byte [cs:undo_saved_for_edit], 0
    popa
    ret

; ============================================================================
; Utility Functions
; ============================================================================

cursor_to_line_col:
    pusha
    xor cx, cx
    xor dx, dx
    xor bx, bx
.ctl_scan:
    cmp bx, [cs:cursor_pos]
    jae .ctl_done
    cmp byte [cs:text_buf + bx], 0x0A
    jne .ctl_not_nl
    inc cx
    xor dx, dx
    jmp .ctl_next
.ctl_not_nl:
    inc dx
.ctl_next:
    inc bx
    jmp .ctl_scan
.ctl_done:
    mov [cs:cursor_line], cx
    mov [cs:cursor_col], dx
    popa
    ret

find_line_start:
    push cx
    push ax
    xor bx, bx
    test cx, cx
    jz .fls_done
.fls_scan:
    cmp bx, [cs:text_len]
    jae .fls_done
    cmp byte [cs:text_buf + bx], 0x0A
    jne .fls_next
    dec cx
    jz .fls_found
.fls_next:
    inc bx
    jmp .fls_scan
.fls_found:
    inc bx
.fls_done:
    pop ax
    pop cx
    ret

find_line_end:
    push ax
.fle_scan:
    cmp bx, [cs:text_len]
    jae .fle_done
    cmp byte [cs:text_buf + bx], 0x0A
    je .fle_done
    inc bx
    jmp .fle_scan
.fle_done:
    pop ax
    ret

ensure_cursor_visible:
    pusha
    mov ax, [cs:cursor_line]

    cmp ax, [cs:scroll_row]
    jae .ecv_check_below
    mov [cs:scroll_row], ax
    jmp .ecv_done

.ecv_check_below:
    mov bx, [cs:scroll_row]
    add bx, [cs:vis_rows]
    cmp ax, bx
    jb .ecv_done
    mov bx, ax
    sub bx, [cs:vis_rows]
    inc bx
    mov [cs:scroll_row], bx

.ecv_done:
    popa
    ret

strip_cr:
    pusha
    xor si, si
    xor di, di
.sc_loop:
    cmp si, [cs:text_len]
    jae .sc_done
    mov al, [cs:text_buf + si]
    cmp al, 0x0D
    je .sc_skip
    mov [cs:text_buf + di], al
    inc di
.sc_skip:
    inc si
    jmp .sc_loop
.sc_done:
    mov [cs:text_len], di
    popa
    ret

copy_str:
    push ax
.cs_loop:
    mov al, [cs:si]
    mov [cs:di], al
    test al, al
    jz .cs_done
    inc si
    inc di
    jmp .cs_loop
.cs_done:
    pop ax
    ret

word_to_decimal:
    push ax
    push bx
    push cx
    push dx
    test ax, ax
    jnz .wtd_nz
    mov byte [cs:di], '0'
    inc di
    jmp .wtd_done
.wtd_nz:
    xor cx, cx
    mov bx, 10
.wtd_div:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .wtd_div
.wtd_store:
    pop ax
    add al, '0'
    mov [cs:di], al
    inc di
    loop .wtd_store
.wtd_done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Data Section
; ============================================================================

window_title:   db 'Notepad', 0
win_handle:     db 0
mount_handle:   db 0
file_handle:    db 0
prev_btn:       db 0
mode:           db MODE_EDIT
needs_redraw:   db 0

; Font metrics
font_adv:       db 6
row_h:          db 7
vis_cols:       dw 52
vis_rows:       dw 22

; Cursor state
cursor_pos:     dw 0
cursor_line:    dw 0
cursor_col:     dw 0
scroll_row:     dw 0

; Text buffer state
text_len:       dw 0

; Drawing scratch
draw_row:       dw 0
draw_y:         dw 0
line_offset:    dw 0
line_char_count: dw 0
saved_char:     db 0

; Selection state
sel_anchor:     dw 0xFFFF               ; 0xFFFF = no selection
sel_start:      dw 0
sel_end:        dw 0
sel_col_s:      dw 0                    ; Selection column start on current draw line
sel_col_e:      dw 0                    ; Selection column end on current draw line
shift_held:     db 0
ctrl_held:      db 0

; Mouse state
mouse_abs_x:    dw 0
mouse_abs_y:    dw 0
mouse_rel_x:    dw 0
mouse_rel_y:    dw 0
mouse_buttons:  db 0
mouse_selecting: db 0
prev_right_btn: db 0

; Clipboard state
clip_len:       dw 0

; Undo state
undo_len:       dw 0
undo_cursor:    dw 0
undo_scroll:    dw 0
undo_valid:     db 0
undo_saved_for_edit: db 0

; Menu state (shared by context menu and file menu)
menu_x:         dw 0
menu_y:         dw 0
active_menu_strings: dw 0               ; Pointer to current menu's string table
active_menu_count:   db 0               ; Number of items in current menu
active_menu_w:       db 0               ; Width of current menu (pixels)
active_menu_h:       db 0               ; Height (computed)

; Context menu string table
ctx_menu_strings:
    dw str_cut
    dw str_copy
    dw str_paste
    dw str_undo_label
    dw str_sel_all

; File menu string table
file_menu_strings:
    dw str_new
    dw str_open
    dw str_save
    dw str_save_as_item

; Input state (for dialogs)
input_len:      db 0

; Strings
str_open:       db 'Open', 0
str_save:       db 'Save', 0
str_new:        db 'New', 0
str_ok:         db 'OK', 0
str_untitled:   db '(untitled)', 0
str_file:       db 'File', 0
str_ln:         db 'Ln', 0
str_col:        db 'Col', 0
str_open_file:  db 'Open:', 0
str_save_as:    db 'Save as:', 0
str_save_as_item: db 'Save As', 0
str_opened:     db 'Opened', 0
str_saved:      db 'Saved', 0
str_err_open:   db 'Open error', 0
str_err_save:   db 'Save error', 0
test_char:      db 'A', 0
str_cut:        db 'Cut', 0
str_copy:       db 'Copy', 0
str_paste:      db 'Paste', 0
str_undo_label: db 'Undo', 0
str_sel_all:    db 'Select All', 0

; Small buffers (in binary)
status_msg:     times 20 db 0
input_buf:      times 14 db 0
filename_buf:   times 14 db 0
num_buf:        times 8 db 0
cursor_char_buf: db ' ', 0
line_buf:       times 80 db 0

; ============================================================================
; Large Buffer Addresses (runtime only, not in binary)
; ============================================================================
clip_buf        equ 0x1800              ; 4KB clipboard buffer
text_buf        equ 0x2800              ; 16KB text buffer
undo_buf        equ 0x6800              ; 16KB undo buffer
