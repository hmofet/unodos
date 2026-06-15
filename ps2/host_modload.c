/* ===========================================================================
 * UnoDOS host module loader  -  the host (WSL) implementation of the platform
 * hook uno_load_module(proc).  Apps are SEPARATE artifacts on storage: one
 * shared object per app under $UNO_APPS (default ./apps_store), named
 * app<NN>.so.  We dlopen the file at runtime and resolve its uno_app_main
 * symbol - genuine runtime loading from storage, no app code in the kernel.
 *
 * This is the host analogue of the PS2 mc0:/UnoDOS/Apps/ loader and the DC
 * /cd romdisk loader; on those targets the same uno_load_module signature is
 * implemented over libmc / fs_iso9660 (ee_modload.c / dc_modload.c).
 * ===========================================================================
 */
#include "uno_app.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

UnoAppEntry uno_load_module(short proc)
{
    const char *dir = getenv("UNO_APPS");
    char path[256];
    void *h;
    UnoAppEntry entry;
    if (!dir) dir = "apps_store";
    snprintf(path, sizeof(path), "%s/app%02d.so", dir, (int)proc);
    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "uno_load_module(%d): %s\n", (int)proc, dlerror()); return NULL; }
    entry = (UnoAppEntry)dlsym(h, UNO_APP_ENTRY_NAME);
    if (!entry) { fprintf(stderr, "uno_load_module(%d): no %s in %s\n",
                          (int)proc, UNO_APP_ENTRY_NAME, path); dlclose(h); return NULL; }
    fprintf(stderr, "loaded module %s\n", path);
    return entry;  /* keep the handle open for the program lifetime */
}
