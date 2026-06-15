; ============================================================================
; UnoDOS/SNES theme engine + Theme app (proc 8) - milestone 3. Ported from
; genesis/theme.i: 8 shared presets (roles: desktop, accent, accent2, text)
; plus a custom RGB editor. Applying a theme rewrites the themed entries of
; the four UI palette lines (the fixed game colours at indices 5-15 of lines
; 2/3 are untouched).
;
; CGRAM is write-only outside vblank, so - unlike the Genesis main-loop CRAM
; writes - apply_theme edits the WRAM palette shadow (v_pal) and raises
; v_pal_dirty; the NMI DMAs the shadow to CGRAM (FlushPalette), matching the
; port's shadow+DMA rule. Colours are BGR555 (5-bit channels, finer than the
; Genesis 3-bit editor).
; ============================================================================

; thm_map: (CGRAM colour index, role) byte pairs; role 0=desktop 1=accent
; 2=accent2 3=text. $FF terminates (read 16 bits at a time: low byte = index).
;   line 0 NORM (0-4)   line 1 INV (16-20)
;   line 2 ACC (32-35)  line 3 KEY (48-51)   [indices 5-15 = game colours]

; theme_init - boot: active colours = preset 0 (Classic VGA = the boot palette)
.proc theme_init
.a16
.i16
        ldx #$0000
@cp:    lda thm_presets,x
        sta v_theme,x
        inx
        inx
        cpx #8
        bne @cp
        stz v_thm_sel
        stz v_thm_slot
        rts
.endproc

; apply_theme - copy the active role colours into the themed shadow entries,
; then flag the NMI to flush. All 16-bit (the map is read a word at a time).
.proc apply_theme
.a16
.i16
        ldy #$0000
@e:     lda thm_map,y           ; A = index | (role<<8)
        and #$00FF
        cmp #$00FF
        beq @done
        lda thm_map,y
        pha
        and #$00FF
        asl a                   ; index*2 -> v_pal word offset
        sta S0
        pla
        xba                     ; role to low byte
        and #$00FF
        asl a                   ; role*2 -> v_theme word offset
        tax
        lda v_theme,x
        ldx S0
        sta v_pal,x
        iny
        iny
        bra @e
@done:  lda #1
        sta v_pal_dirty
        rts
.endproc

; theme_apply_preset - A = preset index: load it into v_theme + apply
.proc theme_apply_preset
.a16
.i16
        asl a
        asl a
        asl a                   ; *8 bytes per preset
        tax
        ldy #$0000
@cp:    lda thm_presets,x
        sta v_theme,y
        inx
        inx
        iny
        iny
        cpy #8
        bne @cp
        jsr apply_theme
        rts
.endproc

; theme_draw - S2 = window offset
.proc theme_draw
.a16
.i16
        ; title
        lda #.loword(str_th_title)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        inc a
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        ; preset rows
        stz LC0
@row:   lda LC0
        cmp #8
        bcs @custom
        ; selection bar
        lda LC0
        cmp v_thm_sel
        bne @nm
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        clc
        adc #3
        clc
        adc LC0
        sta A1
        lda v_wintab+WW,x
        sec
        sbc #2
        sta A2
        lda #1
        sta A3
        lda #(ATTR_INV+T_SOLBG)
        sta A4
        jsr fill_cells
        lda #ATTR_INV
        bra @nmattr
@nm:    lda #ATTR_NORM
@nmattr:
        sta S3                  ; row attr
        lda LC0
        asl a
        tax
        lda thm_name_tab,x
        sta P0
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc #3
        clc
        adc LC0
        sta A1
        lda S3
        sta A4
        jsr draw_str
        inc LC0
        bra @row
@custom:
        ; custom editor: "Slot n" + the slot colour's R/G/B (0-31). Only the
        ; fmt_dec-safe S-regs are used across draws: S3 row, S4 base, S7 letter.
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta S4                  ; left col
        lda v_wintab+WY,x
        clc
        adc #12
        sta S3                  ; editor row
        lda #.loword(str_th_slot)
        sta P0
        lda S4
        sta A0
        lda S3
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        lda v_thm_slot
        sta S0
        jsr fmt_dec
        lda #.loword(v_numbuf)
        sta P0
        lda S4
        clc
        adc #5
        sta A0
        lda S3
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        ; R/G/B of v_theme[slot]; re-read the word each time (S2 = window!)
        lda #'R'
        sta S7
        lda #0
        ldy #7
        jsr theme_chan
        lda #'G'
        sta S7
        lda #5
        ldy #12
        jsr theme_chan
        lda #'B'
        sta S7
        lda #10
        ldy #17
        jsr theme_chan
        ; footer
        lda #.loword(str_th_foot)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        sec
        sbc #2
        sta A1
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        rts
.endproc

; theme_chan - draw one channel "Xnn" of v_theme[slot] at editor row S3,
; base col S4. A = channel shift (0=R,5=G,10=B), Y = column offset, S7 = letter.
; The column is kept on the stack across fmt_dec (which clobbers S0/S1/S5/S6).
.proc theme_chan
.a16
.i16
        sta S0                  ; shift amount
        tya
        clc
        adc S4                  ; col = base + offset
        pha
        lda v_thm_slot
        asl a
        tax
        lda v_theme,x           ; colour word
        ldy S0
        beq @got
@shl:   lsr a
        dey
        bne @shl
@got:   and #$001F
        sta S0                  ; channel value -> fmt_dec input
        pla
        pha                     ; peek col
        sta A0
        lda S3
        sta A1
        lda S7
        sta A2
        lda #ATTR_NORM
        sta A4
        jsr draw_char
        jsr fmt_dec             ; S0 -> v_numbuf
        lda #.loword(v_numbuf)
        sta P0
        pla                     ; col
        inc a
        sta A0
        lda S3
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        rts
.endproc

; theme_edit_chan - S1 = channel shift (0=R,5=G,10=B): +4 step (wrap 32)
.proc theme_edit_chan
.a16
.i16
        lda v_thm_slot
        asl a
        tax
        lda v_theme,x
        sta S2                  ; word
        ; extract channel
        lda S2
@sh:    ldy S1
        beq @got
@shl:   lsr a
        dey
        bne @shl
@got:   and #$001F
        clc
        adc #4
        and #$001F              ; +4, wrap at 32
        sta S3                  ; new value
        ; clear channel in S2: mask = $1F << shift
        lda #$001F
        ldy S1
        beq @mok
@msh:   asl a
        dey
        bne @msh
@mok:   eor #$FFFF
        and S2
        sta S2
        ; new value << shift
        lda S3
        ldy S1
        beq @vok
@vsh:   asl a
        dey
        bne @vsh
@vok:   ora S2
        ldx v_thm_slot          ; wait - need word offset
        pha
        lda v_thm_slot
        asl a
        tax
        pla
        sta v_theme,x
        jsr apply_theme
        rts
.endproc

; theme_key - A1 = event data (raw<<8 | ascii)
.proc theme_key
.a16
.i16
        lda A1
        and #$00FF
        sta S0                  ; ascii
        lda A1
        xba
        and #$00FF
        sta S1                  ; raw
        lda S1
        cmp #$4C
        beq @up
        cmp #$4D
        beq @down
        cmp #$4F
        beq @slotl
        cmp #$4E
        beq @slotr
        lda S0
        cmp #13
        beq @apply
        cmp #'r'
        beq @editr
        cmp #'g'
        beq @editg
        cmp #'b'
        beq @editb
        rts
@up:    lda v_thm_sel
        beq @rd
        dec a
        sta v_thm_sel
        bra @rd
@down:  lda v_thm_sel
        cmp #7
        bcs @rd
        inc a
        sta v_thm_sel
        bra @rd
@slotl: lda v_thm_slot
        bne @sl1
        lda #4
@sl1:   dec a
        sta v_thm_slot
        bra @rd
@slotr: lda v_thm_slot
        inc a
        cmp #4
        bcc @sr1
        lda #0
@sr1:   sta v_thm_slot
        bra @rd
@apply: lda v_thm_sel
        jsr theme_apply_preset
        bra @rd
@editr: stz S1
        jsr theme_edit_chan
        bra @rd
@editg: lda #5
        sta S1
        jsr theme_edit_chan
        bra @rd
@editb: lda #10
        sta S1
        jsr theme_edit_chan
@rd:    jsr redraw_topmost
        rts
.endproc

; ---- data ----
.segment "RODATA"
str_t_theme:   .byte "Theme", 0
name_theme:    .byte "Theme", 0
str_th_title:  .byte "Theme", 0
str_th_slot:   .byte "Slot", 0
str_th_foot:   .byte "Enter:set rgb </>:slot", 0
str_th_1:      .byte "Classic VGA", 0
str_th_2:      .byte "Midnight", 0
str_th_3:      .byte "Forest", 0
str_th_4:      .byte "Sunset", 0
str_th_5:      .byte "Ocean", 0
str_th_6:      .byte "Slate", 0
str_th_7:      .byte "Candy", 0
str_th_8:      .byte "Amber", 0
thm_name_tab:
        .word str_th_1, str_th_2, str_th_3, str_th_4
        .word str_th_5, str_th_6, str_th_7, str_th_8
thm_map:
        .byte 0,0,  1,3,  2,0,  3,1,  4,2
        .byte 16,0, 17,0, 18,3, 19,1, 20,2
        .byte 32,0, 33,1, 34,0, 35,3
        .byte 48,0, 49,0, 50,1, 51,3
        .byte $FF, $FF
