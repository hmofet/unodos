# ============================================================================
# UnoDOS / PowerPC Mac — OutLast. PowerPC translation of the rpi pseudo-3D racer
# (20 perspective bands, snaking road, steerable car, off-road = CRASH). 640x480.
# ============================================================================

ol_band_geom:                            # r3=band -> r3=left r4=right. Leaf.
    srwi  r5, r3, 1
    addi  r5, r5, 3                       # hw
    LWZA  r6, ol_scroll
    add   r6, r6, r3
    andi. r6, r6, 15
    cmpwi r6, 8
    blt   obg_pos
    subi  r6, r6, 16
obg_pos:
    addi  r6, r6, 20                      # centre
    subf  r3, r5, r6                      # left
    cmpwi r3, 0
    bge   obg_l
    li    r3, 0
obg_l:
    add   r4, r6, r5                      # right
    cmpwi r4, 40
    blt   obg_r
    li    r4, 39
obg_r:
    blr

outlast_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 20
    STWA  r3, ol_carx, r4
    li    r3, 0
    STWA  r3, ol_scroll, r4
    STWA  r3, ol_dist, r4
    STWA  r3, ol_over, r4
    li    r3, OL_RATE
    STWA  r3, ol_ctr, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

ol_check:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, (OL_BANDS-1)
    bl    ol_band_geom
    LWZA  r5, ol_carx
    cmpw  r5, r3
    blt   olc_off
    cmpw  r5, r4
    bgt   olc_off
    b     olc_out
olc_off:
    li    r3, 1
    STWA  r3, ol_over, r4
olc_out:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

outlast_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, ol_over
    cmpwi r3, 0
    bne   ou_done
    LWZA  r3, v_pade
    andi. r0, r3, PAD_L
    beq   ou_r
    LWZA  r4, ol_carx
    cmpwi r4, 0
    beq   ou_r
    subi  r4, r4, 1
    STWA  r4, ol_carx, r5
    li    r4, 1
    STWA  r4, v_dirty, r5
ou_r:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_R
    beq   ou_pace
    LWZA  r4, ol_carx
    cmpwi r4, 39
    bge   ou_pace
    addi  r4, r4, 1
    STWA  r4, ol_carx, r5
    li    r4, 1
    STWA  r4, v_dirty, r5
ou_pace:
    LWZA  r3, a_gpause
    cmpwi r3, 0
    bne   ou_done
    LWZA  r3, ol_ctr
    subi  r3, r3, 1
    STWA  r3, ol_ctr, r4
    cmpwi r3, 0
    bne   ou_done
    li    r3, OL_RATE
    STWA  r3, ol_ctr, r4
    LWZA  r3, ol_scroll
    addi  r3, r3, 1
    STWA  r3, ol_scroll, r4
    LWZA  r3, ol_dist
    addi  r3, r3, 1
    STWA  r3, ol_dist, r4
    bl    ol_check
    li    r3, 1
    STWA  r3, v_dirty, r4
ou_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

ol_draw_road:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    li    r14, 0                          # band
odr_loop:
    cmpwi r14, OL_BANDS
    bge   odr_car
    mr    r3, r14
    bl    ol_band_geom
    mr    r15, r3                         # left
    mr    r16, r4                         # right
    mulli r17, r14, OL_BH
    addi  r17, r17, OLO_Y                 # y
    li    r3, 5
    bl    set_fg
    li    r3, 0
    mr    r4, r17
    li    r5, 640
    li    r6, OL_BH
    bl    frect
    li    r3, 9
    bl    set_fg
    slwi  r3, r15, 4
    mr    r4, r17
    subf  r5, r15, r16
    addi  r5, r5, 1
    slwi  r5, r5, 4
    li    r6, OL_BH
    bl    frect
    LWZA  r3, ol_scroll
    add   r3, r3, r14
    andi. r0, r3, 1
    beq   odr_nostripe
    li    r3, 1
    bl    set_fg
    slwi  r3, r15, 4
    mr    r4, r17
    li    r5, OL_COLW
    li    r6, OL_BH
    bl    frect
    li    r3, 1
    bl    set_fg
    slwi  r3, r16, 4
    mr    r4, r17
    li    r5, OL_COLW
    li    r6, OL_BH
    bl    frect
odr_nostripe:
    addi  r14, r14, 1
    b     odr_loop
odr_car:
    li    r3, 7
    bl    set_fg
    LWZA  r3, ol_carx
    slwi  r3, r3, 4
    li    r4, (OL_BH*(OL_BANDS-1)+OLO_Y)
    li    r5, OL_COLW
    li    r6, OL_BH
    bl    frect
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

ol_draw_hud:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 470
    li    r4, 40
    LA    r5, s_oldist
    bl    pstr
    LWZA  r3, ol_dist
    bl    pm_itoa5
    li    r3, 470
    li    r4, 60
    LA    r5, numstr
    bl    pstr
    LWZA  r3, ol_over
    cmpwi r3, 0
    beq   olh_done
    li    r3, 470
    li    r4, 90
    LA    r5, s_olcrash
    bl    pstr
olh_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

app_outlast:
    LA    r5, t_outlast
    bl    draw_chrome
    bl    ol_draw_road
    bl    ol_draw_hud
    b     da_ret

.align 2
s_oldist:  .asciz "DIST"
s_olcrash: .asciz "CRASH!"
.align 2
