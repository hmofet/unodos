; SYSINFO.BIN - System Information for UnoDOS
; Build 346: PIC mask + long iteration test.
; - Build 345 showed IF=1 at all checkpoints, but BT=0 TH=0.
;   Theory A: loop too fast (<55ms on fast 286+), no timer tick in window.
;   Theory B: PIC masks IRQ0 during yield/event_get.
; - This build: 50000 iterations (should take seconds on any CPU).
;   Reads PIC mask before loop, OR-accumulates inside loop, reads after.
;   Also reads BIOS tick counter before/after.
; - No INT 8/9 hooks (simplify — rely on BIOS tick counter).

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

    ; Read PIC mask at entry (before anything else)
    in al, 0x21
    mov [cs:pic_mask_entry], al
    in al, 0xA1
    mov [cs:pic_slave_entry], al

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

    mov bx, 4
    mov cx, 4
    mov si, lbl_wait
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Read PIC mask after window create (kernel may have modified it)
    in al, 0x21
    mov [cs:pic_mask_after_win], al

    ; Read BIOS tick counter BEFORE event loop
    push es
    mov ax, 0x0040
    mov es, ax
    mov ax, [es:0x006C]
    mov [cs:bios_tick_start], ax
    pop es

    ; Event loop: 50000 iterations (should take seconds)
    mov word [cs:iter_count], 0
    mov byte [cs:pic_mask_or], 0        ; OR-accumulate PIC mask inside loop

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; Read PIC mask inside loop and OR-accumulate
    in al, 0x21
    or [cs:pic_mask_or], al

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    ; Got an event — check if ESC
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

    ; Read BIOS tick counter AFTER
    push es
    mov ax, 0x0040
    mov es, ax
    mov ax, [es:0x006C]
    mov [cs:bios_tick_end], ax
    pop es

    ; Read PIC mask at end
    in al, 0x21
    mov [cs:pic_mask_end], al

    ; BIOS tick delta
    mov ax, [cs:bios_tick_end]
    sub ax, [cs:bios_tick_start]
    mov [cs:bios_tick_delta], ax

    ; Redraw window with results
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

    ; Row 2: PIC mask at entry (PE:xx)
    movzx ax, byte [pic_mask_entry]
    call byte_to_hex
    mov bx, 4
    mov cx, 16
    mov si, lbl_pe
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; PIC slave at entry (PS:xx)
    movzx ax, byte [pic_slave_entry]
    call byte_to_hex
    mov bx, 60
    mov cx, 16
    mov si, lbl_ps
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 84
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 3: PIC after win (PW:xx), PIC OR in loop (PO:xx)
    movzx ax, byte [pic_mask_after_win]
    call byte_to_hex
    mov bx, 4
    mov cx, 28
    mov si, lbl_pw
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [pic_mask_or]
    call byte_to_hex
    mov bx, 60
    mov cx, 28
    mov si, lbl_po
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 84
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 4: PIC at end (PX:xx)
    movzx ax, byte [pic_mask_end]
    call byte_to_hex
    mov bx, 4
    mov cx, 40
    mov si, lbl_px
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 5: BIOS tick delta (BT:xxxx)
    mov ax, [bios_tick_delta]
    call word_to_hex
    mov bx, 4
    mov cx, 54
    mov si, lbl_bt
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; BT verdict
    mov bx, 60
    cmp word [bios_tick_delta], 0
    je .bt_zero
    mov si, lbl_ok
    jmp .bt_draw
.bt_zero:
    mov si, lbl_no_tick
.bt_draw:
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

    ; Wait for ESC via fallback polling
    ; Mask IRQ1 for clean port polling
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

    ; Unmask IRQ1
    cli
    in al, 0x21
    and al, 0xFD
    out 0x21, al
    sti
    jmp .exit_ok

.got_esc:
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

; ============================================================================
; Subroutines
; ============================================================================

byte_to_hex:
    ; Input: AL = byte, Output: hex_buf[0..1] + null
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

win_title:          db 'PIC', 0
wh:                 db 0
iter_count:         dw 0
hex_buf:            db 0, 0, 0, 0, 0

pic_mask_entry:     db 0
pic_slave_entry:    db 0
pic_mask_after_win: db 0
pic_mask_or:        db 0        ; OR-accumulated PIC mask inside loop
pic_mask_end:       db 0

bios_tick_start:    dw 0
bios_tick_end:      dw 0
bios_tick_delta:    dw 0

lbl_wait:           db 'Wait...', 0
lbl_timeout:        db 'TIMEOUT', 0
lbl_pe:             db 'PE:', 0     ; PIC Entry
lbl_ps:             db 'PS:', 0     ; PIC Slave
lbl_pw:             db 'PW:', 0     ; PIC after Win
lbl_po:             db 'PO:', 0     ; PIC OR in loop
lbl_px:             db 'PX:', 0     ; PIC at eXit
lbl_bt:             db 'BT:', 0     ; BIOS Ticks
lbl_it:             db 'IT:', 0     ; Iteration count
lbl_ok:             db 'TICKS!', 0
lbl_no_tick:        db 'NO TICK', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
