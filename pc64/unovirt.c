/* ===========================================================================
 * unovirt - the capability gate.  See unovirt.h and pc64/UNOVIRT.md.
 *
 * Phase A0 of docs/UNOVIRT-PLAN.md.  Reads CPUID and the read-only capability
 * MSRs, sums usable RAM, and answers "could an appliance run here".  It writes
 * nothing, enters nothing, and cannot fault: every MSR it touches is one the
 * corresponding CPUID bit guarantees exists.
 *
 * THE ORDER OF THE CHECKS IS THE POINT.  On a machine whose firmware turned
 * virtualization off, CPUID.1:ECX[5] still reads 1 - the feature is in the
 * silicon, it is the MSR that says it may not be used.  A gate that stopped at
 * CPUID would report "supported" and then fault on VMXON, which is the single
 * most common way this goes wrong and the reason the lock bit gets its own
 * blocker with wording aimed at the person who can fix it (in firmware setup,
 * not in the OS).
 * ======================================================================== */
#include "unovirt.h"
#include "unovirt_hv.h"
#include "uefi.h"
#include "bootinfo.h"
#include <stdio.h>
#include <string.h>
#include "pc64_native.h"     /* uno_native_rdtsc: the slice clock            */
#include "uno_debug.h"       /* the heartbeat, and the calibrated TSC rate   */

typedef unsigned int       u32;
typedef unsigned long long u64;

void *uno_pc64_st(void);                /* EFI_SYSTEM_TABLE   (uefi_main.c) */
int   uno_pc64_detached(void);

static uno_vm_caps g_caps;

/* ---- the two instructions this file is built on --------------------------
 * cpuid clobbers all four registers, so the leaf goes in through eax/ecx and
 * everything comes back; rdmsr is safe here only because each call site is
 * guarded by the CPUID bit that makes the MSR architectural.  There is no
 * wrmsr in this file, deliberately: A0 does not change the machine's state,
 * so a probe on an ineligible machine leaves no trace. */
static void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
    u32 ra, rb, rc, rd;
    __asm__ volatile ("cpuid"
                      : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                      : "a"(leaf), "c"(sub));
    if (a) *a = ra;
    if (b) *b = rb;
    if (c) *c = rc;
    if (d) *d = rd;
}

static u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

/* ---- Intel ---------------------------------------------------------------- */
#define IA32_FEATURE_CONTROL   0x3Au
#define FC_LOCK                (1ull << 0)
#define FC_VMXON_SMX           (1ull << 1)
#define FC_VMXON               (1ull << 2)   /* VMXON outside SMX             */

#define IA32_VMX_BASIC         0x480u
#define IA32_VMX_PINBASED      0x481u
#define IA32_VMX_PROCBASED     0x482u
#define IA32_VMX_ENTRY_CTLS    0x484u
#define IA32_VMX_PROCBASED2    0x48Bu
#define IA32_VMX_EPT_VPID_CAP  0x48Cu

/* In every VMX capability MSR the HIGH dword is the allowed-1 mask: a control
 * may be set only where that bit is 1.  Reading the low half instead (the
 * allowed-0 mask) is a classic way to conclude a feature is missing on a
 * machine that has it. */
#define ALLOWED1(m)            ((u32)(rdmsr(m) >> 32))

#define PIN_PREEMPT_TIMER      (1u << 6)
#define PROC_SECONDARY         (1u << 31)
#define PROC2_EPT              (1u << 1)
#define PROC2_VPID             (1u << 5)
#define PROC2_UNRESTRICTED     (1u << 7)
#define PROC2_VIRQ_DELIVERY    (1u << 9)
#define ENTRY_IA32E_GUEST      (1u << 9)

#define EPTCAP_WALK4           (1ull << 6)
#define EPTCAP_WB              (1ull << 14)
#define EPTCAP_2M              (1ull << 16)
#define EPTCAP_1G              (1ull << 17)

/* ---- AMD ------------------------------------------------------------------ */
#define MSR_VM_CR              0xC0010114u
#define VM_CR_LOCK             (1ull << 3)
#define VM_CR_SVMDIS           (1ull << 4)

#define SVM_EDX_NP             (1u << 0)     /* nested paging                 */
#define SVM_EDX_NRIP           (1u << 3)     /* next-RIP saved on #VMEXIT     */
#define SVM_EDX_AVIC           (1u << 13)

/* ---- how much RAM is actually here ---------------------------------------
 * Two paths, because the two boot worlds answer this differently and neither
 * can answer for the other.  Under UEFI the firmware's map is authoritative
 * and only readable while boot services live, which is why the boot path
 * probes during init rather than lazily on first use; on the BIOS path the
 * loader's E820 copy survives forever.
 *
 * Only genuinely free memory counts.  Firmware-reserved, ACPI and MMIO
 * descriptors are not RAM an appliance could ever be given, and counting them
 * would make a 2 GiB machine look like a 4 GiB one at exactly the size where
 * the floor decides.  Boot-services memory is excluded too: it comes back at
 * detach, but the carve is reserved BEFORE that. */
#define MD_CONVENTIONAL 7                    /* EfiConventionalMemory         */

static u64 ram_mb_uefi(void)
{
    static unsigned char map[48 * 1024];
    EFI_SYSTEM_TABLE *st = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    typedef EFI_STATUS (*GMM_FN)(UINTN *, void *, UINTN *, UINTN *, UINT32 *);
    UINTN sz = sizeof map, key = 0, dsz = 0, off;
    UINT32 ver = 0;
    u64 pages = 0;

    if (!st || uno_pc64_detached()) return 0;
    if (((GMM_FN)st->BootServices->GetMemoryMap)(&sz, map, &key, &dsz, &ver)
        != EFI_SUCCESS)
        return 0;
    if (dsz < 40 || sz > sizeof map) return 0;   /* a descriptor is >= 40 B   */
    for (off = 0; off + dsz <= sz; off += dsz) {
        const unsigned char *d = map + off;
        u32 type    = *(const u32 *)d;
        u64 npages  = *(const u64 *)(d + 24);
        if (type == MD_CONVENTIONAL) pages += npages;
    }
    return (pages * 4096ull) >> 20;
}

static u64 ram_mb_bios(const uno_bootinfo *bi)
{
    const uno_e820 *e;
    u64 bytes = 0;
    u32 i;
    if (!bi || !bi->mmap_addr || !bi->mmap_count) return 0;
    e = (const uno_e820 *)(u64)bi->mmap_addr;
    for (i = 0; i < bi->mmap_count; i++)
        if (e[i].type == UNO_E820_USABLE) bytes += e[i].length;
    return bytes >> 20;
}

/* ---- the probe ------------------------------------------------------------ */

static void brand(char *out)
{
    u32 r[12];
    u32 max_ext = 0, i;
    const char *s;
    char *p = out;
    out[0] = 0;
    cpuid(0x80000000u, 0, &max_ext, 0, 0, 0);
    if (max_ext < 0x80000004u) return;
    for (i = 0; i < 3; i++)
        cpuid(0x80000002u + i, 0, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
    s = (const char *)r;
    while (*s == ' ') s++;                       /* Intel pads the front      */
    for (i = 0; i < 47 && s[i]; i++) p[i] = s[i];
    p[i] = 0;
    while (i > 0 && p[i - 1] == ' ') p[--i] = 0; /* ...and sometimes the back */
}

const uno_vm_caps *uno_vmm_probe(void)
{
    uno_vm_caps *c = &g_caps;
    u32 a = 0, b = 0, cx = 0, d = 0, maxleaf = 0, maxext = 0;

    if (c->probed) return c;
    c->probed = 1;

    cpuid(0, 0, &maxleaf, &b, &cx, &d);
    brand(c->cpu);
    cpuid(1, 0, &a, &b, &cx, &d);
    c->in_hypervisor = (cx >> 31) & 1;
    cpuid(0x80000000u, 0, &maxext, 0, 0, 0);
    if (maxext >= 0x80000008u) {
        u32 ea = 0;
        cpuid(0x80000008u, 0, &ea, 0, 0, 0);
        c->phys_bits = (int)(ea & 0xFF);
    }

    /* RAM first: it is the one input that is time-sensitive (the UEFI map
     * dies at ExitBootServices) and it is needed whichever vendor this is. */
    {   const uno_bootinfo *bi = uno_pc64_bootinfo();
        c->ram_mb = bi ? ram_mb_bios(bi) : ram_mb_uefi();
    }

    if ((cx >> 5) & 1) {                          /* CPUID.1:ECX[5] = VMX    */
        u32 pin, proc, proc2 = 0, entry;
        c->vendor = UNO_HV_VMX;
        c->fc     = rdmsr(IA32_FEATURE_CONTROL);
        c->basic  = rdmsr(IA32_VMX_BASIC);
        c->rev    = (u32)(c->basic & 0x7FFFFFFFu);

        pin   = ALLOWED1(IA32_VMX_PINBASED);
        proc  = ALLOWED1(IA32_VMX_PROCBASED);
        entry = ALLOWED1(IA32_VMX_ENTRY_CTLS);
        /* PROCBASED2 exists only when the primary controls allow activating
         * it.  Reading it unconditionally is a #GP on older parts, and EPT
         * lives entirely inside it - so "no secondary controls" and "no EPT"
         * are the same answer, reached without faulting to find out. */
        if (proc & PROC_SECONDARY) proc2 = ALLOWED1(IA32_VMX_PROCBASED2);

        c->preempt_timer = (pin   & PIN_PREEMPT_TIMER) ? 1 : 0;
        c->long_guest    = (entry & ENTRY_IA32E_GUEST) ? 1 : 0;
        c->slat          = (proc2 & PROC2_EPT) ? 1 : 0;
        c->vpid          = (proc2 & PROC2_VPID) ? 1 : 0;
        c->unrestricted  = (proc2 & PROC2_UNRESTRICTED) ? 1 : 0;
        c->apicv         = (proc2 & PROC2_VIRQ_DELIVERY) ? 1 : 0;
        if (c->slat) {
            c->eptcap  = rdmsr(IA32_VMX_EPT_VPID_CAP);
            c->slat_wb = (c->eptcap & EPTCAP_WB) ? 1 : 0;
            c->slat_2m = (c->eptcap & EPTCAP_2M) ? 1 : 0;
            c->slat_1g = (c->eptcap & EPTCAP_1G) ? 1 : 0;
            /* A 4-level walk is not optional for us: the carve is described
             * with 4 KiB granularity inside 2 MiB blocks, and the only other
             * walk length the architecture defines is 5-level, which no part
             * shipping today reports without also reporting 4. */
            if (!(c->eptcap & EPTCAP_WALK4)) c->slat = 0;
        }
    } else if (maxext >= 0x80000001u) {
        u32 ec = 0;
        cpuid(0x80000001u, 0, 0, 0, &ec, 0);
        if ((ec >> 2) & 1) {                      /* CPUID.80000001:ECX[2]   */
            u32 ea = 0, eb = 0, ed = 0;
            c->vendor = UNO_HV_SVM;
            c->fc     = rdmsr(MSR_VM_CR);
            cpuid(0x8000000Au, 0, &ea, &eb, 0, &ed);
            c->rev   = ea & 0xFFu;
            c->asids = eb;
            c->slat  = (ed & SVM_EDX_NP) ? 1 : 0;
            c->nrip  = (ed & SVM_EDX_NRIP) ? 1 : 0;
            c->apicv = (ed & SVM_EDX_AVIC) ? 1 : 0;
            c->vpid  = eb > 1;                    /* ASIDs are AMD's VPIDs   */
            /* NPT entries carry no memory type of their own - the guest PAT
             * and the host MTRRs decide - so write-back is not a capability
             * to test for, it is the default.  Long mode likewise: any part
             * with SVM has it. */
            c->slat_wb = c->slat;
            c->slat_2m = c->slat;
            c->slat_1g = c->slat;
            c->long_guest = 1;
            /* SVM has no preemption timer.  The slice clock on AMD is an
             * intercepted local-APIC one-shot instead (A3), which is why this
             * stays 0 rather than being fudged to 1. */
        }
    }
    return c;
}

/* ---- the verdict ---------------------------------------------------------- */

/* The appliance floor, and the carve steps.  docs/UNOVIRT-PLAN.md §3.4 states
 * these against INSTALLED RAM; the thresholds here sit deliberately below the
 * round numbers because what we can measure is FREE CONVENTIONAL memory, and
 * the firmware has always taken a bite out of it by the time we ask.  A 4 GiB
 * machine reports about 3.9 GiB under OVMF and less under a vendor firmware
 * with a large reserved region; comparing that against 4000 would drop it a
 * whole step and nobody would ever see why. */
#define VM_FLOOR_MB 1800u        /* below this: no appliance at all           */

unsigned uno_vmm_carve_mb(void)
{
    u64 r = uno_vmm_probe()->ram_mb;
    if (r < VM_FLOOR_MB)  return 0;
    if (r < 3500u)        return 768u;    /* nominally a 2 GiB machine        */
    if (r < 7000u)        return 1536u;   /* nominally 4 GiB                  */
    return 2048u;                         /* 8 GiB and up                     */
}

int uno_vmm_eligible(unsigned *blockers)
{
    const uno_vm_caps *c = uno_vmm_probe();
    unsigned m = 0;

    if (c->vendor == UNO_HV_NONE) {
        m |= UNO_VMB_NO_CPU;
        if (c->in_hypervisor) m |= UNO_VMB_NESTED;
    } else {
        if (c->vendor == UNO_HV_VMX) {
            /* Locked with VMXON-outside-SMX clear is the terminal case: the
             * lock bit is write-once per power cycle and the firmware set it.
             * Unlocked is NOT a blocker - phase A1 sets the bits itself. */
            if ((c->fc & FC_LOCK) && !(c->fc & FC_VMXON)) m |= UNO_VMB_FW_OFF;
        } else {
            /* AMD's disable bit has the same two cases: SVMDIS with LOCK is
             * final, SVMDIS without it is ours to clear. */
            if ((c->fc & VM_CR_SVMDIS) && (c->fc & VM_CR_LOCK)) m |= UNO_VMB_FW_OFF;
        }
        if (!c->slat)      m |= UNO_VMB_NO_SLAT;
        if (!c->slat_wb)   m |= UNO_VMB_NO_WB;
        if (!c->long_guest) m |= UNO_VMB_NO_LONG;
    }
    /* Not a property of the CPU: the appliance needs our own IDT, our own
     * LAPIC and a memory map nothing else is going to rewrite underneath it.
     * See docs/UNOVIRT-PLAN.md §3.2 and pc64/DETACH.md. */
    if (!uno_pc64_detached()) m |= UNO_VMB_ATTACHED;
    if (!uno_vmm_carve_mb())  m |= UNO_VMB_LOW_RAM;

    if (blockers) *blockers = m;
    return m == 0;
}

const char *uno_vmm_blocker_str(unsigned m)
{
    /* Ordered by what the reader can act on, not by bit number.  A machine
     * with three blockers is told about the one that would move first. */
    if (m & UNO_VMB_FW_OFF)
        return "firmware disabled virtualization and locked it (enable it in firmware setup)";
    if (m & UNO_VMB_NO_CPU)
        return "no VMX or SVM on this processor";
    if (m & UNO_VMB_NO_SLAT)
        return "no EPT/NPT: a guest could not be isolated";
    if (m & UNO_VMB_NO_WB)
        return "second-stage memory cannot be write-back";
    if (m & UNO_VMB_NO_LONG)
        return "no 64-bit guest support";
    if (m & UNO_VMB_LOW_RAM)
        return "not enough RAM for a guest carve";
    if (m & UNO_VMB_ATTACHED)
        return "still attached to the firmware";
    return "";
}

/* ---- A2: the carve --------------------------------------------------------
 *
 * Taken before ExitBootServices, beside uno_modload_reserve(), because
 * AllocatePages does not exist afterwards.  This is the same shape as the
 * module arena and for the same reason, with one difference worth naming: the
 * arena is small enough that firmware always has it, and a gibibyte is not.
 * So the request halves down to a floor instead of failing, and what it
 * actually got is reported rather than assumed. */
typedef EFI_STATUS (*EFI_ALLOC_PAGES)(UINTN, UINTN, UINTN, unsigned long long *);
#define EFI_LOADER_DATA 2
#define CARVE_FLOOR_MB  128u

static unsigned long long g_carve_base, g_carve_size;
static char g_carve_note[96];

unsigned long long uno_vmm_carve_base(void) { return g_carve_base; }
unsigned long long uno_vmm_carve_size(void) { return g_carve_size; }

void *uno_vmm_gpa(unsigned long long gpa, unsigned long long len)
{
    /* Both halves of the check matter, and so does their order: without the
     * `len > size` arm, a length near 2^64 makes `gpa + len` wrap to a small
     * number that passes the sum test. */
    if (!g_carve_base) return 0;
    if (len > g_carve_size) return 0;
    if (gpa > g_carve_size - len) return 0;
    return (void *)(unsigned long long)(g_carve_base + gpa);
}

void uno_vmm_reserve(void)
{
    EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    unsigned want = uno_vmm_carve_mb();
    const uno_vm_caps *c = uno_vmm_probe();

    if (g_carve_base || !want) return;
    if (c->vendor == UNO_HV_NONE) return;         /* nothing would use it    */
    if (!ST) {
        /* The BIOS path has no allocator: uno_bios_find_ram() hands out the
         * top of the highest usable run and keeps no bookkeeping, so a second
         * caller would be handed memory the module arena is already using.
         * One consumer is all that mechanism supports, and the arena is the
         * one that cannot be done without. */
        snprintf(g_carve_note, sizeof g_carve_note,
                 "no carve: BIOS boot has one bump allocator and modload owns it");
        return;
    }

    while (want >= CARVE_FLOOR_MB) {
        unsigned long long mem = 0;
        /* Two mebibytes more than asked for, so the base can be ROUNDED UP to
         * a 2 MiB boundary.  This is not tidiness: a second-stage large-page
         * entry addresses a 2 MiB frame, so bits 20:12 of the address in it
         * must be zero, and AllocatePages only ever promises 4 KiB alignment.
         * An unaligned frame address is a reserved-bit violation, which the
         * machine reports as EPT MISCONFIGURATION (exit 49) - a number that
         * says "your tables are malformed" and nothing about which field. */
        UINTN pages = (((UINTN)want << 20) + 0x200000u) >> 12;
        if (((EFI_ALLOC_PAGES)ST->BootServices->AllocatePages)
                (0 /*AnyPages*/, EFI_LOADER_DATA, pages, &mem) == EFI_SUCCESS) {
            g_carve_base = (mem + 0x1FFFFFull) & ~0x1FFFFFull;
            g_carve_size = (unsigned long long)want << 20;
            snprintf(g_carve_note, sizeof g_carve_note, "%u MB at %llx%s",
                     want, g_carve_base,
                     want < uno_vmm_carve_mb() ? " (reduced)" : "");
            return;
        }
        want /= 2;
    }
    snprintf(g_carve_note, sizeof g_carve_note,
             "no carve: firmware refused down to %u MB", CARVE_FLOOR_MB);
}

/* Is the carve write-back?  A carve the CPU treats as uncacheable is not a
 * failure anyone would notice as one: the appliance simply runs at a fraction
 * of the speed and the conclusion is "virtualization is slow" rather than
 * "the memory type is wrong" (docs/UNOVIRT-PLAN.md R5).  So it is checked and
 * printed at the point where it can still be believed.
 *
 * The variable MTRRs win over the default type, so both are read; this is a
 * READ of the same registers pc64_mtrr.c owns the writing of, which is why it
 * is six lines here rather than a request to that lane. */
#define MTRR_CAP       0xFEu
#define MTRR_DEF_TYPE  0x2FFu
static int carve_memtype(void)
{
    unsigned long long def = rdmsr(MTRR_DEF_TYPE);
    unsigned long long cap = rdmsr(MTRR_CAP);
    int n = (int)(cap & 0xFF), i;
    int type = (def & (1ull << 11)) ? (int)(def & 0xFF) : 6;  /* E=0 -> UC?  */
    if (!(def & (1ull << 11))) return 6;      /* MTRRs disabled: WB by fiat  */
    for (i = 0; i < n && i < 16; i++) {
        unsigned long long base = rdmsr(0x200u + 2u * (unsigned)i);
        unsigned long long mask = rdmsr(0x201u + 2u * (unsigned)i);
        if (!(mask & (1ull << 11))) continue;                 /* not valid   */
        if (((g_carve_base ^ base) & mask & ~0xFFFull) == 0)
            type = (int)(base & 0xFF);        /* a variable range covers it  */
    }
    return type;
}

/* A3 state, declared here so the selftest can arm it (below). */
#define SLICE_BUDGET_US 4000u
#define SLICE_TEST_N    120          /* frames, not seconds: QEMU draws slowly */

static const uno_hv_t *g_hv;
static int g_slice_armed, g_slice_n, g_slice_other;
static u64 g_slice_max_cyc, g_slice_tot_cyc;
static char g_slice_str[144];
static uno_vm_linux g_lin;               /* A6b: the kernel, running        */
static int g_lin_armed, g_lin_reported;
static char g_lin_str[200];

/* ---- A1: the foothold -----------------------------------------------------
 *
 * The banner does not say "we own the extension", it says "a guest ran and
 * control came back".  Glide's V2 makes the same distinction and it is worth
 * copying exactly: `owned` alone would only prove a branch was taken, while a
 * value that went out through a guest's registers, through its own store
 * instruction, and came back out of its memory proves entry, intercept
 * decode, register write-back, RIP advance, resume and exit - each of which
 * is otherwise a silent hang the first time something depends on it.
 *
 * The crasher runs SECOND and matters as much.  Until a guest that goes wrong
 * reports why, every later mistake in this subsystem presents identically, as
 * a machine that stops. */
#define VM_MARKER 0x534F444F4E55ull      /* 'UNODOS', little-endian          */

static char g_self[720];
static int  g_self_done, g_self_ok;

const char *uno_vmm_selftest_str(void) { return g_self; }

int pc64_stress_cfg_flag(const char *key);   /* DEBUG.CFG key set? -1 = no file */

int uno_vmm_selftest(void)
{
    const uno_vm_caps *c = uno_vmm_probe();
    const uno_hv_t *hv = 0;
    const char *why = 0;
    unsigned m = 0;
    uno_vmexit ex;
    u64 got = 0;

    if (g_self_done) return g_self_ok;
    g_self_done = 1;

    /* OPT-IN, and it stays opt-in until a first VMRUN has been survived on
     * real silicon. The precedent is `mtrr-wc` (pc64_mtrr.c): a change with a
     * genuine risk of taking the machine out runs only with an operator
     * present who asked for it, and refuses rather than guesses.
     *
     * The risk here is not theoretical and is not a fault. A VMRUN that does
     * not return leaves the core with GIF clear, which means no interrupt of
     * any kind can be delivered to it - not the LAPIC watchdog that exists to
     * catch exactly this. There is no report, no reset and no screen: the
     * machine simply stops. Nothing that behaves that way when it goes wrong
     * belongs on the default boot path of an OS somebody is using. */
    if (pc64_stress_cfg_flag("vm-selftest") != 1) {
        snprintf(g_self, sizeof g_self,
                 "not run - opt-in (DEBUG.CFG `vm-selftest`)");
        return 0;
    }

    if (!uno_vmm_eligible(&m)) {
        snprintf(g_self, sizeof g_self, "not run - %s", uno_vmm_blocker_str(m));
        return 0;
    }
    if (c->vendor == UNO_HV_SVM) hv = uno_hv_svm();
    if (c->vendor == UNO_HV_VMX) hv = uno_hv_vmx();
    g_hv = hv;                       /* the frame loop's slice needs it too */
    if (!hv) {
        snprintf(g_self, sizeof g_self, "no backend for this vendor");
        return 0;
    }
    if (!hv->enable(&why)) {
        snprintf(g_self, sizeof g_self, "%s: could not enter host mode - %s",
                 hv->name, why ? why : "?");
        return 0;
    }

    ex.reason = UNO_VX_UNKNOWN; ex.raw = 0; ex.rip = 0; ex.info1 = 0; ex.info2 = 0;
    if (!hv->marker(VM_MARKER, &got, &ex)) {
        snprintf(g_self, sizeof g_self,
                 "%s: entered, but the guest did not come back - got %llx "
                 "want %llx, last exit %llx at rip %llx",
                 hv->name, got, VM_MARKER, ex.raw, ex.rip);
        return 0;
    }
    {   /* A guest that ruins itself, and a machine that survives it. */
        uno_vmexit cr;
        int contained;
        cr.reason = UNO_VX_UNKNOWN; cr.raw = 0; cr.rip = 0; cr.info1 = 0; cr.info2 = 0;
        contained = hv->crasher(&cr);
        snprintf(g_self, sizeof g_self,
                 "%s: entered, guest round trip -> %llx OK, crasher %s "
                 "(exit %llx rip %llx)",
                 hv->name, got, contained ? "contained" : "NOT CONTAINED",
                 cr.raw, cr.rip);
        g_self_ok = contained;
    }

    /* ---- A2: the same round trip, one translation deeper ------------------
     * The addresses are the evidence and they are deliberately unalike: the
     * guest stores at a guest-physical address low enough to be obviously not
     * a real one, and the value turns up somewhere else entirely.  Had the
     * second stage been quietly off, the store would have gone to host
     * physical 0x100000 - which is the kernel's own low memory - and nothing
     * would have appeared where we looked. */
    if (g_self_ok && hv->ept) {
        uno_vmexit ex2;
        unsigned long long g2 = 0, hpa = 0;
        int ok2;
        ex2.reason = UNO_VX_UNKNOWN; ex2.raw = 0; ex2.rip = 0;
        ex2.info1 = 0; ex2.info2 = 0;
        ok2 = hv->ept(VM_MARKER + 1, 0x100000ull, &g2, &hpa, &ex2);
        {   int n = (int)strlen(g_self);
            snprintf(g_self + n, sizeof g_self - (unsigned)n,
                     "; ept %s gpa 0x100000 -> pa %llx wrote %llx (memtype %d, %s)",
                     ok2 ? "OK" : "FAILED", hpa, g2, carve_memtype(),
                     g_carve_note[0] ? g_carve_note : "no carve");
            if (!ok2) g_self_ok = 0;
        }
    } else if (g_self_ok) {
        int n = (int)strlen(g_self);
        snprintf(g_self + n, sizeof g_self - (unsigned)n,
                 "; ept not implemented for %s", hv->name);
    }

    /* ---- A4: a clock, an interrupt, and an MSR space -------------------- */
    if (g_self_ok && hv->clockirq) {
        uno_vm_clockirq k;
        int ok4 = hv->clockirq(&k);
        int n = (int)strlen(g_self);
        snprintf(g_self + n, sizeof g_self - (unsigned)n,
                 "; clock %s (+%llu over %d exits), irq %s (%d, %sredelivered), "
                 "msr %s",
                 k.t2 > k.t1 ? "ticks" : "STUCK", k.t2 - k.t1, k.exits,
                 k.irqs == 1 ? "taken once" : "WRONG", k.irqs,
                 k.redelivered ? "" : "not ",
                 k.msr_echo ? "answered" : "NOT ANSWERED");
        if (!ok4) g_self_ok = 0;
    }

    /* ---- A5: a device the guest discovers and talks to ------------------ */
    if (g_self_ok && hv->virtio) {
        uno_vm_virtio v;
        int ok5 = hv->virtio(&v);
        int n = (int)strlen(g_self);
        snprintf(g_self + n, sizeof g_self - (unsigned)n,
                 "; virtio %s magic %x, used.idx %u, %d bytes in %d notify, "
                 "cycle %s, said \"%s\"",
                 ok5 ? "OK" : "FAILED", v.magic, v.used_idx, v.bytes,
                 v.notifies, v.cycle_refused ? "refused" : "NOT REFUSED",
                 v.text ? v.text : "");
        if (!ok5) g_self_ok = 0;
    }

    /* ---- A6: place a real kernel, and let the FRAME LOOP run it --------
     * A boot needs seconds and a selftest cannot give them, so this only
     * places the kernel; uno_vmm_tick() runs it a slice at a time for as long
     * as it takes. A missing bzImage is not a failure of anything: most
     * machines will not carry one. */
    if (g_self_ok && hv->linux_boot) {
        int placed = hv->linux_boot(&g_lin);
        int n = (int)strlen(g_self);
        if (!g_lin.loaded)
            snprintf(g_self + n, sizeof g_self - (unsigned)n,
                     "; linux: no bzImage (code %u)", g_lin.stop_reason);
        else {
            snprintf(g_self + n, sizeof g_self - (unsigned)n,
                     "; linux: %ld KB placed, %s",
                     g_lin.loaded / 1024, placed ? "running" : "REFUSED");
            g_lin_armed = placed;
            snprintf(g_lin_str, sizeof g_lin_str, "starting");
        }
    }

    /* A3 is ARMED here and finished by the frame loop, because that is the
     * thing being tested: a guest running in the gaps of a desktop that keeps
     * drawing. Nothing else about the boot changes. */
    /* ONE VMCS, so one guest at a time. The spinner's setup vmclears and
     * reconfigures the same control block the kernel is using, which does not
     * fail - it silently replaces a booting Linux with two bytes of `jmp $`,
     * and the only symptom is a kernel that stops saying anything. A6b's
     * kernel wins; the A3 measurement has already been taken on machines
     * without one. */
    if (g_self_ok && !g_lin_armed && hv->spin_start && hv->spin_start()) {
        g_slice_armed = 1;
        snprintf(g_slice_str, sizeof g_slice_str, "armed, running");
    } else if (g_self_ok) {
        snprintf(g_slice_str, sizeof g_slice_str,
                 "not run - no slice clock on this backend");
    }
    return g_self_ok;
}

/* ---- A3: a guest, sliced out of the frame loop ----------------------------
 *
 * pc64 has no scheduler and one core, and that is a property worth keeping
 * rather than a limitation to route around: everything above it is written
 * with no locks because nothing can preempt it. A guest therefore does not get
 * scheduled, it gets a BUDGET - a few milliseconds of the frame the shell was
 * going to spend anyway - and the machine takes the core back whether or not
 * the guest cooperates.
 *
 * The budget is a correctness constraint, not a tuning knob. The watchdog
 * fires on a heartbeat 20 s stale and anything over 100 ms is a logged hitch
 * (S-DBG-09, S-DBG-19), so a slice has to fit inside a frame with room left
 * for the desktop to draw. 4 ms of a 16 ms frame is about a quarter of the
 * core, which is the honest number for a single-core appliance until A9 gives
 * a guest a core of its own. */
const char *uno_vmm_slice_str(void) { return g_slice_str; }

const char *uno_vmm_linux_str(void) { return g_lin_str; }

void uno_vmm_tick(void)
{
    uno_vmexit ex;
    u64 t0, dt;

    /* A6b: the kernel gets its slice first, because it is the guest with
     * somewhere to get to. The budget is the same 4 ms the spinner gets - a
     * guest is a guest, and the desktop's frame is the thing being protected. */
    if (g_lin_armed && g_hv && g_hv->linux_slice) {
        int lines = g_lin.lines;
        if (!g_hv->linux_slice(SLICE_BUDGET_US, &g_lin)) g_lin_armed = 0;
        uno_dbg_heartbeat();
        /* A kernel that stops printing has not necessarily stopped: it can
         * be spinning on a port waiting for hardware nobody emulated. The
         * exit count and the port it is sitting on say which. */
        if ((g_lin.exits & 0x3FFF) == 0)
            uno_dbg_log("vm linux: %d exits %d pio, sitting on port %x, "
                        "%d lines", g_lin.exits, g_lin.pio, g_lin.last_port,
                        g_lin.lines);
        if (g_lin.lines != lines || !g_lin_armed) {
            snprintf(g_lin_str, sizeof g_lin_str,
                     "%s: %d lines %d chars, %d exits, %d pio%s%s",
                     g_lin_armed ? "running" : "stopped",
                     g_lin.lines, g_lin.chars, g_lin.exits, g_lin.pio,
                     g_lin_armed ? "" : " on ",
                     g_lin_armed ? "" : (g_lin.stop_reason == 12 ? "hlt" : "an exit"));
            if (!g_lin_armed && !g_lin_reported) {
                g_lin_reported = 1;
                uno_dbg_log("vm linux: %s, last \"%s\"", g_lin_str,
                            g_lin.last ? g_lin.last : "");
            }
        }
        return;                  /* one guest per frame; the spinner waits  */
    }

    if (!g_slice_armed || !g_hv || !g_hv->slice) return;

    t0 = uno_native_rdtsc();
    if (!g_hv->slice(SLICE_BUDGET_US, &ex)) { g_slice_armed = 0; return; }
    dt = uno_native_rdtsc() - t0;

    /* Feed the watchdog on the far side of the slice as well as at the top of
     * the frame. The frame's own heartbeat would cover a budget this size, but
     * the guest is the one thing here that can hold the core for a length it
     * chose rather than one we did, and the heartbeat is what turns that from
     * a dead machine into a report. */
    uno_dbg_heartbeat();

    g_slice_n++;
    g_slice_tot_cyc += dt;
    if (dt > g_slice_max_cyc) g_slice_max_cyc = dt;
    if (ex.reason != UNO_VX_PREEMPT && ex.reason != UNO_VX_INTR) g_slice_other++;

    if (g_slice_n >= SLICE_TEST_N) {
        u64 per_us = uno_native_tsc_per_us();
        if (!per_us) per_us = 1;
        snprintf(g_slice_str, sizeof g_slice_str,
                 "%d slices x %u us budget: max %llu us, mean %llu us, "
                 "%d exits that were not the clock",
                 g_slice_n, SLICE_BUDGET_US,
                 g_slice_max_cyc / per_us,
                 (g_slice_tot_cyc / (u64)g_slice_n) / per_us,
                 g_slice_other);
        g_slice_armed = 0;           /* the spinner stops being entered      */
        /* The env block was built before this finished, so the kernel log is
         * where the result lands - it is flushed to BOOTLOG.TXT on the 30 s
         * heartbeat and at shutdown. */
        uno_dbg_log("vm slice: %s", g_slice_str);
    }
}

int uno_vmm_status_str(char *buf, int cap)
{
    const uno_vm_caps *c = uno_vmm_probe();
    unsigned m = 0;
    int ok = uno_vmm_eligible(&m), n;

    if (cap <= 1) { if (cap == 1) buf[0] = 0; return 0; }

    if (c->vendor == UNO_HV_NONE) {
        n = snprintf(buf, (unsigned)cap, "none (%s%s), ram %llu MB\n"
                     "            eligible: no - %s",
                     c->cpu[0] ? c->cpu : "?",
                     c->in_hypervisor ? ", inside a hypervisor" : "",
                     c->ram_mb, uno_vmm_blocker_str(m));
    } else {
        n = snprintf(buf, (unsigned)cap,
                     "%s rev 0x%x, %s%s%s%s%s%s%s%s phys=%d, ram %llu MB, carve %u MB\n"
                     "            eligible: %s%s%s",
                     c->vendor == UNO_HV_VMX ? "vmx" : "svm", c->rev,
                     c->slat ? (c->vendor == UNO_HV_VMX ? "ept" : "npt") : "NO-SLAT",
                     c->slat_wb ? " wb" : " NO-WB",
                     c->slat_2m ? " 2m" : "", c->slat_1g ? " 1g" : "",
                     c->unrestricted ? " unrestricted" : "",
                     c->vpid ? " vpid" : "",
                     c->preempt_timer ? " preempt" : "",
                     c->apicv ? " apicv" : "",
                     c->phys_bits, c->ram_mb, uno_vmm_carve_mb(),
                     ok ? "yes" : "no",
                     ok ? "" : " - ", ok ? "" : uno_vmm_blocker_str(m));
    }
    if (n < 0) { buf[0] = 0; return 0; }
    if (n >= cap) n = cap - 1;
    /* The A1 result rides on the same block rather than taking a second line
     * in the env block: one seam, one place to read. */
    if (g_self[0] && n + 4 < cap) {
        int k = snprintf(buf + n, (unsigned)(cap - n),
                         "\n            selftest: %s", g_self);
        if (k > 0) n = (n + k >= cap) ? cap - 1 : n + k;
    }
    if (g_lin_str[0] && n + 4 < cap) {
        int k = snprintf(buf + n, (unsigned)(cap - n),
                         "\n            linux: %s", g_lin_str);
        if (k > 0) n = (n + k >= cap) ? cap - 1 : n + k;
    }
    if (g_slice_str[0] && n + 4 < cap) {
        int k = snprintf(buf + n, (unsigned)(cap - n),
                         "\n            slice: %s", g_slice_str);
        if (k > 0) n = (n + k >= cap) ? cap - 1 : n + k;
    }
    return n;
}
