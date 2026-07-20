// ============================================================================
// UnoDOS / PinePhone (AArch64) — OutLast, the pseudo-3D band road racer
// (c64/outlast.s). The portrait panel suits the long road: 24 perspective
// bands (18 px tall) run down the 640-px screen, each drawn as colour spans
// (grass / road / striped edges) into the framebuffer — the C64's colour-cell
// trick at 32 bpp. The road snakes (a scrolling triangle-wave centre) and
// widens toward the bottom; drift off the road and it's a CRASH.
// D-pad left/right steer, A restarts after a crash, B = back.
// ============================================================================

.equ OL_BANDS, 24
.equ OL_TOP,   32
.equ OL_BANDH, 18
.equ OL_COLW,  12                // 40 columns x 12 px = 480
.equ OL_RATE,  4                 // frames per scroll step

outlast_init:
    ldr   x0, =ol_carx
    mov   w1, #20
    str   w1, [x0]
    ldr   x0, =ol_scroll
    str   wzr, [x0]
    ldr   x0, =ol_dist
    str   wzr, [x0]
    ldr   x0, =ol_over
    str   wzr, [x0]
    mov   w1, #OL_RATE
    ldr   x0, =ol_ctr
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ret

// ol_geom: w0 = band -> ol_hw / ol_left / ol_right. Leaf.
// hw = 3 + band/2; centre = 20 + triangle(scroll+band); clamp to 0..39.
ol_geom:
    lsr   w1, w0, #1
    add   w1, w1, #3
    ldr   x2, =ol_hw
    str   w1, [x2]
    ldr   x2, =ol_scroll
    ldr   w2, [x2]
    add   w2, w2, w0
    and   w2, w2, #15
    cmp   w2, #8
    b.lo  og_p
    sub   w2, w2, #16
og_p:
    add   w2, w2, #20                     // centre
    sub   w3, w2, w1                      // left = centre - hw
    cmp   w3, #0
    csel  w3, wzr, w3, lt
    ldr   x4, =ol_left
    str   w3, [x4]
    add   w3, w2, w1                      // right = centre + hw
    mov   w5, #39
    cmp   w3, #40
    csel  w3, w5, w3, ge
    ldr   x4, =ol_right
    str   w3, [x4]
    ret

// ol_draw_band: w0 = band — grass-left / road / grass-right / edge stripes
ol_draw_band:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    bl    ol_geom
    mov   w20, #OL_BANDH                  // y = OL_TOP + band*18
    mul   w20, w19, w20
    add   w20, w20, #OL_TOP
    ldr   x0, =ol_left
    ldr   w21, [x0]
    ldr   x0, =ol_right
    ldr   w22, [x0]
    cbz   w21, odb_road                   // grass left of the road
    mov   w0, #5
    bl    set_fg
    mov   w0, #0
    mov   w1, w20
    mov   w2, #OL_COLW
    mul   w2, w21, w2
    mov   w3, #OL_BANDH
    bl    frect
odb_road:
    mov   w0, #9                          // the road span
    bl    set_fg
    mov   w0, #OL_COLW
    mul   w0, w21, w0
    mov   w1, w20
    sub   w2, w22, w21
    add   w2, w2, #1
    mov   w3, #OL_COLW
    mul   w2, w2, w3
    mov   w3, #OL_BANDH
    bl    frect
    mov   w2, #39                         // grass right of the road
    subs  w2, w2, w22
    b.eq  odb_str
    mov   w0, #5
    bl    set_fg
    mov   w1, #OL_COLW
    mov   w2, #39
    sub   w2, w2, w22
    mul   w2, w2, w1
    add   w0, w22, #1
    mul   w0, w0, w1
    mov   w1, w20
    mov   w3, #OL_BANDH
    bl    frect
odb_str:
    ldr   x0, =ol_scroll                  // stripe the edges on odd parity
    ldr   w0, [x0]
    add   w0, w0, w19
    tst   w0, #1
    b.eq  odb_done
    mov   w0, #1
    bl    set_fg
    mov   w0, #OL_COLW
    mul   w0, w21, w0
    mov   w1, w20
    mov   w2, #OL_COLW
    mov   w3, #OL_BANDH
    bl    frect
    mov   w0, #OL_COLW
    mul   w0, w22, w0
    mov   w1, w20
    mov   w2, #OL_COLW
    mov   w3, #OL_BANDH
    bl    frect
odb_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

ol_draw_car:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #7
    bl    set_fg
    ldr   x0, =ol_carx
    ldr   w0, [x0]
    mov   w1, #OL_COLW
    mul   w0, w0, w1
    mov   w1, #(OL_TOP+(OL_BANDS-1)*OL_BANDH)
    mov   w2, #OL_COLW
    mov   w3, #OL_BANDH
    bl    frect
    ldp   x29, x30, [sp], #16
    ret

ol_draw_road:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
odr_b:
    mov   w0, w19
    bl    ol_draw_band
    add   w19, w19, #1
    cmp   w19, #OL_BANDS
    b.ne  odr_b
    bl    ol_draw_car
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// ol_draw_bottom: partial repaint after a steer between scroll steps
ol_draw_bottom:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #(OL_BANDS-1)
    bl    ol_draw_band
    bl    ol_draw_car
    ldp   x29, x30, [sp], #16
    ret

ol_draw_hud:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #0
    bl    set_fg
    mov   w0, #16
    mov   w1, #20
    mov   w2, #448
    mov   w3, #8
    bl    frect
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #20
    ldr   x2, =s_ol_dist
    bl    pstr
    ldr   x0, =ol_dist
    ldr   w0, [x0]
    bl    fmt_dec
    mov   w0, #64
    mov   w1, #20
    ldr   x2, =numstr
    bl    pstr
    ldr   x0, =ol_over
    ldr   w0, [x0]
    cbz   w0, ohd_d
    mov   w0, #0
    mov   w1, #1
    bl    setfb
    mov   w0, #240
    mov   w1, #20
    ldr   x2, =s_ol_crash
    bl    pstr
ohd_d:
    ldp   x29, x30, [sp], #16
    ret

// ol_check: crash when the car is outside the bottom band's road span
ol_check:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #(OL_BANDS-1)
    bl    ol_geom
    ldr   x0, =ol_carx
    ldr   w0, [x0]
    ldr   x1, =ol_left
    ldr   w1, [x1]
    cmp   w0, w1
    b.lt  olc_off
    ldr   x1, =ol_right
    ldr   w1, [x1]
    cmp   w0, w1
    b.le  olc_ok
olc_off:
    mov   w1, #1
    ldr   x0, =ol_over
    str   w1, [x0]
olc_ok:
    ldp   x29, x30, [sp], #16
    ret

outlast_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =ol_over
    ldr   w0, [x0]
    cbz   w0, olu_run
    ldr   x0, =v_pade                     // A restarts after a crash
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  olu_ret
    bl    outlast_init
olu_ret:
    ldp   x29, x30, [sp], #16
    ret
olu_run:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_L
    b.eq  olu_1
    ldr   x2, =ol_carx
    ldr   w1, [x2]
    cbz   w1, olu_1
    sub   w1, w1, #1
    str   w1, [x2]
    bl    ol_draw_bottom
olu_1:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_R
    b.eq  olu_2
    ldr   x2, =ol_carx
    ldr   w1, [x2]
    cmp   w1, #39
    b.hs  olu_2
    add   w1, w1, #1
    str   w1, [x2]
    bl    ol_draw_bottom
olu_2:
    ldr   x2, =ol_ctr                     // scroll cadence
    ldr   w0, [x2]
    subs  w0, w0, #1
    str   w0, [x2]
    b.ne  olu_ret
    mov   w0, #OL_RATE
    str   w0, [x2]
    ldr   x2, =ol_scroll
    ldr   w0, [x2]
    add   w0, w0, #1
    str   w0, [x2]
    ldr   x2, =ol_dist
    ldr   w0, [x2]
    add   w0, w0, #1
    str   w0, [x2]
    bl    ol_draw_road
    bl    ol_check
    bl    ol_draw_hud
    b     olu_ret

// app_outlast: full-screen draw
app_outlast:
    ldr   x2, =l_outlast
    bl    draw_chrome
    bl    ol_draw_road
    bl    ol_draw_hud
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
s_ol_dist:  .asciz "Dist:"
s_ol_crash: .asciz "CRASH! A=new"
.section .text
