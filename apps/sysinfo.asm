; SYSINFO.BIN - System Information for UnoDOS
; Build 334: Direct keyboard port polling diagnostic.
; Build 333 showed PIC fix didn't help — both KB and mouse dead.
; This test bypasses INT 0x80 event system entirely.
; Polls keyboard controller port 0x64/0x60 directly.
; If ESC exits: keyboard HW works but event chain is broken.
; If ESC doesn't exit: keyboard controller itself is dead.

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

    ; ---- PIC fix (from Build 333) ----
    cli
    mov al, 0x20
    out 0xA0, al                    ; EOI to secondary PIC
    out 0x20, al                    ; EOI to primary PIC
    in al, 0xA1
    and al, 0xEF                    ; Unmask IRQ12
    out 0xA1, al
    in al, 0x21
    and al, 0xFB                    ; Unmask IRQ2 cascade
    out 0x21, al
    sti

    MARKER 2                        ; PIC fix done

    ; ---- Reinstall INT 9 vector (in case IVT was corrupted) ----
    ; The kernel's INT 9 handler is at 0x1000:int_09_handler
    ; We can't easily get the correct offset from the app, so skip this.
    ; Instead, we test direct port polling.

    ; ---- Direct keyboard port polling loop ----
    ; Bypass ALL interrupt/event infrastructure.
    ; Read keyboard controller status port 0x64 and data port 0x60.
    MARKER 3                        ; entering poll loop

.poll_loop:
    sti                             ; Keep interrupts enabled

    ; Check if keyboard output buffer has data
    in al, 0x64
    test al, 0x01                   ; Bit 0 = output buffer full
    jz .no_key

    ; Read scan code
    in al, 0x60

    ; Skip key releases (bit 7 set)
    test al, 0x80
    jnz .no_key

    ; Any key press: write marker 4 (proves keyboard HW works)
    MARKER 4

    ; Check for ESC (scan code 0x01)
    cmp al, 0x01
    je .got_esc

    ; Not ESC — write scan code to VRAM row 2 for diagnostic
    push es
    push bx
    mov bx, 0xB800
    mov es, bx
    mov byte [es:0xA0], al          ; Row 2, byte 0: raw scan code
    pop bx
    pop es
    jmp .poll_loop

.no_key:
    jmp .poll_loop

.got_esc:
    ; ---- Marker 5: ESC detected via direct port polling ----
    MARKER 5

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
