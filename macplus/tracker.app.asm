; ============================================================================
; UnoDOS/MacPlus Tracker - DISK-LOADED app (TRACKER.APP, proc 10, slot 6).
; 32-row pattern editor UI; port of tracker.i. The playback sequencer
; (tracker_tick, tk_trigger_row, tk_cell, tk_periods) and tk_stop live
; kernel-side (audio, exported) so playback continues when the window is not
; topmost; this app holds the editor draw/key + note-name formatting + the
; demo song. Pattern data (tk_pat) and cursor state (tk_*) are kernel vars.
; ABI: JMP draw/key/tick/click.
; ============================================================================

        mc68000
        include "sysequ.i"
        include "build/kernel_api.inc"

TK_ROWS     equ 32
TK_CHANS    equ 4
TK_VIEW     equ 12
TRACKER_PROC equ 10

        org     APP_LOAD+APPSLOT_TRACKER*APP_SLOT_SZ
        dc.l     tracker_draw        ; +0
        dc.l     tracker_key         ; +4
        dc.l     tracker_tickv       ; +8  (audio runs kernel-side)
        dc.l     tracker_clickv      ; +12
tracker_tickv:
        rts
tracker_clickv:
        rts

tk_notenames:                       ; 2 chars per semitone
        dc.b    "C-C#D-D#E-F-F#G-G#A-A#B-"
tk_instname:
        dc.b    "SQ","SW","TR","NZ"
        even

; tk_fmt_cell - a0=cell -> writes 5 chars ("C#2 1" / "--- -") at a1, advances
tk_fmt_cell:
        movem.l d0-d2/a0/a2,-(sp)
        moveq   #0,d0
        move.b  (a0),d0
        bne     .note
        move.b  #'-',(a1)+
        move.b  #'-',(a1)+
        move.b  #'-',(a1)+
        move.b  #' ',(a1)+
        move.b  #'-',(a1)+
        bra     .done
.note:  subq.w  #1,d0
        move.w  d0,d1
        and.l   #$FFFF,d1
        divu    #12,d1
        move.w  d1,d2
        swap    d1
        add.w   d1,d1
        lea     tk_notenames(pc),a2
        move.b  (a2,d1.w),(a1)+
        move.b  1(a2,d1.w),(a1)+
        add.w   #'2',d2
        move.b  d2,(a1)+
        move.b  #' ',(a1)+
        moveq   #0,d0
        move.b  1(a0),d0
        add.w   d0,d0
        lea     tk_instname(pc),a2
        move.b  (a2,d0.w),(a1)+
.done:  movem.l (sp)+,d0-d2/a0/a2
        rts

tracker_draw:
        movem.l d0-d7/a0-a4,-(sp)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        sub.w   #TBAR_H+1,d3
        moveq   #0,d4
        bsr     fill_rect
        move.w  WX(a2),d6
        addq.w  #6,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+2,d5
        lea     str_tk_hdr(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        move.w  tk_ch(pc),d0
        mulu    #56,d0
        add.w   d6,d0
        add.w   #24,d0
        move.w  d5,d1
        lea     str_tk_mark(pc),a0
        moveq   #3,d2
        bsr     draw_string
        lea     vars(pc),a4
        move.w  tk_row(pc),d0
        move.w  tk_top(pc),d1
        cmp.w   d1,d0
        bge     .t1
        move.w  d0,d1
.t1:    move.w  d1,d2
        add.w   #TK_VIEW,d2
        cmp.w   d2,d0
        blt     .t2
        move.w  d0,d1
        sub.w   #TK_VIEW-1,d1
.t2:    move.w  d1,tk_top-vars(a4)
        add.w   #12,d5
        move.w  tk_top(pc),d7
.row:   lea     npstat(pc),a1
        moveq   #0,d0
        move.w  d7,d0
        divu    #10,d0
        add.b   #'0',d0
        move.b  d0,(a1)+
        swap    d0
        add.b   #'0',d0
        move.b  d0,(a1)+
        move.b  #' ',(a1)+
        move.b  #' ',(a1)+
        moveq   #0,d1
.cell:  move.w  d7,d0
        bsr     tk_cell             ; kernel
        bsr     tk_fmt_cell
        move.b  #' ',(a1)+
        move.b  #' ',(a1)+
        addq.w  #1,d1
        cmp.w   #TK_CHANS,d1
        blt     .cell
        clr.b   (a1)
        moveq   #3,d2
        moveq   #0,d3
        cmp.w   tk_row(pc),d7
        bne     .ncur
        moveq   #0,d2
        moveq   #3,d3
        bra     .draw
.ncur:  move.b  tk_playing(pc),d0
        beq     .draw
        cmp.w   tk_prow(pc),d7
        bne     .draw
        moveq   #3,d2
        moveq   #1,d3
.draw:  lea     npstat(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        bsr     draw_string_bg
        add.w   #10,d5
        addq.w  #1,d7
        move.w  tk_top(pc),d0
        add.w   #TK_VIEW,d0
        cmp.w   d0,d7
        blt     .row
        addq.w  #4,d5
        lea     str_tk_foot1(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        add.w   #10,d5
        lea     str_tk_foot2(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d0-d7/a0-a4
        rts

tracker_key:
        lea     vars(pc),a4
        cmp.b   #$4C,d2
        beq     .up
        cmp.b   #$4D,d2
        beq     .down
        cmp.b   #$4F,d2
        beq     .left
        cmp.b   #$4E,d2
        beq     .right
        cmp.b   #'q',d1
        beq     .ndown
        cmp.b   #'w',d1
        beq     .nup
        cmp.b   #'e',d1
        beq     .instr
        cmp.b   #'x',d1
        beq     .clear
        cmp.b   #'d',d1
        beq     .demo
        cmp.b   #'s',d1
        beq     .save
        cmp.b   #'l',d1
        beq     .load
        cmp.b   #32,d1
        beq     .playstop
        moveq   #1,d0
        rts
.save:  bsr     tk_mount
        move.b  fat_mounted(pc),d0
        beq     .redraw
        lea     str_songname(pc),a0
        lea     tk_pat,a1
        move.l  #TK_ROWS*TK_CHANS*2,d1
        bsr     fat_save_file
        bsr     fat_list_root
        bra     .redraw
.load:  bsr     tk_mount
        move.b  fat_mounted(pc),d0
        beq     .redraw
        lea     str_songname(pc),a0
        bsr     fat_find_file
        tst.w   d0
        bmi     .redraw
        move.l  #TK_ROWS*TK_CHANS*2,d1
        lea     tk_pat,a1
        bsr     fat_read_file
        bra     .redraw
.up:    move.w  tk_row(pc),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,tk_row-vars(a4)
        bra     .redraw
.down:  move.w  tk_row(pc),d0
        cmp.w   #TK_ROWS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,tk_row-vars(a4)
        bra     .redraw
.left:  move.w  tk_ch(pc),d0
        beq     .redraw
        subq.w  #1,d0
        move.w  d0,tk_ch-vars(a4)
        bra     .redraw
.right: move.w  tk_ch(pc),d0
        cmp.w   #TK_CHANS-1,d0
        bge     .redraw
        addq.w  #1,d0
        move.w  d0,tk_ch-vars(a4)
        bra     .redraw
.ndown: bsr     tk_curcell
        move.b  (a0),d0
        beq     .startc
        cmp.b   #1,d0
        ble     .redraw
        subq.b  #1,(a0)
        bra     .preview
.nup:   bsr     tk_curcell
        move.b  (a0),d0
        beq     .startc
        cmp.b   #24,d0
        bge     .redraw
        addq.b  #1,(a0)
        bra     .preview
.startc:
        move.b  #1,(a0)
        bra     .preview
.instr: bsr     tk_curcell
        tst.b   (a0)
        beq     .redraw
        move.b  1(a0),d0
        addq.b  #1,d0
        and.b   #3,d0
        move.b  d0,1(a0)
        bra     .preview
.clear: bsr     tk_curcell
        clr.w   (a0)
        bra     .redraw
.demo:  bsr     tk_load_demo
        bra     .redraw
.playstop:
        move.b  tk_playing(pc),d0
        beq     .start
        bsr     tk_stop             ; kernel
        bra     .redraw
.start: st      tk_playing-vars(a4)
        move.w  #TK_ROWS-1,tk_prow-vars(a4)
        move.l  ticks(pc),d0
        subq.l  #7,d0
        move.l  d0,tk_last-vars(a4)
        bra     .redraw
.preview:
        move.w  tk_row(pc),d0
        bsr     tk_trigger_row      ; kernel
.redraw:
        moveq   #0,d0               ; consumed; kernel redraws topmost
        rts

tk_mount:
        move.b  fat_mounted(pc),d0
        bne     .ok
        bsr     files_mount
.ok:    rts

tk_curcell:
        movem.l d0-d1,-(sp)
        move.w  tk_row(pc),d0
        move.w  tk_ch(pc),d1
        bsr     tk_cell             ; kernel
        movem.l (sp)+,d0-d1
        rts

tk_load_demo:
        movem.l d0/a0-a1,-(sp)
        lea     tk_demo(pc),a0
        lea     tk_pat,a1
        move.w  #TK_ROWS*TK_CHANS*2/4-1,d0
.cp:    move.l  (a0)+,(a1)+
        dbra    d0,.cp
        movem.l (sp)+,d0/a0-a1
        rts

; ---------------------------------------------------------------- data
str_tk_hdr:     dc.b    "Row Chan1  Chan2  Chan3  Chan4",0
str_tk_mark:    dc.b    "^",0
str_tk_foot1:   dc.b    "q/w:note e:inst x:clr d:demo s/l:disk",0
str_tk_foot2:   dc.b    "Space: play/stop   arrows: move",0
        even
tk_demo:
        dc.b    1,1,  13,0,  0,0,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  17,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    1,1,  20,0,  0,0,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  17,0, 13,2,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  13,0, 17,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  15,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  20,0, 15,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  15,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    6,1,  10,0, 13,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  13,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    6,1,  17,0,  0,0,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  13,0, 10,2,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  11,0, 15,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  15,0,  0,0,   0,0
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    8,1,  20,0, 19,2,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        dc.b    0,0,  23,0,  0,0,  20,3
        dc.b    0,0,   0,0,  0,0,   0,0
        even
str_songname:   dc.b    "SONG    UNO"
        even
        end
