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

/* Module entry symbol.  On loadable-module targets the module exports the entry
   the platform loader resolves:
     - host .so / PS2 .uno / DC romdisk : same symbol `uno_app_main`, resolved
       per file at load time.
     - classic Mac native            : each app is built as a SEPARATE, flat
       'CODE' resource (-Wl,--mac-flat, entry = uno_app_main) that the kernel
       brings in with GetResource at runtime - the app is NOT linked into the
       kernel binary.  A flat code resource is loaded at an address unknown at
       link time, so before it touches any global (gK, kIface, string literals)
       it must run the Retro68 runtime relocator.  We therefore rename the app's
       hand-written entry to `uno_app_main_impl` and synthesise the real
       `uno_app_main` entry stub here: it relocates, then tail-calls the impl.
       (RETRO68_RELOCATE is position-independent - it uses only PC-relative
       addressing - so it is safe to call before relocation has happened.) */
#if defined(COMPILING_AS_CODE_RESOURCE)
#  include <Retro68Runtime.h>
#  define uno_app_main uno_app_main_impl
   static const AppInterface *uno_app_main_impl(const KernelApi *k);
   /* The real loaded-resource entry point (linker entry = uno_app_main). */
   const AppInterface *uno_app_main_entry(const KernelApi *k);
   const AppInterface *uno_app_main_entry(const KernelApi *k)
   {
       RETRO68_RELOCATE();          /* fix up this resource's globals first */
       return uno_app_main_impl(k);
   }
#elif defined(UNO_APP_SYM)
/* legacy single-binary fallback: distinct symbol per app, resolved by registry */
#  define uno_app_main UNO_APP_SYM
#endif

#endif
