; ============================================================================
; UnoDOS / NES — Pac-Man (app 9). The 13x13 pillar-maze chase game: maze +
; greedy-Manhattan ghost AI from the shared UnoDOS Pac-Man model (the C64 port
; is the closest 6502 donor; the Genesis port ships the same logic). NES
; rendering is BACKGROUND TILES ONLY: walls / dots / pellets are maze cells,
; and the actors are recoloured cells (T_PAC index3 = yellow, T_GHO index1 =
; white) redrawn as small vblank partials — erase the actor's old cell back to
; its maze tile, draw the new. The app loads its own $3F00 palette (black /
; white / blue walls / yellow) and the launcher restores the desktop's on exit.
;
; The 13x13 maze + a right-hand HUD trims comfortably inside the 32x24-cell
; usable area (and the Mesen-visible top-left region). Ghosts chase greedily
; (minimise Manhattan distance, no reversing) — the documented "simple chase"
; deviation from the arcade scatter/frightened AI. Eat every dot to win; a
; ghost on your tile is game over. D-pad queues a turn, A = new game after
; win/lose, B exits (kernel-global).
; ============================================================================

PM_COLS  = 13
PM_ROWS  = 13
PM_OX    = 1            ; screen col of maze col 0
PM_OY    = 2            ; screen row of maze row 0
PM_RATE  = 15           ; frames per game step (4 steps/s)
PM_HUDX  = 16
PMMAZE   = $0500        ; 169-byte mutable maze (0 floor, 1 wall, 2 dot, 3 power)

; ---- zero page ($7C-$9D) ---------------------------------------------------
pm_ctr    = $7C
pac_col   = $7D
pac_row   = $7E
pac_dir   = $7F         ; 0 up, 1 left, 2 down, 3 right
pac_ndir  = $80         ; queued turn
gh_col    = $81         ; [2]
gh_row    = $83         ; [2]
gh_dir    = $85         ; [2]
pm_dots   = $87         ; dots left (fits 8-bit: max 94)
pm_score  = $88         ; 8-bit score: dot=1, power=5 (max 110 -> 3 digits)
pm_state  = $89         ; 0 play, 1 win, 2 lose
pm_mcol   = $8A         ; maze helpers
pm_mrow   = $8B
pm_ncol   = $8C
pm_nrow   = $8D
pm_gi     = $8E         ; ghost loop index
pm_bestd  = $8F
pm_bestdir = $90
pm_dir    = $91
pm_ta     = $92
pm_tb     = $93
f_pmstep  = $94         ; partial: actors moved (erase olds, draw news)
f_pmhud   = $95         ; partial: score/dots/status changed
pac_ocol  = $96         ; previous actor cells (for the partial erase)
pac_orow  = $97
gh_ocol   = $98         ; [2]
gh_orow   = $9A         ; [2] ($9A,$9B)

; ---- init ------------------------------------------------------------------
pacman_init:
        ; load the maze template + count the collectables
        lda #0
        sta pm_dots
        ldx #0
pmi_lo: lda pm_maze_tpl,x
        sta PMMAZE,x
        cmp #2
        bcc pmi_ln              ; floor/wall
        inc pm_dots
pmi_ln: inx
        cpx #169
        bne pmi_lo
        ; pac at (6,11) heading left, its spawn dot cleared
        lda #6
        sta pac_col
        sta pm_mcol
        lda #11
        sta pac_row
        sta pm_mrow
        lda #1
        sta pac_dir
        sta pac_ndir
        jsr pm_idx
        lda PMMAZE,x
        cmp #2
        bcc pmi_g
        lda #0
        sta PMMAZE,x
        dec pm_dots
pmi_g:
        ; ghosts either side of the house (6,6)
        lda #5
        sta gh_col+0
        lda #7
        sta gh_col+1
        lda #6
        sta gh_row+0
        sta gh_row+1
        lda #3
        sta gh_dir+0
        lda #1
        sta gh_dir+1
        lda #0
        sta pm_score
        sta pm_state
        sta f_pmstep
        sta f_pmhud
        lda #PM_RATE
        sta pm_ctr
        rts

; ---- per-frame logic (no PPU writes) ---------------------------------------
pacman_update:
        lda pm_state
        beq pmu_play
        ; win/lose: A = new game
        lda v_pade
        and #PAD_A
        beq pmu_done
        jsr pacman_init
        lda #1
        sta v_dirty
pmu_done:
        rts
pmu_play:
        ; d-pad queues a turn
        lda v_pade
        and #PAD_U
        beq pmu_l
        lda #0
        sta pac_ndir
pmu_l:  lda v_pade
        and #PAD_L
        beq pmu_d
        lda #1
        sta pac_ndir
pmu_d:  lda v_pade
        and #PAD_D
        beq pmu_r
        lda #2
        sta pac_ndir
pmu_r:  lda v_pade
        and #PAD_R
        beq pmu_step
        lda #3
        sta pac_ndir
pmu_step:
  IF AUTOTEST
        lda a_gpause
        bne pmu_done            ; script done: freeze for a stable shot
  ENDIF
        dec pm_ctr
        bne pmu_done
        lda #PM_RATE
        sta pm_ctr
        jmp pm_step

; ---- pm_idx: pm_mcol/pm_mrow -> X = row*13 + col ----------------------------
pm_idx:
        lda pm_mrow
        asl
        asl
        sta pm_ta               ; r*4
        asl                     ; r*8
        clc
        adc pm_ta               ; r*12
        clc
        adc pm_mrow             ; r*13
        clc
        adc pm_mcol
        tax
        rts

; ---- pm_walk: pm_ncol/pm_nrow -> A=1 open, A=0 wall/out ---------------------
pm_walk:
        lda pm_ncol
        cmp #PM_COLS
        bcs pw_no
        lda pm_nrow
        cmp #PM_ROWS
        bcs pw_no
        asl
        asl
        sta pm_ta
        asl
        clc
        adc pm_ta
        clc
        adc pm_nrow
        clc
        adc pm_ncol
        tax
        lda PMMAZE,x
        cmp #1
        beq pw_no
        lda #1
        rts
pw_no:  lda #0
        rts

; ---- pm_step: one game step ------------------------------------------------
pm_step:
        ; save the old actor cells for the partial erase
        lda pac_col
        sta pac_ocol
        lda pac_row
        sta pac_orow
        lda gh_col+0
        sta gh_ocol+0
        lda gh_col+1
        sta gh_ocol+1
        lda gh_row+0
        sta gh_orow+0
        lda gh_row+1
        sta gh_orow+1
        ; adopt the queued turn if open
        ldx pac_ndir
        lda pac_col
        clc
        adc pm_dx,x
        sta pm_ncol
        lda pac_row
        clc
        adc pm_dy,x
        sta pm_nrow
        jsr pm_walk
        beq ps_keep
        lda pac_ndir
        sta pac_dir
ps_keep:
        ; move pac if open
        ldx pac_dir
        lda pac_col
        clc
        adc pm_dx,x
        sta pm_ncol
        lda pac_row
        clc
        adc pm_dy,x
        sta pm_nrow
        jsr pm_walk
        beq ps_eat
        lda pm_ncol
        sta pac_col
        lda pm_nrow
        sta pac_row
ps_eat:
        ; eat the tile under pac
        lda pac_col
        sta pm_mcol
        lda pac_row
        sta pm_mrow
        jsr pm_idx
        lda PMMAZE,x
        cmp #2
        bcc ps_ghosts           ; nothing to eat
        cmp #3
        beq ps_pwr
        inc pm_score            ; dot = 1 point
        jmp ps_clr
ps_pwr: lda pm_score
        clc
        adc #5                  ; pellet = 5 points
        sta pm_score
ps_clr: lda #0
        sta PMMAZE,x
        dec pm_dots
        lda #1
        sta f_pmhud
ps_ghosts:
        lda #0
        sta pm_gi
psg_loop:
        jsr pm_ghost_steer      ; pick gh_dir[gi] toward pac
        ldx pm_gi
        lda gh_dir,x
        tay
        lda gh_col,x
        clc
        adc pm_dx,y
        sta pm_ncol
        lda gh_row,x
        clc
        adc pm_dy,y
        sta pm_nrow
        jsr pm_walk
        beq psg_next            ; blocked: stay (rare; the chooser avoids it)
        ldx pm_gi
        lda pm_ncol
        sta gh_col,x
        lda pm_nrow
        sta gh_row,x
psg_next:
        inc pm_gi
        lda pm_gi
        cmp #2
        bne psg_loop
        ; collision?
        ldx #0
psc_loop:
        lda gh_col,x
        cmp pac_col
        bne psc_next
        lda gh_row,x
        cmp pac_row
        bne psc_next
        lda #2
        sta pm_state            ; lose
        lda #1
        sta f_pmhud
psc_next:
        inx
        cpx #2
        bne psc_loop
        ; win?
        lda pm_dots
        bne ps_flag
        lda pm_state
        bne ps_flag
        lda #1
        sta pm_state
        sta f_pmhud
ps_flag:
        lda #1
        sta f_pmstep
        rts

; ---- pm_ghost_steer: open non-reverse dir minimising Manhattan dist --------
pm_ghost_steer:
        lda #$FF
        sta pm_bestd
        sta pm_bestdir
        lda #0
        sta pm_dir
pgs_loop:
        ; skip the reverse of the current direction
        ldx pm_gi
        lda gh_dir,x
        clc
        adc #2
        and #3
        cmp pm_dir
        beq pgs_next
        ; candidate tile
        ldx pm_gi
        ldy pm_dir
        lda gh_col,x
        clc
        adc pm_dx,y
        sta pm_ncol
        lda gh_row,x
        clc
        adc pm_dy,y
        sta pm_nrow
        jsr pm_walk
        beq pgs_next
        ; dist = |ncol-pac_col| + |nrow-pac_row|
        lda pm_ncol
        sec
        sbc pac_col
        bpl pgs_dx
        eor #$FF
        clc
        adc #1
pgs_dx: sta pm_tb
        lda pm_nrow
        sec
        sbc pac_row
        bpl pgs_dy
        eor #$FF
        clc
        adc #1
pgs_dy: clc
        adc pm_tb
        cmp pm_bestd
        bcs pgs_next
        sta pm_bestd
        lda pm_dir
        sta pm_bestdir
pgs_next:
        inc pm_dir
        lda pm_dir
        cmp #4
        bne pgs_loop
        lda pm_bestdir
        cmp #$FF
        beq pgs_done
        ldx pm_gi
        sta gh_dir,x
pgs_done:
        rts

; ============================================================================
; rendering
; ============================================================================
; ---- app_pacman: full-screen draw (rendering off) --------------------------
app_pacman:
        ; the app's own palette
        bit PPUSTATUS
        lda #$3F
        sta PPUADDR
        lda #$00
        sta PPUADDR
        ldx #0
apm_pal:
        lda pal_pacman,x
        sta PPUDATA
        inx
        cpx #4
        bne apm_pal
        lda #<t_pacman
        sta ptr
        lda #>t_pacman
        sta ptr+1
        jsr draw_chrome
        ; maze rows, streamed 13 cells each
        lda #0
        sta pm_mrow
apd_row:
        lda #PM_OX
        sta col
        lda pm_mrow
        clc
        adc #PM_OY
        sta row
        jsr set_nt_addr
        lda #0
        sta pm_mcol
        jsr pm_idx              ; X = row*13
apd_cell:
        ldy PMMAZE,x
        lda pm_tiles,y
        sta PPUDATA
        inx
        inc pm_mcol
        lda pm_mcol
        cmp #PM_COLS
        bne apd_cell
        inc pm_mrow
        lda pm_mrow
        cmp #PM_ROWS
        bne apd_row
        ; actors + HUD labels
        jsr pm_draw_actors
        lda #<s_pmscore
        sta ptr
        lda #>s_pmscore
        sta ptr+1
        lda #PM_HUDX
        sta col
        lda #3
        sta row
        jsr cputs
        lda #<s_pmdots
        sta ptr
        lda #>s_pmdots
        sta ptr+1
        lda #PM_HUDX
        sta col
        lda #6
        sta row
        jsr cputs
        jmp pm_draw_hud

; ---- pm_draw_cell_at: pm_mcol/pm_mrow, A = tile ----------------------------
pm_draw_cell_at:
        pha
        lda pm_mcol
        clc
        adc #PM_OX
        sta col
        lda pm_mrow
        clc
        adc #PM_OY
        sta row
        jsr set_nt_addr
        pla
        sta PPUDATA
        rts

; ---- pm_erase_at: redraw the maze tile under (pm_mcol, pm_mrow) ------------
pm_erase_at:
        jsr pm_idx
        ldy PMMAZE,x
        lda pm_tiles,y
        jmp pm_draw_cell_at

; ---- pm_draw_actors: pac + the two ghosts at their current cells -----------
pm_draw_actors:
        lda pac_col
        sta pm_mcol
        lda pac_row
        sta pm_mrow
        lda #T_PAC
        jsr pm_draw_cell_at
        ldx #0
pda_g:  stx pm_ta
        lda gh_col,x
        sta pm_mcol
        lda gh_row,x
        sta pm_mrow
        lda #T_GHO
        jsr pm_draw_cell_at
        ldx pm_ta
        inx
        cpx #2
        bne pda_g
        rts

; ---- pm_draw_hud: score + dots numbers + status ----------------------------
pm_draw_hud:
        ; score (3 digits: repeated-subtract hundreds, then two_digits)
        ldx #$30                ; '0'
        lda pm_score
pdh_h:  cmp #100
        bcc pdh_hd
        sbc #100
        inx
        jmp pdh_h
pdh_hd: stx numstr
        jsr two_digits
        stx numstr+1
        sta numstr+2
        lda #0
        sta numstr+3
        lda #<numstr
        sta ptr
        lda #>numstr
        sta ptr+1
        lda #PM_HUDX
        sta col
        lda #4
        sta row
        jsr cputs
        ; dots left (2 digits)
        lda pm_dots
        jsr two_digits
        stx numstr
        sta numstr+1
        lda #0
        sta numstr+2
        lda #<numstr
        sta ptr
        lda #>numstr
        sta ptr+1
        lda #PM_HUDX
        sta col
        lda #7
        sta row
        jsr cputs
        ; status
        lda pm_state
        beq pdh_done
        cmp #1
        beq pdh_win
        lda #<s_pmlose
        sta ptr
        lda #>s_pmlose
        sta ptr+1
        jmp pdh_put
pdh_win:
        lda #<s_pmwin
        sta ptr
        lda #>s_pmwin
        sta ptr+1
pdh_put:
        lda #PM_HUDX
        sta col
        lda #9
        sta row
        lda #T_FONTINV
        sta tmp
        jsr puts
        lda #<s_pmnew
        sta ptr
        lda #>s_pmnew
        sta ptr+1
        lda #PM_HUDX
        sta col
        lda #11
        sta row
        jsr cputs
pdh_done:
        rts

; ---- render_partials hook (vblank) -----------------------------------------
rp_pacman:
        lda f_pmstep
        beq rppm_hud
        lda #0
        sta f_pmstep
        ; erase all old actor cells first (handles swaps/overlaps), then draw
        lda pac_ocol
        sta pm_mcol
        lda pac_orow
        sta pm_mrow
        jsr pm_erase_at
        lda gh_ocol+0
        sta pm_mcol
        lda gh_orow+0
        sta pm_mrow
        jsr pm_erase_at
        lda gh_ocol+1
        sta pm_mcol
        lda gh_orow+1
        sta pm_mrow
        jsr pm_erase_at
        jsr pm_draw_actors
rppm_hud:
        lda f_pmhud
        beq rppm_done
        lda #0
        sta f_pmhud
        jsr pm_draw_hud
rppm_done:
        rts

; ---- data ------------------------------------------------------------------
pm_dx:  dc.b 0,$FF,0,1          ; up, left, down, right
pm_dy:  dc.b $FF,0,1,0
pm_tiles:
        dc.b 0, T_SOLC, T_DOT, T_PWR   ; floor, wall(blue), dot, power

pm_maze_tpl:
        dc.b 1,1,1,1,1,1,1,1,1,1,1,1,1
        dc.b 1,3,2,2,2,2,2,2,2,2,2,3,1
        dc.b 1,2,1,2,1,2,1,2,1,2,1,2,1
        dc.b 1,2,2,2,2,2,2,2,2,2,2,2,1
        dc.b 1,2,1,2,1,2,1,2,1,2,1,2,1
        dc.b 1,2,2,2,2,2,2,2,2,2,2,2,1
        dc.b 1,2,1,2,1,2,0,2,1,2,1,2,1
        dc.b 1,2,2,2,2,2,2,2,2,2,2,2,1
        dc.b 1,2,1,2,1,2,1,2,1,2,1,2,1
        dc.b 1,2,2,2,2,2,2,2,2,2,2,2,1
        dc.b 1,2,1,2,1,2,1,2,1,2,1,2,1
        dc.b 1,3,2,2,2,2,2,2,2,2,2,3,1
        dc.b 1,1,1,1,1,1,1,1,1,1,1,1,1

t_pacman:  dc.b "Pac-Man",0
s_pmscore: dc.b "Score",0
s_pmdots:  dc.b "Dots",0
s_pmwin:   dc.b "WIN!",0
s_pmlose:  dc.b "LOST",0
s_pmnew:   dc.b "A=new",0
