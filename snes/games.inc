; ============================================================================
; UnoDOS/SNES games (milestone 2): Dostris (proc 4). Ported from
; genesis/games.i - same piece tables, scoring and physics. Rendering is
; cell-based: gcol maps a donor extended-palette colour index to a
; (palette-attr | game-solid-tile) name word on palette lines 2/3.
; Game music is M3 (SPC700) - the gm_* hooks here are no-ops for now.
; ============================================================================

DT_COLS = 10
DT_ROWS = 20

; gcol: A = donor colour 0..31 -> A = cell name word (gcol_tab lookup)
.proc gcol
.a16
.i16
        and #$001F
        asl a
        tax
        lda gcol_tab,x
        rts
.endproc

; gfill: A0=cx A1=cy A2=w A3=h, A4 = donor colour -> gcol + fill_cells
.proc gfill
.a16
.i16
        lda A4
        jsr gcol
        sta A4
        jsr fill_cells
        rts
.endproc

; is_game_topmost -> carry set if the topmost window's proc is a game (4-6)
.proc is_game_topmost
.a16
.i16
        lda v_zcount
        beq @no
        dec a
        jsr zent_x
        sep #$20
.a8
        lda v_wintab+WPROC,x
        rep #$20
.a16
        and #$00FF
        cmp #4
        bcc @no
        cmp #7
        bcs @no
        sec
        rts
@no:    clc
        rts
.endproc

; pad_game - NMI game-mode input: latch d-pad (with hold-repeat) + face
; buttons into v_pad_edge; the main loop decodes them as game keys. No cursor.
.proc pad_game
.a16
.i16
        lda v_pad
        sta S0
        lda v_padprev
        eor #$FFFF
        and S0
        sta S1                  ; newly pressed
        lda S0
        and #PAD_DPAD
        bne @held
        stz v_pad_rptn
        bra @edges
@held:  lda v_pad_rptn
        inc a
        sta v_pad_rptn
        cmp #12
        bcc @edges
        and #3
        bne @edges
        lda S0                  ; repeat tick: held d-pad dirs fire again
        and #PAD_DPAD
        ora S1
        sta S1
@edges: lda S1
        and #(PAD_DPAD|PAD_A|PAD_Y|PAD_X|PAD_B|PAD_STA|PAD_SEL)
        ora v_pad_edge
        sta v_pad_edge
        stz v_mouse_btn
        rts
.endproc

; pad_game_events - main loop: S0 = edge bits, post game keys (uses S0; ev_post
; keeps to S6/S7 so S0 survives across posts)
.proc pad_game_events
.a16
.i16
        lda S0
        bit #PAD_B
        beq @nb
        lda #1
        sta v_kb_toggle
@nb:    lda S0
        bit #PAD_STA
        beq @nst
        lda #EV_KEY
        sta A0
        lda #27
        sta A1
        jsr ev_post
@nst:   lda S0
        bit #PAD_A
        beq @na
        lda #EV_KEY
        sta A0
        lda #32                 ; A = hard drop (space)
        sta A1
        jsr ev_post
@na:    lda S0
        bit #PAD_X
        beq @nx
        lda #EV_KEY
        sta A0
        lda #'n'
        sta A1
        jsr ev_post
@nx:    lda S0
        bit #PAD_Y
        beq @ny
        lda #EV_KEY
        sta A0
        lda #'p'
        sta A1
        jsr ev_post
@ny:    lda S0
        bit #PAD_UP
        beq @nu
        lda #EV_KEY
        sta A0
        lda #($4C * 256)        ; up arrow raw
        sta A1
        jsr ev_post
@nu:    lda S0
        bit #PAD_DN
        beq @nd
        lda #EV_KEY
        sta A0
        lda #($4D * 256)        ; down
        sta A1
        jsr ev_post
@nd:    lda S0
        bit #PAD_LT
        beq @nl
        lda #EV_KEY
        sta A0
        lda #($4F * 256)        ; left
        sta A1
        jsr ev_post
@nl:    lda S0
        bit #PAD_RT
        beq @nr
        lda #EV_KEY
        sta A0
        lda #($4E * 256)        ; right
        sta A1
        jsr ev_post
@nr:    rts
.endproc

; ----------------------------------------------------------------------------
; Dostris
; ----------------------------------------------------------------------------

; dt_rand7 -> A = 0..6 (16-bit LCG on v_dt_seedl/h)
.proc dt_rand7
.a16
.i16
        ; seed = seed*$8405 + 1 (cheap 16-bit LCG; good enough for pieces)
        lda v_dt_seedl
        sta S0
        ; multiply S0 * $8405 via shifts is heavy; use a simpler xorshift
        lda S0
        asl a
        asl a
        asl a
        eor S0
        clc
        adc v_dt_seedh
        adc #$6D2B
        sta v_dt_seedl
        eor v_dt_seedh
        sta v_dt_seedh
        ; mod 7
        and #$7FFF
        sta S0
        lda #7
        sta S1
        jsr div16               ; A=quot, S0=remainder
        lda S0
        rts
.endproc

; dt_fits: S4=piece S5=rot S6=col S7=row -> A = 1 fits / 0 collides
.proc dt_fits
.a16
.i16
        lda S4
        asl a
        asl a
        clc
        adc S5
        asl a
        asl a
        asl a
        clc
        adc #.loword(dt_shapes)
        sta P0
        ldy #$0000
        stz LC0
@cell:  sep #$20
.a8
        lda (P0),y
        rep #$20
.a16
        and #$00FF
        cmp #$80
        bcc @cp
        ora #$FF00
@cp:    clc
        adc S6
        sta A0                  ; c
        iny
        sep #$20
.a8
        lda (P0),y
        rep #$20
.a16
        and #$00FF
        cmp #$80
        bcc @rp
        ora #$FF00
@rp:    clc
        adc S7
        sta A1                  ; r
        iny
        lda A0
        bmi @no
        cmp #DT_COLS
        bcs @no
        lda A1
        bmi @next               ; above the board: ok
        cmp #DT_ROWS
        bcs @no
        ; board[r*10 + c] occupied?
        lda A1
        asl a
        sta S0
        asl a
        asl a
        clc
        adc S0                  ; r*10
        clc
        adc A0
        tax
        sep #$20
.a8
        lda v_dt_board,x
        rep #$20
.a16
        and #$00FF
        bne @no
@next:  inc LC0
        lda LC0
        cmp #4
        bcc @cell
        lda #1
        rts
@no:    lda #0
        rts
.endproc

; dt_interval -> A = gravity interval in ticks (60 Hz)
.proc dt_interval
.a16
.i16
        lda #18
        sec
        sbc v_dt_level
        cmp #2
        bcs @ok
        lda #2
@ok:    sta S0
        ; *10/3
        asl a                   ; *2
        sta S1
        asl a
        asl a                   ; *8
        clc
        adc S1                  ; *10
        sta S0
        lda #3
        sta S1
        jsr div16
        rts
.endproc

; dt_spawn - next becomes current; game over if it cannot drop
.proc dt_spawn
.a16
.i16
        lda v_dt_next
        sta v_dt_piece
        jsr dt_rand7
        sta v_dt_next
        stz v_dt_rot
        lda #3
        sta v_dt_col
        lda #$FFFF
        sta v_dt_row
        lda v_frame
        sta v_dt_last
        lda v_dt_piece
        sta S4
        stz S5
        lda #3
        sta S6
        stz S7
        jsr dt_fits
        cmp #0
        bne @ok
        lda #3
        sta v_dt_state          ; game over
@ok:    rts
.endproc

; dt_clear_lines - collapse full rows, score them
.proc dt_clear_lines
.a16
.i16
        stz LC1                 ; cleared count
        stz S5                  ; row
@row:   lda S5
        cmp #DT_ROWS
        bcc :+
        jmp @scored
:
        ; full?
        stz S6                  ; col
@col:   lda S5
        asl a
        sta S0
        asl a
        asl a
        clc
        adc S0                  ; row*10
        clc
        adc S6
        tax
        sep #$20
.a8
        lda v_dt_board,x
        rep #$20
.a16
        and #$00FF
        beq @notfull
        inc S6
        lda S6
        cmp #DT_COLS
        bcc @col
        ; row full: shift rows [1..row] down (dst=row r1, src=row r1-1), count++
        inc LC1
        lda S5
        sta S1                  ; r1 = row
@shift: lda S1
        beq @top
        ; dst base = r1*10, src base = dst - 10
        lda S1
        asl a
        sta S2
        asl a
        asl a
        clc
        adc S2                  ; r1*10
        sta S2                  ; dst base
        sec
        sbc #DT_COLS
        sta S3                  ; src base
        ldy #$0000
@cp:    tya
        clc
        adc S3
        tax
        sep #$20
.a8
        lda v_dt_board,x        ; src byte
        pha
        rep #$20
.a16
        tya
        clc
        adc S2
        tax
        sep #$20
.a8
        pla
        sta v_dt_board,x        ; dst byte
        rep #$20
.a16
        iny
        cpy #DT_COLS
        bcc @cp
        dec S1
        bra @shift
@top:   ; clear top row
        ldx #0
@z:     sep #$20
.a8
        stz v_dt_board,x
        rep #$20
.a16
        inx
        cpx #DT_COLS
        bcc @z
        ; re-test the same row (it now holds the row above)
        jmp @row
@notfull:
        inc S5
        jmp @row
@scored:
        lda LC1
        bne :+
        jmp @done
:
        ; score += linescore[cleared] * (level+1)
        asl a
        tax
        lda dt_linescore,x
        sta S0
        lda v_dt_level
        inc a
        sta S1
        ; S0 * S1 (small)
        stz S2
@mul:   lda S1
        beq @muldone
        lda S2
        clc
        adc S0
        sta S2
        dec S1
        bra @mul
@muldone:
        lda v_dt_score
        clc
        adc S2
        sta v_dt_score
        lda v_dt_lines
        clc
        adc LC1
        sta v_dt_lines
        ; level = lines/10 + 1, capped 15
        sta S0
        lda #10
        sta S1
        jsr div16
        inc a
        cmp #16
        bcc @lvok
        lda #15
@lvok:  sta v_dt_level
@done:  rts
.endproc

; dt_lock - stamp piece into board, clear lines, respawn
.proc dt_lock
.a16
.i16
        lda v_dt_piece
        sta S4
        asl a
        asl a
        clc
        adc v_dt_rot
        asl a
        asl a
        asl a
        clc
        adc #.loword(dt_shapes)
        sta P0
        ; colour = dt_colors[piece] + 1
        ldx S4
        sep #$20
.a8
        lda dt_colors,x
        inc a
        sta S3                  ; stored colour
        rep #$20
.a16
        ldy #$0000
        stz LC0
@cell:  sep #$20
.a8
        lda (P0),y
        rep #$20
.a16
        and #$00FF
        cmp #$80
        bcc @cp
        ora #$FF00
@cp:    clc
        adc v_dt_col
        sta A0                  ; c
        iny
        sep #$20
.a8
        lda (P0),y
        rep #$20
.a16
        and #$00FF
        cmp #$80
        bcc @rp
        ora #$FF00
@rp:    clc
        adc v_dt_row
        sta A1                  ; r
        iny
        lda A1
        bmi @next
        cmp #DT_ROWS
        bcs @next
        lda A1
        asl a
        sta S0
        asl a
        asl a
        clc
        adc S0
        clc
        adc A0
        tax
        sep #$20
.a8
        lda S3
        sta v_dt_board,x
        rep #$20
.a16
@next:  inc LC0
        lda LC0
        cmp #4
        bcc @cell
        jsr dt_clear_lines
        jsr dt_spawn
        rts
.endproc

; dt_step - gravity: move down or lock
.proc dt_step
.a16
.i16
        lda v_dt_piece
        sta S4
        lda v_dt_rot
        sta S5
        lda v_dt_col
        sta S6
        lda v_dt_row
        inc a
        sta S7
        jsr dt_fits
        cmp #0
        beq @lock
        inc v_dt_row
        rts
@lock:  jsr dt_lock
        rts
.endproc

; dt_cell: A0=col A1=row, A4=donor, S2=window offset -> draw one board cell
.proc dt_cell
.a16
.i16
        ldx S2
        lda A0
        clc
        adc v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda A1
        clc
        adc v_wintab+WY,x
        clc
        adc #2
        sta A1
        lda #1
        sta A2
        lda #1
        sta A3
        jsr gfill
        rts
.endproc

; dostris_draw - S2 = window entry offset
.proc dostris_draw
.a16
.i16
        ; black slab behind the board (12x22 at wx+1,wy+1)
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        lda #12
        sta A2
        lda #22
        sta A3
        lda #30                 ; black
        sta A4
        jsr gfill
        ; settled cells
        stz LC1                 ; row
@brow:  stz LC0                 ; col
@bcol:  lda LC1
        asl a
        sta S0
        asl a
        asl a
        clc
        adc S0                  ; row*10
        clc
        adc LC0
        tax
        sep #$20
.a8
        lda v_dt_board,x
        rep #$20
.a16
        and #$00FF
        beq @empty
        dec a
        sta A4                  ; donor colour
        lda LC0
        sta A0
        lda LC1
        sta A1
        jsr dt_cell
@empty: inc LC0
        lda LC0
        cmp #DT_COLS
        bcc @bcol
        inc LC1
        lda LC1
        cmp #DT_ROWS
        bcc @brow
        ; falling piece (playing or paused)
        lda v_dt_state
        cmp #1
        beq @piece
        cmp #2
        bne @panel
@piece: lda v_dt_piece
        sta S4
        asl a
        asl a
        clc
        adc v_dt_rot
        asl a
        asl a
        asl a
        clc
        adc #.loword(dt_shapes)
        sta P0
        ldx S4
        sep #$20
.a8
        lda dt_colors,x
        sta S5                  ; donor colour
        rep #$20
.a16
        ldy #$0000
        stz LC0
@pcell: sep #$20
.a8
        lda (P0),y
        rep #$20
.a16
        and #$00FF
        cmp #$80
        bcc @pc
        ora #$FF00
@pc:    clc
        adc v_dt_col
        sta A0
        iny
        sep #$20
.a8
        lda (P0),y
        rep #$20
.a16
        and #$00FF
        cmp #$80
        bcc @pr
        ora #$FF00
@pr:    clc
        adc v_dt_row
        sta A1
        iny
        lda A1
        bmi @pskip
        lda S5
        sta A4
        jsr dt_cell
@pskip: inc LC0
        lda LC0
        cmp #4
        bcc @pcell
@panel: jsr dostris_panel
        rts
.endproc

; dostris_panel - the text panel (score/lines/level/state) at wx+15
.proc dostris_panel
.a16
.i16
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #15
        sta S6                  ; panel x
        lda v_wintab+WY,x
        clc
        adc #2
        sta S7                  ; panel y
        ; title
        lda #.loword(str_dt_title)
        sta P0
        lda S6
        sta A0
        lda S7
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        ; score
        lda S7
        clc
        adc #2
        sta S7
        lda #.loword(str_dt_score)
        sta P0
        lda v_dt_score
        jsr dt_labelval
        ; lines
        inc S7
        lda #.loword(str_dt_lines)
        sta P0
        lda v_dt_lines
        jsr dt_labelval
        ; level
        inc S7
        lda #.loword(str_dt_level)
        sta P0
        lda v_dt_level
        jsr dt_labelval
        ; state line
        lda S7
        clc
        adc #2
        sta S7
        lda v_dt_state
        cmp #0
        beq @menu
        cmp #3
        beq @over
        cmp #2
        bne :+
        jmp @pause
:
        rts
@menu:  lda #.loword(str_dt_newg)
        bra @stext
@pause: lda #.loword(str_dt_paused)
        bra @stext
@over:  lda #.loword(str_dt_over)
@stext: sta P0
        lda S6
        sta A0
        lda S7
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
        rts
.endproc

; dt_labelval - P0 = label, A = value: draw label@(S6,S7) + value@(S6+7,S7)
.proc dt_labelval
.a16
.i16
        pha                     ; save value
        lda S6                  ; draw label at (S6,S7)
        sta A0
        lda S7
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str            ; clobbers S0/S1
        pla                     ; value -> S0 for fmt_dec
        sta S0
        lda S6                  ; fmt_dec clobbers S5/S6 - preserve panel x/y
        pha
        lda S7
        pha
        jsr fmt_dec             ; v_numbuf
        pla
        sta S7
        pla
        sta S6
        lda #.loword(v_numbuf)
        sta P0
        lda S6
        clc
        adc #7
        sta A0
        lda S7
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        rts
.endproc

; dostris_new - reset and start
.proc dostris_new
.a16
.i16
        ldx #0
@clr:   sep #$20
.a8
        stz v_dt_board,x
        rep #$20
.a16
        inx
        cpx #(DT_ROWS*DT_COLS)
        bcc @clr
        stz v_dt_score
        stz v_dt_lines
        lda #1
        sta v_dt_level
        lda v_frame
        ora #1
        sta v_dt_seedl
        lda v_secs
        ora #$0100
        sta v_dt_seedh
        jsr dt_rand7
        sta v_dt_next
        lda #1
        sta v_dt_state
        jsr dt_spawn
        rts
.endproc

; dostris_key - A1 = event data
.proc dostris_key
.a16
.i16
        lda A1
        and #$00FF
        sta S0                  ; ascii
        lda A1
        xba
        and #$00FF
        sta S1                  ; raw
        lda S0
        cmp #'n'
        bne :+
        jmp @new
:
        cmp #'p'
        bne :+
        jmp @pause
:
        lda v_dt_state
        cmp #1
        beq :+
        jmp @rd
:
        lda S1
        cmp #$4F
        bne :+
        jmp @left
:
        cmp #$4E
        bne :+
        jmp @right
:
        cmp #$4C
        bne :+
        jmp @rot
:
        cmp #$4D
        bne :+
        jmp @soft
:
        lda S0
        cmp #32
        bne :+
        jmp @hard
:
        rts
@new:   jsr dostris_new
        jmp @rd
@pause: lda v_dt_state
        cmp #1
        bne :+
        jmp @topause
:
        cmp #2
        beq :+
        jmp @rd
:
        lda #1
        sta v_dt_state
        lda v_frame
        sta v_dt_last
        jmp @rd
@topause:
        lda #2
        sta v_dt_state
        jmp @rd
@left:  lda v_dt_piece
        sta S4
        lda v_dt_rot
        sta S5
        lda v_dt_col
        dec a
        sta S6
        lda v_dt_row
        sta S7
        jsr dt_fits
        cmp #0
        bne :+
        jmp @rd
:
        dec v_dt_col
        jmp @rd
@right: lda v_dt_piece
        sta S4
        lda v_dt_rot
        sta S5
        lda v_dt_col
        inc a
        sta S6
        lda v_dt_row
        sta S7
        jsr dt_fits
        cmp #0
        bne :+
        jmp @rd
:
        inc v_dt_col
        jmp @rd
@rot:   lda v_dt_piece
        sta S4
        lda v_dt_rot
        inc a
        and #3
        sta S5
        lda v_dt_col
        sta S6
        lda v_dt_row
        sta S7
        jsr dt_fits
        cmp #0
        bne :+
        jmp @rd
:
        lda v_dt_rot
        inc a
        and #3
        sta v_dt_rot
        jmp @rd
@soft:  lda v_dt_piece
        sta S4
        lda v_dt_rot
        sta S5
        lda v_dt_col
        sta S6
        lda v_dt_row
        inc a
        sta S7
        jsr dt_fits
        cmp #0
        beq @softlock
        inc v_dt_row
        inc v_dt_score
        lda v_frame
        sta v_dt_last
        jmp @rd
@softlock:
        jsr dt_lock
        jmp @rd
@hard:  lda v_dt_piece
        sta S4
        lda v_dt_rot
        sta S5
        lda v_dt_col
        sta S6
@hloop: lda v_dt_row
        inc a
        sta S7
        ; refresh args (dt_fits preserves them but be safe)
        lda v_dt_piece
        sta S4
        lda v_dt_rot
        sta S5
        lda v_dt_col
        sta S6
        jsr dt_fits
        cmp #0
        beq @hdone
        inc v_dt_row
        lda v_dt_score
        clc
        adc #2
        sta v_dt_score
        bra @hloop
@hdone: jsr dt_lock
@rd:    jsr redraw_topmost
        rts
.endproc

; dostris_tick - gravity from the main loop (topmost + playing only)
.proc dostris_tick
.a16
.i16
        lda v_dt_state
        cmp #1
        bne @out
        lda v_frame
        sec
        sbc v_dt_last
        sta S0
        jsr dt_interval
        cmp S0
        beq @step
        bcs @out
@step:  lda v_frame
        sta v_dt_last
        jsr dt_step
        jsr redraw_topmost
@out:   rts
.endproc

; ----------------------------------------------------------------------------
; OutLast (proc 5) - a recognizable port: a linear-perspective scrolling road
; (converging bands, the same trick the Genesis title screen uses - divide-
; free), steerable car, speed/score/time HUD, countdown. DEVIATION from the
; Genesis version: no per-row 1/z raster, road curve, or oncoming traffic
; (the SNES has no fast software 16/16 divide; flagged in HANDOFF).
; ----------------------------------------------------------------------------
OL_FCOLS = 28
OL_FROWS = 20
OL_HORIZ = 3

; ol_fillrow: A0=cx0 A1=row A2=cx1(excl) A4=donor, S2=window. Clips [0,FCOLS).
.proc ol_fillrow
.a16
.i16
        lda A0
        bpl @c0
        stz A0
@c0:    lda A2
        cmp #(OL_FCOLS+1)
        bcc @c1
        lda #OL_FCOLS
        sta A2
@c1:    lda A2
        cmp A0
        bcc @skip
        beq @skip
        sec
        sbc A0                  ; w
        sta S3
        ldx S2
        lda A0
        clc
        adc v_wintab+WX,x
        inc a
        sta A0
        lda A1
        clc
        adc v_wintab+WY,x
        clc
        adc #2
        sta A1
        lda S3
        sta A2
        lda #1
        sta A3
        jsr gfill
@skip:  rts
.endproc

; outlast_new
.proc outlast_new
.a16
.i16
        lda #112
        sta v_ol_x
        stz v_ol_speed
        stz v_ol_z
        stz v_ol_score
        lda #60
        sta v_ol_time
        stz v_ol_crash
        lda #1
        sta v_ol_state
        lda v_frame
        sta v_ol_last
        sta v_ol_lastsec
        rts
.endproc

; ol_band - draw one road band: A1=row, half cells in A2tmp. Helper inline below.

; outlast_draw - S2 = window offset
.proc outlast_draw
.a16
.i16
        ; HUD bar (content row +1, white)
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        lda v_wintab+WW,x
        sec
        sbc #2
        sta A2
        lda #1
        sta A3
        lda #(ATTR_INV+T_SOLBG)
        sta A4
        jsr fill_cells
        ; sky rows 0..HORIZ-1, haze row HORIZ
        stz LC1                 ; row
@sky:   lda LC1
        cmp #OL_HORIZ
        bcs @haze
        stz A0
        lda LC1
        sta A1
        lda #OL_FCOLS
        sta A2
        lda #11                 ; sky donor
        sta A4
        jsr ol_fillrow
        inc LC1
        bra @sky
@haze:  stz A0
        lda #OL_HORIZ
        sta A1
        lda #OL_FCOLS
        sta A2
        lda #12
        sta A4
        jsr ol_fillrow
        ; road bands rows HORIZ+1 .. FROWS-1; half grows with the row
        lda #(OL_HORIZ+1)
        sta LC1
@band:  lda LC1
        cmp #OL_FROWS
        bcc :+
        jmp @car
:
        ; half cells = (row - HORIZ)
        lda LC1
        sec
        sbc #OL_HORIZ
        sta S5                  ; half
        ; centre cell = 18 (148px / 8)
        ; left grass [0, 18-half) grass-A/B by (row+z) parity
        lda LC1
        clc
        adc v_ol_z
        and #1
        bne @gB
        lda #13
        bra @gset
@gB:    lda #14
@gset:  sta S6                  ; grass donor
        ; left grass
        stz A0
        lda LC1
        sta A1
        lda #14
        sec
        sbc S5
        sta A2                  ; lc
        lda S6
        sta A4
        jsr ol_fillrow
        ; road [lc, rc)
        lda #14
        sec
        sbc S5
        sta A0                  ; lc
        lda #14
        clc
        adc S5
        inc a
        sta A2                  ; rc
        lda LC1
        sta A1
        lda #15                 ; road grey
        sta A4
        jsr ol_fillrow
        ; right grass [rc, FCOLS)
        lda #14
        clc
        adc S5
        inc a
        sta A0
        lda #OL_FCOLS
        sta A2
        lda LC1
        sta A1
        lda S6
        sta A4
        jsr ol_fillrow
        ; centre stripe on alternating (row+z) rows
        lda LC1
        clc
        adc v_ol_z
        and #1
        beq @nostripe
        lda #14
        sta A0
        lda #19
        sta A2
        lda LC1
        sta A1
        lda #20                 ; stripe
        sta A4
        jsr ol_fillrow
@nostripe:
        inc LC1
        jmp @band
@car:   ; player car at v_ol_x (hidden while crash-flashing)
        lda v_ol_crash
        and #4
        bne @overlay
        lda v_ol_x
        lsr a
        lsr a
        lsr a
        sta S5                  ; car centre cell
        ; body rows 19-20
        lda S5
        dec a
        sta A0
        lda #19
        sta A1
        lda S5
        clc
        adc #2
        sta A2
        lda #21                 ; car red
        sta A4
        jsr ol_fillrow
        lda S5
        dec a
        sta A0
        lda #20
        sta A1
        lda S5
        clc
        adc #2
        sta A2
        lda #21
        sta A4
        jsr ol_fillrow
        ; windshield row 19
        lda S5
        sta A0
        lda #19
        sta A1
        lda S5
        inc a
        sta A2
        lda #22
        sta A4
        jsr ol_fillrow
@overlay:
        lda v_ol_state
        cmp #2
        bne @hud
        lda #.loword(str_ol_over)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #14
        sta A0
        lda v_wintab+WY,x
        clc
        adc #11
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
@hud:   ; HUD text: Spd/Score/Time
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta S6
        lda v_wintab+WY,x
        inc a
        sta S7
        lda #.loword(str_ol_spd)
        sta P0
        lda v_ol_speed
        jsr dt_labelval2
        lda #.loword(str_ol_time)
        sta P0
        lda v_ol_time
        jsr dt_labelval2b
        rts
.endproc

; dt_labelval2 - like dt_labelval but draws label@(S6,S7), value@(S6+5,S7)
.proc dt_labelval2
.a16
.i16
        pha
        lda S6
        sta A0
        lda S7
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
        pla
        sta S0
        lda S6
        pha
        lda S7
        pha
        jsr fmt_dec
        pla
        sta S7
        pla
        sta S6
        lda #.loword(v_numbuf)
        sta P0
        lda S6
        clc
        adc #5
        sta A0
        lda S7
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
        rts
.endproc

; dt_labelval2b - second HUD field, advancing S6 by 14 first
.proc dt_labelval2b
.a16
.i16
        pha
        lda S6
        clc
        adc #14
        sta S6
        pla
        jsr dt_labelval2
        rts
.endproc

; outlast_key - A1 = event data
.proc outlast_key
.a16
.i16
        lda A1
        and #$00FF
        sta S0
        lda A1
        xba
        and #$00FF
        sta S1
        lda S0
        cmp #'n'
        beq @new
        lda v_ol_state
        cmp #1
        bne @rd
        lda v_ol_crash
        bne @rd
        lda S1
        cmp #$4F
        beq @left
        cmp #$4E
        beq @right
        cmp #$4C
        beq @accel
        cmp #$4D
        beq @brake
        rts
@new:   jsr outlast_new
        jmp @rd
@left:  lda v_ol_x
        sec
        sbc #8
        cmp #20
        bcs @lset
        lda #20
@lset:  sta v_ol_x
        jmp @rd
@right: lda v_ol_x
        clc
        adc #8
        cmp #(208+1)
        bcc @rset
        lda #208
@rset:  sta v_ol_x
        jmp @rd
@accel: lda v_ol_speed
        clc
        adc #4
        cmp #(60+1)
        bcc @aset
        lda #60
@aset:  sta v_ol_speed
        jmp @rd
@brake: lda v_ol_speed
        sec
        sbc #8
        bcs @bset
        lda #0
@bset:  sta v_ol_speed
@rd:    jsr redraw_topmost
        rts
.endproc

; outlast_tick - physics + countdown (topmost + playing)
.proc outlast_tick
.a16
.i16
        lda v_ol_state
        cmp #1
        bne @out
        lda v_frame
        sec
        sbc v_ol_last
        cmp #10
        bcc @out
        lda v_frame
        sta v_ol_last
        ; crashed countdown
        lda v_ol_crash
        beq @alive
        dec a
        sta v_ol_crash
        bne @timer
        lda #112
        sta v_ol_x
        lda #5
        sta v_ol_speed
        bra @timer
@alive: ; auto-accelerate
        lda v_ol_speed
        cmp #60
        bcs @spdok
        inc v_ol_speed
@spdok: ; advance camera (scrolls the road) + score
        lda v_ol_z
        clc
        adc v_ol_speed
        and #$003F              ; cheap scroll counter
        sta v_ol_z
        lda v_ol_speed
        lsr a
        lsr a
        clc
        adc v_ol_score
        sta v_ol_score
@timer: ; one-second countdown (every 6 ticks here -> ~ once/sec at 10-tick step)
        lda v_frame
        sec
        sbc v_ol_lastsec
        cmp #60
        bcc @draw
        lda v_frame
        sta v_ol_lastsec
        lda v_ol_time
        beq @over
        dec v_ol_time
        bne @draw
@over:  lda v_ol_time
        bne @draw
        lda #2
        sta v_ol_state
@draw:  jsr redraw_topmost
@out:   rts
.endproc

; game_tick - main-loop dispatch to the topmost game's tick
.proc game_tick
.a16
.i16
        jsr is_game_topmost
        bcc @out
        lda v_zcount
        dec a
        jsr zent_x
        sep #$20
.a8
        lda v_wintab+WPROC,x
        rep #$20
.a16
        and #$00FF
        cmp #4
        bne @notdt
        jsr dostris_tick
        rts
@notdt: cmp #5
        bne @out
        jsr outlast_tick
@out:   rts
.endproc

; game_key - A = proc, A1 = event data
.proc game_key
.a16
.i16
        cmp #4
        beq @dt
        rts
@dt:    jsr dostris_key
        rts
.endproc

; ---- data ----
.segment "RODATA"
str_t_dostris: .byte "Dostris", 0
name_dostris:  .byte "Dostris", 0
str_t_outlast: .byte "OutLast", 0
name_outlast:  .byte "OutLast", 0
str_ol_over:   .byte "GAME OVER - n:new", 0
str_ol_spd:    .byte "Spd:", 0
str_ol_time:   .byte "Time:", 0
str_dt_title:  .byte "DOSTRIS", 0
str_dt_score:  .byte "Score:", 0
str_dt_lines:  .byte "Lines:", 0
str_dt_level:  .byte "Level:", 0
str_dt_newg:   .byte "X:new game", 0
str_dt_paused: .byte "PAUSED", 0
str_dt_over:   .byte "GAME OVER", 0

dt_shapes:
        .byte 0,1,1,1,2,1,3,1, 2,0,2,1,2,2,2,3, 0,2,1,2,2,2,3,2, 1,0,1,1,1,2,1,3
        .byte 1,0,2,0,1,1,2,1, 1,0,2,0,1,1,2,1, 1,0,2,0,1,1,2,1, 1,0,2,0,1,1,2,1
        .byte 1,0,0,1,1,1,2,1, 0,0,0,1,1,1,0,2, 0,0,1,0,2,0,1,1, 1,0,0,1,1,1,1,2
        .byte 1,0,2,0,0,1,1,1, 0,0,0,1,1,1,1,2, 1,0,2,0,0,1,1,1, 0,0,0,1,1,1,1,2
        .byte 0,0,1,0,1,1,2,1, 1,0,0,1,1,1,0,2, 0,0,1,0,1,1,2,1, 1,0,0,1,1,1,0,2
        .byte 0,0,0,1,1,1,2,1, 0,0,1,0,0,1,0,2, 0,0,1,0,2,0,2,1, 1,0,1,1,0,2,1,2
        .byte 2,0,0,1,1,1,2,1, 0,0,0,1,0,2,1,2, 0,0,1,0,2,0,0,1, 0,0,1,0,1,1,1,2
dt_colors:
        .byte 4,5,6,7,8,9,10
dt_linescore:
        .word 0, 40, 100, 300, 1200

; gcol_tab: donor colour index 0..31 -> SNES cell name word.
; ATTR_ACC + game tile = palette-2 game colours (idx 4-15);
; ATTR_KEY + game tile = palette-3 (OutLast) colours.
gcol_tab:
        .word ATTR_NORM+T_SOLBG          ; 0  UI blue
        .word ATTR_ACC+T_GSOL+0          ; 1  cyan
        .word ATTR_ACC+T_GSOL+7          ; 2  pink/magenta
        .word ATTR_ACC+T_GSOL+9          ; 3  white
        .word ATTR_ACC+T_GSOL+0          ; 4  I cyan
        .word ATTR_ACC+T_GSOL+1          ; 5  O yellow
        .word ATTR_ACC+T_GSOL+2          ; 6  T purple
        .word ATTR_ACC+T_GSOL+3          ; 7  S green
        .word ATTR_ACC+T_GSOL+4          ; 8  Z red
        .word ATTR_ACC+T_GSOL+5          ; 9  J blue
        .word ATTR_ACC+T_GSOL+6          ; 10 L orange
        .word ATTR_KEY+T_GSOL+0          ; 11 sky
        .word ATTR_KEY+T_GSOL+1          ; 12 haze
        .word ATTR_KEY+T_GSOL+2          ; 13 grass A
        .word ATTR_KEY+T_GSOL+3          ; 14 grass B
        .word ATTR_KEY+T_GSOL+4          ; 15 road grey
        .word ATTR_KEY+T_GSOL+4          ; 16 road
        .word ATTR_ACC+T_GSOL+9          ; 17 white
        .word ATTR_ACC+T_GSOL+11         ; 18 black
        .word ATTR_ACC+T_GSOL+0          ; 19 cyan
        .word ATTR_KEY+T_GSOL+5          ; 20 stripe
        .word ATTR_ACC+T_GSOL+4          ; 21 car red
        .word ATTR_KEY+T_GSOL+6          ; 22 windshield
        .word ATTR_KEY+T_GSOL+7          ; 23 wheel
        .word ATTR_ACC+T_GSOL+9          ; 24 white
        .word ATTR_KEY+T_GSOL+9          ; 25 traffic
        .word ATTR_KEY+T_GSOL+5          ; 26
        .word ATTR_KEY+T_GSOL+2          ; 27
        .word ATTR_ACC+T_GSOL+7          ; 28 pinky pink
        .word ATTR_ACC+T_GSOL+8          ; 29 fright blue
        .word ATTR_ACC+T_GSOL+11         ; 30 black
        .word ATTR_KEY+T_GSOL+5          ; 31
