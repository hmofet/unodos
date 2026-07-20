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

/* a file's size in bytes, or -1 if it isn't there */
long uno_fs_size(int vol, const char *name);

/* read from byte `off` - what the audio decoders stream large media through,
 * so a song never has to fit in RAM.  0 at/past EOF, -1 if not found. */
long uno_fs_read_at(int vol, const char *name, long off,
                    unsigned char *buf, long max);

/* write a file to a volume's root; 1 on success, 0 if read-only / failed */
int  uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);
int  uno_fs_writable(int vol);                   /* 1 if uno_fs_write can work  */

/* what backs a volume, and the escape hatch to the native FAT layer's richer
 * calls (subdirs, mkdir, rename - see fat.h) for volumes that have one */
int  uno_fs_kind(int vol);      /* 0 = RAM disk, 1 = native FAT, 2 = firmware SFS, -1 bad */
int  uno_fs_fat_index(int vol); /* fat.c volume index when kind==1, else -1 */

/* M3 detach: rebuild the volume map after the block-device set changed */
void uno_fs_remap(void);

#endif
