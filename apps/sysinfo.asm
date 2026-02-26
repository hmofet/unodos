; SYSINFO.BIN - System Information for UnoDOS
; Build 339: PIC mask diagnostic + force unmask test.
; Reads PIC masks (ports 0x21, 0xA1) at entry to check if BIOS
; INT 13h floppy read masked IRQ1 (keyboard) or IRQ12 (mouse).
; Then force-unmasks all critical IRQs and tests event delivery.

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Sys Info', 0                ; App name
    times (0x04 + 12) - ($ - $$) db 0

    ; 16x16 icon bitmap (64 bytes, 2bpp CGA format)
    db 0x00, 0x00, 0x00, 0x00
    db 0x15, 0x55, 0x55, 0x54
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x35, 0x55, 0x55, 0x5C
    db 0x3F, 0xFF, 0xFF, 0xFC
    db 0x00, 0x3F, 0xFC, 0x00
    db 0x00, 0x0F, 0xF0, 0x00
    db 0x00, 0x3F, 0xFC, 0x00
    db 0x03, 0xFF, 0xFF, 0xC0
    db 0x00, 0x00, 0x00, 0x00

    times 0x50 - ($ - $$) db 0

; --- Code Entry (offset 0x50) ---

API_GFX_DRAW_STRING    equ 4
API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_BEGIN_DRAW      equ 31
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34

EVENT_KEY_PRESS         equ 1
EVENT_MOUSE             equ 3

; Macro: write marker N to CGA VRAM row 0
%macro MARKER 1
    push es
    push bx
    mov bx, 0xB800
    mov es, bx
    mov byte [es:%1*2], 0xFF
    pop bx
    pop es
%endmacro

; Macro: convert nibble in AL (0-15) to hex char in AL
%macro HEXNIB 0
    cmp al, 10
    jb %%digit
    add al, 'A' - 10
    jmp %%done
%%digit:
    add al, '0'
%%done:
%endmacro

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    MARKER 0

    ; === Read PIC masks BEFORE anything else ===
    in al, 0x21                     ; Master PIC mask
    mov [cs:pic_master], al
    in al, 0xA1                     ; Slave PIC mask
    mov [cs:pic_slave], al

    ; === Force unmask critical IRQs ===
    ; Send EOI to both PICs (clear any pending ISR)
    mov al, 0x20
    out 0x20, al
    out 0xA0, al

    ; Unmask IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade) on master
    in al, 0x21
    and al, 0xF8                    ; Clear bits 0,1,2
    out 0x21, al

    ; Unmask IRQ12 (mouse) on slave
    in al, 0xA1
    and al, 0xEF                    ; Clear bit 4
    out 0xA1, al

    ; Read PIC masks AFTER unmask
    in al, 0x21
    mov [cs:pic_master_after], al
    in al, 0xA1
    mov [cs:pic_slave_after], al

    ; Create window
    mov bx, 10
    mov cx, 20
    mov dx, 200
    mov si, 120
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80

    MARKER 1

    jc .exit_no_win
    mov [wh], al

    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    mov ax, cs
    mov ds, ax

    ; Display "P:xx" (master PIC mask before)
    mov al, [pic_master]
    call byte_to_hex
    mov bx, 4
    mov cx, 4
    mov si, lbl_pic_m
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display "S:xx" (slave PIC mask before)
    mov al, [pic_slave]
    call byte_to_hex
    mov bx, 72
    mov cx, 4
    mov si, lbl_pic_s
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 92
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display "p:xx" (master after unmask)
    mov al, [pic_master_after]
    call byte_to_hex
    mov bx, 4
    mov cx, 16
    mov si, lbl_pic_m2
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display "s:xx" (slave after unmask)
    mov al, [pic_slave_after]
    call byte_to_hex
    mov bx, 72
    mov cx, 16
    mov si, lbl_pic_s2
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 92
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display instructions
    mov bx, 4
    mov cx, 30
    mov si, lbl_press_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    MARKER 2

    ; Event loop: 5000 iterations
    mov word [cs:iter_count], 0
    mov word [cs:key_count], 0
    mov word [cs:mouse_count], 0

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    cmp al, EVENT_KEY_PRESS
    jne .check_mouse

    inc word [cs:key_count]
    cmp dl, 27                      ; ESC?
    je .exit_via_events
    jmp .main_loop

.check_mouse:
    cmp al, EVENT_MOUSE
    jne .main_loop
    inc word [cs:mouse_count]
    jmp .main_loop

.no_event:
    inc word [cs:iter_count]
    cmp word [cs:iter_count], 5000
    jb .main_loop

    ; === TIMEOUT ===
    MARKER 4

    mov ax, cs
    mov ds, ax

    mov bx, 4
    mov cx, 44
    mov si, lbl_timeout
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; K:xxxx
    mov ax, [key_count]
    call word_to_hex
    mov bx, 4
    mov cx, 56
    mov si, lbl_keys
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; M:xxxx
    mov ax, [mouse_count]
    call word_to_hex
    mov bx, 72
    mov cx, 56
    mov si, lbl_mouse
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 92
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    MARKER 5

    ; Fallback exit
    cli
    in al, 0x21
    or al, 0x02
    out 0x21, al
    sti

.drain_loop:
    in al, 0x64
    test al, 0x01
    jz .drain_done
    in al, 0x60
    jmp .drain_loop
.drain_done:

.fallback_loop:
    in al, 0x64
    test al, 0x01
    jz .fallback_loop
    in al, 0x60
    test al, 0x80
    jnz .fallback_loop
    cmp al, 0x01
    jne .fallback_loop

    MARKER 6

    cli
    in al, 0x21
    and al, 0xFD
    out 0x21, al
    sti
    jmp .exit_ok

.exit_via_events:
    MARKER 3

    mov ax, cs
    mov ds, ax
    mov bx, 4
    mov cx, 44
    mov si, lbl_ok
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; K:xxxx
    mov ax, [key_count]
    call word_to_hex
    mov bx, 4
    mov cx, 56
    mov si, lbl_keys
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; M:xxxx
    mov ax, [mouse_count]
    call word_to_hex
    mov bx, 72
    mov cx, 56
    mov si, lbl_mouse
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 92
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Brief pause
    mov cx, 50
.pause:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    loop .pause

.exit_ok:
    mov ah, API_WIN_END_DRAW
    int 0x80
    mov al, [cs:wh]
    mov ah, API_WIN_DESTROY
    int 0x80

.exit_no_win:
    pop es
    pop ds
    popa
    retf

; ============================================================================
; Subroutines
; ============================================================================

word_to_hex:
    push ax
    push cx
    mov cx, ax

    mov al, ch
    shr al, 4
    HEXNIB
    mov [cs:hex_buf], al

    mov al, ch
    and al, 0x0F
    HEXNIB
    mov [cs:hex_buf+1], al

    mov al, cl
    shr al, 4
    HEXNIB
    mov [cs:hex_buf+2], al

    mov al, cl
    and al, 0x0F
    HEXNIB
    mov [cs:hex_buf+3], al

    mov byte [cs:hex_buf+4], 0
    pop cx
    pop ax
    ret

byte_to_hex:
    push ax
    push cx
    mov cl, al

    shr al, 4
    HEXNIB
    mov [cs:hex_buf], al

    mov al, cl
    and al, 0x0F
    HEXNIB
    mov [cs:hex_buf+1], al

    mov byte [cs:hex_buf+2], 0
    pop cx
    pop ax
    ret

; ============================================================================
; Data
; ============================================================================

win_title:          db 'System Info', 0
wh:                 db 0
iter_count:         dw 0
key_count:          dw 0
mouse_count:        dw 0
pic_master:         db 0
pic_slave:          db 0
pic_master_after:   db 0
pic_slave_after:    db 0
hex_buf:            db 0, 0, 0, 0, 0

lbl_pic_m:          db 'P:', 0      ; Master PIC mask (before)
lbl_pic_s:          db 'S:', 0      ; Slave PIC mask (before)
lbl_pic_m2:         db 'p:', 0      ; Master PIC mask (after unmask)
lbl_pic_s2:         db 's:', 0      ; Slave PIC mask (after unmask)
lbl_press_esc:      db 'Press ESC...', 0
lbl_timeout:        db 'TIMEOUT', 0
lbl_ok:             db 'ESC OK!', 0
lbl_keys:           db 'K:', 0
lbl_mouse:          db 'M:', 0

; Pad to 2 FAT12 clusters (> 512 bytes)
times 1024 - ($ - $$) db 0x90
