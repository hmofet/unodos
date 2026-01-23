; UnoDOS Kernel
; Loaded at 0x1000:0000 (linear 0x10000 = 64KB)
; Main operating system code

[BITS 16]
[ORG 0x0000]

; ============================================================================
; Signature and Entry Point
; ============================================================================

signature:
    dw 0x4B55                       ; 'UK' signature for kernel verification

entry:
    ; Set up segment registers for our location
    mov ax, 0x1000
    mov ds, ax
    mov es, ax

    ; Set up graphics mode (blue screen)
    call setup_graphics

    ; Draw welcome box with text
    call draw_welcome_box

    ; Halt
halt_loop:
    hlt
    jmp halt_loop

; ============================================================================
; Graphics Setup
; ============================================================================

setup_graphics:
    ; Set CGA 320x200 4-color mode (mode 4)
    mov ah, 0x00
    mov al, 0x04                    ; 320x200, 4 colors
    int 0x10

    ; Clear all CGA video memory explicitly
    ; Mode 4 uses 0xB800:0000-0x3FFF (16KB)
    push es
    mov ax, 0xB800
    mov es, ax
    xor di, di
    xor ax, ax                      ; Fill with 0 (background color)
    mov cx, 8192                    ; 16KB / 2 = 8192 words
    cld
    rep stosw
    pop es

    ; Select color palette (cyan/magenta/white)
    mov ah, 0x0B
    mov bh, 0x01                    ; Palette select
    mov bl, 0x01                    ; Palette 1 (cyan, magenta, white)
    int 0x10

    ; Set background/border color to blue
    mov ah, 0x0B
    mov bh, 0x00                    ; Background color
    mov bl, 0x01                    ; Blue background
    int 0x10

    ret

; ============================================================================
; Welcome Box - White bordered rectangle with text
; ============================================================================

draw_welcome_box:
    push es

    ; Set ES to CGA video memory
    mov ax, 0xB800
    mov es, ax

    ; Draw top border (y=50, x=60 to x=260)
    mov bx, 50                      ; Y coordinate
    mov cx, 60                      ; Start X
.top_border:
    call plot_pixel_white
    inc cx
    cmp cx, 260
    jl .top_border

    ; Draw bottom border (y=150)
    mov bx, 150
    mov cx, 60
.bottom_border:
    call plot_pixel_white
    inc cx
    cmp cx, 260
    jl .bottom_border

    ; Draw left border (x=60, y=50 to y=150)
    mov cx, 60
    mov bx, 50
.left_border:
    call plot_pixel_white
    inc bx
    cmp bx, 150
    jle .left_border

    ; Draw right border (x=259, y=50 to y=150)
    mov cx, 259
    mov bx, 50
.right_border:
    call plot_pixel_white
    inc bx
    cmp bx, 150
    jle .right_border

    ; Test: Can '=' and '>' render on their own?
    mov word [draw_x], 65
    mov word [draw_y], 80

    ; Skip '9' and ':' - test if '=' and '>' work when called first
    ; Character '=' at offset (61-32)*8 = 232
    mov si, char_eq
    call draw_char

    ; Character '>' at offset (62-32)*8 = 240
    mov si, char_gt
    call draw_char

    pop es
    ret

; Hardcoded W character for testing (same as font8x8.asm char8_W)
test_W_char:
    db 0b01100011
    db 0b01100011
    db 0b01100011
    db 0b01101011
    db 0b01111111
    db 0b01110111
    db 0b01100011
    db 0b00000000

; ============================================================================
; Draw 8x8 character
; Input: SI = pointer to 8-byte character bitmap
;        draw_x, draw_y = top-left position
; Modifies: draw_x (advances by 12)
; ============================================================================

draw_char:
    pusha

    mov bx, [draw_y]                ; BX = current Y
    mov bp, 8                       ; BP = row counter

.row_loop:
    lodsb                           ; Get row bitmap into AL (from DS:SI)
    mov ah, al                      ; AH = bitmap for this row
    mov cx, [draw_x]                ; CX = current X
    mov dx, 8                       ; DX = column counter

.col_loop:
    test ah, 0x80                   ; Check leftmost bit
    jz .skip_pixel
    call plot_pixel_white           ; Plot at (CX, BX)
.skip_pixel:
    shl ah, 1                       ; Next bit
    inc cx                          ; Next X
    dec dx                          ; Decrement column counter
    jnz .col_loop

    inc bx                          ; Next Y
    dec bp                          ; Decrement row counter
    jnz .row_loop

    add word [draw_x], 12           ; Advance to next character position

    popa
    ret

; ============================================================================
; Plot a white pixel (color 3)
; Input: CX = X coordinate (0-319), BX = Y coordinate (0-199)
; Preserves all registers except flags
; ============================================================================

plot_pixel_white:
    pusha

    ; Save input values
    mov [.save_x], cx
    mov [.save_y], bx

    ; Calculate memory address
    ; For CGA 320x200 4-color:
    ; Byte offset = (Y/2) * 80 + (X/4)
    ; If Y is odd, add 0x2000

    ; Calculate row offset
    mov ax, bx                      ; AX = Y
    shr ax, 1                       ; AX = Y / 2
    mov cx, 80
    mul cx                          ; AX = (Y/2) * 80
    mov di, ax                      ; DI = row offset

    ; Add column offset
    mov ax, [.save_x]               ; AX = X
    shr ax, 1
    shr ax, 1                       ; AX = X / 4
    add di, ax                      ; DI = byte offset

    ; Check if Y was odd
    mov ax, [.save_y]
    test al, 1
    jz .even_row
    add di, 0x2000                  ; Odd rows are in second bank
.even_row:

    ; Calculate bit position within byte
    ; Each pixel is 2 bits, 4 pixels per byte
    ; Pixel 0 is bits 7-6, pixel 1 is bits 5-4, etc.
    mov ax, [.save_x]
    and ax, 3                       ; Get pixel position (0-3)

    ; Calculate shift amount: (3 - position) * 2
    mov cx, 3
    sub cl, al
    shl cl, 1                       ; Shift amount in CL

    ; Read current byte, set our pixel to white (11b)
    mov al, [es:di]
    mov ah, 0x03                    ; Color 3 (white)
    shl ah, cl                      ; Shift color to correct position

    ; Create mask to clear old pixel
    mov bl, 0x03
    shl bl, cl
    not bl                          ; Now BL has 0s where we want to write

    and al, bl                      ; Clear old pixel
    or al, ah                       ; Set new pixel
    mov [es:di], al

    popa
    ret

.save_x: dw 0
.save_y: dw 0

; ============================================================================
; Variables
; ============================================================================

draw_x: dw 0
draw_y: dw 0

; ============================================================================
; Font Data - 8x8 characters
; ============================================================================

%include "font8x8.asm"

; Character aliases for convenience
char_W      equ font_8x8 + ('W' - 32) * 8
char_E      equ font_8x8 + ('E' - 32) * 8
char_L      equ font_8x8 + ('L' - 32) * 8
char_C      equ font_8x8 + ('C' - 32) * 8
char_O      equ font_8x8 + ('O' - 32) * 8
char_M      equ font_8x8 + ('M' - 32) * 8
char_T      equ font_8x8 + ('T' - 32) * 8
char_U      equ font_8x8 + ('U' - 32) * 8
char_N      equ font_8x8 + ('N' - 32) * 8
char_D      equ font_8x8 + ('D' - 32) * 8
char_S      equ font_8x8 + ('S' - 32) * 8
char_3      equ font_8x8 + ('3' - 32) * 8
char_excl   equ font_8x8 + ('!' - 32) * 8
; Test characters
char_0      equ font_8x8 + ('0' - 32) * 8
char_4      equ font_8x8 + ('4' - 32) * 8
char_9      equ font_8x8 + ('9' - 32) * 8
char_colon  equ font_8x8 + (':' - 32) * 8
char_eq     equ font_8x8 + ('=' - 32) * 8
char_gt     equ font_8x8 + ('>' - 32) * 8

; ============================================================================
; Padding
; ============================================================================

; Pad to 16KB (32 sectors)
times 16384 - ($ - $$) db 0
