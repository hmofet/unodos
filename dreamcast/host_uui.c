/* UnoDOS/Dreamcast - host render of the unoui shell (dc_uui.c's Aurora scene).
 *
 * `./build.sh uui-host` compiles this with the host gcc, linking the SAME
 * portable software path the Dreamcast runs - unoui.c + theme_aurora.c +
 * fb_aa.c over this port's fb.c - and dumps shots/aurora.ppm -> aurora.png. The
 * pixels are byte-identical to what dc_uui.c presents on the Dreamcast; only the
 * present differs (console = fb -> RGB565 -> vram_s each vblank, here = fb ->
 * PPM). This is the render-verification of the Aurora *content* (theme, chrome,
 * widgets, the one-time-baked desktop bg); the DC-specific 565 present + maple
 * input mirror the already-verified legacy dc_main.c present. Same verify
 * discipline as the host/desktop targets above. */
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unoui_ui     UI;
static unoui_window g_about, g_disp;
static const char  *g_themes[] = { "Aurora Light", "Aurora Dark" };

/* the exact windows dc_uui.c builds (kept in sync by hand - two small demos) */
static void build_windows(void)
{
    unoui_window_init(&g_about, "UnoDOS / Dreamcast", 60, 46, 300, 150);
    unoui_add_label (&g_about, 14, 12, "unoui shell on the SH4");
    unoui_add_label (&g_about, 14, 32, "Aurora theme, PVR-presented");
    unoui_add_label (&g_about, 14, 52, "controller d-pad nav");
    unoui_add_button(&g_about, 14, 100, 110, "OK", 0);
    unoui_ui_add(&UI, &g_about);

    unoui_window_init(&g_disp, "Display", 300, 180, 280, 170);
    unoui_add_label   (&g_disp, 14, 12, "Theme:");
    unoui_add_dropdown(&g_disp, 74, 8, 180, g_themes, 2, 0);
    unoui_add_check   (&g_disp, 14, 42, "Aurora lite", 0);
    unoui_add_check   (&g_disp, 150, 42, "Dark mode", 0);
    unoui_add_label   (&g_disp, 14, 74, "Volume");
    unoui_add_slider  (&g_disp, 74, 70, 190, 0, 100, 65);
    unoui_add_button  (&g_disp, 14, 128, 110, "Apply", 0);
    unoui_ui_add(&UI, &g_disp);
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i, n = FB_W * FB_H;
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < n; i++) {
        unsigned px = fb[i];               /* 0xAABBGGRR, R low byte */
        unsigned char rgb[3] = { px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    unoui_event tick;
    const char *out = argc > 1 ? argv[1] : "shots/aurora.ppm";
    unoui_ui_init(&UI, &theme_aurora_light, FB_W, FB_H);
    build_windows();
    memset(&tick, 0, sizeof tick); tick.kind = UI_EV_TICK;
    unoui_handle(&UI, &tick);           /* one tick so caret/anim state settles */
    unoui_render_ui(&UI);
    write_ppm(out);
    printf("wrote %s (%dx%d)\n", out, FB_W, FB_H);
    return 0;
}
