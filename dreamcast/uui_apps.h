/* ===========================================================================
 * UnoDOS console unoui shell - the legacy-app bridge surface (DC edition).
 *
 * The shell (dc_uui.c) refers to the 11 roster apps by their dense proc index
 * (uno_app.h's APP_* enum, 0..UNOAPP_COUNT-1); the bridge (dc_uui_apps.c)
 * hosts each app unchanged behind a KernelApi and paints/feeds it through a
 * unoui canvas. The console analogue of pc64/pc64_uui_apps.h.
 * ======================================================================== */
#ifndef UUI_APPS_H
#define UUI_APPS_H

#include "unoui.h"

#define UNOAPP_COUNT 11        /* == APP_NAPPS (uno_app.h) */

void        unoapp_setup(int *dirtyflag);   /* wire the KernelApi (call once)  */
const char *unoapp_name(int i);             /* short launcher/taskbar name     */
int         unoapp_is_game(int i);          /* 1 = arcade app (ticks hard)     */
void        unoapp_size(int i, int *w, int *h); /* legacy win w/h incl. TBAR   */
void        unoapp_open(int i);             /* app->opened()                   */
void        unoapp_close(int i);            /* app->closed()                   */
void        unoapp_paint(int i, unoui_rect r);  /* draw into the canvas rect   */
int         unoapp_input(int i, const unoui_event *ev);  /* 1 = consumed       */
void        unoapp_focus(int i);            /* the focused app (-1 = none)     */
void        unoapp_run_tick(int i);         /* per-frame: app tick + game music */

/* implemented by the shell: the KernelApi launch_app hook lands here */
void        uui_shell_launch(short proc);

#endif /* UUI_APPS_H */
