/* ===========================================================================
 * unomedia VP8 core test driver (hosted). Parses just enough RIFF to pull
 * the VP8 chunk out of a .webp file (test-side only - the real container
 * decode lives in um_webp.c), then exercises the two-function core:
 *
 *   vp8test dims   <file.webp>            print "WxH"
 *   vp8test decode <file.webp> <out.raw>  decode; write w*h RGBA rows
 *
 * Exit 0 on success; on failure prints "ERR: <um_error>" and exits 1.
 * ======================================================================== */
#include "../unomedia.h"
#include "../unomedia_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xalloc(unsigned long n) { return malloc(n); }

static unsigned long rd32le(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* load the file, return a pointer to the VP8 chunk payload (malloc'd file
 * kept alive), or NULL */
static unsigned char *find_vp8(const char *path, long *chunk_len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len, off;
    if (!f) { fprintf(stderr, "ERR: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 20) { fclose(f); fprintf(stderr, "ERR: short file\n"); return NULL; }
    buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f); free(buf);
        fprintf(stderr, "ERR: read failed\n"); return NULL;
    }
    fclose(f);
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WEBP", 4)) {
        free(buf); fprintf(stderr, "ERR: not a WEBP RIFF\n"); return NULL;
    }
    off = 12;
    while (off + 8 <= len) {
        long sz = (long)rd32le(buf + off + 4);
        if (sz < 0 || off + 8 + sz > len) break;
        if (!memcmp(buf + off, "VP8 ", 4)) {
            *chunk_len = sz;
            memmove(buf, buf + off + 8, (size_t)sz);   /* keep buf as owner */
            return buf;
        }
        off += 8 + sz + (sz & 1);
    }
    free(buf);
    fprintf(stderr, "ERR: no VP8 chunk\n");
    return NULL;
}

int main(int argc, char **argv)
{
    unsigned char *chunk;
    long n;
    int w, h;

    um_set_alloc(xalloc, free);
    if (argc < 3) {
        fprintf(stderr, "usage: vp8test dims|decode <file.webp> [out.raw]\n");
        return 2;
    }
    chunk = find_vp8(argv[2], &n);
    if (!chunk) return 1;

    if (!um_vp8_dims(chunk, n, &w, &h)) {
        printf("ERR: %s\n", um_error());
        free(chunk); return 1;
    }
    if (!strcmp(argv[1], "dims")) {
        printf("%dx%d\n", w, h);
        free(chunk); return 0;
    }

    if (!strcmp(argv[1], "decode")) {
        um_px *px = malloc((size_t)w * h * sizeof(um_px));
        FILE *o;
        if (argc < 4 || !px) { free(chunk); free(px); return 2; }
        if (!um_vp8_decode(chunk, n, px)) {
            printf("ERR: %s\n", um_error());
            free(chunk); free(px); return 1;
        }
        o = fopen(argv[3], "wb");
        if (!o) { free(chunk); free(px); return 2; }
        fwrite(px, sizeof(um_px), (size_t)w * h, o);
        fclose(o);
        printf("%dx%d\n", w, h);
        free(chunk); free(px);
        return 0;
    }
    free(chunk);
    return 2;
}
