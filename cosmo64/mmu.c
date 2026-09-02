/* cosmo64/mmu.c -- identity map + caches for pc64-on-ARM (M1).
 *
 * The asm port runs cache-off forever; pc64 cannot (313k lines of C want a
 * D-cache). Map:
 *   0x00000000 - 0x40000000  Device-nGnRnE, XN   (all MT6771 MMIO, incl TOPRGU)
 *   0x40000000 - 0x7C000000  Normal WB cacheable (DRAM: image, stack, heap, DTB)
 *                            except 0x54400000 - 0x54600000, Normal-NC: the
 *                            kernel's persistent-RAM reservations, where the
 *                            debug log lives (log.c)
 *   0x7C000000 - 0x80000000  Normal non-cacheable (LK reserves the framebuffer
 *                            top-down under 2 GB -- NC keeps the display DMA
 *                            coherent with no per-frame cache cleans, and still
 *                            write-combines on Cortex-A)
 *
 * 4 KB granule, 39-bit VA (T0SZ=25): one L1 with a 1 GB Device block and one
 * 1 GB L2 of 2 MB blocks. Tables live in this file's .bss (4 KB aligned; built
 * with the MMU off, so -mstrict-align keeps the builder itself legal).
 */

typedef unsigned int u32;
typedef unsigned long long u64;

void cpu_early_init(void);
void dcache_inv_all(void);
void mmu_on(u64 ttbr0, u64 mair, u64 tcr);

/* MAIR indices */
#define AI_DEVICE 0   /* 0x00 Device-nGnRnE       */
#define AI_WB     1   /* 0xFF Normal WB read/write-allocate */
#define AI_NC     2   /* 0x44 Normal non-cacheable */
#define MAIR_VAL 0x000000000044FF00ull

#define BLOCK_VALID 0x1ull
#define TABLE_VALID 0x3ull
#define AF (1ull << 10)
#define SH_INNER (3ull << 8)
#define ATTRIDX(i) ((u64)(i) << 2)
#define UXN (1ull << 54)
#define PXN (1ull << 53)

/* TCR: T0SZ=25 (39-bit), 4K granule, WB WA walks, inner-shareable, TTBR1 off,
 * 40-bit IPA */
#define TCR_VAL (25ull | (1ull << 8) | (1ull << 10) | (3ull << 12) \
                 | (1ull << 23) | (2ull << 32))

/* THE SAME MAP, FOR EL2 -- because that is where LK actually leaves us.
 * Measured 2026-09-01: CurrentEL=2, SCTLR_EL2 = 0x30c50830, i.e. M=0 C=0 I=0.
 * Every SCTLR_EL1/TTBR0_EL1/TCR_EL1 write this file has made since M1 went
 * into registers that govern a level this payload never runs at, so the MMU
 * and both caches have been OFF since first light and all memory has been
 * Device-nGnRnE. That is why a volatile add loop costs 315 ns an iteration,
 * why everything must build -mstrict-align (Device memory faults on unaligned
 * access), and why faults "wedge silently at a level EL1 vectors never see".
 *
 * TCR_EL2 is NOT TCR_EL1 with a different name when HCR_EL2.E2H is 0: the
 * physical-address size lives in PS[18:16] rather than IPS[34:32], there is no
 * TTBR1 half to disable, and bits 23 and 31 are RES1. Getting that wrong is a
 * hang, so the two layouts are spelled out separately rather than shared. */
#define TCR_EL2_VAL (25ull | (1ull << 8) | (1ull << 10) | (3ull << 12) \
                     | (2ull << 16) | (1ull << 23) | (1ull << 31))

static unsigned cur_el(void)
{
    u64 v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (unsigned)((v >> 2) & 3);
}

static int e2h_set(void)
{
    u64 v;
    __asm__ volatile("mrs %0, hcr_el2" : "=r"(v));
    return (v >> 34) & 1;                   /* E2H: EL2 uses the EL1 layout */
}

static u64 l1[512] __attribute__((aligned(4096)));
static u64 l2[512] __attribute__((aligned(4096)));

void mmu_init(void)
{
    cpu_early_init();  /* idempotent; entry.s already ran it before c_main */

    l1[0] = 0x00000000ull | BLOCK_VALID | AF | ATTRIDX(AI_DEVICE) | UXN | PXN;
    l1[1] = (u64)l2 | TABLE_VALID;
    for (u32 i = 0; i < 512; i++) {
        u64 pa = 0x40000000ull + (u64)i * 0x200000;
        /* 0x54400000: the kernel's persistent-RAM reservations, which hold the
         * debug log (log.c). Non-cacheable for the same reason as the
         * framebuffer, but the other way round: a warm reset does not flush
         * the D-cache, so a write-back mapping would leave the last lines of
         * the log -- the ones naming whatever went wrong -- stranded in it. */
        if (pa >= 0x7C000000ull || pa == 0x54400000ull)
            l2[i] = pa | BLOCK_VALID | AF | ATTRIDX(AI_NC) | UXN | PXN;
        else
            l2[i] = pa | BLOCK_VALID | AF | ATTRIDX(AI_WB) | SH_INNER;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    dcache_inv_all();
    /* At EL2 with E2H clear the register layout differs; with E2H set, EL2
     * borrows the EL1 layout and the ordinary value is right. mmu_on() picks
     * the matching registers from CurrentEL for itself. */
    u64 tcr = (cur_el() == 2 && !e2h_set()) ? TCR_EL2_VAL : TCR_VAL;
    mmu_on((u64)l1, MAIR_VAL, tcr);
}
