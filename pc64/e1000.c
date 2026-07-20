/* ===========================================================================
 * UnoDOS/pc64 - native Intel e1000 (82540EM) NIC driver.
 *
 * The first real hardware backend for the family's `nic` service
 * (unonet/uno_nic.h): a from-scratch driver for the Intel Gigabit MAC QEMU
 * exposes as `-device e1000` (8086:100E) and the one in a great many
 * spare PCs. It publishes send/recv/link over MMIO descriptor rings.
 *
 * DMA is trivial here because UEFI leaves the machine identity-mapped
 * (virtual == physical) while boot services are alive, so a static .bss
 * buffer's address IS its physical address - exactly what the NIC's ring-base
 * and descriptor-address registers want. (ExitBootServices + a real memory
 * map is the documented tail; until then this is correct and simple.)
 *
 * Legacy descriptors, polled (no interrupt): the stack pumps recv() each
 * frame. Enough for ARP/IP/ICMP/UDP/TCP at interactive rates.
 * ======================================================================== */
#include "pc64_pci.h"
#include "uno_nic.h"
#include <stdint.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;      /* mingw is LLP64: `long` is 32-bit! */

/* ---- registers --------------------------------------------------------- */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5408

#define CTRL_SLU   0x00000040
#define CTRL_ASDE  0x00000020
#define CTRL_RST   0x04000000

#define RCTL_EN    0x00000002
#define RCTL_UPE   0x00000008        /* unicast promiscuous */
#define RCTL_MPE   0x00000010        /* multicast promiscuous */
#define RCTL_BAM   0x00008000        /* broadcast accept */
#define RCTL_SECRC 0x04000000        /* strip CRC */
/* BSIZE bits clear = 2048-byte buffers */

#define TCTL_EN    0x00000002
#define TCTL_PSP   0x00000008
#define TCTL_CT    0x00000100        /* collision threshold 0x10 */
#define TCTL_COLD  0x00040000        /* collision distance 0x40 */

#define TXD_CMD_EOP  0x01
#define TXD_CMD_IFCS 0x02
#define TXD_CMD_RS   0x08
#define TXD_STA_DD   0x01
#define RXD_STA_DD   0x01
#define RXD_STA_EOP  0x02

#define NDESC 16                     /* 16*16 = 256B rings (mult of 128) */
#define BUFSZ 2048

struct rxdesc { u64 addr; u16 len; u16 csum; u8 status; u8 err; u16 special; }
    __attribute__((packed));
struct txdesc { u64 addr; u16 len; u8 cso; u8 cmd; u8 status; u8 css; u16 special; }
    __attribute__((packed));

/* rings + buffers, 16-byte aligned in .bss (identity-mapped => phys == virt).
   volatile: the NIC DMA-writes descriptor status back to this memory. */
static volatile struct rxdesc rx_ring[NDESC] __attribute__((aligned(16)));
static volatile struct txdesc tx_ring[NDESC] __attribute__((aligned(16)));
static u8 rx_buf[NDESC][BUFSZ] __attribute__((aligned(16)));
static u8 tx_buf[NDESC][BUFSZ] __attribute__((aligned(16)));

static volatile u8 *g_mmio;
static int rx_cur, tx_cur;
static u8  g_mac[6];
static int g_up;

static u32 e_rd(u32 r) { return *(volatile u32 *)(g_mmio + r); }
static void e_wr(u32 r, u32 v) { *(volatile u32 *)(g_mmio + r) = v; }

#ifdef UNO_DBGCON
static void dbg_out(unsigned char c)
{ __asm__ volatile ("outb %0, %1" : : "a"(c), "Nd"((unsigned short)0x402)); }
static void dbg_s(const char *s) { while (*s) dbg_out((unsigned char)*s++); }
static void dbg_x(u64 v)
{ const char *h="0123456789ABCDEF"; int i; dbg_s("0x");
  for (i=60;i>=0;i-=4) dbg_out((unsigned char)h[(v>>i)&0xF]); dbg_out('\n'); }
#else
#define dbg_s(x) ((void)0)
#define dbg_x(x) ((void)0)
#endif

static void spin(volatile int n) { while (n-- > 0) __asm__ volatile (""); }

static void read_mac(void)
{
    int i, ok = 1;
    /* EEPROM words 0..2 hold the MAC (canonical on the 82540EM QEMU emulates).
       EERD: bit0 START, bits8-15 ADDR, bit4 DONE, bits16-31 DATA. */
    for (i = 0; i < 3; i++) {
        u32 v; int s = 1000000;
        e_wr(REG_EERD, ((u32)i << 8) | 1);
        do { v = e_rd(REG_EERD); } while (!(v & 0x10) && s--);
        if (s <= 0) { ok = 0; break; }
        g_mac[i * 2]     = (v >> 16) & 0xFF;
        g_mac[i * 2 + 1] = (v >> 24) & 0xFF;
    }
    if (ok && (g_mac[0] | g_mac[1] | g_mac[2] | g_mac[3] | g_mac[4] | g_mac[5]))
        return;
    /* fall back to a preloaded RAL0/RAH0 filter */
    {
        u32 ral = e_rd(REG_RAL0), rah = e_rd(REG_RAH0);
        g_mac[0] = ral; g_mac[1] = ral >> 8; g_mac[2] = ral >> 16; g_mac[3] = ral >> 24;
        g_mac[4] = rah; g_mac[5] = rah >> 8;
    }
}

static void rx_init(void)
{
    int i;
    for (i = 0; i < NDESC; i++) {
        rx_ring[i].addr = (u64)(uintptr_t)rx_buf[i];
        rx_ring[i].status = 0;
    }
    e_wr(REG_RDBAL, (u32)(uintptr_t)rx_ring);
    e_wr(REG_RDBAH, (u32)((u64)(uintptr_t)rx_ring >> 32));
    e_wr(REG_RDLEN, NDESC * 16);
    e_wr(REG_RDH, 0);
    e_wr(REG_RDT, NDESC - 1);
    /* promiscuous (UPE|MPE) so unicast replies aren't filtered even if the
       RA filter is imperfect - single-NIC guest, so this is harmless */
    e_wr(REG_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);
    rx_cur = 0;
}

static void tx_init(void)
{
    int i;
    for (i = 0; i < NDESC; i++) {
        tx_ring[i].addr = (u64)(uintptr_t)tx_buf[i];
        tx_ring[i].status = TXD_STA_DD;         /* mark free */
        tx_ring[i].cmd = 0;
    }
    e_wr(REG_TDBAL, (u32)(uintptr_t)tx_ring);
    e_wr(REG_TDBAH, (u32)((u64)(uintptr_t)tx_ring >> 32));
    e_wr(REG_TDLEN, NDESC * 16);
    e_wr(REG_TDH, 0);
    e_wr(REG_TDT, 0);
    e_wr(REG_TIPG, 10 | (8 << 10) | (6 << 20)); /* IEEE 802.3 default IPG */
    e_wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    tx_cur = 0;
}

static int e1000_send(void *ctx, const void *pkt, int len)
{
    volatile struct txdesc *d;
    const u8 *p = (const u8 *)pkt;
    int i;
    (void)ctx;
    if (len <= 0 || len > BUFSZ) return -1;
    d = &tx_ring[tx_cur];
    /* wait for the descriptor we're about to reuse to be done */
    { int spins = 1000000; while (!(d->status & TXD_STA_DD) && spins--) spin(10); }
    for (i = 0; i < len; i++) tx_buf[tx_cur][i] = p[i];
    d->addr = (u64)(uintptr_t)tx_buf[tx_cur];
    d->len = (u16)len;
    d->cso = 0;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;
    tx_cur = (tx_cur + 1) % NDESC;
    e_wr(REG_TDT, tx_cur);
    return len;
}

static int e1000_recv(void *ctx, void *pkt, int cap)
{
    volatile struct rxdesc *d = &rx_ring[rx_cur];
    int len, i;
    u8 *out = (u8 *)pkt;
    (void)ctx;
    if (!(d->status & RXD_STA_DD)) return 0;    /* nothing ready */
    len = d->len;
    if (len > cap) len = cap;
    for (i = 0; i < len; i++) out[i] = rx_buf[rx_cur][i];
    d->status = 0;
    e_wr(REG_RDT, rx_cur);                       /* hand the buffer back */
    rx_cur = (rx_cur + 1) % NDESC;
    return len;
}

static int e1000_link(void *ctx)
{
    (void)ctx;
    return (e_rd(REG_STATUS) & 0x02) ? 1 : 0;    /* STATUS.LU */
}

static uno_nic_t g_nic;

/* bring the card up; returns a nic or NULL if none present */
uno_nic_t *e1000_nic(void)
{
    pci_dev d;
    int i;
    if (!pci_find(0x8086, 0x100E, &d) &&        /* 82540EM (QEMU default) */
        !pci_find(0x8086, 0x100F, &d))          /* 82545EM */
        return 0;                                /* 82574L (0x10D3) -> e1000e.c */

    pci_enable_bus_master(&d);
    g_mmio = (volatile u8 *)(uintptr_t)pci_bar(&d, 0);

    read_mac();                                  /* BEFORE reset (RA is loaded now) */

    e_wr(REG_IMC, 0xFFFFFFFF);                   /* mask all interrupts (polled) */
    e_wr(REG_CTRL, e_rd(REG_CTRL) | CTRL_RST);   /* reset */
    { int s = 1000000; while ((e_rd(REG_CTRL) & CTRL_RST) && s--) spin(10); }
    e_wr(REG_IMC, 0xFFFFFFFF);
    e_wr(REG_CTRL, e_rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    for (i = 0; i < 128; i++) e_wr(REG_MTA + i * 4, 0);  /* clear MC filter */
    /* restore the receive-address filter cleared by reset: RAL0/RAH0 + AV */
    e_wr(REG_RAL0, g_mac[0] | (g_mac[1]<<8) | (g_mac[2]<<16) | ((u32)g_mac[3]<<24));
    e_wr(REG_RAH0, g_mac[4] | (g_mac[5]<<8) | 0x80000000u); /* AV = bit 31 */
    rx_init();
    tx_init();
    (void)e_rd(REG_ICR);                         /* clear pending */
    g_up = 1;

    dbg_s("e1000 up, bar="); dbg_x((u64)(uintptr_t)g_mmio);

    g_nic.ctx = 0;
    g_nic.send = e1000_send;
    g_nic.recv = e1000_recv;
    g_nic.link = e1000_link;
    return &g_nic;
}

const unsigned char *e1000_mac(void) { return g_mac; }
int e1000_present(void) { return g_up; }
