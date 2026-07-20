@ ============================================================================
@ UnoDOS / GBA — Tracker (icon 6). The 32-row x 4-channel pattern grid from
@ genesis/tracker.i (byte-identical pattern format: note, instr per cell), a
@ 12-row scrolling view. Where the Genesis voices four PSG channels, the GBA
@ port has ONE square voice (channel 1): on each played row the LAST non-empty
@ channel steals the voice, and the instrument column picks the square DUTY
@ (12.5/25/50/75%). Controls: d-pad move, A = note up (empty cell starts C-2),
@ L = note down, R = cycle instrument, SELECT = clear, START = play/stop.
@ Cursor/marker moves repaint only the affected cells (Mode 3 is a bitmap —
@ full-screen repaints are the port's most expensive operation).
@ ============================================================================

.equ TR_ROWS,  32
.equ TR_CHANS, 4
.equ TR_VIEW,  12
.equ TR_RATE,  8               @ frames per row at playback

.equ tr_pat,  0x03000300       @ 256 bytes: (row*4+ch)*2 -> note, instr
.equ t_row,   0x03000400
.equ t_ch,    0x03000404
.equ t_top,   0x03000408
.equ t_play,  0x0300040C
.equ t_prow,  0x03000410
.equ t_tick,  0x03000414
.equ t_orow,  0x03000418
.equ t_och,   0x0300041C
.equ t_oprow, 0x03000420
.equ t_flag,  0x03000424       @ bit0 = cursor cells, bit1 = play-row marker
.equ t_otop,  0x03000428

tracker_init:
    push {r4, lr}
    @ demo pattern -> tr_pat (word copy, both 4-aligned)
    ldr r0, =tr_demo
    ldr r1, =tr_pat
    mov r2, #64
tri_c:
    ldr r3, [r0], #4
    str r3, [r1], #4
    subs r2, r2, #1
    bne tri_c
    @ zero the 11 state words
    ldr r0, =t_row
    mov r1, #0
    mov r2, #11
tri_z:
    str r1, [r0], #4
    subs r2, r2, #1
    bne tri_z
    @ sound on (master + square 1 routed both sides, like music_init)
    ldr r0, =SNDCNT_X
    mov r1, #0x80
    strh r1, [r0]
    ldr r0, =SNDCNT_L
    ldr r1, =0x1177
    strh r1, [r0]
    ldr r0, =SNDCNT_H
    mov r1, #2
    strh r1, [r0]
    pop {r4, pc}
.ltorg

@ ---- per-frame update -------------------------------------------------------
tracker_update:
    push {r4, lr}
    ldr r0, =v_pade
    ldr r4, [r0]
    @ navigation
    tst r4, #(PAD_U|PAD_D|PAD_L|PAD_R)
    beq tu_nonav
    ldr r0, =t_row
    ldr r1, [r0]
    ldr r2, =t_orow
    str r1, [r2]
    ldr r0, =t_ch
    ldr r1, [r0]
    ldr r2, =t_och
    str r1, [r2]
    tst r4, #PAD_U
    beq 1f
    ldr r0, =t_row
    ldr r1, [r0]
    subs r1, r1, #1
    movmi r1, #(TR_ROWS-1)
    str r1, [r0]
1:  tst r4, #PAD_D
    beq 2f
    ldr r0, =t_row
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, #TR_ROWS
    movhs r1, #0
    str r1, [r0]
2:  tst r4, #PAD_L
    beq 3f
    ldr r0, =t_ch
    ldr r1, [r0]
    subs r1, r1, #1
    movmi r1, #(TR_CHANS-1)
    str r1, [r0]
3:  tst r4, #PAD_R
    beq 4f
    ldr r0, =t_ch
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, #TR_CHANS
    movhs r1, #0
    str r1, [r0]
4:  @ keep the cursor row in view; view scroll = full redraw
    ldr r0, =t_row
    ldr r1, [r0]
    ldr r0, =t_top
    ldr r2, [r0]
    cmp r1, r2
    movlt r2, r1
    add r3, r2, #TR_VIEW
    cmp r1, r3
    subge r2, r1, #(TR_VIEW-1)
    str r2, [r0]
    ldr r3, =t_otop
    ldr r12, [r3]
    str r2, [r3]
    cmp r2, r12
    beq tu_part
    ldr r0, =v_dirty
    mov r1, #1
    str r1, [r0]
    b tu_nonav
tu_part:
    bl tr_markcell
tu_nonav:
    @ edits
    tst r4, #PAD_A
    beq 5f
    bl tr_note_up
5:  tst r4, #PAD_SL
    beq 6f
    bl tr_note_dn
6:  tst r4, #PAD_SR
    beq 7f
    bl tr_inst
7:  tst r4, #PAD_SEL
    beq 8f
    bl tr_clear
8:  tst r4, #PAD_ST
    beq 9f
    bl tr_playstop
9:  @ playback advance (frozen once an AUTOTEST script ends)
    ldr r0, =t_play
    ldr r0, [r0]
    cmp r0, #0
    beq tu_done
    ldr r0, =a_gpause
    ldr r0, [r0]
    cmp r0, #0
    bne tu_done
    ldr r0, =t_tick
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, #TR_RATE
    strlo r1, [r0]
    blo tu_done
    mov r1, #0
    str r1, [r0]
    ldr r0, =t_prow
    ldr r1, [r0]
    ldr r2, =t_oprow
    str r1, [r2]
    add r1, r1, #1
    cmp r1, #TR_ROWS
    movhs r1, #0
    str r1, [r0]
    bl tr_trigger
    ldr r0, =t_flag
    ldr r1, [r0]
    orr r1, r1, #2
    str r1, [r0]
tu_done:
    pop {r4, pc}
.ltorg

@ ---- edit helpers (all operate on the cursor cell) --------------------------
tr_cur_ptr:
    ldr r0, =t_row
    ldr r0, [r0]
    ldr r1, =t_ch
    ldr r1, [r1]
    add r0, r1, r0, lsl #2
    ldr r1, =tr_pat
    add r0, r1, r0, lsl #1
    bx lr

tr_markcell:
    ldr r0, =t_flag
    ldr r1, [r0]
    orr r1, r1, #1
    str r1, [r0]
    bx lr

tr_note_up:
    push {lr}
    bl tr_cur_ptr
    ldrb r1, [r0]
    cmp r1, #0
    moveq r1, #1              @ empty starts at C-2
    beq tnu_s
    cmp r1, #24
    addlo r1, r1, #1
tnu_s:
    strb r1, [r0]
    bl tr_markcell
    pop {pc}

tr_note_dn:
    push {lr}
    bl tr_cur_ptr
    ldrb r1, [r0]
    cmp r1, #1
    subhi r1, r1, #1
    strb r1, [r0]
    bl tr_markcell
    pop {pc}

tr_inst:
    push {lr}
    bl tr_cur_ptr
    ldrb r1, [r0]
    cmp r1, #0
    popeq {pc}
    ldrb r1, [r0, #1]
    add r1, r1, #1
    and r1, r1, #3
    strb r1, [r0, #1]
    bl tr_markcell
    pop {pc}

tr_clear:
    push {lr}
    bl tr_cur_ptr
    mov r1, #0
    strh r1, [r0]
    bl tr_markcell
    pop {pc}
.ltorg

tr_playstop:
    push {lr}
    ldr r0, =t_play
    ldr r1, [r0]
    eor r1, r1, #1
    str r1, [r0]
    cmp r1, #0
    bleq tr_silence
    ldr r0, =t_prow           @ start: first tick wraps to row 0
    mov r1, #(TR_ROWS-1)
    str r1, [r0]
    ldr r0, =t_tick
    mov r1, #(TR_RATE-1)
    str r1, [r0]
    ldr r0, =v_dirty          @ status text changes -> full redraw
    mov r1, #1
    str r1, [r0]
    pop {pc}

tr_silence:
    ldr r0, =SND1CNT_H
    mov r1, #0
    strh r1, [r0]             @ envelope volume 0
    bx lr
.ltorg

@ ---- tr_trigger: play row t_prow — last non-empty channel steals the voice --
tr_trigger:
    push {r4-r6, lr}
    ldr r0, =t_prow
    ldr r0, [r0]
    ldr r1, =tr_pat
    add r4, r1, r0, lsl #3    @ row*8
    mov r5, #0                @ stolen note
    mov r6, #0                @ its instrument
    mov r2, #TR_CHANS
tt_l:
    ldrb r0, [r4], #1
    ldrb r1, [r4], #1
    cmp r0, #0
    movne r5, r0
    movne r6, r1
    subs r2, r2, #1
    bne tt_l
    cmp r5, #0
    popeq {r4-r6, pc}         @ all empty: keep ringing (donor semantics)
    mov r0, #0xF000           @ vol 15, duty = instrument
    orr r0, r0, r6, lsl #6
    ldr r1, =SND1CNT_H
    strh r0, [r1]
    sub r5, r5, #1
    ldr r1, =tk_periods
    add r1, r1, r5, lsl #1
    ldrh r0, [r1]
    orr r0, r0, #0x8000       @ restart
    ldr r1, =SND1CNT_X
    strh r0, [r1]
    pop {r4-r6, pc}
.ltorg

@ ============================================================================
@ drawing — full view, then per-cell partials
@ ============================================================================
@ layout: header y=16 ("Rw" + channel names), rows y=26+slot*9 (12 slots),
@ help y=140, chrome title/footer from draw_chrome. Row num x=8; cell x=32+ch*48.
tracker_draw:
    push {r4, lr}
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #8
    mov r1, #16
    ldr r2, =s_tr_rw
    bl pstr
    mov r4, #0
trd_h:
    ldr r1, =tr_chname
    ldr r2, [r1, r4, lsl #2]
    mov r0, #48
    mul r1, r0, r4
    add r0, r1, #32
    mov r1, #16
    bl pstr
    add r4, r4, #1
    cmp r4, #TR_CHANS
    bne trd_h
    @ 12 row slots
    mov r4, #0
trd_r:
    mov r0, r4
    bl tr_draw_slot
    add r4, r4, #1
    cmp r4, #TR_VIEW
    bne trd_r
    @ help + play status
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #0
    mov r1, #140
    ldr r2, =s_tr_help
    bl pstr
    ldr r0, =t_play
    ldr r0, [r0]
    cmp r0, #0
    beq trd_ns
    mov r0, #6
    mov r1, #0
    bl setfb
    mov r0, #208
    mov r1, #16
    ldr r2, =s_tr_play
    bl pstr
trd_ns:
    ldr r0, =t_top
    ldr r1, [r0]
    ldr r0, =t_otop
    str r1, [r0]
    ldr r0, =t_flag
    mov r1, #0
    str r1, [r0]
    pop {r4, pc}
.ltorg

@ tr_draw_slot: r0 = view slot (0..11) -> row number + all 4 cells
tr_draw_slot:
    push {r4-r6, lr}
    mov r4, r0
    ldr r0, =t_top
    ldr r5, [r0]
    add r5, r5, r4            @ absolute row
    mov r0, #9
    mul r1, r0, r4
    add r6, r1, #26           @ y
    @ row number: play row -> black on yellow, else white on desktop
    ldr r0, =t_play
    ldr r0, [r0]
    cmp r0, #0
    beq trs_n
    ldr r0, =t_prow
    ldr r0, [r0]
    cmp r0, r5
    bne trs_n
    mov r0, #4
    mov r1, #6
    b trs_c
trs_n:
    mov r0, #1
    mov r1, #0
trs_c:
    bl setfb
    mov r0, r5
    bl two_digits             @ r1 = tens char, r0 = units char
    ldr r2, =numstr
    strb r1, [r2, #0]
    strb r0, [r2, #1]
    mov r0, #0
    strb r0, [r2, #2]
    mov r0, #8
    mov r1, r6
    bl pstr
    mov r4, #0                @ channel
trs_ch:
    mov r0, r5
    mov r1, r4
    mov r2, r6
    bl tr_draw_cell
    add r4, r4, #1
    cmp r4, #TR_CHANS
    bne trs_ch
    pop {r4-r6, pc}
.ltorg

@ tr_draw_cell: r0 = absolute row, r1 = channel, r2 = y  ("C#2 S" / "--- -")
tr_draw_cell:
    push {r4-r8, lr}
    mov r7, r2                @ y
    mov r8, r1                @ ch
    mov r6, r0                @ row
    add r3, r1, r0, lsl #2
    ldr r1, =tr_pat
    add r3, r1, r3, lsl #1
    ldrb r4, [r3]             @ note
    ldrb r5, [r3, #1]         @ instr
    ldr r3, =clk_str          @ 6-byte cell text scratch
    cmp r4, #0
    bne tdc_n
    mov r1, #'-'
    strb r1, [r3, #0]
    strb r1, [r3, #1]
    strb r1, [r3, #2]
    mov r2, #' '
    strb r2, [r3, #3]
    strb r1, [r3, #4]
    b tdc_t
tdc_n:
    sub r4, r4, #1
    cmp r4, #12
    movhs r2, #'3'
    subhs r4, r4, #12
    movlo r2, #'2'
    ldr r1, =tr_notenames
    add r1, r1, r4, lsl #1
    ldrb r0, [r1]
    strb r0, [r3, #0]
    ldrb r0, [r1, #1]
    strb r0, [r3, #1]
    strb r2, [r3, #2]
    mov r2, #' '
    strb r2, [r3, #3]
    ldr r1, =tr_instlet
    ldrb r0, [r1, r5]
    strb r0, [r3, #4]
tdc_t:
    mov r1, #0
    strb r1, [r3, #5]
    @ cursor cell -> inverted
    ldr r1, =t_row
    ldr r1, [r1]
    cmp r1, r6
    bne tdc_p
    ldr r1, =t_ch
    ldr r1, [r1]
    cmp r1, r8
    bne tdc_p
    mov r0, #0
    mov r1, #1
    b tdc_s
tdc_p:
    mov r0, #1
    mov r1, #0
tdc_s:
    bl setfb
    mov r0, #48
    mul r1, r0, r8
    add r0, r1, #32
    mov r1, r7
    ldr r2, =clk_str
    bl pstr
    pop {r4-r8, pc}
.ltorg

@ ---- partial renderer (vblank) ----------------------------------------------
rp_tracker:
    push {r4, lr}
    ldr r0, =t_flag
    ldr r4, [r0]
    cmp r4, #0
    popeq {r4, pc}
    mov r1, #0
    str r1, [r0]
    tst r4, #1
    beq rpt_2
    ldr r0, =t_orow
    ldr r0, [r0]
    ldr r1, =t_och
    ldr r1, [r1]
    bl tr_cell_if_vis
    ldr r0, =t_row
    ldr r0, [r0]
    ldr r1, =t_ch
    ldr r1, [r1]
    bl tr_cell_if_vis
rpt_2:
    tst r4, #2
    beq rpt_d
    ldr r0, =t_oprow
    ldr r0, [r0]
    bl tr_slot_if_vis
    ldr r0, =t_prow
    ldr r0, [r0]
    bl tr_slot_if_vis
rpt_d:
    pop {r4, pc}

tr_cell_if_vis:               @ r0 = row, r1 = ch
    push {r4, lr}
    ldr r2, =t_top
    ldr r2, [r2]
    subs r3, r0, r2
    popmi {r4, pc}
    cmp r3, #TR_VIEW
    pophs {r4, pc}
    mov r2, #9
    mul r4, r2, r3
    add r2, r4, #26           @ y
    bl tr_draw_cell
    pop {r4, pc}

tr_slot_if_vis:               @ r0 = row
    push {lr}
    ldr r2, =t_top
    ldr r2, [r2]
    subs r3, r0, r2
    popmi {pc}
    cmp r3, #TR_VIEW
    pophs {pc}
    mov r0, r3
    bl tr_draw_slot
    pop {pc}
.ltorg

@ ---------------------------------------------------------------- data
.section .rodata
s_tr_rw:   .asciz "Rw"
s_tr_ch1:  .asciz "Ch1"
s_tr_ch2:  .asciz "Ch2"
s_tr_ch3:  .asciz "Ch3"
s_tr_nz:   .asciz "Nz"
s_tr_play: .asciz "PLAY"
s_tr_help: .asciz "A:+ L:- R:inst SEL:clr ST:play"
tr_notenames: .ascii "C-C#D-D#E-F-F#G-G#A-A#B-"
tr_instlet:   .ascii "SSTN"
.align 2
tr_chname:
    .word s_tr_ch1, s_tr_ch2, s_tr_ch3, s_tr_nz
@ demo song: 32 rows x 4 channels x (note, instr) — byte-identical to the
@ Genesis/Amiga tracker demo (notes 1 = C-2 .. 24 = B-3).
tr_demo:
    .byte 1,1,  13,0,  0,0,  20,3
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 0,0,  17,0,  0,0,   0,0
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 1,1,  20,0,  0,0,  20,3
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 0,0,  17,0, 13,2,   0,0
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 8,1,  13,0, 17,2,  20,3
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 0,0,  15,0,  0,0,   0,0
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 8,1,  20,0, 15,2,  20,3
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 0,0,  15,0,  0,0,   0,0
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 6,1,  10,0, 13,2,  20,3
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 0,0,  13,0,  0,0,   0,0
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 6,1,  17,0,  0,0,  20,3
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 0,0,  13,0, 10,2,   0,0
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 8,1,  11,0, 15,2,  20,3
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 0,0,  15,0,  0,0,   0,0
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 8,1,  20,0, 19,2,  20,3
    .byte 0,0,   0,0,  0,0,   0,0
    .byte 0,0,  23,0,  0,0,  20,3
    .byte 0,0,   0,0,  0,0,   0,0
.align 2
.section .text
