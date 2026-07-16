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
#define display_res_count gK->display_res_count
#define display_res_get   gK->display_res_get
#define display_res_set   gK->display_res_set

enum { C_BLUE = 0, C_CYAN = 1, C_MAG = 2, C_WHITE = 3 };

/* Module entry symbol.  On loadable-module targets (host .so, PS2 .uno, DC CD
   romdisk) every module exports the SAME symbol `uno_app_main`, resolved per
   file at load time.  On the classic Mac native build all modules are linked
   into one binary, so each must export a DISTINCT symbol; the build passes
   -DUNO_APP_SYM=uno_app_main_<name> and mac_modload.c's registry resolves it. */
#ifdef UNO_APP_SYM
#  define uno_app_main UNO_APP_SYM
#endif

/* ---------------------------------------------------------------------------
 * Dreamcast loadable-module (.KLF) shim.  KallistiOS's elf_load requires every
 * loadable library to export the quartet lib_get_name / lib_get_version /
 * lib_open / lib_close; library_open() calls lib_open() right after relocating
 * the module.  We make lib_open() the bridge into the portable ABI: it hands
 * the module's uno_app_main entry back to the loader (uno_dc_register_entry, a
 * kernel export); the loader then invokes it with the real KernelApi (the
 * loader owns the kapi, so there is no ordering hazard).  This keeps
 * uno_app_main's signature identical to the host/PS2/Mac builds - the DC
 * adapter is entirely in this header, compiled into each module.
 *
 * Built with -DUNO_DC_MODULE so it is active ONLY in the per-app .KLF builds;
 * the main 1ST_READ.BIN never sees it (and links in no app at all).
 * ------------------------------------------------------------------------- */
#ifdef UNO_DC_MODULE
#include <kos/library.h>

/* defined below in every app body */
const AppInterface *uno_app_main(const KernelApi *k);

/* kernel export (dc_modload.c) the module resolves at relocate time: the
   module hands its uno_app_main entry back to the loader, which then invokes
   it with the real KernelApi (the loader owns the kapi, not the module). */
extern void uno_dc_register_entry(UnoAppEntry e);

/* NON-static: elf_load's find_sym() matches these by name in the module's
   symbol table, so they must be global symbols in the .KLF. */
const char  *lib_get_name(void)    { return "unoapp"; }
unsigned int lib_get_version(void) { return 0x00000001; }

int lib_open(klibrary_t *lib)
{
    (void)lib;
    uno_dc_register_entry(uno_app_main);   /* defer the call to the loader */
    return 0;
}

int lib_close(klibrary_t *lib) { (void)lib; return 0; }
#endif /* UNO_DC_MODULE */

#endif
