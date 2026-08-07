/* ===========================================================================
 * unovirt - the AMD-V (SVM) backend.  See unovirt_hv.h, pc64/UNOVIRT.md.
 *
 * The vendor half only: how a VMCB is laid out, how a guest is entered, how an
 * exit code is spelled, and where a guest register lives.  The guests, the
 * device models, the instruction decode and the phase tests are in
 * hv_phases.c, above the seam.
 *
 * WHAT THIS BACKEND STILL OWES, and it is now exactly one thing: `map`.  With
 * nested paging absent a guest's physical addresses ARE host physical
 * addresses, so a guest could reach any byte in the machine - survivable for
 * A1's eleven bytes of machine code, whose segment bases address nothing else,
 * and for nothing beyond it.  So `map` is NULL, every phase from A2 on
 * declines with attribution, and the boot carries on.  It used to owe nine
 * test guests as well; those were on the wrong side of the seam and are not
 * this file's business any more.
 *
 * A1 runs in REAL MODE here, because on AMD that needs no permission: a guest
 * may start with CR0.PE clear and no tables at all.  (Intel needs the
 * "unrestricted guest" control for the same thing, which is why hv_vmx.c
 * refuses this mode and gets the long-mode arrangement instead.)  Eleven bytes
 * of guest do not justify building a GDT, page tables and a 64-bit entry path.
 * ======================================================================== */
#include "unovirt_hv.h"
#include "unovirt.h"

typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef unsigned long long u64;

/* ---- MSRs and VMCB layout ------------------------------------------------ */
#define MSR_EFER          0xC0000080u
#define EFER_SVME         (1ull << 12)
#define MSR_VM_CR         0xC0010114u
#define VM_CR_LOCK        (1ull << 3)
#define VM_CR_SVMDIS      (1ull << 4)
#define MSR_VM_HSAVE_PA   0xC0010117u

/* Control area byte offsets (APM vol 2, appendix B).  Written as offsets into
 * a byte array rather than as a struct: the layout has holes, reserved runs
 * and fields the assembler cares about the alignment of, and a struct that
 * models it is a struct somebody eventually "tidies". */
/* THESE THREE OFFSETS COST A DEBUGGING SESSION, so they are spelled out:
 * 0x000/0x004 are the CR and DR intercepts, 0x008 is the EXCEPTION bitmap,
 * 0x00C is the first instruction-intercept word (INTR..SHUTDOWN) and 0x010 is
 * the second (VMRUN..MWAIT). Writing the two instruction words four bytes
 * early - into the exception bitmap and the first word - installs no CPUID
 * intercept, no HLT intercept and no SHUTDOWN intercept, while accidentally
 * setting the VMRUN intercept that the consistency check demands. So VMRUN
 * SUCCEEDS, the guest runs, and it reaches its `hlt` with nothing intercepting
 * it and GIF still clear: the core stops forever, with no fault, no output,
 * and no watchdog (GIF=0 means the LAPIC timer cannot be delivered either).
 * The machine simply stops being a machine. */
#define VMCB_EXC          0x008          /* exception intercepts (u32)       */
#define VMCB_INTERCEPT1   0x00C          /* INTR..SHUTDOWN (u32)             */
#define VMCB_INTERCEPT2   0x010          /* VMRUN..MWAIT (u32)               */
#define VMCB_ASID         0x058          /* u32, MUST be non-zero            */
#define VMCB_TLB_CTL      0x05C          /* u8                               */
#define VMCB_INT_STATE    0x068          /* u64, bit 0 = interrupt shadow    */
#define VMCB_EXITCODE     0x070          /* u64                              */
#define VMCB_EXITINFO1    0x078
#define VMCB_EXITINFO2    0x080
#define VMCB_EVENTINJ     0x0A8          /* u64, the injection field         */
#define VMCB_NP_ENABLE    0x090          /* u64, bit 0                       */
#define VMCB_NRIP         0x0C8          /* u64, next RIP when the part has it */
#define VMCB_SAVE         0x400          /* the state save area starts here  */

/* Instruction-intercept word 1 (VMCB_INTERCEPT1) */
#define INT1_INTR         (1u << 0)
#define INT1_VINTR        (1u << 2)      /* the interrupt window             */
#define INT1_CPUID        (1u << 18)
#define INT1_HLT          (1u << 24)
#define INT1_SHUTDOWN     (1u << 31)
/* Instruction-intercept word 2 (VMCB_INTERCEPT2) */
#define INT2_VMRUN        (1u << 0)      /* VMRUN faults unless this is set  */

/* State save area, offsets from VMCB_SAVE.  A segment is
 * {u16 sel; u16 attrib; u32 limit; u64 base}. */
#define SS_ES   0x00
#define SS_CS   0x10
#define SS_SS   0x20
#define SS_DS   0x30
#define SS_FS   0x40
#define SS_GS   0x50
#define SS_GDTR 0x60
#define SS_LDTR 0x70
#define SS_IDTR 0x80
#define SS_TR   0x90
#define SS_CPL  0xCB          /* u8                                          */
#define SS_EFER 0xD0          /* u64  - guest EFER.SVME MUST be set          */
#define SS_CR4  0x148
#define SS_CR3  0x150
#define SS_CR0  0x158
#define SS_DR7  0x160
#define SS_DR6  0x168
#define SS_RFLAGS 0x170
#define SS_RIP  0x178
#define SS_RSP  0x1D8
#define SS_RAX  0x1F8

/* SVM exit codes we name */
#define SVM_EXIT_CR0_WRITE 0x10          /* 0x10..0x1F: a write to CR0..15   */
#define SVM_EXIT_INTR      0x60
#define SVM_EXIT_VINTR     0x64
#define SVM_EXIT_CPUID     0x72
#define SVM_EXIT_HLT       0x78
#define SVM_EXIT_IOIO      0x7B
#define SVM_EXIT_MSR       0x7C
#define SVM_EXIT_SHUTDOWN  0x7F
#define SVM_EXIT_XSETBV    0x8D
#define SVM_EXIT_NPF       0x400
#define SVM_EXIT_INVALID   0xFFFFFFFFFFFFFFFFull

/* ---- the pages ------------------------------------------------------------
 * .bss, 4 KiB aligned.  The image is identity-mapped, so a virtual address IS
 * a physical address here; that is also why the guest can be handed the same
 * pointer the host uses.  When `map` lands and a carve exists this stops being
 * true, and the translation already has its single seam (uno_vmm_gpa). */
__attribute__((aligned(4096))) static u8 g_vmcb[4096];
__attribute__((aligned(4096))) static u8 g_hsave[4096];

static int g_enabled;
static u64 g_guest_xcr0;

/* ---- bring-up trace, opt-in ----------------------------------------------
 * Every step of a first VMRUN is a candidate for a hang with no output: the
 * consistency checks answer with one undifferentiated exit code, and a guest
 * that faults where nothing can catch it takes the machine with it.  The
 * kernel log is no help, because it is written to disk AFTER this runs.  So
 * this file carries its own trace on the debug console, under the same
 * METAL-UNSAFE opt-in as the rest of the port (uefi_main.c: port 0x402 can be
 * SMM-trapped, so it never ships enabled). */
#ifdef UNO_DBGCON
static void trace(const char *s)
{
    while (*s) {
        __asm__ volatile ("outb %0, %1"
                          : : "a"((u8)*s), "Nd"((u16)0x402));
        s++;
    }
}
static void tracex(const char *tag, u64 v)
{
    static const char H[] = "0123456789abcdef";
    char b[20];
    int i;
    trace(tag);
    for (i = 0; i < 16; i++) b[i] = H[(v >> (60 - 4 * i)) & 15];
    b[16] = '\n'; b[17] = 0;
    trace(b);
}
#else
__attribute__((unused)) static void trace(const char *s) { (void)s; }
__attribute__((unused)) static void tracex(const char *t, u64 v) { (void)t; (void)v; }
#endif

static u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static void wrmsr(u32 msr, u64 v)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}

static void put32(u8 *p, unsigned off, u32 v) { *(u32 *)(p + off) = v; }
static void put64(u8 *p, unsigned off, u64 v) { *(u64 *)(p + off) = v; }
static u32  get32(const u8 *p, unsigned off)  { return *(const u32 *)(p + off); }
static u64  get64(const u8 *p, unsigned off)  { return *(const u64 *)(p + off); }

static void seg(u8 *save, unsigned off, u16 sel, u16 attrib, u32 limit, u64 base)
{
    *(u16 *)(save + off + 0) = sel;
    *(u16 *)(save + off + 2) = attrib;
    *(u32 *)(save + off + 4) = limit;
    *(u64 *)(save + off + 8) = base;
}

/* ---- entering host SVM operation ----------------------------------------- */

static int svm_enable(const char **why)
{
    u64 vmcr;
    if (g_enabled) return 1;

    vmcr = rdmsr(MSR_VM_CR);
    if (vmcr & VM_CR_SVMDIS) {
        /* Disabled - but by whom?  With the lock bit set this is the
         * firmware's decision and it is final until the next power cycle.
         * Without it the bit is just a bit, and clearing it is the documented
         * way in (APM vol 2 §15.4).  uno_vmm_eligible() has already refused
         * the locked case; this arm exists so the unlocked one is not a
         * mysterious VMRUN failure ten functions later. */
        if (vmcr & VM_CR_LOCK) { *why = "SVM disabled and locked by firmware"; return 0; }
        wrmsr(MSR_VM_CR, vmcr & ~VM_CR_SVMDIS);
        if (rdmsr(MSR_VM_CR) & VM_CR_SVMDIS) { *why = "VM_CR.SVMDIS would not clear"; return 0; }
    }

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);
    if (!(rdmsr(MSR_EFER) & EFER_SVME)) { *why = "EFER.SVME would not set"; return 0; }

    /* The host save area is where the CPU parks OUR state across a guest.  It
     * is a physical address and it must be one the CPU can reach with no
     * translation of its own - identity mapping is what makes this pointer
     * legal as an address. */
    wrmsr(MSR_VM_HSAVE_PA, (u64)(unsigned long long)(void *)g_hsave);
    tracex("[hv] svme on, hsave=", (u64)(unsigned long long)(void *)g_hsave);
    g_enabled = 1;
    *why = 0;
    return 1;
}

/* ---- entry and exit -------------------------------------------------------
 * VMRUN saves a SUBSET of host state (CS/SS/RFLAGS/RIP/RSP/CR0/CR3/CR4/EFER
 * and RAX) into the host save area and restores it on #VMEXIT.  It does NOT
 * save the general-purpose registers, so between VMRUN and the first host
 * instruction after it, every GPR except RAX holds a GUEST value.  Saving and
 * restoring them is the caller's job, and this stub is that job.
 *
 * It does not save FS/GS/TR/LDTR hidden state either (that is what VMSAVE is
 * for).  A1's guests never load a segment register, so this stub does not
 * either; the moment a guest is Linux, VMSAVE/VMLOAD around the entry becomes
 * mandatory and this comment is the reminder.
 *
 * GIF: VMRUN sets it, #VMEXIT clears it.  With GIF clear NOTHING is delivered
 * to this core - not the LAPIC timer, not an NMI - so the STGI immediately
 * after VMRUN is not tidiness, it is the difference between a machine that
 * carries on and one that silently stops taking interrupts forever. */
static void svm_entry(u64 vmcb_pa, uno_gprs *g)
{
    __asm__ volatile (
        "push %%rbx\n\t  push %%rbp\n\t  push %%rsi\n\t  push %%rdi\n\t"
        "push %%r12\n\t  push %%r13\n\t  push %%r14\n\t  push %%r15\n\t"
        "push %[vmcb]\n\t"
        "push %[ctx]\n\t"
        /* load the guest's registers; RAX travels in the VMCB, not here */
        "mov (%%rsp), %%rax\n\t"
        "mov 0x08(%%rax), %%rbx\n\t  mov 0x10(%%rax), %%rcx\n\t"
        "mov 0x18(%%rax), %%rdx\n\t  mov 0x20(%%rax), %%rsi\n\t"
        "mov 0x28(%%rax), %%rdi\n\t  mov 0x30(%%rax), %%rbp\n\t"
        "mov 0x38(%%rax), %%r8\n\t   mov 0x40(%%rax), %%r9\n\t"
        "mov 0x48(%%rax), %%r10\n\t  mov 0x50(%%rax), %%r11\n\t"
        "mov 0x58(%%rax), %%r12\n\t  mov 0x60(%%rax), %%r13\n\t"
        "mov 0x68(%%rax), %%r14\n\t  mov 0x70(%%rax), %%r15\n\t"
        "mov 0x08(%%rsp), %%rax\n\t"          /* rax = the VMCB address      */
        "clgi\n\t"
        "vmrun %%rax\n\t"
        "stgi\n\t"
        /* rax is ours again (restored from the host save area); everything
         * else still holds what the guest left. */
        "push %%rax\n\t"
        "mov 0x08(%%rsp), %%rax\n\t"          /* rax = ctx                   */
        "mov %%rbx, 0x08(%%rax)\n\t  mov %%rcx, 0x10(%%rax)\n\t"
        "mov %%rdx, 0x18(%%rax)\n\t  mov %%rsi, 0x20(%%rax)\n\t"
        "mov %%rdi, 0x28(%%rax)\n\t  mov %%rbp, 0x30(%%rax)\n\t"
        "mov %%r8,  0x38(%%rax)\n\t  mov %%r9,  0x40(%%rax)\n\t"
        "mov %%r10, 0x48(%%rax)\n\t  mov %%r11, 0x50(%%rax)\n\t"
        "mov %%r12, 0x58(%%rax)\n\t  mov %%r13, 0x60(%%rax)\n\t"
        "mov %%r14, 0x68(%%rax)\n\t  mov %%r15, 0x70(%%rax)\n\t"
        "add $0x18, %%rsp\n\t"                /* drop saved rax, ctx, vmcb   */
        "pop %%r15\n\t  pop %%r14\n\t  pop %%r13\n\t  pop %%r12\n\t"
        "pop %%rdi\n\t  pop %%rsi\n\t  pop %%rbp\n\t  pop %%rbx\n\t"
        :
        : [vmcb] "r" (vmcb_pa), [ctx] "r" (g)
        : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
}

/* ---- a machine for a guest to run on -------------------------------------- */

static int svm_vcpu_create(uno_vcpu *v, const uno_vm_cfg *cfg)
{
    u8 *vm = g_vmcb, *s = g_vmcb + VMCB_SAVE;
    unsigned i;

    /* Long mode would need a GDT, page tables and a 64-bit entry path built
     * here, and nothing above the seam needs it until `map` exists - so it is
     * refused rather than half-built, and A1 takes the real-mode arrangement.
     * Second stage is the same answer for the same reason. */
    if (cfg->mode != UNO_VM_REAL16) return 0;
    if (cfg->features & UNO_VMF_SLAT) return 0;

    for (i = 0; i < sizeof g_vmcb; i++) vm[i] = 0;

    put32(vm, VMCB_INTERCEPT1, INT1_CPUID | INT1_HLT | INT1_SHUTDOWN | INT1_INTR);
    put32(vm, VMCB_INTERCEPT2, INT2_VMRUN);
    put32(vm, VMCB_ASID, 1);                   /* 0 is illegal              */
    put32(vm, VMCB_TLB_CTL, 1);                /* flush this guest's TLB    */

    /* Real-mode segments whose BASE is where the guest's code actually is.
     * The guest addresses everything through them, which is the only reason a
     * guest with no second-stage translation is bounded at all here. */
    seg(s, SS_CS, (u16)(cfg->seg_base >> 4), 0x009B, 0xFFFF, cfg->seg_base);
    seg(s, SS_DS, (u16)(cfg->seg_base >> 4), 0x0093, 0xFFFF, cfg->seg_base);
    seg(s, SS_ES, (u16)(cfg->seg_base >> 4), 0x0093, 0xFFFF, cfg->seg_base);
    seg(s, SS_SS, (u16)(cfg->seg_base >> 4), 0x0093, 0xFFFF, cfg->seg_base);
    seg(s, SS_FS, 0, 0x0093, 0xFFFF, 0);
    seg(s, SS_GS, 0, 0x0093, 0xFFFF, 0);
    seg(s, SS_GDTR, 0, 0, 0, 0);
    seg(s, SS_LDTR, 0, 0x0082, 0xFFFF, 0);
    seg(s, SS_TR,   0, 0x008B, 0xFFFF, 0);
    /* The IDT is the crasher's whole mechanism: limit 0 means the first
     * exception cannot be delivered, which raises another, which is a triple
     * fault - SHUTDOWN, intercepted.  A working guest never touches it. */
    seg(s, SS_IDTR, 0, 0, (u32)cfg->idt_limit, cfg->idt_base);

    put64(s, SS_CR0, 0x00000010ull);           /* ET; PE and PG both clear  */
    put64(s, SS_CR4, 0);
    put64(s, SS_CR3, 0);
    put64(s, SS_DR6, 0xFFFF0FF0ull);
    put64(s, SS_DR7, 0x400);
    /* EFER.SVME must be set IN THE GUEST or VMRUN fails the consistency check
     * with EXITCODE -1 and no other explanation.  It is the single most common
     * first-VMRUN failure and it is not intuitive: the guest is not running a
     * hypervisor, but the bit is still required. */
    put64(s, SS_EFER, EFER_SVME);
    put64(s, SS_RFLAGS, cfg->rflags);
    put64(s, SS_RIP, cfg->rip);
    put64(s, SS_RSP, cfg->rsp);
    put64(s, SS_RAX, 0);
    *(u8 *)(s + SS_CPL) = 0;
    put64(vm, VMCB_NP_ENABLE, 0);              /* `map` turns this on       */

    for (i = 0; i < sizeof v->gprs / sizeof(u64); i++) ((u64 *)&v->gprs)[i] = 0;
    v->quiet = 0;
    v->impl = g_vmcb;
    return 1;
}

/* How long was the instruction that exited?  The part may save the next RIP
 * for us, and where it does that is the right answer for every encoding.
 * Where it does not, the exits this backend actually services have known
 * lengths, and inventing one for an exit we do not service would be worse than
 * leaving it 0 - a caller stepping by 0 loops visibly, a caller stepping by a
 * guess corrupts the guest silently. */
static unsigned svm_instr_len(u64 code, u64 rip)
{
    u64 nrip = get64(g_vmcb, VMCB_NRIP);
    if (uno_vmm_probe()->nrip && nrip > rip && nrip - rip <= 15)
        return (unsigned)(nrip - rip);
    switch (code) {
    case SVM_EXIT_CPUID: return 2;             /* 0F A2, always             */
    case SVM_EXIT_MSR:   return 2;             /* 0F 30 / 0F 32             */
    case SVM_EXIT_HLT:   return 1;
    case SVM_EXIT_IOIO: {                      /* EXITINFO2 is the next RIP */
        u64 next = get64(g_vmcb, VMCB_EXITINFO2);
        return (next > rip && next - rip <= 15) ? (unsigned)(next - rip) : 0;
    }
    default:             return 0;
    }
}

static void classify(uno_vmexit *out)
{
    u64 code = get64(g_vmcb, VMCB_EXITCODE);
    u64 i1   = get64(g_vmcb, VMCB_EXITINFO1);
    out->raw   = code;
    out->rip   = get64(g_vmcb + VMCB_SAVE, SS_RIP);
    out->info1 = i1;
    out->info2 = get64(g_vmcb, VMCB_EXITINFO2);
    out->instr_len = svm_instr_len(code, out->rip);
    switch (code) {
    case SVM_EXIT_CPUID:    out->reason = UNO_VX_CPUID;    break;
    case SVM_EXIT_HLT:      out->reason = UNO_VX_HLT;      break;
    case SVM_EXIT_SHUTDOWN: out->reason = UNO_VX_SHUTDOWN; break;
    case SVM_EXIT_INTR:     out->reason = UNO_VX_INTR;     break;
    case SVM_EXIT_VINTR:    out->reason = UNO_VX_INTR_WINDOW; break;
    case SVM_EXIT_XSETBV:   out->reason = UNO_VX_XSETBV;   break;
    case SVM_EXIT_INVALID:  out->reason = UNO_VX_INVALID;  break;
    case SVM_EXIT_MSR:
        out->reason = (i1 & 1) ? UNO_VX_WRMSR : UNO_VX_RDMSR;
        break;
    case SVM_EXIT_NPF:
        out->reason    = UNO_VX_NPF;
        out->gpa       = out->info2;
        out->npf_write = (i1 & 2) ? 1 : 0;
        break;
    case SVM_EXIT_IOIO:
        /* EXITINFO1 carries the whole access: direction, string, the size as
         * three separate bits, and the port in the top half. */
        out->reason    = UNO_VX_IO;
        out->io_in     = (i1 & 1) ? 1 : 0;
        out->io_string = (i1 & 4) ? 1 : 0;
        out->io_size   = (i1 & 0x10) ? 1 : ((i1 & 0x20) ? 2 : 4);
        out->io_port   = (unsigned)(i1 >> 16) & 0xFFFF;
        break;
    default:
        if (code >= SVM_EXIT_CR0_WRITE && code < SVM_EXIT_CR0_WRITE + 16) {
            out->reason    = UNO_VX_CR;
            out->cr_num    = (unsigned)(code - SVM_EXIT_CR0_WRITE);
            out->cr_access = 0;                /* a write to the register   */
            /* Which register it came FROM needs the instruction decoded, and
             * nothing above the seam can act on a source it does not know -
             * so this stays 0 (RAX) and a CR exit ends the guest with a
             * report until decode-assist lands. */
            out->cr_reg    = 0;
        } else {
            out->reason = UNO_VX_UNKNOWN;
        }
        break;
    }
}

/* SVM has no preemption timer, so `budget_us` is not honoured here: the slice
 * clock on AMD is an intercepted local-APIC one-shot, which arrives with the
 * rest of A3.  Until then a guest that does not end its own turn is a guest
 * this backend must not be handed - which is what UNO_VMF_PREEMPT being
 * refused by `vcpu_create` (via UNO_VMF_SLAT) already ensures. */
static int svm_vcpu_run(uno_vcpu *v, unsigned budget_us, uno_vmexit *out)
{
    (void)budget_us;
    put64(g_vmcb + VMCB_SAVE, SS_RAX, v->gprs.rax);
    if (!v->quiet) tracex("[hv] vmrun rip=", get64(g_vmcb + VMCB_SAVE, SS_RIP));
    svm_entry((u64)(unsigned long long)(void *)g_vmcb, &v->gprs);
    if (!v->quiet) tracex("[hv] exit  code=", get64(g_vmcb, VMCB_EXITCODE));
    v->gprs.rax = get64(g_vmcb + VMCB_SAVE, SS_RAX);
    classify(out);
    return 1;
}

/* The event-injection field: vector, type, an optional error code, and the
 * valid bit the CPU clears once it has delivered. */
static void svm_inject(uno_vcpu *v, unsigned vector, unsigned err, int has_err)
{
    u64 ev = (1ull << 31) | (vector & 0xFF);   /* type 0 = external interrupt */
    (void)v;
    if (has_err) ev |= (1ull << 11) | ((u64)err << 32);
    put64(g_vmcb, VMCB_EVENTINJ, ev);
}

/* ---- the state window ----------------------------------------------------- */

static u64 svm_get(uno_vcpu *v, int what)
{
    u8 *s = g_vmcb + VMCB_SAVE;
    (void)v;
    switch (what) {
    case UNO_VR_RIP:        return get64(s, SS_RIP);
    case UNO_VR_RSP:        return get64(s, SS_RSP);
    case UNO_VR_RFLAGS:     return get64(s, SS_RFLAGS);
    case UNO_VR_CR0:        return get64(s, SS_CR0);
    case UNO_VR_CR3:        return get64(s, SS_CR3);
    case UNO_VR_CR4:        return get64(s, SS_CR4);
    case UNO_VR_EFER:       return get64(s, SS_EFER);
    case UNO_VR_FS_BASE:    return get64(s, SS_FS + 8);
    case UNO_VR_GS_BASE:    return get64(s, SS_GS + 8);
    case UNO_VR_INTR_SHADOW: return get64(g_vmcb, VMCB_INT_STATE) & 1;
    case UNO_VR_XCR0:       return g_guest_xcr0;
    case UNO_VR_CAN_INJECT:
        if (get64(g_vmcb, VMCB_EVENTINJ) & (1ull << 31)) return 0;
        if (!(get64(s, SS_RFLAGS) & 0x200)) return 0;
        if (get64(g_vmcb, VMCB_INT_STATE) & 1) return 0;
        return 1;
    default:                return 0;          /* no CR shadows on AMD:
                                                  the guest owns its CR4     */
    }
}

static void svm_set(uno_vcpu *v, int what, u64 val)
{
    u8 *s = g_vmcb + VMCB_SAVE;
    (void)v;
    switch (what) {
    case UNO_VR_RIP:        put64(s, SS_RIP, val); break;
    case UNO_VR_RSP:        put64(s, SS_RSP, val); break;
    case UNO_VR_RFLAGS:     put64(s, SS_RFLAGS, val); break;
    case UNO_VR_CR0:        put64(s, SS_CR0, val); break;
    case UNO_VR_CR3:        put64(s, SS_CR3, val); break;
    case UNO_VR_CR4:        put64(s, SS_CR4, val); break;
    case UNO_VR_EFER:       put64(s, SS_EFER, val | EFER_SVME); break;
    case UNO_VR_FS_BASE:    put64(s, SS_FS + 8, val); break;
    case UNO_VR_GS_BASE:    put64(s, SS_GS + 8, val); break;
    case UNO_VR_INTR_SHADOW: {
        u64 st = get64(g_vmcb, VMCB_INT_STATE);
        put64(g_vmcb, VMCB_INT_STATE, (st & ~1ull) | (val & 1));
        break;
    }
    case UNO_VR_INTR_WINDOW: {
        /* AMD's interrupt window is the VINTR intercept: arm it and the CPU
         * exits the instant the guest becomes able to take one. */
        u32 w = get32(g_vmcb, VMCB_INTERCEPT1);
        put32(g_vmcb, VMCB_INTERCEPT1, val ? (w | INT1_VINTR) : (w & ~INT1_VINTR));
        break;
    }
    case UNO_VR_XCR0:       g_guest_xcr0 = val; break;
    default: break;                            /* CR shadows: see svm_get   */
    }
}

/* `map` is NULL: no nested paging yet, so everything from A2 on declines with
 * attribution rather than running a guest that could reach the whole machine. */
static const uno_hv_t SVM = { "svm", svm_enable, svm_vcpu_create, svm_vcpu_run,
                              0, svm_inject, svm_get, svm_set };

const uno_hv_t *uno_hv_svm(void) { return &SVM; }
