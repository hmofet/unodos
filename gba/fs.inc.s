@ ============================================================================
@ UnoDOS / GBA — USV1 mini-FS (the c64/fs.i byte-heap catalog re-expressed in
@ ARM) + the dynamic Files and Notepad apps that sit on it.
@
@ Storage is the first 4 KB of EWRAM (0x02000000..0x02000FFF) — the flat-RAM
@ region the harness persists across runs via a --storage sidecar file (the
@ same scheme the C64 harness uses). On real hardware this region would be
@ backed by cart SRAM (out of scope here).
@
@ Layout (little-endian):
@   +0..3    magic "USV1"
@   +4       file count (byte, 0..15)
@   +6..7    heap_used (hword — moved from the C64's +5 for ARM alignment)
@   +16..255 15 dir entries x 16: name[12], size hword @12, start hword @14
@   +256..   the data heap (3840 bytes), grows upward, compacted on delete
@ fs_save = delete-then-append, exactly like the donor.
@ ============================================================================

.equ FSBASE,    0x02000000
.equ FSHEAP,    (FSBASE+256)
.equ HEAPSZ,    3840
.equ FSC_COUNT, 4
.equ FSC_USED,  6
.equ FSC_DIR,   16
.equ FSE_SIZE,  12
.equ FSE_START, 14

.equ fname_buf, 0x03000BB0     @ 16 bytes: NUL-terminated name scratch
.equ np_len,    0x03000BC0     @ Notepad text length
.equ np_buf,    0x03000C00     @ Notepad text buffer (256)
.equ NP_MAX,    240

@ ---- fs_init: validate the store; format + seed only if "USV1" is absent ----
fs_init:
    push {r4, lr}
    ldr r4, =FSBASE
    ldrb r0, [r4, #0]
    cmp r0, #'U'
    bne fsi_fmt
    ldrb r0, [r4, #1]
    cmp r0, #'S'
    bne fsi_fmt
    ldrb r0, [r4, #2]
    cmp r0, #'V'
    bne fsi_fmt
    ldrb r0, [r4, #3]
    cmp r0, #'1'
    beq fsi_done
fsi_fmt:
    mov r0, #'U'
    strb r0, [r4, #0]
    mov r0, #'S'
    strb r0, [r4, #1]
    mov r0, #'V'
    strb r0, [r4, #2]
    mov r0, #'1'
    strb r0, [r4, #3]
    add r2, r4, #4
    mov r0, #0
    mov r1, #252              @ zero count/used/reserved + directory
fsi_z:
    strb r0, [r2], #1
    subs r1, r1, #1
    bne fsi_z
    ldr r0, =fsn_readme
    ldr r1, =fsd_readme
    ldr r2, =fsd_readme_len
    bl fs_save
    ldr r0, =fsn_hello
    ldr r1, =fsd_hello
    ldr r2, =fsd_hello_len
    bl fs_save
fsi_done:
    pop {r4, pc}
.ltorg

@ ---- fs_entry_ptr: r0 = index -> r0 = &dir[idx] (clobbers r1) ---------------
fs_entry_ptr:
    mov r0, r0, lsl #4
    add r0, r0, #FSC_DIR
    ldr r1, =FSBASE
    add r0, r0, r1
    bx lr
.ltorg

@ ---- fs_find: r0 = 12-byte name ptr -> r0 = index or -1 ---------------------
fs_find:
    push {r4-r7, lr}
    mov r4, r0
    ldr r7, =FSBASE
    ldrb r7, [r7, #FSC_COUNT]
    mov r5, #0
ff_l:
    cmp r5, r7
    mvnhs r0, #0
    pophs {r4-r7, pc}
    mov r0, r5
    bl fs_entry_ptr
    mov r1, r4
    mov r2, #12
ff_c:
    ldrb r3, [r0], #1
    ldrb r6, [r1], #1
    cmp r3, r6
    bne ff_n
    subs r2, r2, #1
    bne ff_c
    mov r0, r5
    pop {r4-r7, pc}
ff_n:
    add r5, r5, #1
    b ff_l
.ltorg

@ ---- fs_read: r0 = index, r1 = dst, r2 = cap -> r0 = bytes copied -----------
fs_read:
    push {r4-r6, lr}
    mov r4, r1
    mov r5, r2
    bl fs_entry_ptr
    ldrh r1, [r0, #FSE_SIZE]
    cmp r1, r5
    movhi r1, r5              @ clamp to the caller's buffer (donor S1 fix)
    ldrh r2, [r0, #FSE_START]
    ldr r3, =FSHEAP
    add r2, r3, r2
    mov r6, r1
frd_l:
    cmp r1, #0
    beq frd_d
    ldrb r0, [r2], #1
    strb r0, [r4], #1
    sub r1, r1, #1
    b frd_l
frd_d:
    mov r0, r6
    pop {r4-r6, pc}
.ltorg

@ ---- fs_delete: r0 = index (compact heap, fix starts, drop the slot) --------
fs_delete:
    push {r4-r8, lr}
    mov r8, r0
    bl fs_entry_ptr
    ldrh r4, [r0, #FSE_SIZE]      @ N
    ldrh r5, [r0, #FSE_START]     @ S
    ldr r6, =FSBASE
    @ memmove heap[S+N..used) down to heap[S]
    ldrh r7, [r6, #FSC_USED]
    sub r7, r7, r5
    sub r7, r7, r4                @ len
    ldr r1, =FSHEAP
    add r0, r1, r5                @ dst
    add r1, r0, r4                @ src
fd_mv:
    cmp r7, #0
    beq fd_mvd
    ldrb r2, [r1], #1
    strb r2, [r0], #1
    sub r7, r7, #1
    b fd_mv
fd_mvd:
    ldrh r0, [r6, #FSC_USED]
    sub r0, r0, r4
    strh r0, [r6, #FSC_USED]
    @ fix start of every entry whose start > S
    ldrb r7, [r6, #FSC_COUNT]
    mov r2, #0
fd_fx:
    cmp r2, r7
    bhs fd_shift
    mov r0, r2
    push {r2}
    bl fs_entry_ptr
    pop {r2}
    ldrh r1, [r0, #FSE_START]
    cmp r1, r5
    bls fd_fxn
    sub r1, r1, r4
    strh r1, [r0, #FSE_START]
fd_fxn:
    add r2, r2, #1
    b fd_fx
fd_shift:
    @ shift dir entries idx+1..count-1 down one slot
    add r2, r8, #1
fd_sh:
    cmp r2, r7
    bhs fd_cnt
    mov r0, r2
    push {r2}
    bl fs_entry_ptr
    pop {r2}
    mov r1, r0                @ src
    sub r0, r0, #16           @ dst
    mov r3, #16
fd_cp:
    ldrb r12, [r1], #1
    strb r12, [r0], #1
    subs r3, r3, #1
    bne fd_cp
    add r2, r2, #1
    b fd_sh
fd_cnt:
    ldrb r0, [r6, #FSC_COUNT]
    sub r0, r0, #1
    strb r0, [r6, #FSC_COUNT]
    pop {r4-r8, pc}
.ltorg

@ ---- fs_save: r0 = name, r1 = data, r2 = size -> r0 = 0 ok / -1 full --------
fs_save:
    push {r4-r8, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
    bl fs_find                @ name still in r0
    cmn r0, #1
    blne fs_delete            @ delete-then-append
    ldr r7, =FSBASE
    ldrb r0, [r7, #FSC_COUNT]
    cmp r0, #15
    mvnhs r0, #0
    pophs {r4-r8, pc}
    ldrh r1, [r7, #FSC_USED]
    add r2, r1, r6
    ldr r3, =HEAPSZ
    cmp r2, r3
    mvnhi r0, #0
    pophi {r4-r8, pc}
    bl fs_entry_ptr           @ r0 = count = the new slot
    mov r8, r0
    mov r2, #12
fsv_n:
    ldrb r1, [r4], #1
    strb r1, [r0], #1
    subs r2, r2, #1
    bne fsv_n
    strh r6, [r8, #FSE_SIZE]
    ldrh r1, [r7, #FSC_USED]
    strh r1, [r8, #FSE_START]
    ldr r0, =FSHEAP
    add r0, r0, r1
    mov r2, r6
fsv_c:
    cmp r2, #0
    beq fsv_d
    ldrb r3, [r5], #1
    strb r3, [r0], #1
    sub r2, r2, #1
    b fsv_c
fsv_d:
    ldrh r1, [r7, #FSC_USED]
    add r1, r1, r6
    strh r1, [r7, #FSC_USED]
    ldrb r0, [r7, #FSC_COUNT]
    add r0, r0, #1
    strb r0, [r7, #FSC_COUNT]
    mov r0, #0
    pop {r4-r8, pc}
.ltorg

@ ============================================================================
@ Files — list the USV1 directory (name + size per row)
@ ============================================================================
files_draw:
    push {r4-r7, lr}
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #8
    mov r1, #20
    ldr r2, =s_fi_hdr
    bl pstr
    ldr r7, =FSBASE
    ldrb r7, [r7, #FSC_COUNT]
    mov r4, #0
fld_l:
    cmp r4, r7
    bhs fld_done
    mov r0, r4
    bl fs_entry_ptr
    mov r5, r0
    ldr r6, =fname_buf
    mov r1, #12
fld_n:
    ldrb r2, [r5], #1
    strb r2, [r6], #1
    subs r1, r1, #1
    bne fld_n
    mov r2, #0
    strb r2, [r6]
    @ y = 34 + idx*10
    mov r1, #10
    mul r6, r1, r4
    add r6, r6, #34
    mov r0, #8
    mov r1, r6
    ldr r2, =fname_buf
    bl pstr
    mov r0, r4
    bl fs_entry_ptr
    ldrh r0, [r0, #FSE_SIZE]
    bl num5
    mov r0, #120
    mov r1, r6
    ldr r2, =numstr
    bl pstr
    add r4, r4, #1
    b fld_l
fld_done:
    @ heap use: "used NNNNN" at the bottom of the content area
    ldr r0, =FSBASE
    ldrh r0, [r0, #FSC_USED]
    bl num5
    mov r0, #8
    mov r1, #128
    ldr r2, =s_fi_used
    bl pstr
    mov r0, #56
    mov r1, #128
    ldr r2, =numstr
    bl pstr
    pop {r4-r7, pc}
.ltorg

@ ============================================================================
@ Notepad — loads HELLO.TXT; A appends '+' and saves (load/save round-trip)
@ ============================================================================
notepad_init:
    push {lr}
    ldr r0, =fsn_hello
    bl fs_find
    cmn r0, #1
    bne npi_read
    ldr r1, =np_len
    mov r0, #0
    str r0, [r1]
    pop {pc}
npi_read:
    ldr r1, =np_buf
    mov r2, #NP_MAX
    bl fs_read
    ldr r1, =np_len
    str r0, [r1]
    pop {pc}
.ltorg

notepad_update:
    push {lr}
    ldr r0, =v_pade
    ldr r0, [r0]
    tst r0, #PAD_A
    popeq {pc}
    ldr r1, =np_len
    ldr r2, [r1]
    cmp r2, #NP_MAX
    pophs {pc}
    ldr r3, =np_buf
    mov r0, #'+'
    strb r0, [r3, r2]
    add r2, r2, #1
    str r2, [r1]
    ldr r0, =fsn_hello
    mov r1, r3
    bl fs_save                @ r2 = new length
    ldr r0, =v_dirty
    mov r1, #1
    str r1, [r0]
    pop {pc}
.ltorg

notepad_draw:
    push {r4-r7, lr}
    mov r0, #1
    mov r1, #0
    bl setfb
    mov r0, #8
    mov r1, #20
    ldr r2, =fsn_hello
    bl pstr
    @ length readout (the storage-proof number)
    ldr r0, =np_len
    ldr r0, [r0]
    bl num5
    mov r0, #192
    mov r1, #20
    ldr r2, =numstr
    bl pstr
    @ text, wrapped at 28 cols; control bytes break the line
    ldr r4, =np_buf
    ldr r7, =np_len
    ldr r7, [r7]
    add r7, r4, r7
    mov r5, #8                @ x
    mov r6, #34               @ y
npd_l:
    cmp r4, r7
    bhs npd_done
    ldrb r2, [r4], #1
    cmp r2, #32
    blo npd_nl
    mov r0, r5
    mov r1, r6
    bl pchar
    add r5, r5, #8
    cmp r5, #232
    blo npd_l
npd_nl:
    mov r5, #8
    add r6, r6, #10
    cmp r6, #136
    blo npd_l
npd_done:
    mov r0, #8
    mov r1, #140
    ldr r2, =s_np_help
    bl pstr
    pop {r4-r7, pc}
.ltorg

.section .rodata
s_fi_hdr:  .asciz "USV1 store:"
s_fi_used: .asciz "used"
s_np_help: .asciz "A = append '+' and save"
fsn_readme: .ascii "README.TXT"
            .byte 0,0
fsn_hello:  .ascii "HELLO.TXT"
            .byte 0,0,0
fsd_readme:
    .ascii "UnoDOS/GBA - the USV1 mini-FS\r"
    .ascii "lives in EWRAM and persists\r"
    .ascii "via the harness sidecar.\r"
fsd_readme_end:
.equ fsd_readme_len, fsd_readme_end - fsd_readme
fsd_hello:
    .ascii "Hello from the GBA!"
fsd_hello_end:
.equ fsd_hello_len, fsd_hello_end - fsd_hello
.align 2
.section .text
