; SYSINFO.BIN - System Information for UnoDOS
; Build 344: IF flag test — hook INT 8 (timer) AND INT 9 (keyboard).
; Timer fires at 18.2Hz automatically. If timer count=0 → IF=0 proven.
; If timer count>0 but keyboard count=0 → IF=1 but IRQ1 specifically broken.

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E
    db 'UI'
    db 'Sys Info', 0
    times (0x04 + 12) - ($ - $$) db 0

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

    ; === Install hooks on INT 8 (timer) and INT 9 (keyboard) ===
    cli
    push es
    xor ax, ax
    mov es, ax

    ; Save + hook INT 8 (timer, IVT offset 0x20)
    mov ax, [es:0x20]
    mov [cs:orig_int8_off], ax
    mov ax, [es:0x22]
    mov [cs:orig_int8_seg], ax
    mov word [es:0x20], int8_hook
    mov [es:0x22], cs

    ; Save + hook INT 9 (keyboard, IVT offset 0x24)
    mov ax, [es:0x24]
    mov [cs:orig_int9_off], ax
    mov ax, [es:0x26]
    mov [cs:orig_int9_seg], ax
    mov word [es:0x24], int9_hook
    mov [es:0x26], cs

    pop es
    sti

    mov word [cs:timer_count], 0
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

    mov bx, 4
    mov cx, 4
    mov si, lbl_press_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Record counts at loop start
    mov ax, [cs:timer_count]
    mov [cs:timer_at_start], ax
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

    ; Calculate deltas
    mov ax, [cs:timer_count]
    sub ax, [cs:timer_at_start]
    mov [cs:timer_delta], ax

    mov ax, [cs:irq1_count]
    sub ax, [cs:irq1_at_start]
    mov [cs:irq1_delta], ax

    mov bx, 4
    mov cx, 16
    mov si, lbl_timeout
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Timer IRQ delta (IRQ0)
    mov ax, [timer_delta]
    call word_to_hex
    mov bx, 4
    mov cx, 28
    mov si, lbl_tmr
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 40
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Keyboard IRQ delta (IRQ1)
    mov ax, [irq1_delta]
    call word_to_hex
    mov bx, 4
    mov cx, 40
    mov si, lbl_kbd
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 40
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Key events from event system
    mov ax, [key_count]
    call word_to_hex
    mov bx, 4
    mov cx, 52
    mov si, lbl_keys
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Verdict
    mov ax, [timer_delta]
    test ax, ax
    jz .if_zero
    ; Timer works — IF=1. Check keyboard
    mov ax, [irq1_delta]
    test ax, ax
    jz .irq1_dead
    ; Both work?!
    mov bx, 4
    mov cx, 66
    mov si, lbl_both_ok
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .do_fallback
.irq1_dead:
    mov bx, 4
    mov cx, 66
    mov si, lbl_irq1_dead
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .do_fallback
.if_zero:
    mov bx, 4
    mov cx, 66
    mov si, lbl_if_zero
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.do_fallback:
    call restore_hooks

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
    mov cx, 16
    mov si, lbl_kbd
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 40
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 4
    mov cx, 28
    mov si, lbl_esc_ok
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov cx, 50
.pause:
    sti
    mov ah, API_APP_YIELD
    int 0x80
    loop .pause

    call restore_hooks

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
    call restore_hooks
    jmp .exit_no_win

; ============================================================================
; Interrupt Hooks — run in interrupt context, CS = our segment
; ============================================================================

int8_hook:
    inc word [cs:timer_count]
    jmp far [cs:orig_int8_off]

int9_hook:
    inc word [cs:irq1_count]
    jmp far [cs:orig_int9_off]

; ============================================================================
; Subroutines
; ============================================================================

restore_hooks:
    cli
    push es
    push ax
    xor ax, ax
    mov es, ax
    ; Restore INT 8
    mov ax, [cs:orig_int8_off]
    mov [es:0x20], ax
    mov ax, [cs:orig_int8_seg]
    mov [es:0x22], ax
    ; Restore INT 9
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

win_title:          db 'IF Test', 0
wh:                 db 0
iter_count:         dw 0
key_count:          dw 0
hex_buf:            db 0, 0, 0, 0, 0

timer_count:        dw 0
irq1_count:         dw 0
timer_at_start:     dw 0
irq1_at_start:      dw 0
timer_delta:        dw 0
irq1_delta:         dw 0

; MUST be adjacent pairs for jmp far
orig_int8_off:      dw 0
orig_int8_seg:      dw 0
orig_int9_off:      dw 0
orig_int9_seg:      dw 0

lbl_press_esc:      db 'Press ESC...', 0
lbl_timeout:        db 'TIMEOUT', 0
lbl_esc_ok:         db 'ESC OK!', 0
lbl_keys:           db 'K:', 0
lbl_tmr:            db 'TMR:', 0    ; Timer IRQ0 delta
lbl_kbd:            db 'KBD:', 0    ; Keyboard IRQ1 delta
lbl_both_ok:        db 'Both IRQs OK', 0
lbl_irq1_dead:      db 'IRQ1 dead!', 0
lbl_if_zero:        db 'IF=0! No IRQs', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
