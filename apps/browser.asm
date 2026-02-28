; BROWSER.BIN - File Manager for UnoDOS
; Clean rewrite using current API conventions.
; Features: file listing, scrolling, delete, rename, copy.

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Files', 0                   ; App name
    times (0x04 + 12) - ($ - $$) db 0

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format) - folder icon
    db 0x0F, 0xFC, 0x00, 0x00      ; Row 0
    db 0x3F, 0xFF, 0x00, 0x00      ; Row 1
    db 0x55, 0x55, 0x55, 0x54      ; Row 2
    db 0x55, 0x55, 0x55, 0x54      ; Row 3
    db 0x40, 0x00, 0x00, 0x04      ; Row 4
    db 0x40, 0x00, 0x00, 0x04      ; Row 5
    db 0x40, 0x00, 0x00, 0x04      ; Row 6
    db 0x40, 0x00, 0x00, 0x04      ; Row 7
    db 0x40, 0x00, 0x00, 0x04      ; Row 8
    db 0x40, 0x00, 0x00, 0x04      ; Row 9
    db 0x40, 0x00, 0x00, 0x04      ; Row 10
    db 0x40, 0x00, 0x00, 0x04      ; Row 11
    db 0x55, 0x55, 0x55, 0x54      ; Row 12
    db 0x55, 0x55, 0x55, 0x54      ; Row 13
    db 0x00, 0x00, 0x00, 0x00      ; Row 14
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

    times 0x50 - ($ - $$) db 0

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_FS_MOUNT            equ 13
API_FS_OPEN             equ 14
API_FS_READ             equ 15
API_FS_CLOSE            equ 16
API_WIN_CREATE          equ 20
API_FS_READDIR          equ 27
API_MOUSE_STATE         equ 28
API_WIN_BEGIN_DRAW      equ 31
API_APP_YIELD           equ 34
API_GET_BOOT_DRIVE      equ 43
API_FS_CREATE           equ 45
API_FS_WRITE            equ 46
API_FS_DELETE           equ 47
API_DRAW_BUTTON         equ 51
API_HIT_TEST            equ 53
API_DRAW_SCROLLBAR      equ 58
API_DRAW_LISTITEM       equ 59
API_DRAW_HLINE          equ 69
API_FS_RENAME           equ 77
API_WORD_TO_STRING      equ 91
API_GET_FONT_INFO       equ 93
API_WIN_GET_CONTENT_SIZE equ 97

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Modes
MODE_NORMAL             equ 0
MODE_CONFIRM_DEL        equ 1
MODE_RENAME             equ 2
MODE_COPY               equ 3

; Constants
SCROLLBAR_W             equ 8
MAX_FILES               equ 64
FILE_ENTRY_SIZE         equ 16

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
    jc .exit
    mov [mount_handle], bl

    ; Get font metrics for layout
    mov ah, API_GET_FONT_INFO
    int 0x80
    mov [font_h], bh
    mov [font_adv], cl

    ; Compute window size from font
    ; Width = 22 * advance + SCROLLBAR_W + 12
    movzx ax, cl
    mov dx, 22
    mul dx
    add ax, SCROLLBAR_W
    add ax, 12
    mov [win_w], ax

    ; row_h = font_h + 2
    movzx ax, bh
    add ax, 2
    mov [row_h], ax

    ; btn_h = font_h + 3
    movzx ax, bh
    add ax, 3
    mov [btn_h], ax

    ; Window height = 14 (titlebar) + 2 (border) + header row + sep + 10*row_h + sep + 2*btn_h + pad
    ; Approximate: compute from rows
    mov ax, [row_h]
    mov dx, 10                      ; Target 10 visible rows
    mul dx
    add ax, 16                      ; Header + seps
    mov dx, [btn_h]
    shl dx, 1
    add dx, 6                       ; Two button rows + padding
    add ax, dx
    add ax, 16                      ; Titlebar + border overhead
    mov [win_h], ax

    ; Create window
    mov bx, 28
    mov cx, 10
    mov dx, [win_w]
    mov si, [win_h]
    mov ax, cs
    mov es, ax
    mov di, window_title
    mov al, 0x03                    ; Bordered + closeable
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit
    mov [win_handle], al

    ; Begin draw context
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Get content size and compute layout
    call compute_layout

    ; Scan files and draw
    call scan_files
    call draw_ui

; ============================================================================
; Main Loop
; ============================================================================
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; Check events first
    mov ah, API_EVENT_GET
    int 0x80
    jc .check_mouse

    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call compute_layout
    call draw_ui
    jmp .main_loop

.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    ; DL=ASCII, DH=scan
    cmp dl, 27                      ; ESC
    je .key_esc
    cmp byte [mode], MODE_NORMAL
    je .key_normal
    cmp byte [mode], MODE_CONFIRM_DEL
    je .key_confirm
    jmp .key_input

.check_mouse:
    mov ah, API_MOUSE_STATE
    int 0x80
    test dl, 1
    jz .mouse_up
    cmp byte [prev_btn], 0
    jne .main_loop
    mov byte [prev_btn], 1
    ; Click dispatch by mode
    cmp byte [mode], MODE_CONFIRM_DEL
    je .click_confirm
    cmp byte [mode], MODE_RENAME
    je .click_input
    cmp byte [mode], MODE_COPY
    je .click_input
    jmp .click_normal

.mouse_up:
    mov byte [prev_btn], 0
    jmp .main_loop

; --- ESC ---
.key_esc:
    cmp byte [mode], MODE_NORMAL
    je .exit
    mov byte [mode], MODE_NORMAL
    call draw_bottom
    jmp .main_loop

; --- Normal keys ---
.key_normal:
    cmp dl, 128                     ; Up arrow
    je .key_up
    cmp dl, 129                     ; Down arrow
    je .key_down
    jmp .main_loop

.key_up:
    cmp byte [sel_index], 0
    je .main_loop
    dec byte [sel_index]
    mov al, [sel_index]
    cmp al, [scroll_top]
    jae .redraw_list
    dec byte [scroll_top]
.redraw_list:
    call draw_file_list
    jmp .main_loop

.key_down:
    mov al, [sel_index]
    inc al
    cmp al, [file_count]
    jae .main_loop
    mov [sel_index], al
    sub al, [scroll_top]
    cmp al, byte [vis_rows]
    jb .redraw_list
    inc byte [scroll_top]
    jmp .redraw_list

; --- Normal click ---
.click_normal:
    ; Test file rows
    xor cl, cl
.test_row:
    mov al, cl
    add al, [scroll_top]
    cmp al, [file_count]
    jae .test_scroll
    push cx
    ; Row rect: X=2, Y=list_y + CL*row_h, W=list_w, H=row_h
    movzx ax, cl
    mul word [row_h]
    add ax, [list_y]
    mov cx, ax
    mov bx, 2
    mov dx, [list_w]
    mov si, [row_h]
    mov ah, API_HIT_TEST
    int 0x80
    pop cx
    test al, al
    jnz .row_hit
    inc cl
    cmp cl, byte [vis_rows]
    jb .test_row

.test_scroll:
    ; Scroll up arrow
    mov bx, [list_w]
    add bx, 2
    mov cx, [list_y]
    mov dx, SCROLLBAR_W
    mov si, 8
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .scroll_up

    ; Scroll down arrow
    mov bx, [list_w]
    add bx, 2
    mov ax, [vis_rows]
    mul word [row_h]
    add ax, [list_y]
    sub ax, 8
    mov cx, ax
    mov dx, SCROLLBAR_W
    mov si, 8
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .scroll_down

    ; Test buttons
    jmp .test_buttons

.row_hit:
    mov al, cl
    add al, [scroll_top]
    mov [sel_index], al
    call draw_file_list
    jmp .main_loop

.scroll_up:
    cmp byte [scroll_top], 0
    je .main_loop
    dec byte [scroll_top]
    mov al, [scroll_top]
    cmp al, [sel_index]
    jbe .scroll_redraw
    mov [sel_index], al
.scroll_redraw:
    call draw_file_list
    jmp .main_loop

.scroll_down:
    movzx ax, byte [file_count]
    sub ax, [vis_rows]
    jle .main_loop
    cmp byte [scroll_top], al
    jae .main_loop
    inc byte [scroll_top]
    movzx ax, byte [scroll_top]
    add ax, [vis_rows]
    dec ax
    cmp al, [sel_index]
    jae .scroll_redraw
    mov [sel_index], al
    jmp .scroll_redraw

.test_buttons:
    cmp byte [file_count], 0
    je .main_loop
    ; Delete
    mov bx, 4
    mov cx, [row1_y]
    mov dx, 52
    mov si, [btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_delete
    ; Rename
    mov bx, 62
    mov cx, [row1_y]
    mov dx, 56
    mov si, [btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_rename
    ; Copy
    mov bx, 124
    mov cx, [row1_y]
    mov dx, 44
    mov si, [btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_copy
    jmp .main_loop

; --- Delete ---
.start_delete:
    mov byte [mode], MODE_CONFIRM_DEL
    call draw_bottom
    jmp .main_loop

; --- Rename ---
.start_rename:
    mov byte [mode], MODE_RENAME
    call get_sel_name
    xor cx, cx
    mov di, input_buf
.sr_copy:
    mov al, [si]
    test al, al
    jz .sr_done
    mov [di], al
    inc si
    inc di
    inc cl
    cmp cl, 12
    jb .sr_copy
.sr_done:
    mov byte [di], 0
    mov [input_len], cl
    call draw_bottom
    jmp .main_loop

; --- Copy ---
.start_copy:
    mov byte [mode], MODE_COPY
    mov byte [input_buf], 0
    mov byte [input_len], 0
    call draw_bottom
    jmp .main_loop

; --- Confirm click ---
.click_confirm:
    mov bx, 4
    mov cx, [row2_y]
    mov dx, 30
    mov si, [btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .do_delete
    mov bx, 40
    mov cx, [row2_y]
    mov dx, 26
    mov si, [btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .cancel_mode
    jmp .main_loop

; --- Confirm keys ---
.key_confirm:
    cmp dl, 'Y'
    je .do_delete
    cmp dl, 'y'
    je .do_delete
    cmp dl, 'N'
    je .cancel_mode
    cmp dl, 'n'
    je .cancel_mode
    jmp .main_loop

.cancel_mode:
    mov byte [mode], MODE_NORMAL
    call draw_bottom
    jmp .main_loop

; --- Input click ---
.click_input:
    mov bx, 200
    mov cx, [row2_y]
    mov dx, 30
    mov si, [btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .submit_input
    jmp .main_loop

; --- Input keys ---
.key_input:
    cmp dl, 8                       ; Backspace
    je .input_bs
    cmp dl, 13                      ; Enter
    je .submit_input
    cmp dl, 32
    jb .main_loop
    cmp dl, 126
    ja .main_loop
    cmp byte [input_len], 12
    jae .main_loop
    ; Auto-uppercase
    cmp dl, 'a'
    jb .input_store
    cmp dl, 'z'
    ja .input_store
    sub dl, 32
.input_store:
    movzx bx, byte [input_len]
    mov [input_buf + bx], dl
    inc byte [input_len]
    movzx bx, byte [input_len]
    mov byte [input_buf + bx], 0
    call draw_bottom
    jmp .main_loop

.input_bs:
    cmp byte [input_len], 0
    je .main_loop
    dec byte [input_len]
    movzx bx, byte [input_len]
    mov byte [input_buf + bx], 0
    call draw_bottom
    jmp .main_loop

.submit_input:
    cmp byte [input_len], 0
    je .main_loop
    cmp byte [mode], MODE_RENAME
    je .do_rename
    cmp byte [mode], MODE_COPY
    je .do_copy
    jmp .main_loop

; ============================================================================
; File Operations
; ============================================================================
.do_delete:
    call get_sel_name
    mov bl, [mount_handle]
    mov ah, API_FS_DELETE
    int 0x80
    jc .op_fail
    call scan_files
    mov byte [mode], MODE_NORMAL
    call draw_ui
    jmp .main_loop

.do_rename:
    call get_sel_name
    mov ax, cs
    mov es, ax
    mov di, input_buf
    mov bl, [mount_handle]
    mov ah, API_FS_RENAME
    int 0x80
    jc .op_fail
    call scan_files
    mov byte [mode], MODE_NORMAL
    call draw_ui
    jmp .main_loop

.do_copy:
    call get_sel_name
    movzx bx, byte [mount_handle]
    mov ah, API_FS_OPEN
    int 0x80
    jc .op_fail
    mov [src_handle], al

    mov si, input_buf
    mov bl, [mount_handle]
    mov ah, API_FS_CREATE
    int 0x80
    jc .copy_close_src
    mov [dst_handle], al

.copy_loop:
    mov al, [src_handle]
    mov ah, API_FS_READ
    push cs
    pop es
    mov di, copy_buf
    mov cx, 512
    int 0x80
    jc .copy_done
    test ax, ax
    jz .copy_done
    mov cx, ax
    push cx
    mov al, [dst_handle]
    mov ah, API_FS_WRITE
    push cs
    pop es
    mov bx, copy_buf
    int 0x80
    pop cx
    jc .copy_done
    cmp cx, 512
    jb .copy_done
    jmp .copy_loop

.copy_done:
    mov al, [dst_handle]
    mov ah, API_FS_CLOSE
    int 0x80
.copy_close_src:
    mov al, [src_handle]
    mov ah, API_FS_CLOSE
    int 0x80
    call scan_files
    mov byte [mode], MODE_NORMAL
    call draw_ui
    jmp .main_loop

.op_fail:
    mov byte [mode], MODE_NORMAL
    mov byte [op_error], 1
    call draw_ui
    jmp .main_loop

; ============================================================================
; Exit - let app_exit_stub handle all cleanup
; ============================================================================
.exit:
    pop es
    pop ds
    popa
    retf

; ============================================================================
; compute_layout - Get content size and derive all Y positions
; ============================================================================
compute_layout:
    pusha
    mov al, 0xFF
    mov ah, API_WIN_GET_CONTENT_SIZE
    int 0x80
    jc .cl_done
    test dx, dx
    jz .cl_done
    mov [content_w], dx
    mov [content_h], si

    ; list_w = content_w - 4 - SCROLLBAR_W
    mov ax, dx
    sub ax, 4
    sub ax, SCROLLBAR_W
    mov [list_w], ax

    ; sep1_y = row_h (header height)
    mov ax, [row_h]
    mov [sep1_y], ax

    ; list_y = sep1_y + 3
    add ax, 3
    mov [list_y], ax

    ; vis_rows = (content_h - list_y - 4 - 2*btn_h) / row_h
    mov ax, [content_h]
    sub ax, [list_y]
    sub ax, 4
    mov dx, [btn_h]
    shl dx, 1
    sub ax, dx
    xor dx, dx
    div word [row_h]
    mov [vis_rows], ax

    ; sep2_y = list_y + vis_rows * row_h
    mul word [row_h]
    add ax, [list_y]
    mov [sep2_y], ax

    ; row1_y = sep2_y + 3
    add ax, 3
    mov [row1_y], ax

    ; row2_y = row1_y + btn_h + 1
    add ax, [btn_h]
    inc ax
    mov [row2_y], ax

.cl_done:
    popa
    ret

; ============================================================================
; scan_files - Read directory into file_table
; ============================================================================
scan_files:
    pusha
    push es
    mov byte [file_count], 0
    mov byte [sel_index], 0
    mov byte [scroll_top], 0
    mov word [dir_state], 0

.sf_loop:
    cmp byte [file_count], MAX_FILES
    jae .sf_done
    mov al, [mount_handle]
    mov cx, [dir_state]
    push cs
    pop es
    mov di, dir_buf
    mov ah, API_FS_READDIR
    int 0x80
    jc .sf_done
    mov [dir_state], cx

    ; Skip volume labels
    test byte [dir_buf + 11], 0x08
    jnz .sf_loop

    ; Convert FAT 8.3 to dot format in file_table
    movzx bx, byte [file_count]
    shl bx, 4
    add bx, file_table

    mov si, dir_buf
    mov di, bx
    mov cx, 8
.sf_name:
    mov al, [si]
    cmp al, ' '
    je .sf_dot
    mov [di], al
    inc si
    inc di
    loop .sf_name
    jmp .sf_dot
.sf_dot:
    mov al, [dir_buf + 8]
    cmp al, ' '
    je .sf_noext
    mov byte [di], '.'
    inc di
    mov si, dir_buf
    add si, 8
    mov cx, 3
.sf_ext:
    mov al, [si]
    cmp al, ' '
    je .sf_noext
    mov [di], al
    inc si
    inc di
    loop .sf_ext
.sf_noext:
    mov byte [di], 0

    ; Store file size (16-bit)
    mov ax, [dir_buf + 28]
    mov [bx + 13], ax

    inc byte [file_count]
    jmp .sf_loop

.sf_done:
    pop es
    popa
    ret

; ============================================================================
; draw_ui - Full redraw
; ============================================================================
draw_ui:
    pusha
    ; Clear content
    mov bx, 0
    mov cx, 0
    mov dx, [content_w]
    mov si, [content_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Header
    mov bx, 6
    mov cx, 1
    mov si, str_name
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 190
    mov cx, 1
    mov si, str_size
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Separator lines
    mov bx, 2
    mov cx, [sep1_y]
    mov dx, [content_w]
    sub dx, 4
    mov al, 3
    mov ah, API_DRAW_HLINE
    int 0x80
    mov bx, 2
    mov cx, [sep2_y]
    mov dx, [content_w]
    sub dx, 4
    mov al, 3
    mov ah, API_DRAW_HLINE
    int 0x80

    call draw_file_list
    call draw_bottom
    mov byte [op_error], 0
    popa
    ret

; ============================================================================
; draw_file_list - Visible rows + scrollbar
; ============================================================================
draw_file_list:
    pusha
    xor cl, cl
.row:
    mov al, cl
    add al, [scroll_top]
    cmp al, [file_count]
    jae .clear_rest

    push cx
    movzx bx, al
    call format_row

    ; Selected?
    xor al, al
    mov ah, cl
    add ah, [scroll_top]
    cmp ah, [sel_index]
    jne .not_sel
    mov al, 1
.not_sel:
    push ax
    movzx ax, cl
    mul word [row_h]
    add ax, [list_y]
    mov cx, ax
    pop ax
    mov bx, 2
    mov dx, [list_w]
    mov si, display_buf
    mov ah, API_DRAW_LISTITEM
    int 0x80
    pop cx
    inc cl
    cmp cl, byte [vis_rows]
    jb .row
    jmp .scrollbar

.clear_rest:
    push cx
    mov ax, [vis_rows]
    xor ch, ch
    sub al, cl
    jbe .skip_clear
    mul word [row_h]
    mov si, ax
    movzx ax, cl
    mul word [row_h]
    add ax, [list_y]
    mov cx, ax
    mov bx, 2
    mov dx, [list_w]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
.skip_clear:
    pop cx

.scrollbar:
    mov bx, [list_w]
    add bx, 2
    mov cx, [list_y]
    mov ax, [vis_rows]
    mul word [row_h]
    mov si, ax
    movzx dx, byte [scroll_top]
    movzx ax, byte [file_count]
    sub ax, [vis_rows]
    jns .sb_ok
    xor ax, ax
.sb_ok:
    mov di, ax
    mov al, 0
    mov ah, API_DRAW_SCROLLBAR
    int 0x80
    popa
    ret

; ============================================================================
; draw_bottom - Button bar / status
; ============================================================================
draw_bottom:
    pusha
    ; Clear bottom area
    mov bx, 0
    mov cx, [row1_y]
    dec cx
    mov dx, [content_w]
    mov si, [content_h]
    add si, 2
    sub si, [row1_y]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    cmp byte [mode], MODE_CONFIRM_DEL
    je .db_confirm
    cmp byte [mode], MODE_RENAME
    je .db_rename
    cmp byte [mode], MODE_COPY
    je .db_copy

    ; Normal mode buttons
    mov ax, cs
    mov es, ax
    mov bx, 4
    mov cx, [row1_y]
    mov dx, 52
    mov si, [btn_h]
    mov di, str_delete
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    mov bx, 62
    mov cx, [row1_y]
    mov dx, 56
    mov si, [btn_h]
    mov di, str_rename
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    mov bx, 124
    mov cx, [row1_y]
    mov dx, 44
    mov si, [btn_h]
    mov di, str_copy
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; File count
    call draw_file_count

    ; Status
    cmp byte [op_error], 1
    je .db_err
    mov bx, 4
    mov cx, [row2_y]
    mov si, str_ready
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .db_done

.db_err:
    mov bx, 4
    mov cx, [row2_y]
    mov si, str_error
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .db_done

.db_confirm:
    mov bx, 4
    mov cx, [row1_y]
    mov si, str_del_pfx
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    call get_sel_name
    movzx bx, byte [font_adv]
    shl bx, 2
    add bx, 4
    push bx
    mov cx, [row1_y]
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    movzx ax, byte [sel_name_len]
    movzx bx, byte [font_adv]
    mul bx
    pop bx
    add bx, ax
    mov cx, [row1_y]
    mov si, str_question
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Yes/No buttons
    mov ax, cs
    mov es, ax
    mov bx, 4
    mov cx, [row2_y]
    mov dx, 30
    mov si, [btn_h]
    mov di, str_yes
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    mov bx, 40
    mov cx, [row2_y]
    mov dx, 26
    mov si, [btn_h]
    mov di, str_no
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    jmp .db_done

.db_rename:
    mov bx, 4
    mov cx, [row1_y]
    mov si, str_newname
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    call draw_input_line
    jmp .db_done

.db_copy:
    mov bx, 4
    mov cx, [row1_y]
    mov si, str_copyto
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    call draw_input_line

.db_done:
    popa
    ret

; ============================================================================
; draw_input_line - Text input + cursor + OK button
; ============================================================================
draw_input_line:
    pusha
    mov bx, 4
    mov cx, [row2_y]
    mov si, input_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Cursor
    movzx ax, byte [input_len]
    movzx bx, byte [font_adv]
    mul bx
    add ax, 4
    mov bx, ax
    mov cx, [row2_y]
    mov si, str_cursor
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; OK button
    mov ax, cs
    mov es, ax
    mov bx, 200
    mov cx, [row2_y]
    mov dx, 30
    mov si, [btn_h]
    mov di, str_ok
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    popa
    ret

; ============================================================================
; draw_file_count - "N files" at right of row1
; ============================================================================
draw_file_count:
    pusha
    movzx dx, byte [file_count]
    mov di, count_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov byte [di], ' '
    inc di
    mov si, str_files
.dfc:
    mov al, [si]
    mov [di], al
    test al, al
    jz .dfc_draw
    inc si
    inc di
    jmp .dfc
.dfc_draw:
    mov bx, 200
    mov cx, [row1_y]
    add cx, 2
    mov si, count_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    popa
    ret

; ============================================================================
; format_row - Format file entry BX into display_buf
; ============================================================================
format_row:
    pusha
    shl bx, 4
    add bx, file_table
    mov di, display_buf
    mov si, bx
    mov cx, 13
.fr_name:
    mov al, [si]
    test al, al
    jz .fr_pad
    mov [di], al
    inc si
    inc di
    loop .fr_name
.fr_pad:
    mov ax, di
    sub ax, display_buf
.fr_padl:
    cmp ax, 22
    jae .fr_size
    mov byte [di], ' '
    inc di
    inc ax
    jmp .fr_padl
.fr_size:
    mov dx, [bx + 13]
    mov ah, API_WORD_TO_STRING
    int 0x80
    popa
    ret

; ============================================================================
; get_sel_name - SI = pointer to selected filename, sel_name_len set
; ============================================================================
get_sel_name:
    push ax
    push bx
    movzx bx, byte [sel_index]
    shl bx, 4
    add bx, file_table
    mov si, bx
    xor al, al
    mov bx, si
.gsn:
    cmp byte [bx], 0
    je .gsn_done
    inc al
    inc bx
    cmp al, 13
    jb .gsn
.gsn_done:
    mov [sel_name_len], al
    pop bx
    pop ax
    ret

; ============================================================================
; Data
; ============================================================================
window_title:   db 'File Manager', 0
win_handle:     db 0
mount_handle:   db 0
prev_btn:       db 0
mode:           db MODE_NORMAL
file_count:     db 0
sel_index:      db 0
scroll_top:     db 0
dir_state:      dw 0
op_error:       db 0
src_handle:     db 0
dst_handle:     db 0
input_len:      db 0
sel_name_len:   db 0

; Font metrics
font_h:         db 8
font_adv:       db 12

; Layout (computed dynamically)
win_w:          dw 264
win_h:          dw 170
row_h:          dw 10
btn_h:          dw 11
vis_rows:       dw 10
list_w:         dw 248
list_y:         dw 13
sep1_y:         dw 10
sep2_y:         dw 113
row1_y:         dw 116
row2_y:         dw 128
content_w:      dw 260
content_h:      dw 156

; Strings
str_name:       db 'Name', 0
str_size:       db 'Size', 0
str_delete:     db 'Delete', 0
str_rename:     db 'Rename', 0
str_copy:       db 'Copy', 0
str_yes:        db 'Yes', 0
str_no:         db 'No', 0
str_ok:         db 'OK', 0
str_ready:      db 'Ready', 0
str_error:      db 'Error!', 0
str_del_pfx:    db 'Del ', 0
str_question:   db '?', 0
str_newname:    db 'New name:', 0
str_copyto:     db 'Copy to:', 0
str_cursor:     db '_', 0
str_files:      db 'files', 0

; Buffers
input_buf:      times 14 db 0
display_buf:    times 32 db 0
count_buf:      times 12 db 0
dir_buf:        times 32 db 0

; File table
file_table:     times (MAX_FILES * FILE_ENTRY_SIZE) db 0

; Copy buffer
copy_buf:       times 512 db 0
