/* ===========================================================================
 * unovirt - the phase tests A1..A6, above the backend seam.
 *
 * Everything in this file was once a function pointer in `uno_hv_t`, one per
 * phase, which meant every backend owed nine test guests before it could claim
 * any phase past the first.  That is why hv_svm.c sat at A1: not because AMD
 * needs a different marker guest, but because the marker guest was on the
 * wrong side of the seam.  None of this is vendor-specific.  The guests are
 * x86 machine code, which is the same machine code on both; servicing their
 * exits is x86 register work, which is the same work; the page tables they run
 * on are x86 page tables.  What differs - how a control block is loaded, how
 * an exit reason is spelled, what the machine insists on in a control word -
 * is behind `uno_hv_t` (unovirt_hv.h) and nowhere in this file.
 *
 * So a backend now owes seven generic operations, and gets A1..A6 for free.
 * A phase DECLINES rather than fails when the machine cannot host it: a
 * backend with no `map` has no second stage, and everything from A2 on says so
 * and lets the boot carry on.
 * ======================================================================== */
#include "unovirt_phase.h"
#include "unovirt.h"
#include "unovdev.h"     /* the device the MMIO decode answers for        */
#include "pc64_fs.h"     /* A6 reads the kernel off the filesystem        */
#include "unovirt_mgr.h" /* which appliance the user asked for            */

typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef unsigned long long u64;

/* ONE VMCS MEANS ONE GUEST, on both vendors and for the same reason: a
 * backend has one control block, and arming a second guest reconfigures the
 * block the first was using.  That does not fail - it silently replaces a
 * booting Linux with two bytes of `jmp $`, and the only symptom is a kernel
 * that stops saying anything (A6b).  So there is one vCPU here too, and the
 * ownership question is answered by whoever created it last. */
static uno_vcpu g_vcpu;

/* ---- bring-up trace, opt-in (see hv_vmx.c for why this exists) ----------- */
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

/* Step the guest past the instruction that exited.  The length comes from the
 * exit, not from counting the bytes ourselves: it is the one number that is
 * right for every encoding of every instruction that can exit here. */
static void step(const uno_hv_t *hv, const uno_vmexit *ex)
{
    hv->set(&g_vcpu, UNO_VR_RIP, hv->get(&g_vcpu, UNO_VR_RIP) + ex->instr_len);
}

/* ---- A1: a machine for the first guest to run on --------------------------
 *
 * With no second stage the guest's physical addresses ARE host physical
 * addresses, so this guest could reach any byte in the machine.  That is
 * survivable for exactly one reason - it is eighteen bytes of machine code we
 * wrote, and either its page tables or its segment bases address nothing else.
 * It is NOT survivable for anything more, which is what A2 and the carve are
 * for.  Glide's V2/V3a split is the same split for the same reason: prove the
 * round trip first, then take away the machine.
 *
 * The image is identity-mapped, so a pointer into this file IS a physical
 * address - which is what lets the guest's memory live above the seam. */
__attribute__((aligned(4096))) static u8 g_a1[16384];
__attribute__((aligned(4096))) static u64 g_a1_pml4[512];
__attribute__((aligned(4096))) static u64 g_a1_pdpt[512];
__attribute__((aligned(4096))) static u64 g_a1_pd[512];

#define G_DATA 0x1000            /* the marker lands here                    */
#define G_GDT  0x2000
#define G_TSS  0x3000

/* One PD covers a gibibyte, so mapping the whole gibibyte that contains the
 * guest costs three pages and cannot be got wrong at the edges.  Linear equals
 * physical inside it; with no second stage, physical equals HOST physical,
 * which is what makes this an A1 arrangement and not a shippable one. */
static int a1_paging(u64 base)
{
    u64 g1 = base & ~0x3FFFFFFFull;                  /* the gibibyte it is in */
    unsigned i;
    if (((base + sizeof g_a1 - 1) & ~0x3FFFFFFFull) != g1) return 0;
    for (i = 0; i < 512; i++) {
        g_a1_pml4[i] = 0;
        g_a1_pdpt[i] = 0;
        g_a1_pd[i]   = (g1 + ((u64)i << 21)) | 0x83; /* present, rw, 2 MiB    */
    }
    g_a1_pdpt[(g1 >> 30) & 511] = (u64)(unsigned long long)(void *)g_a1_pd   | 0x3;
    g_a1_pml4[(g1 >> 39) & 511] = (u64)(unsigned long long)(void *)g_a1_pdpt | 0x3;
    return 1;
}

/* Place the A1 guest's machine, long mode for preference.
 *
 * A backend that has not built its long-mode arrangement refuses the mode and
 * gets the real-mode one, which needs no tables at all - AMD lets a guest
 * start with CR0.PE clear, and eleven bytes of guest do not justify a GDT and
 * a page walk to reach.  *real16 says which it got, because the two want
 * different guest ENCODINGS and nothing else. */
static int a1_place(const uno_hv_t *hv, unsigned idt_limit, int *real16)
{
    u64 base = (u64)(unsigned long long)(void *)g_a1;
    uno_vm_cfg cfg;
    u64 *gdt = (u64 *)(g_a1 + G_GDT);
    unsigned i;

    for (i = 0; i < sizeof g_a1 / 8; i++) ((u64 *)g_a1)[i] = 0;
    gdt[0] = 0;
    gdt[1] = 0x00AF9B000000FFFFull;                  /* 64-bit code, DPL 0    */
    gdt[2] = 0x00CF93000000FFFFull;                  /* data                  */

    for (i = 0; i < sizeof cfg; i++) ((u8 *)&cfg)[i] = 0;
    cfg.mode      = UNO_VM_FLAT64;
    cfg.features  = 0;
    /* RIP is the guest's LINEAR address, and in long mode there is no segment
     * base to fold into it - CS.base must be 0.  So this arrangement enters at
     * the code's actual address.  Entering at 0 instead maps nothing, and the
     * instruction fetch faults into an IDT that cannot deliver: a triple fault
     * at a guest RIP of 0, which looks exactly like a guest that never
     * started.  That bug cost a session on the SVM side, and it is the reason
     * the two arrangements are spelled out separately rather than shared. */
    cfg.rip       = base;
    cfg.rsp       = base + G_TSS;                    /* a page nothing else uses */
    cfg.rflags    = 0x2;
    cfg.cr3       = (u64)(unsigned long long)(void *)g_a1_pml4;
    cfg.gdt_base  = base + G_GDT;  cfg.gdt_limit = 0x2F;
    cfg.idt_base  = base + G_GDT;  cfg.idt_limit = idt_limit;
    cfg.tr_base   = base + G_TSS;
    cfg.cs_sel    = 0x08;
    cfg.ds_sel    = 0x10;
    if (a1_paging(base) && hv->vcpu_create(&g_vcpu, &cfg)) { *real16 = 0; return 1; }

    for (i = 0; i < sizeof cfg; i++) ((u8 *)&cfg)[i] = 0;
    cfg.mode      = UNO_VM_REAL16;
    cfg.seg_base  = base;
    cfg.rip       = 0;
    cfg.rsp       = 0x0F00;
    cfg.rflags    = 0x2;
    cfg.idt_base  = base;          cfg.idt_limit = idt_limit;
    if (hv->vcpu_create(&g_vcpu, &cfg)) { *real16 = 1; return 1; }
    return 0;
}

/* The marker guest, in the two encodings its two machines need.  Same four
 * steps either way: take a value through a cpuid intercept, store it to its
 * own memory at G_DATA, halt.
 *
 * long mode, eighteen bytes:          real mode, twelve:
 *   0F A2        cpuid                  0F A2        cpuid
 *   48 BB <imm64> mov rbx, &data        66 A3 00 10  mov [0x1000], eax
 *   89 03        mov [rbx], eax         66 89 16 04 10  mov [0x1004], edx
 *   89 53 04     mov [rbx+4], edx       F4           hlt
 *   F4           hlt
 *
 * The evidence is the two dwords at G_DATA, not the exit codes.  A guest that
 * never ran leaves them zero; a hypervisor that failed to advance RIP past the
 * cpuid never reaches them at all (it re-executes the same instruction
 * forever, which is a hang rather than a wrong answer); and a value that
 * arrives has been through the guest's registers and its own store. */
static void a1_emit_marker(int real16, u64 data_va)
{
    u8 *p = g_a1;
    unsigned i;
    if (real16) {
        static const u8 M[] = { 0x0F, 0xA2, 0x66, 0xA3, 0x00, 0x10,
                                0x66, 0x89, 0x16, 0x04, 0x10, 0xF4 };
        for (i = 0; i < sizeof M; i++) p[i] = M[i];
        return;
    }
    p[0] = 0x0F; p[1] = 0xA2;
    p[2] = 0x48; p[3] = 0xBB;
    for (i = 0; i < 8; i++) p[4 + i] = (u8)(data_va >> (8 * i));
    p[12] = 0x89; p[13] = 0x03;
    p[14] = 0x89; p[15] = 0x53; p[16] = 0x04;
    p[17] = 0xF4;
}

int uno_hvp_marker(const uno_hv_t *hv, u64 want, u64 *got, uno_vmexit *last)
{
    int real16 = 0, guard;

    *got = 0;
    if (!a1_place(hv, 0xFFF, &real16)) return 0;
    a1_emit_marker(real16, (u64)(unsigned long long)(void *)g_a1 + G_DATA);

    /* Bounded, because an exit we do not handle must end the guest rather
     * than the machine.  Eight is generous for a guest with two exits. */
    for (guard = 0; guard < 8; guard++) {
        if (!hv->vcpu_run(&g_vcpu, 0, last)) return 0;
        if (last->reason == UNO_VX_CPUID) {
            /* Answer as the "device" this guest is talking to, then step it
             * past the instruction. */
            g_vcpu.gprs.rax = want & 0xFFFFFFFFull;
            g_vcpu.gprs.rdx = (want >> 32) & 0xFFFFFFFFull;
            g_vcpu.gprs.rbx = 0;
            g_vcpu.gprs.rcx = 0;
            step(hv, last);
            continue;
        }
        if (last->reason == UNO_VX_INTR) continue;
        break;
    }
    if (last->reason != UNO_VX_HLT) return 0;

    *got = (u64)*(u32 *)(g_a1 + G_DATA)
         | ((u64)*(u32 *)(g_a1 + G_DATA + 4) << 32);
    return *got == want;
}

int uno_hvp_crasher(const uno_hv_t *hv, uno_vmexit *out)
{
    int real16 = 0;
    /* IDT limit 0 is the crasher's whole mechanism: the first exception cannot
     * be delivered, which raises another, which is a triple fault. */
    if (!a1_place(hv, 0, &real16)) return 0;
    if (real16) { g_a1[0] = 0xCD; g_a1[1] = 0x03; }   /* int3, with no IDT   */
    else        { g_a1[0] = 0x0F; g_a1[1] = 0x0B; }   /* ud2                 */
    if (!hv->vcpu_run(&g_vcpu, 0, out)) return 0;
    /* Either the CPU refused the guest outright or the guest destroyed
     * itself.  Both are contained; neither may be a hang or a host fault,
     * which is the whole claim being tested. */
    return out->reason == UNO_VX_SHUTDOWN || out->reason == UNO_VX_INVALID;
}

/* ---- A2: second-stage translation, and a guest that only sees its own -----
 *
 * With second stage on, the guest's physical addresses stop being this
 * machine's.  The carve is mapped at guest-physical 0, so the guest sees a
 * small tidy machine with RAM at the bottom, and everything else in this
 * computer is not "denied" to it - it is not expressible.
 *
 * The guest's own page tables now live in the carve at guest-physical
 * addresses, so CR3 is a GPA and the host writes them through uno_vmm_gpa().
 * That is the whole difference from A1, where CR3 was a host address because
 * the two were the same thing. */

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
#define GP_IDT   0x5000
#define GP_TSS2  0x9000
#define GP_CELLS 0x100000      /* [0] tsc, [1] msr echo, [2] irq counter   */

/* Map the whole carve, and nothing else.  A guest reading past the end of its
 * own memory must take a reportable fault; leaving the tail mapped at whatever
 * follows the carve in host memory would make it a silent success. */
static int carve_map(const uno_hv_t *hv)
{
    u64 base = uno_vmm_carve_base(), size = uno_vmm_carve_size();
    if (!hv->map || !base || !size) return 0;
    return hv->map(0, base, size, UNO_VP_RWX, UNO_VMEM_WB);
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

/* The machine A2..A5's guests all run on: the carve, its own tables inside it,
 * and RIP at a guest-physical address.  Only four things differ between them
 * and each one is a phase's whole subject. */
static int carve_vcpu(const uno_hv_t *hv, unsigned features, u64 idt_base,
                      u64 tr_base, u64 rflags)
{
    uno_vm_cfg cfg;
    unsigned i;
    for (i = 0; i < sizeof cfg; i++) ((u8 *)&cfg)[i] = 0;
    cfg.mode      = UNO_VM_FLAT64;
    cfg.features  = UNO_VMF_SLAT | features;
    cfg.rip       = GP_CODE;
    cfg.rsp       = GP_STACK;
    cfg.rflags    = rflags;
    cfg.cr3       = GP_PML4;
    cfg.gdt_base  = GP_GDT;   cfg.gdt_limit = 0x2F;
    cfg.idt_base  = idt_base; cfg.idt_limit = 0xFFF;
    cfg.tr_base   = tr_base;
    cfg.cs_sel    = 0x08;
    cfg.ds_sel    = 0x10;
    return hv->vcpu_create(&g_vcpu, &cfg);
}

int uno_hvp_ept(const uno_hv_t *hv, u64 want, u64 gpa, u64 *got, u64 *hpa,
                uno_vmexit *last)
{
    u8 *code = (u8 *)uno_vmm_gpa(GP_CODE, 64);
    u32 *cell = (u32 *)uno_vmm_gpa(gpa, 8);
    unsigned i;
    int guard;

    *got = 0;
    *hpa = 0;
    if (!code || !cell) return 0;
    if (!uno_vmm_probe()->slat) return 0;
    if (!carve_map(hv)) return 0;
    if (!guest_tables_in_carve()) return 0;
    *hpa = (u64)(unsigned long long)cell;
    cell[0] = 0; cell[1] = 0;

    if (!carve_vcpu(hv, 0, GP_GDT, GP_STACK, 0x2)) return 0;

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
        if (!hv->vcpu_run(&g_vcpu, 0, last)) return 0;
        if (last->reason == UNO_VX_CPUID) {
            g_vcpu.gprs.rax = want & 0xFFFFFFFFull;
            g_vcpu.gprs.rdx = (want >> 32) & 0xFFFFFFFFull;
            step(hv, last);
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
 * hands it a budget and the slice clock takes it back.
 *
 * TWO mechanisms, not one, and the second is not redundancy. The slice clock
 * bounds the slice; external-interrupt exiting means a host interrupt that
 * arrives mid-slice ends it too, rather than being delivered through the
 * guest's IDT - which this guest does not have, and which would turn every
 * timer tick into a triple fault.  Both ride on UNO_VMF_PREEMPT. */
static int g_spinning;

int uno_hvp_spin_start(const uno_hv_t *hv)
{
    u8 *code = (u8 *)uno_vmm_gpa(GP_CODE, 8);

    g_spinning = 0;
    if (!code || !uno_vmm_probe()->slat || !uno_vmm_probe()->preempt_timer) return 0;
    if (!carve_map(hv) || !guest_tables_in_carve()) return 0;
    if (!carve_vcpu(hv, UNO_VMF_PREEMPT, GP_GDT, GP_STACK, 0x2)) return 0;
    code[0] = 0xEB; code[1] = 0xFE;             /* jmp $                      */
    g_spinning = 1;
    return 1;
}

int uno_hvp_slice(const uno_hv_t *hv, unsigned budget_us, uno_vmexit *out)
{
    if (!g_spinning) return 0;
    if (!hv->vcpu_run(&g_vcpu, budget_us, out)) return 0;
    g_vcpu.quiet = 1;             /* the first slice is traced, the rest are not */
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
#define A4_VECTOR 0x20
#define A4_MSR   0x1234ABCDu     /* an index nothing real uses               */
#define A4_MSR_ANSWER 0x5A5A5A5Au

/* The guest, hand-assembled because eighteen instructions do not justify a
 * second toolchain in the build. Offsets are load-bearing and commented.
 *
 *   00: 48 BB <cells>   mov  rbx, 0x100000     ; its own memory
 *   0A: 0F 31           rdtsc                  ; NOT intercepted: it reads
 *   0C: 89 03           mov  [rbx], eax        ;   the real counter directly
 *   0E: B9 <msr>        mov  ecx, 0x1234ABCD
 *   13: 0F 32           rdmsr                  ; -> an MSR exit, we answer
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
static int a4_step(const uno_hv_t *hv, uno_vmexit *ex)
{
    if (!hv->vcpu_run(&g_vcpu, 2000, ex)) return 0;   /* 2 ms is plenty here */
    switch (ex->reason) {
    case UNO_VX_RDMSR:
        /* The guest asked for an MSR and we are its entire MSR space. With no
         * MSR bitmap every access exits, which is correct for a guest this
         * size and wrong for Linux - A5 wants a bitmap so the hot ones do not
         * trap. */
        g_vcpu.gprs.rax = 0;
        g_vcpu.gprs.rdx = A4_MSR_ANSWER;
        step(hv, ex);
        return 1;
    case UNO_VX_CPUID:
        g_vcpu.gprs.rax = 0;
        step(hv, ex);
        return 1;
    case UNO_VX_PREEMPT:
    case UNO_VX_INTR:
        return 1;
    default:
        return 0;                                 /* anything else is a bug */
    }
}

int uno_hvp_clockirq(const uno_hv_t *hv, uno_vm_clockirq *out)
{
    volatile u32 *cells;
    uno_vmexit ex;
    int n;

    out->t1 = out->t2 = out->msr_echo = 0;
    out->irqs = out->redelivered = out->exits = 0;

    cells = (volatile u32 *)uno_vmm_gpa(GP_CELLS, 16);
    if (!cells || !uno_vmm_probe()->slat || !uno_vmm_probe()->preempt_timer) return 0;
    if (!carve_map(hv) || !guest_tables_in_carve()) return 0;
    cells[0] = 0; cells[1] = 0; cells[2] = 0;

    /* Its own vector table, and interrupts open. A guest with IF clear cannot
     * take the injection, and the failure looks exactly like an injection that
     * never happened. */
    if (!carve_vcpu(hv, UNO_VMF_PREEMPT, GP_IDT, GP_TSS2, 0x202)) return 0;
    if (!a4_emit() || !a4_idt()) return 0;

    /* Phase 1 and 2: does its clock advance ACROSS slices? */
    for (n = 0; n < 40; n++) { if (!a4_step(hv, &ex)) return 0; out->exits++; }
    out->t1 = cells[0];
    for (n = 0; n < 40; n++) { if (!a4_step(hv, &ex)) return 0; out->exits++; }
    out->t2 = cells[0];
    out->msr_echo = cells[1];

    /* Phase 3: hand it an interrupt it never asked for and did not raise. */
    hv->inject(&g_vcpu, A4_VECTOR, 0, 0);
    for (n = 0; n < 20; n++) { if (!a4_step(hv, &ex)) return 0; out->exits++; }
    out->irqs = (int)cells[2];

    /* Phase 4: and does it STAY delivered once, rather than forever? */
    for (n = 0; n < 40; n++) { if (!a4_step(hv, &ex)) return 0; out->exits++; }
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
 * trap-and-emulate is a switch on those fields. x86 gives a second-stage fault
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
 * The instruction LENGTH does not have to be decoded: the exit supplies it,
 * which removes the part of decoding that is genuinely hard.  And none of
 * this is vendor-specific - it is x86 instruction encoding, which is why it
 * belongs above the seam rather than once per backend. */

/* Walk the guest's own page tables to turn a guest LINEAR address into a
 * guest-physical one. A1..A4's guests are identity-mapped, so this is the
 * identity for them; Linux is not, and this is what stops that being a
 * rewrite later. Every table read goes through the bounds seam. */
static int guest_walk(const uno_hv_t *hv, u64 lin, u64 *gpa)
{
    u64 cr3 = hv->get(&g_vcpu, UNO_VR_CR3) & ~0xFFFull;
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
 * RSP is index 4 and lives in the control block rather than the context,
 * which is the one hole a table like this always has.
 *
 * THIS TABLE HAD RSI AND RDI THE WRONG WAY ROUND, and it is worth saying how
 * that surfaced. Encoding 6 is RSI and 7 is RDI; `uno_gprs` declares rsi then
 * rdi at slots 4 and 5. The table sent each to the other's slot, so an MMIO
 * access through either register carried the OTHER one's value - and nothing
 * noticed for two phases, because A5's guest is hand-written to use rax/rbx/
 * rcx/rdx and Linux's port I/O goes through rax. It took a real driver: the
 * virtio-mmio driver writes its status byte from ESI, so every status write
 * arrived as an unrelated 0x04ef0000, the status register read back as zero,
 * and the driver reported `device refuses features: 0` - a conclusion about
 * FEATURES that had nothing to do with features. A wrong register file is
 * invisible until something uses the register you got wrong, and the report
 * you get names the wrong subsystem. */
static u64 *gpr(int n)
{
    u64 *c = (u64 *)&g_vcpu.gprs;
    static const int map[16] = { 0, 2, 3, 1, -1, 6, 4, 5,
                                 7, 8, 9, 10, 11, 12, 13, 14 };
    /* rax rcx rdx rbx rsp rbp rsi rdi r8..r15 -> uno_gprs order */
    if (n < 0 || n > 15 || map[n] < 0) return 0;
    return c + map[n];
}

/* Answer one second-stage fault as the device would, then step the guest past
 * the instruction. Returns 0 when it is not a device access or not a form we
 * decode - both of which end the guest with a report rather than a guess. */
static int mmio_service(const uno_hv_t *hv, const uno_vmexit *ex)
{
    u64 rip = hv->get(&g_vcpu, UNO_VR_RIP), ins_gpa = 0, val = 0;
    const u8 *ins;
    int reg = 0, is_store = 0, decoded_store = 0;
    unsigned size = 4;
    u64 *r;

    if (!guest_walk(hv, rip, &ins_gpa)) return 0;
    ins = (const u8 *)uno_vmm_gpa(ins_gpa, 16);
    if (!ins) return 0;
    if (!decode_mov(ins, &reg, &size, &decoded_store)) return 0;
    is_store = ex->npf_write;
    if (is_store != decoded_store) return 0;      /* the two must agree      */
    r = gpr(reg);
    if (!r) return 0;

    if (is_store) {
        val = *r;
        if (size == 1) val &= 0xFF;
        else if (size == 2) val &= 0xFFFF;
        else if (size == 4) val &= 0xFFFFFFFFull;
        if (!uno_vdev_mmio(ex->gpa, 1, size, &val)) return 0;
    } else {
        if (!uno_vdev_mmio(ex->gpa, 0, size, &val)) return 0;
        /* A 32-bit destination zero-extends into the top half, exactly as the
         * instruction would have. Writing only the low half leaves the guest
         * with whatever it had up there, which is a bug that appears only
         * once a value crosses 4 GiB. */
        if (size == 4) *r = val & 0xFFFFFFFFull;
        else if (size == 2) *r = (*r & ~0xFFFFull) | (val & 0xFFFF);
        else if (size == 1) *r = (*r & ~0xFFull) | (val & 0xFF);
        else *r = val;
    }
    hv->set(&g_vcpu, UNO_VR_RIP, rip + ex->instr_len);
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
    /* QUEUE 1, THE TRANSMIT QUEUE. This used to ring queue 0, which was a
     * latent disagreement with the spec rather than a bug anyone could see:
     * A5's device had one queue and obliged. A virtio console's queue 0 is
     * RECEIVE and queue 1 is TRANSMIT, and once the device learned the
     * difference - which it had to, because a real driver posts receive
     * buffers first - this guest was ringing the queue with nothing to send.
     * `mov eax, 1` is five bytes where `xor eax, eax` was two, so everything
     * after it shifts by three. */
    p[0x18] = 0xB8;
    p[0x19] = 0x01; p[0x1A] = 0; p[0x1B] = 0; p[0x1C] = 0;
    p[0x1D] = 0x89; p[0x1E] = 0x43; p[0x1F] = 0x50;
    p[0x20] = 0x48; p[0x21] = 0xBA;
    for (i = 0; i < 8; i++) p[0x22 + i] = (u8)((u64)GP_VQ_USED >> (8 * i));
    p[0x2A] = 0x8B; p[0x2B] = 0x42; p[0x2C] = 0x02;
    p[0x2D] = 0x89; p[0x2E] = 0x41; p[0x2F] = 0x04;
    p[0x30] = 0xF4;
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

int uno_hvp_virtio(const uno_hv_t *hv, uno_vm_virtio *out)
{
    volatile u32 *cells;
    uno_vmexit ex;
    int n, want;

    out->magic = out->used_idx = 0;
    out->bytes = out->notifies = out->cycle_refused = 0;
    out->text = "";
    ex.reason = UNO_VX_UNKNOWN;

    cells = (volatile u32 *)uno_vmm_gpa(GP_CELLS, 16);
    if (!cells || !uno_vmm_probe()->slat) return 0;
    if (!carve_map(hv) || !guest_tables_in_carve()) return 0;
    cells[0] = 0; cells[1] = 0;
    uno_vdev_reset();
    want = a5_rings();
    if (want <= 0 || !a5_emit()) return 0;

    if (!carve_vcpu(hv, UNO_VMF_PREEMPT, GP_GDT, GP_TSS2, 0x2)) return 0;

    for (n = 0; n < 32; n++) {
        if (!hv->vcpu_run(&g_vcpu, 2000, &ex)) break;
        if (ex.reason == UNO_VX_NPF) {
            if (mmio_service(hv, &ex)) continue;
            break;                            /* undecodable: say so, stop   */
        }
        if (ex.reason == UNO_VX_PREEMPT || ex.reason == UNO_VX_INTR) continue;
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
#define L_INITRD  0x20000000ull          /* 512 MiB: past the kernel's reach */
/* THE COMMAND LINE IS HOW THE GUEST LEARNS ITS DEVICES, and that is the whole
 * reason there is no PCI host bridge here: Linux takes virtio-mmio transports
 * from `virtio_mmio.device=<size>@<base>:<irq>`, so a bus, its config space
 * and its enumeration are all replaced by a string. The addresses match
 * unovdev.c's VDEV_BASE and stride, and the IRQs match the PIC lines it
 * asserts - numbers that have to agree across two files, which is why they
 * are named in both.
 *
 * THE CONSOLE AT 0xd0000000 IS DELIBERATELY NOT LISTED. It exists for A5, and
 * this guest's console is ttyS0: telling Linux about a second one gives it a
 * device to drive that nobody wants driven, and it does not sit quietly - its
 * driver posts a ring full of receive buffers and rings the doorbell for each,
 * drowning out every other diagnostic. A device a guest has no use for is a
 * liability, not a spare. */
#define L_CMDLINE_TEXT \
    "earlyprintk=serial,ttyS0,115200 console=ttyS0 nolapic no_timer_check " \
    "panic=-1 nokaslr lpj=4000000 rdinit=/bin/sh " \
    "virtio_mmio.device=0x200@0xd0000200:6 " \
    "virtio_mmio.device=0x200@0xd0000400:7"

static long g_initrd_size;
static int g_lin_lines;
static int g_lin_running;
static int g_lin_halted;         /* parked on a hlt, waiting for a line     */
static int g_lin_mark;
static u64 g_lin_netstats = ~0ull;
static int g_lin_vdump;          /* the virtio probe, dumped as it grows    */
static int g_lin_injects, g_lin_lastvec = -1;

static char g_lin_last[120];

/* What the shell is asked to say, and the whole difference between a console
 * that prints and one that WORKS. The kernel's own output proves the kernel
 * runs; only a reply proves the guest read something we sent, ran it, and
 * answered - which is the receive path, the 8259 and the injection all at
 * once. The echoed command line reads `~ # echo UNODOS-...`, so the reply is
 * distinguished by being the line that STARTS with the marker. */
#define SHELL_MARK "UNODOS-GUEST-SHELL-OK"
static int g_lin_shell_ok;

static void lin_sink(const char *s)
{
    unsigned i;
    g_lin_lines++;
    for (i = 0; i + 1 < sizeof g_lin_last && s[i]; i++) g_lin_last[i] = s[i];
    g_lin_last[i] = 0;
    for (i = 0; SHELL_MARK[i] && s[i] == SHELL_MARK[i]; i++) { }
    if (!SHELL_MARK[i]) g_lin_shell_ok++;
    uno_vm_con_push(s);              /* the manager app's console view      */
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
    /* The initramfs, and its two fields are the whole interface: an address
     * and a length. Linux unpacks it into a tmpfs and runs /init out of it,
     * which is the difference between a kernel that boots and a machine
     * somebody can use. */
    *(u32 *)(zp + 0x218) = g_initrd_size ? (u32)L_INITRD : 0;
    *(u32 *)(zp + 0x21C) = (u32)g_initrd_size;

    e820_add(zp, &n, 0x00000000, 0x0009FC00, 1);
    e820_add(zp, &n, 0x0009FC00, 0x00000400, 2);
    e820_add(zp, &n, 0x000F0000, 0x00010000, 2);
    e820_add(zp, &n, 0x00100000, 0x00500000, 1);
    e820_add(zp, &n, 0x00600000, 0x00A00000, 2);   /* ours: tables, cmdline */
    e820_add(zp, &n, 0x01000000, carve - 0x01000000, 1);
    zp[0x1E8] = (u8)n;
    return 1;
}

/* The initramfs, read into the carve the same way the kernel is.  Absent is
 * not an error: a kernel with no initrd still boots, it just has nowhere to
 * go afterwards. */
static long lin_initrd(void)
{
    const char *paths[2];
    int vol, nvol = uno_fs_volumes(), i;
    paths[0] = uno_vm_path_initrd();
    if (!paths[0][0]) paths[0] = "EFI\\UNODOS\\VM\\INITRD";
    paths[1] = "INITRD";
    for (vol = 0; vol < nvol; vol++) {
        for (i = 0; i < 2; i++) {
            long size = uno_fs_size(vol, paths[i]), got = 0;
            u8 *dst;
            if (size <= 0) continue;
            dst = (u8 *)uno_vmm_gpa(L_INITRD, (u64)size);
            if (!dst) return 0;
            while (got < size) {
                long want = size - got, r;
                if (want > 0x40000) want = 0x40000;
                r = uno_fs_read_at(vol, paths[i], got, dst + got, want);
                if (r <= 0) break;
                got += r;
            }
            return got == size ? size : 0;
        }
    }
    return 0;
}

/* Read the kernel out of the filesystem straight into the carve. */
static long lin_load(u64 *entry)
{
    long size, off, got;
    int vol, nvol = uno_fs_volumes();
    /* The running appliance's own kernel when it has one. An unconfigured VM
     * falls back to the appliance already on the disk rather than failing,
     * which is what lets a freshly created VM boot at all. */
    const char *want = uno_vm_path_kernel();
    const char *path = want[0] ? want : "EFI\\UNODOS\\VM\\BZIMAGE";
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
    u32 a = (u32)g_vcpu.gprs.rax, c = (u32)g_vcpu.gprs.rcx, ra, rb, rc, rd;
    if (a == 0x40000000u) {                  /* the hypervisor leaf: say we
                                                are not one it knows          */
        g_vcpu.gprs.rax = 0; g_vcpu.gprs.rbx = 0;
        g_vcpu.gprs.rcx = 0; g_vcpu.gprs.rdx = 0;
        return;
    }
    __asm__ volatile ("cpuid" : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                              : "a"(a), "c"(c));
    if (a == 1) {
        rc &= ~(1u << 5);                    /* VMX: not for the guest       */
        rc &= ~(1u << 21);                   /* x2APIC: there is no APIC      */
        rc |=  (1u << 31);                   /* and say plainly it is a guest */
        rd &= ~(1u << 9);                    /* APIC                          */
        /* XSAVE OFF, AND THE WHOLE FAMILY WITH IT. Advertising it drags in
         * XCR0, IA32_XSS and CPUID leaf 0xD, whose answers have to agree with
         * each other and with what the hypervisor actually preserves - and
         * they did not: the kernel died in fpstate_reset with a null pointer,
         * which is what a zero xstate size looks like from the far end.
         *
         * Without it the guest uses FXSAVE, which every x86-64 CPU has and
         * which needs nothing from us. It costs the guest AVX. Turning it
         * back on is a real slice of work (save and restore XCR0 per entry,
         * answer IA32_XSS, keep leaf 0xD consistent) and it is not on the
         * path to a shell. */
        rc &= ~(1u << 26);                   /* XSAVE                         */
        rc &= ~(1u << 27);                   /* OSXSAVE                       */
        rc &= ~(1u << 28);                   /* AVX, which needs both          */
    }
    g_vcpu.gprs.rax = ra; g_vcpu.gprs.rbx = rb;
    g_vcpu.gprs.rcx = rc; g_vcpu.gprs.rdx = rd;
}

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
static int host_osxsave(void)
{ u64 c4; __asm__ volatile ("mov %%cr4, %0" : "=r"(c4)); return (int)((c4 >> 18) & 1); }

static void lin_msr(const uno_hv_t *hv, int write)
{
    u32 idx = (u32)g_vcpu.gprs.rcx;
    u64 v = ((u64)(u32)g_vcpu.gprs.rdx << 32) | (u32)g_vcpu.gprs.rax;
    switch (idx) {
    case 0xC0000080u:                        /* EFER, a guest-state field    */
        if (write) hv->set(&g_vcpu, UNO_VR_EFER, v);
        else       v = hv->get(&g_vcpu, UNO_VR_EFER);
        break;
    case 0xC0000100u:                        /* FS_BASE                      */
        if (write) hv->set(&g_vcpu, UNO_VR_FS_BASE, v);
        else       v = hv->get(&g_vcpu, UNO_VR_FS_BASE);
        break;
    case 0xC0000101u:                        /* GS_BASE                      */
        if (write) hv->set(&g_vcpu, UNO_VR_GS_BASE, v);
        else       v = hv->get(&g_vcpu, UNO_VR_GS_BASE);
        break;
    case 0xC0000081u:                        /* STAR                         */
    case 0xC0000082u:                        /* LSTAR                        */
    case 0xC0000084u:                        /* SFMASK                       */
    case 0xC0000102u:                        /* KERNEL_GS_BASE               */
        /* THESE GO TO THE REAL MSRs, and holding them in a variable is why
         * userspace died on its first syscall. SYSCALL reads LSTAR out of the
         * machine, not out of us: a value we kept to ourselves left the guest
         * entering the HOST's syscall handler, or address zero. SWAPGS is the
         * same argument for KERNEL_GS_BASE - it exchanges with the register,
         * and an exchange with a value that was never written swaps in
         * nothing.
         *
         * Writing through is safe here for a reason specific to this OS:
         * UnoDOS is a ring-0 monolith that never executes `syscall` or
         * `swapgs`, so it has no values of its own to lose. An OS that did
         * would have to save and restore these around every entry. */
        if (write) wrmsr(idx, v); else v = rdmsr(idx);
        break;
    default:
        /* Zero for everything else. A kernel reading an MSR that answers 0 is
         * in far better shape than one reading a value we invented. */
        if (!write) v = 0;
        break;
    }
    if (!write) {
        g_vcpu.gprs.rax = (u32)v;
        g_vcpu.gprs.rdx = (u32)(v >> 32);
    }
}

int uno_hvp_linux_boot(const uno_hv_t *hv, uno_vm_linux *out)
{
    u64 carve = uno_vmm_carve_size(), entry = 0;
    const u8 *setup;
    uno_vm_cfg cfg;
    unsigned i;
    long n;

    out->loaded = 0; out->lines = 0; out->exits = 0;
    out->last = ""; out->stop_reason = 0; out->stop_rip = 0;
    out->fault_vec = 0xFFFF; out->fault_err = 0; out->fault_addr = 0;
    out->pio = 0; out->pio_n = 0;
    out->injects = 0; out->shell_ok = 0;
    g_lin_lines = 0; g_lin_last[0] = 0;
    g_lin_injects = 0; g_lin_shell_ok = 0; g_lin_lastvec = -1;

    if (!uno_vmm_probe()->slat || !carve) return 0;
    if (!carve_map(hv)) return 0;

    n = lin_load(&entry);
    if (n <= 0) { out->stop_reason = (unsigned)(-n); return 0; }
    out->loaded = n;
    g_initrd_size = lin_initrd();
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

    uno_vdev_reset();

    for (i = 0; i < sizeof cfg; i++) ((u8 *)&cfg)[i] = 0;
    cfg.mode     = UNO_VM_FLAT64;
    /* Every one of these was a whole finding, and three of them share a shape:
     * the default is never "works as on real hardware".  UNCONDITIONAL I/O
     * EXITING, because port I/O does not exit unless asked - without it the
     * guest's `in` and `out` execute NATIVELY against this machine's real
     * ports, so the kernel was not failing to write to its serial port, it was
     * writing to the HOST's.  INVPCID, because an instruction the CPU has does
     * not necessarily work in a guest, while CPUID cheerfully says it does.
     * And the CR SHADOW, because a kernel cannot boot without it: the guest's
     * CR4 must keep the virtualization bit set at all times, and Linux does not
     * know it is a guest - it writes CR4 with the bit clear a hundred bytes
     * into its entry point, which unshadowed is #GP(0) with nothing to say
     * why. */
    cfg.features = UNO_VMF_SLAT | UNO_VMF_PREEMPT | UNO_VMF_IO_EXIT
                 | UNO_VMF_INVPCID | UNO_VMF_CR_SHADOW;
    cfg.rip      = entry;
    cfg.rsp      = L_ZEROPG - 0x100;
    cfg.rflags   = 0x2;                      /* interrupts off, as required  */
    cfg.cr3      = L_PML4;
    cfg.gdt_base = L_GDT;   cfg.gdt_limit = 0x1F;
    /* Limit 0, so no exception can be delivered and the base is never
     * consulted: the kernel installs its own IDT within its first hundred
     * instructions, and until it does every fault is a triple fault. */
    cfg.idt_base = 0x2000;  cfg.idt_limit = 0;
    cfg.tr_base  = 0x3000;                   /* a page nothing else uses     */
    cfg.cs_sel   = 0x10;
    cfg.ds_sel   = 0x18;
    if (!hv->vcpu_create(&g_vcpu, &cfg)) return 0;
    g_vcpu.gprs.rsi = L_ZEROPG;              /* THE zero page, in RSI        */

    /* Something for the shell to read the moment it asks. Without it the
     * first read is end-of-file and the shell exits before printing. */
    /* A7 proves the disk with a mount and the network with a ping. devtmpfs
     * FIRST, and it is not a detail: this initramfs carries exactly one
     * device node, /dev/console (A6c), and nothing populates /dev afterwards
     * - so a perfectly working disk has no node to open, and the shell
     * reports the same "No such file or directory" for a missing node as for
     * a missing filesystem. */
    uno_vdev_serial_seed("\necho UNODOS-GUEST-SHELL-OK\nuname -a\n"
                         "busybox ls /bin\n"
                         "mount -t devtmpfs dev /dev\n"
                         "busybox mkdir -p /mnt\n"
                         "mount -t ext4 -o ro /dev/vda /mnt\n"
                         "cat /mnt/HELLO\n"
                         "busybox ip link set eth0 up\n"
                         "busybox ip addr add 10.77.0.2/24 dev eth0\n"
                         "busybox ping -c 2 -W 1 10.77.0.1\n");
    g_lin_running = 1;
    g_lin_halted = 0;
    out->lines = 0;
    out->last = g_lin_last;
    out->chars = 0;
    return 1;                                 /* placed and ready to run     */
}

/* A6e: the injection, at last through the door A4 proved.  Three gates, and
 * each is the difference between an interrupt and a corruption: the previous
 * injection must be gone (the CPU clears the valid bit on delivery, so a set
 * bit means an entry that never happened), the guest must have IF open, and
 * it must not be in an interrupt shadow.  Injection does NOT check any of that
 * on its own - VM entry delivers the event regardless, straight through
 * whatever critical section the guest thought it was protecting - which is
 * why UNO_VR_CAN_INJECT is asked first.
 *
 * ASKING AT THE RIGHT MOMENT IS NOT SOMETHING WE CAN DO BY LOOKING.  A guest
 * spends much of its life unable to take an interrupt, and a hypervisor that
 * only offers one when it happens to glance at a good moment starves it - the
 * machine has a mechanism for exactly this, and interrupt-window exiting is
 * it: arm it with a vector waiting, and the CPU exits the instant the guest
 * becomes ready.  Then delivery is prompt by construction rather than by
 * luck, which is the difference between a shell that responds and one that
 * responds when the sampling happens to line up. */
static void lin_try_inject(const uno_hv_t *hv)
{
    int vec;
    if (!uno_vdev_irq_pending()) { hv->set(&g_vcpu, UNO_VR_INTR_WINDOW, 0); return; }
    if (!hv->get(&g_vcpu, UNO_VR_CAN_INJECT)) {
        hv->set(&g_vcpu, UNO_VR_INTR_WINDOW, 1);
        return;
    }
    hv->set(&g_vcpu, UNO_VR_INTR_WINDOW, 0);
    vec = uno_vdev_irq_take();
    if (vec >= 0) {
        hv->inject(&g_vcpu, (unsigned)vec, 0, 0);
        g_lin_injects++;
        g_lin_lastvec = vec;
    }
}

/* THE IDLE LOOP IS `sti; hlt`, AND THAT IS WHY THIS EXISTS.  STI leaves an
 * interrupt shadow covering exactly one instruction, and that instruction is
 * the HLT - so the HLT exit arrives with the shadow set, and a hypervisor
 * that treats the shadow as "not now" refuses to wake the guest it just
 * stepped past the HLT of.  The guest then loops around its idle path
 * forever, taking no tick and doing no work, which is precisely how this
 * presented: no output, injections frozen, and the guest apparently busy.
 *
 * On real silicon the interrupt is delivered AT the HLT, ending it; once we
 * have consumed the HLT the shadow has done its whole job and retiring it is
 * the accurate model, not a workaround. */
static void lin_wake_shadow(const uno_hv_t *hv)
{
    hv->set(&g_vcpu, UNO_VR_INTR_SHADOW, 0);
}

/* One budgeted slice of the kernel, and everything its exits need.  This is
 * the same servicing the boot-time attempt did, moved to where a kernel can
 * actually live: called once per shell frame, for as long as it takes.
 *
 * A6a ran it inside a selftest with a three-second bound, which is fine for
 * "did it say anything" and hopeless for "did it reach userspace" - a kernel
 * that needs eight seconds is not slow, it is normal. */
int uno_hvp_linux_slice(const uno_hv_t *hv, unsigned budget_us,
                        uno_vm_linux *out)
{
    uno_vmexit ex;
    int n;

    if (!g_lin_running) return 0;

    /* Parked on a hlt.  RIP is still AT the hlt, and stays there until a
     * line rises, because delivery order is the fidelity that matters: on
     * real silicon an interrupt wakes the core, is delivered with the return
     * address AFTER the hlt, and the idle loop re-checks its condition. So
     * the RIP moves past the hlt in the same breath as the injection and
     * never before - waking the guest without a vector would send the idle
     * loop around without the tick it is waiting for. */
    if (g_lin_halted) {
        if (!uno_vdev_irq_pending()) {
            out->lines = g_lin_lines;
            out->last = g_lin_last;
            return 1;                              /* asleep is not stopped */
        }
        /* hlt is one byte */
        hv->set(&g_vcpu, UNO_VR_RIP, hv->get(&g_vcpu, UNO_VR_RIP) + 1);
        lin_wake_shadow(hv);
        g_lin_halted = 0;
    }

    /* Several entries per frame: most exits are a CPUID or a port write that
     * costs a microsecond to answer, and returning to the frame loop after
     * each one would spend the whole budget on the round trip. */
    for (n = 0; n < 512; n++) {
        lin_try_inject(hv);
        if (!hv->vcpu_run(&g_vcpu, budget_us, &ex)) { g_lin_running = 0; break; }
        /* The per-entry trace is right for a selftest that enters three times
         * and ruinous for a kernel that enters tens of thousands of times:
         * every entry is two writes to the debug console, each of which is
         * itself an exit to the far side, and the guest ends up spending its
         * slice on our narration. The same quiet flag the A3 slice sets, and
         * for the same reason - the first entry is traced, the rest are not. */
        g_vcpu.quiet = 1;
        out->exits++;
        if (ex.reason == UNO_VX_PREEMPT) break;    /* the budget is spent    */
        if (ex.reason == UNO_VX_INTR) break;
        /* The guest just became able to take one: nothing to do here, the
         * next pass through the top of this loop is the injection. */
        if (ex.reason == UNO_VX_INTR_WINDOW) continue;
        if (ex.reason == UNO_VX_CPUID) {
            lin_cpuid();
            step(hv, &ex);
            continue;
        }
        if (ex.reason == UNO_VX_RDMSR || ex.reason == UNO_VX_WRMSR) {
            lin_msr(hv, ex.reason == UNO_VX_WRMSR);
            step(hv, &ex);
            continue;
        }
        if (ex.reason == UNO_VX_IO) {
            u64 v;
            if (ex.io_string) { g_lin_running = 0; break; }
            v = g_vcpu.gprs.rax;
            out->pio++;
            out->last_port = ex.io_port;
            /* A rolling record of the last eight ports. One port says where
             * a guest is sitting; eight say what LOOP it is in, which is the
             * difference between "spinning on the interrupt-enable register"
             * and "servicing an interrupt storm that reaches it". */
            out->pio_ports[out->pio_n & 7] = (unsigned short)ex.io_port;
            out->pio_n++;
            uno_vdev_pio(ex.io_port, !ex.io_in, ex.io_size, &v, lin_sink);
            if (ex.io_in) {
                if (ex.io_size == 1)
                    g_vcpu.gprs.rax = (g_vcpu.gprs.rax & ~0xFFull) | (v & 0xFF);
                else if (ex.io_size == 2)
                    g_vcpu.gprs.rax = (g_vcpu.gprs.rax & ~0xFFFFull) | (v & 0xFFFF);
                else g_vcpu.gprs.rax = v & 0xFFFFFFFFull;
            }
            step(hv, &ex);
            continue;
        }
        if (ex.reason == UNO_VX_CR) {
            u64 *r = gpr((int)ex.cr_reg);
            if (ex.cr_access == 0 && ex.cr_num == 4 && r) {
                /* The virtualization bit is ours: the machine requires it set
                 * in the guest, and the read shadow makes the guest see the
                 * value it expects to read back.  Both halves live in the
                 * backend, because which bit that is, is the one part of this
                 * the two vendors spell differently. */
                hv->set(&g_vcpu, UNO_VR_CR4, *r);
                hv->set(&g_vcpu, UNO_VR_CR4_SHADOW, *r);
                step(hv, &ex);
                continue;
            }
            if (ex.cr_access == 0 && ex.cr_num == 0 && r) {
                hv->set(&g_vcpu, UNO_VR_CR0, *r);
                hv->set(&g_vcpu, UNO_VR_CR0_SHADOW, *r);
                step(hv, &ex);
                continue;
            }
            g_lin_running = 0; break;
        }
        if (ex.reason == UNO_VX_XSETBV) {
            /* XCR0 is machine state neither vendor carries in either
             * direction, so a guest that enabled AVX would enable it for the
             * host too and leave it that way.  The backend applies the guest's
             * value around its entries and puts the host's back afterwards. */
            if (host_osxsave() && (u32)g_vcpu.gprs.rcx == 0)
                hv->set(&g_vcpu, UNO_VR_XCR0,
                        ((u64)(u32)g_vcpu.gprs.rdx << 32) | (u32)g_vcpu.gprs.rax);
            step(hv, &ex);
            continue;
        }
        if (ex.reason == UNO_VX_NPF && mmio_service(hv, &ex)) continue;
        if (ex.reason == UNO_VX_HLT) {
            /* A6d stopped the run here, and for a guest with no interrupt
             * controller that was the honest reading: a hlt nothing can end.
             * With the 8259 in, hlt means what it means - wait. A line
             * already up ends it now (step past, and the loop's next entry
             * injects); otherwise the guest parks, costing nothing per frame
             * until a line rises. */
            if (uno_vdev_irq_pending()) {
                step(hv, &ex);
                lin_wake_shadow(hv);
                continue;
            }
            g_lin_halted = 1;
            break;
        }
        /* A guest that stops EARLY is otherwise invisible here: the periodic
         * trace fires every 16384 exits, so a kernel that dies in its first
         * dozen says nothing at all and reads as one that never ran. */
        g_lin_running = 0;
        out->stop_reason = (unsigned)ex.raw;
        out->stop_rip = ex.rip;
        tracex("[lin] STOPPED, exit=", ex.raw);
        tracex("[lin]   at rip=", ex.rip);
        tracex("[lin]   after exits=", (u64)out->exits);
        break;
    }
    out->lines = g_lin_lines;
    out->last = g_lin_last;
    out->chars = uno_vdev_serial_chars();
    out->injects = g_lin_injects;
    out->shell_ok = g_lin_shell_ok;
    /* On CHANGE rather than on a schedule. The periodic trace below fires
     * every 16384 exits, which is a fine cadence for "is it still going" and
     * useless for "did the first packet ever arrive" - the sample that
     * matters can easily land before the guest gets to the command. This is
     * what found the held-receive-queue bug (S-HV-46). */
    {   u64 ns = uno_vdev_net_stats();
        if (ns != g_lin_netstats) { g_lin_netstats = ns; tracex("[lin] net!=", ns); }
    }
    /* On the debug console rather than the kernel log: a kernel that stops
     * printing may be spinning on a port nobody emulated, and the boot log is
     * only flushed on a timer this run does not reach. */
    if ((out->exits >> 14) != g_lin_mark) {
        g_lin_mark = out->exits >> 14;
        /* exits | lines | port | LCR,IER,MCR,IMR, in one line: between them
         * they separate "stopped", "spinning on a port nobody emulated" and
         * "still talking, to a register we are not listening on". */
        u64 ring = 0;
        int k;
        for (k = 0; k < 4; k++)              /* four ports fit in a word     */
            ring = (ring << 16) | out->pio_ports[(out->pio_n + 4 + k) & 7];
        /* Five numbers, each of which failed differently during A6e and each
         * of which is unreadable without the others. Lines that stop while
         * exits climb is a guest running and saying nothing; injections that
         * stop while the PIC shows a line up is a guest being starved of the
         * interrupt it is waiting for (which is how the idle loop's STI
         * shadow was found); and the port ring is the LOOP rather than the
         * instant, which is what separates "spinning on a register" from
         * "servicing an interrupt that reaches it". */
        {   /* The virtio register traffic, streamed from a cursor: a driver
             * reports its conclusion ("device refuses features") rather than
             * what it saw, and the conversation is the evidence. */
            u64 v;
            while ((v = uno_vdev_dbg_entry(g_lin_vdump)) != 0) {
                g_lin_vdump++;
                tracex("[lin]   vdev=", v);
            }
        }
        tracex("[lin] exits=", (u64)out->exits);
        tracex("[lin]   lines=", (u64)out->lines);
        tracex("[lin]   ports=", ring);
        tracex("[lin]   picstate=", uno_vdev_pic_state());
        tracex("[lin]   net=", uno_vdev_net_stats());
        tracex("[lin]   injects=", (u64)g_lin_injects);
        tracex("[lin]   rip=", hv->get(&g_vcpu, UNO_VR_RIP));
    }
    return g_lin_running;
}
