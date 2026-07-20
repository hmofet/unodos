; ============================================================================
; UnoDOS / NES — Paint (app 10). A CELL canvas: NROM has CHR-ROM (no CHR-RAM),
; so free-pixel painting is impossible — instead the canvas is a 16x12 grid of
; cells, each holding one of the 4 palette inks (0 = the blue backdrop, then
; white / cyan / magenta solids from the fixed tile set). This ceiling is
; documented and accepted (the same trade the SMS-class ports make).
;
; UI: a white-framed canvas, a palette row of the 4 ink swatches (brackets
; mark the current ink), and a pad-moved cursor — an outline tile overlaid on
; the cell (4 CHR variants, one per underlying ink, so the cell's colour stays
; visible inside the cursor ring). D-pad moves, A paints, SELECT cycles the
; ink, B exits (kernel-global). All updates are 1-2 cell partials + the
; occasional palette-row repaint — tiny vblank writes.
; ============================================================================

PT_W     = 16
PT_H     = 12
PT_OX    = 2            ; screen col of canvas col 0
PT_OY    = 3            ; screen row of canvas row 0
PT_PROW  = 16           ; palette row
PTCANVAS = $0600        ; 192 bytes: y*16 + x, ink 0..3

; ---- zero page ($9C-$A6) ---------------------------------------------------
pt_cx    = $9C          ; cursor 0..15 / 0..11
pt_cy    = $9D
pt_ox    = $9E          ; previous cursor (for the partial erase)
pt_oy    = $9F
pt_ink   = $A0          ; current ink 0..3
f_ptcur  = $A1          ; partial: cursor moved (erase old cell, draw new)
f_ptcell = $A2          ; partial: cursor cell painted / ring refresh
f_ptink  = $A3          ; partial: ink changed (palette-row markers)
pt_n     = $A4          ; scratch (column countdown)

; ---- init ------------------------------------------------------------------
paint_init:
        ; clear the canvas to ink 0 (the blue backdrop)
        lda #0
        ldx #0
pti_c:  sta PTCANVAS,x
        inx
        cpx #PT_W*PT_H
        bne pti_c
        lda #PT_W/2
        sta pt_cx
        sta pt_ox
        lda #PT_H/2
        sta pt_cy
        sta pt_oy
        lda #1                  ; start ink = white
        sta pt_ink
        lda #0
        sta f_ptcur
        sta f_ptcell
        sta f_ptink
        rts

; ---- per-frame logic (no PPU writes) ---------------------------------------
paint_update:
        lda v_pade
        and #PAD_L
        beq ptu_r
        lda pt_cx
        beq ptu_a               ; at the edge: no move
        jsr pt_save_cur
        dec pt_cx
        jmp ptu_mv
ptu_r:  lda v_pade
        and #PAD_R
        beq ptu_u
        lda pt_cx
        cmp #PT_W-1
        bcs ptu_a
        jsr pt_save_cur
        inc pt_cx
        jmp ptu_mv
ptu_u:  lda v_pade
        and #PAD_U
        beq ptu_d
        lda pt_cy
        beq ptu_a
        jsr pt_save_cur
        dec pt_cy
        jmp ptu_mv
ptu_d:  lda v_pade
        and #PAD_D
        beq ptu_a
        lda pt_cy
        cmp #PT_H-1
        bcs ptu_a
        jsr pt_save_cur
        inc pt_cy
ptu_mv: lda #1
        sta f_ptcur
        rts
ptu_a:  lda v_pade
        and #PAD_A
        beq ptu_sel
        ; paint the cursor cell with the current ink
        jsr pt_cur_idx
        lda pt_ink
        sta PTCANVAS,x
        lda #1
        sta f_ptcell
        rts
ptu_sel:
        lda v_pade
        and #PAD_SEL
        beq ptu_done
        lda pt_ink
        clc
        adc #1
        and #3
        sta pt_ink
        lda #1
        sta f_ptink
ptu_done:
        rts

pt_save_cur:
        lda pt_cx
        sta pt_ox
        lda pt_cy
        sta pt_oy
        rts

; pt_cur_idx: X = pt_cy*16 + pt_cx
pt_cur_idx:
        lda pt_cy
        asl
        asl
        asl
        asl
        clc
        adc pt_cx
        tax
        rts

; ============================================================================
; rendering
; ============================================================================
; ---- pt_draw_plain: canvas cell (idx=x, grp=y) as its solid ink tile --------
pt_draw_plain:
        lda grp
        asl
        asl
        asl
        asl
        clc
        adc idx
        tax
        ldy PTCANVAS,x
        lda pt_solids,y
        ; fall through to pt_put_cell
; ---- pt_put_cell: write tile A at canvas cell (idx, grp) --------------------
pt_put_cell:
        pha
        lda idx
        clc
        adc #PT_OX
        sta col
        lda grp
        clc
        adc #PT_OY
        sta row
        jsr set_nt_addr
        pla
        sta PPUDATA
        rts

; ---- pt_draw_cursor: cursor ring variant over the cell at (pt_cx, pt_cy) ----
pt_draw_cursor:
        lda pt_cx
        sta idx
        lda pt_cy
        sta grp
        jsr pt_cur_idx
        lda PTCANVAS,x          ; underlying ink 0..3
        clc
        adc #T_CUR              ; -> ring-over-ink variant
        jmp pt_put_cell

; ---- pt_draw_palrow: 4 swatches, brackets around the current ink -----------
pt_draw_palrow:
        ldx #0
pdp_l:  stx tile                ; ink being drawn
        ; bracket cells (swatch col - 1 / + 1): '[' + ']' if current, spaces else
        lda pt_swcol,x
        sec
        sbc #1
        sta col
        lda #PT_PROW
        sta row
        jsr set_nt_addr
        ldx tile
        lda #$20                ; ' '
        cpx pt_ink
        bne pdp_lb
        lda #$5B                ; '['
pdp_lb: sec
        sbc #32
        clc
        adc #T_FONT
        sta PPUDATA             ; bracket left
        ; swatch tile (ink 0 shows as its ring tile so it stays visible)
        ldx tile
        lda pt_solids,x
        bne pdp_sw
        lda #T_CUR              ; empty ink: outlined blank
pdp_sw: sta PPUDATA             ; swatch (auto-increment)
        ldx tile
        lda #$20                ; ' '
        cpx pt_ink
        bne pdp_rb
        lda #$5D                ; ']'
pdp_rb: sec
        sbc #32
        clc
        adc #T_FONT
        sta PPUDATA             ; bracket right
        ldx tile
        inx
        cpx #4
        bne pdp_l
        rts

; ---- app_paint: full-screen draw (rendering off) ---------------------------
app_paint:
        jsr load_palette        ; deterministic desktop palette
        lda #<t_paint
        sta ptr
        lda #>t_paint
        sta ptr+1
        jsr draw_chrome
        lda #<c_paint
        sta ptr2
        lda #>c_paint
        sta ptr2+1
        jsr draw_content
        ; white frame: top + bottom rows
        lda #PT_OX-1
        sta col
        lda #PT_OY-1
        sta row
        jsr set_nt_addr
        ldx #PT_W+2
        lda #T_WHITE
apf_t:  sta PPUDATA
        dex
        bne apf_t
        lda #PT_OX-1
        sta col
        lda #PT_OY+PT_H
        sta row
        jsr set_nt_addr
        ldx #PT_W+2
        lda #T_WHITE
apf_b:  sta PPUDATA
        dex
        bne apf_b
        ; side walls
        ldx #0
apf_s:  stx grp
        txa
        clc
        adc #PT_OY
        sta row
        lda #PT_OX-1
        sta col
        jsr set_nt_addr
        lda #T_WHITE
        sta PPUDATA
        lda #PT_OX+PT_W
        sta col
        jsr set_nt_addr
        lda #T_WHITE
        sta PPUDATA
        ldx grp
        inx
        cpx #PT_H
        bne apf_s
        ; canvas rows, streamed 16 cells each
        lda #0
        sta grp                 ; canvas y
apc_row:
        lda #PT_OX
        sta col
        lda grp
        clc
        adc #PT_OY
        sta row
        jsr set_nt_addr
        lda grp
        asl
        asl
        asl
        asl
        tax                     ; X = y*16
        lda #PT_W
        sta pt_n
apc_c:  ldy PTCANVAS,x
        lda pt_solids,y
        sta PPUDATA
        inx
        dec pt_n
        bne apc_c
        inc grp
        lda grp
        cmp #PT_H
        bne apc_row
        ; cursor overlay + the palette row
        jsr pt_draw_cursor
        jmp pt_draw_palrow

; ---- render_partials hook (vblank) -----------------------------------------
rp_paint:
        lda f_ptcur
        beq rppt_cell
        lda #0
        sta f_ptcur
        ; old cell back to plain, cursor ring at the new cell
        lda pt_ox
        sta idx
        lda pt_oy
        sta grp
        jsr pt_draw_plain
        jsr pt_draw_cursor
rppt_cell:
        lda f_ptcell
        beq rppt_ink
        lda #0
        sta f_ptcell
        jsr pt_draw_cursor      ; repaint the (possibly recoloured) cursor cell
rppt_ink:
        lda f_ptink
        beq rppt_done
        lda #0
        sta f_ptink
        jsr pt_draw_palrow
rppt_done:
        rts

; ---- data ------------------------------------------------------------------
t_paint: dc.b "Paint",0
c_paint:
        dc.b 1, 1
        dc.w m_pt0
        dc.b 2, PT_PROW
        dc.w m_pt1
        dc.b $FF
m_pt0:  dc.b "A=paint Sel=ink",0
m_pt1:  dc.b "Ink:",0
pt_solids:
        dc.b 0, T_SOLW, T_SOLC, T_SOLM     ; ink -> solid tile
pt_swcol:
        dc.b 7, 10, 13, 16                 ; palette-row swatch columns