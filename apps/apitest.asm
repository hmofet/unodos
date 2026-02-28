; APITEST.BIN - API Test Suite for UnoDOS
; Tests Build 247 APIs (67-80) and displays pass/fail results
;
; Build: nasm -f bin -o apitest.bin apitest.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'API Test', 0                ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; Checkmark icon: green/cyan check on black
    db 0x00, 0x00, 0x00, 0x00      ; Row 0
    db 0x00, 0x00, 0x00, 0x00      ; Row 1
    db 0x00, 0x00, 0x00, 0x14      ; Row 2:  ............cc
    db 0x00, 0x00, 0x00, 0x50      ; Row 3:  ...........cc.
    db 0x00, 0x00, 0x01, 0x40      ; Row 4:  ..........cc..
    db 0x00, 0x00, 0x05, 0x00      ; Row 5:  .........cc...
    db 0x00, 0x00, 0x14, 0x00      ; Row 6:  ........cc....
    db 0x50, 0x00, 0x50, 0x00      ; Row 7:  .cc....cc.....
    db 0x14, 0x01, 0x40, 0x00      ; Row 8:  ..cc..cc......
    db 0x05, 0x05, 0x00, 0x00      ; Row 9:  ...cccc.......
    db 0x01, 0x54, 0x00, 0x00      ; Row 10: ....ccc.......
    db 0x00, 0x50, 0x00, 0x00      ; Row 11: .....c........
    db 0x00, 0x00, 0x00, 0x00      ; Row 12
    db 0x00, 0x00, 0x00, 0x00      ; Row 13
    db 0x00, 0x00, 0x00, 0x00      ; Row 14
    db 0x00, 0x00, 0x00, 0x00      ; Row 15

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
API_GET_TICK            equ 63

; New Build 247 APIs under test
API_FILLED_RECT_COLOR   equ 67
API_RECT_COLOR          equ 68
API_DRAW_HLINE          equ 69
API_DRAW_VLINE          equ 70
API_DRAW_LINE           equ 71
API_GET_RTC_TIME        equ 72
API_DELAY_TICKS         equ 73
API_GET_TASK_INFO       equ 74
API_FS_SEEK             equ 75
API_FS_GET_FILE_SIZE    equ 76
API_WIN_GET_INFO        equ 79
API_SCROLL_AREA         equ 80

; Event types
EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Window params
WIN_X       equ 10
WIN_Y       equ 5
WIN_W       equ 300
WIN_H       equ 185

; Layout
COL1_X      equ 4          ; Drawing tests column
COL2_X      equ 150        ; Text results column
ROW_H       equ 10         ; Row height for text

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Create test window
    mov bx, WIN_X
    mov cx, WIN_Y
    mov dx, WIN_W
    mov si, WIN_H
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80
    jc .exit_fail
    mov [cs:wh], al

.repaint:
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Clear content area
    mov bx, 0
    mov cx, 0
    mov dx, WIN_W - 2
    mov si, WIN_H - 12
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; Header
    mov bx, 80
    mov cx, 2
    mov si, msg_header
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; === DRAWING TESTS (left column) ===

    ; --- Test API 67: Filled rect color ---
    mov bx, COL1_X
    mov cx, 16
    mov si, msg_t67
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw 4 colored filled rects (10x8 each)
    mov bx, COL1_X
    mov cx, 26
    mov dx, 10
    mov si, 8
    mov al, 0                       ; Black (color 0)
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    mov bx, COL1_X + 12
    mov cx, 26
    mov dx, 10
    mov si, 8
    mov al, 1                       ; Cyan
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    mov bx, COL1_X + 24
    mov cx, 26
    mov dx, 10
    mov si, 8
    mov al, 2                       ; Magenta
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    mov bx, COL1_X + 36
    mov cx, 26
    mov dx, 10
    mov si, 8
    mov al, 3                       ; White
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; --- Test API 68: Rect outline color ---
    mov bx, COL1_X + 55
    mov cx, 16
    mov si, msg_t68
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, COL1_X + 55
    mov cx, 26
    mov dx, 30
    mov si, 8
    mov al, 1                       ; Cyan outline
    mov ah, API_RECT_COLOR
    int 0x80

    mov bx, COL1_X + 90
    mov cx, 26
    mov dx, 30
    mov si, 8
    mov al, 2                       ; Magenta outline
    mov ah, API_RECT_COLOR
    int 0x80

    ; --- Test API 69: Horizontal line ---
    mov bx, COL1_X
    mov cx, 38
    mov si, msg_t69
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, COL1_X
    mov cx, 48
    mov dx, 40
    mov al, 1                       ; Cyan
    mov ah, API_DRAW_HLINE
    int 0x80

    mov bx, COL1_X
    mov cx, 50
    mov dx, 40
    mov al, 2                       ; Magenta
    mov ah, API_DRAW_HLINE
    int 0x80

    mov bx, COL1_X
    mov cx, 52
    mov dx, 40
    mov al, 3                       ; White
    mov ah, API_DRAW_HLINE
    int 0x80

    ; --- Test API 70: Vertical line ---
    mov bx, COL1_X + 55
    mov cx, 38
    mov si, msg_t70
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, COL1_X + 55
    mov cx, 48
    mov dx, 10
    mov al, 1                       ; Cyan
    mov ah, API_DRAW_VLINE
    int 0x80

    mov bx, COL1_X + 58
    mov cx, 48
    mov dx, 10
    mov al, 2                       ; Magenta
    mov ah, API_DRAW_VLINE
    int 0x80

    mov bx, COL1_X + 61
    mov cx, 48
    mov dx, 10
    mov al, 3                       ; White
    mov ah, API_DRAW_VLINE
    int 0x80

    ; --- Test API 71: Bresenham's line ---
    mov bx, COL1_X + 80
    mov cx, 38
    mov si, msg_t71
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Diagonal line from (COL1_X+80, 48) to (COL1_X+120, 58)
    mov bx, COL1_X + 80
    mov cx, 48
    mov dx, COL1_X + 120
    mov si, 58
    mov al, 3                       ; White
    mov ah, API_DRAW_LINE
    int 0x80

    ; Another line (opposite slope)
    mov bx, COL1_X + 80
    mov cx, 58
    mov dx, COL1_X + 120
    mov si, 48
    mov al, 1                       ; Cyan
    mov ah, API_DRAW_LINE
    int 0x80

    ; --- Test API 80: Scroll area ---
    mov bx, COL1_X
    mov cx, 62
    mov si, msg_t80
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw a colored block to scroll
    mov bx, COL1_X
    mov cx, 72
    mov dx, 20
    mov si, 12
    mov al, 2                       ; Magenta
    mov ah, API_FILLED_RECT_COLOR
    int 0x80

    ; Draw label inside
    mov bx, COL1_X + 2
    mov cx, 74
    mov si, msg_up
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Scroll it up by 4 pixels
    mov bx, COL1_X
    mov cx, 72
    mov dx, 20
    mov si, 12
    mov di, 4                       ; 4 pixels up
    mov ah, API_SCROLL_AREA
    int 0x80

    ; === NON-DRAWING TESTS (right column) ===

    ; --- Test API 72: RTC Time ---
    mov ah, API_GET_RTC_TIME
    int 0x80
    jc .t72_fail
    ; Basic sanity: hours < 0x24 (BCD)
    cmp ch, 0x24
    jae .t72_fail
    cmp cl, 0x60
    jae .t72_fail
    mov bx, COL2_X
    mov cx, 16
    mov si, msg_ok72
    jmp .t72_draw
.t72_fail:
    mov bx, COL2_X
    mov cx, 16
    mov si, msg_fail72
.t72_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Test API 73: Delay ticks ---
    mov ah, API_GET_TICK
    int 0x80
    mov [cs:tick_start], ax

    mov cx, 2                       ; Wait 2 ticks (~110ms)
    mov ah, API_DELAY_TICKS
    int 0x80

    mov ah, API_GET_TICK
    int 0x80
    sub ax, [cs:tick_start]
    cmp ax, 2
    jb .t73_fail
    mov bx, COL2_X
    mov cx, 26
    mov si, msg_ok73
    jmp .t73_draw
.t73_fail:
    mov bx, COL2_X
    mov cx, 26
    mov si, msg_fail73
.t73_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Test API 74: Task info ---
    mov ah, API_GET_TASK_INFO
    int 0x80
    ; CL should be > 0 (at least this task is running)
    test cl, cl
    jz .t74_fail
    mov bx, COL2_X
    mov cx, 36
    mov si, msg_ok74
    jmp .t74_draw
.t74_fail:
    mov bx, COL2_X
    mov cx, 36
    mov si, msg_fail74
.t74_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Test API 75: fs_seek (invalid handle, expect CF=1) ---
    mov al, 0xFF                    ; Invalid handle
    xor cx, cx
    xor dx, dx
    mov ah, API_FS_SEEK
    int 0x80
    jc .t75_ok                      ; CF=1 expected for invalid handle
    mov bx, COL2_X
    mov cx, 46
    mov si, msg_fail75
    jmp .t75_draw
.t75_ok:
    mov bx, COL2_X
    mov cx, 46
    mov si, msg_ok75
.t75_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Test API 76: fs_get_file_size (invalid handle, expect CF=1) ---
    mov al, 0xFF                    ; Invalid handle
    mov ah, API_FS_GET_FILE_SIZE
    int 0x80
    jc .t76_ok
    mov bx, COL2_X
    mov cx, 56
    mov si, msg_fail76
    jmp .t76_draw
.t76_ok:
    mov bx, COL2_X
    mov cx, 56
    mov si, msg_ok76
.t76_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- Test API 79: win_get_info ---
    mov al, [cs:wh]
    mov ah, API_WIN_GET_INFO
    int 0x80
    jc .t79_fail
    ; Verify: DX=WIN_W, SI=WIN_H
    cmp dx, WIN_W
    jne .t79_fail
    cmp si, WIN_H
    jne .t79_fail
    mov bx, COL2_X
    mov cx, 66
    mov si, msg_ok79
    jmp .t79_draw
.t79_fail:
    mov bx, COL2_X
    mov cx, 66
    mov si, msg_fail79
.t79_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; --- All tests complete ---
    mov bx, COL2_X
    mov cx, 86
    mov si, msg_done
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Wait for ESC
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
    ; Repaint content without recreating window
    jmp .repaint

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:wh]
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

; Data
win_title:  db 'API Test', 0
wh:         db 0
tick_start: dw 0

msg_header: db 'Build 247 API Tests', 0
msg_t67:    db '67:FillClr', 0
msg_t68:    db '68:RectClr', 0
msg_t69:    db '69:HLine', 0
msg_t70:    db '70:VLine', 0
msg_t71:    db '71:Line', 0
msg_t80:    db '80:Scroll', 0
msg_up:     db '^', 0

msg_ok72:   db '72:RTC    OK', 0
msg_fail72: db '72:RTC    FAIL', 0
msg_ok73:   db '73:Delay  OK', 0
msg_fail73: db '73:Delay  FAIL', 0
msg_ok74:   db '74:Task   OK', 0
msg_fail74: db '74:Task   FAIL', 0
msg_ok75:   db '75:Seek   OK', 0
msg_fail75: db '75:Seek   FAIL', 0
msg_ok76:   db '76:FSize  OK', 0
msg_fail76: db '76:FSize  FAIL', 0
msg_ok79:   db '79:WinInf OK', 0
msg_fail79: db '79:WinInf FAIL', 0

msg_done:   db 'All tests complete', 0
