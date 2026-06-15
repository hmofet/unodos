; ============================================================================
; UnoDOS/Apple IIGS - Dostris (proc 5): a colour Tetris on Super Hi-Res.
;
; 10x18 well of 8x8 cells, 7 tetrominoes x 4 rotations, gravity on a per-frame
; tick (game_tick), keyboard controls, line clear + scoring.  Pieces use the
; SHR game-colour palette entries (2,7,3,10,11,13,9) - the kind of 16-colour
; content the plain Apple II port re-themes to 1-bit.  Board is a 180-byte
; bank-0 buffer; state is in VARS above the FS dir list.
; ============================================================================

DTBOARD     = $9A00           ; 10*18 = 180 bytes (bank 0, after SECBUF)
BW          = 10
BH          = 18
DROP_FRAMES = 24              ; gravity period

; ---- Dostris state (VARS, above v_dir_list which ends at $440) ----
v_dt_state  = VARS+$440       ; 0 play, 1 over
v_dt_piece  = VARS+$442
v_dt_next   = VARS+$444
v_dt_rot    = VARS+$446
v_dt_x      = VARS+$448
v_dt_y      = VARS+$44A
v_dt_score  = VARS+$44C
v_dt_lines  = VARS+$44E
v_dt_seed   = VARS+$450
v_dt_drop   = VARS+$452

; ---- Dostris zp scratch (above the FS temps at $54-$66) ----
DT0 = $68
DT1 = $6A
DT2 = $6C
DT3 = $6E
DT4 = $70
DTCELLS = $72                 ; 4-byte cell buffer
DTP0 = $76
DTP1 = $78
DTR0 = $7A                    ; private to row_base_idx (callers keep DT1/DT2)
DTR1 = $7C

; row_base_idx: A = row -> A = row*10 (board index, no base).
.a16
.i16
row_base_idx:
        sta DTR0
        asl a                  ; *2
        sta DTR1
        lda DTR0
        asl a
        asl a
        asl a                  ; *8
        clc
        adc DTR1               ; *10
        rts

; row_addr: A = row -> A = DTBOARD + row*10 (absolute board-row address).
.a16
.i16
row_addr:
        jsr row_base_idx
        clc
        adc #DTBOARD
        rts

; rand_piece: -> A = 0..6 (mixes v_dt_seed with v_frame).
.a16
.i16
rand_piece:
        lda v_dt_seed
        asl a
        asl a
        clc
        adc v_dt_seed
        clc
        adc v_frame
        inc a
        sta v_dt_seed
        and #$00FF
@m:     cmp #7
        bcc @done
        sec
        sbc #7
        bra @m
@done:  rts

; collide: A0=piece A1=rot A2=x A3=y -> carry set if the piece collides.
.a16
.i16
collide:
        lda A0
        asl a
        asl a
        asl a
        asl a                  ; piece*16
        sta DT0
        lda A1
        asl a
        asl a                  ; rot*4
        clc
        adc DT0
        tax                    ; X = byte offset into pieces
        sep #$20
        ldy #0
@rd:    lda f:pieces,x
        sta DTCELLS,y
        inx
        iny
        cpy #4
        bcc @rd
        rep #$20
        ldy #0
@cell:  sep #$20
        lda DTCELLS,y
        rep #$20
        and #$00FF
        pha
        lsr a
        lsr a
        lsr a
        lsr a                  ; dx
        clc
        adc A2
        sta DT1                ; cell x
        pla
        and #$000F             ; dy
        clc
        adc A3
        sta DT2                ; cell y
        lda DT1
        bmi @hit
        cmp #BW
        bcs @hit
        lda DT2
        cmp #BH
        bcs @hit
        lda DT2
        jsr row_base_idx
        clc
        adc DT1
        tax                    ; board index
        sep #$20
        lda DTBOARD,x
        rep #$20
        and #$00FF
        bne @hit
        iny
        cpy #4
        bcc @cell
        clc
        rts
@hit:   sec
        rts

; lock_piece: write the current piece's cells into the board.
.a16
.i16
lock_piece:
        lda v_dt_piece
        asl a
        asl a
        asl a
        asl a
        sta DT0
        lda v_dt_rot
        asl a
        asl a
        clc
        adc DT0
        tax
        sep #$20
        ldy #0
@rd:    lda f:pieces,x
        sta DTCELLS,y
        inx
        iny
        cpy #4
        bcc @rd
        rep #$20
        lda v_dt_piece
        tax
        sep #$20
        lda f:piece_colors,x
        rep #$20
        and #$00FF
        sta DT3                ; colour
        ldy #0
@cell:  sep #$20
        lda DTCELLS,y
        rep #$20
        and #$00FF
        pha
        lsr a
        lsr a
        lsr a
        lsr a
        clc
        adc v_dt_x
        sta DT1                ; cell x
        pla
        and #$000F
        clc
        adc v_dt_y
        jsr row_base_idx
        clc
        adc DT1
        tax
        sep #$20
        lda DT3
        sta DTBOARD,x
        rep #$20
        iny
        cpy #4
        bcc @cell
        rts

; shift_down: DT0 = a cleared row -> copy rows above it down, clear row 0.
.a16
.i16
shift_down:
        lda DT0
        sta DT4
@r:     lda DT4
        beq @top
        lda DT4
        jsr row_addr
        sta DTP1               ; dest
        lda DT4
        dec a
        jsr row_addr
        sta DTP0               ; src
        ldy #0
        sep #$20
@cp:    lda (DTP0),y
        sta (DTP1),y
        iny
        cpy #BW
        bcc @cp
        rep #$20
        dec DT4
        bra @r
@top:   lda #0
        jsr row_addr
        sta DTP1
        ldy #0
        sep #$20
@cl:    lda #0
        sta (DTP1),y
        iny
        cpy #BW
        bcc @cl
        rep #$20
        rts

; clear_lines: remove full rows (bottom-up), shifting + scoring.
.a16
.i16
clear_lines:
        lda #(BH-1)
        sta DT0
@row:   lda DT0
        bmi @done
        lda DT0
        jsr row_base_idx
        tax                    ; row base index
        lda #1
        sta DT3                ; assume full
        ldy #0
@chk:   sep #$20
        lda DTBOARD,x
        rep #$20
        and #$00FF
        bne @nz
        stz DT3
@nz:    inx
        iny
        cpy #BW
        bcc @chk
        lda DT3
        beq @next
        jsr shift_down         ; DT0 still = the cleared row
        lda v_dt_score
        clc
        adc #100
        sta v_dt_score
        inc v_dt_lines
        bra @row               ; re-check the same row (now shifted)
@next:  dec DT0
        bra @row
@done:  rts

; spawn_piece: next -> current, roll a new next, reset position; over on hit.
.a16
.i16
spawn_piece:
        lda v_dt_next
        sta v_dt_piece
        jsr rand_piece
        sta v_dt_next
        stz v_dt_rot
        lda #3
        sta v_dt_x
        stz v_dt_y
        lda v_dt_piece
        sta A0
        lda v_dt_rot
        sta A1
        lda v_dt_x
        sta A2
        lda v_dt_y
        sta A3
        jsr collide
        bcc @ok
        lda #1
        sta v_dt_state         ; game over
@ok:    rts

; dostris_start: new game.
.a16
.i16
dostris_start:
        ldx #0
        sep #$20
@cl:    lda #0
        sta DTBOARD,x
        inx
        cpx #(BW*BH)
        bcc @cl
        rep #$20
        stz v_dt_score
        stz v_dt_lines
        stz v_dt_state
        stz v_dt_drop
        lda v_frame
        ora #1
        sta v_dt_seed
        jsr rand_piece
        sta v_dt_next
        jsr spawn_piece
        rts

; try_move: A0=piece A1=rot A2=x A3=y -> apply if no collision, then redraw.
.a16
.i16
try_move:
        jsr collide
        bcs @no
        lda A1
        sta v_dt_rot
        lda A2
        sta v_dt_x
        lda A3
        sta v_dt_y
        jsr redraw_topmost
@no:    rts

; drop_one: gravity step - move down or lock+clear+spawn.
.a16
.i16
drop_one:
        lda v_dt_piece
        sta A0
        lda v_dt_rot
        sta A1
        lda v_dt_x
        sta A2
        lda v_dt_y
        inc a
        sta A3
        jsr collide
        bcs @lock
        inc v_dt_y
        rts
@lock:  jsr lock_piece
        jsr clear_lines
        jsr spawn_piece
        rts

; game_tick: per-frame; advances gravity while a Dostris window is open.
.a16
.i16
game_tick:
        stz S5
@scan:  lda S5
        jsr ent_x
        sep #$20
        lda v_wintab+WSTATE,x
        beq @nf
        lda v_wintab+WPROC,x
        cmp #5
        bne @nf
        rep #$20
        bra @found
@nf:    rep #$20
        inc S5
        lda S5
        cmp #MAXWIN
        bcc @scan
        rts                    ; no Dostris window
@found: lda v_dt_state
        bne @out               ; game over: idle
        lda v_dt_drop
        inc a
        sta v_dt_drop
        cmp #DROP_FRAMES
        bcc @out
        stz v_dt_drop
        jsr drop_one
        jsr redraw_topmost
@out:   rts

; dostris_key: S0 = ascii.
.a16
.i16
dostris_key:
        lda v_dt_state
        beq @play
        jsr dostris_start      ; over: any key restarts
        jsr redraw_topmost
        rts
@play:  lda S0
        cmp #$08
        beq @left
        cmp #$15
        beq @right
        cmp #$0B
        beq @rot
        cmp #$0A
        beq @down
        cmp #$20
        beq @hard
        rts
@left:  lda v_dt_piece
        sta A0
        lda v_dt_rot
        sta A1
        lda v_dt_x
        dec a
        sta A2
        lda v_dt_y
        sta A3
        jmp try_move
@right: lda v_dt_piece
        sta A0
        lda v_dt_rot
        sta A1
        lda v_dt_x
        inc a
        sta A2
        lda v_dt_y
        sta A3
        jmp try_move
@rot:   lda v_dt_piece
        sta A0
        lda v_dt_rot
        inc a
        and #3
        sta A1
        lda v_dt_x
        sta A2
        lda v_dt_y
        sta A3
        jmp try_move
@down:  lda v_dt_piece
        sta A0
        lda v_dt_rot
        sta A1
        lda v_dt_x
        sta A2
        lda v_dt_y
        inc a
        sta A3
        jmp try_move
@hard:  lda v_dt_piece
        sta A0
        lda v_dt_rot
        sta A1
        lda v_dt_x
        sta A2
        lda v_dt_y
        inc a
        sta A3
        jsr collide
        bcs @hlock
        inc v_dt_y
        bra @hard
@hlock: jsr lock_piece
        jsr clear_lines
        jsr spawn_piece
        jsr redraw_topmost
        rts

; fillcell: A0=cx A1=cy A2=colour index -> fill one 8x8 cell.
.a16
.i16
fillcell:
        lda A0
        asl a
        asl a
        sta PX
        lda A1
        asl a
        asl a
        asl a
        sta PY
        lda #4
        sta PW
        lda #8
        sta PH
        lda A2
        and #$000F
        sta pxtmp
        asl a
        asl a
        asl a
        asl a
        ora pxtmp
        sta PB
        jsr fill_band
        rts

; dostris_draw: S2 = window offset.
.a16
.i16
dostris_draw:
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta DT0                ; board origin cx
        lda v_wintab+WY,x
        inc a
        sta DT1                ; board origin cy
        stz LC0                ; cy
@row:   lda LC0
        cmp #BH
        bcs @piece
        stz LC1                ; cx
@col:   lda LC1
        cmp #BW
        bcs @nextrow
        lda LC0
        jsr row_base_idx
        clc
        adc LC1
        tax
        sep #$20
        lda DTBOARD,x
        rep #$20
        and #$00FF
        bne @hascolor
        lda #15                ; empty cell -> dark-grey well
@hascolor:
        sta A2
        lda DT0
        clc
        adc LC1
        sta A0
        lda DT1
        clc
        adc LC0
        sta A1
        jsr fillcell
        inc LC1
        bra @col
@nextrow:
        inc LC0
        bra @row
@piece: lda v_dt_state
        bne @panel
        jsr draw_cur_piece
@panel: jsr draw_panel
        rts

; draw_cur_piece: overlay the active piece's cells.
.a16
.i16
draw_cur_piece:
        lda v_dt_piece
        asl a
        asl a
        asl a
        asl a
        sta DT2
        lda v_dt_rot
        asl a
        asl a
        clc
        adc DT2
        tax
        sep #$20
        ldy #0
@rd:    lda f:pieces,x
        sta DTCELLS,y
        inx
        iny
        cpy #4
        bcc @rd
        rep #$20
        lda v_dt_piece
        tax
        sep #$20
        lda f:piece_colors,x
        rep #$20
        and #$00FF
        sta DT3
        ldy #0
@cell:  sep #$20
        lda DTCELLS,y
        rep #$20
        and #$00FF
        pha
        lsr a
        lsr a
        lsr a
        lsr a
        clc
        adc v_dt_x
        clc
        adc DT0
        sta A0
        pla
        and #$000F
        clc
        adc v_dt_y
        clc
        adc DT1
        sta A1
        lda DT3
        sta A2
        phy
        jsr fillcell
        ply
        iny
        cpy #4
        bcc @cell
        rts

; draw_panel: score / lines / game-over text to the right of the well.
.a16
.i16
draw_panel:
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #13
        sta S3                 ; panel col
        lda v_wintab+WY,x
        clc
        adc #2
        sta S4                 ; panel row
        lda #.loword(str_dt_score)
        sta P0
        lda S3
        sta A0
        lda S4
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        lda v_dt_score
        sta S0
        jsr fmt_dec
        lda #.loword(v_numbuf)
        sta P0
        lda S3
        sta A0
        lda S4
        inc a
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        lda #.loword(str_dt_lines)
        sta P0
        lda S3
        sta A0
        lda S4
        clc
        adc #3
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        lda v_dt_lines
        sta S0
        jsr fmt_dec
        lda #.loword(v_numbuf)
        sta P0
        lda S3
        sta A0
        lda S4
        clc
        adc #4
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        lda v_dt_state
        beq @ctrl
        lda #.loword(str_dt_over)
        sta P0
        lda S3
        sta A0
        lda S4
        clc
        adc #7
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
@ctrl:  lda #.loword(str_dt_keys)
        sta P0
        lda S3
        sta A0
        lda S4
        clc
        adc #10
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        rts

; ---------------------------------------------------------------- piece data
; 7 pieces x 4 rotations x 4 cells; each cell byte = (dx<<4)|dy in a 4x4 box.
pieces:
        ; I
        .byte $01,$11,$21,$31, $20,$21,$22,$23, $02,$12,$22,$32, $10,$11,$12,$13
        ; O
        .byte $10,$20,$11,$21, $10,$20,$11,$21, $10,$20,$11,$21, $10,$20,$11,$21
        ; T
        .byte $10,$01,$11,$21, $10,$11,$21,$12, $01,$11,$21,$12, $10,$01,$11,$12
        ; S
        .byte $10,$20,$01,$11, $10,$11,$21,$22, $11,$21,$02,$12, $00,$01,$11,$12
        ; Z
        .byte $00,$10,$11,$21, $20,$11,$21,$12, $01,$11,$12,$22, $10,$01,$11,$02
        ; J
        .byte $00,$01,$11,$21, $10,$20,$11,$12, $01,$11,$21,$22, $10,$11,$02,$12
        ; L
        .byte $20,$01,$11,$21, $10,$11,$12,$22, $01,$11,$21,$02, $00,$10,$11,$12

; piece colour (SHR palette index) per piece: I O T S Z J L
piece_colors:
        .byte 2, 7, 3, 10, 11, 13, 9

str_dt_score: .byte "Score", 0
str_dt_lines: .byte "Lines", 0
str_dt_over:  .byte "GAME OVER", 0
str_dt_keys:  .byte "<-/->/up/dn/spc", 0
