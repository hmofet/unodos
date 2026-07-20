# ============================================================================
# UnoDOS / PowerPC Mac — Paint. A 30x22 cell canvas (16px cells), an 8-colour
# palette bar, and a movable brush cursor, in the port's minimal d-pad+A style:
#
#   d-pad   move the brush (U from the top row climbs onto the palette bar)
#   A       on the canvas: plot (hold + move = drag-paint)
#           on the palette bar: pick that colour
#   B       back to the launcher
# ============================================================================

.equ PTCOLS, 30
.equ PTROWS, 22
.equ PTX0,   32
.equ PTY0,   48

paint_init:
    LA    r3, pt_canvas
    li    r4, (PTCOLS*PTROWS)
    mtctr r4
pti_c:
    li    r0, 4                           # canvas starts black
    stb   r0, 0(r3)
    addi  r3, r3, 1
    bdnz  pti_c
    li    r3, 6
    STWA  r3, pt_cx, r4
    li    r3, 3
    STWA  r3, pt_cy, r4
    li    r3, 1
    STWA  r3, pt_col, r4
    li    r3, 0
    STWA  r3, pt_pf, r4
    blr

pt_saveold:
    LWZA  r3, pt_cx
    STWA  r3, pt_ocx, r4
    LWZA  r3, pt_cy
    STWA  r3, pt_ocy, r4
    blr
pt_markmove:
    LWZA  r3, pt_pf
    ori   r3, r3, 2
    STWA  r3, pt_pf, r4
    blr

paint_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_pade
    andi. r0, r3, PAD_L
    beq   ptu_1
    bl    pt_saveold
    LWZA  r3, pt_cx
    cmpwi r3, 0
    beq   ptu_1
    subi  r3, r3, 1
    STWA  r3, pt_cx, r4
    bl    pt_markmove
ptu_1:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_R
    beq   ptu_2
    bl    pt_saveold
    LWZA  r3, pt_cy
    cmpwi r3, -1
    li    r5, (PTCOLS-1)                  # canvas: max col 29
    bne   ptu_rmax
    li    r5, 7                           # palette: max slot 7
ptu_rmax:
    LWZA  r3, pt_cx
    cmpw  r3, r5
    bge   ptu_2
    addi  r3, r3, 1
    STWA  r3, pt_cx, r4
    bl    pt_markmove
ptu_2:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_U
    beq   ptu_3
    bl    pt_saveold
    LWZA  r3, pt_cy
    cmpwi r3, -1
    beq   ptu_3                           # already on the bar
    cmpwi r3, 0
    bne   ptu_up
    li    r3, -1                          # climb onto the palette bar
    STWA  r3, pt_cy, r4
    LWZA  r3, pt_cx
    srwi  r3, r3, 2
    cmpwi r3, 7
    ble   ptu_uc
    li    r3, 7
ptu_uc:
    STWA  r3, pt_cx, r4
    bl    pt_markmove
    b     ptu_3
ptu_up:
    subi  r3, r3, 1
    STWA  r3, pt_cy, r4
    bl    pt_markmove
ptu_3:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_D
    beq   ptu_4
    bl    pt_saveold
    LWZA  r3, pt_cy
    cmpwi r3, -1
    bne   ptu_dn
    li    r3, 0                           # drop back onto the canvas
    STWA  r3, pt_cy, r4
    LWZA  r3, pt_cx
    slwi  r3, r3, 2
    STWA  r3, pt_cx, r4
    bl    pt_markmove
    b     ptu_4
ptu_dn:
    cmpwi r3, (PTROWS-1)
    bge   ptu_4
    addi  r3, r3, 1
    STWA  r3, pt_cy, r4
    bl    pt_markmove
ptu_4:
    LWZA  r3, pt_cy
    cmpwi r3, -1
    bne   ptu_plot
    LWZA  r3, v_pade                      # palette: A (edge) picks the colour
    andi. r0, r3, PAD_A
    beq   ptu_ret
    LWZA  r3, pt_cx
    addi  r3, r3, 1
    STWA  r3, pt_col, r4
    LWZA  r3, pt_pf
    ori   r3, r3, 4
    STWA  r3, pt_pf, r4
    b     ptu_ret
ptu_plot:
    LWZA  r3, v_pad                       # canvas: A (held) plots
    andi. r0, r3, PAD_A
    beq   ptu_ret
    LWZA  r3, pt_cy
    mulli r3, r3, PTCOLS
    LWZA  r4, pt_cx
    add   r3, r3, r4
    LA    r4, pt_canvas
    LWZA  r5, pt_col
    stbx  r5, r4, r3
    LWZA  r3, pt_pf
    ori   r3, r3, 1
    STWA  r3, pt_pf, r4
ptu_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ============================================================================
# rendering
# ============================================================================
# pt_draw_swatch: r3 = slot 0..7 (self-erasing: repaints its bg margin too)
pt_draw_swatch:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    mr    r14, r3                         # slot
    mulli r15, r3, 40
    addi  r15, r15, PTX0                  # x
    li    r3, 0                           # bg margin
    bl    set_fg
    subi  r3, r15, 2
    li    r4, 22
    li    r5, 36
    li    r6, 22
    bl    frect
    addi  r3, r14, 1                      # the colour itself
    bl    set_fg
    mr    r3, r15
    li    r4, 24
    li    r5, 32
    li    r6, 16
    bl    frect
    LWZA  r3, pt_col                      # selected: white underline
    subi  r3, r3, 1
    cmpw  r3, r14
    bne   pts_cur
    li    r3, 1
    bl    set_fg
    mr    r3, r15
    li    r4, 41
    li    r5, 32
    li    r6, 2
    bl    frect
pts_cur:
    LWZA  r3, pt_cy                       # cursor ring (yellow)
    cmpwi r3, -1
    bne   pts_d
    LWZA  r3, pt_cx
    cmpw  r3, r14
    bne   pts_d
    li    r3, 6
    bl    set_fg
    subi  r3, r15, 2
    li    r4, 22
    li    r5, 36
    li    r6, 2
    bl    frect
    li    r3, 6
    bl    set_fg
    subi  r3, r15, 2
    li    r4, 38
    li    r5, 36
    li    r6, 2
    bl    frect
    li    r3, 6
    bl    set_fg
    subi  r3, r15, 2
    li    r4, 24
    li    r5, 2
    li    r6, 16
    bl    frect
    li    r3, 6
    bl    set_fg
    addi  r3, r15, 32
    li    r4, 24
    li    r5, 2
    li    r6, 16
    bl    frect
pts_d:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

# pt_cell_draw: r3 = cx, r4 = cy (canvas cell; draws the cursor ring if here)
pt_cell_draw:
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    mr    r14, r3                         # cx
    mr    r15, r4                         # cy
    slwi  r16, r3, 4
    addi  r16, r16, PTX0                  # px
    slwi  r17, r4, 4
    addi  r17, r17, PTY0                  # py
    mulli r3, r15, PTCOLS
    add   r3, r3, r14
    LA    r4, pt_canvas
    lbzx  r3, r4, r3
    bl    set_fg
    mr    r3, r16
    mr    r4, r17
    li    r5, 16
    li    r6, 16
    bl    frect
    LWZA  r3, pt_cx                       # cursor here?
    cmpw  r3, r14
    bne   ptc_d
    LWZA  r3, pt_cy
    cmpw  r3, r15
    bne   ptc_d
    li    r3, 1
    bl    set_fg
    mr    r3, r16
    mr    r4, r17
    li    r5, 16
    li    r6, 2
    bl    frect
    li    r3, 1
    bl    set_fg
    mr    r3, r16
    addi  r4, r17, 14
    li    r5, 16
    li    r6, 2
    bl    frect
    li    r3, 1
    bl    set_fg
    mr    r3, r16
    mr    r4, r17
    li    r5, 2
    li    r6, 16
    bl    frect
    li    r3, 1
    bl    set_fg
    addi  r3, r16, 14
    mr    r4, r17
    li    r5, 2
    li    r6, 16
    bl    frect
ptc_d:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

# pt_draw_pos: r3 = cx, r4 = cy — swatch when cy = -1, canvas cell otherwise
pt_draw_pos:
    cmpwi r4, -1
    bne   pt_cell_draw
    b     pt_draw_swatch

paint_partial:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, pt_pf
    andi. r0, r3, 2
    beq   ptp_1
    LWZA  r3, pt_ocx
    LWZA  r4, pt_ocy
    bl    pt_draw_pos
    LWZA  r3, pt_cx
    LWZA  r4, pt_cy
    bl    pt_draw_pos
ptp_1:
    LWZA  r3, pt_pf
    andi. r0, r3, 1
    beq   ptp_2
    LWZA  r3, pt_cx
    LWZA  r4, pt_cy
    bl    pt_draw_pos
ptp_2:
    LWZA  r3, pt_pf
    andi. r0, r3, 4
    beq   ptp_d
    bl    pt_draw_bar
ptp_d:
    li    r3, 0
    STWA  r3, pt_pf, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

pt_draw_bar:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    li    r14, 0
ptb_l:
    mr    r3, r14
    bl    pt_draw_swatch
    addi  r14, r14, 1
    cmpwi r14, 8
    bne   ptb_l
    lwz   r14, 8(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

app_paint_draw:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    bl    pt_draw_bar
    li    r15, 0                          # cy
app_r:
    li    r14, 0                          # cx
app_c:
    mr    r3, r14
    mr    r4, r15
    bl    pt_cell_draw
    addi  r14, r14, 1
    cmpwi r14, PTCOLS
    bne   app_c
    addi  r15, r15, 1
    cmpwi r15, PTROWS
    bne   app_r
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 200
    li    r4, 464
    LA    r5, s_pt_help
    bl    pstr
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

.section .rodata
s_pt_help: .asciz "A=plot / pick colour (bar above)"
.align 2
.section .text
