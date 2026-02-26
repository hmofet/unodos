; SYSINFO.BIN - System Information for UnoDOS
; Build 336: Kernel event_get diagnostic.
; Kernel writes queue state to CGA VRAM row 8 when current_task != 0:
;   Row 8, byte 0 (0x140): event_queue_head
;   Row 8, byte 2 (0x142): event_queue_tail
;   Row 8, byte 4 (0x144): focused_task
;   Row 8, byte 6 (0x146): current_task
;   Row 8, byte 8 (0x148): 0xFF=queue has events, 0x00=empty
;   Row 8, byte 10 (0x14A): 0xFF=focus match, 0x00=mismatch
;
; App uses normal yield + event_get loop.
; Also has IRQ1-masked port polling fallback after 5000 iterations.
; Look at row 8: two white blocks = queue has events + focus match.

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

    MARKER 0                        ; entry

    ; Create window
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

    MARKER 2                        ; entering main loop

    ; Normal yield + event_get loop (with iteration counter)
    mov word [iter_count], 0

.main_loop:
    sti
    mov ah, API_APP_YIELD
    int 0x80

    mov ah, API_EVENT_GET
    int 0x80
    jc .no_event

    ; Got an event! Write marker 3
    MARKER 3

    cmp al, EVENT_KEY_PRESS
    jne .main_loop
    cmp dl, 27                      ; ESC?
    je .exit_ok
    jmp .main_loop

.no_event:
    ; Increment iteration counter
    inc word [cs:iter_count]

    ; Write low byte of counter to VRAM row 10 (offset 0x190 = 80*5)
    ; This proves the loop is spinning
    push es
    push bx
    mov bx, 0xB800
    mov es, bx
    mov al, [cs:iter_count]
    mov byte [es:0x190], al         ; Row 10, byte 0: counter low byte
    mov al, [cs:iter_count+1]
    mov byte [es:0x192], al         ; Row 10, byte 2: counter high byte
    pop bx
    pop es

    ; After 5000 iterations with no events, fall through to port polling
    cmp word [cs:iter_count], 5000
    jb .main_loop

    ; Timeout! Marker 4 = event_get never delivered an event
    MARKER 4

    ; Fall back to IRQ1-masked direct port polling (proven to work in Build 335)
    cli
    in al, 0x21
    or al, 0x02                     ; Mask IRQ1
    out 0x21, al
    sti

    MARKER 5                        ; fallback polling active

.fallback_loop:
    in al, 0x64
    test al, 0x01
    jz .fallback_loop
    in al, 0x60
    test al, 0x80
    jnz .fallback_loop
    cmp al, 0x01                    ; ESC?
    jne .fallback_loop

    MARKER 6                        ; ESC via fallback

    ; Unmask IRQ1
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

; Pad to force 2 FAT12 clusters (> 512 bytes)
times 600 - ($ - $$) db 0x90
