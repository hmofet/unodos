; ============================================================================
; UnoDOS / NES — Paint (app 10). A cell canvas + a swatch row, drawn as
; nametable tiles. The minimal-profile analogue of the SMS/C64 Paint: while
; resident it owns the d-pad — a hollow-box cursor hovers a cell; A paints the
; current colour onto the canvas (or, on the swatch row, picks a swatch); B
; exits. Colours are the solid-colour tiles (erase / white / cyan / magenta) —
; the NES CHR is ROM (NROM-256), so this is a cell canvas, not the per-pixel
; canvas the RAM-CHR ports (SNES) can do. Every edit is user-paced, so each
; action just requests a rendering-off full redraw (v_dirty); no vblank partial.
; ============================================================================

PCW    EQU 16          ; canvas width (cells)
PCH    EQU 12          ; canvas height
PC_OX  EQU 2           ; canvas origin column
PC_OY  EQU 3           ; canvas origin row
NSW    EQU 4           ; swatches (erase, white, cyan, magenta)
PSW_ROW EQU PC_OY+PCH+1 ; swatch row

p_cx   = $90           ; cursor col (0..PCW-1 canvas / 0..NSW-1 swatch)
p_cy   = $91           ; cursor row (0..PCH-1 canvas, PCH = swatch row)
p_col  = $92           ; current paint tile
p_r    = $93           ; draw scratch
p_canvas = $0600       ; PCW*PCH tiles (192 bytes)

paint_swatches:
        dc.b 0, T_SOLW, T_SOLC, T_SOLM      ; erase, white, cyan, magenta
s_p_hint: dc.b "A paint  U=swatches",0

paint_init:
        ldx #0
        lda #0
pn_clr: sta p_canvas,x
        inx
        cpx #PCW*PCH
        bne pn_clr
        ; seed a small demo pattern in the top-left
        lda #T_SOLW
        sta p_canvas+0
        lda #T_SOLC
        sta p_canvas+1
        lda #T_SOLM
        sta p_canvas+2
        lda #0
        sta p_cx
        sta p_cy
        lda #T_SOLW
        sta p_col
        lda #1
        sta v_dirty
        rts

; ---- input -----------------------------------------------------------------
paint_update:
        lda p_cy
        cmp #PCH
        beq pi_pal
        ; --- canvas ---
        lda v_pade
        and #PAD_L
        beq pic_r
        jsr pi_cleft
pic_r:  lda v_pade
        and #PAD_R
        beq pic_u
        jsr pi_cright
pic_u:  lda v_pade
        and #PAD_U
        beq pic_d
        jsr pi_cup
pic_d:  lda v_pade
        and #PAD_D
        beq pic_a
        jsr pi_cdown
pic_a:  lda v_pade
        and #PAD_A
        beq pi_ret
        jsr pi_paint
pi_ret: rts
pi_pal:
        lda v_pade
        and #PAD_L
        beq pip_r
        jsr pi_pleft
pip_r:  lda v_pade
        and #PAD_R
        beq pip_u
        jsr pi_pright
pip_u:  lda v_pade
        and #PAD_U
        beq pip_d
        jsr pi_tocanvas
pip_d:  lda v_pade
        and #PAD_D
        beq pip_a
        jsr pi_tocanvas
pip_a:  lda v_pade
        and #PAD_A
        beq pi_ret
        jsr pi_pick
        rts

pi_cleft:
        lda p_cx
        beq pp_ret
        dec p_cx
        jmp paint_dirty
pi_cright:
        lda p_cx
        cmp #PCW-1
        bcs pp_ret
        inc p_cx
        jmp paint_dirty
pi_cup:
        lda p_cy
        beq pi_cup_pal
        dec p_cy
        jmp paint_dirty
pi_cup_pal:
        lda #PCH
        sta p_cy
        jsr paint_clamp_pal
        jmp paint_dirty
pi_cdown:
        lda p_cy
        cmp #PCH-1
        bcs pi_cdown_pal
        inc p_cy
        jmp paint_dirty
pi_cdown_pal:
        lda #PCH
        sta p_cy
        jsr paint_clamp_pal
        jmp paint_dirty
pi_paint:
        ; p_canvas[p_cy*PCW + p_cx] = p_col   (PCW = 16 -> p_cy<<4)
        lda p_cy
        asl
        asl
        asl
        asl
        clc
        adc p_cx
        tax
        lda p_col
        sta p_canvas,x
        jmp paint_dirty
pi_pleft:
        lda p_cx
        beq pp_ret
        dec p_cx
        jmp paint_dirty
pi_pright:
        lda p_cx
        cmp #NSW-1
        bcs pp_ret
        inc p_cx
        jmp paint_dirty
pi_tocanvas:
        lda p_cx
        cmp #PCW
        bcc pit_ok
        lda #PCW-1
        sta p_cx
pit_ok: lda #PCH-1
        sta p_cy
        jmp paint_dirty
pi_pick:
        ldx p_cx
        lda paint_swatches,x
        sta p_col
        jmp paint_dirty
pp_ret: rts
paint_clamp_pal:
        lda p_cx
        cmp #NSW
        bcc pcp_ok
        lda #NSW-1
        sta p_cx
pcp_ok: rts
paint_dirty:
        lda #1
        sta v_dirty
        rts

; ---- app_paint: chrome + canvas + swatches + cursor (rendering off) --------
app_paint:
        lda #<l_paint
        sta ptr
        lda #>l_paint
        sta ptr+1
        jsr draw_chrome
        ; canvas rows from the buffer
        lda #0
        sta p_r
pd_row:
        lda p_r
        cmp #PCH
        bcs pd_sw
        lda #PC_OX
        sta col
        lda p_r
        clc
        adc #PC_OY
        sta row
        jsr set_nt_addr
        lda p_r
        asl
        asl
        asl
        asl                     ; p_r*PCW
        tax
        ldy #PCW
pd_cell:
        lda p_canvas,x
        sta PPUDATA
        inx
        dey
        bne pd_cell
        inc p_r
        jmp pd_row
pd_sw:
        ; swatch row
        lda #PC_OX
        sta col
        lda #PSW_ROW
        sta row
        jsr set_nt_addr
        ldx #0
pd_swl: lda paint_swatches,x
        sta PPUDATA
        inx
        cpx #NSW
        bne pd_swl
        ; current-colour indicator
        lda #PC_OX+NSW+1
        sta col
        lda #PSW_ROW
        sta row
        jsr set_nt_addr
        lda p_col
        sta PPUDATA
        ; hint
        lda #<s_p_hint
        sta ptr
        lda #>s_p_hint
        sta ptr+1
        lda #PC_OX
        sta col
        lda #PSW_ROW+2
        sta row
        jsr cputs
        ; cursor box at the current cell
        lda p_cy
        cmp #PCH
        beq pd_cur_pal
        lda p_cx
        clc
        adc #PC_OX
        sta col
        lda p_cy
        clc
        adc #PC_OY
        sta row
        jmp pd_cur_put
pd_cur_pal:
        lda p_cx
        clc
        adc #PC_OX
        sta col
        lda #PSW_ROW
        sta row
pd_cur_put:
        jsr set_nt_addr
        lda #T_PCUR
        sta PPUDATA
        rts

rp_paint:
        rts
