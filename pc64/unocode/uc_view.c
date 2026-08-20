/* ===========================================================================
 * uc_view.c - the workbench chrome: the activity bar, the side bar and its
 * five views, the editor tab strip, the breadcrumbs, the panel and the status
 * bar, plus the diagnostics and output models they display.
 *
 * These are drawn, not built out of unoui widgets, for the reason given at the
 * top of unocode.h: VS Code's chrome is not a stack of dialog controls, and
 * expressing it as one would have meant either a widget per row of every tree
 * or a set of theme painters no other app would ever use.  What IS shared with
 * the toolkit is the window, the focus model and the event stream.
 *
 * THE EXPLORER IS A FLATTENED TREE.  Expansion state is a list of open
 * directory paths; a refresh walks the workspace and emits one row per visible
 * entry with its depth.  Rebuilding the whole visible list on a change is far
 * cheaper than a node graph with parent pointers to keep consistent, and a FAT
 * volume with 8.3 names cannot hold enough entries for the difference to show.
 * ======================================================================== */
#include "unocode.h"

/* ---- problems --------------------------------------------------------------- */
#define PROB_MAX 128
static UcProblem g_prob[PROB_MAX];
static int       g_nprob;

void uc_problems_clear(const char *source)
{
    int i = 0;
    while (i < g_nprob) {
        if (!source || !strcmp(g_prob[i].source, source)) {
            int k;
            for (k = i; k < g_nprob - 1; k++) g_prob[k] = g_prob[k + 1];
            g_nprob--;
        } else i++;
    }
}

int uc_problems_add(const UcProblem *p)
{
    if (g_nprob >= PROB_MAX || !p) return 0;
    g_prob[g_nprob++] = *p;
    return 1;
}

int uc_problems_count(int sev)
{
    int i, n = 0;
    for (i = 0; i < g_nprob; i++) if (sev < 0 || g_prob[i].sev == sev) n++;
    return n;
}

int uc_problems_total(void) { return g_nprob; }
UcProblem *uc_problem_at(int i) { return (i >= 0 && i < g_nprob) ? &g_prob[i] : 0; }

/* ---- output channels --------------------------------------------------------- */
#define OUT_CHANNELS 6
#define OUT_LINES    160
#define OUT_LINEW    100
typedef struct {
    char name[24];
    char line[OUT_LINES][OUT_LINEW];
    int  n, head;
} UcOutChannel;
static UcOutChannel g_out[OUT_CHANNELS];
static int g_nout, g_outsel;

int uc_output_channel(const char *name)
{
    int i;
    for (i = 0; i < g_nout; i++) if (!strcmp(g_out[i].name, name)) return i;
    if (g_nout >= OUT_CHANNELS) return 0;
    i = g_nout++;
    memset(&g_out[i], 0, sizeof g_out[i]);
    uc_scpy(g_out[i].name, name, sizeof g_out[i].name);
    return i;
}

void uc_output_write(int ch, const char *s)
{
    UcOutChannel *c;
    const char *p = s;
    if (ch < 0 || ch >= g_nout || !s) return;
    c = &g_out[ch];
    while (*p) {
        int n = 0;
        char *dst = c->line[(c->head + c->n) % OUT_LINES];
        while (*p && *p != '\n' && n < OUT_LINEW - 1) dst[n++] = *p++;
        dst[n] = 0;
        if (*p == '\n') p++;
        if (c->n < OUT_LINES) c->n++;
        else c->head = (c->head + 1) % OUT_LINES;
    }
}

void uc_output_show(int ch)
{
    if (ch >= 0 && ch < g_nout) g_outsel = ch;
    uc_toggle_panel(UC_PANEL_OUTPUT);
}

/* ---- notifications ----------------------------------------------------------- */
#define NOTIF_MAX 4
static struct { char msg[110]; int sev; unsigned long until; } g_notif[NOTIF_MAX];
static int g_nnotif;

void uc_notify(const char *msg, int sev)
{
    int i;
    if (g_nnotif >= NOTIF_MAX) {
        for (i = 0; i < g_nnotif - 1; i++) g_notif[i] = g_notif[i + 1];
        g_nnotif--;
    }
    uc_scpy(g_notif[g_nnotif].msg, msg, sizeof g_notif[0].msg);
    g_notif[g_nnotif].sev = sev;
    g_notif[g_nnotif].until = uno_dbg_uptime_ms() + 5000;
    g_nnotif++;
    uc_repaint();
}

void uc_notif_tick(void)
{
    unsigned long now = uno_dbg_uptime_ms();
    int i = 0;
    while (i < g_nnotif) {
        if (now > g_notif[i].until) {
            int k;
            for (k = i; k < g_nnotif - 1; k++) g_notif[k] = g_notif[k + 1];
            g_nnotif--;
            uc_repaint();
        } else i++;
    }
}

void uc_notif_draw(UcRect wb)
{
    int i, h = uc_ui_h() + 14, w = 300;
    for (i = 0; i < g_nnotif; i++) {
        int y = wb.y + wb.h - (i + 1) * (h + 6) - 30;
        int x = wb.x + wb.w - w - 16;
        fb_blend_rect(x + 3, y + 3, w, h, uc_col(UC_C_WIDGET_SHADOW), 70);
        fb_fill_rect(x, y, w, h, uc_col(UC_C_NOTIF_BG));
        fb_frame_rect(x, y, w, h, uc_col(UC_C_NOTIF_BORDER));
        fb_fill_rect(x, y, 3, h,
                     g_notif[i].sev == UC_SEV_ERROR ? uc_col(UC_C_ERROR_FG) :
                     g_notif[i].sev == UC_SEV_WARN  ? uc_col(UC_C_WARN_FG)
                                                    : uc_col(UC_C_INFO_FG));
        fb_set_clip(x + 8, y, w - 12, h);
        uc_ui_text(x + 10, y + 7, g_notif[i].msg, uc_col(UC_C_NOTIF_FG));
        fb_reset_clip();
    }
}

/* ---- small drawing helpers ---------------------------------------------------- */
static int row_h(void) { return uc_ui_h() + 7; }

/* A tiny procedural glyph set for the activity bar.  Icon fonts are not
 * available here and a bitmap sheet would be one more asset to ship and keep
 * in step with the themes; these are five shapes drawn from rectangles, and
 * they recolour with the palette for free. */
static void icon_files(int x, int y, int s, fb_px c)
{
    fb_frame_rect(x + s/6, y + s/8, s*2/3, s*3/4, c);
    fb_hline(x + s/4, y + s/3, s/3, c);
    fb_hline(x + s/4, y + s/2, s/2 - 2, c);
    fb_hline(x + s/4, y + s*2/3, s/3, c);
}
static void icon_search(int x, int y, int s, fb_px c)
{
    int r = s / 3;
    fb_frame_rect(x + s/6, y + s/6, r * 2, r * 2, c);
    fb_hline(x + s/6 + r*2 - 2, y + s/6 + r*2 - 2, s/3, c);
    fb_vline(x + s/6 + r*2 + s/3 - 3, y + s/6 + r*2 - 2, s/4, c);
}
static void icon_scm(int x, int y, int s, fb_px c)
{
    fb_frame_rect(x + s/6, y + s/8, 5, 5, c);
    fb_frame_rect(x + s/6, y + s*3/4 - 2, 5, 5, c);
    fb_frame_rect(x + s*2/3, y + s/2 - 2, 5, 5, c);
    fb_vline(x + s/6 + 2, y + s/8 + 5, s/2, c);
    fb_hline(x + s/6 + 2, y + s/2, s/2, c);
}
static void icon_run(int x, int y, int s, fb_px c)
{
    int i;
    for (i = 0; i < s/2; i++)
        fb_vline(x + s/4 + i, y + s/4 + i/2, s/2 - i, c);
    fb_frame_rect(x + s/6, y + s/6, s*2/3, s*2/3, c);
}
static void icon_ext(int x, int y, int s, fb_px c)
{
    int q = s / 3;
    fb_frame_rect(x + s/8, y + s/8, q, q, c);
    fb_frame_rect(x + s/8 + q + 2, y + s/8, q, q, c);
    fb_frame_rect(x + s/8, y + s/8 + q + 2, q, q, c);
    fb_fill_rect(x + s/8 + q + 2, y + s/8 + q + 2, q, q, c);
}
static void icon_gear(int x, int y, int s, fb_px c)
{
    fb_frame_rect(x + s/4, y + s/4, s/2, s/2, c);
    fb_hline(x + s/8, y + s/2, s*3/4, c);
    fb_vline(x + s/2, y + s/8, s*3/4, c);
}

/* ---- activity bar --------------------------------------------------------------- */
#define ACT_ICON 34

void uc_activity_draw(UcRect r)
{
    int i, y = r.y + 4;
    fb_fill_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_ACTIVITY_BG));
    fb_vline(r.x + r.w - 1, r.y, r.h, uc_col(UC_C_ACTIVITY_BORDER));
    for (i = 0; i < UC_VIEW_N; i++) {
        int active = UC.sidebar_visible && UC.view == i;
        fb_px c = active ? uc_col(UC_C_ACTIVITY_FG) : uc_col(UC_C_ACTIVITY_DIM);
        int ix = r.x + (r.w - 22) / 2, iy = y + 6;
        if (active) fb_fill_rect(r.x, y, 2, ACT_ICON, uc_col(UC_C_ACTIVITY_FG));
        switch (i) {
        case UC_VIEW_EXPLORER:   icon_files(ix, iy, 22, c); break;
        case UC_VIEW_SEARCH:     icon_search(ix, iy, 22, c); break;
        case UC_VIEW_SCM:        icon_scm(ix, iy, 22, c); break;
        case UC_VIEW_RUN:        icon_run(ix, iy, 22, c); break;
        case UC_VIEW_EXTENSIONS: icon_ext(ix, iy, 22, c); break;
        default: break;
        }
        /* the Explorer badge counts unsaved editors, the way VS Code's does */
        if (i == UC_VIEW_EXPLORER) {
            int k, dirty = 0;
            for (k = 0; k < uc_doc_count(); k++) if (uc_doc_at(k)->dirty) dirty++;
            if (dirty) {
                char num[8];
                uc_itoa(num, dirty);
                fb_round_rect(r.x + r.w - 20, y + 18, 15, 14, 7, uc_col(UC_C_BADGE_BG));
                uc_ui_text(r.x + r.w - 16, y + 20, num, uc_col(UC_C_BADGE_FG));
            }
        }
        if (i == UC_VIEW_SCM) {
            int k, ch = 0;
            for (k = 0; k < uc_doc_count(); k++) if (uc_doc_at(k)->dirty) ch++;
            (void)ch;
        }
        y += ACT_ICON + 6;
    }
    /* the settings gear pinned to the bottom */
    icon_gear(r.x + (r.w - 22) / 2, r.y + r.h - 32, 22, uc_col(UC_C_ACTIVITY_DIM));
}

int uc_activity_hit(UcRect r, int x, int y)
{
    int i, ty = r.y + 4;
    if (x < r.x || x >= r.x + r.w) return -1;
    if (y >= r.y + r.h - 36) return -2;                 /* the gear */
    for (i = 0; i < UC_VIEW_N; i++) {
        if (y >= ty && y < ty + ACT_ICON) return i;
        ty += ACT_ICON + 6;
    }
    return -1;
}

/* ---- explorer ------------------------------------------------------------------ */
#define EXP_MAX 220
typedef struct {
    char name[16];
    char dir[UC_PATH_MAX];
    unsigned char isdir, depth, open;
} ExpRow;
static ExpRow g_exp[EXP_MAX];
static int    g_nexp, g_expsel, g_expscroll;
static char   g_open_dir[12][UC_PATH_MAX];
static int    g_nopen;

static int dir_is_open(const char *path)
{
    int i;
    for (i = 0; i < g_nopen; i++) if (!strcmp(g_open_dir[i], path)) return 1;
    return 0;
}

static void dir_toggle(const char *path)
{
    int i;
    for (i = 0; i < g_nopen; i++)
        if (!strcmp(g_open_dir[i], path)) {
            int k;
            for (k = i; k < g_nopen - 1; k++) uc_scpy(g_open_dir[k], g_open_dir[k+1], UC_PATH_MAX);
            g_nopen--;
            return;
        }
    if (g_nopen < 12) uc_scpy(g_open_dir[g_nopen++], path, UC_PATH_MAX);
}

/* exp_walk RECURSES into open folders, so its listing buffer is heap, not
 * stack: five nested levels of a 3.5 KB local would be 17 KB of kernel stack
 * for a file tree. */
static void exp_walk(const char *dir, int depth)
{
    char (*names)[16];
    unsigned char *isdir;
    int n, i;
    if (depth > 5 || g_nexp >= EXP_MAX) return;
    names = (char (*)[16])malloc((unsigned long)EXP_MAX * 16);
    if (!names) return;
    isdir = (unsigned char *)malloc(EXP_MAX);
    if (!isdir) { free(names); return; }
    n = uc_list_dir(UC.ws_vol, dir, names, isdir, EXP_MAX);
    if (n > EXP_MAX) n = EXP_MAX;
    /* directories first, then files, each alphabetical - the order every file
     * tree uses, and the one that makes a long listing scannable */
    {
        int pass;
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < n && g_nexp < EXP_MAX; i++) {
                char full[UC_PATH_MAX];
                int isd = isdir[i];
                if (!names[i][0] || names[i][0] == '.') continue;
                uc_path_join(full, sizeof full, dir, names[i]);
                if ((pass == 0) != (isd != 0)) continue;
                uc_scpy(g_exp[g_nexp].name, names[i], sizeof g_exp[0].name);
                uc_scpy(g_exp[g_nexp].dir, dir, sizeof g_exp[0].dir);
                g_exp[g_nexp].isdir = (unsigned char)isd;
                g_exp[g_nexp].depth = (unsigned char)depth;
                g_exp[g_nexp].open = (unsigned char)(isd && dir_is_open(full));
                g_nexp++;
                if (isd && dir_is_open(full)) exp_walk(full, depth + 1);
            }
        }
    }
    free(names);
    free(isdir);
}

void uc_explorer_refresh(void)
{
    g_nexp = 0;
    exp_walk(UC.ws_dir, 0);
    if (g_expsel >= g_nexp) g_expsel = g_nexp ? g_nexp - 1 : 0;
}

void uc_explorer_reveal(UcDoc *d)
{
    int i;
    if (!d) return;
    for (i = 0; i < g_nexp; i++)
        if (!strcmp(g_exp[i].name, d->name) && !strcmp(g_exp[i].dir, d->dir)) {
            g_expsel = i;
            return;
        }
}

/* ---- search view ---------------------------------------------------------------- */
#define SR_MAX 200
typedef struct { char file[16]; char text[80]; int line; } SearchHit;
static SearchHit g_hit[SR_MAX];
static int  g_nhit, g_hitsel, g_hitscroll, g_hitfiles;
static char g_query[80];
static int  g_qlen, g_searching;

void uc_search_run(const char *needle)
{
    static char names[EXP_MAX][16];
    static unsigned char isdir[EXP_MAX];
    int n, i, max = uc_cfg_int("search.maxResults");
    g_nhit = 0;
    g_hitfiles = 0;
    if (!needle || !needle[0]) return;
    n = uc_list_dir(UC.ws_vol, UC.ws_dir, names, isdir, EXP_MAX);
    if (n > EXP_MAX) n = EXP_MAX;
    for (i = 0; i < n && g_nhit < max && g_nhit < SR_MAX; i++) {
        char full[UC_PATH_MAX];
        char *src = 0;
        long len = 0;
        int line = 1, k, found_here = 0;
        if (!names[i][0] || isdir[i]) continue;
        uc_path_join(full, sizeof full, UC.ws_dir, names[i]);
        if (uno_fs_size(UC.ws_vol, full) > 512L * 1024) continue;
        if (!uc_read_file(UC.ws_vol, full, &src, &len)) continue;
        /* skip binaries: a NUL in the first kilobyte.  Without this the first
         * hit for any common word is inside a 700 KB TrueType font, which is
         * both useless and the only result anybody sees. */
        {
            int probe = len < 1024 ? (int)len : 1024, b, bin = 0;
            for (b = 0; b < probe; b++) if (!src[b]) { bin = 1; break; }
            if (bin) { free(src); continue; }
        }
        for (k = 0; k < (int)len && g_nhit < max && g_nhit < SR_MAX; k++) {
            int s, e, t;
            if (src[k] == '\n') { line++; continue; }
            if (src[k] != needle[0]) continue;
            if (strncmp(src + k, needle, strlen(needle))) continue;
            s = k; e = k;
            while (s > 0 && src[s-1] != '\n') s--;
            while (e < (int)len && src[e] != '\n') e++;
            uc_scpy(g_hit[g_nhit].file, names[i], sizeof g_hit[0].file);
            g_hit[g_nhit].line = line;
            for (t = 0; t < (int)sizeof g_hit[0].text - 1 && s + t < e; t++)
                g_hit[g_nhit].text[t] = src[s + t] == '\t' ? ' ' : src[s + t];
            g_hit[g_nhit].text[t] = 0;
            g_nhit++;
            found_here = 1;
            /* one hit per line: the row shows the whole line anyway, and ten
             * rows for ten matches on one line is what makes a search result
             * list unreadable.  Resume at the newline so `line` still counts. */
            k = e - 1;
        }
        if (found_here) g_hitfiles++;
        free(src);
    }
    g_hitsel = 0;
    g_hitscroll = 0;
}

/* ---- side bar ------------------------------------------------------------------- */
static const char *view_title(int v)
{
    switch (v) {
    case UC_VIEW_EXPLORER:   return "EXPLORER";
    case UC_VIEW_SEARCH:     return "SEARCH";
    case UC_VIEW_SCM:        return "SOURCE CONTROL";
    case UC_VIEW_RUN:        return "RUN AND DEBUG";
    case UC_VIEW_EXTENSIONS: return "EXTENSIONS";
    default: return "";
    }
}

static void sidebar_explorer(UcRect r)
{
    int i, rh = row_h(), y = r.y, rows = r.h / rh;
    char head[40];
    uc_scpy(head, UC.ws_dir[0] ? UC.ws_dir : uno_fs_volume_name(UC.ws_vol), sizeof head);
    uc_upper(head);
    fb_fill_rect(r.x, y, r.w, rh, uc_col(UC_C_SIDEBAR_SECT));
    uc_ui_text(r.x + 8, y + 4, head, uc_col(UC_C_SIDEBAR_FG));
    y += rh;
    rows--;
    if (g_expsel < g_expscroll) g_expscroll = g_expsel;
    if (g_expsel >= g_expscroll + rows) g_expscroll = g_expsel - rows + 1;
    for (i = 0; i < rows; i++) {
        int k = g_expscroll + i;
        int ry = y + i * rh, ind;
        ExpRow *e;
        char label[24];
        if (k >= g_nexp) break;
        e = &g_exp[k];
        ind = r.x + 8 + e->depth * uc_cfg_int("workbench.tree.indent");
        if (k == g_expsel)
            fb_fill_rect(r.x, ry, r.w, rh,
                         UC.focus == UC_F_SIDEBAR ? uc_col(UC_C_LIST_SEL_BG)
                                                  : uc_col(UC_C_LIST_INACTIVE_BG));
        if (e->isdir) {
            /* a triangle, open or closed */
            int tx = ind, ty = ry + rh / 2, j;
            for (j = 0; j < 4; j++)
                if (e->open) fb_hline(tx + 1, ty - 2 + j, 7 - j * 2, uc_col(UC_C_SIDEBAR_FG));
                else fb_vline(tx + 2 + j, ty - 4 + j, 8 - j * 2, uc_col(UC_C_SIDEBAR_FG));
            ind += 12;
        }
        uc_scpy(label, e->name, sizeof label);
        {
            UcDoc *d = uc_doc_active();
            int is_open = d && !strcmp(d->name, e->name) && !strcmp(d->dir, e->dir);
            fb_px c = e->isdir ? uc_col(UC_C_SIDEBAR_FG)
                               : (is_open ? uc_col(UC_C_LIST_SEL_FG) : uc_col(UC_C_SIDEBAR_FG));
            int k2, mod = 0;
            for (k2 = 0; k2 < uc_doc_count(); k2++) {
                UcDoc *dd = uc_doc_at(k2);
                if (dd->dirty && !strcmp(dd->name, e->name) && !strcmp(dd->dir, e->dir)) mod = 1;
            }
            if (mod) c = uc_col(UC_C_GIT_MODIFIED);
            fb_set_clip(r.x, ry, r.w - 4, rh);
            uc_ui_text_fit(ind + 2, ry + 4, label, r.x + r.w - ind - 10, c);
            fb_reset_clip();
        }
    }
    if (!g_nexp)
        uc_ui_text(r.x + 10, y + 6, "This folder is empty", uc_col(UC_C_BREADCRUMB_FG));
}

static void sidebar_search(UcRect r)
{
    int rh = row_h(), y = r.y + 6, i, rows;
    UcRect f;
    char summary[64], num[16];
    f = (UcRect){ r.x + 8, y, r.w - 16, uc_ui_h() + 8 };
    fb_fill_rect(f.x, f.y, f.w, f.h, uc_col(UC_C_INPUT_BG));
    fb_frame_rect(f.x, f.y, f.w, f.h,
                  UC.focus == UC_F_SIDEBAR ? uc_col(UC_C_FOCUS_BORDER)
                                           : uc_col(UC_C_INPUT_BORDER));
    if (g_qlen) uc_ui_text(f.x + 5, f.y + 4, g_query, uc_col(UC_C_INPUT_FG));
    else uc_ui_text(f.x + 5, f.y + 4, "Search", uc_col(UC_C_INPUT_PLACEHOLDER));
    y += f.h + 8;
    if (g_searching) uc_ui_text(r.x + 10, y, "Searching...", uc_col(UC_C_BREADCRUMB_FG));
    else if (g_qlen) {
        uc_itoa(num, g_nhit);
        uc_scpy(summary, num, sizeof summary);
        uc_scat(summary, g_nhit == 1 ? " result in " : " results in ", sizeof summary);
        uc_itoa(num, g_hitfiles);
        uc_scat(summary, num, sizeof summary);
        uc_scat(summary, g_hitfiles == 1 ? " file" : " files", sizeof summary);
        uc_ui_text(r.x + 10, y, summary, uc_col(UC_C_BREADCRUMB_FG));
    }
    y += rh;
    rows = (r.y + r.h - y) / rh;
    if (g_hitsel < g_hitscroll) g_hitscroll = g_hitsel;
    if (g_hitsel >= g_hitscroll + rows) g_hitscroll = g_hitsel - rows + 1;
    for (i = 0; i < rows; i++) {
        int k = g_hitscroll + i, ry = y + i * rh;
        char loc[28];
        if (k >= g_nhit) break;
        if (k == g_hitsel) fb_fill_rect(r.x, ry, r.w, rh, uc_col(UC_C_LIST_SEL_BG));
        uc_scpy(loc, g_hit[k].file, sizeof loc);
        uc_scat(loc, ":", sizeof loc);
        uc_itoa(num, g_hit[k].line);
        uc_scat(loc, num, sizeof loc);
        fb_set_clip(r.x, ry, r.w - 4, rh);
        uc_ui_text(r.x + 8, ry + 4, loc, uc_col(UC_C_LIST_HIGHLIGHT));
        uc_ui_text(r.x + 8 + uc_ui_text_w(loc) + 8, ry + 4, g_hit[k].text,
                   uc_col(UC_C_SIDEBAR_FG));
        fb_reset_clip();
    }
}

static void sidebar_scm(UcRect r)
{
    int i, rh = row_h(), y = r.y + 4, n = 0;
    uc_ui_text(r.x + 8, y, "LOCAL CHANGES", uc_col(UC_C_SIDEBAR_TITLE));
    y += rh + 2;
    for (i = 0; i < uc_doc_count(); i++) {
        UcDoc *d = uc_doc_at(i);
        char t[24];
        if (!d->dirty) continue;
        uc_doc_title(d, t, sizeof t);
        uc_ui_text(r.x + 14, y, t, uc_col(UC_C_GIT_MODIFIED));
        uc_ui_text(r.x + r.w - 20, y, "M", uc_col(UC_C_GIT_MODIFIED));
        y += rh;
        n++;
    }
    if (!n) {
        uc_ui_text(r.x + 10, y, "No unsaved changes.", uc_col(UC_C_BREADCRUMB_FG));
        y += rh + 6;
        uc_ui_text(r.x + 10, y, "UnoCode tracks local edits", uc_col(UC_C_BREADCRUMB_FG));
        y += rh;
        uc_ui_text(r.x + 10, y, "against the file as opened;", uc_col(UC_C_BREADCRUMB_FG));
        y += rh;
        uc_ui_text(r.x + 10, y, "there is no repository here.", uc_col(UC_C_BREADCRUMB_FG));
    }
}

static void sidebar_run(UcRect r)
{
    int i, rh = row_h(), y = r.y + 4;
    uc_ui_text(r.x + 8, y, "RUN", uc_col(UC_C_SIDEBAR_TITLE));
    y += rh + 4;
    if (!uc_launch_count()) {
        uc_ui_text(r.x + 10, y, "No launch.json.", uc_col(UC_C_BREADCRUMB_FG));
        y += rh;
        uc_ui_text(r.x + 10, y, "F5 runs the active file:", uc_col(UC_C_BREADCRUMB_FG));
        y += rh;
        uc_ui_text(r.x + 10, y, ".PY through PYRT, .UNO", uc_col(UC_C_BREADCRUMB_FG));
        y += rh;
        uc_ui_text(r.x + 10, y, "as an app.", uc_col(UC_C_BREADCRUMB_FG));
        y += rh + 6;
    }
    for (i = 0; i < uc_launch_count(); i++) {
        uc_ui_text(r.x + 14, y, uc_launch_name(i), uc_col(UC_C_SIDEBAR_FG));
        y += rh;
    }
    y += 6;
    uc_ui_text(r.x + 8, y, "TASKS", uc_col(UC_C_SIDEBAR_TITLE));
    y += rh + 2;
    for (i = 0; i < uc_tasks_count(); i++) {
        uc_ui_text(r.x + 14, y, uc_task_label(i), uc_col(UC_C_SIDEBAR_FG));
        y += rh;
    }
    if (!uc_tasks_count())
        uc_ui_text(r.x + 10, y, "No tasks.json.", uc_col(UC_C_BREADCRUMB_FG));
}

static int g_extsel;

static void sidebar_ext(UcRect r)
{
    int i, rh = row_h(), y = r.y + 4;
    char line[60];
    uc_ui_text(r.x + 8, y, "INSTALLED", uc_col(UC_C_SIDEBAR_TITLE));
    y += rh + 2;
    for (i = 0; i < uc_ext_count(); i++) {
        UcExt *e = uc_ext_at(i);
        fb_px c = e->broken ? uc_col(UC_C_ERROR_FG)
                            : (e->enabled ? uc_col(UC_C_SIDEBAR_FG)
                                          : uc_col(UC_C_BREADCRUMB_FG));
        if (i == g_extsel) fb_fill_rect(r.x, y - 2, r.w, rh * 2 + 2, uc_col(UC_C_LIST_SEL_BG));
        fb_set_clip(r.x, y - 2, r.w - 4, rh * 2 + 2);
        {
            /* the right-hand badge ("25 ms" / "disabled") is laid out first,
             * so the name and the description are fitted to what is left */
            int right = r.w - 16;
            if (e->activated || !e->enabled) right -= 64;
            uc_ui_text_fit(r.x + 10, y, e->name[0] ? e->name : e->id,
                           right - 60, c);
            {
                int w = uc_ui_text_w(e->name[0] ? e->name : e->id);
                if (w > right - 60) w = right - 60;
                uc_ui_text(r.x + 16 + w, y, e->version, uc_col(UC_C_BREADCRUMB_FG));
            }
            uc_scpy(line, e->broken ? e->err : e->desc, sizeof line);
            uc_ui_text_fit(r.x + 10, y + rh - 2, line, r.w - 20,
                           e->broken ? uc_col(UC_C_ERROR_FG)
                                     : uc_col(UC_C_BREADCRUMB_FG));
        }
        if (e->activated) {
            char ms[16];
            uc_itoa(ms, (long)e->act_ms);
            uc_scat(ms, " ms", sizeof ms);
            uc_ui_text(r.x + r.w - uc_ui_text_w(ms) - 8, y, ms, uc_col(UC_C_LIST_HIGHLIGHT));
        } else if (!e->enabled) {
            uc_ui_text(r.x + r.w - 60, y, "disabled", uc_col(UC_C_BREADCRUMB_FG));
        }
        fb_reset_clip();
        y += rh * 2 + 4;
        if (y > r.y + r.h - rh) break;
    }
    if (!uc_ext_count()) {
        uc_ui_text(r.x + 10, y, "No extensions installed.", uc_col(UC_C_BREADCRUMB_FG));
        y += rh + 6;
        uc_ui_text(r.x + 10, y, "Drop a folder into EXT\\ with", uc_col(UC_C_BREADCRUMB_FG));
        y += rh;
        uc_ui_text(r.x + 10, y, "a PACKAGE.JSN manifest, then", uc_col(UC_C_BREADCRUMB_FG));
        y += rh;
        uc_ui_text(r.x + 10, y, "run Developer: Reload Extensions.", uc_col(UC_C_BREADCRUMB_FG));
    } else {
        y = r.y + r.h - rh * 2;
        uc_ui_text(r.x + 10, y, "Enter: enable/disable", uc_col(UC_C_BREADCRUMB_FG));
        uc_ui_text(r.x + 10, y + rh - 2, "Ctrl+R: reload all", uc_col(UC_C_BREADCRUMB_FG));
    }
}

void uc_sidebar_draw(UcRect r)
{
    UcRect body;
    int th = row_h() + 4;
    fb_fill_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_SIDEBAR_BG));
    fb_vline(r.x + r.w - 1, r.y, r.h, uc_col(UC_C_SIDEBAR_BORDER));
    uc_ui_text(r.x + 10, r.y + 6, view_title(UC.view), uc_col(UC_C_SIDEBAR_TITLE));
    body = (UcRect){ r.x, r.y + th, r.w - 1, r.h - th };
    fb_set_clip(body.x, body.y, body.w, body.h);
    switch (UC.view) {
    case UC_VIEW_EXPLORER:   sidebar_explorer(body); break;
    case UC_VIEW_SEARCH:     sidebar_search(body); break;
    case UC_VIEW_SCM:        sidebar_scm(body); break;
    case UC_VIEW_RUN:        sidebar_run(body); break;
    case UC_VIEW_EXTENSIONS: sidebar_ext(body); break;
    default: break;
    }
    fb_reset_clip();
}

static void explorer_activate(int k)
{
    ExpRow *e;
    if (k < 0 || k >= g_nexp) return;
    e = &g_exp[k];
    if (e->isdir) {
        char full[UC_PATH_MAX];
        uc_path_join(full, sizeof full, e->dir, e->name);
        dir_toggle(full);
        uc_explorer_refresh();
    } else {
        int i = uc_doc_open(UC.ws_vol, e->dir, e->name);
        if (i >= 0) uc_focus(UC_F_EDITOR);
        else uc_notify("Could not open the file", UC_SEV_ERROR);
    }
}

int uc_sidebar_event(UcRect r, const unoui_event *e)
{
    int th = row_h() + 4, rh = row_h();
    if (e->kind == UI_EV_WHEEL) {
        if (UC.view == UC_VIEW_EXPLORER) {
            g_expscroll += e->wheel * 3;
            if (g_expscroll < 0) g_expscroll = 0;
            if (g_expscroll > g_nexp - 1) g_expscroll = g_nexp > 0 ? g_nexp - 1 : 0;
        } else if (UC.view == UC_VIEW_SEARCH) {
            g_hitscroll += e->wheel * 3;
            if (g_hitscroll < 0) g_hitscroll = 0;
        }
        return 1;
    }
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    uc_focus(UC_F_SIDEBAR);
    if (UC.view == UC_VIEW_EXPLORER) {
        int k = g_expscroll + (e->y - (r.y + th + rh)) / rh;
        if (e->y < r.y + th + rh) return 1;              /* the folder header */
        if (k >= 0 && k < g_nexp) {
            g_expsel = k;
            explorer_activate(k);
        }
        return 1;
    }
    if (UC.view == UC_VIEW_SEARCH) {
        int y0 = r.y + th + 6 + uc_ui_h() + 8 + 8 + rh;
        int k = g_hitscroll + (e->y - y0) / rh;
        if (k >= 0 && k < g_nhit) {
            g_hitsel = k;
            {
                int di = uc_doc_open(UC.ws_vol, UC.ws_dir, g_hit[k].file);
                UcDoc *d = uc_doc_at(di);
                if (d) uc_move_to(d, uc_line_start(d, g_hit[k].line - 1), 0);
                uc_focus(UC_F_EDITOR);
            }
        }
        return 1;
    }
    if (UC.view == UC_VIEW_EXTENSIONS) {
        int k = (e->y - (r.y + th + 4)) / (rh * 2 + 4);
        if (k >= 0 && k < uc_ext_count()) g_extsel = k;
        return 1;
    }
    return 1;
}

int uc_sidebar_key(int key, int mods, int ch)
{
    (void)mods;
    if (UC.view == UC_VIEW_EXPLORER) {
        if (key == UI_KEY_UP)   { if (g_expsel > 0) g_expsel--; return 1; }
        if (key == UI_KEY_DOWN) { if (g_expsel < g_nexp - 1) g_expsel++; return 1; }
        if (key == UI_KEY_ENTER) { explorer_activate(g_expsel); return 1; }
        if (key == UI_KEY_LEFT || key == UI_KEY_RIGHT) { explorer_activate(g_expsel); return 1; }
        return 0;
    }
    if (UC.view == UC_VIEW_SEARCH) {
        if (key == UI_KEY_BACKSPACE) {
            if (g_qlen > 0) g_query[--g_qlen] = 0;
            return 1;
        }
        if (key == UI_KEY_ENTER) { uc_search_run(g_query); return 1; }
        if (key == UI_KEY_UP)   { if (g_hitsel > 0) g_hitsel--; return 1; }
        if (key == UI_KEY_DOWN) { if (g_hitsel < g_nhit - 1) g_hitsel++; return 1; }
        if (ch >= 32 && ch < 127 && g_qlen < (int)sizeof g_query - 1) {
            g_query[g_qlen++] = (char)ch;
            g_query[g_qlen] = 0;
            return 1;
        }
        return 0;
    }
    if (UC.view == UC_VIEW_EXTENSIONS) {
        if (key == UI_KEY_UP)   { if (g_extsel > 0) g_extsel--; return 1; }
        if (key == UI_KEY_DOWN) { if (g_extsel < uc_ext_count() - 1) g_extsel++; return 1; }
        if (key == UI_KEY_ENTER) {
            UcExt *x = uc_ext_at(g_extsel);
            if (x) uc_ext_enable(g_extsel, !x->enabled);
            return 1;
        }
        return 0;
    }
    if (UC.view == UC_VIEW_RUN) {
        if (key == UI_KEY_ENTER) { uc_launch_run(-1); return 1; }
        return 0;
    }
    return 0;
}

/* ---- editor tabs ----------------------------------------------------------------- */
#define TAB_W 132

/* The strip SCROLLS to keep the active tab on screen.  A tab you cannot see
 * is a tab you cannot close, and the editor you are typing into being the one
 * off the right-hand end is worse than either. */
static int g_tabfirst;

static void tabs_reveal(UcRect r)
{
    int active = uc_doc_active_index();
    int fit = r.w / TAB_W;
    if (fit < 1) fit = 1;
    if (active < g_tabfirst) g_tabfirst = active;
    if (active >= g_tabfirst + fit) g_tabfirst = active - fit + 1;
    if (g_tabfirst > uc_doc_count() - fit) g_tabfirst = uc_doc_count() - fit;
    if (g_tabfirst < 0) g_tabfirst = 0;
}

void uc_tabs_draw(UcRect r)
{
    int i, x = r.x, active = uc_doc_active_index();
    fb_fill_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_TABS_BG));
    tabs_reveal(r);
    for (i = g_tabfirst; i < uc_doc_count(); i++) {
        UcDoc *d = uc_doc_at(i);
        char t[24];
        int on = (i == active), w = TAB_W;
        if (x + w > r.x + r.w) break;
        fb_fill_rect(x, r.y, w, r.h,
                     on ? uc_col(UC_C_TAB_ACTIVE_BG) : uc_col(UC_C_TAB_INACTIVE_BG));
        fb_vline(x + w - 1, r.y, r.h, uc_col(UC_C_TAB_BORDER));
        if (on) fb_hline(x, r.y, w - 1, uc_col(UC_C_TAB_ACTIVE_TOP));
        uc_doc_title(d, t, sizeof t);
        fb_set_clip(x + 8, r.y, w - 34, r.h);
        uc_ui_text(x + 10, r.y + (r.h - uc_ui_h()) / 2, t,
                   on ? uc_col(UC_C_TAB_ACTIVE_FG) : uc_col(UC_C_TAB_INACTIVE_FG));
        fb_reset_clip();
        if (d->dirty) {
            fb_round_rect(x + w - 22, r.y + r.h / 2 - 4, 9, 9, 4, uc_col(UC_C_TAB_MODIFIED));
        } else {
            /* the close cross */
            int cx = x + w - 20, cy = r.y + r.h / 2 - 4, k;
            fb_px c = on ? uc_col(UC_C_TAB_ACTIVE_FG) : uc_col(UC_C_TAB_INACTIVE_FG);
            for (k = 0; k < 8; k++) {
                fb_pixel(cx + k, cy + k, c);
                fb_pixel(cx + 7 - k, cy + k, c);
            }
        }
        x += w;
    }
    fb_hline(r.x, r.y + r.h - 1, r.w, uc_col(UC_C_TAB_BORDER));
}

int uc_tabs_event(UcRect r, const unoui_event *e)
{
    int i, x = r.x;
    if (e->kind == UI_EV_WHEEL) {
        g_tabfirst += e->wheel;
        if (g_tabfirst < 0) g_tabfirst = 0;
        if (g_tabfirst > uc_doc_count() - 1) g_tabfirst = uc_doc_count() - 1;
        return 1;
    }
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    for (i = g_tabfirst; i < uc_doc_count(); i++) {
        if (e->x >= x && e->x < x + TAB_W) {
            UcDoc *d = uc_doc_at(i);
            if (e->x >= x + TAB_W - 24 && !d->dirty) uc_doc_close(i);
            else { uc_doc_activate(i); uc_focus(UC_F_EDITOR); }
            return 1;
        }
        x += TAB_W;
    }
    return 1;
}

void uc_breadcrumb_draw(UcRect r)
{
    UcDoc *d = uc_doc_active();
    char path[UC_PATH_MAX + 20];
    fb_fill_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_EDITOR_BG));
    if (!d) return;
    uc_doc_path(d, path, sizeof path);
    {
        /* the path with its separators spaced out, the way breadcrumbs read */
        char out[110];
        int i = 0, o = 0;
        while (path[i] && o < (int)sizeof out - 4) {
            if (path[i] == '\\') { out[o++] = ' '; out[o++] = '>'; out[o++] = ' '; }
            else out[o++] = path[i];
            i++;
        }
        out[o] = 0;
        fb_set_clip(r.x, r.y, r.w, r.h);
        uc_ui_text(r.x + 12, r.y + (r.h - uc_ui_h()) / 2, out, uc_col(UC_C_BREADCRUMB_FG));
        fb_reset_clip();
    }
}

/* ---- the panel -------------------------------------------------------------------- */
static int g_probsel, g_probscroll, g_outscroll;

static void panel_problems(UcRect r)
{
    int i, rh = row_h(), rows = r.h / rh;
    if (!g_nprob) {
        uc_ui_text(r.x + 12, r.y + 6, "No problems have been detected.",
                   uc_col(UC_C_BREADCRUMB_FG));
        return;
    }
    if (g_probsel < g_probscroll) g_probscroll = g_probsel;
    if (g_probsel >= g_probscroll + rows) g_probscroll = g_probsel - rows + 1;
    for (i = 0; i < rows; i++) {
        int k = g_probscroll + i, y = r.y + i * rh;
        char loc[36], num[16];
        UcProblem *p;
        if (k >= g_nprob) break;
        p = &g_prob[k];
        if (k == g_probsel) fb_fill_rect(r.x, y, r.w, rh, uc_col(UC_C_LIST_SEL_BG));
        fb_fill_rect(r.x + 8, y + rh / 2 - 3, 6, 6,
                     p->sev == UC_SEV_ERROR ? uc_col(UC_C_ERROR_FG) :
                     p->sev == UC_SEV_WARN  ? uc_col(UC_C_WARN_FG)
                                            : uc_col(UC_C_INFO_FG));
        uc_scpy(loc, p->file, sizeof loc);
        uc_scat(loc, ":", sizeof loc);
        uc_itoa(num, p->line);
        uc_scat(loc, num, sizeof loc);
        fb_set_clip(r.x, y, r.w, rh);
        uc_ui_text(r.x + 20, y + 3, loc, uc_col(UC_C_LIST_HIGHLIGHT));
        uc_ui_text(r.x + 24 + uc_ui_text_w(loc), y + 3, p->msg, uc_col(UC_C_PANEL_TITLE));
        fb_reset_clip();
    }
}

static void panel_output(UcRect r)
{
    UcOutChannel *c;
    int i, rh = uc_line_h(), rows = (r.h - rh) / rh, first;
    /* the channel selector */
    for (i = 0; i < g_nout; i++) {
        int x = r.x + 10 + i * 110;
        fb_px col = (i == g_outsel) ? uc_col(UC_C_PANEL_TITLE) : uc_col(UC_C_PANEL_TITLE_DIM);
        uc_ui_text(x, r.y + 2, g_out[i].name, col);
        if (i == g_outsel) fb_hline(x, r.y + uc_ui_h() + 3, uc_ui_text_w(g_out[i].name), col);
    }
    if (!g_nout) {
        uc_ui_text(r.x + 12, r.y + 6, "No output channels yet.", uc_col(UC_C_BREADCRUMB_FG));
        return;
    }
    c = &g_out[g_outsel];
    first = c->n - rows - g_outscroll;
    if (first < 0) first = 0;
    for (i = 0; i < rows && first + i < c->n; i++)
        uc_mono(r.x + 10, r.y + rh + i * rh, c->line[(c->head + first + i) % OUT_LINES],
                uc_col(UC_C_PANEL_TITLE), 0);
}

void uc_panel_draw(UcRect r)
{
    static const char *const tabs[UC_PANEL_N] = { "PROBLEMS", "OUTPUT", "TERMINAL" };
    int i, x = r.x + 12, th = row_h() + 6;
    UcRect body;
    fb_fill_rect(r.x, r.y, r.w, r.h, uc_col(UC_C_PANEL_BG));
    fb_hline(r.x, r.y, r.w, uc_col(UC_C_PANEL_BORDER));
    for (i = 0; i < UC_PANEL_N; i++) {
        int on = UC.panel_tab == i, w;
        char label[32];
        uc_scpy(label, tabs[i], sizeof label);
        if (i == UC_PANEL_PROBLEMS && g_nprob) {
            char num[12];
            uc_itoa(num, g_nprob);
            uc_scat(label, " ", sizeof label);
            uc_scat(label, num, sizeof label);
        }
        w = uc_ui_text_w(label);
        uc_ui_text(x, r.y + 6, label,
                   on ? uc_col(UC_C_PANEL_TITLE) : uc_col(UC_C_PANEL_TITLE_DIM));
        if (on) fb_hline(x, r.y + th - 3, w, uc_col(UC_C_PANEL_TITLE));
        x += w + 22;
    }
    body = (UcRect){ r.x, r.y + th, r.w, r.h - th };
    fb_set_clip(body.x, body.y, body.w, body.h);
    switch (UC.panel_tab) {
    case UC_PANEL_PROBLEMS: panel_problems(body); break;
    case UC_PANEL_OUTPUT:   panel_output(body); break;
    case UC_PANEL_TERMINAL: uc_term_draw(body, UC.focus == UC_F_PANEL); break;
    default: break;
    }
    fb_reset_clip();
}

int uc_panel_event(UcRect r, const unoui_event *e)
{
    int i, x = r.x + 12, th = row_h() + 6;
    if (e->kind == UI_EV_WHEEL) {
        if (UC.panel_tab == UC_PANEL_OUTPUT) {
            g_outscroll += e->wheel * -3;
            if (g_outscroll < 0) g_outscroll = 0;
        } else if (UC.panel_tab == UC_PANEL_PROBLEMS) {
            g_probscroll += e->wheel * 3;
            if (g_probscroll < 0) g_probscroll = 0;
        }
        return 1;
    }
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    uc_focus(UC_F_PANEL);
    if (e->y < r.y + th) {
        for (i = 0; i < UC_PANEL_N; i++) {
            int w = uc_ui_text_w(i == 0 ? "PROBLEMS" : i == 1 ? "OUTPUT" : "TERMINAL");
            if (e->x >= x - 6 && e->x < x + w + 10) { UC.panel_tab = i; return 1; }
            x += w + 22;
        }
        return 1;
    }
    if (UC.panel_tab == UC_PANEL_PROBLEMS) {
        int k = g_probscroll + (e->y - (r.y + th)) / row_h();
        if (k >= 0 && k < g_nprob) {
            UcProblem *p = &g_prob[k];
            int di;
            g_probsel = k;
            di = uc_doc_open(p->vol, UC.ws_dir, p->file);
            if (di >= 0) {
                UcDoc *d = uc_doc_at(di);
                uc_move_to(d, uc_offset_of(d, p->line - 1, p->col > 0 ? p->col - 1 : 0), 0);
                uc_focus(UC_F_EDITOR);
            }
        }
    }
    if (UC.panel_tab == UC_PANEL_OUTPUT && e->y < r.y + th + uc_line_h()) {
        int k = (e->x - r.x - 10) / 110;
        if (k >= 0 && k < g_nout) g_outsel = k;
    }
    return 1;
}

int uc_panel_key(int key, int mods, int ch)
{
    if (UC.panel_tab == UC_PANEL_TERMINAL) return uc_term_key(key, mods, ch);
    if (UC.panel_tab == UC_PANEL_PROBLEMS) {
        if (key == UI_KEY_UP)   { if (g_probsel > 0) g_probsel--; return 1; }
        if (key == UI_KEY_DOWN) { if (g_probsel < g_nprob - 1) g_probsel++; return 1; }
    }
    return 0;
}

/* ---- status bar --------------------------------------------------------------------- */
/* THE RIGHT-HAND ITEMS ARE DRAWN FIRST, and their leftmost edge becomes the
 * clip for everything on the left.  A status bar is the one strip where two
 * independent things lay themselves out towards each other, and drawing the
 * left first means a long transient message writes straight through "Ln 1,
 * Col 1".  Which it did. */
void uc_status_draw(UcRect r)
{
    UcDoc *d = uc_doc_active();
    char buf[80], num[16];
    int x, ty = r.y + (r.h - uc_ui_h()) / 2, right_edge;
    fb_px bg = uc_doc_count() ? uc_col(UC_C_STATUS_BG) : uc_col(UC_C_STATUS_NOFOLDER);
    fb_px fg = uc_col(UC_C_STATUS_FG);
    fb_fill_rect(r.x, r.y, r.w, r.h, bg);

    /* right, in VS Code's order: position, indentation, encoding, EOL, language */
    x = r.x + r.w - 10;
    {
        UcLang *L = d ? uc_lang_at(d->lang) : 0;
        const char *lang = L ? L->name : "Plain Text";
        x -= uc_ui_text_w(lang);
        uc_ui_text(x, ty, lang, fg);
        x -= 16;
    }
    {
        const char *eol = !strcmp(uc_cfg_str("files.eol"), "crlf") ? "CRLF" : "LF";
        x -= uc_ui_text_w(eol);
        uc_ui_text(x, ty, eol, fg);
        x -= 16;
    }
    x -= uc_ui_text_w("UTF-8");
    uc_ui_text(x, ty, "UTF-8", fg);
    x -= 16;
    if (d) {
        uc_scpy(buf, uc_doc_spaces(d) ? "Spaces: " : "Tab Size: ", sizeof buf);
        uc_itoa(num, uc_doc_tabsize(d));
        uc_scat(buf, num, sizeof buf);
        x -= uc_ui_text_w(buf);
        uc_ui_text(x, ty, buf, fg);
        x -= 16;
        uc_scpy(buf, "Ln ", sizeof buf);
        uc_itoa(num, uc_line_of(d, d->cur[d->ncur-1].caret) + 1);
        uc_scat(buf, num, sizeof buf);
        uc_scat(buf, ", Col ", sizeof buf);
        uc_itoa(num, uc_col_of(d, d->cur[d->ncur-1].caret) + 1);
        uc_scat(buf, num, sizeof buf);
        if (d->ncur > 1) {
            uc_itoa(num, d->ncur);
            uc_scat(buf, "  (", sizeof buf);
            uc_scat(buf, num, sizeof buf);
            uc_scat(buf, " sel)", sizeof buf);
        }
        x -= uc_ui_text_w(buf);
        uc_ui_text(x, ty, buf, fg);
    }
    right_edge = x - 10;

    /* left: the folder, the problem counts, then whatever room is left */
    x = r.x + 8;
    uc_scpy(buf, uno_fs_volume_name(UC.ws_vol), sizeof buf);
    if (UC.ws_dir[0]) { uc_scat(buf, "\\", sizeof buf); uc_scat(buf, UC.ws_dir, sizeof buf); }
    fb_set_clip(r.x, r.y, right_edge > r.x ? right_edge - r.x : 0, r.h);
    uc_ui_text(x, ty, buf, fg);
    x += uc_ui_text_w(buf) + 16;

    uc_itoa(num, uc_problems_count(UC_SEV_ERROR));
    uc_scpy(buf, "E ", sizeof buf);
    uc_scat(buf, num, sizeof buf);
    uc_itoa(num, uc_problems_count(UC_SEV_WARN));
    uc_scat(buf, "   W ", sizeof buf);
    uc_scat(buf, num, sizeof buf);
    uc_ui_text(x, ty, buf, fg);
    x += uc_ui_text_w(buf) + 16;

    if (uc_status_msg_get()[0]) uc_ui_text(x, ty, uc_status_msg_get(), fg);
    fb_reset_clip();
}

int uc_status_event(UcRect r, const unoui_event *e)
{
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    /* the right-hand items are click targets, exactly as they are in VS Code */
    if (e->x > r.x + r.w - 90) uc_quick_open(UC_Q_LANG);
    else if (e->x > r.x + r.w - 320) uc_quick_open(UC_Q_LINE);
    else if (e->x < r.x + 200) uc_toggle_sidebar(UC_VIEW_EXPLORER);
    else uc_toggle_panel(UC_PANEL_PROBLEMS);
    return 1;
}

void uc_view_init(void)
{
    g_nprob = 0;
    g_nout = 0;
    g_nnotif = 0;
    g_nexp = 0;
    g_nopen = 0;
    uc_output_channel("Log");
    uc_explorer_refresh();
}
