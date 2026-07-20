/* unofs_core — portable FAT12 policy over the `block` service. See unofs.h.
 * Algorithm mirrors the x86 kernel (kernel/kernel.asm fs_*); geometry + layouts
 * are the generated Contract constants, so nothing here is a magic number. */
#include "unofs.h"
#include <stdlib.h>
#include <string.h>

#define BPS  FAT12_BYTES_PER_SECTOR
#define SPC  FAT12_SECTORS_PER_CLUSTER
#define CB   (BPS * SPC)                         /* bytes per cluster */
#define EOC  0xFF8                               /* FAT12 end-of-chain threshold */

static int  upc(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static uint32_t cluster_lba(uint32_t cl) { return FAT12_DATA_AREA_START + (cl - 2) * SPC; }

/* 12-bit FAT entry get/set over the resident FAT image (handles the straddle). */
/* A crafted image can point start_cluster / a chain link at any value below the
 * EOC threshold (0xFF8), and o = cl*3/2 then indexes past the resident FAT buffer
 * (fat_bytes). Bound every access: an out-of-range cluster reads as end-of-chain
 * and its write is refused, closing the OOB read (fat_get) and OOB heap write
 * (fat_set on delete/append). */
static uint16_t fat_get(unofs_t *fs, uint32_t cl) {
    uint32_t o = cl + (cl >> 1);                 /* cl * 3/2 */
    uint16_t w;
    if (o + 1 >= fs->fat_bytes) return 0xFFF;    /* out of range -> end-of-chain */
    w = (uint16_t)(fs->fat[o] | (fs->fat[o + 1] << 8));
    return (cl & 1) ? (w >> 4) : (w & 0x0FFF);
}
static void fat_set(unofs_t *fs, uint32_t cl, uint16_t v) {
    uint32_t o = cl + (cl >> 1);
    uint16_t w;
    if (o + 1 >= fs->fat_bytes) return;          /* out of range -> refuse the write */
    w = (uint16_t)(fs->fat[o] | (fs->fat[o + 1] << 8));
    if (cl & 1) w = (uint16_t)((w & 0x000F) | (v << 4));
    else        w = (uint16_t)((w & 0xF000) | (v & 0x0FFF));
    fs->fat[o]     = (uint8_t)(w & 0xFF);
    fs->fat[o + 1] = (uint8_t)(w >> 8);
}

/* "CLOCK.BIN" -> 11-byte 8.3 space-padded "CLOCK   BIN" */
static void to_83(const char *dotted, char out[11]) {
    memset(out, ' ', 11);
    int i = 0, o = 0;
    for (; dotted[i] && dotted[i] != '.' && o < 8; i++) out[o++] = (char)upc(dotted[i]);
    while (dotted[i] && dotted[i] != '.') i++;
    if (dotted[i] == '.') i++;
    for (o = 8; dotted[i] && o < 11; i++) out[o++] = (char)upc(dotted[i]);
}

/* ---- mount: load both resident regions ----------------------------------- */
int unofs_mount(unofs_t *fs, uno_block_t *blk, int writable) {
    memset(fs, 0, sizeof *fs);
    if (blk->sector_size(blk->ctx) != BPS) return -FS_ERR_READ_ERROR;
    fs->blk = blk; fs->writable = writable;
    fs->fat_bytes  = FAT12_SECTORS_PER_FAT * BPS;
    fs->root_bytes = FAT12_ROOT_DIR_SECTORS * BPS;
    fs->fat  = malloc(fs->fat_bytes);
    fs->root = malloc(fs->root_bytes);
    if (!fs->fat || !fs->root) return -FS_ERR_NO_HANDLES;
    if (blk->read(blk->ctx, FAT12_FAT_START, FAT12_SECTORS_PER_FAT, fs->fat))
        return -FS_ERR_READ_ERROR;
    if (blk->read(blk->ctx, FAT12_ROOT_DIR_START, FAT12_ROOT_DIR_SECTORS, fs->root))
        return -FS_ERR_READ_ERROR;
    return 0;
}

void unofs_unmount(unofs_t *fs) { free(fs->fat); free(fs->root); fs->fat = fs->root = NULL; }

static dirent_t *dirent_at(unofs_t *fs, int idx) {
    return (dirent_t *)(fs->root + idx * DIRENT_SIZE);
}

/* ---- readdir over the resident root ---------------------------------------*/
int unofs_readdir(unofs_t *fs, int *iter, dirent_t *out) {
    while (*iter < FAT12_ROOT_DIR_ENTRIES) {
        dirent_t *e = dirent_at(fs, *iter);
        (*iter)++;
        uint8_t c0 = (uint8_t)e->name[0];
        if (c0 == 0x00) return 0;                 /* 0x00 = no more entries */
        if (c0 == 0xE5) continue;                 /* deleted */
        if (e->attr == 0x0F) continue;            /* LFN */
        if (e->attr & 0x08) continue;             /* volume label */
        memcpy(out, e, sizeof *out);
        return 1;
    }
    return 0;
}

static int find_entry_idx(unofs_t *fs, const char *dotted) {
    char want[11]; to_83(dotted, want);
    for (int i = 0; i < FAT12_ROOT_DIR_ENTRIES; i++) {
        dirent_t *e = dirent_at(fs, i);
        if ((uint8_t)e->name[0] == 0x00) break;
        if ((uint8_t)e->name[0] == 0xE5) continue;
        if (memcmp(e->name, want, 11) == 0) return i;
    }
    return -FS_ERR_NOT_FOUND;
}

static int alloc_handle(unofs_t *fs) {
    for (int i = 0; i < FILE_MAX_HANDLES; i++)
        if (fs->handles[i].status == 0) return i;
    return -FS_ERR_NO_HANDLES;
}

int unofs_open(unofs_t *fs, const char *dotted, uint8_t owner) {
    int ei = find_entry_idx(fs, dotted);
    if (ei < 0) return ei;
    int h = alloc_handle(fs);
    if (h < 0) return h;
    dirent_t *e = dirent_at(fs, ei);
    file_handle_t *fh = &fs->handles[h];
    memset(fh, 0, sizeof *fh);
    fh->status = 1; fh->mount = 0;
    fh->start_cluster = e->start_cluster;
    fh->size = e->size;
    fh->position = 0; fh->owner = owner;
    fs->dirent_idx[h] = ei;
    return h;
}

/* read with consecutive-cluster batching: extend a run while the chain stays
 * physically contiguous, issue one multi-sector block read per run, copy the
 * needed slice (bounce only the partial tail). */
int unofs_read(unofs_t *fs, int h, void *dst, uint32_t n) {
    if (h < 0 || h >= FILE_MAX_HANDLES || !fs->handles[h].status) return -FS_ERR_INVALID_HANDLE;
    file_handle_t *fh = &fs->handles[h];
    uint32_t avail = fh->size - fh->position;
    if (n > avail) n = avail;
    if (n == 0) return 0;

    uint32_t cl = fh->start_cluster;
    for (uint32_t skip = fh->position / CB; skip; skip--) cl = fat_get(fs, cl);
    uint32_t off = fh->position % CB, got = 0;
    uint8_t *out = (uint8_t *)dst;

    while (got < n && cl >= 2 && cl < EOC) {
        uint32_t run_start = cl, run = 1;
        while (got + run * CB - off < n) {        /* need more — extend the run? */
            uint16_t nx = fat_get(fs, cl);
            if (nx != cl + 1 || nx >= EOC) break;  /* not contiguous / end */
            cl = nx; run++;
        }
        uint8_t *tmp = malloc(run * CB);
        if (!tmp) return -FS_ERR_READ_ERROR;
        if (fs->blk->read(fs->blk->ctx, cluster_lba(run_start), run * SPC, tmp)) {
            free(tmp); return -FS_ERR_READ_ERROR;
        }
        uint32_t chunk = run * CB - off;
        if (chunk > n - got) chunk = n - got;
        memcpy(out + got, tmp + off, chunk);
        free(tmp);
        got += chunk; off = 0;
        cl = fat_get(fs, cl);                      /* advance past the run */
    }
    fh->position += got;
    return (int)got;
}

int unofs_close(unofs_t *fs, int h) {
    if (h < 0 || h >= FILE_MAX_HANDLES || !fs->handles[h].status) return -FS_ERR_INVALID_HANDLE;
    fs->handles[h].status = 0;
    return 0;
}

int unofs_reap(unofs_t *fs, uint8_t owner) {
    if (owner == 0xFF) return 0;                   /* kernel handles never reaped */
    int n = 0;
    for (int i = 0; i < FILE_MAX_HANDLES; i++)
        if (fs->handles[i].status && fs->handles[i].owner == owner) {
            fs->handles[i].status = 0; n++;
        }
    return n;
}

/* ---- write path ---------------------------------------------------------- */
static int flush_meta(unofs_t *fs) {
    if (fs->blk->write(fs->blk->ctx, FAT12_FAT_START, FAT12_SECTORS_PER_FAT, fs->fat))
        return -FS_ERR_WRITE_ERROR;
    if (fs->blk->write(fs->blk->ctx, FAT12_FAT_START + FAT12_SECTORS_PER_FAT,
                       FAT12_SECTORS_PER_FAT, fs->fat))      /* FAT2 mirror */
        return -FS_ERR_WRITE_ERROR;
    if (fs->blk->write(fs->blk->ctx, FAT12_ROOT_DIR_START, FAT12_ROOT_DIR_SECTORS, fs->root))
        return -FS_ERR_WRITE_ERROR;
    return 0;
}

static uint32_t max_cluster(unofs_t *fs) {        /* last usable data cluster */
    uint32_t secs = fs->blk->sector_count(fs->blk->ctx);
    if (secs <= FAT12_DATA_AREA_START) return 2;
    return (secs - FAT12_DATA_AREA_START) / SPC + 2;
}
static int alloc_cluster(unofs_t *fs) {
    uint32_t max = max_cluster(fs);
    for (uint32_t c = 2; c < max; c++)
        if (fat_get(fs, c) == 0) return (int)c;
    return -FS_ERR_DISK_FULL;
}

int unofs_create(unofs_t *fs, const char *dotted, uint8_t owner) {
    if (!fs->writable) return -FS_ERR_WRITE_ERROR;
    char want[11]; to_83(dotted, want);
    int slot = -1;
    for (int i = 0; i < FAT12_ROOT_DIR_ENTRIES; i++) {
        uint8_t c0 = (uint8_t)dirent_at(fs, i)->name[0];
        if (c0 == 0x00 || c0 == 0xE5) { slot = i; break; }
    }
    if (slot < 0) return -FS_ERR_DIR_FULL;
    int h = alloc_handle(fs);
    if (h < 0) return h;
    dirent_t *e = dirent_at(fs, slot);
    memset(e, 0, sizeof *e);
    memcpy(e->name, want, 11);
    e->attr = 0x20;                                /* archive */
    e->start_cluster = 0; e->size = 0;
    file_handle_t *fh = &fs->handles[h];
    memset(fh, 0, sizeof *fh);
    fh->status = 1; fh->owner = owner;
    fs->dirent_idx[h] = slot;
    if (flush_meta(fs)) return -FS_ERR_WRITE_ERROR;
    return h;
}

int unofs_write(unofs_t *fs, int h, const void *src, uint32_t n) {
    if (!fs->writable) return -FS_ERR_WRITE_ERROR;
    if (h < 0 || h >= FILE_MAX_HANDLES || !fs->handles[h].status) return -FS_ERR_INVALID_HANDLE;
    file_handle_t *fh = &fs->handles[h];
    const uint8_t *in = (const uint8_t *)src;
    uint32_t written = 0;

    uint32_t prev = 0, cl = fh->start_cluster;     /* find chain tail (append) */
    while (cl >= 2 && cl < EOC) { prev = cl; cl = fat_get(fs, cl); }

    while (written < n) {
        int nc = alloc_cluster(fs);
        if (nc < 0) return nc;
        fat_set(fs, (uint32_t)nc, EOC);
        if (prev) fat_set(fs, prev, (uint16_t)nc); else fh->start_cluster = (uint16_t)nc;
        prev = (uint32_t)nc;

        uint8_t sec[CB];
        memset(sec, 0, sizeof sec);
        uint32_t chunk = n - written; if (chunk > CB) chunk = CB;
        memcpy(sec, in + written, chunk);
        if (fs->blk->write(fs->blk->ctx, cluster_lba((uint32_t)nc), SPC, sec))
            return -FS_ERR_WRITE_ERROR;
        written += chunk;
    }
    fh->size += written;

    dirent_t *e = dirent_at(fs, fs->dirent_idx[h]);  /* update the backing entry */
    e->start_cluster = fh->start_cluster;
    e->size = fh->size;
    if (flush_meta(fs)) return -FS_ERR_WRITE_ERROR;
    return (int)written;
}

int unofs_delete(unofs_t *fs, const char *dotted) {
    if (!fs->writable) return -FS_ERR_WRITE_ERROR;
    int ei = find_entry_idx(fs, dotted);
    if (ei < 0) return ei;
    dirent_t *e = dirent_at(fs, ei);
    uint32_t cl = e->start_cluster;
    while (cl >= 2 && cl < EOC) { uint16_t nx = fat_get(fs, cl); fat_set(fs, cl, 0); cl = nx; }
    e->name[0] = (char)0xE5;
    return flush_meta(fs);
}
