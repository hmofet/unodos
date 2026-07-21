/* ===========================================================================
 * UnoDOS/pc64 - opt-in MTRR rebuild for a write-combining framebuffer (P3).
 *
 * F3 established that the GOP framebuffer is covered by a variable MTRR of
 * type UC on every machine tested, so present is uncached (~14-44 MB/s vs
 * ~2 GB/s to RAM) and dominates the frame. The safe wins (P0 spans, wider
 * stores, GOP Blt) are banked; this is the 100x fix, and the one with real
 * bricking risk if wrong - so it is OPT-IN (STRESS.CFG `mtrr-wc`), runs with
 * the operator present, logs before/after, and REFUSES rather than guess.
 *
 * Why it is hard (SDM 11.11): you cannot just add a WC MTRR over the fb - on
 * a UC-and-WC overlap UC wins. The covering UC range must stop covering the
 * fb. So we TILE the covering range into power-of-2 UC blocks around the fb
 * window and mark the fb window WC. If that needs more variable MTRRs than
 * the CPU has free, or the geometry isn't one we can tile safely, we do
 * nothing. The whole change is volatile (MTRRs reset on power-cycle) and we
 * snapshot + can restore in-session.
 * ======================================================================== */
#include "pc64_mtrr.h"
#include "uno_debug.h"

typedef unsigned long long u64;
typedef unsigned int u32;

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

#define MTRR_CAP        0xFE
#define MTRR_DEF_TYPE   0x2FF
#define MTRR_PHYSBASE(i) (0x200u + 2u * (i))
#define MTRR_PHYSMASK(i) (0x201u + 2u * (i))
#define PHYS_MASK_BITS  0x000FFFFFFFFFF000ull   /* bits 12..51 (52-bit phys)  */

/* snapshot for restore */
static u64 g_save_base[16], g_save_mask[16], g_save_def;
static int g_saved, g_vcnt;
static int g_applied;

/* flush TLBs (SDM 11.11.8 step: toggle CR4.PGE if set, else reload CR3) -
 * without this the fb's already-mapped pages keep their OLD effective memory
 * type and the WC change silently does nothing */
static void flush_tlb(void)
{
    u64 cr4, cr3;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ull << 7)) {                         /* PGE set: toggle it */
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4 & ~(1ull << 7)));
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    } else {
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3));
    }
}

/* the "atomic MTRR change" fence (SDM 11.11.8): cli; CD=1,NW=0; flush caches;
 * flush TLBs; disable MTRRs -> [reprogram] -> flush caches; flush TLBs;
 * enable MTRRs; restore CR0; sti */
static void mtrr_prologue(u64 *saved_cr0)
{
    u64 cr0;
    __asm__ volatile ("cli");
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    *saved_cr0 = cr0;
    cr0 = (cr0 | (1ull << 30)) & ~(1ull << 29);      /* CD=1, NW=0 */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("wbinvd");
    flush_tlb();
    wrmsr(MTRR_DEF_TYPE, rdmsr(MTRR_DEF_TYPE) & ~(1ull << 11));  /* E=0 */
}
static void mtrr_epilogue(u64 new_def, u64 saved_cr0)
{
    __asm__ volatile ("wbinvd");
    flush_tlb();
    wrmsr(MTRR_DEF_TYPE, new_def | (1ull << 11));    /* E=1, chosen default */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(saved_cr0));
    __asm__ volatile ("sti");
}

/* emit UC power-of-2 blocks tiling [lo, hi) into (base[],mask[]) from *n;
 * returns 0 on success, -1 if it would exceed cap. Each block is aligned to
 * its own size, the standard address-range->MTRR decomposition. */
static int emit_uc(u64 lo, u64 hi, u64 *base, u64 *mask, int *n, int cap, u64 phys_hi)
{
    while (lo < hi) {
        u64 size, algn, remain;
        /* largest power-of-2 that is <= remaining AND aligned to lo */
        algn = lo ? (lo & (~lo + 1)) : (1ull << 63);     /* lowest set bit */
        remain = hi - lo;
        size = 1ull;
        while ((size << 1) <= remain && (size << 1) <= algn) size <<= 1;
        if (*n >= cap) return -1;
        base[*n] = (lo & PHYS_MASK_BITS) | 0x00;         /* type UC (0) */
        /* phys_hi is the FULL 52-bit width mask - truncating it to u32 (the
         * bug this replaced) dropped bits 32-51, so the UC tile matched
         * 4 GB-aliased addresses and made real >4 GB RAM uncacheable */
        mask[*n] = ((~(size - 1)) & PHYS_MASK_BITS & phys_hi) | (1ull << 11);
        (*n)++;
        lo += size;
    }
    return 0;
}

int uno_pc64_mtrr_wc_fb(unsigned long long fb_base, unsigned long long fb_size)
{
    u64 cap = rdmsr(MTRR_CAP);
    int vcnt = (int)(cap & 0xFF), i, hit = -1, keep = 0;
    u64 cov_lo = 0, cov_hi = 0;
    u64 nbase[16], nmask[16];
    int nn = 0;
    u32 phys_hi_lo, phys_hi_hi;
    u64 phys_hi;
    u64 fb_lo, fb_hi, wc_size;

    if (g_applied) { uno_dbg_net_trace("mtrr-wc: already applied"); return 0; }
    if (vcnt <= 0 || vcnt > 16) { uno_dbg_net_trace("mtrr-wc: VCNT=%d unusable", vcnt); return -1; }
    if (!(cap & (1ull << 10))) { uno_dbg_net_trace("mtrr-wc: WC type not supported by this CPU"); return -1; }

    /* max phys addr width from cpuid 0x80000008 -> build the mask high bits */
    __asm__ volatile ("cpuid" : "=a"(phys_hi_lo), "=b"(phys_hi_hi)
                      : "a"(0x80000008u) : "ecx", "edx");
    { int pab = phys_hi_lo & 0xFF; if (pab < 36 || pab > 52) pab = 36;
      phys_hi = (pab >= 52) ? ~0ull : ((1ull << pab) - 1); }

    /* snapshot every variable MTRR + the default type */
    g_vcnt = vcnt;
    g_save_def = rdmsr(MTRR_DEF_TYPE);
    for (i = 0; i < vcnt; i++) {
        g_save_base[i] = rdmsr(MTRR_PHYSBASE(i));
        g_save_mask[i] = rdmsr(MTRR_PHYSMASK(i));
    }
    g_saved = 1;

    /* find the UC variable MTRR that covers the fb base */
    for (i = 0; i < vcnt; i++) {
        u64 pb = g_save_base[i], pm = g_save_mask[i];
        if (!(pm & (1ull << 11))) continue;                 /* not valid */
        if ((fb_base & pm & PHYS_MASK_BITS) == (pb & pm & PHYS_MASK_BITS)) {
            if ((pb & 0xFF) == 0) {                          /* UC - our target */
                u64 m = pm & PHYS_MASK_BITS;
                u64 sz = (~m & PHYS_MASK_BITS) + 1;          /* range size */
                cov_lo = pb & PHYS_MASK_BITS;
                cov_hi = cov_lo + sz;
                hit = i;
            }
        }
    }
    if (hit < 0) {
        uno_dbg_net_trace("mtrr-wc: no UC variable MTRR covers fb_base %llx - "
                          "fb may already be WB/WC (nothing to do) or the range "
                          "is a fixed MTRR we won't touch", fb_base);
        return -1;
    }

    /* the WC window: the fb extent, snapped OUT to a power-of-2 block that
     * stays inside the covering range and does not swallow neighbours. We
     * only handle the clean case: a single power-of-2 window >= fb_size that
     * is aligned within [cov_lo,cov_hi). Anything messier -> refuse. */
    wc_size = 0x1000;
    while (wc_size < fb_size) wc_size <<= 1;
    fb_lo = fb_base & ~(wc_size - 1);
    fb_hi = fb_lo + wc_size;
    if (fb_lo < cov_lo || fb_hi > cov_hi) {
        uno_dbg_net_trace("mtrr-wc: fb window [%llx,%llx) not contained in the "
                          "covering UC range [%llx,%llx) - refusing (would need "
                          "multi-range tiling; run with operator + a plan)",
                          fb_lo, fb_hi, cov_lo, cov_hi);
        return -1;
    }

    /* Build the replacement set: keep every OTHER valid MTRR as-is, drop the
     * covering one, tile [cov_lo,fb_lo) and [fb_hi,cov_hi) as UC, and add the
     * fb window as WC. */
    for (i = 0; i < vcnt; i++) {
        u64 pb, pm, mrange, mtype;
        if (i == hit) continue;
        if (!(g_save_mask[i] & (1ull << 11))) continue;     /* invalid: reuse */
        pb = g_save_base[i]; pm = g_save_mask[i];
        mtype = pb & 0xFF;
        /* A SECOND MTRR overlapping the fb window with a non-UC type would make
         * the fb an SDM-UNDEFINED type combination (WC-vs-WB). UC-vs-WC is
         * defined (UC wins, WC just won't take - safe). Refuse the undefined
         * case rather than program it. */
        if (mtype != 0) {
            mrange = (~(pm & PHYS_MASK_BITS) & PHYS_MASK_BITS) + 1;
            u64 mlo = pb & PHYS_MASK_BITS, mhi = mlo + mrange;
            if (fb_lo < mhi && mlo < fb_hi) {
                uno_dbg_net_trace("mtrr-wc: a 2nd MTRR (type %llu) overlaps the fb "
                                  "window - undefined type combo, refusing", mtype);
                return -1;
            }
        }
        if (nn >= 16) { uno_dbg_net_trace("mtrr-wc: too many MTRRs to preserve"); return -1; }
        nbase[nn] = pb;
        nmask[nn] = pm;
        nn++; keep++;
    }
    if (emit_uc(cov_lo, fb_lo, nbase, nmask, &nn, vcnt, phys_hi) < 0 ||
        emit_uc(fb_hi, cov_hi, nbase, nmask, &nn, vcnt, phys_hi) < 0) {
        uno_dbg_net_trace("mtrr-wc: tiling the UC remainder needs > %d MTRRs - "
                          "refusing", vcnt);
        return -1;
    }
    if (nn >= vcnt) {   /* need one more slot for the WC window */
        uno_dbg_net_trace("mtrr-wc: no free MTRR slot for the WC window "
                          "(need %d, have %d) - refusing", nn + 1, vcnt);
        return -1;
    }
    nbase[nn] = (fb_lo & PHYS_MASK_BITS) | 0x01;            /* type WC (1) */
    nmask[nn] = ((~(wc_size - 1)) & PHYS_MASK_BITS & phys_hi) | (1ull << 11);
    nn++;

    uno_dbg_net_trace("mtrr-wc: applying %d MTRRs (fb WC [%llx,%llx), UC remainder "
                      "tiled, %d preserved) - operator-triggered experiment",
                      nn, fb_lo, fb_hi, keep);

    {
        u64 cr0;
        mtrr_prologue(&cr0);
        for (i = 0; i < vcnt; i++) {
            if (i < nn) { wrmsr(MTRR_PHYSBASE(i), nbase[i]); wrmsr(MTRR_PHYSMASK(i), nmask[i]); }
            else        { wrmsr(MTRR_PHYSBASE(i), 0);       wrmsr(MTRR_PHYSMASK(i), 0); }
        }
        /* pass the FULL saved DEF_TYPE (its default-type field is the
         * firmware's WB=6). The old `& ~0xFF` ZEROED the field to UC=0, so
         * every address not covered by an explicit WB MTRR - i.e. most RAM -
         * went uncacheable the instant this ran (machine-wide crawl). The
         * epilogue re-sets the E bit; we keep the type unchanged. */
        mtrr_epilogue(g_save_def, cr0);
    }
    g_applied = 1;
    return 0;
}

void uno_pc64_mtrr_restore(void)
{
    u64 cr0;
    int i;
    if (!g_saved || !g_applied) return;
    mtrr_prologue(&cr0);
    for (i = 0; i < g_vcnt; i++) {
        wrmsr(MTRR_PHYSBASE(i), g_save_base[i]);
        wrmsr(MTRR_PHYSMASK(i), g_save_mask[i]);
    }
    mtrr_epilogue(g_save_def, cr0);
    g_applied = 0;
    uno_dbg_net_trace("mtrr-wc: restored the firmware MTRR set");
}
