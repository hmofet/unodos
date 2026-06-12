; ============================================================================
; UnoDOS/MacPlus games: Dostris (proc 5). Logic ported verbatim from the
; Amiga port (amiga/games.i) - same piece tables, scoring, gravity curve -
; with two adaptations for the standalone Mac:
;   - rendering is 1-bit: piece colors map to the dither set (0=white ..
;     3=black), the board border is black (rect_outline_fg), text is black;
;   - game music is stubbed (gm_* are no-ops) until the M3 sound foundation
;     lands; the Korobeiniki note data still comes from gen_data.i so wiring
;     real Plus pulse-width sound later is a drop-in.
; OutLast + Pac-Man follow as further M3 ports.
; ============================================================================

DOSTRIS_PROC equ 5

; ---- game-music stubs (Plus pulse-width sound is a later M3 item) ----
gm_start:
        rts
gm_stop:
        rts
gm_tick:
        rts

; ---- continuous-tick glue -------------------------------------------------
; tick_wanted -> d0 != 0 when the topmost window is a game that needs the
; main loop to keep running (so gravity advances without input). Keeps the
; idle optimisation intact for everything else.
tick_wanted:
        move.w  zcount(pc),d0
        beq     .no
        movem.l d2/a2,-(sp)
        subq.w  #1,d0
        move.w  d0,d2
        bsr     zwin_ptr
        cmp.b   #DOSTRIS_PROC,WPROC(a2)
        bne     .pm
        move.w  dt_state(pc),d0
        cmp.w   #1,d0               ; 1 = playing
        bne     .no2
        bra     .yes
.pm:    cmp.b   #6,WPROC(a2)        ; PACMAN_PROC (fwd equ: literal)
        bne     .no2
        move.w  pm_state(pc),d0
        cmp.w   #1,d0               ; PMS_READY (auto-starts)
        beq     .yes
        cmp.w   #2,d0               ; PMS_PLAY
        bne     .no2
.yes:   moveq   #1,d0
        movem.l (sp)+,d2/a2
        rts
.no2:   movem.l (sp)+,d2/a2
.no:    moveq   #0,d0
        rts

; games_tick - per-pass tick dispatch from the main loop
games_tick:
        bsr     gm_tick
        bsr     dostris_tick
        bsr     pacman_tick
        rts

; ---------------------------------------------------------------- Dostris

DT_COLS     equ 10
DT_ROWS     equ 20
DT_CELL     equ 8

; dt_rand7 -> d0.w = 0..6 (LCG on dt_seed)
dt_rand7:
        movem.l d1-d2,-(sp)
        move.l  dt_seed(pc),d1
        move.l  d1,d2
        swap    d2
        mulu    #$4E6D,d1
        mulu    #$41C6,d2
        swap    d2
        clr.w   d2
        add.l   d2,d1
        add.l   #12345,d1
        lea     vars(pc),a4
        move.l  d1,dt_seed-vars(a4)
        swap    d1
        and.l   #$7FFF,d1
        divu    #7,d1
        swap    d1
        move.w  d1,d0
        movem.l (sp)+,d1-d2
        rts

; dt_fits - d0=piece d1=rot d2=col d3=row -> d0=1 fits / 0 collides
dt_fits:
        movem.l d1-d7/a0-a1,-(sp)
        lsl.w   #2,d0
        add.w   d1,d0
        lsl.w   #3,d0
        lea     dt_shapes(pc),a0
        lea     (a0,d0.w),a0
        lea     dt_board(pc),a1
        moveq   #0,d7
.cell:  move.b  (a0)+,d4
        ext.w   d4
        add.w   d2,d4
        move.b  (a0)+,d5
        ext.w   d5
        add.w   d3,d5
        tst.w   d4
        bmi     .no
        cmp.w   #DT_COLS,d4
        bge     .no
        cmp.w   #DT_ROWS,d5
        bge     .no
        tst.w   d5
        bmi     .next
        move.w  d5,d6
        mulu    #DT_COLS,d6
        add.w   d4,d6
        tst.b   (a1,d6.w)
        bne     .no
.next:  addq.w  #1,d7
        cmp.w   #4,d7
        blt     .cell
        moveq   #1,d0
        movem.l (sp)+,d1-d7/a0-a1
        rts
.no:    moveq   #0,d0
        movem.l (sp)+,d1-d7/a0-a1
        rts

; dt_interval -> d0.w = gravity interval in ticks
dt_interval:
        move.w  #18,d0
        sub.w   dt_level(pc),d0
        cmp.w   #2,d0
        bge     .ok
        moveq   #2,d0
.ok:    mulu    #3,d0
        rts

; dt_spawn - next becomes current; game over if it can't drop
dt_spawn:
        movem.l d0-d3,-(sp)
        lea     vars(pc),a4
        move.w  dt_next(pc),d0
        move.w  d0,dt_piece-vars(a4)
        bsr     dt_rand7
        move.w  d0,dt_next-vars(a4)
        clr.w   dt_rot-vars(a4)
        move.w  #3,dt_col-vars(a4)
        move.w  #-1,dt_row-vars(a4)
        move.l  ticks(pc),d0
        move.l  d0,dt_last-vars(a4)
        move.w  dt_piece(pc),d0
        moveq   #0,d1
        moveq   #3,d2
        moveq   #0,d3
        bsr     dt_fits
        tst.w   d0
        bne     .ok
        move.w  #3,dt_state-vars(a4)
        bsr     gm_stop
.ok:    movem.l (sp)+,d0-d3
        rts

; dt_clear_lines - collapse full rows, score them
dt_clear_lines:
        movem.l d0-d7/a0-a1,-(sp)
        lea     dt_board(pc),a0
        moveq   #0,d7
        moveq   #0,d5
.row:   moveq   #0,d6
.col:   move.w  d5,d0
        mulu    #DT_COLS,d0
        add.w   d6,d0
        tst.b   (a0,d0.w)
        beq     .notfull
        addq.w  #1,d6
        cmp.w   #DT_COLS,d6
        blt     .col
        addq.w  #1,d7
        move.w  d5,d1
.shift: tst.w   d1
        beq     .top
        move.w  d1,d0
        mulu    #DT_COLS,d0
        lea     (a0,d0.w),a1
        moveq   #DT_COLS-1,d2
.cp:    move.b  -DT_COLS(a1),(a1)+
        dbra    d2,.cp
        subq.w  #1,d1
        bra     .shift
.top:   moveq   #DT_COLS-1,d2
.z:     clr.b   (a0,d2.w)
        dbra    d2,.z
.notfull:
        addq.w  #1,d5
        cmp.w   #DT_ROWS,d5
        blt     .row
        tst.w   d7
        beq     .done
        lea     vars(pc),a4
        move.w  d7,d0
        add.w   d0,d0
        lea     dt_linescore(pc),a1
        move.w  (a1,d0.w),d0
        move.w  dt_level(pc),d1
        addq.w  #1,d1
        mulu    d1,d0
        add.w   dt_score(pc),d0
        move.w  d0,dt_score-vars(a4)
        move.w  dt_lines(pc),d0
        add.w   d7,d0
        move.w  d0,dt_lines-vars(a4)
        and.l   #$FFFF,d0
        divu    #10,d0
        addq.w  #1,d0
        cmp.w   #15,d0
        ble     .lvok
        moveq   #15,d0
.lvok:  move.w  d0,dt_level-vars(a4)
.done:  movem.l (sp)+,d0-d7/a0-a1
        rts

; dt_lock - stamp the current piece into the board, clear, respawn
dt_lock:
        movem.l d0-d7/a0-a1,-(sp)
        move.w  dt_piece(pc),d0
        move.w  d0,d6
        lsl.w   #2,d0
        add.w   dt_rot(pc),d0
        lsl.w   #3,d0
        lea     dt_shapes(pc),a0
        lea     (a0,d0.w),a0
        lea     dt_board(pc),a1
        lea     dt_colors(pc),a4
        moveq   #0,d5
        move.b  (a4,d6.w),d5
        addq.w  #1,d5               ; stored as color+1
        moveq   #0,d7
.cell:  move.b  (a0)+,d1
        ext.w   d1
        add.w   dt_col(pc),d1
        move.b  (a0)+,d2
        ext.w   d2
        add.w   dt_row(pc),d2
        bmi     .next
        cmp.w   #DT_ROWS,d2
        bge     .next
        mulu    #DT_COLS,d2
        add.w   d1,d2
        move.b  d5,(a1,d2.w)
.next:  addq.w  #1,d7
        cmp.w   #4,d7
        blt     .cell
        bsr     dt_clear_lines
        bsr     dt_spawn
        movem.l (sp)+,d0-d7/a0-a1
        rts

; dt_step - gravity: move down or lock
dt_step:
        movem.l d0-d3,-(sp)
        move.w  dt_piece(pc),d0
        move.w  dt_rot(pc),d1
        move.w  dt_col(pc),d2
        move.w  dt_row(pc),d3
        addq.w  #1,d3
        bsr     dt_fits
        tst.w   d0
        beq     .lock
        lea     vars(pc),a4
        move.w  dt_row(pc),d0
        addq.w  #1,d0
        move.w  d0,dt_row-vars(a4)
        bra     .out
.lock:  bsr     dt_lock
.out:   movem.l (sp)+,d0-d3
        rts

; dt_cellrect - d0=col d1=row d4=color, a2=window: draw one board cell
dt_cellrect:
        movem.l d0-d3,-(sp)
        mulu    #DT_CELL,d0
        add.w   WX(a2),d0
        addq.w  #6,d0
        mulu    #DT_CELL,d1
        add.w   WY(a2),d1
        add.w   #TBAR_H+6,d1
        moveq   #DT_CELL-1,d2
        moveq   #DT_CELL-1,d3
        bsr     fill_rect
        movem.l (sp)+,d0-d3
        rts

; dostris_draw - a2 = window
dostris_draw:
        movem.l d0-d7/a0-a4,-(sp)
        ; clear content (white)
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
        ; board border (black)
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+4,d1
        move.w  #DT_COLS*DT_CELL+4,d2
        move.w  #DT_ROWS*DT_CELL+4,d3
        bsr     rect_outline_fg
        ; settled cells
        lea     dt_board(pc),a3
        moveq   #0,d6
.brow:  moveq   #0,d5
.bcol:  move.w  d6,d0
        mulu    #DT_COLS,d0
        add.w   d5,d0
        moveq   #0,d4
        move.b  (a3,d0.w),d4
        beq     .empty
        subq.w  #1,d4
        move.w  d5,d0
        move.w  d6,d1
        bsr     dt_cellrect
.empty: addq.w  #1,d5
        cmp.w   #DT_COLS,d5
        blt     .bcol
        addq.w  #1,d6
        cmp.w   #DT_ROWS,d6
        blt     .brow
        ; falling piece (playing or paused)
        move.w  dt_state(pc),d0
        subq.w  #1,d0
        cmp.w   #1,d0
        bhi     .nopiece
        move.w  dt_piece(pc),d0
        move.w  d0,d7
        lsl.w   #2,d0
        add.w   dt_rot(pc),d0
        lsl.w   #3,d0
        lea     dt_shapes(pc),a0
        lea     (a0,d0.w),a0
        lea     dt_colors(pc),a1
        moveq   #0,d4
        move.b  (a1,d7.w),d4
        moveq   #0,d6
.pcell: move.b  (a0)+,d0
        ext.w   d0
        add.w   dt_col(pc),d0
        move.b  (a0)+,d1
        ext.w   d1
        add.w   dt_row(pc),d1
        bmi     .pskip
        bsr     dt_cellrect
.pskip: addq.w  #1,d6
        cmp.w   #4,d6
        blt     .pcell
.nopiece:
        ; panel
        move.w  WX(a2),d6
        add.w   #100,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+6,d5
        lea     str_dt_title(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        add.w   #16,d5
        lea     str_dt_score(pc),a0
        bsr     dt_label
        move.w  dt_score(pc),d0
        bsr     dt_value
        add.w   #12,d5
        lea     str_dt_lines(pc),a0
        bsr     dt_label
        move.w  dt_lines(pc),d0
        bsr     dt_value
        add.w   #12,d5
        lea     str_dt_level(pc),a0
        bsr     dt_label
        move.w  dt_level(pc),d0
        bsr     dt_value
        ; next preview
        add.w   #18,d5
        lea     str_dt_next(pc),a0
        bsr     dt_label
        add.w   #12,d5
        move.w  dt_next(pc),d7
        move.w  d7,d0
        lsl.w   #5,d0
        lea     dt_shapes(pc),a0
        lea     (a0,d0.w),a0
        lea     dt_colors(pc),a1
        moveq   #0,d4
        move.b  (a1,d7.w),d4
        moveq   #0,d3
.nx:    moveq   #0,d0
        move.b  (a0)+,d0
        mulu    #DT_CELL,d0
        add.w   d6,d0
        moveq   #0,d1
        move.b  (a0)+,d1
        mulu    #DT_CELL,d1
        add.w   d5,d1
        movem.l d2-d4,-(sp)
        moveq   #DT_CELL-1,d2
        move.w  d2,d3
        bsr     fill_rect
        movem.l (sp)+,d2-d4
        addq.w  #1,d3
        cmp.w   #4,d3
        blt     .nx
        ; help + state
        add.w   #28,d5
        lea     str_dt_help1(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #2,d2
        bsr     draw_string
        add.w   #10,d5
        lea     str_dt_help2(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        moveq   #2,d2
        bsr     draw_string
        add.w   #14,d5
        move.w  dt_state(pc),d0
        beq     .smenu
        cmp.w   #2,d0
        beq     .spause
        cmp.w   #3,d0
        beq     .sover
        bra     .sdone
.smenu: lea     str_dt_newg(pc),a0
        bra     .stext
.spause:
        lea     str_dt_paused(pc),a0
        bra     .stext
.sover: lea     str_dt_over(pc),a0
.stext: move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
.sdone: movem.l (sp)+,d0-d7/a0-a4
        rts

; dt_label - a0=string at (d6,d5) black. Preserves d5/d6.
dt_label:
        movem.l d0-d2/a0,-(sp)
        move.w  d6,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d0-d2/a0
        rts

; dt_value - d0.w printed at (d6+56,d5) black. Preserves d5/d6.
dt_value:
        movem.l d0-d2/a0,-(sp)
        lea     npstat(pc),a0
        bsr     fmt_dec
        move.w  d6,d0
        add.w   #56,d0
        move.w  d5,d1
        moveq   #3,d2
        bsr     draw_string
        movem.l (sp)+,d0-d2/a0
        rts

; dostris_new - reset and start
dostris_new:
        movem.l d0-d1/a0,-(sp)
        lea     vars(pc),a4
        lea     dt_board(pc),a0
        move.w  #DT_ROWS*DT_COLS-1,d0
.clr:   clr.b   (a0)+
        dbra    d0,.clr
        clr.w   dt_score-vars(a4)
        clr.w   dt_lines-vars(a4)
        move.w  #1,dt_level-vars(a4)
        move.l  ticks(pc),d0
        bset    #0,d0
        move.l  d0,dt_seed-vars(a4)
        bsr     dt_rand7
        move.w  d0,dt_next-vars(a4)
        move.w  #1,dt_state-vars(a4)
        bsr     dt_spawn
        lea     koro_notes(pc),a0
        move.w  koro_count(pc),d0
        moveq   #DOSTRIS_PROC,d1
        bsr     gm_start
        movem.l (sp)+,d0-d1/a0
        rts

; dostris_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
dostris_key:
        lea     vars(pc),a4
        cmp.b   #'n',d1
        beq     .new
        cmp.b   #'p',d1
        beq     .pause
        move.w  dt_state(pc),d0
        cmp.w   #1,d0
        bne     .nope
        cmp.b   #$4F,d2             ; left
        beq     .left
        cmp.b   #$4E,d2             ; right
        beq     .right
        cmp.b   #$4C,d2             ; up = rotate
        beq     .rot
        cmp.b   #$4D,d2             ; down = soft drop
        beq     .soft
        cmp.b   #32,d1             ; space = hard drop
        beq     .hard
.nope:  moveq   #1,d0
        rts
.new:   bsr     dostris_new
        bra     .redraw
.pause: move.w  dt_state(pc),d0
        cmp.w   #1,d0
        beq     .topause
        cmp.w   #2,d0
        bne     .redraw
        move.w  #1,dt_state-vars(a4)
        move.l  ticks(pc),d0
        move.l  d0,dt_last-vars(a4)
        lea     koro_notes(pc),a0
        move.w  koro_count(pc),d0
        moveq   #DOSTRIS_PROC,d1
        bsr     gm_start
        bra     .redraw
.topause:
        move.w  #2,dt_state-vars(a4)
        bsr     gm_stop
        bra     .redraw
.left:  move.w  dt_piece(pc),d0
        move.w  dt_rot(pc),d1
        move.w  dt_col(pc),d2
        subq.w  #1,d2
        move.w  dt_row(pc),d3
        bsr     dt_fits
        tst.w   d0
        beq     .redraw
        subq.w  #1,dt_col-vars(a4)
        bra     .redraw
.right: move.w  dt_piece(pc),d0
        move.w  dt_rot(pc),d1
        move.w  dt_col(pc),d2
        addq.w  #1,d2
        move.w  dt_row(pc),d3
        bsr     dt_fits
        tst.w   d0
        beq     .redraw
        addq.w  #1,dt_col-vars(a4)
        bra     .redraw
.rot:   move.w  dt_piece(pc),d0
        move.w  dt_rot(pc),d1
        addq.w  #1,d1
        and.w   #3,d1
        move.w  dt_col(pc),d2
        move.w  dt_row(pc),d3
        bsr     dt_fits
        tst.w   d0
        beq     .redraw
        move.w  dt_rot(pc),d0
        addq.w  #1,d0
        and.w   #3,d0
        move.w  d0,dt_rot-vars(a4)
        bra     .redraw
.soft:  move.w  dt_piece(pc),d0
        move.w  dt_rot(pc),d1
        move.w  dt_col(pc),d2
        move.w  dt_row(pc),d3
        addq.w  #1,d3
        bsr     dt_fits
        tst.w   d0
        beq     .softlock
        addq.w  #1,dt_row-vars(a4)
        addq.w  #1,dt_score-vars(a4)
        move.l  ticks(pc),d0
        move.l  d0,dt_last-vars(a4)
        bra     .redraw
.softlock:
        bsr     dt_lock
        bra     .redraw
.hard:  move.w  dt_piece(pc),d0
        move.w  dt_rot(pc),d1
        move.w  dt_col(pc),d2
        move.w  dt_row(pc),d3
.hloop: addq.w  #1,d3
        bsr     dt_fits
        tst.w   d0
        beq     .hdone
        addq.w  #1,dt_row-vars(a4)
        addq.w  #2,dt_score-vars(a4)
        move.w  dt_row(pc),d3
        move.w  dt_piece(pc),d0
        move.w  dt_rot(pc),d1
        move.w  dt_col(pc),d2
        bra     .hloop
.hdone: bsr     dt_lock
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; dostris_tick - gravity from the main loop (topmost + playing only)
dostris_tick:
        movem.l d0-d2/a2/a4,-(sp)
        move.w  dt_state(pc),d0
        cmp.w   #1,d0
        bne     .out
        move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #DOSTRIS_PROC,WPROC(a2)
        bne     .out
        move.l  ticks(pc),d1
        sub.l   dt_last(pc),d1
        bsr     dt_interval
        ext.l   d0
        cmp.l   d0,d1
        blt     .out
        lea     vars(pc),a4
        move.l  ticks(pc),d0
        move.l  d0,dt_last-vars(a4)
        bsr     dt_step
        bsr     redraw_topmost
.out:   movem.l (sp)+,d0-d2/a2/a4
        rts

; ---------------------------------------------------------------- data
        even
dt_shapes:
        dc.b    0,1,1,1,2,1,3,1,  2,0,2,1,2,2,2,3,  0,2,1,2,2,2,3,2,  1,0,1,1,1,2,1,3
        dc.b    1,0,2,0,1,1,2,1,  1,0,2,0,1,1,2,1,  1,0,2,0,1,1,2,1,  1,0,2,0,1,1,2,1
        dc.b    1,0,0,1,1,1,2,1,  0,0,0,1,1,1,0,2,  0,0,1,0,2,0,1,1,  1,0,0,1,1,1,1,2
        dc.b    1,0,2,0,0,1,1,1,  0,0,0,1,1,1,1,2,  1,0,2,0,0,1,1,1,  0,0,0,1,1,1,1,2
        dc.b    0,0,1,0,1,1,2,1,  1,0,0,1,1,1,0,2,  0,0,1,0,1,1,2,1,  1,0,0,1,1,1,0,2
        dc.b    0,0,0,1,1,1,2,1,  0,0,1,0,0,1,0,2,  0,0,1,0,2,0,2,1,  1,0,1,1,0,2,1,2
        dc.b    2,0,0,1,1,1,2,1,  0,0,0,1,0,2,1,2,  0,0,1,0,2,0,0,1,  0,0,1,0,1,1,1,2
; 1-bit dither per piece (alternating black / mid-gray so adjacent pieces
; stay distinguishable on a monochrome screen)
dt_colors:
        dc.b    3,2,3,2,3,2,3
        even
dt_linescore:
        dc.w    0,40,100,300,1200

str_dt_title:   dc.b    "DOSTRIS",0
str_dt_score:   dc.b    "Score",0
str_dt_lines:   dc.b    "Lines",0
str_dt_level:   dc.b    "Level",0
str_dt_next:    dc.b    "Next",0
str_dt_help1:   dc.b    "Arrows: move/rot",0
str_dt_help2:   dc.b    "Spc:drop P:pause",0
str_dt_newg:    dc.b    "N: new game",0
str_dt_paused:  dc.b    "PAUSED",0
str_dt_over:    dc.b    "GAME OVER",0
str_t_dostris:  dc.b    "Dostris",0
name_dostris:   dc.b    "Dostris",0
        even
