/* UnoDOS/Dreamcast - the unoui shell (the modern desktop + Aurora), the DC
 * analogue of pc64/pc64_uui.c and ps2/ee_uui.c. Built by `./build.sh dc uui`
 * instead of the legacy Mac-style core (unodos.c + mac_compat). unoui owns the
 * desktop/windows/widgets and renders into the software fb[] (fb.c, ARGB8888);
 * this file inits the 640x480 RGB565 video mode, presents fb[] to the Dreamcast
 * framebuffer (vram_s) each vblank (Aurora renders full 32-bit internally ->
 * RGB565 out, same as the legacy DC present), and drives keyboard-style focus
 * nav from the maple controller d-pad. Self-contained (own main / present /
 * input); it never pulls in the Mac-compat event queue.
 *
 * The Aurora composite itself is portable software rendering (unoui.c +
 * theme_aurora.c + fb_aa.c), byte-identical to the pc64/ps2 output; only this
 * present + input layer is Dreamcast-specific.
 */
#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/video.h>
#include <string.h>

#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"

/* KOS init: default subsystems + the standard maple drivers (controller). No
   romdisk, no sound (the uui shell is a static desktop demo). */
KOS_INIT_FLAGS(INIT_DEFAULT);

static unoui_ui     UI;
static unoui_window g_about, g_disp;

static void feed(const unoui_event *ev) { unoui_handle(&UI, ev); }
static void feed_key(int k)
{ unoui_event e; memset(&e, 0, sizeof e); e.kind = UI_EV_KEY; e.key = k; feed(&e); }

static const char *g_themes[] = { "Aurora Light", "Aurora Dark" };

static void build_windows(void)
{
    /* an About window: exercises Aurora chrome (soft shadow, rounded frame,
       titlebar accent) + a couple of widgets */
    unoui_window_init(&g_about, "UnoDOS / Dreamcast", 60, 46, 300, 150);
    unoui_add_label (&g_about, 14, 12, "unoui shell on the SH4");
    unoui_add_label (&g_about, 14, 32, "Aurora theme, PVR-presented");
    unoui_add_label (&g_about, 14, 52, "controller d-pad nav");
    unoui_add_button(&g_about, 14, 100, 110, "OK", 0);
    unoui_ui_add(&UI, &g_about);

    /* a Display window: dropdown + checks + slider (more Aurora widgets) */
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

/* ARGB8888 (0xAABBGGRR) -> RGB565 */
static inline uint16 to565(fb_px p)
{
    unsigned r = (p) & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
    return (uint16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void present(void)
{
    uint16 *vram = (uint16 *)vram_s;
    int i, n = FB_W * FB_H;
    vid_waitvbl();                      /* present at vblank to limit tearing */
    for (i = 0; i < n; i++) vram[i] = to565(fb[i]);
}

/* maple controller: d-pad -> arrows, A/B -> Enter, Start -> Esc (edge-detected,
   the same key nav ee_uui reads off the DualShock 2). */
static void poll_controller(uint32 *prev)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st;
    uint32 now, edge;
    if (!dev) return;
    st = (cont_state_t *)maple_dev_status(dev);
    if (!st) return;
    now  = st->buttons;
    edge = now & ~(*prev);              /* newly pressed this frame */
    *prev = now;
    if (edge & CONT_DPAD_LEFT)  feed_key(UI_KEY_LEFT);
    if (edge & CONT_DPAD_RIGHT) feed_key(UI_KEY_RIGHT);
    if (edge & CONT_DPAD_UP)    feed_key(UI_KEY_UP);
    if (edge & CONT_DPAD_DOWN)  feed_key(UI_KEY_DOWN);
    if (edge & CONT_A)          feed_key(UI_KEY_ENTER);
    if (edge & CONT_B)          feed_key(UI_KEY_ENTER);
    if (edge & CONT_START)      feed_key(UI_KEY_ESC);
}

int main(int argc, char **argv)
{
    uint32 prev = 0;
    (void)argc; (void)argv;

    vid_set_mode(DM_640x480, PM_RGB565); /* KOS auto-selects VGA vs NTSC/PAL   */
    vid_empty();

    unoui_ui_init(&UI, &theme_aurora_light, FB_W, FB_H);
    build_windows();

    for (;;) {
        unoui_event tick;
        poll_controller(&prev);
        memset(&tick, 0, sizeof tick); tick.kind = UI_EV_TICK; feed(&tick);
        unoui_render_ui(&UI);
        present();
    }
    return 0;
}
