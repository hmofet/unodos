; SYSINFO.BIN - System Information for UnoDOS
; Build 349: PIC ISR/IRR diagnostic + manual KBC port polling.
; - Build 348: KBC cmd byte 0x47 (correct), 0xAE no change, IRQ1 still dead.
; - Build 347: I9=0 (IRQ1 never fires), BK=0, EV=2 (WIN_REDRAW only).
; - Theory: IRQ1 stuck in PIC ISR (missed EOI). If ISR bit 1 = 1,
;   IRQ1 is in-service and PIC won't deliver another until EOI sent.
; - This build: reads PIC ISR+IRR, polls port 0x64 for scancodes,
;   hooks INT 9 to count IRQ1 fires.

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

    ; === Read PIC ISR (In-Service Register) ===
    mov al, 0x0B            ; Read ISR command
    out 0x20, al
    in al, 0x20             ; Master PIC ISR
    mov [cs:pic_isr], al

    ; === Read PIC IRR (Interrupt Request Register) ===
    mov al, 0x0A            ; Read IRR command
    out 0x20, al
    in al, 0x20             ; Master PIC IRR
    mov [cs:pic_irr], al

    ; === Read PIC mask ===
    in al, 0x21
    mov [cs:pic_mask], al

    ; === If ISR bit 1 set, send EOI to try to unstick IRQ1 ===
    test byte [cs:pic_isr], 0x02
    jz .no_eoi_fix
    mov al, 0x20            ; Non-specific EOI
    out 0x20, al
    ; Read ISR again after EOI
    mov al, 0x0B
    out 0x20, al
    in al, 0x20
    mov [cs:pic_isr_after], al
    mov byte [cs:sent_eoi], 1
    jmp .eoi_done
.no_eoi_fix:
    mov byte [cs:pic_isr_after], 0
    mov byte [cs:sent_eoi], 0
.eoi_done:

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
    mov word [cs:scan_count], 0
    mov byte [cs:last_scan], 0

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; === Manual KBC port poll ===
    in al, KBC_CMD
    test al, 0x01              ; Output buffer full? (scancode waiting?)
    jz .no_scancode
    in al, KBC_DATA            ; Read the scancode
    mov [cs:last_scan], al
    inc word [cs:scan_count]
.no_scancode:

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    ; Got an event
    inc word [cs:evt_count]
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    cmp dl, 27
    je .show_results
    jmp .main_loop

.no_event:
    inc word [cs:iter_count]
    cmp word [cs:iter_count], 50000
    jb .main_loop

    ; === TIMEOUT - show results ===
.show_results:
    mov ax, cs
    mov ds, ax

    ; Read PIC ISR at exit
    mov al, 0x0B
    out 0x20, al
    in al, 0x20
    mov [cs:pic_isr_exit], al

    ; Redraw window
    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80
    mov ax, cs
    mov ds, ax

    ; Row 1: ISR at entry
    movzx ax, byte [pic_isr]
    call byte_to_hex
    mov bx, 4
    mov cx, 4
    mov si, lbl_is
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; IRR at entry
    movzx ax, byte [pic_irr]
    call byte_to_hex
    mov bx, 64
    mov cx, 4
    mov si, lbl_ir
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 88
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Mask
    movzx ax, byte [pic_mask]
    call byte_to_hex
    mov bx, 128
    mov cx, 4
    mov si, lbl_pm
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 152
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 2: EOI sent? ISR after EOI? ISR at exit?
    movzx ax, byte [sent_eoi]
    call byte_to_hex
    mov bx, 4
    mov cx, 16
    mov si, lbl_eo
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [pic_isr_after]
    call byte_to_hex
    mov bx, 64
    mov cx, 16
    mov si, lbl_ia
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 88
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [pic_isr_exit]
    call byte_to_hex
    mov bx, 128
    mov cx, 16
    mov si, lbl_ix
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 152
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 3: Manual scancode count + last scancode
    mov ax, [scan_count]
    call word_to_hex
    mov bx, 4
    mov cx, 28
    mov si, lbl_sc
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [last_scan]
    call byte_to_hex
    mov bx, 88
    mov cx, 28
    mov si, lbl_ls
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 112
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 4: IRQ1 count (I9) + event count (EV)
    mov ax, [irq1_count]
    call word_to_hex
    mov bx, 4
    mov cx, 40
    mov si, lbl_i9
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [evt_count]
    call word_to_hex
    mov bx, 88
    mov cx, 40
    mov si, lbl_ev
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 112
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 5: Verdict
    mov bx, 4
    mov cx, 54
    test byte [pic_isr], 0x02
    jnz .verdict_stuck
    cmp word [scan_count], 0
    ja .verdict_sc_ok
    mov si, lbl_no_sc
    jmp .verdict_draw
.verdict_stuck:
    mov si, lbl_stuck
    jmp .verdict_draw
.verdict_sc_ok:
    mov si, lbl_sc_ok
.verdict_draw:
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 6: iteration count
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
scan_count:         dw 0
last_scan:          db 0
hex_buf:            db 0, 0, 0, 0, 0

pic_isr:            db 0        ; PIC ISR at entry
pic_irr:            db 0        ; PIC IRR at entry
pic_mask:           db 0        ; PIC mask at entry
pic_isr_after:      db 0        ; PIC ISR after EOI fix
pic_isr_exit:       db 0        ; PIC ISR at loop exit
sent_eoi:           db 0        ; 1 if we sent EOI to fix stuck IRQ1

irq1_count:         dw 0

; MUST be adjacent for jmp far
orig_int9_off:      dw 0
orig_int9_seg:      dw 0

lbl_wait:           db 'Press keys...', 0
lbl_is:             db 'IS:', 0     ; ISR at entry
lbl_ir:             db 'IR:', 0     ; IRR at entry
lbl_pm:             db 'PM:', 0     ; PIC Mask
lbl_eo:             db 'EO:', 0     ; EOI sent?
lbl_ia:             db 'IA:', 0     ; ISR after EOI
lbl_ix:             db 'IX:', 0     ; ISR at exit
lbl_sc:             db 'SC:', 0     ; Scancode count (manual poll)
lbl_ls:             db 'LS:', 0     ; Last scancode
lbl_i9:             db 'I9:', 0     ; INT 9 count
lbl_ev:             db 'EV:', 0     ; Event count
lbl_it:             db 'IT:', 0     ; Iteration count
lbl_stuck:          db 'IRQ1 STUCK!', 0
lbl_no_sc:          db 'NO SCANCODE', 0
lbl_sc_ok:          db 'SC OK I9 BAD', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
