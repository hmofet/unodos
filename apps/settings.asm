; SETTINGS.BIN - System Settings for UnoDOS
; Font size selector, color theme selector
;
; Build: nasm -f bin -o settings.bin settings.asm

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Settings', 0               ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    ; Gear/cog: magenta outer, cyan inner ring
    db 0x00, 0xA0, 0xA0, 0x00      ; Row 0:  magenta teeth top
    db 0x02, 0xA8, 0xA8, 0x80      ; Row 1:  magenta teeth
    db 0x02, 0x08, 0x20, 0x80      ; Row 2:  magenta frame
    db 0x28, 0x05, 0x50, 0x28      ; Row 3:  magenta+cyan ring
    db 0x28, 0x04, 0x10, 0x28      ; Row 4:  mag teeth, cyan ring
    db 0x20, 0x01, 0x40, 0x08      ; Row 5:  cyan inner ring
    db 0xA0, 0x05, 0x50, 0x0A      ; Row 6:  mag teeth, cyan ring
    db 0xA0, 0x04, 0x10, 0x0A      ; Row 7:  mag teeth, cyan center
    db 0xA0, 0x04, 0x10, 0x0A      ; Row 8:  mag teeth, cyan center
    db 0xA0, 0x05, 0x50, 0x0A      ; Row 9:  mag teeth, cyan ring
    db 0x20, 0x01, 0x40, 0x08      ; Row 10: cyan inner ring
    db 0x28, 0x04, 0x10, 0x28      ; Row 11: mag teeth, cyan ring
    db 0x28, 0x05, 0x50, 0x28      ; Row 12: magenta+cyan ring
    db 0x02, 0x08, 0x20, 0x80      ; Row 13: magenta frame
    db 0x02, 0xA8, 0xA8, 0x80      ; Row 14: magenta teeth
    db 0x00, 0xA0, 0xA0, 0x00      ; Row 15: magenta teeth bottom

    times 0x50 - ($ - $$) db 0     ; Pad to code entry at offset 0x50

; --- Code Entry (offset 0x50) ---

; API constants
API_GFX_DRAW_PIXEL      equ 0
API_GFX_DRAW_RECT       equ 1
API_GFX_DRAW_STRING     equ 4
API_GFX_CLEAR_AREA      equ 5
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_MOUSE_STATE         equ 28
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34
API_SET_FONT            equ 48
API_DRAW_STRING_WRAP    equ 50
API_DRAW_BUTTON         equ 51
API_DRAW_RADIO          equ 52
API_HIT_TEST            equ 53
API_FS_MOUNT            equ 13
API_FS_CLOSE            equ 16
API_GET_BOOT_DRIVE      equ 43
API_FS_CREATE           equ 45
API_FS_WRITE            equ 46
API_FS_DELETE           equ 47
API_SET_THEME           equ 54
API_GET_THEME           equ 55
API_GET_TICK            equ 63
API_FILLED_RECT_COLOR   equ 67
API_DELAY_TICKS         equ 73
API_GET_RTC_TIME        equ 72
API_SET_RTC_TIME        equ 81
API_GET_SCREEN_INFO     equ 82
API_BCD_TO_ASCII        equ 92
API_SET_VIDEO_MODE      equ 95

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Layout constants
WIN_X       equ 10
WIN_Y       equ 10
WIN_W       equ 300
WIN_H       equ 194

; Color swatch layout
SW_SIZE     equ 10                  ; Swatch width/height
SW_X0       equ 70                  ; Swatch X positions (10px wide, 4px gap)
SW_X1       equ 84
SW_X2       equ 98
SW_X3       equ 112

CLR_Y_TEXT  equ 78                  ; Text color row Y
CLR_Y_BG    equ 94                  ; Desktop bg row Y
CLR_Y_WIN   equ 110                 ; Window color row Y

; Display mode section (left column, below colors)
DISP_Y_LBL  equ 126                ; "Display:" label Y
DISP_Y_RAD1 equ 138                ; First row: CGA / VGA320
DISP_Y_RAD2 equ 150                ; Second row: VGA640 / VESA

BTN_Y       equ 164                 ; Button row Y (moved down for 4 radios)
BTN_DEF_X   equ 116                 ; Defaults button X
BTN_DEF_W   equ 72                  ; Defaults button width

; Time controls (right column, below word wrap)
TIME_X      equ 160                 ; Time controls X
TIME_Y_LBL  equ 108                 ; "Time:" label Y
TIME_Y_DISP equ 120                 ; HH:MM:SS display Y
TIME_Y_BTNS equ 134                 ; H+/H-/M+/M- buttons Y
TIME_Y_SET  equ 148                 ; Set Time button Y
TIME_BTN_W  equ 24                  ; Small button width
TIME_BTN_H  equ 12                  ; Small button height
TIME_BTN_GAP equ 4                  ; Gap between small buttons

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Load current theme from kernel
    mov ah, API_GET_THEME
    int 0x80
    mov [cs:cur_text_clr], al
    mov [cs:cur_bg_clr], bl
    mov [cs:cur_win_clr], cl

    ; Get current video mode from kernel
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [cs:cur_video_mode], al         ; AL = current mode (0x04 or 0x13)

    ; Load current RTC time
    mov ah, API_GET_RTC_TIME
    int 0x80
    mov [cs:cur_hours], ch
    mov [cs:cur_minutes], cl
    mov [cs:cur_seconds], dh

    ; Create window
    mov bx, WIN_X
    mov cx, WIN_Y
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

    call draw_ui

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; Check events
    mov ah, API_EVENT_GET
    int 0x80
    jc .check_mouse
    cmp al, EVENT_KEY_PRESS
    jne .check_redraw
    cmp dl, 27
    je .exit_ok
    cmp dl, '1'
    je .select_small
    cmp dl, '2'
    je .select_medium
    cmp dl, '3'
    je .select_large
    jmp .main_loop

.check_redraw:
    cmp al, EVENT_WIN_REDRAW
    jne .main_loop
    ; Refresh RTC time on repaint
    mov ah, API_GET_RTC_TIME
    int 0x80
    mov [cs:cur_hours], ch
    mov [cs:cur_minutes], cl
    mov [cs:cur_seconds], dh
    call draw_ui
    jmp .main_loop

.check_mouse:
    mov ah, API_MOUSE_STATE
    int 0x80
    test dl, 1
    jz .mouse_up
    cmp byte [cs:prev_btn], 0
    jne .main_loop
    mov byte [cs:prev_btn], 1

    ; Hit test font radio buttons
    mov bx, 4
    mov cx, 18
    mov dx, 120
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_small

    mov bx, 4
    mov cx, 32
    mov dx, 120
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_medium

    mov bx, 4
    mov cx, 46
    mov dx, 120
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_large

    ; Hit test text color swatches
    mov cx, CLR_Y_TEXT
    call hit_test_swatch_row
    jnc .set_text_clr

    ; Hit test bg color swatches
    mov cx, CLR_Y_BG
    call hit_test_swatch_row
    jnc .set_bg_clr

    ; Hit test window color swatches
    mov cx, CLR_Y_WIN
    call hit_test_swatch_row
    jnc .set_win_clr

    ; Apply button
    mov bx, 4
    mov cx, BTN_Y
    mov dx, 60
    mov si, 14
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .apply_all

    ; OK button (apply + close)
    mov bx, 70
    mov cx, BTN_Y
    mov dx, 40
    mov si, 14
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .ok_and_close

    ; Defaults button
    mov bx, BTN_DEF_X
    mov cx, BTN_Y
    mov dx, BTN_DEF_W
    mov si, 14
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .defaults

    ; --- Display mode radio hit tests ---
    ; CGA radio (row 1, left)
    mov bx, 4
    mov cx, DISP_Y_RAD1
    mov dx, 56
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_cga

    ; VGA radio (row 1, right)
    mov bx, 64
    mov cx, DISP_Y_RAD1
    mov dx, 56
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_vga

    ; VGA 640 radio (row 2, left)
    mov bx, 4
    mov cx, DISP_Y_RAD2
    mov dx, 56
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_640

    ; VESA radio (row 2, right)
    mov bx, 64
    mov cx, DISP_Y_RAD2
    mov dx, 56
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_vesa

    ; --- Time control hit tests ---
    ; H+ button
    mov bx, TIME_X
    mov cx, TIME_Y_BTNS
    mov dx, TIME_BTN_W
    mov si, TIME_BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .inc_hours

    ; H- button
    mov bx, TIME_X + TIME_BTN_W + TIME_BTN_GAP
    mov cx, TIME_Y_BTNS
    mov dx, TIME_BTN_W
    mov si, TIME_BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .dec_hours

    ; M+ button
    mov bx, TIME_X + (TIME_BTN_W + TIME_BTN_GAP) * 2
    mov cx, TIME_Y_BTNS
    mov dx, TIME_BTN_W
    mov si, TIME_BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .inc_minutes

    ; M- button
    mov bx, TIME_X + (TIME_BTN_W + TIME_BTN_GAP) * 3
    mov cx, TIME_Y_BTNS
    mov dx, TIME_BTN_W
    mov si, TIME_BTN_H
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .dec_minutes

    ; Set Time button
    mov bx, TIME_X
    mov cx, TIME_Y_SET
    mov dx, 80
    mov si, 14
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .set_time

    jmp .main_loop

.mouse_up:
    mov byte [cs:prev_btn], 0
    jmp .main_loop

.select_small:
    mov byte [cs:cur_font], 0
    call draw_ui
    jmp .main_loop

.select_medium:
    mov byte [cs:cur_font], 1
    call draw_ui
    jmp .main_loop

.select_large:
    mov byte [cs:cur_font], 2
    call draw_ui
    jmp .main_loop

.set_text_clr:
    mov [cs:cur_text_clr], al
    call draw_ui
    jmp .main_loop

.set_bg_clr:
    mov [cs:cur_bg_clr], al
    call draw_ui
    jmp .main_loop

.set_win_clr:
    mov [cs:cur_win_clr], al
    call draw_ui
    jmp .main_loop

.select_cga:
    mov byte [cs:cur_video_mode], 0x04
    call draw_ui
    jmp .main_loop

.select_vga:
    mov byte [cs:cur_video_mode], 0x13
    call draw_ui
    jmp .main_loop

.select_640:
    mov byte [cs:cur_video_mode], 0x12
    call draw_ui
    jmp .main_loop

.select_vesa:
    mov byte [cs:cur_video_mode], 0x01
    call draw_ui
    jmp .main_loop

.defaults:
    mov byte [cs:cur_font], 1
    mov byte [cs:cur_text_clr], 3
    mov byte [cs:cur_bg_clr], 0
    mov byte [cs:cur_win_clr], 3
    mov byte [cs:cur_video_mode], 0x04
    call draw_ui
    jmp .main_loop

.inc_hours:
    call bcd_inc_hours
    call draw_time_section
    jmp .main_loop

.dec_hours:
    call bcd_dec_hours
    call draw_time_section
    jmp .main_loop

.inc_minutes:
    call bcd_inc_minutes
    call draw_time_section
    jmp .main_loop

.dec_minutes:
    call bcd_dec_minutes
    call draw_time_section
    jmp .main_loop

.set_time:
    mov ch, [cs:cur_hours]
    mov cl, [cs:cur_minutes]
    mov dh, [cs:cur_seconds]
    mov ah, API_SET_RTC_TIME
    int 0x80
    jmp .main_loop

.apply_all:
    call apply_with_revert
    call draw_ui
    jmp .main_loop

.ok_and_close:
    call apply_with_revert
    ; Fall through to exit

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
; Hit test a row of 4 color swatches
; Input: CX = row Y
; Output: AL = color (0-3), CF=0 on hit; CF=1 on miss
; Preserves: CX
; ============================================================================
hit_test_swatch_row:
    push bx
    push dx
    push si

    mov dx, SW_SIZE
    mov si, SW_SIZE

    mov bx, SW_X0
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .sw_hit_0

    mov bx, SW_X1
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .sw_hit_1

    mov bx, SW_X2
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .sw_hit_2

    mov bx, SW_X3
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .sw_hit_3

    stc                             ; No hit
    pop si
    pop dx
    pop bx
    ret

.sw_hit_0:
    xor al, al
    jmp .sw_done
.sw_hit_1:
    mov al, 1
    jmp .sw_done
.sw_hit_2:
    mov al, 2
    jmp .sw_done
.sw_hit_3:
    mov al, 3
.sw_done:
    clc
    pop si
    pop dx
    pop bx
    ret

; ============================================================================
; Draw the complete UI
; ============================================================================
draw_ui:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    ; Clear content area
    mov bx, 0
    mov cx, 0
    mov dx, 296
    mov si, 178
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; === Font selection (left column) ===
    mov si, lbl_font_size
    mov bx, 4
    mov cx, 4
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Radio: Small (4x6)
    mov si, lbl_small
    mov bx, 4
    mov cx, 18
    xor al, al
    cmp byte [cs:cur_font], 0
    jne .r1
    mov al, 1
.r1:
    mov ah, API_DRAW_RADIO
    int 0x80

    ; Radio: Medium (8x8)
    mov si, lbl_medium
    mov bx, 4
    mov cx, 32
    xor al, al
    cmp byte [cs:cur_font], 1
    jne .r2
    mov al, 1
.r2:
    mov ah, API_DRAW_RADIO
    int 0x80

    ; Radio: Large (8x12)
    mov si, lbl_large
    mov bx, 4
    mov cx, 46
    xor al, al
    cmp byte [cs:cur_font], 2
    jne .r3
    mov al, 1
.r3:
    mov ah, API_DRAW_RADIO
    int 0x80

    ; === Preview (right column) ===
    mov si, lbl_preview
    mov bx, 160
    mov cx, 4
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Small font sample
    xor al, al
    mov ah, API_SET_FONT
    int 0x80
    mov si, lbl_sample_s
    mov bx, 160
    mov cx, 18
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Medium font sample
    mov al, 1
    mov ah, API_SET_FONT
    int 0x80
    mov si, lbl_sample_m
    mov bx, 160
    mov cx, 30
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Large font sample
    mov al, 2
    mov ah, API_SET_FONT
    int 0x80
    mov si, lbl_sample_l
    mov bx, 160
    mov cx, 44
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Restore current font
    mov al, [cs:cur_font]
    mov ah, API_SET_FONT
    int 0x80

    ; === Word wrap demo (right column) ===
    mov si, lbl_wrap
    mov bx, 160
    mov cx, 64
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov si, lbl_wrap_text
    mov bx, 160
    mov cx, 78
    mov dx, 130
    mov ah, API_DRAW_STRING_WRAP
    int 0x80

    ; === Color selection (left column, below fonts) ===
    mov si, lbl_colors
    mov bx, 4
    mov cx, 64
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Text color row
    mov si, lbl_text
    mov bx, 8
    mov cx, CLR_Y_TEXT
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov al, [cs:cur_text_clr]
    mov cx, CLR_Y_TEXT
    call draw_swatch_row

    ; Desktop bg row
    mov si, lbl_desktop
    mov bx, 8
    mov cx, CLR_Y_BG
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov al, [cs:cur_bg_clr]
    mov cx, CLR_Y_BG
    call draw_swatch_row

    ; Window color row
    mov si, lbl_window
    mov bx, 8
    mov cx, CLR_Y_WIN
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov al, [cs:cur_win_clr]
    mov cx, CLR_Y_WIN
    call draw_swatch_row

    ; === Display mode (left column, below colors) ===
    mov si, lbl_display
    mov bx, 4
    mov cx, DISP_Y_LBL
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; CGA radio (row 1, left)
    mov si, lbl_cga
    mov bx, 4
    mov cx, DISP_Y_RAD1
    xor al, al
    cmp byte [cs:cur_video_mode], 0x04
    jne .rd1
    mov al, 1
.rd1:
    mov ah, API_DRAW_RADIO
    int 0x80

    ; VGA radio (row 1, right)
    mov si, lbl_vga
    mov bx, 64
    mov cx, DISP_Y_RAD1
    xor al, al
    cmp byte [cs:cur_video_mode], 0x13
    jne .rd2
    mov al, 1
.rd2:
    mov ah, API_DRAW_RADIO
    int 0x80

    ; VGA 640 radio (row 2, left)
    mov si, lbl_vga640
    mov bx, 4
    mov cx, DISP_Y_RAD2
    xor al, al
    cmp byte [cs:cur_video_mode], 0x12
    jne .rd3
    mov al, 1
.rd3:
    mov ah, API_DRAW_RADIO
    int 0x80

    ; VESA radio (row 2, right)
    mov si, lbl_vesa
    mov bx, 64
    mov cx, DISP_Y_RAD2
    xor al, al
    cmp byte [cs:cur_video_mode], 0x01
    jne .rd4
    mov al, 1
.rd4:
    mov ah, API_DRAW_RADIO
    int 0x80

    ; === Buttons ===
    mov ax, cs
    mov es, ax
    mov bx, 4
    mov cx, BTN_Y
    mov dx, 60
    mov si, 14
    mov di, lbl_apply
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    mov ax, cs
    mov es, ax
    mov bx, 70
    mov cx, BTN_Y
    mov dx, 40
    mov si, 14
    mov di, lbl_ok
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    mov ax, cs
    mov es, ax
    mov bx, BTN_DEF_X
    mov cx, BTN_Y
    mov dx, BTN_DEF_W
    mov si, 14
    mov di, lbl_defaults
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; === Time controls (right column, below word wrap) ===
    call draw_time_section

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Draw a row of 4 color swatches with selection indicator
; Input: AL = selected color (0-3), CX = row Y
; ============================================================================
draw_swatch_row:
    push ax
    push bx
    push cx
    push dx
    push si
    mov [cs:sel_color], al
    mov [cs:row_y], cx

    ; Swatch 0 (black) - outline only since fill is invisible on black bg
    mov bx, SW_X0
    mov cx, [cs:row_y]
    mov dx, SW_SIZE
    mov si, SW_SIZE
    mov ah, API_GFX_DRAW_RECT
    int 0x80

    ; Swatch 1 (cyan) - fill with color 1
    mov byte [cs:swatch_clr], 1
    mov word [cs:swatch_sx], SW_X1
    call draw_one_swatch

    ; Swatch 2 (magenta) - fill with color 2
    mov byte [cs:swatch_clr], 2
    mov word [cs:swatch_sx], SW_X2
    call draw_one_swatch

    ; Swatch 3 (white) - fill with color 3
    mov byte [cs:swatch_clr], 3
    mov word [cs:swatch_sx], SW_X3
    call draw_one_swatch

    ; Draw selection indicator (border around selected swatch)
    mov al, [cs:sel_color]
    xor ah, ah
    mov bx, SW_SIZE + 4            ; 14 = swatch pitch
    mul bx                         ; AX = color * 14
    add ax, SW_X0 - 2              ; Offset to 2px outside swatch
    mov bx, ax
    mov cx, [cs:row_y]
    sub cx, 2
    mov dx, SW_SIZE + 4            ; 14
    mov si, SW_SIZE + 4            ; 14
    mov ah, API_GFX_DRAW_RECT
    int 0x80

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Draw one filled color swatch (pixel by pixel)
; Uses: swatch_clr, swatch_sx, row_y
; ============================================================================
draw_one_swatch:
    push ax
    push bx
    push cx
    push dx
    push si
    mov bx, [cs:swatch_sx]
    mov cx, [cs:row_y]
    mov dx, SW_SIZE
    mov si, SW_SIZE
    mov al, [cs:swatch_clr]
    mov ah, API_FILLED_RECT_COLOR
    int 0x80
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Apply current settings to kernel (font + colors)
; ============================================================================
apply_settings:
    push ax
    push bx
    push cx
    push dx

    ; Apply font
    mov al, [cs:cur_font]
    mov ah, API_SET_FONT
    int 0x80

    ; Apply theme colors
    mov al, [cs:cur_text_clr]
    mov bl, [cs:cur_bg_clr]
    mov cl, [cs:cur_win_clr]
    mov ah, API_SET_THEME
    int 0x80

    ; Apply video mode if changed
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    cmp al, [cs:cur_video_mode]
    je .as_mode_ok
    mov al, [cs:cur_video_mode]
    mov ah, API_SET_VIDEO_MODE
    int 0x80
.as_mode_ok:

    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Apply settings with revert countdown if video mode changes
; If mode unchanged, applies immediately and saves.
; If mode changed, shows 10-second countdown with Keep/Revert buttons.
; ============================================================================
apply_with_revert:
    pusha

    ; Check if video mode is actually changing
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [cs:saved_video_mode], al       ; Save current mode
    cmp al, [cs:cur_video_mode]
    jne .awr_mode_changing

    ; No mode change — just apply and save
    call apply_settings
    call save_settings
    popa
    ret

.awr_mode_changing:
    ; Mode is changing — apply with countdown
    call apply_settings                  ; Switches mode + font + colors

    ; Initialize countdown
    mov byte [cs:revert_countdown], 10
    mov byte [cs:revert_prev_btn], 0
    call update_countdown_str
    call draw_countdown_ui

    mov word [cs:revert_ticks], 0

.awr_poll:
    sti
    mov cx, 1                           ; Wait 1 tick (~55ms)
    mov ah, API_DELAY_TICKS
    int 0x80

    ; Check for WIN_REDRAW events
    mov ah, API_EVENT_GET
    int 0x80
    jc .awr_check_mouse
    cmp al, EVENT_WIN_REDRAW
    jne .awr_check_mouse
    call draw_countdown_ui

.awr_check_mouse:
    mov ah, API_MOUSE_STATE
    int 0x80
    test dl, 1
    jz .awr_mouse_up
    cmp byte [cs:revert_prev_btn], 0
    jne .awr_tick
    mov byte [cs:revert_prev_btn], 1

    ; Hit test "Keep" button (at 40, 60, w=60, h=14)
    mov bx, 40
    mov cx, 60
    mov dx, 60
    mov si, 14
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .awr_keep

    ; Hit test "Revert" button (at 110, 60, w=60, h=14)
    mov bx, 110
    mov cx, 60
    mov dx, 60
    mov si, 14
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .awr_revert
    jmp .awr_tick

.awr_mouse_up:
    mov byte [cs:revert_prev_btn], 0

.awr_tick:
    inc word [cs:revert_ticks]
    cmp word [cs:revert_ticks], 18      ; 18 ticks ≈ 1 second
    jb .awr_poll
    mov word [cs:revert_ticks], 0
    dec byte [cs:revert_countdown]
    jz .awr_revert
    call update_countdown_str
    call draw_countdown_ui
    jmp .awr_poll

.awr_keep:
    ; User confirmed — save settings to disk
    call save_settings
    popa
    ret

.awr_revert:
    ; Timer expired or user clicked Revert — restore old mode
    mov al, [cs:saved_video_mode]
    mov [cs:cur_video_mode], al
    mov ah, API_SET_VIDEO_MODE
    int 0x80
    ; Re-apply font and colors (mode switch resets draw colors)
    mov al, [cs:cur_font]
    mov ah, API_SET_FONT
    int 0x80
    mov al, [cs:cur_text_clr]
    mov bl, [cs:cur_bg_clr]
    mov cl, [cs:cur_win_clr]
    mov ah, API_SET_THEME
    int 0x80
    popa
    ret

; ============================================================================
; Draw the revert countdown UI inside the Settings window
; ============================================================================
draw_countdown_ui:
    pusha

    ; Clear content area
    mov bx, 0
    mov cx, 0
    mov dx, 296
    mov si, 178
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; "Keep these settings?"
    mov bx, 20
    mov cx, 14
    mov si, lbl_keep_q
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; "Reverting in NN..."
    mov bx, 20
    mov cx, 34
    mov si, lbl_reverting
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw "Keep" button
    mov ax, cs
    mov es, ax
    mov bx, 40
    mov cx, 60
    mov dx, 60
    mov si, 14
    mov di, lbl_keep_btn
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; Draw "Revert" button
    mov ax, cs
    mov es, ax
    mov bx, 110
    mov cx, 60
    mov dx, 60
    mov si, 14
    mov di, lbl_revert_btn
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    popa
    ret

; ============================================================================
; Update the countdown digit(s) in the reverting string
; ============================================================================
update_countdown_str:
    push ax
    push bx

    mov al, [cs:revert_countdown]
    xor ah, ah
    cmp al, 10
    jb .ucs_single
    ; Two digits (10)
    mov byte [cs:countdown_d10], '1'
    sub al, 10
    add al, '0'
    mov [cs:countdown_d1], al
    jmp .ucs_done
.ucs_single:
    mov byte [cs:countdown_d10], ' '
    add al, '0'
    mov [cs:countdown_d1], al
.ucs_done:
    pop bx
    pop ax
    ret

; ============================================================================
; Save settings to SETTINGS.CFG on boot drive
; ============================================================================
save_settings:
    push ax
    push bx
    push cx
    push dx
    push si
    push es

    ; Get boot drive and mount filesystem
    mov ah, API_GET_BOOT_DRIVE
    int 0x80
    mov ah, API_FS_MOUNT
    int 0x80
    ; BX = mount handle (0=FAT12, 1=FAT16)
    mov [cs:cfg_mount], bl

    ; Delete existing SETTINGS.CFG (ignore error if not found)
    mov si, cfg_filename
    mov bl, [cs:cfg_mount]
    mov ah, API_FS_DELETE
    int 0x80

    ; Create new SETTINGS.CFG
    mov si, cfg_filename
    mov bl, [cs:cfg_mount]
    mov ah, API_FS_CREATE
    int 0x80
    jc .ss_done
    mov [cs:cfg_fh], al

    ; Build 6-byte settings buffer
    mov byte [cs:cfg_buf], 0xA5        ; Magic byte
    mov al, [cs:cur_font]
    mov [cs:cfg_buf + 1], al
    mov al, [cs:cur_text_clr]
    mov [cs:cfg_buf + 2], al
    mov al, [cs:cur_bg_clr]
    mov [cs:cfg_buf + 3], al
    mov al, [cs:cur_win_clr]
    mov [cs:cfg_buf + 4], al
    mov al, [cs:cur_video_mode]
    mov [cs:cfg_buf + 5], al

    ; Write 6 bytes
    mov ax, cs
    mov es, ax
    mov bx, cfg_buf
    mov cx, 6
    mov al, [cs:cfg_fh]
    mov ah, API_FS_WRITE
    int 0x80

    ; Close file
    mov al, [cs:cfg_fh]
    mov ah, API_FS_CLOSE
    int 0x80

.ss_done:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Draw time section (label + display + buttons)
; ============================================================================
draw_time_section:
    pusha

    ; Clear the time display area only (avoid full redraw flicker)
    mov bx, TIME_X
    mov cx, TIME_Y_LBL
    mov dx, 130
    mov si, 56
    mov ah, API_GFX_CLEAR_AREA
    int 0x80

    ; "Time:" label
    mov si, lbl_time
    mov bx, TIME_X
    mov cx, TIME_Y_LBL
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Format and display HH:MM:SS
    call format_time_string
    mov si, time_str
    mov bx, TIME_X
    mov cx, TIME_Y_DISP
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw H+/H-/M+/M- buttons using small font
    xor al, al                      ; Font 0 (4x6)
    mov ah, API_SET_FONT
    int 0x80

    mov ax, cs
    mov es, ax

    ; H+ button
    mov bx, TIME_X
    mov cx, TIME_Y_BTNS
    mov dx, TIME_BTN_W
    mov si, TIME_BTN_H
    mov di, lbl_h_up
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; H- button
    mov ax, cs
    mov es, ax
    mov bx, TIME_X + TIME_BTN_W + TIME_BTN_GAP
    mov cx, TIME_Y_BTNS
    mov dx, TIME_BTN_W
    mov si, TIME_BTN_H
    mov di, lbl_h_dn
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; M+ button
    mov ax, cs
    mov es, ax
    mov bx, TIME_X + (TIME_BTN_W + TIME_BTN_GAP) * 2
    mov cx, TIME_Y_BTNS
    mov dx, TIME_BTN_W
    mov si, TIME_BTN_H
    mov di, lbl_m_up
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; M- button
    mov ax, cs
    mov es, ax
    mov bx, TIME_X + (TIME_BTN_W + TIME_BTN_GAP) * 3
    mov cx, TIME_Y_BTNS
    mov dx, TIME_BTN_W
    mov si, TIME_BTN_H
    mov di, lbl_m_dn
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; Restore user's font
    mov al, [cs:cur_font]
    mov ah, API_SET_FONT
    int 0x80

    ; Set Time button (default font)
    mov ax, cs
    mov es, ax
    mov bx, TIME_X
    mov cx, TIME_Y_SET
    mov dx, 80
    mov si, 14
    mov di, lbl_set_time
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    popa
    ret

; ============================================================================
; BCD time helpers
; ============================================================================

; format_time_string - Format BCD time into "HH:MM:SS" ASCII
format_time_string:
    push ax
    push di

    mov di, time_str

    mov al, [cs:cur_hours]
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [cs:di], ah
    mov [cs:di+1], al
    mov byte [cs:di+2], ':'

    mov al, [cs:cur_minutes]
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [cs:di+3], ah
    mov [cs:di+4], al
    mov byte [cs:di+5], ':'

    mov al, [cs:cur_seconds]
    mov ah, API_BCD_TO_ASCII
    int 0x80
    mov [cs:di+6], ah
    mov [cs:di+7], al

    pop di
    pop ax
    ret

; bcd_inc_hours - Increment hours (BCD), wrap 23→00
bcd_inc_hours:
    push ax
    mov al, [cs:cur_hours]
    add al, 1
    daa                             ; Decimal adjust after addition
    cmp al, 0x24
    jb .bih_ok
    xor al, al
.bih_ok:
    mov [cs:cur_hours], al
    pop ax
    ret

; bcd_dec_hours - Decrement hours (BCD), wrap 00→23
bcd_dec_hours:
    push ax
    mov al, [cs:cur_hours]
    test al, al
    jnz .bdh_dec
    mov al, 0x23
    jmp .bdh_done
.bdh_dec:
    sub al, 1
    das                             ; Decimal adjust after subtraction
.bdh_done:
    mov [cs:cur_hours], al
    pop ax
    ret

; bcd_inc_minutes - Increment minutes (BCD), wrap 59→00
bcd_inc_minutes:
    push ax
    mov al, [cs:cur_minutes]
    add al, 1
    daa
    cmp al, 0x60
    jb .bim_ok
    xor al, al
.bim_ok:
    mov [cs:cur_minutes], al
    pop ax
    ret

; bcd_dec_minutes - Decrement minutes (BCD), wrap 00→59
bcd_dec_minutes:
    push ax
    mov al, [cs:cur_minutes]
    test al, al
    jnz .bdm_dec
    mov al, 0x59
    jmp .bdm_done
.bdm_dec:
    sub al, 1
    das
.bdm_done:
    mov [cs:cur_minutes], al
    pop ax
    ret

; ============================================================================
; Strings
; ============================================================================
window_title:       db 'Settings', 0
lbl_font_size:      db 'Font Size:', 0
lbl_small:          db 'Small 4x6', 0
lbl_medium:         db 'Medium 8x8', 0
lbl_large:          db 'Large 8x12', 0
lbl_apply:          db 'Apply', 0
lbl_ok:             db 'OK', 0
lbl_defaults:       db 'Defaults', 0
cfg_filename:       db 'SETTINGS.CFG', 0
lbl_preview:        db 'Preview:', 0
lbl_sample_s:       db 'Small font text', 0
lbl_sample_m:       db 'Medium font', 0
lbl_sample_l:       db 'Large font', 0
lbl_colors:         db 'Colors:', 0
lbl_text:           db 'Text:', 0
lbl_desktop:        db 'Desk:', 0
lbl_window:         db 'Win:', 0
lbl_wrap:           db 'Word Wrap:', 0
lbl_wrap_text:      db 'This text wraps at the edge.', 0
lbl_time:           db 'Time:', 0
lbl_set_time:       db 'Set Time', 0
lbl_h_up:           db 'H+', 0
lbl_h_dn:           db 'H-', 0
lbl_m_up:           db 'M+', 0
lbl_m_dn:           db 'M-', 0
lbl_display:        db 'Display:', 0
lbl_cga:            db 'CGA', 0
lbl_vga:            db 'VGA', 0
lbl_vga640:         db 'VGA640', 0
lbl_vesa:           db 'SVGA', 0
lbl_keep_q:         db 'Keep these settings?', 0
lbl_reverting:      db 'Reverting in '
countdown_d10:      db '1'
countdown_d1:       db '0'
                    db '...', 0
lbl_keep_btn:       db 'Keep', 0
lbl_revert_btn:     db 'Revert', 0
; ============================================================================
; Variables
; ============================================================================
win_handle:     db 0
cur_font:       db 1
prev_btn:       db 0
cur_text_clr:   db 3                ; Current text color selection
cur_bg_clr:     db 0                ; Current desktop bg selection
cur_win_clr:    db 3                ; Current window color selection
sel_color:      db 0                ; Currently selected color in draw_swatch_row
row_y:          dw 0                ; Row Y for draw_swatch_row
swatch_clr:     db 0                ; Fill color for draw_one_swatch
swatch_sx:      dw 0                ; Start X for draw_one_swatch
swatch_row:     dw 0                ; Current row in draw_one_swatch
swatch_col:     dw 0                ; Current col in draw_one_swatch
cfg_fh:         db 0                ; File handle for settings save
cfg_buf:        times 6 db 0        ; Settings buffer (magic + 5 settings bytes)
cur_video_mode: db 0x04             ; Current video mode (0x04=CGA, 0x13=VGA, 0x12=Mode12h, 0x01=VESA)
cfg_mount:      db 0                ; Mount handle for settings save
saved_video_mode: db 0              ; Previous mode for revert
revert_countdown: db 10             ; Countdown seconds remaining
revert_ticks:   dw 0                ; Tick counter within current second
revert_prev_btn: db 0               ; Mouse button state for countdown
cur_hours:      db 0                ; Current hours (BCD)
cur_minutes:    db 0                ; Current minutes (BCD)
cur_seconds:    db 0                ; Current seconds (BCD)
time_str:       db '00:00:00', 0    ; Time display buffer
