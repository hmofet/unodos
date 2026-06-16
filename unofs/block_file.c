/* block_file — host file-backed `block` backend (a .img is a host file).
 * The §15 host reference mechanism: unofs_core's L1 over an ordinary file. */
#include "unofs.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { FILE *f; uint32_t bps, secs; } blkfile_t;

static int bf_read(void *ctx, uint32_t lba, uint32_t count, void *buf) {
    blkfile_t *b = ctx;
    if (fseek(b->f, (long)lba * b->bps, SEEK_SET)) return 1;
    return fread(buf, b->bps, count, b->f) == count ? 0 : 1;
}
static int bf_write(void *ctx, uint32_t lba, uint32_t count, const void *buf) {
    blkfile_t *b = ctx;
    if (fseek(b->f, (long)lba * b->bps, SEEK_SET)) return 1;
    if (fwrite(buf, b->bps, count, b->f) != count) return 1;
    fflush(b->f);
    return 0;
}
static uint32_t bf_size(void *ctx)  { return ((blkfile_t *)ctx)->bps; }
static uint32_t bf_count(void *ctx) { return ((blkfile_t *)ctx)->secs; }

/* opens `path` and wires `out` to it; returns 0 on success. */
int blkfile_open(uno_block_t *out, const char *path, int writable) {
    blkfile_t *b = calloc(1, sizeof *b);
    if (!b) return 1;
    b->f = fopen(path, writable ? "r+b" : "rb");
    if (!b->f) { free(b); return 1; }
    fseek(b->f, 0, SEEK_END);
    long sz = ftell(b->f);
    b->bps  = FAT12_BYTES_PER_SECTOR;
    b->secs = (uint32_t)(sz / b->bps);
    out->ctx = b;
    out->read = bf_read; out->write = bf_write;
    out->sector_size = bf_size; out->sector_count = bf_count;
    return 0;
}
void blkfile_close(uno_block_t *blk) {
    blkfile_t *b = blk->ctx;
    if (b) { if (b->f) fclose(b->f); free(b); }
    blk->ctx = NULL;
}
