; Minimal WonderSwan boot test — proves the Unicorn x86-16 reset path + the
; harness tile renderer (shade pool, palettes, tile patterns, the SCR1 tilemap).
; Builds a 64 KB ROM; the V30MZ reset vector 0xFFFF0 holds the JMP FAR to start.
bits 16
cpu 186
org 0x0000                      ; ROM maps to segment 0xF000 (top 64 KB of the 1 MB space)

start:
    cli
    xor ax, ax
    mov ds, ax                  ; DS/ES/SS = 0 -> the 16 KB internal RAM
    mov es, ax
    mov ss, ax
    mov sp, 0x2000

    ; shade pool (ports 0x1C-0x1F): 8 greys, pool0=white(0) .. pool7=black(15)
    mov al, 0x20                 ; pool1=2,pool0=0
    out 0x1C, al
    mov al, 0x64                 ; pool3=6,pool2=4
    out 0x1D, al
    mov al, 0xB9                 ; pool5=11,pool4=9
    out 0x1E, al
    mov al, 0xFD                 ; pool7=15,pool6=13
    out 0x1F, al

    ; palette 0 = normal: color0=pool0 (white bg), color1=pool7 (black fg)
    mov al, 0x70
    out 0x20, al
    xor al, al
    out 0x21, al
    ; palette 1 = inverted: color0=pool7 (black), color1=pool0 (white)
    mov al, 0x07
    out 0x22, al
    xor al, al
    out 0x23, al

    ; tile 1 = solid (every pixel colour 1): plane0=0xFF, plane1=0x00 per row
    mov di, 0x2000 + 16
    mov cx, 8
.t1:
    mov byte [di], 0xFF
    mov byte [di + 1], 0x00
    add di, 2
    loop .t1

    ; SCR1 map base = 0x0800  (port 0x07 low nibble = 1 -> 1<<11)
    mov al, 0x01
    out 0x07, al

    ; fill the 32x32 tilemap: vertical stripes of blank(tile0)/solid(tile1), palette0
    mov di, 0x0800
    mov cx, 32 * 32
    xor ax, ax
.fill:
    mov [di], ax
    add di, 2
    xor ax, 0x0001              ; toggle tile 0 <-> 1
    loop .fill

    ; scroll 0,0 ; display on (SCR1 enable)
    xor al, al
    out 0x10, al
    out 0x11, al
    mov al, 0x01
    out 0x00, al
.hang:
    jmp .hang

    times 0xFFF0 - ($ - $$) db 0
header:
    db 0xEA                      ; JMP FAR
    dw start                     ; offset
    dw 0xF000                    ; segment
    db 0x00                      ; maintenance
    db 0x00                      ; publisher
    db 0x00                      ; mono
    db 0x00                      ; game id
    db 0x00                      ; version
    db 0x02                      ; ROM size code
    db 0x00                      ; save type
    db 0x04                      ; flags: horizontal, 16-bit bus
    db 0x00                      ; mapper
    dw 0x0000                    ; checksum (patched by build)
