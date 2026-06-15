/* ===========================================================================
 * uno3d - a tiny portable 3D graphics library for UnoDOS.
 *
 * WHY: UnoDOS runs on a dozen wildly different machines. Some have real 3D
 * rasterising hardware (the PS2 Graphics Synthesizer, the Dreamcast PowerVR2);
 * most do not. uno3d lets a 3D application be written ONCE against this API and
 * run on any of them: the CPU-side fixed-function pipeline (matrix math,
 * transform, projection, back-face cull, clip) is identical everywhere, and only
 * the final triangle rasteriser is swapped for a per-platform BACKEND, chosen at
 * compile time:
 *
 *   UNO3D_SOFT   (default) - a portable software rasteriser into the UnoDOS
 *                            software framebuffer (`fb`, from the port's fb.h).
 *                            Works on EVERY port - it needs nothing but a 32-bit
 *                            framebuffer, so it is the universal fallback and the
 *                            host-testable reference.
 *   UNO3D_PS2    - the PlayStation 2 Graphics Synthesizer via gsKit: real
 *                  hardware-rasterised, z-buffered, gouraud triangles.
 *   UNO3D_DC     - the Dreamcast PowerVR2 via KallistiOS pvr: hardware
 *                  tile-based deferred rendering.
 *
 * The application calls only the functions below; it never sees gsKit, the PVR,
 * or the software rasteriser. "Write once, run on any hardware that supports it."
 *
 * Coordinate system: right-handed, +X right, +Y up, -Z into the screen (OpenGL
 * convention). Matrices are column-major (also OpenGL convention).
 * ======================================================================== */
#ifndef UNO3D_H
#define UNO3D_H

typedef struct { float x, y, z; }    u3d_vec3;
typedef struct { float m[16]; }      u3d_mat4;   /* column-major */

/* Application vertex: model-space position + per-vertex RGB (gouraud shaded). */
typedef struct {
    float x, y, z;
    unsigned char r, g, b;
} u3d_vert;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle --------------------------------------------------------- */
/* Bring the renderer up for a w x h viewport. The software backend allocates a
   depth buffer sized to the framebuffer; the GS/PVR backends initialise the 3D
   hardware. Call once. */
void u3d_init(int w, int h);
void u3d_shutdown(void);

/* Start a frame: clear the colour buffer to (r,g,b) and reset the depth buffer. */
void u3d_begin(unsigned char r, unsigned char g, unsigned char b);
/* Finish a frame: flush queued geometry to the rasteriser / hardware. Putting
   the result on screen (a framebuffer blit, a GS flip, a PVR scene submit) is
   the platform glue's job via u3d_present(). */
void u3d_end(void);
/* Present the finished frame to the display (backend + platform specific). */
void u3d_present(void);

/* ---- fixed-function transform (a model-view stack of one + a projection) -- */
void u3d_perspective(float fov_deg, float aspect, float znear, float zfar);
void u3d_load_identity(void);                  /* reset the model-view to I */
void u3d_translate(float x, float y, float z);
void u3d_scale(float x, float y, float z);
void u3d_rotate_x(float deg);
void u3d_rotate_y(float deg);
void u3d_rotate_z(float deg);

/* ---- geometry ---------------------------------------------------------- */
/* Draw tri_count triangles (3 vertices each, model space). The current
   model-view then projection transform them; back faces (clockwise in screen
   space) are culled; triangles crossing behind the near plane are dropped; the
   survivors are gouraud-shaded and depth-tested. */
void u3d_triangles(const u3d_vert *verts, int tri_count);

/* Optional: the average frames-actually-rasterised triangle count of the last
   frame, for HUD/diagnostics. */
int  u3d_last_tris(void);

#ifdef __cplusplus
}
#endif

#endif /* UNO3D_H */
