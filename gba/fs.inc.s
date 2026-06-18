@ ============================================================================
@ UnoDOS / GBA — USV1 byte-heap FS + Files browser + Notepad save (ARMv4T).
@ FS lives in IWRAM (FSBASE), persisted by the harness; on real hardware backed
@ by cart SRAM (the SRAM byte-bus copy is the documented hardware tail). 240x160.
@ ============================================================================

.equ FSBASE,      VARS+0x2000
.equ FSHEAP,      FSBASE+256
.equ FS_HEAPSIZE, 0x1000-256
.equ FS_MAXFILES, 15
.equ FSC_COUNT,   4
.equ FSC_USED,    5
.equ FSC_DIR,     16
.equ FSE_SIZE,    12
.equ FSE_START,   14
.equ FSE_LEN,     16

fs_entry_ptr:
    ldr r1, =FSBASE
    add r1, r1, #FSC_DIR
    add r0, r1, r0, lsl #4
    bx lr
.ltorg
fs_count:
    ldr r1, =FSBASE
    ldrb r0, [r1, #FSC_COUNT]
    bx lr
.ltorg
fs_get_used:
    ldr r1, =FSBASE
    ldrb r0, [r1, #FSC_USED]
    ldrb r2, [r1, #(FSC_USED+1)]
    orr r0, r0, r2, lsl #8
    bx lr
.ltorg
fs_set_used:
    ldr r1, =FSBASE
    strb r0, [r1, #FSC_USED]
    mov r2, r0, lsr #8
    strb r2, [r1, #(FSC_USED+1)]
    bx lr
.ltorg

fs_find:                                 @ r0=name -> r0=idx or -1
    push {r4-r6, lr}
    mov r4, r0
    bl fs_count
    mov r5, r0
    mov r6, #0
ffl:
    cmp r6, r5
    bge ff_no
    mov r0, r6
    bl fs_entry_ptr
    mov r1, #0
ffc:
    cmp r1, #12
    bge ff_match
    ldrb r2, [r0, r1]
    ldrb r3, [r4, r1]
    cmp r2, r3
    bne ff_next
    add r1, r1, #1
    b ffc
ff_match:
    mov r0, r6
    pop {r4-r6, pc}
ff_next:
    add r6, r6, #1
    b ffl
ff_no:
    mvn r0, #0
    pop {r4-r6, pc}
.ltorg

fs_read:                                 @ r0=idx r1=dest -> r0=size
    push {r4-r6, lr}
    mov r4, r1
    bl fs_entry_ptr
    ldrh r1, [r0, #FSE_SIZE]
    ldrh r2, [r0, #FSE_START]
    ldr r3, =FSHEAP
    add r3, r3, r2
    mov r5, r1
fsr_cp:
    cmp r1, #0
    beq fsr_done
    ldrb r6, [r3], #1
    strb r6, [r4], #1
    sub r1, r1, #1
    b fsr_cp
fsr_done:
    mov r0, r5
    pop {r4-r6, pc}
.ltorg

fs_delete:                               @ r0=idx
    push {r4-r10, lr}
    mov r4, r0
    bl fs_entry_ptr
    mov r5, r0
    ldrh r6, [r5, #FSE_SIZE]
    ldrh r7, [r5, #FSE_START]
    bl fs_get_used
    mov r8, r0
    sub r0, r8, r7
    sub r0, r0, r6
    ldr r1, =FSHEAP
    add r2, r1, r7
    add r3, r2, r6
fd_cp:
    cmp r0, #0
    beq fd_cpd
    ldrb r9, [r3], #1
    strb r9, [r2], #1
    sub r0, r0, #1
    b fd_cp
fd_cpd:
    sub r0, r8, r6
    bl fs_set_used
    mov r8, #0
fd_fix:
    bl fs_count
    cmp r8, r0
    bge fd_drop
    mov r0, r8
    bl fs_entry_ptr
    mov r1, r0
    ldrh r2, [r1, #FSE_START]
    cmp r2, r7
    ble fd_fixn
    sub r2, r2, r6
    strh r2, [r1, #FSE_START]
fd_fixn:
    add r8, r8, #1
    b fd_fix
fd_drop:
    mov r8, r4
fd_sh:
    add r9, r8, #1
    bl fs_count
    cmp r9, r0
    bge fd_cnt
    mov r0, r9
    bl fs_entry_ptr
    mov r10, r0
    mov r0, r8
    bl fs_entry_ptr
    mov r1, r0
    mov r2, #0
fd_ce:
    cmp r2, #FSE_LEN
    bge fd_cen
    ldrb r3, [r10, r2]
    strb r3, [r1, r2]
    add r2, r2, #1
    b fd_ce
fd_cen:
    add r8, r8, #1
    b fd_sh
fd_cnt:
    ldr r0, =FSBASE
    ldrb r1, [r0, #FSC_COUNT]
    sub r1, r1, #1
    strb r1, [r0, #FSC_COUNT]
    pop {r4-r10, pc}
.ltorg

fs_save:                                 @ r0=name r1=data r2=size -> r0=0/-1
    push {r4-r9, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
    mov r0, r4
    bl fs_find
    cmp r0, #0
    blt fsv_fresh
    bl fs_delete
fsv_fresh:
    bl fs_count
    cmp r0, #FS_MAXFILES
    blt fsv_room
    mvn r0, #0
    pop {r4-r9, pc}
fsv_room:
    bl fs_get_used
    add r1, r0, r6
    cmp r1, #FS_HEAPSIZE
    ble fsv_fits
    mvn r0, #0
    pop {r4-r9, pc}
fsv_fits:
    bl fs_count
    mov r7, r0
    mov r0, r7
    bl fs_entry_ptr
    mov r8, r0
    mov r1, #0
fsv_nm:
    cmp r1, #12
    bge fsv_nmd
    ldrb r2, [r4, r1]
    strb r2, [r8, r1]
    add r1, r1, #1
    b fsv_nm
fsv_nmd:
    strh r6, [r8, #FSE_SIZE]
    bl fs_get_used
    mov r9, r0
    strh r9, [r8, #FSE_START]
    ldr r0, =FSHEAP
    add r0, r0, r9
    mov r1, #0
fsv_cp:
    cmp r1, r6
    bge fsv_cpd
    ldrb r2, [r5, r1]
    strb r2, [r0, r1]
    add r1, r1, #1
    b fsv_cp
fsv_cpd:
    bl fs_get_used
    add r0, r0, r6
    bl fs_set_used
    ldr r0, =FSBASE
    ldrb r1, [r0, #FSC_COUNT]
    add r1, r1, #1
    strb r1, [r0, #FSC_COUNT]
    mov r0, #0
    pop {r4-r9, pc}
.ltorg

fs_init:
    push {lr}
    ldr r0, =FSBASE
    ldrb r1, [r0, #0]
    cmp r1, #'U'
    bne fsi_format
    ldrb r1, [r0, #1]
    cmp r1, #'S'
    bne fsi_format
    ldrb r1, [r0, #2]
    cmp r1, #'V'
    bne fsi_format
    ldrb r1, [r0, #3]
    cmp r1, #'1'
    beq fsi_done
fsi_format:
    ldr r0, =FSBASE
    mov r1, #'U'
    strb r1, [r0, #0]
    mov r1, #'S'
    strb r1, [r0, #1]
    mov r1, #'V'
    strb r1, [r0, #2]
    mov r1, #'1'
    strb r1, [r0, #3]
    mov r1, #0
    strb r1, [r0, #FSC_COUNT]
    strb r1, [r0, #FSC_USED]
    strb r1, [r0, #(FSC_USED+1)]
    add r0, r0, #FSC_DIR
    mov r1, #(256-FSC_DIR)
fsi_zero:
    mov r2, #0
    strb r2, [r0], #1
    subs r1, r1, #1
    bne fsi_zero
    ldr r0, =seed_readme_name
    ldr r1, =seed_readme
    mov r2, #(seed_readme_end - seed_readme)
    bl fs_save
    ldr r0, =seed_hello_name
    ldr r1, =seed_hello
    mov r2, #(seed_hello_end - seed_hello)
    bl fs_save
fsi_done:
    pop {pc}
.ltorg

files_init:
    mov r1, #0
    ldr r0, =fl_sel
    str r1, [r0]
    ldr r0, =fl_view
    str r1, [r0]
    bx lr
.ltorg

files_update:
    push {r4, lr}
    ldr r0, =v_pade
    ldr r4, [r0]
    ldr r0, =fl_view
    ldr r0, [r0]
    cmp r0, #0
    bne flu_view
    tst r4, #PAD_U
    beq flu_d
    ldr r0, =fl_sel
    ldr r1, [r0]
    cmp r1, #0
    subgt r1, r1, #1
    strgt r1, [r0]
flu_d:
    tst r4, #PAD_D
    beq flu_a
    bl fs_count
    mov r2, r0
    ldr r0, =fl_sel
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, r2
    strlt r1, [r0]
flu_a:
    tst r4, #PAD_A
    beq flu_done
    bl fs_count
    cmp r0, #0
    beq flu_done
    mov r1, #1
    ldr r0, =fl_view
    str r1, [r0]
    b flu_done
flu_view:
    tst r4, #PAD_A
    beq flu_done
    mov r1, #0
    ldr r0, =fl_view
    str r1, [r0]
flu_done:
    cmp r4, #0
    beq flu_ret
    mov r0, #1
    ldr r1, =v_dirty
    str r0, [r1]
flu_ret:
    pop {r4, pc}
.ltorg

files_draw:
    push {lr}
    ldr r0, =fl_view
    ldr r0, [r0]
    cmp r0, #0
    bne fld_view
    ldr r0, =t_files
    bl draw_chrome
    bl fl_draw_list
    pop {pc}
fld_view:
    ldr r0, =t_files
    bl draw_chrome
    bl fl_draw_file
    pop {pc}
.ltorg

fl_draw_list:
    push {r4, lr}
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #4
    mov r1, #18
    ldr r2, =s_fl_hdr
    bl pstr
    bl fs_count
    cmp r0, #0
    bne fll_loop
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #4
    mov r1, #40
    ldr r2, =s_fl_empty
    bl pstr
    b fll_done
fll_loop:
    mov r4, #0
fll_l:
    bl fs_count
    cmp r4, r0
    bge fll_done
    ldr r0, =fl_sel
    ldr r0, [r0]
    cmp r0, r4
    bne fll_norm
    mov r0, #0
    mov r1, #1
    bl setfb
    b fll_show
fll_norm:
    mov r0, #1
    mov r1, #0
    bl setfb
fll_show:
    mov r0, r4
    bl fs_entry_ptr
    mov r2, r0
    mov r0, #12
    mov r1, #14
    mul r1, r4, r1
    add r1, r1, #34
    bl pstr_n12
    add r4, r4, #1
    b fll_l
fll_done:
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #4
    mov r1, #148
    ldr r2, =s_fl_help
    bl pstr
    pop {r4, pc}
.ltorg

pstr_n12:                                @ r0=x r1=y r2=name -> up to 12 chars
    push {r4-r7, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
    mov r7, #0
pn12_l:
    cmp r7, #12
    bge pn12_d
    ldrb r2, [r6, r7]
    cmp r2, #0
    beq pn12_d
    mov r0, r4
    mov r1, r5
    bl pchar
    add r4, r4, #8
    add r7, r7, #1
    b pn12_l
pn12_d:
    pop {r4-r7, pc}
.ltorg

fl_draw_file:
    push {lr}
    ldr r0, =fl_sel
    ldr r0, [r0]
    ldr r1, =fbuf
    bl fs_read
    mov r1, r0
    ldr r0, =fbuf
    bl draw_textbuf
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #4
    mov r1, #148
    ldr r2, =s_fl_vback
    bl pstr
    pop {pc}
.ltorg

draw_textbuf:                            @ r0=ptr r1=len
    push {r4-r8, lr}
    mov r4, r0
    mov r5, r1
    mov r6, #4
    mov r7, #18
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r8, #0
dtb_l:
    cmp r8, r5
    bge dtb_done
    ldrb r0, [r4, r8]
    cmp r0, #13
    beq dtb_nl
    cmp r0, #10
    beq dtb_nl
    mov r2, r0
    mov r0, r6
    mov r1, r7
    bl pchar
    add r6, r6, #8
    cmp r6, #232
    blt dtb_nx
    mov r6, #4
    add r7, r7, #12
dtb_nx:
    add r8, r8, #1
    b dtb_l
dtb_nl:
    mov r6, #4
    add r7, r7, #12
    add r8, r8, #1
    b dtb_l
dtb_done:
    pop {r4-r8, pc}
.ltorg

notepad_init:
    mov r1, #0
    ldr r0, =np_saved
    str r1, [r0]
    bx lr
.ltorg

notepad_update:
    push {lr}
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_A
    beq npu_done
    ldr r0, =notes_name
    ldr r1, =np_text
    mov r2, #(np_text_end - np_text)
    bl fs_save
    mov r1, #1
    ldr r0, =np_saved
    str r1, [r0]
    mov r1, #1
    ldr r0, =v_dirty
    str r1, [r0]
npu_done:
    pop {pc}
.ltorg

notepad_draw:
    push {lr}
    ldr r0, =t_notepad
    bl draw_chrome
    ldr r0, =c_notepad
    bl draw_content
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #4
    mov r1, #100
    ldr r2, =s_np_hint
    bl pstr
    ldr r0, =np_saved
    ldr r0, [r0]
    cmp r0, #0
    beq npd_done
    mov r0, #5
    mov r1, #0
    bl setfb
    mov r0, #4
    mov r1, #114
    ldr r2, =s_np_saved
    bl pstr
npd_done:
    pop {pc}
.ltorg

.align 2
s_fl_hdr:    .asciz "Files (USV1):"
s_fl_empty:  .asciz "(empty)"
s_fl_help:   .asciz "U/D  A=open"
s_fl_vback:  .asciz "A=back"
s_np_hint:   .asciz "A=save NOTES"
s_np_saved:  .asciz "Saved!"
seed_readme_name: .ascii "README.TXT"
                  .byte 0,0
seed_hello_name:  .ascii "HELLO.TXT"
                  .byte 0,0,0
notes_name:       .ascii "NOTES.TXT"
                  .byte 0,0,0
seed_readme:
    .ascii "UnoDOS / GBA.\r"
    .ascii "USV1 disk, harness-\r"
    .ascii "persisted; SRAM on HW.\r"
seed_readme_end:
seed_hello:
    .ascii "Hello from the GBA!\r"
    .ascii "Notepad: A saves.\r"
seed_hello_end:
np_text:
    .ascii "Saved from Notepad\r"
    .ascii "on UnoDOS / GBA.\r"
np_text_end:
.align 2
