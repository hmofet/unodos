// ============================================================================
// UnoDOS / PinePhone (AArch64) — USV1 mini-FS, the c64/fs.i byte-heap catalog
// with a byte-identical layout over a 4 KB DRAM region (FSBASE). The harness
// persists the region across runs via a --storage sidecar; on real hardware it
// would be backed by eMMC/SD (a future driver — the FS logic, Files and Notepad
// are exercised and persisted exactly as on the other ports).
//
// Layout (little-endian, see c64/fs.i):
//   +0..3   magic "USV1"      +4 file count      +5..6 heap_used (u16)
//   +16..255  15 dir entries x 16: name(12, NUL-padded) size(u16@12) start(u16@14)
//   +256..    data heap (3840 bytes), grows upward; delete compacts.
// u16 fields are byte-accessed (the +5 field is unaligned; the payload may run
// with the MMU off on real hardware, where unaligned loads fault).
// ============================================================================

// fs_entry: w0 = index -> x0 = &dir[idx]. Leaf; clobbers x1.
fs_entry:
    ldr   x1, =(FSBASE+FSC_DIR)
    add   x0, x1, w0, uxtw #4
    ret

// ld16: x0 = addr -> w0 = LE u16 (byte access). Leaf; clobbers w1.
ld16:
    ldrb  w1, [x0, #1]
    ldrb  w0, [x0]
    orr   w0, w0, w1, lsl #8
    ret

// st16: x0 = addr, w1 = value (byte access). Leaf; clobbers w2.
st16:
    strb  w1, [x0]
    lsr   w2, w1, #8
    strb  w2, [x0, #1]
    ret

// fs_init: validate the store; format + seed README.TXT/HELLO.TXT only when
// the USV1 magic is absent (first boot / unformatted store).
fs_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =FSBASE
    ldr   w1, [x0]
    ldr   w2, =0x31565355                 // "USV1" little-endian
    cmp   w1, w2
    b.eq  fsi_done
    str   w2, [x0]
    strb  wzr, [x0, #FSC_COUNT]
    strb  wzr, [x0, #FSC_USED]
    strb  wzr, [x0, #FSC_USED+1]
    add   x1, x0, #FSC_DIR                // zero reserved + directory (16..255)
    mov   w3, #240
fsi_z:
    strb  wzr, [x1], #1
    subs  w3, w3, #1
    b.ne  fsi_z
    ldr   x0, =fs_n_readme
    ldr   x1, =fs_d_readme
    ldr   w2, =fs_readme_len
    bl    fs_save
    ldr   x0, =fs_n_hello
    ldr   x1, =fs_d_hello
    ldr   w2, =fs_hello_len
    bl    fs_save
fsi_done:
    ldp   x29, x30, [sp], #16
    ret

// fs_find: x0 = 12-byte NUL-padded name -> w0 = index, or -1 if not found.
fs_find:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   x19, x0
    mov   w20, #0
    ldr   x21, =FSBASE
ffd_loop:
    ldrb  w0, [x21, #FSC_COUNT]
    cmp   w20, w0
    b.hs  ffd_no
    mov   w0, w20
    bl    fs_entry
    mov   w2, #0
ffd_cmp:
    ldrb  w3, [x0, w2, uxtw]
    ldrb  w4, [x19, w2, uxtw]
    cmp   w3, w4
    b.ne  ffd_next
    add   w2, w2, #1
    cmp   w2, #12
    b.ne  ffd_cmp
    mov   w0, w20                         // all 12 matched
    b     ffd_ret
ffd_next:
    add   w20, w20, #1
    b     ffd_loop
ffd_no:
    mov   w0, #-1
ffd_ret:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// fs_read: w0 = index, x1 = dest, w2 = dest capacity -> w0 = bytes copied
// (the catalog size clamped to the caller's buffer, as on every port).
fs_read:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   x19, x1
    mov   w20, w2
    bl    fs_entry
    mov   x21, x0
    add   x0, x21, #FSE_SIZE
    bl    ld16
    mov   w22, w0                         // size
    add   x0, x21, #FSE_START
    bl    ld16                            // start
    ldr   x1, =FSHEAP
    add   x1, x1, w0, uxtw
    cmp   w22, w20
    csel  w22, w20, w22, hi               // clamp to capacity
    mov   w2, #0
frd_cp:
    cmp   w2, w22
    b.hs  frd_d
    ldrb  w3, [x1, w2, uxtw]
    strb  w3, [x19, w2, uxtw]
    add   w2, w2, #1
    b     frd_cp
frd_d:
    mov   w0, w22
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// fs_save: x0 = name, x1 = data, w2 = length -> w0 = 0 ok / -1 full.
// Delete-then-append (an edited file moves to the end of the listing).
fs_save:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    mov   x19, x0
    mov   x20, x1
    mov   w21, w2
    mov   x0, x19
    bl    fs_find
    cmn   w0, #1
    b.eq  fsv_fresh
    bl    fs_delete
fsv_fresh:
    ldr   x22, =FSBASE
    ldrb  w0, [x22, #FSC_COUNT]           // directory full?
    cmp   w0, #FS_MAXFILES
    b.hs  fsv_full
    ldr   x0, =(FSBASE+FSC_USED)          // heap full?
    bl    ld16
    mov   w23, w0
    add   w1, w23, w21
    ldr   w2, =HEAPSIZE
    cmp   w1, w2
    b.hi  fsv_full
    ldrb  w0, [x22, #FSC_COUNT]
    bl    fs_entry
    mov   x24, x0
    mov   w2, #0                          // name (12 bytes)
fsv_nm:
    ldrb  w3, [x19, w2, uxtw]
    strb  w3, [x24, w2, uxtw]
    add   w2, w2, #1
    cmp   w2, #12
    b.ne  fsv_nm
    add   x0, x24, #FSE_SIZE
    mov   w1, w21
    bl    st16
    add   x0, x24, #FSE_START             // start = heap_used
    mov   w1, w23
    bl    st16
    ldr   x1, =FSHEAP                     // copy data
    add   x1, x1, w23, uxtw
    mov   w2, #0
fsv_cp:
    cmp   w2, w21
    b.hs  fsv_cd
    ldrb  w3, [x20, w2, uxtw]
    strb  w3, [x1, w2, uxtw]
    add   w2, w2, #1
    b     fsv_cp
fsv_cd:
    add   w1, w23, w21                    // heap_used += len
    ldr   x0, =(FSBASE+FSC_USED)
    bl    st16
    ldrb  w0, [x22, #FSC_COUNT]           // count++
    add   w0, w0, #1
    strb  w0, [x22, #FSC_COUNT]
    mov   w0, #0
    b     fsv_ret
fsv_full:
    mov   w0, #-1
fsv_ret:
    ldp   x23, x24, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// fs_delete: w0 = index. Compact the heap (move later bytes down, fix every
// later entry's start), then drop the directory slot.
fs_delete:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    stp   x23, x24, [sp, #-16]!
    mov   w19, w0
    bl    fs_entry
    mov   x24, x0
    add   x0, x24, #FSE_SIZE
    bl    ld16
    mov   w20, w0                         // N = victim size
    add   x0, x24, #FSE_START
    bl    ld16
    mov   w21, w0                         // S = victim start
    ldr   x0, =(FSBASE+FSC_USED)
    bl    ld16
    mov   w22, w0                         // heap_used
    ldr   x23, =FSHEAP                    // memmove heap[S+N..used) -> heap[S)
    add   x1, x23, w21, uxtw
    add   x2, x1, w20, uxtw
    sub   w3, w22, w21
    sub   w3, w3, w20
    mov   w4, #0
fdl_mv:
    cmp   w4, w3
    b.hs  fdl_mvd
    ldrb  w5, [x2, w4, uxtw]
    strb  w5, [x1, w4, uxtw]
    add   w4, w4, #1
    b     fdl_mv
fdl_mvd:
    sub   w1, w22, w20                    // heap_used -= N
    ldr   x0, =(FSBASE+FSC_USED)
    bl    st16
    mov   w22, #0                         // fix start of entries with start > S
fdl_fix:
    ldr   x0, =FSBASE
    ldrb  w0, [x0, #FSC_COUNT]
    cmp   w22, w0
    b.hs  fdl_dir
    mov   w0, w22
    bl    fs_entry
    mov   x24, x0
    add   x0, x24, #FSE_START
    bl    ld16
    cmp   w0, w21
    b.ls  fdl_next                        // <= S (equal = the victim itself)
    sub   w1, w0, w20
    add   x0, x24, #FSE_START
    bl    st16
fdl_next:
    add   w22, w22, #1
    b     fdl_fix
fdl_dir:                                  // shift entries idx+1.. down one slot
    mov   w22, w19
fdl_sh:
    add   w2, w22, #1
    ldr   x0, =FSBASE
    ldrb  w0, [x0, #FSC_COUNT]
    cmp   w2, w0
    b.hs  fdl_cnt
    mov   w0, w2
    bl    fs_entry
    mov   x24, x0                         // src = entry i+1 (fs_entry clobbers x1)
    mov   w0, w22
    bl    fs_entry                        // dst = entry i
    mov   x1, x24
    mov   w2, #0
fdl_cp:
    ldrb  w3, [x1, w2, uxtw]
    strb  w3, [x0, w2, uxtw]
    add   w2, w2, #1
    cmp   w2, #16
    b.ne  fdl_cp
    add   w22, w22, #1
    b     fdl_sh
fdl_cnt:
    ldr   x0, =FSBASE
    ldrb  w1, [x0, #FSC_COUNT]
    sub   w1, w1, #1
    strb  w1, [x0, #FSC_COUNT]
    ldp   x23, x24, [sp], #16
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

// ---------------------------------------------------------------- seed data
.section .rodata
fs_n_readme: .ascii "README.TXT\0\0"
fs_n_hello:  .ascii "HELLO.TXT\0\0\0"
fs_n_note:   .ascii "NOTE.TXT\0\0\0\0"
fs_d_readme:
    .ascii "UnoDOS / PinePhone - this file\r"
    .ascii "lives in the USV1 mini-FS,\r"
    .ascii "persisted across boots by the\r"
    .ascii "storage sidecar.\r"
fs_d_readme_e:
.equ fs_readme_len, fs_d_readme_e - fs_d_readme
fs_d_hello:
    .ascii "Hello from the PinePhone!\r"
fs_d_hello_e:
.equ fs_hello_len, fs_d_hello_e - fs_d_hello
np_seed:
    .asciz "New note. Press A to append\ra mark and save to NOTE.TXT.\r"
.section .text
