@ ============================================================================
@ UnoDOS / GBA — Tracker. Pattern-grid editor; audio is the GBA square channel 1
@ (hardware tone, like the rpi PWM). tk_freqs hold precomputed SND1 rate values.
@ ============================================================================

tk_tone:                                 @ r0 = SND rate (0 = rest)
    cmp r0, #0
    beq tkt_rest
    mov r2, r0
    ldr r0, =SND1CNT_H
    ldr r1, =0xF080
    strh r1, [r0]
    ldr r0, =SND1CNT_X
    orr r2, r2, #0x8000
    strh r2, [r0]
    bx lr
tkt_rest:
    ldr r0, =SND1CNT_H
    mov r1, #0
    strh r1, [r0]
    bx lr
.ltorg

tk_play_row:                             @ r0 = row
    push {lr}
    mov r1, #NT_CH
    mul r2, r0, r1
    ldr r1, =tk_pat
    add r1, r1, r2
    mov r2, #0
tpr_l:
    ldrb r3, [r1, r2]
    cmp r3, #0
    bne tpr_play
    add r2, r2, #1
    cmp r2, #NT_CH
    blt tpr_l
    mov r0, #0
    bl tk_tone
    pop {pc}
tpr_play:
    ldr r0, =tk_freqs
    ldr r0, [r0, r3, lsl #2]
    bl tk_tone
    pop {pc}
.ltorg

tracker_init:
    push {lr}
    ldr r0, =SNDCNT_X
    mov r1, #0x80
    strh r1, [r0]
    ldr r0, =SNDCNT_L
    ldr r1, =0x1177
    strh r1, [r0]
    ldr r0, =SNDCNT_H
    mov r1, #2
    strh r1, [r0]
    ldr r0, =tk_pat
    mov r1, #(NT_ROWS*NT_CH)
tin_clr:
    mov r2, #0
    strb r2, [r0], #1
    subs r1, r1, #1
    bne tin_clr
    ldr r0, =tk_pat
    mov r1, #0
tin_seed:
    cmp r1, #8
    bge tin_done
    add r2, r1, #1
    mov r3, #NT_CH
    mul r12, r1, r3
    strb r2, [r0, r12]
    add r1, r1, #1
    b tin_seed
tin_done:
    mov r1, #0
    ldr r0, =tk_crow
    str r1, [r0]
    ldr r0, =tk_cch
    str r1, [r0]
    ldr r0, =tk_prow
    str r1, [r0]
    mov r1, #TK_STEPF
    ldr r0, =tk_ptmr
    str r1, [r0]
    pop {pc}
.ltorg

tracker_update:
    push {r4, lr}
    ldr r0, =v_pade
    ldr r4, [r0]
    tst r4, #PAD_U
    beq tu_d
    ldr r0, =tk_crow
    ldr r1, [r0]
    cmp r1, #0
    subgt r1, r1, #1
    strgt r1, [r0]
tu_d:
    tst r4, #PAD_D
    beq tu_l
    ldr r0, =tk_crow
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, #NT_ROWS
    strlt r1, [r0]
tu_l:
    tst r4, #PAD_L
    beq tu_r
    ldr r0, =tk_cch
    ldr r1, [r0]
    cmp r1, #0
    subgt r1, r1, #1
    strgt r1, [r0]
tu_r:
    tst r4, #PAD_R
    beq tu_a
    ldr r0, =tk_cch
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, #NT_CH
    strlt r1, [r0]
tu_a:
    tst r4, #PAD_A
    beq tu_pace
    ldr r0, =tk_crow
    ldr r0, [r0]
    mov r1, #NT_CH
    mul r2, r0, r1
    ldr r0, =tk_cch
    ldr r0, [r0]
    add r2, r2, r0
    ldr r0, =tk_pat
    ldrb r1, [r0, r2]
    add r1, r1, #1
    cmp r1, #9
    movge r1, #0
    strb r1, [r0, r2]
tu_pace:
    tst r4, #0xF1
    beq tu_play
    mov r0, #1
    ldr r1, =v_dirty
    str r0, [r1]
tu_play:
    ldr r0, =a_gpause
    ldr r0, [r0]
    cmp r0, #0
    bne tu_done
    ldr r1, =tk_ptmr
    ldr r0, [r1]
    sub r0, r0, #1
    str r0, [r1]
    cmp r0, #0
    bne tu_done
    mov r0, #TK_STEPF
    str r0, [r1]
    ldr r1, =tk_prow
    ldr r0, [r1]
    add r0, r0, #1
    cmp r0, #NT_ROWS
    movge r0, #0
    str r0, [r1]
    bl tk_play_row
    mov r0, #1
    ldr r1, =v_dirty
    str r0, [r1]
tu_done:
    pop {r4, pc}
.ltorg

tk_draw_grid:
    push {r4-r6, lr}
    mov r4, #0
tdg_row:
    cmp r4, #NT_ROWS
    bge tdg_done
    ldr r0, =tk_prow
    ldr r0, [r0]
    cmp r0, r4
    bne tdg_nomark
    mov r0, #6
    mov r1, #0
    bl setfb
    mov r0, #10
    mov r1, #TK_RH
    mul r1, r4, r1
    add r1, r1, #TKO_Y
    ldr r2, =s_tkmark
    bl pstr
tdg_nomark:
    mov r5, #0
tdg_ch:
    cmp r5, #NT_CH
    bge tdg_rowend
    mov r1, #NT_CH
    mul r0, r4, r1
    add r0, r0, r5
    ldr r1, =tk_pat
    ldrb r6, [r1, r0]
    ldr r0, =tk_crow
    ldr r0, [r0]
    cmp r0, r4
    bne tdg_norm
    ldr r0, =tk_cch
    ldr r0, [r0]
    cmp r0, r5
    bne tdg_norm
    mov r0, #0
    mov r1, #1
    bl setfb
    b tdg_cell
tdg_norm:
    mov r0, #1
    mov r1, #0
    bl setfb
tdg_cell:
    mov r0, #3
    mul r2, r6, r0
    ldr r0, =tk_names
    add r2, r0, r2
    mov r0, #TK_CW
    mul r0, r5, r0
    add r0, r0, #TKO_X
    mov r1, #TK_RH
    mul r1, r4, r1
    add r1, r1, #TKO_Y
    bl pstr
    add r5, r5, #1
    b tdg_ch
tdg_rowend:
    add r4, r4, #1
    b tdg_row
tdg_done:
    pop {r4-r6, pc}
.ltorg

app_tracker:
    ldr r0, =t_tracker
    bl draw_chrome
    bl tk_draw_grid
    pop {pc}
.ltorg

.align 2
tk_freqs: .word 0, 1548, 1602, 1651, 1672, 1714, 1750, 1783, 1797
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
.align 2
