; ============================================================================
; UnoDOS/Apple IIGS - Paint (proc 6): a mouse-driven Super Hi-Res colour
; canvas.  36x18 fat-pixel cells (one 8x8 SHR cell each), an 8-colour ink
; palette, drag-to-paint (paint_tick paints the cell under the cursor while
; the button is held over the canvas), number keys 1-8 pick the ink, C clears.
; Reuses dostris.i's fillcell.  Canvas buffer is a 648-byte bank-0 region.
; ============================================================================

PAINTBUF = $9C00              ; 36*18 = 648 bytes (bank 0)
PW_CELLS = 36
PH_CELLS = 18

v_paint_ink     = VARS+$454
v_paint_suppress = VARS+$456   ; 1 = ignore the held launch-click until release

; Paint zp scratch (transient; overlaps Dostris DT* harmlessly - never held
; across the other tick).
PT0 = $68
PT1 = $6A
PT2 = $6C
PT3 = $6E
PT4 = $70
PT5 = $72
PT6 = $74

; paint_start: clear the canvas and reset the ink.
.a16
.i16
paint_start:
        ldx #0
        sep #$20
        lda #15
@cl:    sta PAINTBUF,x
        inx
        cpx #(PW_CELLS*PH_CELLS)
        bcc @cl
        rep #$20
        lda #1
        sta v_paint_ink
        sta v_paint_suppress   ; don't paint with the click that launched us
        rts

; canvas_index: A0=relx A1=rely -> A = rely*36 + relx.
.a16
.i16
canvas_index:
        lda A1
        asl a
        asl a                  ; *4
        sta PT6
        lda A1
        asl a
        asl a
        asl a
        asl a
        asl a                  ; *32
        clc
        adc PT6                ; *36
        clc
        adc A0
        rts

; paint_tick: drag-paint while a Paint window is topmost and the button held.
.a16
.i16
paint_tick:
        lda v_zcount
        beq @out
        dec a
        jsr zent_x
        sep #$20
        lda v_wintab+WPROC,x
        rep #$20
        and #$00FF
        cmp #6
        bne @out
        lda v_mouse_btn
        bne @down
        stz v_paint_suppress   ; button released -> painting is allowed again
        bra @out
@down:  lda v_paint_suppress
        bne @out               ; still holding the launch click
        ; cursor cell
        lda v_mouse_x
        lsr a
        lsr a
        lsr a
        sta PT0                ; cursor cx
        lda v_mouse_y
        lsr a
        lsr a
        lsr a
        sta PT1                ; cursor cy
        ; canvas origin = (wx+1, wy+1)
        lda v_zcount
        dec a
        jsr zent_x
        lda v_wintab+WX,x
        inc a
        sta PT2
        lda v_wintab+WY,x
        inc a
        sta PT3
        ; relx / rely, bounds-checked
        lda PT0
        sec
        sbc PT2
        bmi @out
        cmp #PW_CELLS
        bcs @out
        sta A0                 ; relx
        lda PT1
        sec
        sbc PT3
        bmi @out
        cmp #PH_CELLS
        bcs @out
        sta A1                 ; rely
        jsr canvas_index
        tax
        sep #$20
        lda PAINTBUF,x
        cmp v_paint_ink
        beq @skip              ; already this ink - no redraw
        lda v_paint_ink
        sta PAINTBUF,x
        rep #$20
        jsr redraw_topmost
        rts
@skip:  rep #$20
@out:   rts

; paint_key: S0 = ascii. 1-8 select ink; C clears.
.a16
.i16
paint_key:
        lda S0
        cmp #'1'
        bcc @other
        cmp #'9'
        bcs @other
        sec
        sbc #'1'               ; 0..7
        tax
        sep #$20
        lda f:paint_inks,x
        rep #$20
        and #$00FF
        sta v_paint_ink
        jsr redraw_topmost
        rts
@other: cmp #'c'
        beq @clear
        cmp #'C'
        beq @clear
        rts
@clear: jsr paint_start
        jsr redraw_topmost
        rts

; paint_draw: S2 = window offset.
.a16
.i16
paint_draw:
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta PT2                ; canvas origin cx
        lda v_wintab+WY,x
        inc a
        sta PT3                ; canvas origin cy
        stz LC0                ; rely
@row:   lda LC0
        cmp #PH_CELLS
        bcs @palette
        stz LC1                ; relx
@col:   lda LC1
        cmp #PW_CELLS
        bcs @nextrow
        lda LC1
        sta A0
        lda LC0
        sta A1
        jsr canvas_index
        tax
        sep #$20
        lda PAINTBUF,x
        rep #$20
        and #$00FF
        sta A2                 ; colour
        lda PT2
        clc
        adc LC1
        sta A0
        lda PT3
        clc
        adc LC0
        sta A1
        jsr fillcell
        inc LC1
        bra @col
@nextrow:
        inc LC0
        bra @row
@palette:
        ; ink swatches along the bottom + a marker on the current ink
        ldx S2
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        sec
        sbc #2
        sta PT3                ; swatch row
        lda v_wintab+WX,x
        clc
        adc #2
        sta PT2                ; swatch start col
        stz LC0                ; ink index 0..7
@sw:    lda LC0
        cmp #8
        bcs @hint
        lda LC0
        asl a                  ; 2 cols per swatch
        clc
        adc PT2
        sta A0
        lda PT3
        sta A1
        ldx LC0
        sep #$20
        lda f:paint_inks,x
        rep #$20
        and #$00FF
        sta A2
        jsr fillcell
        ; second column of the swatch
        lda A0
        inc a
        sta A0
        jsr fillcell
        inc LC0
        bra @sw
@hint:  ldx S2
        lda v_wintab+WX,x
        clc
        adc #20
        sta A0
        lda PT3
        sta A1
        lda #.loword(str_paint_hint)
        sta P0
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        rts

paint_inks:       .byte 1, 2, 3, 7, 9, 10, 11, 13
str_paint_hint:   .byte "1-8 ink  C clr", 0
