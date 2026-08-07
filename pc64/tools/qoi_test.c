/* host harness for pc64_qoi.c: decode argv[1], write raw RGBA to argv[2]. */
#include "../pc64_qoi.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    static unsigned char in[1 << 20], out[32 * 32 * 4];
    long n;
    int w = 0, h = 0;
    FILE *f;
    if (argc != 3) return 2;
    f = fopen(argv[1], "rb");
    if (!f) return 2;
    n = (long)fread(in, 1, sizeof in, f);
    fclose(f);
    if (uno_qoi_decode(in, n, out, 32, 32, &w, &h) != 0) {
        printf("decode failed\n");
        return 1;
    }
    printf("%d %d\n", w, h);
    f = fopen(argv[2], "wb");
    if (!f) return 2;
    fwrite(out, 1, (size_t)(w * h * 4), f);
    fclose(f);
    return 0;
}
