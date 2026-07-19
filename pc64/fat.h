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

/* root or subdir listing: begin() returns file count + snapshots names,
 * get() copies entry i.  dir "" or NULL = root; else a backslash path. */
int  uno_fat_list(int vol, const char *dir, char (*names)[13], int maxn);

/* read a file by path; returns bytes read (<= max), or -1 if not found */
long uno_fat_read(int vol, const char *path, unsigned char *buf, long max);

/* create/overwrite a file with exactly len bytes; makes no directories.
 * returns 1 on success. */
int  uno_fat_write(int vol, const char *path, const unsigned char *buf, long len);

/* delete a file; returns 1 if it existed and was removed. */
int  uno_fat_delete(int vol, const char *path);

/* Boot-time storage self-test (inert unless armed): on any writable FAT volume
 * holding "WRTEST.REQ", read it, write "WRTEST.OK" containing that text plus a
 * native-FAT-rw tag, then delete the request.  Proves read+write+delete over
 * whatever sector transport is active, on a deterministic target, with zero
 * effect on a normal system (no request file -> no-op).  Returns volumes hit. */
int  uno_fat_selftest(void);

#endif
