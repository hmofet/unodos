; ============================================================================
; UnoDOS/Genesis - boot proof-of-concept (milestone 0)
;
; Boots on a Sega Mega Drive / Genesis (TMSS-safe), initializes the VDP,
; loads the shared UnoDOS 8x8 font as 4bpp tiles, and shows the platform
; splash: "UnoDOS 3 / for Sega Genesis - Mega Drive" on the UnoDOS desktop
; blue, plus a PS/2 port banner reflecting the planned input wiring.
;
; Input plan (per the community precedent - HardWareMan on SpritesMind,
; PS/2 keyboard on joystick port 2 via the #EXT interrupt):
;   port 2 DB9: PS/2 KEYBOARD  CLK -> pin 7 (TH, EXT/HL interrupt capable)
;                              DATA -> pin 1 (D0/Up), +5V pin 5, GND pin 8
;   port 1 DB9: PS/2 MOUSE     CLK -> pin 7 (TH, polled w/ host inhibit)
;                              DATA -> pin 1 (D0/Up), +5V pin 5, GND pin 8
;
; Build: vasmm68k_mot -Fbin -o unodos.gen boot.asm   (see build.sh)
; ============================================================================

VDP_DATA    equ $C00000
VDP_CTRL    equ $C00004
Z80_BUSREQ  equ $A11100
Z80_RESET   equ $A11200
HW_VERSION  equ $A10001
TMSS_PORT   equ $A14000

PLANE_A     equ $C000               ; name table A
SCRW_TILES  equ 64                  ; plane width (tiles)

; ---------------------------------------------------------------- vectors
        org     0
        dc.l    $00FFFE00           ; initial SSP (top of work RAM)
        dc.l    start               ; reset PC
        dc.l    err,err,err,err,err,err     ; bus, address, illegal, div0,
        dc.l    err,err                     ; chk, trapv, priv, trace
        dc.l    err,err,err,err             ; line-A, line-F, rsvd...
        dc.l    err,err,err,err,err,err,err,err
        dc.l    err,err,err                 ; spurious, lvl1, lvl2(EXT)
        dc.l    err                         ; lvl3
        dc.l    hblank                      ; lvl4 = hblank
        dc.l    err                         ; lvl5
        dc.l    vblank                      ; lvl6 = vblank
        dc.l    err                         ; lvl7
        dc.l    err,err,err,err,err,err,err,err   ; traps 0-7
        dc.l    err,err,err,err,err,err,err,err   ; traps 8-15
        dcb.l   16,err              ; remaining reserved vectors

; ---------------------------------------------------------------- header
        org     $100
        dc.b    "SEGA MEGA DRIVE "                  ; console name (16)
        dc.b    "(C)UNOD 2026.JUN"                  ; copyright (16)
        dc.b    "UNODOS 3                                        " ; domestic 48
        dc.b    "UNODOS 3                                        " ; overseas 48
        dc.b    "GM UNODOS3-00"                     ; serial (14)
        dc.b    0
        dc.w    0                                   ; checksum (unused)
        dc.b    "J               "                  ; device support (16)
        dc.l    $00000000                           ; ROM start
        dc.l    $0003FFFF                           ; ROM end (256KB)
        dc.l    $00FF0000                           ; RAM start
        dc.l    $00FFFFFF                           ; RAM end
        dc.b    "            "                      ; no SRAM (12)
        dc.b    "            "                      ; modem (12)
        dc.b    "UnoDOS 3 for Sega Genesis - boot PoC    "  ; notes (40)
        dc.b    "JUE             "                  ; region (16)

; ---------------------------------------------------------------- code
        org     $200
start:
        move.w  #$2700,sr           ; interrupts off, supervisor

        ; TMSS: later consoles hold the VDP hostage until 'SEGA' is written
        move.b  HW_VERSION,d0
        and.b   #$0F,d0
        beq     .notmss
        move.l  #'SEGA',TMSS_PORT
.notmss:

        ; quiet the Z80 (bus request + reset held)
        move.w  #$0100,Z80_BUSREQ
        move.w  #$0100,Z80_RESET

        ; ---- VDP register setup
        lea     VDP_CTRL,a0
        move.w  #$8004,(a0)         ; r0: no HL int, normal
        move.w  #$8144,(a0)         ; r1: display ON, vblank int off (PoC), V28
        move.w  #$8230,(a0)         ; r2: plane A name table = $C000
        move.w  #$8330,(a0)         ; r3: window = off ($C000 unused)
        move.w  #$8407,(a0)         ; r4: plane B name table = $E000
        move.w  #$8578,(a0)         ; r5: sprite table = $F000
        move.w  #$8700,(a0)         ; r7: background = palette 0 color 0
        move.w  #$8A00,(a0)         ; r10: hint counter
        move.w  #$8B00,(a0)         ; r11: full scroll
        move.w  #$8C00,(a0)         ; r12: H32 (256px - cosy PoC)
        move.w  #$8D3F,(a0)         ; r13: hscroll table = $FC00
        move.w  #$8F02,(a0)         ; r15: auto-increment 2
        move.w  #$9001,(a0)         ; r16: scroll size 64x32
        move.w  #$9100,(a0)         ; r17: window x
        move.w  #$9200,(a0)         ; r18: window y

        ; ---- palette: UnoDOS desktop colors (BGR, 3 bits/channel)
        move.l  #$C0000000,(a0)     ; CRAM write, address 0
        lea     VDP_DATA,a1
        move.w  #$0A00,(a1)         ; 0: UnoDOS blue   (#0000AA)
        move.w  #$0EEE,(a1)         ; 1: white         (#EEEEEE)
        move.w  #$0AA0,(a1)         ; 2: cyan          (#00AAAA)
        move.w  #$0A0A,(a1)         ; 3: magenta       (#AA00AA)

        ; ---- clear VRAM (name tables + tiles)
        move.l  #$40000000,(a0)     ; VRAM write, address 0
        move.w  #$7FFF,d0
        moveq   #0,d1
.clr:   move.w  d1,(a1)
        dbra    d0,.clr

        ; ---- load the font: tile index 1.. = ASCII 32.. (tile 0 stays blank)
        move.l  #$40200000,(a0)     ; VRAM write, address $0020 (tile 1)
        lea     font_tiles(pc),a2
        move.w  #(95*8)-1,d0
.font:  move.l  (a2)+,(a1)
        dbra    d0,.font

        ; ---- splash text
        lea     str_title(pc),a2
        moveq   #9,d0               ; x (tiles)
        moveq   #10,d1              ; y
        bsr     print
        lea     str_sub(pc),a2
        moveq   #2,d0
        moveq   #13,d1
        bsr     print
        lea     str_kbd(pc),a2
        moveq   #2,d0
        moveq   #20,d1
        bsr     print
        lea     str_mse(pc),a2
        moveq   #2,d0
        moveq   #22,d1
        bsr     print

        ; ---- PoC idle loop
.idle:  bra     .idle

; print - a2 = NUL string, d0 = x tile, d1 = y tile (plane A, palette 0)
print:
        movem.l d0-d2/a0-a1,-(sp)
        lea     VDP_CTRL,a0
        lea     VDP_DATA,a1
        ; VRAM address = PLANE_A + (y*64 + x) * 2
        lsl.w   #6,d1
        add.w   d0,d1
        add.w   d1,d1
        add.w   #PLANE_A,d1
        ; build the control word: CD=VRAM write, A13-A0 / A15-A14
        moveq   #0,d0
        move.w  d1,d0
        and.w   #$3FFF,d0
        or.w    #$4000,d0
        swap    d0
        move.w  d1,d2
        rol.w   #2,d2
        and.w   #3,d2
        move.w  d2,d0
        move.l  d0,(a0)
.ch:    moveq   #0,d2
        move.b  (a2)+,d2
        beq     .done
        sub.w   #31,d2              ; tile 1 = ASCII 32
        move.w  d2,(a1)
        bra     .ch
.done:  movem.l (sp)+,d0-d2/a0-a1
        rts

err:    bra     err
hblank: rte
vblank: rte

str_title:  dc.b    "U n o D O S   3",0
str_sub:    dc.b    "for Sega Genesis - Mega Drive",0
str_kbd:    dc.b    "PS/2 kbd: port 2 TH=CLK D0=DAT",0
str_mse:    dc.b    "PS/2 mse: port 1 TH=CLK D0=DAT",0
        even

        include "gen_font.i"

; pad the ROM to 32KB
        org     $7FFF
        dc.b    0
