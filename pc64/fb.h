/* ===========================================================================
 * UnoDOS/Dreamcast software framebuffer - the platform drawing layer.
 *
 * ALL UnoDOS rendering happens in software against this 640x480x32 buffer in
 * main RAM (~1.2 MB of 16 MB); each vblank the DC target uploads it to the
 * PowerVR2 as a textured fullscreen quad (the host target writes it to a PNG).
 * This keeps UnoDOS's incremental/XOR drawing semantics exact and shrinks the
 * present to init + a per-vblank copy (HANDOFF SS2). The uno_fill / text_at
 * wrappers in unodos.c sit directly on these primitives.
 *
 * 640x480 is the Dreamcast's native VGA/RGB resolution (vs the PS2 port's
 * 640x448 NTSC), so the desktop gets the full screen with no letterboxing -
 * the portable core derives all geometry from gScreen = FB_W x FB_H, so this
 * is the only file that names the resolution.
 *
 * Pixel layout: a uint32 is 0xAABBGGRR (R in the low byte). The host PPM writer
 * reads R,G,B from the low three bytes; the DC target converts each pixel to
 * RGB565 and copies it into the Dreamcast framebuffer (vram_s) once per vblank
 * (dc_main.c).
 * ===========================================================================
 */
#ifndef UNO_FB_H
#define UNO_FB_H

#include <stdint.h>

#ifdef UNO_PC64
/* pc64: the desktop size is RUNTIME-selected (F9 cycles native/half/third...
   of the GOP mode; uefi_main.c owns the variables). Everything that indexes
   fb[] uses FB_W as the row stride, so drawing code is unchanged - only the
   backing array is sized for the ceiling. */
#define FB_MAX_W 1920
#define FB_MAX_H 1200
extern int uno_fb_w, uno_fb_h;          /* current desktop size */
#define FB_W uno_fb_w
#define FB_H uno_fb_h
#define FB_BUF_PIX (FB_MAX_W * FB_MAX_H)
#else
#define FB_W 640
#define FB_H 480                /* Dreamcast native 640x480 (VGA / RGB) */
#define FB_BUF_PIX (FB_W * FB_H)
#endif

typedef uint32_t fb_px;

#define FB_RGB(r, g, b) \
    ((fb_px)(0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r)))

/* UnoDOS palette (PORT-SPEC SS1): desktop blue, cyan accent, magenta accent2,
   white text/highlight, plus black. */
#define UNO_BLUE  FB_RGB(0x00, 0x00, 0xAA)
#define UNO_CYAN  FB_RGB(0x00, 0xAA, 0xAA)
#define UNO_MAG   FB_RGB(0xAA, 0x00, 0xAA)
#define UNO_WHITE FB_RGB(0xFF, 0xFF, 0xFF)
#define UNO_BLACK FB_RGB(0x00, 0x00, 0x00)

extern fb_px fb[FB_BUF_PIX];

void fb_clear(fb_px c);
void fb_fill_rect(int x, int y, int w, int h, fb_px c);

/* Clip window (absolute fb coords): confine all subsequent drawing to the
 * intersection of this rect and the screen. Default is the whole screen, so
 * callers that never set it are unaffected. unoui uses this to stop widgets
 * from overflowing their window. fb_pixel is a clip-respecting single plot. */
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip(void);
void fb_pixel(int x, int y, fb_px c);
void fb_frame_rect(int x, int y, int w, int h, fb_px c);   /* 1px border */
void fb_invert_rect(int x, int y, int w, int h);           /* XOR to white */
void fb_hline(int x, int y, int w, fb_px c);
void fb_vline(int x, int y, int h, fb_px c);

/* alpha blend (a: 0..255) + vertical gradient - for modern themes. Clip-safe,
 * additive: no effect on callers that don't use them. */
void fb_blend_pixel(int x, int y, fb_px c, int a);
void fb_blend_rect (int x, int y, int w, int h, fb_px c, int a);
void fb_grad_v     (int x, int y, int w, int h, fb_px top, fb_px bot);

/* anti-aliased rounded rectangles. `corners` is a bitmask of FB_CORNER_*. */
enum { FB_CORNER_TL = 1, FB_CORNER_TR = 2, FB_CORNER_BL = 4, FB_CORNER_BR = 8,
       FB_CORNER_ALL = 15, FB_CORNER_TOP = 3, FB_CORNER_BOTTOM = 12 };
void fb_round_rect  (int x, int y, int w, int h, int rad, fb_px c);
void fb_round_rect_a(int x, int y, int w, int h, int rad, fb_px c, int a, int corners);

/* per-channel (subpixel/LCD) blend of `fg` over the pixel, alphas 0..255. With
 * aR==aG==aB this is a plain grayscale-AA plot. Clip-safe. */
void fb_blend_pixel_sub(int x, int y, fb_px fg, int aR, int aG, int aB);

/* Optional text provider: when registered, fb_glyph/fb_text/fb_text_w route
 * through it (e.g. a TrueType engine) instead of the built-in 8x8 bitmap font.
 * Register NULL to fall back to the bitmap font. Additive + default-safe: ports
 * that never register one are unaffected. */
typedef struct {
    int (*glyph) (int x, int y, int cp, fb_px fg, long bg);  /* draw; return advanced x */
    int (*text_w)(const char *s);                            /* pixel width of s        */
    int (*height)(void);                                     /* line height in px       */
} fb_font;
void fb_set_font(const fb_font *f);      /* NULL = built-in bitmap font */
const fb_font *fb_get_font(void);

/* 8x8 text. bg < 0 = transparent (glyph pixels only). Returns the x past the
   string. Each glyph advances 8px. */
int  fb_glyph(int x, int y, int ch, fb_px fg, long bg);
int  fb_text(int x, int y, const char *s, fb_px fg, long bg);
int  fb_text_w(const char *s);                             /* pixel width */

/* scaled text: each font pixel becomes a scale x scale block (8*scale tall,
   advances 8*scale per glyph). bg < 0 = transparent. */
int  fb_big_text(int x, int y, const char *s, fb_px fg, long bg, int scale);

#endif
