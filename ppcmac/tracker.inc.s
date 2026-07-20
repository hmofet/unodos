# ============================================================================
# UnoDOS / PowerPC Mac — Tracker. Pattern-grid editor; audio is software PCM via
# music_gen -> the codec FIFO (one tone channel, leftmost voice), like pinephone.
# ============================================================================

tk_tone:                                 # r3 = freq (0 rest). Leaf.
    STWA  r3, m_freq, r4
    li    r3, 0
    STWA  r3, m_phase, r4
    blr

tk_play_row:                             # r3 = row -> play leftmost non-empty. tail-calls.
    mulli r3, r3, NT_CH
    LA    r4, tk_pat
    add   r4, r4, r3
    li    r5, 0
tpr_l:
    lbzx  r6, r4, r5
    cmpwi r6, 0
    bne   tpr_play
    addi  r5, r5, 1
    cmpwi r5, NT_CH
    blt   tpr_l
    li    r3, 0
    b     tk_tone
tpr_play:
    LA    r3, tk_freqs
    slwi  r6, r6, 2
    lwzx  r3, r3, r6
    b     tk_tone

tracker_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r3, tk_pat
    li    r4, (NT_ROWS*NT_CH)
    mtctr r4
tin_clr:
    li    r0, 0
    stb   r0, 0(r3)
    addi  r3, r3, 1
    bdnz  tin_clr
    LA    r3, tk_pat
    li    r5, 0
tin_seed:
    cmpwi r5, 8
    bge   tin_done
    addi  r6, r5, 1
    mulli r7, r5, NT_CH
    stbx  r6, r3, r7
    addi  r5, r5, 1
    b     tin_seed
tin_done:
    li    r3, 0
    STWA  r3, tk_crow, r4
    STWA  r3, tk_cch, r4
    STWA  r3, tk_prow, r4
    li    r3, TK_STEPF
    STWA  r3, tk_ptmr, r4
    li    r3, 0
    STWA  r3, m_freq, r4
    STWA  r3, m_phase, r4
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

tracker_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    stw   r14, 8(r1)
    bl    music_gen                       # feed the codec FIFO each frame
    LWZA  r14, v_pade
    andi. r0, r14, PAD_U
    beq   tu_d
    LWZA  r3, tk_crow
    cmpwi r3, 0
    beq   tu_d
    subi  r3, r3, 1
    STWA  r3, tk_crow, r4
tu_d:
    andi. r0, r14, PAD_D
    beq   tu_l
    LWZA  r3, tk_crow
    addi  r3, r3, 1
    cmpwi r3, NT_ROWS
    bge   tu_l
    STWA  r3, tk_crow, r4
tu_l:
    andi. r0, r14, PAD_L
    beq   tu_r
    LWZA  r3, tk_cch
    cmpwi r3, 0
    beq   tu_r
    subi  r3, r3, 1
    STWA  r3, tk_cch, r4
tu_r:
    andi. r0, r14, PAD_R
    beq   tu_a
    LWZA  r3, tk_cch
    addi  r3, r3, 1
    cmpwi r3, NT_CH
    bge   tu_a
    STWA  r3, tk_cch, r4
tu_a:
    andi. r0, r14, PAD_A
    beq   tu_pace
    LWZA  r3, tk_crow
    mulli r3, r3, NT_CH
    LWZA  r4, tk_cch
    add   r3, r3, r4
    LA    r4, tk_pat
    lbzx  r5, r4, r3
    addi  r5, r5, 1
    cmpwi r5, 9
    blt   tu_now
    li    r5, 0
tu_now:
    stbx  r5, r4, r3
tu_pace:
    andi. r0, r14, 0xF1
    beq   tu_play
    li    r3, 1
    STWA  r3, v_dirty, r4
tu_play:
    LWZA  r3, a_gpause
    cmpwi r3, 0
    bne   tu_done
    LWZA  r3, tk_ptmr
    subi  r3, r3, 1
    STWA  r3, tk_ptmr, r4
    cmpwi r3, 0
    bne   tu_done
    li    r3, TK_STEPF
    STWA  r3, tk_ptmr, r4
    LWZA  r3, tk_prow
    addi  r3, r3, 1
    cmpwi r3, NT_ROWS
    blt   tu_pw
    li    r3, 0
tu_pw:
    STWA  r3, tk_prow, r4
    LWZA  r3, tk_prow
    bl    tk_play_row
    li    r3, 1
    STWA  r3, v_dirty, r4
tu_done:
    lwz   r14, 8(r1)
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

tk_draw_grid:
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    li    r14, 0
tdg_row:
    cmpwi r14, NT_ROWS
    bge   tdg_done
    LWZA  r3, tk_prow
    cmpw  r3, r14
    bne   tdg_nomark
    li    r3, 6
    li    r4, 0
    bl    setfb
    li    r3, 40
    mulli r4, r14, TK_RH
    addi  r4, r4, TKO_Y
    LA    r5, s_tkmark
    bl    pstr
tdg_nomark:
    li    r15, 0
tdg_ch:
    cmpwi r15, NT_CH
    bge   tdg_rowend
    mulli r3, r14, NT_CH
    add   r3, r3, r15
    LA    r4, tk_pat
    lbzx  r16, r4, r3
    LWZA  r3, tk_crow
    cmpw  r3, r14
    bne   tdg_norm
    LWZA  r3, tk_cch
    cmpw  r3, r15
    bne   tdg_norm
    li    r3, 0
    li    r4, 1
    bl    setfb
    b     tdg_cell
tdg_norm:
    li    r3, 1
    li    r4, 0
    bl    setfb
tdg_cell:
    mulli r3, r16, 3
    LA    r5, tk_names
    add   r5, r5, r3
    mulli r3, r15, TK_CW
    addi  r3, r3, TKO_X
    mulli r4, r14, TK_RH
    addi  r4, r4, TKO_Y
    bl    pstr
    addi  r15, r15, 1
    b     tdg_ch
tdg_rowend:
    addi  r14, r14, 1
    b     tdg_row
tdg_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

app_tracker:
    LA    r5, t_tracker
    bl    draw_chrome
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 470
    li    r4, 44
    LA    r5, s_tkhint
    bl    pstr
    bl    tk_draw_grid
    b     da_ret

.align 2
tk_freqs: .long 0, 262, 294, 330, 349, 392, 440, 494, 523
tk_names: .ascii "--\0"
          .ascii "C4\0"
          .ascii "D4\0"
          .ascii "E4\0"
          .ascii "F4\0"
          .ascii "G4\0"
          .ascii "A4\0"
          .ascii "B4\0"
          .ascii "C5\0"
.align 2
s_tkmark: .asciz ">"
s_tkhint: .asciz "A=note"
.align 2
