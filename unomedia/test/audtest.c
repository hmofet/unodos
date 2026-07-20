/* ===========================================================================
 * unomedia host audio test driver - the audio twin of imgtest.c. Hosted
 * build of the same sources the kernel compiles freestanding; the fast
 * path when changing a decoder (isolates codec bugs from OS plumbing).
 *
 *   audtest <file> <out.raw> [seek_ms]
 *
 * Decodes the whole stream (after an optional seek) to interleaved s16 at
 * the decoder's native rate/channels into out.raw and prints
 *   "FMT rate=R ch=C dur=D frames=N"
 * On failure prints "ERR: <um_error>" and exits 1.
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

#define CHUNK 4096

int main(int argc, char **argv)
{
    um_src src;
    um_audio_info info;
    short buf[CHUNK * 2];
    FILE *out;
    long sz, total = 0;
    int n;

    if (argc < 3) { fprintf(stderr, "usage: audtest <file> <out.raw> [seek_ms]\n"); return 2; }

    um_set_alloc(xalloc, free);
    g_f = fopen(argv[1], "rb");
    if (!g_f) { fprintf(stderr, "ERR: cannot open %s\n", argv[1]); return 1; }
    fseek(g_f, 0, SEEK_END); sz = ftell(g_f);

    src.read = src_read; src.size = sz; src.ctx = 0;
    if (!um_audio_open(&src, argv[1], &info)) {
        printf("ERR: %s\n", um_error());
        return 1;
    }
    if (argc > 3 && !um_audio_seek_ms(atol(argv[3]))) {
        printf("ERR: seek refused\n");
        return 1;
    }
    out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "ERR: cannot write %s\n", argv[2]); return 1; }
    while ((n = um_audio_decode(buf, CHUNK)) > 0) {
        fwrite(buf, 2 * info.channels, (size_t)n, out);
        total += n;
    }
    fclose(out);
    printf("%s rate=%d ch=%d dur=%ld frames=%ld\n",
           info.format, info.rate, info.channels, info.duration_ms, total);
    um_audio_close();
    fclose(g_f);
    return 0;
}
