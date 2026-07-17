/* pc64_font - TrueType text for unoui (see pc64_font.h). Wraps stb_truetype
 * with a freestanding malloc arena + math shims, an ASCII glyph cache, and an
 * fb text provider. Grayscale + subpixel (LCD) anti-aliasing. */
#include "pc64_font.h"
#include "fb.h"
#include "pc64_fs.h"
#include <string.h>
#include <math.h>

/* ---- freestanding shims for stb_truetype -------------------------------- */
static double f_floor(double x){ return (double)floorf((float)x); }
static double f_ceil (double x){ return (double)ceilf((float)x); }
static double f_sqrt (double x){ return (double)sqrtf((float)x); }
static double f_fabs (double x){ return x < 0 ? -x : x; }
static double f_cos  (double x){ return (double)cosf((float)x); }
static double f_fmod (double x, double y){ if (y == 0) return 0;
    { long long i = (long long)(x / y); return x - (double)i * y; } }
/* pow + acos are only reached by stb's SDF path (unused here) - approximate. */
static double f_ln(double x){ int e = 0; double t, t2, s, term; int k;
    if (x <= 0) return 0;
    while (x > 1.5) { x /= 2; e++; } while (x < 0.75) { x *= 2; e--; }
    t = (x - 1) / (x + 1); t2 = t * t; s = t; term = t;
    for (k = 3; k < 15; k += 2) { term *= t2; s += term / k; }
    return 2 * s + e * 0.6931471805599453; }
static double f_exp(double x){ double s = 1, term = 1; int k;
    for (k = 1; k < 20; k++) { term *= x / k; s += term; } return s; }
static double f_pow(double x, double y){ double ax = f_fabs(x), r;
    if (ax == 0) return 0; r = f_exp(y * f_ln(ax)); return x < 0 ? -r : r; }
static double f_acos(double x){ double neg, xx, ret;
    if (x < -1) x = -1; if (x > 1) x = 1; neg = x < 0 ? 1 : 0; xx = f_fabs(x);
    ret = -0.0187293 * xx + 0.0742610; ret = ret * xx - 0.2121144;
    ret = ret * xx + 1.5707288; ret = ret * f_sqrt(1.0 - xx);
    return neg ? 3.14159265358979 - ret : ret; }

/* stb malloc arena: reset before each glyph raster; stb frees within a call. */
#define FA_SZ (512 * 1024)
static unsigned char fa[FA_SZ];
static long fa_used;
static void  fa_reset(void){ fa_used = 0; }
static void *fa_alloc(long n){ n = (n + 15) & ~15L;
    if (fa_used + n > FA_SZ) return 0;
    { void *p = &fa[fa_used]; fa_used += n; return p; } }

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_ifloor(x)    ((int) f_floor(x))
#define STBTT_iceil(x)     ((int) f_ceil(x))
#define STBTT_sqrt(x)      f_sqrt(x)
#define STBTT_pow(x,y)     f_pow(x,y)
#define STBTT_fmod(x,y)    f_fmod(x,y)
#define STBTT_cos(x)       f_cos(x)
#define STBTT_acos(x)      f_acos(x)
#define STBTT_fabs(x)      f_fabs(x)
#define STBTT_malloc(s,u)  ((void)(u), fa_alloc((long)(s)))
#define STBTT_free(p,u)    ((void)(p),(void)(u))
#define STBTT_assert(x)    ((void)0)
#define STBTT_strlen(x)    strlen(x)
#define STBTT_memcpy       memcpy
#define STBTT_memset       memset
#include "stb_truetype.h"

/* ---- font slots ---------------------------------------------------------- */
#define NSLOT 3
#define FONT_BUF (1200 * 1024)
static unsigned char g_data[NSLOT][FONT_BUF];
typedef struct { int loaded; stbtt_fontinfo info; long len; const char *name, *file; } fslot;
static fslot g_fonts[NSLOT] = {
    { 0, {0}, 0, "Sans",   "SANS.TTF"   },
    { 0, {0}, 0, "Mono",   "MONO.TTF"   },
    { 0, {0}, 0, "Ubuntu", "UBUNTU.TTF" },
};

static int font_read(const char *name, unsigned char *buf, long max)
{
    int nv = uno_fs_volumes(), v;
    for (v = 0; v < nv; v++) { long n = uno_fs_read(v, name, buf, max); if (n > 0) return (int)n; }
    return 0;
}
static int ensure_loaded(int slot)
{
    fslot *f;
    if (slot < 0 || slot >= NSLOT) return 0;
    f = &g_fonts[slot];
    if (f->loaded) return 1;
    { long n = font_read(f->file, g_data[slot], FONT_BUF);
      if (n <= 0) return 0;
      f->len = n;
      if (!stbtt_InitFont(&f->info, g_data[slot], stbtt_GetFontOffsetForIndex(g_data[slot], 0)))
          return 0;
      f->loaded = 1; return 1; }
}

/* ---- glyph cache (ASCII) ------------------------------------------------- */
#define GC_FIRST 32
#define GC_COUNT 95                    /* 32..126 */
#define GMAXW 40
#define GMAXH 40
typedef struct {
    int ready, w, h, ox, oy, adv;      /* grayscale bbox + advance (px) */
    int ox3, cw3;                      /* subpixel: 3x box left + 3x width */
    unsigned char cov[GMAXW * 3 * GMAXH];
} gcache;
static gcache g_cache[GC_COUNT];

static int g_active = -1, g_px = 13, g_sub = 1;
static int g_baseline, g_cellh, g_space;

static void invalidate(void){ int i; for (i = 0; i < GC_COUNT; i++) g_cache[i].ready = 0; }

static void set_metrics(void)
{
    fslot *f = &g_fonts[g_active];
    float scale = stbtt_ScaleForPixelHeight(&f->info, (float)g_px);
    int asc, desc, gap, a, l;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    g_baseline = (int)(asc * scale + 0.5f);
    g_cellh    = (int)((asc - desc) * scale + 0.5f) + 1;
    stbtt_GetCodepointHMetrics(&f->info, ' ', &a, &l);
    g_space = (int)(a * scale + 0.5f);
    invalidate();
}

/* Render one glyph's coverage into the cache. In subpixel mode `cov` holds a
 * 3x-horizontal strip (cw3 x h) and ox3 is its left edge in 1/3-px units; in
 * grayscale mode `cov` holds a w x h coverage map with bbox (ox,oy). */
static void render_glyph(int cp)
{
    gcache *gc = &g_cache[cp - GC_FIRST];
    fslot *f = &g_fonts[g_active];
    float scale = stbtt_ScaleForPixelHeight(&f->info, (float)g_px);
    int adv, lsb, x0, y0, x1, y1, w, h;
    stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
    gc->adv = (int)(adv * scale + 0.5f);
    stbtt_GetCodepointBitmapBox(&f->info, cp, scale, scale, &x0, &y0, &x1, &y1);
    w = x1 - x0; h = y1 - y0;
    if (w < 0) w = 0; if (h < 0) h = 0; if (w > GMAXW) w = GMAXW; if (h > GMAXH) h = GMAXH;
    gc->w = w; gc->h = h; gc->ox = x0; gc->oy = y0;
    if (g_sub) {
        int bx0, by0, bx1, by1, cw3;
        stbtt_GetCodepointBitmapBox(&f->info, cp, scale * 3, scale, &bx0, &by0, &bx1, &by1);
        cw3 = bx1 - bx0; if (cw3 < 0) cw3 = 0; if (cw3 > GMAXW * 3) cw3 = GMAXW * 3;
        gc->ox3 = bx0; gc->cw3 = cw3; gc->oy = by0;
        fa_reset();
        if (cw3 && h) stbtt_MakeCodepointBitmap(&f->info, gc->cov, cw3, h, cw3, scale * 3, scale, cp);
    } else {
        fa_reset();
        if (w && h) stbtt_MakeCodepointBitmap(&f->info, gc->cov, w, h, w, scale, scale, cp);
    }
    gc->ready = 1;
}

/* ---- compositing --------------------------------------------------------- */
static int draw_glyph(int x, int y, int cp, fb_px fg, long bg)
{
    gcache *gc;
    if (g_active < 0) return x + 8;                 /* shouldn't happen */
    if (cp < GC_FIRST || cp >= GC_FIRST + GC_COUNT) {
        if (bg >= 0) fb_fill_rect(x, y, g_space, g_cellh, (fb_px)bg);
        return x + g_space;
    }
    gc = &g_cache[cp - GC_FIRST];
    if (!gc->ready) render_glyph(cp);
    if (bg >= 0) fb_fill_rect(x, y, gc->adv > 0 ? gc->adv : g_space, g_cellh, (fb_px)bg);
    if (g_sub) {
        int r, X, cw3 = gc->cw3, pixox = gc->ox3 / 3, base = 3 * pixox - gc->ox3;
        int wout = (cw3 - base) / 3 + 2;
        for (r = 0; r < gc->h; r++) {
            const unsigned char *row = gc->cov + r * cw3;
            int Y = y + g_baseline + gc->oy + r;
            for (X = 0; X < wout; X++) {
                int j = base + 3 * X;                /* subpixel index of this pixel's R */
                int a = (j-1 >= 0 && j-1 < cw3) ? row[j-1] : 0;
                int b = (j   >= 0 && j   < cw3) ? row[j]   : 0;
                int c = (j+1 >= 0 && j+1 < cw3) ? row[j+1] : 0;
                int d = (j+2 >= 0 && j+2 < cw3) ? row[j+2] : 0;
                int e = (j+3 >= 0 && j+3 < cw3) ? row[j+3] : 0;
                int cR = (a + 2*b + c) / 4, cG = (b + 2*c + d) / 4, cB = (c + 2*d + e) / 4;
                if (cR | cG | cB) fb_blend_pixel_sub(x + pixox + X, Y, fg, cR, cG, cB);
            }
        }
    } else {
        int r, c;
        for (r = 0; r < gc->h; r++) {
            const unsigned char *row = gc->cov + r * gc->w;
            int Y = y + g_baseline + gc->oy + r;
            for (c = 0; c < gc->w; c++) {
                int cov = row[c];
                if (cov) fb_blend_pixel_sub(x + gc->ox + c, Y, fg, cov, cov, cov);
            }
        }
    }
    return x + gc->adv;
}

static int adv_of(int cp)
{
    if (cp < GC_FIRST || cp >= GC_FIRST + GC_COUNT) return g_space;
    { gcache *gc = &g_cache[cp - GC_FIRST]; if (!gc->ready) render_glyph(cp); return gc->adv; }
}

/* ---- fb text provider ---------------------------------------------------- */
static int prov_glyph(int x, int y, int cp, fb_px fg, long bg) { return draw_glyph(x, y, cp, fg, bg); }
static int prov_text_w(const char *s) { int w = 0; for (; *s; s++) w += adv_of((unsigned char)*s); return w; }
static int prov_height(void) { return g_cellh; }
static const fb_font g_provider = { prov_glyph, prov_text_w, prov_height };

/* ---- public API ---------------------------------------------------------- */
int uno_font_count(void) { return NSLOT; }
const char *uno_font_name(int slot) { return (slot >= 0 && slot < NSLOT) ? g_fonts[slot].name : "System"; }
int uno_font_active(void) { return g_active; }
int uno_font_subpixel(void) { return g_sub; }

int uno_font_use(int slot)
{
    if (slot < 0) { g_active = -1; fb_set_font(0); return -1; }
    if (!ensure_loaded(slot)) { g_active = -1; fb_set_font(0); return -1; }
    g_active = slot; set_metrics(); fb_set_font(&g_provider);
    return slot;
}
void uno_font_set_px(int px) { if (px < 8) px = 8; if (px > 32) px = 32; g_px = px; if (g_active >= 0) set_metrics(); }
void uno_font_set_subpixel(int on) { g_sub = on ? 1 : 0; invalidate(); }

/* push/pop the active font (depth-1 stack) - lets the toolkit render one window
 * (e.g. the Editor's document) in a different face than the system font. */
static int g_saved = -2; static const fb_font *g_savedf;
void uno_font_push(int slot)
{
    g_saved = g_active; g_savedf = fb_get_font();
    if (slot < -1) return;                       /* -2 = inherit: no change */
    if (slot < 0) { g_active = -1; fb_set_font(0); return; }
    if (!ensure_loaded(slot)) { g_active = -1; fb_set_font(0); return; }
    g_active = slot; set_metrics(); fb_set_font(&g_provider);
}
void uno_font_pop(void)
{
    g_active = g_saved; if (g_active >= 0) set_metrics();
    fb_set_font(g_savedf); g_saved = -2;
}

/* Draw with a specific slot regardless of the system font (Editor doc font).
 * Swaps the active slot around the call, restoring it after. */
int uno_font_draw_with(int slot, int x, int y, const char *s, fb_px fg, long bg)
{
    int save = g_active; const fb_font *savef = fb_get_font();
    if (slot < 0) { fb_set_font(0);
        { int rx = fb_text(x, y, s, fg, bg); fb_set_font(savef); return rx; } }
    if (!ensure_loaded(slot)) return fb_text(x, y, s, fg, bg);
    g_active = slot; set_metrics(); fb_set_font(&g_provider);
    { int rx = fb_text(x, y, s, fg, bg);
      g_active = save; if (save >= 0) set_metrics(); fb_set_font(savef); return rx; }
}
int uno_font_text_w_with(int slot, const char *s)
{
    int save = g_active, w;
    if (slot < 0) { const char *p = s; int n = 0; while (*p++) n++; return n * 8; }
    if (!ensure_loaded(slot)) { const char *p = s; int n = 0; while (*p++) n++; return n * 8; }
    g_active = slot; set_metrics(); w = prov_text_w(s);
    g_active = save; if (save >= 0) set_metrics(); return w;
}
int uno_font_height_with(int slot)
{
    int save = g_active, h;
    if (slot < 0) return 8;
    if (!ensure_loaded(slot)) return 8;
    g_active = slot; set_metrics(); h = g_cellh;
    g_active = save; if (save >= 0) set_metrics(); return h;
}
