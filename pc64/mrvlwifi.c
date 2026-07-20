/* ===========================================================================
 * UnoDOS/pc64 - Marvell / NXP PCIe WiFi driver (see mrvlwifi.h).
 *
 * The mwifiex family: 88W8897 (PCIe8897) and 88W8997 (PCIe8997), plus 88W8766.
 * Firmware-driven: the host streams a Marvell firmware image into the chip over
 * a scratch-register "boot command" channel (the firmware paces it), then talks
 * a HostCmd/response/event + TX/RX DMA-ring interface. The firmware does all
 * 802.11 MAC work and hardware CCMP once keys are installed; the host runs the
 * WPA2-PSK 4-way handshake in software (wifi_wpa.c) and moves Ethernet frames
 * wrapped in a TxPD/RxPD. Publishes uno_nic_t; inert when no card is present.
 *
 * Everything is polled (the upstream download + datapath are already poll-driven
 * in Linux - no MSI needed): we read PCIE_HOST_INT_STATUS and the ring pointer
 * registers directly. DMA is identity-mapped (virt==phys under UEFI boot
 * services). Register map, firmware-download loop, PFU descriptor rings, TxPD/
 * RxPD, the command/event interface and the V2 CCMP key install all follow the
 * Linux mwifiex driver exactly. Firmware (mrvl/pcie8897_uapsta.bin etc.) is
 * user-supplied on the ESP under FIRMWARE\ (licence forbids bundling).
 *
 * HARDWARE-PENDING: the one load-bearing gap is the exact field layout of
 * HostCmd_CMD_PCIE_DESC_DETAILS (0x00fa) - it publishes the ring physical
 * addresses to firmware and its body was not fully extractable; it is built
 * best-effort here and flagged. Verified inert when no supported card is on the
 * bus (so QEMU regressions pass). See NETWORK.md.
 * ======================================================================== */
#include "mrvlwifi.h"
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
 * status + helpers
 * ===================================================================== */
static char g_status[160];
static void st_set(const char *s){ int i=0; while(s[i]&&i<(int)sizeof g_status-1){g_status[i]=s[i];i++;} g_status[i]=0; }
static void st_cat(const char *s){ int i=0; while(g_status[i])i++; while(*s&&i<(int)sizeof g_status-1)g_status[i++]=*s++; g_status[i]=0; }
static void mdelay_(int ms){ uno_pc64_delay_ms(ms); }

/* =====================================================================
 * MMIO (BAR2 register window)
 * ===================================================================== */
static volatile u8 *g_bar;
static pci_dev g_pci;
static int g_present, g_bound, g_joined;
static u8  g_mac[6];

static u32 rd(u32 o){ return *(volatile u32 *)(g_bar+o); }
static void wr(u32 o,u32 v){ *(volatile u32 *)(g_bar+o)=v; }

/* =====================================================================
 * registers (pcie.h)
 * ===================================================================== */
#define PCIE_SCRATCH_0   0xC10
#define PCIE_SCRATCH_1   0xC14
#define PCIE_CPU_INT_EVENT   0xC18
#define PCIE_CPU_INT_STATUS  0xC1C
#define PCIE_HOST_INT_STATUS 0xC30
#define PCIE_HOST_INT_MASK   0xC34
#define PCIE_HOST_INT_STATUS_MASK 0xC3C
#define PCIE_SCRATCH_2   0xC40
#define PCIE_SCRATCH_3   0xC44
#define PCIE_SCRATCH_4   0xCD0
#define PCIE_SCRATCH_5   0xCD4
#define PCIE_SCRATCH_10  0xCE8
#define PCIE_SCRATCH_11  0xCEC
#define PCIE_SCRATCH_12  0xCF0
#define PCIE_RD_DATA_PTR_Q0_Q1 0xC08C
#define PCIE_WR_DATA_PTR_Q0_Q1 0xC05C

#define CPU_INTR_DNLD_RDY   0x01
#define CPU_INTR_DOOR_BELL  0x02
#define CPU_INTR_EVENT_DONE 0x20
#define HOST_INTR_DNLD_DONE 0x01
#define HOST_INTR_UPLD_RDY  0x02
#define HOST_INTR_CMD_DONE  0x04
#define HOST_INTR_EVENT_RDY 0x08
#define HOST_INTR_MASK      0x0F

#define FIRMWARE_READY_PCIE 0xfedcba00u
#define MWIFIEX_PCIE_FLR_HAPPENS 0xFEDCBABAu
#define UPLD_SIZE   2312
#define FW_DL_BLK   256
#define MAXTXRX_BD  32
#define MAX_EVT_BD  8
#define RXDATA_BUF  4096
#define INTF_HDR    4
#define TYPE_DATA   0
#define TYPE_CMD    1

/* PFU buffer descriptor (20 bytes) */
struct pfu_bd { u16 flags; u16 offset; u16 frag_len; u16 len; u64 paddr; u32 rsvd; } __attribute__((packed));
struct evt_bd { u64 paddr; u16 len; u16 flags; } __attribute__((packed));
#define BD_SOP 0x01
#define BD_EOP 0x02

/* per-chip register/mask set */
struct chipreg { u16 tx_rd,tx_wr,rx_rd,rx_wr; u32 tx_mask,tx_wrap,rx_mask,rx_wrap,tx_roll,rx_roll,evt_roll; u16 tx_start_shift; };
static const struct chipreg REG_8897 = {
    PCIE_RD_DATA_PTR_Q0_Q1, PCIE_WR_DATA_PTR_Q0_Q1, PCIE_WR_DATA_PTR_Q0_Q1, PCIE_RD_DATA_PTR_Q0_Q1,
    0x03FF0000u, 0x07FF0000u, 0x000003FFu, 0x000007FFu, (1u<<26), (1u<<10), (1u<<7), 16 };
static const struct chipreg REG_8997 = {
    0xC1A4, 0xC174, 0xC174, 0xC1A4,
    0x0FFF0000u, 0x1FFF0000u, 0x00000FFFu, 0x00001FFFu, (1u<<28), (1u<<12), (1u<<7), 16 };
static const struct chipreg *g_reg;

/* =====================================================================
 * device identity
 * ===================================================================== */
enum { CHIP_8766, CHIP_8897, CHIP_8997 };
static int  g_chip;
static char g_fwfile[24];
static u16  g_devid;
static char g_ssid_str[36];

static int identify(u16 ven,u16 dev)
{
    g_devid = dev;
    if (ven!=0x11ab && ven!=0x1b4b) return 0;
    switch (dev) {
    case 0x2b30: g_chip=CHIP_8766; g_reg=&REG_8897; strcpy(g_fwfile,"FIRMWARE\\W8766.FW"); return 1;
    case 0x2b38: g_chip=CHIP_8897; g_reg=&REG_8897; strcpy(g_fwfile,"FIRMWARE\\W8897.FW"); return 1;
    case 0x2b42: g_chip=CHIP_8997; g_reg=&REG_8997; strcpy(g_fwfile,"FIRMWARE\\W8997.FW"); return 1;
    }
    return 0;
}

/* =====================================================================
 * DMA rings + buffers (identity mapped: phys == virt)
 * ===================================================================== */
#define FWBUF_MAX (1024*1024)
static u8  g_fwbuf[FWBUF_MAX] __attribute__((aligned(64)));
static u32 g_fwlen;
static u8  g_dlbuf[UPLD_SIZE]  __attribute__((aligned(64)));   /* fw-download scratch */
static u8  g_cmdbuf[UPLD_SIZE] __attribute__((aligned(64)));   /* command buffer */
static u8  g_cmdrsp[UPLD_SIZE] __attribute__((aligned(64)));   /* command response */

static struct pfu_bd g_txring[MAXTXRX_BD] __attribute__((aligned(64)));
static struct pfu_bd g_rxring[MAXTXRX_BD] __attribute__((aligned(64)));
static struct evt_bd g_evtring[MAX_EVT_BD] __attribute__((aligned(64)));
static u8  g_txbuf[MAXTXRX_BD][RXDATA_BUF] __attribute__((aligned(64)));
static u8  g_rxbuf[MAXTXRX_BD][RXDATA_BUF] __attribute__((aligned(64)));
static u8  g_evtbuf[MAX_EVT_BD][2048]      __attribute__((aligned(64)));
static u32 g_txbd_wr, g_txbd_rd, g_rxbd_wr, g_rxbd_rd, g_evtbd_rd;
static u64 phys(const void *p){ return (u64)(uintptr_t)p; }

/* stashed decrypted data frames for recv() */
#define DATAQ 16
static struct { u8 buf[1600]; int len; } g_dataq[DATAQ];
static int g_dq_head, g_dq_tail;

/* =====================================================================
 * firmware download (single-stage, fully polled)
 * ===================================================================== */
static int fw_download(void)
{
    u32 offset = 0, tries; int retry = 0;
    wr(PCIE_HOST_INT_MASK, 0);                       /* mask host ints */
    /* FLR-resume path (combo image) is skipped: on a cold boot SCRATCH_13 != FLR */
    while (offset < g_fwlen) {
        u32 len = 0, txlen, blocks;
        for (tries = 0; tries < 100; tries++) { len = rd(PCIE_SCRATCH_2); if (len) break; mdelay_(1); }
        if (len == 0) break;
        if (len > UPLD_SIZE) { st_set("mwifiex: firmware asked for an oversized block"); return -1; }
        txlen = len;
        if (len & 1) {                               /* CRC-error retry: resend same offset */
            if (++retry > 2) { st_set("mwifiex: firmware download CRC retries exhausted"); return -1; }
            len &= ~1u; txlen = 0;
        } else {
            retry = 0;
            if (g_fwlen - offset < txlen) txlen = g_fwlen - offset;
            memcpy(g_dlbuf, g_fwbuf + offset, txlen);
        }
        blocks = (txlen + FW_DL_BLK - 1) / FW_DL_BLK;
        /* send one block: addr + size + doorbell */
        wr(PCIE_SCRATCH_0, (u32)phys(g_dlbuf));
        wr(PCIE_SCRATCH_1, (u32)(phys(g_dlbuf) >> 32));
        wr(PCIE_SCRATCH_2, blocks * FW_DL_BLK);
        wr(PCIE_CPU_INT_EVENT, CPU_INTR_DOOR_BELL);
        for (tries = 0; tries < 100; tries++) { if (!(rd(PCIE_CPU_INT_STATUS) & CPU_INTR_DOOR_BELL)) break; mdelay_(1); }
        offset += txlen;
    }
    /* signal driver-ready, poll firmware-ready */
    wr(PCIE_HOST_INT_STATUS_MASK, HOST_INTR_MASK);
    wr(PCIE_SCRATCH_12, FIRMWARE_READY_PCIE);
    for (tries = 0; tries < 100; tries++) { if (rd(PCIE_SCRATCH_3) == FIRMWARE_READY_PCIE) return 0; mdelay_(10); }
    st_set("mwifiex: firmware did not become ready");
    return -1;
}

/* =====================================================================
 * rings
 * ===================================================================== */
static void rings_init(void)
{
    int i;
    for (i = 0; i < MAXTXRX_BD; i++) {
        g_txring[i].flags = 0; g_txring[i].offset = 0; g_txring[i].frag_len = 0;
        g_txring[i].len = 0; g_txring[i].paddr = phys(g_txbuf[i]); g_txring[i].rsvd = 0;
        g_rxring[i].flags = BD_SOP|BD_EOP; g_rxring[i].offset = 0;
        g_rxring[i].frag_len = RXDATA_BUF; g_rxring[i].len = RXDATA_BUF;
        g_rxring[i].paddr = phys(g_rxbuf[i]); g_rxring[i].rsvd = 0;
    }
    for (i = 0; i < MAX_EVT_BD; i++) {
        g_evtring[i].paddr = phys(g_evtbuf[i]); g_evtring[i].len = 2048; g_evtring[i].flags = 0;
    }
    g_txbd_wr = 0; g_txbd_rd = 0;
    g_rxbd_wr = 0; g_rxbd_rd = g_reg->rx_roll;        /* rx rdptr starts at the rollover bit */
    g_evtbd_rd = g_reg->evt_roll;
}

/* =====================================================================
 * command / response interface (single-buffer doorbell)
 * ===================================================================== */
#define S_DS_GEN 8
static u16 g_seq;
/* build [intf hdr 4][host_cmd_ds_gen 8][body...] in g_cmdbuf; send + wait resp.
 * returns the response body pointer (after the 8-byte cmd header) or NULL. */
static const u8 *send_cmd(u16 cmd, const void *body, int blen, int *rsp_len)
{
    u8 *p = g_cmdbuf; int total = INTF_HDR + S_DS_GEN + blen; u32 tries;
    if (total > UPLD_SIZE) return 0;
    p[0]=(u8)total; p[1]=(u8)(total>>8); p[2]=TYPE_CMD; p[3]=0;    /* interface header */
    { u8 *h = p + INTF_HDR;
      h[0]=(u8)cmd; h[1]=(u8)(cmd>>8);
      h[2]=(u8)(total-INTF_HDR); h[3]=(u8)((total-INTF_HDR)>>8);   /* size (excl intf hdr) */
      h[4]=(u8)g_seq; h[5]=(u8)(g_seq>>8); h[6]=0; h[7]=0;
      if (body && blen) memcpy(h+8, body, blen); }
    g_seq++;
    /* publish response buffer, then command buffer + size, then doorbell */
    wr(PCIE_SCRATCH_4, (u32)phys(g_cmdrsp));   wr(PCIE_SCRATCH_5, (u32)(phys(g_cmdrsp)>>32));
    wr(PCIE_SCRATCH_0, (u32)phys(g_cmdbuf));   wr(PCIE_SCRATCH_1, (u32)(phys(g_cmdbuf)>>32));
    wr(PCIE_SCRATCH_2, (u32)total);
    wr(PCIE_CPU_INT_EVENT, CPU_INTR_DOOR_BELL);
    /* wait for CMD_DONE */
    for (tries = 0; tries < 200; tries++) {
        u32 s = rd(PCIE_HOST_INT_STATUS);
        if (s & HOST_INTR_CMD_DONE) { wr(PCIE_HOST_INT_STATUS, ~HOST_INTR_CMD_DONE); break; }
        mdelay_(1);
    }
    if (tries >= 200) return 0;
    { int rl = g_cmdrsp[0] | (g_cmdrsp[1]<<8);
      wr(PCIE_SCRATCH_4, 0); wr(PCIE_SCRATCH_5, 0);      /* release rsp buffer */
      if (rl < INTF_HDR + S_DS_GEN) return 0;
      if (rsp_len) *rsp_len = rl - INTF_HDR - S_DS_GEN;
      return g_cmdrsp + INTF_HDR + S_DS_GEN; }
}

/* command IDs */
#define CMD_GET_HW_SPEC      0x0003
#define CMD_802_11_SCAN      0x0006
#define CMD_802_11_ASSOCIATE 0x0012
#define CMD_MAC_CONTROL      0x0028
#define CMD_KEY_MATERIAL     0x005e
#define CMD_FUNC_INIT        0x00a9
#define CMD_PCIE_DESC_DETAILS 0x00fa
#define ACT_GEN_GET 0x0000
#define ACT_GEN_SET 0x0001

/* PCIE_DESC_DETAILS: publish ring physical addresses to firmware.
 * [metal gap] the exact field order of host_cmd_ds_pcie_details was not fully
 * extractable; this follows the documented fields (tx/rx/evt bd lo/hi + counts
 * + sleep cookie). Verify against mwifiex_cmd_pcie_host_spec() on metal. */
static void cmd_pcie_desc_details(void)
{
    u8 b[64]; int i = 0;
    #define PUT32(v) do{ u32 _v=(u32)(v); b[i++]=(u8)_v;b[i++]=(u8)(_v>>8);b[i++]=(u8)(_v>>16);b[i++]=(u8)(_v>>24);}while(0)
    PUT32(phys(g_txring));       PUT32(phys(g_txring)>>32);
    PUT32(phys(g_rxring));       PUT32(phys(g_rxring)>>32);
    PUT32(phys(g_evtring));      PUT32(phys(g_evtring)>>32);
    PUT32(MAXTXRX_BD); PUT32(MAXTXRX_BD); PUT32(MAX_EVT_BD);
    #undef PUT32
    send_cmd(CMD_PCIE_DESC_DETAILS, b, i, 0);
}
static void cmd_func_init(void){ send_cmd(CMD_FUNC_INIT, 0, 0, 0); }
static void cmd_get_hw_spec(void)
{
    int rl=0; u16 act=ACT_GEN_GET; const u8 *r = send_cmd(CMD_GET_HW_SPEC, &act, 2, &rl);
    if (r && rl >= 14) memcpy(g_mac, r + 8, 6);   /* permanent_addr @ body+8 */
}
static void cmd_mac_control(void)
{
    u32 act = 0x01|0x02|0x10;                     /* RX_ON | TX_ON | ETHERNETII */
    u8 b[4]; b[0]=(u8)act;b[1]=(u8)(act>>8);b[2]=(u8)(act>>16);b[3]=(u8)(act>>24);
    send_cmd(CMD_MAC_CONTROL, b, 4, 0);
}

/* V2 CCMP key install (KEY_TYPE_ID_AES = 2) */
static void cmd_key_ccmp(const u8 mac[6], int keyidx, int group, const u8 *key)
{
    u8 b[48]; int i=0, klen=16, plen;
    u16 key_info = 0x04 /*ENABLED*/;
    #define P16(v) do{ u16 _v=(u16)(v); b[i++]=(u8)_v; b[i++]=(u8)(_v>>8);}while(0)
    if (group) key_info |= 0x01|0x20;             /* MCAST | RX_KEY */
    else       key_info |= 0x02|0x10|0x20;        /* UNICAST | TX | RX */
    P16(ACT_GEN_SET);                             /* action */
    plen = 10 /*fixed*/ + (2 + 8 + klen);         /* key_param_set.len = fixed + aes_param */
    P16(0x019C);                                  /* TLV_TYPE_KEY_PARAM_V2 */
    P16(plen);
    memcpy(b+i, mac, 6); i+=6;
    b[i++]=(u8)(keyidx & 0xf);
    b[i++]=2;                                     /* KEY_TYPE_ID_AES */
    P16(key_info);
    /* aes_param: pn[8], key_len, key[16] */
    memset(b+i, 0, 8); i+=8;                      /* pn (rx seq) - 0 for a fresh install */
    P16(klen);
    memcpy(b+i, key, klen); i+=klen;
    #undef P16
    send_cmd(CMD_KEY_MATERIAL, b, i, 0);
}

/* =====================================================================
 * TX / RX (802.3 frame wrapped in TxPD/RxPD + 4-byte interface header)
 * ===================================================================== */
static u32 tx_slot(void){ return (g_txbd_wr & g_reg->tx_mask) >> g_reg->tx_start_shift; }
static void tx_ethernet(const u8 *eth, int ethlen)
{
    u32 slot = tx_slot();
    u8 *buf = g_txbuf[slot];
    int off = INTF_HDR + 20;                      /* interface hdr + TxPD */
    if (ethlen + off > RXDATA_BUF) return;
    memset(buf, 0, off);
    buf[0]=(u8)(off+ethlen); buf[1]=(u8)((off+ethlen)>>8); buf[2]=TYPE_DATA; buf[3]=0;
    { u8 *pd = buf + INTF_HDR;                     /* struct txpd */
      pd[0]=0; pd[1]=0;                            /* bss_type=STA, bss_num=0 */
      pd[2]=(u8)ethlen; pd[3]=(u8)(ethlen>>8);     /* tx_pkt_length */
      pd[4]=20; pd[5]=0;                           /* tx_pkt_offset = sizeof(txpd) */
      pd[6]=0; pd[7]=0; }                          /* tx_pkt_type = data */
    memcpy(buf + off, eth, ethlen);
    /* fill the PFU descriptor */
    g_txring[slot].paddr = phys(buf);
    g_txring[slot].len = (u16)(off + ethlen);
    g_txring[slot].frag_len = (u16)(off + ethlen);
    g_txring[slot].offset = 0;
    g_txring[slot].flags = BD_SOP|BD_EOP;
    /* advance write pointer (+= one slot) with rollover toggle */
    g_txbd_wr += (1u << g_reg->tx_start_shift);
    if ((g_txbd_wr & g_reg->tx_mask) == (u32)(MAXTXRX_BD << g_reg->tx_start_shift))
        g_txbd_wr = (g_txbd_wr & g_reg->tx_roll) ^ g_reg->tx_roll;
    wr(g_reg->tx_wr, g_txbd_wr | (g_rxbd_rd & g_reg->rx_wrap));
    wr(PCIE_CPU_INT_EVENT, CPU_INTR_DNLD_RDY);
}

static const u8 SNAP[6] = { 0xAA,0xAA,0x03,0x00,0x00,0x00 };
static wpa_sm_t g_wpa; static int g_keys_done; static u8 g_bssid[6];

static void queue_data(const u8 *rxpd_frame, int plen)
{
    /* rxpd_frame points at struct rx_packet_hdr: eth803(14) + rfc1042(8) */
    u8 eth[1600]; const u8 *llc = rxpd_frame + 14; u16 et; int paylen;
    if (plen < 22) return;
    if (memcmp(llc, SNAP, 6) != 0) return;
    et = (u16)((llc[6]<<8) | llc[7]);
    paylen = plen - 22;
    if (paylen < 0 || paylen > 1600-14) return;
    memcpy(eth, rxpd_frame, 12);                  /* dst[6] src[6] */
    eth[12]=(u8)(et>>8); eth[13]=(u8)et;
    memcpy(eth+14, rxpd_frame + 22, paylen);
    { int n=14+paylen, nx=(g_dq_head+1)%DATAQ; if(nx==g_dq_tail) return;
      memcpy(g_dataq[g_dq_head].buf,eth,n); g_dataq[g_dq_head].len=n; g_dq_head=nx; }
}
static void handle_eapol(const u8 *body, int len)
{
    u8 reply[600]; int r = wpa_sm_rx_eapol(&g_wpa, body, len, reply, sizeof reply);
    if (r > 0) {
        u8 eth[600]; memcpy(eth,g_bssid,6); memcpy(eth+6,g_mac,6); eth[12]=0x88; eth[13]=0x8E;
        memcpy(eth+14, reply, r); tx_ethernet(eth, 14+r);
    }
    if (g_wpa.state==WPA_ST_DONE && !g_keys_done) {
        cmd_key_ccmp(g_bssid, 0, 0, g_wpa.ptk+32);                    /* pairwise PTK */
        if (g_wpa.gtk_len) { u8 bc[6]; memset(bc,0xff,6); cmd_key_ccmp(bc, g_wpa.gtk_idx, 1, g_wpa.gtk); }
        g_keys_done = 1; g_joined = 1;                               /* PORT_RELEASE confirms on metal */
    }
}
/* one RX data buffer: [intf hdr 4][RxPD 20][rx_packet_hdr...] */
static void rx_one(const u8 *buf)
{
    int rl = buf[0] | (buf[1]<<8);
    const u8 *pd = buf + INTF_HDR;
    int pkt_off = pd[4] | (pd[5]<<8);
    int pkt_len = pd[2] | (pd[3]<<8);
    const u8 *frame = pd + pkt_off;
    if (rl < INTF_HDR + 20 || pkt_len <= 0) return;
    /* device-supplied pkt_off/pkt_len are untrusted: keep frame within rl */
    if (pkt_off < 0 || pkt_len > 1600 || pkt_off + pkt_len > rl - INTF_HDR) return;
    /* EAPOL? rx_packet_hdr's SNAP ethertype 0x888E means EAPOL */
    { const u8 *llc = frame + 14;
      if (pkt_len >= 22 && !memcmp(llc, SNAP, 6)) {
          u16 et = (u16)((llc[6]<<8)|llc[7]);
          if (et == 0x888E) handle_eapol(frame + 22, pkt_len - 22);
          else queue_data(frame, pkt_len);
      } }
}
static void rx_poll(void)
{
    u32 wrptr = rd(g_reg->rx_wr);
    g_rxbd_wr = wrptr;
    while ((g_rxbd_wr & g_reg->rx_mask) != (g_rxbd_rd & g_reg->rx_mask) ||
           (g_rxbd_wr & g_reg->rx_roll) == (g_rxbd_rd & g_reg->rx_roll)) {
        u32 ri = g_rxbd_rd & g_reg->rx_mask;
        if (ri >= MAXTXRX_BD) break;
        rx_one(g_rxbuf[ri]);
        g_rxring[ri].flags = BD_SOP|BD_EOP; g_rxring[ri].len = RXDATA_BUF;
        g_rxring[ri].frag_len = RXDATA_BUF; g_rxring[ri].offset = 0;
        if (((++g_rxbd_rd) & g_reg->rx_mask) == MAXTXRX_BD)
            g_rxbd_rd = (g_rxbd_rd & g_reg->rx_roll) ^ g_reg->rx_roll;
        wr(g_reg->rx_rd, g_rxbd_rd | (g_txbd_wr & g_reg->tx_wrap));
        if ((g_rxbd_rd & g_reg->rx_mask) == (wrptr & g_reg->rx_mask)) break;
    }
    /* ack the RX interrupt bit */
    if (rd(PCIE_HOST_INT_STATUS) & HOST_INTR_UPLD_RDY) wr(PCIE_HOST_INT_STATUS, ~HOST_INTR_UPLD_RDY);
}

/* =====================================================================
 * connect + WIFI.CFG
 * ===================================================================== */
static char g_cfg_ssid[36], g_cfg_psk[80];
int  uno_fs_volumes(void); int uno_fs_kind(int); long uno_fs_size(int,const char*);
long uno_fs_read(int,const char*,unsigned char*,long);

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
static void connect(void)
{
    u8 pmk[32];
    /* [metal gap] scan (CMD_802_11_SCAN) to find the BSS + its RSN IE, then
       CMD_802_11_ASSOCIATE with the SSID/rates/RSN TLVs. The associate response
       carries success synchronously; the controlled port opens on EVENT_PORT_
       RELEASE after the 4-way handshake + key install below. */
    memset(g_bssid, 0xFF, 6);
    wpa_pmk_from_psk(g_cfg_ssid,(int)strlen(g_cfg_ssid),g_cfg_psk,pmk);
    wpa_sm_init(&g_wpa,pmk,g_mac,g_bssid);
    strncpy(g_ssid_str,g_cfg_ssid,sizeof g_ssid_str-1);
}

/* =====================================================================
 * uno_nic_t
 * ===================================================================== */
static uno_nic_t g_nic;
static int mrvl_send(void *ctx,const void *pkt,int len)
{ (void)ctx; if(!g_bound||!g_joined||len<=0||len>1514) return -1; tx_ethernet((const u8*)pkt,len); return len; }
static int mrvl_recv(void *ctx,void *pkt,int cap)
{
    (void)ctx; if(!g_bound) return 0; rx_poll();
    if (g_dq_tail!=g_dq_head){ int n=g_dataq[g_dq_tail].len; if(n>cap)n=cap;
        memcpy(pkt,g_dataq[g_dq_tail].buf,n); g_dq_tail=(g_dq_tail+1)%DATAQ; return n; }
    return 0;
}
static int mrvl_link(void *ctx){ (void)ctx; return g_bound && g_joined; }

/* =====================================================================
 * bring-up
 * ===================================================================== */
int mrvlwifi_present(void)
{
    pci_dev d;
    if (g_present) return 1;
    /* Marvell/NXP WiFi: match on vendor+device (class is not reliable). */
    if (pci_find_class(0x02, 0x80, &d)) {
        u16 dev = pci_cfg_read16(&d, 2);
        if (identify(d.vendor, dev)) { g_pci=d; g_present=1; return 1; }
    }
    return 0;
}

uno_nic_t *mrvlwifi_nic(void)
{
    int vol; long fn;
    if (g_bound) return &g_nic;
    if (!mrvlwifi_present()) { st_set("no Marvell/NXP WiFi card"); return 0; }
    st_set("Marvell WiFi "); { char h[7]="0x0000"; const char*x="0123456789ABCDEF";
        h[2]=x[(g_devid>>12)&15];h[3]=x[(g_devid>>8)&15];h[4]=x[(g_devid>>4)&15];h[5]=x[g_devid&15]; st_cat(h); }

    vol = firmware_volume();
    if (vol<0){ st_set("Marvell WiFi: no WIFI.CFG on the ESP"); return 0; }
    if (read_config(vol)<0){ st_set("Marvell WiFi: WIFI.CFG has no ssid="); return 0; }
    fn = uno_fs_read(vol, g_fwfile, g_fwbuf, FWBUF_MAX);
    if (fn<=0) fn = uno_fs_read(vol, g_fwfile+9, g_fwbuf, FWBUF_MAX);
    if (fn<=0){ st_set("Marvell WiFi: firmware not found ("); st_cat(g_fwfile); st_cat(")"); return 0; }
    g_fwlen = (u32)fn;

    pci_enable_bus_master(&g_pci);
    g_bar = (volatile u8 *)(uintptr_t)pci_bar(&g_pci, 2);
    if (!g_bar) g_bar = (volatile u8 *)(uintptr_t)pci_bar(&g_pci, 0);
    if (!g_bar){ st_set("Marvell WiFi: no register BAR"); return 0; }

    if (rd(PCIE_SCRATCH_3) != FIRMWARE_READY_PCIE) {   /* download unless already running */
        if (fw_download() < 0) return 0;
    }
    rings_init();
    cmd_pcie_desc_details();
    cmd_func_init();
    cmd_get_hw_spec();
    cmd_mac_control();
    connect();

    g_nic.ctx=0; g_nic.send=mrvl_send; g_nic.recv=mrvl_recv; g_nic.link=mrvl_link;
    g_bound=1;
    st_set("Marvell WiFi bound: "); st_cat(g_ssid_str[0]?g_ssid_str:g_cfg_ssid);
    st_cat(g_joined?" (joined)":" (associating)");
    return &g_nic;
}

const unsigned char *mrvlwifi_mac(void){ return g_mac; }
void mrvlwifi_status_str(char *buf,int cap)
{ int i=0; if(cap<=0)return; while(g_status[i]&&i<cap-1){buf[i]=g_status[i];i++;} buf[i]=0; }
