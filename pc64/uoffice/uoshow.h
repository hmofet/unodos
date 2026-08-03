/* ===========================================================================
 * uoshow - UnoShow's presentation model, autoshape geometry and slide
 * renderer.                                               [EXPERIMENTAL]
 * (OFFICE97-PLAN §7 phase 11; the conformance items are OFFICE97-SPEC.md
 * S-UOS-03 and S-UOS-04.)
 *
 * ONE RENDERER, FIVE JOBS.  PowerPoint draws the same slide at the editing
 * zoom, as a sorter thumbnail, on the notes page, in the handout grid and
 * full-screen in a show.  Those are not five renderers with different
 * fidelity - they are one renderer told a different destination rectangle,
 * and everything in this file is written in SLIDE POINTS so that stays true.
 * A shape that only looks right at 100% is a shape that is wrong.
 *
 * SLIDE COORDINATES ARE POINTS, 72 to the inch, origin top-left, and the
 * on-screen show is 720 x 540 (10 x 7.5 inches) - PowerPoint 97's default.
 * They are `short`s: a banner slide is 4176 points wide and everything fits
 * with room to spare, and halving the model's size matters when the whole
 * module has to live in a 4 MB arena shared with every other module.
 *
 * AUTOSHAPES ARE PATHS IN A 1000 x 1000 BOX, not pixels.  uos_geom_path()
 * hands back a polygon in that box and the renderer maps it onto the shape's
 * frame, so one description serves every size and every zoom.  The three
 * shapes a polygon describes badly - ellipse, round rectangle, line - say so
 * through uos_geom_kind() rather than being approximated by 64 vertices.
 *
 * MEMORY IS POOLED, and this is the UnoCalc lesson applied at design time
 * rather than after a 104 MB link: shapes, paragraphs and text come out of
 * three pools shared by the whole presentation, and a slide holds indices.
 * Per-slide arrays would multiply the worst case by UOS_MAXSLIDE whether the
 * slides existed or not.
 * ======================================================================== */
#ifndef UOSHOW_H
#define UOSHOW_H

#include "uochrome.h"

/* ---- the slide plane -------------------------------------------------------- */
#define UOS_SLIDE_W 720           /* points: the 10 x 7.5in on-screen show    */
#define UOS_SLIDE_H 540
#define UOS_GEOM_BOX 1000         /* autoshape paths live in a 1000-unit box  */

/* Page setups PowerPoint 97 offers (Page Setup: Slides sized for). */
enum { UOS_PS_SCREEN = 0, UOS_PS_LETTER, UOS_PS_A4, UOS_PS_35MM,
       UOS_PS_OVERHEAD, UOS_PS_BANNER, UOS_PS_CUSTOM, UOS_PS_COUNT };
const char *uos_pagesetup_name(int ps);
void        uos_pagesetup_size(int ps, int *w, int *h);   /* points          */

/* ---- colour schemes ---------------------------------------------------------
 * The 8 roles are PowerPoint's, in PowerPoint's order, because that order is
 * what "the first eight swatches in every colour dropdown" means (S-UOS-03).
 * A shape stores a ROLE where it can, so Apply Design re-colours a deck by
 * swapping one table rather than by walking every shape. */
enum { UOS_C_BG = 0, UOS_C_TEXT, UOS_C_SHADOW, UOS_C_TITLE, UOS_C_FILL,
       UOS_C_ACCENT, UOS_C_ACCENT_HL, UOS_C_ACCENT_FOL, UOS_NSCHEME };

typedef struct {
    char  name[24];
    fb_px c[UOS_NSCHEME];
} uos_scheme;

int               uos_schemes(void);
const uos_scheme *uos_scheme_at(int i);
const char       *uos_scheme_role(int role);

/* A colour is either a literal or a scheme role.  UOS_SCHEME_COLOR(r) marks
 * the low byte as a role and sets a tag byte no real RGB value carries, so
 * one fb_px field can hold both without a second field per colour. */
#define UOS_SCHEME_TAG   0xFEu
#define UOS_SCHEME_COLOR(role) (((fb_px)UOS_SCHEME_TAG << 24) | (fb_px)(role))
#define UOS_IS_SCHEME(c)  ((((c) >> 24) & 0xFFu) == UOS_SCHEME_TAG)
#define UOS_SCHEME_ROLE(c) ((int)((c) & 0x07u))

/* ---- autoshape geometry ----------------------------------------------------- */
enum {
    UOS_G_RECT = 0, UOS_G_ROUNDRECT, UOS_G_ELLIPSE, UOS_G_TRIANGLE,
    UOS_G_RTRIANGLE, UOS_G_DIAMOND, UOS_G_PARALLELOGRAM, UOS_G_TRAPEZOID,
    UOS_G_PENTAGON, UOS_G_HEXAGON, UOS_G_OCTAGON, UOS_G_CROSS,
    UOS_G_STAR5, UOS_G_ARROW_R, UOS_G_ARROW_L, UOS_G_ARROW_U,
    UOS_G_ARROW_D, UOS_G_CHEVRON, UOS_G_CALLOUT, UOS_G_LINE,
    UOS_G_COUNT
};
enum { UOS_GK_POLY = 0, UOS_GK_ELLIPSE, UOS_GK_ROUNDRECT, UOS_GK_LINE };

const char *uos_geom_name(int g);
int         uos_geom_kind(int g);
/* Fill `xy` with 2*n coordinates in the 0..UOS_GEOM_BOX box; returns n.
 * `adj` is the shape's adjustment (0..1000, 500 = the default handle
 * position) and is ignored by shapes that have none. */
int         uos_geom_path(int g, int adj, short *xy, int maxpt);

/* ---- fill, line, shadow ----------------------------------------------------- */
enum { UOS_F_NONE = 0, UOS_F_SOLID, UOS_F_GRAD_V, UOS_F_GRAD_H, UOS_F_PATTERN };
enum { UOS_P_H = 0, UOS_P_V, UOS_P_FDIAG, UOS_P_BDIAG, UOS_P_GRID,
       UOS_P_DOTS, UOS_P_COUNT };
typedef struct {
    unsigned char kind;       /* UOS_F_*                                     */
    unsigned char pattern;    /* UOS_P_* when kind == UOS_F_PATTERN          */
    fb_px c1, c2;             /* c2 = the second gradient / pattern colour   */
} uos_fill;

enum { UOS_L_NONE = 0, UOS_L_SOLID, UOS_L_DASH, UOS_L_DOT };
typedef struct {
    unsigned char kind;       /* UOS_L_*                                     */
    unsigned char width;      /* points                                      */
    fb_px c;
} uos_line;

typedef struct {
    unsigned char on;
    short dx, dy;             /* points                                      */
    fb_px c;
} uos_shadow;

/* ---- text -------------------------------------------------------------------
 * A text body is a run of paragraphs out of the shared paragraph pool; each
 * paragraph names a slice of the shared character pool.  Five outline levels,
 * because that is what the Outline view promotes and demotes between. */
enum { UOS_AL_LEFT = 0, UOS_AL_CENTER, UOS_AL_RIGHT, UOS_AL_JUSTIFY };
#define UOS_MAXLEVEL 5

typedef struct {
    unsigned short size;      /* points                                      */
    unsigned char  bold, italic, underline, shadow;
    fb_px          color;     /* literal or UOS_SCHEME_COLOR(role)           */
} uos_chp;

typedef struct {
    int            at, n;     /* into the presentation's character pool      */
    unsigned char  level;     /* 0..UOS_MAXLEVEL-1                           */
    unsigned char  align;     /* UOS_AL_*                                    */
    unsigned char  bullet;    /* 0 = none, else the bullet character         */
    unsigned char  space_before; /* points                                   */
    uos_chp        chp;
} uos_para;

/* ---- shapes ------------------------------------------------------------------ */
enum { UOS_PH_NONE = 0, UOS_PH_TITLE, UOS_PH_CTRTITLE, UOS_PH_SUBTITLE,
       UOS_PH_BODY, UOS_PH_BODY2, UOS_PH_OBJECT, UOS_PH_NUMBER,
       UOS_PH_DATE, UOS_PH_FOOTER, UOS_PH_COUNT };

typedef struct {
    short x, y, w, h;         /* slide points, top-left origin               */
    short adj;                /* autoshape adjustment, 0..1000               */
    unsigned char geom;       /* UOS_G_*                                     */
    unsigned char ph;         /* UOS_PH_*: what a layout may refill          */
    unsigned char group;      /* 0 = ungrouped, else the group id            */
    unsigned char hidden;
    uos_fill   fill;
    uos_line   line;
    uos_shadow shadow;
    int para_at, para_n;      /* into the paragraph pool; 0 = no text body   */
} uos_shape;

/* ---- the 24 AutoLayouts ------------------------------------------------------
 * The New Slide grid, in the grid's own order.  A layout is a table of
 * placeholder frames, so "apply a layout" is "re-frame the placeholders and
 * add the missing ones", which is exactly what PowerPoint does to a slide
 * that already has content. */
enum {
    UOS_AL_TITLE = 0, UOS_AL_BULLETS, UOS_AL_2COL, UOS_AL_TABLE,
    UOS_AL_TEXT_CHART, UOS_AL_CHART_TEXT, UOS_AL_ORGCHART, UOS_AL_CHART,
    UOS_AL_TEXT_CLIP, UOS_AL_CLIP_TEXT, UOS_AL_TITLE_ONLY, UOS_AL_BLANK,
    UOS_AL_TEXT_OBJ, UOS_AL_OBJ_TEXT, UOS_AL_OBJ, UOS_AL_TEXT_MEDIA,
    UOS_AL_OBJ_OVER_TEXT, UOS_AL_TEXT_OVER_OBJ, UOS_AL_4OBJ, UOS_AL_2OBJ_TEXT,
    UOS_AL_2COL_OBJ, UOS_AL_TEXT_2OBJ, UOS_AL_TITLE_OBJ, UOS_AL_VERT_TEXT,
    UOS_AL_COUNT
};
const char *uos_layout_name(int lay);

/* ---- the presentation --------------------------------------------------------
 * One pool of each thing, shared by every slide (see the header note). */
#define UOS_MAXSLIDE 64
#define UOS_MAXSHAPE 512
#define UOS_MAXPARA  1024
#define UOS_TEXTPOOL (48 * 1024)
#define UOS_MAXPERSLIDE 48        /* shapes on one slide                     */

typedef struct uos_pres uos_pres;

uos_pres *uos_new(void);          /* one Title Slide, the default scheme     */
void      uos_free(uos_pres *p);

int  uos_slides(const uos_pres *p);
int  uos_slide_add(uos_pres *p, int layout);   /* returns the slide index    */
int  uos_slide_insert(uos_pres *p, int at, int layout);
int  uos_slide_delete(uos_pres *p, int i);
int  uos_slide_move(uos_pres *p, int from, int to);
int  uos_slide_layout(const uos_pres *p, int i);
int  uos_slide_set_layout(uos_pres *p, int i, int layout);
int  uos_slide_hidden(const uos_pres *p, int i);
void uos_slide_hide(uos_pres *p, int i, int on);

/* Shapes on a slide, in z-order (0 = bottom). */
int         uos_shapes(const uos_pres *p, int slide);
uos_shape  *uos_shape_at(uos_pres *p, int slide, int z);
const uos_shape *uos_shape_at_c(const uos_pres *p, int slide, int z);
int         uos_shape_add(uos_pres *p, int slide, int geom,
                          int x, int y, int w, int h);
int         uos_shape_delete(uos_pres *p, int slide, int z);
int         uos_shape_raise(uos_pres *p, int slide, int z, int to_top);
int         uos_shape_lower(uos_pres *p, int slide, int z, int to_bottom);
int         uos_shape_group(uos_pres *p, int slide, const int *zs, int n);
int         uos_shape_ungroup(uos_pres *p, int slide, int z);
/* The placeholder of this role on this slide, or -1. */
int         uos_placeholder(const uos_pres *p, int slide, int role);

/* Text.  A shape's body is replaced wholesale by uos_text_set (the model is
 * a store, not an editor - the app owns the caret), and read back paragraph
 * by paragraph.  `text` may contain '\n' to make several paragraphs. */
int          uos_text_set(uos_pres *p, int slide, int z, const char *text);
int          uos_text_paras(const uos_pres *p, int slide, int z);
uos_para    *uos_para_at(uos_pres *p, int slide, int z, int i);
const char  *uos_para_text(const uos_pres *p, int slide, int z, int i, int *len);
int          uos_para_add(uos_pres *p, int slide, int z, const char *text,
                          int level);
int          uos_para_set_level(uos_pres *p, int slide, int z, int i, int level);

/* The deck's scheme, and per-slide background overrides. */
void       uos_set_scheme(uos_pres *p, const uos_scheme *s);
const uos_scheme *uos_get_scheme(const uos_pres *p);
void       uos_slide_bg(uos_pres *p, int slide, const uos_fill *f); /* 0 = master */
void       uos_slide_omit_master(uos_pres *p, int slide, int on);

/* Masters.  The slide master's shapes are drawn under every slide's own; the
 * title master under title slides.  They are slides in the same pools, held
 * out of the numbered sequence. */
enum { UOS_M_SLIDE = 0, UOS_M_TITLE, UOS_M_NOTES, UOS_M_HANDOUT, UOS_M_COUNT };
int  uos_master(const uos_pres *p, int which);   /* a slide index, or -1     */

/* Header and Footer (S-UOS-03). */
typedef struct {
    unsigned char date_on, date_auto, number_on, footer_on, not_on_title;
    char date[32], footer[64];
} uos_hf;
void uos_set_hf(uos_pres *p, const uos_hf *hf);
const uos_hf *uos_get_hf(const uos_pres *p);

int  uos_dirty(const uos_pres *p);
void uos_set_dirty(uos_pres *p, int on);

/* Reading the parts of a slide the renderer needs without exposing the
 * slide struct itself. */
int  uos_slide_has_bg(const uos_pres *p, int slide, uos_fill *out);
int  uos_slide_omits_master(const uos_pres *p, int slide);
/* What a fresh placeholder of this role and level is formatted as. */
uos_chp uos_default_chp(int ph, int level);
int     uos_ph_is_text(int ph);

/* ---- the metrics seam --------------------------------------------------------
 * The only way the renderer learns how wide anything is.  Same shape and same
 * reason as uoword's: the host gate draws with an 8x8 bitmap font and pc64
 * draws with the kerned TTF engine at a size that depends on the zoom, and
 * this must be the only place that difference exists.  `px` is the pixel size
 * the run should draw at, already scaled from the point size. */
typedef struct {
    int  (*text_w)(const char *s, int n, const uos_chp *c, int px, void *ctx);
    int  (*height)(const uos_chp *c, int px, void *ctx);
    void (*draw)(int x, int y, const char *s, int n, const uos_chp *c,
                 int px, fb_px col, void *ctx);
    void *ctx;
} uos_metrics;
void uos_set_metrics(const uos_metrics *m);

/* ---- the renderer ------------------------------------------------------------
 * uos_render draws slide `slide` into the rectangle, letterboxed to the
 * slide's aspect, and fills `map` (if given) with what it used - which is how
 * the app turns a click back into slide coordinates.  ONE function: the
 * editor, the sorter thumbnail and the full-screen show all call this. */
enum { UOS_R_BW = 1,          /* Black and White view                        */
       UOS_R_NOBG = 2,        /* skip the background (drawing over one)      */
       UOS_R_NOMASTER = 4,    /* skip master shapes                          */
       UOS_R_PHFRAMES = 8,    /* dashed frames + prompts on empty holders    */
       UOS_R_NOTEXT = 16 };   /* shapes only - the sorter at tiny sizes      */

typedef struct {
    int x, y, w, h;           /* the letterboxed rectangle actually used     */
    int num, den;             /* the scale applied: screen = slide*num/den   */
} uos_map;

void uos_render(uos_pres *p, int slide, int x, int y, int w, int h,
                int flags, uos_map *map);
/* The letterboxed rectangle uos_render would use, without drawing - the app
 * needs it to place a slide-sized selection layer over the canvas. */
void uos_fit(int w, int h, int *ox, int *oy, int *ow, int *oh);
/* Confine the NEXT renders to a screen rectangle, on top of the slide's own
 * clip.  The slide show's transitions are built out of this: reveal the
 * incoming slide through a moving window, one render per band.  uos_clip_off
 * restores the normal behaviour. */
void uos_clip(int x, int y, int w, int h);
void uos_clip_off(void);

/* Screen <-> slide, both directions, through a map uos_render filled. */
void uos_to_slide(const uos_map *m, int sx, int sy, int *ox, int *oy);
void uos_to_screen(const uos_map *m, int px, int py, int *ox, int *oy);
/* The topmost shape covering this SLIDE point, or -1. */
int  uos_hit(const uos_pres *p, int slide, int px, int py);

/* Resolve a colour that may be a scheme role. */
fb_px uos_color(const uos_pres *p, fb_px c);
/* The B&W view's rule for a colour, exposed because the app's preview needs
 * the same answer the renderer gives. */
fb_px uos_bw(fb_px c);

#endif /* UOSHOW_H */
