/* ===========================================================================
 * UnoDOS/pc64 - UnoTransfer, the windowed app.
 *
 * A native canvas, like the browser and the SSH client, and the third consumer
 * of unoxfer - the other two being the `xfer` URC verb and the terminal.  None
 * of them is privileged, which is why the core is headless.
 *
 * FOUR TABS, which is Portage's shell with the parts that need a multi-window
 * desktop taken out (workspaces, tab groups, per-window themes - see
 * UNOXFER.md, "what was left out"):
 *
 *   Sites     the saved connections, and the editor that creates one
 *   Transfer  the dual pane.  Left is a local volume, right is the site.
 *   Queue     every job, with progress - the same jobs `xfer status` reports,
 *             because there is one engine and not an app copy of it
 *   Terminal  a real VT screen (unoterm) over an SSH shell channel
 *
 * NOTHING HERE BLOCKS.  The listing calls are bounded round trips; the
 * transfers are unoxfer jobs advanced by unoxfer_job_tick() on the shell's
 * frame loop, so a stalled server costs this app a frame and the desktop
 * nothing.  That is the same argument sshapp_ui.c makes about ssh_poll, and it
 * is the reason the engine is a step machine rather than a loop.
 *
 * THE DUAL PANE IS SYMMETRIC ON PURPOSE.  Both sides are the same browser over
 * the same unoxfer_client seam - the left one just happens to hold a LOCAL
 * client.  Portage found the same thing: once "local" is a protocol, a
 * local->remote copy, a remote->local copy and a local->local copy are one
 * code path instead of three.
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_font.h"
#include "pc64_fs.h"
#include "unoxfer.h"
#include "unoterm.h"
#include "unossh.h"

int  pc64_shell_font_mono(void);
void pc64_shell_dirty(void);
const struct unoui_theme *pc64_shell_theme(void);

static const unoui_theme *TH(void) { return pc64_shell_theme(); }

#define NTABS      4
#define T_SITES    0
#define T_XFER     1
#define T_QUEUE    2
#define T_TERM     3

#define PANE_MAX   128
#define TERM_COLS  100
#define TERM_ROWS  32
#define TERM_SB    200

/* ---- a browsable pane ----------------------------------------------------
 * One of these per side.  It owns its client, its path and its listing; the
 * two sides differ only in which site they were opened from. */
typedef struct {
    unoxfer_client *c;
    unoxfer_site    site;
    char            path[UNOXFER_PATHLEN];
    unoxfer_ent     ent[PANE_MAX];
    const char     *row[PANE_MAX];
    char            label[PANE_MAX][80];
    int             n, total, sel, top;
    int             live;
} pane;

static pane g_left, g_right;
static int  g_focus;                     /* 0 = left, 1 = right              */

static int   g_tab = T_SITES;
static const char *g_lbl[NTABS] = { "Sites", "Transfer", "Queue", "Terminal" };
static int   g_first;
static unoui_rect g_rect;

static char  g_status[192];
static int   g_alarm;

/* the site list */
static char  g_sname[UNOXFER_MAXSITE][UNOXFER_NAMELEN];
static char  g_srow[UNOXFER_MAXSITE][96];
static const char *g_slist[UNOXFER_MAXSITE];
static int   g_nsite, g_ssel, g_stop;

/* the queue */
static char  g_qrow[UNOXFER_MAXJOB][128];
static const char *g_qlist[UNOXFER_MAXJOB];
static int   g_nq, g_qsel;

/* the terminal */
static unoterm      g_term;
static unsigned char g_termmem[
    /* one screen, one alternate screen, and the scrollback ring.  Sized here
     * rather than malloc'd, because an app whose terminal fails to open when
     * the heap is busy is an app whose terminal fails when you need it. */
    (TERM_COLS * TERM_ROWS * 2 + TERM_COLS * TERM_SB) * 16 + 4096];
static int  g_term_ready, g_term_conn = -1;

/* ---- tiny helpers -------------------------------------------------------- */
static int  slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void put(char *d, int cap, const char *s)
{ int i = 0; if (cap <= 0) return; if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }
static void cat(char *d, int cap, const char *s)
{ int n = slen(d); put(d + n, cap - n, s); }

static void catn(char *d, int cap, long long v)
{
    char t[24];
    int i = 0, j, n = slen(d);
    if (v < 0) { cat(d, cap, "-"); v = -v; n++; }
    if (!v) { cat(d, cap, "0"); return; }
    while (v && i < 23) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (j = i - 1; j >= 0 && n < cap - 1; j--) d[n++] = t[j];
    d[n] = 0;
}

static void say(const char *a, const char *b, int alarm)
{
    g_status[0] = 0;
    cat(g_status, (int)sizeof g_status, a);
    if (b) cat(g_status, (int)sizeof g_status, b);
    g_alarm = alarm;
    pc64_shell_dirty();
}

/* ---- the site list ------------------------------------------------------- */
static void reload_sites(void)
{
    unoxfer_site s;
    g_nsite = 0;
    while (g_nsite < UNOXFER_MAXSITE) {
        if (unoxfer_site_list(g_nsite, g_sname[g_nsite], UNOXFER_NAMELEN) < 0) break;
        g_srow[g_nsite][0] = 0;
        put(g_srow[g_nsite], 96, g_sname[g_nsite]);
        if (unoxfer_site_get(g_sname[g_nsite], &s) == UNOXFER_OK) {
            cat(g_srow[g_nsite], 96, "   ");
            cat(g_srow[g_nsite], 96, unoxfer_proto_name(s.proto));
            cat(g_srow[g_nsite], 96, "  ");
            if (s.user[0]) { cat(g_srow[g_nsite], 96, s.user); cat(g_srow[g_nsite], 96, "@"); }
            cat(g_srow[g_nsite], 96, s.host);
            /* A protocol whose backend is not usable in this build is marked
             * HERE, in the list, not discovered at connect: SFTP is registered
             * and waiting on unossh's subsystem channel, and a row that looks
             * connectable and is not wastes somebody's afternoon. */
            if (!unoxfer_proto_ready(s.proto)) cat(g_srow[g_nsite], 96, "   [unavailable]");
        }
        g_slist[g_nsite] = g_srow[g_nsite];
        g_nsite++;
    }
    if (g_ssel >= g_nsite) g_ssel = g_nsite ? g_nsite - 1 : 0;
}

/* ---- pane plumbing ------------------------------------------------------- */
static void pane_relabel(pane *p)
{
    int i;
    for (i = 0; i < p->n; i++) {
        p->label[i][0] = 0;
        put(p->label[i], 80, p->ent[i].is_dir ? "[" : " ");
        cat(p->label[i], 80, p->ent[i].name);
        if (p->ent[i].is_dir) cat(p->label[i], 80, "]");
        else {
            cat(p->label[i], 80, "   ");
            catn(p->label[i], 80, (long long)p->ent[i].size);
        }
        p->row[i] = p->label[i];
    }
}

static void pane_refresh(pane *p)
{
    p->n = 0; p->total = 0;
    if (!p->c) return;
    p->n = unoxfer_list(p->c, p->path, p->ent, PANE_MAX, &p->total);
    if (p->n < 0) { p->n = 0; say("List failed: ", unoxfer_error(p->c), 1); return; }
    if (p->total > p->n)
        say("Listing truncated - this pane shows the first 128 entries.", 0, 1);
    if (p->sel >= p->n) p->sel = p->n ? p->n - 1 : 0;
    pane_relabel(p);
    pc64_shell_dirty();
}

static void pane_close(pane *p)
{
    if (p->c) { unoxfer_close(p->c); p->c = 0; }
    p->live = 0; p->n = 0; p->sel = p->top = 0;
}

static int pane_open(pane *p, const unoxfer_site *s)
{
    char err[160];
    pane_close(p);
    p->site = *s;
    p->c = unoxfer_open(s, err, (int)sizeof err);
    if (!p->c) { say("", err, 1); return 0; }
    p->live = 1;
    put(p->path, UNOXFER_PATHLEN, s->root[0] ? s->root : "/");
    if (s->proto == UNOXFER_LOCAL) put(p->path, UNOXFER_PATHLEN, "");
    pane_refresh(p);
    return 1;
}

/* The left pane is always a local volume, and it opens itself at start on
 * uno_fs_pref_vol() - the one place in the tree that knows which volume is
 * actually worth persisting to (pc64_fs.h).  Guessing "volume 0" here would
 * open the RAM disk, which is empty on most boots and looks like a bug. */
static void open_local(int vol)
{
    unoxfer_site s;
    int i;
    for (i = 0; i < (int)sizeof s; i++) ((char *)&s)[i] = 0;
    s.proto = UNOXFER_LOCAL;
    s.vol = vol;
    put(s.name, UNOXFER_NAMELEN, "local");
    put(s.host, UNOXFER_HOSTLEN, uno_fs_volume_name(vol));
    if (pane_open(&g_left, &s)) {
        say("Local volume ", uno_fs_volume_name(vol), 0);
    }
}

/* ---- navigation ---------------------------------------------------------- */
static void pane_up(pane *p)
{
    int i, cut = -1;
    char sep = p->site.proto == UNOXFER_LOCAL ? '\\' : '/';
    for (i = 0; p->path[i]; i++) if (p->path[i] == sep) cut = i;
    if (cut <= 0) p->path[0] = 0;
    else p->path[cut] = 0;
    if (p->site.proto != UNOXFER_LOCAL && !p->path[0]) { p->path[0] = '/'; p->path[1] = 0; }
    p->sel = p->top = 0;
    pane_refresh(p);
}

static void pane_enter(pane *p)
{
    char next[UNOXFER_PATHLEN];
    if (p->sel < 0 || p->sel >= p->n) return;
    if (!p->ent[p->sel].is_dir) return;
    if (p->site.proto == UNOXFER_LOCAL)
        unoxfer_ljoin(next, (int)sizeof next, p->path, p->ent[p->sel].name);
    else
        unoxfer_rjoin(next, (int)sizeof next, p->path, p->ent[p->sel].name);
    put(p->path, UNOXFER_PATHLEN, next);
    p->sel = p->top = 0;
    pane_refresh(p);
}

/* ---- starting a transfer -------------------------------------------------
 * The pane the SELECTION is in decides the direction, which is the rule every
 * two-pane transfer tool has settled on and the only one that does not need a
 * separate "which way?" prompt. */
static void start_transfer(int recurse)
{
    pane *src = g_focus ? &g_right : &g_left;
    pane *dst = g_focus ? &g_left  : &g_right;
    char rp[UNOXFER_PATHLEN], lp[UNOXFER_PATHLEN];
    char fat[16];
    int id;

    if (!src->live || !dst->live) { say("Both panes need a connection first.", 0, 1); return; }
    if (src->sel < 0 || src->sel >= src->n) { say("Nothing selected.", 0, 1); return; }
    if (src->ent[src->sel].is_dir && !recurse) {
        say("That is a directory - press R to copy it recursively.", 0, 1);
        return;
    }

    if (src->site.proto == UNOXFER_LOCAL) {
        /* push: local -> remote */
        unoxfer_ljoin(lp, (int)sizeof lp, src->path, src->ent[src->sel].name);
        unoxfer_rjoin(rp, (int)sizeof rp, dst->path, src->ent[src->sel].name);
        id = unoxfer_job_push(&dst->site, src->site.vol, lp, rp, recurse);
    } else {
        /* pull: remote -> local.  The landing name is mapped onto something
         * FAT will take, and the mapping is SHOWN, because silently landing
         * MYLONG~1.TXT and saying nothing is how a copy appears to have
         * worked and did not. */
        unoxfer_rjoin(rp, (int)sizeof rp, src->path, src->ent[src->sel].name);
        unoxfer_fatname(fat, (int)sizeof fat, src->ent[src->sel].name);
        unoxfer_ljoin(lp, (int)sizeof lp, dst->path, fat);
        id = unoxfer_job_pull(&src->site, rp, dst->site.vol, lp, recurse);
    }
    if (id < 0) { say("Could not start: ", unoxfer_strerror(id), 1); return; }
    g_status[0] = 0;
    cat(g_status, (int)sizeof g_status, recurse ? "Queued (recursive) as job " : "Queued as job ");
    catn(g_status, (int)sizeof g_status, id);
    cat(g_status, (int)sizeof g_status, " - see the Queue tab");
    g_alarm = 0;
    g_tab = T_QUEUE;
    pc64_shell_dirty();
}

/* ---- the queue ----------------------------------------------------------- */
static void reload_queue(void)
{
    unoxfer_job_stat st;
    int i;
    g_nq = 0;
    for (i = 0; i < UNOXFER_MAXJOB; i++) {
        char *r;
        if (unoxfer_job_status(i, &st) != UNOXFER_OK) continue;
        r = g_qrow[g_nq];
        r[0] = 0;
        cat(r, 128, "#"); catn(r, 128, i); cat(r, 128, "  ");
        switch (st.state) {
        case UNOXFER_JOB_PLANNING:  cat(r, 128, "planning "); break;
        case UNOXFER_JOB_RUNNING:   cat(r, 128, "running  "); break;
        case UNOXFER_JOB_DONE:      cat(r, 128, "done     "); break;
        case UNOXFER_JOB_FAILED:    cat(r, 128, "FAILED   "); break;
        case UNOXFER_JOB_CANCELLED: cat(r, 128, "cancelled"); break;
        default:                    cat(r, 128, "idle     "); break;
        }
        catn(r, 128, st.files_done); cat(r, 128, "/"); catn(r, 128, st.files_total);
        cat(r, 128, " files  ");
        if (st.bytes_total) {
            catn(r, 128, (long long)((st.bytes_done * 100ull) / st.bytes_total));
            cat(r, 128, "%  ");
        }
        if (st.state == UNOXFER_JOB_RUNNING && st.cur[0]) cat(r, 128, st.cur);
        else if (st.msg[0]) cat(r, 128, st.msg);
        g_qlist[g_nq] = r;
        g_nq++;
    }
    if (g_qsel >= g_nq) g_qsel = g_nq ? g_nq - 1 : 0;
}

/* ---- the terminal --------------------------------------------------------
 * Opened against whichever site the RIGHT pane is connected to, so "browse a
 * box and get a shell on it" is one connection's worth of thinking rather than
 * two.  Only the SSH-based protocols can host one; unoxfer_proto_terminal()
 * answers that, and the tab says so rather than opening an empty screen. */
static void term_ensure(void)
{
    if (g_term_ready) return;
    g_term_ready = unoterm_init(&g_term, g_termmem, sizeof g_termmem,
                                TERM_COLS, TERM_ROWS, TERM_SB);
}

static void term_open(void)
{
    unsigned char seed[32];
    const unoxfer_site *s = &g_right.site;
    int h, i;

    term_ensure();
    if (!g_term_ready) { say("The terminal could not initialise.", 0, 1); return; }
    if (g_term_conn >= 0) { say("Already connected.", 0, 0); return; }
    if (!g_right.live || !unoxfer_proto_terminal(s->proto)) {
        say("Connect the right pane to an SSH site first.", 0, 1);
        return;
    }
    if (!s->key[0]) { say("That site names no key (see the SSH app).", 0, 1); return; }
    if (ssh_key_load(s->key, "", seed) != 0) {
        say("Key needs a passphrase: ", s->key, 1); return;
    }
    h = ssh_connect(s->host, s->port ? s->port : 22);
    if (h < 0) { say("Connect failed.", 0, 1); return; }
    if (ssh_handshake(h) != 0) { say("Handshake failed: ", ssh_error(h), 1); ssh_close(h); return; }
    if (ssh_verify_host(h, s->host) == SSH_HOST_MISMATCH) {
        say("HOST KEY CHANGED for ", s->host, 1);
        ssh_close(h);
        return;
    }
    if (ssh_auth_key(h, s->user[0] ? s->user : "root", seed) != 0) {
        say("Login refused: ", ssh_error(h), 1);
        for (i = 0; i < 32; i++) seed[i] = 0;
        ssh_close(h);
        return;
    }
    for (i = 0; i < 32; i++) seed[i] = 0;
    if (ssh_shell(h) != 0) { say("No shell: ", ssh_error(h), 1); ssh_close(h); return; }
    g_term_conn = h;
    say("Shell on ", s->host, 0);
}

/* One non-blocking sweep, from the draw path - the same argument sshapp_ui.c
 * makes.  Read AFTER every poll and in small bites: unossh's channel ring is
 * 16 KB and silently drops the overflow, so keeping it shallow is the only
 * mitigation available from this side (filed against unossh). */
static void term_pump(void)
{
    unsigned char chunk[1024];
    int n, guard;
    if (g_term_conn < 0) return;
    for (guard = 0; guard < 16; guard++) {
        ssh_poll(g_term_conn);
        n = ssh_read(g_term_conn, chunk, (int)sizeof chunk);
        if (n > 0) { unoterm_feed(&g_term, chunk, n); pc64_shell_dirty(); continue; }
        if (n < 0) {
            ssh_close(g_term_conn);
            g_term_conn = -1;
            unoterm_feed(&g_term, (const unsigned char *)"\r\n[connection closed]\r\n", 23);
            pc64_shell_dirty();
        }
        break;
    }
}

/* ---- painting ------------------------------------------------------------ */
static int band_tabs_h(void) { return unoui_tabs_h(TH()); }
static int band_stat_h(void) { return fb_text_h() + 6; }

static void draw_list(unoui_rect r, const char **items, int n, int sel, int top,
                      const char *empty, int focused)
{
    if (n) unoui_list_draw(TH(), r, items, n, sel, top);
    else fb_text(r.x + 6, r.y + 6, empty, TH()->pal.text_dim, -1);
    if (focused) {
        /* A two-pane app must show which pane the keys go to.  Without this
         * every arrow key is a guess, and the first wrong guess is a file
         * copied the wrong way. */
        fb_frame_rect(r.x, r.y, r.w, r.h, TH()->pal.accent);
    }
}

static void draw_panes(unoui_rect body)
{
    unoui_rect a = body, b = body;
    int half = body.w / 2, hdr = fb_text_h() + 4;
    a.w = half - 2;
    b.x = body.x + half + 2;
    b.w = body.w - half - 2;

    fb_text(a.x + 4, a.y + 2, g_left.live ? (g_left.path[0] ? g_left.path : "\\")
                                          : "(no volume)", TH()->pal.text, -1);
    fb_text(b.x + 4, b.y + 2, g_right.live ? (g_right.path[0] ? g_right.path : "/")
                                           : "(not connected - pick a site)",
            TH()->pal.text, -1);
    a.y += hdr; a.h -= hdr;
    b.y += hdr; b.h -= hdr;
    draw_list(a, g_left.row,  g_left.n,  g_left.sel,  g_left.top,  "(empty)", g_focus == 0);
    draw_list(b, g_right.row, g_right.n, g_right.sel, g_right.top, "(empty)", g_focus == 1);
}

/* Map a cell's colour onto the framebuffer.  The DEFAULT colour deliberately
 * follows the THEME rather than being hardcoded black-on-white: a terminal
 * that ignores the desktop's palette is the one window that looks wrong in
 * every theme. */
static unsigned cell_color(int c, unsigned dflt)
{
    static const unsigned kAnsi[16] = {
        FB_RGB(0,0,0),       FB_RGB(178,24,44),   FB_RGB(28,152,68),  FB_RGB(180,142,20),
        FB_RGB(36,88,196),   FB_RGB(150,60,170),  FB_RGB(20,150,160), FB_RGB(200,200,200),
        FB_RGB(96,96,96),    FB_RGB(240,80,90),   FB_RGB(70,220,110), FB_RGB(240,210,70),
        FB_RGB(90,150,255),  FB_RGB(210,120,235), FB_RGB(80,220,230), FB_RGB(255,255,255)
    };
    if (c == UNOTERM_DEFAULT_COLOR) return dflt;
    if (c & UNOTERM_RGB_FLAG)
        return FB_RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
    if (c < 16) return kAnsi[c];
    /* The 6x6x6 cube and the grey ramp, computed rather than tabled: 240 more
     * table entries to save two divides is not a trade worth making here. */
    if (c < 232) {
        int v = c - 16, r = v / 36, g = (v / 6) % 6, bl = v % 6;
        return FB_RGB(r ? 55 + r * 40 : 0, g ? 55 + g * 40 : 0, bl ? 55 + bl * 40 : 0);
    }
    { int g = 8 + (c - 232) * 10; return FB_RGB(g, g, g); }
}

static void draw_term(unoui_rect body)
{
    int mono = pc64_shell_font_mono();
    int cw, lh, x, y;
    unsigned dfg, dbg;

    term_ensure();
    fb_fill_rect(body.x, body.y, body.w, body.h, TH()->pal.field_bg);
    if (!g_term_ready) {
        fb_text(body.x + 6, body.y + 6, "terminal unavailable", TH()->pal.text_dim, -1);
        return;
    }
    if (g_term_conn < 0 && !unoterm_scrollback_count(&g_term))
        fb_text(body.x + 6, body.y + 6,
                "Press C to open a shell on the right pane's site.",
                TH()->pal.text_dim, -1);

    if (mono >= 0 && unoui_font_push) unoui_font_push(mono);
    cw = fb_text_w("M");
    lh = fb_text_h() + 1;
    if (cw < 1) cw = 8;
    if (lh < 1) lh = 10;
    dfg = TH()->pal.field_text;
    dbg = TH()->pal.field_bg;

    /* Only as many rows and columns as actually FIT are drawn, and the
     * emulator is resized to match, so what is on the screen is what the far
     * end thinks is on the screen.  A terminal that renders 100 columns into a
     * 60-column window is one whose line wrapping disagrees with the shell's. */
    {
        int cols = (body.w - 8) / cw, rows = (body.h - 4) / lh;
        if (cols > TERM_COLS) cols = TERM_COLS;
        if (rows > TERM_ROWS) rows = TERM_ROWS;
        if (cols > 0 && rows > 0 && (cols != g_term.cols || rows != g_term.rows))
            unoterm_resize(&g_term, cols, rows);
    }

    for (y = 0; y < g_term.rows; y++) {
        int py = body.y + 2 + y * lh;
        if (py + lh > body.y + body.h) break;
        for (x = 0; x < g_term.cols; x++) {
            const unoterm_cell *c = unoterm_cell_at(&g_term, x, y);
            unsigned fg = cell_color(c->fg, dfg), bg = cell_color(c->bg, dbg);
            char s[2];
            int px = body.x + 4 + x * cw;
            if (c->attr & UNOTERM_INVERSE) { unsigned t = fg; fg = bg; bg = t; }
            if (bg != dbg) fb_fill_rect(px, py, cw, lh, bg);
            if (c->ch == ' ' || !c->ch) continue;
            s[0] = (char)(c->ch < 0x80 ? c->ch : '?');
            s[1] = 0;
            fb_text(px, py, s, fg, -1);
        }
    }
    if (g_term.cursor_visible && g_term_conn >= 0) {
        int px = body.x + 4 + g_term.cx * cw, py = body.y + 2 + g_term.cy * lh;
        if (py + lh <= body.y + body.h) fb_frame_rect(px, py, cw, lh, TH()->pal.accent);
    }
    if (mono >= 0 && unoui_font_pop) unoui_font_pop();
}

static void xfer_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    unoui_tabs_model m;
    unoui_rect strip, body, stat;
    (void)w; (void)ctx;
    g_rect = r;

    if (!g_status[0])
        say("Tab switches pane   Enter opens   Backspace up   Space copies   "
            "R copies a folder", 0, 0);
    if (!g_left.live) open_local(uno_fs_pref_vol());

    /* The engine is ticked by the shell's frame loop, not from here - so a job
     * keeps moving whether or not this window is open.  All this does is pump
     * the terminal and refresh what it shows. */
    term_pump();

    strip = r; strip.h = band_tabs_h();
    stat = r; stat.y = r.y + r.h - band_stat_h(); stat.h = band_stat_h();
    body = r; body.y = strip.y + strip.h;
    body.h = r.h - strip.h - stat.h;
    if (body.h < 20) body.h = 20;

    m.labels = g_lbl; m.n = NTABS; m.sel = g_tab; m.first = g_first;
    m.hot = -1; m.hot_part = UI_TAB_NONE;
    m.flags = UI_TF_ELASTIC;
    g_first = unoui_tabs_reveal(TH(), strip, &m, m.sel);
    m.first = g_first;
    unoui_tabs_draw(TH(), strip, &m);

    switch (g_tab) {
    case T_SITES:
        reload_sites();
        draw_list(body, g_slist, g_nsite, g_ssel, g_stop,
                  "no sites yet - press N to add one", 1);
        break;
    case T_XFER:  draw_panes(body); break;
    case T_QUEUE:
        reload_queue();
        draw_list(body, g_qlist, g_nq, g_qsel, 0, "no transfers yet", 1);
        break;
    default:      draw_term(body); break;
    }

    fb_fill_rect(stat.x, stat.y, stat.w, stat.h, TH()->pal.face);
    fb_hline(stat.x, stat.y, stat.w, TH()->pal.shadow);
    fb_text(stat.x + 6, stat.y + 3, g_status,
            g_alarm ? FB_RGB(190, 40, 40) : TH()->pal.text, -1);
}

/* ---- input --------------------------------------------------------------- */
static void connect_selected_site(void)
{
    unoxfer_site s;
    reload_sites();
    if (!g_nsite) { say("No sites saved. Use `xfer site` over URC, or press N.", 0, 1); return; }
    if (unoxfer_site_get(g_sname[g_ssel], &s) != UNOXFER_OK) { say("Cannot read that site.", 0, 1); return; }
    if (!unoxfer_proto_ready(s.proto)) {
        say("Not available in this build: ", unoxfer_proto_name(s.proto), 1);
        return;
    }
    say("Connecting to ", s.host, 0);
    if (pane_open(&g_right, &s)) {
        g_tab = T_XFER;
        g_focus = 1;
        say("Connected to ", s.host, 0);
    }
}

/* The terminal owns EVERY key while it is focused and connected, including the
 * ones that are app commands elsewhere.  A terminal you cannot type 'q' into
 * is not a terminal - which is exactly the mistake the SSH app's status-band
 * editor had to work around. */
static int term_key(const unoui_event *e)
{
    char b[16];
    int n = 0, key = 0;
    if (g_term_conn < 0) return 0;
    if (e->kind == UI_EV_CHAR) key = e->ch;
    else if (e->kind == UI_EV_KEY) {
        switch (e->key) {
        case UI_KEY_UP:    key = UNOTERM_K_UP;    break;
        case UI_KEY_DOWN:  key = UNOTERM_K_DOWN;  break;
        case UI_KEY_LEFT:  key = UNOTERM_K_LEFT;  break;
        case UI_KEY_RIGHT: key = UNOTERM_K_RIGHT; break;
        case UI_KEY_HOME:  key = UNOTERM_K_HOME;  break;
        case UI_KEY_END:   key = UNOTERM_K_END;   break;
        case UI_KEY_PGUP:  key = UNOTERM_K_PGUP;  break;
        case UI_KEY_PGDN:  key = UNOTERM_K_PGDN;  break;
        case UI_KEY_DELETE: key = UNOTERM_K_DEL;  break;
        case UI_KEY_ENTER: key = '\r'; break;
        case UI_KEY_TAB:   key = '\t'; break;
        case UI_KEY_ESC:   key = 27;   break;
        case UI_KEY_BACKSPACE: key = 127; break;
        default: return 0;
        }
    } else return 0;
    n = unoterm_key(&g_term, key, 0, 0, b, (int)sizeof b);
    if (n > 0) ssh_write(g_term_conn, b, n);
    return 1;
}

static void move_sel(int *sel, int n, int d)
{
    if (!n) return;
    *sel += d;
    if (*sel < 0) *sel = 0;
    if (*sel >= n) *sel = n - 1;
}

static int xfer_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    pane *p = g_focus ? &g_right : &g_left;
    (void)w; (void)ctx;

    if (g_tab == T_TERM && term_key(e)) { pc64_shell_dirty(); return 1; }

    if (e->kind == UI_EV_CHAR) {
        switch (e->ch) {
        case '1': g_tab = T_SITES; return 1;
        case '2': g_tab = T_XFER;  return 1;
        case '3': g_tab = T_QUEUE; return 1;
        case '4': g_tab = T_TERM;  return 1;
        case 'c': case 'C':
            if (g_tab == T_TERM) { term_open(); return 1; }
            connect_selected_site();
            return 1;
        case ' ':
            if (g_tab == T_XFER) { start_transfer(0); return 1; }
            break;
        case 'r': case 'R':
            if (g_tab == T_XFER) { start_transfer(1); return 1; }
            if (g_tab == T_SITES) { reload_sites(); return 1; }
            break;
        case 'v': case 'V':
            /* Cycle the LOCAL pane through the volumes.  A transfer app whose
             * local side is stuck on one volume cannot put anything on the
             * stick you actually want it on. */
            if (g_left.live) {
                int nv = uno_fs_volumes();
                open_local((g_left.site.vol + 1) % (nv > 0 ? nv : 1));
            }
            return 1;
        case 'x': case 'X':
            if (g_tab == T_QUEUE && g_nq) {
                reload_queue();
                unoxfer_job_cancel(g_qsel);
                say("Cancelled.", 0, 0);
            }
            return 1;
        case 8:                                   /* backspace as a character */
            if (g_tab == T_XFER && p->live) { pane_up(p); return 1; }
            break;
        default: break;
        }
    }

    if (e->kind == UI_EV_KEY) {
        switch (e->key) {
        case UI_KEY_TAB:
            if (g_tab == T_XFER) { g_focus = !g_focus; pc64_shell_dirty(); return 1; }
            break;
        case UI_KEY_BACKSPACE:
            if (g_tab == T_XFER && p->live) { pane_up(p); return 1; }
            break;
        case UI_KEY_ENTER:
            if (g_tab == T_XFER && p->live) { pane_enter(p); return 1; }
            if (g_tab == T_SITES) { connect_selected_site(); return 1; }
            break;
        case UI_KEY_UP: case UI_KEY_DOWN: {
            int d = e->key == UI_KEY_DOWN ? 1 : -1;
            if (g_tab == T_XFER)       move_sel(&p->sel, p->n, d);
            else if (g_tab == T_SITES) move_sel(&g_ssel, g_nsite, d);
            else if (g_tab == T_QUEUE) move_sel(&g_qsel, g_nq, d);
            pc64_shell_dirty();
            return 1;
        }
        default: break;
        }
    }
    return 0;
}

/* ---- the shell's handle -------------------------------------------------- */
static unoui_canvas g_canvas = { xfer_draw, xfer_event, 0 };

unoui_canvas *pc64_xferapp_canvas(void) { return &g_canvas; }

void pc64_xferapp_open(void)
{
    reload_sites();
    if (!g_left.live) open_local(uno_fs_pref_vol());
    pc64_shell_dirty();
}

/* ---- the debug seam the QEMU gate drives --------------------------------
 * The gate drives the SAME functions a keypress does, rather than a parallel
 * "test" path - a harness that exercises different code proves the harness
 * works.  Weak-stubbed away in a production build by the linker, since nothing
 * in a shipped image calls them. */
int  pc64_xferapp_dbg_tab(void)         { return g_tab; }
void pc64_xferapp_dbg_settab(int t)     { if (t >= 0 && t < NTABS) g_tab = t; }
int  pc64_xferapp_dbg_sites(void)       { reload_sites(); return g_nsite; }
int  pc64_xferapp_dbg_connect(int idx)  { g_ssel = idx; connect_selected_site();
                                          return g_right.live; }
int  pc64_xferapp_dbg_entries(int side) { return side ? g_right.n : g_left.n; }
const char *pc64_xferapp_dbg_status(void) { return g_status; }
