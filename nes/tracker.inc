; ============================================================================
; UnoDOS / NES — Tracker (app 6). A 32-step x 4-channel pattern sequencer
; (the C64 tracker's data model, widened to 32 steps / 4 channels) rendered
; entirely in background tiles: steps run ACROSS the screen (32 columns = the
; full nametable width), the 4 channels are 4 rows. A cell is empty ('.') or a
; note 1..8 (C4..C5, shown as its letter). The cursor cell is inverted.
;
; Audio is the 2A03's pulse 1 with VOICE-STEALING: when playback reaches a
; step, the first non-empty channel (top-down) wins the voice — the minimal
; profile's one-voice answer to the C64's three SID voices.
;
; Controls: d-pad moves the cursor, A cycles the cell (empty -> C..C5 -> empty),
; SELECT toggles playback, B exits (kernel-global). All updates are small
; partial nametable writes (2-3 cells) that fit in vblank.
; ============================================================================

TK_STEPS  = 32          ; pattern length (columns)
TK_CHANS  = 4           ; channels (rows)
TK_RATE   = 10          ; frames per step (~6 steps/s)
TKROW0    = 4           ; screen row of channel 0
TKMROW    = 3           ; play-marker row (above the grid)
TKPAT     = $0480       ; 128 bytes: step*4 + chan, 0 empty / 1..8 note

; ---- zero page ($60-$6B) ---------------------------------------------------
tk_cx     = $60         ; cursor step 0..31
tk_cy     = $61         ; cursor channel 0..3
tk_ox     = $62         ; previous cursor (for the partial erase)
tk_oy     = $63
tk_play   = $64         ; non-zero while playing
tk_prow   = $65         ; playing step
tk_pold   = $66         ; previous playing step (marker erase)
tk_ctr    = $67         ; frames left on the current step
f_tkcur   = $68         ; partial: cursor moved (redraw old + new cells)
f_tkcell  = $69         ; partial: cursor cell edited
f_tkmark  = $6A         ; partial: play marker moved
f_tkstat  = $6B         ; partial: status text changed

; ---- init ------------------------------------------------------------------
tracker_init:
        ; copy the demo pattern into RAM
        ldx #0
tki_cp: lda tk_demo,x
        sta TKPAT,x
        inx
        cpx #TK_STEPS*TK_CHANS
        bne tki_cp
        lda #0
        sta tk_cx
        sta tk_cy
        sta tk_ox
        sta tk_oy
        sta tk_play
        sta tk_prow
        sta tk_pold
        sta f_tkcur
        sta f_tkcell
        sta f_tkmark
        sta f_tkstat
        lda #TK_RATE
        sta tk_ctr
        lda #$01
        sta APUSTATUS           ; enable pulse 1
        jsr tk_voice_off
        rts

; ---- per-frame logic (no PPU writes) ---------------------------------------
tracker_update:
        ; -- cursor --
        lda v_pade
        and #PAD_L
        beq tku_r
        jsr tk_save_cur
        dec tk_cx
        bpl tku_mv
        lda #TK_STEPS-1
        sta tk_cx
        jmp tku_mv
tku_r:  lda v_pade
        and #PAD_R
        beq tku_u
        jsr tk_save_cur
        inc tk_cx
        lda tk_cx
        cmp #TK_STEPS
        bcc tku_mv
        lda #0
        sta tk_cx
        jmp tku_mv
tku_u:  lda v_pade
        and #PAD_U
        beq tku_d
        jsr tk_save_cur
        dec tk_cy
        bpl tku_mv
        lda #TK_CHANS-1
        sta tk_cy
        jmp tku_mv
tku_d:  lda v_pade
        and #PAD_D
        beq tku_a
        jsr tk_save_cur
        inc tk_cy
        lda tk_cy
        cmp #TK_CHANS
        bcc tku_mv
        lda #0
        sta tk_cy
tku_mv: lda #1
        sta f_tkcur
        jmp tku_pb
        ; -- edit: A cycles the cell 0 -> 1..8 -> 0 --
tku_a:  lda v_pade
        and #PAD_A
        beq tku_sel
        jsr tk_cur_idx          ; X = cx*4 + cy
        lda TKPAT,x
        clc
        adc #1
        cmp #9
        bcc tku_as
        lda #0
tku_as: sta TKPAT,x
        lda #1
        sta f_tkcell
        ; -- SELECT toggles playback --
tku_sel:
        lda v_pade
        and #PAD_SEL
        beq tku_pb
        lda tk_play
        eor #1
        sta tk_play
        bne tku_son
        jsr tk_voice_off        ; stopped: silence
        jmp tku_sfl
tku_son:
        lda tk_prow
        sta tk_pold
        lda #0
        sta tk_prow             ; restart from step 0
        lda #TK_RATE
        sta tk_ctr
        lda #1
        sta f_tkmark
tku_sfl:
        lda #1
        sta f_tkstat
        ; -- playback advance --
tku_pb:
        lda tk_play
        beq tku_done
  IF AUTOTEST
        lda a_gpause
        bne tku_done            ; script done: freeze the marker for the shot
  ENDIF
        dec tk_ctr
        bne tku_done
        lda #TK_RATE
        sta tk_ctr
        jsr tk_play_row
        lda tk_prow
        sta tk_pold
        clc
        adc #1
        and #TK_STEPS-1
        sta tk_prow
        lda #1
        sta f_tkmark
tku_done:
        rts

tk_save_cur:
        lda tk_cx
        sta tk_ox
        lda tk_cy
        sta tk_oy
        rts

; tk_cur_idx: X = tk_cx*4 + tk_cy
tk_cur_idx:
        lda tk_cx
        asl
        asl
        clc
        adc tk_cy
        tax
        rts

; ---- tk_play_row: voice-steal pulse 1 with step tk_prow --------------------
tk_play_row:
        lda tk_prow
        asl
        asl                     ; step*4
        tax
        ldy #0
tpr_scan:
        lda TKPAT,x
        bne tpr_note            ; first non-empty channel wins the voice
        inx
        iny
        cpy #TK_CHANS
        bne tpr_scan
        jmp tk_voice_off        ; all empty -> rest
tpr_note:
        sec
        sbc #1                  ; note-1 -> period index
        asl                     ; *2 (word table)
        tay
        lda #$BF                ; duty 50%, length halt, constant vol 15
        sta $4000
        lda #$00
        sta $4001               ; sweep off
        lda tk_periods,y
        sta $4002
        lda tk_periods+1,y
        and #$07
        ora #$08                ; retrigger (length-counter load)
        sta $4003
        rts

tk_voice_off:
        lda #$B0                ; constant volume 0
        sta $4000
        rts

; ============================================================================
; rendering
; ============================================================================
; ---- app_tracker: full-screen draw (rendering off) -------------------------
app_tracker:
        lda #<t_tracker
        sta ptr
        lda #>t_tracker
        sta ptr+1
        jsr draw_chrome
        lda #<c_tracker
        sta ptr2
        lda #>c_tracker
        sta ptr2+1
        jsr draw_content
        ; the 4 channel rows, streamed 32 cells each
        lda #0
        sta grp                 ; chan
tkd_chan:
        lda #0
        sta col
        lda grp
        clc
        adc #TKROW0
        sta row
        jsr set_nt_addr
        ldx #0                  ; step
tkd_step:
        stx idx
        ; cell index = step*4 + chan
        txa
        asl
        asl
        clc
        adc grp
        tax
        lda TKPAT,x
        tax                     ; X = note value
        ; inverted if this is the cursor cell
        lda idx
        cmp tk_cx
        bne tkd_norm
        lda grp
        cmp tk_cy
        bne tkd_norm
        lda #T_FONTINV
        jmp tkd_put
tkd_norm:
        lda #T_FONT
tkd_put:
        sta tmp
        jsr tk_cell_tile        ; X=note, tmp=font base -> A=tile
        sta PPUDATA
        ldx idx
        inx
        cpx #TK_STEPS
        bne tkd_step
        inc grp
        lda grp
        cmp #TK_CHANS
        bne tkd_chan
        jsr tk_draw_marker
        jmp tk_draw_status

; ---- tk_cell_tile: X = note value (0..8), tmp = font base -> A = tile ------
tk_cell_tile:
        txa
        beq tct_dot
        lda tk_notes-1,x        ; note letter
        jmp tct_map
tct_dot:
        lda #$2E                ; '.'
tct_map:
        sec
        sbc #32
        clc
        adc tmp
        rts

; ---- tk_draw_cell: idx=step, grp=chan -> redraw that one cell --------------
tk_draw_cell:
        lda idx
        sta col
        lda grp
        clc
        adc #TKROW0
        sta row
        jsr set_nt_addr
        lda idx
        asl
        asl
        clc
        adc grp
        tax
        lda TKPAT,x
        tax
        lda idx
        cmp tk_cx
        bne tdc_n
        lda grp
        cmp tk_cy
        bne tdc_n
        lda #T_FONTINV
        jmp tdc_p
tdc_n:  lda #T_FONT
tdc_p:  sta tmp
        jsr tk_cell_tile
        sta PPUDATA
        rts

; ---- tk_draw_marker: erase the old play marker, draw the new ---------------
tk_draw_marker:
        lda tk_pold
        sta col
        lda #TKMROW
        sta row
        jsr set_nt_addr
        lda #0                  ; blank
        sta PPUDATA
        lda tk_prow
        sta col
        jsr set_nt_addr
        lda #T_SOLM             ; magenta marker block
        sta PPUDATA
        rts

; ---- tk_draw_status: "PLAY" / "STOP" at (1,10) -----------------------------
tk_draw_status:
        lda tk_play
        beq tds_stop
        lda #<s_tkplay
        sta ptr
        lda #>s_tkplay
        sta ptr+1
        jmp tds_put
tds_stop:
        lda #<s_tkstop
        sta ptr
        lda #>s_tkstop
        sta ptr+1
tds_put:
        lda #1
        sta col
        lda #10
        sta row
        jmp cputs

; ---- render_partials hook (vblank) -----------------------------------------
rp_tracker:
        lda f_tkcur
        beq rptk_cell
        lda #0
        sta f_tkcur
        ; redraw the old cursor cell (now plain) + the new (inverted)
        lda tk_ox
        sta idx
        lda tk_oy
        sta grp
        jsr tk_draw_cell
        lda tk_cx
        sta idx
        lda tk_cy
        sta grp
        jsr tk_draw_cell
rptk_cell:
        lda f_tkcell
        beq rptk_mark
        lda #0
        sta f_tkcell
        lda tk_cx
        sta idx
        lda tk_cy
        sta grp
        jsr tk_draw_cell
rptk_mark:
        lda f_tkmark
        beq rptk_stat
        lda #0
        sta f_tkmark
        jsr tk_draw_marker
rptk_stat:
        lda f_tkstat
        beq rptk_done
        lda #0
        sta f_tkstat
        jsr tk_draw_status
rptk_done:
        rts

; ---- data ------------------------------------------------------------------
t_tracker: dc.b "Tracker",0
c_tracker:
        dc.b 1, 2
        dc.w m_tk0
        dc.b 1,12
        dc.w m_tk1
        dc.b $FF
m_tk0:  dc.b "32x4 pattern",0
m_tk1:  dc.b "A=note Sel=play",0
s_tkplay: dc.b "PLAY",0
s_tkstop: dc.b "STOP",0
tk_notes: dc.b "CDEFGABC"

; demo pattern: 32 steps x 4 channels (step-major; 0 empty, 1..8 = C4..C5)
; ch0 melody, ch1 bass on the beat, ch2 sparse harmony, ch3 accents
tk_demo:
        dc.b 1,1,0,0,  0,0,0,0,  3,0,5,0,  0,0,0,0
        dc.b 5,1,0,0,  0,0,0,0,  8,0,5,0,  0,0,0,8
        dc.b 6,4,0,0,  0,0,0,0,  5,0,8,0,  0,0,0,0
        dc.b 3,4,0,0,  0,0,0,0,  1,0,3,0,  0,0,0,8
        dc.b 1,1,0,0,  0,0,0,0,  3,0,5,0,  0,0,0,0
        dc.b 5,1,0,0,  0,0,0,0,  8,0,5,0,  0,0,0,8
        dc.b 6,4,0,0,  0,0,0,0,  8,0,8,0,  0,0,0,0
        dc.b 5,1,0,0,  0,0,0,0,  1,0,5,0,  0,0,0,8
