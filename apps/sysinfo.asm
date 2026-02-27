; SYSINFO.BIN - System Information for UnoDOS
; Build 347: INT 9 hook + BIOS keyboard buffer test.
; - Build 346: PIC masks stable (D8), BT=TICKS!, IT=C350. Timer works!
;   Issue is keyboard events never reach event_get.
; - This build: hook INT 9 to count IRQ1 fires, also check INT 16h
;   (BIOS keyboard buffer) directly inside the loop.
;   This tells us: does IRQ1 fire? Does BIOS have keys? Does event_get see them?

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

    ; === Install INT 9 hook (count IRQ1 fires) ===
    cli
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

    mov bx, 4
    mov cx, 4
    mov si, lbl_wait
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Event loop: 50000 iterations
    mov word [cs:iter_count], 0
    mov word [cs:evt_count], 0
    mov word [cs:bios_key_count], 0
    mov word [cs:irq1_at_start], 0
    mov ax, [cs:irq1_count]
    mov [cs:irq1_at_start], ax

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; Check BIOS keyboard buffer directly (INT 16h AH=01h)
    mov ah, 0x01
    int 0x16
    jz .no_bios_key
    ; BIOS has a key! Count it (but don't consume — leave for INT 9/event system)
    inc word [cs:bios_key_count]
    ; Actually consume it so it doesn't pile up
    xor ah, ah
    int 0x16
.no_bios_key:

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    ; Got an event
    inc word [cs:evt_count]
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    cmp dl, 27
    je .got_esc
    jmp .main_loop

.no_event:
    inc word [cs:iter_count]
    cmp word [cs:iter_count], 50000
    jb .main_loop

    ; === TIMEOUT ===
.show_results:
    mov ax, cs
    mov ds, ax

    ; Calculate IRQ1 delta
    mov ax, [cs:irq1_count]
    sub ax, [cs:irq1_at_start]
    mov [cs:irq1_delta], ax

    ; Redraw window with results
    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80
    mov ax, cs
    mov ds, ax

    ; Row 1: TIMEOUT
    mov bx, 4
    mov cx, 4
    mov si, lbl_timeout
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 2: IRQ1 count (I9:xxxx) — how many times INT 9 fired
    mov ax, [irq1_delta]
    call word_to_hex
    mov bx, 4
    mov cx, 16
    mov si, lbl_i9
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; IRQ1 verdict
    mov bx, 64
    cmp word [irq1_delta], 0
    je .i9_zero
    mov si, lbl_irq_ok
    jmp .i9_draw
.i9_zero:
    mov si, lbl_no_irq
.i9_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 3: BIOS keyboard count (BK:xxxx) — keys found via INT 16h
    mov ax, [bios_key_count]
    call word_to_hex
    mov bx, 4
    mov cx, 28
    mov si, lbl_bk
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 4: Event count (EV:xxxx) — events from event_get
    mov ax, [evt_count]
    call word_to_hex
    mov bx, 4
    mov cx, 40
    mov si, lbl_ev
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 5: Iteration count (IT:xxxx)
    mov ax, [iter_count]
    call word_to_hex
    mov bx, 4
    mov cx, 54
    mov si, lbl_it
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Restore INT 9
    call restore_hooks

    ; Wait for ESC via fallback polling
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

.got_esc:
    call restore_hooks
    jmp .show_results

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
; Interrupt Hook
; ============================================================================

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

; ============================================================================
; Data
; ============================================================================

win_title:          db 'KBD', 0
wh:                 db 0
iter_count:         dw 0
evt_count:          dw 0
hex_buf:            db 0, 0, 0, 0, 0

irq1_count:         dw 0
irq1_at_start:      dw 0
irq1_delta:         dw 0
bios_key_count:     dw 0

; MUST be adjacent for jmp far
orig_int9_off:      dw 0
orig_int9_seg:      dw 0

lbl_wait:           db 'Press keys...', 0
lbl_timeout:        db 'TIMEOUT', 0
lbl_i9:             db 'I9:', 0     ; INT 9 fire count
lbl_bk:             db 'BK:', 0     ; BIOS Key count
lbl_ev:             db 'EV:', 0     ; Event count
lbl_it:             db 'IT:', 0     ; Iteration count
lbl_irq_ok:         db 'IRQ OK', 0
lbl_no_irq:         db 'NO IRQ', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
