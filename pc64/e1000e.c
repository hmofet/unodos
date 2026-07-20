/* ===========================================================================
 * UnoDOS/pc64 - Intel e1000e Gigabit Ethernet driver (see e1000e.h).
 *
 * The Intel "e1000e" family: the discrete 82571/2/3/74/83 (PCIe add-in cards;
 * the 82574L is what QEMU emulates as `-device e1000e`) and the PCH-integrated
 * LOM found on most laptops/desktops - I217, I218, and the very common I219.
 * No firmware. The datapath registers and legacy descriptor rings are byte-
 * identical to the classic e1000 (e1000.c) - and that datapath is proven in QEMU
 * on the 82574 - so this driver reuses it and adds (a) the full device table and
 * (b) the I219 PCH init gotchas (PHY SW-flag semaphore + ULP disable).
 *
 * DMA is identity-mapped (virt==phys under UEFI boot services); polled, no IRQs.
 * The 82574 / 82571-3 path is exercised by `-device e1000e`; the I219 PCH init is
 * metal-pending (QEMU has no I219 model). Publishes uno_nic_t; inert when absent.
 * ======================================================================== */
#include "e1000e.h"
#include "pc64_pci.h"
#include <stdint.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

void uno_pc64_delay_ms(int ms);

/* ---- registers (identical offsets to legacy e1000) ---------------------- */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_CTRL_EXT 0x0018
#define REG_MDIC     0x0020
#define REG_FEXTNVM6 0x0010
#define REG_ICR      0x00C0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_KABGTXD  0x3004
#define REG_EXTCNF_CTRL 0x0F00
#define REG_CRC_OFFSET 0x5F50
#define REG_FWSM     0x5B54
#define REG_H2ME     0x5B50
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200
#define REG_RAL0     0x5400
#define REG_RAH0     0x5404
#define REG_SHRAL0   0x5438
#define REG_SHRAH0   0x543C

#define CTRL_SLU   0x00000040
#define CTRL_ASDE  0x00000020
#define CTRL_RST   0x04000000
#define CTRL_PHY_RST 0x80000000u
#define STATUS_LU  0x00000002
#define RAH_AV     0x80000000u
#define RCTL_EN    0x00000002
#define RCTL_BAM   0x00008000
#define RCTL_SECRC 0x04000000
#define RCTL_SZ_2048 0x00000000
#define TCTL_EN    0x00000002
#define TCTL_PSP   0x00000008
#define CTRL_EXT_DRV_LOAD 0x10000000u
#define EXTCNF_SWFLAG 0x00000020
#define ICH_FWSM_FW_VALID 0x00008000u
#define FWSM_ULP_CFG_DONE 0x00000400u
#define H2ME_ULP   0x00000800
#define H2ME_ENFORCE 0x00001000
#define MDIC_OP_READ  0x08000000u
#define MDIC_OP_WRITE 0x04000000u
#define MDIC_READY    0x10000000u
#define MDIC_ERROR    0x40000000u

#define NDESC 16
#define BUFSZ 2048
struct rxdesc { u64 addr; u16 len; u16 csum; u8 status; u8 err; u16 special; } __attribute__((packed));
struct txdesc { u64 addr; u16 len; u8 cso; u8 cmd; u8 status; u8 css; u16 special; } __attribute__((packed));

static volatile struct rxdesc rx_ring[NDESC] __attribute__((aligned(16)));
static volatile struct txdesc tx_ring[NDESC] __attribute__((aligned(16)));
static u8 rx_buf[NDESC][BUFSZ] __attribute__((aligned(16)));
static u8 tx_buf[NDESC][BUFSZ] __attribute__((aligned(16)));

static volatile u8 *g_mmio;
static pci_dev g_pci;
static int rx_cur, tx_cur;
static u8  g_mac[6];
static int g_present, g_up, g_is_pch, g_has_me;
static u16 g_devid;
static uno_nic_t g_nic;

static u32 e_rd(u32 r){ return *(volatile u32 *)(g_mmio+r); }
static void e_wr(u32 r,u32 v){ *(volatile u32 *)(g_mmio+r)=v; }
static void mdelay_(int ms){ uno_pc64_delay_ms(ms); }
static void spin(volatile int n){ while(n-->0) __asm__ volatile(""); }

/* ---- device table ------------------------------------------------------- */
/* returns 1 if this is a supported e1000e part; sets g_is_pch for I217/218/219. */
static int match_e1000e(u16 dev)
{
    switch (dev) {
    /* 82571/2/3 (discrete PCIe) */
    case 0x105e: case 0x105f: case 0x10a4: case 0x10bc: case 0x10a5:
    case 0x1060: case 0x10d9: case 0x10da: case 0x10d5:
    case 0x10b9: case 0x107d: case 0x107e: case 0x107f:
    case 0x108b: case 0x108c: case 0x109a:
    /* 82574 / 82583 (discrete; 0x10d3 = QEMU -device e1000e) */
    case 0x10d3: case 0x10f6: case 0x150c:
        g_is_pch = 0; return 1;
    /* 82577/82578/82579 (pchlan/pch2lan LOM) */
    case 0x10ea: case 0x10eb: case 0x10ef: case 0x10f0:
    case 0x1502: case 0x1503:
        g_is_pch = 1; return 1;
    /* I217 / I218 (pch_lpt LOM) */
    case 0x153a: case 0x153b: case 0x155a: case 0x1559:
    case 0x15a0: case 0x15a1: case 0x15a2: case 0x15a3:
        g_is_pch = 1; return 1;
    /* I219 (pch_spt/cnp/tgp/adp/mtp/ptp LOM) - all of them */
    case 0x156f: case 0x1570: case 0x15b7: case 0x15b8: case 0x15b9:
    case 0x15d7: case 0x15d8: case 0x15e3: case 0x15d6: case 0x0d53: case 0x0d55:
    case 0x15bd: case 0x15be: case 0x15bb: case 0x15bc: case 0x15df: case 0x15e0:
    case 0x15e1: case 0x15e2: case 0x0d4e: case 0x0d4f: case 0x0d4c: case 0x0d4d:
    case 0x15fb: case 0x15fc: case 0x15f9: case 0x15fa: case 0x15f4: case 0x15f5:
    case 0x0dc5: case 0x0dc6: case 0x1a1e: case 0x1a1f: case 0x1a1c: case 0x1a1d:
    case 0x0dc7: case 0x0dc8: case 0x550c: case 0x550d:
    case 0x550a: case 0x550b: case 0x550e: case 0x550f: case 0x5510: case 0x5511:
    case 0x57a0: case 0x57a1: case 0x57b3: case 0x57b4: case 0x57b7: case 0x57b8:
    case 0x57b9: case 0x57ba:
        g_is_pch = 1; return 1;
    }
    return 0;
}

/* ---- I219 PHY semaphore (only on ich8lan/pch parts) --------------------- */
static int acquire_swflag(void)
{
    int t; u32 v;
    for (t=0;t<100;t++){ if(!(e_rd(REG_EXTCNF_CTRL)&EXTCNF_SWFLAG)) break; mdelay_(1); }
    v = e_rd(REG_EXTCNF_CTRL) | EXTCNF_SWFLAG; e_wr(REG_EXTCNF_CTRL, v);
    for (t=0;t<1000;t++){ if(e_rd(REG_EXTCNF_CTRL)&EXTCNF_SWFLAG) return 0; mdelay_(1); }
    return -1;
}
static void release_swflag(void){ e_wr(REG_EXTCNF_CTRL, e_rd(REG_EXTCNF_CTRL) & ~EXTCNF_SWFLAG); }

/* ULP disable (I218/I219): ask the ME to un-configure the ultra-low-power link.
 * Only meaningful when an ME is present; on a cold boot ULP is usually already
 * off, so this is best-effort + bounded. */
static void disable_ulp(void)
{
    int t; u32 v;
    if (!(e_rd(REG_FWSM) & ICH_FWSM_FW_VALID)) return;   /* no ME -> SW ULP path (skipped) */
    v = e_rd(REG_H2ME); v &= ~H2ME_ULP; v |= H2ME_ENFORCE; e_wr(REG_H2ME, v);
    for (t=0;t<250;t++){ if(!(e_rd(REG_FWSM) & FWSM_ULP_CFG_DONE)) break; mdelay_(10); }
    e_wr(REG_H2ME, e_rd(REG_H2ME) & ~H2ME_ENFORCE);
}

/* ---- MAC address -------------------------------------------------------- */
static void read_mac(void)
{
    u32 ral = e_rd(REG_RAL0), rah = e_rd(REG_RAH0);
    if (!(rah & RAH_AV)) { ral = e_rd(REG_SHRAL0); rah = e_rd(REG_SHRAH0); }  /* I219 shared-RAM MAC */
    g_mac[0]=(u8)ral; g_mac[1]=(u8)(ral>>8); g_mac[2]=(u8)(ral>>16); g_mac[3]=(u8)(ral>>24);
    g_mac[4]=(u8)rah; g_mac[5]=(u8)(rah>>8);
}

/* ---- rings -------------------------------------------------------------- */
static void rx_init(void)
{
    int i;
    for (i=0;i<NDESC;i++){ rx_ring[i].addr=(u64)(uintptr_t)rx_buf[i]; rx_ring[i].status=0; }
    e_wr(REG_RDBAL, (u32)(uintptr_t)rx_ring);
    e_wr(REG_RDBAH, (u32)((u64)(uintptr_t)rx_ring>>32));
    e_wr(REG_RDLEN, NDESC*16);
    e_wr(REG_RDH, 0); e_wr(REG_RDT, NDESC-1);
    e_wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_SZ_2048 | 0x18 /*UPE|MPE*/);
    rx_cur = 0;
}
static void tx_init(void)
{
    int i;
    for (i=0;i<NDESC;i++){ tx_ring[i].addr=(u64)(uintptr_t)tx_buf[i]; tx_ring[i].status=0x01; tx_ring[i].cmd=0; }
    e_wr(REG_TDBAL, (u32)(uintptr_t)tx_ring);
    e_wr(REG_TDBAH, (u32)((u64)(uintptr_t)tx_ring>>32));
    e_wr(REG_TDLEN, NDESC*16);
    e_wr(REG_TDH, 0); e_wr(REG_TDT, 0);
    e_wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F<<4) | (0x40<<12));
    tx_cur = 0;
}

/* ---- uno_nic_t ---------------------------------------------------------- */
static int e1000e_send(void *ctx,const void *pkt,int len)
{
    volatile struct txdesc *d; const u8 *p=(const u8*)pkt; int i;
    (void)ctx;
    if (!g_up || len<=0 || len>BUFSZ) return -1;
    d = &tx_ring[tx_cur];
    { int s=1000000; while(!(d->status&0x01) && s--) spin(10); }
    for (i=0;i<len;i++) tx_buf[tx_cur][i]=p[i];
    d->addr=(u64)(uintptr_t)tx_buf[tx_cur]; d->len=(u16)len; d->cso=0;
    d->cmd=0x01|0x02|0x08;                 /* EOP | IFCS | RS */
    d->status=0;
    tx_cur=(tx_cur+1)%NDESC;
    e_wr(REG_TDT, tx_cur);
    return len;
}
static int e1000e_recv(void *ctx,void *pkt,int cap)
{
    volatile struct rxdesc *d=&rx_ring[rx_cur]; int len,i; u8 *out=(u8*)pkt;
    (void)ctx;
    if (!g_up || !(d->status&0x01)) return 0;
    len=d->len; if(len>cap) len=cap;
    for (i=0;i<len;i++) out[i]=rx_buf[rx_cur][i];
    d->status=0;
    e_wr(REG_RDT, rx_cur);
    rx_cur=(rx_cur+1)%NDESC;
    return len;
}
static int e1000e_link(void *ctx){ (void)ctx; return g_up && (e_rd(REG_STATUS)&STATUS_LU); }

int e1000e_present(void)
{
    pci_dev d;
    if (g_present) return 1;
    if (pci_find_class(0x02, 0x00, &d)) {   /* network / ethernet */
        u16 dev = pci_cfg_read16(&d, 2);
        if (d.vendor==0x8086 && match_e1000e(dev)) { g_pci=d; g_devid=dev; g_present=1; return 1; }
    }
    return 0;
}

uno_nic_t *e1000e_nic(void)
{
    if (g_up) return &g_nic;
    if (!e1000e_present()) return 0;
    pci_enable_bus_master(&g_pci);
    g_mmio=(volatile u8*)(uintptr_t)pci_bar(&g_pci, 0);
    if (!g_mmio) return 0;
    g_has_me = (e_rd(REG_FWSM) & ICH_FWSM_FW_VALID) ? 1 : 0;

    e_wr(REG_IMC, 0xFFFFFFFFu);                     /* mask all interrupts */
    e_wr(REG_RCTL, 0); e_wr(REG_TCTL, TCTL_PSP);
    (void)e_rd(REG_STATUS); mdelay_(10);

    if (g_is_pch) {                                 /* I217/I218/I219 PCH init */
        acquire_swflag();
        e_wr(REG_CTRL, e_rd(REG_CTRL) | CTRL_RST | CTRL_PHY_RST);
        mdelay_(20);                                /* do NOT flush right after RST on ich8lan */
        release_swflag();
        disable_ulp();
        e_wr(REG_KABGTXD, e_rd(REG_KABGTXD) | 0x00000800);  /* band-gap fix */
        e_wr(REG_CRC_OFFSET, 0x65656565);
    } else {                                        /* 82571-4 / 82583 (incl. QEMU e1000e) */
        e_wr(REG_CTRL, e_rd(REG_CTRL) | CTRL_RST);
        { int s=1000000; while((e_rd(REG_CTRL)&CTRL_RST) && s--) spin(10); }
    }
    e_wr(REG_IMC, 0xFFFFFFFFu); (void)e_rd(REG_ICR);

    read_mac();
    /* restore RAR0 (cleared by reset) + AV */
    e_wr(REG_RAL0, g_mac[0]|(g_mac[1]<<8)|(g_mac[2]<<16)|((u32)g_mac[3]<<24));
    e_wr(REG_RAH0, g_mac[4]|(g_mac[5]<<8)|RAH_AV);
    { int i; for (i=0;i<128;i++) e_wr(REG_MTA + i*4, 0); }

    e_wr(REG_CTRL_EXT, e_rd(REG_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    rx_init();
    tx_init();
    e_wr(REG_CTRL, e_rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE);   /* set link up */
    (void)e_rd(REG_ICR);

    g_nic.ctx=0; g_nic.send=e1000e_send; g_nic.recv=e1000e_recv; g_nic.link=e1000e_link;
    g_up=1;
    return &g_nic;
}

const unsigned char *e1000e_mac(void){ return g_mac; }
