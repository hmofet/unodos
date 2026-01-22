; UnoDOS Second Stage Loader
; Loaded at 0x0800:0000 (linear 0x8000)
; Sets up system and displays graphical Hello World

[BITS 16]
[ORG 0x0000]

; ============================================================================
; Signature and Entry Point
; ============================================================================

signature:
    dw 0x4E55               ; 'UN' signature for boot sector verification

entry:
    ; Set up segment registers for our location
    mov ax, 0x0800
    mov ds, ax
    mov es, ax

    ; Keep SS:SP from boot sector for now

    mov si, msg_stage2
    call print_string

    ; Detect and report memory
    call detect_memory

    ; Detect video adapter
    call detect_video

    ; Set up graphics mode and show Hello World
    call setup_graphics

    ; Main loop - system is running
    mov si, msg_running
    call print_string

.idle:
    hlt
    jmp .idle

; ============================================================================
; Memory Detection
; ============================================================================

detect_memory:
    mov si, msg_detect_mem
    call print_string

    ; Get conventional memory size via INT 12h
    int 0x12                ; Returns KB in AX
    mov [mem_kb], ax

    ; Print memory size
    call print_decimal
    mov si, msg_kb
    call print_string

    ret

; ============================================================================
; Video Adapter Detection
; ============================================================================

detect_video:
    mov si, msg_detect_vid
    call print_string

    ; Check for CGA/MDA by reading BIOS equipment list
    int 0x11                ; Get equipment list in AX
    mov [equipment], ax

    ; Bits 4-5 indicate video mode:
    ; 00 = EGA/VGA, 01 = 40x25 CGA, 10 = 80x25 CGA, 11 = 80x25 MDA
    and ax, 0x30
    mov cl, 4
    shr ax, cl

    cmp al, 3
    je .mda
    cmp al, 0
    je .ega_vga
    ; Otherwise it's CGA
    mov si, msg_cga
    call print_string
    mov byte [video_type], 1    ; CGA
    ret

.mda:
    mov si, msg_mda
    call print_string
    mov byte [video_type], 0    ; MDA
    ret

.ega_vga:
    ; Could be EGA or VGA, treat as CGA-compatible for now
    mov si, msg_ega
    call print_string
    mov byte [video_type], 2    ; EGA/VGA
    ret

; ============================================================================
; Graphics Setup
; ============================================================================

setup_graphics:
    mov si, msg_setup_gfx
    call print_string

    ; Check video type
    mov al, [video_type]
    cmp al, 0
    je .mda_mode

    ; CGA or EGA/VGA - use CGA graphics mode
    jmp .cga_mode

.mda_mode:
    ; MDA doesn't support graphics, use text-based "graphics"
    mov si, msg_mda_text
    call print_string
    call draw_hello_text
    ret

.cga_mode:
    mov si, msg_cga_gfx
    call print_string

    ; Set CGA 320x200 4-color mode (mode 4)
    mov ah, 0x00
    mov al, 0x04            ; 320x200, 4 colors
    int 0x10

    ; Select color palette (cyan/magenta/white)
    mov ah, 0x0B
    mov bh, 0x01            ; Palette select
    mov bl, 0x01            ; Palette 1 (cyan, magenta, white)
    int 0x10

    ; Set background/border color
    mov ah, 0x0B
    mov bh, 0x00            ; Background color
    mov bl, 0x01            ; Blue background
    int 0x10

    ; Draw the graphical Hello World
    call draw_hello_gfx

    ret

; ============================================================================
; Text Mode Hello World (for MDA)
; ============================================================================

draw_hello_text:
    push es

    ; Point to MDA video memory
    mov ax, 0xB000
    mov es, ax

    ; Clear screen with spaces
    mov di, 0
    mov cx, 2000            ; 80x25 = 2000 characters
    mov ax, 0x0720          ; Space with normal attribute
    rep stosw

    ; Draw a simple box in the center
    ; Box at row 10, column 25, 30 chars wide, 5 rows tall

    ; Top border
    mov di, (10 * 160) + (25 * 2)   ; Row 10, Column 25
    mov ah, 0x0F            ; Bright white on black

    mov al, 0xDA            ; Top-left corner
    stosw
    mov cx, 28
    mov al, 0xC4            ; Horizontal line
.top_loop:
    stosw
    loop .top_loop
    mov al, 0xBF            ; Top-right corner
    stosw

    ; Middle rows with text
    mov di, (11 * 160) + (25 * 2)
    mov al, 0xB3            ; Vertical line
    stosw
    mov si, hello_text
    mov cx, 28
.text_loop1:
    lodsb
    mov ah, 0x0F
    stosw
    loop .text_loop1
    mov al, 0xB3
    mov ah, 0x0F
    stosw

    ; "UnoDOS v0.2.0" centered
    mov di, (12 * 160) + (25 * 2)
    mov al, 0xB3
    mov ah, 0x0F
    stosw
    mov si, version_text
    mov cx, 28
.text_loop2:
    lodsb
    mov ah, 0x0E            ; Yellow
    stosw
    loop .text_loop2
    mov al, 0xB3
    mov ah, 0x0F
    stosw

    ; Empty row
    mov di, (13 * 160) + (25 * 2)
    mov al, 0xB3
    mov ah, 0x0F
    stosw
    mov cx, 28
    mov al, ' '
.empty_loop:
    stosw
    loop .empty_loop
    mov al, 0xB3
    stosw

    ; Bottom border
    mov di, (14 * 160) + (25 * 2)
    mov al, 0xC0            ; Bottom-left corner
    mov ah, 0x0F
    stosw
    mov cx, 28
    mov al, 0xC4            ; Horizontal line
.bottom_loop:
    stosw
    loop .bottom_loop
    mov al, 0xD9            ; Bottom-right corner
    stosw

    pop es
    ret

; ============================================================================
; CGA Graphics Hello World
; ============================================================================

draw_hello_gfx:
    push es

    ; CGA graphics memory at 0xB800
    mov ax, 0xB800
    mov es, ax

    ; CGA 320x200 mode uses interleaved memory:
    ; Even lines: 0xB800:0000
    ; Odd lines:  0xB800:2000

    ; Draw a border rectangle
    ; Using color 3 (white in palette 1)

    ; Draw top border (y=50, x=60 to x=260)
    mov bx, 50              ; Y coordinate
    mov cx, 60              ; Start X
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

    ; Draw "HELLO" text using simple bitmap font
    ; Each letter is 8x8 pixels, starting at x=100, y=80

    mov word [draw_x], 100
    mov word [draw_y], 80

    ; H
    mov si, char_H
    call draw_char

    ; E
    mov si, char_E
    call draw_char

    ; L
    mov si, char_L
    call draw_char

    ; L
    mov si, char_L
    call draw_char

    ; O
    mov si, char_O
    call draw_char

    ; Space
    add word [draw_x], 12

    ; Draw "WORLD" on next line
    mov word [draw_x], 92
    add word [draw_y], 20

    ; W
    mov si, char_W
    call draw_char

    ; O
    mov si, char_O
    call draw_char

    ; R
    mov si, char_R
    call draw_char

    ; L
    mov si, char_L
    call draw_char

    ; D
    mov si, char_D
    call draw_char

    ; !
    mov si, char_excl
    call draw_char

    pop es
    ret

; Plot a white pixel (color 3)
; Input: CX = X coordinate (0-319), BX = Y coordinate (0-199)
plot_pixel_white:
    push ax
    push di
    push bx
    push cx

    ; Calculate memory address
    ; For CGA 320x200 4-color:
    ; Byte offset = (Y/2) * 80 + (X/4)
    ; If Y is odd, add 0x2000

    mov ax, bx              ; Y coordinate
    test al, 1              ; Check if odd
    pushf                   ; Save flags
    shr ax, 1               ; Y / 2
    mov di, 80
    mul di                  ; AX = (Y/2) * 80

    mov di, ax              ; DI = row offset

    mov ax, cx              ; X coordinate
    shr ax, 1
    shr ax, 1               ; X / 4
    add di, ax              ; DI = byte offset

    popf                    ; Restore odd/even flag
    jz .even_row
    add di, 0x2000          ; Odd rows are in second bank
.even_row:

    ; Calculate bit position within byte
    ; Each pixel is 2 bits, 4 pixels per byte
    ; Pixel 0 is bits 7-6, pixel 1 is bits 5-4, etc.
    mov ax, cx
    and ax, 3               ; Get pixel position (0-3)

    ; Calculate shift amount: (3 - position) * 2
    mov cl, 3
    sub cl, al
    shl cl, 1               ; Shift amount

    ; Read current byte, set our pixel to white (11b)
    mov al, [es:di]
    mov ah, 0x03            ; Color 3 (white)
    shl ah, cl              ; Shift color to correct position

    ; Create mask to clear old pixel
    mov ch, 0xFC            ; 11111100b
    rol ch, cl              ; Rotate mask to correct position
    ; Actually we need inverse: clear the 2 bits
    mov ch, 0x03
    shl ch, cl
    not ch                  ; Now CH has 0s where we want to write

    and al, ch              ; Clear old pixel
    or al, ah               ; Set new pixel
    mov [es:di], al

    pop cx
    pop bx
    pop di
    pop ax
    ret

; Draw 8x8 character
; Input: SI = pointer to 8-byte character bitmap
;        draw_x, draw_y = top-left position
draw_char:
    push ax
    push bx
    push cx
    push dx

    mov dx, 8               ; 8 rows
    mov bx, [draw_y]

.row_loop:
    lodsb                   ; Get row bitmap
    mov ah, al
    mov cx, [draw_x]
    push cx

    mov al, 8               ; 8 columns
.col_loop:
    test ah, 0x80           ; Check leftmost bit
    jz .skip_pixel
    call plot_pixel_white
.skip_pixel:
    shl ah, 1               ; Next bit
    inc cx                  ; Next X
    dec al
    jnz .col_loop

    pop cx                  ; Restore X
    inc bx                  ; Next Y
    dec dx
    jnz .row_loop

    add word [draw_x], 12   ; Advance to next character position

    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Utility Functions
; ============================================================================

; Print null-terminated string (uses BIOS, works in text mode)
; Need to switch to text mode for debug output
print_string:
    push ax
    push bx
    push si
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    jmp .loop
.done:
    pop si
    pop bx
    pop ax
    ret

; Print AX as decimal number
print_decimal:
    push ax
    push bx
    push cx
    push dx

    mov bx, 10
    xor cx, cx              ; Digit counter

.divide:
    xor dx, dx
    div bx                  ; AX = AX / 10, DX = remainder
    push dx                 ; Save digit
    inc cx
    test ax, ax
    jnz .divide

.print_digits:
    pop dx
    add dl, '0'
    mov ah, 0x0E
    mov al, dl
    mov bh, 0
    int 0x10
    loop .print_digits

    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Data
; ============================================================================

; Variables
mem_kb:         dw 0
equipment:      dw 0
video_type:     db 0        ; 0=MDA, 1=CGA, 2=EGA/VGA
draw_x:         dw 0
draw_y:         dw 0

; Messages
msg_stage2:     db 'Stage2 loaded', 0x0D, 0x0A, 0
msg_detect_mem: db 'Memory: ', 0
msg_kb:         db ' KB', 0x0D, 0x0A, 0
msg_detect_vid: db 'Video: ', 0
msg_mda:        db 'MDA', 0x0D, 0x0A, 0
msg_cga:        db 'CGA', 0x0D, 0x0A, 0
msg_ega:        db 'EGA/VGA', 0x0D, 0x0A, 0
msg_setup_gfx:  db 'Init graphics...', 0x0D, 0x0A, 0
msg_mda_text:   db 'MDA text mode', 0x0D, 0x0A, 0
msg_cga_gfx:    db 'CGA 320x200', 0x0D, 0x0A, 0
msg_running:    db 'UnoDOS running!', 0x0D, 0x0A, 0

; Text for MDA display
hello_text:     db '      HELLO WORLD!      ', 0, 0, 0, 0
version_text:   db '      UnoDOS v0.2.0     ', 0, 0, 0, 0

; 8x8 character bitmaps (1 = pixel on)
char_H:
    db 0b11000011
    db 0b11000011
    db 0b11000011
    db 0b11111111
    db 0b11111111
    db 0b11000011
    db 0b11000011
    db 0b11000011

char_E:
    db 0b11111111
    db 0b11111111
    db 0b11000000
    db 0b11111100
    db 0b11111100
    db 0b11000000
    db 0b11111111
    db 0b11111111

char_L:
    db 0b11000000
    db 0b11000000
    db 0b11000000
    db 0b11000000
    db 0b11000000
    db 0b11000000
    db 0b11111111
    db 0b11111111

char_O:
    db 0b00111100
    db 0b01111110
    db 0b11000011
    db 0b11000011
    db 0b11000011
    db 0b11000011
    db 0b01111110
    db 0b00111100

char_W:
    db 0b11000011
    db 0b11000011
    db 0b11000011
    db 0b11011011
    db 0b11011011
    db 0b11111111
    db 0b01100110
    db 0b01100110

char_R:
    db 0b11111100
    db 0b11111110
    db 0b11000011
    db 0b11111110
    db 0b11111100
    db 0b11001100
    db 0b11000110
    db 0b11000011

char_D:
    db 0b11111100
    db 0b11111110
    db 0b11000011
    db 0b11000011
    db 0b11000011
    db 0b11000011
    db 0b11111110
    db 0b11111100

char_excl:
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00011000
    db 0b00000000
    db 0b00011000
    db 0b00011000

; ============================================================================
; Padding
; ============================================================================

; Pad to fill sectors (we're loading 16 sectors = 8KB)
times 8192 - ($ - $$) db 0
