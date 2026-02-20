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

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Window params
WIN_X       equ 75
WIN_Y       equ 45
WIN_W       equ 170
WIN_H       equ 110

; Layout
ROW_H       equ 10         ; Row height

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

    ; --- Create window ---
    mov bx, WIN_X
    mov cx, WIN_Y
    mov dx, WIN_W
    mov si, WIN_H
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
    mov cx, 2
    mov si, str_version
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 1: Build
    mov bx, 4
    mov cx, 12
    mov si, str_build
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Separator line (clear a thin strip)
    mov bx, 4
    mov cx, 24
    mov dx, 160
    mov si, 1
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Row 2: Boot drive
    mov bx, 4
    mov cx, 28
    mov si, str_boot_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Print drive type
    mov bx, 64
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

    ; Row 3: Video mode
    mov bx, 4
    mov cx, 38
    mov si, str_video_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Format: WIDTHxHEIGHT
    mov ax, [screen_w]
    mov di, num_buf
    call word_to_decimal
    mov bx, 64
    mov cx, 38
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; "x"
    mov bx, 100
    mov cx, 38
    mov si, str_x
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; Height
    mov ax, [screen_h]
    mov di, num_buf
    call word_to_decimal
    mov bx, 112
    mov cx, 38
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 4: Memory
    mov bx, 4
    mov cx, 48
    mov si, str_mem_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov ax, [mem_kb]
    mov di, num_buf
    call word_to_decimal
    mov bx, 64
    mov cx, 48
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; " KB"
    mov bx, 100
    mov cx, 48
    mov si, str_kb
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
    mov bx, 64
    mov cx, 60
    mov dx, 80
    mov si, ROW_H
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    mov bx, 4
    mov cx, 60
    mov si, str_tasks_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ah, API_GET_TASK_INFO
    int 0x80
    ; CL = running task count
    mov al, cl
    mov di, num_buf
    call byte_to_decimal
    mov bx, 64
    mov cx, 60
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    ; "/7"
    mov bx, 88
    mov cx, 60
    mov si, str_slash7
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Separator
    mov bx, 4
    mov cx, 72
    mov dx, 160
    mov si, 1
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Row 6: Time (clear + redraw)
    mov bx, 64
    mov cx, 76
    mov dx, 100
    mov si, ROW_H
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    mov bx, 4
    mov cx, 76
    mov si, str_time_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ah, API_GET_RTC_TIME
    int 0x80
    ; CH=hours(BCD), CL=minutes(BCD), DH=seconds(BCD)
    ; Format: HH:MM:SS
    mov al, ch                  ; Hours
    mov di, time_buf
    call bcd_to_ascii
    mov byte [di], ':'
    inc di
    mov al, cl                  ; Minutes
    call bcd_to_ascii
    mov byte [di], ':'
    inc di
    mov al, dh                  ; Seconds
    call bcd_to_ascii
    mov byte [di], 0            ; Null terminate

    mov bx, 64
    mov cx, 76
    mov si, time_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 7: Ticks (clear + redraw)
    mov bx, 64
    mov cx, 86
    mov dx, 100
    mov si, ROW_H
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    mov bx, 4
    mov cx, 86
    mov si, str_ticks_label
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ah, API_GET_TICK
    int 0x80
    mov di, num_buf
    call word_to_decimal
    mov bx, 64
    mov cx, 86
    mov si, num_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    popa
    ret

; ============================================================================
; Helpers
; ============================================================================

; bcd_to_ascii - Convert BCD byte to 2 ASCII digits
; Input: AL=BCD value, DS:DI=destination
; Output: DI advanced by 2
bcd_to_ascii:
    push ax
    mov ah, al
    shr al, 4                  ; High nibble
    add al, '0'
    mov [di], al
    inc di
    mov al, ah
    and al, 0x0F               ; Low nibble
    add al, '0'
    mov [di], al
    inc di
    pop ax
    ret

; word_to_decimal - Convert 16-bit value to decimal string
; Input: AX=value, DS:DI=destination buffer (6 bytes min)
; Output: null-terminated decimal string at DI
word_to_decimal:
    pusha
    mov bx, di                 ; Save start
    mov cx, 0                  ; Digit count
    mov si, 10

.wtd_loop:
    xor dx, dx
    div si                     ; AX = AX/10, DX = remainder
    push dx                    ; Save digit
    inc cx
    test ax, ax
    jnz .wtd_loop

    ; Pop digits in correct order
    mov di, bx
.wtd_pop:
    pop ax
    add al, '0'
    mov [di], al
    inc di
    loop .wtd_pop
    mov byte [di], 0           ; Null terminate
    popa
    ret

; byte_to_decimal - Convert 8-bit value to decimal string
; Input: AL=value, DS:DI=destination buffer
; Output: null-terminated decimal string at DI
byte_to_decimal:
    push ax
    xor ah, ah
    call word_to_decimal
    pop ax
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

; Static labels
str_version:    db 'UnoDOS v3.21.0', 0
str_build:      db 'Build 276', 0
str_boot_label: db 'Boot:', 0
str_floppy:     db 'Floppy', 0
str_harddisk:   db 'Hard Disk', 0
str_video_label: db 'Video:', 0
str_x:          db 'x', 0
str_mem_label:  db 'Mem:', 0
str_kb:         db 'KB', 0
str_tasks_label: db 'Tasks:', 0
str_slash7:     db '/7', 0
str_time_label: db 'Time:', 0
str_ticks_label: db 'Ticks:', 0

; Buffers
time_buf:       times 9 db 0   ; "HH:MM:SS" + null
num_buf:        times 8 db 0   ; decimal number + null
