; ============================================================================
; UnoDOS/AppleII OutLast (milestone 3) - feasibility prototype of the
; pseudo-3D road racer (HANDOFF-M3 SS2, feasibility-gated).
;
; The road is a per-band raster: OL_BANDS horizontal bands (4px each, the
; documented half-vertical-resolution variant), each filled grass-then-road
; with a perspective half-width and a scrolling curve. Speed stripes alternate
; the grass dither by (band+scroll) parity. The player car is a sprite on the
; bottom band, steered left/right; running onto the grass costs speed. The
; game steps off the M1 soft tick, so the harness measures a deterministic
; frame cost (see tests/m3_outlast.script and the README verdict).
;
;   Left/Right  steer      N  restart      Esc  back to the desktop
; ============================================================================

OL_BANDS  equ 28            ; road bands (4px each) -> y8..y120
OL_Y0     equ 8
OL_CX     equ 20            ; nominal road centre (byte-column)
OL_CARY   equ 26            ; car band row (near the bottom)

; perspective half-width per band (cols): narrow at the horizon (band 0),
; widening toward the player. baked = 1 + band*16/OL_BANDS.
ol_hw:
        dc.b  1, 1, 2, 2, 3, 3, 4, 4
        dc.b  5, 5, 6, 6, 7, 7, 8, 8
        dc.b  9, 9,10,10,11,12,13,14
        dc.b 15,15,16,16

; curve offset per band index into ol_curve_tab (a smooth S, signed bytes)
ol_curve_tab:
        dc.b 0,1,2,3,3,3,2,1,0,$FF,$FE,$FD,$FD,$FD,$FE,$FF

; 7px car sprite (8 rows)
ol_car: dc.b $00,$08,$1C,$3E,$7F,$7F,$5D,$00

; ----------------------------------------------------------- ol_band_center
; ol_band_center - A = band -> A = road centre byte-column for that band,
; bending toward the horizon by the current curve.
ol_band_center:
        sta zpTmp               ; band
        ; curve sample for this band: ol_curve_tab[(band + ol_scroll) & 15]
        clc
        adc ol_scroll
        and #15
        tax
        lda ol_curve_tab,x
        ; weight by distance to the horizon (OL_BANDS-1-band): far -> more bend
        ; approx: center = OL_CX + curve * (OL_BANDS-band) / 8
        sta zpTmp2              ; signed curve
        lda #OL_BANDS
        sec
        sbc zpTmp               ; OL_BANDS - band (1..OL_BANDS)
        lsr
        lsr
        lsr                     ; /8 (0..3)
        sta zpTmp3              ; weight
        ; signed multiply curve * weight (both small): repeated add
        lda #0
        sta zpTmp4
        ldy zpTmp3
        beq obc_apply
obc_mul:
        clc
        lda zpTmp4
        adc zpTmp2
        sta zpTmp4
        dey
        bne obc_mul
obc_apply:
        lda #OL_CX
        clc
        adc zpTmp4              ; signed add (zpTmp4 may be $Fx negative)
        rts

; ------------------------------------------------------------- ol_draw_band
; ol_draw_band - draw road band A (0..OL_BANDS-1): grass fill (striped by
; parity), then the black road span, then white edge lines.
ol_draw_band:
        sta ol_band
        ; band pixel y = OL_Y0 + band*4
        asl
        asl
        clc
        adc #OL_Y0
        sta ol_bandy
        ; grass dither, alternating by (band+scroll) parity for the speed feel
        lda ol_band
        clc
        adc ol_scroll
        and #1
        beq odb_g0
        lda #$2A
        jmp odb_gp
odb_g0:
        lda #$55
odb_gp:
        sta zpFPat
        lda #0
        sta zpFX
        lda ol_bandy
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #4
        sta zpFH
        jsr fill_rows
        ; road span [center-hw, center+hw]
        lda ol_band
        jsr ol_band_center
        sta ol_bc
        ldx ol_band
        lda ol_hw,x
        sta ol_bhw
        ; left = center - hw, clamp >=0
        lda ol_bc
        sec
        sbc ol_bhw
        bcs odb_lok
        lda #0
odb_lok:
        sta ol_bl
        ; right = center + hw, clamp < SCRCOLS
        lda ol_bc
        clc
        adc ol_bhw
        cmp #SCRCOLS
        bcc odb_rok
        lda #(SCRCOLS-1)
odb_rok:
        sta ol_br
        ; fill road black [bl..br]
        lda ol_bl
        sta zpFX
        lda ol_br
        sec
        sbc ol_bl
        clc
        adc #1
        sta zpFW
        lda ol_bandy
        sta zpFY
        lda #4
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        ; white edge lines at bl and br
        lda ol_bl
        sta zpFX
        lda #1
        sta zpFW
        lda ol_bandy
        sta zpFY
        lda #4
        sta zpFH
        lda #$7F
        sta zpFPat
        jsr fill_rows
        lda ol_br
        sta zpFX
        lda #1
        sta zpFW
        lda ol_bandy
        sta zpFY
        lda #4
        sta zpFH
        lda #$7F
        sta zpFPat
        jmp fill_rows

; ------------------------------------------------------------- ol_draw_car
; ol_draw_car - blit the 7px car sprite at ol_carx on the car band.
ol_draw_car:
        lda #OL_CARY
        asl
        asl
        clc
        adc #OL_Y0
        sta zpTmp               ; car y
        ldx #0
odc_loop:
        txa
        clc
        adc zpTmp
        tay
        lda rowlo,y
        clc
        adc ol_carx
        sta zpDst
        lda rowhi,y
        adc #0
        sta zpDst+1
        txa
        tay
        lda ol_car,y
        ldy #0
        sta (zpDst),y
        inx
        cpx #8
        bne odc_loop
        rts

; ----------------------------------------------------------------- ol_draw
; ol_draw - full frame: all road bands + the car + the HUD (distance/status).
ol_draw:
        ; title row
        lda #0
        sta zpFX
        lda #0
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #8
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        lda #<msg_ol_title
        sta zpPtr
        lda #>msg_ol_title
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        ; clear the near field below the road bands (y120..167) to black
        lda #0
        sta zpFX
        lda #(OL_Y0+OL_BANDS*4)
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #(168-(OL_Y0+OL_BANDS*4))
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        ; bands
        lda #0
        sta ol_b
od_loop:
        lda ol_b
        jsr ol_draw_band
        inc ol_b
        lda ol_b
        cmp #OL_BANDS
        bne od_loop
        jsr ol_draw_car
        ; HUD: distance + status
        lda #0
        sta zpFX
        lda #168
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #16
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        lda #0
        sta zpCol
        lda #21
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_ol_dist
        sta zpPtr
        lda #>msg_ol_dist
        sta zpPtr+1
        jsr draw_string
        lda ol_dist
        sta zpFSSize
        lda ol_dist+1
        sta zpFSSize+1
        lda #6
        sta zpCol
        jsr draw_dec16
        ; status (CRASH! when off-road)
        lda ol_crash
        beq od_help
        lda #16
        sta zpCol
        lda #21
        sta zpRow
        lda #<msg_ol_crash
        sta zpPtr
        lda #>msg_ol_crash
        sta zpPtr+1
        jsr draw_string
od_help:
        lda #0
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_ol_help
        sta zpPtr
        lda #>msg_ol_help
        sta zpPtr+1
        jmp draw_string

; ----------------------------------------------------------------- ol_tick
; ol_tick - one game step per soft-clock second: scroll the road, advance
; distance, test the car against the road edges, full-redraw.
ol_tick:
        inc ol_scroll
        ; distance += 1
        inc ol_dist
        bne olt_chk
        inc ol_dist+1
olt_chk:
        ; is the car on the road at the car band?
        lda #OL_CARY
        jsr ol_band_center
        sta ol_bc
        ldx #OL_CARY
        lda ol_hw,x
        sta ol_bhw
        ; compute |carx - center|
        lda ol_carx
        sec
        sbc ol_bc
        bpl olt_abs
        eor #$FF
        clc
        adc #1
olt_abs:
        cmp ol_bhw
        bcc olt_onroad
        lda #1
        sta ol_crash
        jmp ol_draw
olt_onroad:
        lda #0
        sta ol_crash
        jmp ol_draw

; ----------------------------------------------------------------- outlast_open
outlast_open:
        lda #9
        sta app_mode
        jsr beep_click
        lda #OL_CX
        sta ol_carx
        lda #0
        sta ol_scroll
        sta ol_dist
        sta ol_dist+1
        sta ol_crash
        jmp ol_draw

outlast_close:
        lda #0
        sta app_mode
        jsr draw_desktop
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jmp draw_icons

; outlast_key - steer / restart / back (zpTmp = key).
outlast_key:
        lda zpTmp
        cmp #$9B                ; ESC
        beq outlast_close
        cmp #$88                ; left
        beq olk_left
        cmp #$95                ; right
        beq olk_right
        cmp #$CE                ; 'N' restart
        beq olk_new
        rts
olk_left:
        lda ol_carx
        cmp #2
        bcc olk_done
        dec ol_carx
        dec ol_carx
        jmp ol_draw
olk_right:
        lda ol_carx
        cmp #(SCRCOLS-3)
        bcs olk_done
        inc ol_carx
        inc ol_carx
        jmp ol_draw
olk_new:
        jmp outlast_open
olk_done:
        rts

msg_ol_title: dc.b "OutLast",0
msg_ol_dist:  dc.b "Dist:",0
msg_ol_crash: dc.b "OFF ROAD!",0
msg_ol_help:  dc.b "Left/Right steer  N=restart  Esc=back",0
