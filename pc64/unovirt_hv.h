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
    UNO_VX_INVALID,             /* the CPU refused the guest state           */
    UNO_VX_PREEMPT              /* the slice clock ran out (A3)              */
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

/* What A4's guest reports about itself.  Each field is a separate claim with
 * its own way of being wrong, which is why this is a struct and not a bool. */
typedef struct uno_vm_clockirq {
    unsigned long long t1, t2;   /* its clock, sampled across two slices     */
    unsigned long long msr_echo; /* what it read back from an MSR we answered */
    int irqs;                    /* times its own handler ran                */
    int redelivered;             /* did the acknowledged one come back?      */
    int exits;                   /* how many exits it took to get there      */
} uno_vm_clockirq;

/* What A5's guest and its device report between them. */
typedef struct uno_vm_virtio {
    unsigned magic;          /* the identity register, read BY the guest    */
    unsigned used_idx;       /* what the guest read back out of the used ring */
    int bytes, notifies;     /* what the device consumed, and how often     */
    int cycle_refused;       /* a self-referential chain returned, not hung */
    const char *text;        /* what came through                           */
} uno_vm_virtio;

/* How far a real kernel got.  Not a pass/fail: the interesting outcomes are
 * all partial, and `stop_reason` plus the last line it printed is what makes
 * the next step obvious. */
typedef struct uno_vm_linux {
    long loaded;             /* bytes of kernel placed in the carve         */
    int  lines, chars;       /* what it said on its serial port             */
    const char *last;        /* ...and the last line of it                  */
    int  exits;
    unsigned stop_reason;    /* the exit that ended the run                 */
    unsigned long long stop_rip;
    unsigned fault_vec, fault_err;      /* 0xFFFF when it was not a fault   */
    unsigned long long fault_addr;
    int pio, pio_n;                     /* port I/O: how much, and where    */
    int injects;                        /* interrupts handed to the guest   */
    int shell_ok;                       /* the shell READ a line and replied */
    unsigned last_port;                 /* the one it is sitting on         */
    unsigned short pio_ports[8];
} uno_vm_linux;

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

    /* A2: the same round trip, but through second-stage translation.  The
     * guest is placed in the carve at LOW guest-physical addresses and stores
     * its marker at `gpa`; the host reads it back at a completely different
     * host-physical address, which is the evidence.  *hpa receives that
     * address so the report can print the pair.  NULL until a backend has it. */
    int (*ept)(unsigned long long want, unsigned long long gpa,
               unsigned long long *got, unsigned long long *hpa,
               uno_vmexit *last);

    /* A3: place a guest that never yields, then run it one budget at a time.
     * `slice` returns 0 when no such guest is placed.  Together these are how
     * a guest is scheduled on an OS with no scheduler: the frame loop hands
     * it a slice and the machine takes it back. */
    int (*spin_start)(void);
    int (*slice)(unsigned budget_us, uno_vmexit *out);

    /* A4: the three things a guest needs before it can be an operating
     * system - a clock it can read, an interrupt it can take, and an MSR
     * space somebody answers.  One guest exercises all three. */
    int (*clockirq)(struct uno_vm_clockirq *out);

    /* A5: an MMIO device the guest discovers and talks to through a
     * virtqueue.  This is the mechanism every later device is built on. */
    int (*virtio)(struct uno_vm_virtio *out);

    /* A6: load a bzImage into the carve, hand it a zero page, and run it. */
    int (*linux_boot)(struct uno_vm_linux *out);

    /* A6b: one budgeted slice of the kernel, from the frame loop.  A boot
     * needs seconds and a selftest cannot give them; this is where a guest
     * actually lives.  Returns 0 once the kernel has stopped. */
    int (*linux_slice)(unsigned budget_us, struct uno_vm_linux *out);
} uno_hv_t;

const uno_hv_t *uno_hv_svm(void);         /* hv_svm.c                        */
const uno_hv_t *uno_hv_vmx(void);         /* hv_vmx.c                        */

#endif /* UNO_VIRT_HV_H */
