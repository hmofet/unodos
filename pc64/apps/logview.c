/* LogView - the system log, as a unoui-class module (APPS\LOGVIEW.UNO).
 *
 * Contract: pc64/UNOLOG.md.  This replaces the PYAPP first shipped for this
 * subsystem.  A PYAPP needed no shell wiring, which kept unolog inside one
 * lane, but it was the wrong home for a system utility on two counts: there is
 * exactly ONE EX_PYAPP slot, so opening the log evicted whatever Python app was
 * running, and the window title came from the FILE name (pyrt.c derives it from
 * the path), so it read "LOGVIEW.UNO".
 *
 * Reads the RING, not the file: the ring is what is current and the file is a
 * copy that lags it by up to a flush interval.  What the ring has already lost
 * to wrapping is genuinely gone from this view, and the footer says how many
 * records exist against how many can still be seen - a viewer that silently
 * shows a subset teaches you to trust a complete-looking list that is not one.
 */
#include "uno_uuiapp.h"
#include "uno_appdesc.h"
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_font.h"
#include "../unolog.h"
#include <string.h>

void pc64_shell_dirty(void);
const struct unoui_theme *pc64_shell_theme(void);
static const struct unoui_theme *TH(void) { return pc64_shell_theme(); }

#define ROW_H 14

static unoui_window *g_win;
static int  g_top;                  /* offset from the ring's oldest, NOT a seq:
                                     * the ring wraps, and a stored seq would
                                     * scroll under the reader as it did */
static int  g_follow = 1;
static int  g_fac = -1;             /* -1 = every facility */
static int  g_rows = 20;
static unsigned long g_seen;

/* buttons, laid out from the canvas rect every draw so a resize cannot leave
 * the hit targets somewhere other than the labels */
enum { B_LESS, B_MORE, B_FOLLOW, B_FAC, B_WRITE, B_SINK, B_N };
static unoui_rect g_btn[B_N];

/* No number formatter is exported to modules, and pulling in snprintf for five
 * counters is not worth it. */
static char *u2s(unsigned long v, char *out)
{
    char tmp[12];
    int t = 0, i = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return out; }
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t) out[i++] = tmp[--t];
    out[i] = 0;
    return out;
}

static int in_rect(unoui_rect r, int x, int y)
{ return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

/* Severity picks the colour: scanning a log is looking for the bad line, and a
 * wall of one colour makes that a reading exercise rather than a glance. */
static fb_px sev_col(int sev, const unoui_palette *p)
{
    if (sev <= LOG_ERR)     return FB_RGB(200, 40, 40);
    if (sev == LOG_WARNING) return FB_RGB(190, 110, 0);
    if (sev >= LOG_DEBUG)   return p->text_dim;
    return p->text;
}

static void btn_draw(unoui_rect r, const char *label, int on)
{
    const unoui_palette *p = &TH()->pal;
    fb_fill_rect(r.x, r.y, r.w, r.h, on ? p->accent : p->face);
    fb_frame_rect(r.x, r.y, r.w, r.h, p->dark);
    fb_text(r.x + 6, r.y + (r.h - uno_font_height_px(0, 12)) / 2 + 1,
            label, on ? p->accent_text : p->face_text, -1);
}

static void lv_draw(unoui_widget *w, unoui_rect c, void *ctx)
{
    const unoui_palette *p = &TH()->pal;
    unsigned long first = unolog_first(), next = unolog_next(), seq;
    int y, shown = 0, total, i, bx;
    char line[256];
    (void)w; (void)ctx;

    fb_fill_rect(c.x, c.y, c.w, c.h, p->win_bg);

    g_rows = (c.h - 56) / ROW_H;
    if (g_rows < 1) g_rows = 1;

    total = (int)(next - first);
    /* Recomputed every draw rather than on the keypress that set it: the ring
     * keeps moving, so a follow mode that only jumped when you pressed
     * something would drift off the end while you watched it. */
    if (g_follow) g_top = total - g_rows > 0 ? total - g_rows : 0;
    if (g_top > total - 1) g_top = total > 0 ? total - 1 : 0;
    if (g_top < 0) g_top = 0;

    y = c.y + 4;
    for (seq = first + (unsigned long)g_top; seq < next && shown < g_rows; seq++) {
        unolog_rec r;
        if (!unolog_get(seq, &r)) continue;
        if (g_fac >= 0 && r.fac != g_fac) continue;
        unolog_format(&r, line, (int)sizeof line);
        fb_text(c.x + 6, y, line, sev_col(r.sev, p), -1);
        y += ROW_H;
        shown++;
    }
    if (!shown)
        fb_text(c.x + 6, c.y + 4, "(nothing at this level yet)",
                p->text_dim, -1);

    /* ---- footer --------------------------------------------------------- */
    {
        int fy = c.y + c.h - 50;
        char s[200];
        int n;
        fb_fill_rect(c.x, fy, c.w, 50, p->face);
        fb_hline(c.x, fy, c.w, p->dark);

        strcpy(s, "keep <= ");           n = (int)strlen(s);
        strcpy(s + n, unolog_sev_name(unolog_level())); n += (int)strlen(s + n);
        strcpy(s + n, "    send <= ");   n += 12;
        strcpy(s + n, unolog_sev_name(unolog_remote_level())); n += (int)strlen(s + n);
        if (unolog_remote_host()[0]) {
            strcpy(s + n, " to ");       n += 4;
            strcpy(s + n, unolog_remote_host());
        } else { strcpy(s + n, " (no server)"); }
        fb_text(c.x + 6, fy + 4, s, p->text, -1);

        /* The ring is finite and this is a window onto it. Saying so is the
         * difference between a viewer you can trust and one you cannot. */
        {   char num[24];
            strcpy(s, "records ");                       n = (int)strlen(s);
            u2s((unsigned long)next, num);               strcpy(s + n, num); n += (int)strlen(num);
            strcpy(s + n, ", in memory ");               n += 12;
            u2s((unsigned long)(next - first), num);     strcpy(s + n, num); n += (int)strlen(num);
            strcpy(s + n, ", dropped ");                 n += 10;
            u2s(unolog_dropped(), num);                  strcpy(s + n, num); n += (int)strlen(num);
            strcpy(s + n, ", sent ");                    n += 7;
            u2s(unolog_sent(), num);                     strcpy(s + n, num); n += (int)strlen(num);
            strcpy(s + n, ", received ");                n += 11;
            u2s(unolog_received(), num);                 strcpy(s + n, num);
            fb_text(c.x + 6, fy + 18, s, p->text_dim, -1);
        }

        bx = c.x + 6;
        for (i = 0; i < B_N; i++) {
            static const short kW[B_N] = { 44, 44, 66, 78, 52, 74 };
            g_btn[i].x = bx; g_btn[i].y = fy + 32;
            g_btn[i].w = kW[i]; g_btn[i].h = 16;
            bx += kW[i] + 4;
        }
        btn_draw(g_btn[B_LESS],  "Less", 0);
        btn_draw(g_btn[B_MORE],  "More", 0);
        btn_draw(g_btn[B_FOLLOW], g_follow ? "Following" : "Paused", g_follow);
        btn_draw(g_btn[B_FAC],
                 g_fac < 0 ? "All" : unolog_fac_name(g_fac), g_fac >= 0);
        btn_draw(g_btn[B_WRITE], "Write", 0);
        btn_draw(g_btn[B_SINK],
                 unolog_listening() ? "Sink on" : "Sink off", unolog_listening());
    }
}

/* Persisted immediately rather than behind a Save button: writing four lines of
 * config costs nothing next to rebooting into a level you thought you had set. */
static void level_step(int by)
{
    int l = unolog_level() + by;
    if (l < 0) l = 0;
    if (l > LOG_DEBUG) l = LOG_DEBUG;
    unolog_set_level(l);
    unolog_save_cfg();
}

static int lv_event(unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    (void)w; (void)ctx;
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    if (in_rect(g_btn[B_LESS], e->x, e->y))   { level_step(-1); pc64_shell_dirty(); return 1; }
    if (in_rect(g_btn[B_MORE], e->x, e->y))   { level_step(+1); pc64_shell_dirty(); return 1; }
    if (in_rect(g_btn[B_FOLLOW], e->x, e->y)) { g_follow = !g_follow; pc64_shell_dirty(); return 1; }
    if (in_rect(g_btn[B_FAC], e->x, e->y)) {
        if (++g_fac >= UNOLOG_NFAC) g_fac = -1;
        pc64_shell_dirty(); return 1;
    }
    if (in_rect(g_btn[B_WRITE], e->x, e->y))  { unolog_flush(); pc64_shell_dirty(); return 1; }
    if (in_rect(g_btn[B_SINK], e->x, e->y)) {
        unolog_set_listen(!unolog_listening());
        unolog_save_cfg();
        pc64_shell_dirty(); return 1;
    }
    return 0;
}

static int lv_key(int uni, int scan, int ctrl)
{
    (void)ctrl;
    switch (scan) {
    case 0x01: g_follow = 0; g_top -= 1; break;              /* Up    */
    case 0x02: g_follow = 0; g_top += 1; break;              /* Down  */
    case 0x09: g_follow = 0; g_top -= g_rows; break;         /* PgUp  */
    case 0x0A: g_follow = 0; g_top += g_rows; break;         /* PgDn  */
    case 0x05: g_follow = 0; g_top = 0; break;               /* Home  */
    case 0x06: g_follow = 1; break;                          /* End   */
    default:
        if (uni == '-')                 level_step(-1);
        else if (uni == '+' || uni == '=') level_step(+1);
        else if (uni == 'f' || uni == 'F') g_follow = !g_follow;
        else if (uni == 't' || uni == 'T') { if (++g_fac >= UNOLOG_NFAC) g_fac = -1; }
        else if (uni == 'w' || uni == 'W') unolog_flush();
        else return 0;
    }
    pc64_shell_dirty();
    return 1;
}

/* Redraw when the log moves, and ONLY while following: a reader who scrolled
 * back is reading, and repainting under them drags the text away mid-sentence. */
static void lv_frame(void)
{
    unsigned long n;
    if (!g_follow) return;
    n = unolog_next();
    if (n == g_seen) return;
    g_seen = n;
    pc64_shell_dirty();
}

static unoui_canvas g_canvas = { lv_draw, lv_event, 0 };

static void lv_build(unoui_window *win)
{
    const unoui_metrics *m = &TH()->m;
    int aw, ah;
    g_win = win;
    aw = fb_width() - 140;  if (aw > 760) aw = 760; if (aw < 420) aw = 420;
    ah = fb_height() - 130; if (ah > 480) ah = 480; if (ah < 260) ah = 260;
    unoui_window_init(win, "System Log", 44, 34,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      ah + m->title_h + 2 * m->pad + m->frame_w);
    unoui_add_canvas(win, 0, 0, aw, ah, &g_canvas);
    win->flags |= UI_WIN_RESIZE;
}

static void lv_opened(void) { g_follow = 1; g_seen = 0; }
static int  lv_canvas_index(void) { return 0; }

/* what the shell shows for this app, carried in the module (uno_appdesc.h) */
UNO_APP_DESC("id: logview\n"
             "name: System Log\n"
             "short: Log\n"
             "icon: sys\n"
             "cat: system\n"
             "rank: 70\n");

static const UnoUuiApp kLogView = {
    UNO_UUIAPP_ABI, "System Log",
    lv_build, 0, lv_key, lv_frame, lv_opened, 0, lv_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved) { (void)reserved; return &kLogView; }
