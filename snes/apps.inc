; ============================================================================
; UnoDOS/SNES apps (milestone 2): Notepad (proc 2) + Files (proc 7), backed
; by the USV1 SRAM mini-FS (sram.inc).
;
; Notepad is an append-style editor: soft-keyboard / pad keys append to a 2 KB
; buffer, Backspace deletes the last char, Enter inserts a newline, F1 saves
; under the current name to SRAM. (The Genesis port's full caret/line
; navigation is a documented M2 simplification - the storage round-trip is
; what M2 proves.) Files lists the SRAM directory, opens a file into Notepad
; (Enter), and deletes (X/Backspace); arrows / pad move the selection.
; ============================================================================

; ---- Notepad (proc 2) ----

; notepad_set_demo - seed the buffer with demo text + name DEMO.TXT
.proc notepad_set_demo
.a16
.i16
        lda #.loword(demo_text)
        sta P0
        ldy #$0000
@cp:    sep #$20
.a8
        lda (P0),y
        beq @done
        sta v_npbuf,y
        rep #$20
.a16
        iny
        bra @cp
@done:  rep #$20
.a16
        sty v_np_len
        lda #.loword(str_demo_name)
        sta P0
        jsr np_setname
        lda #$01
        sta v_np_dirty
        rts
.endproc

; np_setname - copy 12 bytes from (P0) to v_np_name
.proc np_setname
.a16
.i16
        ldy #$0000
@cp:    sep #$20
.a8
        lda (P0),y
        sta v_np_name,y
        rep #$20
.a16
        iny
        cpy #12
        bne @cp
        rts
.endproc

; np_save - save v_npbuf (v_np_len bytes) under v_np_name to SRAM
.proc np_save
.a16
.i16
        lda #.loword(v_np_name)
        sta P1
        lda #.loword(v_npbuf)
        sta P0
        lda v_np_len
        sta A1
        jsr sram_save
        cmp #$FFFF
        beq @out
        stz v_np_dirty
@out:   rts
.endproc

; notepad_draw - S2 = window entry offset (preserved across the draw)
.proc notepad_draw
.a16
.i16
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta S3                  ; text x (left)
        lda v_wintab+WW,x
        sec
        sbc #2
        sta S4                  ; cols
        lda v_wintab+WH,x
        sec
        sbc #3
        sta S5                  ; visible content rows
        lda S3
        sta A6                  ; cur cx
        ldx S2
        lda v_wintab+WY,x
        inc a
        sta S6                  ; cur cy
        stz LC0                 ; rows used
        stz LC1                 ; source index
@ch:    lda LC0
        cmp S5
        bcs @status
        lda LC1
        cmp v_np_len
        bcs @caret
        ldx LC1
        sep #$20
.a8
        lda v_npbuf,x
        rep #$20
.a16
        and #$00FF
        cmp #13
        beq @nl
        sta A2                  ; char
        lda A6
        sta A0
        lda S6
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_char
        inc A6                  ; cx++
        lda A6
        sec
        sbc S3
        cmp S4
        bcc @next
        ; wrap
        lda S3
        sta A6
        inc S6
        inc LC0
@next:  inc LC1
        bra @ch
@nl:    lda S3
        sta A6
        inc S6
        inc LC0
        inc LC1
        bra @ch
@caret: ; caret block at the current position
        lda LC0
        cmp S5
        bcs @status
        lda A6
        sta A0
        lda S6
        sta A1
        lda #32
        sta A2
        lda #ATTR_INV
        sta A4
        jsr draw_char
@status:
        ; status bar (window row h-2): name + length
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        sec
        sbc #2
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
        lda #.loword(v_np_name)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        sec
        sbc #2
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
        lda v_np_len
        sta S0
        jsr fmt_dec
        lda #.loword(v_numbuf)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        clc
        adc v_wintab+WW,x
        sec
        sbc #6
        sta A0
        ldx S2
        lda v_wintab+WY,x
        clc
        adc v_wintab+WH,x
        sec
        sbc #2
        sta A1
        lda #ATTR_INV
        sta A4
        jsr draw_str
        rts
.endproc

; notepad_key - A1 = event data (raw<<8 | ascii)
.proc notepad_key
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
        cmp #$50                ; F1 = save
        beq @save
        lda S0
        cmp #8                  ; Backspace
        beq @bs
        cmp #13                 ; Enter -> newline
        beq @ins
        cmp #32
        bcc @done               ; control char: ignore
        cmp #127
        bcs @done
@ins:   lda v_np_len
        cmp #(NBUF-1)
        bcs @done
        ldx v_np_len
        sep #$20
.a8
        lda S0
        sta v_npbuf,x
        rep #$20
.a16
        inc v_np_len
        lda #1
        sta v_np_dirty
        bra @redraw
@bs:    lda v_np_len
        beq @done
        dec v_np_len
        lda #1
        sta v_np_dirty
        bra @redraw
@save:  jsr np_save
@redraw:
        jsr redraw_topmost
@done:  rts
.endproc

; ---- Files (proc 7) ----

; files_draw - S2 = window entry offset
.proc files_draw
.a16
.i16
        lda #.loword(str_f_hdr)
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
        jsr sram_count
        sta S3                  ; count
        stz S4                  ; row
@row:   lda S4
        cmp S3
        bcs @foot
        ldx S2
        lda v_wintab+WY,x
        clc
        adc #2
        clc
        adc S4
        sta A1                  ; row y
        lda #ATTR_NORM
        sta S5
        lda S4
        cmp v_files_sel
        bne @nm
        lda #ATTR_INV
        sta S5
@nm:    lda #.loword(v_numbuf)
        sta P1
        lda S4
        jsr sram_name
        lda #.loword(v_numbuf)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda S5
        sta A4
        jsr draw_str
        ; size, right-aligned-ish at col +14
        lda S4
        jsr sram_size
        sta S0
        jsr fmt_dec
        lda #.loword(v_numbuf)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #14
        sta A0
        ldx S2
        lda v_wintab+WY,x
        clc
        adc #2
        clc
        adc S4
        sta A1
        lda S5
        sta A4
        jsr draw_str
        inc S4
        bra @row
@foot:  lda #.loword(str_f_foot)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        inc a
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

; files_key - A1 = event data
.proc files_key
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
        cmp #$4D                ; down
        beq @down
        cmp #$4C                ; up
        beq @up
        lda S0
        cmp #13                 ; Enter -> open
        beq @open
        cmp #'d'
        beq @del
        cmp #8                  ; Backspace
        beq @del
        rts
@down:  jsr sram_count
        sta S2
        lda v_files_sel
        inc a
        cmp S2
        bcs @rd
        sta v_files_sel
        bra @rd
@up:    lda v_files_sel
        beq @rd
        dec a
        sta v_files_sel
        bra @rd
@open:  jsr sram_count
        cmp #0
        beq @rd
        lda #.loword(v_np_name)
        sta P1
        lda v_files_sel
        jsr sram_name
        lda #.loword(v_npbuf)
        sta P0
        lda #(NBUF-1)
        sta A2
        lda v_files_sel
        jsr sram_read
        sta v_np_len
        stz v_np_dirty
        lda #2
        jsr launch_app          ; Notepad
        jsr redraw_topmost
        rts
@del:   jsr sram_count
        cmp #0
        beq @rd
        lda v_files_sel
        jsr sram_delete
        jsr sram_count
        sta S2
        lda v_files_sel
        cmp S2
        bcc @rd
        lda S2
        beq @selz
        dec a
        sta v_files_sel
        bra @rd
@selz:  stz v_files_sel
@rd:    jsr redraw_topmost
        rts
.endproc

; app_key - A = proc, A1 = event data: dispatch keys to the topmost app
.proc app_key
.a16
.i16
        cmp #2
        beq @np
        cmp #4
        beq @dt
        cmp #7
        beq @files
        rts
@np:    jsr notepad_key
        rts
@dt:    jsr dostris_key
        rts
@files: jsr files_key
        rts
.endproc

; ---- data ----
.segment "RODATA"
str_t_notepad: .byte "Notepad", 0
str_t_files:   .byte "Files", 0
name_notepad:  .byte "Notepad", 0
name_files:    .byte "Files", 0
str_demo_name: .byte "DEMO.TXT", 0, 0, 0, 0
str_f_hdr:     .byte "Name          Size", 0
str_f_foot:    .byte "Enter:open  X:del", 0
demo_text:     .byte "UnoDOS SNES Notepad.", 13, "Type, F1 saves to SRAM.", 0
