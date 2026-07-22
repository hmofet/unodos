/* blkdev.h - the pc64 block-device layer (see blkdev.c).
 *
 * A small registry of 512-byte-sector block devices the native storage stack
 * (fat.c) mounts partitions from.  Backends register at init:
 *   - ahci.c        - the NATIVE AHCI (SATA) driver (no firmware in the path)
 *   - nvme.c        - the NATIVE NVMe driver (PCIe SSDs, ditto)
 *   - sdhci.c       - the NATIVE SDHCI driver (eMMC + SD cards, ditto)
 *   - blkdev.c      - a fallback wrapping firmware EFI Block IO whole-disk
 *                     handles, so disks without a native controller driver
 *                     (USB) still get native partition scanning and FAT
 *                     mounting.  The FS bytes still never go through the
 *                     firmware FAT driver - only the sector transport does.
 */
#ifndef PC64_BLKDEV_H
#define PC64_BLKDEV_H

typedef struct uno_bdev {
    int  native;                     /* 1 = our silicon driver, 0 = fw sectors */
    unsigned long long sectors;      /* total 512-byte sectors                 */
    char name[8];                    /* "ahci0", "fw2", ...                    */
    int  pci_dev, pci_fn;            /* controller PCI location (-1 unknown);
                                        used to dedup firmware FS volumes      */
    int  is_boot;                    /* 1 = the disk UnoDOS booted from (the
                                        storage safety gate refuses to wipe it) */
    void *dp;                        /* firmware whole-disk device path (fw
                                        devices only), for authoring a boot entry
                                        on a partition we create; NULL if native */
    void *ctx;
    int (*read)(struct uno_bdev *, unsigned long long lba, unsigned int n, void *buf);
    int (*write)(struct uno_bdev *, unsigned long long lba, unsigned int n, const void *buf);
} uno_bdev;

/* Bring up all backends + register their disks.  Idempotent. */
void uno_blk_init(void);

/* M3: after ExitBootServices - drop the (dead) firmware devices and take the
 * controllers natively.  Follow with uno_fat_remount(). */
void uno_blk_detach(void);

int       uno_blk_count(void);
uno_bdev *uno_blk_get(int i);

/* Register a device (backends call this).  Returns 0 when the table is full. */
int uno_blk_register(const uno_bdev *dev);

/* ahci.c */
int uno_ahci_init(void);             /* registers its disks; returns count */
int uno_ahci_present(void);          /* controller found + brought up      */

/* nvme.c */
int uno_nvme_init(void);             /* registers its namespaces; returns count */
int uno_nvme_present(void);          /* controller found + brought up      */

/* sdhci.c */
int uno_sdhci_init(void);            /* registers the eMMC/SD medium; count */
int uno_sdhci_present(void);         /* controller found + brought up      */

#endif
