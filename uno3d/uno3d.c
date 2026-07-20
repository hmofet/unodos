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
 * near-plane clipping
 *
 * A clip-space vertex (post model-view-projection, BEFORE the perspective
 * divide) plus its gouraud colour, so colours interpolate correctly across a
 * clip-generated vertex. */
#define U3D_WNEAR 0.0001f          /* the near plane, expressed as clip w = eps */

typedef struct { float cx, cy, cz, cw, r, g, b; } u3d_cv;

/* Sutherland-Hodgman clip of a convex polygon (here always a triangle, nin = 3)
   against the single half-space cw >= U3D_WNEAR. Vertices in front of the near
   plane are kept; each edge that crosses the plane contributes one new vertex
   interpolated to exactly cw = eps. A wholly-in-front triangle returns its three
   vertices unchanged (so the downstream projection is bit-identical to the old
   no-clip path); a wholly-behind one returns 0; a crossing one returns 4 (fanned
   into two triangles) or 3. Doing this before the divide is what recovers the
   corridor floor/ceiling geometry that the old whole-triangle reject dropped,
   and what stops a barely-surviving vertex from projecting to a near-infinite
   screen coordinate whose bounding box was the whole display. */
static int u3d_clip_near(const u3d_cv *in, int nin, u3d_cv *out)
{
    int nout = 0, i;
    for (i = 0; i < nin; i++) {
        const u3d_cv *a = &in[i];
        const u3d_cv *b = &in[(i + 1) % nin];
        float da = a->cw - U3D_WNEAR;          /* signed distance to the plane   */
        float db = b->cw - U3D_WNEAR;          /* (>= 0 means in front)          */
        int ain = (da >= 0.0f), bin = (db >= 0.0f);
        if (ain) out[nout++] = *a;
        if (ain != bin) {                      /* edge crosses: emit intersection */
            float t = da / (da - db);          /* da != db when signs differ     */
            u3d_cv *o = &out[nout++];
            o->cx = a->cx + t * (b->cx - a->cx);
            o->cy = a->cy + t * (b->cy - a->cy);
            o->cz = a->cz + t * (b->cz - a->cz);
            o->cw = a->cw + t * (b->cw - a->cw);
            o->r  = a->r  + t * (b->r  - a->r);
            o->g  = a->g  + t * (b->g  - a->g);
            o->b  = a->b  + t * (b->b  - a->b);
        }
    }
    return nout;
}

/* =========================================================================
 * the transform front-end: model space -> screen-space triangle queue
 * ======================================================================== */
void u3d_triangles(const u3d_vert *verts, int tri_count)
{
    u3d_mat4 mvp;
    int i, j, k, nclip;
    u3d_cv cin[3], cout[6];        /* one plane clips a tri to at most 4 verts */
    mat_mul(&mvp, &g_proj, &g_mv);

    for (i = 0; i < tri_count; i++) {
        const u3d_vert *tv = &verts[i * 3];

        /* transform the three vertices to clip space (carry colour) */
        for (j = 0; j < 3; j++) {
            float x = tv[j].x, y = tv[j].y, z = tv[j].z;
            cin[j].cx = mvp.m[0]*x + mvp.m[4]*y + mvp.m[8]*z  + mvp.m[12];
            cin[j].cy = mvp.m[1]*x + mvp.m[5]*y + mvp.m[9]*z  + mvp.m[13];
            cin[j].cz = mvp.m[2]*x + mvp.m[6]*y + mvp.m[10]*z + mvp.m[14];
            cin[j].cw = mvp.m[3]*x + mvp.m[7]*y + mvp.m[11]*z + mvp.m[15];
            cin[j].r  = tv[j].r;
            cin[j].g  = tv[j].g;
            cin[j].b  = tv[j].b;
        }

        nclip = u3d_clip_near(cin, 3, cout);   /* 0, 3 or 4 vertices out */

        /* fan-triangulate the clipped polygon: (0,1,2), (0,2,3), ... */
        for (k = 2; k < nclip; k++) {
            const u3d_cv *p[3];
            u3d_sv sv[3];
            float area;

            p[0] = &cout[0]; p[1] = &cout[k - 1]; p[2] = &cout[k];
            for (j = 0; j < 3; j++) {
                float inv = 1.0f / p[j]->cw;    /* perspective divide (per vertex) */
                sv[j].sx = (p[j]->cx * inv * 0.5f + 0.5f) * (float)g_w;
                sv[j].sy = (1.0f - (p[j]->cy * inv * 0.5f + 0.5f)) * (float)g_h;
                sv[j].z  = p[j]->cz * inv * 0.5f + 0.5f;
                sv[j].r  = p[j]->r;
                sv[j].g  = p[j]->g;
                sv[j].b  = p[j]->b;
            }

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
}
