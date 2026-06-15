/* ===========================================================================
 * uno3d Dreamcast backend - the PowerVR2 (CLX2) via KallistiOS `pvr`.
 *
 * The Dreamcast's PVR is a tile-based deferred hardware rasteriser. As with the
 * PS2 GS backend, the uno3d pipeline hands us screen-space triangles already
 * transformed/projected/culled, and the PVR draws them in hardware with a
 * hardware z-buffer and gouraud interpolation. The SAME demo (uno3d_demo.c) and
 * SAME pipeline (uno3d.c) drive this - only this file differs from the PS2 path.
 *
 * One opaque, gouraud, untextured polygon context is compiled once; each frame
 * is a scene: list_begin -> submit the header + every triangle as a 3-vertex
 * strip -> list_finish -> scene_finish (which flips). PVR depth: larger = nearer,
 * so we submit z = 1 - depth.
 * ======================================================================== */
#include "uno3d_backend.h"
#include <dc/pvr.h>

static pvr_poly_hdr_t g_hdr;

static int dc_init(int w, int h)
{
    pvr_poly_cxt_t cxt;
    (void)w; (void)h;
    pvr_init_defaults();
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);   /* untextured, vertex colour */
    cxt.gen.shading = PVR_SHADE_GOURAUD;
    cxt.gen.culling = PVR_CULLING_NONE;         /* uno3d already back-face culls on the CPU */
    pvr_poly_compile(&g_hdr, &cxt);
    return 0;
}

static void dc_shutdown(void) { pvr_shutdown(); }

static void dc_clear(unsigned char r, unsigned char g, unsigned char b)
{
    pvr_set_bg_color(r / 255.0f, g / 255.0f, b / 255.0f);
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&g_hdr, sizeof(g_hdr));            /* render state for the triangles */
}

static void dc_tri(const u3d_stri *t)
{
    pvr_vertex_t v;
    int i;
    for (i = 0; i < 3; i++) {
        const u3d_sv *s = &t->v[i];
        v.flags = (i == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.x = s->sx;
        v.y = s->sy;
        v.z = 2.0f - s->z;                       /* PVR: larger = nearer, keep z > 0 */
        v.u = 0.0f; v.v = 0.0f;
        v.argb = 0xFF000000u
               | ((unsigned)(unsigned char)s->r << 16)
               | ((unsigned)(unsigned char)s->g << 8)
               | ((unsigned)(unsigned char)s->b);
        v.oargb = 0;
        pvr_prim(&v, sizeof(v));
    }
}

static void dc_flush(void) { pvr_list_finish(); }
static void dc_present(void) { pvr_scene_finish(); }

const u3d_backend u3d_backend_dc = {
    "dc-pvr",
    U3D_CAP_HW | U3D_CAP_ZBUFFER | U3D_CAP_GOURAUD,
    dc_init, dc_shutdown, dc_clear, dc_tri, dc_flush, dc_present
};
