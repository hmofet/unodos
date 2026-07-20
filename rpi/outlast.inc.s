// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — OutLast (app 8), the pseudo-3D road racer.
// The c64/outlast.s band model re-expressed on the framebuffer: 20 perspective
// bands, each drawn as horizontal colour runs (grass / road / striped edges),
// a road that widens toward the bottom and snakes on a scrolling triangle
// wave, a steerable car, distance as score, off-road = CRASH.
//
//   left/right  steer (one column per press)     A  restart after a crash
//   B           back to the launcher
// Geometry is the donor's, scaled x16: 40 columns -> 640px, half-width 3+band/2,
// centre 20 + tri(scroll+band), stripes on (band+scroll) parity.
// ============================================================================

.equ OL_BANDS, 20
.equ OL_TOPY,  48                         // y of band 0
.equ OL_BH,    20                         // band height (px)
.equ OL_RATE,  6                          // frames per scroll step

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
    ldr   x0, =ol_ctr
    mov   w1, #OL_RATE
    str   w1, [x0]
    ret

// outlast_update: steering + the scroll clock (one frame)
outlast_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =ol_over
    ldr   w0, [x0]
    cbnz  w0, olu_over
    // steer on edges
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_L
    b.eq  olu_r
    ldr   x2, =ol_carx
    ldr   w0, [x2]
    cbz   w0, olu_r
    sub   w0, w0, #1
    str   w0, [x2]
    bl    olu_mark
olu_r:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_R
    b.eq  olu_clk
    ldr   x2, =ol_carx
    ldr   w0, [x2]
    cmp   w0, #39
    b.hs  olu_clk
    add   w0, w0, #1
    str   w0, [x2]
    bl    olu_mark
olu_clk:
    ldr   x2, =ol_ctr
    ldr   w0, [x2]
    sub   w0, w0, #1
    str   w0, [x2]
    cbnz  w0, olu_done
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
    // off-road check on the bottom band
    mov   w0, #(OL_BANDS-1)
    bl    ol_geom                         // w1 = left col, w2 = right col
    ldr   x0, =ol_carx
    ldr   w0, [x0]
    cmp   w0, w1
    b.lo  olu_crash
    cmp   w0, w2
    b.hi  olu_crash
    bl    olu_mark
olu_done:
    ldp   x29, x30, [sp], #16
    ret
olu_crash:
    ldr   x0, =ol_over
    mov   w1, #1
    str   w1, [x0]
    bl    olu_mark
    ldp   x29, x30, [sp], #16
    ret
olu_over:
    // A restarts
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  olu_done
    bl    outlast_init
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

olu_mark:
    ldr   x0, =pf_ol
    mov   w1, #1
    str   w1, [x0]
    ret

// ol_geom: w0 = band 0..19 -> w1 = left col, w2 = right col (0..39). Leaf.
// hw = 3 + band/2; curve = tri16(scroll + band) in -8..7; centre = 20 + curve.
ol_geom:
    lsr   w3, w0, #1
    add   w3, w3, #3                      // hw
    ldr   x4, =ol_scroll
    ldr   w4, [x4]
    add   w4, w4, w0
    and   w4, w4, #15
    cmp   w4, #8
    b.lo  olg_pos
    sub   w4, w4, #16
olg_pos:
    add   w4, w4, #20                     // centre
    sub   w1, w4, w3                      // left
    cmp   w1, #0
    csel  w1, wzr, w1, lt
    add   w2, w4, w3                      // right
    cmp   w2, #39
    mov   w5, #39
    csel  w2, w5, w2, gt
    ret

// ol_draw_scene: all 20 bands + car + HUD (everything below the title bar)
ol_draw_scene:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, #0                         // band
ods_band:
    mov   w0, w19
    bl    ol_geom
    mov   w20, w1                         // left col
    mov   w21, w2                         // right col
    mov   w1, #OL_BH
    mul   w22, w19, w1
    add   w22, w22, #OL_TOPY              // band y
    // left grass: 0 .. left*16
    mov   w0, #5
    bl    set_fg
    cbz   w20, ods_road
    mov   w0, #0
    mov   w1, w22
    lsl   w2, w20, #4
    mov   w3, #OL_BH
    bl    frect
ods_road:
    // right grass: (right+1)*16 .. 640  (fg is still 5 from the left span)
    add   w4, w21, #1
    cmp   w4, #40
    b.hs  ods_road2
    lsl   w0, w4, #4
    mov   w2, #SCRW
    sub   w2, w2, w0
    mov   w1, w22
    mov   w3, #OL_BH
    bl    frect
ods_road2:
    // road span: left*16 .. (right+1)*16, grey
    mov   w0, #9
    bl    set_fg
    lsl   w0, w20, #4
    mov   w1, w22
    sub   w2, w21, w20
    add   w2, w2, #1
    lsl   w2, w2, #4
    mov   w3, #OL_BH
    bl    frect
    // striped edges on (band+scroll) parity
    ldr   x0, =ol_scroll
    ldr   w0, [x0]
    add   w0, w0, w19
    tst   w0, #1
    b.eq  ods_next
    mov   w0, #1
    bl    set_fg
    lsl   w0, w20, #4
    mov   w1, w22
    mov   w2, #16
    mov   w3, #OL_BH
    bl    frect
    mov   w0, #1
    bl    set_fg
    lsl   w0, w21, #4
    mov   w1, w22
    mov   w2, #16
    mov   w3, #OL_BH
    bl    frect
ods_next:
    add   w19, w19, #1
    cmp   w19, #OL_BANDS
    b.ne  ods_band
    // the car (red) on the bottom band
    mov   w0, #7
    bl    set_fg
    ldr   x0, =ol_carx
    ldr   w0, [x0]
    lsl   w0, w0, #4
    add   w0, w0, #2
    mov   w1, #(OL_TOPY + (OL_BANDS-1)*OL_BH + 2)
    mov   w2, #12
    mov   w3, #16
    bl    frect
    // HUD: distance (band strip above the road)
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #28
    ldr   x2, =s_ol_dist
    bl    pstr
    ldr   x0, =ol_dist
    ldr   w0, [x0]
    ldr   x1, =str_buf
    bl    fmt_dec
    mov   w0, #72
    mov   w1, #28
    ldr   x2, =str_buf
    bl    pstr
    // pad the tail so a shrinking number doesn't leave stale digits
    mov   w0, #120
    mov   w1, #28
    ldr   x2, =s_ol_pad
    bl    pstr
    ldr   x0, =ol_over
    ldr   w0, [x0]
    cbz   w0, ods_done
    mov   w0, #0
    mov   w1, #1
    bl    setfb
    mov   w0, #248
    mov   w1, #28
    ldr   x2, =s_ol_crash
    bl    pstr
ods_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

outlast_partial:
    b     ol_draw_scene

.section .rodata
s_ol_dist:  .asciz "Dist:"
s_ol_pad:   .asciz "   "
s_ol_crash: .asciz " CRASH!  A = restart "
.align 2
.section .text
