; ============================================================================
; UnoDOS/C64 Tracker - a DISK-LOADED app (app7.bin, icon 7). A 16-row x
; 3-channel pattern sequencer that plays through the SID's THREE voices - where
; the Apple II / x86 Trackers voice only one channel on the 1-bit speaker, the
; C64 sounds all three at once. The other half of the C64 audio story (Music
; plays a fixed tune; Tracker lets you edit one).
;
; Grid: 16 rows, 3 channels (one per SID voice). A cell is a note 0 (empty) or
; 1..8 (C4..C5). CRSR moves the cursor; letter keys C D E F G A B + the up-arrow
; (mapped to high C) enter a note; DEL clears; SPACE plays/stops; RUN/STOP exits.
; Playback advances one row per TK_RATE passes, retriggering each voice.
; ============================================================================

        processor 6502
        include "sys.inc"
        include "build/kernel_api.inc"

TK_ROWS  equ 16
TK_CHANS equ 3
TK_RATE  equ 28         ; mainloop passes per row (harness-tuned tempo)
TKPAT    equ APP_BSS    ; 48 bytes: row*3+chan, value 0 (empty) or note 1..8

zpTkI    equ zpApp0
zpTkCh   equ zpApp1
zpTkTmp  equ zpApp2

        org APP_BASE
        jmp tk_init
        jmp tk_key
        jmp tk_tick

tk_init:
        ; load the demo pattern into TKPAT
        ldx #0
ti_cp:
        lda tk_demo,x
        sta TKPAT,x
        inx
        cpx #(TK_ROWS*TK_CHANS)
        bne ti_cp
        lda #0
        sta tk_row
        sta tk_chan
        sta tk_play
        sta tk_prow
        lda #TK_RATE
        sta tk_ctr
        ; SID: full volume, a short pluck envelope on all three voices
        lda #$0F
        sta SID_BASE+24
        jmp tk_draw

tk_key:
        lda zpTmp
        cmp #K_ESC
        bne tk_k1
        jsr tk_silence
        jmp return_to_desktop
tk_k1:
        cmp #K_SPACE
        bne tk_k2
        ; toggle play
        lda tk_play
        eor #1
        sta tk_play
        bne tk_kplay
        jsr tk_silence
tk_kplay:
        lda #0
        sta tk_prow
        lda #TK_RATE
        sta tk_ctr
        jmp tk_draw
tk_k2:
        cmp #K_UP
        bne tk_k3
        lda tk_row
        bne tk_kdecr
        lda #TK_ROWS
tk_kdecr:
        sec
        sbc #1
        sta tk_row
        jmp tk_draw
tk_k3:
        cmp #K_DOWN
        bne tk_k4
        lda tk_row
        clc
        adc #1
        cmp #TK_ROWS
        bcc tk_ksr
        lda #0
tk_ksr:
        sta tk_row
        jmp tk_draw
tk_k4:
        cmp #K_LEFT
        bne tk_k5
        lda tk_chan
        bne tk_kdecc
        lda #TK_CHANS
tk_kdecc:
        sec
        sbc #1
        sta tk_chan
        jmp tk_draw
tk_k5:
        cmp #K_RIGHT
        bne tk_k6
        lda tk_chan
        clc
        adc #1
        cmp #TK_CHANS
        bcc tk_ksc
        lda #0
tk_ksc:
        sta tk_chan
        jmp tk_draw
tk_k6:
        cmp #K_BS                ; DEL clears the cell
        bne tk_k7
        lda #0
        jmp tk_setcell
tk_k7:
        ; a letter -> a note (C D E F G A B), else ignore
        ldx #0
tk_kl:
        lda zpTmp
        cmp tk_letters,x
        beq tk_klhit
        inx
        cpx #7
        bne tk_kl
        rts
tk_klhit:
        txa
        clc
        adc #1                  ; note 1..7
tk_setcell:
        sta zpTkTmp
        jsr tk_cur_idx          ; X = row*3+chan
        lda zpTkTmp
        sta TKPAT,x
        jmp tk_draw

; tk_tick - advance playback one row per TK_RATE passes.
tk_tick:
        lda tk_play
        beq tt_done
        dec tk_ctr
        bne tt_done
        lda #TK_RATE
        sta tk_ctr
        jsr tk_play_row
        ; advance row (wrap)
        lda tk_prow
        clc
        adc #1
        cmp #TK_ROWS
        bcc tt_sr
        lda #0
tt_sr:
        sta tk_prow
        jmp tk_draw
tt_done:
        rts

; tk_play_row - sound TKPAT row tk_prow: each channel -> its SID voice.
tk_play_row:
        ldx #0                  ; channel / voice
tpr_loop:
        stx zpTkCh
        ; idx = prow*3 + chan
        lda tk_prow
        asl
        clc
        adc tk_prow             ; *3
        clc
        adc zpTkCh
        tax
        lda TKPAT,x
        beq tpr_off             ; empty -> gate off this voice
        tax
        dex                     ; note-1 -> freq index
        ldy zpTkCh
        ; voice base offset = chan*7
        lda tk_vbase,y
        sta zpTkTmp             ; SID voice register base low offset
        ; gate off (retrigger)
        ldy zpTkTmp
        lda #$10
        sta SID_BASE+4,y        ; control = gate off
        lda tk_freq_lo,x
        sta SID_BASE+0,y
        lda tk_freq_hi,x
        sta SID_BASE+1,y
        lda #$09
        sta SID_BASE+5,y        ; attack/decay
        lda #$A8
        sta SID_BASE+6,y        ; sustain/release
        lda #$11
        sta SID_BASE+4,y        ; triangle + gate on
        jmp tpr_next
tpr_off:
        ldy zpTkCh
        lda tk_vbase,y
        tay
        lda #$10
        sta SID_BASE+4,y
tpr_next:
        ldx zpTkCh
        inx
        cpx #TK_CHANS
        bne tpr_loop
        rts

tk_silence:
        lda #$10
        sta SID_BASE+4
        sta SID_BASE+4+7
        sta SID_BASE+4+14
        rts

; tk_cur_idx - X = tk_row*3 + tk_chan.
tk_cur_idx:
        lda tk_row
        asl
        clc
        adc tk_row
        clc
        adc tk_chan
        tax
        rts

; ============================================================================
; drawing
; ============================================================================
tk_draw:
        jsr app_clear
        lda #<msg_tk_title
        sta zpPtr
        lda #>msg_tk_title
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
        ; channel headers
        lda #<msg_tk_hdr
        sta zpPtr
        lda #>msg_tk_hdr
        sta zpPtr+1
        lda #4
        sta zpCol
        lda #1
        sta zpRow
        lda #COL_WIN
        sta zpFCol
        jsr draw_string
        ; rows
        lda #0
        sta zpTkI               ; row
tkd_row:
        ; row number at col 1
        lda zpTkI
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        lda #1
        sta zpCol
        lda zpTkI
        clc
        adc #2
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        jsr draw_dec16
        ; 3 channel cells at cols 5, 11, 17
        lda #0
        sta zpTkCh
tkd_chan:
        ; idx = row*3 + chan
        lda zpTkI
        asl
        clc
        adc zpTkI
        clc
        adc zpTkCh
        tax
        lda TKPAT,x
        sta zpTkTmp             ; note value
        ; col = 5 + chan*6
        lda zpTkCh
        asl
        sta zpApp3
        asl
        clc
        adc zpApp3              ; chan*6
        clc
        adc #5
        sta zpCol
        lda zpTkI
        clc
        adc #2
        sta zpRow
        ; highlight if cursor cell
        lda #0
        sta zpInv
        lda zpTkI
        cmp tk_row
        bne tkd_nohi
        lda zpTkCh
        cmp tk_chan
        bne tkd_nohi
        lda #$FF
        sta zpInv
        ; highlight band (2 cells) - save loop vars (fill_rows clobbers X)
        lda zpCol
        sta zpFX
        lda zpRow
        asl
        asl
        asl
        sta zpFY
        lda #2
        sta zpFW
        lda #8
        sta zpFH
        lda #$FF
        sta zpFPat
        jsr fill_rows
tkd_nohi:
        ; draw the note name (2 chars) or ".."
        lda zpTkTmp
        beq tkd_empty
        ; note letter + octave digit
        tax
        dex
        lda tk_notename,x
        sta zpApp4
        lda #$34                ; '4' (single octave for simplicity)
        sta zpApp5
        jmp tkd_putcell
tkd_empty:
        lda #$2E                ; '.'
        sta zpApp4
        sta zpApp5
tkd_putcell:
        lda zpApp4
        jsr draw_char
        inc zpCol
        lda zpApp5
        jsr draw_char
        ; next chan
        inc zpTkCh
        lda zpTkCh
        cmp #TK_CHANS
        beq tkd_rowdone
        jmp tkd_chan
tkd_rowdone:
        inc zpTkI
        lda zpTkI
        cmp #TK_ROWS
        beq tkd_alldone
        jmp tkd_row
tkd_alldone:
        ; play-row marker / help
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda tk_play
        beq tkd_help
        lda #<msg_tk_playing
        sta zpPtr
        lda #>msg_tk_playing
        sta zpPtr+1
        jmp draw_string
tkd_help:
        lda #<msg_tk_help
        sta zpPtr
        lda #>msg_tk_help
        sta zpPtr+1
        jmp draw_string

; ============================================================================
; data
; ============================================================================
; SID voice register base offsets (voice 1/2/3 = +0/+7/+14)
tk_vbase:   dc.b 0,7,14
; note freq (C4 D4 E4 F4 G4 A4 B4 C5), same table as Music
tk_freq_lo: dc.b $67,$89,$ED,$3B,$13,$44,$DA,$CE
tk_freq_hi: dc.b $11,$13,$15,$17,$1A,$1D,$20,$22
tk_notename: dc.b "CDEFGABC"
tk_letters:  dc.b $43,$44,$45,$46,$47,$41,$42   ; C D E F G A B (uppercase ASCII)

; demo pattern (16 rows x 3 channels) - a little 3-voice riff
tk_demo:
        dc.b 1,5,0,  0,0,0,  3,0,8,  0,0,0
        dc.b 5,1,0,  0,0,0,  6,0,3,  0,0,0
        dc.b 1,5,0,  0,0,0,  3,0,8,  0,0,0
        dc.b 5,1,0,  0,0,0,  8,0,5,  0,0,0
        dc.b 1,5,0,  0,0,0,  3,0,8,  0,0,0
        dc.b 5,1,0,  0,0,0,  6,0,3,  0,0,0
        dc.b 4,6,0,  0,0,0,  1,0,5,  0,0,0
        dc.b 5,1,0,  0,0,0,  8,0,3,  0,0,0

msg_tk_title:   dc.b "Tracker",0
msg_tk_hdr:     dc.b "Ch1   Ch2   Ch3",0
msg_tk_playing: dc.b "PLAYING - 3 SID voices   SPACE=stop",0
msg_tk_help:    dc.b "CRSR move  CDEFGAB=note  SPACE=play",0

; ---- state ----
tk_row:   dc.b 0
tk_chan:  dc.b 0
tk_play:  dc.b 0
tk_prow:  dc.b 0       ; currently-playing row
tk_ctr:   dc.b TK_RATE
