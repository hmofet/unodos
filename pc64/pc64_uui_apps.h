/* Shell-facing surface of the legacy-app bridge (pc64_uui_apps.c). The shell
 * refers to each migrated legacy app by a dense 0..UNOAPP_COUNT-1 index and
 * never sees uno_app.h's APP_* enum. See pc64_uui_apps.c for the design. */
#ifndef PC64_UUI_APPS_H
#define PC64_UUI_APPS_H

#include "unoui.h"      /* unoui_rect, unoui_event */

/* Music left this list: it is now a native unoui app (pc64_music.c) rather
 * than a legacy canvas, so it draws with the theme instead of the old fixed
 * four-colour palette. */
#define UNOAPP_COUNT 6  /* Dostris, Pac-Man, OutLast, Tracker, Paint, Network */

void        unoapp_setup(int *dirtyflag);        /* wire the KernelApi once   */
const char *unoapp_name(int i);
int         unoapp_is_game(int i);               /* 1 => fullscreen-preferred */
void        unoapp_size(int i, int *w, int *h);  /* preferred window size     */
void        unoapp_open(int i);                  /* app opened / closed        */
void        unoapp_close(int i);
void        unoapp_paint(int i, unoui_rect r);   /* draw into the canvas rect  */
int         unoapp_input(int i, const unoui_event *ev);  /* 1 = consumed       */
void        unoapp_focus(int i);                 /* -1 = no legacy app focused */
void        unoapp_run_tick(int i);              /* per-frame game/clock tick  */

/* the Studio user-app slot: hosts the .UNO the user just built (classic
 * KernelApi tier), replaced wholesale on every Run */
int         unoapp_user_run(int vol, const char *path);   /* 0 ok, -1 fail */
void        unoapp_user_close(void);
int         unoapp_user_live(void);
const char *unoapp_user_title(void);
void        unoapp_user_size(int *w, int *h);
void        unoapp_user_paint(unoui_rect r);
int         unoapp_user_input(const unoui_event *ev);
void        unoapp_user_tick(void);

#endif
