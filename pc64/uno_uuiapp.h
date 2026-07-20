/* ===========================================================================
 * UnoDOS/pc64 - the unoui-class module ABI.
 *
 * The classic .UNO tier (uno_app.h) hosts an app on a 4-colour Toolbox
 * canvas.  A unoui-CLASS module (UnoModHdr.flags bit 0) is a full desktop
 * citizen instead: the shell owns its unoui_window, and the module fills it
 * with real widgets and drives them through the same build/action/key/frame
 * hook set the built-in apps (Editor, Files, ...) use.  Everything else -
 * toolkit calls, fonts, the FAT filesystem, malloc - comes through named
 * kernel exports, exactly like any other module import.
 *
 * Studio (the IDE) is the first unoui-class module; a distro that doesn't
 * want it simply doesn't ship APPS\STUDIO.UNO.
 * ======================================================================== */
#ifndef UNO_UUIAPP_H
#define UNO_UUIAPP_H

#include "unoui.h"

#define UNO_MODF_UUI   0x0001      /* UnoModHdr.flags: unoui-class module */
#define UNO_UUIAPP_ABI 1

typedef struct UnoUuiApp {
    int         abi;               /* UNO_UUIAPP_ABI */
    const char *name;              /* launcher / taskbar label */
    void (*build)(unoui_window *w);          /* lay the widgets out       */
    int  (*action)(const unoui_action *a);   /* 1 = consumed              */
    int  (*key)(int uni, int scan, int ctrl);/* accelerators (scan carries
                                                F-keys); 1 = consumed     */
    void (*frame)(void);                     /* per-frame (caret, pumps)  */
    void (*opened)(void);
    void (*closed)(void);
    int  (*canvas_index)(void);              /* initial focus widget, -1  */
} UnoUuiApp;

/* module entry (same entry_rva slot as classic modules; the flags bit picks
 * the signature): const UnoUuiApp *uno_app_main(void *reserved) */
typedef const UnoUuiApp *(*UnoUuiEntry)(void *reserved);

/* loader side (pc64_modload.c) */
int         uno_mod_present(const char *file);    /* APPS\<file> exists?     */
UnoUuiEntry uno_mod_load_uui(const char *file);   /* load, require flags bit0 */
void        uno_mod_unload_user(void);            /* drop the user-app slot  */

#endif /* UNO_UUIAPP_H */
