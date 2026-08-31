/* cosmo64/mmu.c -- identity map + caches for pc64-on-ARM (M1).
 *
 * The asm port runs cache-off forever; pc64 cannot (313k lines of C want a
 * D-cache). Map:
 *   0x00000000 - 0x40000000  Device-nGnRnE, XN   (all MT6771 MMIO, incl TOPRGU)
 *   0x40000000 - 0x7C000000  Normal WB cacheable (DRAM: image, stack, heap, DTB)
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

static u64 l1[512] __attribute__((aligned(4096)));
static u64 l2[512] __attribute__((aligned(4096)));

void mmu_init(void)
{
    cpu_early_init();  /* idempotent; entry.s already ran it before c_main */

    l1[0] = 0x00000000ull | BLOCK_VALID | AF | ATTRIDX(AI_DEVICE) | UXN | PXN;
    l1[1] = (u64)l2 | TABLE_VALID;
    for (u32 i = 0; i < 512; i++) {
        u64 pa = 0x40000000ull + (u64)i * 0x200000;
        if (pa >= 0x7C000000ull)
            l2[i] = pa | BLOCK_VALID | AF | ATTRIDX(AI_NC) | UXN | PXN;
        else
            l2[i] = pa | BLOCK_VALID | AF | ATTRIDX(AI_WB) | SH_INNER;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    dcache_inv_all();
    mmu_on((u64)l1, MAIR_VAL, TCR_VAL);
}
