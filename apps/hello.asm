; HELLO.BIN - Simple test application for UnoDOS v3.11.0
; Proves application loader works by drawing a visible pattern
;
; Build: nasm -f bin -o hello.bin hello.asm
; Copy to FAT12 floppy as HELLO.BIN (8.3 format)

[BITS 16]
[ORG 0x0000]

; Entry point - called by kernel via far CALL
entry:
    push ds
    push es
    push di
    push cx

    ; Write "HI" pattern directly to CGA video memory
    ; CGA mode 04h: 320x200, 4 colors, memory at B800:0000
    ; Each byte contains 4 pixels (2 bits per pixel)
    ; We'll draw at y=80 (middle-ish of screen)

    mov ax, 0xB800
    mov es, ax

    ; In CGA mode 04h:
    ; Even scanlines: offset 0x0000 - 0x1F3F
    ; Odd scanlines: offset 0x2000 - 0x3F3F
    ; Each scanline = 80 bytes (320 pixels / 4 pixels per byte)
    ;
    ; Y=80 is even, so offset = (80/2) * 80 = 40 * 80 = 3200 = 0x0C80
    ; Let's draw at X=150 (middle of screen)
    ; X=150 means byte offset = 150/4 = 37, bit position = (150%4)*2 = 0

    ; Draw a simple 8x8 'H' pattern at (150, 80)
    ; Using color 3 (white) = binary 11

    ; Row 0: |*  * | = 11 00 11 00 = 0xCC (pixels at 0,1 and 4,5)
    mov di, 0x0C80 + 37             ; Y=80, X=148 (byte 37)
    mov byte [es:di], 0xCC
    mov byte [es:di+1], 0xCC

    ; Row 1: |*  * |
    mov di, 0x2000 + 0x0C80 + 37    ; Y=81 (odd)
    mov byte [es:di], 0xCC
    mov byte [es:di+1], 0xCC

    ; Row 2: |*  * |
    mov di, 0x0C80 + 80 + 37        ; Y=82
    mov byte [es:di], 0xCC
    mov byte [es:di+1], 0xCC

    ; Row 3: |****|
    mov di, 0x2000 + 0x0C80 + 80 + 37  ; Y=83
    mov byte [es:di], 0xFF
    mov byte [es:di+1], 0xFF

    ; Row 4: |****|
    mov di, 0x0C80 + 160 + 37       ; Y=84
    mov byte [es:di], 0xFF
    mov byte [es:di+1], 0xFF

    ; Row 5: |*  * |
    mov di, 0x2000 + 0x0C80 + 160 + 37  ; Y=85
    mov byte [es:di], 0xCC
    mov byte [es:di+1], 0xCC

    ; Row 6: |*  * |
    mov di, 0x0C80 + 240 + 37       ; Y=86
    mov byte [es:di], 0xCC
    mov byte [es:di+1], 0xCC

    ; Row 7: |*  * |
    mov di, 0x2000 + 0x0C80 + 240 + 37  ; Y=87
    mov byte [es:di], 0xCC
    mov byte [es:di+1], 0xCC

    pop cx
    pop di
    pop es
    pop ds

    ; Return success (AX = 0)
    xor ax, ax
    retf                            ; Far return to kernel
