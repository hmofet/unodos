/* ===========================================================================
 * uocalc.c - UnoCalc, the spreadsheet (OFFICE97-PLAN §6 phase 10).
 *
 * A unoui-CLASS .UNO module (APPS\UOCALC.UNO), hosted exactly as UnoWord,
 * Studio and Photos are: one window, one canvas, and everything inside it -
 * command bars, the formula bar, the grid, the sheet tabs, the status bar -
 * drawn by the uoffice lane.
 *
 * THE GRID IS DRAWN VIRTUALISED: only the cells inside the viewport are
 * painted, walked from the scroll origin rather than from the store.  A
 * sheet is 65536 x 256 and the store is sparse, so neither "iterate the
 * grid" nor "iterate the cells" is right on its own - you iterate the
 * VISIBLE RECTANGLE and look each cell up, which is O(visible log live).
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
#include "uocalc.h"
#include "uoapp.h"
#include "unodoc.h"
/* unomedia, for um_set_alloc alone: unodoc inflates an OOXML part with
 * um_inflate, which allocates its own working state. */
#include "unomedia.h"
#include "pc64_font.h"

void  pc64_shell_dirty(void);
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

enum {
    C_NEW = 1, C_OPEN, C_SAVE, C_EXIT,
    C_UNDO = 20, C_CUT, C_COPY, C_PASTE,
    C_BOLD = 40, C_ITALIC, C_UNDER, C_LEFT, C_CENTER, C_RIGHT,
    C_CURRENCY = 60, C_PERCENT, C_COMMA, C_DEC_MORE, C_DEC_LESS,
    C_SUM = 80, C_SORTA, C_SORTD, C_RECALC,
    C_ABOUT = 100, C_FUNCS
};

static uoc_ui     CH;
static uod_ui     DL;
static uob_status ST;
static uxl_book  *BK;
static unoui_window *g_win;
static unoui_canvas  g_canvas;
static unoui_rect g_rect;
static int  g_have_rect;
static int  g_cidx = -1;
static int  g_sheet, g_cur_r, g_cur_c;      /* the selected cell            */
static int  g_sel_r, g_sel_c;               /* the anchor of a range        */
static int  g_top_r, g_left_c;              /* the scroll origin            */
static int  g_editing;
static char g_edit[128];
static int  g_dlg;                          /* DLG_* while one is up        */
enum { DLG_NONE = 0, DLG_OPEN, DLG_SAVE, DLG_MSG, DLG_GUARD };
static char g_name[256];                    /* "" until saved or opened     */
static int  g_vol;                          /* ...and the volume it lives on */
/* Unsaved changes.  The workbook model has no revision counter, so the app
 * marks the edits it makes - each one goes through commit_edit, set_fmt_all,
 * a command or a paste, and every one of those sets this. */
static int  g_changed;
static char g_title[300] = "UnoCalc - Book1";
static unsigned char *g_io;                 /* the file buffer              */
static long g_iolen;
static char g_statl[64], g_statr[64];

static void a_cpy(char *d, const char *s, int cap)
{ int i = 0; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int  a_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static char *a_num(long v, char *b)
{
    int n = 0, i = 0, dg[12];
    if (v < 0) { b[n++] = '-'; v = -v; }
    do { dg[i++] = (int)(v % 10); v /= 10; } while (v && i < 12);
    while (i) b[n++] = (char)('0' + dg[--i]);
    b[n] = 0;
    return b;
}

/* ---- geometry ---------------------------------------------------------------
 * Column widths and row heights are uniform in v1; the headers and the cells
 * derive from the SAME two functions, so a click lands on the cell it looks
 * like it lands on. */
/* The workbook's text is CP-1252 (unodoc's, and Excel 97's); the font
 * engine draws UTF-8.  Every string that came out of a cell or a sheet name
 * goes through these two, or an accented letter draws as a broken glyph. */
static void dtext(int x, int y, const char *s, fb_px c)
{
    char u[768];
    uoa_to_utf8(s, -1, u, (int)sizeof u);
    fb_text(x, y, u, c, -1);
}
static int dtext_w(const char *s)
{
    char u[768];
    uoa_to_utf8(s, -1, u, (int)sizeof u);
    return fb_text_w(u);
}

static int cell_h(void) { return fb_text_h() + 4; }
static int cell_w(void) { return fb_text_w("0") * 9 + 6; }
static int head_w(void) { return fb_text_w("0") * 4 + 6; }
static int fbar_h(void) { return fb_text_h() + 8; }
static int tabs_h(void) { return fb_text_h() + 6; }

static void grid_rect(int *x, int *y, int *w, int *h)
{
    int top = g_rect.y + uoc_height(&CH) + fbar_h();
    *x = g_rect.x;
    *y = top;
    *w = g_rect.w;
    *h = g_rect.h - (top - g_rect.y) - tabs_h() - uob_status_h();
    if (*h < cell_h() * 2) *h = cell_h() * 2;
}

static const uof_fs kFs = {
    uno_fs_volumes, uno_fs_volume_name, uno_fs_list_begin,
    uno_fs_list_get, uno_fs_isdir
};

/* ---- menus ------------------------------------------------------------------ */
static const uoc_item kFile[] = {
    { "&New\tCtrl+N",     C_NEW,  UOI_NEW,  0, 0, 0 },
    { "&Open...\tCtrl+O", C_OPEN, UOI_OPEN, 0, 0, 0 },
    { "&Save\tCtrl+S",    C_SAVE, UOI_SAVE, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "E&xit",            C_EXIT, -1, 0, 0, 0 }
};
static const uoc_item kEdit[] = {
    { "&Undo\tCtrl+Z",  C_UNDO,  UOI_UNDO,  UOC_DISABLED, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "Cu&t\tCtrl+X",   C_CUT,   UOI_CUT,   0, 0, 0 },
    { "&Copy\tCtrl+C",  C_COPY,  UOI_COPY,  0, 0, 0 },
    { "&Paste\tCtrl+V", C_PASTE, UOI_PASTE, 0, 0, 0 }
};
static const uoc_item kInsert[] = {
    { "&Function...", C_FUNCS, -1, 0, 0, 0 },
    { "&AutoSum",     C_SUM,   -1, 0, 0, 0 }
};
static const uoc_item kFormat[] = {
    { "&Currency",  C_CURRENCY, -1, 0, 0, 0 },
    { "&Percent",   C_PERCENT,  -1, 0, 0, 0 },
    { "C&omma",     C_COMMA,    -1, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Increase Decimal", C_DEC_MORE, -1, 0, 0, 0 },
    { "&Decrease Decimal", C_DEC_LESS, -1, 0, 0, 0 }
};
static const uoc_item kTools[] = {
    { "&Recalculate\tF9", C_RECALC, -1, 0, 0, 0 }
};
static const uoc_item kHelp[] = {
    { "&About UnoCalc", C_ABOUT, UOI_HELP, 0, 0, 0 }
};
static const uoc_menu kMenus[] = {
    { "&File", kFile, 5 }, { "&Edit", kEdit, 5 }, { "&Insert", kInsert, 2 },
    { "F&ormat", kFormat, 6 }, { "&Tools", kTools, 1 }, { "&Help", kHelp, 1 }
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
    { UOC_TB_BUTTON, C_SUM,   UOI_NUMBERING, "AutoSum", 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_SORTA, UOI_ALIGN_L,   "Sort Ascending", 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_SORTD, UOI_ALIGN_R,   "Sort Descending", 0, 0, 0, 0, 0 }
};
static const uoc_tbitem kFmtBar[] = {
    { UOC_TB_TOGGLE, C_BOLD,   UOI_BOLD,      "Bold",      0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_ITALIC, UOI_ITALIC,    "Italic",    0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_UNDER,  UOI_UNDERLINE, "Underline", 0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_CURRENCY, UOI_FILLCOLOR, "Currency Style", 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_PERCENT,  UOI_FONTCOLOR, "Percent Style",  0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_COMMA,    UOI_HIGHLIGHT, "Comma Style",    0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0, -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_DEC_MORE, UOI_INDENT_INC, "Increase Decimal", 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_DEC_LESS, UOI_INDENT_DEC, "Decrease Decimal", 0, 0, 0, 0, 0 }
};
static const uoc_tbar kBars[] = {
    { "Standard", kStd, 11 }, { "Formatting", kFmtBar, 10 }
};
static const char *const kTypes[] = {
    "Microsoft Excel Workbook (*.xls)", "Excel Workbook (*.xlsx)"
};

/* ---- the status bar and the AutoCalculate well ------------------------------ */
static void sync_status(void)
{
    char n[16];
    int k = 0, i = 0;
    uxl_a1_write(g_cur_r, g_cur_c, 0, 0, g_statl, (int)sizeof g_statl);
    /* Excel's AutoCalculate: the sum of the selection, in the status bar */
    {
        double total = 0;
        int r, c, r0 = g_sel_r < g_cur_r ? g_sel_r : g_cur_r;
        int r1 = g_sel_r > g_cur_r ? g_sel_r : g_cur_r;
        int c0 = g_sel_c < g_cur_c ? g_sel_c : g_cur_c;
        int c1 = g_sel_c > g_cur_c ? g_sel_c : g_cur_c;
        const char *pre = "Sum=";
        for (r = r0; r <= r1; r++)
            for (c = c0; c <= c1; c++) {
                uxl_val v;
                if (uxl_get(BK, g_sheet, r, c, &v) && v.kind == UXL_NUM)
                    total += v.num;
            }
        while (*pre) g_statr[k++] = *pre++;
        a_num((long)total, n);
        while (n[i]) g_statr[k++] = n[i++];
        g_statr[k] = 0;
    }
    ST.page = g_statl;
    ST.pos  = g_statr;
}

/* ---- painting ---------------------------------------------------------------- */
static void draw_formula_bar(void)
{
    const uoc_look *k = uoc_look_97();
    int y = g_rect.y + uoc_height(&CH), h = fbar_h();
    int nb = head_w() * 2;
    char a1[16];
    fb_fill_rect(g_rect.x, y, g_rect.w, h, k->face);
    /* the Name Box, then the entry field - Excel 97's own arrangement */
    fb_fill_rect(g_rect.x + 2, y + 2, nb, h - 4, k->hilight);
    uoc_sunken(k, g_rect.x + 2, y + 2, nb, h - 4);
    uxl_a1_write(g_cur_r, g_cur_c, 0, 0, a1, (int)sizeof a1);
    fb_text(g_rect.x + 5, y + 3, a1, k->text, -1);

    fb_fill_rect(g_rect.x + nb + 8, y + 2, g_rect.w - nb - 12, h - 4, k->hilight);
    uoc_sunken(k, g_rect.x + nb + 8, y + 2, g_rect.w - nb - 12, h - 4);
    {
        const char *t = g_editing ? g_edit : uxl_formula(BK, g_sheet, g_cur_r, g_cur_c);
        char buf[64];
        if (!t) {
            uxl_val v;
            if (uxl_get(BK, g_sheet, g_cur_r, g_cur_c, &v) && v.kind == UXL_STR)
                t = uxl_pool(BK, v.str);
            else if (v.kind == UXL_NUM) { uxl_general(v.num, buf, (int)sizeof buf); t = buf; }
            else t = "";
        }
        dtext(g_rect.x + nb + 11, y + 3, t, k->text);
        if (g_editing)
            fb_vline(g_rect.x + nb + 11 + dtext_w(g_edit), y + 3,
                     fb_text_h(), k->text);
    }
}

static void draw_grid(void)
{
    const uoc_look *k = uoc_look_97();
    int gx, gy, gw, gh, cw = cell_w(), chh = cell_h(), hw = head_w();
    int r, c, x, y;
    int r0 = g_sel_r < g_cur_r ? g_sel_r : g_cur_r;
    int r1 = g_sel_r > g_cur_r ? g_sel_r : g_cur_r;
    int c0 = g_sel_c < g_cur_c ? g_sel_c : g_cur_c;
    int c1 = g_sel_c > g_cur_c ? g_sel_c : g_cur_c;
    char buf[64];

    grid_rect(&gx, &gy, &gw, &gh);
    fb_fill_rect(gx, gy, gw, gh, k->hilight);

    /* the headers, drawn from the same widths the cells use */
    fb_fill_rect(gx, gy, gw, chh, k->face);
    fb_fill_rect(gx, gy, hw, gh, k->face);
    for (c = 0; (x = gx + hw + (c) * cw) < gx + gw; c++) {
        int col = g_left_c + c;
        int on = (col >= c0 && col <= c1);
        if (col >= UXL_COLS) break;
        fb_fill_rect(x, gy, cw - 1, chh - 1, on ? k->shadow : k->face);
        uoc_bevel(x, gy, cw - 1, chh - 1, k->hilight, k->shadow, 1);
        uxl_a1_write(0, col, 0, 0, buf, (int)sizeof buf);
        { int L = a_len(buf); while (L && buf[L-1] >= '0' && buf[L-1] <= '9') buf[--L] = 0; }
        fb_text(x + (cw - fb_text_w(buf)) / 2, gy + 2, buf, k->text, -1);
    }
    for (r = 0; (y = gy + chh + r * chh) < gy + gh; r++) {
        int row = g_top_r + r;
        int on = (row >= r0 && row <= r1);
        if (row >= UXL_ROWS) break;
        fb_fill_rect(gx, y, hw - 1, chh - 1, on ? k->shadow : k->face);
        uoc_bevel(gx, y, hw - 1, chh - 1, k->hilight, k->shadow, 1);
        a_num(row + 1, buf);
        fb_text(gx + (hw - fb_text_w(buf)) / 2, y + 2, buf, k->text, -1);
    }

    /* the cells */
    for (r = 0; (y = gy + chh + r * chh) < gy + gh; r++) {
        int row = g_top_r + r;
        if (row >= UXL_ROWS) break;
        for (c = 0; (x = gx + hw + c * cw) < gx + gw; c++) {
            int col = g_left_c + c;
            int sel = (row >= r0 && row <= r1 && col >= c0 && col <= c1);
            if (col >= UXL_COLS) break;
            if (sel && !(row == g_cur_r && col == g_cur_c))
                fb_fill_rect(x, y, cw - 1, chh - 1, FB_RGB(0xE0,0xE0,0xF0));
            fb_hline(x, y + chh - 1, cw, k->light);
            fb_vline(x + cw - 1, y, chh, k->light);
            if (uxl_text(BK, g_sheet, row, col, buf, (int)sizeof buf) > 0) {
                uxl_val v;
                int w = dtext_w(buf);
                /* numbers right-align, text left - Excel's default and the
                 * fastest way to see that a "number" arrived as text */
                uxl_get(BK, g_sheet, row, col, &v);
                dtext(x + ((v.kind == UXL_NUM || v.kind == UXL_ERR)
                           ? cw - 3 - w : 3), y + 2, buf, k->text);
            }
        }
    }
    /* the active cell's heavy border */
    {
        int ax = gx + hw + (g_cur_c - g_left_c) * cw;
        int ay = gy + chh + (g_cur_r - g_top_r) * chh;
        if (ax >= gx + hw && ay >= gy + chh && ax < gx + gw && ay < gy + gh) {
            fb_frame_rect(ax - 1, ay - 1, cw + 1, chh + 1, k->dkshadow);
            fb_frame_rect(ax, ay, cw - 1, chh - 1, k->dkshadow);
            fb_fill_rect(ax + cw - 4, ay + chh - 4, 4, 4, k->dkshadow);  /* fill handle */
        }
    }
}

static void draw_tabs(void)
{
    const uoc_look *k = uoc_look_97();
    int y = g_rect.y + g_rect.h - uob_status_h() - tabs_h();
    int i, x = g_rect.x + 4;
    fb_fill_rect(g_rect.x, y, g_rect.w, tabs_h(), k->face);
    for (i = 0; i < uxl_sheets(BK); i++) {
        const char *nm = uxl_sheet_name(BK, i);
        int w = dtext_w(nm) + 12;
        int on = (i == g_sheet);
        fb_fill_rect(x, y + (on ? 0 : 2), w, tabs_h() - (on ? 0 : 2),
                     on ? k->hilight : k->face);
        uoc_bevel(x, y + (on ? 0 : 2), w, tabs_h() - (on ? 0 : 2),
                  k->hilight, k->shadow, 1);
        dtext(x + 6, y + 3, nm, k->text);
        x += w + 2;
    }
}

static void app_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    (void)w; (void)ctx;
    g_rect = r;
    g_have_rect = 1;
    sync_status();
    CH.x = r.x; CH.y = r.y; CH.w = r.w; CH.h = r.h;
    uoc_render_bars(&CH);
    draw_formula_bar();
    draw_grid();
    draw_tabs();
    uob_status_render(&ST, r.x, r.y + r.h - uob_status_h(), r.w);
    uoc_render_popups(&CH);          /* an open menu goes OVER the grid */
    if (g_dlg) uod_render(&DL);
}

/* Excel's on-disk error codes and ours are two enumerations of the same
 * seven values; neither may be cast to the other. */
static int xls_err_to_uxl(int e)
{
    switch (e) {
    case UD_XE_NULL:  return UXL_E_NULL;
    case UD_XE_DIV0:  return UXL_E_DIV0;
    case UD_XE_VALUE: return UXL_E_VALUE;
    case UD_XE_REF:   return UXL_E_REF;
    case UD_XE_NAME:  return UXL_E_NAME;
    case UD_XE_NUM:   return UXL_E_NUM;
    default:          return UXL_E_NA;
    }
}
static int uxl_err_to_xls(int e)
{
    switch (e) {
    case UXL_E_NULL:  return UD_XE_NULL;
    case UXL_E_DIV0:  return UD_XE_DIV0;
    case UXL_E_VALUE: return UD_XE_VALUE;
    case UXL_E_REF:   return UD_XE_REF;
    case UXL_E_NAME:  return UD_XE_NAME;
    case UXL_E_NUM:   return UD_XE_NUM;
    default:          return UD_XE_NA;
    }
}

/* ---- .xls, in and out --------------------------------------------------------
 * unodoc does the format; this is the mapping between its cell view and the
 * workbook model, and it is deliberately symmetric - what save writes, open
 * reads back.  A FORMULA round-trips as its TEXT (unodoc decompiles the ptg
 * array on the way in and recompiles it on the way out), with the cached
 * value carried alongside so a reader that does not calculate still shows
 * the right number. */
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

#define UOC_IOCAP (4L * 1024 * 1024)

/* ---- which format a name asks for --------------------------------------------
 * unodoc reads either container by sniffing the bytes, so OPENING needs no
 * help from the name.  SAVING does: an empty document has no bytes to sniff,
 * and the user picked the format when they typed the extension.  Anything that
 * is not one of the OOXML extensions saves as the 97 binary, which is the
 * format this suite is a clone of and the right default for a name that says
 * nothing. */
static const char *xls_ext(int type) { return type == 1 ? ".XLSX" : ".XLS"; }
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
    ext = xls_ext(type);
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

static int load_book(int vol, const char *name)
{
    ud_cfb *c = 0;
    ud_zip *z = 0;
    ud_xls *x = 0;
    ud_src  src;
    long sz;
    int ok = 0;

    ud_set_alloc(malloc, free);
    /* A .xlsx part is a DEFLATE stream, and unodoc inflates it with
     * unomedia's decompressor - which keeps its working state in an
     * allocation of its own.  Registering both allocators is the one extra
     * obligation the OOXML path puts on a caller, and forgetting it makes
     * every .xlsx fail to open with an out-of-memory that is really a
     * forgotten registration. */
    um_set_alloc(malloc, free);
    sz = uno_fs_size(vol, name);
    if (sz <= 0 || sz > UOC_IOCAP) return 0;
    if (!g_io) g_io = (unsigned char *)malloc(UOC_IOCAP);
    if (!g_io) return 0;
    g_iolen = uno_fs_read(vol, name, g_io, sz);
    if (g_iolen <= 0) return 0;

    src.read = io_read; src.size = g_iolen; src.ctx = 0;
    /* The CONTAINER decides which reader, not the extension: a .xls that is
     * really a .xlsx opens, and so does the reverse.  Both readers hand back
     * the same ud_xls, so everything below this point is shared. */
    if (ud_sniff(&src) == UD_C_ZIP) {
        z = ud_zip_open(&src);
        x = z ? ud_xlsx_open(z) : 0;
    } else {
        c = ud_cfb_open(&src);
        x = c ? ud_xls_open(c) : 0;
    }
    if (x) {
        int ns = ud_xls_sheets(x), si;
        BK = uxl_new();
        for (si = 0; si < ns && si < UXL_MAXSHEET; si++) {
            int n = ud_xls_cell_count(x, si), i, sh = si;
            if (si >= uxl_sheets(BK)) sh = uxl_sheet_add(BK, ud_xls_sheet_name(x, si));
            else                      sh = si;
            if (sh < 0) break;
            for (i = 0; i < n; i++) {
                int row = 0, col = 0;
                ud_xcell cv;
                if (!ud_xls_cell_at(x, si, i, &row, &col, &cv)) continue;
                if (cv.formula && cv.ftext && *cv.ftext &&
                    uxl_set_formula(BK, sh, row, col, cv.ftext)) continue;
                switch (cv.kind) {
                case UD_XV_NUM:  uxl_set_num(BK, sh, row, col, cv.num); break;
                case UD_XV_STR:  uxl_set_str(BK, sh, row, col, cv.str ? cv.str : ""); break;
                case UD_XV_BOOL: uxl_set_bool(BK, sh, row, col, cv.num != 0); break;
                case UD_XV_ERR:  uxl_set_err(BK, sh, row, col, xls_err_to_uxl(cv.err)); break;
                default: break;
                }
            }
        }
        ud_xls_close(x);
        uxl_recalc(BK);
        ok = 1;
    }
    ud_cfb_close(c);
    ud_zip_close(z);
    if (ok) {
        g_cur_r = g_cur_c = g_sel_r = g_sel_c = 0;
        g_top_r = g_left_c = 0;
        g_sheet = 0;
        g_editing = 0; g_edit[0] = 0;
    }
    return ok;
}

static int save_book(int vol, const char *name)
{
    ud_xlsw *w;
    unsigned char *out = 0;
    long len = 0;
    int ok = 0, ns, si;

    if (!BK) return 0;
    ud_set_alloc(malloc, free);
    um_set_alloc(malloc, free);
    w = ud_xlsw_new();
    if (!w) return 0;
    ns = uxl_sheets(BK);
    for (si = 0; si < ns; si++) {
        int sh = ud_xlsw_sheet(w, uxl_sheet_name(BK, si));
        int n = uxl_count(BK, si), i;
        if (sh < 0) break;
        for (i = 0; i < n; i++) {
            int row = 0, col = 0;
            uxl_val v;
            const char *f;
            if (!uxl_at(BK, si, i, &row, &col, &v)) continue;
            f = uxl_formula(BK, si, row, col);
            if (f && *f) {
                ud_xcell cached;
                cached.kind = UD_XV_NUM; cached.num = 0; cached.str = 0;
                cached.err = 0; cached.xf = 0; cached.formula = 1; cached.ftext = 0;
                if (v.kind == UXL_NUM || v.kind == UXL_BOOL) cached.num = v.num;
                else if (v.kind == UXL_STR) {
                    cached.kind = UD_XV_STR; cached.str = uxl_pool(BK, v.str);
                }
                if (ud_xlsw_formula(w, sh, row, col, f, &cached)) continue;
                /* an expression unodoc's compiler refuses must not lose the
                 * cell: fall through and write the value it produced */
            }
            switch (v.kind) {
            case UXL_NUM:  ud_xlsw_num(w, sh, row, col, v.num); break;
            case UXL_STR:  ud_xlsw_str(w, sh, row, col, uxl_pool(BK, v.str)); break;
            case UXL_BOOL: ud_xlsw_bool(w, sh, row, col, v.num != 0); break;
            case UXL_ERR:  ud_xlsw_err(w, sh, row, col, uxl_err_to_xls(v.err)); break;
            default:       ud_xlsw_blank(w, sh, row, col); break;
            }
        }
    }
    out = name_is_ooxml(name, "xlsx") ? ud_xlsxw_save(w, &len)
                                     : ud_xlsw_save(w, &len);
    if (out && len > 0) ok = uno_fs_write(vol, name, out, len) ? 1 : 0;
    ud_free(out);
    ud_xlsw_free(w);
    if (ok) g_changed = 0;
    return ok;
}

/* ---- the workbook's name, in the title bar ---------------------------------- */
static void set_title(void)
{
    const char *pre = "UnoCalc - ", *nm = g_name[0] ? g_name : "Book1";
    int k = 0;
    while (*pre) g_title[k++] = *pre++;
    while (*nm && k < (int)sizeof g_title - 1) g_title[k++] = *nm++;
    g_title[k] = 0;
    if (g_win) g_win->title = g_title;
}

static void msg(const char *text)
{
    uod_msgbox(&DL, "UnoCalc", text, UOD_MB_OK, pc64_shell_workarea_w(),
               pc64_shell_workarea_h());
    g_dlg = DLG_MSG;
}

static void open_dialog(int save)
{
    uof_set_fs(&kFs);
    uof_open(&DL, save, kTypes, 2, pc64_shell_workarea_w(),
             pc64_shell_workarea_h());
    g_dlg = save ? DLG_SAVE : DLG_OPEN;
}

/* Open (vol, name), or say why not.  load_book replaces the workbook only
 * with one that parsed, so a failure leaves what was open, open. */
static void open_file(int vol, const char *name)
{
    char m[320];
    int k = 0;
    const char *t;
    if (load_book(vol, name)) {
        a_cpy(g_name, name, (int)sizeof g_name);
        g_vol = vol;
        g_changed = 0;
        set_title();
        return;
    }
    for (t = name; *t && k < 250; t++) m[k++] = *t;
    for (t = " is not a workbook UnoCalc can open."; *t; t++) m[k++] = *t;
    m[k] = 0;
    msg(m);
}

static void new_book(void)
{
    BK = uxl_new();
    g_cur_r = g_cur_c = 0; g_sel_r = g_sel_c = 0;
    g_top_r = g_left_c = 0; g_sheet = 0;
    g_editing = 0; g_edit[0] = 0;
    /* a new workbook has no file yet: without clearing the name, the next
     * Save silently overwrote whatever was open before */
    g_name[0] = 0; g_vol = 0;
    g_changed = 0;
    set_title();
}

/* ---- the unsaved-changes guard's hooks (uoapp.h) ------------------------------ */
static void commit_edit(void);
static int calc_dirty(void) { return g_changed || g_editing; }
static const char *calc_doc_name(void) { return g_name[0] ? g_name : "Book1"; }
static int calc_save(void)
{
    commit_edit();
    if (!g_name[0]) { open_dialog(1); return 2; }
    if (save_book(g_vol, g_name)) return 1;
    msg("Could not write the workbook.");
    return 0;
}
static void calc_proceed(int action)
{
    switch (action) {
    case UOA_NEW:       new_book(); break;
    case UOA_OPEN_DLG:  open_dialog(0); break;
    case UOA_OPEN_FILE: open_file(uoa_open_vol(), uoa_open_name()); break;
    default: break;
    }
}
static int calc_frame_w(void) { return pc64_shell_workarea_w(); }
static int calc_frame_h(void) { return pc64_shell_workarea_h(); }
static void calc_prompted(void) { g_dlg = DLG_GUARD; pc64_shell_dirty(); }
static const uoa_app kGuard = {
    "UnoCalc", calc_dirty, calc_doc_name, calc_save, calc_proceed,
    &DL, calc_frame_w, calc_frame_h, calc_prompted
};

/* ---- commands ---------------------------------------------------------------- */
/* Is the formula being typed at a point where a reference may follow?  Excel
 * says yes after an operator, an opening bracket, a separator or the leading
 * '=' itself, and no straight after an operand - where a click means "select
 * that cell instead", not "and also this one". */
static int point_ready(const char *e)
{
    int n = a_len(e);
    char c;
    if (n <= 0) return 0;
    c = e[n - 1];
    return c == '=' || c == '+' || c == '-' || c == '*' || c == '/' ||
           c == '^' || c == '(' || c == ',' || c == ':' || c == '&' ||
           c == '<' || c == '>' || c == '%' || c == ' ';
}

/* What typing `t` into a cell means - the one rule, shared by leaving an
 * edited cell and by a paste. */
static void set_from_text(int r, int c, const char *t)
{
    if (!t[0]) { uxl_clear(BK, g_sheet, r, c); }
    else if (t[0] == '=') {
        if (!uxl_set_formula(BK, g_sheet, r, c, t))
            uxl_set_str(BK, g_sheet, r, c, t);
    } else {
        /* a bare number is a number; anything else is text - the same test
         * Excel applies as you leave the cell */
        const char *p = t;
        double v = 0, frac = 0.1;
        int neg = 0, digits = 0;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; digits++; }
        if (*p == '.') { p++;
            while (*p >= '0' && *p <= '9') { v += (*p - '0') * frac; frac /= 10; p++; digits++; } }
        if (digits && !*p) uxl_set_num(BK, g_sheet, r, c, neg ? -v : v);
        else uxl_set_str(BK, g_sheet, r, c, t);
    }
}

static void commit_edit(void)
{
    if (!g_editing) return;
    g_editing = 0;
    g_changed = 1;
    set_from_text(g_cur_r, g_cur_c, g_edit);
    g_edit[0] = 0;
    uxl_recalc(BK);
}

/* ---- the clipboard ------------------------------------------------------------
 * A range goes out as tab-separated rows of what the cells SHOW, which is
 * what Excel puts on the clipboard for another program and what a pasted
 * table in any word processor or text editor expects.
 *
 * Between UnoCalc windows - or back into this one - that would turn every
 * formula into its value.  So the copy also keeps each cell's SOURCE (its
 * formula, or its typed value), and a paste whose clipboard text is still
 * exactly the text this copy produced uses the sources instead, with every
 * relative reference moved by the distance pasted, as Excel does: =A1*2
 * copied down a row becomes =A2*2, and $A$1 stays put. */
static char *g_cb_text;             /* what we put on the clipboard (UTF-8)  */
static char *g_cb_src;              /* the sources: '\t' / '\n' separated   */
static int   g_cb_r, g_cb_c;        /* the top-left cell they came from      */

static void sel_box(int *r0, int *c0, int *r1, int *c1)
{
    *r0 = g_sel_r < g_cur_r ? g_sel_r : g_cur_r;
    *r1 = g_sel_r > g_cur_r ? g_sel_r : g_cur_r;
    *c0 = g_sel_c < g_cur_c ? g_sel_c : g_cur_c;
    *c1 = g_sel_c > g_cur_c ? g_sel_c : g_cur_c;
}

/* a cell's source: its formula, else what typing it back would take */
static int cell_source(int r, int c, char *out, int cap)
{
    uxl_val v;
    const char *f = uxl_formula(BK, g_sheet, r, c);
    out[0] = 0;
    if (f && *f) { a_cpy(out, f, cap); return a_len(out); }
    if (!uxl_get(BK, g_sheet, r, c, &v)) return 0;
    switch (v.kind) {
    case UXL_NUM:  return uxl_general(v.num, out, cap);
    case UXL_STR:  a_cpy(out, uxl_pool(BK, v.str), cap); return a_len(out);
    default:       return uxl_text(BK, g_sheet, r, c, out, cap);
    }
}

/* Move every relative A1 reference in formula `f` by (dr, dc).  A token is a
 * reference when it parses as one (uxl_a1_parse) and is not a function name
 * (followed by '(') or part of a longer name; text inside "quotes" is left
 * alone.  A reference moved off the sheet becomes #REF!, as in Excel. */
static void shift_refs(const char *f, int dr, int dc, char *out, int cap)
{
    int i = 0, k = 0, instr = 0;
    while (f[i] && k < cap - 1) {
        char ch = f[i];
        int start = !instr && (ch == '$' || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) &&
                    !(i > 0 && ((f[i-1] >= 'A' && f[i-1] <= 'Z') || (f[i-1] >= 'a' && f[i-1] <= 'z') ||
                                (f[i-1] >= '0' && f[i-1] <= '9') || f[i-1] == '_' || f[i-1] == '.'));
        if (ch == '"') instr = !instr;
        if (start) {
            char tok[16];
            int j = i, n = 0, row, col, ar, ac;
            while (n < 15 && (f[j] == '$' || (f[j] >= 'A' && f[j] <= 'Z') ||
                   (f[j] >= 'a' && f[j] <= 'z') || (f[j] >= '0' && f[j] <= '9') || f[j] == '_'))
                tok[n++] = f[j++];
            tok[n] = 0;
            if (f[j] != '(' && f[j] != '!' && uxl_a1_parse(tok, &row, &col, &ar, &ac)) {
                char w[16];
                int nr = ar ? row : row + dr, nc = ac ? col : col + dc, m;
                if (nr < 0 || nr >= UXL_ROWS || nc < 0 || nc >= UXL_COLS) a_cpy(w, "#REF!", 16);
                else uxl_a1_write(nr, nc, ar, ac, w, 16);
                for (m = 0; w[m] && k < cap - 1; m++) out[k++] = w[m];
            } else {
                int m;
                for (m = 0; m < n && k < cap - 1; m++) out[k++] = tok[m];
            }
            i = j;
            continue;
        }
        out[k++] = ch;
        i++;
    }
    out[k] = 0;
}

/* One pass over the range: `t`/`s` NULL just measures.  Returns the larger
 * of the two lengths, so one measuring pass sizes both buffers. */
static long emit_range(int r0, int c0, int r1, int c1, char *t, char *s)
{
    int r, c;
    long k = 0, ks = 0;
    char cell[256], u8[768];
    for (r = r0; r <= r1; r++) {
        for (c = c0; c <= c1; c++) {
            int n, m;
            if (c > c0) { if (t) t[k] = '\t'; if (s) s[ks] = '\t'; k++; ks++; }
            if (uxl_text(BK, g_sheet, r, c, cell, (int)sizeof cell) <= 0) cell[0] = 0;
            n = uoa_to_utf8(cell, -1, u8, (int)sizeof u8);
            for (m = 0; m < n; m++, k++) if (t) t[k] = u8[m];
            cell_source(r, c, cell, (int)sizeof cell);
            for (m = 0; cell[m]; m++, ks++) if (s) s[ks] = cell[m];
        }
        if (t) t[k] = '\n'; if (s) s[ks] = '\n';
        k++; ks++;
    }
    if (t) t[k] = 0;
    if (s) s[ks] = 0;
    return k > ks ? k : ks;
}

static int copy_range(void)
{
    int r0, c0, r1, c1;
    long cap;
    char *t, *s;
    commit_edit();
    sel_box(&r0, &c0, &r1, &c1);
    if ((long)(r1 - r0 + 1) * (c1 - c0 + 1) > 1000000L) {
        msg("That range is too large to copy.");
        return 0;
    }
    cap = emit_range(r0, c0, r1, c1, 0, 0) + 1;
    t = (char *)malloc((unsigned long)cap);
    s = (char *)malloc((unsigned long)cap);
    if (!t || !s) { if (t) free(t); if (s) free(s); return 0; }
    emit_range(r0, c0, r1, c1, t, s);
    if (g_cb_text) free(g_cb_text);
    if (g_cb_src) free(g_cb_src);
    g_cb_text = t; g_cb_src = s; g_cb_r = r0; g_cb_c = c0;
    return uoa_clip_set(t);
}

static void clear_range(void)
{
    int r0, c0, r1, c1, r, c;
    sel_box(&r0, &c0, &r1, &c1);
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++) uxl_clear(BK, g_sheet, r, c);
    g_changed = 1;
    uxl_recalc(BK);
}

/* Paste at the cursor: rows by '\n', cells by '\t'. */
static void paste_range(void)
{
    char *u = uoa_clip_get(), *src, *line;
    int ours, r, c, dr, dc, last_r, last_c;
    long n = 0, i;
    if (!u) return;
    commit_edit();
    ours = g_cb_text && g_cb_src;
    for (i = 0; ours && (u[i] || g_cb_text[i]); i++)
        if (u[i] != g_cb_text[i]) ours = 0;           /* still our copy?     */
    while (u[n]) n++;
    if (ours) src = g_cb_src;
    else {
        src = (char *)malloc((unsigned long)n + 1);
        if (!src) { free(u); return; }
        uoa_from_utf8(u, src, n + 1);               /* CP-1252 is never longer */
    }
    dr = g_cur_r - g_cb_r; dc = g_cur_c - g_cb_c;
    r = g_cur_r; c = g_cur_c; last_r = r; last_c = c;
    line = src;
    for (i = 0;; i++) {
        char ch = src[i];
        if (ch == '\t' || ch == '\n' || ch == 0) {
            char cell[256], fx[512];
            long len = &src[i] - line;
            if (len > (long)sizeof cell - 1) len = (long)sizeof cell - 1;
            { long q; for (q = 0; q < len; q++) cell[q] = line[q]; }
            cell[len] = 0;
            /* a trailing line end is not one more empty row */
            if (!(ch == 0 && len == 0 && c == g_cur_c) && r < UXL_ROWS && c < UXL_COLS) {
                if (ours && cell[0] == '=') { shift_refs(cell, dr, dc, fx, (int)sizeof fx);
                                              set_from_text(r, c, fx); }
                else set_from_text(r, c, cell);
                last_r = r; last_c = c;
            }
            if (ch == 0) break;
            line = &src[i + 1];
            if (ch == '\t') c++;
            else { r++; c = g_cur_c; }
        }
    }
    if (!ours) free(src);
    free(u);
    /* the pasted block ends up selected, as in Excel */
    g_sel_r = g_cur_r; g_sel_c = g_cur_c;
    g_cur_r = last_r; g_cur_c = last_c;
    g_changed = 1;
    uxl_recalc(BK);
}

static void set_fmt_all(int fmt)
{
    int r, c;
    int r0 = g_sel_r < g_cur_r ? g_sel_r : g_cur_r;
    int r1 = g_sel_r > g_cur_r ? g_sel_r : g_cur_r;
    int c0 = g_sel_c < g_cur_c ? g_sel_c : g_cur_c;
    int c1 = g_sel_c > g_cur_c ? g_sel_c : g_cur_c;
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++) uxl_set_fmt(BK, g_sheet, r, c, fmt);
    g_changed = 1;
}

static void do_command(int cmd)
{
    switch (cmd) {
    /* New, Open and Exit drop the workbook: the guard asks first */
    case C_NEW:  uoa_request(UOA_NEW); break;
    case C_OPEN: uoa_request(UOA_OPEN_DLG); break;
    case C_EXIT: uoa_exit(); break;
    case C_SAVE: calc_save(); break;            /* back where it came from */
    case C_COPY:     copy_range(); break;
    /* Cut takes the cells away now; Excel's marquee-then-move is a later
     * refinement, and what is on the clipboard is the same either way */
    case C_CUT:      if (copy_range()) clear_range(); break;
    case C_PASTE:    paste_range(); break;
    case C_CURRENCY: set_fmt_all(UXL_FMT_CURRENCY); break;
    case C_PERCENT:  set_fmt_all(UXL_FMT_PCT); break;
    case C_COMMA:    set_fmt_all(UXL_FMT_THOUS2); break;
    case C_DEC_MORE: set_fmt_all(UXL_FMT_2DP); break;
    case C_DEC_LESS: set_fmt_all(UXL_FMT_INT); break;
    case C_RECALC:   uxl_recalc(BK); break;
    case C_SUM: {
        /* AutoSum: the run of numbers directly above, Excel's own guess */
        int top = g_cur_r;
        char f[32], a[12], bb[12];
        int n = 0, i = 0;
        while (top > 0) {
            uxl_val v;
            if (!uxl_get(BK, g_sheet, top - 1, g_cur_c, &v) || v.kind != UXL_NUM) break;
            top--;
        }
        if (top == g_cur_r) break;
        uxl_a1_write(top, g_cur_c, 0, 0, a, (int)sizeof a);
        uxl_a1_write(g_cur_r - 1, g_cur_c, 0, 0, bb, (int)sizeof bb);
        f[n++] = '='; f[n++] = 'S'; f[n++] = 'U'; f[n++] = 'M'; f[n++] = '(';
        while (a[i]) f[n++] = a[i++];
        f[n++] = ':';
        i = 0;
        while (bb[i]) f[n++] = bb[i++];
        f[n++] = ')'; f[n] = 0;
        uxl_set_formula(BK, g_sheet, g_cur_r, g_cur_c, f);
        uxl_recalc(BK);
        g_changed = 1;
        break;
    }
    case C_FUNCS: {
        char msg[64], n[12];
        int k = 0, i = 0;
        const char *pre = "Functions available: ";
        while (*pre) msg[k++] = *pre++;
        a_num(uxl_func_count(), n);
        while (n[i]) msg[k++] = n[i++];
        msg[k] = 0;
        uod_msgbox(&DL, "Paste Function", msg, UOD_MB_OK,
                   pc64_shell_workarea_w(), pc64_shell_workarea_h());
        g_dlg = 1;
        break;
    }
    case C_ABOUT:
        uod_msgbox(&DL, "About UnoCalc",
                   "UnoCalc - a Microsoft Excel 97 clone for UnoDOS.",
                   UOD_MB_OK, pc64_shell_workarea_w(),
                   pc64_shell_workarea_h());
        g_dlg = 1;
        break;
    default: break;
    }
    pc64_shell_dirty();
}

/* ---- events ------------------------------------------------------------------ */
static void scroll_to_cursor(void)
{
    int gx, gy, gw, gh, cols, rows;
    grid_rect(&gx, &gy, &gw, &gh);
    cols = (gw - head_w()) / cell_w();
    rows = (gh - cell_h()) / cell_h();
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (g_cur_c < g_left_c) g_left_c = g_cur_c;
    if (g_cur_c >= g_left_c + cols) g_left_c = g_cur_c - cols + 1;
    if (g_cur_r < g_top_r) g_top_r = g_cur_r;
    if (g_cur_r >= g_top_r + rows) g_top_r = g_cur_r - rows + 1;
}

/* Whatever a closing dialog meant.  Shared, because a dialog can be dismissed
 * by the mouse (app_event) or by Enter/Esc (uw_key). */
static void dialog_closed(void)
{
    int res = uod_result(&DL), kind = g_dlg;
    g_dlg = DLG_NONE;
    if (kind == DLG_GUARD) { uoa_prompt_closed(); return; }
    if (kind == DLG_SAVE) {
        int ok = 0;
        if (res == UOD_ID_OK) {
            char nm[256];
            a_cpy(nm, uof_name(), (int)sizeof nm);
            ensure_ext(nm, (int)sizeof nm, uof_type());
            ok = save_book(uof_volume(), nm);
            if (ok) {
                a_cpy(g_name, nm, (int)sizeof g_name);
                g_vol = uof_volume();
                set_title();
            } else msg("Could not write the workbook.");
        }
        uoa_save_as_done(ok);               /* the guard's Save As, if it was */
        return;
    }
    if (res != UOD_ID_OK) return;
    if (kind == DLG_OPEN) open_file(uof_volume(), uof_name());
}

static int app_event(struct unoui_widget *w, const void *evp, void *ctx)
{
    const unoui_event *e = (const unoui_event *)evp;
    int cmd = 0;
    (void)ctx; (void)w;
    if (!g_have_rect) return 0;

    if (g_dlg) {
        uod_handle(&DL, e);
        if (g_dlg == DLG_OPEN || g_dlg == DLG_SAVE) uof_sync(&DL);
        if (!uod_is_open(&DL)) dialog_closed();
        pc64_shell_dirty();
        return 1;
    }
    CH.x = g_rect.x; CH.y = g_rect.y; CH.w = g_rect.w; CH.h = g_rect.h;
    if (uoc_handle(&CH, e, &cmd)) {
        if (cmd) do_command(cmd);
        pc64_shell_dirty();
        return 1;
    }
    if (e->kind == UI_EV_WHEEL) {
        g_top_r += e->wheel * 3;
        if (g_top_r < 0) g_top_r = 0;
        pc64_shell_dirty();
        return 1;
    }
    if (e->kind == UI_EV_MOUSE_DOWN) {
        int gx, gy, gw, gh;
        grid_rect(&gx, &gy, &gw, &gh);
        /* a sheet tab */
        if (e->y >= g_rect.y + g_rect.h - uob_status_h() - tabs_h() &&
            e->y <  g_rect.y + g_rect.h - uob_status_h()) {
            int i, x = g_rect.x + 4;
            for (i = 0; i < uxl_sheets(BK); i++) {
                int tw = dtext_w(uxl_sheet_name(BK, i)) + 12;
                if (e->x >= x && e->x < x + tw) {
                    commit_edit();
                    g_sheet = i;
                    pc64_shell_dirty();
                    return 1;
                }
                x += tw + 2;
            }
            return 1;
        }
        if (e->x >= gx + head_w() && e->y >= gy + cell_h() &&
            e->x < gx + gw && e->y < gy + gh) {
            int pr = g_top_r + (e->y - gy - cell_h()) / cell_h();
            int pc = g_left_c + (e->x - gx - head_w()) / cell_w();
            /* POINT MODE.  While a formula is being typed and the last thing
             * typed was an operator, clicking a cell PUTS ITS REFERENCE IN
             * rather than moving the selection - which is how anyone actually
             * writes =A1*A2 in Excel, and without it the only way to name a
             * cell is to know its address and type it. */
            if (g_editing && g_edit[0] == '=' && point_ready(g_edit)) {
                char a1[16];
                int n = a_len(g_edit);
                uxl_a1_write(pr, pc, 0, 0, a1, (int)sizeof a1);
                a_cpy(g_edit + n, a1, (int)sizeof g_edit - n);
                pc64_shell_dirty();
                return 1;
            }
            commit_edit();
            g_cur_c = g_left_c + (e->x - gx - head_w()) / cell_w();
            g_cur_r = g_top_r + (e->y - gy - cell_h()) / cell_h();
            if (!(e->mods & UI_MOD_SHIFT)) { g_sel_r = g_cur_r; g_sel_c = g_cur_c; }
            pc64_shell_dirty();
            return 1;
        }
    }
    return 0;
}

static void uw_build(unoui_window *win)
{
    int w = pc64_shell_workarea_w() - 40, h = pc64_shell_workarea_h() - 60;
    if (w < 380) w = 380;
    if (h < 260) h = 260;
    unoui_window_init(win, g_title, 24, 20, w, h);
    g_canvas.draw = app_draw;
    g_canvas.event = app_event;
    g_canvas.ctx = 0;
    unoui_widget_fill(unoui_add_canvas(win, 0, 0, w - 12, h - 28, &g_canvas));
    win->flags |= UI_WIN_RESIZE;
    g_win = win;
    g_cidx = 0;
}
static int uw_action(const unoui_action *a) { (void)a; return 0; }

/* how many rows fit in the grid right now - the page for PgUp/PgDn, and the
 * same arithmetic scroll_to_cursor uses so a page always lands on a full screen */
static int page_rows(void)
{
    int gx, gy, gw, gh, rows;
    grid_rect(&gx, &gy, &gw, &gh);
    rows = (gh - cell_h()) / cell_h();
    return rows < 1 ? 1 : rows;
}

/* Move the cursor by (dr, dc), committing whatever was being typed first.
 * With Shift held (an arrow key only), the range's anchor stays put and the
 * selection grows, as in Excel.
 * Committing is what a spreadsheet does: an arrow out of a half-typed cell
 * means "that value, and now I am over there", not "throw it away". */
static void move_cursor_ex(int dr, int dc, int extend)
{
    commit_edit();
    g_cur_r += dr; g_cur_c += dc;
    if (g_cur_r < 0) g_cur_r = 0;
    if (g_cur_c < 0) g_cur_c = 0;
    if (g_cur_r >= UXL_ROWS) g_cur_r = UXL_ROWS - 1;
    if (g_cur_c >= UXL_COLS) g_cur_c = UXL_COLS - 1;
    if (!extend) { g_sel_r = g_cur_r; g_sel_c = g_cur_c; }
    scroll_to_cursor();
    pc64_shell_dirty();
}

static void move_cursor(int dr, int dc) { move_cursor_ex(dr, dc, 0); }

static int uw_key(int uni, int scan, int ctrl)
{
    unoui_event e;
    int i, shift = (uoa_key_mods() & UI_MOD_SHIFT) != 0;
    if (!BK) return 0;
    if (g_dlg) {
        for (i = 0; i < (int)sizeof e; i++) ((char *)&e)[i] = 0;
        if (uni >= ' ') { e.kind = UI_EV_CHAR; e.ch = uni; }
        else if (uni == '\r' || uni == '\n') { e.kind = UI_EV_KEY; e.key = UI_KEY_ENTER; }
        else if (uni == 27) { e.kind = UI_EV_KEY; e.key = UI_KEY_ESC; }
        else return 0;
        uod_handle(&DL, &e);
        if (g_dlg == DLG_OPEN || g_dlg == DLG_SAVE) uof_sync(&DL);
        if (!uod_is_open(&DL)) dialog_closed();
        pc64_shell_dirty();
        return 1;
    }
    /* Navigation, from the firmware SCAN code - these arrive with uni == 0, so
     * everything below (which reads uni) never saw them and the selection could
     * only ever move down, one Enter at a time. Ctrl+Home/End jump to the far
     * corners, the way every spreadsheet since Multiplan has. */
    switch (scan) {
    case 0x01: move_cursor_ex(-1, 0, shift); return 1;            /* up      */
    case 0x02: move_cursor_ex(+1, 0, shift); return 1;            /* down    */
    case 0x03: move_cursor_ex(0, +1, shift); return 1;            /* right   */
    case 0x04: move_cursor_ex(0, -1, shift); return 1;            /* left    */
    case 0x05:                                                    /* home    */
        move_cursor(ctrl ? -g_cur_r : 0, -g_cur_c); return 1;
    case 0x06:                                                    /* end     */
        move_cursor(ctrl ? UXL_ROWS : 0, UXL_COLS); return 1;
    case 0x09: move_cursor(-page_rows(), 0); return 1;            /* page up */
    case 0x0A: move_cursor(+page_rows(), 0); return 1;            /* page dn */
    }
    if (ctrl) {
        int c = uni;
        if (c >= 1 && c <= 26) c += 'a' - 1;
        if (c == 's') { do_command(C_SAVE); return 1; }
        if (c == 'c') { do_command(C_COPY); return 1; }
        if (c == 'x') { do_command(C_CUT); return 1; }
        if (c == 'v') { do_command(C_PASTE); return 1; }
        if (c == 'o') { do_command(C_OPEN); return 1; }
        if (c == 'n') { do_command(C_NEW); return 1; }
        return 0;
    }
    if (uni == 27) { g_editing = 0; g_edit[0] = 0; pc64_shell_dirty(); return 1; }
    /* Enter and Tab are just moves, and go through the same helper - two copies
     * of "commit, step, reselect, scroll" is how they drift apart. */
    if (uni == '\r' || uni == '\n') { move_cursor(+1, 0); return 1; }
    if (uni == '\t')                { move_cursor(0, +1); return 1; }
    if (uni == 8) {
        if (g_editing) {
            int n = a_len(g_edit);
            if (n) g_edit[n - 1] = 0;
        } else { uxl_clear(BK, g_sheet, g_cur_r, g_cur_c); g_changed = 1; }
        pc64_shell_dirty();
        return 1;
    }
    if (uni >= ' ') {
        /* typed Unicode -> the workbook's CP-1252; what it cannot hold is '?' */
        int n, b = uoa_uc_to_1252(uni);
        if (b < 0) b = '?';
        if (!g_editing) { g_editing = 1; g_edit[0] = 0; }
        n = a_len(g_edit);
        if (n < (int)sizeof g_edit - 1) { g_edit[n] = (char)b; g_edit[n + 1] = 0; }
        pc64_shell_dirty();
        return 1;
    }
    return 0;
}

static void uw_frame(void) { }
static void uw_opened(void)
{
    if (!BK) BK = uxl_new();
    uoc_icons_install();
    uoc_init(&CH, kMenus, 6, kBars, 2, 0, 0, 400, 300);
    ST.page = "A1";
    ST.pos  = "Sum=0";
    a_cpy(g_edit, "", (int)sizeof g_edit);
    uoa_register(&kGuard);
}
static void uw_closed(void) { }
static int  uw_canvas_index(void) { return g_cidx; }

/* what the shell shows for this app, carried in the module (uno_appdesc.h) */
UNO_APP_DESC("id: uocalc\n"
             "name: UnoCalc\n"
             "icon: uocalc\n"
             "cat: tools\n"
             "rank: 30\n");

static const UnoUuiApp kApp = {
    UNO_UUIAPP_ABI, "UnoCalc",
    uw_build, uw_action, uw_key, uw_frame, uw_opened, uw_closed,
    uw_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved)
{ (void)reserved; return &kApp; }
