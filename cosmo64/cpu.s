// cosmo64/cpu.s -- ARM CPU glue for pc64-on-ARM: exception vectors with a DRAM
// crash record, whole-D-cache invalidate, and the MMU/cache enable sequence.
//
// There is no UART on this device, so a fault must leave its forensics where a
// later boot (or the harness) can read them: the CRASH RECORD at c64_dbg_page+0x1000 (in-image .bss) --
// magic 'HSRC' ("CRSH" little-endian), then vec index, ESR, ELR, FAR, CurrentEL.
// The handler also paints a red 64x64 block at the raw framebuffer origin when
// fb_init has published one (FBINFO+32), then parks. Sixteen 0x80-byte vectors,
// VBAR-aligned to 2 KB.

    .text

// ---- exception vectors ------------------------------------------------------
    .balign 2048
    .globl vector_table
vector_table:
    .irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    .balign 0x80
    mov   x18, #\n
    b     fault_record
    .endr

fault_record:
    ldr   x0, =c64_dbg_page + 0x1000
    ldr   w1, =0x43525348               // 'CRSH' read back as bytes C R S H
    str   w1, [x0]
    str   w18, [x0, #4]
    mrs   x1, esr_el1
    str   x1, [x0, #8]
    mrs   x1, elr_el1
    str   x1, [x0, #16]
    mrs   x1, far_el1
    str   x1, [x0, #24]
    mrs   x1, CurrentEL
    str   x1, [x0, #32]
    dsb   sy
    // Paint the crash VISIBLY: there is no UART and no /dev/mem afterwards, so
    // the panel is the only forensics channel that reaches a photograph. Use
    // the adopted framebuffer when fb_init published one; before that, the
    // MEASURED panel base/pitch for this unit (0x7DF70000/4352,
    // mblock-7-framebuffer, device-verified) -- diagnostic-only hardcode.
    ldr   x0, =c64_dbg_page             // FBINFO
    ldr   x1, [x0, #32]                 // fb_raw
    ldr   w2, [x0, #64]                 // fb_ppitch
    cbnz  x1, 1f
    ldr   x1, =0x7DF70000
1:  cbnz  w2, 2f
    mov   w2, #4352
2:  // red 64x64 block at the origin = "an exception was taken"
    mov   x4, x1
    mov   w3, #64
3:  mov   x5, x4
    mov   w6, #64
4:  ldr   w7, =0xFFFF0000
    str   w7, [x5], #4
    subs  w6, w6, #1
    b.ne  4b
    add   x4, x4, w2, uxtw
    subs  w3, w3, #1
    b.ne  3b
    // then the registers, one 32-bit word per row of bit-cells (white = 1,
    // dark = 0, bit 31 leftmost): ESR @ y=80, ELR low half @ y=104,
    // FAR low half @ y=128, vec index @ y=152. A photo decodes the fault.
    mrs   x3, esr_el1
    mov   w4, #80
    bl    paint_bits
    mrs   x3, elr_el1
    mov   w4, #104
    bl    paint_bits
    mrs   x3, far_el1
    mov   w4, #128
    bl    paint_bits
    mov   x3, x18
    mov   w4, #152
    bl    paint_bits
    dsb   sy
5:  wfe
    b     5b

// paint_bits: 32 bit-cells (12 px wide, 16 tall, 16 apart) of w3 at panel
// y-offset w4. x1 = panel base, w2 = pitch. Clobbers x5-x13, x30 is dead
// anyway (the handler parks). Cell 0 (leftmost) = bit 31.
paint_bits:
    umull x5, w4, w2
    add   x5, x1, x5
    mov   w6, #0
pb_cell:
    mov   w7, #31
    sub   w7, w7, w6
    lsr   w8, w3, w7
    tst   w8, #1
    ldr   w9, =0xFFFFFFFF
    b.ne  pb_go
    ldr   w9, =0xFF400040               // dark violet = bit clear
pb_go:
    lsl   w10, w6, #4                   // cell x = cell * 16
    add   x10, x5, w10, uxtw #2
    mov   w11, #16
pb_row:
    mov   x12, x10
    mov   w13, #12
pb_px:
    str   w9, [x12], #4
    subs  w13, w13, #1
    b.ne  pb_px
    add   x10, x10, w2, uxtw
    subs  w11, w11, #1
    b.ne  pb_row
    add   w6, w6, #1
    cmp   w6, #32
    b.lo  pb_cell
    ret

// ---- cpu_early_init: vectors + FPU, called before any C -------------------
    .globl cpu_early_init
cpu_early_init:
    ldr   x1, =vector_table
    msr   vbar_el1, x1
    mov   x1, #(3 << 20)                // CPACR_EL1.FPEN: no FP/SIMD traps
    msr   cpacr_el1, x1
    isb
    ret

// ---- dcache_inv_all: invalidate the entire D-cache by set/way -------------
// The standard ARM ARM walk. LK cleaned before handoff, but trusting that on
// every level of an abandoned bootloader is how ghosts get into DRAM.
// Clobbers x0-x12; leaf.
    .globl dcache_inv_all
dcache_inv_all:
    mrs   x0, clidr_el1
    and   w3, w0, #0x07000000           // LoC << 24
    lsr   w3, w3, #23                   // LoC * 2
    cbz   w3, 5f
    mov   w10, #0                       // level * 2
4:  add   w2, w10, w10, lsr #1          // level * 3
    lsr   w1, w0, w2
    and   w1, w1, #7                    // cache type at this level
    cmp   w1, #2
    b.lt  6f                            // no D-cache here
    msr   csselr_el1, x10
    isb
    mrs   x1, ccsidr_el1
    and   w2, w1, #7
    add   w2, w2, #4                    // log2(line length)
    ubfx  w4, w1, #3, #10               // ways - 1
    clz   w5, w4                        // way position
    ubfx  w7, w1, #13, #15              // sets - 1
7:  mov   w9, w4                        // way loop
8:  lsl   w11, w9, w5
    orr   w11, w10, w11
    lsl   w12, w7, w2
    orr   w11, w11, w12
    dc    isw, x11
    subs  w9, w9, #1
    b.ge  8b
    subs  w7, w7, #1
    b.ge  7b
6:  add   w10, w10, #2
    cmp   w3, w10
    b.gt  4b
5:  dsb   sy
    ic    iallu
    dsb   sy
    isb
    ret

// ---- mmu_on(ttbr0, mair, tcr): translation + caches live ------------------
    .globl mmu_on
mmu_on:
    msr   ttbr0_el1, x0
    msr   mair_el1, x1
    msr   tcr_el1, x2
    tlbi  vmalle1
    dsb   ish
    isb
    mrs   x3, sctlr_el1
    orr   x3, x3, #(1 << 0)             // M: MMU
    orr   x3, x3, #(1 << 2)             // C: D-cache
    orr   x3, x3, #(1 << 12)            // I: I-cache
    bic   x3, x3, #(1 << 1)             // A: no alignment checking on Normal
    msr   sctlr_el1, x3
    isb
    ret

// ---- __chkstk: the aarch64-w64-mingw32 stack probe --------------------------
// clang emits a call for any frame over 4 KB (size/16 arrives in x15). Probing
// exists to commit guard pages on Windows; our stack is plain, fully-mapped
// DRAM reserved by entry.s, so the probe has nothing to do.
    .globl __chkstk
__chkstk:
    ret
