/* ===========================================================================
 * unoui_wmanim - animated window geometry. See unoui_wmanim.c.
 *
 * Opt-in: a port compiles this file (it needs unoui_anim.c) and calls
 * unoui_wmanim_install() once. A port that does not is unaffected - snaps stay
 * instant and unoui.c has no link dependency on any of this.
 * ===========================================================================
 */
#ifndef UNOUI_WMANIM_H
#define UNOUI_WMANIM_H

#include "unoui.h"
#include "unoui_anim.h"

/* Install the animator on `ui` and point it at the context the platform pumps.
 * After this, unoui_snap_apply() animates instead of teleporting. Duration is
 * unoui_snap_ms (unoui.h); set it to 0 to turn the animation off again without
 * uninstalling anything. */
void unoui_wmanim_install(unoui_ui *, unoui_anim *);

/* The two hooks, exposed for a platform that wants to install them by hand. */
int  unoui_wmanim_geom(unoui_ui *, unoui_window *, unoui_rect target, int ms);
void unoui_wmanim_tick(unoui_ui *);

/* Is `win` mid-flight, and where is it going? Returns 1 and fills `out` (which
 * may be NULL) if so. What a platform that PERSISTS window geometry should ask
 * before saving: a session written during the ~130 ms of a snap would otherwise
 * record the rect the window happened to be passing through. */
int  unoui_geom_target(const unoui_ui *, const unoui_window *, unoui_rect *out);

/* ---- "no" -----------------------------------------------------------------
 * THE REJECT GESTURE, and the reason it is in the toolkit rather than in each
 * dialog: a wrong password had no standard answer here. Every sheet was free to
 * invent one - a red label, a status line, nothing at all - so the same event
 * looked different in every window, and the one that did nothing was
 * indistinguishable from a machine that had not noticed the Enter key.
 *
 * A shake is the answer because it says "rejected" without saying WHICH part
 * was wrong, which is exactly the amount a login prompt is allowed to tell you.
 *
 * unoui_reject_widget shakes one control (the field, for an error the user can
 * fix in place); unoui_reject_window shakes the whole sheet (for "those
 * credentials are wrong", where nothing in particular is at fault). On an
 * editable widget the reject also SELECTS the text, so typing replaces it -
 * the retry costs one keystroke, not a Backspace held down.
 *
 * Both need the animator installed (unoui_wmanim_install). Without it, or with
 * a full pool, they return 0 and do nothing, and the caller carries on: a
 * missing animation must never swallow the thing it was decorating.
 *
 * An app that wants something else entirely draws it in a UI_CANVAS - that is
 * what the canvas is for. This is the house gesture, not the only one. */
int  unoui_reject_widget(unoui_ui *, unoui_window *, unoui_widget *);
int  unoui_reject_window(unoui_ui *, unoui_window *);

/* Put `win` at its target NOW and drop the animation - for the cases where the
 * move must not be seen to happen (a virtual-desktop switch, a restore from a
 * saved session). A no-op on a window that is not moving. */
void unoui_geom_settle(unoui_ui *, unoui_window *);

#endif /* UNOUI_WMANIM_H */
