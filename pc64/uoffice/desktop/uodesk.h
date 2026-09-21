/* uodesk.h - the desktop shell's own internals (see uodesk.c). */
#ifndef UODESK_H
#define UODESK_H

#include "fb.h"

/* the folders the drawn Open / Save dialog browses; base = where our files
 * are installed (plat_base_path), for the bundled fonts */
void uodesk_fs_init(const char *base);
/* add a folder as a volume (before init, --dir); label NULL = its last name */
void uodesk_fs_add_dir(const char *path, const char *label);
/* a full path the OS picker returned -> its folder's volume (added if new)
 * and the file's name in it; -1 if it cannot be one */
int  uodesk_fs_volume_of(const char *path, char *name, int cap);

int  uodesk_write_ppm(const char *path, const fb_px *px, int w, int h);

#endif
