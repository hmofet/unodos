/* ===========================================================================
 * unostorage - on-device disk authoring: partition (GPT) a raw disk and lay
 * down a bootable FAT32 ESP.  The reusable framework that both the installer
 * and unoautomate wrap; it never re-implements sector transport (blkdev) or
 * filesystem formatting (fat) - it composes them.
 *
 * Runs over a transport-agnostic sector device (`unostorage_dev`) so the same
 * GPT code drives a `uno_bdev` (native/firmware sectors) OR an EFI Block IO
 * handle (the installer's target).  Authoring writes a fresh GPT, so it must
 * run where sector WRITES work - while ATTACHED for a firmware `uno_bdev`, or
 * post-detach for a native one.  See REMOTE.md / installer.c.
 * ======================================================================== */
#ifndef PC64_UNOSTORAGE_H
#define PC64_UNOSTORAGE_H

#include "blkdev.h"

/* A raw 512-byte-sector device.  read/write take an LBA + sector COUNT and
 * return 1 on success / 0 on failure (the uno_bdev convention). */
typedef struct {
    void *ctx;
    unsigned long long sectors;                 /* total 512-B sectors */
    int (*read )(void *ctx, unsigned long long lba, unsigned int n, void *buf);
    int (*write)(void *ctx, unsigned long long lba, unsigned int n, const void *buf);
} unostorage_dev;

/* Adapter: drive a registered block device through the framework. */
unostorage_dev unostorage_from_bdev(uno_bdev *b);

/* Shared reflected CRC-32 (poly 0xEDB88320) - the GPT header/array checksum. */
unsigned int unostorage_crc32(const void *data, unsigned long len);

/* The EFI System Partition type GUID, in on-disk (mixed-endian) byte order. */
extern const unsigned char unostorage_esp_type[16];

/* Write a fresh protective MBR + an empty GPT (primary @LBA1, backup @last),
 * sized to the device.  Erases any existing partition table.  1 on success. */
int unostorage_gpt_init(unostorage_dev *d);

/* Add one partition entry to an existing GPT (first free slot), then rewrite
 * both entry arrays + both headers with fresh CRCs.  `name` is ASCII, stored
 * as UTF-16.  1 on success, 0 if the GPT is bad or the table is full. */
int unostorage_gpt_add(unostorage_dev *d, unsigned long long first_lba,
                       unsigned long long last_lba,
                       const unsigned char type_guid[16], const char *name);

/* High-level, the headline "prepare disk B": author a fresh GPT holding ONE
 * ESP spanning the disk (1 MiB-aligned, backup GPT at the true end) and format
 * it FAT32 (uno_fat_mkfs).  Operates on a uno_bdev.  Afterwards the caller
 * remounts (uno_fat_remount + uno_fs_remap) to surface the new volume.
 * Returns 1 on success, 0 on failure (disk too small / read-only / geometry).
 * NOTE: destructive - every byte on the disk is lost. */
int unostorage_prepare_esp(uno_bdev *dev, const char *label);

/* Read back the first ESP partition's LBA range + unique GUID from the GPT
 * (for authoring a boot entry that targets it).  1 if found. */
int unostorage_find_esp(unostorage_dev *d, unsigned long long *first,
                        unsigned long long *last, unsigned char guid[16]);

#endif /* PC64_UNOSTORAGE_H */
