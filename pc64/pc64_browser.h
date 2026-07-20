/* The pc64 web browser app (pc64_browser.c): a native windowed canvas that
 * renders HTML / Markdown / CSS from the local file system. */
#ifndef PC64_BROWSER_H
#define PC64_BROWSER_H
#include "unoui.h"
unoui_canvas *pc64_browser_canvas(void);
void          pc64_browser_open(void);
void          pc64_browser_open_path(const char *path);  /* Help deep-links */
#endif
