/* uodesk.h - the desktop shell's own internals (see uodesk.c). */
#ifndef UODESK_H
#define UODESK_H

#include "fb.h"

/* the folders Open / Save browse; base = the executable's directory (for the
 * bundled fonts), as SDL_GetBasePath() reports it, or NULL */
void uodesk_fs_init(const char *base);
/* add a folder as a volume (before init, --dir); label NULL = its last name */
void uodesk_fs_add_dir(const char *path, const char *label);

int  uodesk_write_ppm(const char *path, const fb_px *px, int w, int h);

#endif
