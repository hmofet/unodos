/* Per-module prelude: stash the KernelApi pointer and expose the kernel
   helpers under their familiar names so app bodies port near-verbatim from
   unodos.c.  Raw Toolbox primitives (SetRect/RGBForeColor/MoveTo/PaintRect/
   TickCount/...) are external symbols resolved against the kernel at dlopen. */
#ifndef UNO_MOD_H
#define UNO_MOD_H
#include "../uno_app.h"
#include <string.h>

static const KernelApi *gK;

#define kPalette       (gK->palette)
#define kBlack         (*gK->black)
#define TBAR_H         (gK->tbar_h)
#define uno_fill       gK->uno_fill
#define uno_box        gK->uno_box
#define uno_invert     gK->uno_invert
#define text_at        gK->text_at
#define text_at_max    gK->text_at_max
#define fill_rgb       gK->fill_rgb
#define fmt_u          gK->fmt_u
#define put2           gK->put2
#define now_secs       gK->now_secs
#define draw_window    gK->draw_window
#define find_app_window gK->find_app_window
#define launch_app     gK->launch_app
#define repaint_all    gK->repaint_all
#define fat12_mount    gK->fat12_mount
#define fat12_list     gK->fat12_list
#define fat12_read     gK->fat12_read
#define fat12_write    gK->fat12_write
#define gFatCount      (*gK->fat_count)
#define gFatNames      (gK->fat_name)
#define gFatSizes      (gK->fat_sizes)
#define music_open_chan gK->music_open_chan
#define music_note_on  gK->music_note_on
#define music_quiet    gK->music_quiet
#define music_start    gK->music_start
#define music_stop     gK->music_stop
#define gm_start       gK->gm_start
#define gm_stop        gK->gm_stop

enum { C_BLUE = 0, C_CYAN = 1, C_MAG = 2, C_WHITE = 3 };

/* Module entry symbol.  On loadable-module targets (host .so, PS2 .uno, DC CD
   romdisk) every module exports the SAME symbol `uno_app_main`, resolved per
   file at load time.  On the classic Mac native build all modules are linked
   into one binary, so each must export a DISTINCT symbol; the build passes
   -DUNO_APP_SYM=uno_app_main_<name> and mac_modload.c's registry resolves it. */
#ifdef UNO_APP_SYM
#  define uno_app_main UNO_APP_SYM
#endif

#endif
