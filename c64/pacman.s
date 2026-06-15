; ============================================================================
; UnoDOS/C64 Pac-Man - a DISK-LOADED app (not part of the kernel). Assembled
; at APP_BASE ($5000), loaded on demand by the kernel's launch_app, and linked
; to the kernel only by the API addresses in build/kernel_api.inc. This is how
; the mature UnoDOS ports ship their larger apps - off the disk, keeping the
; kernel lean.
;
; A 13x13 pillar maze (the shared mkmaze grid) rendered in colour: blue walls,
; white dots/pellets, a yellow Pac-Man and two chasing ghosts (red + pink).
; CRSR steers (queued turn at the next open tile); ghosts greedily minimise
; Manhattan distance to Pac-Man (no reverse) - the documented "simple chase"
; deviation from the arcade scatter/frightened AI. Eat all dots to win; a ghost
; on your tile is game over. N = new game, RUN/STOP = back to the desktop.
;
; App ABI: the first three words are JMPs (init / key / tick). The kernel calls
; init once (draws the maze), key on each keypress (key in zpTmp), tick every
; mainloop pass (we step the game every PM_RATE passes).
; ============================================================================

        processor 6502
        include "sys.inc"
        include "build/kernel_api.inc"

PM_COLS  equ 13
PM_ROWS  equ 13
PM_OX    equ 2          ; screen cell column of maze col 0
PM_OY    equ 1          ; screen cell row of maze row 0
PM_RATE  equ 120        ; mainloop passes per game step (harness-tuned)
PMMAZE   equ APP_BSS    ; $4C00: 169-byte mutable maze

; ---- app scratch zero page (sys.inc zpApp0..F = $67..$76) ----
zpMcol   equ zpApp0
zpMrow   equ zpApp1
zpNcol   equ zpApp2
zpNrow   equ zpApp3
zpGi     equ zpApp4     ; ghost loop index
zpBestDir equ zpApp5
zpBestD  equ zpApp6
zpDir    equ zpApp7
zpTmpA   equ zpApp8
zpTmpB   equ zpApp9

        org APP_BASE
        jmp pac_init
        jmp pac_key
        jmp pac_tick

; ---------------------------------------------------------------- pac_init
pac_init:
        jsr pm_load_maze
        jsr pm_reset
        lda #PM_RATE
        sta pm_ctr
        jmp pac_draw_all

; ---------------------------------------------------------------- pac_key
pac_key:
        lda zpTmp
        cmp #K_ESC
        bne pk_n1
        jmp return_to_desktop
pk_n1:
        cmp #$4E                ; 'N' new game
        bne pk_n2
        jmp pac_init
pk_n2:
        lda pm_state
        bne pk_done             ; win/lose: only N / STOP
        lda zpTmp
        cmp #K_UP
        bne pk_n3
        lda #0
        sta pac_ndir
        rts
pk_n3:
        cmp #K_LEFT
        bne pk_n4
        lda #1
        sta pac_ndir
        rts
pk_n4:
        cmp #K_DOWN
        bne pk_n5
        lda #2
        sta pac_ndir
        rts
pk_n5:
        cmp #K_RIGHT
        bne pk_done
        lda #3
        sta pac_ndir
pk_done:
        rts

; ---------------------------------------------------------------- pac_tick
pac_tick:
        lda pm_state
        bne pt_done             ; game over: freeze
        dec pm_ctr
        bne pt_done
        lda #PM_RATE
        sta pm_ctr
        jsr pm_step
pt_done:
        rts

; ============================================================================
; maze helpers
; ============================================================================

; pm_idx - zpMcol/zpMrow -> A = row*13+col, also X. (mul13 = r*8+r*4+r)
pm_idx:
        lda zpMrow
        asl
        asl
        sta zpTmpA              ; r*4
        asl                     ; r*8
        clc
        adc zpTmpA              ; r*12
        clc
        adc zpMrow              ; r*13
        clc
        adc zpMcol
        tax
        rts

; pm_walk - zpNcol/zpNrow -> A=1 if in-bounds and not a wall, else 0.
pm_walk:
        lda zpNcol
        cmp #PM_COLS
        bcs pw_no
        lda zpNrow
        cmp #PM_ROWS
        bcs pw_no
        lda zpNrow
        asl
        asl
        sta zpTmpA
        asl
        clc
        adc zpTmpA
        clc
        adc zpNrow
        clc
        adc zpNcol
        tax
        lda PMMAZE,x
        cmp #1
        beq pw_no
        lda #1
        rts
pw_no:
        lda #0
        rts

; pm_draw_maze_cell - draw the maze tile at zpMcol/zpMrow (wall/dot/power/floor)
; at screen cell (PM_OX+col, PM_OY+row).
pm_draw_maze_cell:
        jsr pm_idx              ; X = idx
        lda PMMAZE,x            ; cell value
        sta zpTmpB              ; save cell value
        asl
        tay                     ; cell*2 -> sprite table index
        lda pm_spr_lo,y
        sta zpFontPtr
        lda pm_spr_hi,y
        sta zpFontPtr+1
        ldx zpTmpB              ; cell value -> colour
        lda pm_cellcol,x
        sta zpFCol
        jmp pm_blit_at

; pm_draw_sprite - blit zpFontPtr sprite (colour zpFCol) at maze tile
; zpMcol/zpMrow. (actors)
pm_blit_at:
        lda #0
        sta zpInv
        lda zpMcol
        clc
        adc #PM_OX
        sta zpCol
        lda zpMrow
        clc
        adc #PM_OY
        sta zpRow
        jmp blit_cell

; ============================================================================
; game logic
; ============================================================================

pm_load_maze:
        lda #0
        sta pm_dots
        sta pm_dots+1
        ldx #0
plm:
        lda pm_maze_tpl,x
        sta PMMAZE,x
        cmp #2
        bcc plm_n               ; 0/1 not collectable
        inc pm_dots
        bne plm_n
        inc pm_dots+1
plm_n:
        inx
        cpx #(PM_COLS*PM_ROWS)
        bne plm
        rts

pm_reset:
        lda #6
        sta pac_col
        lda #11
        sta pac_row
        lda #1
        sta pac_dir
        sta pac_ndir
        ; clear the dot under pac's spawn
        lda #6
        sta zpMcol
        lda #11
        sta zpMrow
        jsr pm_idx
        lda PMMAZE,x
        cmp #2
        bcc pr_g
        lda #0
        sta PMMAZE,x
        lda pm_dots
        sec
        sbc #1
        sta pm_dots
        lda pm_dots+1
        sbc #0
        sta pm_dots+1
pr_g:
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
        sta pm_score+1
        sta pm_state
        rts

; pm_step - one game step: move pac, eat, move ghosts, check collisions/win.
pm_step:
        ; --- pac: adopt queued turn if open ---
        lda pac_col
        clc
        adc pm_dx
        ; (use queued dir) - compute candidate from pac_ndir
        ldx pac_ndir
        lda pac_col
        clc
        adc pm_dx,x
        sta zpNcol
        lda pac_row
        clc
        adc pm_dy,x
        sta zpNrow
        jsr pm_walk
        beq ps_keepdir
        lda pac_ndir
        sta pac_dir
ps_keepdir:
        ; redraw pac's current tile from maze (it has been eaten = floor)
        lda pac_col
        sta zpMcol
        lda pac_row
        sta zpMrow
        jsr pm_draw_maze_cell
        ; move pac in pac_dir if open
        ldx pac_dir
        lda pac_col
        clc
        adc pm_dx,x
        sta zpNcol
        lda pac_row
        clc
        adc pm_dy,x
        sta zpNrow
        jsr pm_walk
        beq ps_nomove
        lda zpNcol
        sta pac_col
        lda zpNrow
        sta pac_row
ps_nomove:
        ; eat at the new tile
        jsr pm_eat
        ; --- ghosts ---
        lda #0
        sta zpGi
psg_loop:
        ; redraw the ghost's current tile from maze
        ldx zpGi
        lda gh_col,x
        sta zpMcol
        lda gh_row,x
        sta zpMrow
        jsr pm_draw_maze_cell
        ; choose + take a step
        jsr pm_ghost_steer      ; sets gh_dir[zpGi] toward pac
        ldx zpGi
        lda gh_dir,x
        tay
        lda gh_col,x
        clc
        adc pm_dx,y
        sta zpNcol
        lda gh_row,x
        clc
        adc pm_dy,y
        sta zpNrow
        jsr pm_walk
        beq psg_next            ; blocked: stay (rare; chooser avoids it)
        ldx zpGi
        lda zpNcol
        sta gh_col,x
        lda zpNrow
        sta gh_row,x
psg_next:
        inc zpGi
        lda zpGi
        cmp #2
        bne psg_loop
        ; --- redraw actors ---
        jsr pm_draw_actors
        ; --- collisions ---
        jsr pm_check_hit
        ; win?
        lda pm_dots
        ora pm_dots+1
        bne ps_drawhud
        lda #1
        sta pm_state            ; win
ps_drawhud:
        jmp pac_draw_hud

; pm_eat - if pac's tile holds a dot/pellet: score it, clear it, dec pm_dots.
pm_eat:
        lda pac_col
        sta zpMcol
        lda pac_row
        sta zpMrow
        jsr pm_idx
        lda PMMAZE,x
        cmp #2
        bcc pe_done             ; floor/wall
        ; score: dot(2)=10, power(3)=50
        cmp #3
        beq pe_power
        lda #10
        jmp pe_add
pe_power:
        lda #50
pe_add:
        clc
        adc pm_score
        sta pm_score
        bcc pe_noc
        inc pm_score+1
pe_noc:
        jsr pm_idx
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

; pm_ghost_steer - choose gh_dir[zpGi]: the open, non-reverse direction whose
; resulting tile minimises Manhattan distance to Pac-Man.
pm_ghost_steer:
        lda #$FF
        sta zpBestD
        sta zpBestDir
        lda #0
        sta zpDir
pgs_loop:
        ; skip the reverse of the current direction
        ldx zpGi
        lda gh_dir,x
        clc
        adc #2
        and #3
        cmp zpDir
        beq pgs_next
        ; candidate tile
        ldx zpGi
        ldy zpDir
        lda gh_col,x
        clc
        adc pm_dx,y
        sta zpNcol
        lda gh_row,x
        clc
        adc pm_dy,y
        sta zpNrow
        jsr pm_walk
        beq pgs_next
        ; dist = |ncol-pac_col| + |nrow-pac_row|
        lda zpNcol
        sec
        sbc pac_col
        bpl pgs_dx
        eor #$FF
        clc
        adc #1
pgs_dx:
        sta zpTmpB
        lda zpNrow
        sec
        sbc pac_row
        bpl pgs_dy
        eor #$FF
        clc
        adc #1
pgs_dy:
        clc
        adc zpTmpB              ; A = manhattan distance
        cmp zpBestD
        bcs pgs_next            ; not better
        sta zpBestD
        lda zpDir
        sta zpBestDir
pgs_next:
        inc zpDir
        lda zpDir
        cmp #4
        bne pgs_loop
        ; commit (if some dir was found)
        lda zpBestDir
        cmp #$FF
        beq pgs_done
        ldx zpGi
        sta gh_dir,x
pgs_done:
        rts

; pm_check_hit - if a ghost shares pac's tile, game over (lose).
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
        sta pm_state            ; lose
        rts
pch_next:
        inx
        cpx #2
        bne pch_loop
        rts

; ============================================================================
; drawing
; ============================================================================
pac_draw_all:
        jsr app_clear
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
        ; maze cells
        lda #0
        sta zpMrow
pda_rl:
        lda #0
        sta zpMcol
pda_cl:
        jsr pm_draw_maze_cell
        inc zpMcol
        lda zpMcol
        cmp #PM_COLS
        bne pda_cl
        inc zpMrow
        lda zpMrow
        cmp #PM_ROWS
        bne pda_rl
        jsr pm_draw_actors
        ; help line
        lda #1
        sta zpCol
        lda #23
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda #<msg_pm_help
        sta zpPtr
        lda #>msg_pm_help
        sta zpPtr+1
        jsr draw_string
        jmp pac_draw_hud

pm_draw_actors:
        ; pac (yellow)
        lda #<spr_pac
        sta zpFontPtr
        lda #>spr_pac
        sta zpFontPtr+1
        lda #$70
        sta zpFCol
        lda pac_col
        sta zpMcol
        lda pac_row
        sta zpMrow
        jsr pm_blit_at
        ; ghosts
        lda #<spr_ghost
        sta zpFontPtr
        lda #>spr_ghost
        sta zpFontPtr+1
        lda #$20                ; red
        sta zpFCol
        lda gh_col+0
        sta zpMcol
        lda gh_row+0
        sta zpMrow
        jsr pm_blit_at
        lda #<spr_ghost
        sta zpFontPtr
        lda #>spr_ghost
        sta zpFontPtr+1
        lda #$A0                ; light-red / pink
        sta zpFCol
        lda gh_col+1
        sta zpMcol
        lda gh_row+1
        sta zpMrow
        jmp pm_blit_at

pac_draw_hud:
        lda #COL_WIN
        sta zpFCol
        lda #17
        sta zpCol
        lda #2
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
        lda #24
        sta zpCol
        jsr draw_dec16
        lda #17
        sta zpCol
        lda #3
        sta zpRow
        lda #<msg_pm_dots
        sta zpPtr
        lda #>msg_pm_dots
        sta zpPtr+1
        jsr draw_string
        lda pm_dots
        sta zpFSSize
        lda pm_dots+1
        sta zpFSSize+1
        lda #24
        sta zpCol
        jsr draw_dec16
        ; status row 5
        lda #17
        sta zpCol
        lda #5
        sta zpRow
        lda #0
        sta zpInv
        lda #COL_WIN
        sta zpFCol
        lda pm_state
        beq pdh_blank
        cmp #1
        beq pdh_win
        lda #<msg_pm_lose
        sta zpPtr
        lda #>msg_pm_lose
        sta zpPtr+1
        jmp draw_string
pdh_win:
        lda #<msg_pm_win
        sta zpPtr
        lda #>msg_pm_win
        sta zpPtr+1
        jmp draw_string
pdh_blank:
        lda #<msg_pm_blank
        sta zpPtr
        lda #>msg_pm_blank
        sta zpPtr+1
        jmp draw_string

; ============================================================================
; data
; ============================================================================
pm_dx: dc.b 0,$FF,0,1          ; up,left,down,right
pm_dy: dc.b $FF,0,1,0

; cell value -> sprite (0 floor,1 wall,2 dot,3 power) and colour
pm_spr_lo:  dc.b <spr_floor,<spr_wall,<spr_dot,<spr_power
pm_spr_hi:  dc.b >spr_floor,>spr_wall,>spr_dot,>spr_power
pm_cellcol: dc.b $01,$66,$11,$11   ; floor wht/wht, wall blue/blue, dot wht, power wht

spr_floor: dc.b $00,$00,$00,$00,$00,$00,$00,$00
spr_wall:  dc.b $FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF
spr_dot:   dc.b $00,$00,$00,$18,$18,$00,$00,$00
spr_power: dc.b $00,$3C,$7E,$7E,$7E,$7E,$3C,$00
spr_pac:   dc.b $00,$3C,$7E,$FC,$FC,$7E,$3C,$00
spr_ghost: dc.b $00,$3C,$7E,$DB,$FF,$FF,$DB,$00

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

msg_pm_title: dc.b "Pac-Man",0
msg_pm_score: dc.b "Score:",0
msg_pm_dots:  dc.b "Dots:",0
msg_pm_win:   dc.b "YOU WIN!",0
msg_pm_lose:  dc.b "GAME OVER",0
msg_pm_blank: dc.b "         ",0
msg_pm_help:  dc.b "CRSR steer   N=new   STOP=back",0

; ---- mutable state (reloaded to these values on each launch) ----
pac_col:  dc.b 6
pac_row:  dc.b 11
pac_dir:  dc.b 1
pac_ndir: dc.b 1
gh_col:   dc.b 5,7
gh_row:   dc.b 6,6
gh_dir:   dc.b 3,1
pm_score: dc.w 0
pm_dots:  dc.w 0
pm_state: dc.b 0       ; 0 play, 1 win, 2 lose
pm_ctr:   dc.b PM_RATE
