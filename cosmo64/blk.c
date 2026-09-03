/* cosmo64/blk.c -- the eMMC as a pc64 block device (M3b).
 *
 * pc64's storage stack is three layers: blkdev.c owns a registry of 512-byte
 * sector devices, fat.c mounts FAT volumes off whatever is in that registry,
 * and pc64_fs.c presents those volumes (plus the RAM disk) to the shell. Only
 * the bottom layer is machine-specific, and on x86 it is written around EFI
 * Block IO -- so blkdev.c is the one file of the three this port replaces.
 * fat.c and pc64_fs.c compile here unchanged.
 *
 * WHAT GETS REGISTERED, AND WHY IT IS A PARTITION RATHER THAN THE DISK.
 * The obvious shape is "register the whole eMMC and let fat.c walk the GPT",
 * which is what the x86 backend does with a whole-disk handle. It is the wrong
 * shape on this device. Everything on this eMMC except two partitions belongs
 * to somebody else -- Android's system and vendor, Gemian's rootfs, the GPT
 * itself, and the preloader, which does not come back -- and a whole-disk
 * device hands fat.c an address space in which our partition is a small window
 * and every brick is one arithmetic slip away.
 *
 * So the device registered here IS the partition: LBA 0 of `emmc0` is the
 * first sector of the UnoDOS data partition, and its length is that partition.
 * fat.c cannot express an address outside it, which makes the containment a
 * property of the address space rather than of a check somebody has to
 * remember. c64_blk_write()'s fence in msdc.c still stands underneath as the
 * second layer, and the translation here is deliberately the only place the
 * partition's absolute base is added.
 *
 * fat.c probes a device with no partition table as a "superfloppy" (a BPB
 * right at LBA 0), which is exactly what a partition-as-device presents, so
 * this needs no cooperation from the layer above.
 *
 * WHAT IF THE PARTITION IS NOT FORMATTED. Then nothing mounts and the shell
 * runs on the RAM disk, as it did before this file existed. mount_at() wants a
 * BPB with 512-byte sectors and a 0x55AA signature before it will believe a
 * volume, so the encrypted noise the partition ships with reads as "no FAT
 * here" rather than as a garbage filesystem.
 */

#include "blkdev.h"
#include "fat.h"
#include "pc64_fs.h"
#include "string.h"
#include "cosmo64.h"

#define MAXBLK 4

/* the absolute first LBA each registered device's LBA 0 stands for */
typedef struct { c64_u64 base; } c64_part;

static uno_bdev g_dev[MAXBLK];
static c64_part g_part[MAXBLK];
static int      g_ndev;
static int      g_done;

int       uno_blk_count(void) { return g_ndev; }
uno_bdev *uno_blk_get(int i)  { return (i >= 0 && i < g_ndev) ? &g_dev[i] : 0; }

int uno_blk_register(const uno_bdev *dev)
{
    if (g_ndev >= MAXBLK || !dev) return 0;
    g_dev[g_ndev] = *dev;
    g_ndev++;
    return 1;
}

/* ---- partition-relative transport ---------------------------------------- *
 * The registry's convention is 1 for success and 0 for failure; msdc.c's is 0
 * for success and negative for failure. Translate here rather than anywhere a
 * reader would have to remember which of the two they were looking at.
 *
 * The range check is not belt-and-braces: `n` arrives from a BPB read off the
 * medium, and a cluster count that overruns the volume must fail as a read
 * error, not walk off the end of the partition into Gemian's rootfs. */
static int part_read(uno_bdev *d, unsigned long long lba, unsigned int n, void *buf)
{
    const c64_part *p = (const c64_part *)d->ctx;
    if (!p || !n || lba + n > d->sectors || lba + n < lba) return 0;
    return c64_blk_read(p->base + lba, buf, n) == 0;
}

static int part_write(uno_bdev *d, unsigned long long lba, unsigned int n,
                      const void *buf)
{
    const c64_part *p = (const c64_part *)d->ctx;
    if (!p || !n || lba + n > d->sectors || lba + n < lba) return 0;
    return c64_blk_write(p->base + lba, buf, n) == 0;
}

/* The SD card's transport, same shape and same reasoning: LBA 0 of `sd0` is
 * the first sector of the card's FAT partition, so fat.c cannot express an
 * address outside it, and sdmmc.c's own fence stands underneath. */
static int sd_read(uno_bdev *d, unsigned long long lba, unsigned int n, void *buf)
{
    const c64_part *p = (const c64_part *)d->ctx;
    if (!p || !n || lba + n > d->sectors || lba + n < lba) return 0;
    return c64_sd_read(p->base + lba, buf, n) == 0;
}

static int sd_write(uno_bdev *d, unsigned long long lba, unsigned int n,
                    const void *buf)
{
    const c64_part *p = (const c64_part *)d->ctx;
    if (!p || !n || lba + n > d->sectors || lba + n < lba) return 0;
    return c64_sd_write(p->base + lba, buf, n) == 0;
}

/* ---- BLKTEST=1: the same stack over a RAM transport ----------------------- *
 * There is no gate for storage on QEMU, because the virt board has no MSDC --
 * so without this the first thing that ever runs fat.c on aarch64 is the
 * device, and a failure there is a flash, a boot, a reboot and a log read per
 * attempt. Everything above the transport is portable code meeting a new
 * compiler, a new word size and -mstrict-align for the first time, which is
 * exactly the layer worth testing before the hardware loop rather than in it.
 *
 * So: an injectable fake transport, the same idea as the Genesis port's
 * AUTOTEST_BRAM. A RAM-backed device is registered instead of the (absent)
 * eMMC, formatted with the real uno_fat_mkfs, and put through a write / read /
 * verify / delete round trip through the real uno_fs_* calls the shell uses.
 * What that proves is the whole chain minus the eMMC: mkfs geometry, the
 * superfloppy probe, cluster allocation, directory entries, and the fs layer's
 * volume map. What it cannot prove is msdc.c, which the device proved already.
 *
 * Compiled ONLY under -DC64_BLKTEST (BLKTEST=1 ./build.sh shell), because the
 * disk is 36 MiB of .bss -- fine on QEMU, and never in a shipped image. */
#ifdef C64_BLKTEST
/* 36 MiB. uno_fat_mkfs makes genuine FAT32 only, which needs 65525 clusters;
 * at one sector per cluster that floor lands just above 32.5 MiB. */
#define BLKTEST_SECTORS 73728u
static c64_u8 g_test_disk[(c64_u64)BLKTEST_SECTORS * 512];

static int ram_read(uno_bdev *d, unsigned long long lba, unsigned int n, void *buf)
{
    if (!n || lba + n > d->sectors || lba + n < lba) return 0;
    memcpy(buf, g_test_disk + lba * 512, (unsigned long)n * 512);
    return 1;
}
static int ram_write(uno_bdev *d, unsigned long long lba, unsigned int n,
                     const void *buf)
{
    if (!n || lba + n > d->sectors || lba + n < lba) return 0;
    memcpy(g_test_disk + lba * 512, buf, (unsigned long)n * 512);
    return 1;
}

/* Registered from uno_blk_init, so fat.c's scan finds it already formatted. */
static void blktest_register(void)
{
    uno_bdev d;
    memset(&d, 0, sizeof d);
    d.native  = 1;
    d.sectors = BLKTEST_SECTORS;
    memcpy(d.name, "ramdk", 6);
    d.pci_dev = -1; d.pci_fn = -1;
    d.is_boot = 1;
    d.read = ram_read;
    d.write = ram_write;
    if (!uno_blk_register(&d)) return;
    if (uno_fat_mkfs(uno_blk_get(uno_blk_count() - 1), 0, BLKTEST_SECTORS,
                     "BLKTEST"))
        c64_logf("blktest: formatted a %d MiB RAM disk as FAT32\n",
                 (int)(BLKTEST_SECTORS / 2048));
    else
        c64_log("blktest: FAIL -- uno_fat_mkfs refused the RAM disk\n");
}

/* The round trip, run from the storage report once the volume map exists. */
static void blktest_roundtrip(void)
{
    static const unsigned char msg[] =
        "UnoDOS cosmo64 storage round trip: written and read back.";
    static unsigned char back[128];
    int n = uno_fs_volumes(), v, vol = -1, i;
    long got;

    for (v = 0; v < n; v++)
        if (uno_fs_kind(v) == 1 && uno_fs_writable(v)) { vol = v; break; }
    if (vol < 0) { c64_log("blktest: FAIL -- no writable FAT volume\n"); return; }

    if (!uno_fs_write(vol, "BLKTEST.TXT", msg, (long)sizeof msg - 1)) {
        c64_log("blktest: FAIL -- the write was refused\n");
        return;
    }
    got = uno_fs_read(vol, "BLKTEST.TXT", back, (long)sizeof back);
    if (got != (long)sizeof msg - 1) {
        c64_logf("blktest: FAIL -- read back %d bytes, wrote %d\n",
                 (int)got, (int)(sizeof msg - 1));
        return;
    }
    for (i = 0; i < (int)sizeof msg - 1; i++)
        if (back[i] != msg[i]) {
            c64_logf("blktest: FAIL -- byte %d differs (%02x, wanted %02x)\n",
                     i, back[i], msg[i]);
            return;
        }
    if (!uno_fat_delete(uno_fs_fat_index(vol), "BLKTEST.TXT")) {
        c64_log("blktest: FAIL -- the delete was refused\n");
        return;
    }
    if (uno_fs_read(vol, "BLKTEST.TXT", back, (long)sizeof back) >= 0) {
        c64_log("blktest: FAIL -- the file is still there after the delete\n");
        return;
    }
    c64_logf("blktest: PASS -- mkfs, mount, write %d bytes, read back, "
             "verify, delete\n", (int)(sizeof msg - 1));
}
#endif /* C64_BLKTEST */

/* ---- registration --------------------------------------------------------- */
/* THE SD CARD GOES FIRST, and it is the boot disk. The 2026-09-01 decision
 * put persistence on the card rather than the eMMC -- every partition on the
 * eMMC belongs to Android, to Gemian, to the GPT or to the preloader -- so
 * `sd0` is what SHELL.CFG, sessions and installed apps should land on.
 * `is_boot` is the flag that makes that happen: session_vol() and
 * uno_fs_pref_vol() both prefer the volume the machine came up on over
 * whichever disk enumerated first. Registering the card first also means it
 * keeps index 0 whether or not the eMMC ever grows a UNODATA partition. */
static int register_sd(void)
{
    uno_bdev d;
    c64_u64 base, sectors;

    c64_sd_init();
    if (!c64_sd_ready()) {
        c64_log("blk: no SD card\n");
        return 0;
    }
    base = c64_sd_part_lba();
    sectors = c64_sd_part_sectors();
    if (!sectors) {
        c64_log("blk: the SD card reads, but carries no FAT partition\n");
        return 0;
    }
    memset(&d, 0, sizeof d);
    d.native  = 1;
    d.sectors = sectors;
    d.name[0] = 's'; d.name[1] = 'd'; d.name[2] = '0'; d.name[3] = 0;
    d.pci_dev = -1; d.pci_fn = -1;
    d.is_boot = 1;
    g_part[g_ndev].base = base;
    d.ctx     = &g_part[g_ndev];
    d.read    = sd_read;
    d.write   = sd_write;
    if (!uno_blk_register(&d)) {
        c64_log("blk: registry full\n");
        return 0;
    }
    c64_logf("blk: sd0 = the card's FAT partition at LBA %d, %d sectors "
             "(%d MiB), addressed partition-relative\n",
             (int)base, (int)sectors, (int)(sectors / 2048));
    return 1;
}

void uno_blk_init(void)
{
    uno_bdev d;
    c64_u64 base, sectors;
    int have_sd;

    if (g_done) return;
    g_done = 1;

    have_sd = register_sd();

    /* c_main() brings the eMMC up before the shell starts, and c64_blk_init()
     * is idempotent, so calling it again here costs nothing and removes the
     * ordering assumption: whoever reaches storage first brings it up. */
    c64_blk_init();
    if (!c64_blk_ready()) {
        c64_log("blk: no eMMC -- storage is the RAM disk only\n");
#ifdef C64_BLKTEST
        blktest_register();
#endif
        return;
    }
    base    = c64_blk_data_lba();
    sectors = c64_blk_data_sectors();
    if (!sectors) {
        c64_log("blk: the eMMC reads, but the GPT walk found no UnoDOS data "
                "partition -- storage is the RAM disk only\n");
        return;
    }

    memset(&d, 0, sizeof d);
    d.native  = 1;                    /* our silicon driver, no firmware      */
    d.sectors = sectors;
    d.name[0] = 'e'; d.name[1] = 'm'; d.name[2] = 'm'; d.name[3] = 'c';
    d.name[4] = '0'; d.name[5] = 0;
    d.pci_dev = -1; d.pci_fn = -1;    /* no PCI on this SoC                   */
    /* THE BOOT DISK, and this flag is load-bearing. session_vol() and
     * uno_fs_pref_vol() both put persistent state on the volume the machine
     * came up on in preference to whichever disk enumerated first -- the
     * ZimaBlade lesson written up in pc64_fs.h. p38 and the data partition
     * are the same eMMC, so this is literally true of it -- but if an SD card
     * mounted, the card is where persistence belongs (see register_sd), and
     * two volumes both claiming to be the boot disk is exactly the ambiguity
     * the flag exists to remove. */
    d.is_boot = !have_sd;
    g_part[g_ndev].base = base;
    d.ctx     = &g_part[g_ndev];
    d.read    = part_read;
    d.write   = part_write;

    if (!uno_blk_register(&d)) {
        c64_log("blk: registry full\n");
        return;
    }
    c64_logf("blk: emmc0 = the data partition at LBA %d, %d sectors "
             "(%d MiB), addressed partition-relative\n",
             (int)base, (int)sectors, (int)(sectors / 2048));
}

/* An LK payload is born detached: there is no firmware transport to lose and
 * nothing to re-probe, so the M3 detach hook is a no-op here. */
void uno_blk_detach(void) { }

/* ---- what the log says about storage -------------------------------------- *
 * There is no QEMU gate for any of this (the virt board has no MSDC), so the
 * log is the gate, and a report nobody can read is not one. Print the device,
 * every volume the FAT layer mounted, and which volume the shell will actually
 * persist to -- that last line is the one that answers "why did the Control
 * Panel open again".
 *
 * uno_fat_selftest() is the write proof, and it is inert unless armed: it does
 * nothing at all unless a file called WRTEST.REQ is sitting on a writable
 * volume, in which case it reads it, writes WRTEST.OK beside it and deletes
 * the request. Put the file there from Linux, boot UnoDOS, boot back, and the
 * result is a read, a write and a delete all proven on metal in one pass. */
void c64_storage_report(void)
{
    int n, v, pref;

    n = uno_fs_volumes();                    /* mounts on first call */
    c64_logf("storage: %d block device(s), %d volume(s)\n", uno_blk_count(), n);
    for (v = 0; v < n; v++) {
        const char *kind = uno_fs_kind(v) == 0 ? "ram"
                         : uno_fs_kind(v) == 1 ? "fat" : "fw";
        c64_logf("  vol %d: %s \"%s\"%s%s\n", v, kind, uno_fs_volume_name(v),
                 uno_fs_writable(v) ? " rw" : " ro",
                 uno_fs_is_boot(v) ? " boot" : "");
    }
    pref = uno_fs_pref_vol();
    if (pref < 0)
        c64_log("storage: nowhere to persist -- SHELL.CFG cannot be saved\n");
    else if (uno_fs_kind(pref) == 0)
        c64_logf("storage: persisting to vol %d, which is the RAM DISK -- the "
                 "session will not survive a reboot (no FAT volume mounted)\n",
                 pref);
    else
        c64_logf("storage: persisting to vol %d (%s)\n", pref,
                 uno_fs_volume_name(pref));

    {
        int hit = uno_fat_selftest();
        if (hit)
            c64_logf("storage: WRTEST self-test wrote WRTEST.OK on %d "
                     "volume(s)\n", hit);
    }
#ifdef C64_BLKTEST
    blktest_roundtrip();
#endif
}
