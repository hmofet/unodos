; SETTINGS.BIN - System Settings for UnoDOS
; Font size selector, UI widget showcase
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

EVENT_KEY_PRESS         equ 1
EVENT_WIN_REDRAW        equ 6

; Layout constants
WIN_X       equ 10
WIN_Y       equ 10
WIN_W       equ 300
WIN_H       equ 180

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax

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

    ; Hit test radio buttons (left column)
    mov bx, 4
    mov cx, 24
    mov dx, 120
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_small

    mov bx, 4
    mov cx, 40
    mov dx, 120
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_medium

    mov bx, 4
    mov cx, 56
    mov dx, 120
    mov si, 12
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .select_large

    ; Apply button
    mov bx, 4
    mov cx, 76
    mov dx, 80
    mov si, 16
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .apply_font

    ; OK button (apply + close)
    mov bx, 92
    mov cx, 76
    mov dx, 50
    mov si, 16
    mov ah, API_HIT_TEST
    int 0x80
    test al, al
    jnz .ok_and_close

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

.apply_font:
    mov al, [cs:cur_font]
    mov ah, API_SET_FONT
    int 0x80
    call draw_ui
    jmp .main_loop

.ok_and_close:
    mov al, [cs:cur_font]
    mov ah, API_SET_FONT
    int 0x80
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

    ; === Left column: Font selection ===
    mov si, lbl_font_size
    mov bx, 4
    mov cx, 4
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Radio: Small (4x6)
    mov si, lbl_small
    mov bx, 4
    mov cx, 24
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
    mov cx, 40
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
    mov cx, 56
    xor al, al
    cmp byte [cs:cur_font], 2
    jne .r3
    mov al, 1
.r3:
    mov ah, API_DRAW_RADIO
    int 0x80

    ; Apply button
    mov ax, cs
    mov es, ax
    mov bx, 4
    mov cx, 76
    mov dx, 80
    mov si, 16
    mov di, lbl_apply
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; OK button
    mov ax, cs
    mov es, ax
    mov bx, 92
    mov cx, 76
    mov dx, 50
    mov si, 16
    mov di, lbl_ok
    xor al, al
    mov ah, API_DRAW_BUTTON
    int 0x80

    ; === Right column: Preview ===
    mov si, lbl_preview
    mov bx, 150
    mov cx, 4
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Small font sample
    xor al, al
    mov ah, API_SET_FONT
    int 0x80
    mov si, lbl_sample_s
    mov bx, 150
    mov cx, 24
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Medium font sample
    mov al, 1
    mov ah, API_SET_FONT
    int 0x80
    mov si, lbl_sample_m
    mov bx, 150
    mov cx, 36
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Large font sample
    mov al, 2
    mov ah, API_SET_FONT
    int 0x80
    mov si, lbl_sample_l
    mov bx, 150
    mov cx, 52
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Restore current font
    mov al, [cs:cur_font]
    mov ah, API_SET_FONT
    int 0x80

    ; === Word wrap demo ===
    mov si, lbl_wrap
    mov bx, 150
    mov cx, 76
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov si, lbl_wrap_text
    mov bx, 150
    mov cx, 90
    mov dx, 140
    mov ah, API_DRAW_STRING_WRAP
    int 0x80

    ; === Color pixel demo ===
    mov si, lbl_colors
    mov bx, 4
    mov cx, 100
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw colored rectangles using pixel API
    ; Color 1 (cyan): 8x8 block at (4, 114)
    call draw_swatches

    ; Color labels
    mov si, lbl_clabels
    mov bx, 4
    mov cx, 128
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Draw 4 color swatches using pixel API
; ============================================================================
draw_swatches:
    push ax
    push bx
    push cx

    ; Draw 4 swatches: colors 0-3, 10x10 each with 2px gap
    ; Swatch 0 (black) - outline only
    mov bx, 4
    mov cx, 114
    mov dx, 10
    mov si, 10
    mov ah, API_GFX_DRAW_RECT
    int 0x80

    ; Swatch 1 (cyan) - fill with color 1
    mov byte [cs:swatch_clr], 1
    mov word [cs:swatch_sx], 18
    call draw_one_swatch

    ; Swatch 2 (magenta) - fill with color 2
    mov byte [cs:swatch_clr], 2
    mov word [cs:swatch_sx], 32
    call draw_one_swatch

    ; Swatch 3 (white) - fill with color 3
    mov byte [cs:swatch_clr], 3
    mov word [cs:swatch_sx], 46
    call draw_one_swatch

    pop cx
    pop bx
    pop ax
    ret

draw_one_swatch:
    push ax
    push bx
    push cx
    push dx
    ; Fill 8x8 block at (swatch_sx, 115) with swatch_clr
    mov cx, 8                       ; row count
    mov word [cs:swatch_row], 115
.dos_row:
    push cx
    mov cx, 8                       ; col count
    mov word [cs:swatch_col], 0
.dos_col:
    push cx
    ; Draw pixel: BX=X, CX=Y, AL=color
    mov bx, [cs:swatch_sx]
    add bx, [cs:swatch_col]
    mov cx, [cs:swatch_row]
    mov al, [cs:swatch_clr]
    mov ah, API_GFX_DRAW_PIXEL
    int 0x80
    inc word [cs:swatch_col]
    pop cx
    loop .dos_col
    inc word [cs:swatch_row]
    pop cx
    loop .dos_row
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Strings
; ============================================================================
window_title:       db 'Settings', 0
lbl_font_size:      db 'Font Size (1/2/3):', 0
lbl_small:          db 'Small 4x6', 0
lbl_medium:         db 'Medium 8x8', 0
lbl_large:          db 'Large 8x12', 0
lbl_apply:          db 'Apply', 0
lbl_ok:             db 'OK', 0
lbl_preview:        db 'Preview:', 0
lbl_sample_s:       db 'Small font text', 0
lbl_sample_m:       db 'Medium font', 0
lbl_sample_l:       db 'Large font', 0
lbl_wrap:           db 'Word Wrap:', 0
lbl_wrap_text:      db 'This text wraps automatically when it reaches the edge.', 0
lbl_colors:         db 'CGA Colors:', 0
lbl_clabels:        db 'Bk Cy Mg Wh', 0

; ============================================================================
; Variables
; ============================================================================
win_handle:     db 0
cur_font:       db 1
prev_btn:       db 0
swatch_clr:     db 0
swatch_sx:      dw 0
swatch_row:     dw 0
swatch_col:     dw 0
