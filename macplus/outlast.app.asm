; ============================================================================
; UnoDOS/MacPlus OutLast - DISK-LOADED app (OUTLAST.APP, proc 7, slot 3).
; Pseudo-3D racer; logic ported verbatim from games.i. State (ol_*) in kernel
; vars; game music kernel-side (gm_*). ABI: JMP draw/key/tick/click.
; ============================================================================

        mc68000
        include "sysequ.i"
        include "build/kernel_api.inc"

OL_HORIZ    equ 36
OL_PLAYW    equ 296
OL_PLAYH    equ 150
OL_SEGLEN   equ 80
OL_TRACK    equ 2560
OUTLAST_PROC equ 7

        org     APP_LOAD+APPSLOT_OUTLAST*APP_SLOT_SZ
        dc.l     outlast_draw        ; +0
        dc.l     outlast_key         ; +4
        dc.l     outlast_tick        ; +8
        dc.l     outlast_clickv      ; +12
outlast_clickv:
        rts

; outlast_new
outlast_new:
        movem.l d0/a4,-(sp)
        lea     vars(pc),a4
        move.w  #148,ol_x-vars(a4)
        clr.w   ol_speed-vars(a4)
        clr.l   ol_z-vars(a4)
        clr.w   ol_score-vars(a4)
        move.w  #60,ol_time-vars(a4)
        clr.w   ol_crash-vars(a4)
        move.l  #400,ol_traf0-vars(a4)
        move.l  #1600,ol_traf1-vars(a4)
        move.l  #800,ol_traf2-vars(a4)
        move.l  #2000,ol_traf3-vars(a4)
        move.l  ticks(pc),d0
        move.l  d0,ol_last-vars(a4)
        move.l  d0,ol_lastsec-vars(a4)
        move.w  #1,ol_state-vars(a4)
        movem.l d0-d1/a0-a1,-(sp)
        lea     drive_notes(pc),a0
        move.w  drive_count(pc),d0
        moveq   #OUTLAST_PROC,d1
        bsr     gm_start
        movem.l (sp)+,d0-d1/a0-a1
        movem.l (sp)+,d0/a4
        rts

; ol_rect - playfield rect: d0=x d1=y d2=x2 d3=y2 d4=col, a2=window
ol_rect:
        movem.l d0-d3,-(sp)
        tst.w   d0
        bge     .x0ok
        moveq   #0,d0
.x0ok:  cmp.w   #OL_PLAYW,d2
        ble     .x1ok
        move.w  #OL_PLAYW,d2
.x1ok:  cmp.w   d0,d2
        ble     .skip
        cmp.w   d1,d3
        ble     .skip
        sub.w   d0,d2
        sub.w   d1,d3
        add.w   WX(a2),d0
        addq.w  #4,d0
        add.w   WY(a2),d1
        add.w   #TBAR_H+12,d1
        bsr     fill_rect
.skip:  movem.l (sp)+,d0-d3
        rts

; outlast_draw - a2 = window
outlast_draw:
        movem.l d0-d7/a0-a4,-(sp)
        move.w  ol_state(pc),d0
        bne     .ingame
        moveq   #0,d0
        moveq   #0,d1
        move.w  #OL_PLAYW,d2
        move.w  #OL_PLAYH,d3
        moveq   #3,d4
        bsr     ol_rect
        moveq   #0,d0
        moveq   #60,d1
        move.w  #OL_PLAYW,d2
        moveq   #62,d3
        moveq   #1,d4
        bsr     ol_rect
        moveq   #0,d7
.band:  move.w  d7,d1
        mulu    #9,d1
        add.w   #62,d1
        move.w  #148,d0
        move.w  d7,d2
        mulu    #13,d2
        addq.w  #6,d2
        sub.w   d2,d0
        move.w  #148,d4
        add.w   d2,d4
        move.w  d4,d2
        move.w  d1,d3
        add.w   #9,d3
        moveq   #2,d4
        bsr     ol_rect
        addq.w  #1,d7
        cmp.w   #9,d7
        blt     .band
        move.w  #130,d0
        move.w  #108,d1
        move.w  #166,d2
        move.w  #132,d3
        moveq   #0,d4
        bsr     ol_rect
        move.w  #136,d0
        move.w  #112,d1
        move.w  #160,d2
        move.w  #120,d3
        moveq   #3,d4
        bsr     ol_rect
        lea     str_ol_title(pc),a0
        move.w  WX(a2),d0
        add.w   #100,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+30,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        lea     str_ol_prompt(pc),a0
        move.w  WX(a2),d0
        add.w   #92,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+150,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        bra     .done
.ingame:
        moveq   #0,d0
        moveq   #0,d1
        move.w  #OL_PLAYW,d2
        move.w  #OL_HORIZ,d3
        moveq   #0,d4
        bsr     ol_rect
        moveq   #0,d0
        move.w  #OL_HORIZ,d1
        move.w  #OL_PLAYW,d2
        move.w  #OL_HORIZ+2,d3
        moveq   #3,d4
        bsr     ol_rect
        move.w  #OL_PLAYH-2,d7
        moveq   #0,d6
.strip: move.w  d7,d0
        sub.w   #OL_HORIZ,d0
        move.w  #3000,d1
        ext.l   d1
        divu    d0,d1
        and.l   #$FFFF,d1
        move.l  d1,d2
        lsl.l   #2,d2
        add.l   ol_z(pc),d2
        divu    #OL_TRACK,d2
        swap    d2
        and.l   #$FFFF,d2
        divu    #OL_SEGLEN,d2
        and.w   #31,d2
        lea     ol_curve(pc),a0
        move.b  (a0,d2.w),d3
        ext.w   d3
        add.w   d3,d6
        move.w  d6,d3
        asr.w   #5,d3
        add.w   #148,d3
        move.l  #3584,d4
        divu    d1,d4
        and.l   #$FFFF,d4
        moveq   #2,d5
        btst    #0,d2
        beq     .gok
        moveq   #1,d5
.gok:   movem.w d2/d6,-(sp)
        moveq   #0,d0
        move.w  d7,d1
        move.w  d3,d2
        sub.w   d4,d2
        move.w  d7,d6
        addq.w  #2,d6
        movem.w d3-d5,-(sp)
        move.w  d6,d3
        move.w  d5,d4
        bsr     ol_rect
        movem.w (sp)+,d3-d5
        move.w  d3,d0
        sub.w   d4,d0
        move.w  d7,d1
        move.w  d3,d2
        add.w   d4,d2
        movem.w d3-d5,-(sp)
        move.w  d7,d3
        addq.w  #2,d3
        moveq   #0,d4
        bsr     ol_rect
        movem.w (sp)+,d3-d5
        move.w  d3,d0
        add.w   d4,d0
        move.w  d7,d1
        move.w  #OL_PLAYW,d2
        movem.w d3-d5,-(sp)
        move.w  d7,d3
        addq.w  #2,d3
        move.w  d5,d4
        bsr     ol_rect
        movem.w (sp)+,d3-d5
        movem.w (sp)+,d2/d6
        btst    #0,d2
        beq     .nostripe
        move.w  d3,d0
        subq.w  #1,d0
        move.w  d7,d1
        move.w  d3,d2
        addq.w  #1,d2
        movem.w d3-d4,-(sp)
        move.w  d7,d3
        addq.w  #2,d3
        moveq   #3,d4
        bsr     ol_rect
        movem.w (sp)+,d3-d4
.nostripe:
        cmp.w   #OL_PLAYH-2,d7
        bne     .noedge
        lea     vars(pc),a4
        move.w  d3,d0
        sub.w   d4,d0
        move.w  d0,ol_roadl-vars(a4)
        move.w  d3,d0
        add.w   d4,d0
        move.w  d0,ol_roadr-vars(a4)
.noedge:
        subq.w  #2,d7
        cmp.w   #OL_HORIZ+2,d7
        bgt     .strip
        moveq   #0,d7
.traf:  move.w  d7,d0
        lsl.w   #2,d0
        lea     ol_traf0(pc),a0
        move.l  (a0,d0.w),d1
        move.l  ol_z(pc),d2
        divu    #OL_TRACK,d2
        swap    d2
        and.l   #$FFFF,d2
        sub.l   d2,d1
        bpl     .relok
        add.l   #OL_TRACK,d1
.relok: cmp.l   #20,d1
        blt     .ntraf
        cmp.l   #400,d1
        bgt     .ntraf
        move.l  #1500,d3
        divu    d1,d3
        and.w   #$FF,d3
        cmp.w   #2,d3
        bge     .h1
        moveq   #2,d3
.h1:    cmp.w   #36,d3
        ble     .h2
        moveq   #36,d3
.h2:    move.l  #2100,d4
        divu    d1,d4
        and.w   #$FF,d4
        cmp.w   #3,d4
        bge     .w1
        moveq   #3,d4
.w1:    cmp.w   #44,d4
        ble     .w2
        moveq   #44,d4
.w2:    move.l  #12000,d5
        divu    d1,d5
        and.l   #$FFFF,d5
        add.w   #OL_HORIZ,d5
        cmp.w   #OL_PLAYH-4,d5
        ble     .yok
        move.w  #OL_PLAYH-4,d5
.yok:   move.l  d1,d0
        lsr.l   #4,d0
        moveq   #26,d6
        sub.w   d0,d6
        cmp.w   #6,d6
        bge     .lok
        moveq   #6,d6
.lok:   btst    #0,d7
        bne     .right
        neg.w   d6
.right: add.w   #148,d6
        move.w  d6,d0
        move.w  d4,d2
        lsr.w   #1,d2
        sub.w   d2,d0
        move.w  d6,d2
        move.w  d4,d1
        lsr.w   #1,d1
        add.w   d1,d2
        move.w  d5,d1
        sub.w   d3,d1
        move.w  d5,d3
        moveq   #3,d4
        cmp.w   #2,d7
        blt     .col
        moveq   #2,d4
.col:   bsr     ol_rect
.ntraf: addq.w  #1,d7
        cmp.w   #4,d7
        blt     .traf
        move.w  ol_crash(pc),d0
        btst    #2,d0
        bne     .nocar
        move.w  ol_x(pc),d6
        move.w  d6,d0
        sub.w   #13,d0
        move.w  #118,d1
        move.w  d6,d2
        add.w   #13,d2
        move.w  #134,d3
        moveq   #3,d4
        bsr     ol_rect
        move.w  d6,d0
        subq.w  #8,d0
        move.w  #121,d1
        move.w  d6,d2
        addq.w  #8,d2
        move.w  #126,d3
        moveq   #0,d4
        bsr     ol_rect
        move.w  d6,d0
        sub.w   #15,d0
        move.w  #131,d1
        move.w  d6,d2
        sub.w   #9,d2
        move.w  #137,d3
        moveq   #3,d4
        bsr     ol_rect
        move.w  d6,d0
        add.w   #9,d0
        move.w  #131,d1
        move.w  d6,d2
        add.w   #15,d2
        move.w  #137,d3
        moveq   #3,d4
        bsr     ol_rect
.nocar:
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+1,d1
        move.w  #OL_PLAYW,d2
        moveq   #10,d3
        moveq   #3,d4
        bsr     fill_rect
        lea     npstat(pc),a0
        lea     str_ol_speed(pc),a1
        bsr     str_append
        move.w  ol_speed(pc),d0
        bsr     fmt_dec
.s1:    tst.b   (a0)+
        bne     .s1
        subq.l  #1,a0
        lea     str_ol_score(pc),a1
        bsr     str_append
        move.w  ol_score(pc),d0
        bsr     fmt_dec
.s2:    tst.b   (a0)+
        bne     .s2
        subq.l  #1,a0
        lea     str_ol_time(pc),a1
        bsr     str_append
        move.w  ol_time(pc),d0
        bsr     fmt_dec
        lea     npstat(pc),a0
        move.w  WX(a2),d0
        addq.w  #8,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+2,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        move.w  ol_state(pc),d0
        cmp.w   #2,d0
        bne     .done
        move.w  #70,d0
        move.w  #56,d1
        move.w  #226,d2
        move.w  #100,d3
        moveq   #3,d4
        bsr     ol_rect
        lea     str_ol_over(pc),a0
        move.w  WX(a2),d0
        add.w   #112,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+82,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        lea     str_ol_prompt(pc),a0
        move.w  WX(a2),d0
        add.w   #96,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+98,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
.done:  movem.l (sp)+,d0-d7/a0-a4
        rts

; outlast_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
outlast_key:
        lea     vars(pc),a4
        cmp.b   #'n',d1
        beq     .new
        move.w  ol_state(pc),d0
        cmp.w   #1,d0
        bne     .nope
        move.w  ol_crash(pc),d0
        bne     .eat
        cmp.b   #$4F,d2
        beq     .left
        cmp.b   #$4E,d2
        beq     .right
        cmp.b   #$4C,d2
        beq     .accel
        cmp.b   #$4D,d2
        beq     .brake
.nope:  moveq   #1,d0
        rts
.eat:   moveq   #0,d0
        rts
.new:   bsr     outlast_new
        moveq   #0,d0
        rts
.left:  move.w  ol_x(pc),d0
        subq.w  #8,d0
        cmp.w   #20,d0
        bge     .lset
        moveq   #20,d0
.lset:  move.w  d0,ol_x-vars(a4)
        moveq   #0,d0
        rts
.right: move.w  ol_x(pc),d0
        addq.w  #8,d0
        cmp.w   #276,d0
        ble     .rset
        move.w  #276,d0
.rset:  move.w  d0,ol_x-vars(a4)
        moveq   #0,d0
        rts
.accel: move.w  ol_speed(pc),d0
        addq.w  #4,d0
        cmp.w   #60,d0
        ble     .aset
        moveq   #60,d0
.aset:  move.w  d0,ol_speed-vars(a4)
        moveq   #0,d0
        rts
.brake: move.w  ol_speed(pc),d0
        subq.w  #8,d0
        bge     .bset
        moveq   #0,d0
.bset:  move.w  d0,ol_speed-vars(a4)
        moveq   #0,d0
        rts

; outlast_tick - physics step + redraw (topmost + playing, every 8 ticks)
outlast_tick:
        movem.l d0-d7/a0/a2/a4,-(sp)
        move.w  ol_state(pc),d0
        cmp.w   #1,d0
        bne     .out
        move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #OUTLAST_PROC,WPROC(a2)
        bne     .out
        move.l  ticks(pc),d0
        sub.l   ol_last(pc),d0
        cmp.l   #8,d0
        blt     .out
        lea     vars(pc),a4
        move.l  ticks(pc),d0
        move.l  d0,ol_last-vars(a4)
        move.w  ol_crash(pc),d0
        beq     .alive
        subq.w  #1,d0
        move.w  d0,ol_crash-vars(a4)
        bne     .timer
        move.w  #148,ol_x-vars(a4)
        move.w  #5,ol_speed-vars(a4)
        bra     .timer
.alive:
        move.w  ol_speed(pc),d0
        cmp.w   #60,d0
        bge     .spdok
        addq.w  #1,d0
        move.w  d0,ol_speed-vars(a4)
.spdok:
        move.w  ol_x(pc),d1
        cmp.w   ol_roadl(pc),d1
        blt     .grass
        cmp.w   ol_roadr(pc),d1
        ble     .ongrassok
.grass: move.w  ol_speed(pc),d0
        subq.w  #2,d0
        cmp.w   #5,d0
        bge     .gset
        moveq   #5,d0
.gset:  move.w  d0,ol_speed-vars(a4)
.ongrassok:
        move.l  ol_z(pc),d0
        divu    #OL_TRACK,d0
        swap    d0
        and.l   #$FFFF,d0
        divu    #OL_SEGLEN,d0
        and.w   #31,d0
        lea     ol_curve(pc),a0
        move.b  (a0,d0.w),d1
        ext.w   d1
        asr.w   #3,d1
        move.w  ol_x(pc),d0
        sub.w   d1,d0
        cmp.w   #20,d0
        bge     .dx1
        moveq   #20,d0
.dx1:   cmp.w   #276,d0
        ble     .dx2
        move.w  #276,d0
.dx2:   move.w  d0,ol_x-vars(a4)
        moveq   #0,d0
        move.w  ol_speed(pc),d0
        add.l   d0,ol_z-vars(a4)
        lsr.w   #2,d0
        add.w   ol_score(pc),d0
        move.w  d0,ol_score-vars(a4)
        moveq   #0,d7
.traf:  move.w  d7,d0
        lsl.w   #2,d0
        lea     ol_traf0(pc),a0
        lea     (a0,d0.w),a0
        move.l  (a0),d1
        cmp.w   #2,d7
        blt     .same
        subq.l  #5,d1
        bra     .wrap
.same:  addq.l  #5,d1
.wrap:  tst.l   d1
        bpl     .w2t
        add.l   #OL_TRACK,d1
.w2t:   cmp.l   #OL_TRACK,d1
        blt     .w3
        sub.l   #OL_TRACK,d1
.w3:    move.l  d1,(a0)
        move.l  ol_z(pc),d2
        divu    #OL_TRACK,d2
        swap    d2
        and.l   #$FFFF,d2
        sub.l   d2,d1
        bpl     .crel
        add.l   #OL_TRACK,d1
.crel:  cmp.l   #15,d1
        bge     .ntraf
        move.w  #148,d2
        btst    #0,d7
        beq     .lleft
        add.w   #26,d2
        bra     .lck
.lleft: sub.w   #26,d2
.lck:   move.w  ol_x(pc),d3
        sub.w   d2,d3
        bpl     .abs
        neg.w   d3
.abs:   cmp.w   #25,d3
        bge     .ntraf
        move.w  #30,ol_crash-vars(a4)
        clr.w   ol_speed-vars(a4)
.ntraf: addq.w  #1,d7
        cmp.w   #4,d7
        blt     .traf
.timer:
        move.l  ticks(pc),d0
        sub.l   ol_lastsec(pc),d0
        cmp.l   #TICKS_SEC,d0
        blt     .draw
        move.l  ticks(pc),d0
        move.l  d0,ol_lastsec-vars(a4)
        move.w  ol_time(pc),d0
        subq.w  #1,d0
        bge     .tset
        moveq   #0,d0
.tset:  move.w  d0,ol_time-vars(a4)
        bne     .draw
        move.w  #2,ol_state-vars(a4)
        bsr     gm_stop
.draw:  bsr     redraw_topmost
.out:   movem.l (sp)+,d0-d7/a0/a2/a4
        rts

; OutLast 32-segment curve table
ol_curve:
        dc.b    0,0,0,0,0,0,0,0
        dc.b    5,15,25,30, 25,15,5,0
        dc.b    0,0,0,0,0,0,0,0
        dc.b    -5,-15,-25,-30, -25,-15,-5,0
        even

str_ol_title:   dc.b    "O U T L A S T",0
str_ol_prompt:  dc.b    "N: drive",0
str_ol_speed:   dc.b    "Speed ",0
str_ol_score:   dc.b    "  Score ",0
str_ol_time:    dc.b    "  Time ",0
str_ol_over:    dc.b    "GAME OVER",0
        even
        end
