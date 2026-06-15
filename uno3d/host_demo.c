/* ===========================================================================
 * uno3d host harness - run the portable demo on the SOFTWARE backend on a PC
 * and dump a frame to a PPM. This is the verifiable reference: the exact same
 * uno3d.c + uno3d_soft.c + uno3d_demo.c also build for the PS2/DC ports.
 *
 *   ./host_demo out.ppm [t_seconds]
 *
 * We define the framebuffer here (uno3d_soft.c links against `fb` from fb.h),
 * so the harness needs no port's fb.c - just its fb.h for dimensions/macros.
 * ======================================================================== */
#include "uno3d.h"
#include "uno3d_backend.h"
#include "uno3d_demo.h"
#include "fb.h"
#include <stdio.h>
#include <stdlib.h>

fb_px fb[FB_W * FB_H];          /* the framebuffer the software backend draws to */

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i, n = FB_W * FB_H;
    if (!f) { perror("fopen"); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < n; i++) {
        unsigned px = fb[i];                 /* 0xAABBGGRR, R in low byte */
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(px & 0xFF);
        rgb[1] = (unsigned char)((px >> 8) & 0xFF);
        rgb[2] = (unsigned char)((px >> 16) & 0xFF);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "uno3d.ppm";
    float t = (argc > 2) ? (float)atof(argv[2]) : 0.6f;

    u3d_use_backend(&u3d_backend_soft);
    demo_init(FB_W, FB_H);
    u3d_init(FB_W, FB_H);

    demo_frame(t);
    u3d_present();                 /* no-op for software; glue presents fb */
    write_ppm(out);

    printf("uno3d host: backend=%s t=%.2f tris=%d -> %s (%dx%d)\n",
           u3d_backend_name(), t, u3d_last_tris(), out, FB_W, FB_H);
    u3d_shutdown();
    return 0;
}
