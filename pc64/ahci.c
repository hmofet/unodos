/* ===========================================================================
 * UnoDOS/pc64 - native AHCI driver (SATA disks with no firmware in the path).
 *
 * Same shape as the port's other silicon drivers (xhci/e1000/i2c_hid): find
 * the controller by PCI class (01/06), map the ABAR, bring up each implemented
 * port with static command-list/FIS buffers, and move sectors with polled
 * READ/WRITE DMA EXT - no interrupts, no firmware services, no allocation.
 * Registers every detected disk with the block layer (blkdev.h).
 *
 * Bounds: every wait is an iteration-bounded spin (tens of ms at any clock),
 * so a dead port fails the operation instead of hanging the OS.
 * ======================================================================== */
#include "blkdev.h"
#include "pc64_pci.h"
#include "string.h"
#include <stdint.h>

/* ---- HBA / port registers (offsets into the ABAR) ------------------------- */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_PI    0x0C
#define PORT_BASE 0x100
#define PORT_SIZE 0x80

#define PxCLB  0x00
#define PxCLBU 0x04
#define PxFB   0x08
#define PxFBU  0x0C
#define PxIS   0x10
#define PxIE   0x14
#define PxCMD  0x18
#define PxTFD  0x20
#define PxSIG  0x24
#define PxSSTS 0x28
#define PxSERR 0x30
#define PxCI   0x38

#define CMD_ST  (1u << 0)
#define CMD_SUD (1u << 1)
#define CMD_POD (1u << 2)
#define CMD_FRE (1u << 4)
#define CMD_FR  (1u << 14)
#define CMD_CR  (1u << 15)

#define SIG_SATA 0x00000101u

#define ATA_IDENTIFY   0xEC
#define ATA_READ_DMA48 0x25
#define ATA_WRITE_DMA48 0x35

#define SPIN_MAX 60000000u            /* bounded waits: tens of ms on any CPU */
#define CHUNK_SECT 128u               /* 64 KB per command, one PRD           */

/* ---- per-drive state ------------------------------------------------------ */
#define MAXDRV 4
typedef struct {
    volatile uint32_t *port;                       /* ABAR + port offset      */
    /* one command slot; AHCI alignment: CLB 1 KB, FIS 256 B, table 128 B     */
    uint8_t clb[1024] __attribute__((aligned(1024)));
    uint8_t fis[256]  __attribute__((aligned(256)));
    uint8_t ct[256]   __attribute__((aligned(128)));
} drive;

static drive   g_drv[MAXDRV];
static int     g_ndrv;
static int     g_present;
static pci_dev g_pci;

int uno_ahci_present(void) { return g_present; }

static inline uint32_t rd(volatile uint32_t *base, int off) { return base[off / 4]; }
static inline void     wr(volatile uint32_t *base, int off, uint32_t v) { base[off / 4] = v; }

static int spin_clear(volatile uint32_t *port, int off, uint32_t mask)
{
    uint32_t i;
    for (i = 0; i < SPIN_MAX; i++)
        if (!(rd(port, off) & mask)) return 1;
    return 0;
}

/* ---- one polled command through slot 0 ------------------------------------ */
static int cmd(drive *d, uint8_t ata, uint64_t lba, uint16_t count,
               void *buf, uint32_t bytes, int write)
{
    volatile uint32_t *p = d->port;
    uint32_t *hdr = (uint32_t *)d->clb;
    uint8_t  *cfis = d->ct;
    uint32_t *prd = (uint32_t *)(d->ct + 0x80);
    uint32_t i;

    /* command header 0: CFL=5 dwords, W as needed, PRDTL=1 */
    hdr[0] = 5u | (write ? (1u << 6) : 0) | (1u << 16);
    hdr[1] = 0;
    hdr[2] = (uint32_t)(uintptr_t)d->ct;
    hdr[3] = (uint32_t)((uintptr_t)d->ct >> 32);

    memset(d->ct, 0, sizeof d->ct);
    cfis[0] = 0x27; cfis[1] = 0x80;                /* H2D, command            */
    cfis[2] = ata;
    cfis[4] = (uint8_t)lba;         cfis[5] = (uint8_t)(lba >> 8);
    cfis[6] = (uint8_t)(lba >> 16); cfis[7] = 0x40;          /* LBA mode      */
    cfis[8] = (uint8_t)(lba >> 24); cfis[9] = (uint8_t)(lba >> 32);
    cfis[10] = (uint8_t)(lba >> 40);
    cfis[12] = (uint8_t)count; cfis[13] = (uint8_t)(count >> 8);

    prd[0] = (uint32_t)(uintptr_t)buf;
    prd[1] = (uint32_t)((uintptr_t)buf >> 32);
    prd[2] = 0;
    prd[3] = bytes - 1;                            /* DBC is len-1            */

    wr(p, PxIS, ~0u);                              /* clear stale status      */
    wr(p, PxSERR, ~0u);
    wr(p, PxCI, 1u);

    for (i = 0; i < SPIN_MAX; i++) {
        if (!(rd(p, PxCI) & 1u)) break;
        if (rd(p, PxIS) & (1u << 30)) return 0;    /* TFES: task-file error   */
    }
    if (rd(p, PxCI) & 1u) return 0;                /* timed out               */
    if (rd(p, PxTFD) & 0x01) return 0;             /* ERR in status           */
    return 1;
}

static int drv_read(uno_bdev *b, unsigned long long lba, unsigned int n, void *buf)
{
    drive *d = (drive *)b->ctx;
    uint8_t *out = (uint8_t *)buf;
    while (n) {
        unsigned int c = n > CHUNK_SECT ? CHUNK_SECT : n;
        if (!cmd(d, ATA_READ_DMA48, lba, (uint16_t)c, out, c * 512, 0)) return 0;
        lba += c; out += c * 512; n -= c;
    }
    return 1;
}

static int drv_write(uno_bdev *b, unsigned long long lba, unsigned int n, const void *buf)
{
    drive *d = (drive *)b->ctx;
    const uint8_t *in = (const uint8_t *)buf;
    while (n) {
        unsigned int c = n > CHUNK_SECT ? CHUNK_SECT : n;
        if (!cmd(d, ATA_WRITE_DMA48, lba, (uint16_t)c, (void *)in, c * 512, 1)) return 0;
        lba += c; in += c * 512; n -= c;
    }
    return 1;
}

/* ---- port bring-up -------------------------------------------------------- */
static int port_up(drive *d, volatile uint32_t *port)
{
    uint32_t ssts, sig, cmdreg;

    d->port = port;
    ssts = rd(port, PxSSTS);
    if ((ssts & 0xF) != 3) return 0;               /* no device / no PHY      */
    sig = rd(port, PxSIG);
    if (sig != SIG_SATA) return 0;                 /* disks only (no ATAPI)   */

    /* stop the port before reprogramming its buffers */
    cmdreg = rd(port, PxCMD);
    wr(port, PxCMD, cmdreg & ~CMD_ST);
    if (!spin_clear(port, PxCMD, CMD_CR)) return 0;
    cmdreg = rd(port, PxCMD);
    wr(port, PxCMD, cmdreg & ~CMD_FRE);
    if (!spin_clear(port, PxCMD, CMD_FR)) return 0;

    memset(d->clb, 0, sizeof d->clb);
    memset(d->fis, 0, sizeof d->fis);
    wr(port, PxCLB,  (uint32_t)(uintptr_t)d->clb);
    wr(port, PxCLBU, (uint32_t)((uintptr_t)d->clb >> 32));
    wr(port, PxFB,   (uint32_t)(uintptr_t)d->fis);
    wr(port, PxFBU,  (uint32_t)((uintptr_t)d->fis >> 32));
    wr(port, PxSERR, ~0u);
    wr(port, PxIS,   ~0u);
    wr(port, PxIE,   0);                           /* polled - no interrupts  */

    cmdreg = rd(port, PxCMD) | CMD_SUD | CMD_POD | CMD_FRE;
    wr(port, PxCMD, cmdreg);
    wr(port, PxCMD, rd(port, PxCMD) | CMD_ST);
    return 1;
}

int uno_ahci_init(void)
{
    volatile uint32_t *abar;
    uint32_t pi;
    int i, made = 0;
    static uint8_t id[512] __attribute__((aligned(16)));

    if (g_present) return g_ndrv;
    if (!pci_find_class(0x01, 0x06, &g_pci)) return 0;
    pci_enable_bus_master(&g_pci);                 /* mem decode + DMA        */
    abar = (volatile uint32_t *)(uintptr_t)pci_bar(&g_pci, 5);
    if (!abar) return 0;

    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | (1u << 31));   /* AHCI enable       */
    g_present = 1;

    pi = rd(abar, HBA_PI);
    for (i = 0; i < 32 && g_ndrv < MAXDRV; i++) {
        drive *d;
        uint64_t sectors;
        uno_bdev b;
        uint16_t *w;
        if (!(pi & (1u << i))) continue;
        d = &g_drv[g_ndrv];
        if (!port_up(d, abar + (PORT_BASE + i * PORT_SIZE) / 4)) continue;
        if (!cmd(d, ATA_IDENTIFY, 0, 0, id, 512, 0)) continue;
        w = (uint16_t *)id;
        sectors = ((uint64_t)w[103] << 48) | ((uint64_t)w[102] << 32)
                | ((uint64_t)w[101] << 16) | w[100];         /* LBA48 count   */
        if (!sectors)
            sectors = ((uint32_t)w[61] << 16) | w[60];       /* LBA28 count   */
        if (!sectors) continue;
        memset(&b, 0, sizeof b);
        b.native  = 1;
        b.sectors = sectors;
        b.name[0]='a'; b.name[1]='h'; b.name[2]='c'; b.name[3]='i';
        b.name[4]=(char)('0'+g_ndrv); b.name[5]=0;
        b.pci_dev = g_pci.dev; b.pci_fn = g_pci.fn;
        b.ctx     = d;
        b.read    = drv_read;
        b.write   = drv_write;
        if (uno_blk_register(&b)) { g_ndrv++; made++; }
    }
    return made;
}
