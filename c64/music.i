; ============================================================================
; UnoDOS/C64 Music app (milestone 3) - plays a melody on the SID. This is the
; C64's standout app: where the Apple II port has to busy-loop the 1-bit
; speaker (freezing the desktop), the SID is a real synthesiser - we set a
; voice's frequency + gate and it sounds on its own while the OS keeps running.
;
; Plays "Ode to Joy" (phrase 1) on SID voice 1 (triangle). Notes advance on a
; music_tick (game_tick, app_mode=5); a 5-line staff shows the whole phrase as
; pitch-positioned blocks with the currently-sounding note highlighted.
;
;   SPACE  replay      RUN/STOP  back to desktop (silences the voice)
;
; The harness counts SID register writes, so a test can assert the tune played.
; ============================================================================

MUS_DUR equ 16          ; mainloop passes per note (harness-tuned tempo)

; note frequency table (SID voice registers, PAL): Freg = round(Hz * 17.0278)
; index 0..7 = C4 D4 E4 F4 G4 A4 B4 C5
music_freq_lo: dc.b $67,$89,$ED,$3B,$13,$44,$DA,$CE
music_freq_hi: dc.b $11,$13,$15,$17,$1A,$1D,$20,$22
; staff row per note index (higher pitch -> higher on screen / smaller row)
music_staff_y: dc.b 14,13,12,11,10,9,8,7

; the tune: note indices, $FF terminates.  E E F G G F E D C C D E E D D
music_tune:
        dc.b 2,2,3,4,4,3,2,1,0,0,1,2,2,1,1,$FF
MUS_LEN equ 15

music_open:
        lda #5
        sta app_mode
        jsr sid_click
        jsr music_start
        jmp music_draw

music_close:
        jsr music_silence
        lda #0
        sta app_mode
        jsr draw_desktop
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jmp draw_icons

music_key:
        lda zpTmp
        cmp #K_ESC
        bne mk_n1
        jmp music_close
mk_n1:
        cmp #K_SPACE
        bne mk_done
        jsr music_start
        jmp music_draw
mk_done:
        rts

; music_start - reset to note 0, init the SID voice, sound the first note.
music_start:
        lda #0
        sta mus_idx
        sta mus_ctr
        lda #1
        sta mus_playing
        ; SID voice 1 envelope + volume
        lda #$09
        sta SID_BASE+5          ; attack/decay
        lda #$F4
        sta SID_BASE+6          ; sustain/release
        lda #$0F
        sta SID_BASE+24         ; volume
        jsr music_sound_cur
        rts

; music_sound_cur - sound the note at mus_idx (gate retrigger), or silence at
; the terminator (and clear mus_playing).
music_sound_cur:
        ldx mus_idx
        lda music_tune,x
        cmp #$FF
        beq msc_end
        tax
        lda #$10
        sta SID_BASE+4          ; gate off (retrigger the envelope)
        lda music_freq_lo,x
        sta SID_BASE+0
        lda music_freq_hi,x
        sta SID_BASE+1
        lda #$11
        sta SID_BASE+4          ; triangle + gate on
        rts
msc_end:
        lda #0
        sta mus_playing
        ; fall through to silence
music_silence:
        lda #$10
        sta SID_BASE+4          ; gate off
        rts

; music_tick - advance the tune (called every mainloop pass while app_mode=5).
music_tick:
        lda mus_playing
        beq mt_done
        inc mus_ctr
        lda mus_ctr
        cmp #MUS_DUR
        bcc mt_done
        lda #0
        sta mus_ctr
        inc mus_idx
        jsr music_sound_cur
        jsr music_draw_notes    ; refresh the highlight
mt_done:
        rts

music_open_redraw:
music_draw:
        jsr app_clear
        lda #<msg_mus_title
        sta zpPtr
        lda #>msg_mus_title
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        lda #0
        sta zpFX
        lda #7
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #1
        sta zpFH
        lda #$FF
        sta zpFPat
        jsr fill_rows
        ; 5 staff lines (rows 8,10,12,14,16 -> pixel rows *8 + mid-cell). Use a
        ; ZP counter, not X - fill_rows clobbers X.
        lda #0
        sta zpSlot
mdl_loop:
        lda zpSlot
        asl                     ; *2
        clc
        adc #8                  ; row 8,10,12,...
        asl
        asl
        asl                     ; pixel row
        clc
        adc #3                  ; mid-cell
        sta zpFY
        lda #2
        sta zpFX
        lda #36
        sta zpFW
        lda #1
        sta zpFH
        lda #$FF
        sta zpFPat
        jsr fill_rows
        inc zpSlot
        lda zpSlot
        cmp #5
        bne mdl_loop
        jsr music_draw_notes
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<msg_mus_help
        sta zpPtr
        lda #>msg_mus_help
        sta zpPtr+1
        jmp draw_string

; music_draw_notes - plot each tune note as a block on the staff (col 3 + i*2),
; the current note highlighted (red), the rest blue.
music_draw_notes:
        ldx #0
mdn_loop:
        cpx #MUS_LEN
        beq mdn_done
        stx zpSlot
        lda music_tune,x
        tay                     ; note index
        ; cell col = 3 + i*2, cell row = music_staff_y[note]
        txa
        asl
        clc
        adc #3
        sta zpCX
        sta zpFX
        lda music_staff_y,y
        sta zpCY
        ; colour: red if current note, else blue
        ldx zpSlot
        cpx mus_idx
        bne mdn_blue
        lda #$20                ; red fg (2<<4)
        jmp mdn_col
mdn_blue:
        lda #$60                ; blue fg (6<<4)
mdn_col:
        sta zpFCol
        lda #1
        sta zpCW
        sta zpCH
        jsr color_fill
        ; bitmap block
        lda zpFX
        sta zpFX
        lda zpCY
        asl
        asl
        asl
        sta zpFY
        lda #1
        sta zpFW
        lda #8
        sta zpFH
        lda #$FF
        sta zpFPat
        jsr fill_rows
        ldx zpSlot
        inx
        jmp mdn_loop
mdn_done:
        rts

msg_mus_title: dc.b "Music",0
msg_mus_help:  dc.b "SID voice 1   SPACE replay  STOP back",0
