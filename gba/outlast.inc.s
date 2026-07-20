@ ============================================================================
@ UnoDOS / GBA — OutLast. ARMv4T pseudo-3D racer (20 bands, 6px cols on 240 wide).
@ ============================================================================

ol_band_geom:                            @ r0=band -> r0=left r1=right
    mov r2, r0, lsr #1
    add r2, r2, #3
    ldr r3, =ol_scroll
    ldr r3, [r3]
    add r3, r3, r0
    and r3, r3, #15
    cmp r3, #8
    subge r3, r3, #16
    add r3, r3, #20
    sub r0, r3, r2
    cmp r0, #0
    movlt r0, #0
    add r1, r3, r2
    cmp r1, #40
    movge r1, #39
    bx lr
.ltorg

outlast_init:
    push {lr}
    mov r1, #20
    ldr r0, =ol_carx
    str r1, [r0]
    mov r1, #0
    ldr r0, =ol_scroll
    str r1, [r0]
    ldr r0, =ol_dist
    str r1, [r0]
    ldr r0, =ol_over
    str r1, [r0]
    mov r1, #OL_RATE
    ldr r0, =ol_ctr
    str r1, [r0]
    pop {pc}
.ltorg

ol_check:
    push {lr}
    mov r0, #(OL_BANDS-1)
    bl ol_band_geom
    ldr r2, =ol_carx
    ldr r2, [r2]
    cmp r2, r0
    blt olc_off
    cmp r2, r1
    bgt olc_off
    pop {pc}
olc_off:
    mov r0, #1
    ldr r1, =ol_over
    str r0, [r1]
    pop {pc}
.ltorg

outlast_update:
    push {lr}
    ldr r0, =ol_over
    ldr r0, [r0]
    cmp r0, #0
    bne ou_done
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_L
    beq ou_r
    ldr r1, =ol_carx
    ldr r2, [r1]
    cmp r2, #0
    beq ou_r
    sub r2, r2, #1
    str r2, [r1]
    mov r2, #1
    ldr r1, =v_dirty
    str r2, [r1]
ou_r:
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_R
    beq ou_pace
    ldr r1, =ol_carx
    ldr r2, [r1]
    cmp r2, #39
    bge ou_pace
    add r2, r2, #1
    str r2, [r1]
    mov r2, #1
    ldr r1, =v_dirty
    str r2, [r1]
ou_pace:
    ldr r0, =a_gpause
    ldr r0, [r0]
    cmp r0, #0
    bne ou_done
    ldr r1, =ol_ctr
    ldr r0, [r1]
    sub r0, r0, #1
    str r0, [r1]
    cmp r0, #0
    bne ou_done
    mov r0, #OL_RATE
    str r0, [r1]
    ldr r1, =ol_scroll
    ldr r0, [r1]
    add r0, r0, #1
    str r0, [r1]
    ldr r1, =ol_dist
    ldr r0, [r1]
    add r0, r0, #1
    str r0, [r1]
    bl ol_check
    mov r0, #1
    ldr r1, =v_dirty
    str r0, [r1]
ou_done:
    pop {pc}
.ltorg

ol_draw_road:
    push {r4-r7, lr}
    mov r4, #0
odr_loop:
    cmp r4, #OL_BANDS
    bge odr_car
    mov r0, r4
    bl ol_band_geom
    mov r5, r0                            @ left
    mov r6, r1                            @ right
    mov r1, #OL_BH
    mul r7, r4, r1                        @ y
    add r7, r7, #OLO_Y
    mov r0, #5
    bl set_fg
    mov r0, #0
    mov r1, r7
    mov r2, #240
    mov r3, #OL_BH
    bl frect
    mov r0, #9
    bl set_fg
    mov r1, #OL_COLW
    mul r0, r5, r1                        @ x = left*6
    sub r2, r6, r5
    add r2, r2, #1
    mov r3, #OL_COLW
    mul r2, r3, r2                        @ width = OL_COLW*ncols
    mov r1, r7
    mov r3, #OL_BH
    bl frect
    ldr r0, =ol_scroll
    ldr r0, [r0]
    add r0, r0, r4
    tst r0, #1
    beq odr_nostripe
    mov r0, #1
    bl set_fg
    mov r1, #OL_COLW
    mul r0, r5, r1
    mov r1, r7
    mov r2, #OL_COLW
    mov r3, #OL_BH
    bl frect
    mov r0, #1
    bl set_fg
    mov r1, #OL_COLW
    mul r0, r6, r1
    mov r1, r7
    mov r2, #OL_COLW
    mov r3, #OL_BH
    bl frect
odr_nostripe:
    add r4, r4, #1
    b odr_loop
odr_car:
    mov r0, #7
    bl set_fg
    ldr r2, =ol_carx
    ldr r2, [r2]
    mov r1, #OL_COLW
    mul r0, r2, r1
    mov r1, #(OL_BH*(OL_BANDS-1)+OLO_Y)
    mov r2, #OL_COLW
    mov r3, #OL_BH
    bl frect
    pop {r4-r7, pc}
.ltorg

ol_draw_hud:
    push {lr}
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #195
    mov r1, #16
    ldr r2, =s_oldist
    bl pstr
    ldr r0, =ol_dist
    ldr r0, [r0]
    bl pm_itoa5
    mov r0, #195
    mov r1, #28
    ldr r2, =numstr
    bl pstr
    ldr r0, =ol_over
    ldr r0, [r0]
    cmp r0, #0
    beq olh_done
    mov r0, #90
    mov r1, #70
    ldr r2, =s_olcrash
    bl pstr
olh_done:
    pop {pc}
.ltorg

app_outlast:
    ldr r0, =t_outlast
    bl draw_chrome
    bl ol_draw_road
    bl ol_draw_hud
    pop {pc}
.ltorg

.align 2
s_oldist:  .asciz "DIST"
s_olcrash: .asciz "CRASH!"
.align 2
