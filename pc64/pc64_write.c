/* ===========================================================================
 * UnoDOS/pc64 - the Editor: a WordPad / Windows-3.1-Write-class word
 * processor for the unoui shell.
 *
 * A real rich-text document model - not the toolkit textarea: every character
 * carries a style word (bold / italic / underline, one of 4 faces, one of 8
 * sizes, paragraph alignment), the document word-wraps to the window, and the
 * text renders through the TrueType engine (pc64_font) at per-run pixel sizes.
 *
 * Window anatomy (all unoui widgets except the document itself):
 *   menu bar     File / Edit / Format / Font / Size / Help
 *   toolbar      New Open Save | face + size dropdowns | B I U | L C R
 *   find bar     Find [field] Next   Replace [field] Replace All
 *   document     one UI_CANVAS: ruler + wrapped rich text + scrollbar + status
 *
 * Layout, caret math and drawing all use the same per-character advance cache
 * (uno_font_text_w_styled of the single glyph), so a click always lands on the
 * glyph that was painted. Files are saved as UWD (text + RLE style runs) or as
 * plain .TXT (styles dropped) through the unified uno_fs volumes.
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "pc64_font.h"
#include "pc64_fs.h"
#include "pc64_icons.h"     /* pc64_shell_theme */
#include "pc64_native.h"    /* uno_native_rdtsc (slow-draw autopsy)  */
#include "uno_debug.h"      /* uno_dbg_log / uno_dbg_cyc_to_us       */
#include <string.h>

/* shell services (pc64_uui.c) */
void pc64_shell_add_window(unoui_window *w);
void pc64_shell_remove_window(unoui_window *w);
void pc64_shell_focus_window(unoui_window *w);
void pc64_shell_dirty(void);
int  pc64_shell_workarea_w(void);
int  pc64_shell_workarea_h(void);

/* ---- widget ids (300..349 reserved for the Editor) ----------------------- */
enum {
    WID_MENU = 300, WID_NEW, WID_OPEN, WID_SAVE,
    WID_FACE, WID_SIZE, WID_B, WID_I, WID_U, WID_AL, WID_AC, WID_AR,
    WID_FINDF, WID_FINDGO, WID_REPLF, WID_REPLGO, WID_REPLALL,
    WID_DLG_VOL, WID_DLG_LIST, WID_DLG_NAME, WID_DLG_OK, WID_DLG_CANCEL
};

/* ---- style words ---------------------------------------------------------- */
#define WS_BOLD   1
#define WS_ITAL   2
#define WS_UNDL   4
#define WS_FONTSH 3
#define WS_SIZESH 5
#define WS_ALGNSH 8
#define WS_FONT(w)  (((w) >> WS_FONTSH) & 3)
#define WS_SIZE(w)  (((w) >> WS_SIZESH) & 7)
#define WS_ALIGN(w) (((w) >> WS_ALGNSH) & 3)
#define WS_MKFONT(v)  (((v) & 3) << WS_FONTSH)
#define WS_MKSIZE(v)  (((v) & 7) << WS_SIZESH)
#define WS_MKALIGN(v) (((v) & 3) << WS_ALGNSH)
#define WS_CHARMASK 0xFF                      /* everything but alignment */

static const char *kFaceName[4] = { "Sans", "Mono", "Ubuntu", "Chicago" };
static const int   kFaceSlot[4] = { 1, 2, 3, 0 };
static const char *kSizeName[8] = { "10", "12", "14", "16", "20", "24", "28", "32" };
static const int   kSizePx[8]   = { 10, 12, 14, 16, 20, 24, 28, 32 };
#define DEF_STYLE (WS_MKFONT(0) | WS_MKSIZE(2))        /* Sans 14, left */

/* ---- document ------------------------------------------------------------- */
#define WR_MAX 32768
static char           wr_text[WR_MAX];
static unsigned short wr_style[WR_MAX];
static int  wr_len, wr_caret, wr_sel;         /* sel = anchor (== caret: none) */
static unsigned short wr_cur = DEF_STYLE;     /* style for the next insert */
static int  wr_sticky;                        /* wr_cur was set explicitly */
static int  wr_scroll;                        /* document scroll, px */
static int  wr_modified;
static char wr_fname[28];                     /* current 8.3 name ("" = new) */
static int  wr_fvol;                          /* volume of the current file */

/* clipboard (styled) */
#define CLIP_MAX 8192
static char           clip_t[CLIP_MAX];
static unsigned short clip_s[CLIP_MAX];
static int clip_n;

/* find / replace buffers (unoui edit fields own them) */
static char wr_find[40], wr_repl[40];
static unoui_text wr_find_t, wr_repl_t;

/* ---- metric caches -------------------------------------------------------- */
static short wr_adv[4][8][2][96];             /* [face][size][bold][cp-32] */
static short wr_lh[4][8], wr_bs[4][8];        /* line height / baseline    */

static int adv_of(unsigned short st, int cp)
{
    int f = WS_FONT(st), s = WS_SIZE(st), b = (st & WS_BOLD) ? 1 : 0;
    short *slot;
    if (cp < 32 || cp > 126) cp = ' ';
    slot = &wr_adv[f][s][b][cp - 32];
    if (!*slot) {
        char one[2]; one[0] = (char)cp; one[1] = 0;
        int w = uno_font_text_w_styled(kFaceSlot[f], kSizePx[s],
                                       b ? UNO_FS_BOLD : 0, one);
        *slot = (short)(w > 0 ? w : 1);
    }
    return *slot;
}
static int lh_of(unsigned short st)
{
    int f = WS_FONT(st), s = WS_SIZE(st);
    if (!wr_lh[f][s]) {
        wr_lh[f][s] = (short)(uno_font_height_px(kFaceSlot[f], kSizePx[s]) + 3);
        wr_bs[f][s] = (short)uno_font_baseline_px(kFaceSlot[f], kSizePx[s]);
    }
    return wr_lh[f][s];
}
static int bs_of(unsigned short st)
{ lh_of(st); return wr_bs[WS_FONT(st)][WS_SIZE(st)]; }

static void metrics_reset(void)
{ memset(wr_adv, 0, sizeof wr_adv); memset(wr_lh, 0, sizeof wr_lh); }

/* ---- layout (wrap) -------------------------------------------------------- */
#define WR_MAXL 4096
typedef struct { int s, e, y; short h, base; unsigned char align; } wline;
static wline wr_line[WR_MAXL];
static int wr_nlines, wr_docw = -1, wr_doch, wr_lay_ok;

static int para_start(int i)
{ while (i > 0 && wr_text[i - 1] != '\n') i--; return i; }

static unsigned char align_at(int i)
{
    int ps = para_start(i);
    if (ps < wr_len) return (unsigned char)WS_ALIGN(wr_style[ps]);
    if (wr_len > 0)  return (unsigned char)WS_ALIGN(wr_style[wr_len - 1]);
    return (unsigned char)WS_ALIGN(wr_cur);
}

static void lay_push(int s, int e, int *y, int h, int base, unsigned char al)
{
    wline *L;
    if (wr_nlines >= WR_MAXL) return;
    L = &wr_line[wr_nlines++];
    L->s = s; L->e = e; L->y = *y; L->h = (short)h; L->base = (short)base;
    L->align = al;
    *y += h;
}

static void wr_layout(int docw)
{
    int i = 0, y = 0, pstart = 0;
    if (wr_lay_ok && docw == wr_docw) return;
    wr_docw = docw; wr_nlines = 0;
    while (i <= wr_len) {
        int ls = i, x = 0, lastsp = -1, lastsp_x = 0;
        int h = lh_of(i < wr_len ? wr_style[i] : wr_cur);
        int base = bs_of(i < wr_len ? wr_style[i] : wr_cur);
        /* alignment of the CURRENT paragraph, tracked incrementally. The old
         * align_at(ls) walked backward to the paragraph start for EVERY
         * wrapped line - O(paragraph) per line = O(doc^2) on a document with
         * long paragraphs. That single call was the F11 render spike: 750 ms
         *-3.3 s first-layout stalls on the stress corpus, inside draw. */
        unsigned char al = (pstart < wr_len) ? (unsigned char)WS_ALIGN(wr_style[pstart])
                         : (wr_len > 0)      ? (unsigned char)WS_ALIGN(wr_style[wr_len - 1])
                                             : (unsigned char)WS_ALIGN(wr_cur);
        if (i == wr_len) {                        /* trailing empty line */
            lay_push(ls, ls, &y, h, base, al);
            break;
        }
        while (i < wr_len) {
            int ch = (unsigned char)wr_text[i], cw;
            int chh, chb;
            if (ch == '\n') { i++; pstart = i; break; }   /* paragraph end */
            cw = adv_of(wr_style[i], ch);
            chh = lh_of(wr_style[i]); chb = bs_of(wr_style[i]);
            if (chh > h) h = chh;
            if (chb > base) base = chb;
            if (x + cw > docw && x > 0) {          /* wrap */
                if (lastsp >= ls) { i = lastsp + 1; }   /* break after space */
                break;
            }
            if (ch == ' ') { lastsp = i; lastsp_x = x; }
            x += cw; i++;
        }
        (void)lastsp_x;
        lay_push(ls, i, &y, h, base, al);
        /* a paragraph break both ends this line and (at EOF) yields the final
           empty line via the i<=wr_len loop head */
        if (i == wr_len && wr_len > 0 && wr_text[wr_len - 1] == '\n') {
            int hh = lh_of(wr_cur);
            lay_push(wr_len, wr_len, &y, hh, bs_of(wr_cur), align_at(wr_len));
            break;
        }
        if (i >= wr_len) break;
    }
    if (wr_nlines == 0) lay_push(0, 0, &y, lh_of(wr_cur), bs_of(wr_cur), 0);
    wr_doch = y;
    wr_lay_ok = 1;
}
static void lay_dirty(void) { wr_lay_ok = 0; }

/* line index containing char idx (lines cover [s,e); newline belongs to its
 * line; the caret at wr_len sits on the last line) */
static int line_of(int idx)
{
    int lo = 0, hi = wr_nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (idx < wr_line[mid].s) hi = mid - 1;
        else if (mid + 1 < wr_nlines && idx >= wr_line[mid + 1].s) lo = mid + 1;
        else return mid;
    }
    return lo;
}

/* width of line chars [s..upto) */
static int line_w(int s, int upto)
{
    int x = 0, i;
    for (i = s; i < upto; i++) {
        if (wr_text[i] == '\n') break;
        x += adv_of(wr_style[i], (unsigned char)wr_text[i]);
    }
    return x;
}
static int line_x0(const wline *L, int docw)
{
    int w;
    if (L->align == 0) return 0;
    w = line_w(L->s, L->e);
    if (L->align == 1) return (docw - w) / 2 > 0 ? (docw - w) / 2 : 0;
    return docw - w > 0 ? docw - w : 0;
}

/* ---- editing primitives --------------------------------------------------- */
static void sel_range(int *a, int *b)
{ *a = wr_sel < wr_caret ? wr_sel : wr_caret;
  *b = wr_sel < wr_caret ? wr_caret : wr_sel; }

static void del_range(int a, int b)
{
    if (b <= a) return;
    memmove(wr_text + a, wr_text + b, (size_t)(wr_len - b + 1));
    memmove(wr_style + a, wr_style + b, (size_t)(wr_len - b) * sizeof(short));
    wr_len -= b - a;
    wr_caret = wr_sel = a;
    wr_modified = 1; lay_dirty();
}
static void del_sel(void)
{ int a, b; sel_range(&a, &b); del_range(a, b); }

static void ins_text(const char *s, const unsigned short *st, int n)
{
    int i;
    del_sel();
    if (n > WR_MAX - 1 - wr_len) n = WR_MAX - 1 - wr_len;
    if (n <= 0) return;
    memmove(wr_text + wr_caret + n, wr_text + wr_caret, (size_t)(wr_len - wr_caret + 1));
    memmove(wr_style + wr_caret + n, wr_style + wr_caret, (size_t)(wr_len - wr_caret) * sizeof(short));
    for (i = 0; i < n; i++) {
        wr_text[wr_caret + i] = s[i];
        wr_style[wr_caret + i] = st ? st[i] : wr_cur;
    }
    wr_len += n; wr_caret += n; wr_sel = wr_caret;
    wr_modified = 1; lay_dirty();
}
static void ins_ch(int ch)
{ char c = (char)ch; ins_text(&c, 0, 1); }

/* style at the caret (for toolbar state + sticky inserts) */
static unsigned short style_here(void)
{
    if (wr_sticky) return wr_cur;
    if (wr_caret > 0 && wr_caret <= wr_len) return wr_style[wr_caret - 1];
    if (wr_len > 0) return wr_style[0];
    return wr_cur;
}

/* apply fn over the selection (or set the sticky insert style) */
static void apply_char_style(unsigned short clr, unsigned short set)
{
    int a, b, i;
    sel_range(&a, &b);
    if (b > a) {
        for (i = a; i < b; i++) wr_style[i] = (unsigned short)((wr_style[i] & ~clr) | set);
        wr_modified = 1; lay_dirty();
    }
    wr_cur = (unsigned short)(((b > a ? wr_style[a] : style_here()) & ~clr) | set);
    wr_sticky = 1;
}
static int sel_all_have(unsigned short bit)
{
    int a, b, i;
    sel_range(&a, &b);
    if (b <= a) return (style_here() & bit) != 0;
    for (i = a; i < b; i++) if (!(wr_style[i] & bit)) return 0;
    return 1;
}
static void toggle_bit(unsigned short bit)
{ if (sel_all_have(bit)) apply_char_style(bit, 0); else apply_char_style(0, bit); }

static void apply_align(int al)
{
    int a, b, i;
    sel_range(&a, &b);
    a = para_start(a);
    if (b < a) b = a;
    /* extend b to the end of its paragraph */
    while (b < wr_len && wr_text[b] != '\n') b++;
    for (i = a; i <= b && i < wr_len; i++)
        wr_style[i] = (unsigned short)((wr_style[i] & ~WS_MKALIGN(3)) | WS_MKALIGN(al));
    wr_cur = (unsigned short)((wr_cur & ~WS_MKALIGN(3)) | WS_MKALIGN(al));
    wr_modified = 1; lay_dirty();
}

/* ---- widgets / window ----------------------------------------------------- */
static unoui_window *wr_win;
static unoui_widget *wr_dd_face, *wr_dd_size, *wr_bt_b, *wr_bt_i, *wr_bt_u;
static unoui_widget *wr_bt_al, *wr_bt_ac, *wr_bt_ar;
static int wr_canvas_wi = -1;
static unsigned wr_tick, wr_caret_frame;
static char wr_status[96] = "New document";

static const char *m_file[] = { "New", "Open...", "Save", "Save As..." };
static const char *m_edit[] = { "Cut", "Copy", "Paste", "Select All", "Find Next", "Replace All" };
static const char *m_fmt[]  = { "Bold", "Italic", "Underline", "Align Left", "Align Center", "Align Right" };
static const char *m_help[] = { "About Editor" };
static unoui_menu wr_menus[] = {
    { "File", m_file, 4 }, { "Edit", m_edit, 6 },
    { "Format", m_fmt, 6 }, { "Help", m_help, 1 }
};

/* transient status notice (op feedback), shown ~4 s over the Ln/Col line */
static char wr_notice[64];
static unsigned wr_notice_tick = 0xF0000000u;
static unsigned wr_tick_now(void);
static void notice(const char *s)
{ strncpy(wr_notice, s, 63); wr_notice[63] = 0; wr_notice_tick = wr_tick_now(); }

static void wr_sync_toolbar(void)
{
    unsigned short st = style_here();
    if (wr_dd_face) wr_dd_face->sel = WS_FONT(st);
    if (wr_dd_size) wr_dd_size->sel = WS_SIZE(st);
    if (wr_bt_b) { if (st & WS_BOLD) wr_bt_b->flags |= UI_F_PRESSED; else wr_bt_b->flags &= ~UI_F_PRESSED; }
    if (wr_bt_i) { if (st & WS_ITAL) wr_bt_i->flags |= UI_F_PRESSED; else wr_bt_i->flags &= ~UI_F_PRESSED; }
    if (wr_bt_u) { if (st & WS_UNDL) wr_bt_u->flags |= UI_F_PRESSED; else wr_bt_u->flags &= ~UI_F_PRESSED; }
    { int al = wr_caret <= wr_len ? align_at(wr_caret) : 0;
      if (wr_bt_al) { if (al == 0) wr_bt_al->flags |= UI_F_PRESSED; else wr_bt_al->flags &= ~UI_F_PRESSED; }
      if (wr_bt_ac) { if (al == 1) wr_bt_ac->flags |= UI_F_PRESSED; else wr_bt_ac->flags &= ~UI_F_PRESSED; }
      if (wr_bt_ar) { if (al == 2) wr_bt_ar->flags |= UI_F_PRESSED; else wr_bt_ar->flags &= ~UI_F_PRESSED; }
    }
}

static char *cat(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *cat_int(char *p, int v)
{ char t[12]; int n = 0; if (v < 0) { *p++ = '-'; v = -v; }
  if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
  while (n) *p++ = t[--n]; return p; }

static void wr_update_status(void)
{
    int ln = 0, col = 0, i;
    char *p = wr_status;
    for (i = 0; i < wr_caret && i < wr_len; i++) {
        if (wr_text[i] == '\n') { ln++; col = 0; } else col++;
    }
    p = cat(p, wr_fname[0] ? wr_fname : "(untitled)");
    if (wr_modified) p = cat(p, " *");
    p = cat(p, "   Ln "); p = cat_int(p, ln + 1);
    p = cat(p, ", Col "); p = cat_int(p, col + 1);
    p = cat(p, "   ");    p = cat_int(p, wr_len);
    p = cat(p, " chars");
    if (wr_tick - wr_notice_tick < 240u) {         /* ~4 s at 60 Hz */
        p = cat(p, "   -  "); p = cat(p, wr_notice);
    }
    *p = 0;
}

/* ---- document canvas ------------------------------------------------------ */
#define RULER_H 16
#define SB_W    14
#define MARGIN  10

static void caret_show(void) { wr_caret_frame = wr_tick; }

static void reveal_caret(int view_h)
{
    int li, cy0, cy1;
    if (!wr_lay_ok) return;
    li = line_of(wr_caret);
    cy0 = wr_line[li].y; cy1 = cy0 + wr_line[li].h;
    if (cy0 - wr_scroll < 0) wr_scroll = cy0;
    if (cy1 - wr_scroll > view_h) wr_scroll = cy1 - view_h;
    if (wr_scroll < 0) wr_scroll = 0;
}

static int hit_index(unoui_rect r, int px, int py)
{
    int docw = r.w - 2 * MARGIN - SB_W;
    int y = py - (r.y + RULER_H) + wr_scroll, li, i, x, x0;
    wr_layout(docw);
    if (wr_nlines == 0) return 0;
    for (li = 0; li < wr_nlines - 1; li++)
        if (y < wr_line[li + 1].y) break;
    if (y < 0) li = 0;
    x0 = r.x + MARGIN + line_x0(&wr_line[li], docw);
    x = x0;
    for (i = wr_line[li].s; i < wr_line[li].e; i++) {
        int cw;
        if (wr_text[i] == '\n') break;
        cw = adv_of(wr_style[i], (unsigned char)wr_text[i]);
        if (px < x + cw / 2) return i;
        x += cw;
    }
    /* past the last glyph: caret goes to the line end (before a hard '\n'
       so it stays on this visual line) */
    i = wr_line[li].e;
    if (i > wr_line[li].s && i > 0 && i <= wr_len && wr_text[i - 1] == '\n') i--;
    return i;
}

static void draw_ruler(unoui_rect r, const unoui_theme *t, int docw)
{
    int x, i;
    fb_fill_rect(r.x, r.y, r.w, RULER_H, t->pal.face);
    fb_hline(r.x, r.y + RULER_H - 1, r.w, t->pal.shadow);
    for (i = 0, x = r.x + MARGIN; x < r.x + MARGIN + docw; i++, x += 8) {
        int tall = (i % 4) == 0;
        fb_vline(x, r.y + RULER_H - (tall ? 9 : 5), tall ? 8 : 4, t->pal.text_dim);
    }
}

static void wr_canvas_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = pc64_shell_theme();
    int docw = r.w - 2 * MARGIN - SB_W;
    int fh = fb_text_h(), stat_h = fh + 6;
    int view_h = r.h - RULER_H - stat_h;
    int selA, selB, li;
#ifdef UNO_DEBUG
    /* slow-draw autopsy (F11): when one editor draw blows past 150 ms, log
     * WHERE it went - layout vs glyph loop vs the rest - so a metal PF's
     * "window=Editor" line comes with its own breakdown in the boot log. */
    unsigned long long dbg_t0 = uno_native_rdtsc(), dbg_t1, dbg_t2;
#endif
    (void)w; (void)ctx;
    if (docw < 40) docw = 40;
    wr_layout(docw);
#ifdef UNO_DEBUG
    dbg_t1 = uno_native_rdtsc();
#endif
    reveal_caret(view_h);
    sel_range(&selA, &selB);

    /* paper */
    fb_fill_rect(r.x, r.y + RULER_H, r.w - SB_W, r.h - RULER_H - stat_h, t->pal.field_bg);
    draw_ruler(r, t, docw);

    for (li = 0; li < wr_nlines; li++) {
        const wline *L = &wr_line[li];
        int ly = r.y + RULER_H + L->y - wr_scroll;
        int x, i;
        if (ly + L->h < r.y + RULER_H) continue;
        if (ly > r.y + RULER_H + view_h) break;
        x = r.x + MARGIN + line_x0(L, docw);
        for (i = L->s; i < L->e; i++) {
            unsigned short st;
            int cw, cp = (unsigned char)wr_text[i];
            char one[2];
            if (cp == '\n') break;
            st = wr_style[i];
            cw = adv_of(st, cp);
            if (i >= selA && i < selB)             /* selection backdrop */
                fb_fill_rect(x, ly, cw, L->h, t->pal.accent);
            one[0] = (char)cp; one[1] = 0;
            uno_font_draw_styled(kFaceSlot[WS_FONT(st)], kSizePx[WS_SIZE(st)],
                                 ((st & WS_BOLD) ? UNO_FS_BOLD : 0) |
                                 ((st & WS_ITAL) ? UNO_FS_ITALIC : 0),
                                 x, ly + (L->base - bs_of(st)),
                                 one,
                                 (i >= selA && i < selB) ? t->pal.accent_text
                                                         : t->pal.field_text,
                                 -1);
            if (st & WS_UNDL)
                fb_hline(x, ly + L->base + 2, cw,
                         (i >= selA && i < selB) ? t->pal.accent_text
                                                 : t->pal.field_text);
            x += cw;
        }
        /* newline inside the selection: show a stub so full-line selects read */
        if (selB > selA && L->e > L->s && L->e <= wr_len &&
            L->e - 1 >= selA && L->e - 1 < selB && wr_text[L->e - 1] == '\n')
            fb_fill_rect(x, ly, 4, L->h, t->pal.accent);
    }

#ifdef UNO_DEBUG
    dbg_t2 = uno_native_rdtsc();
#endif
    /* caret */
    if (wr_win && wr_win->active && (((wr_tick - wr_caret_frame) / 30) & 1u) == 0) {
        int cli = line_of(wr_caret);
        const wline *L = &wr_line[cli];
        int cx = r.x + MARGIN + line_x0(L, docw) + line_w(L->s, wr_caret);
        int cy = r.y + RULER_H + L->y - wr_scroll;
        if (cy >= r.y + RULER_H - 2 && cy <= r.y + RULER_H + view_h)
            fb_fill_rect(cx, cy + 1, 2, L->h - 2, t->pal.field_text);
    }

    /* scrollbar (drawn, self-hit-tested) */
    { int sx = r.x + r.w - SB_W, sy = r.y + RULER_H, sh = view_h;
      int th, ty, maxs = wr_doch - view_h;
      fb_fill_rect(sx, sy, SB_W, sh, t->pal.face);
      fb_vline(sx, sy, sh, t->pal.shadow);
      th = maxs > 0 ? sh * view_h / wr_doch : sh;
      if (th < 16) th = 16;
      if (th > sh) th = sh;
      ty = maxs > 0 ? sy + (sh - th) * wr_scroll / maxs : sy;
      fb_fill_rect(sx + 2, ty, SB_W - 4, th, t->pal.dark); }

    /* status strip */
    { int sy = r.y + r.h - stat_h;
      wr_update_status();
      fb_fill_rect(r.x, sy, r.w, stat_h, t->pal.face);
      fb_hline(r.x, sy, r.w, t->pal.shadow);
      fb_text(r.x + 6, sy + 3, wr_status, t->pal.face_text, -1); }
#ifdef UNO_DEBUG
    { unsigned long long tend = uno_native_rdtsc();
      unsigned long ms = uno_dbg_cyc_to_us(tend - dbg_t0) / 1000;
      if (ms > 150)
          uno_dbg_log("editor: SLOW draw %lums (layout %lums, lines %lums, "
                      "rest %lums; len=%d nlines=%d)",
                      ms, uno_dbg_cyc_to_us(dbg_t1 - dbg_t0) / 1000,
                      uno_dbg_cyc_to_us(dbg_t2 - dbg_t1) / 1000,
                      uno_dbg_cyc_to_us(tend - dbg_t2) / 1000,
                      wr_len, wr_nlines); }
#endif
}

/* ---- clipboard / find ----------------------------------------------------- */
static void clip_copy(void)
{
    int a, b;
    sel_range(&a, &b);
    clip_n = b - a;
    if (clip_n > CLIP_MAX) clip_n = CLIP_MAX;
    memcpy(clip_t, wr_text + a, (size_t)clip_n);
    memcpy(clip_s, wr_style + a, (size_t)clip_n * sizeof(short));
}
static void clip_cut(void)   { clip_copy(); del_sel(); }
static void clip_paste(void) { if (clip_n) ins_text(clip_t, clip_s, clip_n); }

static int ci_eq(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}
static int find_from(int start)
{
    int n = (int)strlen(wr_find), i, j;
    if (!n) return -1;
    for (i = start; i + n <= wr_len; i++) {
        for (j = 0; j < n; j++) if (!ci_eq(wr_text[i + j], wr_find[j])) break;
        if (j == n) return i;
    }
    return -1;
}
static void do_find_next(void)
{
    int hit = find_from(wr_caret);
    if (hit < 0) hit = find_from(0);              /* wrap */
    if (hit >= 0) { wr_sel = hit; wr_caret = hit + (int)strlen(wr_find); caret_show(); }
}
static void do_replace_all(void)
{
    int n = (int)strlen(wr_find), hit, from = 0, rl = (int)strlen(wr_repl);
    if (!n) return;
    while ((hit = find_from(from)) >= 0) {
        wr_sel = hit; wr_caret = hit + n;
        ins_text(wr_repl, 0, rl);                 /* replaces the selection */
        from = hit + (rl > 0 ? rl : 0);
    }
    caret_show();
}

/* ---- file I/O (UWD container / plain text) -------------------------------- */
#define WR_IOMAX (8 + WR_MAX + 4 * WR_MAX + 16)
static unsigned char wr_io[WR_IOMAX];
static const char kMagic[8] = "UNOWR1\r\n";

static int name_is_txt(const char *n)
{
    int l = (int)strlen(n);
    return l > 4 && n[l-4] == '.' &&
           ci_eq(n[l-3], 't') && ci_eq(n[l-2], 'x') && ci_eq(n[l-1], 't');
}

static int wr_save_to(int vol, const char *name)
{
    long total;
    if (!uno_fs_writable(vol)) return 0;
    if (name_is_txt(name)) {
        memcpy(wr_io, wr_text, (size_t)wr_len);
        total = wr_len;
    } else {
        unsigned char *p = wr_io;
        int i = 0;
        memcpy(p, kMagic, 8); p += 8;
        *p++ = (unsigned char)(wr_len & 0xFF);
        *p++ = (unsigned char)((wr_len >> 8) & 0xFF);
        *p++ = (unsigned char)((wr_len >> 16) & 0xFF);
        *p++ = 0;
        memcpy(p, wr_text, (size_t)wr_len); p += wr_len;
        while (i < wr_len) {                       /* RLE style runs */
            unsigned short st = wr_style[i];
            int run = 1;
            while (i + run < wr_len && wr_style[i + run] == st && run < 65535) run++;
            *p++ = (unsigned char)(run & 0xFF); *p++ = (unsigned char)(run >> 8);
            *p++ = (unsigned char)(st & 0xFF);  *p++ = (unsigned char)(st >> 8);
            i += run;
        }
        total = (long)(p - wr_io);
    }
    if (!uno_fs_write(vol, name, wr_io, total)) return 0;
    strncpy(wr_fname, name, sizeof wr_fname - 1); wr_fname[sizeof wr_fname - 1] = 0;
    wr_fvol = vol; wr_modified = 0;
    return 1;
}

static int wr_open_from(int vol, const char *name)
{
    long n = uno_fs_read(vol, name, wr_io, WR_IOMAX);
    int i;
    if (n < 0) return 0;
    wr_len = 0; wr_caret = wr_sel = 0; wr_scroll = 0;
    if (n >= 12 && memcmp(wr_io, kMagic, 8) == 0) {
        int tl = wr_io[8] | (wr_io[9] << 8) | (wr_io[10] << 16);
        const unsigned char *p = wr_io + 12 + tl, *end = wr_io + n;
        int at = 0;
        if (tl > WR_MAX - 1) tl = WR_MAX - 1;
        memcpy(wr_text, wr_io + 12, (size_t)tl);
        wr_len = tl;
        for (i = 0; i < wr_len; i++) wr_style[i] = DEF_STYLE;
        while (p + 4 <= end && at < wr_len) {
            int run = p[0] | (p[1] << 8);
            unsigned short st = (unsigned short)(p[2] | (p[3] << 8));
            p += 4;
            for (i = 0; i < run && at < wr_len; i++) wr_style[at++] = st;
        }
    } else {                                       /* plain text (CRLF -> LF) */
        int o = 0;
        for (i = 0; i < n && o < WR_MAX - 1; i++) {
            unsigned char c = wr_io[i];
            if (c == '\r') continue;
            if (c == '\t') c = ' ';
            if (c != '\n' && (c < 32 || c > 126)) continue;
            wr_text[o] = (char)c; wr_style[o] = DEF_STYLE; o++;
        }
        wr_len = o;
    }
    wr_text[wr_len] = 0;
    strncpy(wr_fname, name, sizeof wr_fname - 1); wr_fname[sizeof wr_fname - 1] = 0;
    wr_fvol = vol; wr_modified = 0; wr_sticky = 0;
    lay_dirty(); caret_show();
    return 1;
}

/* ---- Open / Save-As dialog ------------------------------------------------ */
static unoui_window wr_dlg;
static int wr_dlg_open, wr_dlg_save;              /* mode: 0 = open, 1 = save */
static char        wr_dlg_files[40][16];
static const char *wr_dlg_ptr[40];
static int         wr_dlg_n, wr_dlg_vol;
static const char *wr_vols[12];
static int         wr_nvols;
static char        wr_dlg_name[28];
static unoui_text  wr_dlg_name_t;
static unoui_widget *wr_dlg_list_w;

static void dlg_fill_files(void)
{
    int n = uno_fs_list_begin(wr_dlg_vol), i, k = 0;
    for (i = 0; i < n && k < 40; i++) {
        char nm[16];
        if (!uno_fs_list_get(wr_dlg_vol, i, nm, sizeof nm)) continue;
        strncpy(wr_dlg_files[k], nm, 15); wr_dlg_files[k][15] = 0;
        wr_dlg_ptr[k] = wr_dlg_files[k]; k++;
    }
    wr_dlg_n = k;
    if (wr_dlg_list_w) { wr_dlg_list_w->nitems = k; wr_dlg_list_w->sel = k ? 0 : -1; }
}

static void dlg_close(void)
{
    if (!wr_dlg_open) return;
    pc64_shell_remove_window(&wr_dlg);
    wr_dlg_open = 0;
}

static void dlg_show(int save)
{
    unoui_widget *x;
    int fh = fb_text_h(), ch = ui_field_h(), bh = ui_ctl_h();
    int y = 4, cw = 300, i;
    const unoui_theme *t = pc64_shell_theme();
    dlg_close();
    wr_dlg_save = save;
    wr_nvols = uno_fs_volumes(); if (wr_nvols > 12) wr_nvols = 12;
    for (i = 0; i < wr_nvols; i++) wr_vols[i] = uno_fs_volume_name(i);
    if (wr_dlg_vol >= wr_nvols) wr_dlg_vol = 0;
    if (save && !uno_fs_writable(wr_dlg_vol))     /* prefer a writable volume */
        for (i = 0; i < wr_nvols; i++) if (uno_fs_writable(i)) { wr_dlg_vol = i; break; }
    unoui_window_init(&wr_dlg, save ? "Save As" : "Open", 200, 90, 1, 1);
    unoui_add_label(&wr_dlg, 8, y + (ch - fh) / 2, "Volume:");
    x = unoui_add_dropdown(&wr_dlg, 80, y, cw - 88, wr_vols, wr_nvols, wr_dlg_vol);
    x->id = WID_DLG_VOL;
    y += ch + 6;
    { int listh = 9 * ui_row_h() + 6;
      wr_dlg_list_w = unoui_add_list(&wr_dlg, 8, y, cw - 16, listh, wr_dlg_ptr, 0, -1);
      wr_dlg_list_w->id = WID_DLG_LIST;
      y += listh + 6; }
    unoui_add_label(&wr_dlg, 8, y + (ch - fh) / 2, "Name:");
    unoui_text_init(&wr_dlg_name_t, wr_dlg_name, sizeof wr_dlg_name, 0);
    x = unoui_add_edit(&wr_dlg, 80, y, cw - 88, &wr_dlg_name_t); x->id = WID_DLG_NAME;
    y += ch + 10;
    x = unoui_add_button(&wr_dlg, 8, y, 110, save ? "Save" : "Open", UI_F_DEFAULT);
    x->id = WID_DLG_OK;
    x = unoui_add_button(&wr_dlg, cw - 8 - 110, y, 110, "Cancel", 0);
    x->id = WID_DLG_CANCEL;
    y += bh + 8;
    wr_dlg.r.w = cw + 2 * t->m.frame_w + 2 * t->m.pad;
    wr_dlg.r.h = y + t->m.title_h + 2 * t->m.pad + t->m.frame_w;
    dlg_fill_files();
    if (save && wr_fname[0]) { strcpy(wr_dlg_name, wr_fname);
        wr_dlg_name_t.len = (int)strlen(wr_dlg_name);
        wr_dlg_name_t.caret = wr_dlg_name_t.sel = wr_dlg_name_t.len; }
    pc64_shell_add_window(&wr_dlg);
    pc64_shell_focus_window(&wr_dlg);
    wr_dlg_open = 1;
}

static void dlg_commit(void)
{
    if (wr_dlg_save) {
        if (!wr_dlg_name[0]) { notice("Type a file name."); return; }
        if (wr_save_to(wr_dlg_vol, wr_dlg_name)) { notice("Saved."); dlg_close(); }
        else notice("Save FAILED (read-only volume?).");
    } else {
        const char *nm = wr_dlg_name[0] ? wr_dlg_name
                       : (wr_dlg_list_w && wr_dlg_list_w->sel >= 0 &&
                          wr_dlg_list_w->sel < wr_dlg_n)
                         ? wr_dlg_files[wr_dlg_list_w->sel] : 0;
        if (nm && wr_open_from(wr_dlg_vol, nm)) { notice("Opened."); dlg_close(); }
        else notice("Could not open that file.");
    }
    wr_sync_toolbar();
}

/* ---- document canvas events ----------------------------------------------- */
static int wr_canvas_event(struct unoui_widget *w, const void *evp, void *ctx)
{
    const unoui_event *e = (const unoui_event *)evp;
    unoui_rect r;
    const unoui_theme *t = pc64_shell_theme();
    int fh = fb_text_h(), stat_h = fh + 6, view_h;
    (void)ctx;
    if (!wr_win) return 0;
    r = unoui_widget_rect(t, wr_win, w);
    view_h = r.h - RULER_H - stat_h;

    switch (e->kind) {
    case UI_EV_MOUSE_DOWN:
        if (e->x >= r.x + r.w - SB_W) {           /* scrollbar jump */
            int maxs = wr_doch - view_h;
            if (maxs > 0) {
                wr_scroll = (e->y - (r.y + RULER_H)) * maxs / (view_h > 1 ? view_h : 1);
                if (wr_scroll < 0) wr_scroll = 0;
                if (wr_scroll > maxs) wr_scroll = maxs;
            }
            pc64_shell_dirty(); return 1;
        }
        wr_caret = wr_sel = hit_index(r, e->x, e->y);
        wr_sticky = 0; caret_show(); wr_sync_toolbar();
        pc64_shell_dirty(); return 1;
    case UI_EV_MOUSE_MOVE:                        /* drag-select (button held) */
        wr_caret = hit_index(r, e->x, e->y);
        caret_show(); pc64_shell_dirty(); return 1;
    case UI_EV_WHEEL: {
        int maxs = wr_doch - view_h;
        wr_scroll += e->wheel * 3 * lh_of(wr_cur);
        if (wr_scroll > maxs) wr_scroll = maxs;
        if (wr_scroll < 0) wr_scroll = 0;
        pc64_shell_dirty(); return 1;
    }
    case UI_EV_CHAR:
        if (e->ch >= 32 && e->ch < 127) {
            if (!wr_sticky) wr_cur = (unsigned short)((style_here() & WS_CHARMASK) |
                                                      WS_MKALIGN(align_at(wr_caret)));
            ins_ch(e->ch); caret_show(); pc64_shell_dirty();
        }
        return 1;
    case UI_EV_KEY: {
        int ext = (e->mods & UI_MOD_SHIFT) != 0;
        int old = wr_caret;
        switch (e->key) {
        case UI_KEY_LEFT:  if (wr_caret > 0) wr_caret--; break;
        case UI_KEY_RIGHT: if (wr_caret < wr_len) wr_caret++; break;
        case UI_KEY_HOME:  { int li = line_of(wr_caret); wr_caret = wr_line[li].s; break; }
        case UI_KEY_END:   { int li = line_of(wr_caret); wr_caret = wr_line[li].e;
                             if (wr_caret > wr_line[li].s && wr_caret <= wr_len &&
                                 wr_caret > 0 && wr_text[wr_caret - 1] == '\n') wr_caret--;
                             break; }
        case UI_KEY_UP: case UI_KEY_DOWN: {
            int li = line_of(wr_caret);
            int nx = e->key == UI_KEY_UP ? li - 1 : li + 1;
            if (nx >= 0 && nx < wr_nlines) {
                int want = line_w(wr_line[li].s, wr_caret), x = 0, i;
                for (i = wr_line[nx].s; i < wr_line[nx].e; i++) {
                    int cw;
                    if (wr_text[i] == '\n') break;
                    cw = adv_of(wr_style[i], (unsigned char)wr_text[i]);
                    if (x + cw / 2 > want) break;
                    x += cw;
                }
                wr_caret = i;
            }
            break;
        }
        case UI_KEY_PGUP:  wr_scroll -= view_h; if (wr_scroll < 0) wr_scroll = 0;
                           pc64_shell_dirty(); return 1;
        case UI_KEY_PGDN:  { int maxs = wr_doch - view_h;
                             wr_scroll += view_h;
                             if (wr_scroll > maxs) wr_scroll = maxs;
                             if (wr_scroll < 0) wr_scroll = 0;
                             pc64_shell_dirty(); return 1; }
        case UI_KEY_ENTER: ins_ch('\n'); caret_show(); pc64_shell_dirty(); return 1;
        case UI_KEY_BACKSPACE:
            if (wr_sel != wr_caret) del_sel();
            else if (wr_caret > 0) del_range(wr_caret - 1, wr_caret);
            caret_show(); wr_sync_toolbar(); pc64_shell_dirty(); return 1;
        case UI_KEY_DELETE:
            if (wr_sel != wr_caret) del_sel();
            else if (wr_caret < wr_len) del_range(wr_caret, wr_caret + 1);
            caret_show(); wr_sync_toolbar(); pc64_shell_dirty(); return 1;
        default: return 0;
        }
        if (!ext) wr_sel = wr_caret;
        if (wr_caret != old) { wr_sticky = 0; wr_sync_toolbar(); }
        caret_show(); pc64_shell_dirty();
        return 1;
    }
    default: return 0;
    }
}
static unoui_canvas wr_canvas = { wr_canvas_draw, wr_canvas_event, 0 };

/* ---- menu / toolbar / accelerator dispatch -------------------------------- */
static void wr_new_doc(void)
{
    wr_len = 0; wr_caret = wr_sel = 0; wr_scroll = 0;
    wr_text[0] = 0; wr_fname[0] = 0; wr_modified = 0;
    wr_cur = DEF_STYLE; wr_sticky = 0;
    lay_dirty(); caret_show(); wr_sync_toolbar();
}

static void wr_save(void)
{
    if (wr_fname[0])
        notice(wr_save_to(wr_fvol, wr_fname) ? "Saved." : "Save FAILED.");
    else dlg_show(1);
}

static void wr_menu(int mi, int item)
{
    switch (mi) {
    case 0:                                        /* File */
        if      (item == 0) wr_new_doc();
        else if (item == 1) dlg_show(0);
        else if (item == 2) wr_save();
        else if (item == 3) dlg_show(1);
        break;
    case 1:                                        /* Edit */
        if      (item == 0) clip_cut();
        else if (item == 1) clip_copy();
        else if (item == 2) clip_paste();
        else if (item == 3) { wr_sel = 0; wr_caret = wr_len; }
        else if (item == 4) do_find_next();
        else if (item == 5) do_replace_all();
        break;
    case 2:                                        /* Format */
        if      (item == 0) toggle_bit(WS_BOLD);
        else if (item == 1) toggle_bit(WS_ITAL);
        else if (item == 2) toggle_bit(WS_UNDL);
        else if (item == 3) apply_align(0);
        else if (item == 4) apply_align(1);
        else if (item == 5) apply_align(2);
        break;
    case 3:                                        /* Help */
        notice("UnoDOS Editor - rich text, 4 faces, 8 sizes, UWD/.TXT");
        break;
    }
    caret_show(); wr_sync_toolbar();
}

int pc64_write_action(const unoui_action *a)
{
    if (a->id < 300 || a->id >= 350) return 0;
    switch (a->id) {
    case WID_MENU:  wr_menu(a->value >> 8, a->value & 255); break;
    case WID_NEW:   wr_new_doc(); break;
    case WID_OPEN:  dlg_show(0); break;
    case WID_SAVE:  wr_save(); break;
    case WID_FACE:  apply_char_style(WS_MKFONT(3), WS_MKFONT(a->value)); break;
    case WID_SIZE:  apply_char_style(WS_MKSIZE(7), WS_MKSIZE(a->value)); break;
    case WID_B:     toggle_bit(WS_BOLD); break;
    case WID_I:     toggle_bit(WS_ITAL); break;
    case WID_U:     toggle_bit(WS_UNDL); break;
    case WID_AL:    apply_align(0); break;
    case WID_AC:    apply_align(1); break;
    case WID_AR:    apply_align(2); break;
    case WID_FINDF: case WID_FINDGO: do_find_next(); break;
    case WID_REPLF: case WID_REPLGO: {
        /* replace the current match (if the selection is one), then find next */
        int a2, b2, n = (int)strlen(wr_find);
        sel_range(&a2, &b2);
        if (n && b2 - a2 == n) {
            int j; for (j = 0; j < n; j++) if (!ci_eq(wr_text[a2 + j], wr_find[j])) break;
            if (j == n) ins_text(wr_repl, 0, (int)strlen(wr_repl));
        }
        do_find_next();
        break;
    }
    case WID_REPLALL:    do_replace_all(); break;
    case WID_DLG_VOL:    wr_dlg_vol = a->value; dlg_fill_files(); break;
    case WID_DLG_LIST:
        if (a->value >= 0 && a->value < wr_dlg_n) {
            strcpy(wr_dlg_name, wr_dlg_files[a->value]);
            wr_dlg_name_t.len = (int)strlen(wr_dlg_name);
            wr_dlg_name_t.caret = wr_dlg_name_t.sel = wr_dlg_name_t.len;
        }
        break;
    case WID_DLG_NAME:   dlg_commit(); break;      /* Enter in the name field */
    case WID_DLG_OK:     dlg_commit(); break;
    case WID_DLG_CANCEL: dlg_close(); break;
    default: return 0;
    }
    caret_show(); wr_sync_toolbar();
    pc64_shell_dirty();
    return 1;
}

/* Ctrl-key accelerators from the shell (uni may be a control code) */
int pc64_write_key(int uni, int ctrl)
{
    if (!ctrl) return 0;
    if (uni >= 'A' && uni <= 'Z') uni += 32;
    switch (uni) {
    case 's': case 0x13: wr_save(); break;
    case 'o': case 0x0F: dlg_show(0); break;
    case 'n': case 0x0E: wr_new_doc(); break;
    case 'a': case 0x01: wr_sel = 0; wr_caret = wr_len; break;
    case 'x': case 0x18: clip_cut(); break;
    case 'c': case 0x03: clip_copy(); break;
    case 'v': case 0x16: clip_paste(); break;
    case 'b': case 0x02: toggle_bit(WS_BOLD); break;
    case 'i':            toggle_bit(WS_ITAL); break;
    case 'u': case 0x15: toggle_bit(WS_UNDL); break;
    case 'f': case 0x06: do_find_next(); break;
    default: return 0;
    }
    caret_show(); wr_sync_toolbar();
    return 1;
}

/* per-frame: caret blink timebase (repaint rides the shell's idle tick) */
void pc64_write_frame(void) { wr_tick++; }
static unsigned wr_tick_now(void) { return wr_tick; }

int pc64_write_canvas_index(void) { return wr_canvas_wi; }

/* ---- window build --------------------------------------------------------- */
void pc64_write_build(unoui_window *w)
{
    unoui_widget *x;
    int fh = fb_text_h(), ch = ui_field_h(), bh = ui_ctl_h();
    int waw = pc64_shell_workarea_w(), wah = pc64_shell_workarea_h();
    int ww = waw * 3 / 5, wh = wah * 3 / 4, cw, y;
    const unoui_theme *t = pc64_shell_theme();
    if (ww < 480) ww = 480; if (ww > 980) ww = 980;
    if (wh < 360) wh = 360; if (wh > wah - 24) wh = wah - 24;
    metrics_reset(); lay_dirty();
    int bx, need1, need2;
    /* measured flow layout so the toolbar fits under any font/scale and never
     * overflows the window (the window min width follows the toolbar) */
#define BTN(label, wid, keep) do { \
        int bw_ = fb_text_w(label) + 18; \
        x = unoui_add_button(w, bx, y, bw_, label, 0); x->id = (wid); \
        keep; bx += bw_ + 4; } while (0)
    unoui_window_init(w, "Editor", 90, 36, ww, wh);
    wr_win = w;
    x = unoui_add_menubar(w, wr_menus, 4); x->id = WID_MENU;
    /* the menubar spans the content's top edge OUTSIDE the padded origin
       (rect = title_h..title_h+MENUBAR_H); start the toolbar below it */
    y = ui_menubar_h() - t->m.pad + 3; if (y < 0) y = 0;
    bx = 0;
    /* toolbar row 1: file ops | face + size | B I U | L C R */
    BTN("New", WID_NEW, (void)0);
    BTN("Open", WID_OPEN, (void)0);
    BTN("Save", WID_SAVE, (void)0);
    bx += 6;
    { int fw = fb_text_w("Chicago") + 28;
      wr_dd_face = unoui_add_dropdown(w, bx, y, fw, kFaceName, 4, 0);
      wr_dd_face->id = WID_FACE; bx += fw + 4; }
    { int sw = fb_text_w("32") + 28;
      wr_dd_size = unoui_add_dropdown(w, bx, y, sw, kSizeName, 8, 2);
      wr_dd_size->id = WID_SIZE; bx += sw + 8; }
    BTN("B", WID_B, wr_bt_b = x);
    BTN("I", WID_I, wr_bt_i = x);
    BTN("U", WID_U, wr_bt_u = x);
    bx += 4;
    BTN("L", WID_AL, wr_bt_al = x);
    BTN("C", WID_AC, wr_bt_ac = x);
    BTN("R", WID_AR, wr_bt_ar = x);
    need1 = bx;
    y += bh + 4; bx = 0;
    /* toolbar row 2: find / replace */
    unoui_add_label(w, bx, y + (ch - fh) / 2, "Find:");
    bx += fb_text_w("Find:") + 6;
    unoui_text_init(&wr_find_t, wr_find, sizeof wr_find, 0);
    x = unoui_add_edit(w, bx, y, 110, &wr_find_t); x->id = WID_FINDF; bx += 114;
    BTN("Next", WID_FINDGO, (void)0);
    bx += 4;
    unoui_add_label(w, bx, y + (ch - fh) / 2, "Repl:");
    bx += fb_text_w("Repl:") + 6;
    unoui_text_init(&wr_repl_t, wr_repl, sizeof wr_repl, 0);
    x = unoui_add_edit(w, bx, y, 110, &wr_repl_t); x->id = WID_REPLF; bx += 114;
    BTN("Replace", WID_REPLGO, (void)0);
    BTN("All", WID_REPLALL, (void)0);
    need2 = bx;
    y += ch + 6;
#undef BTN
    /* the window must at least fit the wider toolbar row */
    { int need = (need1 > need2 ? need1 : need2)
                 + 2 * t->m.frame_w + 2 * t->m.pad;
      if (ww < need) ww = need;
      if (ww > waw - 8) ww = waw - 8;
      w->r.w = ww; }
    cw = ww - 2 * t->m.frame_w - 2 * t->m.pad;
    /* the document canvas fills the rest of the window */
    x = unoui_add_canvas(w, 0, y, cw, 100, &wr_canvas);
    unoui_widget_fill(x);
    wr_canvas_wi = w->nw - 1;
    unoui_reflow_window(t, w);
    w->min_w = (need1 > need2 ? need1 : need2) + 2 * t->m.frame_w + 2 * t->m.pad;
    w->min_h = 260;
    w->flags |= UI_WIN_RESIZE;
    if (wr_len == 0 && !wr_fname[0] && !wr_modified) {
        static const char hello[] =
            "Welcome to the UnoDOS Editor.\n\n"
            "This is a WordPad-class word processor: select text and use the "
            "toolbar or Format menu for bold, italic and underline, pick one "
            "of four faces and eight sizes, and set paragraph alignment. "
            "Documents save as styled UWD files, or name a file .TXT for "
            "plain text.\n";
        ins_text(hello, 0, (int)sizeof hello - 1);
        wr_caret = wr_sel = 0; wr_modified = 0;
    }
    wr_sync_toolbar();
}
