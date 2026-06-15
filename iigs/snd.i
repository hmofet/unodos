; ============================================================================
; UnoDOS/Apple IIGS - Ensoniq 5503 DOC sound engine (snd.i) + Music (proc 4).
;
; The DOC is the marquee IIGS audio feature (32 oscillators, 64 KB dedicated
; sound RAM) reached through the sound GLU: $C03E/$C03F = 16-bit DOC address,
; $C03C = control (bit5 0=register/1=RAM, bit6=autoincrement), $C03D = data.
; doc_init halts all oscillators and loads one wavetable into DOC RAM;
; snd_note programs an oscillator (freq, volume, wavetable, control=free-run)
; and snd_off halts it.  Music sequences a melody on oscillator 0 with a
; per-frame tick.
;
; Audio is not screenshot-verifiable (the harness has no DOC emulation, and
; sound never is across these ports), but the harness logs every GLU write,
; so tests assert the engine issues the correct oscillator programming.
; ============================================================================

; The DOC sound engine (doc_init/doc_write/snd_note/snd_off) now lives in the
; kernel (shared infra); Music calls snd_note/snd_off via kernel_api.inc.
; v_mus_* live in sys.inc.
MELODY_LEN  = 16
NOTE_FRAMES = 16

; ============================================================================
; Music (proc 4)
; ============================================================================
.a16
.i16
music_start:
        lda #1
        sta v_mus_play
        stz v_mus_idx
        lda #1
        sta v_mus_timer
        rts

; music_tick: called every frame.  Advances the melody only while a Music
; window exists; stops the oscillator when the window is gone.
.a16
.i16
music_tick:
        ; is a proc-4 window present?
        stz S5                 ; slot scan
        stz S6                 ; found flag
@scan:  lda S5
        jsr ent_x
        sep #$20
        lda v_wintab+WSTATE,x
        beq @nf
        lda v_wintab+WPROC,x
        cmp #4
        bne @nf
        rep #$20
        lda #1
        sta S6
        bra @done
@nf:    rep #$20
        inc S5
        lda S5
        cmp #MAXWIN
        bcc @scan
@done:  lda S6
        bne @playing
        ; no Music window: stop if we were playing
        lda v_mus_play
        beq @out
        stz v_mus_play
        stz A0
        jsr snd_off
        rts
@playing:
        lda v_mus_play
        bne @go
        jsr music_start
@go:    dec v_mus_timer
        beq @adv
        rts
@adv:   lda v_mus_idx
        inc a
        cmp #MELODY_LEN
        bcc @ok
        lda #0
@ok:    sta v_mus_idx
        asl a
        tax
        lda f:melody,x
        beq @rest
        sta A1
        stz A0
        jsr snd_note
        bra @timer
@rest:  stz A0
        jsr snd_off
@timer: lda #NOTE_FRAMES
        sta v_mus_timer
@out:   rts

.a16
.i16
music_draw:
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc #2
        sta A1
        lda #.loword(str_music_hdr)
        sta P0
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        ; current note number (accent)
        lda v_mus_idx
        sta S0
        jsr fmt_dec
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc #4
        sta A1
        lda #.loword(str_music_note)
        sta P0
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #9
        sta A0
        lda v_wintab+WY,x
        clc
        adc #4
        sta A1
        lda #.loword(v_numbuf)
        sta P0
        lda #ATTR_ACC
        sta A4
        jsr draw_str
        ldx S2
        lda v_wintab+WX,x
        clc
        adc #2
        sta A0
        lda v_wintab+WY,x
        clc
        adc #6
        sta A1
        lda #.loword(str_music_chip)
        sta P0
        lda #ATTR_NORM
        sta A4
        jsr draw_str
        rts

; melody: DOC frequency words; 0 = rest.  A simple ascending/​descending run.
melody:
        .word $0200,$0240,$0280,$02C0,$0300,$0340,$0380,$03C0
        .word $0380,$0340,$0300,$02C0,$0280,$0240,$0200,$0000
str_music_hdr:  .byte "Music - Ensoniq DOC", 0
str_music_note: .byte "Note:", 0
str_music_chip: .byte "32-osc wavetable synth", 0
