; ============================================================================
; UnoDOS/SNES Tracker (proc 9) - milestone 3. Port of genesis/tracker.i onto
; the SPC700 DSP: the 4 pattern channels map to DSP voices 0-3 (the Genesis
; PSG's 3 tones + noise; on the SNES all four are square-wave tones - the
; noise channel is a documented deviation, the driver only synthesises tones).
; Pattern format is the shared 32 rows x 4 channels x (note, instr).
;
;   arrows     move the cursor (row / channel)
;   q / w      note down / up a semitone (empty cell starts at C-2)
;   e          cycle the cell's instrument
;   x          clear the cell
;   d          load the demo song
;   space      play / stop (loops the pattern, 6 frames/row)
;   s / l      save / load SONG.TRK in cartridge SRAM
; ============================================================================

TK_ROWS  = 32
TK_CHANS = 4
TK_VIEW  = 12
TK_PATLEN = TK_ROWS*TK_CHANS*2

; tk_cell - A0 = row, A1 = chan -> X = byte offset into v_tkpat (note, instr)
.proc tk_cell
.a16
.i16
        lda A0
        asl a
        asl a                   ; row*4
        clc
        adc A1
        asl a                   ; *2 bytes
        tax
        rts
.endproc

; tk_silence - key off all four voices
.proc tk_silence
.a16
.i16
        stz LC0
@v:     lda LC0
        sta A0
        jsr spc_note_off
        inc LC0
        lda LC0
        cmp #TK_CHANS
        bcc @v
        rts
.endproc

; tk_stop
.proc tk_stop
.a16
.i16
        stz v_tk_playing
        jsr tk_silence
        rts
.endproc

; tk_trigger_row - A = row: key on this row's notes on voices 0-3
.proc tk_trigger_row
.a16
.i16
        sta S4                  ; row
        stz LC0                 ; channel
@ch:    lda S4
        sta A0
        lda LC0
        sta A1
        jsr tk_cell             ; X = cell offset
        sep #$20
.a8
        lda v_tkpat,x
        rep #$20
        and #$00FF
        beq @next               ; note 0 = sustain, no retrigger
        dec a                   ; 0..23 -> note_pitch index
        sta A0                  ; (temp: note index)
        lda LC0
        sta S5                  ; voice
        lda A0
        jsr spc_note_idx        ; A1 = pitch
        lda S5
        sta A0                  ; voice
        jsr spc_note_on
@next:  inc LC0
        lda LC0
        cmp #TK_CHANS
        bcc @ch
        rts
.endproc

; tracker_tick - playback sequencer (main loop, 6 frames/row)
.proc tracker_tick
.a16
.i16
        lda v_tk_playing
        bne :+
        rts
:       lda v_frame
        sec
        sbc v_tk_last
        cmp #6
        bcc @out
        lda v_frame
        sta v_tk_last
        lda v_tk_prow
        inc a
        cmp #TK_ROWS
        bcc @rok
        lda #0
@rok:   sta v_tk_prow
        jsr tk_trigger_row
        ; redraw if Tracker (proc 9) is topmost
        lda v_zcount
        beq @out
        dec a
        jsr zent_x
        sep #$20
.a8
        lda v_wintab+WPROC,x
        rep #$20
        and #$00FF
        cmp #9
        bne @out
        jsr redraw_topmost
@out:   rts
.endproc

; tk_fmt_cell - X = cell offset, P1 = dest ptr: write "C#2 S" / "--- -" (5 ch)
.proc tk_fmt_cell
.a16
.i16
        phx
        sep #$20
.a8
        lda v_tkpat,x
        rep #$20
        and #$00FF
        bne @note
        ; empty: "--- -"
        ldy #$0000
        lda #'-'
        sep #$20
.a8
        sta (P1),y
        iny
        sta (P1),y
        iny
        sta (P1),y
        iny
        lda #' '
        sta (P1),y
        iny
        lda #'-'
        sta (P1),y
        rep #$20
        bra @done
@note:  dec a                   ; 0..23
        sta S6                  ; note-1
        ; semitone = (note-1) % 12, octave = (note-1)/12 + 2
        lda S6
        sta S0
        lda #12
        sta S1
        jsr div16               ; A = quotient (octave-2), S0 = remainder (semitone)
        clc
        adc #2                  ; octave digit value
        clc
        adc #'0'
        sta S7                  ; octave char
        lda S0
        asl a                   ; semitone*2
        clc
        adc #.loword(tkg_notenames)
        sta P0                  ; -> 2-char name
        ldy #$0000
        sep #$20
.a8
        lda (P0)                ; first name char (16-bit P0, dp indirect)
        sta (P1),y
        rep #$20
        ldy #$0001
        sep #$20
.a8
        lda (P0),y              ; second name char
        sta (P1),y
        lda S7
        ldy #$0002
        sta (P1),y              ; octave
        lda #' '
        ldy #$0003
        sta (P1),y
        rep #$20
        ; instrument letter
        plx
        phx
        inx
        sep #$20
.a8
        lda v_tkpat,x           ; instr 0-3
        rep #$20
        and #$00FF
        clc
        adc #.loword(tkg_instname)
        sta P0
        sep #$20
.a8
        lda (P0)
        ldy #$0004
        sta (P1),y
        rep #$20
@done:  plx
        rts
.endproc

; tracker_draw - S2 = window offset
.proc tracker_draw
.a16
.i16
        ; title + channel header
        lda #.loword(str_tk_title)
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
        ; keep cursor row in view (scroll v_tk_top)
        lda v_tk_row
        cmp v_tk_top
        bcs @t1
        sta v_tk_top
@t1:    lda v_tk_top
        clc
        adc #TK_VIEW
        dec a
        cmp v_tk_row
        bcs @t2
        lda v_tk_row
        sec
        sbc #(TK_VIEW-1)
        sta v_tk_top
@t2:    ; rows
        stz LC1                 ; visible row counter
@row:   lda LC1
        cmp #TK_VIEW
        bcc :+
        jmp @foot
:       lda v_tk_top
        clc
        adc LC1
        sta S3                  ; pattern row index
        cmp #TK_ROWS
        bcc :+
        jmp @nextrow            ; past the pattern
:
        ; build the row string into v_tkbuf
        lda #.loword(v_tkbuf)
        sta P1
        lda S3                  ; row number -> 2 digits
        sta S0
        lda #10
        sta S1
        jsr div16               ; A=tens, S0=ones
        clc
        adc #'0'
        sep #$20
.a8
        ldy #$0000
        sta (P1),y
        rep #$20
        lda S0
        clc
        adc #'0'
        ldy #$0001
        sep #$20
.a8
        sta (P1),y
        lda #' '
        ldy #$0002
        sta (P1),y
        rep #$20
        lda #3
        sta S4                  ; write index into v_tkbuf
        stz LC0                 ; channel
@cell:  lda #.loword(v_tkbuf)
        clc
        adc S4
        sta P1                  ; dest ptr
        lda S3
        sta A0
        lda LC0
        sta A1
        jsr tk_cell             ; X = cell offset
        jsr tk_fmt_cell         ; writes 5 chars
        lda S4
        clc
        adc #5
        sta S4
        ; separating space
        lda #.loword(v_tkbuf)
        clc
        adc S4
        sta P0
        sep #$20
.a8
        lda #' '
        sta (P0)
        rep #$20
        inc S4
        inc LC0
        lda LC0
        cmp #TK_CHANS
        bcc @cell
        ; NUL terminate
        lda #.loword(v_tkbuf)
        clc
        adc S4
        sta P0
        sep #$20
.a8
        lda #0
        sta (P0)
        rep #$20
        ; attribute: cursor row inverted, play row accent
        lda #ATTR_NORM
        sta S5
        lda S3
        cmp v_tk_row
        bne @nc
        lda #ATTR_INV
        sta S5
@nc:    lda v_tk_playing
        beq @drw
        lda S3
        cmp v_tk_prow
        bne @drw
        lda S5
        cmp #ATTR_INV
        beq @drw
        lda #ATTR_ACC
        sta S5
@drw:   lda #.loword(v_tkbuf)
        sta P0
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        clc
        adc #3
        clc
        adc LC1
        sta A1
        lda S5
        sta A4
        jsr draw_str
@nextrow:
        inc LC1
        jmp @row
@foot:  lda #.loword(str_tk_foot)
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

; tracker_load_demo - a short arpeggio pattern
.proc tracker_load_demo
.a16
.i16
        ; clear
        ldx #$0000
        sep #$20
.a8
@clr:   stz v_tkpat,x
        inx
        cpx #TK_PATLEN
        rep #$20
        sep #$20
.a8
        cpx #TK_PATLEN
        bne @clr
        rep #$20
        ; channel 0: C-2, E-2, G-2, C-3 every 4 rows (notes 1,5,8,13)
        ldx #$0000
        lda #0
        sta S0                  ; row
@d:     lda S0
        and #3
        bne @nx
        lda S0
        asl a
        asl a
        asl a                   ; row*4 chans *2 = row*8, channel 0
        tax
        lda S0
        lsr a
        lsr a                   ; (row/4) 0..7
        and #3
        tay
        sep #$20
.a8
        lda demo_notes,y
        sta v_tkpat,x           ; note
        lda #0
        sta v_tkpat+1,x         ; instr 0
        rep #$20
@nx:    inc S0
        lda S0
        cmp #TK_ROWS
        bcc @d
        rts
.endproc

; tracker_key - A1 = event data (raw<<8 | ascii)
.proc tracker_key
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
        beq @left
        cmp #$4E
        beq @right
        lda S0
        cmp #'q'
        bne :+
        jmp @ndn
:       cmp #'w'
        bne :+
        jmp @nup
:       cmp #'e'
        bne :+
        jmp @inst
:       cmp #'x'
        bne :+
        jmp @clr
:       cmp #'d'
        bne :+
        jmp @demo
:       cmp #' '
        bne :+
        jmp @play
:       cmp #'s'
        bne :+
        jmp @save
:       cmp #'l'
        bne :+
        jmp @load
:       rts
@rdj:   jmp @rd                 ; trampoline (the cursor handlers are far from @rd)
@up:    lda v_tk_row
        beq @rdj
        dec a
        sta v_tk_row
        bra @rdj
@down:  lda v_tk_row
        cmp #(TK_ROWS-1)
        bcs @rdj
        inc a
        sta v_tk_row
        bra @rdj
@left:  lda v_tk_chan
        beq @rdj
        dec a
        sta v_tk_chan
        bra @rdj
@right: lda v_tk_chan
        cmp #(TK_CHANS-1)
        bcs @rdj
        inc a
        sta v_tk_chan
        bra @rdj
@ndn:   jsr tk_curcell          ; X = cursor cell offset
        sep #$20
.a8
        lda v_tkpat,x
        bne :+
        lda #2                  ; empty -> C-2 (then dec to 1 below)
:       dec a
        bne :+
        lda #1
:       sta v_tkpat,x
        rep #$20
        bra @rd
@nup:   jsr tk_curcell
        sep #$20
.a8
        lda v_tkpat,x
        bne :+
        lda #0
:       inc a
        cmp #25
        bcc :+
        lda #24
:       sta v_tkpat,x
        rep #$20
        bra @rd
@inst:  jsr tk_curcell
        inx                     ; instrument byte
        sep #$20
.a8
        lda v_tkpat,x
        inc a
        and #3
        sta v_tkpat,x
        rep #$20
        bra @rd
@clr:   jsr tk_curcell
        sep #$20
.a8
        stz v_tkpat,x
        stz v_tkpat+1,x
        rep #$20
        bra @rd
@demo:  jsr tracker_load_demo
        bra @rd
@play:  lda v_tk_playing
        bne @stop
        lda #1
        sta v_tk_playing
        stz v_tk_prow
        lda v_frame
        sta v_tk_last
        lda #0
        jsr tk_trigger_row
        bra @rd
@stop:  jsr tk_stop
        bra @rd
@save:  jsr tk_save_sram
        bra @rd
@load:  jsr tk_load_sram
@rd:    jsr redraw_topmost
        rts
.endproc

; tk_curcell -> X = byte offset of the cursor cell
.proc tk_curcell
.a16
.i16
        lda v_tk_row
        sta A0
        lda v_tk_chan
        sta A1
        jsr tk_cell
        rts
.endproc

; tk_save_sram - save the 256-byte pattern as SONG.TRK
.proc tk_save_sram
.a16
.i16
        lda #.loword(str_tk_name)
        sta P1
        lda #.loword(v_tkpat)
        sta P0
        lda #TK_PATLEN
        sta A1
        jsr sram_save
        rts
.endproc

; tk_load_sram - load SONG.TRK back into the pattern (if present)
.proc tk_load_sram
.a16
.i16
        lda #.loword(str_tk_name)
        sta P1
        jsr sram_find           ; A = index or $FFFF
        cmp #$FFFF
        beq @out
        pha
        lda #.loword(v_tkpat)
        sta P0
        lda #TK_PATLEN
        sta A2
        pla
        jsr sram_read
@out:   rts
.endproc

; tracker_init - boot: clear cursor/playback state + load the demo song
.proc tracker_init
.a16
.i16
        stz v_tk_row
        stz v_tk_chan
        stz v_tk_top
        stz v_tk_playing
        stz v_tk_prow
        jsr tracker_load_demo
        rts
.endproc

; ---- data ----
.segment "RODATA"
str_t_tracker: .byte "Tracker", 0
name_tracker:  .byte "Tracker", 0
str_tk_title:  .byte "Tracker  Ch0  Ch1  Ch2  Ch3", 0
str_tk_foot:   .byte "qw:note e:ins x:clr sp:play", 0
str_tk_name:   .byte "SONG.TRK", 0, 0, 0, 0
tkg_notenames: .byte "C-C#D-D#E-F-F#G-G#A-A#B-"
tkg_instname:  .byte "SWTN"
demo_notes:    .byte 1, 5, 8, 13   ; C-2 E-2 G-2 C-3
