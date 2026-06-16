/* unofs_test — host-first verification (CONTRACT-ARCH §12 "Host-first").
 *
 * Mounts the REAL shipping x86 floppy (build/unodos-144.img), extracts files via
 * the portable unofs_core, and byte-diffs them against the build outputs they were
 * packed from — proving the contract-driven FAT12 reader is correct before any
 * emulator. Then exercises write/re-read/delete on a scratch copy and owner-based
 * reaping. Run from the repo root.  Exit 0 = all pass.
 */
#include "unofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int  blkfile_open(uno_block_t *out, const char *path, int writable);
void blkfile_close(uno_block_t *blk);

static int fails = 0;
static void check(const char *what, int ok) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

/* read a whole host file into a malloc'd buffer */
static uint8_t *slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != (size_t)*len) { free(b); fclose(f); return NULL; }
    fclose(f);
    return b;
}

/* extract `fname` from fs and byte-compare to host file `golden` (if it exists) */
static void verify_extract(unofs_t *fs, const char *fname, const char *golden) {
    int h = unofs_open(fs, fname, 1);
    if (h < 0) { check(fname, 0); return; }
    file_handle_t *fh = &fs->handles[h];
    uint32_t sz = fh->size;
    uint8_t *got = malloc(sz ? sz : 1);
    int n = unofs_read(fs, h, got, sz);
    unofs_close(fs, h);

    long glen = 0; uint8_t *g = slurp(golden, &glen);
    char label[96];
    if (!g) {
        snprintf(label, sizeof label, "%s read %d bytes (golden %s absent — size only)", fname, n, golden);
        check(label, n == (int)sz);
    } else {
        int ok = (n == (int)sz) && (glen == (long)sz) && memcmp(got, g, sz) == 0;
        snprintf(label, sizeof label, "%s (%u B, %u clusters) == %s byte-identical",
                 fname, sz, (sz + (FAT12_BYTES_PER_SECTOR*FAT12_SECTORS_PER_CLUSTER) - 1)
                              / (FAT12_BYTES_PER_SECTOR*FAT12_SECTORS_PER_CLUSTER), golden);
        check(label, ok);
        free(g);
    }
    free(got);
}

static int copy_file(const char *src, const char *dst) {
    long len; uint8_t *b = slurp(src, &len);
    if (!b) return 1;
    FILE *f = fopen(dst, "wb");
    if (!f) { free(b); return 1; }
    fwrite(b, 1, len, f); fclose(f); free(b);
    return 0;
}

int main(void) {
    const char *IMG = "build/unodos-144.img";
    uno_block_t blk;
    unofs_t fs;

    printf("=== unofs Phase 3 — host verification against the real floppy ===\n");
    if (blkfile_open(&blk, IMG, 0)) { printf("cannot open %s (run from repo root)\n", IMG); return 2; }
    if (unofs_mount(&fs, &blk, 0)) { printf("mount failed\n"); return 2; }

    /* 1. readdir lists the volume */
    printf("--- readdir ---\n");
    int it = 0, count = 0; dirent_t de;
    while (unofs_readdir(&fs, &it, &de)) {
        char nm[12]; memcpy(nm, de.name, 11); nm[11] = 0;
        printf("    %-11.11s  %7u B  clus %u\n", nm, de.size, de.start_cluster);
        count++;
    }
    check("readdir returned entries", count > 0);

    /* 2. extract files and byte-diff vs their build outputs (read + cluster walk
     *    + consecutive-cluster batching across small and large multi-cluster files) */
    printf("--- extract & byte-diff vs build/ outputs ---\n");
    verify_extract(&fs, "CLOCK.BIN",   "build/clock.bin");    /* small, ~2 clusters */
    verify_extract(&fs, "SYSINFO.BIN", "build/sysinfo.bin");  /* 3 clusters         */
    verify_extract(&fs, "TEXT.BIN",    "build/notepad.bin");  /* ~13 clusters       */
    verify_extract(&fs, "PAINT.BIN",   "build/paint.bin");    /* ~62 clusters, batch*/

    /* 3. reaping: handles owned by a dying task are freed; others survive */
    printf("--- owner-based reaping (PORT-SPEC 6.7) ---\n");
    int a = unofs_open(&fs, "CLOCK.BIN",   3);
    int b = unofs_open(&fs, "SYSINFO.BIN", 3);
    int c = unofs_open(&fs, "BROWSER.BIN", 5);
    (void)a; (void)b;
    int reaped = unofs_reap(&fs, 3);
    check("reap(task 3) freed exactly its 2 handles", reaped == 2);
    check("other task's handle survives reap", fs.handles[c].status == 1);
    unofs_close(&fs, c);
    unofs_unmount(&fs);
    blkfile_close(&blk);

    /* 4. write path on a scratch copy: create -> write -> re-read -> delete */
    printf("--- write / re-read / delete (scratch copy) ---\n");
    const char *SCRATCH = "build/unofs_scratch.img";
    if (copy_file(IMG, SCRATCH)) { printf("scratch copy failed\n"); return 2; }
    const char *payload = "Hello, UnoDOS! unofs write path verified.\n";
    uint32_t plen = (uint32_t)strlen(payload);

    if (blkfile_open(&blk, SCRATCH, 1) || unofs_mount(&fs, &blk, 1)) { printf("scratch mount failed\n"); return 2; }
    int wh = unofs_create(&fs, "HELLO.TXT", 7);
    int wn = (wh >= 0) ? unofs_write(&fs, wh, payload, plen) : wh;
    check("create + write HELLO.TXT", wh >= 0 && wn == (int)plen);
    unofs_close(&fs, wh);
    unofs_unmount(&fs); blkfile_close(&blk);

    /* re-mount fresh and read it back */
    if (blkfile_open(&blk, SCRATCH, 0) || unofs_mount(&fs, &blk, 0)) { printf("remount failed\n"); return 2; }
    int rh = unofs_open(&fs, "HELLO.TXT", 7);
    char back[128] = {0};
    int rn = (rh >= 0) ? unofs_read(&fs, rh, back, sizeof back) : rh;
    check("re-read HELLO.TXT matches written bytes",
          rh >= 0 && rn == (int)plen && memcmp(back, payload, plen) == 0);
    unofs_close(&fs, rh);
    unofs_unmount(&fs); blkfile_close(&blk);

    /* delete it, re-mount, confirm gone */
    if (blkfile_open(&blk, SCRATCH, 1) || unofs_mount(&fs, &blk, 1)) { printf("remount(rw) failed\n"); return 2; }
    check("delete HELLO.TXT", unofs_delete(&fs, "HELLO.TXT") == 0);
    unofs_unmount(&fs); blkfile_close(&blk);
    if (blkfile_open(&blk, SCRATCH, 0) || unofs_mount(&fs, &blk, 0)) { printf("remount failed\n"); return 2; }
    check("HELLO.TXT gone after delete", unofs_open(&fs, "HELLO.TXT", 7) == -FS_ERR_NOT_FOUND);
    unofs_unmount(&fs); blkfile_close(&blk);
    remove(SCRATCH);

    printf("\n%s (%d failure%s)\n", fails ? "FAILURES PRESENT" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
