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

/* Put `win` at its target NOW and drop the animation - for the cases where the
 * move must not be seen to happen (a virtual-desktop switch, a restore from a
 * saved session). A no-op on a window that is not moving. */
void unoui_geom_settle(unoui_ui *, unoui_window *);

#endif /* UNOUI_WMANIM_H */
