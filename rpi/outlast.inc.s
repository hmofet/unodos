// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — OutLast. Ported from the canonical
// c64/outlast.s pseudo-3D racer: 20 perspective bands, each a grass strip + a
// road span whose half-width grows toward the bottom and whose centre snakes
// with a scrolling triangle wave; striped edges; a steerable car on the bottom
// band; drift off the road = CRASH. Distance is the score. On the Pi the bands
// render as horizontal pixel strips (40 logical cols * 16px = 640).
// ============================================================================

// ol_band_geom: w0 = band (0 top .. 19 bottom) -> w0 = left col, w1 = right col.
ol_band_geom:
    lsr   w2, w0, #1
    add   w2, w2, #3                      // hw = 3 + band/2
    ldr   x3, =ol_scroll
    ldr   w3, [x3]
    add   w3, w3, w0
    and   w3, w3, #15
    cmp   w3, #8
    b.lt  obg_pos
    sub   w3, w3, #16                     // curve -8..7
obg_pos:
    add   w3, w3, #20                     // centre = 20 + curve
    sub   w0, w3, w2                      // left
    cmp   w0, #0
    b.ge  obg_l
    mov   w0, #0
obg_l:
    add   w1, w3, w2                      // right
    cmp   w1, #40
    b.lt  obg_r
    mov   w1, #39
obg_r:
    ret

// outlast_init / ol_new_game
outlast_init:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #20
    ldr   x1, =ol_carx
    str   w0, [x1]
    ldr   x1, =ol_scroll
    str   wzr, [x1]
    ldr   x1, =ol_dist
    str   wzr, [x1]
    ldr   x1, =ol_over
    str   wzr, [x1]
    mov   w0, #OL_RATE
    ldr   x1, =ol_ctr
    str   w0, [x1]
    ldp   x29, x30, [sp], #16
    ret

// ol_check: off-road if the car isn't within the bottom band's road span.
ol_check:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #(OL_BANDS-1)
    bl    ol_band_geom
    ldr   x2, =ol_carx
    ldr   w2, [x2]
    cmp   w2, w0
    b.lt  olc_off
    cmp   w2, w1
    b.gt  olc_off
    b     olc_out
olc_off:
    ldr   x0, =ol_over
    mov   w1, #1
    str   w1, [x0]
olc_out:
    ldp   x29, x30, [sp], #16
    ret

// outlast_update: steer + scroll pacing (called each frame for app 8).
outlast_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =ol_over
    ldr   w0, [x0]
    cbnz  w0, ou_done                     // crashed: frozen
    ldr   x0, =v_pade
    ldr   w4, [x0]
    tst   w4, #PAD_L
    b.eq  ou_r
    ldr   x0, =ol_carx
    ldr   w1, [x0]
    cbz   w1, ou_r
    sub   w1, w1, #1
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
ou_r:
    tst   w4, #PAD_R
    b.eq  ou_pace
    ldr   x0, =ol_carx
    ldr   w1, [x0]
    cmp   w1, #39
    b.ge  ou_pace
    add   w1, w1, #1
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
ou_pace:
    ldr   x0, =a_gpause                   // AUTOTEST script ended -> freeze
    ldr   w0, [x0]
    cbnz  w0, ou_done
    ldr   x0, =ol_ctr
    ldr   w1, [x0]
    sub   w1, w1, #1
    str   w1, [x0]
    cbnz  w1, ou_done
    mov   w1, #OL_RATE
    ldr   x0, =ol_ctr
    str   w1, [x0]
    ldr   x0, =ol_scroll
    ldr   w1, [x0]
    add   w1, w1, #1
    str   w1, [x0]
    ldr   x0, =ol_dist
    ldr   w1, [x0]
    add   w1, w1, #1
    str   w1, [x0]
    bl    ol_check
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
ou_done:
    ldp   x29, x30, [sp], #16
    ret

// ol_draw_road: 20 bands (grass + road + stripes) then the car.
ol_draw_road:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, #0                         // band
odr_loop:
    cmp   w19, #OL_BANDS
    b.hs  odr_car
    mov   w0, w19
    bl    ol_band_geom
    mov   w20, w0                         // left
    mov   w21, w1                         // right
    mov   w0, #OL_BH                      // band y -> w22
    mul   w22, w19, w0
    add   w22, w22, #OLO_Y
    // grass strip (full width, green)
    mov   w0, #5
    bl    set_fg
    mov   w0, #0
    mov   w1, w22
    mov   w2, #640
    mov   w3, #OL_BH
    bl    frect
    // road span (grey)
    mov   w0, #9
    bl    set_fg
    lsl   w0, w20, #4
    mov   w1, w22
    sub   w2, w21, w20
    add   w2, w2, #1
    lsl   w2, w2, #4
    mov   w3, #OL_BH
    bl    frect
    // edge stripes on alternating bands (white)
    ldr   x0, =ol_scroll
    ldr   w0, [x0]
    add   w0, w0, w19
    tst   w0, #1
    b.eq  odr_nostripe
    mov   w0, #1
    bl    set_fg
    lsl   w0, w20, #4
    mov   w1, w22
    mov   w2, #OL_COLW
    mov   w3, #OL_BH
    bl    frect
    mov   w0, #1
    bl    set_fg
    lsl   w0, w21, #4
    mov   w1, w22
    mov   w2, #OL_COLW
    mov   w3, #OL_BH
    bl    frect
odr_nostripe:
    add   w19, w19, #1
    b     odr_loop
odr_car:
    mov   w0, #7                          // car = red
    bl    set_fg
    ldr   x0, =ol_carx
    ldr   w0, [x0]
    lsl   w0, w0, #4
    mov   w1, #OL_BH
    mov   w2, #(OL_BANDS-1)
    mul   w1, w2, w1
    add   w1, w1, #OLO_Y
    mov   w2, #OL_COLW
    mov   w3, #OL_BH
    bl    frect
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

ol_draw_hud:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #470
    mov   w1, #40
    ldr   x2, =s_oldist
    bl    pstr
    ldr   x0, =ol_dist
    ldr   w0, [x0]
    bl    pm_itoa5
    mov   w0, #470
    mov   w1, #60
    ldr   x2, =numstr
    bl    pstr
    ldr   x0, =ol_over
    ldr   w0, [x0]
    cbz   w0, olh_done
    mov   w0, #470
    mov   w1, #90
    ldr   x2, =s_olcrash
    bl    pstr
olh_done:
    ldp   x29, x30, [sp], #16
    ret

// app_outlast: full draw (frame already pushed by draw_app).
app_outlast:
    ldr   x2, =t_outlast
    bl    draw_chrome
    bl    ol_draw_road
    bl    ol_draw_hud
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
.align 2
s_oldist:  .asciz "DIST"
s_olcrash: .asciz "CRASH!"
.section .text
