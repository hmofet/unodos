/* ===========================================================================
 * unovirt - the vendor backend seam.
 *
 * ARM has one virtualization architecture and x86 has two, which agree on
 * every concept and disagree on every register.  So the subsystem is split the
 * way this repo splits every such thing (uno3d's rasterisers, unoui's themes,
 * unobus' drivers): one vtable, one file per vendor, and everything above the
 * seam compiles once.
 *
 * The rule for what belongs in `uno_vmexit` is worth stating, because getting
 * it wrong is how a "portable" hypervisor ends up with `if (vmx)` in its
 * device models: the struct carries what a CONSUMER needs to act, in terms
 * that exist on both vendors.  The vendor's own exit code rides along as
 * `raw`, for reports only.
 * ======================================================================== */
#ifndef UNO_VIRT_HV_H
#define UNO_VIRT_HV_H

/* Vendor-neutral exit reasons.  The set is small on purpose: A1 needs exactly
 * these, and a reason nobody handles is UNKNOWN with `raw` to identify it. */
enum {
    UNO_VX_UNKNOWN = 0,
    UNO_VX_CPUID,               /* guest executed cpuid                      */
    UNO_VX_HLT,                 /* guest halted: our "done" signal           */
    UNO_VX_SHUTDOWN,            /* guest triple-faulted; it is over          */
    UNO_VX_INTR,                /* a host interrupt is pending               */
    UNO_VX_NPF,                 /* second-stage fault (A2)                   */
    UNO_VX_INVALID              /* the CPU refused the guest state           */
};

typedef struct uno_vmexit {
    int   reason;               /* UNO_VX_*                                  */
    unsigned long long raw;     /* the vendor's own exit code                */
    unsigned long long rip;     /* guest RIP at the exit                     */
    unsigned long long info1;   /* SVM EXITINFO1 / VMX exit qualification    */
    unsigned long long info2;   /* SVM EXITINFO2 / VMX guest linear address  */
} uno_vmexit;

/* The guest's general-purpose registers, saved and restored around every
 * entry.  A guest runs on the machine's real register file, so without this
 * it runs on OURS and hands back whatever it leaves in them - a defect Glide
 * shipped and then had to fix (docs/HYPERVISOR.md, V3c.1).  Doing it from the
 * first guest costs nothing and makes guests resumable, which trap-and-emulate
 * needs anyway.
 *
 * THE OFFSETS ARE LOAD-BEARING: the entry stub addresses these by literal
 * displacement.  The static asserts below are the only thing standing between
 * a reordered field and a guest that runs with scrambled registers. */
typedef struct uno_gprs {
    unsigned long long rax, rbx, rcx, rdx, rsi, rdi, rbp;
    unsigned long long r8, r9, r10, r11, r12, r13, r14, r15;
} uno_gprs;

_Static_assert(sizeof(unsigned long long) == 8, "gpr slots are 8 bytes");
_Static_assert(__builtin_offsetof(uno_gprs, rbx) == 0x08, "gpr layout");
_Static_assert(__builtin_offsetof(uno_gprs, rbp) == 0x30, "gpr layout");
_Static_assert(__builtin_offsetof(uno_gprs, r15) == 0x70, "gpr layout");

typedef struct uno_hv {
    const char *name;                     /* "svm" | "vmx"                   */

    /* Enter host virtualization mode on this CPU.  1 = we are in it; 0 with
     * *why set to a short reason.  Idempotent. */
    int (*enable)(const char **why);

    /* Run the A1 marker guest: it takes a value from us through a cpuid
     * intercept, writes it to its own memory, and halts.  Returns 1 when the
     * value came back out of GUEST memory, and fills *last with the final
     * exit either way. */
    int (*marker)(unsigned long long want, unsigned long long *got,
                  uno_vmexit *last);

    /* Run a guest that destroys itself on purpose.  Returns 1 when the ruin
     * was CONTAINED (the machine is still ours and the exit says why). */
    int (*crasher)(uno_vmexit *out);
} uno_hv_t;

const uno_hv_t *uno_hv_svm(void);         /* hv_svm.c                        */
const uno_hv_t *uno_hv_vmx(void);         /* hv_vmx.c                        */

#endif /* UNO_VIRT_HV_H */
