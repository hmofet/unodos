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
    ; Gear/cog shape
    db 0x00, 0xF0, 0xF0, 0x00      ; Row 0
    db 0x03, 0xFC, 0xFC, 0xC0      ; Row 1
    db 0x03, 0x0C, 0x30, 0xC0      ; Row 2
    db 0x3C, 0x0C, 0x30, 0x3C      ; Row 3
    db 0x3C, 0x00, 0x00, 0x3C      ; Row 4
    db 0x30, 0x03, 0xC0, 0x0C      ; Row 5
    db 0xF0, 0x0F, 0xF0, 0x0F      ; Row 6
    db 0xF0, 0x0C, 0x30, 0x0F      ; Row 7
    db 0xF0, 0x0C, 0x30, 0x0F      ; Row 8
    db 0xF0, 0x0F, 0xF0, 0x0F      ; Row 9
    db 0x30, 0x03, 0xC0, 0x0C      ; Row 10
    db 0x3C, 0x00, 0x00, 0x3C      ; Row 11
    db 0x3C, 0x0C, 0x30, 0x3C      ; Row 12
    db 0x03, 0x0C, 0x30, 0xC0      ; Row 13
    db 0x03, 0xFC, 0xFC, 0xC0      ; Row 14
    db 0x00, 0xF0, 0xF0, 0x00      ; Row 15

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

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Layout constants
WIN_X       equ 10
WIN_Y       equ 10
WIN_W       equ 300
WIN_H       equ 180

; Color swatch layout
SW_SIZE     equ 10                  ; Swatch width/height
SW_X0       equ 70                  ; Swatch X positions (10px wide, 4px gap)
SW_X1       equ 84
SW_X2       equ 98
SW_X3       equ 112

CLR_Y_TEXT  equ 78                  ; Text color row Y
CLR_Y_BG    equ 94                  ; Desktop bg row Y
CLR_Y_WIN   equ 110                 ; Window color row Y

BTN_Y       equ 136                 ; Button row Y
BTN_DEF_X   equ 116                 ; Defaults button X
BTN_DEF_W   equ 72                  ; Defaults button width

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

    ; Load current theme colors from kernel
    mov ah, API_GET_THEME
    int 0x80
    mov [cs:cur_text_clr], al
    mov [cs:cur_bg_clr], bl
    mov [cs:cur_win_clr], cl

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

.defaults:
    mov byte [cs:cur_font], 1
    mov byte [cs:cur_text_clr], 3
    mov byte [cs:cur_bg_clr], 0
    mov byte [cs:cur_win_clr], 3
    call draw_ui
    jmp .main_loop

.apply_all:
    call apply_settings
    call save_settings
    call draw_ui
    jmp .main_loop

.ok_and_close:
    call apply_settings
    call save_settings
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
    mov si, 162
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
    mov cx, SW_SIZE                 ; row count
    mov dx, [cs:row_y]
    mov [cs:swatch_row], dx
.ds_row:
    push cx
    mov cx, SW_SIZE                 ; col count
    mov word [cs:swatch_col], 0
.ds_col:
    push cx
    mov bx, [cs:swatch_sx]
    add bx, [cs:swatch_col]
    mov cx, [cs:swatch_row]
    mov al, [cs:swatch_clr]
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80
    inc word [cs:swatch_col]
    pop cx
    loop .ds_col
    inc word [cs:swatch_row]
    pop cx
    loop .ds_row
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
    mov al, [cs:cur_font]
    mov ah, API_SET_FONT
    int 0x80
    mov al, [cs:cur_text_clr]
    mov bl, [cs:cur_bg_clr]
    mov cl, [cs:cur_win_clr]
    mov ah, API_SET_THEME
    int 0x80
    pop cx
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

    ; Delete existing SETTINGS.CFG (ignore error if not found)
    mov si, cfg_filename
    mov bl, 0
    mov ah, API_FS_DELETE
    int 0x80

    ; Create new SETTINGS.CFG
    mov si, cfg_filename
    mov bl, 0
    mov ah, API_FS_CREATE
    int 0x80
    jc .ss_done
    mov [cs:cfg_fh], al

    ; Build 5-byte settings buffer
    mov byte [cs:cfg_buf], 0xA5        ; Magic byte
    mov al, [cs:cur_font]
    mov [cs:cfg_buf + 1], al
    mov al, [cs:cur_text_clr]
    mov [cs:cfg_buf + 2], al
    mov al, [cs:cur_bg_clr]
    mov [cs:cfg_buf + 3], al
    mov al, [cs:cur_win_clr]
    mov [cs:cfg_buf + 4], al

    ; Write 5 bytes
    mov ax, cs
    mov es, ax
    mov bx, cfg_buf
    mov cx, 5
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
cfg_buf:        times 5 db 0        ; Settings buffer (magic + 4 bytes)
