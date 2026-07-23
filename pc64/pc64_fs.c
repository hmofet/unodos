/* ===========================================================================
 * UnoDOS/pc64 - unified file system (see pc64_fs.h).
 *
 * Volume layering, in order:
 *   0                     the RAM disk (pc64_io.c: uno_ramfs_*)
 *   1 .. 1+nfat           NATIVE FAT16/32 mounts (fat.c over the block layer -
 *                         AHCI or firmware-sector transport; no firmware FAT
 *                         code in the path).  This is where an installed
 *                         UnoDOS lives - any FAT32 partition on any disk.
 *   1+nfat .. end         remaining firmware Simple-File-System volumes that
 *                         the native stack did NOT already mount (e.g. the boot
 *                         USB behind xHCI, which has no native block driver),
 *                         deduplicated against the native mounts by FAT serial.
 *
 * Read AND write.  Native FAT and the RAM disk are writable; firmware SFS is
 * read-only here (its write path stays in installer.c where it is proven).
 * Names are 8.3 in the root of each volume; the native FAT layer also accepts
 * backslash subdir paths for the app loader / font fallback.
 * ======================================================================== */
#include "pc64_fs.h"
#include "fat.h"
#include "unoauto.h"     /* fs.read / fs.write tap points (no-op in prod) */

/* pc64_io.c (RAM disk) */
int  uno_ramfs_count(void);
int  uno_ramfs_name(int idx, char *out, int max);
long uno_ramfs_read(const char *name, unsigned char *buf, long max);
long uno_ramfs_read_at(const char *name, long off, unsigned char *buf, long max);
long uno_ramfs_size(const char *name);
int  uno_ramfs_write(const char *name, const unsigned char *buf, long len);

/* uefi_main.c (EFI Simple File System) */
int  uno_efifs_volumes(void);
int  uno_efifs_snapshot(int vol, char (*names)[32], int maxn);
long uno_efifs_read(int vol, const char *name, unsigned char *buf, long max);
long uno_efifs_read_at(int vol, const char *name, long off,
                       unsigned char *buf, long max);
long uno_efifs_write(int vol, const char *name, const unsigned char *buf, long len);
long uno_efifs_size(int vol, const char *name);
unsigned int uno_efifs_serial(int vol);          /* BPB volume id, 0 unknown  */

/* ---- volume map ----------------------------------------------------------- *
 * Built once: an ordered list of (kind, index) pairs.  kind 0 = RAM,
 * 1 = native FAT (index into fat.c), 2 = firmware SFS (index into efifs). */
#define KIND_RAM 0
#define KIND_FAT 1
#define KIND_FW  2
#define MAXMAP   20
static struct { int kind, idx; } g_map[MAXMAP];
static int g_nmap, g_mapped;

int uno_pc64_detached(void);                              /* uefi_main.c (M3)  */

static void build_map(void)
{
    int i, nfat, m = 0;
    if (g_mapped) return;
    g_mapped = 1;
    uno_fat_init();

    g_map[m].kind = KIND_RAM; g_map[m].idx = 0; m++;      /* volume 0 = RAM    */

    nfat = uno_fat_volumes();
    for (i = 0; i < nfat && m < MAXMAP; i++) { g_map[m].kind = KIND_FAT; g_map[m].idx = i; m++; }

    /* firmware SFS volumes exist only while boot services are live */
    if (!uno_pc64_detached()) {
        int nfw = uno_efifs_volumes();
        for (i = 0; i < nfw && m < MAXMAP; i++) {
            unsigned int fs = uno_efifs_serial(i);
            int dup = 0, j;
            if (fs) for (j = 0; j < nfat; j++) if (uno_fat_serial(j) == fs) { dup = 1; break; }
            if (dup) continue;                            /* same disk, native */
            g_map[m].kind = KIND_FW; g_map[m].idx = i; m++;
        }
    }
    g_nmap = m;
}

/* a firmware-SFS volume left in a stale map is unreachable once detached */
static int fw_dead(int vol)
{ return g_map[vol].kind == KIND_FW && uno_pc64_detached(); }

int uno_fs_volumes(void) { build_map(); return g_nmap; }

const char *uno_fs_volume_name(int vol)
{
    build_map();
    if (vol < 0 || vol >= g_nmap) return "?";
    switch (g_map[vol].kind) {
    case KIND_RAM: return "RAM";
    case KIND_FAT: { const char *l = uno_fat_label(g_map[vol].idx);
                     return (l && l[0] && l[0] != ' ') ? l : "Disk"; }
    default:       return "USB";
    }
}

/* snapshot cache for a listing (RAM + native FAT list live; firmware cached) */
static char g_cache[64][32];
static int  g_cache_n, g_cache_vol = -2;

/* M3 detach: the device set changed under us - rebuild the volume map (the
 * caller already ran uno_blk_detach + uno_fat_remount) */
void uno_fs_remap(void)
{
    g_mapped = 0;
    g_cache_vol = -2;
    build_map();
}

int uno_fs_list_begin(int vol)
{
    build_map();
    if (vol < 0 || vol >= g_nmap || fw_dead(vol)) return 0;
    if (g_map[vol].kind == KIND_RAM) return uno_ramfs_count();
    if (g_map[vol].kind == KIND_FAT) {
        static char fn[64][13];
        int n = uno_fat_list(g_map[vol].idx, 0, fn, 64), i;
        for (i = 0; i < n && i < 64; i++) {
            int j; for (j = 0; j < 12 && fn[i][j]; j++) g_cache[i][j] = fn[i][j]; g_cache[i][j] = 0;
        }
        g_cache_n = n < 64 ? n : 64; g_cache_vol = vol;
        return g_cache_n;
    }
    g_cache_vol = vol;
    g_cache_n = uno_efifs_snapshot(g_map[vol].idx, g_cache, 64);
    return g_cache_n;
}

int uno_fs_list_get(int vol, int idx, char *name, int max)
{
    build_map();
    if (max <= 0 || vol < 0 || vol >= g_nmap) return 0;
    if (g_map[vol].kind == KIND_RAM) return uno_ramfs_name(idx, name, max);
    if (vol != g_cache_vol || idx < 0 || idx >= g_cache_n) return 0;
    { int j; for (j = 0; j < max - 1 && g_cache[idx][j]; j++) name[j] = g_cache[idx][j]; name[j] = 0; }
    return 1;
}

long uno_fs_read(int vol, const char *name, unsigned char *buf, long max)
{
    return uno_fs_read_at(vol, name, 0, buf, max);
}

/* Read from byte `off`.  Media files are far larger than anything the port
 * wants resident, so the audio decoders stream through this instead of
 * slurping whole files; every backend has a real seek behind it. */
long uno_fs_read_at(int vol, const char *name, long off,
                    unsigned char *buf, long max)
{
    { UnoAutoFsEv ev; ev.vol = vol; ev.path = name; ev.len = max;
      unoauto_hook_fire("fs.read", &ev); }
    build_map();
    if (vol < 0 || vol >= g_nmap || fw_dead(vol)) return -1;
    if (g_map[vol].kind == KIND_RAM) return uno_ramfs_read_at(name, off, buf, max);
    if (g_map[vol].kind == KIND_FAT) return uno_fat_read_at(g_map[vol].idx, name, off, buf, max);
    return uno_efifs_read_at(g_map[vol].idx, name, off, buf, max);
}

long uno_fs_size(int vol, const char *name)
{
    build_map();
    if (vol < 0 || vol >= g_nmap || fw_dead(vol)) return -1;
    if (g_map[vol].kind == KIND_RAM) return uno_ramfs_size(name);
    if (g_map[vol].kind == KIND_FAT) return uno_fat_size(g_map[vol].idx, name);
    return uno_efifs_size(g_map[vol].idx, name);
}

/* write a file to a volume's root; 1 on success, 0 if the volume is read-only
 * or the write failed.  RAM disk + native FAT are writable; firmware SFS is not. */
int uno_fs_write(int vol, const char *name, const unsigned char *buf, long len)
{
    { UnoAutoFsEv ev; ev.vol = vol; ev.path = name; ev.len = len;
      unoauto_hook_fire("fs.write", &ev); }
    build_map();
    if (vol < 0 || vol >= g_nmap) return 0;
    if (g_map[vol].kind == KIND_RAM) return uno_ramfs_write(name, buf, len);
    if (g_map[vol].kind == KIND_FAT) return uno_fat_write(g_map[vol].idx, name, buf, len);
    /* firmware SFS: writable while boot services are live (dead after detach).
     * This is how an ATTACHED machine writes its USB stick - the A/B OS-update
     * path in the remote channel. */
    if (fw_dead(vol)) return 0;
    return uno_efifs_write(g_map[vol].idx, name, buf, len) ? 1 : 0;
}

int uno_fs_writable(int vol)
{
    build_map();
    if (vol < 0 || vol >= g_nmap) return 0;
    if (g_map[vol].kind == KIND_FW) return !fw_dead(vol);   /* fw SFS: RW while attached */
    return g_map[vol].kind == KIND_RAM || g_map[vol].kind == KIND_FAT;
}

/* create one directory (its parent must already exist).  Native FAT only:
 * the RAM disk is flat (no subdirs) and the firmware SFS mkdir path is not
 * exposed here.  1 on success, 0 otherwise - see uno_fat_mkdir (fat.c). */
int uno_fs_mkdir(int vol, const char *path)
{
    build_map();
    if (vol < 0 || vol >= g_nmap) return 0;
    if (g_map[vol].kind == KIND_FAT) return uno_fat_mkdir(g_map[vol].idx, path);
    return 0;
}

/* 1 if `path` names an existing directory (native FAT only): listing it as a
 * directory succeeds (>= 0) only when it exists and is a dir - a missing name
 * or a plain file fails.  Used to make `mkdir` idempotent over the remote link. */
int uno_fs_isdir(int vol, const char *path)
{
    build_map();
    if (vol < 0 || vol >= g_nmap) return 0;
    if (g_map[vol].kind == KIND_FAT) {
        uno_fat_entry e;
        return uno_fat_list_ex(g_map[vol].idx, path, &e, 1) >= 0;
    }
    return 0;
}

/* the block device backing a volume, as an opaque handle (native FAT only, else
 * 0).  Stable across a remount, so a caller can match a freshly-formatted
 * target volume to the disk it armed.  Returned as void* to keep blkdev.h out
 * of this header. */
void *uno_fs_vol_bdev(int vol)
{
    build_map();
    if (vol < 0 || vol >= g_nmap || g_map[vol].kind != KIND_FAT) return 0;
    return (void *)uno_fat_dev(g_map[vol].idx);
}

/* Recursively clone every file + directory from src_vol's root onto dst_vol
 * (both native FAT).  `scratch`/`cap` is the per-file copy buffer supplied by
 * the caller (must be >= the largest source file).  Returns the number of files
 * copied, or a negative error (-2 a directory has too many entries, -3 the tree
 * is too deep/wide, -4 a file exceeds the buffer, -5/-6 read/write failure) -
 * never a silent partial copy.  *out_bytes (nullable) gets the total bytes.
 * Iterative BFS with bounded, stack-local work state (no recursion, no BSS). */
int uno_fs_copytree(int src_vol, int dst_vol, unsigned char *scratch, long cap,
                    long *out_bytes)
{
    int sfat, dfat, head = 0, tail = 0, files = 0;
    long bytes = 0;
    char dq[32][96];                 /* BFS queue of source subdirectory paths  */
    uno_fat_entry ents[64];          /* one directory's entries at a time       */
    build_map();
    sfat = uno_fs_fat_index(src_vol);
    dfat = uno_fs_fat_index(dst_vol);
    if (sfat < 0 || dfat < 0 || !scratch || cap <= 0) return -1;

    dq[tail][0] = 0; tail++;         /* seed with the root ("") */
    while (head < tail) {
        char dir[96];
        const char *listdir;
        int n, i, k;
        for (k = 0; dq[head][k] && k < 95; k++) dir[k] = dq[head][k];
        dir[k] = 0; head++;
        listdir = dir[0] ? dir : 0;      /* NULL = root, per dir_locate (fat.c) */
        n = uno_fat_list_ex(sfat, listdir, ents, 64);
        if (n > 64) return -2;                    /* directory too large         */
        for (i = 0; i < n; i++) {
            char path[96];
            const char *s;
            long got;
            k = 0;
            for (s = dir; *s && k < 95; ) path[k++] = *s++;
            if (k == 0 || path[k - 1] != '\\') { if (k < 95) path[k++] = '\\'; }
            for (s = ents[i].name; *s && k < 95; ) path[k++] = *s++;
            path[k] = 0;
            if (ents[i].is_dir) {
                uno_fat_mkdir(dfat, path);        /* harmless if it already exists */
                if (tail >= 32) return -3;        /* queue full: tree too wide/deep */
                for (k = 0; path[k] && k < 95; k++) dq[tail][k] = path[k];
                dq[tail][k] = 0; tail++;
            } else {
                if (ents[i].size > cap) return -4;
                got = uno_fat_read(sfat, path, scratch, cap);
                if (got < 0) return -5;
                if (!uno_fat_write(dfat, path, scratch, got)) return -6;
                bytes += got; files++;
            }
        }
    }
    if (out_bytes) *out_bytes = bytes;
    return files;
}

/* what backs a volume: KIND_RAM / KIND_FAT / KIND_FW, or -1 for a bad index */
int uno_fs_kind(int vol)
{
    build_map();
    if (vol < 0 || vol >= g_nmap) return -1;
    return g_map[vol].kind;
}

/* fat.c volume index behind a native-FAT volume (for the richer uno_fat_*
 * calls - subdirs, mkdir, rename); -1 unless uno_fs_kind(vol) == 1 */
int uno_fs_fat_index(int vol)
{
    build_map();
    if (vol < 0 || vol >= g_nmap || g_map[vol].kind != KIND_FAT) return -1;
    return g_map[vol].idx;
}
