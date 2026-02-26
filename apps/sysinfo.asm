; SYSINFO.BIN - System Information for UnoDOS
; Build 336: Event delivery diagnostic with readable text output.
; Kernel event_get returns diagnostic info on no-event:
;   AL = focused_task, DL = queue_head, DH = queue_tail
; App displays: "F:x T:y H:a T:b" (Focus, Task, Head, Tail)
; Then falls back to direct port polling for ESC exit.

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

    MARKER 1                        ; win_create

    jc .exit_no_win
    mov [wh], al

    ; Begin draw context for text output
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Draw initial label
    mov bx, 4
    mov cx, 4
    mov ax, cs
    mov ds, ax
    mov si, lbl_waiting
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
    ; Save diagnostic values
    mov [cs:diag_focus], al
    mov [cs:diag_head], dl
    mov [cs:diag_tail], dh

    inc word [cs:iter_count]
    cmp word [cs:iter_count], 200
    jb .main_loop

    ; Timeout! Display diagnostic info
    MARKER 4                        ; timeout

    ; Draw "F:xx" (focused_task as hex)
    mov ax, cs
    mov ds, ax

    mov al, [diag_focus]
    shr al, 4
    HEXNIB
    mov [hex_buf], al
    mov al, [diag_focus]
    and al, 0x0F
    HEXNIB
    mov [hex_buf+1], al
    mov byte [hex_buf+2], 0

    mov bx, 4
    mov cx, 20
    mov si, lbl_focus
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw "H:xx" (queue head as hex)
    mov al, [diag_head]
    shr al, 4
    HEXNIB
    mov [hex_buf], al
    mov al, [diag_head]
    and al, 0x0F
    HEXNIB
    mov [hex_buf+1], al

    mov bx, 4
    mov cx, 34
    mov si, lbl_head
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Draw "T:xx" (queue tail as hex)
    mov al, [diag_tail]
    shr al, 4
    HEXNIB
    mov [hex_buf], al
    mov al, [diag_tail]
    and al, 0x0F
    HEXNIB
    mov [hex_buf+1], al

    mov bx, 4
    mov cx, 48
    mov si, lbl_tail
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 28
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
; Data
; ============================================================================

win_title:      db 'System Info', 0
wh:             db 0
iter_count:     dw 0
diag_focus:     db 0
diag_head:      db 0
diag_tail:      db 0
hex_buf:        db 0, 0, 0

lbl_waiting:    db 'Waiting...', 0
lbl_focus:      db 'F:', 0
lbl_head:       db 'H:', 0
lbl_tail:       db 'T:', 0

; Pad to force 2 FAT12 clusters (> 512 bytes)
times 600 - ($ - $$) db 0x90
