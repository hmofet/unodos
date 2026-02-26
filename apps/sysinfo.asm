; SYSINFO.BIN - System Information for UnoDOS
; Build 341: Event queue + KBC diagnostic.
; Reads KBC command byte (port 0x64 cmd 0x20) at entry to check if
; BIOS INT 13h floppy read disabled keyboard IRQ (bit 0 of cmd byte).
; Also shows event queue state from kernel diagnostic no_event_return.

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

    ; === Read KBC command byte at entry ===
    cli
    ; Drain any pending data
.drain_kbc:
    in al, 0x64
    test al, 0x01
    jz .drain_kbc_done
    in al, 0x60
    jmp .drain_kbc
.drain_kbc_done:
    ; Wait for KBC input buffer empty
.wait_kbc_in:
    in al, 0x64
    test al, 0x02
    jnz .wait_kbc_in
    ; Send "read command byte" command
    mov al, 0x20
    out 0x64, al
    ; Wait for response
.wait_kbc_out:
    in al, 0x64
    test al, 0x01
    jz .wait_kbc_out
    in al, 0x60
    mov [cs:kbc_cmd_before], al
    sti

    ; Create window
    mov bx, 10
    mov cx, 20
    mov dx, 200
    mov si, 100
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80

    jc .exit_no_win
    mov [wh], al

    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    mov ax, cs
    mov ds, ax

    ; Row 1: KBC command byte
    ; Format: "KBC:xx" — bit 0=kbd IRQ, bit 1=mouse IRQ
    mov al, [kbc_cmd_before]
    call byte_to_hex
    mov bx, 4
    mov cx, 4
    mov si, lbl_kbc
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 40
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Show bit 0 status (keyboard IRQ enable)
    test byte [kbc_cmd_before], 0x01
    jz .kbc_irq_off
    mov bx, 64
    mov cx, 4
    mov si, lbl_irq_on
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .draw_esc
.kbc_irq_off:
    mov bx, 64
    mov cx, 4
    mov si, lbl_irq_off
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.draw_esc:
    ; Row 2: Press ESC
    mov bx, 4
    mov cx, 16
    mov si, lbl_press_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Event loop: 5000 iterations
    mov word [cs:iter_count], 0
    mov word [cs:key_count], 0
    mov word [cs:mouse_count], 0
    mov byte [cs:got_diag], 0

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
    ; Capture diagnostic on first no_event return
    cmp byte [cs:got_diag], 0
    jne .skip_diag
    mov [cs:diag_focused], dh
    mov [cs:diag_current], dl
    mov [cs:diag_head], ch
    mov [cs:diag_tail], cl
    mov byte [cs:got_diag], 1
.skip_diag:

    inc word [cs:iter_count]
    cmp word [cs:iter_count], 5000
    jb .main_loop

    ; === TIMEOUT ===
    mov ax, cs
    mov ds, ax

    ; Read KBC command byte AFTER event loop
    cli
.drain_kbc2:
    in al, 0x64
    test al, 0x01
    jz .drain_kbc2_done
    in al, 0x60
    jmp .drain_kbc2
.drain_kbc2_done:
.wait_kbc_in2:
    in al, 0x64
    test al, 0x02
    jnz .wait_kbc_in2
    mov al, 0x20
    out 0x64, al
.wait_kbc_out2:
    in al, 0x64
    test al, 0x01
    jz .wait_kbc_out2
    in al, 0x60
    mov [cs:kbc_cmd_after], al
    sti

    ; Row 3: TIMEOUT
    mov bx, 4
    mov cx, 28
    mov si, lbl_timeout
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 4: K:xxxx M:xxxx
    mov ax, [key_count]
    call word_to_hex
    mov bx, 4
    mov cx, 40
    mov si, lbl_keys
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [mouse_count]
    call word_to_hex
    mov bx, 72
    mov cx, 40
    mov si, lbl_mouse
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 92
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 5: F:xx T:xx H:xx Q:xx
    mov al, [diag_focused]
    call byte_to_hex
    mov bx, 4
    mov cx, 52
    mov si, lbl_focus
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov al, [diag_current]
    call byte_to_hex
    mov bx, 56
    mov cx, 52
    mov si, lbl_task
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 76
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov al, [diag_head]
    call byte_to_hex
    mov bx, 108
    mov cx, 52
    mov si, lbl_head
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 128
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov al, [diag_tail]
    call byte_to_hex
    mov bx, 156
    mov cx, 52
    mov si, lbl_qtail
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 176
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 6: KBC after: "KA:xx"
    mov al, [kbc_cmd_after]
    call byte_to_hex
    mov bx, 4
    mov cx, 64
    mov si, lbl_kbc_a
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 32
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Show bit 0 status after
    test byte [kbc_cmd_after], 0x01
    jz .kbc_irq_off2
    mov bx, 56
    mov cx, 64
    mov si, lbl_irq_on
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .do_fallback
.kbc_irq_off2:
    mov bx, 56
    mov cx, 64
    mov si, lbl_irq_off
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.do_fallback:
    ; Fallback ESC via port polling
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

    cli
    in al, 0x21
    and al, 0xFD
    out 0x21, al
    sti
    jmp .exit_ok

.exit_via_events:
    mov ax, cs
    mov ds, ax
    mov bx, 4
    mov cx, 28
    mov si, lbl_esc_ok
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

win_title:          db 'Evt Diag', 0
wh:                 db 0
iter_count:         dw 0
key_count:          dw 0
mouse_count:        dw 0
got_diag:           db 0
hex_buf:            db 0, 0, 0, 0, 0

kbc_cmd_before:     db 0
kbc_cmd_after:      db 0

; Diagnostic captures
diag_focused:       db 0
diag_current:       db 0
diag_head:          db 0
diag_tail:          db 0

lbl_kbc:            db 'KBC:', 0
lbl_kbc_a:          db 'KA:', 0
lbl_irq_on:         db 'IRQ ON', 0
lbl_irq_off:        db 'IRQ OFF!', 0
lbl_press_esc:      db 'Press ESC...', 0
lbl_timeout:        db 'TIMEOUT', 0
lbl_esc_ok:         db 'ESC OK!', 0
lbl_keys:           db 'K:', 0
lbl_mouse:          db 'M:', 0
lbl_focus:          db 'F:', 0
lbl_task:           db 'T:', 0
lbl_head:           db 'H:', 0
lbl_qtail:          db 'Q:', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
