/* ===========================================================================
 * pc64_accounts.c - the security UI (see pc64_accounts.h).
 *
 * Three modal flows over a private unoui_ui, drawn with the live shell theme
 * and driven by the platform fb/input/present primitives:
 *   - pc64_login_gate()     : boot login + fresh-machine first-run.
 *   - pc64_consent_register(): the escalation consent sheet (a UAC-style prompt
 *                              registered with unosecure as its consent hook).
 *   - pc64_accounts_open()  : the Accounts manager (list/new/passwd/delete/role).
 *   - pc64_remote_open()    : the Remote Control panel - the only way to arm
 *                             unoautomate/URC in a production image.
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
#include "unoui_anim.h"        /* the shell's tween clock, for the reject shake */
#include "unoui_wmanim.h"      /* unoui_reject_window                          */
#include "unoauto_gate.h"      /* the Remote Control panel arms unoautomate */
#include "unoauto_remote.h"    /* modal_frame pumps the link (see the note there) */
#include "uno_debug.h"         /* uno_dbg_heartbeat - a no-op in production */
#include "net.h"               /* net_ip/net_link: the address to tell the user */
#include <string.h>
#include <stdio.h>

/* ---- widget ids (private to our modal unoui_ui; no shell-id collision) ---- */
enum {
    ID_OK = 1, ID_CANCEL, ID_GUEST,
    ID_LIST, ID_NEW, ID_DEL, ID_PASSWD, ID_ADMIN, ID_CLOSE, ID_ROLE,
    ID_C_ONCE, ID_C_SESSION, ID_C_DENY,
    ID_R_OBSERVE, ID_R_DRIVE, ID_R_SYSTEM, ID_R_ENABLE, ID_R_DISABLE
};

#define NAME_MAX 32
#define PW_MAX   64

/* ===========================================================================
 * Modal engine: a private unoui_ui, pumped a frame at a time.
 * ======================================================================== */
static unoui_ui   MU;
static int        m_lx, m_ly, m_lb;      /* last mouse for edge detection      */

/* The password the sheets collect.  It is the edit widget's OWN buffer now -
 * unoui_text_secret() masks a field in the toolkit, so the model holds the real
 * passphrase and only the drawing uses '*'.
 *
 * This file used to do it by hand: feed the widget '*' per keystroke, keep the
 * real characters in a side buffer, and intercept backspace to keep the two in
 * step.  Which meant the widget's caret, selection and length all described the
 * MASK - so Home, End, arrow keys, click-to-position and select-all all edited
 * the asterisks while the password sat untouched behind them.  Nobody noticed
 * because nobody arrows around a password field until the one time they do. */
static char       g_pw[PW_MAX + 1];

unoui_anim *uno_pc64_anim(void);        /* pc64_uui.c - the shell's clock */

static void modal_begin(unoui_window *sheet)
{
    int mx, my, mb;
    /* Lock out synthetic input for as long as a security dialog is up: these
     * sheets decide identity and authority, so only a person at the keyboard
     * drives them (uefi_main.c carries the full argument).  Cleared by the
     * shell's frame loop, which cannot run until this modal returns. */
    uno_pc64_input_lock(1);
    unoui_ui_init(&MU, pc64_shell_theme(), FB_W, FB_H);
    /* Borrow the shell's tween clock so a sheet can animate - the reject shake
     * is the only user today.  Pointed at directly rather than through
     * unoui_wmanim_install(), which would also re-point the global geometry
     * hooks and clear the window-move table underneath a shell snap that is
     * still in flight. */
    MU.anim = uno_pc64_anim();
    unoui_ui_add(&MU, sheet);
    uno_pc64_mouse(&mx, &my, &mb);
    m_lx = mx; m_ly = my; m_lb = mb & 1;     /* seed: no phantom press on entry */
}

/* Pump one frame: drain input into MU, render, present.  Returns 1 and fills
 * *out when a widget activated (or Enter->ID_OK / Esc->ID_CANCEL). */
static int modal_frame(unoui_action *out)
{
    unoui_event ev;
    int mx, my, mb, scan, uni, ctrl, got = 0, busy = 0;
    unoui_action a;

    out->changed = 0; out->id = 0;
    uno_pc64_poll();

    uno_pc64_mouse(&mx, &my, &mb); mb &= 1;
    if (mx != m_lx || my != m_ly) {
        memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_MOVE; ev.x = mx; ev.y = my;
        unoui_handle(&MU, &ev); m_lx = mx; m_ly = my;
        busy = 1;                            /* the cursor is being moved */
    }
    if (mb && !m_lb) {
        memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_DOWN; ev.x = mx; ev.y = my;
        a = unoui_handle(&MU, &ev); if (a.changed) { *out = a; got = 1; }
        busy = 1;
    } else if (!mb && m_lb) {
        memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_UP; ev.x = mx; ev.y = my;
        a = unoui_handle(&MU, &ev); if (a.changed) { *out = a; got = 1; }
        busy = 1;
    }
    m_lb = mb;

    while (uno_pc64_next_key(&scan, &uni, &ctrl)) {
        int vk = 0;
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
        memset(&ev, 0, sizeof ev);
        if (vk) {
            ev.kind = UI_EV_KEY; ev.key = vk; ev.mods = ctrl ? UI_MOD_CTRL : 0;
        } else if (uni >= 32 && uni < 127) {
            ev.kind = UI_EV_CHAR; ev.ch = uni;
        } else continue;

        a = unoui_handle(&MU, &ev); if (a.changed) { *out = a; got = 1; }
        busy = 1;
        if (vk == UI_KEY_ENTER) { out->changed = 1; out->id = ID_OK; got = 1; }
        else if (vk == UI_KEY_ESC) { out->changed = 1; out->id = ID_CANCEL; got = 1; }
    }

    memset(&ev, 0, sizeof ev); ev.kind = UI_EV_TICK; unoui_handle(&MU, &ev);
    /* the shell's frame loop is not running while a sheet is up, so the clock
     * this loop borrowed has to be advanced from here or nothing moves */
    unoui_anim_frame(uno_pc64_anim());
    unoui_render_ui(&MU);
    uno_pc64_present();

    /* This loop REPLACES the shell's frame loop for as long as a dialog is up,
     * so it owes the two things that loop does every frame:
     *
     *   - the freeze watchdog's heartbeat.  Without it the debug build's
     *     20-second LAPIC watchdog reads "a human is typing a password" as a
     *     wedged main loop and hard-resets the machine.  Metal-confirmed on the
     *     ZimaBlade 2026-08-03: opening the Remote Control panel reset the box
     *     ~20 s later, every time.  (No watchdog exists in production, so this
     *     compiles away there.)
     *   - the URC pump.  Without it an armed remote link goes silent the moment
     *     any security dialog opens, and on a headless box that is unrecoverable
     *     - you cannot send the click that would close the dialog.  The link
     *     stays serviced, so a remote operator still sees the machine and can
     *     `screen` grab the dialog; the lockout above is what stops them
     *     driving it. */
    uno_dbg_heartbeat();
    unoauto_remote_tick();

    /* SLEEP ONLY WHEN THE FRAME HAD NOTHING TO SHOW.
     *
     * This used to sleep a flat 16 ms every time round, including while the
     * pointer was being moved - so the cursor could not be redrawn faster than
     * ~55 Hz here however cheap the frame was, and in practice a good deal
     * slower, because the render and the present come out of the same 16 ms.
     *
     * The shell's own loop (pc64_uui.c) does NOT do that: a frame in which
     * only the cursor moved goes straight to uno_pc64_present() with no delay
     * after it, and the 16 ms wait is what it does when nothing happened at
     * all. So the sign-in sheet - the FIRST thing anyone touches on this
     * machine - moved its cursor several times slower than the desktop behind
     * it, which is exactly how it reads from metal: floaty at the login
     * screen, normal once the desktop is up.
     *
     * Presenting is already cheap when little changed (uno_pc64_present tracks
     * dirty rows and returns after 1 ms when none are), so dropping the wait on
     * a busy frame costs nothing on an idle one. An animation counts as busy:
     * the reject shake is timed off the TSC and wants real frames to draw into. */
    if (!busy && !unoui_anim_active(uno_pc64_anim()))
        uno_pc64_delay_ms(16);
    return got;
}

/* ===========================================================================
 * Credential sheet (login OR create).  Returns the terminal id (ID_OK /
 * ID_CANCEL / ID_GUEST) and leaves the typed name in `name` and the real
 * password in g_pw.
 * ======================================================================== */
static char       s_name[NAME_MAX + 1];
static unoui_text s_name_t, s_pass_t;
static int        s_role_sel;              /* create: 0 user / 1 admin / 2 guest */

static const char *k_roles[] = { "user", "admin", "guest" };

/* truncate-with-ellipsis to a pixel width; defined with the consent sheet */
static const char *fit_px(char *buf, int cap, const char *s, int px);

/* ---- sheet geometry -------------------------------------------------------
 * unoui_window_init takes the OUTER width; widget x is relative to the CONTENT
 * origin, and the frame plus the theme's padding come out of BOTH sides (26 px
 * under the shipping Aurora theme).  Every sheet in this file laid widgets out
 * against the outer number - `W - 80` wide fields inside a `W` window - so the
 * right-hand end of every text field and every right-anchored button sat 10 px
 * past the content edge and was clipped, at the default font, in the shipping
 * theme.  consent_cb worked this out the hard way in 2026-08; these helpers are
 * that lesson applied to the other three sheets.
 *
 * `sheet_outer` converts a wanted CONTENT size into the window rect to ask for,
 * clamped to the screen; `sheet_inner` is the usable content width. */
static int sheet_chrome(void)
{ const unoui_theme *th = pc64_shell_theme(); return 2 * (th->m.frame_w + th->m.pad); }

static void sheet_outer(int content_w, int content_h, int *W, int *H, int *x, int *y)
{
    const unoui_theme *th = pc64_shell_theme();
    *W = content_w + sheet_chrome();
    *H = content_h + th->m.title_h + th->m.pad + th->m.frame_w;
    if (*W > FB_W) *W = FB_W;
    if (*H > FB_H) *H = FB_H;
    *x = (FB_W - *W) / 2; if (*x < 0) *x = 0;
    *y = (FB_H - *H) / 2; if (*y < 0) *y = 0;
}
static int sheet_inner(int W) { return W - sheet_chrome(); }

/* Run unoui's layout audit over a sheet just before it goes modal, so these
 * four - which are built inside blocking loops and so are out of reach of the
 * shell's boot-time sweep - are covered by the same check as every other
 * window.  Debug builds only; compiles away entirely in production. */
#ifdef UNO_DEBUG
static void sheet_audit_cb(void *ctx, const unoui_window *win, int wi,
                           const char *why, unoui_rect r, int cw, int ch)
{
    (void)ctx;
    uno_dbg_log("layout: %s w%d: %s - rect %d,%d %dx%d, content %dx%d",
                win->title ? win->title : "(sheet)", wi, why,
                r.x, r.y, r.w, r.h, cw, ch);
}
static void sheet_audit(const unoui_window *win)
{
    int n = unoui_window_audit(pc64_shell_theme(), win, sheet_audit_cb, 0);
    if (win->r.w > FB_W || win->r.h > FB_H)
        uno_dbg_log("layout: %s: sheet %dx%d does not fit the %dx%d screen",
                    win->title ? win->title : "(sheet)",
                    win->r.w, win->r.h, FB_W, FB_H);
    else if (!n)
        uno_dbg_log("layout: %s: ok (%dx%d)",
                    win->title ? win->title : "(sheet)", win->r.w, win->r.h);
}
#else
#define sheet_audit(w) ((void)0)
#endif

/* `verify` (NULL for a sheet that just collects) is asked whether the typed
 * credentials are good the moment the sheet is submitted.  Returning 0 means
 * WRONG, and the sheet stays up and SHAKES rather than closing.
 *
 * That is the whole point of it taking a callback.  The login gate used to
 * close the sheet, test the password, and open a brand new one on failure -
 * so a mistyped character cost you the username as well, and the only signal
 * that anything had happened was the dialog flickering.  A prompt that survives
 * a wrong answer is also the only place a reject gesture can live. */
static int cred_sheet(const char *title, const char *sub, int is_create,
                      int allow_guest, int (*verify)(void))
{
    unoui_window win;
    unoui_widget *w;
    int fh = fb_text_h(), ch = ui_field_h(), bh = ui_ctl_h();
    int lofs = (ch - fh) / 2, pad = 8;
    /* the label column is as wide as its widest label, in the LIVE font */
    int lw = fb_text_w("Password:");
    int bw_ok = fb_text_w(is_create ? "Create" : "Sign In") + 30;
    int bw_alt = fb_text_w(allow_guest ? "Guest" : "Cancel") + 30;
    int W, H, x, y, inner, cy = pad;
    { int want = pad + lw + 8 + 160 + pad;        /* a usable field width   */
      int brow = pad + bw_alt + 12 + bw_ok + pad;
      int subw = pad + fb_text_w(sub) + pad;
      if (brow > want) want = brow;
      if (subw > want) want = subw;
      sheet_outer(want, pad + fh + 10 + 2 * (ch + 8)
                        + (is_create ? ch + 8 : 0) + bh + pad, &W, &H, &x, &y); }
    inner = sheet_inner(W);

    s_name[0] = 0; g_pw[0] = 0; s_role_sel = 0;
    unoui_text_init(&s_name_t, s_name, sizeof s_name, 0);
    unoui_text_init(&s_pass_t, g_pw, sizeof g_pw, 0);
    unoui_text_secret(&s_pass_t, '*');      /* masked, with the toolkit's eye */

    unoui_window_init(&win, title, x, y, W, H);
    { static char t1[128];
      unoui_add_label(&win, pad, cy, fit_px(t1, sizeof t1, sub, inner - 2 * pad)); }
    cy += fh + 10;
    unoui_add_label(&win, pad, cy + lofs, "User:");
    w = unoui_add_edit(&win, pad + lw + 8, cy, inner - pad - lw - 8 - pad, &s_name_t);
    w->flags |= UI_F_FOCUS; cy += ch + 8;
    unoui_add_label(&win, pad, cy + lofs, "Password:");
    w = unoui_add_edit(&win, pad + lw + 8, cy, inner - pad - lw - 8 - pad, &s_pass_t);
    cy += ch + 8;
    if (is_create) {
        int dw = 0, k;
        for (k = 0; k < 3; k++) { int t2 = fb_text_w(k_roles[k]); if (t2 > dw) dw = t2; }
        unoui_add_label(&win, pad, cy + lofs, "Role:");
        w = unoui_add_dropdown(&win, pad + lw + 8, cy, dw + 34, k_roles, 3, 0);
        w->id = ID_ROLE;
        cy += ch + 8;
    }
    { unoui_widget *b = unoui_add_button(&win, inner - pad - bw_ok, cy, bw_ok,
                                         is_create ? "Create" : "Sign In",
                                         UI_F_DEFAULT); b->id = ID_OK; }
    { unoui_widget *b = unoui_add_button(&win, pad, cy, bw_alt,
                                         allow_guest ? "Guest" : "Cancel", 0);
      b->id = allow_guest ? ID_GUEST : ID_CANCEL; }

    sheet_audit(&win);
    modal_begin(&win);
    for (;;) {
        unoui_action a;
        if (!modal_frame(&a) || !a.changed) continue;
        if (a.id == ID_ROLE) { s_role_sel = a.value; continue; }
        if (a.kind == UI_ACT_CLOSE) return ID_CANCEL;
        if (a.id == ID_OK && verify && !verify()) {
            /* Wrong.  Shake the sheet, keep everything typed, and say nothing
             * about WHICH part was wrong - a login prompt is not allowed to
             * narrow the guess for whoever is standing there. */
            unoui_reject_window(&MU, &win);
            { int i; for (i = 0; i < PW_MAX; i++) g_pw[i] = 0; }
            s_pass_t.len = s_pass_t.caret = s_pass_t.sel = 0;
            s_pass_t.scroll_x = 0;
            continue;
        }
        if (a.id == ID_OK || a.id == ID_CANCEL || a.id == ID_GUEST) return a.id;
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

static int verify_interactive(void)
{ return try_login_bind(UNOSEC_TRUST_INTERACTIVE); }

/* a login that is valid but not an administrator is still a NO here */
static int verify_admin(void)
{
    if (!try_login_bind(UNOSEC_TRUST_INTERACTIVE)) return 0;
    if (unosec_can(UNOSEC_CAP_USER_CREATE)) return 1;
    unosec_leave();                      /* logged in, but not an admin */
    return 0;
}

void pc64_login_gate(void)
{
    if (unosec_account_list(0, 0) <= 0) return;      /* fresh machine: no gate   */

    /* ONE sheet, which does not close until a login succeeds: it verifies in
     * place, shakes on a wrong password and keeps the username.  Cancel and Esc
     * cannot dismiss it either - there is no guest path once accounts exist -
     * so the loop is only here for those. */
    for (;;)
        if (cred_sheet("Sign in to UnoDOS", "Enter your account credentials.",
                       0, 0, verify_interactive) == ID_OK) return;
}

/* ===========================================================================
 * Escalation consent sheet (the unosecure consent provider).
 * ======================================================================== */

/* Truncate `s` to `px` pixels, ellipsising, into `buf`.  Needed because
 * fb_set_clip does NOT clip text: a line wider than the sheet draws straight
 * past its frame instead of being cut off at it.  Returns `s` untouched when it
 * already fits, so the common case copies nothing. */
static const char *fit_px(char *buf, int cap, const char *s, int px)
{
    int n = 0, dots;
    if (!s || !*s) return "";
    if (fb_text_w(s) <= px) return s;
    dots = fb_text_w("...");
    while (s[n] && n < cap - 4) {
        buf[n] = s[n]; buf[n + 1] = 0;
        if (fb_text_w(buf) > px - dots) { buf[n] = 0; break; }
        n++;
    }
    buf[n] = 0;
    if (n < cap - 4) { buf[n] = '.'; buf[n+1] = '.'; buf[n+2] = '.'; buf[n+3] = 0; }
    return buf;
}
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
    const char *warn = kernel ? "Deny unless you launched this and trust it completely."
                              : "Allow only if you launched this and trust it.";
    /* Buttons size to their text (the house idiom), so this lays out under any
     * font as well as any width. */
    int bw_d = fb_text_w("Deny") + 26;
    int bw_o = fb_text_w("Allow once") + 26;
    int bw_s = fb_text_w("Allow session") + 26;
    const int pad = 8, gap = 8;
    int W, H, x, y, cy = 8, two_rows, inner;
    (void)ctx; (void)cap;

    snprintf(line1, sizeof line1, "A %s script (uid %lu) requests:", tname,
             (unsigned long)uid);
    snprintf(line2, sizeof line2, "  %s  -  %s", cap_name ? cap_name : "?", tier_s);

    /* WIDTH.  This used to be a flat 520 with x = (FB_W - W) / 2, which on the
     * 400x300 desktop put the origin at -60 and ran 60 px off the right edge:
     * the title read "mission requested", the capability line "mate.observe",
     * and the third button "Allow se...".  A KERNEL-tier prompt the user cannot
     * read is not a place to be approving anything.  Found on the ZimaBlade
     * 2026-08-04.  Size to the content, clamp to the screen, never go negative. */
    W = fb_text_w(line2);
    if (fb_text_w(line1) > W) W = fb_text_w(line1);
    if (fb_text_w(warn)  > W) W = fb_text_w(warn);
    if (detail && detail[0] && fb_text_w(detail) > W) W = fb_text_w(detail);
    {   /* unoui_window_init takes the OUTER width: the frame and the theme's
         * content padding come out of it on BOTH sides, and widget x is
         * relative to the content area.  Sizing W to the text alone left every
         * line 2*(frame_w + pad) too long, which is why the first attempt at
         * this fix still ran the capability line into the frame. */
        const unoui_theme *th = pc64_shell_theme();
        int chrome = 2 * (th->m.frame_w + th->m.pad);
        int brow   = pad + bw_d + gap + bw_o + gap + bw_s + pad;
        if (brow > W) W = brow;                        /* the buttons need it   */
        W += 2 * pad + chrome;
        if (W > FB_W) W = FB_W;                        /* never overhang        */
        if (W < 200)  W = 200;
        inner = W - chrome;                            /* usable content width  */
    }

    /* If the buttons no longer fit side by side at that width, stack them: the
     * two Allow buttons on one row, Deny on its own below.  Deny stays the
     * default either way. */
    two_rows = (pad + bw_d + gap + bw_o + gap + bw_s + pad > inner);
    H = 210 + (two_rows ? 28 : 0);
    x = (FB_W - W) / 2; if (x < 0) x = 0;
    y = (FB_H - H) / 2; if (y < 0) y = 0;

    unoui_window_init(&win, kernel ? "Kernel access requested"
                                    : "Permission requested", x, y, W, H);
    {   /* every line clamped to the sheet's content width - see fit_px */
        static char t1[96], t2[96], t3[96], t4[96];
        unoui_add_label(&win, pad, cy, fit_px(t1, sizeof t1, line1, inner - 2 * pad)); cy += 20;
        unoui_add_label(&win, pad, cy, fit_px(t2, sizeof t2, line2, inner - 2 * pad)); cy += 22;
        if (detail && detail[0]) {
            unoui_add_label(&win, pad, cy, fit_px(t3, sizeof t3, detail, inner - 2 * pad));
            cy += 20;
        }
        unoui_add_label(&win, pad, cy, fit_px(t4, sizeof t4, warn, inner - 2 * pad)); cy += 26;
    }

    /* Deny is the default (and the only default for KERNEL tier).  Laid out
     * from the RIGHT edge so the Allow pair stays anchored whatever the width. */
    if (!two_rows) {
        unoui_widget *b;
        b = unoui_add_button(&win, pad, cy, bw_d, "Deny", UI_F_DEFAULT);
        b->id = ID_C_DENY;
        b = unoui_add_button(&win, inner - pad - bw_s - gap - bw_o, cy, bw_o,
                             "Allow once", 0); b->id = ID_C_ONCE;
        b = unoui_add_button(&win, inner - pad - bw_s, cy, bw_s, "Allow session", 0);
        b->id = ID_C_SESSION;
    } else {
        unoui_widget *b;
        b = unoui_add_button(&win, inner - pad - bw_s - gap - bw_o, cy, bw_o,
                             "Allow once", 0); b->id = ID_C_ONCE;
        b = unoui_add_button(&win, inner - pad - bw_s, cy, bw_s, "Allow session", 0);
        b->id = ID_C_SESSION;
        cy += 28;
        b = unoui_add_button(&win, pad, cy, bw_d, "Deny", UI_F_DEFAULT);
        b->id = ID_C_DENY;
    }

    sheet_audit(&win);
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
                           "No accounts exist yet. Create an admin to begin.",
                           0, 0, 0);
        if (r != ID_OK || !s_name[0] || !g_pw[0]) return 0;
        if (!unosec_bootstrap_admin(s_name, g_pw)) return 0;
        if (try_login_bind(UNOSEC_TRUST_INTERACTIVE)) { *pushed = 1; return 1; }
        return 0;
    }
    /* accounts exist but the shell isn't an admin: elevate via a login. */
    {
        /* Verifying in place also fixes a quieter fault: a login that WORKED but
         * was not an admin used to close the sheet, unwind the session and
         * return "no", which from the outside is indistinguishable from a wrong
         * password - and from nothing happening at all. It shakes now, and the
         * sheet stays up for another go. */
        int r = cred_sheet("Administrator required",
                           "Sign in as an administrator to manage accounts.",
                           0, 0, verify_admin);
        if (r == ID_OK) { *pushed = 1; return 1; }
        return 0;
    }
}

/* Sub-action: create a new user from a sheet. */
static void do_new_user(char *status, int cap)
{
    int r = cred_sheet("New account", "Create a user account.", 1, 0, 0);
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
    int r = cred_sheet("Set password", "Enter a new password for the account.",
                       0, 0, 0);
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
    int fh = fb_text_h(), bh = ui_ctl_h(), pad = 8;
    int sel = 0, pushed = 0, W, H, x, y, inner, listh;
    /* every button sized to its label in the live font, and the two rows
       measured, so the window is as wide as its widest row and no wider */
    int b_new  = fb_text_w("New...")       + 26;
    int b_pwd  = fb_text_w("Password...")  + 26;
    int b_del  = fb_text_w("Delete")       + 26;
    int b_adm  = fb_text_w("Toggle admin") + 26;
    int b_cls  = fb_text_w("Close")        + 26;

    if (!ensure_authority(&pushed)) return;      /* cancelled / not authorised   */
    status[0] = 0;
    listh = 6 * ui_row_h() + 6;
    { int r1 = pad + b_new + 8 + b_pwd + 8 + b_del + pad;
      int r2 = pad + b_adm + 12 + b_cls + pad;
      int want = r1 > r2 ? r1 : r2;
      if (want < 300) want = 300;
      sheet_outer(want, pad + fh + 6 + listh + 10 + bh + 8 + bh + 8 + fh + pad,
                  &W, &H, &x, &y); }
    inner = sheet_inner(W);

    for (;;) {
        int cy = pad;
        load_users();
        if (sel >= g_nusers) sel = g_nusers - 1;
        if (sel < 0) sel = 0;

        unoui_window_init(&win, "Accounts", x, y, W, H);
        unoui_add_label(&win, pad, cy, "Users on this system:"); cy += fh + 6;
        lw = unoui_add_list(&win, pad, cy, inner - 2 * pad, listh,
                            g_uptr, g_nusers, sel);
        lw->id = ID_LIST; cy += listh + 10;
        { unoui_widget *b;
          b = unoui_add_button(&win, pad, cy, b_new, "New...", 0); b->id = ID_NEW;
          b = unoui_add_button(&win, pad + b_new + 8, cy, b_pwd, "Password...", 0);
          b->id = ID_PASSWD;
          b = unoui_add_button(&win, pad + b_new + 8 + b_pwd + 8, cy, b_del,
                               "Delete", 0); b->id = ID_DEL;
          cy += bh + 8;
          b = unoui_add_button(&win, pad, cy, b_adm, "Toggle admin", 0);
          b->id = ID_ADMIN;
          b = unoui_add_button(&win, inner - pad - b_cls, cy, b_cls, "Close",
                               UI_F_DEFAULT); b->id = ID_CLOSE;
          cy += bh + 8;
        }
        if (status[0]) { static char st[96];
            unoui_add_label(&win, pad, cy,
                            fit_px(st, sizeof st, status, inner - 2 * pad)); }

        sheet_audit(&win);
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

/* ===========================================================================
 * Remote control (unoautomate / URC) - the arming panel.
 *
 * unoautomate ships in production, disarmed, and this is the ONLY way to turn
 * it on (unoauto_gate.h has the full model).  The panel is deliberately blunt
 * about what each tick hands out, because the person clicking it is the only
 * safeguard between a LAN and a machine that can be driven, watched and
 * reformatted from it.  Ticking a box does not itself grant anything - Enable
 * runs a real unosec_request per power, so a user whose roles do not cover one
 * gets the normal consent sheet on top of this window, and a refusal there
 * means that power simply is not in the arm.
 *
 * The token is displayed, never stored: it lives in RAM until disarm, sign-out
 * or reboot.  There is no "remember this" affordance on purpose.
 * ======================================================================== */
static void ip_str(char *out, int cap)
{
    const u8 *ip = net_ip();
    if (!net_link() || !ip || (!ip[0] && !ip[1] && !ip[2] && !ip[3]))
        { snprintf(out, (size_t)cap, "(no network link yet)"); return; }
    snprintf(out, (size_t)cap, "%d.%d.%d.%d:5099", ip[0], ip[1], ip[2], ip[3]);
}

void pc64_remote_open(void)
{
    unoui_window win;
    char status[96], addr[40], tokline[64];
    int fh = fb_text_h(), bh = ui_ctl_h(), pad = 8;
    int W, H, x, y, inner;
    /* The three tick labels are the widest things here and they are sentences,
       so the panel is sized to THEM rather than to a round 400 - at anything
       above the 8 px bitmap face "Full access - disks, files, run code,
       restart" ran off the end of a sheet that is the only place in the OS
       where remote control gets turned on. */
    static const char *k_obs = "Watch  - see the screen and system state";
    static const char *k_drv = "Control  - move the mouse, type, open apps";
    static const char *k_sys = "Full access  - disks, files, run code, restart";
    static const char *k_l1  = "Another computer can connect to this machine";
    static const char *k_l2  = "and use it. Grant only what you need:";
    int b_on   = fb_text_w("Update access") + 26;
    int b_off  = fb_text_w("Turn off")      + 26;
    int b_cls  = fb_text_w("Close")         + 26;
    /* what the user has ticked - seeded from what is already granted, so
       re-opening the panel on an armed channel shows the truth. */
    int want_obs = 1, want_drv = 1, want_sys = 0;

    status[0] = 0;
    { int want = 0, k;
      const char *lines[5]; lines[0]=k_obs; lines[1]=k_drv; lines[2]=k_sys;
      lines[3]=k_l1; lines[4]=k_l2;
      for (k = 0; k < 5; k++)
          { int t2 = 12 + 18 + fb_text_w(lines[k]) + pad; if (t2 > want) want = t2; }
      { int brow = pad + b_on + 8 + b_off + 12 + b_cls + pad;
        if (brow > want) want = brow; }
      sheet_outer(want, pad + 3 * fh + 8 + 3 * (fh + 8) + 6
                        + 3 * (fh + 4) + 8 + bh + 8 + fh + pad,
                  &W, &H, &x, &y); }
    inner = sheet_inner(W);
    if (unoauto_gate_armed()) {
        unsigned p = unoauto_gate_powers();
        want_obs = (p & UNOAUTO_P_OBSERVE) != 0;
        want_drv = (p & UNOAUTO_P_DRIVE)   != 0;
        want_sys = (p & UNOAUTO_P_SYSTEM)  != 0;
    }

    for (;;) {
        int cy = 8, armed = unoauto_gate_armed();
        unoui_widget *w;

        unoui_window_init(&win, "Remote Control", x, y, W, H);
        unoui_add_label(&win, pad, cy, armed ? "Remote control is ON."
                                             : "Remote control is off."); cy += fh + 4;
        unoui_add_label(&win, pad, cy, k_l1); cy += fh + 2;
        unoui_add_label(&win, pad, cy, k_l2); cy += fh + 8;

        w = unoui_add_check(&win, 12, cy, k_obs, want_obs);
        w->id = ID_R_OBSERVE; cy += fh + 8;
        w = unoui_add_check(&win, 12, cy, k_drv, want_drv);
        w->id = ID_R_DRIVE; cy += fh + 8;
        w = unoui_add_check(&win, 12, cy, k_sys, want_sys);
        w->id = ID_R_SYSTEM; cy += fh + 12;

        if (armed) {
            ip_str(addr, (int)sizeof addr);
            snprintf(tokline, sizeof tokline, "PIN:  %s", unoauto_gate_token());
            unoui_add_label(&win, pad, cy, "Connect to this address and enter the PIN:");
            cy += fh + 4;
            unoui_add_label(&win, 12, cy, addr);    cy += fh + 4;
            unoui_add_label(&win, 12, cy, tokline); cy += fh + 8;
        } else {
            unoui_add_label(&win, pad, cy, "Nothing is listening until you turn this on.");
            cy += 3 * (fh + 4) + 4;             /* keep the buttons in one place */
        }

        { unoui_widget *b;
          int bw = armed ? b_on : (fb_text_w("Turn on") + 26);
          b = unoui_add_button(&win, pad, cy, bw,
                               armed ? "Update access" : "Turn on", UI_F_DEFAULT);
          b->id = ID_R_ENABLE;
          if (armed) { b = unoui_add_button(&win, pad + bw + 8, cy, b_off,
                                            "Turn off", 0);
                       b->id = ID_R_DISABLE; }
          b = unoui_add_button(&win, inner - pad - b_cls, cy, b_cls, "Close", 0);
          b->id = ID_CLOSE;
          cy += bh + 8;
        }
        if (status[0]) { static char st[128];
            unoui_add_label(&win, pad, cy,
                            fit_px(st, sizeof st, status, inner - 2 * pad)); }

        sheet_audit(&win);
    modal_begin(&win);
        for (;;) {
            unoui_action a;
            if (!modal_frame(&a) || !a.changed) continue;
            if (a.id == ID_R_OBSERVE) { want_obs = a.value; continue; }
            if (a.id == ID_R_DRIVE)   { want_drv = a.value; continue; }
            if (a.id == ID_R_SYSTEM)  { want_sys = a.value; continue; }
            if (a.id == ID_CLOSE || a.id == ID_CANCEL || a.kind == UI_ACT_CLOSE) return;
            if (a.id == ID_R_DISABLE) {
                unoauto_gate_disarm("turned off at the console");
                snprintf(status, sizeof status, "Remote control is off.");
                break;
            }
            if (a.id == ID_R_ENABLE || a.id == ID_OK) {
                unsigned want = (want_obs ? UNOAUTO_P_OBSERVE : 0u)
                              | (want_drv ? UNOAUTO_P_DRIVE   : 0u)
                              | (want_sys ? UNOAUTO_P_SYSTEM  : 0u);
                unsigned got;
                usec_session_t cs;
                if (!want) { snprintf(status, sizeof status,
                                      "Tick at least one kind of access."); break; }

                /* Arming needs a bound session, and there may not be one: the
                 * boot login gate is the only other place that binds, and on a
                 * machine whose accounts were created after boot it has already
                 * run.  Telling the user to "sign in" while offering no way to
                 * do it is a dead end (found on the ZimaBlade 2026-08-03), so
                 * offer the sheet here and KEEP the session - exactly what the
                 * boot gate does; from here on it is the shell's session. */
                cs = unosec_current_session();
                if (!cs || !unosec_session_valid(cs)) {
                    if (unosec_account_list(0, 0) <= 0) {
                        snprintf(status, sizeof status,
                                 "Create a user account first (Manage accounts).");
                        break;
                    }
                    if (cred_sheet("Sign in", "Sign in to turn remote control on.",
                                   0, 0, verify_interactive) != ID_OK) {
                        snprintf(status, sizeof status, "Sign-in cancelled.");
                        break;
                    }
                }
                /* This is the escalation.  The consent sheet (if any) draws
                   over us, which is fine - it is its own modal ui. */
                got = unoauto_gate_arm(want, "ui:remote-panel");
                if (!got)
                    snprintf(status, sizeof status,
                             "Not allowed. Sign in as a user with permission.");
                else if (got != want)
                    snprintf(status, sizeof status,
                             "Partly granted - some access was refused.");
                else
                    snprintf(status, sizeof status, "Remote control is on.");
                break;
            }
        }
    }
}
