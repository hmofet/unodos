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

    ; Draw welcome box
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
; Welcome Box - White bordered rectangle
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

    pop es
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
; Padding
; ============================================================================

; Pad to 16KB (32 sectors)
times 16384 - ($ - $$) db 0
