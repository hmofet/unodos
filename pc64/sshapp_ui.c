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

/* ---- the editor -----------------------------------------------------------
 * WHY THIS EXISTS. Until it did, the store this app reads had no writer on the
 * device at all: the Manage tab could list sessions and keys but not create
 * one, `ssh_sess_set` / `ssh_key_generate`'s only callers in the whole tree
 * were the self-test suites, and the `ssh` URC verb - which can populate the
 * store - had never been given its dispatch clause. So a user who installed
 * UnoDOS could only ever connect to sessions that did not exist. The verb is
 * wired now, but a client that needs a SECOND machine to become usable is not
 * a client, so the app gets its own writer.
 *
 * It is a one-line prompt in the status band rather than a modal dialog: this
 * is a CANVAS, so a dialog would mean hosting widgets and a focus model inside
 * it, and the band is already drawn every frame and already the place the app
 * talks to you from. Type, Backspace, Enter for the next field, Esc to
 * abandon. Nothing is written to the store until the last field is entered. */
enum { PR_NONE = 0, PR_SESS, PR_KEY };
#define PR_MAXF 5
static int  g_prompt;                /* PR_* - 0 when not editing            */
static int  g_pfield;                /* which field is being typed           */
static char g_pval[PR_MAXF][SSH_HOSTLEN];
static int  g_pane;                  /* Manage tab: 0 = sessions, 1 = keys   */

/* field labels, per prompt kind */
static const char *kSessF[PR_MAXF] = { "Name", "Host", "Port", "User", "Key" };
static const char *kKeyF[PR_MAXF]  = { "New key name", 0, 0, 0, 0 };
static int prompt_nfields(void) { return g_prompt == PR_SESS ? 5 : 1; }
static const char *prompt_label(int i)
{ return (g_prompt == PR_SESS ? kSessF : kKeyF)[i]; }

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

/* ---- the editor: start, type, commit -------------------------------------- */
static void prompt_start(int kind)
{
    int i;
    g_prompt = kind; g_pfield = 0;
    for (i = 0; i < PR_MAXF; i++) g_pval[i][0] = 0;
    if (kind == PR_SESS) put(g_pval[2], SSH_HOSTLEN, "22");   /* the default port */
    say("Esc cancels, Enter accepts each field.", 0, 0);
}

static int pr_num(const char *s)
{ int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

/* Enter on the LAST field: write the store. Nothing before this point has
 * touched it, so an abandoned edit leaves no trace. */
static void prompt_commit(void)
{
    if (g_prompt == PR_SESS) {
        int port = pr_num(g_pval[2]);
        if (!g_pval[0][0] || !g_pval[1][0] || !g_pval[3][0])
            { say("A session needs a name, a host and a user.", 0, 1); g_prompt = PR_NONE; return; }
        if (port <= 0 || port > 65535) port = 22;
        if (ssh_sess_set(g_pval[0], g_pval[1], port, g_pval[3], g_pval[4]) != 0)
            say("Could not save that session (store full?).", 0, 1);
        else if (!ssh_store_persistent())
            say("Saved to the RAM disk - it will not survive a reboot: ", g_pval[0], 1);
        else
            say("Saved session ", g_pval[0], 0);
    } else if (g_prompt == PR_KEY) {
        if (!g_pval[0][0]) { say("A key needs a name.", 0, 1); g_prompt = PR_NONE; return; }
        /* Generated WITHOUT a passphrase on purpose: ssh_key_load takes "" and
         * there is nowhere yet to type one at connect time. A guarded key is
         * still importable, and `ssh keygen <n> <pass>` over URC still takes
         * one - this is the path that has to work with no second machine. */
        if (ssh_key_generate(g_pval[0], "") != 0)
            say("Could not generate that key (store full? name in use?).", 0, 1);
        else
            say("Generated key ", g_pval[0], 0);
    }
    g_prompt = PR_NONE;
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
        say("+ connects.  n new session   g generate key   p show public key   d delete", 0, 0);
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
    if (g_prompt) {                      /* the editor owns the band while open */
        char line[160];
        int i = 0, k;
        const char *lab = prompt_label(g_pfield);
        for (k = 0; lab[k] && i < (int)sizeof line - 3; k++) line[i++] = lab[k];
        line[i++] = ':'; line[i++] = ' ';
        for (k = 0; g_pval[g_pfield][k] && i < (int)sizeof line - 2; k++)
            line[i++] = g_pval[g_pfield][k];
        line[i++] = '_';                 /* a caret, so it reads as a field    */
        line[i] = 0;
        fb_text(stat.x + 6, stat.y + 3, line, TH()->pal.accent, -1);
    } else
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

    /* The editor swallows everything typed while it is open, INCLUDING the
     * keys that would otherwise start another edit - a prompt you cannot type
     * a 'g' into is not a prompt. */
    if (g_prompt && (e->kind == UI_EV_CHAR || e->kind == UI_EV_KEY)) {
        char *f = g_pval[g_pfield];
        int n = 0;
        while (f[n]) n++;
        if (e->kind == UI_EV_CHAR && e->ch == 27) { g_prompt = PR_NONE; say("Cancelled.", 0, 0); return 1; }
        if (e->kind == UI_EV_KEY && e->key == UI_KEY_ESC) { g_prompt = PR_NONE; say("Cancelled.", 0, 0); return 1; }
        if (e->kind == UI_EV_CHAR && e->ch == 8) { if (n) f[n - 1] = 0; return 1; }
        if (e->kind == UI_EV_KEY && e->key == UI_KEY_BACKSPACE) { if (n) f[n - 1] = 0; return 1; }
        if ((e->kind == UI_EV_KEY && e->key == UI_KEY_ENTER) ||
            (e->kind == UI_EV_CHAR && (e->ch == '\r' || e->ch == '\n'))) {
            if (g_pfield + 1 < prompt_nfields()) g_pfield++;
            else prompt_commit();
            return 1;
        }
        if (e->kind == UI_EV_CHAR && e->ch >= ' ' && e->ch < 127 &&
            n < SSH_HOSTLEN - 1) { f[n] = (char)e->ch; f[n + 1] = 0; }
        return 1;
    }

    /* Manage-tab commands. The keys are the app's whole editor surface, so
     * they are also what the status line offers when nothing is selected. */
    if (g_cur == 0 && e->kind == UI_EV_CHAR) {
        if (e->ch == 'n' || e->ch == 'N') { prompt_start(PR_SESS); return 1; }
        if (e->ch == 'g' || e->ch == 'G') { prompt_start(PR_KEY);  return 1; }
        if (e->ch == 'p' || e->ch == 'P') {          /* the public half, to paste */
            char pub[256], name[SSH_NAMELEN];
            reload_lists();
            if (!g_nkeys) { say("No keys yet - press g to generate one.", 0, 1); return 1; }
            if (ssh_key_list(g_key_sel, name, sizeof name, 0, 0) != 0) return 1;
            if (ssh_key_export_pub(name, pub, sizeof pub) > 0) say(pub, 0, 0);
            else say("Could not export that key.", 0, 1);
            return 1;
        }
        if (e->ch == 'd' || e->ch == 'D') {          /* delete in the live pane   */
            reload_lists();
            if (g_pane == 0 && g_nsess) {
                say(ssh_sess_delete(g_sessn[g_sess_sel]) == 0 ? "Deleted session "
                                                             : "Could not delete ",
                    g_sessn[g_sess_sel], 0);
            } else if (g_pane == 1 && g_nkeys) {
                char name[SSH_NAMELEN];
                if (ssh_key_list(g_key_sel, name, sizeof name, 0, 0) == 0)
                    say(ssh_key_delete(name) == 0 ? "Deleted key " : "Could not delete ",
                        name, 0);
            }
            reload_lists();
            return 1;
        }
    }
    if (g_cur == 0 && e->kind == UI_EV_KEY &&
        (e->key == UI_KEY_UP || e->key == UI_KEY_DOWN)) {
        int d = (e->key == UI_KEY_DOWN) ? 1 : -1;
        reload_lists();
        if (g_pane == 0 && g_nsess) {
            g_sess_sel += d;
            if (g_sess_sel < 0) g_sess_sel = 0;
            if (g_sess_sel >= g_nsess) g_sess_sel = g_nsess - 1;
            say("Session: ", g_sessn[g_sess_sel], 0);
        } else if (g_pane == 1 && g_nkeys) {
            g_key_sel += d;
            if (g_key_sel < 0) g_key_sel = 0;
            if (g_key_sel >= g_nkeys) g_key_sel = g_nkeys - 1;
            say("Key: ", g_keyn[g_key_sel], 0);
        }
        return 1;
    }

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
                g_pane = ci;              /* which list d / arrows act on */
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
