/* foreign_shim.c - the template every installed foreign app is a copy of.
 *
 * Contract: pc64/UNOPKG.md.  Built ONCE into PKG\FSHIM.UNO, which is not in
 * APPS\ and so is never an app itself; `uno_pkg_install` copies it, rewrites
 * two things inside the copy, and drops the result into APPS\ where the
 * ordinary app registry finds it.  The two rewritten things are:
 *
 *   1. THE DESCRIPTOR (`.unodesc`), which is why the desktop icon says
 *      "Firefox" and not "Foreign app".  The block below is padded with blank
 *      lines to reserve room; the installer writes a shorter block into the
 *      same bytes and shrinks the length field.  A shorter block is safe by
 *      construction: `uno_mod_desc_read` reads exactly `len` bytes.
 *
 *   2. THE TARGET BLOB, `uno_fshim_blob`, which is what tells one copy of this
 *      file that it is Firefox and another that it is GIMP.  The installer
 *      finds it by searching the image for the marker string.
 *
 * WHY A PATCHED COPY RATHER THAN ONE SHIM THAT READS A CONFIG FILE.  A module
 * has no idea what path it was loaded from - `uno_mod_load_uui` takes a bare
 * filename and the shell resolves it across volumes - so a shared shim could
 * not tell which of the installed apps it had been opened as.  Carrying the
 * answer inside the copy removes the question.  It also means an installed app
 * is exactly one file plus its icon, which is what makes uninstall a delete.
 *
 * WHAT THIS WINDOW IS FOR RIGHT NOW.  Until the unoguest channel lands (plan
 * phases P3/P4) there is no runtime to host the app, so the window's job is to
 * say so accurately: which runtime this app needs, whether this machine can
 * host one, and what is missing if it cannot.  That is not a placeholder - it
 * is the state a machine with virtualization disabled in firmware will show
 * forever, and it is the one thing the L1 version must not lose.
 */
#include "uno_uuiapp.h"
#include "uno_appdesc.h"
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_font.h"
#include "../pc64_pkg.h"
#include <string.h>

void pc64_shell_dirty(void);
const struct unoui_theme *pc64_shell_theme(void);
static const struct unoui_theme *TH(void) { return pc64_shell_theme(); }

/* ---- the patched blob ----------------------------------------------------
 * MARKER FIRST, so the installer can find it in a linked image without
 * knowing anything about section layout, and `volatile` so no optimiser
 * decides a constant array of ours is foldable.  Layout after the marker:
 *
 *     target \0 display-name \0
 *
 * `target` is opaque here on purpose: this file must not learn to parse
 * "android:org.mozilla.firefox/.App".  It hands the string to uno_pkg_launch
 * and shows what comes back.  That is what keeps a .deb from needing a
 * different shim. */
#define FSHIM_MARK "UNOPKG-TARGET-v1"
#define FSHIM_MARK_N 16

volatile char uno_fshim_blob[320] = FSHIM_MARK;

static const char *blob_target(void)
{
    const char *p = (const char *)uno_fshim_blob + FSHIM_MARK_N;
    return *p ? p : "";
}

static const char *blob_name(void)
{
    const char *p = (const char *)uno_fshim_blob + FSHIM_MARK_N;
    int cap = (int)sizeof uno_fshim_blob - FSHIM_MARK_N - 1, i = 0;
    if (!*p) return "Foreign app";                 /* unpatched template     */
    while (i < cap && p[i]) i++;                   /* step over the target   */
    if (i >= cap || !p[i + 1]) return "Foreign app";
    return p + i + 1;
}

/* ---- state ---------------------------------------------------------------
 * One line from the runtime, refreshed on a slow cadence.  `g_asked` exists
 * because `opened` fires once and `frame` fires sixty times a second: the
 * launch is a request, not a poll, and re-issuing it every frame would be a
 * request storm aimed at a subsystem whose whole job is to be slow. */
static char g_msg[120];
static char g_state[120];
static int  g_asked;
static int  g_tick;
static int  g_ok;

static void refresh(int ask)
{
    const char *t = blob_target();
    if (ask) g_ok = uno_pkg_launch(t, g_msg, (int)sizeof g_msg);
    uno_pkg_runtime_str(t, g_state, (int)sizeof g_state);
    pc64_shell_dirty();
}

/* ---- drawing ------------------------------------------------------------- */

static void fs_draw(unoui_widget *w, unoui_rect c, void *ctx)
{
    const unoui_palette *p = &TH()->pal;
    int fh = uno_font_height_px(0, 12);
    int y = c.y + 10;
    (void)w; (void)ctx;

    fb_fill_rect(c.x, c.y, c.w, c.h, p->win_bg);

    fb_text(c.x + 12, y, blob_name(), p->text, -1);
    y += fh + 10;

    /* The status line is the point of this window.  It is deliberately the
     * runtime's own words rather than a summary of them: "no appliance on
     * this machine" and "virtualization is disabled in firmware setup" are
     * different problems with different fixes, and a shim that flattened
     * both to "cannot start" would cost somebody an afternoon. */
    if (g_msg[0]) { fb_text(c.x + 12, y, g_msg, p->text, -1); y += fh + 4; }
    if (g_state[0]) fb_text(c.x + 12, y, g_state, p->text_dim, -1);

    y = c.y + c.h - fh - 10;
    fb_text(c.x + 12, y, blob_target(), p->text_dim, -1);
}

static int fs_event(unoui_widget *w, const unoui_event *e, void *ctx)
{
    (void)w; (void)ctx;
    if (e->kind == UI_EV_MOUSE_DOWN) { refresh(1); return 1; }
    return 0;
}

static unoui_canvas g_canvas = { fs_draw, fs_event, 0 };

static void fs_build(unoui_window *win)
{
    const unoui_metrics *m = &TH()->m;
    int aw = 460, ah = 200;
    unoui_window_init(win, blob_name(), 90, 70,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      ah + m->title_h + 2 * m->pad + m->frame_w);
    unoui_add_canvas(win, 0, 0, aw, ah, &g_canvas);
    win->flags |= UI_WIN_RESIZE;
}

static void fs_opened(void)
{
    g_asked = 0; g_tick = 0;
    refresh(0);
}

static void fs_frame(void)
{
    /* Ask once when the window opens, then re-read the runtime's state about
     * twice a second.  Reading state is cheap and never starts anything. */
    if (!g_asked) { g_asked = 1; refresh(1); return; }
    if (++g_tick >= 30) { g_tick = 0; if (!g_ok) refresh(0); }
}

static int fs_canvas_index(void) { return 0; }

/* The descriptor.  EVERY field here is overwritten by the installer; the
 * values below are what the template alone would say, and they are chosen so
 * that a template accidentally left in APPS\ is visibly wrong rather than
 * quietly plausible.  The blank lines are RESERVED SPACE - see the file
 * header - and mkuno.py skips them, so they cost nothing at build time. */
UNO_APP_DESC("id: fshim\n"
             "name: Foreign app template\n"
             "short: Template\n"
             "icon: generic\n"
             "cat: other\n"
             "rank: 250\n"
             "flags: hidden\n"
             "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
             "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
             "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
             "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
             "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
             "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
             "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
             "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");

static const UnoUuiApp kShim = {
    UNO_UUIAPP_ABI, "Foreign app",
    fs_build, 0, 0, fs_frame, fs_opened, 0, fs_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved) { (void)reserved; return &kShim; }
