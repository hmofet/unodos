/* ===========================================================================
 * uno3d PS2 demo - the portable cube on the Graphics Synthesizer (hardware 3D).
 *
 * The platform glue: pick the GS backend, then run the SAME demo_frame() that
 * the host software build and the Dreamcast build run. The GS rasterises every
 * triangle in hardware. The cube auto-spins (time from a frame counter) so a
 * PCSX2 capture shows it mid-rotation; Start quits on real hardware.
 * ======================================================================== */
#include "uno3d.h"
#include "uno3d_backend.h"
#include "uno3d_demo.h"

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>

#define W 640
#define H 448

static char g_padbuf[256] __attribute__((aligned(64)));

int main(int argc, char **argv)
{
    float t = 0.0f;
    struct padButtonStatus btn;
    (void)argc; (void)argv;

    u3d_use_backend(&u3d_backend_ps2);     /* hardware GS rasteriser */
    demo_init(W, H);
    u3d_init(W, H);

    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, g_padbuf);

    for (;;) {
        demo_frame(t);                     /* portable: clear + transform + draw */
        u3d_present();                     /* GS flip */
        t += 1.0f / 60.0f;

        if (padGetState(0, 0) == PAD_STATE_STABLE && padRead(0, 0, &btn) != 0) {
            u16 pressed = 0xFFFF ^ btn.btns;       /* libpad: 0 = pressed */
            if (pressed & PAD_START) break;
        }
    }

    padPortClose(0, 0);
    padEnd();
    return 0;
}
