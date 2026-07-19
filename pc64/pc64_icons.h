/* Per-app icon artwork for the unoui shell (pc64_icons.c). Registered as
 * unoui_icon_art so desktop icons / launcher / taskbar draw distinct, high-
 * colour emblems instead of one generic glyph. */
#ifndef PC64_ICONS_H
#define PC64_ICONS_H
#include "unoui.h"

/* draw app `icon`'s emblem to fit `box` (no label) - used by the taskbar */
void pc64_icon_emblem(int icon, unoui_rect box);

/* the unoui_icon_art hook: emblem centred in r + the label below it */
void pc64_icon_art(int icon, unoui_rect r, const char *label, int flags);

/* The UnoDOS brand mark (the Start button logo): a ring broken open at the
 * top with the numeral 1 dropped through the gap. One colour, procedural,
 * reads from 12 px up. Draws inside the size x size box at (x,y). */
void pc64_start_logo(int x, int y, int size, fb_px fg);

/* the live shell theme, so icon art recolours to match it (defined in
 * pc64_uui.c; returns UI.theme). */
struct unoui_theme;
const struct unoui_theme *pc64_shell_theme(void);

#endif
