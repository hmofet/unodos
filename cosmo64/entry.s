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
    ldr   x1, =0x53E00000               // stack: above the image (which can carry
    mov   sp, x1                        // ~60 MB of .bss; flatten.py asserts it
                                        // ends below 0x53000000), below the
                                        // FBINFO/crash block at 0x53F00000 and
                                        // LK's DTB at 0x54000000
    // Zero the .bss: flatten.py ships the image truncated after its last real
    // byte (a 67 MB decompress hung LK at the splash) and records the absolute
    // zero-range in the Image header's res2/res3 words at image offsets 32/40.
    ldr   x1, =0x40080020
    ldp   x2, x3, [x1]                  // x2 = start, x3 = end (16-aligned)
    cmp   x2, x3
    b.hs  bss_done
bss_zero:
    stp   xzr, xzr, [x2], #16
    cmp   x2, x3
    b.lo  bss_zero
bss_done:
    bl    cpu_early_init                // vectors + CPACR BEFORE any C runs --
                                        // the compiler may emit FP anywhere
    bl    c_main                        // x0 still holds LK's DTB pointer
halt:
    wfe
    b     halt
