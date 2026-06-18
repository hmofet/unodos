# ============================================================================
# UnoDOS / PowerPC Mac — Paint. PowerPC translation of the rpi cursor-driven
# cell canvas + palette row. 640x480, same layout as rpi.
# ============================================================================

paint_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r3, pcanvas
    li    r4, (PCW*PCH)
    mtctr r4
pin_clr:
    li    r0, 0
    stb   r0, 0(r3)
    addi  r3, r3, 1
    bdnz  pin_clr
    li    r3, 0
    STWA  r3, p_cx, r4
    STWA  r3, p_cy, r4
    li    r3, 1
    STWA  r3, p_col, r4
    li    r3, 1
    STWA  r3, v_dirty, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

paint_put:                               # write p_col at (p_cx,p_cy). Leaf.
    LWZA  r3, p_cy
    cmpwi r3, PCH
    bge   pp_ret
    mulli r3, r3, PCW
    LWZA  r4, p_cx
    add   r3, r3, r4
    LWZA  r4, p_col
    LA    r5, pcanvas
    stbx  r4, r5, r3
pp_ret:
    blr

paint_clampx:                            # clamp p_cx into canvas range. Leaf.
    LWZA  r3, p_cx
    cmpwi r3, PCW
    blt   pcx_ok
    li    r3, (PCW-1)
    STWA  r3, p_cx, r4
pcx_ok:
    blr

paint_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    stw   r14, 8(r1)
    LWZA  r14, v_pade                     # edges (survive bl calls)
    LWZA  r3, p_cy
    cmpwi r3, PCH
    beq   pu_pal
    andi. r0, r14, PAD_L
    beq   pu_cr
    LWZA  r3, p_cx
    cmpwi r3, 0
    beq   pu_cr
    subi  r3, r3, 1
    STWA  r3, p_cx, r4
pu_cr:
    andi. r0, r14, PAD_R
    beq   pu_cu
    LWZA  r3, p_cx
    addi  r3, r3, 1
    cmpwi r3, PCW
    bge   pu_cu
    STWA  r3, p_cx, r4
pu_cu:
    andi. r0, r14, PAD_U
    beq   pu_cd
    LWZA  r3, p_cy
    cmpwi r3, 0
    beq   pu_topal
    subi  r3, r3, 1
    STWA  r3, p_cy, r4
    b     pu_cd
pu_topal:
    li    r3, PCH
    STWA  r3, p_cy, r4
pu_cd:
    andi. r0, r14, PAD_D
    beq   pu_ca
    LWZA  r3, p_cy
    addi  r4, r3, 1
    cmpwi r4, PCH
    blt   pu_dok
    li    r4, PCH
pu_dok:
    STWA  r4, p_cy, r5
pu_ca:
    andi. r0, r14, PAD_A
    beq   pu_fin
    bl    paint_put
    b     pu_fin
pu_pal:
    andi. r0, r14, PAD_L
    beq   pu_pr
    LWZA  r3, p_cx
    cmpwi r3, 0
    beq   pu_pr
    subi  r3, r3, 1
    STWA  r3, p_cx, r4
pu_pr:
    andi. r0, r14, PAD_R
    beq   pu_pu
    LWZA  r3, p_cx
    addi  r3, r3, 1
    cmpwi r3, NSWATCH
    bge   pu_pu
    STWA  r3, p_cx, r4
pu_pu:
    andi. r0, r14, PAD_U
    beq   pu_pd
    li    r3, (PCH-1)
    STWA  r3, p_cy, r4
    bl    paint_clampx
pu_pd:
    andi. r0, r14, PAD_D
    beq   pu_pa
    li    r3, 0
    STWA  r3, p_cy, r4
    bl    paint_clampx
pu_pa:
    andi. r0, r14, PAD_A
    beq   pu_fin
    LWZA  r3, p_cx
    LA    r4, sw_cols
    lbzx  r3, r4, r3
    STWA  r3, p_col, r4
pu_fin:
    andi. r0, r14, 0xF1
    beq   pu_ret
    li    r3, 1
    STWA  r3, v_dirty, r4
pu_ret:
    lwz   r14, 8(r1)
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# rect_outline: r3=x r4=y r5=w r6=h r7=colour -> 2px frame.
rect_outline:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    mr    r14, r3
    mr    r15, r4
    mr    r16, r5
    mr    r17, r6
    mr    r3, r7
    bl    set_fg
    mr    r3, r14
    mr    r4, r15
    mr    r5, r16
    li    r6, 2
    bl    frect
    mr    r3, r14
    add   r4, r15, r17
    subi  r4, r4, 2
    mr    r5, r16
    li    r6, 2
    bl    frect
    mr    r3, r14
    mr    r4, r15
    li    r5, 2
    mr    r6, r17
    bl    frect
    add   r3, r14, r16
    subi  r3, r3, 2
    mr    r4, r15
    li    r5, 2
    mr    r6, r17
    bl    frect
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

paint_draw_canvas:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    li    r3, (PCO_X-2)
    li    r4, (PCO_Y-2)
    li    r5, (PCW*PCELL+4)
    li    r6, (PCH*PCELL+4)
    li    r7, 9
    bl    rect_outline
    li    r14, 0
pdc_row:
    cmpwi r14, PCH
    bge   pdc_done
    li    r15, 0
pdc_col:
    cmpwi r15, PCW
    bge   pdc_rowend
    mulli r3, r14, PCW
    add   r3, r3, r15
    LA    r4, pcanvas
    lbzx  r16, r4, r3
    cmpwi r16, 0
    beq   pdc_next
    mr    r3, r16
    bl    set_fg
    mulli r3, r15, PCELL
    addi  r3, r3, PCO_X
    mulli r4, r14, PCELL
    addi  r4, r4, PCO_Y
    li    r5, PCELL
    li    r6, PCELL
    bl    frect
pdc_next:
    addi  r15, r15, 1
    b     pdc_col
pdc_rowend:
    addi  r14, r14, 1
    b     pdc_row
pdc_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

paint_draw_palette:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    li    r14, 0
pdp_l:
    cmpwi r14, NSWATCH
    bge   pdp_done
    LA    r3, sw_cols
    lbzx  r15, r3, r14
    mr    r3, r15
    bl    set_fg
    mulli r3, r14, PSW_W
    addi  r3, r3, PCO_X
    li    r4, PSW_Y
    li    r5, (PSW_W-2)
    li    r6, 18
    bl    frect
    addi  r14, r14, 1
    b     pdp_l
pdp_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

paint_draw_cursor:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, p_cy
    cmpwi r3, PCH
    beq   pcur_pal
    LWZA  r4, p_cx
    mulli r3, r4, PCELL
    addi  r3, r3, PCO_X
    LWZA  r4, p_cy
    mulli r4, r4, PCELL
    addi  r4, r4, PCO_Y
    li    r5, PCELL
    li    r6, PCELL
    li    r7, 6
    bl    rect_outline
    b     pcur_done
pcur_pal:
    LWZA  r4, p_cx
    mulli r3, r4, PSW_W
    addi  r3, r3, PCO_X
    li    r4, PSW_Y
    li    r5, (PSW_W-2)
    li    r6, 18
    li    r7, 6
    bl    rect_outline
pcur_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

paint_draw_curcol:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 480
    li    r4, 48
    LA    r5, s_pcolor
    bl    pstr
    LWZA  r3, p_col
    bl    set_fg
    li    r3, 480
    li    r4, 70
    li    r5, 48
    li    r6, 32
    bl    frect
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 480
    li    r4, 120
    LA    r5, s_phint1
    bl    pstr
    li    r3, 480
    li    r4, 140
    LA    r5, s_phint2
    bl    pstr
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

app_paint:
    LA    r5, t_paint
    bl    draw_chrome
    bl    paint_draw_canvas
    bl    paint_draw_palette
    bl    paint_draw_curcol
    bl    paint_draw_cursor
    b     da_ret

.align 2
sw_cols:  .byte 1, 2, 3, 5, 6, 7, 8, 9
.align 2
s_pcolor: .asciz "Colour:"
s_phint1: .asciz "A = paint"
s_phint2: .asciz "U/D = palette"
.align 2
