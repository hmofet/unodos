/* ===========================================================================
 * The `uno` MicroPython module - the friendly Python API for UnoDOS.
 *
 * Thin C wrappers over the kernel exports (framebuffer, unosound, uno3d, the
 * filesystem), plus the `uno.App` base class an app subclasses and a canvas
 * object passed to draw().  Compiled into PYRT.UNO; reaches the platform
 * through the module import table like any .UNO.
 *
 *   import uno
 *   class Game(uno.App):
 *       name = "Game"
 *       def draw(self, cv): cv.clear(uno.rgb(0,0,0)); cv.text(8,8,"hi",uno.rgb(255,255,255))
 *       def tick(self): ...
 *   app = Game()
 * ======================================================================== */
#include "py/runtime.h"
#include "py/objtype.h"
#include "py/obj.h"

/* ---- kernel exports (resolved by the .UNO loader) ------------------------- */
typedef unsigned int fb_px;
void fb_fill_rect(int x, int y, int w, int h, fb_px c);
void fb_hline(int x, int y, int w, fb_px c);
void fb_vline(int x, int y, int h, fb_px c);
void fb_pixel(int x, int y, fb_px c);
void fb_blit(int x, int y, int w, int h, const fb_px *src, int stride);
void fb_frame_rect(int x, int y, int w, int h, fb_px c);
void fb_round_rect(int x, int y, int w, int h, int rad, fb_px c);
int  fb_text(int x, int y, const char *s, fb_px fg, long bg);
int  fb_text_w(const char *s);
/* unodevices (kernel exports; DEVMGR_ROW_N from uno_devmgr.h, kept in step) */
int  devmgr_list_str(char *buf, int cap);
int  devmgr_count(void);
int  devmgr_info(int idx, unsigned int *out, int nmax);
const char *devmgr_driver_name(int idx);
#define DEVMGR_ROW_N 15
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip(void);
void uno_seq_beep(int midi, int ticks);
void uno_seq_stop(void);
unsigned int TickCount(void);        /* Toolbox 60Hz tick counter */
int uno_pc64_keys_held(void);        /* UNO_KH_* bits (hid_kbd.h) */
/* key bindings + small app preferences (uno_binds.h) */
int  uno_bind_name(int action, char *buf, int cap);
int  uno_bind_set(int action, int keyid);
void uno_bind_reset(void);
int  uno_bind_keyid(int uni, int scan);
int  uno_pref_get(const char *name, char *buf, int cap);
int  uno_pref_set(const char *name, const char *value);
long uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
long uno_fs_size(int vol, const char *name);
long uno_fs_read_at(int vol, const char *name, long off, unsigned char *buf, long max);
int  uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);
int  uno_fs_mkdir(int vol, const char *path);
int  uno_fs_volumes(void);
int  uno_fs_list_begin(int vol);
int  uno_fs_list_get(int vol, int idx, char *name, int max);

/* ---- the current canvas rect (published by pyrt.c per draw) ---------------- */
static int gRX, gRY, gRW, gRH;
void uno_set_draw_rect(int x, int y, int w, int h)
{ gRX = x; gRY = y; gRW = w; gRH = h; fb_set_clip(x, y, w, h); }

/* ---- uno.rgb(r,g,b) -> packed 0xAABBGGRR ---------------------------------- */
static mp_obj_t m_rgb(mp_obj_t r, mp_obj_t g, mp_obj_t b) {
    unsigned rr = mp_obj_get_int(r) & 0xFF, gg = mp_obj_get_int(g) & 0xFF, bb = mp_obj_get_int(b) & 0xFF;
    return mp_obj_new_int_from_uint(0xFF000000u | (bb << 16) | (gg << 8) | rr);
}
static MP_DEFINE_CONST_FUN_OBJ_3(rgb_obj, m_rgb);

/* ---- the canvas object passed to draw(): coords are canvas-relative ------- */
typedef struct { mp_obj_base_t base; } canvas_obj_t;
extern const mp_obj_type_t canvas_type;
static canvas_obj_t gCanvasObj;

static fb_px arg_px(mp_obj_t o) { return (fb_px)mp_obj_get_int_truncated(o); }

static mp_obj_t cv_clear(mp_obj_t self, mp_obj_t col)
{ (void)self; fb_fill_rect(gRX, gRY, gRW, gRH, arg_px(col)); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_2(cv_clear_obj, cv_clear);

static mp_obj_t cv_fill_rect(size_t n, const mp_obj_t *a) {
    (void)n;
    fb_fill_rect(gRX + mp_obj_get_int(a[1]), gRY + mp_obj_get_int(a[2]),
                 mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), arg_px(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_fill_rect_obj, 6, 6, cv_fill_rect);

static mp_obj_t cv_rect(size_t n, const mp_obj_t *a) {
    (void)n;
    fb_frame_rect(gRX + mp_obj_get_int(a[1]), gRY + mp_obj_get_int(a[2]),
                  mp_obj_get_int(a[3]), mp_obj_get_int(a[4]), arg_px(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_rect_obj, 6, 6, cv_rect);

static mp_obj_t cv_pixel(size_t n, const mp_obj_t *a) {
    (void)n; fb_pixel(gRX + mp_obj_get_int(a[1]), gRY + mp_obj_get_int(a[2]), arg_px(a[3]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_pixel_obj, 4, 4, cv_pixel);

static mp_obj_t cv_hline(size_t n, const mp_obj_t *a) {
    (void)n; fb_hline(gRX + mp_obj_get_int(a[1]), gRY + mp_obj_get_int(a[2]), mp_obj_get_int(a[3]), arg_px(a[4]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_hline_obj, 5, 5, cv_hline);

static mp_obj_t cv_vline(size_t n, const mp_obj_t *a) {
    (void)n; fb_vline(gRX + mp_obj_get_int(a[1]), gRY + mp_obj_get_int(a[2]), mp_obj_get_int(a[3]), arg_px(a[4]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_vline_obj, 5, 5, cv_vline);

static mp_obj_t cv_text(size_t n, const mp_obj_t *a) {
    (void)n;
    const char *s = mp_obj_str_get_str(a[3]);
    fb_text(gRX + mp_obj_get_int(a[1]), gRY + mp_obj_get_int(a[2]), s, arg_px(a[4]), -1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_text_obj, 5, 5, cv_text);

/* Max pixels composed per fb_blit chunk.  The PYRT canvas height is capped at
 * ~380px (pyrt.c tr_build), so a 512-entry run (2KB stack) blits any column in
 * one pass; taller columns fall back to chunked blits, still one clip each. */
#define FB_WALLCOL_MAX 512

/* Textured wall column: the whole per-pixel inner loop in C so Python calls it
 * once per screen column, not once per pixel.  Samples an 8-bit texture (column-
 * major grid[texcol*th + v]), shades by a fixed-point factor (sh/256), looks up
 * the raw palette (768 bytes rgb) and writes pixels.  v0/dv are .8 fixed-point
 * texel coords.  Args: x, y0, count, grid, tw, th, texcol, v0fp, dvfp, pal, sh */
static mp_obj_t cv_wall_col(size_t n, const mp_obj_t *a) {
    (void)n;
    int x = gRX + mp_obj_get_int(a[1]);
    int y0 = gRY + mp_obj_get_int(a[2]);
    int count = mp_obj_get_int(a[3]);
    mp_buffer_info_t g, p;
    mp_get_buffer_raise(a[4], &g, MP_BUFFER_READ);
    int tw = mp_obj_get_int(a[5]), th = mp_obj_get_int(a[6]);
    int texcol = mp_obj_get_int(a[7]);
    long v = (long)mp_obj_get_int(a[8]), dv = (long)mp_obj_get_int(a[9]);
    mp_get_buffer_raise(a[10], &p, MP_BUFFER_READ);
    int sh = mp_obj_get_int(a[11]);
    const unsigned char *grid = (const unsigned char *)g.buf;
    const unsigned char *pal = (const unsigned char *)p.buf;
    if (sh > 256) sh = 256;                     /* >256 overflows the byte math */
    if (tw <= 0 || th <= 0 || count <= 0) return mp_const_none;
    texcol %= tw; if (texcol < 0) texcol += tw;
    const unsigned char *gcol = grid + texcol * th;      /* hoisted column base */
    /* Compose the column into a local run and blit it once instead of calling
     * fb_pixel per pixel.  fb_pixel re-derives the clip window and the row
     * address (y*FB_W+x) for every pixel; fb_blit clips the span a single time
     * and pointer-walks the framebuffer, so the per-pixel invariants are paid
     * once per column.  Output is byte-identical: fb_blit clips a 1-wide run
     * with the same clip_bounds() the per-pixel path used.  (A dedicated
     * fb_wall_column kernel primitive that also folds the shade/palette step
     * would be the further win, but that needs an fb.c export we can't add here.) */
    for (int base = 0; base < count; ) {
        int chunk = count - base;
        if (chunk > FB_WALLCOL_MAX) chunk = FB_WALLCOL_MAX;
        fb_px run[FB_WALLCOL_MAX];
        for (int i = 0; i < chunk; i++) {
            int vv = (int)((v >> 8) % th); if (vv < 0) vv += th;
            const unsigned char *c = pal + gcol[vv] * 3;
            unsigned rr = (c[0] * sh) >> 8, gg = (c[1] * sh) >> 8, bb = (c[2] * sh) >> 8;
            run[i] = 0xFF000000u | (bb << 16) | (gg << 8) | rr;
            v += dv;
        }
        fb_blit(x, y0 + base, 1, chunk, run, 1);
        base += chunk;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_wall_col_obj, 12, 12, cv_wall_col);

/* Textured wall SPAN: wall_col widened to wpx duplicate canvas columns, so
 * Duum makes ONE call per internal column instead of one per canvas pixel
 * column.  The texel run is composed once and blitted wpx times.
 * Args: x, w, y0, count, grid, tw, th, texcol, v0fp, dvfp, pal, sh */
static mp_obj_t cv_wall_span(size_t n, const mp_obj_t *a) {
    (void)n;
    int x = gRX + mp_obj_get_int(a[1]);
    int wpx = mp_obj_get_int(a[2]);
    int y0 = gRY + mp_obj_get_int(a[3]);
    int count = mp_obj_get_int(a[4]);
    mp_buffer_info_t g, p;
    mp_get_buffer_raise(a[5], &g, MP_BUFFER_READ);
    int tw = mp_obj_get_int(a[6]), th = mp_obj_get_int(a[7]);
    int texcol = mp_obj_get_int(a[8]);
    long v = (long)mp_obj_get_int(a[9]), dv = (long)mp_obj_get_int(a[10]);
    mp_get_buffer_raise(a[11], &p, MP_BUFFER_READ);
    int sh = mp_obj_get_int(a[12]);
    const unsigned char *grid = (const unsigned char *)g.buf;
    const unsigned char *pal = (const unsigned char *)p.buf;
    if (sh > 256) sh = 256;
    if (tw <= 0 || th <= 0 || count <= 0 || wpx <= 0) return mp_const_none;
    texcol %= tw; if (texcol < 0) texcol += tw;
    { const unsigned char *gcol = grid + texcol * th;
    for (int base = 0; base < count; ) {
        int chunk = count - base;
        if (chunk > FB_WALLCOL_MAX) chunk = FB_WALLCOL_MAX;
        fb_px run[FB_WALLCOL_MAX];
        for (int i = 0; i < chunk; i++) {
            int vv = (int)((v >> 8) % th); if (vv < 0) vv += th;
            const unsigned char *c = pal + gcol[vv] * 3;
            unsigned rr = (c[0] * sh) >> 8, gg = (c[1] * sh) >> 8, bb = (c[2] * sh) >> 8;
            run[i] = 0xFF000000u | (bb << 16) | (gg << 8) | rr;
            v += dv;
        }
        for (int k = 0; k < wpx; k++)
            fb_blit(x + k, y0 + base, 1, chunk, run, 1);
        base += chunk;
    } }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_wall_span_obj, 13, 13, cv_wall_span);

/* Masked SPAN (sprites, masked midtextures, HUD): like wall_span but texel
 * 0xFF is transparent and v does NOT wrap - out-of-range rows are skipped.
 * Opaque stretches are batched into runs and blitted.
 * Args: x, w, y0, count, grid, tw, th, texcol, v0fp, dvfp, pal, sh */
static mp_obj_t cv_mask_span(size_t n, const mp_obj_t *a) {
    (void)n;
    int x = gRX + mp_obj_get_int(a[1]);
    int wpx = mp_obj_get_int(a[2]);
    int y0 = gRY + mp_obj_get_int(a[3]);
    int count = mp_obj_get_int(a[4]);
    mp_buffer_info_t g, p;
    mp_get_buffer_raise(a[5], &g, MP_BUFFER_READ);
    int tw = mp_obj_get_int(a[6]), th = mp_obj_get_int(a[7]);
    int texcol = mp_obj_get_int(a[8]);
    long v = (long)mp_obj_get_int(a[9]), dv = (long)mp_obj_get_int(a[10]);
    mp_get_buffer_raise(a[11], &p, MP_BUFFER_READ);
    int sh = mp_obj_get_int(a[12]);
    const unsigned char *grid = (const unsigned char *)g.buf;
    const unsigned char *pal = (const unsigned char *)p.buf;
    if (sh > 256) sh = 256;
    if (tw <= 0 || th <= 0 || count <= 0 || wpx <= 0) return mp_const_none;
    texcol %= tw; if (texcol < 0) texcol += tw;
    { const unsigned char *gcol = grid + texcol * th;
    long vmax = (long)th << 8;
    fb_px run[FB_WALLCOL_MAX];
    int rlen = 0, rstart = 0;
    for (int i = 0; i < count; i++, v += dv) {
        int opaque = 0;
        unsigned char t = 0;
        if (v >= 0 && v < vmax) {
            t = gcol[v >> 8];
            opaque = (t != 0xFF);
        }
        if (opaque && rlen < FB_WALLCOL_MAX) {
            const unsigned char *c = pal + t * 3;
            unsigned rr = (c[0] * sh) >> 8, gg = (c[1] * sh) >> 8, bb = (c[2] * sh) >> 8;
            if (rlen == 0) rstart = i;
            run[rlen++] = 0xFF000000u | (bb << 16) | (gg << 8) | rr;
        } else if (rlen) {
            for (int k = 0; k < wpx; k++)
                fb_blit(x + k, y0 + rstart, 1, rlen, run, 1);
            rlen = 0;
            if (opaque) { i--; v -= dv; }      /* retry this row in a new run */
        }
    }
    if (rlen)
        for (int k = 0; k < wpx; k++)
            fb_blit(x + k, y0 + rstart, 1, rlen, run, 1);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_mask_span_obj, 13, 13, cv_mask_span);

/* Perspective flat SPAN (floors/ceilings): 64x64 world-aligned texture.
 * a = (plane_height - viewz) * vscale; per row dist = a / (ycen - y - 0.5),
 * world = (wx0,wy0) + dir * dist, texel (x & 63, -y & 63), shaded by sector
 * light * the walls' distance falloff.
 * Args: x, w, y0, count, grid, pal, a, ycen, dirx, diry, wx0, wy0, lf */
static mp_obj_t cv_flat_span(size_t n, const mp_obj_t *a) {
    (void)n;
    int x = gRX + mp_obj_get_int(a[1]);
    int wpx = mp_obj_get_int(a[2]);
    int y0i = mp_obj_get_int(a[3]);
    int y0 = gRY + y0i;
    int count = mp_obj_get_int(a[4]);
    mp_buffer_info_t g, p;
    mp_get_buffer_raise(a[5], &g, MP_BUFFER_READ);
    mp_get_buffer_raise(a[6], &p, MP_BUFFER_READ);
    float aa   = (float)mp_obj_get_float(a[7]);
    float ycen = (float)mp_obj_get_float(a[8]);
    float dirx = (float)mp_obj_get_float(a[9]);
    float diry = (float)mp_obj_get_float(a[10]);
    float wx0  = (float)mp_obj_get_float(a[11]);
    float wy0  = (float)mp_obj_get_float(a[12]);
    float lf   = (float)mp_obj_get_float(a[13]);
    const unsigned char *grid = (const unsigned char *)g.buf;
    const unsigned char *pal = (const unsigned char *)p.buf;
    if (count <= 0 || wpx <= 0 || g.len < 4096) return mp_const_none;
    for (int base = 0; base < count; ) {
        int chunk = count - base;
        if (chunk > FB_WALLCOL_MAX) chunk = FB_WALLCOL_MAX;
        fb_px run[FB_WALLCOL_MAX];
        for (int i = 0; i < chunk; i++) {
            int yy = y0i + base + i;
            float yd = ycen - ((float)yy + 0.5f);
            fb_px px = 0xFF000000u;
            if (yd != 0.0f) {
                float dist = aa / yd;
                float wx = wx0 + dirx * dist;
                float wy = wy0 + diry * dist;
                int ix = (int)wx; if (wx < (float)ix) ix--;
                int iy = (int)wy; if (wy < (float)iy) iy--;
                float df = 1200.0f / (dist + 650.0f);
                if (df > 1.0f) df = 1.0f; else if (df < 0.68f) df = 0.68f;
                int sh = (int)(lf * df * 256.0f);
                if (sh > 256) sh = 256;
                { const unsigned char *c =
                      pal + grid[(((-iy) & 63) << 6) | (ix & 63)] * 3;
                  unsigned rr = (c[0] * sh) >> 8, gg = (c[1] * sh) >> 8,
                           bb = (c[2] * sh) >> 8;
                  px = 0xFF000000u | (bb << 16) | (gg << 8) | rr; }
            }
            run[i] = px;
        }
        for (int k = 0; k < wpx; k++)
            fb_blit(x + k, y0 + base, 1, chunk, run, 1);
        base += chunk;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_flat_span_obj, 14, 14, cv_flat_span);


/* ===========================================================================
 * Duum: the per-column seg rasteriser.
 *
 * WHY THIS IS IN C. Duum's renderer is Python by design and stays that way -
 * the BSP walk, visibility, and every bit of game logic are Python. What was
 * never a design choice is that the INNERMOST loop was interpreted too: it
 * runs once per screen column per visible seg, a few thousand times a frame,
 * and under MICROPY_OBJ_REPR_A every float intermediate is a GC heap
 * allocation. The 2026-07-20 review proposed @micropython.viper for exactly
 * this loop; that route is dead - the native emitter crashes the guest, see
 * UNOAUTOMATE-REQUESTS.md - and this is the review's own stated fallback.
 *
 * CONTRACT: this emits the SAME display-list ops, in the same order, into the
 * same Python lists as the interpreted loop did. Nothing downstream changes:
 * draw() replays it unchanged, and tools/duum_verify.py still reads app.frame
 * to check the renderer against an independent model of the level. The Python
 * mirror in tools/duum_host.py is the readable reference implementation; keep
 * the two in step and tools/duum_golden.py will catch it if they drift.
 * ======================================================================== */
typedef struct {
    mp_obj_t out, ceilc, floorc, clips, masked;
    mp_obj_t colx, dcx, dcy, skyu;
    mp_obj_t sky_grid;                 /* MP_OBJ_NULL when the level has none */
    int sky_tw, sky_th, dvsky;
    int rw, ch;
    double hh, viewz, vsc, inv_vsc;
    int c_ceil[3], c_floor[3], fb_mid[3], fb_up[3], fb_lo[3];
    int valid;
} duum_ctx_t;
static duum_ctx_t gD;

static int d_geti(mp_obj_t seq, int i)
{ return mp_obj_get_int(mp_obj_subscr(seq, MP_OBJ_NEW_SMALL_INT(i), MP_OBJ_SENTINEL)); }
static void d_seti(mp_obj_t seq, int i, int v)
{ mp_obj_subscr(seq, MP_OBJ_NEW_SMALL_INT(i), MP_OBJ_NEW_SMALL_INT(v)); }
static void d_rgb3(mp_obj_t t, int *out3)
{ int i; for (i = 0; i < 3; i++) out3[i] = d_geti(t, i); }

/* DUUM.PY's rgb(): 0xFF000000 | b<<16 | g<<8 | r, each clamped at 255 */
static unsigned d_rgb(int r, int g, int b)
{
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return 0xFF000000u | ((unsigned)b << 16) | ((unsigned)g << 8) | (unsigned)r;
}
/* col_rgb(base, f); (int) truncates toward zero exactly like Python's int() */
static unsigned d_col_rgb(const int *base, double f)
{ return d_rgb((int)(base[0] * f), (int)(base[1] * f), (int)(base[2] * f)); }

static void d_push(mp_obj_t *items, size_t n)
{ mp_obj_list_append(gD.out, mp_obj_new_tuple(n, items)); }

/* ---- the emit helpers, faithful to the Python closures ------------------- */
static void d_rect(int cx, int y0, int y1, unsigned color)
{
    int x0, x1;
    if (y1 < y0) return;
    if (y0 < 0) y0 = 0;
    if (y1 > gD.ch - 1) y1 = gD.ch - 1;
    if (y1 < y0) return;
    x0 = d_geti(gD.colx, cx); x1 = d_geti(gD.colx, cx + 1);
    { mp_obj_t it[6] = { MP_ROM_QSTR(MP_QSTR_R), mp_obj_new_int(x0),
                         mp_obj_new_int(y0), mp_obj_new_int(x1 - x0),
                         mp_obj_new_int(y1 - y0 + 1),
                         mp_obj_new_int_from_uint(color) };
      d_push(it, 6); }
}

static void d_flatspan(int cx, int y0, int y1, mp_obj_t grid, double k, double lf)
{
    int x0, x1;
    if (y1 < y0) return;
    if (y0 < 0) y0 = 0;
    if (y1 > gD.ch - 1) y1 = gD.ch - 1;
    if (y1 < y0) return;
    x0 = d_geti(gD.colx, cx); x1 = d_geti(gD.colx, cx + 1);
    { mp_obj_t it[10] = { MP_ROM_QSTR(MP_QSTR_F), mp_obj_new_int(x0),
                          mp_obj_new_int(x1 - x0), mp_obj_new_int(y0),
                          mp_obj_new_int(y1 - y0 + 1), grid,
                          mp_obj_new_float(k),
                          mp_obj_subscr(gD.dcx, MP_OBJ_NEW_SMALL_INT(cx), MP_OBJ_SENTINEL),
                          mp_obj_subscr(gD.dcy, MP_OBJ_NEW_SMALL_INT(cx), MP_OBJ_SENTINEL),
                          mp_obj_new_float(lf) };
      d_push(it, 10); }
}

static void d_skyspan(int cx, int y0, int y1)
{
    int x0, x1;
    if (y1 < y0) return;
    if (y0 < 0) y0 = 0;
    if (y1 > gD.ch - 1) y1 = gD.ch - 1;
    if (y1 < y0) return;
    if (gD.sky_grid == MP_OBJ_NULL) {
        d_rect(cx, y0, y1, d_rgb(96, 120, 170));            /* C_SKY */
        return;
    }
    x0 = d_geti(gD.colx, cx); x1 = d_geti(gD.colx, cx + 1);
    { mp_obj_t it[12] = { MP_ROM_QSTR(MP_QSTR_W), mp_obj_new_int(x0),
                          mp_obj_new_int(x1 - x0), mp_obj_new_int(y0),
                          mp_obj_new_int(y1 - y0 + 1), gD.sky_grid,
                          mp_obj_new_int(gD.sky_tw), mp_obj_new_int(gD.sky_th),
                          mp_obj_subscr(gD.skyu, MP_OBJ_NEW_SMALL_INT(cx), MP_OBJ_SENTINEL),
                          mp_obj_new_int(y0 * gD.dvsky),
                          mp_obj_new_int(gD.dvsky), mp_obj_new_int(256) };
      d_push(it, 12); }
}

/* wall(): tx is (tw, th, grid) or None; fb is the untextured fallback colour */
static void d_wall(int cx, int ytop, int ybot, int ct, int cb, mp_obj_t tx,
                   double u, double v_top, double dv, int sh, const int *fb)
{
    int x0, x1;
    int y0 = ytop > ct ? ytop : ct;
    int y1 = ybot < cb ? ybot : cb;
    if (y0 < 0) y0 = 0;
    if (y1 > gD.ch - 1) y1 = gD.ch - 1;
    if (y1 < y0) return;
    x0 = d_geti(gD.colx, cx); x1 = d_geti(gD.colx, cx + 1);
    if (tx == mp_const_none) {
        mp_obj_t it[6] = { MP_ROM_QSTR(MP_QSTR_R), mp_obj_new_int(x0),
                           mp_obj_new_int(y0), mp_obj_new_int(x1 - x0),
                           mp_obj_new_int(y1 - y0 + 1),
                           mp_obj_new_int_from_uint(
                               d_rgb((fb[0] * sh) >> 8, (fb[1] * sh) >> 8,
                                     (fb[2] * sh) >> 8)) };
        d_push(it, 6);
        return;
    }
    { int tw = d_geti(tx, 0), th = d_geti(tx, 1);
      mp_obj_t grid = mp_obj_subscr(tx, MP_OBJ_NEW_SMALL_INT(2), MP_OBJ_SENTINEL);
      int v0 = (int)((v_top + (y0 - ytop) * dv) * 256.0);
      mp_obj_t it[12] = { MP_ROM_QSTR(MP_QSTR_W), mp_obj_new_int(x0),
                          mp_obj_new_int(x1 - x0), mp_obj_new_int(y0),
                          mp_obj_new_int(y1 - y0 + 1), grid,
                          mp_obj_new_int(tw), mp_obj_new_int(th),
                          mp_obj_new_int((int)u), mp_obj_new_int(v0),
                          mp_obj_new_int((int)(dv * 256.0)),
                          mp_obj_new_int(sh) };
      d_push(it, 12); }
}

/* cv.duum_frame(ctx) - per-frame state, set once instead of per seg.
 * ctx: (out, ceilc, floorc, clips, masked, colx, dcx, dcy, skyu,
 *       sky|None, dvsky, RW, ch, hh, viewz, vsc, inv_vsc,
 *       C_CEIL, C_FLOOR, fb_mid, fb_up, fb_lo) */
static mp_obj_t cv_duum_frame(mp_obj_t self, mp_obj_t ctx)
{
    (void)self;
    mp_obj_t *f;
    size_t n;
    mp_obj_get_array(ctx, &n, &f);
    if (n < 22) mp_raise_ValueError(MP_ERROR_TEXT("duum_frame: short ctx"));
    gD.out = f[0]; gD.ceilc = f[1]; gD.floorc = f[2]; gD.clips = f[3];
    gD.masked = f[4];
    gD.colx = f[5]; gD.dcx = f[6]; gD.dcy = f[7]; gD.skyu = f[8];
    if (f[9] == mp_const_none) {
        gD.sky_grid = MP_OBJ_NULL; gD.sky_tw = gD.sky_th = 0;
    } else {
        gD.sky_tw = d_geti(f[9], 0); gD.sky_th = d_geti(f[9], 1);
        gD.sky_grid = mp_obj_subscr(f[9], MP_OBJ_NEW_SMALL_INT(2), MP_OBJ_SENTINEL);
    }
    gD.dvsky = mp_obj_get_int(f[10]);
    gD.rw = mp_obj_get_int(f[11]);
    gD.ch = mp_obj_get_int(f[12]);
    gD.hh = mp_obj_get_float(f[13]);
    gD.viewz = mp_obj_get_float(f[14]);
    gD.vsc = mp_obj_get_float(f[15]);
    gD.inv_vsc = mp_obj_get_float(f[16]);
    d_rgb3(f[17], gD.c_ceil);  d_rgb3(f[18], gD.c_floor);
    d_rgb3(f[19], gD.fb_mid);  d_rgb3(f[20], gD.fb_up);  d_rgb3(f[21], gD.fb_lo);
    gD.valid = 1;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(cv_duum_frame_obj, cv_duum_frame);

/* cv.seg_cols(...) - the whole per-column loop for ONE seg.  Returns how many
 * columns this seg CLOSED, so the caller keeps its open-column count (the
 * BSP walk stops when every column is closed).
 * a[1..]: ix1 ix2 rx1 inv1 k_invd uz1 k_uz kfc kff kbc kbf sflags lf wk
 *         mid_t up_t lo_t v_mid v_up v_lo cgrid fgrid fc ff bc bf ldflags yoff
 * sflags: bit0 twosided, bit1 bothsky, bit2 csky */
static mp_obj_t cv_seg_cols(size_t n, const mp_obj_t *a)
{
    if (!gD.valid) mp_raise_ValueError(MP_ERROR_TEXT("seg_cols: no duum_frame"));
    (void)n;
    int i = 1;
    int ix1 = mp_obj_get_int(a[i++]), ix2 = mp_obj_get_int(a[i++]);
    double rx1 = mp_obj_get_float(a[i++]);
    double inv1 = mp_obj_get_float(a[i++]), k_invd = mp_obj_get_float(a[i++]);
    double uz1 = mp_obj_get_float(a[i++]), k_uz = mp_obj_get_float(a[i++]);
    double kfc = mp_obj_get_float(a[i++]), kff = mp_obj_get_float(a[i++]);
    double kbc = mp_obj_get_float(a[i++]), kbf = mp_obj_get_float(a[i++]);
    int sflags = mp_obj_get_int(a[i++]);
    double lf = mp_obj_get_float(a[i++]);
    int wk = mp_obj_get_int(a[i++]);
    mp_obj_t mid_t = a[i++], up_t = a[i++], lo_t = a[i++];
    double v_mid = mp_obj_get_float(a[i++]);
    double v_up = mp_obj_get_float(a[i++]), v_lo = mp_obj_get_float(a[i++]);
    mp_obj_t cgrid = a[i++], fgrid = a[i++];
    double fc = mp_obj_get_float(a[i++]), ff = mp_obj_get_float(a[i++]);
    double bc = mp_obj_get_float(a[i++]), bf = mp_obj_get_float(a[i++]);
    int ldflags = mp_obj_get_int(a[i++]);
    double yoff = mp_obj_get_float(a[i++]);

    const int twosided = sflags & 1, bothsky = (sflags >> 1) & 1,
              csky = (sflags >> 2) & 1;
    const double hh = gD.hh, viewz = gD.viewz, vsc = gD.vsc;
    int closed = 0;
    int th_m = 0, tw_m = 0;
    if (mid_t != mp_const_none) { tw_m = d_geti(mid_t, 0); th_m = d_geti(mid_t, 1); }

    for (int x = ix1; x <= ix2; x++) {
        int ct = d_geti(gD.ceilc, x), cb = d_geti(gD.floorc, x);
        if (ct > cb) continue;
        double xf = (double)x - rx1;
        double invd = inv1 + k_invd * xf;
        double dist = 1.0 / invd;
        double u = (uz1 + k_uz * xf) * dist;
        double df = 1200.0 / (dist + 650.0);
        if (df > 1.0) df = 1.0; else if (df < 0.68) df = 0.68;
        int sh = (int)(lf * df * 256.0);
        int shw = (sh * wk) / 100;          /* both non-negative, so / == // */
        if (shw > 256) shw = 256;
        double dv = dist * gD.inv_vsc;
        int yfc = (int)(hh - kfc * invd);
        int yff = (int)(hh - kff * invd);
        int ybc = 0, ybf = 0;
        if (twosided) {
            ybc = (int)(hh - kbc * invd);
            ybf = (int)(hh - kbf * invd);
            if (bothsky && ybc > yfc) yfc = ybc;
        }
        if (yfc > ct) {
            int yb = (yfc - 1) < cb ? (yfc - 1) : cb;
            if (csky)                       d_skyspan(x, ct, yb);
            else if (cgrid != mp_const_none) d_flatspan(x, ct, yb, cgrid, kfc, lf);
            else                            d_rect(x, ct, yb, d_col_rgb(gD.c_ceil, lf * 0.9));
        }
        if (yff < cb) {
            int ya = (yff + 1) > ct ? (yff + 1) : ct;
            if (fgrid != mp_const_none) d_flatspan(x, ya, cb, fgrid, kff, lf);
            else                        d_rect(x, ya, cb, d_col_rgb(gD.c_floor, lf * 0.9));
        }
        if (!twosided) {
            d_wall(x, yfc, yff, ct, cb, mid_t, u, v_mid, dv, shw, gD.fb_mid);
            d_seti(gD.ceilc, x, 1); d_seti(gD.floorc, x, 0);
            closed++;
            { mp_obj_t it[3] = { mp_obj_new_float(dist), MP_OBJ_NEW_SMALL_INT(1),
                                 MP_OBJ_NEW_SMALL_INT(0) };
              mp_obj_list_append(
                  mp_obj_subscr(gD.clips, MP_OBJ_NEW_SMALL_INT(x), MP_OBJ_SENTINEL),
                  mp_obj_new_tuple(3, it)); }
        } else {
            int newct = ct > yfc ? ct : yfc;
            int newcb = cb < yff ? cb : yff;
            if (mid_t != mp_const_none) {
                int yot = yfc > ybc ? yfc : ybc;
                int yob = yff < ybf ? yff : ybf;
                double zt = (ldflags & 16)                    /* ML_DONTPEGBOT */
                    ? ((ff > bf ? ff : bf) + (double)th_m)
                    : (fc < bc ? fc : bc);
                double ytt = hh - (zt - viewz) * vsc * invd;
                int my0 = (int)ytt;
                int my1 = (int)(ytt + (double)th_m / dv);
                if (my0 < yot) my0 = yot;
                if (my0 < ct)  my0 = ct;
                if (my0 < 0)   my0 = 0;
                if (my1 > yob) my1 = yob;
                if (my1 > cb)  my1 = cb;
                if (my1 > gD.ch - 1) my1 = gD.ch - 1;
                if (my1 >= my0) {
                    int x0m = d_geti(gD.colx, x), x1m = d_geti(gD.colx, x + 1);
                    int v0m = (int)((yoff + (my0 - ytt) * dv) * 256.0);
                    mp_obj_t op[12] = {
                        MP_ROM_QSTR(MP_QSTR_M), mp_obj_new_int(x0m),
                        mp_obj_new_int(x1m - x0m), mp_obj_new_int(my0),
                        mp_obj_new_int(my1 - my0 + 1),
                        mp_obj_subscr(mid_t, MP_OBJ_NEW_SMALL_INT(2), MP_OBJ_SENTINEL),
                        mp_obj_new_int(tw_m), mp_obj_new_int(th_m),
                        mp_obj_new_int((int)u), mp_obj_new_int(v0m),
                        mp_obj_new_int((int)(dv * 256.0)), mp_obj_new_int(shw) };
                    mp_obj_t pair[2] = { mp_obj_new_float(dist),
                                         mp_obj_new_tuple(12, op) };
                    mp_obj_list_append(gD.masked, mp_obj_new_tuple(2, pair));
                }
            }
            if (bc < fc && !bothsky) {
                if (up_t != mp_const_none)
                    d_wall(x, yfc, ybc - 1, ct, newcb, up_t, u, v_up, dv, shw, gD.fb_up);
                else {
                    int ya = yfc > ct ? yfc : ct;
                    int yb = (ybc - 1) < newcb ? (ybc - 1) : newcb;
                    d_rect(x, ya, yb, d_col_rgb(gD.c_ceil, lf * df));
                }
                if (ybc > newct) newct = ybc;
            }
            if (bf > ff) {
                if (lo_t != mp_const_none)
                    d_wall(x, ybf + 1, yff, newct, cb, lo_t, u, v_lo, dv, shw, gD.fb_lo);
                else {
                    int ya = (ybf + 1) > newct ? (ybf + 1) : newct;
                    int yb = yff < cb ? yff : cb;
                    d_rect(x, ya, yb, d_col_rgb(gD.c_floor, lf * df));
                }
                if (ybf < newcb) newcb = ybf;
            }
            d_seti(gD.ceilc, x, newct); d_seti(gD.floorc, x, newcb);
            if (newct > newcb) closed++;
            if (newct > ct || newcb < cb) {
                mp_obj_t it[3] = { mp_obj_new_float(dist),
                                   MP_OBJ_NEW_SMALL_INT(newct),
                                   MP_OBJ_NEW_SMALL_INT(newcb) };
                mp_obj_list_append(
                    mp_obj_subscr(gD.clips, MP_OBJ_NEW_SMALL_INT(x), MP_OBJ_SENTINEL),
                    mp_obj_new_tuple(3, it));
            }
        }
    }
    return mp_obj_new_int(closed);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(cv_seg_cols_obj, 29, 29, cv_seg_cols);

static mp_obj_t cv_width(mp_obj_t self)  { (void)self; return mp_obj_new_int(gRW); }
static MP_DEFINE_CONST_FUN_OBJ_1(cv_width_obj, cv_width);
static mp_obj_t cv_height(mp_obj_t self) { (void)self; return mp_obj_new_int(gRH); }
static MP_DEFINE_CONST_FUN_OBJ_1(cv_height_obj, cv_height);

static const mp_rom_map_elem_t canvas_locals[] = {
    { MP_ROM_QSTR(MP_QSTR_clear),     MP_ROM_PTR(&cv_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_rect), MP_ROM_PTR(&cv_fill_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect),      MP_ROM_PTR(&cv_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel),     MP_ROM_PTR(&cv_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_hline),     MP_ROM_PTR(&cv_hline_obj) },
    { MP_ROM_QSTR(MP_QSTR_vline),     MP_ROM_PTR(&cv_vline_obj) },
    { MP_ROM_QSTR(MP_QSTR_text),      MP_ROM_PTR(&cv_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_wall_col),  MP_ROM_PTR(&cv_wall_col_obj) },
    { MP_ROM_QSTR(MP_QSTR_width),     MP_ROM_PTR(&cv_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_height),    MP_ROM_PTR(&cv_height_obj) },
    { MP_ROM_QSTR(MP_QSTR_wall_span), MP_ROM_PTR(&cv_wall_span_obj) },
    { MP_ROM_QSTR(MP_QSTR_mask_span), MP_ROM_PTR(&cv_mask_span_obj) },
    { MP_ROM_QSTR(MP_QSTR_flat_span), MP_ROM_PTR(&cv_flat_span_obj) },
    { MP_ROM_QSTR(MP_QSTR_duum_frame),MP_ROM_PTR(&cv_duum_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_seg_cols),  MP_ROM_PTR(&cv_seg_cols_obj) },
};
static MP_DEFINE_CONST_DICT(canvas_locals_dict, canvas_locals);
MP_DEFINE_CONST_OBJ_TYPE(canvas_type, MP_QSTR_Canvas, MP_TYPE_FLAG_NONE,
    locals_dict, &canvas_locals_dict);

mp_obj_t uno_canvas_obj(void)
{ gCanvasObj.base.type = &canvas_type; return MP_OBJ_FROM_PTR(&gCanvasObj); }
void uno_win_set(void *w) { (void)w; }

/* ---- uno.sound / uno.fs (module-level) ------------------------------------ */
static mp_obj_t m_beep(mp_obj_t midi, mp_obj_t ticks)
{ uno_seq_beep(mp_obj_get_int(midi), mp_obj_get_int(ticks)); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_2(beep_obj, m_beep);
static mp_obj_t m_quiet(void) { uno_seq_stop(); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(quiet_obj, m_quiet);

/* uno.ticks() -> the 60Hz Toolbox tick counter (frame pacing / dt) */
static mp_obj_t m_ticks(void) { return mp_obj_new_int((mp_int_t)TickCount()); }
static MP_DEFINE_CONST_FUN_OBJ_0(ticks_obj, m_ticks);

/* uno.keys_down() -> currently-held navigation keys (UNO_KH_* bits).
 * 0 on the firmware input path, which has no key-up events. */
static mp_obj_t m_keys_down(void)
{ return mp_obj_new_int(uno_pc64_keys_held()); }
static MP_DEFINE_CONST_FUN_OBJ_0(keys_down_obj, m_keys_down);

/* ---- key bindings and app preferences -------------------------------------
 * The five OPTIONAL calls Duum's pause menu probes for with hasattr.  A port
 * without them still plays the whole game and the menu says the platform
 * cannot remap keys; with them, the Controls screen works and the FPS toggle
 * is remembered across boots.  See pc64/DUUM-UPSTREAM.md and uno_binds.h.
 *
 * Actions are named by their UNO_KH_* bit, which is also what keys_down()
 * answers in, so there is one numbering rather than two kept in step. */

/* uno.bind_name(action) -> "Left / A" */
static mp_obj_t m_bind_name(mp_obj_t a) {
    char buf[32];
    int n = uno_bind_name(mp_obj_get_int(a), buf, (int)sizeof buf);
    return mp_obj_new_str(buf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(bind_name_obj, m_bind_name);

/* uno.bind_set(action, uni, scan) -> bool
 *
 * Takes the key EVENT the app just received rather than a name, because the
 * app has the event and this side owns what to call it.  Returns False for a
 * key that cannot carry a binding, and for Use, which this machine reads as an
 * event rather than from the held bitmap - storing that would be storing
 * something that does nothing. */
static mp_obj_t m_bind_set(mp_obj_t a, mp_obj_t u, mp_obj_t sc) {
    int keyid = uno_bind_keyid(mp_obj_get_int(u), mp_obj_get_int(sc));
    return mp_obj_new_bool(uno_bind_set(mp_obj_get_int(a), keyid));
}
static MP_DEFINE_CONST_FUN_OBJ_3(bind_set_obj, m_bind_set);

/* uno.bind_reset() -> every binding back to the shipped default */
static mp_obj_t m_bind_reset(void) { uno_bind_reset(); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(bind_reset_obj, m_bind_reset);

/* uno.pref_get(name) -> str, or None when there is no such preference.
 * None rather than "" so that an app can tell "never set" from "set empty"
 * and fall back to its own default. */
static mp_obj_t m_pref_get(mp_obj_t nm) {
    char buf[32];
    int n = uno_pref_get(mp_obj_str_get_str(nm), buf, (int)sizeof buf);
    if (n <= 0) return mp_const_none;
    return mp_obj_new_str(buf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(pref_get_obj, m_pref_get);

/* uno.pref_set(name, value) -> bool */
static mp_obj_t m_pref_set(mp_obj_t nm, mp_obj_t v) {
    return mp_obj_new_bool(uno_pref_set(mp_obj_str_get_str(nm),
                                        mp_obj_str_get_str(v)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(pref_set_obj, m_pref_set);

/* read a whole file (small): uno.read(vol, name) -> bytes; vol default 0 */
static mp_obj_t m_read(size_t n, const mp_obj_t *a) {
    int vol = n > 1 ? mp_obj_get_int(a[0]) : 0;
    const char *name = mp_obj_str_get_str(a[n - 1]);
    long sz = uno_fs_size(vol, name);
    if (sz < 0) return mp_const_none;
    { vstr_t v; vstr_init_len(&v, sz);
      long got = uno_fs_read(vol, name, (unsigned char *)v.buf, sz);
      if (got < 0) got = 0; v.len = got;
      return mp_obj_new_bytes_from_vstr(&v); }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(read_obj, 1, 2, m_read);

/* uno.read_at(vol, name, off, n) -> bytes (the WAD/streaming door) */
static mp_obj_t m_read_at(size_t n, const mp_obj_t *a) {
    (void)n;
    int vol = mp_obj_get_int(a[0]);
    const char *name = mp_obj_str_get_str(a[1]);
    long off = mp_obj_get_int(a[2]); long cnt = mp_obj_get_int(a[3]);
    vstr_t v; vstr_init_len(&v, cnt);
    long got = uno_fs_read_at(vol, name, off, (unsigned char *)v.buf, cnt);
    if (got < 0) got = 0; v.len = got;
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(read_at_obj, 4, 4, m_read_at);

static mp_obj_t m_fsize(size_t n, const mp_obj_t *a) {
    int vol = n > 1 ? mp_obj_get_int(a[0]) : 0;
    return mp_obj_new_int(uno_fs_size(vol, mp_obj_str_get_str(a[n - 1])));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(fsize_obj, 1, 2, m_fsize);

static mp_obj_t m_write(size_t n, const mp_obj_t *a) {
    int vol = n > 2 ? mp_obj_get_int(a[0]) : 0;
    const char *name = mp_obj_str_get_str(a[n - 2]);
    mp_buffer_info_t bi; mp_get_buffer_raise(a[n - 1], &bi, MP_BUFFER_READ);
    return mp_obj_new_bool(uno_fs_write(vol, name, bi.buf, bi.len));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(write_obj, 2, 3, m_write);

/* uno.mkdir(vol, path) -> bool  (create one dir; its parent must already exist) */
static mp_obj_t m_mkdir(size_t n, const mp_obj_t *a) {
    int vol = n > 1 ? mp_obj_get_int(a[0]) : 0;
    return mp_obj_new_bool(uno_fs_mkdir(vol, mp_obj_str_get_str(a[n - 1])));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mkdir_obj, 1, 2, m_mkdir);

/* ---- uno.App base class (empty; the app subclasses it) -------------------- */
static mp_obj_t app_make_new(const mp_obj_type_t *type, size_t n, size_t nkw, const mp_obj_t *args) {
    (void)n; (void)nkw; (void)args;
    mp_obj_t o = mp_obj_malloc(mp_obj_base_t, type);
    return o;
}
MP_DEFINE_CONST_OBJ_TYPE(uno_app_type, MP_QSTR_App, MP_TYPE_FLAG_NONE, make_new, app_make_new);

/* ---- bind the module-global `app` (called by pyrt.c) ---------------------- */
static mp_obj_t opt_method(mp_obj_t app, qstr name) {
    nlr_buf_t nlr; mp_obj_t r = MP_OBJ_NULL;
    if (nlr_push(&nlr) == 0) { r = mp_load_attr(app, name); nlr_pop(); }
    else { r = MP_OBJ_NULL; }                 /* AttributeError -> absent */
    return r;
}
/* PyApp mirror (must match pyrt.c's struct layout) */
typedef struct { mp_obj_t app, build, draw, action, key, tick, opened, closed; const char *name; } PyApp;
/* The app instance and its bound methods live only in the C-side PyApp struct
 * (pyrt.c) and the module globals - neither is scanned by the GC, so a
 * gc_collect() would free them and leave the trampolines calling dangling
 * objects.  Pin them in a registered root-pointer array so the GC keeps them
 * alive for the app's whole lifetime. */
MP_REGISTER_ROOT_POINTER(mp_obj_t uno_app_roots[8]);

void uno_clear_app_roots(void) {
    for (int i = 0; i < 8; i++) MP_STATE_VM(uno_app_roots)[i] = MP_OBJ_NULL;
}

int uno_bind_app(PyApp *pa) {
    mp_obj_t g = MP_OBJ_FROM_PTR(mp_globals_get());
    mp_map_elem_t *e = mp_map_lookup(mp_obj_dict_get_map(g),
                                     MP_ROM_QSTR(MP_QSTR_app), MP_MAP_LOOKUP);
    if (!e || e->value == MP_OBJ_NULL) return 0;
    pa->app    = e->value;
    pa->build  = opt_method(pa->app, MP_QSTR_build);
    pa->draw   = opt_method(pa->app, MP_QSTR_draw);
    pa->action = opt_method(pa->app, MP_QSTR_action);
    pa->key    = opt_method(pa->app, MP_QSTR_key);
    pa->tick   = opt_method(pa->app, MP_QSTR_tick);
    pa->opened = opt_method(pa->app, MP_QSTR_opened);
    pa->closed = opt_method(pa->app, MP_QSTR_closed);
    /* keep every cached callable reachable across GC cycles */
    mp_obj_t *root = MP_STATE_VM(uno_app_roots);
    root[0] = pa->app;    root[1] = pa->build;  root[2] = pa->draw;
    root[3] = pa->action; root[4] = pa->key;    root[5] = pa->tick;
    root[6] = pa->opened; root[7] = pa->closed;
    return 1;
}

/* ---- the `uno` module dict ------------------------------------------------ */
/* uno.devices() -> str: the whole machine, one line per PCI function,
 * "bb:dd.f ven:dev cc/ss class driver|UNCLAIMED" (see DEVICES.md). */
static char g_devbuf[8192];
static mp_obj_t m_devices(void) {
    int n = devmgr_list_str(g_devbuf, sizeof g_devbuf);
    if (n < 0) n = 0;
    return mp_obj_new_str(g_devbuf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_0(devices_obj, m_devices);

/* uno.pci() -> [(loc, ven, dev, cls, subcls, progif, driver_or_None), ...]
 * The parsed form of the same registry, for scripts that want to filter
 * rather than read.  loc is "bb:dd.f".  Rows come from devmgr_info(), a flat
 * unsigned vector, because this module is built separately from the kernel
 * and resolves its symbols by name - handing it a uno_device* would pin the
 * struct layout across that boundary. */
static mp_obj_t m_pci(void) {
    mp_obj_t list = mp_obj_new_list(0, 0);
    int i, n = devmgr_count();
    for (i = 0; i < n; i++) {
        unsigned r[DEVMGR_ROW_N];
        const char *drv;
        char loc[10];
        static const char H[] = "0123456789abcdef";
        mp_obj_t t[7];
        if (devmgr_info(i, r, DEVMGR_ROW_N) < 0) continue;
        loc[0] = H[(r[0] >> 4) & 0xF]; loc[1] = H[r[0] & 0xF]; loc[2] = ':';
        loc[3] = H[(r[1] >> 4) & 0xF]; loc[4] = H[r[1] & 0xF]; loc[5] = '.';
        loc[6] = (char)('0' + (r[2] & 7));           loc[7] = 0;
        drv = devmgr_driver_name(i);
        t[0] = mp_obj_new_str(loc, 7);
        t[1] = mp_obj_new_int(r[3]);                 /* vendor   */
        t[2] = mp_obj_new_int(r[4]);                 /* device   */
        t[3] = mp_obj_new_int(r[5]);                 /* class    */
        t[4] = mp_obj_new_int(r[6]);                 /* subclass */
        t[5] = mp_obj_new_int(r[7]);                 /* prog-if  */
        t[6] = drv ? mp_obj_new_str(drv, strlen(drv)) : mp_const_none;
        mp_obj_list_append(list, mp_obj_new_tuple(7, t));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(pci_obj, m_pci);

/* ---- uno.log* : the system log (see pc64/UNOLOG.md) -----------------------
 * Scripting the log is worth having on its own - a script that notices
 * something and records it belongs in the same record as the kernel's own
 * lines - and it is what lets the gate assert on the log from outside. */
void unolog(int sev, int fac, const char *fmt, ...);
int  unolog_flush(void);
int  unolog_level(void);
void unolog_set_level(int sev);
void unolog_set_remote_level(int sev);
int  unolog_set_remote(const char *host, int port);
int  unolog_set_listen(int on);
unsigned long unolog_next(void);
unsigned long unolog_dropped(void);
unsigned long unolog_sent(void);
unsigned long unolog_received(void);

static mp_obj_t m_log(size_t n, const mp_obj_t *a)
{   /* uno.log(sev, fac, text) */
    unolog(mp_obj_get_int(a[0]), mp_obj_get_int(a[1]), "%s",
           mp_obj_str_get_str(a[2]));
    (void)n; return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(log_obj, 3, 3, m_log);

static mp_obj_t m_log_flush(void) { return mp_obj_new_int(unolog_flush()); }
static MP_DEFINE_CONST_FUN_OBJ_0(log_flush_obj, m_log_flush);

static mp_obj_t m_log_level(size_t n, const mp_obj_t *a)
{   if (n) unolog_set_level(mp_obj_get_int(a[0]));
    return mp_obj_new_int(unolog_level()); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(log_level_obj, 0, 1, m_log_level);

static mp_obj_t m_log_remote_level(mp_obj_t s)
{ unolog_set_remote_level(mp_obj_get_int(s)); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(log_remote_level_obj, m_log_remote_level);

static mp_obj_t m_log_remote(size_t n, const mp_obj_t *a)
{   int port = (n > 1) ? mp_obj_get_int(a[1]) : 0;
    return mp_obj_new_int(unolog_set_remote(mp_obj_str_get_str(a[0]), port)); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(log_remote_obj, 1, 2, m_log_remote);

static mp_obj_t m_log_listen(mp_obj_t on)
{ return mp_obj_new_int(unolog_set_listen(mp_obj_get_int(on) != 0)); }
static MP_DEFINE_CONST_FUN_OBJ_1(log_listen_obj, m_log_listen);

/* The ring, for a viewer. Sequence numbers rather than indices: the ring
 * wraps, so an index is only meaningful until it is overwritten, and a viewer
 * that scrolled while the machine logged would show the wrong lines. */
unsigned long unolog_first(void);
typedef struct {
    unsigned long seq; int sev, fac; unsigned long ms; long long wall;
    char text[192]; char src[16];
} unolog_rec_py;                    /* mirrors unolog.h's unolog_rec */
int unolog_get(unsigned long seq, unolog_rec_py *out);

/* uno.run_app(vol, path) - open a .UNO app, the way Files does when you
 * double-click one. No new capability: the `py` verb that reaches this is
 * already KERNEL-tier because it is arbitrary code execution. */
int pc64_shell_run_user(int vol, const char *path);
static mp_obj_t m_run_app(mp_obj_t vol, mp_obj_t path)
{ return mp_obj_new_int(pc64_shell_run_user(mp_obj_get_int(vol),
                                            mp_obj_str_get_str(path))); }
static MP_DEFINE_CONST_FUN_OBJ_2(run_app_obj, m_run_app);

int unolog_save_cfg(void);
static mp_obj_t m_log_save(void) { return mp_obj_new_int(unolog_save_cfg()); }
static MP_DEFINE_CONST_FUN_OBJ_0(log_save_obj, m_log_save);

static mp_obj_t m_log_span(void)
{   mp_obj_t t[2];
    t[0] = mp_obj_new_int((mp_int_t)unolog_first());
    t[1] = mp_obj_new_int((mp_int_t)unolog_next());
    return mp_obj_new_tuple(2, t); }
static MP_DEFINE_CONST_FUN_OBJ_0(log_span_obj, m_log_span);

static mp_obj_t m_log_read(mp_obj_t seq)
{   unolog_rec_py r;
    mp_obj_t t[6];
    if (!unolog_get((unsigned long)mp_obj_get_int(seq), &r)) return mp_const_none;
    t[0] = mp_obj_new_int(r.sev);
    t[1] = mp_obj_new_int(r.fac);
    t[2] = mp_obj_new_int((mp_int_t)r.ms);
    t[3] = mp_obj_new_int((mp_int_t)r.wall);
    t[4] = mp_obj_new_str(r.text, strlen(r.text));
    t[5] = mp_obj_new_str(r.src, strlen(r.src));
    return mp_obj_new_tuple(6, t); }
static MP_DEFINE_CONST_FUN_OBJ_1(log_read_obj, m_log_read);

static mp_obj_t m_log_stat(void)
{   mp_obj_t t[4];
    t[0] = mp_obj_new_int((mp_int_t)unolog_next());
    t[1] = mp_obj_new_int((mp_int_t)unolog_dropped());
    t[2] = mp_obj_new_int((mp_int_t)unolog_sent());
    t[3] = mp_obj_new_int((mp_int_t)unolog_received());
    return mp_obj_new_tuple(4, t); }
static MP_DEFINE_CONST_FUN_OBJ_0(log_stat_obj, m_log_stat);

static const mp_rom_map_elem_t uno_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_uno) },
    { MP_ROM_QSTR(MP_QSTR_rgb),      MP_ROM_PTR(&rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_App),      MP_ROM_PTR(&uno_app_type) },
    { MP_ROM_QSTR(MP_QSTR_beep),     MP_ROM_PTR(&beep_obj) },
    { MP_ROM_QSTR(MP_QSTR_quiet),    MP_ROM_PTR(&quiet_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),     MP_ROM_PTR(&read_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_at),  MP_ROM_PTR(&read_at_obj) },
    { MP_ROM_QSTR(MP_QSTR_size),     MP_ROM_PTR(&fsize_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),    MP_ROM_PTR(&write_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir),    MP_ROM_PTR(&mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_devices),  MP_ROM_PTR(&devices_obj) },
    { MP_ROM_QSTR(MP_QSTR_pci),      MP_ROM_PTR(&pci_obj) },
    { MP_ROM_QSTR(MP_QSTR_log),          MP_ROM_PTR(&log_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_flush),    MP_ROM_PTR(&log_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_level),    MP_ROM_PTR(&log_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_remote_level), MP_ROM_PTR(&log_remote_level_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_remote),   MP_ROM_PTR(&log_remote_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_listen),   MP_ROM_PTR(&log_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_stat),     MP_ROM_PTR(&log_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_span),     MP_ROM_PTR(&log_span_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_read),     MP_ROM_PTR(&log_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_save),     MP_ROM_PTR(&log_save_obj) },
    { MP_ROM_QSTR(MP_QSTR_run_app),      MP_ROM_PTR(&run_app_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks),        MP_ROM_PTR(&ticks_obj) },
    { MP_ROM_QSTR(MP_QSTR_keys_down),    MP_ROM_PTR(&keys_down_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind_name),    MP_ROM_PTR(&bind_name_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind_set),     MP_ROM_PTR(&bind_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind_reset),   MP_ROM_PTR(&bind_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_pref_get),     MP_ROM_PTR(&pref_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_pref_set),     MP_ROM_PTR(&pref_set_obj) },
};
static MP_DEFINE_CONST_DICT(uno_globals, uno_globals_table);
const mp_obj_module_t mp_module_uno = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&uno_globals,
};
MP_REGISTER_MODULE(MP_QSTR_uno, mp_module_uno);
