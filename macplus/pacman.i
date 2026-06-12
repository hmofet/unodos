; ============================================================================
; UnoDOS/MacPlus Pac-Man (proc 6) - port of amiga/pacman.i (itself the
; apps/pacman.asm port). Identical maze, 3-ghost AI (Blinky direct, Pinky
; 4-ahead, Clyde hybrid), scatter/chase schedule, frightened mode + eat
; chain. 1-bit rendering scheme:
;   backdrop  black(3)        walls  medium dither(2)   dots/power  white(0)
;   pac       white 6x6       gate   light dither(1)
;   ghosts: body fill density tells them apart - Blinky solid white,
;   Pinky 50% dither, Clyde 25% dither, all with black eyes; frightened =
;   hollow white shell (solid-white flash when about to end); eaten = the
;   3x3 white "eyes" heading home.
; ============================================================================

PACMAN_PROC equ 6

PM_COLS     equ 28
PM_ROWS     equ 25
PM_TILE     equ 7
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

; pm_dxy tables (UP,LEFT,DOWN,RIGHT)
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
        lea     pm_maze(pc),a0
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
        move.w  pm_mode(pc),d0
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
        lea     pm_maze(pc),a1
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
        lea     vars(pc),a4
        move.w  d2,pm_dots-vars(a4)
        movem.l (sp)+,d0-d2/a0-a1/a4
        rts

; pm_reset_actors
pm_reset_actors:
        movem.l d0/a0/a4,-(sp)
        lea     vars(pc),a4
        move.w  #14*PM_TILE,pm_x-vars(a4)
        move.w  #19*PM_TILE,pm_y-vars(a4)
        move.w  #1,pm_dir-vars(a4)      ; LEFT
        move.w  #1,pm_nextdir-vars(a4)
        lea     pm_gh(pc),a0
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
        clr.w   pm_fright-vars(a4)
        clr.w   pm_kills-vars(a4)
        movem.l (sp)+,d0/a0/a4
        rts

; pm_new_game
pm_new_game:
        movem.l d0/a4,-(sp)
        lea     vars(pc),a4
        clr.w   pm_score-vars(a4)
        move.w  #3,pm_lives-vars(a4)
        move.w  #1,pm_level-vars(a4)
        clr.w   pm_mode-vars(a4)
        clr.w   pm_modet-vars(a4)
        bsr     pm_load_maze
        bsr     pm_reset_actors
        move.w  #PMS_READY,pm_state-vars(a4)
        move.l  ticks(pc),d0
        add.l   #60,d0
        move.l  d0,pm_statet-vars(a4)
        move.l  ticks(pc),d0
        move.l  d0,pm_last-vars(a4)
        movem.l (sp)+,d0/a4
        rts

; pm_steer - d7 = ghost index: pick direction at a tile center
pm_steer:
        movem.l d0-d7/a0/a3/a4,-(sp)
        move.w  d7,d0
        mulu    #GSIZE,d0
        lea     pm_gh(pc),a3
        lea     (a3,d0.w),a3
        ; gtx/gty in d5/d6
        move.w  GX(a3),d5
        and.l   #$FFFF,d5
        divu    #PM_TILE,d5
        and.l   #$FFFF,d5
        move.w  GY(a3),d6
        and.l   #$FFFF,d6
        divu    #PM_TILE,d6
        and.l   #$FFFF,d6
        cmp.w   #GH_FRIGHT,GST(a3)
        bne     .target
        ; random direction, not reverse, walkable
        moveq   #7,d4               ; tries
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
        move.w  pm_x(pc),d0
        and.l   #$FFFF,d0
        divu    #PM_TILE,d0
        and.l   #$FFFF,d0
        move.w  pm_y(pc),d1
        and.l   #$FFFF,d1
        divu    #PM_TILE,d1
        and.l   #$FFFF,d1
        tst.w   d7
        beq     .blinky
        cmp.w   #1,d7
        beq     .pinky
        ; clyde: manhattan(gh,pac) <= 8 ? corner TL : pac
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
        move.w  pm_dir(pc),d4
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
        ; best = argmin manhattan over UP,LEFT,DOWN,RIGHT excluding reverse
        moveq   #-1,d4              ; best dir
        move.w  #32000,d0
        lea     vars(pc),a4
        move.w  d0,pm_tmp-vars(a4)
        moveq   #0,d7               ; candidate (d7 reuse: saved)
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
        move.w  d0,d2               ; walkable flag
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
.wx2:   ; dist = |nx-d2| + |ny-d3|
        sub.w   d2,d0
        bpl     .ax
        neg.w   d0
.ax:    sub.w   d3,d1
        bpl     .ay
        neg.w   d1
.ay:    add.w   d1,d0
        cmp.w   pm_tmp(pc),d0
        bge     .skip
        lea     vars(pc),a4
        move.w  d0,pm_tmp-vars(a4)
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
        lea     vars(pc),a4
        move.w  pm_lives(pc),d0
        subq.w  #1,d0
        move.w  d0,pm_lives-vars(a4)
        bgt     .alive
        move.w  #PMS_OVER,pm_state-vars(a4)
        move.w  pm_score(pc),d0
        cmp.w   pm_hi(pc),d0
        blt     .out
        move.w  d0,pm_hi-vars(a4)
        bra     .out
.alive: bsr     pm_reset_actors
        move.w  #PMS_READY,pm_state-vars(a4)
        move.l  ticks(pc),d0
        add.l   #60,d0
        move.l  d0,pm_statet-vars(a4)
.out:   movem.l (sp)+,d0/a4
        rts

; pm_step - one game step (2 one-pixel substeps)
pm_step:
        movem.l d0-d7/a0-a4,-(sp)
        lea     vars(pc),a4
        ; fright countdown / mode schedule
        move.w  pm_fright(pc),d0
        beq     .mode
        subq.w  #1,d0
        move.w  d0,pm_fright-vars(a4)
        bne     .substeps
        ; fright over: frightened ghosts resume the schedule
        lea     pm_gh(pc),a3
        moveq   #2,d7
.fr:    cmp.w   #GH_FRIGHT,GST(a3)
        bne     .frn
        bsr     pm_mode_state
        move.w  d0,GST(a3)
.frn:   lea     GSIZE(a3),a3
        dbra    d7,.fr
        bra     .substeps
.mode:  move.w  pm_modet(pc),d0
        addq.w  #1,d0
        move.w  d0,pm_modet-vars(a4)
        move.w  pm_mode(pc),d1
        cmp.w   #7,d1
        ble     .mok
        moveq   #7,d1
.mok:   add.w   d1,d1
        lea     pm_modedur(pc),a0
        cmp.w   (a0,d1.w),d0
        blt     .substeps
        clr.w   pm_modet-vars(a4)
        move.w  pm_mode(pc),d0
        cmp.w   #7,d0
        bge     .norev
        addq.w  #1,d0
        move.w  d0,pm_mode-vars(a4)
.norev: lea     pm_gh(pc),a3
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
        move.w  pm_x(pc),d0
        and.l   #$FFFF,d0
        divu    #PM_TILE,d0
        swap    d0
        tst.w   d0
        bne     .pmove
        move.w  pm_y(pc),d0
        and.l   #$FFFF,d0
        divu    #PM_TILE,d0
        swap    d0
        tst.w   d0
        bne     .pmove
        ; tile coords
        move.w  pm_x(pc),d2
        and.l   #$FFFF,d2
        divu    #PM_TILE,d2
        and.l   #$FFFF,d2
        move.w  pm_y(pc),d3
        and.l   #$FFFF,d3
        divu    #PM_TILE,d3
        and.l   #$FFFF,d3
        ; eat
        move.w  d3,d0
        mulu    #PM_COLS,d0
        add.w   d2,d0
        lea     pm_maze(pc),a0
        moveq   #0,d1
        move.b  (a0,d0.w),d1
        cmp.w   #2,d1
        bne     .npd
        clr.b   (a0,d0.w)
        move.w  pm_score(pc),d1
        add.w   #10,d1
        move.w  d1,pm_score-vars(a4)
        subq.w  #1,pm_dots-vars(a4)
        bra     .eaten
.npd:   cmp.w   #3,d1
        bne     .eaten
        clr.b   (a0,d0.w)
        move.w  pm_score(pc),d1
        add.w   #50,d1
        move.w  d1,pm_score-vars(a4)
        subq.w  #1,pm_dots-vars(a4)
        move.w  #200,pm_fright-vars(a4)
        clr.w   pm_kills-vars(a4)
        ; frighten active ghosts + reverse
        lea     pm_gh(pc),a3
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
        move.w  pm_dots(pc),d0
        bne     .turn
        addq.w  #1,pm_level-vars(a4)
        bsr     pm_load_maze
        bsr     pm_reset_actors
        move.w  #PMS_READY,pm_state-vars(a4)
        move.l  ticks(pc),d0
        add.l   #60,d0
        move.l  d0,pm_statet-vars(a4)
        bra     .stepout
.turn:  ; try next_dir then dir
        move.w  pm_nextdir(pc),d0
        add.w   d0,d0
        lea     pm_dx(pc),a0
        move.w  (a0,d0.w),d1
        add.w   d2,d1
        lea     pm_dy(pc),a0
        move.w  (a0,d0.w),d0
        add.w   d3,d0
        exg     d0,d1               ; d0=nx d1=ny
        movem.l d2-d3,-(sp)
        moveq   #0,d2
        moveq   #0,d3
        bsr     pm_walkable
        movem.l (sp)+,d2-d3
        tst.w   d0
        beq     .keep
        move.w  pm_nextdir(pc),d0
        move.w  d0,pm_dir-vars(a4)
.keep:  move.w  pm_dir(pc),d0
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
.pmove: move.w  pm_dir(pc),d0
        add.w   d0,d0
        lea     pm_dx(pc),a0
        move.w  (a0,d0.w),d1
        add.w   pm_x(pc),d1
        lea     pm_dy(pc),a0
        move.w  (a0,d0.w),d0
        add.w   pm_y(pc),d0
        ; tunnel wrap on x
        tst.w   d1
        bpl     .pw1
        move.w  #(PM_COLS-1)*PM_TILE,d1
.pw1:   cmp.w   #(PM_COLS-1)*PM_TILE,d1
        ble     .pw2
        moveq   #0,d1
.pw2:   move.w  d1,pm_x-vars(a4)
        move.w  d0,pm_y-vars(a4)
.ghosts:
        lea     pm_gh(pc),a3
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
        and.l   #$FFFF,d0
        divu    #PM_TILE,d0
        swap    d0
        tst.w   d0
        bne     .gmove
        move.w  GY(a3),d0
        and.l   #$FFFF,d0
        divu    #PM_TILE,d0
        swap    d0
        tst.w   d0
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
        and.l   #$FFFF,d2
        divu    #PM_TILE,d2
        and.l   #$FFFF,d2
        move.w  GY(a3),d3
        and.l   #$FFFF,d3
        divu    #PM_TILE,d3
        and.l   #$FFFF,d3
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
        sub.w   pm_x(pc),d0
        bpl     .ca
        neg.w   d0
.ca:    cmp.w   #6,d0
        bge     .gnext
        move.w  GY(a3),d0
        sub.w   pm_y(pc),d0
        bpl     .cb
        neg.w   d0
.cb:    cmp.w   #6,d0
        bge     .gnext
        cmp.w   #GH_FRIGHT,GST(a3)
        bne     .notfr
        move.w  #GH_EATEN,GST(a3)
        ; score += 200 << kills
        move.w  #200,d0
        move.w  pm_kills(pc),d1
        lsl.w   d1,d0
        add.w   pm_score(pc),d0
        move.w  d0,pm_score-vars(a4)
        move.w  pm_kills(pc),d0
        cmp.w   #3,d0
        bge     .gnext
        addq.w  #1,pm_kills-vars(a4)
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

; pm_trect - d0=tx d1=ty d2=inset d4=color, a2=window: draw a tile square
pm_trect:
        movem.l d0-d3,-(sp)
        mulu    #PM_TILE,d0
        add.w   WX(a2),d0
        addq.w  #4,d0
        add.w   d2,d0
        mulu    #PM_TILE,d1
        add.w   WY(a2),d1
        add.w   #TBAR_H+2,d1
        add.w   d2,d1
        move.w  #PM_TILE,d3
        sub.w   d2,d3
        sub.w   d2,d3
        move.w  d3,d2
        bsr     fill_rect
        movem.l (sp)+,d0-d3
        rts

; pm_draw_tile - d5=col d6=row, a2=window: black backdrop + tile content
pm_draw_tile:
        movem.l d0-d4/a0,-(sp)
        move.w  d5,d0
        move.w  d6,d1
        moveq   #0,d2
        moveq   #3,d4               ; black base
        bsr     pm_trect
        move.w  d6,d0
        mulu    #PM_COLS,d0
        add.w   d5,d0
        lea     pm_maze(pc),a0
        moveq   #0,d1
        move.b  (a0,d0.w),d1
        beq     .done
        cmp.w   #1,d1
        bne     .ndot
        move.w  d5,d0
        move.w  d6,d1
        moveq   #1,d2
        moveq   #2,d4               ; walls: medium dither
        bsr     pm_trect
        bra     .done
.ndot:  cmp.w   #2,d1
        bne     .npow
        move.w  d5,d0
        move.w  d6,d1
        moveq   #3,d2
        moveq   #0,d4               ; dot: white
        bsr     pm_trect
        bra     .done
.npow:  cmp.w   #3,d1
        bne     .ngate
        move.w  d5,d0
        move.w  d6,d1
        moveq   #2,d2
        moveq   #0,d4               ; power pellet: bigger white
        bsr     pm_trect
        bra     .done
.ngate: cmp.w   #5,d1
        bne     .done
        move.w  d5,d0
        move.w  d6,d1
        moveq   #3,d2
        moveq   #1,d4               ; gate: light dither
        bsr     pm_trect
.done:  movem.l (sp)+,d0-d4/a0
        rts

; pm_draw_actors - a2 = window: pac + ghosts at current positions
pm_draw_actors:
        movem.l d0-d7/a0/a3,-(sp)
        move.w  pm_x(pc),d0
        add.w   WX(a2),d0
        addq.w  #4,d0
        move.w  pm_y(pc),d1
        add.w   WY(a2),d1
        add.w   #TBAR_H+2,d1
        moveq   #6,d2
        moveq   #6,d3
        moveq   #0,d4               ; pac: solid white
        bsr     fill_rect
        lea     pm_gh(pc),a3
        moveq   #0,d7
.gdrw2: move.w  GX(a3),d0
        add.w   WX(a2),d0
        addq.w  #4,d0
        move.w  GY(a3),d1
        add.w   WY(a2),d1
        add.w   #TBAR_H+2,d1
        cmp.w   #GH_EATEN,GST(a3)
        bne     .gnorm2
        addq.w  #2,d0
        addq.w  #2,d1
        moveq   #3,d2
        moveq   #3,d3
        moveq   #0,d4               ; eaten: small white "eyes" going home
        bsr     fill_rect
        bra     .gn2
.gnorm2:
        cmp.w   #GH_FRIGHT,GST(a3)
        beq     .gfright
        ; body fill density identifies the ghost (solid/50%/25%)
        move.w  d7,d2
        lea     pm_ghcol(pc),a0
        moveq   #0,d4
        move.b  (a0,d2.w),d4
        moveq   #6,d2
        moveq   #6,d3
        bsr     fill_rect
        ; eyes (black)
        movem.w d0-d1,-(sp)
        addq.w  #1,d0
        addq.w  #1,d1
        moveq   #1,d2
        moveq   #1,d3
        moveq   #3,d4
        bsr     fill_rect
        addq.w  #3,d0
        bsr     fill_rect
        movem.w (sp)+,d0-d1
        bra     .gn2
.gfright:
        ; frightened: hollow white shell; solid-white flash near the end
        moveq   #6,d2
        moveq   #6,d3
        moveq   #0,d4
        bsr     fill_rect
        move.w  pm_fright(pc),d2
        cmp.w   #70,d2
        bge     .ghollow
        btst    #3,d2
        bne     .gn2                ; flash phase: stay solid white
.ghollow:
        addq.w  #1,d0
        addq.w  #1,d1
        moveq   #4,d2
        moveq   #4,d3
        moveq   #3,d4               ; black core -> outline shell
        bsr     fill_rect
.gn2:   lea     GSIZE(a3),a3
        addq.w  #1,d7
        cmp.w   #3,d7
        blt     .gdrw2
        movem.l (sp)+,d0-d7/a0/a3
        rts

; pm_record_old - stash actor positions before a step
pm_record_old:
        movem.l d0/a0/a3/a4,-(sp)
        lea     vars(pc),a4
        lea     pm_old-vars(a4),a0
        move.w  pm_x(pc),(a0)+
        move.w  pm_y(pc),(a0)+
        lea     pm_gh(pc),a3
        move.w  GX(a3),(a0)+
        move.w  GY(a3),(a0)+
        move.w  GX+GSIZE(a3),(a0)+
        move.w  GY+GSIZE(a3),(a0)+
        move.w  GX+GSIZE*2(a3),(a0)+
        move.w  GY+GSIZE*2(a3),(a0)+
        movem.l (sp)+,d0/a0/a3/a4
        rts

; pm_refresh - a2 = window: redraw tiles under old actor spots + actors
pm_refresh:
        movem.l d0-d7/a0-a1,-(sp)
        lea     pm_old(pc),a1
        moveq   #0,d7               ; actor index
.act:   move.w  (a1)+,d5            ; old x
        move.w  (a1)+,d6            ; old y
        and.l   #$FFFF,d5
        divu    #PM_TILE,d5
        and.l   #$FFFF,d5
        and.l   #$FFFF,d6
        divu    #PM_TILE,d6
        and.l   #$FFFF,d6
        bsr     pm_draw_tile
        addq.w  #1,d5
        cmp.w   #PM_COLS,d5
        bge     .r1
        bsr     pm_draw_tile
.r1:    addq.w  #1,d6
        cmp.w   #PM_ROWS,d6
        bge     .r2
        cmp.w   #PM_COLS,d5
        bge     .r3
        bsr     pm_draw_tile
.r3:    subq.w  #1,d5
        bsr     pm_draw_tile
.r2:    addq.w  #1,d7
        cmp.w   #4,d7
        blt     .act
        bsr     pm_draw_actors
        ; score value refresh
        move.w  WX(a2),d6
        add.w   #PM_COLS*PM_TILE+10,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+16,d5
        move.w  d6,d0
        move.w  d5,d1
        moveq   #48,d2
        moveq   #8,d3
        moveq   #3,d4               ; erase on the black backdrop
        bsr     fill_rect
        move.w  pm_score(pc),d0
        bsr     pm_value
        movem.l (sp)+,d0-d7/a0-a1
        rts

; pacman_draw - a2 = window
pacman_draw:
        movem.l d0-d7/a0-a4,-(sp)
        ; black maze backdrop (whole content area)
        move.w  WX(a2),d0
        addq.w  #1,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H,d1
        move.w  WW(a2),d2
        subq.w  #2,d2
        move.w  WH(a2),d3
        sub.w   #TBAR_H+1,d3
        moveq   #3,d4               ; black backdrop
        bsr     fill_rect
        move.w  pm_state(pc),d0
        bne     .game
        ; title
        lea     str_pm_title(pc),a0
        move.w  WX(a2),d0
        add.w   #90,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+60,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        lea     str_pm_new(pc),a0
        move.w  WX(a2),d0
        add.w   #96,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+100,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        bra     .done
.game:
        ; tiles via the shared tile renderer
        moveq   #0,d6               ; row
.trow:  moveq   #0,d5               ; col
.tcol:  bsr     pm_draw_tile
        addq.w  #1,d5
        cmp.w   #PM_COLS,d5
        blt     .tcol
        addq.w  #1,d6
        cmp.w   #PM_ROWS,d6
        blt     .trow
        bsr     pm_draw_actors
        ; HUD
        move.w  WX(a2),d6
        add.w   #PM_COLS*PM_TILE+10,d6
        move.w  WY(a2),d5
        add.w   #TBAR_H+6,d5
        lea     str_pm_score(pc),a0
        bsr     pm_label
        add.w   #10,d5
        move.w  pm_score(pc),d0
        bsr     pm_value
        add.w   #14,d5
        lea     str_pm_hi(pc),a0
        bsr     pm_label
        add.w   #10,d5
        move.w  pm_hi(pc),d0
        bsr     pm_value
        add.w   #14,d5
        lea     str_pm_lives(pc),a0
        bsr     pm_label
        add.w   #10,d5
        move.w  pm_lives(pc),d0
        bsr     pm_value
        add.w   #14,d5
        lea     str_pm_level(pc),a0
        bsr     pm_label
        add.w   #10,d5
        move.w  pm_level(pc),d0
        bsr     pm_value
        ; state text
        move.w  pm_state(pc),d0
        cmp.w   #PMS_READY,d0
        bne     .nready
        lea     str_pm_ready(pc),a0
        bra     .stext
.nready:
        cmp.w   #PMS_OVER,d0
        bne     .done
        lea     str_pm_over(pc),a0
.stext: move.w  WX(a2),d0
        add.w   #86,d0
        move.w  WY(a2),d1
        add.w   #TBAR_H+92,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
.done:  movem.l (sp)+,d0-d7/a0-a4
        rts

; pm_value - d0.w at (d6,d5), white on the black backdrop
pm_value:
        movem.l d0-d3/a0,-(sp)
        lea     npstat(pc),a0
        bsr     fmt_dec
        move.w  d6,d0
        move.w  d5,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        movem.l (sp)+,d0-d3/a0
        rts

; pm_label - a0=string at (d6,d5), white on the black backdrop
pm_label:
        movem.l d0-d3/a0,-(sp)
        move.w  d6,d0
        move.w  d5,d1
        moveq   #0,d2
        moveq   #3,d3
        bsr     draw_string_bg
        movem.l (sp)+,d0-d3/a0
        rts

; pacman_key - d1=ascii d2=raw -> d0=0 consumed / 1 not
pacman_key:
        lea     vars(pc),a4
        cmp.b   #'n',d1
        beq     .new
        move.w  pm_state(pc),d0
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
.up:    clr.w   pm_nextdir-vars(a4)
        moveq   #0,d0
        rts
.left:  move.w  #1,pm_nextdir-vars(a4)
        moveq   #0,d0
        rts
.down:  move.w  #2,pm_nextdir-vars(a4)
        moveq   #0,d0
        rts
.right: move.w  #3,pm_nextdir-vars(a4)
        moveq   #0,d0
        rts

; pacman_tick - step + redraw (topmost, every 2 ticks)
pacman_tick:
        movem.l d0-d2/a2/a4,-(sp)
        move.w  pm_state(pc),d0
        cmp.w   #PMS_READY,d0
        beq     .chk
        cmp.w   #PMS_PLAY,d0
        bne     .out
.chk:   move.w  zcount(pc),d2
        beq     .out
        subq.w  #1,d2
        bsr     zwin_ptr
        cmp.b   #PACMAN_PROC,WPROC(a2)
        bne     .out
        lea     vars(pc),a4
        move.w  pm_state(pc),d0
        cmp.w   #PMS_READY,d0
        bne     .play
        move.l  ticks(pc),d0
        cmp.l   pm_statet(pc),d0
        blt     .out
        move.w  #PMS_PLAY,pm_state-vars(a4)
        bsr     redraw_topmost      ; clear the READY overlay
.play:  move.l  ticks(pc),d0
        sub.l   pm_last(pc),d0
        cmp.l   #2,d0
        blt     .out
        move.l  ticks(pc),d0
        move.l  d0,pm_last-vars(a4)
        bsr     pm_record_old
        bsr     pm_step
        move.w  pm_state(pc),d0
        cmp.w   #PMS_PLAY,d0
        beq     .incr
        bsr     redraw_topmost      ; death/level/over: full repaint
        bra     .out
.incr:  bsr     pm_refresh          ; steady state: dirty tiles only
.out:   movem.l (sp)+,d0-d2/a2/a4
        rts

; ---------------------------------------------------------------- data
; ghost body fills: Blinky solid white, Pinky 50% dither, Clyde 25% dither
pm_ghcol:       dc.b    0,2,1
        even
str_pm_title:   dc.b    "P A C - M A N",0
str_pm_new:     dc.b    "N: new game",0
str_pm_score:   dc.b    "SCORE",0
str_pm_hi:      dc.b    "HI",0
str_pm_lives:   dc.b    "LIVES",0
str_pm_level:   dc.b    "LEVEL",0
str_pm_ready:   dc.b    "READY!",0
str_pm_over:    dc.b    "GAME OVER",0
str_t_pacman:   dc.b    "Pac-Man",0
name_pacman:    dc.b    "Pac-Man",0

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
