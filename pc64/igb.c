/* ===========================================================================
 * UnoDOS/pc64 - Intel igb Gigabit Ethernet driver (see igb.h).
 *
 * The Intel "igb" family: discrete PCIe 1GbE - I210 / I211 (desktops, embedded)
 * and the 82575/82576/82580 / I350 / I354 (server, multi-port). Unlike e1000/
 * e1000e these use ADVANCED rx/tx descriptors and per-queue enable bits, so the
 * datapath is a from-scratch rewrite (the control/MAC-filter registers still
 * share the e1000 offsets). No firmware. QEMU emulates this family as
 * `-device igb` (82576), so the driver is testable end-to-end.
 *
 * DMA is identity-mapped (virt==phys under UEFI boot services); polled, no IRQs,
 * single RX + single TX queue (queue 0, at the 0x2800/0x3800 register range).
 * Register offsets, advanced-descriptor layout and the init flow follow the
 * Linux igb driver exactly. Publishes uno_nic_t; inert when absent.
 * ======================================================================== */
#include "igb.h"
#include "pc64_pci.h"
#include <stdint.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

void uno_pc64_delay_ms(int ms);

/* ---- registers ---------------------------------------------------------- */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_CTRL_EXT 0x0018
#define REG_MDIC     0x0020
#define REG_EECD     0x0010
#define REG_ICR      0x00C0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_MTA      0x5200
#define REG_RAL0     0x5400
#define REG_RAH0     0x5404
/* queue-0 ring registers (single-queue driver uses the 0x2800/0x3800 range) */
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_SRRCTL   0x280C
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_RXDCTL   0x2828
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_TXDCTL   0x3828

#define CTRL_RST   0x04000000
#define CTRL_SLU   0x00000040
#define CTRL_LRST  0x00000008
#define CTRL_FRCSPD 0x00000800
#define CTRL_FRCDPX 0x00001000
#define CTRL_MASTER_DIS 0x00000004
#define STATUS_LU  0x00000002
#define STATUS_MASTER_EN 0x00080000u
#define RAH_AV     0x80000000u
#define CTRL_EXT_DRV_LOAD 0x10000000u
#define RCTL_EN    0x00000002
#define RCTL_BAM   0x00008000
#define RCTL_SECRC 0x04000000
#define RCTL_UPE   0x00000008
#define RCTL_MPE   0x00000010
#define TCTL_EN    0x00000002
#define TCTL_PSP   0x00000008
#define TCTL_RTLC  0x01000000
#define TCTL_CT    0x00000FF0
#define SRRCTL_ADV_ONEBUF 0x02000000u
#define DCTL_QUEUE_ENABLE 0x02000000u
/* advanced TX cmd_type_len bits */
#define ADVTXD_DTYP_DATA 0x00300000u
#define ADVTXD_DCMD_DEXT 0x20000000u
#define ADVTXD_DCMD_EOP  0x01000000u
#define ADVTXD_DCMD_IFCS 0x02000000u
#define ADVTXD_DCMD_RS   0x08000000u
#define ADVTXD_PAYLEN_SHIFT 14
#define RXD_STAT_DD  0x01
#define TXD_STAT_DD  0x01

#define NDESC 16
#define BUFSZ 2048

/* advanced descriptors (16 bytes; read + write-back share storage) */
struct adv_rx { u64 pkt_addr; u64 hdr_addr; } __attribute__((packed));        /* read form */
struct adv_rx_wb { u32 lo0; u32 lo1; u32 status_error; u16 length; u16 vlan; } __attribute__((packed));
struct adv_tx { u64 addr; u32 cmd_type_len; u32 olinfo_status; } __attribute__((packed));

static volatile struct adv_rx rx_ring[NDESC] __attribute__((aligned(16)));
static volatile struct adv_tx tx_ring[NDESC] __attribute__((aligned(16)));
static u8 rx_buf[NDESC][BUFSZ] __attribute__((aligned(16)));
static u8 tx_buf[NDESC][BUFSZ] __attribute__((aligned(16)));

static volatile u8 *g_mmio;
static pci_dev g_pci;
static int rx_cur, tx_cur;
static u8  g_mac[6];
static int g_present, g_up;
static u16 g_devid;
static uno_nic_t g_nic;

static u32 e_rd(u32 r){ return *(volatile u32 *)(g_mmio+r); }
static void e_wr(u32 r,u32 v){ *(volatile u32 *)(g_mmio+r)=v; }
static void mdelay_(int ms){ uno_pc64_delay_ms(ms); }
static void spin(volatile int n){ while(n-->0) __asm__ volatile(""); }

/* ---- internal PHY via MDIC (phyaddr 1) ---------------------------------- */
#define MDIC_OP_WRITE (1u<<26)
#define MDIC_OP_READ  (2u<<26)
#define MDIC_PHYADD   (1u<<21)
#define MDIC_READY    (1u<<28)
#define MDIC_ERR      (1u<<30)
static u16 phy_read(u8 reg)
{
    int s=100000;
    e_wr(REG_MDIC, MDIC_OP_READ | MDIC_PHYADD | ((u32)reg<<16));
    while(!(e_rd(REG_MDIC)&(MDIC_READY|MDIC_ERR)) && s--) spin(10);
    return (u16)e_rd(REG_MDIC);
}
static void phy_write(u8 reg, u16 val)
{
    int s=100000;
    e_wr(REG_MDIC, MDIC_OP_WRITE | MDIC_PHYADD | ((u32)reg<<16) | val);
    while(!(e_rd(REG_MDIC)&(MDIC_READY|MDIC_ERR)) && s--) spin(10);
}

/* ---- device table (all map to board_82575) ------------------------------ */
static int match_igb(u16 dev)
{
    switch (dev) {
    case 0x1f40: case 0x1f41: case 0x1f45:                       /* I354 */
    case 0x1539:                                                 /* I211 */
    case 0x1533: case 0x1536: case 0x1537: case 0x1538:          /* I210 */
    case 0x157b: case 0x157c:
    case 0x1521: case 0x1522: case 0x1523: case 0x1524:          /* I350 */
    case 0x150e: case 0x150f: case 0x1510: case 0x1511:          /* 82580 */
    case 0x1516: case 0x1527:
    case 0x0438: case 0x043a: case 0x043c: case 0x0440:          /* DH89xxCC */
    case 0x10c9: case 0x10e6: case 0x10e7: case 0x10e8:          /* 82576 (0x10c9 = QEMU igb) */
    case 0x1526: case 0x150a: case 0x1518: case 0x150d:
    case 0x10a7: case 0x10a9: case 0x10d6:                       /* 82575 */
        return 1;
    }
    return 0;
}

/* ---- MAC ---------------------------------------------------------------- */
static void read_mac(void)
{
    u32 ral = e_rd(REG_RAL0), rah = e_rd(REG_RAH0);
    g_mac[0]=(u8)ral; g_mac[1]=(u8)(ral>>8); g_mac[2]=(u8)(ral>>16); g_mac[3]=(u8)(ral>>24);
    g_mac[4]=(u8)rah; g_mac[5]=(u8)(rah>>8);
}

/* ---- rings -------------------------------------------------------------- */
static void rx_init(void)
{
    int i;
    for (i=0;i<NDESC;i++){ rx_ring[i].pkt_addr=(u64)(uintptr_t)rx_buf[i]; rx_ring[i].hdr_addr=0; }
    e_wr(REG_RXDCTL, 0);                             /* disable queue while programming */
    e_wr(REG_RDBAL, (u32)(uintptr_t)rx_ring);
    e_wr(REG_RDBAH, (u32)((u64)(uintptr_t)rx_ring>>32));
    e_wr(REG_RDLEN, NDESC*16);
    e_wr(REG_SRRCTL, (BUFSZ>>10) | SRRCTL_ADV_ONEBUF);   /* 2KB bufs, advanced 1-buffer */
    e_wr(REG_RDH, 0); e_wr(REG_RDT, 0);
    e_wr(REG_RXDCTL, 8 | (1<<8) | (1<<16) | DCTL_QUEUE_ENABLE);   /* enable queue... */
    { int s=1000; while(!(e_rd(REG_RXDCTL)&DCTL_QUEUE_ENABLE) && s--) spin(200); }
    e_wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_UPE | RCTL_MPE);  /* ...then RX globally */
    e_wr(REG_RDT, NDESC-1);                          /* hand descriptors to HW */
    rx_cur = 0;
}
static void tx_init(void)
{
    int i;
    for (i=0;i<NDESC;i++){ tx_ring[i].addr=(u64)(uintptr_t)tx_buf[i]; tx_ring[i].cmd_type_len=0; tx_ring[i].olinfo_status=0; }
    e_wr(REG_TXDCTL, 0);
    e_wr(REG_TDBAL, (u32)(uintptr_t)tx_ring);
    e_wr(REG_TDBAH, (u32)((u64)(uintptr_t)tx_ring>>32));
    e_wr(REG_TDLEN, NDESC*16);
    e_wr(REG_TDH, 0); e_wr(REG_TDT, 0);
    { u32 t = e_rd(REG_TCTL) & ~TCTL_CT; t |= TCTL_EN|TCTL_PSP|TCTL_RTLC|(15<<4); e_wr(REG_TCTL, t); }
    e_wr(REG_TXDCTL, 8 | (1<<8) | (1<<16) | DCTL_QUEUE_ENABLE);
    { int s=1000; while(!(e_rd(REG_TXDCTL)&DCTL_QUEUE_ENABLE) && s--) spin(200); }
    tx_cur = 0;
}

/* ---- uno_nic_t ---------------------------------------------------------- */
static int igb_send(void *ctx,const void *pkt,int len)
{
    volatile struct adv_tx *d; const u8 *p=(const u8*)pkt; int i;
    (void)ctx;
    if (!g_up || len<=0 || len>BUFSZ) return -1;
    d=&tx_ring[tx_cur];
    /* wait for the write-back DD on the slot we're about to reuse (olinfo bit0) */
    { int s=1000000; while((d->olinfo_status & TXD_STAT_DD)==0 && (d->cmd_type_len!=0) && s--) spin(10); }
    for (i=0;i<len;i++) tx_buf[tx_cur][i]=p[i];
    d->addr=(u64)(uintptr_t)tx_buf[tx_cur];
    d->cmd_type_len=(u32)len | ADVTXD_DTYP_DATA | ADVTXD_DCMD_DEXT |
                    ADVTXD_DCMD_EOP | ADVTXD_DCMD_IFCS | ADVTXD_DCMD_RS;
    d->olinfo_status=(u32)len << ADVTXD_PAYLEN_SHIFT;
    tx_cur=(tx_cur+1)%NDESC;
    e_wr(REG_TDT, tx_cur);
    return len;
}
static int igb_recv(void *ctx,void *pkt,int cap)
{
    volatile struct adv_rx_wb *wb=(volatile struct adv_rx_wb *)&rx_ring[rx_cur];
    int len,i; u8 *out=(u8*)pkt;
    (void)ctx;
    if (!g_up) return 0;
    if (!(wb->status_error & RXD_STAT_DD)) return 0;
    __asm__ volatile("" ::: "memory");
    len=wb->length; if(len>cap) len=cap; if(len<0) len=0;
    for (i=0;i<len;i++) out[i]=rx_buf[rx_cur][i];
    /* recycle: rewrite the read-format address DWORDs (the write-back clobbered them) */
    rx_ring[rx_cur].pkt_addr=(u64)(uintptr_t)rx_buf[rx_cur];
    rx_ring[rx_cur].hdr_addr=0;
    e_wr(REG_RDT, rx_cur);
    rx_cur=(rx_cur+1)%NDESC;
    return len;
}
static int igb_link(void *ctx){ (void)ctx; return g_up && (e_rd(REG_STATUS)&STATUS_LU); }

int igb_present(void)
{
    pci_dev d;
    if (g_present) return 1;
    if (pci_find_class(0x02, 0x00, &d)) {
        u16 dev = pci_cfg_read16(&d, 2);
        if (d.vendor==0x8086 && match_igb(dev)) { g_pci=d; g_devid=dev; g_present=1; return 1; }
    }
    return 0;
}

uno_nic_t *igb_nic(void)
{
    if (g_up) return &g_nic;
    if (!igb_present()) return 0;
    pci_enable_bus_master(&g_pci);
    g_mmio=(volatile u8*)(uintptr_t)pci_bar(&g_pci, 0);
    if (!g_mmio) return 0;

    /* reset */
    e_wr(REG_IMC, 0xFFFFFFFFu);
    e_wr(REG_RCTL, 0); e_wr(REG_TCTL, TCTL_PSP);
    (void)e_rd(REG_STATUS); mdelay_(10);
    e_wr(REG_CTRL, e_rd(REG_CTRL) | CTRL_RST);
    { int s=1000000; while((e_rd(REG_CTRL)&CTRL_RST) && s--) spin(10); }
    mdelay_(3);
    e_wr(REG_IMC, 0xFFFFFFFFu); (void)e_rd(REG_ICR);

    read_mac();
    e_wr(REG_RAL0, g_mac[0]|(g_mac[1]<<8)|(g_mac[2]<<16)|((u32)g_mac[3]<<24));
    e_wr(REG_RAH0, g_mac[4]|(g_mac[5]<<8)|RAH_AV);
    { int i; for (i=0;i<128;i++) e_wr(REG_MTA + i*4, 0); }

    e_wr(REG_CTRL_EXT, e_rd(REG_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    tx_init();
    rx_init();
    /* set link up: clear LRST (which holds the link in reset) + force-speed/dpx,
       let auto-speed-detect run. */
    { u32 c = e_rd(REG_CTRL); c &= ~(CTRL_LRST|CTRL_FRCSPD|CTRL_FRCDPX); c |= CTRL_SLU;
      e_wr(REG_CTRL, c); }
    /* Bring the internal PHY link up: power up + restart autoneg. QEMU's igb
       gates RX (igb_can_receive) on STATUS.LU, which it only sets once PHY
       autonegotiation completes -- so this MDIC kick is required for RX. */
    { u16 bmcr = phy_read(0);            /* MII_BMCR */
      bmcr &= ~0x0800;                   /* clear power-down */
      bmcr |=  0x1000;                   /* autoneg enable  */
      bmcr |=  0x0200;                   /* restart autoneg */
      phy_write(0, bmcr);
      { int s=2000; while(!(e_rd(REG_STATUS)&STATUS_LU) && s--) mdelay_(1); } }
    (void)e_rd(REG_ICR);

    g_nic.ctx=0; g_nic.send=igb_send; g_nic.recv=igb_recv; g_nic.link=igb_link;
    g_up=1;
    return &g_nic;
}

const unsigned char *igb_mac(void){ return g_mac; }
