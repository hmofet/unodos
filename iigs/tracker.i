; ============================================================================
; UnoDOS/Apple IIGS - Tracker (proc 8): a 4-voice pattern sequencer on the
; Ensoniq DOC.  16 steps x 4 channels; each cell is a note 0..12 (0 = rest).
; Editing: arrows move the cursor (left/right = step, up/down = channel),
; keys 1-9 + 0 set a note (1..10), space clears, P toggles playback.  On play
; (tracker_tick), each step plays its 4 channels' notes on DOC oscillators
; 0..3 - the IIGS finally gives UnoDOS real polyphony.  Reuses snd_note/snd_off.
; ============================================================================

TRKPAT = $9F00                ; 16*4 = 64 bytes (bank 0)
TRK_STEPS = 16
TRK_CHANS = 4
TRK_FRAMES = 12               ; frames per step

v_trk_step  = VARS+$458       ; cursor step 0..15
v_trk_chan  = VARS+$45A       ; cursor channel 0..3
v_trk_play  = VARS+$45C
v_trk_pos   = VARS+$45E       ; playback step
v_trk_timer = VARS+$460

; tracker_start: clear the pattern + cursor.
.a16
.i16
tracker_start:
        ldx #0
        sep #$20
        lda #0
@cl:    sta TRKPAT,x
        inx
        cpx #(TRK_STEPS*TRK_CHANS)
        bcc @cl
        rep #$20
        stz v_trk_step
        stz v_trk_chan
        stz v_trk_play
        stz v_trk_pos
        ; a little default riff on channel 0
        lda #1
        sta TRKPAT+0
        lda #3
        sta TRKPAT+(4*4)
        lda #5
        sta TRKPAT+(8*4)
        lda #8
        sta TRKPAT+(12*4)
        rts

; tracker_play_step: A = step -> play its 4 channels on osc 0..3.
.a16
.i16
tracker_play_step:
        asl a
        asl a                  ; step*4
        sta S5                 ; pattern base
        stz S6                 ; channel 0..3
@ch:    lda S6
        clc
        adc S5
        tax
        sep #$20
        lda TRKPAT,x
        rep #$20
        and #$00FF
        beq @rest
        ; note -> freq: trk_freqs[note-1]
        dec a
        asl a
        tax
        lda f:trk_freqs,x
        sta A1
        lda S6
        sta A0                 ; oscillator = channel
        jsr snd_note
        bra @next
@rest:  lda S6
        sta A0
        jsr snd_off
@next:  inc S6
        lda S6
        cmp #TRK_CHANS
        bcc @ch
        rts

; tracker_tick: advance playback while a Tracker window is open + playing.
.a16
.i16
tracker_tick:
        stz S5
@scan:  lda S5
        jsr ent_x
        sep #$20
        lda v_wintab+WSTATE,x
        beq @nf
        lda v_wintab+WPROC,x
        cmp #8
        bne @nf
        rep #$20
        bra @found
@nf:    rep #$20
        inc S5
        lda S5
        cmp #MAXWIN
        bcc @scan
        rts                    ; no Tracker window
@found: lda v_trk_play
        bne @go
        rts
@go:    dec v_trk_timer
        beq @adv
        rts
@adv:   lda v_trk_pos
        inc a
        cmp #TRK_STEPS
        bcc @ok
        lda #0
@ok:    sta v_trk_pos
        jsr tracker_play_step
        lda #TRK_FRAMES
        sta v_trk_timer
        jsr redraw_topmost
        rts

; tracker_key: S0 = ascii.
.a16
.i16
tracker_key:
        lda S0
        cmp #$08
        bne :+
        jmp @left
:       cmp #$15
        bne :+
        jmp @right
:       cmp #$0B
        bne :+
        jmp @up
:       cmp #$0A
        bne :+
        jmp @down
:       cmp #' '
        bne :+
        jmp @clear
:       cmp #'p'
        beq :+
        cmp #'P'
        bne :++
:       jmp @play
:       cmp #'0'
        bne :+
        jmp @note0
:       cmp #'1'
        bcc @out
        cmp #'9'+1
        bcs @out
        ; 1..9 -> note 1..9
        sec
        sbc #'0'
        bra @setnote
@note0: lda #10                ; '0' -> note 10
@setnote:
        sta S1                 ; note value
        jsr trk_curidx
        tax
        sep #$20
        lda S1
        sta TRKPAT,x
        rep #$20
        jsr redraw_topmost
@out:   rts
@clear: jsr trk_curidx
        tax
        sep #$20
        lda #0
        sta TRKPAT,x
        rep #$20
        jsr redraw_topmost
        rts
@left:  lda v_trk_step
        bne @ldec
        lda #TRK_STEPS
@ldec:  dec a
        sta v_trk_step
        jsr redraw_topmost
        rts
@right: lda v_trk_step
        inc a
        cmp #TRK_STEPS
        bcc @rset
        lda #0
@rset:  sta v_trk_step
        jsr redraw_topmost
        rts
@up:    lda v_trk_chan
        bne @udec
        lda #TRK_CHANS
@udec:  dec a
        sta v_trk_chan
        jsr redraw_topmost
        rts
@down:  lda v_trk_chan
        inc a
        cmp #TRK_CHANS
        bcc @dset
        lda #0
@dset:  sta v_trk_chan
        jsr redraw_topmost
        rts
@play:  lda v_trk_play
        eor #1
        sta v_trk_play
        beq @stopall
        lda #1
        sta v_trk_timer
        rts
@stopall:
        ; silence all 4 voices
        stz A0
        jsr snd_off
        lda #1
        sta A0
        jsr snd_off
        lda #2
        sta A0
        jsr snd_off
        lda #3
        sta A0
        jsr snd_off
        rts

; trk_curidx: -> A = cursor pattern index (step*4 + chan).
.a16
.i16
trk_curidx:
        lda v_trk_step
        asl a
        asl a
        clc
        adc v_trk_chan
        rts

; tracker_draw: S2 = window offset.
.a16
.i16
tracker_draw:
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc #1
        sta A1
        lda #.loword(str_trk_hdr)
        sta P0
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta S3                 ; left col
        lda v_wintab+WY,x
        clc
        adc #3
        sta S4                 ; first step row
        stz LC0                ; step
@row:   lda LC0
        cmp #TRK_STEPS
        bcc :+
        jmp @done
:       ; step number
        lda LC0
        sta S0
        jsr fmt_dec
        lda #.loword(v_numbuf)
        sta P0
        lda S3
        sta A0
        lda S4
        clc
        adc LC0
        sta A1
        lda #ATTR_NORM
        ldx LC0
        cpx v_trk_pos
        bne @nopos
        lda #ATTR_ACC          ; play row highlight
@nopos: sta A4
        jsr draw_str
        ; 4 channel notes
        stz LC1                ; channel
@chan:  lda LC1
        cmp #TRK_CHANS
        bcs @nextstep
        ; idx = step*4 + chan
        lda LC0
        asl a
        asl a
        clc
        adc LC1
        tax
        sep #$20
        lda TRKPAT,x
        rep #$20
        and #$00FF
        sta S0                 ; note
        jsr fmt_dec            ; v_numbuf = note number
        lda #.loword(v_numbuf)
        sta P0
        lda S3
        clc
        adc #3
        sta S1                 ; channel column base
        lda LC1
        asl a
        asl a                  ; chan*4
        clc
        adc S1
        sta A0
        lda S4
        clc
        adc LC0
        sta A1
        ; attr: cursor cell INV, play row ACC, else NORM
        lda #ATTR_NORM
        ldx LC0
        cpx v_trk_step
        bne @notcur
        ldx LC1
        cpx v_trk_chan
        bne @notcur
        lda #ATTR_INV
        bra @setattr
@notcur:
        ldx LC0
        cpx v_trk_pos
        bne @setattr
        lda #ATTR_ACC
@setattr:
        sta A4
        jsr draw_str
        inc LC1
        bra @chan
@nextstep:
        inc LC0
        jmp @row
@done:  rts

; one-octave DOC frequency words (note 1..12)
trk_freqs:
        .word $0200,$021E,$023E,$0260,$0284,$02AA,$02D2,$02FE
        .word $032C,$035D,$0391,$03C8
str_trk_hdr: .byte "Tracker  P=play 1-0=note", 0
