// ============================================================================
// UnoDOS / PinePhone (AArch64) — Pac-Man, ported from genesis/pacman.i.
// The 28x25 maze (byte-identical template) fits the 480-px portrait panel at
// 17-px cells (28*17 = 476). The x86-lineage AI is kept: Blinky targets pac,
// Pinky 4 tiles ahead, Clyde goes hybrid (corner within manhattan 8), the
// scatter/chase schedule flips with direction reversal, and power pellets
// start frightened mode with the 200/400/800/1600 eat chain.
//
// Minimal-profile simplifications (no sprites, one full-screen app): actors
// move TILE-STEPPED (one tile per 6-frame step; frightened ghosts every other
// step) and are drawn as filled cells; an eaten ghost returns to the house
// directly (no eyes walking home) and re-emerges after a timer.
// D-pad = pac, A = new game after GAME OVER, B = back.
// ============================================================================

.equ PMC,   28
.equ PMR,   25
.equ PMOX,  2
.equ PMOY,  32
.equ PMCELL, 17
.equ PMS_READY, 1
.equ PMS_PLAY,  2
.equ PMS_OVER,  4
.equ PM_STEPF,  6                // frames per movement step

// pm_load_maze: template -> live maze, count the dots. Leaf.
pm_load_maze:
    ldr   x0, =pm_maze_tpl
    ldr   x1, =pm_maze
    mov   w2, #(PMC*PMR)
    mov   w4, #0
pml_l:
    ldrb  w3, [x0], #1
    strb  w3, [x1], #1
    cmp   w3, #2
    b.eq  pml_d
    cmp   w3, #3
    b.ne  pml_n
pml_d:
    add   w4, w4, #1
pml_n:
    subs  w2, w2, #1
    b.ne  pml_l
    ldr   x0, =pm_dots
    str   w4, [x0]
    ret

// pm_reset_actors. Leaf.
pm_reset_actors:
    ldr   x0, =pm_px
    mov   w1, #14
    str   w1, [x0]
    ldr   x0, =pm_py
    mov   w1, #19
    str   w1, [x0]
    mov   w1, #1
    ldr   x0, =pm_dir
    str   w1, [x0]
    ldr   x0, =pm_ndir
    str   w1, [x0]
    ldr   x0, =pm_gh
    mov   w1, #14                         // Blinky: outside, scattering
    str   w1, [x0, #GH_X]
    mov   w1, #10
    str   w1, [x0, #GH_Y]
    mov   w1, #1
    str   w1, [x0, #GH_DIR]
    mov   w1, #GH_SCAT
    str   w1, [x0, #GH_ST]
    str   wzr, [x0, #GH_TMR]
    mov   w1, #13                         // Pinky: house, 100 frames
    str   w1, [x0, #GH_SIZE+GH_X]
    mov   w1, #12
    str   w1, [x0, #GH_SIZE+GH_Y]
    mov   w1, #1
    str   w1, [x0, #GH_SIZE+GH_DIR]
    mov   w1, #GH_HOUSE
    str   w1, [x0, #GH_SIZE+GH_ST]
    mov   w1, #100
    str   w1, [x0, #GH_SIZE+GH_TMR]
    mov   w1, #15                         // Clyde: house, 200 frames
    str   w1, [x0, #2*GH_SIZE+GH_X]
    mov   w1, #12
    str   w1, [x0, #2*GH_SIZE+GH_Y]
    mov   w1, #1
    str   w1, [x0, #2*GH_SIZE+GH_DIR]
    mov   w1, #GH_HOUSE
    str   w1, [x0, #2*GH_SIZE+GH_ST]
    mov   w1, #200
    str   w1, [x0, #2*GH_SIZE+GH_TMR]
    ldr   x0, =pm_fright
    str   wzr, [x0]
    ldr   x0, =pm_kills
    str   wzr, [x0]
    ldr   x0, =pm_step
    str   wzr, [x0]
    ldr   x0, =pm_par
    str   wzr, [x0]
    ret

pacman_init:
    stp   x29, x30, [sp, #-16]!
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
    bl    pm_load_maze
    bl    pm_reset_actors
    ldr   x0, =pm_state
    mov   w1, #PMS_READY
    str   w1, [x0]
    ldr   x0, =pm_stt
    mov   w1, #90
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// pm_walkable: w0 = tx (signed, wraps), w1 = ty -> w0 = 1 free / 0 blocked.
// Leaf; clobbers w1, w2, x2. Walls, house and gate all block (nobody path-
// walks the house here - exits/returns teleport).
pm_walkable:
    cmp   w1, #0
    b.lt  pwk_no
    cmp   w1, #PMR
    b.ge  pwk_no
    cmp   w0, #0
    b.ge  pwk_x1
    add   w0, w0, #PMC
pwk_x1:
    cmp   w0, #PMC
    b.lt  pwk_x2
    sub   w0, w0, #PMC
pwk_x2:
    mov   w2, #PMC
    mul   w1, w1, w2
    add   w1, w1, w0
    ldr   x2, =pm_maze
    ldrb  w2, [x2, w1, uxtw]
    cmp   w2, #1
    b.eq  pwk_no
    cmp   w2, #4
    b.eq  pwk_no
    cmp   w2, #5
    b.eq  pwk_no
    mov   w0, #1
    ret
pwk_no:
    mov   w0, #0
    ret

// pm_mode_state -> w0 = GH_SCAT / GH_CHASE from the schedule parity. Leaf.
pm_mode_state:
    ldr   x0, =pm_mode
    ldr   w0, [x0]
    tst   w0, #1
    mov   w0, #GH_SCAT
    mov   w1, #GH_CHASE
    csel  w0, w1, w0, ne
    ret

// ---------------------------------------------------------------- rendering
// pm_tile: w0 = tx, w1 = ty — one maze cell (wall/dot/power/gate/empty)
pm_tile:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w2, #PMCELL
    mul   w19, w0, w2
    add   w19, w19, #PMOX
    mul   w20, w1, w2
    add   w20, w20, #PMOY
    mov   w2, #PMC
    mul   w1, w1, w2
    add   w1, w1, w0
    ldr   x2, =pm_maze
    ldrb  w21, [x2, w1, uxtw]
    cmp   w21, #1
    b.ne  pmt_notw
    mov   w0, #0                          // wall = desktop colour
    b     pmt_f
pmt_notw:
    mov   w0, #4                          // corridor/house = black
pmt_f:
    bl    set_fg
    mov   w0, w19
    mov   w1, w20
    mov   w2, #PMCELL
    mov   w3, #PMCELL
    bl    frect
    cmp   w21, #2
    b.eq  pmt_dot
    cmp   w21, #3
    b.eq  pmt_pow
    cmp   w21, #5
    b.eq  pmt_gate
    b     pmt_done
pmt_dot:
    mov   w0, #1
    bl    set_fg
    add   w0, w19, #7
    add   w1, w20, #7
    mov   w2, #3
    mov   w3, #3
    bl    frect
    b     pmt_done
pmt_pow:
    mov   w0, #1
    bl    set_fg
    add   w0, w19, #5
    add   w1, w20, #5
    mov   w2, #7
    mov   w3, #7
    bl    frect
    b     pmt_done
pmt_gate:
    mov   w0, #9
    bl    set_fg
    mov   w0, w19
    add   w1, w20, #7
    mov   w2, #PMCELL
    mov   w3, #3
    bl    frect
pmt_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_actor: w0 = tx, w1 = ty, w2 = colour — a 13x13 filled cell
pm_actor:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    mov   w0, w2
    bl    set_fg
    mov   w2, #PMCELL
    mul   w0, w19, w2
    add   w0, w0, #(PMOX+2)
    mul   w1, w20, w2
    add   w1, w1, #(PMOY+2)
    mov   w2, #13
    mov   w3, #13
    bl    frect
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_ghcol: w0 = ghost index -> w0 = colour (fright = cyan). Leaf.
pm_ghcol:
    ldr   x1, =pm_gh
    mov   w2, #GH_SIZE
    mul   w2, w0, w2
    add   x1, x1, w2, uxtw
    ldr   w2, [x1, #GH_ST]
    cmp   w2, #GH_FRIGHT
    b.ne  pgc_n
    mov   w0, #2
    ret
pgc_n:
    ldr   x1, =pm_ghcols
    ldrb  w0, [x1, w0, uxtw]
    ret

pm_draw_actors:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x0, =pm_px
    ldr   w0, [x0]
    ldr   x1, =pm_py
    ldr   w1, [x1]
    mov   w2, #6
    bl    pm_actor
    mov   w19, #0
pda_l:
    mov   w0, w19
    bl    pm_ghcol
    mov   w2, w0
    ldr   x1, =pm_gh
    mov   w3, #GH_SIZE
    mul   w3, w19, w3
    add   x1, x1, w3, uxtw
    ldr   w0, [x1, #GH_X]
    ldr   w1, [x1, #GH_Y]
    bl    pm_actor
    add   w19, w19, #1
    cmp   w19, #3
    b.ne  pda_l
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_panel: score / level / lives strip (partial redraw, right of "B = Back")
pm_panel:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #0
    bl    set_fg
    mov   w0, #120
    mov   w1, #464
    mov   w2, #352
    mov   w3, #8
    bl    frect
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #120
    mov   w1, #464
    ldr   x2, =s_pm_score
    bl    pstr
    ldr   x0, =pm_score
    ldr   w0, [x0]
    bl    fmt_dec
    mov   w0, #168
    mov   w1, #464
    ldr   x2, =numstr
    bl    pstr
    mov   w0, #248
    mov   w1, #464
    ldr   x2, =s_pm_lv
    bl    pstr
    ldr   x0, =pm_level
    ldr   w0, [x0]
    bl    fmt_dec
    mov   w0, #272
    mov   w1, #464
    ldr   x2, =numstr
    bl    pstr
    mov   w0, #312
    mov   w1, #464
    ldr   x2, =s_pm_lives
    bl    pstr
    ldr   x0, =pm_lives
    ldr   w0, [x0]
    bl    fmt_dec
    mov   w0, #364
    mov   w1, #464
    ldr   x2, =numstr
    bl    pstr
    ldp   x29, x30, [sp], #16
    ret

// ---------------------------------------------------------------- game logic
// pm_fright_all: power pellet — SCAT/CHASE ghosts turn frightened + reverse.
// Leaf.
pm_fright_all:
    ldr   x1, =pm_gh
    mov   w2, #3
pfa_l:
    ldr   w3, [x1, #GH_ST]
    cmp   w3, #GH_SCAT
    b.eq  pfa_s
    cmp   w3, #GH_CHASE
    b.ne  pfa_n
pfa_s:
    mov   w3, #GH_FRIGHT
    str   w3, [x1, #GH_ST]
    ldr   w3, [x1, #GH_DIR]
    eor   w3, w3, #2
    str   w3, [x1, #GH_DIR]
pfa_n:
    add   x1, x1, #GH_SIZE
    subs  w2, w2, #1
    b.ne  pfa_l
    ret

// pm_unfright: fright expired — frightened ghosts resume the schedule
pm_unfright:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    bl    pm_mode_state
    mov   w20, w0
    ldr   x19, =pm_gh
    mov   w2, #3
puf_l:
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_FRIGHT
    b.ne  puf_n
    str   w20, [x19, #GH_ST]
puf_n:
    add   x19, x19, #GH_SIZE
    subs  w2, w2, #1
    b.ne  puf_l
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_mode_flip: schedule advanced — SCAT/CHASE ghosts flip state + reverse
pm_mode_flip:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    bl    pm_mode_state
    mov   w20, w0
    ldr   x19, =pm_gh
    mov   w2, #3
pmff_l:
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_SCAT
    b.eq  pmff_s
    cmp   w1, #GH_CHASE
    b.ne  pmff_n
pmff_s:
    str   w20, [x19, #GH_ST]
    ldr   w1, [x19, #GH_DIR]
    eor   w1, w1, #2
    str   w1, [x19, #GH_DIR]
pmff_n:
    add   x19, x19, #GH_SIZE
    subs  w2, w2, #1
    b.ne  pmff_l
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_kill: pac caught — lose a life / game over
pm_kill:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =pm_lives
    ldr   w1, [x0]
    sub   w1, w1, #1
    str   w1, [x0]
    cmp   w1, #0
    b.gt  pmk_alive
    ldr   x0, =pm_state
    mov   w1, #PMS_OVER
    str   w1, [x0]
    b     pmk_d
pmk_alive:
    bl    pm_reset_actors
    ldr   x0, =pm_state
    mov   w1, #PMS_READY
    str   w1, [x0]
    ldr   x0, =pm_stt
    mov   w1, #90
    str   w1, [x0]
pmk_d:
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// pm_coll_one: w0 = ghost index — resolve a pac/ghost meeting on one tile.
// Frightened -> eaten (chain score, back to the house); active -> pm_kill.
pm_coll_one:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w20, w0
    ldr   x19, =pm_gh
    mov   w1, #GH_SIZE
    mul   w1, w0, w1
    add   x19, x19, w1, uxtw
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_HOUSE
    b.eq  pco_d
    ldr   w1, [x19, #GH_X]
    ldr   x2, =pm_px
    ldr   w2, [x2]
    cmp   w1, w2
    b.ne  pco_d
    ldr   w1, [x19, #GH_Y]
    ldr   x2, =pm_py
    ldr   w2, [x2]
    cmp   w1, w2
    b.ne  pco_d
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_FRIGHT
    b.ne  pco_kill
    ldr   x0, =pm_kills                   // eat chain: 200/400/800/1600
    ldr   w1, [x0]
    mov   w2, #200
    lsl   w2, w2, w1
    ldr   x3, =pm_score
    ldr   w4, [x3]
    add   w4, w4, w2
    str   w4, [x3]
    cmp   w1, #3
    b.ge  pco_h
    add   w1, w1, #1
    str   w1, [x0]
pco_h:
    mov   w1, #14                         // back to the house
    str   w1, [x19, #GH_X]
    mov   w1, #12
    str   w1, [x19, #GH_Y]
    mov   w1, #GH_HOUSE
    str   w1, [x19, #GH_ST]
    mov   w1, #180
    str   w1, [x19, #GH_TMR]
    ldr   x0, =pm_px                      // repaint the meeting tile + pac
    ldr   w0, [x0]
    ldr   x1, =pm_py
    ldr   w1, [x1]
    bl    pm_tile
    ldr   x0, =pm_px
    ldr   w0, [x0]
    ldr   x1, =pm_py
    ldr   w1, [x1]
    mov   w2, #6
    bl    pm_actor
    mov   w0, w20                         // ghost re-appears in the house
    bl    pm_ghcol
    mov   w2, w0
    ldr   w0, [x19, #GH_X]
    ldr   w1, [x19, #GH_Y]
    bl    pm_actor
    b     pco_d
pco_kill:
    bl    pm_kill
pco_d:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

pm_coll_pac:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
pcp_l:
    mov   w0, w19
    bl    pm_coll_one
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_PLAY
    b.ne  pcp_d
    add   w19, w19, #1
    cmp   w19, #3
    b.ne  pcp_l
pcp_d:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_step_pac: one tile step — turn, move, eat, level-clear, draw
pm_step_pac:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    ldr   x0, =pm_px
    ldr   w19, [x0]
    ldr   x1, =pm_py
    ldr   w20, [x1]
    ldr   x0, =pm_ndir                    // turn if the wished dir is open
    ldr   w21, [x0]
    ldr   x1, =pm_dx
    ldr   w0, [x1, w21, uxtw #2]
    add   w0, w0, w19
    ldr   x1, =pm_dy
    ldr   w1, [x1, w21, uxtw #2]
    add   w1, w1, w20
    bl    pm_walkable
    cbz   w0, psp_keep
    ldr   x0, =pm_dir
    str   w21, [x0]
psp_keep:
    ldr   x0, =pm_dir
    ldr   w21, [x0]
    ldr   x1, =pm_dx
    ldr   w22, [x1, w21, uxtw #2]
    add   w22, w22, w19                   // nx (unwrapped)
    ldr   x1, =pm_dy
    ldr   w2, [x1, w21, uxtw #2]
    add   w2, w2, w20
    mov   w21, w2                         // ny
    mov   w0, w22
    mov   w1, w2
    bl    pm_walkable
    cbz   w0, psp_done                    // blocked: stay this step
    mov   w0, w19                         // erase the old tile
    mov   w1, w20
    bl    pm_tile
    cmp   w22, #0                         // tunnel wrap
    b.ge  psp_w1
    add   w22, w22, #PMC
psp_w1:
    cmp   w22, #PMC
    b.lt  psp_w2
    sub   w22, w22, #PMC
psp_w2:
    ldr   x0, =pm_px
    str   w22, [x0]
    ldr   x0, =pm_py
    str   w21, [x0]
    mov   w0, #PMC                        // eat at the new tile
    mul   w0, w21, w0
    add   w0, w0, w22
    ldr   x1, =pm_maze
    ldrb  w2, [x1, w0, uxtw]
    cmp   w2, #2
    b.ne  psp_pow
    strb  wzr, [x1, w0, uxtw]
    ldr   x2, =pm_score
    ldr   w3, [x2]
    add   w3, w3, #10
    str   w3, [x2]
    ldr   x2, =pm_dots
    ldr   w3, [x2]
    sub   w3, w3, #1
    str   w3, [x2]
    b     psp_chk
psp_pow:
    cmp   w2, #3
    b.ne  psp_draw
    strb  wzr, [x1, w0, uxtw]
    ldr   x2, =pm_score
    ldr   w3, [x2]
    add   w3, w3, #50
    str   w3, [x2]
    ldr   x2, =pm_dots
    ldr   w3, [x2]
    sub   w3, w3, #1
    str   w3, [x2]
    ldr   x2, =pm_fright
    mov   w3, #420
    str   w3, [x2]
    ldr   x2, =pm_kills
    str   wzr, [x2]
    bl    pm_fright_all
psp_chk:
    ldr   x0, =pm_dots                    // level clear?
    ldr   w0, [x0]
    cbnz  w0, psp_draw
    ldr   x0, =pm_level
    ldr   w1, [x0]
    add   w1, w1, #1
    str   w1, [x0]
    bl    pm_load_maze
    bl    pm_reset_actors
    ldr   x0, =pm_state
    mov   w1, #PMS_READY
    str   w1, [x0]
    ldr   x0, =pm_stt
    mov   w1, #90
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    b     psp_done
psp_draw:
    ldr   x0, =pm_px
    ldr   w0, [x0]
    ldr   x1, =pm_py
    ldr   w1, [x1]
    mov   w2, #6
    bl    pm_actor
    bl    pm_coll_pac
psp_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_steer: pick GH_DIR for the ghost at x19 (struct ptr), w20 = index.
// Frightened: random walkable non-reverse. Else: argmin manhattan-to-target
// over walkable non-reverse dirs (targets per personality / scatter corners).
pm_steer:
    stp   x29, x30, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_FRIGHT
    b.ne  pss_target
    mov   w21, #8
pss_rnd:
    bl    rng
    and   w22, w0, #3
    ldr   w1, [x19, #GH_DIR]
    eor   w1, w1, #2
    cmp   w22, w1
    b.eq  pss_rtry
    ldr   x0, =pm_dx
    ldr   w0, [x0, w22, uxtw #2]
    ldr   w1, [x19, #GH_X]
    add   w0, w0, w1
    ldr   x1, =pm_dy
    ldr   w1, [x1, w22, uxtw #2]
    ldr   w2, [x19, #GH_Y]
    add   w1, w1, w2
    bl    pm_walkable
    cbz   w0, pss_rtry
    str   w22, [x19, #GH_DIR]
    b     pss_done
pss_rtry:
    subs  w21, w21, #1
    b.ne  pss_rnd
    b     pss_done
pss_target:
    cmp   w1, #GH_CHASE
    b.ne  pss_scat
    ldr   x0, =pm_px
    ldr   w2, [x0]
    ldr   x0, =pm_py
    ldr   w3, [x0]
    cbz   w20, pss_sett                   // Blinky: pac itself
    cmp   w20, #1
    b.ne  pss_clyde
    ldr   x0, =pm_dir                     // Pinky: 4 tiles ahead of pac
    ldr   w4, [x0]
    ldr   x0, =pm_dx
    ldr   w5, [x0, w4, uxtw #2]
    add   w2, w2, w5, lsl #2
    ldr   x0, =pm_dy
    ldr   w5, [x0, w4, uxtw #2]
    add   w3, w3, w5, lsl #2
    b     pss_sett
pss_clyde:
    ldr   w4, [x19, #GH_X]                // Clyde: near pac -> corner
    subs  w4, w4, w2
    cneg  w4, w4, mi
    ldr   w5, [x19, #GH_Y]
    subs  w5, w5, w3
    cneg  w5, w5, mi
    add   w4, w4, w5
    cmp   w4, #8
    b.gt  pss_sett
    mov   w2, #1
    mov   w3, #23
    b     pss_sett
pss_scat:
    ldr   x0, =pm_cornx
    ldr   w2, [x0, w20, uxtw #2]
    ldr   x0, =pm_corny
    ldr   w3, [x0, w20, uxtw #2]
pss_sett:
    ldr   x0, =pm_ttx
    str   w2, [x0]
    ldr   x0, =pm_tty
    str   w3, [x0]
    mov   w21, #0                         // candidate dir
    mov   w22, #-1                        // best dir
    mov   w23, #0x7FFF                    // best distance
pss_try:
    ldr   w1, [x19, #GH_DIR]
    eor   w1, w1, #2
    cmp   w21, w1
    b.eq  pss_next
    ldr   x0, =pm_dx
    ldr   w0, [x0, w21, uxtw #2]
    ldr   w1, [x19, #GH_X]
    add   w24, w0, w1                     // nx
    ldr   x1, =pm_dy
    ldr   w1, [x1, w21, uxtw #2]
    ldr   w2, [x19, #GH_Y]
    add   w2, w1, w2                      // ny
    ldr   x3, =pm_tny
    str   w2, [x3]
    mov   w0, w24
    mov   w1, w2
    bl    pm_walkable
    cbz   w0, pss_next
    mov   w0, w24                         // wrap nx for the distance
    cmp   w0, #0
    b.ge  pss_tw1
    add   w0, w0, #PMC
pss_tw1:
    cmp   w0, #PMC
    b.lt  pss_tw2
    sub   w0, w0, #PMC
pss_tw2:
    ldr   x1, =pm_ttx
    ldr   w1, [x1]
    subs  w0, w0, w1
    cneg  w0, w0, mi
    ldr   x1, =pm_tny
    ldr   w1, [x1]
    ldr   x2, =pm_tty
    ldr   w2, [x2]
    subs  w1, w1, w2
    cneg  w1, w1, mi
    add   w0, w0, w1
    cmp   w0, w23
    b.ge  pss_next
    mov   w23, w0
    mov   w22, w21
pss_next:
    add   w21, w21, #1
    cmp   w21, #4
    b.ne  pss_try
    cmn   w22, #1
    b.eq  pss_done
    str   w22, [x19, #GH_DIR]
pss_done:
    ldp   x23, x24, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pm_step_ghost: w0 = index — house countdown / steer / move / draw / collide
pm_step_ghost:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w20, w0
    ldr   x19, =pm_gh
    mov   w1, #GH_SIZE
    mul   w1, w0, w1
    add   x19, x19, w1, uxtw
    ldr   w1, [x19, #GH_ST]
    cmp   w1, #GH_HOUSE
    b.ne  psg_act
    ldr   w1, [x19, #GH_TMR]
    subs  w1, w1, #1
    str   w1, [x19, #GH_TMR]
    b.gt  psg_done
    ldr   w0, [x19, #GH_X]                // leave: erase, hop to the door tile
    ldr   w1, [x19, #GH_Y]
    bl    pm_tile
    mov   w1, #14
    str   w1, [x19, #GH_X]
    mov   w1, #10
    str   w1, [x19, #GH_Y]
    mov   w1, #1
    str   w1, [x19, #GH_DIR]
    bl    pm_mode_state
    str   w0, [x19, #GH_ST]
    b     psg_draw
psg_act:
    cmp   w1, #GH_FRIGHT                  // frightened = half speed
    b.ne  psg_steer
    ldr   x0, =pm_par
    ldr   w0, [x0]
    cbz   w0, psg_done
psg_steer:
    bl    pm_steer
    ldr   w21, [x19, #GH_DIR]
    ldr   x0, =pm_dx
    ldr   w0, [x0, w21, uxtw #2]
    ldr   w22, [x19, #GH_X]
    add   w0, w0, w22
    ldr   x1, =pm_dy
    ldr   w1, [x1, w21, uxtw #2]
    ldr   w2, [x19, #GH_Y]
    add   w1, w1, w2
    mov   w21, w0                         // nx (unwrapped)
    mov   w22, w1                         // ny
    bl    pm_walkable
    cbz   w0, psg_done
    ldr   w0, [x19, #GH_X]                // erase the old tile
    ldr   w1, [x19, #GH_Y]
    bl    pm_tile
    cmp   w21, #0
    b.ge  psg_w1
    add   w21, w21, #PMC
psg_w1:
    cmp   w21, #PMC
    b.lt  psg_w2
    sub   w21, w21, #PMC
psg_w2:
    str   w21, [x19, #GH_X]
    str   w22, [x19, #GH_Y]
psg_draw:
    ldr   x0, =pm_px                      // pac may share the erased tile
    ldr   w0, [x0]
    ldr   x1, =pm_py
    ldr   w1, [x1]
    mov   w2, #6
    bl    pm_actor
    mov   w0, w20
    bl    pm_ghcol
    mov   w2, w0
    ldr   w0, [x19, #GH_X]
    ldr   w1, [x19, #GH_Y]
    bl    pm_actor
    mov   w0, w20
    bl    pm_coll_one
psg_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

pm_step_ghosts:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
psgs_l:
    mov   w0, w19
    bl    pm_step_ghost
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_PLAY
    b.ne  psgs_d
    add   w19, w19, #1
    cmp   w19, #3
    b.ne  psgs_l
psgs_d:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pacman_update: one frame
pacman_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_OVER
    b.ne  pku_1
    ldr   x0, =v_pade                     // A = new game
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  pku_ret
    bl    pacman_init
    b     pku_ret
pku_1:
    ldr   x1, =v_pade                     // wished direction
    ldr   w1, [x1]
    ldr   x2, =pm_ndir
    tst   w1, #PAD_U
    b.eq  pkd_1
    str   wzr, [x2]
pkd_1:
    tst   w1, #PAD_L
    b.eq  pkd_2
    mov   w3, #1
    str   w3, [x2]
pkd_2:
    tst   w1, #PAD_D
    b.eq  pkd_3
    mov   w3, #2
    str   w3, [x2]
pkd_3:
    tst   w1, #PAD_R
    b.eq  pkd_4
    mov   w3, #3
    str   w3, [x2]
pkd_4:
    cmp   w0, #PMS_READY
    b.ne  pku_play
    ldr   x2, =pm_stt
    ldr   w1, [x2]
    subs  w1, w1, #1
    str   w1, [x2]
    b.ne  pku_ret
    ldr   x0, =pm_state
    mov   w1, #PMS_PLAY
    str   w1, [x0]
    ldr   x0, =v_dirty                    // clears the READY! overlay
    mov   w1, #1
    str   w1, [x0]
    b     pku_ret
pku_play:
    ldr   x2, =pm_fright                  // fright countdown / mode schedule
    ldr   w0, [x2]
    cbz   w0, pku_mode
    subs  w0, w0, #1
    str   w0, [x2]
    b.ne  pku_stp
    bl    pm_unfright
    b     pku_stp
pku_mode:
    ldr   x2, =pm_modet
    ldr   w0, [x2]
    add   w0, w0, #1
    str   w0, [x2]
    ldr   x3, =pm_mode
    ldr   w1, [x3]
    cmp   w1, #7
    mov   w4, #7
    csel  w1, w4, w1, gt
    ldr   x4, =pm_modedur
    ldr   w1, [x4, w1, uxtw #2]
    cmp   w0, w1
    b.lt  pku_stp
    str   wzr, [x2]
    ldr   w1, [x3]
    cmp   w1, #7
    b.ge  pku_rev
    add   w1, w1, #1
    str   w1, [x3]
pku_rev:
    bl    pm_mode_flip
pku_stp:
    ldr   x2, =pm_step                    // movement cadence
    ldr   w0, [x2]
    add   w0, w0, #1
    str   w0, [x2]
    cmp   w0, #PM_STEPF
    b.lt  pku_ret
    str   wzr, [x2]
    ldr   x2, =pm_par
    ldr   w0, [x2]
    eor   w0, w0, #1
    str   w0, [x2]
    bl    pm_step_pac
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_PLAY
    b.ne  pku_ret
    bl    pm_step_ghosts
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_PLAY
    b.ne  pku_ret
    bl    pm_panel
pku_ret:
    ldp   x29, x30, [sp], #16
    ret

// app_pacman: full-screen draw (chrome + maze + actors + panel + overlay)
app_pacman:
    ldr   x2, =l_pacman
    bl    draw_chrome
    stp   x19, x20, [sp, #-16]!
    mov   w20, #0
apm_r:
    mov   w19, #0
apm_c:
    mov   w0, w19
    mov   w1, w20
    bl    pm_tile
    add   w19, w19, #1
    cmp   w19, #PMC
    b.ne  apm_c
    add   w20, w20, #1
    cmp   w20, #PMR
    b.ne  apm_r
    bl    pm_draw_actors
    bl    pm_panel
    ldr   x0, =pm_state
    ldr   w0, [x0]
    cmp   w0, #PMS_READY
    b.ne  apm_no
    mov   w0, #6
    mov   w1, #4
    bl    setfb
    mov   w0, #216
    mov   w1, #274
    ldr   x2, =s_pm_ready
    bl    pstr
    b     apm_d
apm_no:
    cmp   w0, #PMS_OVER
    b.ne  apm_d
    mov   w0, #7
    mov   w1, #4
    bl    setfb
    mov   w0, #176
    mov   w1, #274
    ldr   x2, =s_pm_over
    bl    pstr
apm_d:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
.align 2
pm_dx:      .word 0, -1, 0, 1            // UP LEFT DOWN RIGHT
pm_dy:      .word -1, 0, 1, 0
pm_cornx:   .word 26, 1, 1
pm_corny:   .word 1, 1, 23
pm_modedur: .word 420, 1200, 420, 1200, 300, 1200, 300, 30000
pm_ghcols:  .byte 7, 3, 8                // Blinky red, Pinky magenta, Clyde orange
s_pm_score: .asciz "Score"
s_pm_lv:    .asciz "Lv"
s_pm_lives: .asciz "Lives"
s_pm_ready: .asciz "READY!"
s_pm_over:  .asciz "GAME OVER  A=new"
.section .text
