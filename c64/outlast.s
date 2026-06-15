; ============================================================================
; UnoDOS/C64 OutLast - a DISK-LOADED app (app9.bin, icon 9). The pseudo-3D
; road racer, the C64 take on the Apple II feasibility prototype. Where the
; Apple II renders a 1-bit per-pixel raster road at ~4 fps, the C64 draws the
; road as COLOUR CELL BANDS (green grass / grey road / striped edges) - far
; cheaper than per-pixel, so it runs smoothly: 20 perspective bands, each two
; colour_fills (grass span + road span), a steerable car and a scrolling curve.
;
; CRSR left/right steer; the road snakes (a scrolling triangle-wave centre) and
; widens toward the bottom; drift off the road and it's GAME OVER. N restarts,
; RUN/STOP exits. Distance is the score.
; ============================================================================

        processor 6502
        include "sys.inc"
        include "build/kernel_api.inc"

OL_BANDS equ 20         ; road bands = screen rows 2..21
OL_TOPROW equ 2
OL_RATE  equ 4          ; mainloop passes per scroll step (harness-tuned)
COL_GRASS equ $55       ; green (5<<4|5)
COL_ROAD  equ $CC       ; medium grey
COL_STRIPE equ $11      ; white
COL_CAR   equ $22       ; red

zpBand   equ zpApp0
zpCenter equ zpApp1
zpHw     equ zpApp2
zpLeft   equ zpApp3
zpRight  equ zpApp4
zpTmpA   equ zpApp5

        org APP_BASE
        jmp ol_init
        jmp ol_key
        jmp ol_tick

ol_init:
        lda #20
        sta ol_carx
        lda #0
        sta ol_scroll
        sta ol_dist
        sta ol_dist+1
        sta ol_over
        lda #OL_RATE
        sta ol_ctr
        jmp ol_draw_all

ol_key:
        lda zpTmp
        cmp #K_ESC
        bne ok1
        jmp return_to_desktop
ok1:
        cmp #$4E                ; N restart
        bne ok2
        jmp ol_init
ok2:
        lda ol_over
        bne ok_done
        lda zpTmp
        cmp #K_LEFT
        bne ok3
        lda ol_carx
        beq ok_done
        dec ol_carx
        rts
ok3:
        cmp #K_RIGHT
        bne ok_done
        lda ol_carx
        cmp #39
        bcs ok_done
        inc ol_carx
ok_done:
        rts

ol_tick:
        lda ol_over
        bne ot_done
        dec ol_ctr
        bne ot_done
        lda #OL_RATE
        sta ol_ctr
        inc ol_scroll
        ; distance++
        inc ol_dist
        bne ot_road
        inc ol_dist+1
ot_road:
        jsr ol_draw_road
        jsr ol_check
        jmp ol_draw_hud
ot_done:
        rts

; ol_band_geom - zpBand (0=top..19=bottom): set zpHw (half-width) and zpCenter.
; hw grows 3..12 down the screen; centre snakes with a scrolling triangle wave.
ol_band_geom:
        lda zpBand
        lsr
        clc
        adc #3
        sta zpHw                ; 3 + band/2
        ; curve = ((scroll + band) & 15); if >=8 subtract 16  -> -8..7
        lda ol_scroll
        clc
        adc zpBand
        and #$0F
        cmp #8
        bcc obg_pos
        sec
        sbc #16
obg_pos:
        clc
        adc #20                 ; centre = 20 + curve
        sta zpCenter
        ; left = centre - hw, right = centre + hw (clamp 0..39)
        sec
        lda zpCenter
        sbc zpHw
        bpl obg_l
        lda #0
obg_l:
        sta zpLeft
        clc
        lda zpCenter
        adc zpHw
        cmp #40
        bcc obg_r
        lda #39
obg_r:
        sta zpRight
        rts

; ol_draw_road - draw all 20 bands (grass then road span) + the car.
ol_draw_road:
        lda #0
        sta zpBand
odr_loop:
        jsr ol_band_geom
        ; whole band -> grass
        lda #0
        sta zpCX
        lda zpBand
        clc
        adc #OL_TOPROW
        sta zpCY
        lda #SCRCOLS
        sta zpCW
        lda #1
        sta zpCH
        lda #COL_GRASS
        sta zpFCol
        jsr color_fill
        ; road span (left..right) -> road colour, edges striped by parity
        lda zpLeft
        sta zpCX
        lda zpBand
        clc
        adc #OL_TOPROW
        sta zpCY
        lda zpRight
        sec
        sbc zpLeft
        clc
        adc #1
        sta zpCW                ; width = right-left+1
        lda #1
        sta zpCH
        lda #COL_ROAD
        sta zpFCol
        jsr color_fill
        ; stripe the two edge cells (alternating colour by band+scroll parity)
        lda zpBand
        clc
        adc ol_scroll
        and #1
        beq odr_nostripe
        lda zpLeft
        sta zpCX
        lda zpBand
        clc
        adc #OL_TOPROW
        sta zpCY
        lda #1
        sta zpCW
        sta zpCH
        lda #COL_STRIPE
        sta zpFCol
        jsr color_fill
        lda zpRight
        sta zpCX
        lda #1
        sta zpCW
        lda #COL_STRIPE
        sta zpFCol
        jsr color_fill
odr_nostripe:
        inc zpBand
        lda zpBand
        cmp #OL_BANDS
        bne odr_loop
        ; the car (red) on the bottom band
        lda ol_carx
        sta zpCX
        lda #(OL_TOPROW+OL_BANDS-1)
        sta zpCY
        lda #1
        sta zpCW
        sta zpCH
        lda #COL_CAR
        sta zpFCol
        jmp color_fill

; ol_check - off-road if the car isn't within the bottom band's road span.
ol_check:
        lda #(OL_BANDS-1)
        sta zpBand
        jsr ol_band_geom
        lda ol_carx
        cmp zpLeft
        bcc oc_off
        lda ol_carx
        cmp zpRight
        beq oc_on
        bcs oc_off
oc_on:
        rts
oc_off:
        lda #1
        sta ol_over
        jsr sid_click
        rts

ol_draw_all:
        jsr app_clear
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
        lda #0
        sta zpFX
        lda #7
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #1
        sta zpFH
        lda #$FF
        sta zpFPat
        jsr fill_rows
        jsr ol_draw_road
        ; help on row 23
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<msg_ol_help
        sta zpPtr
        lda #>msg_ol_help
        sta zpPtr+1
        jsr draw_string
        jmp ol_draw_hud

ol_draw_hud:
        ; clear status colour + bitmap for row 22
        lda #COL_WIN
        sta zpFCol
        lda #0
        sta zpCX
        lda #22
        sta zpCY
        lda #SCRCOLS
        sta zpCW
        lda #1
        sta zpCH
        jsr color_fill
        lda #0
        sta zpFX
        lda #176
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #8
        sta zpFH
        lda #$00
        sta zpFPat
        jsr fill_rows
        lda #1
        sta zpCol
        lda #22
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<msg_ol_dist
        sta zpPtr
        lda #>msg_ol_dist
        sta zpPtr+1
        jsr draw_string
        lda ol_dist
        sta zpFSSize
        lda ol_dist+1
        sta zpFSSize+1
        lda #7
        sta zpCol
        jsr draw_dec16
        lda ol_over
        beq odh_done
        lda #20
        sta zpCol
        lda #22
        sta zpRow
        lda #<msg_ol_over
        sta zpPtr
        lda #>msg_ol_over
        sta zpPtr+1
        jsr draw_string
odh_done:
        rts

msg_ol_title: dc.b "OutLast",0
msg_ol_dist:  dc.b "Dist:",0
msg_ol_over:  dc.b "CRASH! N=new",0
msg_ol_help:  dc.b "CRSR steer   N=new   STOP=back",0

ol_carx:   dc.b 20
ol_scroll: dc.b 0
ol_dist:   dc.w 0
ol_over:   dc.b 0
ol_ctr:    dc.b OL_RATE
