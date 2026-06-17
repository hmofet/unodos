// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — Dostris (falling-blocks). The shared
// algorithm; the board renders as 16x16 colour cells into the mailbox framebuffer.
// ============================================================================

// get_mask: g_type,g_rot -> mlo (16-bit shape mask). Leaf.
get_mask:
    ldr   x0, =g_type
    ldr   w0, [x0]
    lsl   w0, w0, #2
    ldr   x1, =g_rot
    ldr   w1, [x1]
    add   w0, w0, w1
    ldr   x1, =piece_masks
    add   x1, x1, w0, uxtw #1
    ldrh  w0, [x1]
    ldr   x1, =mlo
    str   w0, [x1]
    ret

// piece_collide: trial g_tx,g_ty -> w0=1 collide / 0 free
piece_collide:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    bl    get_mask
    ldr   x19, =mlo
    ldr   w19, [x19]
    mov   w20, #0
pc_loop:
    tst   w19, #0x8000
    b.eq  pc_next
    and   w0, w20, #3
    ldr   x1, =g_tx
    ldr   w1, [x1]
    add   w21, w0, w1                     // bx
    cmp   w21, #BW
    b.hs  pc_yes
    lsr   w0, w20, #2
    and   w0, w0, #3
    ldr   x1, =g_ty
    ldr   w1, [x1]
    add   w22, w0, w1                     // by
    cmp   w22, #BH
    b.hs  pc_yes
    mov   w1, #BW
    mul   w0, w22, w1
    add   w0, w0, w21                     // idx = by*BW+bx
    ldr   x1, =g_board
    ldrb  w0, [x1, w0, uxtw]
    cbnz  w0, pc_yes
pc_next:
    lsl   w19, w19, #1
    add   w20, w20, #1
    cmp   w20, #16
    b.ne  pc_loop
    mov   w0, #0
    b     pc_ret
pc_yes:
    mov   w0, #1
pc_ret:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// moves
ds_left:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =g_px
    ldr   w0, [x0]
    sub   w0, w0, #1
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_ty
    str   w0, [x1]
    bl    piece_collide
    cbnz  w0, dsl_done
    ldr   x0, =g_tx
    ldr   w0, [x0]
    ldr   x1, =g_px
    str   w0, [x1]
dsl_done:
    ldp   x29, x30, [sp], #16
    ret
ds_right:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =g_px
    ldr   w0, [x0]
    add   w0, w0, #1
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_ty
    str   w0, [x1]
    bl    piece_collide
    cbnz  w0, dsr_done
    ldr   x0, =g_tx
    ldr   w0, [x0]
    ldr   x1, =g_px
    str   w0, [x1]
dsr_done:
    ldp   x29, x30, [sp], #16
    ret
ds_rotate:
    stp   x29, x30, [sp, #-16]!
    ldr   x2, =g_rot
    ldr   w0, [x2]
    ldr   x1, =g_srot
    str   w0, [x1]
    add   w0, w0, #1
    and   w0, w0, #3
    str   w0, [x2]
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_ty
    str   w0, [x1]
    bl    piece_collide
    cbz   w0, dsrot_done
    ldr   x0, =g_srot
    ldr   w0, [x0]
    ldr   x1, =g_rot
    str   w0, [x1]
dsrot_done:
    ldp   x29, x30, [sp], #16
    ret
ds_softdrop:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    add   w0, w0, #1
    ldr   x1, =g_ty
    str   w0, [x1]
    bl    piece_collide
    cbnz  w0, dsd_done
    ldr   x0, =g_py
    ldr   w1, [x0]
    add   w1, w1, #1
    str   w1, [x0]
dsd_done:
    ldp   x29, x30, [sp], #16
    ret

// dostris_update: one frame of game logic
dostris_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =g_state
    ldr   w0, [x0]
    cbnz  w0, du_ret
    // save old pose
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_oldpx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_oldpy
    str   w0, [x1]
    ldr   x0, =g_rot
    ldr   w0, [x0]
    ldr   x1, =g_oldrot
    str   w0, [x1]
    // input edges
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_L
    b.eq  du_i1
    bl    ds_left
du_i1:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_R
    b.eq  du_i2
    bl    ds_right
du_i2:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_U
    b.eq  du_i3
    bl    ds_rotate
du_i3:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  du_i4
    bl    ds_rotate
du_i4:
    ldr   x0, =v_pad
    ldr   w0, [x0]
    tst   w0, #PAD_D
    b.eq  du_grav
    bl    ds_softdrop
du_grav:
    // gravity (AUTOTEST freezes it once the script ends)
    ldr   x0, =a_gpause
    ldr   w0, [x0]
    cbnz  w0, du_check
    ldr   x3, =g_fall
    ldr   w0, [x3]
    add   w0, w0, #1
    str   w0, [x3]
    cmp   w0, #FALLRATE
    b.lo  du_check
    str   wzr, [x3]
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    add   w0, w0, #1
    ldr   x1, =g_ty
    str   w0, [x1]
    bl    piece_collide
    cbz   w0, du_fall
    bl    dostris_lock
    bl    dostris_clearlines
    bl    dostris_spawn
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =pf_pc
    str   wzr, [x0]
    b     du_ret
du_fall:
    ldr   x0, =g_py
    ldr   w1, [x0]
    add   w1, w1, #1
    str   w1, [x0]
du_check:
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_oldpx
    ldr   w1, [x1]
    cmp   w0, w1
    b.ne  du_moved
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_oldpy
    ldr   w1, [x1]
    cmp   w0, w1
    b.ne  du_moved
    ldr   x0, =g_rot
    ldr   w0, [x0]
    ldr   x1, =g_oldrot
    ldr   w1, [x1]
    cmp   w0, w1
    b.ne  du_moved
    b     du_ret
du_moved:
    ldr   x0, =pf_pc
    mov   w1, #1
    str   w1, [x0]
du_ret:
    ldp   x29, x30, [sp], #16
    ret

dostris_lock:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    ldr   x0, =g_type
    ldr   w0, [x0]
    ldr   x1, =piece_tiles
    ldrb  w0, [x1, w0, uxtw]
    ldr   x1, =g_lt
    str   w0, [x1]
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_ty
    str   w0, [x1]
    bl    get_mask
    ldr   x19, =mlo
    ldr   w19, [x19]
    mov   w20, #0
dl_loop:
    tst   w19, #0x8000
    b.eq  dl_next
    and   w0, w20, #3
    ldr   x1, =g_tx
    ldr   w1, [x1]
    add   w21, w0, w1                     // bx
    lsr   w0, w20, #2
    and   w0, w0, #3
    ldr   x1, =g_ty
    ldr   w1, [x1]
    add   w22, w0, w1                     // by
    mov   w1, #BW
    mul   w0, w22, w1
    add   w0, w0, w21                     // by*BW+bx
    ldr   x1, =g_lt
    ldr   w2, [x1]
    ldr   x1, =g_board
    strb  w2, [x1, w0, uxtw]
dl_next:
    lsl   w19, w19, #1
    add   w20, w20, #1
    cmp   w20, #16
    b.ne  dl_loop
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

dostris_spawn:
    stp   x29, x30, [sp, #-16]!
    bl    rng
ds_mod:
    cmp   w0, #7
    b.lo  ds_mod_done
    sub   w0, w0, #7
    b     ds_mod
ds_mod_done:
    ldr   x1, =g_type
    str   w0, [x1]
    ldr   x1, =g_rot
    str   wzr, [x1]
    ldr   x1, =g_fall
    str   wzr, [x1]
    mov   w0, #3
    ldr   x1, =g_px
    str   w0, [x1]
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x1, =g_py
    str   wzr, [x1]
    ldr   x1, =g_ty
    str   wzr, [x1]
    bl    piece_collide
    cbz   w0, sp_ok
    ldr   x0, =g_state
    mov   w1, #1
    str   w1, [x0]
sp_ok:
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_oldpx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_oldpy
    str   w0, [x1]
    ldr   x0, =g_rot
    ldr   w0, [x0]
    ldr   x1, =g_oldrot
    str   w0, [x1]
    ldp   x29, x30, [sp], #16
    ret

rng:
    ldr   x1, =g_seed
    ldr   w0, [x1]
    lsl   w2, w0, #2
    add   w0, w2, w0                      // *5
    add   w0, w0, #1
    str   w0, [x1]
    and   w0, w0, #0xFF
    ret

dostris_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =g_board
    mov   w2, #(BW*BH)
di_clr:
    strb  wzr, [x0], #1
    subs  w2, w2, #1
    b.ne  di_clr
    ldr   x0, =g_lines
    str   wzr, [x0]
    ldr   x0, =g_state
    str   wzr, [x0]
    ldr   x0, =g_fall
    str   wzr, [x0]
    ldr   x0, =SYS_TIMER_CLO              // seed from a changing source
    ldr   w1, [x0]
    orr   w1, w1, #1
    ldr   x0, =g_seed
    str   w1, [x0]
    bl    dostris_spawn
    ldp   x29, x30, [sp], #16
    ret

dostris_clearlines:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #(BH-1)
cl_loop:
    cmp   w19, #0
    b.lt  cl_done
    ldr   x0, =g_row
    str   w19, [x0]
    bl    row_full
    cbz   w0, cl_dec
    bl    collapse_row
    ldr   x0, =g_lines
    ldr   w1, [x0]
    add   w1, w1, #1
    str   w1, [x0]
    b     cl_loop                         // recheck same row
cl_dec:
    sub   w19, w19, #1
    b     cl_loop
cl_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// row_full: row in g_row -> w0=1 if full else 0
row_full:
    ldr   x0, =g_row
    ldr   w0, [x0]
    mov   w1, #BW
    mul   w4, w0, w1                      // row*BW
    ldr   x5, =g_board
    add   x5, x5, w4, uxtw
    mov   w6, #BW
rf_loop:
    ldrb  w0, [x5], #1
    cbz   w0, rf_no
    subs  w6, w6, #1
    b.ne  rf_loop
    mov   w0, #1
    ret
rf_no:
    mov   w0, #0
    ret

// collapse_row: drop rows above g_row down by one, clear row 0
collapse_row:
    ldr   x0, =g_row
    ldr   w4, [x0]                        // r = g_row
cr_loop:
    cbz   w4, cr_top
    mov   w1, #BW
    mul   w5, w4, w1                      // dst index
    sub   w6, w5, #BW                     // src index
    ldr   x7, =g_board
    mov   w2, #BW
cr_copy:
    ldrb  w0, [x7, w6, uxtw]
    strb  w0, [x7, w5, uxtw]
    add   w5, w5, #1
    add   w6, w6, #1
    subs  w2, w2, #1
    b.ne  cr_copy
    sub   w4, w4, #1
    b     cr_loop
cr_top:
    ldr   x7, =g_board
    mov   w2, #BW
cr_clr:
    strb  wzr, [x7], #1
    subs  w2, w2, #1
    b.ne  cr_clr
    ret

// ============================================================================
// rendering
// ============================================================================
// dcell: w0=bx (signed) w1=by w2=colour idx -> 16x16 cell
dcell:
    stp   x29, x30, [sp, #-16]!
    ldr   x4, =d_fg
    str   w2, [x4]
    lsl   w0, w0, #4
    add   w0, w0, #BORG_X
    lsl   w1, w1, #4
    add   w1, w1, #BORG_Y
    mov   w2, #CELL
    mov   w3, #CELL
    bl    frect
    ldp   x29, x30, [sp], #16
    ret

dostris_draw:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    // walls (white = idx 1)
    mov   w19, #0
dd_walls:
    mov   w0, #-1                         // left wall (bx = -1)
    mov   w1, w19
    mov   w2, #1
    bl    dcell
    mov   w0, #BW                         // right wall
    mov   w1, w19
    mov   w2, #1
    bl    dcell
    add   w19, w19, #1
    cmp   w19, #BH
    b.ne  dd_walls
    // floor
    mov   w19, #-1
dd_floor:
    mov   w0, w19
    mov   w1, #BH
    mov   w2, #1
    bl    dcell
    add   w19, w19, #1
    cmp   w19, #(BW+1)
    b.ne  dd_floor
    // board cells
    mov   w19, #0                         // by
dd_row:
    cmp   w19, #BH
    b.hs  dd_after
    mov   w20, #0                         // bx
dd_col:
    cmp   w20, #BW
    b.hs  dd_rowend
    mov   w1, #BW
    mul   w0, w19, w1
    add   w0, w0, w20
    ldr   x1, =g_board
    ldrb  w2, [x1, w0, uxtw]
    mov   w0, w20
    mov   w1, w19
    bl    dcell
    add   w20, w20, #1
    b     dd_col
dd_rowend:
    add   w19, w19, #1
    b     dd_row
dd_after:
    // live piece (unless game over)
    ldr   x0, =g_state
    ldr   w0, [x0]
    cbnz  w0, dd_score
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_ty
    str   w0, [x1]
    ldr   x0, =g_type
    ldr   w0, [x0]
    ldr   x1, =piece_tiles
    ldrb  w0, [x1, w0, uxtw]
    ldr   x1, =g_pt
    str   w0, [x1]
    bl    draw_piece_cells
dd_score:
    bl    dostris_draw_score
    ldr   x0, =g_state
    ldr   w0, [x0]
    cbz   w0, dd_done
    mov   w0, #0
    mov   w1, #1
    bl    setfb
    mov   w0, #BORG_X
    mov   w1, #(BORG_Y + 6*CELL)
    ldr   x2, =s_gameover
    bl    pstr
dd_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

dostris_draw_score:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #(BORG_X + BW*CELL + 16)
    mov   w1, #BORG_Y
    ldr   x2, =s_lines
    bl    pstr
    ldr   x0, =g_lines
    ldr   w0, [x0]
    bl    two_digits
    ldr   x2, =numstr
    strb  w1, [x2, #0]
    strb  w0, [x2, #1]
    strb  wzr, [x2, #2]
    mov   w0, #(BORG_X + BW*CELL + 16)
    mov   w1, #(BORG_Y + 20)
    ldr   x2, =numstr
    bl    pstr
    ldp   x29, x30, [sp], #16
    ret

// draw_piece_cells: plot g_pt at each cell of mask(g_type,g_rot) at g_tx,g_ty
draw_piece_cells:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    bl    get_mask
    ldr   x19, =mlo
    ldr   w19, [x19]
    mov   w20, #0
dpc_loop:
    tst   w19, #0x8000
    b.eq  dpc_next
    and   w0, w20, #3
    ldr   x1, =g_tx
    ldr   w1, [x1]
    add   w21, w0, w1                     // bx
    lsr   w0, w20, #2
    and   w0, w0, #3
    ldr   x1, =g_ty
    ldr   w1, [x1]
    add   w22, w0, w1                     // by
    ldr   x0, =g_pt
    ldr   w2, [x0]
    mov   w0, w21
    mov   w1, w22
    bl    dcell
dpc_next:
    lsl   w19, w19, #1
    add   w20, w20, #1
    cmp   w20, #16
    b.ne  dpc_loop
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

draw_piece_partial:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    // erase old pose (colour 0 = desktop) using old rotation
    ldr   x0, =g_oldpx
    ldr   w0, [x0]
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x0, =g_oldpy
    ldr   w0, [x0]
    ldr   x1, =g_ty
    str   w0, [x1]
    ldr   x0, =g_rot
    ldr   w19, [x0]                       // save current rot
    ldr   x0, =g_oldrot
    ldr   w0, [x0]
    ldr   x1, =g_rot
    str   w0, [x1]
    ldr   x0, =g_pt
    str   wzr, [x0]
    bl    draw_piece_cells
    ldr   x1, =g_rot
    str   w19, [x1]                       // restore current rot
    // draw new pose
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_tx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_ty
    str   w0, [x1]
    ldr   x0, =g_type
    ldr   w0, [x0]
    ldr   x1, =piece_tiles
    ldrb  w0, [x1, w0, uxtw]
    ldr   x1, =g_pt
    str   w0, [x1]
    bl    draw_piece_cells
    // old = new
    ldr   x0, =g_px
    ldr   w0, [x0]
    ldr   x1, =g_oldpx
    str   w0, [x1]
    ldr   x0, =g_py
    ldr   w0, [x0]
    ldr   x1, =g_oldpy
    str   w0, [x1]
    ldr   x0, =g_rot
    ldr   w0, [x0]
    ldr   x1, =g_oldrot
    str   w0, [x1]
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
s_lines:    .asciz "Ln"
s_gameover: .asciz "GAME OVER"
.section .text
