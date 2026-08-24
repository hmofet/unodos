/* ===========================================================================
 * UnoDOS/pc64 - Intel WiFi (iwlwifi-class) driver.  See iwlwifi.h.
 *
 * A from-scratch driver for Intel's PCIe WiFi families, modelled on the Linux
 * iwlwifi driver's transport + MVM op-mode. It brings the card up the same way
 * Linux does: identify the silicon (CSR_HW_REV / CSR_HW_RF_ID), load the Intel
 * firmware image the user placed on the ESP, boot it to ALIVE, run the
 * post-alive init handshake, scan, join a WPA2-PSK BSS (4-way handshake done in
 * wifi_wpa.c, keys installed into the card for hardware CCMP), and translate
 * Ethernet frames to/from 802.11 - publishing the family `uno_nic_t`.
 *
 * DMA is identity-mapped: UEFI boot services leave virt==phys, so a static .bss
 * buffer's address IS its physical address - exactly what the card's ring-base
 * and context-info registers want (the same trick e1000/xhci use).  Polled
 * throughout (no MSI): "did the FW fill RX / is it alive?" is answered by
 * reading the DRAM closed-RB status word and matching RX notifications, never by
 * an interrupt cause - which is what the Linux rx handler ultimately relies on.
 *
 * Coverage: gen1 (7000/8000/9000, legacy section DMA), gen2 (22000 = AX200 /
 * AX201, context-info self-load) and gen3 (AX210, context-info-v2 + IML +
 * PNVM). The primary metal target is the ThinkPad X1 Carbon Gen 8 (AX201, a
 * gen2 Qu/QuZ part).
 *
 * HARDWARE-PENDING: QEMU has no Intel-WiFi model, so this cannot be exercised in
 * the CI harness the way e1000 is - it is verified only to be INERT when no
 * supported card is on the PCI bus (so the e1000 regression still passes). Real
 * silicon bring-up, and the firmware-version command-struct variance that comes
 * with it, is the metal tail. Everything here uses the exact register values,
 * struct layouts and sequencing from the Linux source; see NETWORK.md.
 * ======================================================================== */
#include "iwlwifi.h"
#include "pc64_pci.h"
#include "pc64_fs.h"
#include "net.h"
#include "wifi_wpa.h"
#include "wifi_sae.h"
#include "uno_debug.h"     /* uno_dbg_net_trace: bring-up trace (no-op in release) */
#include "uefi.h"          /* below-4GB DMA arena (AllocateMaxAddress) */
#include <stdint.h>
#include <string.h>

/* boot-services page allocator (uefi.h leaves AllocatePages a void* slot). The
 * gen2/gen3 boot ROM DMAs the context-info + fw sections from the physaddrs we
 * program into CSR_CTXT_INFO_BA; our DMA memory is static .bss, so on a machine
 * with >4GB RAM it lands above 4GB (metal: ctxt_info + fw_dram both 0x1_42xx)
 * and the early ROM's DMA never reaches it -> FH_INT stays 0, fw never starts.
 * AllocateMaxAddress(<4GB) forces the arena into 32-bit-DMA-reachable memory. */
void *uno_pc64_st(void);                 /* uefi_main.c - the EFI system table */
int   uno_pc64_detached(void);           /* 1 once ExitBootServices has run   */
typedef EFI_STATUS (*EFI_ALLOC_PAGES)(UINTN Type, UINTN MemType, UINTN Pages,
                                      unsigned long long *Memory);

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

void uno_pc64_delay_ms(int ms);

/* progress reporting (section 10a); declared here because the RX wait loops
 * that tick it are defined well above it */
static void prog(const char *what, int step);
static void prog_tick(void);

/* =====================================================================
 * 0. small utilities + a diagnostics string
 * ===================================================================== */
static char g_status[192];
static void st_set(const char *s) { int i=0; while (s[i] && i<(int)sizeof g_status-1){ g_status[i]=s[i]; i++; } g_status[i]=0; }
static void st_cat(const char *s) { int i=0; while (g_status[i]) i++; while (*s && i<(int)sizeof g_status-1) g_status[i++]=*s++; g_status[i]=0; }
static void st_cathex(u32 v) { char t[11]="0x00000000"; const char*h="0123456789ABCDEF"; int i; for(i=0;i<8;i++) t[2+i]=h[(v>>((7-i)*4))&0xF]; st_cat(t); }

static void udelay_(int us) { /* coarse: the firmware Stall is ms-grained */ if (us < 1000) us = 1000; uno_pc64_delay_ms(us/1000); }
static void mdelay_(int ms) { uno_pc64_delay_ms(ms); }

/* =====================================================================
 * 1. MMIO / PRPH / SRAM access (BAR0)
 * ===================================================================== */

/* ---- I/O trace (UNO_DEBUG builds only) -----------------------------------
 * Records every BAR32/BAR8/PRPH access in order, so a whole bring-up can be
 * diffed MECHANICALLY against a ground-truth Linux ftrace (iwlwifi_dev_ioread32
 * / iowrite32 / iowrite_prph32 events) of a WORKING load on this exact card
 * (~/iwl_from_yoga.txt on the dev box; pc64/tools/iwl_iodiff.py aligns them).
 *
 * Sixteen F12 rounds were spent auditing the load path one register at a time
 * from recalled Linux source, and each round verified a different subsystem
 * without finding the divergence.  A recorded trace turns that guessing into a
 * diff.  Consecutive identical accesses are run-length folded so poll loops do
 * not flood the ring (the ftrace side is folded the same way).
 * Off in production: UNO_DEBUG=0 compiles every hook to nothing. */
#if UNO_DEBUG
#define IOT_N 3072
enum { IOT_R32 = 0, IOT_W32, IOT_W8, IOT_PRR, IOT_PRW };
static struct iot_ent { u32 o, v; u16 rep; u8 k; } g_iot[IOT_N];
static int g_iot_n;
static int g_iot_on;      /* armed at bring-up start                       */
static int g_iot_inner;   /* suppress the HBUS w32/r32 behind a PRPH op    */
static void iot(u8 k, u32 o, u32 v)
{
    struct iot_ent *e;
    if (!g_iot_on || g_iot_inner) return;
    if (g_iot_n) {
        e = &g_iot[g_iot_n - 1];
        if (e->k == k && e->o == o && e->v == v && e->rep < 0xFFFF) { e->rep++; return; }
    }
    if (g_iot_n >= IOT_N) return;
    e = &g_iot[g_iot_n++];
    e->k = k; e->o = o; e->v = v; e->rep = 0;
}
#define IOT_ENTER  do { g_iot_inner++; } while (0)
#define IOT_LEAVE  do { g_iot_inner--; } while (0)
/* One line per folded access, in the order they happened.  Deliberately dumped
 * on demand (the 'iwl iotrace' verb) rather than automatically: a failed load
 * is ~250 lines, and a rerun should not have to pay for it. */
static void iot_dump(void)
{
    static const char *kn[5] = { "r32", "w32", "w8_", "prr", "prw" };
    int i;
    uno_dbg_net_trace("wifi: IOTRACE begin (%d folded entries%s)",
                      g_iot_n, g_iot_n >= IOT_N ? ", RING FULL - truncated" : "");
    for (i = 0; i < g_iot_n; i++)
        uno_dbg_net_trace("wifi: IOT %s %06x %08x x%d", kn[g_iot[i].k],
                          g_iot[i].o, g_iot[i].v, (int)g_iot[i].rep + 1);
    uno_dbg_net_trace("wifi: IOTRACE end");
}
#else
#define iot(k, o, v)   ((void)0)
#define IOT_ENTER      ((void)0)
#define IOT_LEAVE      ((void)0)
#endif

static volatile u8 *g_bar;
static pci_dev g_pci;
static u16 g_devid;                     /* PCI device id (for diagnostics)     */
static int g_present, g_bound;

static u32 r32(u32 o) { u32 v = *(volatile u32 *)(g_bar + o); iot(IOT_R32, o, v); return v; }
static void w32(u32 o, u32 v) { *(volatile u32 *)(g_bar + o) = v; iot(IOT_W32, o, v); }
static void w8_(u32 o, u8 v) { *(volatile u8 *)(g_bar + o) = v; iot(IOT_W8, o, v); }
static void w64_(u32 o, u64 v) { w32(o, (u32)v); w32(o+4, (u32)(v>>32)); }
static void set_bit_(u32 o, u32 m) { w32(o, r32(o) | m); }
static void clr_bit_(u32 o, u32 m) { w32(o, r32(o) & ~m); }

/* poll until (r32(reg) & mask) == want, within timeout_ms; 0 ok, -1 timeout */
static int poll_bit(u32 reg, u32 want, u32 mask, int timeout_ms)
{
    int t;
    for (t = 0; t <= timeout_ms; t++) {
        if ((r32(reg) & mask) == want) return 0;
        mdelay_(1);
    }
    return -1;
}

/* ---- CSR ---- */
#define CSR_HW_IF_CONFIG_REG 0x000
#define CSR_INT_COALESCING   0x004
#define CSR_INT              0x008
#define CSR_INT_MASK         0x00c
#define CSR_FH_INT_STATUS    0x010
#define CSR_RESET            0x020
#define CSR_GP_CNTRL         0x024
#define CSR_HW_REV           0x028
#define CSR_UCODE_DRV_GP1_CLR 0x05c
#define CSR_MBOX_SET_REG     0x088
#define CSR_HW_RF_ID         0x09c
#define CSR_MAC_SHADOW_REG_CTRL 0x0a8
#define CSR_GIO_CHICKEN_BITS 0x100
#define CSR_DBG_LINK_PWR_MGMT_REG 0x250
#define CSR_DBG_HPET_MEM_REG 0x240
#define CSR_CTXT_INFO_BA     0x040
#define CSR_CTXT_INFO_ADDR   0x118
#define CSR_IML_DATA_ADDR    0x120
#define CSR_IML_SIZE_ADDR    0x128
#define CSR_CTXT_INFO_BOOT_CTRL 0x000  /* note: a BOOT_CTRL bit in a low CSR */
#define CSR_MSIX_HW_INT_CAUSES_AD 0x2808

#define GP_CNTRL_MAC_CLOCK_READY 0x00000001
#define GP_CNTRL_INIT_DONE       0x00000004
#define GP_CNTRL_MAC_ACCESS_REQ  0x00000008
#define GP_CNTRL_HW_RF_KILL_SW   0x08000000
#define CSR_RESET_SW_RESET       0x00000080
#define CSR_RESET_STOP_MASTER    0x00000200
#define CSR_RESET_MASTER_DISABLED 0x00000100
#define HW_IF_PCI_OWN_SET        0x00400000
#define HW_IF_PREPARE            0x08000000  /* WAKE_ME */
#define HW_IF_HAP_WAKE           0x00080000
#define HW_IF_PERSIST_BIT        0x40000000
#define GIO_CHICKEN_L1A_NO_L0S_RX 0x00800000
#define DBG_HPET_MEM_VAL         0xFFFF0000u
#define RESET_LINK_PWR_MGMT_DIS  0x80000000u
#define MBOX_OS_ALIVE            (1u<<5)
#define CSR_AUTO_FUNC_BOOT_ENA   (1u<<1)
#define MSIX_HW_ALIVE            (1u<<0)
#define MSIX_HW_IML              (1u<<1)
#define CSR_INT_BIT_ALIVE        (1u<<0)
#define CSR_INT_BIT_FH_TX        (1u<<27)
#define CSR_INT_BIT_SW_ERR       (1u<<25)
#define CSR_INT_BIT_HW_ERR       (1u<<29)
#define CSR_INT_BIT_FH_RX        (1u<<31)     /* Rx DMA / cmd responses         */
/* The gen2 ROM self-load handshake needs the FW-load interrupt UNMASKED before
 * the CSR_CTXT_INFO_BA kick - Linux does this in iwl_enable_fw_load_int_ctx_info,
 * and its absence is the leading suspect for "ROM never starts"
 * (UCODE_LOAD_STATUS=0) on the AX201 fleet. */
#define CSR_INT_FWLOAD_MASK      (CSR_INT_BIT_ALIVE | CSR_INT_BIT_FH_RX)
#define CSR_FH_INT_TX_MASK       0x00000003
#define CSR_FH_INT_RX_MASK       0x00030002

/* ---- HBUS windows ---- */
#define HBUS_TARG_MEM_RADDR  0x40c
#define HBUS_TARG_MEM_WADDR  0x410
#define HBUS_TARG_MEM_WDAT   0x418
#define HBUS_TARG_MEM_RDAT   0x41c
#define HBUS_TARG_PRPH_WADDR 0x444
#define HBUS_TARG_PRPH_RADDR 0x448
#define HBUS_TARG_PRPH_WDAT  0x44c
#define HBUS_TARG_PRPH_RDAT  0x450
#define HBUS_TARG_WRPTR      0x460

static u32 g_prph_mask = 0x000FFFFF;   /* 0x00FFFFFF on AX210+ */

/* Every Linux PRPH access (iwl_write_prph/iwl_read_prph) holds MAC access
 * (GP_CNTRL MAC_ACCESS_REQ grabbed, clock-ready polled) around the HBUS
 * window pair; without it the MAC can be asleep and the write silently does
 * not land - the F12 metal runs read UREG_CPU_INIT_RUN back as 0 after we
 * had written 1, which is exactly this. grab_nic/release_nic are refcounted
 * so callers that already hold access (rx_hw_init, load_section_gen1) nest. */
static int grab_nic(void);
static void release_nic(void);

/* _ng = no-grab: for the few pre-APM accesses Linux does with the
 * iwl_*_umac_prph_no_grab variants (persistence bit), where MAC access
 * cannot be grabbed yet. Everything else goes through the grabbing pair. */
static void prph_w_ng(u32 reg, u32 v)
{
    IOT_ENTER;
    w32(HBUS_TARG_PRPH_WADDR, (reg & g_prph_mask) | (3u<<24));
    w32(HBUS_TARG_PRPH_WDAT, v);
    IOT_LEAVE;
    iot(IOT_PRW, reg, v);
}
static u32 prph_r_ng(u32 reg)
{
    u32 v;
    IOT_ENTER;
    w32(HBUS_TARG_PRPH_RADDR, (reg & g_prph_mask) | (3u<<24));
    v = r32(HBUS_TARG_PRPH_RDAT);
    IOT_LEAVE;
    iot(IOT_PRR, reg, v);
    return v;
}
static void prph_w(u32 reg, u32 v)
{
    int g = grab_nic();
    prph_w_ng(reg, v);
    if (g == 0) release_nic();
}
/* referenced by trace/autopsy call sites that compile away in production */
__attribute__((unused))
static u32  prph_r(u32 reg)
{
    u32 v; int g = grab_nic();
    v = prph_r_ng(reg);
    if (g == 0) release_nic();
    return v;
}
static void prph_setbits(u32 reg, u32 m)
{
    int g = grab_nic();
    prph_w_ng(reg, prph_r_ng(reg) | m);
    if (g == 0) release_nic();
}
static void prph_clrbits(u32 reg, u32 m)
{
    int g = grab_nic();
    prph_w_ng(reg, prph_r_ng(reg) & ~m);
    if (g == 0) release_nic();
}

/* =====================================================================
 * 2. device identity: family, generation, firmware file name
 * ===================================================================== */
/* Order matters: the generation checks below are `g_family >= FAM_*`, so the
 * newer gen3 families (BZ/SC) MUST sort after FAM_AX210 to inherit the gen3
 * context-info-v2 + PNVM load path. */
enum { FAM_7000, FAM_8000, FAM_9000, FAM_22000, FAM_AX210, FAM_BZ, FAM_SC };
static int  g_family;

/* ---- ROM-start doorbell + boot-LTR registers (Linux iwl-prph.h / iwl-csr.h,
 * v6.6, verified 2026-07-21). On AX210+ the UREG_* UMAC registers sit behind
 * the +0x300000 UMAC PRPH offset (trans cfg .umac_prph_offset) - the plain
 * address is a DIFFERENT register there. ---- */
#define UREG_UCODE_LOAD_STATUS   0xa05c40
#define UREG_CPU_INIT_RUN        0xa05c44
#define UREG_DOORBELL_TO_ISR6    0xa05c04
#define UMAC_PRPH_OFFSET         0x300000
#define HPM_MAC_LTR_CSR          0xa0348c
#define HPM_MAC_LRT_ENABLE_ALL   0xf
#define HPM_UMAC_LTR             0xa03480
#define CSR_LTR_LONG_VAL_AD      0x0D4
#define CSR_LTR_LAST_MSG         0x0DC
#define GP_CNTRL_ROM_START       0x00000080   /* BZ+ */
#define CSR_FUNC_SCRATCH         0x02C
#define CSR_FUNC_SCRATCH_INIT    0x01010101
/* 250 us in both snoop/no-snoop fields, scale=usec: the boot-time LTR value
 * Linux programs "to workaround hardware latency issues during boot". */
#define LTR_LONG_VAL_250US       0x88FA88FA
/* CNVi power-state plumbing (Linux _iwl_trans_pcie_start_hw parity - round 2
 * of the F12 fix; the Yoga proved the doorbell alone doesn't land, pointing
 * at a power-gated MAC): the persistence bit survives a warm boot and must be
 * cleared BEFORE the sw reset (9000/22000 only), and integrated 22000 parts
 * (every CNVi AX201) need the force-power-gating dance after it. */
#define HPM_DEBUG                0xa03440
#define PERSISTENCE_BIT          (1u<<12)
#define PREG_PRPH_WPROT_9000     0xa04ce0
#define PREG_PRPH_WPROT_22000    0xa04d00
#define PREG_WFPM_ACCESS         (1u<<12)
#define HPM_HIPM_GEN_CFG         0xa03458
#define HIPM_CR_PG_EN            (1u<<0)
#define HIPM_CR_SLP_EN           (1u<<1)
#define HIPM_CR_FORCE_ACTIVE     (1u<<10)
/* Interrupt-mode chicken register: BOTH working drivers (Linux
 * iwl_pcie_conf_msix_hw, OpenBSD iwx_conf_msix_hw) program this on every
 * mq-rx part before the load - MSI_ENABLE when not using MSI-X. Same UREG
 * block as the CPU_INIT_RUN doorbell, and unlike the doorbell it is a
 * readable config register, so its readback answers "do UREG-block writes
 * land at all" (round-3 open question). */
#define UREG_CHICK               0xa05c00
#define UREG_CHICK_MSI           (1u<<24)
#define UREG_CHICK_MSIX          (1u<<25)      /* MSI-X mode - the gen2 ROM requires this */

/* MSI-X hardware config block (BAR0). The gen2/22000 AX201 boot ROM will not
 * start the firmware load unless MSI-X is enabled AND its interrupt causes are
 * mapped + unmasked FIRST (confirmed 2026-07-22 by tracing a working Linux
 * load on the same Yoga - see conf_msix). Offsets are the CSR_MSIX_* registers
 * (CSR_MSIX_BASE = 0x2000). We poll for ALIVE via the RB, so we don't service
 * these vectors - we only put the device in the state the ROM validates. */
#define CSR_MSIX_FH_INT_CAUSES_AD 0x2800
#define CSR_MSIX_FH_INT_MASK_AD   0x2804
#define CSR_MSIX_HW_INT_MASK_AD   0x280C
#define CSR_MSIX_AUTOMASK_ST_AD   0x2810
#define CSR_MSIX_RX_IVAR_BASE     0x2880       /* RX IVAR[i] = base + i     */
#define CSR_MSIX_IVAR_BASE        0x2890       /* cause IVARs (0x89 each)   */
#define MSIX_IVAR_ENA            0x89          /* enable | vector (trace)   */
/* fw-load interrupt state (iwl_enable_fw_load_int_ctx_info, MSI-X form):
 * unmask only ALIVE in the HW mask, and the FH causes in the FH mask. */
#define MSIX_FH_MASK_FWLOAD      0x0000fe00u
#define MSIX_HW_MASK_FWLOAD      0xfffffffeu   /* bit0 (ALIVE) unmasked     */

static u32 uprph(u32 reg) { return g_family >= FAM_AX210 ? reg + UMAC_PRPH_OFFSET : reg; }
static int  g_is_dvm;        /* a recognised but iwldvm-only card (unsupported) */
static int  g_gen2;          /* 22000+ : TFH TFDs, context-info fw load */
static int  g_mq_rx;         /* 9000+  : RFH multi-queue rx */
static u32  g_hw_rev, g_hw_rf_id;
/* Firmware path buffers.  SIZE THIS FROM THE ACTUAL STRINGS, not by eye:
 * "FIRMWARE\IWLAX201.UCO" is 21 characters, so it needs 22 bytes, and these
 * were declared [20] - every strcpy of a full path overflowed by exactly two
 * bytes, 'O' and the NUL.  That is where the `rf_id=0x....004f` in the SURFGO
 * bring-up trace came from: 0x004f is 'O' + NUL landing in the global declared
 * next to g_fwfile.  It was mis-read as a cosmetic trace bug.
 *
 * The `fw=` override is worse than a fixed string: it builds "FIRMWARE\" plus
 * up to 19 characters read out of a text file on the stick, so a long name in
 * WIFI.CFG could overflow by a lot.  32 covers 9 + 19 + NUL with room. */
#define FW_NAME_MAX 32
/* Compile-time proof that the longest path we build actually fits, so this
 * cannot silently regress the next time a firmware name gets longer. */
typedef char fw_name_max_is_big_enough[
    (sizeof "FIRMWARE\\IWLAX201.UCO" <= FW_NAME_MAX) ? 1 : -1];
static char g_fwfile[FW_NAME_MAX];    /* 8.3 name under FIRMWARE\ on the ESP - the one loaded now */
/* Every ucode this card could plausibly want, best guess first.  See the note
 * on choose_firmware(): the decode picks the ORDER, ALIVE picks the answer. */
#define FW_CAND_MAX 4
static char g_fwcand[FW_CAND_MAX][FW_NAME_MAX];
static int  g_fwcand_n;
static char g_pnvmfile[FW_NAME_MAX];

/* append a candidate, ignoring duplicates, overflow and anything too long to
 * hold.  Silently truncating a firmware path would just fail the open later
 * with a confusing name, so an over-long one is refused outright. */
static void fwc_add(const char *name)
{
    int i;
    if (!name || strlen(name) + 1 > FW_NAME_MAX) return;
    for (i = 0; i < g_fwcand_n; i++) if (!strcmp(g_fwcand[i], name)) return;
    if (g_fwcand_n >= FW_CAND_MAX) return;
    strcpy(g_fwcand[g_fwcand_n++], name);
}

/* The image that last posted ALIVE on this machine, remembered for the rest of
 * the session. Empty until one does. */
static char g_fw_known_good[FW_NAME_MAX];

/* Move the known-good image to the front of the candidate list.
 *
 * Without this, EVERY bring-up restarts at candidate 1 and re-runs an image
 * this card has already refused - four and a half seconds of load, doorbell and
 * ALIVE-timeout autopsy, per attempt, per bring-up. A boot does several. It is
 * also what opened the window for the stale-ALIVE misread above: the failing
 * attempt only had a previous success to trip over because it kept being
 * repeated after that success. Answering "which one worked?" removes both. */
static void fwc_promote_known_good(void)
{
    char tmp[FW_NAME_MAX];
    int i;
    if (!g_fw_known_good[0]) return;
    for (i = 0; i < g_fwcand_n; i++)
        if (!strcmp(g_fwcand[i], g_fw_known_good)) break;
    if (i <= 0 || i >= g_fwcand_n) return;         /* absent, or already first */
    strcpy(tmp, g_fwcand[i]);
    for (; i > 0; i--) strcpy(g_fwcand[i], g_fwcand[i - 1]);
    strcpy(g_fwcand[0], tmp);
}
static u8   g_mac[6];
static u8   g_joined;
static int  g_link_lost_reason = -1;   /* last deauth/disassoc reason, -1 = none */
static char g_ssid_str[36];

/* Classify an Intel WiFi PCI device id into its iwlwifi family. In current
 * mainline the family is a pure function of the device id (the subsystem id and
 * RF-ID register only refine the marketing name / RF firmware suffix, which the
 * fixed-per-family ESP filename abstracts away). Full table from Linux
 * pcie/drv.c iwl_hw_card_ids[]. Returns 1 if supported (MVM), 0 otherwise;
 * sets g_is_dvm for the older iwldvm-only parts so the status can say so. */
static int identify_by_pci(u16 dev)
{
    switch (dev) {
    /* ---- FAM_7000 (gen1 legacy fw, single-queue RX) ---- */
    case 0x08b1: case 0x08b2:                     /* 7260  */
    case 0x08b3: case 0x08b4:                     /* 3160  */
    case 0x3165: case 0x3166:                     /* 3165  */
    case 0x24fb:                                  /* 3168  */
    case 0x095a: case 0x095b:                     /* 7265  */
        g_family = FAM_7000; return 1;
    /* ---- FAM_8000 (gen1 secure fw) ---- */
    case 0x24f3: case 0x24f4:                     /* 8260  */
    case 0x24f5: case 0x24f6:                     /* 4165  */
    case 0x24fd:                                  /* 8265/8275 */
        g_family = FAM_8000; return 1;
    /* ---- FAM_9000 (gen1 transport, multi-queue RX) ---- */
    case 0x2526: case 0x271b: case 0x271c:        /* 9260 "th" fw */
    case 0x30dc: case 0x31dc:                     /* 9560 "pu" fw */
    case 0x9df0: case 0xa370:                     /* 9560/9461/9462 "pu" fw */
        g_family = FAM_9000; return 1;
    /* ---- FAM_22000 (gen2 context-info fw load) ---- */
    case 0x2723:                                  /* AX200 discrete -> cc-a0 */
    case 0x02f0: case 0x06f0:                     /* Qu AX201 -> Qu-b0-hr-b0 */
    case 0x34f0: case 0x3df0: case 0x4df0:
    case 0x43f0: case 0xa0f0:
        g_family = FAM_22000; return 1;
    /* ---- FAM_AX210 (gen3 ctx-info-v2 + IML + PNVM); Ty/So/Ma ---- */
    case 0x2725:                                  /* AX210 (Ty) -> ty-a0-gf-a0 */
    case 0x7af0: case 0x7f70:                     /* So AX211/AX411 -> so-a0-gf-a0 */
    case 0x7a70: case 0x51f0: case 0x51f1: case 0x54f0:
    case 0x2729: case 0x7e40:                     /* Ma -> ma-a0-* */
        g_family = FAM_AX210; return 1;
    /* ---- FAM_BZ (WiFi 7 BE200/BE201; gen3-like + TOP reset) [best-effort] ---- */
    case 0x272b:                                  /* Gl / BE200 discrete -> gl-b0-fm-b0 */
    case 0xa840:                                  /* Bz (any subsystem) -> bz-a0-fm-b0 */
    case 0x7740: case 0x4d40:                     /* Bz */
        g_family = FAM_BZ; return 1;
    /* ---- FAM_SC (WiFi 7 BE211; newer) [best-effort] ---- */
    case 0xe440: case 0xe340: case 0xd340:
    case 0x6e70: case 0xd240:                     /* Sc -> sc-a0-wh-a0 / -fm-b0 */
        g_family = FAM_SC; return 1;
    /* ---- iwldvm-only parts: recognised but UNSUPPORTED by this MVM driver ---- */
    case 0x4232: case 0x4235: case 0x4236: case 0x4237: case 0x423a: case 0x423b:
    case 0x423c: case 0x423d:                     /* 5000/5300/5350/5150 */
    case 0x422b: case 0x422c: case 0x4238: case 0x4239: /* 6000 */
    case 0x0082: case 0x0085: case 0x008a: case 0x008b: case 0x0090: case 0x0091:
    case 0x0087: case 0x0089: case 0x0885: case 0x0886: /* 6005/6030/6050/6150 */
    case 0x0083: case 0x0084: case 0x08ae: case 0x08af: /* 1000/100 */
    case 0x0896: case 0x0897: case 0x0890: case 0x0891: /* 130/2000 */
    case 0x0887: case 0x0888: case 0x088e: case 0x088f: /* 2030/6035 */
    case 0x0894: case 0x0895: case 0x0892: case 0x0893: /* 105/135 */
        g_is_dvm = 1; return 0;
    }
    return 0;
}

/* The MAC types this driver cares about, from CSR_HW_REV bits 11:4 - the same
 * numbers Linux calls IWL_CFG_MAC_TYPE_*.  A Qu-family AX201 is Qu or QuZ
 * SILICON, and which one you have is NOT a function of the PCI device id:
 * 0x02f0 and 0x34f0 both appear as either, which is why Linux keys its config
 * table on the hardware revision and not on the id.
 *
 * That matters because Qu-b0, Qu-c0 and QuZ-a0 are three DIFFERENT upstream
 * ucode files.  This driver used to compute mac_type and throw it away, then
 * load one IWLAX201.UCO whichever silicon it found - and the blob that ships
 * is QuZ-a0-hr-b0 (verified byte-identical against Debian's
 * firmware-iwlwifi 20260622-1).  It works on the machines it was developed
 * on; on a Surface Laptop Go the firmware loads, starts, and never posts
 * ALIVE, which is what a stepping mismatch looks like from the outside. */
#define MAC_TYPE_QU   0x33
#define MAC_TYPE_QUZ  0x35
#define MAC_TYPE_QNJ  0x36

/* Decode CSR_HW_REV / CSR_HW_RF_ID and build the ORDERED LIST of ucode files
 * this card might want, best guess first.
 *
 * WHY A LIST AND NOT A CHOICE.  Which Qu-family stepping a machine has is in
 * CSR_HW_REV, and our decode of it is a hypothesis - it has never been checked
 * against silicon that disagrees.  The old code committed to one file and
 * treated a wrong guess as terminal: `g_fwalt` was consulted only when the
 * preferred file was NOT STAGED, never when it loaded and the ROM refused it.
 * So a laptop whose revision we decode wrongly had a dead radio, and the only
 * way through was a human editing `fw=` into a file on the stick.
 *
 * Every blob ships on every stick already (build.sh bundles all of fw-blobs/),
 * so there is no reason for the driver to need a human to tell it which one it
 * is holding.  ALIVE is a clean 2-second pass/fail oracle - the firmware either
 * posts the notification or it does not - which makes "try the next one" both
 * cheap and unambiguous.  The decode therefore picks the ORDER (right first,
 * usually first try) and the hardware picks the answer.  One image, every
 * laptop, no per-machine configuration. */
static void choose_firmware(void)
{
    u32 mac_type = (g_hw_rev >> 4) & 0xFFF;
    u32 step     = (g_hw_rev >> 2) & 0x3;      /* 0 = A, 1 = B, 2 = C */
    g_gen2  = (g_family >= FAM_22000);
    g_mq_rx = (g_family >= FAM_9000);
    if (g_family >= FAM_AX210) g_prph_mask = 0x00FFFFFF;

    g_pnvmfile[0] = 0;
    g_fwcand_n = 0;
    switch (g_family) {
    case FAM_7000:  fwc_add("FIRMWARE\\IWL7260.UCO"); break;
    case FAM_8000:  fwc_add("FIRMWARE\\IWL8000.UCO"); break;
    case FAM_9000:
        /* 9260 (device 0x2526/0x271b/0x271c) uses the th-b0-jf-b0 image;
           9461/9462/9560 use the pu-b0-jf-b0 image - different upstream files.
           The device id settles this one, so the other is only a fallback. */
        if (g_pci.device==0x2526 || g_pci.device==0x271b || g_pci.device==0x271c)
             { fwc_add("FIRMWARE\\IWL9260.UCO"); fwc_add("FIRMWARE\\IWL9000.UCO"); }
        else { fwc_add("FIRMWARE\\IWL9000.UCO"); fwc_add("FIRMWARE\\IWL9260.UCO"); }
        break;
    case FAM_22000:
        /* AX200 (discrete, 0x2723) is the cc-a0 image and is not a Qu part. */
        if (g_pci.device == 0x2723) { fwc_add("FIRMWARE\\IWLAX200.UCO"); break; }
        /* Every other 22000 part here is a Qu-family AX201 CNVi: QuZ-a0, Qu-b0
           and Qu-c0 are three separate ucodes and the PCI id does not say which
           (0x02f0 and 0x34f0 both appear as either).  Lead with the decode,
           then offer the other two - fwc_add dedups, so this is 3 entries. */
        if (mac_type == MAC_TYPE_QUZ)                  fwc_add("FIRMWARE\\IWLAX201.UCO");
        else if (mac_type == MAC_TYPE_QU && step >= 2) fwc_add("FIRMWARE\\IWLAX20C.UCO");
        else                                           fwc_add("FIRMWARE\\IWLAX20B.UCO");
        fwc_add("FIRMWARE\\IWLAX20B.UCO");             /* Qu-b0-hr-b0  */
        fwc_add("FIRMWARE\\IWLAX20C.UCO");             /* Qu-c0-hr-b0  */
        fwc_add("FIRMWARE\\IWLAX201.UCO");             /* QuZ-a0-hr-b0 */
        break;
    case FAM_AX210:
        /* Ty (0x2725) uses ty-a0-gf-a0; So/Ma parts (AX211/AX411) use
           so-a0-gf-a0 - separate files. Both need a matching PNVM, which is
           chosen with the image, so these two are not interchangeable. */
        if (g_pci.device == 0x2725) { fwc_add("FIRMWARE\\IWLAX210.UCO");
                                      strcpy(g_pnvmfile,"FIRMWARE\\IWLAX210.PNV"); }
        else                        { fwc_add("FIRMWARE\\IWLAX211.UCO");
                                      strcpy(g_pnvmfile,"FIRMWARE\\IWLAX211.PNV"); }
        break;
    case FAM_BZ:
        /* WiFi 7 Bz/Gl. Best-effort: routed through the gen3 loader, but Bz adds
           a TOP-reset + ROM-start handshake this driver does not yet perform, so
           this is metal-pending even by the family's standard. */
        if (g_pci.device == 0x272b) { fwc_add("FIRMWARE\\IWLBE200.UCO");   /* Gl discrete */
                                      strcpy(g_pnvmfile,"FIRMWARE\\IWLBE200.PNV"); }
        else                        { fwc_add("FIRMWARE\\IWLBE201.UCO");   /* Bz */
                                      strcpy(g_pnvmfile,"FIRMWARE\\IWLBE201.PNV"); }
        break;
    case FAM_SC:
        fwc_add("FIRMWARE\\IWLBE211.UCO");
        strcpy(g_pnvmfile, "FIRMWARE\\IWLBE211.PNV"); break;
    }
    fwc_promote_known_good();          /* start with what already worked here */
    if (g_fwcand_n) strcpy(g_fwfile, g_fwcand[0]);
}

/* the ESP volume that holds FIRMWARE\ and the credentials file. WIFI.CFG is
 * the documented name; WIFI.TXT is accepted too - it is what the flasher's
 * developer-options folder copy stages from the NAS creds template. */
static char g_cfgname[12];
/* true if the named file on `vol` carries a real `ssid=` (credentials), rather
 * than merely existing - a stress-only DEBUG.CFG, or a starter file whose only
 * ssid= is inside a '#' comment, must both read as "no credentials here".
 *
 * Defined in terms of the SAME parser read_config() uses (see cfg_parse below):
 * this used to be a separate 255-byte raw byte search, and the two disagreeing
 * about the same file is what stopped a correctly-provisioned stick joining. */
static int cfg_parse(int vol, const char *name,
                     char *ssid, int ssidcap, char *psk, int pskcap);
static int file_has_ssid(int vol, const char *name)
{
    return cfg_parse(vol, name, 0, 0, 0, 0) == 0;
}
/* An 8.3 firmware name forced from the config file: `fw=IWLAX20B.UCO` in
 * WIFI.CFG / WIFI.TXT / DEBUG.CFG on any mounted volume.
 *
 * WHY THIS EXISTS.  Which AX201 stepping a machine needs is in CSR_HW_REV, and
 * the decode below is a hypothesis until a real machine confirms it.  The one
 * machine that disagrees with the driver - a Surface Laptop Go whose firmware
 * loads and never posts ALIVE - has NO WIRED NIC, so URC cannot reach it and
 * every experiment otherwise costs a rebuild, a reflash and a walk to the
 * laptop.  With this, all three steppings are on the stick and trying the next
 * one is editing one line in a text file on the stick.  It is a debugging
 * affordance for exactly the situation that has no other affordance. */
static char g_fwforce[20];

static void read_fw_override(void)
{
    static const char *cand[3] = { "WIFI.CFG", "WIFI.TXT", "DEBUG.CFG" };
    static u8 b[512];
    int nv = uno_fs_volumes(), v, j;
    g_fwforce[0] = 0;
    for (v = 0; v < nv; v++)
        for (j = 0; j < 3; j++) {
            long n = uno_fs_read(v, cand[j], b, (long)sizeof b - 1), i;
            if (n <= 0) continue;
            for (i = 0; i + 3 <= n; i++) {
                /* an "fw=" at the start of a line, so a psk containing "fw="
                 * is never mistaken for one */
                if (!(b[i]=='f' && b[i+1]=='w' && b[i+2]=='=')) continue;
                if (i > 0 && b[i-1] != '\n' && b[i-1] != '\r') continue;
                { int k = 0; long q = i + 3;
                  while (q < n && k < (int)sizeof g_fwforce - 1 &&
                         b[q] != '\r' && b[q] != '\n' && b[q] != ' ')
                      g_fwforce[k++] = (char)b[q++];
                  g_fwforce[k] = 0; }
                if (g_fwforce[0]) return;
            }
        }
}

/* A BSSID the join must try FIRST: `bssid=30:29:2b:70:4f:c6` in WIFI.CFG /
 * WIFI.TXT / DEBUG.CFG on any mounted volume. Same affordance as `fw=` above,
 * for the same machine and the same reason.
 *
 * WHY THIS EXISTS. find_and_join() rescans and orders candidates by RSSI on
 * every attempt, so on a mesh the experiment changes underneath you. Between
 * two SURFGO runs a BSS appeared that had not been there before and the one
 * that had associated slid from attempt 2 to attempt 3 - which is the
 * difference between a FIRST-ATTEMPT join and a RETARGETED one, and that is
 * the variable under test. Pinning makes a run repeatable, and it carries no
 * credentials, so it can be staged into an image without a password in it. */
static u8  g_pin_bssid[6];
static int g_pin_on;

static int hexnib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void read_bssid_override(void)
{
    static const char *cand[3] = { "WIFI.CFG", "WIFI.TXT", "DEBUG.CFG" };
    static u8 b[512];
    int nv = uno_fs_volumes(), v, j;
    g_pin_on = 0;
    for (v = 0; v < nv; v++)
        for (j = 0; j < 3; j++) {
            long n = uno_fs_read(v, cand[j], b, (long)sizeof b - 1), i;
            if (n <= 0) continue;
            for (i = 0; i + 6 <= n; i++) {
                long q; int k, got = 0;
                if (memcmp(b + i, "bssid=", 6)) continue;
                /* start of a line only, so a psk containing "bssid=" is not one */
                if (i > 0 && b[i-1] != '\n' && b[i-1] != '\r') continue;
                q = i + 6;
                for (k = 0; k < 6; k++) {
                    int hi, lo;
                    if (q + 1 >= n) break;
                    hi = hexnib(b[q]); lo = hexnib(b[q+1]);
                    if (hi < 0 || lo < 0) break;
                    g_pin_bssid[k] = (u8)((hi << 4) | lo);
                    q += 2; got++;
                    if (k < 5 && q < n && (b[q] == ':' || b[q] == '-')) q++;
                }
                if (got == 6) { g_pin_on = 1; return; }
            }
        }
}

static int firmware_volume(void)
{
    /* DEBUG.CFG can carry the Wi-Fi creds too (debug builds); prefer whichever
     * cfg file actually has an ssid= line so a stress-only DEBUG.CFG or an empty
     * WIFI.CFG never shadows the real credentials. */
    static const char *cand[3] = { "DEBUG.CFG", "WIFI.CFG", "WIFI.TXT" };
    int n = uno_fs_volumes(), i, j;
    for (i = 0; i < n; i++)
        if (uno_fs_kind(i) == 2 || uno_fs_kind(i) == 1) {   /* firmware SFS / native FAT */
            for (j = 0; j < 3; j++)
                if (uno_fs_size(i, cand[j]) > 0 && file_has_ssid(i, cand[j])) {
                    strcpy(g_cfgname, cand[j]); return i;
                }
        }
    return -1;
}

/* The volume that actually holds the firmware image. This used to be assumed
 * to be the volume whose config file carries the credentials, which conflated
 * two unrelated questions - WHERE THE CREDS ARE and WHERE THE FIRMWARE IS - and
 * meant a stick with firmware but no WIFI.CFG could not bring the radio up at
 * all, so the Control Panel's Scan found nothing on a perfectly good card. */
static int fw_volume(void)
{
    int n = uno_fs_volumes(), i;
    for (i = 0; i < n; i++)
        if (uno_fs_kind(i) == 2 || uno_fs_kind(i) == 1) {   /* firmware SFS / native FAT */
            if (uno_fs_size(i, g_fwfile) > 0) return i;
            if (uno_fs_size(i, g_fwfile + 9) > 0) return i;  /* flat root: no FIRMWARE\ */
        }
    return -1;
}

/* =====================================================================
 * 3. DMA arena (identity mapped: phys == virt)
 * ===================================================================== */
/* One firmware image can be ~1-2 MB (AX). We read the .ucode into a file buffer,
   copy each section into the fw-section arena, and keep the rings/context in
   dedicated aligned blocks. All in .bss => phys == virt while boot services
   are alive. */
#define FW_FILE_MAX   (2*1024*1024)
#define FW_ARENA_MAX  (3*1024*1024)
#define PNVM_MAX      (256*1024)
static u8 g_fwbuf[FW_FILE_MAX]   __attribute__((aligned(4096)));  /* file scratch (not DMA'd) */
static u8 g_arena_static[FW_ARENA_MAX] __attribute__((aligned(4096)));  /* fallback if alloc fails */
static u8 *g_arena;                     /* DMA arena base: <4GB block, else the static */
static u8 g_pnvmbuf[PNVM_MAX]    __attribute__((aligned(4096)));  /* file scratch (not DMA'd) */
static long g_pnvm_len;                 /* bytes in g_pnvmbuf (0 = none)      */
static u32 g_arena_used;
static u64 g_arena_phys;                /* base physaddr, for the bring-up trace */

/* Back the DMA arena with pages forced below 4GB (see the note by the includes).
 * Boot-services only - WiFi brings up before ExitBootServices in the debug
 * build. Falls back to the static arena (fine on <=4GB boxes and on QEMU). */
static void arena_init_lowmem(void)
{
    g_arena_used = 0;
    if (g_arena) return;                /* once per boot */
    /* The comment above used to assert that WiFi always brings up before
     * ExitBootServices. It does not: on an X13 Yoga (2026-08-20) bring-up ran
     * at 24 s with detached=1, `ST->BootServices` was already dead, and the
     * dereference below was the null-deref UBSan trapped - a #UD at
     * arena_init_lowmem+0xb5 that reset the box, over and over. Boot services
     * are only callable while we still own them, and `uno_pc64_detached()` is
     * how every other driver here asks. */
    if (!uno_pc64_detached()) {
        EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
        if (ST && ST->BootServices) {
            unsigned long long mem = 0x00000000FFFFF000ull;   /* ceiling: <4GB */
            UINTN pages = (FW_ARENA_MAX + 4095) / 4096;
            if (((EFI_ALLOC_PAGES)ST->BootServices->AllocatePages)(
                    1 /*AllocateMaxAddress*/, 2 /*EfiLoaderData*/, pages, &mem) == EFI_SUCCESS)
                g_arena = (u8 *)(uintptr_t)mem;
        }
    }
    if (!g_arena) g_arena = g_arena_static;             /* fallback (may be >4GB) */
    g_arena_phys = (u64)(uintptr_t)g_arena;
}

int iwl_present(void);          /* defined below; the reserve is gated on it */

/* Reserve the DMA arena WHILE BOOT SERVICES ARE STILL ALIVE.
 *
 * The fallback above is `g_arena_static`, a .bss array - and .bss is wherever
 * the firmware loaded the kernel image. On the Surface Laptop Go that is
 * `image_base 0x140000000`, so the static arena lands at 0x1_43add000: five
 * gigabytes up, and unreachable by a device programmed with the 32-bit
 * physical addresses the boot ROM and the RX ring use.
 *
 * The failure is quiet and looks nothing like a memory problem. The firmware
 * still goes ALIVE, the MAC still reads out of OTP, and then every scan
 * returns `rb_total=0 mpdu_seen=0 aps=0` - not one receive buffer ever comes
 * back, because the device is DMAing to an address it cannot form. Which
 * reads, from the desk, as "the card works but will not join anything".
 *
 * A machine that detaches never gets the good allocation, because by the time
 * WiFi is brought up there are no boot services to ask. So ask EARLY: this is
 * the same pattern uno_modload_reserve() and uno_vmm_reserve() already use for
 * the same reason, and try_detach calls it beside them. Gated on the card
 * actually being present, so a machine with no Intel WiFi reserves nothing.
 *
 * Idempotent - arena_init_lowmem() returns immediately once g_arena is set. */
void uno_iwl_reserve(void)
{
    if (g_arena) return;
    if (!iwl_present()) return;
    arena_init_lowmem();
}

static void *arena_alloc(u32 len)
{
    u32 off = (g_arena_used + 4095) & ~4095u;
    if (!g_arena) arena_init_lowmem();
    if (off + len > FW_ARENA_MAX) return 0;
    g_arena_used = off + len;
    return g_arena + off;
}
static u64 phys(const void *p) { return (u64)(uintptr_t)p; }

/* =====================================================================
 * 4. .ucode TLV firmware parser
 * ===================================================================== */
#define TLV_MAGIC 0x0a4c5749u
#define CPU_SEP   0xFFFFCCCCu
#define PAGE_SEP  0xAAAABBBBu

enum {
    TLV_FLAGS=18, TLV_SEC_RT=19, TLV_SEC_INIT=20, TLV_NUM_OF_CPU=27,
    TLV_API_CHANGES=29, TLV_ENABLED_CAPA=30, TLV_N_SCAN_CH=31, TLV_PAGING=32,
    TLV_FW_VERSION=36, TLV_PHY_SKU=23, TLV_DEF_CALIB=22, TLV_SECURE_SEC_RT=24,
    TLV_SECURE_SEC_INIT=25, TLV_CMD_VERSIONS=48, TLV_IML=52, TLV_PNVM_VERSION=62
};

/* a parsed section: device load offset + a pointer into the file buffer */
#define MAX_SEC 64
typedef struct { u32 offset; const u8 *data; u32 len; } fw_sec;
typedef struct {
    fw_sec rt[MAX_SEC];  int rt_n;
    fw_sec init[MAX_SEC]; int init_n;
    const u8 *iml; u32 iml_len;
    u32 phy_sku; u32 calib_flow, calib_event;
    u32 n_scan_channels;
    u32 num_cpu;
    u32 paging_mem_size;
    u8  api[16], capa[16];      /* bitmaps (128 bits each) */
    int alive_notif_ver;        /* from CMD_VERSIONS if present, else guessed */
    int have;
} fw_image;
static fw_image g_fw;

static u32 le32(const u8 *p){ return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }

static int fw_has_capa(int bit){ return (g_fw.capa[bit>>3] >> (bit&7)) & 1; }

/* Valid antenna masks, straight out of the firmware's PHY_SKU TLV - the same
 * fw->valid_tx_ant / valid_rx_ant Linux derives in iwl_parse_tlv_firmware:
 *   FW_PHY_CFG_TX_CHAIN = bits 16-19, FW_PHY_CFG_RX_CHAIN = bits 20-23.
 * On this AX201 phy_sku = 0x00330018, so BOTH are 0x3 (a 2x2 part). Hardcoding
 * antenna A only (0x1) told the PHY that half its chains do not exist, which
 * nothing notices until something asks the LMAC to schedule real airtime. */
static u32 fw_valid_tx_ant(void){ u32 a = (g_fw.phy_sku >> 16) & 0xf; return a ? a : 1; }
static u32 fw_valid_rx_ant(void){ u32 a = (g_fw.phy_sku >> 20) & 0xf; return a ? a : 1; }


static int parse_ucode(const u8 *buf, u32 n)
{
    u32 off;
    memset(&g_fw, 0, sizeof g_fw);
    if (n < 88) return -1;
    if (le32(buf) != 0 || le32(buf+4) != TLV_MAGIC) return -1;
    off = 88;
    while (off + 8 <= n) {
        u32 type = le32(buf+off), len = le32(buf+off+4);
        const u8 *d = buf+off+8;
        if (off + 8 + len > n) break;
        switch (type) {
        case TLV_SEC_RT: case TLV_SECURE_SEC_RT:
            if (g_fw.rt_n < MAX_SEC && len >= 4) {
                g_fw.rt[g_fw.rt_n].offset = le32(d);
                g_fw.rt[g_fw.rt_n].data = d+4; g_fw.rt[g_fw.rt_n].len = len-4; g_fw.rt_n++;
            } break;
        case TLV_SEC_INIT: case TLV_SECURE_SEC_INIT:
            if (g_fw.init_n < MAX_SEC && len >= 4) {
                g_fw.init[g_fw.init_n].offset = le32(d);
                g_fw.init[g_fw.init_n].data = d+4; g_fw.init[g_fw.init_n].len = len-4; g_fw.init_n++;
            } break;
        case TLV_IML: g_fw.iml = d; g_fw.iml_len = len; break;
        case TLV_PHY_SKU: if (len>=4) g_fw.phy_sku = le32(d); break;
        case TLV_DEF_CALIB: if (len>=12) { g_fw.calib_flow=le32(d+4); g_fw.calib_event=le32(d+8);} break;
        case TLV_N_SCAN_CH: if (len>=4) g_fw.n_scan_channels = le32(d); break;
        case TLV_NUM_OF_CPU: if (len>=4) g_fw.num_cpu = le32(d); break;
        case TLV_PAGING: if (len>=4) g_fw.paging_mem_size = le32(d); break;
        case TLV_API_CHANGES: if (len>=8) { u32 idx=le32(d),fl=le32(d+4); int b; for(b=0;b<32;b++) if(fl&(1u<<b)){int bit=b+32*idx; if(bit<128) g_fw.api[bit>>3]|=1<<(bit&7);} } break;
        case TLV_ENABLED_CAPA: if (len>=8) { u32 idx=le32(d),fl=le32(d+4); int b; for(b=0;b<32;b++) if(fl&(1u<<b)){int bit=b+32*idx; if(bit<128) g_fw.capa[bit>>3]|=1<<(bit&7);} } break;
        case TLV_CMD_VERSIONS: {
            u32 i; for (i=0;i+4<=len;i+=4){ if (d[i]==0x01 && d[i+1]==0x00) g_fw.alive_notif_ver = d[i+3]; } /* ALIVE notif ver */
        } break;
        default: break;
        }
        off += 8 + ((len + 3) & ~3u);
    }
    if (!g_fw.alive_notif_ver) g_fw.alive_notif_ver = (g_family >= FAM_8000) ? 6 : 3;
    g_fw.have = (g_fw.rt_n > 0);
    return g_fw.have ? 0 : -1;
}

/* =====================================================================
 * 5. CSR / APM bring-up (reset, power, NIC-ready, grab access)
 * ===================================================================== */
static int prepare_card_hw(void)
{
    int iter, t;
    set_bit_(CSR_HW_IF_CONFIG_REG, HW_IF_PCI_OWN_SET);
    if (poll_bit(CSR_HW_IF_CONFIG_REG, HW_IF_PCI_OWN_SET, HW_IF_PCI_OWN_SET, 2) == 0) {
        set_bit_(CSR_MBOX_SET_REG, MBOX_OS_ALIVE);   /* Linux iwl_pcie_set_hw_ready */
        return 0;
    }
    set_bit_(CSR_DBG_LINK_PWR_MGMT_REG, RESET_LINK_PWR_MGMT_DIS);
    mdelay_(2);
    for (iter = 0; iter < 10; iter++) {
        set_bit_(CSR_HW_IF_CONFIG_REG, HW_IF_PREPARE);
        for (t = 0; t < 150; t++) {
            if ((r32(CSR_HW_IF_CONFIG_REG) & HW_IF_PCI_OWN_SET)) {
                set_bit_(CSR_MBOX_SET_REG, MBOX_OS_ALIVE);
                return 0;
            }
            mdelay_(1);
        }
        mdelay_(25);
    }
    return -1;
}

/* Linux sw_reset takes retake_ownership=true from start_hw: after the reset
 * the ownership handshake must be redone or later CSR/PRPH traffic can be
 * ignored (we never re-prepared - a round-1 F12 gap). */
static void sw_reset(void)
{
    set_bit_(CSR_RESET, CSR_RESET_SW_RESET);
    mdelay_(6);
    prepare_card_hw();
}

/* Persistence mode survives a warm reboot on 9000/22000 CNVi parts: if the
 * previous OS/BIOS left PERSISTENCE_BIT set, the MAC keeps its old state and
 * a fresh firmware load is ignored. Linux clears it FIRST, before the sw
 * reset, with the no-grab PRPH accessors (APM is not up yet). */
static void clear_persistence_bit(void)
{
    u32 wprot_reg, hpm, wprot;
    if (g_family == FAM_9000)       wprot_reg = PREG_PRPH_WPROT_9000;
    else if (g_family == FAM_22000) wprot_reg = PREG_PRPH_WPROT_22000;
    else return;
    hpm = prph_r_ng(HPM_DEBUG);
    if (hpm != 0xFFFFFFFFu && (hpm & PERSISTENCE_BIT)) {
        wprot = prph_r_ng(wprot_reg);
        if (wprot & PREG_WFPM_ACCESS) {
            uno_dbg_net_trace("wifi: persistence bit SET and write-protected (HPM_DEBUG=%08x WPROT=%08x) - cannot clear", hpm, wprot);
            return;
        }
        prph_w_ng(HPM_DEBUG, hpm & ~PERSISTENCE_BIT);
        uno_dbg_net_trace("wifi: cleared persistence bit (HPM_DEBUG was %08x)", hpm);
    }
}

/* Integrated 22000 (every CNVi AX201) force-power-gating dance - Linux runs
 * this in start_hw between the first sw reset and APM init. Without it a
 * power-gated CNVi MAC absorbs PRPH writes and the boot ROM never runs -
 * exactly the round-1 Yoga signature (doorbell written with MAC access held,
 * read back 0, UCODE_LOAD_STATUS=0). Ends with ANOTHER sw reset + retake. */
#define WFPM_GP1_ENA             0xa03030   /* WFPM enable - working trace writes 0x80000000 here, early */
static int force_power_gating(void)
{
    /* iwl_finish_nic_init: INIT_DONE + wait clock */
    set_bit_(CSR_GP_CNTRL, GP_CNTRL_INIT_DONE);
    if (poll_bit(CSR_GP_CNTRL, GP_CNTRL_MAC_CLOCK_READY, GP_CNTRL_MAC_CLOCK_READY, 25) < 0)
        return -1;
    /* Working-trace write we were missing: a WFPM enable, done right after
     * finish_nic_init, before the HPM power-gating dance. */
    prph_w(WFPM_GP1_ENA, 0x80000000u);
    prph_setbits(HPM_HIPM_GEN_CFG, HIPM_CR_FORCE_ACTIVE);
    udelay_(20);
    prph_setbits(HPM_HIPM_GEN_CFG, HIPM_CR_PG_EN | HIPM_CR_SLP_EN);
    udelay_(20);
    prph_clrbits(HPM_HIPM_GEN_CFG, HIPM_CR_FORCE_ACTIVE);
    sw_reset();                      /* includes the ownership retake */
    return 0;
}

/* Linux stop_device parity (round 5). Every real Linux firmware load runs
 * AFTER iwl_trans_pcie_stop_device tore the device down: interrupts off,
 * bus-master DMA stopped (STOP_MASTER + poll MASTER_DISABLED), INIT_DONE
 * cleared, then the sw reset. We had only ever reset from whatever state the
 * BIOS/CSME left the CNVi MAC in - a ROM that ignores the load kick because
 * the MAC never went through a clean stop matches every round-1..4 symptom
 * (all writes land, no error bits, LOAD_STATUS never moves). */
static int g_fw_loaded;   /* a fw image was kicked; the device may be live+DMAing */
static int g_mvm_arm;     /* opt-in: run the post-ALIVE MVM/join sequence (new frontier) */
static void device_stop(void)
{
    g_fw_loaded = 0;
    w32(CSR_INT_MASK, 0);
    w32(CSR_INT, 0xFFFFFFFFu);
    set_bit_(CSR_RESET, CSR_RESET_STOP_MASTER);
    poll_bit(CSR_RESET, CSR_RESET_MASTER_DISABLED, CSR_RESET_MASTER_DISABLED, 100);
    uno_dbg_net_trace("wifi: device_stop: RESET=%08x after master-stop", r32(CSR_RESET));
    clr_bit_(CSR_GP_CNTRL, GP_CNTRL_INIT_DONE);
    sw_reset();                      /* includes the ownership retake */
}

static int apm_init(void)
{
    set_bit_(CSR_GIO_CHICKEN_BITS, GIO_CHICKEN_L1A_NO_L0S_RX);
    if (!g_gen2) {
        /* gen1-only APM extras: Linux iwl_pcie_gen2_apm_init does NOT set
         * the HPET debug filter or HAP_WAKE - stop diverging on gen2. */
        set_bit_(CSR_DBG_HPET_MEM_REG, DBG_HPET_MEM_VAL);
        set_bit_(CSR_HW_IF_CONFIG_REG, HW_IF_HAP_WAKE);
    }
    /* activate NIC: set INIT_DONE, wait MAC_CLOCK_READY */
    set_bit_(CSR_GP_CNTRL, GP_CNTRL_INIT_DONE);
    if (g_family == FAM_8000) udelay_(2000);
    if (poll_bit(CSR_GP_CNTRL, GP_CNTRL_MAC_CLOCK_READY, GP_CNTRL_MAC_CLOCK_READY, 25) < 0)
        return -1;
    return 0;
}

/* Linux iwl_op_mode_nic_config -> iwl_mvm_nic_config, run inside nic_init
 * BEFORE the firmware load on every pre-AX210 part: program the MAC
 * step/dash (from CSR_HW_REV) and the RADIO type/step/dash straps (from the
 * firmware's PHY_SKU TLV) plus the RADIO_SI/MAC_SI sampling bits into
 * CSR_HW_IF_CONFIG_REG. We never wrote any of it (HW_IF read 0x00480000 on
 * the Yoga) - a boot ROM asked to load an HR-RF image with the radio straps
 * unset is a plausible silent-refuse. */
static void nic_config_radio(void)
{
    u32 pc = g_fw.phy_sku, val, mask;
    if (g_family >= FAM_AX210 || !pc) return;
    val  = g_hw_rev & 0x0000000F;                 /* CSR_HW_REV_STEP_DASH */
    val |= ((pc >> 0) & 3) << 10;                 /* radio type  -> MSK 0x0C00 */
    val |= ((pc >> 4) & 3) << 12;                 /* radio dash  -> MSK 0x3000 */
    val |= ((pc >> 2) & 3) << 14;                 /* radio step  -> MSK 0xC000 */
    /* MAC_SI/RADIO_SI force the MAC to re-sample the radio/silicon straps.
     * The working Linux QuZ (gen2 CNVi) load leaves them CLEAR (ground-truth
     * ftrace HW_IF=0x18489001, not ...9301); forcing the re-sample on this part
     * feeds PHY/RF init a strap state Linux never uses - a plausible silent
     * wedge that runs the ROM but never reaches ALIVE (F12).  Keep 0x300 in the
     * mask so gen2 deterministically CLEARS them; only set them on older parts. */
    if (!g_gen2) val |= 0x00000100 /*MAC_SI*/ | 0x00000200 /*RADIO_SI*/;
    mask = 0x0000000F | 0x00000C00 | 0x00003000 | 0x0000C000 | 0x00000300;
    w32(CSR_HW_IF_CONFIG_REG, (r32(CSR_HW_IF_CONFIG_REG) & ~mask) | val);
    uno_dbg_net_trace("wifi: nic_config: phy_sku=%08x -> HW_IF=%08x",
                      pc, r32(CSR_HW_IF_CONFIG_REG));
}

/* Configure MSI-X. THE F12 FIX: the gen2 AX201 boot ROM refuses to start the
 * firmware load until MSI-X is enabled and its interrupt causes are mapped +
 * unmasked (ground-truth from tracing a working Linux load, 2026-07-22). We
 * poll the RB for ALIVE and never service these vectors - this only puts the
 * device in the state the ROM validates. Values replay the working trace:
 *   UREG_CHICK = MSIX_ENABLE (bit25, not the MSI bit24 we used before)
 *   IVAR table (byte writes) mapping RX + HW causes to a vector
 *   FH mask 0xfe00 / HW mask 0xfffffffe (ALIVE unmasked) - set at fw load. */
static void conf_msix(void)
{
    int i;
    /* config-register writes the working trace does at this phase and we were
     * missing: io[0x3c] (a HW config strap) + the gen2 HPET debug filter.
     * io[0x100] (GIO chicken) we already set via apm_init's set_bit. */
    w32(0x03c, 0x001f0042u);
    w32(CSR_DBG_HPET_MEM_REG, DBG_HPET_MEM_VAL);   /* io[0x240]=0xffff0000, gen2 too */
    prph_w(uprph(UREG_CHICK), UREG_CHICK_MSIX);
    /* RX IVARs: index 0 = 0, indices 1..8 = their queue number (trace) */
    w8_(CSR_MSIX_RX_IVAR_BASE + 0, 0x00);
    for (i = 1; i <= 8; i++) w8_(CSR_MSIX_RX_IVAR_BASE + i, (u8)i);
    /* HW/FH cause IVARs: enable|vector at the exact offsets Linux programs for
     * this part (from the trace); harmless extras, missing ones is what stalls. */
    { static const u8 iv[] = { 0x00,0x01,0x03,0x05, 0x10,0x11,0x12,0x13,
                               0x16,0x17,0x18, 0x29,0x2a,0x2b,0x2d,0x2e };
      for (i = 0; i < (int)(sizeof iv); i++)
          w8_(CSR_MSIX_IVAR_BASE + iv[i], MSIX_IVAR_ENA); }
    /* clear the interrupt-cause status, then set the fw-load masks (ALIVE +
     * FH unmasked) - this is the enable the ROM waits on before it DMAs. */
    w32(CSR_MSIX_FH_INT_CAUSES_AD, 0xffffffffu);
    w32(CSR_MSIX_HW_INT_CAUSES_AD, 0xffffffffu);
    w32(CSR_MSIX_FH_INT_MASK_AD, MSIX_FH_MASK_FWLOAD);
    w32(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_MASK_FWLOAD);
    uno_dbg_net_trace("wifi: MSI-X+PCI[v3] configured: CHICK=%08x FHmask=%08x HWmask=%08x WFPM=%08x",
                      prph_r(uprph(UREG_CHICK)),
                      r32(CSR_MSIX_FH_INT_MASK_AD), r32(CSR_MSIX_HW_INT_MASK_AD),
                      prph_r(WFPM_GP1_ENA));
}

/* grab NIC access so PRPH/SRAM writes land (refcounted: prph_w/prph_r grab
 * for themselves, and some callers already hold access around a batch) */
static int g_nic_ref;
static int g_grab_fail;    /* autopsy: how many PRPH ops ran without access */
static int grab_nic(void)
{
    if (g_nic_ref) { g_nic_ref++; return 0; }
    set_bit_(CSR_GP_CNTRL, GP_CNTRL_MAC_ACCESS_REQ);
    if (g_family >= FAM_8000) udelay_(2000);
    if (poll_bit(CSR_GP_CNTRL, GP_CNTRL_MAC_CLOCK_READY,
                 GP_CNTRL_MAC_CLOCK_READY, 15) < 0) { g_grab_fail++; return -1; }
    g_nic_ref = 1;
    return 0;
}
static void release_nic(void)
{
    if (g_nic_ref > 0 && --g_nic_ref) return;
    clr_bit_(CSR_GP_CNTRL, GP_CNTRL_MAC_ACCESS_REQ);
}

#if UNO_DEBUG
/* Read `dwords` 32-bit words of device SRAM from `addr` into buf, via the HBUS
 * auto-incrementing target-memory read port (write the base to RADDR once, then
 * each RDAT read pulls the next word).  Mirrors iwl_trans_pcie_read_mem.  Needs
 * NIC access; on grab failure the buffer is poisoned so a caller can tell.
 * Debug-only: it exists purely to feed the iwl mem/iwl fwerr verbs. */
static void mem_read(u32 addr, u32 *buf, int dwords)
{
    int i;
    if (grab_nic() < 0) { for (i = 0; i < dwords; i++) buf[i] = 0xdeadbeef; return; }
    w32(HBUS_TARG_MEM_RADDR, addr);
    for (i = 0; i < dwords; i++) buf[i] = r32(HBUS_TARG_MEM_RDAT);
    release_nic();
}
#endif

static int rf_killed(void) { return (r32(CSR_GP_CNTRL) & GP_CNTRL_HW_RF_KILL_SW) ? 0 : 1; }

/* =====================================================================
 * 6. RX ring + TX/command queue (host DRAM structures)
 * ===================================================================== */
/* We run ONE rx queue and the fixed command queue plus one data/mgmt TFD ring.
   Sizes kept small (polled, low throughput). */
/* Linux sizes the free-RBD cyclic buffer from cfg->num_rbds, and for THIS part
 * (iwl_ax201_cfg_quz_hr) that is IWL_NUM_RBDS_22000_HE = 2048, so the CB_SIZE
 * it hands the firmware in context_info.control_flags is ilog2(2048) = 11.  We
 * used 256/8.  The ring sizing is invisible to an MMIO trace - it only ever
 * reaches the device as a field in the DMA'd context-info - which is why it
 * survived every register-level audit.  Match Linux exactly. */
#define RXQ_N        2048
#define RXQ_CB_SIZE  11              /* ilog2(RXQ_N) - keep the two in step */
#define RB_SIZE      4096
#define CMDQ_N       32
#define TXQ_N        256
#define FIRST_TB     20

/* legacy rb status (device writes closed_rb_num); AX210 uses a bare u16 */
struct rb_status { u16 closed_rb_num, closed_fr_num, finished_rb_num, finished_fr_num; u32 spare; };

/* gen1 TFD (128 B, 20 TBs) and gen2 TFH TFD (256 B, 25 TBs) */
struct tfd_tb { u32 lo; u16 hi_n_len; } __attribute__((packed));
struct tfd    { u8 rsv[3]; u8 num_tbs; struct tfd_tb tbs[20]; u32 pad; } __attribute__((packed));
struct tfh_tb { u16 tb_len; u64 addr; } __attribute__((packed));
struct tfh_tfd{ u16 num_tbs; struct tfh_tb tbs[25]; u32 pad; } __attribute__((packed));

/* DMA blocks (aligned; phys==virt). Rings are the largest static cost. */
static struct rb_status g_rbstts __attribute__((aligned(256)));
static u32 g_rbd_free_le32[RXQ_N] __attribute__((aligned(256)));  /* legacy/9000 free list */
static u64 g_rbd_free_le64[RXQ_N] __attribute__((aligned(256)));  /* 9000 mq free (addr|vid) */
static u64 g_rbd_used[RXQ_N]      __attribute__((aligned(256)));  /* mq used/completion list */
static u8  g_rb[RXQ_N][RB_SIZE]   __attribute__((aligned(4096)));

static u8  g_cmd_ring[CMDQ_N * 256]  __attribute__((aligned(256)));  /* TFD or TFH per slot */
static u8  g_cmd_buf[CMDQ_N][2048]    __attribute__((aligned(64)));   /* per-slot command DRAM (2K: UMAC scan v17 ~1940 B) */
static u8  g_cmd_firsttb[CMDQ_N][64]  __attribute__((aligned(64)));   /* 20-byte scratch (bidir) */
static u16 g_cmd_bc[CMDQ_N + 64]      __attribute__((aligned(64)));   /* byte-count table */

static u8  g_tx_ring[TXQ_N * 256]     __attribute__((aligned(256)));
static u8  g_tx_buf[TXQ_N][2048]      __attribute__((aligned(64)));
static u8  g_tx_firsttb[TXQ_N][64]    __attribute__((aligned(64)));
static u16 g_tx_bc[TXQ_N + 64]        __attribute__((aligned(64)));

static int g_cmd_wr, g_tx_wr, g_rx_read, g_rx_write;
static int g_data_qid = -1;              /* fw-assigned data queue (gen2/3) */
#define AP_STA_ID 0                      /* the AP peer's station index */

/* =====================================================================
 * 6a. RX ring init + restock + read
 * ===================================================================== */
static void rx_alloc_lists(void)
{
    int i;
    /* WIPE THE BUFFERS, not just the descriptors.
     *
     * This reset the free/used lists and the indices and left 8 MB of previous
     * frames sitting in g_rb[]. That was harmless while a boot did exactly one
     * bring-up. The candidate-ucode loop does up to three, and the ALIVE-timeout
     * autopsy brute-scans EVERY RB for an ALIVE notification when the closed
     * index looks empty (this card's rb_status DMA is unreliable, so that scan
     * is load-bearing). A previous candidate's ALIVE is therefore still lying
     * in RB 0 when the next candidate's autopsy runs, and gets read as proof
     * that THIS image booted.
     *
     * Metal, SURFGO 2026-08-04, the two autopsies in one boot:
     *   candidate 1, first bring-up:  rb0[0..7]=0000000000000000  -> honest fail
     *   candidate 1, second bring-up: rb0[0..7]=94000020010000c0  -> "ALIVE FOUND
     *     by brute scan in RB 0 - fw BOOTED ... Proceeding"
     * The second is the first bring-up's IWLAX20C ALIVE. The driver then ran MVM
     * init and a scan against an IWLAX20B image that had never started, and the
     * Network app reported "no networks found" on a radio it believed was up.
     *
     * 8 MB at the measured ~2.6 GB/s is about 3 ms, once per bring-up attempt,
     * against a path that takes seconds. Zeroing all of it beats poisoning the
     * first few bytes of each buffer: rx_process_rb() walks packets THROUGH an
     * RB, so a partial clear just moves the stale frame out of reach of the
     * cheap check and leaves it findable by the thorough one. */
    memset(g_rb, 0, sizeof g_rb);
    for (i = 0; i < RXQ_N; i++) {
        g_rbd_free_le32[i] = (u32)(phys(g_rb[i]) >> 8);
        g_rbd_free_le64[i] = phys(g_rb[i]) | (u64)(i + 1);   /* vid = i+1 */
        g_rbd_used[i] = 0;
    }
    g_rbstts.closed_rb_num = 0;
    g_rx_read = 0; g_rx_write = RXQ_N - 1;
}

/* FH (gen1) rx register block */
#define FH_MEM 0x1000
#define FH_RSCSR (FH_MEM + 0xBC0)
#define FH_RSCSR_STTS_WPTR (FH_RSCSR + 0x000)
#define FH_RSCSR_RBDCB_BASE (FH_RSCSR + 0x004)
#define FH_RSCSR_RBDCB_WPTR (FH_RSCSR + 0x008)
#define FH_RSCSR_RDPTR     (FH_RSCSR + 0x00c)
#define FH_RCSR (FH_MEM + 0xC00)
#define FH_RCSR_CONFIG (FH_RCSR + 0x000)
#define FH_RCSR_RBDCB_WPTR (FH_RCSR + 0x008)
#define FH_RCSR_FLUSH  (FH_RCSR + 0x010)
/* RFH (9000 mq) */
#define RFH_Q0_FRBDCB_BA_LSB 0xA08000
#define RFH_Q0_FRBDCB_WIDX   0xA08080
#define RFH_Q0_FRBDCB_WIDX_TRG 0x1C80        /* CSR shadow of the WIDX (write via w32) */
#define RFH_Q0_FRBDCB_RIDX   0xA080C0
#define RFH_Q0_URBDCB_BA_LSB 0xA08100
#define RFH_Q0_URBDCB_WIDX   0xA08180
#define RFH_Q0_URBD_STTS_WPTR_LSB 0xA08200
#define RFH_RXF_DMA_CFG 0xA09820
#define RFH_GEN_CFG     0xA09800
#define RFH_RXF_RXQ_ACTIVE 0xA0980C
#define RFH_DMA_EN       (1u<<31)
#define RFH_DMA_RB_4K    (0x4<<16)
#define RFH_DMA_MIN_RB_4_8 (3u<<24)
#define RFH_DMA_DROP_LARGE (1u<<26)
#define RFH_DMA_RBDCB_512 (0x9<<20)
#define RFH_GEN_SVC_SNOOP (1u<<0)
#define RFH_GEN_DMA_SNOOP (1u<<1)

static void prph_w64(u32 reg, u64 v) { prph_w(reg, (u32)v); prph_w(reg+4, (u32)(v>>32)); }

static void rx_hw_init(void)
{
    rx_alloc_lists();
    if (grab_nic() < 0) return;
    if (!g_mq_rx) {
        /* legacy single-queue (7000/8000) */
        w32(FH_RCSR_CONFIG, 0);
        w32(FH_RCSR_RBDCB_WPTR, 0);
        w32(FH_RCSR_FLUSH, 0);
        w32(FH_RSCSR_RDPTR, 0);
        w32(FH_RSCSR_RBDCB_WPTR, 0);
        w32(FH_RSCSR_RBDCB_BASE, (u32)(phys(g_rbd_free_le32) >> 8));
        w32(FH_RSCSR_STTS_WPTR, (u32)(phys(&g_rbstts) >> 4));
        w32(FH_RCSR_CONFIG, 0x80000000 | 0x00000004 | 0x00001000 |
                            (0x11u<<4) | (8u<<20));   /* enable, ignore-empty, host-int, RB timeout, 256 RBD */
    } else if (!g_gen2) {
        /* 9000 gen1-transport mq: program the RFH */
        u32 enabled = 0;
        prph_w(RFH_RXF_DMA_CFG, 0);
        prph_w(RFH_RXF_RXQ_ACTIVE, 0);
        prph_w64(RFH_Q0_FRBDCB_BA_LSB, phys(g_rbd_free_le64));
        prph_w64(RFH_Q0_URBDCB_BA_LSB, phys(g_rbd_used));
        prph_w64(RFH_Q0_URBD_STTS_WPTR_LSB, phys(&g_rbstts));
        prph_w(RFH_Q0_FRBDCB_WIDX, 0);
        prph_w(RFH_Q0_FRBDCB_RIDX, 0);
        prph_w(RFH_Q0_URBDCB_WIDX, 0);
        enabled = (1u<<0) | (1u<<16);
        prph_w(RFH_RXF_DMA_CFG, RFH_DMA_EN | RFH_DMA_RB_4K | RFH_DMA_MIN_RB_4_8 |
                                RFH_DMA_DROP_LARGE | RFH_DMA_RBDCB_512);
        prph_w(RFH_GEN_CFG, RFH_GEN_SVC_SNOOP | RFH_GEN_DMA_SNOOP | (1u<<4));
        prph_w(RFH_RXF_RXQ_ACTIVE, enabled);
    }
    /* gen2: the RFH is programmed by firmware; we just keep the RB pool */
    release_nic();
    w8_(CSR_INT_COALESCING, 0x40);
    /* push the free-list write pointer (multiple of 8). NOTE: the WIDX_TRG
     * shadow is a CSR write in Linux (iwl_pcie_rxq_inc_wr_ptr uses
     * iwl_write32 RFH_Q_FRBDCB_WIDX_TRG=0x1C80), NOT a PRPH access. */
    g_rx_write = (RXQ_N - 1) & ~7;
    if (!g_mq_rx) w32(FH_RSCSR_RBDCB_WPTR, g_rx_write);
    else if (!g_gen2) w32(RFH_Q0_FRBDCB_WIDX_TRG, g_rx_write);
}

/* Hand consumed RBs back to the firmware. The free list is a static identity
 * mapping (slot i -> rb i, vid i+1) that we never rewrite, so restock is just
 * advancing the write index to one-behind the read index (rounded to 8, as
 * Linux does). Without this the fw exhausts the initial 256 RBDs and RX goes
 * silent - invisible pre-ALIVE, guaranteed once real traffic flows. On gen2
 * the fw programs the RFH from the context info at boot, so the register must
 * not be touched before ALIVE (Linux restocks in fw_alive). */
static int g_alive;
static void rx_restock(void)
{
    int tgt = ((g_rx_read - 1) & (RXQ_N - 1)) & ~7;
    if (tgt == g_rx_write) return;
    if (g_gen2 && !g_alive) return;
    g_rx_write = tgt;
    if (!g_mq_rx) w32(FH_RSCSR_RBDCB_WPTR, g_rx_write);
    else          w32(RFH_Q0_FRBDCB_WIDX_TRG, g_rx_write);
}

static u16 rx_closed(void)
{
    if (g_family >= FAM_AX210) return *(volatile u16 *)&g_rbstts;
    /* the device DMA-writes closed_rb_num; read through volatile so a poll loop
     * re-reads DRAM each iteration instead of caching the first value */
    return ((volatile struct rb_status *)&g_rbstts)->closed_rb_num & 0xFFF;
}

/* =====================================================================
 * 6b. command / TX enqueue (legacy TFD + gen2 TFH) and completion
 * ===================================================================== */
static void tfd_set_tb_gen1(struct tfd *t, u64 addr, int len)
{
    int idx = t->num_tbs;
    t->tbs[idx].lo = (u32)addr;
    t->tbs[idx].hi_n_len = (u16)(((len & 0xFFF) << 4) | ((addr >> 32) & 0xF));
    t->num_tbs = idx + 1;
}
static void tfd_set_tb_gen2(struct tfh_tfd *t, u64 addr, int len)
{
    int idx = t->num_tbs;
    t->tbs[idx].addr = addr;
    t->tbs[idx].tb_len = (u16)len;
    t->num_tbs = idx + 1;
}

/* Build a host command on the command queue. group 0 => short 4-byte header,
   else wide 8-byte. Returns the sequence used (for matching the response). */
static u16 g_cmd_seq_ctr;
static int send_cmd(u8 group, u8 opcode, u8 version, const void *payload, int plen)
{
    int idx = g_cmd_wr & (CMDQ_N - 1);
    u8 *out = g_cmd_buf[idx];
    int hdr = group ? 8 : 4;
    int copy, tb0;
    u16 seq = (u16)(idx & 0xff);        /* [7:0] tfd idx; cmd queue = 0 */

    out[0] = opcode; out[1] = group;
    out[2] = (u8)seq; out[3] = (u8)(seq>>8);
    if (group) { out[4]=(u8)plen; out[5]=(u8)(plen>>8); out[6]=0; out[7]=version; }
    if (plen > 0 && plen <= (int)sizeof g_cmd_buf[0] - hdr) memcpy(out + hdr, payload, plen);
    copy = hdr + plen;

    tb0 = copy < FIRST_TB ? copy : FIRST_TB;
    memcpy(g_cmd_firsttb[idx], out, tb0);

    if (g_gen2) {
        struct tfh_tfd *t = (struct tfh_tfd *)(g_cmd_ring + idx*256);
        memset(t, 0, sizeof *t);
        tfd_set_tb_gen2(t, phys(g_cmd_firsttb[idx]), tb0);
        if (copy > tb0) tfd_set_tb_gen2(t, phys(out + tb0), copy - tb0);
    } else {
        struct tfd *t = (struct tfd *)(g_cmd_ring + idx*128);
        memset(t, 0, sizeof *t);
        tfd_set_tb_gen1(t, phys(g_cmd_firsttb[idx]), tb0);
        if (copy > tb0) tfd_set_tb_gen1(t, phys(out + tb0), copy - tb0);
    }
    g_cmd_wr = (g_cmd_wr + 1) & (CMDQ_N - 1);
    if (g_gen2) w32(HBUS_TARG_WRPTR, g_cmd_wr | (0 << 16));   /* cmd queue id 0 */
    else        w32(HBUS_TARG_WRPTR, g_cmd_wr | (0 << 8));
    (void)version; (void)g_cmd_seq_ctr;
    return idx & 0xff;
}

/* 802.11 MAC header length for a frame we are about to transmit: 24, +2 for a
 * QoS data subtype. Used both to place the TX_CMD's copy of the header and to
 * decide whether the fw needs the 2-byte pad marker. */
static int hdrlen_80211(const u8 *frame)
{
    u16 fc = (u16)(frame[0] | (frame[1] << 8));
    int type = (fc >> 2) & 3, subtype = (fc >> 4) & 0xF;
    return (type == 2 && (subtype & 8)) ? 26 : 24;      /* QoS data carries QC */
}

/* Enqueue an 802.11 frame on the data TX queue wrapped in a TX_CMD.
 *
 * gen2 wire layout of the DMA'd buffer, per iwl_txq_gen2_build_tx:
 *
 *   [ iwl_cmd_header 4 B ][ iwl_tx_cmd_gen2 20 B ][ 802.11 hdr ][pad][ body ]
 *
 * The COMMAND HEADER IS PART OF IT. Omitting it (which this function used to do,
 * starting straight at iwl_tx_cmd_gen2) makes the fw read the frame length as
 * {cmd, group_id}: a 30-byte auth frame arrives as cmd 0x1e group 0 =
 * TXPATH_FLUSH, so nothing is ever transmitted and no TX status comes back.
 * That is exactly what metal showed - 0 REPLY_TX notifications, ever.
 *
 * Other details that have to be right, all from iwl_mvm_set_tx_params +
 * iwl_txq_gen2_tx:
 *  - hdr.sequence carries the QUEUE id, not just the TFD index.
 *  - the byte-count entry is the 802.11 frame length (tx_cmd->len), NOT the
 *    total TFD payload.
 *  - IWL_TX_FLAGS_CMD_RATE must be set or the fw looks up a rate-scaling table
 *    we never configured (no TLC_MNG_CONFIG_CMD).
 *  - IWL_TX_FLAGS_ENCRYPT_DIS must be set until a key is installed, or the fw
 *    tries to encrypt auth/assoc/EAPOL with a key that does not exist.
 *  - this fw reports TX_CMD cmd_ver 9 (> 8), so rate_n_flags is the **v2**
 *    encoding: bits 0-3 a rate INDEX (not a PLCP), bits 8-10 a modulation type,
 *    bits 14-15 the antenna. 1 Mbps CCK on antenna A is 0x4000, where the old
 *    v1 encoding we used (0x420a) decodes as HT garbage.
 */
static int g_keys_installed;
static int g_igtk_install;   /* `iwl igtk 1`: hand the BIP key to the fw */
static int g_key_gtk_idx = -1;   /* GTK index live on the station, -1 = none */
/* Remaining per-frame TX log lines; see the note at the trace site. Sized to
 * cover a whole join (auth, assoc, the four EAPOL messages, first data out)
 * and then fall silent. `iwl txlog <n>` re-arms it. */
static int g_tx_log = 32;
static void tx_enqueue(const u8 *frame, int flen, int high_pri)
{
    int idx = g_tx_wr & (TXQ_N - 1);
    u8 *out = g_tx_buf[idx];
    int tb0, total, qid, h80211, pad, body;
    u16 seq;
    if (flen <= 24 || flen > 2048 - 64) return;
    qid = g_data_qid >= 0 ? g_data_qid : 10;     /* DATA pool base until fw assigns */
    h80211 = hdrlen_80211(frame);
    if (h80211 > flen) h80211 = flen;
    body = flen - h80211;

    memset(out, 0, 64);
    if (g_gen2) {
        u32 rnf, ant = fw_valid_tx_ant();
        u16 off = 0, flags;
        pad = (h80211 & 3) ? 2 : 0;              /* TX_CMD_OFFLD_PAD territory */
        seq = (u16)(((qid & 0x1f) << 8) | (idx & 0xff));
        /* iwl_cmd_header: cmd, group_id, sequence */
        out[0] = 0x1c;                           /* TX_CMD */
        out[1] = 0;                              /* LEGACY group */
        out[2] = (u8)seq; out[3] = (u8)(seq >> 8);
        /* iwl_tx_cmd_gen2 @4: len, offload_assist, flags, dram_info[8], rate_n_flags */
        out[4] = (u8)flen; out[5] = (u8)(flen >> 8);
        if (pad) off |= (1u << 13);              /* TX_CMD_OFFLD_PAD */
        out[6] = (u8)off; out[7] = (u8)(off >> 8);
        flags = (1u << 0);                       /* IWL_TX_FLAGS_CMD_RATE */
        if (!g_keys_installed || ((frame[0] >> 2) & 3) != 2)
            flags |= (1u << 1);                  /* IWL_TX_FLAGS_ENCRYPT_DIS */
        if (high_pri) flags |= (1u << 2);        /* IWL_TX_FLAGS_HIGH_PRI */
        *(u32*)(out + 8) = flags;
        /* dram_info @12..19 stays zero (no PN/key offload yet) */
        ant = ant & (~ant + 1);                  /* lowest valid TX antenna */
        rnf = 0 /*rate idx 0 = 1 Mbps*/ | (0u << 8) /*RATE_MCS_CCK*/ | (ant << 14);
        *(u32*)(out + 20) = rnf;
        /* the 802.11 header sits in the command, then (pad), then the body */
        memcpy(out + 24, frame, h80211);
        if (pad) memset(out + 24 + h80211, 0, pad);
        memcpy(out + 24 + h80211 + pad, frame + h80211, body);
        total = 24 + h80211 + pad + body;
    } else {
        /* iwl_tx_cmd_v6 params: len@0, tx_flags@4, rate_n_flags@12, sta_id@16 */
        pad = 0;
        out[0] = (u8)flen; out[1] = (u8)(flen >> 8);
        { u32 fl = (1u<<3); *(u32*)(out+4) = fl; }   /* TX_CMD_FLG_ACK */
        { u32 rnf = 10 | (1u<<9) | (1u<<14); *(u32*)(out+12) = rnf; }
        out[16] = AP_STA_ID;
        out[17] = high_pri ? 0 : (2 | 0x10);         /* sec_ctl CCM|KEY_FROM_TABLE for data */
        memcpy(out + 56, frame, flen);
        total = 56 + flen;
    }

    tb0 = total < FIRST_TB ? total : FIRST_TB;
    memcpy(g_tx_firsttb[idx], out, tb0);
    if (g_gen2) {
        struct tfh_tfd *t = (struct tfh_tfd *)(g_tx_ring + idx*256);
        int nchunks;
        memset(t, 0, sizeof *t);
        tfd_set_tb_gen2(t, phys(g_tx_firsttb[idx]), tb0);
        if (total > tb0) tfd_set_tb_gen2(t, phys(out + tb0), total - tb0);
        nchunks = ((int)(sizeof(u16) + t->num_tbs*sizeof(struct tfh_tb)) + 63)/64 - 1;
        if (nchunks < 0) nchunks = 0;
        /* byte count = the OVER-THE-AIR frame length in dwords, not the TFD total */
        g_tx_bc[idx] = (u16)(((flen + 3)/4) | (nchunks << 12));
    } else {
        struct tfd *t = (struct tfd *)(g_tx_ring + idx*128);
        memset(t, 0, sizeof *t);
        tfd_set_tb_gen1(t, phys(g_tx_firsttb[idx]), tb0);
        if (total > tb0) tfd_set_tb_gen1(t, phys(out + tb0), total - tb0);
        g_tx_bc[idx] = (u16)((total + 3)/4);
    }
    /* One line per TRANSMITTED FRAME, on a budget.
     *
     * Ungated, this is the single loudest thing in the kernel log: every data
     * frame, every keepalive, and - once URC rides the WiFi link - every frame
     * of the remote session itself, which is then sent over that link.  On
     * SURFGO the entire ring tail was this and nothing else, so the deauth,
     * EAPOL and join lines that the ring exists to preserve had all scrolled
     * out.  On a machine with no wired NIC the crash log is the ONLY diagnostic
     * channel, and this was drowning it.
     *
     * A budget rather than an off switch: the frames worth seeing are the first
     * ones - auth, assoc, the EAPOL exchange, the first data out - and those
     * fit easily. After that it goes quiet AND SAYS SO, because a log that
     * stops without explanation is its own bug report. `iwl txlog <n>` re-arms
     * it (0 = silent). */
    if (g_tx_log > 0) {
        g_tx_log--;
        uno_dbg_net_trace("wifi: TX q=%d idx=%d seq=%04x flen=%d hdr=%d pad=%d tfd=%d bc=%04x",
                          qid, idx, (unsigned)(((qid & 0x1f) << 8) | (idx & 0xff)),
                          flen, h80211, pad, total, g_tx_bc[idx]);
        if (!g_tx_log)
            uno_dbg_net_trace("wifi: TX log budget spent - further frames are NOT logged "
                              "('iwl txlog <n>' for more)");
    }
    g_tx_wr = (g_tx_wr + 1) & (TXQ_N - 1);
    if (g_gen2) w32(HBUS_TARG_WRPTR, g_tx_wr | (qid << 16));
    else        w32(HBUS_TARG_WRPTR, g_tx_wr | (qid << 8));
}

/* =====================================================================
 * 6c. RX notification poll: returns a pointer to the next iwl_rx_packet
 * payload matching (group,cmd), or NULL within timeout. Also feeds 802.11
 * data frames to the recv path via a small ring (below).
 * ===================================================================== */
struct rx_packet { u32 len_n_flags; u8 cmd; u8 group_id; u16 sequence; u8 data[]; };
#define FRAME_SIZE_MSK 0x3FFF
#define SEQ_RX_FRAME   0x8000

/* stashed decrypted data frames for recv() */
#define DATAQ 16
static struct { u8 buf[1600]; int len; } g_dataq[DATAQ];
static int g_dq_head, g_dq_tail;

static void handle_data_frame(const u8 *frame, int len, int snap_off);  /* fwd (802.11->eth) */
static void handle_eapol(const u8 *eapol, int len);        /* fwd (EAPOL hdr, not the 802.11 frame) */
static void nic_publish(void);                             /* fwd (bind the uno_nic_t) */
static void scan_record_beacon(const u8 *frame, int fl, int rssi, int dchan);  /* fwd (beacon parse) */
static void mgmt_capture(const u8 *frame, int fl, u16 fc);  /* fwd (auth/assoc resp) */
static u8  g_mgmt_rx[512];     /* last mgmt frame addressed to us (auth/assoc/deauth) */
static int g_mgmt_diag, g_mgmt_diag_n;   /* log RX grp/cmd during auth/assoc wait */
static int g_mgmt_diag_other;            /* budget for the interesting (non-beacon) ones */
static int g_txresp_n;                   /* REPLY_TX dumps emitted */
static int g_mgmt_rx_len;
static u8  g_mgmt_rx_subtype;
static int g_scanning;   /* beacons are only harvested while a scan is active */
static int g_scan_mpdu_seen, g_scan_beacon_calls, g_scan_rb_total;   /* scan diagnostics */
static const u8 SNAP[6] = { 0xAA,0xAA,0x03,0x00,0x00,0x00 };
static u8  g_bssid[6];                                 /* the AP we joined */
/* The security profile of the BSS we are joining, decided once at select_ap()
 * from its beacon and then consulted by auth, assoc, the firmware station
 * config and the supplicant - so those four cannot disagree about what is
 * being negotiated.  Declared up here with g_bssid because mld_sta_cfg(),
 * far above the join code, has to know whether PMF is in play. */
static wpa_ap_sec_t g_ap_sec;
static int g_join_akm;              /* WPA_AKM_SAE / WPA_AKM_PSK / 0          */
static int g_akm_force;             /* second-pass override; 0 = pick freely   */
static int g_sae_silent;            /* last SAE got no commit back at all      */
static int g_auth_ok;               /* the AP authenticated us this attempt   */
static int g_join_mfp;              /* WPA_MFP_*                              */
static u8  g_rsn_ie[64];            /* the EXACT element sent in assoc-req    */
static int g_rsn_ie_len;
static const char *akm_name(int akm)
{
    return akm == WPA_AKM_SAE ? "WPA3-SAE" : akm == WPA_AKM_PSK ? "WPA2-PSK" : "none";
}
static u32 g_rx_data_n, g_rx_data_drop, g_tx_data_n;   /* data-path counters */
/* ---- post-join diagnosis (see iwl_link) ---------------------------------
 * "Associated, but no DHCP lease" has two candidate explanations and no
 * evidence separating them, so these count the handful of things that do.
 * All of them are increments on paths that already run; nothing here changes
 * behaviour. */
static u32 g_txr_total;        /* TX_STATUS notifications seen                */
static u32 g_txr_ackfail;      /* ...of those, ones the AP did not ACK        */
static u32 g_rx_from_ap;       /* frames whose transmitter IS our BSSID       */
static u32 g_deauth_ours;      /* deauth/disassoc FROM the AP we joined       */
static u32 g_deauth_other;     /* ...from any other BSSID (steering, usually) */
static int g_deauth_reason = -1;             /* the most recent reason code   */
static unsigned long long g_join_ms;         /* when the 4-way completed      */
static int g_postjoin_diag_done;
/* ---- who the frame is from, and what the firmware did with it -------------
 * Two facts the counters above could not tell apart, and both of them decide
 * what "69 frames dropped" means:
 *
 *  - a mesh puts several BSSes on one channel, and an encrypted data frame
 *    from a BSS we did NOT associate with is not evidence of anything. It is
 *    also not ours to deliver, which is what `foreign` now separates.
 *  - `prot` vs `prot_dec` is the hardware's own answer to "did you decrypt
 *    this": every protected frame from our AP, and how many of them arrived
 *    decrypted. prot > 0 with prot_dec == 0 is a key that was never armed,
 *    and it is a different fault from a key that is wrong.               */
static u32 g_rx_foreign;       /* data frames from a BSS we are not joined to */
static u32 g_rx_prot;          /* protected data frames FROM OUR AP           */
static u32 g_rx_prot_dec;      /* ...of those, ones that arrived decrypted    */
static u32 g_rx_st_undec;      /* the rx descriptor status of the last one that did not */
static int g_rx_data_log;      /* log the next N received data frames (debug) */
static int g_rx_mic = -1;      /* does the fw leave the CCMP MIC on? -1 = not learned yet */
static int g_last_rssi;        /* strongest energy seen on a frame from our AP */

/* process one received RB: walk packed iwl_rx_packet records */
static void rx_process_rb(const u8 *rb, int cap,
                          int want_group, int want_cmd, const u8 **found, int *found_len)
{
    int off = 0;
    while (off + 8 <= cap) {
        const struct rx_packet *pkt = (const struct rx_packet *)(rb + off);
        int plen = pkt->len_n_flags & FRAME_SIZE_MSK;
        if (plen < 4 || off + 4 + plen > cap) break;
        if (g_scanning) { g_scan_rb_total++;
            if (g_scan_rb_total <= 12)
                uno_dbg_net_trace("wifi: scan pkt#%d grp=%d cmd=%02x len=%d",
                                  g_scan_rb_total, pkt->group_id, pkt->cmd, plen); }
        /* Diagnostic during the auth/assoc wait. Beacons arrive at ~25/s and used
         * to exhaust a flat 20-entry budget before anything interesting showed up,
         * which hid the REPLY_TX. So count RX_MPDUs but only LOG the notifications
         * that matter (anything that is not a received 802.11 frame). */
        if (g_mgmt_diag) {
            if (pkt->group_id == 0 && pkt->cmd == 0xc1) g_mgmt_diag_n++;
            else if (g_mgmt_diag_other < 12) { g_mgmt_diag_other++;
                uno_dbg_net_trace("wifi: rxnotif grp=%d cmd=%02x len=%d",
                                  pkt->group_id, pkt->cmd, plen); }
        }
        if (found && !*found && pkt->group_id == want_group && pkt->cmd == want_cmd) {
            *found = pkt->data; *found_len = plen - 4;
        }
        /* REPLY_TX (TX response). Hex-dump it rather than decode blind: this fw
         * reports TX_CMD notif_ver 7, and the iwl_mvm_tx_resp layout has moved
         * across versions, so the raw bytes are what we can actually trust.
         * frame_count@0, failure_rts@2, failure_frame@3, initial_rate@4 are
         * stable across all of them; the agg_tx_status (TX_STATUS_SUCCESS=0x01,
         * TX_STATUS_DIRECT_DONE=0x02, failures above 0x80) sits in the tail. */
        /* Count EVERY TX_STATUS, not just the four that get dumped: whether the
         * AP is acknowledging our frames is the single most useful fact about a
         * link that associated and then went quiet. failure_frame@3 is nonzero
         * when the frame was not ACKed. */
        if (pkt->group_id == 0 && pkt->cmd == 0x1c && plen >= 8) {
            g_txr_total++;
            if (pkt->data[3]) g_txr_ackfail++;
        }
        if (pkt->group_id == 0 && pkt->cmd == 0x1c && g_txresp_n < 4) {
            g_txresp_n++;
            const u8 *d = pkt->data; int n = plen - 4, i;
            if (n > 48) n = 48;
            uno_dbg_net_trace("wifi: TXRESP frames=%d btkill=%d fail_rts=%d fail_frame=%d rate=%08x",
                              d[0], d[1], d[2], d[3],
                              (unsigned)(d[4] | (d[5]<<8) | (d[6]<<16) | ((u32)d[7]<<24)));
            for (i = 0; i + 8 <= n; i += 8)
                uno_dbg_net_trace("wifi: TXRESP +%02d: %02x %02x %02x %02x %02x %02x %02x %02x",
                                  i, d[i],d[i+1],d[i+2],d[i+3],d[i+4],d[i+5],d[i+6],d[i+7]);
        }
        /* RX MPDU (a received 802.11 frame) — dispatch EAPOL vs data regardless
           of what we're waiting for, so the handshake makes progress. */
        if (pkt->group_id == 0 && pkt->cmd == 0xc1) {         /* REPLY_RX_MPDU */
            const u8 *frame; int fl, machdr;
            int rssi = 0, dchan = 0;
            u32 st = 0;                  /* iwl_rx_mpdu_desc.status, mq path only */
            if (g_scanning) g_scan_mpdu_seen++;
            if (g_mq_rx) {
                /* iwl_rx_mpdu_desc: mpdu_len@0, mac_flags2@3 (PAD 0x20,
                   HDR_LEN in *2 words). Size = IWL_RX_DESC_SIZE_V1 =
                   offsetofend(desc, v1) = 20 (common DW2-DW6) + 28 (v1
                   DW7-DW13) = 48 for pre-AX210; AX210+ uses the full v3 desc.
                   Was 32 -> frame landed 16B early inside the desc, so the
                   scan saw garbage FC (0x7f30) not a beacon (0x0080). */
                int descsz = (g_family >= FAM_AX210) ? 40 : 48;   /* AX210 v3 size unverified (no HW); 48 = V1 for 9000/22000 */
                int mlen = pkt->data[0] | (pkt->data[1] << 8);
                int pad = (pkt->data[3] & 0x20) ? 2 : 0;
                frame = pkt->data + descsz + pad; fl = mlen;
                machdr = (pkt->data[3] & 0x1f) * 2;
                /* status is a __le32 at desc+12 (DW5 of iwl_rx_mpdu_desc, ahead
                 * of the v1/v3 union, so it is at the same place on every part
                 * this driver binds). It is the firmware's own account of what
                 * it did with the frame, and it has been sitting there unread
                 * through four rounds of inferring the same facts from payload
                 * bytes: SRC_STA_FOUND bit2, KEY_VALID bit3, MIC_OK bit6,
                 * SEC_MASK bits 8-10 (2 = CCM), DECRYPTED bit11, and the
                 * station it matched in bits 24-28. */
                st = (u32)pkt->data[12] | ((u32)pkt->data[13] << 8) |
                     ((u32)pkt->data[14] << 16) | ((u32)pkt->data[15] << 24);
                /* iwl_rx_mpdu_desc_v1 sits at DW7 = desc+20: rss_hash@20,
                 * filter_match@24, rate_n_flags@28, energy_a@32, energy_b@33,
                 * channel@34. RSSI dBm = -max(energy_a, energy_b) (iwlwifi
                 * rxmq.c), and `channel` is the channel the SCANNER WAS PARKED
                 * ON when the frame arrived - the reliable source the DS
                 * Parameter Set is not (5 GHz APs omit IE 3 entirely, which is
                 * why successive scans disagreed about the same BSSID). */
                if (g_family < FAM_AX210 && descsz == 48) {
                    int ea = pkt->data[32] ? -(int)pkt->data[32] : -128;
                    int eb = pkt->data[33] ? -(int)pkt->data[33] : -128;
                    rssi  = ea > eb ? ea : eb;         /* dBm; closest to 0 wins */
                    dchan = pkt->data[34];
                }
            } else {
                /* iwl_rx_mpdu_res_start(4) + frame + status(4) */
                int mlen = pkt->data[0] | (pkt->data[1] << 8);
                frame = pkt->data + 4; fl = mlen;
                machdr = 24;   /* refined from the frame's FC below */
            }
            /* mpdu_len (fl) is descriptor-supplied and independent of the DMA'd
               length: refuse to read fl bytes past the end of rb[cap]. */
            if (fl > 0 && fl < 1600 && 4 + plen <= cap &&
                (int)(frame - rb) + fl <= cap) {
                u16 fc = (u16)(frame[0] | (frame[1] << 8));
                int qos = ((fc >> 4) & 0xF) == 8;
                if (((fc >> 2) & 3) == 0) {          /* type = management */
                    /* Log EVERY non-beacon mgmt frame while waiting, before any
                     * addr1 filtering, so "the AP never replied" can be told
                     * apart from "it replied and we rejected it". */
                    int st_ = (fc >> 4) & 0xF;
                    if (g_mgmt_diag && st_ != 8 && st_ != 5 && g_mgmt_diag_other < 12) {
                        g_mgmt_diag_other++;
                        uno_dbg_net_trace("wifi: mgmt subtype=%d len=%d a1=%02x:%02x:%02x:%02x:%02x:%02x"
                                          " a2=%02x:%02x:%02x:%02x:%02x:%02x mine=%d",
                                          st_, fl,
                                          frame[4],frame[5],frame[6],frame[7],frame[8],frame[9],
                                          frame[10],frame[11],frame[12],frame[13],frame[14],frame[15],
                                          !memcmp(frame + 4, g_mac, 6));
                    }
                    if (g_scanning) scan_record_beacon(frame, fl, rssi, dchan);
                    else            mgmt_capture(frame, fl, fc);
                }
                /* addr2 is the transmitter, and on an infrastructure downlink
                 * that IS the BSSID. Everything below needs to know whether
                 * this frame came from the AP we joined: a mesh runs several
                 * BSSes on one channel, so "an encrypted data frame arrived"
                 * says nothing until you know whose it is. */
                int from_ap = !memcmp(frame + 10, g_bssid, 6);
                if (from_ap) {
                    g_rx_from_ap++;             /* the AP we joined is talking */
                    if (rssi) g_last_rssi = rssi;
                }
                {
                /* Payload offset. With hardware CCMP the firmware decrypts in
                 * place but strips NOTHING: the 8-byte CCMP header still sits
                 * between the MAC header and the LLC/SNAP, and the 8-byte MIC is
                 * still on the tail (mac80211 removes both in
                 * ieee80211_crypto_ccmp_decrypt - iwlwifi only sets
                 * RX_FLAG_DECRYPTED and reports crypt_len = 8). Probing for the
                 * SNAP at both offsets covers either behaviour instead of
                 * guessing: before this, every encrypted data frame failed the
                 * SNAP test at hdr+0 and was dropped without a trace, which is
                 * why no byte of data ever reached the stack. */
                int hl = machdr ? machdr : (qos ? 26 : 24);
                int poff = hl, dlen = fl, enc = 0;
                if (fl > hl + 16 && memcmp(frame + hl, SNAP, 6) &&
                    !memcmp(frame + hl + 8, SNAP, 6)) {
                    poff = hl + 8; enc = 1;               /* CCMP/TKIP ext-IV header */
                    /* Trailing MIC: whether the fw leaves it on is a property of
                     * the firmware, and guessing wrong either truncates a packet
                     * by 8 bytes or leaves 8 stray ones. The payload settles it -
                     * an IPv4 total_length (or ARP's fixed 28) says how much of
                     * what follows is real - so learn it once from the first
                     * frame that carries a length and apply that answer after. */
                    { int avail = fl - poff - 8;
                      u16 pet = (u16)((frame[poff+6] << 8) | frame[poff+7]);
                      int want = -1;
                      if (pet == 0x0800 && avail >= 20)
                          want = (frame[poff+10] << 8) | frame[poff+11];
                      else if (pet == 0x0806 && avail >= 28) want = 28;
                      if (want > 0 && want <= avail) g_rx_mic = (avail - 8 >= want);
                      if (g_rx_mic == 1 && dlen > poff + 16) dlen -= 8; }
                }
                /* A data frame from a BSS we are not associated with is not
                 * ours to deliver and not ours to reason about. Both used to
                 * happen: another BSS's broadcast traffic went into the IP
                 * stack as if the AP had sent it, and its encrypted frames
                 * were counted as our drops - on a mesh channel that is the
                 * majority of what arrives. */
                int is_data = ((fc >> 2) & 3) == 2;
                int prot = (fc & 0x4000) != 0;
                if (is_data && !from_ap) {
                    if (!g_rx_foreign++ && g_rx_data_log > 0)
                        uno_dbg_net_trace("wifi: RXDATA foreign BSS fc=%04x len=%d "
                                          "ta=%02x:%02x:%02x:%02x:%02x:%02x - ignored "
                                          "(further ones counted only)", fc, fl,
                                          frame[10],frame[11],frame[12],frame[13],
                                          frame[14],frame[15]);
                } else if (dlen > poff + 8 && !memcmp(frame + poff, SNAP, 6)) {
                    u16 et = (u16)((frame[poff + 6] << 8) | frame[poff + 7]);
                    if (is_data && prot) { g_rx_prot++; g_rx_prot_dec++; }
                    if (g_rx_data_log > 0 && is_data) {
                        g_rx_data_log--;
                        uno_dbg_net_trace("wifi: RXDATA fc=%04x len=%d hdr=%d enc=%d et=%04x "
                                          "sa=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d st=%08x",
                                          fc, fl, hl, enc, et,
                                          frame[16],frame[17],frame[18],frame[19],frame[20],frame[21],
                                          rssi, (unsigned)st);
                    }
                    /* wpa_sm_rx_eapol() parses from the EAPOL HEADER, not the
                     * 802.11 header - its first check is frame[1] == 0x03
                     * (EAPOL-Key). Handing it the whole frame made it bail
                     * instantly ("EAPOL frame in (131 bytes) -> sm state 0,
                     * reply 0" on metal), so the AP got no 2/4 and deauthed us.
                     * Skip the MAC header, any CCMP header, and the LLC/SNAP. */
                    if (et == 0x888E) handle_eapol(frame + poff + 8, dlen - poff - 8);
                    else              handle_data_frame(frame, dlen, poff);
                } else if (is_data && fl > hl) {
                    g_rx_data_drop++;
                    if (prot) { g_rx_prot++; g_rx_st_undec = st; }
                    if (g_rx_data_log > 0) {
                        g_rx_data_log--;
                        /* The status word is the firmware answering the question
                         * this line exists to ask. sta=1 means it matched the
                         * frame to a station at all, key=1 that it found a key
                         * for it, sec is the cipher it decided the frame carried
                         * (0 = none, 2 = CCM), dec=1 that it decrypted. A
                         * protected frame with sec=0 was never even looked at. */
                        uno_dbg_net_trace("wifi: RXDATA DROP fc=%04x len=%d hdr=%d st=%08x "
                                          "sta=%d key=%d sec=%d mic=%d dec=%d staid=%d",
                                          fc, fl, hl, (unsigned)st,
                                          (int)((st >> 2) & 1), (int)((st >> 3) & 1),
                                          (int)((st >> 8) & 7), (int)((st >> 6) & 1),
                                          (int)((st >> 11) & 1), (int)((st >> 24) & 0x1f));
                        uno_dbg_net_trace("wifi: RXDATA DROP b0=%02x %02x %02x %02x %02x %02x %02x %02x",
                                          frame[hl],frame[hl+1],frame[hl+2],frame[hl+3],
                                          frame[hl+4],frame[hl+5],frame[hl+6],frame[hl+7]);
                    }
                }
                }
            }
        }
        off += (4 + plen + 0x3F) & ~0x3F;   /* FH_RSCSR_FRAME_ALIGN = 64 */
    }
}

/* Wait for a notification (group,cmd). Returns payload ptr + len, or NULL. */
static const u8 *wait_notif(int group, int cmd, int *out_len, int timeout_ms)
{
    int t;
    for (t = 0; t < timeout_ms; t++) {
        u16 closed = rx_closed() & (RXQ_N - 1);
        while (g_rx_read != closed) {
            const u8 *found = 0; int flen = 0;
            int vid = g_mq_rx ? (int)(((volatile u64 *)g_rbd_used)[g_rx_read] & 0xFFF) : (g_rx_read + 1);
            const u8 *rb = (vid >= 1 && vid <= RXQ_N) ? g_rb[vid-1] : g_rb[g_rx_read];
            rx_process_rb(rb, RB_SIZE, group, cmd, &found, &flen);
            g_rx_read = (g_rx_read + 1) & (RXQ_N - 1);
            if (found) { rx_restock(); if (out_len) *out_len = flen; return found; }
        }
        rx_restock();
        /* re-arm MSI-X RX vector 0 so the fw keeps delivering RBs (the working
         * Linux scan trace writes this after every RB; delivery is gated on it) */
        if (g_gen2) w32(CSR_MSIX_AUTOMASK_ST_AD, 1);
        /* FW error? */
        if (!g_gen2 && (r32(CSR_INT) & (CSR_INT_BIT_SW_ERR|CSR_INT_BIT_HW_ERR))) return 0;
        if ((t & 0x7f) == 0) prog_tick();           /* the scan dwell lives here */
        mdelay_(1);
    }
    return 0;
}

/* wait for a command response with matching sequence (or just any completion) */
static int wait_cmd_done(int timeout_ms)
{
    /* the fw acks a command via an RX packet echoing the sequence; for our
       purposes we just pump the RX ring so completions are drained. */
    int t;
    for (t = 0; t < timeout_ms; t++) {
        u16 closed = rx_closed() & (RXQ_N - 1);
        if (g_rx_read != closed) { g_rx_read = closed; rx_restock(); return 0; }
        mdelay_(1);
    }
    return -1;
}

/* Drain every RB the fw has closed, dispatching each frame (EAPOL -> the
 * supplicant, data -> g_dataq, mgmt -> mgmt_capture). This is the one RX pump:
 * wait_mgmt, iwl_recv, iwl_link and the eapol/data debug verbs all go through
 * it, so the ring is advanced and restocked identically everywhere. */
static void rx_pump_once(void)
{
    u16 closed = rx_closed() & (RXQ_N - 1);
    while (g_rx_read != closed) {
        const u8 *found = 0; int fl = 0;
        int vid = g_mq_rx ? (int)(((volatile u64 *)g_rbd_used)[g_rx_read] & 0xFFF) : (g_rx_read + 1);
        const u8 *rb = (vid >= 1 && vid <= RXQ_N) ? g_rb[vid-1] : g_rb[g_rx_read];
        rx_process_rb(rb, RB_SIZE, -1, -1, &found, &fl);
        g_rx_read = (g_rx_read + 1) & (RXQ_N - 1);
    }
    rx_restock();
    if (g_gen2) w32(CSR_MSIX_AUTOMASK_ST_AD, 1);
}

/* Pump for up to ms. `until_joined` stops early once the 4-way handshake has
 * installed keys. Returns the ms actually spent. */
static int rx_pump_ms(int ms, int until_joined)
{
    int t;
    for (t = 0; t < ms; t++) {
        rx_pump_once();
        if (until_joined && g_joined) return t;
        if ((t & 0xff) == 0) uno_dbg_heartbeat();   /* a long pump must not trip the wd */
        if ((t & 0x7f) == 0) prog_tick();           /* ~8 Hz: keep the caller's
                                                       indicator moving */
        mdelay_(1);
    }
    return ms;
}

/* =====================================================================
 * 7. Firmware load: gen1 legacy DMA, gen2 context-info, gen3 v2 + IML + PNVM
 * ===================================================================== */
/* ---- gen1: per-section chunk DMA over FH service channel 9 ---- */
#define FH_SRVC_CHNL 9
#define FH_SRVC_SRAM (FH_MEM + 0x9C8)
#define FH_TFDIB     (FH_MEM + 0x900)
#define FH_TCSR      (FH_MEM + 0xD00)
#define FH_TCSR_CFG(c)  (FH_TCSR + 0x20*(c))
#define FH_TCSR_BUFSTS(c) (FH_TCSR + 0x20*(c) + 0x8)
#define FH_TFDIB_C0(c)  (FH_TFDIB + 8*(c))
#define FH_TFDIB_C1(c)  (FH_TFDIB + 8*(c) + 4)
#define FH_SRVC_SRAM_ADDR(c) (FH_SRVC_SRAM + ((c)-9)*4)

static int load_section_gen1(u32 dst, const u8 *data, u32 len)
{
    u32 done = 0;
    while (done < len) {
        u32 chunk = len - done; if (chunk > 0x20000) chunk = 0x20000;
        u8 *bounce = arena_alloc(chunk);
        if (!bounce) return -1;
        memcpy(bounce, data + done, chunk);
        w32(CSR_INT, CSR_INT_BIT_FH_TX);          /* clear */
        w32(CSR_FH_INT_STATUS, CSR_FH_INT_TX_MASK);
        if (grab_nic() < 0) return -1;
        w32(FH_TCSR_CFG(FH_SRVC_CHNL), 0);         /* pause */
        w32(FH_SRVC_SRAM_ADDR(FH_SRVC_CHNL), dst + done);
        w32(FH_TFDIB_C0(FH_SRVC_CHNL), (u32)phys(bounce));
        w32(FH_TFDIB_C1(FH_SRVC_CHNL), (((u32)(phys(bounce)>>32) & 0xF) << 28) | chunk);
        w32(FH_TCSR_BUFSTS(FH_SRVC_CHNL), (1u<<20) | (1u<<12) | 0x3);
        w32(FH_TCSR_CFG(FH_SRVC_CHNL), 0x80000000 | 0x00000000 | 0x00100000);
        release_nic();
        /* wait for the chunk-done (FH_TX) */
        { int t; for (t=0;t<5000;t++){ if (r32(CSR_FH_INT_STATUS) & CSR_FH_INT_TX_MASK) break; if (r32(CSR_INT) & CSR_INT_BIT_FH_TX) break; mdelay_(1);} }
        done += chunk;
    }
    return 0;
}

static int load_fw_gen1(fw_sec *sec, int nsec)
{
    int i;
    for (i = 0; i < nsec; i++) {
        if (sec[i].offset == CPU_SEP || sec[i].offset == PAGE_SEP) continue;
        if (!sec[i].data) break;
        if (load_section_gen1(sec[i].offset, sec[i].data, sec[i].len) < 0) return -1;
    }
    /* release the CPU from reset -> firmware runs (7000 does this after load) */
    w32(CSR_INT, 0xFFFFFFFFu);
    w32(CSR_RESET, 0);
    return 0;
}

/* ---- gen2 context-info (22000 / AX200 / AX201) ---- */
struct ci_dram { u64 umac[64]; u64 lmac[64]; u64 vimg[64]; } __attribute__((packed));
struct context_info {
    u16 mac_id, version, size, rsv0;         /* version block (8) */
    u32 control_flags, ctl_rsv;              /* control (8) */
    u64 reserved0;                           /* 8 */
    u64 free_rbd, used_rbd, status_wr;       /* rbd_cfg (24) */
    u64 cmd_queue_addr; u8 cmd_queue_size; u8 hrsv[7];  /* hcmd_cfg (16) */
    u32 rsv1[4];                             /* 16 */
    u64 dump_addr; u32 dump_size, drsv;      /* dump_cfg (16) */
    u64 edbg_addr; u32 edbg_size, ersv;      /* early dbg (16) */
    u64 pnvm_addr; u32 pnvm_size, prsv;      /* pnvm_cfg (16) */
    u32 rsv2[16];                            /* 64 */
    struct ci_dram dram;                     /* 1536 */
    u32 rsv3[16];                            /* 64 */
} __attribute__((packed));

#define CTXT_TFD_FORMAT_LONG 0x0100
#define CTXT_RB_SIZE_4K      0x4
#define CTXT_RB_SIZE_POS     9
#define CTXT_RB_CB_SIZE_POS  4

static int count_sec(fw_sec *sec, int n, int start)
{
    int i, c = 0;
    for (i = start; i < n; i++) {
        if (sec[i].offset == CPU_SEP || sec[i].offset == PAGE_SEP || !sec[i].data) break;
        c++;
    }
    return c;
}

static void place_fw_dram(struct ci_dram *dram)
{
    int lmac = count_sec(g_fw.rt, g_fw.rt_n, 0);
    int umac = count_sec(g_fw.rt, g_fw.rt_n, lmac + 1);
    int pag  = count_sec(g_fw.rt, g_fw.rt_n, lmac + 1 + umac + 1);
    int i;
    for (i = 0; i < lmac && i < 64; i++) {
        void *p = arena_alloc(g_fw.rt[i].len);
        if (!p) return;
        memcpy(p, g_fw.rt[i].data, g_fw.rt[i].len);
        dram->lmac[i] = phys(p);
    }
    for (i = 0; i < umac && i < 64; i++) {
        fw_sec *s = &g_fw.rt[lmac + 1 + i];
        void *p = arena_alloc(s->len);
        if (!p) return;
        memcpy(p, s->data, s->len);
        dram->umac[i] = phys(p);
    }
    /* paging sections (after the PAGING separator) -> virtual_img. Linux
     * iwl_pcie_init_fw_sec places all THREE groups; we had left vimg[] zero,
     * and the AX201 images do carry paging sections the fw expects mapped. */
    for (i = 0; i < pag && i < 64; i++) {
        fw_sec *s = &g_fw.rt[lmac + 1 + umac + 1 + i];
        void *p = arena_alloc(s->len);
        if (!p) return;
        memcpy(p, s->data, s->len);
        dram->vimg[i] = phys(p);
    }
    uno_dbg_net_trace("wifi: fw dram map: lmac=%d umac=%d paging=%d sections", lmac, umac, pag);
}

/* diagnostics for the F12 autopsy: the context-info physaddr we kicked with,
 * and the first LMAC firmware section address the ROM will DMA. If the ROM
 * never starts, these tell us whether the kick address or the fw placement is
 * the problem (vs the register write path itself). */
static u64 g_ci_phys, g_ci_dram0;
static u32 g_fh_seen;      /* FH_INT bits latched in the ~200ms after the kick */

/* MSI-to-RAM probe (round 5): enable the PCI MSI capability with the message
 * address pointed at a RAM scratch dword. An MSI is just a DMA write - if the
 * device (boot ROM or fw) EVER tries to signal an interrupt, the 0x4D51 magic
 * lands in the scratch, giving us a visible device-initiated bus-master write.
 * It also makes the interrupt config coherent with UREG_CHICK=MSI: Linux
 * always has MSI or MSI-X enabled in config space before a load; we ran pure
 * INTx. Harmless in our polled world (the "interrupt" is a plain RAM write). */
static volatile u32 *g_msi_scratch;
static int pci_find_cap(const pci_dev *d, u8 want)
{
    int pos;
    if (!(pci_cfg_read16(d, 0x06) & 0x10)) return 0;    /* no cap list */
    pos = pci_cfg_read16(d, 0x34) & 0xFC;
    while (pos) {
        u16 hdr = pci_cfg_read16(d, pos);               /* id | next<<8 */
        if ((hdr & 0xFF) == want) return pos;
        pos = (hdr >> 8) & 0xFC;
    }
    return 0;
}
/* Enable MSI-X in PCI CONFIG space. The BAR0 register trace can't show this
 * (config-space writes aren't traced), but Linux's pci_alloc_irq_vectors puts
 * the function into MSI-X mode at the PCI level - and UREG_CHICK=MSIX is only
 * consistent if the PCI MSI-X Enable bit is also set. We poll and never take a
 * vector, so we set the Function-Mask bit too (all vectors masked, no MSI-X
 * table access needed) - the device is in MSI-X mode for the ROM's check
 * without us having to build a real table. */
static void msix_enable_pci(void)
{
    int pos = pci_find_cap(&g_pci, 0x11);      /* PCI_CAP_ID_MSIX */
    u16 ctl;
    if (!pos) { uno_dbg_net_trace("wifi: no MSI-X capability on the function"); return; }
    ctl = pci_cfg_read16(&g_pci, pos + 2);
    pci_cfg_write16(&g_pci, pos + 2, (u16)(ctl | 0x8000 /*enable*/ | 0x4000 /*func-mask*/));
    uno_dbg_net_trace("wifi: PCI MSI-X enabled: cap@%02x ctl %04x->%04x (tblsize=%d)",
                      pos, ctl, pci_cfg_read16(&g_pci, pos + 2), (ctl & 0x7ff) + 1);
}

/* Build a REAL MSI-X table, then LIFT the function mask.
 *
 * The mask above is safe but final in the wrong way: PCI Function Mask = 1
 * forbids the function from emitting ANY MSI-X message, ever.  We set it
 * because we poll and never take a vector - but the gen2 ctxt-info handshake
 * does not signal ALIVE through a register we can poll: the working ftrace of
 * this card delivers it as an MSI-X message (irq_msix entry:9, hw cause bit 0
 * = MSIX_HW_INT_CAUSES_REG_ALIVE).  A device that cannot send that message can
 * finish loading, start both CPUs, and then sit forever with nothing to do -
 * which is exactly the F12 signature, and why msi_scratch has read back
 * deadc0de (never written) in every round.
 *
 * We still do not want a real interrupt, so every vector is pointed at a host
 * RAM scratch dword instead of the LAPIC window (0xFEE00000): an MSI-X message
 * is just a posted memory write, so the device gets a completable signalling
 * path and we get visible proof it fired, without wiring an IDT vector.  This
 * is the same trick msi_probe_enable() uses for plain MSI on gen1.
 *
 * Per PCIe, each table entry powers up with Vector Control bit 0 (mask) SET,
 * so nothing can be emitted until we clear it here - programming the table
 * before dropping the function mask is the safe order. */
/* OFF unless armed with the "iwl msix" verb, then retried with "iwl rerun".
 * This is a device experiment, and an experiment must never be able to cost the
 * machine: run unconditionally from the boot bring-up it hung the Yoga hard
 * enough to need physical recovery (no URC, so no way back in). Anything that
 * pokes an unvalidated BAR window or lets the device DMA belongs behind a verb,
 * where a bad guess costs one reboot instead of a trip to the hardware. */
static int g_msix_arm;
static volatile u32 *g_msix_scratch;
#define IWL_BAR0_MIN 0x4000u              /* smallest BAR0 on any part we bind */
static void msix_table_setup(void)
{
    int pos = pci_find_cap(&g_pci, 0x11);
    u32 tbl, off;
    int bir, n, i;
    u64 pa;
    u16 ctl;
    if (!pos || !g_bar) return;
    ctl = pci_cfg_read16(&g_pci, pos + 2);
    n = (ctl & 0x7ff) + 1;
    tbl = pci_cfg_read32(&g_pci, pos + 4);
    bir = (int)(tbl & 7u);
    off = tbl & ~7u;
    if (bir != 0) {                       /* table lives in a BAR we have not mapped */
        uno_dbg_net_trace("wifi: MSI-X table in BAR%d (not BAR0) - left masked", bir);
        return;
    }
    /* pci_bar() hands back an address with no length, so we cannot ask how big
     * BAR0 is. Refuse anything that would not fit inside the smallest BAR0 we
     * ever bind rather than write blind into unmapped MMIO. */
    if (off + (u32)n * 16u > IWL_BAR0_MIN) {
        uno_dbg_net_trace("wifi: MSI-X table @+%05x x%d exceeds the assumed %05x "
                          "BAR0 - refusing to write it", off, n, IWL_BAR0_MIN);
        return;
    }
    g_msix_scratch = (volatile u32 *)arena_alloc(64);
    if (!g_msix_scratch) return;
    *g_msix_scratch = 0;
    pa = phys((const void *)g_msix_scratch);
    /* the device will DMA to this address; a bogus one corrupts host RAM */
    if (!pa || (pa & 3u) || (pa >> 32)) {
        uno_dbg_net_trace("wifi: MSI-X scratch phys %08x%08x unusable - refusing",
                          (u32)(pa >> 32), (u32)pa);
        return;
    }
    for (i = 0; i < n; i++) {
        volatile u32 *e = (volatile u32 *)(g_bar + off + (u32)i * 16);
        e[0] = (u32)pa;                   /* message address low  */
        e[1] = (u32)(pa >> 32);           /* message address high */
        e[2] = 0x4D510000u | (u32)i;      /* data: magic | vector */
        e[3] = 0;                         /* vector control: UNMASKED */
    }
    pci_cfg_write16(&g_pci, pos + 2, (u16)((ctl | 0x8000) & ~0x4000));
    uno_dbg_net_trace("wifi: MSI-X table armed: %d vectors @BAR0+%05x -> %08x%08x, "
                      "func-mask lifted (ctl now %04x)", n, off,
                      (u32)(pa >> 32), (u32)pa, pci_cfg_read16(&g_pci, pos + 2));
}

static void msi_probe_enable(void)
{
    int pos = pci_find_cap(&g_pci, 0x05);
    u16 ctl;
    u64 pa;
    if (!pos) { uno_dbg_net_trace("wifi: no MSI capability on the function"); return; }
    g_msi_scratch = (volatile u32 *)arena_alloc(64);
    if (!g_msi_scratch) return;
    *g_msi_scratch = 0;
    pa = phys((const void *)g_msi_scratch);
    ctl = pci_cfg_read16(&g_pci, pos + 2);
    pci_cfg_write32(&g_pci, pos + 4, (u32)pa);
    if (ctl & 0x80) {                                   /* 64-bit MSI */
        pci_cfg_write32(&g_pci, pos + 8, (u32)(pa >> 32));
        pci_cfg_write16(&g_pci, pos + 12, 0x4D51);
    } else {
        pci_cfg_write16(&g_pci, pos + 8, 0x4D51);
    }
    pci_cfg_write16(&g_pci, pos + 2, (u16)((ctl & ~0x70) | 1));  /* enable, 1 vector */
    uno_dbg_net_trace("wifi: MSI probe armed (cap@%02x ctl=%04x scratch=%08x%08x)",
                      pos, ctl, (u32)(pa >> 32), (u32)pa);
}

static int load_fw_gen2(void)
{
    struct context_info *ci = arena_alloc(sizeof *ci);
    if (!ci) return -1;
    memset(ci, 0, sizeof *ci);
    ci->version = 0;
    ci->mac_id = (u16)g_hw_rev;
    ci->size = sizeof(*ci) / 4;
    { u32 cb = RXQ_CB_SIZE; u32 cf = CTXT_TFD_FORMAT_LONG;
      cf |= (cb & 0xf) << CTXT_RB_CB_SIZE_POS;
      cf |= (CTXT_RB_SIZE_4K & 0xf) << CTXT_RB_SIZE_POS;
      ci->control_flags = cf; }
    ci->free_rbd = phys(g_rbd_free_le64);
    ci->used_rbd = phys(g_rbd_used);
    ci->status_wr = phys(&g_rbstts);
    ci->cmd_queue_addr = phys(g_cmd_ring);
    ci->cmd_queue_size = 2;                    /* TFD_QUEUE_CB_SIZE(32) = ilog2(32)-3 */
    place_fw_dram(&ci->dram);
    g_ci_phys = phys(ci); g_ci_dram0 = ci->dram.lmac[0];
    /* Match Linux iwl_pcie_ctxt_info_init() exactly:
     *   1. clear stale interrupts,
     *   2. ARM the FW-load interrupt mask (iwl_enable_fw_load_int_ctx_info) -
     *      the gen2 ROM's self-load handshake needs ALIVE|FH_RX unmasked or it
     *      never begins loading the ucode (the F12 "UCODE_LOAD_STATUS=0" case),
     *   3. kick with the 64-bit CSR_CTXT_INFO_BA write ALONE.
     * The old code skipped step 2 and added a spurious UREG_CPU_INIT_RUN write,
     * which is a gen3/IML register (used by load_fw_gen3, kicked via
     * CSR_CTXT_INFO_ADDR) - a no-op here at best. */
    w32(CSR_INT, 0xFFFFFFFFu);
    /* MSI-X fw-load interrupt enable (the working trace sets these masks in the
     * two writes immediately before the CTXT_INFO_BA kick): unmask ALIVE in the
     * HW mask + the FH causes. This is what the gen2 ROM waits on. conf_msix()
     * already put the device in MSI-X mode + programmed the IVAR table. */
    w32(CSR_MSIX_HW_INT_MASK_AD, MSIX_HW_MASK_FWLOAD);
    w32(CSR_MSIX_FH_INT_MASK_AD, MSIX_FH_MASK_FWLOAD);
    g_fh_seen = 0;
    /* OpenBSD iwx does the ENTIRE kick tail (BA write, LTR, doorbell) under
     * ONE nic lock; we used to grab/release around each PRPH write. Hold one
     * grab across the whole tail (inner prph_w calls nest via the refcount). */
    { int gk = grab_nic();
    w64_(CSR_CTXT_INFO_BA, g_ci_phys);
    /* Linux continues in the CALLER (iwl_trans_pcie_gen2_start_fw) after the
     * BA write - the BA write alone does NOT start the boot ROM:
     *   1. iwl_pcie_set_ltr(): boot-time LTR workaround. Integrated 22000
     *      (every CNVi AX201, incl. this fleet's Qu/QuZ) programs the HPM
     *      LTR PRPH pair; the discrete AX200 writes CSR_LTR_LONG_VAL_AD.
     *   2. UREG_CPU_INIT_RUN = 1 - THE ROM-START DOORBELL. The previous F12
     *      round removed this write as "spurious" because it is absent from
     *      iwl_pcie_ctxt_info_init(); it lives in the caller's tail. Without
     *      it UCODE_LOAD_STATUS stays 0 forever - the exact fleet signature.
     *      It is a PRPH write, so it also needs MAC access held to land
     *      (prph_w now grabs; the original write lacked this and read back 0). */
    if (g_devid == 0x2723) w32(CSR_LTR_LONG_VAL_AD, LTR_LONG_VAL_250US);
    else { prph_w(HPM_MAC_LTR_CSR, HPM_MAC_LRT_ENABLE_ALL);
           prph_w(HPM_UMAC_LTR, LTR_LONG_VAL_250US);
           /* decisive probe: if this reads back 0x88FA88FA the PRPH window
            * works and any remaining failure is past the doorbell; if it
            * reads 0 the MAC is still absorbing PRPH writes (power state). */
           uno_dbg_net_trace("wifi: prph window check: HPM_UMAC_LTR wrote %08x read %08x",
                             LTR_LONG_VAL_250US, prph_r(HPM_UMAC_LTR)); }
    /* kick: device self-loads. Instrumented (round 3): read the doorbell back
     * in the SAME MAC-access grab, then again 10 ms later - splits "the write
     * lands and the ROM consumes/clears it" (instant=1, later=0: doorbell OK,
     * dig into ctxt-info/fw validation) from "this register still refuses the
     * write" (instant=0: power/ownership path again). */
    { u32 v0, v1;
      prph_w(UREG_CPU_INIT_RUN, 1);
      v0 = prph_r(UREG_CPU_INIT_RUN);
      if (gk == 0) release_nic();     /* end of the one-grab kick tail */
      mdelay_(10);
      v1 = prph_r(UREG_CPU_INIT_RUN);
      uno_dbg_net_trace("wifi: doorbell CPU_INIT_RUN: instant=%08x +10ms=%08x "
                        "(register may be write-only - UREG_CHICK is the write-lands proof)", v0, v1); } }
    /* Latch FH_INT for ~200 ms right after the kick: if the ROM's DMA engine
     * runs at all with the now-<4GB arena, we catch it here even if it clears
     * before the ALIVE-timeout autopsy reads the register 2 s later. */
    { int t; for (t = 0; t < 200; t++) { g_fh_seen |= r32(CSR_FH_INT_STATUS); mdelay_(1); } }
    return 0;
}

/* ---- gen3 context-info-v2 + IML + PNVM (AX210) ---- */
struct prph_scratch {
    u16 mac_id, version, size, vrsv;           /* version */
    u32 control_flags, control_flags_ext;      /* control */
    u64 pnvm_base; u32 pnvm_size, prsv;        /* pnvm_cfg */
    u64 hwm_base; u32 hwm_size, dbg_tok;       /* hwm_cfg */
    u64 free_rbd; u32 rbdrsv;                  /* rbd_cfg (free only) */
    u64 rpwr_base; u32 rpwr_size, rprsv;       /* reduce power */
    u32 mbx0, mbx1;                            /* step */
    u32 fseq_override, step_analog;
    u32 rsv[8];
    struct ci_dram common; u64 fseq[8];        /* dram map */
} __attribute__((packed));
struct context_info_v2 {
    u16 version, size; u32 config;
    u64 prph_info_base;
    u64 cr_head, tr_tail, cr_tail, tr_head;
    u16 cr_idx_size, tr_idx_size;
    u64 mtr_base, mcr_base;
    u16 mtr_size, mcr_size;
    u16 mtr_dbv, mcr_dbv, mtr_msi, mcr_msi;
    u8  mtr_oh, mtr_of, mcr_oh, mcr_of;
    u16 msg_ctrl, prph_msi;
    u64 prph_scratch_base; u32 prph_scratch_size, rsv;
} __attribute__((packed));

#define PRPH_SCR_MTR_MODE  (1u<<17)
#define PRPH_MTR_FMT_256B  0xC0000

static int load_fw_gen3(void)
{
    struct prph_scratch *ps = arena_alloc(sizeof *ps);
    struct context_info_v2 *ci;
    u8 *prph_info, *iml;
    if (!ps) return -1;
    memset(ps, 0, sizeof *ps);
    ps->version = 0; ps->mac_id = (u16)g_hw_rev; ps->size = sizeof(*ps)/4;
    ps->control_flags = PRPH_SCR_MTR_MODE | PRPH_MTR_FMT_256B; /* + RB size 2K default */
    ps->free_rbd = phys(g_rbd_free_le64);
    place_fw_dram(&ps->common);
    /* PNVM: point the prph scratch at the blob NOW; the post-ALIVE doorbell
     * (iwl_nic) tells the fw to consume it. This was a dead if-block that
     * never programmed pnvm_base at all - on AX210+ the fw then refuses to
     * leave init, which reads exactly like the Latitude's ALIVE-era stall.
     * Best-effort caveat, stated honestly: g_pnvmbuf is the RAW .PNV TLV
     * stream; Linux parses out the sku-matched payload first. If the fw
     * rejects the raw form, the PNVM-complete trace below will say so. */
    if (g_pnvm_len) {
        ps->pnvm_base = phys(g_pnvmbuf);
        ps->pnvm_size = (u32)g_pnvm_len;
    }
    prph_info = arena_alloc(4096); if (!prph_info) return -1;
    memset(prph_info, 0, 4096);
    ci = arena_alloc(sizeof *ci); if (!ci) return -1;
    memset(ci, 0, sizeof *ci);
    ci->prph_info_base = phys(prph_info);
    ci->prph_scratch_base = phys(ps);
    ci->prph_scratch_size = sizeof(*ps)/4;
    ci->cr_head = phys(&g_rbstts);
    ci->tr_tail = phys(prph_info) + 2048;
    ci->cr_tail = phys(prph_info) + 3072;
    ci->mtr_base = phys(g_cmd_ring);
    ci->mcr_base = phys(g_rbd_used);
    ci->mtr_size = 2; ci->mcr_size = 8;
    iml = arena_alloc(g_fw.iml_len ? g_fw.iml_len : 4);
    if (g_fw.iml && g_fw.iml_len) memcpy(iml, g_fw.iml, g_fw.iml_len);
    w32(CSR_INT, 0xFFFFFFFFu);
    w32(CSR_INT_MASK, CSR_INT_FWLOAD_MASK);    /* gen3 init arms this too (iwl_enable_fw_load_int_ctx_info) */
    w64_(CSR_CTXT_INFO_ADDR, phys(ci));
    w64_(CSR_IML_DATA_ADDR, phys(iml));
    w32(CSR_IML_SIZE_ADDR, g_fw.iml_len);
    set_bit_(CSR_CTXT_INFO_BOOT_CTRL, CSR_AUTO_FUNC_BOOT_ENA);
    /* start_fw tail (same caller-side sequence the gen2 path was missing):
     * LTR, then the ROM-start doorbell. On AX210+ UREG_CPU_INIT_RUN sits at
     * +0x300000 (UMAC PRPH offset) - the old plain-address write hit a
     * different register, so the Latitude's ROM was never started either.
     * Discrete Ty (0x2725) takes the CSR LTR write; integrated So/Ma cannot
     * set LTR from the host (ROM bug), so clear the MSIX IML cause first and
     * poll it after the doorbell instead (iwl_pcie_spin_for_iml). BZ+ kicks
     * via FUNC_SCRATCH + GP_CNTRL ROM_START instead of the UREG doorbell. */
    if (g_devid == 0x2725) w32(CSR_LTR_LONG_VAL_AD, LTR_LONG_VAL_250US);
    else                   w32(CSR_MSIX_HW_INT_CAUSES_AD, MSIX_HW_IML);
    if (g_family >= FAM_BZ) {
        w32(CSR_FUNC_SCRATCH, CSR_FUNC_SCRATCH_INIT);
        set_bit_(CSR_GP_CNTRL, GP_CNTRL_ROM_START);
    } else {
        prph_w(uprph(UREG_CPU_INIT_RUN), 1);
    }
    if (g_devid != 0x2725) {                   /* spin for IML load completion */
        int t;
        for (t = 0; t < 100; t++) {
            if (r32(CSR_LTR_LAST_MSG) > 1) break;
            if (r32(CSR_MSIX_HW_INT_CAUSES_AD) & MSIX_HW_IML) break;
            mdelay_(1);
        }
        w32(CSR_MSIX_HW_INT_CAUSES_AD, MSIX_HW_IML);
    }
    return 0;
}

/* =====================================================================
 * 8. ALIVE + MVM init handshake
 * ===================================================================== */
static u32 g_lmac_err_ptr, g_umac_err_ptr;
static u32 g_sku_id[3];

/* Wait for the firmware to raise the ALIVE cause, in MSI-X terms.
 *
 * This is the F12 bug, and it was a measurement error, not a load failure.  In
 * MSI-X mode the device reports causes in CSR_MSIX_HW/FH_INT_CAUSES_AD; the
 * legacy CSR_INT stays 0 forever.  We polled CSR_INT and the RX ring, saw
 * zeroes, and concluded across seventeen rounds that "the firmware never
 * started" - while CSR_MSIX_HW_INT_CAUSES_AD bit 0 (ALIVE) had been set the
 * whole time.  Verified on metal: after a "failed" load it reads 0x00000001.
 *
 * Then the ordering matters.  The free-RBD write index starts at 0, so the fw
 * has no buffer to put the ALIVE *notification* in and cannot deliver it until
 * we open the ring - which is why the RBs stayed zeroed too.  Linux does
 * exactly this: take the ALIVE interrupt, then write RFH_Q0_FRBDCB_WIDX_TRG
 * (the ground-truth ftrace writes 0x7f8 = (2048-1) & ~7, matching our RXQ_N).
 * So: poll the cause, ack it, open the ring, and only then look for the
 * notification. */
/* out = pre + 8-hex-digit v + post; the verb path has no printf */
static void uno_snprintf_hex(char *out, int cap, const char *pre, u32 v, const char *post)
{
    static const char hx[] = "0123456789abcdef";
    int i = 0, k;
    while (*pre && i < cap - 12) out[i++] = *pre++;
    for (k = 0; k < 8 && i < cap - 2; k++) out[i++] = hx[(v >> ((7 - k) * 4)) & 0xF];
    while (*post && i < cap - 1) out[i++] = *post++;
    out[i] = 0;
}

static int wait_alive_cause(int timeout_ms)
{
    int t;
    for (t = 0; t < timeout_ms; t++) {
        u32 hw = r32(CSR_MSIX_HW_INT_CAUSES_AD);
        if (hw & MSIX_HW_ALIVE) {
            w32(CSR_MSIX_HW_INT_CAUSES_AD, hw);        /* write-1-to-clear */
            w32(CSR_MSIX_AUTOMASK_ST_AD, 1u << 9);     /* release the vector */
            uno_dbg_net_trace("wifi: ALIVE cause seen after %d ms (HW causes %08x)", t, hw);
            return 0;
        }
        mdelay_(1);
    }
    uno_dbg_net_trace("wifi: no ALIVE cause in %d ms (HW causes %08x FH %08x)",
                      timeout_ms, r32(CSR_MSIX_HW_INT_CAUSES_AD),
                      r32(CSR_MSIX_FH_INT_CAUSES_AD));
    return -1;
}

/* The ALIVE handshake, driven step by step from the "iwl alive <n>" verb.
 *
 * Why a verb and not wait_alive(): running this inline wedged the machine twice,
 * and URC log frames turn out to be flushed only when a command COMPLETES, so a
 * mid-command crash takes every trace line with it - breadcrumbs and flush
 * delays included (tested: not even the first line, emitted before anything
 * risky, survived).  There is therefore no way to locate an inline crash from
 * the log.  Behind a verb each step is a separate command that completes and
 * flushes on its own, and "iwl rerun" stays on the known-good path so the rig
 * is always usable.
 *
 *   iwl alive 1   take + ack the MSI-X ALIVE cause, release the automask
 *   iwl alive 2   ... and open the RX ring (fw starts consuming RBDs)
 *   iwl alive 3   ... and read the ALIVE notification out of the ring
 *
 * Steps 1 and 2 were each verified safe live via iwl csw before being coded
 * (opening the ring moved RFH_Q0_FRBDCB_RIDX 0 -> 0x20).  Step 3 is the one
 * that has never run against real firmware-written data. */
static int alive_steps(int upto, char *out, int cap)
{
    u32 hw, ridx;
    int len = 0;
    const u8 *p;
    if (!g_gen2) { strcpy(out, "err gen2 only"); return (int)strlen(out); }
    hw = r32(CSR_MSIX_HW_INT_CAUSES_AD);
    if (!(hw & MSIX_HW_ALIVE)) {
        uno_snprintf_hex(out, cap, "err no ALIVE cause (HW causes ", hw, ") - run 'iwl rerun' first");
        return (int)strlen(out);
    }
    w32(CSR_MSIX_HW_INT_CAUSES_AD, hw);          /* write-1-to-clear */
    w32(CSR_MSIX_AUTOMASK_ST_AD, 1u << 9);       /* release vector 9 */
    if (upto < 2) { strcpy(out, "ok step1: ALIVE cause acked, automask released"); return (int)strlen(out); }

    g_alive = 1;
    w32(RFH_Q0_FRBDCB_WIDX_TRG, g_rx_write);     /* open the RX ring */
    mdelay_(20);
    ridx = prph_r(RFH_Q0_FRBDCB_RIDX);
    if (upto < 3) {
        uno_snprintf_hex(out, cap, "ok step2: RX ring opened, RIDX=", ridx, " (nonzero = fw consuming RBDs)");
        return (int)strlen(out);
    }

    p = wait_notif(0, 0x1 /*UCODE_ALIVE_NTFY*/, &len, 1000);
    if (!p) { strcpy(out, "step3: ring open but NO ALIVE notification in 1 s"); return (int)strlen(out); }
    uno_snprintf_hex(out, cap, "ok step3: ALIVE NOTIFICATION READ, payload ", (u32)len, " bytes");
    return (int)strlen(out);
}

static int wait_alive(int timeout_ms)
{
    int len = 0;
    const u8 *p;
    /* gen2/3 MSI-X ALIVE handshake - proven end-to-end on metal (F12 fix).
     * The fw raises ALIVE in CSR_MSIX_HW_INT_CAUSES_AD (the legacy CSR_INT we
     * used to poll stays 0 in MSI-X mode), and it cannot deliver the ALIVE
     * *notification* until the RX ring is open - but the old flow opened the
     * ring only AFTER this returned, so it deadlocked and timed out.  Ack the
     * cause, release the automask, open the ring; then the wait_notif below
     * actually receives the 144-byte notification.  If the cause never comes we
     * fall through to the autopsy, which reports the MSI-X causes. */
    if (g_gen2 && wait_alive_cause(timeout_ms) == 0) {
        g_alive = 1;
        w32(RFH_Q0_FRBDCB_WIDX_TRG, g_rx_write);
        uno_dbg_net_trace("wifi: ALIVE cause acked, RX ring opened (WIDX_TRG=%x)", g_rx_write);
    }
    p = wait_notif(0, 0x1 /*UCODE_ALIVE_NTFY*/, &len, timeout_ms);
    if (!p) {
        /* F12 autopsy - every fleet machine timed out here, across gen2 AND
         * gen3 loads, which points at a COMMON host-side cause rather than
         * per-card firmware. Two questions, answered in order:
         *   1. Did the firmware boot at all?  CSR/PRPH state says.
         *   2. Did it boot and post ALIVE somewhere our closed-index poll
         *      never looks (rb-status DMA misconfig)?  Brute-scan every RB
         *      for the notification - and if it is there, TAKE it and
         *      continue: that is not a fallback hack, it is the datum that
         *      names the real bug AND unblocks the rest of bring-up. */
        int q;
        uno_dbg_net_trace("wifi: ALIVE timeout autopsy:");
        uno_dbg_net_trace("wifi:   MSIX_HW_CAUSES=%08x FH_CAUSES=%08x AUTOMASK=%08x "
                          "(HW bit0 = ALIVE; CSR_INT below is ALWAYS 0 in MSI-X mode)",
                          r32(CSR_MSIX_HW_INT_CAUSES_AD), r32(CSR_MSIX_FH_INT_CAUSES_AD),
                          r32(CSR_MSIX_AUTOMASK_ST_AD));
        uno_dbg_net_trace("wifi:   CSR_INT=%08x MASK=%08x GP_CNTRL=%08x RESET=%08x GP1=%08x",
                          r32(CSR_INT), r32(CSR_INT_MASK), r32(CSR_GP_CNTRL),
                          r32(CSR_RESET), r32(0x054 /*CSR_UCODE_DRV_GP1*/));
        uno_dbg_net_trace("wifi:   FH_INT=%08x  fh_after_kick=%08x  (any bit = the ROM's DMA engine ran)",
                          r32(CSR_FH_INT_STATUS), g_fh_seen);
        uno_dbg_net_trace("wifi:   UREG_UCODE_LOAD_STATUS=%08x UREG_CPU_INIT_RUN=%08x",
                          prph_r(uprph(UREG_UCODE_LOAD_STATUS)),
                          prph_r(uprph(UREG_CPU_INIT_RUN)));
        uno_dbg_net_trace("wifi:   HW_IF=%08x HPM_DEBUG=%08x HPM_HIPM=%08x HPM_UMAC_LTR=%08x grab_fail=%d",
                          r32(CSR_HW_IF_CONFIG_REG), prph_r(HPM_DEBUG),
                          prph_r(HPM_HIPM_GEN_CFG), prph_r(HPM_UMAC_LTR), g_grab_fail);
        uno_dbg_net_trace("wifi:   msix_scratch=%08x (4d51xxxx = the device DELIVERED an MSI-X message)",
                          g_msix_scratch ? *g_msix_scratch : 0xdeadc0deu);
        uno_dbg_net_trace("wifi:   msi_scratch=%08x (00004d51 = the device fired an interrupt)",
                          g_msi_scratch ? *g_msi_scratch : 0xDEADC0DE);
        /* Did the CSR_CTXT_INFO_BA kick even register? Read it back: if it does
         * not equal what we wrote, the CSR write path (not the fw) is the fault.
         * And confirm the fw sections were placed (dram0 != 0) - a zero there
         * means place_fw_dram never populated the image the ROM is meant to
         * DMA, which alone would leave UCODE_LOAD_STATUS=0. (g_gen2 only.) */
        if (g_gen2) {
            u64 ba = (u64)r32(CSR_CTXT_INFO_BA) | ((u64)r32(CSR_CTXT_INFO_BA + 4) << 32);
            uno_dbg_net_trace("wifi:   CTXT_INFO_BA readback=%08x%08x wrote=%08x%08x "
                              "fw_dram0=%08x%08x %s",
                              (u32)(ba >> 32), (u32)ba,
                              (u32)(g_ci_phys >> 32), (u32)g_ci_phys,
                              (u32)(g_ci_dram0 >> 32), (u32)g_ci_dram0,
                              ba == g_ci_phys ? "(kick stuck)" : "(KICK LOST - CSR write path!)");
            (void)ba;   /* uno_dbg_net_trace is a no-op in prod -> ba else unused */
        }
        uno_dbg_net_trace("wifi:   rb_status=%04x rx_read=%d used[0]=%08x%08x rb0[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x",
                          rx_closed(), g_rx_read,
                          (u32)(g_rbd_used[0] >> 32), (u32)g_rbd_used[0],
                          g_rb[0][0], g_rb[0][1], g_rb[0][2], g_rb[0][3],
                          g_rb[0][4], g_rb[0][5], g_rb[0][6], g_rb[0][7]);
        for (q = 0; q < RXQ_N && !p; q++) {
            const u8 *found = 0; int flen = 0;
            rx_process_rb(g_rb[q], RB_SIZE, 0, 0x1, &found, &flen);
            if (found) {
                uno_dbg_net_trace("wifi:   ALIVE FOUND by brute scan in RB %d "
                                  "(len %d) - fw BOOTED, the closed-index poll "
                                  "is what's broken (rb-status DMA). Proceeding.",
                                  q, flen);
                p = found; len = flen;
            }
        }
        if (!p) {
            uno_dbg_net_trace("wifi:   no ALIVE in any RB - the firmware never "
                              "started (or cannot DMA at all). Load-path issue, "
                              "not notification polling.");
            return -1;
        }
    }
    if (len < 4) return -1;
    { u16 status = (u16)(p[0] | (p[1]<<8));
      if (status != 0xCAFE) return -1; }
    if (g_fw.alive_notif_ver >= 6 && len >= 128) {
        /* v7: lmac_data[2]@4, umac_data@100, sku_id@116 */
        g_lmac_err_ptr = le32(p + 4 + 16);
        g_umac_err_ptr = le32(p + 100 + 8);
        g_sku_id[0]=le32(p+116); g_sku_id[1]=le32(p+120); g_sku_id[2]=le32(p+124);
    } else if (len >= 52) {
        g_lmac_err_ptr = le32(p + 4 + 16);
        g_umac_err_ptr = le32(p + 52 + 0);
    }
    return 0;
}

/* the scheduler bring-up after alive (gen1 only; gen2 fw configures it) */
#define SCD_BASE 0xa02c00
#define SCD_TXFACT (SCD_BASE + 0x10)
#define SCD_DRAM_BASE (SCD_BASE + 0x8)
#define FH_KW (FH_MEM + 0x97C)
#define FH_CBBC_0_15 (FH_MEM + 0x9D0)
static u8 g_kw[4096] __attribute__((aligned(4096)));
static void tx_start_gen1(void)
{
    if (grab_nic() < 0) return;
    prph_w(SCD_DRAM_BASE, (u32)(phys(g_cmd_bc) >> 10));
    w32(FH_CBBC_0_15 + 0*4, (u32)(phys(g_cmd_ring) >> 8));   /* cmd queue = 0 */
    w32(FH_KW, (u32)(phys(g_kw) >> 4));
    prph_w(SCD_TXFACT, 0xFF);      /* activate FIFOs 0..7 */
    release_nic();
}

/* =====================================================================
 * 9. MVM commands (structs from the fw/api headers) — the connect flow
 * ===================================================================== */
/* command group ids */
#define GRP_LEGACY 0
#define GRP_LONG   1
#define GRP_SYSTEM 2
#define GRP_MACCONF 3
#define GRP_DATAPATH 5
#define GRP_REGNVM 0xc

/* MAC_CONF-group opcodes.  0x8/0x9/0xa are the LINK-BASED ("MLD") association
 * API that superseded the legacy MAC_CONTEXT/BINDING/ADD_STA trio.  A working
 * Linux association trace of this exact QuZ-77 AX201 (2026-07-27) contains ONLY
 * these - zero LONG-group 0x28/0x2b/0x18 - which is why SESSION_PROTECTION used
 * to LMAC-FATAL here: it references a LINK that the legacy path never creates.
 * See WIFI-F12-HANDOFF.md round 22. */
#define MC_SESSION_PROT 0x05
#define MC_MAC_CONFIG   0x08
#define MC_LINK_CONFIG  0x09
#define MC_STA_CONFIG   0x0a
#define MC_STA_REMOVE   0x0c            /* STA_REMOVE_CMD, { u32 sta_id } */
#define DP_SEC_KEY      0x18            /* DATA_PATH SEC_KEY_CMD (MLD key path) */

#define FW_CTXT_INVALID 0xFFFFFFFFu
#define MLD_MAC_ID   0                  /* our single MAC context (raw id, no color) */
#define MLD_LINK_ID  0                  /* our single fw_link_id (driver-assigned) */

/* IWL_UCODE_TLV_CAPA_MLD_API_SUPPORT.  When the fw advertises it, iwlmvm drives
 * the whole mac/link/sta setup through MAC_CONF 0x8/0x9/0xa; when it does not,
 * the legacy MAC_CONTEXT_CMD path below is the only one that exists. */
static int fw_has_mld_api(void) { return fw_has_capa(110); }

/* small init commands */
static void mvm_tx_ant(u32 valid){ u32 c=valid; send_cmd(GRP_LONG,0x98,0,&c,4); wait_cmd_done(50);}
static void mvm_power_table(void){ u32 c=0; send_cmd(GRP_LONG,0x77,0,&c,4); wait_cmd_done(50);}

/* BT_CONFIG (LONG 0x9b, iwl_bt_coex_cmd, BT_COEX_CMD_API_S_VER_6 = 8 B).
 * iwl_mvm_up() ALWAYS sends this and we never did. It matters here more than on
 * a discrete card: the AX201 is CNVi, so WiFi and Bluetooth share one radio and
 * the LMAC scheduler has to arbitrate between them. Without a coex config,
 * bookkeeping commands are all fine but anything that asks the LMAC to reserve
 * real airtime has no arbitration policy to reserve it under. This ucode
 * advertises the command at ver 6 (LONG 0x9b in the cmd-version TLV). */
static void mvm_bt_init(void)
{
    struct { u32 mode, enabled_modules; } c;
    c.mode = 1;                         /* BT_COEX_NW */
    c.enabled_modules = (1u<<2)         /* SYNC2SCO (IWL_MVM_BT_COEX_SYNC2SCO) */
                      | (1u<<4);        /* HIGH_BAND_RET */
    if (fw_has_capa(67)) c.enabled_modules |= (1u<<0);   /* MPLUT, capa 67 */
    uno_dbg_net_trace("wifi: BT_CONFIG mode=%d modules=%02x len=%d",
                      (int)c.mode, (unsigned)c.enabled_modules, (int)sizeof c);
    send_cmd(GRP_LONG, 0x9b, 0, &c, (int)sizeof c);
    wait_cmd_done(100);
}

/* MCC_UPDATE_CMD (LONG 0xc8, iwl_mcc_update_cmd, LAR_UPDATE_MCC_CMD_API_S_VER_2
 * = 28 B). Sets the regulatory domain. iwl_mvm_init_mcc() sends it during
 * iwl_mvm_up(); we never did, so the fw has no regulatory state. Note a PASSIVE
 * scan is legal in any domain, which is why scanning works and proves nothing
 * about whether the fw will let us TRANSMIT on the channel we picked.
 * "ZZ" is the world-roaming domain - the same default iwl_mvm_init_mcc falls
 * back to when the BIOS/NVM gives no country. */
static void mvm_mcc_update(const char *cc)
{
    struct { u16 mcc; u8 source_id, rsv; u32 key; u8 rsv2[20]; } c;
    const u8 *r; int len = 0;
    memset(&c, 0, sizeof c);
    c.mcc = (u16)((cc[0] << 8) | cc[1]);
    /* MCC_SOURCE_GET_CURRENT (0x10) with "ZZ" is a QUERY - it asks the fw what
     * regulatory domain it already booted with, which is what iwl_mvm_init_mcc
     * does first. Metal, 2026-07-27: sending MCC_SOURCE_DEFAULT (7) instead got
     * status 4 = MCC_RESP_ILLEGAL. */
    c.source_id = 0x10;                 /* MCC_SOURCE_GET_CURRENT */
    uno_dbg_net_trace("wifi: MCC_UPDATE mcc=\"%c%c\" src=%02x len=%d",
                      cc[0], cc[1], c.source_id, (int)sizeof c);
    send_cmd(GRP_LONG, 0xc8, 0, &c, (int)sizeof c);
    r = wait_notif(GRP_LONG, 0xc8, &len, 200);
    /* iwl_mcc_update_resp_v3: le32 status, le16 mcc, u8 cap, u8 source_id,
     * le16 time, le16 geo_info, le32 n_channels. status/mcc offsets were wrong
     * in the first cut (status read as u16, mcc taken from its high half). */
    if (r && len >= 16)
        uno_dbg_net_trace("wifi: MCC_UPDATE resp: status=%d mcc=%c%c src=%d n_chan=%d",
                          (int)le32(r), r[5], r[4], r[7], (int)le32(r + 12));
    else
        uno_dbg_net_trace("wifi: MCC_UPDATE: short/no response (len=%d)", len);
}
static void mvm_dqa_enable(void){ u32 c=0; send_cmd(GRP_DATAPATH,0x0,0,&c,4); wait_cmd_done(50);}

/* INIT_EXTENDED_CFG (SYSTEM 0x3) + NVM_ACCESS_COMPLETE (REGNVM 0x0) — unified */
static void read_mac_addr(void);
static void mvm_init_unified(void)
{
    u32 init_flags = (1u<<1);                  /* BIT(IWL_INIT_NVM) */
    send_cmd(GRP_SYSTEM, 0x3, 0, &init_flags, 4); wait_cmd_done(100);
    { u32 z = 0; send_cmd(GRP_REGNVM, 0x0, 0, &z, 4); wait_cmd_done(100); }
    /* NO PHY_CONFIGURATION_CMD here.  For unified-ucode devices (family >=
     * 22000, which is every gen2 that reaches this path), Linux's
     * iwl_send_phy_cfg_cmd returns early and sends nothing - only the legacy
     * split-INIT ucode, or a tx_with_siso_diversity part, ever sends 0x6a.
     * The fw runs its default calibrations off NVM_ACCESS_COMPLETE instead.
     * Sending 0x6a here made the UMAC ADVANCED_SYSASSERT (error 0x201002fd,
     * last-cmd 0x016a) - proven live with the iwl fwerr verb 2026-07-25. */
    wait_notif(GRP_LEGACY, 0x4 /*INIT_COMPLETE_NOTIF*/, 0, 500);
    /* NVM_GET_INFO (kept for the sku/phy info it returns; the MAC address does
     * NOT come from here - Linux reads it from the CSR straps/OTP). */
    { u32 z = 0; int len=0; const u8 *r; send_cmd(GRP_REGNVM, 0x2, 0, &z, 4);
      r = wait_notif(GRP_REGNVM, 0x2, &len, 200); (void)r; (void)len; }
    read_mac_addr();
}

/* The whole post-ALIVE init, idempotent, so the boot path, the GUI scan path
 * and `iwl connect` all enter the join from the same fw state. */
static int g_mvm_done;
static int g_no_join;      /* radio_up(): bring the fw up but do not join */
static void mvm_init_all(void)
{
    if (g_mvm_done) return;
    if (!g_gen2) tx_start_gen1();
    mvm_init_unified();
    mvm_tx_ant(fw_valid_tx_ant());
    if (fw_has_capa(12)) mvm_dqa_enable();
    mvm_power_table();
    g_mvm_done = 1;
    uno_dbg_net_trace("wifi: MVM init sequence queued (nvm/phy/tx-ant/power)");
}

/* Read our own MAC address out of the card, mirroring
 * iwl_set_hw_address_from_csr(): try the OEM-fused STRAP pair first and fall
 * back to the OTP pair, with the same byte flip (iwl_flip_hw_address).
 *
 * The base is Linux's per-config `cfg->mac_addr_from_csr`, with OTP at +0/+4 and
 * STRAP at +8/+0xc. It is **0x380 for the AX200/AX201-class 22000 parts** and
 * 0x30380 only from AX210 on - keying it off "family >= 22000" reads an address
 * this device does not decode and returns 0xffffffff (metal, 2026-07-28).
 *
 * THIS WAS NEVER DONE. g_mac stayed all-zero, so every frame we transmitted
 * carried source address 00:00:00:00:00:00, MAC_CONFIG/LINK_CONFIG programmed
 * the card with a zero address, and mgmt_capture() compared each received
 * frame's addr1 against zeros - so an auth response could never match even if
 * the AP sent one. The AP still ACKs at the hardware level (an ACK is generated
 * for any unicast frame addressed to it, whatever the source), which is exactly
 * why metal showed TX_STATUS_SUCCESS and no reply. */
static void read_mac_addr(void)
{
    u32 base = (g_family >= FAM_AX210) ? 0x30380u : 0x380u;
    u32 a[2];
    int try_;
    for (try_ = 0; try_ < 2; try_++) {
        /* try 0 = STRAP (OEM fused), try 1 = OTP */
        u32 off = base + (try_ == 0 ? 8 : 0);
        const u8 *p0, *p1;
        a[0] = r32(off); a[1] = r32(off + 4);
        p0 = (const u8 *)&a[0]; p1 = (const u8 *)&a[1];
        g_mac[0] = p0[3]; g_mac[1] = p0[2]; g_mac[2] = p0[1]; g_mac[3] = p0[0];
        g_mac[4] = p1[1]; g_mac[5] = p1[0];
        /* valid = not multicast, not all-zero */
        if (!(g_mac[0] & 1) &&
            (g_mac[0]|g_mac[1]|g_mac[2]|g_mac[3]|g_mac[4]|g_mac[5])) {
            uno_dbg_net_trace("wifi: MAC %02x:%02x:%02x:%02x:%02x:%02x (from %s)",
                              g_mac[0],g_mac[1],g_mac[2],g_mac[3],g_mac[4],g_mac[5],
                              try_ == 0 ? "STRAP" : "OTP");
            return;
        }
    }
    uno_dbg_net_trace("wifi: MAC address NOT readable from CSR straps/OTP "
                      "(%08x %08x) - association cannot work", a[0], a[1]);
}

/* PHY_CONTEXT_CMD ADD on a 2.4GHz channel (v3+, 32 bytes) */
static void mvm_rlc_config(void);
static u32 g_phy_id = 0x0000;   /* id 0, color 0 */
static int g_phy_chan;          /* the channel that context is currently on */
static u32 g_mac_id = 0x0100;   /* id 1, color 1 (nonzero color) */
static void mvm_phy_ctxt(int chan, int action)
{
    struct { u32 id_color, action; u32 channel; u8 band,width,ctrl,rsv;
             u32 lmac_id, rxchain, dsp; u8 sec,r3[3]; } c;
    int hb = chan > 14;                 /* 5 GHz */
    if (action != 3 /*REMOVE*/) g_phy_chan = chan;
    memset(&c, 0, sizeof c);
    c.id_color = g_phy_id; c.action = action;
    c.channel = chan;
    c.band = hb ? 0 /*PHY_BAND_5*/ : 1 /*PHY_BAND_24*/;
    c.width = 0 /*20MHz*/;
    c.lmac_id = hb ? 1 /*IWL_LMAC_5G_INDEX*/ : 0 /*IWL_LMAC_24G_INDEX*/;
    /* iwl_mvm_phy_ctxt_cmd_data: valid_rx_ant << VALID_POS(1) |
     * idle_cnt << CNT_POS(10) | active_cnt << MIMO_CNT_POS(12). We were sending
     * valid=1 and NO mimo_cnt; on this 2x2 part Linux sends valid=3, cnt=1,
     * mimo_cnt=1 (0x1406). */
    c.rxchain = (fw_valid_rx_ant() << 1) | (1u << 10) | (1u << 12);
    uno_dbg_net_trace("wifi: PHY_CONTEXT action=%d chan=%d band=%s lmac=%d rxchain=%08x len=%d",
                      action, chan, hb ? "5" : "2.4", (int)c.lmac_id, c.rxchain, (int)sizeof c);
    send_cmd(GRP_LONG, 0x8, 0, &c, sizeof c); wait_cmd_done(100);
    if (action != 3 /*REMOVE*/) mvm_rlc_config();
}

/* RLC_CONFIG_CMD (DATA_PATH 0x08, iwl_rlc_config_cmd, 32 B, sent at cmd
 * version 2 in the wide header).  Binds the radio-chain configuration to the
 * PHY context - the layer between "a PHY context exists" and "the LMAC can
 * actually schedule on it".
 *
 * The working Linux association trace of this exact fw sends 0x05.0x08
 * IMMEDIATELY after every 0x01.0x08 (PHY_CONTEXT_CMD), twice.  Note that
 * v6.7's iwl_mvm_phy_send_rlc gates this on the fw advertising DATA_PATH 0x08
 * at cmd_ver >= 2, and this ucode's version TLV does NOT list it - yet the
 * trace shows the working driver sending it anyway.  Ground truth from the
 * device wins over the version gate of one kernel release. */
static void mvm_rlc_config(void)
{
    struct __attribute__((packed)) {
        u32 phy_id;
        u32 rx_chain_info, rlc_reserved;          /* iwl_rlc_properties */
        u32 sad_chain_a, sad_chain_b, sad_mac_id, sad_reserved;
        u8  flags, reserved[3];
    } c;
    memset(&c, 0, sizeof c);
    c.phy_id = g_phy_id;                /* the RAW phy id, as in ctxt->id */
    /* same encoding as the PHY context's rxchain_info; the BUILD_BUG_ONs in
     * iwl_mvm_phy_send_rlc assert the two field layouts are identical */
    c.rx_chain_info = (fw_valid_rx_ant() << 1) | (1u << 10) | (1u << 12);
    uno_dbg_net_trace("wifi: RLC_CONFIG phy=%d rx_chain_info=%08x len=%d",
                      (int)c.phy_id, c.rx_chain_info, (int)sizeof c);
    send_cmd(GRP_DATAPATH, 0x8, 2 /*cmd ver 2*/, &c, (int)sizeof c);
    wait_cmd_done(100);
}

/* MAC_CONTEXT_CMD (BSS STA). Big struct; we fill the common + sta tail. */
static void mvm_mac_ctxt(const u8 bssid[6], int assoc, int aid, int action)
{
    /* iwl_mac_ctx_cmd (MAC_CONTEXT_CMD_API_S_VER_1), STA union, 144 B.
       The fw ADVANCED_SYSASSERTs on this cmd (fwerr cmd=0x0128, 2026-07-25)
       if the required rate + QoS-fifo fields are left zero.
       iwl_mvm_mac_ctxt_cmd_common fills cck/ofdm ack rates and per-AC
       fifos_mask; we mirror the minimum it needs. */
    u8 c[148]; u8 *p = c;
    static const u16 cwmin[4] = { 15, 15, 7, 3 };     /* ucode AC order BK,BE,VI,VO */
    static const u16 cwmax[4] = { 1023, 1023, 15, 7 };
    static const u8  aifs[4]  = { 7, 3, 2, 2 };
    static const u8  fifo[4]  = { 2, 4, 8, 16 };      /* BIT(gen2 EDCA fifo) per AC */
    int a;
    memset(c, 0, sizeof c);
    *(u32*)(p+0)  = g_mac_id;           /* id_and_color */
    *(u32*)(p+4)  = action;
    *(u32*)(p+8)  = 5;                  /* FW_MAC_TYPE_BSS_STA */
    *(u32*)(p+12) = 0;                  /* tsf_id */
    memcpy(p+16, g_mac, 6);             /* node_addr */
    memcpy(p+24, bssid, 6);             /* bssid_addr */
    *(u32*)(p+32) = 0x0f;               /* cck_rates  1/2/5.5/11 */
    *(u32*)(p+36) = 0x15;               /* ofdm_rates 6/12/24 (mandatory) */
    *(u32*)(p+40) = 0;                  /* protection_flags */
    /* cck_short_preamble@44, short_slot@48 = 0 */
    { u32 filt = (1u<<2);              /* ACCEPT_GRP */
      if (!assoc) filt |= (1u<<6);      /* IN_BEACON while connecting */
      *(u32*)(p+52) = filt; }           /* filter_flags */
    *(u32*)(p+56) = 1;                  /* qos_flags = MAC_QOS_FLG_UPDATE_EDCA */
    /* ac[5], each iwl_ac_qos = { le16 cw_min, le16 cw_max, u8 aifsn,
       u8 fifos_mask, le16 edca_txop } @60; entry 4 (index 4) stays zero */
    for (a = 0; a < 4; a++) {
        u8 *q = p + 60 + a*8;
        q[0]=(u8)cwmin[a]; q[1]=(u8)(cwmin[a]>>8);
        q[2]=(u8)cwmax[a]; q[3]=(u8)(cwmax[a]>>8);
        q[4]=aifs[a]; q[5]=fifo[a];
    }
    /* mac_data_sta union @100: is_assoc@100 ... assoc_id@136 */
    *(u32*)(p+100) = assoc ? 1 : 0;
    *(u32*)(p+136) = aid;
    /* 148 = sizeof(iwl_mac_ctx_cmd): 100 common + 48 for the union's largest
       member iwl_mac_data_p2p_sta (sta+ctwin). The fw length-checks against that
       union-max size and ADVANCED_SYSASSERTs if the cmd is shorter - sending the
       144 the STA member needs was 4 B short (fwerr data2=0x94=148 expected). */
    send_cmd(GRP_LONG, 0x28, 0, c, 148); wait_cmd_done(100);
}

/* BINDING_CONTEXT_CMD (v2, 28 bytes) + TIME_QUOTA_CMD */
static u32 g_binding_id = 0x0000;
static void mvm_binding(int action)
{
    struct { u32 id_color, action, macs[3], phy, lmac_id; } c;
    memset(&c, 0, sizeof c);
    c.id_color = g_binding_id; c.action = action;
    c.macs[0] = g_mac_id; c.macs[1] = 0xFFFFFFFF; c.macs[2] = 0xFFFFFFFF;
    c.phy = g_phy_id; c.lmac_id = 0;
    send_cmd(GRP_LONG, 0x2b, 0, &c, sizeof c); wait_cmd_done(100);
}
static void mvm_time_quota(void)
{
    /* iwl_time_quota_cmd v2: quotas[MAX_BINDINGS=4], each 16 B. Unused entries
       MUST be FW_CTXT_INVALID (0xffffffff) - zero reads as binding 0/color 0
       (our real binding), and the duplicate made the fw ADVANCED_SYSASSERT
       (0x201002ff, 2026-07-25). Mirrors iwl_mvm_update_quotas (mvm/quota.c). */
    struct { u32 id_color, quota, max_dur, low_lat; } q[4];
    int i;
    memset(q, 0, sizeof q);
    for (i = 0; i < 4; i++) q[i].id_color = 0xFFFFFFFF;   /* FW_CTXT_INVALID */
    q[0].id_color = g_binding_id; q[0].quota = 128 /*IWL_MVM_MAX_QUOTA*/; q[0].max_dur = 0;
    send_cmd(GRP_LONG, 0x2c, 0, q, sizeof q); wait_cmd_done(100);
}

/* SESSION_PROTECTION_CMD (MAC_CONF 0x5) or TIME_EVENT_CMD — assoc window */
/* TIME_EVENT_CMD (0x29) reserving an association window - the airtime path for
 * fw that LMAC-FATALs on SESSION_PROTECTION_CMD. Field values mirror
 * iwl_mvm_protect_session: id=TE_BSS_STA_AGGRESSIVE_ASSOC(0), interval=1,
 * policy = HOST_EVENT START|END | START_IMMEDIATELY. Waits for
 * TIME_EVENT_NOTIFICATION (LEGACY 0x2a) = the window has begun. */
static void mvm_te_assoc(void) __attribute__((unused));
static void mvm_te_assoc(void)
{
    struct __attribute__((packed)) {
        u32 id_color, action, id, apply_time, max_delay, depends, interval, duration;
        u8 repeat, max_frags; u16 policy;
    } c;
    memset(&c, 0, sizeof c);
    c.id_color = g_mac_id;                 /* FW_CMD_ID_AND_COLOR(mac id, color) */
    c.action = 1;                          /* FW_CTXT_ACTION_ADD */
    c.id = 0;                              /* TE_BSS_STA_AGGRESSIVE_ASSOC */
    c.interval = 1;
    c.duration = 900;                      /* TU */
    c.repeat = 1;
    c.max_frags = 0;                       /* TE_V2_FRAG_NONE */
    c.policy = (1u<<0)|(1u<<1)|(1u<<11);   /* NOTIF START|END | START_IMMEDIATELY */
    send_cmd(GRP_LONG, 0x29, 0, &c, (int)sizeof c);
    wait_notif(GRP_LEGACY, 0x2a, 0, 500);
}

static void mvm_assoc_window(void)
{
    if (fw_has_capa(54)) {
        /* iwl_session_prot_cmd (SESSION_PROTECTION_CMD_API_S_VER_1/2) = 6 u32
         * (24 B): id_and_color, action, conf_id, duration_tu, repetition_count,
         * interval. This fw LENGTH-asserts: a 20-byte send gave UMAC
         * ADVANCED_SYSASSERT on cmd 0x0305 with data2=0x18 (expected 24) /
         * data3=0x14 (got 20).
         *
         * id_and_color per iwl_mvm_get_session_prot_id: the raw mvmvif->id for
         * SESSION_PROTECTION_CMD ver < 2, the **fw_link_id** from ver 2 up. This
         * ucode does not list MAC_CONF 0x5 in its cmd-version TLV, so it is ver 1
         * -> the mac id. Both are 0 in our single-mac/single-link setup, which is
         * why the LMAC-FATAL (data1=0x400) was never about this field: the id was
         * fine, the LINK it implies simply did not exist because the join used the
         * legacy MAC_CONTEXT path. mld_link_cfg() now creates it. */
        struct { u32 id_color, action, conf_id, duration_tu, repetition_count, interval; } c;
        memset(&c,0,sizeof c);
        c.id_color = fw_has_mld_api() ? MLD_LINK_ID : MLD_MAC_ID;
        c.action = 1;                   /* FW_CTXT_ACTION_ADD */
        c.conf_id = 0;                  /* SESSION_PROTECT_CONF_ASSOC */
        c.duration_tu = 900;            /* repetition_count + interval stay 0 */
        uno_dbg_net_trace("wifi: session-prot: MAC_CONF 0x5 len=%d capa54=%d id=%d mld=%d",
                          (int)sizeof c, fw_has_capa(54), (int)c.id_color, fw_has_mld_api());
        send_cmd(GRP_MACCONF, MC_SESSION_PROT, 0, &c, (int)sizeof c);
        wait_notif(GRP_MACCONF, 0xFB, 0, 500);   /* SESSION_PROTECTION_NOTIF start */
    } else {
        struct { u32 id_color, action, id, apply, max_delay, depends, interval, duration; u8 repeat, max_frags; u16 policy; } c;
        memset(&c,0,sizeof c);
        c.id_color = g_mac_id; c.action = 1; c.id = 1 /*TE_BSS_STA_ASSOC*/;
        c.duration = 900; c.repeat = 1; c.policy = (1u<<0)|(1u<<11); /* notif + start now */
        send_cmd(GRP_LONG, 0x29, 0, &c, sizeof c);
        wait_notif(GRP_LEGACY, 0x2a, 0, 500);    /* TIME_EVENT_NOTIFICATION */
    }
}

/* ADD_STA (v7 front covers both; 44 bytes) */
static void mvm_add_sta(const u8 addr[6], int modify, u32 sta_flags)
{
    u8 c[48];
    memset(c, 0, sizeof c);
    c[0] = modify ? 1 : 0;             /* add_modify */
    *(u32*)(c+4) = g_mac_id;           /* mac_id_n_color */
    memcpy(c+8, addr, 6);
    c[16] = AP_STA_ID;                 /* sta_id */
    *(u32*)(c+20) = sta_flags;
    *(u32*)(c+24) = 0xFFFFFFFF;        /* station_flags_msk */
    c[35] = 0;                         /* station_type = IWL_STA_LINK */
    *(u32*)(c+40) = 0;              /* tfd_queue_msk obsolete for new-tx-api */
    send_cmd(GRP_LONG, 0x18, 0, c, g_family >= FAM_9000 ? 48 : 44);
    wait_cmd_done(100);
}

/* ADD_STA_KEY: install a CCMP key (pairwise or, with mcast=1, the GTK) */
static void mvm_add_sta_key(const u8 *key, int keylen, int keyidx, int mcast, const u8 *pn)
{
    u8 c[76];
    memset(c, 0, sizeof c);
    c[0] = AP_STA_ID;                  /* sta_id */
    c[1] = mcast ? 1 : 0;              /* key_offset */
    { u16 kf = (u16)((keyidx<<8) | (1u<<3) /*WEP_KEY_MAP*/ | 2 /*CCM*/);
      if (mcast) kf |= (1u<<14);       /* STA_KEY_MULTICAST */
      c[2]=(u8)kf; c[3]=(u8)(kf>>8); }
    memcpy(c+4, key, keylen < 32 ? keylen : 32);
    if (pn) memcpy(c+36, pn, 6);       /* rx_secur_seq_cnt (RSC) */
    /* v2+ struct: transmit_seq_cnt @68 (leave 0 for RX-side install) */
    send_cmd(GRP_LONG, 0x17, 0, c, 76);
    wait_cmd_done(100);
}

/* =====================================================================
 * 9b. The LINK-BASED (MLD) association API — MAC_CONF group 0x03
 *
 * Structs mirror Linux `fw/api/mac-cfg.h` at the command versions this ucode
 * advertises (tools/iwl_cmd_versions.py against IWLAX201.UCO):
 *   MAC_CONF 0x08 MAC_CONFIG_CMD  -> ver 1  (iwl_mac_config_cmd, 52 B)
 *   MAC_CONF 0x09 LINK_CONFIG_CMD -> absent from the table = default ver 1
 *                                    (iwl_link_config_cmd, 208 B)
 *   MAC_CONF 0x0a STA_CONFIG_CMD  -> absent = default ver 1
 *                                    (iwl_mvm_sta_cfg_cmd, 96 B)
 * VER_1 layouts are the ones in kernel v6.7; later kernels only append/repurpose
 * tail fields for _VER_2+. The compile-time size asserts below are the guard:
 * this fw length-checks every command and ADVANCED_SYSASSERTs on a mismatch
 * (that is exactly how the 20-vs-24-byte SESSION_PROTECTION bug surfaced).
 *
 * Flow (iwlmvm mld-mac.c / link.c / mld-sta.c):
 *   MAC_CONFIG ADD -> LINK_CONFIG ADD (phy invalid) -> PHY_CONTEXT ADD ->
 *   LINK_CONFIG MODIFY(active, phy, rates, qos, beacon timing) ->
 *   STA_CONFIG (the AP peer) -> SESSION_PROTECTION -> auth/assoc.
 * There is NO binding and NO time quota in this API: the link replaces both.
 * ===================================================================== */

/* iwl_ac_qos, 8 B — same shape the legacy MAC_CONTEXT_CMD uses */
struct mld_ac_qos { u16 cw_min, cw_max; u8 aifsn, fifos_mask; u16 edca_txop; } __attribute__((packed));
/* iwl_he_backoff_conf, 8 B (MU-EDCA; left zero, we advertise no HE) */
struct mld_he_backoff { u16 cwmin, cwmax, aifsn, mu_time; } __attribute__((packed));

struct mld_mac_cmd {
    u32 id_and_color, action, mac_type;
    u8  local_mld_addr[6]; u16 rsv_mld_addr;
    u32 filter_flags;
    u16 he_support, he_ap_support; u32 eht_support;
    u32 nic_not_ack_enabled;
    /* union iwl_mac_client_data (the largest member for a STA mac) */
    u8  is_assoc, esr_transition_timeout; u16 medium_sync_delay;
    u16 assoc_id, rsv1, data_policy, rsv2;
    u32 ctwin;
} __attribute__((packed));
typedef char _mld_mac_sz[(sizeof(struct mld_mac_cmd) == 52) ? 1 : -1];

struct mld_link_cmd {
    u32 action, link_id, mac_id, phy_id;
    u8  local_link_addr[6]; u16 rsv_link_addr;
    u32 modify_mask, active, listen_lmac;
    u32 cck_rates, ofdm_rates, cck_short_preamble, short_slot, protection_flags;
    u32 qos_flags;
    struct mld_ac_qos ac[5];
    u8  htc_trig_based_pkt_ext, rand_alloc_ecwmin, rand_alloc_ecwmax, ndp_fdbk_buff_th_exp;
    struct mld_he_backoff trig_based_txf[4];
    u32 bi, dtim_interval;
    u16 puncture_mask, frame_time_rts_th;
    u32 flags, flags_mask;
    u8  ref_bssid_addr[6]; u16 rsv_ref_bssid;
    u8  bssid_index, bss_color, spec_link_id, rsv;
    u8  ibss_bssid_addr[6]; u16 rsv_ibss_bssid;
    u32 rsv_tail[8];
} __attribute__((packed));
typedef char _mld_link_sz[(sizeof(struct mld_link_cmd) == 208) ? 1 : -1];

struct mld_sta_cmd {
    u32 sta_id, link_id;
    u8  peer_mld_address[6]; u16 rsv_mld_addr;
    u8  peer_link_address[6]; u16 rsv_link_addr;
    u32 station_type, assoc_id, beamform_flags, mfp, mimo, mimo_protection,
        ack_enabled, trig_rnd_alloc, tx_ampdu_spacing, tx_ampdu_max_size,
        sp_length, uapsd_acs;
    u8  pkt_ext[20];                    /* iwl_he_pkt_ext_v2 (2 Nss x 5 BW x 2) */
    u32 htc_flags;
} __attribute__((packed));
typedef char _mld_sta_sz[(sizeof(struct mld_sta_cmd) == 96) ? 1 : -1];

/* LINK_CONTEXT_MODIFY_* (enum iwl_link_ctx_modify_flags) */
#define LINK_MOD_ACTIVE        (1u<<0)
#define LINK_MOD_RATES_INFO    (1u<<1)
#define LINK_MOD_PROTECT_FLAGS (1u<<2)
#define LINK_MOD_QOS_PARAMS    (1u<<3)
#define LINK_MOD_BEACON_TIMING (1u<<4)

/* Beacon timing learned from the picked AP's beacon (defaults if it had none) */
static u16 g_join_bi = 100;             /* beacon interval, TU */
static u8  g_join_dtim = 1;             /* DTIM period */

/* MAC_CONFIG_CMD (MAC_CONF 0x8) — creates/updates the MAC context. Replaces the
 * legacy MAC_CONTEXT_CMD (LONG 0x28). action: 1=ADD 2=MODIFY 3=REMOVE. */
static void mld_mac_cfg(int action, int assoc, int aid)
{
    struct mld_mac_cmd c;
    memset(&c, 0, sizeof c);
    c.id_and_color = MLD_MAC_ID;        /* raw mvmvif->id, no color in this API */
    c.action = (u32)action;
    c.mac_type = 5;                     /* FW_MAC_TYPE_BSS_STA */
    memcpy(c.local_mld_addr, g_mac, 6);
    /* enum iwl_mac_config_filter_flags — NOTE these are NOT the legacy
     * MAC_FILTER_* bits: ACCEPT_GRP is BIT(2) in both, but "hear beacons" is
     * ACCEPT_BEACON BIT(3) here, where the legacy cmd used IN_BEACON BIT(6). */
    c.filter_flags = (1u<<2);                       /* ACCEPT_GRP */
    if (!assoc) c.filter_flags |= (1u<<3);          /* ACCEPT_BEACON while connecting */
    /* he_support/eht_support stay 0: we associate as a plain HT-less STA.
     * nic_not_ack_enabled = !iwl_mvm_is_nic_ack_enabled(); this part reports
     * HE MAC CAP2 ACK_EN, so the working driver sends 0. */
    c.nic_not_ack_enabled = 0;
    c.is_assoc = assoc ? 1 : 0;
    c.assoc_id = (u16)aid;
    uno_dbg_net_trace("wifi: MAC_CONFIG action=%d assoc=%d aid=%d len=%d",
                      action, assoc, aid, (int)sizeof c);
    send_cmd(GRP_MACCONF, MC_MAC_CONFIG, 0, &c, (int)sizeof c);
    wait_cmd_done(100);
}

/* LINK_CONFIG_CMD (MAC_CONF 0x9) — creates the link the rest of the association
 * hangs off (SESSION_PROTECTION's id, STA_CONFIG's link_id). Replaces the legacy
 * BINDING_CONTEXT_CMD + TIME_QUOTA_CMD pair.
 *
 * Linux picks fw_link_id itself (ffz over a driver-side bitmap in
 * iwl_mvm_get_free_fw_link_id) — the fw does NOT hand one back — so with a
 * single link ours is always MLD_LINK_ID (0).
 *
 * ADD is sent with phy_id = FW_CTXT_INVALID and no rates, exactly as
 * iwl_mvm_add_link does.
 *
 * `modify_mask` is the caller's, and it MATTERS: the fw validates exactly the
 * field groups the mask selects, and ADVANCED_SYSASSERTs (2010330f on cmd
 * 0x0309, data2/3 = deadbeef, i.e. a content check rather than a length one) on
 * groups it does not expect yet. Metal, 2026-07-27: activating with
 * ACTIVE|RATES|PROTECT|QOS|BEACON_TIMING asserts; Linux activates with
 * **ACTIVE | RATES_INFO only** (iwl_mvm_mld_assign_vif_chanctx), adds
 * QOS_PARAMS later and only once associated and the AP advertises WMM
 * (BSS_CHANGED_QOS), and in the STA path never sets BEACON_TIMING at all. So
 * mirror that: fill a field group only when its mask bit is set. */
static void mld_link_cfg(int action, int active, int have_phy, u32 modify_mask)
{
    struct mld_link_cmd c;
    static const u16 cwmin[4] = { 15, 15, 7, 3 };    /* ucode AC order BK,BE,VI,VO */
    static const u16 cwmax[4] = { 1023, 1023, 15, 7 };
    static const u8  aifs[4]  = { 7, 3, 2, 2 };
    static const u8  fifo[4]  = { 2, 4, 8, 16 };     /* BIT(gen2 EDCA fifo) per AC:
                                                        BK=1 BE=2 VI=3 VO=4 */
    int a;
    memset(&c, 0, sizeof c);
    c.action = (u32)action;
    c.link_id = MLD_LINK_ID;
    c.mac_id = MLD_MAC_ID;
    c.phy_id = have_phy ? g_phy_id : FW_CTXT_INVALID;
    memcpy(c.local_link_addr, g_mac, 6);
    c.listen_lmac = 0;
    c.spec_link_id = 0;                 /* the 802.11 link id (non-MLO: 0) */
    if (action == 2 /*MODIFY*/) {
        c.active = active ? 1 : 0;
        c.modify_mask = modify_mask;
        /* iwl_mvm_link_changed fills bi unconditionally, but dtim_interval is
         * beacon_int * dtim_period and dtim_period stays 0 until a beacon has
         * been heard *after* association - so pre-assoc it really does send 0. */
        c.bi = g_join_bi;
        if (modify_mask & LINK_MOD_RATES_INFO) {
            /* basic/ACK rates: CCK 1/2/5.5/11 and the mandatory OFDM set - the
             * same values the legacy mac_ctxt needed to avoid an assert */
            c.cck_rates = 0x0f;
            c.ofdm_rates = 0x15;
        }
        if (modify_mask & LINK_MOD_QOS_PARAMS) {
            c.qos_flags = 1;            /* MAC_QOS_FLG_UPDATE_EDCA */
            for (a = 0; a < 4; a++) {
                c.ac[a].cw_min = cwmin[a]; c.ac[a].cw_max = cwmax[a];
                c.ac[a].aifsn = aifs[a];   c.ac[a].fifos_mask = fifo[a];
            }
        }
        if (modify_mask & LINK_MOD_BEACON_TIMING)
            c.dtim_interval = (u32)g_join_bi * (g_join_dtim ? g_join_dtim : 1);
    }
    uno_dbg_net_trace("wifi: LINK_CONFIG action=%d active=%d phy=%s mask=%02x "
                      "bi=%d dtim_int=%d len=%d", action, active,
                      have_phy ? "valid" : "INVALID", (unsigned)c.modify_mask,
                      (int)c.bi, (int)c.dtim_interval, (int)sizeof c);
    send_cmd(GRP_MACCONF, MC_LINK_CONFIG, 0, &c, (int)sizeof c);
    wait_cmd_done(100);
}

/* STA_CONFIG_CMD (MAC_CONF 0xa) — the AP peer station. Replaces ADD_STA
 * (LONG 0x18); there is no add/modify flag, re-sending the command updates it.
 * `authorized` gates the MFP-until-authorized bit the way iwl_mvm_mld_cfg_sta
 * does (capa 114 = STA_EXP_MFP_SUPPORT). */
static void mld_sta_cfg(const u8 addr[6], int aid, int authorized)
{
    struct mld_sta_cmd c;
    memset(&c, 0, sizeof c);
    c.sta_id = AP_STA_ID;
    c.link_id = MLD_LINK_ID;
    memcpy(c.peer_mld_address, addr, 6);
    memcpy(c.peer_link_address, addr, 6);
    c.station_type = 0;                 /* STATION_TYPE_PEER */
    c.assoc_id = (u32)aid;              /* only meaningful once associated */
    /* MFP stays set for the life of a PMF association.  The reference driver
     * raises it while unauthorized (capa 114 = STA_EXP_MFP_SUPPORT) and drops
     * it once the 4-way ends - but that is the NON-PMF case.  When we have
     * actually negotiated management-frame protection, clearing it after
     * authorization would tell the firmware to stop protecting the very frames
     * PMF exists for. */
    if (fw_has_capa(114) && (!authorized || g_join_mfp >= WPA_MFP_CAPABLE)) c.mfp = 1;
    c.mimo = 0;                         /* 1 spatial stream, matching rxchain */
    uno_dbg_net_trace("wifi: STA_CONFIG sta=%d link=%d aid=%d auth=%d "
                      "peer=%02x:%02x:%02x:%02x:%02x:%02x len=%d",
                      AP_STA_ID, MLD_LINK_ID, aid, authorized,
                      addr[0],addr[1],addr[2],addr[3],addr[4],addr[5], (int)sizeof c);
    send_cmd(GRP_MACCONF, MC_STA_CONFIG, 0, &c, (int)sizeof c);
    wait_cmd_done(100);
}

/* STA_REMOVE_CMD (MAC_CONF 0x0c) — drop the station entirely. `{ u32 sta_id }`
 * and nothing else (iwl_remove_sta_cmd, REMOVE_STA_API_S_VER_1).
 *
 * METAL, 2026-08-05, SURFGO: re-sending STA_CONFIG_CMD with a different peer
 * updates the station enough for TRANSMIT - the AP answers our CCMP-encrypted
 * DHCP DISCOVER, one 376-byte reply per retransmission - and not enough for
 * RECEIVE. Every one of those replies arrives with the protected bit set, a
 * KeyID-0 CCMP header intact, and an RX descriptor reporting SEC=UNKNOWN with
 * DECRYPTED clear: the hardware found no key to apply to a frame encrypted
 * with the very key it is transmitting under.
 *
 * The reference driver never re-points a live station. Every association
 * removes the station and adds a fresh one, so the address the hardware
 * matches received frames against is written at ADD time and never mutated.
 * That is what this exists for. */
static void mld_sta_remove(void)
{
    struct __attribute__((packed)) { u32 sta_id; } c;
    c.sta_id = (u32)AP_STA_ID;
    uno_dbg_net_trace("wifi: STA_REMOVE sta=%d len=%d", AP_STA_ID, (int)sizeof c);
    send_cmd(GRP_MACCONF, MC_STA_REMOVE, 0, &c, (int)sizeof c);
    wait_cmd_done(100);
}

/* SEC_KEY_CMD (DATA_PATH 0x18) — the MLD key install path. The legacy
 * ADD_STA_KEY (LONG 0x17) belongs to the ADD_STA world and has no station to
 * attach to once the peer came from STA_CONFIG_CMD, so the 4-way handshake has
 * to install its CCMP keys through here instead (iwl_mvm_mld_send_key). */
static void mld_sec_key(const u8 *key, int keylen, int keyidx, int mcast)
{
    struct __attribute__((packed)) {
        u32 action, sta_mask, key_id, key_flags;
        u8 key[32], tkip_mic_rx[8], tkip_mic_tx[8];
        u64 rx_seq, tx_seq;
    } c;
    memset(&c, 0, sizeof c);
    c.action = 1;                       /* FW_CTXT_ACTION_ADD */
    c.sta_mask = (u32)(1u << AP_STA_ID);
    c.key_id = (u32)keyidx;
    c.key_flags = 0x02;                 /* IWL_SEC_KEY_FLAG_CIPHER_CCMP */
    if (mcast) c.key_flags |= 0x40 | 0x08;   /* MCAST_KEY | NO_TX */
    memcpy(c.key, key, keylen < 32 ? keylen : 32);
    uno_dbg_net_trace("wifi: SEC_KEY idx=%d mcast=%d flags=%02x len=%d",
                      keyidx, mcast, (unsigned)c.key_flags, (int)sizeof c);
    send_cmd(GRP_DATAPATH, DP_SEC_KEY, 0, &c, (int)sizeof c);
    wait_cmd_done(100);
}

/* SEC_KEY_CMD with action REMOVE (3). The union's remove form is
 * {sta_mask, key_id, key_flags} inside the same envelope the ADD uses, and the
 * flags must match those the key was installed with (iwl_mvm_sec_key_del).
 *
 * Keys belong to the STATION, so re-pointing the station at another AP leaves
 * the previous association's CCMP keys live and the hardware then decrypts the
 * new AP's frames with the wrong one - on metal that showed up as frames
 * arriving and being discarded as garbage (`tx 8 rx 0 drop 8`) after an
 * otherwise complete retargeted join. */
static void mld_sec_key_remove(int keyidx, int mcast)
{
    struct __attribute__((packed)) {
        u32 action, sta_mask, key_id, key_flags;
        u8 key[32], tkip_mic_rx[8], tkip_mic_tx[8];
        u64 rx_seq, tx_seq;
    } c;
    memset(&c, 0, sizeof c);
    c.action = 3;                       /* FW_CTXT_ACTION_REMOVE */
    c.sta_mask = (u32)(1u << AP_STA_ID);
    c.key_id = (u32)keyidx;
    c.key_flags = 0x02;                 /* CCMP - must match the installed key */
    if (mcast) c.key_flags |= 0x40 | 0x08;   /* MCAST_KEY | NO_TX */
    uno_dbg_net_trace("wifi: SEC_KEY REMOVE idx=%d mcast=%d flags=%02x len=%d",
                      keyidx, mcast, (unsigned)c.key_flags, (int)sizeof c);
    send_cmd(GRP_DATAPATH, DP_SEC_KEY, 0, &c, (int)sizeof c);
    wait_cmd_done(100);
}

/* Allocate one TX queue for (sta_id, tid) via SCD_QUEUE_CFG (queue_alloc_cmd_ver
 * 0 = the pre-AX210 path: LEGACY-group cmd 0x1d, iwl_tx_queue_cfg_cmd, 20 B).
 * Points the fw at our existing TFD ring (g_tx_ring) + byte-count table (g_tx_bc,
 * already in the pre-AX210 dword bc_ent format tx_enqueue writes). The fw echoes
 * iwl_tx_queue_cfg_rsp {queue_number, flags, write_pointer}; we adopt the queue
 * as g_data_qid so tx_enqueue targets it. Nothing has ever transmitted before
 * this - g_data_qid stayed -1 and tx_enqueue fell back to a bogus qid 10.
 * Returns the assigned qid, or -1 on no/short response. */
static int mvm_txq_alloc(int sta_id, int tid, int size)
{
    /* SCD_QUEUE_CONFIG_CMD (DATA_PATH group 5 / cmd 0x17), the queue_alloc_cmd_ver
       3 path. The old SCD_QUEUE_CFG (0x1d) got BAD_COMMAND on this QuZ-77 fw
       (2026-07-25) - this fw only speaks the new iwl_scd_queue_cfg_cmd (36 B).
       operation=IWL_SCD_QUEUE_ADD, sta_mask=BIT(sta_id); point the fw at our
       TFD ring + bc table. Response iwl_tx_queue_cfg_rsp {queue_number, flags,
       write_pointer}; adopt queue_number as g_data_qid. */
    struct __attribute__((packed)) {
        u32 operation; u32 sta_mask; u8 tid; u8 rsv[3]; u32 flags; u32 cb_size;
        u64 bc_addr; u64 tfdq_addr;
    } c;
    const u8 *r; int len = 0, cb = 0, n = size;
    while (n > 8) { cb++; n >>= 1; }              /* cb_size = ilog2(size) - 3 */
    memset(&c, 0, sizeof c);
    c.operation = 0;                              /* IWL_SCD_QUEUE_ADD */
    c.sta_mask = (u32)(1u << sta_id);
    c.tid = (u8)tid;
    c.flags = 0;
    c.cb_size = (u32)cb;
    c.bc_addr = phys(g_tx_bc);
    c.tfdq_addr = phys(g_tx_ring);
    send_cmd(GRP_DATAPATH, 0x17, 0, &c, (int)sizeof c);
    r = wait_notif(GRP_DATAPATH, 0x17, &len, 300);
    if (r && len >= 2) {
        g_data_qid = r[0] | (r[1] << 8);
        if (len >= 6) g_tx_wr = (r[4] | (r[5] << 8)) & (TXQ_N - 1);
        return g_data_qid;
    }
    return -1;
}

/* SCD_QUEUE_CONFIG_CMD with operation = IWL_SCD_QUEUE_REMOVE (1). The union in
 * iwl_scd_queue_cfg_cmd makes the remove form {operation, sta_mask, tid} - all
 * u32 - inside the same 36-byte envelope the ADD uses. Needed to re-point the
 * station at another AP: the queue belongs to the station, and once the
 * station's address changes the fw quietly stops transmitting on it (metal: the
 * first frame comes back not-ACKed, the next produces no TX response at all)
 * while refusing to allocate a second one for the same sta/TID. */
static void mvm_txq_free(int sta_id, int tid)
{
    struct __attribute__((packed)) { u32 operation, sta_mask, tid; u8 pad[24]; } c;
    memset(&c, 0, sizeof c);
    c.operation = 1;                              /* IWL_SCD_QUEUE_REMOVE */
    c.sta_mask = (u32)(1u << sta_id);
    c.tid = (u32)tid;
    uno_dbg_net_trace("wifi: SCD_QUEUE remove sta=%d tid=%d (qid was %d) len=%d",
                      sta_id, tid, g_data_qid, (int)sizeof c);
    send_cmd(GRP_DATAPATH, 0x17, 0, &c, (int)sizeof c);
    wait_cmd_done(100);
    g_data_qid = -1;
}

/* SCAN_CFG_CMD (v5+ small form) */
static void mvm_scan_cfg(void)
{
    struct { u8 cam, promisc, bcast_sta, rsv; u32 tx_chains, rx_chains; } c;
    memset(&c, 0, sizeof c);
    c.bcast_sta = 1; c.tx_chains = 1; c.rx_chains = 1;
    send_cmd(GRP_LONG, 0xc, 0, &c, sizeof c); wait_cmd_done(100);
}

/* =====================================================================
 * 10. WIFI.CFG parse
 * ===================================================================== */
static char g_cfg_ssid[36];
static char g_cfg_psk[80];
/* the hand-staged WIFI.CFG credentials, kept aside when a REMEMBERED network
 * takes precedence over them - see the credential block in iwl_nic() */
static char g_alt_ssid[36];
static char g_alt_psk[80];
/* ONE parser for the credentials file, used both to TEST a candidate volume and
 * to READ the values off it.
 *
 * There used to be two. file_has_ssid() peeked at the first 255 bytes and did a
 * raw byte search for "ssid="; read_config() read 511 and parsed properly,
 * skipping '#' comments. Two parsers over one file format is a divergence
 * waiting to happen, and it happened: the NAS credentials template documents
 * itself in a 434-byte comment header, so `ssid=` sits at byte 434 - past the
 * first parser's window and inside the second's. The log said both things at
 * once (metal, SURFGO 2026-08-04):
 *
 *   plan: ... creds:WIFI.TXT (vol 1)                  <- the planner found it
 *   wifi: FAIL no usable credentials (cfg vol -1)     <- the driver did not
 *
 * so a stick with perfectly good credentials could not join, and the failure
 * named the file it was refusing to read.
 *
 * `ssid`/`psk` may be NULL when the caller only wants to know whether usable
 * credentials are present. Returns 0 if an ssid was found, -1 otherwise.
 *
 * A commented-out example (`# ssid=YourNetwork`, which the starter file
 * tools/uno-wifi-fw.py writes) must NOT read as credentials - that is F13's
 * shadowing trap in a different costume, and skipping '#' lines is what stops
 * it. The raw search never did. */
#define CFG_MAX 4096
static int cfg_parse(int vol, const char *name,
                     char *ssid, int ssidcap, char *psk, int pskcap)
{
    static u8 buf[CFG_MAX];
    long n = uno_fs_read(vol, name, buf, (long)sizeof buf - 1);
    int i = 0, got = 0;
    if (ssid) ssid[0] = 0;
    if (psk)  psk[0]  = 0;
    if (n <= 0) return -1;
    buf[n] = 0;
    while (i < n) {
        char key[16]; char val[80]; int k=0, v=0;
        while (i<n && (buf[i]==' '||buf[i]=='\t'||buf[i]=='\r'||buf[i]=='\n')) i++;
        if (i<n && buf[i]=='#') { while (i<n && buf[i]!='\n') i++; continue; }
        while (i<n && buf[i]!='=' && buf[i]!='\n' && k<15) key[k++]=buf[i++];
        key[k]=0;
        if (i<n && buf[i]=='=') { i++;
            while (i<n && buf[i]!='\n' && buf[i]!='\r' && v<79) val[v++]=buf[i++];
            while (v>0 && (val[v-1]==' '||val[v-1]=='\t')) v--;
            val[v]=0;
            if (!strcmp(key,"ssid")) {
                got = val[0] != 0;
                if (ssid && got) { strncpy(ssid, val, (size_t)ssidcap - 1);
                                   ssid[ssidcap - 1] = 0; }
            } else if (!strcmp(key,"psk") && psk) {
                strncpy(psk, val, (size_t)pskcap - 1); psk[pskcap - 1] = 0;
            }
        }
        while (i<n && buf[i]!='\n') i++;
    }
    return got ? 0 : -1;
}

static int read_config(int vol)
{
    return cfg_parse(vol, g_cfgname[0] ? g_cfgname : "WIFI.CFG",
                     g_cfg_ssid, (int)sizeof g_cfg_ssid,
                     g_cfg_psk,  (int)sizeof g_cfg_psk);
}

/* =====================================================================
 * 10a. Progress reporting (see iwlwifi.h)
 * ===================================================================== */
#define IWL_PROG_STEPS 5
static iwl_progress_fn g_prog_fn;
static void *g_prog_ctx;
static const char *g_prog_what = "";
static int  g_prog_step;

void iwl_progress_set(iwl_progress_fn fn, void *ctx)
{ g_prog_fn = fn; g_prog_ctx = ctx; }

/* enter a phase */
static void prog(const char *what, int step)
{
    g_prog_what = what; g_prog_step = step;
    if (g_prog_fn) g_prog_fn(g_prog_ctx, what, step, IWL_PROG_STEPS);
}
/* still in the same phase, still working - called from the wait loops so the
 * caller's indicator keeps moving through a five-second dwell */
static void prog_tick(void)
{ if (g_prog_fn) g_prog_fn(g_prog_ctx, g_prog_what, g_prog_step, IWL_PROG_STEPS); }

/* =====================================================================
 * 10b. Remembered networks (WIFINETS.CFG)
 * =====================================================================
 * A network joined from the Control Panel used to be forgotten at power-off:
 * only a hand-staged WIFI.CFG survived a reboot, so a laptop with no wired
 * port came back up with no network and no way to get one without retyping the
 * passphrase.  This is the store that fixes that.
 *
 * FORMAT.  The same ssid=/psk= grammar WIFI.CFG uses, repeated, MOST RECENTLY
 * JOINED FIRST - so "rejoin the last network" is just entry 0, and re-joining a
 * remembered network moves it back to the top.  An `ssid=` line starts a new
 * entry; a `psk=` line belongs to the entry above it.  Comments and blank lines
 * are skipped, so the file stays hand-editable like WIFI.CFG.
 *
 * IT IS PLAINTEXT, deliberately and visibly.  A WPA2 passphrase has to be
 * recoverable to derive the PMK, this OS has no user-keyed secret store to wrap
 * it with, and WIFI.CFG has always sat on the same volume in the clear - an
 * encrypted-looking file whose key is also on the disk would claim a protection
 * it does not have.  The file is written to the same volume the shell picks for
 * SHELL.CFG (a real partition ahead of the RAM disk), and `Forget` deletes an
 * entry outright.
 * ===================================================================== */
#define SAVED_MAX      8            /* networks remembered, oldest dropped     */
#define SAVED_SSID_MAX 36
#define SAVED_PSK_MAX  72
#define SAVED_FILE     "WIFINETS.CFG"

typedef struct { char ssid[SAVED_SSID_MAX]; char psk[SAVED_PSK_MAX]; } saved_net;
static saved_net g_saved[SAVED_MAX];
static int       g_saved_n;
static int       g_saved_loaded;     /* the file has been read this boot       */
static int       g_saved_vol = -1;   /* where it was found / will be written   */

/* Where remembered networks live: the same shared answer the shell's session
 * file and unosecure's store use (uno_fs_pref_vol - the boot volume first,
 * then the old fallback order).  Preferring the boot volume also keeps saved
 * networks on the stick the machine actually booted, instead of leaking them
 * onto whatever internal disk happens to enumerate first. */
static int saved_vol(void)
{
    return uno_fs_pref_vol();
}

static void saved_load(void)
{
    static u8 buf[1024];
    int nv, v; long n = -1;
    if (g_saved_loaded) return;
    g_saved_loaded = 1;
    g_saved_n = 0;
    /* read from ANY volume that has one (a stick moved between machines still
     * carries its networks), but write back to the preferred one */
    nv = uno_fs_volumes();
    for (v = 0; v < nv && n <= 0; v++) {
        n = uno_fs_read(v, SAVED_FILE, buf, (long)sizeof buf - 1);
        if (n > 0) g_saved_vol = v;
    }
    if (g_saved_vol < 0) g_saved_vol = saved_vol();
    if (n <= 0) return;
    buf[n] = 0;
    { long i = 0;
      while (i < n) {
        char key[16], val[SAVED_PSK_MAX]; int k = 0, x = 0;
        while (i<n && (buf[i]==' '||buf[i]=='\t'||buf[i]=='\r'||buf[i]=='\n')) i++;
        if (i<n && buf[i]=='#') { while (i<n && buf[i]!='\n') i++; continue; }
        while (i<n && buf[i]!='=' && buf[i]!='\n' && k<15) key[k++] = (char)buf[i++];
        key[k] = 0;
        if (i<n && buf[i]=='=') { i++;
            while (i<n && buf[i]!='\n' && buf[i]!='\r' && x < (int)sizeof val - 1)
                val[x++] = (char)buf[i++];
            while (x>0 && (val[x-1]==' '||val[x-1]=='\t')) x--;
            val[x] = 0;
            if (!strcmp(key, "ssid") && val[0] && g_saved_n < SAVED_MAX) {
                memset(&g_saved[g_saved_n], 0, sizeof g_saved[0]);
                strncpy(g_saved[g_saved_n].ssid, val, SAVED_SSID_MAX - 1);
                g_saved_n++;
            } else if (!strcmp(key, "psk") && g_saved_n > 0) {
                strncpy(g_saved[g_saved_n-1].psk, val, SAVED_PSK_MAX - 1);
            }
        }
        while (i<n && buf[i]!='\n') i++;
      } }
    uno_dbg_net_trace("wifi: %d remembered network(s) from vol %d", g_saved_n, g_saved_vol);
}

static void saved_write(void)
{
    /* 8 entries x (ssid= + 35 + CRLF + psk= + 71 + CRLF) plus the header is
     * 1082 bytes - which 1024 did not hold, so the guard below silently
     * dropped the last network or two. */
    static u8 buf[2048];
    int i; long o = 0;
    const char *hdr = "# UnoDOS remembered WiFi networks - most recent first.\r\n"
                      "# Written by the Control Panel; hand-editable, same keys as WIFI.CFG.\r\n";
    if (g_saved_vol < 0) g_saved_vol = saved_vol();
    if (g_saved_vol < 0) return;                    /* nowhere to keep it */
    while (hdr[o]) { buf[o] = (u8)hdr[o]; o++; }
    for (i = 0; i < g_saved_n; i++) {
        const char *s;
        if (o + SAVED_SSID_MAX + SAVED_PSK_MAX + 16 > (long)sizeof buf) break;
        buf[o++]='s'; buf[o++]='s'; buf[o++]='i'; buf[o++]='d'; buf[o++]='=';
        for (s = g_saved[i].ssid; *s; s++) buf[o++] = (u8)*s;
        buf[o++]='\r'; buf[o++]='\n';
        buf[o++]='p'; buf[o++]='s'; buf[o++]='k'; buf[o++]='=';
        for (s = g_saved[i].psk; *s; s++) buf[o++] = (u8)*s;
        buf[o++]='\r'; buf[o++]='\n';
    }
    /* uno_fs_write returns NON-ZERO ON SUCCESS - the convention every other
     * caller in the tree uses (`if (uno_fs_write(...))` = it worked). This test
     * was inverted, so it announced "could not write" every time the store
     * SAVED and said nothing at all when it genuinely failed. A metal session
     * read that line, reasonably concluded the feature was broken, and filed it
     * as such. An error message that fires on the success path is worse than no
     * message: it sends someone to debug a thing that is working.
     *
     * Both outcomes are logged now, so the next NETLOG reading is unambiguous. */
    if (!uno_fs_write(g_saved_vol, SAVED_FILE, buf, o)) {
        /* A real failure. The likely cause is that saved_load() bound us to the
         * volume it FOUND the file on, which may be read-only (a firmware SFS
         * after detach, or a stick that came up ro). Re-pick a writable one and
         * try once more before giving up. */
        int alt = saved_vol();
        uno_dbg_net_trace("wifi: %s write FAILED on vol %d", SAVED_FILE, g_saved_vol);
        if (alt >= 0 && alt != g_saved_vol &&
            uno_fs_write(alt, SAVED_FILE, buf, o)) {
            uno_dbg_net_trace("wifi: %s saved on vol %d instead (%d network(s))",
                              SAVED_FILE, alt, g_saved_n);
            g_saved_vol = alt;
        } else {
            uno_dbg_net_trace("wifi: %s could not be saved anywhere - the next "
                              "boot has no network to rejoin", SAVED_FILE);
        }
    } else {
        uno_dbg_net_trace("wifi: %s saved on vol %d (%d network(s), %d bytes)",
                          SAVED_FILE, g_saved_vol, g_saved_n, (int)o);
    }
    memset(buf, 0, sizeof buf);                     /* do not leave it in .bss */
}

/* Remember a network that just joined, at the top of the list. */
static void saved_remember(const char *ssid, const char *psk)
{
    int i, j;
    if (!ssid || !ssid[0]) return;
    saved_load();
    for (i = 0; i < g_saved_n; i++) if (!strcmp(g_saved[i].ssid, ssid)) break;
    if (i == g_saved_n) {                            /* new: make room at 0 */
        if (g_saved_n < SAVED_MAX) g_saved_n++;
        i = g_saved_n - 1;
    }
    for (j = i; j > 0; j--) g_saved[j] = g_saved[j-1];
    memset(&g_saved[0], 0, sizeof g_saved[0]);
    strncpy(g_saved[0].ssid, ssid, SAVED_SSID_MAX - 1);
    if (psk) strncpy(g_saved[0].psk, psk, SAVED_PSK_MAX - 1);
    saved_write();
}

int iwl_saved_count(void) { saved_load(); return g_saved_n; }

int iwl_saved_get(int i, char *ssid, int cap)
{
    saved_load();
    if (i < 0 || i >= g_saved_n || !ssid || cap <= 0) return -1;
    strncpy(ssid, g_saved[i].ssid, (unsigned)cap - 1); ssid[cap-1] = 0;
    return 0;
}

int iwl_saved_psk(const char *ssid, char *psk, int cap)
{
    int i;
    saved_load();
    if (!ssid || !psk || cap <= 0) return 0;
    for (i = 0; i < g_saved_n; i++)
        if (!strcmp(g_saved[i].ssid, ssid)) {
            strncpy(psk, g_saved[i].psk, (unsigned)cap - 1); psk[cap-1] = 0;
            return 1;
        }
    return 0;
}

#ifdef UNO_DEBUG
/* ---- test hook -------------------------------------------------------------
 * The store is otherwise reachable ONLY through a real join, i.e. only on a
 * machine with an Intel card - which is exactly why an inverted success test in
 * saved_write() shipped, and why the log line it produced was then read on
 * metal as "the feature is broken". Nothing headless could reach the code to
 * disagree.
 *
 * This runs the whole round trip through the REAL filesystem: remember a
 * network, drop the in-memory copy, re-read it off disk, check it came back
 * first and with its passphrase, then forget it and check it is gone. Returns 0
 * on success or the number of the step that failed.
 *
 * It leaves the store as it found it: remember/forget preserve the other
 * entries and their order, and it refuses to run at all when the store is full,
 * where remembering a ninth network would push the oldest one out. */
int iwl_saved_selftest(void)
{
    static const char *T = "uno-selftest";     /* cannot collide with a real SSID */
    static const char *P = "selftest-passphrase";
    char got[SAVED_PSK_MAX];
    int n0;

    saved_load();
    n0 = g_saved_n;
    if (n0 >= SAVED_MAX) return -1;              /* would evict a real network */

    saved_remember(T, P);
    if (g_saved_n != n0 + 1)                     return 1;
    if (strcmp(g_saved[0].ssid, T))              return 2;   /* most recent first */

    /* forget the in-memory copy and re-read from disk: this is the step that
     * proves the WRITE happened, not just that the array was updated */
    g_saved_loaded = 0; g_saved_n = 0;
    saved_load();
    if (g_saved_n != n0 + 1)                     return 3;
    if (strcmp(g_saved[0].ssid, T))              return 4;
    got[0] = 0;
    if (!iwl_saved_psk(T, got, (int)sizeof got)) return 5;
    if (strcmp(got, P))                          return 6;

    iwl_saved_forget(T);
    if (g_saved_n != n0)                         return 7;
    g_saved_loaded = 0; g_saved_n = 0;           /* and it stayed forgotten */
    saved_load();
    if (g_saved_n != n0)                         return 8;
    if (iwl_saved_psk(T, got, (int)sizeof got))  return 9;
    return 0;
}
#endif /* UNO_DEBUG */

void iwl_saved_forget(const char *ssid)
{
    int i, j;
    saved_load();
    if (!ssid) return;
    for (i = 0; i < g_saved_n; i++)
        if (!strcmp(g_saved[i].ssid, ssid)) {
            for (j = i; j < g_saved_n - 1; j++) g_saved[j] = g_saved[j+1];
            memset(&g_saved[--g_saved_n], 0, sizeof g_saved[0]);
            saved_write();
            return;
        }
}

/* =====================================================================
 * 11. 802.11 <-> Ethernet translation, TX/RX data
 * ===================================================================== */
/* g_bssid is declared up at the RX dispatch (it tags frames from our AP) */
static u16 g_seq_no;
static u16 g_aid;                        /* association id, set by mvm_assoc() */

/* Data frames go out as PLAIN data (subtype 0), not QoS data: our association
 * request carries no WMM/HT IE, so the AP admits us as a non-QoS station, and
 * the fw data queue is allocated on TID 15 (IWL_MAX_TID_COUNT), which is
 * iwlwifi's non-QoS queue. `iwl qos 1` flips this at runtime so a wrong guess
 * costs a URC line instead of a USB reflash. */
static int g_tx_qos;

/* build a QoS-data 802.11 header (ToDS) + LLC/SNAP for an Ethernet frame,
   returns total 802.11 payload length written to out (after the tx cmd). */
static int eth_to_80211(const u8 *eth, int ethlen, u8 *out)
{
    u8 *p = out;
    u16 ethertype = (u16)((eth[12]<<8) | eth[13]);
    /* frame control: type data(2), subtype data(0) or qos-data(8); ToDS => 0x01 */
    *p++ = g_tx_qos ? 0x88 : 0x08; *p++ = 0x01;
    *p++ = 0; *p++ = 0;                 /* duration */
    memcpy(p, g_bssid, 6); p += 6;      /* addr1 = BSSID */
    memcpy(p, g_mac, 6);   p += 6;      /* addr2 = SA (us) */
    memcpy(p, eth, 6);     p += 6;      /* addr3 = DA */
    *p++ = (u8)(g_seq_no<<4); *p++ = (u8)(g_seq_no>>4); g_seq_no++;  /* seq ctl */
    if (g_tx_qos) { *p++ = 0; *p++ = 0; }   /* QoS control (TID 0) */
    memcpy(p, SNAP, 6); p += 6;         /* LLC/SNAP */
    *p++ = (u8)(ethertype>>8); *p++ = (u8)ethertype;
    memcpy(p, eth + 14, ethlen - 14); p += ethlen - 14;
    return (int)(p - out);
}

/* 802.11 data frame -> Ethernet, into out (>= len). `snap` is the offset of the
 * LLC/SNAP header the RX dispatch already located (past the MAC header and any
 * CCMP header). Returns eth len, or -1. */
static int wifi_to_eth(const u8 *f, int len, int snap, u8 *out)
{
    u16 fc = (u16)(f[0] | (f[1]<<8));
    int tods = (fc & 0x0100) != 0, fromds = (fc & 0x0200) != 0;
    const u8 *da, *sa;
    if (len < snap + 8) return -1;
    /* address layout by ToDS/FromDS: from the AP it is FromDS (a1=DA a2=BSSID
     * a3=SA); an IBSS/peer frame is neither (a1=DA a2=SA). 4-address (WDS) is
     * not a shape a STA sees, so it is refused rather than mis-parsed. */
    if (fromds && !tods)       { da = f + 4;  sa = f + 16; }
    else if (!fromds && !tods) { da = f + 4;  sa = f + 10; }
    else if (tods && !fromds)  { da = f + 16; sa = f + 10; }
    else return -1;
    if (memcmp(f + snap, SNAP, 6) != 0) return -1;
    { u16 et = (u16)((f[snap+6]<<8)|f[snap+7]);
      memcpy(out, da, 6); memcpy(out+6, sa, 6);
      out[12]=(u8)(et>>8); out[13]=(u8)et;
      memcpy(out+14, f + snap + 8, len - snap - 8);
      return 14 + (len - snap - 8); }
}

/* queue a decrypted 802.11 data frame for recv() (called from rx processing) */
static void handle_data_frame(const u8 *frame, int len, int snap_off)
{
    u8 eth[1600];
    int n = wifi_to_eth(frame, len, snap_off, eth);
    int nx;
    if (n <= 0 || n > 1600) { g_rx_data_drop++; return; }
    nx = (g_dq_head + 1) % DATAQ;
    if (nx == g_dq_tail) { g_rx_data_drop++; return; }    /* full, drop */
    memcpy(g_dataq[g_dq_head].buf, eth, n);
    g_dataq[g_dq_head].len = n;
    g_dq_head = nx;
    g_rx_data_n++;
}

static wpa_sm_t g_wpa;
static int g_wpa_active;    /* g_keys_installed is declared up at tx_enqueue,
                             * which needs it to decide IWL_TX_FLAGS_ENCRYPT_DIS */
/* `eapol` points at the EAPOL header (after the 802.11 MAC header and
 * LLC/SNAP), which is what wpa_sm_rx_eapol expects - see the call site. */
static void handle_eapol(const u8 *eapol, int len)
{
    u8 reply[600];
    int r;
    if (len <= 0) return;
    /* AN UNARMED SUPPLICANT HAS NO BUSINESS PROCESSING EAPOL, and until now
     * it did, which froze the machine.
     *
     * On the Surface Laptop Go, 2026-08-24: the 4-way completes, the AP then
     * deauthenticates with reason 2, and the deauth handler correctly clears
     * g_wpa_active - but the supplicant object keeps its state, which is
     * WPA_ST_DONE. The AP restarts the handshake; message 1/4 arrives; a DONE
     * supplicant answers it with NOTHING ("state 2, reply 0"), so the AP never
     * receives 2/4, gives up, deauthenticates again - and we meanwhile accept
     * the retried 3/4, reinstall the keys and announce "4-way handshake DONE"
     * once more. Twenty-one times in the log, every 200 ms, forever. The join
     * runs on the shell thread, so the whole desktop sits frozen on
     * "exchanging keys" while this spins.
     *
     * The association is gone the moment the AP says so, and a handshake
     * without one is meaningless: the join path re-arms when it genuinely
     * re-associates. Ignore it here, and say so once rather than per frame. */
    if (!g_wpa_active) {
        static int moaned;
        if (!moaned++) uno_dbg_net_trace("wifi: EAPOL after the link went down "
                                         "- ignored (the supplicant is not armed)");
        return;
    }
    r = wpa_sm_rx_eapol(&g_wpa, eapol, len, reply, sizeof reply);
    /* key_info (big-endian, at EAPOL offset 5) says which message this is:
     * bit 3 PAIRWISE, bit 6 INSTALL, bit 7 ACK, bit 8 MIC, bit 12 ENCRYPTED.
     * 1/4 = ACK, no MIC.  3/4 = ACK|MIC(|ENCRYPTED).  Without it a retried 1/4
     * and a MIC-failing 3/4 look identical in the log. */
    uno_dbg_net_trace("wifi: EAPOL in (%d bytes, type=%02x desc=%02x ki=%04x %s) -> state %d, reply %d",
                      len, len > 1 ? eapol[1] : 0, len > 4 ? eapol[4] : 0,
                      len > 6 ? ((eapol[5] << 8) | eapol[6]) : 0,
                      (len > 6 && (eapol[5] & 0x01)) ? "3/4" : "1/4",
                      g_wpa.state, r);
    if (r <= 0) return;
    /* TX the EAPOL reply as a data frame (in the clear, high priority) before
       installing keys */
    { u8 eth[600], tx[720]; int el, n;
      memcpy(eth, g_bssid, 6); memcpy(eth+6, g_mac, 6);   /* dst=BSSID, src=us */
      eth[12]=0x88; eth[13]=0x8E;                          /* ethertype EAPOL */
      memcpy(eth+14, reply, r); el = 14 + r;
      n = eth_to_80211(eth, el, tx);
      tx_enqueue(tx, n, 1 /*high priority*/);
    }
    if (g_wpa.state == WPA_ST_DONE && !g_keys_installed) {
        if (fw_has_mld_api()) {
            /* the link API installs keys via SEC_KEY_CMD and re-sends
             * STA_CONFIG_CMD (now authorized, so the MFP bit drops) instead of
             * ADD_STA_KEY + an ADD_STA MODIFY */
            mld_sec_key(g_wpa.ptk + 32, 16, 0, 0);             /* pairwise TK */
            if (g_wpa.gtk_len) { mld_sec_key(g_wpa.gtk, g_wpa.gtk_len, g_wpa.gtk_idx, 1);
                                 g_key_gtk_idx = g_wpa.gtk_idx; }
            /* The IGTK verifies BROADCAST management frames (BIP).  We hold
             * it but do not hand it to the firmware by default: the SEC_KEY
             * shape for a BIP key on this fw is untested here, and a command
             * this firmware dislikes does not fail - it SYSASSERTs, and only a
             * reboot recovers.  `iwl igtk 1` turns the install on for the next
             * join so it can be tried on metal deliberately.
             *
             * What we lose meanwhile: an unprotected broadcast deauth is still
             * acted on, exactly as it is on the WPA2 path today.  That is not
             * a regression, but it IS the protection PMF is supposed to add,
             * so it stays listed as open in pc64/WIFI-SECURITY.md. */
            if (g_wpa.igtk_len) {
                if (g_igtk_install) mld_sec_key(g_wpa.igtk, g_wpa.igtk_len, g_wpa.igtk_idx, 1);
                uno_dbg_net_trace("wifi: IGTK received (idx=%d len=%d) - %s",
                                  g_wpa.igtk_idx, g_wpa.igtk_len,
                                  g_igtk_install ? "installed" :
                                  "NOT installed (iwl igtk 1 to try it)");
            }
            g_keys_installed = 1;
            mld_sta_cfg(g_bssid, g_aid, 1 /*authorized*/);
        } else {
            mvm_add_sta_key(g_wpa.ptk + 32, 16, 0, 0, 0);      /* pairwise TK */
            if (g_wpa.gtk_len) mvm_add_sta_key(g_wpa.gtk, g_wpa.gtk_len, g_wpa.gtk_idx, 1, 0);
            g_keys_installed = 1;
            mvm_add_sta(g_bssid, 1, (1u<<14)|(1u<<15));        /* authorize */
        }
        g_joined = 1;
        /* Baseline the post-join counters here, so the diagnosis in iwl_link()
         * describes THIS association and not the noise of getting to it. */
        g_join_ms = uno_dbg_uptime_ms();
        g_postjoin_diag_done = 0;
        g_txr_total = g_txr_ackfail = g_rx_from_ap = 0;
        g_rx_data_n = g_rx_data_drop = g_tx_data_n = 0;
        g_rx_foreign = g_rx_prot = g_rx_prot_dec = 0; g_rx_st_undec = 0;
        /* ARM THE RX DESCRIPTION FOR THE FIRST FEW FRAMES OF THIS ASSOCIATION.
         *
         * g_rx_data_log gates the two RXDATA traces that say what a received
         * data frame actually looked like - and it was only ever armed by a
         * hand-typed `iwl` debug verb. On the Surface that made it unreachable
         * by construction: the machine has no wired NIC, so with no lease
         * there is no URC, so there is no way to type the verb that would
         * explain why there is no lease.
         *
         * That boot ended `rx=0 from_ap=22 drop=69`: the AP was talking to us,
         * every frame we sent was ACKed, and every single data frame that came
         * back was thrown away without a word about why. Sixteen frames is
         * enough to characterise the shape and small enough that a busy link
         * cannot turn it into a log flood - and it is per association, so a
         * rejoin re-arms it. */
        g_rx_data_log = 16;
        g_deauth_ours = g_deauth_other = 0; g_deauth_reason = -1;
        /* Publish the nic service the moment the link is usable, whatever drove
         * the join - the step-by-step debug verbs (mld/auth/assoc/eapol) reach
         * this point without ever going through iwl_nic(), and send/recv both
         * gate on g_bound, so the data path was dead after a manual join. */
        nic_publish();
        uno_dbg_net_trace("wifi: 4-way handshake DONE - CCMP keys installed "
                          "(gtk_len=%d idx=%d), station authorized",
                          g_wpa.gtk_len, g_wpa.gtk_idx);
    }
}

/* =====================================================================
 * 12. connect: scan for the SSID, then run the assoc + 4-way handshake
 * ===================================================================== */
/* ---- UMAC passive scan (SCAN_REQ_UMAC cmd_ver 15 = the v17 struct family) --
 * cmd_ver 15 (per tools/iwl_cmd_versions.py) uses iwl_scan_req_umac_v17, which
 * covers fw versions 14-17.  Passive scan needs no probe-request template: we
 * just listen for beacons on the 2.4 GHz channels and record BSSID/SSID/channel
 * so a real join can target a real AP (the old find_and_join used a broadcast
 * BSSID, which wedged ADD_STA).  Layout mirrors ~/n9/kernel fw/api/scan.h; the
 * static assert below catches any drift from the 1940 B the fw expects. */
struct sc_chan  { u32 flags; u8 num; u8 band; u8 iter_count; u8 iter_interval; } __attribute__((packed));
struct sc_gen   { u16 flags; u8 rsv; u8 mac_or_link; u8 active_dwell[2]; u8 adw2g; u8 adw5g;
                  u8 adw_social; u8 flags2; u16 adw_budget; u32 max_out[2]; u32 suspend[2];
                  u32 priority; u8 passive_dwell[2]; u8 num_frags[2]; } __attribute__((packed));
struct sc_chanp { u8 flags; u8 count; u8 n_aps[2]; struct sc_chan cfg[67]; } __attribute__((packed));
struct sc_sched { u16 interval; u8 iter_count; u8 rsv; } __attribute__((packed));
struct sc_per   { struct sc_sched sched[2]; u16 delay; u16 rsv; } __attribute__((packed));
struct sc_seg   { u16 off; u16 len; } __attribute__((packed));
struct sc_preq  { struct sc_seg mac_hdr; struct sc_seg band[3]; struct sc_seg common; u8 buf[512]; } __attribute__((packed));
struct sc_ssid  { u8 id; u8 len; u8 ssid[32]; } __attribute__((packed));
struct sc_probe { struct sc_preq preq; u8 short_ssid_num; u8 bssid_num; u16 rsv;
                  struct sc_ssid direct[20]; u32 short_ssid[8]; u8 bssid_arr[16][6]; } __attribute__((packed));
struct sc_reqp  { struct sc_gen gen; struct sc_chanp chan; struct sc_per per; struct sc_probe probe; } __attribute__((packed));
struct sc_umac  { u32 uid; u32 ooc; struct sc_reqp p; } __attribute__((packed));
typedef char _sc_umac_sz_check[(sizeof(struct sc_umac) == 1940) ? 1 : -1];

#define SCAN_AP_MAX 24
static struct scan_ap { u8 bssid[6]; u8 chan; u8 ssid_len; char ssid[33]; int seen;
                        u16 bi; u8 dtim; signed char rssi;
                        /* What the beacon says about how this BSS may be
                         * joined.  We used to record none of it and offer
                         * WPA2-PSK to everything, so a WPA3 BSS was not
                         * "rejected" - it was attempted with the wrong AKM
                         * and read back as a bad password. */
                        wpa_ap_sec_t sec; } g_scan_aps[SCAN_AP_MAX];
static int g_scan_ap_n;
/* Which band scan_pick() may choose from: 0 = 2.4 GHz only (the proven path -
 * mvm_phy_ctxt has only ever been driven on band 2.4 / LMAC 0), 1 = 5 GHz only,
 * 2 = either. `iwl band <24|5|any>` flips it live. */
static int g_band_pref;

/* Record one beacon / probe response. `rssi` and `dchan` come from the RX
 * descriptor: dchan is the channel the scanner was PARKED ON, which is ground
 * truth, unlike the DS Parameter Set (IE 3) that 5 GHz APs omit - that omission
 * is why successive scans reported the same BSSID on ch 11 and then ch 1. The
 * DS IE is kept only as a fallback for the descriptor-less (gen1) path. */
static void scan_record_beacon(const u8 *frame, int fl, int rssi, int dchan)
{
    u16 fc; int subtype, ielen, i;
    const u8 *bssid, *ie, *ssid = 0; int ssid_len = 0; u8 chan = 0, dtim = 0;
    u16 bi = 0;
    wpa_ap_sec_t sec;
    memset(&sec, 0, sizeof sec);
    g_scan_beacon_calls++;
    if (g_scan_beacon_calls <= 4)
        uno_dbg_net_trace("wifi: scan rx#%d fl=%d fc=%04x", g_scan_beacon_calls, fl,
                          (fl >= 2) ? (frame[0] | (frame[1] << 8)) : 0);
    if (fl < 36) return;
    fc = (u16)(frame[0] | (frame[1] << 8));
    if (((fc >> 2) & 3) != 0) return;                 /* management frames only */
    subtype = (fc >> 4) & 0xF;
    if (subtype != 8 && subtype != 5) return;         /* beacon (8) / probe-resp (5) */
    bssid = frame + 16;                               /* addr3 */
    bi = (u16)(frame[32] | (frame[33] << 8));         /* beacon interval, TU */
    ie = frame + 36; ielen = fl - 36;                 /* skip ts(8)+bint(2)+cap(2) after the 24 B hdr */
    for (i = 0; i + 2 <= ielen; ) {
        int id = ie[i], ln = ie[i + 1];
        if (i + 2 + ln > ielen) break;
        if (id == 0 && ln <= 32) { ssid = ie + i + 2; ssid_len = ln; }
        else if (id == 3 && ln >= 1) chan = ie[i + 2];
        else if (id == 5 && ln >= 2) dtim = ie[i + 3];   /* TIM: count, PERIOD, ... */
        /* Elements are ordered by id, so 0x30 precedes 0xF4 - but the RSNXE
         * carries the ONE bit that decides which SAE password element we
         * derive, and wpa_parse_rsn_ie() clears the struct.  Keep the H2E flag
         * outside the struct so element order cannot lose it. */
        else if (id == 0x30) { int h = sec.h2e; wpa_parse_rsn_ie(ie + i, ielen - i, &sec); sec.h2e = (u8)h; }
        else if (id == 0xF4) wpa_parse_rsnxe(ie + i, ielen - i, &sec);
        i += 2 + ln;
    }
    if (dchan) chan = (u8)dchan;                      /* descriptor beats the DS IE */
    for (i = 0; i < g_scan_ap_n; i++)
        if (!memcmp(g_scan_aps[i].bssid, bssid, 6)) {
            g_scan_aps[i].seen++;
            if (chan) g_scan_aps[i].chan = chan;
            if (bi) g_scan_aps[i].bi = bi;
            if (dtim) g_scan_aps[i].dtim = dtim;
            if (rssi && rssi > g_scan_aps[i].rssi) g_scan_aps[i].rssi = (signed char)rssi;
            if (sec.has_rsn) g_scan_aps[i].sec = sec;
            return; }
    if (g_scan_ap_n >= SCAN_AP_MAX) return;
    { struct scan_ap *a = &g_scan_aps[g_scan_ap_n++];
      memcpy(a->bssid, bssid, 6); a->chan = chan; a->seen = 1; a->ssid_len = (u8)ssid_len;
      a->bi = bi; a->dtim = dtim; a->rssi = (signed char)rssi; a->sec = sec;
      if (ssid && ssid_len <= 32) { memcpy(a->ssid, ssid, ssid_len); a->ssid[ssid_len] = 0; }
      else a->ssid[0] = 0; }
}

static const u8 g_scan_tmpl[1940] = {
    0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x82,0x00,0x00,0x00,0x0a,0x0a,0x02,0x08,
    0x0a,0x00,0x2c,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x6e,0x6e,0x00,0x00,0x27,0x26,0x0a,0x02,
    0x01,0x00,0x00,0x00,0x01,0x01,0x01,0x00,0x01,0x00,0x00,0x00,0x02,0x01,0x01,0x00,
    0x01,0x00,0x00,0x00,0x03,0x01,0x01,0x00,0x01,0x00,0x00,0x00,0x04,0x01,0x01,0x00,
    0x01,0x00,0x00,0x00,0x05,0x01,0x01,0x00,0x01,0x00,0x00,0x00,0x06,0x01,0x01,0x00,
    0x01,0x00,0x00,0x00,0x07,0x01,0x01,0x00,0x01,0x00,0x00,0x00,0x08,0x01,0x01,0x00,
    0x01,0x00,0x00,0x00,0x09,0x01,0x01,0x00,0x01,0x00,0x00,0x00,0x0a,0x01,0x01,0x00,
    0x01,0x00,0x00,0x00,0x0b,0x01,0x01,0x00,0x01,0x00,0x00,0x00,0x0c,0x01,0x01,0x00,
    0x01,0x00,0x00,0x00,0x0d,0x01,0x01,0x00,0x01,0x00,0x00,0x00,0x24,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x28,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x2c,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x30,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x34,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x38,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x3c,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x40,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x64,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x68,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x6c,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x70,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x74,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x78,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x7c,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x80,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x84,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x88,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x8c,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x90,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x95,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0x99,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x9d,0x00,0x01,0x00,
    0x01,0x00,0x00,0x00,0xa1,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0xa5,0x00,0x01,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x1a,0x00,0x1a,0x00,0x4b,0x00,0x65,0x00,0x54,0x00,
    0xb9,0x00,0x00,0x00,0xb9,0x00,0x09,0x00,0x40,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
    0xff,0xff,0x18,0x26,0x49,0x71,0x91,0x57,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,
    0x00,0x00,0x01,0x08,0x02,0x04,0x0b,0x16,0x0c,0x12,0x18,0x24,0x32,0x04,0x30,0x48,
    0x60,0x6c,0x03,0x01,0x00,0x2d,0x1a,0xef,0x19,0x17,0xff,0xff,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x2c,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0xff,0x1a,0x23,0x01,0x78,0x20,0x9a,0xc0,0xab,0x02,0x3f,0x0e,0x09,0xfd,0x09,
    0x8c,0x16,0x0f,0xf0,0x01,0xfa,0xff,0xfa,0xff,0x61,0x1c,0xc7,0x71,0x01,0x08,0x0c,
    0x12,0x18,0x24,0x30,0x48,0x60,0x6c,0x2d,0x1a,0xef,0x19,0x17,0xff,0xff,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x2c,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0xbf,0x0c,0xf6,0x71,0x90,0x03,0xfa,0xff,0x00,0x00,0xfa,0xff,0x00,
    0x20,0xff,0x1e,0x23,0x01,0x78,0x20,0x8a,0xc0,0xab,0x0c,0x3f,0x0e,0x09,0xfd,0x09,
    0x8c,0x16,0x0f,0xf0,0x01,0xfa,0xff,0xfa,0xff,0xfa,0xff,0xfa,0xff,0x61,0x1c,0xc7,
    0x71,0xdd,0x07,0x00,0x50,0xf2,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
};

/* Verbatim replay of a working Linux SCAN_REQ_UMAC v15 payload captured on THIS
 * card (trace-cmd iwlwifi_dev_hcmd, devbuntu:~/scan_report.txt).  Our hand-built
 * command was dropped silently: per-channel band was 0 (=5GHz) on channels 1-13,
 * gen_flags/cp.flags/n_aps/probe-template all differed.  Replaying the exact
 * bytes removes every field-decode risk - same card, same MAC in the probe
 * template (18:26:49:71:91:57), so it is valid as-is.  38 channels (2.4+5GHz),
 * active wildcard scan, uid 0. */
static int mvm_scan_passive(int dwell_ms)
{
    const u8 *comp;
    g_scan_ap_n = 0; g_scanning = 1; g_scan_mpdu_seen = 0; g_scan_beacon_calls = 0; g_scan_rb_total = 0;
    uno_dbg_net_trace("wifi: scan: pre  rx_closed=%d rx_read=%d", rx_closed() & (RXQ_N-1), g_rx_read);
    send_cmd(GRP_LONG, 0x0d /*SCAN_REQ_UMAC*/, 0, g_scan_tmpl, (int)sizeof g_scan_tmpl);
    /* pump RX so beacons get recorded during the scan; SCAN_COMPLETE_UMAC comes
     * back in the LEGACY group as 0x0f, else we just poll for dwell_ms. */
    comp = wait_notif(GRP_LEGACY, 0x0f, 0, dwell_ms);
    g_scanning = 0;
    uno_dbg_net_trace("wifi: scan: post rx_closed=%d rx_read=%d rb_total=%d",
                      rx_closed() & (RXQ_N-1), g_rx_read, g_scan_rb_total);
    uno_dbg_net_trace("wifi: scan: complete=%s mpdu_seen=%d beacon_calls=%d aps=%d",
                      comp ? "yes" : "no(timeout)", g_scan_mpdu_seen, g_scan_beacon_calls, g_scan_ap_n);
    return g_scan_ap_n;
}

/* Match the configured SSID against the scan results; on success set g_bssid +
 * g_join_chan (prefers the most-often-seen BSSID for that SSID, a rough signal
 * proxy). Returns 0 if the SSID is in range, -1 otherwise. */
static u8 g_join_chan;
static int band_ok(int chan)
{
    if (g_band_pref == 2) return 1;
    if (g_band_pref == 1) return chan > 14;
    return chan >= 1 && chan <= 14;
}
/* The n-th strongest BSS carrying the configured SSID (n = 0 is the strongest),
 * or -1 when there is no n-th one. Split out of scan_pick() so a refused AP can
 * be retried against the next candidate - on a mesh, "strongest" is not the same
 * as "will talk to us" (NimmuNet's e8:d3:eb:47:4e:cf is the loudest BSS on the
 * air here and refuses every auth, while e8:d3:eb:51:8c:8f 36 dB down completes
 * every time). */
static int scan_pick_nth(int n)
{
    unsigned taken = 0;                 /* SCAN_AP_MAX <= 24, one bit each */
    int i, chosen = -1, sl = (int)strlen(g_cfg_ssid), pass, k;
    for (k = 0; k <= n; k++) {
        int best = -1;
        for (pass = 0; pass < 2 && best < 0; pass++)
            for (i = 0; i < g_scan_ap_n; i++) {
                if (taken & (1u << i)) continue;
                if (sl <= 0 || g_scan_aps[i].ssid_len != sl ||
                    memcmp(g_scan_aps[i].ssid, g_cfg_ssid, sl)) continue;
                if (pass == 0 && !band_ok(g_scan_aps[i].chan)) continue;
                if (best < 0 || g_scan_aps[i].rssi > g_scan_aps[best].rssi ||
                    (g_scan_aps[i].rssi == g_scan_aps[best].rssi &&
                     g_scan_aps[i].seen > g_scan_aps[best].seen))
                    best = i;
            }
        if (best < 0) return -1;
        chosen = best;
        taken |= (1u << best);
    }
    return chosen;
}

/* Candidate order for the join: a pinned BSSID first if the scan saw it, then
 * everything else strongest-first. With no pin this is exactly scan_pick_nth,
 * so the stock behaviour is unchanged. */
static int join_pick_nth(int n)
{
    int p = -1, i, idx, want, k;
    if (g_pin_on)
        for (i = 0; i < g_scan_ap_n; i++)
            if (!memcmp(g_scan_aps[i].bssid, g_pin_bssid, 6)) { p = i; break; }
    if (p < 0) return scan_pick_nth(n);
    if (n == 0) return p;
    for (want = n - 1, k = 0; ; k++) {
        idx = scan_pick_nth(k);
        if (idx < 0) return -1;
        if (idx == p) continue;               /* already spent as attempt 1 */
        if (want-- == 0) return idx;
    }
}

/* Point the join at scan result `idx`. */
static void select_ap(int idx)
{
    memcpy(g_bssid, g_scan_aps[idx].bssid, 6);
    g_join_chan = g_scan_aps[idx].chan ? g_scan_aps[idx].chan : 1;
    g_join_bi   = g_scan_aps[idx].bi ? g_scan_aps[idx].bi : 100;
    g_join_dtim = g_scan_aps[idx].dtim ? g_scan_aps[idx].dtim : 1;
    g_ap_sec    = g_scan_aps[idx].sec;
    g_join_akm  = wpa_pick_akm(&g_ap_sec);
    /* A transition-mode AP offers both, we prefer SAE, and when SAE will not
     * complete this is how the second pass asks for the other one. */
    if (g_akm_force && (g_ap_sec.akm & g_akm_force)) g_join_akm = g_akm_force;
    g_join_mfp  = wpa_pick_mfp(&g_ap_sec, g_join_akm);
    uno_dbg_net_trace("wifi: security: akm_offered=%02x pairwise=%02x mfpc=%d mfpr=%d "
                      "h2e=%d -> using %s, mfp=%d",
                      g_ap_sec.akm, g_ap_sec.pairwise, g_ap_sec.mfpc, g_ap_sec.mfpr,
                      g_ap_sec.h2e, akm_name(g_join_akm), g_join_mfp);
}

static int scan_pick(void)
{
    int best = scan_pick_nth(0);
    if (best < 0) return -1;
    select_ap(best);
    uno_dbg_net_trace("wifi: pick: strongest \"%s\" is [%d] %02x:%02x:%02x:%02x:%02x:%02x "
                      "ch=%d rssi=%d seen=%d", g_cfg_ssid, best,
                      g_scan_aps[best].bssid[0], g_scan_aps[best].bssid[1],
                      g_scan_aps[best].bssid[2], g_scan_aps[best].bssid[3],
                      g_scan_aps[best].bssid[4], g_scan_aps[best].bssid[5],
                      g_scan_aps[best].chan, g_scan_aps[best].rssi, g_scan_aps[best].seen);
    return 0;
}


/* SAE authentication frames, one slot per transaction sequence (1 = commit,
 * 2 = confirm).  Each holds the auth body from the algorithm field onward. */
static u8  g_sae_rx[2][512];
static int g_sae_rx_len[2];

/* Stash a management response addressed to us (auth / assoc-resp / deauth /
 * disassoc) so mvm_auth/mvm_assoc can read it. Called from rx_process_rb when a
 * management frame arrives outside a scan. */
static void mgmt_capture(const u8 *frame, int fl, u16 fc)
{
    int st = (fc >> 4) & 0xF;
    if (st != 11 && st != 1 && st != 3 && st != 12 && st != 10) return;
    if (fl < 24 || memcmp(frame + 4, g_mac, 6)) return;   /* addr1 must be us */
    /* SAE needs TWO authentication frames from the AP, and the single-slot
     * g_mgmt_rx below keeps only the newest.  An AP that sends its commit and
     * its confirm back to back - which 802.11 permits - would therefore lose
     * the commit, and the exchange would fail with nothing in the log to say
     * a frame had been overwritten.  Give each sequence number its own slot. */
    if (st == 11 && fl >= 30 && (frame[24] | (frame[25] << 8)) == 3 &&
        !memcmp(frame + 10, g_bssid, 6)) {          /* addr2 = the AP we chose */
        int seq = frame[26] | (frame[27] << 8);
        if (seq == 1 || seq == 2) {
            int n = fl - 24;
            if (n > (int)sizeof g_sae_rx[0]) n = (int)sizeof g_sae_rx[0];
            memcpy(g_sae_rx[seq - 1], frame + 24, (size_t)n);
            g_sae_rx_len[seq - 1] = n;
        }
    }
    if (fl > (int)sizeof g_mgmt_rx) fl = (int)sizeof g_mgmt_rx;
    memcpy(g_mgmt_rx, frame, fl);
    g_mgmt_rx_len = fl; g_mgmt_rx_subtype = (u8)st;
    /* Deauth (12) and disassoc (10) carry a 2-byte reason code after the MAC
     * header, and it is the single most useful byte the AP ever sends us: 1
     * unspecified, 2 previous auth invalid, 4 inactivity, 6/7 class-2/class-3
     * frame from a station the AP does not think is authenticated/associated,
     * 15 4-way timeout.  It used to be dropped on the floor, so a run that
     * ended in a wall of deauths said nothing about WHY. */
    if (st == 12 || st == 10) {
        int reason = fl >= 26 ? (frame[24] | (frame[25] << 8)) : -1;
        int ours = !memcmp(frame + 10, g_bssid, 6);
        /* RATE-LIMIT IT. A DEAUTH WE IGNORE MUST NOT COST US THE DIAGNOSIS.
         *
         * An abandoned mesh candidate sends these once or twice a second,
         * forever. On the Surface that is thousands of identical lines, and
         * the kernel log is a RING: a 29-minute session ended with a boot log
         * whose entire tail was this one message, with the join, the post-join
         * verdict and the RXDATA descriptions - everything we booted that
         * build to collect - rolled out from under it.
         *
         * A diagnostic that evicts the diagnosis is worse than silence,
         * because it looks like data. Log the first few from a given sender,
         * then one line a minute carrying the count, so a persistent talker
         * costs a line a minute instead of a hundred. The counters
         * (g_deauth_ours / g_deauth_other) are unaffected and still see every
         * frame, so the post-join verdict reads exactly as before. */
        {
            static u8  last_src[6];
            static unsigned long long last_ms;
            static u32 burst, hidden;
            unsigned long long now = uno_dbg_uptime_ms();
            int same = !memcmp(last_src, frame + 10, 6);
            if (!same) { memcpy(last_src, frame + 10, 6); burst = 0; hidden = 0; last_ms = 0; }
            if (burst < 4 || now - last_ms >= 60000) {
                char extra[64];
                extra[0] = 0;
                if (hidden)
                    snprintf(extra, sizeof extra, " [+%u more since the last line]",
                             (unsigned)hidden);
                uno_dbg_net_trace("wifi: %s from %02x:%02x:%02x:%02x:%02x:%02x reason=%d%s%s",
                                  st == 12 ? "DEAUTH" : "DISASSOC",
                                  frame[10],frame[11],frame[12],frame[13],frame[14],frame[15],
                                  reason, ours ? " (our AP)" : " (not our BSSID - ignored)",
                                  extra);
                last_ms = now; hidden = 0;
                if (burst < 4) burst++;
            } else {
                hidden++;
            }
        }
        g_deauth_reason = reason;
        if (ours) g_deauth_ours++; else g_deauth_other++;
        /* Acting on it is the point: the association is gone the moment the AP
         * says so, and pretending otherwise is what left the stack retrying
         * DHCP into a dead link for the rest of the run (metal, SURFGO
         * 2026-08-04).  Only our own BSSID may tear us down. */
        if (ours && (g_joined || g_keys_installed)) {
            g_joined = 0; g_keys_installed = 0; g_wpa_active = 0;
            g_link_lost_reason = reason;
            uno_dbg_net_trace("wifi: link DOWN - deauthenticated by the AP "
                              "(reason %d); the Network app must re-join", reason);
        }
        return;
    }
    uno_dbg_net_trace("wifi: mgmt rx subtype=%d len=%d", st, fl);
}

/* Pump the RX ring up to timeout_ms waiting for a captured mgmt frame of
 * `subtype`. 0 on arrival, -1 on timeout. */
static int wait_mgmt(int subtype, int timeout_ms)
{
    int t;
    for (t = 0; t < timeout_ms; t++) {
        rx_pump_once();
        if (g_mgmt_rx_len && g_mgmt_rx_subtype == subtype) return 0;
        mdelay_(1);
    }
    return -1;
}

/* 802.11 Open-System authentication: TX an auth frame (seq 1) on the data
 * queue, wait for the AP auth response (seq 2). 0 = success (status 0),
 * >0 = AP status code, -1 = no response. */
static int mvm_auth(void)
{
    u8 f[30];
    memset(f, 0, sizeof f);
    f[0] = 0xB0;                        /* FC: mgmt(0) / subtype auth(11) */
    memcpy(f + 4, g_bssid, 6);          /* addr1 = BSSID (RA/DA) */
    memcpy(f + 10, g_mac, 6);           /* addr2 = SA (us) */
    memcpy(f + 16, g_bssid, 6);         /* addr3 = BSSID */
    /* seq ctl @22 left 0 (fw stamps it) */
    f[24] = 0; f[25] = 0;               /* auth algorithm = Open System */
    f[26] = 1; f[27] = 0;               /* auth transaction seq = 1 */
    f[28] = 0; f[29] = 0;               /* status code = 0 */
    g_mgmt_rx_len = 0; g_mgmt_diag = 1; g_mgmt_diag_n = 0; g_mgmt_diag_other = 0; g_txresp_n = 0;
    tx_enqueue(f, 30, 1);
    { int rc = wait_mgmt(11, 800); g_mgmt_diag = 0;
      uno_dbg_net_trace("wifi: auth: %d rx pkts seen during wait, mgmt=%d", g_mgmt_diag_n, rc==0);
      if (rc < 0) return -1; }
    return g_mgmt_rx[24 + 4] | (g_mgmt_rx[24 + 5] << 8);   /* auth resp status */
}

/* ---- WPA3: SAE authentication (802.11-2020 12.4) ------------------------
 * Same authentication frames as Open System, algorithm 3 instead of 0, and
 * two round trips instead of one: commit, then confirm.  It runs entirely
 * BEFORE association, and its whole product is the PMK in g_sae.pmk, which
 * then feeds the ordinary 4-way handshake. */
static sae_t g_sae;
static int   g_sae_ready;           /* g_sae.pmk is authenticated             */

/* Send one SAE auth frame. `body` is everything after the 6-byte header. */
static void sae_tx(int seq, int status, const u8 *body, int blen)
{
    u8 f[256];
    int n = 24;
    if (blen > (int)sizeof f - 30) return;
    memset(f, 0, sizeof f);
    f[0] = 0xB0;                                  /* mgmt / auth              */
    memcpy(f + 4, g_bssid, 6);
    memcpy(f + 10, g_mac, 6);
    memcpy(f + 16, g_bssid, 6);
    f[n++] = 3; f[n++] = 0;                       /* algorithm = SAE          */
    f[n++] = (u8)seq; f[n++] = (u8)(seq >> 8);
    f[n++] = (u8)status; f[n++] = (u8)(status >> 8);
    memcpy(f + n, body, (size_t)blen); n += blen;
    tx_enqueue(f, n, 1);
}

/* Pump the RX ring until an SAE frame of transaction `seq` lands. */
static int sae_wait(int seq, int ms)
{
    int t;
    for (t = 0; t < ms; t++) {
        rx_pump_once();
        if (g_sae_rx_len[seq - 1]) return 0;
        mdelay_(1);
    }
    return -1;
}

/* Run the whole SAE exchange against the selected AP.  0 on success (the PMK
 * is authenticated), <0 otherwise. */
/* Derive the password element and draw rand/mask.  SEPARATE from the on-air
 * exchange, and called BEFORE the airtime window is requested, because this is
 * the slow half: hunting-and-pecking runs forty modular exponentiations, and
 * SESSION_PROTECTION_CMD only reserves 900 TU (~922 ms).  Spending any of that
 * on arithmetic that needs no radio would eat into the window the two SAE
 * round trips have to fit inside. */
static int sae_prepare(void)
{
    int rc;
    g_sae_ready = 0;
    g_sae_rx_len[0] = g_sae_rx_len[1] = 0;
    /* h2e is the AP's advertised capability, not our preference: derive the
     * password element the way the AP will, or every subsequent byte differs. */
    rc = sae_init(&g_sae, g_cfg_ssid, (int)strlen(g_cfg_ssid), g_cfg_psk,
                  g_mac, g_bssid, g_ap_sec.h2e);
    if (rc != SAE_OK) {
        uno_dbg_net_trace("wifi: SAE: init failed (%d)%s", rc,
                          rc == SAE_ENOENTROPY
                          ? " - NO USABLE RNG.  SAE is refused rather than run on a"
                            " guessable secret; a wired NIC or a CPU with RDRAND joins."
                          : "");
        return -1;
    }
    uno_dbg_net_trace("wifi: SAE: group 19, PWE by %s",
                      g_ap_sec.h2e ? "hash-to-element" : "hunting-and-pecking");
    return 0;
}

/* The on-air half: commit, then confirm.  sae_prepare() must have run. */
static int mvm_auth_sae(void)
{
    u8 body[256];
    int n, rc, attempt;

    if (g_sae.state != SAE_ST_INIT) {
        uno_dbg_net_trace("wifi: SAE: no prepared state - sae_prepare() did not run");
        return -1;
    }

    /* --- commit.  Two attempts: a busy AP answers the first with status 76
     * and an anti-clogging token, which we echo back in an otherwise
     * IDENTICAL commit (a fresh scalar would restart the exchange). */
    for (attempt = 0; attempt < 2; attempt++) {
        int status;
        g_sae_rx_len[0] = 0;
        n = sae_build_commit(&g_sae, body, sizeof body);
        if (n < 0) { uno_dbg_net_trace("wifi: SAE: commit build failed"); return -1; }
        sae_tx(1, sae_commit_status(&g_sae), body, n);
        if (sae_wait(1, 1200) < 0) {
            uno_dbg_net_trace("wifi: SAE: no commit from the AP within 1200 ms");
            g_sae_silent = 1;   /* the BSS never answered at all */
            return -1;
        }
        status = g_sae_rx[0][4] | (g_sae_rx[0][5] << 8);
        if (status == 76) {                       /* ANTI_CLOGGING_TOKEN_REQUIRED */
            rc = sae_token_from_reject(&g_sae, g_sae_rx[0] + 6, g_sae_rx_len[0] - 6);
            uno_dbg_net_trace("wifi: SAE: AP demanded an anti-clogging token (%d bytes, rc=%d)",
                              g_sae.token_len, rc);
            if (rc != SAE_OK) return -1;
            continue;                             /* re-send the SAME commit */
        }
        if (status != 0 && status != 126) {
            uno_dbg_net_trace("wifi: SAE: AP rejected our commit, status=%d%s", status,
                              status == 77 ? " (unsupported group - we only offer 19)" : "");
            return -1;
        }
        rc = sae_rx_commit(&g_sae, g_sae_rx[0] + 6, g_sae_rx_len[0] - 6);
        if (rc != SAE_OK) {
            uno_dbg_net_trace("wifi: SAE: the AP's commit did not verify (%d)", rc);
            return -1;
        }
        break;
    }
    if (g_sae.state != SAE_ST_COMMITTED) {
        uno_dbg_net_trace("wifi: SAE: commit never completed (token loop exhausted)");
        return -1;
    }

    /* --- confirm.  Re-arm the airtime window first: the commit round trip can
     * have consumed most of the 900 TU on its own, and a confirm transmitted
     * off-channel is simply never heard.  A second SESSION_PROTECTION is
     * accepted by this firmware (the same property the retry path relies on).
     *
     * A wrong passphrase fails HERE, and it fails cleanly: the AP's confirm
     * will not verify.  That is the one honest error this stack can report, so
     * say so rather than letting it read as a timeout. */
    mvm_assoc_window();
    g_sae_rx_len[1] = 0;
    n = sae_build_confirm(&g_sae, body, sizeof body);
    if (n < 0) return -1;
    sae_tx(2, 0, body, n);
    if (sae_wait(2, 1200) < 0) {
        uno_dbg_net_trace("wifi: SAE: no confirm from the AP - it most likely "
                          "rejected our confirm, which means a WRONG PASSPHRASE");
        return -1;
    }
    { int status = g_sae_rx[1][4] | (g_sae_rx[1][5] << 8);
      if (status != 0) {
          uno_dbg_net_trace("wifi: SAE: AP confirm carried status=%d", status);
          return -1;
      } }
    rc = sae_rx_confirm(&g_sae, g_sae_rx[1] + 6, g_sae_rx_len[1] - 6);
    if (rc != SAE_OK) {
        uno_dbg_net_trace("wifi: SAE: the AP's confirm did not verify (%d) - "
                          "WRONG PASSPHRASE, or an active attacker", rc);
        return -1;
    }
    g_sae_ready = 1;
    uno_dbg_net_trace("wifi: SAE: ACCEPTED - PMK established, PMKID "
                      "%02x%02x%02x%02x...", g_sae.pmkid[0], g_sae.pmkid[1],
                      g_sae.pmkid[2], g_sae.pmkid[3]);
    return 0;
}

/* Build + TX an Association Request (capability + listen-interval + SSID +
 * supported/extended rates + WPA2 RSN IE) and wait for the Assoc Response.
 * Returns the AID on success, <0 on failure / timeout. */
static int mvm_assoc(void)
{
    u8 f[256]; int n = 24;
    static const u8 sr[8]  = { 0x82,0x84,0x8b,0x96,0x0c,0x12,0x18,0x24 }; /* 1..18M */
    static const u8 er[4]  = { 0x30,0x48,0x60,0x6c };                     /* 24..54M */
    /* The RSN IE here MUST be byte-identical to the one the supplicant puts in
     * EAPOL 2/4: the authenticator compares them and deauthenticates on any
     * difference. This used to be a private 20-byte literal (RSNE body 0x12)
     * while wpa_build_rsn_ie() emitted 22 bytes (body 0x14, including the
     * 2-byte RSN capabilities field) - so every 4-way died right after our 2/4
     * went out, with the AP sending deauth (metal, 2026-07-28). It is now
     * built ONCE, in wpa_arm(), and both users copy that array - there is no
     * second construction site left to drift. */
    int sl = (int)strlen(g_cfg_ssid);
    memset(f, 0, sizeof f);
    f[0] = 0x00;                        /* FC: mgmt(0) / subtype assoc-req(0) */
    memcpy(f + 4, g_bssid, 6);          /* addr1 = BSSID */
    memcpy(f + 10, g_mac, 6);           /* addr2 = SA */
    memcpy(f + 16, g_bssid, 6);         /* addr3 = BSSID */
    /* assoc-req body @24: capability(2), listen interval(2) */
    f[24] = 0x11; f[25] = 0x00;         /* ESS | Privacy (WPA2/WPA3) */
    f[26] = 0x0a; f[27] = 0x00;         /* listen interval = 10 */
    n = 28;
    f[n++] = 0x00; f[n++] = (u8)sl; memcpy(f + n, g_cfg_ssid, sl); n += sl;   /* SSID */
    f[n++] = 0x01; f[n++] = 8; memcpy(f + n, sr, 8); n += 8;                  /* rates */
    f[n++] = 0x32; f[n++] = 4; memcpy(f + n, er, 4); n += 4;                  /* ext rates */
    memcpy(f + n, g_rsn_ie, (size_t)g_rsn_ie_len); n += g_rsn_ie_len;         /* RSN */
    /* The RSN Extension element tells the AP we derived the password element
     * by hash-to-element.  Element ids ascend, so it goes after the RSNE.  It
     * is NOT part of the element the supplicant echoes in 2/4. */
    n += wpa_build_rsnxe(f + n, (int)sizeof f - n,
                         g_join_akm == WPA_AKM_SAE && g_ap_sec.h2e);
    g_mgmt_rx_len = 0;
    tx_enqueue(f, n, 1);
    if (wait_mgmt(1, 800) < 0) return -1;
    /* assoc resp body @24: capability(2), status(2), aid(2) */
    { u16 status = g_mgmt_rx[24 + 2] | (g_mgmt_rx[24 + 3] << 8);
      if (status != 0) return -(int)(0x1000 | status);
      g_aid = (g_mgmt_rx[24 + 4] | (g_mgmt_rx[24 + 5] << 8)) & 0x3fff;
      return g_aid; }
}

/* Bring up the mac / link / station contexts for the AP already selected into
 * g_bssid + g_join_chan, then allocate the data TX queue. Returns the fw-assigned
 * qid, or <0. csr2808 is logged after each command so a metal trace names the
 * exact one that asserted.
 *
 * Two shapes, chosen by what the fw advertises:
 *  - LINK API (capa 110): MAC_CONFIG -> LINK_CONFIG(ADD) -> PHY_CONTEXT ->
 *    LINK_CONFIG(MODIFY: active + phy + rates + qos) -> STA_CONFIG.
 *  - legacy: PHY_CONTEXT -> MAC_CONTEXT -> BINDING -> ADD_STA.
 * TIME_QUOTA is legacy-only; the link carries the fw's scheduling now. */
static int g_ctx_built;     /* fw mac/link/phy contexts exist (re-ADD asserts) */
static int assoc_setup(void)
{
    u32 h; int q;
    g_ctx_built = 1;
#define TRACE_CSR(what) do { h = r32(CSR_MSIX_HW_INT_CAUSES_AD); \
        uno_dbg_net_trace("wifi: join: after " what " csr2808=%08x", h); } while (0)
    if (fw_has_mld_api()) {
        mld_mac_cfg(1 /*ADD*/, 0 /*not assoc*/, 0);   TRACE_CSR("MAC_CONFIG");
        /* ADD the link with no PHY yet, exactly as iwl_mvm_add_link does */
        mld_link_cfg(1 /*ADD*/, 0, 0 /*phy INVALID*/, 0); TRACE_CSR("LINK_CONFIG ADD");
        mvm_phy_ctxt(g_join_chan, 1 /*ADD*/);          TRACE_CSR("phy_ctxt");
        /* TWO modifies, not one: iwl_mvm_mld_assign_vif_chanctx binds the PHY
         * first with an EMPTY mask while the link is still inactive ("send it
         * first with phy context ID"), and only then activates it. phy_id sits
         * outside the mask system and "can be modified only until the link
         * becomes active" - collapsing the two into a single MODIFY makes the fw
         * ADVANCED_SYSASSERT on cmd 0x0309 (metal, 2026-07-27). */
        mld_link_cfg(2 /*MODIFY*/, 0 /*inactive*/, 1, 0);
                                                       TRACE_CSR("LINK_CONFIG phy-bind");
        if (r32(CSR_MSIX_HW_INT_CAUSES_AD)) return -1;
        mld_link_cfg(2 /*MODIFY*/, 1 /*active*/, 1,
                     LINK_MOD_ACTIVE | LINK_MOD_RATES_INFO);
                                                       TRACE_CSR("LINK_CONFIG activate");
        mld_sta_cfg(g_bssid, 0, 0);                    TRACE_CSR("STA_CONFIG");
    } else {
        mvm_phy_ctxt(g_join_chan, 1 /*ADD*/);          TRACE_CSR("phy_ctxt");
        mvm_mac_ctxt(g_bssid, 0, 0, 1 /*ADD*/);        TRACE_CSR("mac_ctxt");
        mvm_binding(1);                                TRACE_CSR("binding");
        mvm_add_sta(g_bssid, 0, 0);                    TRACE_CSR("add_sta");
    }
    q = mvm_txq_alloc(AP_STA_ID, 15, TXQ_N);
    h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
    uno_dbg_net_trace("wifi: join: txq_alloc -> qid=%d csr2808=%08x g_tx_wr=%d", q, h, g_tx_wr);
    (void)h;                            /* the traces compile out in prod builds */
    return q;
#undef TRACE_CSR
}

/* Tell the fw we are now associated with aid, and arm the WPA2 supplicant. */
/* Arm the WPA2 supplicant. MUST run BEFORE the association request goes out:
 * the AP sends EAPOL 1/4 the instant it has sent the association response, so
 * message 1 is handled inside mvm_assoc()'s own RX wait. Arming afterwards
 * re-ran wpa_sm_init(), which reset the state machine and generated a FRESH
 * SNonce - invalidating the 2/4 we had just transmitted, so the AP saw a bad
 * MIC and deauthed us (metal, 2026-07-28). */
static void wpa_arm(void)
{
    u8 pmk[32];
    wpa_rsn_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.akm = g_join_akm ? g_join_akm : WPA_AKM_PSK;
    cfg.mfp = g_join_mfp;
    if (g_join_akm == WPA_AKM_SAE) {
        /* SAE already ran, pre-association; its PMK is what the 4-way uses,
         * and the PMKID names the PMKSA the AP just cached for us.  Arming on
         * an unauthenticated PMK would produce a 4-way that fails on the MIC,
         * i.e. the WPA2 symptom we are here to stop mistaking for one. */
        if (!g_sae_ready) {
            uno_dbg_net_trace("wifi: wpa_arm: SAE has not accepted - refusing to arm");
            g_wpa_active = 0;
            return;
        }
        memcpy(pmk, g_sae.pmk, 32);
        cfg.pmkid = g_sae.pmkid;
    } else {
        wpa_pmk_from_psk(g_cfg_ssid, (int)strlen(g_cfg_ssid), g_cfg_psk, pmk);
    }
    g_rsn_ie_len = wpa_build_rsn_ie(g_rsn_ie, (int)sizeof g_rsn_ie, &cfg);
    if (g_rsn_ie_len < 0) g_rsn_ie_len = 0;
    wpa_sm_init(&g_wpa, pmk, g_mac, g_bssid, cfg.akm, g_rsn_ie, g_rsn_ie_len);
    memset(pmk, 0, sizeof pmk);
    g_wpa_active = 1; g_keys_installed = 0;
    strncpy(g_ssid_str, g_cfg_ssid, sizeof g_ssid_str - 1);
    uno_dbg_net_trace("wifi: %s supplicant armed for \"%s\" (mfp=%d, RSNE %d bytes)",
                      akm_name(cfg.akm), g_cfg_ssid, cfg.mfp, g_rsn_ie_len);
}

/* Tell the fw we are associated. Deliberately does NOT touch the supplicant. */
static void assoc_mark_associated(int aid)
{
    if (fw_has_mld_api()) {
        mld_mac_cfg(2 /*MODIFY*/, 1 /*assoc*/, aid);
        mld_sta_cfg(g_bssid, aid, 0 /*not authorized until the 4-way ends*/);
    } else {
        mvm_mac_ctxt(g_bssid, 1, aid, 2 /*MODIFY*/);
    }
}

/* Join the AP already selected into g_bssid / g_join_chan: mac/link/sta setup ->
 * airtime -> Open-System auth -> assoc -> pump the RX ring until the 4-way
 * handshake has installed the CCMP keys. Returns 0 once associated (g_joined
 * says whether the keys made it), <0 on a failure that leaves nothing joined.
 *
 * The 4-way is pumped HERE, not left to the caller: every caller that waits for
 * link (ip_suite, net_dhcp_after_link, the Network app) polls link() and not
 * recv(), so a handshake left in flight would never be driven to completion. */
static int join_selected(void)
{
    int q, r;
    q = assoc_setup();
    if (q < 0) { uno_dbg_net_trace("wifi: join: no TX queue - cannot auth"); return -1; }

    /* An AP whose beacon offers nothing we speak is a hard stop, and saying so
     * is the whole point: attempting WPA2-PSK against a WPA3-only BSS produced
     * a deauthentication that read exactly like a wrong password. */
    if (!g_join_akm) {
        uno_dbg_net_trace("wifi: join: \"%s\" offers AKM %02x / pairwise %02x - "
                          "nothing this supplicant can speak (we do WPA2-PSK and "
                          "WPA3-SAE over CCMP)", g_cfg_ssid,
                          g_ap_sec.akm, g_ap_sec.pairwise);
        return -1;
    }
    /* SAE's password element costs tens of milliseconds and needs no radio, so
     * derive it BEFORE reserving airtime rather than out of it. */
    prog("Authenticating", 3);
    if (g_join_akm == WPA_AKM_SAE && sae_prepare() < 0) return -1;

    /* Reserve on-channel airtime for the auth exchange. This fw has no
     * TIME_EVENT_CMD (it SYSASSERTs); SESSION_PROTECTION_CMD is the one it
     * speaks, and it now has a real link to reference. */
    mvm_assoc_window();
    if (r32(CSR_MSIX_HW_INT_CAUSES_AD)) {
        uno_dbg_net_trace("wifi: join: session-prot asserted the fw (iwl fwerr)");
        return -1;
    }
    if (g_join_akm == WPA_AKM_SAE) {
        r = mvm_auth_sae();
        g_auth_ok = (r == 0);
        uno_dbg_net_trace("wifi: join: SAE auth -> %d", r);
        if (r != 0) return -1;
    } else {
        r = mvm_auth();
        g_auth_ok = (r == 0);
        uno_dbg_net_trace("wifi: join: auth -> %d (0=ok, >0 AP status, -1 no resp)", r);
        if (r != 0) return -1;
    }
    wpa_arm();                  /* before the request: EAPOL 1/4 lands inside mvm_assoc() */
    /* Two SAE round trips will have outlived the window the Open-System path
     * only had to fit one into.  Re-arm before the association request. */
    if (g_join_akm == WPA_AKM_SAE) mvm_assoc_window();
    prog("Joining the network", 4);
    r = mvm_assoc();
    uno_dbg_net_trace("wifi: join: assoc -> %d (>=0 AID)", r);
    if (r < 0) return -1;
    assoc_mark_associated(r);
    uno_dbg_net_trace("wifi: join: associated with \"%s\" aid=%d - pumping RX for the 4-way",
                      g_cfg_ssid, r);
    prog("Exchanging keys", 5);
    { int ms = rx_pump_ms(4000, 1);
      uno_dbg_net_trace("wifi: join: 4-way %s after %d ms (keys=%d)",
                        g_joined ? "COMPLETE" : "did NOT complete", ms, g_keys_installed); }
    return 0;
}

/* Re-point the live contexts at a different BSS, without restarting the radio.
 *
 * This is the retry path. Restarting the radio is NOT a recovery on this fw - it
 * reloads, reports ALIVE, then SW_ERRs on the next command - whereas re-pointing
 * keeps it healthy: STA_CONFIG accepts a new peer (it has no add/modify flag,
 * re-sending updates it) and a second SESSION_PROTECTION is accepted too. The
 * one thing that does not survive is the TX queue, hence the free + realloc.
 * Returns 0 when the station is ready to auth again. */
static int retarget_ap(int new_chan)
{
    int q;
    if (!fw_has_mld_api()) return -1;              /* legacy path has no equivalent */
    if (new_chan && new_chan != g_phy_chan) {
        mvm_phy_ctxt(new_chan, 2 /*MODIFY*/);
        if (r32(CSR_MSIX_HW_INT_CAUSES_AD)) return -1;
    }
    /* The old association's keys are still on this station - drop them BEFORE
     * re-pointing it, or the hw decrypts the new AP's frames with the wrong key
     * and every one of them arrives as garbage. */
    if (g_keys_installed) {
        mld_sec_key_remove(0, 0);                  /* pairwise */
        if (g_key_gtk_idx >= 0) mld_sec_key_remove(g_key_gtk_idx, 1);
        g_keys_installed = 0; g_key_gtk_idx = -1;
        if (r32(CSR_MSIX_HW_INT_CAUSES_AD)) {
            uno_dbg_net_trace("wifi: retarget: SEC_KEY remove asserted the fw");
            return -1;
        }
    }
    g_joined = 0; g_wpa_active = 0;                /* leaving the old BSS */
    sae_clear(&g_sae); g_sae_ready = 0;            /* and its SAE secrets */
    g_link_lost_reason = -1;                       /* a new attempt, not the old failure */
    /* REMOVE the station and ADD a fresh one, rather than re-pointing the live
     * one at the next peer. Re-pointing left the hardware transmitting happily
     * under the new association's key while decrypting nothing it received
     * (metal, SURFGO 2026-08-05: SEC=UNKNOWN on every reply the AP sent) -
     * the receive-side address match is written when a station is ADDED.
     *
     * Order is the reference driver's teardown order: the TX queue belongs to
     * the station, so it is released while its owner still exists, and only
     * then is the station dropped. */
    mvm_txq_free(AP_STA_ID, 15);
    mld_sta_remove();
    if (r32(CSR_MSIX_HW_INT_CAUSES_AD)) {
        uno_dbg_net_trace("wifi: retarget: STA_REMOVE asserted the fw - only a reboot recovers");
        return -1;
    }
    mld_sta_cfg(g_bssid, 0, 0);                    /* a NEW station for the new peer */
    if (r32(CSR_MSIX_HW_INT_CAUSES_AD)) return -1;
    q = mvm_txq_alloc(AP_STA_ID, 15, TXQ_N);
    uno_dbg_net_trace("wifi: retarget: fresh TX queue -> qid=%d csr2808=%08x",
                      q, r32(CSR_MSIX_HW_INT_CAUSES_AD));
    if (q < 0 || r32(CSR_MSIX_HW_INT_CAUSES_AD)) return -1;
    return 0;
}

/* Restart the radio: halt the running fw, re-load it, and redo the post-ALIVE
 * init - WITHOUT joining. Needed between join attempts: a failed attempt leaves
 * live MAC/LINK/PHY contexts and the fw asserts if they are added again. */
static int radio_restart(void)
{
    u32 h;
    if (g_bar && g_fw_loaded) {
        device_stop();
        /* Clear the latched MSI-X causes before reloading. Bit 25 of
         * CSR_MSIX_HW_INT_CAUSES_AD is SW_ERR (the fw-assert flag) and bit 0 is
         * ALIVE; neither is ever cleared otherwise, so a stale one makes
         * wait_alive believe a fresh ALIVE arrived and makes the health check
         * report an assert that already happened. */
        w32(CSR_MSIX_HW_INT_CAUSES_AD, 0xFFFFFFFFu);
        w32(CSR_MSIX_FH_INT_CAUSES_AD, 0xFFFFFFFFu);
        w32(CSR_INT, 0xFFFFFFFFu);
    }
    g_bound = 0; g_joined = 0; g_alive = 0; g_mvm_done = 0; g_ctx_built = 0;
    g_keys_installed = 0; g_key_gtk_idx = -1; g_wpa_active = 0; g_data_qid = -1;
    g_mvm_arm = 1; g_no_join = 1;
    iwl_nic();
    g_no_join = 0;
    if (!g_alive || !g_mvm_done) return -1;
    /* METAL, round 25: the reload reports ALIVE but the fw then SW_ERRs on the
     * first command after it (csr2808 = 0x02000000 the moment MAC_CONFIG goes
     * out), i.e. restarting the radio on top of a half-finished association is
     * NOT a working recovery - only a reboot is. Check for it rather than
     * spraying commands at a dead firmware. */
    h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
    if (h) {
        uno_dbg_net_trace("wifi: radio restart came up ASSERTED (csr2808=%08x) - "
                          "only a reboot recovers this; giving up on retries", h);
        return -1;
    }
    return 0;
}

/* A second (or third) attempt at a different BSS, re-using the live contexts. */
static int join_retry(void)
{
    int r;
    if (retarget_ap(g_join_chan) < 0) {
        uno_dbg_net_trace("wifi: join: could not re-point the contexts at the next BSS");
        return -1;
    }
    /* The retry is a join to a DIFFERENT BSS, so it authenticates the way that
     * BSS asks for - select_ap() has already re-read its beacon into
     * g_join_akm.  Retrying an SAE network with Open-System auth is the shape
     * of bug that would have made the whole feature look intermittent. */
    if (!g_join_akm) {
        uno_dbg_net_trace("wifi: join: retry BSS offers no AKM we speak");
        return -1;
    }
    if (g_join_akm == WPA_AKM_SAE && sae_prepare() < 0) return -1;
    mvm_assoc_window();
    if (r32(CSR_MSIX_HW_INT_CAUSES_AD)) {
        uno_dbg_net_trace("wifi: join: session-prot asserted the fw on the retry");
        return -1;
    }
    r = (g_join_akm == WPA_AKM_SAE) ? mvm_auth_sae() : mvm_auth();
    g_auth_ok = (r == 0);
    uno_dbg_net_trace("wifi: join: retry %s auth -> %d", akm_name(g_join_akm), r);
    if (r != 0) return -1;
    wpa_arm();
    if (g_join_akm == WPA_AKM_SAE) mvm_assoc_window();
    r = mvm_assoc();
    uno_dbg_net_trace("wifi: join: retry assoc -> %d (>=0 AID)", r);
    if (r < 0) return -1;
    assoc_mark_associated(r);
    { int ms = rx_pump_ms(4000, 1);
      uno_dbg_net_trace("wifi: join: retry 4-way %s after %d ms (keys=%d)",
                        g_joined ? "COMPLETE" : "did NOT complete", ms, g_keys_installed); }
    return 0;
}

/* Full connect: scan once, then try the SSID's BSSs strongest-first until one
 * of them actually completes a 4-way.
 *
 * One scan, several attempts, and NO rescan between them: the fw's scanner does
 * not survive the radio restart each retry needs (two scans in a row come back
 * with 0 APs), but the candidate list from the first scan does - it lives in
 * g_scan_aps, which only a new scan clears. The retry exists because "loudest"
 * and "will associate" are different properties on a mesh: NimmuNet's
 * e8:d3:eb:47:4e:cf is the strongest BSS on the air, ACKs the auth frame at the
 * MAC layer, never answers it, and deauths after the assoc request - every
 * time. Picking on RSSI alone made a stock join a coin flip. */
#define JOIN_TRIES 3

/* Try the network the way it prefers, and if that cannot be made to work, try
 * it the other way it offered.
 *
 * wpa_pick_akm() takes SAE whenever an AP offers it, transition mode included,
 * and the reason is good: the passphrase is the same and the PMK is not
 * grindable offline from a captured handshake. But "prefer" is not "only". On
 * the Surface Laptop Go, against an AP advertising akm=PSK|SAE with MFP
 * capable and NOT required - textbook WPA2/WPA3 transition - the SAE commit
 * goes out, the hardware reports it transmitted with no failures, and the AP
 * never answers. Three BSSs, same result, so the network is simply not
 * completing SAE with us.
 *
 * Refusing to fall back turns "our SAE has a bug against this AP" into "this
 * machine has no network", which is the worse outcome when the AP is still
 * advertising the WPA2 it would accept. So: SAE first, always. PSK second, and
 * only when the AP offered it. An SAE-only network is unaffected - there is
 * nothing to fall back TO and the second pass is skipped. */
static int find_and_join(void)
{
    int attempt, idx, bss = 0, psk_turn = 0;
    g_akm_force = 0;
    prog("Looking for the network", 2);
    mvm_scan_cfg();
    mvm_scan_passive(5000);
    read_bssid_override();
    if (g_pin_on)
        uno_dbg_net_trace("wifi: join: bssid= pins the first attempt to "
                          "%02x:%02x:%02x:%02x:%02x:%02x",
                          g_pin_bssid[0],g_pin_bssid[1],g_pin_bssid[2],
                          g_pin_bssid[3],g_pin_bssid[4],g_pin_bssid[5]);
    for (attempt = 0; attempt < JOIN_TRIES; attempt++) {
        idx = join_pick_nth(bss);
        if (idx < 0) {
            uno_dbg_net_trace(bss ? "wifi: join: no further \"%s\" candidate after %d BSS"
                                  : "wifi: join: SSID \"%s\" not found in %d scanned APs",
                              g_cfg_ssid, bss ? bss : g_scan_ap_n);
            return -1;
        }
        /* SAE first on a BSS, then - if the AP also offered PSK - the SAME
         * BSS with PSK, before moving to the next one. Varying the AKM
         * before the BSS matters because of what the firmware does under
         * repeated attempts: on 2026-08-23 three SAE tries against three
         * BSSs of one network ended with "session-prot asserted the fw", and
         * after a firmware assert every scan returns rb_total=0 and the
         * radio is dead until reboot. A fallback that waits for all the BSSs
         * to fail therefore never runs at all. It is also the better
         * ordering on its own merits: "the AP never answered our commit" is
         * a property of the AKM, not of the BSS, and the evidence agreed -
         * all three BSSs failed identically. */
        g_akm_force = psk_turn ? WPA_AKM_PSK : 0;
        g_sae_silent = 0;
        g_auth_ok = 0;
        select_ap(idx);
        uno_dbg_net_trace("wifi: join: try %d/%d \"%s\" bssid %02x:%02x:%02x:%02x:%02x:%02x "
                          "chan %d rssi %d bi %d dtim %d (link-api=%d)",
                          attempt + 1, JOIN_TRIES, g_cfg_ssid,
                          g_bssid[0],g_bssid[1],g_bssid[2],g_bssid[3],g_bssid[4],g_bssid[5],
                          g_join_chan, g_scan_aps[idx].rssi, g_join_bi, g_join_dtim,
                          fw_has_mld_api());
        if (attempt == 0) { if (join_selected() == 0 && g_joined) { g_akm_force = 0; return 0; } }
        else               { if (join_retry() == 0 && g_joined) { g_akm_force = 0; return 0; } }
        uno_dbg_net_trace("wifi: join: %02x:%02x:%02x:%02x:%02x:%02x did not complete",
                          g_bssid[0],g_bssid[1],g_bssid[2],g_bssid[3],g_bssid[4],g_bssid[5]);
        /* Get this attempt onto the disk NOW. unolog flushes from the shell
         * main loop, and the join IS the shell main loop for as long as it
         * runs - so nothing is written for the whole join, and a machine that
         * wedges mid-join takes every record with it. That is exactly what
         * happened on 2026-08-24: the log ended at the scan and the 21-round
         * handshake loop that froze the box left no trace of itself. */
        { int unolog_flush(void); (void)unolog_flush(); }
        /* What varies next: the AKM on this BSS if the other one is on offer
         * and we have not spent it yet, otherwise the BSS. */
        /* ...but NOT when the BSS never answered. "No commit within 1200 ms"
         * says this radio is not talking to us at all, and asking it the same
         * question in a different dialect wastes the one thing that is scarce
         * here: attempts. NimmuNet has such a BSS - e8:d3:eb:.. ACKs the auth
         * frame at the MAC layer and never answers it, which METAL-FINDINGS
         * recorded long before this - and burning attempt 2 on it meant the
         * BSS that DOES complete SAE was only reached on the last try, with
         * nothing left when its association needed a second go. Fall back on
         * a BSS that TALKED and refused; skip to the next one when it did
         * not talk. */
        /* ...and only when SAE failed AT AUTHENTICATION. If the AP
         * authenticated us and the join died later - association, the 4-way,
         * or a deauthentication afterwards - then the AKM demonstrably works
         * and the BSS is the problem, so the next BSS is the useful move.
         *
         * NimmuNet again, 2026-08-24: SAE authenticated, association returned
         * AID 1, the 4-way completed with keys installed, and the AP then sent
         * reason 7 - from e8:d3:eb:47:4e:cf, the BSS this file already
         * describes two screens up as the loudest on the air and the one that
         * "deauths after the assoc request - every time". Retrying it under a
         * different AKM spent the attempt that the BSS rotation needed, and
         * the rotation is the entire reason the retry loop exists. */
        if (!psk_turn && g_join_akm == WPA_AKM_SAE && !g_sae_silent &&
            !g_auth_ok && (g_ap_sec.akm & WPA_AKM_PSK)) {
            psk_turn = 1;
            uno_dbg_net_trace("wifi: join: SAE did not complete and this AP also "
                              "offers WPA2-PSK - same BSS, the other AKM");
        } else {
            psk_turn = 0;
            bss++;
        }
        if (attempt + 1 >= JOIN_TRIES) break;
        if (r32(CSR_MSIX_HW_INT_CAUSES_AD)) {
            /* the fw died somewhere in that attempt; re-pointing cannot work and
             * a restart comes up asserted too, so stop rather than pretend */
            uno_dbg_net_trace("wifi: join: fw asserted - only a reboot recovers; giving up");
            return -1;
        }
        /* select_ap() for the next candidate happens at the top of the loop;
         * the contexts stay live and get re-pointed there. */
    }
    return -1;
}

/* =====================================================================
 * 13. uno_nic_t: send / recv / link
 * ===================================================================== */
static uno_nic_t g_nic;

static int iwl_send(void *ctx, const void *pkt, int len)
{
    u8 tx80211[2048];
    int n;
    (void)ctx;
    if (!g_bound || !g_joined || len <= 0 || len > 1514) return -1;
    n = eth_to_80211((const u8 *)pkt, len, tx80211);
    /* Wrap in a TX_CMD on the data queue; the card encrypts from the installed
       CCMP key. gen1/gen2/gen3 TX_CMD layouts differ (fwapi ref Part 6). */
    tx_enqueue(tx80211, n, 0);
    g_tx_data_n++;
    return len;
}

static void postjoin_diag(void);        /* defined below; called from here too */

static int iwl_recv(void *ctx, void *pkt, int cap)
{
    (void)ctx;
    if (!g_bound) return 0;
    /* pump the RX ring so notifications (EAPOL, data, mgmt) get processed;
       rx_process_rb dispatches data frames to handle_data_frame and EAPOL to
       handle_eapol (which drives the 4-way handshake + key install). */
    rx_pump_once();
    /* THE DIAGNOSIS HAS TO HANG OFF A CALL THE STACK ACTUALLY MAKES.
     *
     * postjoin_diag() was hooked into iwl_link() alone, on the reasoning that
     * the IP stack polls the link throughout a DHCP wait. It does not: the
     * Control Panel's renew loop is `net_dhcp_start()` then `net_poll()` in a
     * tight loop, and net_poll drives recv, not link. So on the Surface the
     * verdict never fired ONCE, through a successful join and a hundred
     * seconds of DHCP retries - the whole window it exists to describe.
     *
     * recv is polled on every single pass of that loop, so putting it here
     * means the diagnostic reports whenever DHCP is actually running, which is
     * the only time anybody wants it. It is idempotent and one-shot
     * (g_postjoin_diag_done), so the extra call site costs a predictable
     * branch and cannot double-report. */
    postjoin_diag();
    if (g_dq_tail != g_dq_head) {
        int n = g_dataq[g_dq_tail].len;
        if (n > cap) n = cap;
        memcpy(pkt, g_dataq[g_dq_tail].buf, n);
        g_dq_tail = (g_dq_tail + 1) % DATAQ;
        return n;
    }
    return 0;
}

/* Link = associated AND keyed. The 4-way handshake runs out of the RX pump, and
 * the caller that waits for link (ip_suite, net_dhcp_after_link) does NOT poll
 * recv while it waits - so pump here, or a handshake still in flight can never
 * finish and the link never comes up. */
/* ONE line, once, six seconds after a successful join, that says which of the
 * two "associated but no DHCP lease" explanations this machine is actually
 * suffering from.
 *
 * The two have been indistinguishable from the outside all week:
 *   (a) the AP is not treating our data frames as part of its BSS, so it
 *       accepts them at the radio and drops them above it;
 *   (b) a mesh is steering us off the weak BSS it keeps giving us, and our
 *       frames are not reliably getting there at all.
 * They call for completely different work, and I have twice guessed and been
 * wrong, so this measures instead. Three counters decide it:
 *
 *   ack_fail  the AP is not ACKING us at all      -> (b), radio-level
 *   rx_from_ap==0 with ack_fail==0                -> (a), it hears us and
 *                                                    will not answer
 *   rx_from_ap>0                                  -> neither; the link works
 *                                                    and DHCP itself is at fault
 *
 * iwl_link() is the hook because the IP stack polls it throughout the DHCP
 * wait, so this needs no timer, no tick registration and no URC - and it lands
 * in the NET log, which is the only channel this machine reliably has. */
static void postjoin_diag(void)
{
    unsigned long long now = uno_dbg_uptime_ms();
    const char *reading;
    if (g_postjoin_diag_done || !g_joined || !g_join_ms) return;
    /* 2.5 s, not 6. DHCP DISCOVER retries at roughly 1.5 s intervals, so by
     * here two or three have gone out and the counters have said what they are
     * going to say - and the FIRST attempt at this waited 6 s and never fired,
     * because the captured log ended 3.2 s after the join. A diagnostic that
     * outlives its own capture window measures nothing. */
    if (now - g_join_ms < 2500) return;
    g_postjoin_diag_done = 1;

    if (g_tx_data_n && !g_txr_total)
        /* THE CASE THIS DID NOT HAVE A WORD FOR, AND IT IS THE ONE ON THE
         * SURFACE. Every branch below reasons about the RATIO of ack_fail to
         * txresp, which silently assumes txresp > 0. When the queue produces
         * no completion at all the ratio test is false, rx_from_ap is 0, and
         * it reported "it hears us and will not answer" - an association-level
         * verdict for a fault that never reached the air.
         *
         * Frames enqueued and NOTHING coming back is a different animal: the
         * transmit path itself is not completing. On that machine every
         * management frame after the retarget got a TXRESP (auth, assoc, all
         * four EAPOL) and not one DHCP DISCOVER did, which is the shape this
         * names. */
        reading = "frames were QUEUED and the hw returned no transmit response "
                  "at all - the TX path is not completing, this is not the AP";
    else if (g_txr_total && g_txr_ackfail * 2 >= g_txr_total)
        reading = "the AP is NOT ACKing us - radio-level: wrong BSS, steering, or too weak";
    else if (!g_rx_from_ap)
        reading = "our frames are ACKed but the AP sends us NOTHING - it hears us "
                  "and will not answer (association identity)";
    else if (g_rx_prot && !g_rx_prot_dec)
        /* The AP is talking to us and the hardware is handing its frames up
         * still encrypted. Which of the two that is - a key the fw never armed,
         * or one it armed and could not use - is in the status word on the
         * `prot` line below: sec=0 means it never treated the frame as
         * encrypted at all, sec=2 with mic=0 means it tried and the key is
         * wrong. Nothing above this point could tell those apart. */
        reading = "the AP IS sending to us and every PROTECTED frame arrives "
                  "UNDECRYPTED - read the sta/key/sec bits on the prot line";
    else
        reading = "traffic flows BOTH ways - the link is fine and DHCP itself is the problem";

    uno_dbg_net_trace("wifi: post-join diag +%ds: bssid %02x:%02x:%02x:%02x:%02x:%02x aid %d rssi %d",
                      (int)((now - g_join_ms) / 1000),
                      g_bssid[0],g_bssid[1],g_bssid[2],g_bssid[3],g_bssid[4],g_bssid[5],
                      (int)g_aid, g_last_rssi);
    uno_dbg_net_trace("wifi: post-join diag: tx=%u txresp=%u ack_fail=%u | rx=%u from_ap=%u drop=%u "
                      "| deauth ours=%u other=%u last_reason=%d",
                      (unsigned)g_tx_data_n, (unsigned)g_txr_total, (unsigned)g_txr_ackfail,
                      (unsigned)g_rx_data_n, (unsigned)g_rx_from_ap, (unsigned)g_rx_data_drop,
                      (unsigned)g_deauth_ours, (unsigned)g_deauth_other, g_deauth_reason);
    /* `drop` above now counts only frames from the AP we joined; `foreign` is
     * everything else on the channel, which on a mesh is most of it and used to
     * be indistinguishable. `prot` is how many protected frames our AP sent us
     * and how many of them the hardware decrypted - the fact the whole "no
     * lease" question turns on - and st is the last undecrypted one's status. */
    uno_dbg_net_trace("wifi: post-join diag: prot=%u dec=%u foreign=%u st=%08x "
                      "(sta=%d key=%d sec=%d mic=%d dec=%d staid=%d)",
                      (unsigned)g_rx_prot, (unsigned)g_rx_prot_dec, (unsigned)g_rx_foreign,
                      (unsigned)g_rx_st_undec,
                      (int)((g_rx_st_undec >> 2) & 1), (int)((g_rx_st_undec >> 3) & 1),
                      (int)((g_rx_st_undec >> 8) & 7), (int)((g_rx_st_undec >> 6) & 1),
                      (int)((g_rx_st_undec >> 11) & 1), (int)((g_rx_st_undec >> 24) & 0x1f));
    uno_dbg_net_trace("wifi: post-join diag: -> %s", reading);

    /* PERSIST IT NOW, rather than hoping the boot log gets written later.
     *
     * These lines go to the kernel ring; the ring reaches the stick only when
     * something writes BOOTLOG.TXT, and post-boot nothing routinely does. The
     * NETLOG sink cannot help either - it only flushes while the boot net test
     * is active. So the first version of this diagnostic could have fired
     * perfectly and still left no trace on the medium we read it from.
     *
     * The whole point of this round is that ONE boot produces the answer, and
     * on a machine whose stick gets pulled while it is still running, that
     * means writing it out at the moment it is known.
     *
     * Debug-only: uno_debug.h DECLARES uno_dbg_write_bootlog unconditionally but
     * the release build does not define it, so an unguarded call links fine at
     * UNO_DEBUG=1 and fails at 0. The trace calls above already compile to
     * nothing in release, so there is nothing to persist there anyway. */
#if UNO_DEBUG
    uno_dbg_write_bootlog();
#endif
}

static int iwl_link(void *ctx)
{
    (void)ctx;
    if (!g_bound) return 0;
    if (!g_joined && g_wpa_active) rx_pump_ms(50, 1);
    postjoin_diag();               /* one line, once, 2.5 s after joining */
    return g_joined;
}

/* Publish the family nic service. Idempotent; called both from the bring-up and
 * from the handshake completion (a join driven by the debug verbs). */
static void nic_publish(void)
{
    g_nic.ctx = 0; g_nic.send = iwl_send; g_nic.recv = iwl_recv; g_nic.link = iwl_link;
    g_bound = 1;
}

/* =====================================================================
 * 14. bring-up entry points
 * ===================================================================== */
static int load_pnvm(int vol)
{
    long n;
    if (!g_pnvmfile[0]) return 0;      /* AC/AX200: no PNVM */
    n = uno_fs_read(vol, g_pnvmfile + 9 /*strip FIRMWARE\\*/, g_pnvmbuf, PNVM_MAX);
    if (n <= 0) { /* try full path */ n = uno_fs_read(vol, g_pnvmfile, g_pnvmbuf, PNVM_MAX); }
    g_pnvm_len = n > 0 ? n : 0;
    uno_dbg_net_trace("wifi: pnvm %s: %s (%ld bytes)%s", g_pnvmfile,
                      g_pnvm_len ? "loaded" : "NOT FOUND", g_pnvm_len,
                      (!g_pnvm_len && g_family >= FAM_AX210)
                          ? " - AX210+ fw will not finish init without it" : "");
    return n > 0 ? 0 : -1;
}

int iwl_present(void)
{
    pci_dev d;
    if (g_present) return 1;
    /* Intel WiFi = vendor 0x8086, class 0x02 (network) subclass 0x80 (other).
       Match by our device table. */
    if (pci_find_class(0x02, 0x80, &d)) {
        u16 dev = pci_cfg_read16(&d, 2);
        if (d.vendor == 0x8086) {
            g_devid = dev;
            if (identify_by_pci(dev)) { g_pci = d; g_present = 1; return 1; }
            if (g_is_dvm) st_set("Intel WiFi found, but it is an older iwldvm card (unsupported)");
        }
    }
    return 0;
}

static int iommu_disable(char *out, int cap);   /* defined after iwl_nic (F12 fix) */

/* Bring the card from "BAR0 mapped, image parsed" to "firmware ALIVE".
 *
 * Split out of iwl_nic() so a candidate ucode that never posts ALIVE can be
 * swapped for the next one and the whole sequence re-run - see the loop in
 * iwl_nic().  The return value distinguishes the two failure kinds, because
 * only one of them is worth retrying with a different image:
 *
 *    0  ALIVE
 *   -1  no ALIVE - the image is a candidate for being the wrong one
 *   -2  fatal and image-independent (RF-kill, ownership, APM, loader) - a
 *       different stepping cannot help, so the caller must stop rather than
 *       burn ~4 s per blob re-proving the same hardware problem. */
#define BRINGUP_ALIVE  0
#define BRINGUP_NOFW  -1
#define BRINGUP_FATAL -2
static int bringup_to_alive(void)
{
    if (prepare_card_hw() < 0) {
        st_set("WiFi: card not ready (ME owns it?)");
        uno_dbg_net_trace("wifi: FAIL prepare_card_hw timeout - CSR handshake refused (ME/CNVi ownership?)");
        return BRINGUP_FATAL;
    }
    device_stop();                   /* Linux loads always stop the device first */
    clear_persistence_bit();         /* BEFORE the reset (Linux start_hw order) */
    sw_reset();                      /* now retakes ownership afterwards */
    if (g_family == FAM_22000 && g_devid != 0x2723) {   /* integrated CNVi (all AX201s) */
        if (force_power_gating() < 0)
            uno_dbg_net_trace("wifi: force-power-gating clock-ready timeout (continuing)");
        else
            uno_dbg_net_trace("wifi: CNVi force-power-gating done (HPM_HIPM readback=%08x)",
                              prph_r(HPM_HIPM_GEN_CFG));
    }
    w32(CSR_INT, 0xFFFFFFFFu);
    if (rf_killed()) {
        st_set("WiFi: hardware RF-kill is on");
        uno_dbg_net_trace("wifi: FAIL hardware RF-kill asserted (airplane-mode key/switch)");
        return BRINGUP_FATAL;
    }
    w32(CSR_UCODE_DRV_GP1_CLR, 0x00000002);   /* clear SW rfkill handshake */
    w32(CSR_UCODE_DRV_GP1_CLR, 0x00000004);   /* clear cmd-blocked */

    if (apm_init() < 0) {
        st_set("WiFi: APM init timeout");
        uno_dbg_net_trace("wifi: FAIL APM init timeout (clock-ready never came up)");
        return BRINGUP_FATAL;
    }
    uno_dbg_net_trace("wifi: card hw ready (APM up, no rfkill)");
    if (g_gen2) { msix_enable_pci(); conf_msix();
                  if (g_msix_arm) msix_table_setup(); }  /* PCI MSI-X + BAR0 MSI-X - REQUIRED for the ROM to start */
    else if (g_mq_rx) prph_w(uprph(UREG_CHICK), UREG_CHICK_MSI);
    nic_config_radio();              /* Linux order: apm -> nic_config -> rx init */
    rx_hw_init();
    if (g_gen2) w32(CSR_MAC_SHADOW_REG_CTRL, 0x802FFFFFu);   /* working trace value */

    /* Force the fw/context-info DMA arena below 4GB before we build it: the boot
     * ROM DMAs from these physaddrs, and a >4GB arena is why FH_INT stayed 0. */
    arena_init_lowmem();
    uno_dbg_net_trace("wifi: DMA arena base=%08x%08x (%s 4GB)",
                      (u32)(g_arena_phys >> 32), (u32)g_arena_phys,
                      g_arena_phys < 0x100000000ull ? "below" : "ABOVE - alloc failed!");
    if (!g_gen2) msi_probe_enable();   /* gen2 uses PCI MSI-X (enabled above); MSI probe would conflict */

    /* THE F12 FIX (with MSI-X): the firmware leaves VT-d DMA remapping ON
     * (confirmed: DRHD@fed91000 TES=ON), which blocks the boot ROM's fw-load
     * DMA to our arena. Disable it so the device can DMA. */
    { char rep[128]; int nd = iommu_disable(rep, (int)sizeof rep);
      uno_dbg_net_trace("wifi: IOMMU disable: %d unit(s) %s", nd, rep); }

    /* load firmware + wait ALIVE */
    if (g_family >= FAM_AX210)      { if (load_fw_gen3() < 0) { st_set("WiFi: gen3 fw load failed"); uno_dbg_net_trace("wifi: FAIL gen3 (ctxt-info) fw load"); return BRINGUP_FATAL; } }
    else if (g_gen2)                { if (load_fw_gen2() < 0) { st_set("WiFi: gen2 fw load failed"); uno_dbg_net_trace("wifi: FAIL gen2 (ctxt-info) fw load"); return BRINGUP_FATAL; } }
    else                            { if (load_fw_gen1(g_fw.rt, g_fw.rt_n) < 0) { st_set("WiFi: gen1 fw load failed"); uno_dbg_net_trace("wifi: FAIL gen1 (section DMA) fw load"); return BRINGUP_FATAL; } }
    g_fw_loaded = 1;   /* the ROM is now self-loading/running - a re-init MUST quiesce it first (see the rerun verb) */

    if (wait_alive(2000) < 0) {
        st_set("WiFi: firmware did not ALIVE");
        uno_dbg_net_trace("wifi: FAIL no ALIVE notification within 2 s of fw start");
        return BRINGUP_NOFW;
    }
    uno_dbg_net_trace("wifi: firmware ALIVE");
    return BRINGUP_ALIVE;
}

uno_nic_t *iwl_nic(void)
{
    int vol, cred_vol;
    long fn;
    if (g_bound) return &g_nic;
#if UNO_DEBUG
    g_iot_n = 0; g_iot_inner = 0; g_iot_on = 1;   /* fresh trace per attempt */
#endif
    if (!iwl_present()) {
        st_set("no Intel WiFi card");
        uno_dbg_net_trace("wifi: no supported Intel card on the PCI bus%s",
                          g_is_dvm ? " (an iwldvm-era card was declined)" : "");
        return 0;
    }

    /* Credentials FIRST - see the long note below on why this is ahead of the
     * BAR0 map. A boot join with nothing to join to must cost nothing. */
    {
        int have_cfg;
        cred_vol = firmware_volume();
        have_cfg = (cred_vol >= 0 && read_config(cred_vol) >= 0);
        if (have_cfg) {
            strncpy(g_alt_ssid, g_cfg_ssid, sizeof g_alt_ssid - 1);
            strncpy(g_alt_psk,  g_cfg_psk,  sizeof g_alt_psk  - 1);
            uno_dbg_net_trace("wifi: creds from %s: ssid=\"%s\" psk_len=%d",
                              g_cfgname, g_cfg_ssid, (int)strlen(g_cfg_psk));
        } else { g_alt_ssid[0] = g_alt_psk[0] = 0; }
        /* The LAST NETWORK THE USER JOINED wins over the staged file.  It is
         * their most recent explicit choice, and on a machine that has never
         * had a WIFI.CFG written onto its stick it is the only credential
         * there is.  WIFI.CFG is not demoted to nothing: it stays as g_alt_*
         * and the join below falls back to it, which is what makes editing the
         * file on the stick still a working recovery path when the remembered
         * network is out of range or its password changed. */
        if (iwl_saved_count() > 0) {
            char s[SAVED_SSID_MAX], p[SAVED_PSK_MAX];
            if (iwl_saved_get(0, s, (int)sizeof s) == 0) {
                p[0] = 0; iwl_saved_psk(s, p, (int)sizeof p);
                strncpy(g_cfg_ssid, s, sizeof g_cfg_ssid - 1);
                g_cfg_ssid[sizeof g_cfg_ssid - 1] = 0;
                strncpy(g_cfg_psk, p, sizeof g_cfg_psk - 1);
                g_cfg_psk[sizeof g_cfg_psk - 1] = 0;
                have_cfg = 1;
                uno_dbg_net_trace("wifi: rejoining the last network \"%s\" "
                                  "(psk_len=%d); %s is the fallback",
                                  g_cfg_ssid, (int)strlen(g_cfg_psk),
                                  g_alt_ssid[0] ? g_cfgname : "no staged file");
            }
        }
        if (!have_cfg && !g_no_join) {
            st_set(cred_vol < 0 ? "WiFi: no saved network and no WIFI.CFG on the ESP"
                                : "WiFi: no saved network and WIFI.CFG has no ssid=");
            uno_dbg_net_trace("wifi: FAIL no usable credentials (cfg vol %d) - "
                              "a boot join needs a remembered network or ssid=", cred_vol);
            return 0;
        } else if (!have_cfg) {
            g_cfg_ssid[0] = g_cfg_psk[0] = 0;
            uno_dbg_net_trace("wifi: no stored credentials - radio-only bring-up "
                              "(scan / GUI join)");
        }
    }

    /* map BAR0 + bus master, and read the silicon identity, BEFORE choosing a
     * firmware file.  This used to happen ~100 lines further down - AFTER
     * choose_firmware() had already decoded `g_hw_rev` and after the .ucode had
     * been read off the disk - so on the FIRST bring-up of a cold boot the
     * stepping decode ran on g_hw_rev == 0: mac_type 0, step 0, and the
     * else-branch default for the family.  It only ever saw a real revision
     * from the second bring-up onwards, off the value the first one cached.
     * Metal proof (SURFGO, 2026-08-03): `hw_rev=00000000` on the boot
     * bring-up, `hw_rev=00000332` on the GUI one.  Linux reads CSR_HW_REV in
     * iwl_trans_pcie_alloc(), immediately after mapping BAR0 and before any of
     * the config/firmware selection, which is exactly this order. */
    pci_enable_bus_master(&g_pci);
    g_bar = (volatile u8 *)(uintptr_t)pci_bar(&g_pci, 0);
    if (!g_bar) {
        st_set("WiFi: no BAR0");
        uno_dbg_net_trace("wifi: FAIL BAR0 unmapped");
        return 0;
    }
    g_hw_rev = r32(CSR_HW_REV);
    g_hw_rf_id = r32(CSR_HW_RF_ID);
    uno_dbg_net_trace("wifi: BAR0 ok, hw_rev=%08x rf_id=%08x", g_hw_rev, g_hw_rf_id);

    choose_firmware();
    st_set("Intel WiFi "); st_cathex(g_hw_rev);
    read_fw_override();
    if (g_fwforce[0]) {
        /* g_fwforce comes out of a text file on the stick, so its length is not
         * ours to assume: build into a local, then let fwc_add() vet it. */
        char forced[FW_NAME_MAX];
        if (9 + strlen(g_fwforce) + 1 > FW_NAME_MAX) {
            uno_dbg_net_trace("wifi: fw= name is too long (%d chars) - ignoring it "
                              "and using the decoded candidate list",
                              (int)strlen(g_fwforce));
        } else {
            strcpy(forced, "FIRMWARE\\");
            strcat(forced, g_fwforce);
            g_fwcand_n = 0; fwc_add(forced);   /* forced means forced: no fallback */
            strcpy(g_fwfile, forced);
            uno_dbg_net_trace("wifi: firmware FORCED to %s by an fw= line "
                              "(the automatic stepping retry is disabled)", g_fwfile);
        }
    }

    /* The silicon identity, in the log, on every boot.  Without it a firmware
     * that never posts ALIVE is undiagnosable from a crash dump: the stepping
     * is the first thing anyone asks for and it was the one thing not
     * recorded.  hw_rev's bits 11:4 are the MAC type and 3:2 the step.  The
     * candidate list is logged with it, in the order they will be tried, so a
     * NETLOG says both what we decoded and what we were prepared to fall back
     * to. */
    {
        char fwl[FW_CAND_MAX * FW_NAME_MAX + 4]; int i; unsigned o = 0;
        fwl[0] = 0;
        for (i = 0; i < g_fwcand_n; i++) {
            const char *nm = g_fwcand[i];
            unsigned nl;
            if (strlen(nm) > 9) nm += 9;       /* strip the FIRMWARE\ prefix */
            nl = (unsigned)strlen(nm);
            if (o + nl + 2 >= sizeof fwl) break;      /* never write past the end */
            if (o) fwl[o++] = ',';
            memcpy(fwl + o, nm, nl); o += nl; fwl[o] = 0;
        }
        if (!fwl[0]) strcpy(fwl, "-");
        uno_dbg_net_trace("wifi: card pci=%04x fam=%d gen2=%d hw_rev=%08x "
                          "mac_type=%02x step=%d rf_id=%08x fw=%s",
                          g_devid, g_family, g_gen2, g_hw_rev,
                          (g_hw_rev >> 4) & 0xFFF, (g_hw_rev >> 2) & 3, g_hw_rf_id,
                          fwl);
    }

    /* Credentials: needed to JOIN, NOT to bring the radio up and scan. The GUI
     * scans first and asks for the password afterwards, which is the whole
     * point of the Control Panel's network pane - so a missing WIFI.CFG must
     * not stop the radio (metal: "no networks found" on a box with a working
     * card and no config file). g_no_join marks exactly that bring-up.
     *
     * THIS CHECK COMES BEFORE THE DEVICE IS TOUCHED, and that ordering is load
     * bearing. d3f5ca54 moved the BAR0 map and CSR_HW_REV read above it so the
     * firmware decode had a revision to work with, which was right, but it also
     * meant a boot-join attempt with no credentials now enabled bus mastering
     * and mapped BAR0 before discovering it had nothing to join with. Anything
     * wanting the network retries this - URC's RS_DOWN retry alone does it
     * every ~5 s - so on a box with no WIFI.CFG that was PCI work, and a log
     * line, on a loop forever. The credentials live on a filesystem and the
     * check needs nothing from the card, so it costs nothing to ask first.
     * (The block itself now runs above, before pci_enable_bus_master.) */

    /* Try each candidate ucode until one posts ALIVE.
     *
     * Reading the image: the volume that actually holds the file, then the
     * config volume (what this used to assume), so a filesystem whose size
     * query and read query disagree cannot cost us the radio.  Each is tried
     * with the full FIRMWARE\ path and with the flat root name, since the FAT
     * reader may expose either.
     *
     * A candidate that is not staged is skipped; one that loads and never
     * ALIVEs costs ~4 s and we move on.  Each pass begins by halting whatever
     * image is already running - see the note at the top of the loop; that has
     * to happen before the disk read, not just before prepare_card_hw(). */
    {
        int fi, r = BRINGUP_NOFW, tried = 0;
        for (fi = 0; fi < g_fwcand_n; fi++) {
            int cand[2], nc = 0, ci, fv;
            /* HALT ANY RUNNING IMAGE FIRST, before we touch the disk.
             *
             * Reading the next candidate is ~1.4 MB off the boot stick through
             * the FIRMWARE's USB stack, and iommu_disable() has already turned
             * VT-d OFF for this device.  A previously-kicked ROM that is live
             * and confused can therefore DMA anywhere, unconstrained, for the
             * whole duration of that read - including over the firmware's own
             * USB/HID structures, which is what the pointer runs on while we
             * are still attached.  A dead mouse mid-session is what that looks
             * like from the outside.
             *
             * The hazard is not new (the single-shot path had the same read
             * before its device_stop, and a GUI rescan after a failed attempt
             * hit it), but the candidate loop enters it once per retry instead
             * of once per scan, so it went from rare to routine. */
            if (g_bar && g_fw_loaded) {
                uno_dbg_net_trace("wifi: halting the previous image (device_stop) "
                                  "before reading the next candidate");
                device_stop();
            }
            strcpy(g_fwfile, g_fwcand[fi]);
            fv = fw_volume();                  /* per candidate: different file */
            if (fv >= 0)                         cand[nc++] = fv;
            if (cred_vol >= 0 && cred_vol != fv) cand[nc++] = cred_vol;
            vol = -1; fn = 0;
            for (ci = 0; ci < nc && fn <= 0; ci++) {
                vol = cand[ci];
                fn = uno_fs_read(vol, g_fwfile, g_fwbuf, FW_FILE_MAX);
                if (fn <= 0) fn = uno_fs_read(vol, g_fwfile + 9, g_fwbuf, FW_FILE_MAX);
            }
            if (fn <= 0) {
                uno_dbg_net_trace("wifi: candidate %d/%d %s not staged - skipping",
                                  fi + 1, g_fwcand_n, g_fwfile);
                continue;
            }
            if (parse_ucode(g_fwbuf, (u32)fn) < 0) {
                uno_dbg_net_trace("wifi: candidate %d/%d %s (%ld bytes) failed TLV parse - skipping",
                                  fi + 1, g_fwcand_n, g_fwfile, fn);
                continue;
            }
            uno_dbg_net_trace("wifi: candidate %d/%d %s loaded from disk (%ld bytes)",
                              fi + 1, g_fwcand_n, g_fwfile, fn);
            load_pnvm(vol);
            tried++;
            r = bringup_to_alive();
            if (r == BRINGUP_ALIVE) {
                /* Remember it: later bring-ups start here instead of walking
                 * back through images this card has already refused. */
                if (strcmp(g_fw_known_good, g_fwfile)) {
                    strcpy(g_fw_known_good, g_fwfile);
                    uno_dbg_net_trace("wifi: %s is the working image on this card - "
                                      "later bring-ups will try it first", g_fwfile);
                }
                break;
            }
            if (r == BRINGUP_FATAL) {
                uno_dbg_net_trace("wifi: bring-up failed for a reason a different "
                                  "stepping cannot fix - not trying the rest");
                return 0;
            }
            if (fi + 1 < g_fwcand_n)
                uno_dbg_net_trace("wifi: %s never ALIVEd - trying the next stepping",
                                  g_fwfile);
        }
        if (r != BRINGUP_ALIVE) {
            if (!tried) {
                st_set("WiFi: firmware not found ("); st_cat(g_fwfile); st_cat(")");
                uno_dbg_net_trace("wifi: FAIL no candidate ucode is staged (%d name(s) tried) - "
                                  "uno-wifi-fw.py stages them", g_fwcand_n);
            } else {
                st_set("WiFi: firmware did not ALIVE");
                uno_dbg_net_trace("wifi: FAIL no ALIVE from any of %d staged ucode(s) - "
                                  "this is not a stepping mismatch", tried);
            }
            return 0;
        }
    }
    g_alive = 1;
    rx_restock();      /* gen2: first restock happens at alive (fw owns the RFH) */
    if (g_family >= FAM_AX210 && g_pnvm_len) {
        /* gen3: tell the fw to consume the PNVM staged in the prph scratch
         * (UREG_DOORBELL_TO_ISR6, PNVM bit), then wait for the init-complete
         * notification (REGULATORY_AND_NVM group 0x0c, PNVM 0xFE). */
        int nl = 0;
        prph_w(uprph(UREG_DOORBELL_TO_ISR6), 1u /*PNVM*/);
        if (wait_notif(0x0c, 0xFE, &nl, 1000))
            uno_dbg_net_trace("wifi: PNVM accepted (init complete, %d bytes notif)", nl);
        else
            uno_dbg_net_trace("wifi: PNVM doorbell rung but NO init-complete in 1 s "
                              "(raw-TLV form rejected? sku mismatch? - parse the "
                              ".PNV per sku_id %08x/%08x/%08x next)",
                              g_sku_id[0], g_sku_id[1], g_sku_id[2]);
    }
    /* --- F12 boundary -------------------------------------------------------
     * Reaching ALIVE is the F12 fix and it is DONE: with the handshake in
     * wait_alive() the normal bring-up now gets the 144-byte ALIVE
     * notification (proven standalone via "iwl alive 3": payload 0x90).
     * Everything BELOW - MVM/NVM/PHY init, tx-antenna, power table, scan,
     * auth, assoc - has never executed on this driver and is the next slice.
     * Running it inline wedged the rig (and the wedge ate the logs), so it is
     * gated OFF: a plain bring-up now stops here, reports ALIVE, and stays
     * recoverable.  Arm it deliberately with "iwl mvm" then "iwl rerun" to work
     * that sequence step by step, the same way the ALIVE handshake was built. */
    if (!g_mvm_arm) {
        st_set("WiFi: fw ALIVE (F12 solved) - MVM bring-up gated; 'iwl mvm' to continue");
        uno_dbg_net_trace("wifi: ALIVE reached in the NORMAL path - MVM/join sequence gated off "
                          "(run 'iwl mvm' then 'iwl rerun' to enter it)");
        return 0;
    }

    mvm_init_all();

    /* connect (skipped when the caller only wanted the radio up - the GUI scan
     * path, which picks its own network afterwards) */
    if (g_no_join) {
        st_set("WiFi: radio up - press Scan to list networks");
        return 0;
    }
    if (find_and_join() < 0) {
        /* The remembered network is not here, or its password changed.  Fall
         * back to the hand-staged WIFI.CFG if it names a DIFFERENT one - that
         * is what keeps editing the file on the stick a working recovery path
         * now that the saved store outranks it.  One extra attempt, only on
         * failure, and only when the two disagree. */
        int retry = (g_alt_ssid[0] && strcmp(g_alt_ssid, g_cfg_ssid) != 0);
        if (retry) {
            uno_dbg_net_trace("wifi: \"%s\" did not join - falling back to %s "
                              "(\"%s\")", g_cfg_ssid, g_cfgname, g_alt_ssid);
            strncpy(g_cfg_ssid, g_alt_ssid, sizeof g_cfg_ssid - 1);
            g_cfg_ssid[sizeof g_cfg_ssid - 1] = 0;
            strncpy(g_cfg_psk, g_alt_psk, sizeof g_cfg_psk - 1);
            g_cfg_psk[sizeof g_cfg_psk - 1] = 0;
        }
        if (!retry || find_and_join() < 0) {
            st_set("WiFi: join failed"); uno_dbg_net_trace("wifi: FAIL join"); return 0;
        }
    }

    nic_publish();
    /* it worked: remember it, so the next boot comes back to this network
     * whether it came from the store or from the staged file */
    saved_remember(g_cfg_ssid, g_cfg_psk);
    st_set("WiFi bound: "); st_cat(g_ssid_str[0]?g_ssid_str:g_cfg_ssid);
    st_cat(g_joined ? " (joined)" : " (associating)");
    return &g_nic;
}

const unsigned char *iwl_mac(void) { return g_mac; }

/* ---- runtime network selection (the Network app's join UI) ---------------
 * Bring the card up to a scannable state WITHOUT joining anything: firmware
 * ALIVE + the post-ALIVE MVM init. Idempotent. */
static int radio_up(void)
{
    /* g_bound/g_joined mean the card is already up and initialised - by the
     * step-by-step debug verbs, which do the same MVM init without setting
     * g_mvm_done. Without this the GUI's Scan silently did nothing on a box
     * that had been driven by hand (metal, round 25): iwl_nic() returns the
     * cached nic immediately when bound, so g_mvm_done could never become 1. */
    if (g_alive && (g_mvm_done || g_bound || g_joined)) return 0;
    prog("Starting the radio", 1);
    g_mvm_arm = 1;                       /* past the boot-path MVM gate */
    g_no_join = 1;
    iwl_nic();
    g_no_join = 0;
    return (g_alive && g_mvm_done) ? 0 : -1;
}


/* Scan and report one entry per BSS, strongest first. Returns the count. */
int iwl_scan_aps(iwl_ap_t *out, int max)
{
    int i, j, n = 0;
    if (!out || max <= 0) return 0;
    if (radio_up() < 0) return 0;
    mvm_scan_cfg();
    mvm_scan_passive(5000);
    for (i = 0; i < g_scan_ap_n && n < max; i++) {
        if (!g_scan_aps[i].ssid[0]) continue;             /* hidden BSS */
        /* fold the mesh: one row per SSID, keeping the strongest */
        for (j = 0; j < n; j++)
            if (!strcmp(out[j].ssid, g_scan_aps[i].ssid)) break;
        if (j < n) {
            if (g_scan_aps[i].rssi > out[j].rssi) {
                memcpy(out[j].bssid, g_scan_aps[i].bssid, 6);
                out[j].chan = g_scan_aps[i].chan;
                out[j].rssi = g_scan_aps[i].rssi;
            }
            continue;
        }
        memset(&out[n], 0, sizeof out[n]);
        strncpy(out[n].ssid, g_scan_aps[i].ssid, sizeof out[n].ssid - 1);
        memcpy(out[n].bssid, g_scan_aps[i].bssid, 6);
        out[n].chan = g_scan_aps[i].chan;
        out[n].rssi = g_scan_aps[i].rssi;
        n++;
    }
    for (i = 0; i < n; i++)                                /* strongest first */
        for (j = i + 1; j < n; j++)
            if (out[j].rssi > out[i].rssi) { iwl_ap_t t = out[i]; out[i] = out[j]; out[j] = t; }
    uno_dbg_net_trace("wifi: scan for the UI: %d BSS -> %d networks", g_scan_ap_n, n);
    return n;
}

/* Join `ssid` with `psk` (WPA2-PSK; empty psk = open, untested). Runs the same
 * scan -> pick -> auth -> assoc -> 4-way path as the boot join, then publishes
 * the nic. 0 = joined, <0 = failed (iwl_status_str says how far it got). */
int iwl_join_ssid(const char *ssid, const char *psk)
{
    if (!ssid || !ssid[0]) return -1;
    /* A second join cannot reuse the first one's fw contexts: PHY_CONTEXT ADD on
     * a live context, or a phy-bind on an active link, asserts the LMAC (round
     * 24). Restart the radio instead - a few seconds, and always recoverable. */
    if (g_ctx_built) {
        uno_dbg_net_trace("wifi: join: fw contexts from an earlier join are live - "
                          "restarting the radio before re-joining");
        if (g_bar && g_fw_loaded) device_stop();
        g_bound = 0; g_alive = 0; g_mvm_done = 0; g_ctx_built = 0;
    }
    /* a fresh join must not inherit the previous one's keys or supplicant */
    g_joined = 0; g_keys_installed = 0; g_key_gtk_idx = -1; g_wpa_active = 0;
    g_data_qid = -1; g_dq_head = g_dq_tail = 0;
    if (radio_up() < 0) return -1;
    strncpy(g_cfg_ssid, ssid, sizeof g_cfg_ssid - 1);
    g_cfg_ssid[sizeof g_cfg_ssid - 1] = 0;
    strncpy(g_cfg_psk, psk ? psk : "", sizeof g_cfg_psk - 1);
    g_cfg_psk[sizeof g_cfg_psk - 1] = 0;
    uno_dbg_net_trace("wifi: join request for \"%s\" (psk_len=%d)",
                      g_cfg_ssid, (int)strlen(g_cfg_psk));
    if (find_and_join() < 0) { st_set("WiFi: join failed"); return -1; }
    nic_publish();
    /* A network the user picked and typed a password for is remembered, so the
     * next boot rejoins it without them typing it again.  Only on SUCCESS - a
     * wrong password must not be stored and retried forever at boot. */
    if (g_joined) saved_remember(g_cfg_ssid, g_cfg_psk);
    return g_joined ? 0 : -1;
}

/* ---- interactive F12 debug entry point (see iwlwifi.h) ------------------- */
static int hex_u32(const char **p, u32 *out)
{
    u32 v = 0; int n = 0;
    while (**p == ' ') (*p)++;
    for (;; (*p)++, n++) {
        char c = **p;
        if      (c >= '0' && c <= '9') v = (v << 4) | (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (u32)(c - 'A' + 10);
        else break;
    }
    if (!n) return -1;
    *out = v;
    return 0;
}

/* raw physical reads (UnoDOS is identity-mapped; ACPI + IOMMU MMIO are directly
 * addressable while attached) */
static u8  rdp8(u64 a)  { return *(volatile u8  *)(uintptr_t)a; }
static u32 rdp32(u64 a) { return *(volatile u32 *)(uintptr_t)a; }
static u64 rdp64(u64 a) { return *(volatile u64 *)(uintptr_t)a; }
unsigned long long uno_acpi_rsdp(void);

static char *dm_s(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
static char *dm_h(char *o, u32 v) { const char *hx="0123456789abcdef"; int i;
    for (i=0;i<8;i++) *o++ = hx[(v>>((7-i)*4))&0xF]; return o; }

/* Find the DMAR ACPI table and report each remapping unit's GSTS.TES bit -
 * i.e. whether VT-d DMA-remapping is ENABLED (which would block our device DMA
 * to the RAM arena, the leading F12 hypothesis). No DMAR table => no IOMMU. */
static int dmar_check(char *out, int cap)
{
    u64 rsdp = (u64)uno_acpi_rsdp();
    u64 xsdt = 0, rsdt = 0, dmar = 0;
    char *o = out;
    int i, n, dl, off, rev, units = 0;
    (void)cap;
    if (!rsdp) return (int)(dm_s(o, "no RSDP") - out);
    rev  = rdp8(rsdp + 15);
    rsdt = rdp32(rsdp + 16);
    if (rev >= 2) xsdt = rdp64(rsdp + 24);
    if (xsdt) { n = ((int)rdp32(xsdt + 4) - 36) / 8;
        for (i = 0; i < n; i++) { u64 t = rdp64(xsdt + 36 + i*8);
            if (rdp32(t) == 0x52414d44u) { dmar = t; break; } } }
    if (!dmar && rsdt) { n = ((int)rdp32(rsdt + 4) - 36) / 4;
        for (i = 0; i < n; i++) { u64 t = (u64)rdp32(rsdt + 36 + i*4);
            if (rdp32(t) == 0x52414d44u) { dmar = t; break; } } }
    if (!dmar) return (int)(dm_s(o, "no DMAR: platform has NO VT-d IOMMU") - out);
    o = dm_s(o, "DMAR present; ");
    dl = (int)rdp32(dmar + 4); off = 48;
    while (off + 4 <= dl) {
        u16 type = (u16)(rdp8(dmar+off) | (rdp8(dmar+off+1)<<8));
        u16 sl   = (u16)(rdp8(dmar+off+2) | (rdp8(dmar+off+3)<<8));
        if (!sl) break;
        if (type == 0) {                       /* DRHD */
            u64 base = rdp64(dmar + off + 8);
            u32 gsts = rdp32(base + 0x1c);
            o = dm_s(o, "DRHD@"); o = dm_h(o, (u32)base);
            o = dm_s(o, " GSTS="); o = dm_h(o, gsts);
            o = dm_s(o, (gsts & 0x80000000u) ? " TES=ON(DMA-remap active!) " : " TES=off ");
            units++;
        }
        off += sl;
    }
    if (!units) o = dm_s(o, "(no DRHD units)");
    *o = 0;
    return (int)(o - out);
}

static void wrp32(u64 a, u32 v) { *(volatile u32 *)(uintptr_t)a = v; }

/* Disable VT-d DMA remapping on every active DRHD unit so the WiFi device can
 * DMA to our RAM arena. THE F12 FIX (with conf_msix): the Yoga's firmware leaves
 * TES=ON on the PCH IOMMU (fed91000), which blocks the boot ROM's fw-load DMA -
 * confirmed by `iwl dmar`. Clearing GCMD.TE lets DMA through untranslated, which
 * is fine on a bare OS we fully own. Preserve the other persistent command bits
 * (EAFL/QIE/IRE/CFI) and only clear TE; poll GSTS.TES until it drops. Returns the
 * number of units disabled. `out` (may be NULL) gets a short report. */
static int iommu_disable(char *out, int cap)
{
    u64 rsdp = (u64)uno_acpi_rsdp();
    u64 xsdt = 0, rsdt = 0, dmar = 0;
    char *o = out; int i, n, dl, off, rev, done = 0;
    (void)cap;
    if (!rsdp) { if (o) *dm_s(o, "no RSDP") = 0; return 0; }
    rev = rdp8(rsdp + 15); rsdt = rdp32(rsdp + 16);
    if (rev >= 2) xsdt = rdp64(rsdp + 24);
    if (xsdt) { n = ((int)rdp32(xsdt + 4) - 36) / 8;
        for (i = 0; i < n; i++) { u64 t = rdp64(xsdt + 36 + i*8);
            if (rdp32(t) == 0x52414d44u) { dmar = t; break; } } }
    if (!dmar && rsdt) { n = ((int)rdp32(rsdt + 4) - 36) / 4;
        for (i = 0; i < n; i++) { u64 t = (u64)rdp32(rsdt + 36 + i*4);
            if (rdp32(t) == 0x52414d44u) { dmar = t; break; } } }
    if (!dmar) { if (o) *dm_s(o, "no DMAR") = 0; return 0; }
    dl = (int)rdp32(dmar + 4); off = 48;
    while (off + 4 <= dl) {
        u16 type = (u16)(rdp8(dmar+off) | (rdp8(dmar+off+1)<<8));
        u16 sl   = (u16)(rdp8(dmar+off+2) | (rdp8(dmar+off+3)<<8));
        if (!sl) break;
        if (type == 0) {                          /* DRHD */
            u64 base = rdp64(dmar + off + 8);
            u32 gsts = rdp32(base + 0x1c), pmen;
            int acted = 0;
            if (gsts & 0x80000000u) {             /* TES on -> disable TE */
                wrp32(base + 0x18, gsts & 0x16800000u);   /* keep EAFL/QIE/IRE/CFI, clear TE */
                for (i = 0; i < 100000 && (rdp32(base + 0x1c) & 0x80000000u); i++) ;
                acted = 1;
            }
            /* Disable Protected Memory Regions (PMEN reg @0x64). This is the
             * boot-DMA-protection that blocks DMA to protected physical ranges
             * REGARDLESS of translation - the likely reason clearing TES alone
             * did not unblock the device DMA. Clear EPM (bit31), poll PRS (bit0). */
            pmen = rdp32(base + 0x64);
            if (pmen & 0x80000000u) {
                wrp32(base + 0x64, pmen & ~0x80000000u);
                for (i = 0; i < 100000 && (rdp32(base + 0x64) & 1u); i++) ;
                acted = 1;
            }
            if (acted) {
                done++;
                if (o) { o = dm_s(o, "unit@"); o = dm_h(o, (u32)base);
                         o = dm_s(o, " GSTS->"); o = dm_h(o, rdp32(base + 0x1c));
                         o = dm_s(o, " PMEN->"); o = dm_h(o, rdp32(base + 0x64)); o = dm_s(o, " "); }
            }
        }
        off += sl;
    }
    if (o) { if (!done) o = dm_s(o, "no active IOMMU units"); *o = 0; }
    return done;
}

/* Post-ALIVE MVM bring-up, one stage per call, driven by "iwl mvm <n>".
 *
 * Same rationale as alive_steps(): this sequence has never run against real
 * firmware, running it inline wedged the box, and a wedge eats the in-flight
 * URC logs - so each stage is its own command that completes and flushes, and
 * is armed only after "iwl rerun" has parked the fw at the ALIVE gate.  Drive
 * it under the guard: "guard 40 reboot" then "iwl mvm 1", "iwl mvm 2", ...  A
 * stage that wedges is recovered by the guard and named by the last stage that
 * DID return.  State accumulates across calls (same boot), so run them in order.
 *
 *   1  mvm_init_unified   SYSTEM init + NVM + PHY cfg + INIT_COMPLETE + NVM info
 *   2  mvm_tx_ant         TX_ANT_CONFIG
 *   3  mvm_dqa_enable     DQA (only if fw advertises capa 12)
 *   4  mvm_power_table    POWER_TABLE
 *   5  scan_cfg   6 phy_ctxt   7 mac_ctxt   8 binding   9 time_quota+add_sta+wpa
 *      (find_and_join's send_cmd chain, split so a wedge names the exact cmd) */
static int mvm_steps(int n, char *out, int cap)
{
    if (!g_bar || !g_alive) {
        strcpy(out, "err fw not ALIVE - run 'iwl rerun' first (it parks at the ALIVE gate)");
        return (int)strlen(out);
    }
    switch (n) {
    case 1: mvm_init_unified();
        strcpy(out, "ok mvm1: init_unified returned (SYSTEM/NVM/INIT_COMPLETE, no phy_cfg)"); break;
    case 2: mvm_tx_ant(fw_valid_tx_ant());
        strcpy(out, "ok mvm2: tx_ant returned"); break;
    case 3: if (fw_has_capa(12)) { mvm_dqa_enable(); strcpy(out, "ok mvm3: dqa_enable returned"); }
            else strcpy(out, "ok mvm3: skipped (fw lacks DQA capa 12)"); break;
    case 4: mvm_power_table();
        strcpy(out, "ok mvm4: power_table returned"); break;
    /* stage 5 (find_and_join) wedged; 5..9 walk its send_cmd chain so the exact
     * culprit command is named. Run in order - each needs the prior ones' state.
     * g_bssid is broadcast (the scaffold has no beacon parse yet). */
    case 5: mvm_scan_cfg();
        strcpy(out, "ok mvm5: scan_cfg returned"); break;
    case 6: mvm_phy_ctxt(1, 1 /*ADD*/);
        strcpy(out, "ok mvm6: phy_ctxt ADD returned"); break;
    case 7: memset(g_bssid, 0xFF, 6); mvm_mac_ctxt(g_bssid, 0, 0, 1 /*ADD*/);
        strcpy(out, "ok mvm7: mac_ctxt ADD returned"); break;
    case 8: mvm_binding(1);
        strcpy(out, "ok mvm8: binding returned"); break;
    case 9: mvm_time_quota(); mvm_assoc_window(); mvm_add_sta(g_bssid, 0, 0);
        wpa_arm();
        strcpy(out, "ok mvm9: time_quota+assoc_window+add_sta+wpa returned"); break;
    /* stage-9 split (a..d): stage 9 wedged; walk its four ops. Needs 7 first
     * (g_bssid = broadcast). Suspicion: assoc_window waits on a NOTIFICATION,
     * and with the MAC context up the fw may push real 802.11 RX frames that
     * rx_process_rb's REPLY_RX_MPDU branch has never parsed. */
    case 10: mvm_time_quota();
        strcpy(out, "ok mvm9a: time_quota returned"); break;
    case 11: mvm_assoc_window();
        strcpy(out, "ok mvm9b: assoc_window returned"); break;
    case 12: mvm_add_sta(g_bssid, 0, 0);
        strcpy(out, "ok mvm9c: add_sta returned"); break;
    /* wpa_arm() rather than an open-coded PMK: it is the one place that knows
     * which AKM was negotiated and builds the RSN element the supplicant will
     * echo.  With no beacon parsed (these scaffold verbs point g_bssid at
     * broadcast) it falls back to WPA2-PSK, which is what this step always
     * did. */
    case 13: wpa_arm();
        strcpy(out, "ok mvm9d: wpa PMK/init returned"); break;
    /* e/f: the two iwl_mvm_up() commands we never sent. Separate steps so metal
     * can tell which (if either) is what SESSION_PROTECTION was missing. Run
     * them after mvm 2 (tx_ant), matching Linux's order in iwl_mvm_up(). */
    case 14: mvm_bt_init();
        { u32 h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
          strcpy(out, h ? "err mvme: BT_CONFIG asserted the fw (iwl fwerr)"
                        : "ok mvme: BT_CONFIG accepted"); } break;
    case 15: mvm_mcc_update("ZZ");
        { u32 h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
          strcpy(out, h ? "err mvmf: MCC_UPDATE asserted the fw (iwl fwerr)"
                        : "ok mvmf: MCC_UPDATE accepted (detail in NET log)"); } break;
    default:
        strcpy(out, "err usage: iwl mvm <1-9|a-f> (5scan 6phy 7mac 8bind 9=a+b+c+d: a quota b assoc-window c add-sta d wpa; e BT_CONFIG f MCC_UPDATE)"); break;
    }
    return (int)strlen(out);
}

/* Link-API association, one command per call: "iwl mld <n>". Same rationale as
 * mvm_steps() - a wedge eats the in-flight URC log frames, so each command is its
 * own round trip that completes and flushes, and the last step that DID return
 * names the culprit. Run "iwl scan" first so g_bssid/g_join_chan are real.
 *
 *   1 MAC_CONFIG ADD   2 LINK_CONFIG ADD   3 PHY_CONTEXT ADD
 *   4 LINK_CONFIG MODIFY(active)           5 STA_CONFIG (AP peer)
 *   6 txq_alloc        7 SESSION_PROTECTION            8 auth   9 assoc
 */
static int mld_steps(int n, char *out, int cap)
{
    static const u8 zero6[6] = { 0, 0, 0, 0, 0, 0 };
    u32 h;
    (void)cap;
    if (!g_bar || !g_alive) {
        strcpy(out, "err fw not ALIVE - run 'iwl rerun' then 'iwl mvm 1'..'4'");
        return (int)strlen(out);
    }
    if (!fw_has_mld_api() && n <= 5) {
        strcpy(out, "err fw does not advertise the link API (capa 110) - use 'iwl mvm <n>'");
        return (int)strlen(out);
    }
    if (n >= 3 && n <= 5 && !memcmp(g_bssid, zero6, 6)) {
        strcpy(out, "err no AP picked - run 'iwl scan' first (needs a real BSSID/chan)");
        return (int)strlen(out);
    }
    switch (n) {
    case 1: mld_mac_cfg(1 /*ADD*/, 0, 0);
        strcpy(out, "ok mld1: MAC_CONFIG ADD returned"); break;
    case 2: mld_link_cfg(1 /*ADD*/, 0, 0 /*phy INVALID*/, 0);
        strcpy(out, "ok mld2: LINK_CONFIG ADD returned"); break;
    case 3: mvm_phy_ctxt(g_join_chan ? g_join_chan : 1, 1 /*ADD*/);
        strcpy(out, "ok mld3: PHY_CONTEXT ADD returned"); break;
    /* two MODIFYs: bind the PHY while still inactive, then activate. Bail out
     * between them if the first asserted, so the fwerr names the right one. */
    case 4: mld_link_cfg(2 /*MODIFY*/, 0 /*inactive*/, 1 /*phy*/, 0);
        h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
        uno_dbg_net_trace("wifi: mld4: after LINK_CONFIG phy-bind csr2808=%08x", h);
        if (h) { strcpy(out, "err mld4: LINK_CONFIG phy-bind asserted the fw (iwl fwerr)"); break; }
        mld_link_cfg(2 /*MODIFY*/, 1 /*active*/, 1 /*phy*/,
                     LINK_MOD_ACTIVE | LINK_MOD_RATES_INFO);
        h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
        uno_dbg_net_trace("wifi: mld4: after LINK_CONFIG activate csr2808=%08x", h);
        strcpy(out, h ? "err mld4: LINK_CONFIG activate asserted the fw (iwl fwerr)"
                      : "ok mld4: LINK_CONFIG phy-bind + activate accepted"); break;
    case 5: mld_sta_cfg(g_bssid, 0, 0);
        strcpy(out, "ok mld5: STA_CONFIG (AP peer) returned"); break;
    case 6: { int q = mvm_txq_alloc(AP_STA_ID, 15, TXQ_N);
        uno_dbg_net_trace("wifi: mld6: txq_alloc -> qid=%d", q);
        strcpy(out, q >= 0 ? "ok mld6: TX queue allocated (qid in NET log)"
                           : "err mld6: txq_alloc gave no queue"); break; }
    case 7: mvm_assoc_window();
        h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
        uno_dbg_net_trace("wifi: mld7: after session-prot csr2808=%08x", h);
        strcpy(out, h ? "err mld7: session-prot asserted the fw (iwl fwerr)"
                      : "ok mld7: SESSION_PROTECTION accepted"); break;
    case 8: { int r = mvm_auth();
        uno_dbg_net_trace("wifi: mld8: auth -> %d", r);
        strcpy(out, r == 0 ? "ok mld8: Open-System auth accepted"
                           : (r < 0 ? "err mld8: no auth response" : "err mld8: AP rejected auth")); break; }
    case 9: { int r; wpa_arm(); r = mvm_assoc();
        uno_dbg_net_trace("wifi: mld9: assoc -> %d", r);
        if (r >= 0) {
            assoc_mark_associated(r);
            strcpy(out, "ok mld9: associated (run 'iwl eapol')");
        } else {
            strcpy(out, "err mld9: assoc failed (detail in NET log)");
        }
        break; }
    /* 'a': the post-association link tune Linux sends on BSS_CHANGED_QOS (EDCA
     * params, plus the beacon timing the STA path normally never sets). Kept OUT
     * of the main path on purpose - the fw validates exactly the field groups the
     * modify_mask selects, and these two groups are what asserted when they were
     * folded into the activation MODIFY. Run it after 'iwl mld 9' to see whether
     * this fw accepts them once associated. */
    case 10: mld_link_cfg(2 /*MODIFY*/, 1 /*active*/, 1 /*phy*/,
                          LINK_MOD_QOS_PARAMS | LINK_MOD_BEACON_TIMING);
        h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
        uno_dbg_net_trace("wifi: mlda: after LINK_CONFIG qos+beacon csr2808=%08x", h);
        strcpy(out, h ? "err mlda: qos/beacon-timing MODIFY asserted the fw (iwl fwerr)"
                      : "ok mlda: LINK_CONFIG qos+beacon-timing accepted"); break;
    default:
        strcpy(out, "err usage: iwl mld <1-9|a> (1mac 2link 3phy 4link-active 5sta 6txq 7sessprot 8auth 9assoc a=post-assoc qos/beacon)");
        break;
    }
    return (int)strlen(out);
}

/* ---- data-path probe (`iwl data`, `iwl arp`) -----------------------------
 * The IP stack binds ONE nic (net_init), and on this rig that nic is the USB
 * ethernet carrying the URC control channel - rebinding it to WiFi would cut
 * the only line to the box mid-experiment. So the data path is proven here
 * instead: raw 802.11 frames straight through tx_enqueue/handle_data_frame,
 * with the stack untouched. An ARP request/reply exercises exactly what a DHCP
 * DISCOVER would (TX encrypt with the pairwise key, AP forwards, reply comes
 * back GTK/CCMP-decrypted and reaches the RX queue) in one round trip. */
static int probe_data(const u8 target_ip[4], int listen_ms, char *out, int cap)
{
    u8 eth[64], tx[128];
    u32 rx0 = g_rx_data_n, drop0 = g_rx_data_drop;
    int i, n, got = 0;
    (void)cap;
    if (!g_joined) { strcpy(out, "err not joined - run the join sequence first"); return (int)strlen(out); }
    g_rx_data_log = 12;
    g_txresp_n = 0;             /* re-arm the REPLY_TX dumps: TX_STATUS says whether
                                 * the AP ACKed our frame even if nothing answers */
    /* drain anything already queued so the counts below are about THIS probe */
    while (g_dq_tail != g_dq_head) g_dq_tail = (g_dq_tail + 1) % DATAQ;
    if (target_ip) {
        /* Sender IP: an RFC-5227 probe (spa 0.0.0.0) is NOT reliably answered,
         * which is exactly how the first metal run produced "no data frames"
         * on a link that turned out to work perfectly under real DHCP. Use a
         * link-local address derived from our MAC instead - a real address,
         * off everyone's subnet, so a reply comes back and nothing collides. */
        memset(eth, 0xff, 6);                       /* broadcast */
        memcpy(eth + 6, g_mac, 6);
        eth[12] = 0x08; eth[13] = 0x06;             /* ARP */
        eth[14] = 0x00; eth[15] = 0x01;             /* htype ethernet */
        eth[16] = 0x08; eth[17] = 0x00;             /* ptype IPv4 */
        eth[18] = 6; eth[19] = 4; eth[20] = 0; eth[21] = 1;   /* request */
        memcpy(eth + 22, g_mac, 6);
        eth[28] = 169; eth[29] = 254;               /* spa 169.254.x.y */
        eth[30] = g_mac[4] ? g_mac[4] : 1;
        eth[31] = g_mac[5] ? g_mac[5] : 2;
        memset(eth + 32, 0, 6);
        memcpy(eth + 38, target_ip, 4);
        for (i = 0; i < 3; i++) {
            n = eth_to_80211(eth, 42, tx);
            tx_enqueue(tx, n, 0);
            g_tx_data_n++;
            uno_dbg_net_trace("wifi: probe: ARP who-has %d.%d.%d.%d (frame %d, 802.11 len %d)",
                              target_ip[0], target_ip[1], target_ip[2], target_ip[3], i + 1, n);
            rx_pump_ms(600, 0);
        }
    }
    rx_pump_ms(listen_ms, 0);
    /* report what landed in the recv queue */
    while (g_dq_tail != g_dq_head) {
        const u8 *e = g_dataq[g_dq_tail].buf; int el = g_dataq[g_dq_tail].len;
        if (got < 8)
            uno_dbg_net_trace("wifi: probe: rx eth %02x:%02x:%02x:%02x:%02x:%02x <- "
                              "%02x:%02x:%02x:%02x:%02x:%02x type=%02x%02x len=%d",
                              e[0],e[1],e[2],e[3],e[4],e[5], e[6],e[7],e[8],e[9],e[10],e[11],
                              e[12], e[13], el);
        got++;
        g_dq_tail = (g_dq_tail + 1) % DATAQ;
    }
    uno_dbg_net_trace("wifi: probe: data frames rx=%d (queued %d, dropped %d) tx=%d rssi=%d mic=%d",
                      (int)(g_rx_data_n - rx0), got, (int)(g_rx_data_drop - drop0),
                      (int)g_tx_data_n, g_last_rssi, g_rx_mic);
    { char *o = out; const char *p = got ? "ok probe: data frames received (NET log): "
                                         : "err probe: NO data frames received: ";
      int v = got, m = 0, j; char d[8];
      while (*p) *o++ = *p++;
      if (!v) d[m++] = '0'; else while (v) { d[m++] = (char)('0' + v % 10); v /= 10; }
      for (j = m - 1; j >= 0; j--) *o++ = d[j];
      p = " queued"; while (*p) *o++ = *p++; *o = 0; }
    g_rx_data_log = 0;
    return (int)strlen(out);
}

/* ---- `iwl netup` / `netwifi` / `netres` ----------------------------------
 * Run the real IP suite (DHCP, ping, DNS) over the WiFi link. The IP stack
 * binds one nic, so this borrows it from the adapter carrying URC and (unless
 * asked to stay) hands it straight back - the harness in pc64_nettest.c owns
 * that dance because it is the file that knows every NIC. Weak-stubbed so the
 * driver still links in production builds, which do not compile the harness.
 *
 * The result is STASHED rather than returned live: URC is down for the ~30 s
 * the suite runs, so the reply to `iwl netup` cannot reach anyone. Read it back
 * with `iwl netres` once the link is up again. */
int pc64_wifi_ipsuite(uno_nic_t *nic, const unsigned char *mac, int stay,
                      char *out, int cap);
__attribute__((weak)) int pc64_wifi_ipsuite(uno_nic_t *nic, const unsigned char *mac,
                                            int stay, char *out, int cap)
{
    (void)nic; (void)mac; (void)stay; (void)cap;
    if (out && cap > 0) strcpy(out, "ip suite harness not built (UNO_DEBUG only)");
    return -1;
}
static char g_netres[320];

/* Dump what the firmware told us about itself: the capability bits the join
 * path branches on, the PHY_SKU-derived antenna masks, and the raw capa/api
 * bitmaps. Cheap, read-only, and it answers "does this fw advertise X?" without
 * costing a USB reflash - which is the whole reason it exists. */
static int caps_dump(char *out, int cap)
{
    static const struct { int bit; const char *name; } t[] = {
        { 12, "DQA" }, { 48, "ULTRA_HB_CHANNELS" }, { 54, "SESSION_PROT_CMD" },
        { 56, "PROTECTED_TWT" }, { 100, "BIGTK" }, { 104, "DRAM_FRAG" },
        { 110, "MLD_API" }, { 114, "STA_EXP_MFP" },
    };
    unsigned i;
    (void)cap;
    uno_dbg_net_trace("wifi: caps: phy_sku=%08x -> valid_tx_ant=%x valid_rx_ant=%x, "
                      "alive_notif_ver=%d n_scan_ch=%d",
                      g_fw.phy_sku, fw_valid_tx_ant(), fw_valid_rx_ant(),
                      g_fw.alive_notif_ver, g_fw.n_scan_channels);
    uno_dbg_net_trace("wifi: caps: our MAC %02x:%02x:%02x:%02x:%02x:%02x",
                      g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        uno_dbg_net_trace("wifi: caps:   capa %3d %-18s = %d", t[i].bit, t[i].name,
                          fw_has_capa(t[i].bit));
    for (i = 0; i < 16; i += 4)
        uno_dbg_net_trace("wifi: caps: capa[%2d..] %02x %02x %02x %02x   api[%2d..] %02x %02x %02x %02x",
                          i*8, g_fw.capa[i], g_fw.capa[i+1], g_fw.capa[i+2], g_fw.capa[i+3],
                          i*8, g_fw.api[i], g_fw.api[i+1], g_fw.api[i+2], g_fw.api[i+3]);
    strcpy(out, "ok caps dumped to the NET log (capa bits + antenna masks)");
    return (int)strlen(out);
}

#if UNO_DEBUG
/* Look up the short assert name for an fw error_id (mask off the CPU bits),
 * mirroring iwlwifi fw/img.c advanced_lookup[]. */
static const char *fwerr_name(u32 id)
{
    static const struct { const char *n; u32 v; } t[] = {
        {"NMI_INTERRUPT_WDG",0x34},{"SYSASSERT",0x35},{"UCODE_VERSION_MISMATCH",0x37},
        {"BAD_COMMAND",0x38},{"BAD_COMMAND",0x39},{"NMI_INTERRUPT_DATA_ACTION_PT",0x3C},
        {"FATAL_ERROR",0x3D},{"NMI_TRM_HW_ERR",0x46},{"NMI_INTERRUPT_TRM",0x4C},
        {"NMI_INTERRUPT_BREAK_POINT",0x54},{"NMI_INTERRUPT_WDG_RXF_FULL",0x5C},
        {"NMI_INTERRUPT_WDG_NO_RBD_RXF_FULL",0x64},{"NMI_INTERRUPT_HOST",0x66},
        {"NMI_INTERRUPT_LMAC_FATAL",0x70},{"NMI_INTERRUPT_UMAC_FATAL",0x71},
        {"NMI_INTERRUPT_OTHER_LMAC_FATAL",0x73},{"NMI_INTERRUPT_ACTION_PT",0x7C},
        {"NMI_INTERRUPT_UNKNOWN",0x84},{"NMI_INTERRUPT_INST_ACTION_PT",0x86},
        {"PNVM_MISSING",0x0010070d},
    };
    u32 m = id & ~0xf0000000u;   /* FW_SYSASSERT_CPU_MASK */
    unsigned i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].v == m) return t[i].n;
    return "ADVANCED_SYSASSERT";
}

/* Dump the fw error-event tables (iwl_error_event_table @ g_lmac_err_ptr and
 * iwl_umac_error_event_table @ g_umac_err_ptr, both parsed from the ALIVE
 * notif) to the NET debug log.  This is how we learn *which* assert fired and
 * where (error_id + name + program counter), instead of only that bit 25
 * SW_ERR went high in 0x2808.  Word indices per fw/dump.c. */
static void fwerr_dump(void)
{
    u32 L[24], U[15];
    uno_dbg_net_trace("wifi: FWERR lmac_ptr=%08x umac_ptr=%08x", g_lmac_err_ptr, g_umac_err_ptr);
    if (g_lmac_err_ptr) {
        mem_read(g_lmac_err_ptr, L, 24);
        uno_dbg_net_trace("wifi: LMAC valid=%08x error_id=%08x (%s) pc=%08x hcmd=%08x",
                          L[0], L[1], fwerr_name(L[1]), L[20], L[23]);
        uno_dbg_net_trace("wifi: LMAC blink2=%08x ilink1=%08x ilink2=%08x data1=%08x data2=%08x data3=%08x",
                          L[4], L[5], L[6], L[7], L[8], L[9]);
        uno_dbg_net_trace("wifi: LMAC ver maj=%08x min=%08x hw=%08x brd=%08x frame=%08x stack=%08x",
                          L[16], L[17], L[18], L[19], L[21], L[22]);
    }
    if (g_umac_err_ptr) {
        mem_read(g_umac_err_ptr, U, 15);
        uno_dbg_net_trace("wifi: UMAC valid=%08x error_id=%08x (%s) frame=%08x stack=%08x cmd=%08x",
                          U[0], U[1], fwerr_name(U[1]), U[11], U[12], U[13]);
        uno_dbg_net_trace("wifi: UMAC blink1=%08x blink2=%08x ilink1=%08x ilink2=%08x data1=%08x data2=%08x data3=%08x",
                          U[2], U[3], U[4], U[5], U[6], U[7], U[8]);
    }
}
#endif /* UNO_DEBUG */

int iwl_dbg_cmd(const char *line, char *out, int cap)
{
    static const char hx[] = "0123456789abcdef";
    u32 a, v;
    int i;
    if (!line || !out || cap < 12) return -1;
    /* Drain the RX ring on every command. Once the card is joined but the IP
     * stack is bound elsewhere (the debug rig: URC rides the ethernet), nothing
     * calls iwl_recv, the fw runs out of free RBs and eventually ASSERTS - which
     * is what happened minutes after the first successful DHCP run. */
    if (g_bound && g_joined) rx_pump_once();
    if (!strncmp(line, "dmar off", 8)) { iommu_disable(out, cap); return (int)strlen(out); }
    if (!strncmp(line, "dmar", 4)) return dmar_check(out, cap);
    if (!strncmp(line, "rerun", 5)) {
        /* If a previous attempt reached ALIVE, the ROM is still running and
         * DMAing.  Re-running the full bring-up on top of that wedged the Yoga
         * hard once (prepare_card_hw runs the ownership handshake against a
         * live, busy device before the normal device_stop can quiesce it).
         * So halt the device FIRST - STOP_MASTER + sw reset via device_stop -
         * then re-init from a stopped device, matching a clean boot. */
        if (g_bar && g_fw_loaded) {
            uno_dbg_net_trace("wifi: rerun: firmware was live - halting DMA (device_stop) before re-init");
            device_stop();
        }
        g_bound = 0; g_joined = 0; g_alive = 0; g_mvm_done = 0;   /* force a full retry */
        g_keys_installed = 0; g_key_gtk_idx = -1; g_wpa_active = 0; g_data_qid = -1; g_ctx_built = 0;
        iwl_nic();
        iwl_status_str(out, cap);
        return (int)strlen(out);
    }
    if (!strncmp(line, "status", 6)) { iwl_status_str(out, cap); return (int)strlen(out); }
    if (!strncmp(line, "join", 4)) {
        /* Full pre-auth association setup against the configured SSID: scan ->
         * pick real BSSID/chan -> phy/mac/binding ctx -> time-quota/assoc-window
         * -> ADD_STA(AP peer) -> allocate the TX queue. Detail + fw health to the
         * NET log; the auth/assoc frame exchange is the next slice. */
        int q; u32 h;
        if (!g_bar || !g_alive) { strcpy(out, "err not ALIVE - run rerun then mvm 1 first"); return (int)strlen(out); }
        mvm_scan_cfg();
        mvm_scan_passive(5000);
        if (scan_pick() < 0) {
            uno_dbg_net_trace("wifi: join: SSID \"%s\" not found in %d scanned APs", g_cfg_ssid, g_scan_ap_n);
            strcpy(out, "err SSID not in scan (detail in NET log)"); return (int)strlen(out);
        }
        uno_dbg_net_trace("wifi: join: picked \"%s\" bssid %02x:%02x:%02x:%02x:%02x:%02x "
            "chan %d bi %d dtim %d (link-api=%d)",
            g_cfg_ssid, g_bssid[0],g_bssid[1],g_bssid[2],g_bssid[3],g_bssid[4],g_bssid[5],
            g_join_chan, g_join_bi, g_join_dtim, fw_has_mld_api());
        q = assoc_setup();
        h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
        { char *o = out; const char *p = h ? "err join: fw ASSERTED (iwl fwerr); qid=" : "ok join setup: qid=";
          int v = q, m = 0, j; char d[8];
          while (*p) *o++ = *p++;
          if (v < 0) { *o++ = 0x2d; v = -v; }
          if (v == 0) d[m++] = 0x30; else while (v) { d[m++] = (char)(0x30 + v % 10); v /= 10; }
          for (j = m - 1; j >= 0; j--) *o++ = d[j];
          p = " (detail in NET log)"; while (*p) *o++ = *p++; *o = 0; }
        return (int)strlen(out);
    }
    if (!strncmp(line, "auth", 4)) {
        int r; u32 h;
        if (!g_bar || !g_alive || g_data_qid < 0) { strcpy(out, "err run iwl join first (need TX queue)"); return (int)strlen(out); }
        /* Airtime for the auth window. This QuZ-77 fw is SESSION_PROTECTION-based:
         * CMD_VERSIONS advertises MAC_CONF 0xfb SESSION_PROT, and the legacy
         * TIME_EVENT_CMD (0x29) is ABSENT from its command table - sending it
         * ADVANCED_SYSASSERTs (cmd=0x0129 in the UMAC error dump). So use
         * SESSION_PROTECTION_CMD (mvm_assoc_window capa-54 path). Without a window
         * the radio is not parked on-channel (0 RX in join state). */
        if (fw_has_capa(54)) {
            mvm_assoc_window();
        } else {
            uno_dbg_net_trace("wifi: auth: no SESSION_PROT capa54 - skipping airtime (TIME_EVENT asserts this fw)");
        }
        h = r32(CSR_MSIX_HW_INT_CAUSES_AD);
        uno_dbg_net_trace("wifi: auth: after session-prot csr2808=%08x", h);
        if (h) { strcpy(out, "err auth: session-prot asserted fw (iwl fwerr)"); return (int)strlen(out); }
        /* Authenticate the way the selected BSS asks for.  A step verb that
         * always spoke Open System would report "no response" on every WPA3
         * network and send whoever typed it hunting in the wrong place. */
        if (g_join_akm == WPA_AKM_SAE) {
            if (sae_prepare() < 0) { strcpy(out, "err auth: SAE prepare failed (see NET log)"); return (int)strlen(out); }
            r = mvm_auth_sae();
            strcpy(out, r == 0 ? "ok auth: SAE accepted" : "err auth: SAE failed (see NET log)");
            return (int)strlen(out);
        }
        r = mvm_auth();
        uno_dbg_net_trace("wifi: auth -> %d (0=ok, >0 AP status, -1 no resp)", r);
        strcpy(out, r == 0 ? "ok auth: Open-System accepted" : (r < 0 ? "err auth: no response" : "err auth: AP rejected"));
        return (int)strlen(out);
    }
    if (!strncmp(line, "assoc", 5)) {
        int r;
        if (!g_bar || !g_alive || g_data_qid < 0) { strcpy(out, "err run iwl join + auth first"); return (int)strlen(out); }
        wpa_arm();              /* before the request: EAPOL 1/4 lands inside mvm_assoc() */
        r = mvm_assoc();
        uno_dbg_net_trace("wifi: assoc -> %d (>=0 AID, <0 fail)", r);
        if (r >= 0) {
            assoc_mark_associated(r);
            strcpy(out, "ok assoc: associated, supplicant armed (pump RX for 4-way)");
        } else strcpy(out, "err assoc: failed (detail in NET log)");
        return (int)strlen(out);
    }
    if (!strncmp(line, "eapol", 5)) {
        /* pump RX for a few seconds so the AP EAPOL 4-way frames get handled */
        rx_pump_ms(4000, 1);
        strcpy(out, g_joined ? "ok eapol: 4-way DONE, station authorized" : "err eapol: no/incomplete handshake (NET log)");
        return (int)strlen(out);
    }
    /* "iwl data [a.b.c.d]" - prove the encrypted data path without touching the
     * IP stack (which is bound to the ethernet carrying this very command). With
     * an IP it ARPs for it; without one it just listens for broadcast traffic. */
    if (!strncmp(line, "data", 4)) {
        const char *q = line + 4; u8 ip[4]; int k, any = 0;
        while (*q == 0x20) q++;
        for (k = 0; k < 4 && *q; k++) {
            int v = 0, d = 0;
            while (*q >= 0x30 && *q <= 0x39) { v = v*10 + (*q - 0x30); q++; d = 1; }
            if (!d) break;
            ip[k] = (u8)v; if (k == 3) any = 1;
            if (*q == 0x2e) q++;
        }
        return probe_data(any ? ip : 0, any ? 1500 : 4000, out, cap);
    }
    /* "iwl retarget <n>" - point the LIVE contexts at scan result n and make the
     * station ready to auth again, without restarting the radio: STA_CONFIG with
     * the new peer, then free + re-allocate its TX queue (the queue does not
     * survive the station changing address). Follow with `iwl auth`. */
    if (!strncmp(line, "retarget", 8)) {
        const char *q = line + 8; int n = 0, any = 0;
        while (*q == 0x20) q++;
        while (*q >= 0x30 && *q <= 0x39) { n = n*10 + (*q - 0x30); q++; any = 1; }
        if (!g_bar || !g_alive) { strcpy(out, "err not ALIVE"); return (int)strlen(out); }
        if (!any || n >= g_scan_ap_n) { strcpy(out, "err usage: iwl retarget <n> (from the last scan)");
                                        return (int)strlen(out); }
        select_ap(n);
        uno_dbg_net_trace("wifi: retarget -> [%d] %02x:%02x:%02x:%02x:%02x:%02x chan %d \"%s\"",
                          n, g_bssid[0],g_bssid[1],g_bssid[2],g_bssid[3],g_bssid[4],g_bssid[5],
                          g_join_chan, g_scan_aps[n].ssid);
        if (retarget_ap(g_join_chan) == 0) strcpy(out, "ok retarget: station re-pointed, fresh TX queue - now 'iwl auth'");
        else                               strcpy(out, "err retarget: failed (detail in NET log)");
        return (int)strlen(out);
    }
    /* "iwl netres" - the stashed result of the last netup/netwifi run. */
    if (!strncmp(line, "netres", 6)) {
        strcpy(out, g_netres[0] ? g_netres : "no IP suite has run yet (iwl netup)");
        return (int)strlen(out);
    }
    /* "iwl netup" - real DHCP/ping/DNS over WiFi, then hand the stack back to
     * the management NIC. "iwl netwifi" leaves it on WiFi (URC redials over the
     * WiFi link - only do that once netup has shown a lease). */
    if (!strncmp(line, "netup", 5) || !strncmp(line, "netwifi", 7)) {
        int stay = (line[3] == 0x77);              /* netWifi */
        if (!g_joined) { strcpy(out, "err not joined - connect first"); return (int)strlen(out); }
        g_netres[0] = 0;
        pc64_wifi_ipsuite(&g_nic, g_mac, stay, g_netres, (int)sizeof g_netres);
        strcpy(out, g_netres[0] ? g_netres : "err ip suite produced nothing");
        return (int)strlen(out);
    }
    /* "iwl qos <0|1>" - plain data frames vs QoS data frames. Our assoc request
     * carries no WMM IE (so the AP admits us as non-QoS) and the fw queue is on
     * TID 15, hence plain by default; this flips it without a reflash. */
    if (!strncmp(line, "qos", 3)) {
        const char *q = line + 3;
        while (*q == 0x20) q++;
        if (*q == 0x30 || *q == 0x31) g_tx_qos = (*q == 0x31);
        strcpy(out, g_tx_qos ? "ok qos: TX as QoS data (subtype 8, 26 B hdr)"
                             : "ok qos: TX as plain data (subtype 0, 24 B hdr)");
        return (int)strlen(out);
    }
    /* "iwl txlog <n>" - re-arm the per-frame TX log for n more frames (0 =
     * silent). Ungated it floods the kernel ring and buries everything else -
     * see the note at the trace site - so it runs on a budget and this is how
     * you buy more of it when you are actually looking at the TX path. */
    if (!strncmp(line, "txlog", 5)) {
        const char *q = line + 5;
        int n = 0;
        while (*q == 0x20) q++;
        while (*q >= 0x30 && *q <= 0x39) { n = n * 10 + (*q - 0x30); q++; }
        g_tx_log = n;
        uno_snprintf_hex(out, cap, "ok txlog: next ", (u32)n, " TX frames will be logged");
        return (int)strlen(out);
    }
    /* "iwl band <24|5|any>" - which band scan_pick() may choose from. */
    if (!strncmp(line, "band", 4)) {
        const char *q = line + 4;
        while (*q == 0x20) q++;
        if (!strncmp(q, "24", 2))      g_band_pref = 0;
        else if (*q == 0x35)           g_band_pref = 1;
        else if (!strncmp(q, "any", 3)) g_band_pref = 2;
        strcpy(out, g_band_pref == 0 ? "ok band: 2.4 GHz only" :
                    g_band_pref == 1 ? "ok band: 5 GHz only" : "ok band: either");
        return (int)strlen(out);
    }
    /* "iwl sec" - what security each scanned BSS offers, and what we would
     * negotiate with it.  Answers "why will this network not join" without a
     * capture: an SSID whose row says "none" is one we cannot speak to. */
    if (!strncmp(line, "sec", 3)) {
        int i, n = 0;
        /* A row is at most 32 (SSID) + ~64 of fixed text.  Stop while a whole
         * one still fits, and clamp on snprintf's return: it reports what it
         * WOULD have written, so adding it blind can push n past cap and turn
         * the next `cap - n` into a huge size_t. */
        for (i = 0; i < g_scan_ap_n && n < cap - 128; i++) {
            const wpa_ap_sec_t *s = &g_scan_aps[i].sec;
            int w = snprintf(out + n, (size_t)(cap - n),
                             "%s%-20s akm=%02x pair=%02x mfpc=%d mfpr=%d h2e=%d -> %s",
                             n ? "\n" : "", g_scan_aps[i].ssid,
                             s->akm, s->pairwise, s->mfpc, s->mfpr, s->h2e,
                             akm_name(wpa_pick_akm(s)));
            if (w < 0) break;
            n += (w > cap - n) ? (cap - n) : w;
        }
        if (!n) strcpy(out, "ok sec: no scan results - run iwl scan first");
        return (int)strlen(out);
    }
    /* "iwl igtk <0|1>" - hand the BIP (broadcast management) key to the
     * firmware on the next PMF join.  Off by default; see the install site. */
    if (!strncmp(line, "igtk", 4)) {
        const char *q = line + 4;
        while (*q == 0x20) q++;
        if (*q == 0x30 || *q == 0x31) g_igtk_install = (*q == 0x31);
        strcpy(out, g_igtk_install ? "ok igtk: will be installed on the next join"
                                   : "ok igtk: held but not installed");
        return (int)strlen(out);
    }
    /* "iwl connect [ssid|psk]" - the whole join in ONE command, the same path
     * the Network app's join button takes. With no argument it uses WIFI.CFG. */
    if (!strncmp(line, "connect", 7)) {
        const char *q = line + 7; char ssid[36], psk[80]; int k = 0;
        while (*q == 0x20) q++;
        if (*q) {
            while (*q && *q != 0x7c && k < (int)sizeof ssid - 1) ssid[k++] = *q++;
            while (k > 0 && ssid[k-1] == 0x20) k--;
            ssid[k] = 0;
            if (*q == 0x7c) q++;
            k = 0; while (*q && k < (int)sizeof psk - 1) psk[k++] = *q++;
            psk[k] = 0;
        } else {
            /* No argument = "use WIFI.CFG" - but the file is only read inside
             * iwl_nic(), so on a cold box g_cfg_ssid is still empty here and the
             * join bailed instantly ("err connect:" with no trace at all).
             * Bring the radio up first; that reads the credentials. */
            if (!g_cfg_ssid[0]) radio_up();
            strcpy(ssid, g_cfg_ssid); strcpy(psk, g_cfg_psk);
            if (!ssid[0]) { strcpy(out, "err connect: no ssid (WIFI.CFG/WIFI.TXT missing?)");
                            return (int)strlen(out); }
        }
        if (iwl_join_ssid(ssid, psk) == 0) iwl_status_str(out, cap);
        else { strcpy(out, "err connect: "); iwl_status_str(out + 13, cap - 13); }
        return (int)strlen(out);
    }
    if (!strncmp(line, "scan", 4)) {
        int n, i;
        if (!g_bar || !g_alive) { strcpy(out, "err fw not ALIVE - run 'iwl rerun' (then 'iwl mvm 1') first"); return (int)strlen(out); }
        mvm_scan_cfg();
        n = mvm_scan_passive(5000);
        for (i = 0; i < n; i++)
            uno_dbg_net_trace("wifi: scan[%d] %02x:%02x:%02x:%02x:%02x:%02x ch=%d rssi=%d seen=%d ssid=\"%s\"",
                              i, g_scan_aps[i].bssid[0], g_scan_aps[i].bssid[1], g_scan_aps[i].bssid[2],
                              g_scan_aps[i].bssid[3], g_scan_aps[i].bssid[4], g_scan_aps[i].bssid[5],
                              g_scan_aps[i].chan, g_scan_aps[i].rssi, g_scan_aps[i].seen, g_scan_aps[i].ssid);
        /* also pick the configured SSID so the 'iwl mld <n>' stepper has a real
         * BSSID/channel to work with without going through 'iwl join' */
        if (scan_pick() == 0)
            uno_dbg_net_trace("wifi: scan: picked \"%s\" bssid %02x:%02x:%02x:%02x:%02x:%02x "
                              "chan %d bi %d dtim %d", g_cfg_ssid,
                              g_bssid[0],g_bssid[1],g_bssid[2],g_bssid[3],g_bssid[4],g_bssid[5],
                              g_join_chan, g_join_bi, g_join_dtim);
        else
            uno_dbg_net_trace("wifi: scan: SSID \"%s\" not among the results", g_cfg_ssid);
        { char *o = out; const char *pre = "ok scan done: "; int v = n, j;
          char digs[8]; int m = 0;
          while (*pre) *o++ = *pre++;
          if (v == 0) digs[m++] = '0'; else { while (v) { digs[m++] = (char)('0' + v % 10); v /= 10; } }
          for (j = m - 1; j >= 0; j--) *o++ = digs[j];
          pre = " APs (detail in NET log)"; while (*pre) *o++ = *pre++; *o = 0; }
        return (int)strlen(out);
    }
    if (!strncmp(line, "caps", 4)) return caps_dump(out, cap);
    /* "iwl pick <n>" - target the n-th AP from the last scan instead of whatever
     * scan_pick() chose. NimmuNet is a multi-BSSID mesh across both bands and the
     * DS-Parameter-Set channel we harvest has proven unreliable, so being able to
     * try each candidate over URC beats a USB reflash per guess. */
    if (!strncmp(line, "pick", 4)) {
        const char *q = line + 4; int n = 0, any = 0;
        while (*q == 0x20) q++;
        while (*q >= 0x30 && *q <= 0x39) { n = n*10 + (*q - 0x30); q++; any = 1; }
        if (!any || n >= g_scan_ap_n) {
            int i;
            for (i = 0; i < g_scan_ap_n; i++)
                uno_dbg_net_trace("wifi: pick[%d] %02x:%02x:%02x:%02x:%02x:%02x ch=%d seen=%d ssid=\"%s\"",
                                  i, g_scan_aps[i].bssid[0], g_scan_aps[i].bssid[1],
                                  g_scan_aps[i].bssid[2], g_scan_aps[i].bssid[3],
                                  g_scan_aps[i].bssid[4], g_scan_aps[i].bssid[5],
                                  g_scan_aps[i].chan, g_scan_aps[i].seen, g_scan_aps[i].ssid);
            strcpy(out, "usage: iwl pick <n> - candidates listed in the NET log");
            return (int)strlen(out);
        }
        memcpy(g_bssid, g_scan_aps[n].bssid, 6);
        g_join_chan = g_scan_aps[n].chan ? g_scan_aps[n].chan : 1;
        g_join_bi = g_scan_aps[n].bi ? g_scan_aps[n].bi : 100;
        g_join_dtim = g_scan_aps[n].dtim ? g_scan_aps[n].dtim : 1;
        uno_dbg_net_trace("wifi: pick: %02x:%02x:%02x:%02x:%02x:%02x chan %d \"%s\" - "
                          "re-run 'iwl mld 3'..'6' then 'iwl auth'",
                          g_bssid[0],g_bssid[1],g_bssid[2],g_bssid[3],g_bssid[4],g_bssid[5],
                          g_join_chan, g_scan_aps[n].ssid);
        strcpy(out, "ok pick: target set (detail in NET log)");
        return (int)strlen(out);
    }
    if (!strncmp(line, "mld", 3)) {              /* "iwl mld <n>" - link-API bisect */
        const char *q = line + 3;
        while (*q == 0x20) q++;
        if (!g_bar) { strcpy(out, "no BAR0 (run rerun first)"); return (int)strlen(out); }
        if (*q == 0x61) return mld_steps(10, out, cap);      /* "iwl mld a" */
        if (*q < 0x31 || *q > 0x39) {
            strcpy(out, "err usage: iwl mld <1-9|a> (1mac 2link 3phy 4link-active 5sta 6txq 7sessprot 8auth 9assoc a=post-assoc qos/beacon)");
            return (int)strlen(out);
        }
        return mld_steps(*q - 0x30, out, cap);
    }
    if (!strncmp(line, "mvm", 3)) {
        const char *q = line + 3;
        while (*q == 0x20) q++;
        if (*q >= 0x31 && *q <= 0x39) {              /* "iwl mvm <n>" - stepped bisect */
            if (!g_bar) { strcpy(out, "no BAR0 (run rerun first)"); return (int)strlen(out); }
            return mvm_steps(*q - 0x30, out, cap);
        }
        if (*q >= 0x61 && *q <= 0x66) {              /* "iwl mvm a..d" split, e/f extras */
            if (!g_bar) { strcpy(out, "no BAR0 (run rerun first)"); return (int)strlen(out); }
            return mvm_steps(10 + (*q - 0x61), out, cap);
        }
        g_mvm_arm = 1;                                /* bare "iwl mvm" - arm the inline run */
        strcpy(out, "post-ALIVE MVM/join sequence armed - now run 'iwl rerun' (or 'iwl mvm <n>' to step)");
        return (int)strlen(out);
    }
    if (!strncmp(line, "alive", 5)) {
        int upto = 1;
        const char *q = line + 5;
        while (*q == 0x20) q++;
        if (*q >= 0x31 && *q <= 0x39) upto = *q - 0x30;
        if (!g_bar) { strcpy(out, "no BAR0 (run rerun first)"); return (int)strlen(out); }
        return alive_steps(upto, out, cap);
    }
    if (!strncmp(line, "msix", 4)) {
        g_msix_arm = 1;
        strcpy(out, "MSI-X table arming ON - now run 'iwl rerun'");
        return (int)strlen(out);
    }
#if UNO_DEBUG
    if (!strncmp(line, "iotrace", 7)) {
        iot_dump();
        strcpy(out, "iotrace dumped to the NET debug channel");
        return (int)strlen(out);
    }
#endif
    if (!g_bar) { strcpy(out, "no BAR0 (run rerun first)"); return (int)strlen(out); }
    if (!strncmp(line, "csr ", 4))  { const char *p = line + 4;
        if (hex_u32(&p, &a) < 0) return -1;
        v = r32(a); goto hexout; }
    if (!strncmp(line, "csw ", 4))  { const char *p = line + 4;
        if (hex_u32(&p, &a) < 0 || hex_u32(&p, &v) < 0) return -1;
        w32(a, v); strcpy(out, "ok"); return 2; }
    if (!strncmp(line, "prr ", 4))  { const char *p = line + 4;
        if (hex_u32(&p, &a) < 0) return -1;
        v = prph_r(a); goto hexout; }
    if (!strncmp(line, "prw ", 4))  { const char *p = line + 4;
        if (hex_u32(&p, &a) < 0 || hex_u32(&p, &v) < 0) return -1;
        prph_w(a, v); strcpy(out, "ok"); return 2; }
#if UNO_DEBUG
    if (!strncmp(line, "fwerr", 5)) {          /* dump the fw error-event tables */
        fwerr_dump();
        strcpy(out, "ok fwerr dumped to NET log (lmac/umac error tables)");
        return (int)strlen(out); }
    if (!strncmp(line, "mem ", 4))  {           /* iwl mem <hex> [nwords] */
        const char *p = line + 4; u32 n = 8, buf[64]; unsigned j;
        if (hex_u32(&p, &a) < 0) return -1;
        while (*p == 0x20) p++;
        if (*p && hex_u32(&p, &n) < 0) return -1;
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        mem_read(a, buf, (int)n);
        for (j = 0; j < n; j += 4)
            uno_dbg_net_trace("wifi: MEM %08x: %08x %08x %08x %08x", a + j*4,
                              buf[j], j+1<n?buf[j+1]:0, j+2<n?buf[j+2]:0, j+3<n?buf[j+3]:0);
        v = buf[0]; goto hexout; }
#endif
    return -1;
hexout:
    for (i = 0; i < 8; i++) out[i] = hx[(v >> ((7 - i) * 4)) & 0xF];
    out[8] = 0;
    return 8;
}

/* ---- status line -------------------------------------------------------- */
static void ss_cat(char *b, int cap, int *i, const char *s)
{ while (*s && *i < cap - 1) b[(*i)++] = *s++; b[*i] = 0; }
static void ss_dec(char *b, int cap, int *i, int v)
{ char t[12]; int m = 0;
  if (v < 0) { if (*i < cap - 1) b[(*i)++] = '-'; v = -v; }
  if (!v) t[m++] = '0';
  while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
  while (m && *i < cap - 1) b[(*i)++] = t[--m];
  b[*i] = 0; }
static void ss_mac(char *b, int cap, int *i, const u8 *m)
{ const char *h = "0123456789abcdef"; int k;
  for (k = 0; k < 6; k++) {
      if (*i < cap - 2) { b[(*i)++] = h[m[k] >> 4]; b[(*i)++] = h[m[k] & 15]; }
      if (k < 5 && *i < cap - 1) b[(*i)++] = ':';
  }
  b[*i] = 0; }

/* The live association state, not the last bring-up STAGE message. g_status is
 * where a bring-up FAILURE (or the pre-MVM gate) is recorded, and returning it
 * unconditionally is why a fully joined card still reported "MVM bring-up
 * gated; 'iwl mvm' to continue" on metal (round 24). */
void iwl_status_str(char *buf, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    if (g_alive && (g_joined || g_wpa_active)) {
        ss_cat(buf, cap, &i, g_joined ? "WiFi joined \"" : "WiFi associating \"");
        ss_cat(buf, cap, &i, g_ssid_str[0] ? g_ssid_str : g_cfg_ssid);
        ss_cat(buf, cap, &i, "\" bssid ");
        ss_mac(buf, cap, &i, g_bssid);
        ss_cat(buf, cap, &i, " ch ");   ss_dec(buf, cap, &i, g_join_chan);
        if (g_last_rssi) { ss_cat(buf, cap, &i, " "); ss_dec(buf, cap, &i, g_last_rssi);
                           ss_cat(buf, cap, &i, "dBm"); }
        ss_cat(buf, cap, &i, " aid ");  ss_dec(buf, cap, &i, g_aid);
        ss_cat(buf, cap, &i, g_keys_installed ? " CCMP keys in" : " 4-way pending");
        ss_cat(buf, cap, &i, " tx ");   ss_dec(buf, cap, &i, (int)g_tx_data_n);
        ss_cat(buf, cap, &i, " rx ");   ss_dec(buf, cap, &i, (int)g_rx_data_n);
        if (g_rx_data_drop) { ss_cat(buf, cap, &i, " drop "); ss_dec(buf, cap, &i, (int)g_rx_data_drop); }
        if (g_bar && r32(CSR_MSIX_HW_INT_CAUSES_AD)) ss_cat(buf, cap, &i, " [fw ASSERTED]");
        return;
    }
    /* A link the AP tore down must SAY so. Falling through to g_status left the
     * last happy string ("WiFi bound: SKYNET (joined)") on screen while the
     * association was gone, so the UI and the log disagreed about the one fact
     * that mattered. */
    if (g_link_lost_reason >= 0 && !g_joined) {
        ss_cat(buf, cap, &i, "WiFi: deauthenticated by the AP (reason ");
        ss_dec(buf, cap, &i, g_link_lost_reason);
        ss_cat(buf, cap, &i, ") - press Join to reconnect");
        buf[i < cap ? i : cap - 1] = 0;
        return;
    }
    while (g_status[i] && i < cap-1) { buf[i] = g_status[i]; i++; }
    buf[i] = 0;
}
