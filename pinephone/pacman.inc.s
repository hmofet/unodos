// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — Pac-Man. Ported from the canonical
// genesis/pacman.i (28x25 maze; Blinky direct / Pinky 4-ahead / Clyde hybrid;
// scatter/chase schedule; frightened mode + eat chain). The Genesis carries the
// actors as hardware sprites with pixel sub-steps; the Pi has no sprites, so this
// is TILE-STEPPED: one cell per game step, the whole maze + actors repaint on a
// step (v_dirty). The AI operates on tile coords, so it transfers unchanged.
// Maze cells: 0 empty, 1 wall, 2 dot, 3 power, 4 house, 5 gate.
// Ghost states: 0 house, 1 scatter, 2 chase, 3 frightened, 4 eaten.
// Ghost struct (GSIZE bytes): GX@0 GY@4 GDIR@8 GST@12 GTMR@16.
// ============================================================================

// dirdx / dirdy: w0=dir -> w0 = dx / dy. Leaf.
dirdx:
    ldr   x1, =pm_dx
    ldr   w0, [x1, w0, uxtw #2]
    ret
dirdy:
    ldr   x1, =pm_dy
    ldr   w0, [x1, w0, uxtw #2]
    ret

// pm_mode_state: -> w0 = 2 (chase) if pm_mode odd else 1 (scatter). Leaf.
pm_mode_state:
    ldr   x0, =pm_mode
    ldr   w0, [x0]
    tst   w0, #1
    b.eq  pms_scat
    mov   w0, #2
    ret
pms_scat:
    mov   w0, #1
    ret

// pm_walkable: w0=tx w1=ty w2=ghost? w3=eaten? -> w0=1 ok / 0 blocked. Leaf.
pm_walkable:
    cmp   w1, #0
    b.lt  pw_no
    cmp   w1, #PM_ROWS
    b.ge  pw_no
    cmp   w0, #0
    b.ge  pw_x1
    add   w0, w0, #PM_COLS
pw_x1:
    cmp   w0, #PM_COLS
    b.lt  pw_x2
    sub   w0, w0, #PM_COLS
pw_x2:
    mov   w4, #PM_COLS
    mul   w5, w1, w4
    add   w5, w5, w0
    ldr   x6, =pm_maze
    ldrb  w4, [x6, w5, uxtw]
    cmp   w4, #1
    b.eq  pw_no
    cmp   w4, #4
    b.eq  pw_gate
    cmp   w4, #5
    b.eq  pw_gate
    mov   w0, #1
    ret
pw_gate:
    cbz   w2, pw_no
    cbz   w3, pw_no
    mov   w0, #1
    ret
pw_no:
    mov   w0, #0
    ret

// pm_load_maze: copy template -> pm_maze, count dots/powers into pm_dots.
pm_load_maze:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =pm_maze_tpl
    ldr   x1, =pm_maze
    mov   w2, #(PM_COLS*PM_ROWS)
    mov   w3, #0
plm_l:
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
    b.ne  plm_l
    ldr   x0, =pm_dots
    str   w3, [x0]
    ldp   x29, x30, [sp], #16
    ret

// gh_ptr: w0 = ghost index -> x0 = &pm_gh[index]. Leaf.
gh_ptr:
    ldr   x1, =pm_gh
    mov   w2, #GSIZE
    mul   w0, w0, w2
    add   x0, x1, w0, uxtw
    ret

// pm_reset_actors: place pac + 3 ghosts (tile coords).
pm_reset_actors:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #14
    ldr   x1, =pm_x
    str   w0, [x1]
    mov   w0, #19
    ldr   x1, =pm_y
    str   w0, [x1]
    mov   w0, #1                          // LEFT
    ldr   x1, =pm_dir
    str   w0, [x1]
    ldr   x1, =pm_ndir
    str   w0, [x1]
    // ghost 0 (Blinky): (14,10) LEFT scatter, out immediately
    mov   w0, #0
    bl    gh_ptr
    mov   w1, #14
    str   w1, [x0, #0]
    mov   w1, #10
    str   w1, [x0, #4]
    mov   w1, #1
    str   w1, [x0, #8]
    mov   w1, #1                          // scatter
    str   w1, [x0, #12]
    str   wzr, [x0, #16]
    // ghost 1 (Pinky): (13,12) house, timer 20
    mov   w0, #1
    bl    gh_ptr
    mov   w1, #13
    str   w1, [x0, #0]
    mov   w1, #12
    str   w1, [x0, #4]
    str   wzr, [x0, #8]
    str   wzr, [x0, #12]                  // house
    mov   w1, #20
    str   w1, [x0, #16]
    // ghost 2 (Clyde): (15,12) house, timer 40
    mov   w0, #2
    bl    gh_ptr
    mov   w1, #15
    str   w1, [x0, #0]
    mov   w1, #12
    str   w1, [x0, #4]
    str   wzr, [x0, #8]
    str   wzr, [x0, #12]
    mov   w1, #40
    str   w1, [x0, #16]
    ldr   x0, =pm_fr
    str   wzr, [x0]
    ldr   x0, =pm_kills
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret

// pm_new_game / pacman_init
pacman_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =pm_score
    str   wzr, [x0]
    mov   w1, #3
    ldr   x0, =pm_lives
    str   w1, [x0]
    mov   w1, #1
    ldr   x0, =pm_level
    str   w1, [x0]
    ldr   x0, =pm_mode
    str   wzr, [x0]
    ldr   x0, =pm_modet
    str   wzr, [x0]
    ldr   x0, =pm_st
    str   wzr, [x0]
    ldr   x0, =pm_sc
    str   wzr, [x0]
    ldr   x0, =pm_ft
    str   wzr, [x0]
    // seed the shared rng from the system timer
    mrs   x1, cntpct_el0                  // seed from a changing source
    orr   w1, w1, #1
    ldr   x0, =g_seed
    str   w1, [x0]
    bl    pm_load_maze
    bl    pm_reset_actors
    ldp   x29, x30, [sp], #16
    ret

// pm_for_each helpers: reverse SCAT/CHASE on schedule flip, enter/leave fright.
// pm_frighten: SCAT/CHASE ghosts -> FRIGHT + reverse.
pm_frighten:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
pf_l:
    cmp   w19, #3
    b.hs  pf_done
    mov   w0, w19
    bl    gh_ptr
    mov   x20, x0
    ldr   w0, [x20, #12]
    cmp   w0, #1
    b.eq  pf_set
    cmp   w0, #2
    b.ne  pf_nx
pf_set:
    mov   w0, #3
    str   w0, [x20, #12]
    ldr   w0, [x20, #8]
    eor   w0, w0, #2
    str   w0, [x20, #8]
pf_nx:
    add   w19, w19, #1
    b     pf_l
pf_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_unfright: FRIGHT ghosts -> mode_state.
pm_unfright:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
puf_l:
    cmp   w19, #3
    b.hs  puf_done
    mov   w0, w19
    bl    gh_ptr
    mov   x20, x0
    ldr   w0, [x20, #12]
    cmp   w0, #3
    b.ne  puf_nx
    bl    pm_mode_state
    str   w0, [x20, #12]
puf_nx:
    add   w19, w19, #1
    b     puf_l
puf_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_modeflip: SCAT/CHASE ghosts -> new mode_state + reverse.
pm_modeflip:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
pmf_l:
    cmp   w19, #3
    b.hs  pmf_done
    mov   w0, w19
    bl    gh_ptr
    mov   x20, x0
    ldr   w0, [x20, #12]
    cmp   w0, #1
    b.eq  pmf_set
    cmp   w0, #2
    b.ne  pmf_nx
pmf_set:
    bl    pm_mode_state
    str   w0, [x20, #12]
    ldr   w0, [x20, #8]
    eor   w0, w0, #2
    str   w0, [x20, #8]
pmf_nx:
    add   w19, w19, #1
    b     pmf_l
pmf_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_kill: pac dies.
pm_kill:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =pm_lives
    ldr   w1, [x0]
    sub   w1, w1, #1
    str   w1, [x0]
    cmp   w1, #0
    b.gt  pk_alive
    ldr   x0, =pm_st
    mov   w1, #1
    str   w1, [x0]
    b     pk_out
pk_alive:
    bl    pm_reset_actors
pk_out:
    ldp   x29, x30, [sp], #16
    ret

// pm_steer: w0 = ghost index. Pick GDIR at the current tile (the canonical AI).
pm_steer:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    stp   x25, x26, [sp, #-16]!
    stp   x27, x28, [sp, #-16]!
    mov   w19, w0                         // gidx
    bl    gh_ptr
    mov   x20, x0                         // ghost ptr
    ldr   w23, [x20, #0]                  // gtx
    ldr   w24, [x20, #4]                  // gty
    ldr   w0, [x20, #12]                  // gst
    cmp   w0, #3                          // FRIGHT -> random
    b.ne  ps_target
    mov   w21, #8
ps_rnd:
    bl    rng
    and   w22, w0, #3                     // candidate dir
    ldr   w1, [x20, #8]
    eor   w1, w1, #2
    cmp   w22, w1
    b.eq  ps_rtry
    mov   w0, w22
    bl    dirdx
    add   w25, w0, w23                    // nx
    mov   w0, w22
    bl    dirdy
    add   w26, w0, w24                    // ny
    mov   w0, w25
    mov   w1, w26
    mov   w2, #1
    mov   w3, #0
    bl    pm_walkable
    cbz   w0, ps_rtry
    str   w22, [x20, #8]
    b     ps_out
ps_rtry:
    subs  w21, w21, #1
    b.ne  ps_rnd
    b     ps_out

ps_target:
    cmp   w0, #4                          // EATEN -> home (14,10)
    b.ne  ps_note
    mov   w0, #14
    ldr   x1, =pm_tgx
    str   w0, [x1]
    mov   w0, #10
    ldr   x1, =pm_tgy
    str   w0, [x1]
    b     ps_pick
ps_note:
    cmp   w0, #2                          // CHASE?
    b.ne  ps_scat
    ldr   x1, =pm_x
    ldr   w2, [x1]                        // pac tx
    ldr   x1, =pm_y
    ldr   w3, [x1]                        // pac ty
    cbz   w19, ps_blinky
    cmp   w19, #1
    b.eq  ps_pinky
    // Clyde: manhattan(ghost,pac) <= 8 ? corner : pac
    sub   w4, w23, w2
    cmp   w4, #0
    b.ge  ps_cx
    neg   w4, w4
ps_cx:
    sub   w5, w24, w3
    cmp   w5, #0
    b.ge  ps_cy
    neg   w5, w5
ps_cy:
    add   w4, w4, w5
    cmp   w4, #8
    b.gt  ps_blinky
    b     ps_scat
ps_pinky:
    ldr   x1, =pm_dir
    ldr   w4, [x1]                        // pac dir
    mov   w0, w4
    bl    dirdx
    lsl   w0, w0, #2                      // *4
    add   w0, w0, w2
    ldr   x1, =pm_tgx
    str   w0, [x1]
    ldr   x1, =pm_dir
    ldr   w4, [x1]
    mov   w0, w4
    bl    dirdy
    lsl   w0, w0, #2
    add   w0, w0, w3
    ldr   x1, =pm_tgy
    str   w0, [x1]
    b     ps_pick
ps_blinky:
    ldr   x1, =pm_tgx
    str   w2, [x1]
    ldr   x1, =pm_tgy
    str   w3, [x1]
    b     ps_pick
ps_scat:
    ldr   x1, =pm_cornx
    ldr   w0, [x1, w19, uxtw #2]
    ldr   x1, =pm_tgx
    str   w0, [x1]
    ldr   x1, =pm_corny
    ldr   w0, [x1, w19, uxtw #2]
    ldr   x1, =pm_tgy
    str   w0, [x1]

ps_pick:
    ldr   w27, [x20, #8]
    eor   w27, w27, #2                    // reverse dir (excluded)
    mov   w21, #-1                        // best dir
    mov   w22, #0x7FFF                    // best dist
    mov   w25, #0                         // dir
ps_try:
    cmp   w25, w27
    b.eq  ps_skip
    mov   w0, w25
    bl    dirdx
    add   w26, w0, w23                    // nx
    mov   w0, w25
    bl    dirdy
    add   w28, w0, w24                    // ny
    mov   w0, w26
    mov   w1, w28
    mov   w2, #1
    ldr   w3, [x20, #12]
    cmp   w3, #4
    cset  w3, eq                          // eaten?
    bl    pm_walkable
    cbz   w0, ps_skip
    mov   w0, w26                         // wrap nx for distance
    cmp   w0, #0
    b.ge  ps_w1
    add   w0, w0, #PM_COLS
ps_w1:
    cmp   w0, #PM_COLS
    b.lt  ps_w2
    sub   w0, w0, #PM_COLS
ps_w2:
    ldr   x1, =pm_tgx
    ldr   w1, [x1]
    sub   w0, w0, w1
    cmp   w0, #0
    b.ge  ps_a1
    neg   w0, w0
ps_a1:
    ldr   x1, =pm_tgy
    ldr   w1, [x1]
    sub   w1, w28, w1
    cmp   w1, #0
    b.ge  ps_a2
    neg   w1, w1
ps_a2:
    add   w0, w0, w1                      // manhattan dist
    cmp   w0, w22
    b.ge  ps_skip
    mov   w22, w0
    mov   w21, w25
ps_skip:
    add   w25, w25, #1
    cmp   w25, #4
    b.lt  ps_try
    cmp   w21, #0
    b.lt  ps_out
    str   w21, [x20, #8]
ps_out:
    ldp   x27, x28, [sp], #16
    ldp   x25, x26, [sp], #16
    ldp   x23, x24, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_step: one tile-stepped game step.
pm_step:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    ldr   x0, =pm_st
    ldr   w0, [x0]
    cbnz  w0, pstep_out
    ldr   x0, =pm_sc
    ldr   w1, [x0]
    add   w1, w1, #1
    str   w1, [x0]
    // ---- fright / mode schedule ----
    ldr   x0, =pm_fr
    ldr   w2, [x0]
    cbz   w2, pstep_mode
    sub   w2, w2, #1
    str   w2, [x0]
    cbnz  w2, pstep_pac
    bl    pm_unfright
    b     pstep_pac
pstep_mode:
    ldr   x0, =pm_modet
    ldr   w2, [x0]
    add   w2, w2, #1
    str   w2, [x0]
    ldr   x0, =pm_mode
    ldr   w3, [x0]
    cmp   w3, #7
    b.le  psm_ok
    mov   w3, #7
psm_ok:
    lsl   w3, w3, #2
    ldr   x0, =pm_modedur
    ldr   w0, [x0, w3, uxtw]
    cmp   w2, w0
    b.lt  pstep_pac
    ldr   x0, =pm_modet
    str   wzr, [x0]
    ldr   x0, =pm_mode
    ldr   w3, [x0]
    cmp   w3, #7
    b.ge  psm_nf
    add   w3, w3, #1
    str   w3, [x0]
psm_nf:
    bl    pm_modeflip
pstep_pac:
    // eat at pac's current tile
    ldr   x0, =pm_x
    ldr   w19, [x0]                       // ptx
    ldr   x0, =pm_y
    ldr   w20, [x0]                       // pty
    mov   w0, #PM_COLS
    mul   w3, w20, w0
    add   w3, w3, w19
    ldr   x4, =pm_maze
    ldrb  w5, [x4, w3, uxtw]
    cmp   w5, #2
    b.eq  pstep_dot
    cmp   w5, #3
    b.eq  pstep_pow
    b     pstep_clear
pstep_dot:
    strb  wzr, [x4, w3, uxtw]
    ldr   x0, =pm_score
    ldr   w1, [x0]
    add   w1, w1, #10
    str   w1, [x0]
    ldr   x0, =pm_dots
    ldr   w1, [x0]
    sub   w1, w1, #1
    str   w1, [x0]
    b     pstep_clear
pstep_pow:
    strb  wzr, [x4, w3, uxtw]
    ldr   x0, =pm_score
    ldr   w1, [x0]
    add   w1, w1, #50
    str   w1, [x0]
    ldr   x0, =pm_dots
    ldr   w1, [x0]
    sub   w1, w1, #1
    str   w1, [x0]
    ldr   x0, =pm_fr
    mov   w1, #FRIGHT_STEPS
    str   w1, [x0]
    ldr   x0, =pm_kills
    str   wzr, [x0]
    bl    pm_frighten
pstep_clear:
    ldr   x0, =pm_dots
    ldr   w0, [x0]
    cbnz  w0, pstep_move
    ldr   x0, =pm_level
    ldr   w1, [x0]
    add   w1, w1, #1
    str   w1, [x0]
    bl    pm_load_maze
    bl    pm_reset_actors
    b     pstep_out
pstep_move:
    // try queued dir, else keep dir
    ldr   x0, =pm_ndir
    ldr   w21, [x0]
    mov   w0, w21
    bl    dirdx
    add   w4, w0, w19
    mov   w0, w21
    bl    dirdy
    add   w5, w0, w20
    mov   w0, w4
    mov   w1, w5
    mov   w2, #0
    mov   w3, #0
    bl    pm_walkable
    cbz   w0, pstep_dir
    ldr   x0, =pm_dir
    str   w21, [x0]
pstep_dir:
    ldr   x0, =pm_dir
    ldr   w21, [x0]
    mov   w0, w21
    bl    dirdx
    add   w23, w0, w19                    // nx (callee-saved across pm_walkable)
    mov   w0, w21
    bl    dirdy
    add   w24, w0, w20                    // ny
    mov   w0, w23
    mov   w1, w24
    mov   w2, #0
    mov   w3, #0
    bl    pm_walkable
    cbz   w0, pstep_ghosts
    cmp   w23, #0
    b.ge  pmv_w1
    mov   w23, #(PM_COLS-1)
pmv_w1:
    cmp   w23, #PM_COLS
    b.lt  pmv_w2
    mov   w23, #0
pmv_w2:
    ldr   x0, =pm_x
    str   w23, [x0]
    ldr   x0, =pm_y
    str   w24, [x0]
pstep_ghosts:
    mov   w19, #0
pstep_gl:
    mov   w0, w19
    bl    gh_ptr
    mov   x20, x0
    ldr   w0, [x20, #12]
    cbnz  w0, pstep_gactive
    // house: countdown then emerge
    ldr   w0, [x20, #16]
    sub   w0, w0, #1
    str   w0, [x20, #16]
    cmp   w0, #0
    b.gt  pstep_gnext
    mov   w0, #14
    str   w0, [x20, #0]
    mov   w0, #10
    str   w0, [x20, #4]
    mov   w0, #1
    str   w0, [x20, #8]
    bl    pm_mode_state
    str   w0, [x20, #12]
    b     pstep_gnext
pstep_gactive:
    cmp   w0, #4                          // eaten arriving home -> resume
    b.ne  pstep_gsteer
    ldr   w1, [x20, #0]
    cmp   w1, #14
    b.ne  pstep_gsteer
    ldr   w1, [x20, #4]
    cmp   w1, #10
    b.ne  pstep_gsteer
    bl    pm_mode_state
    str   w0, [x20, #12]
pstep_gsteer:
    mov   w0, w19
    bl    pm_steer
    // frightened ghosts move at half speed (even steps only)
    ldr   w0, [x20, #12]
    cmp   w0, #3
    b.ne  pstep_gmove
    ldr   x0, =pm_sc
    ldr   w0, [x0]
    tst   w0, #1
    b.ne  pstep_gcoll
pstep_gmove:
    ldr   w21, [x20, #8]                  // gdir
    ldr   w22, [x20, #0]                  // gx
    mov   w0, w21
    bl    dirdx
    add   w23, w0, w22                    // nx (callee-saved across pm_walkable)
    mov   w0, w21
    bl    dirdy
    ldr   w1, [x20, #4]                   // gy
    add   w24, w0, w1                     // ny
    mov   w0, w23
    mov   w1, w24
    mov   w2, #1
    ldr   w3, [x20, #12]
    cmp   w3, #4
    cset  w3, eq
    bl    pm_walkable
    cbz   w0, pstep_gcoll
    cmp   w23, #0
    b.ge  pgm_w1
    mov   w23, #(PM_COLS-1)
pgm_w1:
    cmp   w23, #PM_COLS
    b.lt  pgm_w2
    mov   w23, #0
pgm_w2:
    str   w23, [x20, #0]
    str   w24, [x20, #4]
pstep_gcoll:
    ldr   w0, [x20, #0]
    ldr   x1, =pm_x
    ldr   w1, [x1]
    cmp   w0, w1
    b.ne  pstep_gnext
    ldr   w0, [x20, #4]
    ldr   x1, =pm_y
    ldr   w1, [x1]
    cmp   w0, w1
    b.ne  pstep_gnext
    ldr   w0, [x20, #12]
    cmp   w0, #3                          // frightened -> eat ghost
    b.ne  pstep_notfr
    mov   w0, #4
    str   w0, [x20, #12]
    ldr   x0, =pm_kills
    ldr   w1, [x0]
    mov   w0, #200
    lsl   w0, w0, w1
    ldr   x2, =pm_score
    ldr   w3, [x2]
    add   w3, w3, w0
    str   w3, [x2]
    ldr   x0, =pm_kills
    ldr   w1, [x0]
    cmp   w1, #3
    b.ge  pstep_gnext
    add   w1, w1, #1
    str   w1, [x0]
    b     pstep_gnext
pstep_notfr:
    cmp   w0, #4                          // eaten ghost: harmless
    b.eq  pstep_gnext
    bl    pm_kill
    b     pstep_out
pstep_gnext:
    add   w19, w19, #1
    cmp   w19, #3
    b.lt  pstep_gl
pstep_out:
    ldp   x23, x24, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pacman_update: input + step pacing (called each frame for app 9).
pacman_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pade
    ldr   w4, [x0]
    tst   w4, #PAD_U
    b.eq  pmu_l
    ldr   x0, =pm_ndir
    str   wzr, [x0]
pmu_l:
    tst   w4, #PAD_L
    b.eq  pmu_d
    ldr   x0, =pm_ndir
    mov   w1, #1
    str   w1, [x0]
pmu_d:
    tst   w4, #PAD_D
    b.eq  pmu_r
    ldr   x0, =pm_ndir
    mov   w1, #2
    str   w1, [x0]
pmu_r:
    tst   w4, #PAD_R
    b.eq  pmu_pace
    ldr   x0, =pm_ndir
    mov   w1, #3
    str   w1, [x0]
pmu_pace:
    ldr   x0, =a_gpause                   // AUTOTEST script ended -> freeze
    ldr   w0, [x0]
    cbnz  w0, pmu_done
    ldr   x0, =pm_ft
    ldr   w1, [x0]
    add   w1, w1, #1
    str   w1, [x0]
    cmp   w1, #PM_STEPFRAMES
    b.lo  pmu_done
    ldr   x0, =pm_ft
    str   wzr, [x0]
    bl    pm_step
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
pmu_done:
    ldp   x29, x30, [sp], #16
    ret

// ---- rendering -------------------------------------------------------------
// pm_draw_cell: w0=col w1=row w2=colour -> full 16px cell.
pm_draw_cell:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    mov   w0, w2
    bl    set_fg
    mov   w0, #PM_CELL
    mul   w0, w19, w0
    add   w0, w0, #PMO_X
    mov   w1, #PM_CELL
    mul   w1, w20, w1
    add   w1, w1, #PMO_Y
    mov   w2, #PM_CELL
    mov   w3, #PM_CELL
    bl    frect
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_draw_dot: w0=col w1=row w2=colour w3=size -> centred square.
pm_draw_dot:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    mov   w21, w3
    mov   w0, w2
    bl    set_fg
    mov   w0, #PM_CELL
    mul   w0, w19, w0
    add   w0, w0, #PMO_X
    mov   w4, #PM_CELL
    sub   w4, w4, w21
    lsr   w4, w4, #1
    add   w0, w0, w4
    mov   w1, #PM_CELL
    mul   w1, w20, w1
    add   w1, w1, #PMO_Y
    mov   w4, #PM_CELL
    sub   w4, w4, w21
    lsr   w4, w4, #1
    add   w1, w1, w4
    mov   w2, w21
    mov   w3, w21
    bl    frect
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

pm_draw_maze:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, #0                         // row
pdm_row:
    cmp   w19, #PM_ROWS
    b.hs  pdm_done
    mov   w20, #0                         // col
pdm_col:
    cmp   w20, #PM_COLS
    b.hs  pdm_rowend
    mov   w0, #PM_COLS
    mul   w0, w19, w0
    add   w0, w0, w20
    ldr   x1, =pm_maze
    ldrb  w21, [x1, w0, uxtw]
    cmp   w21, #1
    b.ne  pdm_dot
    mov   w0, w20
    mov   w1, w19
    mov   w2, #2                          // wall = cyan
    bl    pm_draw_cell
    b     pdm_next
pdm_dot:
    cmp   w21, #2
    b.ne  pdm_pow
    mov   w0, w20
    mov   w1, w19
    mov   w2, #1
    mov   w3, #4                          // dot
    bl    pm_draw_dot
    b     pdm_next
pdm_pow:
    cmp   w21, #3
    b.ne  pdm_next
    mov   w0, w20
    mov   w1, w19
    mov   w2, #1
    mov   w3, #10                         // power pellet
    bl    pm_draw_dot
pdm_next:
    add   w20, w20, #1
    b     pdm_col
pdm_rowend:
    add   w19, w19, #1
    b     pdm_row
pdm_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

pm_draw_actors:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x0, =pm_x
    ldr   w0, [x0]
    ldr   x1, =pm_y
    ldr   w1, [x1]
    mov   w2, #6                          // pac = yellow
    bl    pm_draw_cell
    mov   w19, #0
pda_gl:
    cmp   w19, #3
    b.hs  pda_done
    mov   w0, w19
    bl    gh_ptr
    mov   x20, x0
    ldr   w0, [x20, #12]
    cmp   w0, #3
    b.ne  pda_ne
    mov   w21, #1                         // fright = white
    b     pda_col
pda_ne:
    cmp   w0, #4
    b.ne  pda_body
    mov   w21, #9                         // eaten = grey
    b     pda_col
pda_body:
    ldr   x1, =pm_ghcol
    ldrb  w21, [x1, w19, uxtw]
pda_col:
    ldr   w0, [x20, #0]
    ldr   w1, [x20, #4]
    mov   w2, w21
    bl    pm_draw_cell
    add   w19, w19, #1
    b     pda_gl
pda_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_itoa5: w0 = value -> numstr (5 digits + NUL). Leaf.
pm_itoa5:
    ldr   x3, =numstr
    mov   w1, #10000
    udiv  w2, w0, w1
    msub  w0, w2, w1, w0
    add   w2, w2, #'0'
    strb  w2, [x3, #0]
    mov   w1, #1000
    udiv  w2, w0, w1
    msub  w0, w2, w1, w0
    add   w2, w2, #'0'
    strb  w2, [x3, #1]
    mov   w1, #100
    udiv  w2, w0, w1
    msub  w0, w2, w1, w0
    add   w2, w2, #'0'
    strb  w2, [x3, #2]
    mov   w1, #10
    udiv  w2, w0, w1
    msub  w0, w2, w1, w0
    add   w2, w2, #'0'
    strb  w2, [x3, #3]
    add   w0, w0, #'0'
    strb  w0, [x3, #4]
    strb  wzr, [x3, #5]
    ret

pm_draw_panel:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #432
    ldr   x2, =s_pmscore
    bl    pstr
    ldr   x0, =pm_score
    ldr   w0, [x0]
    bl    pm_itoa5
    mov   w0, #120
    mov   w1, #432
    ldr   x2, =numstr
    bl    pstr
    mov   w0, #240
    mov   w1, #432
    ldr   x2, =s_pmlives
    bl    pstr
    ldr   x0, =pm_lives
    ldr   w0, [x0]
    bl    two_digits
    ldr   x2, =numstr
    strb  w1, [x2, #0]
    strb  w0, [x2, #1]
    strb  wzr, [x2, #2]
    mov   w0, #340
    mov   w1, #432
    ldr   x2, =numstr
    bl    pstr
    ldr   x0, =pm_st
    ldr   w0, [x0]
    cbz   w0, pmp_done
    mov   w0, #16
    mov   w1, #452
    ldr   x2, =s_pmover
    bl    pstr
pmp_done:
    ldp   x29, x30, [sp], #16
    ret

// app_pacman: full draw (frame already pushed by draw_app).
app_pacman:
    ldr   x2, =t_pacman
    bl    draw_chrome
    bl    pm_draw_maze
    bl    pm_draw_actors
    bl    pm_draw_panel
    ldp   x29, x30, [sp], #16
    ret

// ---- data ------------------------------------------------------------------
.section .rodata
.align 2
pm_dx:     .word 0, -1, 0, 1                // UP LEFT DOWN RIGHT
pm_dy:     .word -1, 0, 1, 0
pm_cornx:  .word 26, 1, 1
pm_corny:  .word 1, 1, 23
pm_modedur:.word 127, 364, 127, 364, 91, 364, 91, 32000
pm_ghcol:  .byte 7, 3, 8                    // Blinky red, Pinky magenta, Clyde orange
.align 2
s_pmscore: .asciz "SCORE"
s_pmlives: .asciz "LIVES"
s_pmover:  .asciz "GAME OVER"
.align 2
// maze template: 0 empty 1 wall 2 dot 3 power 4 house 5 gate
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
.section .text
