; ============================================================================
; UnoDOS/MacPlus disk-loaded demo app (DEMO.APP).
;
; A standalone 68K binary that lives on the floppy's FAT12 volume, NOT in the
; kernel. The kernel reads it into APP_LOAD ($40000) and calls our entry
; (image offset 0) for each window event:
;   d0 = 0  -> draw   (a2 = window record, a5 = ksys jump table)
;   d0 = 1  -> key    (d1 = ascii, d2 = raw,  a2 = window, a5 = ksys table)
; We return d0 = 0 (consumed) / 1 (not) for key events.
;
; The code is position-independent: every self reference is PC-relative and
; every kernel service goes through the a5 table, so we hold no absolute
; kernel addresses. App state (the star counter) lives in our own image and
; persists between calls because the kernel keeps us resident at APP_LOAD.
;
; Build: vasm -Fbin -m68000 -o build/demo.bin demo_app.asm ; mkfs adds DEMO.APP
; ============================================================================

; window record field offsets (must match the kernel's WENT layout)
WX          equ 2
WY          equ 4
WW          equ 6
WH          equ 8
TBAR_H      equ 10

; ksys_table offsets (longs into the a5 table)
K_DRAWSTR   equ 0
K_DRAWSTRBG equ 4
K_FILLRECT  equ 8
K_OUTLINE   equ 12
K_FINDFILE  equ 16
K_READFILE  equ 20
K_TICKS     equ 24

        mc68000
        org     0

; ---------------------------------------------------------------- dispatch
entry:
        lea     ksysp(pc),a0
        move.l  a5,(a0)             ; stash the service table (PIC reload base)
        lea     winp(pc),a0
        move.l  a2,(a0)             ; stash the window record
        tst.w   d0
        bne     do_key

; ---------------------------------------------------------------- draw
do_draw:
        move.l  winp(pc),a2
        move.w  WX(a2),d6
        addq.w  #6,d6               ; content x  (d6 survives kernel calls)
        move.w  WY(a2),d5
        add.w   #TBAR_H+4,d5        ; content y  (d5 survives kernel calls)
        lea     msg1(pc),a0
        moveq   #0,d3
        bsr     draw_line
        lea     msg2(pc),a0
        moveq   #14,d3
        bsr     draw_line
        lea     msg3(pc),a0
        moveq   #24,d3
        bsr     draw_line
        lea     msg4(pc),a0
        moveq   #40,d3
        bsr     draw_line
        ; star row: one '*' per SPACE press
        lea     starbuf(pc),a1
        move.w  count(pc),d0
        moveq   #0,d1
.sl:    cmp.w   d0,d1
        bge     .se
        move.b  #'*',(a1)+
        addq.w  #1,d1
        bra     .sl
.se:    clr.b   (a1)
        lea     starbuf(pc),a0
        moveq   #50,d3
        bsr     draw_line
        rts

; draw_line - a0 = string, d3 = y offset from content top. Keeps d5/d6.
draw_line:
        movem.l d0-d6/a0-a2,-(sp)
        move.w  d6,d0
        move.w  d5,d1
        add.w   d3,d1
        moveq   #3,d2              ; black
        move.l  ksysp(pc),a5
        move.l  K_DRAWSTR(a5),a6
        jsr     (a6)
        movem.l (sp)+,d0-d6/a0-a2
        rts

; ---------------------------------------------------------------- key
do_key:
        cmp.b   #' ',d1
        beq     .space
        moveq   #1,d0               ; not ours
        rts
.space:
        lea     count(pc),a0
        move.w  (a0),d0
        cmp.w   #20,d0
        bge     .wrap
        addq.w  #1,d0
        bra     .set
.wrap:  moveq   #0,d0               ; wrap the counter
.set:   move.w  d0,(a0)
        moveq   #0,d0               ; consumed (kernel redraws)
        rts

; ---------------------------------------------------------------- data
        even
msg1:   dc.b    "Disk-loaded app!",0
msg2:   dc.b    "Read off the floppy",0
msg3:   dc.b    "via FAT12 + .Sony.",0
msg4:   dc.b    "SPACE adds a star:",0
        even
ksysp:  dc.l    0                   ; saved ksys table base
winp:   dc.l    0                   ; saved window record
count:  dc.w    0                   ; star counter (persists while resident)
        even
starbuf: ds.b   24
        end
