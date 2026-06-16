/* unofs — portable FAT12 policy (Layer-2), CONTRACT-ARCH §12 worked example.
 *
 * unofs_core implements mount/open/read/write/close/readdir + cluster-chain walk,
 * 12-bit FAT parse, consecutive-cluster batching, and owner-based handle reaping —
 * all over the `block` service (Layer-1 mechanism). Geometry and the on-disk dirent
 * / in-memory handle layouts come from the generated Contract (unodef.h), so this
 * file hard-codes no magic numbers. A port supplies only a `block` backend.
 */
#ifndef UNOFS_H
#define UNOFS_H
#include <stdint.h>
#include <stddef.h>
#include "unodef.h"          /* generated: FAT12_*, dirent_t, file_handle_t, FS_ERR_* */

/* ---- Layer-1 `block` service (CONTRACT-ARCH §3.1 `service block`) ----------
 * read/write `count` sectors at absolute `lba`; sector_size() reports geometry.
 * Returns 0 on success, non-zero on I/O error. */
typedef struct uno_block {
    void *ctx;
    int      (*read)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int      (*write)(void *ctx, uint32_t lba, uint32_t count, const void *buf);
    uint32_t (*sector_size)(void *ctx);
    uint32_t (*sector_count)(void *ctx);          /* total sectors on the device */
} uno_block_t;

/* ---- mounted filesystem state (FAT + root dir held resident) -------------- */
typedef struct {
    uno_block_t *blk;
    int          writable;
    uint8_t     *fat;                 /* FAT12_SECTORS_PER_FAT sectors            */
    uint8_t     *root;                /* root_dir sectors                         */
    uint32_t     fat_bytes, root_bytes;
    file_handle_t handles[FILE_MAX_HANDLES];
    int          dirent_idx[FILE_MAX_HANDLES];    /* root-dir slot backing each handle */
} unofs_t;

/* ---- API. All return 0 / >=0 on success, negative FS_ERR_* on failure. ----- */
int  unofs_mount(unofs_t *fs, uno_block_t *blk, int writable);
void unofs_unmount(unofs_t *fs);

/* readdir: *iter starts at 0; fills `out` and advances; returns 1=entry,0=end. */
int  unofs_readdir(unofs_t *fs, int *iter, dirent_t *out);

/* open existing file by dotted name (e.g. "CLOCK.BIN"); returns handle id 0..15. */
int  unofs_open(unofs_t *fs, const char *dotted, uint8_t owner);
int  unofs_read(unofs_t *fs, int h, void *dst, uint32_t n);   /* bytes read       */
int  unofs_close(unofs_t *fs, int h);

/* create a new file, write its bytes, and return the handle (left open). */
int  unofs_create(unofs_t *fs, const char *dotted, uint8_t owner);
int  unofs_write(unofs_t *fs, int h, const void *src, uint32_t n);
int  unofs_delete(unofs_t *fs, const char *dotted);

/* owner-based reaping on a kill path (PORT-SPEC §6.7): close every handle whose
 * owner byte == `owner`. Returns count reaped. Kernel handles (0xFF) never reaped. */
int  unofs_reap(unofs_t *fs, uint8_t owner);

#endif /* UNOFS_H */
