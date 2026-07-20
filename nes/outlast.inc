; ============================================================================
; UnoDOS / NES — OutLast (app 8). The band-model pseudo-3D road racer, adapted
; from the C64's colour-cell-band version (also 6502): 13 perspective bands,
; each ONE NAMETABLE ROW, widening toward the bottom, with a scrolling
; triangle-wave centre. Grass is the BACKDROP itself (the app loads its own
; $3F00 palette: green backdrop / white stripes / grey road / red car), so a
; grass cell is just tile 0.
;
; The vblank budget can't take a 13-row repaint in one frame, so band redraws
; are AMORTISED: a scroll step marks all bands pending and render_partials
; repaints OL_STRIDE bands per frame — the sweep finishes well inside one
; scroll period (OL_RATE frames), so the road is always visually coherent.
;
; Controls: Left/Right (edges) steer the car, A restarts after a crash, B
; exits (kernel-global). Distance is the score; off the road = CRASH.
; ============================================================================

OL_BANDS  = 13          ; road bands = screen rows OL_TOP..OL_TOP+12
OL_TOP    = 2
OL_X0     = 1           ; band window: cols OL_X0 .. OL_X0+OL_XW-1
OL_XW     = 19
OL_RATE   = 8           ; frames per scroll step
OL_STRIDE = 2           ; bands repainted per vblank (13 bands / 2 = 7 frames < OL_RATE)
OL_HUDROW = 16

; ---- zero page ($6C-$7A) ---------------------------------------------------
ol_carx   = $6C         ; car column (absolute screen col)
ol_scroll = $6D
ol_dist   = $6E         ; word
ol_over   = $70         ; 1 = crashed
ol_ctr    = $71
ol_pend   = $72         ; next band to repaint, $FF = none
f_olhud   = $73         ; partial: distance / crash text changed
ol_band   = $74         ; band being drawn / geometry input
ol_hw     = $75         ; band half-width
ol_left   = $76         ; band road span (absolute screen cols)
ol_right  = $77
ol_x      = $78         ; column loop
ol_n      = $79         ; scratch
ol_strp   = $7A         ; stripe parity for the band

; ---- init ------------------------------------------------------------------
outlast_init:
        lda #10
        sta ol_carx
        lda #0
        sta ol_scroll
        sta ol_dist
        sta ol_dist+1
        sta ol_over
        sta f_olhud
        lda #$FF
        sta ol_pend
        lda #OL_RATE
        sta ol_ctr
        rts

; ---- per-frame logic (no PPU writes) ---------------------------------------
outlast_update:
        lda ol_over
        beq olu_play
        ; crashed: A restarts
        lda v_pade
        and #PAD_A
        beq olu_done
        jsr outlast_init
        lda #1
        sta v_dirty             ; full redraw
olu_done:
        rts
olu_play:
        ; -- steering (edges) --
        lda v_pade
        and #PAD_L
        beq olu_r
        lda ol_carx
        cmp #OL_X0+1
        bcc olu_car
        dec ol_carx
        jmp olu_car
olu_r:  lda v_pade
        and #PAD_R
        beq olu_scr
        lda ol_carx
        cmp #OL_X0+OL_XW-1
        bcs olu_scr
        inc ol_carx
olu_car:
        ; car moved: make sure the bottom band gets repainted
        lda ol_pend
        cmp #$FF
        bne olu_scr             ; a sweep is running - it will cover it
        lda #OL_BANDS-1
        sta ol_pend
        ; -- scroll step --
olu_scr:
  IF AUTOTEST
        lda a_gpause
        bne olu_done2           ; script done: freeze the road for the shot
  ENDIF
        dec ol_ctr
        bne olu_done2
        lda #OL_RATE
        sta ol_ctr
        inc ol_scroll
        inc ol_dist
        bne olu_d1
        inc ol_dist+1
olu_d1: lda #0
        sta ol_pend             ; repaint sweep from band 0
        lda #1
        sta f_olhud
        ; -- crash check on the bottom band --
        lda #OL_BANDS-1
        sta ol_band
        jsr ol_band_geom
        lda ol_carx
        cmp ol_left
        bcc olu_crash
        lda ol_carx
        cmp ol_right
        bcc olu_done2
        beq olu_done2
olu_crash:
        lda #1
        sta ol_over
        sta f_olhud
olu_done2:
        rts

; ---- ol_band_geom: ol_band -> ol_hw, ol_left, ol_right, ol_strp ------------
; hw = 2 + band/2 (2..8); curve = ((scroll+band)&7) mapped to -4..3;
; centre = 10 + curve; span clamped to the band window.
ol_band_geom:
        lda ol_band
        lsr
        clc
        adc #2
        sta ol_hw
        lda ol_scroll
        clc
        adc ol_band
        sta ol_strp             ; parity source (before masking)
        and #$07
        cmp #4
        bcc obg_pos
        sec
        sbc #8                  ; -> -4..-1
obg_pos:
        clc
        adc #10                 ; centre
        sta ol_n
        sec
        sbc ol_hw
        bpl obg_l
        lda #0
obg_l:  cmp #OL_X0
        bcs obg_l2
        lda #OL_X0
obg_l2: sta ol_left
        lda ol_n
        clc
        adc ol_hw
        cmp #OL_X0+OL_XW
        bcc obg_r
        lda #OL_X0+OL_XW-1
obg_r:  sta ol_right
        lda ol_strp
        and #1
        sta ol_strp
        rts

; ---- ol_draw_band: repaint band ol_band (one streamed row) -----------------
; grass = tile 0 outside the span; stripes on the edges every other band;
; the car overrides its cell on the bottom band.
ol_draw_band:
        jsr ol_band_geom
        lda #OL_X0
        sta col
        lda ol_band
        clc
        adc #OL_TOP
        sta row
        jsr set_nt_addr
        lda #OL_X0
        sta ol_x
odb_loop:
        ; car cell? (bottom band only, when alive)
        lda ol_band
        cmp #OL_BANDS-1
        bne odb_road
        lda ol_over
        bne odb_road
        lda ol_x
        cmp ol_carx
        bne odb_road
        lda #T_SOLM             ; car (index3 = red)
        jmp odb_put
odb_road:
        lda ol_x
        cmp ol_left
        bcc odb_grass           ; left of the road
        beq odb_edge
        cmp ol_right
        beq odb_edge
        bcc odb_mid
odb_grass:
        lda #0                  ; grass = backdrop
        jmp odb_put
odb_edge:
        lda ol_strp
        beq odb_mid
        lda #T_WHITE            ; stripe (index1 = white)
        jmp odb_put
odb_mid:
        lda #T_SOLC             ; road (index2 = grey)
odb_put:
        sta PPUDATA
        inc ol_x
        lda ol_x
        cmp #OL_X0+OL_XW
        bne odb_loop
        rts

; ---- app_outlast: full-screen draw (rendering off) -------------------------
app_outlast:
        ; the app's own palette (rendering is off - direct write is safe)
        bit PPUSTATUS
        lda #$3F
        sta PPUADDR
        lda #$00
        sta PPUADDR
        ldx #0
aol_pal:
        lda pal_outlast,x
        sta PPUDATA
        inx
        cpx #4
        bne aol_pal
        lda #<t_outlast
        sta ptr
        lda #>t_outlast
        sta ptr+1
        jsr draw_chrome
        ; all 13 bands
        lda #0
        sta ol_band
aol_band:
        jsr ol_draw_band
        inc ol_band
        lda ol_band
        cmp #OL_BANDS
        bne aol_band
        lda #$FF
        sta ol_pend
        ; HUD label
        lda #<s_oldist
        sta ptr
        lda #>s_oldist
        sta ptr+1
        lda #1
        sta col
        lda #OL_HUDROW
        sta row
        jsr cputs
        jmp ol_draw_hud

; ---- ol_draw_hud: distance count (+ crash banner) --------------------------
ol_draw_hud:
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
        lda #7
        sta col
        lda #OL_HUDROW
        sta row
        jsr cputs
        lda ol_over
        beq odh_done
        lda #<s_olover
        sta ptr
        lda #>s_olover
        sta ptr+1
        lda #10
        sta col
        lda #OL_HUDROW
        sta row
        lda #T_FONTINV
        sta tmp
        jmp puts
odh_done:
        rts

; ---- render_partials hook (vblank): amortised band sweep + HUD -------------
rp_outlast:
        lda ol_pend
        cmp #$FF
        beq rpol_hud
        ldx #OL_STRIDE
rpol_b: lda ol_pend
        sta ol_band
        txa
        pha
        jsr ol_draw_band
        pla
        tax
        inc ol_pend
        lda ol_pend
        cmp #OL_BANDS
        bcc rpol_n
        lda #$FF
        sta ol_pend
        jmp rpol_hud
rpol_n: dex
        bne rpol_b
rpol_hud:
        lda f_olhud
        beq rpol_done
        lda #0
        sta f_olhud
        jsr ol_draw_hud
rpol_done:
        rts

; ---- data ------------------------------------------------------------------
t_outlast: dc.b "OutLast",0
s_oldist:  dc.b "Dist:",0
s_olover:  dc.b "CRASH! A=new",0
