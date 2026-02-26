; SYSINFO.BIN - System Information for UnoDOS
; Build 335: Mask IRQ1 then poll keyboard port directly.
; Build 334 showed 5 markers (Enter residual consumed) but no new keys.
; This proves INT 9 handler (via IRQ1) is STEALING scan codes from
; our polling loop. Solution: mask IRQ1 so we get the raw scan codes.
; If ESC works: keyboard HW fine, IRQ1 handler works, event_get broken.

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

API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
API_WIN_END_DRAW        equ 32
API_APP_YIELD           equ 34

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

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    ; ---- Marker 0: entry ----
    MARKER 0

    ; ---- Create window ----
    mov bx, 55
    mov cx, 43
    mov dx, 210
    mov si, 114
    mov ax, cs
    mov es, ax
    mov di, win_title
    mov al, 0x03
    mov ah, API_WIN_CREATE
    int 0x80

    MARKER 1                        ; win_create returned

    jc .exit_no_win
    mov [wh], al

    ; ---- Mask IRQ1 (keyboard) so INT 9 handler doesn't steal scan codes ----
    ; This lets our direct port polling see the raw keyboard data.
    cli
    in al, 0x21
    or al, 0x02                     ; Set bit 1 = mask IRQ1
    out 0x21, al
    sti

    MARKER 2                        ; IRQ1 masked

    ; ---- Drain any residual scan codes ----
    in al, 0x64
    test al, 0x01
    jz .drained
    in al, 0x60                     ; Read and discard leftover
.drained:

    MARKER 3                        ; ready for key polling

    ; ---- Direct keyboard port polling (with IRQ1 masked) ----
.poll_loop:
    ; Check if keyboard output buffer has data
    in al, 0x64
    test al, 0x01                   ; Bit 0 = output buffer full
    jz .poll_loop                   ; No data, keep polling

    ; Read scan code
    in al, 0x60

    ; Skip key releases (bit 7 set)
    test al, 0x80
    jnz .poll_loop

    ; Key press detected!
    MARKER 4                        ; proves keyboard HW works

    ; Check for ESC (scan code 0x01)
    cmp al, 0x01
    je .got_esc

    ; Not ESC — write scan code to VRAM for diagnostic, keep polling
    push es
    push bx
    mov bx, 0xB800
    mov es, bx
    mov byte [es:0xA0], al          ; Row 2: raw scan code
    pop bx
    pop es
    jmp .poll_loop

.got_esc:
    MARKER 5                        ; ESC detected

    ; ---- Unmask IRQ1 before exiting ----
    cli
    in al, 0x21
    and al, 0xFD                    ; Clear bit 1 = unmask IRQ1
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

; Pad to force 2 FAT12 clusters (> 512 bytes)
times 600 - ($ - $$) db 0x90
