/* ===========================================================================
 * uoshow.c - UnoShow, the presentation app (OFFICE97-PLAN §7 phase 12).
 *
 * A unoui-CLASS .UNO module (APPS\UOSHOW.UNO), hosted like UnoWord and
 * UnoCalc: the shell hands it a window, it fills it with ONE canvas, and the
 * uoffice lane draws everything inside - menus, toolbars, the slide, the
 * status bar, dialogs.
 *
 * FOUR VIEWS, ONE RENDERER.  Slide, Outline, Slide Sorter and Notes Page are
 * four arrangements of uos_render() plus, in Outline, no renderer at all.
 * The Sorter is the honest test of phase 11's scale claim: if a shape is one
 * pixel out at thumbnail size, twelve thumbnails show it twelve times.
 *
 * THE SHOW HAS NO SECOND FRAMEBUFFER, so a transition cannot cross-fade two
 * bitmaps.  It does what the hardware allows: draw the outgoing slide, then
 * draw the incoming one through a moving window (uos_clip), one render per
 * band.  That is why the transition list here is the subset of PowerPoint's
 * that a bounded number of renders per frame can express - Wipe, Box, Split,
 * Cover, Uncover, Blinds, Random Bars - and why Dissolve and Checkerboard,
 * which need a per-cell mask over the whole slide, are honestly absent rather
 * than approximated by something that stutters.
 * ======================================================================== */
#include "uno_uuiapp.h"
#include "uno_appdesc.h"
#include "unoui.h"
#include "fb.h"
#include "uochrome.h"
#include "uoicons.h"
#include "uodlg.h"
#include "uobars.h"
#include "uofile.h"
#include "uoshow.h"
#include "unodoc.h"
/* unomedia, for um_set_alloc alone: unodoc inflates an OOXML part with
 * um_inflate, which allocates its own working state. */
#include "unomedia.h"
#include "pc64_font.h"

/* the shell's services this module imports by name */
void  pc64_shell_dirty(void);
int   pc64_shell_workarea_w(void);
int   pc64_shell_workarea_h(void);
void  pc64_shell_fullscreen(unoui_window *w);
int   pc64_shell_is_fullscreen(void);
void *malloc(unsigned long);
void  free(void *);
int   uno_fs_volumes(void);
const char *uno_fs_volume_name(int vol);
int   uno_fs_list_begin(int vol);
int   uno_fs_list_get(int vol, int i, char *name, int cap);
int   uno_fs_isdir(int vol, const char *name);
long  uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
long  uno_fs_size(int vol, const char *name);
int   uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);
unsigned long TickCount(void);

/* ---- commands ------------------------------------------------------------- */
enum {
    C_NEW = 1, C_OPEN, C_SAVE, C_EXIT,
    C_UNDO = 20, C_CUT, C_COPY, C_PASTE, C_DELSLIDE, C_DUP,
    C_V_SLIDE = 40, C_V_OUTLINE, C_V_SORTER, C_V_NOTES, C_V_BW,
    C_NEWSLIDE = 60, C_TEXTBOX, C_SHAPE, C_NEWSLIDE_OK,
    C_LAYOUT = 80, C_DESIGN, C_SCHEME, C_BULLET,
    C_SHOW = 100, C_TRANS, C_HIDE,
    C_PROMOTE = 120, C_DEMOTE, C_ALIGNL, C_ALIGNC, C_ALIGNR,
    C_ABOUT = 140
};

/* ---- state ----------------------------------------------------------------- */
static uoc_ui      CH;
static uod_ui      DL;
static uob_status  ST;
static uos_pres   *PR;
static unoui_window *g_win;
static unoui_canvas  g_canvas;
static unoui_rect    g_rect;
static int  g_have_rect;
static int  g_cidx = -1;

static int  g_view;                    /* 0 slide 1 outline 2 sorter 3 notes */
static int  g_bw;
static int  g_cur;                     /* the current slide                  */
static int  g_sel = -1;                /* the selected shape on it, or -1    */
static int  g_editing;
static char g_edit[512];
static unsigned char g_lvl[24];        /* levels survive a text_set rewrite  */
static int  g_nlvl;
static char g_name[64] = "Presentation1";
static char g_file[64];                     /* "" until saved or opened     */
static unsigned char *g_io;
static long g_iolen;
static char g_statl[48], g_statr[48];
static int  g_dlg;                     /* which dialog is up (a C_* code)    */
static uos_map g_map;

/* the show */
static int  g_show, g_show_black;
static int  g_trans = 1;               /* the deck's transition             */
static int  g_tprog, g_tfrom;
#define TRANS_FRAMES 12

/* The file half is defined below the editing half but used from the dialog
 * path above it. */
static int load_pres(int vol, const char *name);
static int save_pres(int vol, const char *name);

static void a_cpy(char *d, const char *s, int cap)
{ int i = 0; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int  a_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static char *a_num(long v, char *b)
{
    char t[16]; int n = 0, i = 0;
    if (v == 0) { b[0] = '0'; b[1] = 0; return b; }
    if (v < 0) { b[i++] = '-'; v = -v; }
    while (v > 0 && n < 15) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) b[i++] = t[--n];
    b[i] = 0;
    return b;
}

/* ---- the file dialog's filesystem seam ------------------------------------- */
static int fs_volumes(void) { return uno_fs_volumes(); }
static const char *fs_vname(int v) { return uno_fs_volume_name(v); }
static int fs_begin(int v) { return uno_fs_list_begin(v); }
static int fs_get(int v, int i, char *n, int c) { return uno_fs_list_get(v, i, n, c); }
static int fs_isdir(int v, const char *n) { return uno_fs_isdir(v, n); }
static const uof_fs kFs = { fs_volumes, fs_vname, fs_begin, fs_get, fs_isdir };
static const char *const kTypes[] = {
    "Presentation (*.ppt)", "Presentation (*.pptx)"
};
/* ---- which format a name asks for --------------------------------------------
 * unodoc reads either container by sniffing the bytes, so OPENING needs no
 * help from the name.  SAVING does: an empty document has no bytes to sniff,
 * and the user picked the format when they typed the extension.  Anything that
 * is not one of the OOXML extensions saves as the 97 binary, which is the
 * format this suite is a clone of and the right default for a name that says
 * nothing. */
static const char *ppt_ext(int type) { return type == 1 ? ".PPTX" : ".PPT"; }
/* ---- Save As: the name the user actually meant --------------------------------
 * The format is decided by the extension, and a user who picks a type from the
 * combo and types a bare name has said which format they want just as clearly
 * as one who typed the extension.  Office 97 appended the extension in exactly
 * this case, so this does too: a name with no dot gets the chosen type's
 * extension, and a name that already has one is left alone - including a name
 * whose extension disagrees with the combo, because what was typed is more
 * specific than what was picked. */
static void ensure_ext(char *name, int cap, int type)
{
    int i, n = 0;
    const char *ext;
    while (name[n]) n++;
    for (i = 0; i < n; i++) if (name[i] == '.') return;
    ext = ppt_ext(type);
    if (!ext) return;
    for (i = 0; ext[i] && n < cap - 1; i++) name[n++] = ext[i];
    name[n] = 0;
}

static int name_is_ooxml(const char *n, const char *ext)
{
    int i, dot = -1;
    for (i = 0; n[i]; i++) if (n[i] == '.') dot = i;
    if (dot < 0) return 0;
    n += dot + 1;
    for (i = 0; ext[i]; i++) {
        char a = n[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return n[i] == 0;
}


/* ---- menus ------------------------------------------------------------------ */
static const uoc_item kFile[] = {
    { "&New\tCtrl+N",     C_NEW,  UOI_NEW,  0, 0, 0 },
    { "&Open...\tCtrl+O", C_OPEN, UOI_OPEN, 0, 0, 0 },
    { "&Save\tCtrl+S",    C_SAVE, UOI_SAVE, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "E&xit",            C_EXIT, -1, 0, 0, 0 }
};
static const uoc_item kEdit[] = {
    { "&Undo\tCtrl+Z",   C_UNDO,  UOI_UNDO,  UOC_DISABLED, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "Cu&t\tCtrl+X",    C_CUT,   UOI_CUT,   0, 0, 0 },
    { "&Copy\tCtrl+C",   C_COPY,  UOI_COPY,  0, 0, 0 },
    { "&Paste\tCtrl+V",  C_PASTE, UOI_PASTE, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Duplicate\tCtrl+D", C_DUP, -1, 0, 0, 0 },
    { "Delete &Slide",   C_DELSLIDE, -1, 0, 0, 0 }
};
static const uoc_item kView[] = {
    { "&Slide",         C_V_SLIDE,   -1, 0, 0, 0 },
    { "&Outline",       C_V_OUTLINE, -1, 0, 0, 0 },
    { "Slide S&orter",  C_V_SORTER,  -1, 0, 0, 0 },
    { "&Notes Page",    C_V_NOTES,   -1, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Black and White", C_V_BW,    -1, 0, 0, 0 }
};
static const uoc_item kInsert[] = {
    { "New &Slide...\tCtrl+M", C_NEWSLIDE, UOI_NEW, 0, 0, 0 },
    { "&Text Box",             C_TEXTBOX,  -1, 0, 0, 0 },
    { "&AutoShape",            C_SHAPE,    -1, 0, 0, 0 }
};
static const uoc_item kFormat[] = {
    { "Slide &Layout...",       C_LAYOUT, -1, 0, 0, 0 },
    { "Slide &Color Scheme...", C_SCHEME, -1, 0, 0, 0 },
    { "Apply &Design...",       C_DESIGN, -1, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Promote\tShift+Tab",    C_PROMOTE, -1, 0, 0, 0 },
    { "&Demote\tTab",           C_DEMOTE,  -1, 0, 0, 0 }
};
static const uoc_item kTools[] = {
    { "&Options...", 0, -1, UOC_DISABLED, 0, 0 }
};
static const uoc_item kShow[] = {
    { "&View Show\tF5",      C_SHOW,  -1, 0, 0, 0 },
    { "Slide &Transition...", C_TRANS, -1, 0, 0, 0 },
    { "&Hide Slide",          C_HIDE,  -1, 0, 0, 0 }
};
static const uoc_item kHelp[] = {
    { "&About UnoShow", C_ABOUT, UOI_HELP, 0, 0, 0 }
};
static const uoc_menu kMenus[] = {
    { "&File", kFile, 5 }, { "&Edit", kEdit, 8 }, { "&View", kView, 6 },
    { "&Insert", kInsert, 3 }, { "F&ormat", kFormat, 6 }, { "&Tools", kTools, 1 },
    { "Slide Sho&w", kShow, 3 }, { "&Help", kHelp, 1 }
};

static const uoc_tbitem kStd[] = {
    { UOC_TB_BUTTON, C_NEW,   UOI_NEW,   "New",   0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_OPEN,  UOI_OPEN,  "Open",  0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_SAVE,  UOI_SAVE,  "Save",  0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_CUT,   UOI_CUT,   "Cut",   0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_COPY,  UOI_COPY,  "Copy",  0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_PASTE, UOI_PASTE, "Paste", 0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_NEWSLIDE, UOI_NEW, "New Slide", 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_LAYOUT,   -1,      "Slide Layout", 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_DESIGN,   -1,      "Apply Design", 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_V_BW,     -1,      "Black and White", 0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_SHOW,     -1,      "Slide Show", 0, 0, 0, 0, 0 }
};
static const uoc_tbitem kFmtBar[] = {
    { UOC_TB_BUTTON, C_ALIGNL, UOI_ALIGN_L, "Align Left",  0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_ALIGNC, UOI_ALIGN_C, "Center",      0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_ALIGNR, UOI_ALIGN_R, "Align Right", 0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_PROMOTE, UOI_INDENT_DEC, "Promote", 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_DEMOTE,  UOI_INDENT_INC, "Demote",  0, 0, 0, 0, 0 }
};
static const uoc_tbar kBars[] = {
    { "Standard", kStd, 14 }, { "Formatting", kFmtBar, 6 }
};

/* ---- the metrics seam over pc64's font engine -------------------------------- */
static int style_of(const uos_chp *c)
{ return (c->bold ? UNO_FS_BOLD : 0) | (c->italic ? UNO_FS_ITALIC : 0); }

/* uno_font_*_styled needs a TTF SLOT, and the system font is slot -1 (the
 * built-in 8x8 bitmap) until the user picks a face - a styled call with -1
 * falls back to fb_text, which has exactly ONE size.  Every run then draws
 * identically: a 44pt title and a 20pt bullet come out the same height, and
 * the full-screen show draws text the size the editor does.  It reads as
 * "the font is a bit small", not as a bug.
 *
 * Nor is "slot 0" the answer: slot 0 is CHICAGO.TTF, a bitmap-style face
 * pinned to a 15px grid, which also ignores the size it is asked for.  So the
 * face is chosen by MEASURING - the first slot whose width answer actually
 * changes with px - and cached.  If none scales, -1 is honest and the text
 * simply is one size. */
static int font_slot(void)
{
    static int cached = -2;
    int s;
    if (cached != -2) return cached;
    s = uno_font_active();
    if (s >= 0 && uno_font_text_w_styled(s, 32, 0, "M") !=
                  uno_font_text_w_styled(s, 12, 0, "M")) { cached = s; return s; }
    for (s = 0; s < 8; s++)
        if (uno_font_text_w_styled(s, 32, 0, "M") !=
            uno_font_text_w_styled(s, 12, 0, "M")) { cached = s; return s; }
    cached = -1;
    return -1;
}

static int mx_w(const char *s, int n, const uos_chp *c, int px, void *ctx)
{
    char b[256];
    int i;
    (void)ctx;
    for (i = 0; i < n && i < 255; i++) b[i] = s[i];
    b[i] = 0;
    return uno_font_text_w_styled(font_slot(), px, style_of(c), b);
}
static int mx_h(const uos_chp *c, int px, void *ctx)
{ (void)c; (void)ctx; return uno_font_height_px(font_slot(), px) + 1; }
static void mx_draw(int x, int y, const char *s, int n, const uos_chp *c,
                    int px, fb_px col, void *ctx)
{
    char b[256];
    int i;
    (void)ctx;
    for (i = 0; i < n && i < 255; i++) b[i] = s[i];
    b[i] = 0;
    uno_font_draw_styled(font_slot(), px, style_of(c),
                         x, y + uno_font_baseline_px(font_slot(), px),
                         b, col, -1);
}
static uos_metrics MET;

/* ---- geometry of the editor ---------------------------------------------------- */
static int sorter_cols(int w) { int c = w / 150; return c < 1 ? 1 : (c > 6 ? 6 : c); }

static void sync_status(void)
{
    char b[16];
    a_cpy(g_statl, "Slide ", (int)sizeof g_statl);
    { int n = a_len(g_statl); a_cpy(g_statl + n, a_num(g_cur + 1, b),
                                    (int)sizeof g_statl - n); }
    { int n = a_len(g_statl); a_cpy(g_statl + n, " of ", (int)sizeof g_statl - n); }
    { int n = a_len(g_statl); a_cpy(g_statl + n, a_num(uos_slides(PR), b),
                                    (int)sizeof g_statl - n); }
    a_cpy(g_statr, uos_get_scheme(PR)->name, (int)sizeof g_statr);
    ST.page = g_statl;
    ST.pos  = g_statr;
}

/* ---- the outline view ------------------------------------------------------------
 * PowerPoint's Outline is the deck's TITLES and BODIES, indented by level, and
 * promoting a line at level 0 makes it a slide of its own.  Drawing it is a
 * walk of the same model the slide view edits - no second store. */
static void draw_outline(int x, int y, int w, int h)
{
    int i, ly = y + 4;
    fb_fill_rect(x, y, w, h, FB_RGB(0xFF, 0xFF, 0xFF));
    for (i = 0; i < uos_slides(PR) && ly < y + h - 12; i++) {
        int t = uos_placeholder(PR, i, UOS_PH_TITLE);
        int b, j, np;
        char num[16];
        if (t < 0) t = uos_placeholder(PR, i, UOS_PH_CTRTITLE);
        fb_text(x + 4, ly, a_num(i + 1, num),
                i == g_cur ? FB_RGB(0, 0, 0x80) : FB_RGB(0x60, 0x60, 0x60), -1);
        if (t >= 0 && uos_text_paras(PR, i, t) > 0) {
            int len = 0;
            const char *s = uos_para_text(PR, i, t, 0, &len);
            char buf[128];
            int k;
            for (k = 0; k < len && k < 127; k++) buf[k] = s[k];
            buf[k] = 0;
            fb_text(x + 26, ly, buf, FB_RGB(0, 0, 0), -1);
        }
        ly += fb_text_h() + 3;
        b = uos_placeholder(PR, i, UOS_PH_BODY);
        if (b < 0) continue;
        np = uos_text_paras(PR, i, b);
        for (j = 0; j < np && ly < y + h - 12; j++) {
            const uos_para *pa = uos_para_at(PR, i, b, j);
            int len = 0;
            const char *s = uos_para_text(PR, i, b, j, &len);
            char buf[128];
            int k;
            for (k = 0; k < len && k < 127; k++) buf[k] = s[k];
            buf[k] = 0;
            fb_text(x + 40 + pa->level * 16, ly, "\x95", FB_RGB(0x40,0x40,0x40), -1);
            fb_text(x + 52 + pa->level * 16, ly, buf, FB_RGB(0x20, 0x20, 0x20), -1);
            ly += fb_text_h() + 2;
        }
        ly += 3;
    }
}

static void draw_sorter(int x, int y, int w, int h)
{
    int cols = sorter_cols(w), i, n = uos_slides(PR);
    int cw = w / cols, chh = cw * UOS_SLIDE_H / UOS_SLIDE_W + 14;
    fb_fill_rect(x, y, w, h, FB_RGB(0x80, 0x80, 0x80));
    for (i = 0; i < n; i++) {
        int cx = x + (i % cols) * cw, cy = y + (i / cols) * chh;
        char num[16];
        if (cy + chh > y + h) break;
        if (i == g_cur) fb_fill_rect(cx + 2, cy + 2, cw - 4, chh - 4, FB_RGB(0, 0, 0x80));
        fb_fill_rect(cx + 6, cy + 6, cw - 12, chh - 22, FB_RGB(0, 0, 0));
        uos_render(PR, i, cx + 7, cy + 7, cw - 14, chh - 24,
                   (g_bw ? UOS_R_BW : 0) | (cw < 110 ? UOS_R_NOTEXT : 0), 0);
        fb_text(cx + 8, cy + chh - 14, a_num(i + 1, num),
                i == g_cur ? FB_RGB(0xFF,0xFF,0xFF) : FB_RGB(0x20,0x20,0x20), -1);
        if (uos_slide_hidden(PR, i))
            fb_text(cx + 26, cy + chh - 14, "hidden", FB_RGB(0xC0,0,0), -1);
    }
}

static void draw_notes(int x, int y, int w, int h)
{
    int sw = w * 3 / 5, sx = x + (w - sw) / 2;
    fb_fill_rect(x, y, w, h, FB_RGB(0x80, 0x80, 0x80));
    fb_fill_rect(sx, y + 8, sw, h - 16, FB_RGB(0xFF, 0xFF, 0xFF));
    uos_render(PR, g_cur, sx + 4, y + 12, sw - 8, (h - 16) / 2 - 8,
               g_bw ? UOS_R_BW : 0, 0);
    fb_frame_rect(sx + 8, y + (h - 16) / 2 + 8, sw - 16, (h - 16) / 2 - 16,
                  FB_RGB(0x80, 0x80, 0x80));
    fb_text(sx + 14, y + (h - 16) / 2 + 14, "Click to add notes",
            FB_RGB(0x80, 0x80, 0x80), -1);
}

/* selection handles, the eight little squares PowerPoint puts round a shape */
static void draw_handles(const uos_shape *sh)
{
    int i, x0, y0, x1, y1, mx, my;
    uos_to_screen(&g_map, sh->x, sh->y, &x0, &y0);
    uos_to_screen(&g_map, sh->x + sh->w, sh->y + sh->h, &x1, &y1);
    mx = (x0 + x1) / 2; my = (y0 + y1) / 2;
    for (i = 0; i < 8; i++) {
        static const signed char kx[8] = { 0, 1, 2, 0, 2, 0, 1, 2 };
        static const signed char ky[8] = { 0, 0, 0, 1, 1, 2, 2, 2 };
        int hx = kx[i] == 0 ? x0 : (kx[i] == 1 ? mx : x1);
        int hy = ky[i] == 0 ? y0 : (ky[i] == 1 ? my : y1);
        fb_fill_rect(hx - 3, hy - 3, 6, 6, FB_RGB(0xFF, 0xFF, 0xFF));
        fb_frame_rect(hx - 3, hy - 3, 6, 6, FB_RGB(0, 0, 0));
    }
}

/* ---- the show ------------------------------------------------------------------
 * Names match the SPEC's transition list; the implementations are all "render
 * the outgoing slide, then render the incoming one through a window". */
enum { TR_NONE = 0, TR_CUT, TR_CUT_BLACK, TR_WIPE_R, TR_WIPE_L, TR_WIPE_D,
       TR_WIPE_U, TR_BOX_IN, TR_BOX_OUT, TR_SPLIT_H, TR_SPLIT_V,
       TR_COVER_R, TR_COVER_L, TR_COVER_D, TR_COVER_U,
       TR_BLINDS_H, TR_BLINDS_V, TR_BARS_H, TR_BARS_V, TR_COUNT };
static const char *const kTransName[TR_COUNT] = {
    "No Transition", "Cut", "Cut Through Black", "Wipe Right", "Wipe Left",
    "Wipe Down", "Wipe Up", "Box In", "Box Out", "Split Horizontal",
    "Split Vertical", "Cover Right", "Cover Left", "Cover Down", "Cover Up",
    "Blinds Horizontal", "Blinds Vertical", "Random Bars Horizontal",
    "Random Bars Vertical"
};

static void show_paint(int x, int y, int w, int h)
{
    uos_map m;
    int flags = g_bw ? UOS_R_BW : 0;
    int prog = g_tprog, i;

    fb_fill_rect(x, y, w, h, FB_RGB(0, 0, 0));
    if (g_show_black) return;

    if (prog >= TRANS_FRAMES || g_trans == TR_NONE || g_trans == TR_CUT) {
        uos_clip_off();
        uos_render(PR, g_cur, x, y, w, h, flags, &m);
        return;
    }

    /* the outgoing slide first, except where the transition goes via black */
    uos_clip_off();
    if (g_trans == TR_CUT_BLACK) {
        if (prog < TRANS_FRAMES / 2) return;      /* black */
        uos_render(PR, g_cur, x, y, w, h, flags, &m);
        return;
    }
    uos_render(PR, g_tfrom, x, y, w, h, flags, &m);

    {
        int sx = m.x, sy = m.y, sw = m.w, sh = m.h;
        int p = prog * 1000 / TRANS_FRAMES;         /* 0..1000 */
        switch (g_trans) {
        case TR_WIPE_R: uos_clip(sx, sy, sw * p / 1000, sh); break;
        case TR_WIPE_L: uos_clip(sx + sw - sw * p / 1000, sy, sw * p / 1000, sh); break;
        case TR_WIPE_D: uos_clip(sx, sy, sw, sh * p / 1000); break;
        case TR_WIPE_U: uos_clip(sx, sy + sh - sh * p / 1000, sw, sh * p / 1000); break;
        case TR_BOX_IN: uos_clip(sx + sw * (1000 - p) / 2000, sy + sh * (1000 - p) / 2000,
                                 sw * p / 1000, sh * p / 1000); break;
        case TR_BOX_OUT: {
            int iw = sw * (1000 - p) / 1000, ih = sh * (1000 - p) / 1000;
            uos_clip(sx, sy, sw, sh);
            uos_render(PR, g_cur, x, y, w, h, flags, 0);
            uos_clip(sx + (sw - iw) / 2, sy + (sh - ih) / 2, iw, ih);
            uos_render(PR, g_tfrom, x, y, w, h, flags, 0);
            uos_clip_off();
            return;
        }
        case TR_SPLIT_H:
            uos_clip(sx, sy + sh / 2 - sh * p / 2000, sw, sh * p / 1000); break;
        case TR_SPLIT_V:
            uos_clip(sx + sw / 2 - sw * p / 2000, sy, sw * p / 1000, sh); break;
        case TR_COVER_R: case TR_COVER_L: case TR_COVER_D: case TR_COVER_U: {
            int dx = 0, dy = 0;
            if (g_trans == TR_COVER_R) dx = -sw + sw * p / 1000;
            if (g_trans == TR_COVER_L) dx =  sw - sw * p / 1000;
            if (g_trans == TR_COVER_D) dy = -sh + sh * p / 1000;
            if (g_trans == TR_COVER_U) dy =  sh - sh * p / 1000;
            uos_clip(sx, sy, sw, sh);
            uos_render(PR, g_cur, x + dx, y + dy, w, h, flags, 0);
            uos_clip_off();
            return;
        }
        case TR_BLINDS_H: case TR_BARS_H: {
            int bands = g_trans == TR_BLINDS_H ? 8 : 12;
            for (i = 0; i < bands; i++) {
                int bh = sh / bands;
                uos_clip(sx, sy + i * bh, sw, bh * p / 1000 + 1);
                uos_render(PR, g_cur, x, y, w, h, flags, 0);
            }
            uos_clip_off();
            return;
        }
        case TR_BLINDS_V: case TR_BARS_V: {
            int bands = g_trans == TR_BLINDS_V ? 8 : 12;
            for (i = 0; i < bands; i++) {
                int bwd = sw / bands;
                uos_clip(sx + i * bwd, sy, bwd * p / 1000 + 1, sh);
                uos_render(PR, g_cur, x, y, w, h, flags, 0);
            }
            uos_clip_off();
            return;
        }
        default: uos_clip(sx, sy, sw, sh); break;
        }
        uos_render(PR, g_cur, x, y, w, h, flags, 0);
        uos_clip_off();
    }
}

static void show_goto(int to)
{
    int n = uos_slides(PR);
    int guard = 0;
    if (n <= 0) return;
    while (to >= 0 && to < n && uos_slide_hidden(PR, to) && guard++ < n)
        to += (to > g_cur) ? 1 : -1;             /* hidden slides are skipped */
    if (to < 0 || to >= n) { /* off the end: end the show */
        g_show = 0;
        pc64_shell_fullscreen(0);
        pc64_shell_dirty();
        return;
    }
    g_tfrom = g_cur;
    g_cur = to;
    g_tprog = 0;
    pc64_shell_dirty();
}

/* ---- painting -------------------------------------------------------------------- */
static void app_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    int top, bh, ch;
    (void)w; (void)ctx;
    g_rect = r;
    g_have_rect = 1;

    if (g_show) { show_paint(r.x, r.y, r.w, r.h); return; }

    sync_status();
    CH.x = r.x; CH.y = r.y; CH.w = r.w; CH.h = r.h;
    uoc_render_bars(&CH);
    top = r.y + uoc_height(&CH);
    bh  = uob_status_h();
    ch  = r.h - (top - r.y) - bh;
    if (ch < 16) ch = 16;

    if (g_view == 1)      draw_outline(r.x, top, r.w, ch);
    else if (g_view == 2) draw_sorter(r.x, top, r.w, ch);
    else if (g_view == 3) draw_notes(r.x, top, r.w, ch);
    else {
        fb_fill_rect(r.x, top, r.w, ch, FB_RGB(0x80, 0x80, 0x80));
        uos_render(PR, g_cur, r.x + 8, top + 6, r.w - 16, ch - 12,
                   UOS_R_PHFRAMES | (g_bw ? UOS_R_BW : 0), &g_map);
        if (g_sel >= 0) {
            const uos_shape *sh = uos_shape_at_c(PR, g_cur, g_sel);
            if (sh) draw_handles(sh);
        }
    }
    uob_status_render(&ST, r.x, r.y + r.h - bh, r.w);
    uoc_render_popups(&CH);            /* an open menu goes OVER the slide */
    if (g_dlg) uod_render(&DL);
}

/* ---- the dialogs --------------------------------------------------------------
 * Three list pickers over one engine, which is the phase 6c claim used: New
 * Slide, Slide Layout and Apply Design differ only in what fills the list. */
enum { ID_LIST = 1 };
static const char *g_items[UOS_AL_COUNT];
static uod_item    g_dlgitems[3];
static uod_dlg     g_dlgdef;

static void open_picker(int which)
{
    int i, n = 0;
    const char *title = "New Slide";
    if (which == C_DESIGN || which == C_SCHEME) {
        n = uos_schemes();
        for (i = 0; i < n; i++) g_items[i] = uos_scheme_at(i)->name;
        title = which == C_DESIGN ? "Apply Design" : "Color Scheme";
    } else if (which == C_TRANS) {
        n = TR_COUNT;
        for (i = 0; i < n; i++) g_items[i] = kTransName[i];
        title = "Slide Transition";
    } else if (which == C_SHAPE) {
        n = UOS_G_COUNT;
        for (i = 0; i < n; i++) g_items[i] = uos_geom_name(i);
        title = "AutoShape";
    } else {
        n = UOS_AL_COUNT;
        for (i = 0; i < n; i++) g_items[i] = uos_layout_name(i);
        title = which == C_LAYOUT ? "Slide Layout" : "New Slide";
    }
    { int k; for (k = 0; k < (int)sizeof g_dlgitems[0]; k++)
        ((char *)&g_dlgitems[0])[k] = 0; }
    g_dlgitems[0].kind = UOD_LIST;
    g_dlgitems[0].id = ID_LIST;
    g_dlgitems[0].x = 10; g_dlgitems[0].y = 10;
    g_dlgitems[0].w = 200; g_dlgitems[0].h = 120;
    g_dlgitems[0].page = -1;
    g_dlgitems[0].list = g_items;
    g_dlgitems[0].nlist = n;
    /* The OK/Cancel row is NOT automatic - a dialog declares its own buttons
     * (uoword's Font dialog does the same).  A picker with no buttons opens,
     * looks finished, and cannot be dismissed by anything but Esc. */
    { int k; for (k = 0; k < 2 * (int)sizeof g_dlgitems[0]; k++)
        ((char *)&g_dlgitems[1])[k] = 0; }
    g_dlgitems[1].kind = UOD_BUTTON; g_dlgitems[1].id = UOD_ID_OK;
    g_dlgitems[1].text = "OK";
    g_dlgitems[1].x = 56; g_dlgitems[1].y = 140;
    g_dlgitems[1].w = 60; g_dlgitems[1].h = 20;
    g_dlgitems[1].page = -1; g_dlgitems[1].flags = UOD_DEFAULT;
    g_dlgitems[2].kind = UOD_BUTTON; g_dlgitems[2].id = UOD_ID_CANCEL;
    g_dlgitems[2].text = "Cancel";
    g_dlgitems[2].x = 126; g_dlgitems[2].y = 140;
    g_dlgitems[2].w = 60; g_dlgitems[2].h = 20;
    g_dlgitems[2].page = -1;

    g_dlgdef.title = title;
    g_dlgdef.item = g_dlgitems;
    g_dlgdef.n = 3;
    g_dlgdef.tab = 0; g_dlgdef.ntab = 0;
    g_dlgdef.w = 226; g_dlgdef.h = 178;
    g_dlgdef.help = 0;
    /* uod_open CENTRES the dialog in a frame - it takes the frame's SIZE,
     * not a position.  Passing a top-left corner puts the dialog at a
     * quarter of the screen and its list somewhere else again. */
    uod_open(&DL, &g_dlgdef, pc64_shell_workarea_w(), pc64_shell_workarea_h());
    g_dlg = which;
}

static void dialog_closed(void)
{
    int res = uod_result(&DL), which = g_dlg, pick = uod_value(&DL, ID_LIST);
    g_dlg = 0;
    if (res != UOD_ID_OK) return;
    switch (which) {
    case C_OPEN:
        a_cpy(g_file, uof_name(), (int)sizeof g_file);
        a_cpy(g_name, g_file, (int)sizeof g_name);
        if (!load_pres(uof_volume(), g_file)) {
            g_file[0] = 0;
            uod_msgbox(&DL, "UnoShow",
                       "That is not a presentation this build reads.",
                       UOD_MB_OK, pc64_shell_workarea_w(),
                       pc64_shell_workarea_h());
            g_dlg = C_ABOUT;              /* a message box, nothing to act on */
        }
        break;
    case C_SAVE:
        a_cpy(g_file, uof_name(), (int)sizeof g_file);
        ensure_ext(g_file, (int)sizeof g_file, uof_type());
        a_cpy(g_name, g_file, (int)sizeof g_name);
        if (!save_pres(uof_volume(), g_file)) {
            uod_msgbox(&DL, "UnoShow", "Could not write the presentation.",
                       UOD_MB_OK, pc64_shell_workarea_w(),
                       pc64_shell_workarea_h());
            g_dlg = C_ABOUT;
        }
        break;
    case C_NEWSLIDE:
        g_cur = uos_slide_insert(PR, g_cur + 1, pick);
        g_sel = -1;
        break;
    case C_LAYOUT:  uos_slide_set_layout(PR, g_cur, pick); break;
    case C_DESIGN:
    case C_SCHEME:  uos_set_scheme(PR, uos_scheme_at(pick)); break;
    case C_TRANS:   g_trans = pick; break;
    case C_SHAPE: {
        int z = uos_shape_add(PR, g_cur, pick, 240, 200, 200, 150);
        if (z >= 0) g_sel = z;
        break;
    }
    default: break;
    }
    pc64_shell_dirty();
}

/* ---- .ppt, in and out --------------------------------------------------------
 * unodoc writes a slide as a TITLE frame and a BODY frame with '\n' between
 * paragraphs, and reads a slide back as its text with the same separator.
 * So the mapping is the obvious one and, more to the point, the SYMMETRIC
 * one: the first paragraph is the title, the rest are the body, and what
 * save writes open reads back. Shapes drawn by hand do not survive a
 * round-trip - unodoc's writer is title-and-body only, which the Save path
 * says out loud rather than dropping them quietly. */
static long io_read(void *ctx, long off, unsigned char *dst, long n)
{
    long i;
    unsigned char *d = (unsigned char *)dst;
    (void)ctx;
    if (off < 0 || off >= g_iolen) return 0;
    if (off + n > g_iolen) n = g_iolen - off;
    for (i = 0; i < n; i++) d[i] = g_io[off + i];
    return n;
}

#define UOS_IOCAP (4L * 1024 * 1024)

/* Split `text` at the first newline: the head becomes the title, the tail
 * (which may itself hold newlines) becomes the body. */
static void put_slide_text(int slide, const char *text)
{
    char head[256];
    int i = 0, t, b;
    while (text[i] && text[i] != '\n' && i < (int)sizeof head - 1) { head[i] = text[i]; i++; }
    head[i] = 0;
    t = uos_placeholder(PR, slide, UOS_PH_TITLE);
    if (t < 0) t = uos_placeholder(PR, slide, UOS_PH_CTRTITLE);
    if (t >= 0 && head[0]) uos_text_set(PR, slide, t, head);
    if (!text[i]) return;
    b = uos_placeholder(PR, slide, UOS_PH_BODY);
    if (b >= 0) uos_text_set(PR, slide, b, text + i + 1);
}

static int load_pres(int vol, const char *name)
{
    ud_cfb *c = 0;
    ud_zip *z = 0;
    ud_ppt *p = 0;
    ud_src  src;
    long sz;
    int ok = 0;

    ud_set_alloc(malloc, free);
    /* A .pptx part is a DEFLATE stream that unodoc inflates with unomedia's
     * decompressor, which needs an allocator of its own.  See UNODOC.md: this
     * is the one extra obligation the OOXML path puts on a caller. */
    um_set_alloc(malloc, free);
    sz = uno_fs_size(vol, name);
    if (sz <= 0 || sz > UOS_IOCAP) return 0;
    if (!g_io) g_io = (unsigned char *)malloc(UOS_IOCAP);
    if (!g_io) return 0;
    g_iolen = uno_fs_read(vol, name, g_io, sz);
    if (g_iolen <= 0) return 0;

    src.read = io_read; src.size = g_iolen; src.ctx = 0;
    /* The CONTAINER decides which reader, not the extension.  Both hand back
     * the same ud_ppt, so everything below is shared. */
    if (ud_sniff(&src) == UD_C_ZIP) {
        z = ud_zip_open(&src);
        p = z ? ud_pptx_open(z) : 0;
    } else {
        c = ud_cfb_open(&src);
        p = c ? ud_ppt_open(c) : 0;
    }
    if (p) {
        int n = ud_ppt_slides(p), i;
        PR = uos_new();
        for (i = 0; i < n; i++) {
            const char *t = ud_ppt_slide_text(p, i);
            /* unodoc's .ppt writer has no layout concept - a slide is a
             * title frame and a body frame - so everything comes back as a
             * Bulleted List, including a deck that was saved from a Title
             * Slide.  Re-laying out slide 0 drops the title layout's now
             * empty holders (uos_model.c does that part). */
            int sl = (i == 0) ? 0 : uos_slide_add(PR, UOS_AL_BULLETS);
            if (sl < 0) break;
            if (i == 0) uos_slide_set_layout(PR, 0, UOS_AL_BULLETS);
            if (t && *t) put_slide_text(sl, t);
        }
        ud_ppt_close(p);
        ok = 1;
    }
    ud_cfb_close(c);
    ud_zip_close(z);
    g_cur = 0; g_sel = -1; g_editing = 0;
    return ok;
}

/* Join a placeholder's paragraphs back into one '\n'-separated string. */
static int gather(int slide, int role, char *out, int cap)
{
    int z = uos_placeholder(PR, slide, role), i, n, k = 0;
    out[0] = 0;
    if (z < 0) return 0;
    n = uos_text_paras(PR, slide, z);
    for (i = 0; i < n && k < cap - 2; i++) {
        int len = 0, j;
        const char *t = uos_para_text(PR, slide, z, i, &len);
        if (i && k < cap - 2) out[k++] = '\n';
        for (j = 0; j < len && k < cap - 2; j++) out[k++] = t[j];
    }
    out[k] = 0;
    return k;
}

static int save_pres(int vol, const char *name)
{
    ud_pptw *w;
    unsigned char *out = 0;
    long len = 0;
    int ok = 0, i, n;
    static char buf[2048];

    if (!PR) return 0;
    ud_set_alloc(malloc, free);
    um_set_alloc(malloc, free);
    w = ud_pptw_new();
    if (!w) return 0;
    n = uos_slides(PR);
    for (i = 0; i < n; i++) {
        int sl = ud_pptw_slide(w);
        if (sl < 0) break;
        if (!gather(i, UOS_PH_TITLE, buf, (int)sizeof buf))
            gather(i, UOS_PH_CTRTITLE, buf, (int)sizeof buf);
        if (buf[0]) ud_pptw_title(w, sl, buf);
        if (!gather(i, UOS_PH_BODY, buf, (int)sizeof buf))
            gather(i, UOS_PH_SUBTITLE, buf, (int)sizeof buf);
        if (buf[0]) ud_pptw_body(w, sl, buf);
    }
    out = name_is_ooxml(name, "pptx") ? ud_pptxw_save(w, &len)
                                     : ud_pptw_save(w, &len);
    if (out && len > 0) ok = uno_fs_write(vol, name, out, len) ? 1 : 0;
    ud_free(out);
    ud_pptw_free(w);
    if (ok) uos_set_dirty(PR, 0);
    return ok;
}

/* ---- editing ---------------------------------------------------------------------
 * The model is a store, not an editor, so the app keeps the selected body as
 * one buffer with '\n' between paragraphs and writes it back on every change.
 * Levels do not survive a rewrite, so they are kept here and re-applied - the
 * same trick uocalc's edit line uses, and the reason uos_text_set can stay a
 * one-line replace. */
static void edit_load(void)
{
    int n = uos_text_paras(PR, g_cur, g_sel), i, k = 0;
    g_edit[0] = 0;
    g_nlvl = 0;
    for (i = 0; i < n && k < (int)sizeof g_edit - 2; i++) {
        int len = 0, j;
        const char *s = uos_para_text(PR, g_cur, g_sel, i, &len);
        if (i) g_edit[k++] = '\n';
        for (j = 0; j < len && k < (int)sizeof g_edit - 2; j++) g_edit[k++] = s[j];
        if (g_nlvl < 24) g_lvl[g_nlvl++] = uos_para_at(PR, g_cur, g_sel, i)->level;
    }
    g_edit[k] = 0;
}
static void edit_flush(void)
{
    int i;
    if (g_sel < 0) return;
    uos_text_set(PR, g_cur, g_sel, g_edit);
    for (i = 0; i < uos_text_paras(PR, g_cur, g_sel) && i < g_nlvl; i++)
        uos_para_set_level(PR, g_cur, g_sel, i, g_lvl[i]);
    pc64_shell_dirty();
}
static int edit_para_index(void)
{
    int i, n = 0;
    for (i = 0; g_edit[i]; i++) if (g_edit[i] == '\n') n++;
    return n;                               /* the caret is always at the end */
}

/* ---- commands ---------------------------------------------------------------------- */
static void do_command(int cmd)
{
    switch (cmd) {
    case C_NEW: PR = uos_new(); g_cur = 0; g_sel = -1; g_editing = 0;
                g_file[0] = 0;
                a_cpy(g_name, "Presentation1", (int)sizeof g_name); break;
    case C_EXIT: break;
    case C_OPEN:
        uof_set_fs(&kFs);
        uof_open(&DL, 0, kTypes, 2, pc64_shell_workarea_w(),
                 pc64_shell_workarea_h());
        g_dlg = C_OPEN;
        return;
    case C_SAVE:
        if (g_file[0]) {
            if (!save_pres(0, g_file)) {
                uod_msgbox(&DL, "UnoShow", "Could not write the presentation.",
                           UOD_MB_OK, pc64_shell_workarea_w(),
                           pc64_shell_workarea_h());
                g_dlg = C_SAVE;
            }
            break;
        }
        uof_set_fs(&kFs);
        uof_open(&DL, 1, kTypes, 2, pc64_shell_workarea_w(),
                 pc64_shell_workarea_h());
        g_dlg = C_SAVE;
        return;
    case C_V_SLIDE:   g_view = 0; break;
    case C_V_OUTLINE: g_view = 1; break;
    case C_V_SORTER:  g_view = 2; break;
    case C_V_NOTES:   g_view = 3; break;
    case C_V_BW:      g_bw = !g_bw; break;
    case C_NEWSLIDE: case C_LAYOUT: case C_DESIGN: case C_SCHEME:
    case C_TRANS: case C_SHAPE:
        open_picker(cmd); return;
    case C_TEXTBOX: {
        int z = uos_shape_add(PR, g_cur, UOS_G_RECT, 240, 220, 240, 60);
        if (z >= 0) {
            uos_shape *sh = uos_shape_at(PR, g_cur, z);
            sh->fill.kind = UOS_F_NONE;
            sh->line.kind = UOS_L_NONE;
            uos_text_set(PR, g_cur, z, "Text");
            g_sel = z; g_editing = 1; edit_load();
        }
        break;
    }
    case C_DELSLIDE:
        if (uos_slides(PR) > 1) {
            uos_slide_delete(PR, g_cur);
            if (g_cur >= uos_slides(PR)) g_cur = uos_slides(PR) - 1;
            g_sel = -1;
        }
        break;
    case C_DUP:
        if (g_sel >= 0) {
            const uos_shape *src = uos_shape_at_c(PR, g_cur, g_sel);
            int z = uos_shape_add(PR, g_cur, src->geom, src->x + 20, src->y + 20,
                                  src->w, src->h);
            if (z >= 0) {
                uos_shape *d = uos_shape_at(PR, g_cur, z);
                uos_fill f = src->fill; uos_line l = src->line;
                d->fill = f; d->line = l; d->shadow = src->shadow;
                g_sel = z;
            }
        }
        break;
    case C_HIDE: uos_slide_hide(PR, g_cur, !uos_slide_hidden(PR, g_cur)); break;
    case C_SHOW:
        /* View Show starts at slide ONE, not at whatever you were editing -
         * PowerPoint's Shift+F5 is the "from current slide" one. */
        g_show = 1; g_show_black = 0; g_tprog = TRANS_FRAMES; g_editing = 0;
        g_cur = 0;
        while (g_cur < uos_slides(PR) && uos_slide_hidden(PR, g_cur)) g_cur++;
        if (g_cur >= uos_slides(PR)) g_cur = 0;
        g_tfrom = g_cur;
        if (g_win) pc64_shell_fullscreen(g_win);
        break;
    case C_PROMOTE: case C_DEMOTE:
        if (g_sel >= 0 && g_nlvl > 0) {
            int i = edit_para_index();
            if (i >= g_nlvl) i = g_nlvl - 1;
            if (cmd == C_DEMOTE && g_lvl[i] < UOS_MAXLEVEL - 1) g_lvl[i]++;
            if (cmd == C_PROMOTE && g_lvl[i] > 0) g_lvl[i]--;
            edit_flush();
        }
        break;
    case C_ALIGNL: case C_ALIGNC: case C_ALIGNR:
        if (g_sel >= 0) {
            int i, n = uos_text_paras(PR, g_cur, g_sel);
            for (i = 0; i < n; i++)
                uos_para_at(PR, g_cur, g_sel, i)->align =
                    (unsigned char)(cmd == C_ALIGNL ? UOS_AL_LEFT :
                                    cmd == C_ALIGNC ? UOS_AL_CENTER : UOS_AL_RIGHT);
        }
        break;
    case C_ABOUT:
        uod_msgbox(&DL, "About UnoShow",
                   "UnoShow - a Microsoft PowerPoint 97 clone for UnoDOS.",
                   UOD_MB_OK, pc64_shell_workarea_w(), pc64_shell_workarea_h());
        g_dlg = C_ABOUT;
        return;
    default: break;
    }
    pc64_shell_dirty();
}

/* ---- events ------------------------------------------------------------------------ */
static int app_event(struct unoui_widget *w, const void *evp, void *ctx)
{
    const unoui_event *e = (const unoui_event *)evp;
    unoui_rect r;
    int cmd = 0;
    (void)w; (void)ctx;
    if (!g_have_rect) return 0;
    r = g_rect;

    if (g_show) {
        if (e->kind == UI_EV_MOUSE_DOWN) { show_goto(g_cur + 1); return 1; }
        return 0;
    }
    if (g_dlg) {
        if (uod_handle(&DL, e)) {
            if (g_dlg == C_OPEN || g_dlg == C_SAVE) uof_sync(&DL);
            if (!uod_is_open(&DL)) dialog_closed();
            pc64_shell_dirty();
            return 1;
        }
        return 1;                       /* modal: swallow everything else */
    }

    CH.x = r.x; CH.y = r.y; CH.w = r.w; CH.h = r.h;
    if (uoc_handle(&CH, e, &cmd)) {
        if (cmd) do_command(cmd);
        pc64_shell_dirty();
        return 1;
    }

    if (e->kind == UI_EV_MOUSE_DOWN) {
        int top = r.y + uoc_height(&CH);
        int ch = r.h - (top - r.y) - uob_status_h();
        if (g_view == 2) {                       /* the sorter selects slides */
            int cols = sorter_cols(r.w), cw = r.w / cols;
            int chh = cw * UOS_SLIDE_H / UOS_SLIDE_W + 14;
            int c = (e->x - r.x) / cw, rw = (e->y - top) / chh;
            int i = rw * cols + c;
            if (e->y >= top && i >= 0 && i < uos_slides(PR)) {
                g_cur = i; g_sel = -1;
                pc64_shell_dirty();
            }
            return 1;
        }
        if (g_view == 0 && e->y >= top && e->y < top + ch) {
            int px = 0, py = 0, hit;
            uos_to_slide(&g_map, e->x, e->y, &px, &py);
            hit = uos_hit(PR, g_cur, px, py);
            if (hit != g_sel) { g_editing = 0; g_sel = hit; }
            else if (hit >= 0) { g_editing = 1; edit_load(); }
            pc64_shell_dirty();
            return 1;
        }
    }
    return 0;
}

static int uw_key(int uni, int scan, int ctrl)
{
    unoui_event e;
    int i;
    if (!PR) return 0;

    if (g_show) {
        if (uni == 27) {                       /* Esc ends the show          */
            g_show = 0; pc64_shell_fullscreen(0); pc64_shell_dirty(); return 1;
        }
        if (uni == 'b' || uni == 'B') { g_show_black = !g_show_black;
                                        pc64_shell_dirty(); return 1; }
        /* Scan codes here are the FIRMWARE's (UEFI SimpleTextIn), not PS/2
         * set 1: Up 0x01, Down 0x02, Right 0x03, Left 0x04, PageUp 0x09,
         * PageDown 0x0A, F5 0x0F, Esc 0x17.  A PS/2 table looks plausible
         * and every key silently does nothing. */
        if (uni == ' ' || uni == 13 || scan == 0x03 || scan == 0x0A)
            { show_goto(g_cur + 1); return 1; }
        if (scan == 0x04 || scan == 0x09 || uni == 8)
            { show_goto(g_cur - 1); return 1; }
        return 1;                              /* the show swallows the rest */
    }
    if (g_dlg) {
        for (i = 0; i < (int)sizeof e; i++) ((char *)&e)[i] = 0;
        if (uni >= ' ')                       { e.kind = UI_EV_CHAR; e.ch = uni; }
        else if (uni == '\r' || uni == '\n') { e.kind = UI_EV_KEY; e.key = UI_KEY_ENTER; }
        else if (uni == 27)                   { e.kind = UI_EV_KEY; e.key = UI_KEY_ESC; }
        else if (uni == '\t')                { e.kind = UI_EV_KEY; e.key = UI_KEY_TAB; }
        /* Backspace is a CHAR to uod_handle (uoffice/uodlg.c), not a key - and
         * without this clause it died in the `else return 1` below, so a wrong
         * File-name could never be corrected and Open was unreachable on any
         * volume whose row 0 was not the wanted file.  UnoWord's bridge
         * (apps/uoword.c) has always forwarded it; this is the same line. */
        else if (uni == 8)                    { e.kind = UI_EV_CHAR; e.ch = 8; }
        else if (scan == 0x02)                { e.kind = UI_EV_KEY; e.key = UI_KEY_DOWN; }
        else if (scan == 0x01)                { e.kind = UI_EV_KEY; e.key = UI_KEY_UP; }
        else return 1;
        uod_handle(&DL, &e);
        if (g_dlg == C_OPEN || g_dlg == C_SAVE) uof_sync(&DL);
        if (!uod_is_open(&DL)) dialog_closed();
        pc64_shell_dirty();
        return 1;
    }
    if (scan == 0x0F) { do_command(C_SHOW); return 1; }     /* F5 */
    if (ctrl && (uni == 'm' || uni == 'M')) { do_command(C_NEWSLIDE); return 1; }
    if (ctrl && (uni == 'n' || uni == 'N')) { do_command(C_NEW); return 1; }

    /* PageDown / PageUp / arrows move between slides when nothing is being
     * typed into - the same keys the show uses, which is what people expect */
    if (!g_editing) {
        if (scan == 0x03 || scan == 0x0A) {          /* Right / PageDown */
            if (g_cur + 1 < uos_slides(PR)) { g_cur++; g_sel = -1; pc64_shell_dirty(); }
            return 1;
        }
        if (scan == 0x04 || scan == 0x09) {          /* Left / PageUp */
            if (g_cur > 0) { g_cur--; g_sel = -1; pc64_shell_dirty(); }
            return 1;
        }
    }

    if (g_editing && g_sel >= 0) {
        int n = a_len(g_edit);
        if (uni == 27) { g_editing = 0; pc64_shell_dirty(); return 1; }
        if (uni == 9) { do_command(ctrl ? C_PROMOTE : C_DEMOTE); return 1; }
        if (uni == 8) {
            if (n > 0) { g_edit[n - 1] = 0; edit_flush(); }
            return 1;
        }
        if (uni == 13) {
            if (n < (int)sizeof g_edit - 2) {
                g_edit[n] = '\n'; g_edit[n + 1] = 0;
                if (g_nlvl < 24) { g_lvl[g_nlvl] = g_nlvl ? g_lvl[g_nlvl - 1] : 0;
                                   g_nlvl++; }
                edit_flush();
            }
            return 1;
        }
        if (uni >= ' ' && n < (int)sizeof g_edit - 2) {
            g_edit[n] = (char)uni; g_edit[n + 1] = 0;
            if (!g_nlvl) { g_lvl[0] = 0; g_nlvl = 1; }
            edit_flush();
            return 1;
        }
    }
    return 0;
}

static void uw_frame(void)
{
    if (g_show && g_tprog < TRANS_FRAMES) { g_tprog++; pc64_shell_dirty(); }
}

static void uw_build(unoui_window *win)
{
    int w = pc64_shell_workarea_w() - 40, h = pc64_shell_workarea_h() - 60;
    if (w < 320) w = 320;
    if (h < 240) h = 240;
    unoui_window_init(win, "UnoShow - Presentation1", 20, 16, w, h);
    g_canvas.draw = app_draw;
    g_canvas.event = app_event;
    g_canvas.ctx = 0;
    unoui_widget_fill(unoui_add_canvas(win, 0, 0, w - 12, h - 28, &g_canvas));
    win->flags |= UI_WIN_RESIZE;
    g_win = win;
    g_cidx = 0;
}
static int  uw_action(const unoui_action *a) { (void)a; return 0; }
static int  uw_canvas_index(void) { return g_cidx; }
static void uw_closed(void)
{
    if (g_show) { g_show = 0; pc64_shell_fullscreen(0); }
}
static void uw_opened(void)
{
    if (!PR) PR = uos_new();
    uoc_icons_install();
    uoc_init(&CH, kMenus, 8, kBars, 2, 0, 0, 400, 300);
    MET.text_w = mx_w; MET.height = mx_h; MET.draw = mx_draw; MET.ctx = 0;
    uos_set_metrics(&MET);
    uof_set_fs(&kFs);
    ST.page = "Slide 1 of 1";
    ST.pos  = "Default";
    sync_status();
}

/* what the shell shows for this app, carried in the module (uno_appdesc.h) */
UNO_APP_DESC("id: uoshow\n"
             "name: UnoShow\n"
             "icon: uoshow\n"
             "cat: tools\n"
             "rank: 40\n");

static const UnoUuiApp kApp = {
    UNO_UUIAPP_ABI, "UnoShow",
    uw_build, uw_action, uw_key, uw_frame, uw_opened, uw_closed,
    uw_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved)
{ (void)reserved; return &kApp; }
