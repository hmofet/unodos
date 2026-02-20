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
    ; Monitor icon: white outline, cyan screen
    db 0x00, 0x00, 0x00, 0x00      ; Row 0:  ................
    db 0x15, 0x55, 0x55, 0x54      ; Row 1:  .ccccccccccccc..
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 2:  WWWWWWWWWWWWWWWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 3:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 4:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 5:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 6:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 7:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 8:  WW.ccccccccc.WWW
    db 0x35, 0x55, 0x55, 0x5C      ; Row 9:  WW.ccccccccc.WWW
    db 0x3F, 0xFF, 0xFF, 0xFC      ; Row 10: WWWWWWWWWWWWWWWW
    db 0x00, 0x3F, 0xFC, 0x00      ; Row 11: ....WWWWWWWW....
    db 0x00, 0x0F, 0xF0, 0x00      ; Row 12: ......WWWW......
    db 0x00, 0x3F, 0xFC, 0x00      ; Row 13: ....WWWWWWWW....
    db 0x03, 0xFF, 0xFF, 0xC0      ; Row 14: ..WWWWWWWWWWWW..
    db 0x00, 0x00, 0x00, 0x00      ; Row 15: ................

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API constants
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
API_GET_FONT_INFO       equ 93

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Window params
WIN_X       equ 55
WIN_W       equ 210

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    ; --- Query static info ---

    ; Boot drive
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov [boot_drive], al

    ; Screen info
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [screen_w], bx
    mov [screen_h], cx

    ; Conventional memory (INT 12h → AX = KB)
    int 0x12
    mov [mem_kb], ax

    ; --- Compute dynamic layout from font metrics ---
    mov ah, API_GET_FONT_INFO
    int 0x80                    ; BH=height, BL=width, CL=advance
    mov [cs:si_font_h], bh
    mov [cs:si_font_adv], cl

    ; row_h = font_h + 2
    movzx ax, bh
    add ax, 2
    mov [cs:si_row_h], ax

    ; val_x = 4 + 6*advance + 4 (label column + gap)
    movzx ax, cl
    mov bx, 6
    mul bx
    add ax, 8
    mov [cs:si_val_x], ax

    ; Y positions: sequential layout with separator gaps
    ; r0 = 2 (Version)
    mov word [cs:si_y0], 2

    ; r1 = 2 + row_h (Build)
    mov ax, [cs:si_row_h]
    add ax, 2
    mov [cs:si_y1], ax

    ; sep1 = 2 + 2*row_h + 2
    mov ax, [cs:si_row_h]
    shl ax, 1
    add ax, 4
    mov [cs:si_ys1], ax

    ; r2 = sep1 + 4 (Boot)
    add ax, 4
    mov [cs:si_y2], ax

    ; r3 = r2 + row_h (Video)
    add ax, [cs:si_row_h]
    mov [cs:si_y3], ax

    ; r4 = r3 + row_h (Memory)
    add ax, [cs:si_row_h]
    mov [cs:si_y4], ax

    ; r5 = r4 + row_h + 2 (Tasks, extra gap)
    add ax, [cs:si_row_h]
    add ax, 2
    mov [cs:si_y5], ax

    ; sep2 = r5 + row_h + 2
    add ax, [cs:si_row_h]
    add ax, 2
    mov [cs:si_ys2], ax

    ; r6 = sep2 + 4 (Time)
    add ax, 4
    mov [cs:si_y6], ax

    ; r7 = r6 + row_h (Ticks)
    add ax, [cs:si_row_h]
    mov [cs:si_y7], ax

    ; win_h = r7 + row_h + 18 (bottom margin + titlebar + borders)
    add ax, [cs:si_row_h]
    add ax, 18
    mov [cs:si_win_h], ax

    ; win_y = (200 - win_h) / 2
    mov bx, 200
    sub bx, ax
    shr bx, 1
    mov [cs:si_win_y], bx

    ; --- Create window ---
    mov bx, WIN_X
    mov cx, [cs:si_win_y]
    mov dx, WIN_W
    mov si, [cs:si_win_h]
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03                ; BORDER | TITLE
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_no_win
    mov [wh], al

    ; Set drawing context
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Draw static info
    call draw_static

    ; Draw dynamic info
    call draw_dynamic

    ; Save initial tick count for refresh
    mov ah, API_GET_TICK
    int 0x80
    mov [last_tick], ax

    ; --- Main loop ---
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; Check events
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    cmp al, EVENT_KEY_PRESS
    jne .check_redraw
    cmp dl, 27                  ; ESC
    je .exit_ok
    jmp .main_loop

.check_redraw:
    cmp al, EVENT_WIN_REDRAW
    jne .main_loop
    call draw_static
    call draw_dynamic
    jmp .main_loop

.no_event:
    ; Check if 1 second elapsed
    mov ah, API_GET_TICK
    int 0x80
    mov bx, ax
    sub bx, [last_tick]
    cmp bx, 18                 ; ~1 second (18.2 Hz)
    jb .main_loop

    ; Update last_tick and refresh dynamic values
    mov [last_tick], ax
    call draw_dynamic
    jmp .main_loop

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
; draw_static - Draw static information labels
; ============================================================================
draw_static:
    pusha

    ; Row 0: Version
    mov bx, 4
    mov cx, [cs:si_y0]
    mov si, str_version
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 1: Build
    mov bx, 4
    mov cx, [cs:si_y1]
    mov si, str_build
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Separator line 1
    mov bx, 4
    mov cx, [cs:si_ys1]
    mov dx, WIN_W - 10
    mov si, 1
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Row 2: Boot drive
    mov bx, 4
    mov cx, [cs:si_y2]
    mov si, str_boot_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Print drive type
    mov bx, [cs:si_val_x]
    mov cx, [cs:si_y2]
    test byte [boot_drive], 0x80
    jnz .boot_hd
    mov si, str_floppy
    jmp .boot_draw
.boot_hd:
    mov si, str_harddisk
.boot_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 3: Video mode
    mov bx, 4
    mov cx, [cs:si_y3]
    mov si, str_video_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Format "WIDTHxHEIGHT" into num_buf
    mov dx, [screen_w]
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov byte [cs:di], 'x'
    inc di
    mov dx, [screen_h]
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, [cs:si_val_x]
    mov cx, [cs:si_y3]
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 4: Memory
    mov bx, 4
    mov cx, [cs:si_y4]
    mov si, str_mem_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Format "NNN KB" into num_buf
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
    mov bx, [cs:si_val_x]
    mov cx, [cs:si_y4]
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; draw_dynamic - Draw dynamic information (tasks, time, ticks)
; ============================================================================
draw_dynamic:
    pusha

    ; Row 5: Tasks (clear + redraw)
    mov bx, [cs:si_val_x]
    mov cx, [cs:si_y5]
    mov dx, WIN_W
    sub dx, [cs:si_val_x]
    mov si, [cs:si_row_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    mov bx, 4
    mov cx, [cs:si_y5]
    mov si, str_tasks_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ah, API_GET_TASK_INFO
    int 0x80
    ; CL = running task count, format "N/7"
    movzx dx, cl
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov byte [cs:di], '/'
    inc di
    mov byte [cs:di], '7'
    inc di
    mov byte [cs:di], 0
    mov bx, [cs:si_val_x]
    mov cx, [cs:si_y5]
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Separator 2
    mov bx, 4
    mov cx, [cs:si_ys2]
    mov dx, WIN_W - 10
    mov si, 1
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Row 6: Time (clear + redraw)
    mov bx, [cs:si_val_x]
    mov cx, [cs:si_y6]
    mov dx, WIN_W
    sub dx, [cs:si_val_x]
    mov si, [cs:si_row_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    mov bx, 4
    mov cx, [cs:si_y6]
    mov si, str_time_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ah, API_GET_RTC_TIME
    int 0x80
    ; CH=hours(BCD), CL=minutes(BCD), DH=seconds(BCD)
    ; Format: HH:MM:SS
    mov al, ch                  ; Hours
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [time_buf], ah
    mov [time_buf+1], al
    mov byte [time_buf+2], ':'
    mov al, cl                  ; Minutes
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [time_buf+3], ah
    mov [time_buf+4], al
    mov byte [time_buf+5], ':'
    mov al, dh                  ; Seconds
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [time_buf+6], ah
    mov [time_buf+7], al
    mov byte [time_buf+8], 0    ; Null terminate

    mov bx, [cs:si_val_x]
    mov cx, [cs:si_y6]
    mov si, time_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 7: Ticks (clear + redraw)
    mov bx, [cs:si_val_x]
    mov cx, [cs:si_y7]
    mov dx, WIN_W
    sub dx, [cs:si_val_x]
    mov si, [cs:si_row_h]
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    mov bx, 4
    mov cx, [cs:si_y7]
    mov si, str_ticks_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ah, API_GET_TICK
    int 0x80
    mov dx, ax
    mov di, num_buf
    mov ah, API_WORD_TO_STRING
    int 0x80
    mov bx, [cs:si_val_x]
    mov cx, [cs:si_y7]
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; Data
; ============================================================================

win_title:      db 'System Info', 0
wh:             db 0
last_tick:      dw 0
boot_drive:     db 0
screen_w:       dw 0
screen_h:       dw 0
mem_kb:         dw 0

; Dynamic layout variables
si_font_h:      db 8
si_font_adv:    db 12
si_row_h:       dw 10
si_val_x:       dw 80
si_win_h:       dw 110
si_win_y:       dw 45
si_y0:          dw 2        ; Version
si_y1:          dw 12       ; Build
si_ys1:         dw 24       ; Separator 1
si_y2:          dw 28       ; Boot
si_y3:          dw 38       ; Video
si_y4:          dw 48       ; Memory
si_y5:          dw 60       ; Tasks
si_ys2:         dw 72       ; Separator 2
si_y6:          dw 76       ; Time
si_y7:          dw 86       ; Ticks

; Static labels
str_version:    db 'UnoDOS v3.21.0', 0
str_build:      db 'Build 278', 0
str_boot_label: db 'Boot:', 0
str_floppy:     db 'Floppy', 0
str_harddisk:   db 'Hard Disk', 0
str_video_label: db 'Video:', 0
str_mem_label:  db 'Mem:', 0
str_tasks_label: db 'Tasks:', 0
str_time_label: db 'Time:', 0
str_ticks_label: db 'Ticks:', 0

; Buffers
time_buf:       times 9 db 0   ; "HH:MM:SS" + null
num_buf:        times 16 db 0  ; "320x200" or "NNN KB" etc.
