; UnoDOS Kernel
; Loaded at 0x1000:0000 (linear 0x10000 = 64KB)
; Main operating system code

[BITS 16]
[ORG 0x0000]

; ============================================================================
; Signature and Entry Point
; ============================================================================

signature:
    dw 0x4B55                       ; 'UK' signature for kernel verification

entry:
    ; Set up segment registers for our location
    mov ax, 0x1000
    mov ds, ax
    mov es, ax

    ; Set up graphics mode (blue screen)
    call setup_graphics

    ; Halt - just show blue screen
halt_loop:
    hlt
    jmp halt_loop

; ============================================================================
; Graphics Setup - Blue Screen Only
; ============================================================================

setup_graphics:
    ; Set CGA 320x200 4-color mode (mode 4)
    mov ah, 0x00
    mov al, 0x04                    ; 320x200, 4 colors
    int 0x10

    ; Clear all CGA video memory explicitly
    ; Mode 4 uses 0xB800:0000-0x3FFF (16KB)
    push es
    mov ax, 0xB800
    mov es, ax
    xor di, di
    xor ax, ax                      ; Fill with 0 (background color)
    mov cx, 8192                    ; 16KB / 2 = 8192 words
    cld
    rep stosw
    pop es

    ; Select color palette (cyan/magenta/white)
    mov ah, 0x0B
    mov bh, 0x01                    ; Palette select
    mov bl, 0x01                    ; Palette 1 (cyan, magenta, white)
    int 0x10

    ; Set background/border color to blue
    mov ah, 0x0B
    mov bh, 0x00                    ; Background color
    mov bl, 0x01                    ; Blue background
    int 0x10

    ret

; ============================================================================
; Padding
; ============================================================================

; Pad to 16KB (32 sectors)
times 16384 - ($ - $$) db 0
