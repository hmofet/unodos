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

#endif
