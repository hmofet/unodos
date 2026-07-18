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

#define FB_W 640
#define FB_H 480                /* Dreamcast native 640x480 (VGA / RGB) */

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

extern fb_px fb[FB_W * FB_H];

void fb_clear(fb_px c);
void fb_fill_rect(int x, int y, int w, int h, fb_px c);
void fb_frame_rect(int x, int y, int w, int h, fb_px c);   /* 1px border */
void fb_invert_rect(int x, int y, int w, int h);           /* XOR to white */
void fb_hline(int x, int y, int w, fb_px c);
void fb_vline(int x, int y, int h, fb_px c);

/* 8x8 text. bg < 0 = transparent (glyph pixels only). Returns the x past the
   string. Each glyph advances 8px. */
int  fb_glyph(int x, int y, int ch, fb_px fg, long bg);
int  fb_text(int x, int y, const char *s, fb_px fg, long bg);
int  fb_text_w(const char *s);                             /* pixel width */

/* scaled text: each font pixel becomes a scale x scale block (8*scale tall,
   advances 8*scale per glyph). bg < 0 = transparent. */
int  fb_big_text(int x, int y, const char *s, fb_px fg, long bg, int scale);

/* ---- unoui/Aurora support (implemented in fb_aa.c, uui build only) --------
 * The alpha-blend / gradient / rounded-corner primitives the unoui toolkit and
 * the Aurora theme draw with (fb.c's legacy Mac core never touches them, so
 * fb_aa.c is only linked into the unoui (`dc uui`) build). */
#define FB_BUF_PIX (FB_W * FB_H)      /* unoui's cached desktop bg is g_bg[FB_BUF_PIX] */

enum { FB_CORNER_TL = 1, FB_CORNER_TR = 2, FB_CORNER_BL = 4, FB_CORNER_BR = 8,
       FB_CORNER_ALL = 15, FB_CORNER_TOP = 3, FB_CORNER_BOTTOM = 12 };

void fb_pixel      (int x, int y, fb_px c);
void fb_blend_pixel(int x, int y, fb_px c, int a);
void fb_blend_rect (int x, int y, int w, int h, fb_px c, int a);
void fb_grad_v     (int x, int y, int w, int h, fb_px top, fb_px bot);
void fb_round_rect (int x, int y, int w, int h, int rad, fb_px c);
void fb_round_rect_a(int x, int y, int w, int h, int rad, fb_px c, int a, int corners);
void fb_set_clip   (int x, int y, int w, int h);   /* confine drawing to a rect */
void fb_reset_clip (void);                          /* clip = whole screen */

#endif
