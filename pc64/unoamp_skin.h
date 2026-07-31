/* UnoAmp skin engine - Winamp 2 .wsz support. See unoamp_skin.c and
 * docs/PLAYER-WINAMP-PLAN.md phase 3.
 *
 * A .wsz is a ZIP of BMP sprite sheets plus a couple of text files, so unlike
 * binary plugins it needs no compatibility layer at all: pc64 already had raw
 * inflate (um_inflate, what ZIP method 8 stores) and a BMP decoder.
 */
#ifndef PC64_UNOAMP_SKIN_H
#define PC64_UNOAMP_SKIN_H

/* The classic sheet set. A skin missing any of these is still usable - Winamp
 * falls back per sheet and so do we. */
enum {
    UNOAMP_SHEET_MAIN = 0,      /* the 275x116 main window background        */
    UNOAMP_SHEET_CBUTTONS,      /* prev/play/pause/stop/next/eject           */
    UNOAMP_SHEET_TITLEBAR,
    UNOAMP_SHEET_SHUFREP,       /* shuffle / repeat / EQ / PL toggles        */
    UNOAMP_SHEET_POSBAR,
    UNOAMP_SHEET_VOLUME,
    UNOAMP_SHEET_BALANCE,
    UNOAMP_SHEET_MONOSTER,
    UNOAMP_SHEET_PLAYPAUS,      /* the play/pause/stop status indicator      */
    UNOAMP_SHEET_NUMBERS,       /* the time digits (NUMBERS or NUMS_EX)      */
    UNOAMP_SHEET_TEXT,          /* the bitmap font for the track title       */
    UNOAMP_SHEET_EQMAIN,
    UNOAMP_SHEET_PLEDIT,
    UNOAMP_SHEET_N
};

#define UNOAMP_VISCOLORS 24     /* viscolor.txt: 24 RGB triples              */

/* px is in FRAMEBUFFER word order (0xAABBGGRR, see FB_RGB in fb.h), not the
 * 0xAARRGGBB a BMP reader instinctively produces. */
typedef struct { unsigned *px; int w, h; } unoamp_sheet;

typedef struct {
    unoamp_sheet sheet[UNOAMP_SHEET_N];
    unsigned viscolor[UNOAMP_VISCOLORS];   /* 0=bg 1=peak 2..17 bars 18..23 osc */
    int      have_viscolor;
    unsigned pl_normal, pl_current, pl_bg; /* pledit.txt, 0 when absent       */
} unoamp_skin;

/* Load a .wsz. 1 = a usable skin (MAIN.BMP at minimum). Replaces whatever was
 * loaded; the skin arena is fixed-size, so a huge archive fails rather than
 * exhausting memory. */
int  unoamp_skin_load(int vol, const char *path);
void unoamp_skin_unload(void);
int  unoamp_skin_loaded(void);
const unoamp_skin *unoamp_skin_get(void);

/* Blit a sprite, clipped to both sheet and destination. */
void unoamp_skin_blit(int sheet, int sx, int sy, int w, int h,
                      unsigned *dst, int dst_w, int dst_h, int dx, int dy);

#endif
