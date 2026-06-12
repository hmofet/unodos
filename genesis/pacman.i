; ============================================================================
; UnoDOS/Genesis Pac-Man (proc 6) - ported from amiga/pacman.i.
; 28x25 maze, one VDP cell per maze tile (PM_TILE = 8 instead of the
; Amiga's 7px software tiles); the x86 AI is unchanged (Blinky direct,
; Pinky 4-ahead, Clyde hybrid, scatter/chase schedule, frightened mode
; with the eat chain). Pac and the ghosts are HARDWARE SPRITES (1-4,
; chained after the cursor), so the maze never needs repainting under
; the actors - only eaten dots redraw.
; ============================================================================

PM_COLS     equ 28
PM_ROWS     equ 25
PM_TILE     equ 8
GX          equ 0
GY          equ 2
GDIR        equ 4
GST         equ 6
GTMR        equ 8
GSIZE       equ 10
; states
PMS_TITLE   equ 0
PMS_READY   equ 1
PMS_PLAY    equ 2
PMS_OVER    equ 4
; ghost states
GH_HOUSE    equ 0
GH_SCAT     equ 1
GH_CHASE    equ 2
GH_FRIGHT   equ 3
GH_EATEN    equ 4

; direction tables (UP,LEFT,DOWN,RIGHT)
pm_dx:  dc.w    0,-1,0,1
pm_dy:  dc.w    -1,0,1,0
pm_cornx: dc.w  26,1,1
pm_corny: dc.w  1,1,23
pm_modedur: dc.w 127,364,127,364,91,364,91,32000

; pm_walkable - d0=tx d1=ty d2=ghost? d3=eaten? -> d0=1 ok / 0 blocked
pm_walkable:
        movem.l d1-d4/a0,-(sp)
        tst.w   d1
        bmi     .no
        cmp.w   #PM_ROWS,d1
        bge     .no
        tst.w   d0
        bpl     .x1
        add.w   #PM_COLS,d0
.x1:    cmp.w   #PM_COLS,d0
        blt     .x2
        sub.w   #PM_COLS,d0
.x2:    mulu    #PM_COLS,d1
        add.w   d0,d1
        lea     VARS+v_pm_maze,a0
        moveq   #0,d4
        move.b  (a0,d1.w),d4
        cmp.w   #1,d4
        beq     .no
        cmp.w   #4,d4
        beq     .gateonly
        cmp.w   #5,d4
        beq     .gateonly
        moveq   #1,d0
        bra     .out
.gateonly:
        tst.w   d2
        beq     .no
        tst.w   d3
        beq     .no
        moveq   #1,d0
        bra     .out
.no:    moveq   #0,d0
.out:   movem.l (sp)+,d1-d4/a0
        rts

; pm_mode_state -> d0 = GH_SCAT or GH_CHASE from pm_mode parity
pm_mode_state:
        move.w  VARS+v_pm_mode,d0
        btst    #0,d0
        beq     .scat
        moveq   #GH_CHASE,d0
        rts
.scat:  moveq   #GH_SCAT,d0
        rts

; pm_load_maze - copy template, count dots
pm_load_maze:
        movem.l d0-d2/a0-a1/a4,-(sp)
        lea     pm_maze_tpl(pc),a0
        lea     VARS+v_pm_maze,a1
        moveq   #0,d2
        move.w  #PM_COLS*PM_ROWS-1,d0
.cp:    move.b  (a0)+,d1
        move.b  d1,(a1)+
        cmp.b   #2,d1
        beq     .dot
        cmp.b   #3,d1
        bne     .nx
.dot:   addq.w  #1,d2
.nx:    dbra    d0,.cp
        lea     VARS,a4
        move.w  d2,v_pm_dots(a4)
        movem.l (sp)+,d0-d2/a0-a1/a4
        rts

; pm_reset_actors
pm_reset_actors:
        movem.l d0/a0/a4,-(sp)
        lea     VARS,a4
        move.w  #14*PM_TILE,v_pm_x(a4)
        move.w  #19*PM_TILE,v_pm_y(a4)
        move.w  #1,v_pm_dir(a4)     ; LEFT
        move.w  #1,v_pm_nextdir(a4)
        lea     v_pm_gh(a4),a0
        move.w  #14*PM_TILE,GX(a0)
        move.w  #10*PM_TILE,GY(a0)
        move.w  #1,GDIR(a0)
        move.w  #GH_SCAT,GST(a0)
        clr.w   GTMR(a0)
        move.w  #13*PM_TILE,GX+GSIZE(a0)
        move.w  #12*PM_TILE,GY+GSIZE(a0)
        clr.w   GDIR+GSIZE(a0)
        move.w  #GH_HOUSE,GST+GSIZE(a0)
        move.w  #100,GTMR+GSIZE(a0)
        move.w  #15*PM_TILE,GX+GSIZE*2(a0)
        move.w  #12*PM_TILE,GY+GSIZE*2(a0)
        clr.w   GDIR+GSIZE*2(a0)
        move.w  #GH_HOUSE,GST+GSIZE*2(a0)
        move.w  #200,GTMR+GSIZE*2(a0)
        clr.w   v_pm_fright(a4)
        clr.w   v_pm_kills(a4)
        movem.l (sp)+,d0/a0/a4
        rts

; pm_new_game
pm_new_game:
        movem.l d0/a4,-(sp)
        lea     VARS,a4
        clr.w   v_pm_score(a4)
        move.w  #3,v_pm_lives(a4)
        move.w  #1,v_pm_level(a4)
        clr.w   v_pm_mode(a4)
        clr.w   v_pm_modet(a4)
        bsr     pm_load_maze
        bsr     pm_reset_actors
        move.w  #PMS_READY,v_pm_state(a4)
        move.l  v_ticks(a4),d0
        add.l   #72,d0
        move.l  d0,v_pm_statet(a4)
        move.l  v_ticks(a4),d0
        move.l  d0,v_pm_last(a4)
        movem.l (sp)+,d0/a4
        rts

; pm_steer - d7 = ghost index: pick direction at a tile center
pm_steer:
        movem.l d0-d7/a0/a3/a4,-(sp)
        lea     VARS,a4
        move.w  d7,d0
        mulu    #GSIZE,d0
        lea     v_pm_gh(a4),a3
        lea     (a3,d0.w),a3
        ; gtx/gty in d5/d6
        move.w  GX(a3),d5
        lsr.w   #3,d5
        move.w  GY(a3),d6
        lsr.w   #3,d6
        cmp.w   #GH_FRIGHT,GST(a3)
        bne     .target
        ; random direction, not reverse, walkable
        moveq   #7,d4
.rnd:   bsr     dt_rand7
        and.w   #3,d0
        move.w  GDIR(a3),d1
        eor.w   #2,d1
        cmp.w   d1,d0
        beq     .rtry
        move.w  d0,d2               ; candidate
        move.w  d0,d3
        add.w   d3,d3
        lea     pm_dx(pc),a0
        move.w  (a0,d3.w),d0
        add.w   d5,d0
        lea     pm_dy(pc),a0
        move.w  (a0,d3.w),d1
        add.w   d6,d1
        movem.l d2-d3,-(sp)
        moveq   #1,d2
        moveq   #0,d3
        bsr     pm_walkable
        movem.l (sp)+,d2-d3
        tst.w   d0
        beq     .rtry
        move.w  d2,GDIR(a3)
        bra     .out
.rtry:  dbra    d4,.rnd
        bra     .out
.target:
        ; pick target tile (d2=tx d3=ty)
        cmp.w   #GH_EATEN,GST(a3)
        bne     .noteaten
        moveq   #14,d2
        moveq   #10,d3
        bra     .pick
.noteaten:
        cmp.w   #GH_CHASE,GST(a3)
        bne     .scatter
        ; pac tile in d0/d1
        move.w  v_pm_x(a4),d0
        lsr.w   #3,d0
        move.w  v_pm_y(a4),d1
        lsr.w   #3,d1
        tst.w   d7
        beq     .blinky
        cmp.w   #1,d7
        beq     .pinky
        ; clyde: manhattan(gh,pac) <= 8 ? corner : pac
        move.w  d5,d2
        sub.w   d0,d2
        bpl     .cx
        neg.w   d2
.cx:    move.w  d6,d3
        sub.w   d1,d3
        bpl     .cy
        neg.w   d3
.cy:    add.w   d3,d2
        cmp.w   #8,d2
        bgt     .blinky
        moveq   #1,d2
        moveq   #1,d3
        bra     .pick
.pinky: ; 4 tiles ahead of pac
        move.w  v_pm_dir(a4),d4
        add.w   d4,d4
        lea     pm_dx(pc),a0
        move.w  (a0,d4.w),d2
        lsl.w   #2,d2
        add.w   d0,d2
        lea     pm_dy(pc),a0
        move.w  (a0,d4.w),d3
        lsl.w   #2,d3
        add.w   d1,d3
        bra     .pick
.blinky:
        move.w  d0,d2
        move.w  d1,d3
        bra     .pick
.scatter:
        move.w  d7,d0
        add.w   d0,d0
        lea     pm_cornx(pc),a0
        move.w  (a0,d0.w),d2
        lea     pm_corny(pc),a0
        move.w  (a0,d0.w),d3
.pick:
        ; best = argmin manhattan over dirs, excluding reverse
        moveq   #-1,d4
        move.w  #32000,d0
        move.w  d0,v_pm_tmp(a4)
        moveq   #0,d7
.try:   move.w  GDIR(a3),d0
        eor.w   #2,d0
        cmp.w   d0,d7
        beq     .skip
        move.w  d7,d1
        add.w   d1,d1
        lea     pm_dx(pc),a0
        move.w  (a0,d1.w),d0
        add.w   d5,d0               ; nx
        lea     pm_dy(pc),a0
        move.w  (a0,d1.w),d1
        add.w   d6,d1               ; ny
        movem.l d2-d3,-(sp)
        moveq   #1,d2
        moveq   #0,d3
        cmp.w   #GH_EATEN,GST(a3)
        bne     .we
        moveq   #1,d3
.we:    movem.l d0-d1,-(sp)
        bsr     pm_walkable
        move.w  d0,d2
        movem.l (sp)+,d0-d1
        tst.w   d2
        movem.l (sp)+,d2-d3
        beq     .skip
        ; wrap nx
        tst.w   d0
        bpl     .wx1
        add.w   #PM_COLS,d0
.wx1:   cmp.w   #PM_COLS,d0
        blt     .wx2
        sub.w   #PM_COLS,d0
.wx2:   sub.w   d2,d0
        bpl     .ax
        neg.w   d0
.ax:    sub.w   d3,d1
        bpl     .ay
        neg.w   d1
.ay:    add.w   d1,d0
        cmp.w   v_pm_tmp(a4),d0
        bge     .skip
        move.w  d0,v_pm_tmp(a4)
        move.w  d7,d4
.skip:  addq.w  #1,d7
        cmp.w   #4,d7
        blt     .try
        tst.w   d4
        bmi     .out
        move.w  d4,GDIR(a3)
.out:   movem.l (sp)+,d0-d7/a0/a3/a4
        rts

; pm_kill - pac dies
pm_kill:
        movem.l d0/a4,-(sp)
        lea     VARS,a4
        move.w  v_pm_lives(a4),d0
        subq.w  #1,d0
        move.w  d0,v_pm_lives(a4)
        bgt     .alive
        move.w  #PMS_OVER,v_pm_state(a4)
        move.w  v_pm_score(a4),d0
        cmp.w   v_pm_hi(a4),d0
        blt     .out
        move.w  d0,v_pm_hi(a4)
        bra     .out
.alive: bsr     pm_reset_actors
        move.w  #PMS_READY,v_pm_state(a4)
        move.l  v_ticks(a4),d0
        add.l   #72,d0
        move.l  d0,v_pm_statet(a4)
.out:   movem.l (sp)+,d0/a4
        rts

; pm_step - one game step (2 one-pixel substeps)
pm_step:
        movem.l d0-d7/a0-a4,-(sp)
        lea     VARS,a4
        ; fright countdown / mode schedule
        move.w  v_pm_fright(a4),d0
        beq     .mode
        subq.w  #1,d0
        move.w  d0,v_pm_fright(a4)
        bne     .substeps
        lea     v_pm_gh(a4),a3
        moveq   #2,d7
.fr:    cmp.w   #GH_FRIGHT,GST(a3)
        bne     .frn
        bsr     pm_mode_state
        move.w  d0,GST(a3)
.frn:   lea     GSIZE(a3),a3
        dbra    d7,.fr
        bra     .substeps
.mode:  move.w  v_pm_modet(a4),d0
        addq.w  #1,d0
        move.w  d0,v_pm_modet(a4)
        move.w  v_pm_mode(a4),d1
        cmp.w   #7,d1
        ble     .mok
        moveq   #7,d1
.mok:   add.w   d1,d1
        lea     pm_modedur(pc),a0
        cmp.w   (a0,d1.w),d0
        blt     .substeps
        clr.w   v_pm_modet(a4)
        move.w  v_pm_mode(a4),d0
        cmp.w   #7,d0
        bge     .norev
        addq.w  #1,d0
        move.w  d0,v_pm_mode(a4)
.norev: lea     v_pm_gh(a4),a3
        moveq   #2,d7
.mg:    cmp.w   #GH_SCAT,GST(a3)
        beq     .mgset
        cmp.w   #GH_CHASE,GST(a3)
        bne     .mgn
.mgset: bsr     pm_mode_state
        move.w  d0,GST(a3)
        move.w  GDIR(a3),d0
        eor.w   #2,d0
        move.w  d0,GDIR(a3)
.mgn:   lea     GSIZE(a3),a3
        dbra    d7,.mg
.substeps:
        moveq   #1,d6               ; two substeps
.sub:
        ; ---- pac at tile center?
        move.w  v_pm_x(a4),d0
        and.w   #PM_TILE-1,d0
        bne     .pmove
        move.w  v_pm_y(a4),d0
        and.w   #PM_TILE-1,d0
        bne     .pmove
        ; tile coords
        move.w  v_pm_x(a4),d2
        lsr.w   #3,d2
        move.w  v_pm_y(a4),d3
        lsr.w   #3,d3
        ; eat
        move.w  d3,d0
        mulu    #PM_COLS,d0
        add.w   d2,d0
        lea     v_pm_maze(a4),a0
        moveq   #0,d1
        move.b  (a0,d0.w),d1
        cmp.w   #2,d1
        bne     .npd
        clr.b   (a0,d0.w)
        move.w  v_pm_score(a4),d1
        add.w   #10,d1
        move.w  d1,v_pm_score(a4)
        subq.w  #1,v_pm_dots(a4)
        st      v_pm_dirty(a4)      ; redraw the eaten cell
        bra     .eaten
.npd:   cmp.w   #3,d1
        bne     .eaten
        clr.b   (a0,d0.w)
        move.w  v_pm_score(a4),d1
        add.w   #50,d1
        move.w  d1,v_pm_score(a4)
        subq.w  #1,v_pm_dots(a4)
        move.w  #200,v_pm_fright(a4)
        clr.w   v_pm_kills(a4)
        st      v_pm_dirty(a4)
        lea     v_pm_gh(a4),a3
        moveq   #2,d7
.fg:    cmp.w   #GH_SCAT,GST(a3)
        beq     .fgset
        cmp.w   #GH_CHASE,GST(a3)
        bne     .fgn
.fgset: move.w  #GH_FRIGHT,GST(a3)
        move.w  GDIR(a3),d0
        eor.w   #2,d0
        move.w  d0,GDIR(a3)
.fgn:   lea     GSIZE(a3),a3
        dbra    d7,.fg
.eaten:
        ; level clear?
        move.w  v_pm_dots(a4),d0
        bne     .turn
        addq.w  #1,v_pm_level(a4)
        bsr     pm_load_maze
        bsr     pm_reset_actors
        move.w  #PMS_READY,v_pm_state(a4)
        move.l  v_ticks(a4),d0
        add.l   #72,d0
        move.l  d0,v_pm_statet(a4)
        bra     .stepout
.turn:  ; try next_dir then dir
        move.w  v_pm_nextdir(a4),d0
        add.w   d0,d0
        lea     pm_dx(pc),a0
        move.w  (a0,d0.w),d1
        add.w   d2,d1
        lea     pm_dy(pc),a0
        move.w  (a0,d0.w),d0
        add.w   d3,d0
        exg     d0,d1
        movem.l d2-d3,-(sp)
        moveq   #0,d2
        moveq   #0,d3
        bsr     pm_walkable
        movem.l (sp)+,d2-d3
        tst.w   d0
        beq     .keep
        move.w  v_pm_nextdir(a4),d0
        move.w  d0,v_pm_dir(a4)
.keep:  move.w  v_pm_dir(a4),d0
        add.w   d0,d0
        lea     pm_dx(pc),a0
        move.w  (a0,d0.w),d1
        add.w   d2,d1
        lea     pm_dy(pc),a0
        move.w  (a0,d0.w),d0
        add.w   d3,d0
        exg     d0,d1
        movem.l d2-d3,-(sp)
        moveq   #0,d2
        moveq   #0,d3
        bsr     pm_walkable
        movem.l (sp)+,d2-d3
        tst.w   d0
        beq     .ghosts             ; blocked: don't move this substep
.pmove: move.w  v_pm_dir(a4),d0
        add.w   d0,d0
        lea     pm_dx(pc),a0
        move.w  (a0,d0.w),d1
        add.w   v_pm_x(a4),d1
        lea     pm_dy(pc),a0
        move.w  (a0,d0.w),d0
        add.w   v_pm_y(a4),d0
        ; tunnel wrap on x
        tst.w   d1
        bpl     .pw1
        move.w  #(PM_COLS-1)*PM_TILE,d1
.pw1:   cmp.w   #(PM_COLS-1)*PM_TILE,d1
        ble     .pw2
        moveq   #0,d1
.pw2:   move.w  d1,v_pm_x(a4)
        move.w  d0,v_pm_y(a4)
.ghosts:
        lea     v_pm_gh(a4),a3
        moveq   #0,d7
.gloop: cmp.w   #GH_HOUSE,GST(a3)
        bne     .gactive
        tst.w   d6                  ; only decrement once per step
        beq     .gnext
        move.w  GTMR(a3),d0
        subq.w  #1,d0
        move.w  d0,GTMR(a3)
        bgt     .gnext
        move.w  #14*PM_TILE,GX(a3)
        move.w  #10*PM_TILE,GY(a3)
        move.w  #1,GDIR(a3)
        bsr     pm_mode_state
        move.w  d0,GST(a3)
        bra     .gnext
.gactive:
        ; at tile center? steer
        move.w  GX(a3),d0
        and.w   #PM_TILE-1,d0
        bne     .gmove
        move.w  GY(a3),d0
        and.w   #PM_TILE-1,d0
        bne     .gmove
        ; eaten ghost arriving home resumes the schedule
        cmp.w   #GH_EATEN,GST(a3)
        bne     .gsteer
        cmp.w   #14*PM_TILE,GX(a3)
        bne     .gsteer
        cmp.w   #10*PM_TILE,GY(a3)
        bne     .gsteer
        bsr     pm_mode_state
        move.w  d0,GST(a3)
.gsteer:
        bsr     pm_steer
        ; can we move at all? (center + blocked = stay)
        move.w  GX(a3),d2
        lsr.w   #3,d2
        move.w  GY(a3),d3
        lsr.w   #3,d3
        move.w  GDIR(a3),d0
        add.w   d0,d0
        lea     pm_dx(pc),a0
        move.w  (a0,d0.w),d1
        add.w   d2,d1
        lea     pm_dy(pc),a0
        move.w  (a0,d0.w),d0
        add.w   d3,d0
        exg     d0,d1
        moveq   #1,d2
        moveq   #0,d3
        cmp.w   #GH_EATEN,GST(a3)
        bne     .gwe
        moveq   #1,d3
.gwe:   bsr     pm_walkable
        tst.w   d0
        beq     .gcoll
.gmove: ; frightened ghosts move only on substep 1 (half speed)
        cmp.w   #GH_FRIGHT,GST(a3)
        bne     .gdo
        tst.w   d6
        beq     .gcoll
.gdo:   move.w  GDIR(a3),d0
        add.w   d0,d0
        lea     pm_dx(pc),a0
        move.w  (a0,d0.w),d1
        add.w   GX(a3),d1
        lea     pm_dy(pc),a0
        move.w  (a0,d0.w),d0
        add.w   GY(a3),d0
        tst.w   d1
        bpl     .gw1
        move.w  #(PM_COLS-1)*PM_TILE,d1
.gw1:   cmp.w   #(PM_COLS-1)*PM_TILE,d1
        ble     .gw2
        moveq   #0,d1
.gw2:   move.w  d1,GX(a3)
        move.w  d0,GY(a3)
.gcoll: ; collision with pac
        move.w  GX(a3),d0
        sub.w   v_pm_x(a4),d0
        bpl     .ca
        neg.w   d0
.ca:    cmp.w   #6,d0
        bge     .gnext
        move.w  GY(a3),d0
        sub.w   v_pm_y(a4),d0
        bpl     .cb
        neg.w   d0
.cb:    cmp.w   #6,d0
        bge     .gnext
        cmp.w   #GH_FRIGHT,GST(a3)
        bne     .notfr
        move.w  #GH_EATEN,GST(a3)
        move.w  #200,d0
        move.w  v_pm_kills(a4),d1
        lsl.w   d1,d0
        add.w   v_pm_score(a4),d0
        move.w  d0,v_pm_score(a4)
        move.w  v_pm_kills(a4),d0
        cmp.w   #3,d0
        bge     .gnext
        addq.w  #1,v_pm_kills(a4)
        bra     .gnext
.notfr: cmp.w   #GH_EATEN,GST(a3)
        beq     .gnext
        bsr     pm_kill
        bra     .stepout
.gnext: lea     GSIZE(a3),a3
        addq.w  #1,d7
        cmp.w   #3,d7
        blt     .gloop
        dbra    d6,.sub
.stepout:
        movem.l (sp)+,d0-d7/a0-a4
        rts

; pm_draw_tile - d5=col d6=row, a2=window: one maze cell
pm_draw_tile:
        movem.l d0-d4/a0/a4,-(sp)
        lea     VARS,a4
        move.w  d6,d0
        mulu    #PM_COLS,d0
        add.w   d5,d0
        lea     v_pm_maze(a4),a0
        moveq   #0,d1
        move.b  (a0,d0.w),d1
        move.w  #ATTR_ACC+T_GSOL+10,d4  ; empty/house: black
        cmp.w   #1,d1
        bne     .nw
        move.w  #ATTR_ACC+T_PMWALL,d4
        bra     .draw
.nw:    cmp.w   #2,d1
        bne     .nd
        move.w  #ATTR_ACC+T_PMDOT,d4
        bra     .draw
.nd:    cmp.w   #3,d1
        bne     .np
        move.w  #ATTR_ACC+T_PMPOW,d4
        bra     .draw
.np:    cmp.w   #5,d1
        bne     .draw
        move.w  #ATTR_ACC+T_PMGATE,d4
.draw:  move.w  d5,d0
        add.w   WX(a2),d0
        addq.w  #1,d0
        move.w  d6,d1
        add.w   WY(a2),d1
        addq.w  #1,d1
        moveq   #1,d2
        moveq   #1,d3
        bsr     fill_cells
        movem.l (sp)+,d0-d4/a0/a4
        rts

; pm_sync_sprites - main-loop VDP sync: actors as sprites 1-4 while the
; Pac-Man window is topmost and a game is on screen; parked otherwise.
pm_sync_sprites:
        movem.l d0-d7/a0-a2/a4,-(sp)
        lea     VARS,a4
        move.w  v_zcount(a4),d2
        beq     .park
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #6,WPROC(a2)
        bne     .park
        move.w  v_pm_state(a4),d0
        beq     .park               ; title screen: no actors
        ; window pixel origin of the maze
        move.w  WX(a2),d5
        addq.w  #1,d5
        lsl.w   #3,d5
        add.w   #128,d5             ; sprite x origin
        move.w  WY(a2),d6
        addq.w  #1,d6
        lsl.w   #3,d6
        add.w   #128,d6             ; sprite y origin
        ; sprite 1: pac
        lea     VDP_CTRL,a0
        lea     VDP_DATA,a1
        move.l  #(((SPRTAB+8)&$3FFF)<<16)|$40000000|((SPRTAB>>14)&3),(a0)
        move.w  v_pm_y(a4),d0
        add.w   d6,d0
        move.w  d0,(a1)
        move.w  #$0002,(a1)         ; 8x8, link 2
        move.w  #$C000+T_SPAC,(a1)  ; pri, pal2
        move.w  v_pm_x(a4),d0
        add.w   d5,d0
        move.w  d0,(a1)
        ; sprites 2-4: ghosts
        lea     v_pm_gh(a4),a3
        moveq   #0,d7
.gh:    move.w  d7,d0
        addq.w  #2,d0               ; sprite number
        lsl.w   #3,d0
        add.w   #SPRTAB,d0
        and.w   #$3FFF,d0
        moveq   #0,d1
        move.w  d0,d1
        swap    d1
        or.l    #$40000000|((SPRTAB>>14)&3),d1
        move.l  d1,(a0)
        move.w  GY(a3),d0
        add.w   d6,d0
        move.w  d0,(a1)             ; y
        move.w  d7,d0
        addq.w  #3,d0
        and.w   #7,d0               ; link 3,4,0
        cmp.w   #5,d0
        bne     .lnk
        moveq   #0,d0
.lnk:   move.w  d0,(a1)             ; 8x8 + link
        ; tile/attr by state
        move.w  #$C000+T_SGHR,d0
        cmp.w   #GH_EATEN,GST(a3)
        bne     .ne
        move.w  #$C000+T_SGHE,d0
        bra     .attr
.ne:    cmp.w   #GH_FRIGHT,GST(a3)
        bne     .body
        move.w  #$E000+T_SGHF,d0    ; pal3: fright blue body
        move.w  v_pm_fright(a4),d1
        cmp.w   #70,d1
        bge     .attr
        btst    #3,d1
        beq     .attr
        move.w  #$C000+T_SGHF,d0    ; end flash: pal2 (pale body)
        bra     .attr
.body:  move.w  d7,d1
        add.w   d1,d1
        lea     .gtile(pc),a2
        move.w  (a2,d1.w),d0
.attr:  move.w  d0,(a1)
        move.w  GX(a3),d0
        add.w   d5,d0
        move.w  d0,(a1)             ; x
        lea     GSIZE(a3),a3
        addq.w  #1,d7
        cmp.w   #3,d7
        blt     .gh
        st      v_pm_spron(a4)
        bra     .out
.gtile: dc.w    $C000+T_SGHR,$C000+T_SGHP,$C000+T_SGHO
.park:  move.b  v_pm_spron(a4),d0
        beq     .out
        sf      v_pm_spron(a4)
        ; park sprites 1-4 (y = 0 is off-screen), keep the link chain
        lea     VDP_CTRL,a0
        lea     VDP_DATA,a1
        moveq   #1,d7
.pk:    move.w  d7,d0
        lsl.w   #3,d0
        add.w   #SPRTAB,d0
        and.w   #$3FFF,d0
        moveq   #0,d1
        move.w  d0,d1
        swap    d1
        or.l    #$40000000|((SPRTAB>>14)&3),d1
        move.l  d1,(a0)
        move.w  #0,(a1)             ; y off-screen
        addq.w  #1,d7
        cmp.w   #5,d7
        blt     .pk
.out:   movem.l (sp)+,d0-d7/a0-a2/a4
        rts

; pacman_draw - a2 = window
pacman_draw:
        movem.l d0-d7/a0-a4,-(sp)
        lea     VARS,a4
        move.w  v_pm_state(a4),d0
        bne     .game
        ; title: black field + texts
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        subq.w  #2,d3
        move.w  #ATTR_ACC+T_GSOL+10,d4
        bsr     fill_cells
        lea     str_pm_title(pc),a0
        move.w  WX(a2),d0
        add.w   #12,d0
        move.w  WY(a2),d1
        add.w   #9,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
        lea     str_pm_new(pc),a0
        move.w  WX(a2),d0
        add.w   #13,d0
        move.w  WY(a2),d1
        add.w   #14,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        bra     .done
.game:
        ; maze cells
        moveq   #0,d6
.trow:  moveq   #0,d5
.tcol:  bsr     pm_draw_tile
        addq.w  #1,d5
        cmp.w   #PM_COLS,d5
        blt     .tcol
        addq.w  #1,d6
        cmp.w   #PM_ROWS,d6
        blt     .trow
        ; panel (cols +30..+36, blue body)
        move.w  WX(a2),d0
        add.w   #29,d0
        move.w  WY(a2),d1
        addq.w  #1,d1
        moveq   #8,d2
        move.w  WH(a2),d3
        subq.w  #2,d3
        move.w  #ATTR_NORM+T_SOLBG,d4
        bsr     fill_cells
        move.w  WX(a2),d6
        add.w   #30,d6
        move.w  WY(a2),d5
        addq.w  #2,d5
        lea     str_pm_score(pc),a0
        bsr     pm_label
        addq.w  #1,d5
        move.w  v_pm_score(a4),d0
        bsr     pm_value
        addq.w  #2,d5
        lea     str_pm_hi(pc),a0
        bsr     pm_label
        addq.w  #1,d5
        move.w  v_pm_hi(a4),d0
        bsr     pm_value
        addq.w  #2,d5
        lea     str_pm_lives(pc),a0
        bsr     pm_label
        addq.w  #1,d5
        move.w  v_pm_lives(a4),d0
        bsr     pm_value
        addq.w  #2,d5
        lea     str_pm_level(pc),a0
        bsr     pm_label
        addq.w  #1,d5
        move.w  v_pm_level(a4),d0
        bsr     pm_value
        ; state overlay
        move.w  v_pm_state(a4),d0
        cmp.w   #PMS_READY,d0
        bne     .nready
        lea     str_pm_ready(pc),a0
        bra     .stext
.nready:
        cmp.w   #PMS_OVER,d0
        bne     .done
        lea     str_pm_over(pc),a0
.stext: move.w  WX(a2),d0
        add.w   #12,d0
        move.w  WY(a2),d1
        add.w   #15,d1
        move.w  #ATTR_INV,d4
        bsr     draw_str
.done:  movem.l (sp)+,d0-d7/a0-a4
        rts

; pm_value / pm_label - panel text at (d6,d5). Preserve d5/d6.
pm_value:
        movem.l d0-d1/d4/a0/a4,-(sp)
        lea     VARS,a4
        lea     v_npstat(a4),a0
        bsr     fmt_dec
        move.w  d6,d0
        move.w  d5,d1
        move.w  #ATTR_NORM,d4
        bsr     draw_str
        movem.l (sp)+,d0-d1/d4/a0/a4
        rts
pm_label:
        movem.l d0-d1/d4/a0,-(sp)
        move.w  d6,d0
        move.w  d5,d1
        move.w  #ATTR_ACC,d4
        bsr     draw_str
        movem.l (sp)+,d0-d1/d4/a0
        rts

; pacman_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
pacman_key:
        lea     VARS,a4
        cmp.b   #'n',d1
        beq     .new
        move.w  v_pm_state(a4),d0
        cmp.w   #PMS_PLAY,d0
        beq     .dirs
        cmp.w   #PMS_READY,d0
        beq     .dirs
        moveq   #1,d0
        rts
.dirs:  cmp.b   #$4C,d2
        beq     .up
        cmp.b   #$4D,d2
        beq     .down
        cmp.b   #$4F,d2
        beq     .left
        cmp.b   #$4E,d2
        beq     .right
        moveq   #1,d0
        rts
.new:   bsr     pm_new_game
        bsr     redraw_topmost
        moveq   #0,d0
        rts
.up:    clr.w   v_pm_nextdir(a4)
        moveq   #0,d0
        rts
.left:  move.w  #1,v_pm_nextdir(a4)
        moveq   #0,d0
        rts
.down:  move.w  #2,v_pm_nextdir(a4)
        moveq   #0,d0
        rts
.right: move.w  #3,v_pm_nextdir(a4)
        moveq   #0,d0
        rts

; pacman_tick - step (topmost, every 2 ticks); sprites carry the motion,
; only eaten-dot cells and the score panel repaint.
pacman_tick:
        movem.l d0-d6/a0/a2/a4,-(sp)
        lea     VARS,a4
        move.w  v_pm_state(a4),d0
        cmp.w   #PMS_READY,d0
        beq     .chk
        cmp.w   #PMS_PLAY,d0
        bne     .out
.chk:   move.w  v_zcount(a4),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #6,WPROC(a2)
        bne     .out
        move.w  v_pm_state(a4),d0
        cmp.w   #PMS_READY,d0
        bne     .play
        move.l  v_ticks(a4),d0
        cmp.l   v_pm_statet(a4),d0
        blt     .out
        move.w  #PMS_PLAY,v_pm_state(a4)
        bsr     redraw_topmost      ; clear the READY overlay
.play:  move.l  v_ticks(a4),d0
        sub.l   v_pm_last(a4),d0
        cmp.l   #2,d0
        blt     .out
        move.l  v_ticks(a4),d0
        move.l  d0,v_pm_last(a4)
        ; remember pac's tile (the dot he might eat this step)
        move.w  v_pm_x(a4),d5
        lsr.w   #3,d5
        move.w  v_pm_y(a4),d6
        lsr.w   #3,d6
        sf      v_pm_dirty(a4)
        bsr     pm_step
        move.w  v_pm_state(a4),d0
        cmp.w   #PMS_PLAY,d0
        beq     .incr
        bsr     redraw_topmost      ; death/level/over: full repaint
        bra     .out
.incr:  move.b  v_pm_dirty(a4),d0
        beq     .score
        bsr     pm_draw_tile        ; eaten dot cell (d5/d6 still set)
.score: ; score value refresh (cheap fixed cell write)
        move.w  WX(a2),d6
        add.w   #30,d6
        move.w  WY(a2),d5
        addq.w  #3,d5
        move.w  v_pm_score(a4),d0
        bsr     pm_value
.out:   movem.l (sp)+,d0-d6/a0/a2/a4
        rts

; ---------------------------------------------------------------- data
        even
str_pm_title:   dc.b    "P A C - M A N",0
str_pm_new:     dc.b    "X:new game",0
str_pm_score:   dc.b    "SCORE",0
str_pm_hi:      dc.b    "HI",0
str_pm_lives:   dc.b    "LIVES",0
str_pm_level:   dc.b    "LEVEL",0
str_pm_ready:   dc.b    "READY!",0
str_pm_over:    dc.b    "GAME OVER",0

        even
; maze template: 0 empty, 1 wall, 2 dot, 3 power, 4 house, 5 gate
pm_maze_tpl:
 dc.b 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
 dc.b 1,3,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,3,1
 dc.b 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
 dc.b 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
 dc.b 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1
 dc.b 1,2,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,2,1
 dc.b 1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1
 dc.b 1,1,1,1,1,2,1,1,1,1,0,1,0,0,0,0,1,0,1,1,1,1,2,1,1,1,1,1
 dc.b 0,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0
 dc.b 0,0,0,0,1,2,1,0,1,1,1,1,5,0,0,5,1,1,1,1,0,1,2,1,0,0,0,0
 dc.b 1,1,1,1,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,1,1,1,1
 dc.b 0,0,0,0,0,2,0,0,1,4,4,4,4,4,4,4,4,4,4,1,0,0,2,0,0,0,0,0
 dc.b 0,0,0,0,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,0,0,0,0
 dc.b 0,0,0,0,1,2,1,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1,2,1,0,0,0,0
 dc.b 1,1,1,1,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,1,1,1,1
 dc.b 1,2,2,2,2,2,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,2,2,2,2,2,1
 dc.b 1,2,1,1,1,2,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,2,1,1,1,2,1
 dc.b 1,3,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,3,1
 dc.b 1,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,1
 dc.b 1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1
 dc.b 1,2,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,2,1
 dc.b 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1
 dc.b 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
 dc.b 1,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,1
 dc.b 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
        even
