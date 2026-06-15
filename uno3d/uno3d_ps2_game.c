/* ===========================================================================
 * UnoDOS Runner on PS2 - the game on the Graphics Synthesizer (hardware 3D).
 * Glue only: pick the GS backend, map the DualShock 2 to game_input, run the
 * shared game loop. SAME uno3d_game.c as host/DC/PC.
 * ======================================================================== */
#include "uno3d.h"
#include "uno3d_backend.h"
#include "uno3d_game.h"

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <string.h>

#define W 640
#define H 448
static char g_padbuf[256] __attribute__((aligned(64)));

int main(int argc, char **argv)
{
    struct padButtonStatus btn;
    game_input in;
    (void)argc; (void)argv;

    u3d_use_backend(&u3d_backend_ps2);
    game_init(W, H);
    u3d_init(W, H);

    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, g_padbuf);

    for (;;) {
        memset(&in, 0, sizeof(in));
        if (padGetState(0, 0) == PAD_STATE_STABLE && padRead(0, 0, &btn) != 0) {
            u16 p = 0xFFFF ^ btn.btns;          /* libpad: 0 = pressed */
            in.left  = (p & PAD_LEFT)  || btn.ljoy_h < 0x50;
            in.right = (p & PAD_RIGHT) || btn.ljoy_h > 0xB0;
            in.up    = (p & PAD_UP);
            in.down  = (p & PAD_DOWN);
            in.fire  = (p & (PAD_CROSS | PAD_CIRCLE)) ? 1 : 0;
            in.start = (p & PAD_START) ? 1 : 0;
        }
        game_update(&in);
        game_render();
        u3d_present();
    }
    return 0;
}
