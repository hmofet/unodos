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
int  devmgr_list_str(char *buf, int cap);  /* unodevices (kernel export) */
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip(void);
void uno_seq_beep(int midi, int ticks);
void uno_seq_stop(void);
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
/* uno.devices() -> str: one line per PCI function "bb:dd.f ven:dev cc/ss class" (see DEVICES.md) */
static char g_devbuf[4096];
static mp_obj_t m_devices(void) {
    int n = devmgr_list_str(g_devbuf, sizeof g_devbuf);
    return mp_obj_new_str(g_devbuf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_0(devices_obj, m_devices);

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
};
static MP_DEFINE_CONST_DICT(uno_globals, uno_globals_table);
const mp_obj_module_t mp_module_uno = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&uno_globals,
};
MP_REGISTER_MODULE(MP_QSTR_uno, mp_module_uno);
