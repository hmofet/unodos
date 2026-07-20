@ ============================================================================
@ UnoDOS / GBA — Paint. ARMv4T cursor canvas + palette row, 240x160 (6px cells).
@ ============================================================================

paint_init:
    push {lr}
    ldr r0, =pcanvas
    mov r1, #(PCW*PCH)
pin_clr:
    mov r2, #0
    strb r2, [r0], #1
    subs r1, r1, #1
    bne pin_clr
    mov r1, #0
    ldr r0, =p_cx
    str r1, [r0]
    ldr r0, =p_cy
    str r1, [r0]
    mov r1, #1
    ldr r0, =p_col
    str r1, [r0]
    mov r1, #1
    ldr r0, =v_dirty
    str r1, [r0]
    pop {pc}
.ltorg

paint_put:
    ldr r0, =p_cy
    ldr r0, [r0]
    cmp r0, #PCH
    bge pp_ret
    mov r1, #PCW
    mul r2, r0, r1
    ldr r0, =p_cx
    ldr r0, [r0]
    add r2, r2, r0
    ldr r0, =p_col
    ldr r0, [r0]
    ldr r1, =pcanvas
    strb r0, [r1, r2]
pp_ret:
    bx lr
.ltorg

paint_clampx:
    ldr r0, =p_cx
    ldr r1, [r0]
    cmp r1, #PCW
    movge r1, #(PCW-1)
    strge r1, [r0]
    bx lr
.ltorg

paint_update:
    push {r4, lr}
    ldr r0, =v_pade
    ldr r4, [r0]
    ldr r0, =p_cy
    ldr r0, [r0]
    cmp r0, #PCH
    beq pu_pal
    tst r4, #PAD_L
    beq pu_cr
    ldr r0, =p_cx
    ldr r1, [r0]
    cmp r1, #0
    subgt r1, r1, #1
    strgt r1, [r0]
pu_cr:
    tst r4, #PAD_R
    beq pu_cu
    ldr r0, =p_cx
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, #PCW
    strlt r1, [r0]
pu_cu:
    tst r4, #PAD_U
    beq pu_cd
    ldr r0, =p_cy
    ldr r1, [r0]
    cmp r1, #0
    beq pu_topal
    sub r1, r1, #1
    str r1, [r0]
    b pu_cd
pu_topal:
    mov r1, #PCH
    str r1, [r0]
pu_cd:
    tst r4, #PAD_D
    beq pu_ca
    ldr r0, =p_cy
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, #PCH
    movgt r1, #PCH
    str r1, [r0]
pu_ca:
    tst r4, #PAD_A
    beq pu_fin
    bl paint_put
    b pu_fin
pu_pal:
    tst r4, #PAD_L
    beq pu_pr
    ldr r0, =p_cx
    ldr r1, [r0]
    cmp r1, #0
    subgt r1, r1, #1
    strgt r1, [r0]
pu_pr:
    tst r4, #PAD_R
    beq pu_pu
    ldr r0, =p_cx
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, #NSWATCH
    strlt r1, [r0]
pu_pu:
    tst r4, #PAD_U
    beq pu_pd
    ldr r0, =p_cy
    mov r1, #(PCH-1)
    str r1, [r0]
    bl paint_clampx
pu_pd:
    tst r4, #PAD_D
    beq pu_pa
    ldr r0, =p_cy
    mov r1, #0
    str r1, [r0]
    bl paint_clampx
pu_pa:
    tst r4, #PAD_A
    beq pu_fin
    ldr r0, =p_cx
    ldr r0, [r0]
    ldr r1, =sw_cols
    ldrb r0, [r1, r0]
    ldr r1, =p_col
    str r0, [r1]
pu_fin:
    tst r4, #0xF1
    beq pu_ret
    mov r0, #1
    ldr r1, =v_dirty
    str r0, [r1]
pu_ret:
    pop {r4, pc}
.ltorg

rect_outline:                            @ r0=x r1=y r2=w r3=h ; uses current fg
    push {r4-r7, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
    mov r7, r3
    mov r0, r4
    mov r1, r5
    mov r2, r6
    mov r3, #2
    bl frect
    mov r0, r4
    add r1, r5, r7
    sub r1, r1, #2
    mov r2, r6
    mov r3, #2
    bl frect
    mov r0, r4
    mov r1, r5
    mov r2, #2
    mov r3, r7
    bl frect
    add r0, r4, r6
    sub r0, r0, #2
    mov r1, r5
    mov r2, #2
    mov r3, r7
    bl frect
    pop {r4-r7, pc}
.ltorg

paint_draw_canvas:
    push {r4-r6, lr}
    mov r0, #9
    bl set_fg
    mov r0, #(PCO_X-2)
    mov r1, #(PCO_Y-2)
    mov r2, #(PCW*PCELL+4)
    mov r3, #(PCH*PCELL+4)
    bl rect_outline
    mov r4, #0
pdc_row:
    cmp r4, #PCH
    bge pdc_done
    mov r5, #0
pdc_col:
    cmp r5, #PCW
    bge pdc_rowend
    mov r1, #PCW
    mul r0, r4, r1
    add r0, r0, r5
    ldr r1, =pcanvas
    ldrb r6, [r1, r0]
    cmp r6, #0
    beq pdc_next
    mov r0, r6
    bl set_fg
    mov r1, #PCELL
    mul r0, r5, r1
    add r0, r0, #PCO_X
    mov r2, #PCELL
    mul r1, r4, r2
    add r1, r1, #PCO_Y
    mov r2, #PCELL
    mov r3, #PCELL
    bl frect
pdc_next:
    add r5, r5, #1
    b pdc_col
pdc_rowend:
    add r4, r4, #1
    b pdc_row
pdc_done:
    pop {r4-r6, pc}
.ltorg

paint_draw_palette:
    push {r4, r5, lr}
    mov r4, #0
pdp_l:
    cmp r4, #NSWATCH
    bge pdp_done
    ldr r0, =sw_cols
    ldrb r5, [r0, r4]
    mov r0, r5
    bl set_fg
    mov r1, #PSW_W
    mul r0, r4, r1
    add r0, r0, #PCO_X
    mov r1, #PSW_Y
    mov r2, #(PSW_W-2)
    mov r3, #14
    bl frect
    add r4, r4, #1
    b pdp_l
pdp_done:
    pop {r4, r5, pc}
.ltorg

paint_draw_cursor:
    push {lr}
    ldr r0, =p_cy
    ldr r0, [r0]
    cmp r0, #PCH
    beq pcur_pal
    mov r0, #6
    bl set_fg
    ldr r0, =p_cx
    ldr r0, [r0]
    mov r1, #PCELL
    mul r2, r0, r1
    add r2, r2, #PCO_X
    ldr r0, =p_cy
    ldr r0, [r0]
    mov r1, #PCELL
    mul r3, r0, r1
    add r3, r3, #PCO_Y
    mov r0, r2
    mov r1, r3
    mov r2, #PCELL
    mov r3, #PCELL
    bl rect_outline
    pop {pc}
pcur_pal:
    mov r0, #6
    bl set_fg
    ldr r0, =p_cx
    ldr r0, [r0]
    mov r1, #PSW_W
    mul r2, r0, r1
    add r2, r2, #PCO_X
    mov r0, r2
    mov r1, #PSW_Y
    mov r2, #(PSW_W-2)
    mov r3, #14
    bl rect_outline
    pop {pc}
.ltorg

paint_draw_curcol:
    push {lr}
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #150
    mov r1, #20
    ldr r2, =s_pcolor
    bl pstr
    ldr r0, =p_col
    ldr r0, [r0]
    bl set_fg
    mov r0, #150
    mov r1, #34
    mov r2, #40
    mov r3, #20
    bl frect
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #150
    mov r1, #62
    ldr r2, =s_phint1
    bl pstr
    mov r0, #150
    mov r1, #74
    ldr r2, =s_phint2
    bl pstr
    pop {pc}
.ltorg

app_paint:
    ldr r0, =t_paint
    bl draw_chrome
    bl paint_draw_canvas
    bl paint_draw_palette
    bl paint_draw_curcol
    bl paint_draw_cursor
    pop {pc}
.ltorg

.align 2
sw_cols:  .byte 1, 2, 3, 5, 6, 7, 8, 9
.align 2
s_pcolor: .asciz "Colour:"
s_phint1: .asciz "A=paint"
s_phint2: .asciz "U/D pal"
.align 2
