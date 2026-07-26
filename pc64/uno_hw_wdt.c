/* unodevices - PCH TCO hardware watchdog.  See HWWATCHDOG.md for the contract
 * and the chipset details; uno_hw_wdt.h for the four-call API.
 *
 * Reference: Linux drivers/watchdog/iTCO_wdt.c + drivers/mfd/lpc_ich.c, and the
 * Intel ICH9 / 5-series PCH datasheets (the TCO register block, the LPC ACPI
 * base, and the RCBA General Control & Status NO_REBOOT bit).
 *
 * OWNS this file (kernel/unodevices).  CONSUMES pc64_pci.c's config accessors
 * and the uno_devmgr device tree to LOCATE the PCH LPC function - it does not
 * edit either.  Compiled only under -DUNO_DEBUG (build.sh); the header stubs it
 * out entirely in prod, so nothing here is in the shipped OS.
 *
 * -------------------------------------------------------------------------
 * WHAT ACTUALLY RESETS THE BOX (the make-or-break details, condensed):
 *
 *  1. NO_REBOOT.  Firmware normally sets the chipset's NO_REBOOT bit so a TCO
 *     timeout counts to zero but never resets.  We MUST clear it, and its home
 *     is generation-specific:
 *       - ICH6 .. ~6-series PCH ("v2"): RCBA General Control & Status (GCS),
 *         at RCBA+0x3410, bit 5.  RCBA is an MMIO window whose base is in LPC
 *         config 0xF0 (bit 0 = enable).  We clear it and READ IT BACK; if it
 *         won't clear, we report absent.
 *       - Skylake-and-later PCH, and the SoC parts (Apollo/Gemini Lake): the
 *         bit moved into the PMC (GEN_PMCON, via a PMC MMIO window), reached
 *         differently per part.  This driver does not yet implement that path,
 *         so on those chips discovery fails the read-back check and present()
 *         returns 0 rather than pretending to guard.  See HWWATCHDOG.md.
 *
 *  2. TCOBASE.  The TCO I/O block base.  On the v1/v2 parts it is the LPC ACPI
 *     base (LPC config 0x40, a.k.a. PMBASE/ABASE, bit 0 = enable) + 0x60.
 *
 *  3. v1 vs v2 register layout differs; we branch on it (see the offsets).
 *
 *  4. TWO-TIMEOUT behaviour.  A classic TCO reboots only on the SECOND
 *     uncleared timeout (the first just sets status / optionally raises SMI).
 *     So we program each single-timeout period near seconds/2: two expiries
 *     then land close to the requested backstop.  We leave the firmware's TCO
 *     SMI routing (SMI_EN.TCO_EN) untouched - the second-timeout reboot does
 *     not depend on it, and disabling firmware SMIs is riskier than sizing.
 *
 *  5. Reload/halt.  Program the period into TCO_TMR, write TCO_RLD to load the
 *     down-counter, clear TCO1_CNT.TCO_TMR_HLT (bit 11) to run; write TCO_RLD
 *     to pet; set TCO_TMR_HLT to disarm.  Tick is ~0.6 s.
 *
 *  6. Firmware may be using the TCO for its own watchdog while attached; we
 *     take it over cleanly (halt, clear status, reprogram) at init.
 * ------------------------------------------------------------------------- */
#ifdef UNO_DEBUG

#include "uno_hw_wdt.h"
#include "pc64_pci.h"
#include "uno_devmgr.h"
#include <stdint.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* --- I/O + MMIO primitives (host gate replaces these via HWWDT_HOSTTEST) ---- */
#ifdef HWWDT_HOSTTEST
u8   hwwdt_test_in8(u32 port);
u16  hwwdt_test_in16(u32 port);
void hwwdt_test_out8(u32 port, u8 v);
void hwwdt_test_out16(u32 port, u16 v);
u32  hwwdt_test_mmio_rd(u64 pa);
void hwwdt_test_mmio_wr(u64 pa, u32 v);
#define IO_IN8(p)      hwwdt_test_in8(p)
#define IO_IN16(p)     hwwdt_test_in16(p)
#define IO_OUT8(p, v)  hwwdt_test_out8((p), (v))
#define IO_OUT16(p, v) hwwdt_test_out16((p), (v))
#define MMIO_RD32(pa)     hwwdt_test_mmio_rd(pa)
#define MMIO_WR32(pa, v)  hwwdt_test_mmio_wr((pa), (v))
#else
static inline void io_out8(u32 p, u8 v)
{ __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"((u16)p)); }
static inline u8 io_in8(u32 p)
{ u8 v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"((u16)p)); return v; }
static inline void io_out16(u32 p, u16 v)
{ __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"((u16)p)); }
static inline u16 io_in16(u32 p)
{ u16 v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"((u16)p)); return v; }
#define IO_IN8(p)      io_in8(p)
#define IO_IN16(p)     io_in16(p)
#define IO_OUT8(p, v)  io_out8((p), (v))
#define IO_OUT16(p, v) io_out16((p), (v))
#define MMIO_RD32(pa)     (*(volatile u32 *)(uintptr_t)(pa))
#define MMIO_WR32(pa, v)  (*(volatile u32 *)(uintptr_t)(pa) = (v))
#endif

/* --- register map ---------------------------------------------------------- */

/* LPC/eSPI (ISA-bridge) config registers.  ABASE + ACPI_CNTL are stable across
 * ICH .. Comet Lake; the decode-enable is ACPI_CNTL bit 7, NOT a bit of ABASE
 * (ABASE bit 0 is just the I/O-space indicator - accidentally 1 on QEMU ich9,
 * but 0 on CML, which is why checking it there wrongly read "decode off"). */
#define LPC_ACPI_BASE   0x40    /* ABASE/PMBASE: I/O base of the ACPI block     */
#define LPC_ACPI_BASE_MASK 0xFF80u
#define LPC_ACPI_CNTL   0x44    /* ACPI_CNTL                                    */
#define LPC_ACPI_EN     0x80    /* ACPI_CNTL bit 7 = ACPI I/O decode enabled    */
#define LPC_RCBA        0xF0    /* Root Complex Base Address (MMIO)            */
#define LPC_RCBA_EN     0x01    /* bit 0 = RCBA enabled                        */

#define ACPI_TCOBASE_OFF 0x60   /* v2: TCO I/O block = LPC ABASE + 0x60         */

/* v3 (Skylake..CML): the TCO I/O base is NOT off the LPC ABASE - it lives in the
 * SMBus function (00:1f.4, class 0c/05) config space, register TCOBASE (0x50,
 * bits 15:5), gated by TCOCTL (0x54) bit 8 = TCO_BASE_EN.  (Confirmed on the
 * live CML-U Yoga: LPC 0x40/0x44 read 0; the TCO base is the SMBus one.) */
#define SMBUS_CLASS      0x0C
#define SMBUS_SUBCLASS   0x05
#define SMBUS_TCOBASE    0x50   /* SMBus config: TCO I/O base (& 0xFFE0)        */
#define SMBUS_TCOCTL     0x54   /* SMBus config: bit 8 = TCO_BASE_EN           */
#define SMBUS_TCO_BASE_EN (1u<<8)
#define SMBUS_TCOBASE_MASK 0xFFE0u

#define RCBA_GCS        0x3410  /* General Control & Status (MMIO, off RCBA)   */
#define GCS_NO_REBOOT   (1u<<5) /* GCS bit 5 = No Reboot (NR)  [iTCO v2, 0x20] */

/* Skylake .. Comet Lake PCH-LP (400-series): RCBA is gone; NO_REBOOT moved into
 * the PMC's GEN_PMCON_A register, in the PWRM (power-management) MMIO window.
 * PWRMBASE is the PMC function's BAR0 (00:14.2 on CML), or the fixed platform
 * base when firmware didn't surface a readable BAR.  GEN_PMCON_A is at
 * PWRMBASE + 0x1020; the no-reboot bit is bit 1 (mask 0x02) - the value Linux
 * iTCO_wdt uses for this "memory-mapped" PCH class (no_reboot_bit() -> 0x02 for
 * the Skylake-family part).  Verified against the live Comet Lake-U Yoga on
 * metal (see HWWATCHDOG.md §4). */
#define PMC_CLASS        0x05   /* PMC enumerates as class 05/00 "memory"       */
#define PMC_SUBCLASS     0x00
#define PMC_BAR0         0x10   /* PMC config: PWRMBASE lives in BAR0           */
#define PWRMBASE_FIXED   0xFE000000u  /* 400-series fixed platform PWRM window   */
#define PWRM_GEN_PMCON_A 0x1020 /* GEN_PMCON_A offset within the PWRM window    */
#define PMCON_A_NO_REBOOT (1u<<1)     /* GEN_PMCON_A bit 1 = No Reboot (0x02)    */

/* TCO I/O block offsets (relative to TCOBASE) */
#define TCO_RLD         0x00    /* 16-bit: write reloads counter from TMR      */
#define TCOv1_TMR       0x01    /* 8-bit  v1 timer initial value (6 bits)      */
#define TCO1_STS        0x04    /* 16-bit: bit 3 = TIMEOUT                      */
#define TCO2_STS        0x06    /* 16-bit: bit1 = SECOND_TO, bit0 = BOOT/INTRD */
#define TCO1_CNT        0x08    /* 16-bit: bit 11 = TCO_TMR_HLT                 */
#define TCOv2_TMR       0x12    /* 16-bit v2 timer initial value (10 bits)     */

#define TCO1_STS_TIMEOUT   (1u<<3)
#define TCO2_STS_SECOND_TO (1u<<1)
#define TCO2_STS_BOOT      (1u<<0)
#define TCO1_CNT_HLT       (1u<<11)  /* TCO_TMR_HLT: 1 = timer halted            */
#define TCO1_CNT_LOCK      (1u<<12)  /* TCO_LOCK: firmware froze TCO1_CNT[11:0]  */

/* TCO ticks are ~0.6 s.  Represent the period internally in ticks. */
#define TCO_TICK_NUM 3          /* seconds-per-tick = 3/5 = 0.6 s              */
#define TCO_TICK_DEN 5
#define TCOv1_MIN 0x04
#define TCOv1_MAX 0x3F          /* 6-bit                                        */
#define TCOv2_MIN 0x04
#define TCOv2_MAX 0x3FF         /* 10-bit                                       */

/* --- discovered state ------------------------------------------------------ */

enum { WDT_NONE = 0, WDT_V1 = 1, WDT_V2 = 2, WDT_V3 = 3 };
/* V2 = RCBA-GCS NO_REBOOT (ICH6..6-series PCH, QEMU ich9).
 * V3 = PMC GEN_PMCON_A NO_REBOOT (Skylake..Comet Lake PCH-LP). */

static int  g_probed;           /* discovery has run                            */
static int  g_gen;              /* WDT_NONE / WDT_V1 / WDT_V2 / WDT_V3           */
static u32  g_tcobase;          /* TCO I/O block base, 0 if none                */
static u64  g_nrreg;            /* NO_REBOOT MMIO reg: GCS (v2) or GEN_PMCON_A (v3) */
static u32  g_nrmask;           /* NO_REBOOT bit mask for the detected gen       */
static u32  g_nrbefore;         /* raw NO_REBOOT reg value as firmware left it   */
static int  g_present;          /* usable + NO_REBOOT confirmed clear           */
static int  g_armed;
static u16  g_period_ticks;     /* single-timeout period currently programmed   */
/* discovery diagnostics (dumped by status, so an absent result is explainable
 * from one URC round-trip on a new chipset rather than guessed at) */
static u16  g_lpc_ven, g_lpc_dev;
static u32  g_abase_raw, g_actl_raw;             /* v2: LPC ABASE + ACPI_CNTL     */
static u32  g_smbus_tcobase_raw, g_smbus_tcoctl_raw;  /* v3: SMBus TCOBASE/TCOCTL */
static u16  g_tco1_cnt_raw;                       /* TCO1_CNT as firmware left it  */
static const char *g_absent_why = "not probed";

/* seconds -> single-timeout ticks, halving for the two-timeout reboot, clamped */
static u16 secs_to_ticks(unsigned seconds, u16 lo, u16 hi)
{
    /* half the requested window per single timeout (two must expire to reset) */
    u64 half = (u64)seconds * TCO_TICK_DEN / (2u * TCO_TICK_NUM);
    if (half < lo) half = lo;
    if (half > hi) half = hi;
    return (u16)half;
}

/* Clear the chipset NO_REBOOT bit (in whichever register the detected gen puts
 * it - RCBA GCS for v2, PMC GEN_PMCON_A for v3) and confirm it reads back clear.
 * Returns 1 on success (reboot now permitted), 0 if the bit could not be cleared
 * or this generation's NO_REBOOT home is unsupported.  Records the firmware
 * value in g_nrbefore for the status dump (diagnosis on a new chipset). */
static int clear_no_reboot(void)
{
    u32 v;
    if (!g_nrreg || !g_nrmask) return 0;   /* v1 / unsupported: never pretend    */
    v = MMIO_RD32(g_nrreg);
    g_nrbefore = v;
    if (v & g_nrmask) {
        MMIO_WR32(g_nrreg, v & ~g_nrmask);
        v = MMIO_RD32(g_nrreg);            /* read back: a locked bit stays set  */
    }
    return (v & g_nrmask) ? 0 : 1;
}

/* Locate the PMC's PWRM MMIO window (Skylake..CML).  Requires an actual Intel
 * PMC function in the tree (class 05/00, vendor 8086 - 00:14.2 on CML) before
 * touching any PWRM address: that keeps us from poking a fixed MMIO on a
 * chipset that isn't this family.  Prefer the PMC's BAR0 if it reads back a sane
 * high window; else the 400-series fixed base.  Returns 0 when no PMC is found. */
static u32 find_pwrmbase(void)
{
    uno_device *d = devmgr_find_class(PMC_CLASS, PMC_SUBCLASS);
    pci_dev pmc;
    u32 bar;
    if (!d || d->bus_type != UNO_BUS_PCI || d->vendor != 0x8086) return 0;
    pmc.bus = d->addr.pci.bus; pmc.dev = d->addr.pci.dev; pmc.fn = d->addr.pci.fn;
    pmc.vendor = d->vendor; pmc.device = d->device;
    bar = pci_cfg_read32(&pmc, PMC_BAR0) & ~0xFu;       /* memory BAR base        */
    if (bar >= 0xF0000000u && bar < 0xFF000000u) return bar;   /* sane PWRM       */
    return PWRMBASE_FIXED;                  /* PMC present, BAR hidden: fixed base */
}

/* v3 TCO I/O base: the SMBus function's TCOBASE register (00:1f.4 config 0x50),
 * gated by TCOCTL (0x54) bit 8.  Records the raw reads for the status dump.
 * Returns the I/O base, or 0 if no enabled TCOBASE. */
static u32 find_tcobase_smbus(void)
{
    uno_device *d = devmgr_find_class(SMBUS_CLASS, SMBUS_SUBCLASS);
    pci_dev sm;
    u32 base, ctl;
    if (!d || d->bus_type != UNO_BUS_PCI || d->vendor != 0x8086) return 0;
    sm.bus = d->addr.pci.bus; sm.dev = d->addr.pci.dev; sm.fn = d->addr.pci.fn;
    sm.vendor = d->vendor; sm.device = d->device;
    base = pci_cfg_read32(&sm, SMBUS_TCOBASE);
    ctl  = pci_cfg_read32(&sm, SMBUS_TCOCTL);
    g_smbus_tcobase_raw = base; g_smbus_tcoctl_raw = ctl;
    if (!(ctl & SMBUS_TCO_BASE_EN)) return 0;           /* TCO base not enabled   */
    return base & SMBUS_TCOBASE_MASK;
}

/* Locate the PCH LPC function via the device tree and read its bases.  Uses the
 * enumerated uno_devmgr registry (the request's "get it via the device tree you
 * already enumerate"); falls back to a direct class scan only if the tree is
 * empty. */
static void probe(void)
{
    pci_dev lpc;
    uno_device *d, *d0;
    u32 abase, actl, rcba;
    int lpc_idx = -1;

    g_probed = 1;
    g_gen = WDT_NONE; g_tcobase = 0; g_nrreg = 0; g_nrmask = 0; g_nrbefore = 0;
    g_present = 0; g_abase_raw = 0; g_actl_raw = 0; g_lpc_ven = 0; g_lpc_dev = 0;
    g_absent_why = "no LPC bridge";

    d = devmgr_find_class(0x06, 0x01);          /* ISA bridge = the PCH LPC     */
    if (d && d->bus_type == UNO_BUS_PCI) {
        lpc.bus = d->addr.pci.bus; lpc.dev = d->addr.pci.dev; lpc.fn = d->addr.pci.fn;
        lpc.vendor = d->vendor; lpc.device = d->device;
        d0 = devmgr_get(0);
        if (d0) lpc_idx = (int)(d - d0);         /* index for the platform node  */
    } else if (!pci_find_class(0x06, 0x01, &lpc)) {
        return;                                  /* no LPC bridge: no TCO here   */
    }
    g_lpc_ven = lpc.vendor; g_lpc_dev = lpc.device;

    /* Intel only: the ACPI-base + RCBA + TCO layout below is Intel PCH.  A
     * non-Intel south bridge (e.g. AMD FCH) has a different watchdog entirely. */
    g_absent_why = "non-Intel LPC";
    if (lpc.vendor != 0x8086) return;

    /* The generation split is RCBA: present => ICH6..6-series PCH (v2, TCO off
     * the LPC ABASE, NO_REBOOT in RCBA GCS); absent => Skylake..CML PCH-LP (v3,
     * TCO off the SMBus TCOBASE, NO_REBOOT in the PMC GEN_PMCON_A). */
    rcba = pci_cfg_read32(&lpc, LPC_RCBA);
    if (rcba & LPC_RCBA_EN) {                     /* v2: LPC ABASE + RCBA GCS      */
        abase = pci_cfg_read32(&lpc, LPC_ACPI_BASE);
        actl  = pci_cfg_read32(&lpc, LPC_ACPI_CNTL);
        g_abase_raw = abase; g_actl_raw = actl;
        g_absent_why = "ACPI decode off (ACPI_CNTL bit7)";
        if (!(actl & LPC_ACPI_EN)) return;
        g_tcobase = (abase & LPC_ACPI_BASE_MASK) + ACPI_TCOBASE_OFF;
        g_absent_why = "no ABASE";
        if ((abase & LPC_ACPI_BASE_MASK) == 0) { g_tcobase = 0; return; }
        g_nrreg  = (u64)(rcba & 0xFFFFC000u) + RCBA_GCS;
        g_nrmask = GCS_NO_REBOOT;
        g_gen = WDT_V2;
    } else {                                       /* v3: SMBus TCOBASE + PMC       */
        u32 pwrm;
        g_absent_why = "no SMBus TCOBASE (TCOCTL bit8)";
        g_tcobase = find_tcobase_smbus();
        if (!g_tcobase) return;
        g_absent_why = "no RCBA + no PMC (NO_REBOOT home unknown)";
        pwrm = find_pwrmbase();
        if (!pwrm) { g_gen = WDT_V1; return; }     /* TCO found but can't reboot it */
        g_nrreg  = (u64)pwrm + PWRM_GEN_PMCON_A;
        g_nrmask = PMCON_A_NO_REBOOT;
        g_gen = WDT_V3;
    }

    /* Take the TCO over cleanly and prove it is actually usable.  Record the
     * firmware TCO1_CNT (its TCO_LOCK bit says whether firmware froze the timer
     * control - e.g. a UEFI that ran the equivalent of coreboot's tco_lockdown).
     * Then require BOTH: NO_REBOOT clears, AND the halt bit can be cleared (the
     * timer can be un-halted).  A locked-halted TCO can never fire, so claiming
     * it would be exactly the false-guard the honesty contract forbids. */
    g_tco1_cnt_raw = IO_IN16(g_tcobase + TCO1_CNT);
    IO_OUT16(g_tcobase + TCO1_CNT, g_tco1_cnt_raw | TCO1_CNT_HLT);   /* halt      */

    g_absent_why = (g_gen == WDT_V1) ? "no RCBA + no PMC (NO_REBOOT home unknown)"
                                     : "NO_REBOOT would not clear (locked?)";
    if (!clear_no_reboot()) { g_present = 0; goto done; }

    if (g_tco1_cnt_raw & TCO1_CNT_LOCK) {         /* firmware locked the control  */
        g_absent_why = "TCO1_CNT firmware-locked (TCO_LOCK)";
        g_present = 0; goto done;
    }
    {   /* prove HLT is clearable, then re-halt (arm() runs it for real) */
        u16 cnt = IO_IN16(g_tcobase + TCO1_CNT);
        IO_OUT16(g_tcobase + TCO1_CNT, (u16)(cnt & ~TCO1_CNT_HLT));
        if (IO_IN16(g_tcobase + TCO1_CNT) & TCO1_CNT_HLT) {
            g_absent_why = "TCO halt bit stuck (timer can't run)";
            g_present = 0; goto done;
        }
        IO_OUT16(g_tcobase + TCO1_CNT, (u16)(IO_IN16(g_tcobase + TCO1_CNT) | TCO1_CNT_HLT));
    }
    g_present = 1;
    g_absent_why = "present";
done:;

    /* Make the TCO visible in the device tree (merge-gate item): a platform
     * node under the LPC function, bound to this driver.  Class 08/80 = system;
     * the 32-byte TCO I/O block is its BAR.  Only when we found it via the tree
     * (lpc_idx >= 0) and it is actually usable. */
    if (g_present && lpc_idx >= 0)
        devmgr_add_platform(lpc_idx, 0x08, 0x80, g_tcobase, 0x20, "tco-wdt");
}

static void ensure_probed(void) { if (!g_probed) probe(); }

int uno_hw_wdt_present(void)
{
    ensure_probed();
    return g_present;
}

void uno_hw_wdt_arm(unsigned seconds)
{
    u16 ticks, cnt;
    ensure_probed();
    if (!g_present) return;

    ticks = (g_gen == WDT_V1) ? secs_to_ticks(seconds, TCOv1_MIN, TCOv1_MAX)
                              : secs_to_ticks(seconds, TCOv2_MIN, TCOv2_MAX);
    g_period_ticks = ticks;

    /* clear any stale timeout/boot status (write-1-to-clear) so a prior
     * expiry does not count toward this arm's two-timeout budget */
    IO_OUT16(g_tcobase + TCO1_STS, TCO1_STS_TIMEOUT);
    IO_OUT16(g_tcobase + TCO2_STS, TCO2_STS_SECOND_TO | TCO2_STS_BOOT);

    if (g_gen == WDT_V1) {
        u8 t = (u8)IO_IN8(g_tcobase + TCOv1_TMR);
        t = (u8)((t & ~0x3Fu) | (ticks & 0x3F));
        IO_OUT8(g_tcobase + TCOv1_TMR, t);
    } else {
        u16 t = IO_IN16(g_tcobase + TCOv2_TMR);
        t = (u16)((t & ~0x3FFu) | (ticks & 0x3FF));
        IO_OUT16(g_tcobase + TCOv2_TMR, t);
    }
    IO_OUT16(g_tcobase + TCO_RLD, 1);             /* load counter from TMR       */

    cnt = IO_IN16(g_tcobase + TCO1_CNT);
    IO_OUT16(g_tcobase + TCO1_CNT, (u16)(cnt & ~TCO1_CNT_HLT));   /* run          */
    g_armed = 1;
}

void uno_hw_wdt_pet(void)
{
    if (!g_present || !g_armed) return;
    /* clear the first-timeout status so the two-timeout progression restarts,
     * then reload the counter to the full period */
    IO_OUT16(g_tcobase + TCO1_STS, TCO1_STS_TIMEOUT);
    IO_OUT16(g_tcobase + TCO_RLD, 1);
}

void uno_hw_wdt_disarm(void)
{
    u16 cnt;
    if (!g_present || !g_armed) return;
    cnt = IO_IN16(g_tcobase + TCO1_CNT);
    IO_OUT16(g_tcobase + TCO1_CNT, (u16)(cnt | TCO1_CNT_HLT));    /* halt          */
    g_armed = 0;
}

/* --- introspection --------------------------------------------------------- */

static int s_cat(char *b, int cap, int at, const char *s)
{ while (*s && at < cap - 1) b[at++] = *s++; if (at < cap) b[at] = 0; return at; }
static int s_hex(char *b, int cap, int at, u64 v, int digits)
{
    static const char H[] = "0123456789abcdef";
    char t[17]; int i;
    if (digits > 16) digits = 16;
    for (i = digits - 1; i >= 0; i--) { t[i] = H[v & 0xF]; v >>= 4; }
    t[digits] = 0;
    return s_cat(b, cap, at, t);
}
static int s_dec(char *b, int cap, int at, u64 v)
{
    char t[21]; int i = 0;
    if (!v) return s_cat(b, cap, at, "0");
    while (v && i < 20) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i-- > 0 && at < cap - 1) b[at++] = t[i];
    if (at < cap) b[at] = 0;
    return at;
}

int uno_hw_wdt_status(char *buf, int cap)
{
    int at = 0;
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    ensure_probed();
    at = s_cat(buf, cap, at, "tco ");
    at = s_cat(buf, cap, at, g_gen == WDT_V3 ? "v3" : g_gen == WDT_V2 ? "v2"
                           : g_gen == WDT_V1 ? "v1" : "none");
    at = s_cat(buf, cap, at, g_present ? " present" : " absent");
    if (!g_present) {                            /* why, for a new-chipset probe */
        at = s_cat(buf, cap, at, " (");
        at = s_cat(buf, cap, at, g_absent_why);
        at = s_cat(buf, cap, at, ")");
    }
    if (g_lpc_ven) {
        at = s_cat(buf, cap, at, " lpc=");
        at = s_hex(buf, cap, at, g_lpc_ven, 4);
        at = s_cat(buf, cap, at, ":");
        at = s_hex(buf, cap, at, g_lpc_dev, 4);
    }
    if (g_abase_raw || g_actl_raw) {
        at = s_cat(buf, cap, at, " abase=0x");
        at = s_hex(buf, cap, at, g_abase_raw, 8);
        at = s_cat(buf, cap, at, " acpi_cntl=0x");
        at = s_hex(buf, cap, at, g_actl_raw, 8);
    }
    if (g_smbus_tcobase_raw || g_smbus_tcoctl_raw) {
        at = s_cat(buf, cap, at, " smb_tcobase=0x");
        at = s_hex(buf, cap, at, g_smbus_tcobase_raw, 8);
        at = s_cat(buf, cap, at, " smb_tcoctl=0x");
        at = s_hex(buf, cap, at, g_smbus_tcoctl_raw, 8);
    }
    if (g_tcobase) {
        at = s_cat(buf, cap, at, " tcobase=0x");
        at = s_hex(buf, cap, at, g_tcobase, 4);
        at = s_cat(buf, cap, at, " tco1_cnt_fw=0x");
        at = s_hex(buf, cap, at, g_tco1_cnt_raw, 4);
        if (g_tco1_cnt_raw & TCO1_CNT_LOCK) at = s_cat(buf, cap, at, "(LOCKED)");
    }
    if (g_nrreg) {
        /* the NO_REBOOT register: GCS (v2) or GEN_PMCON_A (v3).  Dump the raw
         * firmware value + live state - on a new chipset this tells you which
         * bit firmware set, so a wrong g_nrmask is diagnosable, not silent. */
        at = s_cat(buf, cap, at, g_gen == WDT_V3 ? " gen_pmcon_a=0x" : " gcs=0x");
        at = s_hex(buf, cap, at, g_nrreg, 8);
        at = s_cat(buf, cap, at, " fw=0x");
        at = s_hex(buf, cap, at, g_nrbefore, 8);
        at = s_cat(buf, cap, at, MMIO_RD32(g_nrreg) & g_nrmask ? " NO_REBOOT=1"
                                                               : " NO_REBOOT=0");
    }
    at = s_cat(buf, cap, at, g_armed ? " armed period=" : " idle period=");
    at = s_dec(buf, cap, at, g_period_ticks);
    at = s_cat(buf, cap, at, "t");
    if (g_present && g_tcobase) {
        at = s_cat(buf, cap, at, " rld=");
        at = s_dec(buf, cap, at, IO_IN16(g_tcobase + TCO_RLD) & 0x3FF);
    }
    return at;
}

/* Expose the discovered TCOBASE so the driver can register the TCO as a
 * platform node in the device tree once discovery has run (see the devmgr
 * registration in uefi_main wiring / the harness).  Returns 0 if absent. */
u32 uno_hw_wdt_tcobase(void) { ensure_probed(); return g_present ? g_tcobase : 0; }

/* --- command dispatch (uno.hwwdt binding + operator/QEMU trigger) ----------- */

static int tok_eq(const char *a, const char *b)   /* a starts with token b + sep */
{
    while (*b) { if (*a++ != *b++) return 0; }
    return *a == 0 || *a == ' ';
}
static unsigned parse_uint(const char *s)
{
    unsigned v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') v = v * 10u + (unsigned)(*s++ - '0');
    return v;
}
static const char *after_tok(const char *s)
{
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    return s;
}

/* the IRQs-off tight spin the software guard cannot recover from */
static void wedge_irqs_off(void)
{
#ifdef HWWDT_HOSTTEST
    /* the host gate must not actually hang */
#else
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("pause");
#endif
}

int uno_hw_wdt_cmd(const char *line, char *out, int cap)
{
    if (!line) line = "status";
    while (*line == ' ') line++;

    if (!*line || tok_eq(line, "status") || tok_eq(line, "present"))
        return uno_hw_wdt_status(out, cap);

    if (tok_eq(line, "arm")) {
        unsigned s = parse_uint(after_tok(line));
        uno_hw_wdt_arm(s);
        return uno_hw_wdt_status(out, cap);
    }
    if (tok_eq(line, "pet"))    { uno_hw_wdt_pet();    return uno_hw_wdt_status(out, cap); }
    if (tok_eq(line, "disarm")) { uno_hw_wdt_disarm(); return uno_hw_wdt_status(out, cap); }

    if (tok_eq(line, "selftest")) {
        unsigned s = parse_uint(after_tok(line));
        if (!s) s = 8;
        uno_hw_wdt_arm(s);
        (void)uno_hw_wdt_status(out, cap);   /* echo state before we never return */
        wedge_irqs_off();                    /* cli; spin - only the TCO recovers */
        return out ? (int)0 : 0;
    }
    if (tok_eq(line, "wedge")) {
        if (out && cap > 0) { const char *m = "wedging (no arm)"; int i=0; while(m[i]&&i<cap-1){out[i]=m[i];i++;} out[i]=0; }
        wedge_irqs_off();
        return 0;
    }
    if (out && cap > 0) out[0] = 0;
    return -1;
}

/* Boot-time self-demonstration, opt-in via a DEBUG.CFG key, so the hardware
 * backstop can be exercised end-to-end (QEMU q35 ich9-lpc, and metal) WITHOUT
 * any of the unoautomate-side guard wiring in place yet:
 *
 *   hw-wdt-selftest[=<seconds>]   arm the TCO, then cli-spin forever.
 *
 * The software guard cannot recover a cli-spin (no ISR, no loop), so ONLY a
 * working TCO resets the box - which is exactly the claim to prove.  With QEMU
 * -no-reboot a TCO reset makes QEMU exit; on metal the box reboots and (once
 * URC is configured) re-dials home.  Appended as ONE guarded call at the end of
 * the debug boot block in uefi_main.c (AGENTS.md §2 additive seam).  A no-op if
 * the key is absent or no usable TCO was found. */
#ifndef HWWDT_HOSTTEST
int pc64_stress_cfg_flag(const char *key);            /* consumed (uno_debug.c) */
int pc64_stress_cfg_value(const char *key, char *buf, int cap);

void uno_hw_wdt_boot_selftest(void)
{
    char v[16];
    unsigned secs = 8;
    if (pc64_stress_cfg_flag("hw-wdt-selftest") <= 0) return;
    if (pc64_stress_cfg_value("hw-wdt-selftest", v, sizeof v) > 0) {
        unsigned s = parse_uint(v);
        if (s) secs = s;
    }
    if (!uno_hw_wdt_present()) return;    /* no usable TCO here: don't hang       */
    uno_hw_wdt_arm(secs);
    wedge_irqs_off();                     /* only the TCO can end this            */
}
#endif

#endif /* UNO_DEBUG */
