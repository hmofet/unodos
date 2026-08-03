/* ===========================================================================
 * uoicons.h - the Office 97 toolbar icon atlas (phase 6b).
 *
 * Fills the `uoc_set_icons` seam uochrome left open in 6a.  Our own artwork,
 * drawn from shape primitives in uoicons.c; see that file's header for why it
 * is code rather than a data blob.
 * ======================================================================== */
#ifndef UOICONS_H
#define UOICONS_H

#include "uochrome.h"

/* The cell index of every icon.  Toolbars and menus name these, so adding one
 * means appending here - never renumbering, which would silently re-point
 * every table in the suite. */
enum {
    UOI_NEW = 0, UOI_OPEN, UOI_SAVE, UOI_PRINT, UOI_PREVIEW, UOI_SPELL,
    UOI_CUT, UOI_COPY, UOI_PASTE, UOI_PAINTER, UOI_UNDO, UOI_REDO,
    UOI_LINK, UOI_WEB, UOI_TABLE, UOI_COLUMNS, UOI_DRAW, UOI_ZOOM,
    UOI_HELP, UOI_BOLD, UOI_ITALIC, UOI_UNDERLINE,
    UOI_ALIGN_L, UOI_ALIGN_C, UOI_ALIGN_R, UOI_JUSTIFY,
    UOI_NUMBERING, UOI_BULLETS, UOI_INDENT_DEC, UOI_INDENT_INC,
    UOI_BORDERS, UOI_FILLCOLOR, UOI_FONTCOLOR, UOI_HIGHLIGHT,
    UOI_ASSISTANT,
    UOI_COUNT
};

/* The atlas itself, built on first call. */
const fb_px *uoc_icons_97(int *cell, int *cols, int *count);

/* Build it and hand it to uochrome.  Every app calls this once at startup;
 * it is idempotent, the same contract as unomedia's um_set_alloc. */
void uoc_icons_install(void);

#endif /* UOICONS_H */
