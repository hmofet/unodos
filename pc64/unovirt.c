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
#include "uefi.h"
#include "bootinfo.h"
#include <stdio.h>

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
    return n;
}
