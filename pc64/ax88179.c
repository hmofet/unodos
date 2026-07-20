/* ASIX AX88179 / AX88179A USB Gigabit Ethernet driver (see ax88179.h).
 *
 * Register access is AX_ACCESS_MAC vendor control transfers over the xHCI USB
 * stack (uno_usb_*); frames move over the bulk endpoints. Publishes uno_nic_t.
 *
 * The register init + the RX aggregation trailer / TX header formats follow the
 * Linux ax88179_178a driver. Those can only be fully verified against the real
 * chip, so a raw-RX dump is available for tuning if a packet doesn't parse.
 * Inert unless the USB stack is present (uno_usb_* stubs return -1). */
#include "ax88179.h"
#include "xhci.h"
#include <string.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

void uno_pc64_delay_ms(int ms);

/* ---- AX88179 vendor requests + registers -------------------------------- */
#define AX_ACCESS_MAC          0x01
#define AX_ACCESS_PHY          0x02   /* MDIO: value=phy id, index=phy reg */
#define AX88179_PHY_ID         0x03   /* address of the embedded gigabit PHY */
#define MII_BMSR               0x01   /* PHY basic-mode status register    */
#define MII_BMSR_LSTATUS       0x0004 /* link-status bit (latches low)     */
#define AX_NODE_ID             0x10   /* 6 bytes: MAC address           */
#define AX_MEDIUM_STATUS_MODE  0x22   /* 2 bytes                        */
#define AX_RX_CTL              0x0B   /* 2 bytes                        */
#define AX_PHYPWR_RSTCTL       0x26   /* 2 bytes                        */
#define AX_CLK_SELECT          0x33   /* 1 byte                         */
#define AX_RX_BULKIN_QCTRL     0x2E   /* 5 bytes                        */
#define AX_PAUSE_WATERLVL_HIGH 0x54
#define AX_PAUSE_WATERLVL_LOW  0x55
#define AX_MONITOR_MODE        0x24

#define AX_PHYPWR_RSTCTL_IPRL  0x0020
#define AX_CLK_SELECT_BCS      0x01
#define AX_CLK_SELECT_ACS      0x02

#define AX_RX_CTL_PROMISC      0x0001
#define AX_RX_CTL_AB           0x0008   /* accept broadcast            */
#define AX_RX_CTL_AM           0x0010   /* accept multicast            */
#define AX_RX_CTL_AP           0x0020   /* accept physical (this MAC)  */
#define AX_RX_CTL_START        0x0080   /* start RX                    */
#define AX_RX_CTL_DROPCRCERR   0x0100
#define AX_RX_CTL_IPE          0x0200   /* 2-byte IP-header alignment  */

#define AX_MEDIUM_GIGAMODE      0x0001
#define AX_MEDIUM_FULL_DUPLEX   0x0002
#define AX_MEDIUM_RXFLOW_CTRLEN 0x0010
#define AX_MEDIUM_TXFLOW_CTRLEN 0x0020
#define AX_MEDIUM_RECEIVE_EN    0x0100

/* RX aggregation trailer flags (per-packet descriptor) */
#define AX_RXHDR_CRC_ERR       0x20000000u
#define AX_RXHDR_DROP_ERR      0x80000000u

/* ---- state --------------------------------------------------------------- */
static int  g_dev = -1;            /* index in the xHCI enumerated device list */
static int  g_found, g_bound;
static u8   g_mac[6];
static u16  g_vid, g_pid;
static uno_nic_t g_nic;

static int  ax_read(int reg, int size, void *buf)
{ return uno_usb_control(g_dev, 0xC0, AX_ACCESS_MAC, (u16)reg, (u16)size, buf, size); }
static int  ax_write(int reg, int size, const void *buf)
{ return uno_usb_control(g_dev, 0x40, AX_ACCESS_MAC, (u16)reg, (u16)size, (void *)buf, size); }
static int  ax_wr16(int reg, u16 v) { u16 t = v; return ax_write(reg, 2, &t); }
static int  ax_wr8 (int reg, u8 v)  { u8  t = v; return ax_write(reg, 1, &t); }
/* MDIO read of an embedded-PHY register (AX_ACCESS_PHY: value=phy id, index=
 * reg - note the operand order differs from AX_ACCESS_MAC's value=reg,index=len). */
static u16  ax_mii_rd(int reg)
{ u16 t = 0; uno_usb_control(g_dev, 0xC0, AX_ACCESS_PHY, AX88179_PHY_ID, (u16)reg, &t, 2); return t; }

/* find the bulk in/out endpoint addresses from the config descriptor */
static int find_bulk_eps(int *in_ep, int *out_ep, int *in_mps, int *out_mps)
{
    static u8 cfg[512];
    int total, i, got_in = 0, got_out = 0;
    int n = uno_usb_get_config(g_dev, cfg, sizeof cfg);
    if (n < 9) return -1;
    total = cfg[2] | (cfg[3] << 8); if (total > (int)sizeof cfg) total = n;
    for (i = 0; i + 2 <= total; ) {
        int len = cfg[i], type = cfg[i+1];
        if (len < 2) break;
        if (type == 0x05 && i + 6 <= total) {          /* ENDPOINT descriptor */
            int addr = cfg[i+2], attr = cfg[i+3];
            int mps  = cfg[i+4] | (cfg[i+5] << 8);
            if ((attr & 0x03) == 0x02) {               /* bulk */
                if ((addr & 0x80) && !got_in)  { *in_ep = addr;  *in_mps = mps;  got_in = 1; }
                if (!(addr & 0x80) && !got_out){ *out_ep = addr; *out_mps = mps; got_out = 1; }
            }
        }
        i += len;
    }
    return (got_in && got_out) ? 0 : -1;
}

static void ax_reset(void)
{
    u8 qctrl[5] = { 0x07, 0x4F, 0x00, 0x12, 0xFF };    /* bulk-in queue (USB3 defaults) */
    /* PHY reset + release */
    ax_wr16(AX_PHYPWR_RSTCTL, 0x0000);        uno_pc64_delay_ms(20);
    ax_wr16(AX_PHYPWR_RSTCTL, AX_PHYPWR_RSTCTL_IPRL); uno_pc64_delay_ms(200);
    /* clocks */
    ax_wr8(AX_CLK_SELECT, AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS); uno_pc64_delay_ms(100);
    /* MAC address */
    ax_read(AX_NODE_ID, 6, g_mac);
    /* RX bulk-in aggregation + pause water levels */
    ax_write(AX_RX_BULKIN_QCTRL, 5, qctrl);
    ax_wr8(AX_PAUSE_WATERLVL_HIGH, 0x34);
    ax_wr8(AX_PAUSE_WATERLVL_LOW,  0x2C);
    ax_wr8(AX_MONITOR_MODE, 0x00);
    /* medium: gigabit, full duplex, RX + flow control on */
    ax_wr16(AX_MEDIUM_STATUS_MODE,
            AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN | AX_MEDIUM_RXFLOW_CTRLEN |
            AX_MEDIUM_FULL_DUPLEX | AX_MEDIUM_GIGAMODE);
    /* RX control: start, accept our MAC + broadcast + multicast, IP-align, drop bad CRC */
    ax_wr16(AX_RX_CTL, AX_RX_CTL_START | AX_RX_CTL_AP | AX_RX_CTL_AB | AX_RX_CTL_AM |
                       AX_RX_CTL_IPE | AX_RX_CTL_DROPCRCERR);
}

/* ---- uno_nic_t: send / recv / link -------------------------------------- */
static int ax_send(void *ctx, const void *pkt, int len)
{
    static u8 tx[2048];
    u32 h1, h2;
    (void)ctx;
    if (!g_bound || len <= 0 || len + 8 > (int)sizeof tx) return -1;
    /* 8-byte TX header: [len][flags]; the pad bit avoids a 512-byte-multiple
     * transfer that would need a zero-length terminating packet. */
    h1 = (u32)len;
    h2 = (((len + 8) % 512) == 0) ? 0x80008000u : 0;
    tx[0]=(u8)h1; tx[1]=(u8)(h1>>8); tx[2]=(u8)(h1>>16); tx[3]=(u8)(h1>>24);
    tx[4]=(u8)h2; tx[5]=(u8)(h2>>8); tx[6]=(u8)(h2>>16); tx[7]=(u8)(h2>>24);
    memcpy(tx + 8, pkt, len);
    return uno_usb_bulk_out(g_dev, tx, len + 8) > 0 ? len : -1;
}

/* RX buffer + a saved dump of the last transfer for diagnostics */
static u8  g_rx[4096];
static int g_rx_len, g_rx_off, g_rx_cnt, g_rx_hdroff;

static int ax_recv(void *ctx, void *pkt, int cap)
{
    (void)ctx;
    if (!g_bound) return 0;
    /* refill: pull one bulk-in transfer, parse the trailing descriptor array */
    if (g_rx_cnt == 0) {
        int n = uno_usb_bulk_in(g_dev, g_rx, sizeof g_rx);
        if (n < 8) return 0;
        { u32 hdr = g_rx[n-4] | (g_rx[n-3]<<8) | (g_rx[n-2]<<16) | (u32)(g_rx[n-1]<<24);
          g_rx_cnt = hdr & 0xFFFF; g_rx_hdroff = (hdr >> 16); g_rx_off = 0; g_rx_len = n; }
        if (g_rx_cnt == 0 || g_rx_hdroff + 4 > g_rx_len) { g_rx_cnt = 0; return 0; }
    }
    /* pop the next packet: its descriptor is at g_rx_hdroff, data at g_rx_off.
       g_rx_cnt persists across calls, so re-validate the descriptor offset on
       every pop (not just on refill) - a device-supplied count must never walk
       the descriptor pointer past g_rx[]. */
    if (g_rx_hdroff + 4 > g_rx_len || g_rx_off > g_rx_len) { g_rx_cnt = 0; return 0; }
    { u32 *desc = (u32 *)(g_rx + g_rx_hdroff);
      u32 d = *desc;
      int pkt_len = (int)((d >> 16) & 0x1FFF);
      int step = (pkt_len + 7) & ~7;
      int frame = g_rx_off + 2;                 /* 2-byte IP alignment pad */
      int flen  = pkt_len - 2;                  /* frame length after the pad */
      g_rx_hdroff += 4; g_rx_off += step; g_rx_cnt--;
      if (d & (AX_RXHDR_CRC_ERR | AX_RXHDR_DROP_ERR)) return 0;   /* drop, try next call */
      if (flen <= 0 || frame + flen > g_rx_len) return 0;
      if (flen > cap) flen = cap;
      memcpy(pkt, g_rx + frame, flen);
      return flen; }
}

static int ax_link(void *ctx)
{
    u16 bmsr;
    (void)ctx;
    if (!g_bound) return 0;
    /* Read the PHY's real link status. The old code returned
     * AX_MEDIUM_STATUS_MODE & AX_MEDIUM_RECEIVE_EN, but RECEIVE_EN is a bit the
     * driver itself sets in ax_reset() and never clears, so it reported "up"
     * permanently even with the cable unplugged. BMSR bit 2 latches low on a
     * link drop, so read twice and take the second read as the current state. */
    ax_mii_rd(MII_BMSR);
    bmsr = ax_mii_rd(MII_BMSR);
    return (bmsr & MII_BMSR_LSTATUS) ? 1 : 0;
}

/* ---- bring-up ------------------------------------------------------------ */
uno_nic_t *ax88179_nic(void)
{
    int i, n, in_ep = 0, out_ep = 0, in_mps = 512, out_mps = 512;
    if (g_bound) return &g_nic;
    n = uno_xhci_dev_count();
    for (i = 0; i < n; i++) {
        const uno_usb_dev *d = uno_xhci_dev(i);
        if (d && d->vendor == 0x0b95) { g_dev = i; g_found = 1; g_vid = d->vendor; g_pid = d->product; break; }
    }
    if (!g_found) return 0;

    if (uno_usb_set_config(g_dev, 1) < 0) return 0;
    if (find_bulk_eps(&in_ep, &out_ep, &in_mps, &out_mps) < 0) return 0;
    if (uno_usb_setup_bulk(g_dev, in_ep, out_ep, in_mps, out_mps) < 0) return 0;
    ax_reset();

    g_nic.ctx = 0; g_nic.send = ax_send; g_nic.recv = ax_recv; g_nic.link = ax_link;
    g_bound = 1;
    return &g_nic;
}

const unsigned char *ax88179_mac(void) { return g_mac; }

void ax88179_status(int *found, int *bound, int *link, unsigned short *vid, unsigned short *pid)
{
    if (found) *found = g_found;
    if (bound) *bound = g_bound;
    if (link)  *link  = g_bound ? ax_link(0) : 0;
    if (vid)   *vid   = g_vid;
    if (pid)   *pid   = g_pid;
}
