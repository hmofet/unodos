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
#include <kos/dbgio.h>
#include <arch/types.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef UNO_DC_LOADDBG
#include <dirent.h>
#endif

#ifdef UNO_DC_LOADDBG
/* on-screen diagnostic: draw a status line at the bottom of the framebuffer so
   the Flycast screenshot itself reports the load outcome (serial capture is
   unreliable headless).  Disabled in normal builds. */
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

#ifdef UNO_DC_LOADDBG
    {   /* probe: list /cd/UNODOS/APPS and try several name spellings */
        char m[120]; int y = FB_H - 48; const char *try_paths[4];
        char p1[64], p2[64], p3[64];
        DIR *d; struct dirent *de; int k = 0;
        fb_fill_rect(0, FB_H - 56, FB_W, 56, FB_RGB(0,0,0));
        snprintf(p1,sizeof p1,"/cd/UNODOS/APPS/APP%02d.KLF",(int)proc);
        snprintf(p2,sizeof p2,"/cd/UNODOS/APPS/APP%02d.KLF;1",(int)proc);
        snprintf(p3,sizeof p3,"/cd/unodos/apps/app%02d.klf",(int)proc);
        try_paths[0]=p1; try_paths[1]=p2; try_paths[2]=p3; try_paths[3]=0;
        for (k=0; try_paths[k]; k++) {
            FILE *pf = fopen(try_paths[k],"rb");
            if (pf){ unsigned char h[4]={0}; fread(h,1,4,pf); fclose(pf);
                snprintf(m,sizeof m,"OPEN OK: %s hdr=%02x%02x%02x%02x",
                         try_paths[k],h[0],h[1],h[2],h[3]); }
            else snprintf(m,sizeof m,"open FAIL e=%d: %s",errno,try_paths[k]);
            fb_text(2,y,m,FB_RGB(0xFF,0xFF,0x40),-1); y += 9;
        }
        d = opendir("/cd/UNODOS/APPS");
        if (d){ char ls[120]="ls APPS:"; int o=8;
            while((de=readdir(d))){ int l=strlen(de->d_name);
                if(o+l+1<(int)sizeof ls){ ls[o++]=' ';
                    memcpy(ls+o,de->d_name,l); o+=l; ls[o]=0; } }
            closedir(d); fb_text(2,y,ls,FB_RGB(0x40,0xFF,0xFF),-1); }
        else { snprintf(m,sizeof m,"opendir /cd/UNODOS/APPS FAIL e=%d",errno);
            fb_text(2,y,m,FB_RGB(0xFF,0x80,0x80),-1); }
    }
#endif

    /* GENUINE load + relocate from the CD ISO9660 filesystem.  KOS reads the
       .KLF off /cd, allocates a fresh image, applies SH relocations, resolves
       undefined symbols via export_lookup (kernel table + our gUnoSymtab), then
       calls the module's lib_open() -> uno_dc_register_entry(uno_app_main). */
    errno = 0;
    lib = library_open(libname, path);
    if (!lib) {
        char m[80]; snprintf(m,sizeof m,"library_open('%s') FAIL errno=%d",path,errno);
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

#ifdef UNO_DC_LOADHALT
    {   /* prove library_open returned (lib_open already ran inside it) BEFORE
           we dare call the entry - isolates load vs entry-execution crash */
        char m[96]; const AppInterface *ai;
        fb_clear(FB_RGB(0,0,0x60));
        fb_big_text(20,40,"library_open RETURNED",FB_RGB(0xFF,0xFF,0x40),-1,3);
        snprintf(m,sizeof m,"lib@%p entry@%p",(void*)lib,(void*)gPendingEntry);
        fb_big_text(20,90,m,FB_RGB(0xFF,0xFF,0xFF),-1,2);
        uno_dc_present(); uno_dc_present();
        ai = gPendingEntry(0);                 /* now run the relocated entry */
        fb_big_text(20,130,"entry() RETURNED",FB_RGB(0x80,0xFF,0x80),-1,3);
        snprintf(m,sizeof m,"ai@%p draw@%p title=%s",(void*)ai,
                 ai?(void*)ai->draw:0, (ai&&ai->win_title)?ai->win_title:"?");
        fb_big_text(20,180,m,FB_RGB(0x80,0xFF,0xFF),-1,2);
        for(;;) uno_dc_present();
    }
#endif
    dbglog(DBG_INFO, "uno_load_module(%d): loaded+relocated '%s' from CD; "
           "uno_app_main @ %p\n", (int)proc, path, (void *)gPendingEntry);

    return gPendingEntry;   /* app_loader.c calls this with the kernel KernelApi */
}
