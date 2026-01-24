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

    ; Install keyboard handler (Foundation 1.4)
    call install_keyboard

    ; Set up graphics mode (blue screen)
    call setup_graphics

    ; Display version number (top-left corner)
    mov bx, 4
    mov cx, 4
    mov si, version_string
    call gfx_draw_string_stub

    ; Display build number (below version)
    mov bx, 4
    mov cx, 14
    mov si, build_string
    call gfx_draw_string_stub

    ; Welcome box removed - clutters test output
    ; call draw_welcome_box

    ; Enable interrupts
    sti

    ; Run keyboard demo (with filesystem test on 'F' key)
    call keyboard_demo

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
; Keyboard Driver (Foundation 1.4)
; ============================================================================

; Install keyboard interrupt handler
install_keyboard:
    push es
    push ax
    push bx

    ; Save original INT 9h vector
    xor ax, ax
    mov es, ax
    mov ax, [es:0x0024]
    mov [old_int9_offset], ax
    mov ax, [es:0x0026]
    mov [old_int9_segment], ax

    ; Install our handler
    mov word [es:0x0024], int_09_handler
    mov word [es:0x0026], 0x1000

    ; Initialize keyboard buffer
    mov word [kbd_buffer_head], 0
    mov word [kbd_buffer_tail], 0
    mov byte [kbd_shift_state], 0
    mov byte [kbd_ctrl_state], 0
    mov byte [kbd_alt_state], 0

    pop bx
    pop ax
    pop es
    ret

; INT 09h - Keyboard interrupt handler
int_09_handler:
    push ax
    push bx
    push ds

    ; Set DS to kernel segment
    mov ax, 0x1000
    mov ds, ax

    ; Read scan code from keyboard port
    in al, 0x60

    ; Check for modifier keys
    cmp al, 0x2A                    ; Left Shift press
    je .shift_press
    cmp al, 0x36                    ; Right Shift press
    je .shift_press
    cmp al, 0xAA                    ; Left Shift release
    je .shift_release
    cmp al, 0xB6                    ; Right Shift release
    je .shift_release
    cmp al, 0x1D                    ; Ctrl press
    je .ctrl_press
    cmp al, 0x9D                    ; Ctrl release
    je .ctrl_release
    cmp al, 0x38                    ; Alt press
    je .alt_press
    cmp al, 0xB8                    ; Alt release
    je .alt_release

    ; Check if it's a key release (bit 7 set)
    test al, 0x80
    jnz .done

    ; Translate scan code to ASCII
    mov bx, ax                      ; BX = scan code
    xor bh, bh

    ; Check shift state
    cmp byte [kbd_shift_state], 0
    je .use_lower

    ; Use shifted table
    mov al, [scancode_shifted + bx]
    jmp .store_key

.use_lower:
    mov al, [scancode_normal + bx]

.store_key:
    ; Don't store null characters
    test al, al
    jz .done

    ; Store in circular buffer (for backward compatibility)
    mov bx, [kbd_buffer_tail]
    mov [kbd_buffer + bx], al
    inc bx
    and bx, 0x0F                    ; Wrap at 16
    cmp bx, [kbd_buffer_head]       ; Buffer full?
    je .skip_buffer                 ; Skip buffer update if full
    mov [kbd_buffer_tail], bx

.skip_buffer:
    ; Post KEY_PRESS event (Foundation 1.5)
    push ax
    xor dx, dx
    mov dl, al                      ; DX = ASCII character
    mov al, EVENT_KEY_PRESS         ; AL = event type
    call post_event
    pop ax
    jmp .done

.shift_press:
    mov byte [kbd_shift_state], 1
    jmp .done

.shift_release:
    mov byte [kbd_shift_state], 0
    jmp .done

.ctrl_press:
    mov byte [kbd_ctrl_state], 1
    jmp .done

.ctrl_release:
    mov byte [kbd_ctrl_state], 0
    jmp .done

.alt_press:
    mov byte [kbd_alt_state], 1
    jmp .done

.alt_release:
    mov byte [kbd_alt_state], 0

.done:
    ; Send EOI to PIC
    mov al, 0x20
    out 0x20, al

    pop ds
    pop bx
    pop ax
    iret

; Get next character from keyboard buffer (non-blocking)
; Output: AL = ASCII character, 0 if no key available
kbd_getchar:
    push bx
    push ds

    mov ax, 0x1000
    mov ds, ax

    mov bx, [kbd_buffer_head]
    cmp bx, [kbd_buffer_tail]
    je .no_key

    mov al, [kbd_buffer + bx]
    inc bx
    and bx, 0x0F
    mov [kbd_buffer_head], bx

    pop ds
    pop bx
    ret

.no_key:
    xor al, al
    pop ds
    pop bx
    ret

; Wait for keypress (blocking)
; Output: AL = ASCII character
kbd_wait_key:
    call kbd_getchar
    test al, al
    jz kbd_wait_key
    ret

; Clear keyboard buffer and event queue
; Drains all pending keys
clear_kbd_buffer:
    push ax
.drain_loop:
    call kbd_getchar
    test al, al
    jnz .drain_loop
    ; Also drain event queue
.drain_events:
    call event_get_stub
    test al, al
    jnz .drain_events
    pop ax
    ret

; ============================================================================
; Version String
; ============================================================================

version_string: db 'UnoDOS v3.10.1', 0
build_string:   db 'Build: debug01', 0

; ============================================================================
; Filesystem Test - Tests FAT12 Driver (v3.10.0)
; ============================================================================

test_filesystem:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    ; Clear previous display area
    mov bx, 0
    mov cx, 10
    mov dx, 320
    mov si, 190
    call gfx_clear_area_stub

    ; Display instruction to insert test floppy
    mov bx, 10
    mov cx, 30
    mov si, .insert_msg
    call gfx_draw_string_stub

    ; Clear keyboard buffer (F key still in buffer from previous press)
    call clear_kbd_buffer

    ; Wait for keypress
    call kbd_wait_key

    ; Display "Testing..." message
    mov bx, 10
    mov cx, 55
    mov si, .testing_msg
    call gfx_draw_string_stub

    ; Small delay to let floppy drive settle after swap
    mov cx, 0x8000
.settle_delay:
    nop
    loop .settle_delay

    ; Try to mount filesystem
    mov al, 0                       ; Drive A:
    xor ah, ah                      ; Auto-detect
    call fs_mount_stub
    jc .mount_failed

    ; Display mount success
    mov bx, 10
    mov cx, 70
    mov si, .mount_ok
    call gfx_draw_string_stub

    ; Try to open TEST.TXT
    xor bx, bx                      ; Mount handle 0
    mov si, .filename
    call fs_open_stub
    jc .open_failed

    ; Display open success
    push ax                         ; Save file handle
    mov bx, 10
    mov cx, 100
    mov si, .open_ok
    call gfx_draw_string_stub
    pop ax                          ; Restore file handle

    ; Read file contents (up to 1024 bytes for multi-cluster test)
    push ax                         ; Save file handle
    mov bx, 0x1000
    mov es, bx
    mov di, fs_read_buffer
    mov cx, 1024                    ; Read up to 1024 bytes (multi-cluster)
    call fs_read_stub
    jc .read_failed

    ; Display read success
    mov bx, 10
    mov cx, 115
    mov si, .read_ok
    call gfx_draw_string_stub

    ; Display file contents at Y=130
    mov bx, 10
    mov cx, 130
    mov si, fs_read_buffer
    call gfx_draw_string_stub

    ; Close file
    pop ax                          ; Restore file handle
    call fs_close_stub

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.mount_failed:
    mov bx, 10
    mov cx, 70
    mov si, .mount_err
    call gfx_draw_string_stub

    ; Show error code for debugging
    mov bx, 180
    mov cx, 85
    mov si, .err_code_msg
    call gfx_draw_string_stub

    ; Display error code in AX (saved from mount call)
    ; For now just show generic error

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.open_failed:
    mov bx, 10
    mov cx, 100
    mov si, .open_err
    call gfx_draw_string_stub
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.read_failed:
    pop ax                          ; Clean up file handle
    mov bx, 10
    mov cx, 115
    mov si, .read_err
    call gfx_draw_string_stub
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.test_msg:      db 'FAT12 Filesystem Test', 0
.insert_msg:    db 'Insert test disk, press key', 0
.testing_msg:   db 'Testing...', 0
.mount_ok:      db 'Mount: OK', 0
.mount_err:     db 'Mount: FAIL', 0
.err_code_msg:  db 'Err:', 0
.dir_label:     db 'Dir:', 0
.no_entry:      db '(empty)', 0
.trying_open:   db 'Trying open...', 0
.open_ok:       db 'Open TEST.TXT: OK', 0
.open_err:      db 'Open TEST.TXT: FAIL', 0
.read_ok:       db 'Read: OK - File contents:', 0
.read_err:      db 'Read: FAIL', 0
.filename:      db 'TEST.TXT', 0

; ============================================================================
; Keyboard Input Demo - Tests Foundation Layer (1.1-1.4)
; ============================================================================

keyboard_demo:
    push ax
    push bx
    push cx
    push si

    ; Initialize cursor position for input
    mov word [demo_cursor_x], 10
    mov word [demo_cursor_y], 160

.input_loop:
    ; Wait for event (event-driven approach)
    call event_wait_stub            ; Returns AL=type, DX=data

    ; Check event type
    cmp al, EVENT_KEY_PRESS
    jne .input_loop                 ; Ignore non-keyboard events

    ; Extract ASCII character from event data
    mov al, dl                      ; AL = ASCII character from DX

    ; Check for ESC (exit)
    cmp al, 27
    je .exit

    ; Check for 'F' or 'f' (filesystem test)
    cmp al, 'F'
    je .file_test
    cmp al, 'f'
    je .file_test

    ; Check for Enter (newline)
    cmp al, 13
    je .handle_enter

    ; Check for Backspace
    cmp al, 8
    je .handle_backspace

    ; Check for printable characters (space to ~)
    cmp al, 32
    jb .input_loop                  ; Skip control characters
    cmp al, 126
    ja .input_loop                  ; Skip extended ASCII

    ; Draw character at cursor position
    mov bx, [demo_cursor_x]
    mov cx, [demo_cursor_y]
    call gfx_draw_char_stub         ; AL already contains character

    ; Advance cursor
    add word [demo_cursor_x], 8     ; 8 pixels per character
    cmp word [demo_cursor_x], 310   ; Check right edge
    jl .input_loop

    ; Wrap to next line
    mov word [demo_cursor_x], 10
    add word [demo_cursor_y], 10
    cmp word [demo_cursor_y], 195   ; Check bottom edge
    jl .input_loop

    ; Reset to top if at bottom
    mov word [demo_cursor_y], 185
    jmp .input_loop

.handle_enter:
    ; Move to next line
    mov word [demo_cursor_x], 10
    add word [demo_cursor_y], 10
    cmp word [demo_cursor_y], 195
    jl .input_loop
    mov word [demo_cursor_y], 185
    jmp .input_loop

.handle_backspace:
    ; Move cursor back (simple version - doesn't erase)
    cmp word [demo_cursor_x], 10    ; Already at start of line?
    jle .input_loop
    sub word [demo_cursor_x], 8
    jmp .input_loop

.file_test:
    ; Restore registers before calling test_filesystem
    pop si
    pop cx
    pop bx
    pop ax

    ; Call filesystem test
    call test_filesystem

    ; Return (test_filesystem will halt or continue)
    ret

.exit:
    ; Display exit message
    mov bx, 10
    mov cx, 195
    mov si, .exit_msg
    call gfx_draw_string_stub

    pop si
    pop cx
    pop bx
    pop ax
    ret

.prompt: db 'Type text (ESC to exit, F for file test):', 0
.instruction: db 'Event System + Graphics API', 0
.exit_msg: db 'Event demo complete!', 0

; Scan code to ASCII translation tables
scancode_normal:
    db 0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,9
    db 'q','w','e','r','t','y','u','i','o','p','[',']',13,0,'a','s'
    db 'd','f','g','h','j','k','l',';',39,'`',0,92,'z','x','c','v'
    db 'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0
    db 0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1'
    db '2','3','0','.',0,0,0,0,0,0,0,0,0,0,0,0

scancode_shifted:
    db 0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,9
    db 'Q','W','E','R','T','Y','U','I','O','P','{','}',13,0,'A','S'
    db 'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V'
    db 'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0
    db 0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1'
    db '2','3','0','.',0,0,0,0,0,0,0,0,0,0,0,0

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
; Kernel API Table - At fixed address 0x1000:0x0800
; ============================================================================

; Pad to exactly offset 0x0800 (2048 bytes) - expanded for FAT12 (v3.10.0)
times 0x0800 - ($ - $$) db 0

kernel_api_table:
    ; Header
    dw 0x4B41                       ; Magic: 'KA' (Kernel API)
    dw 0x0001                       ; Version: 1.0
    dw 17                           ; Number of function slots
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

    ; Keyboard Input (Foundation 1.4)
    dw kbd_getchar                  ; 10: Get character (non-blocking)
    dw kbd_wait_key                 ; 11: Wait for key (blocking)

    ; Filesystem API (Foundation 1.6)
    dw fs_mount_stub                ; 12: Mount filesystem
    dw fs_open_stub                 ; 13: Open file
    dw fs_read_stub                 ; 14: Read from file
    dw fs_close_stub                ; 15: Close file
    dw fs_register_driver_stub      ; 16: Register filesystem driver

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

; ============================================================================
; Event System (Foundation 1.5)
; ============================================================================

; Event Types
EVENT_NONE          equ 0
EVENT_KEY_PRESS     equ 1
EVENT_KEY_RELEASE   equ 2           ; Future
EVENT_TIMER         equ 3           ; Future
EVENT_MOUSE         equ 4           ; Future

; Event structure (3 bytes):
;   +0: type (byte)
;   +1: data (word) - key code, timer value, mouse position, etc.

; Post event to event queue
; Input: AL = event type, DX = event data
; Preserves: All registers
post_event:
    push bx
    push si
    push ds

    mov bx, 0x1000
    mov ds, bx

    ; Calculate tail position (events are 3 bytes each)
    mov bx, [event_queue_tail]
    mov si, bx
    add si, bx                      ; SI = tail * 2
    add si, bx                      ; SI = tail * 3

    ; Store event in queue
    mov [event_queue + si], al      ; type
    mov [event_queue + si + 1], dx  ; data (word)

    ; Advance tail
    inc bx
    and bx, 0x1F                    ; Wrap at 32 events
    cmp bx, [event_queue_head]      ; Buffer full?
    je .done                        ; Skip if full
    mov [event_queue_tail], bx

.done:
    pop ds
    pop si
    pop bx
    ret

; event_get_stub - Get next event (non-blocking)
; Output: AL = event type (0 if no event), DX = event data
; Preserves: BX, CX, SI, DI
event_get_stub:
    push bx
    push si
    push ds

    mov bx, 0x1000
    mov ds, bx

    mov bx, [event_queue_head]
    cmp bx, [event_queue_tail]
    je .no_event

    ; Calculate head position (events are 3 bytes each)
    mov si, bx
    add si, bx                      ; SI = head * 2
    add si, bx                      ; SI = head * 3

    ; Read event from queue
    mov al, [event_queue + si]      ; type
    mov dx, [event_queue + si + 1]  ; data (word)

    ; Advance head
    inc bx
    and bx, 0x1F                    ; Wrap at 32 events
    mov [event_queue_head], bx

    pop ds
    pop si
    pop bx
    ret

.no_event:
    xor al, al                      ; EVENT_NONE
    xor dx, dx
    pop ds
    pop si
    pop bx
    ret

; event_wait_stub - Wait for event (blocking)
; Output: AL = event type, DX = event data
; Preserves: BX, CX, SI, DI
event_wait_stub:
    call event_get_stub
    test al, al
    jz event_wait_stub              ; Loop if no event
    ret

; ============================================================================
; Filesystem Abstraction Layer (Foundation 1.6)
; ============================================================================

; Filesystem error codes
FS_OK                   equ 0
FS_ERR_NOT_FOUND        equ 1
FS_ERR_NO_DRIVER        equ 2
FS_ERR_READ_ERROR       equ 3
FS_ERR_INVALID_HANDLE   equ 4
FS_ERR_NO_HANDLES       equ 5

; fs_mount_stub - Mount a filesystem on a drive
; Input: AL = drive number (0=A:, 1=B:, 0x80=HDD0)
;        AH = driver ID (0=auto-detect, 1-3=specific driver)
; Output: CF = 0 on success, CF = 1 on error
;         AX = error code if CF=1
;         BX = mount handle if CF=0
; Preserves: CX, DX, SI, DI
fs_mount_stub:
    push si
    push di

    ; For now, only support drive 0 (A:) with FAT12
    cmp al, 0
    jne .unsupported

    ; Try to mount FAT12
    call fat12_mount
    jc .error

    ; Success - return mount handle 0
    xor bx, bx
    clc
    pop di
    pop si
    ret

.unsupported:
.error:
    mov ax, FS_ERR_NO_DRIVER
    stc
    pop di
    pop si
    ret

; fs_open_stub - Open a file for reading
; Input: BX = mount handle (from fs_mount)
;        DS:SI = pointer to filename (null-terminated, "FILENAME.EXT")
; Output: CF = 0 on success, CF = 1 on error
;         AX = file handle (0-15) if CF=0, error code if CF=1
; Preserves: BX, CX, DX
fs_open_stub:
    push bx
    push di

    ; For now, only support mount handle 0 (FAT12)
    test bx, bx
    jnz .invalid_mount

    ; Call FAT12 open
    call fat12_open
    jc .error

    ; Success - return file handle in AX
    clc
    pop di
    pop bx
    ret

.invalid_mount:
    mov ax, FS_ERR_NO_DRIVER
    stc
    pop di
    pop bx
    ret

.error:
    ; Error code already in AX
    stc
    pop di
    pop bx
    ret

; fs_read_stub - Read data from file
; Input: AX = file handle
;        ES:DI = buffer to read into
;        CX = number of bytes to read
; Output: CF = 0 on success, CF = 1 on error
;         AX = actual bytes read if CF=0, error code if CF=1
; Preserves: BX, DX
fs_read_stub:
    push bx
    push si

    ; Validate file handle (0-15)
    cmp ax, 16
    jae .invalid_handle

    ; Call FAT12 read
    call fat12_read
    jc .error

    ; Success - bytes read in AX
    clc
    pop si
    pop bx
    ret

.invalid_handle:
    mov ax, FS_ERR_INVALID_HANDLE
    stc
    pop si
    pop bx
    ret

.error:
    ; Error code already in AX
    stc
    pop si
    pop bx
    ret

; fs_close_stub - Close an open file
; Input: AX = file handle
; Output: CF = 0 on success, CF = 1 on error
;         AX = error code if CF=1
; Preserves: BX, CX, DX
fs_close_stub:
    push si

    ; Validate file handle (0-15)
    cmp ax, 16
    jae .invalid_handle

    ; Call FAT12 close
    call fat12_close
    jc .error

    ; Success
    clc
    pop si
    ret

.invalid_handle:
    mov ax, FS_ERR_INVALID_HANDLE
    stc
    pop si
    ret

.error:
    ; Error code already in AX
    stc
    pop si
    ret

; fs_register_driver_stub - Register a loadable filesystem driver
; Input: ES:BX = pointer to driver structure
; Output: CF = 0 on success, CF = 1 on error
;         AX = driver ID (0-3) if CF=0, error code if CF=1
; Preserves: BX, CX, DX
fs_register_driver_stub:
    ; Not implemented in v3.10.0 - reserved for Tier 2/3
    mov ax, FS_ERR_NO_DRIVER
    stc
    ret

; ============================================================================
; FAT12 Driver Implementation
; ============================================================================

; FAT12 mount - Initialize FAT12 filesystem on drive A:
; Input: None (always uses drive 0)
; Output: CF = 0 on success, CF = 1 on error
;         AX = error code if CF=1
; Preserves: BX, CX, DX, SI, DI
fat12_mount:
    push es
    push bx
    push cx
    push dx
    push si
    push di

    ; Reset disk system first (important after floppy swap!)
    xor ax, ax                      ; AH=00 (reset disk system)
    xor dx, dx                      ; DL=0 (drive A:)
    int 0x13
    jc .read_error

    ; Small delay for drive to spin up
    push cx
    mov cx, 0x4000
.spinup:
    nop
    loop .spinup
    pop cx

    ; Read boot sector (sector 0) into bpb_buffer
    ; Try multiple times in case drive not ready
    push bp
    mov bp, 3                       ; Retry count in BP

.retry_read:
    mov ax, 0x0201                  ; AH=02 (read), AL=01 (1 sector)
    mov cx, 0x0001                  ; CH=0 (cylinder), CL=1 (sector)
    mov dx, 0x0000                  ; DH=0 (head), DL=0 (drive A:)
    push es
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    int 0x13
    pop es
    jnc .read_success

    ; Retry on error
    dec bp
    jnz .retry_read
    pop bp
    jmp .read_error

.read_success:
    pop bp                          ; Restore BP after successful read

    ; Validate boot sector signature (0xAA55 at offset 510)
    cmp word [bpb_buffer + 510], 0xAA55
    jne .read_error

    ; Parse BPB (BIOS Parameter Block)
    ; Bytes per sector at offset 0x0B
    mov ax, [bpb_buffer + 0x0B]
    cmp ax, 512                     ; Must be 512 for FAT12
    jne .read_error
    mov [bytes_per_sector], ax

    ; Sectors per cluster at offset 0x0D
    mov al, [bpb_buffer + 0x0D]
    test al, al                     ; Must not be zero
    jz .read_error
    mov [sectors_per_cluster], al

    ; Reserved sectors at offset 0x0E
    mov ax, [bpb_buffer + 0x0E]
    mov [reserved_sectors], ax

    ; Number of FATs at offset 0x10
    mov al, [bpb_buffer + 0x10]
    mov [num_fats], al

    ; Root directory entries at offset 0x11
    mov ax, [bpb_buffer + 0x11]
    mov [root_dir_entries], ax

    ; Sectors per FAT at offset 0x16
    mov ax, [bpb_buffer + 0x16]
    mov [sectors_per_fat], ax

    ; Calculate root directory start sector
    ; root_dir_start = reserved + (num_fats * sectors_per_fat)
    mov ax, [reserved_sectors]
    mov bl, [num_fats]
    xor bh, bh
    mov cx, [sectors_per_fat]
    push ax
    mov ax, cx
    mul bx                          ; AX = num_fats * sectors_per_fat
    mov bx, ax
    pop ax
    add ax, bx
    mov [root_dir_start], ax

    ; Calculate data area start sector
    ; data_start = root_dir_start + root_dir_sectors
    ; root_dir_sectors = (root_dir_entries * 32) / bytes_per_sector
    mov ax, [root_dir_entries]
    mov cx, 32
    mul cx                          ; AX = root_dir_entries * 32
    mov cx, [bytes_per_sector]
    xor dx, dx
    div cx                          ; AX = root_dir_sectors
    mov bx, ax
    mov ax, [root_dir_start]
    add ax, bx
    mov [data_area_start], ax

    ; Success
    xor ax, ax
    clc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.read_error:
    mov ax, FS_ERR_READ_ERROR
    stc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

; fat12_open - Open a file from root directory
; Input: DS:SI = pointer to filename (null-terminated, "FILENAME.EXT")
; Output: CF = 0 on success, CF = 1 on error
;         AX = file handle (0-15) if CF=0, error code if CF=1
; Preserves: BX, CX, DX
fat12_open:
    push es
    push bx
    push cx
    push dx
    push si
    push di

    ; Convert filename to 8.3 FAT format (padded with spaces)
    ; Allocate 11 bytes on stack
    sub sp, 11
    mov di, sp
    push ss
    pop es

    ; Initialize with spaces
    mov cx, 11
    mov al, ' '
    push di
    rep stosb
    pop di

    ; Copy filename (up to 8 chars before '.')
    mov cx, 8
.copy_name:
    lodsb
    test al, al
    jz .name_done
    cmp al, '.'
    je .copy_ext
    mov [es:di], al
    inc di
    loop .copy_name

    ; Skip remaining chars before '.'
.skip_to_dot:
    lodsb
    test al, al
    jz .name_done
    cmp al, '.'
    jne .skip_to_dot

.copy_ext:
    ; Copy extension (up to 3 chars)
    mov di, sp
    add di, 8                       ; Point to extension part
    mov cx, 3
.copy_ext_loop:
    lodsb
    test al, al
    jz .name_done
    mov [es:di], al
    inc di
    loop .copy_ext_loop

.name_done:
    ; Now search root directory
    ; Root directory starts at sector [root_dir_start]
    ; Each entry is 32 bytes, max 224 entries (14 sectors for 360KB floppy)

    mov ax, [root_dir_start]
    mov cx, 14                      ; Max 14 sectors for root dir

.search_next_sector:
    push cx
    push ax

    ; Read one sector of root directory
    push ax                         ; Save sector number

    ; LBA to CHS conversion for floppy
    ; sector = (LBA % sectors_per_track) + 1
    ; head = (LBA / sectors_per_track) % num_heads
    ; cylinder = LBA / (sectors_per_track * num_heads)
    xor dx, dx
    mov bx, 18                      ; sectors per track (1.44MB floppy)
    div bx                          ; AX = LBA / 18, DX = LBA % 18
    inc dx                          ; DX = sector (1-based)
    mov cl, dl                      ; CL = sector

    xor dx, dx
    mov bx, 2                       ; num heads (1.44MB floppy)
    div bx                          ; AX = cylinder, DX = head
    mov ch, al                      ; CH = cylinder
    mov dh, dl                      ; DH = head

    ; Now read the sector
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    mov ax, 0x0201                  ; AH=02 (read), AL=01 (1 sector)
    mov dl, 0                       ; DL = drive A:
    int 0x13

    pop ax                          ; Restore sector number
    jc .read_error_cleanup

    ; Search through 16 entries in this sector
    ; Set DS to 0x1000 so we can read from bpb_buffer correctly
    push ds
    mov ax, 0x1000
    mov ds, ax
    mov cx, 16
    mov si, bpb_buffer

.search_entry:
    ; Check if entry is free (first byte = 0x00 or 0xE5)
    mov al, [si]
    test al, al
    jz .end_of_dir                  ; End of directory (need to pop DS first!)
    cmp al, 0xE5
    je .next_entry                  ; Deleted entry

    ; Skip special entries: VFAT long filenames (0x0F) and volume labels (0x08)
    ; Also skip directories, system files, hidden files (match debug code logic)
    push ds
    push si
    mov ax, 0x1000
    mov ds, ax
    mov al, [si + 0x0B]             ; Read attribute byte
    pop si
    pop ds
    cmp al, 0x0F                    ; Long filename entry?
    je .next_entry                  ; Skip it
    test al, 0x08                   ; Volume label bit set?
    jnz .next_entry                 ; Skip volume labels
    test al, 0x10                   ; Directory bit set?
    jnz .next_entry                 ; Skip directories (SYSTEM~1)
    test al, 0x04                   ; System bit set?
    jnz .next_entry                 ; Skip system files
    test al, 0x02                   ; Hidden bit set?
    jnz .next_entry                 ; Skip hidden files

    ; Compare filename (11 bytes)
    ; DEBUG: Show directory entry first char and our search name first char
    push ax
    push bx
    push cx
    push dx
    push ds
    ; Show 'D:' then first char of directory entry
    push cs
    pop ds
    mov bx, 10
    mov cx, 85                      ; Y position for debug
    mov si, .dbg_dir
    call gfx_draw_string_stub
    ; Get first char of dir entry (DS is still 0x1000 from earlier, SI points to entry)
    pop ds                          ; Restore DS (0x1000)
    push si
    mov al, [si]                    ; First char of directory entry
    mov [cs:.dbg_char], al
    pop si
    push cs
    pop ds
    mov bx, 30
    mov cx, 85
    mov si, .dbg_char
    call gfx_draw_string_stub
    ; Show 'S:' then first char of our 8.3 name
    mov bx, 50
    mov cx, 85
    mov si, .dbg_srch
    call gfx_draw_string_stub
    ; Get first char of our search name from stack
    ; Stack layout: [11-byte name][CX][AX][DS][...debug pushes...]
    ; Need to reach past our debug pushes (5*2=10) + DS(2) + AX(2) + CX(2) = 16 bytes
    mov bp, sp
    add bp, 16
    mov al, [ss:bp]
    mov [.dbg_char], al
    mov bx, 70
    mov cx, 85
    mov si, .dbg_char
    call gfx_draw_string_stub
    pop dx
    pop cx
    pop bx
    pop ax
    ; Restore DS to 0x1000 for actual comparison
    push ax
    mov ax, 0x1000
    mov ds, ax
    pop ax

    ; Push everything FIRST, then calculate pointer
    push si                         ; Save directory entry pointer
    push di
    push ds
    push si                         ; SI for cmpsb source
    ; Now calculate DI to point to our 8.3 name on stack
    ; Stack: [name][CX][AX][DS from 1608][saved SI][saved DI][saved DS][SI for cmpsb] ← SP
    mov di, sp
    add di, 14                      ; Skip SI(2), DS(2), DI(2), SI(2), DS(2), AX(2), CX(2) = 14 bytes
    push ss
    pop es                          ; ES = SS (stack segment for our name)
    mov ax, 0x1000
    mov ds, ax                      ; DS = 0x1000 (for directory entry)
    mov cx, 11
    repe cmpsb                      ; Compare DS:SI (directory) with ES:DI (our name)
    pop si
    pop ds
    pop di
    pop si
    je .found_file
    jmp .next_entry                 ; Comparison failed, try next entry

.end_of_dir:
    ; End of directory reached (first byte was 0x00)
    ; DS is on stack, need to pop it before cleanup
    pop ds
    jmp .not_found_cleanup

.next_entry:
    add si, 32                      ; Next directory entry
    dec cx
    jnz .search_entry               ; Use jnz instead of loop (longer range)

    ; Move to next sector
    pop ds                          ; Restore DS (we pushed it before .search_entry)
    pop ax
    pop cx
    inc ax
    dec cx
    jnz .search_next_sector

    ; Searched all sectors, file not found (CX/AX already popped)
    add sp, 11                      ; Clean up 8.3 filename only
    mov ax, FS_ERR_NOT_FOUND
    stc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.not_found_cleanup:
    ; Jumped here from inside search loop (CX/AX still on stack)
    add sp, 2                       ; Clean up sector counter
    add sp, 2                       ; Clean up sector number
    add sp, 11                      ; Clean up 8.3 filename
    mov ax, FS_ERR_NOT_FOUND
    stc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.read_error_cleanup:
    add sp, 2                       ; Clean up sector counter
    add sp, 2                       ; Clean up sector number
    add sp, 11                      ; Clean up 8.3 filename
    mov ax, FS_ERR_READ_ERROR
    stc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.found_file:
    ; SI points to directory entry
    ; Extract: starting cluster (offset 0x1A), file size (offset 0x1C)
    mov ax, 0x1000
    mov ds, ax
    mov ax, [si + 0x1A]             ; Starting cluster
    mov bx, [si + 0x1C]             ; File size (low word)
    mov dx, [si + 0x1E]             ; File size (high word)

    ; Restore DS
    push cs
    pop ds

    ; Clean up stack
    add sp, 2                       ; sector counter
    add sp, 2                       ; sector number
    add sp, 11                      ; 8.3 filename

    ; Find free file handle
    push ax
    push bx
    push dx
    call alloc_file_handle
    pop dx
    pop bx
    pop cx                          ; CX = starting cluster (was AX)
    jc .no_handles

    ; AX now contains file handle index
    ; Initialize file handle entry
    mov di, ax
    shl di, 5                       ; DI = handle * 32 (entry size)
    add di, file_table

    mov byte [di], 1                ; Status = open
    mov byte [di + 1], 0            ; Mount handle = 0
    mov [di + 2], cx                ; Starting cluster
    mov [di + 4], bx                ; File size (low)
    mov [di + 6], dx                ; File size (high)
    mov word [di + 8], 0            ; Current position (low)
    mov word [di + 10], 0           ; Current position (high)

    ; Return file handle in AX (already set)
    clc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.no_handles:
    mov ax, FS_ERR_NO_HANDLES
    stc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

; Debug strings for fat12_open (local labels)
.dbg_dir:   db 'D:', 0
.dbg_srch:  db 'S:', 0
.dbg_char:  db ' ', 0, 0            ; Single char + null + padding

; alloc_file_handle - Find a free file handle
; Output: CF = 0 on success, CF = 1 if no handles available
;         AX = file handle index (0-15) if CF=0
; Preserves: BX, CX, DX, SI, DI
alloc_file_handle:
    push si
    xor ax, ax
    mov cx, 16
    mov si, file_table

.check_handle:
    cmp byte [si], 0                ; Status = 0 (free)?
    je .found
    add si, 32
    inc ax
    loop .check_handle

    ; No free handles
    stc
    pop si
    ret

.found:
    clc
    pop si
    ret

; ============================================================================
; get_next_cluster - Read next cluster from FAT12 chain
; ============================================================================
; Input: AX = current cluster number
; Output: AX = next cluster number
;         CF = 1 if end-of-chain (or error), CF = 0 if valid cluster
; Preserves: BX, CX, DX, SI, DI, ES
; Algorithm:
;   FAT12 stores 12-bit entries: offset = (cluster * 3) / 2
;   If cluster is even: value = word[offset] & 0x0FFF
;   If cluster is odd:  value = word[offset] >> 4
;   End-of-chain: >= 0xFF8
get_next_cluster:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es

    ; Save cluster number for even/odd test
    mov bp, ax                      ; BP = original cluster number

    ; Calculate FAT offset: (cluster * 3) / 2
    mov bx, ax
    mov cx, ax
    shl ax, 1                       ; AX = cluster * 2
    add ax, bx                      ; AX = cluster * 3
    shr ax, 1                       ; AX = (cluster * 3) / 2 = byte offset in FAT
    mov si, ax                      ; SI = FAT byte offset

    ; Calculate FAT sector number
    ; FAT sector = reserved_sectors + (byte_offset / 512)
    xor dx, dx
    mov cx, 512
    div cx                          ; AX = sector offset in FAT, DX = byte offset in sector
    mov di, dx                      ; DI = byte offset within sector
    add ax, [reserved_sectors]      ; AX = absolute FAT sector number

    ; Check if this FAT sector is already cached
    cmp ax, [fat_cache_sector]
    je .sector_cached

    ; Load FAT sector into cache
    mov [fat_cache_sector], ax      ; Update cached sector number

    push ax
    mov bx, 0x1000
    mov es, bx
    mov bx, fat_cache

    ; Convert sector to CHS for INT 13h
    mov cx, ax                      ; CX = sector
    and cx, 0x003F
    inc cl                          ; CL = sector (1-based)
    mov ax, cx
    shr ax, 6
    mov ch, al                      ; CH = cylinder
    mov ax, 0x0201                  ; AH=02 (read), AL=01 (1 sector)
    mov dx, 0x0000                  ; DH=0 (head), DL=0 (drive A:)
    int 0x13
    pop ax

    jc .read_error

.sector_cached:
    ; Read 2 bytes from FAT cache at offset DI
    push ds
    mov ax, 0x1000
    mov ds, ax
    mov si, fat_cache
    add si, di
    mov ax, [si]                    ; AX = 2 bytes from FAT
    pop ds

    ; Check if original cluster number (saved in BP) is even or odd
    mov bx, bp                      ; BP has the original cluster number
    test bx, 1                      ; Test bit 0
    jnz .odd_cluster

.even_cluster:
    ; Even cluster: value = word & 0x0FFF
    and ax, 0x0FFF
    jmp .check_end_of_chain

.odd_cluster:
    ; Odd cluster: value = word >> 4
    shr ax, 4

.check_end_of_chain:
    ; Check if end of chain (>= 0xFF8)
    cmp ax, 0xFF8
    jae .end_of_chain

    ; Valid cluster - return with CF=0
    clc
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.end_of_chain:
    ; End of chain - return with CF=1
    stc
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.read_error:
    ; Disk read error - return with CF=1
    stc
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; ============================================================================
; fat12_read - Read data from file
; ============================================================================
; Input: AX = file handle
;        ES:DI = buffer to read into
;        CX = number of bytes to read
; Output: CF = 0 on success, CF = 1 on error
;         AX = actual bytes read if CF=0, error code if CF=1
; Preserves: BX, DX
fat12_read:
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Validate file handle
    cmp ax, 16
    jae .invalid_handle

    ; Get file handle entry
    mov si, ax
    shl si, 5                       ; SI = handle * 32
    add si, file_table

    ; Check if file is open
    cmp byte [si], 1
    jne .invalid_handle

    ; Get file info
    mov ax, [si + 2]                ; Starting cluster
    mov bx, [si + 4]                ; File size (low)
    mov dx, [si + 6]                ; File size (high)
    mov bp, [si + 8]                ; Current position (low)

    ; Calculate remaining bytes in file
    ; remaining = file_size - current_position
    sub bx, bp                      ; BX = remaining bytes
    jnc .check_read_size
    ; Handle high word if needed (files > 64KB not supported yet)
    xor bx, bx

.check_read_size:
    ; Limit read to remaining bytes
    cmp cx, bx
    jbe .read_start
    mov cx, bx                      ; CX = min(requested, remaining)

.read_start:
    ; For simplicity, only support reading from start of file (position = 0)
    ; Multi-cluster reads ARE supported by following FAT chain
    cmp bp, 0
    jne .not_supported

    ; Store variables in temp space (using reserved bytes in file handle)
    mov [si + 12], ax               ; Store starting cluster
    mov [si + 14], cx               ; Store bytes to read
    xor ax, ax
    mov [si + 16], ax               ; Store bytes read = 0

.cluster_loop:
    ; Check if we've read all requested bytes
    mov cx, [si + 14]               ; CX = bytes remaining
    test cx, cx
    jz .read_complete_multi

    ; Get current cluster
    mov ax, [si + 12]               ; AX = current cluster

    ; Convert cluster to sector
    sub ax, 2
    xor bh, bh
    mov bl, [sectors_per_cluster]
    mul bx
    add ax, [data_area_start]       ; AX = sector number

    ; Read sector into bpb_buffer
    push es
    push di
    push si

    mov bx, 0x1000
    push bx
    pop ds
    mov bx, bpb_buffer

    ; Convert LBA to CHS
    push ax
    mov cx, ax
    and cx, 0x003F
    inc cl                          ; CL = sector (1-based)
    mov ax, cx
    shr ax, 6
    mov ch, al                      ; CH = cylinder
    mov ax, 0x0201                  ; Read 1 sector
    mov dx, 0x0000                  ; Drive A:
    int 0x13
    pop ax

    push cs
    pop ds

    pop si
    pop di
    pop es

    jc .read_error_multi

    ; Calculate bytes to copy from this cluster
    mov cx, [si + 14]               ; CX = bytes remaining
    cmp cx, 512
    jbe .bytes_ok_multi
    mov cx, 512                     ; Max 512 bytes per cluster
.bytes_ok_multi:

    ; Copy CX bytes from bpb_buffer to ES:DI
    push si
    push ax
    push ds

    mov ax, cx                      ; Save bytes to copy
    mov si, bpb_buffer
    mov bx, 0x1000
    mov ds, bx
    rep movsb                       ; Copy and advance DI automatically

    pop ds
    mov cx, ax                      ; Restore bytes copied to CX
    pop ax
    pop si

    ; Update counters (CX has bytes just copied)
    mov ax, [si + 14]               ; AX = bytes remaining before
    sub ax, cx                      ; AX = bytes remaining after
    mov [si + 14], ax               ; Store updated bytes remaining

    mov ax, [si + 16]               ; AX = total bytes read so far
    add ax, cx                      ; AX += bytes just copied
    mov [si + 16], ax               ; Store updated total
    ; Note: DI has already been advanced by movsb

    ; Get next cluster in chain
    mov ax, [si + 12]               ; Current cluster
    call get_next_cluster
    jc .read_complete_multi         ; End of chain reached

    mov [si + 12], ax               ; Store next cluster
    jmp .cluster_loop

.read_complete_multi:
    ; Update file position
    mov ax, [si + 8]                ; Current position
    add ax, [si + 16]               ; Add bytes read
    mov [si + 8], ax                ; Store new position

    ; Return bytes read
    mov ax, [si + 16]
    clc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.read_error_multi:
    mov ax, FS_ERR_READ_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.not_supported:
    pop cx
    mov ax, FS_ERR_READ_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.invalid_handle:
    mov ax, FS_ERR_INVALID_HANDLE
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.read_error:
    pop cx
    mov ax, FS_ERR_READ_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; fat12_close - Close a file
; Input: AX = file handle
; Output: CF = 0 on success, CF = 1 on error
;         AX = error code if CF=1
fat12_close:
    push si

    ; Validate file handle
    cmp ax, 16
    jae .invalid_handle

    ; Get file handle entry
    mov si, ax
    shl si, 5                       ; SI = handle * 32
    add si, file_table

    ; Mark as free
    mov byte [si], 0

    xor ax, ax
    clc
    pop si
    ret

.invalid_handle:
    mov ax, FS_ERR_INVALID_HANDLE
    stc
    pop si
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

; Keyboard driver state (Foundation 1.4)
old_int9_offset: dw 0
old_int9_segment: dw 0
kbd_buffer: times 16 db 0
kbd_buffer_head: dw 0
kbd_buffer_tail: dw 0
kbd_shift_state: db 0
kbd_ctrl_state: db 0
kbd_alt_state: db 0

; Keyboard demo state
demo_cursor_x: dw 0
demo_cursor_y: dw 0

; Debug character buffer (for displaying single chars)
char_buffer: db 0, 0
debug_x: dw 0
debug_y: dw 0

; Event system state (Foundation 1.5)
event_queue: times 96 db 0          ; 32 events * 3 bytes each
event_queue_head: dw 0
event_queue_tail: dw 0

; Filesystem state (Foundation 1.6)
; BPB (BIOS Parameter Block) cache
bpb_buffer: times 512 db 0          ; Boot sector buffer
bytes_per_sector: dw 512
sectors_per_cluster: db 1
reserved_sectors: dw 1
num_fats: db 2
root_dir_entries: dw 224
sectors_per_fat: dw 9
root_dir_start: dw 19               ; Calculated: reserved + (num_fats * sectors_per_fat)
data_area_start: dw 33              ; Calculated: root_dir_start + root_dir_sectors

; File handle table (16 entries, 32 bytes each)
; Entry format:
;   Byte 0: Status (0=free, 1=open)
;   Byte 1: Mount handle
;   Bytes 2-3: Starting cluster
;   Bytes 4-7: File size (32-bit)
;   Bytes 8-11: Current position (32-bit)
;   Bytes 12-31: Reserved
file_table: times 512 db 0          ; 16 entries * 32 bytes

; Read buffer for filesystem test (1024 bytes for multi-cluster testing)
fs_read_buffer: times 1024 db 0

; FAT cache (for cluster chain following)
; Stores one sector of FAT at a time
fat_cache: times 512 db 0
fat_cache_sector: dw 0xFFFF          ; Currently cached FAT sector (0xFFFF = invalid)

; ============================================================================
; Padding
; ============================================================================

; Pad to 28KB (56 sectors) - expanded for FAT12 filesystem (v3.10.0)
times 28672 - ($ - $$) db 0
