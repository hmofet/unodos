// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — Pac-Man (app 9), from genesis/pacman.i
// (itself from amiga/pacman.i): the same 28x25 maze template, dot/power-pellet
// eating, scatter/chase schedule, frightened mode with the eat chain, and the
// classic per-ghost targeting (Blinky direct, Pinky 4-ahead, Clyde hybrid; the
// fourth ghost is a close-range Clyde variant on the fourth corner).
//
// This port is CELL-GRID rendered (no sprites): one 16px framebuffer cell per
// maze tile, actors are filled squares (ghosts get white eyes), and movement is
// tile-stepped — pac steps a tile every PM_PSPD frames, ghosts every PM_GSPD
// (frightened ghosts every other step = half speed). Steering happens at every
// tile, exactly like the donor's tile-centre logic.
//
//   d-pad  steer (queued turn, donor-style)   A  new game (title/over)
//   B      back to the launcher
// ============================================================================

.equ PM_COLS,  28
.equ PM_ROWS,  25
.equ PM_OXP,   96                         // maze pixel origin
.equ PM_OYP,   40
.equ PM_PSPD,  4                          // frames per pac step
.equ PM_GSPD,  5                          // frames per ghost step
.equ PM_READYF,90                         // READY! frames
.equ PM_FRIGHTF,360                       // frightened frames
// states
.equ PMS_READY,1
.equ PMS_PLAY, 2
.equ PMS_OVER, 4
// ghost states
.equ GH_HOUSE, 0
.equ GH_SCAT,  1
.equ GH_CHASE, 2
.equ GH_FRIGHT,3
.equ GH_EATEN, 4

// ---------------------------------------------------------------- helpers
// pm_walkable: w0=tx w1=ty w2=ghost? w3=eaten? -> w0 = 1 ok / 0 blocked. Leaf.
pm_walkable:
    cmp   w1, #0
    b.lt  pw_no
    cmp   w1, #PM_ROWS
    b.ge  pw_no
    cmp   w0, #0
    b.ge  pw_x1
    add   w0, w0, #PM_COLS                // tunnel wrap
pw_x1:
    cmp   w0, #PM_COLS
    b.lt  pw_x2
    sub   w0, w0, #PM_COLS
pw_x2:
    mov   w4, #PM_COLS
    mul   w5, w1, w4
    add   w5, w5, w0
    ldr   x4, =pm_maze
    ldrb  w5, [x4, w5, uxtw]
    cmp   w5, #1
    b.eq  pw_no
    cmp   w5, #4
    b.eq  pw_gate
    cmp   w5, #5
    b.eq  pw_gate
    mov   w0, #1
    ret
pw_gate:                                  // house/gate: eaten ghosts only
    cbz   w2, pw_no
    cbz   w3, pw_no
    mov   w0, #1
    ret
pw_no:
    mov   w0, #0
    ret

// pm_mode_state: -> w0 = GH_SCAT or GH_CHASE from pm_mode parity. Leaf.
pm_mode_state:
    ldr   x0, =pm_mode
    ldr   w0, [x0]
    tst   w0, #1
    mov   w1, #GH_CHASE
    mov   w2, #GH_SCAT
    csel  w0, w1, w2, ne
    ret

// pm_gptr: w0 = ghost index -> x0 = ghost struct ptr. Leaf.
pm_gptr:
    mov   w1, #GH_SZ
    mul   w1, w0, w1
    ldr   x0, =pm_gh
    add   x0, x0, w1, uxtw
    ret

// pm_load_maze: template -> pm_maze, count the dots
pm_load_maze:
    ldr   x0, =pm_maze_tpl
    ldr   x1, =pm_maze
    mov   w2, #(PM_COLS*PM_ROWS)
    mov   w3, #0
plm_cp:
    ldrb  w4, [x0], #1
    strb  w4, [x1], #1
    cmp   w4, #2
    b.eq  plm_dot
    cmp   w4, #3
    b.ne  plm_nx
plm_dot:
    add   w3, w3, #1
plm_nx:
    subs  w2, w2, #1
    b.ne  plm_cp
    ldr   x0, =pm_dots
    str   w3, [x0]
    ret

// pm_reset_actors: pac + the 4 ghosts to their spawn poses
pm_reset_actors:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x0, =pm_px
    mov   w1, #14
    str   w1, [x0]
    ldr   x0, =pm_opx
    str   w1, [x0]
    ldr   x0, =pm_py
    mov   w1, #19
    str   w1, [x0]
    ldr   x0, =pm_opy
    str   w1, [x0]
    ldr   x0, =pm_pdir
    mov   w1, #1                          // LEFT
    str   w1, [x0]
    ldr   x0, =pm_pnext
    str   w1, [x0]
    // ghosts from the spawn table (x,y,dir,st,tmr)
    mov   w19, #0
pra_g:
    mov   w0, w19
    bl    pm_gptr
    mov   x20, x0
    ldr   x1, =pm_gspawn
    mov   w2, #20
    mul   w2, w19, w2
    add   x1, x1, w2, uxtw
    ldr   w3, [x1, #0]
    str   w3, [x20, #GH_X]
    str   w3, [x20, #GH_OX]
    ldr   w3, [x1, #4]
    str   w3, [x20, #GH_Y]
    str   w3, [x20, #GH_OY]
    ldr   w3, [x1, #8]
    str   w3, [x20, #GH_DIR]
    ldr   w3, [x1, #12]
    str   w3, [x20, #GH_ST]
    ldr   w3, [x1, #16]
    str   w3, [x20, #GH_TMR]
    add   w19, w19, #1
    cmp   w19, #4
    b.ne  pra_g
    ldr   x0, =pm_fright
    str   wzr, [x0]
    ldr   x0, =pm_kills
    str   wzr, [x0]
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pacman_init: a fresh game (pm_hi survives within the session)
pacman_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =SYS_TIMER_CLO
    ldr   w1, [x0]
    orr   w1, w1, #1
    ldr   x0, =g_seed
    str   w1, [x0]
    ldr   x0, =pm_score
    str   wzr, [x0]
    ldr   x0, =pm_lives
    mov   w1, #3
    str   w1, [x0]
    ldr   x0, =pm_level
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =pm_mode
    str   wzr, [x0]
    ldr   x0, =pm_modet
    str   wzr, [x0]
    ldr   x0, =pm_gtick
    str   wzr, [x0]
    bl    pm_load_maze
    bl    pm_reset_actors
    ldr   x0, =pm_state
    mov   w1, #PMS_READY
    str   w1, [x0]
    ldr   x0, =pm_statet
    mov   w1, #PM_READYF
    str   w1, [x0]
    ldr   x0, =pm_ptmr
    mov   w1, #PM_PSPD
    str   w1, [x0]
    ldr   x0, =pm_gtmr
    mov   w1, #PM_GSPD
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// ---------------------------------------------------------------- update
pacman_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_OVER
    b.eq  pmu_over
    // queued-turn input (READY + PLAY)
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_U
    b.eq  pmu_i1
    ldr   x1, =pm_pnext
    str   wzr, [x1]
pmu_i1:
    tst   w0, #PAD_L
    b.eq  pmu_i2
    ldr   x1, =pm_pnext
    mov   w2, #1
    str   w2, [x1]
pmu_i2:
    tst   w0, #PAD_D
    b.eq  pmu_i3
    ldr   x1, =pm_pnext
    mov   w2, #2
    str   w2, [x1]
pmu_i3:
    tst   w0, #PAD_R
    b.eq  pmu_i4
    ldr   x1, =pm_pnext
    mov   w2, #3
    str   w2, [x1]
pmu_i4:
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_READY
    b.ne  pmu_play
    // READY countdown
    ldr   x2, =pm_statet
    ldr   w0, [x2]
    sub   w0, w0, #1
    str   w0, [x2]
    cbnz  w0, pmu_done
    ldr   x0, =pm_state
    mov   w1, #PMS_PLAY
    str   w1, [x0]
    ldr   x0, =v_dirty                    // clear the READY! overlay
    mov   w1, #1
    str   w1, [x0]
pmu_done:
    ldp   x29, x30, [sp], #16
    ret
pmu_over:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  pmu_done
    bl    pacman_init
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    b     pmu_done
pmu_play:
    // frightened countdown / scatter-chase schedule (donor: schedule pauses
    // while fright runs)
    ldr   x2, =pm_fright
    ldr   w0, [x2]
    cbz   w0, pmu_mode
    sub   w0, w0, #1
    str   w0, [x2]
    cbnz  w0, pmu_steps
    // fright over: frightened ghosts resume the schedule
    bl    pm_unfright
    b     pmu_steps
pmu_mode:
    ldr   x2, =pm_modet
    ldr   w0, [x2]
    add   w0, w0, #1
    str   w0, [x2]
    ldr   x1, =pm_mode
    ldr   w1, [x1]
    cmp   w1, #7
    mov   w3, #7
    csel  w1, w3, w1, gt
    ldr   x3, =pm_modedur
    ldr   w1, [x3, w1, uxtw #2]
    cmp   w0, w1
    b.lt  pmu_steps
    str   wzr, [x2]                       // modet = 0
    ldr   x2, =pm_mode
    ldr   w0, [x2]
    cmp   w0, #7
    b.ge  pmu_flip
    add   w0, w0, #1
    str   w0, [x2]
pmu_flip:
    bl    pm_mode_flip
pmu_steps:
    // pac step clock
    ldr   x2, =pm_ptmr
    ldr   w0, [x2]
    sub   w0, w0, #1
    str   w0, [x2]
    cbnz  w0, pmu_gh
    mov   w0, #PM_PSPD
    str   w0, [x2]
    bl    pac_step
pmu_gh:
    // ghost step clock
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_PLAY
    b.ne  pmu_done
    ldr   x2, =pm_gtmr
    ldr   w0, [x2]
    sub   w0, w0, #1
    str   w0, [x2]
    cbnz  w0, pmu_done
    mov   w0, #PM_GSPD
    str   w0, [x2]
    ldr   x2, =pm_gtick
    ldr   w0, [x2]
    add   w0, w0, #1
    str   w0, [x2]
    ldr   x2, =pm_gi
    str   wzr, [x2]
pmu_gl:
    ldr   x0, =pm_state                   // a kill aborts the sweep
    ldr   w0, [x0]
    cmp   w0, #PMS_PLAY
    b.ne  pmu_done
    ldr   x0, =pm_gi
    ldr   w0, [x0]
    bl    ghost_step
    ldr   x2, =pm_gi
    ldr   w0, [x2]
    add   w0, w0, #1
    str   w0, [x2]
    cmp   w0, #4
    b.ne  pmu_gl
    b     pmu_done

// pm_mode_flip: SCAT<->CHASE ghosts adopt the (new) schedule state + reverse
pm_mode_flip:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
pmf_g:
    mov   w0, w19
    bl    pm_gptr
    mov   x20, x0
    ldr   w1, [x20, #GH_ST]
    cmp   w1, #GH_SCAT
    b.eq  pmf_set
    cmp   w1, #GH_CHASE
    b.ne  pmf_n
pmf_set:
    bl    pm_mode_state
    str   w0, [x20, #GH_ST]
    ldr   w1, [x20, #GH_DIR]
    eor   w1, w1, #2
    str   w1, [x20, #GH_DIR]
pmf_n:
    add   w19, w19, #1
    cmp   w19, #4
    b.ne  pmf_g
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_unfright: frightened ghosts resume the schedule state
pm_unfright:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
puf_g:
    mov   w0, w19
    bl    pm_gptr
    mov   x20, x0
    ldr   w1, [x20, #GH_ST]
    cmp   w1, #GH_FRIGHT
    b.ne  puf_n
    bl    pm_mode_state
    str   w0, [x20, #GH_ST]
puf_n:
    add   w19, w19, #1
    cmp   w19, #4
    b.ne  puf_g
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_fright_on: pellet eaten — SCAT/CHASE ghosts turn frightened + reverse
pm_fright_on:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x0, =pm_fright
    mov   w1, #PM_FRIGHTF
    str   w1, [x0]
    ldr   x0, =pm_kills
    str   wzr, [x0]
    mov   w19, #0
pfo_g:
    mov   w0, w19
    bl    pm_gptr
    mov   x20, x0
    ldr   w1, [x20, #GH_ST]
    cmp   w1, #GH_SCAT
    b.eq  pfo_set
    cmp   w1, #GH_CHASE
    b.ne  pfo_n
pfo_set:
    mov   w1, #GH_FRIGHT
    str   w1, [x20, #GH_ST]
    ldr   w1, [x20, #GH_DIR]
    eor   w1, w1, #2
    str   w1, [x20, #GH_DIR]
pfo_n:
    add   w19, w19, #1
    cmp   w19, #4
    b.ne  pfo_g
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// ---------------------------------------------------------------- pac
pac_step:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    // try the queued turn
    ldr   x0, =pm_pnext
    ldr   w19, [x0]
    ldr   x1, =pm_dx
    ldr   w0, [x1, w19, uxtw #2]
    ldr   x2, =pm_px
    ldr   w2, [x2]
    add   w0, w0, w2
    ldr   x1, =pm_dy
    ldr   w1, [x1, w19, uxtw #2]
    ldr   x2, =pm_py
    ldr   w2, [x2]
    add   w1, w1, w2
    mov   w2, #0
    mov   w3, #0
    bl    pm_walkable
    cbz   w0, ps_keep
    ldr   x0, =pm_pdir
    str   w19, [x0]
ps_keep:
    // move along pdir if walkable
    ldr   x0, =pm_pdir
    ldr   w19, [x0]
    ldr   x1, =pm_dx
    ldr   w0, [x1, w19, uxtw #2]
    ldr   x2, =pm_px
    ldr   w2, [x2]
    add   w20, w0, w2                     // nx (pre-wrap)
    ldr   x1, =pm_dy
    ldr   w1, [x1, w19, uxtw #2]
    ldr   x2, =pm_py
    ldr   w2, [x2]
    add   w19, w1, w2                     // ny
    mov   w0, w20
    mov   w1, w19
    mov   w2, #0
    mov   w3, #0
    bl    pm_walkable
    cbz   w0, ps_done
    // record old, wrap, commit
    ldr   x0, =pm_px
    ldr   w1, [x0]
    ldr   x2, =pm_opx
    str   w1, [x2]
    ldr   x0, =pm_py
    ldr   w1, [x0]
    ldr   x2, =pm_opy
    str   w1, [x2]
    cmp   w20, #0
    b.ge  ps_w1
    add   w20, w20, #PM_COLS
ps_w1:
    cmp   w20, #PM_COLS
    b.lt  ps_w2
    sub   w20, w20, #PM_COLS
ps_w2:
    ldr   x0, =pm_px
    str   w20, [x0]
    ldr   x0, =pm_py
    str   w19, [x0]
    ldr   x0, =pf_pm
    mov   w1, #1
    str   w1, [x0]
    // eat the tile under pac
    mov   w0, #PM_COLS
    mul   w0, w19, w0
    add   w0, w0, w20
    ldr   x1, =pm_maze
    ldrb  w2, [x1, w0, uxtw]
    cmp   w2, #2
    b.eq  ps_dot
    cmp   w2, #3
    b.eq  ps_pow
    b     ps_coll
ps_dot:
    strb  wzr, [x1, w0, uxtw]
    ldr   x2, =pm_score
    ldr   w3, [x2]
    add   w3, w3, #10
    str   w3, [x2]
    b     ps_dec
ps_pow:
    strb  wzr, [x1, w0, uxtw]
    ldr   x2, =pm_score
    ldr   w3, [x2]
    add   w3, w3, #50
    str   w3, [x2]
    bl    pm_fright_on
ps_dec:
    ldr   x2, =pm_dots
    ldr   w3, [x2]
    sub   w3, w3, #1
    str   w3, [x2]
    cbnz  w3, ps_coll
    // level clear
    ldr   x2, =pm_level
    ldr   w3, [x2]
    add   w3, w3, #1
    str   w3, [x2]
    bl    pm_load_maze
    bl    pm_reset_actors
    ldr   x0, =pm_state
    mov   w1, #PMS_READY
    str   w1, [x0]
    ldr   x0, =pm_statet
    mov   w1, #PM_READYF
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    b     ps_done
ps_coll:
    bl    pm_collide_all
ps_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_collide_all: pac vs every ghost (after a pac move)
pm_collide_all:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
pca_g:
    mov   w0, w19
    bl    pm_gptr
    mov   x20, x0
    ldr   w1, [x20, #GH_X]
    ldr   x2, =pm_px
    ldr   w2, [x2]
    cmp   w1, w2
    b.ne  pca_n
    ldr   w1, [x20, #GH_Y]
    ldr   x2, =pm_py
    ldr   w2, [x2]
    cmp   w1, w2
    b.ne  pca_n
    mov   x0, x20
    bl    pm_touch
    ldr   x0, =pm_state                   // a kill stops the sweep
    ldr   w0, [x0]
    cmp   w0, #PMS_PLAY
    b.ne  pca_done
pca_n:
    add   w19, w19, #1
    cmp   w19, #4
    b.ne  pca_g
pca_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_touch: x0 = ghost sharing pac's tile
pm_touch:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   x19, x0
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_FRIGHT
    b.eq  pt_eat
    cmp   w1, #GH_EATEN
    b.eq  pt_done
    bl    pm_kill
    b     pt_done
pt_eat:
    mov   w1, #GH_EATEN
    str   w1, [x19, #GH_ST]
    ldr   x0, =pm_kills
    ldr   w1, [x0]
    mov   w2, #200
    lsl   w2, w2, w1                      // 200 << kills
    ldr   x3, =pm_score
    ldr   w4, [x3]
    add   w4, w4, w2
    str   w4, [x3]
    cmp   w1, #3
    b.ge  pt_done
    add   w1, w1, #1
    str   w1, [x0]
pt_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_kill: pac dies
pm_kill:
    stp   x29, x30, [sp, #-16]!
    ldr   x2, =pm_lives
    ldr   w0, [x2]
    sub   w0, w0, #1
    str   w0, [x2]
    cmp   w0, #0
    b.gt  pk_alive
    ldr   x0, =pm_state
    mov   w1, #PMS_OVER
    str   w1, [x0]
    ldr   x0, =pm_score
    ldr   w0, [x0]
    ldr   x1, =pm_hi
    ldr   w2, [x1]
    cmp   w0, w2
    b.lt  pk_dirty
    str   w0, [x1]
    b     pk_dirty
pk_alive:
    bl    pm_reset_actors
    ldr   x0, =pm_state
    mov   w1, #PMS_READY
    str   w1, [x0]
    ldr   x0, =pm_statet
    mov   w1, #PM_READYF
    str   w1, [x0]
pk_dirty:
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// ---------------------------------------------------------------- ghosts
// ghost_step: w0 = ghost index — one tile-step (house / steer / move / touch)
ghost_step:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    bl    pm_gptr
    mov   x19, x0
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_HOUSE
    b.ne  gs_active
    // house: count down, then release at the door tile
    ldr   w1, [x19, #GH_TMR]
    sub   w1, w1, #1
    str   w1, [x19, #GH_TMR]
    cmp   w1, #0
    b.gt  gs_done
    ldr   w1, [x19, #GH_X]
    str   w1, [x19, #GH_OX]
    ldr   w1, [x19, #GH_Y]
    str   w1, [x19, #GH_OY]
    mov   w1, #14
    str   w1, [x19, #GH_X]
    mov   w1, #10
    str   w1, [x19, #GH_Y]
    mov   w1, #1
    str   w1, [x19, #GH_DIR]
    bl    pm_mode_state
    str   w0, [x19, #GH_ST]
    ldr   x0, =pf_pm
    mov   w1, #1
    str   w1, [x0]
    b     gs_done
gs_active:
    // frightened ghosts move every other ghost tick (half speed)
    cmp   w1, #GH_FRIGHT
    b.ne  gs_home
    ldr   x0, =pm_gtick
    ldr   w0, [x0]
    tst   w0, #1
    b.ne  gs_done
gs_home:
    // an eaten ghost arriving home resumes the schedule
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_EATEN
    b.ne  gs_steer
    ldr   w1, [x19, #GH_X]
    cmp   w1, #14
    b.ne  gs_steer
    ldr   w1, [x19, #GH_Y]
    cmp   w1, #10
    b.ne  gs_steer
    bl    pm_mode_state
    str   w0, [x19, #GH_ST]
gs_steer:
    bl    pm_steer                        // x19 = ghost
    // move along GH_DIR if walkable
    ldr   w20, [x19, #GH_DIR]
    ldr   x1, =pm_dx
    ldr   w0, [x1, w20, uxtw #2]
    ldr   w2, [x19, #GH_X]
    add   w21, w0, w2                     // nx
    ldr   x1, =pm_dy
    ldr   w1, [x1, w20, uxtw #2]
    ldr   w2, [x19, #GH_Y]
    add   w22, w1, w2                     // ny
    ldr   w3, [x19, #GH_ST]
    cmp   w3, #GH_EATEN
    cset  w3, eq
    mov   w0, w21
    mov   w1, w22
    mov   w2, #1
    bl    pm_walkable
    cbz   w0, gs_touch
    ldr   w1, [x19, #GH_X]
    str   w1, [x19, #GH_OX]
    ldr   w1, [x19, #GH_Y]
    str   w1, [x19, #GH_OY]
    cmp   w21, #0
    b.ge  gs_w1
    add   w21, w21, #PM_COLS
gs_w1:
    cmp   w21, #PM_COLS
    b.lt  gs_w2
    sub   w21, w21, #PM_COLS
gs_w2:
    str   w21, [x19, #GH_X]
    str   w22, [x19, #GH_Y]
    ldr   x0, =pf_pm
    mov   w1, #1
    str   w1, [x0]
gs_touch:
    ldr   w1, [x19, #GH_X]
    ldr   x2, =pm_px
    ldr   w2, [x2]
    cmp   w1, w2
    b.ne  gs_done
    ldr   w1, [x19, #GH_Y]
    ldr   x2, =pm_py
    ldr   w2, [x2]
    cmp   w1, w2
    b.ne  gs_done
    mov   x0, x19
    bl    pm_touch
gs_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_steer: x19 = ghost ptr, pm_gi = its index — pick GH_DIR at a tile
pm_steer:
    stp   x29, x30, [sp, #-16]!
    stp   x20, x21, [sp, #-16]!
    stp   x22, x23, [sp, #-16]!
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_FRIGHT
    b.ne  st_target
    // frightened: a random non-reverse walkable direction (donor's 8 tries)
    mov   w20, #8
st_rnd:
    bl    rng
    and   w21, w0, #3
    ldr   w1, [x19, #GH_DIR]
    eor   w1, w1, #2
    cmp   w21, w1
    b.eq  st_rtry
    ldr   x1, =pm_dx
    ldr   w0, [x1, w21, uxtw #2]
    ldr   w2, [x19, #GH_X]
    add   w0, w0, w2
    ldr   x1, =pm_dy
    ldr   w1, [x1, w21, uxtw #2]
    ldr   w2, [x19, #GH_Y]
    add   w1, w1, w2
    mov   w2, #1
    mov   w3, #0
    bl    pm_walkable
    cbz   w0, st_rtry
    str   w21, [x19, #GH_DIR]
    b     st_out
st_rtry:
    subs  w20, w20, #1
    b.ne  st_rnd
    b     st_out
st_target:
    // pick the target tile -> pm_tgx/pm_tgy
    cmp   w1, #GH_EATEN
    b.ne  st_ne
    mov   w2, #14                         // home door
    mov   w3, #10
    b     st_have
st_ne:
    cmp   w1, #GH_CHASE
    b.ne  st_scatter
    ldr   x0, =pm_gi
    ldr   w0, [x0]
    cbz   w0, st_blinky
    cmp   w0, #1
    b.eq  st_pinky
    // ghost 2 (Clyde, radius 8) / ghost 3 (close-range variant, radius 4):
    // far from pac -> chase pac, near -> its scatter corner
    ldr   x1, =pm_px
    ldr   w2, [x1]
    ldr   w3, [x19, #GH_X]
    subs  w3, w3, w2
    cneg  w3, w3, mi
    ldr   x1, =pm_py
    ldr   w4, [x1]
    ldr   w5, [x19, #GH_Y]
    subs  w5, w5, w4
    cneg  w5, w5, mi
    add   w3, w3, w5                      // manhattan(ghost, pac)
    cmp   w0, #2
    mov   w5, #8
    mov   w6, #4
    csel  w5, w5, w6, eq                  // radius
    cmp   w3, w5
    b.gt  st_blinky                       // far: direct chase
st_scatter:
    ldr   x0, =pm_gi
    ldr   w0, [x0]
    ldr   x1, =pm_cornx
    ldr   w2, [x1, w0, uxtw #2]
    ldr   x1, =pm_corny
    ldr   w3, [x1, w0, uxtw #2]
    b     st_have
st_blinky:
    ldr   x1, =pm_px
    ldr   w2, [x1]
    ldr   x1, =pm_py
    ldr   w3, [x1]
    b     st_have
st_pinky:
    // 4 tiles ahead of pac
    ldr   x0, =pm_pdir
    ldr   w0, [x0]
    ldr   x1, =pm_dx
    ldr   w2, [x1, w0, uxtw #2]
    lsl   w2, w2, #2
    ldr   x1, =pm_px
    ldr   w4, [x1]
    add   w2, w2, w4
    ldr   x1, =pm_dy
    ldr   w3, [x1, w0, uxtw #2]
    lsl   w3, w3, #2
    ldr   x1, =pm_py
    ldr   w4, [x1]
    add   w3, w3, w4
st_have:
    ldr   x0, =pm_tgx
    str   w2, [x0]
    ldr   x0, =pm_tgy
    str   w3, [x0]
    // best = argmin manhattan over walkable non-reverse dirs
    ldr   x0, =pm_bestd
    mov   w1, #32000
    str   w1, [x0]
    ldr   x0, =pm_bdir
    mov   w1, #-1
    str   w1, [x0]
    mov   w20, #0                         // candidate dir
st_try:
    ldr   w1, [x19, #GH_DIR]
    eor   w1, w1, #2
    cmp   w20, w1
    b.eq  st_skip
    ldr   x1, =pm_dx
    ldr   w0, [x1, w20, uxtw #2]
    ldr   w2, [x19, #GH_X]
    add   w21, w0, w2                     // nx
    ldr   x1, =pm_dy
    ldr   w1, [x1, w20, uxtw #2]
    ldr   w2, [x19, #GH_Y]
    add   w22, w1, w2                     // ny
    ldr   w3, [x19, #GH_ST]
    cmp   w3, #GH_EATEN
    cset  w3, eq
    mov   w0, w21
    mov   w1, w22
    mov   w2, #1
    bl    pm_walkable
    cbz   w0, st_skip
    // wrap nx for the distance
    cmp   w21, #0
    b.ge  st_wx1
    add   w21, w21, #PM_COLS
st_wx1:
    cmp   w21, #PM_COLS
    b.lt  st_wx2
    sub   w21, w21, #PM_COLS
st_wx2:
    ldr   x0, =pm_tgx
    ldr   w0, [x0]
    subs  w21, w21, w0
    cneg  w21, w21, mi
    ldr   x0, =pm_tgy
    ldr   w0, [x0]
    subs  w22, w22, w0
    cneg  w22, w22, mi
    add   w21, w21, w22                   // dist
    ldr   x0, =pm_bestd
    ldr   w1, [x0]
    cmp   w21, w1
    b.ge  st_skip
    str   w21, [x0]
    ldr   x0, =pm_bdir
    str   w20, [x0]
st_skip:
    add   w20, w20, #1
    cmp   w20, #4
    b.ne  st_try
    ldr   x0, =pm_bdir
    ldr   w0, [x0]
    cmn   w0, #1
    b.eq  st_out
    str   w0, [x19, #GH_DIR]
st_out:
    ldp   x22, x23, [sp], #16
    ldp   x20, x21, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// ---------------------------------------------------------------- rendering
// pm_draw_tile: w0 = tx, w1 = ty — one 16px maze cell
pm_draw_tile:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    lsl   w19, w0, #4
    add   w19, w19, #PM_OXP               // px
    lsl   w20, w1, #4
    add   w20, w20, #PM_OYP               // py
    mov   w2, #PM_COLS
    mul   w1, w1, w2
    add   w1, w1, w0
    ldr   x2, =pm_maze
    ldrb  w21, [x2, w1, uxtw]             // tile byte
    cmp   w21, #1
    b.ne  pdt_open
    mov   w0, #2                          // wall: cyan
    bl    set_fg
    mov   w0, w19
    mov   w1, w20
    mov   w2, #16
    mov   w3, #16
    bl    frect
    b     pdt_done
pdt_open:
    mov   w0, #4                          // open field: black
    bl    set_fg
    mov   w0, w19
    mov   w1, w20
    mov   w2, #16
    mov   w3, #16
    bl    frect
    cmp   w21, #2
    b.eq  pdt_dot
    cmp   w21, #3
    b.eq  pdt_pow
    cmp   w21, #5
    b.eq  pdt_gate
    b     pdt_done
pdt_dot:
    mov   w0, #1
    bl    set_fg
    add   w0, w19, #6
    add   w1, w20, #6
    mov   w2, #4
    mov   w3, #4
    bl    frect
    b     pdt_done
pdt_pow:
    mov   w0, #1
    bl    set_fg
    add   w0, w19, #4
    add   w1, w20, #4
    mov   w2, #8
    mov   w3, #8
    bl    frect
    b     pdt_done
pdt_gate:
    mov   w0, #9
    bl    set_fg
    mov   w0, w19
    add   w1, w20, #6
    mov   w2, #16
    mov   w3, #4
    bl    frect
pdt_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_draw_pac: the yellow square at pac's tile
pm_draw_pac:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #6
    bl    set_fg
    ldr   x0, =pm_px
    ldr   w0, [x0]
    lsl   w0, w0, #4
    add   w0, w0, #(PM_OXP+1)
    ldr   x1, =pm_py
    ldr   w1, [x1]
    lsl   w1, w1, #4
    add   w1, w1, #(PM_OYP+1)
    mov   w2, #14
    mov   w3, #14
    bl    frect
    ldp   x29, x30, [sp], #16
    ret

// pm_draw_ghost: w0 = ghost index — body square + white eyes
pm_draw_ghost:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w22, w0
    bl    pm_gptr
    mov   x19, x0
    ldr   w0, [x19, #GH_X]
    lsl   w20, w0, #4
    add   w20, w20, #PM_OXP
    ldr   w0, [x19, #GH_Y]
    lsl   w21, w0, #4
    add   w21, w21, #PM_OYP
    // body colour by state: fright = 0 (theme field), eaten = 4 (just eyes),
    // else per-ghost (red / magenta / orange / green)
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_FRIGHT
    b.ne  pdg_ne
    mov   w0, #0
    b     pdg_body
pdg_ne:
    cmp   w1, #GH_EATEN
    b.ne  pdg_col
    mov   w0, #4
    b     pdg_body
pdg_col:
    ldr   x0, =pm_gcols
    ldrb  w0, [x0, w22, uxtw]
pdg_body:
    bl    set_fg
    add   w0, w20, #1
    add   w1, w21, #1
    mov   w2, #14
    mov   w3, #14
    bl    frect
    // eyes
    mov   w0, #1
    bl    set_fg
    add   w0, w20, #3
    add   w1, w21, #4
    mov   w2, #3
    mov   w3, #4
    bl    frect
    add   w0, w20, #10
    add   w1, w21, #4
    mov   w2, #3
    mov   w3, #4
    bl    frect
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_draw_actors: pac + the four ghosts
pm_draw_actors:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    bl    pm_draw_pac
    mov   w19, #0
pda_g:
    mov   w0, w19
    bl    pm_draw_ghost
    add   w19, w19, #1
    cmp   w19, #4
    b.ne  pda_g
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_draw_status: the score line under the maze
pm_draw_status:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #96
    mov   w1, #448
    ldr   x2, =s_pm_score
    bl    pstr
    ldr   x0, =pm_score
    ldr   w0, [x0]
    ldr   x1, =str_buf
    bl    fmt_dec
    mov   w0, #148
    mov   w1, #448
    ldr   x2, =str_buf
    bl    pstr
    mov   w0, #256
    mov   w1, #448
    ldr   x2, =s_pm_hi
    bl    pstr
    ldr   x0, =pm_hi
    ldr   w0, [x0]
    ldr   x1, =str_buf
    bl    fmt_dec
    mov   w0, #284
    mov   w1, #448
    ldr   x2, =str_buf
    bl    pstr
    mov   w0, #368
    mov   w1, #448
    ldr   x2, =s_pm_lives
    bl    pstr
    ldr   x0, =pm_lives
    ldr   w0, [x0]
    add   w0, w0, #'0'
    ldr   x1, =str_buf
    strb  w0, [x1]
    strb  wzr, [x1, #1]
    mov   w0, #420
    mov   w1, #448
    ldr   x2, =str_buf
    bl    pstr
    mov   w0, #460
    mov   w1, #448
    ldr   x2, =s_pm_level
    bl    pstr
    ldr   x0, =pm_level
    ldr   w0, [x0]
    ldr   x1, =str_buf
    bl    fmt_dec
    mov   w0, #488
    mov   w1, #448
    ldr   x2, =str_buf
    bl    pstr
    ldp   x29, x30, [sp], #16
    ret

// pm_draw_overlay: READY! / GAME OVER (inverted, centre of the maze)
pm_draw_overlay:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_READY
    b.eq  pdo_ready
    cmp   w0, #PMS_OVER
    b.ne  pdo_done
    mov   w0, #0
    mov   w1, #1
    bl    setfb
    mov   w0, #284
    mov   w1, #276
    ldr   x2, =s_pm_over
    bl    pstr
    b     pdo_done
pdo_ready:
    mov   w0, #0
    mov   w1, #1
    bl    setfb
    mov   w0, #296
    mov   w1, #276
    ldr   x2, =s_pm_ready
    bl    pstr
pdo_done:
    ldp   x29, x30, [sp], #16
    ret

// pacman_draw: the full app screen (under draw_chrome)
pacman_draw:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w20, #0                         // ty
pcd_row:
    mov   w19, #0                         // tx
pcd_col:
    mov   w0, w19
    mov   w1, w20
    bl    pm_draw_tile
    add   w19, w19, #1
    cmp   w19, #PM_COLS
    b.ne  pcd_col
    add   w20, w20, #1
    cmp   w20, #PM_ROWS
    b.ne  pcd_row
    bl    pm_draw_actors
    bl    pm_draw_status
    bl    pm_draw_overlay
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pacman_partial: erase the actors' previous tiles, redraw their current
// tiles, repaint the actors + the score line
pacman_partial:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x0, =pm_opx
    ldr   w0, [x0]
    ldr   x1, =pm_opy
    ldr   w1, [x1]
    bl    pm_draw_tile
    ldr   x0, =pm_px
    ldr   w0, [x0]
    ldr   x1, =pm_py
    ldr   w1, [x1]
    bl    pm_draw_tile
    mov   w19, #0
pcp_g:
    mov   w0, w19
    bl    pm_gptr
    mov   x20, x0
    ldr   w0, [x20, #GH_OX]
    ldr   w1, [x20, #GH_OY]
    bl    pm_draw_tile
    ldr   w0, [x20, #GH_X]
    ldr   w1, [x20, #GH_Y]
    bl    pm_draw_tile
    add   w19, w19, #1
    cmp   w19, #4
    b.ne  pcp_g
    bl    pm_draw_actors
    bl    pm_draw_status
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// ---------------------------------------------------------------- data
.section .rodata
.align 2
pm_dx:      .word 0, -1, 0, 1             // UP LEFT DOWN RIGHT
pm_dy:      .word -1, 0, 1, 0
pm_cornx:   .word 26, 1, 1, 26            // scatter corners per ghost
pm_corny:   .word 1, 1, 23, 23
pm_modedur: .word 420,1200,420,1200,300,1200,300,32000  // frames per phase
// spawn table: x, y, dir, state, house-timer (ghost steps)
pm_gspawn:  .word 14, 10, 1, GH_SCAT,  0
            .word 13, 12, 0, GH_HOUSE, 40
            .word 15, 12, 0, GH_HOUSE, 90
            .word 14, 12, 0, GH_HOUSE, 140
pm_gcols:   .byte 7, 3, 8, 5              // red, magenta, orange, green
s_pm_score: .asciz "Score"
s_pm_hi:    .asciz "Hi"
s_pm_lives: .asciz "Lives"
s_pm_level: .asciz "Lv"
s_pm_ready: .asciz " READY! "
s_pm_over:  .asciz " GAME OVER "
// maze template (genesis/amiga): 0 empty, 1 wall, 2 dot, 3 power, 4 house, 5 gate
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
