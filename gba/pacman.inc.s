@ ============================================================================
@ UnoDOS / GBA — Pac-Man. ARM7TDMI (ARMv4T) translation of the verified rpi port,
@ laid out for the 240x160 Mode-3 screen (5px maze cells). Blinky/Pinky/Clyde AI,
@ scatter/chase + frightened; tile-stepped. Maze 0 empty 1 wall 2 dot 3 power
@ 4 house 5 gate. Ghost struct (GSIZE): GX@0 GY@4 GDIR@8 GST@12 GTMR@16.
@ ============================================================================

dirdx:
    ldr r1, =pm_dx
    ldr r0, [r1, r0, lsl #2]
    bx lr
dirdy:
    ldr r1, =pm_dy
    ldr r0, [r1, r0, lsl #2]
    bx lr
.ltorg

pm_mode_state:
    ldr r0, =pm_mode
    ldr r0, [r0]
    tst r0, #1
    moveq r0, #1
    movne r0, #2
    bx lr
.ltorg

pm_walkable:                             @ r0=tx r1=ty r2=ghost r3=eaten -> r0=1/0
    cmp r1, #0
    blt pw_no
    cmp r1, #PM_ROWS
    bge pw_no
    cmp r0, #0
    addlt r0, r0, #PM_COLS
    cmp r0, #PM_COLS
    subge r0, r0, #PM_COLS
    mov r12, #PM_COLS
    mul r12, r1, r12
    add r12, r12, r0
    ldr r0, =pm_maze
    ldrb r0, [r0, r12]
    cmp r0, #1
    beq pw_no
    cmp r0, #4
    beq pw_gate
    cmp r0, #5
    beq pw_gate
    mov r0, #1
    bx lr
pw_gate:
    cmp r2, #0
    beq pw_no
    cmp r3, #0
    beq pw_no
    mov r0, #1
    bx lr
pw_no:
    mov r0, #0
    bx lr
.ltorg

pm_load_maze:
    push {r4-r7, lr}
    ldr r4, =pm_maze_tpl
    ldr r5, =pm_maze
    mov r6, #(PM_COLS*PM_ROWS)
    mov r7, #0
plm_l:
    ldrb r0, [r4], #1
    strb r0, [r5], #1
    cmp r0, #2
    beq plm_dot
    cmp r0, #3
    bne plm_nx
plm_dot:
    add r7, r7, #1
plm_nx:
    subs r6, r6, #1
    bne plm_l
    ldr r0, =pm_dots
    str r7, [r0]
    pop {r4-r7, pc}
.ltorg

gh_ptr:
    mov r1, #GSIZE
    mul r2, r0, r1
    ldr r0, =pm_gh
    add r0, r0, r2
    bx lr
.ltorg

pm_reset_actors:
    push {lr}
    mov r0, #14
    ldr r1, =pm_x
    str r0, [r1]
    mov r0, #19
    ldr r1, =pm_y
    str r0, [r1]
    mov r0, #1
    ldr r1, =pm_dir
    str r0, [r1]
    ldr r1, =pm_ndir
    str r0, [r1]
    mov r0, #0
    bl gh_ptr
    mov r1, #14
    str r1, [r0, #0]
    mov r1, #10
    str r1, [r0, #4]
    mov r1, #1
    str r1, [r0, #8]
    mov r1, #1
    str r1, [r0, #12]
    mov r1, #0
    str r1, [r0, #16]
    mov r0, #1
    bl gh_ptr
    mov r1, #13
    str r1, [r0, #0]
    mov r1, #12
    str r1, [r0, #4]
    mov r1, #0
    str r1, [r0, #8]
    str r1, [r0, #12]
    mov r1, #20
    str r1, [r0, #16]
    mov r0, #2
    bl gh_ptr
    mov r1, #15
    str r1, [r0, #0]
    mov r1, #12
    str r1, [r0, #4]
    mov r1, #0
    str r1, [r0, #8]
    str r1, [r0, #12]
    mov r1, #40
    str r1, [r0, #16]
    ldr r0, =pm_fr
    mov r1, #0
    str r1, [r0]
    ldr r0, =pm_kills
    mov r1, #0
    str r1, [r0]
    pop {pc}
.ltorg

pacman_init:
    push {lr}
    mov r1, #0
    ldr r0, =pm_score
    str r1, [r0]
    mov r1, #3
    ldr r0, =pm_lives
    str r1, [r0]
    mov r1, #1
    ldr r0, =pm_level
    str r1, [r0]
    mov r1, #0
    ldr r0, =pm_mode
    str r1, [r0]
    ldr r0, =pm_modet
    str r1, [r0]
    ldr r0, =pm_st
    str r1, [r0]
    ldr r0, =pm_sc
    str r1, [r0]
    ldr r0, =pm_ft
    str r1, [r0]
    ldr r0, =REG_VCOUNT
    ldrh r1, [r0]
    orr r1, r1, #1
    ldr r0, =g_seed
    str r1, [r0]
    bl pm_load_maze
    bl pm_reset_actors
    pop {pc}
.ltorg

pm_frighten:
    push {r4, r5, lr}
    mov r4, #0
pf_l:
    cmp r4, #3
    bge pf_done
    mov r0, r4
    bl gh_ptr
    mov r5, r0
    ldr r0, [r5, #12]
    cmp r0, #1
    beq pf_set
    cmp r0, #2
    bne pf_nx
pf_set:
    mov r0, #3
    str r0, [r5, #12]
    ldr r0, [r5, #8]
    eor r0, r0, #2
    str r0, [r5, #8]
pf_nx:
    add r4, r4, #1
    b pf_l
pf_done:
    pop {r4, r5, pc}
.ltorg

pm_unfright:
    push {r4, r5, lr}
    mov r4, #0
puf_l:
    cmp r4, #3
    bge puf_done
    mov r0, r4
    bl gh_ptr
    mov r5, r0
    ldr r0, [r5, #12]
    cmp r0, #3
    bne puf_nx
    bl pm_mode_state
    str r0, [r5, #12]
puf_nx:
    add r4, r4, #1
    b puf_l
puf_done:
    pop {r4, r5, pc}
.ltorg

pm_modeflip:
    push {r4, r5, lr}
    mov r4, #0
pmf_l:
    cmp r4, #3
    bge pmf_done
    mov r0, r4
    bl gh_ptr
    mov r5, r0
    ldr r0, [r5, #12]
    cmp r0, #1
    beq pmf_set
    cmp r0, #2
    bne pmf_nx
pmf_set:
    bl pm_mode_state
    str r0, [r5, #12]
    ldr r0, [r5, #8]
    eor r0, r0, #2
    str r0, [r5, #8]
pmf_nx:
    add r4, r4, #1
    b pmf_l
pmf_done:
    pop {r4, r5, pc}
.ltorg

pm_kill:
    push {lr}
    ldr r1, =pm_lives
    ldr r0, [r1]
    sub r0, r0, #1
    str r0, [r1]
    cmp r0, #0
    bgt pk_alive
    mov r0, #1
    ldr r1, =pm_st
    str r0, [r1]
    pop {pc}
pk_alive:
    bl pm_reset_actors
    pop {pc}
.ltorg

@ pm_steer: r0 = ghost index.
pm_steer:
    push {r4-r11, lr}
    mov r4, r0
    bl gh_ptr
    mov r5, r0                            @ ghost ptr
    ldr r6, [r5, #0]                      @ gtx
    ldr r7, [r5, #4]                      @ gty
    ldr r0, [r5, #12]
    cmp r0, #3
    bne ps_target
    mov r8, #8
ps_rnd:
    bl rng
    and r9, r0, #3
    ldr r0, [r5, #8]
    eor r0, r0, #2
    cmp r9, r0
    beq ps_rtry
    mov r0, r9
    bl dirdx
    add r10, r0, r6
    mov r0, r9
    bl dirdy
    add r11, r0, r7
    mov r0, r10
    mov r1, r11
    mov r2, #1
    mov r3, #0
    bl pm_walkable
    cmp r0, #0
    beq ps_rtry
    str r9, [r5, #8]
    b ps_out
ps_rtry:
    subs r8, r8, #1
    bne ps_rnd
    b ps_out
ps_target:
    cmp r0, #4
    bne ps_note
    mov r0, #14
    ldr r1, =pm_tgx
    str r0, [r1]
    mov r0, #10
    ldr r1, =pm_tgy
    str r0, [r1]
    b ps_pick
ps_note:
    cmp r0, #2
    bne ps_scat
    ldr r8, =pm_x
    ldr r8, [r8]
    ldr r9, =pm_y
    ldr r9, [r9]
    cmp r4, #0
    beq ps_blinky
    cmp r4, #1
    beq ps_pinky
    sub r10, r6, r8
    cmp r10, #0
    rsblt r10, r10, #0
    sub r11, r7, r9
    cmp r11, #0
    rsblt r11, r11, #0
    add r10, r10, r11
    cmp r10, #8
    bgt ps_blinky
    b ps_scat
ps_pinky:
    ldr r0, =pm_dir
    ldr r0, [r0]
    bl dirdx
    mov r0, r0, lsl #2
    add r0, r0, r8
    ldr r1, =pm_tgx
    str r0, [r1]
    ldr r0, =pm_dir
    ldr r0, [r0]
    bl dirdy
    mov r0, r0, lsl #2
    add r0, r0, r9
    ldr r1, =pm_tgy
    str r0, [r1]
    b ps_pick
ps_blinky:
    ldr r1, =pm_tgx
    str r8, [r1]
    ldr r1, =pm_tgy
    str r9, [r1]
    b ps_pick
ps_scat:
    ldr r1, =pm_cornx
    ldr r0, [r1, r4, lsl #2]
    ldr r1, =pm_tgx
    str r0, [r1]
    ldr r1, =pm_corny
    ldr r0, [r1, r4, lsl #2]
    ldr r1, =pm_tgy
    str r0, [r1]
ps_pick:
    ldr r11, [r5, #8]
    eor r11, r11, #2                      @ reverse
    mov r9, #0                            @ dir
    mov r10, #-1                          @ best
    ldr r8, =0x7FFF                       @ bestdist
ps_try:
    cmp r9, r11
    beq ps_skip
    mov r0, r9
    bl dirdx
    ldr r1, [r5, #0]
    add r6, r0, r1                        @ nx
    mov r0, r9
    bl dirdy
    ldr r1, [r5, #4]
    add r7, r0, r1                        @ ny
    mov r0, r6
    mov r1, r7
    mov r2, #1
    ldr r3, [r5, #12]
    cmp r3, #4
    moveq r3, #1
    movne r3, #0
    bl pm_walkable
    cmp r0, #0
    beq ps_skip
    mov r0, r6
    cmp r0, #0
    addlt r0, r0, #PM_COLS
    cmp r0, #PM_COLS
    subge r0, r0, #PM_COLS
    ldr r1, =pm_tgx
    ldr r1, [r1]
    sub r0, r0, r1
    cmp r0, #0
    rsblt r0, r0, #0
    ldr r1, =pm_tgy
    ldr r1, [r1]
    sub r1, r7, r1
    cmp r1, #0
    rsblt r1, r1, #0
    add r0, r0, r1
    cmp r0, r8
    bge ps_skip
    mov r8, r0
    mov r10, r9
ps_skip:
    add r9, r9, #1
    cmp r9, #4
    blt ps_try
    cmp r10, #0
    blt ps_out
    str r10, [r5, #8]
ps_out:
    pop {r4-r11, pc}
.ltorg

pm_step:
    push {r4-r11, lr}
    ldr r0, =pm_st
    ldr r0, [r0]
    cmp r0, #0
    bne pstep_out
    ldr r1, =pm_sc
    ldr r0, [r1]
    add r0, r0, #1
    str r0, [r1]
    ldr r1, =pm_fr
    ldr r0, [r1]
    cmp r0, #0
    beq pstep_mode
    sub r0, r0, #1
    str r0, [r1]
    cmp r0, #0
    bne pstep_pac
    bl pm_unfright
    b pstep_pac
pstep_mode:
    ldr r1, =pm_modet
    ldr r0, [r1]
    add r0, r0, #1
    str r0, [r1]
    mov r2, r0
    ldr r1, =pm_mode
    ldr r0, [r1]
    cmp r0, #7
    movgt r0, #7
    ldr r1, =pm_modedur
    ldr r0, [r1, r0, lsl #2]
    cmp r2, r0
    blt pstep_pac
    mov r0, #0
    ldr r1, =pm_modet
    str r0, [r1]
    ldr r1, =pm_mode
    ldr r0, [r1]
    cmp r0, #7
    bge pstep_doflip
    add r0, r0, #1
    str r0, [r1]
pstep_doflip:
    bl pm_modeflip
pstep_pac:
    ldr r4, =pm_x
    ldr r4, [r4]
    ldr r5, =pm_y
    ldr r5, [r5]
    mov r1, #PM_COLS
    mul r0, r5, r1
    add r0, r0, r4
    ldr r1, =pm_maze
    ldrb r2, [r1, r0]
    cmp r2, #2
    beq pstep_dot
    cmp r2, #3
    beq pstep_pow
    b pstep_clear
pstep_dot:
    mov r3, #0
    strb r3, [r1, r0]
    ldr r1, =pm_score
    ldr r2, [r1]
    add r2, r2, #10
    str r2, [r1]
    ldr r1, =pm_dots
    ldr r2, [r1]
    sub r2, r2, #1
    str r2, [r1]
    b pstep_clear
pstep_pow:
    mov r3, #0
    strb r3, [r1, r0]
    ldr r1, =pm_score
    ldr r2, [r1]
    add r2, r2, #50
    str r2, [r1]
    ldr r1, =pm_dots
    ldr r2, [r1]
    sub r2, r2, #1
    str r2, [r1]
    ldr r1, =pm_fr
    mov r2, #FRIGHT_STEPS
    str r2, [r1]
    ldr r1, =pm_kills
    mov r2, #0
    str r2, [r1]
    bl pm_frighten
pstep_clear:
    ldr r0, =pm_dots
    ldr r0, [r0]
    cmp r0, #0
    bne pstep_move
    ldr r1, =pm_level
    ldr r0, [r1]
    add r0, r0, #1
    str r0, [r1]
    bl pm_load_maze
    bl pm_reset_actors
    b pstep_out
pstep_move:
    ldr r6, =pm_ndir
    ldr r6, [r6]
    mov r0, r6
    bl dirdx
    add r7, r0, r4
    mov r0, r6
    bl dirdy
    add r8, r0, r5
    mov r0, r7
    mov r1, r8
    mov r2, #0
    mov r3, #0
    bl pm_walkable
    cmp r0, #0
    beq pstep_dir
    ldr r1, =pm_dir
    str r6, [r1]
pstep_dir:
    ldr r6, =pm_dir
    ldr r6, [r6]
    mov r0, r6
    bl dirdx
    add r7, r0, r4
    mov r0, r6
    bl dirdy
    add r8, r0, r5
    mov r0, r7
    mov r1, r8
    mov r2, #0
    mov r3, #0
    bl pm_walkable
    cmp r0, #0
    beq pstep_ghosts
    cmp r7, #0
    movlt r7, #(PM_COLS-1)
    cmp r7, #PM_COLS
    movge r7, #0
    ldr r1, =pm_x
    str r7, [r1]
    ldr r1, =pm_y
    str r8, [r1]
pstep_ghosts:
    mov r4, #0
pstep_gl:
    mov r0, r4
    bl gh_ptr
    mov r5, r0
    ldr r0, [r5, #12]
    cmp r0, #0
    bne pstep_gactive
    ldr r0, [r5, #16]
    sub r0, r0, #1
    str r0, [r5, #16]
    cmp r0, #0
    bgt pstep_gnext
    mov r1, #14
    str r1, [r5, #0]
    mov r1, #10
    str r1, [r5, #4]
    mov r1, #1
    str r1, [r5, #8]
    bl pm_mode_state
    str r0, [r5, #12]
    b pstep_gnext
pstep_gactive:
    cmp r0, #4
    bne pstep_gsteer
    ldr r1, [r5, #0]
    cmp r1, #14
    bne pstep_gsteer
    ldr r1, [r5, #4]
    cmp r1, #10
    bne pstep_gsteer
    bl pm_mode_state
    str r0, [r5, #12]
pstep_gsteer:
    mov r0, r4
    bl pm_steer
    ldr r0, [r5, #12]
    cmp r0, #3
    bne pstep_gmove
    ldr r0, =pm_sc
    ldr r0, [r0]
    tst r0, #1
    bne pstep_gcoll
pstep_gmove:
    ldr r6, [r5, #8]
    mov r0, r6
    bl dirdx
    ldr r1, [r5, #0]
    add r7, r0, r1
    mov r0, r6
    bl dirdy
    ldr r1, [r5, #4]
    add r8, r0, r1
    mov r0, r7
    mov r1, r8
    mov r2, #1
    ldr r3, [r5, #12]
    cmp r3, #4
    moveq r3, #1
    movne r3, #0
    bl pm_walkable
    cmp r0, #0
    beq pstep_gcoll
    cmp r7, #0
    movlt r7, #(PM_COLS-1)
    cmp r7, #PM_COLS
    movge r7, #0
    str r7, [r5, #0]
    str r8, [r5, #4]
pstep_gcoll:
    ldr r0, [r5, #0]
    ldr r1, =pm_x
    ldr r1, [r1]
    cmp r0, r1
    bne pstep_gnext
    ldr r0, [r5, #4]
    ldr r1, =pm_y
    ldr r1, [r1]
    cmp r0, r1
    bne pstep_gnext
    ldr r0, [r5, #12]
    cmp r0, #3
    bne pstep_notfr
    mov r1, #4
    str r1, [r5, #12]
    ldr r1, =pm_kills
    ldr r2, [r1]
    mov r3, #200
    mov r3, r3, lsl r2
    ldr r1, =pm_score
    ldr r0, [r1]
    add r0, r0, r3
    str r0, [r1]
    ldr r1, =pm_kills
    ldr r2, [r1]
    cmp r2, #3
    bge pstep_gnext
    add r2, r2, #1
    str r2, [r1]
    b pstep_gnext
pstep_notfr:
    cmp r0, #4
    beq pstep_gnext
    bl pm_kill
    b pstep_out
pstep_gnext:
    add r4, r4, #1
    cmp r4, #3
    blt pstep_gl
pstep_out:
    pop {r4-r11, pc}
.ltorg

pacman_update:
    push {lr}
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_U
    bne pmu_setu
    tst r0, #PAD_L
    bne pmu_setl
    tst r0, #PAD_D
    bne pmu_setd
    tst r0, #PAD_R
    bne pmu_setr
    b pmu_pace
pmu_setu:
    mov r1, #0
    b pmu_seti
pmu_setl:
    mov r1, #1
    b pmu_seti
pmu_setd:
    mov r1, #2
    b pmu_seti
pmu_setr:
    mov r1, #3
pmu_seti:
    ldr r0, =pm_ndir
    str r1, [r0]
pmu_pace:
    ldr r0, =a_gpause
    ldr r0, [r0]
    cmp r0, #0
    bne pmu_done
    ldr r1, =pm_ft
    ldr r0, [r1]
    add r0, r0, #1
    str r0, [r1]
    cmp r0, #PM_STEPFRAMES
    blt pmu_done
    mov r0, #0
    str r0, [r1]
    bl pm_step
    mov r1, #1
    ldr r0, =v_dirty
    str r1, [r0]
pmu_done:
    pop {pc}
.ltorg

pm_draw_cell:                            @ r0=col r1=row r2=colour
    push {r4, r5, lr}
    mov r4, r0
    mov r5, r1
    mov r0, r2
    bl set_fg
    mov r1, #PM_CELL
    mul r0, r4, r1
    add r0, r0, #PMO_X
    mov r2, #PM_CELL
    mul r1, r5, r2
    add r1, r1, #PMO_Y
    mov r2, #PM_CELL
    mov r3, #PM_CELL
    bl frect
    pop {r4, r5, pc}
.ltorg

pm_draw_dot:                             @ r0=col r1=row r2=colour r3=size
    push {r4-r6, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r3
    mov r0, r2
    bl set_fg
    mov r1, #PM_CELL
    mul r0, r4, r1
    add r0, r0, #PMO_X
    mov r1, #PM_CELL
    sub r1, r1, r6
    mov r1, r1, lsr #1
    add r0, r0, r1
    mov r2, #PM_CELL
    mul r1, r5, r2
    add r1, r1, #PMO_Y
    mov r2, #PM_CELL
    sub r2, r2, r6
    mov r2, r2, lsr #1
    add r1, r1, r2
    mov r2, r6
    mov r3, r6
    bl frect
    pop {r4-r6, pc}
.ltorg

pm_draw_maze:
    push {r4-r6, lr}
    mov r4, #0
pdm_row:
    cmp r4, #PM_ROWS
    bge pdm_done
    mov r5, #0
pdm_col:
    cmp r5, #PM_COLS
    bge pdm_rowend
    mov r1, #PM_COLS
    mul r0, r4, r1
    add r0, r0, r5
    ldr r1, =pm_maze
    ldrb r6, [r1, r0]
    cmp r6, #1
    bne pdm_dot
    mov r0, r5
    mov r1, r4
    mov r2, #2
    bl pm_draw_cell
    b pdm_next
pdm_dot:
    cmp r6, #2
    bne pdm_pow
    mov r0, r5
    mov r1, r4
    mov r2, #1
    mov r3, #2
    bl pm_draw_dot
    b pdm_next
pdm_pow:
    cmp r6, #3
    bne pdm_next
    mov r0, r5
    mov r1, r4
    mov r2, #1
    mov r3, #3
    bl pm_draw_dot
pdm_next:
    add r5, r5, #1
    b pdm_col
pdm_rowend:
    add r4, r4, #1
    b pdm_row
pdm_done:
    pop {r4-r6, pc}
.ltorg

pm_draw_actors:
    push {r4-r6, lr}
    ldr r0, =pm_x
    ldr r0, [r0]
    ldr r1, =pm_y
    ldr r1, [r1]
    mov r2, #6
    bl pm_draw_cell
    mov r4, #0
pda_gl:
    cmp r4, #3
    bge pda_done
    mov r0, r4
    bl gh_ptr
    mov r5, r0
    ldr r0, [r5, #12]
    cmp r0, #3
    bne pda_ne
    mov r6, #1
    b pda_col
pda_ne:
    cmp r0, #4
    bne pda_body
    mov r6, #9
    b pda_col
pda_body:
    ldr r1, =pm_ghcol
    ldrb r6, [r1, r4]
pda_col:
    ldr r0, [r5, #0]
    ldr r1, [r5, #4]
    mov r2, r6
    bl pm_draw_cell
    add r4, r4, #1
    b pda_gl
pda_done:
    pop {r4-r6, pc}
.ltorg

pm_itoa5:                                @ r0 = value -> numstr (5 digits + NUL)
    push {r4-r6, lr}
    ldr r4, =numstr
    ldr r5, =10000
    bl pid
    ldr r5, =1000
    bl pid
    mov r5, #100
    bl pid
    mov r5, #10
    bl pid
    add r0, r0, #'0'
    strb r0, [r4], #1
    mov r0, #0
    strb r0, [r4]
    pop {r4-r6, pc}
pid:                                     @ r0=value r5=divisor r4=dest -> digit, r0=rem
    mov r6, #0
pid_l:
    cmp r0, r5
    blo pid_done
    sub r0, r0, r5
    add r6, r6, #1
    b pid_l
pid_done:
    add r6, r6, #'0'
    strb r6, [r4], #1
    bx lr
.ltorg

pm_draw_panel:
    push {lr}
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #150
    mov r1, #20
    ldr r2, =s_pmscore
    bl pstr
    ldr r0, =pm_score
    ldr r0, [r0]
    bl pm_itoa5
    mov r0, #150
    mov r1, #32
    ldr r2, =numstr
    bl pstr
    mov r0, #150
    mov r1, #48
    ldr r2, =s_pmlives
    bl pstr
    ldr r0, =pm_lives
    ldr r0, [r0]
    bl two_digits
    ldr r2, =numstr
    strb r1, [r2, #0]
    strb r0, [r2, #1]
    mov r0, #0
    strb r0, [r2, #2]
    mov r0, #150
    mov r1, #60
    ldr r2, =numstr
    bl pstr
    ldr r0, =pm_st
    ldr r0, [r0]
    cmp r0, #0
    beq pmp_done
    mov r0, #150
    mov r1, #80
    ldr r2, =s_pmover
    bl pstr
pmp_done:
    pop {pc}
.ltorg

app_pacman:
    ldr r0, =t_pacman
    bl draw_chrome
    bl pm_draw_maze
    bl pm_draw_actors
    bl pm_draw_panel
    pop {pc}
.ltorg

.align 2
pm_dx:      .word 0, -1, 0, 1
pm_dy:      .word -1, 0, 1, 0
pm_cornx:   .word 26, 1, 1
pm_corny:   .word 1, 1, 23
pm_modedur: .word 127, 364, 127, 364, 91, 364, 91, 32000
pm_ghcol:   .byte 7, 3, 8
.align 2
s_pmscore:  .asciz "SCORE"
s_pmlives:  .asciz "LIVES"
s_pmover:   .asciz "OVER"
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
