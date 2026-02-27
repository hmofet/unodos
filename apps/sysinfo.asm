; SYSINFO.BIN - System Information for UnoDOS
; Displays system info: video mode, boot drive, tasks, time, font, uptime.
; Uses standard APIs only - no direct port I/O.

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
API_WIN_BEGIN_DRAW     equ 31
API_WIN_END_DRAW       equ 32
API_APP_YIELD          equ 34
API_GET_BOOT_DRIVE     equ 43
API_GET_TICK_COUNT     equ 63
API_GET_RTC_TIME       equ 72
API_GET_TASK_INFO      equ 74
API_GET_SCREEN_INFO    equ 82
API_BCD_TO_ASCII       equ 92
API_GET_FONT_INFO      equ 93

EVENT_KEY_PRESS        equ 1
EVENT_WIN_REDRAW       equ 6

ROW_HEIGHT             equ 12

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create window
    mov bx, 30
    mov cx, 30
    mov dx, 160
    mov si, 110
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

    call draw_info

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    cmp al, EVENT_WIN_REDRAW
    jne .not_redraw
    call draw_info
    jmp .main_loop

.not_redraw:
    cmp al, EVENT_KEY_PRESS
    jne .no_event
    cmp dl, 27                 ; ESC
    je .exit_ok

.no_event:
    jmp .main_loop

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:win_handle]
    mov ah, API_WIN_DESTROY
    int 0x80
    jmp .exit

.exit_fail:
.exit:
    pop es
    pop ds
    popa
    retf

; ============================================================================
; draw_info - Query APIs and draw all system info
; ============================================================================
draw_info:
    pusha
    mov ax, cs
    mov ds, ax

    ; Clear content area
    mov bx, 0
    mov cx, 0
    mov dx, 158
    mov si, 108
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; --- Row 0: Title ---
    mov bx, 4
    mov cx, 2
    mov si, str_title
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Row 1: Video mode + resolution ---
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    ; AL=mode, AH=colors, BX=width, CX=height
    mov [cs:scr_w], bx
    mov [cs:scr_h], cx
    mov [cs:vid_mode], al
    mov [cs:vid_colors], ah

    ; Format: "Video: XXXxYYY ModeXX"
    mov bx, 4
    mov cx, ROW_HEIGHT + 2
    mov si, str_video
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Width
    mov ax, [cs:scr_w]
    call word_to_dec
    mov bx, 52
    mov si, dec_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; "x"
    mov bx, 76
    mov si, str_x
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Height
    mov ax, [cs:scr_h]
    call word_to_dec
    mov bx, 84
    mov si, dec_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Mode hex
    mov bx, 116
    mov si, str_mode
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [cs:vid_mode]
    call byte_to_hex
    mov bx, 140
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Row 2: Boot drive ---
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov [cs:boot_drv], al

    mov bx, 4
    mov cx, ROW_HEIGHT * 2 + 2
    mov si, str_boot
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 52
    cmp byte [cs:boot_drv], 0x80
    jae .hd_boot
    mov si, str_floppy
    jmp .draw_boot
.hd_boot:
    mov si, str_hd
.draw_boot:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Row 3: Tasks ---
    mov ah, API_GET_TASK_INFO
    int 0x80
    ; AL=current, BL=focused, CL=count
    mov [cs:task_cur], al
    mov [cs:task_foc], bl
    mov [cs:task_cnt], cl

    mov bx, 4
    mov cx, ROW_HEIGHT * 3 + 2
    mov si, str_tasks
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [cs:task_cnt]
    call word_to_dec
    mov bx, 52
    mov si, dec_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 76
    mov si, str_running
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Row 4: Font info ---
    mov ah, API_GET_FONT_INFO
    int 0x80
    ; BH=height, BL=width, CL=advance, AL=index
    mov [cs:font_idx], al
    mov [cs:font_w], bl
    mov [cs:font_h], bh

    mov bx, 4
    mov cx, ROW_HEIGHT * 4 + 2
    mov si, str_font
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [cs:font_idx]
    call word_to_dec
    mov bx, 44
    mov si, dec_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; "(WxH)"
    mov bx, 64
    mov si, str_lparen
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [cs:font_w]
    call word_to_dec
    mov bx, 72
    mov si, dec_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 88
    mov si, str_x
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [cs:font_h]
    call word_to_dec
    mov bx, 96
    mov si, dec_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 112
    mov si, str_rparen
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Row 5: Time ---
    mov ah, API_GET_RTC_TIME
    int 0x80
    ; CH=hours, CL=mins, DH=secs (all BCD)
    mov [cs:rtc_h], ch
    mov [cs:rtc_m], cl
    mov [cs:rtc_s], dh

    ; Format HH:MM:SS
    mov al, [cs:rtc_h]
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [cs:time_str], ah
    mov [cs:time_str+1], al

    mov al, [cs:rtc_m]
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [cs:time_str+3], ah
    mov [cs:time_str+4], al

    mov al, [cs:rtc_s]
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [cs:time_str+6], ah
    mov [cs:time_str+7], al

    mov bx, 4
    mov cx, ROW_HEIGHT * 5 + 2
    mov si, str_time
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 44
    mov si, time_str
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Row 6: Uptime in ticks ---
    mov ah, API_GET_TICK_COUNT
    int 0x80
    ; AX = tick count
    mov [cs:ticks], ax

    ; Convert to seconds: ticks / 18
    xor dx, dx
    mov cx, 18
    div cx                     ; AX = seconds
    call word_to_dec

    mov bx, 4
    mov cx, ROW_HEIGHT * 6 + 2
    mov si, str_uptime
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 60
    mov si, dec_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 100
    mov si, str_secs
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Row 7: ESC to close ---
    mov bx, 4
    mov cx, ROW_HEIGHT * 7 + 6
    mov si, str_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; word_to_dec - Convert 16-bit unsigned to decimal string (right-aligned)
; Input: AX = value
; Output: dec_buf filled with decimal string (null terminated)
; ============================================================================
word_to_dec:
    push ax
    push bx
    push cx
    push dx

    mov cx, 0                  ; digit count
    mov bx, 10

.div_loop:
    xor dx, dx
    div bx                     ; AX = quotient, DX = remainder
    push dx                    ; save digit
    inc cx
    test ax, ax
    jnz .div_loop

    ; Pop digits into buffer
    mov bx, 0
.pop_loop:
    pop dx
    add dl, '0'
    mov [cs:dec_buf + bx], dl
    inc bx
    loop .pop_loop
    mov byte [cs:dec_buf + bx], 0

    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; byte_to_hex - Convert byte to 2-char hex string
; Input: AL = byte
; Output: hex_buf filled
; ============================================================================
byte_to_hex:
    push ax
    push cx
    mov cl, al
    shr al, 4
    call .nibble
    mov [cs:hex_buf], al
    mov al, cl
    and al, 0x0F
    call .nibble
    mov [cs:hex_buf+1], al
    mov byte [cs:hex_buf+2], 0
    pop cx
    pop ax
    ret
.nibble:
    cmp al, 10
    jb .is_digit
    add al, 'A' - 10
    ret
.is_digit:
    add al, '0'
    ret

; ============================================================================
; Data
; ============================================================================

win_title:      db 'System Info', 0
win_handle:     db 0

; Buffers
dec_buf:        db '00000', 0
hex_buf:        db '00', 0

; Saved values
scr_w:          dw 0
scr_h:          dw 0
vid_mode:       db 0
vid_colors:     db 0
boot_drv:       db 0
task_cur:       db 0
task_foc:       db 0
task_cnt:       db 0
font_idx:       db 0
font_w:         db 0
font_h:         db 0
rtc_h:          db 0
rtc_m:          db 0
rtc_s:          db 0
ticks:          dw 0
time_str:       db '00:00:00', 0

; Labels
str_title:      db 'System Info', 0
str_video:      db 'Video:', 0
str_x:          db 'x', 0
str_mode:       db 'M:', 0
str_boot:       db 'Boot:', 0
str_floppy:     db 'Floppy', 0
str_hd:         db 'HD/CF', 0
str_tasks:      db 'Tasks:', 0
str_running:    db 'running', 0
str_font:       db 'Font:', 0
str_lparen:     db '(', 0
str_rparen:     db ')', 0
str_time:       db 'Time:', 0
str_uptime:     db 'Uptime:', 0
str_secs:       db 'sec', 0
str_esc:        db '[ESC] Close', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
