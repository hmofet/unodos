/* ===========================================================================
 * unoui_wmanim - animated window geometry (the snap animation).
 *
 * The first consumer of unoui_anim. A snap, an unsnap or a maximize walks the
 * window over to its target in about an eighth of a second instead of
 * teleporting, which is the difference between a window that appears somewhere
 * else and one you can see go there.
 *
 * WHY ITS OWN FILE. It is the join between two modules that are deliberately
 * kept apart: unoui_anim.c has no fb.h and no dependencies at all, and unoui.c
 * has no dependency on the animation module (so ps2 and dreamcast, which
 * compile unoui and not unoui_anim, need no build-list edit and keep the
 * instant snap they have today). This file depends on both, and a port opts in
 * by compiling it and calling unoui_wmanim_install().
 *
 * WHAT IS ANIMATED, AND WHAT IS NOT. The tweens write straight into win->r, so
 * the window's geometry really is mid-flight: hit-testing, drawing and the
 * work-area clamp all agree with what is on screen, because they are all
 * reading the same rect. What is NOT mid-flight is the window's MODEL - its
 * snap state and restore_r are final from the first frame, because those are
 * what a saved session is made of. unoui_geom_target() exists so a platform
 * that persists geometry can ask for the settled rect instead of catching a
 * window halfway.
 * ===========================================================================
 */
#include "unoui_theme.h"
#include "unoui_anim.h"

/* Windows that can be moving at once. Tile/cascade snaps a whole set in one
 * go, so this is not 1. Past it the animator declines and the core snaps that
 * window instantly, which is the right way to run out: the window still ends
 * up where it belongs. */
#define GEOM_MAX 8

static struct {
    unoui_window *win;
    unoui_anim_h  h[4];        /* x, y, w, h - they start and finish together */
    unoui_rect    target;
} g_geom[GEOM_MAX];

static void slot_clear(int i)
{
    int k;
    g_geom[i].win = 0;
    for (k = 0; k < 4; k++) g_geom[i].h[k] = 0;
}

static void slot_release(unoui_anim *ac, int i)
{
    int k;
    for (k = 0; k < 4; k++) unoui_anim_free(ac, g_geom[i].h[k]);
    slot_clear(i);
}

/* unoui_geom_fn: start (or re-aim) `win`'s move to `target`. */
int unoui_wmanim_geom(unoui_ui *ui, unoui_window *win, unoui_rect target, int ms)
{
    unoui_anim *ac = (unoui_anim *)ui->anim;
    unoui_tween tw;
    int i, slot = -1, k;
    int *field[4];
    int from[4], to[4];

    if (!ac || !win || ms <= 0) return 0;

    /* Re-aiming a window that is already moving cancels its old tweens rather
     * than adding a second set. Two sets writing the same four ints would each
     * win on alternate frames, which reads as a window that shakes. */
    for (i = 0; i < GEOM_MAX; i++) {
        if (g_geom[i].win == win) { slot_release(ac, i); slot = i; break; }
        if (!g_geom[i].win && slot < 0) slot = i;
    }
    if (slot < 0) return 0;                    /* full: let the core snap it */

    /* Nothing to do is not a failure, but it must not leave a live slot. */
    if (target.x == win->r.x && target.y == win->r.y &&
        target.w == win->r.w && target.h == win->r.h) return 1;

    field[0] = &win->r.x; field[1] = &win->r.y;
    field[2] = &win->r.w; field[3] = &win->r.h;
    from[0] = win->r.x; from[1] = win->r.y; from[2] = win->r.w; from[3] = win->r.h;
    to[0] = target.x;   to[1] = target.y;   to[2] = target.w;   to[3] = target.h;

    g_geom[slot].win = win;
    g_geom[slot].target = target;
    for (k = 0; k < 4; k++) {
        tw.from = from[k]; tw.to = to[k];
        tw.dur_ms = ms; tw.delay_ms = 0;
        /* out-cubic: leaves fast and arrives slowly, which is what makes a
         * short move read as deliberate rather than as a glitch. */
        tw.ease = UI_EASE_OUT_CUBIC;
        tw.loop = UI_ANIM_ONCE;
        tw.out = field[k];
        g_geom[slot].h[k] = unoui_tween_start(ac, &tw);
        if (!g_geom[slot].h[k]) {              /* pool exhausted mid-set */
            slot_release(ac, slot);
            return 0;
        }
    }
    return 1;
}

/* unoui_geom_tick_fn: called once per frame from unoui_render_ui, before
 * anything is drawn. The tweens have already written win->r (the platform
 * ticked the animation context); what is left is to keep the window's fill
 * widgets in step with a size that is still changing, and to retire the slot. */
void unoui_wmanim_tick(unoui_ui *ui)
{
    unoui_anim *ac = (unoui_anim *)ui->anim;
    int i;
    if (!ac) return;
    for (i = 0; i < GEOM_MAX; i++) {
        unoui_window *w = g_geom[i].win;
        if (!w) continue;
        unoui_reflow_window(ui->theme, w);
        if (unoui_anim_done(ac, g_geom[i].h[0])) {
            /* Land on the target EXACTLY. The tweens do too, but a slot can
             * also retire because its handles were recycled out from under it,
             * and a window left a pixel short of a snap edge is a visible gap
             * against the screen edge it was snapped to. */
            w->r = g_geom[i].target;
            unoui_reflow_window(ui->theme, w);
            slot_release(ac, i);
        }
    }
}

int unoui_geom_target(const unoui_ui *ui, const unoui_window *win, unoui_rect *out)
{
    int i;
    (void)ui;
    for (i = 0; i < GEOM_MAX; i++)
        if (g_geom[i].win == win) { if (out) *out = g_geom[i].target; return 1; }
    return 0;
}

void unoui_geom_settle(unoui_ui *ui, unoui_window *win)
{
    unoui_anim *ac = (unoui_anim *)ui->anim;
    int i;
    for (i = 0; i < GEOM_MAX; i++)
        if (g_geom[i].win == win) {
            win->r = g_geom[i].target;
            if (ac) slot_release(ac, i);
            else    slot_clear(i);
            unoui_reflow_window(ui->theme, win);
            return;
        }
}

void unoui_wmanim_install(unoui_ui *ui, unoui_anim *ac)
{
    int i;
    for (i = 0; i < GEOM_MAX; i++) slot_clear(i);
    ui->anim = ac;
    unoui_geom_anim = unoui_wmanim_geom;
    unoui_geom_tick = unoui_wmanim_tick;
}
