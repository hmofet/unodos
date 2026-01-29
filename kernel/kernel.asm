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
    ; ========== PHASE 1: Early init (before keyboard handler) ==========
    ; Save boot drive number (DL contains drive: 0x00=floppy, 0x80=HDD)
    push dx                         ; Save DL (boot drive)

    ; Set up segment registers first
    mov ax, 0x1000
    mov ds, ax
    mov es, ax

    ; Store boot drive number
    pop dx                          ; Restore DL
    mov [boot_drive], dl            ; Save for later use

    ; Print newline then kernel banner
    mov ah, 0x0E
    mov al, 13
    int 0x10
    mov al, 10
    int 0x10

    ; Print "Kernel: " prefix
    mov si, kernel_prefix
    call print_string_bios

    ; Install INT 0x80 handler for system calls
    call install_int_80
    mov ah, 0x0E
    mov al, '.'
    xor bx, bx
    int 0x10

    ; Skip mouse init for USB boot (8042 controller issues)
    mov byte [mouse_enabled], 0

    ; Print version string
    mov si, version_string
    call print_string_bios

    ; Print build string
    mov al, ' '
    mov ah, 0x0E
    int 0x10
    mov al, '('
    int 0x10
    mov si, build_string
    call print_string_bios
    mov ah, 0x0E
    mov al, ')'
    int 0x10

    ; Newline
    mov ah, 0x0E
    mov al, 13
    int 0x10
    mov al, 10
    int 0x10

    ; ========== PHASE 2: Install interrupt handlers ==========
    ; Now safe to install keyboard handler (won't break INT 16h)
    call install_keyboard

    ; Set up graphics mode (blue screen)
    call setup_graphics

    ; Initialize caller_ds for direct kernel calls to gfx_draw_string_stub
    mov word [caller_ds], 0x1000

    ; DEBUG: Draw test rectangles to confirm pixel plotting works
    ; Test rectangle at top-left (white pixels)
    mov bx, 10
    mov cx, 10
    mov dx, 50
    mov si, 20
    call gfx_draw_filled_rect_stub

    ; Ensure DS is set for string access
    mov ax, 0x1000
    mov ds, ax

    ; Draw white background rectangles for text (so black text is visible!)
    mov bx, 2                       ; X
    mov cx, 33                      ; Y
    mov dx, 150                     ; Width
    mov si, 12                      ; Height
    call gfx_draw_filled_rect_stub

    mov bx, 2
    mov cx, 43
    mov dx, 150
    mov si, 12
    call gfx_draw_filled_rect_stub

    ; Ensure DS is set
    mov ax, 0x1000
    mov ds, ax

    ; Display version number - black text on white background
    mov bx, 4
    mov cx, 35
    mov si, version_string
    call gfx_draw_string_inverted

    ; Display build number - black text on white background
    mov bx, 4
    mov cx, 45
    mov si, build_string
    call gfx_draw_string_inverted

    ; Welcome box removed - clutters test output
    ; call draw_welcome_box

    ; Enable interrupts
    sti

    ; Auto-load launcher from boot disk
    call auto_load_launcher

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

; INT 0x80 Handler - System Call Dispatcher
; Input: AH = function number (0 = discovery, 1-24 = API function)
;        Other registers = function parameters
; Output: Depends on function called
;   AH=0: ES:BX = pointer to kernel_api_table
;   AH>0: Function-specific return values, CF=error status
int_80_handler:
    cmp ah, 0x00
    jne .dispatch_function

    ; AH=0: Return pointer to API table (discovery)
    mov bx, cs
    mov es, bx
    mov bx, kernel_api_table
    iret

.dispatch_function:
    ; AH contains function index (1-29)
    ; Validate function number
    cmp ah, 30                      ; Max function + 1 (0-29)
    jae .invalid_function

    ; Save caller's DS and ES to kernel variables (use CS: since DS not yet changed)
    mov [cs:caller_ds], ds
    mov [cs:caller_es], es

    ; Save caller's DS (apps may have different DS)
    push ds

    ; Set DS to kernel segment for API functions
    push ax
    mov ax, 0x1000
    mov ds, ax
    pop ax

    ; Get function pointer from API table
    ; Function pointer = kernel_api_table + 8 + (index * 2)
    push bx                         ; Save BX (may be parameter)
    push ax                         ; Save AX (has function index in AH)
    mov bl, ah
    xor bh, bh                      ; BX = function index
    shl bx, 1                       ; BX = index * 2
    add bx, kernel_api_table + 8    ; BX = address of function pointer
    mov bx, [bx]                    ; BX = function offset
    mov [cs:syscall_func], bx       ; Save function addr (in data area)
    pop ax                          ; Restore AX
    pop bx                          ; Restore BX

    ; Call the function - it will use near RET
    ; We simulate a CALL by pushing return address then jumping
    push word .return_point
    jmp word [cs:syscall_func]

.return_point:
    ; Function returned - preserve CF from function in return FLAGS
    ; Stack: [caller's DS] [IP] [CS] [FLAGS]
    ; IMPORTANT: Do NOT destroy AX - functions return values in AX!
    pop ds                          ; Restore caller's DS
    ; Stack: [IP] [CS] [FLAGS]
    push bp
    mov bp, sp
    ; Stack: [BP] [IP] [CS] [FLAGS]
    ; [bp+0]=BP, [bp+2]=IP, [bp+4]=CS, [bp+6]=FLAGS
    jc .set_carry                   ; Test CF directly (preserves AX!)
    and word [bp+6], 0xFFFE         ; Clear CF in return FLAGS
    jmp .iret_done
.set_carry:
    or word [bp+6], 0x0001          ; Set CF in return FLAGS
.iret_done:
    pop bp
    iret

.invalid_function:
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
    push dx
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
    pop dx
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
; PS/2 Mouse Driver
; ============================================================================

; 8042 Keyboard Controller ports
KBC_DATA            equ 0x60        ; Data port (read/write)
KBC_STATUS          equ 0x64        ; Status register (read)
KBC_CMD             equ 0x64        ; Command register (write)

; 8042 Status bits
KBC_STAT_OBF        equ 0x01        ; Output buffer full (data ready)
KBC_STAT_IBF        equ 0x02        ; Input buffer full (busy)
KBC_STAT_AUXB       equ 0x20        ; Aux data (mouse vs keyboard)

; 8042 Commands
KBC_CMD_WRITE_AUX   equ 0xD4        ; Write to auxiliary device (mouse)
KBC_CMD_ENABLE_AUX  equ 0xA8        ; Enable auxiliary interface
KBC_CMD_READ_CFG    equ 0x20        ; Read configuration byte
KBC_CMD_WRITE_CFG   equ 0x60        ; Write configuration byte

; Mouse commands (sent via 0xD4)
MOUSE_CMD_RESET     equ 0xFF        ; Reset mouse
MOUSE_CMD_ENABLE    equ 0xF4        ; Enable data reporting
MOUSE_CMD_DISABLE   equ 0xF5        ; Disable data reporting
MOUSE_CMD_DEFAULTS  equ 0xF6        ; Set defaults

; install_mouse - Initialize PS/2 mouse
; Output: CF=0 success, CF=1 no mouse detected
install_mouse:
    push ax
    push bx
    push es

    ; Save original INT 0x74 (IRQ12) vector
    xor ax, ax
    mov es, ax
    mov ax, [es:0x01D0]             ; INT 0x74 offset (0x74 * 4 = 0x1D0)
    mov [old_int74_offset], ax
    mov ax, [es:0x01D2]             ; INT 0x74 segment
    mov [old_int74_segment], ax

    ; Enable auxiliary (mouse) interface
    call kbc_wait_write
    mov al, KBC_CMD_ENABLE_AUX
    out KBC_CMD, al

    ; Read controller configuration
    call kbc_wait_write
    mov al, KBC_CMD_READ_CFG
    out KBC_CMD, al
    call kbc_wait_read
    in al, KBC_DATA
    mov bl, al                      ; Save config

    ; Enable IRQ12 (bit 1) and auxiliary clock (clear bit 5)
    or bl, 0x02                     ; Enable IRQ12
    and bl, 0xDF                    ; Enable aux clock

    ; Write updated configuration
    call kbc_wait_write
    mov al, KBC_CMD_WRITE_CFG
    out KBC_CMD, al
    call kbc_wait_write
    mov al, bl
    out KBC_DATA, al

    ; Reset mouse
    mov al, MOUSE_CMD_RESET
    call mouse_send_cmd
    cmp al, 0xFA                    ; ACK?
    jne .no_mouse

    ; Wait for self-test result (0xAA) and device ID (0x00)
    call kbc_wait_read
    in al, KBC_DATA                 ; Should be 0xAA
    cmp al, 0xAA
    jne .no_mouse

    call kbc_wait_read
    in al, KBC_DATA                 ; Should be 0x00 (standard mouse)

    ; Set defaults
    mov al, MOUSE_CMD_DEFAULTS
    call mouse_send_cmd

    ; Enable data reporting
    mov al, MOUSE_CMD_ENABLE
    call mouse_send_cmd
    cmp al, 0xFA
    jne .no_mouse

    ; Install our interrupt handler
    cli
    xor ax, ax
    mov es, ax
    mov word [es:0x01D0], int_74_handler
    mov word [es:0x01D2], 0x1000    ; Kernel segment
    sti

    ; Enable IRQ12 in slave PIC (unmask)
    in al, 0xA1                     ; Read slave PIC mask
    and al, 0xEF                    ; Clear bit 4 (IRQ12)
    out 0xA1, al

    ; Initialize mouse state
    mov word [mouse_x], 160
    mov word [mouse_y], 100
    mov byte [mouse_buttons], 0
    mov byte [mouse_packet_idx], 0
    mov byte [mouse_enabled], 1

    clc
    jmp .done

.no_mouse:
    mov byte [mouse_enabled], 0
    stc

.done:
    pop es
    pop bx
    pop ax
    ret

; kbc_wait_write - Wait for keyboard controller ready to accept command
; Clobbers: AL
kbc_wait_write:
    push cx
    mov cx, 0xFFFF
.wait:
    in al, KBC_STATUS
    test al, KBC_STAT_IBF           ; Input buffer full?
    jz .done                        ; No, ready to write
    loop .wait
.done:
    pop cx
    ret

; kbc_wait_read - Wait for keyboard controller to have data
; Clobbers: AL
kbc_wait_read:
    push cx
    mov cx, 0xFFFF
.wait:
    in al, KBC_STATUS
    test al, KBC_STAT_OBF           ; Output buffer full?
    jnz .done                       ; Yes, data available
    loop .wait
.done:
    pop cx
    ret

; mouse_send_cmd - Send command to mouse via 8042 controller
; Input: AL = command byte
; Output: AL = response (0xFA = ACK)
mouse_send_cmd:
    push bx
    mov bl, al                      ; Save command

    call kbc_wait_write
    mov al, KBC_CMD_WRITE_AUX       ; Tell 8042 next byte goes to mouse
    out KBC_CMD, al

    call kbc_wait_write
    mov al, bl                      ; Send actual command
    out KBC_DATA, al

    call kbc_wait_read
    in al, KBC_DATA                 ; Read response

    pop bx
    ret

; INT 0x74 - IRQ12 PS/2 Mouse Handler
int_74_handler:
    push ax
    push bx
    push cx
    push dx
    push ds

    mov ax, 0x1000
    mov ds, ax

    ; Check if this is really mouse data (aux bit set)
    in al, KBC_STATUS
    test al, KBC_STAT_AUXB
    jz .not_mouse

    ; Read mouse byte
    in al, KBC_DATA

    ; Store byte in packet buffer
    xor bx, bx
    mov bl, [mouse_packet_idx]
    cmp bl, 3
    jae .send_eoi                   ; Overflow protection
    mov [mouse_packet + bx], al
    inc byte [mouse_packet_idx]

    ; First byte must have bit 3 set (sync bit)
    cmp bl, 0
    jne .check_complete
    test al, 0x08                   ; Bit 3 = always 1 in first byte
    jz .resync                      ; Not synced, reset

.check_complete:
    ; Check if packet complete (3 bytes)
    cmp byte [mouse_packet_idx], 3
    jne .send_eoi

    ; Parse complete packet
    ; Byte 0: YO XO YS XS 1 M R L
    ;   YO/XO = overflow, YS/XS = sign, M/R/L = buttons
    ; Byte 1: X movement (signed 9-bit with XS)
    ; Byte 2: Y movement (signed 9-bit with YS)

    ; Get buttons (bits 0-2 of byte 0)
    mov al, [mouse_packet]
    and al, 0x07
    mov [mouse_buttons], al

    ; Check for overflow - skip if overflow
    mov al, [mouse_packet]
    test al, 0xC0                   ; XO or YO set?
    jnz .reset_packet

    ; Calculate delta X (signed)
    mov al, [mouse_packet + 1]      ; X movement
    mov ah, [mouse_packet]
    test ah, 0x10                   ; X sign bit
    jz .x_positive
    ; Negative: sign extend
    mov ah, 0xFF
    jmp .apply_x
.x_positive:
    xor ah, ah
.apply_x:
    ; AX now has signed 16-bit delta X
    add [mouse_x], ax

    ; Clamp X to 0-319
    cmp word [mouse_x], 0x8000      ; Negative (wrapped)?
    jb .x_not_neg
    mov word [mouse_x], 0
    jmp .do_y
.x_not_neg:
    cmp word [mouse_x], 319
    jbe .do_y
    mov word [mouse_x], 319

.do_y:
    ; Calculate delta Y (signed, inverted for screen coords)
    mov al, [mouse_packet + 2]      ; Y movement
    mov ah, [mouse_packet]
    test ah, 0x20                   ; Y sign bit
    jz .y_positive
    mov ah, 0xFF
    jmp .apply_y
.y_positive:
    xor ah, ah
.apply_y:
    neg ax                          ; Invert for screen Y (mouse up = screen up)
    add [mouse_y], ax

    ; Clamp Y to 0-199
    cmp word [mouse_y], 0x8000
    jb .y_not_neg
    mov word [mouse_y], 0
    jmp .post_event
.y_not_neg:
    cmp word [mouse_y], 199
    jbe .post_event
    mov word [mouse_y], 199

.post_event:
    ; Post mouse event: DL = buttons
    xor dx, dx
    mov dl, [mouse_buttons]
    mov al, EVENT_MOUSE             ; Type = 4
    call post_event

.reset_packet:
    mov byte [mouse_packet_idx], 0
    jmp .send_eoi

.resync:
    mov byte [mouse_packet_idx], 0
    jmp .send_eoi

.not_mouse:
    ; Not mouse data - chain to keyboard or ignore
    in al, KBC_DATA                 ; Clear the byte

.send_eoi:
    ; Send EOI to both PICs (slave then master)
    mov al, 0x20
    out 0xA0, al                    ; EOI to slave PIC
    out 0x20, al                    ; EOI to master PIC

    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    iret

; mouse_get_state - Get current mouse state
; Input: None
; Output: BX = X position (0-319)
;         CX = Y position (0-199)
;         DL = buttons (bit0=left, bit1=right, bit2=middle)
;         DH = enabled flag (0=no mouse, 1=mouse active)
mouse_get_state:
    mov bx, [mouse_x]
    mov cx, [mouse_y]
    mov dl, [mouse_buttons]
    mov dh, [mouse_enabled]
    ret

; mouse_set_position - Set mouse cursor position
; Input: BX = X position, CX = Y position
; Output: None
mouse_set_position:
    cmp bx, 319
    jbe .x_ok
    mov bx, 319
.x_ok:
    mov [mouse_x], bx
    cmp cx, 199
    jbe .y_ok
    mov cx, 199
.y_ok:
    mov [mouse_y], cx
    ret

; mouse_is_enabled - Check if mouse is available
; Input: None
; Output: AL = enabled flag (0=no, 1=yes)
mouse_is_enabled:
    mov al, [mouse_enabled]
    ret

; ============================================================================
; Version String (auto-generated from VERSION and BUILD_NUMBER files)
; ============================================================================

; Include auto-generated version/build info
%include "build_info.inc"

; Aliases for compatibility
version_string equ VERSION_STR
build_string   equ BUILD_NUMBER_STR

; Boot messages
kernel_prefix:  db 'Kernel: ', 0

; Boot configuration
boot_drive:     db 0                    ; Boot drive number (0x00=floppy, 0x80=HDD)

; ============================================================================
; BIOS Print String (for early boot before our handlers are installed)
; Input: DS:SI = null-terminated string
; ============================================================================

print_string_bios:
    push ax
    push bx
    push si
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    jmp .loop
.done:
    pop si
    pop bx
    pop ax
    ret

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

    ; Show cluster 1: "C1:" + char at offset 11 (should be 'A')
    mov bx, 10
    mov cx, 130
    mov si, .c1_label
    call gfx_draw_string_stub
    mov al, [fs_read_buffer + 11]       ; Char after "CLUSTER 1: "
    mov bx, 42
    mov cx, 130
    call gfx_draw_char_stub

    ; Show cluster 2: "C2:" + char at offset 512+11 (should be 'B')
    mov bx, 60
    mov cx, 130
    mov si, .c2_label
    call gfx_draw_string_stub
    mov al, [fs_read_buffer + 512 + 11] ; Char after "CLUSTER 2: "
    mov bx, 92
    mov cx, 130
    call gfx_draw_char_stub

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

.insert_msg:    db 'Insert test disk, press key', 0
.testing_msg:   db 'Testing...', 0
.mount_ok:      db 'Mount: OK', 0
.mount_err:     db 'Mount: FAIL', 0
.open_ok:       db 'Open TEST.TXT: OK', 0
.open_err:      db 'Open TEST.TXT: FAIL', 0
.read_ok:       db 'Read: OK - File contents:', 0
.read_err:      db 'Read: FAIL', 0
.filename:      db 'TEST.TXT', 0
.c1_label:      db 'C1:', 0
.c2_label:      db 'C2:', 0

; ============================================================================
; Application Loader Test - Tests Core Services 2.1
; ============================================================================

test_app_loader:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    ; Ensure DS is set to kernel segment
    push cs
    pop ds

    ; Display prompt
    mov bx, 4
    mov cx, 30
    mov si, .prompt
    call gfx_draw_string_stub

    ; Clear any pending keys (e.g., from pressing 'L')
    call clear_kbd_buffer

    ; Wait for key (swap disks)
    call kbd_wait_key

    ; Display "Loading..."
    mov bx, 4
    mov cx, 40
    mov si, .loading
    call gfx_draw_string_stub

    ; Save DS before changing it for app_load_stub
    push ds

    ; Load application from drive A: (0x00)
    mov ax, 0x1000
    mov ds, ax
    mov si, .app_filename
    mov dl, 0x00                    ; Drive A:
    call app_load_stub

    ; Restore DS immediately
    pop ds

    jc .load_failed

    ; Save app handle
    mov [.app_handle], ax

    ; Display "Load: OK"
    mov bx, 4
    mov cx, 50
    mov si, .load_ok
    call gfx_draw_string_stub

    ; Run the application
    mov ax, [.app_handle]
    call app_run_stub
    jc .run_failed

    ; Display "Run: OK"
    mov bx, 4
    mov cx, 60
    mov si, .run_ok
    call gfx_draw_string_stub

    jmp .done

.load_failed:
    ; DS already restored after app_load_stub
    ; Save error code - gfx_draw_string_stub destroys AL
    push ax

    ; Display "Load: FAIL "
    mov bx, 4
    mov cx, 50
    mov si, .load_err
    call gfx_draw_string_stub

    ; Display error code after string
    pop ax                          ; Restore error code
    mov bx, 136                     ; Position after "Load: FAIL " (11 chars × 12px + 4)
    add al, '0'                     ; Convert to ASCII digit
    call gfx_draw_char_stub

    jmp .done

.run_failed:
    ; Display "Run: FAIL"
    mov bx, 4
    mov cx, 60
    mov si, .run_err
    call gfx_draw_string_stub
    jmp .done

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Local data
.app_handle:    dw 0
.prompt:        db 'Insert app disk, press key', 0
.loading:       db 'Loading...', 0
.load_ok:       db 'Load: OK', 0
.load_err:      db 'Load: FAIL ', 0
.run_ok:        db 'Run: OK', 0
.run_err:       db 'Run: FAIL', 0
.app_filename:  db 'LAUNCHER.BIN', 0   ; Parsed by fat12_open into 8.3 format

; ============================================================================
; Auto-load Launcher - Automatically loads and runs launcher on boot
; ============================================================================

auto_load_launcher:
    push ax
    push dx
    push si

    ; Load LAUNCHER.BIN from boot drive
    mov ax, 0x1000
    mov ds, ax
    mov si, .launcher_filename
    mov dl, [boot_drive]            ; Use saved boot drive number
    mov dh, 0x20                    ; Load to shell segment (0x2000)
    call app_load_stub
    jc .failed                      ; On error, fall back to demo

    ; Run the launcher
    call app_run_stub

.failed:
    ; On any error or launcher exit, fall through to keyboard demo
    pop si
    pop dx
    pop ax
    call keyboard_demo
    ret

; Local data
.launcher_filename: db 'LAUNCHER.BIN', 0

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

    ; Check for 'L' or 'l' (app loader test)
    cmp al, 'L'
    je .app_test
    cmp al, 'l'
    je .app_test

    ; Check for 'W' or 'w' (window test)
    cmp al, 'W'
    je .window_test
    cmp al, 'w'
    je .window_test

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

.app_test:
    ; Restore registers before calling test_app_loader
    pop si
    pop cx
    pop bx
    pop ax

    ; Call app loader test
    call test_app_loader

    ; Return (test_app_loader will continue)
    ret

.window_test:
    ; Create a test window
    pop si
    pop cx
    pop bx
    pop ax

    ; Create window at (50, 30), size 200x100
    mov bx, 50                      ; X
    mov cx, 30                      ; Y
    mov dx, 200                     ; Width
    mov si, 100                     ; Height
    mov di, .win_title              ; Title
    mov al, WIN_FLAG_TITLE | WIN_FLAG_BORDER
    call win_create_stub

    jc .win_fail

    ; Display success message
    push ax                         ; Save window handle
    mov bx, 4
    mov cx, 180
    mov si, .win_ok
    call gfx_draw_string_stub
    pop ax
    ret

.win_fail:
    mov bx, 4
    mov cx, 180
    mov si, .win_err
    call gfx_draw_string_stub
    ret

.win_title: db 'Test Window', 0
.win_ok:    db 'Window: OK', 0
.win_err:   db 'Window: FAIL', 0

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

.prompt: db 'ESC=exit F=file L=app W=win:', 0
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
; Plot a black pixel (color 0) - for inverted text on white backgrounds
; Input: CX = X coordinate (0-319), BX = Y coordinate (0-199)
; Preserves all registers except flags
; ============================================================================

plot_pixel_black:
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
    mov bl, 0x03
    shl bl, cl
    not bl
    and al, bl                      ; Clear bits = color 0 (black)
    mov [es:di], al
    pop dx
    pop di
    pop cx
    pop bx
    pop ax
    ret

; ============================================================================
; Draw character in inverted colors (black on white)
; Uses draw_x, draw_y for position, SI = font data pointer
; ============================================================================

draw_char_inverted:
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
    call plot_pixel_black           ; Plot BLACK at (CX, BX)
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
; Draw string in inverted colors (black text)
; Input: BX = X, CX = Y, SI = string pointer (DS:SI)
; ============================================================================

gfx_draw_string_inverted:
    push es
    push ax
    push dx
    push di
    push bp
    push ds                         ; Save caller's DS
    mov word [draw_x], bx
    mov word [draw_y], cx
    mov dx, 0xB800
    mov es, dx
.loop:
    lodsb                           ; Load character from DS:SI
    test al, al
    jz .done
    push ds                         ; Save caller's DS for string
    mov bp, 0x1000
    mov ds, bp                      ; DS = kernel segment for font access
    sub al, 32
    mov ah, 0
    mov dl, 8
    mul dl
    mov di, si                      ; Save string pointer
    mov si, font_8x8                ; SI = font offset (now in kernel DS!)
    add si, ax
    call draw_char_inverted         ; Draw with DS=0x1000 for font
    mov si, di                      ; Restore string pointer
    pop ds                          ; Restore caller's DS for string
    jmp .loop
.done:
    pop ds                          ; Restore original DS
    pop bp
    pop di
    pop dx
    pop ax
    pop es
    ret

; ============================================================================
; Kernel API Table - At fixed address 0x1000:0x0900
; ============================================================================

; Pad to exactly offset 0x0B00 (2816 bytes) - expanded for mouse driver
times 0x0B00 - ($ - $$) db 0

kernel_api_table:
    ; Header
    dw 0x4B41                       ; Magic: 'KA' (Kernel API)
    dw 0x0001                       ; Version: 1.0
    dw 30                           ; Number of function slots (0-29)
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

    ; Application Loader (Core Services 2.1)
    dw app_load_stub                ; 17: Load application from disk
    dw app_run_stub                 ; 18: Run loaded application

    ; Window Manager (Core Services 2.2)
    dw win_create_stub              ; 19: Create window
    dw win_destroy_stub             ; 20: Destroy window
    dw win_draw_stub                ; 21: Draw/redraw window frame
    dw win_focus_stub               ; 22: Bring window to front
    dw win_move_stub                ; 23: Move window
    dw win_get_content_stub         ; 24: Get content area bounds
    dw register_shell_stub          ; 25: Register app as shell (auto-return)
    dw fs_readdir_stub              ; 26: Read directory entry

    ; Mouse API
    dw mouse_get_state              ; 27: Get mouse position/buttons
    dw mouse_set_position           ; 28: Set mouse position
    dw mouse_is_enabled             ; 29: Check if mouse available

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
; Uses caller_ds for string access (supports apps calling through INT 0x80)
gfx_draw_string_stub:
    push es
    push ax
    push dx
    push di
    push bp
    push ds
    mov word [draw_x], bx
    mov word [draw_y], cx
    mov bp, [caller_ds]             ; BP = caller's segment (0x1000 at boot, app seg via INT 0x80)
    mov dx, 0xB800
    mov es, dx
    mov ds, bp                      ; DS = caller's segment for string access
.loop:
    lodsb                           ; AL = [DS:SI++] from caller's segment
    test al, al
    jz .done
    push ds                         ; Save caller's DS
    mov bp, 0x1000
    mov ds, bp                      ; DS = kernel for font_8x8 access
    sub al, 32
    mov ah, 0
    mov dl, 8
    mul dl
    mov di, si                      ; Save string pointer
    mov si, font_8x8
    add si, ax
    call draw_char
    mov si, di                      ; Restore string pointer
    pop ds                          ; Restore caller's DS
    jmp .loop
.done:
    pop ds
    pop bp
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
    ; API: BX=X, CX=Y. plot_pixel_white needs CX=X, BX=Y
    ; Save: BP = X (from BX), AX = Y (from CX)
    mov bp, bx                      ; BP = X
    mov ax, cx                      ; AX = Y

    ; Top edge: Y=AX, X varies from BP to BP+DX-1
    mov di, 0
.top:
    mov cx, bp                      ; CX = X (for plot_pixel_white)
    add cx, di
    mov bx, ax                      ; BX = Y (for plot_pixel_white)
    call plot_pixel_white
    inc di
    cmp di, dx
    jl .top

    ; Bottom edge: Y=AX+SI-1, X varies from BP to BP+DX-1
    mov di, 0
    push ax
    add ax, si
    dec ax                          ; AX = Y + Height - 1
.bottom:
    mov cx, bp
    add cx, di
    mov bx, ax
    call plot_pixel_white
    inc di
    cmp di, dx
    jl .bottom
    pop ax

    ; Left edge: X=BP, Y varies from AX to AX+SI-1
    mov di, 0
.left:
    mov cx, bp                      ; CX = X (left edge)
    mov bx, ax                      ; BX = Y
    add bx, di
    call plot_pixel_white
    inc di
    cmp di, si
    jl .left

    ; Right edge: X=BP+DX-1, Y varies from AX to AX+SI-1
    mov di, 0
    push bp
    add bp, dx
    dec bp                          ; BP = X + Width - 1
.right:
    mov cx, bp                      ; CX = X (right edge)
    mov bx, ax                      ; BX = Y
    add bx, di
    call plot_pixel_white
    inc di
    cmp di, si
    jl .right
    pop bp

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
    ; Swap BX/CX: API has BX=X,CX=Y but plot_pixel_white needs CX=X,BX=Y
    xchg bx, cx
.row:
    mov di, dx
    push bx                         ; Save Y
    push cx                         ; Save X
.col:
    call plot_pixel_white           ; CX=X, BX=Y
    inc cx                          ; Next X
    dec di
    jnz .col
    pop cx                          ; Restore X
    pop bx                          ; Restore Y
    inc bx                          ; Next Y
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
    push es
    push ax
    push bp
    push di
    push bx
    push cx
    push dx
    push si

    mov ax, 0xB800
    mov es, ax
    mov bp, si                      ; BP = height counter

.clear_row:
    mov di, dx                      ; DI = width counter
    push cx                         ; Save Y
    push bx                         ; Save X

.clear_col:
    ; Plot pixel with color 0 (background)
    call .plot_bg
    inc bx                          ; Next X
    dec di
    jnz .clear_col

    pop bx                          ; Restore X
    pop cx                          ; Restore Y
    inc cx                          ; Next Y
    dec bp
    jnz .clear_row

    pop si
    pop dx
    pop cx
    pop bx
    pop di
    pop bp
    pop ax
    pop es
    ret

; Internal helper: Clear pixel at BX=X, CX=Y to background color
.plot_bg:
    push ax
    push bx
    push cx
    push di
    push dx

    ; Calculate video memory offset (same as plot_pixel_white)
    mov ax, cx                      ; AX = Y
    shr ax, 1                       ; AX = Y / 2
    mov dx, 80
    mul dx                          ; AX = (Y / 2) * 80
    mov di, ax

    mov ax, bx                      ; AX = X
    shr ax, 1
    shr ax, 1                       ; AX = X / 4
    add di, ax

    test cl, 1                      ; Odd scanline?
    jz .even
    add di, 0x2000
.even:
    ; Calculate bit position
    mov ax, bx
    and ax, 3                       ; X % 4
    mov cx, 3
    sub cl, al
    shl cl, 1                       ; Shift amount

    ; Clear pixel (set to color 0)
    mov al, 0x03
    shl al, cl
    not al
    and [es:di], al                 ; Clear the 2 bits

    pop dx
    pop di
    pop cx
    pop bx
    pop ax
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
FS_ERR_END_OF_DIR       equ 6

; Filesystem type constants
FS_TYPE_FAT12           equ 1
FS_TYPE_FAT16           equ 2

; FAT16 constants
FAT16_EOC               equ 0xFFF8      ; End of cluster chain (0xFFF8-0xFFFF)
FAT16_BAD               equ 0xFFF7      ; Bad cluster marker

; Primary IDE controller ports
IDE_DATA                equ 0x1F0       ; Data register (16-bit)
IDE_ERROR               equ 0x1F1       ; Error register (read)
IDE_FEATURES            equ 0x1F1       ; Features register (write)
IDE_SECT_COUNT          equ 0x1F2       ; Sector count
IDE_SECT_NUM            equ 0x1F3       ; Sector number (LBA 0-7)
IDE_CYL_LOW             equ 0x1F4       ; Cylinder low (LBA 8-15)
IDE_CYL_HIGH            equ 0x1F5       ; Cylinder high (LBA 16-23)
IDE_HEAD                equ 0x1F6       ; Drive/head (LBA 24-27 + flags)
IDE_STATUS              equ 0x1F7       ; Status register (read)
IDE_CMD                 equ 0x1F7       ; Command register (write)

; IDE status bits
IDE_STAT_BSY            equ 0x80        ; Busy
IDE_STAT_DRDY           equ 0x40        ; Drive ready
IDE_STAT_DRQ            equ 0x08        ; Data request
IDE_STAT_ERR            equ 0x01        ; Error

; IDE commands
IDE_CMD_READ            equ 0x20        ; Read sectors (with retry)
IDE_CMD_IDENTIFY        equ 0xEC        ; Identify drive

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
    push dx

    ; Check if this is a hard drive (0x80+)
    test al, 0x80
    jnz .try_fat16

    ; Floppy drive - use FAT12
    cmp al, 0
    jne .unsupported

    ; Try to mount FAT12
    call fat12_mount
    jc .error

    ; Success - return mount handle 0 (FAT12)
    xor bx, bx
    clc
    pop dx
    pop di
    pop si
    ret

.try_fat16:
    ; Hard drive - use FAT16
    mov dl, al                      ; Drive number in DL
    call fat16_mount
    jc .error

    ; Success - return mount handle 1 (FAT16)
    mov bx, 1
    clc
    pop dx
    pop di
    pop si
    ret

.unsupported:
.error:
    mov ax, FS_ERR_NO_DRIVER
    stc
    pop dx
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

    ; Route based on mount handle
    cmp bx, 0
    je .fat12
    cmp bx, 1
    je .fat16
    jmp .invalid_mount

.fat12:
    ; Call FAT12 open
    call fat12_open
    jc .error
    jmp .success

.fat16:
    ; Call FAT16 open
    call fat16_open
    jc .error

.success:
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

    ; Get file table entry to check mount handle
    mov si, ax
    shl si, 5                       ; * 32 bytes per entry
    add si, file_table

    ; Check mount handle (offset 1)
    cmp byte [si + 1], 0            ; FAT12?
    je .fat12
    cmp byte [si + 1], 1            ; FAT16?
    je .fat16
    jmp .invalid_handle

.fat12:
    ; Call FAT12 read
    call fat12_read
    jc .error
    jmp .success

.fat16:
    ; Call FAT16 read (ES:DI -> ES:BX)
    mov bx, di
    call fat16_read
    jc .error

.success:
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

; fs_readdir_stub - Read next directory entry
; Input: AL = mount handle (0 for FAT12, 1 for FAT16)
;        CX = iteration state (0 = start fresh)
;        ES:DI = pointer to 32-byte buffer for entry
; Output: CF = 0 success (entry copied to buffer)
;         CF = 1 end of directory or error
;         CX = new state for next call
;         AX = FS_ERR_END_OF_DIR (6) when done
; State encoding: bits 0-3 = entry index (0-15), bits 4-15 = sector offset (0-13)
fs_readdir_stub:
    ; Route based on mount handle
    cmp al, 0
    je .fat12
    cmp al, 1
    je .fat16
    ; Invalid mount handle
    mov ax, FS_ERR_NO_DRIVER
    stc
    ret

.fat16:
    ; Call FAT16 readdir
    call fat16_readdir
    ret

.fat12:
    push bx
    push dx
    push si
    push bp
    push di                         ; Save caller's DI

    ; Parse state from CX
    mov bp, cx                      ; BP = iteration state
    mov ax, bp
    and ax, 0x000F                  ; AX = entry index (0-15)
    mov bx, bp
    shr bx, 4                       ; BX = sector offset (0-13)

    ; Save parsed values
    mov [.entry_idx], ax
    mov [.sector_off], bx

.read_next_sector:
    ; Check if we've passed all root directory sectors
    cmp word [.sector_off], 14
    jae .end_of_dir

    ; Calculate absolute sector = root_dir_start + sector_offset
    mov ax, [root_dir_start]
    add ax, [.sector_off]

    ; LBA to CHS conversion (same pattern as fat12_open)
    push ax                         ; Save LBA
    xor dx, dx
    mov bx, 18                      ; sectors per track (1.44MB floppy)
    div bx                          ; AX = LBA / 18, DX = LBA % 18
    inc dx                          ; DX = sector (1-based)
    mov cl, dl                      ; CL = sector number

    xor dx, dx
    mov bx, 2                       ; num heads
    div bx                          ; AX = cylinder, DX = head
    mov ch, al                      ; CH = cylinder
    mov dh, dl                      ; DH = head

    ; Read sector to bpb_buffer
    push es
    push di
    mov ax, 0x1000
    mov es, ax
    mov bx, bpb_buffer
    mov ax, 0x0201                  ; AH=02 (read), AL=01 (1 sector)
    mov dl, 0                       ; DL = drive A:
    int 0x13
    pop di
    pop es
    pop ax                          ; Restore LBA (not needed but clean stack)
    jc .read_error

.scan_entries:
    ; Calculate entry pointer: bpb_buffer + (entry_idx * 32)
    mov ax, [.entry_idx]
    shl ax, 5                       ; * 32
    mov si, ax
    add si, bpb_buffer

.check_entry:
    ; Check bounds - if entry >= 16, go to next sector
    cmp word [.entry_idx], 16
    jae .next_sector

    ; Check first byte of entry
    push ds
    mov ax, 0x1000
    mov ds, ax                      ; DS = kernel segment for bpb_buffer access
    mov al, [si]
    pop ds

    ; End of directory marker
    test al, al
    jz .end_of_dir

    ; Deleted entry - skip
    cmp al, 0xE5
    je .skip_entry

    ; Check attributes at offset 0x0B
    push ds
    mov ax, 0x1000
    mov ds, ax
    mov al, [si + 0x0B]
    pop ds

    ; Skip LFN entries (attribute = 0x0F)
    cmp al, 0x0F
    je .skip_entry

    ; Skip volume labels (attribute & 0x08)
    test al, 0x08
    jnz .skip_entry

    ; Valid entry found - copy 32 bytes to caller's buffer (ES:DI)
    push cx
    push si
    push di
    push ds
    mov ax, 0x1000
    mov ds, ax                      ; DS:SI = source (bpb_buffer entry)
    mov cx, 32
    cld
    rep movsb                       ; Copy to ES:DI
    pop ds
    pop di
    pop si
    pop cx

    ; Increment state for next call
    inc word [.entry_idx]
    cmp word [.entry_idx], 16
    jb .encode_state
    ; Wrapped to next sector
    mov word [.entry_idx], 0
    inc word [.sector_off]

.encode_state:
    ; CX = (sector_off << 4) | entry_idx
    mov cx, [.sector_off]
    shl cx, 4
    or cx, [.entry_idx]

    ; Success - clear carry
    clc
    jmp .done

.skip_entry:
    inc word [.entry_idx]
    mov ax, [.entry_idx]
    shl ax, 5
    mov si, ax
    add si, bpb_buffer
    jmp .check_entry

.next_sector:
    mov word [.entry_idx], 0
    inc word [.sector_off]
    jmp .read_next_sector

.read_error:
    mov ax, FS_ERR_READ_ERROR
    stc
    jmp .done

.end_of_dir:
    mov ax, FS_ERR_END_OF_DIR
    stc

.done:
    pop di
    pop bp
    pop si
    pop dx
    pop bx
    ret

; Local variables for fs_readdir_stub
.entry_idx:     dw 0
.sector_off:    dw 0

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
    ; Filename has been copied to stack - now set DS to kernel segment
    ; so we can access kernel variables (root_dir_start, etc.)
    mov ax, 0x1000
    mov ds, ax

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

    ; Skip special entries: check attribute byte at offset 0x0B
    ; DS should be 0x1000, read attribute directly
    mov al, [si + 0x0B]             ; Read attribute byte
    ; Now check attributes
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
    ; DS is still 0x1000 from the search loop (or we set it)
    mov ax, 0x1000
    mov ds, ax
    mov ax, [si + 0x1A]             ; Starting cluster
    mov bx, [si + 0x1C]             ; File size (low word)
    mov dx, [si + 0x1E]             ; File size (high word)

    ; Restore DS
    push cs
    pop ds

    ; Clean up stack - DS from line 1608 is still on stack!
    add sp, 2                       ; DS from search loop (line 1608)
    add sp, 2                       ; sector number (AX from line 1574)
    add sp, 2                       ; sector counter (CX from line 1573)
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

    ; Read sector into bpb_buffer (ES:BX = 0x1000:bpb_buffer)
    push es
    push di
    push si

    mov bx, 0x1000
    mov es, bx                      ; ES = 0x1000 (for INT 13h read)
    push bx
    pop ds                          ; DS = 0x1000 (for later access)
    mov bx, bpb_buffer              ; BX = buffer offset

    ; Convert LBA to CHS (proper conversion for floppy)
    ; AX = LBA sector number
    push bx                         ; Save buffer pointer
    push ax                         ; Save LBA
    xor dx, dx
    mov bx, 18                      ; Sectors per track (1.44MB floppy)
    div bx                          ; AX = LBA / 18, DX = LBA % 18
    inc dx                          ; DX = sector (1-based)
    mov cl, dl                      ; CL = sector
    xor dx, dx
    mov bx, 2                       ; Number of heads
    div bx                          ; AX = cylinder, DX = head
    mov ch, al                      ; CH = cylinder
    mov dh, dl                      ; DH = head
    pop ax                          ; Restore LBA (saved for FAT chain)
    pop bx                          ; Restore buffer pointer
    push ax                         ; Save LBA again
    mov ax, 0x0201                  ; Read 1 sector
    mov dl, 0x00                    ; Drive A:
    int 0x13
    pop ax                          ; Restore LBA

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
; FAT16/Hard Drive Driver Implementation (v3.13.0)
; ============================================================================

; fat16_read_sector - Read a sector from hard drive
; Input: EAX = LBA sector number (relative to partition start)
;        ES:BX = buffer to read into
; Output: CF = 0 on success, CF = 1 on error
; Note: Adds partition offset automatically
fat16_read_sector:
    push eax
    push bx
    push cx
    push dx
    push si

    ; Add partition start offset to get absolute LBA
    add eax, [fat16_partition_lba]

    ; Try INT 13h extended read first (LBA mode)
    ; Build disk address packet on stack
    push dword 0                    ; High 32 bits of LBA (0 for <2TB)
    push eax                        ; Low 32 bits of LBA
    push es                         ; Buffer segment
    push bx                         ; Buffer offset
    push word 1                     ; Number of sectors to read
    push word 0x0010                ; Packet size (16 bytes)
    mov si, sp                      ; DS:SI = packet address

    mov ah, 0x42                    ; Extended read
    mov dl, [fat16_drive]           ; Drive number
    push ds
    push ss
    pop ds                          ; DS:SI points to stack packet
    int 0x13
    pop ds
    add sp, 16                      ; Clean up stack packet
    jnc .success                    ; If no error, we're done

    ; Extended read failed - try CHS fallback
    ; First get drive geometry
    mov ah, 0x08                    ; Get drive parameters
    mov dl, [fat16_drive]
    int 0x13
    jc .error

    ; DH = max head number, CL[5:0] = max sector, CL[7:6]:CH = max cylinder
    mov al, cl
    and al, 0x3F                    ; AL = sectors per track
    mov [ide_sectors], al
    inc dh
    mov [ide_heads], dh             ; heads = max_head + 1

    ; Convert LBA to CHS
    ; Restore EAX (LBA)
    mov eax, [esp + 16]             ; Get original EAX from stack
    add eax, [fat16_partition_lba]  ; Add partition offset

    ; Sector = (LBA mod sectors_per_track) + 1
    xor edx, edx
    movzx ecx, byte [ide_sectors]
    div ecx                         ; EAX = LBA / SPT, EDX = LBA mod SPT
    inc dl                          ; Sector is 1-based
    mov cl, dl                      ; CL = sector

    ; Head = (temp / sectors_per_track) mod heads
    ; Cylinder = (temp / sectors_per_track) / heads
    xor edx, edx
    movzx ecx, byte [ide_heads]
    div ecx                         ; EAX = cylinder, EDX = head
    mov dh, dl                      ; DH = head
    mov ch, al                      ; CH = cylinder low 8 bits
    shl ah, 6
    or cl, ah                       ; CL[7:6] = cylinder high 2 bits

    ; Perform CHS read
    mov ah, 0x02                    ; Read sectors
    mov al, 1                       ; 1 sector
    mov dl, [fat16_drive]           ; Drive number
    int 0x13
    jc .error

.success:
    clc
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    ret

.error:
    stc
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    ret

; fat16_mount - Mount FAT16 partition from hard drive
; Input: DL = drive number (0x80 = first HD)
; Output: CF = 0 on success, CF = 1 on error
;         AX = error code if CF = 1
fat16_mount:
    push es
    push bx
    push cx
    push dx
    push si
    push di

    ; Save drive number
    mov [fat16_drive], dl

    ; Reset drive
    xor ax, ax
    int 0x13
    jc .read_error

    ; Read MBR (sector 0)
    mov ax, 0x1000
    mov es, ax
    mov bx, fat16_sector_buf
    xor eax, eax                    ; LBA 0
    mov [fat16_partition_lba], eax  ; Temporarily 0 for absolute read

    ; Use BIOS INT 13h to read MBR directly
    mov ah, 0x02                    ; Read sectors
    mov al, 1                       ; 1 sector
    mov cx, 0x0001                  ; Cylinder 0, sector 1
    xor dh, dh                      ; Head 0
    mov dl, [fat16_drive]
    int 0x13
    jc .read_error

    ; Verify MBR signature
    cmp word [fat16_sector_buf + 510], 0xAA55
    jne .read_error

    ; Find first bootable FAT16 partition in partition table
    ; Partition table starts at offset 0x1BE
    mov si, fat16_sector_buf + 0x1BE
    mov cx, 4                       ; 4 partition entries

.find_partition:
    ; Check partition type (offset 4)
    mov al, [si + 4]
    ; FAT16 types: 0x04 (FAT16 <32MB), 0x06 (FAT16 >32MB), 0x0E (FAT16 LBA)
    cmp al, 0x04
    je .found_partition
    cmp al, 0x06
    je .found_partition
    cmp al, 0x0E
    je .found_partition

    add si, 16                      ; Next partition entry
    loop .find_partition

    ; No FAT16 partition found
    jmp .read_error

.found_partition:
    ; Get partition start LBA (offset 8 in partition entry)
    mov eax, [si + 8]
    mov [fat16_partition_lba], eax

    ; Read VBR (Volume Boot Record) - first sector of partition
    mov bx, fat16_sector_buf
    xor eax, eax                    ; Sector 0 relative to partition
    call fat16_read_sector
    jc .read_error

    ; Verify VBR signature
    cmp word [fat16_sector_buf + 510], 0xAA55
    jne .read_error

    ; Parse BPB (BIOS Parameter Block)
    ; Bytes per sector at offset 0x0B (must be 512)
    cmp word [fat16_sector_buf + 0x0B], 512
    jne .read_error

    ; Sectors per cluster at offset 0x0D
    mov al, [fat16_sector_buf + 0x0D]
    test al, al
    jz .read_error
    mov [fat16_sects_per_clust], al

    ; Reserved sectors at offset 0x0E
    mov ax, [fat16_sector_buf + 0x0E]
    mov [fat16_bpb_cache], ax       ; Store at offset 0 of cache

    ; Number of FATs at offset 0x10
    mov al, [fat16_sector_buf + 0x10]
    mov [fat16_bpb_cache + 2], al

    ; Root directory entries at offset 0x11
    mov ax, [fat16_sector_buf + 0x11]
    mov [fat16_root_entries], ax

    ; Sectors per FAT at offset 0x16
    mov ax, [fat16_sector_buf + 0x16]
    mov [fat16_bpb_cache + 4], ax   ; Store sectors per FAT

    ; Calculate FAT start sector
    ; fat_start = reserved_sectors
    movzx eax, word [fat16_bpb_cache]
    mov [fat16_fat_start], eax

    ; Calculate root directory start sector
    ; root_start = reserved + (num_fats * sectors_per_fat)
    movzx eax, byte [fat16_bpb_cache + 2]  ; num_fats
    movzx ebx, word [fat16_bpb_cache + 4]  ; sectors_per_fat
    imul eax, ebx
    movzx ebx, word [fat16_bpb_cache]      ; reserved_sectors
    add eax, ebx
    mov [fat16_root_start], eax

    ; Calculate data area start sector
    ; data_start = root_start + (root_entries * 32 / 512)
    movzx ebx, word [fat16_root_entries]
    shr ebx, 4                      ; root_entries * 32 / 512 = root_entries / 16
    add eax, ebx
    mov [fat16_data_start], eax

    ; Mark as mounted
    mov byte [fat16_mounted], 1

    ; Invalidate FAT cache
    mov dword [fat16_fat_cached_sect], 0xFFFFFFFF

    ; Success
    xor ax, ax
    clc
    jmp .done

.read_error:
    mov ax, FS_ERR_READ_ERROR
    stc

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

; fat16_open - Open a file on FAT16 volume
; Input: DS:SI = filename (null-terminated, "FILENAME.EXT")
; Output: CF = 0 on success, AX = file handle
;         CF = 1 on error, AX = error code
fat16_open:
    push es
    push bx
    push cx
    push dx
    push si
    push di

    ; Check if FAT16 is mounted
    cmp byte [fat16_mounted], 1
    jne .not_mounted

    ; Convert filename to FAT 8.3 format (space-padded)
    ; Use 11 bytes on stack for converted name
    sub sp, 12                      ; 11 bytes + 1 for alignment
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
    ; Convert to uppercase
    cmp al, 'a'
    jb .store_char
    cmp al, 'z'
    ja .store_char
    sub al, 32                      ; Convert to uppercase
.store_char:
    stosb
    loop .copy_name

    ; Skip to dot if still more chars
.skip_to_dot:
    lodsb
    test al, al
    jz .name_done
    cmp al, '.'
    jne .skip_to_dot

.copy_ext:
    ; Copy extension (up to 3 chars)
    mov di, sp
    add di, 8                       ; Extension starts at offset 8
    mov cx, 3
.copy_ext_loop:
    lodsb
    test al, al
    jz .name_done
    ; Convert to uppercase
    cmp al, 'a'
    jb .store_ext
    cmp al, 'z'
    ja .store_ext
    sub al, 32
.store_ext:
    stosb
    loop .copy_ext_loop

.name_done:
    ; Now search root directory for file
    ; Read root directory sectors
    mov eax, [fat16_root_start]
    movzx ecx, word [fat16_root_entries]
    shr ecx, 4                      ; root_sectors = entries / 16 (16 entries per sector)
    test ecx, ecx
    jz .not_found

    mov di, sp                      ; DI = pointer to converted filename

.search_sector:
    push ecx                        ; Save sector count
    push eax                        ; Save current sector

    ; Read root directory sector
    mov bx, 0x1000
    mov es, bx
    mov bx, fat16_sector_buf
    call fat16_read_sector
    jc .search_error

    ; Search 16 entries in this sector
    mov si, fat16_sector_buf
    mov cx, 16

.search_entry:
    ; Check if entry is used (first byte != 0 and != 0xE5)
    mov al, [si]
    test al, al                     ; End of directory?
    jz .not_found
    cmp al, 0xE5                    ; Deleted entry?
    je .next_entry

    ; Compare 11-byte filename
    push cx
    push si
    push di
    mov cx, 11
    push ss
    pop es                          ; ES:DI = stack filename
    repe cmpsb
    pop di
    pop si
    pop cx
    je .found_file

.next_entry:
    add si, 32                      ; Next directory entry
    loop .search_entry

    ; Move to next sector
    pop eax
    pop ecx
    inc eax
    loop .search_sector

    jmp .not_found

.found_file:
    ; Found the file - allocate file handle
    ; First clean up search loop stack
    add sp, 8                       ; Pop saved eax and ecx

    ; Get file info from directory entry
    mov ax, [si + 26]               ; Starting cluster (offset 26)
    mov dx, [si + 28]               ; File size low word
    mov cx, [si + 30]               ; File size high word

    ; Find free file handle
    push ax
    push cx
    push dx
    mov si, file_table
    xor bx, bx                      ; Handle counter

.find_handle:
    cmp byte [si], 0                ; Free slot?
    je .got_handle
    add si, 32
    inc bx
    cmp bx, 16                      ; Max 16 handles
    jb .find_handle

    ; No free handles
    pop dx
    pop cx
    pop ax
    add sp, 12                      ; Clean up filename
    mov ax, FS_ERR_NO_HANDLES
    stc
    jmp .done

.got_handle:
    pop dx                          ; File size low
    pop cx                          ; File size high
    pop ax                          ; Starting cluster

    ; Fill in file handle
    mov byte [si], 1                ; Status = open
    mov byte [si + 1], 1            ; Mount handle = 1 (FAT16)
    mov [si + 2], ax                ; Starting cluster
    mov [si + 4], dx                ; File size low
    mov [si + 6], cx                ; File size high
    mov dword [si + 8], 0           ; Current position = 0
    mov [si + 12], ax               ; Current cluster = starting cluster

    ; Clean up filename and return handle
    add sp, 12
    mov ax, bx                      ; Return handle in AX
    clc
    jmp .done

.search_error:
    add sp, 8                       ; Pop saved eax and ecx
.not_found:
    add sp, 12                      ; Clean up filename
    mov ax, FS_ERR_NOT_FOUND
    stc
    jmp .done

.not_mounted:
    mov ax, FS_ERR_NO_DRIVER
    stc

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

; fat16_get_next_cluster - Get next cluster in FAT chain
; Input: AX = current cluster
; Output: AX = next cluster (0xFFF8+ = end of chain)
;         CF = 1 if error
fat16_get_next_cluster:
    push bx
    push cx
    push dx
    push es

    ; Calculate FAT sector containing this cluster
    ; Each sector holds 256 cluster entries (512 bytes / 2 bytes per entry)
    mov bx, ax                      ; Save cluster number
    shr ax, 8                       ; sector_index = cluster / 256

    ; Check if this sector is cached
    movzx eax, ax
    add eax, [fat16_fat_start]      ; Absolute FAT sector
    cmp eax, [fat16_fat_cached_sect]
    je .cached

    ; Need to load this FAT sector
    mov [fat16_fat_cached_sect], eax
    push bx
    mov cx, 0x1000
    mov es, cx
    mov cx, fat16_fat_cache
    mov bx, cx
    call fat16_read_sector
    pop bx
    jc .error

.cached:
    ; Get cluster entry from cache
    ; offset = (cluster mod 256) * 2
    mov ax, bx
    and ax, 0x00FF                  ; cluster mod 256
    shl ax, 1                       ; * 2 bytes per entry
    mov bx, ax
    mov ax, [fat16_fat_cache + bx]  ; Get next cluster value

    clc
    pop es
    pop dx
    pop cx
    pop bx
    ret

.error:
    mov dword [fat16_fat_cached_sect], 0xFFFFFFFF  ; Invalidate cache
    stc
    pop es
    pop dx
    pop cx
    pop bx
    ret

; fat16_read - Read from open FAT16 file
; Input: AX = file handle
;        ES:BX = buffer
;        CX = bytes to read
; Output: AX = bytes actually read
;         CF = 1 on error
fat16_read:
    push bx
    push cx
    push dx
    push si
    push di

    ; Validate handle
    cmp ax, 16
    jae .invalid_handle

    ; Get file table entry
    mov si, ax
    shl si, 5                       ; * 32 bytes per entry
    add si, file_table

    ; Check if handle is open and is FAT16
    cmp byte [si], 1
    jne .invalid_handle
    cmp byte [si + 1], 1            ; Mount handle 1 = FAT16
    jne .invalid_handle

    ; Save buffer pointer
    mov [.buffer_seg], es
    mov [.buffer_off], bx
    mov [.bytes_requested], cx

    ; Check if at end of file
    mov eax, [si + 8]               ; Current position
    mov edx, [si + 4]               ; File size
    cmp eax, edx
    jae .eof

    ; Calculate bytes remaining in file
    sub edx, eax                    ; bytes_remaining = size - position
    movzx ecx, word [.bytes_requested]
    cmp ecx, edx
    jbe .size_ok
    mov ecx, edx                    ; Limit to remaining bytes
.size_ok:
    mov [.bytes_to_read], cx

    ; Calculate offset within current cluster
    ; offset_in_cluster = position mod cluster_size
    movzx eax, byte [fat16_sects_per_clust]
    shl eax, 9                      ; cluster_size = sects_per_clust * 512
    mov [.cluster_size], ax

    mov eax, [si + 8]               ; Current position
    xor edx, edx
    movzx ecx, word [.cluster_size]
    div ecx                         ; EAX = cluster index, EDX = offset in cluster
    mov [.offset_in_cluster], dx

    ; Get current cluster from file handle
    mov ax, [si + 12]               ; Current cluster

    ; Read loop
    xor di, di                      ; Total bytes read

.read_loop:
    ; Check if done
    cmp di, [.bytes_to_read]
    jae .read_done

    ; Calculate sector within cluster
    mov ax, [.offset_in_cluster]
    shr ax, 9                       ; sector_in_cluster = offset / 512

    ; Calculate absolute sector
    ; sector = data_start + (cluster - 2) * sects_per_clust + sector_in_cluster
    mov bx, [si + 12]               ; Current cluster
    sub bx, 2
    movzx eax, byte [fat16_sects_per_clust]
    movzx ecx, bx
    imul eax, ecx
    movzx ecx, ax                   ; sector_in_cluster is in ax from before
    mov ax, [.offset_in_cluster]
    shr ax, 9
    add eax, ecx
    add eax, [fat16_data_start]

    ; Read sector
    push di
    push si
    push es
    mov bx, 0x1000
    mov es, bx
    mov bx, fat16_sector_buf
    call fat16_read_sector
    pop es
    pop si
    pop di
    jc .read_error

    ; Copy data from sector to user buffer
    mov cx, [.offset_in_cluster]
    and cx, 0x01FF                  ; offset in sector
    mov bx, 512
    sub bx, cx                      ; bytes available in this sector

    mov ax, [.bytes_to_read]
    sub ax, di                      ; bytes still needed
    cmp bx, ax
    jbe .copy_partial
    mov bx, ax                      ; Only copy what we need
.copy_partial:

    ; Copy BX bytes from fat16_sector_buf+CX to user buffer
    push si
    push di
    mov si, fat16_sector_buf
    add si, cx                      ; Source offset
    mov ax, [.buffer_seg]
    push es
    mov es, ax
    mov ax, [.buffer_off]
    add ax, di                      ; Destination includes bytes already read
    mov di, ax
    mov cx, bx
    rep movsb
    pop es
    pop di
    pop si

    ; Update counters
    add di, bx                      ; Total bytes read
    add [.offset_in_cluster], bx    ; Offset in cluster

    ; Check if we need next cluster
    mov ax, [.offset_in_cluster]
    cmp ax, [.cluster_size]
    jb .read_loop

    ; Move to next cluster
    mov ax, [si + 12]               ; Current cluster
    call fat16_get_next_cluster
    jc .read_error
    cmp ax, FAT16_EOC
    jae .read_done                  ; End of chain
    mov [si + 12], ax               ; Update current cluster
    mov word [.offset_in_cluster], 0
    jmp .read_loop

.read_done:
    ; Update file position
    movzx eax, di
    add [si + 8], eax
    mov ax, di                      ; Return bytes read
    clc
    jmp .done

.eof:
    xor ax, ax                      ; 0 bytes read
    clc
    jmp .done

.invalid_handle:
    mov ax, FS_ERR_INVALID_HANDLE
    stc
    jmp .done

.read_error:
    mov ax, FS_ERR_READ_ERROR
    stc

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; Local variables for fat16_read
.buffer_seg:        dw 0
.buffer_off:        dw 0
.bytes_requested:   dw 0
.bytes_to_read:     dw 0
.cluster_size:      dw 0
.offset_in_cluster: dw 0

; fat16_readdir - Read FAT16 root directory entry
; Input: CX = iteration state (0 = start, or previous state)
;        ES:DI = pointer to 32-byte buffer for entry
; Output: CF = 0 success (entry copied to buffer)
;         CF = 1 end of directory
;         CX = new state for next call
;         AX = FS_ERR_END_OF_DIR when done
; State encoding: CX = entry index (0 to fat16_root_entries-1)
fat16_readdir:
    push bx
    push dx
    push si
    push di

    ; Check if FAT16 is mounted
    cmp byte [fat16_mounted], 1
    jne .not_mounted

    ; CX contains entry index
    mov ax, cx                      ; AX = entry index

.scan_entries:
    ; Check if we've scanned all root entries
    cmp ax, [fat16_root_entries]
    jae .end_of_dir

    ; Calculate sector: entry_index / 16 (16 entries per sector)
    push ax
    mov bx, 16
    xor dx, dx
    div bx                          ; AX = sector offset, DX = entry within sector
    mov [.sector_off], ax
    mov [.entry_idx], dx
    pop ax                          ; Restore entry index
    push ax                         ; Save for later

    ; Calculate LBA: root_start + sector_offset
    mov eax, [fat16_root_start]
    movzx ebx, word [.sector_off]
    add eax, ebx

    ; Read root directory sector
    push es
    push di
    mov bx, fat16_sector_buf
    push ds
    pop es                          ; ES = kernel segment
    call fat16_read_sector
    pop di
    pop es
    pop ax                          ; Restore entry index
    jc .read_error

    ; Calculate entry offset in sector: entry_idx * 32
    mov bx, [.entry_idx]
    shl bx, 5                       ; * 32
    add bx, fat16_sector_buf        ; BX = pointer to entry

    ; Check first byte of entry
    push ds
    mov dx, ds
    mov ds, dx                      ; Ensure DS = kernel segment
    mov al, [bx]
    pop ds

    ; Check for end marker (0x00) or deleted entry (0xE5)
    cmp al, 0x00
    je .end_of_dir                  ; 0x00 = no more entries
    cmp al, 0xE5
    je .skip_entry                  ; 0xE5 = deleted

    ; Check attributes (offset 11)
    push ds
    mov dx, ds
    mov ds, dx
    mov al, [bx + 11]
    pop ds

    ; Check for long filename entry (attr == 0x0F)
    cmp al, 0x0F
    je .skip_entry

    ; Check for volume label (attr & 0x08, but not directory)
    test al, 0x08                   ; Volume label bit set?
    jz .valid_entry                 ; No, it's valid
    test al, 0x10                   ; Directory bit set?
    jnz .valid_entry                ; Yes, it's a dir, allow it
    jmp .skip_entry                 ; Volume label, skip

.valid_entry:

    ; Valid entry - copy to caller's buffer
    push ds
    mov dx, ds
    mov ds, dx                      ; DS = kernel segment
    mov si, bx                      ; SI = source
    mov cx, 32                      ; 32 bytes
.copy_loop:
    lodsb
    stosb
    loop .copy_loop
    pop ds

    ; Increment state and return success
    pop ax                          ; Current entry index
    inc ax                          ; Next entry
    mov cx, ax                      ; Return new state in CX
    push ax
    clc
    jmp .done

.skip_entry:
    ; Move to next entry
    pop ax                          ; Current entry index
    inc ax                          ; Next entry
    jmp .scan_entries

.not_mounted:
    mov ax, FS_ERR_NO_DRIVER
    stc
    jmp .done

.end_of_dir:
    pop ax                          ; Clean stack
    mov ax, FS_ERR_END_OF_DIR
    stc
    jmp .done

.read_error:
    pop ax                          ; Clean stack
    mov ax, FS_ERR_READ_ERROR
    stc

.done:
    pop di
    pop si
    pop dx
    pop bx
    ret

; Local variables
.sector_off:    dw 0
.entry_idx:     dw 0

; ============================================================================
; IDE Direct Access Driver (fallback for INT 13h failures)
; ============================================================================

; ide_wait_ready - Wait for IDE drive to become ready
; Output: CF = 0 if ready, CF = 1 if timeout
ide_wait_ready:
    push ax
    push cx
    push dx

    mov cx, 0xFFFF                  ; Timeout counter
    mov dx, IDE_STATUS
.wait:
    in al, dx
    test al, IDE_STAT_BSY           ; Busy?
    jz .not_busy
    loop .wait
    stc                             ; Timeout
    jmp .done

.not_busy:
    test al, IDE_STAT_DRDY          ; Ready?
    jnz .ready
    loop .wait
    stc                             ; Timeout
    jmp .done

.ready:
    clc

.done:
    pop dx
    pop cx
    pop ax
    ret

; ide_read_sector - Read sector directly via IDE ports
; Input: EAX = LBA sector number, ES:BX = buffer
; Output: CF = 0 on success, CF = 1 on error
ide_read_sector:
    push eax
    push bx
    push cx
    push dx

    ; Wait for drive ready
    call ide_wait_ready
    jc .error

    ; Set up LBA address in registers
    mov dx, IDE_SECT_COUNT
    mov al, 1                       ; Read 1 sector
    out dx, al

    mov dx, IDE_SECT_NUM
    mov eax, [esp + 12]             ; Get LBA from stack
    out dx, al                      ; LBA bits 0-7

    mov dx, IDE_CYL_LOW
    shr eax, 8
    out dx, al                      ; LBA bits 8-15

    mov dx, IDE_CYL_HIGH
    shr eax, 8
    out dx, al                      ; LBA bits 16-23

    mov dx, IDE_HEAD
    shr eax, 8
    and al, 0x0F                    ; LBA bits 24-27
    or al, 0xE0                     ; LBA mode, master drive
    out dx, al

    ; Issue read command
    mov dx, IDE_CMD
    mov al, IDE_CMD_READ
    out dx, al

    ; Wait for data ready
    call ide_wait_ready
    jc .error

    ; Check DRQ
    mov dx, IDE_STATUS
    in al, dx
    test al, IDE_STAT_DRQ
    jz .error

    ; Read 256 words (512 bytes)
    mov dx, IDE_DATA
    mov cx, 256
    rep insw

    clc
    jmp .done

.error:
    stc

.done:
    pop dx
    pop cx
    pop bx
    pop eax
    ret

; ide_detect - Detect IDE drive presence
; Output: CF = 0 if drive found, CF = 1 if not
;         AL = drive type info (if found)
ide_detect:
    push bx
    push cx
    push dx

    ; Select master drive
    mov dx, IDE_HEAD
    mov al, 0xA0                    ; Master, no LBA bits
    out dx, al

    ; Small delay
    mov cx, 10
.delay1:
    in al, dx
    loop .delay1

    ; Check if drive responds
    mov dx, IDE_STATUS
    in al, dx
    cmp al, 0xFF                    ; No drive returns 0xFF
    je .no_drive
    test al, al                     ; No drive may return 0
    jz .no_drive

    ; Try IDENTIFY command
    mov dx, IDE_CMD
    mov al, IDE_CMD_IDENTIFY
    out dx, al

    ; Wait for response
    call ide_wait_ready
    jc .no_drive

    ; Check DRQ
    mov dx, IDE_STATUS
    in al, dx
    test al, IDE_STAT_DRQ
    jz .no_drive

    ; Drive found - read and discard identify data
    mov dx, IDE_DATA
    mov cx, 256
.discard:
    in ax, dx
    loop .discard

    ; Mark drive as present
    or byte [ide_drive_present], 0x01

    clc
    mov al, 0x01                    ; Drive present
    jmp .done

.no_drive:
    stc

.done:
    pop dx
    pop cx
    pop bx
    ret

; ============================================================================
; Application Loader (Core Services 2.1)
; ============================================================================

; Error codes for application loader
APP_ERR_NO_SLOT         equ 1       ; No free app slot
APP_ERR_MOUNT_FAILED    equ 2       ; Failed to mount filesystem
APP_ERR_FILE_NOT_FOUND  equ 3       ; File not found
APP_ERR_ALLOC_FAILED    equ 4       ; Memory allocation failed
APP_ERR_READ_FAILED     equ 5       ; File read failed
APP_ERR_INVALID_HANDLE  equ 6       ; Invalid app handle
APP_ERR_NOT_LOADED      equ 7       ; App not loaded

; app_load_stub - Load application from disk
; Input: DS:SI = Pointer to filename (8.3 format, space-padded)
;        DL = BIOS drive number (0=A:, 1=B:, 0x80=C:, etc.)
;        DH = Target segment high byte (0x20=0x2000 shell, 0x30=0x3000 user)
;             If DH=0, defaults to 0x20 (APP_SEGMENT_SHELL) for compatibility
; Output: CF clear on success, AX = app handle (0-15)
;         CF set on error, AX = error code
; Preserves: None (registers may be modified)
app_load_stub:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es

    ; Save drive number, target segment, and filename pointer
    mov [.drive], dl
    ; Handle target segment - default to 0x20 if DH=0
    or dh, dh
    jnz .save_seg
    mov dh, 0x20                    ; Default to APP_SEGMENT_SHELL (0x2000)
.save_seg:
    mov [.target_seg], dh
    mov [.filename_off], si
    ; Use caller_ds since DS is now kernel segment after INT 0x80 dispatch
    mov ax, [caller_ds]
    mov [.filename_seg], ax

    ; Step 1: Find free slot in app_table
    mov ax, 0x1000
    mov es, ax
    mov di, app_table
    xor cx, cx                      ; CX = slot index

.find_slot:
    cmp cx, APP_MAX_COUNT
    jae .no_slot
    cmp byte [es:di], APP_STATE_FREE
    je .found_slot
    add di, APP_ENTRY_SIZE
    inc cx
    jmp .find_slot

.no_slot:
    mov ax, APP_ERR_NO_SLOT
    jmp .error

.found_slot:
    mov [.slot], cx
    mov [.slot_off], di

    ; Step 2: Mount filesystem
    mov al, [.drive]                ; AL = drive number (fs_mount_stub expects AL, not DL!)
    xor ah, ah                      ; AH = 0 (auto-detect driver)
    call fs_mount_stub
    jc .mount_failed

    ; Step 3: Open file
    ; IMPORTANT: Read filename_off BEFORE changing DS, since local vars are in kernel segment
    mov si, [.filename_off]
    mov ax, [.filename_seg]
    mov ds, ax
    call fs_open_stub
    jc .file_not_found

    mov [.file_handle], ax

    ; Step 4: Get file size from file handle
    ; File handle entry is at file_table + handle * 32
    ; Size is at offset 4 (low) and 6 (high)
    mov bx, ax
    shl bx, 5
    add bx, file_table
    mov ax, 0x1000
    mov ds, ax
    mov cx, [bx + 4]                ; CX = file size (low word)
    mov [.file_size], cx

    ; Step 5: Load app at target segment offset 0
    ; Apps use ORG 0x0000, so they must be loaded at offset 0 in their segment
    ; DH parameter specifies target: 0x20=shell (0x2000), 0x30=user (0x3000)
    mov word [.code_off], 0         ; Always load at offset 0

    ; Step 6: Read file into app code segment
    mov ax, [.file_handle]
    ; Build segment from high byte: 0x20 -> 0x2000, 0x30 -> 0x3000
    xor bx, bx
    mov bh, [.target_seg]           ; BX = target segment (e.g., 0x2000 or 0x3000)
    mov es, bx
    xor di, di                      ; Offset 0 (for ORG 0 apps)
    mov cx, [.file_size]            ; Bytes to read
    call fs_read_stub
    jc .read_failed

    ; Step 7: Close file
    mov ax, [.file_handle]
    call fs_close_stub

    ; Step 8: Store app info in app_table entry
    mov ax, 0x1000
    mov ds, ax
    mov es, ax
    mov di, [.slot_off]

    mov byte [di + 0], APP_STATE_LOADED  ; State = loaded
    mov byte [di + 1], 0                 ; Priority = 0
    ; Store actual target segment
    xor ax, ax
    mov ah, [.target_seg]
    mov [di + 2], ax                     ; Code segment (from DH parameter)
    mov ax, [.code_off]
    mov [di + 4], ax                     ; Code offset
    mov ax, [.file_size]
    mov [di + 6], ax                     ; Code size
    mov word [di + 8], 0                 ; Stack segment (unused for now)
    mov word [di + 10], 0                ; Stack pointer (unused for now)

    ; Copy filename (11 bytes)
    push ds
    mov ax, [.filename_seg]
    mov ds, ax
    mov si, [.filename_off]
    add di, 12
    mov cx, 11
    rep movsb
    pop ds

    ; Step 9: Return app handle
    mov ax, [.slot]
    clc
    jmp .done

.mount_failed:
    mov ax, APP_ERR_MOUNT_FAILED
    jmp .error

.file_not_found:
    mov ax, APP_ERR_FILE_NOT_FOUND
    jmp .error

.read_failed:
    ; Close file before returning error
    mov ax, [.file_handle]
    call fs_close_stub
    mov ax, APP_ERR_READ_FAILED
    jmp .error

.error:
    stc

.done:
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; Local variables for app_load_stub
.drive:        db 0
.target_seg:   db 0                     ; Target segment high byte (0x20 or 0x30)
.filename_seg: dw 0
.filename_off: dw 0
.slot:         dw 0
.slot_off:     dw 0
.file_handle:  dw 0
.file_size:    dw 0
.code_off:     dw 0

; app_run_stub - Execute loaded application
; Input: AL = App handle (0-15) - only AL used, AH ignored (for INT 0x80 compatibility)
; Output: CF clear on success, AX = return value from app
;         CF set on error, AX = error code
; Preserves: None (registers may be modified by app)
app_run_stub:
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Validate app handle (use only AL - AH may contain function number from INT 0x80)
    xor ah, ah                      ; Clear AH, AX = handle
    cmp ax, APP_MAX_COUNT
    jae .invalid_handle

    ; Get app entry
    mov bx, ax
    shl bx, 5                       ; BX = handle * 32
    add bx, app_table

    ; Check if app is loaded
    cmp byte [bx + 0], APP_STATE_LOADED
    jne .not_loaded

    ; Mark as running
    mov byte [bx + 0], APP_STATE_RUNNING

    ; Get code segment and offset
    mov cx, [bx + 2]                ; Code segment
    mov dx, [bx + 4]                ; Code offset (entry point)

    ; Save app table offset for later
    push bx

    ; Set up for far call
    ; We'll push return address and use retf to call the app
    push cs
    push word .app_return

    ; Push app entry point
    push cx                         ; Segment
    push dx                         ; Offset

    ; Far return to app (acts as far call)
    retf

.app_return:
    ; App has returned, AX contains return value
    mov bx, ax                      ; Save return value

    ; Restore app table offset
    pop si                          ; SI = app table offset

    ; Mark as loaded (no longer running)
    mov byte [si + 0], APP_STATE_LOADED

    ; Return app's return value
    mov ax, bx
    clc
    jmp .done

.invalid_handle:
    mov ax, APP_ERR_INVALID_HANDLE
    stc
    jmp .done

.not_loaded:
    mov ax, APP_ERR_NOT_LOADED
    stc

.done:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; register_shell_stub - Register application as shell (auto-return target)
; Input: AL = App handle to register as shell
; Output: CF clear on success
;         CF set on error (invalid handle)
; Notes: When any non-shell app returns, kernel will auto-run shell
register_shell_stub:
    push bx
    push ds

    ; Validate handle
    cmp al, APP_MAX_COUNT
    jae .invalid

    ; Set kernel DS
    mov bx, 0x1000
    mov ds, bx

    ; Store shell handle
    xor ah, ah
    mov [shell_handle], ax

    clc
    jmp .done

.invalid:
    stc

.done:
    pop ds
    pop bx
    ret

; ============================================================================
; Window Manager API (v3.12.0)
; ============================================================================

; Window error codes
WIN_ERR_NO_SLOT     equ 1
WIN_ERR_INVALID     equ 2

; win_create_stub - Create a new window
; Input:  BX = X position, CX = Y position
;         DX = Width, SI = Height
;         DI = Pointer to title string (max 11 chars, null-terminated)
;         AL = Flags (WIN_FLAG_TITLE, WIN_FLAG_BORDER)
; Output: CF = 0 on success, AX = window handle (0-15)
;         CF = 1 on error, AX = error code
win_create_stub:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es

    ; Save parameters
    mov [.save_x], bx
    mov [.save_y], cx
    mov [.save_w], dx
    mov [.save_h], si
    mov [.save_title], di
    mov [.save_flags], al

    ; Find free window slot
    push ds
    mov bp, 0x1000
    mov ds, bp
    xor bp, bp                      ; BP = slot index

.find_slot:
    cmp bp, WIN_MAX_COUNT
    jae .no_slot
    mov bx, bp
    shl bx, 5                       ; BX = slot * 32
    add bx, window_table
    cmp byte [bx + WIN_OFF_STATE], WIN_STATE_FREE
    je .found_slot
    inc bp
    jmp .find_slot

.no_slot:
    pop ds
    mov ax, WIN_ERR_NO_SLOT
    stc
    jmp .exit

.found_slot:
    ; BX = pointer to window entry, BP = handle
    mov [.slot_off], bx

    ; Initialize window entry
    mov al, [.save_flags]
    test al, al
    jnz .has_flags
    mov al, WIN_FLAG_TITLE | WIN_FLAG_BORDER   ; Default flags
.has_flags:
    mov byte [bx + WIN_OFF_STATE], WIN_STATE_VISIBLE
    mov [bx + WIN_OFF_FLAGS], al

    mov ax, [.save_x]
    mov [bx + WIN_OFF_X], ax
    mov ax, [.save_y]
    mov [bx + WIN_OFF_Y], ax
    mov ax, [.save_w]
    mov [bx + WIN_OFF_WIDTH], ax
    mov ax, [.save_h]
    mov [bx + WIN_OFF_HEIGHT], ax

    mov byte [bx + WIN_OFF_ZORDER], 15          ; Top of stack
    mov byte [bx + WIN_OFF_OWNER], 0xFF         ; Kernel owned

    ; Copy title (up to 11 chars)
    ; App passes title as ES:DI. caller_es has the app's ES segment.
    ; DS = 0x1000 (kernel), we read from caller_es:saved_title, write to kernel
    mov di, bx
    add di, WIN_OFF_TITLE           ; DI = dest offset in window_table
    mov si, [.save_title]           ; SI = title offset from caller's DI
    mov ax, 0x1000
    mov es, ax                      ; ES = 0x1000 for writing to kernel
    push ds                         ; Save kernel DS
    mov ax, [caller_es]             ; AX = caller's ES (where title lives)
    mov ds, ax                      ; DS = caller's ES for reading title
    mov cx, 11
.copy_title:
    lodsb                           ; AL = [DS:SI++] from caller's segment
    test al, al
    jz .title_done
    mov [es:di], al                 ; Write to ES:DI = 0x1000:window_table
    inc di
    loop .copy_title
.title_done:
    mov byte [es:di], 0             ; Null terminate in kernel segment

    pop ds                          ; Restore kernel DS
    pop ds                          ; Balance push ds from line 2941

    ; Draw the window
    mov ax, bp                      ; Window handle
    call win_draw_stub

    ; Return handle
    mov ax, bp
    clc

.exit:
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.save_x:     dw 0
.save_y:     dw 0
.save_w:     dw 0
.save_h:     dw 0
.save_title: dw 0
.save_flags: db 0
.slot_off:   dw 0

; win_destroy_stub - Destroy a window
; Input:  AX = Window handle
; Output: CF = 0 on success
win_destroy_stub:
    push bx
    push cx
    push dx
    push si
    push ds

    ; Window handle is in AL (AH has function number from INT 0x80)
    xor ah, ah                      ; Clear AH, use AL as window handle
    cmp ax, WIN_MAX_COUNT
    jae .invalid

    ; Get window entry
    mov bx, 0x1000
    mov ds, bx
    mov bx, ax
    shl bx, 5
    add bx, window_table

    ; Check if window exists
    cmp byte [bx + WIN_OFF_STATE], WIN_STATE_FREE
    je .invalid

    ; Clear window area
    push bx
    mov cx, [bx + WIN_OFF_X]
    mov dx, [bx + WIN_OFF_Y]
    mov si, [bx + WIN_OFF_WIDTH]
    mov di, [bx + WIN_OFF_HEIGHT]
    ; gfx_clear_area expects: BX=X, CX=Y, DX=Width, SI=Height
    mov bx, cx                      ; BX = X
    mov cx, dx                      ; CX = Y
    mov dx, si                      ; DX = Width
    mov si, di                      ; SI = Height
    call gfx_clear_area_stub
    pop bx

    ; Mark as free
    mov byte [bx + WIN_OFF_STATE], WIN_STATE_FREE

    clc
    jmp .done

.invalid:
    mov ax, WIN_ERR_INVALID
    stc

.done:
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    ret

; win_draw_stub - Draw/redraw window frame (title bar and border)
; Input:  AX = Window handle
; Output: CF = 0 on success
win_draw_stub:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es
    push ds

    ; Window handle is in AL (AH has function number from INT 0x80)
    xor ah, ah                      ; Clear AH, use AL as window handle
    cmp ax, WIN_MAX_COUNT
    jae .invalid

    ; Get window entry
    mov bx, 0x1000
    mov ds, bx
    mov bx, ax
    shl bx, 5
    add bx, window_table

    ; Check if window is visible
    cmp byte [bx + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .invalid

    mov [.win_ptr], bx

    ; Get window dimensions
    mov cx, [bx + WIN_OFF_X]
    mov dx, [bx + WIN_OFF_Y]
    mov si, [bx + WIN_OFF_WIDTH]
    mov di, [bx + WIN_OFF_HEIGHT]

    ; Draw title bar (filled rectangle, 10 pixels high)
    push bx
    push cx
    push dx
    push si
    push di
    mov bx, cx                      ; BX = X
    mov cx, dx                      ; CX = Y
    mov dx, si                      ; DX = Width
    mov si, WIN_TITLEBAR_HEIGHT     ; SI = Height (10)
    call gfx_draw_filled_rect_stub
    pop di
    pop si
    pop dx
    pop cx
    pop bx

    ; Draw title text (inside title bar) - inverted (black on white)
    push bx
    mov si, bx
    add si, WIN_OFF_TITLE           ; SI = title pointer
    mov bx, cx                      ; X position
    add bx, 4                       ; 4px padding
    mov cx, dx                      ; Y position
    add cx, 1                       ; 1px padding
    call gfx_draw_string_inverted   ; Black text on white title bar
    pop bx

    ; Draw border (rectangle outline)
    push bx
    mov bx, [.win_ptr]
    mov cx, [bx + WIN_OFF_X]
    mov dx, [bx + WIN_OFF_Y]
    mov si, [bx + WIN_OFF_WIDTH]
    mov di, [bx + WIN_OFF_HEIGHT]
    mov bx, cx                      ; BX = X
    mov cx, dx                      ; CX = Y
    mov dx, si                      ; DX = Width
    mov si, di                      ; SI = Height
    call gfx_draw_rect_stub
    pop bx

    clc
    jmp .done

.invalid:
    mov ax, WIN_ERR_INVALID
    stc

.done:
    pop ds
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.win_ptr: dw 0

; win_focus_stub - Bring window to front (set z-order to max)
; Input:  AX = Window handle
; Output: CF = 0 on success
win_focus_stub:
    push bx
    push cx
    push ds

    ; Window handle is in AL (AH has function number from INT 0x80)
    xor ah, ah                      ; Clear AH, use AL as window handle
    cmp ax, WIN_MAX_COUNT
    jae .invalid

    mov bx, 0x1000
    mov ds, bx
    mov bx, ax
    shl bx, 5
    add bx, window_table

    cmp byte [bx + WIN_OFF_STATE], WIN_STATE_FREE
    je .invalid

    ; Set z-order to 15 (top)
    mov byte [bx + WIN_OFF_ZORDER], 15

    clc
    jmp .done

.invalid:
    stc

.done:
    pop ds
    pop cx
    pop bx
    ret

; win_move_stub - Move window to new position
; Input:  AX = Window handle, BX = New X, CX = New Y
; Output: CF = 0 on success
win_move_stub:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds

    ; Window handle is in AL (AH has function number from INT 0x80)
    xor ah, ah                      ; Clear AH, use AL as window handle
    mov [.new_x], bx
    mov [.new_y], cx
    mov [.handle], ax

    cmp ax, WIN_MAX_COUNT
    jae .invalid

    mov bx, 0x1000
    mov ds, bx
    mov bx, ax
    shl bx, 5
    add bx, window_table

    cmp byte [bx + WIN_OFF_STATE], WIN_STATE_FREE
    je .invalid

    ; Save window pointer
    mov bp, bx

    ; Clear old position
    mov cx, [bx + WIN_OFF_X]
    mov dx, [bx + WIN_OFF_Y]
    mov si, [bx + WIN_OFF_WIDTH]
    mov di, [bx + WIN_OFF_HEIGHT]
    mov bx, cx                      ; BX = X
    mov cx, dx                      ; CX = Y
    mov dx, si                      ; DX = Width
    mov si, di                      ; SI = Height
    call gfx_clear_area_stub

    ; Update position (BP = window pointer)
    mov bx, [.new_x]
    mov [bp + WIN_OFF_X], bx
    mov cx, [.new_y]
    mov [bp + WIN_OFF_Y], cx

    ; Redraw at new position
    mov ax, [.handle]
    call win_draw_stub

    clc
    jmp .done

.invalid:
    stc

.done:
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.new_x:  dw 0
.new_y:  dw 0
.handle: dw 0

; win_get_content_stub - Get content area bounds
; Input:  AX = Window handle
; Output: BX = Content X, CX = Content Y
;         DX = Content Width, SI = Content Height
;         CF = 0 on success
win_get_content_stub:
    push di
    push ds

    ; Window handle is in AL (AH has function number from INT 0x80)
    xor ah, ah                      ; Clear AH, use AL as window handle
    cmp ax, WIN_MAX_COUNT
    jae .invalid

    mov di, 0x1000
    mov ds, di
    mov di, ax
    shl di, 5
    add di, window_table

    cmp byte [di + WIN_OFF_STATE], WIN_STATE_FREE
    je .invalid

    ; Calculate content area
    ; Content X = Window X + 1 (border)
    mov bx, [di + WIN_OFF_X]
    inc bx

    ; Content Y = Window Y + titlebar + 1 (border)
    mov cx, [di + WIN_OFF_Y]
    add cx, WIN_TITLEBAR_HEIGHT
    inc cx

    ; Content Width = Window Width - 2 (borders)
    mov dx, [di + WIN_OFF_WIDTH]
    sub dx, 2

    ; Content Height = Window Height - titlebar - 2 (borders)
    mov si, [di + WIN_OFF_HEIGHT]
    sub si, WIN_TITLEBAR_HEIGHT
    sub si, 2

    clc
    jmp .done

.invalid:
    stc

.done:
    pop ds
    pop di
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

; System call dispatcher temp (v3.12.0)
syscall_func: dw 0
caller_ds: dw 0x1000                ; Caller's DS segment (init to kernel for direct calls)
caller_es: dw 0x1000                ; Caller's ES segment (init to kernel for direct calls)

; Keyboard driver state (Foundation 1.4)
old_int9_offset: dw 0
old_int9_segment: dw 0
kbd_buffer: times 16 db 0
kbd_buffer_head: dw 0
kbd_buffer_tail: dw 0
kbd_shift_state: db 0
kbd_ctrl_state: db 0
kbd_alt_state: db 0

; PS/2 Mouse driver state
old_int74_offset:   dw 0            ; Original IRQ12 vector
old_int74_segment:  dw 0
mouse_packet:       times 3 db 0    ; 3-byte packet buffer
mouse_packet_idx:   db 0            ; Current byte in packet (0-2)
mouse_x:            dw 160          ; Current X position (0-319)
mouse_y:            dw 100          ; Current Y position (0-199)
mouse_buttons:      db 0            ; Bit 0=left, bit 1=right, bit 2=middle
mouse_enabled:      db 0            ; 1 if mouse detected/enabled

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
; FAT16/Hard Drive Driver Data (v3.13.0)
; ============================================================================

; FAT16 mount state
fat16_mounted:          db 0            ; 1 if FAT16 volume mounted
fat16_drive:            db 0x80         ; Drive number (0x80=first HD)
fat16_partition_lba:    dd 0            ; Partition start LBA
fat16_bpb_cache:        times 62 db 0   ; BPB cache (just the important fields)

; FAT16 calculated offsets (32-bit LBA values)
fat16_fat_start:        dd 0            ; First FAT sector (LBA)
fat16_root_start:       dd 0            ; Root directory start (LBA)
fat16_data_start:       dd 0            ; Data area start (LBA)
fat16_root_entries:     dw 0            ; Number of root directory entries
fat16_sects_per_clust:  db 0            ; Sectors per cluster
fat16_reserved:         db 0            ; Reserved for alignment

; FAT16 FAT cache
fat16_fat_cache:        times 512 db 0  ; One sector FAT cache
fat16_fat_cached_sect:  dd 0xFFFFFFFF   ; Currently cached FAT sector (0xFFFFFFFF = invalid)

; Sector read buffer for FAT16 (used during mount/open)
fat16_sector_buf:       times 512 db 0

; IDE driver state
ide_drive_present:      db 0            ; Bit 0 = master, bit 1 = slave
ide_use_lba:            db 1            ; 1 = use LBA mode, 0 = CHS
ide_heads:              dw 16           ; Heads per cylinder (for CHS fallback)
ide_sectors:            dw 63           ; Sectors per track (for CHS fallback)

; ============================================================================
; Application Table (Core Services 2.1)
; ============================================================================

; Application table - track up to 16 loaded apps
; Each entry: 32 bytes
;   Offset 0:  1 byte  - State (0=free, 1=loaded, 2=running, 3=suspended)
;   Offset 1:  1 byte  - Priority (for future scheduler)
;   Offset 2:  2 bytes - Code segment
;   Offset 4:  2 bytes - Code offset (entry point, always 0)
;   Offset 6:  2 bytes - Code size
;   Offset 8:  2 bytes - Stack segment (for future multitasking)
;   Offset 10: 2 bytes - Stack pointer (for future multitasking)
;   Offset 12: 11 bytes - Filename (8.3 format)
;   Offset 23: 9 bytes - Reserved

APP_STATE_FREE      equ 0
APP_STATE_LOADED    equ 1
APP_STATE_RUNNING   equ 2
APP_STATE_SUSPENDED equ 3

APP_MAX_COUNT       equ 16
APP_ENTRY_SIZE      equ 32

; App segment constants (for dual-segment architecture)
APP_SEGMENT_SHELL   equ 0x2000              ; Shell/launcher segment
APP_SEGMENT_USER    equ 0x3000              ; User app segment

app_table: times (APP_MAX_COUNT * APP_ENTRY_SIZE) db 0

; Shell tracking (for auto-return to launcher)
shell_handle:       dw 0xFFFF               ; Handle of shell app (0xFFFF = none)

; ============================================================================
; Window Manager Data (v3.12.0)
; ============================================================================

; Window states
WIN_STATE_FREE      equ 0
WIN_STATE_VISIBLE   equ 1
WIN_STATE_HIDDEN    equ 2

; Window flags
WIN_FLAG_TITLE      equ 0x01
WIN_FLAG_BORDER     equ 0x02

; Window structure constants
WIN_MAX_COUNT       equ 16
WIN_ENTRY_SIZE      equ 32
WIN_TITLEBAR_HEIGHT equ 10

; Window entry structure (32 bytes):
;   Offset 0:  1 byte  - State (0=free, 1=visible, 2=hidden)
;   Offset 1:  1 byte  - Flags (bit 0=title, bit 1=border)
;   Offset 2:  2 bytes - X position
;   Offset 4:  2 bytes - Y position
;   Offset 6:  2 bytes - Width
;   Offset 8:  2 bytes - Height
;   Offset 10: 1 byte  - Z-order (0=bottom, 15=top)
;   Offset 11: 1 byte  - Owner app handle (0xFF=kernel)
;   Offset 12: 12 bytes - Title (11 chars + null)
;   Offset 24: 8 bytes - Reserved

; Window entry field offsets
WIN_OFF_STATE       equ 0
WIN_OFF_FLAGS       equ 1
WIN_OFF_X           equ 2
WIN_OFF_Y           equ 4
WIN_OFF_WIDTH       equ 6
WIN_OFF_HEIGHT      equ 8
WIN_OFF_ZORDER      equ 10
WIN_OFF_OWNER       equ 11
WIN_OFF_TITLE       equ 12

window_table: times (WIN_MAX_COUNT * WIN_ENTRY_SIZE) db 0

; ============================================================================
; Padding
; ============================================================================

; Pad to 28KB (56 sectors) - expanded for FAT12 filesystem (v3.10.0)
times 28672 - ($ - $$) db 0
