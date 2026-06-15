/* ===========================================================================
 * uno3d demo - a spinning, gouraud-shaded, depth-buffered cube.
 *
 * This is a complete 3D application written ONCE against the uno3d API. It
 * names no platform and no backend: the same demo_frame() runs on the software
 * rasteriser (any UnoDOS port), the PS2 Graphics Synthesizer, and the Dreamcast
 * PowerVR2 - whichever backend the platform glue selected. Each port has a tiny
 * main that picks a backend, drives time, and presents; the 3D itself is here.
 * ======================================================================== */
#include "uno3d.h"

/* a unit cube: 6 faces x 2 triangles, wound counter-clockwise when seen from
   outside, each face a distinct solid colour so the rotation reads clearly. */
#define V(x,y,z,r,g,b) { (float)(x), (float)(y), (float)(z), (r),(g),(b) }

static const u3d_vert g_cube[36] = {
    /* +Z front  (cyan)   */
    V(-1,-1, 1, 0,200,200), V( 1,-1, 1, 0,200,200), V( 1, 1, 1, 0,200,200),
    V(-1,-1, 1, 0,200,200), V( 1, 1, 1, 0,200,200), V(-1, 1, 1, 0,200,200),
    /* -Z back   (magenta)*/
    V( 1,-1,-1, 200,0,200), V(-1,-1,-1, 200,0,200), V(-1, 1,-1, 200,0,200),
    V( 1,-1,-1, 200,0,200), V(-1, 1,-1, 200,0,200), V( 1, 1,-1, 200,0,200),
    /* +X right  (red)    */
    V( 1,-1, 1, 220,40,40), V( 1,-1,-1, 220,40,40), V( 1, 1,-1, 220,40,40),
    V( 1,-1, 1, 220,40,40), V( 1, 1,-1, 220,40,40), V( 1, 1, 1, 220,40,40),
    /* -X left   (green)  */
    V(-1,-1,-1, 40,200,40), V(-1,-1, 1, 40,200,40), V(-1, 1, 1, 40,200,40),
    V(-1,-1,-1, 40,200,40), V(-1, 1, 1, 40,200,40), V(-1, 1,-1, 40,200,40),
    /* +Y top    (white)  */
    V(-1, 1, 1, 235,235,235), V( 1, 1, 1, 235,235,235), V( 1, 1,-1, 235,235,235),
    V(-1, 1, 1, 235,235,235), V( 1, 1,-1, 235,235,235), V(-1, 1,-1, 235,235,235),
    /* -Y bottom (blue)   */
    V(-1,-1,-1, 60,90,235), V( 1,-1,-1, 60,90,235), V( 1,-1, 1, 60,90,235),
    V(-1,-1,-1, 60,90,235), V( 1,-1, 1, 60,90,235), V(-1,-1, 1, 60,90,235),
};

static float g_aspect = 4.0f / 3.0f;

void demo_init(int w, int h)
{
    g_aspect = (h > 0) ? (float)w / (float)h : 4.0f / 3.0f;
}

/* render one frame. t is elapsed seconds (drives the rotation). */
void demo_frame(float t)
{
    u3d_begin(0, 0, 40);                         /* clear to deep blue */
    u3d_perspective(60.0f, g_aspect, 0.1f, 100.0f);
    u3d_load_identity();
    u3d_translate(0.0f, 0.0f, -4.5f);            /* push the cube in front */
    u3d_rotate_y(t * 45.0f);                      /* spin */
    u3d_rotate_x(t * 30.0f);
    u3d_triangles(g_cube, 12);
    u3d_end();
}
