/* ===========================================================================
 * UnoDOS Runner on Dreamcast - the game on the PowerVR2 (hardware 3D).
 * Glue only: pick the PVR backend, map the maple controller to game_input, run
 * the shared game loop. SAME uno3d_game.c as host/PS2/PC.
 * ======================================================================== */
#include "uno3d.h"
#include "uno3d_backend.h"
#include "uno3d_game.h"

#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

#define W 640
#define H 480

int main(int argc, char **argv)
{
    game_input in;
    (void)argc; (void)argv;

    u3d_use_backend(&u3d_backend_dc);
    game_init(W, H);
    u3d_init(W, H);

    for (;;) {
        maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        memset(&in, 0, sizeof(in));
        if (cont) {
            cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
            if (st) {
                in.left  = (st->buttons & CONT_DPAD_LEFT)  || st->joyx < -48;
                in.right = (st->buttons & CONT_DPAD_RIGHT) || st->joyx >  48;
                in.up    = (st->buttons & CONT_DPAD_UP)    ? 1 : 0;
                in.down  = (st->buttons & CONT_DPAD_DOWN)  ? 1 : 0;
                in.fire  = (st->buttons & (CONT_A | CONT_B)) ? 1 : 0;
                in.start = (st->buttons & CONT_START) ? 1 : 0;
            }
        }
        game_update(&in);
        game_render();
        u3d_present();
    }
    return 0;
}
