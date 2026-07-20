/* ===========================================================================
 * Photos - the UnoDOS pc64 image viewer.  A unoui-CLASS .UNO module
 * (APPS\PHOTOS.UNO), and the first consumer of unomedia, the shared media
 * foundation: every pixel it shows comes through unomedia's from-scratch
 * decoders (PNG incl. its own inflate, baseline JPEG, animated GIF, BMP,
 * TGA, PNM, QOI, ICO - see unomedia/README.md).  The audio decoders are
 * slated to migrate into unomedia next; this app is the pattern.
 *
 * Shape: one canvas fills the window (the Studio pattern) - a file pane on
 * the left (volumes + FAT subdirectories, filtered to what unomedia can
 * open), a toolbar, a status bar, and the picture area.  Fit-to-window by
 * default; wheel zooms about the pointer's centre, dragging pans, arrows
 * step through the folder's images, GIF animations play through the frame()
 * hook.  A file unomedia RECOGNISES but declines (progressive JPEG, WebP)
 * shows its precise reason in the status bar - never a generic failure.
 *
 * Memory: the decoded image (w*h RGBA) and a viewport-sized scaled cache
 * come from the kernel heap; the cache is rebuilt on zoom/pan/frame change,
 * pre-composited (checkerboard under alpha) so the per-repaint cost is one
 * clipped fb_blit.  Downscale is a box average (photos stay smooth at fit),
 * upscale is nearest (pixels stay honest at 400%).
 * ======================================================================== */
/* -I. = pc64, -I../unoui, -I../unomedia supply these */
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "uno_uuiapp.h"
#include "fat.h"
#include "unomedia.h"

#ifdef UNO_APP_SYM
#  define uno_app_main UNO_APP_SYM
#endif

/* ---- kernel imports (all resolved against kExports[]) --------------------- */
void  fb_fill_rect(int x, int y, int w, int h, fb_px c);
void  fb_hline(int x, int y, int w, fb_px c);
void  fb_vline(int x, int y, int h, fb_px c);
void  fb_blit(int x, int y, int w, int h, const fb_px *src, int stride);
int   fb_text(int x, int y, const char *s, fb_px fg, long bg);
int   fb_text_w(const char *s);
int   fb_text_h(void);
int   fb_width(void);
int   fb_height(void);

int   uno_fs_volumes(void);
const char *uno_fs_volume_name(int vol);
int   uno_fs_list_begin(int vol);
int   uno_fs_list_get(int vol, int idx, char *name, int max);
long  uno_fs_read_at(int vol, const char *name, long off,
                     unsigned char *buf, long max);
long  uno_fs_size(int vol, const char *name);
int   uno_fs_kind(int vol);
int   uno_fs_fat_index(int vol);

void  pc64_shell_dirty(void);
const struct unoui_theme *pc64_shell_theme(void);
long  TickCount(void);

void *malloc(unsigned long n);
void  free(void *p);
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
unsigned long strlen(const char *s);
char *strcpy(char *d, const char *s);
int   strcmp(const char *a, const char *b);

/* ---- geometry ------------------------------------------------------------- */
#define PH_MAXE   192               /* directory entries shown                 */
#define PANE_W    150               /* file pane width                         */
#define ZOOM_MIN  10                /* percent                                 */
#define ZOOM_MAX  800

typedef struct { int x, y, w, h; } R;
static R L_tool, L_pane, L_img, L_status;

static unoui_window *g_win;

/* ---- browser state --------------------------------------------------------- */
static uno_fat_entry ph_e[PH_MAXE];
static int  ph_n, ph_sel, ph_scroll;
static int  ph_vol;                 /* fs volume index                         */
static char ph_path[120];           /* subdir within a FAT volume ("" = root)  */
static int  ph_pane = 1;            /* file pane visible                       */

/* ---- the open image -------------------------------------------------------- */
static char  ph_file[140];          /* full path of the open file              */
static char  ph_name[16];           /* leaf name (status bar)                  */
static int   ph_ok;                 /* an image is open and decoded            */
static um_image_info ph_info;
static um_px *ph_img;               /* w*h decoded frame (persistent for GIF)  */
static char  ph_err[64];            /* last failure reason for the status bar  */

/* animation */
static int   ph_anim, ph_play, ph_frame_no;
static long  ph_due;                /* TickCount deadline for the next frame   */

/* ---- the view -------------------------------------------------------------- */
static int   ph_fit = 1;            /* 1 = fit to window (never upscales)      */
static int   ph_zoom = 100;         /* percent, when !ph_fit                   */
static int   ph_panx, ph_pany;      /* viewport origin in scaled-image px      */
static um_px *ph_cache;             /* viewport-sized composited pixels        */
static int   ph_cw, ph_ch;          /* cache dimensions (== L_img.w/h)         */
static int   ph_cache_ok;
static int   *ph_map;               /* per-column source-x map (ph_cw ints)    */

/* drag state */
static int   ph_drag, ph_dragx, ph_dragy;

/* ---- helpers --------------------------------------------------------------- */
static const struct unoui_theme *TH(void) { return pc64_shell_theme(); }

static int in_rect(R r, int x, int y)
{ return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static char *pcat(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *pcat_int(char *p, long v)
{
    char t[14]; int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

static int ph_is_fat(void) { return uno_fs_kind(ph_vol) == 1; }

static void layout(unoui_rect c)
{
    int th = fb_text_h();
    int toolh = th + 10, stath = th + 6;
    int pw = ph_pane ? PANE_W : 0;
    if (pw > c.w / 2) pw = c.w / 2;
    L_tool   = (R){ c.x, c.y, c.w, toolh };
    L_status = (R){ c.x, c.y + c.h - stath, c.w, stath };
    L_pane   = (R){ c.x, c.y + toolh, pw, c.h - toolh - stath };
    L_img    = (R){ c.x + pw, c.y + toolh, c.w - pw, c.h - toolh - stath };
}

/* ---- directory listing ------------------------------------------------------ */
static void ph_relist(void)
{
    int i, k = 0;
    ph_n = 0;
    if (ph_is_fat()) {
        int fv = uno_fs_fat_index(ph_vol);
        static uno_fat_entry tmp[PH_MAXE];
        int n = uno_fat_list_ex(fv, ph_path[0] ? ph_path : 0, tmp, PH_MAXE);
        if (n > PH_MAXE) n = PH_MAXE;
        for (i = 0; i < n && k < PH_MAXE; i++) {
            if (!tmp[i].is_dir && !um_image_is(tmp[i].name)) continue;
            ph_e[k++] = tmp[i];
        }
    } else {
        int n = uno_fs_list_begin(ph_vol);
        for (i = 0; i < n && k < PH_MAXE; i++) {
            char nm[16];
            if (!uno_fs_list_get(ph_vol, i, nm, sizeof nm)) continue;
            if (!um_image_is(nm)) continue;
            memset(&ph_e[k], 0, sizeof ph_e[k]);
            memcpy(ph_e[k].name, nm, 12); ph_e[k].name[12] = 0;
            k++;
        }
        ph_path[0] = 0;
    }
    ph_n = k;
    if (ph_sel >= ph_n) ph_sel = ph_n - 1;
    if (ph_sel < 0) ph_sel = 0;
    if (ph_scroll > ph_sel) ph_scroll = ph_sel;
}

static void ph_join(const char *name, char *out, int max)
{
    char *p = out, *end = out + max - 1;
    const char *s = ph_path;
    while (*s && p < end) *p++ = *s++;
    if (p != out && p < end) *p++ = '\\';
    s = name; while (*s && p < end) *p++ = *s++;
    *p = 0;
}

/* ---- the byte source unomedia reads through -------------------------------- */
static long ph_src_read(void *ctx, long off, unsigned char *dst, long n)
{
    (void)ctx;
    if (ph_is_fat())
        return uno_fat_read_at(uno_fs_fat_index(ph_vol), ph_file, off, dst, n);
    return uno_fs_read_at(ph_vol, ph_file, off, dst, n);
}

/* ---- opening / closing an image -------------------------------------------- */
static void ph_view_reset(void)
{
    ph_fit = 1; ph_panx = ph_pany = 0; ph_cache_ok = 0;
}

static void ph_close_img(void)
{
    um_image_close();
    if (ph_img)   { free(ph_img);   ph_img = 0; }
    if (ph_cache) { free(ph_cache); ph_cache = 0; ph_cw = ph_ch = 0; }
    if (ph_map)   { free(ph_map);   ph_map = 0; }
    ph_ok = 0; ph_anim = 0; ph_play = 0; ph_frame_no = 0;
    ph_err[0] = 0;
}

static void ph_fail(const char *why)
{
    char *p = ph_err, *end = ph_err + sizeof ph_err - 1;
    while (*why && p < end) *p++ = *why++;
    *p = 0;
    ph_ok = 0;
}

static void ph_open_entry(int idx)
{
    um_src src;
    int delay = 0;
    long sz;

    if (idx < 0 || idx >= ph_n || ph_e[idx].is_dir) return;
    ph_close_img();

    if (ph_is_fat()) ph_join(ph_e[idx].name, ph_file, sizeof ph_file);
    else             strcpy(ph_file, ph_e[idx].name);
    { char *p = ph_name; const char *s = ph_e[idx].name; int i = 0;
      while (*s && i++ < 14) *p++ = *s++;
      *p = 0; }

    sz = ph_is_fat() ? uno_fat_size(uno_fs_fat_index(ph_vol), ph_file)
                     : uno_fs_size(ph_vol, ph_file);
    if (sz <= 0) { ph_fail("cannot read file"); return; }

    src.read = ph_src_read; src.size = sz; src.ctx = 0;
    if (!um_image_open(&src, ph_file, &ph_info)) { ph_fail(um_error()); return; }

    ph_img = (um_px *)malloc((unsigned long)ph_info.w * ph_info.h * 4);
    if (!ph_img) { um_image_close(); ph_fail("out of memory"); return; }
    memset(ph_img, 0, (unsigned long)ph_info.w * ph_info.h * 4);

    if (um_image_frame(ph_img, &delay) != 1) {
        ph_fail(um_error()[0] ? um_error() : "decode failed");
        um_image_close();
        free(ph_img); ph_img = 0;
        return;
    }
    ph_ok = 1;
    ph_frame_no = 1;
    ph_anim = (ph_info.frames != 1);
    ph_play = ph_anim;
    if (ph_anim) {
        int ms = delay > 0 ? delay : 100;
        ph_due = TickCount() + (ms * 60 + 999) / 1000;
    }
    ph_view_reset();
}

/* step to the previous/next image file in the listing (skipping directories) */
static void ph_step(int dir)
{
    int i = ph_sel, n = 0;
    if (!ph_n) return;
    do { i += dir; if (i < 0) i = ph_n - 1; if (i >= ph_n) i = 0; n++; }
    while (ph_e[i].is_dir && n <= ph_n);
    if (ph_e[i].is_dir) return;
    ph_sel = i;
    ph_open_entry(i);
}

/* ---- the scaled view cache -------------------------------------------------- *
 * Maps the viewport (L_img.w x L_img.h) onto the scaled image, composites
 * alpha over a checkerboard, box-averages on downscale, nearest on upscale.
 * All later repaints of an unchanged view are a single fb_blit of this. */
static fb_px ph_checker(int vx, int vy)
{
    return (((vx >> 4) + (vy >> 4)) & 1) ? FB_RGB(0x68, 0x68, 0x68)
                                         : FB_RGB(0x8E, 0x8E, 0x8E);
}

/* current scale in 16.16 (view px per image px). All the scale math below
 * runs in long long: this is an LLP64 target (long is 32-bit) and a scaled
 * coordinate << 16 does not fit in 32 bits. */
static long long ph_scale16(void)
{
    long long s;
    if (!ph_ok) return 65536;
    if (ph_fit) {
        long long sx = ((long long)L_img.w << 16) / ph_info.w;
        long long sy = ((long long)L_img.h << 16) / ph_info.h;
        s = sx < sy ? sx : sy;
        if (s > 65536) s = 65536;          /* fit never upscales */
        if (s < 1) s = 1;
    } else
        s = ((long long)ph_zoom << 16) / 100;
    return s;
}

static int ph_fitpct(void) { return (int)((ph_scale16() * 100) >> 16); }

static void ph_build_view(void)
{
    long long s = ph_scale16();
    int  iw = (int)(((long long)ph_info.w * s) >> 16);
    int  ih = (int)(((long long)ph_info.h * s) >> 16);
    int  vw = L_img.w, vh = L_img.h;
    int  vx, vy;

    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    if (vw < 1 || vh < 1) return;

    if (!ph_cache || ph_cw != vw || ph_ch != vh) {
        if (ph_cache) free(ph_cache);
        if (ph_map)   free(ph_map);
        ph_cache = (um_px *)malloc((unsigned long)vw * vh * 4);
        ph_map   = (int *)malloc((unsigned long)vw * sizeof(int) * 2);
        ph_cw = vw; ph_ch = vh;
        if (!ph_cache || !ph_map) { ph_cache_ok = 0; return; }
    }

    /* clamp the pan; centre when the scaled image is smaller than the view */
    if (iw <= vw) ph_panx = -(vw - iw) / 2;
    else { if (ph_panx < 0) ph_panx = 0; if (ph_panx > iw - vw) ph_panx = iw - vw; }
    if (ih <= vh) ph_pany = -(vh - ih) / 2;
    else { if (ph_pany < 0) ph_pany = 0; if (ph_pany > ih - vh) ph_pany = ih - vh; }

    /* per-column source range [x0,x1) in image px (box filter on downscale) */
    for (vx = 0; vx < vw; vx++) {
        int sx = ph_panx + vx;
        if (sx < 0 || sx >= iw) { ph_map[vx * 2] = -1; continue; }
        {
            int x0 = (int)(((long long)sx << 16) / s);
            int x1 = (int)((((long long)(sx + 1) << 16) + s - 1) / s);
            if (x0 >= ph_info.w) x0 = ph_info.w - 1;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > ph_info.w) x1 = ph_info.w;
            ph_map[vx * 2] = x0; ph_map[vx * 2 + 1] = x1;
        }
    }

    for (vy = 0; vy < vh; vy++) {
        um_px *dst = ph_cache + (long)vy * vw;
        int  sy = ph_pany + vy;
        int  y0, y1;
        if (sy < 0 || sy >= ih) {
            for (vx = 0; vx < vw; vx++) dst[vx] = TH()->pal.dark;
            continue;
        }
        y0 = (int)(((long long)sy << 16) / s);
        y1 = (int)((((long long)(sy + 1) << 16) + s - 1) / s);
        if (y0 >= ph_info.h) y0 = ph_info.h - 1;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > ph_info.h) y1 = ph_info.h;

        for (vx = 0; vx < vw; vx++) {
            int x0 = ph_map[vx * 2], x1;
            unsigned r = 0, g = 0, b = 0, a = 0, cnt;
            if (x0 < 0) { dst[vx] = TH()->pal.dark; continue; }
            x1 = ph_map[vx * 2 + 1];
            cnt = (unsigned)((x1 - x0) * (y1 - y0));
            if (cnt == 1) {
                um_px p = ph_img[(long)y0 * ph_info.w + x0];
                r = p & 0xFF; g = (p >> 8) & 0xFF;
                b = (p >> 16) & 0xFF; a = (p >> 24) & 0xFF;
            } else {
                int xx, yy;
                for (yy = y0; yy < y1; yy++) {
                    const um_px *row = ph_img + (long)yy * ph_info.w;
                    for (xx = x0; xx < x1; xx++) {
                        um_px p = row[xx];
                        r += p & 0xFF; g += (p >> 8) & 0xFF;
                        b += (p >> 16) & 0xFF; a += (p >> 24) & 0xFF;
                    }
                }
                r /= cnt; g /= cnt; b /= cnt; a /= cnt;
            }
            if (a == 255)
                dst[vx] = FB_RGB(r, g, b);
            else {
                fb_px ck = ph_checker(vx, vy);
                unsigned cr = ck & 0xFF, cg = (ck >> 8) & 0xFF,
                         cb = (ck >> 16) & 0xFF;
                dst[vx] = FB_RGB((r * a + cr * (255 - a)) / 255,
                                 (g * a + cg * (255 - a)) / 255,
                                 (b * a + cb * (255 - a)) / 255);
            }
        }
    }
    ph_cache_ok = 1;
}

/* ---- zoom controls ---------------------------------------------------------- */
static void ph_zoom_to(int pct, int cx, int cy)   /* keep (cx,cy) view-stable */
{
    long long s0 = ph_scale16(), s1, ix, iy;
    if (pct < ZOOM_MIN) pct = ZOOM_MIN;
    if (pct > ZOOM_MAX) pct = ZOOM_MAX;
    /* image-space point under the anchor, in 16.16: view px -> image px is
     * a divide by the 16.16 scale, so pre-shift by 32 to keep 16 fractional
     * bits through it (all in 64-bit; worst case ~2^49). */
    ix = (((long long)ph_panx + cx) << 32) / (s0 ? s0 : 1);
    iy = (((long long)ph_pany + cy) << 32) / (s0 ? s0 : 1);
    ph_fit = 0; ph_zoom = pct;
    s1 = ph_scale16();
    ph_panx = (int)((ix * s1) >> 32) - cx;
    ph_pany = (int)((iy * s1) >> 32) - cy;
    ph_cache_ok = 0;
    pc64_shell_dirty();
}

static void ph_zoom_step(int dir, int cx, int cy)
{
    int pct = ph_fit ? ph_fitpct() : ph_zoom;
    pct = dir > 0 ? pct * 5 / 4 + 1 : pct * 4 / 5;
    ph_zoom_to(pct, cx, cy);
}

/* ---- toolbar ---------------------------------------------------------------- */
typedef struct { R r; const char *label; int id; } PhBtn;
enum { BT_PREV, BT_NEXT, BT_ZOUT, BT_FIT, BT_100, BT_ZIN, BT_PLAY, BT_PANE,
       BT_N };
static PhBtn ph_btn[BT_N];

static void tool_layout(void)
{
    static const char *lab[BT_N] =
        { "<", ">", "-", "Fit", "1:1", "+", "Play", "Files" };
    int i, x = L_tool.x + 4, th = fb_text_h();
    for (i = 0; i < BT_N; i++) {
        int w = fb_text_w(lab[i]) + 14;
        ph_btn[i].label = lab[i]; ph_btn[i].id = i;
        ph_btn[i].r = (R){ x, L_tool.y + 3, w, th + 4 };
        x += w + 4;
        if (i == BT_NEXT || i == BT_ZIN) x += 8;   /* group gaps */
    }
}

static void tool_draw(void)
{
    const unoui_palette *p = &TH()->pal;
    int i;
    fb_fill_rect(L_tool.x, L_tool.y, L_tool.w, L_tool.h, p->face);
    fb_hline(L_tool.x, L_tool.y + L_tool.h - 1, L_tool.w, p->shadow);
    tool_layout();
    for (i = 0; i < BT_N; i++) {
        R r = ph_btn[i].r;
        const char *lab = ph_btn[i].label;
        int on = (i == BT_PLAY && ph_play) || (i == BT_PANE && ph_pane);
        if (i == BT_PLAY && !ph_anim) continue;      /* stills: no Play */
        if (i == BT_PLAY && ph_play) lab = "Pause";
        fb_fill_rect(r.x, r.y, r.w, r.h, on ? p->accent : p->face);
        fb_hline(r.x, r.y, r.w, p->light);
        fb_vline(r.x, r.y, r.h, p->light);
        fb_hline(r.x, r.y + r.h - 1, r.w, p->shadow);
        fb_vline(r.x + r.w - 1, r.y, r.h, p->shadow);
        fb_text(r.x + 7, r.y + 2, lab,
                on ? p->accent_text : p->face_text, -1);
    }
}

/* ---- file pane -------------------------------------------------------------- */
static int pane_rows(void) { int rh = fb_text_h() + 3; return (L_pane.h - fb_text_h() - 8) / (rh ? rh : 1); }

static void pane_draw(void)
{
    const unoui_palette *p = &TH()->pal;
    int th = fb_text_h(), rh = th + 3, rows = pane_rows();
    int i, y;
    char hdr[24];

    if (!ph_pane || L_pane.w <= 0) return;
    fb_fill_rect(L_pane.x, L_pane.y, L_pane.w, L_pane.h, p->field_bg);
    fb_vline(L_pane.x + L_pane.w - 1, L_pane.y, L_pane.h, p->shadow);

    /* header: the volume cycler  "< ESP >" */
    { char *q = hdr;
      q = pcat(q, "< ");
      { const char *vn = uno_fs_volume_name(ph_vol); int i2 = 0;
        while (*vn && i2++ < 10) *q++ = *vn++; }
      q = pcat(q, " >"); *q = 0; }
    fb_fill_rect(L_pane.x, L_pane.y, L_pane.w - 1, th + 6, p->face);
    fb_text(L_pane.x + 6, L_pane.y + 3, hdr, p->face_text, -1);
    fb_hline(L_pane.x, L_pane.y + th + 5, L_pane.w - 1, p->shadow);

    if (ph_scroll > ph_sel) ph_scroll = ph_sel;
    if (ph_sel >= ph_scroll + rows) ph_scroll = ph_sel - rows + 1;
    if (ph_scroll < 0) ph_scroll = 0;

    y = L_pane.y + th + 8;
    for (i = ph_scroll; i < ph_n && i < ph_scroll + rows; i++) {
        char row[20]; char *q = row;
        if (ph_e[i].is_dir) q = pcat(q, "[");
        { const char *s = ph_e[i].name; int j = 0;
          while (*s && j++ < 13) *q++ = *s++; }
        if (ph_e[i].is_dir) q = pcat(q, "]");
        *q = 0;
        if (i == ph_sel) {
            fb_fill_rect(L_pane.x, y - 1, L_pane.w - 1, rh, p->accent);
            fb_text(L_pane.x + 6, y, row, p->accent_text, -1);
        } else
            fb_text(L_pane.x + 6, y, row, p->field_text, -1);
        y += rh;
    }
    if (!ph_n)
        fb_text(L_pane.x + 6, L_pane.y + th + 10, "(no images)",
                p->text_dim, -1);
}

/* ---- status bar ------------------------------------------------------------- */
static void status_draw(void)
{
    const unoui_palette *p = &TH()->pal;
    char s[96]; char *q = s;
    fb_fill_rect(L_status.x, L_status.y, L_status.w, L_status.h, p->face);
    fb_hline(L_status.x, L_status.y, L_status.w, p->light);

    if (ph_err[0]) {
        q = pcat(q, ph_name[0] ? ph_name : "file");
        q = pcat(q, ": ");
        { const char *e = ph_err; while (*e && q < s + 90) *q++ = *e++; }
        *q = 0;
    } else if (ph_ok) {
        q = pcat(q, ph_name);
        q = pcat(q, "  ");
        q = pcat_int(q, ph_info.w); q = pcat(q, "x");
        q = pcat_int(q, ph_info.h);
        q = pcat(q, " "); q = pcat(q, ph_info.format);
        q = pcat(q, "  ");
        q = pcat_int(q, ph_fit ? ph_fitpct() : ph_zoom);
        q = pcat(q, "%");
        if (ph_fit) q = pcat(q, " (fit)");
        if (ph_anim) {
            q = pcat(q, "  frame ");
            q = pcat_int(q, ph_frame_no);
            if (ph_info.frames > 1) {
                q = pcat(q, "/");
                q = pcat_int(q, ph_info.frames);
            }
        }
        *q = 0;
    } else
        strcpy(s, "Open an image from the file pane");
    fb_text(L_status.x + 6, L_status.y + 3, s, p->face_text, -1);
}

/* ---- canvas draw ------------------------------------------------------------ */
static void ph_draw(unoui_widget *w, unoui_rect c, void *ctx)
{
    const unoui_palette *p = &TH()->pal;
    (void)w; (void)ctx;
    layout(c);
    tool_draw();
    pane_draw();
    status_draw();

    if (!ph_ok) {
        fb_fill_rect(L_img.x, L_img.y, L_img.w, L_img.h, p->dark);
        if (ph_err[0])
            fb_text(L_img.x + (L_img.w - fb_text_w("cannot show this file")) / 2,
                    L_img.y + L_img.h / 2, "cannot show this file",
                    p->text_dim, -1);
        return;
    }
    if (!ph_cache_ok) ph_build_view();
    if (ph_cache_ok)
        fb_blit(L_img.x, L_img.y, ph_cw, ph_ch, ph_cache, ph_cw);
    else
        fb_fill_rect(L_img.x, L_img.y, L_img.w, L_img.h, p->dark);
}

/* ---- input ------------------------------------------------------------------ */
static void ph_activate(int idx)
{
    if (idx < 0 || idx >= ph_n) return;
    ph_sel = idx;
    if (ph_e[idx].is_dir) {
        /* enter the directory ("..": the FAT lister skips dot entries, so
           going UP is the pane header's job - see pane_click) */
        char np[120]; char *q = np; const char *s = ph_path;
        while (*s) *q++ = *s++;
        if (q != np) *q++ = '\\';
        s = ph_e[idx].name; while (*s) *q++ = *s++;
        *q = 0;
        strcpy(ph_path, np);
        ph_sel = ph_scroll = 0;
        ph_relist();
    } else
        ph_open_entry(idx);
    pc64_shell_dirty();
}

static void pane_click(int x, int y)
{
    int th = fb_text_h(), rh = th + 3;
    if (y < L_pane.y + th + 6) {
        /* volume cycler header: left half = previous, right half = next;
           any click also pops back out of a subdirectory first */
        if (ph_path[0]) {
            char *q = ph_path + strlen(ph_path);
            while (q > ph_path && q[-1] != '\\') q--;
            if (q > ph_path) q[-1] = 0; else ph_path[0] = 0;
        } else {
            int nv = uno_fs_volumes();
            ph_vol += (x >= L_pane.x + L_pane.w / 2) ? 1 : -1;
            if (ph_vol < 0) ph_vol = nv - 1;
            if (ph_vol >= nv) ph_vol = 0;
        }
        ph_sel = ph_scroll = 0;
        ph_relist();
        pc64_shell_dirty();
        return;
    }
    { int idx = ph_scroll + (y - (L_pane.y + th + 8)) / (rh ? rh : 1);
      if (idx >= 0 && idx < ph_n) ph_activate(idx); }
}

static int ph_event(unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    (void)w; (void)ctx;

    switch (e->kind) {
    case UI_EV_MOUSE_DOWN:
        { int i;
          tool_layout();
          for (i = 0; i < BT_N; i++)
              if (in_rect(ph_btn[i].r, e->x, e->y)) {
                  switch (i) {
                  case BT_PREV: ph_step(-1); break;
                  case BT_NEXT: ph_step(1); break;
                  case BT_ZOUT: ph_zoom_step(-1, L_img.w/2, L_img.h/2); break;
                  case BT_ZIN:  ph_zoom_step(1,  L_img.w/2, L_img.h/2); break;
                  case BT_FIT:  ph_fit = 1; ph_cache_ok = 0; break;
                  case BT_100:  ph_zoom_to(100, L_img.w/2, L_img.h/2); break;
                  case BT_PLAY: if (ph_anim) { ph_play = !ph_play;
                                    ph_due = TickCount(); } break;
                  case BT_PANE: ph_pane = !ph_pane; ph_cache_ok = 0; break;
                  }
                  pc64_shell_dirty();
                  return 1;
              }
        }
        if (ph_pane && in_rect(L_pane, e->x, e->y)) { pane_click(e->x, e->y); return 1; }
        if (in_rect(L_img, e->x, e->y) && ph_ok) {
            ph_drag = 1; ph_dragx = e->x; ph_dragy = e->y;
            return 1;
        }
        return 1;
    case UI_EV_MOUSE_MOVE:
        if (ph_drag && ph_ok) {
            ph_panx -= e->x - ph_dragx;
            ph_pany -= e->y - ph_dragy;
            ph_dragx = e->x; ph_dragy = e->y;
            ph_cache_ok = 0;
            pc64_shell_dirty();
        }
        return 1;
    case UI_EV_MOUSE_UP:
        ph_drag = 0;
        return 1;
    case UI_EV_WHEEL:
        if (ph_pane && in_rect(L_pane, e->x, e->y)) {
            ph_scroll += e->wheel * 3;
            if (ph_scroll > ph_n - 1) ph_scroll = ph_n - 1;
            if (ph_scroll < 0) ph_scroll = 0;
            pc64_shell_dirty();
        } else if (ph_ok && in_rect(L_img, e->x, e->y))
            ph_zoom_step(e->wheel < 0 ? 1 : -1,
                         e->x - L_img.x, e->y - L_img.y);
        return 1;
    case UI_EV_KEY:
        switch (e->key) {
        case UI_KEY_LEFT:  ph_step(-1); pc64_shell_dirty(); return 1;
        case UI_KEY_RIGHT: ph_step(1);  pc64_shell_dirty(); return 1;
        case UI_KEY_UP:    if (ph_sel > 0) ph_sel--; pc64_shell_dirty(); return 1;
        case UI_KEY_DOWN:  if (ph_sel < ph_n - 1) ph_sel++; pc64_shell_dirty(); return 1;
        case UI_KEY_ENTER: ph_activate(ph_sel); return 1;
        case UI_KEY_PGUP:  ph_sel -= pane_rows(); if (ph_sel < 0) ph_sel = 0;
                           pc64_shell_dirty(); return 1;
        case UI_KEY_PGDN:  ph_sel += pane_rows(); if (ph_sel > ph_n - 1) ph_sel = ph_n - 1;
                           pc64_shell_dirty(); return 1;
        }
        return 1;
    case UI_EV_CHAR:
        switch (e->ch) {
        case '+': case '=': ph_zoom_step(1, L_img.w/2, L_img.h/2); return 1;
        case '-':           ph_zoom_step(-1, L_img.w/2, L_img.h/2); return 1;
        case 'f': case 'F': ph_fit = 1; ph_cache_ok = 0; pc64_shell_dirty(); return 1;
        case '1':           ph_zoom_to(100, L_img.w/2, L_img.h/2); return 1;
        case ' ':           if (ph_anim) { ph_play = !ph_play;
                                ph_due = TickCount(); pc64_shell_dirty(); }
                            return 1;
        case '\t':          ph_pane = !ph_pane; ph_cache_ok = 0;
                            pc64_shell_dirty(); return 1;
        }
        return 1;
    default:
        return 1;
    }
}

/* ---- module lifecycle ------------------------------------------------------- */
static unoui_canvas g_canvas = { ph_draw, ph_event, 0 };

static void photos_build(unoui_window *win)
{
    const struct unoui_theme *t = TH();
    const unoui_metrics *m = &t->m;
    int aw, ah;
    g_win = win;
    aw = fb_width() - 120;  if (aw > 900) aw = 900; if (aw < 420) aw = 420;
    ah = fb_height() - 110; if (ah > 640) ah = 640; if (ah < 300) ah = 300;
    unoui_window_init(win, "Photos", 36, 28,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      ah + m->title_h + 2 * m->pad + m->frame_w);
    unoui_add_canvas(win, 0, 0, aw, ah, &g_canvas);
    win->flags |= UI_WIN_RESIZE;
}

static int photos_action(const unoui_action *a) { (void)a; return 0; }
static int photos_key(int uni, int scan, int ctrl)
{ (void)uni; (void)scan; (void)ctrl; return 0; }

static void photos_opened(void)
{
    um_set_alloc(malloc, free);
    ph_vol = 0; ph_path[0] = 0; ph_sel = ph_scroll = 0;
    /* start on the first volume with anything to show - image files OR
     * directories that could hold them (the ESP ships PICTURES\) */
    { int nv = uno_fs_volumes(), v;
      for (v = 0; v < nv; v++) { ph_vol = v; ph_relist(); if (ph_n) break; }
      if (ph_n == 0) { ph_vol = 0; ph_relist(); } }
    /* out-of-box: if the volume's root has a PICTURES\ directory (the demo
     * set build.sh stages), step straight into it and show its first image -
     * a viewer that opens onto a picture beats one that opens onto a list */
    if (!ph_path[0]) {
        int i;
        for (i = 0; i < ph_n; i++)
            if (ph_e[i].is_dir && !strcmp(ph_e[i].name, "PICTURES")) {
                int j, before = ph_n;
                strcpy(ph_path, "PICTURES");
                ph_relist();
                if (!ph_n) { ph_path[0] = 0; ph_n = before; ph_relist(); break; }
                for (j = 0; j < ph_n; j++)
                    if (!ph_e[j].is_dir) { ph_sel = j; ph_open_entry(j); break; }
                break;
            }
    }
}

static void photos_closed(void)
{
    ph_close_img();
    ph_drag = 0;
}

static int  photos_canvas_index(void) { return 0; }

/* animation pump: decode the next GIF frame when its delay elapses */
static void photos_frame(void)
{
    int delay = 0, r;
    if (!ph_ok || !ph_anim || !ph_play || !ph_img) return;
    if (TickCount() < ph_due) return;
    r = um_image_frame(ph_img, &delay);
    if (r == 0) {
        um_image_rewind();
        ph_frame_no = 0;
        r = um_image_frame(ph_img, &delay);
    }
    if (r != 1) { ph_play = 0; ph_fail(um_error()); pc64_shell_dirty(); return; }
    ph_frame_no++;
    { int ms = delay > 0 ? delay : 100;
      long t = (ms * 60 + 999) / 1000;
      ph_due = TickCount() + (t < 2 ? 2 : t); }
    ph_cache_ok = 0;
    pc64_shell_dirty();
}

static const UnoUuiApp kPhotos = {
    UNO_UUIAPP_ABI, "Photos",
    photos_build, photos_action, photos_key, photos_frame,
    photos_opened, photos_closed, photos_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved) { (void)reserved; return &kPhotos; }
