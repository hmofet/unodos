/* Per-app icon artwork for the unoui shell (pc64_icons.c). Registered as
 * unoui_icon_art so desktop icons / launcher / taskbar draw distinct, high-
 * colour emblems instead of one generic glyph. */
#ifndef PC64_ICONS_H
#define PC64_ICONS_H
#include "unoui.h"

/* ---- stable emblem ids ----------------------------------------------------
 * An icon is a property OF AN APP, not of its position in the launcher. These
 * ids are this module's API and never renumber: an app names the emblem it
 * wants and the shell can gain, lose or reorder apps freely.
 *
 * They used to be implicit - the icon id was simply the app index - which
 * meant deleting one app silently gave every later app its neighbour's icon.
 * Apps loaded from storage at runtime could never have worked that way at all,
 * since nothing static can predict their index. PCI_GENERIC is the fallback
 * for exactly those: an app that does not name a known emblem still gets a
 * sensible one. */
typedef enum {
    PCI_GENERIC = 0,
    PCI_CTRL, PCI_EDIT, PCI_FILES, PCI_SYS, PCI_CLOCK, PCI_SETUP, PCI_MUSIC,
    PCI_DOSTRIS, PCI_PACMAN, PCI_OUTLAST, PCI_TRACKER, PCI_PAINT,
    PCI_NETWORK, PCI_RUNNER, PCI_BROWSER, PCI_STUDIO, PCI_PHOTOS
} pc64_icon_id;

/* draw emblem `icon` (a PCI_*) to fit `box` (no label) - used by the taskbar */
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
