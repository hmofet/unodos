/* ===========================================================================
 * unomedia host test driver. Hosted build (plain gcc) of the same sources
 * the .UNO module compiles freestanding - the fast path when changing a
 * decoder, isolating codec bugs from OS plumbing (the dec_mp3 precedent).
 *
 *   imgtest info   <file>                     print "FMT WxH alpha=A frames=N"
 *   imgtest decode <file> <out.raw> [maxfr]   decode up to maxfr frames
 *                                             (default all, cap 64) appended
 *                                             into out.raw as w*h RGBA each;
 *                                             print "FMT WxH n=N delays=..."
 *
 * Exit 0 on success; on failure prints "ERR: <um_error>" and exits 1.
 * ======================================================================== */
#include "../unomedia.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_f;
static long src_read(void *ctx, long off, unsigned char *dst, long n)
{
    (void)ctx;
    if (fseek(g_f, off, SEEK_SET)) return -1;
    return (long)fread(dst, 1, (size_t)n, g_f);
}

static void *xalloc(unsigned long n) { return malloc(n); }

int main(int argc, char **argv)
{
    um_src src;
    um_image_info info;
    um_px *buf;
    long sz;
    int decode, maxfr = 64, n = 0, delay;

    if (argc < 3) { fprintf(stderr, "usage: imgtest info|decode <file> [out.raw [maxfr]]\n"); return 2; }
    decode = !strcmp(argv[1], "decode");
    if (decode && argc < 4) { fprintf(stderr, "decode needs <out.raw>\n"); return 2; }
    if (decode && argc > 4) maxfr = atoi(argv[4]);

    um_set_alloc(xalloc, free);

    g_f = fopen(argv[2], "rb");
    if (!g_f) { fprintf(stderr, "ERR: cannot open %s\n", argv[2]); return 1; }
    fseek(g_f, 0, SEEK_END); sz = ftell(g_f);

    src.read = src_read; src.size = sz; src.ctx = 0;
    if (!um_image_open(&src, argv[2], &info)) {
        printf("ERR: %s\n", um_error());
        return 1;
    }
    printf("%s %dx%d alpha=%d frames=%d", info.format, info.w, info.h,
           info.alpha, info.frames);

    if (decode) {
        FILE *out = fopen(argv[3], "wb");
        int delays[64], i, r = 0;
        if (!out) { fprintf(stderr, "ERR: cannot write %s\n", argv[3]); return 1; }
        buf = (um_px *)calloc((size_t)info.w * info.h, 4);
        if (!buf) { fprintf(stderr, "ERR: oom\n"); return 1; }
        while (n < maxfr && (r = um_image_frame(buf, &delay)) == 1) {
            fwrite(buf, 4, (size_t)info.w * info.h, out);
            if (n < 64) delays[n] = delay;
            n++;
        }
        if (r == -1) { printf("\nERR: %s\n", um_error()); free(buf); return 1; }
        fclose(out);
        printf(" n=%d delays=", n);
        for (i = 0; i < n && i < 64; i++) printf(i ? ",%d" : "%d", delays[i]);
        free(buf);
    }
    printf("\n");
    um_image_close();
    fclose(g_f);
    return 0;
}
