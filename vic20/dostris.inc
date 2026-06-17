; ============================================================================
; UnoDOS / VIC-20 — Dostris. The shared algorithm (6502); cells render as a
; solid block char coloured by the piece's colour-RAM nibble (board byte holds
; the colour, 0 = empty).
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
        bcc m7d
        sbc #7
        jmp mod7
m7d:    rts
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
pc_l:   lda mhi
        bpl pc_n
        txa
        and #3
        clc
        adc g_tx
        cmp #BW
        bcs pc_y
        sta g_bx
        txa
        and #$0C
        lsr
        lsr
        clc
        adc g_ty
        cmp #BH
        bcs pc_y
        sta g_by
        jsr board_off
        lda g_board,y
        bne pc_y
pc_n:   asl mlo
        rol mhi
        inx
        cpx #16
        bne pc_l
        lda #0
        rts
pc_y:   lda #1
        rts
ds_left:
        lda g_px
        sec
        sbc #1
        sta g_tx
        lda g_py
        sta g_ty
        jsr piece_collide
        bne dl_n
        lda g_tx
        sta g_px
dl_n:   rts
ds_right:
        lda g_px
        clc
        adc #1
        sta g_tx
        lda g_py
        sta g_ty
        jsr piece_collide
        bne dr_n
        lda g_tx
        sta g_px
dr_n:   rts
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
        beq dro_ok
        lda g_srot
        sta g_rot
dro_ok: rts
ds_softdrop:
        lda g_px
        sta g_tx
        lda g_py
        clc
        adc #1
        sta g_ty
        jsr piece_collide
        bne dsd_n
        inc g_py
dsd_n:  rts

dostris_update:
        lda g_state
        beq du_play
        rts
du_play:
        lda g_px
        sta g_oldpx
        lda g_py
        sta g_oldpy
        lda g_rot
        sta g_oldrot
        lda v_pade
        and #PAD_L
        beq du_r
        jsr ds_left
du_r:   lda v_pade
        and #PAD_R
        beq du_rot
        jsr ds_right
du_rot: lda v_pade
        and #PAD_U
        beq du_dn
        jsr ds_rotate
du_dn:  lda v_pad
        and #PAD_D
        beq du_grav
        jsr ds_softdrop
du_grav:
  IF AUTOTEST
        lda a_gpause
        bne du_check
  ENDIF
        inc g_fall
        lda g_fall
        cmp #FALLRATE
        bcc du_check
        lda #0
        sta g_fall
        lda g_px
        sta g_tx
        lda g_py
        clc
        adc #1
        sta g_ty
        jsr piece_collide
        beq du_fall
        jsr dostris_lock
        jsr dostris_clearlines
        jsr dostris_spawn
        lda #1
        sta v_dirty
        lda #0
        sta pf_pc
        rts
du_fall:
        inc g_py
du_check:
        lda g_px
        cmp g_oldpx
        bne du_mv
        lda g_py
        cmp g_oldpy
        bne du_mv
        lda g_rot
        cmp g_oldrot
        bne du_mv
        rts
du_mv:  lda #1
        sta pf_pc
        rts

dostris_lock:
        ldx g_type
        lda piece_cols,x
        sta g_lt
        lda g_px
        sta g_tx
        lda g_py
        sta g_ty
        jsr get_mask
        ldx #0
dl_l:   lda mhi
        bpl dl_x
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
        stx g_tmp
        jsr board_off
        lda g_lt
        sta g_board,y
        ldx g_tmp
dl_x:   asl mlo
        rol mhi
        inx
        cpx #16
        bne dl_l
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
        beq sp_ok
        lda #1
        sta g_state
sp_ok:  lda g_px
        sta g_oldpx
        lda g_py
        sta g_oldpy
        lda g_rot
        sta g_oldrot
        rts
dostris_init:
        ldx #0
        lda #0
di_c:   sta g_board,x
        inx
        cpx #BW*BH
        bne di_c
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
        lda #BH-1
        sta g_row
cl_l:   lda g_row
        cmp #$FF
        beq cl_d
        jsr row_full
        beq cl_dec
        jsr collapse_row
        inc g_lines
        jmp cl_l
cl_dec: dec g_row
        jmp cl_l
cl_d:   rts
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
rf_l:   lda g_board,x
        beq rf_no
        inx
        dey
        bne rf_l
        lda #1
        rts
rf_no:  lda #0
        rts
collapse_row:
        lda g_row
        sta g_tmp
cr_l:   lda g_tmp
        beq cr_t
        lda g_tmp
        asl
        sta g_n
        asl
        asl
        clc
        adc g_n
        sta g_by
        sec
        sbc #BW
        sta g_bx
        ldy #0
cr_cp:  ldx g_bx
        lda g_board,x
        ldx g_by
        sta g_board,x
        inc g_bx
        inc g_by
        iny
        cpy #BW
        bne cr_cp
        dec g_tmp
        jmp cr_l
cr_t:   ldx #0
        lda #0
cr_cl:  sta g_board,x
        inx
        cpx #BW
        bne cr_cl
        rts

; ---- rendering ----
; dcell: col/row set, A=colour (0=empty) -> draw block char + colour
dcell:
        tax
        beq dc_blank
        ldy #0
        lda #T_BLOCK
        sta (scrptr),y
        txa
        sta (colptr),y
        rts
dc_blank:
        ldy #0
        lda #0
        sta (scrptr),y
        rts

dostris_draw:
        ; walls (white blocks)
        ldx #0
dw_w:   stx g_row
        lda #BORG_COL-1
        sta col
        txa
        clc
        adc #BORG_ROW
        sta row
        jsr cellptr
        lda #1
        jsr dcell
        lda #BORG_COL+BW
        sta col
        lda g_row
        clc
        adc #BORG_ROW
        sta row
        jsr cellptr
        lda #1
        jsr dcell
        ldx g_row
        inx
        cpx #BH
        bne dw_w
        ; floor
        ldx #0
df:     stx g_tmp
        txa
        clc
        adc #BORG_COL-1
        sta col
        lda #BORG_ROW+BH
        sta row
        jsr cellptr
        lda #1
        jsr dcell
        ldx g_tmp
        inx
        cpx #BW+2
        bne df
        ; board cells
        lda #0
        sta g_row
dd_r:   lda g_row
        cmp #BH
        bcs dd_aft
        ; row base offset = g_row*BW
        lda g_row
        asl
        sta g_n
        asl
        asl
        clc
        adc g_n
        sta g_tmp           ; row*BW (board index base)
        ldx #0
dd_c:   stx g_bx
        lda #BORG_COL
        clc
        adc g_bx
        sta col
        lda g_row
        clc
        adc #BORG_ROW
        sta row
        jsr cellptr
        ldx g_bx
        lda g_tmp
        clc
        sta g_n
        txa
        clc
        adc g_n             ; board index = row*BW + bx
        tay
        lda g_board,y
        jsr dcell
        ldx g_bx
        inx
        cpx #BW
        bne dd_c
        inc g_row
        jmp dd_r
dd_aft:
        lda g_state
        bne dd_sc
        lda g_px
        sta g_tx
        lda g_py
        sta g_ty
        ldx g_type
        lda piece_cols,x
        sta g_pt
        jsr draw_piece_cells
dd_sc:  jsr dostris_draw_score
        lda g_state
        beq dd_done
        lda #<s_gameover
        sta strptr
        lda #>s_gameover
        sta strptr+1
        lda #BORG_COL
        sta col
        lda #BORG_ROW+5
        sta row
        lda #T_FONTINV
        sta tmp
        lda #1
        sta pcol
        jsr puts
dd_done:
        rts
dostris_draw_score:
        lda #<s_lines
        sta strptr
        lda #>s_lines
        sta strptr+1
        lda #BORG_COL+BW+2
        sta col
        lda #BORG_ROW
        sta row
        lda #T_FONT
        sta tmp
        lda #1
        sta pcol
        jsr puts
        lda g_lines
        jsr two_digits
        stx numstr
        sta numstr+1
        lda #0
        sta numstr+2
        lda #<numstr
        sta strptr
        lda #>numstr
        sta strptr+1
        lda #BORG_COL+BW+2
        sta col
        lda #BORG_ROW+1
        sta row
        lda #T_FONT
        sta tmp
        lda #1
        sta pcol
        jmp puts
draw_piece_cells:
        jsr get_mask
        ldx #0
dpc_l:  lda mhi
        bpl dpc_n
        stx g_n
        txa
        and #3
        clc
        adc g_tx
        clc
        adc #BORG_COL
        sta col
        lda g_n
        and #$0C
        lsr
        lsr
        clc
        adc g_ty
        clc
        adc #BORG_ROW
        sta row
        jsr cellptr
        lda g_pt
        jsr dcell
        ldx g_n
dpc_n:  asl mlo
        rol mhi
        inx
        cpx #16
        bne dpc_l
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
        lda piece_cols,x
        sta g_pt
        jsr draw_piece_cells
        lda g_px
        sta g_oldpx
        lda g_py
        sta g_oldpy
        lda g_rot
        sta g_oldrot
        rts
s_lines:    dc.b "Ln",0
s_gameover: dc.b "OVER",0
