; SYSINFO.BIN - System Information for UnoDOS
; Build 333: PIC mask diagnostic + force fix.
; Theory: multi-cluster floppy read (INT 13h) corrupts PIC masks,
; masking IRQ12 (PS/2 mouse) or IRQ2 (cascade to secondary PIC).
; Test: force-unmask IRQ12 + send EOI after win_create.

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

API_EVENT_GET           equ 9
API_WIN_CREATE          equ 20
API_WIN_DESTROY         equ 21
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

entry:
    pusha
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    ; ---- Marker 0: entry reached ----
    MARKER 0

    ; ---- Yield before win_create (proves scheduler works) ----
    mov ah, API_APP_YIELD
    int 0x80
    MARKER 1

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

    MARKER 2                        ; win_create returned

    jc .exit_no_win
    mov [wh], al

    ; ---- Marker 3: before PIC fix ----
    MARKER 3

    ; ==============================================================
    ; PIC DIAGNOSTIC + FORCE FIX
    ; If INT 13h (floppy read during app load) corrupted PIC masks,
    ; IRQ12 (PS/2 mouse) won't fire. Fix: unmask + EOI.
    ; ==============================================================
    cli                             ; Atomic PIC manipulation

    ; Send EOI to secondary PIC (clear any stuck ISR bit)
    mov al, 0x20
    out 0xA0, al

    ; Send EOI to primary PIC
    out 0x20, al

    ; Read secondary PIC mask and save to VRAM for diagnostic
    in al, 0xA1
    push es
    push bx
    mov bx, 0xB800
    mov es, bx
    mov byte [es:0xA0], al          ; Row 2, byte 0: raw secondary PIC mask
    pop bx
    pop es

    ; Force unmask IRQ12 (bit 4) on secondary PIC
    and al, 0xEF                    ; Clear bit 4
    out 0xA1, al

    ; Read primary PIC mask and save to VRAM
    in al, 0x21
    push es
    push bx
    mov bx, 0xB800
    mov es, bx
    mov byte [es:0xA2], al          ; Row 2, byte 1: raw primary PIC mask
    pop bx
    pop es

    ; Force unmask IRQ2 (cascade, bit 2) on primary PIC
    and al, 0xFB                    ; Clear bit 2
    out 0x21, al

    sti                             ; Re-enable interrupts

    ; ---- Marker 4: PIC fix applied ----
    MARKER 4

    ; Brief pause to let mouse IRQs flow
    mov ah, API_APP_YIELD
    int 0x80
    mov ah, API_APP_YIELD
    int 0x80

    ; ---- Marker 5: post-fix yields succeeded ----
    MARKER 5

    ; Main loop — if PIC fix worked, ESC should exit
.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .main_loop

    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    cmp dl, 27
    je .exit_ok
    jmp .main_loop

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
