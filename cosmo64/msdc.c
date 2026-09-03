/* cosmo64/msdc.c -- the Cosmo's eMMC as a block device (M3).
 *
 * MSDC0 at 0x11230000 (msdc@11230000 in the device's own DTB), driving an
 * SK hynix H9HP16AECMMDAR, 128 GB, eMMC 5.1.
 *
 * THE SHORTCUT, and it is the same one the framebuffer took: LK has already
 * done the hard part. It brought MSDC0 up, ran the card through its init
 * sequence, assigned it an RCA, picked a bus width and a speed mode, tuned the
 * pads, and then read our own boot image off p38 with it -- so the controller
 * is live and the card is sitting in the transfer state at the instant LK
 * branches to us. And it stays that way: LK's platform_uninit() (mt6771/
 * platform.c) does leds_deinit(), platform_clear_all_on_mux() and
 * platform_deinit_interrupts(), and NOTHING else. It never calls msdc_deinit(),
 * never gates the MSDC clock, never deselects the card.
 *
 * So this is not an eMMC bring-up. It is a command issuer: point the existing
 * controller at a block and read it. No clock tree, no pinmux, no voltage
 * negotiation, no CMD0/1/2/3/9/7 dance, no tuning -- and, deliberately, no
 * controller reset (MSDC_CFG_RST), which would throw away the very tuning that
 * makes the adopted state worth having.
 *
 * PIO, not DMA. The transfers here are a GPT header and, later, session files:
 * small and rare. PIO costs one FIFO loop and no descriptor memory, and it
 * cannot scribble on DRAM if a register field is wrong -- which matters on a
 * device where the failure mode of a wild DMA write is a brick.
 *
 * Register map (offsets and bit positions) is the MT6771 MSDC programming
 * interface, taken from the hardware's own definitions; the code below is
 * this project's. Nothing is copied from MediaTek's LK sources, which are
 * proprietary and license-incompatible with UnoDOS.
 *
 * WRITES ARE GATED. c64_blk_write() refuses any LBA outside the UnoDOS data
 * partition found by the GPT walk. Everything else on this eMMC is somebody
 * else's: Android's system and vendor, Gemian's rootfs, the GPT itself and --
 * the one that does not come back -- the preloader. A block driver on this
 * device is a brick generator unless it is fenced, so it is fenced here, at
 * the bottom, where nothing can route around it.
 */

#include "cosmo64.h"

#define MSDC0 0x11230000ull
#define R32(off) (*(volatile c64_u32 *)(MSDC0 + (off)))

/* register offsets */
#define MSDC_CFG        0x00
#define MSDC_IOCON      0x04
#define MSDC_PS         0x08
#define MSDC_INT        0x0C
#define MSDC_INTEN      0x10
#define MSDC_FIFOCS     0x14
#define MSDC_TXDATA     0x18
#define MSDC_RXDATA     0x1C
#define SDC_CFG         0x30
#define SDC_CMD         0x34
#define SDC_ARG         0x38
#define SDC_STS         0x3C
#define SDC_RESP0       0x40
#define SDC_BLK_NUM     0x50
#define MSDC_VERSION    0x114

/* MSDC_CFG */
#define CFG_PIO         (1u << 3)

/* MSDC_FIFOCS */
#define FIFOCS_RXCNT    0xFFu
#define FIFOCS_TXCNT    (0xFFu << 16)
#define FIFOCS_CLR      (1u << 31)

/* MSDC_INT (write-1-to-clear) */
#define INT_CMDRDY      (1u << 8)
#define INT_CMDTMO      (1u << 9)
#define INT_RSPCRCERR   (1u << 10)
#define INT_XFER_COMPL  (1u << 12)
#define INT_DATTMO      (1u << 14)
#define INT_DATCRCERR   (1u << 15)

/* SDC_CMD fields */
#define CMD_OPC(x)      ((c64_u32)(x) & 0x3F)
#define CMD_RSPTYP(x)   (((c64_u32)(x) & 7u) << 7)
#define CMD_DTYP(x)     (((c64_u32)(x) & 3u) << 11)
#define CMD_RW_WRITE    (1u << 13)
#define CMD_BLKLEN(x)   (((c64_u32)(x) & 0xFFFu) << 16)

#define RSP_NONE 0
#define RSP_R1   1
#define DTYP_NONE   0
#define DTYP_SINGLE 1

/* SDC_STS */
#define STS_SDCBUSY     (1u << 0)
#define STS_CMDBUSY     (1u << 1)

#define BLKSZ 512u

/* eMMC commands used here */
#define MMC_READ_SINGLE  17
#define MMC_WRITE_SINGLE 24

static int g_ready;
static c64_u64 g_data_lba, g_data_sectors;
static c64_u8 g_scratch[BLKSZ];

/* The log window inside our OWN boot partition. p38 (UNODOS) is 32 MiB and the
 * boot image is 512 KiB, so everything past it is ours and nothing else on the
 * device touches it. Sit 2 MiB in, four times clear of the image, and take
 * 128 KiB. Both numbers are offsets from the partition's GPT-discovered first
 * LBA -- never a hardcoded absolute block. */
#define LOG_OFF_SECTORS 4096u            /* 2 MiB into p38                  */
#define LOG_SECTORS     256u             /* 128 KiB, header block included  */
#define LOG_MAGIC0 0x4F4E5528u           /* "(UNO" -- byte order irrelevant */
#define LOG_MAGIC1 0x29474F4Cu           /* "LOG)"                          */

static c64_u64 g_boot_lba, g_boot_sectors;   /* p38, from the GPT walk */
static c64_u64 g_log_lba;
static unsigned g_flushed;

int c64_blk_ready(void)
{
    return g_ready;
}

c64_u64 c64_blk_data_lba(void)
{
    return g_data_lba;
}

c64_u64 c64_blk_data_sectors(void)
{
    return g_data_sectors;
}

/* ---- timing -------------------------------------------------------------- */

static c64_u64 deadline_ms(unsigned ms)
{
    c64_u64 f = c64_cnt_freq();
    if (!f)
        f = 13000000ull;
    return c64_cnt_now() + (f / 1000ull) * ms;
}

static int expired(c64_u64 at)
{
    return c64_cnt_now() > at;
}

/* ---- the command issuer -------------------------------------------------- */

static int wait_idle(void)
{
    c64_u64 t = deadline_ms(500);
    while (R32(SDC_STS) & (STS_SDCBUSY | STS_CMDBUSY))
        if (expired(t)) {
            c64_logf("msdc: controller stuck busy, SDC_STS=%08x\n", R32(SDC_STS));
            return -1;
        }
    return 0;
}

static void fifo_clear(void)
{
    R32(MSDC_FIFOCS) |= FIFOCS_CLR;
    c64_u64 t = deadline_ms(100);
    while (R32(MSDC_FIFOCS) & (FIFOCS_RXCNT | FIFOCS_TXCNT))
        if (expired(t))
            return;
}

/* Issue one command and wait for its response. Data, if any, is moved by the
 * caller between this and wait_xfer(). */
static int send_cmd(c64_u32 opc, c64_u32 arg, c64_u32 rsptyp, c64_u32 dtyp,
                    c64_u32 write, c64_u32 blklen)
{
    if (wait_idle() < 0)
        return -1;
    R32(MSDC_INT) = 0xFFFFFFFFu;                /* W1C: start from clean */
    fifo_clear();

    if (dtyp != DTYP_NONE)
        R32(SDC_BLK_NUM) = 1;
    R32(SDC_ARG) = arg;
    R32(SDC_CMD) = CMD_OPC(opc) | CMD_RSPTYP(rsptyp) | CMD_DTYP(dtyp)
                 | (write ? CMD_RW_WRITE : 0u) | CMD_BLKLEN(blklen);

    if (rsptyp == RSP_NONE)
        return 0;
    c64_u64 t = deadline_ms(500);
    for (;;) {
        c64_u32 st = R32(MSDC_INT);
        if (st & INT_CMDRDY) {
            R32(MSDC_INT) = INT_CMDRDY;
            return 0;
        }
        if (st & (INT_CMDTMO | INT_RSPCRCERR)) {
            R32(MSDC_INT) = INT_CMDTMO | INT_RSPCRCERR;
            c64_logf("msdc: CMD%d arg=%08x failed (%s)\n", (int)opc, arg,
                     (st & INT_CMDTMO) ? "timeout" : "response CRC");
            return -1;
        }
        if (expired(t)) {
            c64_logf("msdc: CMD%d arg=%08x no response, INT=%08x SDC_STS=%08x\n",
                     (int)opc, arg, st, R32(SDC_STS));
            return -1;
        }
    }
}

static int wait_xfer(void)
{
    c64_u64 t = deadline_ms(2000);
    for (;;) {
        c64_u32 st = R32(MSDC_INT);
        if (st & INT_XFER_COMPL) {
            R32(MSDC_INT) = INT_XFER_COMPL;
            return 0;
        }
        if (st & (INT_DATTMO | INT_DATCRCERR)) {
            R32(MSDC_INT) = INT_DATTMO | INT_DATCRCERR;
            c64_logf("msdc: data phase failed (%s)\n",
                     (st & INT_DATTMO) ? "timeout" : "data CRC");
            return -1;
        }
        if (expired(t)) {
            c64_logf("msdc: data phase hung, INT=%08x FIFOCS=%08x\n",
                     st, R32(MSDC_FIFOCS));
            return -1;
        }
    }
}

/* ---- one block in, one block out ---------------------------------------- */
/* Byte-at-a-time into the caller's buffer: -mstrict-align is on for every file
 * in this build, and a caller's buffer has no alignment guarantee. */

static int read_block(c64_u64 lba, c64_u8 *dst)
{
    if (send_cmd(MMC_READ_SINGLE, (c64_u32)lba, RSP_R1, DTYP_SINGLE, 0, BLKSZ) < 0)
        return -1;

    unsigned got = 0;
    c64_u64 t = deadline_ms(2000);
    while (got < BLKSZ) {
        if ((R32(MSDC_FIFOCS) & FIFOCS_RXCNT) < 4) {
            if (R32(MSDC_INT) & (INT_DATTMO | INT_DATCRCERR))
                break;                          /* wait_xfer reports it */
            if (expired(t)) {
                c64_logf("msdc: RX stalled at %d/%d bytes\n", (int)got, BLKSZ);
                return -1;
            }
            continue;
        }
        c64_u32 w = R32(MSDC_RXDATA);
        dst[got + 0] = (c64_u8)w;
        dst[got + 1] = (c64_u8)(w >> 8);
        dst[got + 2] = (c64_u8)(w >> 16);
        dst[got + 3] = (c64_u8)(w >> 24);
        got += 4;
    }
    return wait_xfer();
}

static int write_block(c64_u64 lba, const c64_u8 *src)
{
    if (send_cmd(MMC_WRITE_SINGLE, (c64_u32)lba, RSP_R1, DTYP_SINGLE, 1, BLKSZ) < 0)
        return -1;

    unsigned put = 0;
    c64_u64 t = deadline_ms(2000);
    while (put < BLKSZ) {
        /* TXCNT counts bytes already queued; the FIFO is 128 bytes deep. */
        if (((R32(MSDC_FIFOCS) & FIFOCS_TXCNT) >> 16) > (128u - 4u)) {
            if (expired(t)) {
                c64_logf("msdc: TX stalled at %d/%d bytes\n", (int)put, BLKSZ);
                return -1;
            }
            continue;
        }
        c64_u32 w = (c64_u32)src[put]
                  | ((c64_u32)src[put + 1] << 8)
                  | ((c64_u32)src[put + 2] << 16)
                  | ((c64_u32)src[put + 3] << 24);
        R32(MSDC_TXDATA) = w;
        put += 4;
    }
    return wait_xfer();
}

/* ---- the public block interface ------------------------------------------ */

int c64_blk_read(c64_u64 lba, void *buf, unsigned nblk)
{
    if (!g_ready)
        return -1;
    c64_u8 *p = (c64_u8 *)buf;
    for (unsigned i = 0; i < nblk; i++)
        if (read_block(lba + i, p + (c64_u64)i * BLKSZ) < 0)
            return -1;
    return 0;
}

int c64_blk_write(c64_u64 lba, const void *buf, unsigned nblk)
{
    if (!g_ready)
        return -1;
    /* THE FENCE. Everything else on this eMMC belongs to Android, to Gemian,
     * to the GPT, or to the preloader, and the preloader does not come back.
     * A write outside our own partitions is a bug in the caller, and it is
     * refused here rather than trusted anywhere above. Two windows are legal:
     * the data partition, and the log window inside our own boot slot. */
    int in_data = g_data_sectors && lba >= g_data_lba
                  && lba + nblk <= g_data_lba + g_data_sectors;
    int in_log = g_log_lba && lba >= g_log_lba
                 && lba + nblk <= g_log_lba + LOG_SECTORS;
    if (!in_data && !in_log) {
        c64_logf("msdc: REFUSED write of %d blocks at LBA %d -- outside both "
                 "the data partition (%d..%d) and the log window (%d..%d)\n",
                 (int)nblk, (int)lba, (int)g_data_lba,
                 (int)(g_data_lba + g_data_sectors), (int)g_log_lba,
                 (int)(g_log_lba + LOG_SECTORS));
        return -1;
    }
    const c64_u8 *p = (const c64_u8 *)buf;
    for (unsigned i = 0; i < nblk; i++)
        if (write_block(lba + i, p + (c64_u64)i * BLKSZ) < 0)
            return -1;
    return 0;
}

/* ---- little-endian fetches (strict-align safe) --------------------------- */

static c64_u32 le32(const c64_u8 *p)
{
    return (c64_u32)p[0] | ((c64_u32)p[1] << 8) | ((c64_u32)p[2] << 16)
         | ((c64_u32)p[3] << 24);
}

static c64_u64 le64(const c64_u8 *p)
{
    return (c64_u64)le32(p) | ((c64_u64)le32(p + 4) << 32);
}

/* GPT names are UTF-16LE; ours are ASCII, so compare the low bytes and require
 * the high bytes to be zero. */
static int name_is(const c64_u8 *nm, const char *want)
{
    for (int i = 0; i < 36; i++) {
        c64_u32 ch = (c64_u32)nm[i * 2] | ((c64_u32)nm[i * 2 + 1] << 8);
        c64_u8 w = (c64_u8)want[i];
        if (ch != w)
            return 0;
        if (!w)
            return 1;
    }
    return 1;
}

/* ---- the GPT walk -------------------------------------------------------- */

static int find_data_partition(void)
{
    if (read_block(1, g_scratch) < 0) {
        c64_log("msdc: could not read the GPT header at LBA 1\n");
        return -1;
    }
    static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    for (int i = 0; i < 8; i++)
        if (g_scratch[i] != (c64_u8)sig[i]) {
            c64_logf("msdc: LBA 1 is not a GPT header (%02x %02x %02x %02x "
                     "%02x %02x %02x %02x)\n", g_scratch[0], g_scratch[1],
                     g_scratch[2], g_scratch[3], g_scratch[4], g_scratch[5],
                     g_scratch[6], g_scratch[7]);
            return -1;
        }

    c64_u64 ent_lba = le64(g_scratch + 72);
    c64_u32 n_ent = le32(g_scratch + 80);
    c64_u32 ent_sz = le32(g_scratch + 84);
    c64_logf("msdc: GPT at LBA %d, %d entries of %d bytes\n",
             (int)ent_lba, (int)n_ent, (int)ent_sz);
    if (ent_sz < 128 || ent_sz > BLKSZ || n_ent > 256)
        return -1;

    /* "UNODATA" is the name the port plan gives our partition, and it is the
     * ONLY name this walk will hand out as writable. Android's "userdata"
     * (p44, 27.7 GiB) used to be accepted as a fallback while the plan was to
     * take it over; that plan was reversed on 2026-09-01 (p44 stays Android's,
     * persistence goes to the SD card), and a fallback that outlives the
     * decision is a formatted p44 the first time anything upstream asks for a
     * writable disk -- URC's mkfs/prepdisk verbs will, over the LAN. So p44 is
     * noticed and logged, and never offered. */
    c64_u64 uno_lba = 0, uno_sectors = 0;        /* UNODATA, if present  */
    c64_u64 ud_lba = 0, ud_sectors = 0;          /* Android userdata: log only */
    unsigned per_blk = BLKSZ / ent_sz;
    for (c64_u32 i = 0; i < n_ent; i += per_blk) {
        if (read_block(ent_lba + i / per_blk, g_scratch) < 0)
            return -1;
        for (unsigned j = 0; j < per_blk && i + j < n_ent; j++) {
            const c64_u8 *e = g_scratch + j * ent_sz;
            c64_u64 first = le64(e + 32), last = le64(e + 40);
            if (!first || last < first)
                continue;                        /* unused entry */
            const c64_u8 *nm = e + 56;
            if (name_is(nm, "UNODATA")) {
                uno_lba = first;
                uno_sectors = last - first + 1;
            } else if (name_is(nm, "userdata")) {
                ud_lba = first;
                ud_sectors = last - first + 1;
            } else if (name_is(nm, "UNODOS")) {
                g_boot_lba = first;              /* our own boot slot */
                g_boot_sectors = last - first + 1;
            }
        }
    }

    /* The log window lives in our own boot partition, which is why it is
     * derived from the GPT rather than written down: p38 is 32 MiB and the
     * image is 512 KiB, so 2 MiB in is ours by a wide margin. */
    if (g_boot_sectors > LOG_OFF_SECTORS + LOG_SECTORS) {
        g_log_lba = g_boot_lba + LOG_OFF_SECTORS;
        c64_logf("msdc: UNODOS boot slot at LBA %d (%d sectors); log window "
                 "LBA %d, %d sectors\n", (int)g_boot_lba, (int)g_boot_sectors,
                 (int)g_log_lba, LOG_SECTORS);
    } else {
        c64_log("msdc: no UNODOS partition found -- no eMMC log\n");
    }

    if (uno_sectors) {
        g_data_lba = uno_lba;
        g_data_sectors = uno_sectors;
        c64_logf("msdc: UNODATA at LBA %d, %d sectors (%d MiB)\n",
                 (int)uno_lba, (int)uno_sectors, (int)(uno_sectors / 2048));
        return 0;
    }
    if (ud_sectors)
        c64_logf("msdc: Android userdata at LBA %d, %d sectors (%d MiB) -- "
                 "LEFT ALONE (not a UnoDOS partition; persistence is the SD "
                 "card's job)\n", (int)ud_lba, (int)ud_sectors,
                 (int)(ud_sectors / 2048));
    c64_log("msdc: no UNODATA partition -- data writes stay refused\n");
    return -1;
}

/* ---- the eMMC log sink --------------------------------------------------- */
/* The reason this exists: the ramoops channel in log.c never reaches pstore on
 * this device (cause still open -- DRAM itself demonstrably survives the
 * reset, see log.c's header), and the eMMC is readable from any later Linux
 * boot. p38's unused tail is ours. Block 0 of the window is a header; the
 * text follows. readlog.sh dd's it straight back out.
 *
 * Called from the poll loop when the log has grown, and from the fault
 * handler, which is the case that matters: a payload that dies still leaves
 * its whole log somewhere a later Linux boot can read. */

void c64_log_flush(void)
{
    if (!g_ready || !g_log_lba)
        return;
    unsigned total = c64_log_bytes();
    if (total == g_flushed)
        return;                                  /* nothing new to say */

    unsigned cap = (LOG_SECTORS - 1u) * BLKSZ;
    unsigned from = 0, len = total;
    if (len > cap) {                             /* keep the TAIL, not the head */
        from = len - cap;
        len = cap;
    }

    /* header block: magic, byte count, and the offset the text starts at, so a
     * reader can tell a truncated log from a whole one */
    for (unsigned i = 0; i < BLKSZ; i++)
        g_scratch[i] = 0;
    g_scratch[0] = (c64_u8)LOG_MAGIC0;
    g_scratch[1] = (c64_u8)(LOG_MAGIC0 >> 8);
    g_scratch[2] = (c64_u8)(LOG_MAGIC0 >> 16);
    g_scratch[3] = (c64_u8)(LOG_MAGIC0 >> 24);
    g_scratch[4] = (c64_u8)LOG_MAGIC1;
    g_scratch[5] = (c64_u8)(LOG_MAGIC1 >> 8);
    g_scratch[6] = (c64_u8)(LOG_MAGIC1 >> 16);
    g_scratch[7] = (c64_u8)(LOG_MAGIC1 >> 24);
    g_scratch[8] = (c64_u8)len;
    g_scratch[9] = (c64_u8)(len >> 8);
    g_scratch[10] = (c64_u8)(len >> 16);
    g_scratch[11] = (c64_u8)(len >> 24);
    g_scratch[12] = (c64_u8)from;
    g_scratch[13] = (c64_u8)(from >> 8);
    g_scratch[14] = (c64_u8)(from >> 16);
    g_scratch[15] = (c64_u8)(from >> 24);
    if (c64_blk_write(g_log_lba, g_scratch, 1) < 0)
        return;

    unsigned blocks = (len + BLKSZ - 1u) / BLKSZ;
    for (unsigned b = 0; b < blocks; b++) {
        for (unsigned i = 0; i < BLKSZ; i++)
            g_scratch[i] = 0;
        unsigned n = len - b * BLKSZ;
        if (n > BLKSZ)
            n = BLKSZ;
        c64_log_read(from + b * BLKSZ, g_scratch, n);
        if (c64_blk_write(g_log_lba + 1 + b, g_scratch, 1) < 0)
            return;
    }
    g_flushed = total;
}

/* ---- bring-up ------------------------------------------------------------ */

void c64_blk_init(void)
{
    /* Idempotent: c_main() brings storage up before the shell, and the block
     * registry (blk.c) asks again on its way to mounting, so that neither has
     * to assume it runs first. A second GPT walk would be harmless but is not
     * free, and a second bring-up after a FAILED one would re-run the probe
     * that already timed out. */
    static int g_inited;
    if (g_inited)
        return;
    g_inited = 1;

    c64_logf("msdc: adopting LK's controller: CFG=%08x SDC_CFG=%08x STS=%08x "
             "ver=%08x\n", R32(MSDC_CFG), R32(SDC_CFG), R32(SDC_STS),
             R32(MSDC_VERSION));

    /* The only state we impose on what LK left: PIO rather than DMA, and no
     * interrupts (this driver polls). Notably NOT a controller reset. */
    R32(MSDC_INTEN) = 0;
    R32(MSDC_CFG) |= CFG_PIO;
    R32(MSDC_INT) = 0xFFFFFFFFu;
    fifo_clear();
    g_ready = 1;

    /* The probe IS the proof: LBA 0 of this eMMC is a protective MBR and LBA 1
     * is a GPT header, so a driver that can read them is a driver that works.
     * There is no gate for this on QEMU -- the virt board has no MSDC -- so
     * the log is the gate, and this is what it has to say. */
    if (read_block(0, g_scratch) < 0) {
        c64_log("msdc: LBA 0 unreadable -- eMMC NOT available\n");
        g_ready = 0;
        return;
    }
    c64_logf("msdc: LBA 0 read OK, MBR signature %02x%02x (want 55aa)\n",
             g_scratch[510], g_scratch[511]);
    if (find_data_partition() < 0)
        c64_log("msdc: block reads work, but no writable partition was found\n");
}
