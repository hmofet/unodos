; ============================================================================
; UnoDOS/SNES apps (milestone 2): Notepad (proc 2) + Files (proc 7), backed
; by the USV1 SRAM mini-FS (sram.inc).
;
; Notepad is a caret editor: soft-keyboard / pad keys INSERT at the caret in a
; 2 KB buffer, Backspace deletes the char before the caret, Enter inserts a
; newline, arrows ($4C-$4F: up/down/right/left) move the caret (up/down move by
; logical CR-delimited line, keeping the column, clamped to the line end), and
; F1 saves under the current name to SRAM. The caret renders as the inverted
; cell under it (an inverted space at end-of-text / on a newline).
; Files lists the SRAM directory, opens a file into Notepad
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
        sty v_np_car            ; caret at end of the seeded text
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
        bcc :+
        jmp @status             ; out of visible rows (bcs: out of branch range)
:       lda LC1
        cmp v_np_len
        bcs @caret
        ; the char under the caret draws inverted (S7 = attr for this char)
        lda #ATTR_NORM
        sta S7
        lda LC1
        cmp v_np_car
        bne :+
        lda #ATTR_INV
        sta S7
:       ldx LC1
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
        lda S7
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
@nl:    ; caret on a newline: show an inverted space before wrapping
        lda S7
        cmp #ATTR_INV
        bne @nl2
        lda A6
        sta A0
        lda S6
        sta A1
        lda #32
        sta A2
        lda #ATTR_INV
        sta A4
        jsr draw_char
@nl2:   lda S3
        sta A6
        inc S6
        inc LC0
        inc LC1
        jmp @ch
@caret: ; caret block after end-of-text (only when the caret sits there)
        lda v_np_car
        cmp v_np_len
        bcc @status
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
        cmp #$4F                ; left arrow
        beq @left
        cmp #$4E                ; right arrow
        beq @right
        cmp #$4C                ; up arrow
        beq @up
        cmp #$4D                ; down arrow
        beq @down
        lda S0
        cmp #8                  ; Backspace
        beq @bs
        cmp #13                 ; Enter -> newline
        beq @ins
        cmp #32
        bcc @done               ; control char: ignore
        cmp #127
        bcs @done
        bra @ins
@left:  lda v_np_car
        beq @done
        dec v_np_car
        bra @redraw
@right: lda v_np_car
        cmp v_np_len
        bcs @done
        inc v_np_car
        bra @redraw
@up:    jsr np_caret_up
        bra @redraw
@down:  jsr np_caret_down
        bra @redraw
@save:  jsr np_save
@redraw:
        jsr redraw_topmost
@done:  rts
@ins:   lda v_np_len
        cmp #(NBUF-1)
        bcs @done
        ; shift buf[car..len-1] up one byte (copy from the end downward)
        ldx v_np_len
        sep #$20
.a8
@shr:   cpx v_np_car
        beq @stow
        lda v_npbuf-1,x
        sta v_npbuf,x
        dex
        bra @shr
@stow:  lda S0
        sta v_npbuf,x           ; X = v_np_car
        rep #$20
.a16
        inc v_np_len
        inc v_np_car
        lda #1
        sta v_np_dirty
        bra @redraw
@bs:    lda v_np_car
        beq @done               ; caret at start: nothing before it
        ; shift buf[car..len-1] down one byte
        ldx v_np_car
        sep #$20
.a8
@shl:   cpx v_np_len
        bcs @shld
        lda v_npbuf,x
        sta v_npbuf-1,x
        inx
        bra @shl
@shld:  rep #$20
.a16
        dec v_np_len
        dec v_np_car
        lda #1
        sta v_np_dirty
        bra @redraw
.endproc

; np_linestart - S2 = buffer index of the first char of the caret's
; CR-delimited logical line. Clobbers A/X.
.proc np_linestart
.a16
.i16
        ldx v_np_car
@sc:    cpx #0
        beq @done
        sep #$20
.a8
        lda v_npbuf-1,x
        rep #$20
.a16
        and #$00FF
        cmp #13
        beq @done
        dex
        bra @sc
@done:  stx S2
        rts
.endproc

; np_caret_up - move the caret to the previous logical line, same column,
; clamped to that line's end. No-op on the first line. Clobbers A/X/S2-S4.
.proc np_caret_up
.a16
.i16
        jsr np_linestart        ; S2 = current line start
        lda S2
        beq @out                ; already on the first line
        lda v_np_car
        sec
        sbc S2
        sta S3                  ; column
        ; previous line start: scan back from the CR at S2-1
        ldx S2
        dex                     ; X = index of the CR ending the prev line
@psc:   cpx #0
        beq @pf
        sep #$20
.a8
        lda v_npbuf-1,x
        rep #$20
.a16
        and #$00FF
        cmp #13
        beq @pf
        dex
        bra @psc
@pf:    ; target = min(prevstart + col, prev line end = S2-1)
        txa
        clc
        adc S3
        sta S4
        lda S2
        dec a
        cmp S4
        bcs :+
        sta S4
:       lda S4
        sta v_np_car
@out:   rts
.endproc

; np_caret_down - move the caret to the next logical line, same column,
; clamped to that line's end. No-op on the last line. Clobbers A/X/S2-S5.
.proc np_caret_down
.a16
.i16
        jsr np_linestart        ; S2 = current line start
        lda v_np_car
        sec
        sbc S2
        sta S3                  ; column
        ; find the CR ending the current line
        ldx v_np_car
@fsc:   cpx v_np_len
        bcs @out                ; no CR after the caret: last line
        sep #$20
.a8
        lda v_npbuf,x
        rep #$20
.a16
        and #$00FF
        cmp #13
        beq @cr
        inx
        bra @fsc
@cr:    inx                     ; X = next line start
        stx S4
        ; next line end: CR or end-of-text
@esc:   cpx v_np_len
        bcs @eok
        sep #$20
.a8
        lda v_npbuf,x
        rep #$20
.a16
        and #$00FF
        cmp #13
        beq @eok
        inx
        bra @esc
@eok:   stx S5                  ; next line end
        lda S4
        clc
        adc S3
        cmp S5
        bcc @set
        lda S5
@set:   sta v_np_car
@out:   rts
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
        sta v_np_car            ; caret at end of the opened file
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

; ============================================================================
; Music (proc 3) - a square-wave sequencer playing Canon in D on SPC700 DSP
; voice 0 (the SNES analogue of the Genesis PSG channel-0 Music app). The
; note table is MIDI offsets into note_pitch + per-note durations (frames).
; ============================================================================

MUS_VOICE = 0

; music_note - A = note index into mus_song; key voice 0 on at its pitch,
; set v_mus_end = v_frame + its duration.
.proc music_note
.a16
.i16
        asl a                   ; *2 (word entries: offset, duration interleaved)
        asl a                   ; song entry = 2 words = 4 bytes
        tax
        lda mus_song,x          ; note-pitch index (or $FFFF = rest)
        cmp #$FFFF
        beq @rest
        pha
        lda #MUS_VOICE
        sta A0
        pla
        jsr spc_note_idx        ; A1 = pitch
        jsr spc_note_on
        bra @dur
@rest:  lda #MUS_VOICE
        sta A0
        jsr spc_note_off
@dur:   lda mus_song+2,x        ; duration (frames)
        clc
        adc v_frame
        sta v_mus_end
        rts
.endproc

; music_start
.proc music_start
.a16
.i16
        lda #1
        sta v_mus_playing
        stz v_mus_ix
        lda #0
        jsr music_note
        rts
.endproc

; music_stop
.proc music_stop
.a16
.i16
        stz v_mus_playing
        lda #MUS_VOICE
        sta A0
        jsr spc_note_off
        rts
.endproc

; music_tick - main-loop sequencer advance; refresh only if Music is topmost
.proc music_tick
.a16
.i16
        lda v_mus_playing
        bne :+
        rts
:       lda v_frame
        sec
        sbc v_mus_end
        bmi @out                ; current note still sounding
        lda v_mus_ix
        inc a
        cmp #MUS_COUNT
        bcc @ixok
        lda #0                  ; loop the song
@ixok:  sta v_mus_ix
        jsr music_note
        ; visual refresh only when Music (proc 3) is topmost
        lda v_zcount
        beq @out
        dec a
        jsr zent_x
        sep #$20
.a8
        lda v_wintab+WPROC,x
        rep #$20
        and #$00FF
        cmp #3
        bne @out
        jsr redraw_topmost
@out:   rts
.endproc

; music_key - A1 = event data; space toggles playback
.proc music_key
.a16
.i16
        lda A1
        and #$00FF
        cmp #' '
        bne @out
        lda v_mus_playing
        bne @stop
        jsr music_start
        bra @rd
@stop:  jsr music_stop
@rd:    jsr redraw_topmost
@out:   rts
.endproc

; music_draw - S2 = window offset: title, a note ribbon (current note accent),
; and the play/stop state.
.proc music_draw
.a16
.i16
        ; title
        lda #.loword(str_m_title)
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
        ; note ribbon: one cell per song note, current note in accent colour
        stz LC0                 ; note index
@n:     lda LC0
        cmp #MUS_COUNT
        bcs @state
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        clc
        adc LC0
        sta A0
        lda v_wintab+WY,x
        clc
        adc #4
        sta A1
        lda #1
        sta A2
        sta A3
        lda LC0
        cmp v_mus_ix
        bne @dim
        lda #(ATTR_ACC+T_GSOL+1) ; current note: yellow
        bra @put
@dim:   lda #(ATTR_ACC+T_GSOL+5) ; others: blue
@put:   sta A4
        jsr fill_cells
        inc LC0
        bra @n
@state: ; "Playing"/"Stopped" + hint
        lda v_mus_playing
        bne @pl
        lda #.loword(str_m_stop)
        bra @st
@pl:    lda #.loword(str_m_play)
@st:    sta P0
        ldx S2
        lda v_wintab+WX,x
        inc a
        sta A0
        lda v_wintab+WY,x
        clc
        adc #6
        sta A1
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        lda #.loword(str_m_hint)
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

; app_key - A = proc, A1 = event data: dispatch keys to the topmost app
.proc app_key
.a16
.i16
        cmp #2
        beq @np
        cmp #3
        beq @mus
        cmp #4
        beq @dt
        cmp #5
        beq @ol
        cmp #6
        beq @pm
        cmp #7
        beq @files
        cmp #8
        beq @theme
        cmp #9
        beq @trk
        cmp #10
        beq @pnt
        rts
@np:    jsr notepad_key
        rts
@mus:   jsr music_key
        rts
@dt:    jsr dostris_key
        rts
@ol:    jsr outlast_key
        rts
@pm:    jsr pacman_key
        rts
@files: jsr files_key
        rts
@theme: jsr theme_key
        rts
@trk:   jsr tracker_key
        rts
@pnt:   jsr paint_key
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

str_t_music:   .byte "Music", 0
name_music:    .byte "Music", 0
str_m_title:   .byte "Canon in D", 0
str_m_play:    .byte "Playing  (space)", 0
str_m_stop:    .byte "Stopped  (space)", 0
str_m_hint:    .byte "Voice 0 / SPC700", 0

; Canon in D melody: pairs of (note_pitch index = MIDI-36, duration frames).
; $FFFF = rest. The famous descending line + an answering phrase, looped.
MUS_COUNT = 16
mus_song:
        .word 42,28,  40,28,  38,28,  37,28,  35,28,  33,28,  35,28,  37,28
        .word 38,28,  37,28,  35,28,  33,28,  31,28,  30,28,  31,28,  28,40
