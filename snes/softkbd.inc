; ============================================================================
; UnoDOS/SNES soft keyboard - kernel overlay on cell rows 22..27, drawn into
; the tilemap shadow. Clicking a key posts a normal EV_KEY (with the Amiga/
; Genesis raw codes: arrows $4C-$4F, F1 $50) so apps can't tell it from a real
; keystroke. Toggled with the pad's B button. Ported from genesis/softkbd.i,
; re-laid-out for the SNES's 32 cells (Genesis is 40) with 2-wide keys.
;
; Key entry (10 bytes): x, row(0-4), w, ascii, raw, pad, label[4 NUL-padded].
; raw $FE = the sticky Shift key.
; ============================================================================

KB_ROW0   = KBD_TOP + 1          ; first key row (22 = panel header)
KB_NKEYS  = 55
KB_SHIFTIX = 39                  ; index of the Shift key

.segment "CODE"

.proc softkbd_show
.a16
.i16
        sep #$20
.a8
        lda #1
        sta v_kb_vis
        rep #$20
.a16
        lda #$FFFF
        sta v_kb_hover
        jsr softkbd_draw
        rts
.endproc

.proc softkbd_hide
.a16
.i16
        sep #$20
.a8
        stz v_kb_vis
        stz v_kb_shift
        rep #$20
.a16
        jsr repaint_all
        rts
.endproc

.proc softkbd_draw
.a16
.i16
        ; panel slab over rows KBD_TOP..27
        stz A0
        lda #KBD_TOP
        sta A1
        lda #SCRW_C
        sta A2
        lda #(SCRH_C - KBD_TOP)
        sta A3
        lda #(ATTR_KEY + T_SOLBG)
        sta A4
        jsr fill_cells
        stz LC0                 ; key index (drawkey doesn't touch LC0)
@key:   lda LC0
        jsr softkbd_drawkey
        inc LC0
        lda LC0
        cmp #KB_NKEYS
        bne @key
        rts
.endproc

; softkbd_keyent: A = index -> P1 = &kb_keys[index*10]
.proc softkbd_keyent
.a16
.i16
        and #$00FF
        sta S0
        asl a
        asl a
        asl a                   ; *8
        clc
        adc S0                  ; *9
        clc
        adc S0                  ; *10
        clc
        adc #.loword(kb_keys)
        sta P1
        rts
.endproc

; softkbd_drawkey: A = key index
.proc softkbd_drawkey
.a16
.i16
        sta S6                  ; index
        jsr softkbd_keyent      ; P1 = entry
        ; palette attr -> S4 (hovered key, or latched Shift -> inverted)
        lda #ATTR_KEY
        sta S4
        lda S6
        cmp v_kb_hover
        beq @inv
        sep #$20
.a8
        ldy #4
        lda (P1),y              ; raw
        cmp #$FE
        bne @pal
        lda v_kb_shift
        beq @pal
        rep #$20
.a16
        bra @inv
@pal:   rep #$20
.a16
        bra @fill
@inv:   lda #ATTR_INV
        sta S4
@fill:  ; A0 = x, A1 = KB_ROW0+row, A2 = w, A3 = 1, A4 = attr+solid
        sep #$20
.a8
        ldy #0
        lda (P1),y
        rep #$20
.a16
        and #$00FF
        sta A0
        sta S5                  ; key x
        sep #$20
.a8
        ldy #1
        lda (P1),y
        rep #$20
.a16
        and #$00FF
        clc
        adc #KB_ROW0
        sta A1
        sep #$20
.a8
        ldy #2
        lda (P1),y
        rep #$20
.a16
        and #$00FF
        sta A2
        sta S7                  ; key w
        lda #1
        sta A3
        lda S4
        clc
        adc #T_SOLBG
        sta A4
        jsr fill_cells
        ; label length
        ldy #6
        ldx #0
@len:   cpx #4
        bcs @gotlen
        sep #$20
.a8
        lda (P1),y
        rep #$20
.a16
        and #$00FF
        beq @gotlen
        inx
        iny
        bra @len
@gotlen:
        txa
        sta S0                  ; len
        lda S7                  ; w
        sec
        sbc S0
        bpl @pos
        lda #0
@pos:   lsr a
        clc
        adc S5                  ; + key x
        sta A0
        ; P0 = label pointer = P1 + 6
        lda P1
        clc
        adc #6
        sta P0
        sep #$20
.a8
        ldy #1
        lda (P1),y
        rep #$20
.a16
        and #$00FF
        clc
        adc #KB_ROW0
        sta A1
        lda S4
        sta A4
        jsr draw_str
        rts
.endproc

; softkbd_key_at: A0 = cx, A1 = cy -> A = key index or $FFFF
.proc softkbd_key_at
.a16
.i16
        lda A1
        sec
        sbc #KB_ROW0
        bcc @miss
        cmp #5
        bcs @miss
        sta S0                  ; krow
        stz S1                  ; index
        lda #.loword(kb_keys)
        sta P1
@scan:  sep #$20
.a8
        ldy #1
        lda (P1),y              ; row
        rep #$20
.a16
        and #$00FF
        cmp S0
        bne @next
        sep #$20
.a8
        ldy #0
        lda (P1),y              ; x
        rep #$20
.a16
        and #$00FF
        sta S2                  ; kx
        lda A0
        cmp S2
        bcc @next               ; cx < kx
        sep #$20
.a8
        ldy #2
        lda (P1),y              ; w
        rep #$20
.a16
        and #$00FF
        clc
        adc S2                  ; kx+w
        cmp A0
        beq @next               ; cx == kx+w
        bcc @next               ; kx+w < cx
        lda S1                  ; hit
        rts
@next:  lda P1
        clc
        adc #10
        sta P1
        inc S1
        lda S1
        cmp #KB_NKEYS
        bcc @scan
@miss:  lda #$FFFF
        rts
.endproc

; softkbd_click: A0 = cx, A1 = cy
.proc softkbd_click
.a16
.i16
        jsr softkbd_key_at
        cmp #$FFFF
        bne :+
        jmp @out
:       sta S3                  ; key index
        jsr softkbd_keyent      ; P1
        sep #$20
.a8
        ldy #4
        lda (P1),y              ; raw
        cmp #$FE
        bne @normal
        ; Shift: toggle the latch + redraw
        lda v_kb_shift
        eor #$FF
        sta v_kb_shift
        rep #$20
.a16
        lda S3
        jsr softkbd_drawkey
        lda #1
        sta v_dirty
        rts
@normal:
        rep #$20
.a16
        sep #$20
.a8
        ldy #3
        lda (P1),y              ; ascii
        sta S0
        lda v_kb_shift
        beq @post
        ; sticky shift: uppercase a letter, release the latch
        lda S0
        cmp #'a'
        bcc @unlatch
        cmp #('z'+1)
        bcs @unlatch
        sec
        sbc #32
        sta S0
@unlatch:
        stz v_kb_shift
        rep #$20
.a16
        lda S3
        pha
        lda #KB_SHIFTIX
        jsr softkbd_drawkey
        lda #1
        sta v_dirty
        pla
        sta S3
        jsr softkbd_keyent      ; restore P1 (drawkey moved it)
        sep #$20
.a8
@post:  ldy #4
        lda (P1),y              ; raw
        sta S1
        rep #$20
.a16
        lda S0
        and #$00FF
        sta S0
        lda S1
        and #$00FF
        xba                     ; raw -> high byte
        ora S0
        sta A1                  ; data = (raw<<8)|ascii
        lda #EV_KEY
        sta A0
        jsr ev_post
@out:   rts
.endproc

; softkbd_hover: track the cursor, repaint the (un)hovered keys
.proc softkbd_hover
.a16
.i16
        lda v_kb_vis
        and #$00FF
        beq @out
        lda v_mouse_x
        lsr a
        lsr a
        lsr a
        sta A0
        lda v_mouse_y
        lsr a
        lsr a
        lsr a
        sta A1
        jsr softkbd_key_at
        cmp v_kb_hover
        beq @out
        sta S2                  ; new hover
        lda v_kb_hover
        sta S3                  ; old hover
        lda S2
        sta v_kb_hover
        lda S3
        cmp #$FFFF
        beq @new
        lda S3
        jsr softkbd_drawkey
        lda #1
        sta v_dirty
@new:   lda v_kb_hover
        cmp #$FFFF
        beq @out
        jsr softkbd_drawkey
        lda #1
        sta v_dirty
@out:   rts
.endproc

; ----------------------------------------------------------------------------
; Key table: x, row, w, ascii, raw, pad, label[4]   (32 cells wide, 2-wide keys)
; row0 0-12, row1 13-25, row2 26-38, row3 39-51, row4 52-54
; ----------------------------------------------------------------------------
.segment "RODATA"
kb_keys:
        ; row 0: digits + BS (0-12)
        .byte  0,0,2,'1',0,0, "1",0,0,0
        .byte  2,0,2,'2',0,0, "2",0,0,0
        .byte  4,0,2,'3',0,0, "3",0,0,0
        .byte  6,0,2,'4',0,0, "4",0,0,0
        .byte  8,0,2,'5',0,0, "5",0,0,0
        .byte 10,0,2,'6',0,0, "6",0,0,0
        .byte 12,0,2,'7',0,0, "7",0,0,0
        .byte 14,0,2,'8',0,0, "8",0,0,0
        .byte 16,0,2,'9',0,0, "9",0,0,0
        .byte 18,0,2,'0',0,0, "0",0,0,0
        .byte 20,0,2,'-',0,0, "-",0,0,0
        .byte 22,0,2,'=',0,0, "=",0,0,0
        .byte 24,0,4,8,0,0,   "BS",0,0
        ; row 1: qwertyuiop[] + RET (13-25)
        .byte  0,1,2,'q',0,0, "q",0,0,0
        .byte  2,1,2,'w',0,0, "w",0,0,0
        .byte  4,1,2,'e',0,0, "e",0,0,0
        .byte  6,1,2,'r',0,0, "r",0,0,0
        .byte  8,1,2,'t',0,0, "t",0,0,0
        .byte 10,1,2,'y',0,0, "y",0,0,0
        .byte 12,1,2,'u',0,0, "u",0,0,0
        .byte 14,1,2,'i',0,0, "i",0,0,0
        .byte 16,1,2,'o',0,0, "o",0,0,0
        .byte 18,1,2,'p',0,0, "p",0,0,0
        .byte 20,1,2,'[',0,0, "[",0,0,0
        .byte 22,1,2,']',0,0, "]",0,0,0
        .byte 24,1,4,13,0,0,  "RET",0
        ; row 2: asdfghjkl;' + ^ + F1 (26-38)
        .byte  0,2,2,'a',0,0, "a",0,0,0
        .byte  2,2,2,'s',0,0, "s",0,0,0
        .byte  4,2,2,'d',0,0, "d",0,0,0
        .byte  6,2,2,'f',0,0, "f",0,0,0
        .byte  8,2,2,'g',0,0, "g",0,0,0
        .byte 10,2,2,'h',0,0, "h",0,0,0
        .byte 12,2,2,'j',0,0, "j",0,0,0
        .byte 14,2,2,'k',0,0, "k",0,0,0
        .byte 16,2,2,'l',0,0, "l",0,0,0
        .byte 18,2,2,';',0,0, ";",0,0,0
        .byte 20,2,2,39,0,0,  39,0,0,0
        .byte 22,2,2,0,$4C,0, "^",0,0,0
        .byte 24,2,4,0,$50,0, "F1",0,0
        ; row 3: Shift + zxcvbnm,./ + Left + Down (39-51)
        .byte  0,3,3,0,$FE,0, "Sh",0,0
        .byte  3,3,2,'z',0,0, "z",0,0,0
        .byte  5,3,2,'x',0,0, "x",0,0,0
        .byte  7,3,2,'c',0,0, "c",0,0,0
        .byte  9,3,2,'v',0,0, "v",0,0,0
        .byte 11,3,2,'b',0,0, "b",0,0,0
        .byte 13,3,2,'n',0,0, "n",0,0,0
        .byte 15,3,2,'m',0,0, "m",0,0,0
        .byte 17,3,2,',',0,0, ",",0,0,0
        .byte 19,3,2,'.',0,0, ".",0,0,0
        .byte 21,3,2,'/',0,0, "/",0,0,0
        .byte 23,3,2,0,$4F,0, "<",0,0,0
        .byte 25,3,2,0,$4D,0, "v",0,0,0
        ; row 4: Esc + Space + Right (52-54)
        .byte  0,4,4,27,0,0,  "ESC",0
        .byte  4,4,18,32,0,0, "SPC",0
        .byte 22,4,4,0,$4E,0, ">",0,0,0
