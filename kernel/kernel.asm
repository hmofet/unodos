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

    ; Install INT 0x80 handler for system calls
    call install_int_80

    ; Set up graphics mode (blue screen)
    call setup_graphics

    ; Draw welcome box with text
    call draw_welcome_box

    ; Test INT 0x80 discovery mechanism
    call test_int_80

    ; Halt
halt_loop:
    hlt
    jmp halt_loop

; ============================================================================
; System Call Infrastructure
; ============================================================================

; Install INT 0x80 handler for system call discovery
; Modifies: AX, BX, ES (temporarily)
install_int_80:
    pusha
    push ds

    ; Point to IVT at 0x0000:0000
    xor ax, ax
    mov es, ax

    ; Calculate IVT offset: 0x80 * 4 = 0x200
    mov bx, 0x0200

    ; Install handler offset
    mov word [es:bx], int_80_handler
    ; Install handler segment
    mov word [es:bx+2], 0x1000

    pop ds
    popa
    ret

; INT 0x80 Handler - System Call Discovery
; Input: AX = function number
;   AX = 0x0000: Get API table pointer
; Output: ES:BX = pointer to kernel_api_table
; Preserves: All other registers
int_80_handler:
    cmp ax, 0x0000
    jne .unknown_function

    ; Return pointer to API table
    mov bx, cs
    mov es, bx
    mov bx, kernel_api_table
    iret

.unknown_function:
    ; Unknown function - return error
    stc                             ; Set carry flag for error
    iret

; Test INT 0x80 Discovery Mechanism
; Calls INT 0x80, verifies API table, displays status
test_int_80:
    pusha
    push ds
    push es

    ; Call INT 0x80 to get API table
    mov ax, 0x0000
    int 0x80

    ; ES:BX now points to kernel_api_table
    ; Verify magic number
    cmp word [es:bx], 0x4B41        ; Check for 'KA'
    jne .test_failed

    ; Success - display checkmark at bottom right
    mov word [draw_x], 290
    mov word [draw_y], 185

    ; Draw checkmark character (using 'OK' for now)
    push es
    mov ax, 0xB800
    mov es, ax

    mov si, char_O
    call draw_char
    mov si, char_K
    call draw_char

    pop es
    jmp .test_done

.test_failed:
    ; Failure - display 'XX' at bottom right
    mov word [draw_x], 290
    mov word [draw_y], 185

    push es
    mov ax, 0xB800
    mov es, ax

    mov si, char_X
    call draw_char
    mov si, char_X
    call draw_char

    pop es

.test_done:
    pop es
    pop ds
    popa
    ret

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

    ; Render "WELCOME TO UNODOS 3!" message
    ; All these characters are at offsets 0-200 (working range)
    mov word [draw_x], 70
    mov word [draw_y], 85

    mov si, char_W
    call draw_char
    mov si, char_E
    call draw_char
    mov si, char_L
    call draw_char
    mov si, char_L
    call draw_char
    mov si, char_C
    call draw_char
    mov si, char_O
    call draw_char
    mov si, char_M
    call draw_char
    mov si, char_E
    call draw_char

    ; Space
    mov word [draw_x], 70
    mov word [draw_y], 100

    mov si, char_T
    call draw_char
    mov si, char_O
    call draw_char

    ; Space and next line
    mov word [draw_x], 70
    mov word [draw_y], 115

    mov si, char_U
    call draw_char
    mov si, char_N
    call draw_char
    mov si, char_O
    call draw_char
    mov si, char_D
    call draw_char
    mov si, char_O
    call draw_char
    mov si, char_S
    call draw_char

    ; Space
    add word [draw_x], 12

    mov si, char_3
    call draw_char
    mov si, char_excl
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

; Hardcoded '=' character for testing
test_eq_char:
    db 0b00000000
    db 0b00000000
    db 0b01111110
    db 0b00000000
    db 0b01111110
    db 0b00000000
    db 0b00000000
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
    mov [pixel_save_x], cx
    mov [pixel_save_y], bx

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
    mov ax, [pixel_save_x]          ; AX = X
    shr ax, 1
    shr ax, 1                       ; AX = X / 4
    add di, ax                      ; DI = byte offset

    ; Check if Y was odd
    mov ax, [pixel_save_y]
    test al, 1
    jz .even_row
    add di, 0x2000                  ; Odd rows are in second bank
.even_row:

    ; Calculate bit position within byte
    ; Each pixel is 2 bits, 4 pixels per byte
    ; Pixel 0 is bits 7-6, pixel 1 is bits 5-4, etc.
    mov ax, [pixel_save_x]
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

; ============================================================================
; Kernel API Table - At fixed address 0x1000:0x0500
; ============================================================================

; Pad to exactly offset 0x0500 (1280 bytes)
times 0x0500 - ($ - $$) db 0

kernel_api_table:
    ; Header
    dw 0x4B41                       ; Magic: 'KA' (Kernel API)
    dw 0x0001                       ; Version: 1.0
    dw 10                           ; Number of function slots
    dw 0                            ; Reserved for future use

    ; Function Pointers (Offset from table start)
    ; Graphics API (frequent calls - optimize for speed)
    dw gfx_draw_pixel_stub          ; 0: Draw single pixel
    dw gfx_draw_rect_stub           ; 1: Draw rectangle outline
    dw gfx_draw_filled_rect_stub    ; 2: Draw filled rectangle
    dw gfx_draw_char_stub           ; 3: Draw character
    dw gfx_draw_string_stub         ; 4: Draw string
    dw gfx_clear_area_stub          ; 5: Clear rectangular area

    ; Memory Management
    dw mem_alloc_stub               ; 6: Allocate memory (malloc)
    dw mem_free_stub                ; 7: Free memory

    ; Event System
    dw event_get_stub               ; 8: Get next event (non-blocking)
    dw event_wait_stub              ; 9: Wait for event (blocking)

; ============================================================================
; API Stub Functions (Temporary - To Be Implemented)
; ============================================================================

; Graphics stubs
gfx_draw_pixel_stub:
    ; TODO: Implement in Foundation 1.2
    ret

gfx_draw_rect_stub:
    ; TODO: Implement in Foundation 1.2
    ret

gfx_draw_filled_rect_stub:
    ; TODO: Implement in Foundation 1.2
    ret

gfx_draw_char_stub:
    ; TODO: Implement in Foundation 1.2
    ret

gfx_draw_string_stub:
    ; TODO: Implement in Foundation 1.2
    ret

gfx_clear_area_stub:
    ; TODO: Implement in Foundation 1.2
    ret

; Memory management stubs
mem_alloc_stub:
    ; TODO: Implement in Foundation 1.3
    xor ax, ax                      ; Return NULL for now
    ret

mem_free_stub:
    ; TODO: Implement in Foundation 1.3
    ret

; Event system stubs
event_get_stub:
    ; TODO: Implement in Foundation 1.5
    xor ax, ax                      ; Return 0 (no event)
    ret

event_wait_stub:
    ; TODO: Implement in Foundation 1.5
    hlt                             ; Wait for interrupt
    xor ax, ax                      ; Return 0 (no event)
    ret

; ============================================================================
; Font Data - 8x8 characters
; ============================================================================
; IMPORTANT: Font must come BEFORE variables to avoid addressing issues

%include "font8x8.asm"

; ============================================================================
; Variables
; ============================================================================

; Character drawing state
draw_x: dw 0
draw_y: dw 0

; plot_pixel_white temporary storage
pixel_save_x: dw 0
pixel_save_y: dw 0

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
char_K      equ font_8x8 + ('K' - 32) * 8
char_X      equ font_8x8 + ('X' - 32) * 8
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
