/* ===========================================================================
 * uno3d Dreamcast demo - the portable cube on the PowerVR2 (hardware 3D).
 *
 * The platform glue: select the PVR backend, then run the SAME demo_frame() that
 * the host and PS2 builds run. The PVR rasterises every triangle in hardware.
 * Start on the controller quits.
 * ======================================================================== */
#include "uno3d.h"
#include "uno3d_backend.h"
#include "uno3d_demo.h"

#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

#define W 640
#define H 480

int main(int argc, char **argv)
{
    float t = 0.0f;
    (void)argc; (void)argv;

    u3d_use_backend(&u3d_backend_dc);          /* hardware PowerVR2 rasteriser */
    demo_init(W, H);
    u3d_init(W, H);

    for (;;) {
        maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (cont) {
            cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
            if (st && (st->buttons & CONT_START)) break;
        }
        demo_frame(t);                          /* portable: clear + transform + draw */
        u3d_present();                          /* PVR scene finish (flip) */
        t += 1.0f / 60.0f;
    }

    u3d_shutdown();
    return 0;
}
