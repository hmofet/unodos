; NOTEPAD.BIN - Text Editor for UnoDOS
; Simple text editor with open, save, and cursor navigation.
; Build 261
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
    ; Document icon: white page with cyan text lines
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
API_DRAW_HLINE          equ 69

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Modes
MODE_EDIT               equ 0
MODE_OPEN               equ 1
MODE_SAVE               equ 2

; Layout (content-relative, total window 318x198, content 316x186)
WIN_W                   equ 318
WIN_H                   equ 198
CONTENT_W               equ 316
CONTENT_H               equ 186
TOOLBAR_Y               equ 0
SEP1_Y                  equ 11
TEXT_X                   equ 2
TEXT_Y                  equ 13
TEXT_W                  equ 312
TEXT_H                  equ 155         ; text area height in pixels
SEP2_Y                  equ 170
STATUS_Y                equ 173

; Button layout
BTN_OPEN_X              equ 2
BTN_OPEN_W              equ 40
BTN_SAVE_X              equ 46
BTN_SAVE_W              equ 40
BTN_NEW_X               equ 90
BTN_NEW_W               equ 36
BTN_H                   equ 10
FNAME_X                 equ 132

; Dialog button
BTN_OK_X                equ 260
BTN_OK_W                equ 30

; Buffer
TEXT_MAX                 equ 16384      ; 16KB text buffer

; ============================================================================
; Entry Point
; ============================================================================
entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

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
    test dl, 1
    jz .mouse_up
    cmp byte [cs:prev_btn], 0
    jne .check_event
    mov byte [cs:prev_btn], 1

    ; Click detected — dispatch by mode
    cmp byte [cs:mode], MODE_EDIT
    je .click_edit
    jmp .click_dialog

.mouse_up:
    mov byte [cs:prev_btn], 0

.check_event:
    mov ah, API_EVENT_GET
    int 0x80
    jc .main_loop
    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call compute_layout             ; Re-measure font on redraw
    call draw_ui
    mov byte [cs:needs_redraw], 0   ; Clear flag after full redraw
    jmp .main_loop
.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    ; DL=ASCII, DH=scan code
    cmp byte [cs:mode], MODE_EDIT
    je .key_edit
    jmp .key_dialog

; ============================================================================
; Edit Mode Click Handling
; ============================================================================
.click_edit:
    ; [Open] button
    mov bx, BTN_OPEN_X
    mov cx, TOOLBAR_Y
    mov dx, BTN_OPEN_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_open

    ; [Save] button
    mov bx, BTN_SAVE_X
    mov cx, TOOLBAR_Y
    mov dx, BTN_SAVE_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_save

    ; [New] button
    mov bx, BTN_NEW_X
    mov cx, TOOLBAR_Y
    mov dx, BTN_NEW_W
    mov si, BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .do_new

    jmp .check_event

.start_open:
    mov byte [cs:mode], MODE_OPEN
    mov byte [cs:input_buf], 0
    mov byte [cs:input_len], 0
    call draw_status
    jmp .check_event

.start_save:
    ; If we have a filename, save directly
    cmp byte [cs:filename_buf], 0
    je .start_save_as
    call do_save_file
    call draw_status
    jmp .check_event
.start_save_as:
    mov byte [cs:mode], MODE_SAVE
    mov byte [cs:input_buf], 0
    mov byte [cs:input_len], 0
    call draw_status
    jmp .check_event

.do_new:
    call do_new_file
    call draw_ui
    jmp .main_loop

; ============================================================================
; Edit Mode Key Handling
; ============================================================================
.key_edit:
    ; ESC - exit
    cmp dl, 27
    je .exit_ok

    ; Backspace
    cmp dl, 8
    je .do_backspace

    ; Enter
    cmp dl, 13
    je .do_enter

    ; Tab - insert spaces
    cmp dl, 9
    je .do_tab

    ; Check scan codes for non-ASCII keys
    test dl, dl
    jnz .check_printable

    ; Arrow keys and special keys (DL=0, DH=scan code)
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
    jmp .main_loop

.check_printable:
    ; Printable char (32-126)
    cmp dl, 32
    jb .main_loop
    cmp dl, 126
    ja .main_loop
    call buf_insert_char
    inc word [cs:cursor_col]        ; Incremental column tracking

    ; Check if typing at end of line (fast path: draw 2 chars only)
    mov bx, [cs:cursor_pos]
    cmp bx, [cs:text_len]
    jae .type_at_eol                ; Past end of buffer = EOL
    cmp byte [cs:text_buf + bx], 0x0A
    je .type_at_eol                 ; Next char is newline = EOL

    ; Mid-line insertion: need line redraw (chars shifted right)
    jmp .set_line_redraw

.type_at_eol:
    ; Ultra-fast path: draw just the typed char + cursor (2 API calls)
    call draw_typed_char
    jmp .check_event               ; Check for more events immediately

.do_backspace:
    call buf_delete_char
    mov byte [cs:needs_redraw], 2  ; Full redraw (line structure may change)
    jmp .check_event

.do_enter:
    mov dl, 0x0A
    call buf_insert_char
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_tab:
    mov dl, ' '
    call buf_insert_char
    call buf_insert_char
    call buf_insert_char
    call buf_insert_char
    jmp .set_line_redraw           ; Line-only redraw

.do_delete:
    call buf_delete_fwd
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_cursor_up:
    call cursor_up
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_cursor_down:
    call cursor_down
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_cursor_left:
    cmp word [cs:cursor_pos], 0
    je .main_loop
    dec word [cs:cursor_pos]
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_cursor_right:
    mov ax, [cs:cursor_pos]
    cmp ax, [cs:text_len]
    jae .main_loop
    inc word [cs:cursor_pos]
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_home:
    call cursor_home
    mov byte [cs:needs_redraw], 2
    jmp .check_event

.do_end:
    call cursor_end
    mov byte [cs:needs_redraw], 2
    jmp .check_event

; Helper: set line-only redraw (don't downgrade from full redraw)
.set_line_redraw:
    cmp byte [cs:needs_redraw], 2
    jae .check_event               ; Already needs full redraw, don't downgrade
    mov byte [cs:needs_redraw], 1
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
; update_after_move - After cursor move, ensure visible and redraw
; ============================================================================
update_after_move:
    pusha
    call cursor_to_line_col
    call ensure_cursor_visible
    call draw_text_area
    call draw_status
    popa
    ret

; ============================================================================
; draw_current_line - Fast redraw of only the cursor's line (for typing)
; Falls back to full redraw if scroll is needed.
; ============================================================================
draw_current_line:
    pusha
    ; Compute cursor line/col
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
    sub ax, [cs:scroll_row]          ; AX = screen row (0-based)

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
    call find_line_start              ; BX = start of line

    ; Copy line to line_buf (up to vis_cols chars)
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

    ; Draw cursor (inverted char at cursor_col)
    mov ax, [cs:cursor_col]
    cmp ax, [cs:vis_cols]
    jae .dcl_no_cursor
    movzx dx, byte [cs:font_adv]
    mul dx
    add ax, TEXT_X
    mov bx, ax                       ; BX = cursor X
    mov cx, [cs:.dcl_y]

    ; Get char at cursor position
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
    ; Skip status bar update for speed (updated on full redraws only)
    popa
    ret

.dcl_full:
    ; Scroll needed — fall back to full redraw
    call ensure_cursor_visible
    call draw_text_area
    popa
    ret

.dcl_y: dw 0                         ; Saved Y position for current line

; ============================================================================
; draw_typed_char - Ultra-fast: draw just the typed char + cursor (2 API calls)
; Uses cursor_line (unchanged for printable chars) and cursor_col (incremented
; by caller). No clearing, no line scan, no status bar update.
; ============================================================================
draw_typed_char:
    pusha

    ; Check if cursor is visible (cursor_line within scroll range)
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
    jae .dtc_done                    ; Past visible area, char is in buffer

    ; Calculate Y = (cursor_line - scroll_row) * row_h + TEXT_Y
    mov ax, [cs:cursor_line]
    sub ax, [cs:scroll_row]
    movzx dx, byte [cs:row_h]
    mul dx
    add ax, TEXT_Y
    mov [cs:.dtc_y], ax

    ; Draw the typed char at cursor_col - 1 (replaces old inverted cursor)
    mov ax, [cs:cursor_col]
    dec ax                           ; Position of the char just typed
    movzx dx, byte [cs:font_adv]
    mul dx
    add ax, TEXT_X
    mov bx, ax                      ; BX = X
    mov cx, [cs:.dtc_y]             ; CX = Y

    ; Get the char at cursor_pos - 1 (the one we just inserted)
    push bx
    mov bx, [cs:cursor_pos]
    dec bx
    mov al, [cs:text_buf + bx]
    mov [cs:cursor_char_buf], al
    mov byte [cs:cursor_char_buf + 1], 0
    pop bx
    mov si, cursor_char_buf
    mov ah, API_GFX_DRAW_STRING      ; Normal draw (clears bg under char)
    int 0x80

    ; Draw cursor at cursor_col (inverted space — we're at end of line)
    mov ax, [cs:cursor_col]
    cmp ax, [cs:vis_cols]
    jae .dtc_done                    ; Cursor past visible area
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
    ; Scroll needed — full redraw
    call ensure_cursor_visible
    call draw_text_area
    call draw_status
    popa
    ret

.dtc_y: dw 0

; ============================================================================
; compute_layout - Measure current font and compute visible cols/rows
; ============================================================================
compute_layout:
    pusha
    ; Measure character advance using text_width of "A"
    mov si, test_char
    mov ah, API_GFX_TEXT_WIDTH
    int 0x80
    ; DX = advance width of one character
    mov [cs:font_adv], dl

    ; Derive row height from advance
    cmp dl, 8
    jae .large_font
    ; Small font (advance < 8, likely 6 for font 0)
    mov al, dl
    inc al                          ; height = advance + 1 gap pixel
    mov [cs:row_h], al
    jmp .calc_grid
.large_font:
    ; Larger font (advance >= 8)
    mov al, dl
    mov [cs:row_h], al              ; row_h = advance (generous spacing)

.calc_grid:
    ; vis_cols = TEXT_W / font_adv
    mov ax, TEXT_W
    xor dx, dx
    movzx bx, byte [cs:font_adv]
    div bx
    mov [cs:vis_cols], ax

    ; vis_rows = TEXT_H / row_h
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
    ; Clear entire content area
    mov bx, 0
    mov cx, 0
    mov dx, CONTENT_W
    mov si, CONTENT_H
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Draw toolbar
    call draw_toolbar

    ; Separators
    mov bx, 0
    mov cx, SEP1_Y
    mov dx, CONTENT_W
    mov al, 3                          ; white
    mov ah, API_DRAW_HLINE
    int 0x80
    mov bx, 0
    mov cx, SEP2_Y
    mov dx, CONTENT_W
    mov al, 3
    mov ah, API_DRAW_HLINE
    int 0x80

    ; Text area
    call draw_text_area

    ; Status bar
    call draw_status

    popa
    ret

; ============================================================================
; draw_toolbar - Draw [Open] [Save] [New] buttons and filename
; ============================================================================
draw_toolbar:
    pusha
    mov ax, cs
    mov es, ax

    ; [Open]
    mov bx, BTN_OPEN_X
    mov cx, TOOLBAR_Y
    mov dx, BTN_OPEN_W
    mov si, BTN_H
    mov di, str_open
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; [Save]
    mov bx, BTN_SAVE_X
    mov cx, TOOLBAR_Y
    mov dx, BTN_SAVE_W
    mov si, BTN_H
    mov di, str_save
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; [New]
    mov bx, BTN_NEW_X
    mov cx, TOOLBAR_Y
    mov dx, BTN_NEW_W
    mov si, BTN_H
    mov di, str_new
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; Filename
    mov bx, FNAME_X
    mov cx, TOOLBAR_Y + 2
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
; draw_text_area - Draw visible text lines and cursor
; ============================================================================
draw_text_area:
    pusha
    ; Clear text area
    mov bx, TEXT_X
    mov cx, TEXT_Y
    movzx si, byte [cs:row_h]
    mov ax, [cs:vis_rows]
    mul si                              ; DX:AX = vis_rows * row_h (clobbers DX!)
    mov si, ax                          ; SI = height
    mov dx, TEXT_W                      ; DX = width (set AFTER mul, which clobbers DX)
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Compute cursor line/col
    call cursor_to_line_col

    ; Find byte offset for scroll_row
    mov cx, [cs:scroll_row]
    call find_line_start
    ; BX = offset of first visible line in text_buf

    mov word [cs:draw_row], 0

.row_loop:
    mov ax, [cs:draw_row]
    cmp ax, [cs:vis_rows]
    jae .rows_done
    cmp bx, [cs:text_len]
    ja .rows_done

    ; Copy line to line_buf (up to vis_cols chars, stop at newline/EOF)
    mov si, bx                          ; SI = source offset in text_buf
    xor di, di                          ; DI = char count
.copy_char:
    cmp di, [cs:vis_cols]
    jae .line_end
    cmp si, [cs:text_len]
    jae .line_end
    mov al, [cs:text_buf + si]
    cmp al, 0x0A
    je .line_end
    mov [cs:line_buf + di], al
    inc si
    inc di
    jmp .copy_char
.line_end:
    mov byte [cs:line_buf + di], 0      ; Null-terminate

    ; Calculate Y for this row
    push bx                             ; Save text offset
    mov ax, [cs:draw_row]
    movzx dx, byte [cs:row_h]
    mul dx
    add ax, TEXT_Y
    mov cx, ax                          ; CX = Y

    ; Draw line text
    mov bx, TEXT_X
    mov si, line_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Check if cursor is on this line
    mov ax, [cs:draw_row]
    add ax, [cs:scroll_row]
    cmp ax, [cs:cursor_line]
    jne .no_cursor

    ; Draw cursor (inverted char at cursor_col)
    mov ax, [cs:cursor_col]
    cmp ax, [cs:vis_cols]
    jae .no_cursor                      ; Cursor past visible area
    movzx dx, byte [cs:font_adv]
    mul dx
    add ax, TEXT_X
    mov bx, ax                          ; BX = cursor X

    ; Recalculate Y
    mov ax, [cs:draw_row]
    movzx dx, byte [cs:row_h]
    mul dx
    add ax, TEXT_Y
    mov cx, ax                          ; CX = cursor Y

    ; Get char at cursor position (or space at end/newline)
    push bx
    mov bx, [cs:cursor_pos]
    cmp bx, [cs:text_len]
    jae .cursor_space
    mov al, [cs:text_buf + bx]
    cmp al, 0x0A
    je .cursor_space
    cmp al, 32
    jb .cursor_space
    mov [cs:cursor_char_buf], al
    jmp .cursor_got
.cursor_space:
    mov byte [cs:cursor_char_buf], ' '
.cursor_got:
    mov byte [cs:cursor_char_buf + 1], 0
    pop bx
    mov si, cursor_char_buf
    mov ah, API_GFX_DRAW_STRING_INV
    int 0x80

.no_cursor:
    pop bx                              ; Restore text offset

    ; Advance past this line in text_buf
.skip_line:
    cmp bx, [cs:text_len]
    jae .advance_row
    cmp byte [cs:text_buf + bx], 0x0A
    je .found_nl
    inc bx
    jmp .skip_line
.found_nl:
    inc bx                              ; Past the newline
.advance_row:
    inc word [cs:draw_row]
    jmp .row_loop

.rows_done:
    popa
    ret

; ============================================================================
; draw_status - Draw status bar or dialog
; ============================================================================
draw_status:
    pusha
    ; Clear status area
    mov bx, 0
    mov cx, STATUS_Y - 1
    mov dx, CONTENT_W
    mov si, CONTENT_H - STATUS_Y + 2
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    cmp byte [cs:mode], MODE_EDIT
    je .status_normal
    ; Dialog mode
    jmp .status_dialog

.status_normal:
    ; "Ln X Col Y" on the left
    call cursor_to_line_col

    mov bx, 4
    mov cx, STATUS_Y
    mov si, str_ln
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Line number (1-based)
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

    ; "Col"
    mov bx, 60
    mov cx, STATUS_Y
    mov si, str_col
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Column number (1-based)
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
    ; Append " B"
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
    mov byte [cs:status_msg], 0         ; Clear after showing once
    jmp .status_done

.status_dialog:
    ; Show filename input prompt
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

    ; Text field for filename
    mov bx, 70
    mov cx, STATUS_Y
    mov dx, 140
    mov si, input_buf
    movzx di, byte [cs:input_len]
    mov al, 1                          ; Focused
    mov ah, API_DRAW_TEXTFIELD
    int 0x80

    ; [OK] button
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
; buf_insert_char - Insert character DL at cursor_pos
; ============================================================================
buf_insert_char:
    pusha
    mov ax, [cs:text_len]
    cmp ax, TEXT_MAX
    jae .bi_done

    ; Shift text_buf[cursor_pos..text_len-1] right by 1 byte
    mov cx, ax
    sub cx, [cs:cursor_pos]             ; CX = bytes to shift
    jcxz .bi_no_shift

    ; Use backward copy: src = text_buf+text_len-1, dst = text_buf+text_len
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
    inc di                              ; DI = &text_buf[text_len]
    std
    rep movsb
    cld
    pop es
    pop ds

.bi_no_shift:
    ; Store character
    mov bx, [cs:cursor_pos]
    mov [cs:text_buf + bx], dl
    inc word [cs:text_len]
    inc word [cs:cursor_pos]

.bi_done:
    popa
    ret

; ============================================================================
; buf_delete_char - Delete character before cursor (backspace)
; ============================================================================
buf_delete_char:
    pusha
    cmp word [cs:cursor_pos], 0
    je .bd_done

    ; Shift text_buf[cursor_pos..text_len-1] left by 1 byte
    mov cx, [cs:text_len]
    sub cx, [cs:cursor_pos]             ; CX = bytes to shift

    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, text_buf
    add si, [cs:cursor_pos]             ; SI = &text_buf[cursor_pos]
    mov di, si
    dec di                              ; DI = &text_buf[cursor_pos-1]
    cld
    rep movsb
    pop es
    pop ds

    dec word [cs:text_len]
    dec word [cs:cursor_pos]

.bd_done:
    popa
    ret

; ============================================================================
; buf_delete_fwd - Delete character at cursor (delete key)
; ============================================================================
buf_delete_fwd:
    pusha
    mov ax, [cs:cursor_pos]
    cmp ax, [cs:text_len]
    jae .bf_done

    ; Shift text_buf[cursor_pos+1..text_len-1] left by 1 byte
    mov cx, [cs:text_len]
    dec cx
    sub cx, [cs:cursor_pos]             ; CX = bytes to shift
    jcxz .bf_no_shift

    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov si, text_buf
    add si, [cs:cursor_pos]
    inc si                              ; SI = &text_buf[cursor_pos+1]
    mov di, si
    dec di                              ; DI = &text_buf[cursor_pos]
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
; cursor_to_line_col - Compute cursor_line and cursor_col from cursor_pos
; ============================================================================
cursor_to_line_col:
    pusha
    xor cx, cx                          ; CX = line count
    xor dx, dx                          ; DX = col count
    xor bx, bx                          ; BX = index
.ctl_scan:
    cmp bx, [cs:cursor_pos]
    jae .ctl_done
    cmp byte [cs:text_buf + bx], 0x0A
    jne .ctl_not_nl
    inc cx                              ; New line
    xor dx, dx                          ; Reset column
    jmp .ctl_next
.ctl_not_nl:
    inc dx                              ; Advance column
.ctl_next:
    inc bx
    jmp .ctl_scan
.ctl_done:
    mov [cs:cursor_line], cx
    mov [cs:cursor_col], dx
    popa
    ret

; ============================================================================
; find_line_start - Find byte offset of line N
; Input: CX = target line (0-based)
; Output: BX = byte offset (start of that line, or text_len if past end)
; ============================================================================
find_line_start:
    push cx
    push ax
    xor bx, bx
    test cx, cx
    jz .fls_done                        ; Line 0 starts at 0
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
    inc bx                              ; Start past the newline
.fls_done:
    pop ax
    pop cx
    ret

; ============================================================================
; find_line_end - Find end of line starting at BX
; Input: BX = line start offset
; Output: BX = offset of newline or text_len
; ============================================================================
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

; ============================================================================
; cursor_up - Move cursor up one line
; ============================================================================
cursor_up:
    pusha
    call cursor_to_line_col
    cmp word [cs:cursor_line], 0
    je .cu_done

    ; Find start of previous line
    mov cx, [cs:cursor_line]
    dec cx
    call find_line_start                ; BX = start of previous line
    push bx                             ; Save prev line start
    call find_line_end                  ; BX = end of previous line
    pop ax                              ; AX = prev line start
    sub bx, ax                          ; BX = prev line length

    ; Position cursor at min(cursor_col, prev_line_len)
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

; ============================================================================
; cursor_down - Move cursor down one line
; ============================================================================
cursor_down:
    pusha
    call cursor_to_line_col

    ; Find start of next line
    mov cx, [cs:cursor_line]
    inc cx
    call find_line_start                ; BX = start of next line
    cmp bx, [cs:text_len]
    ja .cd_done

    push bx                             ; Save next line start
    call find_line_end                  ; BX = end of next line
    pop ax                              ; AX = next line start
    sub bx, ax                          ; BX = next line length

    ; Position cursor at min(cursor_col, next_line_len)
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

; ============================================================================
; cursor_home - Move cursor to start of current line
; ============================================================================
cursor_home:
    pusha
    call cursor_to_line_col
    mov cx, [cs:cursor_line]
    call find_line_start
    mov [cs:cursor_pos], bx
    popa
    ret

; ============================================================================
; cursor_end - Move cursor to end of current line
; ============================================================================
cursor_end:
    pusha
    call cursor_to_line_col
    mov cx, [cs:cursor_line]
    call find_line_start
    call find_line_end
    mov [cs:cursor_pos], bx
    popa
    ret

; ============================================================================
; ensure_cursor_visible - Adjust scroll_row so cursor line is visible
; ============================================================================
ensure_cursor_visible:
    pusha
    mov ax, [cs:cursor_line]

    ; If cursor above visible area, scroll up
    cmp ax, [cs:scroll_row]
    jae .ecv_check_below
    mov [cs:scroll_row], ax
    jmp .ecv_done

.ecv_check_below:
    ; If cursor below visible area, scroll down
    mov bx, [cs:scroll_row]
    add bx, [cs:vis_rows]
    cmp ax, bx
    jb .ecv_done
    ; scroll_row = cursor_line - vis_rows + 1
    mov bx, ax
    sub bx, [cs:vis_rows]
    inc bx
    mov [cs:scroll_row], bx

.ecv_done:
    popa
    ret

; ============================================================================
; do_open_file - Open file from filename_buf into text_buf
; ============================================================================
do_open_file:
    pusha
    ; Open file
    mov si, filename_buf
    movzx bx, byte [cs:mount_handle]
    mov ah, API_FS_OPEN
    int 0x80
    jc .of_fail
    mov [cs:file_handle], al

    ; Read contents
    push cs
    pop es
    mov di, text_buf
    mov cx, TEXT_MAX
    mov al, [cs:file_handle]
    mov ah, API_FS_READ
    int 0x80
    jc .of_close_fail
    mov [cs:text_len], ax

    ; Close file
    mov al, [cs:file_handle]
    mov ah, API_FS_CLOSE
    int 0x80

    ; Strip CR bytes (0x0D) for clean LF-only
    call strip_cr

    ; Reset cursor
    mov word [cs:cursor_pos], 0
    mov word [cs:scroll_row], 0
    mov word [cs:cursor_line], 0
    mov word [cs:cursor_col], 0

    ; Set status
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

; ============================================================================
; do_save_file - Save text_buf to filename_buf
; ============================================================================
do_save_file:
    pusha
    cmp byte [cs:filename_buf], 0
    je .sf_fail

    ; Delete existing file (ignore error if not found)
    mov si, filename_buf
    mov bl, [cs:mount_handle]
    mov ah, API_FS_DELETE
    int 0x80

    ; Create new file
    mov si, filename_buf
    mov bl, [cs:mount_handle]
    mov ah, API_FS_CREATE
    int 0x80
    jc .sf_fail
    mov [cs:file_handle], al

    ; Write contents
    mov ax, cs
    mov es, ax
    mov bx, text_buf
    mov cx, [cs:text_len]
    mov al, [cs:file_handle]
    mov ah, API_FS_WRITE
    int 0x80

    ; Close
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

; ============================================================================
; do_new_file - Clear buffer for new file
; ============================================================================
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
    popa
    ret

; ============================================================================
; strip_cr - Remove all 0x0D (CR) bytes from text_buf
; ============================================================================
strip_cr:
    pusha
    xor si, si                          ; SI = read index
    xor di, di                          ; DI = write index
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

; ============================================================================
; copy_str - Copy CS:SI to CS:DI (null-terminated)
; ============================================================================
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

; ============================================================================
; word_to_decimal - Convert AX to decimal string at CS:DI
; Output: DI points past last digit
; ============================================================================
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
needs_redraw:   db 0                    ; 0=none, 1=line-only, 2=full redraw

; Font metrics (computed at startup)
font_adv:       db 6                    ; Character advance in pixels
row_h:          db 7                    ; Row height in pixels
vis_cols:       dw 52                   ; Visible columns
vis_rows:       dw 22                   ; Visible rows

; Cursor state
cursor_pos:     dw 0                    ; Byte offset in text_buf
cursor_line:    dw 0                    ; Current line (0-based)
cursor_col:     dw 0                    ; Current column (0-based)
scroll_row:     dw 0                    ; First visible line

; Text buffer state
text_len:       dw 0                    ; Current text length in bytes

; Drawing scratch
draw_row:       dw 0                    ; Current row being drawn

; Input state (for dialogs)
input_len:      db 0

; Strings
str_open:       db 'Open', 0
str_save:       db 'Save', 0
str_new:        db 'New', 0
str_ok:         db 'OK', 0
str_untitled:   db '(untitled)', 0
str_ln:         db 'Ln', 0
str_col:        db 'Col', 0
str_open_file:  db 'Open:', 0
str_save_as:    db 'Save as:', 0
str_opened:     db 'Opened', 0
str_saved:      db 'Saved', 0
str_err_open:   db 'Open error', 0
str_err_save:   db 'Save error', 0
test_char:      db 'A', 0

; Buffers
status_msg:     times 20 db 0
input_buf:      times 14 db 0           ; 12 chars + null + pad
filename_buf:   times 14 db 0
num_buf:        times 8 db 0
cursor_char_buf: db ' ', 0
line_buf:       times 80 db 0           ; Render scratch for one line

; Text buffer (16KB)
text_buf:       times TEXT_MAX db 0
