# ============================================================================
# UnoDOS / PowerPC Mac — Dostris (falling-blocks). The shared algorithm in
# PowerPC; the board renders as 16x16 colour cells into the framebuffer.
# ============================================================================

# get_mask: g_type,g_rot -> mlo (16-bit shape mask). Leaf.
get_mask:
    LWZA  r3, g_type
    slwi  r3, r3, 2
    LWZA  r4, g_rot
    add   r3, r3, r4
    slwi  r3, r3, 1                       # *2 (short table)
    LA    r4, piece_masks
    lhzx  r3, r4, r3
    STWA  r3, mlo, r4
    blr

# piece_collide: trial g_tx,g_ty -> r3=1 collide / 0 free
piece_collide:
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    bl    get_mask
    LWZA  r14, mlo
    li    r15, 0
pc_loop:
    andi. r0, r14, 0x8000
    beq   pc_next
    andi. r16, r15, 3
    LWZA  r3, g_tx
    add   r16, r16, r3                    # bx
    cmplwi r16, BW                        # unsigned: a negative bx (off the left
    bge   pc_yes                          # edge) reads as huge -> collision, not OOB
    srwi  r0, r15, 2
    andi. r17, r0, 3
    LWZA  r3, g_ty
    add   r17, r17, r3                    # by
    cmplwi r17, BH                        # unsigned lower+upper bound in one check
    bge   pc_yes
    mulli r0, r17, BW
    add   r0, r0, r16
    LA    r3, g_board
    lbzx  r0, r3, r0
    cmpwi r0, 0
    bne   pc_yes
pc_next:
    slwi  r14, r14, 1
    addi  r15, r15, 1
    cmpwi r15, 16
    bne   pc_loop
    li    r3, 0
    b     pc_ret
pc_yes:
    li    r3, 1
pc_ret:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

ds_left:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, g_px
    subi  r3, r3, 1
    STWA  r3, g_tx, r4
    LWZA  r3, g_py
    STWA  r3, g_ty, r4
    bl    piece_collide
    cmpwi r3, 0
    bne   dsl_done
    LWZA  r3, g_tx
    STWA  r3, g_px, r4
dsl_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr
ds_right:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, g_px
    addi  r3, r3, 1
    STWA  r3, g_tx, r4
    LWZA  r3, g_py
    STWA  r3, g_ty, r4
    bl    piece_collide
    cmpwi r3, 0
    bne   dsr_done
    LWZA  r3, g_tx
    STWA  r3, g_px, r4
dsr_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr
ds_rotate:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, g_rot
    STWA  r3, g_srot, r4
    addi  r3, r3, 1
    andi. r3, r3, 3
    STWA  r3, g_rot, r4
    LWZA  r3, g_px
    STWA  r3, g_tx, r4
    LWZA  r3, g_py
    STWA  r3, g_ty, r4
    bl    piece_collide
    cmpwi r3, 0
    beq   dsrot_done
    LWZA  r3, g_srot
    STWA  r3, g_rot, r4
dsrot_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr
ds_softdrop:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, g_px
    STWA  r3, g_tx, r4
    LWZA  r3, g_py
    addi  r3, r3, 1
    STWA  r3, g_ty, r4
    bl    piece_collide
    cmpwi r3, 0
    bne   dsd_done
    LWZA  r3, g_py
    addi  r3, r3, 1
    STWA  r3, g_py, r4
dsd_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# dostris_update: one frame of game logic
dostris_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, g_state
    cmpwi r3, 0
    bne   du_ret
    LWZA  r3, g_px
    STWA  r3, g_oldpx, r4
    LWZA  r3, g_py
    STWA  r3, g_oldpy, r4
    LWZA  r3, g_rot
    STWA  r3, g_oldrot, r4
    LWZA  r3, v_pade
    andi. r0, r3, PAD_L
    beq   du_i1
    bl    ds_left
du_i1:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_R
    beq   du_i2
    bl    ds_right
du_i2:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_U
    beq   du_i3
    bl    ds_rotate
du_i3:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_A
    beq   du_i4
    bl    ds_rotate
du_i4:
    LWZA  r3, v_pad
    andi. r0, r3, PAD_D
    beq   du_grav
    bl    ds_softdrop
du_grav:
    LWZA  r3, a_gpause
    cmpwi r3, 0
    bne   du_check
    LWZA  r3, g_fall
    addi  r3, r3, 1
    STWA  r3, g_fall, r4
    cmpwi r3, FALLRATE
    blt   du_check
    li    r3, 0
    STWA  r3, g_fall, r4
    LWZA  r3, g_px
    STWA  r3, g_tx, r4
    LWZA  r3, g_py
    addi  r3, r3, 1
    STWA  r3, g_ty, r4
    bl    piece_collide
    cmpwi r3, 0
    beq   du_fall
    bl    dostris_lock
    bl    dostris_clearlines
    bl    dostris_spawn
    li    r3, 1
    STWA  r3, v_dirty, r4
    li    r3, 0
    STWA  r3, pf_pc, r4
    b     du_ret
du_fall:
    LWZA  r3, g_py
    addi  r3, r3, 1
    STWA  r3, g_py, r4
du_check:
    LWZA  r3, g_px
    LWZA  r4, g_oldpx
    cmpw  r3, r4
    bne   du_moved
    LWZA  r3, g_py
    LWZA  r4, g_oldpy
    cmpw  r3, r4
    bne   du_moved
    LWZA  r3, g_rot
    LWZA  r4, g_oldrot
    cmpw  r3, r4
    bne   du_moved
    b     du_ret
du_moved:
    li    r3, 1
    STWA  r3, pf_pc, r4
du_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

dostris_lock:
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    LWZA  r3, g_type
    LA    r4, piece_tiles
    lbzx  r3, r4, r3
    STWA  r3, g_lt, r4
    LWZA  r3, g_px
    STWA  r3, g_tx, r4
    LWZA  r3, g_py
    STWA  r3, g_ty, r4
    bl    get_mask
    LWZA  r14, mlo
    li    r15, 0
dl_loop:
    andi. r0, r14, 0x8000
    beq   dl_next
    andi. r16, r15, 3
    LWZA  r3, g_tx
    add   r16, r16, r3                    # bx
    srwi  r0, r15, 2
    andi. r17, r0, 3
    LWZA  r3, g_ty
    add   r17, r17, r3                    # by
    mulli r0, r17, BW
    add   r0, r0, r16
    LWZA  r3, g_lt
    LA    r4, g_board
    stbx  r3, r4, r0
dl_next:
    slwi  r14, r14, 1
    addi  r15, r15, 1
    cmpwi r15, 16
    bne   dl_loop
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

dostris_spawn:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    bl    rng
ds_mod:
    cmpwi r3, 7
    blt   ds_mod_done
    subi  r3, r3, 7
    b     ds_mod
ds_mod_done:
    STWA  r3, g_type, r4
    li    r3, 0
    STWA  r3, g_rot, r4
    li    r3, 0
    STWA  r3, g_fall, r4
    li    r3, 3
    STWA  r3, g_px, r4
    STWA  r3, g_tx, r4
    li    r3, 0
    STWA  r3, g_py, r4
    STWA  r3, g_ty, r4
    bl    piece_collide
    cmpwi r3, 0
    beq   sp_ok
    li    r3, 1
    STWA  r3, g_state, r4
sp_ok:
    LWZA  r3, g_px
    STWA  r3, g_oldpx, r4
    LWZA  r3, g_py
    STWA  r3, g_oldpy, r4
    LWZA  r3, g_rot
    STWA  r3, g_oldrot, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# rng: g_seed = g_seed*5 + 1 ; r3 = seed & 0xFF. Leaf.
rng:
    LWZA  r3, g_seed
    slwi  r4, r3, 2
    add   r3, r4, r3                      # *5
    addi  r3, r3, 1
    STWA  r3, g_seed, r4
    andi. r3, r3, 0xFF
    blr

dostris_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r3, g_board
    li    r4, (BW*BH)
    mtctr r4
di_clr:
    li    r0, 0
    stb   r0, 0(r3)
    addi  r3, r3, 1
    bdnz  di_clr
    li    r3, 0
    STWA  r3, g_lines, r4
    li    r3, 0
    STWA  r3, g_state, r4
    li    r3, 0
    STWA  r3, g_fall, r4
    lis   r3, 0x0001                      # seed (deterministic; AUTOTEST drives moves)
    ori   r3, r3, 0x2345
    STWA  r3, g_seed, r4
    bl    dostris_spawn
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

dostris_clearlines:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    li    r14, (BH-1)
cl_loop:
    cmpwi r14, 0
    blt   cl_done
    STWA  r14, g_row, r4
    bl    row_full
    cmpwi r3, 0
    beq   cl_dec
    bl    collapse_row
    LWZA  r3, g_lines
    addi  r3, r3, 1
    STWA  r3, g_lines, r4
    b     cl_loop
cl_dec:
    subi  r14, r14, 1
    b     cl_loop
cl_done:
    lwz   r14, 8(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# row_full: row in g_row -> r3=1 if full else 0. Leaf.
row_full:
    LWZA  r3, g_row
    mulli r3, r3, BW
    LA    r4, g_board
    add   r4, r4, r3
    li    r5, BW
rf_loop:
    lbz   r0, 0(r4)
    cmpwi r0, 0
    beq   rf_no
    addi  r4, r4, 1
    subic. r5, r5, 1
    bne   rf_loop
    li    r3, 1
    blr
rf_no:
    li    r3, 0
    blr

# collapse_row: drop rows above g_row down by one, clear row 0. Leaf.
collapse_row:
    LWZA  r4, g_row                       # r = g_row
cr_loop:
    cmpwi r4, 0
    beq   cr_top
    mulli r5, r4, BW                      # dst index
    subi  r6, r5, BW                      # src index
    LA    r7, g_board
    li    r8, BW
cr_copy:
    lbzx  r0, r7, r6
    stbx  r0, r7, r5
    addi  r5, r5, 1
    addi  r6, r6, 1
    subic. r8, r8, 1
    bne   cr_copy
    subi  r4, r4, 1
    b     cr_loop
cr_top:
    LA    r7, g_board
    li    r8, BW
    li    r0, 0
cr_clr:
    stb   r0, 0(r7)
    addi  r7, r7, 1
    subic. r8, r8, 1
    bne   cr_clr
    blr

# ============================================================================
# rendering
# ============================================================================
# dcell: r3=bx (signed) r4=by r5=colour idx -> 16x16 cell
dcell:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    STWA  r5, d_fg, r6
    slwi  r3, r3, 4
    addi  r3, r3, BORG_X
    slwi  r4, r4, 4
    addi  r4, r4, BORG_Y
    li    r5, CELL
    li    r6, CELL
    bl    frect
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

dostris_draw:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    li    r14, 0
dd_walls:
    li    r3, -1
    mr    r4, r14
    li    r5, 1
    bl    dcell
    li    r3, BW
    mr    r4, r14
    li    r5, 1
    bl    dcell
    addi  r14, r14, 1
    cmpwi r14, BH
    bne   dd_walls
    li    r14, -1
dd_floor:
    mr    r3, r14
    li    r4, BH
    li    r5, 1
    bl    dcell
    addi  r14, r14, 1
    cmpwi r14, (BW+1)
    bne   dd_floor
    li    r14, 0                          # by
dd_row:
    cmpwi r14, BH
    bge   dd_after
    li    r15, 0                          # bx
dd_col:
    cmpwi r15, BW
    bge   dd_rowend
    mulli r0, r14, BW
    add   r0, r0, r15
    LA    r3, g_board
    lbzx  r5, r3, r0                      # cell value
    mr    r3, r15
    mr    r4, r14
    bl    dcell
    addi  r15, r15, 1
    b     dd_col
dd_rowend:
    addi  r14, r14, 1
    b     dd_row
dd_after:
    LWZA  r3, g_state
    cmpwi r3, 0
    bne   dd_score
    LWZA  r3, g_px
    STWA  r3, g_tx, r4
    LWZA  r3, g_py
    STWA  r3, g_ty, r4
    LWZA  r3, g_type
    LA    r4, piece_tiles
    lbzx  r3, r4, r3
    STWA  r3, g_pt, r4
    bl    draw_piece_cells
dd_score:
    bl    dostris_draw_score
    LWZA  r3, g_state
    cmpwi r3, 0
    beq   dd_done
    li    r3, 0
    li    r4, 1
    bl    setfb
    li    r3, BORG_X
    li    r4, (BORG_Y + 6*CELL)
    LA    r5, s_gameover
    bl    pstr
dd_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

dostris_draw_score:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, (BORG_X + BW*CELL + 16)
    li    r4, BORG_Y
    LA    r5, s_lines
    bl    pstr
    LWZA  r3, g_lines
    bl    two_digits
    LA    r5, numstr
    stb   r4, 0(r5)
    stb   r3, 1(r5)
    li    r3, 0
    stb   r3, 2(r5)
    li    r3, (BORG_X + BW*CELL + 16)
    li    r4, (BORG_Y + 20)
    LA    r5, numstr
    bl    pstr
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# draw_piece_cells: plot g_pt at each cell of mask(g_type,g_rot) at g_tx,g_ty
draw_piece_cells:
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    bl    get_mask
    LWZA  r14, mlo
    li    r15, 0
dpc_loop:
    andi. r0, r14, 0x8000
    beq   dpc_next
    andi. r16, r15, 3
    LWZA  r3, g_tx
    add   r16, r16, r3                    # bx
    srwi  r0, r15, 2
    andi. r17, r0, 3
    LWZA  r3, g_ty
    add   r17, r17, r3                    # by
    LWZA  r5, g_pt
    mr    r3, r16
    mr    r4, r17
    bl    dcell
dpc_next:
    slwi  r14, r14, 1
    addi  r15, r15, 1
    cmpwi r15, 16
    bne   dpc_loop
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

draw_piece_partial:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    # erase old pose (colour 0) using old rotation
    LWZA  r3, g_oldpx
    STWA  r3, g_tx, r4
    LWZA  r3, g_oldpy
    STWA  r3, g_ty, r4
    LWZA  r14, g_rot                      # save current rot
    LWZA  r3, g_oldrot
    STWA  r3, g_rot, r4
    li    r3, 0
    STWA  r3, g_pt, r4
    bl    draw_piece_cells
    STWA  r14, g_rot, r4                  # restore current rot
    # draw new pose
    LWZA  r3, g_px
    STWA  r3, g_tx, r4
    LWZA  r3, g_py
    STWA  r3, g_ty, r4
    LWZA  r3, g_type
    LA    r4, piece_tiles
    lbzx  r3, r4, r3
    STWA  r3, g_pt, r4
    bl    draw_piece_cells
    # old = new
    LWZA  r3, g_px
    STWA  r3, g_oldpx, r4
    LWZA  r3, g_py
    STWA  r3, g_oldpy, r4
    LWZA  r3, g_rot
    STWA  r3, g_oldrot, r4
    lwz   r14, 8(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

.section .rodata
s_lines:    .asciz "Ln"
s_gameover: .asciz "GAME OVER"
.section .text
