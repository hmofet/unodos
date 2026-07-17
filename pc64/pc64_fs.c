/* ===========================================================================
 * UnoDOS/pc64 - unified file system (see pc64_fs.h).
 *
 * Volume 0 = the RAM disk (pc64_io.c: uno_ramfs_*). Volumes 1.. = the FAT /
 * local disks the firmware mounted, via the EFI Simple File System wrappers in
 * uefi_main.c (uno_efifs_*). One flat namespace per volume; read-only.
 * ======================================================================== */
#include "pc64_fs.h"

/* pc64_io.c (RAM disk) */
int  uno_ramfs_count(void);
int  uno_ramfs_name(int idx, char *out, int max);
long uno_ramfs_read(const char *name, unsigned char *buf, long max);

/* uefi_main.c (EFI Simple File System) */
int  uno_efifs_volumes(void);
int  uno_efifs_snapshot(int vol, char (*names)[32], int maxn);
long uno_efifs_read(int vol, const char *name, unsigned char *buf, long max);

int uno_fs_volumes(void) { return 1 + uno_efifs_volumes(); }

const char *uno_fs_volume_name(int vol) { return vol == 0 ? "RAM" : "Disk"; }

/* snapshot cache for the current EFI volume listing (RAM lists live) */
static char g_cache[64][32];
static int  g_cache_n, g_cache_vol = -2;

int uno_fs_list_begin(int vol)
{
    if (vol == 0) return uno_ramfs_count();
    g_cache_vol = vol;
    g_cache_n = uno_efifs_snapshot(vol - 1, g_cache, 64);
    return g_cache_n;
}
int uno_fs_list_get(int vol, int idx, char *name, int max)
{
    if (max <= 0) return 0;
    if (vol == 0) return uno_ramfs_name(idx, name, max);
    if (vol != g_cache_vol || idx < 0 || idx >= g_cache_n) return 0;
    { int j; for (j = 0; j < max - 1 && g_cache[idx][j]; j++) name[j] = g_cache[idx][j]; name[j] = 0; }
    return 1;
}
long uno_fs_read(int vol, const char *name, unsigned char *buf, long max)
{
    if (vol == 0) return uno_ramfs_read(name, buf, max);
    return uno_efifs_read(vol - 1, name, buf, max);
}
