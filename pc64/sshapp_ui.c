/* ===========================================================================
 * UnoDOS/pc64 - the SSH client app.
 *
 * A native windowed canvas, like the browser, and the second consumer of
 * unossh - the first being the automation verb. Neither is privileged, which
 * is why the core is headless: the verb has to work on a box with no desktop.
 *
 * It is the joining point of the whole programme. The tab strip is the control
 * tabs-a built and tabs-c proved a canvas can host; the Manage tab's two panes
 * are tabs-b's MDI children; the lists inside them are UI_LIST's public
 * geometry; the sessions and keys are ssh-d's store; and a connection is
 * ssh-b's transport under ssh-c's auth.
 *
 * NOTHING HERE BLOCKS. ssh_poll() is called once per frame from the draw path
 * and returns whatever has arrived; a connection that stalls costs this app a
 * frame's worth of nothing, not the desktop's responsiveness.
 *
 * HOST-KEY POLICY LIVES HERE, which is where ssh-b and ssh-d both said it
 * belonged: the store answers known/unknown/mismatch, and the app decides. A
 * MISMATCH refuses and says so in red; an UNKNOWN host is recorded on first
 * sight and reported. That is trust-on-first-use, stated rather than implied.
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_font.h"
#include "pc64_icons.h"
#include "unossh.h"

#define NTABS     5
#define SCROLLCAP 4096

int  pc64_shell_font_mono(void);
void pc64_shell_dirty(void);

typedef struct {
    int  used, conn;                 /* conn = ssh handle, -1 = not connected */
    char title[24];
    char buf[SCROLLCAP];
    int  len;
} sshtab;

static sshtab g_tab[NTABS];
static int g_ntab, g_cur;
static const char *g_lbl[NTABS];
static int g_map[NTABS];
static int g_first;

static unoui_mdi_child g_kids[2];
static unoui_mdi g_mdi;
static int g_mdi_ready;

static const char *g_sess[SSH_MAXSESS];
static char  g_sessn[SSH_MAXSESS][SSH_NAMELEN];
static int   g_nsess, g_sess_sel, g_sess_top;
static const char *g_keys[SSH_MAXKEYS];
static char  g_keyn[SSH_MAXKEYS][SSH_NAMELEN + 16];
static int   g_nkeys, g_key_sel, g_key_top;

static char g_status[128];
static int  g_alarm;                 /* 1 = paint the status line as a warning */
static unoui_rect g_rect;

static const unoui_theme *TH(void) { return pc64_shell_theme(); }

static void put(char *d, int cap, const char *s)
{ int i; for (i = 0; i < cap - 1 && s && s[i]; i++) d[i] = s[i]; d[i] = 0; }

static void say(const char *a, const char *b, int alarm)
{
    int i = 0, j;
    for (j = 0; a && a[j] && i < (int)sizeof g_status - 1; j++) g_status[i++] = a[j];
    for (j = 0; b && b[j] && i < (int)sizeof g_status - 1; j++) g_status[i++] = b[j];
    g_status[i] = 0;
    g_alarm = alarm;
}

/* ---- the store, reloaded into display form ------------------------------- */
static void reload_lists(void)
{
    for (g_nsess = 0; g_nsess < SSH_MAXSESS; g_nsess++) {
        if (ssh_sess_list(g_nsess, g_sessn[g_nsess], SSH_NAMELEN) != 0) break;
        g_sess[g_nsess] = g_sessn[g_nsess];
    }
    for (g_nkeys = 0; g_nkeys < SSH_MAXKEYS; g_nkeys++) {
        int guarded = 0;
        char n[SSH_NAMELEN];
        if (ssh_key_list(g_nkeys, n, SSH_NAMELEN, 0, &guarded) != 0) break;
        put(g_keyn[g_nkeys], SSH_NAMELEN + 16, n);
        {   int k = 0;
            while (g_keyn[g_nkeys][k]) k++;
            put(g_keyn[g_nkeys] + k, 16, guarded ? "  (locked)" : "  (open)"); }
        g_keys[g_nkeys] = g_keyn[g_nkeys];
    }
    if (g_sess_sel >= g_nsess) g_sess_sel = g_nsess ? g_nsess - 1 : 0;
    if (g_key_sel >= g_nkeys)  g_key_sel = g_nkeys ? g_nkeys - 1 : 0;
}

static void ensure_mdi(void)
{
    if (g_mdi_ready) return;
    g_mdi_ready = 1;
    g_mdi.ch = g_kids;
    g_mdi.cap = 2;
    g_mdi.min_w = 120;
    g_mdi.min_h = 70;
    unoui_mdi_add(&g_mdi, "Sessions", 4, 4, 200, 150, UI_MDI_RESIZE, 0);
    unoui_mdi_add(&g_mdi, "Keys", 212, 4, 200, 150, UI_MDI_RESIZE, 0);
}

/* ---- tabs ---------------------------------------------------------------- */
static void tabs_model(unoui_tabs_model *m)
{
    int i, n = 0;
    m->sel = 0; m->hot = -1; m->hot_part = UI_TAB_NONE;
    for (i = 0; i < NTABS; i++) {
        if (!g_tab[i].used) continue;
        g_lbl[n] = g_tab[i].title;
        g_map[n] = i;
        if (i == g_cur) m->sel = n;
        n++;
    }
    m->labels = g_lbl; m->n = n; m->first = g_first;
    m->flags = UI_TF_CLOSE | UI_TF_PLUS | UI_TF_ELASTIC | UI_TF_OVERFLOW;
}

static int tab_new(const char *title)
{
    int i;
    for (i = 1; i < NTABS && g_tab[i].used; i++) ;
    if (i == NTABS) { say("No free tab. Close one first.", 0, 1); return -1; }
    g_tab[i].used = 1; g_tab[i].conn = -1; g_tab[i].len = 0;
    put(g_tab[i].title, sizeof g_tab[i].title, title);
    g_ntab++;
    g_cur = i;
    return i;
}

static void tab_close(int i)
{
    if (i <= 0 || i >= NTABS || !g_tab[i].used) return;   /* tab 0 is Manage */
    if (g_tab[i].conn >= 0) ssh_close(g_tab[i].conn);
    g_tab[i].used = 0; g_tab[i].conn = -1; g_tab[i].len = 0;
    g_ntab--;
    if (g_cur == i) g_cur = 0;
}

static void tab_add_text(sshtab *t, const char *s, int n)
{
    int i;
    if (t->len + n > SCROLLCAP) {          /* drop the oldest half */
        int keep = SCROLLCAP / 2;
        for (i = 0; i < keep; i++) t->buf[i] = t->buf[t->len - keep + i];
        t->len = keep;
    }
    for (i = 0; i < n && t->len < SCROLLCAP; i++) t->buf[t->len++] = s[i];
}

/* ---- connecting ---------------------------------------------------------- */
static void connect_selected(void)
{
    char host[SSH_HOSTLEN], user[SSH_NAMELEN], keyn[SSH_NAMELEN];
    unsigned char seed[32];
    int port = 22, h, ti;

    if (!g_nsess) { say("No saved sessions.", 0, 1); return; }
    if (ssh_sess_get(g_sessn[g_sess_sel], host, sizeof host, &port,
                     user, sizeof user, keyn, sizeof keyn) != 0) {
        say("Cannot read that session.", 0, 1); return;
    }
    if (!keyn[0]) { say("That session has no key.", 0, 1); return; }
    if (ssh_key_load(keyn, "", seed) != 0) {
        say("Key needs a passphrase: ", keyn, 1); return;
    }
    say("Connecting to ", host, 0);
    h = ssh_connect(host, port);
    if (h < 0) { say("Connect failed: ", ssh_error(h), 1); return; }
    if (ssh_handshake(h) != 0) { say("Handshake failed: ", ssh_error(h), 1); ssh_close(h); return; }

    /* the policy ssh-b and ssh-d both deferred to here */
    switch (ssh_verify_host(h, host)) {
    case SSH_HOST_MISMATCH:
        say("HOST KEY CHANGED for ", host, 1);
        ssh_close(h);
        return;
    case SSH_HOST_UNKNOWN:
        ssh_known_add(host, ssh_host_fingerprint(h));
        say("New host, key recorded: ", host, 0);
        break;
    default:
        say("Known host: ", host, 0);
        break;
    }
    if (ssh_auth_key(h, user, seed) != 0) {
        say("Login refused: ", ssh_error(h), 1); ssh_close(h); return;
    }
    ti = tab_new(g_sessn[g_sess_sel]);
    if (ti < 0) { ssh_close(h); return; }
    g_tab[ti].conn = h;
    if (ssh_shell(h) != 0) {
        /* a server with no shell can still run one command */
        if (ssh_exec(h, "echo connected; uname -a 2>/dev/null || true") != 0) {
            say("Could not start a session: ", ssh_error(h), 1);
        }
    }
    say("Connected to ", host, 0);
}

/* One non-blocking sweep of every live connection. Called once per frame from
 * the draw path - which is the whole reason this app never stalls the desktop -
 * and by the gate, so the test drives the same code a frame does. */
static void pump_connections(void)
{
    int i;
    for (i = 1; i < NTABS; i++) {
        char chunk[512];
        int n;
        if (!g_tab[i].used || g_tab[i].conn < 0) continue;
        ssh_poll(g_tab[i].conn);
        n = ssh_read(g_tab[i].conn, chunk, (int)sizeof chunk);
        if (n > 0) { tab_add_text(&g_tab[i], chunk, n); pc64_shell_dirty(); }
        else if (n < 0) {
            ssh_close(g_tab[i].conn);
            g_tab[i].conn = -1;
            tab_add_text(&g_tab[i], "\n[connection closed]\n", 21);
        }
    }
}

/* ---- painting ------------------------------------------------------------ */
static int band_tabs_h(void) { return unoui_tabs_h(TH()); }
static int band_stat_h(void) { return fb_text_h() + 6; }

static void draw_list_pane(unoui_rect r, const char **items, int n, int sel,
                           int top, const char *empty)
{
    if (n) unoui_list_draw(TH(), r, items, n, sel, top);
    else fb_text(r.x + 6, r.y + 6, empty, TH()->pal.text_dim, -1);
}

static void draw_manage(unoui_rect body)
{
    unoui_rect a, b;
    ensure_mdi();
    reload_lists();
    unoui_mdi_draw(TH(), body, &g_mdi);
    a = unoui_mdi_content_rect(TH(), body, &g_mdi, 0);
    b = unoui_mdi_content_rect(TH(), body, &g_mdi, 1);
    if (a.w > 0) draw_list_pane(a, g_sess, g_nsess, g_sess_sel, g_sess_top, "no saved sessions");
    if (b.w > 0) draw_list_pane(b, g_keys, g_nkeys, g_key_sel, g_key_top, "no keys");
}

static void draw_term(unoui_rect body, sshtab *t)
{
    int mono = pc64_shell_font_mono();
    int lh, y, i, start;
    fb_fill_rect(body.x, body.y, body.w, body.h, TH()->pal.field_bg);
    if (mono >= 0 && unoui_font_push) unoui_font_push(mono);
    lh = fb_text_h() + 2;
    {   /* show the tail that fits: walk back the right number of newlines */
        int rows = body.h / (lh > 0 ? lh : 1), seen = 0;
        start = t->len;
        while (start > 0 && seen <= rows) { if (t->buf[start - 1] == '\n') seen++; start--; }
        if (start < 0) start = 0;
    }
    y = body.y + 2;
    i = start;
    while (i < t->len && y + lh <= body.y + body.h) {
        char line[160];
        int k = 0;
        while (i < t->len && t->buf[i] != '\n' && k < (int)sizeof line - 1) {
            char c = t->buf[i++];
            line[k++] = (c == '\r' || c == '\t') ? ' ' : c;
        }
        line[k] = 0;
        if (i < t->len && t->buf[i] == '\n') i++;
        fb_text(body.x + 4, y, line, TH()->pal.field_text, -1);
        y += lh;
    }
    if (mono >= 0 && unoui_font_pop) unoui_font_pop();
}

static void ssh_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    unoui_tabs_model m;
    unoui_rect strip, body, stat;
    (void)w; (void)ctx;
    g_rect = r;
    if (!g_tab[0].used) {                 /* the Manage tab always exists */
        g_tab[0].used = 1; g_tab[0].conn = -1;
        put(g_tab[0].title, sizeof g_tab[0].title, "Manage");
        g_ntab = 1;
        say("Pick a session and press + to connect.", 0, 0);
    }

    pump_connections();

    strip = r; strip.h = band_tabs_h();
    stat = r; stat.y = r.y + r.h - band_stat_h(); stat.h = band_stat_h();
    body = r; body.y = strip.y + strip.h;
    body.h = r.h - strip.h - stat.h;
    if (body.h < 20) body.h = 20;

    tabs_model(&m);
    g_first = unoui_tabs_reveal(TH(), strip, &m, m.sel);
    m.first = g_first;
    unoui_tabs_draw(TH(), strip, &m);

    if (g_cur == 0) draw_manage(body);
    else            draw_term(body, &g_tab[g_cur]);

    fb_fill_rect(stat.x, stat.y, stat.w, stat.h, TH()->pal.face);
    fb_hline(stat.x, stat.y, stat.w, TH()->pal.shadow);
    fb_text(stat.x + 6, stat.y + 3, g_status,
            g_alarm ? FB_RGB(190, 40, 40) : TH()->pal.text, -1);
}

/* ---- input --------------------------------------------------------------- */
static int ssh_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    unoui_rect r = g_rect, strip, body;
    (void)w; (void)ctx;
    strip = r; strip.h = band_tabs_h();
    body = r; body.y = strip.y + strip.h;
    body.h = r.h - strip.h - band_stat_h();

    if (e->kind == UI_EV_MOUSE_DOWN) {
        unoui_tabs_model m;
        int slot = -1, part;
        tabs_model(&m);
        part = unoui_tabs_hit(TH(), strip, &m, e->x, e->y, &slot);
        if (part == UI_TAB_PLUS)  { connect_selected(); return 1; }
        if (part == UI_TAB_CLOSE && slot >= 0) {
            if (g_map[slot] == 0) say("The Manage tab stays open.", 0, 0);
            else tab_close(g_map[slot]);
            return 1;
        }
        if (part == UI_TAB_SEL && slot >= 0) { g_cur = g_map[slot]; return 1; }
        if (part == UI_TAB_OVER) {
            int mf = unoui_tabs_maxfirst(TH(), strip, &m);
            if (g_first < mf) g_first++;
            return 1;
        }
        if (g_cur == 0 && e->y >= body.y) {          /* the Manage panes */
            int ci = unoui_mdi_at(body, &g_mdi, e->x, e->y);
            if (ci >= 0) {
                unoui_rect in = unoui_mdi_content_rect(TH(), body, &g_mdi, ci);
                unoui_mdi_raise(&g_mdi, ci);
                if (e->y >= in.y && e->x >= in.x && e->x < in.x + in.w) {
                    if (ci == 0 && g_nsess) {
                        g_sess_sel = unoui_list_index_at(in, g_nsess, g_sess_top, e->y);
                        say("Session: ", g_sessn[g_sess_sel], 0);
                    } else if (ci == 1 && g_nkeys) {
                        g_key_sel = unoui_list_index_at(in, g_nkeys, g_key_top, e->y);
                        say("Key: ", g_keyn[g_key_sel], 0);
                    }
                }
                return 1;
            }
        }
        return 1;
    }
    if (e->kind == UI_EV_CHAR && g_cur > 0 && g_tab[g_cur].conn >= 0) {
        char c = (char)e->ch;
        ssh_write(g_tab[g_cur].conn, &c, 1);
        return 1;
    }
    if (e->kind == UI_EV_KEY && g_cur > 0 && g_tab[g_cur].conn >= 0 &&
        e->key == UI_KEY_ENTER) {
        ssh_write(g_tab[g_cur].conn, "\n", 1);
        return 1;
    }
    return 0;
}

static unoui_canvas g_canvas = { ssh_draw, ssh_event, 0 };

unoui_canvas *pc64_sshapp_canvas(void) { return &g_canvas; }

void pc64_sshapp_open(void)
{
    reload_lists();
    if (!ssh_store_persistent())
        say("Store is on the RAM disk: keys will not survive a reboot.", 0, 1);
}

/* ---- the gate's way in ---------------------------------------------------
 * These drive the SAME functions a click does - connect_selected(), tab_close()
 * and the per-frame pump - rather than a parallel copy of them. Clicking the
 * "+" through the Start menu would have meant depending on where EX_SSH lands
 * in the launcher, which fails for reasons that have nothing to do with the
 * app; the pointer is used for the screenshot, where it proves something the
 * assertions cannot. */
int pc64_sshapp_dbg_select(const char *name)
{
    int i;
    reload_lists();
    for (i = 0; i < g_nsess; i++) {
        int k;
        for (k = 0; name[k] && g_sessn[i][k] == name[k]; k++) ;
        if (!name[k] && !g_sessn[i][k]) { g_sess_sel = i; return 1; }
    }
    return 0;
}
int pc64_sshapp_dbg_connect(void)
{
    reload_lists();
    connect_selected();
    return (g_cur > 0 && g_tab[g_cur].conn >= 0) ? 1 : 0;
}
void pc64_sshapp_dbg_pump(void) { pump_connections(); }
void pc64_sshapp_dbg_close(void) { tab_close(g_cur); }
int pc64_sshapp_dbg_tabs(void) { return g_ntab; }
int pc64_sshapp_dbg_cur(void) { return g_cur; }
const char *pc64_sshapp_dbg_status(void) { return g_status; }
int pc64_sshapp_dbg_textlen(void) { return (g_cur > 0) ? g_tab[g_cur].len : 0; }
