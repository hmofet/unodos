; SYSINFO.BIN - System Information for UnoDOS
; Build 348: KBC enable keyboard fix test.
; - Build 347: I9=0 (IRQ1 never fires), BK=0, EV=2 (WIN_REDRAW only).
;   Timer works, PIC fine, but keyboard IRQ1 dead.
; - Theory: BIOS INT 13h sends KBC cmd 0xAD (disable keyboard) during
;   multi-cluster disk reads and doesn't re-enable with 0xAE.
; - This build: reads KBC command byte before/after sending 0xAE,
;   then runs event loop with INT 9 hook to see if IRQ1 starts working.

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
KBC_CMD                 equ 0x64
KBC_DATA                equ 0x60

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

    ; === Read KBC command byte BEFORE fix ===
    call kbc_wait_write
    mov al, 0x20                ; Read command byte
    out KBC_CMD, al
    call kbc_wait_read
    in al, KBC_DATA
    mov [cs:kbc_before], al

    ; === Read port 0x64 status (is output buffer full?) ===
    in al, KBC_CMD
    mov [cs:kbc_status], al

    ; === Drain any stale data from KBC output buffer ===
    mov byte [cs:drain_count], 0
.drain_entry:
    in al, KBC_CMD
    test al, 0x01              ; Output buffer full?
    jz .drain_entry_done
    in al, KBC_DATA            ; Read and discard
    inc byte [cs:drain_count]
    jmp .drain_entry
.drain_entry_done:

    ; === Send 0xAE (enable keyboard interface) ===
    call kbc_wait_write
    mov al, 0xAE
    out KBC_CMD, al

    ; Small delay for KBC to process
    call kbc_wait_write

    ; === Read KBC command byte AFTER fix ===
    mov al, 0x20
    out KBC_CMD, al
    call kbc_wait_read
    in al, KBC_DATA
    mov [cs:kbc_after], al

    ; === Install INT 9 hook ===
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
    mov word [cs:key_count], 0
    mov ax, [cs:irq1_count]
    mov [cs:irq1_at_start], ax

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    ; Got an event
    inc word [cs:evt_count]
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    inc word [cs:key_count]
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

    ; IRQ1 delta
    mov ax, [cs:irq1_count]
    sub ax, [cs:irq1_at_start]
    mov [cs:irq1_delta], ax

    ; Redraw window
    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80
    mov ax, cs
    mov ds, ax

    ; Row 1: TIMEOUT or ESC
    mov bx, 4
    mov cx, 4
    mov si, lbl_timeout
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 2: KBC before (KB:xx) and after (KA:xx)
    movzx ax, byte [kbc_before]
    call byte_to_hex
    mov bx, 4
    mov cx, 16
    mov si, lbl_kb
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [kbc_after]
    call byte_to_hex
    mov bx, 64
    mov cx, 16
    mov si, lbl_ka
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 88
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 3: Status (ST:xx) Drain count (DR:xx)
    movzx ax, byte [kbc_status]
    call byte_to_hex
    mov bx, 4
    mov cx, 28
    mov si, lbl_st
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [drain_count]
    call byte_to_hex
    mov bx, 64
    mov cx, 28
    mov si, lbl_dr
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 88
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 4: IRQ1 count (I9:xxxx)
    mov ax, [irq1_delta]
    call word_to_hex
    mov bx, 4
    mov cx, 42
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

    ; Row 5: Event count (EV:xxxx) Key count (KY:xxxx)
    mov ax, [evt_count]
    call word_to_hex
    mov bx, 4
    mov cx, 54
    mov si, lbl_ev
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [key_count]
    call word_to_hex
    mov bx, 64
    mov cx, 54
    mov si, lbl_ky
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 88
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 6: Iteration count (IT:xxxx)
    mov ax, [iter_count]
    call word_to_hex
    mov bx, 4
    mov cx, 66
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

kbc_wait_write:
    in al, KBC_CMD
    test al, 0x02              ; Input buffer full?
    jnz kbc_wait_write
    ret

kbc_wait_read:
    in al, KBC_CMD
    test al, 0x01              ; Output buffer has data?
    jz kbc_wait_read
    ret

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

win_title:          db 'KBC', 0
wh:                 db 0
iter_count:         dw 0
evt_count:          dw 0
key_count:          dw 0
hex_buf:            db 0, 0, 0, 0, 0

kbc_before:         db 0        ; KBC cmd byte before 0xAE
kbc_after:          db 0        ; KBC cmd byte after 0xAE
kbc_status:         db 0        ; Port 0x64 status at entry
drain_count:        db 0        ; Bytes drained from output buffer

irq1_count:         dw 0
irq1_at_start:      dw 0
irq1_delta:         dw 0

; MUST be adjacent for jmp far
orig_int9_off:      dw 0
orig_int9_seg:      dw 0

lbl_wait:           db 'Press keys...', 0
lbl_timeout:        db 'TIMEOUT', 0
lbl_kb:             db 'KB:', 0     ; KBC Before
lbl_ka:             db 'KA:', 0     ; KBC After
lbl_st:             db 'ST:', 0     ; Status port 0x64
lbl_dr:             db 'DR:', 0     ; Drain count
lbl_i9:             db 'I9:', 0     ; INT 9 count
lbl_ev:             db 'EV:', 0     ; Event count
lbl_ky:             db 'KY:', 0     ; Key event count
lbl_it:             db 'IT:', 0     ; Iteration count
lbl_irq_ok:         db 'IRQ OK', 0
lbl_no_irq:         db 'NO IRQ', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
