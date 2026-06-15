; ============================================================================
; UnoDOS/Apple IIGS - Pac-Man (proc 9): a tile-stepped maze chase on SHR.
;
; 13x11 maze of 8x8 cells; pac (arrows, queued turns) eats dots for score; two
; ghosts greedily chase (pick the non-reversing legal move minimising Manhattan
; distance), at half speed.  Touching a ghost ends the game; clearing the dots
; wins.  Static maze in RODATA, copied to a mutable bank-0 buffer so eaten dots
; persist; actors are tracked as tile coordinates and drawn over the maze.
; ============================================================================

; PMMAZE + v_pm_* live in sys.inc.
MZW = 13
MZH = 11
PM_STEP = 8                   ; frames per pac step

PM0 = $68                     ; transient scratch (shared w/ other tick temps)
PM1 = $6A
PM2 = $6C
PM3 = $6E
PM4 = $70
PM5 = $72

; pm_tile_idx: A0=tx A1=ty -> A = ty*13 + tx.
.a16
.i16
pm_tile_idx:
        lda A1
        asl a
        asl a                  ; *4
        sta PM0
        lda A1
        asl a
        asl a
        asl a                  ; *8
        clc
        adc PM0                ; *12
        clc
        adc A1                 ; *13
        clc
        adc A0
        rts

; pm_is_wall: A0=tx A1=ty -> carry set if out of bounds or a wall.
.a16
.i16
pm_is_wall:
        lda A0
        bmi @wall
        cmp #MZW
        bcs @wall
        lda A1
        bmi @wall
        cmp #MZH
        bcs @wall
        jsr pm_tile_idx
        tax
        sep #$20
        lda PMMAZE,x
        rep #$20
        and #$00FF
        cmp #1
        beq @wall
        clc
        rts
@wall:  sec
        rts

; pacman_start: load the maze, place actors, count dots.
.a16
.i16
pacman_start:
        ldx #0
        stz PM1                ; dot count
@cp:    sep #$20
        lda f:pm_maze0,x
        sta PMMAZE,x
        cmp #2
        rep #$20
        bne @nd
        inc PM1
@nd:    inx
        cpx #(MZW*MZH)
        bcc @cp
        lda PM1
        sta v_pm_dots
        stz v_pm_state
        stz v_pm_score
        stz v_pm_last
        stz v_pm_half
        lda #1                 ; pac start tile (1,1)
        sta v_pm_px
        sta v_pm_py
        lda #3                 ; facing right
        sta v_pm_dir
        sta v_pm_ndir
        lda #6                 ; ghost 0 at (6,5)
        sta v_pm_g0x
        lda #5
        sta v_pm_g0y
        lda #2
        sta v_pm_g0d
        lda #6                 ; ghost 1 at (6,3)
        sta v_pm_g1x
        lda #3
        sta v_pm_g1y
        lda #3
        sta v_pm_g1d
        rts

; pacman_key: S0 = ascii -> set the queued direction (or restart when over).
.a16
.i16
pacman_key:
        lda v_pm_state
        beq @play
        jsr pacman_start
        jsr redraw_topmost
        rts
@play:  lda S0
        cmp #$0B
        bne :+
        lda #0
        sta v_pm_ndir
        rts
:       cmp #$0A
        bne :+
        lda #1
        sta v_pm_ndir
        rts
:       cmp #$08
        bne :+
        lda #2
        sta v_pm_ndir
        rts
:       cmp #$15
        bne :+
        lda #3
        sta v_pm_ndir
:       rts

; pm_step_dir: A0=tx A1=ty A2=dir -> A0/A1 advanced one tile (no wall check).
.a16
.i16
pm_step_dir:
        lda A2
        asl a
        tax
        lda A0
        clc
        adc f:pm_dx,x
        sta A0
        lda A1
        clc
        adc f:pm_dy,x
        sta A1
        rts

; pacman_tick: advance the game while a Pac-Man window is open + playing.
.a16
.i16
pacman_tick:
        stz S5
@scan:  lda S5
        jsr ent_x
        sep #$20
        lda v_wintab+WSTATE,x
        beq @nf
        lda v_wintab+WPROC,x
        cmp #9
        bne @nf
        rep #$20
        bra @found
@nf:    rep #$20
        inc S5
        lda S5
        cmp #MAXWIN
        bcc @scan
        rts
@found: lda v_pm_state
        beq @go
        rts
@go:    lda v_pm_last
        inc a
        sta v_pm_last
        cmp #PM_STEP
        bcc @out2
        stz v_pm_last
        jsr pm_move_pac
        lda v_pm_half
        eor #1
        sta v_pm_half
        beq @nog               ; ghosts at half speed
        jsr pm_move_ghosts
@nog:   jsr pm_check
        jsr redraw_topmost
@out2:  rts

; pm_move_pac: turn to the queued dir if legal, then advance + eat.
.a16
.i16
pm_move_pac:
        ; try queued direction
        lda v_pm_px
        sta A0
        lda v_pm_py
        sta A1
        lda v_pm_ndir
        sta A2
        jsr pm_step_dir
        jsr pm_is_wall
        bcs @keepdir
        lda v_pm_ndir
        sta v_pm_dir
@keepdir:
        lda v_pm_px
        sta A0
        lda v_pm_py
        sta A1
        lda v_pm_dir
        sta A2
        jsr pm_step_dir
        jsr pm_is_wall
        bcs @done              ; blocked
        lda A0
        sta v_pm_px
        lda A1
        sta v_pm_py
        ; eat a dot
        jsr pm_tile_idx
        tax
        sep #$20
        lda PMMAZE,x
        cmp #2
        rep #$20
        bne @done
        sep #$20
        lda #0
        sta PMMAZE,x           ; clear the dot
        rep #$20
        lda v_pm_score
        clc
        adc #10
        sta v_pm_score
        dec v_pm_dots
        bne @done
        lda #2                 ; all dots eaten -> win
        sta v_pm_state
@done:  rts

; pm_move_ghosts: greedy chase for both ghosts.
.a16
.i16
pm_move_ghosts:
        lda #.loword(v_pm_g0x)
        jsr pm_ghost_ai
        lda #.loword(v_pm_g1x)
        jsr pm_ghost_ai
        rts

; pm_ghost_ai: A = pointer to a ghost's [x,y,d] triple (3 words). Picks the
; legal, non-reversing direction minimising Manhattan distance to pac.
.a16
.i16
pm_ghost_ai:
        sta PM5                ; ghost struct base (abs addr)
        lda #$7FFF
        sta PM3                ; best distance
        lda #$FFFF
        sta PM4                ; best dir
        stz PM2                ; candidate dir 0..3
@try:   ; reverse of current dir is forbidden: rev(d) = d^1
        lda PM2
        ldx PM5
        eor $0004,x            ; current dir at +4 (3rd word)
        cmp #1
        beq @next              ; this candidate is the reverse
        ; target tile
        ldx PM5
        lda $0000,x
        sta A0
        lda $0002,x
        sta A1
        lda PM2
        sta A2
        jsr pm_step_dir
        jsr pm_is_wall
        bcs @next
        ; manhattan distance to pac
        lda A0
        sec
        sbc v_pm_px
        bpl @ax
        eor #$FFFF
        inc a
@ax:    sta PM0
        lda A1
        sec
        sbc v_pm_py
        bpl @ay
        eor #$FFFF
        inc a
@ay:    clc
        adc PM0
        cmp PM3
        bcs @next
        sta PM3
        lda PM2
        sta PM4
@next:  inc PM2
        lda PM2
        cmp #4
        bcc @try
        ; apply best dir (if any)
        lda PM4
        cmp #$FFFF
        beq @done
        ldx PM5
        sta $0004,x            ; new dir
        lda $0000,x
        sta A0
        lda $0002,x
        sta A1
        lda PM4
        sta A2
        jsr pm_step_dir
        ldx PM5
        lda A0
        sta $0000,x
        lda A1
        sta $0002,x
@done:  rts

; pm_check: ghost-on-pac collision -> dead.
.a16
.i16
pm_check:
        lda v_pm_g0x
        cmp v_pm_px
        bne @g1
        lda v_pm_g0y
        cmp v_pm_py
        bne @g1
        lda #1
        sta v_pm_state
        rts
@g1:    lda v_pm_g1x
        cmp v_pm_px
        bne @ok
        lda v_pm_g1y
        cmp v_pm_py
        bne @ok
        lda #1
        sta v_pm_state
@ok:    rts

; pacman_draw: S2 = window offset.
.a16
.i16
pacman_draw:
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta PM4                ; maze origin cx
        lda v_wintab+WY,x
        clc
        adc #2
        sta PM5                ; maze origin cy
        stz LC0                ; ty
@row:   lda LC0
        cmp #MZH
        bcs @actors
        stz LC1                ; tx
@col:   lda LC1
        cmp #MZW
        bcs @nextrow
        lda LC1
        sta A0
        lda LC0
        sta A1
        jsr pm_tile_idx
        tax
        sep #$20
        lda PMMAZE,x
        rep #$20
        and #$00FF
        ; tile -> colour: wall blue, dot grey pip, empty black corridor
        cmp #1
        bne @notwall
        lda #13                ; wall (sky blue)
        bra @put
@notwall:
        cmp #2
        bne @empty
        lda #5                 ; dot -> light grey
        bra @put
@empty: lda #4                 ; eaten corridor -> black
@put:   sta A2
        lda PM4
        clc
        adc LC1
        sta A0
        lda PM5
        clc
        adc LC0
        sta A1
        jsr fillcell
        inc LC1
        bra @col
@nextrow:
        inc LC0
        bra @row
@actors:
        ; pac (yellow)
        lda v_pm_px
        clc
        adc PM4
        sta A0
        lda v_pm_py
        clc
        adc PM5
        sta A1
        lda #7
        sta A2
        jsr fillcell
        ; ghost 0 (red), ghost 1 (pink)
        lda v_pm_g0x
        clc
        adc PM4
        sta A0
        lda v_pm_g0y
        clc
        adc PM5
        sta A1
        lda #11
        sta A2
        jsr fillcell
        lda v_pm_g1x
        clc
        adc PM4
        sta A0
        lda v_pm_g1y
        clc
        adc PM5
        sta A1
        lda #14
        sta A2
        jsr fillcell
        ; status line
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc #14
        sta A1
        lda #.loword(str_pm_score)
        sta P0
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        lda v_pm_score
        sta S0
        jsr fmt_dec
        lda #.loword(v_numbuf)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #8
        sta A0
        lda v_wintab+WY,x
        clc
        adc #14
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        lda v_pm_state
        beq @done
        cmp #2
        beq @win
        lda #.loword(str_pm_dead)
        bra @msg
@win:   lda #.loword(str_pm_win)
@msg:   sta P0
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #14
        sta A0
        lda v_wintab+WY,x
        clc
        adc #14
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
@done:  rts

; direction deltas (up, down, left, right)
pm_dx: .word 0, 0, $FFFF, 1
pm_dy: .word $FFFF, 1, 0, 0

; static maze: 1=wall, 2=dot, 0=empty (13x11)
pm_maze0:
        .byte 1,1,1,1,1,1,1,1,1,1,1,1,1
        .byte 1,2,2,2,2,2,1,2,2,2,2,2,1
        .byte 1,2,1,1,1,2,1,2,1,1,1,2,1
        .byte 1,2,1,2,2,2,2,2,2,2,1,2,1
        .byte 1,2,1,2,1,1,2,1,1,2,1,2,1
        .byte 1,2,2,2,2,2,0,2,2,2,2,2,1
        .byte 1,2,1,2,1,1,2,1,1,2,1,2,1
        .byte 1,2,1,2,2,2,2,2,2,2,1,2,1
        .byte 1,2,1,1,1,2,1,2,1,1,1,2,1
        .byte 1,2,2,2,2,2,1,2,2,2,2,2,1
        .byte 1,1,1,1,1,1,1,1,1,1,1,1,1

str_pm_score: .byte "Score", 0
str_pm_dead:  .byte "CAUGHT!", 0
str_pm_win:   .byte "YOU WIN", 0
