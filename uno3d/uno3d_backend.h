/* ===========================================================================
 * uno3d backend interface - the ONE thing a new platform implements.
 *
 * uno3d is split into a portable front-end (uno3d.c: matrix math, transform,
 * projection, cull, clip - identical everywhere) and a per-platform rasteriser
 * BACKEND described by the vtable below. Porting uno3d to a new machine is:
 *
 *   1. write uno3d_<plat>.c that fills in a `const u3d_backend u3d_backend_<plat>`
 *   2. add `extern` for it here
 *   3. link that file into the port's build and have the port's glue call
 *      `u3d_use_backend(&u3d_backend_<plat>)` before u3d_init()
 *
 * No change to uno3d.c or to any 3D application is ever required. The same app
 * binary-logic runs on the software rasteriser, the PS2 GS, the Dreamcast PVR,
 * and (future) PS3 RSX, PC Direct3D/OpenGL, GameCube GX, Xbox, ... - each is
 * just another file implementing this vtable.
 *
 * A backend receives geometry already transformed to SCREEN SPACE (u3d_stri:
 * pixel x/y, depth 0..1, gouraud colour). So a backend only ever does four
 * things: clear, rasterise a triangle, flush, present. Hardware backends map
 * those onto their triangle pipeline (gsKit prims, PVR vertex lists, GX display
 * lists, D3D draw calls); the software backend rasterises into the framebuffer.
 *
 * Backends may coexist in one build (e.g. a PC links both a Direct3D backend and
 * the software backend and picks at runtime with hardware detection); selection
 * is by pointer, so it can be made at runtime, not just compile time.
 * ======================================================================== */
#ifndef UNO3D_BACKEND_H
#define UNO3D_BACKEND_H

#include "uno3d_int.h"

typedef struct u3d_backend {
    const char *name;                 /* e.g. "soft", "ps2-gs", "dc-pvr" */
    int  caps;                        /* U3D_CAP_* bitmask */
    int  (*init)(int w, int h);       /* bring the renderer/HW up; 0 = ok */
    void (*shutdown)(void);
    void (*clear)(unsigned char r, unsigned char g, unsigned char b);
    void (*tri)(const u3d_stri *t);   /* rasterise one screen-space triangle */
    void (*flush)(void);              /* end-of-geometry kick (DMA/scene close) */
    void (*present)(void);            /* put the finished frame on screen */
} u3d_backend;

/* capability flags (advisory - apps can query via u3d_backend_caps) */
#define U3D_CAP_HW       (1 << 0)     /* hardware-rasterised (not the CPU) */
#define U3D_CAP_ZBUFFER  (1 << 1)     /* real depth buffering */
#define U3D_CAP_GOURAUD  (1 << 2)     /* per-vertex colour interpolation */
#define U3D_CAP_TEXTURE  (1 << 3)     /* textured triangles (future) */

/* choose the active backend (call before u3d_init). */
void        u3d_use_backend(const u3d_backend *be);
const char *u3d_backend_name(void);
int         u3d_backend_caps(void);

/* ---- the backends that exist today (each in its own .c; link what you use) -
 * Referencing one that isn't linked is a link error, not a compile error, so a
 * port only pulls in the backends it actually builds. New platforms append here. */
extern const u3d_backend u3d_backend_soft;   /* uno3d_soft.c - universal (also the
                                              * PC/DOS 386+ renderer; a 386 has no
                                              * 3D hardware) */
extern const u3d_backend u3d_backend_ps2;    /* uno3d_ps2.c  - GS (gsKit) */
extern const u3d_backend u3d_backend_dc;     /* uno3d_dc.c   - PowerVR2 (KOS) */
/* planned HARDWARE backends: u3d_backend_ps3 (RSX), u3d_backend_pc_d3d (D3D/GL on
 * a GPU-equipped PC), u3d_backend_gc (GX), u3d_backend_xbox (D3D8), ... - add the
 * extern when the file lands. (Old/3D-less PCs just use u3d_backend_soft.)        */

#endif /* UNO3D_BACKEND_H */
