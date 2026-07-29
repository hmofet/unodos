/* The pc64 web browser app (pc64_browser.c): a native windowed canvas that
 * draws its own chrome - tab strip, toolbar, address bar, Bookmarks and
 * History panels, status line - and renders HTML / Markdown / CSS from the
 * local disks or the network. */
#ifndef PC64_BROWSER_H
#define PC64_BROWSER_H
#include "unoui.h"
unoui_canvas *pc64_browser_canvas(void);
void          pc64_browser_open(void);
void          pc64_browser_open_path(const char *path);  /* Help deep-links */
/* Ctrl accelerators + F5, routed by the shell while the browser is in front:
 * a canvas app never sees a Ctrl-modified character otherwise. 1 = consumed. */
int           pc64_browser_key(int uni, int scan, int ctrl);
#endif
