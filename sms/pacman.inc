; ============================================================================
; UnoDOS/SMS — Pac-Man (proc 9). The shared 13x13 pillar maze + greedy-chase
; ghosts (the C64 algorithm, same as the NES/rpi ports), fitted to a window.
; The maze, dots and actors are all background tiles: cyan walls (T_SOLC), white
; dots, yellow power pellets, a yellow Pac-Man and two magenta ghosts. D-pad
; steers (the turn is queued and adopted at the next open tile); ghosts greedily
; minimise Manhattan distance to Pac-Man with no reverse — the documented
; "simple chase" deviation from the arcade scatter AI. Eat every dot to WIN; a
; ghost on your tile = game OVER. While topmost it owns the d-pad (B2 closes).
;
; Unlike the NES partial-repaint, the SMS WM redraws window content on v_dirty,
; so pacman_draw repaints the whole maze + actors + HUD each game step.
; ============================================================================

PM_COLS EQU 13
PM_ROWS EQU 13
PM_OX   EQU 1           ; maze col 0 = window content col (WX+1) + PM_OX
PM_OY   EQU 1           ; maze row 0 = window content row (WY+1) + PM_OY
PM_RATE EQU 10          ; frames per game step

PMMAZE   EQU $C300      ; 169-byte mutable maze (0 floor,1 wall,2 dot,3 power)
pac_col  EQU $C3B0
pac_row  EQU $C3B1
pac_dir  EQU $C3B2
pac_ndir EQU $C3B3
pm_ctr   EQU $C3B4
pm_state EQU $C3B5      ; 0 play, 1 win, 2 lose
pm_dots  EQU $C3B6      ; word
pm_mc    EQU $C3B8      ; maze col scratch
pm_mr    EQU $C3B9      ; maze row scratch
pm_nc    EQU $C3BA      ; candidate col
pm_nr    EQU $C3BB      ; candidate row
pm_gi    EQU $C3BC      ; ghost index (0..1)
pm_bdir  EQU $C3BD      ; ghost-steer best dir
pm_bd    EQU $C3BE      ; ghost-steer best distance
pm_dir   EQU $C3BF      ; ghost-steer dir loop
pm_tb    EQU $C3C6      ; temp
gh_col   EQU $C3C0      ; 2 ghosts
gh_row   EQU $C3C2      ; 2 ghosts
gh_dir   EQU $C3C4      ; 2 ghosts

pm_dx:       db 0,$FF,0,1      ; up,left,down,right
pm_dy:       db $FF,0,1,0
pm_celltile: db T_SGREEN, T_SOLC, T_PMDOT, T_PMPOW   ; 0 floor(unused),1 wall,2 dot,3 power
s_pm_dots:   db "Dots",0
s_pm_win:    db "WIN!",0
s_pm_over:   db "OVER",0
s_pm_blank:  db "    ",0

pm_maze_tpl:
        db 1,1,1,1,1,1,1,1,1,1,1,1,1
        db 1,3,2,2,2,2,2,2,2,2,2,3,1
        db 1,2,1,2,1,2,1,2,1,2,1,2,1
        db 1,2,2,2,2,2,2,2,2,2,2,2,1
        db 1,2,1,2,1,2,1,2,1,2,1,2,1
        db 1,2,2,2,2,2,2,2,2,2,2,2,1
        db 1,2,1,2,1,2,0,2,1,2,1,2,1
        db 1,2,2,2,2,2,2,2,2,2,2,2,1
        db 1,2,1,2,1,2,1,2,1,2,1,2,1
        db 1,2,2,2,2,2,2,2,2,2,2,2,1
        db 1,2,1,2,1,2,1,2,1,2,1,2,1
        db 1,3,2,2,2,2,2,2,2,2,2,3,1
        db 1,1,1,1,1,1,1,1,1,1,1,1,1

; ---- pm_idx: pm_mc/pm_mr -> HL = PMMAZE + mr*13 + mc -----------------------
pm_idx:
        ld a, (pm_mr)
        ld e, a
        add a, a                        ; 2
        add a, a                        ; 4
        ld d, a
        add a, a                        ; 8
        add a, d                        ; 12
        add a, e                        ; 13*mr
        ld e, a
        ld a, (pm_mc)
        add a, e
        ld e, a
        ld d, 0
        ld hl, PMMAZE
        add hl, de
        ret

; ---- pm_walk: pm_nc/pm_nr -> A=1 if in-bounds and not a wall, else 0 -------
pm_walk:
        ld a, (pm_nc)
        cp PM_COLS
        jr nc, pw_no                    ; unsigned: also catches -1 = $FF
        ld a, (pm_nr)
        cp PM_ROWS
        jr nc, pw_no
        ld a, (pm_nr)
        ld e, a
        add a, a
        add a, a
        ld d, a
        add a, a
        add a, d
        add a, e                        ; 13*nr
        ld e, a
        ld a, (pm_nc)
        add a, e
        ld e, a
        ld d, 0
        ld hl, PMMAZE
        add hl, de
        ld a, (hl)
        cp 1                            ; wall?
        jr z, pw_no
        ld a, 1
        ret
pw_no:
        xor a
        ret

; ---- pacman_init -----------------------------------------------------------
pacman_init:
        ; copy the template into mutable maze RAM + count collectables
        ld hl, pm_maze_tpl
        ld de, PMMAZE
        ld bc, 0                        ; bc = dot count
        ld a, PM_COLS*PM_ROWS
        ld (pm_ctr), a                  ; reuse pm_ctr as loop count
pmi_l:
        ld a, (hl)
        ld (de), a
        cp 2
        jr c, pmi_n                     ; floor/wall not collectable
        inc bc
pmi_n:
        inc hl
        inc de
        ld a, (pm_ctr)
        dec a
        ld (pm_ctr), a
        jr nz, pmi_l
        ld (pm_dots), bc
        ; pac at (6,11) heading left
        ld a, 6
        ld (pac_col), a
        ld a, 11
        ld (pac_row), a
        ld a, 1
        ld (pac_dir), a
        ld (pac_ndir), a
        ; clear the dot under pac's spawn
        ld a, 6
        ld (pm_mc), a
        ld a, 11
        ld (pm_mr), a
        call pm_idx
        ld a, (hl)
        cp 2
        jr c, pmi_g
        ld (hl), 0
        ld hl, (pm_dots)
        dec hl
        ld (pm_dots), hl
pmi_g:
        ; ghosts either side of the house centre (6,6)
        ld a, 5
        ld (gh_col+0), a
        ld a, 7
        ld (gh_col+1), a
        ld a, 6
        ld (gh_row+0), a
        ld (gh_row+1), a
        ld a, 3
        ld (gh_dir+0), a
        ld a, 1
        ld (gh_dir+1), a
        xor a
        ld (pm_state), a
        ld a, PM_RATE
        ld (pm_ctr), a
        ld a, 1
        ld (v_dirty), a
        ret

; ---- input + step ----------------------------------------------------------
pacman_input:
        call topmost_proc               ; ix = window
        ld a, (v_pad_edge)
        bit 5, a                        ; B2 -> close
        jp nz, close_topmost
        ld a, (pm_state)
        or a
        jr nz, pi_hover                 ; frozen on win/lose: no steer, no step
        ; steer -> pac_ndir  (U=0 L=1 D=2 R=3)
        ld a, (v_pad_edge)
        bit 0, a                        ; PAD_U
        jr z, pi_l
        xor a
        ld (pac_ndir), a
pi_l:   ld a, (v_pad_edge)
        bit 2, a                        ; PAD_L
        jr z, pi_d
        ld a, 1
        ld (pac_ndir), a
pi_d:   ld a, (v_pad_edge)
        bit 1, a                        ; PAD_D
        jr z, pi_r
        ld a, 2
        ld (pac_ndir), a
pi_r:   ld a, (v_pad_edge)
        bit 3, a                        ; PAD_R
        jr z, pi_tick
        ld a, 3
        ld (pac_ndir), a
pi_tick:
        ld a, (v_gpause)
        or a
        jr nz, pi_hover
        ld a, (pm_ctr)
        dec a
        ld (pm_ctr), a
        or a
        jr nz, pi_hover
        ld a, PM_RATE
        ld (pm_ctr), a
        call pm_step
        ld a, 1
        ld (v_dirty), a
pi_hover:
        ; park the sprite cursor on Pac-Man
        ld a, (ix+WX)
        add a, 1+PM_OX
        ld b, a
        ld a, (pac_col)
        add a, b
        add a, a
        add a, a
        add a, a
        ld (v_curx), a
        ld a, (ix+WY)
        add a, 1+PM_OY
        ld b, a
        ld a, (pac_row)
        add a, b
        add a, a
        add a, a
        add a, a
        ld (v_cury), a
        ret

; ---- pm_step: advance pac + ghosts one tile -------------------------------
pm_step:
        ; pac: adopt the queued turn if that tile is open
        ld a, (pac_ndir)
        call pm_cand_pac                ; pm_nc/pm_nr = pac + dir[a]
        call pm_walk
        or a
        jr z, ps_keep
        ld a, (pac_ndir)
        ld (pac_dir), a
ps_keep:
        ; move pac in pac_dir if open
        ld a, (pac_dir)
        call pm_cand_pac
        call pm_walk
        or a
        jr z, ps_nomove
        ld a, (pm_nc)
        ld (pac_col), a
        ld a, (pm_nr)
        ld (pac_row), a
ps_nomove:
        call pm_eat
        ; ghosts
        xor a
        ld (pm_gi), a
ps_gloop:
        call pm_ghost_steer
        ; move ghost gi in gh_dir[gi] if open
        ld a, (pm_gi)
        ld e, a
        ld d, 0
        ld hl, gh_dir
        add hl, de
        ld a, (hl)                      ; dir
        call pm_cand_ghost2             ; pm_nc/pm_nr = ghost[gi] + d[dir]
        call pm_walk
        or a
        jr z, ps_gnext
        ld a, (pm_gi)
        ld e, a
        ld d, 0
        ld hl, gh_col
        add hl, de
        ld a, (pm_nc)
        ld (hl), a
        ld hl, gh_row
        add hl, de
        ld a, (pm_nr)
        ld (hl), a
ps_gnext:
        ld a, (pm_gi)
        inc a
        ld (pm_gi), a
        cp 2
        jr nz, ps_gloop
        call pm_check_hit
        ; all dots gone -> win
        ld hl, (pm_dots)
        ld a, h
        or l
        ret nz
        ld a, 1
        ld (pm_state), a
        ret

; pm_cand_pac: A=dir -> pm_nc = pac_col+dx[dir], pm_nr = pac_row+dy[dir]
pm_cand_pac:
        ld e, a
        ld d, 0
        ld hl, pm_dx
        add hl, de
        ld c, (hl)
        ld a, (pac_col)
        add a, c
        ld (pm_nc), a
        ld hl, pm_dy
        add hl, de
        ld c, (hl)
        ld a, (pac_row)
        add a, c
        ld (pm_nr), a
        ret

; ---- pm_eat: collect a dot/power on pac's tile ----------------------------
pm_eat:
        ld a, (pac_col)
        ld (pm_mc), a
        ld a, (pac_row)
        ld (pm_mr), a
        call pm_idx
        ld a, (hl)
        cp 2
        ret c
        ld (hl), 0
        ld hl, (pm_dots)
        dec hl
        ld (pm_dots), hl
        ret

; ---- pm_ghost_steer: gh_dir[gi] = open non-reverse dir minimising Manhattan
; distance to Pac-Man ---------------------------------------------------------
pm_ghost_steer:
        ld a, $FF
        ld (pm_bd), a
        ld (pm_bdir), a
        xor a
        ld (pm_dir), a
pgs_loop:
        ; skip the reverse of the ghost's current direction
        ld a, (pm_gi)
        ld e, a
        ld d, 0
        ld hl, gh_dir
        add hl, de
        ld a, (hl)
        add a, 2
        and 3
        ld hl, pm_dir
        cp (hl)
        jr z, pgs_next
        ; candidate tile in pm_dir
        ld a, (pm_dir)
        call pm_cand_ghost2             ; pm_nc/pm_nr = ghost[gi] + d[pm_dir]
        call pm_walk
        or a
        jr z, pgs_next
        ; Manhattan distance = |nc-pac_col| + |nr-pac_row|
        ld a, (pm_nc)
        ld hl, pac_col
        sub (hl)
        jp p, pgs_dx
        neg
pgs_dx: ld (pm_tb), a
        ld a, (pm_nr)
        ld hl, pac_row
        sub (hl)
        jp p, pgs_dy
        neg
pgs_dy: ld hl, pm_tb
        add a, (hl)                     ; A = distance
        ld hl, pm_bd
        cp (hl)
        jr nc, pgs_next                 ; not strictly better
        ld (pm_bd), a
        ld a, (pm_dir)
        ld (pm_bdir), a
pgs_next:
        ld a, (pm_dir)
        inc a
        ld (pm_dir), a
        cp 4
        jr nz, pgs_loop
        ld a, (pm_bdir)
        cp $FF
        ret z
        ld e, a
        ld a, (pm_gi)
        ld c, a
        ld b, 0
        ld hl, gh_dir
        add hl, bc
        ld (hl), e
        ret

; pm_cand_ghost2: A=dir -> pm_nc/pm_nr = ghost[pm_gi] + d[dir] (dir preserved)
pm_cand_ghost2:
        push af
        ld e, a
        ld d, 0
        ld hl, pm_dx
        add hl, de
        ld c, (hl)
        ld a, (pm_gi)
        ld e, a
        ld d, 0
        ld hl, gh_col
        add hl, de
        ld a, (hl)
        add a, c
        ld (pm_nc), a
        pop af
        ld e, a
        ld d, 0
        ld hl, pm_dy
        add hl, de
        ld c, (hl)
        ld a, (pm_gi)
        ld e, a
        ld d, 0
        ld hl, gh_row
        add hl, de
        ld a, (hl)
        add a, c
        ld (pm_nr), a
        ret

; ---- pm_check_hit: a ghost on pac's tile -> lose --------------------------
pm_check_hit:
        ld b, 0
pch_loop:
        ld a, b
        ld e, a
        ld d, 0
        ld hl, gh_col
        add hl, de
        ld a, (hl)
        ld hl, pac_col
        cp (hl)
        jr nz, pch_next
        ld a, b
        ld e, a
        ld hl, gh_row
        add hl, de
        ld a, (hl)
        ld hl, pac_row
        cp (hl)
        jr nz, pch_next
        ld a, 2
        ld (pm_state), a
        ret
pch_next:
        inc b
        ld a, b
        cp 2
        jr nz, pch_loop
        ret

; ---- draw (ix = window): full maze + actors + HUD -------------------------
pacman_draw:
        ; maze cells
        xor a
        ld (pm_mr), a
pd_rl:
        xor a
        ld (pm_mc), a
pd_cl:
        call pm_draw_cell
        ld a, (pm_mc)
        inc a
        ld (pm_mc), a
        cp PM_COLS
        jr nz, pd_cl
        ld a, (pm_mr)
        inc a
        ld (pm_mr), a
        cp PM_ROWS
        jr nz, pd_rl
        ; Pac-Man
        ld a, (pac_col)
        ld (pm_mc), a
        ld a, (pac_row)
        ld (pm_mr), a
        ld a, T_PMPAC
        call pm_draw_at
        ; ghosts
        ld b, 0
pd_gl:
        push bc
        ld a, b
        ld e, a
        ld d, 0
        ld hl, gh_col
        add hl, de
        ld a, (hl)
        ld (pm_mc), a
        ld hl, gh_row
        add hl, de
        ld a, (hl)
        ld (pm_mr), a
        ld a, T_PMGHO
        call pm_draw_at
        pop bc
        inc b
        ld a, b
        cp 2
        jr nz, pd_gl
        jp pm_draw_hud

; pm_draw_cell: draw the maze tile at pm_mc/pm_mr (floor -> window bg blank)
pm_draw_cell:
        call pm_idx
        ld a, (hl)
        or a
        jr nz, pdc_tile
        ; floor: draw a blank (window bg) space
        ld a, T_FONT                    ; T_FONT = ' ' at index 1 (space)
        jr pm_draw_a2
pdc_tile:
        ld e, a
        ld d, 0
        ld hl, pm_celltile
        add hl, de
        ld a, (hl)
        jr pm_draw_a2

; pm_draw_at: A = tile, draw at pm_mc/pm_mr (window-relative)
pm_draw_at:
pm_draw_a2:
        ld c, a                         ; c = tile
        ld a, (ix+WX)
        add a, 1+PM_OX
        ld b, a
        ld a, (pm_mc)
        add a, b
        ld (dp_col), a
        ld a, (ix+WY)
        add a, 1+PM_OY
        ld b, a
        ld a, (pm_mr)
        add a, b
        ld (dp_row), a
        ld a, 1
        ld (dp_w), a
        ld (dp_h), a
        ld a, c
        ld (dp_tile), a
        jp fill_cells

; ---- pm_draw_hud: Dots NN + status ----------------------------------------
pm_draw_hud:
        ld a, (ix+WX)
        add a, 1+PM_OX+PM_COLS+1
        ld d, a
        ld a, (ix+WY)
        add a, 1+PM_OY
        ld e, a
        ld hl, s_pm_dots
        ld c, T_FONT
        call puts_at
        ; count on the next row
        ld a, (pm_dots)
        ld hl, g_numstr
        call put2
        xor a
        ld (hl), a
        ld a, (ix+WX)
        add a, 1+PM_OX+PM_COLS+1
        ld d, a
        ld a, (ix+WY)
        add a, 1+PM_OY+1
        ld e, a
        ld hl, g_numstr
        ld c, T_FONT
        call puts_at
        ; status: WIN!/OVER/blank
        ld a, (ix+WX)
        add a, 1+PM_OX+PM_COLS+1
        ld d, a
        ld a, (ix+WY)
        add a, 1+PM_OY+3
        ld e, a
        ld hl, s_pm_blank
        ld a, (pm_state)
        or a
        jr z, phd_go
        cp 1
        jr nz, phd_over
        ld hl, s_pm_win
        jr phd_go
phd_over:
        ld hl, s_pm_over
phd_go:
        ld c, T_FONT
        jp puts_at

  IFDEF AUTOTEST_PACMAN
auto_script:
    db 5, 76, 140, 0                 ; settle on the Pac-Man icon (i=9: col9,row17)
    db 2, 76, 140, $10               ; click -> launch Pac-Man
    db 4, 76, 140, 0
    db 40, 76, 140, 0                ; let the ghosts chase for a few steps
    db 0, 0, 0, 0                    ; end -> idle
  ENDIF
