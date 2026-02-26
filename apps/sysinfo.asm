; SYSINFO.BIN - System Information for UnoDOS
; Build 338: Long-duration event system test.
; Shows screen_height (B:xxxx), then runs 5000-iteration event loop.
; If ESC via events: exits with marker 3 (event system works).
; If timeout: marker 4 + fallback port polling (event system broken).
; Key test: does the user have time to press ESC via the event system?

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

    ; Begin draw context (restore window handle in AL first)
    mov al, [cs:wh]
    mov ah, API_WIN_BEGIN_DRAW
    int 0x80

    ; Display "B:xxxx" (screen_height before)
    mov ax, cs
    mov ds, ax
    mov ax, [sh_before]
    call word_to_hex
    mov bx, 4
    mov cx, 4
    mov si, lbl_before
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display instructions
    mov bx, 4
    mov cx, 16
    mov si, lbl_press_esc
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    MARKER 2                        ; draw done, entering loop

    ; Event loop: 5000 iterations (should be several seconds)
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

    ; Got an event!
    cmp al, EVENT_KEY_PRESS
    jne .check_mouse

    ; Key press event
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

    ; === TIMEOUT: event system appears broken ===
    MARKER 4                        ; timeout

    mov ax, cs
    mov ds, ax

    ; Display "TIMEOUT" label
    mov bx, 4
    mov cx, 30
    mov si, lbl_timeout
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display key event count: "K:xxxx"
    mov ax, [key_count]
    call word_to_hex
    mov bx, 4
    mov cx, 42
    mov si, lbl_keys
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display mouse event count: "M:xxxx"
    mov ax, [mouse_count]
    call word_to_hex
    mov bx, 80
    mov cx, 42
    mov si, lbl_mouse
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 100
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Display iteration count: "I:xxxx"
    mov ax, [iter_count]
    call word_to_hex
    mov bx, 4
    mov cx, 54
    mov si, lbl_iter
    mov ah, API_GFX_DRAW_STRING
    int 0x80
    mov bx, 24
    mov si, hex_buf
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    MARKER 5                        ; diag displayed

    ; Fall back to IRQ1-masked port polling for exit
    cli
    in al, 0x21
    or al, 0x02                     ; Mask IRQ1
    out 0x21, al
    sti

    ; Drain residual scancodes
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
    test al, 0x80                   ; Skip break codes
    jnz .fallback_loop
    cmp al, 0x01                    ; ESC scancode
    jne .fallback_loop

    MARKER 6                        ; ESC via fallback

    ; Unmask IRQ1
    cli
    in al, 0x21
    and al, 0xFD
    out 0x21, al
    sti
    jmp .exit_ok

.exit_via_events:
    ; ESC received through the event system!
    MARKER 3                        ; event system works!

    mov ax, cs
    mov ds, ax
    mov bx, 4
    mov cx, 30
    mov si, lbl_ok
    mov ah, API_GFX_DRAW_STRING
    int 0x80

    ; Brief pause so user can see "OK" message
    mov cx, 30
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

; word_to_hex: Convert AX (word) to 4-char hex string at hex_buf
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

win_title:      db 'System Info', 0
wh:             db 0
iter_count:     dw 0
key_count:      dw 0
mouse_count:    dw 0
sh_before:      dw 0
hex_buf:        db 0, 0, 0, 0, 0

lbl_before:     db 'B:', 0
lbl_press_esc:  db 'Press ESC...', 0
lbl_timeout:    db 'TIMEOUT', 0
lbl_ok:         db 'ESC OK!', 0
lbl_keys:       db 'K:', 0
lbl_mouse:      db 'M:', 0
lbl_iter:       db 'I:', 0

; Pad to 2 FAT12 clusters (> 512 bytes)
times 800 - ($ - $$) db 0x90
