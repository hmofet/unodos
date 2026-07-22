/* ===========================================================================
 * UnoDOS/pc64 - the Python-runtime module ABI (PYRT.UNO).
 *
 * A third .UNO tier alongside the classic (uno_app.h) and unoui-class
 * (uno_uuiapp.h) modules.  Two new UnoModHdr.flags bits:
 *
 *   UNO_MODF_PY     the module IS the Python runtime (MicroPython + the `uno`
 *                   bindings).  Its uno_app_main returns a PyHost*, not an app.
 *   UNO_MODF_PYAPP  a container holding a Python program (source bytes); no
 *                   code/relocs/imports - the shell hands the payload to the
 *                   loaded PYRT, which compiles it and returns a UnoUuiApp the
 *                   shell hosts exactly like Studio's.
 *
 * Modules cannot import from each other, so PYRT is the single bridge: the
 * shell loads it once, holds its PyHost*, and routes every Python app through
 * load().  Python is optional - no PYRT.UNO on the disk => no Python, and the
 * base OS is unchanged (like Studio).
 * ======================================================================== */
#ifndef PC64_PYHOST_H
#define PC64_PYHOST_H

#include "uno_uuiapp.h"     /* UnoUuiApp - what a hosted Python app looks like */

#define UNO_MODF_PY     0x0002
#define UNO_MODF_PYAPP  0x0004
#define UNO_PYHOST_ABI  2       /* +run_src (ABI 2) */

typedef struct PyHost {
    int abi;                                     /* UNO_PYHOST_ABI */
    /* one-time: mp_init + gc_init over a caller-owned heap block */
    int  (*init)(void *gc_heap, unsigned long gc_size);
    /* compile+exec a Python program, bind its `app` object; returns a
     * UnoUuiApp whose build/action/key/frame/draw call into Python, or 0 on
     * error (see last_error()).  name is for tracebacks. */
    const UnoUuiApp *(*load)(const unsigned char *src, int len, const char *name);
    void (*unload)(void);                        /* drop the current app's roots */
    const char *(*last_error)(void);             /* traceback text for Studio */
    /* [ABI 2] exec a Python source string in the current runtime (statements
     * allowed), capturing stdout into out (NUL-terminated, capped).  Returns 0
     * on success, <0 on error (out carries the traceback).  Requires init().
     * Shares the VM/global namespace with any running Python app. */
    int (*run_src)(const char *src, int len, char *out, int cap);
} PyHost;

typedef const PyHost *(*PyHostEntry)(void *reserved);

/* loader side (pc64_modload.c) */
PyHostEntry uno_mod_load_pyrt(void);             /* load PYRT.UNO, require MODF_PY */
/* read a PYAPP container's source payload out of the staging buffer (no exec
 * pages, no rebase); *src points into transient module-load memory. */
int uno_mod_load_pyapp(int vol, const char *path, const unsigned char **src, int *len);
unsigned short uno_mod_peek_flags(int vol, const char *path);  /* header flags, 0 if absent */

#endif /* PC64_PYHOST_H */
