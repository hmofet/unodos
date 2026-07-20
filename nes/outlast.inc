; ============================================================================
; UnoDOS / NES — OutLast (app 8). The pseudo-3D racer (the C64/SMS "band" model)
; fitted to the minimal profile: OL_BANDS rows, each a road span whose half-width
; grows toward the camera and whose centre snakes with scroll; a steerable car on
; the bottom band; running off the road = CRASH; distance is the score.
;
; Colour trick: OutLast installs its OWN 4-colour palette (grass-green backdrop /
; white road / yellow centre line / red car). Because a cleared nametable is all
; backdrop, the grass needs no drawing — only the road span, the dashed centre
; line and the car are written over it. While resident it owns the d-pad (L/R
; steer, B exits). Each scroll step does a clean rendering-off redraw (v_dirty),
; the kernel's big-change path; the brief blank is the minimal-profile cost.
; ============================================================================

OL_BANDS EQU 12
OL_COLS  EQU 18
OL_OX    EQU 2          ; field origin column
OL_OY    EQU 2          ; field origin row
OL_RATE  EQU 10         ; frames per scroll step

ol_carx   = $68
ol_scroll = $69
ol_dist   = $6A         ; word
ol_over   = $6C
ol_ctr    = $6D
ol_bnd    = $6E         ; draw: current band
ol_l      = $6F         ; band left col
ol_rt     = $70         ; band right col
ol_hw     = $71         ; band half-width
ol_cen    = $72         ; band centre

ol_palette: dc.b $1A,$30,$28,$16        ; green, white, yellow, red
s_ol_dist:  dc.b "Dist",0
s_ol_crash: dc.b "CRASH",0

; ---- ol_band: A = band -> ol_l (left col), ol_rt (right col) ---------------
ol_band:
        pha
        lsr
        lsr                     ; band/4
        clc
        adc #2                  ; hw = 2 + band/4
        sta ol_hw
        pla                     ; band
        clc
        adc ol_scroll
        and #7
        cmp #4
        bcc ob_pos
        sec
        sbc #8                  ; curve -4..-1
ob_pos:
        clc
        adc #9                  ; centre = 9 + curve
        sta ol_cen
        sec
        sbc ol_hw               ; left = centre - hw
        bpl ob_lok
        lda #0
ob_lok: sta ol_l
        lda ol_cen
        clc
        adc ol_hw               ; right = centre + hw
        cmp #OL_COLS
        bcc ob_rok
        lda #OL_COLS-1
ob_rok: sta ol_rt
        rts

outlast_init:
        lda #9
        sta ol_carx
        lda #0
        sta ol_scroll
        sta ol_dist
        sta ol_dist+1
        sta ol_over
        lda #OL_RATE
        sta ol_ctr
        lda #1
        sta v_dirty
        rts

; ---- ol_check: crash if the car is off the bottom band's road -------------
ol_check:
        lda #OL_BANDS-1
        jsr ol_band
        lda ol_carx
        cmp ol_l
        bcc oc_off              ; carx < left
        lda ol_carx
        cmp ol_rt
        beq oc_on
        bcs oc_off             ; carx > right
oc_on:  rts
oc_off: lda #1
        sta ol_over
        rts

; ---- input -----------------------------------------------------------------
outlast_update:
        lda v_pade
        and #PAD_L
        beq ou_r
        jsr ol_left
ou_r:   lda v_pade
        and #PAD_R
        beq ou_step
        jsr ol_right
ou_step:
        lda ol_over
        bne ou_done            ; crashed: freeze
  IF AUTOTEST
        lda a_gpause
        bne ou_done            ; script done: freeze for a stable shot
  ENDIF
        dec ol_ctr
        bne ou_done
        lda #OL_RATE
        sta ol_ctr
        inc ol_scroll
        inc ol_dist
        bne ou_nc
        inc ol_dist+1
ou_nc:  jsr ol_check
        lda #1
        sta v_dirty
ou_done:
        rts

ol_left:
        lda ol_carx
        beq ol_ret
        dec ol_carx
        jmp ol_dirty
ol_right:
        lda ol_carx
        cmp #OL_COLS-1
        bcs ol_ret
        inc ol_carx
        jmp ol_dirty
ol_ret: rts
ol_dirty:
        lda #1
        sta v_dirty
        rts

; ---- ol_set_palette: install the OutLast 4-colour palette ------------------
ol_set_palette:
        bit PPUSTATUS
        lda #$3F
        sta PPUADDR
        lda #$00
        sta PPUADDR
        ldx #0
osp_l:  lda ol_palette,x
        sta PPUDATA
        inx
        cpx #4
        bne osp_l
        rts

; ---- app_outlast: chrome + palette + field + car + HUD (rendering off) -----
; The nametable was just cleared (full_redraw), so every cell is grass already;
; only the road span, dashed centre line and the car are drawn over it.
app_outlast:
        lda #<l_outlast
        sta ptr
        lda #>l_outlast
        sta ptr+1
        jsr draw_chrome
        jsr ol_set_palette
        lda #0
        sta ol_bnd
ob_row:
        lda ol_bnd
        cmp #OL_BANDS
        bcs ob_car
        lda ol_bnd
        jsr ol_band
        ; road span ol_l..ol_rt (white)
        lda #OL_OX
        clc
        adc ol_l
        sta col
        lda ol_bnd
        clc
        adc #OL_OY
        sta row
        jsr set_nt_addr
        lda ol_rt
        sec
        sbc ol_l
        tax
        inx                     ; width = right - left + 1
        lda #T_SOLW
ob_road:
        sta PPUDATA
        dex
        bne ob_road
        ; dashed centre line (yellow) on alternate scroll-bands
        lda ol_scroll
        clc
        adc ol_bnd
        and #1
        beq ob_nostripe
        lda ol_l
        clc
        adc ol_rt
        lsr                     ; (left+right)/2
        clc
        adc #OL_OX
        sta col
        lda ol_bnd
        clc
        adc #OL_OY
        sta row
        jsr set_nt_addr
        lda #T_SOLC             ; index2 = yellow
        sta PPUDATA
ob_nostripe:
        inc ol_bnd
        jmp ob_row
ob_car:
        ; car (red) on the bottom band
        lda #OL_OX
        clc
        adc ol_carx
        sta col
        lda #OL_OY+OL_BANDS-1
        sta row
        jsr set_nt_addr
        lda #T_SOLM             ; index3 = red
        sta PPUDATA
        jmp ol_draw_hud

ol_draw_hud:
        lda #<s_ol_dist
        sta ptr
        lda #>s_ol_dist
        sta ptr+1
        lda #OL_OX+OL_COLS+1
        sta col
        lda #OL_OY
        sta row
        jsr cputs
        lda ol_dist
        jsr two_digits
        stx numstr
        sta numstr+1
        lda #0
        sta numstr+2
        lda #<numstr
        sta ptr
        lda #>numstr
        sta ptr+1
        lda #OL_OX+OL_COLS+1
        sta col
        lda #OL_OY+1
        sta row
        jsr cputs
        lda ol_over
        beq oh_done
        lda #<s_ol_crash
        sta ptr
        lda #>s_ol_crash
        sta ptr+1
        lda #OL_OX+OL_COLS+1
        sta col
        lda #OL_OY+3
        sta row
        jsr cputs
oh_done:
        rts

rp_outlast:
        rts
