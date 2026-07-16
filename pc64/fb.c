/* UnoDOS/PS2 software framebuffer primitives (see fb.h). Pure C, no platform
 * dependencies - shared verbatim by the host shim and the EE target. */
#include "fb.h"
#include "build/font_data.h"

fb_px fb[FB_BUF_PIX];

/* clip a rect to the framebuffer; returns 0 if fully off-screen */
static int clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > FB_W) *w = FB_W - *x;
    if (*y + *h > FB_H) *h = FB_H - *y;
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
    int r, c;
    const unsigned char *g;
    if (ch < UNO_FONT_FIRST || ch >= UNO_FONT_FIRST + UNO_FONT_COUNT) ch = ' ';
    g = uno_font8x8[ch - UNO_FONT_FIRST];
    for (r = 0; r < 8; r++) {
        int yy = y + r;
        unsigned char row = g[r];
        if (yy < 0 || yy >= FB_H) continue;
        for (c = 0; c < 8; c++) {
            int xx = x + c;
            if (xx < 0 || xx >= FB_W) continue;
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
