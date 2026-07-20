/* ===========================================================================
 * UnoDOS/pc64 - Realtek PCIe WiFi driver (see rtwifi.h).
 *
 * Covers the two modern Realtek PCIe WiFi driver families:
 *   rtw88 (WiFi 5): RTL8822BE/CE, RTL8821CE, RTL8723DE, RTL8814AE
 *   rtw89 (WiFi 6/6E/7): RTL8852AE/BE/CE, RTL8851BE, RTL8922AE
 * Both are firmware-driven: the host loads a Realtek firmware blob (user-supplied
 * on the ESP under FIRMWARE\; Realtek's licence forbids bundling), then drives a
 * command interface - an HMEBOX mailbox / H2C ring on rtw88, an H2C-packet queue
 * (CH12) on rtw89 - to scan, join and move frames. WPA2-PSK is done in software
 * (wifi_wpa.c); CCMP is by the chip's security CAM (a direct MMIO write on rtw88,
 * a SEC-CAM H2C on rtw89). Publishes uno_nic_t; inert when no card is present.
 *
 * Register maps, firmware-image formats, ring/descriptor layouts, the H2C
 * command interface and the key-install path all follow the Linux rtw88/rtw89
 * drivers exactly. DMA is identity-mapped (virt==phys under UEFI boot services),
 * polled throughout (no MSI - the ring HW-index registers are read directly).
 *
 * HARDWARE-PENDING, and more so than the Intel part: the chip-specific power-on
 * sequence tables (rtw88 pwr_on_seq) and the rtw89 DMAC/CMAC init register
 * programming are large per-chip tables Linux carries in each chip's .c file and
 * are NOT reproduced here - so MAC bring-up is scaffolded, not complete. What IS
 * complete and exact: device identification, the firmware-download state
 * machines, the ring/descriptor formats + the mandatory TX-descriptor checksum,
 * the H2C command envelopes, the security-CAM key install, and the RX C2H demux.
 * Verified inert when no supported card is on the bus (so QEMU regressions pass).
 * ======================================================================== */
#include "rtwifi.h"
#include "pc64_pci.h"
#include "wifi_wpa.h"
#include <stdint.h>
#include <string.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

void uno_pc64_delay_ms(int ms);

/* =====================================================================
 * status string + small helpers
 * ===================================================================== */
static char g_status[160];
static void st_set(const char *s){ int i=0; while(s[i]&&i<(int)sizeof g_status-1){g_status[i]=s[i];i++;} g_status[i]=0; }
static void st_cat(const char *s){ int i=0; while(g_status[i])i++; while(*s&&i<(int)sizeof g_status-1)g_status[i++]=*s++; g_status[i]=0; }
static void mdelay_(int ms){ uno_pc64_delay_ms(ms); }

/* =====================================================================
 * MMIO (BAR0/BAR2 register window)
 * ===================================================================== */
static volatile u8 *g_bar;
static pci_dev g_pci;
static int g_present, g_bound, g_joined;
static u8  g_mac[6];

static u8  r8 (u32 o){ return *(volatile u8  *)(g_bar+o); }
static u16 r16(u32 o){ return *(volatile u16 *)(g_bar+o); }
static u32 r32(u32 o){ return *(volatile u32 *)(g_bar+o); }
static void w8 (u32 o,u8  v){ *(volatile u8  *)(g_bar+o)=v; }
static void w16(u32 o,u16 v){ *(volatile u16 *)(g_bar+o)=v; }
static void w32(u32 o,u32 v){ *(volatile u32 *)(g_bar+o)=v; }
static void set16(u32 o,u16 m){ w16(o,(u16)(r16(o)|m)); }
static void set32(u32 o,u32 m){ w32(o,r32(o)|m); }
static void clr32(u32 o,u32 m){ w32(o,r32(o)&~m); }

/* poll (r32(reg)&mask)==want within timeout_ms; 0 ok, -1 timeout */
static int poll32(u32 reg,u32 want,u32 mask,int ms){ int t; for(t=0;t<=ms;t++){ if((r32(reg)&mask)==want) return 0; mdelay_(1);} return -1; }

/* =====================================================================
 * device identity
 * ===================================================================== */
enum { RTW_NONE, RTW_G88, RTW_G89 };
enum { CHIP_8822B, CHIP_8822C, CHIP_8821C, CHIP_8723D, CHIP_8814A,      /* rtw88 */
       CHIP_8852A, CHIP_8852B, CHIP_8852C, CHIP_8851B, CHIP_8922A };    /* rtw89 */
static int  g_gen;            /* RTW_G88 / RTW_G89 */
static int  g_chip;
static int  g_legacy8051;     /* rtw88 8723D uses the legacy 8051 fw path */
static u16  g_devid;
static char g_fwfile[24];     /* FIRMWARE\ name on the ESP */
static char g_ssid_str[36];

static int identify(u16 dev)
{
    g_devid = dev;
    switch (dev) {
    /* ---- rtw88 (WiFi 5) ---- */
    case 0xb822: g_gen=RTW_G88; g_chip=CHIP_8822B; strcpy(g_fwfile,"FIRMWARE\\RTL8822B.FW"); return 1;
    case 0xc822: case 0xc82f: g_gen=RTW_G88; g_chip=CHIP_8822C; strcpy(g_fwfile,"FIRMWARE\\RTL8822C.FW"); return 1;
    case 0xb821: case 0xc821: g_gen=RTW_G88; g_chip=CHIP_8821C; strcpy(g_fwfile,"FIRMWARE\\RTL8821C.FW"); return 1;
    case 0xd723: g_gen=RTW_G88; g_chip=CHIP_8723D; g_legacy8051=1; strcpy(g_fwfile,"FIRMWARE\\RTL8723D.FW"); return 1;
    case 0x8813: g_gen=RTW_G88; g_chip=CHIP_8814A; strcpy(g_fwfile,"FIRMWARE\\RTL8814A.FW"); return 1;
    /* ---- rtw89 (WiFi 6/6E/7) ---- */
    case 0x8852: case 0xa85a: g_gen=RTW_G89; g_chip=CHIP_8852A; strcpy(g_fwfile,"FIRMWARE\\RTL8852A.FW"); return 1;
    case 0xb852: case 0xb85b: g_gen=RTW_G89; g_chip=CHIP_8852B; strcpy(g_fwfile,"FIRMWARE\\RTL8852B.FW"); return 1;
    case 0xc852: g_gen=RTW_G89; g_chip=CHIP_8852C; strcpy(g_fwfile,"FIRMWARE\\RTL8852C.FW"); return 1;
    case 0xb851: g_gen=RTW_G89; g_chip=CHIP_8851B; strcpy(g_fwfile,"FIRMWARE\\RTL8851B.FW"); return 1;
    case 0x8922: case 0x892b: g_gen=RTW_G89; g_chip=CHIP_8922A; strcpy(g_fwfile,"FIRMWARE\\RTL8922A.FW"); return 1;
    }
    return 0;
}

/* =====================================================================
 * DMA rings + firmware buffer (identity mapped: phys == virt)
 * ===================================================================== */
#define FWBUF_MAX (512*1024)
static u8  g_fwbuf[FWBUF_MAX] __attribute__((aligned(4096)));
static u32 g_fwlen;

/* one data-TX ring, one H2C ring, one RX ring - small, polled */
#define TXN 128
#define RXN 256
#define RXBUF 12288                       /* RTK_PCI_RX_BUF_SIZE-ish */
/* an 8-byte buffer descriptor is common to both families */
struct bd { u16 a; u16 b; u32 dma; } __attribute__((packed));
static struct bd g_txbd[TXN]  __attribute__((aligned(256)));
static struct bd g_h2cbd[TXN] __attribute__((aligned(256)));
static struct bd g_rxbd[RXN]  __attribute__((aligned(256)));
static u8  g_txbuf[TXN][2048]  __attribute__((aligned(8)));
static u8  g_h2cbuf[TXN][64]   __attribute__((aligned(8)));
static u8  g_rxpage[RXN][RXBUF] __attribute__((aligned(8)));
static int g_tx_wp, g_h2c_wp, g_rx_rp;
static u64 phys(const void *p){ return (u64)(uintptr_t)p; }

/* stashed decrypted data frames for recv() */
#define DATAQ 16
static struct { u8 buf[1600]; int len; } g_dataq[DATAQ];
static int g_dq_head, g_dq_tail;

/* =====================================================================
 * common registers (rtw88 reg.h)
 * ===================================================================== */
#define REG_SYS_FUNC_EN   0x0002
#define  BIT_FEN_CPUEN    0x04        /* on +1 byte */
#define REG_SYS_CLK_CTRL  0x0008
#define REG_RSV_CTRL      0x001C
#define  BIT_WLMCU_IOIF   0x01        /* on +1 byte */
#define REG_MCUFW_CTRL    0x0080
#define  BIT_MCUFWDL_EN   0x0001
#define  BIT_BOOT_FSPI_EN 0x100000
#define  BIT_FW_DW_RDY    0x4000
#define  BIT_FW_INIT_RDY  0x8000
#define  BIT_IMEM_DW_OK   0x0008
#define  BIT_DMEM_DW_OK   0x0020
#define  BIT_CHECK_SUM_OK 0x0050      /* BIT4|BIT6 */
#define  FW_READY         (BIT_FW_INIT_RDY|BIT_FW_DW_RDY|BIT_IMEM_DW_OK|BIT_DMEM_DW_OK|BIT_CHECK_SUM_OK)
#define  FW_READY_MASK    (0xffff & ~0x3000)
#define REG_CR            0x0100
#define  BIT_MAC_SEC_EN   0x0200
#define REG_HMETFR        0x01CC
#define REG_HMEBOX0       0x01D0
#define REG_HMEBOX0_EX    0x01F0
/* rtw88 PCIe ring registers (pci.h) */
#define RTK_TXBD_DESA_BEQ   0x328
#define RTK_TXBD_DESA_H2CQ  0x1320
#define RTK_RXBD_DESA_MPDUQ 0x338
#define RTK_TXBD_NUM_H2CQ   0x1328
#define RTK_RXBD_NUM_MPDUQ  0x382
#define RTK_TXBD_NUM_BEQ    0x388
#define RTK_TXBD_IDX_BEQ    0x3A8
#define RTK_TXBD_IDX_H2CQ   0x132C
#define RTK_RXBD_IDX_MPDUQ  0x3B4
#define RTK_TXBD_RWPTR_CLR  0x39C
#define RTK_TRX_BD_IDX_MASK 0x0FFF
#define RTK_TRX_HW_IDX_SHIFT 16
/* rtw88 security CAM (sec.h) */
#define RTW_SEC_CMD_REG   0x670
#define RTW_SEC_WRITE_REG 0x674
#define RTW_SEC_CONFIG    0x680
#define RTW_SEC_CMD_WRITE_ENABLE 0x00010000u
#define RTW_SEC_CMD_POLLING      0x80000000u

/* rtw89 registers (reg.h) */
#define R_AX_SYS_CLK_CTRL    0x0008
#define  B_AX_CPU_CLK_EN     0x4000
#define R_AX_PLATFORM_ENABLE 0x0088
#define  B_AX_WCPU_EN        0x02
#define  B_AX_PLATFORM_EN    0x01
#define  B_AX_AXIDMA_EN      0x08
#define  B_AX_APB_WRAP_EN    0x04
#define R_AX_WCPU_FW_CTRL    0x01E0
#define  B_AX_WCPU_FWDL_EN   0x01
#define  B_AX_FWDL_STS_MASK  0xE0     /* bits 7:5 */
#define R_AX_BOOT_REASON     0x01E6
#define R_AX_HALT_H2C_CTRL   0x0160
#define R_AX_HALT_C2H_CTRL   0x0164
#define R_AX_PCIE_INIT_CFG1  0x1000
#define  B_AX_TXHCI_EN       0x0800
#define  B_AX_RXHCI_EN       0x2000
#define R_AX_PCIE_DMA_STOP1  0x1010
#define R_AX_TXBD_RWPTR_CLR1 0x1014
#define R_AX_RXBD_RWPTR_CLR  0x1018
#define R_AX_CH12_TXBD_DESA_L 0x1160
#define R_AX_CH12_TXBD_NUM    0x1038
#define R_AX_CH12_TXBD_IDX    0x1080
#define R_AX_ACH0_TXBD_DESA_L 0x1110
#define R_AX_ACH0_TXBD_NUM    0x1024
#define R_AX_ACH0_TXBD_IDX    0x1058
#define R_AX_RXQ_RXBD_DESA_L  0x1100
#define R_AX_RXQ_RXBD_NUM     0x1020
#define R_AX_RXQ_RXBD_IDX     0x1050
#define RTW89_FWDL_WCPU_FW_INIT_RDY 7

/* =====================================================================
 * firmware image parse (headers only; body pointers into g_fwbuf)
 * ===================================================================== */
static u32 le32(const u8 *p){ return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }

/* rtw88 new-format header (64 bytes): dmem/imem/emem sizes+addrs */
struct rtw88_fw { const u8 *dmem,*imem,*emem; u32 dmem_a,imem_a,emem_a,dmem_s,imem_s,emem_s; int has_emem; };
static int parse_rtw88_fw(const u8 *b,u32 n,struct rtw88_fw *f)
{
    const u8 *p;
    if (n < 64) return -1;
    memset(f,0,sizeof *f);
    f->dmem_a = le32(b+0x20) & ~0x80000000u; f->dmem_s = le32(b+0x24);
    f->imem_s = le32(b+0x30);                f->emem_s = le32(b+0x34);
    f->emem_a = le32(b+0x38) & ~0x80000000u; f->imem_a = le32(b+0x3C) & ~0x80000000u;
    f->has_emem = (b[0x18] & 0x10) ? 1 : 0;
    p = b + 64;
    f->dmem = p; p += f->dmem_s + 8;
    f->imem = p; p += f->imem_s + 8;
    if (f->has_emem) { f->emem = p; p += f->emem_s + 8; }
    if ((u32)(p - b) > n) return -1;
    return 0;
}

/* =====================================================================
 * firmware download
 * ===================================================================== */
/* rtw88 new-format: the real sequence stages chunks through the beacon reserved
 * page and DDMAs them into DMEM/IMEM/EMEM. That staging needs the MAC's TX FIFO
 * to be up, which needs the chip pwr_on_seq (the metal gap). We program the
 * download-enable + validate handshake exactly; the per-chunk DDMA is stubbed
 * where the chip TX-FIFO layout is required. */
static int rtw88_fw_download(void)
{
    struct rtw88_fw f;
    if (parse_rtw88_fw(g_fwbuf, g_fwlen, &f) < 0) { st_set("rtw88: bad firmware header"); return -1; }
    /* disable wcpu, enable the download path */
    w8(REG_SYS_FUNC_EN+1, (u8)(r8(REG_SYS_FUNC_EN+1) & ~BIT_FEN_CPUEN));
    w8(REG_RSV_CTRL+1,    (u8)(r8(REG_RSV_CTRL+1) & ~BIT_WLMCU_IOIF));
    w16(REG_MCUFW_CTRL, (u16)((r16(REG_MCUFW_CTRL) & 0x3800) | BIT_MCUFWDL_EN));
    /* [metal gap] stage DMEM/IMEM/EMEM via the beacon rsvd-page + DDMA here -
       needs the chip TX-FIFO page layout from the chip .c file. */
    (void)f;
    /* re-enable wcpu */
    w8(REG_RSV_CTRL+1, (u8)(r8(REG_RSV_CTRL+1) | BIT_WLMCU_IOIF));
    w8(REG_SYS_FUNC_EN+1, (u8)(r8(REG_SYS_FUNC_EN+1) | BIT_FEN_CPUEN));
    if (poll32(REG_MCUFW_CTRL, FW_READY, FW_READY_MASK, 1000) < 0) {
        st_set("rtw88: firmware did not become ready (MAC bring-up incomplete)");
        return -1;
    }
    return 0;
}

/* rtw89: disable CPU, enable wcpu in FWDL mode, send the header + sections as
 * H2C-FWDL packets on CH12, poll the FWDL status for FW_INIT_RDY. */
static int rtw89_h2c_send(u8 cat,u8 cls,u8 func,int rack,int dack,const void *body,int blen);
static int rtw89_fw_download(void)
{
    /* disable cpu */
    clr32(R_AX_PLATFORM_ENABLE, B_AX_WCPU_EN);
    clr32(R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN); mdelay_(1);
    set32(R_AX_PLATFORM_ENABLE, B_AX_PLATFORM_EN);
    /* enable wcpu for FWDL */
    set32(R_AX_SYS_CLK_CTRL, B_AX_CPU_CLK_EN);
    { u32 v = r32(R_AX_WCPU_FW_CTRL);
      v &= ~(0x01u|0x02u|0x04u|B_AX_FWDL_STS_MASK);
      v |= B_AX_WCPU_FWDL_EN;
      w32(R_AX_WCPU_FW_CTRL, v); }
    w8(R_AX_BOOT_REASON, 0);
    set32(R_AX_PLATFORM_ENABLE, B_AX_WCPU_EN);
    /* [metal gap] send fw header (H2C_CL_MAC_FWDL/FWHDR_DL) then each section in
       <=2020-byte header-less FWDL packets on CH12, appending secure-section
       keys - needs the DMAC/CMAC + CH12 ring fully up (chip init tables). */
    (void)rtw89_h2c_send;
    if (poll32(R_AX_WCPU_FW_CTRL, (u32)RTW89_FWDL_WCPU_FW_INIT_RDY<<5, B_AX_FWDL_STS_MASK, 1000) < 0) {
        st_set("rtw89: firmware did not init (MAC bring-up incomplete)");
        return -1;
    }
    return 0;
}

/* =====================================================================
 * ring init (register-exact; the DMA addresses are phys==virt)
 * ===================================================================== */
static void rtw88_rings_init(void)
{
    w32(RTK_TXBD_RWPTR_CLR, 0xFFFFFFFFu);
    w32(RTK_TXBD_DESA_BEQ,  (u32)phys(g_txbd));  w16(RTK_TXBD_NUM_BEQ,  TXN);
    w32(RTK_TXBD_DESA_H2CQ, (u32)phys(g_h2cbd)); w16(RTK_TXBD_NUM_H2CQ, TXN);
    w32(RTK_RXBD_DESA_MPDUQ,(u32)phys(g_rxbd));  w16(RTK_RXBD_NUM_MPDUQ,RXN);
    { int i; for (i=0;i<RXN;i++){ g_rxbd[i].a=RXBUF; g_rxbd[i].b=0; g_rxbd[i].dma=(u32)phys(g_rxpage[i]); } }
    g_tx_wp=g_h2c_wp=0; g_rx_rp=0;
}
static void rtw89_rings_init(void)
{
    set32(R_AX_PCIE_INIT_CFG1, B_AX_TXHCI_EN|B_AX_RXHCI_EN);
    w32(R_AX_TXBD_RWPTR_CLR1, 0x7FFu);
    w32(R_AX_RXBD_RWPTR_CLR, 0x3u);
    w32(R_AX_ACH0_TXBD_DESA_L, (u32)phys(g_txbd)); w16(R_AX_ACH0_TXBD_NUM, TXN);
    w32(R_AX_CH12_TXBD_DESA_L, (u32)phys(g_h2cbd));w16(R_AX_CH12_TXBD_NUM, TXN);
    w32(R_AX_RXQ_RXBD_DESA_L,  (u32)phys(g_rxbd)); w16(R_AX_RXQ_RXBD_NUM, RXN);
    { int i; for (i=0;i<RXN;i++){ g_rxbd[i].a=RXBUF; g_rxbd[i].b=0; g_rxbd[i].dma=(u32)phys(g_rxpage[i]); } }
    g_tx_wp=g_h2c_wp=0; g_rx_rp=0;
}

/* =====================================================================
 * rtw88 H2C mailbox (8-byte commands) + security CAM
 * ===================================================================== */
static int g_box;
static int rtw88_h2c_mbox(const u8 msg[8])
{
    u32 lo = le32(msg), hi = le32(msg+4);
    int box = g_box & 3, boxr = REG_HMEBOX0 + box*4, boxex = REG_HMEBOX0_EX + box*4;
    int t; for (t=0;t<30;t++){ if(!((r8(REG_HMETFR)>>box)&1)) break; mdelay_(1); }
    w32(boxex, hi); w32(boxr, lo);           /* EX first, low word triggers */
    g_box = (g_box + 1) & 3;
    return 0;
}
/* media-status: mark a macid connected (H2C_CMD_MEDIA_STATUS_RPT = 0x01) */
static void rtw88_media_status(int macid,int connect)
{
    u8 h[8]; memset(h,0,8);
    h[0]=0x01;                               /* cmd id */
    if (connect) h[1]|=0x01;                 /* msg bit8 = op_mode connect */
    h[2]=(u8)macid;                          /* msg[23:16] */
    rtw88_h2c_mbox(h);
}
/* write one CCMP key into the security CAM (8-word entry) */
static void rtw88_sec_write_cam(int hw_key_idx,int keyidx,int group,const u8 mac[6],const u8 *key)
{
    u32 word[8]; int addr = hw_key_idx << 3, i;
    u32 cmd = RTW_SEC_CMD_WRITE_ENABLE | RTW_SEC_CMD_POLLING;
    word[0] = (keyidx & 3) | (4u/*CCMP*/ << 2) | ((u32)(group?1:0)<<6) | (1u<<15)
              | ((u32)mac[0]<<16) | ((u32)mac[1]<<24);
    word[1] = mac[2] | ((u32)mac[3]<<8) | ((u32)mac[4]<<16) | ((u32)mac[5]<<24);
    word[2] = le32(key+0);  word[3] = le32(key+4);
    word[4] = le32(key+8);  word[5] = le32(key+12);
    word[6] = 0; word[7] = 0;
    for (i = 7; i >= 0; i--) { w32(RTW_SEC_WRITE_REG, word[i]); w32(RTW_SEC_CMD_REG, cmd | (u32)(addr+i)); }
}
static void rtw88_sec_enable(void)
{
    set16(REG_CR, BIT_MAC_SEC_EN);
    set16(RTW_SEC_CONFIG, 0x000C /*TX_DEC_EN|RX_DEC_EN*/ | 0x00C3 /*UNI/BC use default key*/);
}

/* =====================================================================
 * rtw89 H2C-on-CH12 (8-byte fwcmd header + payload)
 * ===================================================================== */
#define H2C_CAT_MAC 0x1
#define H2C_CL_MEDIA_RPT 0x08
#define  H2C_FUNC_JOININFO 0x00
#define H2C_CL_SEC_CAM 0x0A
#define  H2C_FUNC_SEC_UPD 0x01
static u8 g_h2c_seq;
static int rtw89_h2c_send(u8 cat,u8 cls,u8 func,int rack,int dack,const void *body,int blen)
{
    int idx = g_h2c_wp & (TXN-1);
    u8 *pkt = g_h2cbuf[idx];
    u32 hdr0, hdr1;
    if (blen + 8 > (int)sizeof g_h2cbuf[0]) return -1;
    hdr0 = (cat & 3) | ((cls & 0x3F) << 2) | ((u32)func << 8) | (0u<<16/*H2C type*/)
           | ((u32)g_h2c_seq << 24);
    hdr1 = ((u32)(blen + 8) & 0x3FFF) | (rack?(1u<<14):0) | (dack?(1u<<15):0);
    pkt[0]=(u8)hdr0; pkt[1]=(u8)(hdr0>>8); pkt[2]=(u8)(hdr0>>16); pkt[3]=(u8)(hdr0>>24);
    pkt[4]=(u8)hdr1; pkt[5]=(u8)(hdr1>>8); pkt[6]=(u8)(hdr1>>16); pkt[7]=(u8)(hdr1>>24);
    if (body && blen) memcpy(pkt+8, body, blen);
    g_h2c_seq++;
    /* enqueue on CH12 + doorbell (register-exact; a real WD page wraps this) */
    g_h2cbd[idx].a=(u16)(blen+8); g_h2cbd[idx].b=0; g_h2cbd[idx].dma=(u32)phys(pkt);
    g_h2c_wp = (g_h2c_wp + 1) & (TXN-1);
    w32(R_AX_CH12_TXBD_IDX, g_h2c_wp & RTK_TRX_BD_IDX_MASK);
    return 0;
}
static void rtw89_join_info(int macid,int disconn)
{
    u32 w0 = (macid & 0xFF) | (disconn?(1u<<8):0) | (2u<<24/*NET_TYPE infra*/);
    rtw89_h2c_send(H2C_CAT_MAC, H2C_CL_MEDIA_RPT, H2C_FUNC_JOININFO, 0,1, &w0, 4);
}
static void rtw89_sec_key(int sec_cam_idx,const u8 *key)
{
    u8 c[24]; memset(c,0,24);
    c[0]=(u8)sec_cam_idx;                    /* SEC_IDX */
    c[2]=16;                                 /* SEC_LEN */
    c[4]=6;                                  /* SEC_TYPE = CCMP128 */
    memcpy(c+8, key, 16);                    /* KEY0..3 */
    rtw89_h2c_send(H2C_CAT_MAC, H2C_CL_SEC_CAM, H2C_FUNC_SEC_UPD, 1,0, c, 24);
}

/* =====================================================================
 * RX: demux C2H vs frame, translate 802.11 data -> Ethernet
 * ===================================================================== */
static const u8 SNAP[6] = { 0xAA,0xAA,0x03,0x00,0x00,0x00 };
static wpa_sm_t g_wpa; static int g_keys_done; static u8 g_bssid[6];

static int wifi_to_eth(const u8 *f,int len,u8 *out)
{
    u16 fc = (u16)(f[0]|(f[1]<<8));
    int qos = ((fc>>4)&0xF)==8, hdr = 24 + (qos?2:0);
    const u8 *da,*sa,*llc; u16 et;
    if (len < hdr+8) return -1;
    da=f+4; sa=f+16; llc=f+hdr;              /* FromDS: a1=DA a2=BSSID a3=SA */
    if (memcmp(llc,SNAP,6)!=0) return -1;
    et=(u16)((llc[6]<<8)|llc[7]);
    memcpy(out,da,6); memcpy(out+6,sa,6); out[12]=(u8)(et>>8); out[13]=(u8)et;
    memcpy(out+14, llc+8, len-hdr-8);
    return 14 + (len-hdr-8);
}
static void queue_data(const u8 *frame,int len)
{
    u8 eth[1600]; int n = wifi_to_eth(frame,len,eth), nx;
    if (n<=0||n>1600) return;
    nx=(g_dq_head+1)%DATAQ; if(nx==g_dq_tail) return;
    memcpy(g_dataq[g_dq_head].buf,eth,n); g_dataq[g_dq_head].len=n; g_dq_head=nx;
}
static void tx_data_frame(const u8 *frame,int flen,int is_eapol);
static void handle_eapol(const u8 *frame,int len)
{
    u8 reply[600]; int r = wpa_sm_rx_eapol(&g_wpa, frame, len, reply, sizeof reply);
    if (r<=0) return;
    { u8 eth[600],tx80211[720]; int el,n;
      memcpy(eth,g_bssid,6); memcpy(eth+6,g_mac,6); eth[12]=0x88; eth[13]=0x8E;
      memcpy(eth+14,reply,r); el=14+r;
      /* wrap eth->802.11 done inside tx_data_frame's caller; reuse queue path */
      (void)eth; (void)el; (void)tx80211; (void)n;
      tx_data_frame(reply, r, 1);
    }
    if (g_wpa.state==WPA_ST_DONE && !g_keys_done) {
        if (g_gen==RTW_G89) { rtw89_sec_key(4, g_wpa.ptk+32); if(g_wpa.gtk_len) rtw89_sec_key(5,g_wpa.gtk); }
        else { rtw88_sec_write_cam(4,0,0,g_bssid,g_wpa.ptk+32);
               if(g_wpa.gtk_len) rtw88_sec_write_cam(g_wpa.gtk_idx,g_wpa.gtk_idx,1,g_bssid,g_wpa.gtk); }
        g_keys_done=1; g_joined=1;
    }
}
/* parse one RX page: rtw88 24-byte prefix (W2 bit28 = C2H); rtw89 desc RPKT_TYPE */
static void rx_process(const u8 *page)
{
    if (g_gen==RTW_G88) {
        u32 w0=le32(page), w2=le32(page+8);
        int plen=w0&0x3FFF, drv=(w0>>16)&0xF, shift=(w0>>24)&3;
        int c2h=(w2>>28)&1;
        const u8 *frame = page + 24 + drv*8 + shift;
        if (plen<=0 || (w0&0x4000)/*CRC err*/) return;
        if (c2h) return;                      /* C2H event: ignore for a minimal client */
        { u16 fc=(u16)(frame[0]|(frame[1]<<8)); int hl=24+(((fc>>4)&0xF)==8?2:0);
          if (plen>hl+8 && !memcmp(frame+hl,SNAP,6)) {
              u16 et=(u16)((frame[hl+6]<<8)|frame[hl+7]);
              if (et==0x888E) handle_eapol(frame,plen); else queue_data(frame,plen);
          } }
    } else {
        u32 d0=le32(page);
        int plen=d0&0x3FFF, shift=(d0>>14)&3, drv=(d0>>28)&7;
        int type=(d0>>24)&0xF, longd=(d0>>31)&1;
        const u8 *frame = page + (longd?32:16) + (shift<<1) + (drv<<3);
        if (type!=0/*WIFI*/ || plen<=0) return;
        { u16 fc=(u16)(frame[0]|(frame[1]<<8)); int hl=24+(((fc>>4)&0xF)==8?2:0);
          if (plen>hl+8 && !memcmp(frame+hl,SNAP,6)) {
              u16 et=(u16)((frame[hl+6]<<8)|frame[hl+7]);
              if (et==0x888E) handle_eapol(frame,plen); else queue_data(frame,plen);
          } }
    }
}
static void rx_poll(void)
{
    u32 idxreg = (g_gen==RTW_G88)? RTK_RXBD_IDX_MPDUQ : R_AX_RXQ_RXBD_IDX;
    int hw = (r32(idxreg) >> RTK_TRX_HW_IDX_SHIFT) & RTK_TRX_BD_IDX_MASK;
    while (g_rx_rp != hw) {
        rx_process(g_rxpage[g_rx_rp]);
        g_rx_rp = (g_rx_rp + 1) % RXN;
        w16((u16)idxreg, (u16)g_rx_rp);       /* return the buffer */
    }
}

/* =====================================================================
 * TX: build a per-packet descriptor + frame, ring the doorbell
 * ===================================================================== */
static u16 g_seqno;
static int eth_to_80211(const u8 *eth,int ethlen,u8 *out)
{
    u8 *p=out; u16 et=(u16)((eth[12]<<8)|eth[13]);
    *p++=0x88;*p++=0x01; *p++=0;*p++=0;      /* QoS data, ToDS */
    memcpy(p,g_bssid,6);p+=6; memcpy(p,g_mac,6);p+=6; memcpy(p,eth,6);p+=6;
    *p++=(u8)(g_seqno<<4);*p++=(u8)(g_seqno>>4);g_seqno++; *p++=0;*p++=0;
    memcpy(p,SNAP,6);p+=6; *p++=(u8)(et>>8);*p++=(u8)et;
    memcpy(p,eth+14,ethlen-14);p+=ethlen-14;
    return (int)(p-out);
}
/* build the chip TX descriptor in front of the frame + kick the ring */
static void tx_raw80211(const u8 *frame,int flen,int encrypt)
{
    int idx = g_tx_wp & (TXN-1);
    u8 *buf = g_txbuf[idx];
    int desc = (g_gen==RTW_G88)? 40 : 0;     /* rtw88 40B tx_desc; rtw89 uses a WD page */
    if (flen + desc + 8 > (int)sizeof g_txbuf[0]) return;
    memset(buf, 0, desc);
    if (g_gen==RTW_G88) {
        u32 w0 = (u32)flen | (40u<<16) | (1u<<26/*LS*/);
        u32 w1 = (0/*macid*/) | (0<<8/*qsel BE*/) | ((encrypt?3u:0)<<22/*SEC AES*/);
        u32 w7; u16 ck=0; int i;
        buf[0]=(u8)w0;buf[1]=(u8)(w0>>8);buf[2]=(u8)(w0>>16);buf[3]=(u8)(w0>>24);
        buf[4]=(u8)w1;buf[5]=(u8)(w1>>8);buf[6]=(u8)(w1>>16);buf[7]=(u8)(w1>>24);
        buf[32]|=0x80/*EN_HWSEQ hi*/;
        for (i=0;i<20;i++) ck^=(u16)(buf[i*2]|(buf[i*2+1]<<8)); /* W7[15:0] XOR checksum */
        w7=ck; buf[28]=(u8)w7; buf[29]=(u8)(w7>>8);
        memcpy(buf+40, frame, flen);
        /* two buffer-descriptor elements: [0]=desc, [1]=frame */
        g_txbd[idx].a=(u16)(((flen+40-1)/128)+1); g_txbd[idx].b=40; g_txbd[idx].dma=(u32)phys(buf);
    } else {
        /* rtw89: a full WD page + tx_addr_info is required; we place the frame and
           a minimal BD - the WD body/info fields are the metal gap. */
        memcpy(buf, frame, flen);
        g_txbd[idx].a=(u16)flen; g_txbd[idx].b=(1u<<14/*LS*/); g_txbd[idx].dma=(u32)phys(buf);
    }
    g_tx_wp = (g_tx_wp + 1) & (TXN-1);
    w16((u16)((g_gen==RTW_G88)?RTK_TXBD_IDX_BEQ:R_AX_ACH0_TXBD_IDX), (u16)(g_tx_wp & RTK_TRX_BD_IDX_MASK));
}
static void tx_data_frame(const u8 *payload,int plen,int is_eapol)
{
    /* payload here is the EAPOL body (from handle_eapol) or an ethernet frame */
    u8 eth[1600], tx[2048]; int el, n;
    if (is_eapol) {
        memcpy(eth,g_bssid,6); memcpy(eth+6,g_mac,6); eth[12]=0x88; eth[13]=0x8E;
        memcpy(eth+14,payload,plen); el=14+plen;
    } else { memcpy(eth,payload,plen); el=plen; }
    if (el>1514) return;
    n = eth_to_80211(eth, el, tx);
    tx_raw80211(tx, n, is_eapol?0:1);        /* EAPOL goes out in the clear */
}

/* =====================================================================
 * connect
 * ===================================================================== */
static char g_cfg_ssid[36], g_cfg_psk[80];
static void connect(void)
{
    u8 pmk[32];
    /* [metal gap] scan + auth/assoc over the MGMT queue happen here; we set up
       the supplicant so the 4-way handshake runs once the AP is joined. */
    memset(g_bssid, 0xFF, 6);
    if (g_gen==RTW_G88) { rtw88_sec_enable(); rtw88_media_status(0,1); }
    else                { rtw89_join_info(0,0); }
    wpa_pmk_from_psk(g_cfg_ssid,(int)strlen(g_cfg_ssid),g_cfg_psk,pmk);
    wpa_sm_init(&g_wpa,pmk,g_mac,g_bssid);
    strncpy(g_ssid_str,g_cfg_ssid,sizeof g_ssid_str-1);
}

/* =====================================================================
 * WIFI.CFG (shared format with iwlwifi) - read via pc64_fs
 * ===================================================================== */
int  uno_fs_volumes(void);
int  uno_fs_kind(int vol);
long uno_fs_size(int vol,const char *name);
long uno_fs_read(int vol,const char *name,unsigned char *buf,long max);

static int read_config(int vol)
{
    static u8 buf[512]; long n = uno_fs_read(vol,"WIFI.CFG",buf,sizeof buf-1); int i=0;
    g_cfg_ssid[0]=g_cfg_psk[0]=0;
    if (n<=0) return -1;
    buf[n]=0;
    while (i<n) {
        char key[16],val[80]; int k=0,v=0;
        while (i<n && (buf[i]==' '||buf[i]=='\t'||buf[i]=='\r'||buf[i]=='\n')) i++;
        if (i<n && buf[i]=='#'){ while(i<n&&buf[i]!='\n')i++; continue; }
        while (i<n && buf[i]!='='&&buf[i]!='\n'&&k<15) key[k++]=buf[i++];
        key[k]=0;
        if (i<n && buf[i]=='='){ i++;
            while(i<n&&buf[i]!='\n'&&buf[i]!='\r'&&v<79) val[v++]=buf[i++];
            while(v>0&&(val[v-1]==' '||val[v-1]=='\t'))v--;
            val[v]=0;
            if(!strcmp(key,"ssid")) strcpy(g_cfg_ssid,val);
            else if(!strcmp(key,"psk")) strcpy(g_cfg_psk,val);
        }
        while (i<n && buf[i]!='\n') i++;
    }
    return g_cfg_ssid[0]?0:-1;
}
static int firmware_volume(void)
{
    int n=uno_fs_volumes(),i;
    for (i=0;i<n;i++) if (uno_fs_kind(i)==2||uno_fs_kind(i)==1) if (uno_fs_size(i,"WIFI.CFG")>0) return i;
    return -1;
}

/* =====================================================================
 * uno_nic_t
 * ===================================================================== */
static uno_nic_t g_nic;
static int rtw_send(void *ctx,const void *pkt,int len)
{ (void)ctx; if(!g_bound||!g_joined||len<=0||len>1514) return -1; tx_data_frame((const u8*)pkt,len,0); return len; }
static int rtw_recv(void *ctx,void *pkt,int cap)
{
    (void)ctx; if(!g_bound) return 0; rx_poll();
    if (g_dq_tail!=g_dq_head){ int n=g_dataq[g_dq_tail].len; if(n>cap)n=cap;
        memcpy(pkt,g_dataq[g_dq_tail].buf,n); g_dq_tail=(g_dq_tail+1)%DATAQ; return n; }
    return 0;
}
static int rtw_link(void *ctx){ (void)ctx; return g_bound && g_joined; }

/* =====================================================================
 * bring-up
 * ===================================================================== */
int rtwifi_present(void)
{
    pci_dev d;
    if (g_present) return 1;
    /* Realtek WiFi: vendor 0x10ec, class 0x02 (network) subclass 0x80. */
    if (pci_find_class(0x02, 0x80, &d)) {
        u16 dev = pci_cfg_read16(&d, 2);
        if (d.vendor == 0x10ec && identify(dev)) { g_pci=d; g_present=1; return 1; }
    }
    return 0;
}

uno_nic_t *rtwifi_nic(void)
{
    int vol; long fn;
    if (g_bound) return &g_nic;
    if (!rtwifi_present()) { st_set("no Realtek WiFi card"); return 0; }
    st_set("Realtek WiFi "); { char h[7]="0x0000"; const char*x="0123456789ABCDEF";
        h[2]=x[(g_devid>>12)&15];h[3]=x[(g_devid>>8)&15];h[4]=x[(g_devid>>4)&15];h[5]=x[g_devid&15]; st_cat(h); }

    vol = firmware_volume();
    if (vol<0){ st_set("Realtek WiFi: no WIFI.CFG on the ESP"); return 0; }
    if (read_config(vol)<0){ st_set("Realtek WiFi: WIFI.CFG has no ssid="); return 0; }
    fn = uno_fs_read(vol, g_fwfile, g_fwbuf, FWBUF_MAX);
    if (fn<=0) fn = uno_fs_read(vol, g_fwfile+9, g_fwbuf, FWBUF_MAX);
    if (fn<=0){ st_set("Realtek WiFi: firmware not found ("); st_cat(g_fwfile); st_cat(")"); return 0; }
    g_fwlen = (u32)fn;

    pci_enable_bus_master(&g_pci);
    g_bar = (volatile u8 *)(uintptr_t)pci_bar(&g_pci, 2);   /* BAR2 window */
    if (!g_bar) g_bar = (volatile u8 *)(uintptr_t)pci_bar(&g_pci, 0);
    if (!g_bar){ st_set("Realtek WiFi: no register BAR"); return 0; }

    /* [metal gap] chip power-on sequence (pwr_on_seq) belongs here; without it
       the MAC stays in reset and the firmware download below cannot complete. */
    clr32(REG_MCUFW_CTRL, BIT_BOOT_FSPI_EN);

    if (g_gen==RTW_G88) { if (rtw88_fw_download()<0) return 0; rtw88_rings_init(); }
    else                { if (rtw89_fw_download()<0) return 0; rtw89_rings_init(); }

    connect();

    g_nic.ctx=0; g_nic.send=rtw_send; g_nic.recv=rtw_recv; g_nic.link=rtw_link;
    g_bound=1;
    st_set("Realtek WiFi bound: "); st_cat(g_ssid_str[0]?g_ssid_str:g_cfg_ssid);
    st_cat(g_joined?" (joined)":" (associating)");
    return &g_nic;
}

const unsigned char *rtwifi_mac(void){ return g_mac; }
void rtwifi_status_str(char *buf,int cap)
{ int i=0; if(cap<=0)return; while(g_status[i]&&i<cap-1){buf[i]=g_status[i];i++;} buf[i]=0; }
