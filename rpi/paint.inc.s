// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — Paint (app 10). The c64/paint.s colour
// canvas re-expressed on the framebuffer, driven in the port's minimal
// pad-only interaction style (dostris-like): a 32x16 canvas of 16px cells
// (one palette index per cell) plus an 8-colour palette row below it.
//
//   d-pad  move the brush cursor (down past the canvas reaches the palette row)
//   A      on the canvas: plot the current ink   on the palette row: pick ink
//   B      back to the launcher
// The white box is the cursor; the bar under a swatch marks the current ink.
// ============================================================================

.equ PT_W,    32
.equ PT_H,    16                          // canvas rows; row PT_H = palette row
.equ PT_CS,   16                          // cell px
.equ PT_OXP,  64                          // canvas pixel origin
.equ PT_OYP,  48
.equ PT_PY,   336                         // palette row pixel y
.equ PT_PX0,  64                          // first swatch x
.equ PT_PSW,  32                          // swatch w
.equ PT_PSH,  20                          // swatch h
.equ PT_PPITCH,40                         // swatch pitch

paint_init:
    ldr   x0, =pt_canvas
    mov   w1, #1                          // white canvas
    mov   w2, #(PT_W*PT_H)
pti_cl:
    strb  w1, [x0], #1
    subs  w2, w2, #1
    b.ne  pti_cl
    ldr   x0, =pt_cx
    mov   w1, #16
    str   w1, [x0]
    ldr   x0, =pt_ocx
    str   w1, [x0]
    ldr   x0, =pt_cy
    mov   w1, #8
    str   w1, [x0]
    ldr   x0, =pt_ocy
    str   w1, [x0]
    ldr   x0, =pt_ink
    mov   w1, #7                          // red
    str   w1, [x0]
    ret

// paint_update: cursor moves / plot / ink pick (one frame)
paint_update:
    stp   x29, x30, [sp, #-16]!
    // snapshot the cursor for the partial redraw
    ldr   x0, =pt_cx
    ldr   w0, [x0]
    ldr   x1, =pt_ocx
    str   w0, [x1]
    ldr   x0, =pt_cy
    ldr   w0, [x0]
    ldr   x1, =pt_ocy
    str   w0, [x1]
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_L
    b.eq  ptu_r
    ldr   x2, =pt_cx
    ldr   w1, [x2]
    cbz   w1, ptu_r
    sub   w1, w1, #1
    str   w1, [x2]
    bl    ptu_mark
ptu_r:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_R
    b.eq  ptu_u
    // max col: 31 on the canvas, 7 on the palette row
    ldr   x1, =pt_cy
    ldr   w1, [x1]
    cmp   w1, #PT_H
    mov   w3, #(PT_W-1)
    mov   w4, #7
    csel  w3, w4, w3, eq
    ldr   x2, =pt_cx
    ldr   w1, [x2]
    cmp   w1, w3
    b.hs  ptu_u
    add   w1, w1, #1
    str   w1, [x2]
    bl    ptu_mark
ptu_u:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_U
    b.eq  ptu_d
    ldr   x2, =pt_cy
    ldr   w1, [x2]
    cbz   w1, ptu_d
    cmp   w1, #PT_H
    b.ne  ptu_uc
    // palette -> canvas: swatch index back to a canvas column
    ldr   x3, =pt_cx
    ldr   w4, [x3]
    lsl   w4, w4, #2
    str   w4, [x3]
ptu_uc:
    sub   w1, w1, #1
    str   w1, [x2]
    bl    ptu_mark
ptu_d:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_D
    b.eq  ptu_a
    ldr   x2, =pt_cy
    ldr   w1, [x2]
    cmp   w1, #PT_H
    b.hs  ptu_a
    cmp   w1, #(PT_H-1)
    b.ne  ptu_dc
    // canvas -> palette: column to the nearest swatch
    ldr   x3, =pt_cx
    ldr   w4, [x3]
    lsr   w4, w4, #2
    cmp   w4, #7
    mov   w5, #7
    csel  w4, w5, w4, hi
    str   w4, [x3]
ptu_dc:
    add   w1, w1, #1
    str   w1, [x2]
    bl    ptu_mark
ptu_a:
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  ptu_done
    ldr   x0, =pt_cy
    ldr   w0, [x0]
    cmp   w0, #PT_H
    b.eq  ptu_pick
    // plot: canvas[cy*32+cx] = ink
    lsl   w0, w0, #5
    ldr   x1, =pt_cx
    ldr   w1, [x1]
    add   w0, w0, w1
    ldr   x1, =pt_ink
    ldr   w1, [x1]
    ldr   x2, =pt_canvas
    strb  w1, [x2, w0, uxtw]
    bl    ptu_mark
    b     ptu_done
ptu_pick:
    ldr   x0, =pt_cx
    ldr   w0, [x0]
    add   w0, w0, #1                      // swatch i -> palette index i+1
    ldr   x1, =pt_ink
    str   w0, [x1]
    bl    ptu_mark
ptu_done:
    ldp   x29, x30, [sp], #16
    ret

ptu_mark:
    ldr   x0, =pf_pt
    mov   w1, #1
    str   w1, [x0]
    ret

// pt_draw_cell: w0 = cx, w1 = cy — one canvas cell from the canvas bytes
pt_draw_cell:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    lsl   w2, w1, #5
    add   w2, w2, w0
    ldr   x3, =pt_canvas
    ldrb  w0, [x3, w2, uxtw]
    bl    set_fg
    lsl   w0, w19, #4
    add   w0, w0, #PT_OXP
    lsl   w1, w20, #4
    add   w1, w1, #PT_OYP
    mov   w2, #PT_CS
    mov   w3, #PT_CS
    bl    frect
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pt_draw_palette: the 8 swatches + the ink bar (also erases old outlines)
pt_draw_palette:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
ptp_i:
    mov   w1, #PT_PPITCH
    mul   w20, w19, w1
    add   w20, w20, #PT_PX0               // swatch x
    // border field (erases a stale cursor outline)
    mov   w0, #0
    bl    set_fg
    sub   w0, w20, #2
    mov   w1, #(PT_PY-2)
    mov   w2, #(PT_PSW+4)
    mov   w3, #(PT_PSH+4)
    bl    frect
    // the swatch (palette colour i+1)
    add   w0, w19, #1
    bl    set_fg
    mov   w0, w20
    mov   w1, #PT_PY
    mov   w2, #PT_PSW
    mov   w3, #PT_PSH
    bl    frect
    // ink bar under the current ink
    ldr   x0, =pt_ink
    ldr   w0, [x0]
    sub   w0, w0, #1
    cmp   w0, w19
    mov   w1, #1
    mov   w2, #0
    csel  w0, w1, w2, eq
    bl    set_fg
    mov   w0, w20
    mov   w1, #(PT_PY+PT_PSH+4)
    mov   w2, #PT_PSW
    mov   w3, #4
    bl    frect
    add   w19, w19, #1
    cmp   w19, #8
    b.ne  ptp_i
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pt_draw_cursor: the white box at the cursor (canvas cell or palette swatch)
pt_draw_cursor:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    ldr   x0, =pt_cy
    ldr   w20, [x0]
    ldr   x0, =pt_cx
    ldr   w19, [x0]
    cmp   w20, #PT_H
    b.eq  ptc_pal
    // canvas: white 16x16 frame, cell colour refilled inside
    mov   w0, #1
    bl    set_fg
    lsl   w0, w19, #4
    add   w0, w0, #PT_OXP
    lsl   w1, w20, #4
    add   w1, w1, #PT_OYP
    mov   w2, #PT_CS
    mov   w3, #PT_CS
    bl    frect
    lsl   w2, w20, #5
    add   w2, w2, w19
    ldr   x3, =pt_canvas
    ldrb  w0, [x3, w2, uxtw]
    bl    set_fg
    lsl   w0, w19, #4
    add   w0, w0, #(PT_OXP+2)
    lsl   w1, w20, #4
    add   w1, w1, #(PT_OYP+2)
    mov   w2, #(PT_CS-4)
    mov   w3, #(PT_CS-4)
    bl    frect
    b     ptc_done
ptc_pal:
    // palette: white outline around the swatch, swatch repainted inside
    mov   w1, #PT_PPITCH
    mul   w20, w19, w1
    add   w20, w20, #PT_PX0
    mov   w0, #1
    bl    set_fg
    sub   w0, w20, #2
    mov   w1, #(PT_PY-2)
    mov   w2, #(PT_PSW+4)
    mov   w3, #(PT_PSH+4)
    bl    frect
    add   w0, w19, #1
    bl    set_fg
    mov   w0, w20
    mov   w1, #PT_PY
    mov   w2, #PT_PSW
    mov   w3, #PT_PSH
    bl    frect
ptc_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// paint_partial: repaint the old cursor cell, the palette row, the (possibly
// just-painted) current cell, then the cursor
paint_partial:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =pt_ocy
    ldr   w1, [x0]
    cmp   w1, #PT_H
    b.hs  ppp_pal                         // old cursor was on the palette row
    ldr   x0, =pt_ocx
    ldr   w0, [x0]
    bl    pt_draw_cell
ppp_pal:
    bl    pt_draw_palette
    ldr   x0, =pt_cy
    ldr   w1, [x0]
    cmp   w1, #PT_H
    b.hs  ppp_cur
    ldr   x0, =pt_cx
    ldr   w0, [x0]
    bl    pt_draw_cell
ppp_cur:
    bl    pt_draw_cursor
    ldp   x29, x30, [sp], #16
    ret

// paint_draw: the full app screen (under draw_chrome)
paint_draw:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w20, #0                         // cy
ptd_row:
    mov   w19, #0                         // cx
ptd_col:
    mov   w0, w19
    mov   w1, w20
    bl    pt_draw_cell
    add   w19, w19, #1
    cmp   w19, #PT_W
    b.ne  ptd_col
    add   w20, w20, #1
    cmp   w20, #PT_H
    b.ne  ptd_row
    bl    pt_draw_palette
    bl    pt_draw_cursor
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #64
    mov   w1, #24
    ldr   x2, =s_pt_help
    bl    pstr
    mov   w0, #64
    mov   w1, #380
    ldr   x2, =s_pt_ink
    bl    pstr
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
s_pt_help: .asciz "A = plot   d-pad = move (down to the palette row picks ink)"
s_pt_ink:  .asciz "Ink bar marks the current colour"
.align 2
.section .text
