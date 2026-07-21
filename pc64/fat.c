/* ===========================================================================
 * UnoDOS/pc64 - native FAT16/32 driver (read + write) over the block layer.
 *
 * No firmware FAT code in the path: this reads/writes raw sectors through
 * uno_blk (native AHCI, or firmware-Block-IO sectors for disks with no native
 * driver) and implements FAT itself.  Partition discovery per disk: GPT, then
 * MBR, then a superfloppy (BPB at LBA 0) probe.  Every FAT16/32 partition is
 * mounted - an ESP and a plain basic-data FAT32 are treated identically, which
 * is what lets the OS run from any FAT32 volume, not just "the ESP".
 *
 * 8.3 short names only (the whole OS uses them); backslash subdir paths.  A
 * one-sector LRU-ish cache keeps directory/FAT walks cheap.  Sizes are bounded
 * for a freestanding kernel: files up to a few MB, clusters walked with a
 * loop guard so a cross-linked chain can't spin forever.
 * ======================================================================== */
#include "fat.h"
#include "blkdev.h"
#include "pc64_pci.h"       /* uno_fat_native_eligible: AHCI-class lookup */
#include "string.h"
#include <stdint.h>

#define MAXVOL   8
#define SECT     512
#define CH_N     8                          /* sector cache lines            */
#define MAXCLUS_WALK 0x100000               /* chain loop guard              */

typedef struct {
    uno_bdev *dev;
    uint64_t  part_lba;                     /* partition start sector        */
    int       fat32;
    uint32_t  fat_start;                    /* abs LBA of FAT #0             */
    uint32_t  fat_sectors;
    uint32_t  data_start;                   /* abs LBA of cluster 2          */
    uint32_t  root_start;                   /* FAT16: abs LBA of root dir    */
    uint32_t  root_sectors;                 /* FAT16 fixed root              */
    uint32_t  root_clus;                    /* FAT32 root cluster            */
    uint32_t  sec_per_clus;
    uint32_t  clusters;                     /* count of data clusters        */
    uint32_t  serial;
    char      label[12];
    int       used;
} fatvol;

static fatvol g_vol[MAXVOL];
static int    g_nvol;
static int    g_done;

/* ---- tiny sector cache ---------------------------------------------------- *
 * buf is 128-byte aligned: EFI Block IO honours Media->IoAlign and native AHCI
 * DMAs straight out of this buffer, so an unaligned sector buffer silently
 * returns garbage (0xFF fill) on some controllers.  Aligning the member forces
 * the struct stride to preserve the alignment across the whole array. */
typedef struct {
    uno_bdev *dev; uint64_t lba; int valid, dirty, age;
    uint8_t buf[SECT] __attribute__((aligned(128)));
} cline;
static cline g_ch[CH_N];
static int   g_clock;

static void cache_flush_line(cline *c)
{
    if (c->valid && c->dirty && c->dev) { c->dev->write(c->dev, c->lba, 1, c->buf); c->dirty = 0; }
}
static cline *cache_get(uno_bdev *dev, uint64_t lba)
{
    int i, victim = 0, oldest = 0x7fffffff;
    for (i = 0; i < CH_N; i++)
        if (g_ch[i].valid && g_ch[i].dev == dev && g_ch[i].lba == lba) { g_ch[i].age = ++g_clock; return &g_ch[i]; }
    for (i = 0; i < CH_N; i++) {
        if (!g_ch[i].valid) { victim = i; break; }
        if (g_ch[i].age < oldest) { oldest = g_ch[i].age; victim = i; }
    }
    cache_flush_line(&g_ch[victim]);
    g_ch[victim].dev = dev; g_ch[victim].lba = lba; g_ch[victim].valid = 1;
    g_ch[victim].dirty = 0; g_ch[victim].age = ++g_clock;
    if (!dev->read(dev, lba, 1, g_ch[victim].buf)) { g_ch[victim].valid = 0; return 0; }
    return &g_ch[victim];
}
static void cache_put(cline *c) { c->dirty = 1; }
static void cache_sync(void) { int i; for (i = 0; i < CH_N; i++) cache_flush_line(&g_ch[i]); }
static void cache_drop(uno_bdev *dev)   /* invalidate a device's lines after raw IO */
{ int i; for (i = 0; i < CH_N; i++) if (g_ch[i].dev == dev) { cache_flush_line(&g_ch[i]); g_ch[i].valid = 0; } }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void     wr16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void     wr32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* ---- mount one FAT partition at abs LBA `start` --------------------------- */
static int mount_at(uno_bdev *dev, uint64_t start)
{
    uint8_t bs[SECT];
    fatvol v;
    uint32_t tot, rootdir_sectors, fatsz, data_sectors, bytes_per_sec, resv, nfats;
    if (g_nvol >= MAXVOL) return 0;
    if (!dev->read(dev, start, 1, bs)) return 0;
    bytes_per_sec = rd16(bs + 11);
    if (bytes_per_sec != SECT) return 0;                 /* 512 B/s only       */
    if (bs[510] != 0x55 || bs[511] != 0xAA) return 0;
    memset(&v, 0, sizeof v);
    v.dev = dev; v.part_lba = start;
    v.sec_per_clus = bs[13];
    if (!v.sec_per_clus) return 0;
    resv  = rd16(bs + 14);
    nfats = bs[16];
    if (!nfats) return 0;
    tot   = rd16(bs + 19); if (!tot) tot = rd32(bs + 32);
    fatsz = rd16(bs + 22);
    v.fat32 = (fatsz == 0);
    if (v.fat32) fatsz = rd32(bs + 36);
    v.fat_sectors  = fatsz;
    v.fat_start    = (uint32_t)start + resv;
    rootdir_sectors = v.fat32 ? 0 : ((rd16(bs + 17) * 32u) + SECT - 1) / SECT;
    v.root_sectors = rootdir_sectors;
    v.data_start   = (uint32_t)start + resv + nfats * fatsz + rootdir_sectors;
    v.root_start   = (uint32_t)start + resv + nfats * fatsz;   /* FAT16 root   */
    /* Crafted-BPB guard: if TotalSectors is below the reserved+FAT+root region,
     * this unsigned subtraction underflows to ~4 billion clusters and the first
     * fat_alloc scan effectively hangs the machine. Compute the metadata span in
     * 64-bit (a huge fatsz can also overflow 32-bit) and reject anything below it. */
    {
        unsigned long long meta = (unsigned long long)resv +
              (unsigned long long)nfats * fatsz + rootdir_sectors;
        if (tot <= meta) return 0;
        data_sectors = (uint32_t)(tot - meta);
    }
    v.clusters     = data_sectors / v.sec_per_clus;
    if (v.clusters > 0x0FFFFFF6u) return 0;         /* implausible: beyond FAT32 max */
    /* classify by cluster count if the BPB was ambiguous */
    if (!v.fat32 && v.clusters < 4085) return 0;    /* FAT12: unsupported - a
                                                       FAT16 mis-mount would
                                                       serve garbage chains */
    if (v.fat32) {
        v.root_clus = rd32(bs + 44);
        v.serial    = rd32(bs + 67);
        memcpy(v.label, bs + 71, 11); v.label[11] = 0;
    } else {
        v.root_clus = 0;
        v.serial    = rd32(bs + 39);
        memcpy(v.label, bs + 43, 11); v.label[11] = 0;
    }
    /* sanity: FAT must sit inside the device */
    if (v.data_start >= dev->sectors) return 0;
    v.used = 1;
    g_vol[g_nvol++] = v;
    return 1;
}

/* ---- partition discovery per disk ----------------------------------------- */
static void scan_disk(uno_bdev *dev)
{
    uint8_t sec[SECT];
    int i;
    if (!dev->read(dev, 0, 1, sec)) return;

    /* GPT? protective/hybrid MBR type 0xEE at entry 0, "EFI PART" at LBA 1 */
    if (sec[450] == 0xEE) {
        uint8_t gh[SECT];
        if (dev->read(dev, 1, 1, gh) && memcmp(gh, "EFI PART", 8) == 0) {
            uint64_t elba = ((uint64_t)rd32(gh + 76) << 32) | rd32(gh + 72);
            uint32_t num  = rd32(gh + 80), esz = rd32(gh + 84), e;
            uint8_t  tbl[SECT];
            for (e = 0; e < num && g_nvol < MAXVOL; e++) {
                uint64_t entry_byte = (uint64_t)e * esz;
                uint64_t sec_lba = elba + entry_byte / SECT;
                uint32_t off = (uint32_t)(entry_byte % SECT);
                uint64_t first;
                static const uint8_t zero[16] = { 0 };
                if (dev->read(dev, sec_lba, 1, tbl) == 0) break;
                if (memcmp(tbl + off, zero, 16) == 0) continue;   /* unused    */
                first = ((uint64_t)rd32(tbl + off + 36) << 32) | rd32(tbl + off + 32);
                mount_at(dev, first);                 /* try any GUID as FAT   */
            }
            return;
        }
    }

    /* MBR: four primaries; try each FAT type code (and any, mount_at validates) */
    {
        int any = 0;
        for (i = 0; i < 4; i++) {
            const uint8_t *p = sec + 446 + i * 16;
            uint8_t type = p[4];
            uint32_t lba = rd32(p + 8);
            if (!type || !lba) continue;
            if (type == 0x0B || type == 0x0C || type == 0x0E ||   /* FAT32/16  */
                type == 0x06 || type == 0x04 || type == 0x01 ||
                type == 0xEF /* ESP-on-MBR */)
                any |= mount_at(dev, lba);
        }
        if (any) return;
    }

    /* superfloppy: BPB right at LBA 0 (no partition table) */
    mount_at(dev, 0);
}

void uno_fat_init(void)
{
    int i, n;
    if (g_done) return;
    g_done = 1;
    uno_blk_init();
    n = uno_blk_count();
    for (i = 0; i < n; i++) {
        uno_bdev *d = uno_blk_get(i);
        if (d) scan_disk(d);
    }
}

/* flush every dirty cache line to disk (call BEFORE the sector transport
 * changes - e.g. right before ExitBootServices kills firmware Block IO) */
void uno_fat_sync(void) { cache_sync(); }

/* Rebuild the volume table over the CURRENT block-device set, dropping every
 * cached line WITHOUT flushing (the old devices' transport is already gone -
 * the caller synced while it was still alive).  The M3 detach path: firmware
 * Block IO devices out, native AHCI in, same disks re-scanned natively. */
void uno_fat_remount(void)
{
    uno_fat_seq_flush();   /* chains may move under the read cursor */
    int i;
    for (i = 0; i < CH_N; i++) { g_ch[i].valid = 0; g_ch[i].dirty = 0; g_ch[i].dev = 0; }
    g_nvol = 0;
    g_done = 0;
    uno_fat_init();
}

/* The detach gate: 1 only if a volume a native driver will still reach
 * after the firmware dies (its controller PCI dev/fn is an AHCI- or
 * NVMe-class function pc64's drivers bind) actually CARRIES OUR SYSTEM (a
 * UnoDOS BOOTX64.EFI).  A merely-present foreign FAT partition must not
 * count - a USB-booted system would detach away its own boot volume and
 * the firmware-dependent Install app. */
int uno_fat_native_eligible(void)
{
    static const char *marks[] = { "EFI\\UNODOS\\BOOTX64.EFI",
                                   "EFI\\BOOT\\BOOTX64.EFI" };
    static const unsigned char cls[3][2] = { { 0x01, 0x06 },   /* AHCI  */
                                             { 0x01, 0x08 },   /* NVMe  */
                                             { 0x08, 0x05 } }; /* SDHCI */
    pci_dev ctl;
    int c, i, m;
    unsigned char sig[2];
    for (c = 0; c < 3; c++) {
        if (!pci_find_class(cls[c][0], cls[c][1], &ctl)) continue;
        for (i = 0; i < g_nvol; i++) {
            if (!g_vol[i].dev || g_vol[i].dev->pci_dev != ctl.dev ||
                g_vol[i].dev->pci_fn != ctl.fn)
                continue;
            for (m = 0; m < 2; m++)
                if (uno_fat_read(i, marks[m], sig, 2) == 2 &&
                    sig[0] == 'M' && sig[1] == 'Z')
                    return 1;
        }
    }
    return 0;
}

int uno_fat_volumes(void) { return g_nvol; }
const char *uno_fat_label(int vol)  { return (vol >= 0 && vol < g_nvol) ? g_vol[vol].label : ""; }
unsigned int uno_fat_serial(int vol){ return (vol >= 0 && vol < g_nvol) ? g_vol[vol].serial : 0; }

/* ---- FAT table access ----------------------------------------------------- */
static uint32_t fat_get(fatvol *v, uint32_t clus)
{
    if (v->fat32) {
        uint32_t off = clus * 4, lba = v->fat_start + off / SECT;
        cline *c = cache_get(v->dev, lba);
        if (!c) return 0x0FFFFFFF;
        return rd32(c->buf + (off % SECT)) & 0x0FFFFFFF;
    } else {
        uint32_t off = clus * 2, lba = v->fat_start + off / SECT;
        cline *c = cache_get(v->dev, lba);
        if (!c) return 0xFFFF;
        return rd16(c->buf + (off % SECT));
    }
}
static void fat_set(fatvol *v, uint32_t clus, uint32_t val)
{
    if (v->fat32) {
        uint32_t off = clus * 4, lba = v->fat_start + off / SECT;
        cline *c = cache_get(v->dev, lba);
        if (!c) return;
        wr32(c->buf + (off % SECT), (rd32(c->buf + (off % SECT)) & 0xF0000000) | (val & 0x0FFFFFFF));
        cache_put(c);
    } else {
        uint32_t off = clus * 2, lba = v->fat_start + off / SECT;
        cline *c = cache_get(v->dev, lba);
        if (!c) return;
        wr16(c->buf + (off % SECT), (uint16_t)val);
        cache_put(c);
    }
}
static int fat_eoc(fatvol *v, uint32_t c)
{ return v->fat32 ? (c >= 0x0FFFFFF8) : (c >= 0xFFF8); }

static uint32_t clus_lba(fatvol *v, uint32_t clus)
{ return v->data_start + (clus - 2) * v->sec_per_clus; }

/* allocate one free cluster, mark EOC; 0 = disk full */
static uint32_t fat_alloc(fatvol *v)
{
    uint32_t c;
    for (c = 2; c < v->clusters + 2; c++)
        if (fat_get(v, c) == 0) {
            fat_set(v, c, v->fat32 ? 0x0FFFFFFF : 0xFFFF);
            return c;
        }
    return 0;
}
static void fat_free_chain(fatvol *v, uint32_t clus)
{
    uint32_t guard = 0;
    while (clus >= 2 && !fat_eoc(v, clus) && guard++ < MAXCLUS_WALK) {
        uint32_t next = fat_get(v, clus);
        fat_set(v, clus, 0);
        clus = next;
    }
}

/* ---- directory model ------------------------------------------------------ *
 * A directory is a sequence of 32-byte entries.  We iterate the sequence of
 * sectors that back it (FAT16 fixed root = a flat run; else a cluster chain)
 * via a small cursor so read/list/write share one walker. */
typedef struct {
    fatvol  *v;
    int      is_fixed;                      /* FAT16 root                    */
    uint32_t lba;                           /* current sector                */
    uint32_t clus;                          /* current cluster (chained)     */
    uint32_t sec_in_clus;
    uint32_t fixed_left;                    /* sectors left (fixed root)     */
    uint32_t guard;
} dircur;

static void dir_open(dircur *d, fatvol *v, uint32_t start_clus, int fixed_root)
{
    d->v = v; d->guard = 0;
    if (fixed_root) {
        d->is_fixed = 1; d->lba = v->root_start; d->fixed_left = v->root_sectors;
    } else {
        d->is_fixed = 0; d->clus = start_clus; d->sec_in_clus = 0;
        d->lba = clus_lba(v, start_clus);
    }
}
/* advance to the next sector; 0 = end of directory */
static int dir_next(dircur *d)
{
    if (++d->guard > MAXCLUS_WALK) return 0;
    if (d->is_fixed) {
        if (d->fixed_left <= 1) return 0;
        d->fixed_left--; d->lba++;
        return 1;
    }
    if (++d->sec_in_clus < d->v->sec_per_clus) { d->lba++; return 1; }
    {
        uint32_t next = fat_get(d->v, d->clus);
        if (fat_eoc(d->v, next) || next < 2) return 0;
        d->clus = next; d->sec_in_clus = 0; d->lba = clus_lba(d->v, next);
        return 1;
    }
}

/* pack "name.ext" -> 11-byte padded 8.3 (uppercased). returns 0 on bad name */
static int pack83(const char *n, uint8_t out[11])
{
    int i = 0, j = 0;
    memset(out, ' ', 11);
    for (; n[i] && n[i] != '.' && j < 8; i++) {
        char c = n[i]; if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[j++] = (uint8_t)c;
    }
    while (n[i] && n[i] != '.') i++;
    if (n[i] == '.') {
        i++; j = 8;
        for (; n[i] && j < 11; i++) {
            char c = n[i]; if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[j++] = (uint8_t)c;
        }
    }
    return out[0] != ' ';
}
static void unpack83(const uint8_t *e, char *out)
{
    int i, k = 0;
    for (i = 0; i < 8 && e[i] != ' '; i++) out[k++] = (char)e[i];
    if (e[8] != ' ') { out[k++] = '.'; for (i = 8; i < 11 && e[i] != ' '; i++) out[k++] = (char)e[i]; }
    out[k] = 0;
}

/* find entry `name83` in the directory rooted at start_clus/fixed_root.
 * fills *lba,*off of the 32-byte entry and returns 1; else 0. */
static int dir_find(fatvol *v, uint32_t start_clus, int fixed,
                    const uint8_t name83[11], uint32_t *lba, int *off)
{
    dircur d; dir_open(&d, v, start_clus, fixed);
    do {
        cline *c = cache_get(v->dev, d.lba);
        int i;
        if (!c) return 0;
        for (i = 0; i < SECT; i += 32) {
            uint8_t *e = c->buf + i;
            if (e[0] == 0x00) return 0;               /* end of dir           */
            if (e[0] == 0xE5) continue;               /* deleted              */
            if ((e[11] & 0x0F) == 0x0F) continue;     /* LFN                  */
            if (e[11] & 0x08) continue;               /* volume label         */
            if (memcmp(e, name83, 11) == 0) { *lba = d.lba; *off = i; return 1; }
        }
    } while (dir_next(&d));
    return 0;
}

/* walk a backslash path to its parent dir + leaf 8.3; returns 1 on success.
 * start_clus/fixed describe the parent directory to search/insert in. */
static int resolve_parent(fatvol *v, const char *path,
                          uint32_t *start_clus, int *fixed, uint8_t leaf[11])
{
    const char *seg = path;
    uint32_t clus = v->fat32 ? v->root_clus : 0;
    int fx = v->fat32 ? 0 : 1;
    while (*seg == '\\' || *seg == '/') seg++;
    for (;;) {
        const char *slash = seg; char part[16]; int pl = 0;
        while (*slash && *slash != '\\' && *slash != '/') slash++;
        while (seg < slash && pl < 15) part[pl++] = *seg++;
        part[pl] = 0;
        if (!*slash) {                                /* leaf                 */
            *start_clus = clus; *fixed = fx;
            return pack83(part, leaf);
        }
        {                                             /* descend into a subdir */
            uint8_t d83[11]; uint32_t elba; int eoff; cline *c;
            if (!pack83(part, d83)) return 0;
            if (!dir_find(v, clus, fx, d83, &elba, &eoff)) return 0;
            c = cache_get(v->dev, elba);
            if (!c || !(c->buf[eoff + 11] & 0x10)) return 0;    /* not a dir   */
            clus = ((uint32_t)rd16(c->buf + eoff + 20) << 16) | rd16(c->buf + eoff + 26);
            fx = 0;
        }
        seg = slash + 1;
    }
}

/* resolve `dir` ("" or NULL = root; else a backslash path) to the directory's
 * start for a cursor: fills *clus,*fixed and returns 1, else 0. */
static int dir_locate(fatvol *v, const char *dir, uint32_t *clus, int *fixed)
{
    uint8_t leaf[11]; uint32_t elba; int eoff; cline *c;
    if (!dir || !dir[0]) {
        *clus = v->fat32 ? v->root_clus : 0; *fixed = v->fat32 ? 0 : 1;
        return 1;
    }
    /* resolve_parent leaves us at the dir's own parent + leaf name; open it */
    if (!resolve_parent(v, dir, clus, fixed, leaf)) return 0;
    if (!dir_find(v, *clus, *fixed, leaf, &elba, &eoff)) return 0;
    c = cache_get(v->dev, elba);
    if (!c || !(c->buf[eoff + 11] & 0x10)) return 0;    /* not a dir          */
    *clus = ((uint32_t)rd16(c->buf + eoff + 20) << 16) | rd16(c->buf + eoff + 26);
    *fixed = 0;
    return 1;
}

/* ---- public: list --------------------------------------------------------- */
int uno_fat_list(int vol, const char *dir, char (*names)[13], int maxn)
{
    fatvol *v; uint32_t clus; int fixed, cnt = 0;
    if (vol < 0 || vol >= g_nvol) return 0;
    v = &g_vol[vol];
    if (!dir_locate(v, dir, &clus, &fixed)) return 0;
    {
        dircur d; dir_open(&d, v, clus, fixed);
        do {
            cline *c = cache_get(v->dev, d.lba); int i;
            if (!c) break;
            for (i = 0; i < SECT; i += 32) {
                uint8_t *e = c->buf + i;
                if (e[0] == 0x00) return cnt;
                if (e[0] == 0xE5 || (e[11] & 0x0F) == 0x0F || (e[11] & 0x08)) continue;
                if (e[11] & 0x10) continue;           /* skip subdirs in listing */
                if (cnt < maxn) unpack83(e, names[cnt]);
                cnt++;
            }
        } while (dir_next(&d));
    }
    return cnt;
}

/* like uno_fat_list, but with metadata (dir flag + size) and INCLUDING
 * subdirectory entries; "."/"..", volume labels, LFN and deleted entries are
 * skipped.  Returns the total entry count (may exceed maxn). */
int uno_fat_list_ex(int vol, const char *dir, uno_fat_entry *ents, int maxn)
{
    fatvol *v; uint32_t clus; int fixed, cnt = 0;
    if (vol < 0 || vol >= g_nvol) return 0;
    v = &g_vol[vol];
    if (!dir_locate(v, dir, &clus, &fixed)) return 0;
    {
        dircur d; dir_open(&d, v, clus, fixed);
        do {
            cline *c = cache_get(v->dev, d.lba); int i;
            if (!c) break;
            for (i = 0; i < SECT; i += 32) {
                uint8_t *e = c->buf + i;
                if (e[0] == 0x00) return cnt;
                if (e[0] == 0xE5 || (e[11] & 0x0F) == 0x0F || (e[11] & 0x08)) continue;
                if (e[0] == '.') continue;            /* "." and ".."          */
                if (cnt < maxn) {
                    unpack83(e, ents[cnt].name);
                    ents[cnt].is_dir = (e[11] & 0x10) ? 1 : 0;
                    ents[cnt].size   = (long)rd32(e + 28);
                }
                cnt++;
            }
        } while (dir_next(&d));
    }
    return cnt;
}

/* ---- public: read --------------------------------------------------------- */
/* locate a file's directory entry and pull out its start cluster + size */
static int file_locate(fatvol *v, const char *path, uint32_t *clus, uint32_t *size)
{
    uint32_t pclus; int fixed; uint8_t leaf[11];
    uint32_t elba; int eoff; cline *c;
    if (!resolve_parent(v, path, &pclus, &fixed, leaf)) return 0;
    if (!dir_find(v, pclus, fixed, leaf, &elba, &eoff)) return 0;
    c = cache_get(v->dev, elba); if (!c) return 0;
    *clus = ((uint32_t)rd16(c->buf + eoff + 20) << 16) | rd16(c->buf + eoff + 26);
    *size = rd32(c->buf + eoff + 28);
    return 1;
}

long uno_fat_size(int vol, const char *path)
{
    uint32_t clus, size;
    if (vol < 0 || vol >= g_nvol) return -1;
    if (!file_locate(&g_vol[vol], path, &clus, &size)) return -1;
    return (long)size;
}

/* Read `max` bytes starting at byte `off`.  Media files run far larger than
 * anything this port wants resident, so the audio decoders stream through
 * here instead of slurping whole files: walk the cluster chain to the one
 * holding `off`, then copy from that point. */
/* Sequential-read cursor.
 *
 * Walking the cluster chain from byte 0 on every call makes streaming a file
 * quadratic: read N of a large file and you have walked N/2 chains. On QEMU's
 * cached vvfat that is invisible; on real storage it is why audio playback
 * skipped. One cursor is enough - the media layer streams one file at a time -
 * and it only ever SKIPS work, so a miss is just the old behaviour. */
static struct {
    int      vol;
    char     path[80];
    long     off;                       /* byte offset this cluster starts at */
    uint32_t clus;
    int      valid;
} g_seq;

static int seq_hit(int vol, const char *path, long off, uint32_t *clus, long *rem)
{
    int i;
    if (!g_seq.valid || g_seq.vol != vol || off < g_seq.off) return 0;
    for (i = 0; i < 79 && path[i]; i++)
        if (g_seq.path[i] != path[i]) return 0;
    if (g_seq.path[i] != 0 || path[i] != 0) return 0;
    *clus = g_seq.clus;
    *rem  = off - g_seq.off;            /* bytes from this cluster's start    */
    return 1;
}

static void seq_save(int vol, const char *path, long off, uint32_t clus)
{
    int i;
    for (i = 0; i < 79 && path[i]; i++) g_seq.path[i] = path[i];
    g_seq.path[i] = 0;
    g_seq.vol = vol; g_seq.off = off; g_seq.clus = clus; g_seq.valid = 1;
}

/* any write/delete/rename can move a chain out from under the cursor */
void uno_fat_seq_flush(void) { g_seq.valid = 0; }

long uno_fat_read_at(int vol, const char *path, long off,
                     unsigned char *buf, long max)
{
    fatvol *v; uint32_t clus, size; long total = 0;
    uint32_t bpc, guard = 0, skip_sec;
    long skip_byte, walked = 0;
    if (vol < 0 || vol >= g_nvol || off < 0) return -1;
    v = &g_vol[vol];
    if (!file_locate(v, path, &clus, &size)) return -1;
    if (off >= (long)size) return 0;
    if (max > (long)size - off) max = (long)size - off;
    if (max <= 0) return 0;

    /* walk whole clusters until `off` falls inside the current one, resuming
       from the sequential cursor when this read continues the last one */
    bpc = v->sec_per_clus * SECT;
    { uint32_t c2; long rem;
      if (seq_hit(vol, path, off, &c2, &rem)) { clus = c2; walked = off - rem; off = rem; }
    }
    while (off >= (long)bpc && clus >= 2 && !fat_eoc(v, clus)
           && guard++ < MAXCLUS_WALK) {
        clus = fat_get(v, clus);
        off -= (long)bpc;
        walked += (long)bpc;
    }
    if (clus >= 2 && !fat_eoc(v, clus)) seq_save(vol, path, walked, clus);
    skip_sec  = (uint32_t)(off / SECT);        /* sectors into this cluster    */
    skip_byte = off % SECT;                    /* bytes into that sector       */

    guard = 0;
    while (total < max && clus >= 2 && !fat_eoc(v, clus) && guard++ < MAXCLUS_WALK) {
        uint32_t s;
        for (s = skip_sec; s < v->sec_per_clus && total < max; s++) {
            cline *cc = cache_get(v->dev, clus_lba(v, clus) + s);
            long n = SECT - skip_byte;
            if (n > max - total) n = max - total;
            if (!cc) return total;
            memcpy(buf + total, cc->buf + skip_byte, (size_t)n);
            total += n;
            skip_byte = 0;
        }
        skip_sec = 0;
        clus = fat_get(v, clus);
    }
    return total;
}

long uno_fat_read(int vol, const char *path, unsigned char *buf, long max)
{
    return uno_fat_read_at(vol, path, 0, buf, max);
}

/* ---- public: write (create/overwrite) ------------------------------------- */
static int dir_alloc_slot(fatvol *v, uint32_t pclus, int fixed, uint32_t *lba, int *off);

int uno_fat_write(int vol, const char *path, const unsigned char *buf, long len)
{
    uno_fat_seq_flush();   /* chains may move under the read cursor */
    fatvol *v; uint32_t pclus; int fixed; uint8_t leaf[11];
    uint32_t elba; int eoff; cline *c;
    uint32_t first = 0, prev = 0, need, made = 0;
    long left = len;
    if (vol < 0 || vol >= g_nvol) return 0;
    v = &g_vol[vol];
    if (v->dev->write == 0) return 0;
    if (!resolve_parent(v, path, &pclus, &fixed, leaf)) return 0;

    /* if the file exists, reuse the entry; else alloc. S-FAT-28: capture the
     * old chain but DON'T free it yet - freeing before the new chain is built
     * means an ENOSPC mid-write leaves the entry pointing at freed clusters
     * that another file can then claim (cross-link corruption). Free it only
     * after the new chain and entry are committed. */
    uint32_t old_chain = 0;
    if (dir_find(v, pclus, fixed, leaf, &elba, &eoff)) {
        c = cache_get(v->dev, elba); if (!c) return 0;
        old_chain = ((uint32_t)rd16(c->buf + eoff + 20) << 16) | rd16(c->buf + eoff + 26);
    } else {
        if (!dir_alloc_slot(v, pclus, fixed, &elba, &eoff)) return 0;
        c = cache_get(v->dev, elba); if (!c) return 0;
        memset(c->buf + eoff, 0, 32);
        memcpy(c->buf + eoff, leaf, 11);
        c->buf[eoff + 11] = 0x20;                     /* archive              */
        cache_put(c);   /* mark dirty NOW: fat_alloc's free-scan below churns
                           the 8-line cache, and evicting a clean line DISCARDS
                           the name - the final entry-fill then re-reads the
                           old sector and stamps cluster/size into a slot whose
                           name is zeroed (invisible file, leaked chain) */
    }

    /* write data cluster by cluster */
    need = (uint32_t)((len + (long)v->sec_per_clus * SECT - 1) / ((long)v->sec_per_clus * SECT));
    while (made < need) {
        uint32_t cl = fat_alloc(v), s;
        if (!cl) { if (first) fat_free_chain(v, first); return 0; }  /* old chain intact */
        if (!first) first = cl; else fat_set(v, prev, cl);
        prev = cl;
        for (s = 0; s < v->sec_per_clus; s++) {
            uint8_t tmp[SECT]; long n = left; cline *dc;
            if (n > SECT) n = SECT;
            if (n < 0) n = 0;
            memset(tmp, 0, SECT);
            if (n) memcpy(tmp, buf + (len - left), (size_t)n);
            dc = cache_get(v->dev, clus_lba(v, cl) + s);
            if (!dc) return 0;
            memcpy(dc->buf, tmp, SECT); cache_put(dc);
            left -= n;
        }
        made++;
    }

    /* fill the directory entry (re-fetch: fat ops may have evicted the line) */
    c = cache_get(v->dev, elba); if (!c) return 0;
    wr16(c->buf + eoff + 26, (uint16_t)(first & 0xFFFF));         /* lo cluster */
    wr16(c->buf + eoff + 20, (uint16_t)((first >> 16) & 0xFFFF)); /* hi cluster */
    wr32(c->buf + eoff + 28, (uint32_t)len);                     /* size       */
    cache_put(c);
    /* new chain + entry committed: NOW it is safe to free the old chain (and
     * only if it isn't the same chain we just wrote, which can't happen here
     * since fat_alloc never returns an in-use cluster). */
    if (old_chain >= 2 && old_chain != first) fat_free_chain(v, old_chain);
    cache_sync();
    cache_drop(v->dev);
    return 1;
}

/* find (or grow into) a free 32-byte slot in a directory */
static int dir_alloc_slot(fatvol *v, uint32_t pclus, int fixed, uint32_t *lba, int *off)
{
    dircur d; dir_open(&d, v, pclus, fixed);
    do {
        cline *c = cache_get(v->dev, d.lba); int i;
        if (!c) return 0;
        for (i = 0; i < SECT; i += 32) {
            uint8_t *e = c->buf + i;
            if (e[0] == 0x00 || e[0] == 0xE5) { *lba = d.lba; *off = i; return 1; }
        }
    } while (dir_next(&d));
    /* FAT32 subdir: grow the chain by one cluster of empty entries */
    if (!fixed && v->fat32) {
        uint32_t nc = fat_alloc(v), s;
        if (!nc) return 0;
        fat_set(v, d.clus, nc);
        for (s = 0; s < v->sec_per_clus; s++) {
            cline *cc = cache_get(v->dev, clus_lba(v, nc) + s);
            if (cc) { memset(cc->buf, 0, SECT); cache_put(cc); }
        }
        *lba = clus_lba(v, nc); *off = 0;
        return 1;
    }
    return 0;                                          /* fixed root full      */
}

/* ---- public: delete ------------------------------------------------------- */
int uno_fat_delete(int vol, const char *path)
{
    uno_fat_seq_flush();   /* chains may move under the read cursor */
    fatvol *v; uint32_t pclus; int fixed; uint8_t leaf[11];
    uint32_t elba; int eoff; cline *c; uint32_t clus;
    if (vol < 0 || vol >= g_nvol) return 0;
    v = &g_vol[vol];
    if (v->dev->write == 0) return 0;
    if (!resolve_parent(v, path, &pclus, &fixed, leaf)) return 0;
    if (!dir_find(v, pclus, fixed, leaf, &elba, &eoff)) return 0;
    c = cache_get(v->dev, elba); if (!c) return 0;
    clus = ((uint32_t)rd16(c->buf + eoff + 20) << 16) | rd16(c->buf + eoff + 26);
    c->buf[eoff] = 0xE5; cache_put(c);
    if (clus >= 2) fat_free_chain(v, clus);
    cache_sync(); cache_drop(v->dev);
    return 1;
}

/* ---- public: mkdir -------------------------------------------------------- */
int uno_fat_mkdir(int vol, const char *path)
{
    fatvol *v; uint32_t pclus; int fixed; uint8_t leaf[11];
    uint32_t elba; int eoff; cline *c;
    uint32_t nc, dotdot, s;
    if (vol < 0 || vol >= g_nvol) return 0;
    v = &g_vol[vol];
    if (v->dev->write == 0) return 0;
    if (!resolve_parent(v, path, &pclus, &fixed, leaf)) return 0;
    if (dir_find(v, pclus, fixed, leaf, &elba, &eoff)) return 0;  /* exists    */

    if (!dir_alloc_slot(v, pclus, fixed, &elba, &eoff)) return 0;
    c = cache_get(v->dev, elba); if (!c) return 0;
    memset(c->buf + eoff, 0, 32);
    memcpy(c->buf + eoff, leaf, 11);
    c->buf[eoff + 11] = 0x10;                         /* directory            */
    cache_put(c);   /* mark dirty NOW - same eviction trap as uno_fat_write:
                       fat_alloc's free-scan can evict the line, and a clean
                       eviction discards the name */

    nc = fat_alloc(v);
    if (!nc) {                                        /* disk full: undo slot */
        c = cache_get(v->dev, elba);
        if (c) { c->buf[eoff] = 0xE5; cache_put(c); cache_sync(); }
        return 0;
    }
    /* zero the directory's cluster (a 0x00 first byte = end-of-dir) */
    for (s = 0; s < v->sec_per_clus; s++) {
        cline *cc = cache_get(v->dev, clus_lba(v, nc) + s);
        if (!cc) return 0;
        memset(cc->buf, 0, SECT); cache_put(cc);
    }
    /* "." = this dir; ".." = the parent - except the root, whose ".." link is
       cluster 0 by the FAT spec (even on FAT32, where the real root cluster
       is elsewhere) */
    dotdot = (fixed || (v->fat32 && pclus == v->root_clus)) ? 0 : pclus;
    {
        cline *cc = cache_get(v->dev, clus_lba(v, nc));
        if (!cc) return 0;
        memcpy(cc->buf, ".          ", 11);
        cc->buf[11] = 0x10;
        wr16(cc->buf + 26, (uint16_t)(nc & 0xFFFF));
        wr16(cc->buf + 20, (uint16_t)((nc >> 16) & 0xFFFF));
        memcpy(cc->buf + 32, "..         ", 11);
        cc->buf[32 + 11] = 0x10;
        wr16(cc->buf + 32 + 26, (uint16_t)(dotdot & 0xFFFF));
        wr16(cc->buf + 32 + 20, (uint16_t)((dotdot >> 16) & 0xFFFF));
        cache_put(cc);
    }
    /* fill the parent entry (re-fetch: fat ops may have evicted the line) */
    c = cache_get(v->dev, elba); if (!c) return 0;
    wr16(c->buf + eoff + 26, (uint16_t)(nc & 0xFFFF));            /* lo cluster */
    wr16(c->buf + eoff + 20, (uint16_t)((nc >> 16) & 0xFFFF));    /* hi cluster */
    wr32(c->buf + eoff + 28, 0);                                  /* dirs: size 0 */
    cache_put(c);
    cache_sync();
    cache_drop(v->dev);
    return 1;
}

/* ---- public: rename ------------------------------------------------------- */
int uno_fat_rename(int vol, const char *path, const char *newname)
{
    uno_fat_seq_flush();   /* chains may move under the read cursor */
    fatvol *v; uint32_t pclus; int fixed; uint8_t leaf[11], new83[11];
    uint32_t elba, dlba; int eoff, doff, i; cline *c;
    if (vol < 0 || vol >= g_nvol) return 0;
    v = &g_vol[vol];
    if (v->dev->write == 0) return 0;
    for (i = 0; newname[i]; i++)                      /* bare name, no path   */
        if (newname[i] == '\\' || newname[i] == '/') return 0;
    if (!pack83(newname, new83)) return 0;
    if (!resolve_parent(v, path, &pclus, &fixed, leaf)) return 0;
    if (!dir_find(v, pclus, fixed, leaf, &elba, &eoff)) return 0;
    if (dir_find(v, pclus, fixed, new83, &dlba, &doff)) return 0; /* taken    */
    c = cache_get(v->dev, elba); if (!c) return 0;
    memcpy(c->buf + eoff, new83, 11);
    cache_put(c);
    cache_sync(); cache_drop(v->dev);
    return 1;
}

/* ---- boot-time storage self-test (armed by a WRTEST.REQ file) ------------- */
int uno_fat_selftest(void)
{
    int vol, hit = 0;
    static unsigned char req[512], out[560];
    uno_fat_init();
    for (vol = 0; vol < g_nvol; vol++) {
        long n;
        if (g_vol[vol].dev->write == 0) continue;
        n = uno_fat_read(vol, "WRTEST.REQ", req, (long)sizeof req - 1);
        if (n < 0) continue;
        if (n > (long)sizeof req - 1) n = (long)sizeof req - 1;
        {   /* out = "<req text>[native-fat-rw]" */
            int i, k = 0;
            for (i = 0; i < n && k < (int)sizeof out - 20; i++) out[k++] = req[i];
            { const char *tag = "[native-fat-rw]"; int j; for (j = 0; tag[j]; j++) out[k++] = (unsigned char)tag[j]; }
            if (uno_fat_write(vol, "WRTEST.OK", out, k)) {
                uno_fat_delete(vol, "WRTEST.REQ");
                hit++;
            }
        }
    }
    return hit;
}
