/* ide.c - legacy ATA/IDE, PIO mode, for machines that predate AHCI.
 *
 * WHY THIS EXISTS. pc64's storage stack is AHCI, NVMe and SDHCI, and every one
 * of those postdates the hardware phase D of docs/BIOS-BOOT-PLAN.md targets.
 * ICH5 and earlier have no AHCI at all, and a great many ICH6-ICH9 boards ship
 * with the SATA controller in "IDE" / "compatibility" mode in their firmware
 * defaults - which is also the mode a CSM boot tends to leave it in. On those
 * machines the disk is reachable only through the ATA task-file interface, and
 * without this file the OS boots to a desktop and cannot see its own volume.
 *
 * PIO, NOT DMA, DELIBERATELY. Bus-master DMA needs a PRD table, a bus-master
 * base address and an interrupt or a status-poll dance, and buys throughput
 * this OS does not need: the load is module files and documents, not streaming.
 * PIO with 16-bit string I/O is ~5-15 MB/s, entirely adequate, and it is the
 * one path that works identically on a PIIX3 from 1996 and an ICH9 in
 * compatibility mode. Correct and boring beats fast and untestable on hardware
 * that is hard to come by.
 *
 * NO INTERRUPTS. Everything here polls the status register with a TSC-bounded
 * timeout. The OS has no ATA ISR, the boot path runs with interrupts off in
 * places, and a driver that needs IRQ14 to make progress would deadlock exactly
 * where it is least debuggable. Every wait can time out; none can hang.
 *
 * Registered through the same uno_bdev seam as ahci.c, so unofs, the installer
 * and unostorage need no knowledge of it.
 */
#include "blkdev.h"
#include "pc64_pci.h"
#include "pc64_native.h"
#include <string.h>

/* ---- task-file registers, as offsets from the command block base ---------- */
#define R_DATA      0
#define R_ERR       1           /* read: error; write: features               */
#define R_COUNT     2
#define R_LBA0      3
#define R_LBA1      4
#define R_LBA2      5
#define R_DRIVE     6
#define R_STATUS    7           /* read: status; write: command               */

/* status bits */
#define ST_ERR      0x01
#define ST_DRQ      0x08
#define ST_DF       0x20
#define ST_RDY      0x40
#define ST_BSY      0x80

#define CMD_READ_PIO      0x20  /* LBA28 */
#define CMD_WRITE_PIO     0x30
#define CMD_READ_PIO48    0x24
#define CMD_WRITE_PIO48   0x34
#define CMD_FLUSH         0xE7
#define CMD_FLUSH48       0xEA
#define CMD_IDENTIFY      0xEC

#define MAXDRV 4                /* two channels x master/slave               */

typedef struct {
    unsigned short cmd;         /* command block base (0x1F0 / 0x170 / BAR)  */
    unsigned short ctl;         /* control block base (cmd + 0x206 legacy)   */
    unsigned char  slave;       /* 0 = master, 1 = slave                     */
    unsigned char  lba48;
    unsigned long long sectors;
} drive;

static drive g_drv[MAXDRV];
static int   g_ndrv;
static int   g_present;

static inline unsigned char  inb_(unsigned short p)
{ unsigned char v; __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outb_(unsigned short p, unsigned char v)
{ __asm__ volatile ("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline void insw_(unsigned short p, void *buf, unsigned n)
{ __asm__ volatile ("rep insw" : "+D"(buf), "+c"(n) : "d"(p) : "memory"); }
static inline void outsw_(unsigned short p, const void *buf, unsigned n)
{ __asm__ volatile ("rep outsw" : "+S"(buf), "+c"(n) : "d"(p)); }

/* A 400 ns settle after a drive-select or a command write, the interval the ATA
 * spec requires before BSY is meaningful. Four reads of the ALTERNATE status
 * register is the canonical way to spend it: alternate status has no side
 * effects, where reading the primary status register acknowledges the
 * interrupt we are deliberately not using. */
static void settle(drive *d)
{
    int i;
    for (i = 0; i < 4; i++) (void)inb_(d->ctl);
}

/* Wait for BSY to clear, then for one of the wanted bits. Bounded in real time
 * by the TSC, so a missing or wedged drive costs milliseconds, not the boot. */
static int wait_ready(drive *d, unsigned char want, unsigned timeout_ms)
{
    unsigned long long deadline_us = (unsigned long long)timeout_ms * 1000ull;
    unsigned long long spent = 0;
    for (;;) {
        unsigned char st = inb_(d->cmd + R_STATUS);
        if (st == 0xFF) return 0;               /* floating bus: nothing there */
        if (!(st & ST_BSY)) {
            if (st & (ST_ERR | ST_DF)) return 0;
            if (!want || (st & want)) return 1;
        }
        uno_native_delay_us(20);
        spent += 20;
        if (spent > deadline_us) return 0;
    }
}

static void select_drive(drive *d, unsigned char head_bits)
{
    outb_(d->cmd + R_DRIVE,
          (unsigned char)(0xE0 | (d->slave ? 0x10 : 0) | (head_bits & 0x0F)));
    settle(d);
}

/* ---- one PIO transfer ------------------------------------------------------
 * `n` sectors, at most 256 (LBA28) or 65536 (LBA48) per command; the callers
 * below chunk to that. Each sector is its own DRQ handshake - the drive raises
 * DRQ per block and the spec does not permit assuming otherwise, however many
 * were asked for.
 */
static int pio_xfer(drive *d, unsigned long long lba, unsigned int n,
                    void *buf, int write)
{
    unsigned char *p = (unsigned char *)buf;
    unsigned int i;

    if (!d->cmd || !n) return 0;

    if (d->lba48) {
        select_drive(d, 0);
        outb_(d->cmd + R_COUNT, (unsigned char)((n >> 8) & 0xFF));
        outb_(d->cmd + R_LBA0,  (unsigned char)((lba >> 24) & 0xFF));
        outb_(d->cmd + R_LBA1,  (unsigned char)((lba >> 32) & 0xFF));
        outb_(d->cmd + R_LBA2,  (unsigned char)((lba >> 40) & 0xFF));
        outb_(d->cmd + R_COUNT, (unsigned char)(n & 0xFF));
        outb_(d->cmd + R_LBA0,  (unsigned char)(lba & 0xFF));
        outb_(d->cmd + R_LBA1,  (unsigned char)((lba >> 8) & 0xFF));
        outb_(d->cmd + R_LBA2,  (unsigned char)((lba >> 16) & 0xFF));
        outb_(d->cmd + R_STATUS, write ? CMD_WRITE_PIO48 : CMD_READ_PIO48);
    } else {
        if (lba > 0x0FFFFFFFull) return 0;
        select_drive(d, (unsigned char)((lba >> 24) & 0x0F));
        outb_(d->cmd + R_COUNT, (unsigned char)(n & 0xFF));   /* 256 -> 0 */
        outb_(d->cmd + R_LBA0,  (unsigned char)(lba & 0xFF));
        outb_(d->cmd + R_LBA1,  (unsigned char)((lba >> 8) & 0xFF));
        outb_(d->cmd + R_LBA2,  (unsigned char)((lba >> 16) & 0xFF));
        outb_(d->cmd + R_STATUS, write ? CMD_WRITE_PIO : CMD_READ_PIO);
    }
    settle(d);

    for (i = 0; i < n; i++) {
        if (!wait_ready(d, ST_DRQ, 3000)) return 0;
        if (write) outsw_(d->cmd + R_DATA, p, 256);
        else       insw_ (d->cmd + R_DATA, p, 256);
        p += 512;
        /* A write needs the drive to finish the block before the next DRQ; the
         * settle keeps us from reading a status that is still the last one. */
        if (write) settle(d);
    }
    if (write) {
        outb_(d->cmd + R_STATUS, d->lba48 ? CMD_FLUSH48 : CMD_FLUSH);
        settle(d);
        if (!wait_ready(d, 0, 30000)) return 0;   /* a flush can be slow */
    }
    return 1;
}

/* LBA28 carries at most 256 sectors per command and LBA48 at most 65536; both
 * encode "the maximum" as a zero count, so the chunk size is capped below that
 * rather than at it - a 256-sector chunk written as 0 is correct but one more
 * special case to get wrong, and 128 KB per command is already far past the
 * point where the per-command overhead matters. */
#define CHUNK 128u

static int drv_read(struct uno_bdev *b, unsigned long long lba,
                    unsigned int n, void *buf)
{
    drive *d = (drive *)b->ctx;
    unsigned char *p = (unsigned char *)buf;
    while (n) {
        unsigned int k = n > CHUNK ? CHUNK : n;
        if (!pio_xfer(d, lba, k, p, 0)) return 0;
        lba += k; n -= k; p += (unsigned long)k * 512u;
    }
    return 1;
}

static int drv_write(struct uno_bdev *b, unsigned long long lba,
                     unsigned int n, const void *buf)
{
    drive *d = (drive *)b->ctx;
    const unsigned char *p = (const unsigned char *)buf;
    while (n) {
        unsigned int k = n > CHUNK ? CHUNK : n;
        if (!pio_xfer(d, lba, k, (void *)(unsigned long long)p, 1)) return 0;
        lba += k; n -= k; p += (unsigned long)k * 512u;
    }
    return 1;
}

/* ---- probe one drive ------------------------------------------------------- */
static int identify(drive *d, unsigned short *w)
{
    unsigned char st;

    select_drive(d, 0);
    /* Zero the task file before IDENTIFY: a non-zero count/LBA is how you ask
     * a PACKET device (an ATAPI CD-ROM) to reply with garbage instead of
     * signature. */
    outb_(d->cmd + R_COUNT, 0);
    outb_(d->cmd + R_LBA0, 0);
    outb_(d->cmd + R_LBA1, 0);
    outb_(d->cmd + R_LBA2, 0);
    outb_(d->cmd + R_STATUS, CMD_IDENTIFY);
    settle(d);

    st = inb_(d->cmd + R_STATUS);
    if (st == 0 || st == 0xFF) return 0;        /* no device on this position */

    /* An ATAPI device answers IDENTIFY with an abort and its signature in the
     * LBA mid/high registers. We do not do optical media, so decline rather
     * than mis-register a CD-ROM as a disk with a nonsense capacity. */
    if (!wait_ready(d, ST_DRQ, 2000)) return 0;
    if (inb_(d->cmd + R_LBA1) || inb_(d->cmd + R_LBA2)) return 0;

    insw_(d->cmd + R_DATA, w, 256);
    return 1;
}

static int add_drive(unsigned short cmd_base, unsigned short ctl_base,
                     int slave, int chan)
{
    static unsigned short id[256];
    drive *d;
    uno_bdev b;
    unsigned long long sectors;

    if (g_ndrv >= MAXDRV) return 0;
    d = &g_drv[g_ndrv];
    memset(d, 0, sizeof *d);
    d->cmd = cmd_base;
    d->ctl = ctl_base;
    d->slave = (unsigned char)slave;

    outb_(d->ctl, 0x02);                        /* nIEN: interrupts off, we poll */

    if (!identify(d, id)) return 0;

    sectors = ((unsigned long long)id[103] << 48) | ((unsigned long long)id[102] << 32)
            | ((unsigned long long)id[101] << 16) | id[100];      /* LBA48 */
    if (sectors && (id[83] & (1u << 10))) {
        d->lba48 = 1;
    } else {
        sectors = ((unsigned long)id[61] << 16) | id[60];         /* LBA28 */
        d->lba48 = 0;
    }
    if (!sectors) return 0;
    d->sectors = sectors;

    memset(&b, 0, sizeof b);
    b.native  = 1;
    b.sectors = sectors;
    b.name[0]='i'; b.name[1]='d'; b.name[2]='e';
    b.name[3]=(char)('0' + chan); b.name[4]=(char)('0' + slave); b.name[5]=0;
    b.pci_dev = -1; b.pci_fn = -1;
    b.ctx     = d;
    b.read    = drv_read;
    b.write   = drv_write;
    if (!uno_blk_register(&b)) return 0;
    g_ndrv++;
    return 1;
}

/* ---- entry ------------------------------------------------------------------
 * Both legacy channels, master and slave on each.
 *
 * THE PORTS ARE PROBED WHETHER OR NOT PCI ADMITS TO AN IDE CONTROLLER. A
 * controller in legacy/compatibility mode responds at the fixed ISA addresses
 * and may report a native-mode BAR of zero, and some early chipsets do not
 * present a recognisable class code at all. wait_ready() already treats a
 * floating bus (0xFF) as "nothing there", so probing an empty address costs a
 * few microseconds and cannot wedge - which makes the unconditional probe
 * cheaper than the class-code test it would replace.
 */
int uno_ide_init(void)
{
    int made = 0;
    if (g_present) return g_ndrv;
    g_present = 1;

    /* A PCI IDE controller must be bus-master-enabled and have I/O decode on
     * before its task file answers; on a legacy-mode controller this is
     * usually already done by firmware, but not always after a CSM boot. */
    {
        pci_dev p;
        if (pci_find_class(0x01, 0x01, &p)) pci_enable_bus_master(&p);
    }

    made += add_drive(0x1F0, 0x3F6, 0, 0);
    made += add_drive(0x1F0, 0x3F6, 1, 0);
    made += add_drive(0x170, 0x376, 0, 1);
    made += add_drive(0x170, 0x376, 1, 1);
    return made;
}

int uno_ide_present(void) { return g_ndrv > 0; }
