# ============================================================================
# UnoDOS / PowerPC Mac — OutLast, the pseudo-3D road racer (donor c64/outlast.s).
# The road is drawn as horizontal COLOUR-RUN BANDS straight into the framebuffer:
# 20 perspective bands, each a grass fill + a road-span fill (striped edges by
# parity), a steerable car and a scrolling triangle-wave centre line. Drift off
# the road and it's CRASH — A restarts. Distance is the score.
#
#   L / R (held or tapped)   steer      A  new game after a crash
# ============================================================================

.equ OLBANDS, 20
.equ OLY0,    64                # first band's pixel y (bands are 16px tall)
.equ OLRATE,  4                 # frames per scroll step

outlast_init:
    li    r3, 20
    STWA  r3, ol_carx, r4
    li    r3, 0
    STWA  r3, ol_scroll, r4
    STWA  r3, ol_dist, r4
    STWA  r3, ol_over, r4
    STWA  r3, ol_pf, r4
    li    r3, OLRATE
    STWA  r3, ol_ctr, r4
    blr

# ol_geom: r3 = band (0 top .. 19 bottom) -> r4 = left col, r5 = right col.
# hw grows 3..12 down the screen; centre snakes with a scrolling triangle wave.
# Leaf; clobbers r6-r7.
ol_geom:
    srwi  r6, r3, 1
    addi  r6, r6, 3                       # hw = 3 + band/2
    LWZA  r7, ol_scroll
    add   r7, r7, r3
    andi. r7, r7, 15
    cmpwi r7, 8
    blt   olg_p
    subi  r7, r7, 16                      # -> -8..7
olg_p:
    addi  r7, r7, 20                      # centre = 20 + curve
    subf  r4, r6, r7                      # left = centre - hw
    cmpwi r4, 0
    bge   olg_l
    li    r4, 0
olg_l:
    add   r5, r7, r6                      # right = centre + hw
    cmpwi r5, 39
    ble   olg_r
    li    r5, 39
olg_r:
    blr

outlast_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, ol_over
    cmpwi r3, 0
    beq   olu_run
    LWZA  r3, v_pade                      # crashed: A = new game
    andi. r0, r3, PAD_A
    beq   olu_ret
    bl    outlast_init
    li    r3, 1
    STWA  r3, v_dirty, r4
    b     olu_ret
olu_run:
    LWZA  r3, v_pade                      # tap steering (edge)
    andi. r0, r3, PAD_L
    beq   olu_s1
    bl    ol_steer_l
olu_s1:
    LWZA  r3, v_pade
    andi. r0, r3, PAD_R
    beq   olu_s2
    bl    ol_steer_r
olu_s2:
    LWZA  r3, ol_ctr
    subi  r3, r3, 1
    STWA  r3, ol_ctr, r4
    cmpwi r3, 0
    bne   olu_ret
    li    r3, OLRATE
    STWA  r3, ol_ctr, r4
    LWZA  r3, v_pad                       # held steering, once per scroll step
    andi. r0, r3, PAD_L
    beq   olu_h1
    bl    ol_steer_l
olu_h1:
    LWZA  r3, v_pad
    andi. r0, r3, PAD_R
    beq   olu_h2
    bl    ol_steer_r
olu_h2:
    LWZA  r3, ol_scroll
    addi  r3, r3, 1
    STWA  r3, ol_scroll, r4
    LWZA  r3, ol_dist
    addi  r3, r3, 1
    STWA  r3, ol_dist, r4
    li    r3, 1
    STWA  r3, ol_pf, r4
    li    r3, (OLBANDS-1)                 # crash check on the bottom band
    bl    ol_geom
    LWZA  r3, ol_carx
    cmpw  r3, r4
    blt   olu_crash
    cmpw  r3, r5
    bgt   olu_crash
    b     olu_ret
olu_crash:
    li    r3, 1
    STWA  r3, ol_over, r4
    li    r3, 1
    STWA  r3, v_dirty, r4
    li    r3, 0
    STWA  r3, ol_pf, r4
olu_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

ol_steer_l:
    LWZA  r3, ol_carx
    cmpwi r3, 0
    beq   osl_d
    subi  r3, r3, 1
    STWA  r3, ol_carx, r4
    li    r3, 1
    STWA  r3, ol_pf, r4
osl_d:
    blr
ol_steer_r:
    LWZA  r3, ol_carx
    cmpwi r3, 39
    bge   osr_d
    addi  r3, r3, 1
    STWA  r3, ol_carx, r4
    li    r3, 1
    STWA  r3, ol_pf, r4
osr_d:
    blr

# ============================================================================
# rendering
# ============================================================================
# ol_draw_road: all 20 bands (grass fill, road span, parity stripes) + the car.
ol_draw_road:
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    li    r14, 0                          # band
odr_b:
    mr    r3, r14
    bl    ol_geom
    mr    r15, r4                         # left
    mr    r16, r5                         # right
    mulli r17, r14, 16
    addi  r17, r17, OLY0                  # band pixel y
    li    r3, 5                           # grass (green)
    bl    set_fg
    li    r3, 0
    mr    r4, r17
    li    r5, SCRW
    li    r6, 16
    bl    frect
    li    r3, 9                           # road (grey)
    bl    set_fg
    slwi  r3, r15, 4
    mr    r4, r17
    subf  r5, r15, r16
    addi  r5, r5, 1
    slwi  r5, r5, 4
    li    r6, 16
    bl    frect
    LWZA  r3, ol_scroll                   # stripe the edges by parity
    add   r3, r3, r14
    andi. r0, r3, 1
    beq   odr_ns
    li    r3, 1                           # stripe (white)
    bl    set_fg
    slwi  r3, r15, 4
    mr    r4, r17
    li    r5, 16
    li    r6, 16
    bl    frect
    li    r3, 1
    bl    set_fg
    slwi  r3, r16, 4
    mr    r4, r17
    li    r5, 16
    li    r6, 16
    bl    frect
odr_ns:
    addi  r14, r14, 1
    cmpwi r14, OLBANDS
    bne   odr_b
    li    r3, 7                           # the car (red) on the bottom band
    bl    set_fg
    LWZA  r3, ol_carx
    slwi  r3, r3, 4
    li    r4, (OLY0 + (OLBANDS-1)*16)
    li    r5, 16
    li    r6, 16
    bl    frect
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

ol_draw_dist:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, ol_dist
    LA    r4, dec_buf
    bl    fmt_dec4
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 64
    li    r4, 24
    LA    r5, dec_buf
    bl    pstr
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

outlast_partial:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    bl    ol_draw_road
    bl    ol_draw_dist
    li    r3, 0
    STWA  r3, ol_pf, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

app_outlast_draw:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 16
    li    r4, 24
    LA    r5, s_ol_dist
    bl    pstr
    li    r3, 200
    li    r4, 464
    LA    r5, s_ol_help
    bl    pstr
    bl    ol_draw_road
    bl    ol_draw_dist
    LWZA  r3, ol_over
    cmpwi r3, 0
    beq   aol_d
    li    r3, 0
    li    r4, 1
    bl    setfb
    li    r3, 264
    li    r4, 40
    LA    r5, s_ol_crash
    bl    pstr
aol_d:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

.section .rodata
s_ol_dist:  .asciz "Dist:"
s_ol_help:  .asciz "L/R steer   A=new"
s_ol_crash: .asciz "CRASH! A=new"
.align 2
.section .text
