/* ===========================================================================
 * pc64_accounts.c - the security UI (see pc64_accounts.h).
 *
 * Three modal flows over a private unoui_ui, drawn with the live shell theme
 * and driven by the platform fb/input/present primitives:
 *   - pc64_login_gate()     : boot login + fresh-machine first-run.
 *   - pc64_consent_register(): the escalation consent sheet (a UAC-style prompt
 *                              registered with unosecure as its consent hook).
 *   - pc64_accounts_open()  : the Accounts manager (list/new/passwd/delete/role).
 *
 * These CONSUME unosecure; they never make a security decision themselves -
 * every mutation goes through the unosec_* API, which fail-closed-enforces the
 * sec.* capabilities.  Password input is masked (real chars kept aside; the
 * widget only ever sees '*').
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "mac_compat.h"        /* FB_W/FB_H + uno_pc64_* + fb_* */
#include "pc64_icons.h"        /* pc64_shell_theme() */
#include "unosecure.h"
#include "pc64_accounts.h"
#include <string.h>
#include <stdio.h>

/* ---- widget ids (private to our modal unoui_ui; no shell-id collision) ---- */
enum {
    ID_OK = 1, ID_CANCEL, ID_GUEST,
    ID_LIST, ID_NEW, ID_DEL, ID_PASSWD, ID_ADMIN, ID_CLOSE, ID_ROLE,
    ID_C_ONCE, ID_C_SESSION, ID_C_DENY
};

#define NAME_MAX 32
#define PW_MAX   64

/* ===========================================================================
 * Modal engine: a private unoui_ui, pumped a frame at a time.
 * ======================================================================== */
static unoui_ui   MU;
static int        m_lx, m_ly, m_lb;      /* last mouse for edge detection      */

/* password masking: the real secret is kept here; the edit widget only sees
 * '*'.  s_pw_wi is the widget index of the active password field (-1 = none). */
static char       g_pw[PW_MAX + 1];
static int        g_pwlen;
static int        s_pw_wi = -1;

static void modal_begin(unoui_window *sheet)
{
    int mx, my, mb;
    unoui_ui_init(&MU, pc64_shell_theme(), FB_W, FB_H);
    unoui_ui_add(&MU, sheet);
    uno_pc64_mouse(&mx, &my, &mb);
    m_lx = mx; m_ly = my; m_lb = mb & 1;     /* seed: no phantom press on entry */
}

/* Pump one frame: drain input into MU, render, present.  Returns 1 and fills
 * *out when a widget activated (or Enter->ID_OK / Esc->ID_CANCEL). */
static int modal_frame(unoui_action *out)
{
    unoui_event ev;
    int mx, my, mb, scan, uni, ctrl, got = 0;
    unoui_action a;

    out->changed = 0; out->id = 0;
    uno_pc64_poll();

    uno_pc64_mouse(&mx, &my, &mb); mb &= 1;
    if (mx != m_lx || my != m_ly) {
        memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_MOVE; ev.x = mx; ev.y = my;
        unoui_handle(&MU, &ev); m_lx = mx; m_ly = my;
    }
    if (mb && !m_lb) {
        memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_DOWN; ev.x = mx; ev.y = my;
        a = unoui_handle(&MU, &ev); if (a.changed) { *out = a; got = 1; }
    } else if (!mb && m_lb) {
        memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_UP; ev.x = mx; ev.y = my;
        a = unoui_handle(&MU, &ev); if (a.changed) { *out = a; got = 1; }
    }
    m_lb = mb;

    while (uno_pc64_next_key(&scan, &uni, &ctrl)) {
        int vk = 0, on_pass;
        switch (scan) {
        case 0x01: vk = UI_KEY_UP; break;    case 0x02: vk = UI_KEY_DOWN; break;
        case 0x03: vk = UI_KEY_RIGHT; break; case 0x04: vk = UI_KEY_LEFT; break;
        case 0x05: vk = UI_KEY_HOME; break;  case 0x06: vk = UI_KEY_END; break;
        case 0x08: vk = UI_KEY_DELETE; break;case 0x17: vk = UI_KEY_ESC; break;
        }
        if (!vk) {
            if (uni == 0x0D || uni == 0x0A) vk = UI_KEY_ENTER;
            else if (uni == 0x08) vk = UI_KEY_BACKSPACE;
            else if (uni == 0x09) vk = UI_KEY_TAB;
        }
        on_pass = (s_pw_wi >= 0 && MU.focus_wi == s_pw_wi);

        memset(&ev, 0, sizeof ev);
        if (vk) {
            ev.kind = UI_EV_KEY; ev.key = vk; ev.mods = ctrl ? UI_MOD_CTRL : 0;
            if (vk == UI_KEY_BACKSPACE && on_pass && g_pwlen > 0) g_pw[--g_pwlen] = 0;
        } else if (uni >= 32 && uni < 127) {
            ev.kind = UI_EV_CHAR;
            if (on_pass) {                       /* keep the real char, show '*' */
                if (g_pwlen < PW_MAX) { g_pw[g_pwlen++] = (char)uni; g_pw[g_pwlen] = 0; }
                ev.ch = '*';
            } else ev.ch = uni;
        } else continue;

        a = unoui_handle(&MU, &ev); if (a.changed) { *out = a; got = 1; }
        if (vk == UI_KEY_ENTER) { out->changed = 1; out->id = ID_OK; got = 1; }
        else if (vk == UI_KEY_ESC) { out->changed = 1; out->id = ID_CANCEL; got = 1; }
    }

    memset(&ev, 0, sizeof ev); ev.kind = UI_EV_TICK; unoui_handle(&MU, &ev);
    unoui_render_ui(&MU);
    uno_pc64_present();
    uno_pc64_delay_ms(16);
    return got;
}

/* ===========================================================================
 * Credential sheet (login OR create).  Returns the terminal id (ID_OK /
 * ID_CANCEL / ID_GUEST) and leaves the typed name in `name` and the real
 * password in g_pw.
 * ======================================================================== */
static char       s_name[NAME_MAX + 1];
static char       s_passvis[PW_MAX + 1];   /* the masked ('*') display buffer   */
static unoui_text s_name_t, s_pass_t;
static int        s_role_sel;              /* create: 0 user / 1 admin / 2 guest */

static const char *k_roles[] = { "user", "admin", "guest" };

static int cred_sheet(const char *title, const char *sub, int is_create,
                      int allow_guest)
{
    unoui_window win;
    unoui_widget *w;
    int W = 360, H = is_create ? 210 : 176;
    int x = (FB_W - W) / 2, y = (FB_H - H) / 2, cy = 8;

    s_name[0] = 0; s_passvis[0] = 0; g_pw[0] = 0; g_pwlen = 0; s_role_sel = 0;
    unoui_text_init(&s_name_t, s_name, sizeof s_name, 0);
    unoui_text_init(&s_pass_t, s_passvis, sizeof s_passvis, 0);

    unoui_window_init(&win, title, x, y, W, H);
    unoui_add_label(&win, 8, cy, sub); cy += 22;
    unoui_add_label(&win, 8, cy + 3, "User:");
    w = unoui_add_edit(&win, 64, cy, W - 80, &s_name_t); w->flags |= UI_F_FOCUS; cy += 26;
    unoui_add_label(&win, 8, cy + 3, "Password:");
    w = unoui_add_edit(&win, 80, cy, W - 96, &s_pass_t);
    s_pw_wi = win.nw - 1;                       /* mark for masking              */
    cy += 26;
    if (is_create) {
        unoui_add_label(&win, 8, cy + 3, "Role:");
        w = unoui_add_dropdown(&win, 64, cy, 140, k_roles, 3, 0); w->id = ID_ROLE;
        cy += 28;
    }
    { unoui_widget *b = unoui_add_button(&win, W - 108, cy, 100,
                                         is_create ? "Create" : "Sign In",
                                         UI_F_DEFAULT); b->id = ID_OK; }
    if (allow_guest) {
        unoui_widget *b = unoui_add_button(&win, 8, cy, 96, "Guest", 0); b->id = ID_GUEST;
    } else {
        unoui_widget *b = unoui_add_button(&win, 8, cy, 96, "Cancel", 0); b->id = ID_CANCEL;
    }

    modal_begin(&win);
    for (;;) {
        unoui_action a;
        if (!modal_frame(&a) || !a.changed) continue;
        if (a.id == ID_ROLE) { s_role_sel = a.value; continue; }
        if (a.kind == UI_ACT_CLOSE) { s_pw_wi = -1; return ID_CANCEL; }
        if (a.id == ID_OK || a.id == ID_CANCEL || a.id == ID_GUEST) {
            s_pw_wi = -1; return a.id;
        }
    }
}

/* ===========================================================================
 * Login gate (boot) + shell-session elevation.
 * ======================================================================== */
static usec_session_t g_shell_session;      /* the session the shell runs under */

/* Attempt a login using the sheet's current name/password.  On success binds
 * the session (persistently) and returns 1. */
static int try_login_bind(usc_trust_t trust)
{
    usec_session_t s = unosec_login(s_name, g_pw, trust);
    if (!s) return 0;
    if (!unosec_enter_session(s)) { unosec_logout(s); return 0; }
    g_shell_session = s;
    return 1;
}

void pc64_login_gate(void)
{
    if (unosec_account_list(0, 0) <= 0) return;      /* fresh machine: no gate   */

    for (;;) {
        int r = cred_sheet("Sign in to UnoDOS",
                           "Enter your account credentials.", 0, 0);
        if (r == ID_OK && try_login_bind(UNOSEC_TRUST_INTERACTIVE)) return;
        /* wrong credentials (or Cancel/Esc, which cannot bypass the gate):
         * loop and prompt again.  There is no guest path once accounts exist. */
    }
}

/* ===========================================================================
 * Escalation consent sheet (the unosecure consent provider).
 * ======================================================================== */
static usc_consent_t consent_cb(void *ctx, usc_uid_t uid, usc_trust_t trust,
                                usc_cap_t cap, const char *cap_name,
                                usc_tier_t tier, const char *detail)
{
    unoui_window win;
    char line1[96], line2[96];
    const char *tname = (trust == UNOSEC_TRUST_INTERACTIVE) ? "interactive"
                      : (trust == UNOSEC_TRUST_INSTALLED)   ? "installed"
                      : (trust == UNOSEC_TRUST_REMOTE)      ? "remote" : "sandbox";
    const char *tier_s = (tier >= USC_TIER_KERNEL) ? "KERNEL - can harm the machine"
                       : "system-wide (admin)";
    int kernel = (tier >= USC_TIER_KERNEL);
    int W = 520, H = 210, x = (FB_W - W) / 2, y = (FB_H - H) / 2, cy = 8;
    (void)ctx; (void)cap;

    snprintf(line1, sizeof line1, "A %s script (uid %lu) requests:", tname,
             (unsigned long)uid);
    snprintf(line2, sizeof line2, "  %s  -  %s", cap_name ? cap_name : "?", tier_s);

    unoui_window_init(&win, kernel ? "Kernel access requested"
                                    : "Permission requested", x, y, W, H);
    unoui_add_label(&win, 8, cy, line1); cy += 20;
    unoui_add_label(&win, 8, cy, line2); cy += 22;
    if (detail && detail[0]) { unoui_add_label(&win, 8, cy, detail); cy += 20; }
    unoui_add_label(&win, 8, cy, kernel
        ? "Deny unless you launched this and trust it completely."
        : "Allow only if you launched this and trust it."); cy += 26;

    /* Deny is the default (and the only default for KERNEL tier). */
    { unoui_widget *b = unoui_add_button(&win, 8, cy, 100, "Deny",
                                         UI_F_DEFAULT); b->id = ID_C_DENY; }
    { unoui_widget *b = unoui_add_button(&win, W - 310, cy, 140, "Allow once",
                                         0); b->id = ID_C_ONCE; }
    { unoui_widget *b = unoui_add_button(&win, W - 162, cy, 154, "Allow session",
                                         0); b->id = ID_C_SESSION; }

    modal_begin(&win);
    for (;;) {
        unoui_action a;
        if (!modal_frame(&a) || !a.changed) continue;
        if (a.id == ID_C_ONCE)    return UNOSEC_CONSENT_ONCE;
        if (a.id == ID_C_SESSION) return UNOSEC_CONSENT_SESSION;
        if (a.id == ID_C_DENY || a.id == ID_CANCEL || a.kind == UI_ACT_CLOSE)
            return UNOSEC_CONSENT_DENY;
        /* Enter maps to ID_OK -> the default action = Deny (fail-closed). */
        if (a.id == ID_OK) return UNOSEC_CONSENT_DENY;
    }
}

void pc64_consent_register(void) { unosec_set_consent_provider(consent_cb, 0); }

/* ===========================================================================
 * Accounts manager.
 * ======================================================================== */
static char        g_unames[16][NAME_MAX + 4];
static const char *g_uptr[16];
static usc_uid_t   g_uids[16];
static int         g_nusers;

static void load_users(void)
{
    usc_uid_t ids[16];
    int n = unosec_account_list(ids, 16), i;
    if (n < 0) n = 0;
    if (n > 16) n = 16;
    g_nusers = n;
    for (i = 0; i < n; i++) {
        const char *nm = unosec_account_name(ids[i]);
        g_uids[i] = ids[i];
        snprintf(g_unames[i], sizeof g_unames[i], "%s", nm ? nm : "?");
        g_uptr[i] = g_unames[i];
    }
}

/* Ensure the shell session can manage accounts.  Returns 1 if authorised
 * (possibly after a self-elevation login the caller must unwind), and sets
 * *pushed to the number of sessions it entered for elevation. */
static int ensure_authority(int *pushed)
{
    *pushed = 0;
    if (unosec_can(UNOSEC_CAP_USER_CREATE)) return 1;

    if (unosec_account_list(0, 0) <= 0) {
        /* first run: create the first administrator, then sign in as them. */
        int r = cred_sheet("Create the first administrator",
                           "No accounts exist yet. Create an admin to begin.", 0, 0);
        if (r != ID_OK || !s_name[0] || !g_pw[0]) return 0;
        if (!unosec_bootstrap_admin(s_name, g_pw)) return 0;
        if (try_login_bind(UNOSEC_TRUST_INTERACTIVE)) { *pushed = 1; return 1; }
        return 0;
    }
    /* accounts exist but the shell isn't an admin: elevate via a login. */
    {
        int r = cred_sheet("Administrator required",
                           "Sign in as an administrator to manage accounts.", 0, 0);
        if (r == ID_OK && try_login_bind(UNOSEC_TRUST_INTERACTIVE)) {
            if (unosec_can(UNOSEC_CAP_USER_CREATE)) { *pushed = 1; return 1; }
            unosec_leave();          /* logged in, but not an admin */
        }
        return 0;
    }
}

/* Sub-action: create a new user from a sheet. */
static void do_new_user(char *status, int cap)
{
    int r = cred_sheet("New account", "Create a user account.", 1, 0);
    if (r != ID_OK) { snprintf(status, cap, "New account cancelled."); return; }
    if (!s_name[0] || !g_pw[0]) { snprintf(status, cap, "Name and password required."); return; }
    if (unosec_account_create(s_name, g_pw, k_roles[s_role_sel]))
        snprintf(status, cap, "Created '%s' (%s).", s_name, k_roles[s_role_sel]);
    else
        snprintf(status, cap, "Could not create '%s' (exists / no authority).", s_name);
}

static void do_set_password(usc_uid_t uid, char *status, int cap)
{
    const char *nm = unosec_account_name(uid);
    int r = cred_sheet("Set password", "Enter a new password for the account.", 0, 0);
    if (r != ID_OK) { snprintf(status, cap, "Password change cancelled."); return; }
    if (!g_pw[0]) { snprintf(status, cap, "Password cannot be empty."); return; }
    if (unosec_account_set_password(uid, g_pw))
        snprintf(status, cap, "Password updated for '%s'.", nm ? nm : "?");
    else
        snprintf(status, cap, "Could not update password (no authority).");
}

void pc64_accounts_open(void)
{
    unoui_window win;
    unoui_widget *lw;
    char status[80];
    int sel = 0, pushed = 0, W = 380, H = 300;
    int x = (FB_W - W) / 2, y = (FB_H - H) / 2;

    if (!ensure_authority(&pushed)) return;      /* cancelled / not authorised   */
    status[0] = 0;

    for (;;) {
        int cy = 8;
        load_users();
        if (sel >= g_nusers) sel = g_nusers - 1;
        if (sel < 0) sel = 0;

        unoui_window_init(&win, "Accounts", x, y, W, H);
        unoui_add_label(&win, 8, cy, "Users on this system:"); cy += 20;
        lw = unoui_add_list(&win, 8, cy, W - 16, 120, g_uptr, g_nusers, sel);
        lw->id = ID_LIST; cy += 128;
        { unoui_widget *b;
          b = unoui_add_button(&win, 8,   cy, 84, "New...", 0);      b->id = ID_NEW;
          b = unoui_add_button(&win, 96,  cy, 108, "Password...", 0); b->id = ID_PASSWD;
          b = unoui_add_button(&win, 208, cy, 84, "Delete", 0);      b->id = ID_DEL;
          cy += 28;
          b = unoui_add_button(&win, 8,   cy, 130, "Toggle admin", 0); b->id = ID_ADMIN;
          b = unoui_add_button(&win, W - 92, cy, 84, "Close", UI_F_DEFAULT); b->id = ID_CLOSE;
          cy += 26;
        }
        if (status[0]) unoui_add_label(&win, 8, cy, status);

        modal_begin(&win);
        for (;;) {
            unoui_action a;
            if (!modal_frame(&a) || !a.changed) continue;
            if (a.id == ID_LIST) { sel = a.value; continue; }
            if (a.id == ID_CLOSE || a.id == ID_CANCEL || a.kind == UI_ACT_CLOSE)
                goto done;
            if (a.id == ID_NEW) { do_new_user(status, sizeof status); break; }
            if (a.id == ID_PASSWD) {
                if (g_nusers > 0) do_set_password(g_uids[sel], status, sizeof status);
                break;
            }
            if (a.id == ID_DEL) {
                if (g_nusers > 0) {
                    const char *nm = g_unames[sel];
                    if (unosec_account_delete(g_uids[sel]))
                        snprintf(status, sizeof status, "Deleted '%s'.", nm);
                    else
                        snprintf(status, sizeof status, "Could not delete (no authority).");
                }
                break;
            }
            if (a.id == ID_ADMIN) {
                if (g_nusers > 0) {
                    usc_uid_t u = g_uids[sel];
                    /* toggle: if it already checks admin caps, revoke; else grant */
                    if (unosec_role_grant(u, "admin"))
                        snprintf(status, sizeof status, "'%s' is now an admin.", g_unames[sel]);
                    else if (unosec_role_revoke(u, "admin"))
                        snprintf(status, sizeof status, "Removed admin from '%s'.", g_unames[sel]);
                    else
                        snprintf(status, sizeof status, "Could not change roles (no authority).");
                }
                break;
            }
            /* ID_OK (Enter) with nothing else: ignore. */
        }
    }
done:
    while (pushed-- > 0) unosec_leave();          /* drop any elevation we added */
}
