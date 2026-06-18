// ============================================================================
// UnoDOS / PinePhone (AArch64) — Tracker. Same pattern-grid editor as the rpi
// port, but the A64 has no PWM tone — audio is software square-wave PCM synthesis
// fed to the I2S0 TX FIFO each frame (music_gen). So tk_tone just selects the
// current note frequency (m_freq) and tracker_update emits a PCM frame every tick.
//   d-pad : move cursor (row / channel)   A : cycle note   B : back
// ============================================================================

// tk_tone: w0 = freq Hz (0 = rest). Select the note for the I2S synthesiser. Leaf.
tk_tone:
    ldr   x1, =m_freq
    str   w0, [x1]
    ldr   x1, =m_phase
    str   wzr, [x1]
    ret

// tk_play_row: w0 = row -> play leftmost non-empty cell (or rest). Leaf (tail-calls).
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

// tracker_init: clear + seed a C-major scale, reset cursor + the synth phase.
tracker_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =tk_pat
    mov   w2, #(NT_ROWS*NT_CH)
tin_clr:
    strb  wzr, [x0], #1
    subs  w2, w2, #1
    b.ne  tin_clr
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
    ldr   x0, =m_freq
    str   wzr, [x0]
    ldr   x0, =m_phase
    str   wzr, [x0]
    ldp   x29, x30, [sp], #16
    ret

// tracker_update: cursor edit + auto playback (emits a PCM frame every tick).
tracker_update:
    stp   x29, x30, [sp, #-16]!
    bl    music_gen                       // feed I2S with the current note each frame
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
    cmp   w2, #9
    csel  w2, wzr, w2, hs
    strb  w2, [x0, w1, uxtw]
tu_pace:
    mov   w1, #0xF1
    and   w0, w4, w1
    cbz   w0, tu_play
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
tu_play:
    ldr   x0, =a_gpause
    ldr   w0, [x0]
    cbnz  w0, tu_done
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
    mov   w19, #0
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
    mov   w20, #0
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
    bl    setfb
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
    mov   w0, #16
    mov   w1, #410
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
