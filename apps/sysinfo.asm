; SYSINFO.BIN - System Information for UnoDOS
; Build 343: PIC ISR (In-Service Register) diagnostic.
; Reads PIC ISR to check if IRQ1 is stuck in-service (blocking
; all future keyboard interrupts). Sends multiple EOIs to clear,
; then re-checks. Also includes INT 9 hook to count IRQs.

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

    ; === Read PIC ISR BEFORE any EOI ===
    cli
    mov al, 0x0B
    out 0x20, al                    ; Read ISR command (master)
    in al, 0x20                     ; Read master ISR
    mov [cs:isr_before], al

    mov al, 0x0B
    out 0xA0, al                    ; Read ISR command (slave)
    in al, 0xA0
    mov [cs:isr_slave_before], al

    ; === Send 8 EOIs to master PIC to clear ALL stuck ISR bits ===
    mov cx, 8
.eoi_loop:
    mov al, 0x20
    out 0x20, al
    loop .eoi_loop
    ; Also clear slave
    mov al, 0x20
    out 0xA0, al

    ; === Read PIC ISR AFTER EOIs ===
    mov al, 0x0B
    out 0x20, al
    in al, 0x20
    mov [cs:isr_after], al

    ; === Drain KBC output buffer ===
.drain_kbc:
    in al, 0x64
    test al, 0x01
    jz .drain_done
    in al, 0x60
    jmp .drain_kbc
.drain_done:

    ; === Install INT 9 hook ===
    push es
    xor ax, ax
    mov es, ax
    mov ax, [es:0x24]
    mov [cs:orig_int9_off], ax
    mov ax, [es:0x26]
    mov [cs:orig_int9_seg], ax
    mov word [es:0x24], int9_hook
    mov [es:0x26], cs
    pop es
    sti

    mov word [cs:irq1_count], 0

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
    jc .exit_no_win_unhook
    mov [wh], al

    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    mov ax, cs
    mov ds, ax

    ; Row 1: ISR before EOI (master)
    mov al, [isr_before]
    call byte_to_hex
    mov bx, 4
    mov cx, 4
    mov si, lbl_isr_b
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 32
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Show if bit 1 (IRQ1) is set
    test byte [isr_before], 0x02
    jz .isr_b_ok
    mov bx, 56
    mov cx, 4
    mov si, lbl_stuck
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .show_isr_after
.isr_b_ok:
    mov bx, 56
    mov cx, 4
    mov si, lbl_clear
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.show_isr_after:
    ; Row 2: ISR after EOI
    mov al, [isr_after]
    call byte_to_hex
    mov bx, 4
    mov cx, 16
    mov si, lbl_isr_a
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 32
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 3: Slave ISR
    mov al, [isr_slave_before]
    call byte_to_hex
    mov bx, 4
    mov cx, 28
    mov si, lbl_isr_sl
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 36
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 4: Press ESC
    mov bx, 4
    mov cx, 42
    mov si, lbl_press_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Record IRQ1 count at loop start
    mov ax, [cs:irq1_count]
    mov [cs:irq1_at_start], ax

    ; Event loop: 5000 iterations
    mov word [cs:iter_count], 0
    mov word [cs:key_count], 0

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    cmp al, EVENT_KEY_PRESS
    jne .main_loop

    inc word [cs:key_count]
    cmp dl, 27
    je .exit_via_events
    jmp .main_loop

.no_event:
    inc word [cs:iter_count]
    cmp word [cs:iter_count], 5000
    jb .main_loop

    ; === TIMEOUT ===
    mov ax, cs
    mov ds, ax

    ; IRQ1 delta
    mov ax, [cs:irq1_count]
    sub ax, [cs:irq1_at_start]
    mov [cs:irq1_delta], ax

    ; Read ISR at timeout
    cli
    mov al, 0x0B
    out 0x20, al
    in al, 0x20
    mov [cs:isr_timeout], al
    sti

    ; Row 5: TIMEOUT
    mov bx, 4
    mov cx, 54
    mov si, lbl_timeout
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 6: K:xxxx D:xxxx (key events, IRQ delta)
    mov ax, [key_count]
    call word_to_hex
    mov bx, 4
    mov cx, 66
    mov si, lbl_keys
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [irq1_delta]
    call word_to_hex
    mov bx, 72
    mov cx, 66
    mov si, lbl_delta
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 92
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 7: ISR at timeout
    mov al, [isr_timeout]
    call byte_to_hex
    mov bx, 4
    mov cx, 78
    mov si, lbl_isr_t
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 32
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Show stuck/clear for timeout ISR
    test byte [isr_timeout], 0x02
    jz .isr_t_ok
    mov bx, 56
    mov cx, 78
    mov si, lbl_stuck
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .do_fallback
.isr_t_ok:
    mov bx, 56
    mov cx, 78
    mov si, lbl_clear
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.do_fallback:
    call restore_int9

    cli
    in al, 0x21
    or al, 0x02
    out 0x21, al
    sti

.drain_loop:
    in al, 0x64
    test al, 0x01
    jz .fdrain_done
    in al, 0x60
    jmp .drain_loop
.fdrain_done:

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

    mov ax, [cs:irq1_count]
    sub ax, [cs:irq1_at_start]
    call word_to_hex
    mov bx, 4
    mov cx, 54
    mov si, lbl_delta
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 4
    mov cx, 66
    mov si, lbl_esc_ok
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov cx, 50
.pause:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    loop .pause

    call restore_int9

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

.exit_no_win_unhook:
    call restore_int9
    jmp .exit_no_win

; ============================================================================
; INT 9 Hook
; ============================================================================

int9_hook:
    inc word [cs:irq1_count]
    jmp far [cs:orig_int9_off]

; ============================================================================
; Subroutines
; ============================================================================

restore_int9:
    cli
    push es
    push ax
    xor ax, ax
    mov es, ax
    mov ax, [cs:orig_int9_off]
    mov [es:0x24], ax
    mov ax, [cs:orig_int9_seg]
    mov [es:0x26], ax
    pop ax
    pop es
    sti
    ret

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

win_title:          db 'PIC ISR', 0
wh:                 db 0
iter_count:         dw 0
key_count:          dw 0
hex_buf:            db 0, 0, 0, 0, 0

irq1_count:         dw 0
irq1_at_start:      dw 0
irq1_delta:         dw 0

isr_before:         db 0
isr_after:          db 0
isr_slave_before:   db 0
isr_timeout:        db 0

; MUST be adjacent for jmp far
orig_int9_off:      dw 0
orig_int9_seg:      dw 0

lbl_isr_b:          db 'IB:', 0     ; ISR Before EOI
lbl_isr_a:          db 'IA:', 0     ; ISR After EOI
lbl_isr_sl:         db 'SL:', 0     ; Slave ISR
lbl_isr_t:          db 'IT:', 0     ; ISR at Timeout
lbl_stuck:          db 'IRQ1!', 0   ; IRQ1 stuck in-service
lbl_clear:          db 'ok', 0      ; IRQ1 clear
lbl_press_esc:      db 'Press ESC...', 0
lbl_timeout:        db 'TIMEOUT', 0
lbl_esc_ok:         db 'ESC OK!', 0
lbl_keys:           db 'K:', 0
lbl_delta:          db 'D:', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
