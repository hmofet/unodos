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

; (gm_start/gm_stop/gm_tick are the real sequencer in snd.i now)

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
        bne     .ol
        move.w  pm_state(pc),d0
        cmp.w   #1,d0               ; PMS_READY (auto-starts)
        beq     .yes
        cmp.w   #2,d0               ; PMS_PLAY
        bne     .no2
        bra     .yes
.ol:    cmp.b   #7,WPROC(a2)        ; OUTLAST_PROC
        bne     .no2
        move.w  ol_state(pc),d0
        cmp.w   #1,d0               ; driving (incl. crash recovery)
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
        bsr     outlast_tick
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
        lea     dt_board,a1
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
        lea     dt_board,a0
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
        lea     dt_board,a1
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
        lea     dt_board,a3
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
        lea     dt_board,a0
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

; ---------------------------------------------------------------- OutLast
; Port of the amiga/games.i racer (proc 7 there AND here). Same track
; table, physics, traffic and crash rules; 50 Hz time constants -> 60 Hz
; (1-second countdown = TICKS_SEC). 1-bit scheme: white sky + road, black
; horizon line, grass parity = medium/light dither (the rush-by effect),
; black center stripe + cars (oncoming = 50% dither), white windshield.

OL_HORIZ    equ 36                  ; horizon offset inside the playfield
OL_PLAYW    equ 296
OL_PLAYH    equ 150
OL_SEGLEN   equ 80
OL_TRACK    equ 2560
OUTLAST_PROC equ 7

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
; (coords relative to the playfield origin; clamps x to [0,OL_PLAYW])
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
        sub.w   d0,d2               ; w
        sub.w   d1,d3               ; h
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
        ; ---- title screen (black backdrop, white car)
        moveq   #0,d0
        moveq   #0,d1
        move.w  #OL_PLAYW,d2
        move.w  #OL_PLAYH,d3
        moveq   #3,d4               ; black
        bsr     ol_rect
        moveq   #0,d0
        moveq   #60,d1
        move.w  #OL_PLAYW,d2
        moveq   #62,d3
        moveq   #1,d4               ; horizon haze: light dither
        bsr     ol_rect
        ; converging road bands
        moveq   #0,d7
.band:  move.w  d7,d1
        mulu    #9,d1
        add.w   #62,d1
        move.w  #148,d0
        move.w  d7,d2
        mulu    #13,d2
        addq.w  #6,d2
        sub.w   d2,d0               ; x1
        move.w  #148,d4
        add.w   d2,d4
        move.w  d4,d2               ; x2
        move.w  d1,d3
        add.w   #9,d3
        moveq   #2,d4               ; road bands: medium dither
        bsr     ol_rect
        addq.w  #1,d7
        cmp.w   #9,d7
        blt     .band
        ; car silhouette (white on black)
        move.w  #130,d0
        move.w  #108,d1
        move.w  #166,d2
        move.w  #132,d3
        moveq   #0,d4               ; body: white
        bsr     ol_rect
        move.w  #136,d0
        move.w  #112,d1
        move.w  #160,d2
        move.w  #120,d3
        moveq   #3,d4               ; windshield: black
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
        ; ---- sky + horizon
        moveq   #0,d0
        moveq   #0,d1
        move.w  #OL_PLAYW,d2
        move.w  #OL_HORIZ,d3
        moveq   #0,d4               ; sky: white
        bsr     ol_rect
        moveq   #0,d0
        move.w  #OL_HORIZ,d1
        move.w  #OL_PLAYW,d2
        move.w  #OL_HORIZ+2,d3
        moveq   #3,d4               ; horizon: black line
        bsr     ol_rect
        ; ---- road strips bottom-up; d7 = y, d6 = curve accumulator
        move.w  #OL_PLAYH-2,d7
        moveq   #0,d6
.strip: move.w  d7,d0
        sub.w   #OL_HORIZ,d0        ; 2..112
        move.w  #3000,d1
        ext.l   d1
        divu    d0,d1               ; d1.w = z
        and.l   #$FFFF,d1
        ; world z = ol_z + z*4
        move.l  d1,d2
        lsl.l   #2,d2
        add.l   ol_z(pc),d2
        divu    #OL_TRACK,d2
        swap    d2                  ; remainder: worldz mod track
        and.l   #$FFFF,d2
        divu    #OL_SEGLEN,d2
        and.w   #31,d2              ; segment index
        lea     ol_curve(pc),a0
        move.b  (a0,d2.w),d3
        ext.w   d3
        add.w   d3,d6               ; accumulate curve
        move.w  d6,d3
        asr.w   #5,d3
        add.w   #148,d3             ; center
        ; half width = 14*256/z
        move.l  #3584,d4
        divu    d1,d4
        and.l   #$FFFF,d4
        ; grass dither by segment parity (rush-by animation)
        moveq   #2,d5               ; grass A: medium
        btst    #0,d2
        beq     .gok
        moveq   #1,d5               ; grass B: light
.gok:   movem.w d2/d6,-(sp)
        ; left grass [0, center-hw)
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
        ; road [center-hw, center+hw)
        move.w  d3,d0
        sub.w   d4,d0
        move.w  d7,d1
        move.w  d3,d2
        add.w   d4,d2
        movem.w d3-d5,-(sp)
        move.w  d7,d3
        addq.w  #2,d3
        moveq   #0,d4               ; road: white
        bsr     ol_rect
        movem.w (sp)+,d3-d5
        ; right grass [center+hw, OL_PLAYW)
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
        ; center stripe on odd segments
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
        moveq   #3,d4               ; stripe: black
        bsr     ol_rect
        movem.w (sp)+,d3-d4
.nostripe:
        ; record road edges at the car strip
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
        ; ---- traffic (4 cars)
        moveq   #0,d7
.traf:  move.w  d7,d0
        lsl.w   #2,d0
        lea     ol_traf0(pc),a0
        move.l  (a0,d0.w),d1
        move.l  ol_z(pc),d2
        divu    #OL_TRACK,d2
        swap    d2                  ; remainder = camera mod track
        and.l   #$FFFF,d2
        sub.l   d2,d1               ; rel
        bpl     .relok
        add.l   #OL_TRACK,d1
.relok: cmp.l   #20,d1
        blt     .ntraf
        cmp.l   #400,d1
        bgt     .ntraf
        ; size: h=1500/rel w=2100/rel ; y = HORIZ + 12000/rel
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
.yok:   ; x = 148 +/- lane offset (26 - rel/16, min 6)
        move.l  d1,d0
        lsr.l   #4,d0
        moveq   #26,d6
        sub.w   d0,d6
        cmp.w   #6,d6
        bge     .lok
        moveq   #6,d6
.lok:   btst    #0,d7               ; lane: odd index = right
        bne     .right
        neg.w   d6
.right: add.w   #148,d6             ; cx
        ; body
        move.w  d6,d0
        move.w  d4,d2
        lsr.w   #1,d2
        sub.w   d2,d0               ; x1
        move.w  d6,d2
        move.w  d4,d1
        lsr.w   #1,d1
        add.w   d1,d2               ; x2
        move.w  d5,d1
        sub.w   d3,d1               ; y1
        move.w  d5,d3
        ; cars 0-1 same dir = black, 2-3 oncoming = medium dither
        moveq   #3,d4
        cmp.w   #2,d7
        blt     .col
        moveq   #2,d4
.col:   bsr     ol_rect
.ntraf: addq.w  #1,d7
        cmp.w   #4,d7
        blt     .traf
        ; ---- player car (flash while crashed)
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
        moveq   #3,d4               ; body: black
        bsr     ol_rect
        move.w  d6,d0
        subq.w  #8,d0
        move.w  #121,d1
        move.w  d6,d2
        addq.w  #8,d2
        move.w  #126,d3
        moveq   #0,d4               ; windshield: white
        bsr     ol_rect
        move.w  d6,d0
        sub.w   #15,d0
        move.w  #131,d1
        move.w  d6,d2
        sub.w   #9,d2
        move.w  #137,d3
        moveq   #3,d4               ; wheel
        bsr     ol_rect
        move.w  d6,d0
        add.w   #9,d0
        move.w  #131,d1
        move.w  d6,d2
        add.w   #15,d2
        move.w  #137,d3
        moveq   #3,d4               ; wheel
        bsr     ol_rect
.nocar:
        ; ---- HUD
        move.w  WX(a2),d0
        addq.w  #4,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+1,d1
        move.w  #OL_PLAYW,d2
        moveq   #10,d3
        moveq   #3,d4               ; HUD: black bar
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
        ; ---- game over overlay
        move.w  ol_state(pc),d0
        cmp.w   #2,d0
        bne     .done
        move.w  #70,d0
        move.w  #56,d1
        move.w  #226,d2
        move.w  #100,d3
        moveq   #3,d4               ; overlay: black
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
        bne     .eat                ; input locked while crashed
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
        bsr     redraw_topmost
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
        ; crashed?
        move.w  ol_crash(pc),d0
        beq     .alive
        subq.w  #1,d0
        move.w  d0,ol_crash-vars(a4)
        bne     .timer
        move.w  #148,ol_x-vars(a4)  ; recover: recenter, crawl
        move.w  #5,ol_speed-vars(a4)
        bra     .timer
.alive:
        ; auto-accelerate
        move.w  ol_speed(pc),d0
        cmp.w   #60,d0
        bge     .spdok
        addq.w  #1,d0
        move.w  d0,ol_speed-vars(a4)
.spdok:
        ; grass slowdown
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
        ; curve drift
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
        ; advance camera + score
        moveq   #0,d0
        move.w  ol_speed(pc),d0
        add.l   d0,ol_z-vars(a4)
        lsr.w   #2,d0
        add.w   ol_score(pc),d0
        move.w  d0,ol_score-vars(a4)
        ; traffic movement + collision
        moveq   #0,d7
.traf:  move.w  d7,d0
        lsl.w   #2,d0
        lea     ol_traf0(pc),a0
        lea     (a0,d0.w),a0
        move.l  (a0),d1
        cmp.w   #2,d7
        blt     .same
        subq.l  #5,d1               ; oncoming
        bra     .wrap
.same:  addq.l  #5,d1
.wrap:  tst.l   d1
        bpl     .w2t
        add.l   #OL_TRACK,d1
.w2t:   cmp.l   #OL_TRACK,d1
        blt     .w3
        sub.l   #OL_TRACK,d1
.w3:    move.l  d1,(a0)
        ; collision: rel < 15 and |x - lane center| < 25
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
        ; one-second countdown (60 Hz ticks)
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

; OutLast 32-segment curve table (signed, from apps/outlast.asm)
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
str_t_outlast:  dc.b    "OutLast",0
name_outlast:   dc.b    "OutLast",0
        even

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
