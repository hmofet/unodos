// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — USV1 byte-heap filesystem (ported from
// c64/fs.i, byte-identical on-medium layout) + the interactive Files browser
// and Notepad save. Storage is a 4 KB region the harness persists across runs
// (FSBASE..FSBASE+0xFFF); on real hardware that region is loaded from / flushed
// to the SD card by the EMMC/SDHCI driver — the documented hardware tail. The FS
// logic, Files (list + view) and Notepad save are fully exercised and persisted
// in the harness exactly as the C64/Genesis/Apple II ports do.
//
// Layout (little-endian): +0..3 "USV1", +4 count, +5..6 heap_used,
//   +16..255 15 dir entries x16 (name[12], size:hw, start:hw); FSHEAP=+256.
// ============================================================================

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

fs_entry_ptr:                            // w0=idx -> x0 = entry ptr. Leaf.
    ldr   x1, =FSBASE
    add   x1, x1, #FSC_DIR
    add   x0, x1, w0, uxtw #4
    ret

fs_count:                                // -> w0. Leaf.
    ldr   x1, =FSBASE
    ldrb  w0, [x1, #FSC_COUNT]
    ret

fs_get_used:                             // -> w0. Leaf.
    ldr   x1, =FSBASE
    ldrb  w0, [x1, #FSC_USED]
    ldrb  w2, [x1, #(FSC_USED+1)]
    orr   w0, w0, w2, lsl #8
    ret

fs_set_used:                             // w0 = used. Leaf.
    ldr   x1, =FSBASE
    strb  w0, [x1, #FSC_USED]
    lsr   w2, w0, #8
    strb  w2, [x1, #(FSC_USED+1)]
    ret

// fs_find: x0 = 12-byte name -> w0 = index or -1.
fs_find:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   x19, x0
    bl    fs_count
    mov   w20, w0
    mov   w21, #0
ffl:
    cmp   w21, w20
    b.hs  ff_no
    mov   w0, w21
    bl    fs_entry_ptr
    mov   x22, x0
    mov   w0, #0
ffc:
    cmp   w0, #12
    b.hs  ff_match
    ldrb  w1, [x22, w0, uxtw]
    ldrb  w2, [x19, w0, uxtw]
    cmp   w1, w2
    b.ne  ff_next
    add   w0, w0, #1
    b     ffc
ff_match:
    mov   w0, w21
    b     ff_out
ff_next:
    add   w21, w21, #1
    b     ffl
ff_no:
    mov   w0, #-1
ff_out:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// fs_read: w0 = index, x1 = dest -> copies bytes, returns w0 = size.
fs_read:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   x20, x1
    bl    fs_entry_ptr
    ldrh  w1, [x0, #FSE_SIZE]
    ldrh  w2, [x0, #FSE_START]
    ldr   x3, =FSHEAP
    add   x3, x3, w2, uxtw
    mov   w19, w1
fsr_cp:
    cbz   w1, fsr_done
    ldrb  w4, [x3], #1
    strb  w4, [x20], #1
    sub   w1, w1, #1
    b     fsr_cp
fsr_done:
    mov   w0, w19
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// fs_delete: w0 = index. Compact heap, fix later starts, drop the slot.
fs_delete:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    mov   w19, w0
    bl    fs_entry_ptr
    mov   x20, x0
    ldrh  w21, [x20, #FSE_SIZE]           // N
    ldrh  w22, [x20, #FSE_START]          // S
    bl    fs_get_used
    mov   w23, w0                         // used
    sub   w24, w23, w22
    sub   w24, w24, w21                   // len = used - S - N
    ldr   x0, =FSHEAP
    add   x1, x0, w22, uxtw               // dst = heap + S
    add   x2, x1, w21, uxtw               // src = dst + N
fd_cp:
    cbz   w24, fd_cpd
    ldrb  w3, [x2], #1
    strb  w3, [x1], #1
    sub   w24, w24, #1
    b     fd_cp
fd_cpd:
    sub   w0, w23, w21
    bl    fs_set_used
    mov   w23, #0                         // fix starts: i
fd_fix:
    bl    fs_count
    cmp   w23, w0
    b.hs  fd_drop
    mov   w0, w23
    bl    fs_entry_ptr
    mov   x1, x0
    ldrh  w2, [x1, #FSE_START]
    cmp   w2, w22
    b.ls  fd_fixn                         // start <= S: skip
    sub   w2, w2, w21
    strh  w2, [x1, #FSE_START]
fd_fixn:
    add   w23, w23, #1
    b     fd_fix
fd_drop:
    mov   w23, w19                        // shift entries idx+1.. down
fd_sh:
    add   w24, w23, #1
    bl    fs_count
    cmp   w24, w0
    b.hs  fd_cnt
    mov   w0, w24
    bl    fs_entry_ptr
    mov   x3, x0                          // src
    mov   w0, w23
    bl    fs_entry_ptr
    mov   x4, x0                          // dst
    mov   w0, #0
fd_ce:
    cmp   w0, #FSE_LEN
    b.hs  fd_cen
    ldrb  w5, [x3, w0, uxtw]
    strb  w5, [x4, w0, uxtw]
    add   w0, w0, #1
    b     fd_ce
fd_cen:
    add   w23, w23, #1
    b     fd_sh
fd_cnt:
    ldr   x0, =FSBASE
    ldrb  w1, [x0, #FSC_COUNT]
    sub   w1, w1, #1
    strb  w1, [x0, #FSC_COUNT]
    ldp   x23, x24, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// fs_save: x0 = name, x1 = data, w2 = size -> w0 = 0 ok / -1 full. Overwrite by
// delete-then-append.
fs_save:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    mov   x19, x0                         // name
    mov   x20, x1                         // data
    mov   w21, w2                         // size
    mov   x0, x19
    bl    fs_find
    cmp   w0, #0
    b.lt  fsv_fresh
    bl    fs_delete
fsv_fresh:
    bl    fs_count
    cmp   w0, #FS_MAXFILES
    b.lt  fsv_room
    mov   w0, #-1
    b     fsv_out
fsv_room:
    bl    fs_get_used
    add   w1, w0, w21
    cmp   w1, #FS_HEAPSIZE
    b.le  fsv_fits
    mov   w0, #-1
    b     fsv_out
fsv_fits:
    bl    fs_count
    mov   w22, w0                         // new index
    mov   w0, w22
    bl    fs_entry_ptr
    mov   x23, x0                         // entry ptr
    mov   w0, #0
fsv_nm:
    cmp   w0, #12
    b.hs  fsv_nmd
    ldrb  w1, [x19, w0, uxtw]
    strb  w1, [x23, w0, uxtw]
    add   w0, w0, #1
    b     fsv_nm
fsv_nmd:
    strh  w21, [x23, #FSE_SIZE]
    bl    fs_get_used
    mov   w24, w0                         // start = used
    strh  w24, [x23, #FSE_START]
    ldr   x0, =FSHEAP
    add   x0, x0, w24, uxtw               // dst
    mov   w1, #0
fsv_cp:
    cmp   w1, w21
    b.hs  fsv_cpd
    ldrb  w2, [x20, w1, uxtw]
    strb  w2, [x0, w1, uxtw]
    add   w1, w1, #1
    b     fsv_cp
fsv_cpd:
    bl    fs_get_used
    add   w0, w0, w21
    bl    fs_set_used
    ldr   x0, =FSBASE
    ldrb  w1, [x0, #FSC_COUNT]
    add   w1, w1, #1
    strb  w1, [x0, #FSC_COUNT]
    mov   w0, #0
fsv_out:
    ldp   x23, x24, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// fs_init: keep a valid "USV1" store; otherwise format + seed two files.
fs_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =FSBASE
    ldrb  w1, [x0, #0]
    cmp   w1, #'U'
    b.ne  fsi_format
    ldrb  w1, [x0, #1]
    cmp   w1, #'S'
    b.ne  fsi_format
    ldrb  w1, [x0, #2]
    cmp   w1, #'V'
    b.ne  fsi_format
    ldrb  w1, [x0, #3]
    cmp   w1, #'1'
    b.eq  fsi_done
fsi_format:
    ldr   x0, =FSBASE
    mov   w1, #'U'
    strb  w1, [x0, #0]
    mov   w1, #'S'
    strb  w1, [x0, #1]
    mov   w1, #'V'
    strb  w1, [x0, #2]
    mov   w1, #'1'
    strb  w1, [x0, #3]
    strb  wzr, [x0, #FSC_COUNT]
    strb  wzr, [x0, #FSC_USED]
    strb  wzr, [x0, #(FSC_USED+1)]
    // zero the directory region 16..255
    add   x0, x0, #FSC_DIR
    mov   w1, #(256-FSC_DIR)
fsi_zero:
    strb  wzr, [x0], #1
    subs  w1, w1, #1
    b.ne  fsi_zero
    // seed README.TXT + HELLO.TXT
    ldr   x0, =seed_readme_name
    ldr   x1, =seed_readme
    mov   w2, #(seed_readme_end - seed_readme)
    bl    fs_save
    ldr   x0, =seed_hello_name
    ldr   x1, =seed_hello
    mov   w2, #(seed_hello_end - seed_hello)
    bl    fs_save
fsi_done:
    ldp   x29, x30, [sp], #16
    ret

// ---- Files browser ---------------------------------------------------------
files_init:
    ldr   x0, =fl_sel
    str   wzr, [x0]
    ldr   x0, =fl_view
    str   wzr, [x0]
    ret

files_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pade
    ldr   w4, [x0]
    ldr   x0, =fl_view
    ldr   w0, [x0]
    cbnz  w0, flu_view
    // list mode: U/D move, A open
    tst   w4, #PAD_U
    b.eq  flu_d
    ldr   x0, =fl_sel
    ldr   w1, [x0]
    cbz   w1, flu_d
    sub   w1, w1, #1
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
flu_d:
    tst   w4, #PAD_D
    b.eq  flu_a
    bl    fs_count
    mov   w2, w0
    ldr   x0, =fl_sel
    ldr   w1, [x0]
    add   w1, w1, #1
    cmp   w1, w2
    b.hs  flu_a
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
flu_a:
    tst   w4, #PAD_A
    b.eq  flu_done
    bl    fs_count
    cbz   w0, flu_done                    // no files
    ldr   x0, =fl_view
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
    b     flu_done
flu_view:
    // view mode: A returns to list
    tst   w4, #PAD_A
    b.eq  flu_done
    ldr   x0, =fl_view
    str   wzr, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
flu_done:
    ldp   x29, x30, [sp], #16
    ret

files_draw:
    stp   x29, x30, [sp, #-16]!
    ldr   x2, =t_files
    bl    draw_chrome
    ldr   x0, =fl_view
    ldr   w0, [x0]
    cbnz  w0, fld_view
    bl    fl_draw_list
    ldp   x29, x30, [sp], #16
    ret
fld_view:
    bl    fl_draw_file
    ldp   x29, x30, [sp], #16
    ret

fl_draw_list:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #40
    ldr   x2, =s_fl_hdr
    bl    pstr
    bl    fs_count
    cbnz  w0, fll_loop
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #80
    ldr   x2, =s_fl_empty
    bl    pstr
    b     fll_done
fll_loop:
    mov   w19, #0
fll_l:
    bl    fs_count
    cmp   w19, w0
    b.hs  fll_done
    ldr   x0, =fl_sel                     // selected -> inverted
    ldr   w0, [x0]
    cmp   w0, w19
    b.ne  fll_norm
    mov   w0, #0
    mov   w1, #1
    bl    setfb
    b     fll_show
fll_norm:
    mov   w0, #1
    mov   w1, #0
    bl    setfb
fll_show:
    mov   w0, w19
    bl    fs_entry_ptr                    // x0 = entry = name ptr
    mov   x2, x0
    mov   w0, #32
    mov   w1, #80
    mov   w3, #20
    mul   w3, w19, w3
    add   w1, w1, w3
    bl    pstr_n12                        // draw up to 12 name chars
    add   w19, w19, #1
    b     fll_l
fll_done:
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #440
    ldr   x2, =s_fl_help
    bl    pstr
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// pstr_n12: x2 = name ptr -> draw up to 12 chars (stop at NUL). w0=x w1=y in.
pstr_n12:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   w19, w0
    mov   w20, w1
    mov   x21, x2
    mov   w22, #0
pn12_l:
    cmp   w22, #12
    b.hs  pn12_d
    ldrb  w2, [x21, w22, uxtw]
    cbz   w2, pn12_d
    mov   w0, w19
    mov   w1, w20
    bl    pchar
    add   w19, w19, #8
    add   w22, w22, #1
    b     pn12_l
pn12_d:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

fl_draw_file:
    stp   x29, x30, [sp, #-16]!
    // read selected file into fbuf
    ldr   x0, =fl_sel
    ldr   w0, [x0]
    ldr   x1, =fbuf
    bl    fs_read                         // w0 = size
    ldr   x1, =fbuf
    mov   w2, w0
    mov   x0, x1
    mov   w1, w2
    bl    draw_textbuf
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #440
    ldr   x2, =s_fl_vback
    bl    pstr
    ldp   x29, x30, [sp], #16
    ret

// draw_textbuf: x0 = ptr, w1 = len. 8px font, newlines on CR/LF, wraps at 624.
draw_textbuf:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    mov   x19, x0
    mov   w20, w1
    mov   w21, #16
    mov   w22, #48
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w23, #0
dtb_l:
    cmp   w23, w20
    b.hs  dtb_done
    ldrb  w0, [x19, w23, uxtw]
    cmp   w0, #13
    b.eq  dtb_nl
    cmp   w0, #10
    b.eq  dtb_nl
    mov   w2, w0
    mov   w0, w21
    mov   w1, w22
    bl    pchar
    add   w21, w21, #8
    cmp   w21, #464
    b.lo  dtb_nx
    mov   w21, #16
    add   w22, w22, #16
dtb_nx:
    add   w23, w23, #1
    b     dtb_l
dtb_nl:
    mov   w21, #16
    add   w22, w22, #20
    add   w23, w23, #1
    b     dtb_l
dtb_done:
    ldp   x23, x24, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// ---- Notepad (save to NOTES.TXT) -------------------------------------------
notepad_init:
    ldr   x0, =np_saved
    str   wzr, [x0]
    ret

notepad_update:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =v_pade
    ldr   w0, [x0]
    tst   w0, #PAD_A
    b.eq  npu_done
    ldr   x0, =notes_name
    ldr   x1, =np_text
    mov   w2, #(np_text_end - np_text)
    bl    fs_save
    ldr   x0, =np_saved
    mov   w1, #1
    str   w1, [x0]
    ldr   x0, =v_dirty
    mov   w1, #1
    str   w1, [x0]
npu_done:
    ldp   x29, x30, [sp], #16
    ret

notepad_draw:
    stp   x29, x30, [sp, #-16]!
    ldr   x2, =t_notepad
    bl    draw_chrome
    ldr   x0, =c_notepad
    bl    draw_content
    mov   w0, #1
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #200
    ldr   x2, =s_np_hint
    bl    pstr
    ldr   x0, =np_saved
    ldr   w0, [x0]
    cbz   w0, npd_done
    mov   w0, #5
    mov   w1, #0
    bl    setfb
    mov   w0, #16
    mov   w1, #220
    ldr   x2, =s_np_saved
    bl    pstr
npd_done:
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
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
    .ascii "UnoDOS / Raspberry Pi (AArch64).\r"
    .ascii "This file lives in the USV1 disk,\r"
    .ascii "persisted by the harness; on real\r"
    .ascii "hardware it is backed by the SD card.\r"
seed_readme_end:
seed_hello:
    .ascii "Hello from the Pi! Open Notepad and\r"
    .ascii "press A to save NOTES.TXT.\r"
seed_hello_end:
np_text:
    .ascii "Saved from Notepad on\r"
    .ascii "UnoDOS / Raspberry Pi.\r"
np_text_end:
.section .text
