; ============================================================================
; UnoDOS / NES — Tracker (app 6). A pattern grid (NT_ROWS rows x NT_CH channel
; columns of note cells) that auto-loops, playing the leftmost non-empty note of
; the current playhead row on the 2A03 APU pulse 1. The minimal-profile analogue
; of the SMS/C64 tracker: while resident it owns the d-pad — the cursor (an
; inverted cell) moves over the grid, A cycles the note (-- C4 D4 E4 F4 G4 A4 B4
; C5), B exits. A ">" playhead marker steps down the rows every TK_STEPF frames.
;
; Rendering follows the kernel's model: cursor/note edits are user-paced and do a
; rendering-off full redraw (v_dirty); the frequent playhead step is a SMALL
; vblank partial (rewrite the one marker column) flagged by pf_tk.
; ============================================================================

NT_ROWS  EQU 14
NT_CH    EQU 4
TK_STEPF EQU 12         ; frames per playhead step
TK_COL   EQU 5          ; grid origin column (note cells)
TK_ROW   EQU 4          ; grid origin row
TK_MCOL  EQU 3          ; playhead marker column

tk_crow  = $60          ; cursor row
tk_cch   = $61          ; cursor channel
tk_prow  = $62          ; playhead row
tk_ptmr  = $63          ; frames left on the current playhead step
tk_r     = $64          ; draw scratch (row)
tk_c     = $65          ; draw scratch (channel)
pf_tk    = $66          ; partial: playhead marker moved

tk_pat   = $0200        ; NT_ROWS*NT_CH note cells (0 = empty, 1..8 = note)

; APU pulse period = round(1789773 / (16*freq)) - 1, per the mkdata formula.
tk_periods:
        dc.w 0, $01AA, $017B, $0152, $0140, $011C, $00FD, $00E1, $00D5
        ;    --   C4     D4     E4     F4     G4     A4     B4     C5
tk_names:
        dc.b "--",0,"C4",0,"D4",0,"E4",0,"F4",0,"G4",0,"A4",0,"B4",0,"C5",0
s_tkmark: dc.b ">",0
s_tk_hint: dc.b "d-pad move  A cycle",0

; ---- tk_tone: A = note (0 = rest) -> program APU pulse 1 -------------------
tk_tone:
        cmp #0
        bne tkt_play
        lda #$B0                ; duty 50%, length halt, constant volume 0 = silent
        sta $4000
        rts
tkt_play:
        asl                     ; note*2 (word index)
        tay
        lda #$BF                ; duty 50%, length halt, constant volume 15
        sta $4000
        lda #$00
        sta $4001
        lda tk_periods,y
        sta $4002               ; period low
        lda tk_periods+1,y
        and #$07
        ora #$08                ; period high 3 bits + length-counter load
        sta $4003
        rts

; ---- tk_play_row: A = row -> play its leftmost non-empty note (or rest) -----
tk_play_row:
        asl
        asl                     ; row*NT_CH
        tay
        ldx #NT_CH
tpr_l:
        lda tk_pat,y
        bne tpr_hit
        iny
        dex
        bne tpr_l
        lda #0                  ; whole row empty -> rest
        jmp tk_tone
tpr_hit:
        jmp tk_tone             ; A = note

; ---- tracker_init ----------------------------------------------------------
tracker_init:
        ldx #0
        lda #0
ti_clr: sta tk_pat,x
        inx
        cpx #NT_ROWS*NT_CH
        bne ti_clr
        ; seed a C-major scale down channel 0, rows 0..7
        ldx #0
ti_seed:
        txa
        asl
        asl                     ; row*NT_CH
        tay
        txa
        clc
        adc #1                  ; note = row+1
        sta tk_pat,y
        inx
        cpx #8
        bne ti_seed
        lda #0
        sta tk_crow
        sta tk_cch
        sta tk_prow
        lda #TK_STEPF
        sta tk_ptmr
        lda #$01
        sta APUSTATUS           ; enable pulse 1
        lda #1
        sta v_dirty
        rts

; ---- tk_cell_idx: -> Y = tk_crow*NT_CH + tk_cch ----------------------------
tk_cell_idx:
        lda tk_crow
        asl
        asl
        clc
        adc tk_cch
        tay
        rts

; ---- input -----------------------------------------------------------------
tracker_update:
        lda v_pade
        and #PAD_U
        beq tu_d
        jsr tk_up
tu_d:   lda v_pade
        and #PAD_D
        beq tu_l
        jsr tk_down
tu_l:   lda v_pade
        and #PAD_L
        beq tu_r
        jsr tk_left
tu_r:   lda v_pade
        and #PAD_R
        beq tu_a
        jsr tk_right
tu_a:   lda v_pade
        and #PAD_A
        beq tu_step
        jsr tk_cycle
tu_step:
  IF AUTOTEST
        lda a_gpause
        bne tu_done             ; script done: freeze the playhead for a stable shot
  ENDIF
        dec tk_ptmr
        bne tu_done
        lda #TK_STEPF
        sta tk_ptmr
        inc tk_prow
        lda tk_prow
        cmp #NT_ROWS
        bcc tu_play
        lda #0
        sta tk_prow
tu_play:
        lda tk_prow
        jsr tk_play_row
        lda #1
        sta pf_tk
tu_done:
        rts

; ---- rp_tracker: vblank partial — move the playhead marker ----------------
rp_tracker:
        lda pf_tk
        bne rpt_go
        rts
rpt_go: jsr tk_draw_marker
        lda #0
        sta pf_tk
        rts

tk_up:
        lda tk_crow
        beq tk_ret
        dec tk_crow
        jmp tk_dirty
tk_down:
        lda tk_crow
        cmp #NT_ROWS-1
        bcs tk_ret
        inc tk_crow
        jmp tk_dirty
tk_left:
        lda tk_cch
        beq tk_ret
        dec tk_cch
        jmp tk_dirty
tk_right:
        lda tk_cch
        cmp #NT_CH-1
        bcs tk_ret
        inc tk_cch
        jmp tk_dirty
tk_cycle:
        jsr tk_cell_idx
        lda tk_pat,y
        clc
        adc #1
        cmp #9
        bcc tkc_s
        lda #0
tkc_s:  sta tk_pat,y
        jmp tk_dirty
tk_ret: rts
tk_dirty:
        lda #1
        sta v_dirty
        rts

; ---- tk_draw_marker: rewrite the marker column (erase all, draw '>') -------
tk_draw_marker:
        ldx #0
tkm_er: stx tk_r
        lda #TK_MCOL
        sta col
        txa
        clc
        adc #TK_ROW
        sta row
        jsr set_nt_addr
        lda #0
        sta PPUDATA
        ldx tk_r
        inx
        cpx #NT_ROWS
        bne tkm_er
        lda #<s_tkmark
        sta ptr
        lda #>s_tkmark
        sta ptr+1
        lda #TK_MCOL
        sta col
        lda tk_prow
        clc
        adc #TK_ROW
        sta row
        lda #T_FONT
        sta tmp
        jmp puts

; ---- app_tracker: chrome + the grid (rendering off) ------------------------
app_tracker:
        lda #<l_tracker
        sta ptr
        lda #>l_tracker
        sta ptr+1
        jsr draw_chrome
        jmp tracker_draw

; ---- tracker_draw: full grid (rendering off) -------------------------------
tracker_draw:
        ; hint line
        lda #<s_tk_hint
        sta ptr
        lda #>s_tk_hint
        sta ptr+1
        lda #2
        sta col
        sta row
        jsr cputs
        lda #0
        sta tk_r
tdg_row:
        lda tk_r
        cmp #NT_ROWS
        bcc tdg_in
        jmp tdg_done
tdg_in:
        lda #0
        sta tk_c
tdg_ch:
        lda tk_c
        cmp #NT_CH
        bcs tdg_rownext
        ; note = tk_pat[tk_r*NT_CH + tk_c]
        lda tk_r
        asl
        asl
        clc
        adc tk_c
        tay
        lda tk_pat,y
        ; ptr = tk_names + note*3
        sta tk_n_tmp
        asl
        clc
        adc tk_n_tmp            ; note*3
        clc
        adc #<tk_names
        sta ptr
        lda #>tk_names
        adc #0
        sta ptr+1
        ; col = TK_COL + tk_c*3 ; row = TK_ROW + tk_r
        lda tk_c
        asl
        clc
        adc tk_c
        clc
        adc #TK_COL
        sta col
        lda tk_r
        clc
        adc #TK_ROW
        sta row
        ; inverted if this is the cursor cell
        lda #T_FONT
        sta tmp
        lda tk_r
        cmp tk_crow
        bne tdg_nf
        lda tk_c
        cmp tk_cch
        bne tdg_nf
        lda #T_FONTINV
        sta tmp
tdg_nf:
        jsr puts
        inc tk_c
        jmp tdg_ch
tdg_rownext:
        inc tk_r
        jmp tdg_row
tdg_done:
        jmp tk_draw_marker

tk_n_tmp = $67
