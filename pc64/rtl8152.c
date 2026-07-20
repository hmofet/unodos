/* Realtek RTL8152/RTL8153/RTL8156 USB Ethernet driver (see rtl8152.h).
 *
 * Register access is RTL8152_REQ_GET/SET_REGS vendor control transfers over
 * the xHCI stack (uno_usb_*); frames move over bulk endpoints 1 (in) / 2 (out).
 * The OCP register map, version detection, per-family init and the RX/TX
 * descriptor formats follow the Linux r8152 driver. The heavy analog PHY
 * tuning tables and the optional MCU/PHY firmware patches are omitted - the
 * link trains without them (at slightly reduced margin), which is the right
 * trade for a compact bare-metal driver. Covers the v1-descriptor chips
 * (8152/8153/8155/8156), i.e. essentially every USB dongle and laptop dock;
 * the 16-byte-descriptor 8157/8159 (5G/10G) are recognised but declined.
 * Inert unless the USB stack is present (uno_usb_* stubs return -1). */
#include "rtl8152.h"
#include "xhci.h"
#include <string.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

void uno_pc64_delay_ms(int ms);

/* ---- vendor request protocol -------------------------------------------- */
#define REQT_READ        0xc0
#define REQT_WRITE       0x40
#define REQ_GET_REGS     0x05
#define REQ_SET_REGS     0x05
#define MCU_TYPE_PLA     0x0100
#define MCU_TYPE_USB     0x0000
#define BYTE_EN_DWORD    0xff
#define BYTE_EN_WORD     0x33
#define BYTE_EN_BYTE     0x11
#define BYTE_EN_SIX      0x3f

/* ---- registers actually used (addresses verbatim from the Linux driver) -- */
#define PLA_IDR          0xc000
#define PLA_RCR          0xc010
#define PLA_RMS          0xc016
#define PLA_RXFIFO_CTRL0 0xc0a0
#define PLA_RXFIFO_CTRL1 0xc0a4
#define PLA_RXFIFO_CTRL2 0xc0a8
#define PLA_FMC          0xc0b4
#define PLA_TEREDO_CFG   0xc0bc
#define PLA_MAR          0xcd00
#define PLA_BACKUP       0xd000
#define PLA_PHY_PWR      0xe84c
#define PLA_BOOT_CTRL    0xe004
#define PLA_LED_FEATURE  0xdd92
#define PLA_TCR0         0xe610
#define PLA_TXFIFO_CTRL  0xe618
#define PLA_RSTTALLY     0xe800
#define PLA_CR           0xe813
#define PLA_CRWECR       0xe81c
#define PLA_CPCR         0xe854
#define PLA_MISC_1       0xe85a
#define PLA_OCP_GPHY_BASE 0xe86c
#define PLA_SFF_STS_7    0xe8de
#define PLA_OOB_CTRL     0xe84f
#define PLA_PHYSTATUS    0xe908
#define PLA_CONFIG6      0xe90a
#define PLA_MAC_PWR_CTRL2 0xe0ca
#define PLA_MAC_PWR_CTRL3 0xe0cc
#define PLA_TCR1         0xe612

#define USB_USB_CTRL     0xd406
#define USB_TX_AGG       0xd40a
#define USB_RX_BUF_TH    0xd40c
#define USB_RX_EARLY_TIMEOUT 0xd42c
#define USB_RX_EARLY_SIZE 0xd42e
#define USB_RX_EXTRA_AGGR_TMR 0xd432
#define USB_TX_DMA       0xd434
#define USB_UPT_RXDMA_OWN 0xd437
#define USB_BMU_RESET    0xd4b0
#define USB_U2P3_CTRL    0xb460
#define USB_LPM_CONFIG   0xcfd8
#define USB_MSC_TIMER    0xcbfc
#define USB_ECM_OPTION   0xcfee
#define USB_ECM_OP       0xd26b
#define USB_SPEED_OPTION 0xd32a
#define USB_U1U2_TIMER   0xd4da
#define USB_TOLERANCE    0xd490

/* bits */
#define RCR_AAP  0x00000001
#define RCR_APM  0x00000002
#define RCR_AM   0x00000004
#define RCR_AB   0x00000008
#define RCR_ACPT_ALL (RCR_AAP|RCR_APM|RCR_AM|RCR_AB)
#define CR_RST   0x10
#define CR_RE    0x08
#define CR_TE    0x04
#define CRWECR_NORMAL 0x00
#define CRWECR_CONFIG 0xc0
#define NOW_IS_OOB    0x80
#define FIFO_EMPTY    0x30
#define LINK_LIST_READY 0x02
#define RE_INIT_LL    0x8000
#define MCU_BORW_EN   0x4000
#define RXDY_GATED_EN 0x0008
#define TCR0_AUTO_FIFO 0x0080
#define TCR0_TX_EMPTY  0x0800
#define AUTOLOAD_DONE 0x0002
#define RX_AGG_DISABLE 0x0010
#define RX_ZERO_EN     0x0080
#define PFM_PWM_SWITCH 0x0040
#define LINK_STATUS    0x02
#define OCP_BASE_MII   0xa400
#define MII_BMCR       0
#define MII_ADVERTISE  4
#define MII_CTRL1000   9
#define BMCR_RESET     0x8000
#define BMCR_ANENABLE  0x1000
#define BMCR_PDOWN     0x0800
#define BMCR_ANRESTART 0x0200

/* ---- state -------------------------------------------------------------- */
enum { FAM_NONE, FAM_8152, FAM_8153A, FAM_8153B, FAM_8156 };

static int  g_dev = -1;
static int  g_found, g_bound, g_link_up;
static u8   g_mac[6];
static u16  g_vid, g_pid;
static int  g_version, g_family;
static u16  g_ocp_base = 0xffff;      /* cached PHY OCP page (force first set) */
static uno_nic_t g_nic;

/* one bulk-in aggregation buffer + a per-call parse cursor */
#define RXBUF 16384
static u8  g_rx[RXBUF] __attribute__((aligned(16)));
static int g_rx_have, g_rx_off, g_rx_armed;

/* ---- raw register access ------------------------------------------------ */
static int set_regs(u16 val, u16 idx, int size, const void *data)
{ return uno_usb_control(g_dev, REQT_WRITE, REQ_SET_REGS, val, idx, (void *)data, size); }
static int get_regs(u16 val, u16 idx, int size, void *data)
{ return uno_usb_control(g_dev, REQT_READ, REQ_GET_REGS, val, idx, data, size); }

/* generic OCP read: whole aligned dwords, chunked at 64 bytes */
static int gen_ocp_read(u16 index, int size, void *data, u16 type)
{
    u8 *p = (u8 *)data;
    while (size > 0) {
        int chunk = size > 64 ? 64 : size;
        if (get_regs(index, type, chunk, p) < 0) return -1;
        index += chunk; p += chunk; size -= chunk;
    }
    return 0;
}

/* dword/word/byte helpers - read the aligned dword, mask in software */
static u32 ocp_rd_dword(u16 type, u16 index)
{ u8 b[4] = {0,0,0,0}; gen_ocp_read(index, 4, b, type);
  return (u32)b[0] | ((u32)b[1]<<8) | ((u32)b[2]<<16) | ((u32)b[3]<<24); }
static u16 ocp_rd_word(u16 type, u16 index)
{ int sh=(index&2)*8; u32 v=ocp_rd_dword(type, index & ~3u); return (u16)((v>>sh)&0xffff); }
static u8 ocp_rd_byte(u16 type, u16 index)
{ int sh=(index&3)*8; u32 v=ocp_rd_dword(type, index & ~3u); return (u8)((v>>sh)&0xff); }

static void ocp_wr_dword(u16 type, u16 index, u32 v)
{ u8 b[4]; b[0]=(u8)v; b[1]=(u8)(v>>8); b[2]=(u8)(v>>16); b[3]=(u8)(v>>24);
  set_regs(index, (u16)(type | BYTE_EN_DWORD), 4, b); }
static void ocp_wr_word(u16 type, u16 index, u16 val)
{
    int shift = index & 2;
    u16 byen = BYTE_EN_WORD;
    u32 data = val;
    if (shift) { byen <<= shift; data <<= shift*8; index &= ~3u; }
    { u8 b[4]; b[0]=(u8)data; b[1]=(u8)(data>>8); b[2]=(u8)(data>>16); b[3]=(u8)(data>>24);
      set_regs(index, (u16)(type | byen), 4, b); }
}
static void ocp_wr_byte(u16 type, u16 index, u8 val)
{
    int shift = index & 3;
    u16 byen = BYTE_EN_BYTE;
    u32 data = val;
    if (shift) { byen <<= shift; data <<= shift*8; index &= ~3u; }
    { u8 b[4]; b[0]=(u8)data; b[1]=(u8)(data>>8); b[2]=(u8)(data>>16); b[3]=(u8)(data>>24);
      set_regs(index, (u16)(type | byen), 4, b); }
}

static void ocp_word_set(u16 type, u16 idx, u16 m) { ocp_wr_word(type, idx, (u16)(ocp_rd_word(type,idx)|m)); }
static void ocp_word_clr(u16 type, u16 idx, u16 m) { ocp_wr_word(type, idx, (u16)(ocp_rd_word(type,idx)&~m)); }
static void ocp_byte_set(u16 type, u16 idx, u8 m)  { ocp_wr_byte(type, idx, (u8)(ocp_rd_byte(type,idx)|m)); }
static void ocp_byte_clr(u16 type, u16 idx, u8 m)  { ocp_wr_byte(type, idx, (u8)(ocp_rd_byte(type,idx)&~m)); }
static void ocp_dword_clr(u16 type, u16 idx, u32 m){ ocp_wr_dword(type, idx, ocp_rd_dword(type,idx)&~m); }

/* ---- PHY (GPHY) access via the windowed OCP space ----------------------- */
static u16 ocp_reg_read(u16 addr)
{
    u16 base = addr & 0xf000, idx = (addr & 0x0fff) | 0xb000;
    if (base != g_ocp_base) { ocp_wr_word(MCU_TYPE_PLA, PLA_OCP_GPHY_BASE, base); g_ocp_base = base; }
    return ocp_rd_word(MCU_TYPE_PLA, idx);
}
static void ocp_reg_write(u16 addr, u16 data)
{
    u16 base = addr & 0xf000, idx = (addr & 0x0fff) | 0xb000;
    if (base != g_ocp_base) { ocp_wr_word(MCU_TYPE_PLA, PLA_OCP_GPHY_BASE, base); g_ocp_base = base; }
    ocp_wr_word(MCU_TYPE_PLA, idx, data);
}
static u16 mdio_read(int reg)          { return ocp_reg_read((u16)(OCP_BASE_MII + reg*2)); }
static void mdio_write(int reg, u16 v) { ocp_reg_write((u16)(OCP_BASE_MII + reg*2), v); }

/* ---- version detection -------------------------------------------------- */
static void detect_version(void)
{
    u32 v = ocp_rd_dword(MCU_TYPE_PLA, PLA_TCR0);
    u32 raw = (v >> 16) & 0x7cf0;
    g_version = (int)raw;
    switch (raw) {
    case 0x4c00: case 0x4c10: case 0x4800: g_family = FAM_8152; break;   /* 01,02,07 */
    case 0x5c00: case 0x5c10: case 0x5c20: case 0x5c30: g_family = FAM_8153A; break; /* 03-06 */
    case 0x6000: case 0x6010: case 0x6400: g_family = FAM_8153B; break;  /* 08,09,14(8153C) */
    case 0x7020: case 0x7030: case 0x7400: case 0x7410: case 0x7420:
        g_family = FAM_8156; break;                                      /* 10-13,15 */
    default: g_family = FAM_NONE; break;    /* incl. 8157/8159 v2-desc: decline */
    }
}

/* ---- MAC address -------------------------------------------------------- */
static void read_mac(void)
{
    u8 buf[8];
    /* VER_01 uses the live IDR; every other chip autoloads the factory address
       into the backup register at 0xd000. */
    u16 src = (g_version == 0x4c00) ? PLA_IDR : PLA_BACKUP;
    gen_ocp_read(src, 8, buf, MCU_TYPE_PLA);
    memcpy(g_mac, buf, 6);
    /* program it into the live filter (all chips but VER_01, harmless there) */
    ocp_wr_byte(MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
    { u8 idr[8]; memcpy(idr, g_mac, 6); idr[6]=idr[7]=0;
      set_regs(PLA_IDR, (u16)(MCU_TYPE_PLA | BYTE_EN_SIX), 8, idr); }
    ocp_wr_byte(MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORMAL);
}

/* ---- small polled waits ------------------------------------------------- */
static void wait_autoload(void)
{ int i; for (i=0;i<500;i++){ if (ocp_rd_word(MCU_TYPE_PLA,PLA_BOOT_CTRL)&AUTOLOAD_DONE) return; uno_pc64_delay_ms(20); } }
static void wait_ll_ready(void)
{ int i; for (i=0;i<1000;i++){ if (ocp_rd_byte(MCU_TYPE_PLA,PLA_OOB_CTRL)&LINK_LIST_READY) return; uno_pc64_delay_ms(1); } }

static void nic_reset(void)
{
    int i;
    ocp_wr_byte(MCU_TYPE_PLA, PLA_CR, CR_RST);
    for (i=0;i<1000;i++){ if(!(ocp_rd_byte(MCU_TYPE_PLA,PLA_CR)&CR_RST)) break; uno_pc64_delay_ms(1); }
}
static void reset_bmu(void)
{ ocp_byte_clr(MCU_TYPE_USB, USB_BMU_RESET, 0x03); ocp_byte_set(MCU_TYPE_USB, USB_BMU_RESET, 0x03); }

static void phy_power_up(void)
{ u16 b = mdio_read(MII_BMCR); if (b & BMCR_PDOWN) mdio_write(MII_BMCR, (u16)(b & ~BMCR_PDOWN)); }

/* ---- per-family init (required steps only; tuning/firmware omitted) ------ */
static void common_teredo_off(void)
{
    if (g_family == FAM_8152) ocp_word_clr(MCU_TYPE_PLA, PLA_TEREDO_CFG, 0x8001|0x00fe);
    else ocp_wr_byte(MCU_TYPE_PLA, PLA_TEREDO_CFG, 0xff);
}

static void exit_oob(void)      /* bring the MAC from OOB/WOL into NIC mode */
{
    ocp_dword_clr(MCU_TYPE_PLA, PLA_RCR, RCR_ACPT_ALL);
    ocp_word_set(MCU_TYPE_PLA, PLA_MISC_1, RXDY_GATED_EN);
    common_teredo_off();
    ocp_wr_byte(MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORMAL);
    ocp_wr_byte(MCU_TYPE_PLA, PLA_CR, 0x00);
    ocp_byte_clr(MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB);
    ocp_word_clr(MCU_TYPE_PLA, PLA_SFF_STS_7, MCU_BORW_EN);
    wait_ll_ready();
    ocp_word_set(MCU_TYPE_PLA, PLA_SFF_STS_7, RE_INIT_LL);
    wait_ll_ready();
    nic_reset();
    if (g_family >= FAM_8153B) reset_bmu();
    ocp_wr_dword(MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, 0x00080002);
    ocp_wr_word (MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, 0x00a0);
    ocp_wr_word (MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, 0x0110);
    ocp_wr_dword(MCU_TYPE_PLA, PLA_TXFIFO_CTRL,  0x01000008);
    ocp_word_clr(MCU_TYPE_PLA, PLA_CPCR, 0x0040);            /* rx VLAN strip off */
    ocp_wr_word (MCU_TYPE_PLA, PLA_RMS, 1522);
    ocp_word_set(MCU_TYPE_PLA, PLA_TCR0, TCR0_AUTO_FIFO);
    nic_reset();
}

static void chip_init(void)
{
    phy_power_up();
    switch (g_family) {
    case FAM_8152:
        ocp_word_set(MCU_TYPE_PLA, PLA_PHY_PWR, 0x0080|PFM_PWM_SWITCH);
        break;
    case FAM_8153A:
    case FAM_8153B:
        set_regs(USB_TOLERANCE, MCU_TYPE_USB|BYTE_EN_SIX, 8, "\0\0\0\0\0\0\0\0"); /* u1u2 off (8153A) */
        ocp_word_clr(MCU_TYPE_USB, USB_LPM_CONFIG, 0x0001);   /* u1u2 off (8153B) */
        wait_autoload();
        ocp_byte_clr(MCU_TYPE_USB, USB_U2P3_CTRL, 0x0001);    /* U2->P3 off */
        ocp_word_clr(MCU_TYPE_PLA, PLA_LED_FEATURE, 0x0700);
        if (g_family == FAM_8153B) {
            ocp_word_set(MCU_TYPE_PLA, PLA_MAC_PWR_CTRL2, 0x8000);
            ocp_word_clr(MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3, 0x4000);
        }
        break;
    case FAM_8156:
        ocp_byte_clr(MCU_TYPE_USB, USB_ECM_OP, 0x0001);
        ocp_wr_word (MCU_TYPE_USB, USB_SPEED_OPTION, 0);
        ocp_word_set(MCU_TYPE_USB, USB_ECM_OPTION, 0x0020);   /* BYPASS_MAC_RESET */
        ocp_word_clr(MCU_TYPE_USB, USB_LPM_CONFIG, 0x0001);
        wait_autoload();
        ocp_byte_clr(MCU_TYPE_USB, USB_U2P3_CTRL, 0x0001);
        ocp_wr_word (MCU_TYPE_USB, USB_MSC_TIMER, 0x0fff);
        ocp_wr_word (MCU_TYPE_USB, USB_U1U2_TIMER, 500);
        ocp_word_clr(MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3, 0x4000);
        break;
    }
    /* aggregation on (every family); rx-zero off */
    ocp_word_clr(MCU_TYPE_USB, USB_USB_CTRL, RX_AGG_DISABLE|RX_ZERO_EN);
    ocp_word_set(MCU_TYPE_PLA, PLA_RSTTALLY, 0x0001);         /* tally reset */
}

/* advertise everything, restart autoneg with a PHY reset */
static void set_speed_autoneg(void)
{
    u16 adv = mdio_read(MII_ADVERTISE);
    adv |= 0x0020|0x0040|0x0080|0x0100;      /* 10H/10F/100H/100F */
    adv |= 0x0400|0x0800;                     /* pause | asym-pause */
    mdio_write(MII_ADVERTISE, adv);
    if (g_family != FAM_8152) {
        u16 g = mdio_read(MII_CTRL1000);
        mdio_write(MII_CTRL1000, (u16)(g | 0x0200));   /* 1000F */
    }
    mdio_write(MII_BMCR, BMCR_RESET|BMCR_ANENABLE|BMCR_ANRESTART);
    { int i; for (i=0;i<50;i++){ if(!(mdio_read(MII_BMCR)&BMCR_RESET)) break; uno_pc64_delay_ms(20); } }
}

/* ---- enable / disable (link transitions) -------------------------------- */
static void set_rx_mode(void)
{
    /* accept my-MAC + broadcast + multicast; all-ones multicast hash */
    u8 mar[8]; memset(mar, 0xff, 8);
    set_regs(PLA_MAR, MCU_TYPE_PLA|BYTE_EN_DWORD, 8, mar);
    ocp_wr_dword(MCU_TYPE_PLA, PLA_RCR,
        (ocp_rd_dword(MCU_TYPE_PLA,PLA_RCR) & ~RCR_ACPT_ALL) | RCR_APM|RCR_AB|RCR_AM);
}

static void rx_early_config(void)
{
    /* Make the device end an aggregation transfer at a frame boundary well
       before our RXBUF fills, so a frame is never split across two bulk-INs.
       early size uses OUR buffer size, not the chip's default rx_buf_sz. */
    int mtu_sz = 1500 + 18 + 4;                          /* VLAN_ETH_HLEN + FCS */
    int early = RXBUF - (mtu_sz + 24 + 8);               /* desc(24) + align(8) */
    switch (g_family) {
    case FAM_8152: break;                                 /* no early regs */
    case FAM_8153A:
        ocp_wr_word(MCU_TYPE_USB, USB_RX_EARLY_TIMEOUT, 250000/8);
        ocp_wr_word(MCU_TYPE_USB, USB_RX_EARLY_SIZE, (u16)(early/4));
        break;
    case FAM_8153B:
        ocp_wr_word(MCU_TYPE_USB, USB_RX_EARLY_TIMEOUT, 128/8);
        ocp_wr_word(MCU_TYPE_USB, USB_RX_EXTRA_AGGR_TMR, 15000/8);
        ocp_wr_word(MCU_TYPE_USB, USB_RX_EARLY_SIZE, (u16)(early/8));
        break;
    case FAM_8156:
        ocp_wr_word(MCU_TYPE_USB, USB_RX_EARLY_TIMEOUT, 640/8);
        ocp_wr_word(MCU_TYPE_USB, USB_RX_EXTRA_AGGR_TMR, 15000/8);
        ocp_wr_word(MCU_TYPE_USB, USB_RX_EARLY_SIZE, (u16)(early/8));
        break;
    }
    ocp_wr_dword(MCU_TYPE_USB, USB_RX_BUF_TH, g_family==FAM_8152 ? 0x7a120180 :
                 g_family==FAM_8156 ? 0x00600400 : 0x00010001);
}

static void rtl_enable(void)
{
    rx_early_config();
    ocp_word_clr(MCU_TYPE_PLA, PLA_FMC, 0x0001);         /* reset packet filter */
    ocp_word_set(MCU_TYPE_PLA, PLA_FMC, 0x0001);
    ocp_byte_set(MCU_TYPE_PLA, PLA_CR, CR_RE|CR_TE);
    if (g_family >= FAM_8153B) ocp_wr_byte(MCU_TYPE_USB, USB_UPT_RXDMA_OWN, 0x03);
    ocp_word_clr(MCU_TYPE_PLA, PLA_MISC_1, RXDY_GATED_EN);   /* ungate rx */
    set_rx_mode();
    g_rx_have = g_rx_off = 0; g_rx_armed = 0;
}

static void rtl_disable(void)
{
    int i;
    ocp_dword_clr(MCU_TYPE_PLA, PLA_RCR, RCR_ACPT_ALL);
    ocp_word_set(MCU_TYPE_PLA, PLA_MISC_1, RXDY_GATED_EN);
    for (i=0;i<1000;i++){ if((ocp_rd_byte(MCU_TYPE_PLA,PLA_OOB_CTRL)&FIFO_EMPTY)==FIFO_EMPTY) break; uno_pc64_delay_ms(1); }
    for (i=0;i<1000;i++){ if(ocp_rd_word(MCU_TYPE_PLA,PLA_TCR0)&TCR0_TX_EMPTY) break; uno_pc64_delay_ms(1); }
    nic_reset();
    g_rx_armed = 0;
}

/* poll the PHY link and run enable/disable on a transition */
static int poll_link(void)
{
    int up = (ocp_rd_word(MCU_TYPE_PLA, PLA_PHYSTATUS) & LINK_STATUS) ? 1 : 0;
    if (up && !g_link_up)  { rtl_enable();  g_link_up = 1; }
    if (!up && g_link_up)  { rtl_disable(); g_link_up = 0; }
    return g_link_up;
}

/* ---- uno_nic_t: send / recv / link -------------------------------------- */
static int rtl_send(void *ctx, const void *pkt, int len)
{
    static u8 tx[8 + 1600];
    u32 o1, o2;
    (void)ctx;
    if (!g_bound || len <= 0 || len > 1514) return -1;
    if (!g_link_up) return -1;
    /* 8-byte tx_desc: opts1 = len | TX_FS | TX_LS, opts2 = 0 (little-endian) */
    o1 = (u32)len | 0x80000000u | 0x40000000u;
    o2 = 0;
    tx[0]=(u8)o1; tx[1]=(u8)(o1>>8); tx[2]=(u8)(o1>>16); tx[3]=(u8)(o1>>24);
    tx[4]=(u8)o2; tx[5]=(u8)(o2>>8); tx[6]=(u8)(o2>>16); tx[7]=(u8)(o2>>24);
    memcpy(tx + 8, pkt, len);
    return uno_usb_bulk_out(g_dev, tx, len + 8) > 0 ? len : -1;
}

static int rtl_recv(void *ctx, void *pkt, int cap)
{
    (void)ctx;
    if (!g_bound) return 0;
    poll_link();
    if (!g_link_up) return 0;

    /* pop remaining packets from the current aggregation buffer first */
    while (g_rx_off + 24 <= g_rx_have) {
        const u8 *d = g_rx + g_rx_off;
        u32 opts1 = (u32)d[0]|((u32)d[1]<<8)|((u32)d[2]<<16)|((u32)d[3]<<24);
        int plen = (int)(opts1 & 0x7fff);        /* length incl. 4-byte FCS */
        int flen, next;
        if (plen < 60) { g_rx_have = 0; break; }  /* short desc terminates */
        if (g_rx_off + 24 + plen > g_rx_have) { g_rx_have = 0; break; } /* truncated */
        flen = plen - 4;                          /* strip FCS */
        next = g_rx_off + 24 + plen;
        next = (next + 7) & ~7;                    /* 8-byte descriptor align */
        g_rx_off = next;
        if (flen <= 0) continue;
        if (flen > cap) flen = cap;
        memcpy(pkt, d + 24, flen);
        return flen;
    }

    /* buffer drained: arm a bulk-IN, then poll it non-blocking */
    if (!g_rx_armed) {
        if (uno_usb_bulk_in_arm(g_dev, g_rx, RXBUF) < 0) return 0;
        g_rx_armed = 1;
    }
    { int n = uno_usb_bulk_in_poll(g_dev);
      if (n <= 0) { if (n < 0) g_rx_armed = 0; return 0; }  /* not ready / error */
      g_rx_armed = 0;
      if (n < 60) return 0;                       /* link glitch */
      g_rx_have = n; g_rx_off = 0;
    }
    return 0;   /* next call drains the freshly filled buffer */
}

static int rtl_link(void *ctx) { (void)ctx; if (!g_bound) return 0; return poll_link(); }

/* ---- device match: RTL815x are vendor-specific; check VID + version ------ */
static int is_rtl_vid(u16 v)
{
    switch (v) {
    case 0x0bda: case 0x045e: case 0x04e8: case 0x17ef: case 0x13b1:
    case 0x0955: case 0x2357: case 0x2001: case 0x413c: case 0x0b05:
    case 0x20f4: return 1;
    }
    return 0;
}

/* pick the vendor-mode configuration (interface 0 class == 0xff) and its bulk
 * endpoints. Returns 0 and fills in/out on success. */
static int find_vendor_bulk(int *cfgval, int *in_ep, int *out_ep, int *in_mps, int *out_mps)
{
    static u8 cfg[512];
    int c;
    for (c = 1; c <= 2; c++) {
        int n, total, i, if0_vendor = 0, got_in = 0, got_out = 0;
        /* GET_DESCRIPTOR(CONFIGURATION, index=c-1) */
        n = uno_usb_control(g_dev, 0x80, 6, (u16)(0x0200 | (c-1)), 0, cfg, sizeof cfg);
        if (n < 9) continue;
        *cfgval = cfg[5];                              /* bConfigurationValue */
        total = cfg[2] | (cfg[3] << 8); if (total > (int)sizeof cfg) total = n;
        for (i = 0; i + 2 <= total; ) {
            int len = cfg[i], type = cfg[i+1];
            if (len < 2) break;
            if (type == 0x04) {                        /* INTERFACE */
                int inum = cfg[i+2], icls = cfg[i+5];
                if (inum == 0) if0_vendor = (icls == 0xff);
            } else if (type == 0x05 && if0_vendor && i + 6 <= total) {  /* ENDPOINT */
                int addr = cfg[i+2], attr = cfg[i+3];
                int mps  = cfg[i+4] | (cfg[i+5] << 8);
                if ((attr & 0x03) == 0x02) {           /* bulk */
                    if ((addr & 0x80) && !got_in)  { *in_ep=addr;  *in_mps=mps;  got_in=1; }
                    if (!(addr & 0x80) && !got_out){ *out_ep=addr; *out_mps=mps; got_out=1; }
                }
            }
            i += len;
        }
        if (if0_vendor && got_in && got_out) return 0;
    }
    return -1;
}

uno_nic_t *rtl8152_nic(void)
{
    int i, n, cfgval = 1, in_ep = 0x81, out_ep = 0x02, in_mps = 512, out_mps = 512;
    if (g_bound) return &g_nic;
    n = uno_xhci_dev_count();
    for (i = 0; i < n; i++) {
        const uno_usb_dev *d = uno_xhci_dev(i);
        if (d && is_rtl_vid(d->vendor)) { g_dev = i; g_vid = d->vendor; g_pid = d->product; g_found = 1; break; }
    }
    if (!g_found) return 0;

    if (find_vendor_bulk(&cfgval, &in_ep, &out_ep, &in_mps, &out_mps) < 0) return 0;
    if (uno_usb_set_config(g_dev, cfgval) < 0) return 0;
    if (uno_usb_setup_bulk(g_dev, in_ep, out_ep, in_mps, out_mps) < 0) return 0;

    detect_version();
    if (g_family == FAM_NONE) return 0;    /* unknown / 5G-10G v2-desc: decline */

    read_mac();
    chip_init();
    exit_oob();
    set_speed_autoneg();

    g_nic.ctx = 0; g_nic.send = rtl_send; g_nic.recv = rtl_recv; g_nic.link = rtl_link;
    g_bound = 1;
    poll_link();                            /* enable now if the link is already up */
    return &g_nic;
}

const unsigned char *rtl8152_mac(void) { return g_mac; }

void rtl8152_status(int *found, int *bound, int *link,
                    unsigned short *vid, unsigned short *pid, int *version)
{
    if (found)   *found   = g_found;
    if (bound)   *bound   = g_bound;
    if (link)    *link    = g_bound ? poll_link() : 0;
    if (vid)     *vid     = g_vid;
    if (pid)     *pid     = g_pid;
    if (version) *version = g_version;
}
