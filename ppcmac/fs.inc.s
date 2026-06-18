# ============================================================================
# UnoDOS / PowerPC Mac — USV1 byte-heap filesystem + Files browser + Notepad save.
# PowerPC translation of the rpi port. FSBASE is a region the harness persists
# across runs; on real hardware it is backed by the boot disk (the .Sony/SCSI
# driver) — the documented hardware tail. 16-bit fields are native (big-endian),
# consistent within the port.
# ============================================================================

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

fs_entry_ptr:                            # r3=idx -> r3=entry ptr. Leaf.
    LA    r4, FSBASE
    addi  r4, r4, FSC_DIR
    slwi  r3, r3, 4
    add   r3, r4, r3
    blr

fs_count:                                # -> r3. Leaf.
    LA    r4, FSBASE
    lbz   r3, FSC_COUNT(r4)
    blr

fs_get_used:                             # -> r3. Leaf.
    LA    r4, FSBASE
    lbz   r3, FSC_USED(r4)
    lbz   r5, (FSC_USED+1)(r4)
    slwi  r5, r5, 8
    or    r3, r3, r5
    blr

fs_set_used:                             # r3=used. Leaf.
    LA    r4, FSBASE
    stb   r3, FSC_USED(r4)
    srwi  r5, r3, 8
    stb   r5, (FSC_USED+1)(r4)
    blr

fs_find:                                 # r3=name -> r3=idx or -1.
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    mr    r14, r3
    bl    fs_count
    mr    r15, r3
    li    r16, 0
ffl:
    cmpw  r16, r15
    bge   ff_no
    mr    r3, r16
    bl    fs_entry_ptr
    li    r4, 0
ffc:
    cmpwi r4, 12
    bge   ff_match
    lbzx  r5, r3, r4
    lbzx  r6, r14, r4
    cmpw  r5, r6
    bne   ff_next
    addi  r4, r4, 1
    b     ffc
ff_match:
    mr    r3, r16
    b     ff_out
ff_next:
    addi  r16, r16, 1
    b     ffl
ff_no:
    li    r3, -1
ff_out:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

fs_read:                                 # r3=idx, r4=dest -> r3=size.
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    stw   r14, 8(r1)
    mr    r14, r4
    bl    fs_entry_ptr
    lhz   r4, FSE_SIZE(r3)
    lhz   r5, FSE_START(r3)
    LA    r6, FSHEAP
    add   r6, r6, r5
    mr    r7, r4
fsr_cp:
    cmpwi r4, 0
    beq   fsr_done
    lbz   r8, 0(r6)
    stb   r8, 0(r14)
    addi  r6, r6, 1
    addi  r14, r14, 1
    subi  r4, r4, 1
    b     fsr_cp
fsr_done:
    mr    r3, r7
    lwz   r14, 8(r1)
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

fs_delete:                               # r3=idx.
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    stw   r18, 24(r1)
    stw   r19, 28(r1)
    stw   r20, 32(r1)
    mr    r14, r3
    bl    fs_entry_ptr
    mr    r15, r3
    lhz   r16, FSE_SIZE(r15)             # N
    lhz   r17, FSE_START(r15)            # S
    bl    fs_get_used
    mr    r18, r3                        # used
    subf  r3, r17, r18                   # used - S
    subf  r3, r16, r3                    # - N -> len
    LA    r4, FSHEAP
    add   r5, r4, r17                    # dst
    add   r6, r5, r16                    # src
fd_cp:
    cmpwi r3, 0
    beq   fd_cpd
    lbz   r7, 0(r6)
    stb   r7, 0(r5)
    addi  r6, r6, 1
    addi  r5, r5, 1
    subi  r3, r3, 1
    b     fd_cp
fd_cpd:
    subf  r3, r16, r18                   # used - N
    bl    fs_set_used
    li    r18, 0
fd_fix:
    bl    fs_count
    cmpw  r18, r3
    bge   fd_drop
    mr    r3, r18
    bl    fs_entry_ptr
    mr    r4, r3
    lhz   r5, FSE_START(r4)
    cmpw  r5, r17
    ble   fd_fixn
    subf  r5, r16, r5
    sth   r5, FSE_START(r4)
fd_fixn:
    addi  r18, r18, 1
    b     fd_fix
fd_drop:
    mr    r18, r14
fd_sh:
    addi  r19, r18, 1
    bl    fs_count
    cmpw  r19, r3
    bge   fd_cnt
    mr    r3, r19
    bl    fs_entry_ptr
    mr    r20, r3
    mr    r3, r18
    bl    fs_entry_ptr
    mr    r4, r3
    li    r5, 0
fd_ce:
    cmpwi r5, FSE_LEN
    bge   fd_cen
    lbzx  r6, r20, r5
    stbx  r6, r4, r5
    addi  r5, r5, 1
    b     fd_ce
fd_cen:
    addi  r18, r18, 1
    b     fd_sh
fd_cnt:
    LA    r3, FSBASE
    lbz   r4, FSC_COUNT(r3)
    subi  r4, r4, 1
    stb   r4, FSC_COUNT(r3)
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r18, 24(r1)
    lwz   r19, 28(r1)
    lwz   r20, 32(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

fs_save:                                 # r3=name r4=data r5=size -> r3=0/-1.
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    stw   r18, 24(r1)
    stw   r19, 28(r1)
    mr    r14, r3
    mr    r15, r4
    mr    r16, r5
    mr    r3, r14
    bl    fs_find
    cmpwi r3, 0
    blt   fsv_fresh
    bl    fs_delete
fsv_fresh:
    bl    fs_count
    cmpwi r3, FS_MAXFILES
    blt   fsv_room
    li    r3, -1
    b     fsv_out
fsv_room:
    bl    fs_get_used
    add   r4, r3, r16
    cmpwi r4, FS_HEAPSIZE
    ble   fsv_fits
    li    r3, -1
    b     fsv_out
fsv_fits:
    bl    fs_count
    mr    r17, r3
    mr    r3, r17
    bl    fs_entry_ptr
    mr    r18, r3
    li    r4, 0
fsv_nm:
    cmpwi r4, 12
    bge   fsv_nmd
    lbzx  r5, r14, r4
    stbx  r5, r18, r4
    addi  r4, r4, 1
    b     fsv_nm
fsv_nmd:
    sth   r16, FSE_SIZE(r18)
    bl    fs_get_used
    mr    r19, r3
    sth   r19, FSE_START(r18)
    LA    r3, FSHEAP
    add   r3, r3, r19
    li    r4, 0
fsv_cp:
    cmpw  r4, r16
    bge   fsv_cpd
    lbzx  r5, r15, r4
    stbx  r5, r3, r4
    addi  r4, r4, 1
    b     fsv_cp
fsv_cpd:
    bl    fs_get_used
    add   r3, r3, r16
    bl    fs_set_used
    LA    r3, FSBASE
    lbz   r4, FSC_COUNT(r3)
    addi  r4, r4, 1
    stb   r4, FSC_COUNT(r3)
    li    r3, 0
fsv_out:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r18, 24(r1)
    lwz   r19, 28(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

fs_init:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r3, FSBASE
    lbz   r4, 0(r3)
    cmpwi r4, 'U'
    bne   fsi_format
    lbz   r4, 1(r3)
    cmpwi r4, 'S'
    bne   fsi_format
    lbz   r4, 2(r3)
    cmpwi r4, 'V'
    bne   fsi_format
    lbz   r4, 3(r3)
    cmpwi r4, '1'
    beq   fsi_done
fsi_format:
    LA    r3, FSBASE
    li    r4, 'U'
    stb   r4, 0(r3)
    li    r4, 'S'
    stb   r4, 1(r3)
    li    r4, 'V'
    stb   r4, 2(r3)
    li    r4, '1'
    stb   r4, 3(r3)
    li    r4, 0
    stb   r4, FSC_COUNT(r3)
    stb   r4, FSC_USED(r3)
    stb   r4, (FSC_USED+1)(r3)
    addi  r3, r3, FSC_DIR
    li    r4, (256-FSC_DIR)
    mtctr r4
fsi_zero:
    li    r0, 0
    stb   r0, 0(r3)
    addi  r3, r3, 1
    bdnz  fsi_zero
    LA    r3, seed_readme_name
    LA    r4, seed_readme
    li    r5, (seed_readme_end - seed_readme)
    bl    fs_save
    LA    r3, seed_hello_name
    LA    r4, seed_hello
    li    r5, (seed_hello_end - seed_hello)
    bl    fs_save
fsi_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

# ---- Files browser ---------------------------------------------------------
files_init:
    li    r3, 0
    STWA  r3, fl_sel, r4
    STWA  r3, fl_view, r4
    blr

files_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    stw   r14, 8(r1)
    LWZA  r14, v_pade
    LWZA  r3, fl_view
    cmpwi r3, 0
    bne   flu_view
    andi. r0, r14, PAD_U
    beq   flu_d
    LWZA  r3, fl_sel
    cmpwi r3, 0
    beq   flu_d
    subi  r3, r3, 1
    STWA  r3, fl_sel, r4
    li    r3, 1
    STWA  r3, v_dirty, r4
flu_d:
    andi. r0, r14, PAD_D
    beq   flu_a
    bl    fs_count
    mr    r5, r3
    LWZA  r3, fl_sel
    addi  r3, r3, 1
    cmpw  r3, r5
    bge   flu_a
    STWA  r3, fl_sel, r4
    li    r3, 1
    STWA  r3, v_dirty, r4
flu_a:
    andi. r0, r14, PAD_A
    beq   flu_done
    bl    fs_count
    cmpwi r3, 0
    beq   flu_done
    li    r3, 1
    STWA  r3, fl_view, r4
    STWA  r3, v_dirty, r4
    b     flu_done
flu_view:
    andi. r0, r14, PAD_A
    beq   flu_done
    li    r3, 0
    STWA  r3, fl_view, r4
    li    r3, 1
    STWA  r3, v_dirty, r4
flu_done:
    lwz   r14, 8(r1)
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

files_draw:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r5, t_files
    bl    draw_chrome
    LWZA  r3, fl_view
    cmpwi r3, 0
    bne   fld_view
    bl    fl_draw_list
    b     fld_ret
fld_view:
    bl    fl_draw_file
fld_ret:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

fl_draw_list:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    stw   r14, 8(r1)
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 16
    li    r4, 40
    LA    r5, s_fl_hdr
    bl    pstr
    bl    fs_count
    cmpwi r3, 0
    bne   fll_loop
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 16
    li    r4, 80
    LA    r5, s_fl_empty
    bl    pstr
    b     fll_done
fll_loop:
    li    r14, 0
fll_l:
    bl    fs_count
    cmpw  r14, r3
    bge   fll_done
    LWZA  r3, fl_sel
    cmpw  r3, r14
    bne   fll_norm
    li    r3, 0
    li    r4, 1
    bl    setfb
    b     fll_show
fll_norm:
    li    r3, 1
    li    r4, 0
    bl    setfb
fll_show:
    mr    r3, r14
    bl    fs_entry_ptr
    mr    r5, r3
    li    r3, 32
    li    r4, 80
    mulli r6, r14, 20
    add   r4, r4, r6
    bl    pstr_n12
    addi  r14, r14, 1
    b     fll_l
fll_done:
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 16
    li    r4, 440
    LA    r5, s_fl_help
    bl    pstr
    lwz   r14, 8(r1)
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

pstr_n12:                                # r3=x r4=y r5=name -> up to 12 chars.
    mflr  r0
    stwu  r1, -32(r1)
    stw   r0, 36(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    mr    r14, r3
    mr    r15, r4
    mr    r16, r5
    li    r17, 0
pn12_l:
    cmpwi r17, 12
    bge   pn12_d
    lbzx  r5, r16, r17
    cmpwi r5, 0
    beq   pn12_d
    mr    r3, r14
    mr    r4, r15
    bl    pchar
    addi  r14, r14, 8
    addi  r17, r17, 1
    b     pn12_l
pn12_d:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r0, 36(r1)
    addi  r1, r1, 32
    mtlr  r0
    blr

fl_draw_file:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, fl_sel
    LA    r4, fbuf
    bl    fs_read
    mr    r4, r3
    LA    r3, fbuf
    bl    draw_textbuf
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 16
    li    r4, 440
    LA    r5, s_fl_vback
    bl    pstr
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

draw_textbuf:                            # r3=ptr r4=len.
    mflr  r0
    stwu  r1, -48(r1)
    stw   r0, 52(r1)
    stw   r14, 8(r1)
    stw   r15, 12(r1)
    stw   r16, 16(r1)
    stw   r17, 20(r1)
    stw   r18, 24(r1)
    mr    r14, r3
    mr    r15, r4
    li    r16, 16
    li    r17, 48
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r18, 0
dtb_l:
    cmpw  r18, r15
    bge   dtb_done
    lbzx  r3, r14, r18
    cmpwi r3, 13
    beq   dtb_nl
    cmpwi r3, 10
    beq   dtb_nl
    mr    r5, r3
    mr    r3, r16
    mr    r4, r17
    bl    pchar
    addi  r16, r16, 8
    cmpwi r16, 624
    blt   dtb_nx
    li    r16, 16
    addi  r17, r17, 16
dtb_nx:
    addi  r18, r18, 1
    b     dtb_l
dtb_nl:
    li    r16, 16
    addi  r17, r17, 20
    addi  r18, r18, 1
    b     dtb_l
dtb_done:
    lwz   r14, 8(r1)
    lwz   r15, 12(r1)
    lwz   r16, 16(r1)
    lwz   r17, 20(r1)
    lwz   r18, 24(r1)
    lwz   r0, 52(r1)
    addi  r1, r1, 48
    mtlr  r0
    blr

# ---- Notepad save ----------------------------------------------------------
notepad_init:
    li    r3, 0
    STWA  r3, np_saved, r4
    blr

notepad_update:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LWZA  r3, v_pade
    andi. r0, r3, PAD_A
    beq   npu_done
    LA    r3, notes_name
    LA    r4, np_text
    li    r5, (np_text_end - np_text)
    bl    fs_save
    li    r3, 1
    STWA  r3, np_saved, r4
    li    r3, 1
    STWA  r3, v_dirty, r4
npu_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

notepad_draw:
    mflr  r0
    stwu  r1, -16(r1)
    stw   r0, 20(r1)
    LA    r5, t_notepad
    bl    draw_chrome
    LA    r3, c_notepad
    bl    draw_content
    li    r3, 1
    li    r4, 0
    bl    setfb
    li    r3, 16
    li    r4, 200
    LA    r5, s_np_hint
    bl    pstr
    LWZA  r3, np_saved
    cmpwi r3, 0
    beq   npd_done
    li    r3, 5
    li    r4, 0
    bl    setfb
    li    r3, 16
    li    r4, 220
    LA    r5, s_np_saved
    bl    pstr
npd_done:
    lwz   r0, 20(r1)
    addi  r1, r1, 16
    mtlr  r0
    blr

.align 2
s_fl_hdr:    .asciz "Files (USV1 disk):"
s_fl_empty:  .asciz "(empty)"
s_fl_help:   .asciz "U/D select   A = open"
s_fl_vback:  .asciz "A = back to list"
s_np_hint:   .asciz "A = save NOTES.TXT"
s_np_saved:  .asciz "Saved!"
seed_readme_name: .ascii "README.TXT"
                  .byte 0,0
seed_hello_name:  .ascii "HELLO.TXT"
                  .byte 0,0,0
notes_name:       .ascii "NOTES.TXT"
                  .byte 0,0,0
seed_readme:
    .ascii "UnoDOS / PowerPC Mac (big-endian).\r"
    .ascii "This file lives in the USV1 disk,\r"
    .ascii "persisted by the harness; on real\r"
    .ascii "hardware it is backed by the boot disk.\r"
seed_readme_end:
seed_hello:
    .ascii "Hello from the Power Mac! Open Notepad\r"
    .ascii "and press A to save NOTES.TXT.\r"
seed_hello_end:
np_text:
    .ascii "Saved from Notepad on\r"
    .ascii "UnoDOS / PowerPC Mac.\r"
np_text_end:
.align 2
