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

SNDCTL  = $00C03C
SNDDATA = $00C03D
SNDADRL = $00C03E
SNDADRH = $00C03F

; DOC per-oscillator register bases
DOC_FREQL = $00
DOC_FREQH = $20
DOC_VOL   = $40
DOC_WTPTR = $80
DOC_CTRL  = $A0
DOC_WTSZ  = $C0
DOC_OSCEN = $E1

; ---- Music state (VARS) ----
v_mus_play  = VARS+$332
v_mus_idx   = VARS+$334
v_mus_timer = VARS+$336
MELODY_LEN  = 16
NOTE_FRAMES = 16

; doc_write: A2 low = DOC register, A3 low = value.
.a16
.i16
doc_write:
        sep #$20
        lda A2
        sta f:SNDADRL
        lda #0
        sta f:SNDADRH
        lda #$00               ; register access, no autoincrement
        sta f:SNDCTL
        lda A3
        sta f:SNDDATA
        rep #$20
        rts

; doc_load_wave: write a 256-byte sawtooth into DOC RAM at $0000.
.a16
.i16
doc_load_wave:
        sep #$20
        rep #$10
        lda #0
        sta f:SNDADRL
        lda #0
        sta f:SNDADRH
        lda #$60               ; RAM access + autoincrement
        sta f:SNDCTL
        ldx #0
@w:     txa
        ora #$01               ; never a 0 sample (0 halts one-shot modes)
        sta f:SNDDATA
        inx
        cpx #256
        bcc @w
        rep #$30
        rts

; doc_init: halt all 32 oscillators, set the oscillator-enable scan, load wave.
.a16
.i16
doc_init:
        ldx #0
@halt:  txa
        clc
        adc #DOC_CTRL
        sta A2
        lda #$01               ; halt
        sta A3
        phx
        jsr doc_write
        plx
        inx
        cpx #32
        bcc @halt
        lda #DOC_OSCEN
        sta A2
        lda #(8*2)             ; scan ~8 oscillators
        sta A3
        jsr doc_write
        jsr doc_load_wave
        rts

; snd_note: A0 = oscillator, A1 = DOC frequency word. Programs + starts it.
.a16
.i16
snd_note:
        sep #$20
        lda A0                 ; freq low
        sta A2
        lda A1
        sta A3
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_FREQH
        sta A2
        lda A1+1
        sta A3
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_VOL
        sta A2
        lda #$C0
        sta A3
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_WTPTR
        sta A2
        stz A3                 ; wavetable at DOC RAM page 0
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_WTSZ
        sta A2
        stz A3                 ; 256-byte table, 256-byte resolution
        rep #$20
        jsr doc_write
        sep #$20
        lda A0
        clc
        adc #DOC_CTRL
        sta A2
        stz A3                 ; free-run, not halted, channel 0
        rep #$20
        jsr doc_write
        rts

; snd_off: A0 = oscillator -> halt it.
.a16
.i16
snd_off:
        sep #$20
        lda A0
        clc
        adc #DOC_CTRL
        sta A2
        lda #$01
        sta A3
        rep #$20
        jsr doc_write
        rts

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
