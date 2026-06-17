@ ============================================================================
@ UnoDOS / GBA — Dostris (falling-blocks). The shared algorithm in ARM; the board
@ renders as 8x8 colour cells into the Mode 3 framebuffer.
@ ============================================================================

@ get_mask: g_type,g_rot -> mlo (16-bit shape mask)
get_mask:
    ldr r0, =g_type
    ldr r0, [r0]
    mov r0, r0, lsl #2
    ldr r1, =g_rot
    ldr r1, [r1]
    add r0, r0, r1
    ldr r1, =piece_masks
    add r1, r1, r0, lsl #1
    ldrh r0, [r1]
    ldr r1, =mlo
    str r0, [r1]
    bx lr
.ltorg

@ piece_collide: trial g_tx,g_ty -> r0=1 collide / 0 free
piece_collide:
    push {r4-r8, lr}
    bl get_mask
    ldr r4, =mlo
    ldr r4, [r4]
    mov r5, #0
pc_loop:
    tst r4, #0x8000
    beq pc_next
    and r0, r5, #3
    ldr r1, =g_tx
    ldr r1, [r1]
    add r0, r0, r1
    cmp r0, #BW
    bhs pc_yes
    mov r6, r0
    and r0, r5, #12
    mov r0, r0, lsr #2
    ldr r1, =g_ty
    ldr r1, [r1]
    add r0, r0, r1
    cmp r0, #BH
    bhs pc_yes
    mov r7, r0
    mov r1, #BW
    mul r0, r1, r7
    add r0, r0, r6
    ldr r1, =g_board
    ldrb r0, [r1, r0]
    cmp r0, #0
    bne pc_yes
pc_next:
    mov r4, r4, lsl #1
    add r5, r5, #1
    cmp r5, #16
    bne pc_loop
    mov r0, #0
    pop {r4-r8, pc}
pc_yes:
    mov r0, #1
    pop {r4-r8, pc}
.ltorg

@ moves
ds_left:
    push {lr}
    ldr r0, =g_px
    ldr r0, [r0]
    sub r0, r0, #1
    ldr r1, =g_tx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_ty
    str r0, [r1]
    bl piece_collide
    cmp r0, #0
    popne {pc}
    ldr r0, =g_tx
    ldr r0, [r0]
    ldr r1, =g_px
    str r0, [r1]
    pop {pc}
ds_right:
    push {lr}
    ldr r0, =g_px
    ldr r0, [r0]
    add r0, r0, #1
    ldr r1, =g_tx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_ty
    str r0, [r1]
    bl piece_collide
    cmp r0, #0
    popne {pc}
    ldr r0, =g_tx
    ldr r0, [r0]
    ldr r1, =g_px
    str r0, [r1]
    pop {pc}
ds_rotate:
    push {lr}
    ldr r2, =g_rot
    ldr r0, [r2]
    ldr r1, =g_srot
    str r0, [r1]
    add r0, r0, #1
    and r0, r0, #3
    str r0, [r2]
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_tx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_ty
    str r0, [r1]
    bl piece_collide
    cmp r0, #0
    popeq {pc}
    ldr r0, =g_srot
    ldr r0, [r0]
    ldr r1, =g_rot
    str r0, [r1]
    pop {pc}
ds_softdrop:
    push {lr}
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_tx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    add r0, r0, #1
    ldr r1, =g_ty
    str r0, [r1]
    bl piece_collide
    cmp r0, #0
    popne {pc}
    ldr r0, =g_py
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    pop {pc}
.ltorg

@ dostris_update: one frame of game logic
dostris_update:
    push {lr}
    ldr r0, =g_state
    ldr r0, [r0]
    cmp r0, #0
    popne {pc}
    @ save old pose
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_oldpx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_oldpy
    str r0, [r1]
    ldr r0, =g_rot
    ldr r0, [r0]
    ldr r1, =g_oldrot
    str r0, [r1]
    @ input
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_L
    blne ds_left
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_R
    blne ds_right
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_U
    blne ds_rotate
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_A
    blne ds_rotate
    ldr r0, =v_pad
    ldr r0, [r0]
    tst r0, #PAD_D
    blne ds_softdrop
    @ gravity (AUTOTEST freezes it once the script ends)
    ldr r0, =a_gpause
    ldr r0, [r0]
    cmp r0, #0
    bne du_check
    ldr r3, =g_fall
    ldr r0, [r3]
    add r0, r0, #1
    str r0, [r3]
    cmp r0, #FALLRATE
    blo du_check
    mov r0, #0
    str r0, [r3]
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_tx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    add r0, r0, #1
    ldr r1, =g_ty
    str r0, [r1]
    bl piece_collide
    cmp r0, #0
    beq du_fall
    bl dostris_lock
    bl dostris_clearlines
    bl dostris_spawn
    ldr r0, =v_dirty
    mov r1, #1
    str r1, [r0]
    ldr r0, =pf_pc
    mov r1, #0
    str r1, [r0]
    pop {pc}
du_fall:
    ldr r0, =g_py
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
du_check:
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_oldpx
    ldr r1, [r1]
    cmp r0, r1
    bne du_moved
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_oldpy
    ldr r1, [r1]
    cmp r0, r1
    bne du_moved
    ldr r0, =g_rot
    ldr r0, [r0]
    ldr r1, =g_oldrot
    ldr r1, [r1]
    cmp r0, r1
    bne du_moved
    pop {pc}
du_moved:
    ldr r0, =pf_pc
    mov r1, #1
    str r1, [r0]
    pop {pc}
.ltorg

dostris_lock:
    push {r4-r7, lr}
    ldr r0, =g_type
    ldr r0, [r0]
    ldr r1, =piece_tiles
    ldrb r0, [r1, r0]
    ldr r1, =g_lt
    str r0, [r1]
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_tx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_ty
    str r0, [r1]
    bl get_mask
    ldr r4, =mlo
    ldr r4, [r4]
    mov r5, #0
dl_loop:
    tst r4, #0x8000
    beq dl_next
    and r0, r5, #3
    ldr r1, =g_tx
    ldr r1, [r1]
    add r6, r0, r1            @ bx
    and r0, r5, #12
    mov r0, r0, lsr #2
    ldr r1, =g_ty
    ldr r1, [r1]
    add r0, r0, r1           @ by
    mov r1, #BW
    mul r7, r1, r0
    add r7, r7, r6           @ by*BW+bx
    ldr r0, =g_lt
    ldr r0, [r0]
    ldr r1, =g_board
    strb r0, [r1, r7]
dl_next:
    mov r4, r4, lsl #1
    add r5, r5, #1
    cmp r5, #16
    bne dl_loop
    pop {r4-r7, pc}
.ltorg

dostris_spawn:
    push {lr}
    bl rng
    @ mod 7
ds_mod:
    cmp r0, #7
    subhs r0, r0, #7
    bhs ds_mod
    ldr r1, =g_type
    str r0, [r1]
    mov r0, #0
    ldr r1, =g_rot
    str r0, [r1]
    ldr r1, =g_fall
    str r0, [r1]
    mov r0, #3
    ldr r1, =g_px
    str r0, [r1]
    ldr r1, =g_tx
    str r0, [r1]
    mov r0, #0
    ldr r1, =g_py
    str r0, [r1]
    ldr r1, =g_ty
    str r0, [r1]
    bl piece_collide
    cmp r0, #0
    beq sp_ok
    ldr r0, =g_state
    mov r1, #1
    str r1, [r0]
sp_ok:
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_oldpx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_oldpy
    str r0, [r1]
    ldr r0, =g_rot
    ldr r0, [r0]
    ldr r1, =g_oldrot
    str r0, [r1]
    pop {pc}
.ltorg

rng:
    ldr r1, =g_seed
    ldr r0, [r1]
    mov r2, r0, lsl #2
    add r0, r2, r0           @ *5
    add r0, r0, #1
    str r0, [r1]
    and r0, r0, #0xFF
    bx lr
.ltorg

dostris_init:
    push {r4, lr}
    ldr r0, =g_board
    mov r1, #0
    mov r2, #(BW*BH)
di_clr:
    strb r1, [r0], #1
    subs r2, r2, #1
    bne di_clr
    mov r1, #0
    ldr r0, =g_lines
    str r1, [r0]
    ldr r0, =g_state
    str r1, [r0]
    ldr r0, =g_fall
    str r1, [r0]
    ldr r0, =REG_VCOUNT       @ seed from a changing source
    ldrh r1, [r0]
    orr r1, r1, #1
    ldr r0, =g_seed
    str r1, [r0]
    bl dostris_spawn
    pop {r4, pc}
.ltorg

dostris_clearlines:
    push {r4, lr}
    mov r4, #(BH-1)
cl_loop:
    cmp r4, #0
    blt cl_done
    ldr r0, =g_row
    str r4, [r0]
    bl row_full
    cmp r0, #0
    beq cl_dec
    bl collapse_row
    ldr r0, =g_lines
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    b cl_loop                @ recheck same row
cl_dec:
    sub r4, r4, #1
    b cl_loop
cl_done:
    pop {r4, pc}
.ltorg

@ row_full: row in g_row -> r0=1 if full else 0
row_full:
    push {r4-r6, lr}
    ldr r0, =g_row
    ldr r0, [r0]
    mov r1, #BW
    mul r4, r1, r0           @ row*BW
    ldr r5, =g_board
    add r5, r5, r4
    mov r6, #BW
rf_loop:
    ldrb r0, [r5], #1
    cmp r0, #0
    beq rf_no
    subs r6, r6, #1
    bne rf_loop
    mov r0, #1
    pop {r4-r6, pc}
rf_no:
    mov r0, #0
    pop {r4-r6, pc}
.ltorg

@ collapse_row: drop rows above g_row down by one, clear row 0
collapse_row:
    push {r4-r7, lr}
    ldr r0, =g_row
    ldr r4, [r0]            @ r = g_row
cr_loop:
    cmp r4, #0
    beq cr_top
    @ dst = r*BW, src = (r-1)*BW
    mov r1, #BW
    mul r5, r1, r4          @ dst index
    sub r6, r5, #BW         @ src index
    ldr r7, =g_board
    mov r2, #BW
cr_copy:
    ldrb r0, [r7, r6]
    strb r0, [r7, r5]
    add r5, r5, #1
    add r6, r6, #1
    subs r2, r2, #1
    bne cr_copy
    sub r4, r4, #1
    b cr_loop
cr_top:
    ldr r7, =g_board
    mov r0, #0
    mov r2, #BW
cr_clr:
    strb r0, [r7], #1
    subs r2, r2, #1
    bne cr_clr
    pop {r4-r7, pc}
.ltorg

@ ============================================================================
@ rendering
@ ============================================================================
@ dcell: r0=bx (signed) r1=by r2=colour idx -> 8x8 cell
dcell:
    push {r4, lr}
    ldr r4, =d_fg
    str r2, [r4]
    mov r0, r0, lsl #3
    add r0, r0, #BORG_X
    mov r1, r1, lsl #3
    add r1, r1, #BORG_Y
    mov r2, #8
    mov r3, #8
    bl frect
    pop {r4, pc}
.ltorg

dostris_draw:
    push {r4-r6, lr}
    @ walls (white = idx 1)
    mov r4, #0
dd_walls:
    mvn r0, #0               @ bx = -1 (left wall)
    mov r1, r4
    mov r2, #1
    bl dcell
    mov r0, #BW              @ right wall
    mov r1, r4
    mov r2, #1
    bl dcell
    add r4, r4, #1
    cmp r4, #BH
    bne dd_walls
    @ floor
    mvn r4, #0               @ -1
dd_floor:
    mov r0, r4
    mov r1, #BH
    mov r2, #1
    bl dcell
    add r4, r4, #1
    cmp r4, #(BW+1)
    bne dd_floor
    @ board cells
    mov r4, #0               @ by
dd_row:
    cmp r4, #BH
    bhs dd_after
    mov r5, #0               @ bx
dd_col:
    cmp r5, #BW
    bhs dd_rowend
    mov r1, #BW
    mul r0, r1, r4
    add r0, r0, r5
    ldr r1, =g_board
    ldrb r6, [r1, r0]        @ cell value (0 or colour idx)
    mov r0, r5
    mov r1, r4
    mov r2, r6
    bl dcell
    add r5, r5, #1
    b dd_col
dd_rowend:
    add r4, r4, #1
    b dd_row
dd_after:
    @ live piece (unless game over)
    ldr r0, =g_state
    ldr r0, [r0]
    cmp r0, #0
    bne dd_score
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_tx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_ty
    str r0, [r1]
    ldr r0, =g_type
    ldr r0, [r0]
    ldr r1, =piece_tiles
    ldrb r0, [r1, r0]
    ldr r1, =g_pt
    str r0, [r1]
    bl draw_piece_cells
dd_score:
    bl dostris_draw_score
    ldr r0, =g_state
    ldr r0, [r0]
    cmp r0, #0
    beq dd_done
    mov r0, #0
    mov r1, #1
    bl setfb
    mov r0, #(BORG_X)
    mov r1, #(BORG_Y + 5*8)
    ldr r2, =s_gameover
    bl pstr
dd_done:
    pop {r4-r6, pc}
.ltorg

dostris_draw_score:
    push {lr}
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #(BORG_X + BW*8 + 8)
    mov r1, #BORG_Y
    ldr r2, =s_lines
    bl pstr
    ldr r0, =g_lines
    ldr r0, [r0]
    bl two_digits           @ r1=tens r0=units
    ldr r2, =numstr
    strb r1, [r2, #0]
    strb r0, [r2, #1]
    mov r0, #0
    strb r0, [r2, #2]
    mov r0, #(BORG_X + BW*8 + 8)
    mov r1, #(BORG_Y + 12)
    ldr r2, =numstr
    bl pstr
    pop {pc}
.ltorg

@ draw_piece_cells: plot g_pt at each cell of mask(g_type,g_rot) at g_tx,g_ty
draw_piece_cells:
    push {r4-r7, lr}
    bl get_mask
    ldr r4, =mlo
    ldr r4, [r4]
    mov r5, #0
dpc_loop:
    tst r4, #0x8000
    beq dpc_next
    and r0, r5, #3
    ldr r1, =g_tx
    ldr r1, [r1]
    add r6, r0, r1          @ bx
    and r0, r5, #12
    mov r0, r0, lsr #2
    ldr r1, =g_ty
    ldr r1, [r1]
    add r7, r0, r1          @ by
    ldr r0, =g_pt
    ldr r2, [r0]
    mov r0, r6
    mov r1, r7
    bl dcell
dpc_next:
    mov r4, r4, lsl #1
    add r5, r5, #1
    cmp r5, #16
    bne dpc_loop
    pop {r4-r7, pc}
.ltorg

draw_piece_partial:
    push {lr}
    @ erase old pose (colour 0 = desktop) using old rotation
    ldr r0, =g_oldpx
    ldr r0, [r0]
    ldr r1, =g_tx
    str r0, [r1]
    ldr r0, =g_oldpy
    ldr r0, [r0]
    ldr r1, =g_ty
    str r0, [r1]
    ldr r0, =g_rot
    ldr r0, [r0]
    mov r2, r0               @ save current rot
    ldr r0, =g_oldrot
    ldr r0, [r0]
    ldr r1, =g_rot
    str r0, [r1]
    ldr r0, =g_pt
    mov r3, #0
    str r3, [r0]
    push {r2}
    bl draw_piece_cells
    pop {r2}
    ldr r1, =g_rot
    str r2, [r1]
    @ draw new pose
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_tx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_ty
    str r0, [r1]
    ldr r0, =g_type
    ldr r0, [r0]
    ldr r1, =piece_tiles
    ldrb r0, [r1, r0]
    ldr r1, =g_pt
    str r0, [r1]
    bl draw_piece_cells
    @ old = new
    ldr r0, =g_px
    ldr r0, [r0]
    ldr r1, =g_oldpx
    str r0, [r1]
    ldr r0, =g_py
    ldr r0, [r0]
    ldr r1, =g_oldpy
    str r0, [r1]
    ldr r0, =g_rot
    ldr r0, [r0]
    ldr r1, =g_oldrot
    str r0, [r1]
    pop {pc}
.ltorg

.section .rodata
s_lines:    .asciz "Ln"
s_gameover: .asciz "GAME OVER"
.section .text
