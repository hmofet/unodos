/* ===========================================================================
 * uoword.c - UnoWord, the word processor (OFFICE97-PLAN §5 phase 8).
 *
 * A unoui-CLASS .UNO module (APPS\UOWORD.UNO), hosted exactly as Studio and
 * Photos are: the shell hands it a window, it fills it with ONE canvas, and
 * everything inside that canvas - the menu bar, the toolbars, the ruler, the
 * page, the status bar, any dialog - is drawn by the uoffice lane.  That is
 * the same arrangement UnoAmp uses for Winamp's window, and it is why nothing
 * in this app needed unoui to grow a feature.
 *
 * The whole uoffice lane plus unodoc's Word half link INTO the module, the
 * PHOTOS pattern: the kernel gains no document code, just the exports the
 * module imports by name.
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
#include "uoword.h"
#include "unodoc.h"
#include "pc64_font.h"

/* the shell's services this module imports by name */
void  pc64_shell_dirty(void);
int   uno_font_count(void);
const char *uno_font_name(int slot);
int   pc64_shell_workarea_w(void);
int   pc64_shell_workarea_h(void);
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

/* ---- commands -------------------------------------------------------------- */
enum {
    C_NEW = 1, C_OPEN, C_SAVE, C_SAVEAS, C_CLOSE, C_EXIT, C_PRINT,
    C_UNDO = 20, C_REDO, C_CUT, C_COPY, C_PASTE, C_SELALL,
    C_NORMAL = 40, C_PAGELAYOUT, C_RULER, C_ZOOM,
    C_FONT = 60, C_PARA, C_BULLETS, C_NUMBER,
    C_BOLD = 80, C_ITALIC, C_UNDER, C_LEFT, C_CENTER, C_RIGHT, C_JUSTIFY,
    C_STYLE = 100, C_FONTNAME, C_FONTSIZE, C_COLOR,
    C_ABOUT = 120, C_WORDCOUNT
};

/* ---- the app's state ------------------------------------------------------- */
static uoc_ui      CH;
static uod_ui      DL;
static uob_status  ST;
static uob_ruler   RU;
static uow_doc    *DOC;
static uow_layout *LAY;
static uow_metrics MET;
static unoui_window *g_win;
static unoui_canvas  g_canvas;
static int  g_cidx = -1;
static long g_caret, g_anchor;
static int  g_dragging;      /* between mouse-down and mouse-up in the page */
static int  g_scroll;
static int  g_zoom = 100;
static int  g_dirty_layout = 1;
static int  g_dlg_kind;              /* which dialog is up: 0 none           */
static char g_name[40] = "Document1";
static char g_status_l[64], g_status_r[64];
static int  g_showruler = 1;

/* THE CANVAS RECT, TAKEN FROM THE PAINTER.
 *
 * A widget's own w->r is relative to the window's CONTENT origin, not to its
 * frame, so reconstructing screen coordinates as (window.x + widget.x) is
 * short by the frame width and the whole title bar - about twenty pixels in
 * y.  That is enough to push a click on the menu bar into the toolbar row
 * while a click in the document still lands somewhere plausible, which is
 * exactly how it presented: the caret moved, and no menu ever opened.
 *
 * unoui_content_origin() would give the right answer, but the stronger fix is
 * to have only ONE answer: the rect uoc_render() painted into is the rect
 * uoc_handle() must hit-test against, so the painter records it and the event
 * path reads it back.  This is the same rule the rest of the lane is built on
 * (uochrome.c's header, rule 1) and this is what breaking it looks like. */
static unoui_rect g_rect;
static int        g_have_rect;

enum { DLG_NONE = 0, DLG_OPEN, DLG_SAVE, DLG_FONT, DLG_MSG };

static int a_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void a_cpy(char *d, const char *s, int cap)
{ int i = 0; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static char *a_num(long v, char *b)
{
    int n = 0, i = 0, dg[12];
    if (v < 0) { b[n++] = '-'; v = -v; }
    do { dg[i++] = (int)(v % 10); v /= 10; } while (v && i < 12);
    while (i) b[n++] = (char)('0' + dg[--i]);
    b[n] = 0;
    return b;
}

/* The Font combo's list.  It cannot be a literal: which faces exist is a
 * property of the machine (they are TTFs read off the ESP), so the array is
 * filled at startup from uno_font_count/uno_font_name and the toolbar table
 * just points at it.  Entry 0 is the document default, which is what a
 * character run with face == 0 means. */
#define MAXFACE 8
static const char *g_faces[MAXFACE + 1];
static int         g_nface = 1;

/* ---- the metrics seam, over pc64's kerned TTF ----------------------------- */
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

/* A run's face, resolved to a pc64 font slot.  face 0 is "the document
 * default" - whatever font_slot() found - and face n is slot n-1, so the
 * model carries an index that survives a machine with a different font set
 * rather than a raw slot number. */
static int slot_of(const uow_chp *c)
{
    if (c && c->face > 0 && (int)c->face <= g_nface - 1) return (int)c->face - 1;
    return font_slot();
}

static int px_of(const uow_chp *c)
{
    int px = (c->size ? c->size : 20) / 2;      /* half-points -> points     */
    px = px * g_zoom / 100;
    if (px < 8) px = 8;
    if (px > 40) px = 40;                       /* uno_font's own clamp      */
    return px;
}
static int style_of(const uow_chp *c)
{ return (c->bold ? UNO_FS_BOLD : 0) | (c->italic ? UNO_FS_ITALIC : 0); }

static int m_text_w(const char *s, long n, const uow_chp *c, void *ctx)
{
    char b[256];
    long i;
    (void)ctx;
    for (i = 0; i < n && i < 255; i++) b[i] = s[i];
    b[i] = 0;
    return uno_font_text_w_styled(slot_of(c), px_of(c), style_of(c), b);
}
static int m_height(const uow_chp *c, void *ctx)
{ (void)ctx; return uno_font_height_px(slot_of(c), px_of(c)) + 1; }
static int m_baseline(const uow_chp *c, void *ctx)
{ (void)ctx; return uno_font_baseline_px(slot_of(c), px_of(c)); }
static int m_space(const uow_chp *c, void *ctx)
{ (void)ctx; return uno_font_text_w_styled(slot_of(c), px_of(c), 0, " "); }

/* ---- the filesystem seam the Open dialog browses -------------------------- */
static const uof_fs kFs = {
    uno_fs_volumes, uno_fs_volume_name, uno_fs_list_begin,
    uno_fs_list_get, uno_fs_isdir
};

/* ---- menus ----------------------------------------------------------------- */
static const uoc_item kFile[] = {
    { "&New\tCtrl+N",        C_NEW,    UOI_NEW,   0, 0, 0 },
    { "&Open...\tCtrl+O",    C_OPEN,   UOI_OPEN,  0, 0, 0 },
    { "&Close",              C_CLOSE,  -1, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Save\tCtrl+S",       C_SAVE,   UOI_SAVE,  0, 0, 0 },
    { "Save &As...",         C_SAVEAS, -1, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Print...\tCtrl+P",   C_PRINT,  UOI_PRINT, UOC_DISABLED, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "E&xit",               C_EXIT,   -1, 0, 0, 0 }
};
static const uoc_item kEdit[] = {
    { "&Undo\tCtrl+Z",  C_UNDO,   UOI_UNDO,  0, 0, 0 },
    { "&Redo\tCtrl+Y",  C_REDO,   UOI_REDO,  0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "Cu&t\tCtrl+X",   C_CUT,    UOI_CUT,   0, 0, 0 },
    { "&Copy\tCtrl+C",  C_COPY,   UOI_COPY,  0, 0, 0 },
    { "&Paste\tCtrl+V", C_PASTE,  UOI_PASTE, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "Select &All\tCtrl+A", C_SELALL, -1, 0, 0, 0 }
};
static const uoc_item kView[] = {
    { "&Normal",       C_NORMAL,     -1, UOC_RADIO, 0, 0 },
    { "&Page Layout",  C_PAGELAYOUT, -1, UOC_RADIO | UOC_CHECKED, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Ruler",        C_RULER,      -1, UOC_CHECKED, 0, 0 },
    { "&Zoom...",      C_ZOOM,       UOI_ZOOM, 0, 0, 0 }
};
static const uoc_item kFormat[] = {
    { "&Font...",            C_FONT,   -1, 0, 0, 0 },
    { "&Paragraph...",       C_PARA,   -1, UOC_DISABLED, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Bullets and Numbering...", C_BULLETS, UOI_BULLETS, UOC_DISABLED, 0, 0 }
};
static const uoc_item kTools[] = {
    { "&Word Count...", C_WORDCOUNT, -1, 0, 0, 0 }
};
static const uoc_item kHelp[] = {
    { "&About UnoWord", C_ABOUT, UOI_HELP, 0, 0, 0 }
};
static const uoc_menu kMenus[] = {
    { "&File", kFile, 10 }, { "&Edit", kEdit, 8 }, { "&View", kView, 5 },
    { "F&ormat", kFormat, 4 }, { "&Tools", kTools, 1 }, { "&Help", kHelp, 1 }
};

static const char *const kStyleList[] = {
    "Normal", "Heading 1", "Heading 2", "Heading 3", "Body Text",
    "Title", "List Bullet"
};
static const char *const kSizeList[] = { "8", "9", "10", "12", "14", "18", "24" };

static const uoc_tbitem kStd[] = {
    { UOC_TB_BUTTON, C_NEW,   UOI_NEW,   "New",   0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_OPEN,  UOI_OPEN,  "Open",  0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_SAVE,  UOI_SAVE,  "Save",  0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_PRINT, UOI_PRINT, "Print", UOC_DISABLED, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_UNDO,  UOI_UNDO,  "Undo",  0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_REDO,  UOI_REDO,  "Redo",  0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_CUT,   UOI_CUT,   "Cut",   0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_COPY,  UOI_COPY,  "Copy",  0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_PASTE, UOI_PASTE, "Paste", 0, 0, 0, 0, 0 }
};
static const uoc_tbitem kFmt[] = {
    { UOC_TB_COMBO,  C_STYLE,    -1, "Style", 0, 76, kStyleList, 7, 0 },
    { UOC_TB_COMBO,  C_FONTNAME, -1, "Font",  0, 80,
      (const char *const *)g_faces, MAXFACE + 1, 0 },
    { UOC_TB_COMBO,  C_FONTSIZE, -1, "Size",  0, 40, kSizeList,  7, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_BOLD,   UOI_BOLD,      "Bold",      0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_ITALIC, UOI_ITALIC,    "Italic",    0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_UNDER,  UOI_UNDERLINE, "Underline", 0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_LEFT,    UOI_ALIGN_L, "Align Left", 0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_CENTER,  UOI_ALIGN_C, "Center",     0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_RIGHT,   UOI_ALIGN_R, "Align Right",0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_JUSTIFY, UOI_JUSTIFY, "Justify",    0, 0, 0, 0, 0 }
};
static const uoc_tbar kBars[] = {
    { "Standard", kStd, 12 }, { "Formatting", kFmt, 12 }
};

static const char *const kDocTypes[] = {
    "Word Document (*.doc)", "Text Only (*.txt)"
};

/* ---- layout bookkeeping ---------------------------------------------------- */
/* Word's "Page Width" zoom, applied automatically.
 *
 * THE DESKTOP IS 640x400 AND A US LETTER PAGE IS 816 PIXELS WIDE AT 100%, so
 * at 1:1 the sheet is wider than the whole screen and the document looks like
 * it has no right margin at all.  (It measured correctly the whole time - the
 * gate's numbers were right - the page simply did not fit.)  So the app opens
 * at whatever zoom makes the page fit its viewport, never magnifying past
 * 1:1, which is what Word's Page Width does and what every reader expects. */
static void fit_page_width(int viewport_w)
{
    const uow_sect *sc;
    int want;
    if (!DOC || viewport_w < 64) return;
    sc = uow_section(DOC);
    if (!sc || sc->page_w <= 0) return;
    want = (int)(((long)(viewport_w - 24) * 15 * 100) / sc->page_w);
    if (want > 100) want = 100;
    if (want < 25)  want = 25;
    if (want != g_zoom) { g_zoom = want; g_dirty_layout = 1; }
}

static void relayout(void)
{
    if (!LAY || !DOC) return;
    uow_layout_run(LAY, DOC, &MET, g_zoom);
    g_dirty_layout = 0;
}
static void touched(void) { g_dirty_layout = 1; pc64_shell_dirty(); }

/* the toolbar toggles follow the caret, which is what makes Bold light up
 * when you move into bold text rather than only when you press it */
static void sync_toggles(void)
{
    uow_chp c;
    uow_pap p;
    char b[16];
    if (!DOC) return;
    uow_chp_at(DOC, g_caret > 0 ? g_caret - 1 : 0, &c);
    uow_pap_at(DOC, g_caret, &p);
    uoc_toggle_set(&CH, C_BOLD,   c.bold);
    uoc_toggle_set(&CH, C_ITALIC, c.italic);
    uoc_toggle_set(&CH, C_UNDER,  c.underline);
    uoc_toggle_set(&CH, C_LEFT,    p.align == UOW_AL_LEFT);
    uoc_toggle_set(&CH, C_CENTER,  p.align == UOW_AL_CENTER);
    uoc_toggle_set(&CH, C_RIGHT,   p.align == UOW_AL_RIGHT);
    uoc_toggle_set(&CH, C_JUSTIFY, p.align == UOW_AL_JUSTIFY);
    uoc_combo_set(&CH, C_STYLE, p.style < 7 ? p.style : 0);
    {   int i, px = (c.size ? c.size : 20) / 2;
        a_num(px, b);
        for (i = 0; i < 7; i++) {
            const char *s = kSizeList[i];
            if (s[0] == b[0] && s[1] == b[1]) { uoc_combo_set(&CH, C_FONTSIZE, i); break; }
        }
    }
    /* the status bar's page and position cells */
    {
        int ln = uow_line_of(LAY, g_caret);
        int pg = (ln >= 0 && ln < LAY->nline) ? LAY->line[ln].page + 1 : 1;
        char n1[12], n2[12], n3[12];
        int k = 0;
        const char *pre = "Page ";
        while (*pre) g_status_l[k++] = *pre++;
        a_num(pg, n1);
        { int i = 0; while (n1[i]) g_status_l[k++] = n1[i++]; }
        g_status_l[k++] = ' '; g_status_l[k++] = ' ';
        pre = "Sec 1   ";
        while (*pre) g_status_l[k++] = *pre++;
        a_num(pg, n2);
        { int i = 0; while (n2[i]) g_status_l[k++] = n2[i++]; }
        g_status_l[k++] = '/';
        a_num(LAY->npage, n3);
        { int i = 0; while (n3[i]) g_status_l[k++] = n3[i++]; }
        g_status_l[k] = 0;
    }
    {
        char n1[12], n2[12];
        int k = 0, i = 0;
        long ps = uow_para_start(DOC, g_caret);
        const char *pre = "Ln ";
        while (*pre) g_status_r[k++] = *pre++;
        a_num(uow_line_of(LAY, g_caret) + 1, n1);
        while (n1[i]) g_status_r[k++] = n1[i++];
        pre = "   Col ";
        while (*pre) g_status_r[k++] = *pre++;
        a_num(g_caret - ps + 1, n2);
        i = 0;
        while (n2[i]) g_status_r[k++] = n2[i++];
        g_status_r[k] = 0;
    }
    ST.page = g_status_l;
    ST.pos  = g_status_r;
}

/* ---- .doc I/O through unodoc ----------------------------------------------- */
static unsigned char *g_io;
static long g_iolen;
static long io_read(void *ctx, long off, unsigned char *dst, long n)
{
    long i;
    (void)ctx;
    if (off < 0 || off >= g_iolen) return 0;
    if (off + n > g_iolen) n = g_iolen - off;
    for (i = 0; i < n; i++) dst[i] = g_io[off + i];
    return n;
}

/* Carry the document's formatting onto the loaded text.
 *
 * Opening a .doc used to keep the characters and throw everything else away:
 * this app asked unodoc for ud_doc_plain() and never for ud_doc_chp_at() or
 * ud_doc_pap_at(), so a bold word arrived unbold and a centred paragraph
 * arrived flush left. The editor's own model has carried bold, italic,
 * underline, strike, caps, size, alignment and indents all along - the toolbar
 * sets them and save_doc() writes them out - so the loss was one-sided, and
 * open-then-save quietly stripped a document on disk.
 *
 * unodoc reports formatting per character position, resolving the piece table
 * itself, and cp here is the same position: the '\n' -> '\r' pass above is
 * one-for-one, so no mapping is needed.
 *
 * Runs are coalesced rather than set per character. A 40,000 character
 * document is 40,000 lookups either way, but uow_format over a whole run
 * touches its piece list once instead of once per character.
 *
 * WHAT THIS CANNOT RECOVER, stated so the next reader does not re-diagnose it:
 * unodoc resolves DIRECT formatting only. A document whose bold comes from a
 * named character or paragraph STYLE still arrives unbold, because unodoc
 * hands us a zero. Measured on unodoc/test/corpus/fmt.doc, which LibreOffice
 * wrote entirely through styles: underline and strike come through, and bold,
 * italic, size, caps and every paragraph alignment do not. Fixing that is
 * unodoc's STSH slice, not this function's job. */
static void load_doc_formatting(ud_doc *w, long len)
{
    long cp = 0;

    if (len <= 0) return;

    /* characters, in runs of identical formatting */
    while (cp < len) {
        ud_chp a, b;
        uow_chp c;
        long end;

        ud_doc_chp_at(w, cp, &a);
        for (end = cp + 1; end < len; end++) {
            ud_doc_chp_at(w, end, &b);
            if (b.bold != a.bold || b.italic != a.italic ||
                b.underline != a.underline || b.strike != a.strike ||
                b.caps != a.caps || b.smallcaps != a.smallcaps ||
                b.super != a.super || b.sub != a.sub || b.size != a.size)
                break;
        }
        uow_chp_at(DOC, cp, &c);              /* keep the face the editor chose */
        c.bold      = (unsigned char)(a.bold      ? 1 : 0);
        c.italic    = (unsigned char)(a.italic    ? 1 : 0);
        c.underline = (unsigned char)(a.underline ? 1 : 0);
        c.strike    = (unsigned char)(a.strike    ? 1 : 0);
        c.caps      = (unsigned char)(a.caps      ? 1 : 0);
        c.smallcaps = (unsigned char)(a.smallcaps ? 1 : 0);
        c.super     = (unsigned char)(a.super     ? 1 : 0);
        c.sub       = (unsigned char)(a.sub       ? 1 : 0);
        /* size is half-points in both, and 0 means "not set directly": leave
         * the editor's default standing rather than collapsing the text to 0pt */
        if (a.size > 0) c.size = (unsigned short)a.size;
        uow_format(DOC, cp, end - cp, &c);
        cp = end;
    }

    /* paragraphs, one at a time: they are already the unit both sides use */
    cp = 0;
    while (cp < len) {
        long e = uow_para_end(DOC, cp);
        ud_pap a;
        uow_pap p;

        ud_doc_pap_at(w, cp, &a);
        uow_pap_at(DOC, cp, &p);
        switch (a.align) {
        case 1:  p.align = UOW_AL_CENTER;  break;
        case 2:  p.align = UOW_AL_RIGHT;   break;
        case 3:  p.align = UOW_AL_JUSTIFY; break;
        default: p.align = UOW_AL_LEFT;    break;
        }
        p.left   = (short)a.left;          /* twips on both sides */
        p.right  = (short)a.right;
        p.first  = (short)a.first;
        p.before = (short)a.before;
        p.after  = (short)a.after;
        uow_format_para(DOC, cp, 1, &p);
        if (e <= cp) break;                /* never loop on a stuck position */
        cp = e + 1;
    }
}

static int load_doc(int vol, const char *name)
{
    ud_cfb *c;
    ud_doc *w;
    ud_src  src;
    long sz;
    int ok = 0;

    ud_set_alloc(malloc, free);
    sz = uno_fs_size(vol, name);
    if (sz <= 0 || sz > 4L * 1024 * 1024) return 0;
    if (!g_io) g_io = (unsigned char *)malloc(4L * 1024 * 1024);
    if (!g_io) return 0;
    g_iolen = uno_fs_read(vol, name, g_io, sz);
    if (g_iolen <= 0) return 0;

    src.read = io_read; src.size = g_iolen; src.ctx = 0;
    c = ud_cfb_open(&src);
    if (!c) return 0;
    w = ud_doc_open(c);
    if (w) {
        const char *plain = ud_doc_plain(w);
        DOC = uow_new();
        if (plain && *plain) {
            /* .doc's paragraph mark is '\r'; ud_doc_plain hands back '\n' */
            long i, n = 0;
            static char conv[65536];
            for (i = 0; plain[i] && n < (long)sizeof conv - 1; i++)
                conv[n++] = (plain[i] == '\n') ? '\r' : plain[i];
            if (n && conv[n-1] == '\r') n--;      /* the doc has its own mark */
            if (n) uow_insert(DOC, 0, conv, n);
            load_doc_formatting(w, n);
        }
        ud_doc_close(w);
        ok = 1;
    }
    ud_cfb_close(c);
    g_caret = g_anchor = 0;
    touched();
    return ok;
}

static int save_doc(int vol, const char *name)
{
    ud_docw *w;
    unsigned char *out;
    long n = 0, cp = 0, len;
    int ok = 0;

    ud_set_alloc(malloc, free);
    w = ud_docw_new();
    if (!w) return 0;
    len = uow_len(DOC);
    while (cp < len) {
        static char para[4096];
        long e = uow_para_end(DOC, cp), take = e - cp;
        uow_pap p;
        uow_chp c;
        if (take > (long)sizeof para - 1) take = (long)sizeof para - 1;
        if (take > 0) uow_read(DOC, cp, take, para);
        para[take > 0 ? take : 0] = 0;
        uow_pap_at(DOC, cp, &p);
        uow_chp_at(DOC, cp, &c);
        ud_docw_para(w, para, c.bold, c.italic, p.align);
        cp = e + 1;
    }
    out = ud_docw_save(w, &n);
    ud_docw_free(w);
    if (out) {
        ok = uno_fs_write(vol, name, out, n);
        ud_free(out);
    }
    return ok;
}

/* ---- the Font dialog, as a data table ------------------------------------- */
enum { FD_SIZE = 300, FD_BOLD, FD_ITALIC, FD_UNDER, FD_PREVIEW };
static const uod_item kFontItems[] = {
    { UOD_LABEL, 0, "&Size:", 10, 8, 60, 0, -1, 0, 0, 0, 0, 0, 0 },
    { UOD_SPIN, FD_SIZE, "10", 70, 6, 60, 0, -1, 0, 0, 0, 0, 6, 40 },
    { UOD_GROUP, 0, "Effects", 10, 34, 180, 62, -1, 0, 0, 0, 0, 0, 0 },
    { UOD_CHECK, FD_BOLD,   "&Bold",      20, 46, 120, 0, -1, 0, 0, 0, 0, 0, 0 },
    { UOD_CHECK, FD_ITALIC, "&Italic",    20, 64, 120, 0, -1, 0, 0, 0, 0, 0, 0 },
    { UOD_CHECK, FD_UNDER,  "&Underline", 20, 82, 120, 0, -1, 0, 0, 0, 0, 0, 0 },
    { UOD_PREVIEW, FD_PREVIEW, 0, 10, 104, 180, 34, -1, 0, 0, 0, 0, 0, 0 },
    { UOD_BUTTON, UOD_ID_OK,     "OK",     40, 148, 60, 20, -1, UOD_DEFAULT, 0, 0, 0, 0, 0 },
    { UOD_BUTTON, UOD_ID_CANCEL, "Cancel", 110, 148, 60, 20, -1, 0, 0, 0, 0, 0, 0 }
};
static const uod_dlg kFontDlg = { "Font", kFontItems, 9, 0, 0, 210, 200, 1 };

/* ---- commands -------------------------------------------------------------- */
static void apply_chp_bit(int which, int on)
{
    uow_chp c;
    long a = g_anchor < g_caret ? g_anchor : g_caret;
    long b = g_anchor < g_caret ? g_caret : g_anchor;
    if (a == b) return;                       /* nothing selected: no-op     */
    uow_chp_at(DOC, a, &c);
    if (which == C_BOLD)   c.bold = (unsigned char)on;
    if (which == C_ITALIC) c.italic = (unsigned char)on;
    if (which == C_UNDER)  c.underline = (unsigned char)on;
    uow_format(DOC, a, b - a, &c);
    touched();
}
static void apply_align(int align)
{
    uow_pap p;
    long a = g_anchor < g_caret ? g_anchor : g_caret;
    uow_pap_at(DOC, a, &p);
    p.align = (unsigned char)align;
    uow_format_para(DOC, a, 1, &p);
    touched();
}

static void do_command(int cmd)
{
    switch (cmd) {
    case C_NEW:
        DOC = uow_new();
        g_caret = g_anchor = 0;
        a_cpy(g_name, "Document1", (int)sizeof g_name);
        touched();
        break;
    case C_OPEN:
        uof_set_fs(&kFs);
        uof_open(&DL, 0, kDocTypes, 2, pc64_shell_workarea_w(),
                 pc64_shell_workarea_h());
        g_dlg_kind = DLG_OPEN;
        break;
    case C_SAVEAS:
        uof_set_fs(&kFs);
        uof_open(&DL, 1, kDocTypes, 2, pc64_shell_workarea_w(),
                 pc64_shell_workarea_h());
        g_dlg_kind = DLG_SAVE;
        break;
    case C_SAVE:
        if (!save_doc(0, g_name))
            uod_msgbox(&DL, "UnoWord", "The document could not be saved.",
                       UOD_MB_OK, pc64_shell_workarea_w(),
                       pc64_shell_workarea_h());
        else
            uod_msgbox(&DL, "UnoWord", "Saved.", UOD_MB_OK,
                       pc64_shell_workarea_w(), pc64_shell_workarea_h());
        g_dlg_kind = DLG_MSG;
        break;
    case C_UNDO: uow_undo(DOC); if (g_caret > uow_len(DOC)) g_caret = uow_len(DOC);
                 g_anchor = g_caret; touched(); break;
    case C_REDO: uow_redo(DOC); if (g_caret > uow_len(DOC)) g_caret = uow_len(DOC);
                 g_anchor = g_caret; touched(); break;
    case C_SELALL: g_anchor = 0; g_caret = uow_len(DOC) - 1; touched(); break;
    case C_BOLD:   apply_chp_bit(C_BOLD,   uoc_toggle(&CH, C_BOLD));   break;
    case C_ITALIC: apply_chp_bit(C_ITALIC, uoc_toggle(&CH, C_ITALIC)); break;
    case C_UNDER:  apply_chp_bit(C_UNDER,  uoc_toggle(&CH, C_UNDER));  break;
    case C_LEFT:    apply_align(UOW_AL_LEFT);    break;
    case C_CENTER:  apply_align(UOW_AL_CENTER);  break;
    case C_RIGHT:   apply_align(UOW_AL_RIGHT);   break;
    case C_JUSTIFY: apply_align(UOW_AL_JUSTIFY); break;
    case C_FONTNAME: {
        int f = uoc_combo(&CH, C_FONTNAME);
        long a = g_anchor < g_caret ? g_anchor : g_caret;
        long b = g_anchor < g_caret ? g_caret : g_anchor;
        uow_chp c;
        uow_chp_at(DOC, a, &c);
        c.face = (unsigned short)f;
        if (b > a) uow_format(DOC, a, b - a, &c);
        touched();
        break;
    }
    case C_STYLE: {
        int s = uoc_combo(&CH, C_STYLE);
        long a = g_anchor < g_caret ? g_anchor : g_caret;
        uow_set_style(DOC, a, 1, s);
        touched();
        break;
    }
    case C_RULER: g_showruler = !g_showruler; touched(); break;
    case C_FONT:
        uod_open(&DL, &kFontDlg, pc64_shell_workarea_w(),
                 pc64_shell_workarea_h());
        {   uow_chp c;
            uow_chp_at(DOC, g_caret > 0 ? g_caret - 1 : 0, &c);
            uod_set_value(&DL, FD_BOLD, c.bold);
            uod_set_value(&DL, FD_ITALIC, c.italic);
            uod_set_value(&DL, FD_UNDER, c.underline);
            uod_set_value(&DL, FD_SIZE, (c.size ? c.size : 20) / 2);
        }
        g_dlg_kind = DLG_FONT;
        break;
    case C_WORDCOUNT: {
        char msg[64];
        long i, len = uow_len(DOC), words = 0;
        int inw = 0;
        for (i = 0; i < len; i++) {
            int ch = uow_char_at(DOC, i);
            if (ch == ' ' || ch == '\r' || ch == '\t') inw = 0;
            else if (!inw) { inw = 1; words++; }
        }
        {   int k = 0; const char *p = "Words: ";
            char n[12];
            while (*p) msg[k++] = *p++;
            a_num(words, n);
            { int j = 0; while (n[j]) msg[k++] = n[j++]; }
            msg[k] = 0;
        }
        uod_msgbox(&DL, "Word Count", msg, UOD_MB_OK,
                   pc64_shell_workarea_w(), pc64_shell_workarea_h());
        g_dlg_kind = DLG_MSG;
        break;
    }
    case C_ABOUT:
        uod_msgbox(&DL, "About UnoWord",
                   "UnoWord - a Microsoft Word 97 clone for UnoDOS.",
                   UOD_MB_OK, pc64_shell_workarea_w(),
                   pc64_shell_workarea_h());
        g_dlg_kind = DLG_MSG;
        break;
    default: break;
    }
    sync_toggles();
}

/* ---- painting -------------------------------------------------------------- */
static void draw_doc(int cx, int cy, int cw, int ch)
{
    int i, k;
    long selA = g_anchor < g_caret ? g_anchor : g_caret;
    long selB = g_anchor < g_caret ? g_caret : g_anchor;

    fb_fill_rect(cx, cy, cw, ch, FB_RGB(0x80,0x80,0x80));
    if (!LAY) return;
    for (i = 0; i < LAY->npage; i++) {
        const uow_page *pg = &LAY->page[i];
        int y = cy + pg->y - g_scroll;
        if (y > cy + ch || y + pg->h < cy) continue;
        fb_fill_rect(cx + pg->x, y, pg->w, pg->h, FB_RGB(0xFF,0xFF,0xFF));
        fb_frame_rect(cx + pg->x, y, pg->w, pg->h, FB_RGB(0x40,0x40,0x40));
    }
    for (i = 0; i < LAY->nline; i++) {
        const uow_line *ln = &LAY->line[i];
        int y = cy + ln->y - g_scroll;
        if (y > cy + ch || y + ln->h < cy) continue;
        for (k = 0; k < ln->nrun; k++) {
            const uow_lrun *r = &LAY->run[ln->run0 + k];
            char buf[256];
            long got = uow_read(DOC, r->cp, r->n < 255 ? r->n : 255, buf);
            fb_px col = r->chp.color ? r->chp.color : FB_RGB(0,0,0);
            buf[got] = 0;
            if (selB > selA && r->cp < selB && r->cp + r->n > selA)
                fb_fill_rect(cx + ln->x + r->x, y, r->w, ln->h,
                             FB_RGB(0x00,0x00,0x80));
            uno_font_draw_styled(slot_of(&r->chp), px_of(&r->chp),
                                 style_of(&r->chp),
                                 cx + ln->x + r->x, y, buf,
                                 (selB > selA && r->cp >= selA && r->cp < selB)
                                     ? FB_RGB(0xFF,0xFF,0xFF) : col, -1);
            if (r->chp.underline)
                fb_hline(cx + ln->x + r->x, y + ln->baseline + 1, r->w, col);
        }
    }
    /* the caret */
    {
        int ln = uow_line_of(LAY, g_caret);
        if (ln >= 0 && ln < LAY->nline) {
            int x = uow_caret_x(LAY, &MET, g_caret);
            int y = cy + LAY->line[ln].y - g_scroll;
            if (y >= cy && y + LAY->line[ln].h <= cy + ch)
                fb_vline(cx + x, y, LAY->line[ln].h, FB_RGB(0,0,0));
        }
    }
}

static void app_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    int cx, cy, cw, chh, top;
    (void)w; (void)ctx;
    fit_page_width(r.w);
    g_rect = r;
    g_have_rect = 1;
    if (g_dirty_layout) relayout();
    sync_toggles();

    CH.x = r.x; CH.y = r.y; CH.w = r.w; CH.h = r.h;
    uoc_render_bars(&CH);
    top = r.y + uoc_height(&CH);
    if (g_showruler) {
        uob_ruler_render(&RU, r.x, top, r.w);
        top += uob_ruler_h();
    }
    cx = r.x; cy = top; cw = r.w;
    chh = r.h - (top - r.y) - uob_status_h();
    if (chh < 16) chh = 16;
    draw_doc(cx, cy, cw, chh);
    uob_status_render(&ST, r.x, r.y + r.h - uob_status_h(), r.w);
    uoc_render_popups(&CH);          /* an open menu goes OVER the page */
    if (g_dlg_kind) uod_render(&DL);
}

/* ---- events ---------------------------------------------------------------- */
static int doc_top(unoui_rect r)
{
    int t = r.y + uoc_height(&CH);
    if (g_showruler) t += uob_ruler_h();
    return t;
}

/* Whatever a closing dialog meant.  Shared, because a dialog can be
 * dismissed by the mouse (app_event) or by Enter/Esc (uw_key). */
static void dialog_closed(void)
{
    int res = uod_result(&DL), kind = g_dlg_kind;
    g_dlg_kind = DLG_NONE;
    if (res == UOD_ID_OK && kind == DLG_OPEN) {
        a_cpy(g_name, uof_name(), (int)sizeof g_name);
        load_doc(uof_volume(), g_name);
    } else if (res == UOD_ID_OK && kind == DLG_SAVE) {
        a_cpy(g_name, uof_name(), (int)sizeof g_name);
        save_doc(uof_volume(), g_name);
    } else if (res == UOD_ID_OK && kind == DLG_FONT) {
        uow_chp c;
        long a = g_anchor < g_caret ? g_anchor : g_caret;
        long b = g_anchor < g_caret ? g_caret : g_anchor;
        uow_chp_at(DOC, a, &c);
        c.bold      = (unsigned char)uod_value(&DL, FD_BOLD);
        c.italic    = (unsigned char)uod_value(&DL, FD_ITALIC);
        c.underline = (unsigned char)uod_value(&DL, FD_UNDER);
        c.size      = (unsigned short)(uod_value(&DL, FD_SIZE) * 2);
        if (b > a) uow_format(DOC, a, b - a, &c);
    }
    touched();
}

static int app_event(struct unoui_widget *w, const void *evp, void *ctx)
{
    const unoui_event *e = (const unoui_event *)evp;
    unoui_rect r;
    int cmd = 0;
    (void)ctx; (void)w;
    if (!g_have_rect) return 0;      /* nothing has been painted yet */
    r = g_rect;

    /* a dialog is modal: it eats everything until it closes */
    if (g_dlg_kind) {
        uod_handle(&DL, e);
        if (g_dlg_kind == DLG_OPEN || g_dlg_kind == DLG_SAVE) uof_sync(&DL);
        if (!uod_is_open(&DL)) dialog_closed();
        pc64_shell_dirty();
        return 1;
    }

    CH.x = r.x; CH.y = r.y; CH.w = r.w; CH.h = r.h;
    if (uoc_handle(&CH, e, &cmd)) {
        if (cmd) do_command(cmd);
        pc64_shell_dirty();
        return 1;
    }
    if (g_showruler &&
        uob_ruler_handle(&RU, e, r.x, r.y + uoc_height(&CH), r.w)) {
        pc64_shell_dirty();
        return 1;
    }
    if (e->kind == UI_EV_WHEEL) {
        g_scroll += e->wheel * 32;
        if (g_scroll < 0) g_scroll = 0;
        if (LAY && g_scroll > LAY->doc_h) g_scroll = LAY->doc_h;
        pc64_shell_dirty();
        return 1;
    }
    /* Click to place the caret, DRAG to select.
     *
     * The first cut extended the selection on a move event `if (e->button)`.
     * `button` is not "a button is held" - it is WHICH button, and the left
     * one is 0 - so that test was false for every left-button drag ever made
     * and selecting text with the mouse simply did not work.  The app tracks
     * the drag itself, between DOWN and UP, which is the only thing the event
     * stream actually tells it. */
    if (e->kind == UI_EV_MOUSE_UP) { g_dragging = 0; return 0; }
    if (e->kind == UI_EV_MOUSE_DOWN || e->kind == UI_EV_MOUSE_MOVE) {
        int top = doc_top(r);
        if (e->kind == UI_EV_MOUSE_MOVE && !g_dragging) return 0;
        if (e->y >= top && LAY) {
            long cp = uow_cp_at(LAY, &MET, e->x - r.x, e->y - top + g_scroll);
            if (e->kind == UI_EV_MOUSE_DOWN) {
                g_caret = g_anchor = cp;
                g_dragging = 1;
            } else {
                g_caret = cp;                   /* dragging: extend           */
            }
            sync_toggles();
            pc64_shell_dirty();
            return 1;
        }
    }
    return 0;
}

/* ---- the app vtable -------------------------------------------------------- */
static void uw_build(unoui_window *win)
{
    int w = pc64_shell_workarea_w() - 40, h = pc64_shell_workarea_h() - 60;
    if (w < 380) w = 380;
    if (h < 260) h = 260;
    unoui_window_init(win, "UnoWord - Document1", 20, 16, w, h);
    g_canvas.draw = app_draw;
    g_canvas.event = app_event;
    g_canvas.ctx = 0;
    unoui_widget_fill(unoui_add_canvas(win, 0, 0, w - 12, h - 28, &g_canvas));
    win->flags |= UI_WIN_RESIZE;
    g_win = win;
    g_cidx = 0;
}
static int uw_action(const unoui_action *a) { (void)a; return 0; }

static int uw_key(int uni, int scan, int ctrl)
{
    unoui_event e;
    int i;
    (void)scan;
    if (!DOC) return 0;
    if (g_dlg_kind) {
        for (i = 0; i < (int)sizeof e; i++) ((char *)&e)[i] = 0;
        if (uni >= ' ') { e.kind = UI_EV_CHAR; e.ch = uni; }
        else if (uni == '\r' || uni == '\n') { e.kind = UI_EV_KEY; e.key = UI_KEY_ENTER; }
        else if (uni == 27) { e.kind = UI_EV_KEY; e.key = UI_KEY_ESC; }
        else if (uni == '\t') { e.kind = UI_EV_KEY; e.key = UI_KEY_TAB; }
        else if (uni == 8) { e.kind = UI_EV_CHAR; e.ch = 8; }
        else return 0;
        uod_handle(&DL, &e);
        if (g_dlg_kind == DLG_OPEN || g_dlg_kind == DLG_SAVE) uof_sync(&DL);
        if (!uod_is_open(&DL)) dialog_closed();
        pc64_shell_dirty();
        return 1;
    }
    if (ctrl) {
        int c = uni;
        if (c >= 1 && c <= 26) c += 'a' - 1;       /* control codes to letters */
        switch (c) {
        case 'b': uoc_toggle_set(&CH, C_BOLD, !uoc_toggle(&CH, C_BOLD));
                  do_command(C_BOLD); return 1;
        case 'i': uoc_toggle_set(&CH, C_ITALIC, !uoc_toggle(&CH, C_ITALIC));
                  do_command(C_ITALIC); return 1;
        case 'u': uoc_toggle_set(&CH, C_UNDER, !uoc_toggle(&CH, C_UNDER));
                  do_command(C_UNDER); return 1;
        case 'z': do_command(C_UNDO); return 1;
        case 'y': do_command(C_REDO); return 1;
        case 'a': do_command(C_SELALL); return 1;
        case 's': do_command(C_SAVE); return 1;
        case 'o': do_command(C_OPEN); return 1;
        case 'n': do_command(C_NEW); return 1;
        default: return 0;
        }
    }
    if (uni == 8) {                                 /* backspace              */
        if (g_caret > 0) { uow_delete(DOC, g_caret - 1, 1); g_caret--; }
        g_anchor = g_caret;
        touched();
        return 1;
    }
    if (uni == '\r' || uni == '\n') {
        char nl = '\r';
        uow_insert(DOC, g_caret, &nl, 1);
        g_caret++;
        g_anchor = g_caret;
        touched();
        return 1;
    }
    if (uni >= ' ') {
        char ch = (char)uni;
        uow_insert(DOC, g_caret, &ch, 1);
        g_caret++;
        g_anchor = g_caret;
        touched();
        return 1;
    }
    return 0;
}

static void uw_frame(void) { }
static void uw_opened(void)
{
    if (!DOC) DOC = uow_new();
    if (!LAY) LAY = (uow_layout *)malloc(sizeof(uow_layout));
    uoc_icons_install();
    {   /* whatever faces this machine actually has, plus the default */
        int n = uno_font_count(), i;
        g_faces[0] = "Default";
        g_nface = 1;
        for (i = 0; i < n && g_nface <= MAXFACE; i++) {
            const char *nm = uno_font_name(i);
            if (nm && *nm) g_faces[g_nface++] = nm;
        }
        for (i = g_nface; i <= MAXFACE; i++) g_faces[i] = "";
    }
    uoc_init(&CH, kMenus, 6, kBars, 2, 0, 0, 400, 300);
    uob_ruler_init(&RU, 20, 300);
    ST.page = "Page 1   Sec 1   1/1";
    ST.pos  = "Ln 1   Col 1";
    MET.text_w = m_text_w; MET.height = m_height;
    MET.baseline = m_baseline; MET.space_w = m_space; MET.ctx = 0;
    g_dirty_layout = 1;
}
static void uw_closed(void) { }
static int  uw_canvas_index(void) { return g_cidx; }

/* what the shell shows for this app, carried in the module (uno_appdesc.h) */
UNO_APP_DESC("id: uoword\n"
             "name: UnoWord\n"
             "icon: uoword\n"
             "cat: tools\n"
             "rank: 20\n");

static const UnoUuiApp kApp = {
    UNO_UUIAPP_ABI, "UnoWord",
    uw_build, uw_action, uw_key, uw_frame, uw_opened, uw_closed,
    uw_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved)
{ (void)reserved; return &kApp; }
