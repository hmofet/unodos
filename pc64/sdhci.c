/* ===========================================================================
 * UnoDOS/pc64 - native SDHCI driver: eMMC (the Surface Laptop Go's internal
 * storage) and SD cards, with no firmware in the path.
 *
 * Same shape as the port's other silicon drivers (ahci/nvme/xhci): find the
 * controller by PCI class (08/05, the SD Host Controller standard), map
 * BAR0, bring the card up, and move sectors polled - no interrupts, no
 * firmware services, no allocation. Registers the medium with the block
 * layer (blkdev.h) as "emmc0" or "sd0".
 *
 * Card bring-up follows the standard dual identity probe: CMD0, then the SD
 * handshake (CMD8 + ACMD41); if the card ignores it, the MMC/eMMC handshake
 * (CMD1, host-assigned RCA, EXT_CSD for capacity). The data plane is
 * identical either way: 4-bit bus at ~25 MHz, single-block CMD17/CMD24 with
 * PIO through the buffer-data port - byte-bounded, no DMA descriptors, and
 * plenty for post-detach .UNO loads + document saves (multi-block/SDMA is
 * the upgrade path if this ever becomes the bottleneck).
 *
 * Like ahci/nvme this normally runs DETACHED only (the firmware owns the
 * controller while boot services live). -DUNO_SDHCI_TEST brings it up
 * eagerly while attached for QEMU (safe there when the firmware ships no
 * SdMmc driver - then we are the only one touching the controller).
 *
 * Bounds: every wait is an iteration-bounded spin; a dead controller or an
 * empty slot fails probe instead of hanging boot.
 * ======================================================================== */
#include "blkdev.h"
#include "pc64_pci.h"
#include "string.h"
#include <stdint.h>

/* ---- SDHCI registers (offsets into BAR0) ---------------------------------- */
#define R_BLKSZ   0x04                 /* 16-bit: transfer block size          */
#define R_BLKCNT  0x06                 /* 16-bit                               */
#define R_ARG     0x08
#define R_XFER    0x0C                 /* 16-bit transfer mode                 */
#define R_CMD     0x0E                 /* 16-bit: index<<8 | flags             */
#define R_RESP0   0x10                 /* .. 0x1C                              */
#define R_DATA    0x20                 /* PIO buffer data port                 */
#define R_PRES    0x24                 /* present state                        */
#define R_HCTL    0x28                 /* 8-bit host control 1                 */
#define R_PWR     0x29                 /* 8-bit bus power                      */
#define R_CLK     0x2C                 /* 16-bit clock control                 */
#define R_TMO     0x2E                 /* 8-bit data timeout                   */
#define R_SRST    0x2F                 /* 8-bit software reset                 */
#define R_ISTAT   0x30                 /* 16-bit normal interrupt status (W1C) */
#define R_ESTAT   0x32                 /* 16-bit error interrupt status (W1C)  */
#define R_ISTEN   0x34                 /* status-latch enables                 */
#define R_ESTEN   0x36
#define R_CAPS    0x40                 /* 64-bit capabilities                  */
#define R_HVER    0xFE                 /* 16-bit host version                  */

#define IS_CMD_DONE  0x0001
#define IS_XFER_DONE 0x0002
#define IS_WR_READY  0x0010
#define IS_RD_READY  0x0020
#define IS_ERROR     0x8000

/* command register response-type flags */
#define RSP_NONE  0x00
#define RSP_R2    0x09                 /* 136-bit + CRC                        */
#define RSP_R3    0x02                 /* 48-bit, no CRC/index (OCR)           */
#define RSP_R1    0x1A                 /* 48-bit + CRC + index                 */
#define RSP_R1B   0x1B                 /* 48-bit + busy                       */
#define F_DATA    0x20                 /* data present                         */

#define SPIN_MAX  4000000u
#define SECT      512u

static volatile uint8_t *gM;
static pci_dev  g_pci;
static int      g_present;             /* controller up                        */
static int      g_is_mmc;              /* 1 = eMMC/MMC, 0 = SD                 */
static int      g_blockaddr;           /* 1 = LBA arguments, 0 = byte offsets  */
static uint32_t g_rca;                 /* RCA<<16, ready to OR into arguments  */
static unsigned long long g_sectors;

static uint8_t  r8 (unsigned o) { return gM[o]; }
static uint16_t r16(unsigned o) { return *(volatile uint16_t *)(gM + o); }
static uint32_t r32(unsigned o) { return *(volatile uint32_t *)(gM + o); }
static void w8 (unsigned o, uint8_t  v) { gM[o] = v; }
static void w16(unsigned o, uint16_t v) { *(volatile uint16_t *)(gM + o) = v; }
static void w32(unsigned o, uint32_t v) { *(volatile uint32_t *)(gM + o) = v; }

static int spin_stat(uint16_t want)    /* wait for a status bit (or error)     */
{
    unsigned i;
    for (i = 0; i < SPIN_MAX; i++) {
        uint16_t s = r16(R_ISTAT);
        if (s & IS_ERROR) return 0;
        if (s & want) { w16(R_ISTAT, want); return 1; }
    }
    return 0;
}

static void recover(void)              /* clear errors + reset CMD/DAT lines   */
{
    w16(R_ESTAT, 0xFFFF);
    w16(R_ISTAT, 0xFFFF);
    w8(R_SRST, 0x06);                  /* reset CMD + DAT                      */
    { unsigned i; for (i = 0; i < SPIN_MAX && (r8(R_SRST) & 0x06); i++) { } }
}

/* issue one command; data phase (if any) is driven by the caller */
static int cmd(unsigned idx, uint32_t arg, unsigned rsp, unsigned data)
{
    unsigned i;
    for (i = 0; i < SPIN_MAX && (r32(R_PRES) & 0x3); i++) { }  /* inhibits    */
    w32(R_ARG, arg);
    w16(R_CMD, (uint16_t)((idx << 8) | rsp | (data ? F_DATA : 0)));
    if (!spin_stat(IS_CMD_DONE)) { recover(); return 0; }
    return 1;
}

static uint32_t resp(void) { return r32(R_RESP0); }

/* 136-bit response bit extractor: RESP0..3 hold CSD[127:8] (CRC stripped),
 * so CSD bit n lives at response bit n-8 */
static uint32_t r2_bits(int start, int len)
{
    uint32_t out = 0; int i;
    for (i = len - 1; i >= 0; i--) {
        int b = start + i - 8;
        out = (out << 1) | ((r32(R_RESP0 + (b / 32) * 4) >> (b % 32)) & 1);
    }
    return out;
}

/* ---- clock / power -------------------------------------------------------- */
static void clock_set(unsigned khz)
{
    uint32_t caps = r32(R_CAPS);
    unsigned base = (caps >> 8) & 0xFF;            /* MHz                      */
    unsigned ver  = r16(R_HVER) & 0xFF;            /* 0=v1 1=v2 2=v3           */
    unsigned div, val;
    if (!base) base = 50;                          /* cap says "ask elsewhere" */
    if (ver >= 2) {                                /* v3: 10-bit divisor       */
        div = (base * 1000 + 2 * khz - 1) / (2 * khz);   /* freq = base/(2N)  */
        if (base * 1000 <= khz) div = 0;
        if (div > 1023) div = 1023;
        val = ((div & 0xFF) << 8) | ((div >> 8) << 6);
    } else {                                       /* v2: power-of-two 2N      */
        unsigned n = 1;
        while (n < 256 && base * 1000 / (2 * n) > khz) n <<= 1;
        if (base * 1000 <= khz) n = 0;
        val = (n & 0xFF) << 8;
    }
    w16(R_CLK, 0);
    w16(R_CLK, (uint16_t)(val | 0x1));             /* internal clock on        */
    { unsigned i; for (i = 0; i < SPIN_MAX && !(r16(R_CLK) & 0x2); i++) { } }
    w16(R_CLK, (uint16_t)(val | 0x5));             /* SD clock on              */
}

static int host_up(void)
{
    uint32_t caps;
    unsigned i;
    w8(R_SRST, 0x01);                              /* reset everything         */
    for (i = 0; i < SPIN_MAX && (r8(R_SRST) & 0x01); i++) { }
    if (r8(R_SRST) & 0x01) return 0;
    w16(R_ISTEN, 0xFFFF); w16(R_ESTEN, 0xFFFF);    /* latch statuses (no irqs) */
    w16(R_ISTAT, 0xFFFF); w16(R_ESTAT, 0xFFFF);
    w8(R_TMO, 0x0E);                               /* max data timeout         */
    caps = r32(R_CAPS);
    w8(R_PWR, 0);
    if      (caps & (1u << 24)) w8(R_PWR, 0x0F);   /* 3.3 V                    */
    else if (caps & (1u << 25)) w8(R_PWR, 0x0D);   /* 3.0 V                    */
    else                        w8(R_PWR, 0x0B);   /* 1.8 V (Intel eMMC rails) */
    clock_set(400);                                /* ID phase: <= 400 kHz     */
    for (i = 0; i < 200000; i++) (void)r32(R_PRES);/* card power settle        */
    return 1;
}

/* ---- card identification -------------------------------------------------- */
static int card_up(void)
{
    unsigned i; uint32_t ocr = 0;

    if (!cmd(0, 0, RSP_NONE, 0)) return 0;         /* GO_IDLE                  */

    /* SD first: CMD8 (v2 check) then ACMD41 until ready */
    g_is_mmc = 0;
    { int v2 = cmd(8, 0x1AA, RSP_R1, 0) && (resp() & 0xFF) == 0xAA;
      for (i = 0; i < 1000; i++) {
          if (!cmd(55, 0, RSP_R1, 0)) break;       /* APP_CMD                  */
          if (!cmd(41, (v2 ? 0x40000000u : 0) | 0x00FF8000u, RSP_R3, 0)) break;
          ocr = resp();
          if (ocr & 0x80000000u) break;            /* powered up               */
      }
    }
    if (!(ocr & 0x80000000u)) {                    /* not SD -> eMMC/MMC       */
        g_is_mmc = 1;
        if (!cmd(0, 0, RSP_NONE, 0)) return 0;
        for (i = 0; i < 1000; i++) {
            /* sector-mode capable + high-voltage window (incl. dual-volt bit) */
            if (!cmd(1, 0x40FF8080u, RSP_R3, 0)) return 0;
            ocr = resp();
            if (ocr & 0x80000000u) break;
        }
        if (!(ocr & 0x80000000u)) return 0;
    }
    g_blockaddr = (ocr >> 30) & 1;                 /* SD CCS / MMC sector mode */

    if (!cmd(2, 0, RSP_R2, 0)) return 0;           /* ALL_SEND_CID             */
    if (g_is_mmc) {
        g_rca = 1u << 16;                          /* host assigns the RCA     */
        if (!cmd(3, g_rca, RSP_R1, 0)) return 0;
    } else {
        if (!cmd(3, 0, RSP_R1, 0)) return 0;       /* card publishes its RCA   */
        g_rca = resp() & 0xFFFF0000u;
    }

    /* capacity from the CSD (still in stand-by state) */
    if (!cmd(9, g_rca, RSP_R2, 0)) return 0;
    {
        unsigned structure = r2_bits(126, 2);
        if (!g_is_mmc && structure == 1) {         /* SD CSD v2 (SDHC/SDXC)    */
            g_sectors = ((unsigned long long)r2_bits(48, 22) + 1) * 1024;
        } else {                                   /* SD v1 / MMC CSD          */
            unsigned c_size = r2_bits(62, 12), mult = r2_bits(47, 3);
            unsigned bl_len = r2_bits(80, 4);
            g_sectors = ((unsigned long long)(c_size + 1) << (mult + 2))
                        << (bl_len > 9 ? bl_len - 9 : 0);
            if (g_is_mmc && c_size == 0xFFF) g_sectors = 0;  /* > 2 GB: EXT_CSD */
        }
    }

    if (!cmd(7, g_rca, RSP_R1B, 0)) return 0;      /* SELECT: -> transfer state */

    /* >2 GB eMMC: real capacity = EXT_CSD SEC_COUNT (a 512-byte data read) */
    if (g_is_mmc && !g_sectors) {
        static uint8_t ext[SECT] __attribute__((aligned(4)));
        unsigned w;
        w16(R_BLKSZ, SECT); w16(R_BLKCNT, 1);
        w16(R_XFER, 0x0010);                       /* read, single block       */
        if (!cmd(8, 0, RSP_R1, 1)) return 0;       /* MMC SEND_EXT_CSD         */
        if (!spin_stat(IS_RD_READY)) { recover(); return 0; }
        for (w = 0; w < SECT / 4; w++) ((uint32_t *)ext)[w] = r32(R_DATA);
        if (!spin_stat(IS_XFER_DONE)) { recover(); return 0; }
        g_sectors = (unsigned long long)ext[212] | ((unsigned long long)ext[213] << 8)
                  | ((unsigned long long)ext[214] << 16) | ((unsigned long long)ext[215] << 24);
    }
    if (!g_sectors) return 0;

    /* 4-bit bus, then full speed */
    if (g_is_mmc) {
        /* SWITCH: write EXT_CSD[183] (BUS_WIDTH) = 1 (4-bit) */
        if (cmd(6, 0x03B70100u, RSP_R1B, 0)) {
            unsigned i2; for (i2 = 0; i2 < SPIN_MAX && !(r32(R_PRES) & (1u << 20)); i2++) { }
            w8(R_HCTL, (uint8_t)(r8(R_HCTL) | 0x02));
        }
    } else {
        if (cmd(55, g_rca, RSP_R1, 0) && cmd(6, 2, RSP_R1, 0))   /* ACMD6 4-bit */
            w8(R_HCTL, (uint8_t)(r8(R_HCTL) | 0x02));
    }
    if (!g_blockaddr) cmd(16, SECT, RSP_R1, 0);    /* byte-addressed: blocklen  */
    clock_set(25000);                              /* data phase: 25 MHz        */
    return 1;
}

/* ---- polled single-block PIO data plane ----------------------------------- */
static int one_block(unsigned long long lba, void *buf, int write)
{
    uint32_t arg = g_blockaddr ? (uint32_t)lba : (uint32_t)(lba * SECT);
    uint32_t *p = (uint32_t *)buf;
    unsigned w;
    w16(R_BLKSZ, SECT); w16(R_BLKCNT, 1);
    w16(R_XFER, write ? 0x0000 : 0x0010);
    if (!cmd(write ? 24 : 17, arg, RSP_R1, 1)) return 0;
    if (!spin_stat(write ? IS_WR_READY : IS_RD_READY)) { recover(); return 0; }
    if (write) for (w = 0; w < SECT / 4; w++) w32(R_DATA, p[w]);
    else       for (w = 0; w < SECT / 4; w++) p[w] = r32(R_DATA);
    if (!spin_stat(IS_XFER_DONE)) { recover(); return 0; }
    return 1;
}

static int drv_read(uno_bdev *b, unsigned long long lba, unsigned int n, void *buf)
{
    uint8_t *out = (uint8_t *)buf; (void)b;
    while (n--) {
        if (!one_block(lba, out, 0)) return 0;
        lba++; out += SECT;
    }
    return 1;
}

static int drv_write(uno_bdev *b, unsigned long long lba, unsigned int n, const void *buf)
{
    const uint8_t *in = (const uint8_t *)buf; (void)b;
    while (n--) {
        if (!one_block(lba, (void *)in, 1)) return 0;
        lba++; in += SECT;
    }
    return 1;
}

/* ---- bring-up ------------------------------------------------------------- */
int uno_sdhci_present(void) { return g_present; }

int uno_sdhci_init(void)
{
    uno_bdev b;
    if (g_present) {
        /* already brought up (the eager -DUNO_SDHCI_TEST attached mode): the
         * M3 detach clears the blkdev registry and calls every native init
         * again, so re-register the live medium instead of vanishing */
        if (!g_sectors) return 0;
        goto reg;
    }
    if (!pci_find_class(0x08, 0x05, &g_pci)) return 0;
    pci_enable_bus_master(&g_pci);                 /* mem decode (PIO needs no BM) */
    gM = (volatile uint8_t *)(uintptr_t)pci_bar(&g_pci, 0);
    if (!gM) return 0;
    if (!host_up()) return 0;
    g_present = 1;                                 /* controller is ours now   */
    if (!card_up()) { g_sectors = 0; return 0; }   /* empty slot / dead card   */

reg:
    memset(&b, 0, sizeof b);
    b.native  = 1;
    b.sectors = g_sectors;
    if (g_is_mmc) { b.name[0]='e'; b.name[1]='m'; b.name[2]='m'; b.name[3]='c';
                    b.name[4]='0'; b.name[5]=0; }
    else          { b.name[0]='s'; b.name[1]='d'; b.name[2]='0'; b.name[3]=0; }
    b.pci_dev = g_pci.dev; b.pci_fn = g_pci.fn;
    b.read    = drv_read;
    b.write   = drv_write;
    return uno_blk_register(&b) ? 1 : 0;
}
