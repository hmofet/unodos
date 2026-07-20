# ============================================================================
# UnoDOS / PowerPC Mac — Pac-Man. PowerPC translation of the verified rpi port
# (28x25 maze; Blinky/Pinky/Clyde AI; scatter/chase + frightened). Tile-stepped,
# one cell per step, full repaint on a step. 640x480, same layout as rpi.
# Maze: 0 empty 1 wall 2 dot 3 power 4 house 5 gate. Ghost states 0..4.
# Ghost struct (GSIZE): GX@0 GY@4 GDIR@8 GST@12 GTMR@16.
# ============================================================================

dirdx:                                   # r3=dir -> r3=dx. Leaf.
    LA    r4, pm_dx
    slwi  r3, r3, 2
    lwzx  r3, r4, r3
    blr
dirdy:
    LA    r4, pm_dy
    slwi  r3, r3, 2
    lwzx  r3, r4, r3
    blr

pm_mode_state:                           # -> r3 = 2 chase if pm_mode odd else 1. Leaf.
    LWZA  r3, pm_mode
    andi. r0, r3, 1
    beq   pms_scat
    li    r3, 2
    blr
pms_scat:
    li    r3, 1
    blr

# pm_walkable: r3=tx r4=ty r5=ghost r6=eaten -> r3=1/0. Leaf.
pm_walkable:
    cmpwi r4, 0
    blt   pw_no
    cmpwi r4, PM_ROWS
    bge   pw_no
    cmpwi r3, 0
    bge   pw_x1
    addi  r3, r3, PM_COLS
pw_x1:
    cmpwi r3, PM_COLS
    blt   pw_x2
    subi  r3, r3, PM_COLS
pw_x2:
    mulli r7, r4, PM_COLS
    add   r7, r7, r3
    LA    r8, pm_maze
    lbzx  r9, r8, r7
    cmpwi r9, 1
    beq   pw_no
    cmpwi r9, 4
    beq   pw_gate
    cmpwi r9, 5
    beq   pw_gate
    li    r3, 1
    blr
pw_gate:
    cmpwi r5, 0
    beq   pw_no
    cmpwi r6, 0
    beq   pw_no
    li    r3, 1
    blr
pw_no:
    li    r3, 0
    blr

pm_load_maze:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r3, pm_maze_tpl
    LA    r4, pm_maze
    li    r5, (PM_COLS*PM_ROWS)
    mtctr r5
    li    r6, 0
plm_l:
    lbz   r7, 0(r3)
    stb   r7, 0(r4)
    addi  r3, r3, 1
    addi  r4, r4, 1
    cmpwi r7, 2
    beq   plm_dot
    cmpwi r7, 3
    bne   plm_nx
plm_dot:
    addi  r6, r6, 1
plm_nx:
    bdnz  plm_l
    STWA  r6, pm_dots, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

gh_ptr:                                  # r3=idx -> r3 = &pm_gh[idx]. Leaf.
    mulli r3, r3, GSIZE
    LA    r4, pm_gh
    add   r3, r4, r3
    blr

pm_reset_actors:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 14
    STWA  r3, pm_x, r4
    li    r3, 19
    STWA  r3, pm_y, r4
    li    r3, 1
    STWA  r3, pm_dir, r4
    STWA  r3, pm_ndir, r4
    li    r3, 0
    bl    gh_ptr                          # Blinky (14,10) scatter
    li    r4, 14
    stw   r4, 0(r3)
    li    r4, 10
    stw   r4, 4(r3)
    li    r4, 1
    stw   r4, 8(r3)
    li    r4, 1
    stw   r4, 12(r3)
    li    r4, 0
    stw   r4, 16(r3)
    li    r3, 1
    bl    gh_ptr                          # Pinky (13,12) house t=20
    li    r4, 13
    stw   r4, 0(r3)
    li    r4, 12
    stw   r4, 4(r3)
    li    r4, 0
    stw   r4, 8(r3)
    stw   r4, 12(r3)
    li    r4, 20
    stw   r4, 16(r3)
    li    r3, 2
    bl    gh_ptr                          # Clyde (15,12) house t=40
    li    r4, 15
    stw   r4, 0(r3)
    li    r4, 12
    stw   r4, 4(r3)
    li    r4, 0
    stw   r4, 8(r3)
    stw   r4, 12(r3)
    li    r4, 40
    stw   r4, 16(r3)
    li    r3, 0
    STWA  r3, pm_fr, r4
    STWA  r3, pm_kills, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

pacman_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 0
    STWA  r3, pm_score, r4
    li    r3, 3
    STWA  r3, pm_lives, r4
    li    r3, 1
    STWA  r3, pm_level, r4
    li    r3, 0
    STWA  r3, pm_mode, r4
    STWA  r3, pm_modet, r4
    STWA  r3, pm_st, r4
    STWA  r3, pm_sc, r4
    STWA  r3, pm_ft, r4
    lis   r3, 0x0001                      # deterministic seed (AUTOTEST drives moves)
    ori   r3, r3, 0x2345
    STWA  r3, g_seed, r4
    bl    pm_load_maze
    bl    pm_reset_actors
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ghost iteration helpers (frighten / unfright / modeflip)
pm_frighten:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    li    r14, 0
pf_l:
    cmpwi r14, 3
    bge   pf_done
    mr    r3, r14
    bl    gh_ptr
    mr    r15, r3
    lwz   r3, 12(r15)
    cmpwi r3, 1
    beq   pf_set
    cmpwi r3, 2
    bne   pf_nx
pf_set:
    li    r4, 3
    stw   r4, 12(r15)
    lwz   r4, 8(r15)
    xori  r4, r4, 2
    stw   r4, 8(r15)
pf_nx:
    addi  r14, r14, 1
    b     pf_l
pf_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

pm_unfright:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    li    r14, 0
puf_l:
    cmpwi r14, 3
    bge   puf_done
    mr    r3, r14
    bl    gh_ptr
    mr    r15, r3
    lwz   r3, 12(r15)
    cmpwi r3, 3
    bne   puf_nx
    bl    pm_mode_state
    stw   r3, 12(r15)
puf_nx:
    addi  r14, r14, 1
    b     puf_l
puf_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

pm_modeflip:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    li    r14, 0
pmf_l:
    cmpwi r14, 3
    bge   pmf_done
    mr    r3, r14
    bl    gh_ptr
    mr    r15, r3
    lwz   r3, 12(r15)
    cmpwi r3, 1
    beq   pmf_set
    cmpwi r3, 2
    bne   pmf_nx
pmf_set:
    bl    pm_mode_state
    stw   r3, 12(r15)
    lwz   r4, 8(r15)
    xori  r4, r4, 2
    stw   r4, 8(r15)
pmf_nx:
    addi  r14, r14, 1
    b     pmf_l
pmf_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

pm_kill:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, pm_lives
    subi  r3, r3, 1
    STWA  r3, pm_lives, r4
    cmpwi r3, 0
    bgt   pk_alive
    li    r3, 1
    STWA  r3, pm_st, r4
    b     pk_out
pk_alive:
    bl    pm_reset_actors
pk_out:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# pm_steer: r3 = ghost index. Pick GDIR (the canonical AI).
pm_steer:
    mflr  r0
    stwu  r1, -80(r1)
    stw   r0, 84(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    stw   r18, 24(r1)
    stw   r19, 28(r1)
    stw   r20, 32(r1)
    stw   r21, 36(r1)
    stw   r22, 40(r1)
    stw   r23, 44(r1)
    mr    r14, r3                         # gidx
    bl    gh_ptr
    mr    r15, r3                         # ghost ptr
    lwz   r16, 0(r15)                     # gtx
    lwz   r17, 4(r15)                     # gty
    lwz   r3, 12(r15)
    cmpwi r3, 3                           # FRIGHT -> random
    bne   ps_target
    li    r18, 8
ps_rnd:
    bl    rng
    andi. r19, r3, 3                      # cand
    lwz   r4, 8(r15)
    xori  r4, r4, 2
    cmpw  r19, r4
    beq   ps_rtry
    mr    r3, r19
    bl    dirdx
    add   r20, r3, r16                    # nx
    mr    r3, r19
    bl    dirdy
    add   r21, r3, r17                    # ny
    mr    r3, r20
    mr    r4, r21
    li    r5, 1
    li    r6, 0
    bl    pm_walkable
    cmpwi r3, 0
    beq   ps_rtry
    stw   r19, 8(r15)
    b     ps_out
ps_rtry:
    subi  r18, r18, 1
    cmpwi r18, 0
    bne   ps_rnd
    b     ps_out
ps_target:
    cmpwi r3, 4                           # EATEN -> home (14,10)
    bne   ps_note
    li    r3, 14
    STWA  r3, pm_tgx, r4
    li    r3, 10
    STWA  r3, pm_tgy, r4
    b     ps_pick
ps_note:
    cmpwi r3, 2                           # CHASE?
    bne   ps_scat
    LWZA  r18, pm_x                       # pac tx
    LWZA  r19, pm_y                       # pac ty
    cmpwi r14, 0
    beq   ps_blinky
    cmpwi r14, 1
    beq   ps_pinky
    # Clyde: manhattan(ghost,pac) <= 8 ? corner : pac
    subf  r20, r18, r16                   # gtx - ptx
    cmpwi r20, 0
    bge   ps_cx
    neg   r20, r20
ps_cx:
    subf  r21, r19, r17
    cmpwi r21, 0
    bge   ps_cy
    neg   r21, r21
ps_cy:
    add   r20, r20, r21
    cmpwi r20, 8
    bgt   ps_blinky
    b     ps_scat
ps_pinky:
    LWZA  r3, pm_dir
    bl    dirdx
    slwi  r3, r3, 2                       # 4*dx
    add   r3, r3, r18
    STWA  r3, pm_tgx, r4
    LWZA  r3, pm_dir
    bl    dirdy
    slwi  r3, r3, 2
    add   r3, r3, r19
    STWA  r3, pm_tgy, r4
    b     ps_pick
ps_blinky:
    STWA  r18, pm_tgx, r4
    STWA  r19, pm_tgy, r4
    b     ps_pick
ps_scat:
    mr    r3, r14
    slwi  r3, r3, 2
    LA    r4, pm_cornx
    lwzx  r3, r4, r3
    STWA  r3, pm_tgx, r4
    mr    r3, r14
    slwi  r3, r3, 2
    LA    r4, pm_corny
    lwzx  r3, r4, r3
    STWA  r3, pm_tgy, r4
ps_pick:
    lwz   r23, 8(r15)
    xori  r23, r23, 2                     # reverse dir
    li    r21, -1                         # best dir
    li    r22, 0x7FFF                     # best dist
    li    r18, 0                          # dir
ps_try:
    cmpw  r18, r23
    beq   ps_skip
    mr    r3, r18
    bl    dirdx
    add   r19, r3, r16                    # nx
    mr    r3, r18
    bl    dirdy
    add   r20, r3, r17                    # ny
    mr    r3, r19
    mr    r4, r20
    li    r5, 1
    lwz   r6, 12(r15)
    cmpwi r6, 4
    li    r6, 0
    bne   ps_we
    li    r6, 1
ps_we:
    bl    pm_walkable
    cmpwi r3, 0
    beq   ps_skip
    mr    r3, r19                         # wrap nx for distance
    cmpwi r3, 0
    bge   ps_w1
    addi  r3, r3, PM_COLS
ps_w1:
    cmpwi r3, PM_COLS
    blt   ps_w2
    subi  r3, r3, PM_COLS
ps_w2:
    LWZA  r4, pm_tgx
    subf  r3, r4, r3
    cmpwi r3, 0
    bge   ps_a1
    neg   r3, r3
ps_a1:
    LWZA  r4, pm_tgy
    subf  r4, r4, r20
    cmpwi r4, 0
    bge   ps_a2
    neg   r4, r4
ps_a2:
    add   r3, r3, r4                      # manhattan
    cmpw  r3, r22
    bge   ps_skip
    mr    r22, r3
    mr    r21, r18
ps_skip:
    addi  r18, r18, 1
    cmpwi r18, 4
    blt   ps_try
    cmpwi r21, 0
    blt   ps_out
    stw   r21, 8(r15)
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
    lwz   r23, 44(r1)
    lwz   r0, 84(r1)
    addi  r1, r1, 80
    mtlr  r0
    blr

# pm_step: one tile-stepped game step.
pm_step:
    mflr  r0
    stwu  r1, -64(r1)
    stw   r0, 68(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    stw   r18, 24(r1)
    LWZA  r3, pm_st
    cmpwi r3, 0
    bne   pstep_out
    LWZA  r3, pm_sc
    addi  r3, r3, 1
    STWA  r3, pm_sc, r4
    LWZA  r3, pm_fr
    cmpwi r3, 0
    beq   pstep_mode
    subi  r3, r3, 1
    STWA  r3, pm_fr, r4
    cmpwi r3, 0
    bne   pstep_pac
    bl    pm_unfright
    b     pstep_pac
pstep_mode:
    LWZA  r3, pm_modet
    addi  r3, r3, 1
    STWA  r3, pm_modet, r4
    mr    r5, r3                          # modet
    LWZA  r3, pm_mode
    cmpwi r3, 7
    ble   psm_ok
    li    r3, 7
psm_ok:
    slwi  r3, r3, 2
    LA    r4, pm_modedur
    lwzx  r4, r4, r3
    cmpw  r5, r4
    blt   pstep_pac
    li    r3, 0
    STWA  r3, pm_modet, r4
    LWZA  r3, pm_mode
    cmpwi r3, 7
    bge   psm_nf
    addi  r3, r3, 1
    STWA  r3, pm_mode, r4
psm_nf:
    bl    pm_modeflip
pstep_pac:
    LWZA  r14, pm_x                       # ptx
    LWZA  r15, pm_y                       # pty
    mulli r3, r15, PM_COLS
    add   r3, r3, r14
    LA    r4, pm_maze
    lbzx  r5, r4, r3
    cmpwi r5, 2
    beq   pstep_dot
    cmpwi r5, 3
    beq   pstep_pow
    b     pstep_clear
pstep_dot:
    li    r6, 0
    stbx  r6, r4, r3
    LWZA  r6, pm_score
    addi  r6, r6, 10
    STWA  r6, pm_score, r7
    LWZA  r6, pm_dots
    subi  r6, r6, 1
    STWA  r6, pm_dots, r7
    b     pstep_clear
pstep_pow:
    li    r6, 0
    stbx  r6, r4, r3
    LWZA  r6, pm_score
    addi  r6, r6, 50
    STWA  r6, pm_score, r7
    LWZA  r6, pm_dots
    subi  r6, r6, 1
    STWA  r6, pm_dots, r7
    li    r6, FRIGHT_STEPS
    STWA  r6, pm_fr, r7
    li    r6, 0
    STWA  r6, pm_kills, r7
    bl    pm_frighten
pstep_clear:
    LWZA  r3, pm_dots
    cmpwi r3, 0
    bne   pstep_move
    LWZA  r3, pm_level
    addi  r3, r3, 1
    STWA  r3, pm_level, r4
    bl    pm_load_maze
    bl    pm_reset_actors
    b     pstep_out
pstep_move:
    LWZA  r16, pm_ndir                    # try queued dir
    mr    r3, r16
    bl    dirdx
    add   r17, r3, r14
    mr    r3, r16
    bl    dirdy
    add   r18, r3, r15
    mr    r3, r17
    mr    r4, r18
    li    r5, 0
    li    r6, 0
    bl    pm_walkable
    cmpwi r3, 0
    beq   pstep_dir
    STWA  r16, pm_dir, r4
pstep_dir:
    LWZA  r16, pm_dir
    mr    r3, r16
    bl    dirdx
    add   r17, r3, r14
    mr    r3, r16
    bl    dirdy
    add   r18, r3, r15
    mr    r3, r17
    mr    r4, r18
    li    r5, 0
    li    r6, 0
    bl    pm_walkable
    cmpwi r3, 0
    beq   pstep_ghosts
    cmpwi r17, 0
    bge   pmv_w1
    li    r17, (PM_COLS-1)
pmv_w1:
    cmpwi r17, PM_COLS
    blt   pmv_w2
    li    r17, 0
pmv_w2:
    STWA  r17, pm_x, r4
    STWA  r18, pm_y, r4
pstep_ghosts:
    li    r14, 0                          # gidx
pstep_gl:
    mr    r3, r14
    bl    gh_ptr
    mr    r15, r3                         # ghost ptr
    lwz   r3, 12(r15)
    cmpwi r3, 0
    bne   pstep_gactive
    lwz   r3, 16(r15)                     # house timer
    subi  r3, r3, 1
    stw   r3, 16(r15)
    cmpwi r3, 0
    bgt   pstep_gnext
    li    r4, 14
    stw   r4, 0(r15)
    li    r4, 10
    stw   r4, 4(r15)
    li    r4, 1
    stw   r4, 8(r15)
    bl    pm_mode_state
    stw   r3, 12(r15)
    b     pstep_gnext
pstep_gactive:
    cmpwi r3, 4                           # eaten arriving home?
    bne   pstep_gsteer
    lwz   r4, 0(r15)
    cmpwi r4, 14
    bne   pstep_gsteer
    lwz   r4, 4(r15)
    cmpwi r4, 10
    bne   pstep_gsteer
    bl    pm_mode_state
    stw   r3, 12(r15)
pstep_gsteer:
    mr    r3, r14
    bl    pm_steer
    lwz   r3, 12(r15)                     # frightened half speed
    cmpwi r3, 3
    bne   pstep_gmove
    LWZA  r3, pm_sc
    andi. r0, r3, 1
    bne   pstep_gcoll
pstep_gmove:
    lwz   r16, 8(r15)                     # gdir
    lwz   r17, 0(r15)                     # gx
    mr    r3, r16
    bl    dirdx
    add   r17, r3, r17                    # nx
    lwz   r18, 4(r15)
    mr    r3, r16
    bl    dirdy
    add   r18, r3, r18                    # ny
    mr    r3, r17
    mr    r4, r18
    li    r5, 1
    lwz   r6, 12(r15)
    cmpwi r6, 4
    li    r6, 0
    bne   pgm_we
    li    r6, 1
pgm_we:
    bl    pm_walkable
    cmpwi r3, 0
    beq   pstep_gcoll
    cmpwi r17, 0
    bge   pgm_w1
    li    r17, (PM_COLS-1)
pgm_w1:
    cmpwi r17, PM_COLS
    blt   pgm_w2
    li    r17, 0
pgm_w2:
    stw   r17, 0(r15)
    stw   r18, 4(r15)
pstep_gcoll:
    lwz   r3, 0(r15)
    LWZA  r4, pm_x
    cmpw  r3, r4
    bne   pstep_gnext
    lwz   r3, 4(r15)
    LWZA  r4, pm_y
    cmpw  r3, r4
    bne   pstep_gnext
    lwz   r3, 12(r15)
    cmpwi r3, 3                           # frightened -> eat ghost
    bne   pstep_notfr
    li    r4, 4
    stw   r4, 12(r15)
    LWZA  r4, pm_kills
    li    r0, 200
    slw   r3, r0, r4                      # 200 << kills
    LWZA  r5, pm_score
    add   r5, r5, r3
    STWA  r5, pm_score, r6
    LWZA  r4, pm_kills
    cmpwi r4, 3
    bge   pstep_gnext
    addi  r4, r4, 1
    STWA  r4, pm_kills, r5
    b     pstep_gnext
pstep_notfr:
    cmpwi r3, 4
    beq   pstep_gnext
    bl    pm_kill
    b     pstep_out
pstep_gnext:
    addi  r14, r14, 1
    cmpwi r14, 3
    blt   pstep_gl
pstep_out:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r18, 24(r1)
    lwz   r0, 68(r1)
    addi  r1, r1, 64
    mtlr  r0
    blr

pacman_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_pade
    andi. r0, r3, PAD_U
    beq   pmu_l
    li    r4, 0
    STWA  r4, pm_ndir, r5
pmu_l:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_L
    beq   pmu_d
    li    r4, 1
    STWA  r4, pm_ndir, r5
pmu_d:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_D
    beq   pmu_r
    li    r4, 2
    STWA  r4, pm_ndir, r5
pmu_r:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_R
    beq   pmu_pace
    li    r4, 3
    STWA  r4, pm_ndir, r5
pmu_pace:
    LWZA  r3, a_gpause
    cmpwi r3, 0
    bne   pmu_done
    LWZA  r3, pm_ft
    addi  r3, r3, 1
    STWA  r3, pm_ft, r4
    cmpwi r3, PM_STEPFRAMES
    blt   pmu_done
    li    r3, 0
    STWA  r3, pm_ft, r4
    bl    pm_step
    li    r3, 1
    STWA  r3, v_dirty, r4
pmu_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ---- rendering -------------------------------------------------------------
pm_draw_cell:                            # r3=col r4=row r5=colour
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    mr    r14, r3
    mr    r15, r4
    mr    r3, r5
    bl    set_fg
    mulli r3, r14, PM_CELL
    addi  r3, r3, PMO_X
    mulli r4, r15, PM_CELL
    addi  r4, r4, PMO_Y
    li    r5, PM_CELL
    li    r6, PM_CELL
    bl    frect
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

pm_draw_dot:                             # r3=col r4=row r5=colour r6=size
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    mr    r14, r3
    mr    r15, r4
    mr    r16, r6
    mr    r3, r5
    bl    set_fg
    mulli r3, r14, PM_CELL
    addi  r3, r3, PMO_X
    li    r7, PM_CELL
    subf  r7, r16, r7
    srwi  r7, r7, 1
    add   r3, r3, r7
    mulli r4, r15, PM_CELL
    addi  r4, r4, PMO_Y
    li    r7, PM_CELL
    subf  r7, r16, r7
    srwi  r7, r7, 1
    add   r4, r4, r7
    mr    r5, r16
    mr    r6, r16
    bl    frect
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

pm_draw_maze:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    li    r14, 0                          # row
pdm_row:
    cmpwi r14, PM_ROWS
    bge   pdm_done
    li    r15, 0                          # col
pdm_col:
    cmpwi r15, PM_COLS
    bge   pdm_rowend
    mulli r3, r14, PM_COLS
    add   r3, r3, r15
    LA    r4, pm_maze
    lbzx  r16, r4, r3
    cmpwi r16, 1
    bne   pdm_dot
    mr    r3, r15
    mr    r4, r14
    li    r5, 2
    bl    pm_draw_cell
    b     pdm_next
pdm_dot:
    cmpwi r16, 2
    bne   pdm_pow
    mr    r3, r15
    mr    r4, r14
    li    r5, 1
    li    r6, 4
    bl    pm_draw_dot
    b     pdm_next
pdm_pow:
    cmpwi r16, 3
    bne   pdm_next
    mr    r3, r15
    mr    r4, r14
    li    r5, 1
    li    r6, 10
    bl    pm_draw_dot
pdm_next:
    addi  r15, r15, 1
    b     pdm_col
pdm_rowend:
    addi  r14, r14, 1
    b     pdm_row
pdm_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

pm_draw_actors:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    LWZA  r3, pm_x
    LWZA  r4, pm_y
    li    r5, 6                           # pac = yellow
    bl    pm_draw_cell
    li    r14, 0
pda_gl:
    cmpwi r14, 3
    bge   pda_done
    mr    r3, r14
    bl    gh_ptr
    mr    r15, r3
    lwz   r3, 12(r15)
    cmpwi r3, 3
    bne   pda_ne
    li    r16, 1                          # fright = white
    b     pda_col
pda_ne:
    cmpwi r3, 4
    bne   pda_body
    li    r16, 9                          # eaten = grey
    b     pda_col
pda_body:
    LA    r4, pm_ghcol
    lbzx  r16, r4, r14
pda_col:
    lwz   r3, 0(r15)
    lwz   r4, 4(r15)
    mr    r5, r16
    bl    pm_draw_cell
    addi  r14, r14, 1
    b     pda_gl
pda_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

pm_itoa5:                                # r3 = value -> numstr (5 digits + NUL). Leaf.
    LA    r9, numstr
    li    r4, 10000
    divwu r5, r3, r4
    mullw r6, r5, r4
    subf  r3, r6, r3
    addi  r5, r5, '0'
    stb   r5, 0(r9)
    li    r4, 1000
    divwu r5, r3, r4
    mullw r6, r5, r4
    subf  r3, r6, r3
    addi  r5, r5, '0'
    stb   r5, 1(r9)
    li    r4, 100
    divwu r5, r3, r4
    mullw r6, r5, r4
    subf  r3, r6, r3
    addi  r5, r5, '0'
    stb   r5, 2(r9)
    li    r4, 10
    divwu r5, r3, r4
    mullw r6, r5, r4
    subf  r3, r6, r3
    addi  r5, r5, '0'
    stb   r5, 3(r9)
    addi  r3, r3, '0'
    stb   r3, 4(r9)
    li    r0, 0
    stb   r0, 5(r9)
    blr

pm_draw_panel:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 470
    li    r4, 40
    LA    r5, s_pmscore
    bl    pstr
    LWZA  r3, pm_score
    bl    pm_itoa5
    li    r3, 470
    li    r4, 60
    LA    r5, numstr
    bl    pstr
    li    r3, 470
    li    r4, 90
    LA    r5, s_pmlives
    bl    pstr
    LWZA  r3, pm_lives
    bl    two_digits
    LA    r5, numstr
    stb   r4, 0(r5)
    stb   r3, 1(r5)
    li    r0, 0
    stb   r0, 2(r5)
    li    r3, 470
    li    r4, 110
    LA    r5, numstr
    bl    pstr
    LWZA  r3, pm_st
    cmpwi r3, 0
    beq   pmp_done
    li    r3, 470
    li    r4, 150
    LA    r5, s_pmover
    bl    pstr
pmp_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

app_pacman:
    LA    r5, t_pacman
    bl    draw_chrome
    bl    pm_draw_maze
    bl    pm_draw_actors
    bl    pm_draw_panel
    b     da_ret

# ---- data ------------------------------------------------------------------
.align 2
pm_dx:     .long 0, -1, 0, 1
pm_dy:     .long -1, 0, 1, 0
pm_cornx:  .long 26, 1, 1
pm_corny:  .long 1, 1, 23
pm_modedur:.long 127, 364, 127, 364, 91, 364, 91, 32000
pm_ghcol:  .byte 7, 3, 8
.align 2
s_pmscore: .asciz "SCORE"
s_pmlives: .asciz "LIVES"
s_pmover:  .asciz "GAME OVER"
.align 2
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
 .byte 1,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,1
 .byte 1,2,1,1,1,2,1,1,1,1,1,1,2,1,1,2,1,1,1,1,1,1,2,1,1,1,2,1
 .byte 1,2,2,2,2,2,2,2,2,2,2,2,2,1,1,2,2,2,2,2,2,2,2,2,2,2,2,1
 .byte 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
.align 2
