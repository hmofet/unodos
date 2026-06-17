; ============================================================================
; UnoDOS / NES — Dostris (falling-blocks game), the M3 interactive showcase.
; The exact algorithm the SMS port uses (get_mask / piece_collide / lock /
; clear-lines / gravity), ported to 6502. The board is BW x BH cells; each byte
; holds a block tile (0 = empty). The piece tables come from the Contract-shaped
; mkdata generator (piece_masks / piece_tiles), the same shape as the SMS port.
;
; In the minimal profile the game IS the resident app: while Dostris is open it
; owns the d-pad (left/right move, up or A rotate, down soft-drops). Gravity
; steps every FALLRATE frames. The falling piece is a SMALL per-frame partial
; (erase old cells + draw new cells, in vblank); a lock/line-clear triggers a
; rendering-off full board redraw (a brief settle, like a piece landing).
; ============================================================================

; ---- get_mask: g_type,g_rot -> mlo/mhi (current 16-bit shape mask) ---------
get_mask:
        lda g_type
        asl
        asl                     ; type*4
        clc
        adc g_rot               ; + rot
        asl                     ; *2 (word)
        tay
        lda piece_masks,y
        sta mlo
        lda piece_masks+1,y
        sta mhi
        rts

; ---- rng / mod7 ------------------------------------------------------------
rng:
        lda g_seed
        sta g_n
        asl
        asl
        clc
        adc g_n                 ; *5
        clc
        adc #1
        sta g_seed
        rts
mod7:
        cmp #7
        bcc m7_done
        sbc #7
        jmp mod7
m7_done:
        rts

; ---- piece_collide: trial origin g_tx,g_ty -> A=1 collide / 0 free ----------
; px wraps mod 256 for negatives; since BW=10 << 246 a single unsigned `cmp #BW`
; upper-bound test catches both off-board and (wrapped) negative columns.
piece_collide:
        jsr get_mask
        ldx #0                  ; bit index 0..15
pc_loop:
        lda mhi
        bpl pc_next             ; bit15 clear -> cell absent
        txa
        and #3
        clc
        adc g_tx                ; bx = origin + (bit & 3)
        cmp #BW
        bcs pc_yes              ; bx out of [0,BW)
        sta g_bx
        txa
        and #$0C
        lsr
        lsr                     ; row = (bit & 12) >> 2
        clc
        adc g_ty                ; by
        cmp #BH
        bcs pc_yes              ; by >= BH (floor)
        sta g_by
        jsr board_off           ; Y = by*BW + bx
        lda g_board,y
        bne pc_yes              ; occupied
pc_next:
        asl mlo
        rol mhi                 ; shift the mask left -> next cell into bit15
        inx
        cpx #16
        bne pc_loop
        lda #0
        rts
pc_yes:
        lda #1
        rts

; ---- board_off: g_by (row), g_bx (col) -> Y = by*BW + bx -------------------
board_off:
        lda g_by
        asl                     ; by*2
        sta g_n
        asl
        asl                     ; by*8
        clc
        adc g_n                 ; by*10
        clc
        adc g_bx
        tay
        rts

; ---- moves -----------------------------------------------------------------
ds_left:
        lda g_px
        sec
        sbc #1
        sta g_tx
        lda g_py
        sta g_ty
        jsr piece_collide
        bne dsl_no
        lda g_tx
        sta g_px
dsl_no: rts
ds_right:
        lda g_px
        clc
        adc #1
        sta g_tx
        lda g_py
        sta g_ty
        jsr piece_collide
        bne dsr_no
        lda g_tx
        sta g_px
dsr_no: rts
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
        beq dsro_ok
        lda g_srot
        sta g_rot               ; blocked: revert
dsro_ok:
        rts
ds_softdrop:
        lda g_px
        sta g_tx
        lda g_py
        clc
        adc #1
        sta g_ty
        jsr piece_collide
        bne dsd_no              ; rest (gravity will lock it); never cascades
        inc g_py
dsd_no: rts

; ============================================================================
; dostris_update: one frame of game logic (input + gravity)
; ============================================================================
dostris_update:
        lda g_state
        beq du_play             ; game over: ignore everything but B (handled above)
        rts
du_play:
        ; remember the pre-move pose so the partial can erase it
        lda g_px
        sta g_oldpx
        lda g_py
        sta g_oldpy
        lda g_rot
        sta g_oldrot
        ; --- input ---
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
        beq du_a
        jsr ds_rotate
du_a:   lda v_pade
        and #PAD_A
        beq du_dn
        jsr ds_rotate
du_dn:  lda v_pad
        and #PAD_D
        beq du_grav
        jsr ds_softdrop
        ; --- gravity ---
du_grav:
  IF AUTOTEST
        lda a_gpause
        bne du_check            ; script done: freeze gravity for a stable shot
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
        ; blocked below -> lock, clear lines, spawn the next piece
        jsr dostris_lock
        jsr dostris_clearlines
        jsr dostris_spawn
        lda #1
        sta v_dirty             ; full board redraw (includes the fresh piece)
        lda #0
        sta pf_pc
        rts
du_fall:
        inc g_py
du_check:
        lda g_px
        cmp g_oldpx
        bne du_moved
        lda g_py
        cmp g_oldpy
        bne du_moved
        lda g_rot
        cmp g_oldrot
        bne du_moved
        rts
du_moved:
        lda #1
        sta pf_pc
du_done:
        rts

; ---- dostris_lock: stamp the current piece into the board ------------------
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
dl_loop:
        lda mhi
        bpl dl_next
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
dl_next:
        asl mlo
        rol mhi
        inx
        cpx #16
        bne dl_loop
        rts

; ---- dostris_spawn: a new piece at the top -> game over if blocked ----------
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
        sta g_state             ; spawn blocked -> game over
sp_ok:
        lda g_px                ; the partial's "old" pose starts at the spawn
        sta g_oldpx
        lda g_py
        sta g_oldpy
        lda g_rot
        sta g_oldrot
        rts

; ---- dostris_init: fresh board + first piece -------------------------------
dostris_init:
        ldx #0
        lda #0
di_clr: sta g_board,x
        inx
        cpx #BW*BH
        bne di_clr
        lda #0
        sta g_lines
        sta g_state
        sta g_fall
        lda v_tick
        ora #1
        sta g_seed
        jsr dostris_spawn
        rts

; ---- line clearing ---------------------------------------------------------
dostris_clearlines:
        lda #BH-1
        sta g_row
cl_loop:
        lda g_row
        cmp #$FF                ; decremented below 0 -> done
        beq cl_done
        jsr row_full
        beq cl_dec
        jsr collapse_row
        inc g_lines
        jmp cl_loop             ; recheck the same row (the one above fell in)
cl_dec:
        dec g_row
        jmp cl_loop
cl_done:
        rts

; row_full: -> A=1 (Z=0) if row g_row is full, else A=0 (Z=1)
row_full:
        lda g_row
        asl
        sta g_n
        asl
        asl
        clc
        adc g_n                 ; row*10
        tax
        ldy #BW
rf_loop:
        lda g_board,x
        beq rf_no
        inx
        dey
        bne rf_loop
        lda #1
        rts
rf_no:  lda #0
        rts

; collapse_row: drop every row above g_row down by one, clear row 0
collapse_row:
        lda g_row
        sta g_tmp
cr_loop:
        lda g_tmp
        beq cr_top
        ; dst index = g_tmp*10, src index = (g_tmp-1)*10
        lda g_tmp
        asl
        sta g_n
        asl
        asl
        clc
        adc g_n
        sta g_by                ; reuse as dst index
        sec
        sbc #BW
        sta g_bx                ; reuse as src index
        ldy #0
cr_copy:
        ldx g_bx
        lda g_board,x
        ldx g_by
        sta g_board,x
        inc g_bx
        inc g_by
        iny
        cpy #BW
        bne cr_copy
        dec g_tmp
        jmp cr_loop
cr_top:
        ldx #0
        lda #0
cr_clr: sta g_board,x
        inx
        cpx #BW
        bne cr_clr
        rts

; ============================================================================
; rendering
; ============================================================================
; ---- dostris_draw: full board + frame + piece + score (rendering off) ------
; Called from app_dostris (after draw_chrome) during a full redraw.
dostris_draw:
        ; left + right walls (white) and the floor
        ldx #0
dw_walls:
        stx g_row
        ; left wall at (BORG_COL-1, BORG_ROW+row)
        lda #BORG_COL-1
        sta col
        txa
        clc
        adc #BORG_ROW
        sta row
        jsr set_nt_addr
        lda #T_WHITE
        sta PPUDATA
        ; right wall at (BORG_COL+BW, BORG_ROW+row)
        lda #BORG_COL+BW
        sta col
        jsr set_nt_addr
        lda #T_WHITE
        sta PPUDATA
        ldx g_row
        inx
        cpx #BH
        bne dw_walls
        ; floor row under the board
        lda #BORG_COL-1
        sta col
        lda #BORG_ROW+BH
        sta row
        jsr set_nt_addr
        ldx #BW+2
        lda #T_WHITE
dw_floor:
        sta PPUDATA
        dex
        bne dw_floor
        ; board cells
        lda #0
        sta g_row
dd_row:
        lda g_row
        cmp #BH
        bcs dd_after
        lda #BORG_COL
        sta col
        lda g_row
        clc
        adc #BORG_ROW
        sta row
        jsr set_nt_addr
        ; row pointer in X = g_row*BW
        lda g_row
        asl
        sta g_n
        asl
        asl
        clc
        adc g_n
        tax
        ldy #BW
dd_cell:
        lda g_board,x
        sta PPUDATA
        inx
        dey
        bne dd_cell
        inc g_row
        jmp dd_row
dd_after:
        ; live piece (unless game over)
        lda g_state
        bne dd_score
        lda g_px
        sta g_tx
        lda g_py
        sta g_ty
        ldx g_type
        lda piece_tiles,x
        sta g_pt
        jsr draw_piece_cells
dd_score:
        jsr dostris_draw_score
        lda g_state
        beq dd_done
        ; game-over banner
        lda #<s_gameover
        sta ptr
        lda #>s_gameover
        sta ptr+1
        lda #BORG_COL
        sta col
        lda #BORG_ROW+5
        sta row
        lda #T_FONTINV
        sta tmp
        jsr puts
dd_done:
        rts

; ---- dostris_draw_score: "Lines" + count to the right of the board ---------
dostris_draw_score:
        lda #<s_lines
        sta ptr
        lda #>s_lines
        sta ptr+1
        lda #BORG_COL+BW+2
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
        lda #BORG_COL+BW+2
        sta col
        lda #BORG_ROW+2
        sta row
        jmp cputs

; ---- draw_piece_cells: write g_pt at every cell of mask(g_type,g_rot) ------
; screen cell = (BORG_COL + g_tx + colbit, BORG_ROW + g_ty + rowbit)
draw_piece_cells:
        jsr get_mask
        ldx #0
dpc_loop:
        lda mhi
        bpl dpc_next
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
        jsr set_nt_addr         ; preserves X (unrolled, no X)
        lda g_pt
        sta PPUDATA
dpc_next:
        asl mlo
        rol mhi
        inx
        cpx #16
        bne dpc_loop
        rts

; ---- draw_piece_partial: erase the old pose, draw the new (vblank) ----------
draw_piece_partial:
        ; erase old cells (tile 0) using the saved old rotation
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
        ; draw the new cells
        lda g_px
        sta g_tx
        lda g_py
        sta g_ty
        ldx g_type
        lda piece_tiles,x
        sta g_pt
        jsr draw_piece_cells
        ; the new pose becomes the next frame's "old"
        lda g_px
        sta g_oldpx
        lda g_py
        sta g_oldpy
        lda g_rot
        sta g_oldrot
        rts

s_lines:    dc.b "Lines",0
s_gameover: dc.b "GAME OVER",0
