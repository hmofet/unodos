/* Host-side test for UnoAmp's .wsz loader.
 *
 * The skin engine is pure data handling - a ZIP walk, an inflate and a BMP
 * reader - so it does not need a kernel, a framebuffer or a disk to exercise.
 * Running it natively turns "the skin did not load on the box" into a specific
 * failing sheet in under a second, instead of a ten-minute build-push-reboot
 * cycle per hypothesis.
 *
 * Build (from pc64/):
 *   cc -I. -I../unomedia -o /tmp/skintest tools/skintest.c unoamp_skin.c \
 *      ../unomedia/um_inflate.c ../unomedia/unomedia.c
 *   /tmp/skintest build/skins/EMBER.wsz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unoamp_skin.h"

static const char *g_file;

/* The two fs entry points unoamp_skin.c uses, backed by a real file. */
long uno_fs_read(int vol, const char *name, unsigned char *buf, long max)
{
    FILE *f;
    long n;
    (void)vol; (void)name;
    f = fopen(g_file, "rb");
    if (!f) return -1;
    n = (long)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}
long uno_fs_read_at(int vol, const char *name, long off,
                    unsigned char *buf, long max)
{
    FILE *f;
    long n;
    (void)vol; (void)name;
    f = fopen(g_file, "rb");
    if (!f) return -1;
    if (fseek(f, off, SEEK_SET)) { fclose(f); return -1; }
    n = (long)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}
long uno_fs_size(int vol, const char *name)
{
    FILE *f;
    long n;
    (void)vol; (void)name;
    f = fopen(g_file, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    n = ftell(f);
    fclose(f);
    return n;
}

static const char *kName[UNOAMP_SHEET_N] = {
    "MAIN", "CBUTTONS", "TITLEBAR", "SHUFREP", "POSBAR", "VOLUME", "BALANCE",
    "MONOSTER", "PLAYPAUS", "NUMBERS", "TEXT", "EQMAIN", "PLEDIT"
};

int main(int argc, char **argv)
{
    const unoamp_skin *sk;
    int i, ok = 0;
    if (argc < 2) { fprintf(stderr, "usage: skintest <file.wsz>\n"); return 2; }
    g_file = argv[1];

    printf("loading %s\n", g_file);
    if (!unoamp_skin_load(0, "SKIN.WSZ")) {
        printf("FAILED: unoamp_skin_load returned 0 "
               "(MAIN.BMP is the load gate - it did not decode)\n");
    }
    sk = unoamp_skin_get();
    if (!sk) { printf("no skin loaded\n"); }
    else {
        for (i = 0; i < UNOAMP_SHEET_N; i++) {
            const unoamp_sheet *s = &sk->sheet[i];
            if (s->px) {
                ok++;
                printf("  %-9s %4d x %-4d  first px %08X\n",
                       kName[i], s->w, s->h, s->px[0]);
            } else {
                printf("  %-9s MISSING\n", kName[i]);
            }
        }
        printf("  viscolor %s", sk->have_viscolor ? "yes" : "NO");
        if (sk->have_viscolor)
            printf("  bg=%08X peak=%08X bar0=%08X", sk->viscolor[0],
                   sk->viscolor[1], sk->viscolor[2]);
        printf("\n  pledit   normal=%08X current=%08X bg=%08X selbg=%08X\n",
               sk->pl_normal, sk->pl_current, sk->pl_bg, sk->pl_selbg);
        printf("%d/%d sheets\n", ok, UNOAMP_SHEET_N);
    }
    return ok ? 0 : 1;
}
