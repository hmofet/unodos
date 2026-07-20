/* ===========================================================================
 * UnoDOS/pc64 - Files: a real file manager for the unoui shell.
 *
 * Replaces the old hardcoded 3-row list with a live view of every mounted
 * volume (RAM disk, native FAT disks, firmware USB), with:
 *   - a toolbar of real operations: Up, New folder, Rename, Delete (armed
 *     twice), Copy, Move, Refresh - wired to the native FAT driver
 *     (uno_fat_mkdir / uno_fat_rename / uno_fat_delete / read+write copy)
 *   - subdirectory navigation on FAT volumes (Enter or the Open button; ".."
 *     row / Up to go back)
 *   - an optional TWO-PANE commander layout: Copy/Move then target the other
 *     pane's directory
 *
 * The panes are one canvas (drawn + hit-tested here); the toolbar is normal
 * unoui widgets. Non-FAT volumes (RAM, firmware) list read-only-ish: files
 * open/copy FROM them, but mkdir/rename/delete need native FAT.
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "pc64_fs.h"
#include "fat.h"
#include "pc64_icons.h"     /* pc64_shell_theme */
#include <string.h>

void pc64_shell_dirty(void);
int  pc64_shell_run_user(int vol, const char *path);   /* launch a .UNO app */
int  pc64_shell_workarea_w(void);
int  pc64_shell_workarea_h(void);

/* ---- widget ids (400..429 reserved for Files) ----------------------------- */
enum {
    FID_UP = 400, FID_OPEN, FID_MKDIR, FID_RENAME, FID_DELETE,
    FID_COPY, FID_MOVE, FID_REFRESH, FID_TWOPANE,
    FID_NAME, FID_VOL_L, FID_VOL_R
};

/* ---- pane state ----------------------------------------------------------- */
#define FM_MAXE 96
typedef struct {
    int  vol;                      /* uno_fs volume index */
    char path[120];                /* FAT subdir path ("" = root) */
    uno_fat_entry e[FM_MAXE];
    int  n, sel, scroll;           /* scroll = first visible row */
} fm_pane;

static fm_pane fm_p[2];
static int fm_two, fm_active;      /* two-pane on/off; which pane is active */
static int fm_arm_del = -1;        /* Delete pressed once on this pane-sel   */
static char fm_status[96] = "Pick a file; ops need a native FAT volume.";
static char fm_name[16];           /* the Name: field (rename / new folder)  */
static unoui_text fm_name_t;

static unoui_window *fm_win;
static unoui_widget *fm_dd_l, *fm_dd_r, *fm_bt_two;
static int fm_canvas_wi = -1;
static const char *fm_vols[12];
static int fm_nvols;

static char *fcat(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
/* bounded fcat: never writes past end (which must point at the last slot) */
static char *fcatn(char *p, char *end, const char *s) { while (*s && p < end) *p++ = *s++; return p; }
static char *fcat_int(char *p, long v)
{ char t[14]; int n = 0; if (v < 0) { *p++ = '-'; v = -v; }
  if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
  while (n) *p++ = t[--n]; return p; }

static void set_status(const char *s) { strncpy(fm_status, s, 95); fm_status[95] = 0; }
static void toggle_two(void);

/* join pane path + name into out ("A\B" or "B" at root) */
static void join_path(const fm_pane *P, const char *name, char *out, int max)
{
    char *p = out, *end = out + max - 1;
    const char *s = P->path;
    while (*s && p < end) *p++ = *s++;
    if (p != out && p < end) *p++ = '\\';
    s = name; while (*s && p < end) *p++ = *s++;
    *p = 0;
}

static int pane_is_fat(const fm_pane *P) { return uno_fs_kind(P->vol) == 1; }

static void pane_refresh(fm_pane *P)
{
    P->n = 0;
    if (pane_is_fat(P)) {
        int fv = uno_fs_fat_index(P->vol);
        int n = uno_fat_list_ex(fv, P->path[0] ? P->path : 0, P->e, FM_MAXE);
        P->n = n > FM_MAXE ? FM_MAXE : n;
    } else {
        int n = uno_fs_list_begin(P->vol), i, k = 0;
        for (i = 0; i < n && k < FM_MAXE; i++) {
            char nm[16];
            if (!uno_fs_list_get(P->vol, i, nm, sizeof nm)) continue;
            strncpy(P->e[k].name, nm, 12); P->e[k].name[12] = 0;
            P->e[k].is_dir = 0; P->e[k].size = -1;
            k++;
        }
        P->n = k;
        P->path[0] = 0;                       /* flat stores have no subdirs */
    }
    if (P->sel >= P->n) P->sel = P->n - 1;
    if (P->sel < 0 && P->n > 0) P->sel = 0;
    if (P->scroll > P->n - 1) P->scroll = P->n > 0 ? P->n - 1 : 0;
    fm_arm_del = -1;
}

static void pane_up(fm_pane *P)
{
    int i = (int)strlen(P->path);
    if (!i) return;
    while (i > 0 && P->path[i - 1] != '\\') i--;
    if (i > 0) i--;
    P->path[i] = 0;
    P->sel = 0; P->scroll = 0;
    pane_refresh(P);
}

static void pane_enter(fm_pane *P)
{
    uno_fat_entry *E;
    if (P->sel < 0 || P->sel >= P->n) return;
    E = &P->e[P->sel];
    if (!E->is_dir) {
        int L = 0; while (E->name[L]) L++;
        if (L >= 4 && E->name[L-4] == '.' &&
            (E->name[L-3] == 'U' || E->name[L-3] == 'u') &&
            (E->name[L-2] == 'N' || E->name[L-2] == 'n') &&
            (E->name[L-1] == 'O' || E->name[L-1] == 'o')) {
            char np[120]; join_path(P, E->name, np, sizeof np);
            if (pc64_shell_run_user(P->vol, np) == 0) set_status("Launched.");
            else set_status("Could not launch that .UNO.");
        } else {
            set_status("That is a file (dirs open with Enter/Open).");
        }
        return;
    }
    { char np[120]; join_path(P, E->name, np, sizeof np);
      strcpy(P->path, np); }
    P->sel = 0; P->scroll = 0;
    pane_refresh(P);
}

/* ---- operations ----------------------------------------------------------- */
static fm_pane *pact(void)  { return &fm_p[fm_two ? fm_active : 0]; }
static fm_pane *pother(void){ return &fm_p[fm_two ? !fm_active : 0]; }

static void op_mkdir(void)
{
    fm_pane *P = pact();
    char path[136];
    if (!pane_is_fat(P)) { set_status("New folder needs a native FAT volume."); return; }
    if (!fm_name[0]) { set_status("Type the new folder's name in the Name box."); return; }
    join_path(P, fm_name, path, sizeof path);
    if (uno_fat_mkdir(uno_fs_fat_index(P->vol), path)) {
        set_status("Folder created.");
        pane_refresh(P);
    } else set_status("Could not create the folder (exists? bad name?).");
}

static void op_rename(void)
{
    fm_pane *P = pact();
    char path[136];
    if (!pane_is_fat(P)) { set_status("Rename needs a native FAT volume."); return; }
    if (P->sel < 0 || P->sel >= P->n) { set_status("Select something to rename."); return; }
    if (!fm_name[0]) { set_status("Type the new name in the Name box."); return; }
    join_path(P, P->e[P->sel].name, path, sizeof path);
    if (uno_fat_rename(uno_fs_fat_index(P->vol), path, fm_name)) {
        set_status("Renamed.");
        pane_refresh(P);
    } else set_status("Rename failed (name taken or invalid).");
}

static void op_delete(void)
{
    fm_pane *P = pact();
    char path[136];
    int key = (fm_two ? fm_active : 0) * 1000 + P->sel;
    if (!pane_is_fat(P)) { set_status("Delete needs a native FAT volume."); return; }
    if (P->sel < 0 || P->sel >= P->n) { set_status("Select something to delete."); return; }
    if (fm_arm_del != key) {
        fm_arm_del = key;
        set_status("Deletes it permanently - press Delete again.");
        return;
    }
    fm_arm_del = -1;
    if (P->e[P->sel].is_dir) {                    /* only empty directories */
        uno_fat_entry probe[2];
        char dp[136];
        join_path(P, P->e[P->sel].name, dp, sizeof dp);
        if (uno_fat_list_ex(uno_fs_fat_index(P->vol), dp, probe, 2) > 0) {
            set_status("That folder is not empty.");
            return;
        }
    }
    join_path(P, P->e[P->sel].name, path, sizeof path);
    if (uno_fat_delete(uno_fs_fat_index(P->vol), path)) {
        set_status("Deleted.");
        pane_refresh(P);
    } else set_status("Delete failed.");
}

/* copy buffer: bounded, static (freestanding - no heap games) */
#define FM_COPYMAX (2 * 1024 * 1024)
static unsigned char fm_buf[FM_COPYMAX];

static int read_src(fm_pane *P, const char *name, long *n)
{
    char path[136];
    join_path(P, name, path, sizeof path);
    if (pane_is_fat(P)) *n = uno_fat_read(uno_fs_fat_index(P->vol), path, fm_buf, FM_COPYMAX);
    else                *n = uno_fs_read(P->vol, name, fm_buf, FM_COPYMAX);
    return *n >= 0;
}
static int write_dst(fm_pane *P, const char *name, long n)
{
    char path[136];
    if (pane_is_fat(P)) {
        join_path(P, name, path, sizeof path);
        return uno_fat_write(uno_fs_fat_index(P->vol), path, fm_buf, n);
    }
    return uno_fs_write(P->vol, name, fm_buf, n);
}

static int op_copy_inner(int also_delete)
{
    fm_pane *S = pact(), *D = pother();
    long n;
    char dstname[16];
    if (S->sel < 0 || S->sel >= S->n) { set_status("Select a file to copy."); return 0; }
    if (S->e[S->sel].is_dir) { set_status("Folders can't be copied yet."); return 0; }
    if (!read_src(S, S->e[S->sel].name, &n)) { set_status("Read failed."); return 0; }
    if (n >= FM_COPYMAX) { set_status("File too big for the copy buffer (2 MB)."); return 0; }
    strcpy(dstname, S->e[S->sel].name);
    if (!fm_two || (S == D)) {                    /* same dir: prefix the copy */
        char tmp[16]; char *p = tmp;
        p = fcat(p, "C_");
        strncpy(p, dstname, 10); p[10] = 0;
        strcpy(dstname, tmp);
    }
    if (!write_dst(D, dstname, n)) { set_status("Write failed (read-only target?)."); return 0; }
    if (also_delete) {
        char path[136];
        join_path(S, S->e[S->sel].name, path, sizeof path);
        if (pane_is_fat(S)) uno_fat_delete(uno_fs_fat_index(S->vol), path);
        pane_refresh(S);
    }
    pane_refresh(D);
    set_status(also_delete ? "Moved." : "Copied.");
    return 1;
}
static void op_copy(void) { op_copy_inner(0); }
static void op_move(void) { op_copy_inner(1); }

/* ---- the pane canvas ------------------------------------------------------ */
static unoui_rect fm_last_r;

static int row_h(void) { return ui_row_h() + 2; }

static void draw_pane(const unoui_theme *t, fm_pane *P, unoui_rect r, int active)
{
    int fh = fb_text_h(), rh = row_h(), hdr = fh + 6;
    int rows = (r.h - hdr) / rh, i;
    char head[160]; char *p, *hend = head + sizeof head - 1;
    /* header: VOLUME\PATH (bounded: label ~11 + sep + path up to 119) */
    p = fcatn(head, hend, uno_fs_volume_name(P->vol));
    if (P->path[0]) { p = fcatn(p, hend, "\\"); p = fcatn(p, hend, P->path); }
    *p = 0;
    fb_fill_rect(r.x, r.y, r.w, hdr, active ? t->pal.accent : t->pal.face);
    fb_set_clip(r.x, r.y, r.w - 4, hdr);
    fb_text(r.x + 5, r.y + 3, head, active ? t->pal.accent_text : t->pal.face_text, -1);
    fb_set_clip(r.x, r.y, r.w, r.h);
    /* rows */
    fb_fill_rect(r.x, r.y + hdr, r.w, r.h - hdr, t->pal.field_bg);
    if (P->scroll > P->n - rows) P->scroll = P->n - rows;
    if (P->scroll < 0) P->scroll = 0;
    for (i = 0; i < rows && P->scroll + i < P->n; i++) {
        uno_fat_entry *E = &P->e[P->scroll + i];
        int y = r.y + hdr + i * rh, selr = (P->scroll + i == P->sel);
        fb_px fg = t->pal.field_text;
        if (selr) { fb_fill_rect(r.x, y, r.w, rh, t->pal.accent); fg = t->pal.accent_text; }
        /* tiny emblem: folder tab or doc page */
        if (E->is_dir) {
            fb_fill_rect(r.x + 5, y + rh/2 - 4, 12, 8, selr ? t->pal.accent_text : t->pal.accent);
            fb_fill_rect(r.x + 5, y + rh/2 - 6, 6, 2,  selr ? t->pal.accent_text : t->pal.accent);
        } else {
            fb_frame_rect(r.x + 6, y + rh/2 - 5, 9, 11, fg);
            fb_hline(r.x + 8, y + rh/2 - 2, 5, fg);
            fb_hline(r.x + 8, y + rh/2,     5, fg);
            fb_hline(r.x + 8, y + rh/2 + 2, 5, fg);
        }
        fb_set_clip(r.x, y, r.w - 66, rh);
        fb_text(r.x + 22, y + (rh - fh) / 2, E->name, fg, -1);
        fb_set_clip(r.x, r.y, r.w, r.h);
        if (E->is_dir) {
            fb_text(r.x + r.w - 6 - fb_text_w("<dir>"), y + (rh - fh) / 2, "<dir>", fg, -1);
        } else if (E->size >= 0) {
            char sz[16]; char *q = sz;
            long v = E->size;
            if (v >= 10240) { q = fcat_int(q, v >> 10); q = fcat(q, " K"); }
            else            { q = fcat_int(q, v); q = fcat(q, " B"); }
            *q = 0;
            fb_text(r.x + r.w - 6 - fb_text_w(sz), y + (rh - fh) / 2, sz, fg, -1);
        }
    }
    fb_frame_rect(r.x, r.y, r.w, r.h, active ? t->pal.accent : t->pal.shadow);
}

static void fm_canvas_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = pc64_shell_theme();
    int fh = fb_text_h(), stat_h = fh + 6;
    unoui_rect area = { r.x, r.y, r.w, r.h - stat_h };
    (void)w; (void)ctx;
    fm_last_r = r;
    if (fm_two) {
        unoui_rect L = { area.x, area.y, area.w / 2 - 2, area.h };
        unoui_rect R = { area.x + area.w / 2 + 2, area.y,
                         area.w - area.w / 2 - 2, area.h };
        draw_pane(t, &fm_p[0], L, fm_active == 0);
        draw_pane(t, &fm_p[1], R, fm_active == 1);
    } else {
        draw_pane(t, &fm_p[0], area, 1);
    }
    /* status strip */
    { int sy = r.y + r.h - stat_h;
      fb_fill_rect(r.x, sy, r.w, stat_h, t->pal.face);
      fb_hline(r.x, sy, r.w, t->pal.shadow);
      fb_set_clip(r.x, sy, r.w - 4, stat_h);
      fb_text(r.x + 6, sy + 3, fm_status, t->pal.face_text, -1);
      fb_set_clip(r.x, r.y, r.w, r.h); }
}

/* which pane rect contains (x,y)? fills *pr with that pane's rect */
static int pane_at(int x, int y, unoui_rect *pr)
{
    int fh = fb_text_h(), stat_h = fh + 6;
    unoui_rect area = { fm_last_r.x, fm_last_r.y, fm_last_r.w, fm_last_r.h - stat_h };
    (void)y;
    if (!fm_two) { *pr = area; return 0; }
    if (x < area.x + area.w / 2) {
        pr->x = area.x; pr->y = area.y; pr->w = area.w / 2 - 2; pr->h = area.h;
        return 0;
    }
    pr->x = area.x + area.w / 2 + 2; pr->y = area.y;
    pr->w = area.w - area.w / 2 - 2; pr->h = area.h;
    return 1;
}

static int fm_canvas_event(struct unoui_widget *w, const void *evp, void *ctx)
{
    const unoui_event *e = (const unoui_event *)evp;
    (void)w; (void)ctx;
    switch (e->kind) {
    case UI_EV_MOUSE_DOWN: {
        unoui_rect pr;
        int pi = pane_at(e->x, e->y, &pr);
        int fh = fb_text_h(), hdr = fh + 6, row;
        fm_pane *P = &fm_p[pi];
        fm_active = pi;
        row = (e->y - (pr.y + hdr)) / row_h();
        if (e->y >= pr.y + hdr && row >= 0 && P->scroll + row < P->n) {
            if (P->sel == P->scroll + row) pane_enter(P);   /* re-click opens */
            else P->sel = P->scroll + row;
            fm_arm_del = -1;
        }
        pc64_shell_dirty();
        return 1;
    }
    case UI_EV_WHEEL: {
        unoui_rect pr;
        int pi = pane_at(e->x, e->y, &pr);
        fm_pane *P = &fm_p[pi];
        P->scroll += e->wheel * 3;
        if (P->scroll < 0) P->scroll = 0;
        if (P->scroll > P->n - 1) P->scroll = P->n > 0 ? P->n - 1 : 0;
        pc64_shell_dirty();
        return 1;
    }
    case UI_EV_CHAR:
        if (e->ch == '2') { toggle_two(); pc64_shell_dirty(); return 1; }
        return 0;
    case UI_EV_KEY: {
        fm_pane *P = pact();
        switch (e->key) {
        case UI_KEY_UP:   if (P->sel > 0) P->sel--; break;
        case UI_KEY_DOWN: if (P->sel < P->n - 1) P->sel++; break;
        case UI_KEY_ENTER: pane_enter(P); break;
        case UI_KEY_BACKSPACE: pane_up(P); break;
        case UI_KEY_TAB: if (fm_two) fm_active = !fm_active; break;
        default: return 0;
        }
        { int fh = fb_text_h(), rows = (fm_last_r.h - (fh+6) - (fh+6)) / row_h();
          if (P->sel < P->scroll) P->scroll = P->sel;
          if (rows > 0 && P->sel >= P->scroll + rows) P->scroll = P->sel - rows + 1; }
        fm_arm_del = -1;
        pc64_shell_dirty();
        return 1;
    }
    default: return 0;
    }
}
static unoui_canvas fm_canvas = { fm_canvas_draw, fm_canvas_event, 0 };

static void toggle_two(void)
{
    fm_two = !fm_two;
    if (fm_bt_two) fm_bt_two->text = fm_two ? "One pane" : "Two panes";
    if (fm_two && fm_p[1].n == 0) pane_refresh(&fm_p[1]);
    fm_active = 0;
}

/* ---- actions -------------------------------------------------------------- */
int pc64_files_action(const unoui_action *a)
{
    if (a->id < 400 || a->id >= 430) return 0;
    switch (a->id) {
    case FID_UP:      pane_up(pact()); break;
    case FID_OPEN:    pane_enter(pact()); break;
    case FID_MKDIR:   op_mkdir(); break;
    case FID_RENAME:  op_rename(); break;
    case FID_DELETE:  op_delete(); break;
    case FID_COPY:    op_copy(); break;
    case FID_MOVE:    op_move(); break;
    case FID_REFRESH: pane_refresh(&fm_p[0]); if (fm_two) pane_refresh(&fm_p[1]);
                      set_status("Refreshed."); break;
    case FID_TWOPANE:
        toggle_two();
        break;
    case FID_VOL_L:
        if (a->value >= 0 && a->value < fm_nvols && a->value != fm_p[0].vol) {
            fm_p[0].vol = a->value; fm_p[0].path[0] = 0;
            fm_p[0].sel = fm_p[0].scroll = 0;
            pane_refresh(&fm_p[0]);
        }
        break;
    case FID_VOL_R:
        if (a->value >= 0 && a->value < fm_nvols && a->value != fm_p[1].vol) {
            fm_p[1].vol = a->value; fm_p[1].path[0] = 0;
            fm_p[1].sel = fm_p[1].scroll = 0;
            pane_refresh(&fm_p[1]);
        }
        break;
    case FID_NAME: break;                          /* Enter in the name box */
    default: return 0;
    }
    pc64_shell_dirty();
    return 1;
}

int pc64_files_canvas_index(void) { return fm_canvas_wi; }

/* ---- window build --------------------------------------------------------- */
void pc64_files_build(unoui_window *w)
{
    unoui_widget *x;
    int fh = fb_text_h(), ch = ui_field_h(), bh = ui_ctl_h();
    int waw = pc64_shell_workarea_w(), wah = pc64_shell_workarea_h();
    int ww = waw / 2, wh = wah * 3 / 5, cw, y, i, bx;
    const unoui_theme *t = pc64_shell_theme();
    if (ww < 470) ww = 470; if (ww > 860) ww = 860;
    if (wh < 320) wh = 320; if (wh > wah - 24) wh = wah - 24;
    int need1, need2;
#define FBTN(label, wid, keep) do { \
        int bw_ = fb_text_w(label) + 16; \
        x = unoui_add_button(w, bx, y, bw_, label, 0); x->id = (wid); \
        keep; bx += bw_ + 4; } while (0)
    unoui_window_init(w, "Files", 120, 64, ww, wh);
    fm_win = w; (void)fm_win;
    fm_nvols = uno_fs_volumes(); if (fm_nvols > 12) fm_nvols = 12;
    for (i = 0; i < fm_nvols; i++) fm_vols[i] = uno_fs_volume_name(i);
    if (fm_p[0].vol >= fm_nvols) fm_p[0].vol = 0;
    if (fm_p[1].vol >= fm_nvols) fm_p[1].vol = fm_nvols > 1 ? 1 : 0;
    /* toolbar row 1: navigation + volume pickers (measured flow layout) */
    y = 2; bx = 0;
    FBTN("Up", FID_UP, (void)0);
    FBTN("Open", FID_OPEN, (void)0);
    { int dw = fb_text_w("MMMMMM") + 26;
      x = unoui_add_dropdown(w, bx, y, dw, fm_vols, fm_nvols, fm_p[0].vol);
      x->id = FID_VOL_L; fm_dd_l = x; bx += dw + 4;
      x = unoui_add_dropdown(w, bx, y, dw, fm_vols, fm_nvols, fm_p[1].vol);
      x->id = FID_VOL_R; fm_dd_r = x; (void)fm_dd_r; bx += dw + 4; }
    { int bw_ = fb_text_w("Two panes") + 16;
      x = unoui_add_button(w, bx, y, bw_, fm_two ? "One pane" : "Two panes", 0);
      x->id = FID_TWOPANE; fm_bt_two = x; bx += bw_ + 4; }
    FBTN("Refresh", FID_REFRESH, (void)0);
    need1 = bx;
    y += bh + 4;
    /* toolbar row 2: file operations + name box */
    bx = 0;
    FBTN("Folder+", FID_MKDIR, (void)0);
    FBTN("Rename", FID_RENAME, (void)0);
    FBTN("Delete", FID_DELETE, (void)0);
    FBTN("Copy", FID_COPY, (void)0);
    FBTN("Move", FID_MOVE, (void)0);
    unoui_add_label(w, bx, y + (ch - fh) / 2, "Name:"); bx += fb_text_w("Name:") + 8;
    need2 = bx + 80;                          /* name box wants >= 80 px */
    /* the window must fit the wider toolbar row */
    { int need = (need1 > need2 ? need1 : need2)
                 + 2 * t->m.frame_w + 2 * t->m.pad;
      if (ww < need) ww = need;
      if (ww > waw - 8) ww = waw - 8;
      w->r.w = ww; }
    cw = ww - 2 * t->m.frame_w - 2 * t->m.pad;
    unoui_text_init(&fm_name_t, fm_name, sizeof fm_name, 0);
    { int fw = cw - bx; if (fw < 60) fw = 60;
      x = unoui_add_edit(w, bx, y, fw, &fm_name_t); x->id = FID_NAME; }
    y += ch + 6;
#undef FBTN
    /* the panes fill the rest */
    x = unoui_add_canvas(w, 0, y, cw, 100, &fm_canvas);
    unoui_widget_fill(x);
    fm_canvas_wi = w->nw - 1;
    unoui_reflow_window(t, w);
    w->min_w = (need1 > need2 ? need1 : need2) + 2 * t->m.frame_w + 2 * t->m.pad;
    w->min_h = 240;
    w->flags |= UI_WIN_RESIZE;
    pane_refresh(&fm_p[0]);
    if (fm_two) pane_refresh(&fm_p[1]);
}
