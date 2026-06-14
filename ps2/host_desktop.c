/* ===========================================================================
 * UnoDOS/PS2 host shim for the FULL desktop (M1+).
 *
 * Compiles the portable core ps2/unodos.c + the Mac-compat shim (mac_compat.*,
 * mac_io.*) + fb.* with a normal host compiler (WSL gcc), so the whole UnoDOS
 * desktop / window manager / apps render on the PC without a PS2 toolchain or
 * PCSX2 - the family's fastest inner loop (HANDOFF SS3). ps2/unodos.c owns
 * main(); under -DUNO_HOST it drives the AUTOTEST app it was built for, then
 * calls uno_host_present() here to dump the framebuffer to a PPM.
 *
 * The output path is taken from $UNO_OUT (default shots/desktop.ppm).
 * ===========================================================================
 */
#include "fb.h"
#include "mac_compat.h"
#include <stdio.h>
#include <stdlib.h>

void uno_host_present(void)
{
    const char *out = getenv("UNO_OUT");
    FILE *f;
    int i;
    if (!out) out = "shots/desktop.ppm";
    f = fopen(out, "wb");
    if (!f) { perror(out); return; }
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
}
