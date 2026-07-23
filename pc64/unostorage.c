/* ===========================================================================
 * unostorage - see unostorage.h.  GPT authoring ported from flash/UnoDisk.cs
 * (the proven host formatter) + installer.c's clone math, over blkdev.
 * ======================================================================== */
#include "unostorage.h"
#include "fat.h"            /* uno_fat_mkfs */
#include "string.h"
#include <stdint.h>

/* ---- little-endian byte writers ------------------------------------------ */
static void w32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void w64(uint8_t *p, uint64_t v) { int i; for (i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t r32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t r64(const uint8_t *p)
{ uint64_t v=0; int i; for (i=0;i<8;i++) v |= (uint64_t)p[i]<<(8*i); return v; }

/* ---- CRC-32 (reflected, poly 0xEDB88320), bit-at-a-time ------------------ */
unsigned int unostorage_crc32(const void *data, unsigned long len)
{
    const unsigned char *p = data;
    unsigned int c = 0xFFFFFFFFu; unsigned long i; int k;
    for (i = 0; i < len; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

const unsigned char unostorage_esp_type[16] = {   /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};

/* ---- uno_bdev adapter ---------------------------------------------------- */
static int bdev_read(void *ctx, unsigned long long lba, unsigned int n, void *buf)
{ uno_bdev *b = ctx; return b->read ? b->read(b, lba, n, buf) : 0; }
static int bdev_write(void *ctx, unsigned long long lba, unsigned int n, const void *buf)
{ uno_bdev *b = ctx; return b->write ? b->write(b, lba, n, buf) : 0; }

unostorage_dev unostorage_from_bdev(uno_bdev *b)
{
    unostorage_dev d;
    d.ctx = b; d.sectors = b->sectors; d.read = bdev_read; d.write = bdev_write;
    return d;
}

/* ---- GPT geometry constants ---------------------------------------------- */
#define SEC        512
#define GPT_ENTS   128
#define GPT_ENTSZ  128
#define ARR_BYTES  (GPT_ENTS * GPT_ENTSZ)      /* 16384 = 32 sectors */
#define ARR_SECS   (ARR_BYTES / SEC)           /* 32 */

static unsigned char g_arr[ARR_BYTES];         /* the partition entry array (single-threaded) */

/* a deterministic pseudo-GUID (no RNG on-device; uniqueness is best-effort) */
static void synth_guid(unsigned char g[16], unsigned long long salt)
{
    static const unsigned char t[16] = {
        0x55,0x4E,0x4F,0x53, 0x54,0x4F, 0x52,0x45, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
    int i;
    for (i = 0; i < 16; i++) g[i] = (unsigned char)(t[i] ^ (salt >> ((i & 7) * 8)));
    g[7] = (unsigned char)((g[7] & 0x0F) | 0x40);   /* version 4-ish */
    g[8] = (unsigned char)((g[8] & 0x3F) | 0x80);   /* variant       */
}

/* build a GPT header sector into `h` (512 zeroed). */
static void gpt_header(uint8_t *h, uint64_t my, uint64_t alt,
                       uint64_t firstUsable, uint64_t lastUsable,
                       const unsigned char diskGuid[16], uint64_t entryLba,
                       uint32_t arrCrc)
{
    memset(h, 0, SEC);
    memcpy(h, "EFI PART", 8);
    w32(h + 8, 0x00010000);          /* revision 1.0 */
    w32(h + 12, 92);                 /* header size  */
    w32(h + 16, 0);                  /* CRC placeholder */
    w64(h + 24, my);
    w64(h + 32, alt);
    w64(h + 40, firstUsable);
    w64(h + 48, lastUsable);
    memcpy(h + 56, diskGuid, 16);
    w64(h + 72, entryLba);
    w32(h + 80, GPT_ENTS);
    w32(h + 84, GPT_ENTSZ);
    w32(h + 88, arrCrc);
    w32(h + 16, (uint32_t)unostorage_crc32(h, 92));
}

/* write protective MBR + the (already-filled) entry array + both headers. */
static int write_gpt(unostorage_dev *d, const unsigned char diskGuid[16])
{
    uint8_t sec[SEC];
    uint64_t diskSectors = d->sectors;
    uint64_t altHdr = diskSectors - 1;
    uint64_t altArr = altHdr - ARR_SECS;
    uint64_t firstUsable = 2 + ARR_SECS;             /* 34 */
    uint64_t lastUsable = diskSectors - 1 - (1 + ARR_SECS);
    uint32_t arrCrc = (uint32_t)unostorage_crc32(g_arr, ARR_BYTES);
    unsigned i;

    if (diskSectors < 2 * ARR_SECS + 4) return 0;

    /* protective MBR @ LBA0 */
    memset(sec, 0, SEC);
    sec[446 + 1] = 0x00; sec[446 + 2] = 0x02; sec[446 + 3] = 0x00;   /* CHS 0/0/2 */
    sec[446 + 4] = 0xEE;                                             /* type */
    sec[446 + 5] = 0xFF; sec[446 + 6] = 0xFF; sec[446 + 7] = 0xFF;
    w32(sec + 446 + 8, 1);
    w32(sec + 446 + 12, diskSectors - 1 >= 0xFFFFFFFFull
                        ? 0xFFFFFFFFu : (uint32_t)(diskSectors - 1));
    sec[510] = 0x55; sec[511] = 0xAA;
    if (!d->write(d->ctx, 0, 1, sec)) return 0;

    /* entry array (32 sectors) at LBA2 and at altArr */
    for (i = 0; i < ARR_SECS; i++) {
        if (!d->write(d->ctx, 2 + i, 1, g_arr + i * SEC)) return 0;
        if (!d->write(d->ctx, altArr + i, 1, g_arr + i * SEC)) return 0;
    }
    /* primary header @1, backup header @altHdr */
    gpt_header(sec, 1, altHdr, firstUsable, lastUsable, diskGuid, 2, arrCrc);
    if (!d->write(d->ctx, 1, 1, sec)) return 0;
    gpt_header(sec, altHdr, 1, firstUsable, lastUsable, diskGuid, altArr, arrCrc);
    if (!d->write(d->ctx, altHdr, 1, sec)) return 0;
    return 1;
}

/* fill entry slot `idx` of g_arr with a partition. */
static void set_entry(int idx, unsigned long long first, unsigned long long last,
                      const unsigned char type[16], const char *name)
{
    unsigned char *e = g_arr + idx * GPT_ENTSZ;
    int i;
    memset(e, 0, GPT_ENTSZ);
    memcpy(e, type, 16);
    synth_guid(e + 16, first ^ (last << 12) ^ 0xA5A5);      /* unique partition GUID */
    w64(e + 32, first);
    w64(e + 40, last);
    for (i = 0; name && name[i] && i < 35; i++)             /* UTF-16LE name (72 bytes) */
        e[56 + i * 2] = (unsigned char)name[i];
}

int unostorage_gpt_init(unostorage_dev *d)
{
    unsigned char dg[16];
    if (!d || !d->write) return 0;
    memset(g_arr, 0, ARR_BYTES);
    synth_guid(dg, d->sectors ^ 0x1234);
    return write_gpt(d, dg);
}

int unostorage_gpt_add(unostorage_dev *d, unsigned long long first_lba,
                       unsigned long long last_lba,
                       const unsigned char type_guid[16], const char *name)
{
    uint8_t hdr[SEC];
    unsigned char dg[16];
    uint64_t entryLba; uint32_t num, esz; unsigned i; int free_slot = -1;

    if (!d || !d->write || !d->read) return 0;
    if (!d->read(d->ctx, 1, 1, hdr) || memcmp(hdr, "EFI PART", 8) != 0) return 0;
    entryLba = r64(hdr + 72); num = r32(hdr + 80); esz = r32(hdr + 84);
    if (esz != GPT_ENTSZ || num != GPT_ENTS) return 0;   /* we only author the standard shape */
    memcpy(dg, hdr + 56, 16);

    for (i = 0; i < ARR_SECS; i++)
        if (!d->read(d->ctx, entryLba + i, 1, g_arr + i * SEC)) return 0;
    for (i = 0; i < GPT_ENTS; i++) {
        const unsigned char *e = g_arr + i * GPT_ENTSZ;
        int zero = 1, k; for (k = 0; k < 16; k++) if (e[k]) { zero = 0; break; }
        if (zero) { free_slot = (int)i; break; }
    }
    if (free_slot < 0) return 0;                          /* table full */
    set_entry(free_slot, first_lba, last_lba, type_guid, name);
    return write_gpt(d, dg);
}

int unostorage_gpt_finalize_clone(unostorage_dev *d)
{
    uint8_t hdr[SEC];
    unsigned char dg[16];
    uint64_t entryLba; uint32_t num, esz; unsigned i;

    if (!d || !d->read || !d->write) return 0;
    /* the clone left the source's primary header at LBA1 and its entry array at
     * entryLba; read the disk GUID + the array back, then re-author over them. */
    if (!d->read(d->ctx, 1, 1, hdr) || memcmp(hdr, "EFI PART", 8) != 0) return 0;
    entryLba = r64(hdr + 72); num = r32(hdr + 80); esz = r32(hdr + 84);
    if (esz != GPT_ENTSZ || num != GPT_ENTS) return 0;   /* only the standard shape */
    memcpy(dg, hdr + 56, 16);
    for (i = 0; i < ARR_SECS; i++)
        if (!d->read(d->ctx, entryLba + i, 1, g_arr + i * SEC)) return 0;
    /* write_gpt recomputes altHdr/altArr/lastUsable from d->sectors (the target),
     * relocating the backup GPT to the true end and fixing every CRC + the MBR. */
    return write_gpt(d, dg);
}

int unostorage_find_esp(unostorage_dev *d, unsigned long long *first,
                        unsigned long long *last, unsigned char guid[16])
{
    uint8_t hdr[SEC]; uint64_t entryLba; uint32_t num, esz; unsigned i;
    if (!d || !d->read) return 0;
    if (!d->read(d->ctx, 1, 1, hdr) || memcmp(hdr, "EFI PART", 8) != 0) return 0;
    entryLba = r64(hdr + 72); num = r32(hdr + 80); esz = r32(hdr + 84);
    if (esz != GPT_ENTSZ || num != GPT_ENTS) return 0;
    for (i = 0; i < ARR_SECS; i++)
        if (!d->read(d->ctx, entryLba + i, 1, g_arr + i * SEC)) return 0;
    for (i = 0; i < GPT_ENTS; i++) {
        const unsigned char *e = g_arr + i * GPT_ENTSZ;
        if (memcmp(e, unostorage_esp_type, 16) == 0) {      /* first ESP */
            if (first) *first = r64(e + 32);
            if (last)  *last  = r64(e + 40);
            if (guid)  { int k; for (k = 0; k < 16; k++) guid[k] = e[16 + k]; }
            return 1;
        }
    }
    return 0;
}

int unostorage_prepare_esp(uno_bdev *dev, const char *label)
{
    unostorage_dev d;
    unsigned char dg[16];
    unsigned long long diskSectors, lastUsable, partLast, volSectors, first = 2048;
    const unsigned long long MAX_SECTORS = 0xFFFFFFFFull;   /* FAT32 total-sector field is 32-bit */
    const unsigned long long MIN_SECTORS = 96ull * 1024 * 1024 / 512;   /* 96 MiB */

    if (!dev || !dev->write) return 0;
    d = unostorage_from_bdev(dev);
    diskSectors = d.sectors;
    if (diskSectors < MIN_SECTORS) return 0;

    lastUsable = (diskSectors - 1) - (1 + ARR_SECS);       /* backup header + array at the end */
    partLast   = ((lastUsable + 1) / first) * first - 1;   /* 1 MiB-aligned inclusive end */
    if (partLast - first + 1 > MAX_SECTORS)
        partLast = ((first + MAX_SECTORS) / first) * first - 1;
    if (partLast <= first) return 0;
    volSectors = partLast - first + 1;
    if (volSectors < MIN_SECTORS) return 0;

    /* one ESP spanning [first, partLast] */
    memset(g_arr, 0, ARR_BYTES);
    set_entry(0, first, partLast, unostorage_esp_type, "UNO-ESP");
    synth_guid(dg, diskSectors ^ 0x9E37);
    if (!write_gpt(&d, dg)) return 0;

    /* format it FAT32 (raw writes; caller remounts to surface it) */
    return uno_fat_mkfs(dev, first, volSectors, label);
}
