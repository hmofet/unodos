/* ===========================================================================
 * unovirt - the Intel VT-x (VMX) backend.  See unovirt_hv.h, pc64/UNOVIRT.md.
 *
 * This file is the vendor half and nothing else: how a control block is made
 * current, what the machine insists on in a control word, how an exit reason
 * is spelled, and where a guest register lives.  The guests, the device
 * models, the instruction decode and every phase test are in hv_phases.c,
 * above the seam, because none of them are Intel-specific.
 *
 * TWO THINGS ABOUT VMX SHAPE EVERYTHING BELOW, and both are forced by the
 * architecture rather than chosen.
 *
 * ONE: a guest must be paged.  AMD lets a guest start with CR0.PE clear and no
 * tables at all; Intel requires CR0.PE and CR0.PG unless the "unrestricted
 * guest" secondary control is active, and that control requires EPT - which
 * would drag A2 into A1.  So `vcpu_create` refuses UNO_VM_REAL16 and every
 * guest here runs in long mode from the start.
 *
 * TWO: VMX saves and restores NOTHING of the general-purpose file, not even
 * RAX (SVM at least carries RAX in the VMCB).  Host RSP and RIP come out of
 * the VMCS, which is what makes a VM exit land on an instruction we chose.
 * ======================================================================== */
#include "unovirt_hv.h"
#include "unovirt.h"
#include "pc64_native.h" /* the calibrated TSC rate, for the slice budget */

typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef unsigned long long u64;

#define MSR_EFER              0xC0000080u
#define EFER_LME              (1ull << 8)
#define EFER_LMA              (1ull << 10)
#define IA32_FEATURE_CONTROL  0x3Au
#define FC_LOCK               (1ull << 0)
#define FC_VMXON              (1ull << 2)
#define IA32_VMX_BASIC        0x480u
#define IA32_VMX_PINBASED     0x481u
#define IA32_VMX_PROCBASED    0x482u
#define IA32_VMX_EXIT_CTLS    0x483u
#define IA32_VMX_ENTRY_CTLS   0x484u
#define IA32_VMX_MISC         0x485u
#define IA32_VMX_CR0_FIXED0   0x486u
#define IA32_VMX_CR0_FIXED1   0x487u
#define IA32_VMX_CR4_FIXED0   0x488u
#define IA32_VMX_CR4_FIXED1   0x489u
#define IA32_VMX_PROCBASED2   0x48Bu
#define IA32_VMX_TRUE_PIN     0x48Du
#define IA32_VMX_TRUE_PROC    0x48Eu
#define IA32_VMX_TRUE_EXIT    0x48Fu
#define IA32_VMX_TRUE_ENTRY   0x490u

/* VMCS field encodings (SDM vol 3, appendix B) */
#define GUEST_ES_SEL      0x0800
#define GUEST_CS_SEL      0x0802
#define GUEST_SS_SEL      0x0804
#define GUEST_DS_SEL      0x0806
#define GUEST_FS_SEL      0x0808
#define GUEST_GS_SEL      0x080A
#define GUEST_LDTR_SEL    0x080C
#define GUEST_TR_SEL      0x080E
#define HOST_ES_SEL       0x0C00
#define HOST_CS_SEL       0x0C02
#define HOST_SS_SEL       0x0C04
#define HOST_DS_SEL       0x0C06
#define HOST_FS_SEL       0x0C08
#define HOST_GS_SEL       0x0C0A
#define HOST_TR_SEL       0x0C0C
#define VMCS_LINK_PTR     0x2800
#define GUEST_IA32_EFER   0x2806
#define GUEST_PHYS_ADDR   0x2400
#define EPT_POINTER       0x201A
#define PIN_BASED_CTLS    0x4000
#define PROC_BASED_CTLS   0x4002
#define EXCEPTION_BITMAP  0x4004
#define CR3_TARGET_COUNT  0x400A
#define VM_EXIT_CTLS      0x400C
#define VM_EXIT_MSR_STORE_CNT 0x400E
#define VM_EXIT_MSR_LOAD_CNT  0x4010
#define VM_ENTRY_CTLS     0x4012
#define VM_ENTRY_MSR_LOAD_CNT 0x4014
#define VM_ENTRY_INTR_INFO    0x4016
#define VM_ENTRY_INTR_ERROR   0x4018
#define PROC_BASED_CTLS2  0x401E
#define VM_INSTR_ERROR    0x4400
#define VM_EXIT_REASON    0x4402
#define VM_EXIT_INSTR_LEN 0x440C
#define VMX_PREEMPT_VALUE 0x482E
#define GUEST_ES_LIMIT    0x4800
#define GUEST_CS_LIMIT    0x4802
#define GUEST_SS_LIMIT    0x4804
#define GUEST_DS_LIMIT    0x4806
#define GUEST_FS_LIMIT    0x4808
#define GUEST_GS_LIMIT    0x480A
#define GUEST_LDTR_LIMIT  0x480C
#define GUEST_TR_LIMIT    0x480E
#define GUEST_GDTR_LIMIT  0x4810
#define GUEST_IDTR_LIMIT  0x4812
#define GUEST_ES_AR       0x4814
#define GUEST_CS_AR       0x4816
#define GUEST_SS_AR       0x4818
#define GUEST_DS_AR       0x481A
#define GUEST_FS_AR       0x481C
#define GUEST_GS_AR       0x481E
#define GUEST_LDTR_AR     0x4820
#define GUEST_TR_AR       0x4822
#define GUEST_INTERRUPTIBILITY 0x4824
#define GUEST_ACTIVITY    0x4826
#define GUEST_SYSENTER_CS 0x482A
#define HOST_SYSENTER_CS  0x4C00
#define CR0_GH_MASK       0x6000
#define CR4_GH_MASK       0x6002
#define CR0_READ_SHADOW   0x6004
#define CR4_READ_SHADOW   0x6006
#define EXIT_QUALIFICATION 0x6400
#define GUEST_CR0         0x6800
#define GUEST_CR3         0x6802
#define GUEST_CR4         0x6804
#define GUEST_ES_BASE     0x6806
#define GUEST_CS_BASE     0x6808
#define GUEST_SS_BASE     0x680A
#define GUEST_DS_BASE     0x680C
#define GUEST_FS_BASE     0x680E
#define GUEST_GS_BASE     0x6810
#define GUEST_LDTR_BASE   0x6812
#define GUEST_TR_BASE     0x6814
#define GUEST_GDTR_BASE   0x6816
#define GUEST_IDTR_BASE   0x6818
#define GUEST_DR7         0x681A
#define GUEST_RSP         0x681C
#define GUEST_RIP         0x681E
#define GUEST_RFLAGS      0x6820
#define HOST_CR0          0x6C00
#define HOST_CR3          0x6C02
#define HOST_CR4          0x6C04
#define HOST_FS_BASE      0x6C06
#define HOST_GS_BASE      0x6C08
#define HOST_TR_BASE      0x6C0A
#define HOST_GDTR_BASE    0x6C0C
#define HOST_IDTR_BASE    0x6C0E
#define HOST_RSP          0x6C14
#define HOST_RIP          0x6C16

/* Exit reasons we name */
#define EXIT_EXCEPTION    0
#define EXIT_EXT_INTR     1
#define EXIT_TRIPLE_FAULT 2
#define EXIT_INTR_WINDOW  7
#define EXIT_CPUID        10
#define EXIT_HLT          12
#define EXIT_CR           28
#define EXIT_IO           30
#define EXIT_RDMSR        31
#define EXIT_WRMSR        32
#define EXIT_EPT_VIOLATION 48
#define EXIT_EPT_MISCONFIG 49
#define EXIT_PREEMPT      52
#define EXIT_XSETBV       55

/* Control bits, each of which is a finding in pc64/UNOVIRT.md */
#define PIN_EXT_INTR_EXIT (1u << 0)
#define PIN_PREEMPT       (1u << 6)
#define PROC_INTR_WINDOW  (1u << 2)
#define PROC_HLT_EXITING  (1u << 7)
#define PROC_IO_EXITING   (1u << 24)
#define PROC_SECONDARY    (1u << 31)
#define PROC2_EPT         (1u << 1)
#define PROC2_RDTSCP      (1u << 3)
#define PROC2_INVPCID     (1u << 12)
#define EXIT_HOST_ADDR64  (1u << 9)
#define ENTRY_IA32E_GUEST (1u << 9)
#define ENTRY_LOAD_EFER   (1u << 15)
#define CR4_VMXE          (1ull << 13)
#define VM_ENTRY_INTR_VALID 0x80000000u

__attribute__((aligned(4096))) static u8 g_vmxon[4096];
__attribute__((aligned(4096))) static u8 g_vmcs[4096];

static int g_on;
static int g_launched;
static volatile u32 g_entry_failed;     /* the stub sets this, see vmx_entry */

/* Latched by vmcs_begin, because they cannot change while a vCPU is current
 * and re-deriving them costs a `rdmsr` on a path that runs tens of thousands
 * of times a second. */
static u32 g_proc;                      /* the primary controls, adjusted     */
static unsigned g_preempt_shift;        /* the slice clock's tick scale       */
static int g_preempt_on;

/* ---- bring-up trace, opt-in (see hv_svm.c for why this exists) ----------- */
#ifdef UNO_DBGCON
static void trace(const char *s)
{
    while (*s) { __asm__ volatile ("outb %0, %1" : : "a"((u8)*s), "Nd"((u16)0x402)); s++; }
}
static void tracex(const char *tag, u64 v)
{
    static const char H[] = "0123456789abcdef";
    char b[20]; int i;
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

/* AT&T reverses the Intel operand order: `vmwrite value, field`. */
static void vmwrite(u64 field, u64 val)
{
    __asm__ volatile ("vmwrite %1, %0" : : "r"(field), "r"(val) : "cc");
}
static u64 vmread(u64 field)
{
    u64 v = 0;
    __asm__ volatile ("vmread %1, %0" : "=r"(v) : "r"(field) : "cc");
    return v;
}

/* A control word is not what we ask for, it is what we ask for after the
 * machine has had its say: the low half of each capability MSR is the set of
 * bits that MUST be 1, the high half the set that MAY be.  Skipping this is
 * how a VMLAUNCH fails with "invalid control field" on one CPU generation and
 * works on the next. */
static u32 adjust(u32 msr_true, u32 msr_legacy, u32 want, int have_true)
{
    u64 m = rdmsr(have_true ? msr_true : msr_legacy);
    u32 v = want;
    v |= (u32)m;
    v &= (u32)(m >> 32);
    return v;
}

static u64 cr0_read(void) { u64 v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static u64 cr3_read(void) { u64 v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v; }
static u64 cr4_read(void) { u64 v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }
static void cr4_write(u64 v) { __asm__ volatile ("mov %0, %%cr4" : : "r"(v)); }

/* ---- XCR0, which VMX does not save or restore ----------------------------
 * XSETBV exits unconditionally, and it has to: XCR0 is machine state VMX
 * carries in neither direction, so a guest that enables AVX would enable it
 * for the host too and leave it that way. The guest's value is applied around
 * its entries and the host's put back afterwards. */
static u64 g_host_xcr0, g_guest_xcr0;

static int host_osxsave(void)
{ u64 c4; __asm__ volatile ("mov %%cr4, %0" : "=r"(c4)); return (int)((c4 >> 18) & 1); }
static u64 xgetbv0(void)
{ u32 a, d; __asm__ volatile ("xgetbv" : "=a"(a), "=d"(d) : "c"(0)); return ((u64)d << 32) | a; }
static void xsetbv0(u64 v)
{ __asm__ volatile ("xsetbv" : : "a"((u32)v), "d"((u32)(v >> 32)), "c"(0)); }

/* ---- entering VMX operation ---------------------------------------------- */

static int vmx_enable(const char **why)
{
    u64 fc, basic, cr4, cr0;
    if (g_on) return 1;

    fc = rdmsr(IA32_FEATURE_CONTROL);
    if (fc & FC_LOCK) {
        if (!(fc & FC_VMXON)) { *why = "VMX disabled and locked by firmware"; return 0; }
    } else {
        /* Unlocked: ours to set, once, and the lock bit goes with it because
         * the architecture requires VMXON to see it set. */
        wrmsr(IA32_FEATURE_CONTROL, fc | FC_LOCK | FC_VMXON);
        fc = rdmsr(IA32_FEATURE_CONTROL);
        if (!(fc & FC_VMXON)) { *why = "IA32_FEATURE_CONTROL would not take VMXON"; return 0; }
    }

    /* CR0 and CR4 have to satisfy the FIXED0/FIXED1 MSRs before VMXON, not
     * after: a bit the machine requires and we have clear is a #GP on the
     * VMXON itself, with nothing to read afterwards that says which bit. */
    cr0 = cr0_read();
    if ((cr0 | rdmsr(IA32_VMX_CR0_FIXED0)) != cr0 ||
        (cr0 & ~rdmsr(IA32_VMX_CR0_FIXED1)) != 0) {
        *why = "CR0 does not satisfy IA32_VMX_CR0_FIXED0/1";
        return 0;
    }
    cr4 = cr4_read() | CR4_VMXE;
    cr4 |= rdmsr(IA32_VMX_CR4_FIXED0);
    if (cr4 & ~rdmsr(IA32_VMX_CR4_FIXED1)) {
        *why = "CR4 cannot satisfy IA32_VMX_CR4_FIXED0/1";
        return 0;
    }
    cr4_write(cr4);

    basic = rdmsr(IA32_VMX_BASIC);
    *(u32 *)g_vmxon = (u32)(basic & 0x7FFFFFFFu);        /* revision id       */
    {
        u64 pa = (u64)(unsigned long long)(void *)g_vmxon;
        u8 fail = 0;
        __asm__ volatile ("vmxon %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
        if (fail) { *why = "VMXON refused"; return 0; }
    }
    tracex("[hv] vmxon ok, rev=", basic & 0x7FFFFFFFu);
    g_on = 1;
    *why = 0;
    return 1;
}

/* ---- entry and exit -------------------------------------------------------
 * VMX restores host RSP and RIP from the VMCS on every exit, so the exit lands
 * on the label below with the stack we chose - which is the whole reason this
 * stub can be written at all.  Nothing else is restored: every GPR holds a
 * guest value at that point, RAX included.
 *
 * `g_entry_failed` distinguishes the two ways out.  A VM exit resumes at
 * HOST_RIP (label 1); a VMLAUNCH that the machine REFUSES simply falls through
 * to the next instruction with a flag set and no exit reason written, which is
 * otherwise indistinguishable from a guest that exited for reason 0. */
static void vmx_entry(uno_gprs *g)
{
    __asm__ volatile (
        "push %%rbx\n\t  push %%rbp\n\t  push %%rsi\n\t  push %%rdi\n\t"
        "push %%r12\n\t  push %%r13\n\t  push %%r14\n\t  push %%r15\n\t"
        /* A VM EXIT LOADS HOST RFLAGS WITH 0x2 - every flag clear, IF
         * included. Without saving and restoring them here, the host returns
         * from its first guest with interrupts disabled and stays that way.
         * Nothing in this OS is interrupt-driven except the LAPIC watchdog,
         * so the machine looks perfectly healthy while the one mechanism that
         * exists to catch a hang is quietly dead. */
        "pushfq\n\t"
        "push %[ctx]\n\t"
        "mov $0x6C14, %%rdx\n\t  vmwrite %%rsp, %%rdx\n\t"   /* HOST_RSP     */
        "lea 1f(%%rip), %%rax\n\t"
        "mov $0x6C16, %%rdx\n\t  vmwrite %%rax, %%rdx\n\t"   /* HOST_RIP     */
        "mov (%%rsp), %%rax\n\t"
        "mov 0x08(%%rax), %%rbx\n\t  mov 0x10(%%rax), %%rcx\n\t"
        "mov 0x18(%%rax), %%rdx\n\t  mov 0x20(%%rax), %%rsi\n\t"
        "mov 0x28(%%rax), %%rdi\n\t  mov 0x30(%%rax), %%rbp\n\t"
        "mov 0x38(%%rax), %%r8\n\t   mov 0x40(%%rax), %%r9\n\t"
        "mov 0x48(%%rax), %%r10\n\t  mov 0x50(%%rax), %%r11\n\t"
        "mov 0x58(%%rax), %%r12\n\t  mov 0x60(%%rax), %%r13\n\t"
        "mov 0x68(%%rax), %%r14\n\t  mov 0x70(%%rax), %%r15\n\t"
        "mov (%%rax), %%rax\n\t"                             /* guest RAX    */
        "cmpl $0, %[launched]\n\t"
        "jne 2f\n\t"
        "vmlaunch\n\t"
        "jmp 3f\n\t"
        "2:\n\t"
        "vmresume\n\t"
        "3:\n\t"                                     /* the machine refused  */
        "movl $1, %[failed]\n\t"
        "jmp 4f\n\t"
        "1:\n\t"                                     /* a VM exit landed     */
        "movl $0, %[failed]\n\t"
        "4:\n\t"
        "push %%rax\n\t"
        "mov 0x08(%%rsp), %%rax\n\t"                 /* rax = ctx            */
        "mov %%rbx, 0x08(%%rax)\n\t  mov %%rcx, 0x10(%%rax)\n\t"
        "mov %%rdx, 0x18(%%rax)\n\t  mov %%rsi, 0x20(%%rax)\n\t"
        "mov %%rdi, 0x28(%%rax)\n\t  mov %%rbp, 0x30(%%rax)\n\t"
        "mov %%r8,  0x38(%%rax)\n\t  mov %%r9,  0x40(%%rax)\n\t"
        "mov %%r10, 0x48(%%rax)\n\t  mov %%r11, 0x50(%%rax)\n\t"
        "mov %%r12, 0x58(%%rax)\n\t  mov %%r13, 0x60(%%rax)\n\t"
        "mov %%r14, 0x68(%%rax)\n\t  mov %%r15, 0x70(%%rax)\n\t"
        "pop %%rcx\n\t"
        "mov %%rcx, (%%rax)\n\t"                     /* guest RAX            */
        "add $0x08, %%rsp\n\t"                       /* drop ctx             */
        "popfq\n\t"                                  /* ...and IF comes back */
        "pop %%r15\n\t  pop %%r14\n\t  pop %%r13\n\t  pop %%r12\n\t"
        "pop %%rdi\n\t  pop %%rsi\n\t  pop %%rbp\n\t  pop %%rbx\n\t"
        : [failed] "=m" (g_entry_failed)
        : [ctx] "r" (g), [launched] "m" (g_launched)
        : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
}

/* ---- second stage: EPT ----------------------------------------------------
 *
 * The carve is mapped at guest-physical 0, so the guest sees a small tidy
 * machine with RAM at the bottom, and everything else in this computer is not
 * "denied" to it - it is not expressible.
 *
 * Only whole large frames INSIDE the mapping are installed.  Rounding up to
 * the next 2 MiB would hand the guest whatever follows the carve in host
 * memory, and it would work perfectly in every test (S-HV-18).  And a 2 MiB
 * leaf must be 2 MiB aligned: an unaligned frame address is a reserved-bit
 * violation, which the machine reports as EPT MISCONFIGURATION (exit 49) - a
 * number that says the tables are malformed and nothing about which field. */
#define EPTP_WALK4        (3u << 3)
#define EPT_LARGE         (1ull << 7)

__attribute__((aligned(4096))) static u64 g_ept_pml4[512];
__attribute__((aligned(4096))) static u64 g_ept_pdpt[512];
__attribute__((aligned(4096))) static u64 g_ept_pd[4][512];
static u64 g_eptp;

static int vmx_map(u64 gpa, u64 hpa, u64 len, unsigned prot, unsigned memtype)
{
    unsigned gb, i;
    if (!len || !uno_vmm_probe()->slat) return 0;
    for (i = 0; i < 512; i++) { g_ept_pml4[i] = 0; g_ept_pdpt[i] = 0; }
    for (gb = 0; gb < 4; gb++) {
        for (i = 0; i < 512; i++) {
            u64 g = ((u64)gb << 30) | ((u64)i << 21);
            g_ept_pd[gb][i] = (g >= gpa && g + 0x200000 <= gpa + len)
                            ? ((hpa + (g - gpa)) | (prot & 7u)
                               | ((u64)(memtype & 7u) << 3) | EPT_LARGE)
                            : 0;
        }
        if (((u64)gb << 30) < gpa + len)
            g_ept_pdpt[gb] = (u64)(unsigned long long)(void *)g_ept_pd[gb] | UNO_VP_RWX;
    }
    g_ept_pml4[0] = (u64)(unsigned long long)(void *)g_ept_pdpt | UNO_VP_RWX;
    g_eptp = (u64)(unsigned long long)(void *)g_ept_pml4
           | (u64)(memtype & 7u) | EPTP_WALK4;
    return 1;
}

/* ---- vmcs_begin: the one place a VMCS is built ----------------------------
 *
 * Every phase used to open with the same forty lines - zero the register
 * context, stamp the revision, vmclear, vmptrld, clear the launch state, lay
 * down the whole guest and host state, then adjust and write PIN, PROC,
 * PROC2, the EPT pointer, CR3, GDTR, IDTR, TR and RSP.  Six copies, differing
 * in about eight values, which is six places for a field to go missing and
 * one machine-check number to tell you about it.  This is that block, once,
 * taking the eight values in `cfg`.
 *
 * ONE VMCS MEANS ONE GUEST.  Arming a second guest vmclears and reconfigures
 * the block the first was using.  That does not fail; it silently replaces a
 * booting Linux with two bytes of `jmp $`, and the only symptom is a kernel
 * that stops saying anything (A6b). */
static int vmcs_begin(const uno_vm_cfg *cfg)
{
    u64 pa = (u64)(unsigned long long)(void *)g_vmcs;
    int tr = (rdmsr(IA32_VMX_BASIC) >> 55) & 1;      /* TRUE ctls available?  */
    u32 pin_want = 0, proc_want = PROC_HLT_EXITING, proc2_want = 0;
    u8 fail = 0;

    /* Intel needs "unrestricted guest" to run a guest with paging off, and
     * that control needs EPT.  A guest that has neither is a guest this
     * backend cannot host, and saying so is what lets A1 fall back to the
     * arrangement the other vendor can. */
    if (cfg->mode != UNO_VM_FLAT64) return 0;
    if ((cfg->features & UNO_VMF_SLAT) && !g_eptp) return 0;

    /* --- make it the current VMCS, unlaunched --- */
    *(u32 *)g_vmcs = (u32)(rdmsr(IA32_VMX_BASIC) & 0x7FFFFFFFu);
    __asm__ volatile ("vmclear %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    __asm__ volatile ("vmptrld %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    g_launched = 0;

    /* --- the controls, and only two words of them are ours ---
     * Everything else in these words is whatever the machine insists on. */
    if (cfg->features & UNO_VMF_PREEMPT)
        pin_want |= PIN_EXT_INTR_EXIT | PIN_PREEMPT;
    if (cfg->features & UNO_VMF_SLAT)    { proc_want |= PROC_SECONDARY;
                                           proc2_want |= PROC2_EPT; }
    if (cfg->features & UNO_VMF_IO_EXIT)  proc_want |= PROC_IO_EXITING;
    /* RDTSCP rides with INVPCID because they share a failure shape (A6b's
     * lesson, met again at A8's Chromium): the CPU has the instruction,
     * CPUID says so, and it still #UDs in a guest until the secondary
     * control asks - which is how a renderer crash-looped with `trap
     * invalid opcode in libhwy` on the instruction AFTER all the SIMD was
     * ruled out: rdtscp, in a profiler nobody suspected. */
    if (cfg->features & UNO_VMF_INVPCID)  proc2_want |= PROC2_INVPCID
                                                     | PROC2_RDTSCP;

    g_proc = adjust(IA32_VMX_TRUE_PROC, IA32_VMX_PROCBASED, proc_want, tr);
    vmwrite(PIN_BASED_CTLS,  adjust(IA32_VMX_TRUE_PIN, IA32_VMX_PINBASED,
                                    pin_want, tr));
    vmwrite(PROC_BASED_CTLS, g_proc);
    if (cfg->features & UNO_VMF_SLAT) {
        vmwrite(PROC_BASED_CTLS2,
                adjust(IA32_VMX_PROCBASED2, IA32_VMX_PROCBASED2, proc2_want, 0));
        vmwrite(EPT_POINTER, g_eptp);
    }
    vmwrite(VM_EXIT_CTLS,    adjust(IA32_VMX_TRUE_EXIT,  IA32_VMX_EXIT_CTLS,
                                    EXIT_HOST_ADDR64, tr));
    vmwrite(VM_ENTRY_CTLS,   adjust(IA32_VMX_TRUE_ENTRY, IA32_VMX_ENTRY_CTLS,
                                    ENTRY_IA32E_GUEST | ENTRY_LOAD_EFER, tr));
    vmwrite(EXCEPTION_BITMAP, 0);
    /* WHICH EXCEPTIONS TO STEAL, and the answer is none.  Trapping #UD/#GP/#PF
     * is what made A6's CR4 fault visible, and then it got in the way twice:
     * the decompressor takes page faults ON PURPOSE (its identity map is built
     * on demand by its own handler), and a kernel far enough in to have an IDT
     * raises #UD deliberately for BUG().  The bitmap earned its keep on the
     * first two faults and is now retired. */
    vmwrite(CR3_TARGET_COUNT, 0);
    vmwrite(VM_EXIT_MSR_STORE_CNT, 0);
    vmwrite(VM_EXIT_MSR_LOAD_CNT, 0);
    vmwrite(VM_ENTRY_MSR_LOAD_CNT, 0);
    vmwrite(VM_ENTRY_INTR_INFO, 0);
    vmwrite(VMCS_LINK_PTR, ~0ull);                   /* MUST be all ones      */

    g_preempt_shift = (unsigned)(rdmsr(IA32_VMX_MISC) & 0x1F);
    g_preempt_on = (cfg->features & UNO_VMF_PREEMPT) ? 1 : 0;

    /* --- host state: where the exit lands ---
     * RSP and RIP are written by the entry stub, because only it knows them. */
    {
        u16 cs, ss, ds, es, fs, gs;
        struct { u16 limit; u64 base; } __attribute__((packed)) gdtr, idtr;
        __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
        __asm__ volatile ("mov %%ss, %0" : "=r"(ss));
        __asm__ volatile ("mov %%ds, %0" : "=r"(ds));
        __asm__ volatile ("mov %%es, %0" : "=r"(es));
        __asm__ volatile ("mov %%fs, %0" : "=r"(fs));
        __asm__ volatile ("mov %%gs, %0" : "=r"(gs));
        __asm__ volatile ("sgdt %0" : "=m"(gdtr));
        __asm__ volatile ("sidt %0" : "=m"(idtr));
        /* The selector fields must have RPL and TI clear, and TR must be
         * non-null. The firmware's TR is usually 0 by the time we are here,
         * and a zero host TR selector is a control-field failure with a
         * number nobody recognises, so it is forced to a plausible one. */
        vmwrite(HOST_CS_SEL, cs & 0xF8);
        vmwrite(HOST_SS_SEL, ss & 0xF8);
        vmwrite(HOST_DS_SEL, ds & 0xF8);
        vmwrite(HOST_ES_SEL, es & 0xF8);
        vmwrite(HOST_FS_SEL, fs & 0xF8);
        vmwrite(HOST_GS_SEL, gs & 0xF8);
        vmwrite(HOST_TR_SEL, 0x08);
        vmwrite(HOST_GDTR_BASE, gdtr.base);
        vmwrite(HOST_IDTR_BASE, idtr.base);
        vmwrite(HOST_TR_BASE, 0);
        vmwrite(HOST_FS_BASE, rdmsr(0xC0000100u));
        vmwrite(HOST_GS_BASE, rdmsr(0xC0000101u));
        vmwrite(HOST_CR0, cr0_read());
        vmwrite(HOST_CR3, cr3_read());
        vmwrite(HOST_CR4, cr4_read());
        vmwrite(HOST_SYSENTER_CS, 0);
    }

    /* --- guest state: long mode, its own tables, its own GDT --- */
    vmwrite(GUEST_CR0, (cr0_read() | rdmsr(IA32_VMX_CR0_FIXED0))
                       & rdmsr(IA32_VMX_CR0_FIXED1));
    vmwrite(GUEST_CR4, (cr4_read() | rdmsr(IA32_VMX_CR4_FIXED0))
                       & rdmsr(IA32_VMX_CR4_FIXED1));
    vmwrite(GUEST_CR3, cfg->cr3);
    vmwrite(GUEST_IA32_EFER, EFER_LME | EFER_LMA);
    vmwrite(GUEST_DR7, 0x400);
    vmwrite(GUEST_RFLAGS, cfg->rflags);
    vmwrite(GUEST_RIP, cfg->rip);
    vmwrite(GUEST_RSP, cfg->rsp);
    vmwrite(GUEST_ACTIVITY, 0);
    vmwrite(GUEST_INTERRUPTIBILITY, 0);
    vmwrite(GUEST_SYSENTER_CS, 0);

    vmwrite(GUEST_CS_SEL, cfg->cs_sel);  vmwrite(GUEST_CS_BASE, 0);
    vmwrite(GUEST_CS_LIMIT, 0xFFFFFFFF); vmwrite(GUEST_CS_AR, 0xA09B);
#define DSEG(s) do { vmwrite(GUEST_##s##_SEL, cfg->ds_sel); \
                     vmwrite(GUEST_##s##_BASE, 0); \
                     vmwrite(GUEST_##s##_LIMIT, 0xFFFFFFFF); \
                     vmwrite(GUEST_##s##_AR, 0xC093); } while (0)
    DSEG(SS); DSEG(DS); DSEG(ES); DSEG(FS); DSEG(GS);
#undef DSEG
    vmwrite(GUEST_LDTR_SEL, 0); vmwrite(GUEST_LDTR_BASE, 0);
    vmwrite(GUEST_LDTR_LIMIT, 0); vmwrite(GUEST_LDTR_AR, 0x10000);  /* unusable */
    vmwrite(GUEST_TR_SEL, 0x18); vmwrite(GUEST_TR_BASE, cfg->tr_base);
    vmwrite(GUEST_TR_LIMIT, 0x67); vmwrite(GUEST_TR_AR, 0x8B);      /* busy TSS */
    vmwrite(GUEST_GDTR_BASE, cfg->gdt_base);
    vmwrite(GUEST_GDTR_LIMIT, cfg->gdt_limit);
    /* An IDT that cannot deliver anything is the crasher's whole mechanism:
     * the first exception becomes a triple fault. */
    vmwrite(GUEST_IDTR_BASE, cfg->idt_base);
    vmwrite(GUEST_IDTR_LIMIT, cfg->idt_limit);

    /* --- the shadow registers, and a kernel cannot boot without them ---
     *
     * VMX requires the guest's CR4 to keep VMXE set at all times - it is in
     * IA32_VMX_CR4_FIXED0. Linux does not know it is a guest and writes CR4
     * with its own idea of the bits, VMXE cleared among them, about a hundred
     * bytes into its entry point. Unshadowed, that write is #GP(0) with
     * nothing to say why, which is precisely where A6 stopped.
     *
     * So VMXE is owned by us: the mask makes a write that changes it exit
     * instead of faulting, and the read shadow makes the guest see the bit as
     * clear, which is the value it expects to read back. The guest keeps
     * every other bit. */
    if (cfg->features & UNO_VMF_CR_SHADOW) {
        vmwrite(CR4_GH_MASK, CR4_VMXE);
        vmwrite(CR4_READ_SHADOW, vmread(GUEST_CR4) & ~CR4_VMXE);
        vmwrite(CR0_GH_MASK, 0);
        vmwrite(CR0_READ_SHADOW, vmread(GUEST_CR0));
    } else {
        vmwrite(CR4_GH_MASK, 0);      vmwrite(CR4_READ_SHADOW, 0);
        vmwrite(CR0_GH_MASK, 0);      vmwrite(CR0_READ_SHADOW, 0);
    }
    return 1;
}

static int vmx_vcpu_create(uno_vcpu *v, const uno_vm_cfg *cfg)
{
    unsigned i;
    if (!vmcs_begin(cfg)) return 0;
    for (i = 0; i < sizeof v->gprs / sizeof(u64); i++) ((u64 *)&v->gprs)[i] = 0;
    v->quiet = 0;
    v->impl = g_vmcs;
    return 1;
}

/* ---- one entry, and what the exit meant ---------------------------------- */

static void classify(uno_vmexit *out)
{
    u64 r = vmread(VM_EXIT_REASON);
    out->raw       = r;
    out->rip       = vmread(GUEST_RIP);
    out->instr_len = (unsigned)vmread(VM_EXIT_INSTR_LEN);
    out->info1     = out->instr_len;
    out->info2     = vmread(VM_INSTR_ERROR);
    if (g_entry_failed) { out->reason = UNO_VX_INVALID; return; }
    if (r & (1ull << 31)) { out->reason = UNO_VX_INVALID; return; }  /* entry failed */
    switch (r & 0xFFFF) {
    case EXIT_CPUID:         out->reason = UNO_VX_CPUID;    break;
    case EXIT_HLT:           out->reason = UNO_VX_HLT;      break;
    case EXIT_TRIPLE_FAULT:  out->reason = UNO_VX_SHUTDOWN; break;
    case EXIT_EXT_INTR:      out->reason = UNO_VX_INTR;     break;
    case EXIT_PREEMPT:       out->reason = UNO_VX_PREEMPT;  break;
    case EXIT_INTR_WINDOW:   out->reason = UNO_VX_INTR_WINDOW; break;
    case EXIT_RDMSR:         out->reason = UNO_VX_RDMSR;    break;
    case EXIT_WRMSR:         out->reason = UNO_VX_WRMSR;    break;
    case EXIT_XSETBV:        out->reason = UNO_VX_XSETBV;   break;
    case EXIT_EPT_VIOLATION:
        /* An EPT violation gives the faulting address and a direction bit,
         * and nothing else - no access size, no register.  Those are only
         * knowable by decoding the instruction, which is the caller's job and
         * is the same job on both vendors. */
        out->reason    = UNO_VX_NPF;
        out->gpa       = vmread(GUEST_PHYS_ADDR);
        out->npf_write = (vmread(EXIT_QUALIFICATION) & 2) ? 1 : 0;
        break;
    case EXIT_IO: {
        u64 q = vmread(EXIT_QUALIFICATION);
        out->reason    = UNO_VX_IO;
        out->io_size   = (unsigned)(q & 7) + 1;
        out->io_in     = (q & 8) ? 1 : 0;
        out->io_string = (q & 0x10) ? 1 : 0;
        out->io_port   = (unsigned)(q >> 16) & 0xFFFF;
        break;
    }
    case EXIT_CR: {
        u64 q = vmread(EXIT_QUALIFICATION);
        out->reason    = UNO_VX_CR;
        out->cr_num    = (unsigned)(q & 15);
        out->cr_access = (unsigned)((q >> 4) & 3);
        out->cr_reg    = (unsigned)((q >> 8) & 15);
        break;
    }
    default:                 out->reason = UNO_VX_UNKNOWN;  break;
    }
}

/* The timer counts down in units of 2^N TSC ticks, N from IA32_VMX_MISC.  A
 * budget converted with the wrong shift is not a wrong answer, it is a slice
 * 32 times too long or too short - the first of which is a visible stall and
 * the second a guest that never progresses. */
static void arm_slice_clock(unsigned budget_us)
{
    /* The rate comes from pc64_native, not the debug harness: this has to
     * work in a production build, where uno_dbg_* is compiled away. */
    u64 per_us = uno_native_tsc_per_us();
    u64 ticks;
    if (!per_us) per_us = 1000ull;              /* uncalibrated: 1 GHz guess  */
    ticks = per_us * budget_us;
    ticks >>= g_preempt_shift;
    if (ticks < 1) ticks = 1;
    if (ticks > 0xFFFFFFFFull) ticks = 0xFFFFFFFFull;
    vmwrite(VMX_PREEMPT_VALUE, ticks);
}

static int vmx_vcpu_run(uno_vcpu *v, unsigned budget_us, uno_vmexit *out)
{
    int swap = g_guest_xcr0 && host_osxsave();
    u64 t0;
    if (g_preempt_on && budget_us) arm_slice_clock(budget_us);
    if (!v->quiet) tracex("[hv] vmentry rip=", vmread(GUEST_RIP));
    if (swap) xsetbv0(g_guest_xcr0);
    t0 = uno_native_rdtsc();
    vmx_entry(&v->gprs);
    /* What the guest actually got, as opposed to what the wall did. The
     * periodic devices count against this; see uno_vmm_guest_cycles. It
     * over-counts by the entry and exit themselves, which is a rounding
     * error against a millisecond slice and errs the safe way (a tick
     * slightly early beats a tick that outruns its handler). */
    uno_vmm_add_guest_cycles(uno_native_rdtsc() - t0);
    if (swap) xsetbv0(g_host_xcr0);
    if (!g_entry_failed) g_launched = 1;
    classify(out);
    if (!v->quiet) tracex("[hv] exit reason=", out->raw);
    return 1;                    /* attempted; *out says how it went          */
}

/* Injection does NOT check whether the guest can take one - VM entry delivers
 * the event regardless, straight through whatever critical section the guest
 * thought it was protecting.  UNO_VR_CAN_INJECT is the question to ask first. */
static void vmx_inject(uno_vcpu *v, unsigned vector, unsigned err, int has_err)
{
    u32 info = VM_ENTRY_INTR_VALID | (vector & 0xFF);
    (void)v;
    if (has_err) {
        info |= (1u << 11);
        vmwrite(VM_ENTRY_INTR_ERROR, err);
    }
    vmwrite(VM_ENTRY_INTR_INFO, info);
}

/* ---- the state window ----------------------------------------------------- */

static u64 vmx_get(uno_vcpu *v, int what)
{
    (void)v;
    switch (what) {
    case UNO_VR_RIP:        return vmread(GUEST_RIP);
    case UNO_VR_RSP:        return vmread(GUEST_RSP);
    case UNO_VR_RFLAGS:     return vmread(GUEST_RFLAGS);
    case UNO_VR_CR0:        return vmread(GUEST_CR0);
    case UNO_VR_CR3:        return vmread(GUEST_CR3);
    case UNO_VR_CR4:        return vmread(GUEST_CR4);
    case UNO_VR_EFER:       return vmread(GUEST_IA32_EFER);
    case UNO_VR_FS_BASE:    return vmread(GUEST_FS_BASE);
    case UNO_VR_GS_BASE:    return vmread(GUEST_GS_BASE);
    case UNO_VR_CR0_SHADOW: return vmread(CR0_READ_SHADOW);
    case UNO_VR_CR4_SHADOW: return vmread(CR4_READ_SHADOW);
    case UNO_VR_INTR_SHADOW: return vmread(GUEST_INTERRUPTIBILITY) & 3;
    case UNO_VR_XCR0:       return g_guest_xcr0;
    case UNO_VR_CAN_INJECT:
        /* Three things say no, and only two of them are the guest's own
         * state: an entry field still valid is an injection that has not been
         * delivered yet, IF clear is a guest in a critical section, and an
         * interrupt shadow is the one instruction after STI or MOV-SS. */
        if (vmread(VM_ENTRY_INTR_INFO) & VM_ENTRY_INTR_VALID) return 0;
        if (!(vmread(GUEST_RFLAGS) & 0x200)) return 0;
        if (vmread(GUEST_INTERRUPTIBILITY) & 3) return 0;
        return 1;
    default:                return 0;
    }
}

static void vmx_set(uno_vcpu *v, int what, u64 val)
{
    (void)v;
    switch (what) {
    case UNO_VR_RIP:        vmwrite(GUEST_RIP, val); break;
    case UNO_VR_RSP:        vmwrite(GUEST_RSP, val); break;
    case UNO_VR_RFLAGS:     vmwrite(GUEST_RFLAGS, val); break;
    case UNO_VR_CR3:        vmwrite(GUEST_CR3, val); break;
    case UNO_VR_EFER:       vmwrite(GUEST_IA32_EFER, val); break;
    case UNO_VR_FS_BASE:    vmwrite(GUEST_FS_BASE, val); break;
    case UNO_VR_GS_BASE:    vmwrite(GUEST_GS_BASE, val); break;
    case UNO_VR_CR0:
        vmwrite(GUEST_CR0, (val | rdmsr(IA32_VMX_CR0_FIXED0))
                           & rdmsr(IA32_VMX_CR0_FIXED1));
        break;
    case UNO_VR_CR4:
        /* VMXE is the bit the machine requires of every guest and the guest
         * must never see set; the shadow below is the other half. */
        vmwrite(GUEST_CR4, val | CR4_VMXE);
        break;
    case UNO_VR_CR0_SHADOW: vmwrite(CR0_READ_SHADOW, val); break;
    case UNO_VR_CR4_SHADOW: vmwrite(CR4_READ_SHADOW, val & ~CR4_VMXE); break;
    case UNO_VR_INTR_SHADOW: {
        u64 s = vmread(GUEST_INTERRUPTIBILITY);
        if ((s & 3) != (val & 3)) vmwrite(GUEST_INTERRUPTIBILITY,
                                          (s & ~3ull) | (val & 3));
        break;
    }
    case UNO_VR_INTR_WINDOW: {
        /* The base has already been through the machine's allowed-0 and
         * allowed-1 masks, so re-adjusting on every entry would be that work
         * repeated thousands of times a second for an answer that cannot
         * change. */
        u32 p = val ? (g_proc | PROC_INTR_WINDOW) : (g_proc & ~PROC_INTR_WINDOW);
        if (p != (u32)vmread(PROC_BASED_CTLS)) vmwrite(PROC_BASED_CTLS, p);
        break;
    }
    case UNO_VR_XCR0:
        if (host_osxsave()) {
            if (!g_host_xcr0) g_host_xcr0 = xgetbv0();
            g_guest_xcr0 = val;
        }
        break;
    default: break;
    }
}

static const uno_hv_t VMX = { "vmx", vmx_enable, vmx_vcpu_create, vmx_vcpu_run,
                              vmx_map, vmx_inject, vmx_get, vmx_set };

const uno_hv_t *uno_hv_vmx(void) { return &VMX; }
