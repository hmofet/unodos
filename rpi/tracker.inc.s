// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — Tracker. The minimal-profile take on the
// pattern editor: a 16-row x 4-channel grid that auto-loops, playing the
// LEFTMOST non-empty voice of the current row on the PWM headphone tone path
// (the Pi has one tone channel, so this mirrors the x86 PC-speaker tracker).
//   d-pad : move the cursor (row / channel)
//   A     : cycle the note under the cursor (-- C4 D4 E4 F4 G4 A4 B4 C5)
//   B     : back. Pattern cells are note indices 0..8 in tk_pat.
// ============================================================================

// tk_tone: w0 = freq Hz (0 = silence) -> PWM mark/space. Leaf.
tk_tone:
    cbz   w0, tkt_rest
    ldr   w4, =PWM_CLK_HZ
    udiv  w4, w4, w0
    ldr   x1, =PWM_RNG1
    str   w4, [x1]
    lsr   w4, w4, #1
    ldr   x1, =PWM_DAT1
    str   w4, [x1]
    ldr   x1, =PWM_CTL
    mov   w0, #0x81                       // PWEN1 | MSEN1
    str   w0, [x1]
    ret
tkt_rest:
    ldr   x1, =PWM_CTL
    str   wzr, [x1]
    ret

// tk_play_row: w0 = row -> play leftmost non-empty cell (or silence). Leaf (tail-calls).
tk_play_row:
    mov   w1, #NT_CH
    mul   w0, w0, w1
    ldr   x1, =tk_pat
    add   x1, x1, w0, uxtw
    mov   w2, #0
tpr_l:
    ldrb  w3, [x1, w2, uxtw]
    cbnz  w3, tpr_play
    add   w2, w2, #1
    cmp   w2, #NT_CH
    b.lt  tpr_l
    mov   w0, #0
    b     tk_tone
tpr_play:
    ldr   x0, =tk_freqs
    ldr   w0, [x0, w3, uxtw #2]
    b     tk_tone

// tracker_init: clear + seed a C-major scale, reset cursor, start the PWM clock.
tracker_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =tk_pat
    mov   w2, #(NT_ROWS*NT_CH)
tin_clr:
    strb  wzr, [x0], #1
    subs  w2, w2, #1
    b.ne  tin_clr
    // seed channel 0 rows 0..7 with notes 1..8
    ldr   x0, =tk_pat
    mov   w2, #0
tin_seed:
    cmp   w2, #8
    b.hs  tin_done
    add   w3, w2, #1
    mov   w4, #NT_CH
    mul   w5, w2, w4
    strb  w3, [x0, w5, uxtw]
    add   w2, w2, #1
    b     tin_seed
tin_done:
    ldr   x0, =tk_crow
    str   wzr, [x0]
    ldr   x0, =tk_cch
    str   wzr, [x0]
    ldr   x0, =tk_prow
    str   wzr, [x0]
    mov   w0, #TK_STEPF
    ldr   x1, =tk_ptmr
    str   w0, [x1]
    // PWM clock (same as music_init)
    ldr   x0, =CM_PWMCTL
    ldr   w1, =0x5A000000
    str   w1, [x0]
    ldr   x0, =CM_PWMDIV
    ldr   w1, =0x5A002000
    str   w1, [x0]
    ldr   x0, =CM_PWMCTL
    ldr   w1, =0x5A000211
    str   w1, [x0]
    ldp   x29, x30, [sp], #16
    ret

// tracker_update: cursor edit + auto playback.
tracker_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pade
    ldr   w4, [x0]
    tst   w4, #PAD_U
    b.eq  tu_d
    ldr   x0, =tk_crow
    ldr   w1, [x0]
    cbz   w1, tu_d
    sub   w1, w1, #1
    str   w1, [x0]
tu_d:
    tst   w4, #PAD_D
    b.eq  tu_l
    ldr   x0, =tk_crow
    ldr   w1, [x0]
    add   w1, w1, #1
    cmp   w1, #NT_ROWS
    b.hs  tu_l
    str   w1, [x0]
tu_l:
    tst   w4, #PAD_L
    b.eq  tu_r
    ldr   x0, =tk_cch
    ldr   w1, [x0]
    cbz   w1, tu_r
    sub   w1, w1, #1
    str   w1, [x0]
tu_r:
    tst   w4, #PAD_R
    b.eq  tu_a
    ldr   x0, =tk_cch
    ldr   w1, [x0]
    add   w1, w1, #1
    cmp   w1, #NT_CH
    b.hs  tu_a
    str   w1, [x0]
tu_a:
    tst   w4, #PAD_A
    b.eq  tu_pace
    // cycle note at (crow,cch)
    ldr   x0, =tk_crow
    ldr   w1, [x0]
    mov   w2, #NT_CH
    mul   w1, w1, w2
    ldr   x0, =tk_cch
    ldr   w2, [x0]
    add   w1, w1, w2
    ldr   x0, =tk_pat
    ldrb  w2, [x0, w1, uxtw]
    add   w2, w2, #1
    cmp   w2, #9                          // 0..8 then wrap
    csel  w2, wzr, w2, hs
    strb  w2, [x0, w1, uxtw]
tu_pace:
    mov   w1, #0xF1                       // any of L/R/U/D/A acted -> redraw
    and   w0, w4, w1
    cbz   w0, tu_play
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
tu_play:
    ldr   x0, =a_gpause
    ldr   w0, [x0]
    cbnz  w0, tu_done                     // AUTOTEST ended -> freeze
    ldr   x0, =tk_ptmr
    ldr   w1, [x0]
    sub   w1, w1, #1
    str   w1, [x0]
    cbnz  w1, tu_done
    mov   w1, #TK_STEPF
    ldr   x0, =tk_ptmr
    str   w1, [x0]
    ldr   x0, =tk_prow
    ldr   w1, [x0]
    add   w1, w1, #1
    cmp   w1, #NT_ROWS
    csel  w1, wzr, w1, hs
    str   w1, [x0]
    mov   w0, w1
    bl    tk_play_row
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
tu_done:
    ldp   x29, x30, [sp], #16
    ret

// tk_draw_grid: rows x channels of note names; play-row marker + cursor invert.
tk_draw_grid:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, #0                         // row
tdg_row:
    cmp   w19, #NT_ROWS
    b.hs  tdg_done
    ldr   x0, =tk_prow
    ldr   w0, [x0]
    cmp   w0, w19
    b.ne  tdg_nomark
    mov   w0, #6
    mov   w1, #0
    bl    setfb
    mov   w0, #40
    mov   w1, #TK_RH
    mul   w1, w19, w1
    add   w1, w1, #TKO_Y
    ldr   x2, =s_tkmark
    bl    pstr
tdg_nomark:
    mov   w20, #0                         // channel
tdg_ch:
    cmp   w20, #NT_CH
    b.hs  tdg_rowend
    mov   w0, #NT_CH
    mul   w0, w19, w0
    add   w0, w0, w20
    ldr   x1, =tk_pat
    ldrb  w21, [x1, w0, uxtw]
    ldr   x0, =tk_crow
    ldr   w0, [x0]
    cmp   w0, w19
    b.ne  tdg_norm
    ldr   x0, =tk_cch
    ldr   w0, [x0]
    cmp   w0, w20
    b.ne  tdg_norm
    mov   w0, #0
    mov   w1, #1
    bl    setfb                           // cursor cell inverted
    b     tdg_cell
tdg_norm:
    mov   w0, #1
    mov   w1, #0
    bl    setfb
tdg_cell:
    mov   w0, #3
    mul   w0, w21, w0
    ldr   x2, =tk_names
    add   x2, x2, w0, uxtw
    mov   w0, #TK_CW
    mul   w0, w20, w0
    add   w0, w0, #TKO_X
    mov   w1, #TK_RH
    mul   w1, w19, w1
    add   w1, w1, #TKO_Y
    bl    pstr
    add   w20, w20, #1
    b     tdg_ch
tdg_rowend:
    add   w19, w19, #1
    b     tdg_row
tdg_done:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// app_tracker: full draw (frame already pushed by draw_app).
app_tracker:
    ldr   x2, =t_tracker
    bl    draw_chrome
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #470
    mov   w1, #44
    ldr   x2, =s_tkhint
    bl    pstr
    bl    tk_draw_grid
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
.align 2
tk_freqs:  .word 0, 262, 294, 330, 349, 392, 440, 494, 523
tk_names:  .ascii "--\0"
           .ascii "C4\0"
           .ascii "D4\0"
           .ascii "E4\0"
           .ascii "F4\0"
           .ascii "G4\0"
           .ascii "A4\0"
           .ascii "B4\0"
           .ascii "C5\0"
.align 2
s_tkmark:  .asciz ">"
s_tkhint:  .asciz "A=note"
.section .text
