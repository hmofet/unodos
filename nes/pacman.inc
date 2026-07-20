; ============================================================================
; UnoDOS / NES — Pac-Man (app 9). The shared 13x13 pillar maze + greedy-chase
; ghosts (the C64 algorithm), re-expressed for the minimal profile. The port has
; no sprite layer, so the maze, dots and actors are ALL background tiles: blue
; walls (the solid-cyan tile under Pac-Man's own palette), white dots, yellow
; power pellets, a yellow Pac-Man and two white ghosts. D-pad steers (the turn is
; queued and adopted at the next open tile); ghosts greedily minimise Manhattan
; distance to Pac-Man (no reverse) — the documented "simple chase" deviation from
; the arcade scatter AI. Eat every dot to WIN; a ghost on your tile = game OVER.
;
; Rendering: the full maze + actors are drawn once (rendering off) on launch; each
; game step is a SMALL vblank partial (rp_pacman) that repaints only the few
; cells the actors vacated/entered — flicker-free, no full clear. Colours come
; from a per-app palette installed on draw (restored to default on exit).
; ============================================================================

PM_COLS EQU 13
PM_ROWS EQU 13
PM_OX   EQU 2           ; screen column of maze col 0
PM_OY   EQU 2           ; screen row of maze row 0
PM_RATE EQU 12          ; frames per game step
PMMAZE  = $0500         ; 169-byte mutable maze (0 floor,1 wall,2 dot,3 power)

pac_col  = $74
pac_row  = $75
pac_dir  = $76
pac_ndir = $77
opac_c   = $78
opac_r   = $79
pm_ctr   = $7A
pm_state = $7B          ; 0 play, 1 win, 2 lose
pm_dots  = $7C          ; word
pm_mc    = $7E          ; maze col scratch
pm_mr    = $7F          ; maze row scratch
pm_nc    = $80          ; candidate col
pm_nr    = $81          ; candidate row
pm_gi    = $82          ; ghost index
pm_bdir  = $83          ; ghost-steer best dir
pm_bd    = $84          ; ghost-steer best distance
pm_dir   = $85          ; ghost-steer dir loop
pm_ta    = $86          ; temp
pm_tb    = $87          ; temp
pf_pm    = $88          ; partial: a game step happened

gh_col   = $0240        ; 2 ghosts
gh_row   = $0242
gh_dir   = $0244
ogh_col  = $0246
ogh_row  = $0248

pm_dx:       dc.b 0,$FF,0,1      ; up,left,down,right
pm_dy:       dc.b $FF,0,1,0
pm_celltile: dc.b 0, T_SOLC, T_PMDOT, T_PMPOW   ; floor wall dot power
pm_palette:  dc.b $0F,$30,$11,$28               ; black, white, blue, yellow
s_pm_dots:   dc.b "Dots",0
s_pm_win:    dc.b "WIN!",0
s_pm_over:   dc.b "OVER",0
s_pm_blank:  dc.b "    ",0

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

; ---- pm_idx: pm_mc/pm_mr -> X = mr*13 + mc ---------------------------------
pm_idx:
        lda pm_mr
        asl
        asl
        sta pm_ta               ; mr*4
        asl                     ; mr*8
        clc
        adc pm_ta               ; mr*12
        clc
        adc pm_mr               ; mr*13
        clc
        adc pm_mc
        tax
        rts

; ---- pm_walk: pm_nc/pm_nr -> A=1 if in-bounds and not a wall, else 0 -------
pm_walk:
        lda pm_nc
        cmp #PM_COLS
        bcs pw_no
        lda pm_nr
        cmp #PM_ROWS
        bcs pw_no
        lda pm_nr
        asl
        asl
        sta pm_ta
        asl
        clc
        adc pm_ta
        clc
        adc pm_nr
        clc
        adc pm_nc
        tax
        lda PMMAZE,x
        cmp #1
        beq pw_no
        lda #1
        rts
pw_no:  lda #0
        rts

; ---- pm_draw_maze_cell: draw the maze tile at pm_mc/pm_mr -------------------
pm_draw_maze_cell:
        jsr pm_idx
        lda PMMAZE,x
        tay
        lda pm_celltile,y
        pha
        lda pm_mc
        clc
        adc #PM_OX
        sta col
        lda pm_mr
        clc
        adc #PM_OY
        sta row
        jsr set_nt_addr
        pla
        sta PPUDATA
        rts

; ---- pm_draw_actor: A = tile, at maze pm_mc/pm_mr --------------------------
pm_draw_actor:
        pha
        lda pm_mc
        clc
        adc #PM_OX
        sta col
        lda pm_mr
        clc
        adc #PM_OY
        sta row
        jsr set_nt_addr
        pla
        sta PPUDATA
        rts

; ---- pacman_init -----------------------------------------------------------
pacman_init:
        lda #0
        sta pm_dots
        sta pm_dots+1
        ldx #0
pmi_l:  lda pm_maze_tpl,x
        sta PMMAZE,x
        cmp #2
        bcc pmi_n               ; floor/wall not collectable
        inc pm_dots
        bne pmi_n
        inc pm_dots+1
pmi_n:  inx
        cpx #PM_COLS*PM_ROWS
        bne pmi_l
        ; pac at (6,11) heading left
        lda #6
        sta pac_col
        lda #11
        sta pac_row
        lda #1
        sta pac_dir
        sta pac_ndir
        ; clear the dot under pac's spawn
        lda #6
        sta pm_mc
        lda #11
        sta pm_mr
        jsr pm_idx
        lda PMMAZE,x
        cmp #2
        bcc pmi_g
        lda #0
        sta PMMAZE,x
        lda pm_dots
        sec
        sbc #1
        sta pm_dots
        lda pm_dots+1
        sbc #0
        sta pm_dots+1
pmi_g:
        ; ghosts either side of the house centre (6,6)
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
        sta pm_state
        lda #PM_RATE
        sta pm_ctr
        lda #1
        sta v_dirty
        rts

; ---- input + step ----------------------------------------------------------
pacman_update:
        lda pm_state
        bne pu_step             ; game over: ignore steering
        lda v_pade
        and #PAD_U
        beq pu_l
        lda #0
        sta pac_ndir
pu_l:   lda v_pade
        and #PAD_L
        beq pu_d
        lda #1
        sta pac_ndir
pu_d:   lda v_pade
        and #PAD_D
        beq pu_r
        lda #2
        sta pac_ndir
pu_r:   lda v_pade
        and #PAD_R
        beq pu_step
        lda #3
        sta pac_ndir
pu_step:
        lda pm_state
        bne pu_done             ; frozen on win/lose
  IF AUTOTEST
        lda a_gpause
        bne pu_done
  ENDIF
        dec pm_ctr
        bne pu_done
        lda #PM_RATE
        sta pm_ctr
        jsr pm_step
        lda #1
        sta pf_pm
pu_done:
        rts

pm_step:
        ; save old actor positions for the partial erase
        lda pac_col
        sta opac_c
        lda pac_row
        sta opac_r
        ldx #0
ps_save:
        lda gh_col,x
        sta ogh_col,x
        lda gh_row,x
        sta ogh_row,x
        inx
        cpx #2
        bne ps_save
        ; pac: adopt the queued turn if that tile is open
        ldx pac_ndir
        lda pac_col
        clc
        adc pm_dx,x
        sta pm_nc
        lda pac_row
        clc
        adc pm_dy,x
        sta pm_nr
        jsr pm_walk
        beq ps_keep
        lda pac_ndir
        sta pac_dir
ps_keep:
        ; move pac in pac_dir if open
        ldx pac_dir
        lda pac_col
        clc
        adc pm_dx,x
        sta pm_nc
        lda pac_row
        clc
        adc pm_dy,x
        sta pm_nr
        jsr pm_walk
        beq ps_nomove
        lda pm_nc
        sta pac_col
        lda pm_nr
        sta pac_row
ps_nomove:
        jsr pm_eat
        ; ghosts
        lda #0
        sta pm_gi
ps_gloop:
        jsr pm_ghost_steer
        ldx pm_gi
        lda gh_dir,x
        tay
        lda gh_col,x
        clc
        adc pm_dx,y
        sta pm_nc
        lda gh_row,x
        clc
        adc pm_dy,y
        sta pm_nr
        jsr pm_walk
        beq ps_gnext
        ldx pm_gi
        lda pm_nc
        sta gh_col,x
        lda pm_nr
        sta gh_row,x
ps_gnext:
        inc pm_gi
        lda pm_gi
        cmp #2
        bne ps_gloop
        jsr pm_check_hit
        ; all dots gone -> win
        lda pm_dots
        ora pm_dots+1
        bne ps_done
        lda #1
        sta pm_state
ps_done:
        rts

; ---- pm_eat: collect a dot/power on pac's tile -----------------------------
pm_eat:
        lda pac_col
        sta pm_mc
        lda pac_row
        sta pm_mr
        jsr pm_idx
        lda PMMAZE,x
        cmp #2
        bcc pe_done
        lda #0
        sta PMMAZE,x
        lda pm_dots
        sec
        sbc #1
        sta pm_dots
        lda pm_dots+1
        sbc #0
        sta pm_dots+1
pe_done:
        rts

; ---- pm_ghost_steer: pick gh_dir[pm_gi] = open, non-reverse dir that
; minimises Manhattan distance to Pac-Man -----------------------------------
pm_ghost_steer:
        lda #$FF
        sta pm_bd
        sta pm_bdir
        lda #0
        sta pm_dir
pgs_loop:
        ldx pm_gi
        lda gh_dir,x
        clc
        adc #2
        and #3
        cmp pm_dir
        beq pgs_next            ; skip the reverse of the current direction
        ldx pm_gi
        ldy pm_dir
        lda gh_col,x
        clc
        adc pm_dx,y
        sta pm_nc
        lda gh_row,x
        clc
        adc pm_dy,y
        sta pm_nr
        jsr pm_walk
        beq pgs_next
        lda pm_nc
        sec
        sbc pac_col
        bpl pgs_dx
        eor #$FF
        clc
        adc #1
pgs_dx: sta pm_tb
        lda pm_nr
        sec
        sbc pac_row
        bpl pgs_dy
        eor #$FF
        clc
        adc #1
pgs_dy: clc
        adc pm_tb               ; A = Manhattan distance
        cmp pm_bd
        bcs pgs_next            ; not strictly better
        sta pm_bd
        lda pm_dir
        sta pm_bdir
pgs_next:
        inc pm_dir
        lda pm_dir
        cmp #4
        bne pgs_loop
        lda pm_bdir
        cmp #$FF
        beq pgs_done
        ldx pm_gi
        sta gh_dir,x
pgs_done:
        rts

; ---- pm_check_hit: a ghost on pac's tile -> lose ---------------------------
pm_check_hit:
        ldx #0
pch_loop:
        lda gh_col,x
        cmp pac_col
        bne pch_next
        lda gh_row,x
        cmp pac_row
        bne pch_next
        lda #2
        sta pm_state
        rts
pch_next:
        inx
        cpx #2
        bne pch_loop
        rts

; ---- pm_set_palette / pm_draw_hud ------------------------------------------
pm_set_palette:
        bit PPUSTATUS
        lda #$3F
        sta PPUADDR
        lda #$00
        sta PPUADDR
        ldx #0
pmp_l:  lda pm_palette,x
        sta PPUDATA
        inx
        cpx #4
        bne pmp_l
        rts

pm_draw_hud:
        lda #<s_pm_dots
        sta ptr
        lda #>s_pm_dots
        sta ptr+1
        lda #PM_OX+PM_COLS+1
        sta col
        lda #PM_OY
        sta row
        jsr cputs
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
        lda #PM_OX+PM_COLS+1
        sta col
        lda #PM_OY+1
        sta row
        jsr cputs
        ; status
        lda pm_state
        beq phd_blank
        cmp #1
        beq phd_win
        lda #<s_pm_over
        sta ptr
        lda #>s_pm_over
        sta ptr+1
        jmp phd_draw
phd_win:
        lda #<s_pm_win
        sta ptr
        lda #>s_pm_win
        sta ptr+1
        jmp phd_draw
phd_blank:
        lda #<s_pm_blank
        sta ptr
        lda #>s_pm_blank
        sta ptr+1
phd_draw:
        lda #PM_OX+PM_COLS+1
        sta col
        lda #PM_OY+3
        sta row
        jmp cputs

; ---- app_pacman: chrome + palette + full maze + actors + HUD (off) ---------
app_pacman:
        lda #<l_pacman
        sta ptr
        lda #>l_pacman
        sta ptr+1
        jsr draw_chrome
        jsr pm_set_palette
        lda #0
        sta pm_mr
pda_rl:
        lda #0
        sta pm_mc
pda_cl:
        jsr pm_draw_maze_cell
        inc pm_mc
        lda pm_mc
        cmp #PM_COLS
        bne pda_cl
        inc pm_mr
        lda pm_mr
        cmp #PM_ROWS
        bne pda_rl
        ; actors
        lda pac_col
        sta pm_mc
        lda pac_row
        sta pm_mr
        lda #T_PMPAC
        jsr pm_draw_actor
        ldx #0
pda_ag: stx pm_gi
        lda gh_col,x
        sta pm_mc
        lda gh_row,x
        sta pm_mr
        lda #T_PMGHO
        jsr pm_draw_actor
        ldx pm_gi
        inx
        cpx #2
        bne pda_ag
        jmp pm_draw_hud

; ---- rp_pacman: vblank partial — repaint only the cells actors changed -----
rp_pacman:
        lda pf_pm
        bne rpm_go
        rts
rpm_go:
        ; erase old actor cells (repaint the maze tile beneath)
        lda opac_c
        sta pm_mc
        lda opac_r
        sta pm_mr
        jsr pm_draw_maze_cell
        ldx #0
rpm_eg: stx pm_gi
        lda ogh_col,x
        sta pm_mc
        lda ogh_row,x
        sta pm_mr
        jsr pm_draw_maze_cell
        ldx pm_gi
        inx
        cpx #2
        bne rpm_eg
        ; draw actors at their new tiles
        lda pac_col
        sta pm_mc
        lda pac_row
        sta pm_mr
        lda #T_PMPAC
        jsr pm_draw_actor
        ldx #0
rpm_dg: stx pm_gi
        lda gh_col,x
        sta pm_mc
        lda gh_row,x
        sta pm_mr
        lda #T_PMGHO
        jsr pm_draw_actor
        ldx pm_gi
        inx
        cpx #2
        bne rpm_dg
        jsr pm_draw_hud
        lda #0
        sta pf_pm
        rts
