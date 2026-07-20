// ============================================================================
// UnoDOS / Raspberry Pi (AArch64) — "USV1" byte-heap mini-FS, the AArch64
// re-expression of c64/fs.i (itself the flat-RAM analogue of apple2/fs.i and
// genesis/sram.i). Storage is a fixed 4KB RAM region (FS_BASE) that the harness
// persists across runs via --storage <file>; on real hardware this region would
// be backed by an SD sector range (that driver is the documented hardware tail).
//
// Layout (little-endian; fields kept word/hword-ALIGNED because the kernel runs
// with the MMU off, where unaligned accesses fault):
//   FS_BASE+0..3    magic "USV1"
//   FS_BASE+4       file count (word, 0..FS_MAXF=15)
//   FS_BASE+8       heap_used (word)
//   FS_BASE+12..15  reserved
//   FS_BASE+16..255 15 directory entries x 16 bytes:
//       0..11   name (NUL-padded, 12 bytes)
//       12..13  size in bytes (hword)
//       14..15  start = byte offset into the heap (hword)
//   FS_HEAP = FS_BASE+256: contiguous file data, heap grows upward (FS_HEAPSZ).
//
// fs_save overwrites by delete-then-append; fs_delete compacts the heap and
// fixes every later entry's start — the same shape as every USV1 port.
// fs_init formats + seeds README.TXT/HELLO.TXT only when the magic is absent.
// ============================================================================

// fs_init: mount the store; format + seed only if "USV1" is absent.
fs_init:
    stp   x29, x30, [sp, #-16]!
    ldr   x0, =FS_BASE
    ldr   w1, [x0]
    ldr   w2, =0x31565355                 // "USV1" (LE)
    cmp   w1, w2
    b.eq  fsi_done
    // format: magic, count=0, used=0, zero the directory region
    str   w2, [x0]
    str   wzr, [x0, #4]
    str   wzr, [x0, #8]
    str   wzr, [x0, #12]
    add   x1, x0, #16
    mov   w2, #60                         // 240 bytes = 60 words
fsi_zero:
    str   wzr, [x1], #4
    subs  w2, w2, #1
    b.ne  fsi_zero
    // seed README.TXT + HELLO.TXT
    ldr   x0, =fn_readme
    ldr   x1, =seed_readme
    mov   w2, #(seed_readme_end - seed_readme)
    bl    fs_save
    ldr   x0, =fn_hello
    ldr   x1, =seed_hello
    mov   w2, #(seed_hello_end - seed_hello)
    bl    fs_save
fsi_done:
    ldp   x29, x30, [sp], #16
    ret

// fs_find: x0 = 12-byte NUL-padded name -> w0 = index, or -1 not found. Leaf.
fs_find:
    ldr   x3, =FS_BASE
    ldr   w4, [x3, #4]                    // count
    mov   w5, #0
ff_ent:
    cmp   w5, w4
    b.hs  ff_no
    add   x6, x3, #16
    add   x6, x6, w5, uxtw #4             // entry ptr
    mov   w7, #0
ff_cmp:
    ldrb  w8, [x6, w7, uxtw]
    ldrb  w9, [x0, w7, uxtw]
    cmp   w8, w9
    b.ne  ff_next
    add   w7, w7, #1
    cmp   w7, #12
    b.ne  ff_cmp
    mov   w0, w5                          // all 12 matched
    ret
ff_next:
    add   w5, w5, #1
    b     ff_ent
ff_no:
    mov   w0, #-1
    ret

// fs_read: w0 = index, x1 = dest, w2 = dest capacity -> w0 = bytes copied
// (size clamped to the cap, so a crafted size field can't overrun). Leaf.
fs_read:
    ldr   x3, =FS_BASE
    add   x4, x3, #16
    add   x4, x4, w0, uxtw #4             // entry ptr
    ldrh  w5, [x4, #12]                   // size
    ldrh  w6, [x4, #14]                   // start
    cmp   w5, w2
    csel  w5, w2, w5, hi                  // clamp to cap
    ldr   x7, =FS_HEAP
    add   x7, x7, w6, uxtw                // src
    mov   w8, w5
fsr_cp:
    cbz   w8, fsr_done
    ldrb  w9, [x7], #1
    strb  w9, [x1], #1
    sub   w8, w8, #1
    b     fsr_cp
fsr_done:
    mov   w0, w5
    ret

// fs_delete: w0 = index. Compact the heap (move bytes after the victim down by
// its size, fix later entries' start), then drop the directory slot. Leaf.
fs_delete:
    ldr   x3, =FS_BASE
    add   x4, x3, #16
    add   x4, x4, w0, uxtw #4             // victim entry
    ldrh  w5, [x4, #12]                   // N = victim size
    ldrh  w6, [x4, #14]                   // S = victim start
    // memmove heap[S] <- heap[S+N], len = used - S - N
    ldr   w7, [x3, #8]                    // heap_used
    sub   w8, w7, w6
    sub   w8, w8, w5                      // len
    ldr   x9, =FS_HEAP
    add   x9, x9, w6, uxtw                // dst
    add   x10, x9, w5, uxtw               // src
fsd_mv:
    cbz   w8, fsd_mvd
    ldrb  w11, [x10], #1
    strb  w11, [x9], #1
    sub   w8, w8, #1
    b     fsd_mv
fsd_mvd:
    sub   w7, w7, w5
    str   w7, [x3, #8]                    // used -= N
    // fix start of every entry whose start > S
    ldr   w12, [x3, #4]                   // count
    mov   w13, #0
fsd_fix:
    cmp   w13, w12
    b.hs  fsd_shift
    add   x14, x3, #16
    add   x14, x14, w13, uxtw #4
    ldrh  w15, [x14, #14]
    cmp   w15, w6
    b.ls  fsd_fn                          // <= S (== is the victim itself)
    sub   w15, w15, w5
    strh  w15, [x14, #14]
fsd_fn:
    add   w13, w13, #1
    b     fsd_fix
    // drop the slot: shift entries idx+1.. down by one (16 bytes each)
fsd_shift:
    mov   w13, w0                         // j = idx
fsd_sh:
    add   w14, w13, #1
    cmp   w14, w12
    b.hs  fsd_cnt                         // no more entries to pull down
    add   x9, x3, #16
    add   x9, x9, w14, uxtw #4            // src = entry j+1
    add   x10, x3, #16
    add   x10, x10, w13, uxtw #4          // dst = entry j
    ldp   x11, x15, [x9]
    stp   x11, x15, [x10]
    add   w13, w13, #1
    b     fsd_sh
fsd_cnt:
    sub   w12, w12, #1
    str   w12, [x3, #4]                   // count--
    ret

// fs_save: x0 = name, x1 = data, w2 = size -> w0 = 0 ok / -1 full.
// Delete-then-append (an overwritten file moves to the end of the listing).
fs_save:
    stp   x29, x30, [sp, #-16]!
    stp   x19, x20, [sp, #-16]!
    stp   x21, x22, [sp, #-16]!
    mov   x19, x0                         // name
    mov   x20, x1                         // data
    mov   w21, w2                         // size
    bl    fs_find                         // (x0 = name still)
    cmn   w0, #1
    b.eq  fsv_fresh
    bl    fs_delete
fsv_fresh:
    ldr   x3, =FS_BASE
    ldr   w4, [x3, #4]                    // count
    cmp   w4, #FS_MAXF
    b.hs  fsv_full
    ldr   w5, [x3, #8]                    // heap_used
    add   w6, w5, w21
    mov   w7, #FS_HEAPSZ
    cmp   w6, w7
    b.hi  fsv_full
    // new entry = slot[count]
    add   x7, x3, #16
    add   x7, x7, w4, uxtw #4
    mov   w8, #0
fsv_nm:
    ldrb  w9, [x19, w8, uxtw]
    strb  w9, [x7, w8, uxtw]
    add   w8, w8, #1
    cmp   w8, #12
    b.ne  fsv_nm
    strh  w21, [x7, #12]                  // size
    strh  w5, [x7, #14]                   // start = current used
    // copy data into the heap
    ldr   x9, =FS_HEAP
    add   x9, x9, w5, uxtw
    mov   w8, w21
    mov   x10, x20
fsv_cp:
    cbz   w8, fsv_cpd
    ldrb  w11, [x10], #1
    strb  w11, [x9], #1
    sub   w8, w8, #1
    b     fsv_cp
fsv_cpd:
    str   w6, [x3, #8]                    // used += size
    add   w4, w4, #1
    str   w4, [x3, #4]                    // count++
    mov   w0, #0
    b     fsv_ret
fsv_full:
    mov   w0, #-1
fsv_ret:
    ldp   x21, x22, [sp], #16
    ldp   x19, x20, [sp], #16
    ldp   x29, x30, [sp], #16
    ret

.section .rodata
fn_readme:  .ascii "README.TXT"
            .byte 0,0
fn_hello:   .ascii "HELLO.TXT"
            .byte 0,0,0
fn_note:    .ascii "NOTE.TXT"
            .byte 0,0,0,0
seed_readme:
    .ascii "UnoDOS/rpi - the USV1 mini-FS. Notepad saves NOTE.TXT here."
seed_readme_end:
seed_hello:
    .ascii "Hello from the Raspberry Pi!"
seed_hello_end:
.align 2
.section .text
