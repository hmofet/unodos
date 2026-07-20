/* ===========================================================================
 * Studio - the UnoDOS pc64 IDE.  A unoui-CLASS .UNO module (APPS\STUDIO.UNO):
 * a monospace code editor with UnoC syntax highlighting, a project file list,
 * a build-output pane, an AI assistant column, and the in-OS UnoC compiler
 * (apps/ucc.c) - all of it running as a loadable app the shell hosts like any
 * built-in.  F7 builds the open file into a .UNO; F5 runs it in a live window.
 *
 * The module reaches the kernel only through the named exports in
 * pc64_modload.c's kExports[] - the toolkit, the framebuffer, the TrueType
 * text path, the FAT filesystem, malloc, and a few shell services - so it
 * carries no kernel code and a distro drops it by not shipping the file.
 * ======================================================================== */
/* -I. = pc64, -I../unoui supply these; apps live in pc64/apps */
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "uno_uuiapp.h"
#include "ucc.h"
#include "studio_hl.h"
#include "studio_ai.h"

#ifdef UNO_APP_SYM
#  define uno_app_main UNO_APP_SYM
#endif

/* ---- kernel imports (all resolved against kExports[]) --------------------- */
void  fb_fill_rect(int x, int y, int w, int h, fb_px c);
void  fb_hline(int x, int y, int w, fb_px c);
void  fb_vline(int x, int y, int h, fb_px c);
int   fb_text(int x, int y, const char *s, fb_px fg, long bg);
int   fb_text_w(const char *s);
int   fb_text_h(void);
int   fb_width(void);
int   fb_height(void);
int   uno_font_draw_styled(int slot, int px, int style, int x, int y,
                           const char *s, fb_px fg, long bg);
int   uno_font_text_w_styled(int slot, int px, int style, const char *s);
int   uno_font_height_px(int slot, int px);
int   uno_font_baseline_px(int slot, int px);

int   uno_fs_volumes(void);
const char *uno_fs_volume_name(int vol);
int   uno_fs_list_begin(int vol);
int   uno_fs_list_get(int vol, int idx, char *name, int max);
long  uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
long  uno_fs_size(int vol, const char *name);
int   uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);
int   uno_fs_writable(int vol);
int   uno_fs_kind(int vol);

void  pc64_shell_dirty(void);
const struct unoui_theme *pc64_shell_theme(void);
int   pc64_shell_run_user(int vol, const char *path);
int   pc64_shell_font_mono(void);
void  pc64_browser_open_path(const char *path);

const char *pc64_shell_py_error(void);       /* PYRT compile/exec traceback */

/* studio_py.c - Python app packaging (source -> UNO_MODF_PYAPP container) */
int   studio_is_py(const char *name);
int   studio_py_pack(const unsigned char *src, int len, unsigned char *out, int cap);

void *malloc(unsigned long n);
void  free(void *p);
void *memcpy(void *d, const void *s, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
unsigned long strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, unsigned long n);
char *strcpy(char *d, const char *s);

/* ---- geometry / buffers --------------------------------------------------- */
#define ED_CAP    (192 * 1024)     /* source ceiling (v1)                     */
#define WORK_CAP  (3 * 1024 * 1024)/* ucc arena                               */
#define UNO_CAP   (256 * 1024)     /* .UNO output                             */
#define CLIP_CAP  (32 * 1024)
#define OUT_ROWS  6                 /* build-output lines shown                */
#define MAXPROJ   128

static int   g_mono = -1;          /* mono font slot                          */
static int   g_px   = 15;          /* editor font pixel size                  */
static int   g_cw, g_lh, g_asc;    /* char width, line height, ascent         */

/* the document */
static char *ed_buf;
static int   ed_len, ed_caret, ed_sel;   /* sel == caret => no selection      */
static int   ed_scroll;                   /* first visible line               */
static int   ed_hscroll;                  /* first visible column             */
static int   ed_dirty;                    /* unsaved edits                    */
static char  ed_name[16];                 /* 8.3 file name, "" = untitled     */
static int   ed_vol = -1;                 /* volume the doc lives on          */

/* work + output */
static void         *g_work;
static unsigned char *g_uno;
static char  clip[CLIP_CAP]; static int clip_len;

/* build output ring (text lines) + jump targets */
static char  out_line[OUT_ROWS][80];
static int   out_n;
static UccDiag g_diag[16];
static int   g_ndiag;

/* project file list */
static char  proj_name[MAXPROJ][16];
static int   proj_n, proj_sel, proj_scroll;
static int   proj_vol = -1;

/* focus + panes */
enum { PANE_EDIT, PANE_PROJ, PANE_AI };
static int   g_focus = PANE_EDIT;
static int   g_show_proj = 1, g_show_ai = 1;

/* filename entry mode (Save As / New): the status bar becomes an input box */
static int   name_mode;             /* 0 none, 1 save-as, 2 new                */
static char  name_in[16]; static int name_inlen;

/* menu state */
static int   menu_open = -1;        /* which top menu is dropped, -1 = none    */
static unoui_window *g_win;

/* ---- layout rects (recomputed from the canvas rect each draw/hit) --------- */
typedef struct { int x, y, w, h; } R;
static R L_menu, L_proj, L_edit, L_out, L_ai, L_status;

static int menuh(void) { return g_lh + 6; }

static void layout(unoui_rect c)
{
    int mh = menuh(), sh = g_lh + 4, oh = g_lh * OUT_ROWS + 6;
    /* the AI column wants room; on the default chunky desktop it stays hidden
     * (raise the resolution in the Control Panel to bring it in) */
    int lw = (g_show_proj && c.w >= 470) ? 116 : 0;
    int rw = (g_show_ai   && c.w >= 720) ? 208 : 0;
    int bodyy = c.y + mh, bodyh = c.h - mh - sh;
    L_menu   = (R){ c.x, c.y, c.w, mh };
    L_status = (R){ c.x, c.y + c.h - sh, c.w, sh };
    L_proj   = (R){ c.x, bodyy, lw, bodyh };
    L_ai     = (R){ c.x + c.w - rw, bodyy, rw, bodyh };
    L_edit   = (R){ c.x + lw, bodyy, c.w - lw - rw, bodyh - oh };
    L_out    = (R){ c.x + lw, bodyy + bodyh - oh, c.w - lw - rw, oh };
}

static int in_rect(R r, int x, int y)
{ return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

/* ---- theme helpers -------------------------------------------------------- */
static const struct unoui_theme *TH(void) { return pc64_shell_theme(); }

static fb_px hl_color(int cls, const unoui_palette *p)
{
    switch (cls) {
    case HL_KW:      return FB_RGB(0xC5, 0x86, 0xC0);
    case HL_TYPE:    return FB_RGB(0x4E, 0xC9, 0xB0);
    case HL_STR:     return FB_RGB(0xCE, 0x91, 0x78);
    case HL_CHAR:    return FB_RGB(0xD7, 0xBA, 0x7D);
    case HL_NUM:     return FB_RGB(0xB5, 0xCE, 0xA8);
    case HL_COMMENT: return FB_RGB(0x6A, 0x99, 0x55);
    case HL_PREPROC: return FB_RGB(0x9B, 0x9B, 0x9B);
    default:         return p->field_text;
    }
}

/* ---- small utilities ------------------------------------------------------ */
static void s_cpy(char *d, const char *s, int cap)
{ int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int  s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void u_dec(char *out, long v)
{
    char t[16]; int n = 0, i = 0;
    if (v <= 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[i++] = t[--n];
    out[i] = 0;
}

static void draw_mono(int x, int y, const char *s, fb_px fg)
{ uno_font_draw_styled(g_mono, g_px, 0, x, y, s, fg, -1); }

/* draw one glyph via a 1-char string (monospace positioning by caller) */
static void draw_ch(int x, int y, char ch, fb_px fg)
{ char b[2]; b[0] = ch; b[1] = 0; draw_mono(x, y, b, fg); }

/* ---- editor line indexing ------------------------------------------------- */
static int line_start(int line)                 /* byte offset of line N start */
{
    int i = 0, ln = 0;
    if (line <= 0) return 0;
    for (; i < ed_len; i++) {
        if (ed_buf[i] == '\n') { if (++ln == line) return i + 1; }
    }
    return ed_len;
}
static int line_end(int off)                    /* offset of newline at/after  */
{ while (off < ed_len && ed_buf[off] != '\n') off++; return off; }
static int line_count(void)
{ int i, n = 1; for (i = 0; i < ed_len; i++) if (ed_buf[i] == '\n') n++; return n; }
static int caret_line(void)
{ int i, ln = 0; for (i = 0; i < ed_caret && i < ed_len; i++) if (ed_buf[i] == '\n') ln++; return ln; }
static int caret_col(void)
{ int ls = line_start(caret_line()); return ed_caret - ls; }

/* ---- editing -------------------------------------------------------------- */
static int has_sel(void) { return ed_sel != ed_caret; }
static void sel_range(int *a, int *b)
{ if (ed_sel < ed_caret) { *a = ed_sel; *b = ed_caret; } else { *a = ed_caret; *b = ed_sel; } }

static void del_range(int a, int b)
{
    if (a < 0) a = 0;
    if (b > ed_len) b = ed_len;
    if (a >= b) return;
    memmove(ed_buf + a, ed_buf + b, (unsigned long)(ed_len - b));
    ed_len -= (b - a);
    ed_caret = ed_sel = a;
    ed_dirty = 1;
}
static void del_sel(void) { if (has_sel()) { int a, b; sel_range(&a, &b); del_range(a, b); } }

static void ins_text(const char *s, int n)
{
    del_sel();
    if (ed_len + n >= ED_CAP) n = ED_CAP - 1 - ed_len;
    if (n <= 0) return;
    memmove(ed_buf + ed_caret + n, ed_buf + ed_caret, (unsigned long)(ed_len - ed_caret));
    memcpy(ed_buf + ed_caret, s, (unsigned long)n);
    ed_len += n; ed_caret += n; ed_sel = ed_caret;
    ed_buf[ed_len] = 0;
    ed_dirty = 1;
}
/* AI code-block insertion (studio_ai.h) */
void studio_insert_text(const char *s, int n) { ins_text(s, n); pc64_shell_dirty(); }

static void ins_ch(char c) { ins_text(&c, 1); }

/* auto-indent: on Enter, copy the leading whitespace of the current line */
static void ins_newline(void)
{
    int ls = line_start(caret_line()), i = ls, ind = 0;
    char pad[40];
    while (i < ed_caret && (ed_buf[i] == ' ' || ed_buf[i] == '\t') && ind < 38)
        pad[ind++] = ed_buf[i++];
    pad[ind] = 0;
    ins_ch('\n');
    if (ind) ins_text(pad, ind);
}

/* ---- caret movement ------------------------------------------------------- */
static void reveal_caret(void)
{
    int ln = caret_line(), col = caret_col();
    int rows = L_edit.h / g_lh, cols;
    if (rows < 1) rows = 1;
    if (ln < ed_scroll) ed_scroll = ln;
    if (ln >= ed_scroll + rows) ed_scroll = ln - rows + 1;
    cols = (L_edit.w - 46) / g_cw; if (cols < 1) cols = 1;
    if (col < ed_hscroll) ed_hscroll = col;
    if (col >= ed_hscroll + cols) ed_hscroll = col - cols + 1;
    if (ed_scroll < 0) ed_scroll = 0;
    if (ed_hscroll < 0) ed_hscroll = 0;
}

static void move_to(int pos, int keep_sel)
{
    if (pos < 0) pos = 0;
    if (pos > ed_len) pos = ed_len;
    ed_caret = pos;
    if (!keep_sel) ed_sel = ed_caret;
    reveal_caret();
}
static void move_updown(int dir, int keep_sel)
{
    int ln = caret_line(), col = caret_col(), nl = ln + dir, ls, le;
    if (nl < 0) nl = 0;
    if (nl >= line_count()) nl = line_count() - 1;
    ls = line_start(nl); le = line_end(ls);
    if (ls + col > le) move_to(le, keep_sel); else move_to(ls + col, keep_sel);
}

/* ---- files ---------------------------------------------------------------- */
static void status_set(const char *a, const char *b);

static void refresh_project(void)
{
    int nv = uno_fs_volumes(), v, i, cnt;
    char nm[32];
    proj_n = 0;
    proj_vol = ed_vol;
    if (proj_vol < 0) {                    /* default: first writable volume */
        for (v = 0; v < nv; v++) if (uno_fs_writable(v)) { proj_vol = v; break; }
        if (proj_vol < 0) proj_vol = 0;
    }
    cnt = uno_fs_list_begin(proj_vol);
    for (i = 0; i < cnt && proj_n < MAXPROJ; i++) {
        if (uno_fs_list_get(proj_vol, i, nm, sizeof nm)) {
            int L = s_len(nm);
            /* show source-ish files: .C .H .PY .MD .TXT .UNO */
            if (L >= 2 && (
                (nm[L-2]=='.' && (nm[L-1]=='C'||nm[L-1]=='H'||nm[L-1]=='c'||nm[L-1]=='h')) ||
                (L>=3 && nm[L-3]=='.' && (nm[L-2]=='M'||nm[L-2]=='m')) ||
                (L>=3 && nm[L-3]=='.' && (nm[L-2]=='P'||nm[L-2]=='p') && (nm[L-1]=='Y'||nm[L-1]=='y')) ||
                (L>=4 && nm[L-4]=='.') ))
                s_cpy(proj_name[proj_n++], nm, 16);
        }
    }
    if (proj_sel >= proj_n) proj_sel = proj_n - 1;
    if (proj_sel < 0) proj_sel = 0;
}

static void doc_load(int vol, const char *name)
{
    long n = uno_fs_read(vol, name, (unsigned char *)ed_buf, ED_CAP - 1);
    if (n < 0) n = 0;
    /* normalize CRLF -> LF */
    { int r = 0, w = 0; for (r = 0; r < n; r++) if (ed_buf[r] != '\r') ed_buf[w++] = ed_buf[r]; n = w; }
    ed_len = (int)n; ed_buf[ed_len] = 0;
    ed_caret = ed_sel = 0; ed_scroll = ed_hscroll = 0; ed_dirty = 0;
    ed_vol = vol; s_cpy(ed_name, name, sizeof ed_name);
    out_n = 0; g_ndiag = 0;
    status_set("Opened", name);
}

static int doc_save(void)
{
    if (!ed_name[0]) { name_mode = 1; name_inlen = 0; name_in[0] = 0;
                       status_set("Save as:", 0); return 0; }
    if (!uno_fs_writable(ed_vol)) { status_set("Read-only volume", 0); return 0; }
    if (uno_fs_write(ed_vol, ed_name, (unsigned char *)ed_buf, ed_len)) {
        ed_dirty = 0; status_set("Saved", ed_name); refresh_project(); return 1;
    }
    status_set("Save failed", ed_name); return 0;
}

/* ---- status line ---------------------------------------------------------- */
static char g_status[96];
static void status_set(const char *a, const char *b)
{
    int i = 0; while (a[i] && i < 90) { g_status[i] = a[i]; i++; }
    if (b) { if (i < 90) g_status[i++] = ' ';
             { int j = 0; while (b[j] && i < 94) g_status[i++] = b[j++]; } }
    g_status[i] = 0;
}

/* ---- build + run ---------------------------------------------------------- */
/* resolve #include "NAME.H": try the doc's volume root, then SDK\ on the
 * doc's volume, then the same two on every other volume (the SDK headers
 * live in SDK\ on the ESP, which is rarely the doc's volume) */
static long inc_reader(void *ctx, const char *name, char *buf, long max)
{
    int nv = uno_fs_volumes(), v;
    char sdk[32];
    long n;
    (void)ctx;
    s_cpy(sdk, "SDK\\", sizeof sdk);
    { int i = 0; while (name[i] && i < 24) { sdk[4 + i] = name[i]; i++; } sdk[4 + i] = 0; }
    if (ed_vol >= 0) {
        n = uno_fs_read(ed_vol, name, (unsigned char *)buf, max); if (n >= 0) return n;
        n = uno_fs_read(ed_vol, sdk,  (unsigned char *)buf, max); if (n >= 0) return n;
    }
    for (v = 0; v < nv; v++) {
        n = uno_fs_read(v, name, (unsigned char *)buf, max); if (n >= 0) return n;
        n = uno_fs_read(v, sdk,  (unsigned char *)buf, max); if (n >= 0) return n;
    }
    return -1;
}

static void out_reset(void) { out_n = 0; }
static void out_add(const char *s)
{
    if (out_n < OUT_ROWS) s_cpy(out_line[out_n++], s, 80);
    else { int i; for (i = 1; i < OUT_ROWS; i++)
             memcpy(out_line[i-1], out_line[i], 80);
           s_cpy(out_line[OUT_ROWS-1], s, 80); }
}

/* NAME.C -> NAME.UNO into the same dir */
static void uno_name(char *dst, const char *src)
{
    int L = s_len(src), i;
    for (i = 0; i < L && src[i] != '.' && i < 10; i++) dst[i] = src[i];
    dst[i] = 0;
    { const char *e = ".UNO"; int j = 0; while (e[j]) dst[i++] = e[j++]; dst[i] = 0; }
}

static int do_build(char *out_uno_name)
{
    long n; int ndiag = 0;
    char line[80], num[16];
    out_reset(); g_ndiag = 0;
    if (!ed_name[0]) { out_add("Save the file first (Ctrl-S)."); return -1; }

    if (studio_is_py(ed_name)) {                  /* Python: wrap source, no ucc */
        int cn = studio_py_pack((const unsigned char *)ed_buf, ed_len,
                                (unsigned char *)g_uno, UNO_CAP);
        if (cn < 0) { out_add("Python source too large for the .UNO buffer."); return -1; }
        uno_name(out_uno_name, ed_name);
        if (!uno_fs_write(ed_vol, out_uno_name, (const unsigned char *)g_uno, cn)) {
            out_add("Saving the .UNO failed (read-only volume?)."); return -1; }
        s_cpy(line, "Packed ", 80);
        { int p = s_len(line), k = 0; while (out_uno_name[k] && p < 60) line[p++] = out_uno_name[k++];
          { const char *sm = " (Python - runs on PYRT.UNO)"; int j = 0; while (sm[j] && p < 78) line[p++] = sm[j++]; }
          line[p] = 0; }
        out_add(line);
        status_set("Packed", out_uno_name);
        refresh_project();
        return 0;
    }

    n = ucc_compile(ed_buf, ed_len, ed_name, inc_reader, 0,
                    g_work, WORK_CAP, g_uno, UNO_CAP, g_diag, 16, &ndiag);
    g_ndiag = ndiag;
    if (n < 0) {
        int i;
        s_cpy(line, "Build failed: ", 80);
        { char c[8]; u_dec(c, ndiag); }
        out_add(line);
        for (i = 0; i < ndiag && i < OUT_ROWS - 1; i++) {
            int p = 0; UccDiag *d = &g_diag[i];
            line[0] = 0;
            if (d->line) { s_cpy(line, d->file[0] ? d->file : ed_name, 80);
                           p = s_len(line); line[p++] = ':';
                           u_dec(num, d->line); { int k=0; while (num[k]&&p<78) line[p++]=num[k++]; }
                           line[p++] = ':'; line[p++] = ' '; line[p] = 0; }
            { int k = 0; while (d->msg[k] && p < 78) line[p++] = d->msg[k++]; line[p] = 0; }
            out_add(line);
        }
        status_set("Build failed", 0);
        return -1;
    }
    uno_name(out_uno_name, ed_name);
    if (!uno_fs_write(ed_vol, out_uno_name, g_uno, n)) {
        out_add("Wrote code, but saving the .UNO failed."); return -1;
    }
    s_cpy(line, "Built ", 80);
    { int p = s_len(line), k = 0; while (out_uno_name[k] && p < 60) line[p++] = out_uno_name[k++];
      line[p++] = ' '; { const char *sm = ucc_summary(); int j = 0; while (sm[j] && p < 78) line[p++] = sm[j++]; }
      line[p] = 0; }
    out_add(line);
    status_set("Build OK", out_uno_name);
    refresh_project();
    return 0;
}

static void do_run(void)
{
    char uno[16];
    if (ed_dirty) doc_save();
    if (do_build(uno) != 0) return;
    if (pc64_shell_run_user(ed_vol, uno) != 0) {
        if (studio_is_py(ed_name)) {              /* Python: distinguish causes */
            const char *err = pc64_shell_py_error();
            if (err && err[0]) {                  /* a compile/exec traceback */
                out_add("Python error:");
                out_add(err);
            } else {
                out_add("Python runtime not installed (APPS\\PYRT.UNO missing).");
            }
        } else {
            out_add("Run failed: the module would not load.");
        }
        status_set("Run failed", 0);
    }
    else
        status_set("Running", uno);
}

/* jump the caret to a build diagnostic's line */
static void jump_to_diag(int i)
{
    int ls;
    if (i < 0 || i >= g_ndiag || !g_diag[i].line) return;
    ls = line_start(g_diag[i].line - 1);
    move_to(ls + (g_diag[i].col > 0 ? g_diag[i].col - 1 : 0), 0);
    g_focus = PANE_EDIT;
}

/* ---- menu ----------------------------------------------------------------- */
static const char *kMenuTitles[] = { "File", "Edit", "Build", "Run", "AI", "Help" };
#define NMENU 6
static const char *kMenuItems[NMENU][6] = {
    { "New", "Save  ^S", "Save As", 0, 0, 0 },
    { "Cut  ^X", "Copy  ^C", "Paste  ^V", "Select All  ^A", 0, 0 },
    { "Build  ^B", 0, 0, 0, 0, 0 },
    { "Run  ^R", 0, 0, 0, 0, 0 },
    { "Ask Assistant", "Settings", 0, 0, 0, 0 },
    { "API Reference", "App Guide", "Language", "IDE Manual", "Contents", 0 },
};
static int menu_nitems(int m)
{ int n = 0; while (n < 6 && kMenuItems[m][n]) n++; return n; }

static void clip_copy(void)
{ int a, b; if (!has_sel()) return; sel_range(&a, &b);
  if (b - a > CLIP_CAP - 1) b = a + CLIP_CAP - 1;
  memcpy(clip, ed_buf + a, (unsigned long)(b - a)); clip_len = b - a; }
static void clip_cut(void)  { clip_copy(); del_sel(); }
static void clip_paste(void){ if (clip_len) ins_text(clip, clip_len); }
static void select_all(void){ ed_sel = 0; ed_caret = ed_len; }

static void help_open(const char *doc) { pc64_browser_open_path(doc); }

static void menu_do(int m, int it)
{
    menu_open = -1;
    switch (m) {
    case 0: /* File */
        if (it == 0) { name_mode = 2; name_inlen = 0; name_in[0] = 0; status_set("New file:", 0); }
        else if (it == 1) doc_save();
        else if (it == 2) { name_mode = 1; name_inlen = 0; name_in[0] = 0; status_set("Save as:", 0); }
        break;
    case 1: /* Edit */
        if (it == 0) clip_cut(); else if (it == 1) clip_copy();
        else if (it == 2) clip_paste(); else if (it == 3) select_all();
        break;
    case 2: { char u[16]; do_build(u); } break;
    case 3: do_run(); break;
    case 4: if (it == 0) { g_focus = PANE_AI; studio_ai_attach_file(ed_name, ed_buf, ed_len); }
            else studio_ai_settings();
            break;
    case 5:
        if (it == 0) help_open("DOCS\\API.MD");
        else if (it == 1) help_open("DOCS\\APPDEV.MD");
        else if (it == 2) help_open("DOCS\\LANG.MD");
        else if (it == 3) help_open("DOCS\\IDE.MD");
        else help_open("DOCS\\INDEX.MD");
        break;
    }
}

/* ---- drawing -------------------------------------------------------------- */
static void fillR(R r, fb_px c) { fb_fill_rect(r.x, r.y, r.w, r.h, c); }

static void draw_menu(const unoui_palette *p)
{
    int x = L_menu.x + 6, i;
    fillR(L_menu, p->title_bg);
    for (i = 0; i < NMENU; i++) {
        int w = fb_text_w(kMenuTitles[i]) + 16;
        if (i == menu_open) fb_fill_rect(x, L_menu.y, w, L_menu.h, p->accent);
        fb_text(x + 8, L_menu.y + 3, kMenuTitles[i],
                i == menu_open ? p->accent_text : p->title_fg, -1);
        x += w;
    }
}

static void draw_dropdown(const unoui_palette *p)
{
    int m = menu_open, x = L_menu.x + 6, i, n, dw = 150, dy = L_menu.y + L_menu.h, dh;
    if (m < 0) return;
    for (i = 0; i < m; i++) x += fb_text_w(kMenuTitles[i]) + 16;
    n = menu_nitems(m); dh = n * (g_lh) + 6;
    fb_fill_rect(x, dy, dw, dh, p->win_bg);
    fb_hline(x, dy, dw, p->dark); fb_hline(x, dy + dh - 1, dw, p->dark);
    fb_vline(x, dy, dh, p->dark); fb_vline(x + dw - 1, dy, dh, p->dark);
    for (i = 0; i < n; i++)
        fb_text(x + 8, dy + 3 + i * g_lh, kMenuItems[m][i], p->text, -1);
}
/* which dropdown item is at (mx,my), or -1 */
static int dropdown_hit(int mx, int my)
{
    int m = menu_open, x = L_menu.x + 6, i, n, dw = 150, dy = L_menu.y + L_menu.h;
    if (m < 0) return -1;
    for (i = 0; i < m; i++) x += fb_text_w(kMenuTitles[i]) + 16;
    n = menu_nitems(m);
    if (mx < x || mx >= x + dw || my < dy || my >= dy + n * g_lh + 6) return -2;
    i = (my - dy - 3) / g_lh;
    return (i >= 0 && i < n) ? i : -1;
}
/* which top-menu title is at mx (on the menu bar), or -1 */
static int menubar_hit(int mx)
{
    int x = L_menu.x + 6, i;
    for (i = 0; i < NMENU; i++) {
        int w = fb_text_w(kMenuTitles[i]) + 16;
        if (mx >= x && mx < x + w) return i;
        x += w;
    }
    return -1;
}

static void draw_project(const unoui_palette *p)
{
    int i, y;
    if (L_proj.w == 0) return;
    fillR(L_proj, p->field_bg);
    fb_vline(L_proj.x + L_proj.w - 1, L_proj.y, L_proj.h, p->dark);
    fb_text(L_proj.x + 6, L_proj.y + 3, "Project", p->text_dim, -1);
    y = L_proj.y + g_lh + 4;
    for (i = proj_scroll; i < proj_n && y < L_proj.y + L_proj.h - g_lh; i++) {
        int sel = (i == proj_sel && g_focus == PANE_PROJ);
        if (sel) fb_fill_rect(L_proj.x, y, L_proj.w - 1, g_lh, p->accent);
        fb_text(L_proj.x + 8, y + 1, proj_name[i], sel ? p->accent_text : p->field_text, -1);
        y += g_lh;
    }
}

static void draw_editor(const unoui_palette *p)
{
    int rows = L_edit.h / g_lh, r, off, ln, in_comment = 0, i;
    int gutw = 44, tx0 = L_edit.x + gutw + 2;
    int lang = studio_is_py(ed_name) ? HL_LANG_PY : HL_LANG_C;
    HlSpan sp[128];
    char numbuf[8];

    fillR(L_edit, p->field_bg);
    fb_fill_rect(L_edit.x, L_edit.y, gutw, L_edit.h, p->win_bg);   /* gutter */
    fb_vline(L_edit.x + gutw, L_edit.y, L_edit.h, p->dark);

    /* compute in-comment state up to the first visible line */
    off = 0; ln = 0;
    while (ln < ed_scroll && off < ed_len) {
        int le = line_end(off);
        int junk = in_comment;
        studio_hl_line_lang(ed_buf + off, le - off, &junk, sp, 128, lang);
        in_comment = junk;
        off = (le < ed_len) ? le + 1 : le; ln++;
    }
    /* draw visible lines */
    for (r = 0; r < rows; r++) {
        int le, ns, k, curline = ed_scroll + r, cn = in_comment;
        int y = L_edit.y + r * g_lh;
        if (off > ed_len) break;
        le = line_end(off);
        /* line number */
        u_dec(numbuf, curline + 1);
        fb_text(L_edit.x + gutw - 6 - fb_text_w(numbuf), y + 1, numbuf, p->text_dim, -1);
        /* tokenize + draw spans (honoring horizontal scroll) */
        ns = studio_hl_line_lang(ed_buf + off, le - off, &cn, sp, 128, lang);
        for (k = 0; k < ns; k++) {
            int cstart = sp[k].start, clen = sp[k].len, c;
            fb_px col = hl_color(sp[k].cls, p);
            for (c = 0; c < clen; c++) {
                int col_i = cstart + c - ed_hscroll;
                if (col_i < 0) continue;
                { int gx = tx0 + col_i * g_cw;
                  if (gx > L_edit.x + L_edit.w - g_cw) break;
                  draw_ch(gx, y, ed_buf[off + cstart + c], col); }
            }
        }
        in_comment = cn;
        off = (le < ed_len) ? le + 1 : le + 1;
        if (le >= ed_len && curline >= line_count() - 1) { /* last line drawn */ }
    }

    /* selection + caret (only if editor focused) */
    {
        int cl = caret_line(), cc = caret_col();
        if (cl >= ed_scroll && cl < ed_scroll + rows) {
            int cx = tx0 + (cc - ed_hscroll) * g_cw;
            int cy = L_edit.y + (cl - ed_scroll) * g_lh;
            if (cx >= tx0 && g_focus == PANE_EDIT)
                fb_vline(cx, cy, g_lh, p->text);
        }
    }
    (void)i;
}

static void draw_output(const unoui_palette *p)
{
    int i, y = L_out.y + 2;
    fb_hline(L_out.x, L_out.y, L_out.w, p->dark);
    fb_fill_rect(L_out.x, L_out.y + 1, L_out.w, L_out.h - 1, p->win_bg);
    for (i = 0; i < out_n; i++) {
        fb_px c = (i < g_ndiag && g_diag[i].line) ? FB_RGB(0xE0, 0x6C, 0x6C) : p->text;
        /* first error rows are clickable jumps: tint them */
        fb_text(L_out.x + 6, y, out_line[i], c, -1);
        y += g_lh;
    }
}

static void draw_status(const unoui_palette *p)
{
    char buf[128]; int px = L_status.x + 6;
    fillR(L_status, p->title_bg_in);
    if (name_mode) {
        s_cpy(buf, name_mode == 2 ? "New: " : "Save as: ", 128);
        { int n = s_len(buf), i = 0; while (name_in[i] && n < 120) buf[n++] = name_in[i++];
          buf[n++] = '_'; buf[n] = 0; }
        fb_text(px, L_status.y + 2, buf, p->title_fg_in, -1);
        return;
    }
    /* left: caret pos + dirty flag */
    { char c1[8], c2[8]; s_cpy(buf, ed_name[0] ? ed_name : "(untitled)", 128);
      { int n = s_len(buf); if (ed_dirty && n < 124) { buf[n++]='*'; buf[n]=0; } }
      u_dec(c1, caret_line() + 1); u_dec(c2, caret_col() + 1);
      { int n = s_len(buf); buf[n++]=' '; buf[n++]='L'; { int i=0; while(c1[i]&&n<120)buf[n++]=c1[i++]; }
        buf[n++]=':'; buf[n++]='C'; { int i=0; while(c2[i]&&n<120)buf[n++]=c2[i++]; } buf[n]=0; }
      fb_text(px, L_status.y + 2, buf, p->title_fg_in, -1); }
    /* right: status message */
    { int w = fb_text_w(g_status); fb_text(L_status.x + L_status.w - w - 8, L_status.y + 2,
        g_status, p->title_fg_in, -1); }
}

/* the whole IDE canvas */
static void studio_draw(struct unoui_widget *w, unoui_rect c, void *ctx)
{
    const struct unoui_theme *t = TH();
    const unoui_palette *p = &t->pal;
    (void)w; (void)ctx;
    layout(c);
    draw_editor(p);
    draw_output(p);
    draw_project(p);
    if (L_ai.w) studio_ai_draw((unoui_rect){ L_ai.x, L_ai.y, L_ai.w, L_ai.h }, t);
    draw_menu(p);
    draw_status(p);
    if (menu_open >= 0) draw_dropdown(p);
}

/* ---- input ---------------------------------------------------------------- */
static void proj_activate(void)
{
    if (proj_sel < 0 || proj_sel >= proj_n) return;
    if (ed_dirty && ed_name[0]) doc_save();
    doc_load(proj_vol, proj_name[proj_sel]);
    g_focus = PANE_EDIT;
}

static int studio_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    (void)w; (void)ctx;

    if (e->kind == UI_EV_MOUSE_DOWN) {
        int mx = e->x, my = e->y;
        /* open dropdown handling first */
        if (menu_open >= 0) {
            int hit = dropdown_hit(mx, my);
            if (hit >= 0) { menu_do(menu_open, hit); pc64_shell_dirty(); return 1; }
            if (hit == -2) { menu_open = -1; /* fall through to other panes */ }
        }
        if (in_rect(L_menu, mx, my)) {
            int m = menubar_hit(mx);
            menu_open = (menu_open == m) ? -1 : m;
            pc64_shell_dirty(); return 1;
        }
        menu_open = -1;
        if (L_ai.w && in_rect((R){L_ai.x,L_ai.y,L_ai.w,L_ai.h}, mx, my)) {
            g_focus = PANE_AI;
            studio_ai_click(mx, my, (unoui_rect){L_ai.x,L_ai.y,L_ai.w,L_ai.h});
            pc64_shell_dirty(); return 1;
        }
        if (L_proj.w && in_rect(L_proj, mx, my)) {
            int row = (my - (L_proj.y + g_lh + 4)) / g_lh + proj_scroll;
            g_focus = PANE_PROJ;
            if (row >= 0 && row < proj_n) {
                if (row == proj_sel) proj_activate(); else proj_sel = row;
            }
            pc64_shell_dirty(); return 1;
        }
        if (in_rect(L_out, mx, my)) {
            int row = (my - (L_out.y + 2)) / g_lh;
            jump_to_diag(row);
            pc64_shell_dirty(); return 1;
        }
        if (in_rect(L_edit, mx, my)) {
            int gutw = 44, col = (mx - (L_edit.x + gutw + 2)) / g_cw + ed_hscroll;
            int row = (my - L_edit.y) / g_lh + ed_scroll, ls, le;
            g_focus = PANE_EDIT;
            if (row < 0) row = 0;
            if (row >= line_count()) row = line_count() - 1;
            ls = line_start(row); le = line_end(ls);
            if (col < 0) col = 0;
            move_to(ls + col > le ? le : ls + col, 0);
            pc64_shell_dirty(); return 1;
        }
        return 1;
    }

    if (e->kind == UI_EV_WHEEL) {
        if (g_focus == PANE_PROJ) { proj_scroll += e->wheel; if (proj_scroll < 0) proj_scroll = 0; }
        else { ed_scroll += e->wheel * 2;
               if (ed_scroll < 0) ed_scroll = 0;
               if (ed_scroll > line_count() - 1) ed_scroll = line_count() - 1; }
        pc64_shell_dirty(); return 1;
    }

    /* filename entry mode swallows typing */
    if (name_mode && e->kind == UI_EV_CHAR) {
        if (name_inlen < 14 && e->ch >= 32 && e->ch < 127)
            { name_in[name_inlen++] = (char)e->ch; name_in[name_inlen] = 0; }
        pc64_shell_dirty(); return 1;
    }
    if (name_mode && e->kind == UI_EV_KEY) {
        if (e->key == UI_KEY_ENTER) {
            if (name_inlen) { s_cpy(ed_name, name_in, sizeof ed_name);
                              if (ed_vol < 0) ed_vol = proj_vol;
                              if (name_mode == 2) { ed_len = 0; ed_buf[0] = 0; ed_caret = ed_sel = 0; }
                              name_mode = 0; doc_save(); }
            else name_mode = 0;
        } else if (e->key == UI_KEY_BACKSPACE) {
            if (name_inlen) name_in[--name_inlen] = 0;
        } else if (e->key == UI_KEY_ESC) name_mode = 0;
        pc64_shell_dirty(); return 1;
    }

    /* AI pane focused: route text/keys there */
    if (g_focus == PANE_AI) {
        if (e->kind == UI_EV_CHAR && studio_ai_char(e->ch)) { pc64_shell_dirty(); return 1; }
        if (e->kind == UI_EV_KEY) {
            if (e->key == UI_KEY_ESC) { g_focus = PANE_EDIT; studio_ai_blur(); pc64_shell_dirty(); return 1; }
            if (studio_ai_key(e->key)) { pc64_shell_dirty(); return 1; }
        }
    }

    if (g_focus == PANE_PROJ && e->kind == UI_EV_KEY) {
        if (e->key == UI_KEY_UP)   { if (proj_sel > 0) proj_sel--; pc64_shell_dirty(); return 1; }
        if (e->key == UI_KEY_DOWN) { if (proj_sel < proj_n-1) proj_sel++; pc64_shell_dirty(); return 1; }
        if (e->key == UI_KEY_ENTER){ proj_activate(); pc64_shell_dirty(); return 1; }
        if (e->key == UI_KEY_TAB)  { g_focus = PANE_EDIT; pc64_shell_dirty(); return 1; }
    }

    /* editor keys */
    if (e->kind == UI_EV_CHAR && g_focus == PANE_EDIT) {
        if (e->ch >= 32 && e->ch < 127) { ins_ch((char)e->ch); reveal_caret(); pc64_shell_dirty(); return 1; }
    }
    if (e->kind == UI_EV_KEY && g_focus == PANE_EDIT) {
        int keep = (e->mods & UI_MOD_SHIFT) != 0;
        switch (e->key) {
        case UI_KEY_LEFT:  move_to(ed_caret - 1, keep); break;
        case UI_KEY_RIGHT: move_to(ed_caret + 1, keep); break;
        case UI_KEY_UP:    move_updown(-1, keep); break;
        case UI_KEY_DOWN:  move_updown(1, keep); break;
        case UI_KEY_HOME:  move_to(line_start(caret_line()), keep); break;
        case UI_KEY_END:   move_to(line_end(line_start(caret_line())), keep); break;
        case UI_KEY_PGUP:  { int r = L_edit.h/g_lh; ed_scroll -= r; if (ed_scroll<0) ed_scroll=0;
                             move_updown(-r, keep); } break;
        case UI_KEY_PGDN:  { int r = L_edit.h/g_lh; move_updown(r, keep); } break;
        case UI_KEY_ENTER: ins_newline(); reveal_caret(); break;
        case UI_KEY_TAB:   ins_text("    ", 4); reveal_caret(); break;
        case UI_KEY_BACKSPACE:
            if (has_sel()) del_sel();
            else if (ed_caret > 0) del_range(ed_caret - 1, ed_caret);
            reveal_caret(); break;
        case UI_KEY_DELETE:
            if (has_sel()) del_sel(); else del_range(ed_caret, ed_caret + 1);
            reveal_caret(); break;
        default: return 1;
        }
        pc64_shell_dirty(); return 1;
    }
    return 1;
}

/* backspace helper fix: the branch above needs a clean single-char delete */
/* (kept inline; del_range already moves the caret to the range start) */

/* ---- accelerators (Ctrl-*, F5/F7) routed by the shell key() hook ---------- */
static int studio_key(int uni, int scan, int ctrl)
{
    (void)scan;
    /* Build/Run are Ctrl-B / Ctrl-R: function keys do not survive
       ExitBootServices (firmware ConIn is gone), so the reliable path is a
       control chord, matching the menu's ^B / ^R labels. */
    if (name_mode) return 0;
    if (ctrl) {
        int c = uni;
        if (c >= 'A' && c <= 'Z') c += 32;
        switch (c) {
        case 's': doc_save(); return 1;
        case 'x': clip_cut(); pc64_shell_dirty(); return 1;
        case 'c': clip_copy(); return 1;
        case 'v': clip_paste(); pc64_shell_dirty(); return 1;
        case 'a': select_all(); pc64_shell_dirty(); return 1;
        case 'b': { char u[16]; do_build(u); pc64_shell_dirty(); return 1; }
        case 'r': do_run(); pc64_shell_dirty(); return 1;
        }
        if (studio_ai_accel(uni, ctrl)) { pc64_shell_dirty(); return 1; }
    }
    return 0;
}

/* ---- lifecycle ------------------------------------------------------------ */
static unoui_canvas g_canvas = { studio_draw, studio_event, 0 };

static void metrics_init(void)
{
    g_mono = pc64_shell_font_mono();
    g_cw   = uno_font_text_w_styled(g_mono, g_px, 0, "M");
    if (g_cw < 4) g_cw = 8;
    g_lh   = uno_font_height_px(g_mono, g_px);
    if (g_lh < 8) g_lh = g_px + 2;
    g_asc  = uno_font_baseline_px(g_mono, g_px);
}

static void studio_build(unoui_window *win)
{
    const struct unoui_theme *t = TH();
    const unoui_metrics *m = &t->m;
    int aw, ah;
    g_win = win;
    metrics_init();
    aw = fb_width() - 80;  if (aw > 1000) aw = 1000; if (aw < 520) aw = 520;
    ah = fb_height() - 90; if (ah > 700)  ah = 700;  if (ah < 340) ah = 340;
    unoui_window_init(win, "Studio", 24, 20,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      ah + m->title_h + 2 * m->pad + m->frame_w);
    unoui_add_canvas(win, 0, 0, aw, ah, &g_canvas);
    win->flags |= UI_WIN_RESIZE;
}

static int studio_action(const unoui_action *a) { (void)a; return 0; }

static void studio_opened(void)
{
    if (!ed_buf)  ed_buf  = (char *)malloc(ED_CAP);
    if (!g_work)  g_work  = malloc(WORK_CAP);
    if (!g_uno)   g_uno   = (unsigned char *)malloc(UNO_CAP);
    metrics_init();
    refresh_project();
    if (ed_buf && !ed_len && !ed_name[0]) {
        /* first open: greet with an SDK sample, found on whichever volume
         * carries the ESP (search them all - it is rarely volume 0).  Python
         * is a first-class app language here, so lead with SAMPLE.PY when it
         * ships, falling back to the UnoC SAMPLE.C. */
        static const char *const greet[2][2] = {
            { "SDK\\SAMPLE.PY", "SAMPLE.PY" },
            { "SDK\\SAMPLE.C",  "SAMPLE.C"  } };
        int nv = uno_fs_volumes(), v, g;
        long n = -1;
        for (g = 0; g < 2 && n < 0; g++)
            for (v = 0; v < nv && n < 0; v++)
                if ((n = uno_fs_read(v, greet[g][0], (unsigned char *)ed_buf, ED_CAP - 1)) > 0) {
                    int r, w = 0;
                    for (r = 0; r < n; r++) if (ed_buf[r] != '\r') ed_buf[w++] = ed_buf[r];
                    ed_len = w; ed_buf[ed_len] = 0;
                    s_cpy(ed_name, greet[g][1], sizeof ed_name);
                    ed_vol = v;
                }
        if (n <= 0) ed_buf[0] = 0;
    }
    studio_ai_init();
    status_set("^B Build   ^R Run   AI menu for the assistant", 0);
}

static void studio_closed(void) { menu_open = -1; }
static int  studio_canvas_index(void) { return 0; }
static void studio_frame(void) { studio_ai_frame(); }

static const UnoUuiApp kStudio = {
    UNO_UUIAPP_ABI, "Studio",
    studio_build, studio_action, studio_key, studio_frame,
    studio_opened, studio_closed, studio_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved) { (void)reserved; return &kStudio; }
