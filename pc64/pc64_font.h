/* pc64_font - a TrueType text engine for the unoui shell, built on the vendored
 * stb_truetype rasteriser (freestanding: our own malloc arena + math shims). It
 * registers an fb text provider (fb_set_font), so selecting a TTF re-skins ALL
 * UI text - titles, labels, buttons, lists - proportionally and anti-aliased,
 * with optional subpixel (LCD) rendering. Fonts are loaded from the ESP/FAT on
 * demand (SANS.TTF / MONO.TTF / UBUNTU.TTF). Slot -1 = the built-in bitmap. */
#ifndef PC64_FONT_H
#define PC64_FONT_H
#include "fb.h"          /* fb_px */

int         uno_font_count(void);            /* number of bundled TTF faces */
const char *uno_font_name(int slot);         /* display name of a TTF slot  */
int         uno_font_active(void);           /* current slot, or -1 = bitmap */
int         uno_font_subpixel(void);         /* 1 if subpixel AA is on */

/* Select the system font. slot -1 = the built-in 8x8 bitmap; 0..count-1 = a
 * bundled TTF (loaded on first use). Returns the slot actually in effect (falls
 * back to -1 if the TTF can't be loaded). Registers/among fb_set_font. */
int  uno_font_use(int slot);
void uno_font_set_px(int px);                /* target UI text height in px */
void uno_font_set_subpixel(int on);          /* LCD subpixel AA on/off */
void uno_font_set_ui_scale(int pct);         /* UI scale % (100..200): scales all text */
int  uno_font_ui_scale(void);

/* Draw with a specific slot regardless of the system font (for the Editor's
 * per-document font). Returns the advanced x. slot -1 = bitmap. These push/pop
 * the active provider around the call. */
int  uno_font_draw_with(int slot, int x, int y, const char *s, fb_px fg, long bg);
int  uno_font_text_w_with(int slot, const char *s);
int  uno_font_height_with(int slot);

/* depth-1 push/pop of the active font (for per-window font overrides). slot -2
 * = inherit (no change); -1 = bitmap; 0.. = a TTF. */
void uno_font_push(int slot);
void uno_font_pop(void);

/* Styled text at an explicit pixel size, independent of the system font/px
 * (browser headings, the Write app's rich runs). style: bit 0 = bold (double
 * strike), bit 1 = italic (sheared). px is clamped 8..40; slot must be a TTF
 * slot (falls back to the system path if it can't load). Draw returns the
 * advanced x; the _w/_h forms measure with identical metrics. */
#define UNO_FS_BOLD   1
#define UNO_FS_ITALIC 2
int  uno_font_draw_styled(int slot, int px, int style, int x, int y,
                          const char *s, fb_px fg, long bg);
int  uno_font_text_w_styled(int slot, int px, int style, const char *s);
int  uno_font_height_px(int slot, int px);
int  uno_font_baseline_px(int slot, int px);         /* ascent within the cell */

#endif
