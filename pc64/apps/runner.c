/* Runner3D app module (APP_RUNNER) - the uno3d "UnoDOS Runner" game on pc64.
 *
 * Runs the write-once uno3d game (uno3d/uno3d_game.c) through the portable
 * pipeline (uno3d/uno3d.c) and the chosen backend into the software
 * framebuffer, full-screen at the current desktop resolution. The backend is
 * u3d_backend_intel - the pc64 Intel-iGPU scaffold, which probes for an Intel
 * display device and (until the real command-streamer path exists) renders
 * via the software rasteriser. So this is the same 3D game the PS2 (GS) and
 * Dreamcast (PVR) ports run, now on bare-metal x86-64.
 *
 * It takes over the framebuffer while open (like a full-screen game); ESC
 * closes it and the desktop repaints. pc64-only (calls u3d_/game_ directly -
 * same statically-linked image).
 */
#include "uno_mod.h"
#include "uno3d.h"
#include "uno3d_game.h"
#include "uno3d_backend.h"
#include "fb.h"

static int gInited;
static int gSteerL, gSteerR;

static void runner_start(void)
{
    u3d_use_backend(&u3d_backend_intel);     /* probes Intel; soft-fallback */
    game_init(FB_W, FB_H);
    u3d_init(FB_W, FB_H);
    gInited = 1; gSteerL = gSteerR = 0;
}

static void render_frame(void)
{
    game_input in;
    char line[40], num[12];
    if (!gInited) runner_start();
    /* re-init if the desktop resolution changed under us */
    in.left = gSteerL > 0; in.right = gSteerR > 0;
    in.up = in.down = 0;
    in.fire = in.start = (gSteerL < 0);       /* (unused trigger) */
    if (gSteerL > 0) gSteerL--;
    if (gSteerR > 0) gSteerR--;
    game_update(&in);
    game_render();                            /* draws the 3D corridor into fb */

    /* HUD overlay straight into fb (bypasses the WM; we own the screen) */
    strcpy(line, "UnoDOS Runner 3D   score ");
    fmt_u(game_score(), num); strcat(line, num);
    fb_text(12, 10, line, FB_RGB(0xFF,0xFF,0xFF), -1);
    fb_text(12, 22, u3d_backend_name(), FB_RGB(0x66,0xFF,0xFF), -1);
    if (game_over())
        fb_text(FB_W/2 - 70, FB_H/2, "CRASH - Space to restart",
                FB_RGB(0xFF,0x66,0x66), -1);
    else
        fb_text(12, FB_H - 16, "Left/Right steer   Esc quit",
                FB_RGB(0xAA,0xAA,0xAA), -1);
}

static void runner_draw(UnoWin *w) { (void)w; render_frame(); }
static void runner_tick(void) { render_frame(); }

static Boolean runner_key(char ch, short code, Boolean cmd)
{
    game_input in = {0,0,0,0,0,0};
    (void)cmd;
    if (code == 0x7B || ch == 0x1C) { gSteerL = 6; return true; }   /* left */
    if (code == 0x7C || ch == 0x1D) { gSteerR = 6; return true; }   /* right */
    if (ch == ' ' || ch == '\r') { in.start = 1; game_update(&in); return true; }
    return false;
}

static void runner_opened(void) { runner_start(); }
static void runner_closed(void) { if (gInited) { u3d_shutdown(); gInited = 0; } }

static const AppInterface kIface = {
    runner_draw, runner_key, 0, runner_tick, runner_opened, runner_closed,
    "Runner3D", { 0, 0, 320, 200 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
