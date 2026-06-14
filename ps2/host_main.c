/* ===========================================================================
 * UnoDOS/PS2 host shim - builds the software-framebuffer code with a normal
 * host compiler (WSL gcc) and dumps the rendered frame to a PPM, so the FB +
 * font + splash pipeline is testable on the PC WITHOUT a PS2 toolchain or
 * PCSX2 (HANDOFF SS3 "build.sh host - the family's fastest inner loop"). The
 * real EE target (main.c) shares fb.c + uno_splash.c verbatim; only the
 * present-the-frame + input layers differ.
 *
 *   host_splash [out.ppm] [cursor_x] [cursor_y]
 * ===========================================================================
 */
#include "fb.h"
#include <stdio.h>
#include <stdlib.h>

void uno_render_splash(int cx, int cy);

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "shots/m0_splash.ppm";
    int cx = (argc > 2) ? atoi(argv[2]) : FB_W / 2;
    int cy = (argc > 3) ? atoi(argv[3]) : FB_H / 2;
    FILE *f;
    int i;

    uno_render_splash(cx, cy);

    f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < FB_W * FB_H; i++) {
        fb_px p = fb[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(p & 0xFF);          /* R (low byte) */
        rgb[1] = (unsigned char)((p >> 8) & 0xFF);   /* G */
        rgb[2] = (unsigned char)((p >> 16) & 0xFF);  /* B */
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s (%dx%d)\n", out, FB_W, FB_H);
    return 0;
}
