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

    ; Install INT 0x80 handler for system calls
    call install_int_80

    ; Initialize mouse
    call install_mouse

    ; Install keyboard handler
    call install_keyboard

    ; Disable mouse cursor rendering for the entire boot graphics phase.
    ; CLI alone is NOT sufficient: BIOS INT 0x10 can execute STI internally
    ; during mode switch, palette and background register writes. If IRQ12
    ; fires during a BIOS call, mouse_cursor_show XOR-draws the cursor into
    ; CGA memory that then gets cleared by rep stosw or BIOS reinitialization,
    ; breaking the XOR invariant and leaving a permanent ghost at (160,100).
    ; Setting mouse_enabled=0 makes mouse_cursor_show a no-op even if called.
    mov byte [mouse_enabled], 0

    cli

    ; Set CGA mode 4 (320x200x4)
    mov byte [video_mode], 0x04
    mov word [video_segment], 0xB800
    xor ax, ax
    mov al, 0x04
    int 0x10

    call setup_graphics_post_mode

    ; Reset cursor state: even if IRQ12 fired during BIOS calls and set
    ; cursor_visible=1, force it to 0 so mouse_cursor_show at line 79
    ; will draw the cursor fresh at the correct position.
    mov byte [cursor_visible], 0
    mov byte [mouse_enabled], 1

    ; ========== PHASE 2: Graphics init complete ==========

    ; Initialize caller_ds for direct kernel calls to gfx_draw_string_stub
    mov word [caller_ds], 0x1000

    ; Initialize draw colors from theme
    mov al, [text_color]
    mov [draw_fg_color], al
    mov al, [desktop_bg_color]
    mov [draw_bg_color], al

    ; Display version number (top-left corner) — CGA boot splash
    mov bx, 4
    mov cx, 4
    mov si, version_string
    call gfx_draw_string_stub

    ; Display build number (below version)
    mov bx, 4
    mov cx, 14
    mov si, build_string
    call gfx_draw_string_stub

    ; Enable interrupts
    sti

    ; Draw initial mouse cursor (if mouse was detected)
    call mouse_cursor_show

    ; Auto-load launcher from boot disk
    call auto_load_launcher

    ; If we get here, auto_load_launcher failed
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
; Output: Function-specific return values, CF=error status
; NOTE: AH=0 is gfx_draw_pixel (no longer API discovery)
int_80_handler:
    ; Validate function number (0-56 valid)
    cmp ah, 98                      ; Max function count (0-97 valid)
    jae .invalid_function

    ; Save caller's DS and ES to kernel variables (use CS: since DS not yet changed)
    mov [cs:caller_ds], ds
    mov [cs:caller_es], es

    ; Re-enable hardware interrupts for API functions
    ; INT instruction clears IF, but API functions (especially disk I/O via INT 0x13)
    ; need IRQ 6 (floppy controller) to complete DMA transfers on real hardware.
    ; IRET will restore original FLAGS including IF state.
    sti

    ; Save caller's DS (apps may have different DS)
    push ds

    ; Set DS to kernel segment for API functions
    push ax
    mov ax, 0x1000
    mov ds, ax
    ; Set draw foreground color from text theme color
    mov al, [text_color]
    mov [draw_fg_color], al
    ; Auto-set draw_bg_color: window context = 0 (black), no context = desktop color
    cmp byte [draw_context], 0xFF
    jne .set_bg_win
    mov al, [desktop_bg_color]
    mov [draw_bg_color], al
    jmp .set_bg_end
.set_bg_win:
    mov byte [draw_bg_color], 0
.set_bg_end:
    pop ax

    ; --- Window-relative coordinate translation ---
    ; If draw_context is active (valid window handle) and function is a
    ; drawing API (0-6), translate BX/CX from window-relative to absolute
    ; screen coordinates. Apps draw at (0,0) = top-left of content area.
    ; BX/CX are restored after the API call so apps keep their original coords.
    mov byte [_did_translate], 0    ; Clear translation flag
    cmp byte [draw_context], 0xFF
    je .no_translate                ; No active context
    cmp byte [draw_context], WIN_MAX_COUNT
    jae .no_translate               ; Invalid context, treat as no context
    cmp ah, 6
    jbe .do_translate               ; APIs 0-6: drawing functions
    cmp ah, 50
    jb .no_translate                ; APIs 7-49: no translation
    cmp ah, 53
    jb .do_translate                ; APIs 50-52: translate coords
    cmp ah, 56
    jb .no_translate                ; APIs 53-55: no translation
    cmp ah, 63
    jb .do_translate                ; APIs 56-62: widget APIs
    cmp ah, 65
    jb .no_translate                ; APIs 63-64: not drawing, skip
    cmp ah, 67
    jb .do_translate                ; APIs 65-66: combobox, menubar
    cmp ah, 72
    jb .do_translate                ; APIs 67-71: colored drawing + line
    cmp ah, 80
    jb .no_translate                ; APIs 72-79: non-drawing
    cmp ah, 81
    jb .do_translate                ; API 80: gfx_scroll_area
    cmp ah, 87
    jb .no_translate                ; APIs 81-86: non-drawing
    cmp ah, 88
    jb .do_translate                ; API 87: menu_open
    cmp ah, 94
    je .do_translate                ; API 94: gfx_draw_sprite
    jmp .no_translate               ; Others: skip translation
.do_translate:                      ; APIs 0-6, 50-52, 56-62, 65-66: translate

    ; Save original BX/CX so we can restore them after the API call.
    ; Without this, apps that reuse BX/CX across drawing calls get
    ; cumulative translation offsets (e.g. Music staff lines drifting).
    mov [_save_bx], bx
    mov [_save_cx], cx
    mov byte [_did_translate], 1
    mov byte [_did_scale], 0

    ; Translate: BX += content_x, CX += content_y
    push si
    push ax
    xor ah, ah
    mov al, [draw_context]
    mov si, ax
    shl si, 5                       ; SI = handle * 32
    add si, window_table

    ; Content scaling: double app coordinates for auto-scaled windows
    cmp byte [si + WIN_OFF_CONTENT_SCALE], 2
    jne .no_content_scale
    shl bx, 1                       ; app_x * 2
    shl cx, 1                       ; app_y * 2
    mov byte [_did_scale], 1         ; Flag for DX/SI scaling after zclip
.no_content_scale:

    ; content_x = win_x + 1 (inside left border)
    add bx, [si + WIN_OFF_X]
    inc bx
    ; content_y = win_y + titlebar height (below title bar)
    add cx, [si + WIN_OFF_Y]
    add cx, [titlebar_height]

    ; --- Z-order clipping: only topmost window can draw to screen ---
    ; Background apps keep running but their draws are silently dropped.
    ; When they become foreground, WIN_REDRAW triggers a full repaint.
    cmp byte [si + WIN_OFF_ZORDER], 15
    je .zclip_ok                    ; This IS the topmost window, draw freely
    ; Not topmost — skip draw entirely
    pop ax
    pop si
    clc
    jmp int80_return_point

.zclip_ok:
    pop ax
    pop si

    ; --- Content dimension scaling for auto-scaled windows ---
    cmp byte [_did_scale], 0
    je .no_dim_scale

    ; Save originals for restore after API call
    mov [_save_dx], dx
    mov [_save_si], si

    ; Group A: Scale DX (width) AND SI (height)
    ;   APIs 0-2 (pixel/rect/fill), 5 (clear_area), 51 (button),
    ;   58 (scrollbar), 61 (groupbox), 67-68 (color rect), 80 (scroll area)
    cmp ah, 3
    jb .scale_dx_si
    cmp ah, 5
    je .scale_dx_si
    cmp ah, 51
    je .scale_dx_si
    cmp ah, 58
    je .scale_dx_si
    cmp ah, 61
    je .scale_dx_si
    cmp ah, 67
    je .scale_dx_si
    cmp ah, 68
    je .scale_dx_si
    cmp ah, 80
    je .scale_dx_si

    ; Group B: Scale DX only (SI is pointer or non-dimension)
    ;   APIs 50 (string_wrap), 57 (textfield), 59 (listitem),
    ;   60 (progress bar), 62 (separator), 65-66 (combobox/menubar),
    ;   69-70 (h/vline)
    cmp ah, 50
    je .scale_dx_only
    cmp ah, 57
    je .scale_dx_only
    cmp ah, 59
    je .scale_dx_only
    cmp ah, 60
    je .scale_dx_only
    cmp ah, 62
    je .scale_dx_only
    cmp ah, 65
    je .scale_dx_only
    cmp ah, 66
    je .scale_dx_only
    cmp ah, 69
    je .scale_dx_only
    cmp ah, 70
    je .scale_dx_only

    ; Group D: Scale DH only (DL is count, not dimension) - API 87 (menu_open)
    ; Note: API 94 (sprite) NOT scaled — bitmap data is fixed-size, can't double DH/DL
    cmp ah, 87
    je .scale_dh_only

    ; Default: no DX/SI scaling for this API
    mov byte [_did_scale], 0         ; Clear flag to skip restore
    jmp .no_dim_scale

.scale_dx_si:
    shl dx, 1
    shl si, 1
    jmp .no_dim_scale
.scale_dx_only:
    shl dx, 1
    jmp .no_dim_scale
.scale_dh_only:
    shl dh, 1                       ; menu width * 2
.no_dim_scale:

    ; For draw_line (API 71), also translate DX/SI (second endpoint X2,Y2)
    cmp ah, 71
    jne .no_translate
    push di
    push ax
    xor ah, ah
    mov al, [draw_context]
    mov di, ax
    shl di, 5
    add di, window_table
    ; Scale second endpoint if content_scale=2
    cmp byte [di + WIN_OFF_CONTENT_SCALE], 2
    jne .line_no_scale
    shl dx, 1                       ; X2 * 2
    shl si, 1                       ; Y2 * 2
.line_no_scale:
    add dx, [di + WIN_OFF_X]
    inc dx
    add si, [di + WIN_OFF_Y]
    add si, [titlebar_height]
    pop ax
    pop di
    jmp .no_translate

.no_translate:
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
    push word int80_return_point
    jmp word [cs:syscall_func]

.invalid_function:
    stc                             ; Set carry flag for error
    iret

int80_return_point:
    ; Function returned - preserve CF from function in return FLAGS
    ; Stack: [caller's DS] [IP] [CS] [FLAGS]
    ; IMPORTANT: Do NOT destroy AX - functions return values in AX!

    ; Restore pre-translation BX/CX so apps keep their original coordinates.
    ; DS is still kernel (0x1000) at this point (caller DS is on stack).
    ; CRITICAL: pushf/popf preserves CF from the API function — the cmp
    ; below would otherwise clobber it, breaking all CF-based return status.
    pushf
    cmp byte [_did_translate], 0
    je .no_coord_restore
    mov bx, [_save_bx]
    mov cx, [_save_cx]
    cmp byte [_did_scale], 0
    je .no_coord_restore
    mov dx, [_save_dx]
    mov si, [_save_si]
.no_coord_restore:
    popf
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

; ============================================================================
; Keyboard Driver (Foundation 1.4)
; ============================================================================

; Install keyboard interrupt handler
install_keyboard:
    push es
    push ax
    push bx

    ; Initialize keyboard buffer
    mov word [kbd_buffer_head], 0
    mov word [kbd_buffer_tail], 0
    mov byte [kbd_shift_state], 0
    mov byte [kbd_ctrl_state], 0
    mov byte [kbd_alt_state], 0

    ; Install custom INT 9 handler on all boot types.
    ; This ensures keyboard events go through the event queue with focus filtering,
    ; preventing the launcher from stealing keystrokes meant for focused apps.
    ; (The old BIOS INT 16h polling fallback remains but is no longer the primary path.)

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
    mov byte [use_bios_keyboard], 0
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

    ; Handle E0 prefix (extended keys like arrow keys)
    cmp al, 0xE0
    je .set_e0_flag

    ; Check if previous scancode was E0 prefix
    cmp byte [kbd_e0_flag], 1
    je .handle_extended

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
    jmp .store_key

.set_e0_flag:
    mov byte [kbd_e0_flag], 1
    jmp .done

.handle_extended:
    mov byte [kbd_e0_flag], 0       ; Clear flag
    ; Handle Right Ctrl (E0 + 0x1D make / 0x9D release)
    cmp al, 0x1D
    je .ctrl_press
    cmp al, 0x9D
    je .ctrl_release
    ; Ignore extended key releases
    test al, 0x80
    jnz .done
    ; Map extended scancodes to special key codes
    cmp al, 0x48                    ; Up arrow
    je .arrow_up
    cmp al, 0x50                    ; Down arrow
    je .arrow_down
    cmp al, 0x4B                    ; Left arrow
    je .arrow_left
    cmp al, 0x4D                    ; Right arrow
    je .arrow_right
    cmp al, 0x1C                    ; Numpad Enter
    je .numpad_enter
    cmp al, 0x47                    ; Home
    je .ext_home
    cmp al, 0x4F                    ; End
    je .ext_end
    cmp al, 0x53                    ; Delete
    je .ext_delete
    cmp al, 0x49                    ; Page Up
    je .ext_pgup
    cmp al, 0x51                    ; Page Down
    je .ext_pgdn
    jmp .done                       ; Ignore other extended keys

.arrow_up:
    mov al, 128                     ; Special code for Up arrow
    jmp .store_key
.arrow_down:
    mov al, 129                     ; Special code for Down arrow
    jmp .store_key
.arrow_left:
    mov al, 130                     ; Special code for Left arrow
    jmp .store_key
.arrow_right:
    mov al, 131                     ; Special code for Right arrow
    jmp .store_key
.numpad_enter:
    mov al, 13                      ; Same as regular Enter
    jmp .store_key
.ext_home:
    mov al, 132
    jmp .store_key
.ext_end:
    mov al, 133
    jmp .store_key
.ext_delete:
    mov al, 134
    jmp .store_key
.ext_pgup:
    mov al, 135
    jmp .store_key
.ext_pgdn:
    mov al, 136

.store_key:
    ; Don't store null characters
    test al, al
    jz .done

    ; Map Ctrl+letter to control codes (ASCII 1-26)
    cmp byte [kbd_ctrl_state], 0
    je .no_ctrl_map
    cmp al, 'a'
    jb .check_upper_ctrl
    cmp al, 'z'
    ja .no_ctrl_map
    sub al, 96                      ; 'a'(97)->1, 'c'(99)->3, 'v'(118)->22, etc.
    jmp .no_ctrl_map
.check_upper_ctrl:
    cmp al, 'A'
    jb .no_ctrl_map
    cmp al, 'Z'
    ja .no_ctrl_map
    sub al, 64                      ; 'A'(65)->1, etc.
.no_ctrl_map:

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
KBC_CMD_DISABLE_AUX equ 0xA7        ; Disable auxiliary interface

; Mouse commands (sent via 0xD4)
MOUSE_CMD_RESET     equ 0xFF        ; Reset mouse
MOUSE_CMD_ENABLE    equ 0xF4        ; Enable data reporting
MOUSE_CMD_DISABLE   equ 0xF5        ; Disable data reporting
MOUSE_CMD_DEFAULTS  equ 0xF6        ; Set defaults

; install_mouse - Initialize PS/2 mouse
; Tries BIOS INT 15h/C2 services first (works with USB legacy emulation),
; falls back to direct KBC port I/O for native PS/2 / QEMU.
; Output: CF=0 success, CF=1 no mouse detected
install_mouse:
    push ax
    push bx
    push cx
    push es

    ; ===== Method 1: BIOS PS/2 mouse services (INT 15h/C2xx) =====
    ; Works on any BIOS with PS/2 support — handles USB emulation transparently.
    ; We don't touch KBC ports or PIC masks; the BIOS manages all of that.

    ; Initialize pointing device interface (3-byte packets)
    mov ax, 0xC205
    mov bh, 3
    int 0x15
    jc .try_kbc                      ; BIOS PS/2 services not available

    ; Set our callback handler (ES:BX = handler address)
    mov ax, 0x1000
    mov es, ax
    mov bx, mouse_bios_callback
    mov ax, 0xC207
    int 0x15
    jc .try_kbc

    ; Enable pointing device
    mov ax, 0xC200
    mov bh, 1                        ; 1 = enable
    int 0x15
    jc .try_kbc

    ; BIOS method succeeded
    mov byte [mouse_diag], 'B'      ; B = BIOS method
    jmp .init_success

    ; ===== Method 2: Direct KBC port I/O (fallback) =====
.try_kbc:
    ; Save original INT 0x74 (IRQ12) vector
    xor ax, ax
    mov es, ax
    mov ax, [es:0x01D0]
    mov [old_int74_offset], ax
    mov ax, [es:0x01D2]
    mov [old_int74_segment], ax

    ; Flush stale data from KBC output buffer
    mov cx, 16
.flush_kbc:
    in al, KBC_STATUS
    test al, KBC_STAT_OBF
    jz .flush_done
    in al, KBC_DATA
    loop .flush_kbc
.flush_done:

    ; Disable keyboard during mouse init
    call kbc_wait_write
    mov al, 0xAD
    out KBC_CMD, al

    ; Save original KBC config
    call kbc_wait_write
    mov al, KBC_CMD_READ_CFG
    out KBC_CMD, al
    call kbc_wait_read
    in al, KBC_DATA
    mov [saved_kbc_config], al

    ; Enable auxiliary interface
    call kbc_wait_write
    mov al, KBC_CMD_ENABLE_AUX
    out KBC_CMD, al

    ; Write config: enable IRQ12 (bit 1), enable aux clock (clear bit 5)
    mov bl, [saved_kbc_config]
    or bl, 0x02
    and bl, 0xDF
    call kbc_wait_write
    mov al, KBC_CMD_WRITE_CFG
    out KBC_CMD, al
    call kbc_wait_write
    mov al, bl
    out KBC_DATA, al

    ; Flush after config change
    mov cx, 16
.flush_kbc2:
    in al, KBC_STATUS
    test al, KBC_STAT_OBF
    jz .flush_done2
    in al, KBC_DATA
    loop .flush_kbc2
.flush_done2:

    ; Reset mouse (retry up to 3 times)
    mov cl, 3
.try_reset:
    mov al, MOUSE_CMD_RESET
    call mouse_send_cmd
    cmp al, 0xFA
    je .reset_ok
    dec cl
    jnz .try_reset
    jmp .fail_reset
.reset_ok:

    ; Wait for self-test (0xAA)
    call kbc_wait_read_long
    in al, KBC_DATA
    cmp al, 0xAA
    jne .fail_selftest

    ; Read device ID
    call kbc_wait_read_long
    in al, KBC_DATA

    ; Set defaults + enable data reporting
    mov al, MOUSE_CMD_DEFAULTS
    call mouse_send_cmd
    mov al, MOUSE_CMD_ENABLE
    call mouse_send_cmd
    cmp al, 0xFA
    jne .fail_enable

    ; Install our IRQ12 handler
    cli
    xor ax, ax
    mov es, ax
    mov word [es:0x01D0], int_74_handler
    mov word [es:0x01D2], 0x1000
    sti

    ; Unmask IRQ12 on slave PIC and IRQ2 cascade on master PIC
    in al, 0xA1
    and al, 0xEF
    out 0xA1, al
    in al, 0x21
    and al, 0xFB
    out 0x21, al

    ; Re-enable keyboard
    call kbc_wait_write
    mov al, 0xAE
    out KBC_CMD, al

    mov byte [mouse_diag], 'K'      ; K = KBC method
    jmp .init_success

.fail_reset:
    mov byte [mouse_diag], 'R'
    jmp .no_mouse
.fail_selftest:
    mov byte [mouse_diag], 'S'
    jmp .no_mouse
.fail_enable:
    mov byte [mouse_diag], 'E'

.no_mouse:
    ; Restore original KBC config
    call kbc_wait_write
    mov al, KBC_CMD_WRITE_CFG
    out KBC_CMD, al
    call kbc_wait_write
    mov al, [saved_kbc_config]
    out KBC_DATA, al
    call kbc_wait_write
    mov al, KBC_CMD_DISABLE_AUX
    out KBC_CMD, al
    call kbc_wait_write
    mov al, 0xAE
    out KBC_CMD, al

    mov byte [mouse_enabled], 0
    stc
    jmp .done

.init_success:
    mov word [mouse_x], 160
    mov word [mouse_y], 100
    mov byte [mouse_buttons], 0
    mov byte [mouse_packet_idx], 0
    mov byte [mouse_enabled], 1
    clc

.done:
    pop es
    pop cx
    pop bx
    pop ax
    ret

; kbc_wait_write - Wait for keyboard controller ready to accept command
; Clobbers: AL
kbc_wait_write:
    push cx
    mov cx, 0x0100                  ; 256 iterations (real KBC responds in <10)
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
    mov cx, 0x0100                  ; 256 iterations (real KBC responds in <10)
.wait:
    in al, KBC_STATUS
    test al, KBC_STAT_OBF           ; Output buffer full?
    jnz .done                       ; Yes, data available
    loop .wait
.done:
    pop cx
    ret

; kbc_wait_read_long - Wait for KBC data with ~1 second timeout
; Uses BIOS timer tick at 0040:006C (18.2 Hz) for CPU-speed independence
; Needed for mouse reset self-test which takes 300-500ms on real hardware
; Has raw counter fallback in case BIOS timer is not running (USB boot)
; Clobbers: AL
kbc_wait_read_long:
    push cx
    push dx
    push es
    push ax
    mov ax, 0x0040
    mov es, ax
    sti                             ; Ensure timer IRQ is firing
    mov cx, [es:0x006C]            ; Start tick
    mov dx, 0xFFFF                 ; Raw fallback counter (~1s on XT-class hardware)
.wait:
    in al, KBC_STATUS
    test al, KBC_STAT_OBF
    jnz .ready
    ; BIOS timer check (primary timeout)
    mov ax, [es:0x006C]
    sub ax, cx                      ; Elapsed ticks (wraps correctly)
    cmp ax, 20                      ; ~1.1 second timeout
    jae .ready
    ; Raw counter fallback (fast bail on SMI-heavy systems)
    dec dx
    jnz .wait
.ready:
    pop ax
    pop es
    pop dx
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

    call kbc_wait_read_long          ; Long timeout — real hardware can be slow
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
    push es
    push si
    push di
    push bp

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

    ; Erase cursor before updating position
    call mouse_cursor_hide

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

    ; Clamp X to 0..screen_width-1
    cmp word [mouse_x], 0x8000      ; Negative (wrapped)?
    jb .x_not_neg
    mov word [mouse_x], 0
    jmp .do_y
.x_not_neg:
    mov ax, [screen_width]
    dec ax
    cmp [mouse_x], ax
    jbe .do_y
    mov [mouse_x], ax

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

    ; Clamp Y to 0..screen_height-1
    cmp word [mouse_y], 0x8000
    jb .y_not_neg
    mov word [mouse_y], 0
    jmp .post_event
.y_not_neg:
    mov ax, [screen_height]
    dec ax
    cmp [mouse_y], ax
    jbe .post_event
    mov [mouse_y], ax

.post_event:
    ; Update drag state machine (sets flags only, no win_move)
    call mouse_drag_update

    ; Redraw cursor at new position
    call mouse_cursor_show

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

    pop bp
    pop di
    pop si
    pop es
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    iret

; BIOS PS/2 mouse callback handler
; Called by BIOS via FAR CALL from its own IRQ12 handler.
; The BIOS handles all KBC/USB/PIC details — we just process the packet.
;
; Stack layout (verified via QEMU/SeaBIOS raw dump: 00 FB 0A 28):
;   BIOS pushes: status, X, Y, 0, then CALL FAR handler
;   After push bp / mov bp, sp:
;     [BP+6]  = 0 (padding)
;     [BP+8]  = Y delta      (0-255, sign in status bit 5)
;     [BP+10] = X delta      (0-255, sign in status bit 4)
;     [BP+12] = status byte  (YO XO YS XS 1 M R L)
mouse_bios_callback:
    push bp
    mov bp, sp
    push ax
    push bx
    push cx
    push dx
    push ds
    push es
    push si
    push di

    mov ax, 0x1000
    mov ds, ax

    ; Read packet from stack — verified layout from QEMU/SeaBIOS dump:
    ;   BIOS pushes: status, X, Y, 0, then CALL FAR handler
    ;   [BP+6]  = 0 (padding)
    ;   [BP+8]  = Y delta
    ;   [BP+10] = X delta
    ;   [BP+12] = status byte
    ; DH holds status throughout (never clobbered)
    mov dh, [bp+12]                 ; Status byte
    mov bl, [bp+10]                 ; X delta
    mov cl, [bp+8]                  ; Y delta

    ; Extract buttons (bits 0-2 of status)
    mov al, dh
    and al, 0x07
    mov [mouse_buttons], al

    ; Skip if overflow
    test dh, 0xC0
    jnz .bios_cb_done

    ; Hide cursor before updating position
    call mouse_cursor_hide

    ; === X delta: sign-extend BL using status bit 4 ===
    xor ah, ah
    mov al, bl                      ; AX = unsigned X (0-255)
    test dh, 0x10                   ; X sign bit in status
    jz .bios_x_pos
    mov ah, 0xFF                    ; Sign-extend negative
.bios_x_pos:
    add [mouse_x], ax

    ; Clamp X to 0..screen_width-1
    cmp word [mouse_x], 0x8000
    jb .bios_x_not_neg
    mov word [mouse_x], 0
    jmp .bios_do_y
.bios_x_not_neg:
    mov ax, [screen_width]
    dec ax
    cmp [mouse_x], ax
    jbe .bios_do_y
    mov [mouse_x], ax

.bios_do_y:
    ; === Y delta: sign-extend CL using status bit 5, then negate ===
    xor ah, ah
    mov al, cl                      ; AX = unsigned Y (0-255)
    test dh, 0x20                   ; Y sign bit in status (DH still intact!)
    jz .bios_y_pos
    mov ah, 0xFF                    ; Sign-extend negative
.bios_y_pos:
    neg ax                          ; Invert for screen coords (mouse up = Y--)
    add [mouse_y], ax

    ; Clamp Y to 0..screen_height-1
    cmp word [mouse_y], 0x8000
    jb .bios_y_not_neg
    mov word [mouse_y], 0
    jmp .bios_post
.bios_y_not_neg:
    mov ax, [screen_height]
    dec ax
    cmp [mouse_y], ax
    jbe .bios_post
    mov [mouse_y], ax

.bios_post:
    call mouse_drag_update
    call mouse_cursor_show

    ; Post mouse event
    xor dx, dx
    mov dl, [mouse_buttons]
    mov al, EVENT_MOUSE
    call post_event

.bios_cb_done:
    pop di
    pop si
    pop es
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    pop bp
    retf                            ; Far return to BIOS IRQ handler

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
    mov ax, [screen_width]
    dec ax
    cmp bx, ax
    jbe .x_ok
    mov bx, ax
.x_ok:
    mov [mouse_x], bx
    mov ax, [screen_height]
    dec ax
    cmp cx, ax
    jbe .y_ok
    mov cx, ax
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
; Window Drawing Context API
; Apps call begin_draw with their window handle. All subsequent drawing
; calls (API 0-6) auto-translate coordinates to window-relative.
; End_draw switches back to absolute/fullscreen mode.
; ============================================================================

; win_begin_draw - Set window drawing context
; Input: AL = window handle
; Effect: Drawing APIs (0-6) will translate coordinates relative to
;         window content area until win_end_draw is called
win_begin_draw:
    cmp al, WIN_MAX_COUNT
    jae .wbd_invalid
    mov [draw_context], al
    ; Set up clip rectangle from window content area
    push bx
    push si
    mov bl, al
    xor bh, bh
    shl bx, 5                      ; BX = handle * 32
    add bx, window_table
    mov si, bx
    ; clip_x1 = win_x + 1
    mov bx, [si + WIN_OFF_X]
    inc bx
    mov [clip_x1], bx
    ; clip_y1 = win_y + TITLEBAR_HEIGHT
    mov bx, [si + WIN_OFF_Y]
    add bx, [titlebar_height]
    mov [clip_y1], bx
    ; clip_x2 = win_x + win_w - 2 (inside right border)
    mov bx, [si + WIN_OFF_X]
    add bx, [si + WIN_OFF_WIDTH]
    sub bx, 2
    mov [clip_x2], bx
    ; clip_y2 = win_y + win_h - 2 (inside bottom border)
    mov bx, [si + WIN_OFF_Y]
    add bx, [si + WIN_OFF_HEIGHT]
    sub bx, 2
    mov [clip_y2], bx
    mov byte [clip_enabled], 1
    pop si
    pop bx
    ret
.wbd_invalid:
    stc
    ret

; win_end_draw - Clear window drawing context (fullscreen mode)
; Effect: Drawing APIs use absolute screen coordinates
win_end_draw:
    mov byte [draw_context], 0xFF
    mov byte [clip_enabled], 0
    ret

; ============================================================================
; Version String (auto-generated from VERSION and BUILD_NUMBER files)
; ============================================================================

; Include auto-generated version/build info
%include "build_info.inc"

; Aliases for compatibility
version_string equ VERSION_STR
build_string   equ BUILD_NUMBER_STR

; Boot configuration
boot_drive:         db 0                ; Boot drive number (0x00=floppy, 0x80=HDD)
use_bios_keyboard:  db 0                ; 1=use INT 16h (USB boot), 0=custom INT 9

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
    mov dx, [screen_width]
    mov si, [screen_height]
    sub si, 10
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
    push bx
    push dx
    push si

    ; Load LAUNCHER.BIN from boot drive
    mov ax, 0x1000
    mov ds, ax
    mov si, .launcher_filename
    mov dl, [boot_drive]            ; Use saved boot drive number
    mov dh, 0x20                    ; Load to shell segment (0x2000)
    call app_load_stub
    jc .fail_load

    ; Load saved settings (font + colors) from SETTINGS.CFG if it exists
    call load_settings

    ; Redraw splash text (load_settings may have switched video mode, clearing screen)
    ; Center horizontally based on current screen width
    mov word [caller_ds], 0x1000
    mov bx, [screen_width]
    shr bx, 1
    sub bx, 60                      ; Approximate center for ~15 char strings
    mov cx, 4
    mov si, version_string
    call gfx_draw_string_stub
    mov bx, [screen_width]
    shr bx, 1
    sub bx, 60
    mov cx, 14
    mov si, build_string
    call gfx_draw_string_stub

    ; Start launcher as a cooperative task (non-blocking)
    call app_start_stub
    jc .fail_start

    ; Enter scheduler - switch to the launcher task
    mov byte [current_task], 0xFF   ; Kernel is not a task
    call scheduler_next             ; Find the launcher task
    cmp al, 0xFF
    je .fail_sched

    ; Initial context switch to first task
    mov [current_task], al
    mov [scheduler_last], al

    ; Restore per-task state
    mov al, [bx + APP_OFF_DRAW_CTX]
    mov [draw_context], al
    mov byte [clip_enabled], 0          ; New task starts with no clip
    mov al, [bx + APP_OFF_FONT]
    push bx
    call gfx_set_font
    pop bx
    mov ax, [bx + APP_OFF_CALLER_DS]
    mov [caller_ds], ax
    mov ax, [bx + APP_OFF_CALLER_ES]
    mov [caller_es], ax

    ; Switch stack and enter task
    cli
    mov ss, [bx + APP_OFF_STACK_SEG]
    mov sp, [bx + APP_OFF_STACK_PTR]
    sti
    popa                            ; Consume dummy pusha frame (Build 198 added this)
    ret                             ; Now pops int80_return_point correctly

.fail_load:
    ; app_load_stub failed — draw error code (AX) on CGA screen
    ; AX has the error code from app_load_stub
    push ax                         ; Save error code
    mov word [caller_ds], 0x1000
    mov bx, 4
    mov cx, 30
    mov si, .err_load_msg
    call gfx_draw_string_stub
    pop ax                          ; Restore error code
    ; Draw error code digit at (170, 30)
    add al, '0'                     ; Convert 1-7 to ASCII digit
    mov [.err_code_str], al
    mov bx, 170
    mov cx, 30
    mov si, .err_code_str
    call gfx_draw_string_stub
    jmp .failed
.fail_start:
    mov word [caller_ds], 0x1000
    mov bx, 4
    mov cx, 30
    mov si, .err_start_msg
    call gfx_draw_string_stub
    jmp .failed
.fail_sched:
    mov word [caller_ds], 0x1000
    mov bx, 4
    mov cx, 30
    mov si, .err_sched_msg
    call gfx_draw_string_stub

.failed:
    ; On any error, fall through to keyboard demo
    pop si
    pop dx
    pop bx
    pop ax
    call keyboard_demo
    ret

; Local data
.launcher_filename: db 'LAUNCHER.BIN', 0
.err_load_msg:  db 'ERR: app_load failed', 0
.err_code_str:  db '?', 0
.err_start_msg: db 'ERR: app_start failed', 0
.err_sched_msg: db 'ERR: scheduler failed', 0

; ============================================================================
; Load saved settings from SETTINGS.CFG on boot drive
; Called after app_load_stub (filesystem is mounted)
; Silently does nothing if file doesn't exist or is invalid
; ============================================================================
load_settings:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    ; Open SETTINGS.CFG (filesystem already mounted by app_load_stub)
    ; Route by boot drive type (FAT12 for floppy, FAT16 for HD)
    mov si, .ls_filename
    cmp byte [boot_drive], 0x80
    jae .ls_open16
    call fat12_open
    jmp .ls_opened
.ls_open16:
    call fat16_open
.ls_opened:
    jc .ls_done                         ; File not found = use defaults

    ; Save file handle
    mov [.ls_fh], al

    ; Read 6 bytes (magic + font + text + bg + win + video_mode)
    xor ah, ah                          ; AX = file handle
    mov bx, 0x1000
    mov es, bx
    mov cx, 6
    cmp byte [boot_drive], 0x80
    jae .ls_read16
    mov di, .ls_buf
    call fat12_read
    jmp .ls_read_done
.ls_read16:
    mov bx, .ls_buf                     ; fat16_read uses ES:BX
    call fat16_read
.ls_read_done:
    jc .ls_close

    ; Close file first
    xor ah, ah
    mov al, [.ls_fh]
    cmp byte [boot_drive], 0x80
    jae .ls_close16
    call fat12_close
    jmp .ls_apply
.ls_close16:
    call fat12_close

.ls_apply:
    ; Verify magic byte
    cmp byte [.ls_buf], 0xA5
    jne .ls_done

    ; Apply font (validate 0-2)
    mov al, [.ls_buf + 1]
    cmp al, 3
    jae .ls_done
    call gfx_set_font                  ; AL = font index, sets all metrics

    ; Apply colors (CGA plotter naturally uses low 2 bits, no masking needed)
    mov al, [.ls_buf + 2]
    mov [text_color], al
    mov [draw_fg_color], al
    mov al, [.ls_buf + 3]
    mov [desktop_bg_color], al
    mov [draw_bg_color], al
    mov al, [.ls_buf + 4]
    mov [win_color], al

    ; Apply video mode (byte 5: 0x04=CGA, 0x13=VGA, 0x12=Mode12h, 0x01=VESA)
    mov al, [.ls_buf + 5]
    cmp al, 0x04
    je .ls_done                         ; CGA = default at boot, no switch
    cmp al, [video_mode]
    je .ls_done                         ; Already in requested mode
    ; set_video_mode handles all modes with fallback chain
    call set_video_mode

    jmp .ls_done

.ls_close:
    xor ah, ah
    mov al, [.ls_fh]
    cmp byte [boot_drive], 0x80
    jae .ls_close16b
    call fat12_close
    jmp .ls_done
.ls_close16b:
    call fat12_close

.ls_done:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.ls_filename: db 'SETTINGS.CFG', 0
.ls_fh:       db 0
.ls_buf:      times 6 db 0

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

; setup_graphics - Set CGA mode 4 (called from old code paths, e.g. floppy boot)
setup_graphics:
    xor ax, ax
    mov al, 0x04
    int 0x10
    ; Fall through to post-mode setup

; setup_graphics_post_mode - Clear screen and set palette (after mode already set)
setup_graphics_post_mode:
    cmp byte [video_mode], 0x01
    je .sgpm_vesa
    cmp byte [video_mode], 0x12
    je .sgpm_mode12h
    cmp byte [video_mode], 0x13
    je .sgpm_vga
    ; CGA: clear 16KB video memory and set palette
    push es
    mov ax, [video_segment]
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 8192
    rep stosw
    pop es
    ; Select palette 1 (cyan/magenta/white)
    mov ax, 0x0B00
    mov bx, 0x0100                  ; BH=1 (select palette), BL=0 → palette 0? No...
    mov bl, 0x01                    ; BL=1 = palette 1
    int 0x10
    ; Set background/border color to blue (index 1)
    mov ax, 0x0B00                  ; AH=0x0B (must set explicitly!)
    xor bx, bx                     ; BH=0 (set background color)
    mov bl, 0x01                    ; BL=1 (blue)
    int 0x10
    ret
.sgpm_vga:
    ; VGA: clear 64000-byte linear framebuffer
    push es
    mov ax, 0xA000
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 32000                   ; 64000 bytes / 2
    rep stosw
    pop es
    ; Set up VGA palette (16 standard colors)
    call setup_vga_palette
    ret

.sgpm_vesa:
    ; VESA: clear 307200 bytes (640*480) across banks, set palette
    push es
    push dx
    mov ax, 0xA000
    mov es, ax
    ; Clear bank 0 (64KB)
    xor ax, ax
    call vesa_set_bank
    xor di, di
    xor ax, ax
    mov cx, 32768                  ; 65536 / 2
    rep stosw
    ; Clear bank 1 (64KB)
    mov ax, 1
    call vesa_set_bank
    xor di, di
    xor ax, ax
    mov cx, 32768
    rep stosw
    ; Clear bank 2 (64KB)
    mov ax, 2
    call vesa_set_bank
    xor di, di
    xor ax, ax
    mov cx, 32768
    rep stosw
    ; Clear bank 3 (64KB)
    mov ax, 3
    call vesa_set_bank
    xor di, di
    xor ax, ax
    mov cx, 32768
    rep stosw
    ; Clear bank 4 (remainder: 307200 - 4*65536 = 44928 bytes)
    mov ax, 4
    call vesa_set_bank
    xor di, di
    xor ax, ax
    mov cx, 22464                  ; 44928 / 2
    rep stosw
    pop dx
    pop es
    ; Set VGA palette (same 32-color palette as mode 13h)
    call setup_vga_palette
    ret

.sgpm_mode12h:
    ; Mode 12h (640x480x16): clear all 4 planes, set palette
    push es
    push dx
    mov ax, 0xA000
    mov es, ax
    ; Enable all planes via Sequencer Map Mask
    mov dx, 0x3C4
    mov al, 2                      ; Index 2: Map Mask
    out dx, al
    inc dx
    mov al, 0x0F                   ; All 4 planes
    out dx, al
    ; Clear 38400 bytes (640*480/8 = 38400)
    xor di, di
    xor ax, ax
    mov cx, 19200                  ; 38400 / 2
    rep stosw
    pop dx
    pop es
    ; Set Mode 12h palette via INT 10h
    call setup_mode12h_palette
    ret

; setup_mode12h_palette - Set 16 DAC registers for Mode 12h
setup_mode12h_palette:
    push ax
    push bx
    push cx
    push dx
    push es
    push ds
    pop es                          ; ES = DS = 0x1000
    mov ax, 0x1012
    xor bx, bx                     ; Start at register 0
    mov cx, 16                      ; 16 registers
    mov dx, mode12h_palette_data
    int 0x10
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; setup_vga_palette - Set first 16 DAC registers to standard VGA colors
setup_vga_palette:
    push ax
    push bx
    push cx
    push dx
    push es
    ; INT 10h AX=1012h: Set block of DAC registers
    ; BX=start, CX=count, ES:DX → RGB triples (6-bit values)
    push ds
    pop es                          ; ES = DS = 0x1000
    mov ax, 0x1012
    xor bx, bx                     ; Start at register 0
    mov cx, 32                      ; 32 registers (16 standard + 16 system)
    mov dx, vga_palette_data        ; ES:DX → palette data
    int 0x10
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; VGA palette data (16 entries × 3 bytes RGB, 6-bit values 0-63)
; Entries 0-3 match CGA palette 1 (black/cyan/magenta/white) for compatibility
vga_palette_data:
    db  0,  0,  0                   ; 0: Black (CGA background)
    db  0, 42, 42                   ; 1: Cyan (CGA color 1)
    db 42,  0, 42                   ; 2: Magenta (CGA color 2)
    db 63, 63, 63                   ; 3: White (CGA color 3)
    db 42,  0,  0                   ; 4: Red
    db  0,  0, 42                   ; 5: Blue
    db  0, 42,  0                   ; 6: Green
    db 42, 42, 42                   ; 7: Light Gray
    db 21, 21, 21                   ; 8: Dark Gray
    db 21, 21, 63                   ; 9: Light Blue
    db 21, 63, 21                   ; 10: Light Green
    db 21, 63, 63                   ; 11: Light Cyan
    db 63, 21, 21                   ; 12: Light Red
    db 63, 21, 63                   ; 13: Light Magenta
    db 63, 63, 21                   ; 14: Yellow
    db 63, 63, 63                   ; 15: Bright White (cursor + VGA standard)
    ; System widget colors (16-31) for 3D UI chrome
    db 48, 48, 48                   ; 16: Button Face (light gray)
    db 63, 63, 63                   ; 17: Button Highlight (white)
    db 32, 32, 32                   ; 18: Button Shadow (dark gray)
    db 16, 16, 16                   ; 19: Dark Shadow
    db  0,  0, 32                   ; 20: Active Title (dark blue)
    db  4, 33, 52                   ; 21: Active Title Mid (medium blue)
    db 40, 40, 40                   ; 22: Inactive Title (lighter gray)
    db  0, 32, 32                   ; 23: Desktop Teal
    db 52, 52, 52                   ; 24: Selection highlight
    db 56, 56, 56                   ; 25: Light button face
    db  8, 16, 40                   ; 26: Title gradient step
    db 63, 63,  0                   ; 27: Bright yellow
    db  0, 32,  0                   ; 28: Dark green
    db 32,  0,  0                   ; 29: Dark red
    db 42, 42, 63                   ; 30: Light blue
    db  8,  8,  8                   ; 31: Near black

; System palette color constants
SYS_BTN_FACE     equ 16
SYS_BTN_HILIGHT  equ 17
SYS_BTN_SHADOW   equ 18
SYS_BTN_DKSHADOW equ 19
SYS_TITLE_ACTIVE equ 20
SYS_TITLE_MID    equ 21
SYS_TITLE_INACT  equ 22
SYS_DESKTOP      equ 23

; Mode 12h (640x480x16) color translation table
; Maps 256 system color indices to 4-bit EGA/VGA attribute values
; Only first 32 matter (0-15 = standard, 16-31 = system widget colors)
mode12h_color_map:
    db  0,  3,  5, 15              ; 0=black, 1=cyan, 2=magenta, 3=white
    db  4,  1,  2,  7              ; 4=red, 5=blue, 6=green, 7=light gray
    db  8,  9, 10, 11              ; 8=dark gray, 9=light blue, 10=light green, 11=light cyan
    db 12, 13, 14,  6              ; 12=light red, 13=light magenta, 14=yellow, 15=brown
    db  7, 15,  8,  0              ; 16=btn face→7, 17=highlight→15, 18=shadow→8, 19=dk shadow→0
    db  1,  9,  8,  3              ; 20=title→1, 21=title mid→9, 22=inactive→8, 23=teal→3
    db  7,  7,  1, 14              ; 24-27: reserved mappings
    db  2,  4, 11,  8              ; 28-31: reserved mappings

; Mode 12h palette: 16 DAC registers set via INT 10h AX=1012h
; Matched to approximate the 256-color system palette
mode12h_palette_data:
    db  0,  0,  0                   ; 0: Black
    db  0,  0, 32                   ; 1: Dark Blue (title active)
    db  0, 42,  0                   ; 2: Green
    db  0, 42, 42                   ; 3: Cyan / Teal
    db 42,  0,  0                   ; 4: Red
    db 42,  0, 42                   ; 5: Magenta
    db 42, 21,  0                   ; 6: Brown
    db 48, 48, 48                   ; 7: Light Gray (button face)
    db 32, 32, 32                   ; 8: Dark Gray (button shadow)
    db 21, 21, 63                   ; 9: Light Blue (title mid)
    db 21, 63, 21                   ; 10: Light Green
    db 21, 63, 63                   ; 11: Light Cyan
    db 63, 21, 21                   ; 12: Light Red
    db 63, 21, 63                   ; 13: Light Magenta
    db 63, 63, 21                   ; 14: Yellow
    db 63, 63, 63                   ; 15: White (button highlight)

; ============================================================================
; Mode 12h planar graphics helpers
; ============================================================================

; mode12h_plot_pixel - Plot single pixel in Mode 12h planar memory
; Input: CX=X, BX=Y, DL=color (0-255, mapped to 4-bit), ES=0xA000
; Preserves all registers
mode12h_plot_pixel:
    push ax
    push bx
    push cx
    push dx
    push di

    ; Map color through translation table
    push bx
    movzx bx, dl
    mov al, [mode12h_color_map + bx]
    pop bx
    mov [cs:.m12pp_color], al

    ; Calculate byte offset: Y * 80 + X / 8
    mov ax, bx                     ; AX = Y
    push dx
    mov di, 80
    mul di                          ; AX = Y * 80
    pop dx
    mov di, ax
    mov ax, cx
    shr ax, 3                      ; AX = X / 8
    add di, ax                     ; DI = Y*80 + X/8

    ; Calculate bit mask: 0x80 >> (X & 7)
    mov ax, cx
    and al, 7
    mov cl, al
    mov ah, 0x80
    shr ah, cl                     ; AH = bit mask

    ; GC: Set/Reset = color
    mov dx, 0x3CE
    xor al, al                     ; Index 0: Set/Reset
    out dx, al
    inc dx
    mov al, [cs:.m12pp_color]
    out dx, al

    ; GC: Enable Set/Reset = 0x0F
    dec dx
    mov al, 1                      ; Index 1
    out dx, al
    inc dx
    mov al, 0x0F
    out dx, al

    ; GC: Bit Mask = pixel mask
    dec dx
    mov al, 8                      ; Index 8
    out dx, al
    inc dx
    mov al, ah                     ; Bit mask
    out dx, al

    ; Read-modify-write
    mov al, [es:di]                ; Read (loads latches)
    mov [es:di], al                ; Write (Set/Reset provides color)

    ; Reset: Bit Mask = 0xFF, Enable Set/Reset = 0
    dec dx
    mov al, 8
    out dx, al
    inc dx
    mov al, 0xFF
    out dx, al
    dec dx
    mov al, 1
    out dx, al
    inc dx
    xor al, al
    out dx, al

    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret
.m12pp_color: db 0

; mode12h_xor_pixel - XOR a single pixel in Mode 12h
; Input: CX=X, BX=Y, ES=0xA000
; Preserves all registers
mode12h_xor_pixel:
    push ax
    push bx
    push cx
    push dx
    push di

    ; Calculate byte offset: Y * 80 + X / 8
    mov ax, bx                     ; AX = Y
    push dx
    mov di, 80
    mul di
    pop dx
    mov di, ax
    mov ax, cx
    shr ax, 3
    add di, ax                     ; DI = Y*80 + X/8

    ; Bit mask: 0x80 >> (X & 7)
    mov ax, cx
    and al, 7
    mov cl, al
    mov ah, 0x80
    shr ah, cl                     ; AH = bit mask

    ; GC: Data Rotate = XOR mode (0x18)
    mov dx, 0x3CE
    mov al, 3                      ; Index 3: Data Rotate / Function Select
    out dx, al
    inc dx
    mov al, 0x18                   ; Function = XOR (11b << 3)
    out dx, al

    ; GC: Set/Reset = 0x0F (all planes)
    dec dx
    xor al, al                     ; Index 0
    out dx, al
    inc dx
    mov al, 0x0F
    out dx, al

    ; GC: Enable Set/Reset = 0x0F
    dec dx
    mov al, 1
    out dx, al
    inc dx
    mov al, 0x0F
    out dx, al

    ; GC: Bit Mask = pixel mask
    dec dx
    mov al, 8
    out dx, al
    inc dx
    mov al, ah
    out dx, al

    ; Read-modify-write (XOR with all planes)
    mov al, [es:di]                ; Read (loads latches)
    mov [es:di], al                ; Write (XOR applied by GC)

    ; Reset GC state
    dec dx
    mov al, 8                      ; Bit Mask = 0xFF
    out dx, al
    inc dx
    mov al, 0xFF
    out dx, al
    dec dx
    mov al, 3                      ; Data Rotate = 0 (normal)
    out dx, al
    inc dx
    xor al, al
    out dx, al
    dec dx
    mov al, 1                      ; Enable Set/Reset = 0
    out dx, al
    inc dx
    xor al, al
    out dx, al

    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; mode12h_fill_rect - Fill rectangle in Mode 12h (640x480x16) planar memory
; Uses Write Mode 0 + Set/Reset (same technique as mode12h_plot_pixel).
; Input: BX=X, CX=Y, DX=width, SI=height, AL=color (0-31), ES=0xA000
; Preserves all registers
mode12h_fill_rect:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Map color through translation table
    push bx
    movzx bx, al
    mov al, [mode12h_color_map + bx]
    pop bx
    mov [cs:.m12_color], al

    ; Calculate left/right byte positions
    mov ax, bx                     ; AX = X
    shr ax, 3                      ; AX = X / 8 = left byte
    mov [cs:.m12_left_byte], ax
    mov ax, bx
    add ax, dx
    dec ax                         ; AX = X + W - 1
    shr ax, 3                      ; AX = right byte
    mov [cs:.m12_right_byte], ax

    ; Left mask: pixels from (X%8) to 7
    push cx                        ; Save Y (CL needed for shifts)
    mov ax, bx
    and ax, 7
    mov cl, al
    mov al, 0xFF
    shr al, cl
    mov [cs:.m12_left_mask], al

    ; Right mask: 0xFF << (7 - ((X+W-1)%8))
    mov ax, bx
    add ax, dx
    dec ax
    and ax, 7
    mov cl, 7
    sub cl, al
    mov al, 0xFF
    shl al, cl
    mov [cs:.m12_right_mask], al
    pop cx                         ; Restore Y

    mov bp, si                     ; BP = height counter
    mov ax, cx                     ; AX = Y (start row)

    ; === Set up GC registers ONCE (Write Mode 0 + Set/Reset) ===
    mov dx, 0x3CE
    ; GC index 0: Set/Reset = color
    push ax
    xor al, al
    out dx, al
    inc dx
    mov al, [cs:.m12_color]
    out dx, al
    ; GC index 1: Enable Set/Reset = 0x0F (all planes)
    dec dx
    mov al, 1
    out dx, al
    inc dx
    mov al, 0x0F
    out dx, al
    ; GC index 3: Data Rotate = 0 (replace)
    dec dx
    mov al, 3
    out dx, al
    inc dx
    xor al, al
    out dx, al
    pop ax

.m12_row:
    push ax                        ; Save current Y

    ; Calculate row base: DI = Y * 80
    push dx
    mov di, 80
    mul di                          ; DX:AX = Y * 80
    pop dx
    mov di, ax                     ; DI = row base

    ; Check single-byte case
    mov ax, [cs:.m12_left_byte]
    cmp ax, [cs:.m12_right_byte]
    je .m12_single

    ; --- Left partial byte ---
    add di, ax
    mov dx, 0x3CE
    mov al, 8                      ; Bit Mask register
    out dx, al
    inc dx
    mov al, [cs:.m12_left_mask]
    out dx, al
    mov al, [es:di]                ; Read (loads latches)
    mov byte [es:di], 0            ; Write (Set/Reset provides color, value ignored)
    inc di

    ; --- Middle full bytes ---
    mov cx, [cs:.m12_right_byte]
    sub cx, [cs:.m12_left_byte]
    dec cx
    jle .m12_right

    ; Bit Mask = 0xFF
    dec dx
    mov al, 8
    out dx, al
    inc dx
    mov al, 0xFF
    out dx, al

    xor al, al
    rep stosb                      ; Write CX bytes (Set/Reset provides color)

.m12_right:
    ; --- Right partial byte ---
    mov dx, 0x3CE
    mov al, 8
    out dx, al
    inc dx
    mov al, [cs:.m12_right_mask]
    out dx, al
    mov al, [es:di]                ; Read (loads latches)
    mov byte [es:di], 0            ; Write (Set/Reset provides color)
    jmp .m12_row_cleanup

.m12_single:
    ; Single byte case
    add di, ax
    mov al, [cs:.m12_left_mask]
    and al, [cs:.m12_right_mask]
    mov dx, 0x3CE
    push ax
    mov al, 8
    out dx, al
    inc dx
    pop ax
    out dx, al
    mov al, [es:di]                ; Read (loads latches)
    mov byte [es:di], 0            ; Write (Set/Reset provides color)

.m12_row_cleanup:
    pop ax                         ; Restore Y
    inc ax                         ; Next row
    dec bp
    jnz .m12_row

    ; === Reset GC registers ONCE at end ===
    mov dx, 0x3CE
    mov al, 8                      ; Bit Mask = 0xFF
    out dx, al
    inc dx
    mov al, 0xFF
    out dx, al
    dec dx
    mov al, 1                      ; Enable Set/Reset = 0
    out dx, al
    inc dx
    xor al, al
    out dx, al

    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.m12_color:       db 0
.m12_left_byte:   dw 0
.m12_right_byte:  dw 0
.m12_left_mask:   db 0
.m12_right_mask:  db 0

; ============================================================================
; VESA helpers (640x480x256, mode 0x101, banked)
; ============================================================================

; vesa_set_bank - Switch VESA display bank
; Input: AX = bank number (in granularity units)
; Preserves all registers except flags
vesa_set_bank:
    cmp ax, [vesa_cur_bank]
    je .vsb_done
    mov [vesa_cur_bank], ax
    push ax
    push bx
    push dx
    mov dx, ax                     ; DX = bank number
    xor bx, bx                    ; BH=0, BL=0 (window A)
    mov ax, 0x4F05                 ; VESA set window
    int 0x10
    pop dx
    pop bx
    pop ax
.vsb_done:
    ret

; vesa_plot_pixel - Plot pixel in VESA 640x480x256 banked mode
; Input: CX=X, BX=Y, DL=color, ES=0xA000
; Preserves all registers
vesa_plot_pixel:
    push ax
    push bx
    push di
    push dx

    ; Linear offset = Y * 640 + X → DX:AX (32-bit)
    mov ax, bx                     ; AX = Y
    push dx                        ; Save color
    mov dx, 640
    mul dx                          ; DX:AX = Y * 640
    add ax, cx                     ; Add X
    adc dx, 0                      ; Propagate carry to high word
    ; DX = bank (64KB unit), AX = offset within bank
    ; Need to convert to granularity units: bank = DX * 64 / granularity
    ; For most cards, granularity = 64KB, so DX is the bank number directly
    mov di, ax                     ; DI = offset within bank
    mov ax, dx                     ; AX = high word (bank)
    pop dx                         ; Restore DL = color
    push dx                        ; Save DL again
    call vesa_set_bank
    pop dx                         ; Restore DL = color
    mov [es:di], dl
    pop dx
    pop di
    pop bx
    pop ax
    ret

; vesa_read_pixel - Read one pixel from VESA banked framebuffer
; Input: CX=X, BX=Y, ES=0xA000
; Output: AL=color
; Preserves: BX, CX, DI
vesa_read_pixel:
    push bx
    push di
    push dx
    ; Linear offset = Y * 640 + X
    mov ax, bx                     ; AX = Y
    mov di, 640
    mul di                         ; DX:AX = Y * 640
    add ax, cx
    adc dx, 0
    ; DX = bank, AX = offset within bank
    mov di, ax
    mov ax, dx
    call vesa_set_bank
    mov al, [es:di]                ; Read pixel
    pop dx
    pop di
    pop bx
    ret

; vesa_fill_rect - Fill rectangle in VESA 640x480x256
; Input: BX=X, CX=Y, DX=width, SI=height, AL=color
;        ES=0xA000
; Preserves all registers
vesa_fill_rect:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov [cs:.vf_color], al
    mov [cs:.vf_x], bx
    mov [cs:.vf_w], dx
    mov bp, si                     ; BP = height counter

.vf_row:
    ; Calculate linear offset for start of row: Y * 640 + X
    mov ax, cx                     ; AX = current Y
    mov dx, 640
    mul dx                          ; DX:AX = Y * 640
    add ax, [cs:.vf_x]
    adc dx, 0                      ; DX:AX = linear start offset

    ; How many bytes left in current bank from this offset?
    mov di, ax                     ; DI = offset in bank window
    push dx                        ; Save bank number
    push cx                        ; Save Y

    mov ax, dx                     ; AX = bank number
    call vesa_set_bank

    ; Fill this row: may need to cross a bank boundary
    mov si, [cs:.vf_w]            ; SI = remaining width
    mov al, [cs:.vf_color]

    ; Check if row crosses 64K boundary
    ; Bytes until boundary = 0x10000 - DI
    push dx
    mov dx, 0
    sub dx, di                     ; DX = bytes until boundary (0x10000 - DI)
    cmp si, dx                     ; width <= remaining?
    jbe .vf_no_cross
    ; Row crosses bank boundary
    mov cx, dx                     ; CX = bytes before boundary
    rep stosb                      ; Fill first part
    ; Switch to next bank
    mov ax, [vesa_cur_bank]
    inc ax
    call vesa_set_bank
    xor di, di                     ; DI = 0 (start of new bank)
    sub si, dx                     ; SI = remaining bytes
    mov cx, si
    mov al, [cs:.vf_color]
    rep stosb
    pop dx
    jmp .vf_row_done

.vf_no_cross:
    mov cx, si
    rep stosb
    pop dx

.vf_row_done:
    pop cx                         ; Restore Y
    pop dx                         ; Restore (unused)
    inc cx                          ; Next row
    dec bp
    jnz .vf_row

    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.vf_color: db 0
.vf_x:     dw 0
.vf_w:     dw 0

; ============================================================================
; Welcome Box - White bordered rectangle with text
; ============================================================================

draw_welcome_box:
    push es

    ; Set ES to CGA video memory
    mov ax, [video_segment]
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
; Input: SI = pointer to character bitmap
;        draw_x, draw_y = top-left position
;        draw_font_height, draw_font_width, draw_font_advance must be set
; Modifies: draw_x (advances by draw_font_advance)
; ============================================================================

draw_char:
    pusha

    xor bx, bx
    mov bl, [draw_font_height]      ; BX = font height (BH zeroed above)
    mov bp, bx                      ; BP = row counter
    mov bx, [draw_y]                ; BX = current Y

.row_loop:
    lodsb                           ; Get row bitmap into AL (from DS:SI)
    mov ah, al                      ; AH = bitmap for this row
    mov cx, [draw_x]                ; CX = current X
    xor dx, dx
    mov dl, [draw_font_width]       ; DX = glyph column counter

.col_loop:
    test ah, 0x80                   ; Check leftmost bit
    jz .clear_pixel
    call plot_pixel_white           ; "1" bit: foreground color
    jmp .next_pixel
.clear_pixel:
    cmp byte [draw_bg_color], 0
    jne .bg_color_pixel
    call plot_pixel_black           ; Fast path: black background
    jmp .next_pixel
.bg_color_pixel:
    push dx
    mov dl, [draw_bg_color]
    call plot_pixel_color
    pop dx
.next_pixel:
    shl ah, 1                       ; Next bit
    inc cx                          ; Next X
    dec dx                          ; Decrement glyph column counter
    jnz .col_loop

    ; Fill gap pixels (advance - width) with background color
    push dx
    xor dx, dx
    mov dl, [draw_font_advance]
    sub dl, [draw_font_width]
    jz .no_gap
    cmp byte [draw_bg_color], 0
    jne .gap_color
.gap_black:
    call plot_pixel_black
    inc cx
    dec dl
    jnz .gap_black
    jmp .no_gap
.gap_color:
    push dx
    mov dl, [draw_bg_color]
    call plot_pixel_color
    pop dx
    inc cx
    dec dl
    jnz .gap_color
.no_gap:
    pop dx

    inc bx                          ; Next Y
    dec bp                          ; Decrement row counter
    jnz .row_loop

    xor ax, ax
    mov al, [draw_font_advance]
    add [draw_x], ax                ; Advance to next character position

    popa
    ret

; ============================================================================
; Plot a white pixel (color 3)
; Input: CX = X coordinate (0-319), BX = Y coordinate (0-199)
; Preserves all registers except flags
; ============================================================================

; ============================================================================
; cga_pixel_calc - Calculate CGA byte offset and bit shift for a pixel
; Input: CX = X coordinate (0-319), BX = Y coordinate (0-199)
;        ES must be video segment
; Output: DI = CGA byte offset in video memory
;         CL = bit shift for 2-bit pixel within byte
; Trashes: AX, BX, DX (caller must save these registers)
; ============================================================================
cga_pixel_calc:
    mov ax, bx
    shr ax, 1                       ; AX = Y / 2
    mov dx, 80
    mul dx                          ; AX = (Y/2) * 80
    mov di, ax
    mov ax, cx
    shr ax, 1
    shr ax, 1                       ; AX = X / 4
    add di, ax
    test bl, 1                      ; Odd scanline?
    jz .even
    add di, 0x2000                  ; Odd row: +8K interlace offset
.even:
    mov ax, cx
    and ax, 3
    mov cx, 3
    sub cl, al
    shl cl, 1                       ; CL = (3 - (X mod 4)) * 2
    ret

plot_pixel_white:
    cmp cx, [cs:screen_width]
    jae .out
    cmp bx, [cs:screen_height]
    jae .out
    cmp byte [cs:video_mode], 0x01
    je .vesa
    cmp byte [cs:video_mode], 0x12
    je .mode12h
    cmp byte [cs:video_mode], 0x13
    je .vga
    push ax
    push bx
    push cx
    push di
    push dx
    call cga_pixel_calc
    mov al, [es:di]
    mov ah, [cs:draw_fg_color]
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
.out:
    ret
.vga:
    push ax
    push dx
    push di
    mov ax, bx
    mul word [cs:screen_pitch]         ; AX = Y * pitch (DX:AX, DX ignored for 16-bit)
    add ax, cx                         ; AX = Y * pitch + X
    mov di, ax
    mov al, [cs:draw_fg_color]
    mov [es:di], al
    pop di
    pop dx
    pop ax
    ret
.mode12h:
    push dx
    mov dl, [cs:draw_fg_color]
    call mode12h_plot_pixel
    pop dx
    ret
.vesa:
    push dx
    mov dl, [cs:draw_fg_color]
    call vesa_plot_pixel
    pop dx
    ret

; ============================================================================
; Plot a black pixel (color 0) - for inverted text on white backgrounds
; Input: CX = X coordinate (0-319), BX = Y coordinate (0-199)
; Preserves all registers except flags
; ============================================================================

plot_pixel_black:
    cmp cx, [cs:screen_width]
    jae .out
    cmp bx, [cs:screen_height]
    jae .out
    cmp byte [cs:video_mode], 0x01
    je .vesa
    cmp byte [cs:video_mode], 0x12
    je .mode12h
    cmp byte [cs:video_mode], 0x13
    je .vga
    push ax
    push bx
    push cx
    push di
    push dx
    call cga_pixel_calc
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
.out:
    ret
.vga:
    push ax
    push dx
    push di
    mov ax, bx
    mul word [cs:screen_pitch]
    add ax, cx
    mov di, ax
    mov byte [es:di], 0             ; Color 0 = black
    pop di
    pop dx
    pop ax
    ret
.mode12h:
    push dx
    xor dl, dl                     ; Color 0 = black
    call mode12h_plot_pixel
    pop dx
    ret
.vesa:
    push dx
    xor dl, dl
    call vesa_plot_pixel
    pop dx
    ret

; ============================================================================
; Plot a pixel using XOR with color 3 (white) - for mouse cursor
; Input: CX = X coordinate (0-319), BX = Y coordinate (0-199)
; ES must be video segment
; Preserves all registers except flags
; ============================================================================

plot_pixel_xor:
    cmp cx, [screen_width]
    jae .out
    cmp bx, [screen_height]
    jae .out
    cmp byte [video_mode], 0x01
    je .vesa
    cmp byte [video_mode], 0x12
    je .mode12h
    cmp byte [video_mode], 0x13
    je .vga
    push ax
    push bx
    push cx
    push di
    push dx
    call cga_pixel_calc
    mov al, 0x03                    ; White color bits
    shl al, cl                      ; Position to correct pixel
    xor [es:di], al                 ; XOR with screen (self-inverse)
    pop dx
    pop di
    pop cx
    pop bx
    pop ax
.out:
    ret
.vga:
    push ax
    push di
    push dx
    mov ax, bx
    mul word [screen_pitch]
    add ax, cx
    mov di, ax
    xor byte [es:di], 0xFF         ; Full byte XOR for cursor visibility
    pop dx
    pop di
    pop ax
    ret
.mode12h:
    jmp mode12h_xor_pixel              ; Tail call (preserves all regs)
.vesa:
    ; VESA XOR: compute bank, read-xor-write
    push ax
    push bx
    push di
    push dx
    mov ax, bx                     ; AX = Y
    push dx
    mov dx, 640
    mul dx                          ; DX:AX = Y * 640
    add ax, cx
    adc dx, 0
    mov di, ax
    mov ax, dx
    pop dx
    call vesa_set_bank
    xor byte [es:di], 0xFF
    pop dx
    pop di
    pop bx
    pop ax
    ret

; ============================================================================
; Mouse Cursor Sprite (XOR-based)
; ============================================================================

CURSOR_WIDTH    equ 8

; cursor_xor_sprite - Draw/erase cursor at given position via XOR
; Input: CX = cursor X, BX = cursor Y (hotspot at top-left)
; ES must be video segment
; Preserves all registers
; Color cursor: 2bpp, 14 rows, white outline + cyan fill
cursor_xor_sprite:
    pusha
    cmp byte [cs:video_mode], 0x01
    je .mode12h_cursor             ; VESA: share per-pixel XOR cursor path
    cmp byte [cs:video_mode], 0x12
    je .mode12h_cursor
    cmp byte [cs:video_mode], 0x13
    je .vga_cursor
    mov bp, 14                      ; Row counter
    mov si, cursor_bitmap_color

.row_loop:
    push cx
    cmp bx, [screen_height]
    jae .skip_row
    mov ah, [si]                    ; AH = first byte (pixels 0-3)
    mov al, [si+1]                  ; AL = second byte (pixels 4-7)
    mov di, 8
.col_loop:
    mov dl, ah
    shr dl, 6                       ; DL = pixel color (0-3)
    test dl, dl
    jz .skip_pixel
    cmp cx, [screen_width]
    jae .skip_pixel
    mov [cs:cursor_color], dl
    push ax
    push bx
    push cx
    push di
    call cga_pixel_calc             ; DI=byte offset, CL=shift
    mov al, [cs:cursor_color]
    shl al, cl
    xor [es:di], al
    pop di
    pop cx
    pop bx
    pop ax
.skip_pixel:
    shl ax, 2
    inc cx
    dec di
    jnz .col_loop
.skip_row:
    pop cx
    add si, 2
    inc bx
    dec bp
    jnz .row_loop

    popa
    ret

.vga_cursor:
    mov bp, 14
    mov si, cursor_bitmap_color
.vc_row:
    push cx                         ; Save X start
    cmp bx, [screen_height]
    jae .vc_skip_row
    mov ah, [si]                    ; Pixels 0-3 (2bpp)
    mov al, [si+1]                  ; Pixels 4-7 (2bpp)
    mov di, 8                       ; 8 pixels per row
.vc_col:
    mov dl, ah
    shr dl, 6                       ; DL = pixel color (0-3)
    test dl, dl
    jz .vc_skip_pix
    cmp cx, [screen_width]
    jae .vc_skip_pix
    ; Calculate VGA offset for (CX=X, BX=Y)
    push ax
    push di
    push dx
    mov ax, bx
    mul word [screen_pitch]
    add ax, cx
    mov di, ax
    xor byte [es:di], 0xFF         ; XOR for cursor visibility
    pop dx
    pop di
    pop ax
.vc_skip_pix:
    shl ax, 2                       ; Next 2bpp pixel
    inc cx
    dec di
    jnz .vc_col
.vc_skip_row:
    pop cx                          ; Restore X start
    add si, 2
    inc bx
    dec bp
    jnz .vc_row
    popa
    ret

.mode12h_cursor:
    ; Mode 12h/VESA cursor: per-pixel XOR, 2x scaled in 640x480
    mov bp, 14
    mov si, cursor_bitmap_color
.m12c_row:
    push cx                         ; Save X start
    cmp bx, [screen_height]
    jae .m12c_skip_row
    mov ah, [si]
    mov al, [si+1]
    mov di, 8
.m12c_col:
    mov dl, ah
    shr dl, 6
    test dl, dl
    jz .m12c_skip_pix
    cmp cx, [screen_width]
    jae .m12c_skip_pix
    call plot_pixel_xor             ; Top-left
    cmp word [screen_width], 640
    jb .m12c_skip_pix
    inc cx
    call plot_pixel_xor             ; Top-right
    dec cx
    inc bx
    call plot_pixel_xor             ; Bottom-left
    inc cx
    call plot_pixel_xor             ; Bottom-right
    dec cx
    dec bx
.m12c_skip_pix:
    shl ax, 2
    inc cx
    cmp word [screen_width], 640
    jb .m12c_noscale_x
    inc cx                          ; Extra advance for 2x
.m12c_noscale_x:
    dec di
    jnz .m12c_col
.m12c_skip_row:
    pop cx
    add si, 2
    inc bx
    cmp word [screen_width], 640
    jb .m12c_noscale_y
    inc bx                          ; Extra row advance for 2x
.m12c_noscale_y:
    dec bp
    jnz .m12c_row
    popa
    ret

; ============================================================================
; Solid White Cursor (VGA/VESA) — save background, draw white, restore
; ============================================================================

; cursor_save_and_draw_vga - Save background pixels and draw solid white cursor
; Input: CX=X, BX=Y, ES=0xA000 (VGA linear framebuffer)
; Uses cursor_save_buf (8 bytes × 14 rows = 112 bytes)
cursor_save_and_draw_vga:
    pusha
    mov di, cursor_save_buf         ; DI = save buffer pointer
    mov si, cursor_bitmap_color
    mov bp, 14                      ; 14 rows
.sdv_row:
    push cx                         ; Save X start
    cmp bx, [screen_height]
    jae .sdv_skip_row
    ; Get bitmap data for this row
    mov dh, [si]                    ; Pixels 0-3 (2bpp)
    mov dl, [si+1]                  ; Pixels 4-7 (2bpp)
    ; Calculate VRAM row base offset
    push si
    mov ax, bx
    push dx
    mul word [screen_pitch]         ; AX = Y * pitch
    pop dx
    mov si, ax                      ; SI = VRAM row base
    ; Process 8 pixels
    push bp
    mov bp, 8
.sdv_col:
    cmp cx, [screen_width]
    jae .sdv_skip_pix
    ; Save current pixel from VRAM
    push si
    add si, cx
    mov al, [es:si]                 ; Read VRAM pixel
    mov [di], al                    ; Save to buffer
    ; Check bitmap bit — if non-transparent, draw white
    mov al, dh
    shr al, 6
    test al, al
    jz .sdv_no_draw
    mov byte [es:si], 0x0F         ; Write white
.sdv_no_draw:
    pop si
.sdv_skip_pix:
    shl dx, 2                      ; Next bitmap pixel
    inc cx
    inc di                          ; Always advance buffer
    dec bp
    jnz .sdv_col
    pop bp
    pop si                          ; Restore bitmap pointer
.sdv_skip_row:
    pop cx                          ; Restore X start
    add si, 2                       ; Next bitmap row
    inc bx
    dec bp
    jnz .sdv_row
    popa
    ret

; cursor_restore_vga - Restore background pixels (erase cursor)
; Input: CX=X, BX=Y, ES=0xA000
cursor_restore_vga:
    pusha
    mov si, cursor_save_buf         ; SI = saved pixels
    mov bp, 14
.rv_row:
    push cx
    cmp bx, [screen_height]
    jae .rv_skip_row
    ; Calculate VRAM row base
    mov ax, bx
    push dx
    mul word [screen_pitch]
    pop dx
    mov di, ax                      ; DI = VRAM row base
    ; Restore 8 pixels
    push bp
    mov bp, 8
.rv_col:
    cmp cx, [screen_width]
    jae .rv_skip_pix
    push di
    add di, cx
    mov al, [si]
    mov [es:di], al                 ; Write saved pixel to VRAM
    pop di
.rv_skip_pix:
    inc cx
    inc si
    dec bp
    jnz .rv_col
    pop bp
    jmp .rv_row_end
.rv_skip_row:
    add si, 8                       ; Skip 8 bytes in save buffer
.rv_row_end:
    pop cx
    inc bx
    dec bp
    jnz .rv_row
    popa
    ret

; cursor_save_and_draw_vesa - Save background and draw 2x-scaled white cursor
; Input: CX=X, BX=Y, ES=0xA000 (VESA banked framebuffer)
; Uses cursor_save_buf (16 bytes × 28 rows = 448 bytes)
cursor_save_and_draw_vesa:
    pusha
    mov word [_csr_buf_ptr], cursor_save_buf
    mov si, cursor_bitmap_color
    mov bp, 14                      ; 14 bitmap rows
.sdvs_bmp_row:
    ; Each bitmap row → 2 screen rows
    mov ah, [si]
    mov al, [si+1]
    mov [_csr_bmp_16], ax           ; Save bitmap for reuse
    ; Screen row 1 (top half of 2x)
    push cx
    cmp bx, [screen_height]
    jae .sdvs_skip_top
    call .sdvs_scan_line
    jmp .sdvs_top_done
.sdvs_skip_top:
    add word [_csr_buf_ptr], 16     ; Skip 16 save slots
.sdvs_top_done:
    pop cx
    inc bx
    ; Screen row 2 (bottom half of 2x)
    mov ax, [_csr_bmp_16]           ; Reload bitmap (consumed by shift)
    push cx
    cmp bx, [screen_height]
    jae .sdvs_skip_bot
    call .sdvs_scan_line
    jmp .sdvs_bot_done
.sdvs_skip_bot:
    add word [_csr_buf_ptr], 16
.sdvs_bot_done:
    pop cx
    add si, 2                       ; Next bitmap row
    inc bx
    dec bp
    jnz .sdvs_bmp_row
    popa
    ret

; .sdvs_scan_line - Process one screen row: save 16 pixels + draw white
; Input: AH/AL=bitmap, CX=X, BX=Y, _csr_buf_ptr=buffer pos
; Modifies: AX (shifted), _csr_buf_ptr (advanced by 16)
.sdvs_scan_line:
    push bp
    push di
    mov bp, 8                       ; 8 bitmap pixels → 16 screen pixels
.sdvs_sl_col:
    ; Extract pixel value before any clobbering
    mov dl, ah
    shr dl, 6                       ; DL = pixel value (0=transparent)
    shl ax, 2                       ; Advance bitmap (do it now, before AX clobbered)
    push ax                         ; Save shifted bitmap on stack
    ; --- Left pixel ---
    cmp cx, [screen_width]
    jae .sdvs_left_off
    call vesa_read_pixel            ; AL = bg color, BX/CX preserved
    mov di, [_csr_buf_ptr]
    mov [di], al
    test dl, dl
    jz .sdvs_left_nodraw
    push dx
    mov dl, 0x0F                    ; White
    call vesa_plot_pixel
    pop dx
.sdvs_left_nodraw:
    inc word [_csr_buf_ptr]
    inc cx
    jmp .sdvs_do_right
.sdvs_left_off:
    inc word [_csr_buf_ptr]
    inc cx
.sdvs_do_right:
    ; --- Right pixel ---
    cmp cx, [screen_width]
    jae .sdvs_right_off
    call vesa_read_pixel
    mov di, [_csr_buf_ptr]
    mov [di], al
    test dl, dl
    jz .sdvs_right_nodraw
    push dx
    mov dl, 0x0F
    call vesa_plot_pixel
    pop dx
.sdvs_right_nodraw:
    inc word [_csr_buf_ptr]
    inc cx
    jmp .sdvs_sl_next
.sdvs_right_off:
    inc word [_csr_buf_ptr]
    inc cx
.sdvs_sl_next:
    pop ax                          ; Restore shifted bitmap
    dec bp
    jnz .sdvs_sl_col
    pop di
    pop bp
    ret

; cursor_restore_vesa - Restore background pixels (erase 2x-scaled cursor)
; Input: CX=X, BX=Y, ES=0xA000
cursor_restore_vesa:
    pusha
    mov si, cursor_save_buf
    mov word [_csr_buf_ptr], 0      ; Row counter
.rvs_row:
    cmp word [_csr_buf_ptr], 28
    jae .rvs_done
    push cx
    cmp bx, [screen_height]
    jae .rvs_skip_row
    push bp
    mov bp, 16                      ; 16 pixels per row
.rvs_col:
    cmp cx, [screen_width]
    jae .rvs_skip_pix
    mov dl, [si]                    ; Color from save buffer
    call vesa_plot_pixel            ; BX/CX preserved
.rvs_skip_pix:
    inc cx
    inc si
    dec bp
    jnz .rvs_col
    pop bp
    jmp .rvs_row_end
.rvs_skip_row:
    add si, 16                      ; Skip 16 bytes in save buffer
.rvs_row_end:
    pop cx
    inc bx
    inc word [_csr_buf_ptr]
    jmp .rvs_row
.rvs_done:
    popa
    ret

; mouse_cursor_hide - Erase cursor if currently visible
; Safe to call even if not visible (no-op)
; Preserves all registers
; IMPORTANT: Uses PUSHF/CLI/POPF to prevent IRQ12 from interleaving
; cursor operations, which would cause cursor ghost artifacts.
mouse_cursor_hide:
    pushf                           ; Save interrupt state
    cli                             ; Atomic: check + erase + flag update
    cmp byte [cursor_locked], 0
    jne .mch_skip                   ; Skip if locked (counter > 0)
    cmp byte [cursor_visible], 0
    je .mch_skip
    push es
    push cx
    push bx
    push ax
    mov ax, [video_segment]
    mov es, ax
    mov cx, [cursor_drawn_x]
    mov bx, [cursor_drawn_y]
    ; Dispatch: VGA/VESA use save/restore, CGA/Mode12h use XOR
    cmp byte [video_mode], 0x13
    je .mch_restore_vga
    cmp byte [video_mode], 0x01
    je .mch_restore_vesa
    call cursor_xor_sprite          ; CGA/Mode12h: XOR erase
    jmp .mch_done
.mch_restore_vga:
    call cursor_restore_vga
    jmp .mch_done
.mch_restore_vesa:
    call cursor_restore_vesa
.mch_done:
    mov byte [cursor_visible], 0
    pop ax
    pop bx
    pop cx
    pop es
.mch_skip:
    popf                            ; Restore interrupt state
    ret

; mouse_cursor_show - Draw cursor at current mouse position
; Preserves all registers
; IMPORTANT: Uses PUSHF/CLI/POPF to prevent IRQ12 from interleaving
; cursor operations, which would cause cursor ghost artifacts.
mouse_cursor_show:
    pushf                           ; Save interrupt state
    cli                             ; Atomic: check + draw + flag update
    cmp byte [cursor_locked], 0
    jne .mcs_skip                   ; Skip if locked (counter > 0)
    cmp byte [mouse_enabled], 0
    je .mcs_skip
    cmp byte [cursor_visible], 1
    je .mcs_skip                    ; Already drawn, skip
    push es
    push cx
    push bx
    push ax
    mov ax, [video_segment]
    mov es, ax
    mov cx, [mouse_x]
    mov bx, [mouse_y]
    mov [cursor_drawn_x], cx
    mov [cursor_drawn_y], bx
    ; Dispatch: VGA/VESA use save/restore, CGA/Mode12h use XOR
    cmp byte [video_mode], 0x13
    je .mcs_save_draw_vga
    cmp byte [video_mode], 0x01
    je .mcs_save_draw_vesa
    call cursor_xor_sprite          ; CGA/Mode12h: XOR draw
    jmp .mcs_done
.mcs_save_draw_vga:
    call cursor_save_and_draw_vga
    jmp .mcs_done
.mcs_save_draw_vesa:
    call cursor_save_and_draw_vesa
.mcs_done:
    mov byte [cursor_visible], 1
    pop ax
    pop bx
    pop cx
    pop es
.mcs_skip:
    popf                            ; Restore interrupt state
    ret

; ============================================================================
; Window Title Bar Hit Testing
; ============================================================================

; mouse_hittest_titlebar - Test if mouse position hits any window title bar
; Input: mouse_x, mouse_y (global vars)
; Output: CF=0 hit, AL=window handle; CF=1 no hit
mouse_hittest_titlebar:
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov cx, [mouse_x]
    mov dx, [mouse_y]

    ; Step 1: Find topmost window (highest z-order) whose FULL AREA contains click
    xor si, si                      ; SI = window index
    mov di, window_table
    mov bp, 0xFFFF                  ; BP = best handle (0xFFFF = none)
    mov bl, 0                       ; BL = best z-order so far

.find_topmost:
    cmp si, WIN_MAX_COUNT
    jae .found_topmost

    cmp byte [di + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .ft_next

    ; Check X: window_x <= mouse_x < window_x + width
    mov ax, [di + WIN_OFF_X]
    cmp cx, ax
    jb .ft_next
    add ax, [di + WIN_OFF_WIDTH]
    cmp cx, ax
    jae .ft_next

    ; Check Y: window_y <= mouse_y < window_y + height (FULL window)
    mov ax, [di + WIN_OFF_Y]
    cmp dx, ax
    jb .ft_next
    add ax, [di + WIN_OFF_HEIGHT]
    cmp dx, ax
    jae .ft_next

    ; Point is inside this window - check z-order
    mov al, [di + WIN_OFF_ZORDER]
    cmp bp, 0xFFFF                  ; First hit?
    je .ft_new_best
    cmp al, bl
    jbe .ft_next

.ft_new_best:
    mov bp, si                      ; BP = topmost window handle
    mov bl, al                      ; BL = its z-order

.ft_next:
    add di, WIN_ENTRY_SIZE
    inc si
    jmp .find_topmost

.found_topmost:
    cmp bp, 0xFFFF
    je .no_hit                      ; Click not inside any window

    ; Step 2: Check if click is in that window's TITLE BAR specifically
    mov ax, bp
    shl ax, 5
    add ax, window_table
    mov di, ax

    ; Skip titlebar check for frameless windows
    test byte [di + WIN_OFF_FLAGS], WIN_FLAG_TITLE
    jz .no_hit

    mov ax, [di + WIN_OFF_Y]
    add ax, [titlebar_height]
    cmp dx, ax                      ; mouse_y < window_y + titlebar_height?
    jae .no_hit                     ; Click is in body, not title bar

    ; Click is in the topmost window's title bar
    mov ax, bp
    clc
    jmp .done

.no_hit:
    stc

.done:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; ============================================================================
; Mouse Resize Handle Hit-Test
; ============================================================================

; mouse_hittest_resize - Test if mouse position hits any window's resize handle
; Input: mouse_x, mouse_y (global vars)
; Output: CF=0 hit, AL=window handle; CF=1 no hit
; Resize handle = bottom-right 10x10 pixel zone of bordered windows
mouse_hittest_resize:
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov cx, [mouse_x]
    mov dx, [mouse_y]

    ; Find topmost visible window whose resize handle contains the click
    xor si, si                      ; SI = window index
    mov di, window_table
    mov bp, 0xFFFF                  ; BP = best handle (0xFFFF = none)
    mov bl, 0                       ; BL = best z-order so far

.rht_find:
    cmp si, WIN_MAX_COUNT
    jae .rht_found

    cmp byte [di + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .rht_next

    ; Skip frameless windows (no resize handle)
    test byte [di + WIN_OFF_FLAGS], WIN_FLAG_BORDER
    jz .rht_next

    ; Check if click is in bottom-right 10x10 zone
    ; corner_x = win_x + win_w - 10
    mov ax, [di + WIN_OFF_X]
    add ax, [di + WIN_OFF_WIDTH]
    sub ax, 10
    cmp cx, ax                      ; mouse_x >= corner_x?
    jb .rht_next
    ; Also check mouse_x < win_x + win_w
    add ax, 10
    cmp cx, ax
    jae .rht_next

    ; corner_y = win_y + win_h - 10
    mov ax, [di + WIN_OFF_Y]
    add ax, [di + WIN_OFF_HEIGHT]
    sub ax, 10
    cmp dx, ax                      ; mouse_y >= corner_y?
    jb .rht_next
    ; Also check mouse_y < win_y + win_h
    add ax, 10
    cmp dx, ax
    jae .rht_next

    ; Hit! Check if higher z-order than current best
    mov al, [di + WIN_OFF_ZORDER]
    cmp bp, 0xFFFF
    je .rht_new_best
    cmp al, bl
    jbe .rht_next

.rht_new_best:
    mov bp, si
    mov bl, al

.rht_next:
    add di, WIN_ENTRY_SIZE
    inc si
    jmp .rht_find

.rht_found:
    cmp bp, 0xFFFF
    je .rht_no_hit
    mov ax, bp
    clc
    jmp .rht_done

.rht_no_hit:
    stc

.rht_done:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; ============================================================================
; Mouse Drag State Machine
; ============================================================================

; mouse_drag_update - Track window drag state from button transitions
; Called from int_74_handler after position update
; Sets flags only - never calls win_move_stub (avoids reentrancy)
mouse_drag_update:
    pusha

    mov al, [mouse_buttons]
    mov ah, [drag_prev_buttons]
    mov [drag_prev_buttons], al

    ; Detect left button press (0 -> 1 transition)
    test ah, 0x01                   ; Was left pressed before?
    jnz .already_held
    test al, 0x01                   ; Is left pressed now?
    jz .check_release

    ; === Left button JUST pressed: hit-test title bars ===
    call mouse_hittest_titlebar
    jc .try_resize                  ; No titlebar hit — check resize handle

    ; Check if click is on close button (rightmost 12px of title bar)
    push si
    xor ah, ah
    mov si, ax
    shl si, 5
    add si, window_table
    mov bx, [si + WIN_OFF_X]
    add bx, [si + WIN_OFF_WIDTH]
    sub bx, 12                      ; BX = left edge of close button zone
    cmp word [mouse_x], bx
    pop si
    jb .start_drag                  ; Click left of close button → drag

    ; Close button clicked — set flag for deferred processing
    mov [drag_window], al
    mov byte [drag_needs_focus], 1
    mov [close_kill_window], al
    mov byte [close_needs_kill], 1
    jmp .done

.try_resize:
    ; No titlebar hit — check if click is on a resize handle
    call mouse_hittest_resize
    jc .done                        ; No resize hit either
    jmp .start_resize

.start_drag:
    ; Start drag setup
    mov [drag_window], al
    mov byte [drag_needs_focus], 1  ; Signal focus needed (handled in main thread)

    ; Calculate grab offset = mouse_pos - window_pos
    xor ah, ah
    mov bx, ax
    shl bx, 5                      ; BX = handle * 32
    add bx, window_table

    mov ax, [mouse_x]
    sub ax, [bx + WIN_OFF_X]
    mov [drag_offset_x], ax

    mov ax, [mouse_y]
    sub ax, [bx + WIN_OFF_Y]
    mov [drag_offset_y], ax

    ; Initialize target to current window position (prevents jump on first frame)
    mov ax, [bx + WIN_OFF_X]
    mov [drag_target_x], ax
    mov ax, [bx + WIN_OFF_Y]
    mov [drag_target_y], ax

    ; Set active LAST - after target is initialized (prevents race with process_drag)
    mov byte [drag_active], 1

    jmp .done

.start_resize:
    ; Start resize setup
    mov [resize_window], al
    mov byte [drag_needs_focus], 1  ; Bring window to front
    mov [drag_window], al           ; Focus uses drag_window

    ; Read current window dimensions
    xor ah, ah
    mov bx, ax
    shl bx, 5
    add bx, window_table
    mov ax, [bx + WIN_OFF_WIDTH]
    mov [resize_start_w], ax
    mov [resize_target_w], ax
    mov ax, [bx + WIN_OFF_HEIGHT]
    mov [resize_start_h], ax
    mov [resize_target_h], ax

    ; Save mouse position at grab start
    mov ax, [mouse_x]
    mov [resize_start_mx], ax
    mov ax, [mouse_y]
    mov [resize_start_my], ax

    ; Set active LAST
    mov byte [resize_active], 1
    jmp .done

.already_held:
    test al, 0x01                   ; Still held?
    jz .button_released

    ; Check if resize is active
    cmp byte [resize_active], 0
    jne .resize_held

    cmp byte [drag_active], 0
    je .done

    ; Calculate drag target = mouse - offset
    mov ax, [mouse_x]
    sub ax, [drag_offset_x]
    cmp ax, 0x8000                  ; Negative wrap?
    jb .target_x_ok
    xor ax, ax
.target_x_ok:
    mov [drag_target_x], ax

    mov ax, [mouse_y]
    sub ax, [drag_offset_y]
    cmp ax, 0x8000
    jb .target_y_ok
    xor ax, ax
.target_y_ok:
    mov [drag_target_y], ax
    jmp .done

.resize_held:
    ; Get window position for position-aware clamping
    push bx
    xor ah, ah
    mov al, [resize_window]
    mov bx, ax
    shl bx, 5
    add bx, window_table

    ; Calculate resize target = start_dim + (mouse - start_mouse)
    mov ax, [mouse_x]
    sub ax, [resize_start_mx]
    add ax, [resize_start_w]
    ; Clamp minimum width to 60
    cmp ax, 60
    jge .resize_w_min_ok
    mov ax, 60
.resize_w_min_ok:
    ; Clamp maximum width to screen_width - win_x
    push cx
    mov cx, [screen_width]
    sub cx, [bx + WIN_OFF_X]
    cmp ax, cx
    jbe .resize_w_max_ok
    mov ax, cx
.resize_w_max_ok:
    pop cx
    mov [resize_target_w], ax

    mov ax, [mouse_y]
    sub ax, [resize_start_my]
    add ax, [resize_start_h]
    ; Clamp minimum height to 40
    cmp ax, 40
    jge .resize_h_min_ok
    mov ax, 40
.resize_h_min_ok:
    ; Clamp maximum height to screen_height - win_y
    push cx
    mov cx, [screen_height]
    sub cx, [bx + WIN_OFF_Y]
    cmp ax, cx
    jbe .resize_h_max_ok
    mov ax, cx
.resize_h_max_ok:
    pop cx
    mov [resize_target_h], ax
    pop bx
    jmp .done

.button_released:
    ; If resize was active, set up deferred finish
    cmp byte [resize_active], 0
    je .check_drag_release
    mov ax, [resize_target_w]
    mov [resize_finish_w], ax
    mov ax, [resize_target_h]
    mov [resize_finish_h], ax
    mov byte [resize_needs_finish], 1
    mov byte [resize_active], 0
    jmp .done

.check_drag_release:
    ; If drag was active, set up deferred finish (move window on release)
    cmp byte [drag_active], 0
    je .button_released_done
    mov ax, [drag_target_x]
    mov [drag_finish_x], ax
    mov ax, [drag_target_y]
    mov [drag_finish_y], ax
    mov byte [drag_needs_finish], 1
.button_released_done:
    mov byte [drag_active], 0
    jmp .done

.check_release:
    mov byte [drag_active], 0
    mov byte [resize_active], 0

.done:
    popa
    ret

; ============================================================================
; draw_xor_rect_outline - Draw/erase XOR rectangle outline on screen
; Self-inverse: call twice at same position to erase
; Input: BX=X, CX=Y, DX=width, SI=height
; Uses plot_pixel_xor (ES must be video segment)
; ============================================================================
draw_xor_rect_outline:
    pusha
    push es

    mov ax, [video_segment]
    mov es, ax

    ; Save parameters
    mov [.rx], bx
    mov [.ry], cx
    mov [.rw], dx
    mov [.rh], si

    ; --- Top edge: (x..x+w-1, y) ---
    mov cx, bx                      ; CX = X start
    mov bx, [.ry]                   ; BX = Y
    mov dx, [.rw]
.top:
    call plot_pixel_xor
    inc cx
    dec dx
    jnz .top

    ; --- Bottom edge: (x..x+w-1, y+h-1) ---
    mov cx, [.rx]
    mov bx, [.ry]
    add bx, [.rh]
    dec bx                          ; BX = Y + H - 1
    mov dx, [.rw]
.bottom:
    call plot_pixel_xor
    inc cx
    dec dx
    jnz .bottom

    ; --- Left edge: (x, y+1..y+h-2) ---
    mov cx, [.rx]
    mov bx, [.ry]
    inc bx                          ; BX = Y + 1
    mov dx, [.rh]
    sub dx, 2                       ; skip corners
    jle .skip_sides
.left:
    call plot_pixel_xor
    inc bx
    dec dx
    jnz .left

    ; --- Right edge: (x+w-1, y+1..y+h-2) ---
    mov cx, [.rx]
    add cx, [.rw]
    dec cx                          ; CX = X + W - 1
    mov bx, [.ry]
    inc bx
    mov dx, [.rh]
    sub dx, 2
.right:
    call plot_pixel_xor
    inc bx
    dec dx
    jnz .right

.skip_sides:
    pop es
    popa
    ret

.rx: dw 0
.ry: dw 0
.rw: dw 0
.rh: dw 0

; ============================================================================
; Deferred Drag Processing (called from event_get_stub)
; ============================================================================

; mouse_process_drag - Process pending window drag (called from event_get_stub)
; Polls drag state and moves window if target differs from current position.
mouse_process_drag:
    ; Handle deferred focus (bring clicked window to front)
    cmp byte [drag_needs_focus], 0
    je .no_focus

    mov byte [drag_needs_focus], 0
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    ; Save and disable clip state - win_draw_stub uses gfx_draw_string_inverted
    ; which checks clip_enabled; stale clip rect from calling task clips title text
    push word [clip_enabled]
    mov byte [clip_enabled], 0

    ; Check if window is already topmost - skip content clear if so
    xor ah, ah
    mov al, [drag_window]
    mov si, ax
    shl si, 5
    add si, window_table
    cmp byte [si + WIN_OFF_ZORDER], 15
    je .focus_already_top           ; Already focused, just redraw frame

    ; Window is being brought to front - focus, clear, and redraw
    mov al, [drag_window]
    call win_focus_stub             ; Update z-order
    call win_draw_stub              ; Redraw frame on top

    ; Clear content area and trigger app redraw
    xor ah, ah
    mov al, [drag_window]
    mov si, ax
    shl si, 5
    add si, window_table

    mov bx, [si + WIN_OFF_X]
    mov cx, [si + WIN_OFF_Y]
    mov dx, [si + WIN_OFF_WIDTH]
    mov di, [si + WIN_OFF_HEIGHT]
    test byte [si + WIN_OFF_FLAGS], WIN_FLAG_TITLE | WIN_FLAG_BORDER
    jz .focus_clear_frameless
    inc bx                          ; Inside left border
    add cx, [titlebar_height]       ; Below title bar
    sub dx, 2                       ; Inside both borders
    sub di, [titlebar_height]
    dec di                          ; Above bottom border
.focus_clear_frameless:
    mov si, di                      ; SI = height
    test si, si
    jz .focus_skip_clear
    call gfx_clear_area_stub
.focus_skip_clear:

    ; Post redraw event so the app redraws content
    mov al, EVENT_WIN_REDRAW
    xor dx, dx
    mov dl, [drag_window]
    call post_event
    jmp .focus_done

.focus_already_top:
    ; Already topmost - just redraw frame (no content clear/redraw)
    mov al, [drag_window]
    call win_draw_stub

.focus_done:
    pop word [clip_enabled]         ; Restore clip state
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
.no_focus:

    ; Handle deferred close button kill
    cmp byte [close_needs_kill], 0
    je .no_close_kill
    mov byte [close_needs_kill], 0

    ; Look up the window's owner task
    push ax
    push bx
    push si
    xor ah, ah
    mov al, [close_kill_window]
    mov si, ax
    shl si, 5
    add si, window_table
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .close_kill_done            ; Window already gone
    mov al, [si + WIN_OFF_OWNER]
    cmp al, 0xFF
    je .close_kill_done             ; Kernel-owned window, skip

    cmp al, [current_task]
    je .close_kill_self

    ; --- Kill a different task ---
    xor ah, ah
    mov si, ax
    shl si, 5
    add si, app_table
    cmp byte [si + APP_OFF_STATE], APP_STATE_RUNNING
    jne .close_kill_done
    mov byte [si + APP_OFF_STATE], APP_STATE_FREE
    ; Free segment
    mov bx, [si + APP_OFF_CODE_SEG]
    cmp bx, APP_SEGMENT_SHELL
    je .close_kill_skip_free
    call free_segment
.close_kill_skip_free:
    call speaker_off_stub           ; Silence speaker
    call destroy_task_windows       ; AL = task handle
.close_kill_done:
    pop si
    pop bx
    pop ax
    jmp .no_close_kill

.close_kill_self:
    ; Killing the current task — app_exit_stub handles full teardown + context switch
    pop si
    pop bx
    pop ax
    call speaker_off_stub
    jmp app_exit_stub               ; Never returns

.no_close_kill:
    ; --- Phase 3: Handle active drag (draw XOR outline) ---
    cmp byte [drag_active], 0
    je .check_resize_active

    ; Read drag state atomically
    cli
    xor ax, ax
    mov al, [drag_window]
    mov bx, [drag_target_x]
    mov cx, [drag_target_y]
    sti

    ; If outline already drawn at this exact position, skip
    cmp byte [drag_outline_drawn], 0
    je .need_outline
    cmp bx, [drag_outline_x]
    jne .need_outline
    cmp cx, [drag_outline_y]
    je .done                        ; Same position, nothing to do

.need_outline:
    ; Hide cursor for clean XOR drawing
    call mouse_cursor_hide
    inc byte [cursor_locked]

    ; Erase old outline if one is drawn
    cmp byte [drag_outline_drawn], 0
    je .no_erase
    push bx
    push cx
    mov bx, [drag_outline_x]
    mov cx, [drag_outline_y]
    mov dx, [drag_outline_w]
    mov si, [drag_outline_h]
    call draw_xor_rect_outline
    pop cx
    pop bx

.no_erase:
    ; Get window dimensions from window table
    push ax
    xor ah, ah
    mov al, [drag_window]
    mov si, ax
    shl si, 5
    add si, window_table
    mov dx, [si + WIN_OFF_WIDTH]
    mov [drag_outline_w], dx
    mov si, [si + WIN_OFF_HEIGHT]
    mov [drag_outline_h], si
    pop ax

    ; Draw new outline at target position
    mov [drag_outline_x], bx
    mov [drag_outline_y], cx
    mov dx, [drag_outline_w]
    mov si, [drag_outline_h]
    call draw_xor_rect_outline
    mov byte [drag_outline_drawn], 1

    dec byte [cursor_locked]
    call mouse_cursor_show
    jmp .done

.check_resize_active:
    ; --- Phase 3b: Handle active resize (draw XOR outline with new size) ---
    cmp byte [resize_active], 0
    je .check_finish

    ; Read resize state atomically
    cli
    mov dx, [resize_target_w]
    mov si, [resize_target_h]
    sti

    ; If outline already drawn at this exact size, skip
    cmp byte [resize_outline_drawn], 0
    je .need_resize_outline
    cmp dx, [drag_outline_w]
    jne .need_resize_outline
    cmp si, [drag_outline_h]
    je .done                        ; Same size, nothing to do

.need_resize_outline:
    call mouse_cursor_hide
    inc byte [cursor_locked]

    ; Erase old outline if one is drawn
    cmp byte [resize_outline_drawn], 0
    je .no_resize_erase
    push dx
    push si
    mov bx, [drag_outline_x]
    mov cx, [drag_outline_y]
    mov dx, [drag_outline_w]
    mov si, [drag_outline_h]
    call draw_xor_rect_outline
    pop si
    pop dx

.no_resize_erase:
    ; Get window position (stays constant during resize)
    push dx
    push si
    xor ah, ah
    mov al, [resize_window]
    mov di, ax
    shl di, 5
    add di, window_table
    mov bx, [di + WIN_OFF_X]
    mov cx, [di + WIN_OFF_Y]
    mov [drag_outline_x], bx
    mov [drag_outline_y], cx
    pop si
    pop dx

    ; Draw new outline at window position with target size
    mov [drag_outline_w], dx
    mov [drag_outline_h], si
    call draw_xor_rect_outline
    mov byte [resize_outline_drawn], 1

    dec byte [cursor_locked]
    call mouse_cursor_show
    jmp .done

.check_resize_finish:
    ; --- Phase 4b: Handle resize finish (apply new dimensions) ---
    cmp byte [resize_needs_finish], 0
    je .done
    mov byte [resize_needs_finish], 0

    call mouse_cursor_hide
    inc byte [cursor_locked]

    ; Erase resize outline if one is drawn
    cmp byte [resize_outline_drawn], 0
    je .no_resize_final_erase
    mov bx, [drag_outline_x]
    mov cx, [drag_outline_y]
    mov dx, [drag_outline_w]
    mov si, [drag_outline_h]
    call draw_xor_rect_outline
    mov byte [resize_outline_drawn], 0

.no_resize_final_erase:
    ; Check if size actually changed
    xor ah, ah
    mov al, [resize_window]
    mov di, ax
    shl di, 5
    add di, window_table
    mov dx, [resize_finish_w]
    cmp dx, [di + WIN_OFF_WIDTH]
    jne .do_resize
    mov si, [resize_finish_h]
    cmp si, [di + WIN_OFF_HEIGHT]
    je .resize_done                 ; Same size, skip

.do_resize:
    ; Apply resize via win_resize_stub
    mov dx, [resize_finish_w]
    mov si, [resize_finish_h]
    xor ah, ah
    mov al, [resize_window]
    push word [clip_enabled]
    mov byte [clip_enabled], 0
    call win_resize_stub            ; Redraws frame, posts WIN_REDRAW

    ; Update topmost cache if this is the topmost window
    xor ah, ah
    mov al, [resize_window]
    cmp al, [topmost_handle]
    jne .resize_not_topmost
    mov ax, [resize_finish_w]
    mov [topmost_win_w], ax
    mov ax, [resize_finish_h]
    mov [topmost_win_h], ax
.resize_not_topmost:
    pop word [clip_enabled]

.resize_done:
    dec byte [cursor_locked]
    call mouse_cursor_show
    jmp .done

.check_finish:
    ; --- Phase 4: Handle drag finish (move window to final position) ---
    cmp byte [drag_needs_finish], 0
    je .check_resize_finish
    mov byte [drag_needs_finish], 0

    call mouse_cursor_hide
    inc byte [cursor_locked]

    ; Erase outline if one is drawn
    cmp byte [drag_outline_drawn], 0
    je .no_final_erase
    mov bx, [drag_outline_x]
    mov cx, [drag_outline_y]
    mov dx, [drag_outline_w]
    mov si, [drag_outline_h]
    call draw_xor_rect_outline
    mov byte [drag_outline_drawn], 0

.no_final_erase:
    ; Check if window actually moved (skip if same position)
    xor ah, ah
    mov al, [drag_window]
    mov si, ax
    shl si, 5
    add si, window_table
    mov bx, [drag_finish_x]
    cmp bx, [si + WIN_OFF_X]
    jne .do_final_move
    mov cx, [drag_finish_y]
    cmp cx, [si + WIN_OFF_Y]
    je .finish_done                 ; Same position, skip move

.do_final_move:
    ; Move window to final position
    ; win_move_stub: draws frame, clears content, clears old strips,
    ; calls redraw_affected_windows for background windows
    xor ah, ah
    mov al, [drag_window]
    mov bx, [drag_finish_x]
    mov cx, [drag_finish_y]
    call win_move_stub

    ; Post redraw event so the dragged window's app repaints content
    mov al, EVENT_WIN_REDRAW
    xor dx, dx
    mov dl, [drag_window]
    call post_event

.finish_done:
    dec byte [cursor_locked]
    call mouse_cursor_show

.done:
    ret

; ============================================================================
; Draw character in inverted colors (black on white)
; Uses draw_x, draw_y for position, SI = font data pointer
; ============================================================================

draw_char_inverted:
    pusha

    xor bx, bx
    mov bl, [draw_font_height]      ; BX = font height (BH zeroed above)
    mov bp, bx                      ; BP = row counter
    mov bx, [draw_y]                ; BX = current Y

.row_loop:
    lodsb                           ; Get row bitmap into AL (from DS:SI)
    mov ah, al                      ; AH = bitmap for this row
    mov cx, [draw_x]                ; CX = current X
    xor dx, dx
    mov dl, [draw_font_width]       ; DX = glyph column counter

.col_loop:
    test ah, 0x80                   ; Check leftmost bit
    jz .clear_pixel
    call plot_pixel_black           ; "1" bit: black pixel (inverted)
    jmp .next_pixel
.clear_pixel:
    call plot_pixel_white           ; "0" bit: white background
.next_pixel:
    shl ah, 1                       ; Next bit
    inc cx                          ; Next X
    dec dx                          ; Decrement glyph column counter
    jnz .col_loop

    ; Fill gap pixels (advance - width) with inverted background color
    push dx
    xor dx, dx
    mov dl, [draw_font_advance]
    sub dl, [draw_font_width]
    jz .no_gap
.gap_loop:
    call plot_pixel_white           ; White background in inter-character gap
    inc cx
    dec dl
    jnz .gap_loop
.no_gap:
    pop dx

    inc bx                          ; Next Y
    dec bp                          ; Decrement row counter
    jnz .row_loop

    xor ax, ax
    mov al, [draw_font_advance]
    add [draw_x], ax                ; Advance to next character position

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
    push ds                         ; Save kernel DS
    mov word [draw_x], bx
    mov word [draw_y], cx
    mov bp, [caller_ds]             ; BP = caller's segment
    mov dx, [video_segment]
    mov es, dx
    mov ds, bp                      ; DS = caller's segment for string access
.loop:
    lodsb                           ; Load character from caller's DS:SI
    test al, al
    jz .done
    push ds                         ; Save caller's DS for string
    mov bp, 0x1000
    mov ds, bp                      ; DS = kernel segment for font access
    ; --- Character-level clipping ---
    cmp byte [clip_enabled], 0
    je .no_clip
    mov di, [draw_y]
    cmp di, [clip_y2]
    ja .clip_exit
    mov di, [draw_x]
    cmp di, [clip_x2]
    ja .skip_char
    xor dx, dx
    mov dl, [draw_font_width]
    add di, dx
    cmp di, [clip_x1]
    jb .skip_char
.no_clip:
    sub al, 32
    mov ah, 0
    mov dl, [draw_font_bpc]
    mul dl
    mov di, si                      ; Save string pointer
    mov si, [draw_font_base]        ; SI = font offset (now in kernel DS!)
    add si, ax
    call draw_char_inverted         ; Draw with DS=0x1000 for font
    mov si, di                      ; Restore string pointer
    pop ds                          ; Restore caller's DS for string
    jmp .loop
.skip_char:
    xor ax, ax
    mov al, [draw_font_advance]
    add [draw_x], ax
    pop ds
    jmp .loop
.clip_exit:
    pop ds
.done:
    pop ds                          ; Restore original DS
    pop bp
    pop di
    pop dx
    pop ax
    pop es
    ret

; ============================================================================
; gfx_text_width - Measure string width in pixels
; Input: SI = pointer to string (uses caller_ds for segment)
; Output: DX = width in pixels (characters * advance)
; Preserves: All other registers
; ============================================================================
gfx_text_width:
    push ax
    push si
    push ds

    mov ds, [cs:caller_ds]          ; Use app's data segment
    xor dx, dx                      ; DX = pixel width accumulator

.tw_loop:
    lodsb
    test al, al
    jz .tw_done
    add dl, [cs:draw_font_advance]  ; Use current font advance
    adc dh, 0
    jmp .tw_loop

.tw_done:
    pop ds
    pop si
    pop ax
    clc
    ret

; ============================================================================
; System Clipboard APIs (Build 273)
; ============================================================================

; clip_copy - Copy data to system clipboard (API 84)
; Input: SI = source offset in caller's DS segment, CX = byte count
; Output: CF=0 success, CF=1 error (CX > CLIP_MAX_SIZE)
clip_copy:
    cmp cx, CLIP_MAX_SIZE
    ja .clip_copy_err
    push ds
    push es
    push si
    push di
    push cx
    mov [cs:clip_data_len], cx
    mov ds, [cs:caller_ds]          ; Source = caller's segment
    mov ax, SCRATCH_SEGMENT
    mov es, ax
    xor di, di                      ; Dest = 0x9000:0000
    cld
    rep movsb
    pop cx
    pop di
    pop si
    pop es
    pop ds
    clc
    ret
.clip_copy_err:
    stc
    ret

; clip_paste - Read data from system clipboard (API 85)
; Input: DI = dest offset in caller's ES segment, CX = max bytes to read
; Output: CX = actual bytes copied, CF=0 success, CF=1 clipboard empty
clip_paste:
    push ds
    push es
    push si
    push di
    mov ax, [clip_data_len]
    test ax, ax
    jz .clip_paste_empty
    cmp ax, cx
    jbe .clip_paste_ok
    mov ax, cx                      ; Clamp to max
.clip_paste_ok:
    mov cx, ax
    push cx
    mov ax, SCRATCH_SEGMENT
    mov ds, ax
    xor si, si                      ; Source = 0x9000:0000
    mov es, [cs:caller_es]          ; Dest = caller's segment
    cld
    rep movsb
    pop cx
    pop di
    pop si
    pop es
    pop ds
    clc
    ret
.clip_paste_empty:
    xor cx, cx
    pop di
    pop si
    pop es
    pop ds
    stc
    ret

; clip_get_len - Get clipboard content length (API 86)
; Input: none
; Output: CX = clipboard length (0 = empty)
clip_get_len:
    mov cx, [clip_data_len]
    clc
    ret

; ============================================================================
; Popup Menu APIs (Build 273)
; ============================================================================

KMENU_ITEM_H        equ 10                  ; Pixels per menu item

; menu_open - Open a popup menu at specified position (API 87)
; Input: BX=X, CX=Y (auto-translated by draw_context),
;        SI=string table pointer (in caller's DS), DL=item count, DH=width
; Output: CF=0 success
; Note: BX/CX arrive already translated to absolute screen coords by INT 0x80
menu_open:
    call mouse_cursor_hide
    inc byte [cursor_locked]

    ; Save menu parameters
    mov [kmenu_count], dl
    mov [kmenu_w], dh
    mov [kmenu_str_table], si       ; String table offset in caller's segment

    ; Clamp X so menu fits on screen
    movzx ax, dh                    ; AX = menu width
    mov si, [screen_width]
    sub si, ax                      ; SI = max X
    cmp bx, si
    jbe .mo_x_ok
    mov bx, si
.mo_x_ok:
    mov [kmenu_x], bx

    ; Clamp Y so menu fits on screen
    movzx ax, dl                    ; AX = item count
    imul ax, KMENU_ITEM_H           ; AX = menu height
    mov si, [screen_height]
    sub si, ax                      ; SI = max Y
    cmp cx, si
    jbe .mo_y_ok
    mov cx, si
.mo_y_ok:
    mov [kmenu_y], cx
    mov byte [kmenu_active], 1

    ; Save and clear draw_context + clip_enabled for absolute screen drawing
    ; Without this, gfx_draw_string_inverted clips menu text against stale
    ; window clip rect from the calling app's WIN_BEGIN_DRAW context.
    push word [draw_context]
    push word [clip_enabled]
    mov byte [draw_context], 0xFF
    mov byte [clip_enabled], 0

    ; Compute menu pixel height
    movzx si, byte [kmenu_count]
    imul si, KMENU_ITEM_H

    ; Draw white filled rectangle
    mov bx, [kmenu_x]
    mov cx, [kmenu_y]
    movzx dx, byte [kmenu_w]
    mov al, 3                       ; White
    call gfx_draw_filled_rect_color

    ; Draw black border
    mov bx, [kmenu_x]
    mov cx, [kmenu_y]
    movzx dx, byte [kmenu_w]
    movzx si, byte [kmenu_count]
    imul si, KMENU_ITEM_H
    mov al, 0                       ; Black
    call gfx_draw_rect_color

    ; Draw menu items — strings are in caller's segment (caller_ds)
    xor cx, cx                      ; CX = item index (byte in CL)
.mo_item_loop:
    cmp cl, [kmenu_count]
    jae .mo_items_done

    ; Compute item Y = kmenu_y + index * KMENU_ITEM_H + 2
    movzx ax, cl
    imul ax, KMENU_ITEM_H
    add ax, [kmenu_y]
    add ax, 2
    push cx                         ; Save index

    ; Read string pointer from caller's string table
    movzx bx, cl
    shl bx, 1
    add bx, [kmenu_str_table]       ; BX = offset of pointer in caller's segment
    push ds
    mov ds, [cs:caller_ds]          ; DS = caller's segment
    mov si, [bx]                    ; SI = string offset in caller's segment
    pop ds                          ; DS = kernel segment

    ; Text X = kmenu_x + 4
    mov bx, [kmenu_x]
    add bx, 4
    mov cx, ax                      ; CX = Y position
    ; caller_ds is already the app's DS — gfx_draw_string_inverted reads from it
    call gfx_draw_string_inverted

    pop cx
    inc cl
    jmp .mo_item_loop
.mo_items_done:
    ; Restore clip_enabled and draw_context
    pop word [clip_enabled]
    pop word [draw_context]

    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; menu_close - Close popup menu and trigger repaint (API 88)
; Input: none
; Output: CF=0
menu_close:
    cmp byte [kmenu_active], 0
    je .mc_done
    mov byte [kmenu_active], 0
    ; Post WIN_REDRAW for the topmost window so app repaints over menu
    movzx dx, byte [topmost_handle]
    cmp dl, 0xFF
    je .mc_done
    mov al, EVENT_WIN_REDRAW
    call post_event
.mc_done:
    clc
    ret

; menu_hit - Hit-test the active popup menu (API 89)
; Input: none (reads mouse_x, mouse_y internally)
; Output: AL = item index or 0xFF if outside menu
menu_hit:
    cmp byte [kmenu_active], 0
    je .mh_miss
    ; Check X bounds
    mov ax, [mouse_x]
    cmp ax, [kmenu_x]
    jb .mh_miss
    mov bx, [kmenu_x]
    movzx cx, byte [kmenu_w]
    add bx, cx
    cmp ax, bx
    jae .mh_miss
    ; Check Y bounds and compute item index
    mov ax, [mouse_y]
    sub ax, [kmenu_y]
    js .mh_miss                     ; Above menu
    xor dx, dx
    mov bx, KMENU_ITEM_H
    div bx                          ; AX = item index
    cmp al, [kmenu_count]
    jae .mh_miss
    ; AL = valid item index
    clc
    ret
.mh_miss:
    mov al, 0xFF
    clc
    ret

; ============================================================================
; System File Dialog (Build 274)
; ============================================================================

; Constants
FDLG_W          equ 152                 ; Dialog window width
FDLG_X          equ 84                  ; Centered X: (320-152)/2
FDLG_BTN_GAP    equ 2                   ; Gap between list and buttons
FDLG_MAX_FILES  equ 64                  ; Max files stored
FDLG_ENTRY_SIZE equ 13                  ; 12 chars "FILENAME.EXT" + null
FDLG_LIST_W     equ FDLG_W - 4 - SCROLLBAR_WIDTH  ; List width (140) with scrollbar
FDLG_BUF_SEG    equ 0x9000              ; Scratch segment
FDLG_BUF_OFF    equ 0x1000              ; Offset in scratch (clipboard uses 0x0000-0x0FFF)

; ============================================================================
; file_dialog_open - System file open dialog (API 90)
; Input:  BL = mount handle (0=FAT12, 1=FAT16)
;         ES:DI = destination buffer for filename (13+ bytes)
; Output: CF=0 → file selected, filename at ES:DI (null-terminated dot format)
;         CF=1 → cancelled or error
; Blocking: creates modal window, runs event loop, returns on select/cancel
; ============================================================================
file_dialog_open:
    ; Save caller state
    mov ax, [caller_es]
    mov [fdlg_caller_es], ax            ; Save original app ES
    mov al, [draw_context]
    mov [fdlg_save_ctx], al
    call win_end_draw                   ; Clear caller's draw context
    mov [fdlg_mount], bl
    mov [fdlg_result_di], di

    ; Compute dynamic layout from current font
    movzx ax, byte [draw_font_height]
    add ax, 2
    mov [fdlg_item_h], ax              ; item_h = font_height + 2

    movzx ax, byte [draw_font_height]
    add ax, 4
    mov [fdlg_btn_h], ax               ; btn_h = font_height + 4

    ; Compute max visible items, capped at 11
    mov ax, [screen_height]
    sub ax, 18                         ; titlebar + borders + gap + margins
    sub ax, [fdlg_btn_h]
    xor dx, dx
    div word [fdlg_item_h]
    cmp ax, 11
    jbe .fdlg_vis_ok
    mov ax, 11
.fdlg_vis_ok:
    mov [fdlg_vis], ax

    ; list_h = vis * item_h
    mul word [fdlg_item_h]
    mov [fdlg_list_h], ax

    ; win_h = 11 (title+top_border) + list_h + 2 (gap) + btn_h + 1 (bottom_border)
    mov ax, [fdlg_list_h]
    add ax, 14                         ; 11 + 2 + 1
    add ax, [fdlg_btn_h]
    mov [fdlg_h_dyn], ax

    ; Center: y = (screen_height - win_h) / 2
    mov bx, [screen_height]
    sub bx, ax
    shr bx, 1
    mov [fdlg_y_dyn], bx

    ; Scan directory
    call fdlg_scan_files

    ; Create dialog window (title in kernel segment)
    mov word [caller_es], 0x1000
    mov bx, [screen_width]
    sub bx, FDLG_W
    shr bx, 1                          ; Center X: (screen_width - FDLG_W) / 2
    mov cx, [fdlg_y_dyn]
    mov dx, FDLG_W
    mov si, [fdlg_h_dyn]
    mov di, fdlg_title
    mov al, WIN_FLAG_TITLE | WIN_FLAG_BORDER
    call win_create_stub
    jc .fdlg_error
    mov [fdlg_handle], al

    ; Force no content scaling — dialog draws via direct call, not INT 0x80
    push bx
    xor ah, ah
    mov bx, ax
    shl bx, 5
    add bx, window_table
    mov byte [bx + WIN_OFF_CONTENT_SCALE], 1
    pop bx

    ; Initialize selection state
    mov word [fdlg_sel], 0
    mov word [fdlg_scroll], 0
    mov byte [fdlg_prev_btn], 0

    ; Initial draw
    call fdlg_draw_full

    ; --- Modal event loop ---
.fdlg_loop:
    sti                                 ; Re-enable interrupts
    hlt                                 ; Wait for next interrupt

    ; Poll events
    call event_get_stub
    jc .fdlg_check_mouse               ; No event — check mouse

    ; Dispatch by event type
    cmp al, EVENT_KEY_PRESS
    je .fdlg_key
    cmp al, EVENT_WIN_REDRAW
    je .fdlg_redraw
    jmp .fdlg_loop

.fdlg_key:
    cmp dl, 27                          ; ESC → cancel
    je .fdlg_cancel
    cmp dl, 13                          ; Enter → select
    je .fdlg_select
    cmp dl, 128                         ; Up arrow (INT 9 unified)
    je .fdlg_up
    cmp dl, 129                         ; Down arrow
    je .fdlg_down
    ; Fallback scancode check (BIOS INT 16h: DL=0, DH=scancode)
    test dl, dl
    jnz .fdlg_loop
    cmp dh, 0x48                        ; Up scancode
    je .fdlg_up
    cmp dh, 0x50                        ; Down scancode
    je .fdlg_down
    jmp .fdlg_loop

.fdlg_redraw:
    cmp dl, [fdlg_handle]              ; Only redraw our dialog
    jne .fdlg_loop
    call fdlg_draw_full
    jmp .fdlg_loop

.fdlg_check_mouse:
    test byte [mouse_buttons], 1        ; Left button
    jz .fdlg_mouse_up
    cmp byte [fdlg_prev_btn], 0
    jne .fdlg_loop                      ; Button held, not new click
    mov byte [fdlg_prev_btn], 1
    jmp .fdlg_click
.fdlg_mouse_up:
    mov byte [fdlg_prev_btn], 0
    jmp .fdlg_loop

    ; --- Navigation ---
.fdlg_up:
    cmp word [fdlg_sel], 0
    je .fdlg_loop
    dec word [fdlg_sel]
    call fdlg_scroll_into_view
    call fdlg_draw_full
    jmp .fdlg_loop

.fdlg_down:
    mov ax, [fdlg_sel]
    inc ax
    cmp ax, [fdlg_count]
    jae .fdlg_loop
    mov [fdlg_sel], ax
    call fdlg_scroll_into_view
    call fdlg_draw_full
    jmp .fdlg_loop

    ; --- Mouse click hit-test ---
.fdlg_click:
    ; Get content area origin from window table
    movzx bx, byte [fdlg_handle]
    shl bx, 5
    add bx, window_table
    mov ax, [bx + WIN_OFF_X]
    inc ax                              ; +1 border
    mov si, ax                          ; SI = content_x
    mov ax, [bx + WIN_OFF_Y]
    add ax, 11                          ; +1 border + 10 title
    mov di, ax                          ; DI = content_y

    ; Check X bounds
    mov ax, [mouse_x]
    sub ax, si
    jb .fdlg_loop
    cmp ax, FDLG_W - 2
    jae .fdlg_loop

    ; Check Y bounds and compute item index
    mov ax, [mouse_y]
    sub ax, di
    jb .fdlg_loop
    cmp ax, [fdlg_list_h]
    jae .fdlg_check_buttons             ; Below list → check buttons

    ; Check if click is on scrollbar (X >= FDLG_LIST_W relative to content)
    push ax                             ; Save Y-relative
    mov ax, [mouse_x]
    sub ax, si                          ; Content-relative X
    cmp ax, FDLG_LIST_W
    pop ax                              ; Restore Y-relative
    jae .fdlg_scrollbar_click           ; Click on scrollbar

    ; Compute clicked item
    xor dx, dx
    div word [fdlg_item_h]             ; AX = relative item index
    add ax, [fdlg_scroll]              ; AX = absolute item index
    cmp ax, [fdlg_count]
    jae .fdlg_loop

    ; Click on selected → confirm; else just select
    cmp ax, [fdlg_sel]
    je .fdlg_select
    mov [fdlg_sel], ax
    call fdlg_draw_full
    jmp .fdlg_loop

    ; --- Button hit-test ---
.fdlg_check_buttons:
    ; AX = Y relative to content, SI = content_x (still valid)
    mov bx, [fdlg_list_h]
    add bx, FDLG_BTN_GAP
    cmp ax, bx
    jb .fdlg_loop                       ; In gap between list and buttons
    add bx, [fdlg_btn_h]
    cmp ax, bx
    jae .fdlg_loop                      ; Below buttons

    ; In button row — check X to determine which button
    mov ax, [mouse_x]
    sub ax, si                          ; AX = X relative to content

    ; Open button: X range [54, 94)
    cmp ax, 54
    jb .fdlg_loop
    cmp ax, 94
    jb .fdlg_select                     ; Open = confirm selection

    ; Cancel button: X range [98, 150)
    cmp ax, 98
    jb .fdlg_loop                       ; In gap between buttons
    cmp ax, 150
    jb .fdlg_cancel

    jmp .fdlg_loop

    ; --- Scrollbar click ---
.fdlg_scrollbar_click:
    ; AX = Y relative to content (in list area)
    cmp ax, SCROLLBAR_ARROW_H           ; Top 8px = up arrow
    jb .fdlg_up
    mov bx, [fdlg_list_h]
    sub bx, SCROLLBAR_ARROW_H
    cmp ax, bx
    jae .fdlg_down                      ; Bottom 8px = down arrow
    jmp .fdlg_loop                      ; Track area — ignore

    ; --- Selection ---
.fdlg_select:
    cmp word [fdlg_count], 0
    je .fdlg_loop                       ; No files to select

    ; Copy filename to caller's buffer
    mov ax, [fdlg_sel]
    mov bx, FDLG_ENTRY_SIZE
    mul bx
    add ax, FDLG_BUF_OFF

    push ds
    push es
    mov bx, FDLG_BUF_SEG
    mov ds, bx
    mov si, ax                          ; DS:SI = filename in scratch segment
    mov ax, [cs:fdlg_caller_es]
    mov es, ax
    mov di, [cs:fdlg_result_di]
    mov cx, FDLG_ENTRY_SIZE
    cld
    rep movsb
    pop es
    pop ds

    ; Cleanup and return success
    call fdlg_cleanup
    clc
    ret

    ; --- Cancel ---
.fdlg_cancel:
    call fdlg_cleanup
    stc
    ret

    ; --- Error (win_create failed) ---
.fdlg_error:
    mov al, [fdlg_save_ctx]
    mov [draw_context], al
    cmp al, 0xFF
    je .fdlg_err_ret
    call win_begin_draw
.fdlg_err_ret:
    ; Restore caller_es
    mov ax, [fdlg_caller_es]
    mov [caller_es], ax
    stc
    ret

; ============================================================================
; fdlg_cleanup - Destroy dialog, restore caller state
; ============================================================================
fdlg_cleanup:
    movzx ax, byte [fdlg_handle]
    call win_destroy_stub

    ; Restore caller's draw context
    mov al, [fdlg_save_ctx]
    mov [draw_context], al
    cmp al, 0xFF
    je .fcleanup_no_ctx
    call win_begin_draw
.fcleanup_no_ctx:
    ; Restore caller_es
    mov ax, [fdlg_caller_es]
    mov [caller_es], ax
    ret

; ============================================================================
; fdlg_scan_files - Scan directory, build file list in scratch buffer
; ============================================================================
fdlg_scan_files:
    pusha
    push es

    mov word [fdlg_count], 0
    mov ax, FDLG_BUF_SEG
    mov es, ax                          ; ES = scratch segment for storing filenames

    xor cx, cx                          ; CX = readdir state (0 = start)

.fscan_loop:
    cmp word [fdlg_count], FDLG_MAX_FILES
    jae .fscan_done

    ; Read next dir entry into kernel temp buffer
    push es
    mov ax, 0x1000
    mov es, ax
    mov di, fdlg_dir_entry
    mov al, [fdlg_mount]
    call fs_readdir_stub
    pop es
    jc .fscan_done                      ; End of directory

    ; Filter: skip deleted entries
    cmp byte [fdlg_dir_entry], 0xE5
    je .fscan_loop
    ; End-of-directory marker
    cmp byte [fdlg_dir_entry], 0x00
    je .fscan_done
    ; Skip volume labels, directories, LFN entries
    mov al, [fdlg_dir_entry + 11]
    test al, 0x08                       ; Volume label
    jnz .fscan_loop
    test al, 0x10                       ; Directory
    jnz .fscan_loop
    cmp al, 0x0F                        ; LFN entry
    je .fscan_loop

    ; Convert 8.3 FAT name to dot format
    ; Destination: ES:DI = FDLG_BUF_SEG : (FDLG_BUF_OFF + count * FDLG_ENTRY_SIZE)
    mov ax, [fdlg_count]
    push dx
    mov bx, FDLG_ENTRY_SIZE
    mul bx
    pop dx
    add ax, FDLG_BUF_OFF
    mov di, ax

    ; Copy base name (8 bytes), strip trailing spaces
    push cx
    mov si, fdlg_dir_entry
    mov cx, 8
.fscan_name:
    mov al, [si]
    cmp al, ' '
    je .fscan_name_done
    mov [es:di], al
    inc di
    inc si
    loop .fscan_name
    jmp .fscan_check_ext
.fscan_name_done:
    ; Skip remaining name spaces (SI already points past copied chars)
    add si, cx                          ; Jump past remaining spaces
.fscan_check_ext:
    ; Check if extension is blank
    cmp byte [fdlg_dir_entry + 8], ' '
    je .fscan_no_ext
    ; Add dot and extension
    mov byte [es:di], '.'
    inc di
    mov si, fdlg_dir_entry
    add si, 8
    mov cx, 3
.fscan_ext:
    mov al, [si]
    cmp al, ' '
    je .fscan_ext_done
    mov [es:di], al
    inc di
    inc si
    loop .fscan_ext
.fscan_ext_done:
.fscan_no_ext:
    mov byte [es:di], 0                 ; Null terminate
    pop cx

    inc word [fdlg_count]
    jmp .fscan_loop

.fscan_done:
    pop es
    popa
    ret

; ============================================================================
; fdlg_draw_full - Redraw entire file list
; ============================================================================
fdlg_draw_full:
    pusha

    ; Calculate content area origin from window table
    movzx bx, byte [fdlg_handle]
    shl bx, 5
    add bx, window_table
    mov ax, [bx + WIN_OFF_X]
    inc ax                              ; +1 border
    mov [fdlg_cx], ax
    mov ax, [bx + WIN_OFF_Y]
    add ax, 11                          ; +1 border + 10 title
    mov [fdlg_cy], ax

    ; Set draw_context for clipping
    mov al, [fdlg_handle]
    call win_begin_draw

    ; Check for empty directory
    cmp word [fdlg_count], 0
    je .fdraw_empty

    ; Set caller_ds to scratch segment so widget_draw_listitem reads filenames
    push word [caller_ds]
    mov word [caller_ds], FDLG_BUF_SEG

    xor cx, cx                          ; CX = visible index
.fdraw_item:
    cmp cx, [fdlg_vis]
    jae .fdraw_items_done

    ; Absolute item index = scroll + visible index
    mov ax, [fdlg_scroll]
    add ax, cx
    cmp ax, [fdlg_count]
    jae .fdraw_blank                    ; Past end — clear row

    ; SI = filename offset in scratch segment
    push dx
    push cx
    mov bx, FDLG_ENTRY_SIZE
    mul bx
    add ax, FDLG_BUF_OFF
    mov si, ax

    ; Screen Y = content_y + visible_index * item_h
    pop cx                              ; Restore visible index
    mov ax, cx
    mul word [fdlg_item_h]
    add ax, [fdlg_cy]
    mov di, ax                          ; DI = screen Y (saved)

    ; Screen X = content_x
    mov bx, [fdlg_cx]

    ; AL flags: bit 0 = selected
    mov dx, [fdlg_scroll]
    add dx, cx
    xor al, al
    cmp dx, [fdlg_sel]
    jne .fdraw_not_sel
    or al, 1
.fdraw_not_sel:
    push cx                             ; Save visible index for loop
    mov cx, di                          ; CX = correct screen Y
    mov dx, FDLG_LIST_W                 ; Width (minus scrollbar)
    call widget_draw_listitem
    pop cx                              ; Restore visible index
    pop dx                              ; Matches push dx at loop start

    inc cx
    jmp .fdraw_item

.fdraw_blank:
    ; Clear remaining rows
    push cx
    mov ax, cx
    mul word [fdlg_item_h]
    add ax, [fdlg_cy]
    mov cx, ax                          ; Y
    mov bx, [fdlg_cx]                   ; X
    mov dx, FDLG_LIST_W                 ; Width (minus scrollbar)
    mov si, [fdlg_item_h]              ; Height
    call gfx_clear_area_stub
    pop cx
    inc cx
    cmp cx, [fdlg_vis]
    jb .fdraw_blank

.fdraw_items_done:
    pop word [caller_ds]                ; Restore caller_ds
    jmp .fdraw_scrollbar

.fdraw_empty:
    ; Show "(No files)" message
    push word [caller_ds]
    mov word [caller_ds], 0x1000        ; Kernel segment for string
    mov bx, [fdlg_cx]
    add bx, 20
    mov cx, [fdlg_cy]
    add cx, 40
    mov si, fdlg_empty
    call gfx_draw_string_stub
    pop word [caller_ds]

.fdraw_scrollbar:
    ; Draw scrollbar on right side of list
    mov bx, [fdlg_cx]
    add bx, FDLG_LIST_W                ; X = right edge of list
    mov cx, [fdlg_cy]                   ; Y = content top
    mov si, [fdlg_list_h]              ; Track height
    mov dx, [fdlg_scroll]              ; Current scroll position
    ; Compute max_range = max(0, fdlg_count - fdlg_vis)
    mov ax, [fdlg_count]
    sub ax, [fdlg_vis]
    jg .fdraw_sb_range
    xor ax, ax
.fdraw_sb_range:
    mov di, ax                          ; Max range
    xor al, al                          ; Flags: vertical
    call widget_draw_scrollbar

.fdraw_done:
    ; Draw Open and Cancel buttons at bottom
    push word [caller_es]
    mov word [caller_es], 0x1000        ; Kernel segment for button labels

    ; Open button (left)
    mov bx, [fdlg_cx]
    add bx, 54                          ; X offset in content area
    mov cx, [fdlg_cy]
    add cx, [fdlg_list_h]
    add cx, FDLG_BTN_GAP
    mov dx, 40                          ; Width
    mov si, [fdlg_btn_h]               ; Height
    mov di, fdlg_str_open               ; Label
    xor al, al                          ; Not pressed
    call widget_draw_button

    ; Cancel button (right)
    mov bx, [fdlg_cx]
    add bx, 98                          ; X offset in content area
    mov cx, [fdlg_cy]
    add cx, [fdlg_list_h]
    add cx, FDLG_BTN_GAP
    mov dx, 52                          ; Width
    mov si, [fdlg_btn_h]               ; Height
    mov di, fdlg_str_cancel             ; Label
    xor al, al                          ; Not pressed
    call widget_draw_button

    pop word [caller_es]
    call win_end_draw
    popa
    ret

; ============================================================================
; fdlg_scroll_into_view - Adjust scroll to keep selection visible
; ============================================================================
fdlg_scroll_into_view:
    mov ax, [fdlg_sel]
    ; If sel < scroll, scroll = sel
    cmp ax, [fdlg_scroll]
    jae .fscroll_bottom
    mov [fdlg_scroll], ax
    ret
.fscroll_bottom:
    ; If sel >= scroll + fdlg_vis, scroll = sel - fdlg_vis + 1
    mov bx, [fdlg_scroll]
    add bx, [fdlg_vis]
    cmp ax, bx
    jb .fscroll_ok
    sub ax, [fdlg_vis]
    inc ax
    mov [fdlg_scroll], ax
.fscroll_ok:
    ret

; ============================================================================
; Utility API Functions (Build 277)
; ============================================================================

; util_word_to_string - Convert 16-bit word to decimal string (API 91)
; Input:  DX = 16-bit value, DI = destination buffer offset (in caller's DS)
; Output: Null-terminated decimal string at caller_ds:DI, DI advanced past null
; Preserves: AX, BX, CX, DX
util_word_to_string:
    push es
    push ax
    push bx
    push cx
    push dx
    mov ax, [caller_ds]
    mov es, ax
    mov ax, dx                  ; Value to convert
    test ax, ax
    jnz .wts_nonzero
    mov byte [es:di], '0'
    inc di
    jmp .wts_done
.wts_nonzero:
    xor cx, cx                  ; Digit count
    mov bx, 10
.wts_div:
    xor dx, dx
    div bx                     ; AX/10 → AX=quotient, DX=remainder
    push dx                    ; Save digit (on stack above saved regs)
    inc cx
    test ax, ax
    jnz .wts_div
.wts_store:
    pop ax                     ; Pop digits in reverse (correct order)
    add al, '0'
    mov [es:di], al
    inc di
    loop .wts_store
.wts_done:
    mov byte [es:di], 0        ; Null terminate
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    clc
    ret

; util_bcd_to_ascii - Convert BCD byte to ASCII digit pair (API 92)
; Input:  AL = BCD byte (e.g. 0x59)
; Output: AH = tens digit ASCII ('0'-'9'), AL = ones digit ASCII ('0'-'9')
; Preserves: all other registers
util_bcd_to_ascii:
    mov ah, al
    and al, 0x0F               ; Low nibble → ones
    shr ah, 4                  ; High nibble → tens
    add al, '0'
    add ah, '0'
    clc
    ret

; gfx_get_current_font_info - Get current font metrics (API 93)
; Input:  None
; Output: BH = height, BL = width, CL = advance, AL = font index, CF=0
; Preserves: DX, SI, DI
gfx_get_current_font_info:
    mov bh, [draw_font_height]
    mov bl, [draw_font_width]
    mov cl, [draw_font_advance]
    mov al, [current_font]
    clc
    ret

; gfx_draw_sprite - Draw 1-bit transparent sprite (API 94)
; Input: BX=X, CX=Y, DL=height, DH=width(1-8), AL=color(0-3)
;        SI=bitmap offset in caller's DS segment
; Bitmap: 1 byte per row, MSB=leftmost pixel, only set bits draw
; Output: CF=0 success
gfx_draw_sprite:
    pusha
    push es
    ; Save params (DS=0x1000 at entry from INT 0x80 dispatch)
    mov [_spr_color], al
    mov [_spr_height], dl
    mov [_spr_width], dh
    ; Swap to draw convention: BX=Y, CX=X
    xchg bx, cx
    ; ES = video segment
    mov ax, [video_segment]
    mov es, ax
    ; Cursor protection (DS=0x1000)
    call mouse_cursor_hide
    inc byte [cursor_locked]
    ; Draw loop — DS stays 0x1000, switch to caller_ds only for lodsb
    movzx bp, byte [_spr_height]
.spr_row:
    ; Read bitmap byte from caller's segment (brief DS switch)
    push ds
    mov ds, [cs:caller_ds]
    lodsb                           ; AL = bitmap byte from caller_ds:SI
    pop ds                          ; DS back to 0x1000 for plot_pixel_color
    mov ah, al
    push cx                         ; save base X
    movzx di, byte [_spr_width]
.spr_col:
    test ah, 0x80                   ; check leftmost bit
    jz .spr_skip
    push dx
    mov dl, [_spr_color]
    call plot_pixel_color           ; CX=X, BX=Y, DL=color, ES=video segment
    pop dx
.spr_skip:
    shl ah, 1                       ; next bit
    inc cx                          ; next X
    dec di
    jnz .spr_col
    pop cx                          ; restore base X
    inc bx                          ; next Y row
    dec bp
    jnz .spr_row
    ; Restore cursor
    dec byte [cursor_locked]
    call mouse_cursor_show
    pop es
    popa
    clc
    ret

; ============================================================================
; Kernel API Table
; ============================================================================

; Pad to API table alignment
times 0x26A0 - ($ - $$) db 0

kernel_api_table:
    ; Header
    dw 0x4B41                       ; Magic: 'KA' (Kernel API)
    dw 0x0001                       ; Version: 1.0
    dw 64                           ; Number of function slots (0-63)
    dw 0                            ; Reserved for future use

    ; Function Pointers (Offset from table start)
    ; Graphics API (frequent calls - optimize for speed)
    dw gfx_draw_pixel_stub          ; 0: Draw single pixel
    dw gfx_draw_rect_stub           ; 1: Draw rectangle outline
    dw gfx_draw_filled_rect_stub    ; 2: Draw filled rectangle
    dw gfx_draw_char_stub           ; 3: Draw character
    dw gfx_draw_string_stub         ; 4: Draw string (black)
    dw gfx_clear_area_stub          ; 5: Clear rectangular area
    dw gfx_draw_string_inverted     ; 6: Draw string (white)

    ; Memory Management
    dw mem_alloc_stub               ; 7: Allocate memory (malloc)
    dw mem_free_stub                ; 8: Free memory

    ; Event System
    dw event_get_stub               ; 9: Get next event (non-blocking)
    dw event_wait_stub              ; 10: Wait for event (blocking)

    ; Keyboard Input (Foundation 1.4)
    dw kbd_getchar                  ; 11: Get character (non-blocking)
    dw kbd_wait_key                 ; 12: Wait for key (blocking)

    ; Filesystem API (Foundation 1.6)
    dw fs_mount_stub                ; 13: Mount filesystem
    dw fs_open_api                  ; 14: Open file (uses caller_ds)
    dw fs_read_stub                 ; 15: Read from file
    dw fs_close_stub                ; 16: Close file
    dw fs_register_driver_stub      ; 17: Register filesystem driver

    ; Application Loader (Core Services 2.1)
    dw app_load_stub                ; 18: Load application from disk
    dw app_run_stub                 ; 19: Run loaded application

    ; Window Manager (Core Services 2.2)
    dw win_create_stub              ; 20: Create window
    dw win_destroy_stub             ; 21: Destroy window
    dw win_draw_stub                ; 22: Draw/redraw window frame
    dw win_focus_stub               ; 23: Bring window to front
    dw win_move_stub                ; 24: Move window
    dw win_get_content_stub         ; 25: Get content area bounds
    dw register_shell_stub          ; 26: Register app as shell (auto-return)
    dw fs_readdir_stub              ; 27: Read directory entry

    ; Mouse API
    dw mouse_get_state              ; 28: Get mouse position/buttons
    dw mouse_set_position           ; 29: Set mouse position
    dw mouse_is_enabled             ; 30: Check if mouse available

    ; Window Drawing Context API
    dw win_begin_draw               ; 31: Set draw context (AL=window handle)
    dw win_end_draw                 ; 32: Clear draw context (fullscreen mode)

    ; Text Measurement API
    dw gfx_text_width               ; 33: Measure string width in pixels

    ; Cooperative Multitasking API
    dw app_yield_stub               ; 34: Yield CPU to next task
    dw app_start_stub               ; 35: Start task (non-blocking)
    dw app_exit_stub                ; 36: Exit current task

    ; Desktop Icon API (v3.14.0)
    dw desktop_set_icon_stub        ; 37: Register desktop icon
    dw desktop_clear_icons_stub     ; 38: Clear all desktop icons
    dw gfx_draw_icon_stub           ; 39: Draw 16x16 icon bitmap
    dw fs_read_header_stub          ; 40: Read file header bytes

    ; PC Speaker API (v3.15.0)
    dw speaker_tone_stub            ; 41: Play tone (BX=freq Hz, 0=off)
    dw speaker_off_stub             ; 42: Turn off speaker
    dw get_boot_drive_stub          ; 43: Get boot drive (AL=drive number)

    ; Filesystem Write API (v3.19.0)
    dw fs_write_sector_stub         ; 44: Write raw sector to disk
    dw fs_create_stub               ; 45: Create new file
    dw fs_write_stub                ; 46: Write to open file
    dw fs_delete_stub               ; 47: Delete file

    ; GUI Toolkit API (Build 205)
    dw gfx_set_font                 ; 48: Set current font (AL=index)
    dw gfx_get_font_metrics         ; 49: Get font metrics (AL=index)
    dw gfx_draw_string_wrap         ; 50: Draw string with word wrap
    dw widget_draw_button           ; 51: Draw button
    dw widget_draw_radio            ; 52: Draw radio button
    dw widget_hit_test              ; 53: Hit test rectangle

    ; Theme API (Build 208)
    dw theme_set_colors             ; 54: Set theme colors
    dw theme_get_colors             ; 55: Get theme colors

    ; Checkbox widget (Build 226)
    dw widget_draw_checkbox         ; 56: Draw checkbox

    ; Extended Widget Toolkit (Build 235)
    dw widget_draw_textfield        ; 57: Draw text input field
    dw widget_draw_scrollbar        ; 58: Draw scrollbar
    dw widget_draw_listitem         ; 59: Draw list item
    dw widget_draw_progress         ; 60: Draw progress bar
    dw widget_draw_groupbox         ; 61: Draw group box
    dw widget_draw_separator        ; 62: Draw separator line
    dw get_tick_count               ; 63: Get BIOS tick counter
    dw point_over_window            ; 64: Check if point is over a window
    dw widget_draw_combobox         ; 65: Draw combo box
    dw widget_draw_menubar          ; 66: Draw menu bar

    ; Colored Drawing APIs (Build 247)
    dw gfx_draw_filled_rect_color   ; 67: Draw filled rect with color
    dw gfx_draw_rect_color          ; 68: Draw rect outline with color
    dw gfx_draw_hline               ; 69: Draw horizontal line
    dw gfx_draw_vline               ; 70: Draw vertical line
    dw gfx_draw_line                ; 71: Draw line (Bresenham's)

    ; System APIs (Build 247)
    dw get_rtc_time                 ; 72: Read real-time clock
    dw delay_ticks                  ; 73: Delay with yield
    dw get_task_info                ; 74: Get task info

    ; Filesystem APIs (Build 247)
    dw fs_seek_stub                 ; 75: Seek file position
    dw fs_get_file_size_stub        ; 76: Get file size

    ; File Rename API (Build 247)
    dw fs_rename_stub               ; 77: Rename file

    ; Window APIs (Build 247)
    dw win_resize_stub              ; 78: Resize window
    dw win_get_info_stub            ; 79: Get window info

    ; Scroll API (Build 247)
    dw gfx_scroll_area              ; 80: Scroll rectangular area
    dw set_rtc_time                 ; 81: Set real-time clock
    dw get_screen_info              ; 82: Get screen dimensions/mode
    dw get_key_modifiers            ; 83: Get keyboard modifier states

    ; Clipboard APIs (Build 273)
    dw clip_copy                    ; 84: Copy to system clipboard
    dw clip_paste                   ; 85: Paste from system clipboard
    dw clip_get_len                 ; 86: Get clipboard length

    ; Popup Menu APIs (Build 273)
    dw menu_open                    ; 87: Open popup menu (generic)
    dw menu_close                   ; 88: Close popup menu
    dw menu_hit                     ; 89: Hit-test popup menu

    ; File Dialog API (Build 274)
    dw file_dialog_open             ; 90: System file open dialog

    ; Utility APIs (Build 277)
    dw util_word_to_string          ; 91: Convert word to decimal string
    dw util_bcd_to_ascii            ; 92: Convert BCD byte to ASCII pair
    dw gfx_get_current_font_info    ; 93: Get current font metrics

    ; Sprite API (Build 279)
    dw gfx_draw_sprite              ; 94: Draw 1-bit transparent sprite

    ; Video Mode API (Build 281)
    dw set_video_mode               ; 95: Switch video mode at runtime

    ; Content Scale API (Build 309)
    dw win_get_content_scale        ; 96: Get content scale for current draw_context

    ; Window Content Size API (Build 353)
    dw win_get_content_size         ; 97: Get content area dimensions

; ============================================================================
; Graphics API Functions (Foundation 1.2)
; ============================================================================

; gfx_draw_pixel_stub - Draw single pixel (API 0)
; Input: BX = X coordinate (0-319), CX = Y coordinate (0-199), AL = Color (0-3)
; Output: None
; Preserves: All registers
gfx_draw_pixel_stub:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push dx
    mov dx, [cs:video_segment]
    mov es, dx
    xchg bx, cx                    ; plot_pixel_color wants CX=X, BX=Y
    mov dl, al                     ; DL = color (0-3)
    call plot_pixel_color
    xchg bx, cx                    ; Restore BX=X, CX=Y
    pop dx
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

; gfx_draw_char_stub - Draw character
gfx_draw_char_stub:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push dx
    mov word [draw_x], bx
    mov word [draw_y], cx
    mov dx, [video_segment]
    mov es, dx
    sub al, 32
    mov ah, 0
    mov dl, [draw_font_bpc]
    mul dl
    mov si, [draw_font_base]
    add si, ax
    call draw_char
    pop dx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

; gfx_draw_string_stub - Draw null-terminated string
; Uses caller_ds for string access (supports apps calling through INT 0x80)
gfx_draw_string_stub:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push dx
    push di
    push bp
    push ds
    mov word [draw_x], bx
    mov word [draw_y], cx
    mov bp, [caller_ds]             ; BP = caller's segment (0x1000 at boot, app seg via INT 0x80)
    mov dx, [video_segment]
    mov es, dx
    mov ds, bp                      ; DS = caller's segment for string access
.loop:
    lodsb                           ; AL = [DS:SI++] from caller's segment
    test al, al
    jz .done
    push ds                         ; Save caller's DS
    mov bp, 0x1000
    mov ds, bp                      ; DS = kernel for font access
    ; --- Character-level clipping ---
    cmp byte [clip_enabled], 0
    je .no_clip
    ; If draw_y > clip_y2, exit early (past bottom)
    mov di, [draw_y]
    cmp di, [clip_y2]
    ja .clip_exit
    ; If draw_x > clip_x2, skip char (past right edge) but advance
    mov di, [draw_x]
    cmp di, [clip_x2]
    ja .skip_char
    ; If draw_x + font_width < clip_x1, skip char (before left edge)
    xor dx, dx
    mov dl, [draw_font_width]
    add di, dx
    cmp di, [clip_x1]
    jb .skip_char
.no_clip:
    sub al, 32
    mov ah, 0
    mov dl, [draw_font_bpc]
    mul dl
    mov di, si                      ; Save string pointer
    mov si, [draw_font_base]
    add si, ax
    call draw_char
    mov si, di                      ; Restore string pointer
    pop ds                          ; Restore caller's DS
    jmp .loop
.skip_char:
    ; Advance draw_x without drawing
    xor ax, ax
    mov al, [draw_font_advance]
    add [draw_x], ax
    pop ds
    jmp .loop
.clip_exit:
    pop ds                          ; Balance stack from push ds
.done:
    pop ds
    pop bp
    pop di
    pop dx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

; ============================================================================
; gfx_set_font - Set current font (API 48)
; Input: AL = font index (0=4x6, 1=8x8, 2=8x12)
; Output: CF=0 on success, CF=1 if invalid index
; ============================================================================
gfx_set_font:
    cmp al, FONT_COUNT
    jae .bad_font
    push bx
    push si
    mov [current_font], al
    ; Propagate font to ALL active tasks so per-task restore never reverts it
    push cx
    push di
    mov di, app_table
    mov cx, APP_MAX_COUNT
.update_all_tasks:
    cmp byte [di + APP_OFF_STATE], APP_STATE_FREE
    je .skip_task
    mov [di + APP_OFF_FONT], al
.skip_task:
    add di, APP_ENTRY_SIZE
    dec cx
    jnz .update_all_tasks
    pop di
    pop cx
    ; Calculate font_table offset: index * FONT_DESC_SIZE
    mov bl, al
    xor bh, bh
    mov al, FONT_DESC_SIZE
    mul bl                          ; AX = index * 6
    mov si, font_table
    add si, ax
    ; Load descriptor into working variables
    mov ax, [si]                    ; Font data pointer
    mov [draw_font_base], ax
    mov al, [si + 2]               ; Height
    mov [draw_font_height], al
    mov al, [si + 3]               ; Width
    mov [draw_font_width], al
    mov al, [si + 4]               ; Advance
    mov [draw_font_advance], al
    mov al, [si + 5]               ; Bytes per char
    mov [draw_font_bpc], al
    pop si
    pop bx
    clc
    ret
.bad_font:
    stc
    ret

; ============================================================================
; gfx_get_font_metrics - Get metrics for a font (API 49)
; Input: AL = font index (0=4x6, 1=8x8, 2=8x12)
; Output: BL=width, BH=height, CL=advance, CF=0
;         CF=1 if invalid index
; ============================================================================
gfx_get_font_metrics:
    cmp al, FONT_COUNT
    jae .bad_idx
    push si
    mov bl, al
    xor bh, bh
    push ax
    mov al, FONT_DESC_SIZE
    mul bl
    mov si, font_table
    add si, ax
    pop ax
    mov bh, [si + 2]               ; Height
    mov bl, [si + 3]               ; Width
    mov cl, [si + 4]               ; Advance
    pop si
    clc
    ret
.bad_idx:
    stc
    ret

; ============================================================================
; plot_pixel_color - Plot pixel with arbitrary color
; Input: CX = X (0-319), BX = Y (0-199), DL = color (0-3 CGA, 0-255 VGA)
;        ES = video segment
; Preserves all registers except flags
; ============================================================================
plot_pixel_color:
    cmp cx, [cs:screen_width]
    jae .ppc_out
    cmp bx, [cs:screen_height]
    jae .ppc_out
    cmp byte [cs:video_mode], 0x01
    je .ppc_vesa
    cmp byte [cs:video_mode], 0x12
    je .ppc_mode12h
    cmp byte [cs:video_mode], 0x13
    je .ppc_vga
    push ax
    push bx
    push cx
    push di
    push dx
    call cga_pixel_calc
    ; Read-modify-write: clear old 2 bits, OR in color
    mov al, [es:di]                ; Read current byte
    mov ah, 0x03
    shl ah, cl                     ; AH = mask for our pixel's 2 bits
    not ah
    and al, ah                     ; Clear old pixel
    ; Get color from stack (DL was pushed last as part of DX)
    mov bx, sp
    mov ah, [ss:bx]                ; AH = saved DL (color) from pushed DX
    and ah, 0x03                   ; Ensure 2-bit color
    shl ah, cl                     ; Shift color into position
    or al, ah                      ; Set new color
    mov [es:di], al                ; Write back
    pop dx
    pop di
    pop cx
    pop bx
    pop ax
.ppc_out:
    ret
.ppc_vga:
    push ax
    push di
    push dx                            ; Save DL (color) before mul clobbers DX
    mov ax, bx
    mul word [cs:screen_pitch]         ; AX = Y * pitch
    add ax, cx                         ; AX = Y * pitch + X
    mov di, ax
    pop dx                             ; Restore DL (color)
    mov [es:di], dl                    ; Write color byte directly
    pop di
    pop ax
    ret
.ppc_mode12h:
    jmp mode12h_plot_pixel             ; Tail call (preserves all regs)
.ppc_vesa:
    jmp vesa_plot_pixel                ; Tail call (preserves all regs)

; ============================================================================
; gfx_draw_string_wrap - Draw string with word wrapping (API 50)
; Input: BX=X, CX=Y, DX=wrap_width, SI=string (caller_ds)
; Output: CX=final Y after last line
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
gfx_draw_string_wrap:
    ; Save all registers we'll modify
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push dx
    push di
    push bp
    push ds
    ; BX=X (start), CX=Y, DX=wrap_width, SI=string
    mov word [draw_x], bx
    mov word [draw_y], cx
    mov [wrap_start_x], bx         ; Remember starting X for line breaks
    mov [wrap_width], dx            ; Remember wrap width
    mov bp, [caller_ds]
    mov dx, [video_segment]
    mov es, dx
    mov ds, bp                      ; DS = caller's segment
.wrap_loop:
    lodsb
    test al, al
    jz .wrap_done
    cmp al, 10                      ; Newline?
    je .wrap_newline
    ; Check if character would exceed wrap boundary
    push ds
    mov bp, 0x1000
    mov ds, bp
    mov di, [draw_x]
    xor dx, dx
    mov dl, [draw_font_advance]
    add di, dx                      ; DI = draw_x after this char
    mov dx, [wrap_start_x]
    add dx, [wrap_width]            ; DX = right edge
    cmp di, dx
    jbe .wrap_draw                  ; Fits on this line
    ; Line break: reset X, advance Y
    mov dx, [wrap_start_x]
    mov [draw_x], dx
    xor dx, dx
    mov dl, [draw_font_height]
    add dx, 2                       ; Line spacing
    add [draw_y], dx
    pop ds
    dec si                          ; Re-process this character on new line
    jmp .wrap_loop
.wrap_draw:
    ; Draw the character (DS=kernel here)
    sub al, 32
    mov ah, 0
    mov dl, [draw_font_bpc]
    mul dl
    mov di, si
    mov si, [draw_font_base]
    add si, ax
    call draw_char
    mov si, di
    pop ds
    jmp .wrap_loop
.wrap_newline:
    ; Force line break
    push ds
    mov bp, 0x1000
    mov ds, bp
    mov dx, [wrap_start_x]
    mov [draw_x], dx
    xor dx, dx
    mov dl, [draw_font_height]
    add dx, 2
    add [draw_y], dx
    pop ds
    jmp .wrap_loop
.wrap_done:
    ; Return final Y in CX
    push ds
    mov bp, 0x1000
    mov ds, bp
    mov cx, [draw_y]
    pop ds
    pop ds
    pop bp
    pop di
    pop dx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; ============================================================================
; 3D Bevel Helper Functions (used by widgets when widget_style=1)
; ============================================================================

; draw_raised_bevel - Draw raised 3D bevel edges
; Input: BX=X, CX=Y, DX=width, SI=height
; Preserves all registers
draw_raised_bevel:
    push ax
    mov [.rb_x], bx
    mov [.rb_y], cx
    mov [.rb_w], dx
    mov [.rb_h], si
    ; Top: highlight
    mov al, SYS_BTN_HILIGHT
    call gfx_draw_hline
    ; Left: highlight
    mov bx, [.rb_x]
    mov cx, [.rb_y]
    mov dx, [.rb_h]
    call gfx_draw_vline
    ; Bottom: shadow
    mov bx, [.rb_x]
    mov cx, [.rb_y]
    add cx, [.rb_h]
    dec cx
    mov dx, [.rb_w]
    mov al, SYS_BTN_SHADOW
    call gfx_draw_hline
    ; Right: shadow
    mov bx, [.rb_x]
    add bx, [.rb_w]
    dec bx
    mov cx, [.rb_y]
    mov dx, [.rb_h]
    call gfx_draw_vline
    ; Restore registers
    mov bx, [.rb_x]
    mov cx, [.rb_y]
    mov dx, [.rb_w]
    mov si, [.rb_h]
    pop ax
    ret
.rb_x: dw 0
.rb_y: dw 0
.rb_w: dw 0
.rb_h: dw 0

; draw_sunken_bevel - Draw sunken 3D bevel edges
; Input: BX=X, CX=Y, DX=width, SI=height
; Preserves all registers
draw_sunken_bevel:
    push ax
    mov [.sb_x], bx
    mov [.sb_y], cx
    mov [.sb_w], dx
    mov [.sb_h], si
    ; Top: shadow
    mov al, SYS_BTN_SHADOW
    call gfx_draw_hline
    ; Left: shadow
    mov bx, [.sb_x]
    mov cx, [.sb_y]
    mov dx, [.sb_h]
    call gfx_draw_vline
    ; Bottom: highlight
    mov bx, [.sb_x]
    mov cx, [.sb_y]
    add cx, [.sb_h]
    dec cx
    mov dx, [.sb_w]
    mov al, SYS_BTN_HILIGHT
    call gfx_draw_hline
    ; Right: highlight
    mov bx, [.sb_x]
    add bx, [.sb_w]
    dec bx
    mov cx, [.sb_y]
    mov dx, [.sb_h]
    call gfx_draw_vline
    ; Restore registers
    mov bx, [.sb_x]
    mov cx, [.sb_y]
    mov dx, [.sb_w]
    mov si, [.sb_h]
    pop ax
    ret
.sb_x: dw 0
.sb_y: dw 0
.sb_w: dw 0
.sb_h: dw 0

; ============================================================================
; widget_draw_button - Draw a clickable button (API 51)
; Input: BX=X, CX=Y, DX=width, SI=height, DI=label (caller_es:DI)
;        AL=flags (bit 0: pressed)
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
widget_draw_button:
    ; BX=X, CX=Y, DX=width, SI=height, DI=label (caller_es:DI), AL=flags
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    mov [btn_flags], al
    mov [btn_x], bx
    mov [btn_y], cx
    mov [btn_w], dx
    mov [btn_h], si
    mov ax, [video_segment]
    mov es, ax

    cmp byte [widget_style], 0
    je .btn_flat

    ; === 3D BUTTON ===
    ; Fill with button face color
    mov al, SYS_BTN_FACE
    call gfx_draw_filled_rect_color
    ; Draw bevel
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, [btn_w]
    mov si, [btn_h]
    test byte [btn_flags], 1
    jnz .btn_3d_pressed
    call draw_raised_bevel
    jmp .btn_3d_fg
.btn_3d_pressed:
    call draw_sunken_bevel
.btn_3d_fg:
    ; Set black foreground for label text, button face background
    mov byte [draw_fg_color], 0
    mov byte [draw_bg_color], SYS_BTN_FACE
    jmp .btn_label

.btn_flat:
    ; === FLAT BUTTON ===
    push ax
    mov al, [win_color]
    mov [draw_fg_color], al
    pop ax
    call gfx_draw_filled_rect_stub
    ; Draw border rect
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, [btn_w]
    mov si, [btn_h]
    call gfx_draw_rect_stub
    ; If pressed, draw inset border
    test byte [btn_flags], 1
    jz .btn_label
    mov bx, [btn_x]
    inc bx
    mov cx, [btn_y]
    inc cx
    mov dx, [btn_w]
    sub dx, 2
    mov si, [btn_h]
    sub si, 2
    call gfx_draw_rect_stub
.btn_label:
    ; Save original caller_ds, set caller_ds = caller_es for label access
    mov ax, [caller_ds]
    mov [btn_saved_cds], ax
    mov ax, [caller_es]
    mov [caller_ds], ax
    ; Measure label width
    mov si, di                      ; SI = label pointer
    call gfx_text_width             ; DX = label width
    ; Center horizontally: x = btn_x + (btn_w - text_width) / 2
    mov bx, [btn_w]
    cmp bx, dx
    jb .btn_left_align             ; Text wider than button: left-align
    sub bx, dx
    shr bx, 1
    add bx, [btn_x]
    jmp .btn_x_done
.btn_left_align:
    mov bx, [btn_x]
    add bx, 2                     ; Small left padding
.btn_x_done:
    ; Center vertically: y = btn_y + (btn_h - font_height) / 2
    xor cx, cx
    mov cl, [draw_font_height]
    mov ax, [btn_h]
    sub ax, cx
    shr ax, 1
    add ax, [btn_y]
    mov cx, ax
    ; If pressed, offset text by 1px
    test byte [btn_flags], 1
    jz .btn_draw_label
    inc bx
    inc cx
.btn_draw_label:
    ; Set clip bounds to button rect so text doesn't overflow
    push word [clip_x1]
    push word [clip_x2]
    push word [clip_y1]
    push word [clip_y2]
    push word [clip_enabled]
    mov ax, [btn_x]
    mov [clip_x1], ax
    add ax, [btn_w]
    dec ax
    mov [clip_x2], ax
    mov ax, [btn_y]
    mov [clip_y1], ax
    add ax, [btn_h]
    dec ax
    mov [clip_y2], ax
    mov byte [clip_enabled], 1
    ; Draw label text
    cmp byte [widget_style], 0
    je .btn_flat_text
    call gfx_draw_string_stub          ; 3D: black text (draw_fg_color=0)
    jmp .btn_text_done
.btn_flat_text:
    call gfx_draw_string_inverted      ; Flat: inverted (black on white)
.btn_text_done:
    ; Restore clip state
    pop word [clip_enabled]
    pop word [clip_y2]
    pop word [clip_y1]
    pop word [clip_x2]
    pop word [clip_x1]
    ; Restore caller_ds
    mov ax, [btn_saved_cds]
    mov [caller_ds], ax
    ; Restore text_color as foreground, black background
    mov al, [text_color]
    mov [draw_fg_color], al
    mov byte [draw_bg_color], 0
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; ============================================================================
; widget_draw_radio - Draw a radio button (API 52)
; Input: BX=X, CX=Y, SI=label (caller_ds), AL=flags (bit 0: selected)
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
widget_draw_radio:
    ; BX=X, CX=Y, SI=label (caller_ds), AL=flags (bit 0: selected)
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    mov [btn_flags], al
    mov [btn_x], bx
    mov [btn_y], cx
    mov ax, [video_segment]
    mov es, ax
    ; Draw radio circle using draw_char (8 rows, 8 cols)
    mov word [draw_x], bx
    mov word [draw_y], cx
    ; Save and set font params for 8x8 bitmap
    mov al, [draw_font_height]
    mov [btn_saved_fh], al
    mov al, [draw_font_width]
    mov [btn_saved_fw], al
    mov al, [draw_font_advance]
    mov [btn_saved_fa], al
    mov byte [draw_font_height], 8
    mov byte [draw_font_width], 8
    mov byte [draw_font_advance], 10  ; Advance past radio circle
    mov si, radio_empty_bitmap
    test byte [btn_flags], 1
    jz .radio_draw
    mov si, radio_filled_bitmap
.radio_draw:
    call draw_char
    ; Restore font params
    mov al, [btn_saved_fh]
    mov [draw_font_height], al
    mov al, [btn_saved_fw]
    mov [draw_font_width], al
    mov al, [btn_saved_fa]
    mov [draw_font_advance], al
    ; Draw label string at (btn_x + 12, btn_y)
    pop si                          ; Restore label pointer (SI)
    mov bx, [btn_x]
    add bx, 12
    mov cx, [btn_y]
    call gfx_draw_string_stub
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; Radio button bitmaps (8x8)
radio_empty_bitmap:
    db 0b00111100                   ; ..XXXX..
    db 0b01000010                   ; .X....X.
    db 0b10000001                   ; X......X
    db 0b10000001                   ; X......X
    db 0b10000001                   ; X......X
    db 0b10000001                   ; X......X
    db 0b01000010                   ; .X....X.
    db 0b00111100                   ; ..XXXX..

radio_filled_bitmap:
    db 0b00111100                   ; ..XXXX..
    db 0b01000010                   ; .X....X.
    db 0b10011001                   ; X..XX..X
    db 0b10111101                   ; X.XXXX.X
    db 0b10111101                   ; X.XXXX.X
    db 0b10011001                   ; X..XX..X
    db 0b01000010                   ; .X....X.
    db 0b00111100                   ; ..XXXX..

; ============================================================================
; widget_draw_checkbox - Draw a checkbox (API 56)
; Input: BX=X, CX=Y, SI=label (caller_ds), AL=flags (bit 0: checked)
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
widget_draw_checkbox:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    mov [btn_flags], al
    mov [btn_x], bx
    mov [btn_y], cx
    mov ax, [video_segment]
    mov es, ax
    ; Draw checkbox using draw_char (8x8 bitmap)
    mov word [draw_x], bx
    mov word [draw_y], cx
    ; Save and set font params for 8x8 bitmap
    mov al, [draw_font_height]
    mov [btn_saved_fh], al
    mov al, [draw_font_width]
    mov [btn_saved_fw], al
    mov al, [draw_font_advance]
    mov [btn_saved_fa], al
    mov byte [draw_font_height], 8
    mov byte [draw_font_width], 8
    mov byte [draw_font_advance], 10
    mov si, checkbox_empty_bitmap
    test byte [btn_flags], 1
    jz .chk_draw
    mov si, checkbox_checked_bitmap
.chk_draw:
    call draw_char
    ; Restore font params
    mov al, [btn_saved_fh]
    mov [draw_font_height], al
    mov al, [btn_saved_fw]
    mov [draw_font_width], al
    mov al, [btn_saved_fa]
    mov [draw_font_advance], al
    ; Draw label string at (btn_x + 12, btn_y)
    pop si                          ; Restore label pointer (SI)
    mov bx, [btn_x]
    add bx, 12
    mov cx, [btn_y]
    call gfx_draw_string_stub
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; Checkbox bitmaps (8x8)
checkbox_empty_bitmap:
    db 0b11111111                   ; XXXXXXXX
    db 0b10000001                   ; X......X
    db 0b10000001                   ; X......X
    db 0b10000001                   ; X......X
    db 0b10000001                   ; X......X
    db 0b10000001                   ; X......X
    db 0b10000001                   ; X......X
    db 0b11111111                   ; XXXXXXXX

checkbox_checked_bitmap:
    db 0b11111111                   ; XXXXXXXX
    db 0b11000011                   ; XX....XX
    db 0b10100101                   ; X.X..X.X
    db 0b10011001                   ; X..XX..X
    db 0b10011001                   ; X..XX..X
    db 0b10100101                   ; X.X..X.X
    db 0b11000011                   ; XX....XX
    db 0b11111111                   ; XXXXXXXX

; ============================================================================
; widget_draw_separator - Draw a separator line (API 62)
; Input: BX=X, CX=Y, DX=length, AL=flags (bit 0: vertical, else horizontal)
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
widget_draw_separator:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    mov [btn_flags], al
    mov ax, [video_segment]
    mov es, ax
    ; Swap BX/CX: plot_pixel_white wants CX=X, BX=Y
    xchg bx, cx                    ; Now BX=Y, CX=X
    mov di, dx                     ; DI = length counter
    test byte [btn_flags], 1
    jnz .sep_vert
.sep_horiz:
    call plot_pixel_white
    inc cx
    dec di
    jnz .sep_horiz
    jmp .sep_done
.sep_vert:
    call plot_pixel_white
    inc bx
    dec di
    jnz .sep_vert
.sep_done:
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; ============================================================================
; widget_draw_listitem - Draw a list item row (API 59)
; Input: BX=X, CX=Y, DX=width, SI=text_ptr (caller_ds), AL=flags
;        bit 0: selected (inverted colors)
;        bit 1: cursor (draw left-edge marker)
; Height: font_height (auto)
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
widget_draw_listitem:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    mov [btn_flags], al
    mov [btn_x], bx
    mov [btn_y], cx
    mov [btn_w], dx
    mov [wgt_text_ptr], si          ; Save text pointer to variable
    ; Height = current font height
    xor ah, ah
    mov al, [draw_font_height]
    mov [btn_h], ax
    mov ax, [video_segment]
    mov es, ax
    test byte [btn_flags], 1
    jz .li_normal
    ; Selected: draw filled rect background, then inverted text
    mov si, [btn_h]
    call gfx_draw_filled_rect_stub  ; BX=X, CX=Y, DX=W, SI=H
    ; Set clip bounds to item rect
    push word [clip_x1]
    push word [clip_x2]
    push word [clip_y1]
    push word [clip_y2]
    push word [clip_enabled]
    mov ax, [btn_x]
    mov [clip_x1], ax
    add ax, [btn_w]
    dec ax
    mov [clip_x2], ax
    mov ax, [btn_y]
    mov [clip_y1], ax
    add ax, [btn_h]
    dec ax
    mov [clip_y2], ax
    mov byte [clip_enabled], 1
    ; Draw inverted (black) text
    mov si, [wgt_text_ptr]
    mov bx, [btn_x]
    add bx, 2                      ; 2px left padding
    mov cx, [btn_y]
    call gfx_draw_string_inverted
    ; Restore clip state
    pop word [clip_enabled]
    pop word [clip_y2]
    pop word [clip_y1]
    pop word [clip_x2]
    pop word [clip_x1]
    jmp .li_cursor
.li_normal:
    ; Normal: clear area, then draw normal text
    mov si, [btn_h]
    call gfx_clear_area_stub        ; BX=X, CX=Y, DX=W, SI=H
    mov si, [wgt_text_ptr]
    mov bx, [btn_x]
    add bx, 2                      ; 2px left padding
    mov cx, [btn_y]
    call gfx_draw_string_stub
.li_cursor:
    ; Draw cursor marker if bit 1 set
    test byte [btn_flags], 2
    jz .li_done
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, 1                       ; 1px wide
    xor ax, ax
    mov al, [draw_font_height]
    mov si, ax                      ; Height = font_height
    test byte [btn_flags], 1
    jnz .li_cursor_inv
    call gfx_draw_filled_rect_stub  ; White bar on black background
    jmp .li_done
.li_cursor_inv:
    call gfx_clear_area_stub        ; Black bar on white selected background
.li_done:
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; ============================================================================
; widget_draw_progress - Draw a progress bar (API 60)
; Input: BX=X, CX=Y, DX=width, SI=value (0-100), AL=flags
;        bit 0: show percentage text centered on bar
; Height: 8px fixed
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
PROGRESS_HEIGHT equ 8

widget_draw_progress:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    mov [btn_flags], al
    mov [btn_x], bx
    mov [btn_y], cx
    mov [btn_w], dx
    ; Clamp value to 0-100
    cmp si, 100
    jbe .prog_val_ok
    mov si, 100
.prog_val_ok:
    mov [wgt_scratch], si           ; Store value (0-100)
    mov ax, [video_segment]
    mov es, ax
    ; Draw progress bar frame
    mov si, PROGRESS_HEIGHT
    cmp byte [widget_style], 0
    je .prog_flat_frame
    ; 3D: face fill + sunken bevel
    mov al, SYS_BTN_FACE
    call gfx_draw_filled_rect_color
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, [btn_w]
    mov si, PROGRESS_HEIGHT
    call draw_sunken_bevel
    jmp .prog_frame_done
.prog_flat_frame:
    call gfx_clear_area_stub
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, [btn_w]
    mov si, PROGRESS_HEIGHT
    call gfx_draw_rect_stub
.prog_frame_done:
    ; Calculate fill width: (value * (width-2)) / 100
    mov ax, [wgt_scratch]           ; AX = value (0-100)
    mov dx, [btn_w]
    sub dx, 2                       ; DX = inner width
    mul dx                          ; DX:AX = value * inner_width
    mov cx, 100
    div cx                          ; AX = fill width in pixels
    mov [wgt_cursor_pos], ax        ; Save fill width for text clipping
    ; Draw filled portion
    test ax, ax
    jz .prog_no_fill
    mov dx, ax                      ; DX = fill width
    mov bx, [btn_x]
    inc bx                          ; Inside left border
    mov cx, [btn_y]
    inc cx                          ; Inside top border
    mov si, PROGRESS_HEIGHT - 2     ; Inner height
    call gfx_draw_filled_rect_stub
.prog_no_fill:
    ; Optionally draw percentage text
    test byte [btn_flags], 1
    jz .prog_done
    ; Build percentage string: "NN%" or "100%"
    mov ax, [wgt_scratch]           ; AX = value
    cmp ax, 100
    jne .prog_not_100
    mov byte [prog_str_buf], '1'
    mov byte [prog_str_buf+1], '0'
    mov byte [prog_str_buf+2], '0'
    mov byte [prog_str_buf+3], '%'
    mov byte [prog_str_buf+4], 0
    jmp .prog_draw_text
.prog_not_100:
    xor dx, dx
    mov cx, 10
    div cx                          ; AX = tens, DX = ones
    test ax, ax
    jz .prog_ones_only
    add al, '0'
    mov [prog_str_buf], al
    add dl, '0'
    mov [prog_str_buf+1], dl
    mov byte [prog_str_buf+2], '%'
    mov byte [prog_str_buf+3], 0
    jmp .prog_draw_text
.prog_ones_only:
    add dl, '0'
    mov [prog_str_buf], dl
    mov byte [prog_str_buf+1], '%'
    mov byte [prog_str_buf+2], 0
.prog_draw_text:
    ; Measure text width (string in kernel segment)
    mov ax, [caller_ds]
    mov [btn_saved_cds], ax
    mov word [caller_ds], 0x1000
    mov si, prog_str_buf
    call gfx_text_width             ; DX = text width
    ; Center: x = btn_x + (btn_w - text_width) / 2
    mov bx, [btn_w]
    sub bx, dx
    shr bx, 1
    add bx, [btn_x]
    mov [wgt_text_ptr], bx          ; Save text X for second pass
    ; y = btn_y + (PROGRESS_HEIGHT - font_height) / 2
    xor cx, cx
    mov cl, [draw_font_height]
    mov ax, PROGRESS_HEIGHT
    sub ax, cx
    shr ax, 1
    add ax, [btn_y]
    mov cx, ax
    mov [wgt_scratch], cx           ; Save text Y for second pass
    ; Save clip state
    push word [clip_x1]
    push word [clip_x2]
    push word [clip_y1]
    push word [clip_y2]
    push word [clip_enabled]
    ; Clip Y to bar interior
    mov ax, [btn_y]
    inc ax
    mov [clip_y1], ax
    mov ax, [btn_y]
    add ax, PROGRESS_HEIGHT - 2
    mov [clip_y2], ax
    mov byte [clip_enabled], 1
    ; --- Pass 1: inverted (black) text over filled area ---
    mov ax, [wgt_cursor_pos]        ; fill_width
    test ax, ax
    jz .prog_skip_inverted
    mov dx, [btn_x]
    inc dx
    mov [clip_x1], dx               ; Left edge of fill
    add dx, ax
    dec dx                          ; Right edge of fill (inclusive)
    mov [clip_x2], dx
    mov si, prog_str_buf
    mov bx, [wgt_text_ptr]
    mov cx, [wgt_scratch]
    call gfx_draw_string_inverted   ; Black text on filled area
.prog_skip_inverted:
    ; --- Pass 2: normal (white) text over unfilled area ---
    mov ax, [btn_x]
    inc ax
    add ax, [wgt_cursor_pos]        ; AX = right edge of fill + 1
    mov dx, [btn_x]
    add dx, [btn_w]
    sub dx, 2                       ; DX = right edge of bar interior
    cmp ax, dx
    ja .prog_skip_normal            ; Fill covers entire bar, no unfilled area
    mov [clip_x1], ax
    mov [clip_x2], dx
    mov si, prog_str_buf
    mov bx, [wgt_text_ptr]
    mov cx, [wgt_scratch]
    call gfx_draw_string_stub       ; White text on unfilled area
.prog_skip_normal:
    ; Restore clip state
    pop word [clip_enabled]
    pop word [clip_y2]
    pop word [clip_y1]
    pop word [clip_x2]
    pop word [clip_x1]
    ; Restore caller_ds
    mov ax, [btn_saved_cds]
    mov [caller_ds], ax
.prog_done:
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; Progress bar string buffer
prog_str_buf: db '100%', 0, 0

; ============================================================================
; widget_draw_groupbox - Draw a group box with label (API 61)
; Input: BX=X, CX=Y, DX=width, SI=height, DI=label_ptr (caller_es), AL=flags
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
widget_draw_groupbox:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    mov [btn_flags], al
    mov [btn_x], bx
    mov [btn_y], cx
    mov [btn_w], dx
    mov [btn_h], si
    mov [wgt_text_ptr], di          ; Save label pointer
    mov ax, [video_segment]
    mov es, ax
    ; Border starts font_height/2 below Y, so label sits on the top edge
    xor ax, ax
    mov al, [draw_font_height]
    shr al, 1
    mov [wgt_scratch], ax           ; half_fh
    ; Draw border at (X, Y+half_fh) with (width, height-half_fh)
    mov bx, [btn_x]
    mov cx, [btn_y]
    add cx, [wgt_scratch]
    mov dx, [btn_w]
    mov si, [btn_h]
    sub si, [wgt_scratch]
    cmp byte [widget_style], 0
    je .gb_flat_border
    call draw_sunken_bevel           ; 3D: etched border
    jmp .gb_border_done
.gb_flat_border:
    call gfx_draw_rect_stub          ; Flat: white outline
.gb_border_done:
    ; Measure label to clear gap behind it
    mov ax, [caller_ds]
    mov [btn_saved_cds], ax
    mov ax, [caller_es]
    mov [caller_ds], ax             ; caller_ds = label segment
    mov si, [wgt_text_ptr]
    call gfx_text_width             ; DX = label width
    ; Clear area at (X+6, Y) size (label_width+4, font_height)
    mov bx, [btn_x]
    add bx, 6
    mov cx, [btn_y]
    add dx, 4
    xor ax, ax
    mov al, [draw_font_height]
    mov si, ax
    call gfx_clear_area_stub
    ; Draw label text at (X+8, Y)
    mov si, [wgt_text_ptr]
    mov bx, [btn_x]
    add bx, 8
    mov cx, [btn_y]
    call gfx_draw_string_stub
    ; Restore caller_ds
    mov ax, [btn_saved_cds]
    mov [caller_ds], ax
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; ============================================================================
; widget_draw_textfield - Draw a text input field (API 57)
; Input: BX=X, CX=Y, DX=width, SI=text_ptr (caller_ds), DI=cursor_pos
;        AL=flags: bit 0=focused (show cursor), bit 1=password mode (dots)
; Height: font_height + 4 (auto)
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
widget_draw_textfield:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    mov [btn_flags], al
    mov [btn_x], bx
    mov [btn_y], cx
    mov [btn_w], dx
    mov [wgt_cursor_pos], di
    mov [wgt_text_ptr], si          ; Save text pointer
    ; Field height = font_height + 4
    xor ah, ah
    mov al, [draw_font_height]
    add ax, 4
    mov [btn_h], ax
    mov ax, [video_segment]
    mov es, ax
    ; Clear area and draw border
    mov si, [btn_h]
    cmp byte [widget_style], 0
    je .tf_flat_border
    ; 3D: white interior + sunken bevel
    mov al, 3                          ; White
    call gfx_draw_filled_rect_color
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, [btn_w]
    mov si, [btn_h]
    call draw_sunken_bevel
    jmp .tf_border_done
.tf_flat_border:
    call gfx_clear_area_stub
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, [btn_w]
    mov si, [btn_h]
    call gfx_draw_rect_stub
.tf_border_done:
    ; Set clipping to field interior
    push word [clip_x1]
    push word [clip_x2]
    push word [clip_y1]
    push word [clip_y2]
    push word [clip_enabled]
    mov ax, [btn_x]
    inc ax
    mov [clip_x1], ax
    mov ax, [btn_x]
    add ax, [btn_w]
    sub ax, 2
    mov [clip_x2], ax
    mov ax, [btn_y]
    inc ax
    mov [clip_y1], ax
    mov ax, [btn_y]
    add ax, [btn_h]
    sub ax, 2
    mov [clip_y2], ax
    mov byte [clip_enabled], 1
    ; Draw text at (X+2, Y+2)
    ; For 3D mode, set text fg=black, bg=white to match white interior
    cmp byte [widget_style], 0
    je .tf_colors_done
    mov byte [draw_fg_color], 0
    mov byte [draw_bg_color], 3
.tf_colors_done:
    mov si, [wgt_text_ptr]
    mov bx, [btn_x]
    add bx, 2
    mov cx, [btn_y]
    add cx, 2
    test byte [btn_flags], 2
    jnz .tf_password
    call gfx_draw_string_stub
    jmp .tf_cursor
.tf_password:
    ; Count string length in caller segment
    mov ax, [caller_ds]
    mov [btn_saved_cds], ax
    push ds
    mov ds, ax
    xor cx, cx
.tf_count:
    lodsb
    test al, al
    jz .tf_count_done
    inc cx
    jmp .tf_count
.tf_count_done:
    pop ds
    ; Draw CX dots
    mov word [caller_ds], 0x1000
    mov [wgt_scratch], cx
    mov bx, [btn_x]
    add bx, 2
    xor di, di
.tf_draw_dots:
    cmp di, [wgt_scratch]
    jge .tf_dots_done
    mov si, tf_dot_char
    mov cx, [btn_y]
    add cx, 2
    call gfx_draw_string_stub
    xor ax, ax
    mov al, [draw_font_advance]
    add bx, ax
    inc di
    jmp .tf_draw_dots
.tf_dots_done:
    mov ax, [btn_saved_cds]
    mov [caller_ds], ax
.tf_cursor:
    ; Draw cursor if focused
    test byte [btn_flags], 1
    jz .tf_no_cursor
    ; Cursor X = btn_x + 2 + cursor_pos * font_advance
    xor ax, ax
    mov al, [draw_font_advance]
    mul word [wgt_cursor_pos]
    add ax, [btn_x]
    add ax, 2
    ; Draw 1px wide filled rect for cursor (much faster than pixel loop)
    mov bx, ax                         ; BX = cursor X
    mov cx, [btn_y]
    add cx, 2                          ; CX = cursor Y
    mov dx, 1                          ; DX = width (1px)
    xor ax, ax
    mov al, [draw_font_height]
    mov si, ax                          ; SI = height
    call gfx_draw_filled_rect_stub
.tf_no_cursor:
    ; Restore draw colors if 3D mode changed them
    cmp byte [widget_style], 0
    je .tf_restore_clip
    mov al, [text_color]
    mov [draw_fg_color], al
    mov byte [draw_bg_color], 0
.tf_restore_clip:
    ; Restore clip state
    pop word [clip_enabled]
    pop word [clip_y2]
    pop word [clip_y1]
    pop word [clip_x2]
    pop word [clip_x1]
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

tf_dot_char: db '*', 0

; ============================================================================
; widget_draw_scrollbar - Draw a scrollbar (API 58)
; Input: BX=X, CX=Y, SI=track_height, DX=position, DI=max_range, AL=flags
;        bit 0: horizontal (else vertical)
; Width: 8px (vertical)
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
SCROLLBAR_WIDTH equ 8
SCROLLBAR_ARROW_H equ 8
SCROLLBAR_MIN_THUMB equ 4

widget_draw_scrollbar:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    mov [btn_flags], al
    mov [btn_x], bx
    mov [btn_y], cx
    mov [btn_w], si                 ; Track height
    ; Clamp position to max_range
    cmp dx, di
    jbe .sb_pos_ok
    mov dx, di
.sb_pos_ok:
    mov [btn_h], dx                 ; Position
    mov [wgt_scratch], di           ; Max range
    mov ax, [video_segment]
    mov es, ax
    ; Draw track
    mov dx, SCROLLBAR_WIDTH
    mov si, [btn_w]
    cmp byte [widget_style], 0
    je .sb_flat_track
    ; 3D: face fill + sunken bevel
    mov al, SYS_BTN_FACE
    call gfx_draw_filled_rect_color
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, SCROLLBAR_WIDTH
    mov si, [btn_w]
    call draw_sunken_bevel
    jmp .sb_track_done
.sb_flat_track:
    call gfx_clear_area_stub
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, SCROLLBAR_WIDTH
    mov si, [btn_w]
    call gfx_draw_rect_stub
.sb_track_done:
    ; Draw up arrow bitmap at top
    mov bx, [btn_x]
    mov word [draw_x], bx
    mov cx, [btn_y]
    mov word [draw_y], cx
    ; Save and set font params for 8x8 bitmap
    mov al, [draw_font_height]
    mov [btn_saved_fh], al
    mov al, [draw_font_width]
    mov [btn_saved_fw], al
    mov al, [draw_font_advance]
    mov [btn_saved_fa], al
    mov byte [draw_font_height], 8
    mov byte [draw_font_width], 8
    mov byte [draw_font_advance], 8
    ; Set bg color to match scrollbar face for 3D mode
    cmp byte [widget_style], 0
    je .sb_arrow_draw
    mov byte [draw_bg_color], SYS_BTN_FACE
.sb_arrow_draw:
    mov si, scrollbar_up_bitmap
    call draw_char
    ; Draw down arrow bitmap at bottom
    mov bx, [btn_x]
    mov word [draw_x], bx
    mov cx, [btn_y]
    add cx, [btn_w]
    sub cx, SCROLLBAR_ARROW_H
    mov word [draw_y], cx
    mov si, scrollbar_down_bitmap
    call draw_char
    mov byte [draw_bg_color], 0
    ; Restore font params
    mov al, [btn_saved_fh]
    mov [draw_font_height], al
    mov al, [btn_saved_fw]
    mov [draw_font_width], al
    mov al, [btn_saved_fa]
    mov [draw_font_advance], al
    ; Calculate thumb
    ; Usable = track_height - 2*arrow_h
    mov ax, [btn_w]
    sub ax, SCROLLBAR_ARROW_H * 2
    cmp ax, SCROLLBAR_MIN_THUMB
    jl .sb_no_thumb
    ; Thumb height = max(MIN_THUMB, usable/4)
    mov cx, ax
    shr cx, 2
    cmp cx, SCROLLBAR_MIN_THUMB
    jge .sb_thumb_ok
    mov cx, SCROLLBAR_MIN_THUMB
.sb_thumb_ok:
    mov [wgt_cursor_pos], cx        ; Save thumb height
    ; Travel range = usable - thumb_h
    mov di, ax
    sub di, cx
    ; If max_range == 0, thumb at top
    cmp word [wgt_scratch], 0
    je .sb_thumb_top
    mov ax, [btn_h]                 ; position
    mul di                          ; DX:AX = pos * travel
    div word [wgt_scratch]          ; AX = thumb offset
    jmp .sb_draw_thumb
.sb_thumb_top:
    xor ax, ax
.sb_draw_thumb:
    ; Filled rect for thumb
    add ax, [btn_y]
    add ax, SCROLLBAR_ARROW_H
    mov cx, ax                      ; CX = thumb Y
    mov bx, [btn_x]
    inc bx
    mov dx, SCROLLBAR_WIDTH - 2
    mov si, [wgt_cursor_pos]        ; SI = thumb height
    cmp byte [widget_style], 0
    je .sb_flat_thumb
    ; 3D: face fill + raised bevel
    mov al, SYS_BTN_FACE
    call gfx_draw_filled_rect_color
    call draw_raised_bevel
    jmp .sb_thumb_done
.sb_flat_thumb:
    call gfx_draw_filled_rect_stub
.sb_thumb_done:
.sb_no_thumb:
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; Scrollbar arrow bitmaps (8x8)
scrollbar_up_bitmap:
    db 0b11111111                   ; XXXXXXXX
    db 0b10000001                   ; X......X
    db 0b10011001                   ; X..XX..X
    db 0b10111101                   ; X.XXXX.X
    db 0b10011001                   ; X..XX..X
    db 0b10011001                   ; X..XX..X
    db 0b10000001                   ; X......X
    db 0b11111111                   ; XXXXXXXX

scrollbar_down_bitmap:
    db 0b11111111                   ; XXXXXXXX
    db 0b10000001                   ; X......X
    db 0b10011001                   ; X..XX..X
    db 0b10011001                   ; X..XX..X
    db 0b10111101                   ; X.XXXX.X
    db 0b10011001                   ; X..XX..X
    db 0b10000001                   ; X......X
    db 0b11111111                   ; XXXXXXXX


; ============================================================================
; widget_hit_test - Test if mouse is inside a rectangle (API 53)
; Input: BX=X, CX=Y, DX=width, SI=height (window-relative)
; Output: AL=1 if mouse inside, AL=0 if outside
; NOT auto-translated (handles its own translation)
; ============================================================================
widget_hit_test:
    ; BX=X, CX=Y, DX=width, SI=height (window-relative)
    ; Output: AL=1 if mouse inside, AL=0 if outside
    push bx
    push cx
    push dx
    push si
    push di
    ; Translate to absolute coords if draw_context active
    cmp byte [draw_context], 0xFF
    je .ht_abs
    cmp byte [draw_context], WIN_MAX_COUNT
    jae .ht_abs
    ; Translate BX,CX using window content area
    push ax
    xor ah, ah
    mov al, [draw_context]
    mov di, ax
    shl di, 5
    add di, window_table
    ; Content scaling: double coordinates for auto-scaled windows
    cmp byte [di + WIN_OFF_CONTENT_SCALE], 2
    jne .ht_no_scale
    shl bx, 1                     ; rect_x * 2
    shl cx, 1                     ; rect_y * 2
    shl dx, 1                     ; rect_w * 2
    shl si, 1                     ; rect_h * 2
.ht_no_scale:
    add bx, [di + WIN_OFF_X]
    inc bx
    add cx, [di + WIN_OFF_Y]
    add cx, [titlebar_height]
    pop ax
.ht_abs:
    ; Now BX=abs_x, CX=abs_y, DX=width, SI=height
    ; Get mouse position
    mov di, [mouse_x]
    ; Check X: mouse_x >= abs_x && mouse_x < abs_x + width
    cmp di, bx
    jb .ht_miss
    add bx, dx                     ; BX = abs_x + width
    cmp di, bx
    jae .ht_miss
    ; Check Y: mouse_y >= abs_y && mouse_y < abs_y + height
    mov di, [mouse_y]
    cmp di, cx
    jb .ht_miss
    add cx, si                     ; CX = abs_y + height
    cmp di, cx
    jae .ht_miss
    ; Hit!
    mov al, 1
    jmp .ht_done
.ht_miss:
    xor al, al
.ht_done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    clc
    ret

; ============================================================================
; theme_set_colors - Set theme colors (API 54)
; Input: AL = text_color, BL = desktop_bg_color, CL = win_color
; Output: CF = 0
; ============================================================================
theme_set_colors:
    cmp byte [video_mode], 0x13
    je .tsc_no_mask
    and al, 0x03
    and bl, 0x03
    and cl, 0x03
.tsc_no_mask:
    mov [text_color], al
    mov [draw_fg_color], al
    mov [desktop_bg_color], bl
    mov [win_color], cl
    clc
    ret

; ============================================================================
; theme_get_colors - Get theme colors (API 55)
; Input: None
; Output: AL = text_color, BL = desktop_bg_color, CL = win_color, CF = 0
; ============================================================================
theme_get_colors:
    mov al, [text_color]
    mov bl, [desktop_bg_color]
    mov cl, [win_color]
    clc
    ret

; ============================================================================
; get_tick_count - Get BIOS timer tick count (API 63)
; Input: None
; Output: AX = tick count (low 16 bits, wraps at 65536, 18.2 Hz)
;         CF = 0
; ============================================================================
get_tick_count:
    push es
    push bx
    mov bx, 0x0040
    mov es, bx
    mov ax, [es:0x006C]
    pop bx
    pop es
    clc
    ret

; point_over_window - Check if a screen point is over any visible window (API 64)
; Input: BX = X position, CX = Y position
; Output: CF=0 if over a visible window (AL=window handle), CF=1 if not
; Preserves: BX, CX
point_over_window:
    push si
    push di
    push dx

    xor si, si                      ; SI = window index
    mov di, window_table

.pow_scan:
    cmp si, WIN_MAX_COUNT
    jae .pow_not_over

    cmp byte [di + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .pow_next

    ; Check X: win_x <= BX < win_x + width
    mov ax, [di + WIN_OFF_X]
    cmp bx, ax
    jb .pow_next
    add ax, [di + WIN_OFF_WIDTH]
    cmp bx, ax
    jae .pow_next

    ; Check Y: win_y <= CX < win_y + height
    mov ax, [di + WIN_OFF_Y]
    cmp cx, ax
    jb .pow_next
    add ax, [di + WIN_OFF_HEIGHT]
    cmp cx, ax
    jae .pow_next

    ; Point is inside this window
    mov ax, si                      ; AL = window handle
    pop dx
    pop di
    pop si
    clc
    ret

.pow_next:
    add di, WIN_ENTRY_SIZE
    inc si
    jmp .pow_scan

.pow_not_over:
    pop dx
    pop di
    pop si
    stc
    ret

; ============================================================================
; widget_draw_combobox - Draw a dropdown combo box (API 65)
; Input: BX=X, CX=Y, DX=width, SI=text_ptr (caller_ds), AL=flags
;        Flags: bit 0=focused, bit 1=open/pressed
; Height: font_height + 4 (auto)
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
COMBO_ARROW_W equ 8

widget_draw_combobox:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    mov [btn_flags], al
    mov [btn_x], bx
    mov [btn_y], cx
    mov [btn_w], dx
    mov [wgt_text_ptr], si
    ; Height = font_height + 4
    xor ah, ah
    mov al, [draw_font_height]
    add ax, 4
    mov [btn_h], ax
    mov ax, [video_segment]
    mov es, ax
    ; Clear area and draw border
    mov si, [btn_h]
    call gfx_clear_area_stub
    mov bx, [btn_x]
    mov cx, [btn_y]
    mov dx, [btn_w]
    mov si, [btn_h]
    call gfx_draw_rect_stub
    ; Draw text clipped to field interior (minus arrow area)
    push word [clip_x1]
    push word [clip_x2]
    push word [clip_y1]
    push word [clip_y2]
    push word [clip_enabled]
    mov ax, [btn_x]
    inc ax
    mov [clip_x1], ax
    mov ax, [btn_x]
    add ax, [btn_w]
    sub ax, COMBO_ARROW_W + 2
    mov [clip_x2], ax
    mov ax, [btn_y]
    inc ax
    mov [clip_y1], ax
    mov ax, [btn_y]
    add ax, [btn_h]
    sub ax, 2
    mov [clip_y2], ax
    mov byte [clip_enabled], 1
    mov si, [wgt_text_ptr]
    mov bx, [btn_x]
    add bx, 2
    mov cx, [btn_y]
    add cx, 2
    call gfx_draw_string_stub
    ; Restore clip
    pop word [clip_enabled]
    pop word [clip_y2]
    pop word [clip_y1]
    pop word [clip_x2]
    pop word [clip_x1]
    ; Draw arrow button separator line (vertical)
    mov bx, [btn_x]
    add bx, [btn_w]
    sub bx, COMBO_ARROW_W
    mov cx, [btn_y]
    inc cx
    xor ah, ah
    mov al, [draw_font_height]
    add al, 2
    mov di, ax
.cb_vline:
    cmp di, 0
    je .cb_arrow
    push cx
    push bx
    ; plot_pixel_white: CX=X, BX=Y
    xchg bx, cx
    call plot_pixel_white
    pop bx
    pop cx
    inc cx
    dec di
    jmp .cb_vline
.cb_arrow:
    ; Draw down-arrow triangle in arrow area
    ; Arrow center: btn_x + btn_w - COMBO_ARROW_W/2
    mov ax, [btn_x]
    add ax, [btn_w]
    sub ax, COMBO_ARROW_W / 2
    mov [wgt_scratch], ax           ; Center X
    mov ax, [btn_y]
    add ax, 2
    xor bx, bx
    mov bl, [draw_font_height]
    shr bx, 1                      ; Half font height
    add ax, bx
    sub ax, 1                      ; Center Y - approx middle
    mov [wgt_cursor_pos], ax        ; Center Y
    ; Draw 3-row triangle: row0=1px, row1=3px, row2=5px
    ; Row 0: center pixel
    mov cx, [wgt_scratch]
    mov bx, [wgt_cursor_pos]
    xchg bx, cx
    call plot_pixel_white
    ; Row 1: center-1 to center+1
    mov bx, [wgt_cursor_pos]
    inc bx
    mov cx, [wgt_scratch]
    dec cx
    xchg bx, cx
    call plot_pixel_white
    mov bx, [wgt_cursor_pos]
    inc bx
    mov cx, [wgt_scratch]
    xchg bx, cx
    call plot_pixel_white
    mov bx, [wgt_cursor_pos]
    inc bx
    mov cx, [wgt_scratch]
    inc cx
    xchg bx, cx
    call plot_pixel_white
    ; Row 2: center-2 to center+2
    mov bx, [wgt_cursor_pos]
    add bx, 2
    mov cx, [wgt_scratch]
    sub cx, 2
    xchg bx, cx
    call plot_pixel_white
    mov cx, [wgt_scratch]
    dec cx
    mov bx, [wgt_cursor_pos]
    add bx, 2
    xchg bx, cx
    call plot_pixel_white
    mov cx, [wgt_scratch]
    mov bx, [wgt_cursor_pos]
    add bx, 2
    xchg bx, cx
    call plot_pixel_white
    mov cx, [wgt_scratch]
    inc cx
    mov bx, [wgt_cursor_pos]
    add bx, 2
    xchg bx, cx
    call plot_pixel_white
    mov cx, [wgt_scratch]
    add cx, 2
    mov bx, [wgt_cursor_pos]
    add bx, 2
    xchg bx, cx
    call plot_pixel_white
    ; Done
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; ============================================================================
; widget_draw_menubar - Draw a horizontal menu bar (API 66)
; Input: BX=X, CX=Y, DX=bar_width, SI=items_ptr (caller_ds, consecutive
;        null-terminated strings), DI=item_count, AL=selected_index (0xFF=none)
; Height: font_height + 2 (auto)
; Auto-translated by INT 0x80 for draw_context
; ============================================================================
MENUBAR_PAD equ 6                       ; Padding between items

widget_draw_menubar:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    mov [btn_flags], al                 ; Selected index
    mov [btn_x], bx
    mov [btn_y], cx
    mov [btn_w], dx
    mov [wgt_text_ptr], si              ; Items pointer
    mov [wgt_scratch], di               ; Item count
    ; Height = font_height + 2
    xor ah, ah
    mov al, [draw_font_height]
    add ax, 2
    mov [btn_h], ax
    mov ax, [video_segment]
    mov es, ax
    ; Clear bar area (black background)
    mov si, [btn_h]
    call gfx_clear_area_stub
    ; Draw separator line at bottom of bar
    mov bx, [btn_x]
    mov cx, [btn_y]
    add cx, [btn_h]
    dec cx
    xor di, di                          ; pixel counter
.mb_sepline:
    cmp di, [btn_w]
    jge .mb_sep_done
    push cx
    push bx
    xchg bx, cx                        ; plot_pixel_white: CX=X, BX=Y
    call plot_pixel_white
    pop bx
    pop cx
    inc bx
    inc di
    jmp .mb_sepline
.mb_sep_done:
    ; Draw items
    xor di, di                          ; Item index = 0
    mov bx, [btn_x]
    add bx, MENUBAR_PAD
    mov si, [wgt_text_ptr]
.mb_loop:
    cmp di, [wgt_scratch]
    jge .mb_done
    ; First: walk string in caller_ds to count chars
    push ds
    push si
    mov ax, [caller_ds]
    mov ds, ax
    xor cx, cx
.mb_count:
    lodsb
    test al, al
    jz .mb_count_done
    inc cx
    jmp .mb_count
.mb_count_done:
    pop si                              ; Restore SI to string start
    pop ds
    ; CX = char count, SI = string start (in caller_ds)
    ; Compute pixel width
    push dx
    xor ax, ax
    mov al, [draw_font_advance]
    mul cx
    mov [wgt_cursor_pos], ax            ; Item pixel width
    pop dx
    ; Check if selected
    mov al, [btn_flags]
    xor ah, ah
    cmp ax, di
    jne .mb_draw_normal
    ; Selected: draw filled rect highlight, then inverted text
    push bx
    push di
    push si                             ; Save string pointer!
    mov dx, [wgt_cursor_pos]
    add dx, 4                           ; Small padding around text
    mov cx, [btn_y]
    dec bx                              ; Shift rect 1px left for padding
    mov si, [btn_h]
    dec si                              ; Don't cover separator
    call gfx_draw_filled_rect_stub
    pop si                              ; Restore string pointer
    pop di
    pop bx
    ; Draw inverted (black) text on white rect
    push bx
    push si
    push di
    mov cx, [btn_y]
    inc cx
    call gfx_draw_string_inverted
    pop di
    pop si
    pop bx
    jmp .mb_advance
.mb_draw_normal:
    ; Normal: draw white text on black background
    push bx
    push si
    push di
    mov cx, [btn_y]
    inc cx
    call gfx_draw_string_stub
    pop di
    pop si
    pop bx
.mb_advance:
    ; Walk SI past the string null terminator in caller_ds
    push ds
    mov ax, [caller_ds]
    mov ds, ax
.mb_skip:
    lodsb
    test al, al
    jnz .mb_skip
    pop ds
    ; SI now past null - points to next string
    ; Advance X
    add bx, [wgt_cursor_pos]
    add bx, MENUBAR_PAD
    inc di
    jmp .mb_loop
.mb_done:
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

; gfx_draw_rect_stub - Draw rectangle outline
; Input: BX = X, CX = Y, DX = Width, SI = Height
gfx_draw_rect_stub:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bp
    push di
    mov ax, [video_segment]
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
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

; gfx_draw_filled_rect_stub - Draw filled rectangle
gfx_draw_filled_rect_stub:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bp
    push di
    mov ax, [video_segment]
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
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

; gfx_clear_area_stub - Clear rectangular area
; Input: BX = X coordinate (top-left)
;        CX = Y coordinate (top-left)
;        DX = Width
;        SI = Height
; Output: None
; Preserves: All registers
gfx_clear_area_stub:
    ; Defensive guard: 0 width or height would cause 65536-iteration loop
    test dx, dx
    jz .early_ret
    test si, si
    jz .early_ret

    cmp byte [cs:video_mode], 0x01
    je .vesa_clear
    cmp byte [cs:video_mode], 0x12
    je .mode12h_clear
    cmp byte [cs:video_mode], 0x13
    je .vga_clear

    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bp
    push di
    push bx
    push cx
    push dx
    push si

    mov ax, [cs:video_segment]
    mov es, ax
    mov bp, si                      ; BP = height counter

    ; Check for byte-aligned fast path (BX % 4 == 0 AND DX % 4 == 0)
    mov ax, bx
    and ax, 3
    jnz .slow_path
    mov ax, dx
    and ax, 3
    jnz .slow_path

    ; Fast path: byte-aligned, use rep stosb per row
    mov si, dx
    shr si, 2                       ; SI = bytes per row (DX / 4)
.fast_row:
    push cx                         ; Save Y
    push bx                         ; Save X
    ; Calculate CGA row offset
    mov ax, cx
    shr ax, 1
    push dx
    mov di, 80
    mul di
    pop dx
    mov di, ax                      ; DI = (Y/2) * 80
    ; Add X byte offset
    mov ax, bx
    shr ax, 2
    add di, ax
    ; Handle interlacing (odd rows at +0x2000)
    test cl, 1
    jz .fast_even
    add di, 0x2000
.fast_even:
    mov cx, si                      ; CX = byte count
    xor al, al
    rep stosb
    pop bx
    pop cx
    inc cx                          ; Next Y
    dec bp
    jnz .fast_row
    jmp .clear_done

.slow_path:
    ; Optimized hybrid CGA clear: partial edge bytes + fast middle rep stosb
    ; BX=X, DX=width, CX=Y, BP=height, ES=video segment
    cmp dx, 4
    jb .pixel_path                  ; Very narrow: pixel-by-pixel is fine

.opt_row:
    push cx                         ; Save Y
    push bx                         ; Save start X
    push dx                         ; Save width

    ; Calculate CGA row base offset
    mov ax, cx
    shr ax, 1
    push bx
    push dx
    mov di, 80
    mul di                          ; AX = (Y/2) * 80
    pop dx
    pop bx
    mov si, ax                      ; SI = row base
    test cl, 1
    jz .or_even
    add si, 0x2000
.or_even:

    ; --- Leading partial byte (clear X%4..3 in first byte) ---
    mov ax, bx
    and ax, 3
    jz .or_no_lead                  ; X aligned, skip leading

    mov di, bx
    shr di, 2
    add di, si                      ; DI = first byte in video mem

    push cx
    mov cx, 4
    sub cx, ax                      ; CX = pixels to clear (1-3)
    sub dx, cx                      ; Reduce remaining width
    add bx, cx                      ; BX = first aligned X
    shl cl, 1                       ; CL = bits to clear (2,4,6)
    mov al, 0xFF
    shl al, cl                      ; AL = AND mask (keep upper bits)
    and [es:di], al                 ; Clear lower bits
    pop cx

.or_no_lead:
    ; --- Middle full bytes (BX is now 4-aligned) ---
    mov ax, dx
    shr ax, 2                       ; AX = full bytes to clear
    jz .or_no_middle

    mov di, bx
    shr di, 2
    add di, si                      ; DI = first full byte offset

    push cx
    mov cx, ax                      ; CX = byte count
    xor al, al
    rep stosb                       ; Clear full bytes
    pop cx

    ; Advance BX past cleared middle
    mov ax, dx
    shr ax, 2
    shl ax, 2                       ; AX = middle pixels (multiple of 4)
    add bx, ax

.or_no_middle:
    ; --- Trailing partial byte (clear pixels 0..trailing-1) ---
    mov ax, dx
    and ax, 3                       ; AX = trailing pixels (0-3)
    jz .or_no_trail

    mov di, bx
    shr di, 2
    add di, si                      ; DI = last byte offset

    push cx
    mov cl, al                      ; CL = trailing pixels (1-3)
    shl cl, 1                       ; CL = bits to clear (2,4,6)
    mov al, 0xFF
    shr al, cl                      ; AL = AND mask (keep lower bits)
    and [es:di], al
    pop cx

.or_no_trail:
    pop dx                          ; Restore width
    pop bx                          ; Restore start X
    pop cx                          ; Restore Y
    inc cx                          ; Next Y
    dec bp
    jnz .opt_row
    jmp .clear_done

.pixel_path:
    ; Original pixel-by-pixel path for very narrow widths (< 4 pixels)
.px_row:
    mov di, dx                      ; DI = width counter
    push cx                         ; Save Y
    push bx                         ; Save X
.px_col:
    call .plot_bg
    inc bx                          ; Next X
    dec di
    jnz .px_col
    pop bx                          ; Restore X
    pop cx                          ; Restore Y
    inc cx                          ; Next Y
    dec bp
    jnz .px_row

.clear_done:
    pop si
    pop dx
    pop cx
    pop bx
    pop di
    pop bp
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
.early_ret:
    ret

.vesa_clear:
    ; VESA: use vesa_fill_rect with color 0
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    mov ax, 0xA000
    mov es, ax
    xor al, al
    call vesa_fill_rect
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

.mode12h_clear:
    ; Mode 12h: use mode12h_fill_rect with color 0
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    mov ax, 0xA000
    mov es, ax
    xor al, al                     ; Color 0 = black
    call mode12h_fill_rect          ; BX=X, CX=Y, DX=W, SI=H, AL=color
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

.vga_clear:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov ax, [cs:video_segment]
    mov es, ax
    ; BX=X, CX=Y, DX=width, SI=height
.vga_clear_row:
    push cx                         ; Save Y
    ; DI = Y * screen_pitch + X
    mov ax, cx
    push dx
    mul word [cs:screen_pitch]
    pop dx
    add ax, bx
    mov di, ax                      ; DI = Y*pitch + X
    mov cx, dx                      ; CX = width (bytes to clear)
    xor al, al
    rep stosb
    pop cx                          ; Restore Y
    inc cx                          ; Next row
    dec si
    jnz .vga_clear_row

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

; Internal helper: Clear pixel at BX=X, CX=Y to background color
.plot_bg:
    cmp bx, [screen_width]
    jae .bg_out
    cmp cx, [screen_height]
    jae .bg_out
    push ax
    push bx
    push cx
    push di
    push dx

    ; Calculate video memory offset
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
.bg_out:
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
EVENT_MOUSE         equ 4
EVENT_WIN_MOVED     equ 5
EVENT_WIN_REDRAW    equ 6               ; Window needs content redraw (DX = handle)

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

    ; Process any pending window drag (deferred from IRQ12)
    call mouse_process_drag

.evt_check_next:
    mov bx, [event_queue_head]
    cmp bx, [event_queue_tail]
    je .no_event

    ; Calculate head position (events are 3 bytes each)
    mov si, bx
    add si, bx                      ; SI = head * 2
    add si, bx                      ; SI = head * 3

    ; Read event from queue (DO NOT advance head yet)
    mov al, [event_queue + si]      ; type
    mov dx, [event_queue + si + 1]  ; data (word)

    ; Filter keyboard events: only deliver to focused task
    cmp al, EVENT_KEY_PRESS
    jne .evt_not_key
    push ax
    mov al, [focused_task]
    cmp al, 0xFF                    ; No window focused? Deliver to current task
    je .evt_focus_ok
    cmp al, [current_task]
    je .evt_focus_ok
    pop ax
    jmp .no_event                   ; Not focused - leave event in queue for correct task
.evt_focus_ok:
    pop ax
    jmp .evt_consume                ; This task has focus (or no focus set), consume and deliver

.evt_not_key:
    ; Filter: skip WIN_REDRAW events not for current task's window
    cmp al, EVENT_WIN_REDRAW
    jne .evt_consume                ; Other event types: consume and pass through
    cmp dl, WIN_MAX_COUNT
    jae .evt_discard                ; Invalid window handle: consume garbage and retry
    push si
    push ax
    xor ah, ah
    mov al, dl
    mov si, ax
    shl si, 5
    add si, window_table
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .evt_discard_pop            ; Window freed/destroyed: discard stale event
    mov al, [si + WIN_OFF_OWNER]
    cmp al, [current_task]
    pop ax
    pop si
    je .evt_consume                 ; Window belongs to current task, consume and return
    jmp .no_event                   ; Wrong task's window - leave in queue

.evt_consume:
    ; Now advance head (event confirmed for this task)
    inc bx
    and bx, 0x1F                    ; Wrap at 32 events
    mov [event_queue_head], bx

.evt_return:
    clc                             ; CF=0 = event available
    pop ds
    pop si
    pop bx
    ret

.evt_discard_pop:
    pop ax
    pop si
    ; fall through to evt_discard
.evt_discard:
    ; Consume invalid event and try next
    inc bx
    and bx, 0x1F
    mov [event_queue_head], bx
    jmp .evt_check_next

.no_event:
    ; On HD/USB boot, poll BIOS keyboard since our INT 9 handler isn't installed
    cmp byte [use_bios_keyboard], 1
    jne .no_event_return
    ; Focus check: only read BIOS keyboard if this task has focus (or no focus set).
    ; Without this, the launcher steals keystrokes meant for the focused app.
    push ax
    mov al, [focused_task]
    cmp al, 0xFF                    ; No focus set? Any task can read
    je .bios_focus_ok
    cmp al, [current_task]
    jne .bios_focus_fail            ; Not focused — leave key in BIOS buffer
.bios_focus_ok:
    pop ax
    ; Check BIOS keyboard buffer (INT 16h AH=01h)
    mov ah, 0x01
    int 0x16
    jz .no_event_return             ; ZF=1 means no key available
    ; Key available — read it (INT 16h AH=00h)
    xor ah, ah
    int 0x16                        ; AH=scancode, AL=ASCII
    ; Translate extended keys (AL=0) to special codes matching INT 9 handler
    test al, al
    jnz .bios_key_ready             ; Normal ASCII key — use as-is
    ; Extended key: map scancode (AH) to special key codes
    cmp ah, 0x48
    je .bios_arrow_up
    cmp ah, 0x50
    je .bios_arrow_down
    cmp ah, 0x4B
    je .bios_arrow_left
    cmp ah, 0x4D
    je .bios_arrow_right
    cmp ah, 0x47
    je .bios_home
    cmp ah, 0x4F
    je .bios_end
    cmp ah, 0x53
    je .bios_delete
    cmp ah, 0x49
    je .bios_pgup
    cmp ah, 0x51
    je .bios_pgdn
    jmp .bios_key_ready             ; Unknown extended key
.bios_arrow_up:
    mov al, 128
    jmp .bios_key_ready
.bios_arrow_down:
    mov al, 129
    jmp .bios_key_ready
.bios_arrow_left:
    mov al, 130
    jmp .bios_key_ready
.bios_arrow_right:
    mov al, 131
    jmp .bios_key_ready
.bios_home:
    mov al, 132
    jmp .bios_key_ready
.bios_end:
    mov al, 133
    jmp .bios_key_ready
.bios_delete:
    mov al, 134
    jmp .bios_key_ready
.bios_pgup:
    mov al, 135
    jmp .bios_key_ready
.bios_pgdn:
    mov al, 136
.bios_key_ready:
    ; Synthesize a KEY_PRESS event
    mov dl, al                      ; DL = ASCII char (or special code)
    mov dh, ah                      ; DH = scancode
    mov al, EVENT_KEY_PRESS
    clc                             ; CF=0 = event available
    pop ds
    pop si
    pop bx
    ret

.bios_focus_fail:
    pop ax
.no_event_return:
    xor al, al                      ; AL = 0 (no event, keeps event_wait working)
    mov dh, [focused_task]          ; DH = focused task ID
    mov dl, [current_task]          ; DL = current task ID
    mov ch, byte [event_queue_head] ; CH = queue head index
    mov cl, byte [event_queue_tail] ; CL = queue tail index
    stc                             ; CF=1 = no event available
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
FS_ERR_WRITE_ERROR      equ 7
FS_ERR_DISK_FULL        equ 8
FS_ERR_DIR_FULL         equ 9

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

; fs_open_api - API table wrapper for fs_open (API 14)
; Sets DS from caller_ds so apps can pass filenames from their own segment.
; Internal kernel callers should use fs_open_stub directly.
fs_open_api:
    push ds
    mov ds, [cs:caller_ds]          ; DS = caller's segment for filename
    call fs_open_stub
    pop ds
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

    ; Route based on mount handle (check BL only, not full BX)
    cmp bl, 0
    je .fat12
    cmp bl, 1
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

    ; Clear AH (still has function number from INT 0x80 dispatch)
    xor ah, ah                      ; AL = file handle, AH was function 15

    ; Validate file handle (0-15)
    cmp ax, FILE_MAX_HANDLES
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

    ; Clear AH (still has function number from INT 0x80 dispatch)
    xor ah, ah                      ; AL = file handle

    ; Validate file handle (0-15)
    cmp ax, FILE_MAX_HANDLES
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

    ; Read sector to bpb_buffer (with retry for real floppy drives)
    push es
    push di
    mov [.save_ch], ch              ; Save CHS for retry
    mov [.save_cl], cl
    mov [.save_dh], dh
    mov byte [.retry], 3           ; 3 attempts
.retry_read:
    mov ax, 0x1000
    mov es, ax
    mov bx, bpb_buffer
    mov ch, [.save_ch]
    mov cl, [.save_cl]
    mov dh, [.save_dh]
    mov ax, 0x0201                  ; AH=02 (read), AL=01 (1 sector)
    mov dl, 0                       ; DL = drive A:
    int 0x13
    jnc .read_ok_dir
    ; Reset drive and retry
    dec byte [.retry]
    jz .retry_failed
    xor ah, ah
    mov dl, 0
    int 0x13
    jmp .retry_read
.retry_failed:
    pop di
    pop es
    pop ax
    jmp .read_error
.read_ok_dir:
    pop di
    pop es
    pop ax                          ; Restore LBA (not needed but clean stack)

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
.save_ch:       db 0
.save_cl:       db 0
.save_dh:       db 0
.retry:         db 0

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

    ; HARD-CODE BPB values instead of reading sector 78
    ; Our FAT12 filesystem at sector 78 has known parameters:
    ; - 512 bytes per sector
    ; - 1 sector per cluster
    ; - 1 reserved sector
    ; - 2 FATs
    ; - 224 root directory entries
    ; - 9 sectors per FAT

    mov word [bytes_per_sector], 512
    mov byte [sectors_per_cluster], 1
    mov word [reserved_sectors], 1
    mov byte [num_fats], 2
    mov word [root_dir_entries], 224
    mov word [sectors_per_fat], 9

    ; Calculate FAT start sector (absolute)
    ; fat_start = 86 + reserved_sectors = 87
    mov word [fat_start], 87        ; Filesystem at sector 86 + 1 reserved

    ; Calculate root directory start sector
    ; root_dir_start = 86 + reserved + (num_fats * sectors_per_fat)
    ; = 86 + 1 + (2 * 9) = 86 + 1 + 18 = 105
    mov ax, 1                       ; reserved_sectors
    add ax, 18                      ; num_fats * sectors_per_fat
    add ax, 86                      ; Filesystem starts at sector 86
    mov [root_dir_start], ax        ; = 105

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

    ; Invalidate FAT cache (critical after floppy swap — stale cache
    ; from a previous mount would corrupt fat12_alloc_cluster/set_fat_entry)
    mov word [fat_cache_sector], 0xFFFF

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

    ; Read one sector of root directory with retry
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_read_sector         ; AX = LBA, ES:BX = buffer (preserves AX)
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
    add sp, 2                       ; CX (sector counter) from .search_next_sector
    add sp, 2                       ; AX (sector number) from .search_next_sector
    add sp, 11                      ; 8.3 filename allocated with sub sp, 11
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
    add sp, 2                       ; CX (sector counter) from .search_next_sector
    add sp, 2                       ; AX (sector number) from .search_next_sector
    add sp, 11                      ; 8.3 filename allocated with sub sp, 11
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

    ; Clean up stack from search loop
    add sp, 2                       ; DS pushed at .search_next_sector (push ds)
    add sp, 2                       ; AX (sector number) from .search_next_sector
    add sp, 2                       ; CX (sector counter) from .search_next_sector
    add sp, 11                      ; 8.3 filename allocated with sub sp, 11

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
    mov cx, FILE_MAX_HANDLES
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
    ; FAT sector = fat_start + (byte_offset / 512)
    xor dx, dx
    mov cx, 512
    div cx                          ; AX = sector offset in FAT, DX = byte offset in sector
    mov di, dx                      ; DI = byte offset within sector
    add ax, [fat_start]             ; AX = absolute FAT sector number

    ; Check if this FAT sector is already cached
    cmp ax, [fat_cache_sector]
    je .sector_cached

    ; Load FAT sector into cache with retry
    mov [fat_cache_sector], ax      ; Update cached sector number

    push ax
    mov bx, 0x1000
    mov es, bx
    mov bx, fat_cache

    ; AX = absolute FAT sector number
    call floppy_read_sector         ; Read with retry (preserves AX)
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
; floppy_read_sector - Read one sector with retry logic
; Input: AX = LBA sector number, ES:BX = buffer to read into
; Output: CF = 0 on success, CF = 1 on error
; Preserves: AX, ES, BX (buffer pointer)
; Clobbers: CX, DX
; ============================================================================
floppy_read_sector:
    push ax
    mov [cs:.frs_lba], ax
    mov byte [cs:.frs_retry], 3     ; 3 attempts
.frs_loop:
    ; Convert LBA to CHS
    mov ax, [cs:.frs_lba]
    xor dx, dx
    push bx                         ; Save buffer pointer
    mov bx, 18                      ; Sectors per track (1.44MB floppy)
    div bx                          ; AX = LBA / 18, DX = LBA % 18
    inc dx                          ; DX = sector (1-based)
    mov cl, dl                      ; CL = sector
    xor dx, dx
    mov bx, 2                       ; Number of heads
    div bx                          ; AX = cylinder, DX = head
    mov ch, al                      ; CH = cylinder
    mov dh, dl                      ; DH = head
    pop bx                          ; Restore buffer pointer
    mov ax, 0x0201                  ; AH=02 (read), AL=01 (1 sector)
    mov dl, 0x00                    ; Drive A:
    int 0x13
    jnc .frs_ok
    ; Reset drive and retry
    dec byte [cs:.frs_retry]
    jz .frs_fail
    xor ah, ah
    mov dl, 0
    int 0x13
    jmp .frs_loop
.frs_fail:
    pop ax
    stc
    ret
.frs_ok:
    pop ax
    clc
    ret
.frs_lba:   dw 0
.frs_retry: db 0

; ============================================================================
; floppy_write_sector - Write one sector with retry logic
; Input: AX = LBA sector number, ES:BX = buffer to write from
; Output: CF = 0 on success, CF = 1 on error
; Preserves: AX, ES, BX (buffer pointer)
; Clobbers: CX, DX
; ============================================================================
floppy_write_sector:
    push ax
    mov [cs:.fws_lba], ax
    mov byte [cs:.fws_retry], 3     ; 3 attempts
.fws_loop:
    ; Convert LBA to CHS
    mov ax, [cs:.fws_lba]
    xor dx, dx
    push bx                         ; Save buffer pointer
    mov bx, 18                      ; Sectors per track (1.44MB floppy)
    div bx                          ; AX = LBA / 18, DX = LBA % 18
    inc dx                          ; DX = sector (1-based)
    mov cl, dl                      ; CL = sector
    xor dx, dx
    mov bx, 2                       ; Number of heads
    div bx                          ; AX = cylinder, DX = head
    mov ch, al                      ; CH = cylinder
    mov dh, dl                      ; DH = head
    pop bx                          ; Restore buffer pointer
    mov ax, 0x0301                  ; AH=03 (write), AL=01 (1 sector)
    mov dl, 0x00                    ; Drive A:
    int 0x13
    jnc .fws_ok
    ; Reset drive and retry
    dec byte [cs:.fws_retry]
    jz .fws_fail
    xor ah, ah
    mov dl, 0
    int 0x13
    jmp .fws_loop
.fws_fail:
    pop ax
    stc
    ret
.fws_ok:
    pop ax
    clc
    ret
.fws_lba:   dw 0
.fws_retry: db 0

; ============================================================================
; fat12_alloc_cluster - Find and allocate a free cluster in FAT12
; Input: None
; Output: AX = allocated cluster number, CF=0 success
;         CF=1 if disk full (AX = FS_ERR_DISK_FULL)
; Clobbers: CX, DX
; ============================================================================
fat12_alloc_cluster:
    push bx
    push si
    push di
    push bp
    push es

    ; Scan FAT entries starting from cluster 2
    ; Total data clusters on 1.44MB floppy with our layout:
    ; (2880 - 78) total FS sectors = 2802, minus overhead = ~2772 data clusters
    mov cx, 2                       ; Start scanning from cluster 2
.scan_loop:
    cmp cx, 2847                    ; Max cluster for our floppy (conservative limit)
    jae .disk_full

    ; Calculate FAT byte offset: (cluster * 3) / 2
    mov ax, cx
    mov bx, cx
    shl ax, 1                      ; AX = cluster * 2
    add ax, bx                     ; AX = cluster * 3
    shr ax, 1                      ; AX = byte offset in FAT
    mov si, ax                     ; SI = FAT byte offset

    ; Calculate which FAT sector and offset within it
    xor dx, dx
    push cx
    mov cx, 512
    div cx                          ; AX = sector offset, DX = byte in sector
    pop cx
    mov di, dx                      ; DI = byte offset within sector
    add ax, [fat_start]             ; AX = absolute FAT sector number

    ; Check if this FAT sector is cached
    cmp ax, [fat_cache_sector]
    je .ac_cached

    ; Load FAT sector into cache
    mov [fat_cache_sector], ax
    push cx
    push ax
    mov bx, 0x1000
    mov es, bx
    mov bx, fat_cache
    call floppy_read_sector
    pop ax
    pop cx
    jc .alloc_error

.ac_cached:
    ; Read 2 bytes from FAT cache at offset DI
    mov si, fat_cache
    add si, di
    mov ax, [si]                    ; AX = 2 bytes from FAT

    ; Check even/odd cluster
    test cx, 1
    jnz .ac_odd

.ac_even:
    and ax, 0x0FFF
    jmp .ac_check_free

.ac_odd:
    shr ax, 4

.ac_check_free:
    cmp ax, 0                       ; 0 = free cluster
    je .found_free
    inc cx
    jmp .scan_loop

.found_free:
    ; Found free cluster in CX - mark it as end-of-chain (0xFFF)
    mov ax, cx                      ; AX = cluster to allocate
    mov dx, 0x0FFF                  ; DX = end-of-chain marker
    call fat12_set_fat_entry
    jc .alloc_error

    ; Return allocated cluster in AX
    mov ax, cx
    clc
    pop es
    pop bp
    pop di
    pop si
    pop bx
    ret

.disk_full:
    mov ax, FS_ERR_DISK_FULL
    stc
    pop es
    pop bp
    pop di
    pop si
    pop bx
    ret

.alloc_error:
    mov ax, FS_ERR_WRITE_ERROR
    stc
    pop es
    pop bp
    pop di
    pop si
    pop bx
    ret

; ============================================================================
; fat12_set_fat_entry - Set a FAT12 entry to a given value
; Input: AX = cluster number, DX = new 12-bit value
; Output: CF=0 success, CF=1 error
; Clobbers: BX, CX, SI, DI
; Writes to both FAT copies on disk
; ============================================================================
fat12_set_fat_entry:
    push ax
    push dx
    push bp
    push es

    mov bp, ax                      ; BP = cluster number
    mov [cs:.sfe_value], dx         ; Save new value

    ; Calculate FAT byte offset: (cluster * 3) / 2
    mov bx, ax
    shl ax, 1                       ; AX = cluster * 2
    add ax, bx                      ; AX = cluster * 3
    shr ax, 1                       ; AX = byte offset in FAT
    mov [cs:.sfe_fat_offset], ax

    ; Calculate FAT sector and byte position
    xor dx, dx
    mov cx, 512
    div cx                          ; AX = sector offset, DX = byte in sector
    mov di, dx                      ; DI = byte offset within sector
    mov [cs:.sfe_sector_off], ax    ; Save sector offset within FAT

    ; Calculate absolute FAT sector
    add ax, [fat_start]             ; AX = absolute FAT1 sector

    ; Ensure FAT sector is cached
    cmp ax, [fat_cache_sector]
    je .sfe_cached

    mov [fat_cache_sector], ax
    push ax
    mov bx, 0x1000
    mov es, bx
    mov bx, fat_cache
    call floppy_read_sector
    pop ax
    jc .sfe_error

.sfe_cached:
    ; Modify the entry in fat_cache
    mov si, fat_cache
    add si, di                      ; SI = pointer to entry bytes

    mov dx, [cs:.sfe_value]         ; DX = new 12-bit value

    ; Handle even/odd cluster
    test bp, 1
    jnz .sfe_odd

.sfe_even:
    ; Even cluster: low 12 bits of word at offset
    ; byte[off] = val & 0xFF
    ; byte[off+1] = (byte[off+1] & 0xF0) | ((val >> 8) & 0x0F)
    mov [si], dl                    ; Low byte of value
    mov al, [si + 1]
    and al, 0xF0                    ; Preserve high nibble of next byte
    mov ah, dh
    and ah, 0x0F                    ; High nibble of value
    or al, ah
    mov [si + 1], al
    jmp .sfe_write_back

.sfe_odd:
    ; Odd cluster: high 12 bits of word at offset
    ; byte[off] = (byte[off] & 0x0F) | ((val << 4) & 0xF0)
    ; byte[off+1] = (val >> 4) & 0xFF
    mov al, [si]
    and al, 0x0F                    ; Preserve low nibble
    mov ah, dl
    shl ah, 4                       ; Shift value low nibble to high
    or al, ah
    mov [si], al
    ; byte[off+1] = (val >> 4)
    mov ax, dx
    shr ax, 4
    mov [si + 1], al

.sfe_write_back:
    ; Write modified sector to FAT1
    mov ax, [cs:.sfe_sector_off]
    add ax, [fat_start]
    mov bx, 0x1000
    mov es, bx
    mov bx, fat_cache
    call floppy_write_sector
    jc .sfe_error

    ; Write same sector to FAT2 (fat_start + sectors_per_fat + sector_offset)
    mov ax, [cs:.sfe_sector_off]
    add ax, [fat_start]
    add ax, [sectors_per_fat]       ; FAT2 offset
    call floppy_write_sector
    jc .sfe_error

    clc
    pop es
    pop bp
    pop dx
    pop ax
    ret

.sfe_error:
    stc
    pop es
    pop bp
    pop dx
    pop ax
    ret

.sfe_value:      dw 0
.sfe_fat_offset: dw 0
.sfe_sector_off: dw 0

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
    cmp ax, FILE_MAX_HANDLES
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

    ; Read sector into bpb_buffer with retry (ES:BX = 0x1000:bpb_buffer)
    push es
    push di
    push si

    mov bx, 0x1000
    mov es, bx                      ; ES = 0x1000 (for INT 13h read)
    push bx
    pop ds                          ; DS = 0x1000 (for later access)
    mov bx, bpb_buffer              ; BX = buffer offset

    ; AX = LBA sector number
    call floppy_read_sector         ; Read with retry (preserves AX)

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
    cmp ax, FILE_MAX_HANDLES
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
    mov [.saved_lba], eax           ; Save for CHS fallback

    ; Build DAP at 0x0000:0x0600 (low memory — USB BIOSes require DAP below 64KB)
    push es
    mov cx, es                      ; CX = caller's buffer segment
    xor si, si
    mov es, si                      ; ES = 0x0000
    mov word [es:0x0600], 0x0010    ; Packet size = 16
    mov word [es:0x0602], 1         ; Read 1 sector
    mov [es:0x0604], bx             ; Buffer offset
    mov [es:0x0606], cx             ; Buffer segment (from caller's ES)
    mov [es:0x0608], eax            ; LBA low 32
    mov dword [es:0x060C], 0        ; LBA high 32
    pop es                          ; Restore caller's ES

    ; Try INT 13h extended read (LBA mode) with DS:SI = 0x0000:0x0600
    mov dl, [fat16_drive]           ; Drive number (DS=0x1000 still)
    push ds
    xor si, si
    mov ds, si                      ; DS = 0x0000
    mov si, 0x0600                  ; DS:SI = 0x0000:0x0600
    mov ah, 0x42
    int 0x13
    pop ds                          ; Restore DS = 0x1000 (pop doesn't affect CF)
    jnc .success

    ; Extended read failed - try CHS fallback
    ; First get drive geometry (INT 0x13/08 destroys ES, so save it)
    push es
    push bx
    mov ah, 0x08                    ; Get drive parameters
    mov dl, [fat16_drive]
    int 0x13
    pop bx                          ; Restore caller's BX
    pop es                          ; Restore caller's ES
    jc .error

    ; DH = max head number, CL[5:0] = max sector, CL[7:6]:CH = max cylinder
    mov al, cl
    and al, 0x3F                    ; AL = sectors per track
    mov [ide_sectors], al
    inc dh
    mov [ide_heads], dh             ; heads = max_head + 1

    ; Convert saved LBA to CHS
    ; CRITICAL: Use EBX for divisor (not ECX!) to avoid clobbering CL
    ; which holds the sector number between the two divisions.
    mov eax, [.saved_lba]

    ; Sector = (LBA mod sectors_per_track) + 1
    push ebx                        ; Save buffer offset for INT 13h
    xor edx, edx
    movzx ebx, byte [ide_sectors]
    div ebx                         ; EAX = LBA / SPT, EDX = LBA mod SPT
    inc dl                          ; Sector is 1-based
    mov cl, dl                      ; CL = sector

    ; Head = (temp / sectors_per_track) mod heads
    ; Cylinder = (temp / sectors_per_track) / heads
    xor edx, edx
    movzx ebx, byte [ide_heads]
    div ebx                         ; EAX = cylinder, EDX = head
    mov dh, dl                      ; DH = head
    mov ch, al                      ; CH = cylinder low 8 bits
    shl ah, 6
    or cl, ah                       ; CL[7:6] = cylinder high 2 bits
    pop ebx                         ; Restore buffer offset

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

.saved_lba: dd 0

; fat16_write_sector - Write a 512-byte sector to FAT16 partition
; Input: EAX = sector number (relative to partition start)
;        ES:BX = buffer to write from
; Output: CF = 0 on success, CF = 1 on error
fat16_write_sector:
    push eax
    push bx
    push cx
    push dx
    push si

    ; Add partition start offset to get absolute LBA
    add eax, [fat16_partition_lba]
    mov [.saved_lba], eax

    ; Build DAP at 0x0000:0x0600 (low memory — USB BIOSes require DAP below 64KB)
    push es
    mov cx, es                      ; CX = caller's buffer segment
    xor si, si
    mov es, si                      ; ES = 0x0000
    mov word [es:0x0600], 0x0010    ; Packet size = 16
    mov word [es:0x0602], 1         ; Write 1 sector
    mov [es:0x0604], bx             ; Buffer offset
    mov [es:0x0606], cx             ; Buffer segment (from caller's ES)
    mov [es:0x0608], eax            ; LBA low 32
    mov dword [es:0x060C], 0        ; LBA high 32
    pop es                          ; Restore caller's ES

    ; Try INT 13h extended write (LBA mode) with DS:SI = 0x0000:0x0600
    mov dl, [fat16_drive]
    push ds
    xor si, si
    mov ds, si                      ; DS = 0x0000
    mov si, 0x0600                  ; DS:SI = 0x0000:0x0600
    mov ah, 0x43
    mov al, 0                      ; No verify after write
    int 0x13
    pop ds
    jnc .ws_success

    ; Extended write failed - try CHS fallback
    push es
    push bx
    mov ah, 0x08
    mov dl, [fat16_drive]
    int 0x13
    pop bx
    pop es
    jc .ws_error

    ; DH = max head number, CL[5:0] = max sector, CL[7:6]:CH = max cylinder
    mov al, cl
    and al, 0x3F
    mov [ide_sectors], al
    inc dh
    mov [ide_heads], dh

    ; Convert saved LBA to CHS
    mov eax, [.saved_lba]

    ; Sector = (LBA mod sectors_per_track) + 1
    push ebx
    xor edx, edx
    movzx ebx, byte [ide_sectors]
    div ebx
    inc dl
    mov cl, dl                      ; CL = sector

    ; Head and Cylinder
    xor edx, edx
    movzx ebx, byte [ide_heads]
    div ebx
    mov dh, dl                      ; DH = head
    mov ch, al                      ; CH = cylinder low 8 bits
    shl ah, 6
    or cl, ah                       ; CL[7:6] = cylinder high 2 bits
    pop ebx

    ; Perform CHS write
    mov ah, 0x03
    mov al, 1
    mov dl, [fat16_drive]
    int 0x13
    jc .ws_error

.ws_success:
    clc
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    ret

.ws_error:
    stc
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    ret

.saved_lba: dd 0

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
    push ds                         ; Save caller's DS (may be app segment)

    ; Check if FAT16 is mounted (CS: because DS may be caller's segment)
    cmp byte [cs:fat16_mounted], 1
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
    ; Switch DS to kernel segment for root directory search
    ; (Filename conversion used caller's DS:SI, but is now on stack via SS:DI)
    mov ax, 0x1000
    mov ds, ax

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
    jz .search_error                ; Jump to search_error to pop saved eax/ecx first
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
    pop ds                          ; Restore caller's DS
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

; fat16_set_fat_entry - Write a 16-bit value to the FAT16 table
; Input: AX = cluster number, DX = new 16-bit value
; Output: CF = 0 on success, CF = 1 on error
; Writes to both FAT1 and FAT2
fat16_set_fat_entry:
    push eax
    push bx
    push cx
    push dx
    push si
    push es

    mov [cs:.sfe16_value], dx
    mov [cs:.sfe16_cluster], ax

    ; Calculate FAT sector: cluster / 256 (each sector holds 256 16-bit entries)
    shr ax, 8                       ; sector_index = cluster / 256
    movzx eax, ax
    add eax, [fat16_fat_start]      ; Absolute FAT sector

    ; Ensure FAT sector is cached
    cmp eax, [fat16_fat_cached_sect]
    je .sfe16_cached

    ; Load FAT sector into cache
    mov [fat16_fat_cached_sect], eax
    mov cx, 0x1000
    mov es, cx
    mov bx, fat16_fat_cache
    call fat16_read_sector
    jc .sfe16_error

.sfe16_cached:
    ; Calculate byte offset: (cluster mod 256) * 2
    mov ax, [cs:.sfe16_cluster]
    and ax, 0x00FF
    shl ax, 1                       ; * 2 bytes per entry
    mov bx, ax
    mov dx, [cs:.sfe16_value]
    mov [fat16_fat_cache + bx], dx  ; Write new value

    ; Write modified cache to FAT1
    mov eax, [fat16_fat_cached_sect]
    mov cx, 0x1000
    mov es, cx
    mov bx, fat16_fat_cache
    call fat16_write_sector
    jc .sfe16_error

    ; Write same sector to FAT2 (fat1_start + sectors_per_fat)
    movzx ecx, word [fat16_bpb_cache + 4]  ; sectors_per_fat
    add eax, ecx
    call fat16_write_sector
    jc .sfe16_error

    clc
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    ret

.sfe16_error:
    mov dword [fat16_fat_cached_sect], 0xFFFFFFFF  ; Invalidate cache
    stc
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    ret

.sfe16_value:   dw 0
.sfe16_cluster: dw 0

; fat16_alloc_cluster - Find and allocate a free cluster in FAT16
; Output: AX = allocated cluster number, CF = 0 on success
;         CF = 1 on error (AX = error code)
fat16_alloc_cluster:
    push bx
    push cx
    push dx
    push es

    ; Scan FAT entries starting from cluster 2
    ; FAT16 max is 65525 data clusters, but our 64MB image uses far fewer
    mov cx, 2                       ; Start from cluster 2
.f16ac_scan:
    cmp cx, 0xFFF0                  ; Max valid cluster
    jae .f16ac_full

    ; Calculate which FAT sector holds this cluster
    mov ax, cx
    shr ax, 8                       ; sector_index = cluster / 256
    movzx eax, ax
    add eax, [fat16_fat_start]

    ; Check if cached
    cmp eax, [fat16_fat_cached_sect]
    je .f16ac_cached

    ; Load FAT sector
    mov [fat16_fat_cached_sect], eax
    push cx
    mov bx, 0x1000
    mov es, bx
    mov bx, fat16_fat_cache
    call fat16_read_sector
    pop cx
    jc .f16ac_error

.f16ac_cached:
    ; Check entry: (cluster mod 256) * 2
    mov ax, cx
    and ax, 0x00FF
    shl ax, 1
    mov bx, ax
    cmp word [fat16_fat_cache + bx], 0  ; 0 = free
    je .f16ac_found

    inc cx
    jmp .f16ac_scan

.f16ac_found:
    ; Mark cluster as EOC (0xFFF8)
    mov ax, cx
    mov dx, 0xFFF8
    call fat16_set_fat_entry
    jc .f16ac_error

    ; Return allocated cluster
    mov ax, cx
    clc
    pop es
    pop dx
    pop cx
    pop bx
    ret

.f16ac_full:
    mov ax, FS_ERR_DISK_FULL
    stc
    pop es
    pop dx
    pop cx
    pop bx
    ret

.f16ac_error:
    mov ax, FS_ERR_WRITE_ERROR
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
    cmp ax, FILE_MAX_HANDLES
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
    imul eax, ecx                   ; EAX = (cluster - 2) * sects_per_clust
    mov ecx, eax                    ; ECX = full 32-bit cluster sector offset
    movzx eax, word [.offset_in_cluster]
    shr eax, 9                      ; EAX = sector offset within cluster
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
    mov [.current_entry], ax        ; Save for .skip_entry path
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
    mov ax, [.current_entry]        ; Load saved entry index
    inc ax                          ; Next entry
    mov cx, ax                      ; Return new state in CX
    clc                             ; Success - removed buggy push ax
    jmp .done

.skip_entry:
    ; Move to next entry (load from saved variable, not stack)
    mov ax, [.current_entry]
    inc ax                          ; Next entry
    jmp .scan_entries

.not_mounted:
    mov ax, FS_ERR_NO_DRIVER
    stc
    jmp .done

.end_of_dir:
    mov ax, FS_ERR_END_OF_DIR
    stc
    jmp .done

.read_error:
    mov ax, FS_ERR_READ_ERROR
    stc

.done:
    pop di
    pop si
    pop dx
    pop bx
    ret

; Local variables
.sector_off:      dw 0
.entry_idx:       dw 0
.current_entry:   dw 0

; ============================================================================
; FAT16 Write Functions (Build 258)
; ============================================================================

; fat16_create - Create a new file in FAT16 root directory
; Input: DS:SI = pointer to filename (null-terminated, "FILENAME.EXT")
; Output: CF=0 success, AX = file handle
;         CF=1 error, AX = error code
fat16_create:
    push es
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Convert filename to 8.3 FAT format (padded with spaces)
    sub sp, 12                      ; 11 bytes for name + 1 alignment
    mov di, sp
    push ss
    pop es

    ; Initialize with spaces
    mov cx, 11
    mov al, ' '
    push di
    rep stosb
    pop di

    ; Copy name part (up to 8 chars before '.')
    mov cx, 8
.f16c_copy_name:
    lodsb
    test al, al
    jz .f16c_name_done
    cmp al, '.'
    je .f16c_copy_ext
    cmp al, 'a'
    jb .f16c_store_name
    cmp al, 'z'
    ja .f16c_store_name
    sub al, 32
.f16c_store_name:
    mov [es:di], al
    inc di
    loop .f16c_copy_name
.f16c_skip_dot:
    lodsb
    test al, al
    jz .f16c_name_done
    cmp al, '.'
    jne .f16c_skip_dot

.f16c_copy_ext:
    mov di, sp
    add di, 8
    mov cx, 3
.f16c_copy_ext_loop:
    lodsb
    test al, al
    jz .f16c_name_done
    cmp al, 'a'
    jb .f16c_store_ext
    cmp al, 'z'
    ja .f16c_store_ext
    sub al, 32
.f16c_store_ext:
    mov [es:di], al
    inc di
    loop .f16c_copy_ext_loop

.f16c_name_done:
    ; Switch DS to kernel segment
    mov ax, 0x1000
    mov ds, ax

    ; Search root directory for free entry (0x00 or 0xE5)
    ; root dir sectors = (fat16_root_entries * 32) / 512
    movzx cx, byte [fat16_root_entries]     ; Low byte (entries / 16 sectors = entries >> 4)
    mov ax, [fat16_root_entries]
    shr ax, 4                       ; AX = number of root dir sectors (entries/16)
    mov cx, ax
    xor dx, dx                      ; DX = sector index (0-based)

.f16c_search_sector:
    cmp dx, cx
    jae .f16c_dir_full

    push cx
    push dx

    ; Calculate absolute root dir sector
    movzx eax, dx
    add eax, [fat16_root_start]

    ; Read sector into fat16_sector_buf
    push ds
    pop es
    mov bx, fat16_sector_buf
    call fat16_read_sector
    jc .f16c_read_error

    ; Search 16 entries in this sector
    mov cx, 16
    mov si, fat16_sector_buf
    xor bx, bx                     ; BX = byte offset within sector

.f16c_check_entry:
    mov al, [si]
    test al, al                     ; 0x00 = free
    jz .f16c_found_free
    cmp al, 0xE5                    ; Deleted = free
    je .f16c_found_free

    add si, 32
    add bx, 32
    dec cx
    jnz .f16c_check_entry

    ; Next sector
    pop dx
    pop cx
    inc dx
    jmp .f16c_search_sector

.f16c_dir_full:
    add sp, 12
    mov ax, FS_ERR_DIR_FULL
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16c_read_error:
    pop dx
    pop cx
    add sp, 12
    mov ax, FS_ERR_READ_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16c_found_free:
    ; Found free entry at fat16_sector_buf + BX
    mov [cs:.f16c_dir_offset], bx
    pop dx                          ; DX = sector index (relative to root start)
    mov [cs:.f16c_dir_sector], dx
    pop cx                          ; Sector count

    ; Allocate a cluster for the file
    call fat16_alloc_cluster
    jc .f16c_alloc_failed
    mov [cs:.f16c_start_cluster], ax

    ; Build directory entry at fat16_sector_buf + offset
    push ds
    pop es                          ; ES = 0x1000
    mov di, fat16_sector_buf
    add di, [cs:.f16c_dir_offset]

    ; Copy 8.3 filename from stack
    push ds
    push ss
    pop ds
    mov si, sp
    add si, 2                       ; Skip the DS we just pushed
    mov cx, 11
    rep movsb
    pop ds

    ; Set attributes and cluster/size
    mov byte [es:di], 0x20          ; Attribute = archive at offset +11
    inc di                          ; DI now at +12
    ; Clear bytes 12-25
    mov cx, 14
    xor al, al
    rep stosb                       ; DI now at +26
    ; Write starting cluster at +26
    mov ax, [cs:.f16c_start_cluster]
    mov [es:di], ax
    add di, 2                       ; DI at +28
    mov word [es:di], 0             ; File size low
    mov word [es:di + 2], 0         ; File size high

    ; Write modified directory sector back
    movzx eax, word [cs:.f16c_dir_sector]
    add eax, [fat16_root_start]
    mov bx, fat16_sector_buf
    call fat16_write_sector
    jc .f16c_write_error

    ; Allocate file handle
    call alloc_file_handle
    jc .f16c_no_handles

    ; Initialize file handle entry
    mov di, ax
    shl di, 5
    add di, file_table

    mov byte [di], 1                ; Status = open
    mov byte [di + 1], 1            ; Mount handle = FAT16
    mov cx, [cs:.f16c_start_cluster]
    mov [di + 2], cx                ; Starting cluster
    mov dword [di + 4], 0           ; File size = 0
    mov dword [di + 8], 0           ; Position = 0
    mov cx, [cs:.f16c_dir_sector]
    mov [di + 18], cx               ; Dir sector (relative index within root dir)
    mov cx, [cs:.f16c_dir_offset]
    mov [di + 20], cx               ; Dir entry offset within sector
    mov cx, [cs:.f16c_start_cluster]
    mov [di + 22], cx               ; Last cluster (= start, file is empty)

    ; Clean up and return handle in AX
    add sp, 12
    clc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16c_alloc_failed:
    add sp, 12
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16c_write_error:
    add sp, 12
    mov ax, FS_ERR_WRITE_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16c_no_handles:
    add sp, 12
    mov ax, FS_ERR_NO_HANDLES
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16c_dir_sector:    dw 0
.f16c_dir_offset:    dw 0
.f16c_start_cluster: dw 0

; ============================================================================
; fat16_write - Write data to an open FAT16 file
; Input: AX = file handle, ES:BX = data buffer, CX = byte count
; Output: AX = bytes written, CF=0 success; CF=1 error
; ============================================================================
fat16_write:
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Validate file handle
    cmp ax, FILE_MAX_HANDLES
    jae .f16w_invalid

    ; Get file table entry
    mov si, ax
    shl si, 5
    add si, file_table
    mov [cs:.f16w_handle_ptr], si

    ; Check if file is open
    cmp byte [si], 1
    jne .f16w_invalid

    ; Save write parameters
    mov [cs:.f16w_buffer_seg], es
    mov [cs:.f16w_buffer_off], bx
    mov [cs:.f16w_bytes_left], cx
    mov word [cs:.f16w_bytes_written], 0

    ; Get current cluster (last_cluster field, offset 22-23)
    mov ax, [si + 22]
    mov [cs:.f16w_current_cluster], ax

    ; Get current file size
    mov ax, [si + 4]
    mov [cs:.f16w_file_size], ax

    ; Get cluster size in bytes
    movzx ax, byte [fat16_sects_per_clust]
    shl ax, 9                       ; * 512
    mov [cs:.f16w_cluster_size], ax

.f16w_write_loop:
    mov cx, [cs:.f16w_bytes_left]
    test cx, cx
    jz .f16w_done

    ; Calculate position within current cluster
    ; sector_in_cluster = (file_size % cluster_size) / 512
    ; byte_in_sector = file_size % 512
    mov ax, [cs:.f16w_file_size]
    and ax, 0x01FF                  ; byte offset within sector (0-511)
    mov [cs:.f16w_sector_offset], ax

    ; Bytes we can write to this sector
    mov bx, 512
    sub bx, ax
    cmp cx, bx
    jbe .f16w_count_ok
    mov cx, bx
.f16w_count_ok:
    mov [cs:.f16w_chunk_size], cx

    ; Check if we need a new cluster
    ; Need new cluster when: file_size > 0 AND file_size is cluster-aligned
    mov ax, [cs:.f16w_file_size]
    test ax, ax
    jz .f16w_have_cluster           ; File is empty, use starting cluster

    ; Check if at cluster boundary
    mov bx, [cs:.f16w_cluster_size]
    xor dx, dx
    div bx                          ; DX = file_size % cluster_size
    test dx, dx
    jnz .f16w_have_cluster          ; Not at boundary

    ; At cluster boundary - allocate new cluster
    call fat16_alloc_cluster
    jc .f16w_write_error

    ; Link previous cluster to new one
    push ax
    mov dx, ax                      ; DX = new cluster number
    mov ax, [cs:.f16w_current_cluster]
    call fat16_set_fat_entry        ; Set prev -> new
    pop ax
    jc .f16w_write_error

    mov [cs:.f16w_current_cluster], ax

.f16w_have_cluster:
    ; If partial sector, read existing data first
    cmp word [cs:.f16w_sector_offset], 0
    je .f16w_clear_buf

    ; Calculate absolute sector for current position
    mov ax, [cs:.f16w_file_size]
    mov bx, [cs:.f16w_cluster_size]
    xor dx, dx
    div bx                          ; DX = offset within cluster
    mov ax, dx
    shr ax, 9                       ; AX = sector within cluster

    ; Absolute sector = data_start + (cluster-2) * sects_per_clust + sector_in_cluster
    push ax                         ; Save sector_in_cluster
    movzx eax, word [cs:.f16w_current_cluster]
    sub eax, 2
    movzx ebx, byte [fat16_sects_per_clust]
    imul eax, ebx
    pop bx                          ; BX = sector_in_cluster
    movzx ebx, bx
    add eax, ebx
    add eax, [fat16_data_start]

    push es
    push ds
    pop es                          ; ES = 0x1000
    mov bx, fat16_sector_buf
    call fat16_read_sector
    pop es
    jc .f16w_write_error
    jmp .f16w_do_copy

.f16w_clear_buf:
    ; Zero out sector buffer for clean write
    push es
    push di
    push cx
    push ds
    pop es
    mov di, fat16_sector_buf
    mov cx, 256
    xor ax, ax
    rep stosw
    pop cx
    pop di
    pop es

.f16w_do_copy:
    ; Copy data from caller's buffer to fat16_sector_buf
    push es
    push ds
    push si
    push di
    push cx

    mov ax, [cs:.f16w_buffer_seg]
    mov ds, ax
    mov si, [cs:.f16w_buffer_off]

    mov ax, 0x1000
    mov es, ax
    mov di, fat16_sector_buf
    add di, [cs:.f16w_sector_offset]

    mov cx, [cs:.f16w_chunk_size]
    rep movsb

    pop cx
    pop di
    pop si
    pop ds
    pop es

    ; Calculate absolute sector for write
    mov ax, [cs:.f16w_file_size]
    mov bx, [cs:.f16w_cluster_size]
    xor dx, dx
    div bx                          ; DX = offset within cluster
    mov ax, dx
    shr ax, 9                       ; AX = sector within cluster

    push ax
    movzx eax, word [cs:.f16w_current_cluster]
    sub eax, 2
    movzx ebx, byte [fat16_sects_per_clust]
    imul eax, ebx
    pop bx
    movzx ebx, bx
    add eax, ebx
    add eax, [fat16_data_start]

    ; Write sector
    push es
    push ds
    pop es
    mov bx, fat16_sector_buf
    call fat16_write_sector
    pop es
    jc .f16w_write_error

    ; Update counters
    mov cx, [cs:.f16w_chunk_size]
    add [cs:.f16w_bytes_written], cx
    sub [cs:.f16w_bytes_left], cx
    add [cs:.f16w_file_size], cx
    add [cs:.f16w_buffer_off], cx

    jmp .f16w_write_loop

.f16w_done:
    ; Update file table entry
    mov si, [cs:.f16w_handle_ptr]
    mov ax, [cs:.f16w_file_size]
    mov [si + 4], ax                ; File size low word
    mov word [si + 6], 0            ; High word stays 0
    mov ax, [cs:.f16w_current_cluster]
    mov [si + 22], ax               ; Update last cluster

    ; Update directory entry on disk
    movzx eax, word [si + 18]       ; Dir sector (relative index)
    add eax, [fat16_root_start]     ; Absolute LBA

    push es
    push ds
    pop es
    mov bx, fat16_sector_buf
    call fat16_read_sector
    pop es
    jc .f16w_write_error

    ; Patch file size in dir entry
    mov bx, [si + 20]              ; Dir entry offset within sector
    mov di, fat16_sector_buf
    add di, bx
    mov ax, [cs:.f16w_file_size]
    mov [di + 28], ax              ; File size low word
    mov word [di + 30], 0          ; File size high word

    ; Write dir sector back
    movzx eax, word [si + 18]
    add eax, [fat16_root_start]
    push es
    push ds
    pop es
    mov bx, fat16_sector_buf
    call fat16_write_sector
    pop es
    jc .f16w_write_error

    ; Return bytes written
    mov ax, [cs:.f16w_bytes_written]
    clc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.f16w_invalid:
    mov ax, FS_ERR_INVALID_HANDLE
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.f16w_write_error:
    mov ax, FS_ERR_WRITE_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.f16w_handle_ptr:      dw 0
.f16w_buffer_seg:      dw 0
.f16w_buffer_off:      dw 0
.f16w_bytes_left:      dw 0
.f16w_bytes_written:   dw 0
.f16w_current_cluster: dw 0
.f16w_file_size:       dw 0
.f16w_sector_offset:   dw 0
.f16w_chunk_size:      dw 0
.f16w_cluster_size:    dw 0

; ============================================================================
; fat16_delete - Delete a file from FAT16 root directory
; Input: DS:SI = pointer to filename (null-terminated, "FILENAME.EXT")
; Output: CF=0 success, CF=1 error (AX=error code)
; ============================================================================
fat16_delete:
    push es
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Convert filename to 8.3 FAT format
    sub sp, 12
    mov di, sp
    push ss
    pop es

    mov cx, 11
    mov al, ' '
    push di
    rep stosb
    pop di

    mov cx, 8
.f16d_copy_name:
    lodsb
    test al, al
    jz .f16d_name_done
    cmp al, '.'
    je .f16d_copy_ext
    cmp al, 'a'
    jb .f16d_store_name
    cmp al, 'z'
    ja .f16d_store_name
    sub al, 32
.f16d_store_name:
    mov [es:di], al
    inc di
    loop .f16d_copy_name
.f16d_skip_dot:
    lodsb
    test al, al
    jz .f16d_name_done
    cmp al, '.'
    jne .f16d_skip_dot

.f16d_copy_ext:
    mov di, sp
    add di, 8
    mov cx, 3
.f16d_copy_ext_loop:
    lodsb
    test al, al
    jz .f16d_name_done
    cmp al, 'a'
    jb .f16d_store_ext
    cmp al, 'z'
    ja .f16d_store_ext
    sub al, 32
.f16d_store_ext:
    mov [es:di], al
    inc di
    loop .f16d_copy_ext_loop

.f16d_name_done:
    ; Switch to kernel DS
    mov ax, 0x1000
    mov ds, ax

    ; Search root directory for the file
    mov ax, [fat16_root_entries]
    shr ax, 4                       ; Number of root dir sectors
    mov cx, ax
    xor dx, dx                      ; DX = sector index

.f16d_search_sector:
    cmp dx, cx
    jae .f16d_not_found

    push cx
    push dx

    ; Read root dir sector
    movzx eax, dx
    add eax, [fat16_root_start]
    push ds
    pop es
    mov bx, fat16_sector_buf
    call fat16_read_sector
    jc .f16d_read_error

    mov cx, 16
    mov si, fat16_sector_buf
    xor bx, bx

.f16d_check_entry:
    mov al, [si]
    test al, al
    jz .f16d_not_found_pop          ; End of directory
    cmp al, 0xE5
    je .f16d_next_entry

    ; Compare filename
    push si
    push di
    push ds
    push cx
    mov di, sp
    add di, 12                      ; Skip our 4 pushes (8 bytes) + loop pushes (4 bytes) = 12
    push ss
    pop es
    mov ax, 0x1000
    mov ds, ax
    mov cx, 11
    repe cmpsb
    pop cx
    pop ds
    pop di
    pop si
    je .f16d_found_file

.f16d_next_entry:
    add si, 32
    add bx, 32
    dec cx
    jnz .f16d_check_entry

    pop dx
    pop cx
    inc dx
    jmp .f16d_search_sector

.f16d_not_found:
    add sp, 12
    mov ax, FS_ERR_NOT_FOUND
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16d_not_found_pop:
    pop dx
    pop cx
    add sp, 12
    mov ax, FS_ERR_NOT_FOUND
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16d_read_error:
    pop dx
    pop cx
    add sp, 12
    mov ax, FS_ERR_READ_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16d_found_file:
    ; SI points to directory entry in fat16_sector_buf
    ; Get starting cluster before marking as deleted
    mov ax, [si + 0x1A]
    mov [cs:.f16d_start_cluster], ax

    ; Mark entry as deleted
    mov byte [si], 0xE5

    ; Write modified directory sector back
    ; Sector index is on stack from push dx
    mov bp, sp
    mov dx, [ss:bp]                 ; DX = sector index from push dx
    mov [cs:.f16d_dir_sector], dx

    movzx eax, dx
    add eax, [fat16_root_start]
    push ds
    pop es
    mov bx, fat16_sector_buf
    call fat16_write_sector
    jc .f16d_write_error_cleanup

    ; Walk FAT chain and free all clusters
    mov ax, [cs:.f16d_start_cluster]

.f16d_free_chain:
    cmp ax, 2
    jb .f16d_chain_done
    cmp ax, 0xFFF8
    jae .f16d_chain_done

    ; Get next cluster before freeing current
    push ax
    call fat16_get_next_cluster
    mov [cs:.f16d_next_cluster], ax
    jc .f16d_chain_end              ; Error or EOC
    cmp ax, 0xFFF8
    jae .f16d_chain_end
    ; Have valid next cluster
    pop ax
    push word [cs:.f16d_next_cluster]

    ; Free current cluster
    xor dx, dx                      ; 0 = free
    call fat16_set_fat_entry

    pop ax                          ; AX = next cluster
    jmp .f16d_free_chain

.f16d_chain_end:
    ; Free the last cluster
    pop ax
    xor dx, dx
    call fat16_set_fat_entry

.f16d_chain_done:
    pop dx                          ; Sector index
    pop cx                          ; Sector count
    add sp, 12                      ; Filename
    xor ax, ax
    clc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16d_write_error_cleanup:
    pop dx
    pop cx
    add sp, 12
    mov ax, FS_ERR_WRITE_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16d_dir_sector:    dw 0
.f16d_start_cluster: dw 0
.f16d_next_cluster:  dw 0

; ============================================================================
; fat16_rename - Rename a file in FAT16 root directory
; Input: DS:SI = old filename (dot format), caller_es:DI = new filename
; Output: CF=0 success, CF=1 error
; ============================================================================
fat16_rename:
    push es
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Convert OLD filename to 8.3 on stack
    sub sp, 12
    mov di, sp
    push ss
    pop es

    mov cx, 11
    mov al, ' '
    push di
    rep stosb
    pop di

    mov cx, 8
.f16r_copy_old_name:
    lodsb
    test al, al
    jz .f16r_old_done
    cmp al, '.'
    je .f16r_copy_old_ext
    cmp al, 'a'
    jb .f16r_store_old
    cmp al, 'z'
    ja .f16r_store_old
    sub al, 32
.f16r_store_old:
    mov [es:di], al
    inc di
    loop .f16r_copy_old_name
.f16r_skip_old_dot:
    lodsb
    test al, al
    jz .f16r_old_done
    cmp al, '.'
    jne .f16r_skip_old_dot
.f16r_copy_old_ext:
    mov di, sp
    add di, 8
    mov cx, 3
.f16r_copy_old_ext_loop:
    lodsb
    test al, al
    jz .f16r_old_done
    cmp al, 'a'
    jb .f16r_store_old_ext
    cmp al, 'z'
    ja .f16r_store_old_ext
    sub al, 32
.f16r_store_old_ext:
    mov [es:di], al
    inc di
    loop .f16r_copy_old_ext_loop

.f16r_old_done:
    ; Convert NEW filename to 8.3 on stack
    sub sp, 12
    mov di, sp
    push ss
    pop es

    mov cx, 11
    mov al, ' '
    push di
    rep stosb
    pop di

    ; Read new name from caller_es:DI_saved
    push ds
    mov ds, [cs:caller_es]
    ; Original DI is saved in push frame
    ; Stack: [12 new][12 old][BP][DI][SI][DX][CX][BX][ES]
    mov si, sp
    add si, 12 + 12 + 2             ; Skip new_name + old_name + BP
    mov si, [ss:si]                  ; SI = original DI (new filename pointer)

    mov cx, 8
.f16r_copy_new_name:
    lodsb
    test al, al
    jz .f16r_new_done
    cmp al, '.'
    je .f16r_copy_new_ext
    cmp al, 'a'
    jb .f16r_store_new
    cmp al, 'z'
    ja .f16r_store_new
    sub al, 32
.f16r_store_new:
    mov [es:di], al
    inc di
    loop .f16r_copy_new_name
.f16r_skip_new_dot:
    lodsb
    test al, al
    jz .f16r_new_done
    cmp al, '.'
    jne .f16r_skip_new_dot
.f16r_copy_new_ext:
    mov di, sp
    add di, 8
    mov cx, 3
.f16r_copy_new_ext_loop:
    lodsb
    test al, al
    jz .f16r_new_done
    cmp al, 'a'
    jb .f16r_store_new_ext
    cmp al, 'z'
    ja .f16r_store_new_ext
    sub al, 32
.f16r_store_new_ext:
    mov [es:di], al
    inc di
    loop .f16r_copy_new_ext_loop

.f16r_new_done:
    pop ds                          ; Restore kernel DS

    ; Search root directory for old filename
    mov ax, 0x1000
    mov ds, ax
    mov ax, [fat16_root_entries]
    shr ax, 4
    mov cx, ax
    xor dx, dx

.f16r_search_sector:
    cmp dx, cx
    jae .f16r_not_found

    push cx
    push dx

    movzx eax, dx
    add eax, [fat16_root_start]
    push ds
    pop es
    mov bx, fat16_sector_buf
    call fat16_read_sector
    jc .f16r_read_error

    mov cx, 16
    mov si, fat16_sector_buf
    xor bx, bx

.f16r_check_entry:
    mov al, [si]
    test al, al
    jz .f16r_not_found_pop
    cmp al, 0xE5
    je .f16r_next_entry

    ; Compare with old filename (at SP + 12 bytes new + 4 bytes our pushes + 4 bytes loop pushes = +20)
    push si
    push di
    push ds
    push cx
    ; Old name is second on stack: [12 new][12 old]
    ; From current SP: skip our 4 pushes (8) + loop pushes (4) + new name (12) = 24
    mov di, sp
    add di, 8 + 4 + 12             ; 8 for our pushes, 4 for loop cx/dx, 12 for new name
    push ss
    pop es
    mov ax, 0x1000
    mov ds, ax
    mov cx, 11
    repe cmpsb
    pop cx
    pop ds
    pop di
    pop si
    je .f16r_found

.f16r_next_entry:
    add si, 32
    add bx, 32
    dec cx
    jnz .f16r_check_entry

    pop dx
    pop cx
    inc dx
    jmp .f16r_search_sector

.f16r_not_found:
    add sp, 24                      ; Two 12-byte names
    mov ax, FS_ERR_NOT_FOUND
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16r_not_found_pop:
    pop dx
    pop cx
    add sp, 24
    mov ax, FS_ERR_NOT_FOUND
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16r_read_error:
    pop dx
    pop cx
    add sp, 24
    mov ax, FS_ERR_READ_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16r_found:
    ; SI points to dir entry. Overwrite first 11 bytes with new name.
    ; New name is at bottom of stack
    push ds
    push ss
    pop ds
    ; New name starts at SP + 2 (our push ds)
    mov di, si                      ; DI = dir entry in fat16_sector_buf
    mov ax, 0x1000
    mov es, ax
    mov si, sp
    add si, 2                       ; Skip push ds
    mov cx, 11
    rep movsb
    pop ds

    ; Write modified sector back
    mov bp, sp
    mov dx, [ss:bp]                 ; DX = sector index from push dx
    movzx eax, dx
    add eax, [fat16_root_start]
    push ds
    pop es
    mov bx, fat16_sector_buf
    call fat16_write_sector
    jc .f16r_write_error

    pop dx
    pop cx
    add sp, 24                      ; Two 12-byte names
    xor ax, ax
    clc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.f16r_write_error:
    pop dx
    pop cx
    add sp, 24
    mov ax, FS_ERR_WRITE_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

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

; alloc_segment - Allocate a free user segment from the pool
; Input: AL = task handle (owner)
; Output: BX = segment (e.g., 0x3000), CF clear on success
;         CF set if no free segments
alloc_segment:
    push cx
    push si
    xor cx, cx
.as_scan:
    cmp cx, APP_NUM_USER_SEGS
    jae .as_fail
    mov si, cx
    cmp byte [segment_owner + si], 0xFF
    je .as_found
    inc cx
    jmp .as_scan
.as_found:
    mov [segment_owner + si], al        ; Mark as owned by task
    shl si, 1
    mov bx, [segment_pool + si]         ; BX = segment value
    clc
    pop si
    pop cx
    ret
.as_fail:
    stc
    pop si
    pop cx
    ret

; free_segment - Return a segment to the pool
; Input: BX = segment to free
; Output: CF clear on success, CF set if segment not in pool
free_segment:
    push cx
    push si
    xor cx, cx
.fs_scan:
    cmp cx, APP_NUM_USER_SEGS
    jae .fs_fail
    mov si, cx
    shl si, 1
    cmp [segment_pool + si], bx
    je .fs_found
    inc cx
    jmp .fs_scan
.fs_found:
    shr si, 1
    mov byte [segment_owner + si], 0xFF
    clc
    pop si
    pop cx
    ret
.fs_fail:
    stc
    pop si
    pop cx
    ret

; app_load_stub - Load application from disk
; Input: DS:SI = Pointer to filename (8.3 format, space-padded)
;        DL = BIOS drive number (0=A:, 1=B:, 0x80=C:, etc.)
;        DH = Target segment: 0x20=shell (fixed), >0x20=auto-allocate user segment
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

    ; Save drive number, segment mode, and filename pointer
    mov [.drive], dl
    mov [.seg_mode], dh             ; Save DH for segment decision later
    mov [.filename_off], si
    ; Use caller_ds since DS is now kernel segment after INT 0x80 dispatch
    mov ax, [caller_ds]
    mov [.filename_seg], ax
    mov word [.target_seg], 0       ; Clear (will be set below)
    mov byte [.did_alloc], 0        ; No segment allocated yet

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

    ; Step 2: Determine target segment
    mov dh, [.seg_mode]
    cmp dh, 0x20
    ja .alloc_user_seg
    ; DH=0 or DH=0x20 → shell segment (fixed)
    mov word [.target_seg], APP_SEGMENT_SHELL
    jmp .seg_ready

.alloc_user_seg:
    ; DH > 0x20 → auto-allocate from segment pool
    mov ax, [.slot]
    call alloc_segment              ; AL=task handle, returns BX=segment
    jc .alloc_failed
    mov [.target_seg], bx
    mov byte [.did_alloc], 1        ; Remember we allocated (for error cleanup)

.seg_ready:
    ; Safety: For shell segment, terminate any RUNNING app there
    ; For user segments, pool guarantees no conflict
    cmp word [.target_seg], APP_SEGMENT_SHELL
    jne .skip_kill

    push di
    push cx
    mov bx, [.target_seg]
    mov di, app_table
    xor cx, cx
.kill_existing:
    cmp cx, APP_MAX_COUNT
    jae .kill_done
    cmp byte [di + APP_OFF_STATE], APP_STATE_RUNNING
    jne .kill_next
    cmp [di + APP_OFF_CODE_SEG], bx
    jne .kill_next
    ; Found running app at target segment - force terminate
    push bx
    mov bx, [di + APP_OFF_CODE_SEG]
    call free_segment               ; Free segment if in pool (no-op for shell)
    pop bx
    mov byte [di + APP_OFF_STATE], APP_STATE_FREE
    mov al, cl                      ; AL = task handle for destroy_task_windows
    call destroy_task_windows
.kill_next:
    add di, APP_ENTRY_SIZE
    inc cx
    jmp .kill_existing
.kill_done:
    pop cx
    pop di

.skip_kill:
    ; Step 3: Mount filesystem
    mov al, [.drive]                ; AL = drive number (fs_mount_stub expects AL, not DL!)
    xor ah, ah                      ; AH = 0 (auto-detect driver)
    call fs_mount_stub
    jc .mount_failed

    ; Step 4: Open file
    ; IMPORTANT: Read filename_off BEFORE changing DS, since local vars are in kernel segment
    mov si, [.filename_off]
    mov ax, [.filename_seg]
    mov ds, ax
    call fs_open_stub
    jc .file_not_found

    mov [.file_handle], ax

    ; Step 5: Get file size from file handle
    ; File handle entry is at file_table + handle * 32
    ; Size is at offset 4 (low) and 6 (high)
    mov bx, ax
    shl bx, 5
    add bx, file_table
    mov ax, 0x1000
    mov ds, ax
    mov cx, [bx + 4]                ; CX = file size (low word)
    mov [.file_size], cx

    mov word [.code_off], 0         ; Always load at offset 0

    ; Step 6: Read file into app code segment
    mov ax, [.file_handle]
    mov bx, [.target_seg]           ; Full segment word (e.g., 0x3000)
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
    mov ax, [.target_seg]
    mov [di + 2], ax                     ; Code segment (full word)
    mov ax, [.code_off]
    mov [di + 4], ax                     ; Code offset
    mov ax, [.file_size]
    mov [di + 6], ax                     ; Code size
    mov word [di + 8], 0                 ; Stack segment (set by app_start)
    mov word [di + 10], 0                ; Stack pointer (set by app_start)

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

.alloc_failed:
    mov ax, APP_ERR_ALLOC_FAILED
    jmp .error

.mount_failed:
    mov ax, APP_ERR_MOUNT_FAILED
    jmp .error_free_seg

.file_not_found:
    mov ax, APP_ERR_FILE_NOT_FOUND
    jmp .error_free_seg

.read_failed:
    ; Close file before returning error
    push word APP_ERR_READ_FAILED
    mov ax, [.file_handle]
    call fs_close_stub
    pop ax
    ; Fall through to error_free_seg

.error_free_seg:
    ; Free allocated segment on failure (if we allocated one)
    push ax
    cmp byte [.did_alloc], 0
    je .no_free
    mov bx, [.target_seg]
    call free_segment
.no_free:
    pop ax

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
.seg_mode:     db 0                     ; Original DH parameter
.target_seg:   dw 0                     ; Full target segment (e.g., 0x3000)
.did_alloc:    db 0                     ; 1 if we allocated from pool
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
; Cooperative Multitasking (v3.14.0)
; ============================================================================

; scheduler_next - Find next RUNNING task (round-robin)
; Output: AL = task handle (0xFF if none), BX = app_table entry offset
; Preserves: CX, DX, SI, DI
scheduler_next:
    push cx
    mov cl, [scheduler_last]
    inc cl
    and cl, 0x0F                    ; Wrap to 0-15
    mov ch, 16                      ; Check all 16 slots

.scan:
    cmp ch, 0
    je .none_found

    ; Calculate entry offset
    mov al, cl
    xor ah, ah
    mov bx, ax
    shl bx, 5                      ; BX = slot * 32
    add bx, app_table

    cmp byte [bx + APP_OFF_STATE], APP_STATE_RUNNING
    je .found

    inc cl
    and cl, 0x0F                    ; Wrap
    dec ch
    jmp .scan

.found:
    mov al, cl                      ; AL = task handle
    pop cx
    ret

.none_found:
    mov al, 0xFF
    pop cx
    ret

; app_yield_stub - Yield CPU to next task (API 34)
; Cooperative context switch: saves current task state, switches to next RUNNING task
; Called by apps via INT 0x80 with AH=34
app_yield_stub:
    ; Save all general-purpose registers on the current stack.
    ; Context switch swaps SS:SP — without this, registers get clobbered
    ; by whatever the other task was doing when it yielded.
    pusha

    ; --- Save current task context ---
    mov al, [current_task]
    cmp al, 0xFF
    je .no_save

    xor ah, ah
    mov si, ax
    shl si, 5                      ; SI = handle * 32
    add si, app_table

    ; Save draw_context per task
    mov al, [draw_context]
    mov [si + APP_OFF_DRAW_CTX], al
    ; Font is global (system setting from SETTINGS.CFG), not saved per task
    ; Save caller_ds/es per task
    mov ax, [caller_ds]
    mov [si + APP_OFF_CALLER_DS], ax
    mov ax, [caller_es]
    mov [si + APP_OFF_CALLER_ES], ax
    ; Save SS:SP (this captures the full stack state)
    mov [si + APP_OFF_STACK_SEG], ss
    mov [si + APP_OFF_STACK_PTR], sp

.no_save:
    ; --- Find next task ---
    call scheduler_next             ; AL = next handle, BX = entry offset
    cmp al, 0xFF
    je .idle

    cmp al, [current_task]
    je .same_task                   ; Only one task running, just return

    ; --- Switch to next task ---
    mov [current_task], al
    mov [scheduler_last], al

    ; Restore draw_context and recalculate clip state.
    ; clip_enabled / clip_x1/x2/y1/y2 are global state NOT saved per-task.
    ; Without this, a task inherits the previous task's clip rect, causing
    ; drawing to be clipped to the wrong window's bounds.
    mov al, [bx + APP_OFF_DRAW_CTX]
    mov [draw_context], al
    cmp al, 0xFF
    je .restore_no_clip
    cmp al, WIN_MAX_COUNT
    jae .restore_no_clip
    push bx
    call win_begin_draw             ; Sets clip_enabled=1, clip rect from window
    pop bx
    jmp .restore_clip_done
.restore_no_clip:
    mov byte [clip_enabled], 0
.restore_clip_done:
    ; Font is global (system setting), not restored per task
    ; Restore caller_ds/es
    mov ax, [bx + APP_OFF_CALLER_DS]
    mov [caller_ds], ax
    mov ax, [bx + APP_OFF_CALLER_ES]
    mov [caller_es], ax
    ; Restore SS:SP (this switches to the other task's stack!)
    cli
    mov ss, [bx + APP_OFF_STACK_SEG]
    mov sp, [bx + APP_OFF_STACK_PTR]
    sti

.same_task:
    popa                            ; Restore all general-purpose registers
    ret                             ; Returns via int80_return_point → pop ds → iret

.idle:
    sti
    hlt                             ; Wait for interrupt
    jmp .no_save                    ; Try scheduler again

; app_start_stub - Start task non-blocking (API 35)
; Input: AL = app handle (must be in LOADED state)
; Output: CF clear on success, CF set on error
app_start_stub:
    push bx
    push cx
    push es

    ; Validate handle
    xor ah, ah
    cmp ax, APP_MAX_COUNT
    jae .start_invalid

    ; Get app entry
    mov bx, ax
    shl bx, 5
    add bx, app_table

    ; Must be LOADED
    cmp byte [bx + APP_OFF_STATE], APP_STATE_LOADED
    jne .start_invalid

    ; Build initial stack frame in app's segment
    ; When the scheduler first switches to this task, yield_stub does
    ; popa (restores dummy registers) then ret → int80_return_point
    ; → pop ds → iret into the app.
    ;
    ; Stack layout (growing down from FFFE):
    ;   FFFC: task_exit_handler CS (0x1000)  ← for app's RETF exit
    ;   FFFA: task_exit_handler offset       ← for app's RETF exit
    ;   FFF8: FLAGS (0x0202, IF=1)           ← for IRET
    ;   FFF6: app CS                         ← for IRET
    ;   FFF4: app IP (0x0000)                ← for IRET
    ;   FFF2: app DS (= app CS)              ← for pop ds
    ;   FFF0: int80_return_point             ← for yield's ret
    ;   FFEE-FFE0: pusha frame (8 words)     ← for yield's popa
    ;   Saved SP = FFE0

    mov cx, [bx + APP_OFF_CODE_SEG] ; CX = app segment
    mov es, cx

    mov word [es:0xFFFC], 0x1000            ; task_exit CS
    mov word [es:0xFFFA], task_exit_handler  ; task_exit IP
    mov word [es:0xFFF8], 0x0202            ; FLAGS (IF=1)
    mov [es:0xFFF6], cx                     ; CS = app segment
    mov word [es:0xFFF4], 0x0000            ; IP = entry point
    mov [es:0xFFF2], cx                     ; DS = app segment
    mov word [es:0xFFF0], int80_return_point

    ; Dummy pusha frame (AX,CX,DX,BX,SP,BP,SI,DI — all zero)
    mov word [es:0xFFEE], 0                 ; AX
    mov word [es:0xFFEC], 0                 ; CX
    mov word [es:0xFFEA], 0                 ; DX
    mov word [es:0xFFE8], 0                 ; BX
    mov word [es:0xFFE6], 0                 ; SP (ignored by popa)
    mov word [es:0xFFE4], 0                 ; BP
    mov word [es:0xFFE2], 0                 ; SI
    mov word [es:0xFFE0], 0                 ; DI

    ; Save initial SS:SP to app_table
    mov [bx + APP_OFF_STACK_SEG], cx        ; SS = app segment
    mov word [bx + APP_OFF_STACK_PTR], 0xFFE0

    ; Initialize per-task saved context
    mov byte [bx + APP_OFF_DRAW_CTX], 0xFF  ; No draw context
    mov al, [current_font]
    mov [bx + APP_OFF_FONT], al              ; Inherit current system font
    mov [bx + APP_OFF_CALLER_DS], cx        ; caller_ds = app seg
    mov [bx + APP_OFF_CALLER_ES], cx        ; caller_es = app seg

    ; Mark as RUNNING
    mov byte [bx + APP_OFF_STATE], APP_STATE_RUNNING

    clc
    jmp .start_done

.start_invalid:
    stc

.start_done:
    pop es
    pop cx
    pop bx
    ret

; task_exit_handler - Reached when app does RETF (normal exit)
; The initial stack frame has CS:IP = 0x1000:task_exit_handler
task_exit_handler:
    ; We're in kernel CS (0x1000) since RETF popped our CS:IP
    mov ax, 0x1000
    mov ds, ax
    ; Fall through to app_exit_common

; app_exit_stub - Exit current task (API 36)
app_exit_stub:
    ; Silence speaker (in case task was playing sound)
    call speaker_off_stub

    ; Mark current task as FREE
    mov al, [current_task]
    cmp al, 0xFF
    je .exit_no_task

    xor ah, ah
    mov si, ax
    shl si, 5
    add si, app_table
    mov byte [si + APP_OFF_STATE], APP_STATE_FREE

    ; Free allocated segment back to pool (skip shell segment)
    mov bx, [si + APP_OFF_CODE_SEG]
    cmp bx, APP_SEGMENT_SHELL
    je .skip_free_seg
    call free_segment
.skip_free_seg:

    ; Clear draw context BEFORE destroying windows (prevents stale context during redraw)
    mov byte [draw_context], 0xFF
    mov byte [clip_enabled], 0

    ; Restore default font BEFORE destroying windows (so title bars render correctly)
    push ax
    mov al, 1
    call gfx_set_font
    pop ax

    ; Destroy all windows owned by this task
    push ax                         ; Save task handle
    call destroy_task_windows       ; ZF=1 if no windows destroyed
    pop ax

    ; Only repaint full desktop if no windows were destroyed (windowless/fullscreen app)
    ; Windowed apps already trigger redraw via win_destroy_stub
    jnz .skip_fullscreen_repaint
    mov word [redraw_old_x], 0
    mov word [redraw_old_y], 0
    mov ax, [screen_width]
    mov [redraw_old_w], ax
    mov ax, [screen_height]
    mov [redraw_old_h], ax
    call redraw_affected_windows
.skip_fullscreen_repaint:

    ; Find next task to run
    mov byte [current_task], 0xFF
    call scheduler_next
    cmp al, 0xFF
    je .exit_no_tasks

    ; Switch to next task
    mov [current_task], al
    mov [scheduler_last], al

    mov al, [bx + APP_OFF_DRAW_CTX]
    mov [draw_context], al
    ; Recalculate clip state from draw_context
    cmp al, 0xFF
    je .exit_no_clip
    cmp al, WIN_MAX_COUNT
    jae .exit_no_clip
    push bx
    call win_begin_draw
    pop bx
    jmp .exit_clip_done
.exit_no_clip:
    mov byte [clip_enabled], 0
.exit_clip_done:
    ; Restore font
    mov al, [bx + APP_OFF_FONT]
    push bx
    call gfx_set_font
    pop bx
    mov ax, [bx + APP_OFF_CALLER_DS]
    mov [caller_ds], ax
    mov ax, [bx + APP_OFF_CALLER_ES]
    mov [caller_es], ax

    cli
    mov ss, [bx + APP_OFF_STACK_SEG]
    mov sp, [bx + APP_OFF_STACK_PTR]
    sti
    popa                            ; Consume pusha frame from yielded task
    ret                             ; Now pops int80_return_point correctly

.exit_no_tasks:
    ; No tasks left - reload launcher
    call auto_load_launcher
.exit_no_task:
    ret

; destroy_task_windows - Destroy all windows owned by a task
; Input: AL = task handle to match against window owners
destroy_task_windows:
    push ax
    push bx
    push cx
    push si

    xor bx, bx                     ; BX = destroyed count
    mov si, window_table
    xor cx, cx                      ; CX = window handle counter

.dtw_loop:
    cmp cx, WIN_MAX_COUNT
    jae .dtw_done

    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .dtw_next
    cmp [si + WIN_OFF_OWNER], al
    jne .dtw_next

    ; Destroy this window
    push ax
    push cx
    mov al, cl                      ; AL = window handle
    call win_destroy_stub
    pop cx
    pop ax
    inc bx                          ; Count destroyed windows

.dtw_next:
    add si, WIN_ENTRY_SIZE
    inc cx
    jmp .dtw_loop

.dtw_done:
    test bx, bx                    ; ZF=1 if no windows destroyed (POP won't change flags)
    pop si
    pop cx
    pop bx
    pop ax
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
    call mouse_cursor_hide
    inc byte [cursor_locked]

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

    ; Auto-center windows in hi-res modes (no content scaling — text can't scale)
    mov byte [.save_scale], 1        ; Always 1: no content scaling
    cmp word [screen_width], 640
    jb .no_autocenter
    cmp dx, 400                      ; Width < 400 → designed for 320x200
    jae .no_autocenter
    cmp si, 300                      ; Height < 300 → designed for 320x200
    jae .no_autocenter
    ; Center window at its original size (don't scale dimensions)
    push ax
    mov ax, [screen_width]
    sub ax, dx
    shr ax, 1
    mov [.save_x], ax
    mov ax, [screen_height]
    sub ax, si
    shr ax, 1
    mov [.save_y], ax
    pop ax
.no_autocenter:

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
    mov al, [.save_scale]
    mov [bx + WIN_OFF_CONTENT_SCALE], al

    ; Demote all other visible windows' z-order before setting ours to top
    push si
    push cx
    mov si, window_table
    mov cx, WIN_MAX_COUNT
.demote_loop:
    cmp si, bx                      ; Skip our own entry
    je .demote_next
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .demote_next
    cmp byte [si + WIN_OFF_ZORDER], 0
    je .demote_next
    dec byte [si + WIN_OFF_ZORDER]
.demote_next:
    add si, WIN_ENTRY_SIZE
    loop .demote_loop
    pop cx
    pop si

    ; Redraw old topmost window's title bar as inactive
    push ax
    cmp byte [topmost_handle], 0xFF
    je .skip_old_create_redraw
    xor ah, ah
    mov al, [topmost_handle]
    call win_draw_stub
.skip_old_create_redraw:
    pop ax

    mov byte [bx + WIN_OFF_ZORDER], 15          ; Top of stack
    push ax
    mov al, [current_task]
    mov [bx + WIN_OFF_OWNER], al                ; Owned by creating task
    mov [focused_task], al                       ; New window gets keyboard focus
    pop ax

    ; Update topmost cache for z-order clipping
    push ax
    mov ax, bp
    mov [topmost_handle], al        ; BP = window handle (low byte)
    pop ax
    push word [bx + WIN_OFF_X]
    pop word [topmost_win_x]
    push word [bx + WIN_OFF_Y]
    pop word [topmost_win_y]
    push word [bx + WIN_OFF_WIDTH]
    pop word [topmost_win_w]
    push word [bx + WIN_OFF_HEIGHT]
    pop word [topmost_win_h]

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
    pop ds                          ; Balance push ds at find_slot (line 13588)

    ; Full z-order repaint of new window area: ensures background windows
    ; overlapping this area have their frames properly drawn (painter's algorithm).
    ; Without this, only the old topmost is redrawn above; lower z-order windows
    ; accumulate frame damage from content clears and are never repaired.
    mov bx, [.slot_off]
    push ds
    mov ax, 0x1000
    mov ds, ax
    mov ax, [bx + WIN_OFF_X]
    mov [redraw_old_x], ax
    mov ax, [bx + WIN_OFF_Y]
    mov [redraw_old_y], ax
    mov ax, [bx + WIN_OFF_WIDTH]
    mov [redraw_old_w], ax
    mov ax, [bx + WIN_OFF_HEIGHT]
    mov [redraw_old_h], ax
    pop ds
    call redraw_affected_windows

    ; Draw the new window's frame on top (redraw_affected_windows skips z=15)
    push ax
    mov ax, bp
    and al, 0x0F
    call win_draw_stub
    pop ax

    ; Clear content area so desktop background/icons don't show through
    push ds
    mov ax, 0x1000
    mov ds, ax
    mov bx, [.slot_off]
    mov ax, [bx + WIN_OFF_X]
    mov cx, [bx + WIN_OFF_Y]
    mov dx, [bx + WIN_OFF_WIDTH]
    mov si, [bx + WIN_OFF_HEIGHT]
    test byte [bx + WIN_OFF_FLAGS], WIN_FLAG_TITLE | WIN_FLAG_BORDER
    jz .create_clear_frameless
    inc ax                          ; Inside left border
    add cx, [titlebar_height]       ; Below title bar
    sub dx, 2                       ; Inside both borders
    sub si, [titlebar_height]
    dec si                          ; Above bottom border
.create_clear_frameless:
    mov bx, ax                      ; BX = X
    test si, si
    jz .create_skip_clear
    call gfx_clear_area_stub
.create_skip_clear:
    pop ds

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
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

.save_x:     dw 0
.save_y:     dw 0
.save_w:     dw 0
.save_h:     dw 0
.save_title: dw 0
.save_flags: db 0
.save_scale: db 1
.slot_off:   dw 0

; ============================================================================
; Desktop Icon APIs (v3.14.0)
; ============================================================================

; desktop_set_icon_stub - Register a desktop icon
; Input: AL = slot (0-7), BX = X pos, CX = Y pos
;        SI -> 76 bytes in caller's DS: 64B bitmap + 12B name
; Output: CF=0 success, CF=1 invalid slot
desktop_set_icon_stub:
    push di
    push si
    push cx
    push bx
    push ax
    push es
    push ds

    ; Validate slot
    cmp al, DESKTOP_MAX_ICONS
    jae .dsi_invalid

    ; Calculate destination: desktop_icons + (slot * 80)
    xor ah, ah
    mov di, DESKTOP_ICON_SIZE
    mul di                          ; AX = slot * 80
    add ax, desktop_icons
    mov di, ax                      ; DI = destination in kernel data

    ; Set kernel DS for writing
    mov ax, 0x1000
    mov ds, ax

    ; Store X and Y position
    mov [di + DESKTOP_ICON_OFF_X], bx
    mov [di + DESKTOP_ICON_OFF_Y], cx

    ; Copy 76 bytes (64B bitmap + 12B name) from caller's segment
    mov ax, [caller_ds]
    mov ds, ax                      ; DS = caller's segment
    ; DI = destination offset (kernel), SI = source offset (caller)
    ; Need ES:DI = kernel, DS:SI = caller
    mov ax, 0x1000
    mov es, ax
    add di, DESKTOP_ICON_OFF_BITMAP ; DI points to bitmap field
    mov cx, 76                      ; 64 bitmap + 12 name
    rep movsb

    ; Update icon count (in kernel DS)
    mov ax, 0x1000
    mov ds, ax
    ; Count occupied slots
    mov si, desktop_icons
    xor cx, cx                      ; Count
    mov al, DESKTOP_MAX_ICONS
.dsi_count:
    cmp word [si + DESKTOP_ICON_OFF_X], 0
    jne .dsi_has_icon
    cmp word [si + DESKTOP_ICON_OFF_Y], 0
    je .dsi_count_next
.dsi_has_icon:
    inc cl
.dsi_count_next:
    add si, DESKTOP_ICON_SIZE
    dec al
    jnz .dsi_count
    mov [desktop_icon_count], cl

    pop ds
    pop es
    pop ax
    pop bx
    pop cx
    pop si
    pop di
    clc
    ret

.dsi_invalid:
    pop ds
    pop es
    pop ax
    pop bx
    pop cx
    pop si
    pop di
    stc
    ret

; desktop_clear_icons_stub - Clear all desktop icons
; Input: none
; Output: CF=0 always
desktop_clear_icons_stub:
    push ax
    push cx
    push di
    push es

    mov ax, 0x1000
    mov es, ax
    mov di, desktop_icons
    mov cx, (DESKTOP_MAX_ICONS * DESKTOP_ICON_SIZE)
    xor al, al
    rep stosb
    mov byte [desktop_icon_count], 0

    pop es
    pop di
    pop cx
    pop ax
    clc
    ret

; gfx_draw_icon_stub - Draw a 16x16 2bpp icon to screen
; Input: BX = X position (should be divisible by 4 for byte alignment)
;        CX = Y position
;        SI -> 64-byte bitmap in caller's DS segment
; Output: none
; Handles CGA interlacing (even rows bank 0, odd rows bank 1)
gfx_draw_icon_stub:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push ds

    ; Set up video segment BEFORE changing DS (video_segment is DS-relative)
    mov ax, [video_segment]
    mov es, ax
    ; Check video mode before changing DS
    cmp byte [video_mode], 0x01
    je .icon_mode12h             ; VESA: share per-pixel icon path
    cmp byte [video_mode], 0x12
    je .icon_mode12h
    cmp byte [video_mode], 0x13
    je .icon_vga

    ; Set up source segment from caller_ds
    mov ax, [caller_ds]
    mov ds, ax

    ; Hide cursor during draw
    push bx
    push cx
    push ds
    mov ax, 0x1000
    mov ds, ax
    call mouse_cursor_hide
    inc byte [cursor_locked]
    pop ds
    pop cx
    pop bx

    ; Draw 16 rows, 4 bytes per row (CGA: 4 bytes = 16 pixels at 2bpp)
    mov dx, 16                      ; Row counter

.icon_row:
    ; Calculate CGA address for row at (BX, CX)
    ; Even rows: (CX/2)*80 + BX/4
    ; Odd rows:  (CX/2)*80 + BX/4 + 0x2000
    push cx
    push bx
    mov ax, cx
    shr ax, 1                       ; AX = Y / 2
    push dx
    mov di, 80
    mul di                          ; AX = (Y/2) * 80
    pop dx
    mov di, ax                      ; DI = row base

    mov ax, bx
    shr ax, 1
    shr ax, 1                       ; AX = X / 4
    add di, ax                      ; DI = row base + byte offset

    test cl, 1                      ; Odd row?
    jz .icon_even
    add di, 0x2000
.icon_even:
    ; Copy 4 bytes from DS:SI to ES:DI
    movsb
    movsb
    movsb
    movsb

    pop bx
    pop cx
    inc cx                          ; Next Y row
    dec dx
    jnz .icon_row
    jmp .icon_done

.icon_mode12h:
    ; Mode 12h: unpack 2bpp icon data, write via plot_pixel_color
    mov ax, [caller_ds]
    mov ds, ax
    ; Hide cursor
    push bx
    push cx
    push ds
    mov ax, 0x1000
    mov ds, ax
    call mouse_cursor_hide
    inc byte [cursor_locked]
    pop ds
    pop cx
    pop bx
    mov dx, 16                      ; 16 rows
.im12_row:
    push cx                         ; Save Y
    push bx                         ; Save X (API: BX=X, CX=Y)
    ; Unpack 4 source bytes = 16 pixels
    push dx
    mov di, 4                       ; 4 bytes per row
.im12_byte:
    lodsb                           ; AL = source byte (4 pixels at 2bpp)
    mov ah, al
    push di
    mov di, 4                       ; 4 pixels per byte
.im12_pixel:
    mov dl, ah
    shr dl, 6                       ; DL = 2bpp color
    ; plot_pixel_color needs CX=X, BX=Y, DL=color, ES=video seg
    ; Currently: BX=X, CX=Y — need to swap
    xchg bx, cx                    ; Now CX=X, BX=Y
    push ds
    push ax
    mov ax, 0x1000
    mov ds, ax
    call plot_pixel_color           ; Top-left pixel
    ; 2x scale: draw 2x2 block (DS=0x1000 here, screen_width is accessible)
    cmp word [screen_width], 640
    jb .im12_no_scale_pp
    inc cx                          ; X+1
    call plot_pixel_color           ; Top-right pixel
    dec cx                          ; Restore X
    inc bx                          ; Y+1
    call plot_pixel_color           ; Bottom-left pixel
    inc cx                          ; X+1
    call plot_pixel_color           ; Bottom-right pixel
    dec cx
    dec bx                          ; Restore Y
.im12_no_scale_pp:
    pop ax
    pop ds
    xchg bx, cx                    ; Restore BX=X, CX=Y
    shl ah, 2
    ; Advance X by 2 if scaling, 1 if not
    inc bx
    cmp word [cs:screen_width], 640
    jb .im12_noadv
    inc bx                          ; Extra pixel advance for 2x
.im12_noadv:
    dec di
    jnz .im12_pixel
    pop di
    dec di
    jnz .im12_byte
    pop dx
    pop bx                          ; Restore original X
    pop cx                          ; Restore Y
    ; Advance Y by 2 if scaling, 1 if not
    inc cx
    cmp word [cs:screen_width], 640
    jb .im12_noyadv
    inc cx
.im12_noyadv:
    dec dx
    jnz .im12_row
    jmp .icon_done

.icon_vga:
    ; VGA: unpack 2bpp icon data to 8bpp linear framebuffer
    mov ax, [caller_ds]
    mov ds, ax

    ; Hide cursor
    push bx
    push cx
    push ds
    mov ax, 0x1000
    mov ds, ax
    call mouse_cursor_hide
    inc byte [cursor_locked]
    pop ds
    pop cx
    pop bx

    mov dx, 16                      ; 16 rows
.iv_row:
    push cx                         ; Save Y
    push bx                         ; Save X
    ; Calculate VGA offset: Y*pitch + X
    mov ax, cx
    push dx
    mul word [cs:screen_pitch]      ; CS override: DS is caller_ds, not kernel!
    pop dx
    add ax, bx
    mov di, ax                      ; DI = Y*pitch + X
    ; Unpack 4 source bytes (16 pixels at 2bpp) to 16 VGA bytes
    push dx                         ; Save row counter
    mov cx, 4                       ; 4 source bytes
.iv_byte:
    lodsb                           ; AL = source byte (4 pixels at 2bpp)
    push cx
    mov cx, 4                       ; 4 pixels per byte
.iv_pixel:
    push ax
    shr al, 6                       ; Get top 2 bits = palette index
    stosb                           ; Write to VGA framebuffer
    pop ax
    shl al, 2                       ; Shift to next pixel
    dec cx
    jnz .iv_pixel
    pop cx
    dec cx
    jnz .iv_byte
    pop dx                          ; Restore row counter
    pop bx                          ; Restore X
    pop cx                          ; Restore Y
    inc cx                          ; Next Y row
    dec dx
    jnz .iv_row

.icon_done:
    ; Show cursor again
    push ds
    mov ax, 0x1000
    mov ds, ax
    dec byte [cursor_locked]
    call mouse_cursor_show
    pop ds

    pop ds
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; fs_read_header_stub - Read first N bytes from a file by name
; Opens the file, reads CX bytes into ES:DI, closes it
; Input: BX = mount handle (0=FAT12, 1=FAT16)
;        SI -> filename (null-terminated, in caller's segment)
;        ES:DI = buffer to read into (caller sets ES before INT 0x80)
;        CX = bytes to read
; Output: CF=0 success (AX=bytes read), CF=1 error
; Note: Uses caller_ds for the filename segment
fs_read_header_stub:
    push bx
    push dx
    push si
    push di

    ; Save read params
    mov [cs:.rh_count], cx
    mov [cs:.rh_buf_seg], es
    mov [cs:.rh_buf_off], di

    ; Open file: need DS:SI = filename in caller's segment
    ; Save mount handle
    mov [cs:.rh_mount], bx

    push ds
    mov ax, [cs:caller_ds]
    mov ds, ax                      ; DS = caller's segment for filename
    call fs_open_stub               ; BX = mount handle, DS:SI = filename
    pop ds                          ; fat12_open changes DS to 0x1000 internally
    jc .rh_open_err

    ; File opened, AX = file handle
    mov [cs:.rh_handle], ax

    ; Read CX bytes into ES:DI
    ; fs_read_stub: AX = handle, ES:DI = buffer, CX = count
    mov es, [cs:.rh_buf_seg]
    mov di, [cs:.rh_buf_off]
    mov cx, [cs:.rh_count]
    call fs_read_stub
    push ax                         ; Save bytes read / error
    pushf                           ; Save CF

    ; Close file regardless of read result
    mov ax, [cs:.rh_handle]
    call fs_close_stub

    popf                            ; Restore CF from read
    pop ax                          ; Restore bytes read

    pop di
    pop si
    pop dx
    pop bx
    ret

.rh_open_err:
    stc
    pop di
    pop si
    pop dx
    pop bx
    ret

.rh_mount: dw 0
.rh_handle: dw 0
.rh_count: dw 0
.rh_buf_seg: dw 0
.rh_buf_off: dw 0

; draw_desktop_region - Internal: paint desktop background + icons in a region
; Input: redraw_old_x/y/w/h define the region
; Called from redraw_affected_windows before the z-order window loop
draw_desktop_region:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Fill affected area with background color
    ; Use byte-level CGA fill for the background color
    mov bx, [redraw_old_x]
    mov cx, [redraw_old_y]
    mov dx, [redraw_old_w]
    mov si, [redraw_old_h]
    mov al, [desktop_bg_color]
    call gfx_fill_color             ; Fill rectangle with color AL

    ; Set draw_bg_color for icon label text on desktop
    mov al, [desktop_bg_color]
    mov [draw_bg_color], al

    ; Draw icons that overlap the affected rect (skip far-away icons to prevent corruption)
    ; Compute icon dimensions based on resolution (2x in 640x480+)
    mov word [.icon_size], 16       ; Lo-res: 16x16 icon
    mov word [.icon_bbox_w], 52     ; Lo-res: icon + label right extent
    mov word [.icon_bbox_h], 30     ; Lo-res: icon height + gap + label
    mov word [.label_shift], 12     ; Lo-res: label left shift
    cmp word [screen_width], 640
    jb .ddr_bounds_ok
    mov word [.icon_size], 32       ; Hi-res: 32x32 icon (2x scaled)
    mov word [.icon_bbox_w], 84     ; Hi-res: 32px icon + label right extent
    mov word [.icon_bbox_h], 50     ; Hi-res: 32px icon + gap + label
    mov word [.label_shift], 24     ; Hi-res: label left shift (2x)
.ddr_bounds_ok:

    mov si, desktop_icons
    xor bp, bp                      ; Icon counter
.ddr_icon_loop:
    cmp byte [desktop_icon_count], 0
    je .ddr_done
    xor ax, ax
    mov al, [desktop_icon_count]
    cmp bp, ax
    jae .ddr_done

    ; Bounds check: skip icons whose bounding box doesn't overlap affected rect
    ; Icon bbox: lo-res (x-12, y) to (x+52, y+30), hi-res (x-24, y) to (x+84, y+50)
    ; Test: icon fully right of rect?
    mov ax, [si + DESKTOP_ICON_OFF_X]
    mov dx, [redraw_old_x]
    add dx, [redraw_old_w]
    add dx, [.label_shift]          ; Account for label left shift
    cmp ax, dx
    jae .ddr_skip_icon
    ; Test: icon fully left of rect?
    mov ax, [si + DESKTOP_ICON_OFF_X]
    add ax, [.icon_bbox_w]          ; Icon bitmap + label right extent
    cmp ax, [redraw_old_x]
    jbe .ddr_skip_icon
    ; Test: icon fully below rect?
    mov ax, [si + DESKTOP_ICON_OFF_Y]
    mov dx, [redraw_old_y]
    add dx, [redraw_old_h]
    cmp ax, dx
    jae .ddr_skip_icon
    ; Test: icon fully above rect?
    mov ax, [si + DESKTOP_ICON_OFF_Y]
    add ax, [.icon_bbox_h]          ; Icon height + gap + label height
    cmp ax, [redraw_old_y]
    jbe .ddr_skip_icon

    ; Skip icons fully inside topmost window (avoids icon flash on window create)
    cmp byte [topmost_handle], 0xFF
    je .ddr_draw_icon
    mov ax, [si + DESKTOP_ICON_OFF_X]
    cmp ax, [topmost_win_x]
    jbe .ddr_draw_icon              ; Icon left of window left edge
    mov ax, [si + DESKTOP_ICON_OFF_Y]
    cmp ax, [topmost_win_y]
    jbe .ddr_draw_icon              ; Icon above window top edge
    mov ax, [si + DESKTOP_ICON_OFF_X]
    add ax, [.icon_size]            ; Icon right edge (16 or 32)
    mov dx, [topmost_win_x]
    add dx, [topmost_win_w]
    cmp ax, dx
    jae .ddr_draw_icon              ; Icon extends past window right
    mov ax, [si + DESKTOP_ICON_OFF_Y]
    add ax, [.icon_bbox_h]          ; Icon + label bottom edge
    mov dx, [topmost_win_y]
    add dx, [topmost_win_h]
    cmp ax, dx
    jae .ddr_draw_icon              ; Icon extends past window bottom
    jmp .ddr_skip_icon              ; Icon fully inside topmost, skip

.ddr_draw_icon:
    ; Icon overlaps affected rect - draw it
    push si
    push bp

    mov bx, [si + DESKTOP_ICON_OFF_X]
    mov cx, [si + DESKTOP_ICON_OFF_Y]
    add si, DESKTOP_ICON_OFF_BITMAP
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    call gfx_draw_icon_stub
    pop word [caller_ds]

    pop bp
    pop si
    push si
    push bp

    ; Draw icon name label below the icon (shifted left to match launcher)
    mov bx, [si + DESKTOP_ICON_OFF_X]
    sub bx, 8                      ; Match launcher's label offset
    mov cx, [si + DESKTOP_ICON_OFF_Y]
    add cx, 20                      ; 16px icon + 4px gap (lo-res default)
    cmp word [screen_width], 640
    jb .ddr_label_y_ok
    add cx, 20                      ; Hi-res: 32px icon + 8px gap = +40 total
    sub bx, 8                      ; Hi-res: wider label shift
.ddr_label_y_ok:
    push si
    add si, DESKTOP_ICON_OFF_NAME
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    call gfx_draw_string_stub
    pop word [caller_ds]
    pop si

    pop bp
    pop si

.ddr_skip_icon:
    add si, DESKTOP_ICON_SIZE
    inc bp
    jmp .ddr_icon_loop

.ddr_done:
    ; Restore draw_bg_color to 0 (prevent color pollution for window drawing)
    mov byte [draw_bg_color], 0
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.icon_size:    dw 16               ; 16 (lo-res) or 32 (hi-res)
.icon_bbox_w:  dw 52               ; Icon + label bounding box width
.icon_bbox_h:  dw 30               ; Icon + label bounding box height
.label_shift:  dw 12               ; Label left shift from icon X

; gfx_fill_color - Fill a rectangle with a specific CGA color
; Input: BX = X, CX = Y, DX = width, SI = height, AL = color (0-3)
; Uses CGA byte-level operations for speed
gfx_fill_color:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push bp

    ; Save color for slow path
    mov [.fill_color], al

    ; Build the color byte: replicate 2-bit color across all 4 pixels
    ; Color 0 = 0x00, Color 1 = 0x55, Color 2 = 0xAA, Color 3 = 0xFF
    and al, 3
    mov ah, al
    shl ah, 2
    or al, ah
    mov ah, al
    shl ah, 4
    or al, ah                       ; AL = color byte (4 identical pixels)
    mov [.fill_byte], al

    mov ax, [video_segment]
    mov es, ax
    mov bp, si                      ; BP = height counter

    ; Bounds check
    test dx, dx
    jz .gfc_done
    test bp, bp
    jz .gfc_done

    call mouse_cursor_hide
    inc byte [cursor_locked]

    ; VESA fast path: banked fill
    cmp byte [video_mode], 0x01
    je .gfc_vesa

    ; Mode 12h fast path: planar fill
    cmp byte [video_mode], 0x12
    je .gfc_mode12h

    ; VGA fast path: linear framebuffer, 1 byte per pixel
    cmp byte [video_mode], 0x13
    je .gfc_vga

    ; Check for byte-aligned fast path (BX % 4 == 0 AND DX % 4 == 0)
    mov ax, bx
    and ax, 3
    jnz .gfc_slow
    mov ax, dx
    and ax, 3
    jnz .gfc_slow

.gfc_row:
    ; Fast path: byte-aligned fill with rep stosb
    push cx                         ; Save Y
    push bx                         ; Save X
    push dx                         ; Save width

    ; Row base address
    mov ax, cx
    shr ax, 1
    push dx
    mov di, 80
    mul di
    pop dx
    mov di, ax                      ; DI = (Y/2) * 80

    test cl, 1
    jz .gfc_even
    add di, 0x2000
.gfc_even:
    ; Byte offset for X
    mov ax, bx
    shr ax, 1
    shr ax, 1                       ; AX = X / 4
    add di, ax                      ; DI = start byte in CGA memory

    ; Bytes to fill = width / 4
    mov cx, dx
    shr cx, 1
    shr cx, 1

    ; Fill bytes
    mov al, [.fill_byte]
    rep stosb

    pop dx                          ; Restore width
    pop bx                          ; Restore X
    pop cx                          ; Restore Y
    inc cx                          ; Next Y
    dec bp
    jnz .gfc_row
    jmp .gfc_cursor_done

.gfc_slow:
    ; Slow path: pixel-by-pixel for non-aligned rects
    ; On entry: BX=X, CX=Y, DX=width, BP=height
    ; plot_pixel_color: CX=X, BX=Y, DL=color (preserves all regs)
    mov [.save_x], bx
    mov [.save_w], dx
    xchg bx, cx                    ; BX=Y, CX=X
.gfc_srow:
    mov cx, [.save_x]              ; CX = start X
    mov si, [.save_w]              ; SI = width counter
.gfc_scol:
    mov dl, [.fill_color]
    call plot_pixel_color
    inc cx                          ; Next X
    dec si
    jnz .gfc_scol
    inc bx                          ; Next Y
    dec bp
    jnz .gfc_srow
    jmp .gfc_cursor_done            ; Prevent fall-through to VGA path

.gfc_vesa:
    ; VESA: banked fill
    mov si, bp                     ; Restore SI = height for vesa_fill_rect
    mov al, [.fill_color]
    call vesa_fill_rect             ; BX=X, CX=Y, DX=W, SI=H, AL=color
    jmp .gfc_cursor_done

.gfc_mode12h:
    ; Mode 12h: use planar fill helper
    mov si, bp                     ; Restore SI = height for mode12h_fill_rect
    mov al, [.fill_color]
    call mode12h_fill_rect          ; BX=X, CX=Y, DX=W, SI=H, AL=color
    jmp .gfc_cursor_done

.gfc_vga:
    ; VGA: linear framebuffer, 1 byte per pixel, rep stosb per row
    ; BX=X, CX=Y, DX=width, BP=height
    mov al, [.fill_color]
.gfc_vga_row:
    push cx                         ; Save Y
    push bx                         ; Save X
    push dx                         ; Save width
    ; Calculate offset: DI = Y * pitch + X
    mov ax, cx
    push dx
    mul word [screen_pitch]
    pop dx
    add ax, bx
    mov di, ax                      ; DI = Y * pitch + X
    mov cx, dx                      ; CX = width (rep count)
    mov al, [.fill_color]
    rep stosb                       ; Fill width bytes with color
    pop dx                          ; Restore width
    pop bx                          ; Restore X
    pop cx                          ; Restore Y
    inc cx                          ; Next Y
    dec bp
    jnz .gfc_vga_row
    jmp .gfc_cursor_done

.gfc_cursor_done:
    dec byte [cursor_locked]
    call mouse_cursor_show

.gfc_done:
    pop bp
    pop es
    pop di
    pop dx
    pop si
    pop cx
    pop bx
    pop ax
    ret

.fill_byte:  db 0
.fill_color: db 0
.save_x:     dw 0
.save_w:     dw 0

; redraw_affected_windows - Redraw windows that overlapped a cleared rectangle
; Input: Variables redraw_old_x/y/w/h set by caller
; Redraws frames and posts EVENT_WIN_REDRAW for each affected window
; Draws in z-order (lowest first) so topmost windows end up on top
redraw_affected_windows:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Disable clipping for all kernel-internal drawing in this function.
    ; The caller's task may have clip_enabled=1 with its window's clip rect,
    ; which would cause gfx_draw_string_stub to skip text outside that rect.
    ; This was the root cause of missing desktop icon labels on window close
    ; and corrupted window title bars during z-order redraws.
    push word [clip_enabled]
    mov byte [clip_enabled], 0

    ; Paint desktop background + icons in the affected region (if active)
    cmp byte [desktop_icon_count], 0
    je .no_desktop_repaint
    call draw_desktop_region
.no_desktop_repaint:

    ; Outer loop: z = 0 to 14 (draw lowest z first)
    ; Skip z=15 (topmost) - callers handle the topmost window separately
    mov byte [.cur_z], 0

.z_loop:
    cmp byte [.cur_z], 15
    jae .raw_done

    ; Inner loop: scan all windows for matching z-order
    mov si, window_table
    xor bp, bp                      ; BP = window handle counter

.raw_loop:
    cmp bp, WIN_MAX_COUNT
    jae .z_next

    ; Skip non-visible windows
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .raw_next

    ; Skip if z-order doesn't match current pass
    mov al, [si + WIN_OFF_ZORDER]
    cmp al, [.cur_z]
    jne .raw_next

    ; Rectangle intersection test: only process windows overlapping affected rect
    ; win_x < old_x + old_w  AND  old_x < win_x + win_w
    ; win_y < old_y + old_h  AND  old_y < win_y + win_h
    mov ax, [redraw_old_x]
    add ax, [redraw_old_w]
    cmp [si + WIN_OFF_X], ax
    jge .raw_next

    mov ax, [si + WIN_OFF_X]
    add ax, [si + WIN_OFF_WIDTH]
    cmp [redraw_old_x], ax
    jge .raw_next

    mov ax, [redraw_old_y]
    add ax, [redraw_old_h]
    cmp [si + WIN_OFF_Y], ax
    jge .raw_next

    mov ax, [si + WIN_OFF_Y]
    add ax, [si + WIN_OFF_HEIGHT]
    cmp [redraw_old_y], ax
    jge .raw_next

    ; Window overlaps affected rect - redraw frame and clipped content area
    push si
    push bp
    mov ax, bp                      ; AX = window handle
    and al, 0x0F
    call win_draw_stub

    ; Clear only the INTERSECTION of content area and affected rect.
    ; Clearing the full content area would erase higher-z windows that
    ; sit on top of this window but outside the affected rect.
    ; Content bounds: (win_x+1, win_y+TITLEBAR) to (win_x+win_w-1, win_y+win_h-1)
    ; Clip left = max(content_left, rect_left)
    mov bx, [si + WIN_OFF_X]
    inc bx                          ; BX = content left
    mov ax, [redraw_old_x]
    cmp ax, bx
    jle .raw_cl_ok
    mov bx, ax
.raw_cl_ok:
    ; Clip top = max(content_top, rect_top)
    mov cx, [si + WIN_OFF_Y]
    add cx, [titlebar_height]     ; CX = content top
    mov ax, [redraw_old_y]
    cmp ax, cx
    jle .raw_ct_ok
    mov cx, ax
.raw_ct_ok:
    ; Clip right = min(content_right, rect_right)
    mov dx, [si + WIN_OFF_X]
    add dx, [si + WIN_OFF_WIDTH]
    dec dx                          ; DX = content right
    mov ax, [redraw_old_x]
    add ax, [redraw_old_w]
    cmp ax, dx
    jge .raw_cr_ok
    mov dx, ax
.raw_cr_ok:
    ; Clip bottom = min(content_bottom, rect_bottom)
    mov ax, [si + WIN_OFF_Y]
    add ax, [si + WIN_OFF_HEIGHT]
    dec ax                          ; AX = content bottom
    push ax                         ; save content_bottom
    mov ax, [redraw_old_y]
    add ax, [redraw_old_h]
    pop si                          ; SI = content bottom
    cmp ax, si
    jge .raw_cb_ok
    mov si, ax                      ; SI = clipped bottom
.raw_cb_ok:
    ; Convert to (x, y, width, height) for gfx_clear_area_stub
    sub dx, bx                      ; DX = width
    jle .raw_skip_clear
    sub si, cx                      ; SI = height
    jle .raw_skip_clear
    call gfx_clear_area_stub

.raw_skip_clear:
    ; Post EVENT_WIN_REDRAW so app can redraw when focused
    mov al, EVENT_WIN_REDRAW
    mov dx, bp                      ; DX = window handle (BP preserved)
    call post_event

    pop bp
    pop si

.raw_next:
    add si, WIN_ENTRY_SIZE
    inc bp
    jmp .raw_loop

.z_next:
    inc byte [.cur_z]
    jmp .z_loop

.raw_done:
    ; After z-orders 0-14, redraw topmost window (z=15) if it overlaps
    cmp byte [topmost_handle], 0xFF
    je .topmost_done                ; No topmost window

    ; Find topmost window in table
    xor ax, ax
    mov al, [topmost_handle]
    mov si, ax
    shl si, 5
    add si, window_table
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .topmost_done

    ; Always redraw topmost frame (ensures it's on top after z-loop)
    xor ah, ah
    mov al, [topmost_handle]
    call win_draw_stub

    ; Only clear content + post WIN_REDRAW if topmost overlaps affected area
    mov ax, [redraw_old_x]
    add ax, [redraw_old_w]
    cmp [si + WIN_OFF_X], ax
    jge .topmost_done
    mov ax, [si + WIN_OFF_X]
    add ax, [si + WIN_OFF_WIDTH]
    cmp [redraw_old_x], ax
    jge .topmost_done
    mov ax, [redraw_old_y]
    add ax, [redraw_old_h]
    cmp [si + WIN_OFF_Y], ax
    jge .topmost_done
    mov ax, [si + WIN_OFF_Y]
    add ax, [si + WIN_OFF_HEIGHT]
    cmp [redraw_old_y], ax
    jge .topmost_done

    ; Topmost overlaps — clipped content clear (same algorithm as z-loop)
    mov bx, [si + WIN_OFF_X]
    inc bx
    mov ax, [redraw_old_x]
    cmp ax, bx
    jle .top_cl_ok
    mov bx, ax
.top_cl_ok:
    mov cx, [si + WIN_OFF_Y]
    add cx, [titlebar_height]
    mov ax, [redraw_old_y]
    cmp ax, cx
    jle .top_ct_ok
    mov cx, ax
.top_ct_ok:
    mov dx, [si + WIN_OFF_X]
    add dx, [si + WIN_OFF_WIDTH]
    dec dx
    mov ax, [redraw_old_x]
    add ax, [redraw_old_w]
    cmp ax, dx
    jge .top_cr_ok
    mov dx, ax
.top_cr_ok:
    mov ax, [si + WIN_OFF_Y]
    add ax, [si + WIN_OFF_HEIGHT]
    dec ax
    push ax
    mov ax, [redraw_old_y]
    add ax, [redraw_old_h]
    pop si
    cmp ax, si
    jge .top_cb_ok
    mov si, ax
.top_cb_ok:
    sub dx, bx
    jle .topmost_done
    sub si, cx
    jle .topmost_done
    call gfx_clear_area_stub

    ; Post WIN_REDRAW so topmost app repaints content
    mov al, EVENT_WIN_REDRAW
    xor dx, dx
    mov dl, [topmost_handle]
    call post_event

.topmost_done:
    pop word [clip_enabled]         ; Restore caller's clip state
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.cur_z: db 0

; Variables for redraw_affected_windows
redraw_old_x:   dw 0
redraw_old_y:   dw 0
redraw_old_w:   dw 0
redraw_old_h:   dw 0

; win_destroy_stub - Destroy a window
; Input:  AX = Window handle
; Output: CF = 0 on success
win_destroy_stub:
    call mouse_cursor_hide
    inc byte [cursor_locked]

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

    ; Save window bounds for redraw_affected_windows
    mov cx, [bx + WIN_OFF_X]
    mov [redraw_old_x], cx
    mov dx, [bx + WIN_OFF_Y]
    mov [redraw_old_y], dx
    mov si, [bx + WIN_OFF_WIDTH]
    mov [redraw_old_w], si
    mov di, [bx + WIN_OFF_HEIGHT]
    mov [redraw_old_h], di

    ; Clear window area
    push bx
    ; gfx_clear_area expects: BX=X, CX=Y, DX=Width, SI=Height
    mov bx, cx                      ; BX = X
    mov cx, dx                      ; CX = Y
    mov dx, si                      ; DX = Width
    mov si, di                      ; SI = Height
    call gfx_clear_area_stub
    pop bx

    ; Mark as free BEFORE redrawing (so this window isn't redrawn)
    mov byte [bx + WIN_OFF_STATE], WIN_STATE_FREE

    ; Invalidate topmost cache BEFORE redraw so draw_desktop_region
    ; doesn't skip icons under the now-destroyed window
    mov byte [topmost_handle], 0xFF

    ; Redraw any windows that were overlapped by this one
    call redraw_affected_windows

    ; Promote next highest z-order window to topmost (z=15)
    ; Without this, no window can draw after the topmost is destroyed
    mov si, window_table
    mov cx, WIN_MAX_COUNT
    mov byte [.best_z], 0
    mov byte [.best_handle], 0xFF
    xor bx, bx                     ; BX = window handle counter
.promote_scan:
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .promote_skip
    mov al, [si + WIN_OFF_ZORDER]
    cmp al, [.best_z]
    jb .promote_skip
    mov [.best_z], al
    mov [.best_handle], bl          ; BL = handle (0-15)
.promote_skip:
    add si, WIN_ENTRY_SIZE
    inc bl
    loop .promote_scan

    ; If we found a window, focus it and trigger redraw
    cmp byte [.best_handle], 0xFF
    jne .promote_focus
    ; No more visible windows — reset focused_task so launcher gets keyboard
    mov byte [focused_task], 0xFF
    mov byte [topmost_handle], 0xFF
    jmp .promote_done
.promote_focus:
    ; Save and disable clip state - stale clip from calling task can clip title text
    push word [clip_enabled]
    mov byte [clip_enabled], 0
    xor ah, ah
    mov al, [.best_handle]
    call win_focus_stub             ; Promotes to z=15, updates focused_task
    call win_draw_stub              ; Redraw frame at top
    pop word [clip_enabled]         ; Restore clip state
    ; Post EVENT_WIN_REDRAW so app redraws content
    mov al, EVENT_WIN_REDRAW
    xor dh, dh
    mov dl, [.best_handle]
    call post_event
.promote_done:

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
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

.best_z:      db 0
.best_handle: db 0xFF

; ============================================================================
; PC Speaker API
; ============================================================================

; speaker_tone_stub - Play a tone on the PC speaker (API 41)
; Input: BX = frequency in Hz (20-20000). BX=0 turns off speaker.
; Preserves: All registers
speaker_tone_stub:
    test bx, bx
    jz speaker_off_stub
    push ax
    push dx
    ; Program PIT Channel 2 for square wave (mode 3)
    mov al, 0xB6                    ; Channel 2, access lo/hi, mode 3, binary
    out 0x43, al
    ; Calculate divisor = 1193182 / frequency
    mov ax, 0x34DE                  ; Low word of 1193182 (0x001234DE)
    mov dx, 0x0012                  ; High word
    div bx                          ; AX = divisor
    out 0x42, al                    ; Low byte of divisor to PIT
    mov al, ah
    out 0x42, al                    ; High byte of divisor to PIT
    ; Enable speaker gate and data
    in al, 0x61
    or al, 0x03                     ; Set bits 0 (gate) and 1 (speaker)
    out 0x61, al
    pop dx
    pop ax
    ret

; speaker_off_stub - Turn off the PC speaker (API 42)
; Preserves: All registers
speaker_off_stub:
    push ax
    in al, 0x61
    and al, 0xFC                    ; Clear bits 0 and 1
    out 0x61, al
    pop ax
    ret

; get_boot_drive_stub - Get boot drive number (API 43)
; Output: AL = boot drive (0x00=floppy, 0x80=first HDD)
; Preserves: All other registers
get_boot_drive_stub:
    mov al, [boot_drive]
    ret

; ============================================================================
; Filesystem Write API Stubs (v3.19.0, Build 202)
; ============================================================================

; fs_write_sector_stub - Write raw sector to disk
; Input: SI = LBA sector number, ES:BX = 512-byte data buffer, DL = drive
; Output: CF=0 success, CF=1 error
fs_write_sector_stub:
    test dl, 0x80
    jnz .ws_not_supported           ; HD write not supported yet
    mov ax, si                      ; floppy_write_sector expects AX = LBA
    call floppy_write_sector
    ret
.ws_not_supported:
    stc
    ret

; fs_create_stub - Create new file
; Input: DS:SI = filename (dot format), BL = mount handle (0=FAT12)
; Output: AX = file handle, CF=0 success; CF=1 error (AX=error code)
; Note: Caller's DS:SI is in caller_ds:SI
fs_create_stub:
    cmp bl, 0
    jne .fc_check_fat16
    push ds
    mov ds, [cs:caller_ds]
    call fat12_create
    pop ds
    ret
.fc_check_fat16:
    cmp bl, 1
    jne .fc_not_supported
    push ds
    mov ds, [cs:caller_ds]
    call fat16_create
    pop ds
    ret
.fc_not_supported:
    mov ax, FS_ERR_NO_DRIVER
    stc
    ret

; fs_write_stub - Write data to open file
; Input: AL = file handle, ES:BX = data buffer, CX = byte count
; Output: AX = bytes written, CF=0 success; CF=1 error
; Note: Caller's ES:BX is in caller_es:BX
fs_write_stub:
    xor ah, ah                      ; Clear AH (was function number 46)
    ; Check file handle's mount_handle to route FAT12 vs FAT16
    push si
    mov si, ax
    shl si, 5
    add si, file_table
    cmp byte [si + 1], 1            ; Mount handle 1 = FAT16?
    pop si
    je .fsw_fat16
    push es
    mov es, [cs:caller_es]
    call fat12_write
    pop es
    ret
.fsw_fat16:
    push es
    mov es, [cs:caller_es]
    call fat16_write
    pop es
    ret

; fs_delete_stub - Delete a file
; Input: DS:SI = filename (dot format), BL = mount handle (0=FAT12)
; Output: CF=0 success, CF=1 error (AX=error code)
; Note: Caller's DS:SI is in caller_ds:SI
fs_delete_stub:
    cmp bl, 0
    jne .fd_check_fat16
    push ds
    mov ds, [cs:caller_ds]
    call fat12_delete
    pop ds
    ret
.fd_check_fat16:
    cmp bl, 1
    jne .fd_not_supported
    push ds
    mov ds, [cs:caller_ds]
    call fat16_delete
    pop ds
    ret
.fd_not_supported:
    mov ax, FS_ERR_NO_DRIVER
    stc
    ret

; ============================================================================
; fat12_create - Create a new file in FAT12 root directory
; Input: DS:SI = pointer to filename (null-terminated, "FILENAME.EXT")
; Output: CF=0 success, AX = file handle
;         CF=1 error, AX = error code
; ============================================================================
fat12_create:
    push es
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Convert filename to 8.3 FAT format (padded with spaces)
    sub sp, 12                      ; 11 bytes for name + 1 padding for alignment
    mov di, sp
    push ss
    pop es

    ; Initialize with spaces
    mov cx, 11
    mov al, ' '
    push di
    rep stosb
    pop di

    ; Copy name part (up to 8 chars before '.')
    mov cx, 8
.fc_copy_name:
    lodsb
    test al, al
    jz .fc_name_done
    cmp al, '.'
    je .fc_copy_ext
    ; Convert to uppercase
    cmp al, 'a'
    jb .fc_store_name
    cmp al, 'z'
    ja .fc_store_name
    sub al, 32
.fc_store_name:
    mov [es:di], al
    inc di
    loop .fc_copy_name
    ; Skip to dot
.fc_skip_dot:
    lodsb
    test al, al
    jz .fc_name_done
    cmp al, '.'
    jne .fc_skip_dot

.fc_copy_ext:
    mov di, sp
    add di, 8                       ; Point to extension part
    mov cx, 3
.fc_copy_ext_loop:
    lodsb
    test al, al
    jz .fc_name_done
    ; Convert to uppercase
    cmp al, 'a'
    jb .fc_store_ext
    cmp al, 'z'
    ja .fc_store_ext
    sub al, 32
.fc_store_ext:
    mov [es:di], al
    inc di
    loop .fc_copy_ext_loop

.fc_name_done:
    ; Switch DS to kernel segment
    mov ax, 0x1000
    mov ds, ax

    ; Search root directory for free entry (0x00 or 0xE5)
    mov ax, [root_dir_start]
    mov cx, 14                      ; Max 14 root dir sectors
    mov word [cs:.fc_dir_sector], 0 ; Track which sector has free entry

.fc_search_sector:
    push cx
    push ax
    mov [cs:.fc_dir_sector], ax     ; Save current sector LBA

    ; Read root dir sector into bpb_buffer
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_read_sector
    jc .fc_read_error

    ; Search 16 entries in this sector
    mov cx, 16
    mov si, bpb_buffer
    xor dx, dx                      ; DX = byte offset within sector

.fc_check_entry:
    mov al, [si]
    test al, al                     ; 0x00 = end of directory (free)
    jz .fc_found_free
    cmp al, 0xE5                    ; Deleted entry (free)
    je .fc_found_free

    add si, 32
    add dx, 32
    dec cx
    jnz .fc_check_entry

    ; Next sector
    pop ax
    pop cx
    inc ax
    dec cx
    jnz .fc_search_sector

    ; Directory full
    add sp, 12                      ; Clean up filename
    mov ax, FS_ERR_DIR_FULL
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fc_read_error:
    pop ax
    pop cx
    add sp, 12
    mov ax, FS_ERR_READ_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fc_found_free:
    ; Found free entry at bpb_buffer + DX
    ; Save dir entry offset
    mov [cs:.fc_dir_offset], dx     ; DX = 0-480 (entry offset in sector)

    ; Pop sector loop state
    pop ax                          ; Sector number (from push ax)
    pop cx                          ; Sector counter (from push cx)

    ; Allocate a cluster for the file
    call fat12_alloc_cluster
    jc .fc_alloc_failed
    mov [cs:.fc_start_cluster], ax

    ; Build directory entry at bpb_buffer + offset
    mov bx, 0x1000
    mov es, bx
    mov di, bpb_buffer
    mov dx, [cs:.fc_dir_offset]
    add di, dx                      ; DI = pointer to dir entry in buffer

    ; Copy 8.3 filename from stack
    push ds
    push ss
    pop ds
    ; Calculate filename position on stack
    ; Stack currently: [12-byte filename] [bp] [di] [si] [dx] [cx] [bx] [es]
    ; SP points past the push cx/ax we popped, so filename is at current SP + (amount pushed since sub sp)
    ; Actually filename is at a fixed position. Let me calculate properly.
    ; When we did sub sp, 12 the filename started at that SP. Let's use BP to find it.
    mov si, sp
    ; Stack: [12-byte filename is deep in the stack]
    ; Actually we need to find where the 12 bytes are relative to current SP
    ; After sub sp,12: filename at SP
    ; Then we pushed bp,di,si,dx,cx (from fat12_create entry) = already on stack before sub sp
    ; Wait - sub sp,12 was AFTER all the pushes. So filename is at SP + 0 from when we did sub sp.
    ; But then we pushed more stuff: cx, ax (from .fc_search_sector loop), and popped them.
    ; So the filename is still at the same position.
    ; Current stack layout:
    ;   SP → [pushed by us in between...]
    ; Let me just save BP at entry and use it.
    ; Actually the simplest approach: after sub sp,12, the filename is at the SP value at that point.
    ; Since we've popped cx and ax from the loop, and haven't pushed anything else new,
    ; the filename should be right at current SP position going back...
    ;
    ; Let me reconsider. At .fc_name_done we switched DS. Since then:
    ;   push cx, push ax (loop), pop ax, pop cx - net 0
    ; So the filename is still at exactly where sub sp,12 left it.
    ; But we also did push ds just now. So filename is at SP + 2.
    add si, 2                       ; Skip the DS we just pushed
    mov cx, 11
    rep movsb                       ; Copy filename to dir entry
    pop ds

    ; Set attributes and cluster/size
    mov byte [es:di], 0x20          ; Attribute = archive at offset +11
    inc di                          ; DI now at +12
    ; Clear bytes 12-25 (reserved, time, date fields)
    mov cx, 14
    xor al, al
    rep stosb                       ; DI now at offset +26
    ; Write starting cluster at offset +26
    mov ax, [cs:.fc_start_cluster]
    mov [es:di], ax                 ; Starting cluster
    add di, 2                       ; DI at offset +28
    mov word [es:di], 0             ; File size low
    mov word [es:di + 2], 0         ; File size high

    ; Write modified directory sector back to disk
    mov ax, [cs:.fc_dir_sector]
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_write_sector
    jc .fc_write_error

    ; Allocate file handle
    call alloc_file_handle
    jc .fc_no_handles

    ; Initialize file handle entry
    mov di, ax
    shl di, 5
    add di, file_table

    mov byte [di], 1                ; Status = open
    mov byte [di + 1], 0            ; Mount handle = FAT12
    mov cx, [cs:.fc_start_cluster]
    mov [di + 2], cx                ; Starting cluster
    mov dword [di + 4], 0           ; File size = 0
    mov dword [di + 8], 0           ; Position = 0
    ; Save dir sector and offset for later size updates
    mov cx, [cs:.fc_dir_sector]
    mov [di + 18], cx               ; Dir sector LBA
    mov cx, [cs:.fc_dir_offset]
    mov [di + 20], cx               ; Dir entry offset (word)
    mov cx, [cs:.fc_start_cluster]
    mov [di + 22], cx               ; Last cluster (= start, file is empty)

    ; Clean up and return handle in AX
    add sp, 12                      ; Clean up filename
    clc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fc_alloc_failed:
    add sp, 12
    ; AX already has error code from fat12_alloc_cluster
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fc_write_error:
    add sp, 12
    mov ax, FS_ERR_WRITE_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fc_no_handles:
    add sp, 12
    mov ax, FS_ERR_NO_HANDLES
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fc_dir_sector:  dw 0
.fc_dir_offset:  dw 0
.fc_start_cluster: dw 0

; ============================================================================
; fat12_write - Write data to an open FAT12 file
; Input: AX = file handle, ES:BX = data buffer, CX = byte count
; Output: AX = bytes written, CF=0 success; CF=1 error
; ============================================================================
fat12_write:
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Validate file handle
    cmp ax, FILE_MAX_HANDLES
    jae .fw_invalid

    ; Get file table entry
    mov si, ax
    shl si, 5
    add si, file_table
    mov [cs:.fw_handle_ptr], si     ; Save pointer to file table entry

    ; Check if file is open
    cmp byte [si], 1
    jne .fw_invalid

    ; Save write parameters
    mov [cs:.fw_buffer_seg], es     ; Caller's buffer segment
    mov [cs:.fw_buffer_off], bx     ; Caller's buffer offset
    mov [cs:.fw_bytes_left], cx     ; Bytes remaining to write
    mov word [cs:.fw_bytes_written], 0

    ; Get current cluster (last_cluster field, byte 22-23)
    mov ax, [si + 22]              ; Last cluster
    mov [cs:.fw_current_cluster], ax

    ; Get current file size (used to determine position within cluster)
    mov ax, [si + 4]               ; File size low word
    mov [cs:.fw_file_size], ax

.fw_write_loop:
    mov cx, [cs:.fw_bytes_left]
    test cx, cx
    jz .fw_done                     ; No more bytes to write

    ; Calculate position within current cluster (file_size % 512)
    mov ax, [cs:.fw_file_size]
    and ax, 0x01FF                  ; AX = byte offset within sector (0-511)
    mov [cs:.fw_sector_offset], ax

    ; Bytes we can write to this sector
    mov bx, 512
    sub bx, ax                      ; BX = space left in current sector
    cmp cx, bx
    jbe .fw_count_ok
    mov cx, bx                      ; Cap at remaining space in sector
.fw_count_ok:
    mov [cs:.fw_chunk_size], cx

    ; If sector_offset > 0, we need to read existing sector first (partial write)
    cmp word [cs:.fw_sector_offset], 0
    je .fw_skip_read

    ; Read existing sector into bpb_buffer
    mov ax, [cs:.fw_current_cluster]
    sub ax, 2
    add ax, [data_area_start]       ; AX = data sector LBA
    push es
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_read_sector
    pop es
    jc .fw_write_error
    jmp .fw_copy_data

.fw_skip_read:
    ; Starting at sector boundary - check if we need a new cluster
    ; If file_size > 0 and file_size is sector-aligned, we need a new cluster
    mov ax, [cs:.fw_file_size]
    test ax, ax
    jz .fw_copy_data                ; File is empty, use the starting cluster

    ; File_size > 0 and sector-aligned: allocate new cluster
    call fat12_alloc_cluster
    jc .fw_write_error

    ; Link previous cluster to new one
    push ax                         ; Save new cluster
    mov dx, ax                      ; DX = new cluster number
    mov ax, [cs:.fw_current_cluster] ; AX = previous cluster
    call fat12_set_fat_entry        ; Set prev -> new
    pop ax
    jc .fw_write_error

    ; Update current cluster
    mov [cs:.fw_current_cluster], ax

.fw_copy_data:
    ; Clear bpb_buffer if writing full sector from offset 0
    cmp word [cs:.fw_sector_offset], 0
    jne .fw_do_copy
    ; Zero out bpb_buffer first
    push es
    push di
    push cx
    mov ax, 0x1000
    mov es, ax
    mov di, bpb_buffer
    mov cx, 256                     ; 512 bytes / 2
    xor ax, ax
    rep stosw
    pop cx
    pop di
    pop es

.fw_do_copy:
    ; Copy data from caller's buffer to bpb_buffer at sector_offset
    push es
    push ds
    push si
    push di
    push cx

    ; Source: caller's buffer
    mov ax, [cs:.fw_buffer_seg]
    mov ds, ax
    mov si, [cs:.fw_buffer_off]

    ; Destination: bpb_buffer + sector_offset
    mov ax, 0x1000
    mov es, ax
    mov di, bpb_buffer
    add di, [cs:.fw_sector_offset]

    ; CX already set to chunk size
    rep movsb

    pop cx
    pop di
    pop si
    pop ds
    pop es

    ; Write sector to disk
    mov ax, [cs:.fw_current_cluster]
    sub ax, 2
    add ax, [data_area_start]       ; AX = data sector LBA
    push es
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_write_sector
    pop es
    jc .fw_write_error

    ; Update counters
    mov cx, [cs:.fw_chunk_size]
    add [cs:.fw_bytes_written], cx
    sub [cs:.fw_bytes_left], cx
    add [cs:.fw_file_size], cx
    add [cs:.fw_buffer_off], cx

    jmp .fw_write_loop

.fw_done:
    ; Update file table entry with new size and last cluster
    mov si, [cs:.fw_handle_ptr]
    mov ax, [cs:.fw_file_size]
    mov [si + 4], ax                ; Update file size low word
    mov word [si + 6], 0            ; High word stays 0 (files < 64KB)
    mov ax, [cs:.fw_current_cluster]
    mov [si + 22], ax               ; Update last cluster

    ; Update directory entry on disk with new file size
    mov ax, [si + 18]               ; Dir sector LBA
    push es
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_read_sector         ; Read dir sector
    pop es
    jc .fw_write_error

    ; Update size in directory entry
    mov bx, [si + 20]              ; Dir entry offset within sector (word)
    mov di, bpb_buffer
    add di, bx
    mov ax, [cs:.fw_file_size]
    mov [di + 28], ax               ; File size low word
    mov word [di + 30], 0           ; File size high word

    ; Write dir sector back
    mov ax, [si + 18]
    push es
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_write_sector
    pop es
    jc .fw_write_error

    ; Return bytes written
    mov ax, [cs:.fw_bytes_written]
    clc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.fw_invalid:
    mov ax, FS_ERR_INVALID_HANDLE
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.fw_write_error:
    mov ax, FS_ERR_WRITE_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.fw_handle_ptr:     dw 0
.fw_buffer_seg:     dw 0
.fw_buffer_off:     dw 0
.fw_bytes_left:     dw 0
.fw_bytes_written:  dw 0
.fw_current_cluster: dw 0
.fw_file_size:      dw 0
.fw_sector_offset:  dw 0
.fw_chunk_size:     dw 0

; ============================================================================
; fat12_delete - Delete a file from FAT12 root directory
; Input: DS:SI = pointer to filename (null-terminated, "FILENAME.EXT")
; Output: CF=0 success, CF=1 error (AX=error code)
; ============================================================================
fat12_delete:
    push es
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Convert filename to 8.3 FAT format (same as fat12_open)
    sub sp, 12
    mov di, sp
    push ss
    pop es

    mov cx, 11
    mov al, ' '
    push di
    rep stosb
    pop di

    mov cx, 8
.fd_copy_name:
    lodsb
    test al, al
    jz .fd_name_done
    cmp al, '.'
    je .fd_copy_ext
    cmp al, 'a'
    jb .fd_store_name
    cmp al, 'z'
    ja .fd_store_name
    sub al, 32
.fd_store_name:
    mov [es:di], al
    inc di
    loop .fd_copy_name
.fd_skip_dot:
    lodsb
    test al, al
    jz .fd_name_done
    cmp al, '.'
    jne .fd_skip_dot

.fd_copy_ext:
    mov di, sp
    add di, 8
    mov cx, 3
.fd_copy_ext_loop:
    lodsb
    test al, al
    jz .fd_name_done
    cmp al, 'a'
    jb .fd_store_ext
    cmp al, 'z'
    ja .fd_store_ext
    sub al, 32
.fd_store_ext:
    mov [es:di], al
    inc di
    loop .fd_copy_ext_loop

.fd_name_done:
    ; Switch to kernel DS
    mov ax, 0x1000
    mov ds, ax

    ; Search root directory for the file
    mov ax, [root_dir_start]
    mov cx, 14

.fd_search_sector:
    push cx
    push ax

    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_read_sector
    jc .fd_read_error

    mov cx, 16
    mov si, bpb_buffer
    xor dx, dx                      ; DX = byte offset within sector

.fd_check_entry:
    mov al, [si]
    test al, al
    jz .fd_not_found_pop            ; End of directory
    cmp al, 0xE5
    je .fd_next_entry

    ; Compare filename: DS:SI=dir entry, ES:DI=8.3 name on stack
    push si
    push di
    push ds
    push cx
    ; Stack: [cx][ds][di][si] [ax][cx from loop] [12-byte name] ...
    ; Filename is at SP + 8 (our pushes) + 4 (loop pushes) = SP + 12
    mov di, sp
    add di, 12                      ; Point to 8.3 name on stack
    push ss
    pop es
    mov ax, 0x1000
    mov ds, ax
    mov cx, 11
    repe cmpsb
    pop cx
    pop ds
    pop di
    pop si
    je .fd_found_file

.fd_next_entry:
    add si, 32
    add dx, 32
    dec cx
    jnz .fd_check_entry

    pop ax
    pop cx
    inc ax
    dec cx
    jnz .fd_search_sector

    ; File not found
    add sp, 12
    mov ax, FS_ERR_NOT_FOUND
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fd_not_found_pop:
    pop ax
    pop cx
    add sp, 12
    mov ax, FS_ERR_NOT_FOUND
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fd_read_error:
    pop ax
    pop cx
    add sp, 12
    mov ax, FS_ERR_READ_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fd_found_file:
    ; SI points to directory entry in bpb_buffer
    ; Get starting cluster before marking as deleted
    mov ax, [si + 0x1A]             ; Starting cluster
    mov [cs:.fd_start_cluster], ax

    ; Mark entry as deleted
    mov byte [si], 0xE5

    ; Save current dir sector LBA (it's on the stack from push ax)
    ; Stack: AX (sector), CX (counter) under us
    mov bp, sp
    mov ax, [ss:bp]                 ; This is the saved SI actually... no
    ; Actually the search compare block popped everything. The loop pushes are:
    ; [AX=sector] [CX=counter] are next on stack
    ; Wait - we jumped to .fd_found_file from je, which was after pop si. So:
    ;   pop cx, pop ds, pop di, pop si all happened
    ;   AX (sector) and CX (counter) are on stack
    mov bp, sp
    mov ax, [ss:bp]                 ; AX = sector number from push ax
    mov [cs:.fd_dir_sector], ax

    ; Write modified directory sector back
    push es
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_write_sector
    pop es
    jc .fd_write_error_cleanup

    ; Now walk the FAT chain and free all clusters
    mov ax, [cs:.fd_start_cluster]

.fd_free_chain:
    cmp ax, 2
    jb .fd_chain_done               ; Invalid cluster
    cmp ax, 0xFF8
    jae .fd_chain_done              ; End of chain

    ; Save current cluster and get next before zeroing
    push ax
    call get_next_cluster           ; Get next cluster in AX
    mov [cs:.fd_next_cluster], ax
    mov cx, ax                      ; CX = carry flag state... no
    ; CF set if end of chain
    pushf                           ; Save CF state
    pop bx                          ; BX = flags
    pop ax                          ; AX = current cluster to free

    ; Zero out this FAT entry
    xor dx, dx                      ; DX = 0 (free)
    call fat12_set_fat_entry

    ; Check if we should continue
    test bx, 1                      ; Test CF in saved flags
    jnz .fd_chain_done              ; End of chain reached
    mov ax, [cs:.fd_next_cluster]
    jmp .fd_free_chain

.fd_chain_done:
    ; Clean up and return success
    pop ax                          ; Sector number
    pop cx                          ; Sector counter
    add sp, 12                      ; Filename
    xor ax, ax
    clc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fd_write_error_cleanup:
    pop ax
    pop cx
    add sp, 12
    mov ax, FS_ERR_WRITE_ERROR
    stc
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.fd_dir_sector:    dw 0
.fd_start_cluster: dw 0
.fd_next_cluster:  dw 0

; win_draw_stub - Draw/redraw window frame (title bar and border)
; Input:  AX = Window handle
; Output: CF = 0 on success
win_draw_stub:
    call mouse_cursor_hide
    inc byte [cursor_locked]

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

    ; Use win_color for window chrome, black bg for title bar text
    push ax
    mov al, [win_color]
    mov [draw_fg_color], al
    mov byte [draw_bg_color], 0
    pop ax

    mov bx, ax
    shl bx, 5
    add bx, window_table

    ; Check if window is visible
    cmp byte [bx + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .invalid

    ; Skip drawing for frameless windows (no title, no border)
    test byte [bx + WIN_OFF_FLAGS], WIN_FLAG_TITLE | WIN_FLAG_BORDER
    jz .nodraw

    mov [.win_ptr], bx

    ; Get window dimensions
    mov cx, [bx + WIN_OFF_X]
    mov dx, [bx + WIN_OFF_Y]
    mov si, [bx + WIN_OFF_WIDTH]
    mov di, [bx + WIN_OFF_HEIGHT]

    ; Choose titlebar font based on resolution
    mov al, [current_font]
    mov [.saved_font], al
    cmp word [titlebar_height], 14
    jae .use_tb_font2
    ; Lo-res: force font 1 (8x8) — fits in 10px titlebar
    cmp al, 1
    je .font_ok
    push ax
    mov al, 1
    call gfx_set_font
    pop ax
    jmp .font_ok
.use_tb_font2:
    ; Hi-res: use font 2 (8x14) — fits in 18px titlebar
    cmp al, 2
    je .font_ok
    push ax
    mov al, 2
    call gfx_set_font
    pop ax
.font_ok:

    ; Check if this is the active (topmost) window
    cmp byte [bx + WIN_OFF_ZORDER], 15
    jne .draw_inactive_titlebar

    ; === Active window titlebar ===
    push bx
    push cx
    push dx
    push si
    push di
    mov bx, cx                      ; BX = X
    mov cx, dx                      ; CX = Y
    mov dx, si                      ; DX = Width
    mov si, [titlebar_height]     ; SI = Height (10)
    cmp byte [widget_style], 0
    je .active_flat_fill
    mov al, SYS_TITLE_ACTIVE
    call gfx_draw_filled_rect_color ; 3D: blue titlebar
    jmp .active_fill_done
.active_flat_fill:
    call gfx_draw_filled_rect_stub  ; Flat: white titlebar
.active_fill_done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx

    ; Draw title text
    push bx
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    mov si, bx
    add si, WIN_OFF_TITLE
    mov bx, cx
    add bx, 4
    mov cx, dx
    add cx, 1
    cmp byte [widget_style], 0
    je .active_flat_text
    mov byte [draw_bg_color], SYS_TITLE_ACTIVE
    call gfx_draw_string_stub       ; 3D: white text on blue
    mov byte [draw_bg_color], 0
    jmp .active_text_done
.active_flat_text:
    call gfx_draw_string_inverted   ; Flat: inverted (black on white)
.active_text_done:
    pop word [caller_ds]
    pop bx

    ; Draw close button [X]
    push bx
    push cx
    push dx
    push si
    mov si, [.win_ptr]
    mov bx, [si + WIN_OFF_X]
    add bx, [si + WIN_OFF_WIDTH]
    sub bx, 10
    mov cx, [si + WIN_OFF_Y]
    inc cx
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    mov si, close_btn_str
    cmp byte [widget_style], 0
    je .active_flat_close
    mov byte [draw_bg_color], SYS_TITLE_ACTIVE
    call gfx_draw_string_stub       ; 3D: white text
    mov byte [draw_bg_color], 0
    jmp .active_close_done
.active_flat_close:
    call gfx_draw_string_inverted   ; Flat: inverted
.active_close_done:
    pop word [caller_ds]
    pop si
    pop dx
    pop cx
    pop bx
    jmp .draw_border

.draw_inactive_titlebar:
    ; === Inactive window titlebar ===
    push bx
    push cx
    push dx
    push si
    push di
    mov bx, cx                      ; BX = X
    mov cx, dx                      ; CX = Y
    mov dx, si                      ; DX = Width
    mov si, [titlebar_height]     ; SI = Height (10)
    cmp byte [widget_style], 0
    je .inactive_flat_fill
    mov al, SYS_TITLE_INACT
    call gfx_draw_filled_rect_color ; 3D: gray titlebar
    jmp .inactive_fill_done
.inactive_flat_fill:
    call gfx_clear_area_stub        ; Flat: black titlebar
.inactive_fill_done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx

    ; Draw title text - normal white on black (flat) or white on gray (3D)
    push bx
    cmp byte [widget_style], 0
    je .inactive_title_text
    mov byte [draw_bg_color], SYS_TITLE_INACT
.inactive_title_text:
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    mov si, bx
    add si, WIN_OFF_TITLE
    mov bx, cx
    add bx, 4
    mov cx, dx
    add cx, 1
    call gfx_draw_string_stub
    pop word [caller_ds]
    mov byte [draw_bg_color], 0
    pop bx

    ; Draw close button [X] - normal white
    push bx
    push cx
    push dx
    push si
    cmp byte [widget_style], 0
    je .inactive_close_text
    mov byte [draw_bg_color], SYS_TITLE_INACT
.inactive_close_text:
    mov si, [.win_ptr]
    mov bx, [si + WIN_OFF_X]
    add bx, [si + WIN_OFF_WIDTH]
    sub bx, 10
    mov cx, [si + WIN_OFF_Y]
    inc cx
    push word [caller_ds]
    mov word [caller_ds], 0x1000
    mov si, close_btn_str
    call gfx_draw_string_stub
    pop word [caller_ds]
    mov byte [draw_bg_color], 0
    pop si
    pop dx
    pop cx
    pop bx

.draw_border:

    ; Restore original font after titlebar text drawing
    mov al, [.saved_font]
    cmp al, [current_font]
    je .font_restored
    call gfx_set_font
.font_restored:

    ; Draw border
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
    cmp byte [widget_style], 0
    je .flat_border
    call draw_raised_bevel           ; 3D: highlight/shadow edges
    jmp .border_done
.flat_border:
    call gfx_draw_rect_stub          ; Flat: white outline
.border_done:
    ; Draw resize grip (diagonal dots in bottom-right corner)
    ; plot_pixel_color: CX=X, BX=Y, DL=color
    push bx
    mov di, [.win_ptr]
    test byte [di + WIN_OFF_FLAGS], WIN_FLAG_BORDER
    jz .no_grip
    push si
    push bp
    mov si, [di + WIN_OFF_X]
    add si, [di + WIN_OFF_WIDTH]   ; SI = right edge X
    mov bp, [di + WIN_OFF_Y]
    add bp, [di + WIN_OFF_HEIGHT]  ; BP = bottom edge Y (as register, not [bp] memory)
    mov dl, 3                       ; White color
    ; Dot 1: (right-3, bottom-2)
    mov cx, si
    sub cx, 3
    mov bx, bp
    sub bx, 2
    call plot_pixel_color
    ; Dot 2: (right-5, bottom-4)
    mov cx, si
    sub cx, 5
    mov bx, bp
    sub bx, 4
    call plot_pixel_color
    ; Dot 3: (right-2, bottom-5)
    mov cx, si
    sub cx, 2
    mov bx, bp
    sub bx, 5
    call plot_pixel_color
    ; Dot 4: (right-4, bottom-3)
    mov cx, si
    sub cx, 4
    mov bx, bp
    sub bx, 3
    call plot_pixel_color
    pop bp
    pop si
.no_grip:
    pop bx
    pop bx

.nodraw:
    clc
    jmp .done

.invalid:
    mov ax, WIN_ERR_INVALID
    stc

.done:
    ; Restore text_color as foreground
    push ax
    mov al, [text_color]
    mov [draw_fg_color], al
    pop ax

    pop ds
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

.win_ptr: dw 0
.saved_font: db 0

; win_focus_stub - Bring window to front (set z-order to max)
; Input:  AX = Window handle
; Output: CF = 0 on success
; Note: No ownership check - kernel mouse handler must focus any window on click
win_focus_stub:
    push bx
    push cx
    push si
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

    ; Already on top? Skip demotion
    cmp byte [bx + WIN_OFF_ZORDER], 15
    je .already_top

    ; Demote all other visible windows' z-order
    mov si, window_table
    mov cx, WIN_MAX_COUNT
.demote_loop:
    cmp si, bx                      ; Skip our own entry
    je .demote_next
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .demote_next
    cmp byte [si + WIN_OFF_ZORDER], 0
    je .demote_next
    dec byte [si + WIN_OFF_ZORDER]
.demote_next:
    add si, WIN_ENTRY_SIZE
    loop .demote_loop

    ; Redraw old topmost window's title bar as inactive
    push ax
    cmp byte [topmost_handle], 0xFF
    je .skip_old_redraw
    xor ah, ah
    mov al, [topmost_handle]
    call win_draw_stub              ; Now draws with inactive style (z<15)
.skip_old_redraw:
    pop ax

.already_top:
    ; Set z-order to 15 (top)
    mov byte [bx + WIN_OFF_ZORDER], 15

    ; Track which task owns the focused window
    push ax
    mov al, [bx + WIN_OFF_OWNER]
    mov [focused_task], al
    pop ax

    ; Cache topmost window bounds for z-order clipping
    mov [topmost_handle], al        ; AL = window handle
    push si
    mov si, [bx + WIN_OFF_X]
    mov [topmost_win_x], si
    mov si, [bx + WIN_OFF_Y]
    mov [topmost_win_y], si
    mov si, [bx + WIN_OFF_WIDTH]
    mov [topmost_win_w], si
    mov si, [bx + WIN_OFF_HEIGHT]
    mov [topmost_win_h], si
    pop si

    clc
    jmp .done

.invalid:
    stc

.done:
    pop ds
    pop si
    pop cx
    pop bx
    ret

; win_move_stub - Move window to new position
; Input:  AX = Window handle, BX = New X, CX = New Y
; Output: CF = 0 on success
; Algorithm: Draw frame at new position FIRST, then clear only exposed edges.
; This avoids the full-window clear that causes "blue screen" on slow hardware.
win_move_stub:
    call mouse_cursor_hide
    inc byte [cursor_locked]

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
    ; NOTE: BP defaults to SS segment in x86 real mode!
    ; Must use ds: override for all [bp + ...] accesses to reach kernel data.
    mov bp, bx

    ; Read window dimensions (ds: override required - BP defaults to SS!)
    mov ax, [ds:bp + WIN_OFF_WIDTH]
    mov [.win_w], ax
    mov ax, [ds:bp + WIN_OFF_HEIGHT]
    mov [.win_h], ax

    ; Clamp new position to keep window on screen
    mov ax, [screen_width]
    sub ax, [.win_w]                ; AX = max X (screen_width - width)
    js .x_clamp_done                ; Skip if window wider than screen
    cmp [.new_x], ax
    jbe .x_clamp_done
    mov [.new_x], ax
.x_clamp_done:
    mov ax, [screen_height]
    sub ax, [.win_h]                ; AX = max Y (screen_height - height)
    js .y_clamp_done                ; Skip if window taller than screen
    cmp [.new_y], ax
    jbe .y_clamp_done
    mov [.new_y], ax
.y_clamp_done:

    ; Save old position before updating (ds: override for BP)
    mov ax, [ds:bp + WIN_OFF_X]
    mov [.old_x], ax
    mov ax, [ds:bp + WIN_OFF_Y]
    mov [.old_y], ax

    ; Update position in window table FIRST (ds: override for BP)
    mov bx, [.new_x]
    mov [ds:bp + WIN_OFF_X], bx
    mov cx, [.new_y]
    mov [ds:bp + WIN_OFF_Y], cx

    ; Update topmost bounds cache if this is the topmost window
    push ax
    mov ax, [.handle]
    cmp al, [topmost_handle]
    jne .not_topmost_move
    mov [topmost_win_x], bx
    mov [topmost_win_y], cx
.not_topmost_move:
    pop ax

    ; Draw window frame at new position immediately (makes window visible fast)
    mov ax, [.handle]
    call win_draw_stub

    ; Clear content area at new position (inside border, below title bar)
    mov bx, [.new_x]
    inc bx                          ; Inside left border
    mov cx, [.new_y]
    add cx, [titlebar_height]       ; Below title bar
    mov dx, [.win_w]
    sub dx, 2                       ; Inside both borders
    mov si, [.win_h]
    sub si, [titlebar_height]
    dec si                          ; Above bottom border
    test si, si
    jz .skip_drag_clear1
    call gfx_clear_area_stub
.skip_drag_clear1:

    ; Calculate deltas: dx = new_x - old_x, dy = new_y - old_y
    mov ax, [.new_x]
    sub ax, [.old_x]
    mov [.dx], ax
    mov ax, [.new_y]
    sub ax, [.old_y]
    mov [.dy], ax

    ; --- Clear exposed vertical strip (left or right edge) ---
    mov ax, [.dx]
    test ax, ax
    jz .no_v_strip                  ; No horizontal movement

    ; Check sign of dx
    test ax, 0x8000
    jnz .moved_left

    ; Moved RIGHT: clear left strip at old_x, width = dx
    cmp ax, [.win_w]
    jae .clear_full_old             ; No overlap, clear entire old rect
    mov bx, [.old_x]
    mov cx, [.old_y]
    mov dx, ax                      ; Strip width = dx
    mov si, [.win_h]
    call gfx_clear_area_stub
    jmp .no_v_strip

.moved_left:
    neg ax                          ; AX = |dx|
    cmp ax, [.win_w]
    jae .clear_full_old             ; No overlap, clear entire old rect
    ; Clear right strip: x = old_x + width - |dx|
    mov bx, [.old_x]
    add bx, [.win_w]
    sub bx, ax
    mov cx, [.old_y]
    mov dx, ax                      ; Strip width = |dx|
    mov si, [.win_h]
    call gfx_clear_area_stub

.no_v_strip:
    ; --- Clear exposed horizontal strip (top or bottom edge) ---
    mov ax, [.dy]
    test ax, ax
    jz .clear_done                  ; No vertical movement

    test ax, 0x8000
    jnz .moved_up

    ; Moved DOWN: clear top strip at old_y, height = dy
    cmp ax, [.win_h]
    jae .clear_full_old             ; No overlap, clear entire old rect
    mov bx, [.old_x]
    mov cx, [.old_y]
    mov dx, [.win_w]
    mov si, ax                      ; Strip height = dy
    call gfx_clear_area_stub
    jmp .clear_done

.moved_up:
    neg ax                          ; AX = |dy|
    cmp ax, [.win_h]
    jae .clear_full_old             ; No overlap, clear entire old rect
    ; Clear bottom strip: y = old_y + height - |dy|
    mov bx, [.old_x]
    mov cx, [.old_y]
    add cx, [.win_h]
    sub cx, ax
    mov dx, [.win_w]
    mov si, ax                      ; Strip height = |dy|
    call gfx_clear_area_stub
    jmp .clear_done

.clear_full_old:
    ; No overlap between old and new rects - clear entire old area
    mov bx, [.old_x]
    mov cx, [.old_y]
    mov dx, [.win_w]
    mov si, [.win_h]
    call gfx_clear_area_stub

.clear_done:
    ; Trigger redraw of windows that overlapped the old position
    mov ax, [.old_x]
    mov [redraw_old_x], ax
    mov ax, [.old_y]
    mov [redraw_old_y], ax
    mov ax, [.win_w]
    mov [redraw_old_w], ax
    mov ax, [.win_h]
    mov [redraw_old_h], ax
    call redraw_affected_windows

    ; Redraw moved window ON TOP - background repaints and desktop icon
    ; draws may have painted over the new position when areas overlap
    mov ax, [.handle]
    call win_draw_stub

    ; Re-clear content area (desktop icons / background frames may be in it)
    mov bx, [.new_x]
    inc bx                          ; Inside left border
    mov cx, [.new_y]
    add cx, [titlebar_height]     ; Below title bar
    mov dx, [.win_w]
    sub dx, 2                       ; Inside both borders
    mov si, [.win_h]
    sub si, [titlebar_height]
    dec si                          ; Above bottom border
    test si, si
    jz .skip_drag_clear2
    call gfx_clear_area_stub
.skip_drag_clear2:

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
    dec byte [cursor_locked]
    call mouse_cursor_show
    ret

.new_x:  dw 0
.new_y:  dw 0
.old_x:  dw 0
.old_y:  dw 0
.handle: dw 0
.win_w:  dw 0
.win_h:  dw 0
.dx:     dw 0
.dy:     dw 0

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
    add cx, [titlebar_height]
    inc cx

    ; Content Width = Window Width - 2 (borders)
    mov dx, [di + WIN_OFF_WIDTH]
    sub dx, 2

    ; Content Height = Window Height - titlebar - 2 (borders)
    mov si, [di + WIN_OFF_HEIGHT]
    sub si, [titlebar_height]
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
; New APIs (Build 247) — Colored Drawing, System, Filesystem, Window
; ============================================================================

; gfx_draw_filled_rect_color - Draw filled rectangle with color (API 67)
; Input: BX=X, CX=Y, DX=W, SI=H, AL=color(0-3)
; Output: CF=0
; Wraps existing internal gfx_fill_color which has cursor protection
gfx_draw_filled_rect_color:
    call gfx_fill_color
    clc
    ret

; gfx_draw_rect_color - Draw colored outline rectangle (API 68)
; Input: BX=X, CX=Y, DX=W, SI=H, AL=color(0-3)
; Output: CF=0
gfx_draw_rect_color:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bp
    push di
    mov [cs:.drc_color], al
    mov ax, [video_segment]
    mov es, ax
    ; API: BX=X, CX=Y. plot_pixel_color needs CX=X, BX=Y, DL=color
    mov bp, bx                      ; BP = X
    mov ax, cx                      ; AX = Y

    ; Top edge: Y=AX, X varies from BP to BP+DX-1
    mov di, 0
.drc_top:
    cmp di, dx
    jge .drc_top_done
    mov cx, bp
    add cx, di
    mov bx, ax
    mov dl, [cs:.drc_color]
    call plot_pixel_color
    inc di
    jmp .drc_top
.drc_top_done:

    ; Bottom edge: Y=AX+SI-1, X varies from BP to BP+DX-1
    mov di, 0
    push ax
    add ax, si
    dec ax
.drc_bottom:
    cmp di, dx
    jge .drc_bottom_done
    mov cx, bp
    add cx, di
    mov bx, ax
    mov dl, [cs:.drc_color]
    call plot_pixel_color
    inc di
    jmp .drc_bottom
.drc_bottom_done:
    pop ax

    ; Left edge: X=BP, Y varies from AX to AX+SI-1
    mov di, 0
.drc_left:
    cmp di, si
    jge .drc_left_done
    mov cx, bp
    mov bx, ax
    add bx, di
    mov dl, [cs:.drc_color]
    call plot_pixel_color
    inc di
    jmp .drc_left
.drc_left_done:

    ; Right edge: X=BP+DX-1, Y varies from AX to AX+SI-1
    mov di, 0
    push bp
    add bp, dx
    dec bp
.drc_right:
    cmp di, si
    jge .drc_right_done
    mov cx, bp
    mov bx, ax
    add bx, di
    mov dl, [cs:.drc_color]
    call plot_pixel_color
    inc di
    jmp .drc_right
.drc_right_done:
    pop bp

    pop di
    pop bp
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret
.drc_color: db 0

; gfx_draw_hline - Draw horizontal line (API 69)
; Input: BX=X, CX=Y, DX=length, AL=color(0-3)
; Output: CF=0
gfx_draw_hline:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    mov [cs:.hl_color], al
    mov ax, [video_segment]
    mov es, ax
    ; plot_pixel_color: CX=X, BX=Y, DL=color
    xchg bx, cx                    ; BX=Y, CX=X
    mov di, dx                     ; DI=length counter
.hl_loop:
    test di, di
    jz .hl_done
    mov dl, [cs:.hl_color]
    call plot_pixel_color
    inc cx                          ; Next X
    dec di
    jnz .hl_loop
.hl_done:
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret
.hl_color: db 0

; gfx_draw_vline - Draw vertical line (API 70)
; Input: BX=X, CX=Y, DX=height, AL=color(0-3)
; Output: CF=0
gfx_draw_vline:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push di
    mov [cs:.vl_color], al
    mov ax, [video_segment]
    mov es, ax
    ; plot_pixel_color: CX=X, BX=Y, DL=color
    xchg bx, cx                    ; BX=Y, CX=X
    mov di, dx                     ; DI=height counter
.vl_loop:
    test di, di
    jz .vl_done
    mov dl, [cs:.vl_color]
    call plot_pixel_color
    inc bx                          ; Next Y
    dec di
    jnz .vl_loop
.vl_done:
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret
.vl_color: db 0

; gfx_draw_line - Bresenham's line algorithm (API 71)
; Input: BX=X1, CX=Y1, DX=X2, SI=Y2, AL=color(0-3)
; Output: CF=0
gfx_draw_line:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push es
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov [cs:.bl_color], al
    mov ax, [cs:video_segment]
    mov es, ax

    ; Store endpoints: X1=BX, Y1=CX, X2=DX, Y2=SI
    mov [cs:.bl_x], bx
    mov [cs:.bl_y], cx
    mov [cs:.bl_x2], dx
    mov [cs:.bl_y2], si
    ; DX = abs(X2 - X1), step_x = sign
    sub dx, bx                     ; DX = X2 - X1
    mov word [cs:.bl_sx], 1
    test dx, dx
    jge .bl_dx_pos
    neg dx
    mov word [cs:.bl_sx], -1
.bl_dx_pos:
    mov [cs:.bl_dx], dx

    ; DY = -abs(Y2 - Y1), step_y = sign
    sub si, cx                     ; SI = Y2 - Y1
    mov word [cs:.bl_sy], 1
    test si, si
    jge .bl_dy_pos
    neg si
    mov word [cs:.bl_sy], -1
.bl_dy_pos:
    neg si                          ; SI = -abs(dy)
    mov [cs:.bl_dy], si

    ; err = dx + dy
    mov ax, dx
    add ax, si
    mov [cs:.bl_err], ax

.bl_pixel:
    ; Plot current point
    mov cx, [cs:.bl_x]
    mov bx, [cs:.bl_y]
    mov dl, [cs:.bl_color]
    call plot_pixel_color

    ; Check if we reached the endpoint
    mov ax, [cs:.bl_x]
    cmp ax, [cs:.bl_x2]
    jne .bl_continue
    mov ax, [cs:.bl_y]
    cmp ax, [cs:.bl_y2]
    je .bl_done

.bl_continue:
    mov ax, [cs:.bl_err]
    mov bp, ax
    shl bp, 1                      ; BP = 2*err

    ; if 2*err >= dy: err += dy, x += sx
    cmp bp, [cs:.bl_dy]
    jl .bl_skip_x
    mov ax, [cs:.bl_err]
    add ax, [cs:.bl_dy]
    mov [cs:.bl_err], ax
    mov ax, [cs:.bl_x]
    add ax, [cs:.bl_sx]
    mov [cs:.bl_x], ax
.bl_skip_x:

    ; if 2*err <= dx: err += dx, y += sy
    cmp bp, [cs:.bl_dx]
    jg .bl_skip_y
    mov ax, [cs:.bl_err]
    add ax, [cs:.bl_dx]
    mov [cs:.bl_err], ax
    mov ax, [cs:.bl_y]
    add ax, [cs:.bl_sy]
    mov [cs:.bl_y], ax
.bl_skip_y:
    jmp .bl_pixel

.bl_done:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

.bl_x:     dw 0
.bl_y:     dw 0
.bl_x2:    dw 0
.bl_y2:    dw 0
.bl_dx:    dw 0
.bl_dy:    dw 0
.bl_sx:    dw 0
.bl_sy:    dw 0
.bl_err:   dw 0
.bl_color: db 0

; get_rtc_time - Read real-time clock (API 72)
; Input: none
; Output: CH=hours(BCD), CL=minutes(BCD), DH=seconds(BCD), CF=0
get_rtc_time:
    push ax
    mov ah, 0x02
    int 0x1A
    pop ax
    clc
    ret

; set_rtc_time - Set real-time clock (API 81)
; Input: CH=hours(BCD), CL=minutes(BCD), DH=seconds(BCD)
; Output: CF=0
set_rtc_time:
    push ax
    mov ah, 0x03
    int 0x1A
    pop ax
    clc
    ret

; get_screen_info - Get screen dimensions and mode (API 82)
; Output: BX=width, CX=height, AL=mode, AH=colors, CF=0
get_screen_info:
    mov bx, [screen_width]
    mov cx, [screen_height]
    mov al, [video_mode]                ; AL = current mode (0x04 or 0x13)
    cmp al, 0x13
    je .gsi_vga
    mov ah, 4                           ; CGA: 4 colors
    clc
    ret
.gsi_vga:
    mov ah, 0                           ; VGA: 256 colors (0 wraps = 256)
    clc
    ret

; set_video_mode - Switch video mode at runtime (API 95)
; Input: AL = video mode (0x04=CGA, 0x13=VGA)
; Output: AL = actual mode set, CF=0 success
; Triggers full-screen redraw of desktop + all windows
set_video_mode:
    push bx
    push cx
    push dx
    push si
    push di

    ; Hide cursor during mode switch
    call mouse_cursor_hide
    inc byte [cursor_locked]

    cmp al, 0x01
    je .svm_try_vesa
    cmp al, 0x12
    je .svm_try_mode12h
    cmp al, 0x13
    je .svm_try_vga

    ; Set CGA mode 4
    mov byte [video_mode], 0x04
    mov word [video_segment], 0xB800
    mov word [screen_width], 320
    mov word [screen_height], 200
    mov byte [screen_bpp], 2
    mov word [screen_pitch], 80
    mov byte [widget_style], 0
    push ax
    xor ax, ax
    mov al, 0x04
    int 0x10
    pop ax
    jmp .svm_setup

.svm_try_vga:
    push ax
    xor ax, ax
    mov al, 0x13
    int 0x10
    ; Verify mode was set
    mov ah, 0x0F
    int 0x10
    and al, 0x7F
    cmp al, 0x13
    pop ax
    je .svm_vga_ok
    ; VGA failed, fall back to CGA
    xor ax, ax
    mov al, 0x04
    int 0x10
    mov byte [video_mode], 0x04
    mov word [video_segment], 0xB800
    mov word [screen_width], 320
    mov word [screen_height], 200
    mov byte [screen_bpp], 2
    mov word [screen_pitch], 80
    mov byte [widget_style], 0
    jmp .svm_setup

.svm_vga_ok:
    mov byte [video_mode], 0x13
    mov word [video_segment], 0xA000
    mov word [screen_width], 320
    mov word [screen_height], 200
    mov byte [screen_bpp], 8
    mov word [screen_pitch], 320
    mov byte [widget_style], 1
    jmp .svm_setup

.svm_try_mode12h:
    push ax
    xor ax, ax
    mov al, 0x12
    int 0x10
    ; Verify mode was set
    mov ah, 0x0F
    int 0x10
    and al, 0x7F
    cmp al, 0x12
    pop ax
    je .svm_mode12h_ok
    ; Mode 12h failed, fall back to VGA 13h
    jmp .svm_try_vga

.svm_mode12h_ok:
    mov byte [video_mode], 0x12
    mov word [video_segment], 0xA000
    mov word [screen_width], 640
    mov word [screen_height], 480
    mov byte [screen_bpp], 4
    mov word [screen_pitch], 80
    mov byte [widget_style], 1
    jmp .svm_setup

.svm_try_vesa:
    ; Query VESA mode 0x101 (640x480x256) info
    push es
    push di
    mov ax, 0x9000
    mov es, ax
    xor di, di                     ; ES:DI = 0x9000:0000 (scratch buffer)
    mov ax, 0x4F01                 ; VESA: Get Mode Info
    mov cx, 0x0101                 ; Mode 0x101 = 640x480x256
    int 0x10
    cmp ax, 0x004F                 ; Success?
    pop di
    pop es
    jne .svm_vesa_fail
    ; Check mode attributes bit 0 (mode supported)
    push ds
    mov ax, 0x9000
    mov ds, ax
    test byte [0x0000], 1          ; Mode attributes bit 0
    pop ds
    jz .svm_vesa_fail
    ; Save window granularity (offset 4 in mode info)
    push ds
    mov ax, 0x9000
    mov ds, ax
    mov ax, [0x0004]               ; Window granularity in KB
    pop ds
    mov [vesa_gran], ax
    ; Set VESA mode 0x101
    push ax
    mov ax, 0x4F02
    mov bx, 0x0101                 ; Mode 0x101
    int 0x10
    cmp ax, 0x004F
    pop ax
    jne .svm_vesa_fail
    ; VESA mode set successfully
    mov byte [video_mode], 0x01    ; Internal marker for VESA
    mov word [video_segment], 0xA000
    mov word [screen_width], 640
    mov word [screen_height], 480
    mov byte [screen_bpp], 8
    mov word [screen_pitch], 640
    mov byte [widget_style], 1
    mov word [vesa_cur_bank], 0xFFFF  ; Invalidate bank cache
    jmp .svm_setup

.svm_vesa_fail:
    ; VESA failed, try Mode 12h
    jmp .svm_try_mode12h

.svm_setup:
    call setup_graphics_post_mode

    ; Set dynamic titlebar height based on resolution
    mov word [titlebar_height], 10
    cmp word [screen_width], 640
    jb .svm_tb_done
    mov word [titlebar_height], 18
.svm_tb_done:

    ; Reinit draw colors from theme
    mov al, [text_color]
    mov [draw_fg_color], al
    mov al, [desktop_bg_color]
    mov [draw_bg_color], al

    ; Update clipping region for new resolution
    mov ax, [screen_width]
    dec ax
    mov [clip_x2], ax
    mov ax, [screen_height]
    dec ax
    mov [clip_y2], ax

    ; Recenter all existing windows for the new resolution (no scaling)
    push si
    push cx
    mov si, window_table
    mov cx, WIN_MAX_COUNT
.svm_win_loop:
    cmp byte [si + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .svm_win_next
    ; If window was previously scaled (from older build), reverse it
    cmp byte [si + WIN_OFF_CONTENT_SCALE], 2
    jne .svm_no_reverse
    push dx
    mov dx, [si + WIN_OFF_WIDTH]
    sub dx, 2
    shr dx, 1
    add dx, 2
    mov [si + WIN_OFF_WIDTH], dx
    mov dx, [si + WIN_OFF_HEIGHT]
    sub dx, [titlebar_height]
    dec dx
    shr dx, 1
    add dx, [titlebar_height]
    inc dx
    mov [si + WIN_OFF_HEIGHT], dx
    pop dx
    mov byte [si + WIN_OFF_CONTENT_SCALE], 1
.svm_no_reverse:
    ; Center window in new resolution
    push ax
    mov ax, [screen_width]
    sub ax, [si + WIN_OFF_WIDTH]
    shr ax, 1
    mov [si + WIN_OFF_X], ax
    mov ax, [screen_height]
    sub ax, [si + WIN_OFF_HEIGHT]
    shr ax, 1
    mov [si + WIN_OFF_Y], ax
    pop ax
.svm_win_next:
    add si, WIN_ENTRY_SIZE
    dec cx
    jnz .svm_win_loop
    pop cx
    pop si

    ; Center mouse cursor in new resolution
    mov ax, [screen_width]
    shr ax, 1
    mov [mouse_x], ax
    mov ax, [screen_height]
    shr ax, 1
    mov [mouse_y], ax

    ; Force cursor state reset (mode switch clears VRAM)
    mov byte [cursor_visible], 0

    ; Clear cursor save buffer to prevent stale restore after mode change
    push es
    push di
    push cx
    push ax
    push ds
    pop es                             ; ES = DS = kernel segment
    mov di, cursor_save_buf
    xor ax, ax
    mov cx, 224                        ; 448 bytes / 2
    rep stosw
    pop ax
    pop cx
    pop di
    pop es

    ; Trigger full-screen redraw
    mov word [redraw_old_x], 0
    mov word [redraw_old_y], 0
    push ax
    mov ax, [screen_width]
    mov [redraw_old_w], ax
    mov ax, [screen_height]
    mov [redraw_old_h], ax
    pop ax
    call redraw_affected_windows

    dec byte [cursor_locked]
    call mouse_cursor_show

    mov al, [video_mode]
    clc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; get_key_modifiers - Get keyboard modifier states (API 83)
; Input: none
; Output: AL=shift state, AH=ctrl state, DL=alt state, CF=0
get_key_modifiers:
    mov al, [kbd_shift_state]
    mov ah, [kbd_ctrl_state]
    mov dl, [kbd_alt_state]
    clc
    ret

; delay_ticks - Sleep with yield (API 73)
; Input: CX=ticks to wait (1 tick ~ 55ms at 18.2Hz)
; Output: none, CF=0
delay_ticks:
    push ax
    push bx
    push cx
    push dx

    ; Read initial tick count
    push es
    mov bx, 0x0040
    mov es, bx
    mov dx, [es:0x006C]             ; DX = start tick
    pop es

.dt_loop:
    ; Yield to other tasks
    call app_yield_stub

    ; Read current tick
    push es
    mov bx, 0x0040
    mov es, bx
    mov ax, [es:0x006C]             ; AX = current tick
    pop es

    ; Calculate elapsed = current - start (handles 16-bit wrap)
    sub ax, dx                      ; AX = elapsed ticks
    cmp ax, cx                      ; Elapsed >= requested?
    jb .dt_loop

    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

; get_task_info - Current task and focus info (API 74)
; Input: none
; Output: AL=current_task_id, BL=focused_task_id, CL=running_task_count, CF=0
get_task_info:
    mov al, [current_task]
    mov bl, [focused_task]

    ; Count running tasks
    push si
    push di
    xor cl, cl                      ; CL = count
    xor si, si                      ; SI = index
    mov di, app_table
.gti_loop:
    cmp si, APP_MAX_COUNT
    jae .gti_done
    cmp byte [di + APP_OFF_STATE], APP_STATE_FREE
    je .gti_next
    inc cl
.gti_next:
    add di, APP_ENTRY_SIZE
    inc si
    jmp .gti_loop
.gti_done:
    pop di
    pop si
    clc
    ret

; fs_seek_stub - Seek file position (API 75)
; Input: AL=file_handle, CX=position_hi, DX=position_lo
; Output: CF=0 success, CF=1 error
fs_seek_stub:
    push bx
    push si

    ; Validate handle
    xor ah, ah
    cmp ax, FILE_MAX_HANDLES
    jae .fss_invalid

    ; Calculate file table entry offset
    mov si, ax
    shl si, 5                       ; SI = handle * 32
    add si, file_table

    ; Check if handle is open
    cmp byte [si], 1                ; Status: 1=open
    jne .fss_invalid

    ; Check if position is past EOF
    ; Compare position (CX:DX) with file size (bytes 4-7)
    cmp cx, [si + 6]                ; Compare hi word
    ja .fss_past_eof
    jb .fss_ok
    cmp dx, [si + 4]                ; Compare lo word
    ja .fss_past_eof

.fss_ok:
    ; Set position (bytes 8-11)
    mov [si + 8], dx                ; Position lo
    mov [si + 10], cx               ; Position hi
    clc
    pop si
    pop bx
    ret

.fss_past_eof:
.fss_invalid:
    stc
    pop si
    pop bx
    ret

; fs_get_file_size_stub - Query file size (API 76)
; Input: AL=file_handle
; Output: DX=size_hi, AX=size_lo, CF=0 success; CF=1 error
fs_get_file_size_stub:
    push bx
    push si

    ; Validate handle
    xor ah, ah
    cmp ax, FILE_MAX_HANDLES
    jae .fgs_invalid

    ; Calculate file table entry offset
    mov si, ax
    shl si, 5
    add si, file_table

    ; Check if handle is open
    cmp byte [si], 1
    jne .fgs_invalid

    ; Read size (bytes 4-7)
    mov ax, [si + 4]                ; Size lo
    mov dx, [si + 6]                ; Size hi
    clc
    pop si
    pop bx
    ret

.fgs_invalid:
    xor ax, ax
    xor dx, dx
    stc
    pop si
    pop bx
    ret

; fs_rename_stub - Rename file (API 77)
; Input: DS:SI=old_name (caller_ds), ES:DI=new_name (caller_es), BL=mount_handle
; Output: CF=0 success, CF=1 error
fs_rename_stub:
    cmp bl, 0
    je .frn_fat12
    cmp bl, 1
    je .frn_fat16
    stc
    ret
.frn_fat12:
    push ds
    mov ds, [cs:caller_ds]
    call fat12_rename
    pop ds
    ret
.frn_fat16:
    push ds
    mov ds, [cs:caller_ds]
    call fat16_rename
    pop ds
    ret

; fat12_rename - Rename a file in FAT12 root directory
; Input: DS:SI = old filename (dot format), caller_es:DI has new name
; Output: CF=0 success, CF=1 error
fat12_rename:
    push es
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    ; Convert OLD filename to 8.3 FAT format on stack
    sub sp, 12                      ; 11 bytes + 1 alignment
    mov di, sp
    push ss
    pop es

    mov cx, 11
    mov al, ' '
    push di
    rep stosb
    pop di

    mov cx, 8
.frn_copy_old_name:
    lodsb
    test al, al
    jz .frn_old_done
    cmp al, '.'
    je .frn_copy_old_ext
    cmp al, 'a'
    jb .frn_store_old
    cmp al, 'z'
    ja .frn_store_old
    sub al, 32
.frn_store_old:
    mov [es:di], al
    inc di
    loop .frn_copy_old_name
.frn_skip_old_dot:
    lodsb
    test al, al
    jz .frn_old_done
    cmp al, '.'
    jne .frn_skip_old_dot
.frn_copy_old_ext:
    mov di, sp
    add di, 8
    mov cx, 3
.frn_copy_old_ext_loop:
    lodsb
    test al, al
    jz .frn_old_done
    cmp al, 'a'
    jb .frn_store_old_ext
    cmp al, 'z'
    ja .frn_store_old_ext
    sub al, 32
.frn_store_old_ext:
    mov [es:di], al
    inc di
    loop .frn_copy_old_ext_loop

.frn_old_done:
    ; Now convert NEW filename to 8.3 on stack (another 12 bytes)
    sub sp, 12
    mov di, sp
    push ss
    pop es

    mov cx, 11
    mov al, ' '
    push di
    rep stosb
    pop di

    ; Read new name from caller_es:DI_saved
    ; DI was pushed on main stack — we saved original DI
    ; Get it from stack: original DI is at known offset
    push ds
    mov ds, [cs:caller_es]
    ; Original DI is saved in the push frame. We need to recover it.
    ; Stack layout: [12 new name][12 old name][BP][DI][SI][DX][CX][BX][ES]
    ; Original DI is at SP + 12 + 12 + 2 + 0 = SP + 26... but that's the pushed DI
    mov si, sp
    add si, 12 + 12 + 2             ; Skip new_name(12) + old_name(12) + BP(2)
    mov si, [ss:si]                  ; SI = original DI (the new filename pointer)

    mov cx, 8
.frn_copy_new_name:
    lodsb
    test al, al
    jz .frn_new_done
    cmp al, '.'
    je .frn_copy_new_ext
    cmp al, 'a'
    jb .frn_store_new
    cmp al, 'z'
    ja .frn_store_new
    sub al, 32
.frn_store_new:
    mov [es:di], al
    inc di
    loop .frn_copy_new_name
.frn_skip_new_dot:
    lodsb
    test al, al
    jz .frn_new_done
    cmp al, '.'
    jne .frn_skip_new_dot
.frn_copy_new_ext:
    mov di, sp
    add di, 8
    mov cx, 3
.frn_copy_new_ext_loop:
    lodsb
    test al, al
    jz .frn_new_done
    cmp al, 'a'
    jb .frn_store_new_ext
    cmp al, 'z'
    ja .frn_store_new_ext
    sub al, 32
.frn_store_new_ext:
    mov [es:di], al
    inc di
    loop .frn_copy_new_ext_loop

.frn_new_done:
    pop ds

    ; Switch to kernel DS for directory search
    mov ax, 0x1000
    mov ds, ax

    ; Search root directory for old name
    mov ax, [root_dir_start]
    mov cx, 14

.frn_search_sector:
    push cx
    push ax
    mov [cs:.frn_dir_sector], ax

    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_read_sector
    jc .frn_read_error

    mov cx, 16
    mov si, bpb_buffer
    xor dx, dx

.frn_check_entry:
    mov al, [si]
    test al, al
    jz .frn_not_found_pop
    cmp al, 0xE5
    je .frn_next_entry

    ; Compare with old name on stack
    push si
    push di
    push ds
    push cx
    ; Old name is at SP + 8 (our pushes) + 4 (loop pushes) + 12 (new name) = SP + 24
    mov di, sp
    add di, 24
    push ss
    pop es
    mov ax, 0x1000
    mov ds, ax
    mov cx, 11
    repe cmpsb
    pop cx
    pop ds
    pop di
    pop si
    je .frn_found_file

.frn_next_entry:
    add si, 32
    add dx, 32
    dec cx
    jnz .frn_check_entry

    pop ax
    pop cx
    inc ax
    dec cx
    jnz .frn_search_sector

    ; Not found
    add sp, 24                      ; Remove both names
    mov ax, FS_ERR_NOT_FOUND
    stc
    jmp .frn_cleanup

.frn_not_found_pop:
    pop ax
    pop cx
    add sp, 24
    mov ax, FS_ERR_NOT_FOUND
    stc
    jmp .frn_cleanup

.frn_read_error:
    pop ax
    pop cx
    add sp, 24
    mov ax, FS_ERR_READ_ERROR
    stc
    jmp .frn_cleanup

.frn_found_file:
    ; SI points to directory entry in bpb_buffer
    ; Copy new name (11 bytes) over the old name in the entry
    push si
    push di
    push cx
    mov di, si                      ; DI = dir entry in bpb_buffer
    ; New name is at SP + 6 (our pushes) + 4 (loop pushes) = SP + 10
    mov si, sp
    add si, 10
    push ss
    pop ds
    mov ax, 0x1000
    mov es, ax                      ; ES = kernel (bpb_buffer segment)
    mov cx, 11
    rep movsb
    ; Restore DS to kernel
    mov ax, 0x1000
    mov ds, ax
    pop cx
    pop di
    pop si

    ; Write the modified sector back
    pop ax                          ; AX = sector LBA
    pop cx                          ; CX = remaining sectors counter
    mov bx, 0x1000
    mov es, bx
    mov bx, bpb_buffer
    call floppy_write_sector
    jc .frn_write_error

    add sp, 24                      ; Remove both names from stack
    clc
    jmp .frn_cleanup

.frn_write_error:
    add sp, 24
    mov ax, FS_ERR_WRITE_ERROR
    stc

.frn_cleanup:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    ret

.frn_dir_sector: dw 0

; win_resize_stub - Resize existing window (API 78)
; Input: AL=window_handle, DX=new_width, SI=new_height
; Output: CF=0 success, CF=1 error
win_resize_stub:
    push bx
    push cx
    push di

    ; Validate handle
    xor ah, ah
    cmp ax, WIN_MAX_COUNT
    jae .wrs_invalid

    ; Get window entry
    mov di, ax
    shl di, 5
    add di, window_table

    ; Check if window is visible
    cmp byte [di + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .wrs_invalid

    ; Save old dimensions for desktop redraw
    push ax
    mov ax, [di + WIN_OFF_X]
    mov [redraw_old_x], ax
    mov ax, [di + WIN_OFF_Y]
    mov [redraw_old_y], ax
    mov ax, [di + WIN_OFF_WIDTH]
    mov [redraw_old_w], ax
    mov ax, [di + WIN_OFF_HEIGHT]
    mov [redraw_old_h], ax
    pop ax

    ; Update dimensions to new size
    mov [di + WIN_OFF_WIDTH], dx
    mov [di + WIN_OFF_HEIGHT], si

    ; Update clip rect if this window is the active draw context
    cmp al, [draw_context]
    jne .wrs_no_clip_update
    push bx
    ; clip_x1 = win_x + 1
    mov bx, [di + WIN_OFF_X]
    inc bx
    mov [clip_x1], bx
    ; clip_y1 = win_y + titlebar_height
    mov bx, [di + WIN_OFF_Y]
    add bx, [titlebar_height]
    mov [clip_y1], bx
    ; clip_x2 = win_x + new_width - 2
    mov bx, [di + WIN_OFF_X]
    add bx, dx
    sub bx, 2
    mov [clip_x2], bx
    ; clip_y2 = win_y + new_height - 2
    mov bx, [di + WIN_OFF_Y]
    add bx, si
    sub bx, 2
    mov [clip_y2], bx
    pop bx
.wrs_no_clip_update:

    ; Redraw the background where old window was, then redraw frame
    push ax
    call draw_desktop_region
    call win_draw_stub              ; Redraw frame with new size

    ; Clear content area so desktop icons don't show through
    push dx
    push si
    mov bx, [di + WIN_OFF_X]
    mov cx, [di + WIN_OFF_Y]
    mov dx, [di + WIN_OFF_WIDTH]
    mov si, [di + WIN_OFF_HEIGHT]
    test byte [di + WIN_OFF_FLAGS], WIN_FLAG_TITLE | WIN_FLAG_BORDER
    jz .wrs_clear_frameless
    inc bx                          ; Inside left border
    add cx, [titlebar_height]       ; Below title bar
    sub dx, 2                       ; Inside both borders
    sub si, [titlebar_height]
    dec si                          ; Above bottom border
.wrs_clear_frameless:
    test si, si
    jz .wrs_skip_clear
    call gfx_clear_area_stub
.wrs_skip_clear:
    pop si
    pop dx
    pop ax

    ; Post WIN_REDRAW event so app repaints content
    push ax
    push dx
    xor dh, dh
    mov dl, al                      ; DL = window handle
    mov al, EVENT_WIN_REDRAW
    call post_event
    pop dx
    pop ax

    clc
    pop di
    pop cx
    pop bx
    ret

.wrs_invalid:
    stc
    pop di
    pop cx
    pop bx
    ret

; win_get_info_stub - Query window properties (API 79)
; Input: AL=window_handle
; Output: BX=X, CX=Y, DX=width, SI=height, DI=flags<<8|state, CF=0
win_get_info_stub:
    push ax

    ; Validate handle
    xor ah, ah
    cmp ax, WIN_MAX_COUNT
    jae .wgi_invalid

    ; Get window entry
    mov di, ax
    shl di, 5
    add di, window_table

    ; Check if entry is used
    cmp byte [di + WIN_OFF_STATE], WIN_STATE_FREE
    je .wgi_invalid

    ; Read fields
    mov bx, [di + WIN_OFF_X]
    mov cx, [di + WIN_OFF_Y]
    mov dx, [di + WIN_OFF_WIDTH]
    mov si, [di + WIN_OFF_HEIGHT]

    ; Return app-space dimensions for auto-scaled windows.
    ; Apps think they're in a small window; the kernel scales transparently.
    ; Reverse the auto-scaling formula to recover original dimensions:
    ;   phys_w = (orig_w - 2) * 2 + 2  →  orig_w = (phys_w + 2) / 2
    ;   phys_h = (orig_h - TB - 1) * 2 + TB + 1  →  orig_h = (phys_h + TB + 1) / 2
    cmp byte [di + WIN_OFF_CONTENT_SCALE], 2
    jne .wgi_no_scale
    add dx, 2
    shr dx, 1
    add si, [titlebar_height]
    inc si
    shr si, 1
.wgi_no_scale:

    ; Pack flags and state into DI
    mov ah, [di + WIN_OFF_FLAGS]
    mov al, [di + WIN_OFF_STATE]
    mov di, ax

    pop ax
    clc
    ret

.wgi_invalid:
    pop ax
    stc
    ret

; win_get_content_scale - Get content scale for current draw_context (API 96)
; Input: None (uses draw_context)
; Output: AL = content_scale (1=normal, 2=double), CF=0
win_get_content_scale:
    cmp byte [draw_context], 0xFF
    je .gcs_none
    cmp byte [draw_context], WIN_MAX_COUNT
    jae .gcs_none
    push si
    xor ah, ah
    mov al, [draw_context]
    mov si, ax
    shl si, 5
    add si, window_table
    mov al, [si + WIN_OFF_CONTENT_SCALE]
    pop si
    clc
    ret
.gcs_none:
    mov al, 1
    clc
    ret

; win_get_content_size - Get window content area dimensions (API 97)
; Input: AL = window handle (0xFF = use current draw_context)
; Output: DX = content width, SI = content height, CF=0
;         CF=1 if invalid handle or no draw context
; Note: Returns app-facing dimensions (accounts for content_scale)
win_get_content_size:
    cmp al, 0xFF
    jne .wgcs_use_handle
    mov al, [draw_context]
    cmp al, 0xFF
    je .wgcs_invalid
.wgcs_use_handle:
    cmp al, WIN_MAX_COUNT
    jae .wgcs_invalid
    push bx
    xor ah, ah
    mov bx, ax
    shl bx, 5
    add bx, window_table
    cmp byte [bx + WIN_OFF_STATE], WIN_STATE_VISIBLE
    jne .wgcs_invalid_pop
    ; content_w = win_w - 4 (1px border each side + 1px padding each side)
    mov dx, [bx + WIN_OFF_WIDTH]
    sub dx, 4
    ; content_h = win_h - titlebar_height - 2 (titlebar + 1px bottom border + 1px padding)
    mov si, [bx + WIN_OFF_HEIGHT]
    sub si, [titlebar_height]
    sub si, 2
    ; Handle content scaling: return app-facing (logical) dimensions
    cmp byte [bx + WIN_OFF_CONTENT_SCALE], 2
    jne .wgcs_no_scale
    shr dx, 1
    shr si, 1
.wgcs_no_scale:
    pop bx
    clc
    ret
.wgcs_invalid_pop:
    pop bx
.wgcs_invalid:
    xor dx, dx
    xor si, si
    stc
    ret

; gfx_scroll_area - Scroll rectangular region vertically (API 80)
; Input: BX=X, CX=Y, DX=W, SI=H, DI=scroll_pixels (positive=up)
; Output: none, CF=0
; Note: Operates on CGA byte boundaries for X/W (rounds to 4-pixel groups)
gfx_scroll_area:
    call mouse_cursor_hide
    inc byte [cursor_locked]
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push bp

    mov ax, [video_segment]
    mov es, ax

    ; Save parameters
    mov [cs:.sa_x], bx
    mov [cs:.sa_y], cx
    mov [cs:.sa_w], dx
    mov [cs:.sa_h], si
    mov [cs:.sa_scroll], di

    cmp byte [cs:video_mode], 0x01
    je .sa_vesa
    cmp byte [cs:video_mode], 0x12
    je .sa_mode12h
    cmp byte [cs:video_mode], 0x13
    je .sa_vga

    ; Calculate bytes per row for the region
    mov ax, bx
    add ax, dx                      ; AX = X + W
    add ax, 3
    shr ax, 1
    shr ax, 1                       ; AX = ceil((X+W)/4)
    mov bp, bx
    shr bp, 1
    shr bp, 1                       ; BP = X/4 (start byte)
    sub ax, bp                      ; AX = bytes per row
    mov [cs:.sa_bpr], ax

    ; Number of rows to copy = H - scroll_pixels
    mov ax, si
    sub ax, di                      ; AX = rows to copy
    test ax, ax
    jle .sa_clear_all               ; Scroll >= height, just clear everything

    mov cx, ax                      ; CX = rows to copy

    ; Copy rows: for scroll up, copy from (Y+scroll) to Y
    mov ax, [cs:.sa_y]
    mov [cs:.sa_dst_y], ax          ; dst starts at Y
    add ax, di
    mov [cs:.sa_src_y], ax          ; src starts at Y + scroll

.sa_copy_loop:
    push cx

    ; Calculate source row CGA address
    mov ax, [cs:.sa_src_y]
    push ax
    shr ax, 1
    push dx
    mov dx, 80
    mul dx
    pop dx
    mov si, ax
    pop ax
    test al, 1
    jz .sa_src_even
    add si, 0x2000
.sa_src_even:
    ; Add X byte offset
    mov ax, [cs:.sa_x]
    shr ax, 1
    shr ax, 1
    add si, ax

    ; Calculate dest row CGA address
    mov ax, [cs:.sa_dst_y]
    push ax
    shr ax, 1
    push dx
    mov dx, 80
    mul dx
    pop dx
    mov di, ax
    pop ax
    test al, 1
    jz .sa_dst_even
    add di, 0x2000
.sa_dst_even:
    mov ax, [cs:.sa_x]
    shr ax, 1
    shr ax, 1
    add di, ax

    ; Copy bytes for this row
    mov cx, [cs:.sa_bpr]
    push ds
    push es
    pop ds                          ; DS = ES = video segment
    rep movsb
    pop ds

    ; Advance Y positions
    inc word [cs:.sa_src_y]
    inc word [cs:.sa_dst_y]

    pop cx
    dec cx
    jnz .sa_copy_loop

    ; Clear the exposed strip at bottom (scroll_pixels rows)
    mov cx, [cs:.sa_scroll]
    ; dst_y is already positioned at first exposed row
.sa_clear_loop:
    push cx

    mov ax, [cs:.sa_dst_y]
    push ax
    shr ax, 1
    push dx
    mov dx, 80
    mul dx
    pop dx
    mov di, ax
    pop ax
    test al, 1
    jz .sa_clr_even
    add di, 0x2000
.sa_clr_even:
    mov ax, [cs:.sa_x]
    shr ax, 1
    shr ax, 1
    add di, ax

    mov cx, [cs:.sa_bpr]
    xor al, al
    rep stosb

    inc word [cs:.sa_dst_y]
    pop cx
    dec cx
    jnz .sa_clear_loop
    jmp .sa_done

.sa_clear_all:
    ; Clear entire region
    mov cx, [cs:.sa_h]
    mov ax, [cs:.sa_y]
    mov [cs:.sa_dst_y], ax
.sa_clear_all_loop:
    push cx

    mov ax, [cs:.sa_dst_y]
    push ax
    shr ax, 1
    push dx
    mov dx, 80
    mul dx
    pop dx
    mov di, ax
    pop ax
    test al, 1
    jz .sa_clra_even
    add di, 0x2000
.sa_clra_even:
    mov ax, [cs:.sa_x]
    shr ax, 1
    shr ax, 1
    add di, ax

    mov cx, [cs:.sa_bpr]
    xor al, al
    rep stosb

    inc word [cs:.sa_dst_y]
    pop cx
    dec cx
    jnz .sa_clear_all_loop

.sa_vesa:
    ; VESA scroll: row-by-row copy with bank switching, then clear strip
    ; For each row: compute 32-bit src/dst offsets, set bank, movsb
    ; If src and dst are in same bank, direct movsb; otherwise per-byte
    mov ax, [cs:.sa_h]
    sub ax, [cs:.sa_scroll]
    test ax, ax
    jle .sa_vesa_clear_all

    mov cx, ax                      ; CX = rows to copy
    mov ax, [cs:.sa_y]
    mov [cs:.sa_dst_y], ax
    add ax, [cs:.sa_scroll]
    mov [cs:.sa_src_y], ax

.sa_vesa_copy:
    push cx
    ; Compute source 32-bit offset: src_y * 640 + x
    mov ax, [cs:.sa_src_y]
    mov cx, 640
    mul cx                          ; DX:AX = src_y * 640
    add ax, [cs:.sa_x]
    adc dx, 0
    mov [cs:.sa_vesa_sbank], dx
    mov [cs:.sa_vesa_soff], ax

    ; Compute dest 32-bit offset: dst_y * 640 + x
    mov ax, [cs:.sa_dst_y]
    mov cx, 640
    mul cx
    add ax, [cs:.sa_x]
    adc dx, 0
    mov [cs:.sa_vesa_dbank], dx
    mov [cs:.sa_vesa_doff], ax

    ; Check if src and dst in same bank
    mov ax, [cs:.sa_vesa_sbank]
    cmp ax, [cs:.sa_vesa_dbank]
    jne .sa_vesa_cross

    ; Same bank: set bank and direct movsb
    call vesa_set_bank
    mov si, [cs:.sa_vesa_soff]
    mov di, [cs:.sa_vesa_doff]
    mov cx, [cs:.sa_w]
    push ds
    push es
    pop ds                          ; DS = ES = 0xA000
    rep movsb
    pop ds
    jmp .sa_vesa_next

.sa_vesa_cross:
    ; Different banks: per-byte read src, write dst
    mov cx, [cs:.sa_w]
    mov si, [cs:.sa_vesa_soff]
    mov di, [cs:.sa_vesa_doff]
.sa_vc_byte:
    ; Set source bank, read byte
    push ax
    mov ax, [cs:.sa_vesa_sbank]
    call vesa_set_bank
    pop ax
    mov al, [es:si]
    ; Set dest bank, write byte
    push ax
    mov ax, [cs:.sa_vesa_dbank]
    call vesa_set_bank
    pop ax
    mov [es:di], al
    inc si
    inc di
    dec cx
    jnz .sa_vc_byte

.sa_vesa_next:
    inc word [cs:.sa_src_y]
    inc word [cs:.sa_dst_y]
    pop cx
    dec cx
    jnz .sa_vesa_copy

    ; Clear exposed strip
    mov bx, [cs:.sa_x]
    mov cx, [cs:.sa_dst_y]
    mov dx, [cs:.sa_w]
    mov si, [cs:.sa_scroll]
    xor al, al
    call vesa_fill_rect
    jmp .sa_done

.sa_vesa_clear_all:
    mov bx, [cs:.sa_x]
    mov cx, [cs:.sa_y]
    mov dx, [cs:.sa_w]
    mov si, [cs:.sa_h]
    xor al, al
    call vesa_fill_rect
    jmp .sa_done

.sa_vesa_sbank: dw 0
.sa_vesa_soff:  dw 0
.sa_vesa_dbank: dw 0
.sa_vesa_doff:  dw 0

.sa_mode12h:
    ; Mode 12h scroll: use write mode 1 for copy (latch pass-through)
    ; Pitch = 80 bytes/row, each byte = 8 pixels
    mov ax, [cs:.sa_h]
    sub ax, [cs:.sa_scroll]
    test ax, ax
    jle .sa_m12_clear_all

    mov cx, ax                      ; CX = rows to copy
    mov ax, [cs:.sa_y]
    mov [cs:.sa_dst_y], ax
    add ax, [cs:.sa_scroll]
    mov [cs:.sa_src_y], ax

    ; Calculate byte positions for the region
    mov ax, [cs:.sa_x]
    shr ax, 3                      ; Left byte
    mov [cs:.sa_bpr], ax           ; Temp: left byte offset
    mov ax, [cs:.sa_x]
    add ax, [cs:.sa_w]
    add ax, 7
    shr ax, 3                      ; Right byte (exclusive)
    sub ax, [cs:.sa_bpr]           ; Bytes per row
    mov [cs:.sa_bpr], ax

    ; Set GC to write mode 1 (latches pass through on write)
    mov dx, 0x3CE
    mov al, 5                      ; Index 5: Mode register
    out dx, al
    inc dx
    mov al, 1                      ; Write mode 1
    out dx, al

.sa_m12_copy:
    push cx
    ; Source: src_y * 80 + left_byte
    mov ax, [cs:.sa_src_y]
    push dx
    mov di, 80
    mul di
    pop dx
    mov si, ax
    mov ax, [cs:.sa_x]
    shr ax, 3
    add si, ax
    ; Dest: dst_y * 80 + left_byte
    mov ax, [cs:.sa_dst_y]
    push dx
    mov di, 80
    mul di
    pop dx
    mov di, ax
    mov ax, [cs:.sa_x]
    shr ax, 3
    add di, ax
    ; Copy bytes (write mode 1: read loads latches, write outputs latches)
    mov cx, [cs:.sa_bpr]
    push ds
    push es
    pop ds                          ; DS = ES = 0xA000
    rep movsb
    pop ds
    inc word [cs:.sa_src_y]
    inc word [cs:.sa_dst_y]
    pop cx
    dec cx
    jnz .sa_m12_copy

    ; Reset write mode to 0
    mov dx, 0x3CE
    mov al, 5
    out dx, al
    inc dx
    xor al, al                     ; Write mode 0
    out dx, al

    ; Clear exposed strip using mode12h_fill_rect
    mov bx, [cs:.sa_x]
    mov cx, [cs:.sa_dst_y]         ; First exposed row
    mov dx, [cs:.sa_w]
    mov si, [cs:.sa_scroll]        ; Number of rows to clear
    xor al, al                     ; Color 0 = black
    call mode12h_fill_rect
    jmp .sa_done

.sa_m12_clear_all:
    ; Reset write mode (safety)
    mov dx, 0x3CE
    mov al, 5
    out dx, al
    inc dx
    xor al, al
    out dx, al
    ; Clear entire region
    mov bx, [cs:.sa_x]
    mov cx, [cs:.sa_y]
    mov dx, [cs:.sa_w]
    mov si, [cs:.sa_h]
    xor al, al
    call mode12h_fill_rect
    jmp .sa_done

.sa_vga:
    ; VGA: linear memory, 1 pixel = 1 byte, width in pixels = bytes
    mov ax, [cs:.sa_h]
    sub ax, [cs:.sa_scroll]
    test ax, ax
    jle .sa_vga_clear_all

    mov cx, ax                      ; CX = rows to copy
    mov ax, [cs:.sa_y]
    mov [cs:.sa_dst_y], ax
    add ax, [cs:.sa_scroll]
    mov [cs:.sa_src_y], ax

.sa_vga_copy:
    push cx
    ; Source offset: src_y * pitch + x
    mov ax, [cs:.sa_src_y]
    push dx
    mul word [screen_pitch]
    pop dx
    add ax, [cs:.sa_x]
    mov si, ax
    ; Dest offset: dst_y * pitch + x
    mov ax, [cs:.sa_dst_y]
    push dx
    mul word [screen_pitch]
    pop dx
    add ax, [cs:.sa_x]
    mov di, ax
    ; Copy width bytes
    mov cx, [cs:.sa_w]
    push ds
    push es
    pop ds                          ; DS = ES = video segment
    rep movsb
    pop ds
    inc word [cs:.sa_src_y]
    inc word [cs:.sa_dst_y]
    pop cx
    dec cx
    jnz .sa_vga_copy

    ; Clear exposed strip at bottom
    mov cx, [cs:.sa_scroll]
.sa_vga_clear:
    push cx
    mov ax, [cs:.sa_dst_y]
    push dx
    mul word [screen_pitch]
    pop dx
    add ax, [cs:.sa_x]
    mov di, ax
    mov cx, [cs:.sa_w]
    xor al, al
    rep stosb
    inc word [cs:.sa_dst_y]
    pop cx
    dec cx
    jnz .sa_vga_clear
    jmp .sa_done

.sa_vga_clear_all:
    mov cx, [cs:.sa_h]
    mov ax, [cs:.sa_y]
    mov [cs:.sa_dst_y], ax
    jmp .sa_vga_clear

.sa_done:
    pop bp
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    dec byte [cursor_locked]
    call mouse_cursor_show
    clc
    ret

.sa_x:       dw 0
.sa_y:       dw 0
.sa_w:       dw 0
.sa_h:       dw 0
.sa_scroll:  dw 0
.sa_bpr:     dw 0
.sa_src_y:   dw 0
.sa_dst_y:   dw 0

; ============================================================================
; Font Data - 8x8 characters
; ============================================================================
; IMPORTANT: Font must come BEFORE variables to avoid addressing issues

%include "font8x8.asm"
%include "font4x6.asm"
%include "font8x12.asm"

; ============================================================================
; Variables
; ============================================================================

; Character drawing state
draw_x: dw 0
draw_y: dw 0

; Font system variables (Build 205)
current_font:      db 1              ; 0=4x6, 1=8x8, 2=8x12
draw_font_height:  db 8              ; Current font height in pixels
draw_font_width:   db 8              ; Current font glyph width in pixels
draw_font_advance: db 12             ; Pixels to advance per character
draw_font_bpc:     db 8              ; Bytes per character in font data
draw_font_base:    dw font_8x8       ; Pointer to current font data

; Font descriptor table: 6 bytes per entry (pointer, height, width, advance, bpc)
FONT_DESC_SIZE     equ 6
FONT_COUNT         equ 3
font_table:
    dw font_4x6                       ; Font 0: small
    db 6, 4, 6, 6                     ; height=6, width=4, advance=6, bpc=6
    dw font_8x8                       ; Font 1: medium (default)
    db 8, 8, 12, 8                    ; height=8, width=8, advance=12, bpc=8
    dw font_8x12                      ; Font 2: large
    db 12, 8, 12, 12                  ; height=12, width=8, advance=12, bpc=12

; Text clipping variables (Build 205)
clip_enabled: db 0                    ; 0=no clipping, 1=clip to rect
clip_x1:      dw 0                    ; Left (inclusive, absolute)
clip_y1:      dw 0                    ; Top (inclusive, absolute)
clip_x2:      dw 319                  ; Right (inclusive, absolute)
clip_y2:      dw 199                  ; Bottom (inclusive, absolute)

; Video mode variables (Build 281, extended Build 291)
video_mode:     db 0x04                 ; Current video mode (0x04=CGA, 0x12=VGA16, 0x13=VGA256)
video_segment:  dw 0xB800              ; Video memory segment (0xB800=CGA, 0xA000=VGA)
screen_width:   dw 320                 ; Current screen width in pixels
screen_height:  dw 200                 ; Current screen height in pixels
screen_bpp:     db 2                   ; Bits per pixel (2=CGA, 4=Mode12h, 8=VGA13h/VESA)
screen_pitch:   dw 80                  ; Bytes per scanline (80=CGA, 320=VGA13h, 640=VESA)
widget_style:   db 0                   ; 0=flat (CGA), 1=3D beveled (VGA modes)
titlebar_height: dw 10                 ; Dynamic titlebar height (10=lo-res, 18=hi-res)
vesa_gran:      dw 64                  ; VESA window granularity in KB
vesa_cur_bank:  dw 0xFFFF              ; Current VESA bank (0xFFFF = invalid/unset)

; Color theme variables (Build 208)
draw_fg_color:  db 3                    ; Current foreground drawing color (0-3 CGA, 0-255 VGA)
draw_bg_color:  db 0                    ; Current background drawing color
text_color:     db 3                    ; Text/foreground color (default: white)
win_color:      db 3                    ; Window chrome color (default: white)
; desktop_bg_color is at line ~10033

; Widget scratch variables (Build 205)
btn_flags:      db 0                    ; Button/radio flags
btn_x:          dw 0                    ; Button X
btn_y:          dw 0                    ; Button Y
btn_w:          dw 0                    ; Button width
btn_h:          dw 0                    ; Button height
btn_saved_cds:  dw 0                    ; Saved caller_ds during button draw
btn_saved_fh:   db 0                    ; Saved font height
btn_saved_fw:   db 0                    ; Saved font width
btn_saved_fa:   db 0                    ; Saved font advance
wgt_text_ptr:   dw 0                    ; Widget text/label pointer (Build 235)
wgt_scratch:    dw 0                    ; Widget scratch variable (Build 235)
wgt_cursor_pos: dw 0                    ; Widget cursor position (Build 235)

; Word-wrap scratch variables (Build 205)
wrap_start_x:   dw 0                    ; Starting X for line breaks
wrap_width:     dw 0                    ; Wrap width in pixels

; Sprite drawing temp vars (Build 279)
_spr_color:  db 0
_spr_height: db 0
_spr_width:  db 0

heap_initialized: dw 0

; System call dispatcher temp (v3.12.0)
syscall_func: dw 0
caller_ds: dw 0x1000                ; Caller's DS segment (init to kernel for direct calls)
caller_es: dw 0x1000                ; Caller's ES segment (init to kernel for direct calls)
_did_translate: db 0                 ; 1 if BX/CX were translated by INT 0x80
_save_bx: dw 0                      ; Pre-translation BX (caller's original value)
_save_cx: dw 0                      ; Pre-translation CX (caller's original value)
_did_scale: db 0                     ; 1 if DX/SI were scaled by content_scale
_save_dx: dw 0                      ; Pre-scale DX (caller's original value)
_save_si: dw 0                      ; Pre-scale SI (caller's original value)

; Window drawing context (0xFF = no context / fullscreen, 0-15 = window handle)
; When active, drawing APIs (0-6) auto-translate coordinates to window-relative
draw_context: db 0xFF

; Keyboard driver state (Foundation 1.4)
old_int9_offset: dw 0
old_int9_segment: dw 0
kbd_buffer: times 16 db 0
kbd_buffer_head: dw 0
kbd_buffer_tail: dw 0
kbd_shift_state: db 0
kbd_ctrl_state: db 0
kbd_alt_state: db 0
kbd_e0_flag: db 0

; PS/2 Mouse driver state
old_int74_offset:   dw 0            ; Original IRQ12 vector
old_int74_segment:  dw 0
mouse_packet:       times 3 db 0    ; 3-byte packet buffer
mouse_packet_idx:   db 0            ; Current byte in packet (0-2)
mouse_x:            dw 160          ; Current X position (0-319)
mouse_y:            dw 100          ; Current Y position (0-199)
mouse_buttons:      db 0            ; Bit 0=left, bit 1=right, bit 2=middle
mouse_enabled:      db 0            ; 1 if mouse detected/enabled
mouse_diag:         db '?'          ; Diagnostic: B=BIOS, K=KBC, R/S/E=failure
saved_kbc_config:   db 0            ; Original 8042 config (restored on mouse init failure)

; Mouse cursor state
cursor_visible:     db 0            ; 1 = cursor currently drawn on screen
cursor_drawn_x:     dw 0            ; X where cursor was last drawn
cursor_drawn_y:     dw 0            ; Y where cursor was last drawn
cursor_locked:      db 0            ; Lock counter (>0 = cursor rendering suppressed)

; Cursor bitmap: 8 pixels wide, 10 rows tall
; Each byte = 1 row, MSB = leftmost pixel, 1 = draw (XOR white)
cursor_bitmap_color:        ; 2bpp: 2 bytes/row, 14 rows, W=white c=cyan
    db 0xC0, 0x00               ; W.......
    db 0xF0, 0x00               ; WW......
    db 0xD4, 0x00               ; Wcc.....
    db 0xD5, 0x00               ; Wccc....
    db 0xD5, 0x40               ; Wcccc...
    db 0xD5, 0x50               ; Wccccc..
    db 0xD5, 0x54               ; Wcccccc.
    db 0xD5, 0x55               ; Wccccccc
    db 0xD5, 0x40               ; Wcccc...
    db 0xF1, 0x40               ; WW.cc...
    db 0xC0, 0x50               ; W...cc..
    db 0x00, 0x50               ; ....cc..
    db 0x00, 0x14               ; .....cc.
    db 0x00, 0x14               ; .....cc.
cursor_color:   db 0            ; Scratch for cursor sprite

; Save/restore cursor background buffer (max 16×28=448 bytes for VESA 2x)
cursor_save_buf:    times 448 db 0
_csr_bmp_16:        dw 0            ; VESA: current bitmap row (16-bit)
_csr_buf_ptr:       dw 0            ; VESA: pointer into save buffer
_csr_row_off:       dw 0            ; VGA: precomputed VRAM row offset

; Window drag state
drag_active:        db 0            ; 1 = currently dragging a window
drag_window:        db 0            ; Window handle being dragged (0-15)
drag_offset_x:      dw 0            ; Mouse X offset from window X at grab
drag_offset_y:      dw 0            ; Mouse Y offset from window Y at grab
drag_target_x:      dw 0            ; Desired new window X position
drag_target_y:      dw 0            ; Desired new window Y position
drag_prev_buttons:  db 0            ; Previous button state (for edge detection)
drag_needs_focus:   db 0            ; 1 = need to focus dragged window
close_needs_kill:   db 0            ; 1 = close button clicked, need to kill task
close_kill_window:  db 0            ; Window handle that was close-clicked
close_btn_str:      db 'X', 0       ; Close button text

; Outline drag state (draw XOR outline during drag, move on release)
drag_outline_drawn: db 0            ; 1 = XOR outline currently visible on screen
drag_outline_x:     dw 0            ; Current outline X position
drag_outline_y:     dw 0            ; Current outline Y position
drag_outline_w:     dw 0            ; Outline width (from window)
drag_outline_h:     dw 0            ; Outline height (from window)
drag_needs_finish:  db 0            ; 1 = drag completed, need to move window
drag_finish_x:      dw 0            ; Final target X position
drag_finish_y:      dw 0            ; Final target Y position

; Window resize drag state
resize_active:      db 0            ; 1 = currently resize-dragging
resize_window:      db 0            ; Handle of window being resized
resize_start_w:     dw 0            ; Original width at grab start
resize_start_h:     dw 0            ; Original height at grab start
resize_start_mx:    dw 0            ; Mouse X at grab start
resize_start_my:    dw 0            ; Mouse Y at grab start
resize_target_w:    dw 0            ; Current target width
resize_target_h:    dw 0            ; Current target height
resize_outline_drawn: db 0          ; 1 = XOR outline currently visible
resize_needs_finish:  db 0          ; 1 = resize completed, apply now
resize_finish_w:    dw 0            ; Final width
resize_finish_h:    dw 0            ; Final height

; Keyboard demo state
demo_cursor_x: dw 0
demo_cursor_y: dw 0

; Debug character buffer (for displaying single chars)
char_buffer: db 0, 0
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
fat_start: dw 87                    ; Absolute FAT sector = filesystem_start(86) + reserved(1)
root_dir_start: dw 105              ; Calculated: 86 + reserved + (num_fats * sectors_per_fat)
data_area_start: dw 119             ; Calculated: root_dir_start + root_dir_sectors

; File handle table (16 entries, 32 bytes each)
; Entry format:
;   Byte 0: Status (0=free, 1=open)
;   Byte 1: Mount handle
;   Bytes 2-3: Starting cluster
;   Bytes 4-7: File size (32-bit)
;   Bytes 8-11: Current position (32-bit)
;   Bytes 12-31: Reserved
FILE_MAX_HANDLES    equ 16
FILE_ENTRY_SIZE     equ 32
file_table: times (FILE_MAX_HANDLES * FILE_ENTRY_SIZE) db 0

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
;   Offset 8:  2 bytes - Stack segment (context switch)
;   Offset 10: 2 bytes - Stack pointer (context switch)
;   Offset 12: 11 bytes - Filename (8.3 format)
;   Offset 23: 1 byte  - Saved draw_context
;   Offset 24: 2 bytes - Saved caller_ds
;   Offset 26: 2 bytes - Saved caller_es
;   Offset 28: 1 byte  - Saved current_font
;   Offset 29: 3 bytes - Reserved

; App entry field offsets
APP_OFF_STATE       equ 0
APP_OFF_PRIORITY    equ 1
APP_OFF_CODE_SEG    equ 2
APP_OFF_CODE_OFF    equ 4
APP_OFF_CODE_SIZE   equ 6
APP_OFF_STACK_SEG   equ 8
APP_OFF_STACK_PTR   equ 10
APP_OFF_FILENAME    equ 12
APP_OFF_DRAW_CTX    equ 23
APP_OFF_CALLER_DS   equ 24
APP_OFF_CALLER_ES   equ 26
APP_OFF_FONT        equ 28

APP_STATE_FREE      equ 0
APP_STATE_LOADED    equ 1
APP_STATE_RUNNING   equ 2
APP_STATE_SUSPENDED equ 3

APP_MAX_COUNT       equ 16
APP_ENTRY_SIZE      equ 32

; App segment constants
APP_SEGMENT_SHELL   equ 0x2000              ; Shell/launcher segment (fixed)
APP_NUM_USER_SEGS   equ 6                   ; Number of dynamic user segments
SCRATCH_SEGMENT     equ 0x9000              ; Scratch buffer / system clipboard
CLIP_MAX_SIZE       equ 4096                ; Max clipboard data size (bytes)

app_table: times (APP_MAX_COUNT * APP_ENTRY_SIZE) db 0

; Dynamic segment allocation pool (6 user segments: 0x3000-0x8000)
segment_pool:   dw 0x3000, 0x4000, 0x5000, 0x6000, 0x7000, 0x8000
segment_owner:  db 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF  ; 0xFF = free

; Shell tracking (for auto-return to launcher)
shell_handle:       dw 0xFFFF               ; Handle of shell app (0xFFFF = none)

; Cooperative multitasking scheduler state
current_task:       db 0xFF                 ; Currently running task (0xFF = kernel/none)
scheduler_last:     db 0                    ; Last task checked (for round-robin)
focused_task:       db 0xFF                 ; Task owning topmost window (receives keyboard)

; Topmost window bounds cache (for z-order clipping in INT 0x80)
; Updated by win_focus_stub and win_move_stub
topmost_handle:     db 0xFF                 ; Window handle of topmost (0xFF = none)
topmost_win_x:      dw 0
topmost_win_y:      dw 0
topmost_win_w:      dw 0
topmost_win_h:      dw 0

; ============================================================================
; Desktop Icon Data (v3.14.0)
; ============================================================================

; Desktop icon constants
DESKTOP_MAX_ICONS       equ 16
DESKTOP_ICON_SIZE       equ 80      ; 2+2+64+12 bytes per entry
DESKTOP_ICON_OFF_X      equ 0       ; word: X screen position
DESKTOP_ICON_OFF_Y      equ 2       ; word: Y screen position
DESKTOP_ICON_OFF_BITMAP equ 4       ; 64 bytes: 16x16 icon (2bpp CGA)
DESKTOP_ICON_OFF_NAME   equ 68      ; 12 bytes: display name (null-terminated)

desktop_icons:      times (DESKTOP_MAX_ICONS * DESKTOP_ICON_SIZE) db 0
desktop_icon_count: db 0
desktop_bg_color:   db 0            ; CGA palette color 0 (black)

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
;   Offset 24: 1 byte  - Content scale (1=normal, 2=double for hi-res auto-scaled)
;   Offset 25: 7 bytes - Reserved

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
WIN_OFF_CONTENT_SCALE equ 24

window_table: times (WIN_MAX_COUNT * WIN_ENTRY_SIZE) db 0

; ============================================================================
; System Clipboard (Build 273)
; ============================================================================
; Data stored at SCRATCH_SEGMENT (0x9000:0x0000), up to CLIP_MAX_SIZE bytes
clip_data_len:      dw 0                    ; Current clipboard content length

; ============================================================================
; Popup Menu State (Build 273)
; ============================================================================
kmenu_active:       db 0                    ; 1 = popup menu currently shown
kmenu_x:            dw 0                    ; Absolute screen X of menu
kmenu_y:            dw 0                    ; Absolute screen Y of menu
kmenu_w:            db 0                    ; Width in pixels (per-call)
kmenu_count:        db 0                    ; Number of items (per-call)
kmenu_str_table:    dw 0                    ; String table offset in caller's segment

; ============================================================================
; File Dialog State (Build 274)
; ============================================================================
fdlg_handle:        db 0                    ; Dialog window handle
fdlg_count:         dw 0                    ; Number of files found
fdlg_sel:           dw 0                    ; Selected index
fdlg_scroll:        dw 0                    ; First visible index (scroll position)
fdlg_mount:         db 0                    ; Mount handle for readdir
fdlg_prev_btn:      db 0                    ; Previous left button state (edge detect)
fdlg_result_di:     dw 0                    ; Caller's DI (destination buffer offset)
fdlg_save_ctx:      db 0                    ; Saved draw_context from caller
fdlg_caller_es:     dw 0                    ; Saved caller_es (app's ES segment)
fdlg_cx:            dw 0                    ; Content area X (drawing temp)
fdlg_cy:            dw 0                    ; Content area Y (drawing temp)
fdlg_item_h:        dw 10                   ; Dynamic: font_height + 2
fdlg_vis:           dw 11                   ; Dynamic: visible items (capped at 11)
fdlg_btn_h:         dw 12                   ; Dynamic: font_height + 4
fdlg_h_dyn:         dw 140                  ; Dynamic: computed window height
fdlg_y_dyn:         dw 30                   ; Dynamic: computed window Y position
fdlg_list_h:        dw 110                  ; Dynamic: vis * item_h
fdlg_dir_entry:     times 32 db 0           ; Temp buffer for readdir entries
fdlg_title:         db 'Open File', 0
fdlg_empty:         db '(No files)', 0
fdlg_str_open:      db 'Open', 0
fdlg_str_cancel:    db 'Cancel', 0

; ============================================================================
; Padding
; ============================================================================

; Pad to 40KB (80 sectors) - expanded for cursor save/restore (Build 311)
times 40960 - ($ - $$) db 0
