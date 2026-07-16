/* UnoDOS/PS2 software framebuffer primitives (see fb.h). Pure C, no platform
 * dependencies - shared verbatim by the host shim and the EE target. */
#include "fb.h"
#include "build/font_data.h"

fb_px fb[FB_BUF_PIX];

/* Optional clip window (absolute fb coords). All drawing is confined to the
 * intersection of this window and the screen. Default = whole screen, so any
 * caller that never touches it (the legacy core, every non-unoui port) is
 * unaffected. unoui sets it per-window so a widget can never overflow its
 * frame onto the desktop. */
#define CL_BIG (1 << 28)
static int cl_x0 = 0, cl_y0 = 0, cl_x1 = CL_BIG, cl_y1 = CL_BIG;

void fb_set_clip(int x, int y, int w, int h)
{
    cl_x0 = x; cl_y0 = y; cl_x1 = x + w; cl_y1 = y + h;
}

void fb_reset_clip(void)
{
    cl_x0 = 0; cl_y0 = 0; cl_x1 = CL_BIG; cl_y1 = CL_BIG;
}

/* current clip window intersected with the screen */
static void clip_bounds(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = cl_x0 > 0 ? cl_x0 : 0;
    *y0 = cl_y0 > 0 ? cl_y0 : 0;
    *x1 = cl_x1 < FB_W ? cl_x1 : FB_W;
    *y1 = cl_y1 < FB_H ? cl_y1 : FB_H;
}

void fb_pixel(int x, int y, fb_px c)
{
    int x0, y0, x1, y1; clip_bounds(&x0, &y0, &x1, &y1);
    if (x >= x0 && x < x1 && y >= y0 && y < y1) fb[y * FB_W + x] = c;
}

/* clip a rect to the framebuffer AND the active clip window; 0 if fully out */
static int clip(int *x, int *y, int *w, int *h)
{
    int x0, y0, x1, y1; clip_bounds(&x0, &y0, &x1, &y1);
    if (*x < x0) { *w += *x - x0; *x = x0; }
    if (*y < y0) { *h += *y - y0; *y = y0; }
    if (*x + *w > x1) *w = x1 - *x;
    if (*y + *h > y1) *h = y1 - *y;
    return (*w > 0 && *h > 0);
}

void fb_clear(fb_px c)
{
    int i;
    for (i = 0; i < FB_W * FB_H; i++) fb[i] = c;
}

void fb_fill_rect(int x, int y, int w, int h, fb_px c)
{
    int r, j;
    if (!clip(&x, &y, &w, &h)) return;
    for (r = 0; r < h; r++) {
        fb_px *p = &fb[(y + r) * FB_W + x];
        for (j = 0; j < w; j++) p[j] = c;
    }
}

void fb_hline(int x, int y, int w, fb_px c) { fb_fill_rect(x, y, w, 1, c); }
void fb_vline(int x, int y, int h, fb_px c) { fb_fill_rect(x, y, 1, h, c); }

void fb_frame_rect(int x, int y, int w, int h, fb_px c)
{
    if (w <= 0 || h <= 0) return;
    fb_hline(x, y, w, c);
    fb_hline(x, y + h - 1, w, c);
    fb_vline(x, y, h, c);
    fb_vline(x + w - 1, y, h, c);
}

void fb_invert_rect(int x, int y, int w, int h)
{
    int r, j;
    if (!clip(&x, &y, &w, &h)) return;
    for (r = 0; r < h; r++) {
        fb_px *p = &fb[(y + r) * FB_W + x];
        for (j = 0; j < w; j++) p[j] ^= 0x00FFFFFFu;   /* invert RGB, keep A */
    }
}

int fb_glyph(int x, int y, int ch, fb_px fg, long bg)
{
    int r, c, x0, y0, x1, y1;
    const unsigned char *g;
    clip_bounds(&x0, &y0, &x1, &y1);
    if (ch < UNO_FONT_FIRST || ch >= UNO_FONT_FIRST + UNO_FONT_COUNT) ch = ' ';
    g = uno_font8x8[ch - UNO_FONT_FIRST];
    for (r = 0; r < 8; r++) {
        int yy = y + r;
        unsigned char row = g[r];
        if (yy < y0 || yy >= y1) continue;
        for (c = 0; c < 8; c++) {
            int xx = x + c;
            if (xx < x0 || xx >= x1) continue;
            if (row & (0x80 >> c)) fb[yy * FB_W + xx] = fg;
            else if (bg >= 0)      fb[yy * FB_W + xx] = (fb_px)bg;
        }
    }
    return x + 8;
}

int fb_text(int x, int y, const char *s, fb_px fg, long bg)
{
    for (; *s; s++) x = fb_glyph(x, y, (unsigned char)*s, fg, bg);
    return x;
}

int fb_text_w(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n * 8;
}

static int fb_big_glyph(int x, int y, int ch, fb_px fg, long bg, int scale)
{
    int r, c;
    const unsigned char *g;
    if (ch < UNO_FONT_FIRST || ch >= UNO_FONT_FIRST + UNO_FONT_COUNT) ch = ' ';
    g = uno_font8x8[ch - UNO_FONT_FIRST];
    for (r = 0; r < 8; r++) {
        unsigned char row = g[r];
        for (c = 0; c < 8; c++) {
            if (row & (0x80 >> c))     fb_fill_rect(x + c * scale, y + r * scale, scale, scale, fg);
            else if (bg >= 0)          fb_fill_rect(x + c * scale, y + r * scale, scale, scale, (fb_px)bg);
        }
    }
    return x + 8 * scale;
}

int fb_big_text(int x, int y, const char *s, fb_px fg, long bg, int scale)
{
    for (; *s; s++) x = fb_big_glyph(x, y, (unsigned char)*s, fg, bg, scale);
    return x;
}
