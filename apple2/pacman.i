; ============================================================================
; UnoDOS/AppleII Pac-Man (milestone 3) - the 1 MHz-envelope adaptation of
; macplus/pacman.i (itself the 28x25 / 3-ghost arcade port). Documented
; deviations, all forced by the 6502 frame budget (HANDOFF-M3 SS1-2):
;   * 13x13 maze (the mkmaze.py-generated pillar grid), not the 28x25 arcade
;     board - one byte-column (7px) per tile, so a tile is a single store.
;   * Tile-stepped actors (one whole tile per game tick), not pixel-substep
;     sprites - the documented "7-px actors" deviation. Movement is driven by
;     the M1 soft tick (pacman_tick fires once per soft-clock second, like
;     Dostris's gravity), so harness `wait N` advances N deterministic steps.
;   * Two ghosts (Blinky=direct-chase, Pinky=4-ahead), not four; same
;     scatter/chase alternation + frightened-on-power-pellet + eat-chain AI,
;     re-expressed with a Manhattan-distance argmin steer (no division).
; 1-bit scheme (absolute black/white, like Dostris - content is not themed):
;   backdrop black, walls 50% dither, dots/power white pips, pac solid disc,
;   Blinky solid body, Pinky hollow body, frightened a scared face, eaten
;   just the eyes heading home (the dither-density ghost identities of the
;   68K ports, mapped onto a 7x8 cell).
;
;   Left/Right/Up/Down  steer (queued; pac turns at the next open tile)
;   N  new game        Esc  back to the desktop (maze state is lost)
; ============================================================================

PM_COLS    equ 13
PM_ROWS    equ 13
PM_BOARD_Y equ 8         ; pixel row of board row 0 (content row1)
PM_HUD_COL equ 15        ; HUD text column (right of the 13-col board)
PM_HOME_C  equ 6         ; ghost-house tile (maze cell 0)
PM_HOME_R  equ 6
PM_STEP_DIV equ 3        ; advance the game once every N soft-clock seconds
PM_FRIGHT  equ 5         ; frightened duration in game steps
PM_SCAT_T  equ 4         ; scatter phase length (game steps)
PM_CHASE_T equ 10        ; chase phase length (game steps)

; ghost modes
GM_NORMAL  equ 0         ; obeys pm_mode (scatter/chase)
GM_FRIGHT  equ 1
GM_EATEN   equ 2

; ---- BSS (above DOSBOARD; KBSS=$9000, NOTEBUF ends $9CFF, DOSBOARD
; $9D00-$9DC7) ----
PMMAZE equ KBSS+$E00     ; $9E00-$9EA8: 13x13 mutable maze (1 byte/cell)

; ---- direction tables (0=up,1=left,2=down,3=right; -1 stored as $FF) ----
pm_dx: dc.b 0,$FF,0,1
pm_dy: dc.b $FF,0,1,0

; ---- 7x8 cell sprites: 8 bytes, one per pixel row (bit0=leftmost pixel) ----
spr_blank: dc.b $00,$00,$00,$00,$00,$00,$00,$00
spr_wall:  dc.b $55,$2A,$55,$2A,$55,$2A,$55,$2A   ; 50% dither
spr_dot:   dc.b $00,$00,$00,$08,$08,$00,$00,$00   ; 2px centre pip
spr_power: dc.b $00,$00,$1C,$1C,$1C,$1C,$00,$00   ; 3x4 block
spr_pac:   dc.b $00,$1C,$3E,$7F,$7F,$3E,$1C,$00   ; solid disc
spr_gh0:   dc.b $00,$1C,$3E,$7F,$7F,$7F,$55,$00   ; Blinky: solid, legged base
spr_gh1:   dc.b $00,$1C,$22,$41,$41,$7F,$55,$00   ; Pinky: hollow body
spr_fr:    dc.b $00,$1C,$3E,$5D,$7F,$6B,$55,$00   ; frightened: scared face
spr_eat:   dc.b $00,$00,$22,$22,$00,$00,$00,$00   ; eaten: eyes only

; cell-value -> sprite (0=blank,1=wall,2=dot,3=power)
pm_cellspr_lo: dc.b <spr_blank,<spr_wall,<spr_dot,<spr_power
pm_cellspr_hi: dc.b >spr_blank,>spr_wall,>spr_dot,>spr_power

; ---- maze template (mkmaze.py): 0 floor,1 wall,2 dot,3 power ----
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

; ---------------------------------------------------------------- pm_calc_idx
; pm_calc_idx - zpPMCol/zpPMRow -> zpPMIdx = row*13+col (0-168, mul13 via
; r*8+r*4+r). Leaves X = idx.
pm_calc_idx:
        lda zpPMRow
        asl
        asl
        sta zpPMT1              ; r*4
        asl                     ; r*8
        clc
        adc zpPMT1              ; r*12
        clc
        adc zpPMRow             ; r*13
        clc
        adc zpPMCol
        sta zpPMIdx
        tax
        rts

; -------------------------------------------------------------- pm_step_walkable
; pm_step_walkable - candidate zpPMNCol/zpPMNRow -> A=1 if in-bounds (0-12)
; and the maze cell is not a wall (cell!=1), else A=0. Preserves zpPMCol/Row
; (uses its own inline mul13 on the candidate).
pm_step_walkable:
        lda zpPMNCol
        cmp #PM_COLS
        bcs psw_no              ; >=13 (also catches the $FF underflow)
        lda zpPMNRow
        cmp #PM_ROWS
        bcs psw_no
        lda zpPMNRow
        asl
        asl
        sta zpPMT1
        asl
        clc
        adc zpPMT1              ; nrow*12
        clc
        adc zpPMNRow            ; nrow*13
        clc
        adc zpPMNCol
        tax
        lda PMMAZE,x
        cmp #1
        beq psw_no
        lda #1
        rts
psw_no:
        lda #0
        rts

; ----------------------------------------------------------------- pm_blit8
; pm_blit8 - draw an 8-byte cell sprite: zpPMCol = byte-column, zpTmp =
; top pixel row, zpPtr = sprite pointer.
pm_blit8:
        ldx #0
pb_loop:
        txa
        clc
        adc zpTmp               ; absolute pixel row (8..111, no carry)
        tay
        lda rowlo,y
        clc
        adc zpPMCol
        sta zpDst
        lda rowhi,y
        adc #0
        sta zpDst+1
        txa
        tay
        lda (zpPtr),y           ; sprite row
        ldy #0
        sta (zpDst),y
        inx
        cpx #8
        bne pb_loop
        rts

; --------------------------------------------------------------- pm_draw_cell
; pm_draw_cell - redraw the maze tile at zpPMCol/zpPMRow from PMMAZE (blank/
; wall/dot/power). Used by the full redraw and to erase old actor tiles.
pm_draw_cell:
        jsr pm_calc_idx
        lda PMMAZE,x
        asl
        tax
        lda pm_cellspr_lo,x
        sta zpPtr
        lda pm_cellspr_hi,x
        sta zpPtr+1
        lda zpPMRow
        asl
        asl
        asl
        clc
        adc #PM_BOARD_Y
        sta zpTmp
        jmp pm_blit8

; --------------------------------------------------------------- pm_draw_spr
; pm_draw_spr - draw the sprite at zpPtr over tile zpPMCol/zpPMRow (actors).
pm_draw_spr:
        lda zpPMRow
        asl
        asl
        asl
        clc
        adc #PM_BOARD_Y
        sta zpTmp
        jmp pm_blit8

; -------------------------------------------------------------- pm_set_srcptr
; pm_set_srcptr - A=lo,X=hi -> zpPtr (helper to shorten sprite selection).
pm_set_srcptr:
        sta zpPtr
        stx zpPtr+1
        rts

; ============================================================================
; game logic
; ============================================================================

; --------------------------------------------------------------- pm_load_maze
; pm_load_maze - copy pm_maze_tpl into PMMAZE, counting dots+power into
; pm_dots.
pm_load_maze:
        lda #0
        sta pm_dots
        sta pm_dots+1
        ldx #0
plm_cp:
        lda pm_maze_tpl,x
        sta PMMAZE,x
        cmp #2
        bcc plm_next            ; 0 or 1: not collectable
        inc pm_dots
        bne plm_next
        inc pm_dots+1
plm_next:
        inx
        cpx #(PM_COLS*PM_ROWS)
        bne plm_cp
        rts

; ------------------------------------------------------------ pm_reset_actors
; pm_reset_actors - place pac (bottom centre) and the two ghosts (either side
; of the house), clear frightened/mode state. Clears pac's start dot.
pm_reset_actors:
        lda #6
        sta pac_col
        lda #11
        sta pac_row
        lda #1                  ; facing left
        sta pac_dir
        sta pac_nextdir
        ; clear the dot under pac's spawn tile
        lda #6
        sta zpPMCol
        lda #11
        sta zpPMRow
        jsr pm_calc_idx
        lda PMMAZE,x
        cmp #2
        bcc pra_g               ; already floor/wall
        lda #0
        sta PMMAZE,x
        lda pm_dots
        sec
        sbc #1
        sta pm_dots
        lda pm_dots+1
        sbc #0
        sta pm_dots+1
pra_g:
        lda #5                  ; ghost 0 (Blinky) left of house
        sta gh_col+0
        lda #6
        sta gh_row+0
        lda #1
        sta gh_dir+0
        lda #GM_NORMAL
        sta gh_mode+0
        lda #7                  ; ghost 1 (Pinky) right of house
        sta gh_col+1
        lda #6
        sta gh_row+1
        lda #3
        sta gh_dir+1
        lda #GM_NORMAL
        sta gh_mode+1
        lda #0
        sta pm_fright
        sta pm_mode             ; start in scatter
        sta pm_modet
        sta pm_subtick
        rts

; ----------------------------------------------------------------- pm_newgame
pm_newgame:
        lda #0
        sta pm_score
        sta pm_score+1
        sta pm_state            ; 0 = playing
        lda #3
        sta pm_lives
        lda #1
        sta pm_level
        lda #1
        sta pm_rng
        jsr pm_load_maze
        jsr pm_reset_actors
        rts

; ------------------------------------------------------------------- pm_rand
; pm_rand - advance the 8-bit LCG (x'=5x+1) -> A = new pm_rng.
pm_rand:
        lda pm_rng
        asl
        asl
        clc
        adc pm_rng
        clc
        adc #1
        sta pm_rng
        rts

; ----------------------------------------------------------------- pm_eat
; pm_eat - pac has entered tile pac_col/pac_row: eat a dot/power pellet.
pm_eat:
        lda pac_col
        sta zpPMCol
        lda pac_row
        sta zpPMRow
        jsr pm_calc_idx         ; X = idx
        lda PMMAZE,x
        cmp #2
        beq pe_dot
        cmp #3
        beq pe_power
        rts
pe_dot:
        lda #0
        sta PMMAZE,x
        lda pm_score
        clc
        adc #10
        sta pm_score
        bcc pe_dot2
        inc pm_score+1
pe_dot2:
        jmp pm_dec_dots
pe_power:
        lda #0
        sta PMMAZE,x
        lda pm_score
        clc
        adc #50
        sta pm_score
        bcc pe_pow2
        inc pm_score+1
pe_pow2:
        jsr pm_dec_dots
        ; frighten normal ghosts + reverse them
        lda #PM_FRIGHT
        sta pm_fright
        ldx #0
pe_frloop:
        lda gh_mode,x
        cmp #GM_NORMAL
        bne pe_frnext
        lda #GM_FRIGHT
        sta gh_mode,x
        lda gh_dir,x
        eor #2
        sta gh_dir,x
pe_frnext:
        inx
        cpx #2
        bne pe_frloop
        rts

; pm_dec_dots - pm_dots-- (16-bit).
pm_dec_dots:
        lda pm_dots
        sec
        sbc #1
        sta pm_dots
        lda pm_dots+1
        sbc #0
        sta pm_dots+1
        rts

; --------------------------------------------------------------- pm_move_pac
; pm_move_pac - apply queued turn if open, then advance one tile if open.
; Eats on arrival. Returns with C set if a level-clear happened (caller does
; a full reset+redraw).
pm_move_pac:
        ; try nextdir
        ldx pac_nextdir
        lda pac_col
        clc
        adc pm_dx,x
        sta zpPMNCol
        lda pac_row
        clc
        adc pm_dy,x
        sta zpPMNRow
        jsr pm_step_walkable
        beq pmp_keepdir
        lda pac_nextdir
        sta pac_dir
pmp_keepdir:
        ldx pac_dir
        lda pac_col
        clc
        adc pm_dx,x
        sta zpPMNCol
        lda pac_row
        clc
        adc pm_dy,x
        sta zpPMNRow
        jsr pm_step_walkable
        beq pmp_blocked
        lda zpPMNCol
        sta pac_col
        lda zpPMNRow
        sta pac_row
        jsr pm_eat
        ; level clear?
        lda pm_dots
        ora pm_dots+1
        bne pmp_blocked
        sec                     ; signal level clear
        rts
pmp_blocked:
        clc
        rts

; ----------------------------------------------------------------- pm_steer
; pm_steer - choose gh[zpPMGI]'s direction at its current tile. Frightened:
; a random non-reverse open dir. Otherwise: the open, non-reverse dir whose
; resulting tile minimises Manhattan distance to the mode/role target.
pm_steer:
        ldx zpPMGI
        lda gh_dir,x
        eor #2
        sta zpPMRevDir          ; reverse direction (forbidden unless trapped)
        lda gh_mode,x
        cmp #GM_FRIGHT
        bne ps_target
        ; ---- frightened: random non-reverse open dir, up to 4 tries ----
        jsr pm_rand
        and #3
        sta zpPMDir             ; random start direction
        lda #4
        sta zpPMT2              ; remaining tries
ps_rloop:
        lda zpPMDir
        and #3
        cmp zpPMRevDir
        beq ps_rnext
        sta zpPMDir2
        tax
        ldy zpPMGI
        lda gh_col,y
        clc
        adc pm_dx,x
        sta zpPMNCol
        lda gh_row,y
        clc
        adc pm_dy,x
        sta zpPMNRow
        jsr pm_step_walkable
        beq ps_rnext
        ldx zpPMGI
        lda zpPMDir2
        sta gh_dir,x
        rts
ps_rnext:
        inc zpPMDir
        dec zpPMT2
        bne ps_rloop
        ldx zpPMGI              ; trapped: reverse
        lda zpPMRevDir
        sta gh_dir,x
        rts

; (pm_steer continues below with the target branch)
ps_target:
        ; compute target tile -> zpPMTCol/zpPMTRow
        lda gh_mode,x
        cmp #GM_EATEN
        bne ps_noteaten
        lda #PM_HOME_C
        sta zpPMTCol
        lda #PM_HOME_R
        sta zpPMTRow
        jmp ps_pick
ps_noteaten:
        lda pm_mode
        beq ps_scatter          ; pm_mode 0 = scatter
        ; ---- chase ----
        lda zpPMGI
        bne ps_pinky
        ; Blinky: target pac directly
        lda pac_col
        sta zpPMTCol
        lda pac_row
        sta zpPMTRow
        jmp ps_pick
ps_pinky:
        ; Pinky: 4 tiles ahead of pac (clamped 0..12)
        lda pac_col
        sta zpPMTCol
        lda pac_row
        sta zpPMTRow
        ldx pac_dir
        ldy #4
ps_ahead:
        lda zpPMTCol
        clc
        adc pm_dx,x
        sta zpPMTCol
        lda zpPMTRow
        clc
        adc pm_dy,x
        sta zpPMTRow
        dey
        bne ps_ahead
        jsr pm_clamp_target
        jmp ps_pick
ps_scatter:
        ; scatter corners: Blinky top-right, Pinky top-left (both away from
        ; pac's bottom-centre spawn, so the opening seconds stay survivable)
        lda zpPMGI
        bne ps_sc1
        lda #11
        sta zpPMTCol
        lda #1
        sta zpPMTRow
        jmp ps_pick
ps_sc1:
        lda #1
        sta zpPMTCol
        lda #1
        sta zpPMTRow
ps_pick:
        ; argmin Manhattan over the 4 dirs, excluding reverse
        lda #$FF
        sta zpPMBestDir
        lda #$FF
        sta zpPMBestD
        lda #0
        sta zpPMDir
ps_ploop:
        lda zpPMDir
        cmp zpPMRevDir
        beq ps_pnext
        ldx zpPMDir
        ldy zpPMGI
        lda gh_col,y
        clc
        adc pm_dx,x
        sta zpPMNCol
        lda gh_row,y
        clc
        adc pm_dy,x
        sta zpPMNRow
        jsr pm_step_walkable
        beq ps_pnext
        jsr pm_manhattan        ; A = |ncol-tcol|+|nrow-trow|
        cmp zpPMBestD
        bcs ps_pnext            ; >= best: keep current best
        sta zpPMBestD
        lda zpPMDir
        sta zpPMBestDir
ps_pnext:
        inc zpPMDir
        lda zpPMDir
        cmp #4
        bne ps_ploop
        ; commit
        lda zpPMBestDir
        cmp #$FF
        bne ps_commit
        lda zpPMRevDir          ; dead end: reverse
ps_commit:
        ldx zpPMGI
        sta gh_dir,x
        rts

; pm_clamp_target - clamp zpPMTCol/zpPMTRow to 0..12 (underflow $80+ -> 0).
pm_clamp_target:
        lda zpPMTCol
        bmi pct_c0
        cmp #PM_COLS
        bcc pct_r
        lda #(PM_COLS-1)
        sta zpPMTCol
        jmp pct_r
pct_c0:
        lda #0
        sta zpPMTCol
pct_r:
        lda zpPMTRow
        bmi pct_r0
        cmp #PM_ROWS
        bcc pct_done
        lda #(PM_ROWS-1)
        sta zpPMTRow
        rts
pct_r0:
        lda #0
        sta zpPMTRow
pct_done:
        rts

; pm_manhattan - A = |zpPMNCol-zpPMTCol| + |zpPMNRow-zpPMTRow|.
pm_manhattan:
        lda zpPMNCol
        sec
        sbc zpPMTCol
        bpl pmm_x
        eor #$FF
        clc
        adc #1
pmm_x:
        sta zpPMT2
        lda zpPMNRow
        sec
        sbc zpPMTRow
        bpl pmm_y
        eor #$FF
        clc
        adc #1
pmm_y:
        clc
        adc zpPMT2
        rts

; --------------------------------------------------------------- pm_move_ghost
; pm_move_ghost - zpPMGI = ghost index: eaten-arrival check, steer, advance
; one tile if open. Frightened ghosts move at half speed (only on even
; sub-ticks). Returns C set if this ghost just killed pac (caller resets).
pm_move_ghost:
        ldx zpPMGI
        ; eaten ghost reaching home resumes normal mode
        lda gh_mode,x
        cmp #GM_EATEN
        bne pmg_steer
        lda gh_col,x
        cmp #PM_HOME_C
        bne pmg_steer
        lda gh_row,x
        cmp #PM_HOME_R
        bne pmg_steer
        lda #GM_NORMAL
        sta gh_mode,x
pmg_steer:
        jsr pm_steer
        ; half speed when frightened
        ldx zpPMGI
        lda gh_mode,x
        cmp #GM_FRIGHT
        bne pmg_do
        lda pm_subtick
        bne pmg_do
        jmp pmg_collide         ; skip move this tick, still test collision
pmg_do:
        ldx zpPMGI
        ldy gh_dir,x
        lda gh_col,x
        clc
        adc pm_dx,y
        sta zpPMNCol
        lda gh_row,x
        clc
        adc pm_dy,y
        sta zpPMNRow
        jsr pm_step_walkable
        beq pmg_collide
        ldx zpPMGI
        lda zpPMNCol
        sta gh_col,x
        lda zpPMNRow
        sta gh_row,x
pmg_collide:
        ldx zpPMGI
        lda gh_col,x
        cmp pac_col
        bne pmg_nocol
        lda gh_row,x
        cmp pac_row
        bne pmg_nocol
        ; same tile as pac
        lda gh_mode,x
        cmp #GM_FRIGHT
        bne pmg_notfright
        ; eat the ghost: +200, send home
        lda #GM_EATEN
        sta gh_mode,x
        lda pm_score
        clc
        adc #200
        sta pm_score
        bcc pmg_nocol
        inc pm_score+1
        jmp pmg_nocol
pmg_notfright:
        cmp #GM_EATEN
        beq pmg_nocol           ; harmless eyes
        sec                     ; pac dies
        rts
pmg_nocol:
        clc
        rts

; ============================================================================
; rendering
; ============================================================================

; pm_draw_actors - draw pac then the two ghosts over their tiles.
pm_draw_actors:
        lda pac_col
        sta zpPMCol
        lda pac_row
        sta zpPMRow
        lda #<spr_pac
        ldx #>spr_pac
        jsr pm_set_srcptr
        jsr pm_draw_spr
        lda #0
        sta zpPMGI
pda_loop:
        ldx zpPMGI
        lda gh_col,x
        sta zpPMCol
        lda gh_row,x
        sta zpPMRow
        lda gh_mode,x
        cmp #GM_EATEN
        beq pda_eaten
        cmp #GM_FRIGHT
        beq pda_fright
        ; normal: per-ghost body
        lda zpPMGI
        bne pda_gh1
        lda #<spr_gh0
        ldx #>spr_gh0
        jmp pda_set
pda_gh1:
        lda #<spr_gh1
        ldx #>spr_gh1
        jmp pda_set
pda_fright:
        lda #<spr_fr
        ldx #>spr_fr
        jmp pda_set
pda_eaten:
        lda #<spr_eat
        ldx #>spr_eat
pda_set:
        jsr pm_set_srcptr
        jsr pm_draw_spr
        inc zpPMGI
        lda zpPMGI
        cmp #2
        bne pda_loop
        rts

; pm_draw_hud - score / lives / level / status (cols 15+).
pm_draw_hud:
        ; clear HUD column band first (cols 15-39, rows 1-8)
        lda #(PM_HUD_COL-1)*1
        sta zpFX
        lda #8
        sta zpFY
        lda #(SCRCOLS-PM_HUD_COL+1)
        sta zpFW
        lda #72
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows

        lda #PM_HUD_COL
        sta zpCol
        lda #1
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_pm_score
        sta zpPtr
        lda #>msg_pm_score
        sta zpPtr+1
        jsr draw_string
        lda pm_score
        sta zpFSSize
        lda pm_score+1
        sta zpFSSize+1
        lda #(PM_HUD_COL+7)
        sta zpCol
        jsr draw_dec16

        lda #PM_HUD_COL
        sta zpCol
        lda #3
        sta zpRow
        lda #<msg_pm_lives
        sta zpPtr
        lda #>msg_pm_lives
        sta zpPtr+1
        jsr draw_string
        lda pm_lives
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        lda #(PM_HUD_COL+7)
        sta zpCol
        jsr draw_dec16

        lda #PM_HUD_COL
        sta zpCol
        lda #5
        sta zpRow
        lda #<msg_pm_level
        sta zpPtr
        lda #>msg_pm_level
        sta zpPtr+1
        jsr draw_string
        lda pm_level
        sta zpFSSize
        lda #0
        sta zpFSSize+1
        lda #(PM_HUD_COL+7)
        sta zpCol
        jsr draw_dec16

        ; status row 7
        lda #PM_HUD_COL
        sta zpCol
        lda #7
        sta zpRow
        lda #0
        sta zpInv
        lda pm_state
        beq pdh_play
        lda #<msg_pm_over
        sta zpPtr
        lda #>msg_pm_over
        sta zpPtr+1
        jmp draw_string
pdh_play:
        lda pm_fright
        beq pdh_blank
        lda #<msg_pm_power
        sta zpPtr
        lda #>msg_pm_power
        sta zpPtr+1
        jmp draw_string
pdh_blank:
        lda #<msg_pm_blank
        sta zpPtr
        lda #>msg_pm_blank
        sta zpPtr+1
        jmp draw_string

; pm_draw_score_only - refresh just the score value (steady-state).
pm_draw_score_only:
        lda #(PM_HUD_COL+7)
        sta zpFX
        lda #8
        sta zpFY
        lda #6
        sta zpFW
        lda #8
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        lda #PM_HUD_COL+7
        sta zpCol
        lda #1
        sta zpRow
        lda #0
        sta zpInv
        lda pm_score
        sta zpFSSize
        lda pm_score+1
        sta zpFSSize+1
        jmp draw_dec16

; pacman_draw - full redraw: title, separator, all maze tiles, actors, HUD,
; help line.
pacman_draw:
        ; title row
        lda #0
        sta zpFX
        lda #0
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #8
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        lda #<msg_pm_title
        sta zpPtr
        lda #>msg_pm_title
        sta zpPtr+1
        lda #1
        sta zpCol
        lda #0
        sta zpRow
        lda #0
        sta zpInv
        jsr draw_string
        ; separator
        lda #0
        sta zpFX
        lda #7
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #1
        sta zpFH
        lda #$7F
        sta zpFPat
        jsr fill_rows
        ; clear content area
        lda #0
        sta zpFX
        lda #APP_CONTENT_Y
        sta zpFY
        lda #SCRCOLS
        sta zpFW
        lda #(APP_CONTENT_H+8)
        sta zpFH
        lda #0
        sta zpFPat
        jsr fill_rows
        ; all maze tiles
        lda #0
        sta zpPMRow
pdr_row:
        lda #0
        sta zpPMCol
pdr_col:
        jsr pm_draw_cell
        inc zpPMCol
        lda zpPMCol
        cmp #PM_COLS
        bne pdr_col
        inc zpPMRow
        lda zpPMRow
        cmp #PM_ROWS
        bne pdr_row
        jsr pm_draw_actors
        jsr pm_draw_hud
        ; help line row23
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #<msg_pm_help
        sta zpPtr
        lda #>msg_pm_help
        sta zpPtr+1
        jmp draw_string

; ============================================================================
; tick / input / open / close
; ============================================================================

; pacman_tick - one game step per soft-clock second (called from ml when
; app_mode = 5). No-op when the game is over.
pacman_tick:
        lda pm_state
        beq pt_active           ; game over -> nothing to step
        rts
pt_active:
        ; step the game only every PM_STEP_DIV-th soft-clock second, so the
        ; board evolves at a readable pace (and `wait N` lands deterministic
        ; frames - the heavy full redraws on open/new/death/level need ~12
        ; ticks to complete, so a slow step rate keeps them from overlapping
        ; a screenshot).
        inc pm_stepctr
        lda pm_stepctr
        cmp #PM_STEP_DIV
        bcs pt_step
        rts
pt_step:
        lda #0
        sta pm_stepctr
        ; flip the half-speed sub-tick
        lda pm_subtick
        eor #1
        sta pm_subtick
        ; frightened countdown
        lda pm_fright
        beq pt_mode
        sec
        sbc #1
        sta pm_fright
        bne pt_mode
        ; fright ended: frightened ghosts resume normal
        ldx #0
pt_unfr:
        lda gh_mode,x
        cmp #GM_FRIGHT
        bne pt_unfrn
        lda #GM_NORMAL
        sta gh_mode,x
pt_unfrn:
        inx
        cpx #2
        bne pt_unfr
pt_mode:
        ; scatter/chase schedule
        inc pm_modet
        lda pm_mode
        bne pt_chasephase
        lda pm_modet
        cmp #PM_SCAT_T
        bcc pt_record
        jmp pt_modeswap
pt_chasephase:
        lda pm_modet
        cmp #PM_CHASE_T
        bcc pt_record
pt_modeswap:
        lda #0
        sta pm_modet
        lda pm_mode
        eor #1
        sta pm_mode
        ; reverse normal ghosts on a phase change
        ldx #0
pt_rev:
        lda gh_mode,x
        cmp #GM_NORMAL
        bne pt_revn
        lda gh_dir,x
        eor #2
        sta gh_dir,x
pt_revn:
        inx
        cpx #2
        bne pt_rev
pt_record:
        ; stash old positions for the dirty-tile refresh
        lda pac_col
        sta zpPMOldPC
        lda pac_row
        sta zpPMOldPR
        lda gh_col+0
        sta zpPMOldGC+0
        lda gh_row+0
        sta zpPMOldGR+0
        lda gh_col+1
        sta zpPMOldGC+1
        lda gh_row+1
        sta zpPMOldGR+1
        ; move pac
        jsr pm_move_pac
        bcc pt_ghosts
        ; level clear: reload + reset, keep score/lives, level++
        inc pm_level
        jsr pm_load_maze
        jsr pm_reset_actors
        jmp pacman_draw
pt_ghosts:
        lda #0
        sta zpPMGI
pt_gloop:
        jsr pm_move_ghost
        bcc pt_gnext
        ; pac died
        jsr pm_lose_life
        jmp pacman_draw
pt_gnext:
        inc zpPMGI
        lda zpPMGI
        cmp #2
        bne pt_gloop
        ; steady-state dirty redraw
        jsr pm_refresh
pt_done:
        rts

; pm_lose_life - lives--, game over at 0, else reset actors (maze kept).
pm_lose_life:
        dec pm_lives
        bne pll_alive
        lda #1
        sta pm_state            ; game over
        rts
pll_alive:
        jsr pm_reset_actors
        rts

; pm_refresh - redraw the maze cells under the old actor tiles, redraw the
; actors, refresh the score. (Dirty-cell rendering, HANDOFF-M3 SS1.)
pm_refresh:
        lda zpPMOldPC
        sta zpPMCol
        lda zpPMOldPR
        sta zpPMRow
        jsr pm_draw_cell
        lda zpPMOldGC+0
        sta zpPMCol
        lda zpPMOldGR+0
        sta zpPMRow
        jsr pm_draw_cell
        lda zpPMOldGC+1
        sta zpPMCol
        lda zpPMOldGR+1
        sta zpPMRow
        jsr pm_draw_cell
        jsr pm_draw_actors
        jsr pm_draw_score_only
        rts

; pacman_open - launch: new game, full-screen.
pacman_open:
        lda #5
        sta app_mode
        jsr beep_click
        jsr pm_newgame
        jmp pacman_draw

; pacman_close - Esc: back to the desktop (maze state lost).
pacman_close:
        lda #0
        sta app_mode
        jsr draw_desktop
        jsr draw_sysinfo_win
        jsr draw_clock_win
        jmp draw_icons

; pacman_key - route a key while Pac-Man is active (zpTmp = key code).
pacman_key:
        lda zpTmp
        cmp #$9B                ; ESC
        beq pacman_close
        cmp #$CE                ; 'N' new game
        beq pk_new
        lda pm_state
        bne pk_done             ; over: only Esc/N
        lda zpTmp
        cmp #$8B                ; up
        bne pk_chkleft
        lda #0
        jmp pk_setdir
pk_chkleft:
        cmp #$88                ; left
        bne pk_chkdown
        lda #1
        jmp pk_setdir
pk_chkdown:
        cmp #$8A                ; down
        bne pk_chkright
        lda #2
        jmp pk_setdir
pk_chkright:
        cmp #$95                ; right
        bne pk_done
        lda #3
pk_setdir:
        sta pac_nextdir
pk_done:
        rts
pk_new:
        jsr pm_newgame
        jmp pacman_draw

msg_pm_title: dc.b "Pac-Man",0
msg_pm_score: dc.b "Score:",0
msg_pm_lives: dc.b "Lives:",0
msg_pm_level: dc.b "Level:",0
msg_pm_over:  dc.b "GAME OVER",0
msg_pm_power: dc.b "POWER!   ",0
msg_pm_blank: dc.b "         ",0
msg_pm_help:  dc.b "Arrows steer  N=new  Esc=back",0
