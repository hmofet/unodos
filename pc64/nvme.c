/* ===========================================================================
 * UnoDOS/pc64 - native NVMe driver (PCIe SSDs with no firmware in the path).
 *
 * Same shape as the port's other silicon drivers (ahci/xhci/e1000): find the
 * controller by PCI class (01/08), map BAR0, bring it up with static
 * admin + one I/O queue pair, and move sectors with polled READ/WRITE - no
 * interrupts, no firmware services, no allocation. Registers every
 * 512-byte-sector namespace with the block layer (blkdev.h).
 *
 * Like ahci.c this only ever runs DETACHED (post-ExitBootServices): while
 * boot services are live the firmware owns the controller and blkdev.c's
 * EFI Block IO fallback moves the sectors instead (one driver per
 * controller - reprogramming a live firmware controller corrupts its I/O).
 *
 * Data transfers use PRPs: PRP1 carries the (dword-aligned) buffer with its
 * in-page offset, PRP2 is either the second page or a pointer to the static
 * PRP list page - so a 64 KB chunk (17 pages worst case) needs no dynamic
 * memory. Chunk size shrinks to the controller's MDTS when that is smaller.
 *
 * Bounds: every wait is an iteration-bounded spin, so a dead controller
 * fails probe/IO instead of hanging the OS. Namespaces formatted with 4 KB
 * LBAs are skipped (fat.c speaks 512-byte sectors only).
 * ======================================================================== */
#include "blkdev.h"
#include "pc64_pci.h"
#include "string.h"
#include <stdint.h>

/* ---- controller registers (offsets into BAR0) ----------------------------- */
#define R_CAP   0x00                   /* 64-bit capabilities                  */
#define R_CC    0x14                   /* controller configuration             */
#define R_CSTS  0x1C                   /* status: bit0 RDY, bit1 CFS           */
#define R_AQA   0x24                   /* admin queue sizes (0-based)          */
#define R_ASQ   0x28                   /* admin SQ base (64-bit)               */
#define R_ACQ   0x30                   /* admin CQ base (64-bit)               */
#define R_DB    0x1000                 /* doorbells (stride from CAP.DSTRD)    */

#define CC_EN        (1u << 0)
#define CC_IOSQES    (6u << 16)        /* 2^6 = 64 B SQ entries                */
#define CC_IOCQES    (4u << 20)        /* 2^4 = 16 B CQ entries                */

/* opcodes */
#define A_CREATE_SQ  0x01
#define A_CREATE_CQ  0x05
#define A_IDENTIFY   0x06
#define A_SETFEAT    0x09
#define IO_WRITE     0x01
#define IO_READ      0x02

#define QDEPTH    16u                  /* one command outstanding; 16 is roomy */
#define SPIN_MAX  60000000u            /* RDY flips can take real time         */
#define PAGE      4096u
#define MAXCHUNK  128u                 /* 64 KB per command, like ahci.c       */
#define MAXNS     4

/* ---- static DMA structures (identity-mapped) ------------------------------ */
typedef struct { uint32_t dw[16]; } sqe;               /* 64 B */
typedef struct { uint32_t dw0, dw1, sqinfo, status; } cqe;   /* 16 B */

static sqe      gAsq[QDEPTH] __attribute__((aligned(4096)));
static cqe      gAcq[QDEPTH] __attribute__((aligned(4096)));
static sqe      gIsq[QDEPTH] __attribute__((aligned(4096)));
static cqe      gIcq[QDEPTH] __attribute__((aligned(4096)));
static uint64_t gPrpList[PAGE / 8] __attribute__((aligned(4096)));
static uint8_t  gIdBuf[PAGE] __attribute__((aligned(4096)));

typedef struct {
    unsigned nsid;
    unsigned long long sectors;
} nvme_ns;
static nvme_ns  g_ns[MAXNS];

static volatile uint8_t *gM;
static unsigned gDbShift;              /* doorbell stride: 2 + CAP.DSTRD       */
static unsigned gChunk = MAXCHUNK;     /* sectors per command (MDTS-capped)    */
static int      gPresent;
static pci_dev  g_pci;

/* per-queue state: tails/heads + phase (CQEs flip the phase bit each lap) */
static unsigned gSqT[2], gCqH[2], gPhase[2] = { 1, 1 };  /* [0]=admin [1]=io */
static uint16_t gCid;

static uint32_t r32(unsigned o) { return *(volatile uint32_t *)(gM + o); }
static void     w32(unsigned o, uint32_t v) { *(volatile uint32_t *)(gM + o) = v; }
static void     w64(unsigned o, uint64_t v) { w32(o, (uint32_t)v); w32(o + 4, (uint32_t)(v >> 32)); }

static void db_ring(unsigned qid, int is_cq, unsigned v)
{ w32(R_DB + ((2 * qid + (is_cq ? 1 : 0)) << gDbShift), v); }

/* ---- one polled command through a queue pair ------------------------------ */
static int submit(unsigned q, sqe *sq, cqe *cq, const sqe *c, uint32_t *result)
{
    unsigned t = gSqT[q], i;
    volatile cqe *e = (volatile cqe *)&cq[gCqH[q]];

    sq[t] = *c;
    sq[t].dw[0] |= (uint32_t)(++gCid) << 16;       /* unique CID              */
    __asm__ volatile ("" ::: "memory");
    gSqT[q] = (t + 1) % QDEPTH;
    db_ring(q, 0, gSqT[q]);

    for (i = 0; i < SPIN_MAX; i++)
        if (((e->status >> 16) & 1) == gPhase[q]) break;
    if (((e->status >> 16) & 1) != gPhase[q]) return 0;      /* timed out     */

    if (result) *result = e->dw0;
    { int ok = ((e->status >> 17) & 0x7FFF) == 0;  /* status field: 0 = good  */
      gCqH[q] = (gCqH[q] + 1) % QDEPTH;
      if (gCqH[q] == 0) gPhase[q] ^= 1;
      db_ring(q, 1, gCqH[q]);
      return ok; }
}

static int admin(const sqe *c, uint32_t *result)
{ return submit(0, gAsq, gAcq, c, result); }

/* ---- PRP setup for one transfer (dword-aligned buffer, <= gChunk sectors) - */
static void prps(sqe *c, const void *buf, uint32_t bytes)
{
    uint64_t a = (uint64_t)(uintptr_t)buf;
    uint64_t first_len = PAGE - (a % PAGE);
    c->dw[6] = (uint32_t)a; c->dw[7] = (uint32_t)(a >> 32);       /* PRP1    */
    if (bytes <= first_len) return;                               /* 1 page  */
    if (bytes <= first_len + PAGE) {                              /* 2 pages */
        uint64_t p2 = (a + first_len);
        c->dw[8] = (uint32_t)p2; c->dw[9] = (uint32_t)(p2 >> 32); /* PRP2    */
        return;
    }
    {   unsigned n = 0;                                           /* list    */
        uint64_t p = a + first_len;
        uint32_t left = bytes - (uint32_t)first_len;
        while (left) {
            gPrpList[n++] = p;
            p += PAGE;
            left -= (left > PAGE) ? PAGE : left;
        }
        c->dw[8] = (uint32_t)(uintptr_t)gPrpList;
        c->dw[9] = (uint32_t)((uint64_t)(uintptr_t)gPrpList >> 32);
    }
}

static int io_rw(unsigned nsid, int write, unsigned long long lba,
                 unsigned int nsect, void *buf)
{
    sqe c;
    memset(&c, 0, sizeof c);
    c.dw[0]  = write ? IO_WRITE : IO_READ;
    c.dw[1]  = nsid;
    prps(&c, buf, nsect * 512);
    c.dw[10] = (uint32_t)lba;
    c.dw[11] = (uint32_t)(lba >> 32);
    c.dw[12] = nsect - 1;                          /* NLB is 0-based          */
    return submit(1, gIsq, gIcq, &c, 0);
}

static int drv_read(uno_bdev *b, unsigned long long lba, unsigned int n, void *buf)
{
    unsigned nsid = (unsigned)(uintptr_t)b->ctx;
    uint8_t *out = (uint8_t *)buf;
    while (n) {
        unsigned int c = n > gChunk ? gChunk : n;
        if (!io_rw(nsid, 0, lba, c, out)) return 0;
        lba += c; out += c * 512; n -= c;
    }
    return 1;
}

static int drv_write(uno_bdev *b, unsigned long long lba, unsigned int n, const void *buf)
{
    unsigned nsid = (unsigned)(uintptr_t)b->ctx;
    const uint8_t *in = (const uint8_t *)buf;
    while (n) {
        unsigned int c = n > gChunk ? gChunk : n;
        if (!io_rw(nsid, 1, lba, c, (void *)in)) return 0;
        lba += c; in += c * 512; n -= c;
    }
    return 1;
}

/* ---- bring-up ------------------------------------------------------------- */
static int identify(unsigned cns, unsigned nsid)
{
    sqe c;
    memset(&c, 0, sizeof c);
    c.dw[0]  = A_IDENTIFY;
    c.dw[1]  = nsid;
    prps(&c, gIdBuf, PAGE);
    c.dw[10] = cns;
    return admin(&c, 0);
}

int uno_nvme_present(void) { return gPresent; }

int uno_nvme_init(void)
{
    uint32_t cap_lo, cap_hi, i, nn;
    int made = 0, nns = 0;

    if (gPresent) return 0;                        /* idempotent (like ahci)  */
    if (!pci_find_class(0x01, 0x08, &g_pci)) return 0;
    pci_enable_bus_master(&g_pci);
    gM = (volatile uint8_t *)(uintptr_t)pci_bar(&g_pci, 0);
    if (!gM) return 0;

    cap_lo = r32(R_CAP); cap_hi = r32(R_CAP + 4);
    (void)cap_lo;
    gDbShift = 2 + ((cap_hi >> 0) & 0xF);          /* CAP.DSTRD (bits 35:32)  */

    w32(R_CC, r32(R_CC) & ~CC_EN);                 /* disable                 */
    for (i = 0; i < SPIN_MAX && (r32(R_CSTS) & 1); i++) { }
    if (r32(R_CSTS) & 1) return 0;

    memset(gAsq, 0, sizeof gAsq); memset(gAcq, 0, sizeof gAcq);
    memset(gIsq, 0, sizeof gIsq); memset(gIcq, 0, sizeof gIcq);
    gSqT[0] = gSqT[1] = gCqH[0] = gCqH[1] = 0;
    gPhase[0] = gPhase[1] = 1;

    w32(R_AQA, ((QDEPTH - 1) << 16) | (QDEPTH - 1));
    w64(R_ASQ, (uint64_t)(uintptr_t)gAsq);
    w64(R_ACQ, (uint64_t)(uintptr_t)gAcq);
    w32(R_CC, CC_IOSQES | CC_IOCQES | CC_EN);      /* CSS=NVM, MPS=4K, round-robin */
    for (i = 0; i < SPIN_MAX && !(r32(R_CSTS) & 1); i++) { }
    if (!(r32(R_CSTS) & 1) || (r32(R_CSTS) & 2)) return 0;   /* !RDY or CFS   */
    gPresent = 1;

    if (!identify(1, 0)) return 0;                 /* controller data page    */
    nn = *(uint32_t *)(gIdBuf + 516);              /* namespace count         */
    {   uint8_t mdts = gIdBuf[77];                 /* max transfer, 2^n pages */
        if (mdts && mdts < 4)                      /* < our 64 KB chunks      */
            gChunk = (PAGE << mdts) / 512 / 2;     /* worst-case offset: halve */
    }

    /* ask for one I/O queue pair (feature 07h; 0-based counts) */
    {   sqe c; memset(&c, 0, sizeof c);
        c.dw[0] = A_SETFEAT; c.dw[10] = 0x07; c.dw[11] = 0;
        admin(&c, 0);                              /* default grants >= 1     */
    }
    {   sqe c; memset(&c, 0, sizeof c);            /* I/O CQ first            */
        c.dw[0]  = A_CREATE_CQ;
        c.dw[6]  = (uint32_t)(uintptr_t)gIcq;
        c.dw[7]  = (uint32_t)((uint64_t)(uintptr_t)gIcq >> 32);
        c.dw[10] = ((QDEPTH - 1) << 16) | 1;       /* qid 1                   */
        c.dw[11] = 1;                              /* physically contiguous   */
        if (!admin(&c, 0)) return 0;
    }
    {   sqe c; memset(&c, 0, sizeof c);            /* then its I/O SQ         */
        c.dw[0]  = A_CREATE_SQ;
        c.dw[6]  = (uint32_t)(uintptr_t)gIsq;
        c.dw[7]  = (uint32_t)((uint64_t)(uintptr_t)gIsq >> 32);
        c.dw[10] = ((QDEPTH - 1) << 16) | 1;       /* qid 1                   */
        c.dw[11] = (1 << 16) | 1;                  /* on CQ 1, contiguous     */
        if (!admin(&c, 0)) return 0;
    }

    /* namespaces: 1..NN, register every 512-byte-LBA one */
    for (i = 1; i <= nn && nns < MAXNS && uno_blk_count() < 12; i++) {
        unsigned long long nsze; unsigned flbas, lbads; uno_bdev b;
        if (!identify(0, i)) continue;             /* inactive ns fails: skip */
        nsze  = *(uint64_t *)(gIdBuf + 0);
        if (!nsze) continue;
        flbas = gIdBuf[26] & 0xF;
        lbads = gIdBuf[128 + 4 * flbas + 2];       /* LBA format: data size   */
        if (lbads != 9) continue;                  /* 512-byte sectors only   */
        memset(&b, 0, sizeof b);
        b.native  = 1;
        b.sectors = nsze;
        b.name[0]='n'; b.name[1]='v'; b.name[2]='m'; b.name[3]='e';
        b.name[4]=(char)('0'+nns); b.name[5]=0;
        b.pci_dev = g_pci.dev; b.pci_fn = g_pci.fn;
        b.ctx     = (void *)(uintptr_t)i;          /* nsid                    */
        b.read    = drv_read;
        b.write   = drv_write;
        g_ns[nns].nsid = i; g_ns[nns].sectors = nsze;
        if (uno_blk_register(&b)) { nns++; made++; }
    }
    return made;
}
