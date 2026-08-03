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
#include "unoui.h"
#include "fb.h"
#include "uochrome.h"
#include "uoicons.h"
#include "uodlg.h"
#include "uobars.h"
#include "uofile.h"
#include "uocalc.h"
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
static int  g_dlg;
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
static const char *const kTypes[] = { "Microsoft Excel Workbook (*.xls)" };

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
        fb_text(g_rect.x + nb + 11, y + 3, t, k->text, -1);
        if (g_editing)
            fb_vline(g_rect.x + nb + 11 + fb_text_w(g_edit), y + 3,
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
                int w = fb_text_w(buf);
                /* numbers right-align, text left - Excel's default and the
                 * fastest way to see that a "number" arrived as text */
                uxl_get(BK, g_sheet, row, col, &v);
                fb_text(x + ((v.kind == UXL_NUM || v.kind == UXL_ERR)
                             ? cw - 3 - w : 3), y + 2, buf, k->text, -1);
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
        int w = fb_text_w(nm) + 12;
        int on = (i == g_sheet);
        fb_fill_rect(x, y + (on ? 0 : 2), w, tabs_h() - (on ? 0 : 2),
                     on ? k->hilight : k->face);
        uoc_bevel(x, y + (on ? 0 : 2), w, tabs_h() - (on ? 0 : 2),
                  k->hilight, k->shadow, 1);
        fb_text(x + 6, y + 3, nm, k->text, -1);
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

/* ---- commands ---------------------------------------------------------------- */
static void commit_edit(void)
{
    if (!g_editing) return;
    g_editing = 0;
    if (!g_edit[0]) { uxl_clear(BK, g_sheet, g_cur_r, g_cur_c); }
    else if (g_edit[0] == '=') {
        if (!uxl_set_formula(BK, g_sheet, g_cur_r, g_cur_c, g_edit))
            uxl_set_str(BK, g_sheet, g_cur_r, g_cur_c, g_edit);
    } else {
        /* a bare number is a number; anything else is text - the same test
         * Excel applies as you leave the cell */
        const char *p = g_edit;
        double v = 0, frac = 0.1;
        int neg = 0, digits = 0;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; digits++; }
        if (*p == '.') { p++;
            while (*p >= '0' && *p <= '9') { v += (*p - '0') * frac; frac /= 10; p++; digits++; } }
        if (digits && !*p) uxl_set_num(BK, g_sheet, g_cur_r, g_cur_c, neg ? -v : v);
        else uxl_set_str(BK, g_sheet, g_cur_r, g_cur_c, g_edit);
    }
    g_edit[0] = 0;
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
}

static void do_command(int cmd)
{
    switch (cmd) {
    case C_NEW: BK = uxl_new(); g_cur_r = g_cur_c = 0; g_sel_r = g_sel_c = 0; break;
    case C_OPEN:
        uof_set_fs(&kFs);
        uof_open(&DL, 0, kTypes, 1, pc64_shell_workarea_w(),
                 pc64_shell_workarea_h());
        g_dlg = 1;
        break;
    case C_SAVE:
        uod_msgbox(&DL, "UnoCalc",
                   "Saving .xls is not in this build yet.", UOD_MB_OK,
                   pc64_shell_workarea_w(), pc64_shell_workarea_h());
        g_dlg = 1;
        break;
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

static int app_event(struct unoui_widget *w, const void *evp, void *ctx)
{
    const unoui_event *e = (const unoui_event *)evp;
    int cmd = 0;
    (void)ctx; (void)w;
    if (!g_have_rect) return 0;

    if (g_dlg) {
        uod_handle(&DL, e);
        uof_sync(&DL);
        if (!uod_is_open(&DL)) g_dlg = 0;
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
                int tw = fb_text_w(uxl_sheet_name(BK, i)) + 12;
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
    unoui_window_init(win, "UnoCalc - Book1", 24, 20, w, h);
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
    if (!BK) return 0;
    if (g_dlg) {
        for (i = 0; i < (int)sizeof e; i++) ((char *)&e)[i] = 0;
        if (uni >= ' ') { e.kind = UI_EV_CHAR; e.ch = uni; }
        else if (uni == '\r' || uni == '\n') { e.kind = UI_EV_KEY; e.key = UI_KEY_ENTER; }
        else if (uni == 27) { e.kind = UI_EV_KEY; e.key = UI_KEY_ESC; }
        else return 0;
        uod_handle(&DL, &e);
        uof_sync(&DL);
        if (!uod_is_open(&DL)) g_dlg = 0;
        pc64_shell_dirty();
        return 1;
    }
    if (ctrl) {
        int c = uni;
        if (c >= 1 && c <= 26) c += 'a' - 1;
        if (c == 's') { do_command(C_SAVE); return 1; }
        if (c == 'o') { do_command(C_OPEN); return 1; }
        if (c == 'n') { do_command(C_NEW); return 1; }
        return 0;
    }
    if (uni == 27) { g_editing = 0; g_edit[0] = 0; pc64_shell_dirty(); return 1; }
    if (uni == '\r' || uni == '\n') {
        commit_edit();
        if (g_cur_r + 1 < UXL_ROWS) g_cur_r++;
        g_sel_r = g_cur_r; g_sel_c = g_cur_c;
        scroll_to_cursor();
        pc64_shell_dirty();
        return 1;
    }
    if (uni == '\t') {
        commit_edit();
        if (g_cur_c + 1 < UXL_COLS) g_cur_c++;
        g_sel_r = g_cur_r; g_sel_c = g_cur_c;
        scroll_to_cursor();
        pc64_shell_dirty();
        return 1;
    }
    if (uni == 8) {
        if (g_editing) {
            int n = a_len(g_edit);
            if (n) g_edit[n - 1] = 0;
        } else uxl_clear(BK, g_sheet, g_cur_r, g_cur_c);
        pc64_shell_dirty();
        return 1;
    }
    if (uni >= ' ') {
        int n;
        if (!g_editing) { g_editing = 1; g_edit[0] = 0; }
        n = a_len(g_edit);
        if (n < (int)sizeof g_edit - 1) { g_edit[n] = (char)uni; g_edit[n + 1] = 0; }
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
}
static void uw_closed(void) { }
static int  uw_canvas_index(void) { return g_cidx; }

static const UnoUuiApp kApp = {
    UNO_UUIAPP_ABI, "UnoCalc",
    uw_build, uw_action, uw_key, uw_frame, uw_opened, uw_closed,
    uw_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved)
{ (void)reserved; return &kApp; }
