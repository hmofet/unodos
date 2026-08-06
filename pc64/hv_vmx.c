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

static void run_once(uno_vmexit *out)
{
    tracex("[hv] vmentry rip=", vmread(GUEST_RIP));
    vmx_entry(&g_ctx);
    if (!g_entry_failed) g_launched = 1;
    classify(out);
    tracex("[hv] exit reason=", out->raw);
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

static const uno_hv_t VMX = { "vmx", vmx_enable, vmx_marker, vmx_crasher };

const uno_hv_t *uno_hv_vmx(void) { return &VMX; }
