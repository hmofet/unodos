/* fat.h - native FAT16/32 read+write over the block layer (see fat.c).
 *
 * Mounts every FAT16/32 partition found on every uno_blk device (GPT, MBR, or
 * superfloppy) - firmware plays no part beyond (optionally) moving sectors for
 * the fallback backend.  8.3 names; backslash-separated subdir paths
 * ("EFI\UNODOS\SANS.TTF").  This is what lets UnoDOS live on a plain FAT32
 * partition on any disk instead of "the ESP the firmware happened to expose".
 */
#ifndef PC64_FAT_H
#define PC64_FAT_H

/* Scan all block devices + mount FAT volumes.  Idempotent. */
void uno_fat_init(void);

/* M3 detach support: flush dirty cache lines (while the old transport is
 * still alive); re-scan volumes over the current device set (after the
 * transport changed - drops the cache unflushed); detach-eligibility gate
 * (a FAT volume lives on the disk the native AHCI driver will reach). */
void uno_fat_sync(void);
void uno_fat_remount(void);
int  uno_fat_native_eligible(void);

int         uno_fat_volumes(void);
const char *uno_fat_label(int vol);              /* 11-char volume label / ""  */
unsigned int uno_fat_serial(int vol);            /* BPB volume id (dedup key)  */
struct uno_bdev *uno_fat_dev(int vol);           /* backing block device / 0   */

/* root or subdir listing: begin() returns file count + snapshots names,
 * get() copies entry i.  dir "" or NULL = root; else a backslash path. */
int  uno_fat_list(int vol, const char *dir, char (*names)[13], int maxn);

/* like uno_fat_list, but with metadata (dir flag + byte size) and INCLUDING
 * subdirectory entries; "."/"..", labels, LFN and deleted entries are skipped.
 * Fills up to maxn entries; returns the total count (may exceed maxn). */
typedef struct { char name[13]; int is_dir; long size; } uno_fat_entry;
int  uno_fat_list_ex(int vol, const char *dir, uno_fat_entry *ents, int maxn);

/* read a file by path; returns bytes read (<= max), or -1 if not found */
long uno_fat_read(int vol, const char *path, unsigned char *buf, long max);

/* a file's size in bytes, or -1 if not found */
long uno_fat_size(int vol, const char *path);

/* drop the sequential-read cursor (see fat.c) - called automatically by the
 * write/delete/rename/remount paths; exposed for anything that moves sectors
 * behind this driver's back. */
void uno_fat_seq_flush(void);

/* read from byte `off` - what the audio decoders stream large media through,
 * so a file never has to fit in RAM.  Returns bytes read (0 at/past EOF), or
 * -1 if not found. */
long uno_fat_read_at(int vol, const char *path, long off,
                     unsigned char *buf, long max);

/* create/overwrite a file with exactly len bytes; makes no directories.
 * returns 1 on success. */
int  uno_fat_write(int vol, const char *path, const unsigned char *buf, long len);

/* delete a file; returns 1 if it existed and was removed. */
int  uno_fat_delete(int vol, const char *path);

/* create ONE directory level ("DOCS" or "EFI\DOCS"; the parent must already
 * exist).  Returns 1 on success, 0 on failure - an existing entry of that
 * name counts as failure (harmless: nothing is touched). */
int  uno_fat_mkdir(int vol, const char *path);

/* rename a file or subdirectory within its directory (newname is a bare 8.3
 * name, no path).  Returns 1 on success, 0 if the source is missing or the
 * new name is already taken there. */
int  uno_fat_rename(int vol, const char *path, const char *newname);

/* Boot-time storage self-test (inert unless armed): on any writable FAT volume
 * holding "WRTEST.REQ", read it, write "WRTEST.OK" containing that text plus a
 * native-FAT-rw tag, then delete the request.  Proves read+write+delete over
 * whatever sector transport is active, on a deterministic target, with zero
 * effect on a normal system (no request file -> no-op).  Returns volumes hit. */
int  uno_fat_selftest(void);

/* Format [first_lba, first_lba+sectors) on a raw block device as FAT32 (the
 * mount-compatible layout of flash/UnoDisk.cs).  Writes RAW via dev->write, so
 * the caller must uno_fat_remount() afterwards to surface the new volume.  1 on
 * success, 0 on failure (read-only device / geometry too small for FAT32).
 * The disk-authoring framework unostorage_prepare_esp() calls this. */
struct uno_bdev;
int  uno_fat_mkfs(struct uno_bdev *dev, unsigned long long first_lba,
                  unsigned long long sectors, const char *label);

#endif
