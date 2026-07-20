/* Module-internal stubs for PYRT.UNO so it needn't import compiler/runtime
 * helpers.  mingw emits a stack-probe call for frames > 4 KB; the UEFI stack
 * is fully committed, so a plain ret (clobbers nothing) satisfies it - the
 * same stub the kernel's pc64_libc.c uses. */
__asm__(
    ".globl ___chkstk_ms\n"
    "___chkstk_ms:\n"
    "    ret\n"
);

/* MicroPython's shared gchelper needs a per-arch gc_helper_get_regs_and_sp
 * (gc_helper_regs_t is uintptr_t[6] on x86-64) - the vendored tree ships it
 * for the SysV ABI, not Win64/mingw, so provide it here.  Win64: the regs
 * pointer arrives in %rcx; save the callee-saved GP set the GC must trace and
 * return the caller's stack pointer in %rax. */
__asm__(
    ".globl gc_helper_get_regs_and_sp\n"
    "gc_helper_get_regs_and_sp:\n"
    "    mov %rbx,  0(%rcx)\n"
    "    mov %rbp,  8(%rcx)\n"
    "    mov %r12, 16(%rcx)\n"
    "    mov %r13, 24(%rcx)\n"
    "    mov %r14, 32(%rcx)\n"
    "    mov %r15, 40(%rcx)\n"
    "    lea 8(%rsp), %rax\n"
    "    ret\n"
);

