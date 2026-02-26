; SYSINFO.BIN - System Information for UnoDOS
; Build 337: Screen height corruption diagnostic.
; Reads screen_height (API 82) at 3 points:
;   B:xxxx = Before win_create
;   A:xxxx = After win_create
;   N:xxxx = Now (at timeout, after 200 yield+event_get loops)
; Also shows F:xx H:xx T:xx (focus, queue head, queue tail)
; Falls back to IRQ1-masked port polling for ESC exit.

[BITS 16]
[ORG 0x0000]

; --- Icon Header (80 bytes: 0x00-0x4F) ---
    db 0xEB, 0x4E                   ; JMP short to offset 0x50
    db 'UI'                         ; Magic bytes
    db 'Sys Info', 0                ; App name
    times (0x04 + 12) - ($ - $$) db 0  ; Pad name to 12 bytes

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
API_GET_SCREEN_INFO     equ 82

EVENT_KEY_PRESS         equ 1

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

    MARKER 0                        ; entry

    ; Read screen_height BEFORE win_create (API 82)
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [cs:sh_before], cx          ; CX = screen_height

    ; Create window
    mov bx, 10
    mov cx, 30
    mov dx, 200
    mov si, 80
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80

    MARKER 1                        ; win_create done

    jc .exit_no_win
    mov [wh], al

    ; Read screen_height AFTER win_create (API 82)
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [cs:sh_after], cx           ; CX = screen_height

    ; Begin draw context
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Draw initial label
    mov ax, cs
    mov ds, ax
    mov bx, 4
    mov cx, 4
    mov si, lbl_waiting
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display "B:xxxx" (screen_height before win_create)
    mov ax, [cs:sh_before]
    call word_to_hex
    mov bx, 4
    mov cx, 16
    mov si, lbl_before
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display "A:xxxx" (screen_height after win_create)
    mov ax, [cs:sh_after]
    call word_to_hex
    mov bx, 80
    mov cx, 16
    mov si, lbl_after
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 100
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    MARKER 2                        ; draw done, entering loop

    ; Try yield + event_get for a limited number of iterations
    mov word [cs:iter_count], 0

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    ; Got an event!
    MARKER 3
    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    cmp dl, 27
    je .exit_ok
    jmp .main_loop

.no_event:
    ; AL = focused_task, DL = queue_head, DH = queue_tail (from kernel)
    mov [cs:diag_focus], al
    mov [cs:diag_head], dl
    mov [cs:diag_tail], dh

    inc word [cs:iter_count]
    cmp word [cs:iter_count], 200
    jb .main_loop

    ; Timeout! Read current screen_height
    MARKER 4                        ; timeout
    mov ah, API_GET_SCREEN_INFO
    int 0x80
    mov [cs:sh_now], cx

    mov ax, cs
    mov ds, ax

    ; Display "N:xxxx" (screen_height now)
    mov ax, [sh_now]
    call word_to_hex
    mov bx, 4
    mov cx, 28
    mov si, lbl_now
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display "F:xx" (focused_task as hex byte)
    mov al, [diag_focus]
    call byte_to_hex
    mov bx, 4
    mov cx, 42
    mov si, lbl_focus
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display "H:xx" (queue head)
    mov al, [diag_head]
    call byte_to_hex
    mov bx, 64
    mov cx, 42
    mov si, lbl_head
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 84
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display "T:xx" (queue tail)
    mov al, [diag_tail]
    call byte_to_hex
    mov bx, 120
    mov cx, 42
    mov si, lbl_tail
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 140
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    MARKER 5                        ; diag displayed

    ; Fall back to IRQ1-masked port polling for exit
    cli
    in al, 0x21
    or al, 0x02
    out 0x21, al
    sti

    ; Drain any residual scancodes
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

    MARKER 6                        ; ESC

    cli
    in al, 0x21
    and al, 0xFD
    out 0x21, al
    sti

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

; word_to_hex: Convert AX (word) to 4-char hex string at hex_buf
word_to_hex:
    push ax
    push cx
    mov cx, ax              ; Save value in CX

    mov al, ch              ; High byte, high nibble
    shr al, 4
    HEXNIB
    mov [cs:hex_buf], al

    mov al, ch              ; High byte, low nibble
    and al, 0x0F
    HEXNIB
    mov [cs:hex_buf+1], al

    mov al, cl              ; Low byte, high nibble
    shr al, 4
    HEXNIB
    mov [cs:hex_buf+2], al

    mov al, cl              ; Low byte, low nibble
    and al, 0x0F
    HEXNIB
    mov [cs:hex_buf+3], al

    mov byte [cs:hex_buf+4], 0
    pop cx
    pop ax
    ret

; byte_to_hex: Convert AL (byte) to 2-char hex string at hex_buf
byte_to_hex:
    push ax
    push cx
    mov cl, al              ; Save value

    shr al, 4               ; High nibble
    HEXNIB
    mov [cs:hex_buf], al

    mov al, cl              ; Low nibble
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

win_title:      db 'System Info', 0
wh:             db 0
iter_count:     dw 0
diag_focus:     db 0
diag_head:      db 0
diag_tail:      db 0
sh_before:      dw 0
sh_after:       dw 0
sh_now:         dw 0
hex_buf:        db 0, 0, 0, 0, 0

lbl_waiting:    db 'Waiting...', 0
lbl_before:     db 'B:', 0
lbl_after:      db 'A:', 0
lbl_now:        db 'N:', 0
lbl_focus:      db 'F:', 0
lbl_head:       db 'H:', 0
lbl_tail:       db 'T:', 0

; Already > 512 bytes (2 FAT12 clusters), pad to 800
times 800 - ($ - $$) db 0x90
