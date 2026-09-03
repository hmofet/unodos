// cosmo64/entry.s -- LK entry shim for pc64-on-ARM (Cosmo Communicator, MT6771).
//
// Planet's LK loads the flat image at 0x40080000 and branches to its first byte
// per the arm64 boot protocol: DTB pointer in x0, MMU off, one core. flatten.py
// puts a branch to this symbol at image offset 0, so the PE entry point can live
// wherever the linker put it.
//
// Everything here is the minimum between LK and C:
//   1. disable the TOPRGU watchdog -- LK armed it, and a payload that ignores it
//      reboots after ~30 s (the asm port's first hardware lesson);
//   2. park any core that is not the boot core;
//   3. set a stack and call c_main(dtb).
// Cache/MMU state is left exactly as LK handed it over (MMU off, so all memory
// is Device -- which is why every C file is compiled -mstrict-align). Bringing
// the MMU and caches up properly is an M1 job, not an entry-shim job.

    .text
    .globl _start
_start:
    ldr   x1, =0x10007000               // TOPRGU: key 0x2200 in the top half,
    ldr   w2, =0x22000000               // enable bit clear = watchdog off
    str   w2, [x1]
    mrs   x1, mpidr_el1
    and   x1, x1, #0xFFFFFF             // Aff2:Aff1:Aff0
    cbz   x1, boot_core
park:
    wfe
    b     park
boot_core:
    // Stack: inside the image's own .bss (c64_boot_stack in videolfb.c). The
    // zero loop below writes every byte of it before anything pushes a frame,
    // so its DRAM is proven writable before it is trusted -- an off-image
    // stack at 0x53E00000 was on the suspect list for a silent pre-vector
    // wedge (a fault while pushing the frame can't reach a handler usefully).
    ldr   x1, =c64_boot_stack + 0x80000
    mov   sp, x1
    // publish the debug page's address into our own header (offset 0x30, the
    // ARM64 res4 word) so the harness can find the FBINFO/crash block without
    // a fixed off-image address
    ldr   x1, =c64_dbg_page
    ldr   x2, =0x40080030
    str   x1, [x2]
    bl    cpu_early_init                // vectors FIRST: anything after this
                                        // faults into the crash record + the
                                        // painted ESR, never a silent wedge
    // Stage beacons, painted straight over LK's splash at the MEASURED panel
    // base (0x7DF70000, pitch 4352 -- mblock-7-framebuffer, device-verified).
    // Diagnostic-only hardcode: the adopt path still reads the real base from
    // the DTB, and the first thing it does is clear the panel, wiping these.
    // GREEN block = LK jumped to us and the vectors are live.
    ldr   w1, =0xFF00FF00
    mov   w2, #128
    bl    beacon_block
    // Zero the .bss: flatten.py ships the image truncated after its last real
    // byte (a 67 MB decompress trips LK's 28 MB cap) and records the absolute
    // zero-range in the Image header's res2/res3 words at image offsets 32/40.
    //
    // ONLY THE EARLY PART, if there is one. The shell carries ~100 MB of .bss
    // and the MMU is off here, so every store below is its own Device-memory
    // bus transaction: zeroing the lot cost about a second of every boot. The
    // build collects everything the boot touches before mmu_init() returns
    // (stack, fault stack, debug page, page tables, log bookkeeping) into a
    // ".early" section, and flatten.py leaves its absolute range at image
    // offset 0x50. c64_bss_zero_rest() clears the remainder the instant
    // mmu_init() returns, with the caches on, where it costs milliseconds.
    // The guarantee that made the original loop worth having is unchanged:
    // no byte of DRAM is trusted before something has written it.
    //
    // Zero at 0x50 means no such section -- an older flatten.py, or a payload
    // built without it -- and the fall-back is the whole range, as before.
    ldr   x1, =0x40080020
    ldp   x2, x3, [x1]                  // x2 = start, x3 = end (16-aligned)
    ldr   x1, =0x40080050
    ldp   x4, x5, [x1]                  // x4 = early start, x5 = early end
    cbz   x4, bss_range
    cmp   x5, x4
    b.ls  bss_range
    mov   x2, x4
    mov   x3, x5
bss_range:
    cmp   x2, x3
    b.hs  bss_done
bss_zero:
    stp   xzr, xzr, [x2], #16
    cmp   x2, x3
    b.lo  bss_zero
bss_done:
    // CYAN block = the .bss zero completed
    ldr   w1, =0xFF00FFFF
    mov   w2, #176
    bl    beacon_block
    bl    c_main                        // x0 still holds LK's DTB pointer
halt:
    wfe
    b     halt

// beacon_block: paint a 32x32 block of colour w1 at panel x = w2, y = 0,
// using the measured panel base/pitch. Preserves x0 (the DTB pointer).
// Clobbers x1-x6.
beacon_block:
    ldr   x3, =0x7DF70000
    add   x3, x3, w2, uxtw #2           // + x * 4
    mov   w4, #32                       // rows
    mov   w5, #4352                     // panel pitch (too big for an add imm)
1:  mov   x6, x3
    mov   w7, #32                       // columns
2:  str   w1, [x6], #4
    subs  w7, w7, #1
    b.ne  2b
    add   x3, x3, w5, uxtw
    subs  w4, w4, #1
    b.ne  1b
    dsb   sy
    ret
