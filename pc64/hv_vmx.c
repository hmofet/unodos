/* ===========================================================================
 * unovirt - the Intel VT-x (VMX) backend.  See unovirt_hv.h, pc64/UNOVIRT.md.
 *
 * The same phase A1 as hv_svm.c: enter host virtualization mode, run a guest,
 * get control back, and prove all three.  Two things differ, and both are
 * forced by the architecture rather than chosen.
 *
 * ONE: the guest is 64-bit, not real mode.  AMD lets a guest start with CR0.PE
 * clear and no tables at all; Intel requires CR0.PE and CR0.PG unless the
 * "unrestricted guest" secondary control is active, and that control requires
 * EPT.  Using it here would drag A2 into A1.  So this guest gets a page table
 * of its own - three pages and a GDT - and runs in long mode from the start.
 * With no EPT its physical addresses are still host physical addresses, so the
 * comment at the top of hv_svm.c applies here word for word: this is bounded
 * only because the guest is eighteen bytes we wrote.
 *
 * TWO: VMX saves and restores NOTHING of the general-purpose file, not even
 * RAX (SVM at least carries RAX in the VMCB).  Host RSP and RIP come out of
 * the VMCS, which is what makes a VM exit land on an instruction we chose.
 * ======================================================================== */
#include "unovirt_hv.h"
#include "unovirt.h"
#include "pc64_native.h" /* the calibrated TSC rate, for the slice budget */
#include "unovdev.h"     /* the device the MMIO decode answers for        */
#include "pc64_fs.h"     /* A6 reads the kernel off the filesystem        */

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
#define IA32_VMX_CR0_FIXED0   0x486u
#define IA32_VMX_CR0_FIXED1   0x487u
#define IA32_VMX_CR4_FIXED0   0x488u
#define IA32_VMX_CR4_FIXED1   0x489u
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
#define PROC_BASED_CTLS2  0x401E
#define EPT_POINTER       0x201A
#define IA32_VMX_PROCBASED2   0x48Bu
#define VM_INSTR_ERROR    0x4400
#define VM_EXIT_REASON    0x4402
#define VM_EXIT_INSTR_LEN 0x440C
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
#define EXIT_EXT_INTR     1
#define EXIT_TRIPLE_FAULT 2
#define EXIT_CPUID        10
#define EXIT_HLT          12
#define EXIT_EPT_VIOLATION 48

#define PROC_HLT_EXITING  (1u << 7)
#define EXIT_HOST_ADDR64  (1u << 9)
#define ENTRY_IA32E_GUEST (1u << 9)
#define ENTRY_LOAD_EFER   (1u << 15)

__attribute__((aligned(4096))) static u8 g_vmxon[4096];
__attribute__((aligned(4096))) static u8 g_vmcs[4096];
__attribute__((aligned(4096))) static u8 g_guest[16384];
__attribute__((aligned(4096))) static u64 g_pml4[512];
__attribute__((aligned(4096))) static u64 g_pdpt[512];
__attribute__((aligned(4096))) static u64 g_pd[512];

#define G_DATA 0x1000            /* the marker lands here                    */
#define G_GDT  0x2000
#define G_TSS  0x3000

static int g_on;
static uno_gprs g_ctx;
static int g_launched;
static volatile u32 g_entry_failed;     /* the stub sets this, see vmx_entry */

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
    cr4 = cr4_read() | (1ull << 13);                     /* CR4.VMXE          */
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

/* ---- a machine for a guest to run on -------------------------------------- */

/* One PD covers a gibibyte, so mapping the whole gibibyte that contains the
 * guest costs three pages and cannot be got wrong at the edges.  Linear equals
 * physical inside it; with no EPT, physical equals HOST physical, which is
 * what makes this an A1 arrangement and not a shippable one. */
static int guest_paging(u64 base)
{
    u64 g1 = base & ~0x3FFFFFFFull;                  /* the gibibyte it is in */
    unsigned i;
    if (((base + sizeof g_guest - 1) & ~0x3FFFFFFFull) != g1) return 0;
    for (i = 0; i < 512; i++) {
        g_pml4[i] = 0;
        g_pdpt[i] = 0;
        g_pd[i]   = (g1 + ((u64)i << 21)) | 0x83;    /* present, rw, 2 MiB    */
    }
    g_pdpt[(g1 >> 30) & 511] = (u64)(unsigned long long)(void *)g_pd   | 0x3;
    g_pml4[(g1 >> 39) & 511] = (u64)(unsigned long long)(void *)g_pdpt | 0x3;
    return 1;
}

static void vmcs_reset(u64 base, u64 rip, u64 idtr_limit)
{
    int tr = (rdmsr(IA32_VMX_BASIC) >> 55) & 1;      /* TRUE ctls available?  */
    u64 *gdt = (u64 *)(g_guest + G_GDT);
    unsigned i;

    for (i = 0; i < sizeof g_guest / 8; i++) ((u64 *)g_guest)[i] = 0;
    gdt[0] = 0;
    gdt[1] = 0x00AF9B000000FFFFull;                  /* 64-bit code, DPL 0    */
    gdt[2] = 0x00CF93000000FFFFull;                  /* data                  */

    /* Controls.  Only two are asked for; everything else in these words is
     * whatever the machine insists on. */
    vmwrite(PIN_BASED_CTLS,  adjust(IA32_VMX_TRUE_PIN,   IA32_VMX_PINBASED,  0, tr));
    vmwrite(PROC_BASED_CTLS, adjust(IA32_VMX_TRUE_PROC,  IA32_VMX_PROCBASED,
                                    PROC_HLT_EXITING, tr));
    vmwrite(VM_EXIT_CTLS,    adjust(IA32_VMX_TRUE_EXIT,  IA32_VMX_EXIT_CTLS,
                                    EXIT_HOST_ADDR64, tr));
    vmwrite(VM_ENTRY_CTLS,   adjust(IA32_VMX_TRUE_ENTRY, IA32_VMX_ENTRY_CTLS,
                                    ENTRY_IA32E_GUEST | ENTRY_LOAD_EFER, tr));
    vmwrite(EXCEPTION_BITMAP, 0);
    vmwrite(CR3_TARGET_COUNT, 0);
    vmwrite(VM_EXIT_MSR_STORE_CNT, 0);
    vmwrite(VM_EXIT_MSR_LOAD_CNT, 0);
    vmwrite(VM_ENTRY_MSR_LOAD_CNT, 0);
    vmwrite(VM_ENTRY_INTR_INFO, 0);
    vmwrite(VMCS_LINK_PTR, ~0ull);                   /* MUST be all ones      */

    /* Host state: where the exit lands.  RSP and RIP are written by the entry
     * stub, because only it knows them. */
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

    /* Guest state: long mode, its own tables, its own GDT. */
    vmwrite(GUEST_CR0, (cr0_read() | rdmsr(IA32_VMX_CR0_FIXED0))
                       & rdmsr(IA32_VMX_CR0_FIXED1));
    vmwrite(GUEST_CR4, (cr4_read() | rdmsr(IA32_VMX_CR4_FIXED0))
                       & rdmsr(IA32_VMX_CR4_FIXED1));
    vmwrite(GUEST_CR3, (u64)(unsigned long long)(void *)g_pml4);
    vmwrite(GUEST_IA32_EFER, EFER_LME | EFER_LMA);
    vmwrite(GUEST_DR7, 0x400);
    vmwrite(GUEST_RFLAGS, 0x2);
    vmwrite(GUEST_RIP, rip);
    vmwrite(GUEST_RSP, base + G_TSS);                /* a page nothing else uses */
    vmwrite(GUEST_ACTIVITY, 0);
    vmwrite(GUEST_INTERRUPTIBILITY, 0);
    vmwrite(GUEST_SYSENTER_CS, 0);

    vmwrite(GUEST_CS_SEL, 0x08);  vmwrite(GUEST_CS_BASE, 0);
    vmwrite(GUEST_CS_LIMIT, 0xFFFFFFFF); vmwrite(GUEST_CS_AR, 0xA09B);
#define DSEG(s) do { vmwrite(GUEST_##s##_SEL, 0x10); vmwrite(GUEST_##s##_BASE, 0); \
                     vmwrite(GUEST_##s##_LIMIT, 0xFFFFFFFF); \
                     vmwrite(GUEST_##s##_AR, 0xC093); } while (0)
    DSEG(SS); DSEG(DS); DSEG(ES); DSEG(FS); DSEG(GS);
#undef DSEG
    vmwrite(GUEST_LDTR_SEL, 0); vmwrite(GUEST_LDTR_BASE, 0);
    vmwrite(GUEST_LDTR_LIMIT, 0); vmwrite(GUEST_LDTR_AR, 0x10000);  /* unusable */
    vmwrite(GUEST_TR_SEL, 0x18); vmwrite(GUEST_TR_BASE, base + G_TSS);
    vmwrite(GUEST_TR_LIMIT, 0x67); vmwrite(GUEST_TR_AR, 0x8B);      /* busy TSS */
    vmwrite(GUEST_GDTR_BASE, base + G_GDT);
    vmwrite(GUEST_GDTR_LIMIT, 0x2F);
    /* The crasher's whole mechanism, as on AMD: an IDT that cannot deliver
     * anything, so the first exception becomes a triple fault. */
    vmwrite(GUEST_IDTR_BASE, base + G_GDT);
    vmwrite(GUEST_IDTR_LIMIT, idtr_limit);
}

/* 64-bit guest, eighteen bytes:
 *   0F A2                 cpuid              -> always exits, we answer
 *   48 BB <imm64>         mov  rbx, &data
 *   89 03                 mov  [rbx], eax
 *   89 53 04              mov  [rbx+4], edx
 *   F4                    hlt                -> exits, the guest is done   */
static void emit_marker(u64 data_va)
{
    u8 *p = g_guest;
    unsigned i;
    p[0] = 0x0F; p[1] = 0xA2;
    p[2] = 0x48; p[3] = 0xBB;
    for (i = 0; i < 8; i++) p[4 + i] = (u8)(data_va >> (8 * i));
    p[12] = 0x89; p[13] = 0x03;
    p[14] = 0x89; p[15] = 0x53; p[16] = 0x04;
    p[17] = 0xF4;
}

static void classify(uno_vmexit *out)
{
    u64 r = vmread(VM_EXIT_REASON);
    out->raw   = r;
    out->rip   = vmread(GUEST_RIP);
    out->info1 = vmread(VM_EXIT_INSTR_LEN);
    out->info2 = vmread(VM_INSTR_ERROR);
    if (g_entry_failed) { out->reason = UNO_VX_INVALID; return; }
    if (r & (1ull << 31)) { out->reason = UNO_VX_INVALID; return; }  /* entry failed */
    switch (r & 0xFFFF) {
    case EXIT_CPUID:         out->reason = UNO_VX_CPUID;    break;
    case EXIT_HLT:           out->reason = UNO_VX_HLT;      break;
    case EXIT_TRIPLE_FAULT:  out->reason = UNO_VX_SHUTDOWN; break;
    case EXIT_EXT_INTR:      out->reason = UNO_VX_INTR;     break;
    case EXIT_EPT_VIOLATION: out->reason = UNO_VX_NPF;      break;
    default:                 out->reason = UNO_VX_UNKNOWN;  break;
    }
}

/* The bring-up trace is per-entry, which is right for a selftest that enters
 * three times and wrong for a slice that enters every frame forever: it floods
 * the console and slows the very thing being measured. */
static int g_quiet;

static void run_once(uno_vmexit *out)
{
    if (!g_quiet) tracex("[hv] vmentry rip=", vmread(GUEST_RIP));
    vmx_entry(&g_ctx);
    if (!g_entry_failed) g_launched = 1;
    classify(out);
    if (!g_quiet) tracex("[hv] exit reason=", out->raw);
}

static int vmcs_load(u64 base, u64 rip, u64 idtr_limit)
{
    u64 pa = (u64)(unsigned long long)(void *)g_vmcs;
    u8 fail = 0;
    *(u32 *)g_vmcs = (u32)(rdmsr(IA32_VMX_BASIC) & 0x7FFFFFFFu);
    __asm__ volatile ("vmclear %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    __asm__ volatile ("vmptrld %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    g_launched = 0;
    if (!guest_paging(base)) return 0;
    vmcs_reset(base, rip, idtr_limit);
    return 1;
}

static int vmx_marker(u64 want, u64 *got, uno_vmexit *last)
{
    u64 base = (u64)(unsigned long long)(void *)g_guest;
    unsigned i;
    int guard;

    for (i = 0; i < sizeof g_ctx / sizeof(u64); i++) ((u64 *)&g_ctx)[i] = 0;
    *got = 0;
    /* RIP is the guest's LINEAR address, and in long mode there is no segment
     * base to fold into it - CS.base must be 0. So unlike the SVM backend,
     * which points a real-mode CS at the code and runs from offset 0, this one
     * enters at the code's actual address. Entering at 0 instead maps nothing,
     * and the instruction fetch faults into an IDT that cannot deliver: exit
     * reason 2, triple fault, at a guest RIP of 0 that looks exactly like a
     * guest which never started. */
    if (!vmcs_load(base, base, 0xFFF)) return 0;
    emit_marker(base + G_DATA);

    for (guard = 0; guard < 8; guard++) {
        run_once(last);
        if (last->reason == UNO_VX_CPUID) {
            g_ctx.rax = want & 0xFFFFFFFFull;
            g_ctx.rdx = (want >> 32) & 0xFFFFFFFFull;
            g_ctx.rbx = 0;
            g_ctx.rcx = 0;
            /* The instruction length comes from the exit, not from counting
             * the bytes ourselves: it is the one number that is right for
             * every encoding of every instruction that can exit here. */
            vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTR_LEN));
            continue;
        }
        if (last->reason == UNO_VX_INTR) continue;
        break;
    }
    if (last->reason != UNO_VX_HLT) return 0;

    *got = (u64)*(u32 *)(g_guest + G_DATA)
         | ((u64)*(u32 *)(g_guest + G_DATA + 4) << 32);
    return *got == want;
}

static int vmx_crasher(uno_vmexit *out)
{
    u64 base = (u64)(unsigned long long)(void *)g_guest;
    unsigned i;
    for (i = 0; i < sizeof g_ctx / sizeof(u64); i++) ((u64 *)&g_ctx)[i] = 0;
    if (!vmcs_load(base, base, 0)) return 0;   /* IDT limit 0: nothing lands */
    g_guest[0] = 0x0F; g_guest[1] = 0x0B;      /* ud2                        */
    run_once(out);
    return out->reason == UNO_VX_SHUTDOWN || out->reason == UNO_VX_INVALID;
}

/* ---- A2: second-stage translation, and a guest that only sees its own -----
 *
 * With EPT on, the guest's physical addresses stop being this machine's.  The
 * carve is mapped at guest-physical 0, so the guest sees a small tidy machine
 * with RAM at the bottom, and everything else in this computer is not
 * "denied" to it - it is not expressible.
 *
 * The guest's own page tables now live in the carve at guest-physical
 * addresses, so CR3 is a GPA and the host writes them through uno_vmm_gpa().
 * That is the whole difference from A1, where CR3 was a host address because
 * the two were the same thing. */
#define EPT_PROC2         (1u << 1)
#define EPTP_WB           6u
#define EPTP_WALK4        (3u << 3)
#define EPT_RWX           0x7ull
#define EPT_PAGE_WB       (6ull << 3)
#define EPT_LARGE         (1ull << 7)

/* Guest-physical layout inside the carve.  Deliberately low and round: an
 * address like 0x100000 is obviously the guest's own, and if the second stage
 * were off the same store would land on the kernel's low memory, where it
 * would be noticed as something other than a passing test. */
#define GP_PML4  0x1000
#define GP_PDPT  0x2000
#define GP_PD    0x3000
#define GP_PD2   0x6000        /* the gibibyte the devices live in         */
#define GP_GDT   0x4000
#define GP_STACK 0x8000
#define GP_CODE  0x10000

__attribute__((aligned(4096))) static u64 g_ept_pml4[512];
__attribute__((aligned(4096))) static u64 g_ept_pdpt[512];
__attribute__((aligned(4096))) static u64 g_ept_pd[4][512];

static u64 ept_build(void)
{
    u64 base = uno_vmm_carve_base(), size = uno_vmm_carve_size();
    unsigned gb, i;
    if (!base) return 0;
    for (i = 0; i < 512; i++) { g_ept_pml4[i] = 0; g_ept_pdpt[i] = 0; }
    for (gb = 0; gb < 4; gb++) {
        for (i = 0; i < 512; i++) {
            u64 gpa = ((u64)gb << 30) | ((u64)i << 21);
            /* Only what the carve actually covers is mapped.  A guest reading
             * past the end of its own memory takes an EPT violation, which is
             * a reportable exit; leaving the tail mapped at whatever follows
             * the carve in host memory would make it a silent success. */
            g_ept_pd[gb][i] = (gpa + 0x200000 <= size)
                            ? ((base + gpa) | EPT_RWX | EPT_PAGE_WB | EPT_LARGE)
                            : 0;
        }
        if (((u64)gb << 30) < size)
            g_ept_pdpt[gb] = (u64)(unsigned long long)(void *)g_ept_pd[gb] | EPT_RWX;
    }
    g_ept_pml4[0] = (u64)(unsigned long long)(void *)g_ept_pdpt | EPT_RWX;
    return (u64)(unsigned long long)(void *)g_ept_pml4 | EPTP_WB | EPTP_WALK4;
}

/* The guest's stage-1 tables, written into the carve at guest-physical
 * addresses.  Identity again, so guest linear equals guest physical - which
 * keeps the interesting translation the one being tested. */
static int guest_tables_in_carve(void)
{
    u64 *pml4 = (u64 *)uno_vmm_gpa(GP_PML4, 4096);
    u64 *pdpt = (u64 *)uno_vmm_gpa(GP_PDPT, 4096);
    u64 *pd   = (u64 *)uno_vmm_gpa(GP_PD,   4096);
    u64 *pd2  = (u64 *)uno_vmm_gpa(GP_PD2,  4096);
    u64 *gdt  = (u64 *)uno_vmm_gpa(GP_GDT,  4096);
    unsigned i;
    if (!pml4 || !pdpt || !pd || !pd2 || !gdt) return 0;
    for (i = 0; i < 512; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
        pd[i]   = ((u64)i << 21) | 0x83;         /* present, rw, 2 MiB       */
        /* The fourth gibibyte, where the devices are. THE GUEST'S OWN TABLES
         * HAVE TO MAP IT: a device access is meant to fault in stage TWO, and
         * a guest whose stage-one mapping is missing never gets that far - it
         * takes a page fault into an IDT it has not built, and the result is
         * a triple fault at an address that has nothing to do with the
         * device. That is exactly how this arrived. */
        pd2[i]  = (3ull << 30) | ((u64)i << 21) | 0x83;
    }
    pdpt[0] = GP_PD   | 0x3;                     /* GPAs, not host addresses */
    pdpt[3] = GP_PD2  | 0x3;
    pml4[0] = GP_PDPT | 0x3;
    gdt[0] = 0;
    gdt[1] = 0x00AF9B000000FFFFull;
    gdt[2] = 0x00CF93000000FFFFull;
    return 1;
}

static int vmx_ept(u64 want, u64 gpa, u64 *got, u64 *hpa, uno_vmexit *last)
{
    u64 eptp, pa = (u64)(unsigned long long)(void *)g_vmcs;
    u8 *code = (u8 *)uno_vmm_gpa(GP_CODE, 64);
    u32 *cell = (u32 *)uno_vmm_gpa(gpa, 8);
    u32 proc, proc2;
    u8 fail = 0;
    unsigned i;
    int guard;

    *got = 0;
    *hpa = 0;
    if (!code || !cell) return 0;
    if (!uno_vmm_probe()->slat) return 0;
    eptp = ept_build();
    if (!eptp) return 0;
    if (!guest_tables_in_carve()) return 0;
    *hpa = (u64)(unsigned long long)cell;
    cell[0] = 0; cell[1] = 0;

    for (i = 0; i < sizeof g_ctx / sizeof(u64); i++) ((u64 *)&g_ctx)[i] = 0;
    *(u32 *)g_vmcs = (u32)(rdmsr(IA32_VMX_BASIC) & 0x7FFFFFFFu);
    __asm__ volatile ("vmclear %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    __asm__ volatile ("vmptrld %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    g_launched = 0;

    vmcs_reset(0, GP_CODE, 0xFFF);               /* segment bases 0, GPA rip */
    /* EPT lives in the secondary controls, which have to be activated in the
     * primary word first - and both go through the same allowed-0/allowed-1
     * adjustment as everything else. */
    proc  = adjust(IA32_VMX_TRUE_PROC, IA32_VMX_PROCBASED,
                   PROC_HLT_EXITING | (1u << 31), 1);
    proc2 = adjust(IA32_VMX_PROCBASED2, IA32_VMX_PROCBASED2, EPT_PROC2, 0);
    vmwrite(PROC_BASED_CTLS, proc);
    vmwrite(PROC_BASED_CTLS2, proc2);
    vmwrite(EPT_POINTER, eptp);
    vmwrite(GUEST_CR3, GP_PML4);                 /* a GUEST-physical address */
    vmwrite(GUEST_GDTR_BASE, GP_GDT);
    vmwrite(GUEST_IDTR_BASE, GP_GDT);
    vmwrite(GUEST_TR_BASE, GP_STACK);
    vmwrite(GUEST_RSP, GP_STACK);

    /* The same eighteen bytes as A1, at a guest-physical address this time. */
    {   u8 *p = code;
        p[0] = 0x0F; p[1] = 0xA2;
        p[2] = 0x48; p[3] = 0xBB;
        for (i = 0; i < 8; i++) p[4 + i] = (u8)(gpa >> (8 * i));
        p[12] = 0x89; p[13] = 0x03;
        p[14] = 0x89; p[15] = 0x53; p[16] = 0x04;
        p[17] = 0xF4;
    }

    for (guard = 0; guard < 8; guard++) {
        run_once(last);
        if (last->reason == UNO_VX_CPUID) {
            g_ctx.rax = want & 0xFFFFFFFFull;
            g_ctx.rdx = (want >> 32) & 0xFFFFFFFFull;
            vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTR_LEN));
            continue;
        }
        if (last->reason == UNO_VX_INTR) continue;
        break;
    }
    if (last->reason != UNO_VX_HLT) return 0;
    *got = (u64)cell[0] | ((u64)cell[1] << 32);
    return *got == want;
}

/* ---- A3: a guest that never yields, and a core that comes back anyway -----
 *
 * The guest is two bytes, `jmp $`. It makes no hypercall, takes no fault and
 * touches no device, so nothing it does can end its turn: the only thing that
 * can is the machine, and that is exactly the property being tested. This is
 * how a guest gets scheduled on an OS that has no scheduler - the frame loop
 * hands it a budget and the preemption timer takes it back.
 *
 * TWO mechanisms, not one, and the second is not redundancy. The preemption
 * timer bounds the slice; external-interrupt exiting means a host interrupt
 * that arrives mid-slice ends it too, rather than being delivered through the
 * guest's IDT - which this guest does not have, and which would turn every
 * timer tick into a triple fault. */
#define PIN_EXT_INTR_EXIT (1u << 0)
#define PIN_PREEMPT       (1u << 6)
#define VMX_PREEMPT_VALUE 0x482E
#define IA32_VMX_MISC     0x485u
#define EXIT_PREEMPT      52

static int g_spinning;

static int vmx_spin_start(void)
{
    u64 eptp, pa = (u64)(unsigned long long)(void *)g_vmcs;
    u8 *code = (u8 *)uno_vmm_gpa(GP_CODE, 8);
    u8 fail = 0;
    unsigned i;
    u32 proc, proc2, pin;

    g_spinning = 0;
    if (!code || !uno_vmm_probe()->slat || !uno_vmm_probe()->preempt_timer) return 0;
    eptp = ept_build();
    if (!eptp || !guest_tables_in_carve()) return 0;

    for (i = 0; i < sizeof g_ctx / sizeof(u64); i++) ((u64 *)&g_ctx)[i] = 0;
    *(u32 *)g_vmcs = (u32)(rdmsr(IA32_VMX_BASIC) & 0x7FFFFFFFu);
    __asm__ volatile ("vmclear %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    __asm__ volatile ("vmptrld %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    g_launched = 0;

    vmcs_reset(0, GP_CODE, 0xFFF);
    pin   = adjust(IA32_VMX_TRUE_PIN, IA32_VMX_PINBASED,
                   PIN_EXT_INTR_EXIT | PIN_PREEMPT, 1);
    if (!(pin & PIN_PREEMPT)) return 0;        /* the machine said no         */
    proc  = adjust(IA32_VMX_TRUE_PROC, IA32_VMX_PROCBASED,
                   PROC_HLT_EXITING | (1u << 31), 1);
    proc2 = adjust(IA32_VMX_PROCBASED2, IA32_VMX_PROCBASED2, EPT_PROC2, 0);
    vmwrite(PIN_BASED_CTLS, pin);
    vmwrite(PROC_BASED_CTLS, proc);
    vmwrite(PROC_BASED_CTLS2, proc2);
    vmwrite(EPT_POINTER, eptp);
    vmwrite(GUEST_CR3, GP_PML4);
    vmwrite(GUEST_GDTR_BASE, GP_GDT);
    vmwrite(GUEST_IDTR_BASE, GP_GDT);
    vmwrite(GUEST_TR_BASE, GP_STACK);
    vmwrite(GUEST_RSP, GP_STACK);
    code[0] = 0xEB; code[1] = 0xFE;             /* jmp $                      */
    g_spinning = 1;
    return 1;
}

/* The timer counts down in units of 2^N TSC ticks, N from IA32_VMX_MISC.  A
 * budget converted with the wrong shift is not a wrong answer, it is a slice
 * 32 times too long or too short - the first of which is a visible stall and
 * the second a guest that never progresses. */
static int vmx_slice(unsigned budget_us, uno_vmexit *out)
{
    u64 per_us = uno_native_tsc_per_us();
    unsigned shift = (unsigned)(rdmsr(IA32_VMX_MISC) & 0x1F);
    u64 ticks;
    if (!g_spinning) return 0;
    /* The rate comes from pc64_native, not the debug harness: this has to
     * work in a production build, where uno_dbg_* is compiled away. */
    if (!per_us) per_us = 1000ull;              /* uncalibrated: 1 GHz guess  */
    ticks = per_us * budget_us;
    ticks >>= shift;
    if (ticks < 1) ticks = 1;
    if (ticks > 0xFFFFFFFFull) ticks = 0xFFFFFFFFull;
    vmwrite(VMX_PREEMPT_VALUE, ticks);
    run_once(out);
    g_quiet = 1;                  /* the first slice is traced, the rest are not */
    if (out->raw == EXIT_PREEMPT) out->reason = UNO_VX_PREEMPT;
    return 1;
}

/* ---- A4: a clock, an interrupt, and an MSR space --------------------------
 *
 * These are the three things a guest needs before it can be an operating
 * system rather than a program. Linux asks for all three in its first
 * milliseconds: it reads a counter, it installs an IDT and expects timer
 * interrupts through it, and it reads MSRs constantly.
 *
 * One guest exercises all three, and the shape of the proof matters more than
 * the numbers. The clock is sampled ACROSS two slices, so a guest that merely
 * ran once cannot pass - it has to keep making progress. The interrupt is
 * counted BY THE GUEST in its own memory, through its own vector table, so
 * nothing about the delivery is taken on trust. And the count is checked again
 * after further slices, because an interrupt that is delivered forever is a
 * different bug from one that is never delivered and looks identical for the
 * first millisecond.
 */
#define GP_IDT   0x5000
#define GP_TSS2  0x9000
#define GP_CELLS 0x100000        /* [0] tsc, [1] msr echo, [2] irq counter   */
#define A4_VECTOR 0x20
#define A4_MSR   0x1234ABCDu     /* an index nothing real uses               */
#define A4_MSR_ANSWER 0x5A5A5A5Au
#define EXIT_RDMSR 31
#define EXIT_WRMSR 32
#define VM_ENTRY_INTR_VALID 0x80000000u

/* The guest, hand-assembled because eighteen instructions do not justify a
 * second toolchain in the build. Offsets are load-bearing and commented.
 *
 *   00: 48 BB <cells>   mov  rbx, 0x100000     ; its own memory
 *   0A: 0F 31           rdtsc                  ; NOT intercepted: it reads
 *   0C: 89 03           mov  [rbx], eax        ;   the real counter directly
 *   0E: B9 <msr>        mov  ecx, 0x1234ABCD
 *   13: 0F 32           rdmsr                  ; -> exit 31, we answer
 *   15: 89 53 04        mov  [rbx+4], edx      ; the answer, in its memory
 *   18: EB F0           jmp  0x0A              ; forever
 *
 *   handler, at +0x100:
 *   FF 05 <disp32>      incl [rip+disp]        ; count it, in its own memory
 *   48 CF               iretq
 */
static int a4_emit(void)
{
    u8 *p = (u8 *)uno_vmm_gpa(GP_CODE, 0x110);
    u8 *h;
    unsigned i;
    u64 counter = GP_CELLS + 8;
    long disp;
    if (!p) return 0;
    for (i = 0; i < 0x110; i++) p[i] = 0;

    p[0] = 0x48; p[1] = 0xBB;
    for (i = 0; i < 8; i++) p[2 + i] = (u8)((u64)GP_CELLS >> (8 * i));
    p[0x0A] = 0x0F; p[0x0B] = 0x31;
    p[0x0C] = 0x89; p[0x0D] = 0x03;
    p[0x0E] = 0xB9;
    for (i = 0; i < 4; i++) p[0x0F + i] = (u8)(A4_MSR >> (8 * i));
    p[0x13] = 0x0F; p[0x14] = 0x32;
    p[0x15] = 0x89; p[0x16] = 0x53; p[0x17] = 0x04;
    p[0x18] = 0xEB; p[0x19] = (u8)(signed char)(0x0A - 0x1A);

    h = p + 0x100;
    /* rip-relative displacement is measured from the END of the instruction,
     * which is six bytes on from its start. */
    disp = (long)(counter - (GP_CODE + 0x100 + 6));
    h[0] = 0xFF; h[1] = 0x05;
    for (i = 0; i < 4; i++) h[2 + i] = (u8)((unsigned long)disp >> (8 * i));
    h[6] = 0x48; h[7] = 0xCF;
    return 1;
}

static int a4_idt(void)
{
    u8 *idt = (u8 *)uno_vmm_gpa(GP_IDT, 4096);
    u64 off = GP_CODE + 0x100;
    u8 *e;
    unsigned i;
    if (!idt) return 0;
    for (i = 0; i < 4096; i++) idt[i] = 0;
    e = idt + A4_VECTOR * 16;
    e[0] = (u8)off;  e[1] = (u8)(off >> 8);
    e[2] = 0x08;     e[3] = 0x00;              /* the guest's own code sel   */
    e[4] = 0;                                  /* IST 0: the current stack   */
    e[5] = 0x8E;                               /* present, DPL 0, int gate   */
    e[6] = (u8)(off >> 16); e[7] = (u8)(off >> 24);
    for (i = 0; i < 4; i++) e[8 + i] = (u8)(off >> (32 + 8 * i));
    return 1;
}

/* One entry, with whatever servicing the exit needs.  Returns 0 when the
 * guest is no longer runnable, which is how a wrong turn ends the test
 * instead of the machine. */
static int a4_step(uno_vmexit *ex)
{
    u64 per_us = uno_native_tsc_per_us();
    unsigned shift = (unsigned)(rdmsr(IA32_VMX_MISC) & 0x1F);
    u64 ticks;
    if (!per_us) per_us = 1000ull;
    ticks = (per_us * 2000ull) >> shift;         /* 2 ms is plenty here      */
    if (!ticks) ticks = 1;
    vmwrite(VMX_PREEMPT_VALUE, ticks);
    run_once(ex);
    switch (ex->raw) {
    case EXIT_RDMSR:
        /* The guest asked for an MSR and we are its entire MSR space. With no
         * MSR bitmap every access exits, which is correct for a guest this
         * size and wrong for Linux - A5 wants a bitmap so the hot ones do not
         * trap. */
        g_ctx.rax = 0;
        g_ctx.rdx = A4_MSR_ANSWER;
        vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTR_LEN));
        return 1;
    case EXIT_CPUID:
        g_ctx.rax = 0;
        vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTR_LEN));
        return 1;
    case EXIT_PREEMPT:
    case EXIT_EXT_INTR:
        return 1;
    default:
        return 0;                                 /* anything else is a bug */
    }
}

static int vmx_clockirq(uno_vm_clockirq *out)
{
    u64 eptp, pa = (u64)(unsigned long long)(void *)g_vmcs;
    volatile u32 *cells;
    uno_vmexit ex;
    u8 fail = 0;
    unsigned i;
    u32 proc, proc2, pin;
    int n;

    out->t1 = out->t2 = out->msr_echo = 0;
    out->irqs = out->redelivered = out->exits = 0;

    cells = (volatile u32 *)uno_vmm_gpa(GP_CELLS, 16);
    if (!cells || !uno_vmm_probe()->slat || !uno_vmm_probe()->preempt_timer) return 0;
    eptp = ept_build();
    if (!eptp || !guest_tables_in_carve()) return 0;
    cells[0] = 0; cells[1] = 0; cells[2] = 0;

    for (i = 0; i < sizeof g_ctx / sizeof(u64); i++) ((u64 *)&g_ctx)[i] = 0;
    *(u32 *)g_vmcs = (u32)(rdmsr(IA32_VMX_BASIC) & 0x7FFFFFFFu);
    __asm__ volatile ("vmclear %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    __asm__ volatile ("vmptrld %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    g_launched = 0;

    vmcs_reset(0, GP_CODE, 0xFFF);
    pin   = adjust(IA32_VMX_TRUE_PIN, IA32_VMX_PINBASED,
                   PIN_EXT_INTR_EXIT | PIN_PREEMPT, 1);
    proc  = adjust(IA32_VMX_TRUE_PROC, IA32_VMX_PROCBASED,
                   PROC_HLT_EXITING | (1u << 31), 1);
    proc2 = adjust(IA32_VMX_PROCBASED2, IA32_VMX_PROCBASED2, EPT_PROC2, 0);
    vmwrite(PIN_BASED_CTLS, pin);
    vmwrite(PROC_BASED_CTLS, proc);
    vmwrite(PROC_BASED_CTLS2, proc2);
    vmwrite(EPT_POINTER, eptp);
    vmwrite(GUEST_CR3, GP_PML4);
    vmwrite(GUEST_GDTR_BASE, GP_GDT);
    vmwrite(GUEST_TR_BASE, GP_TSS2);
    vmwrite(GUEST_RSP, GP_STACK);
    /* Its own vector table, and interrupts open. A guest with IF clear cannot
     * take the injection, and the failure looks exactly like an injection that
     * never happened. */
    vmwrite(GUEST_IDTR_BASE, GP_IDT);
    vmwrite(GUEST_IDTR_LIMIT, 0xFFF);
    vmwrite(GUEST_RFLAGS, 0x202);
    if (!a4_emit() || !a4_idt()) return 0;

    /* Phase 1 and 2: does its clock advance ACROSS slices? */
    for (n = 0; n < 40; n++) { if (!a4_step(&ex)) return 0; out->exits++; }
    out->t1 = cells[0];
    for (n = 0; n < 40; n++) { if (!a4_step(&ex)) return 0; out->exits++; }
    out->t2 = cells[0];
    out->msr_echo = cells[1];

    /* Phase 3: hand it an interrupt it never asked for and did not raise. */
    vmwrite(VM_ENTRY_INTR_INFO, VM_ENTRY_INTR_VALID | A4_VECTOR);
    for (n = 0; n < 20; n++) { if (!a4_step(&ex)) return 0; out->exits++; }
    out->irqs = (int)cells[2];

    /* Phase 4: and does it STAY delivered once, rather than forever? */
    for (n = 0; n < 40; n++) { if (!a4_step(&ex)) return 0; out->exits++; }
    out->redelivered = ((int)cells[2] != out->irqs);
    return out->t2 > out->t1
        && out->msr_echo == A4_MSR_ANSWER
        && out->irqs == 1
        && !out->redelivered;
}

/* ---- A5: MMIO, and the thing x86 does not give you ------------------------
 *
 * On ARM a stage-2 abort hands the hypervisor a syndrome register that names
 * the access size, the direction and WHICH REGISTER the guest used. Glide's
 * trap-and-emulate is a switch on those fields. x86 gives an EPT violation
 * with the faulting guest-physical address, a direction bit, and nothing
 * else: the register and the operand size are only knowable by DECODING THE
 * INSTRUCTION. That is why KVM carries an x86 emulator, and it is the single
 * biggest structural difference between this port and Glide's.
 *
 * What saves it from being an emulator is scope. A device driver's MMIO
 * accesses are `mov` between a register and memory - that is what `readl` and
 * `writel` compile to on every compiler anyone uses. So this decodes exactly
 * the four MOV forms and refuses everything else, loudly, rather than
 * guessing. An unrecognised opcode is a reportable exit, not a wrong answer.
 *
 * The instruction LENGTH does not have to be decoded: VMX supplies it for
 * EPT violations, which removes the part of decoding that is genuinely hard. */
#define EXIT_EPT_MISCONFIG 49
#define GUEST_PHYS_ADDR    0x2400
#define EXIT_QUALIFICATION 0x6400

/* Walk the guest's own page tables to turn a guest LINEAR address into a
 * guest-physical one. A1..A4's guests are identity-mapped, so this is the
 * identity for them; Linux is not, and this is what stops that being a
 * rewrite later. Every table read goes through the bounds seam. */
static int guest_walk(u64 lin, u64 *gpa)
{
    u64 cr3 = vmread(GUEST_CR3) & ~0xFFFull;
    int level;
    u64 tbl = cr3;
    for (level = 39; level >= 12; level -= 9) {
        const u64 *t = (const u64 *)uno_vmm_gpa(tbl, 4096);
        u64 e;
        if (!t) return 0;
        e = t[(lin >> level) & 511];
        if (!(e & 1)) return 0;                    /* not present            */
        if ((e & 0x80) && level > 12) {            /* a large page ends it   */
            u64 mask = (1ull << level) - 1;
            *gpa = (e & ~mask & 0x000FFFFFFFFFF000ull) | (lin & mask);
            return 1;
        }
        tbl = e & 0x000FFFFFFFFFF000ull;
        if (level == 12) { *gpa = tbl | (lin & 0xFFF); return 1; }
    }
    return 0;
}

/* The four forms, and what each one means:
 *   88 /r  mov r/m8,  r8      store, 1 byte
 *   89 /r  mov r/m32, r32     store, operand size
 *   8A /r  mov r8,  r/m8      load,  1 byte
 *   8B /r  mov r32, r/m32     load,  operand size
 * Everything before the opcode is prefixes; everything after is addressing we
 * do not need, because the faulting ADDRESS already came from the CPU. */
static int decode_mov(const u8 *ins, int *reg, unsigned *size, int *is_store)
{
    int i = 0, rex = 0, opsz = 4;
    for (; i < 8; i++) {
        u8 b = ins[i];
        if (b == 0x66) { opsz = 2; continue; }
        if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) continue;
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65) continue;
        if ((b & 0xF0) == 0x40) { rex = b; if (b & 8) opsz = 8; continue; }
        break;
    }
    switch (ins[i]) {
    case 0x88: *is_store = 1; *size = 1; break;
    case 0x8A: *is_store = 0; *size = 1; break;
    case 0x89: *is_store = 1; *size = (unsigned)opsz; break;
    case 0x8B: *is_store = 0; *size = (unsigned)opsz; break;
    default: return 0;
    }
    *reg = ((ins[i + 1] >> 3) & 7) | ((rex & 4) ? 8 : 0);
    return 1;
}

/* The guest's register file, addressed the way an instruction encoding does.
 * RSP is index 4 and lives in the VMCS rather than the context, which is the
 * one hole a table like this always has. */
static u64 *gpr(int n)
{
    u64 *c = (u64 *)&g_ctx;
    static const int map[16] = { 0, 2, 3, 1, -1, 6, 5, 4,
                                 7, 8, 9, 10, 11, 12, 13, 14 };
    /* rax rcx rdx rbx rsp rbp rsi rdi r8..r15 -> uno_gprs order */
    if (n < 0 || n > 15 || map[n] < 0) return 0;
    return c + map[n];
}

/* Answer one EPT violation as the device would, then step the guest past the
 * instruction. Returns 0 when it is not a device access or not a form we
 * decode - both of which end the guest with a report rather than a guess. */
static int mmio_service(void)
{
    u64 gpa = vmread(GUEST_PHYS_ADDR);
    u64 qual = vmread(EXIT_QUALIFICATION);
    u64 rip = vmread(GUEST_RIP), ins_gpa = 0, val = 0;
    const u8 *ins;
    int reg = 0, is_store = 0, decoded_store = 0;
    unsigned size = 4;
    u64 *r;

    if (!guest_walk(rip, &ins_gpa)) return 0;
    ins = (const u8 *)uno_vmm_gpa(ins_gpa, 16);
    if (!ins) return 0;
    if (!decode_mov(ins, &reg, &size, &decoded_store)) return 0;
    is_store = (qual & 2) ? 1 : 0;
    if (is_store != decoded_store) return 0;      /* the two must agree      */
    r = gpr(reg);
    if (!r) return 0;

    if (is_store) {
        val = *r;
        if (size == 1) val &= 0xFF;
        else if (size == 2) val &= 0xFFFF;
        else if (size == 4) val &= 0xFFFFFFFFull;
        if (!uno_vdev_mmio(gpa, 1, size, &val)) return 0;
    } else {
        if (!uno_vdev_mmio(gpa, 0, size, &val)) return 0;
        /* A 32-bit destination zero-extends into the top half, exactly as the
         * instruction would have. Writing only the low half leaves the guest
         * with whatever it had up there, which is a bug that appears only
         * once a value crosses 4 GiB. */
        if (size == 4) *r = val & 0xFFFFFFFFull;
        else if (size == 2) *r = (*r & ~0xFFFFull) | (val & 0xFFFF);
        else if (size == 1) *r = (*r & ~0xFFull) | (val & 0xFF);
        else *r = val;
    }
    vmwrite(GUEST_RIP, rip + vmread(VM_EXIT_INSTR_LEN));
    return 1;
}

/* A5's guest: read the transport's identity, ring the doorbell, then read
 * back what the device wrote into the guest's own used ring.
 *
 *   00: 48 BB <mmio>   mov rbx, 0xD0000000
 *   0A: 48 B9 <cells>  mov rcx, 0x100000
 *   14: 8B 03          mov eax, [rbx]        ; MagicValue  -> a load exit
 *   16: 89 01          mov [rcx], eax
 *   18: 31 C0          xor eax, eax
 *   1A: 89 43 50       mov [rbx+0x50], eax   ; QueueNotify -> a store exit
 *   1D: 48 BA <used>   mov rdx, 0x202000
 *   27: 8B 42 02       mov eax, [rdx+2]      ; used.idx, written by the device
 *   2A: 89 41 04       mov [rcx+4], eax
 *   2D: F4             hlt
 */
#define GP_VQ_DESC  0x200000
#define GP_VQ_AVAIL 0x201000
#define GP_VQ_USED  0x202000
#define GP_VQ_BUF   0x203000
#define VQ_MSG "a message through a virtqueue"

static int a5_emit(void)
{
    u8 *p = (u8 *)uno_vmm_gpa(GP_CODE, 0x40);
    unsigned i;
    if (!p) return 0;
    for (i = 0; i < 0x40; i++) p[i] = 0;
    p[0] = 0x48; p[1] = 0xBB;
    for (i = 0; i < 8; i++) p[2 + i] = (u8)(uno_vdev_base() >> (8 * i));
    p[0x0A] = 0x48; p[0x0B] = 0xB9;
    for (i = 0; i < 8; i++) p[0x0C + i] = (u8)((u64)GP_CELLS >> (8 * i));
    p[0x14] = 0x8B; p[0x15] = 0x03;
    p[0x16] = 0x89; p[0x17] = 0x01;
    p[0x18] = 0x31; p[0x19] = 0xC0;
    p[0x1A] = 0x89; p[0x1B] = 0x43; p[0x1C] = 0x50;
    p[0x1D] = 0x48; p[0x1E] = 0xBA;
    for (i = 0; i < 8; i++) p[0x1F + i] = (u8)((u64)GP_VQ_USED >> (8 * i));
    p[0x27] = 0x8B; p[0x28] = 0x42; p[0x29] = 0x02;
    p[0x2A] = 0x89; p[0x2B] = 0x41; p[0x2C] = 0x04;
    p[0x2D] = 0xF4;
    return 1;
}

/* The rings, placed on the guest's behalf.  A6's guest is Linux and builds
 * its own; what is being tested here is the DEVICE walking them. */
static int a5_rings(void)
{
    u8 *d = (u8 *)uno_vmm_gpa(GP_VQ_DESC, 64);
    u8 *a = (u8 *)uno_vmm_gpa(GP_VQ_AVAIL, 64);
    u8 *u = (u8 *)uno_vmm_gpa(GP_VQ_USED, 64);
    char *b = (char *)uno_vmm_gpa(GP_VQ_BUF, 64);
    const char *m = VQ_MSG;
    unsigned n = 0, i;
    if (!d || !a || !u || !b) return 0;
    while (m[n]) { b[n] = m[n]; n++; }
    for (i = 0; i < 64; i++) { d[i] = 0; a[i] = 0; u[i] = 0; }
    *(u64 *)(d + 0) = GP_VQ_BUF;      /* desc[0].addr                        */
    *(u32 *)(d + 8) = n;              /* desc[0].len                         */
    *(u16 *)(a + 2) = 1;              /* avail.idx = 1                       */
    *(u16 *)(a + 4) = 0;              /* avail.ring[0] = descriptor 0        */
    uno_vdev_queue(GP_VQ_DESC, GP_VQ_AVAIL, GP_VQ_USED, 8);
    return (int)n;
}

static int vmx_virtio(uno_vm_virtio *out)
{
    u64 eptp, pa = (u64)(unsigned long long)(void *)g_vmcs;
    volatile u32 *cells;
    uno_vmexit ex;
    u8 fail = 0;
    unsigned i;
    u32 proc, proc2, pin;
    int n, want;

    out->magic = out->used_idx = 0;
    out->bytes = out->notifies = out->cycle_refused = 0;
    out->text = "";

    cells = (volatile u32 *)uno_vmm_gpa(GP_CELLS, 16);
    if (!cells || !uno_vmm_probe()->slat) return 0;
    eptp = ept_build();
    if (!eptp || !guest_tables_in_carve()) return 0;
    cells[0] = 0; cells[1] = 0;
    uno_vdev_reset();
    want = a5_rings();
    if (want <= 0 || !a5_emit()) return 0;

    for (i = 0; i < sizeof g_ctx / sizeof(u64); i++) ((u64 *)&g_ctx)[i] = 0;
    *(u32 *)g_vmcs = (u32)(rdmsr(IA32_VMX_BASIC) & 0x7FFFFFFFu);
    __asm__ volatile ("vmclear %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    __asm__ volatile ("vmptrld %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    g_launched = 0;

    vmcs_reset(0, GP_CODE, 0xFFF);
    pin   = adjust(IA32_VMX_TRUE_PIN, IA32_VMX_PINBASED,
                   PIN_EXT_INTR_EXIT | PIN_PREEMPT, 1);
    proc  = adjust(IA32_VMX_TRUE_PROC, IA32_VMX_PROCBASED,
                   PROC_HLT_EXITING | (1u << 31), 1);
    proc2 = adjust(IA32_VMX_PROCBASED2, IA32_VMX_PROCBASED2, EPT_PROC2, 0);
    vmwrite(PIN_BASED_CTLS, pin);
    vmwrite(PROC_BASED_CTLS, proc);
    vmwrite(PROC_BASED_CTLS2, proc2);
    vmwrite(EPT_POINTER, eptp);
    vmwrite(GUEST_CR3, GP_PML4);
    vmwrite(GUEST_GDTR_BASE, GP_GDT);
    vmwrite(GUEST_IDTR_BASE, GP_GDT);
    vmwrite(GUEST_TR_BASE, GP_TSS2);
    vmwrite(GUEST_RSP, GP_STACK);

    for (n = 0; n < 32; n++) {
        u64 ticks = uno_native_tsc_per_us() * 2000ull
                    >> (unsigned)(rdmsr(IA32_VMX_MISC) & 0x1F);
        vmwrite(VMX_PREEMPT_VALUE, ticks ? ticks : 1);
        run_once(&ex);
        if (ex.raw == EXIT_EPT_VIOLATION) {
            if (mmio_service()) continue;
            break;                            /* undecodable: say so, stop   */
        }
        if (ex.raw == EXIT_PREEMPT || ex.raw == EXIT_EXT_INTR) continue;
        break;
    }

    out->magic    = cells[0];
    out->used_idx = cells[1];
    out->text     = uno_vdev_output(&n, &out->notifies, &out->bytes);
    out->cycle_refused = uno_vdev_cycle_refused(GP_VQ_DESC, 8);
    return ex.reason == UNO_VX_HLT
        && out->magic == 0x74726976u
        && out->used_idx == 1
        && n == want
        && out->cycle_refused;
}

/* ---- A6: the Linux boot protocol ------------------------------------------
 *
 * THE X86 EQUIVALENT OF GLIDE'S DEVICE TREE IS `boot_params`. An arm64 kernel
 * is entered with x0 holding a flattened device tree and learns its machine
 * from it; an x86-64 kernel is entered with RSI holding a zero page and learns
 * its machine from that. Same sentence, different structure, and in both cases
 * a kernel that is handed nothing does not get far enough to complain.
 *
 * No BIOS, no UEFI, no real mode: the 64-bit entry point at load address +
 * 0x200 is entered directly with paging already on. That is what makes
 * "unrestricted guest" a nice-to-have here rather than a requirement.
 *
 * The guest's memory map is ours to invent, and the layout below keeps the
 * structures Linux must not tread on out of the region it is told is free. */
#define L_PML4    0x700000ull
#define L_PDPT    0x701000ull
#define L_PD0     0x702000ull            /* 4 PDs: 4 GiB identity-mapped     */
#define L_GDT     0x706000ull
#define L_CMDLINE 0x800000ull
#define L_ZEROPG  0x900000ull
#define L_KERNEL  0x1000000ull           /* 16 MiB, clear of everything      */
#define EXIT_IO   30
#define EXIT_CR   28
#define EXIT_EXCEPTION 0
#define CR0_GH_MASK        0x6000
#define CR4_GH_MASK        0x6002
#define CR0_READ_SHADOW    0x6004
#define CR4_READ_SHADOW    0x6006
#define CR4_VMXE           (1ull << 13)
#define VM_EXIT_INTR_INFO  0x4404
#define VM_EXIT_INTR_ERROR 0x4406
#define L_CMDLINE_TEXT \
    "earlyprintk=serial,ttyS0,115200 console=ttyS0 nolapic no_timer_check " \
    "panic=-1 nokaslr lpj=4000000"

static int g_lin_lines;
static unsigned g_lin_lastport;
static int g_lin_running;
static int g_lin_mark;
static char g_lin_last[120];

static void lin_sink(const char *s)
{
    unsigned i;
    g_lin_lines++;
    for (i = 0; i + 1 < sizeof g_lin_last && s[i]; i++) g_lin_last[i] = s[i];
    g_lin_last[i] = 0;
    trace("[lin] "); trace(s); trace("\n");
}

/* Its page tables, at addresses the e820 map calls reserved.  Four gibibytes
 * identity-mapped, because the boot protocol requires the kernel, its zero
 * page and its command line all to be reachable before it builds its own. */
static int lin_paging(void)
{
    u64 *pml4 = (u64 *)uno_vmm_gpa(L_PML4, 4096);
    u64 *pdpt = (u64 *)uno_vmm_gpa(L_PDPT, 4096);
    unsigned g, i;
    if (!pml4 || !pdpt) return 0;
    for (i = 0; i < 512; i++) { pml4[i] = 0; pdpt[i] = 0; }
    for (g = 0; g < 4; g++) {
        u64 *pd = (u64 *)uno_vmm_gpa(L_PD0 + 0x1000 * g, 4096);
        if (!pd) return 0;
        for (i = 0; i < 512; i++)
            pd[i] = ((u64)g << 30) | ((u64)i << 21) | 0x83;
        pdpt[g] = (L_PD0 + 0x1000 * g) | 0x3;
    }
    pml4[0] = L_PML4 + 0x1000 - 0x1000;   /* placeholder, set below          */
    pml4[0] = L_PDPT | 0x3;
    return 1;
}

/* One e820 entry, appended.  The reserved run in the middle is the loader's
 * own structures: a kernel told that memory is free will use it, and the page
 * tables it is still running on are in there. */
static void e820_add(u8 *zp, int *n, u64 base, u64 len, u32 type)
{
    u8 *e = zp + 0x2D0 + (*n) * 20;
    if (*n >= 128) return;
    *(u64 *)(e + 0)  = base;
    *(u64 *)(e + 8)  = len;
    *(u32 *)(e + 16) = type;
    (*n)++;
}

static int lin_zeropage(const u8 *setup, u64 carve)
{
    u8 *zp = (u8 *)uno_vmm_gpa(L_ZEROPG, 4096);
    char *cl = (char *)uno_vmm_gpa(L_CMDLINE, 512);
    const char *t = L_CMDLINE_TEXT;
    int n = 0, i;
    if (!zp || !cl) return 0;
    for (i = 0; i < 4096; i++) zp[i] = 0;
    for (i = 0; t[i] && i < 511; i++) cl[i] = t[i];
    cl[i] = 0;

    /* The setup header travels verbatim from the image: it carries the
     * kernel's own answers about itself (version, relocatability, init_size),
     * and inventing any of them is how a loader breaks on the next release. */
    for (i = 0x1F1; i <= 0x268; i++) zp[i] = setup[i];
    zp[0x210] = 0xFF;                     /* type_of_loader: not a known one */
    /* CAN_USE_HEAP goes OFF, not on. It is a promise that heap_end_ptr is
     * valid, and a flag set beside a field left at zero points the kernel's
     * heap at address zero. The 64-bit entry path does not want a heap from
     * the loader anyway. */
    zp[0x211] &= (u8)~0x80;
    zp[0x211] &= (u8)~0x20;               /* QUIET off, we want the output   */
    /* code32_start is where WE put the kernel, not where the image was built
     * to sit. It ships as 0x100000 and we load at 16 MiB. */
    *(u32 *)(zp + 0x214) = (u32)L_KERNEL;
    *(u32 *)(zp + 0x224) = 0;             /* heap_end_ptr: none, and say so  */
    *(u32 *)(zp + 0x228) = (u32)L_CMDLINE;
    *(u32 *)(zp + 0x218) = 0;             /* no initrd yet                   */
    *(u32 *)(zp + 0x21C) = 0;

    e820_add(zp, &n, 0x00000000, 0x0009FC00, 1);
    e820_add(zp, &n, 0x0009FC00, 0x00000400, 2);
    e820_add(zp, &n, 0x000F0000, 0x00010000, 2);
    e820_add(zp, &n, 0x00100000, 0x00500000, 1);
    e820_add(zp, &n, 0x00600000, 0x00A00000, 2);   /* ours: tables, cmdline */
    e820_add(zp, &n, 0x01000000, carve - 0x01000000, 1);
    zp[0x1E8] = (u8)n;
    return 1;
}

/* Read the kernel out of the filesystem straight into the carve. */
static long lin_load(u64 *entry)
{
    long size, off, got;
    int vol, nvol = uno_fs_volumes();
    const char *path = "EFI\\UNODOS\\VM\\BZIMAGE";
    u8 *setup = (u8 *)uno_vmm_gpa(L_ZEROPG + 0x1000, 4096);   /* scratch     */
    u8 *dst;
    unsigned setup_sects;

    if (!setup) return 0;
    for (vol = 0; vol < nvol; vol++) {
        size = uno_fs_size(vol, path);
        if (size > 0) break;
        size = uno_fs_size(vol, "BZIMAGE");
        if (size > 0) { path = "BZIMAGE"; break; }
    }
    if (vol >= nvol || size <= 0x300) return 0;
    if (uno_fs_read_at(vol, path, 0, setup, 4096) < 0x300) return 0;
    if (*(const u32 *)(setup + 0x202) != 0x53726448u) return -1;   /* 'HdrS' */
    if (!(setup[0x236] & 1)) return -2;               /* no 64-bit entry     */

    setup_sects = setup[0x1F1] ? setup[0x1F1] : 4;
    off = (long)(setup_sects + 1) * 512;
    dst = (u8 *)uno_vmm_gpa(L_KERNEL, (u64)(size - off));
    if (!dst) return -3;
    /* In chunks, because a multi-megabyte read through the FAT cache one
     * cluster at a time is what the sequential cursor exists for. */
    for (got = 0; got < size - off; ) {
        long want = size - off - got;
        long r;
        if (want > 0x40000) want = 0x40000;
        r = uno_fs_read_at(vol, path, off + got, dst + got, want);
        if (r <= 0) break;
        got += r;
    }
    if (got < size - off) return -4;
    *entry = L_KERNEL + 0x200;
    return got;
}

/* CPUID for a kernel rather than for a test guest: the real answers, with the
 * few bits masked that would send Linux looking for hardware we do not have. */
static void lin_cpuid(void)
{
    u32 a = (u32)g_ctx.rax, c = (u32)g_ctx.rcx, ra, rb, rc, rd;
    if (a == 0x40000000u) {                  /* the hypervisor leaf: say we
                                                are not one it knows          */
        g_ctx.rax = 0; g_ctx.rbx = 0; g_ctx.rcx = 0; g_ctx.rdx = 0;
        return;
    }
    __asm__ volatile ("cpuid" : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                              : "a"(a), "c"(c));
    if (a == 1) {
        rc &= ~(1u << 5);                    /* VMX: not for the guest       */
        rc &= ~(1u << 21);                   /* x2APIC: there is no APIC      */
        rc |=  (1u << 31);                   /* and say plainly it is a guest */
        rd &= ~(1u << 9);                    /* APIC                          */
    }
    g_ctx.rax = ra; g_ctx.rbx = rb; g_ctx.rcx = rc; g_ctx.rdx = rd;
}

static u64 g_kernel_gs;

static void lin_msr(int write)
{
    u32 idx = (u32)g_ctx.rcx;
    u64 v = ((u64)(u32)g_ctx.rdx << 32) | (u32)g_ctx.rax;
    switch (idx) {
    case 0xC0000080u:                        /* EFER, a VMCS guest field     */
        if (write) vmwrite(GUEST_IA32_EFER, v); else v = vmread(GUEST_IA32_EFER);
        break;
    case 0xC0000100u:                        /* FS_BASE                      */
        if (write) vmwrite(GUEST_FS_BASE, v); else v = vmread(GUEST_FS_BASE);
        break;
    case 0xC0000101u:                        /* GS_BASE                      */
        if (write) vmwrite(GUEST_GS_BASE, v); else v = vmread(GUEST_GS_BASE);
        break;
    case 0xC0000102u:                        /* KERNEL_GS_BASE               */
        if (write) g_kernel_gs = v; else v = g_kernel_gs;
        break;
    default:
        /* Zero for everything else. A kernel reading an MSR that answers 0 is
         * in far better shape than one reading a value we invented. */
        if (!write) v = 0;
        break;
    }
    if (!write) { g_ctx.rax = (u32)v; g_ctx.rdx = (u32)(v >> 32); }
}

static int vmx_linux(uno_vm_linux *out)
{
    u64 eptp, pa = (u64)(unsigned long long)(void *)g_vmcs, entry = 0;
    u64 carve = uno_vmm_carve_size();
    const u8 *setup;
    u8 fail = 0;
    unsigned i;
    u32 proc, proc2, pin;
    long n;

    out->loaded = 0; out->lines = 0; out->exits = 0;
    out->last = ""; out->stop_reason = 0; out->stop_rip = 0;
    out->fault_vec = 0xFFFF; out->fault_err = 0; out->fault_addr = 0;
    out->pio = 0; out->pio_n = 0; g_lin_lastport = 0xFFFFFFFFu;
    g_lin_lines = 0; g_lin_last[0] = 0; g_kernel_gs = 0;

    if (!uno_vmm_probe()->slat || !carve) return 0;
    eptp = ept_build();
    if (!eptp) return 0;

    n = lin_load(&entry);
    if (n <= 0) { out->stop_reason = (unsigned)(-n); return 0; }
    out->loaded = n;
    setup = (const u8 *)uno_vmm_gpa(L_ZEROPG + 0x1000, 4096);
    if (!setup || !lin_paging() || !lin_zeropage(setup, carve)) return 0;

    {   /* Its GDT, and THE SELECTOR NUMBERS ARE PART OF THE PROTOCOL. The
         * 64-bit boot protocol does not say "provide a code and a data
         * segment", it says __BOOT_CS is selector 0x10 and __BOOT_DS is
         * 0x18 - because startup_64 loads 0x18 into DS, ES and SS within its
         * first few instructions, before it has its own GDT.
         *
         * Putting them at 0x08 and 0x10 (which is what every other guest in
         * this file uses) means that load reaches a null descriptor: #GP,
         * into an IDT the kernel has not installed yet, triple fault, about
         * a hundred bytes past the entry point. Which is exactly where this
         * first landed. */
        u64 *g = (u64 *)uno_vmm_gpa(L_GDT, 4096);
        if (!g) return 0;
        for (i = 0; i < 512; i++) g[i] = 0;
        g[2] = 0x00AF9B000000FFFFull;         /* selector 0x10: __BOOT_CS    */
        g[3] = 0x00CF93000000FFFFull;         /* selector 0x18: __BOOT_DS    */
    }

    for (i = 0; i < sizeof g_ctx / sizeof(u64); i++) ((u64 *)&g_ctx)[i] = 0;
    *(u32 *)g_vmcs = (u32)(rdmsr(IA32_VMX_BASIC) & 0x7FFFFFFFu);
    __asm__ volatile ("vmclear %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    __asm__ volatile ("vmptrld %1; setna %0" : "=r"(fail) : "m"(pa) : "cc");
    if (fail) return 0;
    g_launched = 0;
    uno_vdev_reset();

    vmcs_reset(0, entry, 0);                 /* IDT limit 0: it installs one */
    pin   = adjust(IA32_VMX_TRUE_PIN, IA32_VMX_PINBASED,
                   PIN_EXT_INTR_EXIT | PIN_PREEMPT, 1);
    /* UNCONDITIONAL I/O EXITING, and its absence is why the kernel was
     * silent. Port I/O does not exit unless asked: without bit 24 the guest's
     * `in` and `out` execute NATIVELY, against this machine's real ports. So
     * the kernel was not failing to write to its serial port - it was writing
     * to the HOST's, along with whatever else it probed. Fourteen hundred
     * exits and not one of them I/O should have been the tell: a booting
     * kernel touches the PIT, the CMOS and the PCI config ports constantly. */
    proc  = adjust(IA32_VMX_TRUE_PROC, IA32_VMX_PROCBASED,
                   PROC_HLT_EXITING | (1u << 24) | (1u << 31), 1);
    /* ENABLE INVPCID, and it is the same lesson as the I/O controls: an
     * instruction the CPU has does not necessarily WORK in a guest. INVPCID
     * raises #UD in VMX non-root operation unless bit 12 says otherwise, and
     * CPUID cheerfully tells the guest it is available. Linux believes CPUID
     * and uses it in native_flush_tlb_global, which is early and unavoidable:
     *   PANIC: early exception 0x06 ... native_flush_tlb_global+0x3c
     *   Code: ... <66> 0f 38 82   <- invpcid
     * One bit, and the alternative would have been masking the feature out of
     * CPUID and making every TLB flush slower for no reason. */
    proc2 = adjust(IA32_VMX_PROCBASED2, IA32_VMX_PROCBASED2,
                   EPT_PROC2 | (1u << 12), 0);
    vmwrite(PIN_BASED_CTLS, pin);
    vmwrite(PROC_BASED_CTLS, proc);
    vmwrite(PROC_BASED_CTLS2, proc2);
    vmwrite(EPT_POINTER, eptp);
    vmwrite(GUEST_CR3, L_PML4);
    vmwrite(GUEST_GDTR_BASE, L_GDT);
    vmwrite(GUEST_GDTR_LIMIT, 0x1F);
    /* WHICH EXCEPTIONS TO STEAL, and the answer is fewer than it looks.
     *
     * Trapping #UD/#GP/#PF is what made the CR4 fault visible: before its own
     * IDT exists, every exception the kernel takes is a triple fault with no
     * vector and no address. But the decompressor TAKES PAGE FAULTS ON
     * PURPOSE - its identity map is built on demand, and its own #PF handler
     * adds the mapping and returns. Intercepting #PF steals an exception the
     * guest was going to handle correctly, and reports the kernel's normal
     * operation as a failure.
     *
     * So #PF is left to the guest. #UD and #GP are still stolen: nothing in a
     * booting kernel expects either, and while they are trapped a fault that
     * WOULD have been a silent triple fault names itself. That trade goes
     * away once the kernel is known to boot. */
    /* And now NONE of them. Once the kernel is far enough in to have its own
     * IDT, #UD and #GP are its business too - Linux raises #UD deliberately
     * for BUG() and patches instructions around it. Stealing those turns the
     * kernel's own error handling into our stop. The bitmap earned its keep
     * on the first two faults and is now in the way. */
    vmwrite(EXCEPTION_BITMAP, 0);
    /* THE SHADOW REGISTERS, and a kernel cannot boot without them.
     *
     * VMX requires the guest's CR4 to keep VMXE set at all times - it is in
     * IA32_VMX_CR4_FIXED0. Linux does not know it is a guest and writes CR4
     * with its own idea of the bits, VMXE cleared among them, about a hundred
     * bytes into its entry point. Unshadowed, that write is #GP(0) with
     * nothing to say why, which is precisely where this stopped.
     *
     * So VMXE is owned by us: the mask makes a write that changes it exit
     * instead of faulting, and the read shadow makes the guest see the bit as
     * clear, which is the value it expects to read back. The guest keeps
     * every other bit. */
    vmwrite(CR4_GH_MASK, CR4_VMXE);
    vmwrite(CR4_READ_SHADOW, vmread(GUEST_CR4) & ~CR4_VMXE);
    vmwrite(CR0_GH_MASK, 0);
    vmwrite(CR0_READ_SHADOW, vmread(GUEST_CR0));
    vmwrite(GUEST_CS_SEL, 0x10);
    vmwrite(GUEST_SS_SEL, 0x18); vmwrite(GUEST_DS_SEL, 0x18);
    vmwrite(GUEST_ES_SEL, 0x18); vmwrite(GUEST_FS_SEL, 0x18);
    vmwrite(GUEST_GS_SEL, 0x18);
    vmwrite(GUEST_RSP, L_ZEROPG - 0x100);
    vmwrite(GUEST_RFLAGS, 0x2);              /* interrupts off, as required  */
    g_ctx.rsi = L_ZEROPG;                    /* THE zero page, in RSI        */

    g_lin_running = 1;
    out->lines = 0;
    out->last = g_lin_last;
    out->chars = 0;
    return 1;                                 /* placed and ready to run     */
}

/* One budgeted slice of the kernel, and everything its exits need.  This is
 * the same servicing the boot-time attempt did, moved to where a kernel can
 * actually live: called once per shell frame, for as long as it takes.
 *
 * A6a ran it inside a selftest with a three-second bound, which is fine for
 * "did it say anything" and hopeless for "did it reach userspace" - a kernel
 * that needs eight seconds is not slow, it is normal. */
static int vmx_linux_slice(unsigned budget_us, uno_vm_linux *out)
{
    uno_vmexit ex;
    u64 per_us = uno_native_tsc_per_us();
    unsigned shift = (unsigned)(rdmsr(IA32_VMX_MISC) & 0x1F);
    u64 ticks;
    int n;

    if (!g_lin_running) return 0;
    if (!per_us) per_us = 1000ull;
    ticks = (per_us * budget_us) >> shift;
    if (!ticks) ticks = 1;

    /* Several entries per frame: most exits are a CPUID or a port write that
     * costs a microsecond to answer, and returning to the frame loop after
     * each one would spend the whole budget on the round trip. */
    for (n = 0; n < 512; n++) {
        vmwrite(VMX_PREEMPT_VALUE, ticks);
        run_once(&ex);
        out->exits++;
        if (ex.raw == EXIT_PREEMPT) break;         /* the budget is spent    */
        if (ex.raw == EXIT_EXT_INTR) break;
        if (ex.raw == EXIT_CPUID) {
            lin_cpuid();
            vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTR_LEN));
            continue;
        }
        if (ex.raw == EXIT_RDMSR || ex.raw == EXIT_WRMSR) {
            lin_msr(ex.raw == EXIT_WRMSR);
            vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTR_LEN));
            continue;
        }
        if (ex.raw == EXIT_IO) {
            u64 q = vmread(EXIT_QUALIFICATION), v;
            unsigned size = (unsigned)(q & 7) + 1;
            unsigned port = (unsigned)(q >> 16) & 0xFFFF;
            int is_in = (q & 8) ? 1 : 0;
            if (q & 0x10) { g_lin_running = 0; break; }   /* a string op     */
            v = g_ctx.rax;
            out->pio++;
            out->last_port = port;
            uno_vdev_pio(port, !is_in, size, &v, lin_sink);
            if (is_in) {
                if (size == 1) g_ctx.rax = (g_ctx.rax & ~0xFFull) | (v & 0xFF);
                else if (size == 2) g_ctx.rax = (g_ctx.rax & ~0xFFFFull) | (v & 0xFFFF);
                else g_ctx.rax = v & 0xFFFFFFFFull;
            }
            vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTR_LEN));
            continue;
        }
        if (ex.raw == EXIT_CR) {
            u64 q = vmread(EXIT_QUALIFICATION);
            int cr = (int)(q & 15), acc = (int)((q >> 4) & 3);
            u64 *r = gpr((int)((q >> 8) & 15));
            if (acc == 0 && cr == 4 && r) {
                vmwrite(GUEST_CR4, *r | CR4_VMXE);
                vmwrite(CR4_READ_SHADOW, *r & ~CR4_VMXE);
                vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTR_LEN));
                continue;
            }
            if (acc == 0 && cr == 0 && r) {
                vmwrite(GUEST_CR0, (*r | rdmsr(IA32_VMX_CR0_FIXED0))
                                   & rdmsr(IA32_VMX_CR0_FIXED1));
                vmwrite(CR0_READ_SHADOW, *r);
                vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTR_LEN));
                continue;
            }
            g_lin_running = 0; break;
        }
        if (ex.raw == EXIT_EPT_VIOLATION && mmio_service()) continue;
        if (ex.raw == EXIT_HLT) {
            /* A halted kernel is waiting for an interrupt it will not get
             * until there is a timer. Stopping here rather than spinning
             * says so, instead of burning a slice a frame forever. */
            g_lin_running = 0;
            out->stop_reason = (unsigned)ex.raw;
            out->stop_rip = ex.rip;
            break;
        }
        g_lin_running = 0;
        out->stop_reason = (unsigned)ex.raw;
        out->stop_rip = ex.rip;
        break;
    }
    out->lines = g_lin_lines;
    out->last = g_lin_last;
    out->chars = uno_vdev_serial_chars();
    /* On the debug console rather than the kernel log: a kernel that stops
     * printing may be spinning on a port nobody emulated, and the boot log is
     * only flushed on a timer this run does not reach. */
    if ((out->exits >> 14) != g_lin_mark) {
        g_lin_mark = out->exits >> 14;
        tracex("[lin] still going, port=", (u64)out->last_port);
    }
    return g_lin_running;
}

static const uno_hv_t VMX = { "vmx", vmx_enable, vmx_marker, vmx_crasher,
                              vmx_ept, vmx_spin_start, vmx_slice, vmx_clockirq,
                              vmx_virtio, vmx_linux, vmx_linux_slice };

const uno_hv_t *uno_hv_vmx(void) { return &VMX; }
