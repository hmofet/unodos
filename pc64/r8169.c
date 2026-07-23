/* ===========================================================================
 * UnoDOS/pc64 - Realtek RTL8168/8111 Gigabit Ethernet driver (see r8169.h).
 *
 * The most common consumer wired NIC: the Realtek RTL8168/8111 (r8169) on the
 * vast majority of desktop boards and many laptops, plus the RTL8101/8106 Fast
 * Ethernet and the RTL8125 2.5GbE. From-scratch, polled (no MSI), 16-byte
 * descriptor rings over MMIO. No firmware. Register map, chip-version detection,
 * descriptor layout, MAC read and the bring-up sequence follow the Linux r8169
 * driver exactly. DMA is identity-mapped (virt==phys under UEFI boot services).
 *
 * The mainstream RTL8168 (8168g/h, MAC version >= 40) datapath is implemented;
 * the per-version EPHY/ERI electrical tuning Linux carries is skipped (link
 * comes up without it, at slightly reduced margin) - the one tuning write that
 * RX actually needs on VER_40+ (clearing MISC.RXDV_GATED_EN) IS done. The
 * 8125-family register relocation (IntrMask/TxPoll moved) is handled.
 *
 * QEMU has no RTL8168 model (only the ancient rtl8139), so this is verified only
 * to be INERT when no supported card is on the bus - real bring-up is the metal
 * tail. Publishes uno_nic_t.
 * ======================================================================== */
#include "r8169.h"
#include "pc64_pci.h"
#include "uno_debug.h"          /* uno_dbg_net_trace: on-screen in UNO_DEBUG, no-op in prod */
#include <stdint.h>
#include <string.h>

void uno_pc64_delay_ms(int ms); /* firmware Stall (idle pacing) */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* ---- registers (r8169_main.c) ------------------------------------------- */
#define MAC0        0x00
#define MAC4        0x04
#define MAR0        0x08
#define TxDescLow   0x20
#define TxDescHigh  0x24
#define ChipCmd     0x37
#define  CmdReset   0x10
#define  CmdRxEnb   0x08
#define  CmdTxEnb   0x04
#define TxPoll      0x38          /* 8168: 8-bit */
#define  NPQ        0x40
#define IntrMask    0x3C          /* 8168: 16-bit */
#define IntrStatus  0x3E
#define TxConfig    0x40
#define RxConfig    0x44
#define Cfg9346     0x50
#define  Cfg9346_Unlock 0xC0
#define  Cfg9346_Lock   0x00
#define PHYAR       0x60
#define PHYstatus   0x6C
#define  LinkStatus 0x02
#define  _1000bpsF  0x10
#define  _100bps    0x08
#define  _10bps     0x04
#define RxMaxSize   0xDA
#define CPlusCmd    0xE0
#define IntrMitigate 0xE2
#define RxDescLow   0xE4
#define RxDescHigh  0xE8
#define MaxTxPktSz  0xEC
#define MISC        0xF0
#define  RXDV_GATED_EN 0x00080000u
/* RTL8125 relocated registers */
#define IntrMask_8125   0x38      /* 32-bit */
#define IntrStatus_8125 0x3C
#define TxPoll_8125     0x90      /* 16-bit */

/* descriptor opts1 bits */
#define DescOwn   0x80000000u
#define RingEnd   0x40000000u
#define FirstFrag 0x20000000u
#define LastFrag  0x10000000u
#define RxRES     0x00200000u
#define RX_LEN_MASK 0x00003FFFu

/* ---- ring geometry ------------------------------------------------------ */
#define TXN   64
#define RXN   64
#define BUFSZ 2048
#define RXMAX 1536                /* max accepted frame (standard MTU + headers) */

struct desc { u32 opts1; u32 opts2; u64 addr; } __attribute__((packed));
static struct desc g_tx[TXN] __attribute__((aligned(256)));
static struct desc g_rx[RXN] __attribute__((aligned(256)));
static u8 g_txbuf[TXN][BUFSZ] __attribute__((aligned(8)));
static u8 g_rxbuf[RXN][BUFSZ] __attribute__((aligned(8)));

/* ---- state -------------------------------------------------------------- */
static volatile u8 *g_mmio;
static pci_dev g_pci;
static int g_present, g_up, g_is8125;
static u8  g_mac[6];
static u16 g_devid;
static int g_tx_cur, g_rx_cur;
static uno_nic_t g_nic;
static u64 phys(const void *p){ return (u64)(uintptr_t)p; }

static u8  r8 (u32 o){ return *(volatile u8  *)(g_mmio+o); }
static u16 r16(u32 o){ return *(volatile u16 *)(g_mmio+o); }
static u32 r32(u32 o){ return *(volatile u32 *)(g_mmio+o); }
static void w8 (u32 o,u8  v){ *(volatile u8  *)(g_mmio+o)=v; }
static void w16(u32 o,u16 v){ *(volatile u16 *)(g_mmio+o)=v; }
static void w32(u32 o,u32 v){ *(volatile u32 *)(g_mmio+o)=v; }
static void commit(void){ (void)r8(ChipCmd); }
static void spin(volatile int n){ while(n-->0) __asm__ volatile(""); }

/* ---- device match ------------------------------------------------------- */
static int is_realtek_vendor(u16 v)
{ return v==0x10ec || v==0x1186 || v==0x1259 || v==0x16ec || v==0x1737 || v==0x10ff; }

/* is this an 8125-family part (relocated IntrMask/TxPoll)? by device id + the
 * TxConfig XID (VER_61+). */
static int detect_8125(u16 dev)
{
    u32 xid;
    if (dev==0x8125 || dev==0x8126 || dev==0x8127) return 1;
    xid = (r32(TxConfig) >> 20) & 0xfcf;
    switch (xid & 0x7cf) {   /* the 2.5G+/8125 XID patterns */
    case 0x609: case 0x641: case 0x649: case 0x64a: case 0x681:
    case 0x688: case 0x689: case 0x68a: case 0x68b: case 0x6c9: case 0x708:
        return 1;
    }
    return 0;
}

/* ---- MAC + link --------------------------------------------------------- */
static void read_mac(void)
{
    u32 lo = r32(MAC0), hi = r32(MAC4);
    g_mac[0]=(u8)lo; g_mac[1]=(u8)(lo>>8); g_mac[2]=(u8)(lo>>16); g_mac[3]=(u8)(lo>>24);
    g_mac[4]=(u8)hi; g_mac[5]=(u8)(hi>>8);
}
static int link_up(void){ return (r8(PHYstatus) & LinkStatus) ? 1 : 0; }

/* ---- ring init ---------------------------------------------------------- */
static void rings_init(void)
{
    int i;
    for (i=0;i<TXN;i++){ g_tx[i].opts1=0; g_tx[i].opts2=0; g_tx[i].addr=phys(g_txbuf[i]); }
    g_tx[TXN-1].opts1 = RingEnd;
    for (i=0;i<RXN;i++){
        g_rx[i].opts2=0; g_rx[i].addr=phys(g_rxbuf[i]);
        g_rx[i].opts1 = DescOwn | (i==RXN-1?RingEnd:0) | BUFSZ;
    }
    g_tx_cur=0; g_rx_cur=0;
}

/* ---- bring-up (minimal mandatory subset for VER_40+ / common 8168) ------ */
static int hw_start(void)
{
    int t;
    /* soft reset */
    w8(ChipCmd, CmdReset);
    for (t=0;t<1000;t++){ if(!(r8(ChipCmd)&CmdReset)) break; spin(2000); }
    if (r8(ChipCmd)&CmdReset) {
        uno_dbg_net_trace("r8169: soft-reset never cleared (ChipCmd=%02x) - BAR/reset failure",
                          r8(ChipCmd));
        return -1;
    }
    uno_dbg_net_trace("r8169: soft-reset cleared (t=%d)", t);

    read_mac();
    rings_init();

    w8(Cfg9346, Cfg9346_Unlock);
    w16(CPlusCmd, (u16)(r16(CPlusCmd) & 0x2063 /*CPCMD_MASK*/));
    w8(MaxTxPktSz, 0x3f);
    w16(RxMaxSize, RXMAX);
    /* program ring addresses (high then low) */
    w32(TxDescHigh, (u32)(phys(g_tx)>>32)); w32(TxDescLow, (u32)phys(g_tx));
    w32(RxDescHigh, (u32)(phys(g_rx)>>32)); w32(RxDescLow, (u32)phys(g_rx));
    w16(IntrMitigate, 0);
    w8(Cfg9346, Cfg9346_Lock);
    commit();

    /* the one tuning write RX needs on VER_40+: clear the RX-DV gate */
    if (!g_is8125) w32(MISC, r32(MISC) & ~RXDV_GATED_EN);

    /* enable TX + RX */
    w8(ChipCmd, CmdRxEnb | CmdTxEnb);
    /* RxConfig DMA/FIFO baseline */
    w32(RxConfig, g_is8125 ? 0x40000F00u : 0x0000CF00u);
    /* TxConfig: DMA burst 7, IFG 3, AUTO_FIFO */
    w32(TxConfig, (7u<<8) | (3u<<24) | (1u<<7));
    /* accept broadcast + my-mac + multicast; all-ones multicast hash */
    w32(MAR0, 0xFFFFFFFFu); w32(MAR0+4, 0xFFFFFFFFu);
    w32(RxConfig, (r32(RxConfig) & ~0x0Fu) | 0x0E);
    /* poll instead of interrupt */
    if (g_is8125) w32(IntrMask_8125, 0);
    else          w16(IntrMask, 0);
    return 0;
}

/* ---- uno_nic_t ---------------------------------------------------------- */
static int r8169_send(void *ctx, const void *pkt, int len)
{
    int idx = g_tx_cur % TXN;
    struct desc *d = &g_tx[idx];
    u32 opts1;
    (void)ctx;
    if (!g_up || len <= 0 || len > BUFSZ) return -1;
    if (len < 60) len = 60;                         /* pad to ETH_ZLEN; HW appends FCS */
    /* wait for the slot we're about to reuse to be free (not owned) */
    { int s=100000; while ((d->opts1 & DescOwn) && s--) spin(10); }
    memcpy(g_txbuf[idx], pkt, len);
    d->addr = phys(g_txbuf[idx]);
    d->opts2 = 0;
    opts1 = (u32)len | FirstFrag | LastFrag | (idx==TXN-1?RingEnd:0);
    d->opts1 = opts1;                                /* fields visible... */
    __asm__ volatile("" ::: "memory");
    d->opts1 = opts1 | DescOwn;                      /* ...then hand to NIC */
    g_tx_cur++;
    if (g_is8125) w16(TxPoll_8125, 1); else w8(TxPoll, NPQ);   /* doorbell */
    return len;
}

static int r8169_recv(void *ctx, void *pkt, int cap)
{
    int idx = g_rx_cur % RXN;
    struct desc *d = &g_rx[idx];
    u32 st;
    int len;
    (void)ctx;
    if (!g_up) return 0;
    st = d->opts1;
    if (st & DescOwn) return 0;                      /* still owned by NIC: nothing */
    __asm__ volatile("" ::: "memory");
    if (!(st & RxRES) && (st & (FirstFrag|LastFrag)) == (FirstFrag|LastFrag)) {
        len = (int)(st & RX_LEN_MASK) - 4;           /* strip FCS */
        if (len > 0) {
            if (len > cap) len = cap;
            memcpy(pkt, g_rxbuf[idx], len);
        } else len = 0;
    } else len = 0;                                  /* error / fragmented: drop */
    /* re-arm the descriptor (preserve EOR) */
    d->opts2 = 0;
    __asm__ volatile("" ::: "memory");
    d->opts1 = DescOwn | (idx==RXN-1?RingEnd:0) | BUFSZ;
    g_rx_cur++;
    return len;
}

static int r8169_link(void *ctx){ (void)ctx; return g_up ? link_up() : 0; }

/* ---- debug bring-up aid: watch the link for ~4s ------------------------- *
 * hw_start() deliberately never powers the PHY up or restarts autoneg, so on
 * real silicon fresh out of UEFI (which parks the PHY) this is where we learn,
 * empirically, whether link ever asserts on its own.  Prints the MAC, then
 * PHYstatus every 250ms, logging only on change plus a final summary.
 * Compiled out entirely in production - the uno_pc64_delay_ms() is real time. */
#ifdef UNO_DEBUG
static void r8169_phy_poll(void)
{
    int i, last = -1;
    uno_dbg_net_trace("r8169: MAC %02x:%02x:%02x:%02x:%02x:%02x - polling PHYstatus ~4s",
                      g_mac[0],g_mac[1],g_mac[2],g_mac[3],g_mac[4],g_mac[5]);
    for (i = 0; i < 16; i++) {
        int ps = r8(PHYstatus);
        if (ps != last) {
            uno_dbg_net_trace("r8169:  +%dms PHYstatus=%02x link=%d speed=%s",
                              i*250, ps, (ps & LinkStatus) ? 1 : 0,
                              (ps & _1000bpsF) ? "1000" :
                              (ps & _100bps)   ? "100"  :
                              (ps & _10bps)    ? "10"   : "-");
            last = ps;
        }
        uno_pc64_delay_ms(250);
    }
    uno_dbg_net_trace("r8169:  PHY poll done - final link=%s", link_up() ? "UP" : "DOWN");
}
#else
static void r8169_phy_poll(void) { }
#endif

int r8169_present(void)
{
    pci_dev d;
    if (g_present) return 1;
    /* Realtek GbE: network class 0x02 subclass 0x00 (ethernet). */
    if (pci_find_class(0x02, 0x00, &d)) {
        u16 dev = pci_cfg_read16(&d, 2);
        if (is_realtek_vendor(d.vendor) &&
            (dev==0x8168||dev==0x8161||dev==0x8162||dev==0x8167||dev==0x8169||
             dev==0x8136||dev==0x8125||dev==0x8126||dev==0x8127||dev==0x2600||
             dev==0x8129||dev==0x0e10||dev==0x3000||dev==0x5000)) {
            g_pci = d; g_devid = dev; g_present = 1;
            uno_dbg_net_trace("r8169: pci match %04x:%04x at %02x:%02x.%d (class 02:00)",
                              d.vendor, dev, d.bus, d.dev, d.fn);
            return 1;
        }
        uno_dbg_net_trace("r8169: ethernet %04x:%04x at %02x:%02x.%d present but not a "
                          "recognized RTL8168/8125 device", d.vendor, dev, d.bus, d.dev, d.fn);
    }
    return 0;
}

uno_nic_t *r8169_nic(void)
{
    u64 bar2, bar0;
    if (g_up) return &g_nic;
    if (!r8169_present()) return 0;
    pci_enable_bus_master(&g_pci);
    /* the register window is the first memory BAR (BAR2 on most 8168, BAR0 some) */
    bar2 = pci_bar(&g_pci, 2);
    bar0 = pci_bar(&g_pci, 0);
    g_mmio = (volatile u8 *)(uintptr_t)(bar2 ? bar2 : bar0);
    uno_dbg_net_trace("r8169: BAR2=%llx BAR0=%llx -> mmio=%p (%s)",
                      bar2, bar0, (void *)g_mmio,
                      bar2 ? "BAR2" : (bar0 ? "BAR0" : "NONE"));
    if (!g_mmio) return 0;
    g_is8125 = detect_8125(g_devid);
    uno_dbg_net_trace("r8169: TxConfig=%08x XID=%03x is8125=%d",
                      r32(TxConfig), (unsigned)((r32(TxConfig) >> 20) & 0xfcf), g_is8125);
    if (hw_start() < 0) return 0;
    g_nic.ctx=0; g_nic.send=r8169_send; g_nic.recv=r8169_recv; g_nic.link=r8169_link;
    g_up = 1;
    r8169_phy_poll();       /* debug: watch link for ~4s (compiled out in prod) */
    return &g_nic;
}

const unsigned char *r8169_mac(void){ return g_mac; }

/* ---- live debug hook: the `eth` verb over unoautomate ------------------- *
 * status | reg <off> | wreg <off> <val> | phy <reg> | wphy <reg> <val> |
 * link | mac | rerun.  MMIO + PHY peek/poke so the metal bring-up can be
 * driven register-by-register over the remote channel without reflashing
 * (unoauto_remote.c's eth verb calls this).  This is the STRONG definition;
 * it overrides the weak "not built" fallback shipped in unoauto_remote.c.
 * Returns reply length written to out, or -1 only on an unknown subcommand
 * (a recognized-but-failed command still writes an explanatory reply). */

/* PHYAR MDIO access (Linux r8169 r8168_phy_read/write): read = write reg<<16
 * then poll for bit31 SET; write = set bit31|reg<<16|val then poll for clear. */
static int phy_read(int reg)
{
    int t;
    w32(PHYAR, (u32)((reg & 0x1f) << 16));
    for (t = 0; t < 2000; t++) {
        u32 v = r32(PHYAR);
        if (v & 0x80000000u) return (int)(v & 0xffff);
        spin(200);
    }
    return -1;
}
static void phy_write(int reg, u16 val)
{
    int t;
    w32(PHYAR, 0x80000000u | (u32)((reg & 0x1f) << 16) | val);
    for (t = 0; t < 2000; t++) { if (!(r32(PHYAR) & 0x80000000u)) return; spin(200); }
}

/* tiny bounded output builder + arg parsing (freestanding, no libc printf) */
struct ob { char *p; int cap; int len; };
static void ob_c(struct ob *o, char c){ if (o->len < o->cap - 1) o->p[o->len++] = c; }
static void ob_s(struct ob *o, const char *s){ while (*s) ob_c(o, *s++); }
static void ob_u(struct ob *o, u32 v)
{ char t[12]; int n = 0; if (!v){ ob_c(o,'0'); return; } while (v){ t[n++]='0'+(char)(v%10); v/=10; } while (n) ob_c(o, t[--n]); }
static void ob_hex(struct ob *o, u32 v, int digits)
{ static const char H[] = "0123456789abcdef"; int i; for (i=(digits-1)*4; i>=0; i-=4) ob_c(o, H[(v>>i)&0xf]); }
static u32 ahex(const char *s, const char **end)
{
    u32 v = 0;
    while (*s==' '||*s=='\t') s++;
    if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) s += 2;
    for (;;) { char c=*s; int d;
        if (c>='0'&&c<='9') d=c-'0'; else if (c>='a'&&c<='f') d=c-'a'+10;
        else if (c>='A'&&c<='F') d=c-'A'+10; else break;
        v=(v<<4)|(u32)d; s++; }
    if (end) *end = s;
    return v;
}
/* whole-token match: advance past kw (+ trailing spaces) iff the leading token equals kw */
static int mtok(const char **s, const char *kw)
{
    const char *p = *s;
    while (*kw){ if (*p != *kw) return 0; p++; kw++; }
    if (*p && *p!=' ' && *p!='\t') return 0;
    while (*p==' '||*p=='\t') p++;
    *s = p; return 1;
}

int r8169_dbg_cmd(const char *line, char *out, int cap)
{
    struct ob o; const char *s = line ? line : "";
    o.p = out; o.cap = cap; o.len = 0;
    if (cap <= 0) return -1;
    while (*s==' '||*s=='\t') s++;

    if (mtok(&s, "status")) {
        ob_s(&o, "present="); ob_u(&o, (u32)r8169_present());
        ob_s(&o, " up=");     ob_u(&o, (u32)g_up);
        ob_s(&o, " dev=");    ob_hex(&o, g_devid, 4);
        ob_s(&o, " is8125="); ob_u(&o, (u32)g_is8125);
        ob_s(&o, " mmio=");   ob_hex(&o, (u32)(uintptr_t)g_mmio, 8);
        if (g_mmio) {
            ob_s(&o, " ChipCmd=");   ob_hex(&o, r8(ChipCmd), 2);
            ob_s(&o, " PHYstatus="); ob_hex(&o, r8(PHYstatus), 2);
            ob_s(&o, " link=");      ob_u(&o, (u32)link_up());
        } else ob_s(&o, " (window not mapped - run 'rerun')");
    }
    else if (mtok(&s, "rerun")) {
        g_up = 0; g_present = 0;                 /* force a fresh probe+map+bring-up */
        ob_s(&o, r8169_nic() ? "rerun ok - link=" : "rerun FAILED (see screen trace) link=");
        ob_u(&o, (u32)(g_mmio ? link_up() : 0));
    }
    else if (mtok(&s, "link")) {
        if (!g_mmio) { ob_s(&o, "window not mapped - run 'rerun'"); }
        else { int ps = r8(PHYstatus);
            ob_s(&o, "PHYstatus="); ob_hex(&o, (u32)ps, 2);
            ob_s(&o, " link=");  ob_u(&o, (ps & LinkStatus) ? 1u : 0u);
            ob_s(&o, " speed=");
            ob_s(&o, (ps & _1000bpsF) ? "1000" : (ps & _100bps) ? "100" : (ps & _10bps) ? "10" : "-"); }
    }
    else if (mtok(&s, "mac")) {
        int i; if (g_mmio) read_mac();
        for (i = 0; i < 6; i++) { if (i) ob_c(&o, ':'); ob_hex(&o, g_mac[i], 2); }
    }
    else if (mtok(&s, "reg")) {
        if (!g_mmio) { ob_s(&o, "window not mapped - run 'rerun'"); }
        else { u32 off = ahex(s, 0);
            ob_s(&o, "["); ob_hex(&o, off, 3); ob_s(&o, "]=");
            ob_hex(&o, r32(off), 8); }
    }
    else if (mtok(&s, "wreg")) {
        if (!g_mmio) { ob_s(&o, "window not mapped - run 'rerun'"); }
        else { const char *e; u32 off = ahex(s, &e); u32 val = ahex(e, 0);
            w32(off, val); commit();
            ob_s(&o, "["); ob_hex(&o, off, 3); ob_s(&o, "]<-"); ob_hex(&o, val, 8);
            ob_s(&o, " now="); ob_hex(&o, r32(off), 8); }
    }
    else if (mtok(&s, "phy")) {
        if (!g_mmio) { ob_s(&o, "window not mapped - run 'rerun'"); }
        else { int reg = (int)ahex(s, 0), v = phy_read(reg);
            ob_s(&o, "phy["); ob_hex(&o, (u32)reg, 2); ob_s(&o, "]=");
            if (v < 0) ob_s(&o, "timeout"); else ob_hex(&o, (u32)v, 4); }
    }
    else if (mtok(&s, "wphy")) {
        if (!g_mmio) { ob_s(&o, "window not mapped - run 'rerun'"); }
        else { const char *e; int reg = (int)ahex(s, &e); u32 val = ahex(e, 0);
            phy_write(reg, (u16)val);
            ob_s(&o, "phy["); ob_hex(&o, (u32)reg, 2); ob_s(&o, "]<-"); ob_hex(&o, val, 4);
            ob_s(&o, " now="); { int v = phy_read(reg); if (v<0) ob_s(&o,"timeout"); else ob_hex(&o,(u32)v,4); } }
    }
    else { out[0] = 0; return -1; }             /* unknown subcommand */

    out[o.len] = 0;
    return o.len;
}
