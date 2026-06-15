; ============================================================================
; UnoDOS/MacPlus Demo - DISK-LOADED app (DEMO.APP, proc 4, slot 8).
; A tiny sample app on the new fixed-org + address-table ABI (replacing the
; old a5-jump-table demo_app.asm). Shows text and counts SPACE presses; its
; counter lives in the app image and persists while the app stays resident.
; ABI: dc.l draw/key/tick/click header; links to the kernel by address.
; ============================================================================

        mc68000
        include "sysequ.i"
        include "build/kernel_api.inc"

        org     APP_LOAD+APPSLOT_DEMO*APP_SLOT_SZ
        dc.l    demo_draw           ; +0
        dc.l    demo_key            ; +4
        dc.l    demo_tickv          ; +8
        dc.l    demo_clickv         ; +12
demo_tickv:
        rts
demo_clickv:
        rts

demo_draw:
        movem.l d0-d7/a0-a2,-(sp)
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+4,d5
        lea     msg1(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        lea     msg2(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #14,d1
        moveq   #3,d2
        bsr     draw_string
        lea     msg3(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        add.w   #28,d1
        moveq   #3,d2
        bsr     draw_string
        ; one '*' per SPACE press
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
        move.w  d6,d0
        move.w  d5,d1
        add.w   #46,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d0-d7/a0-a2
        rts

demo_key:
        cmp.b   #' ',d1
        beq     .space
        moveq   #1,d0
        rts
.space:
        lea     count(pc),a0
        move.w  (a0),d0
        cmp.w   #20,d0
        bge     .wrap
        addq.w  #1,d0
        bra     .set
.wrap:  moveq   #0,d0
.set:   move.w  d0,(a0)
        moveq   #0,d0               ; consumed; kernel redraws topmost
        rts

        even
msg1:   dc.b    "Disk-loaded app!",0
msg2:   dc.b    "Read off the floppy via",0
msg3:   dc.b    "FAT12 + .Sony.  SPACE:",0
        even
count:  dc.w    0
        even
starbuf: ds.b   24
        even
        end
