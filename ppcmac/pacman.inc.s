# ============================================================================
# UnoDOS / PowerPC Mac — Pac-Man (donor genesis/pacman.i, tile-stepped).
# 28x25 maze, one 16px framebuffer cell per maze tile. The donor AI is kept:
# Blinky targets pac, Pinky 4 tiles ahead, Clyde goes home when close, and a
# fourth ghost reflects Blinky through pac ("Inky-lite"); scatter/chase runs on
# the classic alternating schedule, power pellets turn the pack frightened
# (random walk, half speed, eat chain 200/400/800/1600), eaten ghosts run home
# as eyes. Actors step one tile per game step (every 6 frames); rendering is
# cell-grid: erase = repaint the maze tile, then repaint the actors.
#
#   d-pad  steer (queued turn)     A  new game (title/over)     B  back
# ============================================================================

.equ PMC,    28
.equ PMR,    25
.equ PMX0,   16
.equ PMY0,   48
.equ PMS_READY, 1
.equ PMS_PLAY,  2
.equ PMS_OVER,  4
.equ GH_HOUSE,  0
.equ GH_SCAT,   1
.equ GH_CHASE,  2
.equ GH_FRIGHT, 3
.equ GH_EATEN,  4
.equ PM_STEPT,  6               # frames per game step
.equ PM_FRSTEPS, 60             # frightened duration (steps)
.equ PM_READYT, 40              # READY overlay (frames)

# pm_walkable: r3=tx r4=ty r5=ghost? r6=eaten? -> r3 = 1 ok / 0 blocked. Leaf.
pm_walkable:
    cmpwi r4, 0
    blt   pw_no
    cmpwi r4, PMR
    bge   pw_no
    cmpwi r3, 0
    bge   pw_1
    addi  r3, r3, PMC
pw_1:
    cmpwi r3, PMC
    blt   pw_2
    subi  r3, r3, PMC
pw_2:
    mulli r7, r4, PMC
    add   r7, r7, r3
    LA    r8, pm_maze
    lbzx  r7, r8, r7
    cmpwi r7, 1
    beq   pw_no
    cmpwi r7, 4
    beq   pw_g
    cmpwi r7, 5
    beq   pw_g
    li    r3, 1
    blr
pw_g:
    cmpwi r5, 0
    beq   pw_no
    cmpwi r6, 0
    beq   pw_no
    li    r3, 1
    blr
pw_no:
    li    r3, 0
    blr

# pm_mode_state: -> r3 = GH_SCAT or GH_CHASE from the schedule parity. Leaf.
pm_mode_state:
    LWZA  r3, pm_mode
    andi. r3, r3, 1
    beq   pms_s
    li    r3, GH_CHASE
    blr
pms_s:
    li    r3, GH_SCAT
    blr

# pm_load_maze: template -> live maze, count the dots. Leaf (loop only).
pm_load_maze:
    LA    r3, pm_maze_tpl
    LA    r4, pm_maze
    li    r5, (PMC*PMR)
    li    r6, 0
    mtctr r5
plm_c:
    lbz   r7, 0(r3)
    stb   r7, 0(r4)
    cmpwi r7, 2
    beq   plm_d
    cmpwi r7, 3
    bne   plm_n
plm_d:
    addi  r6, r6, 1
plm_n:
    addi  r3, r3, 1
    addi  r4, r4, 1
    bdnz  plm_c
    STWA  r6, pm_dots, r3
    blr

# pm_reset_actors: pac + 4 ghosts to spawn poses. Leaf (loop only).
pm_reset_actors:
    li    r3, 14
    STWA  r3, pm_px, r4
    STWA  r3, pm_opx, r4
    li    r3, 19
    STWA  r3, pm_py, r4
    STWA  r3, pm_opy, r4
    li    r3, 1
    STWA  r3, pm_dir, r4
    STWA  r3, pm_ndir, r4
    LA    r3, pm_gh_init
    LA    r4, pm_gh
    li    r5, 20                          # 4 ghosts x 5 words
    mtctr r5
pra_c:
    lwz   r6, 0(r3)
    stw   r6, 0(r4)
    addi  r3, r3, 4
    addi  r4, r4, 4
    bdnz  pra_c
    LA    r3, pm_gh                       # olds = current
    LA    r4, pm_ogh
    li    r5, 4
pra_o:
    lwz   r6, 0(r3)
    stw   r6, 0(r4)
    lwz   r6, 4(r3)
    stw   r6, 4(r4)
    addi  r3, r3, 20
    addi  r4, r4, 8
    subic. r5, r5, 1
    bne   pra_o
    li    r3, 0
    STWA  r3, pm_fright, r4
    STWA  r3, pm_kills, r4
    STWA  r3, pm_fpar, r4
    blr

# pacman_init / pm_new_game
pacman_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 0
    STWA  r3, pm_score, r4
    STWA  r3, pm_mode, r4
    STWA  r3, pm_modet, r4
    STWA  r3, pm_ctr, r4
    STWA  r3, pm_pf, r4
    li    r3, 3
    STWA  r3, pm_lives, r4
    bl    pm_load_maze
    bl    pm_reset_actors
    li    r3, PMS_READY
    STWA  r3, pm_state, r4
    li    r3, PM_READYT
    STWA  r3, pm_stt, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# pm_kill: pac dies — lose a life or game over.
pm_kill:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, pm_lives
    subi  r3, r3, 1
    STWA  r3, pm_lives, r4
    cmpwi r3, 0
    bgt   pk_alive
    li    r3, PMS_OVER
    STWA  r3, pm_state, r4
    LWZA  r3, pm_score
    LWZA  r4, pm_hi
    cmpw  r3, r4
    blt   pk_dirty
    STWA  r3, pm_hi, r4
    b     pk_dirty
pk_alive:
    bl    pm_reset_actors
    li    r3, PMS_READY
    STWA  r3, pm_state, r4
    li    r3, PM_READYT
    STWA  r3, pm_stt, r4
pk_dirty:
    li    r3, 1
    STWA  r3, v_dirty, r4
    li    r3, 0
    STWA  r3, pm_pf, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ============================================================================
# pm_steer: r3 = ghost index — pick a direction at the current tile.
# ============================================================================
pm_steer:
    mflr  r0
    stwu  r1, -64(r1)
    stw   r0, 68(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    stw   r18, 24(r1)
    stw   r19, 28(r1)
    stw   r20, 32(r1)
    stw   r21, 36(r1)
    stw   r22, 40(r1)
    mr    r21, r3                         # idx
    mulli r4, r3, 20
    LA    r14, pm_gh
    add   r14, r14, r4                    # ghost ptr
    lwz   r15, 0(r14)                     # gtx
    lwz   r16, 4(r14)                     # gty
    lwz   r3, 12(r14)
    cmpwi r3, GH_FRIGHT
    bne   ps_target
    # frightened: random non-reverse walkable direction (8 tries)
    li    r19, 8
ps_rt:
    bl    rng
    andi. r3, r3, 3
    lwz   r4, 8(r14)
    xori  r4, r4, 2
    cmpw  r3, r4
    beq   ps_rn
    mr    r20, r3
    slwi  r4, r3, 2
    LA    r5, pm_dx
    lwzx  r6, r5, r4
    add   r3, r6, r15
    LA    r5, pm_dy
    lwzx  r7, r5, r4
    add   r4, r7, r16
    li    r5, 1
    li    r6, 0
    bl    pm_walkable
    cmpwi r3, 0
    beq   ps_rn
    stw   r20, 8(r14)
    b     ps_out
ps_rn:
    subic. r19, r19, 1
    bne   ps_rt
    b     ps_out                          # keep the old direction
ps_target:
    lwz   r3, 12(r14)
    cmpwi r3, GH_EATEN
    bne   ps_nte
    li    r17, 14                         # eyes head for the door
    li    r18, 10
    b     ps_argmin
ps_nte:
    cmpwi r3, GH_CHASE
    bne   ps_scat
    LWZA  r5, pm_px
    LWZA  r6, pm_py
    cmpwi r21, 0
    bne   ps_c1
    mr    r17, r5                         # Blinky: pac itself
    mr    r18, r6
    b     ps_argmin
ps_c1:
    cmpwi r21, 1
    bne   ps_c2
    LWZA  r3, pm_dir                      # Pinky: 4 tiles ahead of pac
    slwi  r3, r3, 2
    LA    r4, pm_dx
    lwzx  r7, r4, r3
    slwi  r7, r7, 2
    add   r17, r5, r7
    LA    r4, pm_dy
    lwzx  r7, r4, r3
    slwi  r7, r7, 2
    add   r18, r6, r7
    b     ps_argmin
ps_c2:
    cmpwi r21, 2
    bne   ps_c3
    subf  r3, r5, r15                     # Clyde: near -> corner, far -> pac
    srawi r4, r3, 31
    xor   r3, r3, r4
    subf  r3, r4, r3
    subf  r7, r6, r16
    srawi r4, r7, 31
    xor   r7, r7, r4
    subf  r7, r4, r7
    add   r3, r3, r7
    cmpwi r3, 8
    bgt   ps_cpac
    li    r17, 1
    li    r18, 23
    b     ps_argmin
ps_cpac:
    mr    r17, r5
    mr    r18, r6
    b     ps_argmin
ps_c3:
    LA    r3, pm_gh                       # Inky-lite: reflect ghost0 through pac
    lwz   r4, 0(r3)
    slwi  r7, r5, 1
    subf  r17, r4, r7
    lwz   r4, 4(r3)
    slwi  r7, r6, 1
    subf  r18, r4, r7
    b     ps_argmin
ps_scat:
    slwi  r3, r21, 3
    LA    r4, pm_corners
    lwzx  r17, r4, r3
    addi  r3, r3, 4
    lwzx  r18, r4, r3
ps_argmin:
    li    r19, -1                         # best dir
    li    r20, 0x7FFF                     # best dist
    li    r22, 0                          # dir loop
ps_al:
    lwz   r3, 8(r14)
    xori  r3, r3, 2
    cmpw  r3, r22
    beq   ps_an                           # never reverse
    slwi  r4, r22, 2
    LA    r5, pm_dx
    lwzx  r6, r5, r4
    add   r3, r6, r15                     # nx
    LA    r5, pm_dy
    lwzx  r7, r5, r4
    add   r4, r7, r16                     # ny
    stw   r3, 44(r1)
    stw   r4, 48(r1)
    li    r5, 1
    lwz   r6, 12(r14)
    cmpwi r6, GH_EATEN
    li    r6, 0
    bne   ps_we
    li    r6, 1
ps_we:
    bl    pm_walkable
    cmpwi r3, 0
    beq   ps_an
    lwz   r3, 44(r1)
    lwz   r4, 48(r1)
    cmpwi r3, 0
    bge   ps_w1
    addi  r3, r3, PMC
ps_w1:
    cmpwi r3, PMC
    blt   ps_w2
    subi  r3, r3, PMC
ps_w2:
    subf  r3, r17, r3
    srawi r5, r3, 31
    xor   r3, r3, r5
    subf  r3, r5, r3
    subf  r4, r18, r4
    srawi r5, r4, 31
    xor   r4, r4, r5
    subf  r4, r5, r4
    add   r3, r3, r4                      # manhattan to target
    cmpw  r3, r20
    bge   ps_an
    mr    r20, r3
    mr    r19, r22
ps_an:
    addi  r22, r22, 1
    cmpwi r22, 4
    bne   ps_al
    cmpwi r19, -1
    beq   ps_out
    stw   r19, 8(r14)
ps_out:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r18, 24(r1)
    lwz   r19, 28(r1)
    lwz   r20, 32(r1)
    lwz   r21, 36(r1)
    lwz   r22, 40(r1)
    lwz   r0, 68(r1)
    addi  r1, r1, 64
    mtlr  r0
    blr

# ============================================================================
# pm_step: one game step (pac + all ghosts move one tile)
# ============================================================================
pm_step:
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    # remember old poses (for the partial erase + swap collision)
    LWZA  r3, pm_px
    STWA  r3, pm_opx, r4
    LWZA  r3, pm_py
    STWA  r3, pm_opy, r4
    LA    r5, pm_gh
    LA    r6, pm_ogh
    li    r7, 4
pst_o:
    lwz   r3, 0(r5)
    stw   r3, 0(r6)
    lwz   r3, 4(r5)
    stw   r3, 4(r6)
    addi  r5, r5, 20
    addi  r6, r6, 8
    subic. r7, r7, 1
    bne   pst_o
    LWZA  r3, pm_fpar
    xori  r3, r3, 1
    STWA  r3, pm_fpar, r4
    # fright countdown / mode schedule
    LWZA  r3, pm_fright
    cmpwi r3, 0
    beq   pst_mode
    subi  r3, r3, 1
    STWA  r3, pm_fright, r4
    cmpwi r3, 0
    bne   pst_pac
    LA    r14, pm_gh                      # fright over: back on schedule
    li    r15, 4
pst_fx:
    lwz   r3, 12(r14)
    cmpwi r3, GH_FRIGHT
    bne   pst_fxn
    bl    pm_mode_state
    stw   r3, 12(r14)
pst_fxn:
    addi  r14, r14, 20
    subic. r15, r15, 1
    bne   pst_fx
    b     pst_pac
pst_mode:
    LWZA  r3, pm_modet
    addi  r3, r3, 1
    STWA  r3, pm_modet, r4
    LWZA  r4, pm_mode
    cmpwi r4, 7
    ble   pst_mok
    li    r4, 7
pst_mok:
    slwi  r4, r4, 2
    LA    r5, pm_modedur
    lwzx  r5, r5, r4
    cmpw  r3, r5
    blt   pst_pac
    li    r3, 0
    STWA  r3, pm_modet, r4
    LWZA  r3, pm_mode
    cmpwi r3, 7
    bge   pst_mrev
    addi  r3, r3, 1
    STWA  r3, pm_mode, r4
pst_mrev:
    LA    r14, pm_gh                      # schedule flip: new state + reverse
    li    r15, 4
pst_mg:
    lwz   r3, 12(r14)
    cmpwi r3, GH_SCAT
    beq   pst_mgs
    cmpwi r3, GH_CHASE
    bne   pst_mgn
pst_mgs:
    bl    pm_mode_state
    stw   r3, 12(r14)
    lwz   r3, 8(r14)
    xori  r3, r3, 2
    stw   r3, 8(r14)
pst_mgn:
    addi  r14, r14, 20
    subic. r15, r15, 1
    bne   pst_mg
pst_pac:
    # ---- pac: eat, turn, move ------------------------------------------------
    LWZA  r14, pm_px
    LWZA  r15, pm_py
    mulli r3, r15, PMC
    add   r3, r3, r14
    LA    r4, pm_maze
    lbzx  r5, r4, r3
    cmpwi r5, 2
    bne   pst_pw
    li    r5, 0
    stbx  r5, r4, r3
    LWZA  r3, pm_score
    addi  r3, r3, 10
    STWA  r3, pm_score, r4
    LWZA  r3, pm_dots
    subi  r3, r3, 1
    STWA  r3, pm_dots, r4
    b     pst_lvl
pst_pw:
    cmpwi r5, 3
    bne   pst_lvl
    li    r5, 0
    stbx  r5, r4, r3
    LWZA  r3, pm_score
    addi  r3, r3, 50
    STWA  r3, pm_score, r4
    LWZA  r3, pm_dots
    subi  r3, r3, 1
    STWA  r3, pm_dots, r4
    li    r3, PM_FRSTEPS
    STWA  r3, pm_fright, r4
    li    r3, 0
    STWA  r3, pm_kills, r4
    LA    r16, pm_gh                      # pack goes frightened + reverses
    li    r17, 4
pst_fg:
    lwz   r3, 12(r16)
    cmpwi r3, GH_SCAT
    beq   pst_fgs
    cmpwi r3, GH_CHASE
    bne   pst_fgn
pst_fgs:
    li    r3, GH_FRIGHT
    stw   r3, 12(r16)
    lwz   r3, 8(r16)
    xori  r3, r3, 2
    stw   r3, 8(r16)
pst_fgn:
    addi  r16, r16, 20
    subic. r17, r17, 1
    bne   pst_fg
pst_lvl:
    LWZA  r3, pm_dots
    cmpwi r3, 0
    bne   pst_turn
    bl    pm_load_maze                    # level clear
    bl    pm_reset_actors
    li    r3, PMS_READY
    STWA  r3, pm_state, r4
    li    r3, PM_READYT
    STWA  r3, pm_stt, r4
    li    r3, 1
    STWA  r3, v_dirty, r4
    li    r3, 0
    STWA  r3, pm_pf, r4
    b     pst_out
pst_turn:
    LWZA  r3, pm_ndir                     # queued turn if now possible
    slwi  r3, r3, 2
    LA    r4, pm_dx
    lwzx  r5, r4, r3
    add   r5, r5, r14
    LA    r4, pm_dy
    lwzx  r6, r4, r3
    add   r6, r6, r15
    mr    r3, r5
    mr    r4, r6
    li    r5, 0
    li    r6, 0
    bl    pm_walkable
    cmpwi r3, 0
    beq   pst_keep
    LWZA  r3, pm_ndir
    STWA  r3, pm_dir, r4
pst_keep:
    LWZA  r3, pm_dir
    slwi  r3, r3, 2
    LA    r4, pm_dx
    lwzx  r5, r4, r3
    add   r5, r5, r14
    LA    r4, pm_dy
    lwzx  r6, r4, r3
    add   r6, r6, r15
    stw   r5, 28(r1)
    stw   r6, 32(r1)
    mr    r3, r5
    mr    r4, r6
    li    r5, 0
    li    r6, 0
    bl    pm_walkable
    cmpwi r3, 0
    beq   pst_gh                          # blocked: stay
    lwz   r3, 28(r1)
    lwz   r4, 32(r1)
    cmpwi r3, 0                           # tunnel wrap
    bge   pst_w1
    li    r3, (PMC-1)
pst_w1:
    cmpwi r3, PMC
    blt   pst_w2
    li    r3, 0
pst_w2:
    STWA  r3, pm_px, r5
    STWA  r4, pm_py, r5
pst_gh:
    # ---- ghosts --------------------------------------------------------------
    LA    r14, pm_gh
    LA    r16, pm_ogh
    li    r15, 0
pst_gl:
    lwz   r3, 12(r14)
    cmpwi r3, GH_HOUSE
    bne   pst_ga
    lwz   r3, 16(r14)
    subi  r3, r3, 1
    stw   r3, 16(r14)
    cmpwi r3, 0
    bgt   pst_gn                          # still housed
    li    r3, 14                          # exit to the door tile
    stw   r3, 0(r14)
    li    r3, 10
    stw   r3, 4(r14)
    li    r3, 1
    stw   r3, 8(r14)
    bl    pm_mode_state
    stw   r3, 12(r14)
    b     pst_gn
pst_ga:
    lwz   r3, 12(r14)
    cmpwi r3, GH_FRIGHT                   # frightened: half speed
    bne   pst_gs
    LWZA  r3, pm_fpar
    cmpwi r3, 0
    beq   pst_gcoll
pst_gs:
    mr    r3, r15
    bl    pm_steer
    lwz   r3, 8(r14)
    slwi  r3, r3, 2
    LA    r4, pm_dx
    lwzx  r5, r4, r3
    lwz   r6, 0(r14)
    add   r5, r5, r6
    LA    r4, pm_dy
    lwzx  r6, r4, r3
    lwz   r7, 4(r14)
    add   r6, r6, r7
    stw   r5, 28(r1)
    stw   r6, 32(r1)
    mr    r3, r5
    mr    r4, r6
    li    r5, 1
    lwz   r6, 12(r14)
    cmpwi r6, GH_EATEN
    li    r6, 0
    bne   pst_gw
    li    r6, 1
pst_gw:
    bl    pm_walkable
    cmpwi r3, 0
    beq   pst_ghome
    lwz   r3, 28(r1)
    lwz   r4, 32(r1)
    cmpwi r3, 0
    bge   pst_gw1
    li    r3, (PMC-1)
pst_gw1:
    cmpwi r3, PMC
    blt   pst_gw2
    li    r3, 0
pst_gw2:
    stw   r3, 0(r14)
    stw   r4, 4(r14)
pst_ghome:
    lwz   r3, 12(r14)                     # eyes arriving home resume
    cmpwi r3, GH_EATEN
    bne   pst_gcoll
    lwz   r3, 0(r14)
    cmpwi r3, 14
    bne   pst_gcoll
    lwz   r3, 4(r14)
    cmpwi r3, 10
    bne   pst_gcoll
    bl    pm_mode_state
    stw   r3, 12(r14)
pst_gcoll:
    lwz   r3, 0(r14)                      # same tile as pac?
    LWZA  r4, pm_px
    cmpw  r3, r4
    bne   pst_swap
    lwz   r3, 4(r14)
    LWZA  r4, pm_py
    cmpw  r3, r4
    beq   pst_hit
pst_swap:
    lwz   r3, 0(r14)                      # or swapped tiles this step?
    LWZA  r4, pm_opx
    cmpw  r3, r4
    bne   pst_gn
    lwz   r3, 4(r14)
    LWZA  r4, pm_opy
    cmpw  r3, r4
    bne   pst_gn
    lwz   r3, 0(r16)
    LWZA  r4, pm_px
    cmpw  r3, r4
    bne   pst_gn
    lwz   r3, 4(r16)
    LWZA  r4, pm_py
    cmpw  r3, r4
    bne   pst_gn
pst_hit:
    lwz   r3, 12(r14)
    cmpwi r3, GH_FRIGHT
    bne   pst_hkill
    li    r3, GH_EATEN                    # eat the ghost: 200 << kills
    stw   r3, 12(r14)
    li    r3, 200
    LWZA  r4, pm_kills
    slw   r3, r3, r4
    LWZA  r5, pm_score
    add   r3, r3, r5
    STWA  r3, pm_score, r5
    LWZA  r3, pm_kills
    cmpwi r3, 3
    bge   pst_gn
    addi  r3, r3, 1
    STWA  r3, pm_kills, r4
    b     pst_gn
pst_hkill:
    cmpwi r3, GH_EATEN
    beq   pst_gn
    bl    pm_kill
    b     pst_out
pst_gn:
    addi  r14, r14, 20
    addi  r16, r16, 8
    addi  r15, r15, 1
    cmpwi r15, 4
    bne   pst_gl
pst_out:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

# pacman_update: one frame — input + state machine.
pacman_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_pade
    andi. r0, r3, PAD_U
    beq   pu_1
    li    r3, 0
    STWA  r3, pm_ndir, r4
pu_1:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_L
    beq   pu_2
    li    r3, 1
    STWA  r3, pm_ndir, r4
pu_2:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_D
    beq   pu_3
    li    r3, 2
    STWA  r3, pm_ndir, r4
pu_3:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_R
    beq   pu_4
    li    r3, 3
    STWA  r3, pm_ndir, r4
pu_4:
    LWZA  r3, pm_state
    cmpwi r3, PMS_READY
    bne   pu_play
    LWZA  r3, pm_stt
    subi  r3, r3, 1
    STWA  r3, pm_stt, r4
    cmpwi r3, 0
    bne   pu_ret
    li    r3, PMS_PLAY
    STWA  r3, pm_state, r4
    li    r3, 1
    STWA  r3, v_dirty, r4                 # clear the READY overlay
    b     pu_ret
pu_play:
    cmpwi r3, PMS_PLAY
    bne   pu_over
    LWZA  r3, pm_ctr
    addi  r3, r3, 1
    STWA  r3, pm_ctr, r4
    cmpwi r3, PM_STEPT
    blt   pu_ret
    li    r3, 0
    STWA  r3, pm_ctr, r4
    bl    pm_step
    LWZA  r3, pm_state
    cmpwi r3, PMS_PLAY
    bne   pu_ret
    li    r3, 1
    STWA  r3, pm_pf, r4
    b     pu_ret
pu_over:
    cmpwi r3, PMS_OVER
    bne   pu_ret
    LWZA  r3, v_pade
    andi. r0, r3, PAD_A
    beq   pu_ret
    bl    pacman_init
    li    r3, 1
    STWA  r3, v_dirty, r4
pu_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ============================================================================
# rendering
# ============================================================================
# pm_draw_tile: r3 = tx, r4 = ty — one maze cell (16x16).
pm_draw_tile:
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    mulli r16, r4, PMC
    add   r16, r16, r3
    LA    r5, pm_maze
    lbzx  r16, r5, r16                    # tile value
    slwi  r14, r3, 4
    addi  r14, r14, PMX0                  # px
    slwi  r15, r4, 4
    addi  r15, r15, PMY0                  # py
    cmpwi r16, 1
    bne   pdt_e
    li    r3, 2                           # wall (cyan)
    bl    set_fg
    mr    r3, r14
    mr    r4, r15
    li    r5, 16
    li    r6, 16
    bl    frect
    b     pdt_d
pdt_e:
    li    r3, 4                           # floor (black)
    bl    set_fg
    mr    r3, r14
    mr    r4, r15
    li    r5, 16
    li    r6, 16
    bl    frect
    cmpwi r16, 2
    bne   pdt_p
    li    r3, 6                           # dot
    bl    set_fg
    addi  r3, r14, 6
    addi  r4, r15, 6
    li    r5, 4
    li    r6, 4
    bl    frect
    b     pdt_d
pdt_p:
    cmpwi r16, 3
    bne   pdt_g
    li    r3, 6                           # power pellet
    bl    set_fg
    addi  r3, r14, 4
    addi  r4, r15, 4
    li    r5, 8
    li    r6, 8
    bl    frect
    b     pdt_d
pdt_g:
    cmpwi r16, 5
    bne   pdt_d
    li    r3, 9                           # gate bar
    bl    set_fg
    mr    r3, r14
    addi  r4, r15, 6
    li    r5, 16
    li    r6, 4
    bl    frect
pdt_d:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

pm_draw_pac:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 6
    bl    set_fg
    LWZA  r3, pm_px
    slwi  r3, r3, 4
    addi  r3, r3, (PMX0+2)
    LWZA  r4, pm_py
    slwi  r4, r4, 4
    addi  r4, r4, (PMY0+2)
    li    r5, 12
    li    r6, 12
    bl    frect
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# pm_draw_ghost: r3 = index
pm_draw_ghost:
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    mulli r4, r3, 20
    LA    r14, pm_gh
    add   r14, r14, r4
    stw   r3, 20(r1)                      # idx
    lwz   r5, 0(r14)
    slwi  r5, r5, 4
    addi  r15, r5, PMX0
    lwz   r5, 4(r14)
    slwi  r5, r5, 4
    addi  r16, r5, PMY0
    lwz   r5, 12(r14)
    cmpwi r5, GH_EATEN
    bne   pdg_b
    li    r3, 1                           # eyes only
    bl    set_fg
    addi  r3, r15, 3
    addi  r4, r16, 5
    li    r5, 4
    li    r6, 4
    bl    frect
    li    r3, 1
    bl    set_fg
    addi  r3, r15, 9
    addi  r4, r16, 5
    li    r5, 4
    li    r6, 4
    bl    frect
    b     pdg_d
pdg_b:
    cmpwi r5, GH_FRIGHT
    bne   pdg_c
    li    r3, 9                           # frightened (grey)
    b     pdg_s
pdg_c:
    lwz   r3, 20(r1)
    LA    r4, pm_gcols
    lbzx  r3, r4, r3
pdg_s:
    bl    set_fg
    addi  r3, r15, 2
    addi  r4, r16, 2
    li    r5, 12
    li    r6, 12
    bl    frect
pdg_d:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

pm_draw_vals:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 1
    li    r4, 0
    bl    setfb
    LWZA  r3, pm_score
    LA    r4, dec_buf
    bl    fmt_dec4
    li    r3, 496
    li    r4, 80
    LA    r5, dec_buf
    bl    pstr
    LWZA  r3, pm_hi
    LA    r4, dec_buf
    bl    fmt_dec4
    li    r3, 496
    li    r4, 128
    LA    r5, dec_buf
    bl    pstr
    LWZA  r3, pm_lives
    bl    two_digits
    LA    r5, numstr
    stb   r4, 0(r5)
    stb   r3, 1(r5)
    li    r3, 0
    stb   r3, 2(r5)
    li    r3, 496
    li    r4, 176
    LA    r5, numstr
    bl    pstr
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

pm_draw_actors:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    bl    pm_draw_pac
    li    r14, 0
pda_g:
    mr    r3, r14
    bl    pm_draw_ghost
    addi  r14, r14, 1
    cmpwi r14, 4
    bne   pda_g
    lwz   r14, 8(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

pacman_partial:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    LWZA  r3, pm_opx                      # erase old + repaint current tiles
    LWZA  r4, pm_opy
    bl    pm_draw_tile
    LWZA  r3, pm_px
    LWZA  r4, pm_py
    bl    pm_draw_tile
    LA    r14, pm_ogh
    li    r15, 4
pp_e:
    lwz   r3, 0(r14)
    lwz   r4, 4(r14)
    bl    pm_draw_tile
    addi  r14, r14, 8
    subic. r15, r15, 1
    bne   pp_e
    LA    r14, pm_gh
    li    r15, 4
pp_c:
    lwz   r3, 0(r14)
    lwz   r4, 4(r14)
    bl    pm_draw_tile
    addi  r14, r14, 20
    subic. r15, r15, 1
    bne   pp_c
    bl    pm_draw_actors
    bl    pm_draw_vals
    li    r3, 0
    STWA  r3, pm_pf, r4
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

app_pacman_draw:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    li    r15, 0                          # ty
apd_r:
    li    r14, 0                          # tx
apd_c:
    mr    r3, r14
    mr    r4, r15
    bl    pm_draw_tile
    addi  r14, r14, 1
    cmpwi r14, PMC
    bne   apd_c
    addi  r15, r15, 1
    cmpwi r15, PMR
    bne   apd_r
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 480
    li    r4, 64
    LA    r5, s_pm_score
    bl    pstr
    li    r3, 480
    li    r4, 112
    LA    r5, s_pm_hi
    bl    pstr
    li    r3, 480
    li    r4, 160
    LA    r5, s_pm_lives
    bl    pstr
    bl    pm_draw_vals
    bl    pm_draw_actors
    LWZA  r3, pm_state
    cmpwi r3, PMS_READY
    bne   apd_ov
    li    r3, 0
    li    r4, 1
    bl    setfb
    li    r3, 216
    li    r4, 244
    LA    r5, s_pm_ready
    bl    pstr
    b     apd_d
apd_ov:
    cmpwi r3, PMS_OVER
    bne   apd_d
    li    r3, 0
    li    r4, 1
    bl    setfb
    li    r3, 204
    li    r4, 244
    LA    r5, s_pm_over
    bl    pstr
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 480
    li    r4, 208
    LA    r5, s_pm_new
    bl    pstr
apd_d:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# ---------------------------------------------------------------- data
.section .rodata
.align 2
pm_dx:      .long 0, -1, 0, 1             # UP, LEFT, DOWN, RIGHT
pm_dy:      .long -1, 0, 1, 0
pm_corners: .long 26,1,  1,1,  1,23,  26,23
pm_modedur: .long 70, 200, 70, 200, 50, 200, 50, 30000
# ghost spawn: x, y, dir, state, timer (x4). Ghost0 starts at the door.
pm_gh_init: .long 14,10,1,GH_SCAT,0
            .long 13,12,0,GH_HOUSE,15
            .long 15,12,0,GH_HOUSE,30
            .long 12,12,0,GH_HOUSE,45
pm_gcols:   .byte 7, 3, 8, 5              # red, magenta, orange, green
s_pm_score: .asciz "SCORE"
s_pm_hi:    .asciz "HI"
s_pm_lives: .asciz "LIVES"
s_pm_ready: .asciz "READY!"
s_pm_over:  .asciz "GAME OVER"
s_pm_new:   .asciz "A=new"
.align 2
# maze template: 0 empty, 1 wall, 2 dot, 3 power, 4 house, 5 gate (28x25)
pm_maze_tpl:
 .byte 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
 .byte 1,3,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,3,1
 .byte 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
 .byte 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
 .byte 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1
 .byte 1,2,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,1,2,1
 .byte 1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1
 .byte 1,1,1,1,1,2,1,1,1,1,0,1,0,0,0,0,1,0,1,1,1,1,2,1,1,1,1,1
 .byte 0,0,0,0,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,0,0,0,0
 .byte 0,0,0,0,1,2,1,0,1,1,1,1,5,0,0,5,1,1,1,1,0,1,2,1,0,0,0,0
 .byte 1,1,1,1,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,1,1,1,1
 .byte 0,0,0,0,0,2,0,0,1,4,4,4,4,4,4,4,4,4,4,1,0,0,2,0,0,0,0,0
 .byte 0,0,0,0,1,2,1,0,1,4,4,4,4,4,4,4,4,4,4,1,0,1,2,1,0,0,0,0
 .byte 0,0,0,0,1,2,1,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1,2,1,0,0,0,0
 .byte 1,1,1,1,1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,1,1,1,1,1
 .byte 1,2,2,2,2,2,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,2,2,2,2,2,1
 .byte 1,2,1,1,1,2,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,2,1,1,1,2,1
 .byte 1,3,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,3,1
 .byte 1,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,1
 .byte 1,2,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,1,2,2,2,2,2,1
 .byte 1,2,1,1,1,1,1,1,1,1,2,1,2,1,1,2,1,2,1,1,1,1,1,1,1,1,2,1
 .byte 1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1
 .byte 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
 .byte 1,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,1
 .byte 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
.align 2
.section .text
