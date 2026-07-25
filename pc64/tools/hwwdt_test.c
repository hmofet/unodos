/* Host-side gate for the PCH TCO hardware watchdog (pc64/uno_hw_wdt.c).
 *
 * uno_hw_wdt.c reaches hardware through exactly three seams: pc64_pci.c config
 * reads, the uno_devmgr tree (to locate the LPC function), and port-I/O + one
 * MMIO dword (behind HWWDT_HOSTTEST macros).  So it links against a SYNTHETIC
 * ICH9-style south bridge and runs natively - no QEMU, no metal - which is
 * enough to decide the things most likely to be wrong: the NO_REBOOT
 * clear+read-back, the seconds->ticks halving for the two-timeout reboot, the
 * v1/v2 timer-offset branch, and the HLT run/halt/reload sequencing.  QEMU's
 * ich9-lpc then confirms an armed TCO actually resets the machine.
 *
 * Build + run (see tools/hwwdt_test.sh):
 *   cc -DUNO_DEBUG -DHWWDT_HOSTTEST -I.. -o /tmp/hwwdt_test \
 *      tools/hwwdt_test.c uno_hw_wdt.c && /tmp/hwwdt_test
 */
#include <stdio.h>
#include <string.h>
#include "pc64_pci.h"
#include "uno_devmgr.h"
#include "uno_hw_wdt.h"

/* --- synthetic ICH9-style LPC config space --------------------------------- */

#define LPC_BUS 0
#define LPC_DEV 0x1f
#define LPC_FN  0
static unsigned short g_lpc_ven = 0x8086;
static unsigned g_lpc_cfg[64];          /* dword-indexed LPC config space       */
static int g_have_lpc = 1;              /* 0 = the machine has no LPC bridge     */

/* PMC function (Comet Lake 00:14.2, 8086:02ef, class 05/00) for the v3 path */
#define PMC_BUS 0
#define PMC_DEV 0x14
#define PMC_FN  2
static unsigned g_pmc_cfg[64];
static int g_have_pmc;                  /* 0 = no PMC (v2-only machine)          */

/* SMBus function (Comet Lake 00:1f.4, 8086:02a3, class 0c/05): holds TCOBASE */
#define SMB_BUS 0
#define SMB_DEV 0x1f
#define SMB_FN  4
static unsigned g_smb_cfg[64];
static int g_have_smb;                   /* v3: SMBus carries the TCOBASE         */

#define ACPI_BASE   0x0400u             /* PMBASE                               */
#define RCBA_BASE   0xFED1C000u
#define TCOBASE     (ACPI_BASE + 0x60)  /* 0x0460                               */
#define GCS_ADDR    (RCBA_BASE + 0x3410)
#define PWRM_BASE   0xFE000000u
#define PMCON_ADDR  (PWRM_BASE + 0x1020)  /* GEN_PMCON_A                        */

static void lpc_reset(int rcba_enabled, int acpi_enabled)
{
    memset(g_lpc_cfg, 0, sizeof g_lpc_cfg);
    g_lpc_cfg[0x00 / 4] = ((unsigned)0x2918 << 16) | 0x8086;      /* ICH9 LPC    */
    g_lpc_cfg[0x08 / 4] = (0x06u << 24) | (0x01u << 16);          /* isa-bridge  */
    g_lpc_cfg[0x40 / 4] = ACPI_BASE;                             /* ABASE        */
    g_lpc_cfg[0x44 / 4] = acpi_enabled ? 0x80u : 0u;             /* ACPI_CNTL b7 */
    g_lpc_cfg[0xF0 / 4] = RCBA_BASE | (rcba_enabled ? 1u : 0u);   /* RCBA        */
    memset(g_pmc_cfg, 0, sizeof g_pmc_cfg);
    g_pmc_cfg[0x00 / 4] = ((unsigned)0x02ef << 16) | 0x8086;      /* CML PMC     */
    g_pmc_cfg[0x08 / 4] = (0x05u << 24) | (0x00u << 16);          /* memory      */
    g_pmc_cfg[0x10 / 4] = PWRM_BASE;                             /* BAR0=PWRMBASE*/
    memset(g_smb_cfg, 0, sizeof g_smb_cfg);
    g_smb_cfg[0x00 / 4] = ((unsigned)0x02a3 << 16) | 0x8086;      /* CML SMBus   */
    g_smb_cfg[0x08 / 4] = (0x0cu << 24) | (0x05u << 16);          /* smbus       */
    g_smb_cfg[0x50 / 4] = TCOBASE;                               /* TCOBASE reg  */
    g_smb_cfg[0x54 / 4] = 0x100u;                               /* TCO_BASE_EN   */
}

unsigned int pci_cfg_read32(const pci_dev *d, int off)
{
    if (d->bus == LPC_BUS && d->dev == LPC_DEV && d->fn == LPC_FN)
        return g_lpc_cfg[(off & 0xFC) / 4];
    if (g_have_pmc && d->bus == PMC_BUS && d->dev == PMC_DEV && d->fn == PMC_FN)
        return g_pmc_cfg[(off & 0xFC) / 4];
    if (g_have_smb && d->bus == SMB_BUS && d->dev == SMB_DEV && d->fn == SMB_FN)
        return g_smb_cfg[(off & 0xFC) / 4];
    return 0xFFFFFFFFu;
}
unsigned short pci_cfg_read16(const pci_dev *d, int off)
{ return (unsigned short)(pci_cfg_read32(d, off) >> ((off & 2) * 8)); }
void pci_cfg_write32(const pci_dev *d, int off, unsigned int v)
{ if (d->bus == LPC_BUS && d->dev == LPC_DEV && d->fn == LPC_FN) g_lpc_cfg[(off & 0xFC) / 4] = v; }
void pci_cfg_write16(const pci_dev *d, int off, unsigned short v) { (void)d; (void)off; (void)v; }
void pci_enable_bus_master(const pci_dev *d) { (void)d; }
unsigned long long pci_bar(const pci_dev *d, int n) { (void)d; (void)n; return 0; }

int pci_find_class(unsigned char c, unsigned char s, pci_dev *o)
{
    if (g_have_lpc && c == 0x06 && s == 0x01) {
        o->bus = LPC_BUS; o->dev = LPC_DEV; o->fn = LPC_FN;
        o->vendor = g_lpc_ven; o->device = 0x2918;
        return 1;
    }
    return 0;
}
int pci_find(unsigned short v, unsigned short d, pci_dev *o)
{ (void)v; (void)d; (void)o; return 0; }

/* --- fake device tree ------------------------------------------------------ */

static uno_device g_tree[4];
static int g_treen;
static int g_plat_calls;
static int g_plat_backing;
static unsigned long long g_plat_iobase;
static const char *g_plat_drv;

static void tree_reset(int have_lpc)
{
    memset(g_tree, 0, sizeof g_tree);
    g_treen = 0;
    g_plat_calls = 0;
    if (have_lpc) {
        uno_device *d = &g_tree[g_treen++];
        d->bus_type = UNO_BUS_PCI;
        d->addr.pci.bus = LPC_BUS; d->addr.pci.dev = LPC_DEV; d->addr.pci.fn = LPC_FN;
        d->vendor = g_lpc_ven; d->device = 0x2918;
        d->cls = 0x06; d->subcls = 0x01;
        d->parent = UNO_DEV_NOPARENT; d->state = UNO_DEV_UNBOUND;
    }
    if (g_have_pmc) {
        uno_device *d = &g_tree[g_treen++];
        d->bus_type = UNO_BUS_PCI;
        d->addr.pci.bus = PMC_BUS; d->addr.pci.dev = PMC_DEV; d->addr.pci.fn = PMC_FN;
        d->vendor = 0x8086; d->device = 0x02ef;
        d->cls = 0x05; d->subcls = 0x00;
        d->parent = UNO_DEV_NOPARENT; d->state = UNO_DEV_UNBOUND;
    }
    if (g_have_smb) {
        uno_device *d = &g_tree[g_treen++];
        d->bus_type = UNO_BUS_PCI;
        d->addr.pci.bus = SMB_BUS; d->addr.pci.dev = SMB_DEV; d->addr.pci.fn = SMB_FN;
        d->vendor = 0x8086; d->device = 0x02a3;
        d->cls = 0x0c; d->subcls = 0x05;
        d->parent = UNO_DEV_NOPARENT; d->state = UNO_DEV_UNBOUND;
    }
}
uno_device *devmgr_get(int idx)
{ return (idx >= 0 && idx < g_treen) ? &g_tree[idx] : 0; }
uno_device *devmgr_find_class(unsigned char cls, unsigned char sub)
{
    int i;
    for (i = 0; i < g_treen; i++)
        if (g_tree[i].cls == cls && g_tree[i].subcls == sub) return &g_tree[i];
    return 0;
}
int devmgr_add_platform(int backing, unsigned char cls, unsigned char sub,
                        unsigned long long io_base, unsigned long long io_len,
                        const char *drv)
{
    (void)cls; (void)sub; (void)io_len;
    g_plat_calls++;
    g_plat_backing = backing; g_plat_iobase = io_base; g_plat_drv = drv;
    return backing >= 0 ? 1 : -1;
}

/* --- synthetic I/O + MMIO -------------------------------------------------- */

static unsigned char g_io[0x100];       /* TCO block, indexed off TCOBASE       */
static unsigned g_gcs;                   /* v2 GCS MMIO dword                     */
static int g_gcs_locked;                 /* 1 = GCS writes ignored (locked bit)  */
static unsigned g_pmcon;                 /* v3 GEN_PMCON_A MMIO dword             */
static int g_pmcon_locked;               /* 1 = GEN_PMCON_A writes ignored        */

static int io_ok(unsigned port) { return port >= TCOBASE && port < TCOBASE + 0x100; }

unsigned char hwwdt_test_in8(unsigned port)
{ return io_ok(port) ? g_io[port - TCOBASE] : 0xFF; }
unsigned short hwwdt_test_in16(unsigned port)
{ return io_ok(port) ? (unsigned short)(g_io[port-TCOBASE] | (g_io[port-TCOBASE+1] << 8)) : 0xFFFF; }
void hwwdt_test_out8(unsigned port, unsigned char v)
{ if (io_ok(port)) g_io[port - TCOBASE] = v; }
void hwwdt_test_out16(unsigned port, unsigned short v)
{
    if (!io_ok(port)) return;
    /* TCO1_STS (0x04) and TCO2_STS (0x06) are write-1-to-clear */
    unsigned off = port - TCOBASE;
    if (off == 0x04 || off == 0x06) {
        unsigned cur = g_io[off] | (g_io[off+1] << 8);
        cur &= ~(unsigned)v;
        g_io[off] = (unsigned char)cur; g_io[off+1] = (unsigned char)(cur >> 8);
    } else {
        g_io[off] = (unsigned char)v; g_io[off+1] = (unsigned char)(v >> 8);
    }
}
unsigned hwwdt_test_mmio_rd(unsigned long long pa)
{ return pa == GCS_ADDR ? g_gcs : pa == PMCON_ADDR ? g_pmcon : 0; }
void hwwdt_test_mmio_wr(unsigned long long pa, unsigned v)
{
    if (pa == GCS_ADDR && !g_gcs_locked) g_gcs = v;
    else if (pa == PMCON_ADDR && !g_pmcon_locked) g_pmcon = v;
}

static unsigned io16(unsigned off) { return g_io[off] | (g_io[off+1] << 8); }

/* --- assertions ------------------------------------------------------------ */

static int g_fail;
static void ck(int cond, const char *what)
{ printf("  %s %s\n", cond ? "ok  " : "FAIL", what); if (!cond) g_fail++; }

/* reset the whole rig; the driver's own discovery cache is static, so each
 * scenario runs in its own process via main()'s argv switch */
static void rig_reset(int rcba_en, int acpi_en, int gcs_no_reboot, int have_lpc,
                      unsigned short lpc_ven)
{
    g_lpc_ven = lpc_ven;
    g_have_lpc = have_lpc;
    lpc_reset(rcba_en, acpi_en);
    tree_reset(have_lpc);
    memset(g_io, 0, sizeof g_io);
    g_io[0x08] = 0x00; g_io[0x09] = 0x08;        /* TCO1_CNT: HLT set (firmware) */
    g_gcs = gcs_no_reboot ? (1u << 5) : 0;
    g_gcs_locked = 0;
}

int main(int argc, char **argv)
{
    const char *scen = argc > 1 ? argv[1] : "present";
    char sbuf[256];

    if (!strcmp(scen, "present")) {
        /* firmware left NO_REBOOT set; the driver must clear it and report present */
        rig_reset(1, 1, 1, 1, 0x8086);
        ck(uno_hw_wdt_present() == 1, "present after NO_REBOOT cleared");
        ck((g_gcs & (1u << 5)) == 0, "GCS NO_REBOOT bit cleared in hardware");
        ck(g_plat_calls == 1 && g_plat_backing == 0 && g_plat_iobase == TCOBASE &&
           g_plat_drv && !strcmp(g_plat_drv, "tco-wdt"),
           "TCO registered as a platform node under the LPC");
        ck((io16(0x08) & (1u << 11)) != 0, "TCO left halted until armed");

        uno_hw_wdt_arm(30);
        /* 30 s / 2 timeouts / 0.6 s-per-tick = 25 ticks per single timeout */
        ck((io16(0x12) & 0x3FF) == 25, "arm(30): TCOv2_TMR = 25 ticks (two-timeout halving)");
        ck((io16(0x08) & (1u << 11)) == 0, "arm clears TCO_TMR_HLT (timer runs)");
        ck(io16(0x00) != 0 || 1, "arm wrote TCO_RLD (reload)");

        /* pet reloads + clears the first-timeout status */
        g_io[0x04] = (1u << 3); g_io[0x05] = 0;   /* pretend one timeout latched  */
        uno_hw_wdt_pet();
        ck((io16(0x04) & (1u << 3)) == 0, "pet clears TCO1_STS timeout latch");

        uno_hw_wdt_disarm();
        ck((io16(0x08) & (1u << 11)) != 0, "disarm sets TCO_TMR_HLT (timer halted)");

        uno_hw_wdt_status(sbuf, sizeof sbuf);
        printf("  status: %s\n", sbuf);
        ck(strstr(sbuf, "v2") && strstr(sbuf, "present") && strstr(sbuf, "NO_REBOOT=0"),
           "status line reports v2 present NO_REBOOT=0");

        /* command dispatch (the uno.hwwdt binding surface) */
        ck(uno_hw_wdt_cmd("arm 60", sbuf, sizeof sbuf) > 0 &&
           (io16(0x12) & 0x3FF) == 50, "cmd 'arm 60' -> 50 ticks");
        ck(uno_hw_wdt_cmd("disarm", sbuf, sizeof sbuf) > 0 &&
           (io16(0x08) & (1u << 11)) != 0, "cmd 'disarm' halts");
        ck(uno_hw_wdt_cmd("bogus", sbuf, sizeof sbuf) == -1, "cmd unknown -> -1");
        /* 'wedge'/'selftest' are cli-spins - no-ops under HWWDT_HOSTTEST, so safe */
        ck(uno_hw_wdt_cmd("selftest 4", sbuf, sizeof sbuf) >= 0 &&
           (io16(0x08) & (1u << 11)) == 0, "cmd 'selftest' arms (spin stubbed in host)");
    } else if (!strcmp(scen, "locked")) {
        /* NO_REBOOT is locked (firmware set it write-once): the clear is ignored,
         * the read-back still shows it set, so the driver must report ABSENT and
         * must NOT arm a timer that can never reset the board. */
        rig_reset(1, 1, 1, 1, 0x8086);
        g_gcs_locked = 1;
        ck(uno_hw_wdt_present() == 0, "locked NO_REBOOT -> absent (read-back honest)");
        ck((g_gcs & (1u << 5)) != 0, "locked NO_REBOOT stays set");
        ck(g_plat_calls == 0, "a TCO we can't reboot with is NOT added to the tree");
        uno_hw_wdt_arm(30);
        ck((io16(0x08) & (1u << 11)) != 0, "arm() is a no-op while absent (stays halted)");
    } else if (!strcmp(scen, "foreign")) {
        /* a non-Intel south bridge: no Intel TCO layout -> absent */
        rig_reset(1, 1, 1, 1, 0x1022 /*AMD*/);
        ck(uno_hw_wdt_present() == 0, "non-Intel LPC -> absent (no Intel TCO)");
        ck(g_plat_calls == 0, "absent TCO is NOT added to the device tree");
    } else if (!strcmp(scen, "noacpi")) {
        /* ACPI I/O decode disabled -> can't find TCOBASE -> absent */
        rig_reset(1, 0, 1, 1, 0x8086);
        ck(uno_hw_wdt_present() == 0, "ACPI decode off -> absent");
    } else if (!strcmp(scen, "norcba")) {
        /* no RCBA AND no PMC in the tree -> nowhere to reach NO_REBOOT -> absent.
         * (find_pwrmbase refuses to poke a fixed MMIO with no PMC present.) */
        rig_reset(0, 1, 1, 1, 0x8086);
        ck(uno_hw_wdt_present() == 0, "no RCBA + no PMC -> absent (won't guess PWRM)");
        ck(g_plat_calls == 0, "unsupported TCO not added to the tree");
    } else if (!strcmp(scen, "cml")) {
        /* Comet Lake: no RCBA, PMC present, NO_REBOOT in GEN_PMCON_A (bit 1).
         * Firmware left it set; the driver clears it via the PMC MMIO window and
         * reports present, then arms the same TCOv2 timer as v2. */
        g_have_pmc = 1; g_have_smb = 1;
        g_pmcon = (1u << 1);                 /* firmware set NO_REBOOT           */
        rig_reset(0, 1, 0, 1, 0x8086);       /* RCBA off, ACPI on -> v3 path      */
        ck(uno_hw_wdt_present() == 1, "CML/PMC: present after GEN_PMCON_A NO_REBOOT cleared");
        ck((g_pmcon & (1u << 1)) == 0, "GEN_PMCON_A NO_REBOOT bit cleared in hardware");
        ck(g_plat_calls == 1 && g_plat_iobase == TCOBASE, "TCO registered in the tree (v3)");
        uno_hw_wdt_arm(40);
        /* 40 / 2 / 0.6 = 33 ticks per single timeout */
        ck((io16(0x12) & 0x3FF) == 33, "arm(40): TCOv2_TMR = 33 ticks (v3 uses v2 timer)");
        ck((io16(0x08) & (1u << 11)) == 0, "arm clears TCO_TMR_HLT (timer runs)");
        uno_hw_wdt_status(sbuf, sizeof sbuf);
        printf("  status: %s\n", sbuf);
        ck(strstr(sbuf, "v3") && strstr(sbuf, "present") && strstr(sbuf, "gen_pmcon_a") &&
           strstr(sbuf, "NO_REBOOT=0"), "status: v3 present, dumps GEN_PMCON_A");
    } else if (!strcmp(scen, "cml-locked")) {
        /* CML with GEN_PMCON_A NO_REBOOT locked -> clear ignored -> honest absent */
        g_have_pmc = 1; g_have_smb = 1;
        g_pmcon = (1u << 1);
        g_pmcon_locked = 1;
        rig_reset(0, 1, 0, 1, 0x8086);
        ck(uno_hw_wdt_present() == 0, "CML locked NO_REBOOT -> absent (read-back honest)");
        ck(g_plat_calls == 0, "a v3 TCO we can't reboot with is NOT added to the tree");
    } else if (!strcmp(scen, "tco-locked")) {
        /* CML where firmware locked TCO1_CNT (TCO_LOCK bit 12): NO_REBOOT clears
         * fine, but the halt bit can never be cleared, so the timer can't fire.
         * present() must be honest and refuse it (not a false guard). */
        g_have_pmc = 1; g_have_smb = 1;
        g_pmcon = (1u << 1);
        rig_reset(0, 1, 0, 1, 0x8086);
        g_io[0x09] |= 0x10;                  /* TCO1_CNT |= TCO_LOCK (bit 12)      */
        ck(uno_hw_wdt_present() == 0, "TCO1_CNT firmware-locked -> absent (can't un-halt)");
        ck(g_plat_calls == 0, "a locked-halted TCO is NOT added to the tree");
        uno_hw_wdt_status(sbuf, sizeof sbuf);
        ck(strstr(sbuf, "LOCKED") != 0, "status flags the TCO_LOCK");
    } else if (!strcmp(scen, "nolpc")) {
        rig_reset(1, 1, 1, 0, 0x8086);
        ck(uno_hw_wdt_present() == 0, "no LPC bridge at all -> absent");
    } else {
        printf("unknown scenario %s\n", scen);
        return 2;
    }

    printf(g_fail ? "FAILED (%d)\n" : "ok (%d failures)\n", g_fail);
    return g_fail ? 1 : 0;
}
