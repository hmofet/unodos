; ============================================================================
; UnoDOS / PC Engine — Dostris. The shared algorithm (HuC6280); board cells are
; drawn as VDC BAT entries at the board origin.
; ============================================================================

get_mask:
    lda g_type
    asl
    asl
    clc
    adc g_rot
    asl
    tay
    lda piece_masks,y
    sta mlo
    lda piece_masks+1,y
    sta mhi
    rts

rng:
    lda g_seed
    sta g_n
    asl
    asl
    clc
    adc g_n
    clc
    adc #1
    sta g_seed
    rts
mod7:
    cmp #7
    bcc @d
    sbc #7
    bra mod7
@d: rts

board_off:
    lda g_by
    asl
    sta g_n
    asl
    asl
    clc
    adc g_n
    clc
    adc g_bx
    tay
    rts

piece_collide:
    jsr get_mask
    ldx #0
@loop:
    lda mhi
    bpl @next
    txa
    and #3
    clc
    adc g_tx
    cmp #BW
    bcs @yes
    sta g_bx
    txa
    and #$0C
    lsr
    lsr
    clc
    adc g_ty
    cmp #BH
    bcs @yes
    sta g_by
    jsr board_off
    lda g_board,y
    bne @yes
@next:
    asl mlo
    rol mhi
    inx
    cpx #16
    bne @loop
    lda #0
    rts
@yes:
    lda #1
    rts

ds_left:
    lda g_px
    sec
    sbc #1
    sta g_tx
    lda g_py
    sta g_ty
    jsr piece_collide
    bne @no
    lda g_tx
    sta g_px
@no: rts
ds_right:
    lda g_px
    clc
    adc #1
    sta g_tx
    lda g_py
    sta g_ty
    jsr piece_collide
    bne @no
    lda g_tx
    sta g_px
@no: rts
ds_rotate:
    lda g_rot
    sta g_srot
    clc
    adc #1
    and #3
    sta g_rot
    lda g_px
    sta g_tx
    lda g_py
    sta g_ty
    jsr piece_collide
    beq @ok
    lda g_srot
    sta g_rot
@ok: rts
ds_softdrop:
    lda g_px
    sta g_tx
    lda g_py
    clc
    adc #1
    sta g_ty
    jsr piece_collide
    bne @no
    inc g_py
@no: rts

dostris_update:
    lda g_state
    beq @play
    rts
@play:
    lda g_px
    sta g_oldpx
    lda g_py
    sta g_oldpy
    lda g_rot
    sta g_oldrot
    lda v_pade
    and #PAD_L
    beq @r
    jsr ds_left
@r: lda v_pade
    and #PAD_R
    beq @rot
    jsr ds_right
@rot: lda v_pade
    and #PAD_U
    beq @a
    jsr ds_rotate
@a: lda v_pade
    and #PAD_A
    beq @dn
    jsr ds_rotate
@dn: lda v_pad
    and #PAD_D
    beq @grav
    jsr ds_softdrop
@grav:
.ifdef AUTOTEST
    lda a_gpause
    bne @check
.endif
    inc g_fall
    lda g_fall
    cmp #FALLRATE
    bcc @check
    lda #0
    sta g_fall
    lda g_px
    sta g_tx
    lda g_py
    clc
    adc #1
    sta g_ty
    jsr piece_collide
    beq @fall
    jsr dostris_lock
    jsr dostris_clearlines
    jsr dostris_spawn
    ; only the board region changed (locked cells / collapsed rows / new
    ; piece) — repaint just the board body, not display_off + clear_bat (all
    ; 1024 BAT entries through the single VDC port) + a full dostris_draw.
    lda #1
    sta pf_bd
    lda #0
    sta pf_pc
    rts
@fall:
    inc g_py
@check:
    lda g_px
    cmp g_oldpx
    bne @moved
    lda g_py
    cmp g_oldpy
    bne @moved
    lda g_rot
    cmp g_oldrot
    bne @moved
    rts
@moved:
    lda #1
    sta pf_pc
    rts

dostris_lock:
    ldx g_type
    lda piece_tiles,x
    sta g_lt
    lda g_px
    sta g_tx
    lda g_py
    sta g_ty
    jsr get_mask
    ldx #0
@loop:
    lda mhi
    bpl @next
    txa
    and #3
    clc
    adc g_tx
    sta g_bx
    txa
    and #$0C
    lsr
    lsr
    clc
    adc g_ty
    sta g_by
    jsr board_off
    lda g_lt
    sta g_board,y
@next:
    asl mlo
    rol mhi
    inx
    cpx #16
    bne @loop
    rts

dostris_spawn:
    jsr rng
    jsr mod7
    sta g_type
    lda #0
    sta g_rot
    sta g_fall
    lda #3
    sta g_px
    lda #0
    sta g_py
    lda g_px
    sta g_tx
    lda g_py
    sta g_ty
    jsr piece_collide
    beq @ok
    lda #1
    sta g_state
@ok:
    lda g_px
    sta g_oldpx
    lda g_py
    sta g_oldpy
    lda g_rot
    sta g_oldrot
    rts

dostris_init:
    ldx #0
    lda #0
@clr: sta g_board,x
    inx
    cpx #(BW*BH)
    bne @clr
    lda #0
    sta g_lines
    sta g_state
    sta g_fall
    lda v_frac
    ora #1
    sta g_seed
    jsr dostris_spawn
    rts

dostris_clearlines:
    lda #(BH-1)
    sta g_row
@loop:
    lda g_row
    cmp #$FF
    beq @done
    jsr row_full
    beq @dec
    jsr collapse_row
    inc g_lines
    bra @loop
@dec:
    dec g_row
    bra @loop
@done:
    rts

row_full:
    lda g_row
    asl
    sta g_n
    asl
    asl
    clc
    adc g_n
    tax
    ldy #BW
@loop:
    lda g_board,x
    beq @no
    inx
    dey
    bne @loop
    lda #1
    rts
@no:
    lda #0
    rts

collapse_row:
    lda g_row
    sta g_tmp
@loop:
    lda g_tmp
    beq @top
    lda g_tmp
    asl
    sta g_n
    asl
    asl
    clc
    adc g_n
    sta g_by               ; dst index
    sec
    sbc #BW
    sta g_bx               ; src index
    ldy #0
@copy:
    ldx g_bx
    lda g_board,x
    ldx g_by
    sta g_board,x
    inc g_bx
    inc g_by
    iny
    cpy #BW
    bne @copy
    dec g_tmp
    bra @loop
@top:
    ldx #0
    lda #0
@clr: sta g_board,x
    inx
    cpx #BW
    bne @clr
    rts

; ---- rendering ----
; dcell: col=BORG_COL+bx, row=BORG_ROW+by set by caller -> write tile A
dostris_draw:
    ; walls
    ldx #0
@walls:
    stx g_row
    lda #(BORG_COL-1)
    sta col
    txa
    clc
    adc #BORG_ROW
    sta row
    jsr vdc_cell
    lda #T_WHITE
    jsr putcell
    lda #(BORG_COL+BW)
    sta col
    lda g_row
    clc
    adc #BORG_ROW
    sta row
    jsr vdc_cell
    lda #T_WHITE
    jsr putcell
    ldx g_row
    inx
    cpx #BH
    bne @walls
    lda #(BORG_COL-1)
    sta col
    lda #(BORG_ROW+BH)
    sta row
    jsr vdc_cell
    ldx #(BW+2)
@floor:
    lda #T_WHITE
    jsr putcell
    dex
    bne @floor
    ; --- board body (cells + piece + score); repainted alone on lock ---
dostris_draw_board:
    ; board cells
    lda #0
    sta g_row
@brow:
    lda g_row
    cmp #BH
    bcs @after
    lda #BORG_COL
    sta col
    lda g_row
    clc
    adc #BORG_ROW
    sta row
    jsr vdc_cell
    lda g_row
    asl
    sta g_n
    asl
    asl
    clc
    adc g_n
    tax                    ; g_row*BW
    ldy #BW
@cell:
    lda g_board,x
    jsr putcell
    inx
    dey
    bne @cell
    inc g_row
    bra @brow
@after:
    lda g_state
    bne @score
    lda g_px
    sta g_tx
    lda g_py
    sta g_ty
    ldx g_type
    lda piece_tiles,x
    sta g_pt
    jsr draw_piece_cells
@score:
    jsr dostris_draw_score
    lda g_state
    beq @done
    lda #<s_gameover
    sta ptr
    lda #>s_gameover
    sta ptr+1
    lda #BORG_COL
    sta col
    lda #(BORG_ROW+5)
    sta row
    lda #T_FONTINV
    sta tmp
    jsr puts
@done:
    rts

dostris_draw_score:
    lda #<s_lines
    sta ptr
    lda #>s_lines
    sta ptr+1
    lda #(BORG_COL+BW+2)
    sta col
    lda #BORG_ROW
    sta row
    jsr cputs
    lda g_lines
    jsr two_digits
    stx numstr
    sta numstr+1
    lda #0
    sta numstr+2
    lda #<numstr
    sta ptr
    lda #>numstr
    sta ptr+1
    lda #(BORG_COL+BW+2)
    sta col
    lda #(BORG_ROW+2)
    sta row
    jmp cputs

draw_piece_cells:
    jsr get_mask
    ldx #0
@loop:
    lda mhi
    bpl @next
    txa
    and #3
    clc
    adc g_tx
    clc
    adc #BORG_COL
    sta col
    txa
    and #$0C
    lsr
    lsr
    clc
    adc g_ty
    clc
    adc #BORG_ROW
    sta row
    phx
    jsr vdc_cell
    lda g_pt
    jsr putcell
    plx
@next:
    asl mlo
    rol mhi
    inx
    cpx #16
    bne @loop
    rts

draw_piece_partial:
    lda g_oldpx
    sta g_tx
    lda g_oldpy
    sta g_ty
    lda g_rot
    pha
    lda g_oldrot
    sta g_rot
    lda #0
    sta g_pt
    jsr draw_piece_cells
    pla
    sta g_rot
    lda g_px
    sta g_tx
    lda g_py
    sta g_ty
    ldx g_type
    lda piece_tiles,x
    sta g_pt
    jsr draw_piece_cells
    lda g_px
    sta g_oldpx
    lda g_py
    sta g_oldpy
    lda g_rot
    sta g_oldrot
    rts

s_lines:    .byte "Lines",0
s_gameover: .byte "GAME OVER",0
