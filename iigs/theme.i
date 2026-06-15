; ============================================================================
; UnoDOS/Apple IIGS - Theme (proc 3): the 8 shared UI palette presets
; (PORT-SPEC SS1) live-rewriting SHR palette line 0.  On the IIGS this is a
; showcase of the 4096-colour palette: writing $E1:9E00 recolours the whole
; desktop instantly (SHR looks up the palette per pixel at scan-out), so a
; theme switch needs only a palette poke - no repaint.
; ============================================================================

; v_theme lives in sys.inc

.a16
.i16
apply_theme:
        ; A = preset index -> copy 8 words (indices 0..7) to SHR palette line 0.
        ; GP's bank byte is $E1 (preset at boot); aim it at the palette offset.
        and #$0007
        sta v_theme
        asl a
        asl a
        asl a
        asl a                  ; *16 bytes (8 words) = offset into theme_presets
        tax                    ; X = source offset (f:theme_presets adds the base)
        lda #SHR_PAL
        sta GP                 ; GP = $9E00 (palette), bank $E1
        ldy #0
@cp:    lda f:theme_presets,x
        sta [GP],y             ; SHR palette entry (indirect-long, bank $E1)
        inx
        inx
        iny
        iny
        cpy #16
        bcc @cp
        rts

.a16
.i16
theme_draw:
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta S3                 ; left col
        lda v_wintab+WY,x
        clc
        adc #2
        sta S4                 ; top row
        ; caption
        lda #.loword(str_theme_hdr)
        sta P0
        lda S3
        sta A0
        lda S4
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        ; preset name (accent)
        lda v_theme
        and #$0007
        asl a
        tax
        lda f:theme_names,x
        sta P0
        lda S3
        sta A0
        lda S4
        clc
        adc #2
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        ; a row of 8 colour swatches (cells filled with palette indices 0..7)
        stz LC0
@sw:    lda LC0
        cmp #8
        bcs @hint
        lda S3
        clc
        adc LC0
        sta A0
        lda S4
        clc
        adc #4
        sta A1
        lda #2
        sta A2
        lda #2
        sta A3
        ; build a solid byte of index LC0 (both nibbles)
        lda LC0
        sta PB
        ; fill_cells uses attr->bg; instead fill directly via fill_band
        lda A0
        asl a
        asl a
        sta PX
        lda A1
        asl a
        asl a
        asl a
        sta PY
        lda #8
        sta PW
        lda #16
        sta PH
        lda LC0
        and #$000F
        sta pxtmp
        asl a
        asl a
        asl a
        asl a
        ora pxtmp
        sta PB
        jsr fill_band
        inc LC0
        bra @sw
@hint:  lda #.loword(str_theme_hint)
        sta P0
        lda S3
        sta A0
        lda S4
        clc
        adc #7
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        rts

.a16
.i16
theme_key:
        lda S0
        cmp #$15               ; right -> next preset
        beq @next
        cmp #$08               ; left -> prev preset
        beq @prev
        rts
@next:  lda v_theme
        inc a
        and #$0007
        bra @apply
@prev:  lda v_theme
        dec a
        and #$0007
@apply: jsr apply_theme
        jsr repaint_all        ; refresh chrome under the new palette
        rts

; ---------------------------------------------------------------- preset data
; 8 presets x 8 colours (indices 0..7), $0RGB.  Index 0 desktop bg, 1 fg/text,
; 2 accent, 3 alt, 4 black, 5 grey, 6 title-bar fill, 7 highlight.
theme_presets:
        .word $000A,$0FFF,$00AA,$0A0A,$0000,$0CCC,$0006,$0FF0  ; Classic
        .word $0001,$0ACE,$008F,$058F,$0000,$0668,$0003,$0FFF  ; Midnight
        .word $0030,$0DFD,$00C0,$08F8,$0000,$09B9,$0020,$0FF8  ; Forest
        .word $0700,$0FED,$0F80,$0FB4,$0300,$0C97,$0500,$0FF0  ; Sunset
        .word $0014,$0CEF,$00CF,$06CF,$0002,$08AC,$0023,$0FFF  ; Ocean
        .word $0334,$0EEF,$0AAB,$099C,$0111,$0889,$0223,$0FFF  ; Slate
        .word $0508,$0FDF,$0F6C,$0F9D,$0305,$0C9C,$0406,$0FF0  ; Candy
        .word $0320,$0FE8,$0F90,$0FC4,$0110,$0CA6,$0210,$0FF0  ; Amber

theme_names:
        .word tn0, tn1, tn2, tn3, tn4, tn5, tn6, tn7
tn0: .byte "Classic VGA", 0
tn1: .byte "Midnight", 0
tn2: .byte "Forest", 0
tn3: .byte "Sunset", 0
tn4: .byte "Ocean", 0
tn5: .byte "Slate", 0
tn6: .byte "Candy", 0
tn7: .byte "Amber", 0
str_theme_hdr:  .byte "Theme presets:", 0
str_theme_hint: .byte "Left / Right to change", 0
