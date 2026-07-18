/* UnoDOS/PS2 - the unoui shell (the modern desktop + Aurora), the EE analogue
 * of pc64/pc64_uui.c. Built by `./build.sh ee uui` instead of the legacy
 * Mac-style core (unodos.c). unoui owns the desktop/windows/widgets/render into
 * fb[]; the EE platform (ee_platform.c) inits the GS + pad and presents fb each
 * frame; the DualShock 2 drives keyboard-style focus nav.
 *
 * NOTE: build-verified (ee-gcc links it) but not yet run - no PCSX2/BIOS on this
 * dev box. Render-verification is deferred to a machine with PCSX2. */
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include <tamtypes.h>
#include <libpad.h>
#include <string.h>

/* EE platform hooks (ee_platform.c) */
void uno_ee_init(void);
void uno_ee_present(void);

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
    unoui_window_init(&g_about, "UnoDOS / PS2", 60, 46, 300, 150);
    unoui_add_label (&g_about, 14, 12, "unoui shell on the EE");
    unoui_add_label (&g_about, 14, 32, "Aurora theme, GS-presented");
    unoui_add_label (&g_about, 14, 52, "DualShock 2 nav");
    unoui_add_button(&g_about, 14, 100, 110, "OK", 0);
    unoui_ui_add(&UI, &g_about);

    /* a Display window: dropdown + checks + slider (more Aurora widgets) */
    unoui_window_init(&g_disp, "Display", 220, 150, 280, 170);
    unoui_add_label   (&g_disp, 14, 12, "Theme:");
    unoui_add_dropdown(&g_disp, 74, 8, 180, g_themes, 2, 0);
    unoui_add_check   (&g_disp, 14, 42, "Aurora lite", 0);
    unoui_add_check   (&g_disp, 150, 42, "Dark mode", 0);
    unoui_add_label   (&g_disp, 14, 74, "Volume");
    unoui_add_slider  (&g_disp, 74, 70, 190, 0, 100, 65);
    unoui_add_button  (&g_disp, 14, 128, 110, "Apply", 0);
    unoui_ui_add(&UI, &g_disp);
}

int main(void)
{
    u16 prev = 0xFFFF;

    uno_ee_init();                                  /* GS + pad + fb texture */
    unoui_ui_init(&UI, &theme_aurora_light, FB_W, FB_H);
    build_windows();

    for (;;) {
        struct padButtonStatus btn;
        unoui_event tick;

        if (padRead(0, 0, &btn) != 0) {
            u16 now  = btn.btns;                    /* libpad: bit 0 => pressed */
            u16 edge = (u16)(prev & ~now);          /* newly pressed this frame */
            prev = now;
            if (edge & PAD_LEFT)   feed_key(UI_KEY_LEFT);
            if (edge & PAD_RIGHT)  feed_key(UI_KEY_RIGHT);
            if (edge & PAD_UP)     feed_key(UI_KEY_UP);
            if (edge & PAD_DOWN)   feed_key(UI_KEY_DOWN);
            if (edge & PAD_CROSS)  feed_key(UI_KEY_ENTER);
            if (edge & PAD_CIRCLE) feed_key(UI_KEY_ENTER);
            if (edge & PAD_START)  feed_key(UI_KEY_ESC);
        }

        memset(&tick, 0, sizeof tick); tick.kind = UI_EV_TICK; feed(&tick);

        unoui_render_ui(&UI);
        uno_ee_present();
    }
    return 0;
}
