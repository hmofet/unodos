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

    ; Test graphics API functions
    call test_graphics_api

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

; Test Graphics API Functions
; Demonstrates all graphics API functions visually
test_graphics_api:
    pusha
    push es

    ; Set up video memory
    mov ax, 0xB800
    mov es, ax

    ; Test 1: Draw filled rectangle (below welcome box)
    ; Position: X=20, Y=160, Width=60, Height=30
    mov bx, 20                      ; X
    mov cx, 160                     ; Y
    mov dx, 60                      ; Width
    mov si, 30                      ; Height
    call gfx_draw_filled_rect_stub

    ; Test 2: Draw rectangle outline next to it
    ; Position: X=90, Y=160, Width=60, Height=30
    mov bx, 90                      ; X
    mov cx, 160                     ; Y
    mov dx, 60                      ; Width
    mov si, 30                      ; Height
    call gfx_draw_rect_stub

    ; Test 3: Draw small filled rectangle (indicator)
    ; Position: X=160, Y=165, Width=10, Height=10
    mov bx, 160                     ; X
    mov cx, 165                     ; Y
    mov dx, 10                      ; Width
    mov si, 10                      ; Height
    call gfx_draw_filled_rect_stub

    ; Test 4: Draw text using gfx_draw_string
    ; Position: X=180, Y=165, String="API"
    mov bx, 180                     ; X
    mov cx, 165                     ; Y
    mov si, .test_string
    call gfx_draw_string_stub

    ; Test 5: Draw individual characters using gfx_draw_char
    ; Position: X=220, Y=165, Characters "OK"
    mov bx, 220                     ; X
    mov cx, 165                     ; Y
    mov al, 'O'
    call gfx_draw_char_stub

    mov bx, 232                     ; X (12 pixels spacing)
    mov cx, 165                     ; Y
    mov al, 'K'
    call gfx_draw_char_stub

    pop es
    popa
    ret

.test_string:
    db 'API', 0

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
; Graphics API Functions (Foundation 1.2)
; ============================================================================

; gfx_draw_pixel_stub - Draw single pixel
; Input: CX = X coordinate (0-319)
;        BX = Y coordinate (0-199)
;        AL = Color (0-3)
; Output: None
; Preserves: All registers
gfx_draw_pixel_stub:
    ; For now, only supports color 3 (white)
    ; TODO: Add multi-color support
    call plot_pixel_white
    ret

; gfx_draw_char_stub - Draw character
; Input: BX = X coordinate
;        CX = Y coordinate
;        AL = ASCII character
; Output: None
; Modifies: draw_x advances by 12
gfx_draw_char_stub:
    pusha
    push es

    ; Set up video memory
    mov dx, 0xB800
    mov es, dx

    ; Set position
    mov [draw_x], bx
    mov [draw_y], cx

    ; Calculate font offset: (ASCII - 32) * 8
    sub al, 32
    mov ah, 0
    mov dl, 8
    mul dl                          ; AX = (ASCII - 32) * 8

    ; Get font address
    mov si, font_8x8
    add si, ax

    ; Draw the character
    call draw_char

    pop es
    popa
    ret

; gfx_draw_string_stub - Draw null-terminated string
; Input: BX = X coordinate
;        CX = Y coordinate
;        SI = Pointer to null-terminated string
; Output: None
; Modifies: draw_x advances
gfx_draw_string_stub:
    pusha
    push es
    push ds

    ; Set up video memory
    mov dx, 0xB800
    mov es, dx

    ; Set starting position
    mov [draw_x], bx
    mov [draw_y], cx

.string_loop:
    lodsb                           ; Load character from DS:SI
    test al, al                     ; Check for null terminator
    jz .string_done

    ; Calculate font offset
    sub al, 32
    mov ah, 0
    mov dl, 8
    mul dl

    ; Get font address
    push si
    mov si, font_8x8
    add si, ax

    ; Draw character
    call draw_char

    pop si
    jmp .string_loop

.string_done:
    pop ds
    pop es
    popa
    ret

; gfx_draw_rect_stub - Draw rectangle outline
; Input: BX = X coordinate (top-left)
;        CX = Y coordinate (top-left)
;        DX = Width
;        SI = Height
; Output: None
; Preserves: All registers
gfx_draw_rect_stub:
    pusha
    push es

    ; Set up video memory
    mov ax, 0xB800
    mov es, ax

    ; Save parameters
    push bx                         ; Save X
    push cx                         ; Save Y
    push dx                         ; Save Width
    push si                         ; Save Height

    ; Draw top edge
    pop si                          ; Restore Height (will push back)
    pop dx                          ; Restore Width
    pop cx                          ; Restore Y
    pop bx                          ; Restore X

    push bx
    push cx
    push dx
    push si

    ; Top edge: Y=CX, X from BX to BX+DX
    mov di, 0                       ; Counter
.top_edge:
    push cx
    push bx
    add bx, di
    call plot_pixel_white
    pop bx
    pop cx
    inc di
    cmp di, dx
    jl .top_edge

    ; Bottom edge: Y=CX+SI-1, X from BX to BX+DX
    pop si
    pop dx
    pop cx
    pop bx

    push bx
    push cx
    push dx
    push si

    add cx, si
    dec cx                          ; Y = Y + Height - 1
    mov di, 0
.bottom_edge:
    push cx
    push bx
    add bx, di
    call plot_pixel_white
    pop bx
    pop cx
    inc di
    cmp di, dx
    jl .bottom_edge

    ; Left edge: X=BX, Y from CX to CX+SI
    pop si
    pop dx
    pop cx
    pop bx

    push bx
    push cx
    push dx
    push si

    mov di, 0
.left_edge:
    push cx
    push bx
    add cx, di
    call plot_pixel_white
    pop bx
    pop cx
    inc di
    cmp di, si
    jl .left_edge

    ; Right edge: X=BX+DX-1, Y from CX to CX+SI
    pop si
    pop dx
    pop cx
    pop bx

    add bx, dx
    dec bx                          ; X = X + Width - 1
    mov di, 0
.right_edge:
    push cx
    push bx
    add cx, di
    call plot_pixel_white
    pop bx
    pop cx
    inc di
    cmp di, si
    jl .right_edge

    pop es
    popa
    ret

; gfx_draw_filled_rect_stub - Draw filled rectangle
; Input: BX = X coordinate (top-left)
;        CX = Y coordinate (top-left)
;        DX = Width
;        SI = Height
; Output: None
; Preserves: All registers
gfx_draw_filled_rect_stub:
    pusha
    push es

    ; Set up video memory
    mov ax, 0xB800
    mov es, ax

    ; Use BP for height counter, DI for width counter
    mov bp, si                      ; BP = Height
    mov ax, cx                      ; AX = Start Y

.row_loop:
    mov di, dx                      ; DI = Width counter
    push cx                          ; Save current Y
    push bx                          ; Save start X

.col_loop:
    call plot_pixel_white            ; Draw pixel at (BX, CX)
    inc bx                           ; Next X
    dec di
    jnz .col_loop

    pop bx                           ; Restore start X
    pop cx                           ; Restore Y
    inc cx                           ; Next row
    dec bp
    jnz .row_loop

    pop es
    popa
    ret

; gfx_clear_area_stub - Clear rectangular area
; Input: BX = X coordinate (top-left)
;        CX = Y coordinate (top-left)
;        DX = Width
;        SI = Height
; Output: None
; Preserves: All registers
gfx_clear_area_stub:
    ; TODO: Implement proper clear (set pixels to background color)
    ; For now, just return (no-op)
    ret

; ============================================================================
; Memory Management API (Foundation 1.3)
; ============================================================================

; Simple memory allocator - First-fit algorithm
; Heap starts at 0x1400:0x0000, extends to ~0x9FFF (640KB limit)
; Each block has 4-byte header: [size:2][flags:2]
;   size = total block size including header
;   flags = 0x0000 (free) or 0xFFFF (allocated)

; mem_alloc_stub - Allocate memory
; Input: AX = Size in bytes
; Output: AX = Pointer (offset from 0x1400:0000), 0 if failed
; Preserves: BX, CX, DX
mem_alloc_stub:
    push bx
    push si
    push es

    test ax, ax
    jz .fail                        ; Don't allocate 0 bytes

    ; Round up to 4-byte boundary, add header
    add ax, 7                       ; +4 header +3 for rounding
    and ax, 0xFFFC                  ; Round to 4-byte
    mov bx, ax                      ; BX = requested size

    ; Set up heap segment
    push ds
    mov ax, 0x1400
    mov ds, ax
    mov es, ax

    ; Initialize heap if needed (first allocation)
    cmp word [heap_initialized], 0
    jne .heap_ready

    ; Initialize first free block
    mov word [0], 0xF000            ; Size: ~60KB initially
    mov word [2], 0                 ; Flags: free
    mov word [heap_initialized], 1

.heap_ready:
    ; Search for first-fit block
    xor si, si                      ; SI = current block offset

.search:
    mov ax, [si]                    ; AX = block size
    test ax, ax
    jz .fail_restore                ; End of heap

    ; Check if block is free
    cmp word [si+2], 0
    jne .next                       ; Block allocated, skip

    ; Check if large enough
    cmp ax, bx
    jge .allocate                   ; Found suitable block

.next:
    add si, ax                      ; Move to next block
    cmp si, 0xF000                  ; Check heap limit
    jb .search

.fail_restore:
    pop ds
.fail:
    xor ax, ax                      ; Return NULL
    jmp .done

.allocate:
    ; Mark block as allocated
    mov word [si+2], 0xFFFF

    ; Return pointer (skip header)
    mov ax, si
    add ax, 4

    pop ds

.done:
    pop es
    pop si
    pop bx
    ret

; mem_free_stub - Free memory
; Input: AX = Pointer (offset from 0x1400:0000)
; Output: None
mem_free_stub:
    push ax
    push bx
    push ds

    test ax, ax
    jz .done                        ; NULL pointer, ignore

    ; Set up heap segment
    mov bx, 0x1400
    mov ds, bx

    ; Get block header
    sub ax, 4                       ; Point to header

    ; Mark as free
    mov bx, ax
    mov word [bx+2], 0              ; Clear allocated flag

.done:
    pop ds
    pop bx
    pop ax
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

; Memory allocator state
heap_initialized: dw 0

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
