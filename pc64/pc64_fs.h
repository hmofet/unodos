/* ===========================================================================
 * UnoDOS/pc64 - unified file-system access (pc64_fs.c).
 *
 * Volume 0 is the RAM disk (the flat store in pc64_io.c). Volumes 1.. are the
 * FAT (incl. FAT32) / other local disks the firmware has mounted, reached
 * through the UEFI Simple File System protocol - the same "firmware-as-BIOS"
 * approach the port uses for GOP and the pointer. Read-only for now.
 * ======================================================================== */
#ifndef PC64_FS_H
#define PC64_FS_H

int  uno_fs_volumes(void);                       /* number of volumes (>=1)     */
const char *uno_fs_volume_name(int vol);         /* short label, e.g. "RAM"     */

/* list the root directory of `vol`: begin() snapshots + returns the count,
   get() copies entry i's name. */
int  uno_fs_list_begin(int vol);
int  uno_fs_list_get(int vol, int idx, char *name, int max);

/* list a SUBDIRECTORY into the caller's array (no shared snapshot cache).
 * Returns the TOTAL entry count, which may exceed `maxn` - so a caller can
 * tell "these are all of them" from "there were more than I asked for", and
 * never presents a truncated list as a complete one.  0 on the RAM disk
 * (flat), on a missing directory, or on a dead firmware volume. */
/* `names` is maxn slots of `stride` bytes each; the caller picks the width and
 * says so, because this export is resolved by NAME and a width baked into two
 * headers cannot be checked against itself.  Returns the TOTAL entry count,
 * which may exceed maxn. */
int  uno_fs_list_dir(int vol, const char *dir, char *names, int stride, int maxn);

/* read a file from a volume's root; returns bytes read, or -1 */
long uno_fs_read(int vol, const char *name, unsigned char *buf, long max);

/* a file's size in bytes, or -1 if it isn't there */
long uno_fs_size(int vol, const char *name);

/* read from byte `off` - what the audio decoders stream large media through,
 * so a song never has to fit in RAM.  0 at/past EOF, -1 if not found. */
long uno_fs_read_at(int vol, const char *name, long off,
                    unsigned char *buf, long max);

/* write a file to a volume's root; 1 on success, 0 if read-only / failed */
int  uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);
int  uno_fs_writable(int vol);                   /* 1 if uno_fs_write can work  */

/* create a single directory (its parent must already exist); 1 on success,
 * 0 if read-only / parent missing / already exists / unsupported backing.
 * Native FAT only - RAM disk is flat, firmware SFS is not exposed here. To lay
 * down a nested path, create each component in order (\EFI then \EFI\BOOT). */
int  uno_fs_mkdir(int vol, const char *path);

/* 1 if `path` exists as a directory on the volume, else 0 (native FAT only). */
int  uno_fs_isdir(int vol, const char *path);

/* opaque handle for the block device backing a volume (native FAT only, else 0);
 * stable across a remount, so a caller can match a target volume to its disk. */
void *uno_fs_vol_bdev(int vol);

/* recursively clone src_vol's whole tree onto dst_vol (both native FAT), using
 * caller-supplied scratch[cap] as the per-file buffer. Returns files copied, or
 * a negative error (never a silent partial). *out_bytes (nullable) = total bytes. */
int  uno_fs_copytree(int src_vol, int dst_vol, unsigned char *scratch, long cap,
                     long *out_bytes);

/* what backs a volume, and the escape hatch to the native FAT layer's richer
 * calls (subdirs, mkdir, rename - see fat.h) for volumes that have one */
int  uno_fs_kind(int vol);      /* 0 = RAM disk, 1 = native FAT, 2 = firmware SFS, -1 bad */
int  uno_fs_fat_index(int vol); /* fat.c volume index when kind==1, else -1 */

/* 1 when this volume sits on the disk UnoDOS booted from.  Lets a caller that
 * must persist state choose the medium the machine actually came up on, rather
 * than whichever disk happens to enumerate first - see unosecure.c pick_vol(). */
int  uno_fs_is_boot(int vol);

/* WHERE PERSISTENT STATE BELONGS, in one place instead of three.
 *
 * Volume 0 is the RAM disk, so "the first writable volume" writes state to a
 * filesystem that dies with the power.  Worse, the lowest-indexed writable
 * NATIVE volume is whichever disk enumerated first, and on the ZimaBlade that
 * is an internal eMMC which enumerates, reports writable, and never completes
 * a transfer: the write does not return and the machine stops dead, with no
 * fault and nothing on disk.  That cost most of a day twice, once through
 * SHELL.CFG and once through unosecure's store, because each had its own copy
 * of the heuristic.
 *
 * So: the boot volume first, since it is the one medium the machine has
 * already proved it can read and write; then any other native FAT volume;
 * then anything writable; then the RAM disk, which at least does not hang.
 *
 * pc64_uui.c session_vol() and unosecure.c pick_vol() still carry their own
 * copies of this and should be moved onto it - both belong to other lanes. */
int  uno_fs_pref_vol(void);

/* M3 detach: rebuild the volume map after the block-device set changed */
void uno_fs_remap(void);

#endif
