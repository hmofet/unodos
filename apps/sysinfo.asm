; SYSINFO.BIN - System Information for UnoDOS
; Displays OS version, boot drive, video, memory, tasks, time, ticks
;
; Build: nasm -f bin -o sysinfo.bin sysinfo.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Sys Info', 0                ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
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

API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_GET_BOOT_DRIVE      equ 43
API_GET_TICK            equ 63
API_GET_RTC_TIME        equ 72
API_GET_TASK_INFO       equ 74
API_GET_SCREEN_INFO     equ 82
API_WORD_TO_STRING      equ 91
API_BCD_TO_ASCII        equ 92

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

VAL_X       equ 56
WIN_W       equ 210

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    ; --- Diagnostic: draw progress markers at top of screen ---
    ; Each marker = letter drawn at increasing X position
    ; If crash occurs, last visible letter shows where it happened

    ; Marker A: before any API calls
    mov bx, 0
    mov cx, 0
    mov si, str_A
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Query boot drive
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov [boot_drive], al

    ; Marker B: after get_boot_drive
    mov bx, 8
    mov cx, 0
    mov si, str_B
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Query screen info
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [screen_w], bx
    mov [screen_h], cx

    ; Marker C: after get_screen_info
    mov bx, 16
    mov cx, 0
    mov si, str_C
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Read BDA memory
    push es
    mov ax, 0x0040
    mov es, ax
    mov ax, [es:0x0013]
    pop es
    mov [mem_kb], ax

    ; Marker D: after BDA read
    mov bx, 24
    mov cx, 0
    mov si, str_D
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Create window
    mov bx, 55
    mov cx, 43
    mov dx, WIN_W
    mov si, 114
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_no_win
    mov [wh], al

    ; Marker E: after win_create
    ; (draw_context not set yet, so this goes to screen directly)
    mov bx, 32
    mov cx, 0
    mov si, str_E
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Begin draw
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Marker F: after begin_draw (now inside window context)
    mov bx, 4
    mov cx, 2
    mov si, str_F
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw version string
    mov bx, 12
    mov cx, 2
    mov si, str_version
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Marker G: after first draw_string in window
    mov bx, 4
    mov cx, 12
    mov si, str_G
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw separator (clear_area)
    mov bx, 4
    mov cx, 24
    mov dx, WIN_W - 10
    mov si, 1
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Marker H: after clear_area
    mov bx, 12
    mov cx, 12
    mov si, str_H
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw boot drive value
    mov bx, 4
    mov cx, 28
    mov si, str_boot_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, VAL_X
    mov cx, 28
    test byte [boot_drive], 0x80
    jnz .boot_hd
    mov si, str_floppy
    jmp .boot_draw
.boot_hd:
    mov si, str_harddisk
.boot_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Marker I: after boot drive draw
    mov bx, 20
    mov cx, 12
    mov si, str_I
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Format screen resolution
    mov dx, [screen_w]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80

    ; Marker J: after first word_to_string
    mov bx, 28
    mov cx, 12
    mov si, str_J
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov byte [cs:di], 'x'
    inc di
    mov dx, [screen_h]
    mov ah, API_WORD_TO_STRING
    int 0x80

    mov bx, 4
    mov cx, 38
    mov si, str_video_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, VAL_X
    mov cx, 38
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Marker K: after video line
    mov bx, 36
    mov cx, 12
    mov si, str_K
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Memory line
    mov bx, 4
    mov cx, 48
    mov si, str_mem_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov dx, [mem_kb]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov byte [cs:di], ' '
    inc di
    mov byte [cs:di], 'K'
    inc di
    mov byte [cs:di], 'B'
    inc di
    mov byte [cs:di], 0
    mov bx, VAL_X
    mov cx, 48
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Marker L: after memory line
    mov bx, 44
    mov cx, 12
    mov si, str_L
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Tasks
    mov bx, 4
    mov cx, 60
    mov si, str_tasks_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ah, API_GET_TASK_INFO
    int 0x80
    movzx dx, cl
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov byte [cs:di], '/'
    inc di
    mov byte [cs:di], '7'
    inc di
    mov byte [cs:di], 0
    mov bx, VAL_X
    mov cx, 60
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Marker M: after tasks
    mov bx, 52
    mov cx, 12
    mov si, str_M
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Separator 2
    mov bx, 4
    mov cx, 72
    mov dx, WIN_W - 10
    mov si, 1
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Time
    mov bx, 4
    mov cx, 76
    mov si, str_time_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ah, API_GET_RTC_TIME
    int 0x80
    mov al, ch
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [time_buf], ah
    mov [time_buf+1], al
    mov byte [time_buf+2], ':'
    mov al, cl
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [time_buf+3], ah
    mov [time_buf+4], al
    mov byte [time_buf+5], ':'
    mov al, dh
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [time_buf+6], ah
    mov [time_buf+7], al
    mov byte [time_buf+8], 0
    mov bx, VAL_X
    mov cx, 76
    mov si, time_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Marker N: after time
    mov bx, 60
    mov cx, 12
    mov si, str_N
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Ticks
    mov bx, 4
    mov cx, 86
    mov si, str_ticks_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ah, API_GET_TICK
    int 0x80
    mov dx, ax
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, VAL_X
    mov cx, 86
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Marker O: ALL DONE - entering main loop
    mov bx, 68
    mov cx, 12
    mov si, str_O
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Main loop (no periodic refresh for diagnostics) ---
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .main_loop

    cmp al, EVENT_KEY_PRESS
    jne .check_redraw
    cmp dl, 27
    je .exit_ok
    jmp .main_loop

.check_redraw:
    cmp al, EVENT_WIN_REDRAW
    jne .main_loop
    jmp .main_loop              ; Skip redraw for now (diag build)

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:wh]
    mov ah, API_WIN_DESTROY
    int 0x80

.exit_no_win:
    pop es
    pop ds
    popa
    retf

; ============================================================================
; Data
; ============================================================================

win_title:      db 'System Info', 0
wh:             db 0
boot_drive:     db 0
screen_w:       dw 0
screen_h:       dw 0
mem_kb:         dw 0

; Diagnostic markers (single-char strings)
str_A:          db 'A', 0
str_B:          db 'B', 0
str_C:          db 'C', 0
str_D:          db 'D', 0
str_E:          db 'E', 0
str_F:          db 'F', 0
str_G:          db 'G', 0
str_H:          db 'H', 0
str_I:          db 'I', 0
str_J:          db 'J', 0
str_K:          db 'K', 0
str_L:          db 'L', 0
str_M:          db 'M', 0
str_N:          db 'N', 0
str_O:          db 'O', 0

str_version:    db 'UnoDOS v3.21.0', 0
str_build:      db 'Build 323', 0
str_boot_label: db 'Boot:', 0
str_floppy:     db 'Floppy', 0
str_harddisk:   db 'Hard Disk', 0
str_video_label: db 'Video:', 0
str_mem_label:  db 'Mem:', 0
str_tasks_label: db 'Tasks:', 0
str_time_label: db 'Time:', 0
str_ticks_label: db 'Ticks:', 0

; Buffers
time_buf:       times 9 db 0
num_buf:        times 16 db 0
