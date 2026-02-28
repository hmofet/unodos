; BROWSER.BIN - File Manager for UnoDOS
; Scrollable file list with selection, delete, rename, and copy operations.
; Build 257
;
; Build: nasm -f bin -o browser.bin browser.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Files', 0                   ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; Folder icon: cyan folder with white tab
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

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

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
API_WIN_DESTROY         equ 21
API_FS_READDIR          equ 27
API_MOUSE_STATE         equ 28
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_GET_BOOT_DRIVE      equ 43
API_FS_CREATE           equ 45
API_FS_WRITE            equ 46
API_FS_DELETE           equ 47
API_DRAW_BUTTON         equ 51
API_HIT_TEST            equ 53
API_DRAW_LISTITEM       equ 59
API_DRAW_SCROLLBAR      equ 58
API_DRAW_HLINE          equ 69
API_FS_RENAME           equ 77
API_WORD_TO_STRING      equ 91
API_WIN_GET_INFO        equ 79
API_WIN_GET_CONTENT_SIZE equ 97
API_GET_FONT_INFO       equ 93

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Modes
MODE_NORMAL             equ 0
MODE_CONFIRM_DEL        equ 1
MODE_RENAME             equ 2
MODE_COPY               equ 3

; Layout (content-relative)
WIN_W                   equ 264
WIN_H                   equ 170
SCROLLBAR_W             equ 8
SCROLLBAR_ARROW_H       equ 8
BTN_DEL_X               equ 4
BTN_DEL_W               equ 52
BTN_REN_X               equ 62
BTN_REN_W               equ 56
BTN_CPY_X               equ 124
BTN_CPY_W               equ 44
BTN_YES_X               equ 4
BTN_YES_W               equ 30
BTN_NO_X                equ 40
BTN_NO_W                equ 26
BTN_OK_X                equ 200
BTN_OK_W                equ 30

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

    ; Get boot drive and mount
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov ah, API_FS_MOUNT
    int 0x80
    jc .exit_fail
    mov [cs:mount_handle], bl

    ; Create window
    mov bx, 28
    mov cx, 12
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

    ; Get actual content dimensions via API 97
    mov al, 0xFF                    ; Current draw context
    mov ah, API_WIN_GET_CONTENT_SIZE
    int 0x80                        ; DX = content_w, SI = content_h
    mov [cs:content_w], dx
    mov [cs:content_h], si
    ; list_w = content_w - 4 - SCROLLBAR_W
    mov ax, dx
    sub ax, 4
    sub ax, SCROLLBAR_W
    mov [cs:list_w], ax

    ; Compute dynamic layout from current font
    mov ah, API_GET_FONT_INFO
    int 0x80                        ; BH=height, BL=width, CL=advance
    mov [cs:font_h], bh
    mov [cs:font_w], bl
    mov [cs:font_adv], cl

    ; row_h = font_h + 2
    movzx ax, bh
    add ax, 2
    mov [cs:row_h], ax

    ; sep1_y = font_h + 2
    mov [cs:sep1_y], ax

    ; list_y = sep1_y + 3
    add ax, 3
    mov [cs:list_y], ax

    ; btn_h = font_h + 3
    movzx ax, bh
    add ax, 3
    mov [cs:btn_h], ax

    ; vis_rows = (content_h - list_y - 4 - 2*btn_h) / row_h
    mov ax, [cs:content_h]
    sub ax, [cs:list_y]
    sub ax, 4
    mov dx, [cs:btn_h]
    shl dx, 1
    sub ax, dx
    xor dx, dx
    div word [cs:row_h]
    mov [cs:vis_rows], ax

    ; sep2_y = list_y + vis_rows * row_h
    mul word [cs:row_h]
    add ax, [cs:list_y]
    mov [cs:sep2_y], ax

    ; row1_y = sep2_y + 3
    add ax, 3
    mov [cs:row1_y], ax

    ; row2_y = row1_y + btn_h + 1
    add ax, [cs:btn_h]
    inc ax
    mov [cs:row2_y], ax

    ; Scan files and draw UI
    call scan_files
    call draw_ui

; ============================================================================
; Main Loop
; ============================================================================
.main_loop:
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

    ; Click detected - dispatch by mode
    cmp byte [cs:mode], MODE_CONFIRM_DEL
    je .click_confirm
    cmp byte [cs:mode], MODE_RENAME
    je .click_input
    cmp byte [cs:mode], MODE_COPY
    je .click_input
    ; MODE_NORMAL: test file rows, then buttons
    jmp .click_normal

.mouse_up:
    mov byte [cs:prev_btn], 0

.check_event:
    mov ah, API_EVENT_GET
    int 0x80
    jc .main_loop
    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call recompute_layout
    call draw_ui
    jmp .main_loop
.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    ; DL=ASCII, DH=scan code
    cmp dl, 27
    je .key_esc
    cmp byte [cs:mode], MODE_NORMAL
    je .key_normal
    cmp byte [cs:mode], MODE_CONFIRM_DEL
    je .key_confirm
    ; MODE_RENAME or MODE_COPY: text input
    jmp .key_input

; --- ESC key ---
.key_esc:
    cmp byte [cs:mode], MODE_NORMAL
    je .exit_ok
    ; Cancel current mode
    mov byte [cs:mode], MODE_NORMAL
    call draw_bottom
    jmp .main_loop

; --- Normal mode keys ---
.key_normal:
    cmp dl, 128                     ; Up arrow (special code)
    je .key_up
    cmp dl, 129                     ; Down arrow (special code)
    je .key_down
    jmp .main_loop

.key_up:
    cmp byte [cs:sel_index], 0
    je .main_loop
    dec byte [cs:sel_index]
    mov al, [cs:sel_index]
    cmp al, [cs:scroll_top]
    jae .redraw_list
    dec byte [cs:scroll_top]
.redraw_list:
    call draw_file_list
    jmp .main_loop

.key_down:
    mov al, [cs:sel_index]
    inc al
    cmp al, [cs:file_count]
    jae .main_loop
    mov [cs:sel_index], al
    sub al, [cs:scroll_top]
    cmp al, byte [cs:vis_rows]
    jb .redraw_list
    inc byte [cs:scroll_top]
    jmp .redraw_list

; --- Normal mode click ---
.click_normal:
    ; Hit test each visible row
    xor cl, cl                      ; CL = row counter
.test_row:
    mov al, cl
    add al, [cs:scroll_top]
    cmp al, [cs:file_count]
    jae .test_buttons
    ; Row rect: X=2, Y=list_y + CL*row_h, W=list_w, H=row_h
    push cx
    mov bx, 2
    xor ch, ch
    movzx ax, cl
    mul word [cs:row_h]
    add ax, [cs:list_y]
    mov cx, ax
    mov dx, [cs:list_w]
    mov si, [cs:row_h]
    mov ah, API_HIT_TEST
    int 0x80
    pop cx
    test al, al
    jnz .row_clicked
    inc cl
    cmp cl, byte [cs:vis_rows]
    jb .test_row

    ; Test scrollbar arrows
    mov bx, [cs:list_w]
    add bx, 2                      ; X = list_w + 2
    mov cx, [cs:list_y]            ; Y = list_y
    mov dx, SCROLLBAR_W
    mov si, SCROLLBAR_ARROW_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .scroll_up_click

    ; Down arrow
    mov bx, [cs:list_w]
    add bx, 2
    mov ax, [cs:vis_rows]
    mul word [cs:row_h]
    add ax, [cs:list_y]
    sub ax, SCROLLBAR_ARROW_H
    mov cx, ax
    mov dx, SCROLLBAR_W
    mov si, SCROLLBAR_ARROW_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .scroll_down_click

    jmp .test_buttons

.scroll_up_click:
    cmp byte [cs:scroll_top], 0
    je .check_event
    dec byte [cs:scroll_top]
    ; Keep sel_index in view
    mov al, [cs:scroll_top]
    cmp al, [cs:sel_index]
    jbe .scroll_redraw
    mov [cs:sel_index], al
.scroll_redraw:
    call draw_file_list
    jmp .check_event

.scroll_down_click:
    movzx ax, byte [cs:file_count]
    sub ax, [cs:vis_rows]
    jle .check_event                ; No scrolling needed
    cmp byte [cs:scroll_top], al
    jae .check_event                ; Already at bottom
    inc byte [cs:scroll_top]
    ; Keep sel_index in view
    movzx ax, byte [cs:scroll_top]
    add ax, [cs:vis_rows]
    dec ax                          ; Last visible index
    cmp al, [cs:sel_index]
    jae .scroll_redraw
    mov al, [cs:scroll_top]
    add al, byte [cs:vis_rows]
    dec al
    mov [cs:sel_index], al          ; Clamp to bottom of view
    jmp .scroll_redraw

.row_clicked:
    mov al, cl
    add al, [cs:scroll_top]
    mov [cs:sel_index], al
    call draw_file_list
    jmp .check_event

.test_buttons:
    cmp byte [cs:file_count], 0
    je .check_event

    ; Delete button
    mov bx, BTN_DEL_X
    mov cx, [cs:row1_y]
    mov dx, BTN_DEL_W
    mov si, [cs:btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_delete

    ; Rename button
    mov bx, BTN_REN_X
    mov cx, [cs:row1_y]
    mov dx, BTN_REN_W
    mov si, [cs:btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_rename

    ; Copy button
    mov bx, BTN_CPY_X
    mov cx, [cs:row1_y]
    mov dx, BTN_CPY_W
    mov si, [cs:btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .start_copy

    jmp .check_event

; --- Start Delete ---
.start_delete:
    mov byte [cs:mode], MODE_CONFIRM_DEL
    call draw_bottom
    jmp .check_event

; --- Start Rename ---
.start_rename:
    mov byte [cs:mode], MODE_RENAME
    ; Pre-fill input with selected filename
    call get_sel_name               ; SI = filename ptr
    xor cx, cx
    mov di, input_buf
.sr_copy:
    mov al, [cs:si]
    test al, al
    jz .sr_done
    mov [cs:di], al
    inc si
    inc di
    inc cl
    cmp cl, 12
    jb .sr_copy
.sr_done:
    mov byte [cs:di], 0
    mov [cs:input_len], cl
    call draw_bottom
    jmp .check_event

; --- Start Copy ---
.start_copy:
    mov byte [cs:mode], MODE_COPY
    mov byte [cs:input_buf], 0
    mov byte [cs:input_len], 0
    call draw_bottom
    jmp .check_event

; --- Confirm delete click ---
.click_confirm:
    mov bx, BTN_YES_X
    mov cx, [cs:row2_y]
    mov dx, BTN_YES_W
    mov si, [cs:btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .do_delete
    mov bx, BTN_NO_X
    mov cx, [cs:row2_y]
    mov dx, BTN_NO_W
    mov si, [cs:btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .cancel_mode
    jmp .check_event

; --- Confirm delete keys ---
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
    mov byte [cs:mode], MODE_NORMAL
    call draw_bottom
    jmp .main_loop

; --- Text input click (rename/copy) ---
.click_input:
    mov bx, BTN_OK_X
    mov cx, [cs:row2_y]
    mov dx, BTN_OK_W
    mov si, [cs:btn_h]
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .submit_input
    jmp .check_event

; --- Text input keys ---
.key_input:
    ; Backspace
    cmp dl, 8
    je .input_backspace
    ; Enter
    cmp dl, 13
    je .submit_input
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
    jb .input_store
    cmp dl, 'z'
    ja .input_store
    sub dl, 32
.input_store:
    movzx bx, byte [cs:input_len]
    mov [cs:input_buf + bx], dl
    inc byte [cs:input_len]
    movzx bx, byte [cs:input_len]
    mov byte [cs:input_buf + bx], 0
    call draw_bottom
    jmp .main_loop

.input_backspace:
    cmp byte [cs:input_len], 0
    je .main_loop
    dec byte [cs:input_len]
    movzx bx, byte [cs:input_len]
    mov byte [cs:input_buf + bx], 0
    call draw_bottom
    jmp .main_loop

; --- Submit input ---
.submit_input:
    cmp byte [cs:input_len], 0
    je .main_loop
    cmp byte [cs:mode], MODE_RENAME
    je .do_rename
    cmp byte [cs:mode], MODE_COPY
    je .do_copy
    jmp .main_loop

; ============================================================================
; Do Delete
; ============================================================================
.do_delete:
    call get_sel_name               ; SI = filename
    mov bl, [cs:mount_handle]
    mov ah, API_FS_DELETE
    int 0x80
    jc .op_fail
    ; Rescan and redraw
    call scan_files
    mov byte [cs:mode], MODE_NORMAL
    call draw_ui
    jmp .main_loop

; ============================================================================
; Do Rename
; ============================================================================
.do_rename:
    call get_sel_name               ; SI = old name
    mov ax, cs
    mov es, ax
    mov di, input_buf               ; ES:DI = new name
    mov bl, [cs:mount_handle]
    mov ah, API_FS_RENAME
    int 0x80
    jc .op_fail
    call scan_files
    mov byte [cs:mode], MODE_NORMAL
    call draw_ui
    jmp .main_loop

; ============================================================================
; Do Copy
; ============================================================================
.do_copy:
    ; Open source file
    call get_sel_name               ; SI = source filename
    movzx bx, byte [cs:mount_handle]
    mov ah, API_FS_OPEN
    int 0x80
    jc .op_fail
    mov [cs:src_handle], al

    ; Create destination file
    mov si, input_buf
    mov bl, [cs:mount_handle]
    mov ah, API_FS_CREATE
    int 0x80
    jc .copy_close_src
    mov [cs:dst_handle], al

    ; Stream copy loop
.copy_loop:
    mov al, [cs:src_handle]
    mov ah, API_FS_READ
    push cs
    pop es
    mov di, copy_buf
    mov cx, 512
    int 0x80
    jc .copy_close_both
    test ax, ax
    jz .copy_close_both             ; EOF
    ; Write what we read
    mov cx, ax                      ; CX = bytes to write
    push cx
    mov al, [cs:dst_handle]
    mov ah, API_FS_WRITE
    push cs
    pop es
    mov bx, copy_buf
    int 0x80
    pop cx
    jc .copy_close_both
    cmp cx, 512
    jb .copy_close_both             ; Last chunk (partial read = EOF)
    jmp .copy_loop

.copy_close_both:
    mov al, [cs:dst_handle]
    mov ah, API_FS_CLOSE
    int 0x80
.copy_close_src:
    mov al, [cs:src_handle]
    mov ah, API_FS_CLOSE
    int 0x80
    ; Rescan
    call scan_files
    mov byte [cs:mode], MODE_NORMAL
    call draw_ui
    jmp .main_loop

.op_fail:
    mov byte [cs:mode], MODE_NORMAL
    mov byte [cs:op_error], 1
    call draw_ui
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
; scan_files - Read directory into file_table
; ============================================================================
scan_files:
    pusha
    push es
    mov byte [cs:file_count], 0
    mov byte [cs:sel_index], 0
    mov byte [cs:scroll_top], 0
    mov word [cs:dir_state], 0

.sf_loop:
    cmp byte [cs:file_count], MAX_FILES
    jae .sf_done
    mov al, [cs:mount_handle]
    mov cx, [cs:dir_state]
    push cs
    pop es
    mov di, dir_entry_buf
    mov ah, API_FS_READDIR
    int 0x80
    jc .sf_done
    mov [cs:dir_state], cx

    ; Skip volume labels (attr bit 3)
    test byte [cs:dir_entry_buf + 11], 0x08
    jnz .sf_loop

    ; Convert FAT 8.3 to dot format and store in file_table
    movzx bx, byte [cs:file_count]
    shl bx, 4                      ; * FILE_ENTRY_SIZE (16)
    add bx, file_table

    ; Copy name (8 chars, trim trailing spaces)
    mov si, dir_entry_buf
    mov di, bx                      ; DI points into file_table entry
    mov cx, 8
.sf_name:
    mov al, [cs:si]
    cmp al, ' '
    je .sf_name_end
    mov [cs:di], al
    inc si
    inc di
    loop .sf_name
    jmp .sf_dot
.sf_name_end:
    ; Skip remaining name spaces
.sf_dot:
    ; Check if extension is blank
    mov al, [cs:dir_entry_buf + 8]
    cmp al, ' '
    je .sf_no_ext
    mov byte [cs:di], '.'
    inc di
    mov si, dir_entry_buf
    add si, 8
    mov cx, 3
.sf_ext:
    mov al, [cs:si]
    cmp al, ' '
    je .sf_no_ext
    mov [cs:di], al
    inc si
    inc di
    loop .sf_ext
.sf_no_ext:
    mov byte [cs:di], 0            ; Null-terminate

    ; Store file size (16-bit, from offset 28 in dir entry)
    mov ax, [cs:dir_entry_buf + 28]
    mov [cs:bx + 13], ax

    inc byte [cs:file_count]
    jmp .sf_loop

.sf_done:
    pop es
    popa
    ret

; ============================================================================
; recompute_layout - Update layout from actual window dimensions
; ============================================================================
recompute_layout:
    pusha

    ; Get actual content dimensions via API 97
    mov al, [cs:win_handle]
    mov ah, API_WIN_GET_CONTENT_SIZE  ; Returns DX=content_w, SI=content_h
    int 0x80
    mov [cs:content_w], dx
    mov [cs:content_h], si
    ; list_w = content_w - 4 - SCROLLBAR_W
    mov ax, dx
    sub ax, 4
    sub ax, SCROLLBAR_W
    mov [cs:list_w], ax

    ; Recompute vis_rows from new content_h
    mov ax, [cs:content_h]
    sub ax, [cs:list_y]
    sub ax, 4
    mov dx, [cs:btn_h]
    shl dx, 1
    sub ax, dx
    xor dx, dx
    div word [cs:row_h]
    mov [cs:vis_rows], ax

    ; sep2_y = list_y + vis_rows * row_h
    mul word [cs:row_h]
    add ax, [cs:list_y]
    mov [cs:sep2_y], ax

    ; row1_y = sep2_y + 3
    add ax, 3
    mov [cs:row1_y], ax

    ; row2_y = row1_y + btn_h + 1
    add ax, [cs:btn_h]
    inc ax
    mov [cs:row2_y], ax

    popa
    ret

; ============================================================================
; draw_ui - Full UI redraw
; ============================================================================
draw_ui:
    pusha
    ; Clear content area (use API 97 for correct dimensions)
    mov al, 0xFF                    ; Current draw context
    mov ah, API_WIN_GET_CONTENT_SIZE
    int 0x80                        ; DX = content_w, SI = content_h
    mov bx, 0
    mov cx, 0
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Header
    mov bx, 6
    mov cx, 1                      ; HEADER_Y (always 1)
    mov si, str_hdr_name
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 190
    mov cx, 1
    mov si, str_hdr_size
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Separator lines
    mov bx, 2
    mov cx, [cs:sep1_y]
    mov dx, [cs:content_w]
    sub dx, 4
    mov al, 3                      ; white
    mov ah, API_DRAW_HLINE
    int 0x80
    mov bx, 2
    mov cx, [cs:sep2_y]
    mov dx, [cs:content_w]
    sub dx, 4
    mov al, 3
    mov ah, API_DRAW_HLINE
    int 0x80

    ; File list
    call draw_file_list

    ; Bottom area (buttons + status)
    call draw_bottom

    mov byte [cs:op_error], 0      ; Clear error after redraw
    popa
    ret

; ============================================================================
; draw_file_list - Draw visible file rows
; ============================================================================
draw_file_list:
    pusha
    xor cl, cl                      ; CL = visible row index
.dfl_row:
    mov al, cl
    add al, [cs:scroll_top]
    cmp al, [cs:file_count]
    jae .dfl_clear_rest

    ; Format row string
    push cx
    movzx bx, al
    call format_row                 ; display_buf filled

    ; Determine if selected
    xor al, al                      ; flags = 0 (normal)
    mov ah, cl
    add ah, [cs:scroll_top]
    cmp ah, [cs:sel_index]
    jne .dfl_not_sel
    mov al, 1                      ; flags = selected
.dfl_not_sel:
    ; Draw listitem: BX=X, CX=Y, DX=W, SI=text, AL=flags
    push ax
    xor ch, ch
    movzx ax, cl
    mul word [cs:row_h]
    add ax, [cs:list_y]
    mov cx, ax
    pop ax
    mov bx, 2
    mov dx, [cs:list_w]
    mov si, display_buf
    mov ah, API_DRAW_LISTITEM
    int 0x80
    pop cx
    inc cl
    cmp cl, byte [cs:vis_rows]
    jb .dfl_row
    jmp .dfl_scrollbar

.dfl_clear_rest:
    ; Clear remaining rows
    push cx
    ; Compute height FIRST while CL still has the row counter
    mov ax, [cs:vis_rows]
    xor ch, ch
    sub al, cl                      ; AL = remaining rows
    mul word [cs:row_h]
    mov si, ax                      ; SI = height
    ; Now compute Y (this clobbers CX)
    movzx ax, cl
    mul word [cs:row_h]
    add ax, [cs:list_y]
    mov cx, ax                      ; CX = Y
    mov bx, 2
    mov dx, [cs:list_w]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80
    pop cx

.dfl_scrollbar:
    ; Draw scrollbar at right edge of list area
    mov bx, [cs:list_w]
    add bx, 2                      ; X = list_w + 2
    mov cx, [cs:list_y]            ; Y = list_y
    mov ax, [cs:vis_rows]
    mul word [cs:row_h]
    mov si, ax                     ; track_height = vis_rows * row_h
    movzx dx, byte [cs:scroll_top] ; position
    movzx ax, byte [cs:file_count]
    sub ax, [cs:vis_rows]
    jns .dfl_sb_ok
    xor ax, ax
.dfl_sb_ok:
    mov di, ax                     ; max_range = file_count - vis_rows (or 0)
    mov al, 0                      ; flags: vertical
    mov ah, API_DRAW_SCROLLBAR
    int 0x80

.dfl_done:
    popa
    ret

; ============================================================================
; draw_bottom - Draw button bar and status line
; ============================================================================
draw_bottom:
    pusha
    ; Clear bottom area
    mov bx, 0
    mov cx, [cs:row1_y]
    dec cx                          ; row1_y - 1
    mov dx, [cs:content_w]
    mov si, [cs:content_h]
    add si, 2
    sub si, [cs:row1_y]            ; content_h - row1_y + 2
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    cmp byte [cs:mode], MODE_CONFIRM_DEL
    je .db_confirm
    cmp byte [cs:mode], MODE_RENAME
    je .db_rename
    cmp byte [cs:mode], MODE_COPY
    je .db_copy

    ; --- Normal mode ---
    mov ax, cs
    mov es, ax

    mov bx, BTN_DEL_X
    mov cx, [cs:row1_y]
    mov dx, BTN_DEL_W
    mov si, [cs:btn_h]
    mov di, str_delete
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    mov bx, BTN_REN_X
    mov cx, [cs:row1_y]
    mov dx, BTN_REN_W
    mov si, [cs:btn_h]
    mov di, str_rename
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    mov bx, BTN_CPY_X
    mov cx, [cs:row1_y]
    mov dx, BTN_CPY_W
    mov si, [cs:btn_h]
    mov di, str_copy
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; File count
    call draw_file_count

    ; Status line
    cmp byte [cs:op_error], 1
    je .db_err_status
    mov bx, 4
    mov cx, [cs:row2_y]
    mov si, str_ready
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .db_done

.db_err_status:
    mov bx, 4
    mov cx, [cs:row2_y]
    mov si, str_error
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .db_done

    ; --- Confirm delete mode ---
.db_confirm:
    ; "Del FILENAME?"
    mov bx, 4
    mov cx, [cs:row1_y]
    mov si, str_del_prefix
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Draw selected filename after "Del "
    call get_sel_name
    ; X = 4 + 4*font_adv ("Del " is 4 chars)
    movzx bx, byte [cs:font_adv]
    shl bx, 2                      ; * 4 chars
    add bx, 4
    push bx                        ; Save prefix end X
    mov cx, [cs:row1_y]
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; "?" after name: X = prefix_x + name_len * font_adv
    movzx ax, byte [cs:sel_name_len]
    movzx bx, byte [cs:font_adv]
    mul bx
    pop bx                         ; prefix end X
    add bx, ax
    mov cx, [cs:row1_y]
    mov si, str_question
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; [Yes] [No] buttons
    mov ax, cs
    mov es, ax
    mov bx, BTN_YES_X
    mov cx, [cs:row2_y]
    mov dx, BTN_YES_W
    mov si, [cs:btn_h]
    mov di, str_yes
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    mov bx, BTN_NO_X
    mov cx, [cs:row2_y]
    mov dx, BTN_NO_W
    mov si, [cs:btn_h]
    mov di, str_no
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    jmp .db_done

    ; --- Rename mode ---
.db_rename:
    mov bx, 4
    mov cx, [cs:row1_y]
    mov si, str_new_name
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Draw input with cursor
    call draw_input_line
    jmp .db_done

    ; --- Copy mode ---
.db_copy:
    mov bx, 4
    mov cx, [cs:row1_y]
    mov si, str_copy_to
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    call draw_input_line

.db_done:
    popa
    ret

; ============================================================================
; draw_input_line - Draw text input + cursor + [OK] button at ROW2_Y
; ============================================================================
draw_input_line:
    pusha
    ; Draw input text
    mov bx, 4
    mov cx, [cs:row2_y]
    mov si, input_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Draw cursor '_' after text
    movzx ax, byte [cs:input_len]
    movzx bx, byte [cs:font_adv]
    mul bx                          ; AX = input_len * font_adv
    add ax, 4
    mov bx, ax
    mov cx, [cs:row2_y]
    mov si, str_cursor
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; [OK] button
    mov ax, cs
    mov es, ax
    mov bx, BTN_OK_X
    mov cx, [cs:row2_y]
    mov dx, BTN_OK_W
    mov si, [cs:btn_h]
    mov di, str_ok
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80
    popa
    ret

; ============================================================================
; draw_file_count - Draw "N files" at right side of ROW1_Y
; ============================================================================
draw_file_count:
    pusha
    movzx dx, byte [cs:file_count]
    mov di, count_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov byte [cs:di], ' '
    inc di
    ; Append "files"
    mov si, str_files
.dfc_copy:
    mov al, [cs:si]
    mov [cs:di], al
    test al, al
    jz .dfc_draw
    inc si
    inc di
    jmp .dfc_copy
.dfc_draw:
    mov bx, 200
    mov cx, [cs:row1_y]
    add cx, 2
    mov si, count_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    popa
    ret

; ============================================================================
; format_row - Format file entry BX (index) into display_buf
; Input: BX = file index (0-based)
; ============================================================================
format_row:
    pusha
    shl bx, 4                      ; * FILE_ENTRY_SIZE
    add bx, file_table

    mov di, display_buf
    ; Copy filename
    mov si, bx
    mov cx, 13
.fr_name:
    mov al, [cs:si]
    test al, al
    jz .fr_pad
    mov [cs:di], al
    inc si
    inc di
    loop .fr_name
.fr_pad:
    ; Pad to column 22 with spaces
    mov ax, di
    sub ax, display_buf
.fr_pad_loop:
    cmp ax, 22
    jae .fr_size
    mov byte [cs:di], ' '
    inc di
    inc ax
    jmp .fr_pad_loop
.fr_size:
    ; Append file size as decimal
    mov dx, [cs:bx + 13]
    mov ah, API_WORD_TO_STRING
    int 0x80
    popa
    ret

; ============================================================================
; get_sel_name - Get pointer to selected filename in file_table
; Output: SI = pointer to filename, sel_name_len set
; ============================================================================
get_sel_name:
    push ax
    push bx
    movzx bx, byte [cs:sel_index]
    shl bx, 4
    add bx, file_table
    mov si, bx
    ; Compute length
    xor al, al
    mov bx, si
.gsn_len:
    cmp byte [cs:bx], 0
    je .gsn_done
    inc al
    inc bx
    cmp al, 13
    jb .gsn_len
.gsn_done:
    mov [cs:sel_name_len], al
    pop bx
    pop ax
    ret

; ============================================================================
; Data Section
; ============================================================================

window_title:   db 'File Manager', 0
win_handle:     db 0
mount_handle:   db 0
boot_drive:     db 0
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

; Dynamic layout variables (computed from font metrics at startup)
font_h:         db 8                ; Current font height
font_w:         db 8                ; Current font width
font_adv:       db 12               ; Current font advance
row_h:          dw 10               ; font_h + 2
vis_rows:       dw 12               ; Visible file rows
list_w:         dw 252              ; content_w - 4 - SCROLLBAR_W
list_y:         dw 13               ; sep1_y + 3
sep1_y:         dw 10               ; font_h + 2
sep2_y:         dw 133              ; list_y + vis_rows * row_h
row1_y:         dw 136              ; sep2_y + 3
row2_y:         dw 148              ; row1_y + btn_h + 1
btn_h:          dw 11               ; font_h + 3
content_w:      dw 260              ; WIN_W - 4 (border + padding)
content_h:      dw 156              ; WIN_H - 14 (titlebar + border)

; Strings
str_hdr_name:   db 'Name', 0
str_hdr_size:   db 'Size', 0
str_delete:     db 'Delete', 0
str_rename:     db 'Rename', 0
str_copy:       db 'Copy', 0
str_yes:        db 'Yes', 0
str_no:         db 'No', 0
str_ok:         db 'OK', 0
str_ready:      db 'Ready', 0
str_error:      db 'Error!', 0
str_del_prefix: db 'Del ', 0
str_question:   db '?', 0
str_new_name:   db 'New name:', 0
str_copy_to:    db 'Copy to:', 0
str_cursor:     db '_', 0
str_files:      db 'files', 0

; Buffers
input_buf:      times 14 db 0      ; 12 chars + null + padding
display_buf:    times 32 db 0
count_buf:      times 12 db 0
dir_entry_buf:  times 32 db 0

; File table: MAX_FILES entries x FILE_ENTRY_SIZE bytes
; Each entry: 13 bytes filename + 2 bytes size + 1 byte reserved
file_table:     times (MAX_FILES * FILE_ENTRY_SIZE) db 0

; Copy buffer (512 bytes for streaming)
copy_buf:       times 512 db 0
