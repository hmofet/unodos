// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — Paint. The minimal-profile (directional-nav)
// realisation of the desktop Paint app: a cursor-driven cell canvas plus a
// palette swatch row one 'up' press from the top canvas row.
//   d-pad : move the cursor (canvas <-> palette row)
//   A     : on canvas = paint the current colour; on palette = pick that colour
//   B     : back (handled by the kernel up_app path)
// The canvas is PCW*PCH palette indices in RAM (pcanvas); a state change sets
// v_dirty and full_redraw repaints from the buffer (changes are human-paced).
// ============================================================================

// paint_init: clear the canvas, reset cursor + colour, force a redraw.
paint_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =pcanvas
    mov   w2, #(PCW*PCH)
pin_clr:
    strb  wzr, [x0], #1
    subs  w2, w2, #1
    b.ne  pin_clr
    ldr   x0, =p_cx
    str   wzr, [x0]
    ldr   x0, =p_cy
    str   wzr, [x0]
    ldr   x0, =p_col
    mov   w1, #1                          // white
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// paint_put: write p_col into pcanvas[p_cy*PCW + p_cx] (no-op if on palette row). Leaf.
paint_put:
    ldr   x0, =p_cy
    ldr   w1, [x0]
    cmp   w1, #PCH
    b.hs  pp_ret
    mov   w2, #PCW
    mul   w1, w1, w2
    ldr   x0, =p_cx
    ldr   w2, [x0]
    add   w1, w1, w2
    ldr   x0, =p_col
    ldr   w2, [x0]
    ldr   x0, =pcanvas
    strb  w2, [x0, w1, uxtw]
pp_ret:
    ret

// paint_update: one frame of cursor/paint input (called from up_disp for app 10).
paint_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pade
    ldr   w4, [x0]                        // edges (no calls clobber w4 until pu_fin)
    ldr   x0, =p_cy
    ldr   w5, [x0]
    cmp   w5, #PCH
    b.eq  pu_pal

    // ---- canvas mode ----
    tst   w4, #PAD_L
    b.eq  pu_cr
    ldr   x0, =p_cx
    ldr   w1, [x0]
    cbz   w1, pu_cr
    sub   w1, w1, #1
    str   w1, [x0]
pu_cr:
    tst   w4, #PAD_R
    b.eq  pu_cu
    ldr   x0, =p_cx
    ldr   w1, [x0]
    add   w1, w1, #1
    cmp   w1, #PCW
    b.hs  pu_cu
    str   w1, [x0]
pu_cu:
    tst   w4, #PAD_U
    b.eq  pu_cd
    ldr   x0, =p_cy
    ldr   w1, [x0]
    cbz   w1, pu_topal
    sub   w1, w1, #1
    str   w1, [x0]
    b     pu_cd
pu_topal:
    ldr   x0, =p_cy
    mov   w1, #PCH                        // top row -> palette row
    str   w1, [x0]
pu_cd:
    tst   w4, #PAD_D
    b.eq  pu_ca
    ldr   x0, =p_cy
    ldr   w1, [x0]
    add   w2, w1, #1
    cmp   w2, #PCH
    b.lo  pu_dok
    mov   w2, #PCH                        // bottom row -> palette row
pu_dok:
    str   w2, [x0]
pu_ca:
    tst   w4, #PAD_A
    b.eq  pu_fin
    bl    paint_put
    b     pu_fin

    // ---- palette mode ----
pu_pal:
    tst   w4, #PAD_L
    b.eq  pu_pr
    ldr   x0, =p_cx
    ldr   w1, [x0]
    cbz   w1, pu_pr
    sub   w1, w1, #1
    str   w1, [x0]
pu_pr:
    tst   w4, #PAD_R
    b.eq  pu_pu
    ldr   x0, =p_cx
    ldr   w1, [x0]
    add   w1, w1, #1
    cmp   w1, #NSWATCH
    b.hs  pu_pu
    str   w1, [x0]
pu_pu:
    tst   w4, #PAD_U
    b.eq  pu_pd
    ldr   x0, =p_cy
    mov   w1, #(PCH-1)                    // palette -> canvas bottom row
    str   w1, [x0]
    bl    paint_clampx
pu_pd:
    tst   w4, #PAD_D
    b.eq  pu_pa
    ldr   x0, =p_cy
    str   wzr, [x0]                       // palette -> canvas top row
    bl    paint_clampx
pu_pa:
    tst   w4, #PAD_A
    b.eq  pu_fin
    ldr   x0, =p_cx                       // pick colour = sw_cols[p_cx]
    ldr   w1, [x0]
    ldr   x0, =sw_cols
    ldrb  w1, [x0, w1, uxtw]
    ldr   x0, =p_col
    str   w1, [x0]

pu_fin:
    mov   w1, #0xF1                       // PAD_L|R|U|D|A (not a logical-imm; build it)
    and   w0, w4, w1
    cbz   w0, pu_ret
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
pu_ret:
    ldp   x29, x30, [sp], #16
    ret

// paint_clampx: clamp p_cx into the canvas column range when leaving the palette. Leaf.
paint_clampx:
    ldr   x0, =p_cx
    ldr   w1, [x0]
    cmp   w1, #PCW
    b.lo  pcx_ok
    mov   w1, #(PCW-1)
    str   w1, [x0]
pcx_ok:
    ret

// ---- drawing ---------------------------------------------------------------
// app_paint: full draw (called from draw_app; frame already pushed there).
app_paint:
    ldr   x2, =t_paint
    bl    draw_chrome
    bl    paint_draw_canvas
    bl    paint_draw_palette
    bl    paint_draw_curcol
    bl    paint_draw_cursor
    ldp   x29, x30, [sp], #16
    ret

// rect_outline: w0=x w1=y w2=w w3=h w4=colour idx -> 2px frame.
rect_outline:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    mov   w21, w2
    mov   w22, w3
    mov   w0, w4
    bl    set_fg
    mov   w0, w19                         // top
    mov   w1, w20
    mov   w2, w21
    mov   w3, #2
    bl    frect
    mov   w0, w19                         // bottom
    add   w1, w20, w22
    sub   w1, w1, #2
    mov   w2, w21
    mov   w3, #2
    bl    frect
    mov   w0, w19                         // left
    mov   w1, w20
    mov   w2, #2
    mov   w3, w22
    bl    frect
    add   w0, w19, w21                    // right
    sub   w0, w0, #2
    mov   w1, w20
    mov   w2, #2
    mov   w3, w22
    bl    frect
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

paint_draw_canvas:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w0, #(PCO_X-2)                  // grey frame around the canvas
    mov   w1, #(PCO_Y-2)
    mov   w2, #(PCW*PCELL+4)
    mov   w3, #(PCH*PCELL+4)
    mov   w4, #9
    bl    rect_outline
    mov   w19, #0                         // cy
pdc_row:
    cmp   w19, #PCH
    b.hs  pdc_done
    mov   w20, #0                         // cx
pdc_col:
    cmp   w20, #PCW
    b.hs  pdc_rowend
    mov   w0, #PCW
    mul   w0, w19, w0
    add   w0, w0, w20
    ldr   x1, =pcanvas
    ldrb  w21, [x1, w0, uxtw]
    cbz   w21, pdc_next                   // blank cell = desktop, skip
    mov   w0, w21
    bl    set_fg
    mov   w0, #PCELL
    mul   w0, w20, w0
    add   w0, w0, #PCO_X
    mov   w1, #PCELL
    mul   w1, w19, w1
    add   w1, w1, #PCO_Y
    mov   w2, #PCELL
    mov   w3, #PCELL
    bl    frect
pdc_next:
    add   w20, w20, #1
    b     pdc_col
pdc_rowend:
    add   w19, w19, #1
    b     pdc_row
pdc_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

paint_draw_palette:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w19, #0
pdp_l:
    cmp   w19, #NSWATCH
    b.hs  pdp_done
    ldr   x0, =sw_cols
    ldrb  w20, [x0, w19, uxtw]
    mov   w0, w20
    bl    set_fg
    mov   w0, #PSW_W
    mul   w0, w19, w0
    add   w0, w0, #PCO_X
    mov   w1, #PSW_Y
    mov   w2, #(PSW_W-2)
    mov   w3, #18
    bl    frect
    add   w19, w19, #1
    b     pdp_l
pdp_done:
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

paint_draw_cursor:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =p_cy
    ldr   w1, [x0]
    cmp   w1, #PCH
    b.eq  pcur_pal
    ldr   x0, =p_cx                       // canvas cell outline
    ldr   w2, [x0]
    mov   w0, #PCELL
    mul   w0, w2, w0
    add   w0, w0, #PCO_X
    mov   w3, #PCELL
    mul   w3, w1, w3
    add   w3, w3, #PCO_Y
    mov   w1, w3
    mov   w2, #PCELL
    mov   w3, #PCELL
    mov   w4, #6                          // yellow cursor
    bl    rect_outline
    b     pcur_done
pcur_pal:
    ldr   x0, =p_cx                       // swatch outline
    ldr   w2, [x0]
    mov   w0, #PSW_W
    mul   w0, w2, w0
    add   w0, w0, #PCO_X
    mov   w1, #PSW_Y
    mov   w2, #(PSW_W-2)
    mov   w3, #18
    mov   w4, #6
    bl    rect_outline
pcur_done:
    ldp   x29, x30, [sp], #16
    ret

paint_draw_curcol:
    stp   x29, x30, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #480
    mov   w1, #48
    ldr   x2, =s_pcolor
    bl    pstr
    ldr   x0, =p_col                      // current-colour swatch
    ldr   w0, [x0]
    bl    set_fg
    mov   w0, #480
    mov   w1, #70
    mov   w2, #48
    mov   w3, #32
    bl    frect
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #480
    mov   w1, #120
    ldr   x2, =s_phint1
    bl    pstr
    mov   w0, #480
    mov   w1, #140
    ldr   x2, =s_phint2
    bl    pstr
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
.align 2
sw_cols:   .byte 1, 2, 3, 5, 6, 7, 8, 9     // white cyan magenta green yellow red orange grey
.align 2
s_pcolor:  .asciz "Colour:"
s_phint1:  .asciz "A = paint"
s_phint2:  .asciz "U/D = palette"
.section .text
