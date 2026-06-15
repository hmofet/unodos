; ============================================================================
; UnoDOS/Apple IIGS - OutLast (proc 10): a pseudo-3D road racer on SHR.
;
; A perspective road raster - rows from the horizon down widen toward the
; viewer (per-row half-width table), with an animated dashed centre line for
; speed and a gentle curve that sways the road.  Steer the car (red, bottom
; centre) left/right; distance scores.  Cell-granular bands keep the raster
; cheap; reuses dostris.i's fillcell.
; ============================================================================

OLW = 34                      ; canvas width in cells
OLH = 20                      ; canvas height in cells
OL_SKY = 4                    ; sky rows
OL_ROADH = (OLH - OL_SKY)     ; 16 road rows
OL_CX = (OLW / 2)             ; road centre column (17)

; v_ol_* live in sys.inc.

OL0 = $68
OL1 = $6A
OL2 = $6C
OL3 = $6E

.a16
.i16
outlast_start:
        stz v_ol_state
        stz v_ol_carx
        stz v_ol_dist
        stz v_ol_scroll
        stz v_ol_phase
        rts

; outlast_tick: animate the road + distance while the window is open.
.a16
.i16
outlast_tick:
        stz S5
@scan:  lda S5
        jsr ent_x
        sep #$20
        lda v_wintab+WSTATE,x
        beq @nf
        lda v_wintab+WPROC,x
        cmp #10
        bne @nf
        rep #$20
        bra @found
@nf:    rep #$20
        inc S5
        lda S5
        cmp #MAXWIN
        bcc @scan
        rts
@found: inc v_ol_scroll
        lda v_ol_dist
        clc
        adc #1
        sta v_ol_dist
        lda v_ol_scroll
        and #7
        bne @nophase
        inc v_ol_phase         ; advance the curve sway slowly
@nophase:
        jsr redraw_topmost
        rts

; outlast_key: S0 = ascii -> steer.
.a16
.i16
outlast_key:
        lda S0
        cmp #$08
        bne :+
        lda v_ol_carx
        cmp #($10000 - 12)     ; clamp left
        beq @out
        dec a
        sta v_ol_carx
        jmp @redraw
:       cmp #$15
        bne @out
        lda v_ol_carx
        cmp #12                ; clamp right
        beq @out
        inc a
        sta v_ol_carx
@redraw:
        jsr redraw_topmost
@out:   rts

; ol_curve: -> A = lateral road offset for the current phase (triangle wave).
.a16
.i16
ol_curve:
        lda v_ol_phase
        and #15
        cmp #8
        bcc @up
        ; 8..15 -> 8 down to 1
        eor #$FFFF
        clc
        adc #16
@up:    sec
        sbc #4                 ; centre to roughly -4..+4
        rts

.a16
.i16
outlast_draw:
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta OL2                ; canvas origin cx
        lda v_wintab+WY,x
        clc
        adc #1
        sta OL3                ; canvas origin cy
        jsr ol_curve
        sta OL1                ; curve amplitude
        stz LC0                ; row
@row:   lda LC0
        cmp #OLH
        bcs @car
        ; sky rows
        lda LC0
        cmp #OL_SKY
        bcs @road
        jsr ol_fill_row_sky
        inc LC0
        bra @row
@road:  jsr ol_draw_road_row
        inc LC0
        bra @row
@car:   ; the car (red), two cells, near the bottom centre
        lda #OL_CX
        clc
        adc v_ol_carx
        clc
        adc OL2
        sta A0
        lda OL3
        clc
        adc #(OLH-2)
        sta A1
        lda #11
        sta A2
        jsr fillcell
        lda OL3
        clc
        adc #(OLH-1)
        sta A1
        lda #11
        sta A2
        jsr fillcell
        ; HUD: distance
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc #1
        sta A1
        lda #.loword(str_ol_dist)
        sta P0
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        lda v_ol_dist
        sta S0
        jsr fmt_dec
        lda #.loword(v_numbuf)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #7
        sta A0
        lda v_wintab+WY,x
        clc
        adc #1
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        rts

; ol_fill_row_sky: fill canvas row LC0 with sky.
.a16
.i16
ol_fill_row_sky:
        stz LC1
@c:     lda LC1
        cmp #OLW
        bcs @done
        lda OL2
        clc
        adc LC1
        sta A0
        lda OL3
        clc
        adc LC0
        sta A1
        lda #13                ; sky blue
        sta A2
        jsr fillcell
        inc LC1
        bra @c
@done:  rts

; ol_draw_road_row: perspective road row LC0 (road row rr = LC0-OL_SKY).
.a16
.i16
ol_draw_road_row:
        lda LC0
        sec
        sbc #OL_SKY
        sta OL0                ; rr (0..ROADH-1)
        ; half-width = ol_hw[rr]
        tax
        sep #$20
        lda f:ol_hw,x
        rep #$20
        and #$00FF
        sta S6                 ; hw
        ; centre = OL_CX + curve*(ROADH-rr)/ROADH  (far rows curve more)
        lda #OL_ROADH
        sec
        sbc OL0                ; ROADH-rr
        sta S7
        lda OL1                ; curve amp (signed)
        ; multiply curve*(ROADH-rr): small, do shift approx (>>2)
        ; use S7 as count of additions (cheap, ROADH small)
        stz S3                 ; accumulator
        ldy S7
        beq @cdone
@cmul:  lda S3
        clc
        adc OL1
        sta S3
        dey
        bne @cmul
@cdone: ; signed divide of S3 by ROADH (16) = arithmetic >>4
        lda S3
        bit #$8000
        beq @pos
        eor #$FFFF
        inc a
        lsr a
        lsr a
        lsr a
        lsr a
        eor #$FFFF
        inc a
        bra @havec
@pos:   lsr a
        lsr a
        lsr a
        lsr a
@havec: clc
        adc #OL_CX
        sta S4                 ; row centre column
        ; draw each column
        stz LC1
@c:     lda LC1
        cmp #OLW
        bcs @done
        ; d = |col - centre|
        lda LC1
        sec
        sbc S4
        bpl @dp
        eor #$FFFF
        inc a
@dp:    cmp S6
        bcc @road
        beq @road
        lda #10                ; grass green
        bra @put
@road:  ; centre dashed stripe
        lda LC1
        sec
        sbc S4
        bne @plain
        lda OL0
        clc
        adc v_ol_scroll
        and #3
        cmp #2
        bcs @plain
        lda #1                 ; white stripe dash
        bra @put
@plain: lda #12                ; road grey
@put:   sta A2
        lda OL2
        clc
        adc LC1
        sta A0
        lda OL3
        clc
        adc LC0
        sta A1
        jsr fillcell
        inc LC1
        bra @c
@done:  rts

; per-road-row half-width (cells), horizon -> viewer
ol_hw:  .byte 2,2,3,3,4,5,5,6,7,7,8,9,10,11,12,13
str_ol_dist: .byte "Dist", 0
