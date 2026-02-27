; SYSINFO.BIN - System Information for UnoDOS
; Build 350: KBC output port + mouse vs keyboard data separation.
; - Build 349: ISR=00 (not stuck), SC=2 but LS=00 (not a keyboard scancode!).
;   Those 2 bytes are likely mouse data, not keyboard scancodes.
; - Theory: BIOS INT 13h cleared KBC output port bit 4 (IRQ1 gate),
;   or sent 0xF5 (disable scanning) to keyboard during disk I/O.
; - This build:
;   1) Reads KBC output port (cmd 0xD0) - check bit 4 (IRQ1 enable)
;   2) Separates mouse data (port 0x64 bit 5=1) from keyboard data (bit 5=0)
;   3) Sends 0xF4 (enable scanning) to keyboard
;   4) Checks if keyboard data starts arriving after 0xF4

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

    ; === Read KBC output port (cmd 0xD0) ===
    ; Drain any pending data first
.drain_pre:
    in al, KBC_CMD
    test al, 0x01
    jz .drain_pre_done
    in al, KBC_DATA
    jmp .drain_pre
.drain_pre_done:

    call kbc_wait_write
    mov al, 0xD0                ; Read output port command
    out KBC_CMD, al
    call kbc_wait_read
    in al, KBC_DATA
    mov [cs:kbc_outport], al    ; Save output port value

    ; === Send 0xF4 (Enable Scanning) to keyboard ===
    call kbc_wait_write
    mov al, 0xF4                ; Enable scanning command
    out KBC_DATA, al            ; Send to keyboard (port 0x60)

    ; Wait for ACK (0xFA) from keyboard
    mov cx, 5000
.wait_ack:
    in al, KBC_CMD
    test al, 0x01
    jz .no_ack_yet
    in al, KBC_DATA
    cmp al, 0xFA               ; ACK?
    je .got_ack
    cmp al, 0xFE               ; Resend? Try again
    je .send_f4_again
.no_ack_yet:
    loop .wait_ack
    mov byte [cs:f4_result], 'T'  ; Timeout
    jmp .f4_done
.send_f4_again:
    call kbc_wait_write
    mov al, 0xF4
    out KBC_DATA, al
    jmp .wait_ack
.got_ack:
    mov byte [cs:f4_result], 'A'  ; ACK received
.f4_done:

    ; === Read KBC output port AFTER 0xF4 ===
.drain_pre2:
    in al, KBC_CMD
    test al, 0x01
    jz .drain_pre2_done
    in al, KBC_DATA
    jmp .drain_pre2
.drain_pre2_done:

    call kbc_wait_write
    mov al, 0xD0
    out KBC_CMD, al
    call kbc_wait_read
    in al, KBC_DATA
    mov [cs:kbc_outport2], al

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
    mov word [cs:kb_count], 0       ; Keyboard data (bit5=0)
    mov word [cs:ms_count], 0       ; Mouse data (bit5=1)
    mov byte [cs:last_kb_scan], 0
    mov byte [cs:last_kb_stat], 0

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    ; === Manual KBC port poll with mouse/keyboard separation ===
    in al, KBC_CMD
    test al, 0x01              ; Output buffer full?
    jz .no_data

    mov ah, al                 ; Save status in AH
    in al, KBC_DATA            ; Read the data byte

    test ah, 0x20              ; Bit 5: 1=mouse, 0=keyboard
    jnz .is_mouse

    ; Keyboard data!
    mov [cs:last_kb_scan], al
    mov [cs:last_kb_stat], ah
    inc word [cs:kb_count]
    jmp .no_data

.is_mouse:
    inc word [cs:ms_count]

.no_data:
    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

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

    ; Redraw window
    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80
    mov ax, cs
    mov ds, ax

    ; Row 1: KBC output port before and after 0xF4
    movzx ax, byte [kbc_outport]
    call byte_to_hex
    mov bx, 4
    mov cx, 4
    mov si, lbl_op
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [kbc_outport2]
    call byte_to_hex
    mov bx, 72
    mov cx, 4
    mov si, lbl_o2
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 96
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; F4 result
    mov al, [f4_result]
    mov [cs:hex_buf], al
    mov byte [cs:hex_buf+1], 0
    mov bx, 136
    mov cx, 4
    mov si, lbl_f4
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 160
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 2: Keyboard data count + last scancode
    mov ax, [kb_count]
    call word_to_hex
    mov bx, 4
    mov cx, 16
    mov si, lbl_kb
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    movzx ax, byte [last_kb_scan]
    call byte_to_hex
    mov bx, 80
    mov cx, 16
    mov si, lbl_ks
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 104
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 3: Mouse data count
    mov ax, [ms_count]
    call word_to_hex
    mov bx, 4
    mov cx, 28
    mov si, lbl_ms
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 4: IRQ1 count + event count
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
    mov bx, 80
    mov cx, 40
    mov si, lbl_ev
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 104
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 5: Iteration count
    mov ax, [iter_count]
    call word_to_hex
    mov bx, 4
    mov cx, 52
    mov si, lbl_it
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row 6: Verdict
    mov bx, 4
    mov cx, 66
    cmp word [kb_count], 0
    ja .has_kb
    mov si, lbl_no_kb
    jmp .verdict_draw
.has_kb:
    cmp word [irq1_count], 0
    ja .all_ok
    mov si, lbl_kb_no_irq
    jmp .verdict_draw
.all_ok:
    mov si, lbl_ok
.verdict_draw:
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
kb_count:           dw 0        ; Keyboard data bytes (port 0x64 bit5=0)
ms_count:           dw 0        ; Mouse data bytes (port 0x64 bit5=1)
last_kb_scan:       db 0        ; Last keyboard scancode
last_kb_stat:       db 0        ; Last port 0x64 status for keyboard
hex_buf:            db 0, 0, 0, 0, 0

kbc_outport:        db 0        ; KBC output port before 0xF4
kbc_outport2:       db 0        ; KBC output port after 0xF4
f4_result:          db 0        ; 'A'=ACK, 'T'=timeout

irq1_count:         dw 0

; MUST be adjacent for jmp far
orig_int9_off:      dw 0
orig_int9_seg:      dw 0

lbl_wait:           db 'Press keys...', 0
lbl_op:             db 'OP:', 0     ; Output port before
lbl_o2:             db 'OA:', 0     ; Output port after
lbl_f4:             db 'F4:', 0     ; F4 result (A=ack T=timeout)
lbl_kb:             db 'KB:', 0     ; Keyboard data count
lbl_ks:             db 'KS:', 0     ; Last keyboard scancode
lbl_ms:             db 'MS:', 0     ; Mouse data count
lbl_i9:             db 'I9:', 0     ; INT 9 count
lbl_ev:             db 'EV:', 0     ; Event count
lbl_it:             db 'IT:', 0     ; Iteration count
lbl_no_kb:          db 'NO KB DATA', 0
lbl_kb_no_irq:      db 'KB OK NO IRQ', 0
lbl_ok:             db 'ALL OK', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
