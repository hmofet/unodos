; SYSINFO.BIN - System Information for UnoDOS
; Build 340: IVT corruption diagnostic.
; Reads INT 9 (keyboard), INT 74h (mouse), INT 80h (API) vectors
; from IVT at 0x0000:xxxx to check if BIOS INT 13h floppy read
; corrupted them during 2-cluster app load.

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

    MARKER 0

    ; === Read IVT entries BEFORE anything else ===
    ; IVT is at 0x0000:0000, each entry = 4 bytes (offset:segment)
    push es
    xor ax, ax
    mov es, ax

    ; INT 9 (keyboard) at IVT offset 0x24 (9 * 4)
    mov ax, [es:0x24]              ; offset
    mov [cs:ivt9_off], ax
    mov ax, [es:0x26]              ; segment
    mov [cs:ivt9_seg], ax

    ; INT 74h (IRQ12/mouse) at IVT offset 0x1D0 (0x74 * 4)
    mov ax, [es:0x1D0]            ; offset
    mov [cs:ivt74_off], ax
    mov ax, [es:0x1D2]            ; segment
    mov [cs:ivt74_seg], ax

    ; INT 80h (API) at IVT offset 0x200 (0x80 * 4)
    mov ax, [es:0x200]            ; offset
    mov [cs:ivt80_off], ax
    mov ax, [es:0x202]            ; segment
    mov [cs:ivt80_seg], ax

    pop es

    ; Create window
    mov bx, 10
    mov cx, 20
    mov dx, 200
    mov si, 120
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80

    MARKER 1

    jc .exit_no_win
    mov [wh], al

    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    mov ax, cs
    mov ds, ax

    ; === Row 1: INT 9 vector (keyboard) ===
    ; Format: "I9:SSSS:OOOO" where SSSS=segment, OOOO=offset
    mov bx, 4
    mov cx, 4
    mov si, lbl_int9
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [ivt9_seg]
    call word_to_hex
    mov bx, 28
    mov cx, 4
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 60
    mov cx, 4
    mov si, lbl_colon
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [ivt9_off]
    call word_to_hex
    mov bx, 68
    mov cx, 4
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Show OK/BAD for INT 9
    mov ax, [ivt9_seg]
    cmp ax, 0x1000
    jne .int9_bad
    mov bx, 108
    mov cx, 4
    mov si, lbl_ok
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .draw_int74
.int9_bad:
    mov bx, 108
    mov cx, 4
    mov si, lbl_bad
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.draw_int74:
    ; === Row 2: INT 74h vector (mouse IRQ12) ===
    mov bx, 4
    mov cx, 16
    mov si, lbl_int74
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [ivt74_seg]
    call word_to_hex
    mov bx, 36
    mov cx, 16
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 68
    mov cx, 16
    mov si, lbl_colon
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [ivt74_off]
    call word_to_hex
    mov bx, 76
    mov cx, 16
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Show OK/BAD for INT 74h
    mov ax, [ivt74_seg]
    cmp ax, 0x1000
    jne .int74_bad
    mov bx, 116
    mov cx, 16
    mov si, lbl_ok
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .draw_int80
.int74_bad:
    mov bx, 116
    mov cx, 16
    mov si, lbl_bad
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.draw_int80:
    ; === Row 3: INT 80h vector (API) ===
    mov bx, 4
    mov cx, 28
    mov si, lbl_int80
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [ivt80_seg]
    call word_to_hex
    mov bx, 36
    mov cx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 68
    mov cx, 28
    mov si, lbl_colon
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [ivt80_off]
    call word_to_hex
    mov bx, 76
    mov cx, 28
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Show OK/BAD for INT 80h
    mov ax, [ivt80_seg]
    cmp ax, 0x1000
    jne .int80_bad
    mov bx, 116
    mov cx, 28
    mov si, lbl_ok
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    jmp .draw_esc
.int80_bad:
    mov bx, 116
    mov cx, 28
    mov si, lbl_bad
    mov ah, API_GFX_DRAW_STRING
    int 0x80

.draw_esc:
    ; Display instructions
    mov bx, 4
    mov cx, 42
    mov si, lbl_press_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    MARKER 2

    ; Event loop: 5000 iterations
    mov word [cs:iter_count], 0
    mov word [cs:key_count], 0
    mov word [cs:mouse_count], 0

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
    inc word [cs:iter_count]
    cmp word [cs:iter_count], 5000
    jb .main_loop

    ; === TIMEOUT - re-read IVT to see if it changed ===
    MARKER 4

    push es
    xor ax, ax
    mov es, ax
    mov ax, [es:0x26]              ; INT 9 segment after loop
    mov [cs:ivt9_seg_after], ax
    mov ax, [es:0x24]              ; INT 9 offset after loop
    mov [cs:ivt9_off_after], ax
    pop es

    mov ax, cs
    mov ds, ax

    ; Show TIMEOUT
    mov bx, 4
    mov cx, 56
    mov si, lbl_timeout
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; K:xxxx M:xxxx
    mov ax, [key_count]
    call word_to_hex
    mov bx, 4
    mov cx, 68
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
    mov cx, 68
    mov si, lbl_mouse
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 92
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Row: "9A:SSSS:OOOO" (INT 9 vector AFTER event loop)
    mov bx, 4
    mov cx, 82
    mov si, lbl_int9a
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [ivt9_seg_after]
    call word_to_hex
    mov bx, 28
    mov cx, 82
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov bx, 60
    mov cx, 82
    mov si, lbl_colon
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    mov ax, [ivt9_off_after]
    call word_to_hex
    mov bx, 68
    mov cx, 82
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    MARKER 5

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

    MARKER 6

    cli
    in al, 0x21
    and al, 0xFD
    out 0x21, al
    sti
    jmp .exit_ok

.exit_via_events:
    MARKER 3

    mov ax, cs
    mov ds, ax
    mov bx, 4
    mov cx, 56
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

; ============================================================================
; Data
; ============================================================================

win_title:          db 'IVT Check', 0
wh:                 db 0
iter_count:         dw 0
key_count:          dw 0
mouse_count:        dw 0
hex_buf:            db 0, 0, 0, 0, 0

; IVT vectors read at entry
ivt9_off:           dw 0
ivt9_seg:           dw 0
ivt74_off:          dw 0
ivt74_seg:          dw 0
ivt80_off:          dw 0
ivt80_seg:          dw 0

; IVT vectors read after event loop
ivt9_off_after:     dw 0
ivt9_seg_after:     dw 0

lbl_int9:           db 'I9:', 0
lbl_int74:          db 'I74:', 0
lbl_int80:          db 'I80:', 0
lbl_int9a:          db '9A:', 0
lbl_colon:          db ':', 0
lbl_ok:             db 'OK', 0
lbl_bad:            db 'BAD!', 0
lbl_press_esc:      db 'Press ESC...', 0
lbl_timeout:        db 'TIMEOUT', 0
lbl_esc_ok:         db 'ESC OK!', 0
lbl_keys:           db 'K:', 0
lbl_mouse:          db 'M:', 0

; Pad to 3 FAT12 clusters (> 1024 bytes)
times 1536 - ($ - $$) db 0x90
