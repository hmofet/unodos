/* ===========================================================================
 * unovirt - the vendor backend seam.  See unovirt_phase.h, pc64/UNOVIRT.md,
 * docs/UNOVIRT-PLAN.md §3.1.
 *
 * ARM has one virtualization architecture and x86 has two, which agree on
 * every concept and disagree on every register.  So the subsystem is split the
 * way this repo splits every such thing (uno3d's rasterisers, unoui's themes,
 * unobus' drivers): one vtable, one file per vendor, and everything above the
 * seam compiles once.
 *
 * WHAT THE SEAM IS FOR, AND THE MISTAKE IT ONCE MADE.  This vtable used to
 * carry one entry per PHASE - `marker`, `crasher`, `ept`, `spin_start`,
 * `slice`, `clockirq`, `virtio`, `linux_boot`, `linux_slice`.  Every one of
 * those was a TEST, and putting a test behind a vendor seam means writing it
 * twice: the SVM backend could not reach A2 without reimplementing nine guests
 * that have nothing vendor-specific about them (an x86 instruction encoding is
 * an x86 instruction encoding on both).  The seam is now the four generic
 * operations §3.1 asked for - `vcpu_create`, `vcpu_run`, `map`, `inject` -
 * plus the narrow window on vCPU state a caller needs to service an exit, and
 * the phase tests are CALLERS of it in hv_phases.c, compiled once.
 *
 * The rule for what belongs in `uno_vmexit` is the other half of that, because
 * getting it wrong is how a "portable" hypervisor ends up with `if (vmx)` in
 * its device models: the struct carries what a CONSUMER needs to act, in terms
 * that exist on both vendors.  The vendor's own exit code rides along as
 * `raw`, for reports only.
 * ======================================================================== */
#ifndef UNO_VIRT_HV_H
#define UNO_VIRT_HV_H

/* Vendor-neutral exit reasons.  Every one of these is a thing a caller does
 * something about; a reason nobody handles is UNKNOWN with `raw` to identify
 * it, which is what ends a guest with a report rather than a guess. */
enum {
    UNO_VX_UNKNOWN = 0,
    UNO_VX_CPUID,               /* guest executed cpuid                      */
    UNO_VX_HLT,                 /* guest halted: our "done" signal           */
    UNO_VX_SHUTDOWN,            /* guest triple-faulted; it is over          */
    UNO_VX_INTR,                /* a host interrupt is pending               */
    UNO_VX_NPF,                 /* second-stage fault (A2), `gpa` is where   */
    UNO_VX_INVALID,             /* the CPU refused the guest state           */
    UNO_VX_PREEMPT,             /* the slice clock ran out (A3)              */
    UNO_VX_RDMSR,               /* A4: we are the guest's MSR space          */
    UNO_VX_WRMSR,
    UNO_VX_IO,                  /* A6: port I/O, decoded into io_*           */
    UNO_VX_CR,                  /* A6: a control-register access, into cr_*  */
    UNO_VX_XSETBV,              /* A6: XCR0, which neither vendor carries    */
    UNO_VX_INTR_WINDOW          /* A6e: the guest can take one NOW           */
};

typedef struct uno_vmexit {
    int   reason;               /* UNO_VX_*                                  */
    unsigned long long raw;     /* the vendor's own exit code, for reports   */
    unsigned long long rip;     /* guest RIP at the exit                     */
    unsigned instr_len;         /* length of the instruction that exited     */

    /* Decoded by the backend, in terms both vendors have.  Only the fields
     * belonging to `reason` are written; the rest keep whatever they had. */
    unsigned long long gpa;     /* UNO_VX_NPF: the faulting guest-physical   */
    int npf_write;              /* ...and which way it was going             */
    unsigned io_port, io_size;  /* UNO_VX_IO                                 */
    int io_in, io_string;
    unsigned cr_num, cr_access, cr_reg;   /* UNO_VX_CR: which, how, from where */

    unsigned long long info1;   /* raw vendor detail, reports only:          */
    unsigned long long info2;   /*   VMX instr error / SVM EXITINFO2         */
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

/* A vCPU, as everything above the seam sees one.  The registers are here
 * rather than behind the seam because servicing an exit is register work and
 * that work is identical on both vendors: the guest's RAX after a CPUID is
 * the guest's RAX.  Everything the ARCHITECTURE disagrees about - the VMCS or
 * the VMCB, the launch state, the control words - is `impl`, and only its own
 * backend ever looks at it.
 *
 * `gprs` MUST stay first: the entry stubs address it by displacement. */
typedef struct uno_vcpu {
    uno_gprs gprs;
    int      quiet;             /* suppress the per-entry bring-up trace.  It
                                   is right for a selftest that enters three
                                   times and ruinous for a kernel that enters
                                   tens of thousands.                        */
    void    *impl;              /* the backend's control block               */
} uno_vcpu;

_Static_assert(__builtin_offsetof(uno_vcpu, gprs) == 0, "gprs lead a vcpu");

/* How a vCPU starts.  Two modes, because that is how many the two vendors
 * between them make easy: AMD lets a guest start with CR0.PE clear and no
 * tables at all, Intel requires paging unless the "unrestricted guest" control
 * is active - and that control requires EPT, which would drag A2 into A1.  A
 * backend that cannot host a mode says so by refusing `vcpu_create`. */
enum {
    UNO_VM_FLAT64 = 0,     /* long mode, flat segments, its own page tables  */
    UNO_VM_REAL16          /* real mode, everything through one segment base */
};

/* What the guest is allowed to do, and what the machine must do about it.
 * Each of these is one control bit on both vendors and a whole finding in
 * pc64/UNOVIRT.md on at least one of them. */
#define UNO_VMF_SLAT      (1u << 0)  /* run behind second-stage translation  */
#define UNO_VMF_PREEMPT   (1u << 1)  /* the slice clock, and host-intr exits */
#define UNO_VMF_IO_EXIT   (1u << 2)  /* every in/out exits (A6a's finding)   */
#define UNO_VMF_INVPCID   (1u << 3)  /* let the guest use INVPCID (A6b's)    */
#define UNO_VMF_CR_SHADOW (1u << 4)  /* we own the virtualization bit in CR4 */

typedef struct uno_vm_cfg {
    int mode;                             /* UNO_VM_*                        */
    unsigned features;                    /* UNO_VMF_*                       */
    unsigned long long rip, rsp, rflags;
    unsigned long long cr3;               /* FLAT64: its own page tables     */
    unsigned long long seg_base;          /* REAL16: where its segments point */
    unsigned long long gdt_base, idt_base, tr_base;
    unsigned gdt_limit, idt_limit;
    unsigned cs_sel, ds_sel;              /* FLAT64: what its GDT calls them */
} uno_vm_cfg;

/* The window on vCPU state.  Deliberately small: everything on this list is
 * something an exit handler above the seam has to read or write, and nothing
 * else is exposed.  A backend answers 0 for a thing it does not model. */
enum {
    UNO_VR_RIP = 0,
    UNO_VR_RSP,
    UNO_VR_RFLAGS,
    UNO_VR_CR0,                 /* set applies the vendor's required bits    */
    UNO_VR_CR3,
    UNO_VR_CR4,                 /* ...likewise; see S-HV-31                  */
    UNO_VR_EFER,
    UNO_VR_FS_BASE,
    UNO_VR_GS_BASE,
    UNO_VR_CR0_SHADOW,          /* what the guest reads back                 */
    UNO_VR_CR4_SHADOW,
    UNO_VR_INTR_SHADOW,         /* the one-instruction window after STI      */
    UNO_VR_CAN_INJECT,          /* read-only: would an injection land NOW    */
    UNO_VR_INTR_WINDOW,         /* write-only: exit when the guest is ready  */
    UNO_VR_XCR0                 /* the guest's XCR0, applied around entries  */
};

/* Second-stage protection and memory type.  Both vendors express these; the
 * numbers are ours. */
#define UNO_VP_R      (1u << 0)
#define UNO_VP_W      (1u << 1)
#define UNO_VP_X      (1u << 2)
#define UNO_VP_RWX    (UNO_VP_R | UNO_VP_W | UNO_VP_X)
#define UNO_VMEM_WB   6u             /* write-back, and see S-HV-19          */

typedef struct uno_hv {
    const char *name;                     /* "svm" | "vmx"                   */

    /* Enter host virtualization mode on this CPU.  1 = we are in it; 0 with
     * *why set to a short reason.  Idempotent. */
    int (*enable)(const char **why);

    /* Build a vCPU to `cfg` and make it the current one.  0 when this backend
     * cannot host that configuration - an unsupported mode, a control the
     * machine refuses, or second stage asked for with nothing mapped.  ONE
     * VMCS MEANS ONE GUEST: creating a vCPU replaces whatever was running. */
    int (*vcpu_create)(uno_vcpu *v, const uno_vm_cfg *cfg);

    /* One entry, bounded by `budget_us` when the vCPU was created with
     * UNO_VMF_PREEMPT (and unbounded otherwise, which is only ever right for
     * a guest that ends its own turn).  Returns 0 if the entry could not be
     * attempted at all; *out is filled either way. */
    int (*vcpu_run)(uno_vcpu *v, unsigned budget_us, uno_vmexit *out);

    /* Second-stage translation: make `len` bytes at guest-physical `gpa`
     * resolve to host-physical `hpa`.  The tables belong to the machine, not
     * to a vCPU, and this call REBUILDS them - one carve is what an appliance
     * gets, and a second call replaces the first.  Whole large frames only: a
     * partial tail is left unmapped so a guest reading past the end of its own
     * memory takes a reportable fault instead of touching whatever follows the
     * carve in host memory (S-HV-18).  NULL until a backend has second stage
     * at all, which is exactly the question "can this backend get past A1". */
    int (*map)(unsigned long long gpa, unsigned long long hpa,
               unsigned long long len, unsigned prot, unsigned memtype);

    /* Hand the guest an event on the next entry.  It does NOT check whether
     * the guest can take one - VM entry delivers regardless, straight through
     * whatever critical section the guest thought it was protecting - so ask
     * UNO_VR_CAN_INJECT first (S-HV-36). */
    void (*inject)(uno_vcpu *v, unsigned vector, unsigned err, int has_err);

    /* The state window above.  `set` on a control register applies whatever
     * bits the vendor requires of a guest, which is why it is a call and not
     * a struct field. */
    unsigned long long (*get)(uno_vcpu *v, int what);
    void (*set)(uno_vcpu *v, int what, unsigned long long val);
} uno_hv_t;

const uno_hv_t *uno_hv_svm(void);         /* hv_svm.c                        */
const uno_hv_t *uno_hv_vmx(void);         /* hv_vmx.c                        */

#endif /* UNO_VIRT_HV_H */
