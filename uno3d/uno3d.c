/* ===========================================================================
 * uno3d core - the portable fixed-function pipeline (platform-independent).
 *
 * Matrix math, vertex transform, projection, back-face cull and near-plane
 * reject live here and are identical on every platform. The transformed,
 * screen-space triangles are queued and handed to the active rasteriser BACKEND
 * (a u3d_backend vtable - see uno3d_backend.h) which is the only per-platform
 * part. This file contains NO platform code and never needs editing to add a
 * new machine.
 *
 * Coordinates: right-handed, +X right, +Y up, -Z into screen; column-major
 * matrices (OpenGL conventions).
 * ======================================================================== */
#include "uno3d.h"
#include "uno3d_backend.h"
#include <math.h>
#include <string.h>

#define U3D_PI     3.14159265358979f
#define U3D_MAXTRI 8192           /* per-frame transformed-triangle queue */

/* ---- pipeline state ---------------------------------------------------- */
static u3d_mat4 g_proj;
static u3d_mat4 g_mv;
static int      g_w = 640, g_h = 448;
static int      g_tris;                       /* tris rasterised this frame */

static const u3d_backend *g_be;               /* active rasteriser */

static u3d_stri g_queue[U3D_MAXTRI];
static int      g_nq;

/* =========================================================================
 * backend selection
 * ======================================================================== */
void u3d_use_backend(const u3d_backend *be) { g_be = be; }
const char *u3d_backend_name(void) { return g_be ? g_be->name : "none"; }
int  u3d_backend_caps(void) { return g_be ? g_be->caps : 0; }

/* =========================================================================
 * matrix math (column-major: element (row r, col c) = m[c*4 + r])
 * ======================================================================== */
static void mat_identity(u3d_mat4 *o)
{
    memset(o, 0, sizeof(*o));
    o->m[0] = o->m[5] = o->m[10] = o->m[15] = 1.0f;
}

static void mat_mul(u3d_mat4 *o, const u3d_mat4 *a, const u3d_mat4 *b)
{
    u3d_mat4 t;
    int c, r, k;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += a->m[k * 4 + r] * b->m[c * 4 + k];
            t.m[c * 4 + r] = s;
        }
    *o = t;
}

void u3d_perspective(float fov_deg, float aspect, float znear, float zfar)
{
    float f = 1.0f / tanf(fov_deg * 0.5f * U3D_PI / 180.0f);
    memset(&g_proj, 0, sizeof(g_proj));
    g_proj.m[0]  = f / aspect;
    g_proj.m[5]  = f;
    g_proj.m[10] = (zfar + znear) / (znear - zfar);
    g_proj.m[11] = -1.0f;
    g_proj.m[14] = (2.0f * zfar * znear) / (znear - zfar);
}

void u3d_load_identity(void) { mat_identity(&g_mv); }

void u3d_translate(float x, float y, float z)
{
    u3d_mat4 t; mat_identity(&t);
    t.m[12] = x; t.m[13] = y; t.m[14] = z;
    mat_mul(&g_mv, &g_mv, &t);
}

void u3d_scale(float x, float y, float z)
{
    u3d_mat4 s; mat_identity(&s);
    s.m[0] = x; s.m[5] = y; s.m[10] = z;
    mat_mul(&g_mv, &g_mv, &s);
}

static void rotate_plane(float deg, int a, int b)
{
    u3d_mat4 r; float c = cosf(deg * U3D_PI / 180.0f), s = sinf(deg * U3D_PI / 180.0f);
    mat_identity(&r);
    r.m[a * 4 + a] = c;  r.m[b * 4 + b] = c;
    r.m[b * 4 + a] = -s; r.m[a * 4 + b] = s;
    mat_mul(&g_mv, &g_mv, &r);
}
void u3d_rotate_x(float deg) { rotate_plane(deg, 1, 2); }
void u3d_rotate_y(float deg) { rotate_plane(deg, 2, 0); }
void u3d_rotate_z(float deg) { rotate_plane(deg, 0, 1); }

/* =========================================================================
 * lifecycle (dispatched to the active backend)
 * ======================================================================== */
void u3d_init(int w, int h)
{
    g_w = w; g_h = h;
    /* the platform glue must pick a backend (u3d_use_backend) before init; this
       keeps hardware-only builds from being forced to link the software path. */
    if (g_be && g_be->init) g_be->init(w, h);
}
void u3d_shutdown(void) { if (g_be && g_be->shutdown) g_be->shutdown(); }

void u3d_begin(unsigned char r, unsigned char g, unsigned char b)
{
    g_nq = 0; g_tris = 0;
    if (g_be && g_be->clear) g_be->clear(r, g, b);
}

void u3d_end(void)
{
    int i;
    if (g_be && g_be->tri)
        for (i = 0; i < g_nq; i++) g_be->tri(&g_queue[i]);
    g_tris = g_nq;
    if (g_be && g_be->flush) g_be->flush();
}

void u3d_present(void) { if (g_be && g_be->present) g_be->present(); }

int u3d_last_tris(void) { return g_tris; }

/* =========================================================================
 * the transform front-end: model space -> screen-space triangle queue
 * ======================================================================== */
void u3d_triangles(const u3d_vert *verts, int tri_count)
{
    u3d_mat4 mvp;
    int i, j;
    mat_mul(&mvp, &g_proj, &g_mv);

    for (i = 0; i < tri_count; i++) {
        const u3d_vert *tv = &verts[i * 3];
        u3d_sv sv[3];
        float area;
        int behind = 0;

        for (j = 0; j < 3; j++) {
            float x = tv[j].x, y = tv[j].y, z = tv[j].z;
            float cx = mvp.m[0]*x + mvp.m[4]*y + mvp.m[8]*z  + mvp.m[12];
            float cy = mvp.m[1]*x + mvp.m[5]*y + mvp.m[9]*z  + mvp.m[13];
            float cz = mvp.m[2]*x + mvp.m[6]*y + mvp.m[10]*z + mvp.m[14];
            float cw = mvp.m[3]*x + mvp.m[7]*y + mvp.m[11]*z + mvp.m[15];
            if (cw < 0.0001f) { behind = 1; break; }     /* near-plane reject */
            {
                float inv = 1.0f / cw;
                sv[j].sx = (cx * inv * 0.5f + 0.5f) * (float)g_w;
                sv[j].sy = (1.0f - (cy * inv * 0.5f + 0.5f)) * (float)g_h;
                sv[j].z  = cz * inv * 0.5f + 0.5f;
                sv[j].r  = tv[j].r;
                sv[j].g  = tv[j].g;
                sv[j].b  = tv[j].b;
            }
        }
        if (behind) continue;

        /* back-face cull: screen-space signed area (y down, so a model front
           face winds clockwise here -> negative area is front-facing) */
        area = (sv[1].sx - sv[0].sx) * (sv[2].sy - sv[0].sy)
             - (sv[2].sx - sv[0].sx) * (sv[1].sy - sv[0].sy);
        if (area >= 0.0f) continue;   /* back-face cull (front faces wind CW) */

        if (g_nq < U3D_MAXTRI) {
            g_queue[g_nq].v[0] = sv[0];
            g_queue[g_nq].v[1] = sv[1];
            g_queue[g_nq].v[2] = sv[2];
            g_nq++;
        }
    }
}
