/* ===========================================================================
 * uno3d host harness for UnoDOS Runner - software backend on the PC.
 *
 * Runs the game on autopilot for N frames (steering toward each wall's gap) and
 * dumps the final frame to a PPM, so the exact game + software rasteriser that
 * the consoles and the 386 DOS build run is verifiable here.
 *
 *   ./host_game out.ppm [frames]
 * ======================================================================== */
#include "uno3d.h"
#include "uno3d_backend.h"
#include "uno3d_game.h"
#include "fb.h"
#include <stdio.h>
#include <stdlib.h>

fb_px fb[FB_W * FB_H];

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i, n = FB_W * FB_H;
    if (!f) { perror("fopen"); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < n; i++) {
        unsigned px = fb[i];
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
    const char *out = (argc > 1) ? argv[1] : "game.ppm";
    int frames = (argc > 2) ? atoi(argv[2]) : 40;
    int f;

    u3d_use_backend(&u3d_backend_soft);
    game_init(FB_W, FB_H);
    u3d_init(FB_W, FB_H);

    for (f = 0; f < frames; f++) {
        game_input in;
        float target = game_ai_target(), px = game_player_x();
        in.left  = (px > target + 0.2f);
        in.right = (px < target - 0.2f);
        in.up = in.down = in.fire = in.start = 0;
        game_update(&in);
        game_render();
    }

    write_ppm(out);
    printf("UnoDOS Runner (host/%s): frames=%d score=%d over=%d -> %s\n",
           u3d_backend_name(), frames, game_score(), game_over(), out);
    u3d_shutdown();
    return 0;
}
