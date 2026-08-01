#ifndef UNOUI_APP_H
#define UNOUI_APP_H
#include "unoui.h"

/* The interactive write-once demo: builds two windows exercising every widget
 * and every interaction (menus, tabs, multi-line editing, sliders, scrollbars,
 * dropdown, drag). The same tree is driven by the abstract event stream on any
 * platform. Buffers are owned here (static), so the app needs no allocator. */
/* `panes` is built but NOT added to the UI - the harness adds it late, so the
 * MDI container appears only in the frames that are about it. */
void demo_app_build(unoui_window *editor, unoui_window *palette,
                    unoui_window *panes);

/* widget ids the harness/app can react to */
enum { ID_OK = 1, ID_CANCEL, ID_BODY, ID_NAME, ID_VOL, ID_COUNT,
       ID_FORMAT, ID_WRAP, ID_DARK, ID_TABS, ID_FILES, ID_MENU, ID_APPLY,
       ID_DOCTABS, ID_PANES };

/* find a built widget by id (the harness drives the document tabs through the
 * public geometry rather than hard-coded pixels) */
unoui_widget *demo_app_widget(unoui_window *win, int id);
struct unoui_mdi *demo_app_mdi(void);

#endif
