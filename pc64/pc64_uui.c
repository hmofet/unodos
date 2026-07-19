/* ===========================================================================
 * UnoDOS/pc64 - the unoui SHELL (built with -DUNO_UUI in place of unodos.c).
 *
 * Makes the cross-platform unoui toolkit the whole UI: a themed desktop +
 * window manager + retained-mode widgets. A persistent LAUNCHER opens app
 * windows on demand (raise if already open, Ctrl-W to close). unoui owns the
 * windows, focus, dragging, menus, widgets and rendering into `fb`;
 * uno_pc64_present() scales `fb` to the panel.
 *
 * The frame is only redrawn/presented when something changed (input, or the
 * caret blink) - idle frames touch no VRAM, so the desktop is rock-steady and
 * a drag only rewrites the moving window's rows (no full-screen churn).
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "mac_compat.h"      /* FB_W/FB_H + uno_pc64_* + FSOpen/... */
#include "pc64_uui_apps.h"   /* the legacy-app bridge (paint, tracker, music) */
#include "pc64_games.h"      /* native unoui games (Dostris, ...) */
#include "pc64_browser.h"    /* the web browser (native windowed canvas) */
#include "pc64_icons.h"      /* per-app icon artwork */
#include "pc64_font.h"       /* TrueType text engine (system font) */
#include "unosound.h"        /* UnoSound live sequencer (game/app audio) */
#include "xhci.h"            /* USB host controller (gated -DUNO_XHCI) */
#include "ax88179.h"         /* USB Ethernet adapter (ASIX) */
#include "i2c_hid.h"         /* native trackpad status/diag (System readout) */
#ifdef UNO_ACPI
#include "acpi_power.h"      /* unoacpi: AML battery/lid (portable consumer API) */
#include "acpi_host.h"       /* pc64 bring-up status (RSDP) for the System readout */
#include "snd_pcm.h"         /* PCM audio (HDA/AC'97): the Volume slider target */
#endif
#include "installer.h"       /* install to a local disk (the Install app) */
#include "blkdev.h"          /* native block layer (System readout) */
#include "fat.h"             /* native FAT mounts (System readout) */
#include <string.h>

/* ---- themes (dropdown + live re-skin) ---------------------------------- */
static const struct { const char *name; const struct unoui_theme *theme; } kThemes[] = {
    { "Aurora Light", &theme_aurora_light }, { "Aurora Dark", &theme_aurora_dark },
    { "UnoDOS",    &theme_unodos  }, { "Mac OS 7",    &theme_macos7 },
    { "Mac Plus",  &theme_macplus }, { "Windows 3.1", &theme_win31  },
    { "Amiga",     &theme_amiga   }, { "C64",         &theme_c64    },
    { "Apple II",  &theme_apple2  }, { "NeXTSTEP",    &theme_next   }
};
#define NTHEMES ((int)(sizeof kThemes / sizeof kThemes[0]))
static const char *kThemeNames[NTHEMES];

/* ---- apps --------------------------------------------------------------- *
 * The first NNATIVE are unoui-native (built from widgets). The rest are the
 * migrated legacy apps (games / paint / tracker / music), each hosted in a
 * canvas via the pc64_uui_apps bridge; app index a>=NNATIVE maps to legacy
 * index a-NNATIVE. */
enum { APP_CTRL, APP_EDIT, APP_FILES, APP_SYS, APP_CLOCK, APP_DEMO, APP_SETUP, NNATIVE };
#define NEXTRA 2                          /* extra native apps beyond the bridge */
#define EX_RUNNER  (NNATIVE + UNOAPP_COUNT)       /* Runner3D: shell app index    */
#define EX_BROWSER (NNATIVE + UNOAPP_COUNT + 1)   /* Browser: shell app index     */
#define NAPPS  (NNATIVE + UNOAPP_COUNT + NEXTRA)
#define APP_TBAR 18                       /* legacy apps' own title-bar height */
static const char *kAppNames[NNATIVE] =
    { "Control Panel", "Editor", "Files", "System", "Clock", "Canvas", "Install" };

/* taskbar height follows the active font (26 px under the classic 8px font) */
static int tb_h(void) { int h = fb_text_h() + 12; return h < 26 ? 26 : h; }
#define TASKH (tb_h())

static unoui_ui     UI;

/* the live theme, for theme-aware icon recolouring (pc64_icons.c) */
const struct unoui_theme *pc64_shell_theme(void) { return UI.theme; }

static unoui_window g_launch;             /* app menu, opened by the Start button */
static unoui_window g_desk;               /* bare/bottom: the desktop-icon layer */
static unoui_window g_task;               /* bare/top: the taskbar */
static unoui_window g_win[NAPPS];
static int          g_built[NAPPS], g_open[NAPPS];
static int          g_dirty = 1;

/* ---- lid-close deep-idle sleep (ACPI) -------------------------------------
 * Mirrors the Writer's Unlock pattern the contract names as the worked example:
 * poll acpi_lid_event() (cached ~1 Hz), CLOSE -> blank the screen + low-power
 * idle, OPEN (or any key) -> wake + full repaint. Read-only ACPI, no GPE/SCI.
 * Inert on machines with no lid (lid_state -1 -> the edge detector never fires)
 * and when ACPI isn't built in. */
static int g_asleep;
static int g_lidsleep = 1;       /* lid-close enters sleep (on by default) */
#ifdef UNO_ACPI
#ifdef UNO_DBGCON
static void slp_dbg(const char *s){ while(*s) __asm__ volatile("outb %0,%1"::"a"((unsigned char)*s++),"Nd"((unsigned short)0x402)); }
#else
static void slp_dbg(const char *s){ (void)s; }
#endif
static void uui_sleep_enter(void)
{
    g_asleep = 1;
    slp_dbg("SLEEP: lid closed -> screen off\n");
    uno_seq_stop();              /* silence any music/SFX */
    fb_clear(FB_RGB(0, 0, 0));   /* blank the framebuffer */
    uno_pc64_present();
}
static void uui_sleep_wake(void)
{
    g_asleep = 0;
    slp_dbg("WAKE: repaint\n");
    g_dirty = 1;                 /* force a full repaint next frame */
}
#endif

/* one canvas per legacy app; ctx carries its legacy index */
static unoui_canvas g_lcanvas[UNOAPP_COUNT];
static int          g_lidx[UNOAPP_COUNT];

static const char *kNativeShort[NNATIVE] =
    { "Control", "Editor", "Files", "System", "Clock", "Canvas", "Install" };
static const char *app_name(int a)
{ return a == EX_RUNNER ? "Runner3D" : a == EX_BROWSER ? "Browser"
       : a < NNATIVE ? kAppNames[a] : unoapp_name(a - NNATIVE); }
static const char *app_short(int a)
{ return a == EX_RUNNER ? "Runner" : a == EX_BROWSER ? "Browser"
       : a < NNATIVE ? kNativeShort[a] : unoapp_name(a - NNATIVE); }

/* widget ids */
enum { ID_THEME = 1, ID_RES, ID_DARK, ID_WRAP, ID_VOL, ID_SCALE, ID_ABOUT,
       ID_MENU, ID_BODY, ID_NAME, ID_SAVE, ID_OPEN, ID_NEWF, ID_FILES, ID_FMT,
       ID_FULL, ID_DATE, ID_TIME, ID_SETDT, ID_FONT, ID_CAL, ID_EFONT, ID_ALITE,
       ID_ILIST, ID_IDEF, ID_IRESCAN, ID_IGO, ID_LIDSLP,
       ID_START = 90, ID_SHUTDOWN = 91, ID_RESTART = 92,
       ID_LAUNCH0 = 100,                  /* desktop icons + launcher: +app     */
       ID_TASK0   = 200 };                /* taskbar window buttons: +app       */

/* shell status buffers */
static char g_res_str[12][14]; static const char *g_res_items[12]; static int g_res_n;
static char g_clock[40] = "Uptime 0 s";
static char g_batt[12];      /* tray battery chip, "" = no battery reported */

/* UI scale choices (percent) - drives uno_font_set_ui_scale + a shell rebuild */
static const char *g_scale_items[] = { "100%", "125%", "150%", "200%" };
static const int   g_scale_pcts[]  = { 100, 125, 150, 200 };
#define NSCALES 4

static void build_res_items(void)
{
    int i, n = uno_pc64_res_count(); if (n > 12) n = 12;
    for (i = 0; i < n; i++) {
        short w, h, z; Boolean act; char *o = g_res_str[i]; int j = 0;
        uno_pc64_res_get(i, &w, &h, &z, &act);
        { int v = w, k = 0; char t[8]; if (!v) t[k++]='0'; while (v){t[k++]='0'+v%10;v/=10;}
          while (k) o[j++] = t[--k]; }
        o[j++] = 'x';
        { int v = h, k = 0; char t[8]; if (!v) t[k++]='0'; while (v){t[k++]='0'+v%10;v/=10;}
          while (k) o[j++] = t[--k]; }
        o[j] = 0; g_res_items[i] = o;
    }
    g_res_n = n;
}

/* ---- app window builders ------------------------------------------------- *
 * Layouts flow rows from font-derived metrics (ui_field_h/ui_ctl_h/fb_text_h),
 * so they stay aligned under any font or UI scale. */
static unoui_widget *g_sp_h, *g_sp_mi;      /* time spinners (date = calendar) */

static const char *g_font_items[6];
static int         g_font_n;
static void build_font_items(void)
{
    int i, nf = uno_font_count();
    g_font_items[0] = "System (mono)";
    for (i = 0; i < nf && i < 5; i++) g_font_items[1 + i] = uno_font_name(i);
    g_font_n = 1 + (nf > 5 ? 5 : nf);
}

/* window height for `content_h` px of content under the current theme */
static int win_h_for(int content_h)
{
    const unoui_metrics *m = &UI.theme->m;
    return content_h + m->title_h + 2 * m->pad + m->frame_w;
}

static void build_ctrl(unoui_window *w)
{
    unoui_widget *x; int i;
    int hh = 0, mi = 0;
    int fh = fb_text_h(), ch = ui_field_h(), bh = ui_ctl_h();
    int row = ch + 8, y = 4, lw = fb_text_w("Resolution:") + 12, cw = 340;
    int lofs = (ch - fh) / 2;                   /* label centred beside a control */
    for (i = 0; i < NTHEMES; i++) kThemeNames[i] = kThemes[i].name;
    build_res_items();
    build_font_items();
    unoui_window_init(w, "Control Panel", 150, 24, 1, 1);   /* sized below */
    unoui_add_label(w, 8, y + lofs, "Theme:");
    x = unoui_add_dropdown(w, lw, y, cw - lw - 8, kThemeNames, NTHEMES, 0); x->id = ID_THEME;
    y += row;
    unoui_add_label(w, 8, y + lofs, "Resolution:");
    x = unoui_add_dropdown(w, lw, y, cw - lw - 8, g_res_items, g_res_n, 0); x->id = ID_RES;
    y += row;
    unoui_add_label(w, 8, y + lofs, "Font:");
    x = unoui_add_dropdown(w, lw, y, cw - lw - 8, g_font_items, g_font_n, uno_font_active()+1); x->id = ID_FONT;
    y += row;
    unoui_add_label(w, 8, y + lofs, "UI scale:");
    { int cur = 0; for (i = 0; i < NSCALES; i++) if (g_scale_pcts[i] == uno_font_ui_scale()) cur = i;
      x = unoui_add_dropdown(w, lw, y, 100, g_scale_items, NSCALES, cur); x->id = ID_SCALE; }
    y += row;
    unoui_add_check(w, 8, y, "Dark mode", 0);   w->w[w->nw-1].id = ID_DARK;
    unoui_add_check(w, cw / 2, y, "Aurora lite", unoui_aurora_lite);
    w->w[w->nw-1].id = ID_ALITE;
    y += fh + 10;
    unoui_add_check(w, 8, y, "Lid sleep", g_lidsleep);
    w->w[w->nw-1].id = ID_LIDSLP;
    y += fh + 12;
    unoui_add_label(w, 8, y + lofs, "Volume");
    x = unoui_add_slider(w, lw, y, cw - lw - 8, 0, 100, 70); x->id = ID_VOL;
    y += row;
    unoui_add_sep(w, 8, y, cw - 16); y += 8;
    /* --- clock (firmware RTC): time spinners; the date is set via the
       calendar picker only --- */
    uno_pc64_time(0, 0, 0, &hh, &mi, 0);
    unoui_add_label(w, 8, y + lofs, "Time:");
    g_sp_h  = unoui_add_spinner(w, lw,      y, 52, 0, 23, hh);
    unoui_add_label(w, lw + 56, y + lofs, ":");
    g_sp_mi = unoui_add_spinner(w, lw + 66, y, 52, 0, 59, mi);
    x = unoui_add_button(w, lw + 126, y, 92, "Set time", 0); x->id = ID_SETDT;
    y += row;
    x = unoui_add_button(w, 8, y, 120, "Set date...", 0); x->id = ID_CAL;
    x = unoui_add_button(w, cw - 8 - 96, y, 96, "About", 0); x->id = ID_ABOUT;
    y += bh + 8;
    w->r.w = cw + 2 * UI.theme->m.frame_w + 2 * UI.theme->m.pad;
    w->r.h = win_h_for(y);
    w->min_w = w->r.w; w->min_h = w->r.h;
    w->flags |= UI_WIN_RESIZE;
}

/* Editor (WordPad-style word processor) + Files (real file manager) live in
 * their own translation units; the shell just delegates the build. */
void pc64_write_build(unoui_window *w);
void pc64_files_build(unoui_window *w);
static void build_edit(unoui_window *w)  { pc64_write_build(w); }
static void build_files(unoui_window *w) { pc64_files_build(w); }
/* tiny no-libc string builders for the diagnostics line */
static char *ap_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *ap_int(char *p, int v) { char t[12]; int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; } if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; } while (n) *p++ = t[--n]; return p; }
static char *ap_hex(char *p, int v) { const char *h = "0123456789abcdef";
    *p++ = '0'; *p++ = 'x'; *p++ = h[(v >> 4) & 0xF]; *p++ = h[v & 0xF]; return p; }
static char *ap_hex16(char *p, unsigned v) { const char *h = "0123456789abcdef"; int i;
    *p++ = '0'; *p++ = 'x'; for (i = 12; i >= 0; i -= 4) *p++ = h[(v >> i) & 0xF]; return p; }

static char g_tp1[64], g_tp2[64];
static void build_tpstat(void)
{
    int nb = 0, nc = 0, pr = 0, ad = 0, pa = 0; char *p;
    uno_i2c_hid_status(&nb, &nc, &pr, &ad, &pa);
    p = ap_str(g_tp1, "Trackpad I2C: ");
    p = ap_int(p, nc); p = ap_str(p, " DW ctrl / ");
    p = ap_int(p, nb); p = ap_str(p, " bars"); *p = 0;
    p = ap_str(g_tp2, "  HID device: ");
    if (pr) { p = ap_str(p, "UP  addr "); p = ap_hex(p, ad);
              p = ap_str(p, pa ? "  parsed" : "  UNPARSED"); }
    else if (nc) {                               /* found controllers, no device */
        int sa = 0; unsigned ab = 0; uno_i2c_hid_diag(&sa, &ab);
        p = ap_str(p, sa ? "no HID (bus ok, abrt " : "no HID (no ACK, abrt ");
        p = ap_hex16(p, ab); *p++ = ')';
    } else  { p = ap_str(p, "no controller (ACPI-only?)"); }
    *p = 0;
}

static char g_usb[80], g_usb2[80];
static void build_usbstat(void)
{
    int pr = 0, np = 0, nd = 0; unsigned e = 0; char *p;
    uno_xhci_status(&pr, &np, &nd, &e);
    g_usb2[0] = 0;
    if (pr && nd == 0 && np > 0) {              /* enumeration failed - show why */
        int sl=0, ad=0, de=0, sp=0, dc=0; unsigned sts=0, ev0=0; char *q = g_usb2;
        uno_xhci_diag(&sl, &ad, &de, &sp);
        uno_xhci_diag2(&sts, &ev0, &dc);
        q = ap_str(q, "  sl="); q = ap_int(q, sl);
        q = ap_str(q, " ad="); q = ap_int(q, ad);
        q = ap_str(q, " de="); q = ap_int(q, de);
        q = ap_str(q, " sts="); q = ap_hex16(q, sts); *q = 0;
    }
    p = ap_str(g_usb, "USB xHCI: ");
    if (pr) { p = ap_str(p, "up, "); p = ap_int(p, np); p = ap_str(p, " port, ");
              p = ap_int(p, nd); p = ap_str(p, " dev");
              if (nd > 0) { const uno_usb_dev *d = uno_xhci_dev(0);
                  p = ap_str(p, "  "); p = ap_hex16(p, d->vendor);
                  p = ap_str(p, ":"); p = ap_hex16(p, d->product); } }
    else if (e) { p = ap_str(p, "init failed at stage "); p = ap_int(p, (int)e); }
    else        { p = ap_str(p, "no controller"); }
    *p = 0;
    /* AX88179 USB Ethernet (only shown when an adapter was seen) */
    { int fnd=0, bnd=0, lnk=0; unsigned short vid=0, pid=0;
      ax88179_status(&fnd, &bnd, &lnk, &vid, &pid);
      if (fnd) { char *r = ap_str(g_usb2, "  ASIX ");
          r = ap_hex16(r, vid); r = ap_str(r, ":"); r = ap_hex16(r, pid);
          r = ap_str(r, bnd ? "  bound  link " : "  found (not bound)");
          if (bnd) r = ap_str(r, lnk ? "up" : "down"); *r = 0; } }
}

/* audio: which backend the Sound Manager voice reaches, for System */
static char g_snd[64];
static void build_sndstat(void)
{
    char *p = ap_str(g_snd, "Audio: ");
    if (uno_snd_active()) { p = ap_str(p, uno_snd_name());
                            p = ap_str(p, "  (PCM 48k s16 stereo)"); }
    else                    p = ap_str(p, "PC speaker (PIT ch2)");
    *p = 0;
}

/* ACPI (unoacpi AML interpreter): bring-up + battery/lid, for System */
static char g_acpi1[80], g_acpi2[80];
static void build_acpistat(void)
{
#ifdef UNO_ACPI
    acpi_power_diag d;
    char *p;
    acpi_power_get_diag(&d);
    p = ap_str(g_acpi1, "ACPI AML: ");
    if (!uno_acpi_rsdp())      p = ap_str(p, "no RSDP");
    else if (!d.ok)            p = ap_str(p, "bring-up failed");
    else {
        p = ap_str(p, "up, "); p = ap_int(p, (int)d.ns_nodes);
        p = ap_str(p, " nodes  bat ");
        if (d.bat_percent >= 0) { p = ap_int(p, d.bat_percent); *p++ = '%'; }
        else                      p = ap_str(p, "--");
        p = ap_str(p, "  lid ");
        p = ap_str(p, d.lid_state == 1 ? "open" :
                      d.lid_state == 0 ? "closed" : "--");
    }
    *p = 0;
    p = ap_str(g_acpi2, "  EC ");
    p = ap_str(p, d.ec_present ? "up" : "--");
    p = ap_str(p, " rd="); p = ap_int(p, d.ec_reads);
    p = ap_str(p, " tmo="); p = ap_int(p, d.ec_timeouts);
    p = ap_str(p, "  arena "); p = ap_int(p, (int)(d.arena_peak >> 10));
    p = ap_str(p, "/"); p = ap_int(p, (int)(d.arena_total >> 10));
    p = ap_str(p, " KB"); *p = 0;
#else
    ap_str(g_acpi1, "ACPI AML: not built in")[0] = 0;
    g_acpi2[0] = 0;
#endif
}

/* Native storage: our block registry + FAT mounts.  Our FS code does all
 * partition scanning + FAT read/write; the sector TRANSPORT is firmware Block
 * IO while attached, and the native AHCI driver once detached (M3). */
static char g_nat[80];
static void build_natstat(void)
{
    int nblk = uno_blk_count(), i, nnat = 0, nfat = uno_fat_volumes();
    char *p;
    const char *nat0 = 0;                /* first native driver's name (ahci0/nvme0) */
    int uno_pc64_detached(void);
    for (i = 0; i < nblk; i++) { uno_bdev *b = uno_blk_get(i);
        if (b && b->native) { nnat++; if (!nat0) nat0 = b->name; } }
    p = ap_str(g_nat, uno_pc64_detached() ? "DETACHED (native): "
                                          : "Native FS: ");
    if (nnat) { p = ap_str(p, nat0); *p++ = ' '; }
    else        p = ap_str(p, "fw-sect ");
    p = ap_int(p, nblk); p = ap_str(p, " disk");
    p = ap_str(p, "  FAT vols "); p = ap_int(p, nfat);
    if (nfat > 0) { p = ap_str(p, " (");
        for (i = 0; i < nfat && i < 3; i++) { const char *l = uno_fat_label(i);
            if (i) *p++ = ',';
            if (l && l[0] && l[0] != ' ') { int k; for (k = 0; k < 11 && l[k] && l[k] != ' '; k++) *p++ = l[k]; }
            else p = ap_str(p, "?"); }
        *p++ = ')'; }
    *p = 0;
}

static void build_sys(unoui_window *w)
{
    int fh = fb_text_h(), lh = fh + 4, y = 4, cw = 420, gx = 8, tx = 20;
    int g0;
    build_tpstat();
    build_usbstat();
    build_natstat();
    build_acpistat();
    build_sndstat();
    unoui_window_init(w, "System", 400, 100, 1, 1);
    /* header */
    unoui_add_label(w, gx, y, "UnoDOS / pc64  -  unoui shell");           y += lh;
    unoui_add_label(w, gx, y, "x86-64 UEFI  -  bare metal  -  10 themes"); y += lh + 8;
    /* grouped hardware readouts: a box per subsystem, rows inside */
#define SYS_GROUP(title, nrows) \
    g0 = y; (void)g0; unoui_add_group(w, gx, y, cw - 2 * gx, (nrows) * lh + fh + 10, title); \
    y += fh + 4
#define SYS_ROW(s) unoui_add_label(w, tx, y, s); y += lh
    SYS_GROUP("Input & USB", 4);
    SYS_ROW(g_tp1);
    SYS_ROW(g_tp2);
    SYS_ROW(g_usb);
    SYS_ROW(g_usb2);
    y += 12;
    SYS_GROUP("Storage", 1);
    SYS_ROW(g_nat);
    y += 12;
    SYS_GROUP("Power & ACPI", 2);
    SYS_ROW(g_acpi1);
    SYS_ROW(g_acpi2);
    y += 12;
    SYS_GROUP("Audio", 1);
    SYS_ROW(g_snd);
    y += 12;
#undef SYS_GROUP
#undef SYS_ROW
    w->r.w = cw + 2 * UI.theme->m.frame_w + 2 * UI.theme->m.pad;
    w->r.h = win_h_for(y);
    w->min_w = 360; w->min_h = 240;
    w->flags |= UI_WIN_RESIZE;
}
static void build_clock(unoui_window *w)
{
    int fh = fb_text_h();
    unoui_window_init(w, "Clock", 420, 30, 220, 1);
    unoui_add_label(w, 10, 8, g_clock);
    unoui_add_label(w, 10, 8 + fh + 6, "(firmware RTC)");
    w->r.h = win_h_for(2 * fh + 20);
}

/* ---- Canvas demo: proves UI_CANVAS (app-drawn region) + unoui_fullscreen -- */
static void canvas_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    int i; (void)w; (void)ctx;
    for (i = 0; i < 8; i++) {                       /* colour bars */
        fb_px c = FB_RGB((i & 1) ? 255 : 0, (i & 2) ? 255 : 0, (i & 4) ? 255 : 0);
        fb_fill_rect(r.x + r.w * i / 8, r.y, r.w / 8 + 1, r.h, c);
    }
    fb_frame_rect(r.x, r.y, r.w, r.h, FB_RGB(255, 255, 255));
    /* caption on a dark backing strip so it reads over the bars and fits */
    fb_fill_rect(r.x + 1, r.y + 1, r.w - 2, 30, FB_RGB(0, 0, 0));
    fb_text(r.x + 6, r.y + 5,  "UI_CANVAS: the app owns",
            FB_RGB(255, 255, 255), FB_RGB(0, 0, 0));
    fb_text(r.x + 6, r.y + 17, "these pixels.",
            FB_RGB(255, 255, 255), FB_RGB(0, 0, 0));
}
static int canvas_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; (void)w; (void)ctx;
    if (e->kind == UI_EV_KEY && e->key == UI_KEY_ESC) {
        unoui_fullscreen(&UI, 0); g_dirty = 1; return 1;    /* exit fullscreen */
    }
    return 0;
}
static unoui_canvas g_canvas = { canvas_draw, canvas_event, 0 };

static void build_demo(unoui_window *w)
{
    unoui_widget *x;
    unoui_window_init(w, "Canvas", 200, 120, 270, 210);
    unoui_add_label(w, 6, 6, "App-drawn canvas (UI_CANVAS):");
    unoui_add_canvas(w, 6, 20, 256, 120, &g_canvas);
    x = unoui_add_button(w, 6, 150, 120, "Fullscreen", UI_F_DEFAULT); x->id = ID_FULL;
}

/* ---- Install: put UnoDOS on a local disk (backend: installer.c) ---------- */
#define INST_MAXT 12
static char        g_inst_item[INST_MAXT][72];
static const char *g_inst_ptr[INST_MAXT];
static int         g_inst_n, g_inst_sel = -1, g_inst_armed;
static char        g_inst_stat[96] = "Select a target, then Install.";
static int         g_inst_default = 1;       /* "Boot UnoDOS by default"       */
static unoui_widget *g_inst_list_w, *g_inst_prog_w;

static void inst_rescan(void)
{
    int i;
    g_inst_n = uno_inst_scan();
    if (g_inst_n > INST_MAXT) g_inst_n = INST_MAXT;
    for (i = 0; i < g_inst_n; i++) {
        strncpy(g_inst_item[i], uno_inst_desc(i), 71); g_inst_item[i][71] = 0;
        g_inst_ptr[i] = g_inst_item[i];
    }
    g_inst_sel = g_inst_n ? 0 : -1; g_inst_armed = 0;
    if (g_inst_list_w) { g_inst_list_w->nitems = g_inst_n; g_inst_list_w->value = g_inst_sel; }
    if (g_inst_prog_w) g_inst_prog_w->value = 0;
    strcpy(g_inst_stat, g_inst_n ? "Select a target, then Install."
                                 : "No install targets found.");
}

static void inst_select(int n)
{
    if (n < 0 || n >= g_inst_n) return;
    g_inst_sel = n; g_inst_armed = 0;
    if (g_inst_list_w) g_inst_list_w->value = n;
    strcpy(g_inst_stat, uno_inst_kind(n) == UNO_INST_DISK
           ? "Destructive: erases that disk. Install asks twice."
           : "Non-destructive: adds \\EFI\\UNODOS + a boot entry.");
}

/* live progress while the copy runs (the shell loop is blocked inside the
 * install call, so paint + present directly - same trick as the Paint drag) */
static void inst_progress(int pct, const char *msg)
{
    char *p = g_inst_stat;
    if (g_inst_prog_w) g_inst_prog_w->value = pct;
    while (*msg && p < g_inst_stat + 78) *p++ = *msg++;
    *p = 0;
    unoui_render_ui(&UI);
    uno_pc64_present();
}

static void inst_go(void)
{
    int k;
    if (g_inst_sel < 0 || g_inst_sel >= g_inst_n) {
        strcpy(g_inst_stat, "Pick a target from the list first.");
        return;
    }
    if (!uno_inst_usable(g_inst_sel)) {
        strcpy(g_inst_stat, "That target cannot be used (see its listing).");
        return;
    }
    k = uno_inst_kind(g_inst_sel);
    if (k == UNO_INST_DISK && !g_inst_armed) {
        g_inst_armed = 1;                    /* destructive: double-confirm    */
        strcpy(g_inst_stat, "ERASES THAT WHOLE DISK - press Install again.");
        return;
    }
    g_inst_armed = 0;
    if (uno_inst_install(g_inst_sel, g_inst_default, inst_progress)) {
        strcpy(g_inst_stat, "Installed. Remove the USB stick and restart.");
        if (g_inst_prog_w) g_inst_prog_w->value = 100;
    } else {
        char *p = ap_str(g_inst_stat, "FAILED: ");
        const char *e = uno_inst_error();
        while (*e && p < g_inst_stat + 90) *p++ = *e++;
        *p = 0;
        if (g_inst_prog_w) g_inst_prog_w->value = 0;
    }
}

static void build_setup(unoui_window *w)
{
    inst_rescan();
    unoui_window_init(w, "Install", 150, 60, 400, 286);
    unoui_add_label(w, 8, 4, "Install UnoDOS to a local disk");
    unoui_add_label(w, 8, 20, "Volumes keep your files; Disks are ERASED.");
    unoui_add_label(w, 8, 34, "Keys: Up/Down pick - I installs - R rescans");
    g_inst_list_w = unoui_add_list(w, 8, 52, 384, 92, g_inst_ptr, g_inst_n, g_inst_sel);
    g_inst_list_w->id = ID_ILIST;
    { unoui_widget *c = unoui_add_check(w, 8, 150, "Boot UnoDOS by default", 1);
      c->id = ID_IDEF; }
    { unoui_widget *b = unoui_add_button(w, 8, 172, 100, "Rescan", 0);  b->id = ID_IRESCAN; }
    { unoui_widget *b = unoui_add_button(w, 292, 172, 100, "Install", 0); b->id = ID_IGO; }
    g_inst_prog_w = unoui_add_progress(w, 8, 200, 384, 0, 100);
    unoui_add_label(w, 8, 218, g_inst_stat);
}

static void (*const g_build[NNATIVE])(unoui_window *) =
    { build_ctrl, build_edit, build_files, build_sys, build_clock, build_demo,
      build_setup };

/* ---- window management -------------------------------------------------- */
static int g_launch_open;
static int g_menu_scroll, g_menu_hot = -1, g_scroll_tmr;   /* Start-menu scroll */
static unoui_window g_cal;                 /* calendar date-picker popup */
static int g_cal_open, g_cal_y = 2026, g_cal_mo = 1, g_cal_sel = 1;
static unoui_rect g_cal_rect;

static void raise_win(unoui_window *win) { unoui_bring_to_front(&UI, win); }

static void remove_win(unoui_window *win)
{
    int i, j;
    for (i = 0; i < UI.nwin; i++) if (UI.win[i] == win) break;
    if (i >= UI.nwin) return;
    for (j = i; j < UI.nwin - 1; j++) UI.win[j] = UI.win[j + 1];
    UI.nwin--;
    if (UI.focus_win >= UI.nwin) UI.focus_win = UI.nwin - 1;
    UI.focus_wi = -1;
}

/* the taskbar background: a bare window draws no chrome, so a non-interactive
 * canvas paints the bar face + top highlight under the buttons. */
/* forward decls (taskbar events fire these, defined below) */
static void toggle_launcher(void);
static void open_app(int a);
static void fmt_clock(int uptime_secs);
int pc64_write_canvas_index(void);    /* pc64_write.c: doc-canvas widget index */
int pc64_files_canvas_index(void);    /* pc64_files.c: pane widget index      */

/* ---- taskbar: a single canvas we draw + hit-test by hand for full control -- *
 * All layout is measured from the live font so the bar stays aligned whatever
 * face/scale is active (draw + hit-test share these functions). */
#define TB_START_X 4
static int tb_logo_sz(void)  { int s = fb_text_h(); return s < 12 ? 12 : s; }
static int tb_start_w(void)  { return 10 + tb_logo_sz() + 6 + fb_text_w("Start") + 10; }
static int tb_chip_w(void)   { int w = 40 + fb_text_w("Manager"); return w < 108 ? 108 : w; }
static int tb_chip_gap(void) { return tb_chip_w() + 4; }
static int tb_chip_x(void)   { return TB_START_X + tb_start_w() + 8; }

/* app index of the focused window, or -1 (used to highlight its taskbar chip) */
static int focused_app(void)
{
    int i;
    if (UI.focus_win < 0 || UI.focus_win >= UI.nwin) return -1;
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == UI.win[UI.focus_win]) return i;
    return -1;
}

/* a raised (or pressed) 3D panel - the shared taskbar-chip / Start look */
static void tb_panel(int x, int y, int w, int h, fb_px face, int pressed)
{
    const unoui_theme *t = UI.theme;
    fb_fill_rect(x, y, w, h, face);
    fb_frame_rect(x, y, w, h, t->pal.dark);          /* crisp outer edge */
    if (pressed) {
        fb_hline(x + 1, y + 1, w - 2, t->pal.shadow);
        fb_vline(x + 1, y + 1, h - 2, t->pal.shadow);
    } else {
        fb_hline(x + 1, y + 1, w - 2, t->pal.light);
        fb_vline(x + 1, y + 1, h - 2, t->pal.light);
        fb_hline(x + 1, y + h - 2, w - 2, t->pal.shadow);
        fb_vline(x + w - 2, y + 1, h - 2, t->pal.shadow);
    }
}

static void taskbar_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = UI.theme;
    int modern = t->m.radius > 0;
    int i, x, act = focused_app(), by = r.y + (modern ? 5 : 3), bh = TASKH - (modern ? 10 : 6);
    int cr = bh/2 > 8 ? 8 : bh/2;                 /* chip corner radius */
    (void)w; (void)ctx;
    if (modern) {                                 /* Aurora: a frosted bar + hairline */
        fb_blend_rect(r.x, r.y, r.w, r.h, t->pal.win_bg, 236);
        fb_blend_rect(r.x, r.y, r.w, 1, t->pal.text_dim, 45);
    } else {
        fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
        fb_hline(r.x, r.y, r.w, t->pal.light);
    }
    /* Start button - accent-coloured, the UnoDOS "One" mark + label, the
       logo+label group centred as a unit within the button */
    { int bx = r.x + TB_START_X, bw = tb_start_w(), fh = fb_text_h();
      int lsz = tb_logo_sz(), grp = lsz + 6 + fb_text_w("Start");
      int gx = bx + (bw - grp) / 2, gy = by + (bh - lsz) / 2;
      if (modern) fb_round_rect(bx, by, bw, bh, cr, t->pal.accent);
      else        tb_panel(bx, by, bw, bh, t->pal.accent, 0);
      pc64_start_logo(gx, gy, lsz, t->pal.accent_text);
      fb_text(gx + lsz + 6, by + (bh - fh) / 2, "Start", t->pal.accent_text, -1); }
    /* one chip per open window: mini icon + name, highlighted if it's active.
       Chips stop before the tray (clock/battery) instead of colliding. */
    x = r.x + tb_chip_x();
    { int cw = tb_chip_w(), fh = fb_text_h(), es = bh - 4 > 16 ? 16 : bh - 4;
      int tray_x = r.x + r.w - (fb_text_w(g_clock) + 16) - 6
                   - (g_batt[0] ? fb_text_w(g_batt) + 20 : 0);
    for (i = 0; i < NAPPS; i++) {
        int d = (i == act) ? 1 : 0;
        unoui_rect eb;
        if (!g_open[i]) continue;
        if (x + cw > tray_x - 4) break;      /* no room left before the tray */
        if (modern) {
            if (d) { fb_round_rect_a(x, by, cw, bh, cr, t->pal.accent, 48, FB_CORNER_ALL);
                     fb_fill_rect(x + 8, by + bh - 2, cw - 16, 2, t->pal.accent); }
            else     fb_round_rect_a(x, by, cw, bh, cr, t->pal.text, 18, FB_CORNER_ALL);
        } else tb_panel(x, by, cw, bh, t->pal.face, d);
        { int dd = modern ? 0 : d;
          eb.x = x + 4 + dd; eb.y = by + (bh - es) / 2 + dd; eb.w = es; eb.h = es;
          pc64_icon_emblem(i, eb);
          fb_set_clip(x + es + 6, by, cw - es - 8, bh);        /* keep the name in the chip */
          fb_text(x + es + 8 + dd, by + (bh - fh) / 2 + dd, app_short(i), t->pal.text, -1);
          fb_set_clip(r.x, r.y, r.w, r.h); }                  /* back to the bar */
        x += tb_chip_gap();
    } }
    /* system tray: battery (when ACPI reports one) + clock chips, right-aligned */
    { int fh = fb_text_h();
      int cw = fb_text_w(g_clock) + 16, cxx = r.x + r.w - cw - 6;
      if (modern) fb_round_rect_a(cxx, by, cw, bh, cr, t->pal.text, 16, FB_CORNER_ALL);
      else        tb_panel(cxx, by, cw, bh, t->pal.field_bg, 1);
      fb_text(cxx + 8, by + (bh - fh) / 2, g_clock, modern ? t->pal.text : t->pal.field_text, -1);
      if (g_batt[0]) {
          int bw = fb_text_w(g_batt) + 16, bx = cxx - bw - 4;
          if (modern) fb_round_rect_a(bx, by, bw, bh, cr, t->pal.text, 16, FB_CORNER_ALL);
          else        tb_panel(bx, by, bw, bh, t->pal.field_bg, 1);
          fb_text(bx + 8, by + (bh - fh) / 2, g_batt, modern ? t->pal.text : t->pal.field_text, -1);
      } }
}

static int taskbar_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    int px, i, x;
    (void)w; (void)ctx;
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    px = e->x - g_task.r.x;
    if (px >= TB_START_X && px < TB_START_X + tb_start_w()) { toggle_launcher(); return 1; }
    x = tb_chip_x();
    for (i = 0; i < NAPPS; i++) {
        if (!g_open[i]) continue;
        if (px >= x && px < x + tb_chip_w()) { open_app(i); return 1; }
        x += tb_chip_gap();
    }
    return 0;
}
static unoui_canvas g_taskcv = { taskbar_draw, taskbar_event, 0 };

/* the taskbar is one canvas; it reads g_open live, so "rebuild" is just a
 * redraw request (kept as a name the open/close paths already call). */
static void rebuild_taskbar(void) { g_dirty = 1; }

static void build_taskbar(void)
{
    unoui_window_init(&g_task, "", 0, FB_H - TASKH, FB_W, TASKH);
    g_task.flags = UI_WIN_BARE | UI_WIN_TOP;
    unoui_add_canvas(&g_task, 0, 0, FB_W, TASKH, &g_taskcv);
}

static void build_desktop(void)
{
    int i, percol, fh = fb_text_h();
    int ich = 34 + fh, pitch = ich + 8, colw = 20 + fb_text_w("MMMMMMMM");
    unoui_window_init(&g_desk, "", 0, 0, FB_W, FB_H - TASKH);
    g_desk.flags = UI_WIN_BARE | UI_WIN_BOTTOM;
    percol = (FB_H - TASKH - 20) / pitch; if (percol < 1) percol = 1;   /* grid */
    for (i = 0; i < NAPPS; i++) {
        int col = i / percol, row = i % percol;
        unoui_widget *ic = unoui_add_icon(&g_desk, 16 + col * colw, 14 + row * pitch,
                                          app_short(i));
        ic->r.w = colw - 20; ic->r.h = ich; /* room for emblem + label         */
        ic->icon = i;                       /* -> pc64_icon_art draws its art  */
        ic->id  = ID_LAUNCH0 + i;
    }
}

/* pull a window fully into the work area (screen minus the taskbar) */
static void clamp_to_workarea(unoui_window *w)
{
    if (w->r.x + w->r.w > FB_W)         w->r.x = FB_W - w->r.w;
    if (w->r.y + w->r.h > FB_H - TASKH) w->r.y = FB_H - TASKH - w->r.h;
    if (w->r.x < 0) w->r.x = 0;
    if (w->r.y < 0) w->r.y = 0;
}

/* ---- legacy apps hosted in a canvas ------------------------------------- */
static void lcanvas_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{ (void)w; unoapp_paint(*(int *)ctx, r); }

static int lcanvas_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; (void)w;
    if (UI.full && e->kind == UI_EV_KEY && e->key == UI_KEY_ESC) {
        unoui_fullscreen(&UI, 0); g_dirty = 1; return 1;   /* leave fullscreen */
    }
    if (unoapp_input(*(int *)ctx, e)) { g_dirty = 1; return 1; }
    return 0;
}

/* the native GAME index for shell app `a`, or -1 (the rest use the bridge).
 * Only Runner3D is native (it drives uno3d directly and has no module
 * counterpart); the classic games run as .UNO modules through the bridge so
 * ALL apps load from storage - the decoupling contract. */
static int app_game(int a)
{
    int g = (a == EX_RUNNER) ? GAME_RUNNER : -1;
    return (g >= 0 && pc64_game_canvas(g)) ? g : -1;
}
/* a mac_compat-bridge app (Music/Tracker/Paint/Network - not a native game/browser) */
static int app_is_bridge(int a)
{ int li = a - NNATIVE; return a >= NNATIVE && li >= 0 && li < UNOAPP_COUNT && app_game(a) < 0; }

static void build_legacy(int a)
{
    int li = a - NNATIVE, aw, ah, g = app_game(a);
    const unoui_metrics *m = &UI.theme->m;
    unoui_canvas *cv;
    if (g >= 0) {                      /* native game: one canvas that scales */
        aw = 320; ah = 240;
        unoui_window_init(&g_win[a], app_name(a), 30, 16,
                          aw + 2*m->frame_w + 2*m->pad, ah + m->title_h + 2*m->pad + m->frame_w);
        unoui_widget_fill(unoui_add_canvas(&g_win[a], 0, 0, aw, ah, pc64_game_canvas(g)));
        g_win[a].flags |= UI_WIN_RESIZE;    /* the game canvas scales to the rect */
        return;
    }
    if (a == EX_BROWSER) {             /* native windowed browser canvas */
        aw = 440; ah = 300;
        unoui_window_init(&g_win[a], app_name(a), 24, 14,
                          aw + 2*m->frame_w + 2*m->pad, ah + m->title_h + 2*m->pad + m->frame_w);
        unoui_widget_fill(unoui_add_canvas(&g_win[a], 0, 0, aw, ah, pc64_browser_canvas()));
        g_win[a].flags |= UI_WIN_RESIZE;    /* text reflows to the new width */
        return;
    }
    cv = &g_lcanvas[li];
    unoapp_size(li, &aw, &ah);
    if (aw < 140) aw = 140;
    if (ah < 100) ah = 100;
    g_lidx[li] = li;
    cv->draw = lcanvas_draw; cv->event = lcanvas_event; cv->ctx = &g_lidx[li];
    /* size the window so its content area == the app's drawable area (the app
     * minus its own title bar, which unoui draws instead). */
    unoui_window_init(&g_win[a], app_name(a), 40, 20,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      (ah - APP_TBAR) + m->title_h + 2 * m->pad + m->frame_w);
    /* bridge apps (Paint/Tracker/Music/Network) draw a FIXED pixel layout, so
     * resizing can't reflow them - leave them non-resizable (no awkward margin).
     * Only the browser + games (which scale to their rect) are resizable. */
    unoui_add_canvas(&g_win[a], 0, 0, aw, ah - APP_TBAR, cv);
}

static void open_app(int a)
{
    if (a < 0 || a >= NAPPS) return;
    if (!g_built[a]) {
        if (a < NNATIVE) g_build[a](&g_win[a]);
        else             build_legacy(a);
        g_built[a] = 1;
    }
    if (!g_open[a]) {
        int g = app_game(a);
        clamp_to_workarea(&g_win[a]);   /* designed pos, but never off-screen */
        if (!unoui_ui_add(&UI, &g_win[a])) {         /* window table full */
#ifdef UNO_DBGCON
            { const char *s = "open_app: window table full\n";
              while (*s) __asm__ volatile ("outb %0, %1"
                             : : "a"((unsigned char)*s++), "Nd"((unsigned short)0x402)); }
#endif
            return;
        }
        g_open[a] = 1;
        if (g >= 0)                 pc64_game_open(g);           /* native game   */
        else if (a == EX_BROWSER)   pc64_browser_open();         /* browser       */
        else if (app_is_bridge(a))  unoapp_open(a - NNATIVE);    /* bridge app    */
        rebuild_taskbar();
    } else raise_win(&g_win[a]);
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }  /* Start-menu closes */
    /* focus the opened window + its canvas (closing the launcher above moved
     * focus to the taskbar, so do this last). */
    { int fi; for (fi = 0; fi < UI.nwin; fi++) if (UI.win[fi] == &g_win[a]) { UI.focus_win = fi; break; } }
    if (a >= NNATIVE) UI.focus_wi = 0;   /* focus the app's canvas for keyboard */
    if (a == APP_EDIT)  { int wi = pc64_write_canvas_index(); if (wi >= 0) UI.focus_wi = wi; }
    if (a == APP_FILES) { int wi = pc64_files_canvas_index(); if (wi >= 0) UI.focus_wi = wi; }
    /* native games scale to any rect, so they can fill the screen (Esc returns).
     * Bridge apps + the browser stay windowed (they draw at a fixed size). */
    if (app_game(a) >= 0) unoui_fullscreen(&UI, &g_win[a]);
    g_dirty = 1;
}

/* Start button: toggle the app-menu launcher window */
static void toggle_launcher(void)
{
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }
    else { g_menu_scroll = 0; g_menu_hot = 0;
           clamp_to_workarea(&g_launch); unoui_ui_add(&UI, &g_launch);
           UI.focus_wi = 0;                    /* focus the menu canvas for keys */
           g_launch_open = 1; }
    g_dirty = 1;
}

/* F2 / Ctrl-Tab: raise the next OPEN app window (skips desktop + taskbar) */
static void cycle_window(void)
{
    int i, cur = -1;
    if (UI.focus_win >= 0 && UI.focus_win < UI.nwin)
        for (i = 0; i < NAPPS; i++) if (&g_win[i] == UI.win[UI.focus_win]) { cur = i; break; }
    for (i = 1; i <= NAPPS; i++) {
        int a = (cur + i) % NAPPS;
        if (g_open[a]) { raise_win(&g_win[a]); g_dirty = 1; return; }
    }
}

static void close_focused(void)
{
    int f = UI.focus_win, i;
    unoui_window *win;
    if (f < 0 || f >= UI.nwin) return;
    win = UI.win[f];
    if (win->flags & UI_WIN_BARE) return;         /* never close desktop/taskbar */
    if (win == &g_launch) { remove_win(&g_launch); g_launch_open = 0; g_dirty = 1; return; }
    if (win == &g_cal)    { remove_win(&g_cal);    g_cal_open = 0;    g_dirty = 1; return; }
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == win) {
        int g = app_game(i);
        g_open[i] = 0;
        if (g >= 0)              pc64_game_close(g);        /* native game teardown */
        else if (app_is_bridge(i)) unoapp_close(i - NNATIVE); /* bridge app        */
        break;
    }
    if (UI.full == win) unoui_fullscreen(&UI, 0);   /* closing a fullscreen game */
    remove_win(win);
    rebuild_taskbar();
    g_dirty = 1;
}

/* ---- Start menu: a scrollable canvas (apps + Restart + Shut Down) --------- *
 * Entries beyond the visible window scroll: hovering the bottom edge shows a
 * down chevron and scrolls down; once scrolled, an up chevron appears at the
 * top edge and hovering it scrolls back. */
static int mrow_h(void) { int h = fb_text_h() + 8; return h < 22 ? 22 : h; }
#define MROW (mrow_h())
#define MENU_MAXVIS 11

static int  menu_count(void)         { return NAPPS + 2; }   /* apps + restart + shutdown */
static const char *menu_label(int i) { return i < NAPPS ? app_name(i) : (i == NAPPS ? "Restart" : "Shut Down"); }
static int  menu_icon(int i)         { return i < NAPPS ? i : -1; }
static int  menu_vis(void)           { int t = menu_count(); return t < MENU_MAXVIS ? t : MENU_MAXVIS; }

static void menu_activate(int i)
{
    if (i < 0 || i >= menu_count()) return;
    if (i < NAPPS)       open_app(i);            /* also closes the launcher */
    else if (i == NAPPS) uno_pc64_restart();
    else                 uno_pc64_shutdown();
}

static void chevron(int cx, int y, int dir, fb_px c)         /* -1 up, +1 down */
{ int k; for (k = 0; k < 4; k++) { int wd = (dir < 0 ? 2*k+1 : 2*(3-k)+1); fb_hline(cx - wd/2, y + k, wd, c); } }

static void launcher_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = UI.theme; int vis = menu_vis(), total = menu_count(), i;
    (void)w; (void)ctx;
    for (i = 0; i < vis; i++) {
        int idx = g_menu_scroll + i, ry = r.y + i * MROW, hot = (idx == g_menu_hot);
        if (idx >= total) break;
        if (hot) fb_fill_rect(r.x, ry, r.w, MROW, t->pal.accent);
        if (idx == NAPPS) fb_hline(r.x, ry, r.w, t->pal.shadow);     /* power divider */
        if (menu_icon(idx) >= 0) { unoui_rect eb = { r.x + 3, ry + (MROW - 18) / 2, 18, 18 }; pc64_icon_emblem(menu_icon(idx), eb); }
        fb_text(r.x + 26, ry + (MROW - fb_text_h()) / 2, menu_label(idx),
                hot ? t->pal.accent_text : t->pal.text, -1);
    }
    if (g_menu_scroll > 0)           chevron(r.x + r.w/2, r.y + 1,           -1, t->pal.text);
    if (g_menu_scroll + vis < total) chevron(r.x + r.w/2, r.y + vis*MROW - 6, +1, t->pal.text);
}

/* keep the highlighted entry within the scrolled window */
static void menu_reveal(void)
{
    int vis = menu_vis();
    if (g_menu_hot < g_menu_scroll)           g_menu_scroll = g_menu_hot;
    if (g_menu_hot >= g_menu_scroll + vis)    g_menu_scroll = g_menu_hot - vis + 1;
}

static int launcher_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; int ox, oy, row;
    (void)w; (void)ctx;
    if (e->kind == UI_EV_KEY) {                       /* keyboard navigation */
        int total = menu_count();
        if (g_menu_hot < 0) g_menu_hot = g_menu_scroll;
        if (e->key == UI_KEY_DOWN) { if (g_menu_hot < total - 1) g_menu_hot++; menu_reveal(); g_dirty = 1; return 1; }
        if (e->key == UI_KEY_UP)   { if (g_menu_hot > 0) g_menu_hot--;         menu_reveal(); g_dirty = 1; return 1; }
        if (e->key == UI_KEY_ENTER){ menu_activate(g_menu_hot); return 1; }
        return 0;
    }
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    unoui_content_origin(UI.theme, &g_launch, &ox, &oy);
    row = (e->y - oy) / MROW;
    if (row >= 0 && row < menu_vis()) { menu_activate(g_menu_scroll + row); return 1; }
    return 0;
}
static unoui_canvas g_menu_cv = { launcher_draw, launcher_event, 0 };

/* per-frame while the launcher is open: highlight the hovered row, and scroll
 * when the pointer rests on the top/bottom scroll zone. */
static void launcher_hover(int mx, int my)
{
    int ox, oy, vis = menu_vis(), total = menu_count(), row, oldhot = g_menu_hot;
    unoui_content_origin(UI.theme, &g_launch, &ox, &oy);
    /* only the pointer sitting ON the menu changes the highlight - otherwise
     * leave it (so keyboard selection isn't clobbered every frame). */
    if (mx < g_launch.r.x || mx >= g_launch.r.x + g_launch.r.w) return;
    if (my < oy || my >= oy + vis * MROW) return;
    if (g_menu_scroll > 0 && my < oy + 12) {                        /* up zone */
        if (++g_scroll_tmr >= 6) { g_scroll_tmr = 0; g_menu_scroll--; g_dirty = 1; }
        return;
    }
    if (g_menu_scroll + vis < total && my >= oy + vis*MROW - 12) {  /* down zone */
        if (++g_scroll_tmr >= 6) { g_scroll_tmr = 0; g_menu_scroll++; g_dirty = 1; }
        return;
    }
    g_scroll_tmr = 0;
    row = (my - oy) / MROW;
    if (row >= 0 && row < vis) g_menu_hot = g_menu_scroll + row;
    if (g_menu_hot != oldhot) g_dirty = 1;
}

static void build_launcher(void)
{
    const unoui_metrics *m = &UI.theme->m;
    int i, tw = 0, winw, vis = menu_vis();
    for (i = 0; i < menu_count(); i++) { int t = fb_text_w(menu_label(i)); if (t > tw) tw = t; }
    winw = tw + 26 + 14 + 2 * m->frame_w + 2 * m->pad;
    g_menu_scroll = 0; g_menu_hot = -1;
    unoui_window_init(&g_launch, "Programs", 8, 20,
                      winw, m->title_h + m->pad + vis * MROW + m->pad + m->frame_w);
    unoui_add_canvas(&g_launch, 0, 0, winw - 2 * m->frame_w - 2 * m->pad, vis * MROW, &g_menu_cv);
}

/* ---- keep windows on-screen after a resolution change ------------------- */
static void reflow(void)
{
    int i;
    UI.screen_w = FB_W; UI.screen_h = FB_H;
    unoui_bg_invalidate();                             /* desktop size changed: rebuild bg cache */
    g_desk.r.w = FB_W; g_desk.r.h = FB_H - TASKH;      /* desktop fills - taskbar */
    g_task.r.y = FB_H - TASKH; g_task.r.w = FB_W;      /* taskbar re-anchored     */
    if (g_task.nw > 0) g_task.w[0].r.w = FB_W;         /* stretch the bar canvas  */
    rebuild_taskbar();
    for (i = 0; i < UI.nwin; i++) {
        unoui_window *win = UI.win[i];
        if (win->flags & UI_WIN_BARE) continue;        /* desk/taskbar done above */
        if (win->r.x + win->r.w > FB_W) win->r.x = FB_W - win->r.w;
        if (win->r.y + win->r.h > FB_H - TASKH) win->r.y = FB_H - TASKH - win->r.h;
        if (win->r.x < 0) win->r.x = 0;
        if (win->r.y < 0) win->r.y = 0;
    }
    g_dirty = 1;
}
void uno_screen_changed(void) { if (UI.nwin) reflow(); }

/* ---- calendar date picker (a popup over the unoui calendar core) --------- */
static void cal_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{ (void)w; (void)ctx; g_cal_rect = r;
  unoui_calendar_draw(UI.theme, r, g_cal_y, g_cal_mo, g_cal_sel); }

static void cal_apply_and_close(void)
{
    /* the calendar owns the date; the time of day is left as it is (from the
       time spinners when the Control Panel is built, else the live RTC) */
    int hh = 0, mi = 0;
    if (g_sp_h && g_sp_mi) { hh = g_sp_h->value; mi = g_sp_mi->value; }
    else uno_pc64_time(0, 0, 0, &hh, &mi, 0);
    uno_pc64_set_time(g_cal_y, g_cal_mo, g_cal_sel, hh, mi, 0);
    fmt_clock(0);
    remove_win(&g_cal); g_cal_open = 0; g_dirty = 1;
}

static int cal_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev; (void)w; (void)ctx;
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    { int hit = unoui_calendar_hit(g_cal_rect, g_cal_y, g_cal_mo, e->x, e->y);
      if (hit == UI_CAL_PREV) { if (--g_cal_mo < 1) { g_cal_mo = 12; g_cal_y--; } g_dirty = 1; return 1; }
      if (hit == UI_CAL_NEXT) { if (++g_cal_mo > 12) { g_cal_mo = 1;  g_cal_y++; } g_dirty = 1; return 1; }
      if (hit >= 1) { g_cal_sel = hit; cal_apply_and_close(); return 1; } }
    return 0;
}
static unoui_canvas g_cal_cv = { cal_draw, cal_event, 0 };

static void open_calendar(void)
{
    const unoui_metrics *m = &UI.theme->m;
    int cw = 210, chh = 176, yy = 2026, mo = 1, dd = 1, hh = 0, mi = 0;
    if (g_cal_open) { remove_win(&g_cal); g_cal_open = 0; }
    uno_pc64_time(&yy, &mo, &dd, &hh, &mi, 0);
    g_cal_y = yy; g_cal_mo = mo; g_cal_sel = dd;
    unoui_window_init(&g_cal, "Pick a date", 180, 56,
                      cw + 2*m->frame_w + 2*m->pad, chh + m->title_h + 2*m->pad + m->frame_w);
    unoui_add_canvas(&g_cal, 0, 0, cw, chh, &g_cal_cv);
    clamp_to_workarea(&g_cal);
    unoui_ui_add(&UI, &g_cal);
    g_cal_open = 1; g_dirty = 1;
}

/* ---- shell services for the standalone apps (Write / Files) ------------- */
void pc64_shell_add_window(unoui_window *w)
{ clamp_to_workarea(w); unoui_ui_add(&UI, w); g_dirty = 1; }
void pc64_shell_remove_window(unoui_window *w) { remove_win(w); g_dirty = 1; }
void pc64_shell_focus_window(unoui_window *w)
{ int i; for (i = 0; i < UI.nwin; i++) if (UI.win[i] == w) { UI.focus_win = i; break; } }
void pc64_shell_dirty(void) { g_dirty = 1; }
int  pc64_shell_workarea_w(void) { return FB_W; }
int  pc64_shell_workarea_h(void) { return FB_H - TASKH; }

/* app-side action hooks (in pc64_write.c / pc64_files.c) */
int pc64_write_action(const unoui_action *a);
int pc64_files_action(const unoui_action *a);
int pc64_write_key(int uni, int ctrl);
void pc64_write_frame(void);

/* Rebuild everything laid out in font-space (native app windows, taskbar,
 * desktop icon grid, Start menu) after a font or UI-scale change. Open native
 * windows are torn down and reopened at the new metrics; module/bridge apps
 * keep their canvases and just get clamped. */
static void rebuild_shell(void)
{
    int a, wasopen[NAPPS];
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }
    if (g_cal_open)    { remove_win(&g_cal);    g_cal_open = 0; }
    for (a = 0; a < NAPPS; a++) {
        wasopen[a] = g_open[a];
        if (a < NNATIVE) {
            if (g_open[a]) { remove_win(&g_win[a]); g_open[a] = 0; }
            g_built[a] = 0;
        }
    }
    remove_win(&g_desk); remove_win(&g_task);
    build_desktop();  unoui_ui_add(&UI, &g_desk);
    build_taskbar();  unoui_ui_add(&UI, &g_task);
    build_launcher();
    for (a = 0; a < NNATIVE; a++) if (wasopen[a]) open_app(a);
    for (a = NNATIVE; a < NAPPS; a++) if (g_open[a]) clamp_to_workarea(&g_win[a]);
    g_dirty = 1;
}

/* ---- actions ----------------------------------------------------------- */
static void on_action(const unoui_action *a)
{
    if (!a->changed) return;
    g_dirty = 1;
    if (a->kind == UI_ACT_CLOSE) { close_focused(); return; }   /* title-bar close box */
    if (a->id >= ID_TASK0   && a->id < ID_TASK0   + NAPPS) { open_app(a->id - ID_TASK0);   return; }
    if (a->id >= ID_LAUNCH0 && a->id < ID_LAUNCH0 + NAPPS) { open_app(a->id - ID_LAUNCH0); return; }
    if (pc64_write_action(a)) return;           /* the Editor's menus/toolbar */
    if (pc64_files_action(a)) return;           /* the file manager's toolbar */
    switch (a->id) {
    case ID_ILIST:    inst_select(a->value); break;
    case ID_IDEF:     g_inst_default = a->value; break;
    case ID_IRESCAN:  inst_rescan(); break;
    case ID_IGO:      inst_go(); break;
    case ID_START:    toggle_launcher(); break;
    case ID_SHUTDOWN: uno_pc64_shutdown(); break;
    case ID_RESTART:  uno_pc64_restart();  break;
    case ID_THEME: if (a->value >= 0 && a->value < NTHEMES) unoui_ui_theme(&UI, kThemes[a->value].theme); break;
    case ID_DARK:  unoui_ui_theme(&UI, a->value ? &theme_aurora_dark : &theme_aurora_light); break;
    case ID_ALITE: unoui_aurora_lite = a->value ? 1 : 0; g_dirty = 1; break;   /* Aurora full<->lite (no live composite) */
    case ID_LIDSLP: g_lidsleep = a->value ? 1 : 0; break;                      /* lid-close enters sleep */
    case ID_VOL:   uno_snd_volume(a->value); break;    /* PCM gain; PC speaker has none */
    case ID_RES:   if (a->value >= 0 && a->value < g_res_n) { uno_pc64_res_set(a->value); reflow(); } break;
    case ID_ABOUT: open_app(APP_SYS); break;
    case ID_SETDT: {                    /* time spinners; the date stays as-is */
        int yy = 2026, mo = 1, dd = 1;
        uno_pc64_time(&yy, &mo, &dd, 0, 0, 0);
        if (g_sp_h && g_sp_mi)
            uno_pc64_set_time(yy, mo, dd, g_sp_h->value, g_sp_mi->value, 0);
        fmt_clock(0);
        break;
    }
    case ID_FULL:  unoui_fullscreen(&UI, &g_win[APP_DEMO]); break;   /* go fullscreen */
    case ID_FONT:  uno_font_use(a->value - 1); rebuild_shell(); break;
    case ID_SCALE: if (a->value >= 0 && a->value < NSCALES) {
                       uno_font_set_ui_scale(g_scale_pcts[a->value]);
                       rebuild_shell();
                   } break;
    case ID_CAL:   open_calendar(); break;              /* calendar date picker */
    default: break;
    }
}

/* ---- UEFI input -> unoui_event ----------------------------------------- */
static int feed(const unoui_event *ev)
{ unoui_action a = unoui_handle(&UI, ev); on_action(&a); return 1; }

static int pump_input(void)
{
    static int lastx = -1, lasty = -1, lastb = 0;
    int mx, my, mb, scan, uni, ctrl, any = 0;
    unoui_event ev;

    uno_pc64_mouse(&mx, &my, &mb);
    if (mx != lastx || my != lasty) {
        memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_MOVE; ev.x = mx; ev.y = my;
        feed(&ev); lastx = mx; lasty = my; any = 1;
    }
    if (mb != lastb) {
        memset(&ev, 0, sizeof ev);
        ev.kind = mb ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP; ev.x = mx; ev.y = my;
        feed(&ev); lastb = mb; any = 1;
    }
    while (uno_pc64_next_key(&scan, &uni, &ctrl)) {
        int mods = ctrl ? UI_MOD_CTRL : 0, vk = 0;
        any = 1;
        if (UI.full && scan == 0x17) {          /* Esc leaves a fullscreen game */
            unoui_fullscreen(&UI, 0); UI.focus_wi = 0; g_dirty = 1; continue;
        }
        if (ctrl && scan == 0x17) { toggle_launcher(); continue; }               /* Ctrl-Esc: Start menu */
        if (ctrl && (uni == 'w' || uni == 'W' || uni == 0x17)) { close_focused(); continue; }  /* Ctrl-W */
        if (scan == 0x0C || (ctrl && uni == 0x09)) { cycle_window(); continue; }  /* F2 / Ctrl-Tab */
        /* Editor accelerators (Ctrl-S/O/N, B/I/U, X/C/V, A, F...) - only when
           the Editor window is in front; pc64_write.c owns the mapping. */
        if (ctrl && !g_launch_open && !UI.full && g_open[APP_EDIT] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[APP_EDIT]) {
            if (pc64_write_key(uni, 1)) { g_dirty = 1; continue; }
        }
        /* Install window focused: keyboard drive (works before any pointer is
           up - important on the harness AND on laptops with exotic trackpads).
           Up/Down pick a target, I = Install, R = Rescan. */
        if (!g_launch_open && !UI.full && g_open[APP_SETUP] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[APP_SETUP]) {
            int used = 1;
            if      (scan == 0x01) inst_select(g_inst_sel - 1);      /* up   */
            else if (scan == 0x02) inst_select(g_inst_sel + 1);      /* down */
            else if (uni == 'i' || uni == 'I') inst_go();
            else if (uni == 'r' || uni == 'R') inst_rescan();
            else used = 0;
            if (used) { g_dirty = 1; continue; }
        }
        switch (scan) {
        case 0x01: vk = UI_KEY_UP; break;    case 0x02: vk = UI_KEY_DOWN; break;
        case 0x03: vk = UI_KEY_RIGHT; break; case 0x04: vk = UI_KEY_LEFT; break;
        case 0x05: vk = UI_KEY_HOME; break;  case 0x06: vk = UI_KEY_END; break;
        case 0x09: vk = UI_KEY_PGUP; break;  case 0x0A: vk = UI_KEY_PGDN; break;
        case 0x08: vk = UI_KEY_DELETE; break;case 0x17: vk = UI_KEY_ESC; break;
        }
        if (!vk) {
            if (uni == 0x0D || uni == 0x0A) vk = UI_KEY_ENTER;
            else if (uni == 0x08) vk = UI_KEY_BACKSPACE;
            else if (uni == 0x09) vk = UI_KEY_TAB;
        }
        memset(&ev, 0, sizeof ev);
        if (vk) { ev.kind = UI_EV_KEY; ev.key = vk; ev.mods = mods; feed(&ev); }
        else if (uni >= 32 && uni < 127) { ev.kind = UI_EV_CHAR; ev.ch = uni; feed(&ev); }
    }
    return any;
}

/* system-tray clock: the firmware RTC wall time, HH:MM:SS */
static void fmt_clock(int uptime_secs)
{
    int h = 0, mi = 0, s = 0;
    if (uno_pc64_time(0, 0, 0, &h, &mi, &s)) {
        g_clock[0]='0'+(h/10)%10;  g_clock[1]='0'+h%10;  g_clock[2]=':';
        g_clock[3]='0'+(mi/10)%10; g_clock[4]='0'+mi%10; g_clock[5]=':';
        g_clock[6]='0'+(s/10)%10;  g_clock[7]='0'+s%10;  g_clock[8]=0;
    } else {                              /* no RTC (e.g. bare QEMU): uptime */
        int j = 0, v = uptime_secs, k = 0; char t[12];
        strcpy(g_clock, "up ");
        j = 3; if (!v) t[k++]='0'; while (v){t[k++]='0'+v%10; v/=10;}
        while (k) g_clock[j++]=t[--k]; g_clock[j++]='s'; g_clock[j]=0;
    }
}

/* tray battery chip: AML _BST percentage (internally cached ~2 s, so this is
 * cheap to call every half-second tick).  Empty string = no battery -> hidden. */
static void fmt_batt(void)
{
#ifdef UNO_ACPI
    int pct = acpi_battery_percent();
    if (pct >= 0) {
        int j = 0;
        if (pct >= 100) g_batt[j++] = '0' + (pct / 100) % 10;
        if (pct >= 10)  g_batt[j++] = '0' + (pct / 10) % 10;
        g_batt[j++] = '0' + pct % 10;
        g_batt[j++] = '%';
        g_batt[j] = 0;
    } else
        g_batt[0] = 0;
#endif
}

/* the legacy app index currently in front (fullscreen or focused), or -1 */
static int active_legacy(void)
{
    unoui_window *win = UI.full ? UI.full
                      : (UI.focus_win >= 0 && UI.focus_win < UI.nwin ? UI.win[UI.focus_win] : 0);
    int a;
    if (win) for (a = NNATIVE; a < NAPPS; a++)
        if (&g_win[a] == win && g_open[a]) return a - NNATIVE;
    return -1;
}

int main(void)
{
    unoui_event tick;
    int idle = 0, halfsecs = 0, was_dragging = 0;

    uno_pc64_init();
    unoui_ui_init(&UI, &theme_aurora_light, FB_W, FB_H);   /* modern default look */
    unoui_icon_art = pc64_icon_art;     /* distinct per-app icon artwork */
    unoui_font_push = uno_font_push;    /* per-window font overrides (Editor doc font) */
    unoui_font_pop  = uno_font_pop;
    uno_mac_mouse   = uno_pc64_mac_mouse;   /* live pointer for Paint's drag spin */
    uno_xhci_init();                    /* USB host controller (inert unless -DUNO_XHCI) */
    ax88179_nic();                      /* bind a USB Ethernet adapter if one is attached */
    uno_seq_init();                     /* UnoSound: PC-speaker voice */
    uno_seq_backend(uno_pc64_snd_note, uno_pc64_snd_quiet);
    unoapp_setup(&g_dirty);             /* wire the legacy-app KernelApi */
    uno_font_set_subpixel(1);           /* subpixel AA for the outline faces  */
    /* Default to the bundled Chicago-style bitmap face (slot 0). It renders at
     * its native px with AA off (crisp 1:1 pixels). If its TTF can't be loaded
     * (e.g. missing from the ESP) uno_font_use falls back to the built-in 8x8
     * bitmap, which the Control Panel picker ("System (mono)") also selects.
     * MUST run before the shell chrome is built: the taskbar, icon grid and
     * launcher are all laid out in the live font's metrics. */
    uno_font_use(0);
    build_desktop();  unoui_ui_add(&UI, &g_desk);   /* bottom: icon layer  */
    build_taskbar();  unoui_ui_add(&UI, &g_task);   /* top: the taskbar    */
    build_launcher();                                /* opened via Start    */
    fmt_clock(0);                                    /* tray clock ready now */
    fmt_batt();                                      /* tray battery (ACPI)  */
    open_app(APP_CTRL);                 /* start with Control Panel open */

    memset(&tick, 0, sizeof tick); tick.kind = UI_EV_TICK;
    for (;;) {
        int la;
        uno_pc64_poll();
#ifdef UNO_ACPI
        {
            /* Always poll the edge (keeps the detector's baseline current even
             * when the toggle is off); only ACT on it when lid-sleep is on. */
            acpi_lid_event_t le = acpi_lid_event();
            if (!g_lidsleep) le = ACPI_LID_EVT_NONE;
            if (!g_asleep) {
                if (le == ACPI_LID_EVT_CLOSE) uui_sleep_enter();
            } else {
                /* asleep: wake on lid-open or any key/click; discard the input */
                int woke = (le == ACPI_LID_EVT_OPEN), sc, uni, ct, mx, my, mb;
                while (uno_pc64_next_key(&sc, &uni, &ct)) woke = 1;
                uno_pc64_mouse(&mx, &my, &mb); if (mb) woke = 1;
                if (woke) uui_sleep_wake();
                else { uno_pc64_delay_ms(50); continue; }
            }
        }
#endif
        if (pump_input()) { g_dirty = 1; idle = 0; }
        else if (++idle >= 30) {        /* ~0.5 s: caret blink + tray clock tick */
            idle = 0; g_dirty = 1;
            fmt_clock(++halfsecs / 2);
            fmt_batt();                 /* AML _BST, self-cached ~2 s */
        }
        feed(&tick);                    /* advance the caret-blink timebase */
        pc64_write_frame();             /* Editor caret blink / autoscroll */
        uno_seq_tick();                 /* UnoSound: advance music/SFX ~60 Hz */
        if (g_launch_open) {            /* Start-menu hover highlight + scroll */
            int mx, my, mb; uno_pc64_mouse(&mx, &my, &mb); launcher_hover(mx, my);
        }
        la = active_legacy();           /* drive the focused game/tool clock */
        { int g = (la >= 0) ? app_game(NNATIVE + la) : -1;
          if (g >= 0) { pc64_game_tick(g); g_dirty = 1; unoapp_focus(-1); }  /* native game */
          else if (la >= 0 && app_is_bridge(NNATIVE + la)) { unoapp_focus(la); unoapp_run_tick(la); }
          else unoapp_focus(-1); }                                            /* browser: no tick */
        if (UI.full) g_dirty = 1;       /* fullscreen apps redraw every frame */
        /* Rubber-band window drag: the desktop and windows are static while a
           title bar is dragged - only the outline moves. Render the full scene
           once when the drag begins, snapshot it, then each moved frame just
           restore the snapshot and redraw the outline, instead of re-running the
           (alpha-blend heavy) full-scene painter every mouse move. This is the
           fix for laggy dragging. */
        if (UI.drag_active) {
            if (!was_dragging) {                 /* drag just began */
                UI.drag_active = 0;              /* render the scene without the outline */
                unoui_render_ui(&UI);
                UI.drag_active = 1;
                uno_pc64_scene_save();
                unoui_draw_drag_outline(&UI);
                uno_pc64_present();
                g_dirty = 0;
            } else if (g_dirty) {                /* outline moved */
                uno_pc64_scene_restore();
                unoui_draw_drag_outline(&UI);
                uno_pc64_present();
                g_dirty = 0;
            } else {
                uno_pc64_delay_ms(16);
            }
        } else {
            if (was_dragging) g_dirty = 1;       /* drag ended: repaint to commit the move */
            if (g_dirty) { unoui_render_ui(&UI); uno_pc64_present(); g_dirty = 0; }
            else uno_pc64_delay_ms(16);
        }
        was_dragging = UI.drag_active;
    }
    return 0;
}
