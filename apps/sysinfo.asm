; SYSINFO.BIN - System Information for UnoDOS
; Displays system info with font-aware layout.
; Builds each row as a complete string in a line buffer for clean spacing.

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E
    db 'UI'
    db 'Sys Info', 0
    times (0x04 + 12) - ($ - $$) db 0

    db 0x00, 0x00, 0x00, 0x00
    db 0x15, 0x55, 0x55, 0x54
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0x00, 0x3F, 0xFC, 0x00
    db 0x00, 0x0F, 0xF0, 0x00
    db 0x00, 0x3F, 0xFC, 0x00
    db 0x03, 0xFF, 0xFF, 0xC0
    db 0x00, 0x00, 0x00, 0x00

    times 0x50 - ($ - $$) db 0

; --- Code Entry (offset 0x50) ---

API_GFX_DRAW_STRING    equ 4
API_GFX_CLEAR_AREA     equ 5
API_EVENT_GET          equ 9
API_WIN_CREATE         equ 20
API_WIN_DESTROY        equ 21
API_MOUSE_STATE        equ 28
API_WIN_BEGIN_DRAW     equ 31
API_WIN_END_DRAW       equ 32
API_APP_YIELD          equ 34
API_GET_BOOT_DRIVE     equ 43
API_DRAW_BUTTON        equ 51
API_HIT_TEST           equ 53
API_GET_TICK_COUNT     equ 63
API_GET_RTC_TIME       equ 72
API_WIN_GET_CONTENT_SIZE equ 97
API_GET_TASK_INFO      equ 74
API_WIN_RESIZE         equ 78
API_GET_SCREEN_INFO    equ 82
API_BCD_TO_ASCII       equ 92
API_GET_FONT_INFO      equ 93

EVENT_KEY_PRESS        equ 1
EVENT_WIN_REDRAW       equ 6

NUM_ROWS               equ 6
LINE_COLS              equ 22          ; Max chars per line
VAL_COL                equ 9          ; Value starts at char column 9

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Get font metrics to compute window size
    mov ah, API_GET_FONT_INFO
    int 0x80
    ; BH=height, BL=width, CL=advance
    mov [cs:char_w], cl            ; Use advance for spacing
    mov [cs:char_h], bh

    ; Compute window dimensions from font
    ; Width = LINE_COLS * advance + 8 (4px padding each side)
    movzx ax, cl
    mov dx, LINE_COLS
    mul dx
    add ax, 8
    mov [cs:win_w], ax

    ; Height = NUM_ROWS * (char_h + 4) + btn_height + 8
    movzx ax, bh
    add ax, 4                      ; row_height = char_h + 4
    mov [cs:row_h], ax
    mov dx, NUM_ROWS
    mul dx                         ; AX = rows * row_h
    add ax, 22                     ; 14 btn + 4 gap + 4 bottom pad
    mov [cs:win_h], ax

    ; Button Y = rows * row_h + 4
    mov ax, [cs:row_h]
    mov dx, NUM_ROWS
    mul dx
    add ax, 4
    mov [cs:btn_y], ax

    ; Create window (centered)
    mov ax, [cs:win_w]
    mov dx, ax                     ; DX = width
    mov ax, [cs:win_h]
    mov si, ax                     ; SI = height
    mov bx, 10
    mov cx, 10
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:win_handle], al

    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    call draw_ui

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .check_mouse

    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call draw_ui
    jmp .main_loop

.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    cmp dl, 27
    je .exit_ok
    cmp dl, 13
    je .exit_ok
    jmp .main_loop

.check_mouse:
    mov ah, API_MOUSE_STATE
    int 0x80
    test dl, 1
    jz .mouse_up
    cmp byte [cs:prev_btn], 0
    jne .main_loop
    mov byte [cs:prev_btn], 1

    ; Hit test OK button (centered)
    mov ax, [cs:win_w]
    sub ax, 40
    shr ax, 1
    mov bx, ax
    mov cx, [cs:btn_y]
    mov dx, 40
    mov si, 14
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .exit_ok
    jmp .main_loop

.mouse_up:
    mov byte [cs:prev_btn], 0
    jmp .main_loop

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80

.exit_fail:
    pop es
    pop ds
    popa
    retf

; ============================================================================
; draw_ui - Build and draw all rows + OK button
; ============================================================================
draw_ui:
    pusha
    mov ax, cs
    mov ds, ax

    ; Clear content area (use API 97 for correct dimensions)
    mov al, 0xFF                    ; Current draw context
    mov ah, API_WIN_GET_CONTENT_SIZE
    int 0x80                        ; DX = content_w, SI = content_h
    mov bx, 0
    mov cx, 0
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; --- Row 0: "Video    320x200" ---
    call line_clear
    mov si, s_video
    call line_puts
    call line_pad_val
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [cs:t_scr_w], bx
    mov [cs:t_scr_h], cx
    mov [cs:t_vmode], al
    mov ax, [cs:t_scr_w]
    call line_putdec
    mov byte [cs:line_buf + di], 'x'
    inc di
    mov ax, [cs:t_scr_h]
    call line_putdec
    call line_term
    mov cx, 0
    call draw_row

    ; --- Row 1: "Boot     HD/CF" ---
    call line_clear
    mov si, s_boot
    call line_puts
    call line_pad_val
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    cmp al, 0x80
    jae .r1_hd
    mov si, s_floppy
    jmp .r1_put
.r1_hd:
    mov si, s_hd
.r1_put:
    call line_puts
    call line_term
    mov cx, 1
    call draw_row

    ; --- Row 2: "Tasks    2 running" ---
    call line_clear
    mov si, s_tasks
    call line_puts
    call line_pad_val
    mov ah, API_GET_TASK_INFO
    int 0x80
    movzx ax, cl
    call line_putdec
    mov byte [cs:line_buf + di], ' '
    inc di
    mov si, s_running
    call line_puts
    call line_term
    mov cx, 2
    call draw_row

    ; --- Row 3: "Font     8x8" ---
    call line_clear
    mov si, s_font
    call line_puts
    call line_pad_val
    mov ah, API_GET_FONT_INFO
    int 0x80
    movzx ax, bl
    call line_putdec
    mov byte [cs:line_buf + di], 'x'
    inc di
    movzx ax, bh
    call line_putdec
    call line_term
    mov cx, 3
    call draw_row

    ; --- Row 4: "Time     12:34:56" ---
    call line_clear
    mov si, s_time
    call line_puts
    call line_pad_val
    mov ah, API_GET_RTC_TIME
    int 0x80
    push cx
    push dx
    mov al, ch
    call line_putbcd
    mov byte [cs:line_buf + di], ':'
    inc di
    pop dx
    pop cx
    mov al, cl
    call line_putbcd
    mov byte [cs:line_buf + di], ':'
    inc di
    mov al, dh
    call line_putbcd
    call line_term
    mov cx, 4
    call draw_row

    ; --- Row 5: "Uptime   123s M:13" ---
    call line_clear
    mov si, s_uptime
    call line_puts
    call line_pad_val
    mov ah, API_GET_TICK_COUNT
    int 0x80
    push ax
    xor dx, dx
    mov cx, 18
    div cx
    call line_putdec
    mov byte [cs:line_buf + di], 's'
    inc di
    ; Append mode info: " M:xx"
    mov byte [cs:line_buf + di], ' '
    inc di
    mov byte [cs:line_buf + di], 'M'
    inc di
    mov byte [cs:line_buf + di], ':'
    inc di
    movzx ax, byte [cs:t_vmode]
    call line_puthex
    pop ax
    call line_term
    mov cx, 5
    call draw_row

    ; --- OK button (centered) ---
    mov ax, cs
    mov es, ax
    mov ax, [cs:win_w]
    sub ax, 40
    shr ax, 1
    mov bx, ax
    mov cx, [cs:btn_y]
    mov dx, 40
    mov si, 14
    mov di, s_ok
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    popa
    ret

; ============================================================================
; draw_row - Draw line_buf at row CX
; Input: CX = row index (0-5)
; ============================================================================
draw_row:
    push ax
    push bx
    push cx
    push dx
    ; Y = 4 + row_index * row_h
    mov ax, [cs:row_h]
    mul cx
    add ax, 4
    mov cx, ax                     ; CX = Y
    mov bx, 4                      ; X = 4
    mov si, line_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Line buffer helpers (DI = current write position in line_buf)
; ============================================================================

; line_clear: reset buffer position
line_clear:
    xor di, di
    ret

; line_puts: copy null-terminated string at DS:SI to line_buf
line_puts:
    push ax
.lp_loop:
    mov al, [cs:si]
    test al, al
    jz .lp_done
    mov [cs:line_buf + di], al
    inc si
    inc di
    jmp .lp_loop
.lp_done:
    pop ax
    ret

; line_pad_val: pad with spaces to VAL_COL
line_pad_val:
.lpv:
    cmp di, VAL_COL
    jae .lpv_done
    mov byte [cs:line_buf + di], ' '
    inc di
    jmp .lpv
.lpv_done:
    ret

; line_putdec: write AX as decimal digits to line_buf
line_putdec:
    push ax
    push bx
    push cx
    push dx
    mov cx, 0
    mov bx, 10
.lpd_div:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .lpd_div
.lpd_pop:
    pop dx
    add dl, '0'
    mov [cs:line_buf + di], dl
    inc di
    loop .lpd_pop
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; line_puthex: write AL as 2 hex chars to line_buf
line_puthex:
    push ax
    push cx
    mov cl, al
    shr al, 4
    call .hex_nib
    mov [cs:line_buf + di], al
    inc di
    mov al, cl
    and al, 0x0F
    call .hex_nib
    mov [cs:line_buf + di], al
    inc di
    pop cx
    pop ax
    ret
.hex_nib:
    cmp al, 10
    jb .hex_dig
    add al, 'A' - 10
    ret
.hex_dig:
    add al, '0'
    ret

; line_putbcd: write AL (BCD) as 2 decimal chars to line_buf
line_putbcd:
    push ax
    push cx
    mov cl, al
    shr al, 4
    add al, '0'
    mov [cs:line_buf + di], al
    inc di
    mov al, cl
    and al, 0x0F
    add al, '0'
    mov [cs:line_buf + di], al
    inc di
    pop cx
    pop ax
    ret

; line_term: null-terminate line_buf
line_term:
    mov byte [cs:line_buf + di], 0
    ret

; ============================================================================
; Data
; ============================================================================

win_title:      db 'System Info', 0
win_handle:     db 0
prev_btn:       db 0
char_w:         db 0
char_h:         db 0
win_w:          dw 0
win_h:          dw 0
row_h:          dw 0
btn_y:          dw 0
t_scr_w:        dw 0
t_scr_h:        dw 0
t_vmode:        db 0

line_buf:       times 26 db 0

s_video:        db 'Video', 0
s_boot:         db 'Boot', 0
s_tasks:        db 'Tasks', 0
s_font:         db 'Font', 0
s_time:         db 'Time', 0
s_uptime:       db 'Uptime', 0
s_floppy:       db 'Floppy', 0
s_hd:           db 'HD/CF', 0
s_running:      db 'running', 0
s_ok:           db 'OK', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
