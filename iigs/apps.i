; ============================================================================
; UnoDOS/Apple IIGS - M2 apps: Files (proc 7) + Notepad (proc 2).
;
; Files lists the FAT12 root directory (v_dir_list, filled by fat_list_root)
; and opens the selected file into Notepad.  Notepad is an append-style text
; editor over NBUF (bank-0, 4 KB): printable keys + CR append, backspace
; removes the last char, Ctrl-S writes the buffer back to disk via
; fat_save_file.  Both render with the kernel's cell primitives.
; ============================================================================

v_np_loaded = VARS+$30E       ; 1 = Files preloaded NBUF before launching Notepad

; draw_nchars: P0 = ptr (bank 0), A2 = count, A0 = cx, A1 = cy, A4 = attr.
.a16
.i16
draw_nchars:
        lda A4
        jsr set_attr
        lda A0
        asl a
        asl a
        sta PX
        lda A1
        asl a
        asl a
        asl a
        sta PY
        jsr calc_gp_px
        lda P0
        sta STRP
        sep #$20
        stz STRP+2
        rep #$20
        stz S7
@ch:    lda S7
        cmp A2
        bcs @done
        ldy S7
        sep #$20
        lda [STRP],y
        rep #$20
        and #$00FF
        jsr render_glyph
        inc S7
        bra @ch
@done:  rts

; ============================================================================
; Files (proc 7)
; ============================================================================
.a16
.i16
files_draw:
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta S3                 ; left col
        lda v_wintab+WY,x
        clc
        adc #2
        sta S4                 ; top row
        stz LC0                ; entry index
@row:   lda LC0
        cmp v_fs_dircount
        bcs @done
        lda LC0
        asl a
        asl a
        asl a
        asl a                  ; *16
        clc
        adc #.loword(v_dir_list)
        sta P0
        lda #11
        sta A2
        lda S3
        sta A0
        lda S4
        clc
        adc LC0
        sta A1
        lda #ATTR_NORM
        sta A4
        ldx v_fs_sel
        cpx LC0
        bne @na
        lda #ATTR_INV
        sta A4
@na:    jsr draw_nchars
        inc LC0
        bra @row
@done:  rts

.a16
.i16
files_key:
        lda S0
        cmp #$0A
        beq @down
        cmp #$0B
        beq @up
        cmp #$0D
        beq @open
        rts
@down:  lda v_fs_sel
        inc a
        cmp v_fs_dircount
        bcc @set
        lda #0
        bra @set
@up:    lda v_fs_sel
        bne @dec
        lda v_fs_dircount
        beq @set0
        dec a
        bra @set
@set0:  lda #0
        bra @set
@dec:   dec a
@set:   sta v_fs_sel
        jsr redraw_topmost
        rts
@open:  jsr files_open
        rts

; files_open: load the selected file into NBUF and launch Notepad (proc 2).
.a16
.i16
files_open:
        lda v_fs_sel
        cmp v_fs_dircount
        bcs @out
        asl a
        asl a
        asl a
        asl a
        clc
        adc #.loword(v_dir_list)
        sta DPTR
        ldy #0
        sep #$20
@cn:    lda (DPTR),y
        sta v_np_name,y
        iny
        cpy #11
        bcc @cn
        rep #$20
        ldy #12
        lda (DPTR),y
        sta F0                 ; first cluster
        ldy #14
        lda (DPTR),y           ; size
        cmp #NBUFSZ
        bcc @okc
        lda #NBUFSZ
@okc:   sta v_np_len
        sta F2
        lda F0
        sta A0
        lda #.loword(NBUF)
        sta P0
        jsr fat_read_file
        stz v_np_dirty
        lda #1
        sta v_np_loaded
        lda #2
        jsr launch_app
@out:   rts

; ============================================================================
; Notepad (proc 2)
; ============================================================================

; notepad_new: reset to an empty buffer with the default name.
.a16
.i16
notepad_new:
        stz v_np_len
        stz v_np_dirty
        ldx #0
        sep #$20
@cn:    lda f:note_defname,x
        sta v_np_name,x
        inx
        cpx #11
        bcc @cn
        rep #$20
        rts

.a16
.i16
notepad_draw:
        ; name header (accent)
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc #2
        sta A1
        lda #.loword(v_np_name)
        sta P0
        lda #11
        sta A2
        lda #ATTR_ACC
        sta A4
        jsr draw_nchars
        ; text flow
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta F0                 ; left col
        sta F3                 ; cur col
        lda v_wintab+WY,x
        clc
        adc #4
        sta F4                 ; cur row
        lda v_wintab+WX,x
        clc
        adc v_wintab+WW,x
        dec a
        dec a
        sta F1                 ; max col (exclusive)
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        dec a
        sta F5                 ; max row (exclusive)
        stz LC0                ; byte index
@tl:    lda LC0
        cmp v_np_len
        bcs @caret
        lda F4
        cmp F5
        bcs @caret
        ldx LC0
        sep #$20
        lda NBUF,x
        rep #$20
        and #$00FF
        cmp #$0D
        beq @nl
        sta A2
        lda F3
        sta A0
        lda F4
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_char
        inc F3
        lda F3
        cmp F1
        bcc @nextb
        lda F0
        sta F3
        inc F4
        bra @nextb
@nl:    lda F0
        sta F3
        inc F4
@nextb: inc LC0
        bra @tl
@caret: lda F4
        cmp F5
        bcs @done
        lda #'_'
        sta A2
        lda F3
        sta A0
        lda F4
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_char
@done:  rts

.a16
.i16
notepad_key:
        lda S0
        cmp #$13               ; Ctrl-S -> save
        beq @save
        cmp #$08               ; backspace
        beq @bs
        cmp #$7F
        beq @bs
        cmp #$0D               ; CR -> insert
        beq @ins
        cmp #$20
        bcc @out               ; other control char
        cmp #$7F
        bcs @out
@ins:   lda v_np_len
        cmp #NBUFSZ
        bcs @out
        tax
        lda S0
        sep #$20
        sta NBUF,x
        rep #$20
        inc v_np_len
        lda #1
        sta v_np_dirty
        jsr redraw_topmost
@out:   rts
@bs:    lda v_np_len
        beq @out
        dec a
        sta v_np_len
        lda #1
        sta v_np_dirty
        jsr redraw_topmost
        rts
@save:  lda #.loword(NBUF)
        sta P0
        lda v_np_len
        sta F2
        jsr fat_save_file
        stz v_np_dirty
        jsr redraw_topmost
        rts

note_defname: .byte "NOTE    TXT"
