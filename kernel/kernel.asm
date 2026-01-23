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

    ; Halt
halt_loop:
    hlt
    jmp halt_loop

; ============================================================================
; System Call Infrastructure
; ============================================================================

; Install INT 0x80 handler for system call discovery
install_int_80:
    push es
    xor ax, ax
    mov es, ax
    mov word [es:0x0200], int_80_handler
    mov word [es:0x0202], 0x1000
    pop es
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

; ============================================================================
; Graphics Setup
; ============================================================================

setup_graphics:
    xor ax, ax
    mov al, 0x04
    int 0x10
    push es
    mov ax, 0xB800
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 8192
    rep stosw
    pop es
    mov ax, 0x0B01
    mov bx, 0x0101
    int 0x10
    mov bh, 0
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

    ; Render "WELCOME TO UNODOS 3!" message using strings
    mov bx, 70
    mov cx, 85
    mov si, .msg1
    call gfx_draw_string_stub

    mov bx, 70
    mov cx, 100
    mov si, .msg2
    call gfx_draw_string_stub

    mov bx, 70
    mov cx, 115
    mov si, .msg3
    call gfx_draw_string_stub

    pop es
    ret

.msg1: db 'WELCOME', 0
.msg2: db 'TO', 0
.msg3: db 'UNODOS 3!', 0

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
    push ax
    push bx
    push cx
    push di
    push dx
    mov ax, bx
    shr ax, 1
    mov dx, 80
    mul dx
    mov di, ax
    mov ax, cx
    shr ax, 1
    shr ax, 1
    add di, ax
    test bl, 1
    jz .e
    add di, 0x2000
.e:
    mov ax, cx
    and ax, 3
    mov cx, 3
    sub cl, al
    shl cl, 1
    mov al, [es:di]
    mov ah, 0x03
    shl ah, cl
    mov bl, 0x03
    shl bl, cl
    not bl
    and al, bl
    or al, ah
    mov [es:di], al
    pop dx
    pop di
    pop cx
    pop bx
    pop ax
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
gfx_draw_char_stub:
    push es
    push ax
    push dx
    mov word [draw_x], bx
    mov word [draw_y], cx
    mov dx, 0xB800
    mov es, dx
    sub al, 32
    mov ah, 0
    mov dl, 8
    mul dl
    mov si, font_8x8
    add si, ax
    call draw_char
    pop dx
    pop ax
    pop es
    ret

; gfx_draw_string_stub - Draw null-terminated string
gfx_draw_string_stub:
    push es
    push ax
    push dx
    push di
    mov word [draw_x], bx
    mov word [draw_y], cx
    mov dx, 0xB800
    mov es, dx
.loop:
    lodsb
    test al, al
    jz .done
    sub al, 32
    mov ah, 0
    mov dl, 8
    mul dl
    mov di, si
    mov si, font_8x8
    add si, ax
    call draw_char
    mov si, di
    jmp .loop
.done:
    pop di
    pop dx
    pop ax
    pop es
    ret

; gfx_draw_rect_stub - Draw rectangle outline
; Input: BX = X, CX = Y, DX = Width, SI = Height
gfx_draw_rect_stub:
    push es
    push ax
    push bp
    push di
    mov ax, 0xB800
    mov es, ax
    mov bp, bx                      ; BP = X
    mov ax, cx                      ; AX = Y

    ; Top edge
    mov di, 0
.top:
    mov bx, bp
    add bx, di
    mov cx, ax
    call plot_pixel_white
    inc di
    cmp di, dx
    jl .top

    ; Bottom edge
    mov di, 0
    mov cx, ax
    add cx, si
    dec cx
.bottom:
    mov bx, bp
    add bx, di
    call plot_pixel_white
    inc di
    cmp di, dx
    jl .bottom

    ; Left edge
    mov di, 0
    mov bx, bp
.left:
    mov cx, ax
    add cx, di
    call plot_pixel_white
    inc di
    cmp di, si
    jl .left

    ; Right edge
    mov di, 0
    mov bx, bp
    add bx, dx
    dec bx
.right:
    mov cx, ax
    add cx, di
    call plot_pixel_white
    inc di
    cmp di, si
    jl .right

    pop di
    pop bp
    pop ax
    pop es
    ret

; gfx_draw_filled_rect_stub - Draw filled rectangle
gfx_draw_filled_rect_stub:
    push es
    push ax
    push bp
    push di
    mov ax, 0xB800
    mov es, ax
    mov bp, si
.row:
    mov di, dx
    push cx
    push bx
.col:
    call plot_pixel_white
    inc bx
    dec di
    jnz .col
    pop bx
    pop cx
    inc cx
    dec bp
    jnz .row
    pop di
    pop bp
    pop ax
    pop es
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
heap_initialized: dw 0

; ============================================================================
; Padding
; ============================================================================

; Pad to 24KB (48 sectors)
times 24576 - ($ - $$) db 0
