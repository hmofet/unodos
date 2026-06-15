#ifndef UNOUI_APP_H
#define UNOUI_APP_H
#include "unoui.h"

/* The interactive write-once demo: builds two windows exercising every widget
 * and every interaction (menus, tabs, multi-line editing, sliders, scrollbars,
 * dropdown, drag). The same tree is driven by the abstract event stream on any
 * platform. Buffers are owned here (static), so the app needs no allocator. */
void demo_app_build(unoui_window *editor, unoui_window *palette);

/* widget ids the harness/app can react to */
enum { ID_OK = 1, ID_CANCEL, ID_BODY, ID_NAME, ID_VOL, ID_COUNT,
       ID_FORMAT, ID_WRAP, ID_DARK, ID_TABS, ID_FILES, ID_MENU, ID_APPLY };

#endif
