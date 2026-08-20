/* ===========================================================================
 * ucc_host - compile a UnoC source to a .UNO on the development machine.
 *
 * The same compiler Studio runs on the device (apps/ucc.c + ucc_x64.c), driven
 * from a PC. ucc.h has named this file as the hosted driver since the compiler
 * landed; tools/ucc_test.c was the only thing exercising ucc_compile until now,
 * and it throws the container away after running it.
 *
 * What it is for: building a UnoC sample or app WITHOUT the device in the loop -
 * a CI check that the SDK samples still compile, or a .UNO to stage onto an
 * image. It is not a replacement for Studio.
 *
 * Build (Linux/WSL, alongside the compiler sources):
 *   gcc -O1 -Wall -o ucc_host tools/ucc_host.c apps/ucc.c apps/ucc_x64.c \
 *       -DUCC_KEXPORTS_H='"../build/apps/ucc_kexports.h"'
 *
 * Use:
 *   ./ucc_host sdk/TIMER.C build/esp/APPS/TIMER.UNO [--desc TIMER.DESC]
 *              [include-dir ...]
 *
 * With no include directory, `#include "UNO.H"` is searched for beside the
 * source and then in sdk/ - the same two places Studio looks.
 *
 * --desc embeds an app descriptor, which is what earns a module a Start-menu
 * row and a desktop icon (uno_appdesc.h, pc64/MODULES.md). ucc itself emits
 * none - it has no .unodesc section to point at - so the block is spliced in
 * between the image and the relocation table: `file_size` and `mem_size` grow
 * by its length, `desc_rva` addresses it, and the crc is recomputed. Every
 * loader invariant still holds (`48 + file_size + 4*nreloc == n`, and
 * `desc_rva < file_size`), no relocation points into the added bytes, and the
 * module never reads them. It is the same block tools/mkuno.py writes; that
 * script remains the validator of record - the checks here are the cheap ones.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../apps/ucc.h"

#define WORKLEN (8L << 20)          /* the compiler's scratch arena */
#define OUTMAX  (1L << 20)          /* .UNO ceiling; Studio's is 256 KB */
#define SRCMAX  (1L << 18)          /* ED_CAP: Studio's editor ceiling */

static const char *g_dirs[8];
static int g_ndirs;

/* ---- the .UNO container (tools/mkuno.py documents the layout) ------------- */
#define H_FLAGS      6          /* u16 */
#define H_MEM_SIZE  12          /* u32 */
#define H_FILE_SIZE 16          /* u32 */
#define H_NRELOC    20          /* u32 */
#define H_CRC       40          /* u32 */
#define H_DESC_RVA  44          /* u32 */
#define HDR_LEN     48
#define DESC_MAGIC  0x50504155u /* 'UAPP' */
#define DESC_MAX    1024

static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static void wr32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void wr16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

/* CRC-32/ISO-HDLC - the one zlib.crc32 computes, which is what the loader
 * checks and what mkuno.py writes. */
static unsigned crc32_of(const unsigned char *p, long n)
{
    unsigned c = 0xFFFFFFFFu;
    long i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (unsigned)(-(int)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

/* Splice an app descriptor into a freshly compiled container. Returns the new
 * total length, or -1 with a message on stderr. */
static long embed_desc(unsigned char *uno, long n, long cap, const char *path)
{
    unsigned char body[DESC_MAX], blk[DESC_MAX];
    unsigned file_size, mem_size, nreloc, blklen;
    long bn, relocs;
    FILE *f = fopen(path, "rb");

    if (!f) { fprintf(stderr, "ucc_host: cannot read %s\n", path); return -1; }
    bn = (long)fread(body, 1, sizeof body - 2, f);
    fclose(f);
    if (bn <= 0) { fprintf(stderr, "ucc_host: %s is empty\n", path); return -1; }
    if (body[bn - 1] != '\n') body[bn++] = '\n';
    body[bn++] = 0;                       /* the reader stops at the first NUL */

    blklen = (unsigned)(8 + bn);
    if (blklen > DESC_MAX) {
        fprintf(stderr, "ucc_host: descriptor is %u bytes, the cap is %d\n",
                blklen, DESC_MAX);
        return -1;
    }
    if (!strstr((char *)body, "id:")) {
        fprintf(stderr, "ucc_host: %s has no `id:` line\n", path);
        return -1;
    }
    wr32(blk, DESC_MAGIC); wr16(blk + 4, 1); wr16(blk + 6, blklen);
    memcpy(blk + 8, body, (size_t)bn);

    file_size = rd32(uno + H_FILE_SIZE);
    mem_size  = rd32(uno + H_MEM_SIZE);
    nreloc    = rd32(uno + H_NRELOC);
    relocs    = (long)nreloc * 4;
    if (n + (long)blklen > cap) {
        fprintf(stderr, "ucc_host: no room for the descriptor\n");
        return -1;
    }
    /* image | DESC | relocs - the relocation table slides right by blklen */
    memmove(uno + HDR_LEN + file_size + blklen,
            uno + HDR_LEN + file_size, (size_t)relocs);
    memcpy(uno + HDR_LEN + file_size, blk, blklen);
    wr32(uno + H_DESC_RVA,  file_size);           /* addresses the new block */
    wr32(uno + H_FILE_SIZE, file_size + blklen);
    wr32(uno + H_MEM_SIZE,  mem_size + blklen);
    n += blklen;
    wr32(uno + H_CRC, crc32_of(uno + HDR_LEN, n - HDR_LEN));
    return n;
}

static long slurp(const char *path, char *buf, long max)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    n = (long)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}

/* Resolve #include "NAME.H" against the search list, in order. */
static long inc_read(void *ctx, const char *name, char *buf, long max)
{
    char path[512];
    int i;
    long n;
    (void)ctx;
    for (i = 0; i < g_ndirs; i++) {
        snprintf(path, sizeof path, "%s/%s", g_dirs[i], name);
        if ((n = slurp(path, buf, max)) >= 0) return n;
    }
    return -1;
}

int main(int argc, char **argv)
{
    static char src[SRCMAX];
    static unsigned char out[OUTMAX];
    UccDiag diags[8];
    char dir[512], *slash;
    const char *in, *outpath, *base, *descpath = 0;
    void *work;
    long n, uno;
    int nd = 0, i;
    FILE *f;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.c> <out.uno> [--desc <file>] "
                        "[include-dir ...]\n", argv[0]);
        return 2;
    }
    in = argv[1];
    outpath = argv[2];

    /* search list: the explicit dirs, else the source's folder then sdk/ */
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--desc") && i + 1 < argc) { descpath = argv[++i]; }
        else if (g_ndirs < 8) g_dirs[g_ndirs++] = argv[i];
    }
    if (!g_ndirs) {
        snprintf(dir, sizeof dir, "%s", in);
        slash = strrchr(dir, '/');
        if (!slash) slash = strrchr(dir, '\\');
        if (slash) { *slash = 0; g_dirs[g_ndirs++] = dir; }
        else        g_dirs[g_ndirs++] = ".";
        g_dirs[g_ndirs++] = "sdk";
    }

    if ((n = slurp(in, src, SRCMAX - 1)) < 0) {
        fprintf(stderr, "ucc_host: cannot read %s\n", in);
        return 1;
    }
    /* diagnostics name the file, and the field is 8.3 - pass the basename */
    base = strrchr(in, '/');
    if (!base) base = strrchr(in, '\\');
    base = base ? base + 1 : in;

    if (!(work = malloc(WORKLEN))) {
        fprintf(stderr, "ucc_host: out of memory\n");
        return 1;
    }
    uno = ucc_compile(src, n, base, inc_read, 0, work, WORKLEN,
                      out, OUTMAX, diags, 8, &nd);
    if (uno < 0) {
        for (i = 0; i < nd; i++)
            fprintf(stderr, "%s:%d:%d: %s\n", diags[i].file, diags[i].line,
                    diags[i].col, diags[i].msg);
        if (!nd) fprintf(stderr, "ucc_host: compile failed with no diagnostic\n");
        free(work);
        return 1;
    }
    if (descpath && (uno = embed_desc(out, uno, OUTMAX, descpath)) < 0) {
        free(work);
        return 1;
    }
    if (!(f = fopen(outpath, "wb"))) {
        fprintf(stderr, "ucc_host: cannot write %s\n", outpath);
        free(work);
        return 1;
    }
    if (fwrite(out, 1, (size_t)uno, f) != (size_t)uno) {
        fprintf(stderr, "ucc_host: short write on %s\n", outpath);
        fclose(f);
        free(work);
        return 1;
    }
    fclose(f);
    printf("%s -> %s (%ld bytes) %s\n", in, outpath, uno, ucc_summary());
    free(work);
    return 0;
}
