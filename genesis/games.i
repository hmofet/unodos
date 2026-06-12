; ============================================================================
; UnoDOS/Genesis games: Dostris (proc 4) + OutLast (proc 5) + game music.
; Ported from amiga/games.i - same piece tables, track table, scoring and
; physics; 50 Hz PAL tick constants rescaled to the 60 Hz NTSC vblank.
; Rendering is cell-based: gcol maps the donor's extended-palette color
; indexes (4-31) to (attr|solid-tile) name words on palette lines 2/3.
; ============================================================================

; gcol - d4 = donor color index 0..31 -> d4 = name-table word
gcol:
        movem.l a0,-(sp)
        and.w   #31,d4
        add.w   d4,d4
        lea     gcol_tab(pc),a0
        move.w  (a0,d4.w),d4
        movem.l (sp)+,a0
        rts

gcol_tab:
        dc.w    ATTR_NORM+T_SOLBG           ; 0 UI blue
        dc.w    ATTR_NORM+T_SOLCY           ; 1 UI cyan
        dc.w    ATTR_NORM+T_SOLMG           ; 2 UI magenta
        dc.w    ATTR_NORM+T_SOLFG           ; 3 UI white
        dc.w    ATTR_ACC+T_GSOL+0           ; 4 I cyan
        dc.w    ATTR_ACC+T_GSOL+1           ; 5 O / pac yellow
        dc.w    ATTR_ACC+T_GSOL+2           ; 6 T purple
        dc.w    ATTR_ACC+T_GSOL+3           ; 7 S green
        dc.w    ATTR_ACC+T_GSOL+4           ; 8 Z red
        dc.w    ATTR_ACC+T_GSOL+5           ; 9 J / maze-wall blue
        dc.w    ATTR_ACC+T_GSOL+6           ; 10 L / clyde orange
        dc.w    ATTR_KEY+T_GSOL+0           ; 11 sky
        dc.w    ATTR_KEY+T_GSOL+1           ; 12 horizon haze
        dc.w    ATTR_KEY+T_GSOL+2           ; 13 grass A
        dc.w    ATTR_KEY+T_GSOL+3           ; 14 grass B
        dc.w    ATTR_KEY+T_GSOL+4           ; 15 road gray
        dc.w    ATTR_KEY+T_GSOL+4           ; 16 (road)
        dc.w    ATTR_ACC+T_GSOL+9           ; 17 (cursor white)
        dc.w    ATTR_ACC+T_GSOL+10          ; 18 (cursor blue -> black)
        dc.w    ATTR_ACC+T_GSOL+0           ; 19 (cursor cyan)
        dc.w    ATTR_KEY+T_GSOL+5           ; 20 stripe yellow
        dc.w    ATTR_ACC+T_GSOL+7           ; 21 car / blinky red
        dc.w    ATTR_KEY+T_GSOL+6           ; 22 windshield
        dc.w    ATTR_KEY+T_GSOL+7           ; 23 wheel
        dc.w    ATTR_ACC+T_GSOL+9           ; 24 white
        dc.w    ATTR_KEY+T_GSOL+9           ; 25 traffic yellow
        dc.w    ATTR_KEY+T_GSOL+5           ; 26
        dc.w    ATTR_KEY+T_GSOL+2           ; 27
        dc.w    ATTR_ACC+T_GSOL+8           ; 28 pinky pink
        dc.w    ATTR_KEY+T_GSOL+8           ; 29 frightened blue
        dc.w    ATTR_ACC+T_GSOL+10          ; 30 black
        dc.w    ATTR_KEY+T_GSOL+5           ; 31

; gfill - d0=cx d1=cy d2=w d3=h d4=donor color: gcol + fill_cells
gfill:
        movem.l d4,-(sp)
        bsr     gcol
        bsr     fill_cells
        movem.l (sp)+,d4
        rts

; ------------------------------------------------------- game music (PSG ch1)

; gm_start - a0 = (psg,dur) note pairs, d0 = count, d1 = owner proc
gm_start:
        movem.l d0-d1/a4,-(sp)
        lea     VARS,a4
        move.l  a0,v_gm_notes(a4)
        move.w  d0,v_gm_count(a4)
        move.w  d1,v_gm_owner(a4)
        subq.w  #1,d0
        move.w  d0,v_gm_ix(a4)      ; first tick wraps to note 0
        st      v_gm_on(a4)
        move.l  v_ticks(a4),d0
        move.l  d0,v_gm_end(a4)
        movem.l (sp)+,d0-d1/a4
        rts

; gm_stop - silence and disable the sequencer
gm_stop:
        movem.l a4,-(sp)
        lea     VARS,a4
        sf      v_gm_on(a4)
        move.b  #$BF,PSG            ; ch1 volume off
        movem.l (sp)+,a4
        rts

; gm_tick - advance the sequencer; mutes while the owner is not topmost
gm_tick:
        movem.l d0-d3/a0/a2/a4,-(sp)
        lea     VARS,a4
        move.b  v_gm_on(a4),d0
        beq     .out
        move.w  v_zcount(a4),d2
        beq     .mute
        subq.w  #1,d2
        bsr     zwin_ptr
        moveq   #0,d0
        move.b  WPROC(a2),d0
        cmp.w   v_gm_owner(a4),d0
        bne     .mute
        move.l  v_ticks(a4),d0
        cmp.l   v_gm_end(a4),d0
        blt     .out
        move.w  v_gm_ix(a4),d1
        addq.w  #1,d1
        cmp.w   v_gm_count(a4),d1
        blt     .ixok
        moveq   #0,d1
.ixok:  move.w  d1,v_gm_ix(a4)
        move.l  v_gm_notes(a4),a0
        add.w   d1,d1
        add.w   d1,d1               ; index * 4 bytes
        lea     (a0,d1.w),a0
        move.w  (a0)+,d2            ; PSG value (0 = rest)
        moveq   #0,d3
        move.w  (a0),d3
        add.l   d3,d0
        move.l  d0,v_gm_end(a4)
        tst.w   d2
        beq     .rest
        ; ch1 tone: latch $A0 | low nibble, then high 6 bits
        move.w  d2,d0
        and.b   #$0F,d0
        or.b    #$A0,d0
        move.b  d0,PSG
        lsr.w   #4,d2
        and.b   #$3F,d2
        move.b  d2,PSG
        move.b  #$B3,PSG            ; ch1 volume on
        bra     .out
.rest:  move.b  #$BF,PSG
        bra     .out
.mute:  move.b  #$BF,PSG
.out:   movem.l (sp)+,d0-d3/a0/a2/a4
        rts

; ---------------------------------------------------------------- Dostris
; window (6,2,28,25): black slab at +1,+1 (12x22), board cells at +2,+2,
; panel text from +15.

DT_COLS     equ 10
DT_ROWS     equ 20

; dt_rand7 -> d0.w = 0..6 (LCG on dt_seed)
dt_rand7:
        movem.l d1-d2/a4,-(sp)
        lea     VARS,a4
        move.l  v_dt_seed(a4),d1
        move.l  d1,d2
        swap    d2
        mulu    #$4E6D,d1
        mulu    #$41C6,d2
        swap    d2
        clr.w   d2
        add.l   d2,d1
        add.l   #12345,d1
        move.l  d1,v_dt_seed(a4)
        swap    d1
        and.l   #$7FFF,d1
        divu    #7,d1
        swap    d1
        move.w  d1,d0
        movem.l (sp)+,d1-d2/a4
        rts

; dt_fits - d0=piece d1=rot d2=col d3=row -> d0=1 fits / 0 collides
dt_fits:
        movem.l d1-d7/a0-a1,-(sp)
        lsl.w   #2,d0
        add.w   d1,d0
        lsl.w   #3,d0
        lea     dt_shapes(pc),a0
        lea     (a0,d0.w),a0
        lea     VARS+v_dt_board,a1
        moveq   #0,d7
.cell:  move.b  (a0)+,d4
        ext.w   d4
        add.w   d2,d4               ; c
        move.b  (a0)+,d5
        ext.w   d5
        add.w   d3,d5               ; r
        tst.w   d4
        bmi     .no
        cmp.w   #DT_COLS,d4
        bge     .no
        cmp.w   #DT_ROWS,d5
        bge     .no
        tst.w   d5
        bmi     .next               ; above the board: OK
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

; dt_interval -> d0.w = gravity interval in NTSC ticks
; (x86 is (18-level) ticks at 18.2 Hz; *10/3 lands on the 60 Hz vblank)
dt_interval:
        move.w  #18,d0
        sub.w   VARS+v_dt_level,d0
        cmp.w   #2,d0
        bge     .ok
        moveq   #2,d0
.ok:    mulu    #10,d0
        divu    #3,d0
        rts

; dt_spawn - next becomes current; game over if it can't drop
dt_spawn:
        movem.l d0-d3/a4,-(sp)
        lea     VARS,a4
        move.w  v_dt_next(a4),d0
        move.w  d0,v_dt_piece(a4)
        bsr     dt_rand7
        move.w  d0,v_dt_next(a4)
        clr.w   v_dt_rot(a4)
        move.w  #3,v_dt_col(a4)
        move.w  #-1,v_dt_row(a4)
        move.l  v_ticks(a4),d0
        move.l  d0,v_dt_last(a4)
        move.w  v_dt_piece(a4),d0
        moveq   #0,d1
        moveq   #3,d2
        moveq   #0,d3
        bsr     dt_fits
        tst.w   d0
        bne     .ok
        move.w  #3,v_dt_state(a4)   ; game over
        bsr     gm_stop
.ok:    movem.l (sp)+,d0-d3/a4
        rts

; dt_clear_lines - collapse full rows, score them
dt_clear_lines:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        lea     v_dt_board(a4),a0
        moveq   #0,d7               ; cleared count
        moveq   #0,d5               ; row
.row:   moveq   #0,d6               ; col
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
        move.w  d7,d0
        add.w   d0,d0
        lea     dt_linescore(pc),a1
        move.w  (a1,d0.w),d0
        move.w  v_dt_level(a4),d1
        addq.w  #1,d1
        mulu    d1,d0
        add.w   v_dt_score(a4),d0
        move.w  d0,v_dt_score(a4)
        move.w  v_dt_lines(a4),d0
        add.w   d7,d0
        move.w  d0,v_dt_lines(a4)
        and.l   #$FFFF,d0
        divu    #10,d0
        addq.w  #1,d0
        cmp.w   #15,d0
        ble     .lvok
        moveq   #15,d0
.lvok:  move.w  d0,v_dt_level(a4)
.done:  movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; dt_lock - stamp the piece into the board, clear, respawn
dt_lock:
        movem.l d0-d7/a0-a1/a4,-(sp)
        lea     VARS,a4
        move.w  v_dt_piece(a4),d0
        move.w  d0,d6
        lsl.w   #2,d0
        add.w   v_dt_rot(a4),d0
        lsl.w   #3,d0
        lea     dt_shapes(pc),a0
        lea     (a0,d0.w),a0
        lea     v_dt_board(a4),a1
        lea     dt_colors(pc),a3
        moveq   #0,d5
        move.b  (a3,d6.w),d5
        addq.w  #1,d5               ; stored as color+1
        moveq   #0,d7
.cell:  move.b  (a0)+,d1
        ext.w   d1
        add.w   v_dt_col(a4),d1
        move.b  (a0)+,d2
        ext.w   d2
        add.w   v_dt_row(a4),d2
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
        movem.l (sp)+,d0-d7/a0-a1/a4
        rts

; dt_step - gravity: move down or lock
dt_step:
        movem.l d0-d3/a4,-(sp)
        lea     VARS,a4
        move.w  v_dt_piece(a4),d0
        move.w  v_dt_rot(a4),d1
        move.w  v_dt_col(a4),d2
        move.w  v_dt_row(a4),d3
        addq.w  #1,d3
        bsr     dt_fits
        tst.w   d0
        beq     .lock
        addq.w  #1,v_dt_row(a4)
        bra     .out
.lock:  bsr     dt_lock
.out:   movem.l (sp)+,d0-d3/a4
        rts

; dt_cell - d0=col d1=row d4=donor color, a2=window: one board cell
dt_cell:
        movem.l d0-d3,-(sp)
        add.w   WX(a2),d0
        addq.w  #2,d0
        add.w   WY(a2),d1
        addq.w  #2,d1
        moveq   #1,d2
        moveq   #1,d3
        bsr     gfill
        movem.l (sp)+,d0-d3
        rts

; dostris_draw - a2 = window
dostris_draw:
        movem.l d0-d7/a0-a4,-(sp)
        lea     VARS,a4
        ; content: window body blue; black slab behind the board
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        subq.w  #2,d3
        move.w  #ATTR_NORM+T_SOLBG,d4
        bsr     fill_cells
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        moveq   #12,d2
        moveq   #22,d3
        moveq   #30,d4              ; black
        bsr     gfill
        ; settled cells
        lea     v_dt_board(a4),a3
        moveq   #0,d6               ; row
.brow:  moveq   #0,d5               ; col
.bcol:  move.w  d6,d0
        mulu    #DT_COLS,d0
        add.w   d5,d0
        moveq   #0,d4
        move.b  (a3,d0.w),d4
        beq     .empty
        subq.w  #1,d4
        move.w  d5,d0
        move.w  d6,d1
        bsr     dt_cell
.empty: addq.w  #1,d5
        cmp.w   #DT_COLS,d5
        blt     .bcol
        addq.w  #1,d6
        cmp.w   #DT_ROWS,d6
        blt     .brow
        ; falling piece (playing or paused)
        move.w  v_dt_state(a4),d0
        subq.w  #1,d0
        cmp.w   #1,d0
        bhi     .nopiece
        move.w  v_dt_piece(a4),d0
        move.w  d0,d7
        lsl.w   #2,d0
        add.w   v_dt_rot(a4),d0
        lsl.w   #3,d0
        lea     dt_shapes(pc),a0
        lea     (a0,d0.w),a0
        lea     dt_colors(pc),a1
        moveq   #0,d4
        move.b  (a1,d7.w),d4
        moveq   #0,d6
.pcell: move.b  (a0)+,d0
        ext.w   d0
        add.w   v_dt_col(a4),d0
        move.b  (a0)+,d1
        ext.w   d1
        add.w   v_dt_row(a4),d1
        bmi     .pskip
        bsr     dt_cell
.pskip: addq.w  #1,d6
        cmp.w   #4,d6
        blt     .pcell
.nopiece:
        ; panel (text x = wx+15)
        move.w  WX(a2),d6
        add.w   #15,d6
        move.w  WY(a2),d5
        addq.w  #2,d5
        lea     str_dt_title(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        addq.w  #2,d5
        lea     str_dt_score(pc),a0
        bsr     dt_label
        move.w  v_dt_score(a4),d0
        bsr     dt_value
        addq.w  #1,d5
        lea     str_dt_lines(pc),a0
        bsr     dt_label
        move.w  v_dt_lines(a4),d0
        bsr     dt_value
        addq.w  #1,d5
        lea     str_dt_level(pc),a0
        bsr     dt_label
        move.w  v_dt_level(a4),d0
        bsr     dt_value
        ; next preview
        addq.w  #2,d5
        lea     str_dt_next(pc),a0
        bsr     dt_label
        addq.w  #1,d5
        move.w  v_dt_next(a4),d7
        move.w  d7,d0
        lsl.w   #5,d0
        lea     dt_shapes(pc),a0
        lea     (a0,d0.w),a0
        lea     dt_colors(pc),a1
        moveq   #0,d4
        move.b  (a1,d7.w),d4
        moveq   #0,d3
.nx:    movem.l d3,-(sp)
        moveq   #0,d0
        move.b  (a0)+,d0
        add.w   d6,d0
        moveq   #0,d1
        move.b  (a0)+,d1
        add.w   d5,d1
        moveq   #1,d2
        moveq   #1,d3
        bsr     gfill
        movem.l (sp)+,d3
        addq.w  #1,d3
        cmp.w   #4,d3
        blt     .nx
        ; help + state
        addq.w  #5,d5
        lea     str_dt_help1(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        addq.w  #1,d5
        lea     str_dt_help2(pc),a0
        move.w  d6,d0
        move.w  d5,d1
        bsr     draw_str
        addq.w  #2,d5
        move.w  v_dt_state(a4),d0
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
        move.w  #ATTR_INV,d4
        bsr     draw_str
.sdone: movem.l (sp)+,d0-d7/a0-a4
        rts

; dt_label - a0 = string at (d6,d5) accent. Preserves d5/d6.
dt_label:
        movem.l d0-d1/d4/a0,-(sp)
        move.w  d6,d0
        move.w  d5,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        movem.l (sp)+,d0-d1/d4/a0
        rts

; dt_value - d0.w printed at (d6+7,d5) white. Preserves d5/d6.
dt_value:
        movem.l d0-d1/d4/a0/a4,-(sp)
        lea     VARS,a4
        lea     v_npstat(a4),a0
        bsr     fmt_dec
        move.w  d6,d0
        addq.w  #7,d0
        move.w  d5,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
        movem.l (sp)+,d0-d1/d4/a0/a4
        rts

; dostris_new - reset and start
dostris_new:
        movem.l d0-d1/a0/a4,-(sp)
        lea     VARS,a4
        lea     v_dt_board(a4),a0
        move.w  #DT_ROWS*DT_COLS-1,d0
.clr:   clr.b   (a0)+
        dbra    d0,.clr
        clr.w   v_dt_score(a4)
        clr.w   v_dt_lines(a4)
        move.w  #1,v_dt_level(a4)
        move.l  v_ticks(a4),d0
        bset    #0,d0
        move.l  d0,v_dt_seed(a4)
        bsr     dt_rand7
        move.w  d0,v_dt_next(a4)
        move.w  #1,v_dt_state(a4)
        bsr     dt_spawn
        lea     koro_notes(pc),a0
        move.w  koro_count(pc),d0
        moveq   #4,d1
        bsr     gm_start
        movem.l (sp)+,d0-d1/a0/a4
        rts

; dostris_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
dostris_key:
        lea     VARS,a4
        cmp.b   #'n',d1
        beq     .new
        cmp.b   #'p',d1
        beq     .pause
        move.w  v_dt_state(a4),d0
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
        cmp.b   #32,d1              ; space = hard drop
        beq     .hard
.nope:  moveq   #1,d0
        rts
.new:   bsr     dostris_new
        bra     .redraw
.pause: move.w  v_dt_state(a4),d0
        cmp.w   #1,d0
        beq     .topause
        cmp.w   #2,d0
        bne     .redraw
        move.w  #1,v_dt_state(a4)
        move.l  v_ticks(a4),d0
        move.l  d0,v_dt_last(a4)
        lea     koro_notes(pc),a0
        move.w  koro_count(pc),d0
        moveq   #4,d1
        bsr     gm_start
        bra     .redraw
.topause:
        move.w  #2,v_dt_state(a4)
        bsr     gm_stop
        bra     .redraw
.left:  move.w  v_dt_piece(a4),d0
        move.w  v_dt_rot(a4),d1
        move.w  v_dt_col(a4),d2
        subq.w  #1,d2
        move.w  v_dt_row(a4),d3
        bsr     dt_fits
        tst.w   d0
        beq     .redraw
        subq.w  #1,v_dt_col(a4)
        bra     .redraw
.right: move.w  v_dt_piece(a4),d0
        move.w  v_dt_rot(a4),d1
        move.w  v_dt_col(a4),d2
        addq.w  #1,d2
        move.w  v_dt_row(a4),d3
        bsr     dt_fits
        tst.w   d0
        beq     .redraw
        addq.w  #1,v_dt_col(a4)
        bra     .redraw
.rot:   move.w  v_dt_piece(a4),d0
        move.w  v_dt_rot(a4),d1
        addq.w  #1,d1
        and.w   #3,d1
        move.w  v_dt_col(a4),d2
        move.w  v_dt_row(a4),d3
        bsr     dt_fits
        tst.w   d0
        beq     .redraw
        move.w  v_dt_rot(a4),d0
        addq.w  #1,d0
        and.w   #3,d0
        move.w  d0,v_dt_rot(a4)
        bra     .redraw
.soft:  move.w  v_dt_piece(a4),d0
        move.w  v_dt_rot(a4),d1
        move.w  v_dt_col(a4),d2
        move.w  v_dt_row(a4),d3
        addq.w  #1,d3
        bsr     dt_fits
        tst.w   d0
        beq     .softlock
        addq.w  #1,v_dt_row(a4)
        addq.w  #1,v_dt_score(a4)
        move.l  v_ticks(a4),d0
        move.l  d0,v_dt_last(a4)
        bra     .redraw
.softlock:
        bsr     dt_lock
        bra     .redraw
.hard:  move.w  v_dt_piece(a4),d0
        move.w  v_dt_rot(a4),d1
        move.w  v_dt_col(a4),d2
        move.w  v_dt_row(a4),d3
.hloop: addq.w  #1,d3
        bsr     dt_fits
        tst.w   d0
        beq     .hdone
        addq.w  #1,v_dt_row(a4)
        addq.w  #2,v_dt_score(a4)
        move.w  v_dt_row(a4),d3
        move.w  v_dt_piece(a4),d0
        move.w  v_dt_rot(a4),d1
        move.w  v_dt_col(a4),d2
        bra     .hloop
.hdone: bsr     dt_lock
.redraw:
        bsr     redraw_topmost
        moveq   #0,d0
        rts

; dostris_tick - gravity from the main loop (topmost + playing only)
dostris_tick:
        movem.l d0-d2/a2/a4,-(sp)
        lea     VARS,a4
        move.w  v_dt_state(a4),d0
        cmp.w   #1,d0
        bne     .out
        move.w  v_zcount(a4),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #4,WPROC(a2)
        bne     .out
        move.l  v_ticks(a4),d1
        sub.l   v_dt_last(a4),d1
        bsr     dt_interval
        ext.l   d0
        cmp.l   d0,d1
        blt     .out
        move.l  v_ticks(a4),d0
        move.l  d0,v_dt_last(a4)
        bsr     dt_step
        bsr     redraw_topmost
.out:   movem.l (sp)+,d0-d2/a2/a4
        rts

; ---------------------------------------------------------------- OutLast
; window (1,1,38,26): HUD = content row +1, playfield rows +2..+24
; (23 rows x 36 cols). The donor's per-strip road math runs per cell row
; in the same 0..288px space; ol_x/ol_roadl/r stay in pixels.

OL_SEGLEN   equ 80
OL_TRACK    equ 2560
OL_FROWS    equ 23                  ; playfield rows
OL_FCOLS    equ 36
OL_HORIZR   equ 4                   ; haze row (playfield-relative)

; outlast_new
outlast_new:
        movem.l d0-d1/a0/a4,-(sp)
        lea     VARS,a4
        move.w  #148,v_ol_x(a4)
        clr.w   v_ol_speed(a4)
        clr.l   v_ol_z(a4)
        clr.w   v_ol_score(a4)
        move.w  #60,v_ol_time(a4)
        clr.w   v_ol_crash(a4)
        move.l  #400,v_ol_traf0(a4)
        move.l  #1600,v_ol_traf1(a4)
        move.l  #800,v_ol_traf2(a4)
        move.l  #2000,v_ol_traf3(a4)
        move.l  v_ticks(a4),d0
        move.l  d0,v_ol_last(a4)
        move.l  d0,v_ol_lastsec(a4)
        move.w  #1,v_ol_state(a4)
        lea     drive_notes(pc),a0
        move.w  drive_count(pc),d0
        moveq   #5,d1
        bsr     gm_start
        movem.l (sp)+,d0-d1/a0/a4
        rts

; ol_fillrow - one playfield cell row span: d0=cx0 d1=row d2=cx1(excl)
; d4=donor color, a2=window. Clips to [0,OL_FCOLS). Preserves d0-d4.
ol_fillrow:
        movem.l d0-d4,-(sp)
        tst.w   d0
        bge     .c0
        moveq   #0,d0
.c0:    cmp.w   #OL_FCOLS,d2
        ble     .c1
        move.w  #OL_FCOLS,d2
.c1:    cmp.w   d0,d2
        ble     .skip
        sub.w   d0,d2               ; w
        add.w   WX(a2),d0
        addq.w  #1,d0
        add.w   WY(a2),d1
        addq.w  #2,d1
        moveq   #1,d3
        bsr     gfill
.skip:  movem.l (sp)+,d0-d4
        rts

; outlast_draw - a2 = window
outlast_draw:
        movem.l d0-d7/a0-a4,-(sp)
        lea     VARS,a4
        ; HUD row (content row +1, white)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        moveq   #1,d3
        move.w  #ATTR_INV+T_SOLBG,d4
        bsr     fill_cells
        move.w  v_ol_state(a4),d0
        bne     .ingame
        ; ---- title screen: black field, converging road, car
        moveq   #0,d0
        moveq   #0,d1
        moveq   #OL_FCOLS,d2
        moveq   #30,d4              ; black
        moveq   #0,d3
.trow:  move.w  d3,d1
        bsr     ol_fillrow
        addq.w  #1,d3
        cmp.w   #OL_FROWS,d3
        blt     .trow
        moveq   #7,d3               ; road bands rows 7..21
.band:  move.w  d3,d1
        move.w  d3,d2
        subq.w  #6,d2               ; half width grows with the row
        move.w  #18,d0
        sub.w   d2,d0
        move.w  #18,d4
        add.w   d2,d4
        move.w  d4,d2
        moveq   #15,d4              ; road gray
        bsr     ol_fillrow
        addq.w  #1,d3
        cmp.w   #22,d3
        blt     .band
        ; car silhouette
        moveq   #16,d0
        moveq   #19,d1
        moveq   #21,d2
        moveq   #21,d4
        bsr     ol_fillrow
        addq.w  #1,d1
        bsr     ol_fillrow
        moveq   #17,d0
        moveq   #19,d1
        moveq   #20,d2
        moveq   #22,d4
        bsr     ol_fillrow
        lea     str_ol_title(pc),a0
        move.w  WX(a2),d0
        add.w   #12,d0
        move.w  WY(a2),d1
        addq.w  #5,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
        lea     str_ol_prompt(pc),a0
        move.w  WX(a2),d0
        add.w   #14,d0
        move.w  WY(a2),d1
        add.w   #16,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        bra     .hud
.ingame:
        ; ---- sky rows 0..3, haze row 4
        moveq   #0,d3
.sky:   moveq   #0,d0
        move.w  d3,d1
        moveq   #OL_FCOLS,d2
        moveq   #11,d4
        bsr     ol_fillrow
        addq.w  #1,d3
        cmp.w   #OL_HORIZR,d3
        blt     .sky
        moveq   #0,d0
        move.w  #OL_HORIZR,d1
        moveq   #OL_FCOLS,d2
        moveq   #12,d4
        bsr     ol_fillrow
        ; ---- road rows bottom-up; d7 = row, d6 = curve accumulator
        move.w  #OL_FROWS-1,d7
        moveq   #0,d6
.strip: move.w  d7,d0
        sub.w   #OL_HORIZR,d0
        lsl.w   #3,d0
        addq.w  #4,d0               ; dy px (12..148+)
        move.w  #3000,d1
        ext.l   d1
        divu    d0,d1
        and.l   #$FFFF,d1           ; d1 = z
        ; world z = ol_z + z*4 -> segment index
        move.l  d1,d2
        lsl.l   #2,d2
        add.l   v_ol_z(a4),d2
        divu    #OL_TRACK,d2
        swap    d2
        and.l   #$FFFF,d2
        divu    #OL_SEGLEN,d2
        and.w   #31,d2              ; segment index
        lea     ol_curve(pc),a0
        move.b  (a0,d2.w),d3
        ext.w   d3
        add.w   d3,d6
        add.w   d3,d6
        add.w   d3,d6               ; x3: ~4 donor 2px strips per cell row
        move.w  d6,d3
        asr.w   #5,d3
        add.w   #148,d3             ; center px
        ; half width px = 3584/z
        move.l  #3584,d4
        divu    d1,d4
        and.l   #$FFFF,d4
        ; record road edges at the car row (donor px space)
        cmp.w   #OL_FROWS-1,d7
        bne     .noedge
        move.w  d3,d0
        sub.w   d4,d0
        move.w  d0,v_ol_roadl(a4)
        move.w  d3,d0
        add.w   d4,d0
        move.w  d0,v_ol_roadr(a4)
.noedge:
        ; cells: left grass [0,lc) road [lc,rc) right grass [rc,36)
        movem.w d2/d6,-(sp)
        move.w  d3,d5
        sub.w   d4,d5
        asr.w   #3,d5               ; lc
        move.w  d3,d6
        add.w   d4,d6
        asr.w   #3,d6
        addq.w  #1,d6               ; rc (round up)
        ; grass color by segment parity
        moveq   #13,d4
        btst    #0,d2
        beq     .gok
        moveq   #14,d4
.gok:   moveq   #0,d0
        move.w  d7,d1
        move.w  d5,d2
        bsr     ol_fillrow          ; left grass
        move.w  d5,d0
        move.w  d6,d2
        movem.w d4,-(sp)
        moveq   #15,d4
        bsr     ol_fillrow          ; road
        movem.w (sp)+,d4
        move.w  d6,d0
        moveq   #OL_FCOLS,d2
        bsr     ol_fillrow          ; right grass
        movem.w (sp)+,d2/d6
        ; center stripe cell on odd segments
        btst    #0,d2
        beq     .nostripe
        move.w  d3,d0
        asr.w   #3,d0
        move.w  d7,d1
        move.w  d0,d2
        addq.w  #1,d2
        moveq   #20,d4
        bsr     ol_fillrow
.nostripe:
        subq.w  #1,d7
        cmp.w   #OL_HORIZR,d7
        bgt     .strip
        ; ---- traffic (4 cars)
        moveq   #0,d7
.traf:  move.w  d7,d0
        lsl.w   #2,d0
        lea     v_ol_traf0(a4),a0
        move.l  (a0,d0.w),d1
        move.l  v_ol_z(a4),d2
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
        ; bottom row: y = HORIZ*8 + 12000/rel px
        move.l  #12000,d5
        divu    d1,d5
        and.l   #$FFFF,d5
        add.w   #OL_HORIZR*8,d5
        lsr.w   #3,d5
        cmp.w   #OL_FROWS-1,d5
        ble     .yok
        move.w  #OL_FROWS-1,d5
.yok:   ; lane center cell -> d6 (px = 148 +/- (26 - rel/16, min 6))
        move.l  d1,d0
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
        asr.w   #3,d6
        ; h cells -> d3, w cells -> d2 (px sizes / 8, min 1)
        move.l  #1500,d3
        divu    d1,d3
        lsr.w   #3,d3
        addq.w  #1,d3
        move.l  #2100,d2
        divu    d1,d2
        lsr.w   #3,d2
        addq.w  #1,d2
        ; left col + right end
        move.w  d2,d0
        lsr.w   #1,d0
        sub.w   d0,d6
        add.w   d6,d2               ; right end col (ol_fillrow preserves)
        move.w  d5,d1
        sub.w   d3,d1
        addq.w  #1,d1               ; top row
        ; color: 0-1 same dir yellow, 2-3 oncoming white
        moveq   #25,d4
        cmp.w   #2,d7
        blt     .tcol
        moveq   #24,d4
.tcol:
.trow2: cmp.w   d5,d1
        bgt     .ntraf
        move.w  d6,d0
        bsr     ol_fillrow
        addq.w  #1,d1
        bra     .trow2
.ntraf: addq.w  #1,d7
        cmp.w   #4,d7
        blt     .traf
        ; ---- player car (flash while crashed)
        move.w  v_ol_crash(a4),d0
        btst    #2,d0
        bne     .nocar
        move.w  v_ol_x(a4),d6
        asr.w   #3,d6               ; car center cell
        move.w  d6,d0
        subq.w  #1,d0
        moveq   #19,d1
        move.w  d6,d2
        addq.w  #2,d2
        moveq   #21,d4
        bsr     ol_fillrow          ; body row 1
        addq.w  #1,d1
        bsr     ol_fillrow          ; body row 2
        move.w  d6,d0
        moveq   #19,d1
        move.w  d6,d2
        addq.w  #1,d2
        moveq   #22,d4
        bsr     ol_fillrow          ; windshield
        move.w  d6,d0
        subq.w  #1,d0
        moveq   #21,d1
        move.w  d6,d2
        moveq   #23,d4
        bsr     ol_fillrow          ; left wheel
        move.w  d6,d0
        addq.w  #1,d0
        move.w  d6,d2
        addq.w  #2,d2
        bsr     ol_fillrow          ; right wheel
.nocar:
        ; ---- game over overlay
        move.w  v_ol_state(a4),d0
        cmp.w   #2,d0
        bne     .hud
        lea     str_ol_over(pc),a0
        move.w  WX(a2),d0
        add.w   #14,d0
        move.w  WY(a2),d1
        add.w   #11,d1
        move.w  #ATTR_INV,d4
        bsr     draw_str
        lea     str_ol_prompt(pc),a0
        move.w  WX(a2),d0
        add.w   #14,d0
        move.w  WY(a2),d1
        add.w   #13,d1
        move.w  #ATTR_INV,d4
        bsr     draw_str
.hud:
        ; ---- HUD text (blue on the white bar)
        lea     v_npstat(a4),a0
        lea     str_ol_speed(pc),a1
        bsr     str_append
        move.w  v_ol_speed(a4),d0
        bsr     fmt_dec
.s1:    tst.b   (a0)+
        bne     .s1
        subq.l  #1,a0
        lea     str_ol_score(pc),a1
        bsr     str_append
        move.w  v_ol_score(a4),d0
        bsr     fmt_dec
.s2:    tst.b   (a0)+
        bne     .s2
        subq.l  #1,a0
        lea     str_ol_time(pc),a1
        bsr     str_append
        move.w  v_ol_time(a4),d0
        bsr     fmt_dec
        lea     v_npstat(a4),a0
        move.w  WX(a2),d0
        addq.w  #2,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        move.w  #ATTR_INV,d4
        bsr     draw_str
        movem.l (sp)+,d0-d7/a0-a4
        rts

; outlast_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
outlast_key:
        lea     VARS,a4
        cmp.b   #'n',d1
        beq     .new
        move.w  v_ol_state(a4),d0
        cmp.w   #1,d0
        bne     .nope
        move.w  v_ol_crash(a4),d0
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
.left:  move.w  v_ol_x(a4),d0
        subq.w  #8,d0
        cmp.w   #20,d0
        bge     .lset
        moveq   #20,d0
.lset:  move.w  d0,v_ol_x(a4)
        moveq   #0,d0
        rts
.right: move.w  v_ol_x(a4),d0
        addq.w  #8,d0
        cmp.w   #276,d0
        ble     .rset
        move.w  #276,d0
.rset:  move.w  d0,v_ol_x(a4)
        moveq   #0,d0
        rts
.accel: move.w  v_ol_speed(a4),d0
        addq.w  #4,d0
        cmp.w   #60,d0
        ble     .aset
        moveq   #60,d0
.aset:  move.w  d0,v_ol_speed(a4)
        moveq   #0,d0
        rts
.brake: move.w  v_ol_speed(a4),d0
        subq.w  #8,d0
        bge     .bset
        moveq   #0,d0
.bset:  move.w  d0,v_ol_speed(a4)
        moveq   #0,d0
        rts

; outlast_tick - physics step + redraw (topmost + playing, every 10 ticks)
outlast_tick:
        movem.l d0-d7/a0/a2/a4,-(sp)
        lea     VARS,a4
        move.w  v_ol_state(a4),d0
        cmp.w   #1,d0
        bne     .out
        move.w  v_zcount(a4),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #5,WPROC(a2)
        bne     .out
        move.l  v_ticks(a4),d0
        sub.l   v_ol_last(a4),d0
        cmp.l   #10,d0
        blt     .out
        move.l  v_ticks(a4),d0
        move.l  d0,v_ol_last(a4)
        ; crashed?
        move.w  v_ol_crash(a4),d0
        beq     .alive
        subq.w  #1,d0
        move.w  d0,v_ol_crash(a4)
        bne     .timer
        move.w  #148,v_ol_x(a4)     ; recover: recenter, crawl
        move.w  #5,v_ol_speed(a4)
        bra     .timer
.alive:
        ; auto-accelerate
        move.w  v_ol_speed(a4),d0
        cmp.w   #60,d0
        bge     .spdok
        addq.w  #1,d0
        move.w  d0,v_ol_speed(a4)
.spdok:
        ; grass slowdown
        move.w  v_ol_x(a4),d1
        cmp.w   v_ol_roadl(a4),d1
        blt     .grass
        cmp.w   v_ol_roadr(a4),d1
        ble     .ongrassok
.grass: move.w  v_ol_speed(a4),d0
        subq.w  #2,d0
        cmp.w   #5,d0
        bge     .gset
        moveq   #5,d0
.gset:  move.w  d0,v_ol_speed(a4)
.ongrassok:
        ; curve drift
        move.l  v_ol_z(a4),d0
        divu    #OL_TRACK,d0
        swap    d0
        and.l   #$FFFF,d0
        divu    #OL_SEGLEN,d0
        and.w   #31,d0
        lea     ol_curve(pc),a0
        move.b  (a0,d0.w),d1
        ext.w   d1
        asr.w   #3,d1
        move.w  v_ol_x(a4),d0
        sub.w   d1,d0
        cmp.w   #20,d0
        bge     .dx1
        moveq   #20,d0
.dx1:   cmp.w   #276,d0
        ble     .dx2
        move.w  #276,d0
.dx2:   move.w  d0,v_ol_x(a4)
        ; advance camera + score
        moveq   #0,d0
        move.w  v_ol_speed(a4),d0
        add.l   d0,v_ol_z(a4)
        lsr.w   #2,d0
        add.w   v_ol_score(a4),d0
        move.w  d0,v_ol_score(a4)
        ; traffic movement + collision
        moveq   #0,d7
.traf:  move.w  d7,d0
        lsl.w   #2,d0
        lea     v_ol_traf0(a4),a0
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
        move.l  v_ol_z(a4),d2
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
.lck:   move.w  v_ol_x(a4),d3
        sub.w   d2,d3
        bpl     .abs
        neg.w   d3
.abs:   cmp.w   #25,d3
        bge     .ntraf
        move.w  #30,v_ol_crash(a4)
        clr.w   v_ol_speed(a4)
.ntraf: addq.w  #1,d7
        cmp.w   #4,d7
        blt     .traf
.timer:
        ; one-second countdown (60 NTSC ticks)
        move.l  v_ticks(a4),d0
        sub.l   v_ol_lastsec(a4),d0
        cmp.l   #60,d0
        blt     .draw
        move.l  v_ticks(a4),d0
        move.l  d0,v_ol_lastsec(a4)
        move.w  v_ol_time(a4),d0
        subq.w  #1,d0
        bge     .tset
        moveq   #0,d0
.tset:  move.w  d0,v_ol_time(a4)
        bne     .draw
        move.w  #2,v_ol_state(a4)
        bsr     gm_stop
.draw:  bsr     redraw_topmost
.out:   movem.l (sp)+,d0-d7/a0/a2/a4
        rts

; ---------------------------------------------------------------- data

str_dt_title:   dc.b    "DOSTRIS",0
str_dt_score:   dc.b    "Score",0
str_dt_lines:   dc.b    "Lines",0
str_dt_level:   dc.b    "Level",0
str_dt_next:    dc.b    "Next",0
str_dt_help1:   dc.b    "Pad:move/rot",0
str_dt_help2:   dc.b    "A:drop Y:pause",0
str_dt_newg:    dc.b    "X:new game",0
str_dt_paused:  dc.b    "PAUSED",0
str_dt_over:    dc.b    "GAME OVER",0
str_ol_title:   dc.b    "O U T L A S T",0
str_ol_prompt:  dc.b    "X:drive",0
str_ol_speed:   dc.b    "Speed ",0
str_ol_score:   dc.b    "  Score ",0
str_ol_time:    dc.b    "  Time ",0
str_ol_over:    dc.b    "GAME OVER",0

        even
; 7 pieces x 4 rotations x 4 cells x (col,row) - from apps/dostris.asm
dt_shapes:
        dc.b    0,1,1,1,2,1,3,1,  2,0,2,1,2,2,2,3,  0,2,1,2,2,2,3,2,  1,0,1,1,1,2,1,3
        dc.b    1,0,2,0,1,1,2,1,  1,0,2,0,1,1,2,1,  1,0,2,0,1,1,2,1,  1,0,2,0,1,1,2,1
        dc.b    1,0,0,1,1,1,2,1,  0,0,0,1,1,1,0,2,  0,0,1,0,2,0,1,1,  1,0,0,1,1,1,1,2
        dc.b    1,0,2,0,0,1,1,1,  0,0,0,1,1,1,1,2,  1,0,2,0,0,1,1,1,  0,0,0,1,1,1,1,2
        dc.b    0,0,1,0,1,1,2,1,  1,0,0,1,1,1,0,2,  0,0,1,0,1,1,2,1,  1,0,0,1,1,1,0,2
        dc.b    0,0,0,1,1,1,2,1,  0,0,1,0,0,1,0,2,  0,0,1,0,2,0,2,1,  1,0,1,1,0,2,1,2
        dc.b    2,0,0,1,1,1,2,1,  0,0,0,1,0,2,1,2,  0,0,1,0,2,0,0,1,  0,0,1,0,1,1,1,2
dt_colors:
        dc.b    4,5,6,7,8,9,10      ; I,O,T,S,Z,J,L (donor ext indexes)
        even
dt_linescore:
        dc.w    0,40,100,300,1200

; OutLast 32-segment curve table (signed, from apps/outlast.asm)
ol_curve:
        dc.b    0,0,0,0,0,0,0,0
        dc.b    5,15,25,30, 25,15,5,0
        dc.b    0,0,0,0,0,0,0,0
        dc.b    -5,-15,-25,-30, -25,-15,-5,0
        even
