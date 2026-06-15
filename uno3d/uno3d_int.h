/* ===========================================================================
 * uno3d internal types - the screen-space geometry the shared pipeline
 * (uno3d.c) produces and hands to a rasteriser backend. Included by the core
 * and by every backend (uno3d_soft.c, uno3d_ps2.c, uno3d_dc.c, ...).
 * ======================================================================== */
#ifndef UNO3D_INT_H
#define UNO3D_INT_H

/* screen-space vertex: pixel x/y, depth z in [0,1] (0 = near), colour 0..255 */
typedef struct {
    float sx, sy, z;
    float r, g, b;
} u3d_sv;

typedef struct { u3d_sv v[3]; } u3d_stri;

#endif /* UNO3D_INT_H */
