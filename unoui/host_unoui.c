/* ===========================================================================
 * unoui host harness - render the write-once demo window under every theme and
 * dump one PPM per theme. Same verify path as uno3d: define `fb` here, link the
 * shared fb.c primitives, render in software, write PPM -> ppm2png -> a tiled
 * contact sheet. The exact same unoui core + themes build for the PS2/DC ports.
 *
 *   ./host_unoui <out_dir>
 *       writes <out_dir>/<NN>_<name>.ppm for each theme.
 * ======================================================================== */
#include "unoui_theme.h"
#include "unoui_demo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `fb` itself is defined by the shared ../ps2/fb.c we link in. */

static const unoui_theme *THEMES[] = {
    &theme_unodos, &theme_macos7, &theme_macplus, &theme_win31,
    &theme_amiga,  &theme_c64,    &theme_apple2,  &theme_next
};
#define NTHEME ((int)(sizeof(THEMES)/sizeof(THEMES[0])))

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i, n = FB_W * FB_H;
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < n; i++) {
        unsigned px = fb[i];               /* 0xAABBGGRR, R in low byte */
        unsigned char rgb[3] = { px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* turn "Mac OS 7" -> "mac_os_7" for a filesystem-safe name */
static void slug(const char *s, char *out)
{
    int i = 0;
    for (; *s && i < 40; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[i++] = c;
        else if (i && out[i-1] != '_') out[i++] = '_';
    }
    while (i && out[i-1] == '_') i--;
    out[i] = 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "build";
    int i;
    char path[256], name[48];

    for (i = 0; i < NTHEME; i++) {
        unoui_window win;
        const unoui_theme *t = THEMES[i];
        static const char *dn[] = { "truecolor", "8-bit", "4-bit", "1-bit" };
        char cap[80];
        demo_build(&win, FB_W, FB_H);
        unoui_desktop(t, FB_W, FB_H);
        unoui_render(&win, t);
        /* a self-labelling caption strip so the contact sheet needs no legend */
        fb_fill_rect(0, 0, FB_W, 13, FB_RGB(0x10,0x10,0x10));
        fb_hline(0, 13, FB_W, FB_RGB(0x80,0x80,0x80));
        sprintf(cap, "%s  -  %s theme", t->name, dn[t->m.depth]);
        fb_text((FB_W - fb_text_w(cap)) / 2, 3, cap, FB_RGB(0xFF,0xFF,0xFF), -1);
        slug(t->name, name);
        sprintf(path, "%s/%02d_%s.ppm", dir, i, name);
        write_ppm(path);
        printf("theme %d/%d  %-16s depth=%d  -> %s\n",
               i + 1, NTHEME, t->name, t->m.depth, path);
    }
    printf("rendered %d themes from ONE write-once window.\n", NTHEME);
    return 0;
}
