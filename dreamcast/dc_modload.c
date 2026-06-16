/* ===========================================================================
 * UnoDOS/Dreamcast native module loader  -  GENUINE runtime CD module loading.
 *
 * Each of the 11 apps ships as a SEPARATE relocatable-ELF module on the CD
 * image at /cd/UNODOS/APPS/APPNN.KLF.  This loader uses KallistiOS's real
 * dynamic-library machinery to load and RELOCATE the module at runtime:
 *
 *     library_open(name, "/cd/UNODOS/APPS/APPNN.KLF")
 *         -> elf_load()      reads the .KLF off the ISO9660 FS (fs_iso9660),
 *                            allocates a fresh memory image, applies the R_SH
 *                            relocations, and resolves every undefined symbol
 *                            against the kernel export tables (export_lookup).
 *         -> lib->lib_open() KOS then calls the module's lib_open(), which (in
 *                            the module's UNO_DC shim, see apps/uno_mod.h) runs
 *                            uno_app_main(kapi) and registers the returned
 *                            AppInterface back here via uno_dc_register_iface().
 *
 * The app modules reference Mac-Toolbox primitives (SetRect/MoveTo/PaintRect/
 * TickCount/NewPtr/...), libc (memcpy/strcpy/...) and a couple of libgcc SH
 * helpers (__sdivsi3_i4i/__movmem_i4_even).  The KOS kernel export table only
 * carries libc, so this file registers a SECOND export symtab (gUnoSymtab) with
 * the Toolbox + libgcc symbols, added to the name manager with
 * nmmgr_handler_add().  export_lookup() walks every registered symtab, so the
 * module's undefined references resolve against the running kernel image.
 *
 * Proof this is real load-from-CD (not link-in): nm/objdump of 1ST_READ.BIN
 * shows NO app symbols (sysinfo_/pacman_/dostris_/... and no uno_app_main); the
 * app code lives only in the APPNN.KLF files on the CD and is relocated into a
 * fresh heap image at launch.
 * ===========================================================================
 */
#define _GNU_SOURCE   /* stpcpy */
#include "uno_app.h"
#include "fb.h"
#include <kos/library.h>
#include <kos/exports.h>
#include <kos/nmmgr.h>
#include <kos/dbglog.h>
#include <kos/cache.h>
#include <arch/types.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef UNO_DC_LOADDBG
/* on-screen diagnostic: draw a status line at the bottom of the framebuffer so
   the load outcome shows up in a headless Flycast screenshot (serial capture is
   unreliable headless).  Off in normal builds (-DUNO_DC_LOADDBG to enable). */
static void load_dbg(const char *msg)
{
    fb_fill_rect(0, FB_H - 12, FB_W, 12, FB_RGB(0,0,0));
    fb_text(2, FB_H - 10, msg, FB_RGB(0xFF,0xFF,0x40), -1);
}
#else
#define load_dbg(m) ((void)0)
#endif

/* ---- the entry the just-loaded module hands back ------------------------ *
 * The module's lib_open() calls uno_dc_register_entry() (exported below) with
 * its uno_app_main function pointer.  library_open() invokes lib_open()
 * synchronously, so we read gPendingEntry right after it returns, then call it
 * ourselves with the kernel's KernelApi (we own the kapi; the module never
 * touches it directly - no init-ordering hazard). */
static UnoAppEntry gPendingEntry;

/* exported to the module (resolved by elf_load at relocate time) */
void uno_dc_register_entry(UnoAppEntry e) { gPendingEntry = e; }

/* ===========================================================================
 * Custom export symtab: the Toolbox + libgcc symbols the kernel table lacks.
 * These are ordinary symbols in the linked main image (mac_compat.c/mac_io.c
 * for the Toolbox; libgcc for the SH helpers), so &Name is their live address.
 * Names carry NO leading underscore - elf_load strips ELF_SYM_PREFIX_LEN before
 * calling export_lookup (matches the kernel_symtab convention).
 * ========================================================================= */

/* libgcc SH integer helpers some modules pull in (signed div, struct move). */
extern void __sdivsi3_i4i(void);
extern void __movmem_i4_even(void);
/* stpcpy is a GNU extension; declare it in case the headers hide it */
extern char *stpcpy(char *, const char *);

#define EXP(sym) { #sym, (uintptr_t)(&sym) }

/* forward decl: the loader callback the module calls from lib_open() */
void uno_dc_register_entry(UnoAppEntry e);

static export_sym_t gUnoExports[] = {
    /* the loader hook the module's lib_open() invokes */
    EXP(uno_dc_register_entry),
    /* QuickDraw rect math */
    EXP(SetRect), EXP(OffsetRect), EXP(InsetRect), EXP(PtInRect),
    /* colour / pen / text state */
    EXP(RGBForeColor), EXP(PenMode), EXP(PenNormal), EXP(TextMode),
    /* drawing */
    EXP(MoveTo), EXP(LineTo), EXP(PaintRect), EXP(FrameRect),
    EXP(PaintOval), EXP(FrameOval), EXP(DrawText), EXP(TextWidth),
    /* events / time / input / memory */
    EXP(TickCount), EXP(GetMouse), EXP(StillDown), EXP(Random),
    EXP(NewPtr), EXP(DisposePtr),
    /* libc string/memory the modules use.  CRITICAL: the running kernel's own
       export table (kernel_symtab) is NOT linked into an ordinary kos-cc app,
       so export_lookup would NOT find these - we must publish them ourselves,
       else elf_load aborts a module with "symbol '_strcat' is undefined". */
    EXP(memcpy), EXP(memmove), EXP(memset),
    EXP(strcpy), EXP(strcat), EXP(strncpy), EXP(strlen),
    { "stpcpy", (uintptr_t)&stpcpy },
    /* libgcc SH helpers (referenced by name, not via & for code symbols) */
    { "__sdivsi3_i4i",    (uintptr_t)&__sdivsi3_i4i },
    { "__movmem_i4_even", (uintptr_t)&__movmem_i4_even },
    { NULL, 0 }
};

static symtab_handler_t gUnoSymtab = {
    {
        "sym/uno/toolbox",
        0,
        0x00010000,
        0,
        NMMGR_TYPE_SYMTAB,
        NMMGR_LIST_INIT
    },
    gUnoExports
};

static int gExportsRegistered = 0;

static void register_uno_exports(void)
{
    if (gExportsRegistered) return;
    nmmgr_handler_add(&gUnoSymtab.nmmgr);   /* export_lookup now sees our table */
    gExportsRegistered = 1;
}

/* ===========================================================================
 * The platform hook the generic loader (app_loader.c) calls.  It returns a
 * UnoAppEntry; app_loader.c then calls entry(&gKApi) to get the AppInterface.
 * The genuine load + relocate happened in library_open() below; the entry we
 * return is the module's own uno_app_main (resolved from the relocated image),
 * so app_loader.c's entry(&gKApi) executes APP CODE THAT WAS LOADED FROM THE CD.
 * ========================================================================= */

UnoAppEntry uno_load_module(short proc)
{
    char path[64], libname[16];
    klibrary_t *lib;

    if (proc < 0 || proc >= APP_NAPPS) return NULL;

    register_uno_exports();

    snprintf(path, sizeof(path), "/cd/UNODOS/APPS/APP%02d.KLF", (int)proc);
    snprintf(libname, sizeof(libname), "unoapp%02d", (int)proc);

    gPendingEntry = NULL;

    /* GENUINE load + relocate from the CD ISO9660 filesystem.  KOS reads the
       .KLF off /cd, allocates a fresh image, applies SH relocations, resolves
       undefined symbols via export_lookup (kernel table + our gUnoSymtab), then
       calls the module's lib_open() -> uno_dc_register_entry(uno_app_main). */
    errno = 0;
    lib = library_open(libname, path);
    if (!lib) {
        char m[80];
        snprintf(m, sizeof m, "library_open('%s') FAIL errno=%d", path, errno);
        load_dbg(m);
        dbglog(DBG_ERROR, "uno_load_module(%d): %s\n", (int)proc, m);
        return NULL;
    }

    if (!gPendingEntry) {
        load_dbg("library_open OK but module registered NO entry");
        dbglog(DBG_ERROR, "uno_load_module(%d): module '%s' registered no "
               "uno_app_main entry\n", (int)proc, path);
        return NULL;
    }

    /* Cache coherency for the just-relocated image: elf_load only icache-syncs;
       force the relocated bytes (written through the operand cache) back to RAM
       and invalidate stale I-cache lines so the freshly-loaded code is executed
       as written.  Important for self-loaded code under cache-accurate cores. */
    if (lib->image.data && lib->image.size) {
        dcache_purge_range((uintptr_t)lib->image.data, lib->image.size);
        icache_inval_range((uintptr_t)lib->image.data, lib->image.size);
    }

#ifdef UNO_DC_LOADDBG
    {   char m[80];
        snprintf(m, sizeof m, "LOADED+RELOCATED %s from CD  entry@%p",
                 path, (void *)gPendingEntry);
        load_dbg(m);
    }
#endif
    dbglog(DBG_INFO, "uno_load_module(%d): loaded+relocated '%s' from CD; "
           "uno_app_main @ %p\n", (int)proc, path, (void *)gPendingEntry);

    return gPendingEntry;   /* app_loader.c calls this with the kernel KernelApi */
}
