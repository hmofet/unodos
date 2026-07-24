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
typedef EFI_STATUS (*EFI_ALLOC_PAGES)(UINTN Type, UINTN MemType, UINTN Pages,
                                      unsigned long long *Memory);

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

void uno_pc64_delay_ms(int ms);

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
static char g_fwfile[20];    /* 8.3 name under FIRMWARE\ on the ESP */
static char g_pnvmfile[20];
static u8   g_mac[6];
static u8   g_joined;
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

/* decode CSR_HW_REV / CSR_HW_RF_ID and choose the firmware file name */
static void choose_firmware(void)
{
    u32 mac_type = (g_hw_rev >> 4) & 0xFFF;
    (void)mac_type;
    g_gen2  = (g_family >= FAM_22000);
    g_mq_rx = (g_family >= FAM_9000);
    if (g_family >= FAM_AX210) g_prph_mask = 0x00FFFFFF;

    g_pnvmfile[0] = 0;
    switch (g_family) {
    case FAM_7000:  strcpy(g_fwfile, "FIRMWARE\\IWL7260.UCO"); break;
    case FAM_8000:  strcpy(g_fwfile, "FIRMWARE\\IWL8000.UCO"); break;
    case FAM_9000:
        /* 9260 (device 0x2526/0x271b/0x271c) uses the th-b0-jf-b0 image;
           9461/9462/9560 use the pu-b0-jf-b0 image - different upstream files. */
        if (g_pci.device==0x2526 || g_pci.device==0x271b || g_pci.device==0x271c)
             strcpy(g_fwfile, "FIRMWARE\\IWL9260.UCO");
        else strcpy(g_fwfile, "FIRMWARE\\IWL9000.UCO");
        break;
    case FAM_22000:
        /* AX200 (discrete, device 0x2723) uses the cc-a0 image; AX201 (Qu/QuZ
           CNVi) uses the Qu-b0-hr-b0 image - different files. */
        if (g_pci.device == 0x2723) strcpy(g_fwfile, "FIRMWARE\\IWLAX200.UCO");
        else                        strcpy(g_fwfile, "FIRMWARE\\IWLAX201.UCO");
        break;
    case FAM_AX210:
        /* Ty (0x2725) uses ty-a0-gf-a0; So/Ma parts (AX211/AX411) use
           so-a0-gf-a0 - separate files. Both need a matching PNVM. */
        if (g_pci.device == 0x2725) { strcpy(g_fwfile,"FIRMWARE\\IWLAX210.UCO");
                                      strcpy(g_pnvmfile,"FIRMWARE\\IWLAX210.PNV"); }
        else                        { strcpy(g_fwfile,"FIRMWARE\\IWLAX211.UCO");
                                      strcpy(g_pnvmfile,"FIRMWARE\\IWLAX211.PNV"); }
        break;
    case FAM_BZ:
        /* WiFi 7 Bz/Gl. Best-effort: routed through the gen3 loader, but Bz adds
           a TOP-reset + ROM-start handshake this driver does not yet perform, so
           this is metal-pending even by the family's standard. */
        if (g_pci.device == 0x272b) { strcpy(g_fwfile,"FIRMWARE\\IWLBE200.UCO");   /* Gl discrete */
                                      strcpy(g_pnvmfile,"FIRMWARE\\IWLBE200.PNV"); }
        else                        { strcpy(g_fwfile,"FIRMWARE\\IWLBE201.UCO");   /* Bz */
                                      strcpy(g_pnvmfile,"FIRMWARE\\IWLBE201.PNV"); }
        break;
    case FAM_SC:
        strcpy(g_fwfile,   "FIRMWARE\\IWLBE211.UCO");
        strcpy(g_pnvmfile, "FIRMWARE\\IWLBE211.PNV"); break;
    }
}

/* the ESP volume that holds FIRMWARE\ and the credentials file. WIFI.CFG is
 * the documented name; WIFI.TXT is accepted too - it is what the flasher's
 * developer-options folder copy stages from the NAS creds template. */
static char g_cfgname[12];
static int firmware_volume(void)
{
    int n = uno_fs_volumes(), i;
    for (i = 0; i < n; i++)
        if (uno_fs_kind(i) == 2 || uno_fs_kind(i) == 1) {   /* firmware SFS / native FAT */
            if (uno_fs_size(i, "WIFI.CFG") > 0) { strcpy(g_cfgname, "WIFI.CFG"); return i; }
            if (uno_fs_size(i, "WIFI.TXT") > 0) { strcpy(g_cfgname, "WIFI.TXT"); return i; }
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
    EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    g_arena_used = 0;
    if (g_arena) return;                /* once per boot */
    if (ST) {
        unsigned long long mem = 0x00000000FFFFF000ull;   /* ceiling: below 4GB */
        UINTN pages = (FW_ARENA_MAX + 4095) / 4096;
        if (((EFI_ALLOC_PAGES)ST->BootServices->AllocatePages)(
                1 /*AllocateMaxAddress*/, 2 /*EfiLoaderData*/, pages, &mem) == EFI_SUCCESS)
            g_arena = (u8 *)(uintptr_t)mem;
    }
    if (!g_arena) g_arena = g_arena_static;             /* fallback (may be >4GB) */
    g_arena_phys = (u64)(uintptr_t)g_arena;
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
static void device_stop(void)
{
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
static u8  g_cmd_buf[CMDQ_N][512]     __attribute__((aligned(64)));   /* per-slot command DRAM */
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
    return g_rbstts.closed_rb_num & 0xFFF;
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

/* Enqueue an 802.11 frame on the data TX queue wrapped in a TX_CMD. gen1 uses
 * iwl_tx_cmd_v6 (56-byte params), gen2/gen3 use the shorter v9/gen3 header; the
 * frame's bytes follow the header. Encryption is done by the card from the
 * installed CCMP key (sec_ctl / the station's key). `high_pri` marks EAPOL so
 * it isn't starved during the handshake. Metal-pending: the TX_CMD field detail
 * varies by firmware version (fwapi ref Part 6). */
static void tx_enqueue(const u8 *frame, int flen, int high_pri)
{
    int idx = g_tx_wr & (TXQ_N - 1);
    u8 *out = g_tx_buf[idx];
    int hdrlen, tb0, total, qid;
    if (flen <= 0 || flen > 2048 - 64) return;

    memset(out, 0, 64);
    if (g_gen2) {
        /* iwl_tx_cmd_v9: len@0, offload_assist@2, flags@4, dram_info@8, r_n_f@16 */
        out[0] = (u8)flen; out[1] = (u8)(flen >> 8);
        if (high_pri) out[4] = (1u<<2);          /* IWL_TX_FLAGS_HIGH_PRI */
        { u32 rnf = 10 | (1u<<9) | (1u<<14);      /* 1M CCK, ant A (safe mgmt rate) */
          out[16]=(u8)rnf; out[17]=(u8)(rnf>>8); out[18]=(u8)(rnf>>16); out[19]=(u8)(rnf>>24); }
        hdrlen = 20;
    } else {
        /* iwl_tx_cmd_v6 params: len@0, tx_flags@4, rate_n_flags@12, sta_id@16 */
        out[0] = (u8)flen; out[1] = (u8)(flen >> 8);
        { u32 fl = (1u<<3); *(u32*)(out+4) = fl; }   /* TX_CMD_FLG_ACK */
        { u32 rnf = 10 | (1u<<9) | (1u<<14); *(u32*)(out+12) = rnf; }
        out[16] = AP_STA_ID;
        out[17] = high_pri ? 0 : (2 | 0x10);         /* sec_ctl CCM|KEY_FROM_TABLE for data */
        hdrlen = 56;
    }
    memcpy(out + hdrlen, frame, flen);
    total = hdrlen + flen;

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
        g_tx_bc[idx] = (u16)(((total + 3)/4) | (nchunks << 12));
    } else {
        struct tfd *t = (struct tfd *)(g_tx_ring + idx*128);
        memset(t, 0, sizeof *t);
        tfd_set_tb_gen1(t, phys(g_tx_firsttb[idx]), tb0);
        if (total > tb0) tfd_set_tb_gen1(t, phys(out + tb0), total - tb0);
        g_tx_bc[idx] = (u16)((total + 3)/4);
    }
    qid = g_data_qid >= 0 ? g_data_qid : 10;     /* DATA pool base until fw assigns */
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

static void handle_data_frame(const u8 *frame, int len);   /* fwd (802.11->eth) */
static void handle_eapol(const u8 *frame, int len);        /* fwd */
static const u8 SNAP[6] = { 0xAA,0xAA,0x03,0x00,0x00,0x00 };

/* process one received RB: walk packed iwl_rx_packet records */
static void rx_process_rb(const u8 *rb, int cap,
                          int want_group, int want_cmd, const u8 **found, int *found_len)
{
    int off = 0;
    while (off + 8 <= cap) {
        const struct rx_packet *pkt = (const struct rx_packet *)(rb + off);
        int plen = pkt->len_n_flags & FRAME_SIZE_MSK;
        if (plen < 4 || off + 4 + plen > cap) break;
        if (found && !*found && pkt->group_id == want_group && pkt->cmd == want_cmd) {
            *found = pkt->data; *found_len = plen - 4;
        }
        /* RX MPDU (a received 802.11 frame) — dispatch EAPOL vs data regardless
           of what we're waiting for, so the handshake makes progress. */
        if (pkt->group_id == 0 && pkt->cmd == 0xc1) {         /* REPLY_RX_MPDU */
            const u8 *frame; int fl, machdr;
            if (g_mq_rx) {
                /* iwl_rx_mpdu_desc: mpdu_len@0, mac_flags2@3 (PAD 0x20,
                   HDR_LEN in *2 words); desc size differs v1(32)/v3(40). */
                int descsz = (g_family >= FAM_AX210) ? 40 : 32;
                int mlen = pkt->data[0] | (pkt->data[1] << 8);
                int pad = (pkt->data[3] & 0x20) ? 2 : 0;
                frame = pkt->data + descsz + pad; fl = mlen;
                machdr = (pkt->data[3] & 0x1f) * 2;
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
                int hl = machdr ? machdr : (qos ? 26 : 24);
                if (fl > hl + 8) {
                    const u8 *llc = frame + hl;
                    u16 et = (u16)((llc[6] << 8) | llc[7]);
                    if (!memcmp(llc, SNAP, 6) && et == 0x888E) handle_eapol(frame, fl);
                    else if (!memcmp(llc, SNAP, 6))            handle_data_frame(frame, fl);
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
            int vid = g_mq_rx ? (int)(g_rbd_used[g_rx_read] & 0xFFF) : (g_rx_read + 1);
            const u8 *rb = (vid >= 1 && vid <= RXQ_N) ? g_rb[vid-1] : g_rb[g_rx_read];
            rx_process_rb(rb, RB_SIZE, group, cmd, &found, &flen);
            g_rx_read = (g_rx_read + 1) & (RXQ_N - 1);
            if (found) { rx_restock(); if (out_len) *out_len = flen; return found; }
        }
        rx_restock();
        /* FW error? */
        if (!g_gen2 && (r32(CSR_INT) & (CSR_INT_BIT_SW_ERR|CSR_INT_BIT_HW_ERR))) return 0;
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
__attribute__((unused))
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

static int wait_alive(int timeout_ms)
{
    int len = 0;
    const u8 *p;
    /* NOTE: acting on the ALIVE cause here - acking it, releasing the automask
     * and opening the RX ring - wedged the machine twice, both times losing the
     * log frames in flight, so there is no evidence yet for WHICH of those
     * steps is unsafe.  The claim this round is the diagnosis, not the cure:
     * the autopsy below now prints the MSI-X causes, and MSIX_HW_CAUSES bit 0
     * reads 1 on every "failed" load.  Bringing the fw up from there is the
     * next slice, on a rig that can be recovered without hands. */
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

/* small init commands */
static void mvm_tx_ant(u32 valid){ u32 c=valid; send_cmd(GRP_LONG,0x98,0,&c,4); wait_cmd_done(50);}
static void mvm_power_table(void){ u32 c=0; send_cmd(GRP_LONG,0x77,0,&c,4); wait_cmd_done(50);}
static void mvm_dqa_enable(void){ u32 c=0; send_cmd(GRP_DATAPATH,0x0,0,&c,4); wait_cmd_done(50);}

/* PHY_CONFIGURATION_CMD (v1: 12 bytes) */
static void mvm_phy_cfg(void)
{
    struct { u32 phy_cfg, flow, event; } c;
    c.phy_cfg = g_fw.phy_sku & 0x00FF00FF;   /* radio type/step/dash + chains subset */
    c.flow = g_fw.calib_flow; c.event = g_fw.calib_event;
    send_cmd(GRP_LONG, 0x6a, 0, &c, sizeof c); wait_cmd_done(200);
}

/* INIT_EXTENDED_CFG (SYSTEM 0x3) + NVM_ACCESS_COMPLETE (REGNVM 0x0) — unified */
static void mvm_init_unified(void)
{
    u32 init_flags = (1u<<1);                  /* BIT(IWL_INIT_NVM) */
    send_cmd(GRP_SYSTEM, 0x3, 0, &init_flags, 4); wait_cmd_done(100);
    { u32 z = 0; send_cmd(GRP_REGNVM, 0x0, 0, &z, 4); wait_cmd_done(100); }
    mvm_phy_cfg();
    wait_notif(GRP_LEGACY, 0x4 /*INIT_COMPLETE_NOTIF*/, 0, 500);
    /* NVM_GET_INFO for the MAC address */
    { u32 z = 0; int len=0; const u8 *r; send_cmd(GRP_REGNVM, 0x2, 0, &z, 4);
      r = wait_notif(GRP_REGNVM, 0x2, &len, 200);
      if (r && len >= 16) { /* general(8) + sku(4) + phy(8); mac addr sits later -
                               varies by ver; leave g_mac from NVM read below */ }
    }
}

/* PHY_CONTEXT_CMD ADD on a 2.4GHz channel (v3+, 32 bytes) */
static u32 g_phy_id = 0x0000;   /* id 0, color 0 */
static u32 g_mac_id = 0x0100;   /* id 1, color 1 (nonzero color) */
static void mvm_phy_ctxt(int chan, int action)
{
    struct { u32 id_color, action; u32 channel; u8 band,width,ctrl,rsv;
             u32 lmac_id, rxchain, dsp; u8 sec,r3[3]; } c;
    memset(&c, 0, sizeof c);
    c.id_color = g_phy_id; c.action = action;
    c.channel = chan; c.band = 1 /*PHY_BAND_24*/; c.width = 0 /*20MHz*/;
    c.lmac_id = 0; c.rxchain = (1u<<1)|(1u<<10);   /* valid ant A, 1 chain */
    send_cmd(GRP_LONG, 0x8, 0, &c, sizeof c); wait_cmd_done(100);
}

/* MAC_CONTEXT_CMD (BSS STA). Big struct; we fill the common + sta tail. */
static void mvm_mac_ctxt(const u8 bssid[6], int assoc, int aid, int action)
{
    u8 c[140]; u8 *p = c;
    memset(c, 0, sizeof c);
    *(u32*)(p+0) = g_mac_id;            /* id_and_color */
    *(u32*)(p+4) = action;
    *(u32*)(p+8) = 5;                   /* FW_MAC_TYPE_BSS_STA */
    *(u32*)(p+12) = 0;                  /* tsf_id A */
    memcpy(p+16, g_mac, 6);             /* node_addr */
    memcpy(p+24, bssid, 6);             /* bssid_addr */
    *(u32*)(p+40) = 0;                  /* protection_flags */
    { u32 filt = (1u<<2);              /* ACCEPT_GRP */
      if (!assoc) filt |= (1u<<6);      /* IN_BEACON while connecting */
      *(u32*)(p+52) = filt; }
    /* ac[5] EDCA defaults @60 (cw_min 0x0f, cw_max 0x3f, aifsn 1) */
    { int a; for (a=0;a<5;a++){ u8*q=p+60+a*8; q[0]=0x0f; q[2]=0x3f; q[4]=1; } }
    /* mac_data_sta union @100: is_assoc, ... assoc_id @136 */
    *(u32*)(p+100) = assoc ? 1 : 0;
    *(u32*)(p+136) = aid;
    send_cmd(GRP_LONG, 0x28, 0, c, 100 + 44); wait_cmd_done(100);
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
    struct { u32 id_color, quota, max_dur, low_lat; } q[4];
    memset(q, 0, sizeof q);
    q[0].id_color = g_binding_id; q[0].quota = 128; q[0].max_dur = 0;
    send_cmd(GRP_LONG, 0x2c, 0, q, sizeof q); wait_cmd_done(100);
}

/* SESSION_PROTECTION_CMD (MAC_CONF 0x5) or TIME_EVENT_CMD — assoc window */
static void mvm_assoc_window(void)
{
    if (fw_has_capa(54)) {
        struct { u32 id_color, action, conf_id, dur, rep, interval; } c;
        memset(&c,0,sizeof c);
        c.id_color = g_mac_id; c.action = 1 /*ADD*/; c.conf_id = 0 /*ASSOC*/; c.dur = 900;
        send_cmd(GRP_MACCONF, 0x5, 0, &c, sizeof c);
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
    *(u32*)(c+40) = (1u<<g_data_qid>0? 0:0);
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
static int read_config(int vol)
{
    static u8 buf[512];
    long n = uno_fs_read(vol, g_cfgname[0] ? g_cfgname : "WIFI.CFG", buf, sizeof buf - 1);
    int i = 0;
    g_cfg_ssid[0] = g_cfg_psk[0] = 0;
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
            if (!strcmp(key,"ssid")) strcpy(g_cfg_ssid, val);
            else if (!strcmp(key,"psk")) strcpy(g_cfg_psk, val);
        }
        while (i<n && buf[i]!='\n') i++;
    }
    return g_cfg_ssid[0] ? 0 : -1;
}

/* =====================================================================
 * 11. 802.11 <-> Ethernet translation, TX/RX data
 * ===================================================================== */
static u8 g_bssid[6];
static u16 g_seq_no;

/* build a QoS-data 802.11 header (ToDS) + LLC/SNAP for an Ethernet frame,
   returns total 802.11 payload length written to out (after the tx cmd). */
static int eth_to_80211(const u8 *eth, int ethlen, u8 *out)
{
    u8 *p = out;
    u16 ethertype = (u16)((eth[12]<<8) | eth[13]);
    /* frame control: type data(2) subtype qosdata(8) => 0x88, ToDS => 0x01 */
    *p++ = 0x88; *p++ = 0x01;
    *p++ = 0; *p++ = 0;                 /* duration */
    memcpy(p, g_bssid, 6); p += 6;      /* addr1 = BSSID */
    memcpy(p, g_mac, 6);   p += 6;      /* addr2 = SA (us) */
    memcpy(p, eth, 6);     p += 6;      /* addr3 = DA */
    *p++ = (u8)(g_seq_no<<4); *p++ = (u8)(g_seq_no>>4); g_seq_no++;  /* seq ctl */
    *p++ = 0; *p++ = 0;                 /* QoS control (TID 0) */
    memcpy(p, SNAP, 6); p += 6;         /* LLC/SNAP */
    *p++ = (u8)(ethertype>>8); *p++ = (u8)ethertype;
    memcpy(p, eth + 14, ethlen - 14); p += ethlen - 14;
    return (int)(p - out);
}

/* 802.11 data frame (FromDS) -> Ethernet, into out (>= len). Returns eth len. */
static int wifi_to_eth(const u8 *f, int len, u8 *out)
{
    int hdr = 24;
    u16 fc = (u16)(f[0] | (f[1]<<8));
    int qos = ((fc>>4)&0xF) == 8;      /* QoS data subtype */
    const u8 *da, *sa;
    int snap;
    if (qos) hdr += 2;
    if (len < hdr + 8) return -1;
    /* addr layout for FromDS: addr1=DA, addr2=BSSID, addr3=SA */
    da = f + 4; sa = f + 16;
    snap = hdr;
    if (memcmp(f + snap, SNAP, 6) != 0) return -1;
    { u16 et = (u16)((f[snap+6]<<8)|f[snap+7]);
      memcpy(out, da, 6); memcpy(out+6, sa, 6);
      out[12]=(u8)(et>>8); out[13]=(u8)et;
      memcpy(out+14, f + snap + 8, len - snap - 8);
      return 14 + (len - snap - 8); }
}

/* queue a decrypted 802.11 data frame for recv() (called from rx processing) */
static void handle_data_frame(const u8 *frame, int len)
{
    u8 eth[1600];
    int n = wifi_to_eth(frame, len, eth);
    int nx;
    if (n <= 0 || n > 1600) return;
    nx = (g_dq_head + 1) % DATAQ;
    if (nx == g_dq_tail) return;        /* full, drop */
    memcpy(g_dataq[g_dq_head].buf, eth, n);
    g_dataq[g_dq_head].len = n;
    g_dq_head = nx;
}

static wpa_sm_t g_wpa;
static int g_wpa_active, g_keys_installed;
static void handle_eapol(const u8 *frame, int len)
{
    u8 reply[600];
    int r = wpa_sm_rx_eapol(&g_wpa, frame, len, reply, sizeof reply);
    uno_dbg_net_trace("wifi: EAPOL frame in (%d bytes) -> sm state %d, reply %d",
                      len, g_wpa.state, r);
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
        mvm_add_sta_key(g_wpa.ptk + 32, 16, 0, 0, 0);          /* pairwise TK */
        if (g_wpa.gtk_len) mvm_add_sta_key(g_wpa.gtk, g_wpa.gtk_len, g_wpa.gtk_idx, 1, 0);
        g_keys_installed = 1;
        mvm_add_sta(g_bssid, 1, (1u<<14)|(1u<<15));            /* authorize */
        g_joined = 1;
        uno_dbg_net_trace("wifi: 4-way handshake DONE - CCMP keys installed "
                          "(gtk_len=%d idx=%d), station authorized",
                          g_wpa.gtk_len, g_wpa.gtk_idx);
    }
}

/* =====================================================================
 * 12. connect: scan for the SSID, then run the assoc + 4-way handshake
 * ===================================================================== */
static int find_and_join(void)
{
    int chan = 0;
    /* A real scan issues SCAN_REQ_UMAC and collects beacons; here we drive the
       config + request and let the connect flow proceed on the configured SSID.
       Channel discovery from beacons is part of the metal bring-up. */
    mvm_scan_cfg();
    /* State the scaffold boundary in the trace so a metal log is never read as
       "the AP rejected us": everything below runs against a broadcast BSSID
       and no auth/assoc exchange is performed yet. Reaching this line on metal
       means card + firmware + command layer all work - the remaining tail is
       MLME (beacon parse -> real BSSID/channel -> auth/assoc), not transport. */
    uno_dbg_net_trace("wifi: join: scan cfg sent. KNOWN GAP: beacon parse + "
                      "auth/assoc not implemented - bssid=broadcast, chan=1 "
                      "assumed; a real join CANNOT complete yet");
    /* (SCAN_REQ_UMAC build is family-version-specific; see fwapi ref §5.12.
       On metal we parse the SCAN_COMPLETE + beacon RX to learn BSSID/channel.) */
    if (chan == 0) chan = 1;           /* default until beacon parse fills it */

    /* assoc sequence (fwapi ref Part 11 / §8.1) */
    mvm_phy_ctxt(chan, 1 /*ADD*/);
    memset(g_bssid, 0xFF, 6);
    mvm_mac_ctxt(g_bssid, 0, 0, 1 /*ADD*/);
    mvm_binding(1);
    mvm_time_quota();
    mvm_assoc_window();
    mvm_add_sta(g_bssid, 0, 0);        /* ADD the AP peer station */
    /* auth + assoc frame exchange, then MAC ctxt MODIFY(assoc) — metal path.
       The 4-way handshake then runs via handle_eapol() as EAPOL frames arrive. */

    /* set up the WPA supplicant with the PMK */
    { u8 pmk[32];
      wpa_pmk_from_psk(g_cfg_ssid, (int)strlen(g_cfg_ssid), g_cfg_psk, pmk);
      wpa_sm_init(&g_wpa, pmk, g_mac, g_bssid);
      g_wpa_active = 1; }
    strncpy(g_ssid_str, g_cfg_ssid, sizeof g_ssid_str - 1);
    uno_dbg_net_trace("wifi: join: phy/mac ctxt + binding + sta queued for "
                      "\"%s\", WPA2 supplicant armed (PMK derived)", g_cfg_ssid);
    return 0;
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
    return len;
}

static int iwl_recv(void *ctx, void *pkt, int cap)
{
    (void)ctx;
    if (!g_bound) return 0;
    /* pump the RX ring so notifications (EAPOL, data, mgmt) get processed;
       rx_process_rb dispatches data frames to handle_data_frame and EAPOL to
       handle_eapol (which drives the 4-way handshake + key install). */
    { u16 closed = rx_closed() & (RXQ_N - 1);
      while (g_rx_read != closed) {
          const u8 *found = 0; int fl = 0;
          int vid = g_mq_rx ? (int)(g_rbd_used[g_rx_read] & 0xFFF) : (g_rx_read + 1);
          const u8 *rb = (vid >= 1 && vid <= RXQ_N) ? g_rb[vid-1] : g_rb[g_rx_read];
          rx_process_rb(rb, RB_SIZE, -1, -1, &found, &fl);
          g_rx_read = (g_rx_read + 1) & (RXQ_N - 1);
      }
      rx_restock(); }
    if (g_dq_tail != g_dq_head) {
        int n = g_dataq[g_dq_tail].len;
        if (n > cap) n = cap;
        memcpy(pkt, g_dataq[g_dq_tail].buf, n);
        g_dq_tail = (g_dq_tail + 1) % DATAQ;
        return n;
    }
    return 0;
}

static int iwl_link(void *ctx) { (void)ctx; return g_bound && g_joined; }

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

uno_nic_t *iwl_nic(void)
{
    int vol;
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

    choose_firmware();
    st_set("Intel WiFi "); st_cathex(g_hw_rev);
    uno_dbg_net_trace("wifi: card pci=%04x fam=%d gen2=%d fw=%s",
                      g_devid, g_family, g_gen2, g_fwfile);

    vol = firmware_volume();
    if (vol < 0) {
        st_set("WiFi: no WIFI.CFG on the ESP");
        uno_dbg_net_trace("wifi: FAIL no WIFI.CFG/WIFI.TXT on any volume - no credentials");
        return 0;
    }
    if (read_config(vol) < 0) {
        st_set("WiFi: WIFI.CFG has no ssid=");
        uno_dbg_net_trace("wifi: FAIL %s (vol %d) has no ssid= line", g_cfgname, vol);
        return 0;
    }
    uno_dbg_net_trace("wifi: creds from %s: ssid=\"%s\" psk_len=%d",
                      g_cfgname, g_cfg_ssid, (int)strlen(g_cfg_psk));

    /* read the .ucode image (strip the FIRMWARE\ prefix for the FAT reader if
       it exposes only a flat root; try both) */
    fn = uno_fs_read(vol, g_fwfile, g_fwbuf, FW_FILE_MAX);
    if (fn <= 0) fn = uno_fs_read(vol, g_fwfile + 9, g_fwbuf, FW_FILE_MAX);
    if (fn <= 0) {
        st_set("WiFi: firmware not found ("); st_cat(g_fwfile); st_cat(")");
        uno_dbg_net_trace("wifi: FAIL firmware %s not on the ESP (uno-wifi-fw.py stages it)", g_fwfile);
        return 0;
    }
    if (parse_ucode(g_fwbuf, (u32)fn) < 0) {
        st_set("WiFi: bad .ucode TLV");
        uno_dbg_net_trace("wifi: FAIL %s (%ld bytes) failed TLV parse", g_fwfile, fn);
        return 0;
    }
    uno_dbg_net_trace("wifi: firmware %s loaded from disk (%ld bytes)", g_fwfile, fn);
    load_pnvm(vol);

    /* map BAR0 + bus master */
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

    if (prepare_card_hw() < 0) {
        st_set("WiFi: card not ready (ME owns it?)");
        uno_dbg_net_trace("wifi: FAIL prepare_card_hw timeout - CSR handshake refused (ME/CNVi ownership?)");
        return 0;
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
        return 0;
    }
    w32(CSR_UCODE_DRV_GP1_CLR, 0x00000002);   /* clear SW rfkill handshake */
    w32(CSR_UCODE_DRV_GP1_CLR, 0x00000004);   /* clear cmd-blocked */

    if (apm_init() < 0) {
        st_set("WiFi: APM init timeout");
        uno_dbg_net_trace("wifi: FAIL APM init timeout (clock-ready never came up)");
        return 0;
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
    if (g_family >= FAM_AX210)      { if (load_fw_gen3() < 0) { st_set("WiFi: gen3 fw load failed"); uno_dbg_net_trace("wifi: FAIL gen3 (ctxt-info) fw load"); return 0; } }
    else if (g_gen2)                { if (load_fw_gen2() < 0) { st_set("WiFi: gen2 fw load failed"); uno_dbg_net_trace("wifi: FAIL gen2 (ctxt-info) fw load"); return 0; } }
    else                            { if (load_fw_gen1(g_fw.rt, g_fw.rt_n) < 0) { st_set("WiFi: gen1 fw load failed"); uno_dbg_net_trace("wifi: FAIL gen1 (section DMA) fw load"); return 0; } }

    if (wait_alive(2000) < 0) {
        st_set("WiFi: firmware did not ALIVE");
        uno_dbg_net_trace("wifi: FAIL no ALIVE notification within 2 s of fw start");
        return 0;
    }
    uno_dbg_net_trace("wifi: firmware ALIVE");
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
    if (!g_gen2) tx_start_gen1();

    /* post-alive init (unified path; AC split path adds INIT image + calib) */
    mvm_init_unified();
    mvm_tx_ant(1);
    if (fw_has_capa(12)) mvm_dqa_enable();
    mvm_power_table();
    uno_dbg_net_trace("wifi: MVM init sequence queued (nvm/phy/tx-ant/power)");

    /* connect */
    if (find_and_join() < 0) { st_set("WiFi: join failed"); uno_dbg_net_trace("wifi: FAIL join"); return 0; }

    g_nic.ctx = 0; g_nic.send = iwl_send; g_nic.recv = iwl_recv; g_nic.link = iwl_link;
    g_bound = 1;
    st_set("WiFi bound: "); st_cat(g_ssid_str[0]?g_ssid_str:g_cfg_ssid);
    st_cat(g_joined ? " (joined)" : " (associating)");
    return &g_nic;
}

const unsigned char *iwl_mac(void) { return g_mac; }

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

int iwl_dbg_cmd(const char *line, char *out, int cap)
{
    static const char hx[] = "0123456789abcdef";
    u32 a, v;
    int i;
    if (!line || !out || cap < 12) return -1;
    if (!strncmp(line, "dmar off", 8)) { iommu_disable(out, cap); return (int)strlen(out); }
    if (!strncmp(line, "dmar", 4)) return dmar_check(out, cap);
    if (!strncmp(line, "rerun", 5)) {
        g_bound = 0; g_joined = 0; g_alive = 0;       /* force a full retry */
        iwl_nic();
        iwl_status_str(out, cap);
        return (int)strlen(out);
    }
    if (!strncmp(line, "status", 6)) { iwl_status_str(out, cap); return (int)strlen(out); }
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
    return -1;
hexout:
    for (i = 0; i < 8; i++) out[i] = hx[(v >> ((7 - i) * 4)) & 0xF];
    out[8] = 0;
    return 8;
}

void iwl_status_str(char *buf, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    while (g_status[i] && i < cap-1) { buf[i] = g_status[i]; i++; }
    buf[i] = 0;
}
