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

/* read a file from a volume's root; returns bytes read, or -1 */
long uno_fs_read(int vol, const char *name, unsigned char *buf, long max);

#endif
