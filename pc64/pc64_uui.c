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
#include "pc64_accounts.h"   /* the security UI: login gate, consent sheet, accounts */
#include "pc64_icons.h"      /* per-app icon artwork */
#include "pc64_font.h"       /* TrueType text engine (system font) */
#include "unosound.h"        /* UnoSound live sequencer (game/app audio) */
#include "xhci.h"            /* USB host controller (gated -DUNO_XHCI) */
#include "detachgate.h"      /* which device is holding this machine attached */
#include "uno_debug.h"       /* debug build: heartbeat/HUD/stress (no-ops otherwise) */
#include "unoauto.h"         /* unoautomate taps + DRIVE accessors (no-ops in prod) */
#include "unoauto_remote.h"  /* remote dev-PC link pump (no-op in prod) */
#include "unoauto_screen.h"  /* remote-desktop screen capture tick (no-op in prod) */
#include "netdisc.h"         /* zero-config LAN discovery (no-op in prod) */
#ifdef UNO_DEBUG
unsigned long long uno_native_rdtsc(void);
#endif
#include "ax88179.h"         /* USB Ethernet adapter (ASIX) */
#include "rtl8152.h"         /* USB Ethernet adapter (Realtek; docks/dongles) */
#include "net.h"             /* net_link / net_ip / net_dhcp_done - tray LAN chip */
#include "pc64_fs.h"         /* uno_fs_* - session persistence (SHELL.CFG) */
#include "bootinfo.h"   /* which firmware actually started this machine */
#include "iwlwifi.h"         /* Intel AC/AX WiFi (firmware-driven) */
#include "i2c_hid.h"         /* native trackpad status/diag (System readout) */
#include "pc64_native.h"     /* PS/2 kbd/aux bind status (System readout)   */
/* firmware pointer-instance counts + the detach-held-for-pointer flag */
void uno_pc64_ptr_status(int *nsimple, int *nabs, int *blocked);
/* trackpad pointer speed, as a percentage (Control Panel slider) */
void uno_pc64_pointer_speed(int pct);
int  uno_pc64_pointer_speed_get(void);
int  uno_pc64_detached(void);
void pc64_music_closed(void);
int  pc64_music_key(int uni, int scan);
int  pc64_clock_action(const unoui_action *a);
void pc64_clock_tick(void);
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
enum { APP_CTRL, APP_EDIT, APP_FILES, APP_SYS, APP_CLOCK, APP_SETUP,
       APP_MUSIC, APP_UNOAMP, NNATIVE };
#define NEXTRA 9                          /* extra native apps beyond the bridge */
#define EX_RUNNER  (NNATIVE + UNOAPP_COUNT)       /* Runner3D: shell app index    */
#define EX_BROWSER (NNATIVE + UNOAPP_COUNT + 1)   /* Browser: shell app index     */
#define EX_STUDIO  (NNATIVE + UNOAPP_COUNT + 2)   /* Studio IDE (a .UNO module)   */
#define EX_PHOTOS  (NNATIVE + UNOAPP_COUNT + 3)   /* Photos viewer (.UNO module)  */
#define EX_USERAPP (NNATIVE + UNOAPP_COUNT + 4)   /* the app Studio just built    */
#define EX_PYAPP   (NNATIVE + UNOAPP_COUNT + 5)   /* a running Python app (PYRT)  */
#define EX_SSH     (NNATIVE + UNOAPP_COUNT + 6)   /* SSH client (native canvas)   */
#define EX_UOWORD  (NNATIVE + UNOAPP_COUNT + 7)   /* UnoWord (a .UNO module)      */
#define EX_UOCALC  (NNATIVE + UNOAPP_COUNT + 8)   /* UnoCalc (a .UNO module)      */
#define NAPPS  (NNATIVE + UNOAPP_COUNT + NEXTRA)
#define APP_TBAR 18                       /* legacy apps' own title-bar height */
static const char *kAppNames[NNATIVE] =
    { "Control Panel", "Editor", "Files", "System", "Clock", "Install",
      "Music", "UnoAmp" };

/* taskbar height follows the active font (26 px under the classic 8px font) */
static int tb_h(void) { int h = fb_text_h() + 12; return h < 26 ? 26 : h; }
#define TASKH (tb_h())

static unoui_ui     UI;

/* Publish the work area to unoui: the screen minus the taskbar, which is what
 * unoui clamps, maximizes and snaps into. TASKH follows the active font, so
 * this has to be re-published whenever the font, UI scale or resolution moves -
 * see build_taskbar() and reflow(). */
static void set_workarea(void)
{
    UI.work.x = 0; UI.work.y = 0;
    UI.work.w = FB_W; UI.work.h = FB_H - TASKH;
}

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

/* ---- Studio: the IDE, a unoui-CLASS module (APPS\STUDIO.UNO) --------------
 * Not linked into the kernel: the shell probes for the file, loads it on
 * first open, and drives it through the same build/action/key/frame hooks
 * the built-in apps use.  A distro without the file just has no Studio. */
#include "uno_uuiapp.h"
#include "pyhost.h"
static const UnoUuiApp *g_studio;
static int  g_studio_tried, g_studio_present;
static void studio_ensure(void)
{
    if (g_studio || g_studio_tried) return;
    g_studio_tried = 1;
    {
        UnoUuiEntry e = uno_mod_load_uui("STUDIO.UNO");
        if (e) g_studio = e(0);
        if (g_studio && g_studio->abi != UNO_UUIAPP_ABI) g_studio = 0;
    }
}

/* ---- PYRT: the Python runtime module (APPS\PYRT.UNO), optional -----------
 * Loaded lazily on the first Python run.  It hosts Python apps and hands the
 * shell a UnoUuiApp (g_pyapp) driven exactly like Studio's window. */
static const PyHost   *g_pyrt;
static int             g_pyrt_tried, g_pyrt_present;
static char            g_pyrt_gc[16 * 1024 * 1024];   /* MicroPython GC heap */
static const UnoUuiApp *g_pyapp;                       /* the running Python app */
/* automation-app caps (unoscript.c): a launched Python app runs under an
 * isolated session carrying the caps its signed manifest declares. */
int  unoscript_app_caps_begin(int vol, const char *path);
void unoscript_app_caps_end(void);
#ifdef UNO_DBGCON
static void pdbg(const char *s){ while(*s) __asm__ volatile("outb %0,%1"::"a"((unsigned char)*s++),"Nd"((unsigned short)0x402)); }
#else
static void pdbg(const char *s){ (void)s; }
#endif
static void pyrt_ensure(void)
{
    if (g_pyrt || g_pyrt_tried) return;
    g_pyrt_tried = 1;
    {
        PyHostEntry e = uno_mod_load_pyrt();
        pdbg(e ? "pyrt: PYRT.UNO loaded\n" : "pyrt: PYRT.UNO absent\n");
        if (e) g_pyrt = e(0);
        if (g_pyrt && g_pyrt->abi != UNO_PYHOST_ABI) { pdbg("pyrt: abi mismatch\n"); g_pyrt = 0; }
        if (g_pyrt) { g_pyrt->init(g_pyrt_gc, sizeof g_pyrt_gc); g_pyrt_present = 1; pdbg("pyrt: init ok\n"); }
    }
}

/* ---- Photos: the image viewer, the second unoui-CLASS module --------------
 * (APPS\PHOTOS.UNO - the unomedia decoders ride inside the module).  Same
 * hosting contract as Studio; a distro without the file has no Photos. */
static const UnoUuiApp *g_photos;
static int  g_photos_tried, g_photos_present;
static void photos_ensure(void)
{
    if (g_photos || g_photos_tried) return;
    g_photos_tried = 1;
    {
        UnoUuiEntry e = uno_mod_load_uui("PHOTOS.UNO");
        if (e) g_photos = e(0);
        if (g_photos && g_photos->abi != UNO_UUIAPP_ABI) g_photos = 0;
    }
}

/* ---- UnoWord: the word processor, a unoui-CLASS module --------------------
 * (APPS\\UOWORD.UNO - the whole uoffice chrome lane and unodoc's Word half
 * ride inside the module).  Same hosting contract as Studio and Photos; a
 * distro without the file simply has no UnoWord. */
static const UnoUuiApp *g_uoword;
static int  g_uoword_tried, g_uoword_present;
static void uoword_ensure(void)
{
    if (g_uoword || g_uoword_tried) return;
    g_uoword_tried = 1;
    {
        UnoUuiEntry e = uno_mod_load_uui("UOWORD.UNO");
        if (e) g_uoword = e(0);
        if (g_uoword && g_uoword->abi != UNO_UUIAPP_ABI) g_uoword = 0;
    }
}

static const UnoUuiApp *g_uocalc;
static int  g_uocalc_tried, g_uocalc_present;
static void uocalc_ensure(void)
{
    if (g_uocalc || g_uocalc_tried) return;
    g_uocalc_tried = 1;
    {
        UnoUuiEntry e = uno_mod_load_uui("UOCALC.UNO");
        if (e) g_uocalc = e(0);
        if (g_uocalc && g_uocalc->abi != UNO_UUIAPP_ABI) g_uocalc = 0;
    }
}

static const char *kNativeShort[NNATIVE] =
    { "Control", "Editor", "Files", "System", "Clock", "Install",
      "Music", "UnoAmp" };
static const char *py_app_name(void)
{ return (g_pyapp && g_pyapp->name) ? g_pyapp->name : "Python app"; }
static const char *app_name(int a)
{ return a == EX_RUNNER ? "Runner3D" : a == EX_BROWSER ? "Browser"
       : a == EX_SSH ? "SSH"
       : a == EX_STUDIO ? "Studio" : a == EX_PHOTOS ? "Photos"
       : a == EX_UOWORD ? "UnoWord"
       : a == EX_UOCALC ? "UnoCalc"
       : a == EX_USERAPP ? unoapp_user_title()
       : a == EX_PYAPP ? py_app_name()
       : a < NNATIVE ? kAppNames[a] : unoapp_name(a - NNATIVE); }
static const char *app_short(int a)
{ return a == EX_RUNNER ? "Runner" : a == EX_BROWSER ? "Browser"
       : a == EX_SSH ? "SSH"
       : a == EX_STUDIO ? "Studio" : a == EX_PHOTOS ? "Photos"
       : a == EX_UOWORD ? "UnoWord"
       : a == EX_UOCALC ? "UnoCalc"
       : a == EX_USERAPP ? unoapp_user_title()
       : a == EX_PYAPP ? py_app_name()
       : a < NNATIVE ? kNativeShort[a] : unoapp_name(a - NNATIVE); }

/* hidden from the launcher + desktop: the user/py-app slots until something
 * runs in them, and Studio when no STUDIO.UNO ships on this system */
static int app_hidden(int a)
{
    if (a == EX_USERAPP) return !g_open[EX_USERAPP];
    if (a == EX_PYAPP)   return !g_open[EX_PYAPP];
    if (a == EX_STUDIO)  return !g_studio_present;
    if (a == EX_PHOTOS)  return !g_photos_present;
    if (a == EX_UOWORD)  return !g_uoword_present;
    if (a == EX_UOCALC)  return !g_uocalc_present;
    return 0;
}

/* Which emblem an app wears. A LOOKUP, not the app's index: apps come and go
 * (and will eventually be loaded from storage, where no static numbering can
 * predict them), so an app names its icon and everyone else's stays put.
 * Anything unrecognised gets PCI_GENERIC rather than a neighbour's art. */
static const unsigned char kNativeIcon[NNATIVE] = {
    PCI_CTRL, PCI_EDIT, PCI_FILES, PCI_SYS, PCI_CLOCK, PCI_SETUP, PCI_MUSIC,
    PCI_MUSIC        /* UnoAmp shares the note icon - it IS the music app */
};
static const unsigned char kBridgeIcon[UNOAPP_COUNT] = {
    PCI_DOSTRIS, PCI_PACMAN, PCI_OUTLAST, PCI_TRACKER, PCI_PAINT
};
static int app_icon(int a)
{
    if (a == EX_RUNNER)  return PCI_RUNNER;
    if (a == EX_BROWSER) return PCI_BROWSER;
    if (a == EX_STUDIO)  return PCI_STUDIO;
    if (a == EX_PHOTOS)  return PCI_PHOTOS;
    if (a == EX_UOWORD)  return PCI_GENERIC;
    if (a == EX_UOCALC)  return PCI_GENERIC;
    if (a == EX_USERAPP) return PCI_GENERIC;
    if (a == EX_PYAPP)   return PCI_GENERIC;
    if (a >= 0 && a < NNATIVE) return kNativeIcon[a];
    if (a >= NNATIVE && a - NNATIVE < UNOAPP_COUNT) return kBridgeIcon[a - NNATIVE];
    return PCI_GENERIC;
}

/* ---- desktop icon arrangement (Control Panel) ------------------------------
 * g_desk_flow: 0 = fill columns (down, then across), 1 = fill rows (across,
 * then down). g_desk_sort: 0 = launcher order, 1 = by name. Kept as plain
 * settings so the desktop can be rebuilt from them at any time.
 *
 * An icon the user has DRAGGED stops taking part in that flow: its position is
 * remembered in g_icon_pos and survives a rebuild (theme, font or resolution
 * change all rebuild the desktop). Auto-arrange forgets every placement and
 * lets the flow lay them out again. */
static int g_desk_flow, g_desk_sort;
static int g_desk_snap = 1;             /* snap dragged icons to the grid     */
static int g_desk_lock;                 /* lock: no dragging at all           */
static struct { short x, y; unsigned char placed; } g_icon_pos[32];

/* ---- tray / wallpaper preferences (Control Panel) --------------------------
 * All in-memory, like the desktop-arrangement settings above: they rebuild the
 * live UI on change but are not persisted across a reboot. */
static int g_wallpaper;                 /* index into g_wall_names (0 = theme default) */
static int g_clock_12h;                 /* tray clock: 0 = 24-hour, 1 = 12-hour AM/PM  */
enum { BATT_PCT, BATT_ICON, BATT_BOTH };
static int g_batt_mode = BATT_BOTH;     /* tray battery: percent / icon / both */
/* The built-in wallpapers.  Index 0 falls through to the active theme's own
 * desktop painter; the rest are procedural (no image assets) so they work on
 * every build and cost nothing to ship. pc64_wallpaper_paint() renders them. */
static const char *g_wall_names[] = {
    "Theme default", "Midnight", "Sunrise", "Evergreen",
    "Aurora", "Graphite grid", "Slate"
};
#define NWALL ((int)(sizeof g_wall_names / sizeof g_wall_names[0]))

/* the layout grid, shared by build_desktop and the drag snap */
static int desk_cell_w(void) { return 20 + fb_text_w("MMMMMMMM"); }
static int desk_cell_h(void) { return 34 + fb_text_h() + 8; }

/* widget ids */
enum { ID_THEME = 1, ID_RES, ID_DARK, ID_WRAP, ID_VOL, ID_SCALE, ID_ABOUT,
       ID_MENU, ID_BODY, ID_NAME, ID_SAVE, ID_OPEN, ID_NEWF, ID_FILES, ID_FMT,
       ID_DATE, ID_TIME, ID_SETDT, ID_FONT, ID_CAL, ID_EFONT, ID_ALITE,
       ID_ILIST, ID_IDEF, ID_IRESCAN, ID_IGO, ID_ICONF, ID_LIDSLP,
       ID_DFLOW, ID_DSORT, ID_PSPEED, ID_DSNAP, ID_DLOCK, ID_DARRANGE,
       ID_LIC, ID_ACCT, ID_WALL, ID_CLOCKFMT, ID_BATTMODE,
       ID_CPTAB, ID_NETREFRESH, ID_NETRENEW, ID_SESSION,
       ID_WIFISCAN, ID_WIFILIST, ID_WIFIPSK, ID_WIFIJOIN,
       ID_START = 90, ID_SHUTDOWN = 91, ID_RESTART = 92,
       ID_LAUNCH0 = 100,                  /* desktop icons + launcher: +app     */
       ID_TASK0   = 200 };                /* taskbar window buttons: +app       */

/* shell status buffers */
static char g_res_str[12][14]; static const char *g_res_items[12]; static int g_res_n;
static char g_clock[40] = "Uptime 0 s";
static char g_batt[12];      /* tray battery chip, "" = no battery reported */
static int  g_batt_pct = -1; /* battery %, -1 = none (drives the tray icon)  */
static char g_net[8];        /* tray LAN chip: "LAN"=lease, "LAN?"=up no IP, ""=down */
/* LAN chip activity: sampled tx/rx frame counters drive a blinking dot -
 * yellow while transmitting (upload), green while receiving (download). */
static unsigned g_net_tx_last, g_net_rx_last;
static int  g_net_act;       /* 0 idle, bit0 = upload, bit1 = download        */
static int  g_net_blink;     /* toggled each sample so an active dot blinks    */
static int  g_net_cx, g_net_cy, g_net_cw, g_net_ch;  /* LAN chip screen rect (for hover) */
static int  g_net_hover;     /* pointer is over the LAN chip -> show tooltip   */

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

/* ---- Control Panel tabs ---------------------------------------------------
 * The panel is organised into sections selected by a tab strip. Switching tabs
 * rebuilds the window content (rebuild_ctrl_window). */
enum { CT_DISPLAY, CT_PERSONAL, CT_NETWORK, CT_AUDIO, CT_DATETIME, CT_SYSTEM, CT_N };
static const char *kCtrlTabs[CT_N] =
    { "Display", "Personalization", "Network", "Audio", "Date & Time", "System" };
static int g_ctrl_tab;
static int g_session_restore = 1;   /* reopen last session's windows at boot */
static int g_session_ready;         /* 1 once boot restore is done (gate saves) */
static void session_save(void);
/* Network-tab status lines (labels store the pointer, so these must persist). */
static char g_cp_net[5][52];
static char *ap_str(char *p, const char *s);   /* fwd (defined below) */
static char *ap_int(char *p, int v);
static void clamp_to_workarea(unoui_window *w);
static void rebuild_ctrl_window(void);

/* ---- Network tab: join a WiFi network -------------------------------------
 * Scan -> pick an SSID -> type the passphrase -> Join. The driver calls block
 * for seconds (a scan is a 5 s dwell; a join is association + the 4-way), and
 * the shell loop is what would normally repaint - so each one paints its own
 * "working" line first and presents it directly, the same trick the installer's
 * progress uses. WIFI.CFG still works; this is the runtime alternative. */
#define CP_WIFI_MAX 16          /* networks kept from a scan (strongest first) */
#define CP_WIFI_ROWS 6          /* rows of the network list shown at once      */
static iwl_ap_t    g_cp_aps[CP_WIFI_MAX];
static char        g_cp_ap_lbl[CP_WIFI_MAX][44];
static const char *g_cp_ap_ptr[CP_WIFI_MAX];
static int         g_cp_ap_n, g_cp_ap_sel;
/* The network list SCROLLS (unoui_add_list + wheel / scrollbar / arrow keys),
 * so every SSID a scan found is reachable. It used to be paged by hand here -
 * a "More" button that advanced a window of CP_WIFI_ROWS - because unoui lists
 * could not scroll at all (metal, X1 Carbon: "the list of found SSIDs doesn't
 * scroll, so I can't choose my network"). The toolkit does it now. */
static char        g_cp_psk[72];
static unoui_text  g_cp_psk_t;
static char        g_cp_wifi_msg[220] = "Press Scan to look for networks.";
static char        g_cp_wifi_more[48];
static char        g_cp_wifi_pwlbl[56];
static char        g_cp_wifi_stat[196];

/* "Network:  <ssid>" - the join target. Written into a persistent buffer the
 * label widget points at, so moving the selection updates the text in place. */
static void cp_wifi_target_label(void)
{
    char *p = ap_str(g_cp_wifi_pwlbl, "Network:  ");
    p = ap_str(p, (g_cp_ap_n > 0 && g_cp_ap_sel >= 0 && g_cp_ap_sel < g_cp_ap_n)
                  ? g_cp_aps[g_cp_ap_sel].ssid : "-");
    *p = 0;
}

/* update the status line and paint it NOW (we are about to block) */
static void cp_wifi_note(const char *s)
{
    char *p = g_cp_wifi_msg;
    while (*s && p < g_cp_wifi_msg + sizeof g_cp_wifi_msg - 1) *p++ = *s++;
    *p = 0;
    unoui_render_ui(&UI);
    uno_pc64_present();
}

static void cp_wifi_scan(void)
{
    int i;
    cp_wifi_note("Scanning for networks...");
    g_cp_ap_n = iwl_scan_aps(g_cp_aps, CP_WIFI_MAX);
    for (i = 0; i < g_cp_ap_n; i++) {
        char *p = ap_str(g_cp_ap_lbl[i], g_cp_aps[i].ssid);
        p = ap_str(p, "   ");
        if (g_cp_aps[i].rssi) { p = ap_int(p, g_cp_aps[i].rssi); p = ap_str(p, " dBm"); }
        p = ap_str(p, "   ch "); p = ap_int(p, g_cp_aps[i].chan);
        *p = 0;
        g_cp_ap_ptr[i] = g_cp_ap_lbl[i];
    }
    g_cp_ap_sel = 0;
    if (g_cp_ap_n) {
        cp_wifi_note("Pick a network, type its password, then Join.");
    } else {
        /* An empty scan almost never means "the air is empty" - it means the
         * radio never came up, and the driver already knows why. Guessing
         * ("blocked by rfkill?") sent a real debugging session down the wrong
         * path; report what the driver says instead. */
        char msg[220]; char *p = ap_str(msg, "No networks found - ");
        char st[160];
        iwl_status_str(st, sizeof st);
        p = ap_str(p, st[0] ? st : "the radio did not come up.");
        *p = 0;
        cp_wifi_note(msg);
    }
    rebuild_ctrl_window();
}

/* "<what> ... (N s)" - the join blocks the shell for seconds at a time, so
 * every phase says what it is doing AND keeps a running clock, otherwise a
 * working join is indistinguishable from a hung machine (metal: "it looks like
 * it's frozen"). */
static void cp_wifi_phase(const char *what, int secs)
{
    char msg[120];
    char *p = ap_str(msg, what);
    if (secs >= 0) { p = ap_str(p, "  ("); p = ap_int(p, secs); p = ap_str(p, " s)"); }
    *p = 0;
    cp_wifi_note(msg);
}

static void cp_wifi_join(void)
{
    uno_nic_t *nic;
    int i;
    if (g_cp_ap_n <= 0) { cp_wifi_note("Scan first."); return; }
    g_cp_psk[g_cp_psk_t.len] = 0;
    { char msg[120]; char *p = ap_str(msg, "Joining \"");
      p = ap_str(p, g_cp_aps[g_cp_ap_sel].ssid);
      p = ap_str(p, "\" - associating and running the 4-way handshake...");
      *p = 0; cp_wifi_note(msg); }
    if (iwl_join_ssid(g_cp_aps[g_cp_ap_sel].ssid, g_cp_psk) != 0) {
        cp_wifi_note("Join failed - check the password (details in the NET log).");
    } else {
        cp_wifi_phase("Joined. Asking the network for an address (DHCP)", 0);
        nic = iwl_nic();
        if (nic) {
            net_init(nic, iwl_mac());       /* the stack binds one nic: WiFi now */
            net_dhcp_start();
            /* 20 s, not 9: a DHCP server behind a fresh WPA2 association often
             * needs more than one DISCOVER, and the retransmit timer only
             * advances while net_poll() runs. Repaint twice a second so the
             * screen is visibly alive the whole time. */
            for (i = 0; i < 4000 && !net_dhcp_done(); i++) {
                net_poll(); uno_pc64_delay_ms(5);
                if (i && (i % 100) == 0)
                    cp_wifi_phase("Asking the network for an address (DHCP)", i / 200);
            }
        }
        cp_wifi_note(net_dhcp_done()
            ? "Connected."
            : "Joined, but no address yet - still asking in the background.");
    }
    for (i = 0; i < (int)sizeof g_cp_psk; i++) g_cp_psk[i] = 0;   /* do not keep it */
    g_cp_psk_t.len = g_cp_psk_t.caret = g_cp_psk_t.sel = 0;
    rebuild_ctrl_window();
}

static void build_ctrl(unoui_window *w)
{
    unoui_widget *x; int i;
    int fh = fb_text_h(), ch = ui_field_h(), bh = ui_ctl_h();
    int row = ch + 8, y = 4, lw = fb_text_w("Resolution:") + 12, cw = 356;
    int lofs = (ch - fh) / 2;                   /* label centred beside a control */
    for (i = 0; i < NTHEMES; i++) kThemeNames[i] = kThemes[i].name;
    build_res_items();
    build_font_items();
    /* widen the panel if the tab strip (laid out by label width) needs it, so no
       tab spills past the window edge under a wider font */
    { int tabw = 8; for (i = 0; i < CT_N; i++) tabw += fb_text_w(kCtrlTabs[i]) + 16;
      if (tabw > cw) cw = tabw; }
    unoui_window_init(w, "Control Panel", 150, 24, 1, 1);   /* sized below */
    x = unoui_add_tabs(w, 4, y, cw - 4, kCtrlTabs, CT_N, g_ctrl_tab); x->id = ID_CPTAB;
    y += UI_TAB_H + 10;

    switch (g_ctrl_tab) {
    case CT_DISPLAY:
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
        unoui_add_check(w, 8, y, "Aurora lite (no live compositing)", unoui_aurora_lite);
        w->w[w->nw-1].id = ID_ALITE;
        y += fh + 10;
        break;

    case CT_PERSONAL:
        unoui_add_label(w, 8, y + lofs, "Theme:");
        x = unoui_add_dropdown(w, lw, y, cw - lw - 8, kThemeNames, NTHEMES, 0); x->id = ID_THEME;
        y += row;
        unoui_add_check(w, 8, y, "Dark mode", 0);   w->w[w->nw-1].id = ID_DARK;
        y += fh + 10;
        { int lw2 = fb_text_w("Wallpaper:") + 8;
          unoui_add_label(w, 8, y + lofs, "Wallpaper:");
          unoui_add_dropdown(w, 8 + lw2, y, cw - lw2 - 8, g_wall_names, NWALL, g_wallpaper);
          w->w[w->nw-1].id = ID_WALL; }
        y += row;
        unoui_add_sep(w, 8, y, cw - 16); y += 10;
        { static const char *flow[] = { "Columns", "Rows" };
          static const char *sort[] = { "Launcher order", "Name" };
          int lw2 = fb_text_w("Desktop icons:") + 8;
          unoui_add_label(w, 8, y + lofs, "Desktop icons:");
          unoui_add_dropdown(w, 8 + lw2, y, fb_text_w("Columns") + 30, flow, 2, g_desk_flow);
          w->w[w->nw-1].id = ID_DFLOW;
          unoui_add_dropdown(w, 8 + lw2 + fb_text_w("Columns") + 36, y,
                             fb_text_w("Launcher order") + 30, sort, 2, g_desk_sort);
          w->w[w->nw-1].id = ID_DSORT; }
        y += ch + 8;
        { int bw = fb_text_w("Auto-arrange") + 16;
          unoui_add_check(w, 8, y, "Snap to grid", g_desk_snap);
          w->w[w->nw-1].id = ID_DSNAP;
          unoui_add_check(w, 8 + fb_text_w("Snap to grid") + 34, y, "Lock desktop", g_desk_lock);
          w->w[w->nw-1].id = ID_DLOCK;
          unoui_add_button(w, cw - bw - 8, y - 4, bw, "Auto-arrange", 0);
          w->w[w->nw-1].id = ID_DARRANGE; }
        y += ch + 10;
        break;

    case CT_NETWORK: {
        /* live status, formatted into persistent buffers (labels keep the ptr) */
        char *p; int up = net_link(), mbps = net_link_speed_mbps();
        p = ap_str(g_cp_net[0], "Status:  ");
        p = ap_str(p, up ? (net_dhcp_done() ? "connected" : "link up, no DHCP lease")
                         : "no link (no NIC bound)"); *p = 0;
        p = ap_str(g_cp_net[1], "IP address:  ");
        if (up && net_dhcp_done()) { const unsigned char *ip = net_ip();
            p = ap_int(p, ip[0]); *p++='.'; p = ap_int(p, ip[1]); *p++='.';
            p = ap_int(p, ip[2]); *p++='.'; p = ap_int(p, ip[3]); }
        else { p = ap_str(p, "-"); }
        *p = 0;
        p = ap_str(g_cp_net[2], "Gateway:  ");
        if (up && net_dhcp_done()) { const unsigned char *gw = net_gw();
            p = ap_int(p, gw[0]); *p++='.'; p = ap_int(p, gw[1]); *p++='.';
            p = ap_int(p, gw[2]); *p++='.'; p = ap_int(p, gw[3]); }
        else { p = ap_str(p, "-"); }
        *p = 0;
        p = ap_str(g_cp_net[3], "Link speed:  ");
        if (mbps >= 1000) { p = ap_int(p, mbps/1000); p = ap_str(p, " Gbps"); }
        else if (mbps > 0){ p = ap_int(p, mbps);      p = ap_str(p, " Mbps"); }
        else              { p = ap_str(p, up ? "negotiating / not reported" : "-"); }
        *p = 0;
        p = ap_str(g_cp_net[4], "Frames:  tx ");
        p = ap_int(p, (int)net_tx_frames()); p = ap_str(p, "   rx ");
        p = ap_int(p, (int)net_rx_frames());
        *p = 0;
        for (i = 0; i < 5; i++) { unoui_add_label(w, 8, y + lofs, g_cp_net[i]); y += row; }
        y += 2;
        x = unoui_add_button(w, 8, y, 110, "Refresh", 0); x->id = ID_NETREFRESH;
        x = unoui_add_button(w, 126, y, 110, "Renew IP", 0); x->id = ID_NETRENEW;
        unoui_add_label(w, 248, y + lofs, up && net_dhcp_done()
                        ? "DHCP is automatic." : "No lease? Try Renew IP.");
        y += bh + 8;
        /* ---- WiFi: scan, pick, type the password, join ---- */
        if (iwl_present()) {
            int pw;
            unoui_add_sep(w, 8, y, cw - 16); y += 8;
            iwl_status_str(g_cp_wifi_stat, sizeof g_cp_wifi_stat);
            unoui_add_label(w, 8, y + lofs, "WiFi:");
            unoui_add_label(w, 8 + fb_text_w("WiFi:") + 8, y + lofs,
                            g_cp_wifi_stat[0] ? g_cp_wifi_stat : "Intel WiFi card present.");
            y += fh + 8;
            if (g_cp_ap_n > 0) {
                int rows = g_cp_ap_n < CP_WIFI_ROWS ? g_cp_ap_n : CP_WIFI_ROWS;
                int lh = rows * (fh + 4) + 6;
                /* the whole scan goes into ONE scrolling list: wheel, the
                 * inline scrollbar and the arrow keys reach the rest */
                x = unoui_add_list(w, 8, y, cw - 16, lh,
                                   g_cp_ap_ptr, g_cp_ap_n, g_cp_ap_sel);
                x->id = ID_WIFILIST;
                unoui_list_set_sel(x, g_cp_ap_sel);   /* keep it in view */
                y += lh + 6;
                if (g_cp_ap_n > rows) {
                    char *p = ap_int(g_cp_wifi_more, g_cp_ap_n);
                    p = ap_str(p, " networks - scroll for the rest"); *p = 0;
                    unoui_add_label(w, 8, y + lofs, g_cp_wifi_more);
                    y += fh + 6;
                }
            }
            /* name the target on its OWN line: the highlighted row can be
             * scrolled out of sight, and the label is rewritten in place when
             * the selection moves (no window rebuild, so the list keeps its
             * scroll position), which a "Password for X:" prefix could not do
             * without re-laying the field out. */
            cp_wifi_target_label();
            unoui_add_label(w, 8, y + lofs, g_cp_wifi_pwlbl);
            y += fh + 6;
            pw = fb_text_w("Password:") + 10;
            unoui_add_label(w, 8, y + lofs, "Password:");
            unoui_text_init(&g_cp_psk_t, g_cp_psk, sizeof g_cp_psk, 0);
            g_cp_psk_t.len = (int)strlen(g_cp_psk);
            g_cp_psk_t.caret = g_cp_psk_t.sel = g_cp_psk_t.len;
            x = unoui_add_edit(w, 8 + pw, y, cw - pw - 16, &g_cp_psk_t); x->id = ID_WIFIPSK;
            y += row;
            x = unoui_add_button(w, 8, y, 110, "Scan", 0); x->id = ID_WIFISCAN;
            x = unoui_add_button(w, 126, y, 110, "Join", 0); x->id = ID_WIFIJOIN;
            y += bh + 6;
            unoui_add_label(w, 8, y + lofs, g_cp_wifi_msg);
            y += fh + 8;
        }
        break; }

    case CT_AUDIO:
        unoui_add_label(w, 8, y + lofs, "Volume:");
        x = unoui_add_slider(w, lw, y, cw - lw - 8, 0, 100, 70); x->id = ID_VOL;
        y += row + 4;
        unoui_add_label(w, 8, y + lofs, "Output device:");
        unoui_add_label(w, lw, y + lofs,
                        uno_snd_active() ? uno_snd_name() : "PC speaker (PIT)");
        y += row;
        break;

    case CT_DATETIME: {
        int hh = 0, mi = 0;
        static const char *cfmt[] = { "24-hour", "12-hour" };
        uno_pc64_time(0, 0, 0, &hh, &mi, 0);
        unoui_add_label(w, 8, y + lofs, "Time:");
        g_sp_h  = unoui_add_spinner(w, lw,      y, 52, 0, 23, hh);
        unoui_add_label(w, lw + 56, y + lofs, ":");
        g_sp_mi = unoui_add_spinner(w, lw + 66, y, 52, 0, 59, mi);
        x = unoui_add_button(w, lw + 126, y, 92, "Set time", 0); x->id = ID_SETDT;
        y += row;
        x = unoui_add_button(w, 8, y, 120, "Set date...", 0); x->id = ID_CAL;
        y += row + 4;
        { int lwc = fb_text_w("Clock format:") + 8;
          unoui_add_label(w, 8, y + lofs, "Clock format:");
          unoui_add_dropdown(w, 8 + lwc, y, 120, cfmt, 2, g_clock_12h);
          w->w[w->nw-1].id = ID_CLOCKFMT; }
        y += row;
        break; }

    case CT_SYSTEM:
        { static const char *bmode[] = { "Percent", "Icon", "Both" };
          int lwb = fb_text_w("Battery display:") + 8;
          unoui_add_label(w, 8, y + lofs, "Battery display:");
          unoui_add_dropdown(w, 8 + lwb, y, 120, bmode, 3, g_batt_mode);
          w->w[w->nw-1].id = ID_BATTMODE; }
        y += row;
        unoui_add_check(w, 8, y, "Restore last session at startup", g_session_restore);
        w->w[w->nw-1].id = ID_SESSION;
        y += fh + 8;
        unoui_add_check(w, 8, y, "Lid sleep", g_lidsleep);
        w->w[w->nw-1].id = ID_LIDSLP;
        y += fh + 10;
        { int lw2 = fb_text_w("Pointer speed:") + 8;
          unoui_add_label(w, 8, y + lofs, "Pointer speed:");
          unoui_add_slider(w, 8 + lw2, y, cw - lw2 - 8, 25, 800,
                           uno_pc64_pointer_speed_get());
          w->w[w->nw-1].id = ID_PSPEED; }
        y += ch + 10;
        unoui_add_sep(w, 8, y, cw - 16); y += 8;
        x = unoui_add_button(w, 8, y, 110, "Accounts...", 0); x->id = ID_ACCT;
        x = unoui_add_button(w, 126, y, 110, "Licenses", 0); x->id = ID_LIC;
        x = unoui_add_button(w, cw - 8 - 96, y, 96, "About", 0); x->id = ID_ABOUT;
        y += bh + 8;
        break;
    }
    w->r.w = cw + 2 * UI.theme->m.frame_w + 2 * UI.theme->m.pad;
    w->r.h = win_h_for(y);
    w->min_w = w->r.w; w->min_h = w->r.h;
    w->flags |= UI_WIN_RESIZE;
}

/* Rebuild the Control Panel content in place (tab switch / network refresh)
 * without moving the window from where the user put it. */
static void rebuild_ctrl_window(void)
{
    int px = g_win[APP_CTRL].r.x, py = g_win[APP_CTRL].r.y;
    build_ctrl(&g_win[APP_CTRL]);
    g_win[APP_CTRL].r.x = px; g_win[APP_CTRL].r.y = py;
    clamp_to_workarea(&g_win[APP_CTRL]);
    UI.focus_wi = 0;
    g_dirty = 1;
}

/* Editor (WordPad-style word processor) + Files (real file manager) live in
 * their own translation units; the shell just delegates the build. */
void pc64_write_build(unoui_window *w);
void pc64_files_build(unoui_window *w);
void pc64_music_build(unoui_window *w);
static void build_edit(unoui_window *w)  { pc64_write_build(w); }
static void build_files(unoui_window *w) { pc64_files_build(w); }
static void build_music(unoui_window *w) { pc64_music_build(w); }
/* UnoAmp is a skinned, chromeless window that draws itself - see
 * pc64/unoamp_ui.c. It owns its EQ and playlist windows rather than the shell
 * doing so, because the three are one player, not three applications. */
void unoamp_ui_build(unoui_window *w);
static void build_unoamp(unoui_window *w) { unoamp_ui_build(w); }
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
    if (pr) { int tm = uno_i2c_hid_timing();
              if (tm >= 0) { p = ap_str(p, "  scl#"); p = ap_int(p, tm); } }
    *p = 0;
}

/* Which pointer path is actually live. Nothing reported this before, so a
 * machine that boots with a dead trackpad gave no clue whether the pad never
 * bound, the aux port is empty, or detach took the firmware pointer away. */
static char g_ptr1[80], g_ptr2[80];
static void build_ptrstat(void)
{
    int nsim = 0, nabs = 0, blocked = 0;
    int kbd = 0, aux = 0, auxport = 0, auxid = -1;
    char *p;
    uno_pc64_ptr_status(&nsim, &nabs, &blocked);
    uno_ps2_status(&kbd, &aux, &auxport, &auxid);
    p = ap_str(g_ptr1, "Pointer: fw simple ");
    p = ap_int(p, nsim); p = ap_str(p, " / abs "); p = ap_int(p, nabs);
    p = ap_str(p, uno_pc64_detached() ? "  (dead: detached)" : "  (live)");
    *p = 0;
    p = ap_str(g_ptr2, "  PS/2: kbd ");
    p = ap_str(p, kbd ? "up" : "--");
    p = ap_str(p, ", aux port ");
    p = ap_str(p, auxport ? "ok" : "none");
    p = ap_str(p, ", mouse ");
    if (aux) { p = ap_str(p, "streaming id "); p = ap_int(p, auxid); }
    else     { p = ap_str(p, "--"); }
    if (blocked) p = ap_str(p, "  [attached to keep pointer]");
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

/* network: the tray LAN chip + the System app's network line. net_link() is
 * null-safe (0 when no NIC is bound) and only reads the PHY status (the medium
 * is cached), so it is cheap on the tray's ~2 s cadence. net_ip() is only
 * meaningful once a DHCP lease landed (net_dhcp_done), so we gate the address
 * on that - "link up, NO lease" is exactly the state that looks connected but
 * cannot actually reach the LAN (what the Yoga hit). */
static char g_netline[48];   /* System app: "Network: link up, IP a.b.c.d" */
static void fmt_net(void)
{
    char *p;
    int up = net_link();
    if (!up)                  g_net[0] = 0;          /* no NIC / link down: hide */
    else if (net_dhcp_done()) { g_net[0]='L'; g_net[1]='A'; g_net[2]='N'; g_net[3]=0; }
    else                      { g_net[0]='L'; g_net[1]='A'; g_net[2]='N'; g_net[3]='?'; g_net[4]=0; }

    p = ap_str(g_netline, "Network: ");
    if (!up) { *ap_str(p, "no link (no NIC bound)") = 0; return; }
    if (net_dhcp_done()) {
        const unsigned char *ip = net_ip();
        p = ap_str(p, "link up, IP ");
        p = ap_int(p, ip[0]); *p++='.'; p = ap_int(p, ip[1]); *p++='.';
        p = ap_int(p, ip[2]); *p++='.'; p = ap_int(p, ip[3]);
    } else {
        p = ap_str(p, "link up, NO DHCP lease (tx ");
        p = ap_int(p, (int)net_tx_frames()); p = ap_str(p, " rx ");
        p = ap_int(p, (int)net_rx_frames()); *p++ = ')';
    }
    *p = 0;
}

/* Sample the link frame counters and derive the LAN-chip activity state.  Runs
 * on the ~0.5 s tray tick: a delta since the last sample means traffic this
 * interval (bit0 = we transmitted, bit1 = we received), and g_net_blink flips
 * every active sample so the dot visibly blinks while data moves. */
static void net_activity_sample(void)
{
    unsigned tx = net_tx_frames(), rx = net_rx_frames();
    int act = 0;
    if (tx != g_net_tx_last) act |= 1;         /* upload  */
    if (rx != g_net_rx_last) act |= 2;         /* download */
    g_net_tx_last = tx; g_net_rx_last = rx;
    g_net_act = act;
    g_net_blink = act ? !g_net_blink : 0;      /* steady (on) when idle */
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
/* Sized for the worst case: "DETACHED (native): " + driver name + disk count +
 * "  FAT vols N (" + three 11-char labels with commas + ")" is ~90 bytes, plus
 * the trailing detach reason (bounded at 64 below); 256 leaves headroom so the
 * unbounded ap_str/ap_int appends below cannot overrun. */
/* 320, not 256: the worst case is a detached box with three labelled volumes,
 * a 64-char reason and a 40-char blocker device, which overran the old size. */
static char g_nat[320];
static void build_natstat(void)
{
    int nblk = uno_blk_count(), i, nnat = 0, nfat = uno_fat_volumes();
    int blocked = 0, stranded = 0;
    const char *why = 0;
    char *p;
    const char *nat0 = 0;                /* first native driver's name (ahci0/nvme0) */
    int uno_pc64_detached(void);
    void uno_pc64_detach_status(int *blocked, int *stranded, const char **why);
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
    /* ...and WHY the machine is where it is. "Native FS:" alone never said
     * whether staying attached was a decision or an accident, which is the
     * first question anyone asks of a box that did not detach. */
    uno_pc64_detach_status(&blocked, &stranded, &why);
    if (why && why[0]) {
        int k;
        p = ap_str(p, stranded ? "  STRANDED: " : blocked ? "  held: " : "  ");
        for (k = 0; k < 64 && why[k]; k++) *p++ = why[k];
    }
    /* ...and WHICH device is holding it (phase D of the detach plan). "held:
     * would lose the only pointer" tells you the class of problem; the PCI
     * function tells you what to go and look at. Empty when the device manager
     * is not in this build or the device is not in its tree - render nothing
     * rather than an empty pair of brackets. */
    if (blocked) {
        const char *dev = uno_dg_blocker();
        if (dev && dev[0]) {
            int k;
            p = ap_str(p, " [");
            for (k = 0; k < 40 && dev[k]; k++) *p++ = dev[k];
            *p++ = ']';
        }
    }
    *p = 0;
}

static void build_sys(unoui_window *w)
{
    int fh = fb_text_h(), lh = fh + 4, y = 4, cw = 420, gx = 8, tx = 20;
    int g0;
    build_tpstat();
    build_ptrstat();
    build_usbstat();
    build_natstat();
    build_acpistat();
    build_sndstat();
    fmt_net();                    /* Network line (IP / lease state) */
    unoui_window_init(w, "System", 400, 100, 1, 1);
    /* header - identity + licensing (the About surface; the notices the
     * bundled open components require live behind View licenses, which
     * opens DOCS\LICENSES.MD in the Browser). Kept up here: the window
     * already runs the height of an 800 px desktop, so anything appended
     * at the bottom is born invisible. */
    unoui_add_label(w, gx, y, "UnoDOS / pc64  -  unoui shell");           y += lh;
    /* Which firmware ACTUALLY started this machine. It used to say UEFI
     * unconditionally, which became false the day pc64 learned to boot from a
     * BIOS - and the System window is precisely where someone goes to find out
     * what a machine is doing. */
    unoui_add_label(w, gx, y,
                    uno_pc64_bootinfo() ? "x86-64 legacy BIOS  -  bare metal  -  10 themes"
                                        : "x86-64 UEFI  -  bare metal  -  10 themes");
    y += lh;
    { int bw = fb_text_w("View licenses") + 26;
      unoui_widget *b;
      unoui_add_label(w, gx, y, "CC BY-NC 4.0 + MIT/Apache-2.0 parts");
      b = unoui_add_button(w, cw - gx - 4 - bw, y - 3, bw, "View licenses", 0);
      b->id = ID_LIC; y += lh + 8; }
    /* accounts & security: opens the Accounts manager (login/RBAC via unosecure) */
    { int aw = fb_text_w("Manage accounts...") + 26;
      unoui_widget *b = unoui_add_button(w, gx, y, aw, "Manage accounts...", 0);
      b->id = ID_ACCT; y += lh + 8; }
    /* grouped hardware readouts: a box per subsystem, rows inside */
#define SYS_GROUP(title, nrows) \
    g0 = y; (void)g0; unoui_add_group(w, gx, y, cw - 2 * gx, (nrows) * lh + fh + 10, title); \
    y += fh + 4
#define SYS_ROW(s) unoui_add_label(w, tx, y, s); y += lh
    SYS_GROUP("Input & USB", 6);
    SYS_ROW(g_tp1);
    SYS_ROW(g_tp2);
    SYS_ROW(g_ptr1);
    SYS_ROW(g_ptr2);
    SYS_ROW(g_usb);
    SYS_ROW(g_usb2);
    y += 12;
    SYS_GROUP("Storage", 1);
    SYS_ROW(g_nat);
    y += 12;
    SYS_GROUP("Network", 1);
    SYS_ROW(g_netline);
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
/* Clock lives in its own translation unit now (analog face + world map). */
void pc64_clock_build(unoui_window *w);
static void build_clock(unoui_window *w) { pc64_clock_build(w); }

/* ---- Install: put UnoDOS on a local disk (backend: installer.c) ---------- */
#define INST_MAXT 12
static char        g_inst_item[INST_MAXT][72];
static const char *g_inst_ptr[INST_MAXT];
static int         g_inst_n, g_inst_sel = -1, g_inst_armed;
static char        g_inst_stat[96] = "Select a target, then Install.";
static int         g_inst_default = 1;       /* "Boot UnoDOS by default"       */
static unoui_widget *g_inst_list_w, *g_inst_prog_w, *g_inst_conf_w;

/* Whole-disk installs erase everything on the target, so they take TWO
 * deliberate acts of different kinds: the word below has to be TYPED into the
 * confirm box, and only then does clicking Install commit. Requiring two
 * different input modalities is the point - a repeated click is muscle memory
 * and a mis-click can supply both halves, whereas typing a specific word
 * cannot happen by accident. Volume installs are non-destructive and skip all
 * of this. */
#define INST_CONFIRM_WORD "ERASE"
static char       g_inst_conf[16];
static unoui_text g_inst_conf_t;
static int        g_inst_conf_wi = -1;       /* widget index, for focus checks */
static void       inst_disarm(void);

static void inst_rescan(void)
{
    int i;
    g_inst_n = uno_inst_scan();
    if (g_inst_n > INST_MAXT) g_inst_n = INST_MAXT;
    for (i = 0; i < g_inst_n; i++) {
        strncpy(g_inst_item[i], uno_inst_desc(i), 71); g_inst_item[i][71] = 0;
        g_inst_ptr[i] = g_inst_item[i];
    }
    g_inst_sel = g_inst_n ? 0 : -1;
    inst_disarm();
    if (g_inst_list_w) { g_inst_list_w->nitems = g_inst_n;
                         unoui_list_set_sel(g_inst_list_w, g_inst_sel); }
    if (g_inst_prog_w) g_inst_prog_w->value = 0;
    strcpy(g_inst_stat, g_inst_n ? "Select a target, then Install."
                                 : "No install targets found.");
}

/* changing target throws away any confirmation already given - the word was
 * typed about a specific disk, and must not carry over to a different one */
static void inst_disarm(void)
{
    g_inst_armed = 0;
    g_inst_conf[0] = 0;
    g_inst_conf_t.len = g_inst_conf_t.caret = g_inst_conf_t.sel = 0;
}

static void inst_select(int n)
{
    if (n < 0 || n >= g_inst_n) return;
    g_inst_sel = n;
    inst_disarm();
    if (g_inst_list_w) unoui_list_set_sel(g_inst_list_w, n);
    strcpy(g_inst_stat, uno_inst_kind(n) == UNO_INST_DISK
           ? "ERASES the whole disk: type " INST_CONFIRM_WORD ", then Install."
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

/* has the confirm word been typed exactly? case-insensitive, but no partial
 * match and no surrounding whitespace - "erase" yes, "eras"/"erased" no */
static int inst_conf_typed(void)
{
    const char *w = INST_CONFIRM_WORD;
    int i = 0;
    while (w[i]) {
        char c = g_inst_conf[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != w[i]) return 0;
        i++;
    }
    return g_inst_conf[i] == 0;
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
    if (k == UNO_INST_DISK) {
        /* stage 1: the word must have been TYPED (case-insensitively, but
           spelled exactly - no partial match, no leading/trailing slop) */
        if (!inst_conf_typed()) {
            char *p = ap_str(g_inst_stat, "Type ");
            p = ap_str(p, INST_CONFIRM_WORD);
            p = ap_str(p, " in the confirm box to erase ");
            { const char *d = uno_inst_desc(g_inst_sel);
              while (*d && p < g_inst_stat + 92) *p++ = *d++; }
            *p = 0;
            return;
        }
        /* stage 2: and THEN Install has to be pressed a second time */
        if (!g_inst_armed) {
            char *p = ap_str(g_inst_stat, "Ready to ERASE ");
            const char *d = uno_inst_desc(g_inst_sel);
            while (*d && p < g_inst_stat + 70) *p++ = *d++;
            p = ap_str(p, " - Install again.");
            *p = 0;
            g_inst_armed = 1;
            return;
        }
    }
    g_inst_armed = 0;
    inst_disarm();                       /* never leave a live confirmation up */
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
    const unoui_theme *t = pc64_shell_theme();
    const char *conf_lab = "To erase a whole disk, type " INST_CONFIRM_WORD ":";
    const char *keys_lab = "Keys: Up/Down pick - C confirm box (Esc leaves) - I installs - R rescans";
    int confw = fb_text_w(conf_lab), instw = fb_text_w("Install") + 24;
    int cw, y;

    /* Content width is measured, not assumed: the confirm row is the widest
       thing here and its width follows the active font, so a fixed 400 px
       window clipped it (and left the Install button un-anchored). */
    cw = 8 + confw + 8 + 80 + 8;
    if (cw < 8 + fb_text_w(keys_lab) + 8) cw = 8 + fb_text_w(keys_lab) + 8;
    if (cw < 400) cw = 400;

    inst_rescan();
    unoui_window_init(w, "Install", 150, 60, cw, 286);
    unoui_add_label(w, 8, 4, "Install UnoDOS to a local disk");
    unoui_add_label(w, 8, 20, "Volumes keep your files; Disks are ERASED.");
    unoui_add_label(w, 8, 34, keys_lab);
    g_inst_list_w = unoui_add_list(w, 8, 52, cw - 16, 92, g_inst_ptr, g_inst_n, g_inst_sel);
    g_inst_list_w->id = ID_ILIST;
    { unoui_widget *c = unoui_add_check(w, 8, 150, "Boot UnoDOS by default", 1);
      c->id = ID_IDEF; }
    /* the typed half of the whole-disk confirmation (ignored for volumes) */
    unoui_add_label(w, 8, 174, conf_lab);
    unoui_text_init(&g_inst_conf_t, g_inst_conf, sizeof g_inst_conf, 0);
    { unoui_widget *e = unoui_add_edit(w, 8 + confw + 8, 170, 80, &g_inst_conf_t);
      e->id = ID_ICONF; g_inst_conf_w = e; g_inst_conf_wi = w->nw - 1; }
    y = 196;
    { unoui_widget *b = unoui_add_button(w, 8, y, 100, "Rescan", 0); b->id = ID_IRESCAN; }
    { unoui_widget *b = unoui_add_button(w, cw - 8 - instw, y, instw, "Install", 0);
      b->id = ID_IGO; }
    g_inst_prog_w = unoui_add_progress(w, 8, 224, cw - 16, 0, 100);
    unoui_add_label(w, 8, 242, g_inst_stat);
    w->r.w = cw + 2 * t->m.frame_w + 2 * t->m.pad;
    w->r.h = win_h_for(262);
}

static void (*const g_build[NNATIVE])(unoui_window *) =
    { build_ctrl, build_edit, build_files, build_sys, build_clock,
      build_setup, build_music, build_unoamp };

/* ---- window management -------------------------------------------------- */
static int g_launch_open;
static int g_menu_scroll, g_menu_hot = -1, g_scroll_tmr;   /* Start-menu scroll */
static unoui_window g_cal;                 /* calendar date-picker popup */
static int g_cal_open, g_cal_y = 2026, g_cal_mo = 1, g_cal_sel = 1;
static unoui_rect g_cal_rect;

/* ---- popovers (phase F) ----------------------------------------------------
 * The window context menu, the taskbar context menu and the ">>" overflow list
 * are the same object - a short list you click one row out of - so they share
 * ONE window, built the way launcher_at() builds the Start menu: a small
 * BARE|TOP window with a list canvas. ui->popup_* is deliberately not reused;
 * it belongs to an owner WIDGET (a menubar or a dropdown), and a context menu
 * has none. */
#define POP_MAXITEMS 20
enum {                                  /* what a row does when it is clicked */
    POP_NONE = 0, POP_SEP, POP_RESTORE, POP_MIN, POP_MAX, POP_SNAPL, POP_SNAPR,
    POP_DESK, POP_GROUP, POP_CLOSE, POP_ACTIVATE, POP_TILE, POP_CASCADE,
    POP_MINALL
};
static unoui_window g_pop;
static int  g_pop_open, g_pop_app = -1, g_pop_hot = -1;
static struct { const char *label; short cmd, arg, icon; } g_pop_it[POP_MAXITEMS];
static int  g_pop_n;

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

/* ---- MRU focus order (phase D) --------------------------------------------
 * The Alt-Tab switcher steps app windows most-recently-focused first, which is
 * what makes "Alt-Tab takes me back to what I was just doing" true. The stack
 * holds app indices; slot 0 is the most recently focused. */
static short g_mru[NAPPS];
static int   g_nmru;

static void wm_note_focus(int a)
{
    int i, j;
    if (a < 0 || a >= NAPPS) return;
    for (i = 0; i < g_nmru; i++) if (g_mru[i] == a) break;
    if (g_nmru && i == 0) return;                   /* already the front       */
    if (i >= g_nmru && g_nmru < NAPPS) g_nmru++;    /* new entry: grow, then
                                                       shift; a full stack drops
                                                       its oldest instead      */
    for (j = (i < g_nmru ? i : g_nmru - 1); j > 0; j--) g_mru[j] = g_mru[j - 1];
    g_mru[0] = (short)a;
}

/* Parked windows: an app that is still OPEN but whose window is out of the
 * scene. Alt+D (show desktop) parks the set; phase B's minimize adopts this
 * flag. A parked window is not in UI.win[], so raising it is a no-op - every
 * route back to the app must unpark first, which open_app does. */
static int g_parked[NAPPS];

static void wm_unpark(int a)
{
    if (a < 0 || a >= NAPPS || !g_open[a] || !g_parked[a]) return;
    g_parked[a] = 0;
    unoui_ui_add(&UI, &g_win[a]);
    raise_win(&g_win[a]);
    g_dirty = 1;
}
static void wm_park(int a)
{
    if (a < 0 || a >= NAPPS || !g_open[a] || g_parked[a]) return;
    remove_win(&g_win[a]);
    g_parked[a] = 1;
    g_dirty = 1;
}

/* ---- virtual desktops (phase E) --------------------------------------------
 * Four fixed desktops, entirely shell policy - unoui knows nothing about them.
 * A desktop IS a set of app windows plus the z-order they were left in, so a
 * switch is remove-set / add-set over the one z-list. Everything else is shared
 * by all four: the wallpaper, the desktop icons, the taskbar and its tray.
 *
 * A parked window is already out of the scene, so a switch never touches it: a
 * window minimized on desktop 2 is still minimized when you come back, and its
 * chip - drawn only while its own desktop is current - is still the only thing
 * that says it exists. The switch itself must therefore never unpark, and
 * wm_desk_apply() below is written so that it cannot.
 *
 * The state lives here because the taskbar (pager cells, which chips to draw)
 * reads it; the machinery that acts on it sits with the rest of the window
 * policy, after focus_next_mru(). */
#define NDESK 4
static int  g_cur_desk;                    /* 0-based; the desktop on screen  */
static signed char g_desk_of[NAPPS];       /* which desktop each app lives on */
/* Saved z-order per desktop, bottom-to-top, storing app index PLUS ONE and
 * terminated by a 0. The +1 is not decoration: a bss array reads as all-zero,
 * and with a plain "-1 terminates" convention every untouched desktop would
 * decode as NAPPS copies of app 0 with no terminator - which is exactly the
 * out-of-bounds write UBSan trapped on the first Alt+Ctrl+Fn of a fresh boot.
 * Encoded this way, zero-initialized means "empty", which is the truth. */
static signed char g_dz[NDESK][NAPPS];

static void wm_desk_switch(int d);

static int wm_in_scene(int a)
{
    int i;
    for (i = 0; i < UI.nwin; i++) if (UI.win[i] == &g_win[a]) return 1;
    return 0;
}

/* ---- link groups (phase F) -------------------------------------------------
 * Grouping v1 is a LINK: windows keep their own frames and simply act as one
 * set - move, raise, minimize/restore and (with desktops) switch together. That
 * is the whole behaviour of grouping without a container concept in unoui, and
 * it leaves the tabbed-frame version (v2, explicitly deferred) free to adopt
 * this same id namespace later.
 *
 * The id is shell state: 0 = ungrouped, 1..WM_NGROUP = a group. unoui learns of
 * it through exactly one hook (unoui_win_badge), which paints the title-bar
 * dot; it has no other notion of a group. */
#define WM_NGROUP 2                       /* the menu offers "A" and "B"       */
static unsigned char g_group[NAPPS];

/* Fill `out` with every OPEN app linked to `a`, `a` itself included, and return
 * the count. An ungrouped app is a set of one, so no caller needs a special
 * case for "not in a group". */
static int wm_group_set(int a, int *out)
{
    int i, n = 0;
    if (a < 0 || a >= NAPPS || !g_open[a]) return 0;
    if (!g_group[a]) { out[0] = a; return 1; }
    for (i = 0; i < NAPPS; i++)
        if (g_open[i] && g_group[i] == g_group[a]) out[n++] = i;
    return n;
}

/* the badge index unoui paints in a window's title bar, or UI_BADGE_NONE */
static int shell_win_badge(const unoui_window *w)
{
    int a;
    for (a = 0; a < NAPPS; a++)
        if (&g_win[a] == w) return g_group[a] ? g_group[a] - 1 : UI_BADGE_NONE;
    return UI_BADGE_NONE;
}

/* Lift `a`'s whole set above everything else. The peers go up first, keeping
 * their relative order, and the window the user actually touched goes up last,
 * so it ends on top: the spec's "members directly above the grabbed one" would
 * bury the very window that was just clicked.
 *
 * Raising rewrites UI.win[], and both cap_win and focus_win are INDEXES into
 * it, so each is re-derived from the window it named rather than left dangling
 * - a stale cap_win would hand the rest of a live drag to the wrong window. */
static void wm_raise_group(int a)
{
    int set[NAPPS], n, i, fwi = UI.focus_wi;
    unoui_window *capw = (UI.cap_mode != UI_CAP_NONE &&
                          UI.cap_win >= 0 && UI.cap_win < UI.nwin)
                       ? UI.win[UI.cap_win] : 0;
    if (a < 0 || a >= NAPPS || !g_open[a] || g_parked[a] || !g_group[a]) return;
    n = wm_group_set(a, set);
    if (n < 2) return;
    for (i = 0; i < n; i++)
        if (set[i] != a && !g_parked[set[i]])
            unoui_bring_to_front(&UI, &g_win[set[i]]);
    unoui_bring_to_front(&UI, &g_win[a]);
    UI.focus_wi = fwi;
    if (capw) for (i = 0; i < UI.nwin; i++)
        if (UI.win[i] == capw) { UI.cap_win = i; break; }
    g_dirty = 1;
}

/* ---- dragging a link group -------------------------------------------------
 * unoui moves only the window it captured, and knows nothing about groups. So
 * the shell watches the captured window's origin across the events that move
 * it and applies the same delta to its peers. A peer that hits the keep-on-
 * screen clamp simply stops there and the set spreads a little; that is the
 * same rule a single window drag obeys, so it cannot go anywhere unreachable. */
static int g_gdrag = -1;                 /* app whose window unoui captured   */
static int g_gdrag_x, g_gdrag_y;         /* its origin at the previous event  */
static int g_gdrag_snap;                 /* ...and its snap state then        */

/* The app whose TITLE BAR covers (mx, my), topmost first, or -1. The context
 * gesture wants the bar only: a right-click in a window's body belongs to the
 * app (the Editor, Files and the Browser all use it). */
static int win_titlebar_app_at(int mx, int my)
{
    int k, i, th = UI.theme->m.title_h;
    for (k = UI.nwin - 1; k >= 0; k--) {
        unoui_window *w = UI.win[k];
        if (w->flags & UI_WIN_BARE) continue;
        if (mx < w->r.x || mx >= w->r.x + w->r.w) continue;
        if (my < w->r.y || my >= w->r.y + w->r.h) continue;
        if (my >= w->r.y + th) return -1;          /* the body, not the bar   */
        for (i = 0; i < NAPPS; i++)
            if (&g_win[i] == w && g_open[i]) return i;
        return -1;                                 /* chrome, not an app      */
    }
    return -1;
}

static void wm_group_drag(void)
{
    int a = -1, i, dx, dy, set[NAPPS], n;
    if (UI.cap_mode == UI_CAP_WINDOW && UI.cap_win >= 0 && UI.cap_win < UI.nwin)
        for (i = 0; i < NAPPS; i++)
            if (UI.win[UI.cap_win] == &g_win[i] && g_open[i]) { a = i; break; }
    if (a < 0 || !g_group[a]) { g_gdrag = -1; return; }
    if (a != g_gdrag) {                        /* the grab: take a baseline   */
        g_gdrag = a; g_gdrag_x = g_win[a].r.x; g_gdrag_y = g_win[a].r.y;
        g_gdrag_snap = g_win[a].snap;
        return;
    }
    if (g_win[a].snap != g_gdrag_snap) {
        /* phase C just un-snapped it: the window was handed a whole new rect
         * under the pointer, which is not a translation of the set. Re-baseline
         * and let the NEXT move carry the peers, or every one of them would
         * jump by the un-snap's offset. */
        g_gdrag_snap = g_win[a].snap;
        g_gdrag_x = g_win[a].r.x; g_gdrag_y = g_win[a].r.y;
        return;
    }
    dx = g_win[a].r.x - g_gdrag_x; dy = g_win[a].r.y - g_gdrag_y;
    g_gdrag_x = g_win[a].r.x; g_gdrag_y = g_win[a].r.y;
    if (!dx && !dy) return;
    n = wm_group_set(a, set);
    for (i = 0; i < n; i++) {
        int b = set[i];
        if (b == a || g_parked[b]) continue;
        g_win[b].r.x += dx; g_win[b].r.y += dy;
        g_win[b].snap = UI_SNAP_NONE;      /* it was moved: it is not snapped */
        unoui_clamp_window(&UI, &g_win[b]);
    }
    g_dirty = 1;
}

/* the taskbar background: a bare window draws no chrome, so a non-interactive
 * canvas paints the bar face + top highlight under the buttons. */
/* forward decls (taskbar events fire these, defined below) */
static void toggle_launcher(void);
static void menu_refresh(void);
static void build_desktop(void);
static void open_app(int a);
static void minimize_app(int a);
static void restore_app(int a);
static void pop_overflow(int px, int py);      /* the ">>" chip's app list   */
static void pop_window_menu(int a, int x, int y);
static void pop_task_menu(int x, int y);
static void pop_close(void);
static void fmt_clock(int uptime_secs);
static void fmt_batt(void);
static int  batt_icon_w(void);
static void draw_batt_icon(int x, int y, int bh, int pct, fb_px outline);
int pc64_write_canvas_index(void);    /* pc64_write.c: doc-canvas widget index */
int pc64_files_canvas_index(void);    /* pc64_files.c: pane widget index      */

/* ---- taskbar: a single canvas we draw + hit-test by hand for full control -- *
 * All layout is measured from the live font so the bar stays aligned whatever
 * face/scale is active (draw + hit-test share these functions). */
/* The bar is: Start button | window chips | tray. The Start button opens the
 * app launcher and is always visible, so the launcher is reachable even when a
 * window covers the desktop (the right-click gesture can't reach a covered
 * desktop; Ctrl-Esc also toggles it). Chips start after the Start button. */
static int tb_chip_w(void)   { int w = 40 + fb_text_w("Manager"); return w < 108 ? 108 : w; }
static int tb_chip_gap(void) { return tb_chip_w() + 4; }
static int tb_start_logo_sz(void) { int s = fb_text_h() + 2; return s < 12 ? 12 : s; }
static int tb_start_w(void)  { return 8 + tb_start_logo_sz() + 6 + fb_text_w("Start") + 10; }
/* the desktop pager: [1][2][3][4], between the Start button and the chips.
 * Draw and hit-test both derive their x from these, so the two cannot drift. */
static int tb_pager_cell(void) { int w = fb_text_w("4") + 14; return w < 22 ? 22 : w; }
static int tb_pager_gap(void)  { return tb_pager_cell() + 3; }
static int tb_pager_x(void)    { return 6 + tb_start_w() + 8; }
static int tb_pager_w(void)    { return NDESK * tb_pager_gap() - 3; }
static int tb_chip_x(void)   { return tb_pager_x() + tb_pager_w() + 8; }
/* does desktop d hold any open window (the pager's occupancy dot)? */
static int tb_desk_used(int d)
{
    int a;
    for (a = 0; a < NAPPS; a++) if (g_open[a] && g_desk_of[a] == d) return 1;
    return 0;
}

/* app index of the focused window, or -1 (used to highlight its taskbar chip) */
static int g_hidden_app = -1;            /* see drag_scene_without() */
static int focused_app(void)
{
    int i;
    if (g_hidden_app >= 0) return g_hidden_app;   /* lifted out for one pass */
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

/* Battery tray-chip content width (excluding chip padding) for the active
 * display mode; 0 when there is no battery so the chip is hidden entirely. */
static int tray_batt_cw(void)
{
    if (!g_batt[0]) return 0;
    switch (g_batt_mode) {
    case BATT_ICON: return batt_icon_w();
    case BATT_PCT:  return fb_text_w(g_batt);
    default:        return batt_icon_w() + 6 + fb_text_w(g_batt);   /* BATT_BOTH */
    }
}

/* ---- chip strip layout (phase F: overflow) ---------------------------------
 * The chips used to stop at the tray with a bare `break`, so the apps past the
 * edge simply vanished off the bar with nothing to say so and no way back to
 * them but Alt-Tab. Now the last slot becomes a ">>" chip opening a popover of
 * the rest. Draw and hit-test both derive the strip from these three
 * functions, so a chip can only be clicked where one was drawn. */

/* the tray's left edge (LAN | battery | clock), in screen coords */
static int tb_tray_x(void)
{
    int bcw = tray_batt_cw();
    return FB_W - (fb_text_w(g_clock) + 16) - 6
           - (bcw ? bcw + 20 : 0)
           - (g_net[0] ? fb_text_w(g_net) + 16 + 12 : 0);
}

/* how many chips fit between the Start button (plus whatever else precedes
 * them) and the tray */
static int tb_maxchips(void)
{
    int avail = tb_tray_x() - 4 - tb_chip_x();
    int cw = tb_chip_w(), gap = tb_chip_gap(), n = 0;
    while (n * gap + cw <= avail) n++;
    return n;
}

/* The apps that get a chip, in bar order: open, and on the desktop currently
 * on screen (phase E). The draw, the hit-test and the overflow popover all
 * read this ONE list, so they cannot disagree about what is on the bar. */
static int tb_open_list(int *out)
{
    int i, n = 0;
    for (i = 0; i < NAPPS; i++)
        if (g_open[i] && g_desk_of[i] == g_cur_desk) out[n++] = i;
    return n;
}

/* How many of `n` open apps get a chip of their own. When they do not all fit,
 * the final slot is the ">>" chip rather than an app, so one fewer shows. */
static int tb_nvis(int n)
{
    int mx = tb_maxchips();
    if (mx <= 0) return 0;
    return (n <= mx) ? n : mx - 1;
}

/* the app whose chip covers bar-x `px`: -2 = the ">>" overflow chip, -1 = no
 * chip there (Start button, bare bar or the tray) */
static int tb_chip_app_at(int px)
{
    int list[NAPPS], nopen = tb_open_list(list), nvis = tb_nvis(nopen);
    int x = tb_chip_x(), cw = tb_chip_w(), k;
    for (k = 0; k < nvis; k++, x += tb_chip_gap())
        if (px >= x && px < x + cw) return list[k];
    if (nopen > nvis && px >= x && px < x + cw) return -2;
    return -1;
}

/* Hover tooltip for the LAN chip: current IP address + negotiated link speed,
 * drawn as a small panel sitting just above the chip (chip_x = its left edge,
 * bar_y = the taskbar's top). Link speed shows "n/a" until a driver reports it. */
static void draw_net_tooltip(int chip_x, int bar_y)
{
    const unoui_theme *t = UI.theme;
    char ipline[40], spline[32];
    char *p; int mbps = net_link_speed_mbps();
    int fh = fb_text_h(), pad = 6, lh = fh + 3;

    p = ap_str(ipline, "IP: ");
    if (net_dhcp_done()) {
        const unsigned char *ip = net_ip();
        p = ap_int(p, ip[0]); *p++='.'; p = ap_int(p, ip[1]); *p++='.';
        p = ap_int(p, ip[2]); *p++='.'; p = ap_int(p, ip[3]);
    } else p = ap_str(p, "(no lease)");
    *p = 0;

    p = ap_str(spline, "Link: ");
    if (mbps >= 1000)    { p = ap_int(p, mbps / 1000); p = ap_str(p, " Gbps"); }
    else if (mbps > 0)   { p = ap_int(p, mbps);        p = ap_str(p, " Mbps"); }
    else                   p = ap_str(p, "up (speed n/a)");
    *p = 0;

    { int tw1 = fb_text_w(ipline), tw2 = fb_text_w(spline);
      int bw = (tw1 > tw2 ? tw1 : tw2) + pad * 2;
      int bhh = lh * 2 + pad * 2 - 3;
      int bx = chip_x, byy = bar_y - bhh - 4;
      if (bx + bw > FB_W) bx = FB_W - bw;      /* keep it on-screen */
      if (bx < 0) bx = 0;
      if (byy < 0) byy = 0;
      /* the taskbar canvas is clipped to the bar; widen so the tooltip can sit
         above it. draw_one restores the window clip after this callback. */
      fb_set_clip(0, 0, FB_W, FB_H);
      fb_round_rect_a(bx, byy, bw, bhh, 6, t->pal.win_bg, 250, FB_CORNER_ALL);
      fb_text(bx + pad, byy + pad,      ipline, t->pal.text, -1);
      fb_text(bx + pad, byy + pad + lh, spline, t->pal.text, -1);
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
    /* Start button (left): the UnoDOS brand mark + "Start", accent-coloured.
       Opens the app launcher - reachable even when a window covers the desktop.
       Shows pressed while the launcher is open. */
    { int sw = tb_start_w(), sx0 = r.x + 6, ls = tb_start_logo_sz();
      int fh = fb_text_h(), gx = sx0 + 8, ly = by + (bh - ls) / 2;
      if (modern) fb_round_rect_a(sx0, by, sw, bh, cr, t->pal.accent,
                                  g_launch_open ? 200 : 255, FB_CORNER_ALL);
      else        tb_panel(sx0, by, sw, bh, t->pal.accent, g_launch_open);
      pc64_start_logo(gx, ly, ls, t->pal.accent_text);
      fb_text(gx + ls + 6, by + (bh - fh) / 2, "Start", t->pal.accent_text, -1); }
    /* the desktop pager: one cell per virtual desktop, the current one filled
       with the accent, a 2 px dot under any desktop that has windows on it.
       Shared chrome - it looks the same on all four desktops. */
    { int px = r.x + tb_pager_x(), pw = tb_pager_cell(), fh = fb_text_h(), d;
      for (d = 0; d < NDESK; d++) {
          int cur = (d == g_cur_desk);
          char lbl[2];
          lbl[0] = (char)('1' + d); lbl[1] = 0;
          if (modern) fb_round_rect_a(px, by, pw, bh, cr,
                                      cur ? t->pal.accent : t->pal.text,
                                      cur ? 255 : 18, FB_CORNER_ALL);
          else        tb_panel(px, by, pw, bh, cur ? t->pal.accent : t->pal.face, cur);
          fb_text(px + (pw - fb_text_w(lbl)) / 2, by + (bh - fh) / 2 - 2, lbl,
                  cur ? t->pal.accent_text : t->pal.text, -1);
          if (tb_desk_used(d))
              fb_fill_rect(px + pw / 2 - 3, by + bh - 6, 6, 2,
                           cur ? t->pal.accent_text : t->pal.text_dim);
          px += tb_pager_gap();
      } }
    /* one chip per open window ON THIS DESKTOP: mini icon + name, highlighted
       if it's active. Chips stop before the tray (clock/battery) instead of
       colliding. */
    x = r.x + tb_chip_x();
    { int cw = tb_chip_w(), fh = fb_text_h(), es = bh - 4 > 16 ? 16 : bh - 4;
      int list[NAPPS], nopen = tb_open_list(list), nvis = tb_nvis(nopen), k;
    for (k = 0; k < nvis; k++) {
        int d, park;
        unoui_rect eb;
        i = list[k];
        d = (i == act) ? 1 : 0;
        park = g_parked[i];                  /* minimized: running, off-screen */
        /* a parked chip reads as "still running, not on screen": fainter
           panel, no accent underline (it cannot be the active window) and
           dimmed text. Same palette, no new colours. */
        if (modern) {
            if (d) { fb_round_rect_a(x, by, cw, bh, cr, t->pal.accent, 48, FB_CORNER_ALL);
                     fb_fill_rect(x + 8, by + bh - 2, cw - 16, 2, t->pal.accent); }
            else     fb_round_rect_a(x, by, cw, bh, cr, t->pal.text,
                                     park ? 8 : 18, FB_CORNER_ALL);
        } else tb_panel(x, by, cw, bh, park ? t->pal.win_bg : t->pal.face, d);
        { int dd = modern ? 0 : d;
          eb.x = x + 4 + dd; eb.y = by + (bh - es) / 2 + dd; eb.w = es; eb.h = es;
          pc64_icon_emblem(app_icon(i), eb);
          fb_set_clip(x + es + 6, by, cw - es - 8, bh);        /* keep the name in the chip */
          fb_text(x + es + 8 + dd, by + (bh - fh) / 2 + dd, app_short(i),
                  park ? t->pal.text_dim : t->pal.text, -1);
          fb_set_clip(r.x, r.y, r.w, r.h); }                  /* back to the bar */
        x += tb_chip_gap();
    }
    /* the overflow chip: the apps that did not fit, reachable instead of gone */
    if (nopen > nvis) {
        char cnt[8]; char *p = cnt;
        *p++ = '+'; p = ap_int(p, nopen - nvis); *p = 0;
        if (modern) fb_round_rect_a(x, by, cw, bh, cr, t->pal.text, 18, FB_CORNER_ALL);
        else        tb_panel(x, by, cw, bh, t->pal.face, 0);
        fb_set_clip(x + 4, by, cw - 8, bh);
        fb_text(x + 8, by + (bh - fh) / 2, ">>", t->pal.text, -1);
        fb_text(x + 8 + fb_text_w(">>  "), by + (bh - fh) / 2, cnt,
                t->pal.text_dim, -1);
        fb_set_clip(r.x, r.y, r.w, r.h);
    } }
    /* system tray, right-aligned: LAN chip | battery | clock. Each chip is
       placed to the LEFT of the previous one; cxx tracks the running left edge. */
    { int fh = fb_text_h();
      int cw = fb_text_w(g_clock) + 16, cxx = r.x + r.w - cw - 6;
      if (modern) fb_round_rect_a(cxx, by, cw, bh, cr, t->pal.text, 16, FB_CORNER_ALL);
      else        tb_panel(cxx, by, cw, bh, t->pal.field_bg, 1);
      fb_text(cxx + 8, by + (bh - fh) / 2, g_clock, modern ? t->pal.text : t->pal.field_text, -1);
      { int bcw = tray_batt_cw();        /* battery chip: icon / percent / both */
        if (bcw) {
          fb_px tc = modern ? t->pal.text : t->pal.field_text;
          int bw = bcw + 16, bx = cxx - bw - 4, ix = bx + 8;
          if (modern) fb_round_rect_a(bx, by, bw, bh, cr, t->pal.text, 16, FB_CORNER_ALL);
          else        tb_panel(bx, by, bw, bh, t->pal.field_bg, 1);
          if (g_batt_mode != BATT_PCT) {                 /* icon or both */
              draw_batt_icon(ix, by, bh, g_batt_pct, tc);
              ix += batt_icon_w() + 6;
          }
          if (g_batt_mode != BATT_ICON)                  /* percent or both */
              fb_text(ix, by + (bh - fh) / 2, g_batt, tc, -1);
          cxx = bx;
      } }
      if (g_net[0]) {                    /* LAN chip: activity dot + label */
          int lease = net_dhcp_done();
          /* Blink on traffic: yellow while uploading, green while downloading;
             steady lease/no-lease colour when idle. */
          unsigned dot;
          if (g_net_act && !g_net_blink) dot = t->pal.win_bg;        /* blink off phase */
          else if (g_net_act & 1)        dot = FB_RGB(232, 200, 40); /* upload  (tx) */
          else if (g_net_act & 2)        dot = FB_RGB(60, 200, 90);  /* download(rx) */
          else if (lease)                dot = FB_RGB(60, 200, 90);  /* green: lease */
          else                           dot = FB_RGB(232, 170, 40); /* amber: no IP */
          int bw = fb_text_w(g_net) + 16 + 12, bx = cxx - bw - 4;
          int ds = 6, dy = by + (bh - ds) / 2;
          if (modern) fb_round_rect_a(bx, by, bw, bh, cr, t->pal.text, 16, FB_CORNER_ALL);
          else        tb_panel(bx, by, bw, bh, t->pal.field_bg, 1);
          fb_fill_rect(bx + 8, dy, ds, ds, dot);
          fb_text(bx + 8 + ds + 5, by + (bh - fh) / 2, g_net,
                  modern ? t->pal.text : t->pal.field_text, -1);
          g_net_cx = bx; g_net_cy = by; g_net_cw = bw; g_net_ch = bh;  /* for hover */
          if (g_net_hover) draw_net_tooltip(bx, r.y);
      } }
}

static int taskbar_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    int px, i;
    (void)w; (void)ctx;
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    px = e->x - g_task.r.x;
    if (px >= 6 && px < 6 + tb_start_w()) { toggle_launcher(); return 1; }   /* Start button */
    /* the desktop pager. It needs nothing from focus_app()/g_mru, so the
       read-focus-before-the-press trap the chip toggle below documents does
       not apply here - a pager cell means one thing whatever had focus. The
       band between cells is swallowed rather than passed on, so a near miss
       is a no-op instead of falling through to something else. */
    { int pp = px - tb_pager_x(), d;
      if (pp >= 0 && pp < tb_pager_w()) {
          d = pp / tb_pager_gap();
          if (d >= 0 && d < NDESK && pp - d * tb_pager_gap() < tb_pager_cell())
              wm_desk_switch(d);
          return 1;
      } }
    i = tb_chip_app_at(px);
    if (i == -2) { pop_overflow(e->x, e->y); return 1; }   /* the ">>" chip */
    if (i >= 0) {
        /* The modern chip toggle: parked -> restore + raise; unfocused ->
           raise; the app that already has focus -> park it.
           "Focused" is read off the MRU stack, NOT focused_app(): the
           press that got us here already raised the TASKBAR (it is a
           UI_WIN_TOP window like any other), so by now focused_app() is
           -1 and every chip click would read as "not focused". g_mru[0]
           still names the app that had focus, because raising shell
           chrome never calls wm_note_focus(). */
        if (g_parked[i])                        restore_app(i);
        else if (g_nmru && g_mru[0] == i)       minimize_app(i);
        else                                    open_app(i);
        return 1;
    }
    return 0;
}
static unoui_canvas g_taskcv = { taskbar_draw, taskbar_event, 0 };

/* the taskbar is one canvas; it reads g_open live, so "rebuild" is just a
 * redraw request (kept as a name the open/close paths already call). */
static void rebuild_taskbar(void) { g_dirty = 1; }

static void build_taskbar(void)
{
    set_workarea();                    /* TASKH follows the font: re-publish */
    unoui_window_init(&g_task, "", 0, FB_H - TASKH, FB_W, TASKH);
    g_task.flags = UI_WIN_BARE | UI_WIN_TOP;
    unoui_add_canvas(&g_task, 0, 0, FB_W, TASKH, &g_taskcv);
}

/* the order icons appear in, as app indices */
static void desk_order(int *out, int n)
{
    int i, j;
    for (i = 0; i < n; i++) out[i] = i;
    if (!g_desk_sort) return;
    for (i = 1; i < n; i++) {                       /* insertion sort by name */
        int v = out[i];
        for (j = i; j > 0 && strcmp(app_short(out[j - 1]), app_short(v)) > 0; j--)
            out[j] = out[j - 1];
        out[j] = v;
    }
}

/* ---- wallpaper painter (registered as unoui_wallpaper) --------------------
 * Draws the whole-screen backdrop for the chosen wallpaper.  Returns 1 when it
 * painted, 0 for "Theme default" so the toolkit falls back to the theme's own
 * desktop.  All styles are procedural (no image assets) and the result is
 * captured by the UNO_BG_CACHE, so the per-frame cost is one blit. */
static int pc64_wallpaper_paint(const unoui_theme *t, int W, int H)
{
    switch (g_wallpaper) {
    default:
    case 0:                                     /* Theme default: fall through */
        return 0;
    case 1: {                                   /* Midnight: navy + starfield  */
        unsigned s = 0x1234567u; int i;
        fb_grad_v(0, 0, W, H, FB_RGB(0x0a, 0x0e, 0x24), FB_RGB(0x02, 0x03, 0x0b));
        for (i = 0; i < 140; i++) {
            int x, y, b;
            s = s * 1664525u + 1013904223u; x = (int)((s >> 8) % (unsigned)W);
            s = s * 1664525u + 1013904223u; y = (int)((s >> 8) % (unsigned)(H * 3 / 4));
            s = s * 1664525u + 1013904223u; b = 90 + (int)((s >> 8) % 140u);
            fb_blend_rect(x, y, 1 + (b > 200), 1 + (b > 200), FB_RGB(b, b, b + 20 > 255 ? 255 : b + 20), b);
        }
        return 1; }
    case 2:                                     /* Sunrise: warm vertical wash  */
        fb_grad_v(0, 0, W, H, FB_RGB(0x2a, 0x3a, 0x66), FB_RGB(0xff, 0x9a, 0x5a));
        fb_round_rect_a(W * 2 / 3, H / 2, W / 2, H / 2, H / 4,
                        FB_RGB(0xff, 0xe0, 0x9a), 40, FB_CORNER_ALL);
        return 1;
    case 3:                                     /* Evergreen: deep green depth  */
        fb_grad_v(0, 0, W, H, FB_RGB(0x0c, 0x2b, 0x1e), FB_RGB(0x04, 0x12, 0x0d));
        fb_round_rect_a(-W / 5, H / 3, W * 3 / 4, H * 3 / 4, H / 2,
                        FB_RGB(0x3f, 0xc0, 0x7a), 10, FB_CORNER_ALL);
        return 1;
    case 4:                                     /* Aurora: accent blobs (any theme) */
        fb_grad_v(0, 0, W, H, FB_RGB(0x16, 0x19, 0x22), FB_RGB(0x10, 0x14, 0x1d));
        fb_round_rect_a(W - W * 3 / 5 - 30, -H / 5, W * 3 / 5, H * 3 / 5, H / 3,
                        t->pal.accent, 16, FB_CORNER_ALL);
        fb_round_rect_a(-W / 4, H / 2, W * 3 / 5, H * 3 / 5, H / 3,
                        t->pal.accent, 9, FB_CORNER_ALL);
        return 1;
    case 5: {                                   /* Graphite grid                */
        int gx;
        fb_fill_rect(0, 0, W, H, FB_RGB(0x1b, 0x1e, 0x24));
        for (gx = 0; gx < W; gx += 32) fb_blend_rect(gx, 0, 1, H, FB_RGB(0xff, 0xff, 0xff), 10);
        for (gx = 0; gx < H; gx += 32) fb_blend_rect(0, gx, W, 1, FB_RGB(0xff, 0xff, 0xff), 10);
        return 1; }
    case 6:                                     /* Slate: flat neutral          */
        fb_fill_rect(0, 0, W, H, FB_RGB(0x30, 0x36, 0x40));
        return 1;
    }
}

static void build_desktop(void)
{
    int k, fh = fb_text_h();
    int ich = 34 + fh, pitch = ich + 8, colw = 20 + fb_text_w("MMMMMMMM");
    int percol, percol_rows, order[NAPPS];
    unoui_window_init(&g_desk, "", 0, 0, FB_W, FB_H - TASKH);
    g_desk.flags = UI_WIN_BARE | UI_WIN_BOTTOM;
    percol      = (FB_H - TASKH - 20) / pitch; if (percol < 1) percol = 1;
    percol_rows = (FB_W - 16) / colw;           if (percol_rows < 1) percol_rows = 1;
    desk_order(order, NAPPS);
    { int kk = 0;
    for (k = 0; k < NAPPS; k++) {
        int i = order[k];
        int col, row, ix, iy;
        unoui_widget *ic;
        if (app_hidden(i)) continue;           /* no icon for absent apps */
        col = g_desk_flow ? (kk % percol_rows) : (kk / percol);
        row = g_desk_flow ? (kk / percol_rows) : (kk % percol);
        kk++;
        ix = 16 + col * colw; iy = 14 + row * pitch;
        if (i < 32 && g_icon_pos[i].placed) {      /* the user put it here */
            ix = g_icon_pos[i].x; iy = g_icon_pos[i].y;
        }
        ic = unoui_add_icon(&g_desk, ix, iy, app_short(i));
        ic->r.w = colw - 20; ic->r.h = ich; /* room for emblem + label         */
        ic->icon = app_icon(i);             /* -> pc64_icon_art draws its art  */
        ic->id  = ID_LAUNCH0 + i;
    } }
}

/* pull a window fully into the work area (screen minus the taskbar). unoui's
 * own clamp keeps a GRABBABLE strip on screen, which is right for a drag but
 * wrong for placing a freshly built window, so this stays: a window the shell
 * opens is pulled fully inside. */
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

/* the user-app slot's canvas (same hosting, separate identity) */
static void userapp_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{ (void)w; (void)ctx; unoapp_user_paint(r); }
static int userapp_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    (void)w; (void)ctx;
    if (unoapp_user_input((const unoui_event *)ev)) { g_dirty = 1; return 1; }
    return 0;
}

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

void pc64_sshapp_open(void);
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
    if (a == EX_SSH) {                 /* native windowed SSH client canvas */
        unoui_canvas *pc64_sshapp_canvas(void);
        aw = 470; ah = 300;
        unoui_window_init(&g_win[a], app_name(a), 40, 30,
                          aw + 2*m->frame_w + 2*m->pad, ah + m->title_h + 2*m->pad + m->frame_w);
        unoui_widget_fill(unoui_add_canvas(&g_win[a], 0, 0, aw, ah, pc64_sshapp_canvas()));
        g_win[a].flags |= UI_WIN_RESIZE;
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
    if (a == EX_PHOTOS) {              /* the viewer module fills its window */
        photos_ensure();
        if (g_photos) { g_photos->build(&g_win[a]); return; }
        unoui_window_init(&g_win[a], "Photos", 60, 40, 300, 90);
        unoui_add_label(&g_win[a], 8, 10, "APPS\\PHOTOS.UNO is missing");
        unoui_add_label(&g_win[a], 8, 28, "This system ships without the viewer.");
        return;
    }
    if (a == EX_UOCALC) {              /* the spreadsheet fills its window   */
        uocalc_ensure();
        if (g_uocalc) { g_uocalc->build(&g_win[a]); return; }
        unoui_window_init(&g_win[a], "UnoCalc", 60, 40, 320, 90);
        unoui_add_label(&g_win[a], 8, 10, "APPS\\UOCALC.UNO is missing");
        return;
    }
    if (a == EX_UOWORD) {              /* the word processor fills its window */
        uoword_ensure();
        if (g_uoword) { g_uoword->build(&g_win[a]); return; }
        unoui_window_init(&g_win[a], "UnoWord", 60, 40, 320, 90);
        unoui_add_label(&g_win[a], 8, 10, "APPS\\UOWORD.UNO is missing");
        unoui_add_label(&g_win[a], 8, 28, "This system ships without the word processor.");
        return;
    }
    if (a == EX_STUDIO) {              /* the IDE module fills its own window */
        studio_ensure();
        if (g_studio) { g_studio->build(&g_win[a]); return; }
        unoui_window_init(&g_win[a], "Studio", 60, 40, 280, 90);
        unoui_add_label(&g_win[a], 8, 10, "APPS\\STUDIO.UNO is missing");
        unoui_add_label(&g_win[a], 8, 28, "This system ships without the IDE.");
        return;
    }
    if (a == EX_USERAPP) {             /* the app Studio just built */
        static unoui_canvas ucv;
        unoapp_user_size(&aw, &ah);
        if (aw < 140) aw = 140;
        if (ah < 100) ah = 100;
        ucv.draw  = userapp_draw; ucv.event = userapp_event; ucv.ctx = 0;
        unoui_window_init(&g_win[a], app_name(a), 50, 26,
                          aw + 2 * m->frame_w + 2 * m->pad,
                          (ah - APP_TBAR) + m->title_h + 2 * m->pad + m->frame_w);
        unoui_add_canvas(&g_win[a], 0, 0, aw, ah - APP_TBAR, &ucv);
        return;
    }
    if (a == EX_PYAPP) {               /* a running Python app (a UnoUuiApp) */
        if (g_pyapp) { g_pyapp->build(&g_win[a]); return; }
        unoui_window_init(&g_win[a], "Python", 60, 40, 240, 80);
        unoui_add_label(&g_win[a], 8, 10, "No Python app running.");
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
    /* Virtual desktops: these are single-instance apps, so "open" an app that
     * is already up on another desktop means GO TO IT - dragging its window
     * across would lose the layout the user left there. A window that is not
     * open yet lands on the desktop being looked at. */
    if (g_open[a]) { if (g_desk_of[a] != g_cur_desk) wm_desk_switch(g_desk_of[a]); }
    else             g_desk_of[a] = (signed char)g_cur_desk;
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
        else if (a == EX_SSH)       pc64_sshapp_open();          /* ssh client    */
        else if (a == EX_STUDIO)    { if (g_studio && g_studio->opened) g_studio->opened(); }
        else if (a == EX_PHOTOS)    { if (g_photos && g_photos->opened) g_photos->opened(); }
        else if (a == EX_UOWORD)    { if (g_uoword && g_uoword->opened) g_uoword->opened(); }
        else if (a == EX_UOCALC)    { if (g_uocalc && g_uocalc->opened) g_uocalc->opened(); }
        else if (a == EX_PYAPP)     { if (g_pyapp && g_pyapp->opened) g_pyapp->opened(); }
        else if (a == EX_USERAPP)   { }                          /* run() opened it */
        else if (app_is_bridge(a))  unoapp_open(a - NNATIVE);    /* bridge app    */
        rebuild_taskbar();
        session_save();                 /* remember the open set for next boot */
    } else if (g_parked[a]) {          /* show-desktop / minimize: back in */
        wm_unpark(a);
        rebuild_taskbar();             /* the chip stops being dimmed      */
        session_save();                /* ...and stops being parked on disk */
    }
    else raise_win(&g_win[a]);
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }  /* Start-menu closes */
    /* focus the opened window + its canvas (closing the launcher above moved
     * focus to the taskbar, so do this last). */
    { int fi; for (fi = 0; fi < UI.nwin; fi++) if (UI.win[fi] == &g_win[a]) { UI.focus_win = fi; break; } }
    if (a >= NNATIVE) UI.focus_wi = 0;   /* focus the app's canvas for keyboard */
    if (a == APP_EDIT)  { int wi = pc64_write_canvas_index(); if (wi >= 0) UI.focus_wi = wi; }
    if (a == APP_FILES) { int wi = pc64_files_canvas_index(); if (wi >= 0) UI.focus_wi = wi; }
    if (a == EX_STUDIO && g_studio && g_studio->canvas_index)
        { int wi = g_studio->canvas_index(); if (wi >= 0) UI.focus_wi = wi; }
    if (a == EX_UOCALC && g_uocalc && g_uocalc->canvas_index)
        return g_uocalc->canvas_index();
    if (a == EX_UOWORD && g_uoword && g_uoword->canvas_index)
        return g_uoword->canvas_index();
    if (a == EX_PHOTOS && g_photos && g_photos->canvas_index)
        { int wi = g_photos->canvas_index(); if (wi >= 0) UI.focus_wi = wi; }
    if (a == EX_PYAPP && g_pyapp && g_pyapp->canvas_index)
        { int wi = g_pyapp->canvas_index(); if (wi >= 0) UI.focus_wi = wi; }
    /* native games scale to any rect, so they can fill the screen (Esc returns).
     * Bridge apps + the browser stay windowed (they draw at a fixed size). */
    if (app_game(a) >= 0) unoui_fullscreen(&UI, &g_win[a]);
    wm_note_focus(a);                    /* MRU: this is now the front window */
    g_dirty = 1;
}

/* Start button: toggle the app-menu launcher window */
static void toggle_launcher(void)
{
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }
    else { g_menu_scroll = 0; g_menu_hot = 0; menu_refresh();
           /* anchor the menu bottom-left, flush with the Start button and
              sitting directly on top of the taskbar (a real Start menu), rather
              than floating at the old fixed (8,20). */
           g_launch.r.x = 6;
           g_launch.r.y = FB_H - TASKH - g_launch.r.h;
           clamp_to_workarea(&g_launch); unoui_ui_add(&UI, &g_launch);
           UI.focus_wi = 0;                    /* focus the menu canvas for keys */
           g_launch_open = 1; }
    g_dirty = 1;
}

/* Open the launcher AT the pointer (the right-click gesture). Already open
 * somewhere else: move it here rather than closing, which is what a second
 * right-click elsewhere should obviously do. */
static void launcher_at(int x, int y)
{
    g_menu_scroll = 0; g_menu_hot = 0; menu_refresh();
    g_launch.r.x = x;
    g_launch.r.y = y;
    clamp_to_workarea(&g_launch);
    if (!g_launch_open) { unoui_ui_add(&UI, &g_launch); g_launch_open = 1; }
    else                  unoui_bring_to_front(&UI, &g_launch);
    UI.focus_wi = 0;
    g_dirty = 1;
}

/* ---- dragging a desktop icon ------------------------------------------------
 * unoui treats UI_ICON as a button: press and release fire an action. Dragging
 * therefore has to be intercepted BEFORE the toolkit sees the press, and the
 * press replayed at release if the pointer barely moved - otherwise every drag
 * would also launch the app it started on. */
static int g_drag_icon = -1;            /* widget index in g_desk, or -1     */
static int g_drag_app;                  /* which app that icon is            */
static int g_drag_ox, g_drag_oy;        /* grab offset inside the icon       */
static int g_drag_x0, g_drag_y0;        /* where the press landed            */
static int g_drag_moved;

/* Snap (*x,*y) to the nearest FREE grid cell, ignoring icon `self`.
 *
 * Snapping to the plain nearest cell lets two icons land on top of each other,
 * which on a desktop just looks broken - overlapping emblems and interleaved
 * labels. Search outward in rings from the nearest cell instead and take the
 * first unoccupied one, which is what a user dropping an icon expects. */
static void desk_snap_free(int self, int *x, int *y)
{
    int cw = desk_cell_w(), chh = desk_cell_h();
    int c0 = (*x - 16 + cw / 2) / cw, r0 = (*y - 14 + chh / 2) / chh;
    int ring, i;
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    for (ring = 0; ring < 24; ring++) {
        int dc, dr;
        for (dr = -ring; dr <= ring; dr++)
            for (dc = -ring; dc <= ring; dc++) {
                int c, r, px, py, taken = 0;
                if (ring && dr != -ring && dr != ring && dc != -ring && dc != ring)
                    continue;                       /* interior already tried */
                c = c0 + dc; r = r0 + dr;
                if (c < 0 || r < 0) continue;
                px = 16 + c * cw; py = 14 + r * chh;
                if (px + cw > FB_W || py + chh > FB_H - TASKH) continue;
                for (i = 0; i < g_desk.nw; i++) {
                    if (i == self) continue;
                    if (g_desk.w[i].r.x == px && g_desk.w[i].r.y == py) { taken = 1; break; }
                }
                if (!taken) { *x = px; *y = py; return; }
            }
    }
    /* every cell in range is occupied: leave it where it was dropped */
}

/* the desktop icon under (x,y), as a widget index, or -1 */
static int desk_icon_at(int x, int y)
{
    int i;
    for (i = 0; i < g_desk.nw; i++) {
        unoui_rect r = g_desk.w[i].r;      /* BARE window at 0,0: r is screen */
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
    }
    return -1;
}

/* Is (x,y) on bare desktop - i.e. not over any window, popup or the taskbar?
 * Walks every window the UI knows rather than testing the desktop's own rect,
 * because the desktop fills the screen and everything else sits on top. */
static int point_on_desktop(int x, int y)
{
    int i;
    for (i = 0; i < UI.nwin; i++) {
        unoui_window *w = UI.win[i];
        if (!w || w == &g_desk) continue;
        if (x >= w->r.x && x < w->r.x + w->r.w &&
            y >= w->r.y && y < w->r.y + w->r.h) return 0;
    }
    return 1;
}

/* ---- Alt-Tab window switcher (phase D) ------------------------------------
 * A bare TOP overlay: a centred strip of icon+name cells, one per open app
 * (parked ones included), in MRU order. Alt+Tab opens it and steps forward,
 * Alt+Shift+Tab steps back, Esc cancels, and it COMMITS when Alt is released -
 * uno_pc64_mods() dropping UI_MOD_ALT, polled once per frame.
 *
 * F2 and Ctrl-Tab drive the same overlay and the same MRU order. They are the
 * fallback for every transport that cannot report Alt at all (USB HID until
 * the usb lane exposes its boot-report modifier byte; firmware with no Ex
 * KeyState), and they have no release edge, so they commit on a ~0.8 s timer
 * after the last step. That timer also backstops the Alt path, whose firmware
 * source is a per-keystroke LATCH and can read "still held" after the key is
 * up; on native PS/2, where make and break are both tracked, the release edge
 * always wins and the timer never fires.
 *
 * This replaces the old cycle_window(), which rotated blindly through the app
 * table with no preview and no MRU. */
#define SW_CELL_W 96
#define SW_CELL_H 74
#define SW_ICON   32
#define SW_PAD    8
#define SW_COMMIT_TICKS 48                 /* ~0.8 s at the shell's ~60 Hz loop */
#define SW_STALE_TICKS  180                /* ~3 s: a latch that never sees the
                                              Alt release must not strand the
                                              overlay on screen forever. Long
                                              enough that a genuine hold-and-
                                              read is never cut short.         */

static unoui_window g_sw;                  /* the overlay window (bare + top)  */
static int   g_sw_open, g_sw_sel, g_sw_n, g_sw_timer, g_sw_alt;
static short g_sw_list[NAPPS];

static int sw_cols(void) { return g_sw_n < 1 ? 1 : g_sw_n; }

static void sw_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = UI.theme;
    int i, fh = fb_text_h();
    (void)w; (void)ctx;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.face);
    fb_frame_rect(r.x, r.y, r.w, r.h, t->pal.dark);
    for (i = 0; i < g_sw_n; i++) {
        int a  = g_sw_list[i];
        int cx = r.x + SW_PAD + i * SW_CELL_W, cy = r.y + SW_PAD;
        int sel = (i == g_sw_sel);
        const char *nm = app_short(a);
        int tw = fb_text_w(nm), tx = cx + (SW_CELL_W - tw) / 2;
        unoui_rect eb;
        if (sel) {
            fb_fill_rect(cx, cy, SW_CELL_W, SW_CELL_H, t->pal.accent);
            fb_frame_rect(cx, cy, SW_CELL_W, SW_CELL_H, t->pal.dark);
        }
        eb.x = cx + (SW_CELL_W - SW_ICON) / 2; eb.y = cy + 8;
        eb.w = SW_ICON; eb.h = SW_ICON;
        if (app_icon(a) >= 0) pc64_icon_emblem(app_icon(a), eb);
        if (tx < cx) tx = cx;
        fb_text(tx, cy + SW_CELL_H - fh - 6, nm,
                sel ? t->pal.accent_text : t->pal.text, -1);
    }
}
static unoui_canvas g_sw_cv = { sw_draw, 0, 0 };

/* rebuild the candidate list: MRU order first, then any open app the MRU has
 * not seen yet (apps opened before the stack existed, or restored windows).
 *
 * Scoped to the CURRENT desktop, the same guard the taskbar chips use. A
 * switcher that reaches other desktops turns every Alt-Tab into a possible
 * desktop switch, which is exactly what a user separating work across
 * desktops is trying to avoid; the pager and Ctrl-F1..F4 are how you leave. */
static int sw_candidate(int a)
{ return g_open[a] && g_desk_of[a] == g_cur_desk; }

static void sw_fill(void)
{
    int i, a;
    g_sw_n = 0;
    for (i = 0; i < g_nmru && g_sw_n < NAPPS; i++)
        if (sw_candidate(g_mru[i])) g_sw_list[g_sw_n++] = g_mru[i];
    for (a = 0; a < NAPPS && g_sw_n < NAPPS; a++) {
        if (!sw_candidate(a)) continue;
        for (i = 0; i < g_sw_n; i++) if (g_sw_list[i] == a) break;
        if (i == g_sw_n) g_sw_list[g_sw_n++] = (short)a;
    }
}

static void sw_close(void)
{
    if (!g_sw_open) return;
    remove_win(&g_sw);
    g_sw_open = 0;
    g_dirty = 1;
}

static void sw_commit(void)
{
    int a = (g_sw_sel >= 0 && g_sw_sel < g_sw_n) ? g_sw_list[g_sw_sel] : -1;
    sw_close();
    /* re-test the candidate: a desktop switch while the overlay was open can
       strand an entry elsewhere, and committing to it would drag the user back
       to a desktop they just left. */
    if (a >= 0 && sw_candidate(a)) open_app(a);  /* raises, unparks, focuses */
}

/* one Alt+Tab (or F2 / Ctrl-Tab) press: open the overlay, or step it.
 * `back` steps toward the older end; `alt` marks the caller as release-driven. */
static void sw_step(int back, int alt)
{
    if (!g_sw_open) {
        int ww, wh;
        sw_fill();
        if (g_sw_n < 1) return;
        if (g_sw_n == 1) {                       /* nothing to switch TO      */
            if (g_open[g_sw_list[0]]) open_app(g_sw_list[0]);
            return;
        }
        ww = sw_cols() * SW_CELL_W + 2 * SW_PAD;
        wh = SW_CELL_H + 2 * SW_PAD;
        unoui_window_init(&g_sw, "", 0, 0, ww, wh);
        g_sw.flags = UI_WIN_BARE | UI_WIN_TOP;
        unoui_add_canvas(&g_sw, 0, 0, ww, wh, &g_sw_cv);
        g_sw.r.x = (FB_W - ww) / 2;
        g_sw.r.y = (FB_H - TASKH - wh) / 2;
        if (g_sw.r.x < 0) g_sw.r.x = 0;
        if (g_sw.r.y < 0) g_sw.r.y = 0;
        if (!unoui_ui_add(&UI, &g_sw)) return;
        g_sw_open = 1;
        g_sw_alt  = alt;
        g_sw_sel  = back ? g_sw_n - 1 : 1;       /* start on the PREVIOUS app */
    } else {
        g_sw_sel += back ? -1 : 1;
        if (g_sw_sel < 0)        g_sw_sel = g_sw_n - 1;
        if (g_sw_sel >= g_sw_n)  g_sw_sel = 0;
        if (alt) g_sw_alt = 1;
    }
    g_sw_timer = 0;
    g_dirty = 1;
}

/* polled once per frame: the release edge, and the no-release-edge timer */
static void sw_tick(void)
{
    if (!g_sw_open) return;
    if (g_sw_alt) {
        /* held-to-browse: the overlay stays up for as long as Alt reads down,
           so stepping and reading the strip is unhurried. The long backstop is
           purely for a firmware latch that never sees the release. */
        if (!(uno_pc64_mods() & UI_MOD_ALT)) { sw_commit(); return; }
        if (++g_sw_timer >= SW_STALE_TICKS) sw_commit();
        return;
    }
    if (++g_sw_timer >= SW_COMMIT_TICKS) sw_commit();
}

/* ---- window commands (keyboard, phase D; title-bar double click, phase A) --
 * Snap geometry, the restore rect and the don't-stretch-a-fixed-layout rule now
 * live in unoui (unoui_snap_apply over ui->work), so every route into a snap -
 * Alt+arrows, a double-clicked title bar, and phase C's drag-to-edge - goes
 * through this one shell entry point and cannot drift apart. WM_SNAP_* were
 * chosen to match UI_SNAP_*, so phase D's bindings are unchanged. */
enum { WM_SNAP_NONE = UI_SNAP_NONE, WM_SNAP_MAX = UI_SNAP_MAX,
       WM_SNAP_L = UI_SNAP_L, WM_SNAP_R = UI_SNAP_R };

static int wm_focused_app(void)
{
    int i;
    if (UI.focus_win < 0 || UI.focus_win >= UI.nwin) return -1;
    for (i = 0; i < NAPPS; i++)
        if (UI.win[UI.focus_win] == &g_win[i] && g_open[i]) return i;
    return -1;
}

/* The app a whole-window command should act on. Normally the focused one, but
 * focus lands on shell chrome easily enough - the taskbar is an ordinary
 * UI_WIN_TOP window, so any click on it takes focus, and a desktop with no
 * windows leaves focus there - and a keystroke that then silently does nothing
 * reads as a broken binding. Fall back to the topmost app window, the same
 * "topmost non-bare" rule focus_next_mru() ends on. */
static int wm_target_app(void)
{
    int a = wm_focused_app(), k, i;
    if (a >= 0) return a;
    for (k = UI.nwin - 1; k >= 0; k--)
        for (i = 0; i < NAPPS; i++)
            if (UI.win[k] == &g_win[i] && g_open[i]) return i;
    return -1;
}

static void wm_snap(int a, int snap)
{
    if (a < 0 || a >= NAPPS || !g_open[a] || g_parked[a]) return;
    unoui_snap_apply(&UI, &g_win[a], snap);
    session_save();                     /* geometry is part of the session now */
    g_dirty = 1;
}

/* Alt+D: park every window on THIS desktop ("show desktop"), and restore the
 * same set on a repeat press. Scoped to the current desktop for the same reason
 * the chips are: the other three are not the desktop being shown, and parking
 * their windows would silently minimize things the user cannot even see. */
static int g_showdesk;
static void wm_show_desktop(void)
{
    int a, n = 0;
    if (UI.full) { unoui_fullscreen(&UI, 0); UI.focus_wi = 0; }
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }
    if (g_cal_open)    { remove_win(&g_cal);    g_cal_open = 0;    }
    if (g_showdesk) {
        for (a = 0; a < NAPPS; a++)
            if (g_parked[a] && g_desk_of[a] == g_cur_desk) wm_unpark(a);
        g_showdesk = 0; g_dirty = 1; return;
    }
    for (a = 0; a < NAPPS; a++)
        if (g_open[a] && !g_parked[a] && g_desk_of[a] == g_cur_desk)
            { wm_park(a); n++; }
    g_showdesk = n ? 1 : 0;
    g_dirty = 1;
}

/* ---- minimize policy (phase B) --------------------------------------------
 * wm_park() above is the mechanism - the window leaves the scene and the flag
 * goes up. Minimizing through the title-bar button, the taskbar chip or Ctrl-M
 * is that plus the policy those routes need: hand focus on, redraw the chip in
 * its parked style, and remember the parked set for the next boot. Alt+D's
 * bulk park deliberately has none of it, because it is undone wholesale. */

/* focus the most recently focused app still in the scene (phase D's MRU
 * order), falling back to the topmost non-bare window. Without this,
 * remove_win() leaves focus on the taskbar, which is pinned last. */
static void focus_next_mru(void)
{
    int i, k;
    for (i = 0; i < g_nmru; i++) {
        int a = g_mru[i];
        if (a < 0 || a >= NAPPS || !g_open[a] || g_parked[a]) continue;
        for (k = 0; k < UI.nwin; k++) if (UI.win[k] == &g_win[a]) {
            UI.focus_win = k; UI.focus_wi = (a >= NNATIVE) ? 0 : -1;
            wm_note_focus(a);
            return;
        }
    }
    for (k = UI.nwin - 1; k >= 0; k--)
        if (!(UI.win[k]->flags & UI_WIN_BARE)) {
            UI.focus_win = k; UI.focus_wi = -1; return;
        }
}

/* ---- virtual desktops: the machinery (state + metrics are up at NDESK) -----
 * Three primitives. capture() remembers a desktop's z-order while its windows
 * are still in the scene; apply() makes the scene equal the current desktop's
 * unparked set, in that order; switch() is capture-then-apply plus the policy
 * a switch owes the rest of the shell. Everything that moves a window between
 * desktops goes through these, so no route can leave the scene and the
 * assignment table disagreeing. */

/* Remember desktop d's z-order, bottom-to-top. Parked windows are not in
 * UI.win[] so they are not recorded - wm_unpark() raises to the front, which
 * is what a restore should do anyway. */
static void wm_desk_capture(int d)
{
    int i, a, n = 0;
    if (d < 0 || d >= NDESK) return;
    for (i = 0; i < UI.nwin && n < NAPPS - 1; i++)
        for (a = 0; a < NAPPS; a++)
            if (UI.win[i] == &g_win[a]) {
                if (g_open[a] && g_desk_of[a] == d) g_dz[d][n++] = (signed char)(a + 1);
                break;
            }
    g_dz[d][n] = 0;
}

/* Scene := exactly the current desktop's unparked windows, in the order it was
 * left in. A window assigned here but absent from that order - one moved in
 * while we were away, or a session just restored - goes on top. The parked test
 * is the reason a switch cannot unpark anything. */
static void wm_desk_apply(void)
{
    int i, a;
    for (a = 0; a < NAPPS; a++)
        if (g_open[a] && g_desk_of[a] != g_cur_desk) remove_win(&g_win[a]);
    for (i = 0; i < NAPPS && g_dz[g_cur_desk][i]; i++) {
        a = g_dz[g_cur_desk][i] - 1;
        if (a >= 0 && a < NAPPS && g_open[a] && !g_parked[a] &&
            g_desk_of[a] == g_cur_desk && !wm_in_scene(a))
            unoui_ui_add(&UI, &g_win[a]);
    }
    for (a = 0; a < NAPPS; a++)
        if (g_open[a] && !g_parked[a] &&
            g_desk_of[a] == g_cur_desk && !wm_in_scene(a))
            unoui_ui_add(&UI, &g_win[a]);
}

static void wm_desk_switch(int d)
{
    if (d < 0 || d >= NDESK || d == g_cur_desk) return;
    /* A fullscreen game pins its desktop: leave fullscreen first, or a window
     * that is not on the incoming desktop would still be covering it. */
    if (UI.full) { unoui_fullscreen(&UI, 0); UI.focus_wi = 0; }
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }  /* popovers */
    if (g_cal_open)    { remove_win(&g_cal);    g_cal_open = 0;    }  /* close    */
    g_showdesk = 0;                 /* that show-desktop set was the old desktop's */
    wm_desk_capture(g_cur_desk);
    g_cur_desk = d;
    wm_desk_apply();
    /* the ONE MRU stack, not a second notion of recency: focus_next_mru skips
     * anything not in the scene, so walking it now lands on the most recently
     * focused window OF THIS DESKTOP. */
    focus_next_mru();
    rebuild_taskbar();
    session_save();
    g_dirty = 1;
}

/* Move app `a` to desktop `d`. `follow` also switches there, which is what
 * Alt+Ctrl+Fn does; phase F's "Move to desktop N" menu item can pass 0 to send
 * a window away without leaving. A parked window keeps its parked state either
 * way - it just gets parked somewhere else. */
static void wm_desk_move(int a, int d, int follow)
{
    int i, j, n;
    if (a < 0 || a >= NAPPS || !g_open[a] || d < 0 || d >= NDESK) return;
    if (g_desk_of[a] == d) { if (follow) wm_desk_switch(d); return; }
    if (UI.full == &g_win[a]) { unoui_fullscreen(&UI, 0); UI.focus_wi = 0; }
    wm_desk_capture(g_cur_desk);              /* the outgoing order, intact */
    g_desk_of[a] = (signed char)d;
    for (i = 0; i < NDESK; i++) {             /* drop it from every saved order */
        for (j = 0, n = 0; j < NAPPS && g_dz[i][j]; j++)
            if (g_dz[i][j] - 1 != a) g_dz[i][n++] = g_dz[i][j];
        if (n < NAPPS) g_dz[i][n] = 0;        /* n == NAPPS: nothing to drop */
    }
    if (follow) {
        wm_desk_switch(d);
        /* land on top of what is already there, and with focus: a move you
         * followed that dropped the window behind another would read as a bug */
        if (!g_parked[a]) open_app(a);
        return;
    }
    wm_desk_apply();
    focus_next_mru();
    rebuild_taskbar();
    session_save();
    g_dirty = 1;
}

static void minimize_app(int a)
{
    int set[NAPPS], n, i;
    if (a < 0 || a >= NAPPS || !g_open[a] || g_parked[a]) return;
    n = wm_group_set(a, set);       /* a linked set minimizes as one (phase F) */
    for (i = 0; i < n; i++) {
        int b = set[i];
        if (g_parked[b]) continue;
        if (UI.full == &g_win[b]) { unoui_fullscreen(&UI, 0); UI.focus_wi = 0; }
        wm_park(b);
    }
    focus_next_mru();
    rebuild_taskbar();
    session_save();                 /* remember the parked set for next boot */
}

static void restore_app(int a)
{
    int set[NAPPS], n, i;
    if (a < 0 || a >= NAPPS || !g_open[a] || !g_parked[a]) return;
    g_showdesk = 0;                 /* the show-desktop set is broken up now */
    n = wm_group_set(a, set);       /* ...and comes back as one               */
    for (i = 0; i < n; i++)
        if (set[i] != a && g_parked[set[i]]) open_app(set[i]);
    open_app(a);                    /* unparks, raises, focuses, notes MRU,
                                       redraws the chip and saves the session,
                                       so Alt-Tab back to a parked window is
                                       the same restore as the chip click.
                                       LAST, so the grabbed one ends on top. */
}

/* Close app `a` whether or not it is the focused window - the context menu
 * (phase F) closes a window the pointer merely pointed at, and a PARKED app has
 * no z-index to be focused through at all. close_focused() is this plus "which
 * app is in front", so the teardown lives in exactly one place. */
static void close_app(int a)
{
    int g;
    if (a < 0 || a >= NAPPS || !g_open[a]) return;
    g = app_game(a);
    g_open[a] = 0;
    g_parked[a] = 0;                             /* window-manager state dies with it */
    g_group[a] = 0;
    g_win[a].snap = UI_SNAP_NONE;
    if (g >= 0)              pc64_game_close(g);        /* native game teardown */
    else if (a == APP_MUSIC) pc64_music_closed();       /* stop playback      */
    else if (a == EX_STUDIO) { if (g_studio && g_studio->closed) g_studio->closed(); }
    else if (a == EX_PHOTOS) { if (g_photos && g_photos->closed) g_photos->closed(); }
    else if (a == EX_PYAPP)  { if (g_pyapp) { unoscript_app_caps_end();
                                 if (g_pyapp->closed) g_pyapp->closed();
                                 if (g_pyrt) g_pyrt->unload();
                                 g_pyapp = 0; } }
    else if (a == EX_USERAPP) unoapp_user_close();
    else if (app_is_bridge(a)) unoapp_close(a - NNATIVE); /* bridge app        */
    if (UI.full == &g_win[a]) unoui_fullscreen(&UI, 0);  /* fullscreen game    */
    remove_win(&g_win[a]);
    focus_next_mru();
    rebuild_taskbar();
    session_save();                     /* remember the open set for next boot */
    g_dirty = 1;
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
    if (win == &g_pop)    { pop_close(); return; }
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == win) { close_app(i); return; }
    remove_win(win);
    /* Hand focus on, exactly as minimizing does. Without this, remove_win()
       leaves focus_win pointing at whatever slid into the closed window's
       index - in practice the pinned taskbar - so every keyboard window
       command silently no-ops until something is clicked. The closed app is
       already g_open = 0, so the MRU walk skips it. */
    focus_next_mru();
    rebuild_taskbar();
    g_dirty = 1;
}

/* ---- tiling commands (phase F) ---------------------------------------------
 * Commands, not a modal tiling mode: the user asks for a layout once and the
 * windows stay ordinary draggable windows afterwards. Tile routes 1/2/4 through
 * unoui_snap_apply so it inherits the snap geometry and the never-stretch-a-
 * fixed-layout rule for free, and only the n>4 grid needs rects of its own. */

/* the windows a tiling command arranges: open, in the scene, in z order */
static int wm_tile_list(int *out)
{
    int k, i, n = 0;
    for (k = 0; k < UI.nwin; k++)
        for (i = 0; i < NAPPS; i++)
            if (UI.win[k] == &g_win[i] && g_open[i] && !g_parked[i])
                { out[n++] = i; break; }
    return n;
}

/* put window `a` in cell `c`: resizable windows take the cell, fixed-layout
 * ones are centred in it (the same policy unoui_snap_apply applies) */
static void wm_place(int a, unoui_rect c)
{
    unoui_window *w = &g_win[a];
    w->snap = UI_SNAP_NONE;                 /* a grid cell is not a snap state */
    if (w->flags & UI_WIN_RESIZE) {
        if (w->min_w > 0 && c.w < w->min_w) c.w = w->min_w;
        if (w->min_h > 0 && c.h < w->min_h) c.h = w->min_h;
        w->r = c;
        unoui_reflow_window(UI.theme, w);
    } else {
        w->r.x = c.x + (c.w - w->r.w) / 2;
        w->r.y = c.y + (c.h - w->r.h) / 2;
    }
    unoui_clamp_window(&UI, w);
}

static void wm_tile(void)
{
    static const unsigned char kQuad[4] =
        { UI_SNAP_TL, UI_SNAP_TR, UI_SNAP_BL, UI_SNAP_BR };
    int list[NAPPS], n = wm_tile_list(list), i;
    if (n <= 0) return;
    if (n == 1)
        unoui_snap_apply(&UI, &g_win[list[0]], UI_SNAP_MAX);
    else if (n == 2) {
        unoui_snap_apply(&UI, &g_win[list[0]], UI_SNAP_L);
        unoui_snap_apply(&UI, &g_win[list[1]], UI_SNAP_R);
    } else if (n <= 4) {
        for (i = 0; i < n; i++)
            unoui_snap_apply(&UI, &g_win[list[i]], kQuad[i]);
    } else {
        unoui_rect wk = unoui_work_area(&UI);
        int cols = 1, rows;
        while (cols * cols < n) cols++;                    /* ceil(sqrt(n))   */
        rows = (n + cols - 1) / cols;
        for (i = 0; i < n; i++) {
            unoui_rect c;
            c.w = wk.w / cols; c.h = wk.h / rows;
            c.x = wk.x + (i % cols) * c.w;
            c.y = wk.y + (i / cols) * c.h;
            wm_place(list[i], c);
        }
    }
    session_save();
    g_dirty = 1;
}

/* the classic escape hatch for a window lost off the edge: everything stacked
 * from the work-area origin in 24 px steps, snap states given back first so a
 * maximized window returns to a size a cascade can actually show. */
static void wm_cascade(void)
{
    unoui_rect wk = unoui_work_area(&UI);
    int list[NAPPS], n = wm_tile_list(list), i;
    int x = wk.x, y = wk.y, step = 24;
    for (i = 0; i < n; i++) {
        unoui_window *w = &g_win[list[i]];
        unoui_snap_apply(&UI, w, UI_SNAP_NONE);
        if (x + 160 > wk.x + wk.w || y + 100 > wk.y + wk.h) { x = wk.x; y = wk.y; }
        w->r.x = x; w->r.y = y;
        unoui_clamp_window(&UI, w);
        unoui_bring_to_front(&UI, w);       /* list order = back to front      */
        x += step; y += step;
    }
    if (n) { UI.focus_wi = -1; session_save(); }
    g_dirty = 1;
}

static void wm_minimize_all(void)
{
    int list[NAPPS], n = wm_tile_list(list), i;
    if (UI.full) { unoui_fullscreen(&UI, 0); UI.focus_wi = 0; }
    for (i = 0; i < n; i++) wm_park(list[i]);
    if (n) { g_showdesk = 1; focus_next_mru(); rebuild_taskbar(); session_save(); }
    g_dirty = 1;
}

/* Send `a`'s whole link group to desktop `d`. The fourth thing a set does
 * together (after move, raise and minimize/restore): a group split across two
 * desktops would be a group in name only. Nothing follows it - see the menu
 * builder - so wm_desk_move's `follow` is 0 for every member. */
static void wm_group_desk_move(int a, int d, int follow)
{
    int set[NAPPS], n, i;
    n = wm_group_set(a, set);
    for (i = 0; i < n; i++) wm_desk_move(set[i], d, i == n - 1 ? follow : 0);
    rebuild_taskbar();
}

/* join / leave a link group (0 = leave). Group membership is session state
 * like geometry, so it is saved the same way. */
static void wm_group_join(int a, int gid)
{
    if (a < 0 || a >= NAPPS || !g_open[a]) return;
    if (gid < 0 || gid > WM_NGROUP) gid = 0;
    g_group[a] = (unsigned char)gid;
    session_save();
    g_dirty = 1;
}

/* ---- the popover: draw, hit-test, build, dismiss --------------------------- */
static int pop_row_h(void) { int h = fb_text_h() + 6; return h < 20 ? 20 : h; }

static void pop_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    const unoui_theme *t = UI.theme;
    int i, rh = pop_row_h(), fh = fb_text_h();
    (void)w; (void)ctx;
    fb_fill_rect(r.x, r.y, r.w, r.h, t->pal.win_bg);
    fb_frame_rect(r.x, r.y, r.w, r.h, t->pal.dark);
    for (i = 0; i < g_pop_n; i++) {
        int ry = r.y + 1 + i * rh, hot = (i == g_pop_hot);
        if (g_pop_it[i].cmd == POP_SEP) {
            fb_hline(r.x + 5, ry + rh / 2, r.w - 10, t->pal.shadow);
            continue;
        }
        if (hot) fb_fill_rect(r.x + 1, ry, r.w - 2, rh, t->pal.accent);
        if (g_pop_it[i].icon >= 0) {
            unoui_rect eb;
            eb.x = r.x + 4; eb.y = ry + (rh - 14) / 2; eb.w = eb.h = 14;
            pc64_icon_emblem(g_pop_it[i].icon, eb);
        }
        fb_text(r.x + 22, ry + (rh - fh) / 2, g_pop_it[i].label,
                hot ? t->pal.accent_text : t->pal.text, -1);
    }
}

static void pop_activate(int row)
{
    int cmd, arg, a = g_pop_app;
    if (row < 0 || row >= g_pop_n) return;
    cmd = g_pop_it[row].cmd; arg = g_pop_it[row].arg;
    pop_close();                        /* the menu goes first, whatever it did */
    /* Acting on a window through its own context menu also brings it forward:
     * the menu was aimed at it, so it is what the user is working on. Closing
     * the popover left focus on shell chrome (it is a TOP window), so without
     * this a snap would leave nothing focused at all. */
    if (a >= 0 && a < NAPPS && g_open[a] && !g_parked[a] && cmd != POP_CLOSE) {
        unoui_bring_to_front(&UI, &g_win[a]);
        UI.focus_wi = (a >= NNATIVE) ? 0 : -1;
        wm_note_focus(a);
    }
    switch (cmd) {
    case POP_RESTORE:  if (a >= 0) { if (g_parked[a]) restore_app(a);
                                     else             wm_snap(a, WM_SNAP_NONE); } break;
    case POP_MIN:      minimize_app(a); break;
    case POP_MAX:      wm_snap(a, WM_SNAP_MAX); break;
    case POP_SNAPL:    wm_snap(a, WM_SNAP_L); break;
    case POP_SNAPR:    wm_snap(a, WM_SNAP_R); break;
    case POP_GROUP:    wm_group_join(a, arg); break;
    case POP_DESK:     wm_group_desk_move(a, arg, 0); break;
    case POP_CLOSE:    close_app(a); break;
    case POP_ACTIVATE: if (g_parked[arg]) restore_app(arg); else open_app(arg); break;
    case POP_TILE:     wm_tile(); break;
    case POP_CASCADE:  wm_cascade(); break;
    case POP_MINALL:   wm_minimize_all(); break;
    default: break;
    }
}

static int pop_event(struct unoui_widget *w, const void *ev, void *ctx)
{
    const unoui_event *e = (const unoui_event *)ev;
    int rh = pop_row_h(), row;
    (void)w; (void)ctx;
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    row = (e->y - (g_pop.r.y + 1)) / rh;
    if (row >= 0 && row < g_pop_n) pop_activate(row);
    return 1;
}
static unoui_canvas g_pop_cv = { pop_draw, pop_event, 0 };

static void pop_close(void)
{
    if (!g_pop_open) return;
    remove_win(&g_pop);
    g_pop_open = 0; g_pop_hot = -1;
    g_dirty = 1;
}

static void pop_add(const char *label, int cmd, int arg, int icon)
{
    if (g_pop_n >= POP_MAXITEMS) return;
    g_pop_it[g_pop_n].label = label;
    g_pop_it[g_pop_n].cmd   = (short)cmd;
    g_pop_it[g_pop_n].arg   = (short)arg;
    g_pop_it[g_pop_n].icon  = (short)icon;
    g_pop_n++;
}

/* Show the rows collected by pop_add() with their top-left at (x, y), pulled
 * fully into the work area (a menu opened off a taskbar chip would otherwise
 * hang below the screen). */
static void pop_show(int x, int y)
{
    int i, tw = 0, rh = pop_row_h(), ww, wh;
    if (g_pop_n <= 0) return;
    for (i = 0; i < g_pop_n; i++)
        if (g_pop_it[i].label) {
            int t = fb_text_w(g_pop_it[i].label);
            if (t > tw) tw = t;
        }
    ww = tw + 22 + 12;
    wh = 2 + g_pop_n * rh;
    if (g_pop_open) remove_win(&g_pop);
    unoui_window_init(&g_pop, "", x, y, ww, wh);
    g_pop.flags = UI_WIN_BARE | UI_WIN_TOP;
    unoui_add_canvas(&g_pop, 0, 0, ww, wh, &g_pop_cv);
    if (g_pop.r.y + wh > FB_H - TASKH) g_pop.r.y = FB_H - TASKH - wh;
    clamp_to_workarea(&g_pop);
    g_pop_hot = -1;
    if (!unoui_ui_add(&UI, &g_pop)) return;   /* window table full */
    g_pop_open = 1;
    g_dirty = 1;
}

/* right-click on a title bar or a taskbar chip */
static void pop_window_menu(int a, int x, int y)
{
    if (a < 0 || a >= NAPPS || !g_open[a]) return;
    g_pop_app = a; g_pop_n = 0;
    pop_add(g_parked[a] ? "Restore" : "Restore size", POP_RESTORE, 0, -1);
    pop_add("Minimize",   POP_MIN,   0, -1);
    pop_add("Maximize",   POP_MAX,   0, -1);
    pop_add("Snap left",  POP_SNAPL, 0, -1);
    pop_add("Snap right", POP_SNAPR, 0, -1);
    pop_add(0, POP_SEP, 0, -1);
    /* phase E's desktops. SENDING, not following: a menu item aimed at one
     * window should move that window, and leave you looking at the desktop you
     * were on - which is also what wm_desk_move's `follow` argument exists to
     * let this decide (Alt+Ctrl+Fn is the follow form). The desktop the window
     * already lives on is marked, and moving to it is a harmless no-op. */
    pop_add(g_desk_of[a] == 0 ? "To desktop 1 *" : "To desktop 1", POP_DESK, 0, -1);
    pop_add(g_desk_of[a] == 1 ? "To desktop 2 *" : "To desktop 2", POP_DESK, 1, -1);
    pop_add(g_desk_of[a] == 2 ? "To desktop 3 *" : "To desktop 3", POP_DESK, 2, -1);
    pop_add(g_desk_of[a] == 3 ? "To desktop 4 *" : "To desktop 4", POP_DESK, 3, -1);
    pop_add(0, POP_SEP, 0, -1);
    pop_add(g_group[a] == 0 ? "Group: none *" : "Group: none", POP_GROUP, 0, -1);
    pop_add(g_group[a] == 1 ? "Group: A *"    : "Group: A",    POP_GROUP, 1, -1);
    pop_add(g_group[a] == 2 ? "Group: B *"    : "Group: B",    POP_GROUP, 2, -1);
    pop_add(0, POP_SEP, 0, -1);
    pop_add("Close", POP_CLOSE, 0, -1);
    pop_show(x, y);
}

/* right-click on blank taskbar: the layout commands */
static void pop_task_menu(int x, int y)
{
    g_pop_app = -1; g_pop_n = 0;
    pop_add("Tile windows",    POP_TILE,    0, -1);
    pop_add("Cascade windows", POP_CASCADE, 0, -1);
    pop_add("Minimize all",    POP_MINALL,  0, -1);
    pop_show(x, y);
}

/* the ">>" chip: the apps that did not fit on the bar, same click semantics as
 * a chip (parked -> restore, otherwise raise + focus) */
static void pop_overflow(int px, int py)
{
    int list[NAPPS], nopen = tb_open_list(list), nvis = tb_nvis(nopen), k;
    (void)py;
    g_pop_app = -1; g_pop_n = 0;
    for (k = nvis; k < nopen; k++)
        pop_add(app_name(list[k]), POP_ACTIVATE, list[k], app_icon(list[k]));
    if (!g_pop_n) return;
    pop_show(px, FB_H - TASKH - (2 + g_pop_n * pop_row_h()));
}

/* per-frame while a popover is up: highlight the row under the pointer. The
 * shell only repaints when something changed, so the highlight has to ask for
 * the repaint itself. */
static void pop_hover(int mx, int my)
{
    int rh = pop_row_h(), old = g_pop_hot, row;
    if (mx < g_pop.r.x || mx >= g_pop.r.x + g_pop.r.w ||
        my < g_pop.r.y || my >= g_pop.r.y + g_pop.r.h) { row = -1; }
    else {
        row = (my - (g_pop.r.y + 1)) / rh;
        if (row < 0 || row >= g_pop_n || g_pop_it[row].cmd == POP_SEP) row = -1;
    }
    g_pop_hot = row;
    if (g_pop_hot != old) g_dirty = 1;
}

/* ---- session restore (SHELL.CFG v2) ---------------------------------------
 * Persist the "restore" preference, the set of open restorable windows, and
 * each one's geometry, so the next boot reopens them WHERE THEY WERE. Only
 * stable apps are saved (native apps + the Browser); games, transient
 * user/Python slots and loadable modules are not.
 *
 * Line-oriented `key=value`, CRLF, purely additive: an older build ignores the
 * keys it does not know (cfg_line_val matches whole keys), and a newer build
 * reading an older file just finds no geometry and uses the designed position.
 *
 *   restore=1
 *   open=0,2,14
 *   geom0=40,20,520,380      x,y,w,h - the RESTORE rect, not the snapped one
 *   snap0=0                  UI_SNAP_*
 */
static int app_restorable(int a)
{ return (a >= 0 && a < NNATIVE) || a == EX_BROWSER; }

/* Where SHELL.CFG belongs. Volume 0 is the RAM disk, so "the first writable
 * volume" - what this used to do - wrote the session to a filesystem that dies
 * with the power, and no window has ever actually reopened where it was left.
 * Prefer a native FAT partition, then any other persistent volume, and only
 * fall back to RAM when the machine has nothing else. Same order unosecure
 * picks for its own store (pick_vol, unosecure.c). */
static int session_vol(void)
{
    int n = uno_fs_volumes(), v;
    for (v = 1; v < n; v++) if (uno_fs_kind(v) == 1 && uno_fs_writable(v)) return v;
    for (v = 1; v < n; v++) if (uno_fs_writable(v)) return v;
    return (n > 0 && uno_fs_writable(0)) ? 0 : -1;
}

static void session_save(void)
{
    unsigned char buf[1024]; char *p = (char *)buf; int a, first = 1, v;
    if (!g_session_ready) return;       /* don't write during boot restore */
    p = ap_str(p, "restore="); *p++ = g_session_restore ? '1' : '0';
    *p++ = '\r'; *p++ = '\n';
    p = ap_str(p, "cur_desk="); p = ap_int(p, g_cur_desk);
    *p++ = '\r'; *p++ = '\n';
    p = ap_str(p, "open=");
    for (a = 0; a < NAPPS; a++) {
        if (!g_open[a] || !app_restorable(a)) continue;
        if (!first) *p++ = ',';
        p = ap_int(p, a); first = 0;
    }
    *p++ = '\r'; *p++ = '\n';
    for (a = 0; a < NAPPS; a++) {
        /* A snapped window's rect is re-derived from the work area at restore
         * time, so what gets saved is the rect it would go back to - otherwise
         * a font-size change between boots would restore a stale half-screen. */
        unoui_rect r;
        if (!g_open[a] || !app_restorable(a) || !g_built[a]) continue;
        r = (g_win[a].snap != UI_SNAP_NONE && g_win[a].restore_r.w > 0)
            ? g_win[a].restore_r : g_win[a].r;
        p = ap_str(p, "geom"); p = ap_int(p, a); *p++ = '=';
        p = ap_int(p, r.x); *p++ = ',';
        p = ap_int(p, r.y); *p++ = ',';
        p = ap_int(p, r.w); *p++ = ',';
        p = ap_int(p, r.h);
        *p++ = '\r'; *p++ = '\n';
        p = ap_str(p, "snap"); p = ap_int(p, a); *p++ = '=';
        p = ap_int(p, g_win[a].snap);
        *p++ = '\r'; *p++ = '\n';
        /* parked = minimized. Only the parked apps get a line; an absent
         * minN= reads as 0, so an older file behaves as it always did. */
        if (g_parked[a]) {
            p = ap_str(p, "min"); p = ap_int(p, a);
            *p++ = '='; *p++ = '1'; *p++ = '\r'; *p++ = '\n';
        }
        /* virtual desktop. Desktop 0 is the default, so only a window that
         * lives elsewhere gets a line - an absent deskN= reads as 0, which is
         * exactly what an older file (and a build without desktops) means. */
        if (g_desk_of[a]) {
            p = ap_str(p, "desk"); p = ap_int(p, a); *p++ = '=';
            p = ap_int(p, g_desk_of[a]);
            *p++ = '\r'; *p++ = '\n';
        }
        /* link-group membership, same rule: only the grouped apps get a line,
         * so an absent grpN= reads as ungrouped and an older file behaves as
         * it always did. */
        if (g_group[a]) {
            p = ap_str(p, "grp"); p = ap_int(p, a); *p++ = '=';
            p = ap_int(p, g_group[a]); *p++ = '\r'; *p++ = '\n';
        }
    }
    *p = 0;
    v = session_vol();
    if (v >= 0) uno_fs_write(v, "SHELL.CFG", buf, (long)(p - (char *)buf));
}

/* Find `key` at the start of a line in `buf`; return the pointer just past it. */
static const char *cfg_line_val(const char *buf, const char *key)
{
    int kl = 0; const char *p = buf;
    while (key[kl]) kl++;
    while (*p) {
        int i = 0; while (i < kl && p[i] == key[i]) i++;
        if (i == kl) return p + kl;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return 0;
}

/* Read a decimal from *pp, advancing past it; 0 if there is no digit there. */
static int cfg_num(const char **pp)
{
    const char *p = *pp; int v = 0, neg = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    if (*p == ',') p++;
    *pp = p;
    return neg ? -v : v;
}

/* Put app `a`'s window back where the last session left it. Called AFTER
 * open_app has built the window and before the first present, so the restored
 * rect is what gets painted rather than a jump on the second frame. */
static void session_restore_geom(const char *buf, int a)
{
    char key[10]; char *k = key;
    const char *p;
    unoui_rect r;
    k = ap_str(k, "geom"); k = ap_int(k, a); *k++ = '='; *k = 0;
    p = cfg_line_val(buf, key);
    if (!p) return;                          /* an older file: designed position */
    r.x = cfg_num(&p); r.y = cfg_num(&p); r.w = cfg_num(&p); r.h = cfg_num(&p);
    if (r.w < 60 || r.h < 40) return;        /* corrupt: leave the window alone  */
    if (g_win[a].flags & UI_WIN_RESIZE) {
        g_win[a].r = r;
        unoui_reflow_window(UI.theme, &g_win[a]);
    } else {                                 /* fixed layout: position only      */
        g_win[a].r.x = r.x; g_win[a].r.y = r.y;
    }
    /* the DRAG clamp, not clamp_to_workarea: a window the user parked with its
     * right half off the edge must come back parked, not shoved fully inside */
    unoui_clamp_window(&UI, &g_win[a]);
    /* geom applies BEFORE snap, and a snapped window re-derives its rect from
     * the live work area - so a maximized window tracks a font-size change
     * between boots instead of restoring a stale rect. */
    k = key; k = ap_str(k, "snap"); k = ap_int(k, a); *k++ = '='; *k = 0;
    p = cfg_line_val(buf, key);
    if (p) {
        int s = cfg_num(&p);
        if (s > UI_SNAP_NONE && s <= UI_SNAP_BR) unoui_snap_apply(&UI, &g_win[a], s);
    }
}

/* Boot: reopen the saved session, or fall back to opening the Control Panel. */
static void session_load(void)
{
    unsigned char buf[1024]; long got = -1; int v, n = uno_fs_volumes();
    const char *rp, *op;
    for (v = 0; v < n && got < 0; v++)
        got = uno_fs_read(v, "SHELL.CFG", buf, (long)sizeof buf - 1);
    if (got < 0) { open_app(APP_CTRL); g_session_ready = 1; session_save(); return; }
    buf[got] = 0;
    rp = cfg_line_val((char *)buf, "restore=");
    if (rp) g_session_restore = (*rp == '0') ? 0 : 1;
    if (!g_session_restore) { open_app(APP_CTRL); g_session_ready = 1; return; }
    op = cfg_line_val((char *)buf, "open=");
    { int any = 0, val = 0, have = 0;
      for (; op && *op && *op != '\r' && *op != '\n'; op++) {
          if (*op >= '0' && *op <= '9') { val = val * 10 + (*op - '0'); have = 1; }
          else if (*op == ',') {
              if (have && app_restorable(val) && !app_hidden(val)) {
                  open_app(val); session_restore_geom((char *)buf, val); any = 1; }
              val = 0; have = 0;
          }
      }
      if (have && app_restorable(val) && !app_hidden(val)) {
          open_app(val); session_restore_geom((char *)buf, val); any = 1; }
      if (!any) open_app(APP_CTRL);
    }
    /* link groups BEFORE the re-park below, or minimize_app would only park
     * the one app instead of its whole set. */
    { int a; char key[10];
      for (a = 0; a < NAPPS; a++) {
          char *k = key; const char *gp;
          if (!g_open[a]) continue;
          k = ap_str(k, "grp"); k = ap_int(k, a); *k++ = '='; *k = 0;
          gp = cfg_line_val((const char *)buf, key);
          if (gp) { int g = cfg_num(&gp);
                    if (g > 0 && g <= WM_NGROUP) g_group[a] = (unsigned char)g; }
      } }
    /* re-park whatever was parked, after the whole open set is up so the
     * windows land in the z-order they were saved in. */
    { int a; char key[10];
      for (a = 0; a < NAPPS; a++) {
          char *k = key; const char *mp;
          if (!g_open[a]) continue;
          k = ap_str(k, "min"); k = ap_int(k, a); *k++ = '='; *k = 0;
          mp = cfg_line_val((const char *)buf, key);
          if (mp && *mp == '1') minimize_app(a);
      } }
    /* Desktops LAST. Every window above was opened while g_cur_desk was still
     * 0, so open_app assigned them all to desktop 1; the file is what actually
     * decides, and it can only be applied once the whole set is up. Then land
     * on the desktop the session was left on and let the scene follow. */
    { int a; char key[12]; const char *dp;
      for (a = 0; a < NAPPS; a++) {
          char *k = key;
          if (!g_open[a]) continue;
          k = ap_str(k, "desk"); k = ap_int(k, a); *k++ = '='; *k = 0;
          dp = cfg_line_val((const char *)buf, key);
          if (!dp) continue;
          { int d = cfg_num(&dp);
            if (d > 0 && d < NDESK) g_desk_of[a] = (signed char)d; }
      }
      dp = cfg_line_val((const char *)buf, "cur_desk=");
      if (dp) { int d = cfg_num(&dp); if (d > 0 && d < NDESK) g_cur_desk = d; }
      wm_desk_apply();
      focus_next_mru();
    }
    g_session_ready = 1;
}

/* ---- Start menu: a scrollable canvas (apps + Restart + Shut Down) --------- *
 * Entries beyond the visible window scroll: hovering the bottom edge shows a
 * down chevron and scrolls down; once scrolled, an up chevron appears at the
 * top edge and hovering it scrolls back. */
static int mrow_h(void) { int h = fb_text_h() + 8; return h < 22 ? 22 : h; }
#define MROW (mrow_h())
#define MENU_MAXVIS 11

/* the launcher lists only visible apps (Studio needs its module on disk;
 * the user-app slot appears once something has run in it) */
static int menu_apps[NAPPS];
static int menu_napps;
static void menu_refresh(void)
{
    int a;
    menu_napps = 0;
    for (a = 0; a < NAPPS; a++) if (!app_hidden(a)) menu_apps[menu_napps++] = a;
}
/* apps, then the window-layout commands (phase F), then power. The commands
 * live here as well as on the taskbar's context menu because the Start button
 * is the one piece of chrome that is always reachable - a right-click needs a
 * patch of bar that no chip has taken. */
#define MENU_NCMD 3                        /* Tile / Cascade / Minimize all   */
static const char *kMenuCmd[MENU_NCMD] =
{ "Tile windows", "Cascade windows", "Minimize all" };

static int  menu_count(void) { return menu_napps + MENU_NCMD + 2; }
static const char *menu_label(int i)
{
    if (i < menu_napps)             return app_name(menu_apps[i]);
    i -= menu_napps;
    if (i < MENU_NCMD)              return kMenuCmd[i];
    return (i == MENU_NCMD) ? "Restart" : "Shut Down";
}
static int  menu_icon(int i) { return i < menu_napps ? app_icon(menu_apps[i]) : -1; }
static int  menu_vis(void)   { int t = menu_count(); return t < MENU_MAXVIS ? t : MENU_MAXVIS; }

static void menu_activate(int i)
{
    if (i < 0 || i >= menu_count()) return;
    if (i < menu_napps) { open_app(menu_apps[i]); return; }  /* closes the launcher */
    i -= menu_napps;
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; g_dirty = 1; }
    switch (i) {
    case 0:         wm_tile();          break;
    case 1:         wm_cascade();       break;
    case 2:         wm_minimize_all();  break;
    case MENU_NCMD: uno_pc64_restart(); break;
    default:        uno_pc64_shutdown();break;
    }
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
        /* dividers: apps | window commands | power */
        if (idx == menu_napps || idx == menu_napps + MENU_NCMD)
            fb_hline(r.x, ry, r.w, t->pal.shadow);
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
    int i, tw = 0, winw, vis;
    menu_refresh();
    vis = menu_vis();
    for (i = 0; i < menu_count(); i++) { int t = fb_text_w(menu_label(i)); if (t > tw) tw = t; }
    winw = tw + 26 + 14 + 2 * m->frame_w + 2 * m->pad;
    g_menu_scroll = 0; g_menu_hot = -1;
    unoui_window_init(&g_launch, "Programs", 8, 20,
                      winw, m->title_h + m->pad + vis * MROW + m->pad + m->frame_w);
    /* the Start menu is a titled window, but it is not one you minimize or
     * maximize: without this it inherits phase B's boxes and draws controls
     * that correctly do nothing (spec 13.7). Same for the calendar. */
    g_launch.flags |= UI_WIN_NOCTL;
    unoui_add_canvas(&g_launch, 0, 0, winw - 2 * m->frame_w - 2 * m->pad, vis * MROW, &g_menu_cv);
}

/* ---- keep windows on-screen after a resolution change ------------------- */
static void reflow(void)
{
    int i;
    UI.screen_w = FB_W; UI.screen_h = FB_H;
    set_workarea();                                    /* new resolution: new work area */
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
/* The calendar core offers prev/next MONTH only, so correcting the year meant
 * twelve clicks per year - and fourteen years of them on a box whose CMOS
 * battery is dead. This adds a year strip BELOW the calendar rather than
 * extending unoui_calendar_hit: the core owns its own rect and hit regions, so
 * drawing outside them is how to add this without reaching into another
 * subsystem's layout. */
#define CAL_YEARH 22

static int cal_in(unoui_rect q, int x, int y)
{ return x >= q.x && y >= q.y && x < q.x + q.w && y < q.y + q.h; }

static void cal_year_rects(unoui_rect r, unoui_rect *m10, unoui_rect *m1,
                           unoui_rect *p1, unoui_rect *p10)
{
    int y = r.y + r.h - CAL_YEARH, bw = 30, cx = r.x + r.w / 2, h = CAL_YEARH - 4;
    m10->x = cx - 2*bw - 26; m10->y = y; m10->w = bw; m10->h = h;
    m1->x  = cx - bw - 26;   m1->y  = y; m1->w  = bw; m1->h  = h;
    p1->x  = cx + 26;        p1->y  = y; p1->w  = bw; p1->h  = h;
    p10->x = cx + bw + 26;   p10->y = y; p10->w = bw; p10->h = h;
}

static void cal_draw(struct unoui_widget *w, unoui_rect r, void *ctx)
{
    unoui_rect cal = r, b[4];
    const char *lab[4];
    char buf[8];
    int i, v;
    (void)w; (void)ctx;
    cal.h -= CAL_YEARH;
    g_cal_rect = cal;                       /* the core's rect excludes the strip */
    unoui_calendar_draw(UI.theme, cal, g_cal_y, g_cal_mo, g_cal_sel);

    cal_year_rects(r, &b[0], &b[1], &b[2], &b[3]);
    lab[0] = "-10"; lab[1] = "-1"; lab[2] = "+1"; lab[3] = "+10";
    for (i = 0; i < 4; i++) {
        fb_fill_rect(b[i].x, b[i].y, b[i].w, b[i].h, UI.theme->pal.face);
        fb_frame_rect(b[i].x, b[i].y, b[i].w, b[i].h, UI.theme->pal.text_dim);
        fb_text(b[i].x + (b[i].w - fb_text_w(lab[i])) / 2,
                b[i].y + (b[i].h - fb_text_h()) / 2, lab[i], UI.theme->pal.text, -1);
    }
    v = g_cal_y; buf[4] = 0;
    for (i = 3; i >= 0; i--) { buf[i] = (char)('0' + v % 10); v /= 10; }
    fb_text(r.x + r.w/2 - fb_text_w(buf)/2,
            b[1].y + (b[1].h - fb_text_h())/2, buf, UI.theme->pal.text, -1);
}

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
    /* Keyboard is the fast path: Up/Down step a YEAR, Left/Right a month, so a
     * badly wrong clock is a few keypresses instead of a click marathon. */
    if (e->kind == UI_EV_KEY) {
        if (e->key == UI_KEY_UP)    { g_cal_y++; g_dirty = 1; return 1; }
        if (e->key == UI_KEY_DOWN)  { if (g_cal_y > 1970) g_cal_y--; g_dirty = 1; return 1; }
        if (e->key == UI_KEY_RIGHT) { if (++g_cal_mo > 12) { g_cal_mo = 1; g_cal_y++; } g_dirty = 1; return 1; }
        if (e->key == UI_KEY_LEFT)  { if (--g_cal_mo < 1) { g_cal_mo = 12; g_cal_y--; } g_dirty = 1; return 1; }
        if (e->key == UI_KEY_ENTER) { cal_apply_and_close(); return 1; }
        return 0;
    }
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    {   unoui_rect full = g_cal_rect, b[4];
        full.h += CAL_YEARH;                  /* g_cal_rect excludes the strip */
        cal_year_rects(full, &b[0], &b[1], &b[2], &b[3]);
        if (cal_in(b[0], e->x, e->y)) { g_cal_y = g_cal_y > 1980 ? g_cal_y - 10 : 1970; g_dirty = 1; return 1; }
        if (cal_in(b[1], e->x, e->y)) { if (g_cal_y > 1970) g_cal_y--; g_dirty = 1; return 1; }
        if (cal_in(b[2], e->x, e->y)) { g_cal_y++; g_dirty = 1; return 1; }
        if (cal_in(b[3], e->x, e->y)) { g_cal_y += 10; g_dirty = 1; return 1; } }
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
    int cw = 210, chh = 176 + CAL_YEARH, yy = 2026, mo = 1, dd = 1, hh = 0, mi = 0;
    if (g_cal_open) { remove_win(&g_cal); g_cal_open = 0; }
    uno_pc64_time(&yy, &mo, &dd, &hh, &mi, 0);
    g_cal_y = yy; g_cal_mo = mo; g_cal_sel = dd;
    unoui_window_init(&g_cal, "Pick a date", 180, 56,
                      cw + 2*m->frame_w + 2*m->pad, chh + m->title_h + 2*m->pad + m->frame_w);
    g_cal.flags |= UI_WIN_NOCTL;
    unoui_add_canvas(&g_cal, 0, 0, cw, chh, &g_cal_cv);
    clamp_to_workarea(&g_cal);
    unoui_ui_add(&UI, &g_cal);
    g_cal_open = 1; g_dirty = 1;
}

/* ---- shell services for the standalone apps (Write / Files) ------------- */
void pc64_shell_add_window(unoui_window *w)
{ clamp_to_workarea(w); unoui_ui_add(&UI, w); g_dirty = 1; }
void pc64_shell_remove_window(unoui_window *w) { remove_win(w); g_dirty = 1; }
void pc64_shell_del_window(unoui_window *w) { pc64_shell_remove_window(w); }
                                     /* alias: import names cap at 23 chars */
void pc64_shell_focus_window(unoui_window *w)
{ int i; for (i = 0; i < UI.nwin; i++) if (UI.win[i] == w) { UI.focus_win = i; break; } }
void pc64_shell_dirty(void) { g_dirty = 1; }
int  pc64_shell_workarea_w(void) { return FB_W; }
int  pc64_shell_workarea_h(void) { return FB_H - TASKH; }

#ifdef UNO_DEBUG
/* unoautomate PROBE accessors (unoauto_probe.c): enumerate the open windows.
 * Titles are string literals, so the returned pointer is stable. */
int pc64_shell_win_count(void) { return UI.nwin; }
const char *pc64_shell_win_title(int i)
{ return (i >= 0 && i < UI.nwin) ? UI.win[i]->title : 0; }
int pc64_shell_win_focused(int i) { return i == UI.focus_win; }
#endif

/* ---- production accessors for unoscript's ui.* automation surface ----------
 * Always built (unlike the DRIVE/PROBE accessors above); unoscript gates them at
 * its own layer via unosecure capabilities.  Screen "read" is the window-tree
 * accessibility text (titles + which is focused), not a pixel grab, so a script
 * can see and target windows.  The clipboard is a small shell-owned text buffer
 * (there is no system clipboard otherwise - each app keeps its own today). */
int pc64_shell_screen_text(char *out, int cap)
{
    int i, k = 0;
    if (!out || cap <= 0) return 0;
    for (i = 0; i < UI.nwin && k < cap - 1; i++) {
        const char *t = UI.win[i]->title;
        const char *mark = (i == UI.focus_win) ? "* " : "  ";
        int j;
        for (j = 0; mark[j] && k < cap - 1; j++) out[k++] = mark[j];
        for (j = 0; t && t[j] && k < cap - 1; j++) out[k++] = t[j];
        if (k < cap - 1) out[k++] = '\n';
    }
    out[k] = 0;
    return k;
}

static char g_shell_clip[512];
static int  g_shell_clip_n;
int pc64_shell_clip_set(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n] && n < (int)sizeof g_shell_clip - 1) { g_shell_clip[n] = s[n]; n++; }
    g_shell_clip[n] = 0; g_shell_clip_n = n;
    return 1;
}
int pc64_shell_clip_get(char *out, int cap)
{
    int n = 0;
    if (!out || cap <= 0) return 0;
    while (g_shell_clip[n] && n < cap - 1) { out[n] = g_shell_clip[n]; n++; }
    out[n] = 0;
    return n;
}

/* ---- app control (production; unoscript app.ctrl / app.msg) ----------------
 * Launch/close go through the exact code paths the launcher click and the
 * title-bar close box use.  EX_PYAPP/EX_USERAPP are refused to LAUNCH - doing
 * so would displace the automation script that is running.  (Formerly the
 * UNO_DEBUG-only DRIVE accessors; now production, gated at the unoscript layer.) */
int pc64_shell_app_count(void) { return NAPPS; }
int pc64_shell_launch(int a)
{
    if (a < 0 || a >= NAPPS || a == EX_PYAPP || a == EX_USERAPP) return 0;
    open_app(a);
    return 1;
}
void pc64_shell_close_top(void) { close_focused(); }

/* bounded string append: writes s at dst[at..], NUL-terminates, returns new len */
static int sput(char *dst, int cap, int at, const char *s)
{ while (*s && at < cap - 1) dst[at++] = *s++; if (at < cap) dst[at] = 0; return at; }

/* structured app control (unoscript app.msg, tier 1).  Minimal v1 verb set by
 * app index: `info` (name/open/focused), `focus`, `close`.  Returns the reply
 * length; `reply` always gets a short status.  Per-app custom verbs (an app-side
 * message handler) are a later addition. */
int pc64_shell_app_message(int idx, const char *msg, char *reply, int cap)
{
    int wi;
    if (!reply || cap <= 0) return 0;
    if (idx < 0 || idx >= NAPPS || !msg) return sput(reply, cap, 0, "bad-idx");
    if (!strcmp(msg, "info")) {
        int at = sput(reply, cap, 0, "name=");
        at = sput(reply, cap, at, app_name(idx));
        at = sput(reply, cap, at, g_open[idx] ? " open=1" : " open=0");
        at = sput(reply, cap, at, (focused_app() == idx) ? " focused=1" : " focused=0");
        return at;
    }
    if (!strcmp(msg, "focus")) {
        if (!g_open[idx]) return sput(reply, cap, 0, "not-open");
        restore_app(idx);                    /* parked: bring it back first  */
        raise_win(&g_win[idx]);
        for (wi = 0; wi < UI.nwin; wi++) if (UI.win[wi] == &g_win[idx]) { UI.focus_win = wi; break; }
        g_dirty = 1;
        return sput(reply, cap, 0, "focused");
    }
    if (!strcmp(msg, "close")) {
        if (!g_open[idx]) return sput(reply, cap, 0, "not-open");
        restore_app(idx);        /* a parked window has no z-index to focus  */
        for (wi = 0; wi < UI.nwin; wi++) if (UI.win[wi] == &g_win[idx]) { UI.focus_win = wi; break; }
        close_focused();
        return sput(reply, cap, 0, "closed");
    }
    return sput(reply, cap, 0, "unknown-verb");
}

/* ---- running-process enumeration (unoscript proc.list / proc.inspect) ------
 * pc64 is a single-address-space cooperative shell: there is no preemptive
 * scheduler, so a "process" is an OPEN app slot - the same run-set the F11 /
 * unoauto PROBE surface reports.  These production primitives let unoscript
 * compose its usc_proc_ent rows (pid = app slot, name, focused) without
 * reaching into shell internals.  `idx` is 0..pc64_shell_app_count()-1 and is
 * stable for a boot (slot indices are fixed; EX_PYAPP/EX_USERAPP included, so a
 * running Python automation app enumerates as a process too). */
int pc64_shell_app_open(int idx)
{ return (idx >= 0 && idx < NAPPS) ? g_open[idx] : 0; }
const char *pc64_shell_app_name(int idx)
{ return (idx >= 0 && idx < NAPPS) ? app_name(idx) : 0; }
int pc64_shell_app_is_focused(int idx)
{ return (idx >= 0 && idx < NAPPS) ? (focused_app() == idx) : 0; }

/* the bundled monospace face's font slot (Studio's code editor), -1 = none */
#ifdef UNO_DEBUG
/* Open the SSH client's window on demand. The visual half of the ssh_app gate
 * needs the window UP at a known moment; driving the Start menu instead means
 * depending on where EX_SSH lands in the launcher, and the first attempt at
 * that opened the Control Panel. */
void pc64_dbg_open_ssh(void) { open_app(EX_SSH); }
#endif

int pc64_shell_font_mono(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        const char *n = uno_font_name(i);
        if (!n) break;
        if (n[0] == 'M' && n[1] == 'o' && n[2] == 'n' && n[3] == 'o' && !n[4])
            return i;
    }
    return -1;
}

/* Studio's Run: host the module it just wrote at vol:path in the user-app
 * window (replacing whatever ran there before). 0 = running, -1 = load
 * failure (bad image / unresolved import / slot full). */
/* run the Python container at vol:path: ensure PYRT is loaded, hand it the
 * source, host the returned UnoUuiApp in the EX_PYAPP window. */
static int pc64_shell_run_python(int vol, const char *path)
{
    const unsigned char *src; int len;
    pyrt_ensure();
    if (!g_pyrt) return -2;                       /* no PYRT.UNO on this system */
    if (uno_mod_load_pyapp(vol, path, &src, &len) < 0) return -1;
    if (g_open[EX_PYAPP]) {                        /* replace any running app */
        remove_win(&g_win[EX_PYAPP]);
        unoscript_app_caps_end();                 /* drop the replaced app's caps */
        if (g_pyapp && g_pyapp->closed) g_pyapp->closed();
        g_pyrt->unload();
        g_open[EX_PYAPP] = 0;
    }
    pdbg("pyrt: compiling app\n");
    g_pyapp = g_pyrt->load(src, len, path);
    if (!g_pyapp) { pdbg("pyrt: load FAILED\n"); pdbg(g_pyrt->last_error()); return -3; }
    pdbg("pyrt: app loaded, opening window\n");
    g_built[EX_PYAPP] = 0;
    open_app(EX_PYAPP);
    unoscript_app_caps_begin(vol, path);          /* grant its manifest-declared caps */
    return 0;
}

#ifdef UNO_DEBUG
/* unoautomate: run a Python source/container directly (peek_flags cannot see
 * a raw .py - no magic - so the automation runner comes in through here). */
int pc64_shell_run_py(int vol, const char *path)
{ return pc64_shell_run_python(vol, path); }

/* unoautomate remote `py` verb: exec a Python source string via PYRT and
 * capture its stdout into out.  0 = ok, <0 = no PYRT / error (out carries the
 * message).  Shares the VM with any running Python app. */
int pc64_shell_py_exec(const char *src, char *out, int cap)
{
    unsigned long n = 0;
    while (src[n]) n++;
    pyrt_ensure();
    if (!g_pyrt || !g_pyrt->run_src) {
        if (cap > 0) { const char *m = "no PYRT.UNO"; int i = 0;
                       while (m[i] && i < cap - 1) { out[i] = m[i]; i++; } out[i] = 0; }
        return -2;
    }
    return g_pyrt->run_src(src, (int)n, out, cap);
}
#endif

int pc64_shell_run_user(int vol, const char *path)
{
    if (uno_mod_peek_flags(vol, path) & UNO_MODF_PYAPP)   /* a Python app */
        return pc64_shell_run_python(vol, path);
    if (unoapp_user_run(vol, path) < 0) return -1;
    if (g_open[EX_USERAPP]) {            /* rebuild for the new size/title */
        remove_win(&g_win[EX_USERAPP]);
        g_open[EX_USERAPP] = 0;
    }
    g_built[EX_USERAPP] = 0;
    open_app(EX_USERAPP);
    return 0;
}

/* Studio calls this to show a Python compile error in its output pane */
const char *pc64_shell_py_error(void) { return g_pyrt ? g_pyrt->last_error() : ""; }

/* app-side action hooks (in pc64_write.c / pc64_files.c) */
int pc64_write_action(const unoui_action *a);
int pc64_files_action(const unoui_action *a);
int pc64_music_action(const unoui_action *a);
void pc64_music_tick(void);
void pc64_music_closed(void);
int pc64_write_key(int uni, int ctrl);
void pc64_write_frame(void);

/* Rebuild everything laid out in font-space (native app windows, taskbar,
 * desktop icon grid, Start menu) after a font or UI-scale change. Open native
 * windows are torn down and reopened at the new metrics; module/bridge apps
 * keep their canvases and just get clamped. */
static void rebuild_shell(void)
{
    int a, wasopen[NAPPS], wasdesk[NAPPS];
    sw_close();                        /* the overlay is sized in font-space */
    g_showdesk = 0;
    if (g_launch_open) { remove_win(&g_launch); g_launch_open = 0; }
    if (g_cal_open)    { remove_win(&g_cal);    g_cal_open = 0; }
    for (a = 0; a < NAPPS; a++) {
        wasopen[a] = g_open[a];
        wasdesk[a] = g_desk_of[a];     /* reopening below would reassign them */
        /* everything below re-adds windows to the scene, so no window can stay
           parked across a rebuild; a snapped rect is re-derived by the reopen */
        if (g_parked[a]) { g_parked[a] = 0; if (a >= NNATIVE) unoui_ui_add(&UI, &g_win[a]); }
        g_win[a].snap = UI_SNAP_NONE;
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
    /* every window is back in the scene now, on whatever desktop the reopen
     * assigned; put the split back the way the user had it. */
    for (a = 0; a < NAPPS; a++) g_desk_of[a] = (signed char)wasdesk[a];
    wm_desk_apply();
    focus_next_mru();
    g_dirty = 1;
}

/* ---- actions ----------------------------------------------------------- */
static void on_action(const unoui_action *a)
{
    if (!a->changed) return;
    /* unoautomate tap: scripts observe every widget action (no-op in prod) */
    { UnoAutoUiEv ev; ev.id = a->id; ev.kind = a->kind; ev.value = a->value;
      unoauto_hook_fire("uui.action", &ev); }
    g_dirty = 1;
    if (a->kind == UI_ACT_CLOSE) { close_focused(); return; }   /* title-bar close box */
    /* Title-bar minimize box. The input layer focused that window before
     * emitting, so the focused app IS the target; a non-app window (the Start
     * menu, the calendar) reports -1 and the action is dropped. */
    if (a->kind == UI_ACT_MIN) { minimize_app(wm_focused_app()); return; }
    if (a->kind == UI_ACT_MAX) {   /* maximize box, or a double-clicked bar   */
        /* Same path as Alt+Up, so the three can never disagree about what
         * "restore" means. A window with no UI_WIN_RESIZE is left alone
         * rather than being centred by unoui_snap_apply's move-only rule:
         * that is what makes its maxbox a genuinely disabled control, and a
         * fixed-layout app jumping across the desktop on a double-click is
         * worse than nothing happening. */
        int a2 = wm_focused_app();
        if (a2 >= 0 && (g_win[a2].flags & UI_WIN_RESIZE))
            wm_snap(a2, g_win[a2].snap == UI_SNAP_MAX ? UI_SNAP_NONE : UI_SNAP_MAX);
        return;
    }
    if (a->id >= ID_TASK0   && a->id < ID_TASK0   + NAPPS) { open_app(a->id - ID_TASK0);   return; }
    if (a->id >= ID_LAUNCH0 && a->id < ID_LAUNCH0 + NAPPS) { open_app(a->id - ID_LAUNCH0); return; }
    if (pc64_write_action(a)) return;           /* the Editor's menus/toolbar */
    if (pc64_files_action(a)) return;           /* the file manager's toolbar */
    if (pc64_music_action(a)) return;           /* the media player           */
    if (pc64_clock_action(a)) return;           /* the world clock            */
    if (g_studio && g_open[EX_STUDIO] && g_studio->action &&
        g_studio->action(a)) return;            /* the Studio module          */
    if (g_photos && g_open[EX_PHOTOS] && g_photos->action &&
        g_photos->action(a)) return;            /* the Photos module          */
    if (g_uoword && g_open[EX_UOWORD] && g_uoword->action &&
        g_uoword->action(a)) return;            /* the UnoWord module         */
    if (g_uocalc && g_open[EX_UOCALC] && g_uocalc->action &&
        g_uocalc->action(a)) return;            /* the UnoCalc module         */
    if (g_pyapp && g_open[EX_PYAPP] && g_pyapp->action &&
        g_pyapp->action(a)) return;             /* the running Python app      */
    switch (a->id) {
    case ID_ILIST:    inst_select(a->value); break;
    case ID_IDEF:     g_inst_default = a->value; break;
    case ID_IRESCAN:  inst_rescan(); break;
    case ID_IGO:      inst_go(); break;
    /* editing the confirm box after arming revokes the arm: the second click
       must follow the typing, not the other way round */
    case ID_ICONF:    g_inst_armed = 0; break;
    case ID_START:    toggle_launcher(); break;
    case ID_SHUTDOWN: uno_pc64_shutdown(); break;
    case ID_RESTART:  uno_pc64_restart();  break;
    case ID_THEME: if (a->value >= 0 && a->value < NTHEMES) unoui_ui_theme(&UI, kThemes[a->value].theme); break;
    case ID_DARK:  unoui_ui_theme(&UI, a->value ? &theme_aurora_dark : &theme_aurora_light); break;
    case ID_ALITE: unoui_aurora_lite = a->value ? 1 : 0; g_dirty = 1; break;   /* Aurora full<->lite (no live composite) */
    case ID_LIDSLP: g_lidsleep = a->value ? 1 : 0; break;                      /* lid-close enters sleep */
    /* desktop arrangement: rebuild the icon layer in place */
    case ID_DFLOW: g_desk_flow = a->value ? 1 : 0; build_desktop(); g_dirty = 1; break;
    case ID_DSORT: g_desk_sort = a->value ? 1 : 0; build_desktop(); g_dirty = 1; break;
    case ID_WALL: if (a->value >= 0 && a->value < NWALL) {   /* desktop wallpaper */
                      g_wallpaper = a->value; unoui_bg_invalidate(); g_dirty = 1; } break;
    case ID_CLOCKFMT: g_clock_12h = a->value ? 1 : 0; fmt_clock(0); g_dirty = 1; break;
    case ID_BATTMODE: if (a->value >= 0 && a->value <= BATT_BOTH) {
                          g_batt_mode = a->value; fmt_batt(); g_dirty = 1; } break;
    case ID_CPTAB: if (a->value >= 0 && a->value < CT_N) {   /* Control Panel tab */
                       g_ctrl_tab = a->value; rebuild_ctrl_window(); } break;
    case ID_NETREFRESH: rebuild_ctrl_window(); break;        /* re-read live net status */
    case ID_NETRENEW: {                                     /* ask for a lease again */
        int i;
        cp_wifi_phase("Asking the network for an address (DHCP)", 0);
        net_dhcp_start();
        for (i = 0; i < 4000 && !net_dhcp_done(); i++) {
            net_poll(); uno_pc64_delay_ms(5);
            if (i && (i % 100) == 0)
                cp_wifi_phase("Asking the network for an address (DHCP)", i / 200);
        }
        cp_wifi_note(net_dhcp_done() ? "Connected."
                                     : "Still no address - the network did not answer.");
        rebuild_ctrl_window();
        break; }
    case ID_WIFILIST:  if (a->value >= 0 && a->value < g_cp_ap_n) {
                           g_cp_ap_sel = a->value;
                           cp_wifi_target_label();  /* in place: no rebuild, so
                                                       the list keeps its view */
                       } break;
    case ID_WIFISCAN:  cp_wifi_scan(); break;
    case ID_WIFIJOIN:  cp_wifi_join(); break;
    case ID_WIFIPSK:   break;                               /* typing; nothing to do */
    case ID_SESSION: g_session_restore = a->value ? 1 : 0; session_save(); break;
    case ID_PSPEED: uno_pc64_pointer_speed(a->value); break;
    case ID_DSNAP:  g_desk_snap = a->value ? 1 : 0; break;
    case ID_DLOCK:  g_desk_lock = a->value ? 1 : 0; break;
    case ID_DARRANGE: {          /* forget every hand placement and reflow */
        int i; for (i = 0; i < 32; i++) g_icon_pos[i].placed = 0;
        build_desktop(); g_dirty = 1; break; }
    case ID_VOL:   uno_snd_volume(a->value); break;    /* PCM gain; PC speaker has none */
    case ID_RES:   if (a->value >= 0 && a->value < g_res_n) { uno_pc64_res_set(a->value); reflow(); } break;
    case ID_ABOUT: open_app(APP_SYS); break;
    case ID_LIC:   pc64_browser_open_path("DOCS\\LICENSES.MD");
                   open_app(EX_BROWSER); break;
    case ID_ACCT:  pc64_accounts_open(); g_dirty = 1; break;   /* Accounts manager */
    case ID_SETDT: {                    /* time spinners; the date stays as-is */
        int yy = 2026, mo = 1, dd = 1;
        uno_pc64_time(&yy, &mo, &dd, 0, 0, 0);
        if (g_sp_h && g_sp_mi)
            uno_pc64_set_time(yy, mo, dd, g_sp_h->value, g_sp_mi->value, 0);
        fmt_clock(0);
        break;
    }
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

/* 1 while an editable text widget owns the keyboard. Shell accelerators the
 * user could plausibly be typing stand down when this is set - the same rule
 * the Install window applies around its confirm box, generalised. */
static int typing_in_field(void)
{
    const unoui_window *w;
    if (UI.focus_win < 0 || UI.focus_win >= UI.nwin || UI.focus_wi < 0) return 0;
    w = UI.win[UI.focus_win];
    if (UI.focus_wi >= w->nw) return 0;
    return w->w[UI.focus_wi].edit != 0;
}

static int pump_input(void)
{
    static int lastx = -1, lasty = -1, lastb = 0;
    int mx, my, mb, scan, uni, ctrl, mods, any = 0, real = 0;
    unoui_event ev;

    uno_pc64_mouse(&mx, &my, &mb);
    if (mx != lastx || my != lasty) {
        lastx = mx; lasty = my; any = 1;
        if (g_drag_icon >= 0) {                 /* carrying an icon */
            g_desk.w[g_drag_icon].r.x = mx - g_drag_ox;
            g_desk.w[g_drag_icon].r.y = my - g_drag_oy;
            if (mx - g_drag_x0 > 3 || g_drag_x0 - mx > 3 ||
                my - g_drag_y0 > 3 || g_drag_y0 - my > 3) g_drag_moved = 1;
            g_dirty = 1; real = 1;
        } else {
            /* Bare cursor motion only needs the cursor recomposited (present does
               that), NOT the whole alpha-blend scene repaint - unless the move
               crosses a widget/menu boundary and changes the hover highlight, or
               the Start menu is open (its hover is recomputed each frame). Snapshot
               the hover state across the feed and only force a full repaint when it
               actually changed. This is the "redraw is slow on mouse move" fix. */
            int ohw = UI.hot_win, ohi = UI.hot_wi, oph = UI.popup_hot;
            memset(&ev, 0, sizeof ev); ev.kind = UI_EV_MOUSE_MOVE; ev.x = mx; ev.y = my;
            feed(&ev);
            wm_group_drag();          /* a linked set follows the dragged one */
            if (g_pop_open) pop_hover(mx, my);
            /* Also force a full repaint while a drag/capture is live: window
               drag (the rubber-band outline), live resize, and text drag-select
               all update visible state on a bare move but go through a cap_mode
               that returns NO_ACT and never touches the hover fields, so the
               hover test alone would freeze them until button-release. */
            if (g_launch_open || g_pop_open || UI.hot_win != ohw ||
                UI.hot_wi != ohi || UI.popup_hot != oph ||
                UI.drag_active || UI.cap_mode != UI_CAP_NONE)
                real = 1;
        }
    }
    /* the wheel: one UI_EV_WHEEL at the pointer, which unoui routes to whatever
       is hovered (a scrolling list, a scrollbar, or a canvas app - the Browser,
       Editor, Files and Photos all take it) */
    { int wz = uno_pc64_wheel();
      if (wz) {
          memset(&ev, 0, sizeof ev);
          ev.kind = UI_EV_WHEEL; ev.x = mx; ev.y = my; ev.wheel = wz;
          feed(&ev); any = 1; real = 1;
      } }

    /* Only the LEFT button drives the widget layer. unoui has one notion of
       "the mouse button", so feeding it a right-click would activate whatever
       is under the pointer - the opposite of what a context gesture should do. */
    { int left = mb & 1, right = (mb >> 1) & 1;
      if (left && !(lastb & 1)) {                     /* press */
          int hit;
          /* a press anywhere but ON the popover dismisses it, and is swallowed
             - the same "one click closes the menu" every context menu has. */
          if (g_pop_open &&
              (mx < g_pop.r.x || mx >= g_pop.r.x + g_pop.r.w ||
               my < g_pop.r.y || my >= g_pop.r.y + g_pop.r.h)) {
              pop_close(); lastb = mb; return 1;    /* 1 = "repaint the scene" */
          }
          hit = (!g_desk_lock && point_on_desktop(mx, my))
                ? desk_icon_at(mx, my) : -1;
          any = 1; real = 1;
          if (hit >= 0) {                             /* begin a drag */
              g_drag_icon = hit;
              g_drag_app  = g_desk.w[hit].id - ID_LAUNCH0;
              g_drag_ox   = mx - g_desk.w[hit].r.x;
              g_drag_oy   = my - g_desk.w[hit].r.y;
              g_drag_x0   = mx; g_drag_y0 = my;
              g_drag_moved = 0;
          } else {
              memset(&ev, 0, sizeof ev);
              ev.kind = UI_EV_MOUSE_DOWN; ev.x = mx; ev.y = my; feed(&ev);
              wm_group_drag();      /* a group grab: baseline BEFORE it moves */
          }
      } else if (!left && (lastb & 1)) {              /* release */
          any = 1; real = 1;
          if (g_drag_icon >= 0) {
              if (!g_drag_moved) {
                  /* it was a click after all - replay it so the app opens */
                  memset(&ev, 0, sizeof ev);
                  ev.kind = UI_EV_MOUSE_DOWN; ev.x = g_drag_x0; ev.y = g_drag_y0;
                  feed(&ev);
                  memset(&ev, 0, sizeof ev);
                  ev.kind = UI_EV_MOUSE_UP;   ev.x = mx; ev.y = my; feed(&ev);
              } else {
                  int x = g_desk.w[g_drag_icon].r.x, y = g_desk.w[g_drag_icon].r.y;
                  if (g_desk_snap) desk_snap_free(g_drag_icon, &x, &y);
                  g_desk.w[g_drag_icon].r.x = x;
                  g_desk.w[g_drag_icon].r.y = y;
                  if (g_drag_app >= 0 && g_drag_app < 32) {
                      g_icon_pos[g_drag_app].x = (short)x;
                      g_icon_pos[g_drag_app].y = (short)y;
                      g_icon_pos[g_drag_app].placed = 1;
                  }
              }
              g_drag_icon = -1;
              g_dirty = 1;
          } else {
              memset(&ev, 0, sizeof ev);
              ev.kind = UI_EV_MOUSE_UP; ev.x = mx; ev.y = my; feed(&ev);
          }
      }
      /* Right-press: the context gesture. The title-bar and taskbar tests come
         BEFORE the desktop one - a right-click on a window used to fall through
         to "not the desktop, do nothing". */
      if (right && !((lastb >> 1) & 1)) {
          any = 1; real = 1;
          if (g_pop_open) pop_close();          /* a second right-click re-aims */
          if (my >= FB_H - TASKH) {             /* the taskbar */
              int a = tb_chip_app_at(mx - g_task.r.x);
              if (a >= 0) pop_window_menu(a, mx, my);
              else        pop_task_menu(mx, my);   /* blank bar / tray / Start */
          } else {
              int a = win_titlebar_app_at(mx, my);
              if (a >= 0)                        pop_window_menu(a, mx, my);
              else if (point_on_desktop(mx, my)) launcher_at(mx, my);
              else if (g_launch_open)            toggle_launcher();
              else                               { any = 0; real = 0; }
          }
      }
      lastb = mb;
    }
    while (uno_pc64_next_key2(&scan, &uni, &mods)) {
        int vk = 0;
        ctrl = (mods & UI_MOD_CTRL) != 0;
        any = 1; real = 1;
#ifdef UNO_DEBUG
        /* F12 = operator escape hatch: stop the stress driver and hand back a
         * usable desktop.  FIRST in the loop and it also drops fullscreen, so
         * it works even while a fullscreen app (Runner3D) has focus - that is
         * precisely when the operator is otherwise trapped and can't reach
         * Start > Shut Down.  F10 is taken by the platform (GOP mode cycle). */
        if (scan == 0x16) {
            pc64_stress_stop();
            if (UI.full) { unoui_fullscreen(&UI, 0); UI.focus_wi = 0; }
            g_dirty = 1;
            continue;
        }
#endif
        if (g_pop_open && scan == 0x17) { pop_close(); continue; }   /* Esc */
        /* the switcher owns the keyboard while it is up: Esc cancels, the
           arrows step it, and every other key falls through to commit-on-tick */
        if (g_sw_open) {
            if (scan == 0x17) { sw_close(); continue; }              /* Esc     */
            if (scan == 0x03 && !(mods & UI_MOD_ALT)) { sw_step(0, 0); continue; }
            if (scan == 0x04 && !(mods & UI_MOD_ALT)) { sw_step(1, 0); continue; }
            if (uni == 0x0D || uni == 0x0A) { sw_commit(); continue; }
        }
        if (UI.full && scan == 0x17) {          /* Esc leaves a fullscreen game */
            unoui_fullscreen(&UI, 0); UI.focus_wi = 0; g_dirty = 1; continue;
        }
        if (ctrl && scan == 0x17) { toggle_launcher(); continue; }               /* Ctrl-Esc: Start menu */
        if (ctrl && (uni == 'w' || uni == 'W' || uni == 0x17)) { close_focused(); continue; }  /* Ctrl-W */
        /* Ctrl-M minimizes the focused window - the ctrl-reachable twin of
           Alt+Down, for keyboards whose transport reports no Alt. No control-
           code alias: Ctrl-M's is 0x0D, which is Enter, the same reason Ctrl-I
           has no 0x09 alias. Never fires while a field has the caret. */
        if (ctrl && (uni == 'm' || uni == 'M') && !typing_in_field()) {
            minimize_app(wm_focused_app()); continue;
        }
        /* Virtual desktops: Ctrl+F1..F4 switch, Alt+Ctrl+F1..F4 move the
           focused window there and follow it. EFI scans 0x0B..0x0E (F1..F4),
           the same contiguous block the F2 = 0x0C switcher uses - which is why
           this sits AHEAD of it and why that test now demands no ctrl.
           One carve-out: with the Browser focused, Ctrl+F4 stays its
           close-tab. Losing tab-close outright is a real regression, whereas
           desktop 4 is still one pager click, one Alt+Ctrl+F4, or the same key
           from any other window away. */
        if (ctrl && scan >= 0x0B && scan <= 0x0E &&
            !(scan == 0x0E && !(mods & UI_MOD_ALT) && g_open[EX_BROWSER] &&
              UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
              UI.win[UI.focus_win] == &g_win[EX_BROWSER])) {
            if (mods & UI_MOD_ALT)                       /* the set goes too */
                wm_group_desk_move(wm_target_app(), scan - 0x0B, 1);
            else                   wm_desk_switch(scan - 0x0B);
            continue;
        }
        /* Alt-Tab family. The Alt form commits on the release edge; F2 and
           Ctrl-Tab are the ctrl-reachable fallback for keyboards whose
           transport cannot report Alt, and commit on sw_tick()'s timer. */
        if ((mods & UI_MOD_ALT) && uni == 0x09)
            { sw_step((mods & UI_MOD_SHIFT) != 0, 1); continue; }
        if ((scan == 0x0C && !ctrl) || (ctrl && uni == 0x09))
            { sw_step((mods & UI_MOD_SHIFT) != 0, 0); continue; }   /* F2 / Ctrl-Tab */
        /* Alt window commands. Every one of these is reachable another way
           (the titlebar, the taskbar chip, Ctrl-W), because a USB keyboard
           reports no Alt at all until the usb lane's modifier byte lands. */
        if ((mods & UI_MOD_ALT) && !(mods & UI_MOD_CTRL)) {
            int fa  = wm_focused_app();
            int cur = (fa >= 0) ? g_win[fa].snap : WM_SNAP_NONE;
            if (scan == 0x01)                                         /* Alt+Up    */
                { wm_snap(fa, cur == WM_SNAP_MAX ? WM_SNAP_NONE : WM_SNAP_MAX); continue; }
            if (scan == 0x04) { wm_snap(fa, WM_SNAP_L); continue; }   /* Alt+Left  */
            if (scan == 0x03) { wm_snap(fa, WM_SNAP_R); continue; }   /* Alt+Right */
            if (scan == 0x02) {                                       /* Alt+Down  */
                /* un-snap first, minimize only once already free-floating.
                   Minimizing goes through the phase-B policy path (focus
                   handoff, chip repaint, session), not the bare park - Alt+D's
                   bulk park is the only route that skips those. */
                if (cur != WM_SNAP_NONE) wm_snap(fa, WM_SNAP_NONE);
                else                     minimize_app(fa);
                continue;
            }
            if (uni == 'd' || uni == 'D') { wm_show_desktop(); continue; }
        }
        /* Editor accelerators (Ctrl-S/O/N, B/I/U, X/C/V, A, F...) - only when
           the Editor window is in front; pc64_write.c owns the mapping. */
        if (ctrl && !g_launch_open && !UI.full && g_open[APP_EDIT] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[APP_EDIT]) {
            if (pc64_write_key(uni, 1)) { g_dirty = 1; continue; }
        }
        /* Studio accelerators (Ctrl-S/F, F5 Run, F7 Build, ...) - only while
           its window is in front; the module owns the mapping. */
        if (!g_launch_open && !UI.full && g_studio && g_open[EX_STUDIO] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[EX_STUDIO] && g_studio->key) {
            if (g_studio->key(uni, scan, ctrl)) { g_dirty = 1; continue; }
        }
        if (!g_launch_open && !UI.full && g_photos && g_open[EX_PHOTOS] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[EX_PHOTOS] && g_photos->key) {
            if (g_photos->key(uni, scan, ctrl)) { g_dirty = 1; continue; }
        }
        /* UnoWord takes EVERY key while it is in front - it is a word
           processor, so the plain characters are the point, not just the
           accelerators. */
        if (!g_launch_open && !UI.full && g_uoword && g_open[EX_UOWORD] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[EX_UOWORD] && g_uoword->key) {
            if (g_uoword->key(uni, scan, ctrl)) { g_dirty = 1; continue; }
        }
        if (!g_launch_open && !UI.full && g_uocalc && g_open[EX_UOCALC] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[EX_UOCALC] && g_uocalc->key) {
            if (g_uocalc->key(uni, scan, ctrl)) { g_dirty = 1; continue; }
        }
        /* a focused Python app's accelerators */
        if (!g_launch_open && !UI.full && g_pyapp && g_open[EX_PYAPP] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[EX_PYAPP] && g_pyapp->key) {
            if (g_pyapp->key(uni, scan, ctrl)) { g_dirty = 1; continue; }
        }
        /* Install window focused: keyboard drive (works before any pointer is
           up - important on the harness AND on laptops with exotic trackpads).
           Up/Down pick a target, I = Install, R = Rescan. */
        if (!g_launch_open && !UI.full && g_open[APP_SETUP] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[APP_SETUP]) {
            /* ...but NOT while the confirm box has focus. The word the user
               has to type is ERASE, which contains R - so leaving the
               accelerators live would make it impossible to type (every R
               would rescan, and rescanning clears the box). Let the edit
               widget have the keystrokes. */
            int typing = (UI.focus_wi == g_inst_conf_wi && g_inst_conf_wi >= 0);
            int used = !typing;
            if (typing) {
                /* leave the box on Esc so the accelerators come back */
                if (scan == 0x17) { UI.focus_wi = -1; g_dirty = 1; continue; }
                /* otherwise fall through: the edit widget gets the keystroke */
            }
            else if (scan == 0x01) inst_select(g_inst_sel - 1);      /* up   */
            else if (scan == 0x02) inst_select(g_inst_sel + 1);      /* down */
            /* C puts the caret in the confirm box. It needs its own key: Tab
               cycles through every focusable widget, and until focus actually
               lands on the box the letters of ERASE are still accelerators -
               the R would hit Rescan, which clears the box. A dedicated key
               that appears in neither ERASE nor the other accelerators makes
               "select target, C, type, Esc, I" a path that cannot misfire. */
            else if (uni == 'c' || uni == 'C') {
                if (g_inst_conf_wi >= 0) UI.focus_wi = g_inst_conf_wi;
            }
            else if (uni == 'i' || uni == 'I') inst_go();
            else if (uni == 'r' || uni == 'R') inst_rescan();
            else used = 0;
            if (used) { g_dirty = 1; continue; }
        }
        /* Browser focused: its Ctrl accelerators (L address bar, T new tab,
           D bookmark, B/H panels, R / F5 reload, Ctrl-F4 close tab). A canvas
           app never sees a Ctrl-modified character - CHAR events carry no
           mods - so the shell has to route them, the same way it does for the
           Editor and Studio. */
        if (!g_launch_open && !UI.full && g_open[EX_BROWSER] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[EX_BROWSER]) {
            if (pc64_browser_key(uni, scan, ctrl)) { g_dirty = 1; continue; }
        }
        /* Music focused: same keyboard-drive rationale as Install above -
           the player must be fully operable with no pointer at all. */
        if (!g_launch_open && !UI.full && g_open[APP_MUSIC] &&
            UI.focus_win >= 0 && UI.focus_win < UI.nwin &&
            UI.win[UI.focus_win] == &g_win[APP_MUSIC]) {
            if (pc64_music_key(uni, scan)) { g_dirty = 1; continue; }
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
    /* MRU: a pointer click that changed the front window has to reach the
       switcher's order too, and clicks land through unoui, not through a shell
       entry point we could hook. Sampling the front app once per pump is
       cheap and catches every route (click, action, module raise). Skipped
       while the switcher is up - its own window would otherwise take slot 0. */
    if (!g_sw_open) {
        int fa = wm_focused_app();
        if (fa >= 0) {
            /* the SAME sample drives phase F's group raise: a link group comes
               up as a set when one of its windows takes focus. Only on a real
               change of front app, and never mid-drag - raising rewrites the
               z-list under a live capture (wm_raise_group repairs cap_win, but
               there is no reason to churn it every frame). */
            int changed = !g_nmru || g_mru[0] != fa;
            wm_note_focus(fa);
            if (changed && UI.cap_mode == UI_CAP_NONE) wm_raise_group(fa);
        }
    }
    /* 0 = nothing; 1 = something changed, full repaint; 2 = cursor moved only,
       present recomposites the cursor without the full-scene painter. */
    return real ? 1 : (any ? 2 : 0);
}

/* system-tray clock: the firmware RTC wall time.  24-hour "HH:MM:SS" by
 * default, or 12-hour "H:MM:SS AM/PM" when g_clock_12h is set (Control Panel). */
static void fmt_clock(int uptime_secs)
{
    int h = 0, mi = 0, s = 0;
    if (uno_pc64_time(0, 0, 0, &h, &mi, &s)) {
        if (g_clock_12h) {
            int pm = h >= 12, h12 = h % 12; if (h12 == 0) h12 = 12;   /* 0/12 -> 12 */
            int j = 0;
            if (h12 >= 10) g_clock[j++] = '0' + h12 / 10;             /* no leading 0 */
            g_clock[j++] = '0' + h12 % 10;                g_clock[j++] = ':';
            g_clock[j++] = '0'+(mi/10)%10; g_clock[j++] = '0'+mi%10;  g_clock[j++] = ':';
            g_clock[j++] = '0'+(s/10)%10;  g_clock[j++] = '0'+s%10;   g_clock[j++] = ' ';
            g_clock[j++] = pm ? 'P' : 'A';  g_clock[j++] = 'M';       g_clock[j] = 0;
        } else {
            g_clock[0]='0'+(h/10)%10;  g_clock[1]='0'+h%10;  g_clock[2]=':';
            g_clock[3]='0'+(mi/10)%10; g_clock[4]='0'+mi%10; g_clock[5]=':';
            g_clock[6]='0'+(s/10)%10;  g_clock[7]='0'+s%10;  g_clock[8]=0;
        }
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
    g_batt_pct = pct;
    if (pct >= 0) {
        int j = 0;
        if (pct >= 100) g_batt[j++] = '0' + (pct / 100) % 10;
        if (pct >= 10)  g_batt[j++] = '0' + (pct / 10) % 10;
        g_batt[j++] = '0' + pct % 10;
        g_batt[j++] = '%';
        g_batt[j] = 0;
    } else
        g_batt[0] = 0;
#else
    g_batt_pct = -1;
#endif
}

/* Draw a small battery glyph in [x,y] within a `bh`-tall chip: an outline body,
 * a positive-terminal nub, and an inner fill bar proportional to `pct` (green
 * when healthy, amber low, red critical).  Width is fixed at batt_icon_w(). */
static int batt_icon_w(void) { return 22 + 2; }   /* body 22 + nub 2 */
static void draw_batt_icon(int x, int y, int bh, int pct, fb_px outline)
{
    int bw = 22, bhh = fb_text_h() - 2; if (bhh < 8) bhh = 8;
    int by = y + (bh - bhh) / 2, nub = bhh / 3;
    fb_px fill = pct >= 40 ? FB_RGB(60, 200, 90)
               : pct >= 15 ? FB_RGB(232, 200, 40)
                           : FB_RGB(230, 70, 60);
    fb_frame_rect(x, by, bw, bhh, outline);                 /* body outline */
    fb_fill_rect(x + bw, by + (bhh - nub) / 2, 2, nub, outline);  /* + terminal */
    if (pct > 0) {                                          /* charge level */
        int iw = (bw - 4) * (pct > 100 ? 100 : pct) / 100; if (iw < 1) iw = 1;
        fb_fill_rect(x + 2, by + 2, iw, bhh - 4, fill);
    }
}

/* ---- drag-frame cost -------------------------------------------------------
 * The window-management spec budgets a live opaque drag against the old
 * rubber-band one, so the cost of a MOVED drag frame has to be a number, not an
 * impression. Accumulate render+present cycles per moved frame and report the
 * average when the drag ends: it lands in the kernel log (and the debug
 * console), which a harness run can read out of the boot log - the on-screen
 * HUD averages every frame and is unreadable from a screenshot. */
#ifdef UNO_DEBUG
static unsigned long long g_drag_cyc, g_drag_paint;
static unsigned long      g_drag_frames;
static unsigned long long drag_cyc_now(void) { return uno_native_rdtsc(); }
static void drag_cyc_note(unsigned long long c) { g_drag_cyc += c; g_drag_frames++; }
static void drag_paint_note(unsigned long long c) { g_drag_paint += c; }
static void drag_cyc_report(const char *how)
{
    if (!g_drag_frames) return;
    /* the paint split matters: everything except `paint` is the snapshot
     * restore plus present, which BOTH drags pay, so it is the paint alone
     * that an opaque drag adds over a rubber band. */
    uno_dbg_log("wm: %s drag: %lu frames, %lu kcyc/frame (paint %lu kcyc)",
                how, g_drag_frames,
                (unsigned long)(g_drag_cyc / g_drag_frames / 1000),
                (unsigned long)(g_drag_paint / g_drag_frames / 1000));
    g_drag_cyc = 0; g_drag_paint = 0; g_drag_frames = 0;
}
#else
static unsigned long long drag_cyc_now(void) { return 0; }
#define drag_cyc_note(c)    ((void)(c))
#define drag_paint_note(c)  ((void)(c))
#define drag_cyc_report(h)  ((void)(h))
#endif

/* ---- live drag: the scene WITHOUT the window being dragged -----------------
 * The snapshot an opaque drag restores each frame must not contain the dragged
 * window, or it would smear a copy of it at the position the drag started. So
 * lift the window out of the z-list for exactly one render pass and put it back
 * at the same index. focus_win moves with it, and while it is out there is no
 * focused app window at all - which would blank the taskbar's active chip for
 * the whole drag, since the snapshot is taken once - so focused_app() is told
 * to keep naming it. */
/* the windows a live drag lifts: the dragged one plus, when it belongs to a
 * link group, every member of that set - they all move, so they must all be
 * missing from the snapshot and all be repainted per frame. Bottom-to-top z
 * order, which is also the order they are repainted in. */
static unoui_window *g_dragset[NAPPS];
static int g_ndragset;

static void drag_set_build(unoui_window *dw)
{
    int a = -1, i, k, set[NAPPS], n;
    g_ndragset = 0;
    if (!dw) return;
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == dw) { a = i; break; }
    if (a < 0 || !g_group[a]) { g_dragset[g_ndragset++] = dw; return; }
    n = wm_group_set(a, set);
    for (k = 0; k < UI.nwin; k++)
        for (i = 0; i < n; i++)
            if (UI.win[k] == &g_win[set[i]] && !g_parked[set[i]])
                { g_dragset[g_ndragset++] = UI.win[k]; break; }
}

static void drag_scene_without(unoui_window *win)
{
    unoui_window *save[UNOUI_MAX_WINDOWS], *fw;
    int nsave = UI.nwin, ofocus = UI.focus_win, i, j, keep = 0;
    if (!win || !g_ndragset) { unoui_render_ui(&UI); return; }
    for (i = 0; i < NAPPS; i++) if (&g_win[i] == win) { g_hidden_app = i; break; }
    fw = (ofocus >= 0 && ofocus < nsave) ? UI.win[ofocus] : 0;
    for (i = 0; i < nsave; i++) save[i] = UI.win[i];
    for (i = 0; i < nsave; i++) {
        for (j = 0; j < g_ndragset; j++) if (save[i] == g_dragset[j]) break;
        if (j < g_ndragset) continue;                  /* lifted for this pass */
        UI.win[keep++] = save[i];
    }
    UI.nwin = keep;
    UI.focus_win = -1;
    if (fw) for (i = 0; i < keep; i++) if (UI.win[i] == fw) { UI.focus_win = i; break; }
    unoui_render_ui(&UI);
    for (i = 0; i < nsave; i++) UI.win[i] = save[i];
    UI.nwin = nsave;
    UI.focus_win = ofocus;
    g_hidden_app = -1;
}

/* ---- live drag: the dragged window's pixels, cached ------------------------
 * A window's CONTENT cannot change while its title bar is being dragged, so
 * re-running the widget pass every frame is wasted work. Cache the rendered
 * window once at drag start and blit it thereafter.
 *
 * What a move DOES invalidate is the translucent perimeter - the drop shadow
 * (six expanding alpha layers) and the anti-aliased corner arcs - because those
 * are composited against whatever is behind the window, and behind it is
 * different at every position. Measured on this build, that perimeter is also
 * where most of the paint went: 49.8 Mcyc per drag frame, of which 31 Mcyc was
 * the shadow alone. So the cached blit deliberately SKIPS the four corner
 * squares, and the perimeter is repainted CLIPPED to a thin ring: the shadow
 * still costs six passes, but over ~21 k pixels of ring instead of ~178 k
 * pixels of window.
 *
 * Falls back to a full unoui_render_window whenever the cache is unusable (no
 * memory, or the window changed size mid-drag, which happens when a drag
 * un-snaps a snapped window). */
extern void *malloc(unsigned long);      /* pc64_libc.c; no libc header here   */
extern void  free(void *);

static fb_px *g_dragpix;                 /* cached window image, row-major     */
static int g_dragpix_cap;                /* pixels the buffer can hold         */
static int g_dragpix_w, g_dragpix_h;     /* the rect it was captured at        */

/* how far past win->r the theme's shadow reaches, and the corner radius the
 * anti-aliased arcs occupy. Generous by a few pixels on purpose: too wide only
 * repaints ring that did not need it, too narrow leaves a stale halo. */
static int drag_ring_px(void) { return UI.theme->m.shadow_off + 6; }
static int drag_corner_px(void)
{
    int rad = UI.theme->m.radius + 2, lim;
    if (rad < 2) rad = 2;
    lim = (UI.theme->m.frame_w + 2);
    if (rad < lim) rad = lim;
    return rad;
}

static void drag_cache_drop(void)
{
    if (g_dragpix) { free(g_dragpix); g_dragpix = 0; }
    g_dragpix_cap = g_dragpix_w = g_dragpix_h = 0;
}

/* Copy the window just drawn at win->r out of the framebuffer. */
static void drag_cache_take(const unoui_window *win)
{
    int w = win->r.w, h = win->r.h, y;
    if (w <= 0 || h <= 0 || win->r.x < 0 || win->r.y < 0 ||
        win->r.x + w > FB_W || win->r.y + h > FB_H) { drag_cache_drop(); return; }
    if (g_dragpix_cap < w * h) {
        drag_cache_drop();
        g_dragpix = (fb_px *)malloc((unsigned long)w * h * sizeof(fb_px));
        if (!g_dragpix) return;
        g_dragpix_cap = w * h;
    }
    for (y = 0; y < h; y++)
        memcpy(g_dragpix + (long)y * w, fb + (long)(win->r.y + y) * FB_W + win->r.x,
               (unsigned long)w * sizeof(fb_px));
    g_dragpix_w = w; g_dragpix_h = h;
}

/* Repaint the chrome clipped to one box. */
static void drag_chrome_in(unoui_window *win, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    fb_set_clip(x, y, w, h);
    unoui_render_window_chrome(&UI, win);
    fb_reset_clip();
}

/* Put the cached window back at its new rect. 0 = the cache was unusable and
 * the caller must render the window the slow way. */
static int drag_blit_window(unoui_window *win)
{
    int x = win->r.x, y = win->r.y, w = win->r.w, h = win->r.h;
    int s = drag_ring_px(), c = drag_corner_px();
    if (!g_dragpix || g_dragpix_w != w || g_dragpix_h != h) return 0;
    if (c * 2 > w || c * 2 > h) return 0;          /* window smaller than its own corners */
    /* Everything but the four corner squares is opaque, so it blits verbatim.
       The corners are left showing the restored scene, which is exactly the
       background the arcs below must composite against. */
    fb_blit(x + c, y,         w - 2 * c, c,         g_dragpix + c, w);
    fb_blit(x,     y + c,     w,         h - 2 * c, g_dragpix + (long)c * w, w);
    fb_blit(x + c, y + h - c, w - 2 * c, c,
            g_dragpix + (long)(h - c) * w + c, w);
    /* the four corner arcs, then the shadow ring outside the window */
    drag_chrome_in(win, x,         y,         c, c);
    drag_chrome_in(win, x + w - c, y,         c, c);
    drag_chrome_in(win, x,         y + h - c, c, c);
    drag_chrome_in(win, x + w - c, y + h - c, c, c);
    drag_chrome_in(win, x - s,     y - s,     w + 2 * s, s);
    drag_chrome_in(win, x - s,     y + h,     w + 2 * s, s);
    drag_chrome_in(win, x - s,     y,         s,         h);
    drag_chrome_in(win, x + w,     y,         s,         h);
    return 1;
}

/* Windows pinned ABOVE the dragged one - the taskbar, a popover - must stay
 * above it. The scene snapshot is taken once and the dragged window is drawn
 * over it every frame, so nothing put them back: a window dragged to the
 * bottom of the screen painted straight across the taskbar.
 *
 * They are RE-BLITTED, not re-rendered. The taskbar's own painter blends (it
 * is a translucent bar), so drawing it again over the window shows the window
 * through it. The snapshot already holds them correctly composited over the
 * desktop, so the fix is to keep those pixels and put them back. One union
 * rect covers the lot; the desktop it may also span is part of the same static
 * scene, so blitting it back is a no-op there. */
static fb_px *g_dragtop;
static int g_dragtop_cap;
static unoui_rect g_dragtop_r;

static void drag_top_drop(void)
{
    if (g_dragtop) { free(g_dragtop); g_dragtop = 0; }
    g_dragtop_cap = 0; g_dragtop_r.w = g_dragtop_r.h = 0;
}

/* Call with the fb holding the scene WITHOUT the dragged window. */
static void drag_top_take(int wi)
{
    unoui_rect u; int k, first = 1, y;
    drag_top_drop();
    u.x = u.y = u.w = u.h = 0;
    for (k = wi + 1; k < UI.nwin; k++) {
        unoui_rect o = UI.win[k]->r;
        if (o.w <= 0 || o.h <= 0) continue;
        if (first) { u = o; first = 0; continue; }
        if (o.x < u.x)                 { u.w += u.x - o.x; u.x = o.x; }
        if (o.y < u.y)                 { u.h += u.y - o.y; u.y = o.y; }
        if (o.x + o.w > u.x + u.w)     u.w = o.x + o.w - u.x;
        if (o.y + o.h > u.y + u.h)     u.h = o.y + o.h - u.y;
    }
    if (first) return;                              /* nothing pinned above */
    if (u.x < 0) { u.w += u.x; u.x = 0; }
    if (u.y < 0) { u.h += u.y; u.y = 0; }
    if (u.x + u.w > FB_W) u.w = FB_W - u.x;
    if (u.y + u.h > FB_H) u.h = FB_H - u.y;
    if (u.w <= 0 || u.h <= 0) return;
    g_dragtop = (fb_px *)malloc((unsigned long)u.w * u.h * sizeof(fb_px));
    if (!g_dragtop) return;
    g_dragtop_cap = u.w * u.h;
    for (y = 0; y < u.h; y++)
        memcpy(g_dragtop + (long)y * u.w, fb + (long)(u.y + y) * FB_W + u.x,
               (unsigned long)u.w * sizeof(fb_px));
    g_dragtop_r = u;
}

static void drag_top_put(unoui_rect r)
{
    unoui_rect u = g_dragtop_r;
    if (!g_dragtop || u.w <= 0) return;
    if (r.x >= u.x + u.w || u.x >= r.x + r.w ||
        r.y >= u.y + u.h || u.y >= r.y + r.h) return;   /* window nowhere near */
    fb_blit(u.x, u.y, u.w, u.h, g_dragtop, u.w);
}

/* ---- a link group on the cached drag path (phase F) ------------------------
 * The pixel cache above holds ONE window, and the grabbed one is the window
 * worth caching: it is the one under the pointer and the one that moves every
 * frame. A set's other members move by the same delta but are otherwise
 * ordinary, so they are simply repainted - one extra window paint per peer,
 * which is the same honest cost model phase A measured for the drag itself.
 * They go down FIRST so the grabbed window stays on top of its own set, which
 * is the z-order wm_raise_group() established before the drag began. */
static void drag_paint_peers(unoui_window *dw)
{
    int k;
    for (k = 0; k < g_ndragset; k++)
        if (g_dragset[k] != dw) unoui_render_window(&UI, g_dragset[k]);
}

/* Pinned chrome goes back over EVERY member's rect: with a set, a peer can be
 * under the taskbar while the grabbed window is nowhere near it, and
 * drag_top_put() early-outs on exactly that test. */
static void drag_top_put_set(void)
{
    int k;
    for (k = 0; k < g_ndragset; k++) drag_top_put(g_dragset[k]->r);
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
    int idle = 0, halfsecs = 0, was_dragging = 0, dragging = 0;

    uno_pc64_init();
    unoui_ui_init(&UI, &theme_aurora_light, FB_W, FB_H);   /* modern default look */
    /* Opaque window drag. The rubber band existed because dragging used to
       re-run the whole alpha-blend scene painter per mouse move; the scene
       snapshot in the frame loop below made that obsolete, and this port can
       afford the fb-sized buffer it needs (it already keeps one for
       UNO_BG_CACHE). Ports that cannot leave live_drag 0 and keep the band. */
    UI.live_drag = 1;
    set_workarea();                     /* windows clamp/maximize inside the bar */
    unoui_icon_art = pc64_icon_art;     /* distinct per-app icon artwork */
    unoui_win_badge = shell_win_badge;  /* the link-group dot in a title bar */
    unoui_wallpaper = pc64_wallpaper_paint;  /* Control Panel wallpaper picker */
    unoui_font_push = uno_font_push;    /* per-window font overrides (Editor doc font) */
    unoui_font_pop  = uno_font_pop;
    uno_mac_mouse   = uno_pc64_mac_mouse;   /* live pointer for Paint's drag spin */
    uno_xhci_init();                    /* USB host controller (inert unless -DUNO_XHCI) */
    if (!ax88179_nic())                 /* bind a USB Ethernet adapter if one is attached */
        rtl8152_nic();                  /* ...else try a Realtek USB NIC (docks/dongles) */
    /* Intel WiFi is bound lazily on first net use (pc64_net_up): it reads
       WIFI.CFG + firmware off the ESP and runs a scan/join that can take a
       few seconds - not something to do on the boot path. */
    /* Proactive network bring-up (pc64_net_boot) runs once at the top of the
       main loop, AFTER the debug net test has had first crack - see there. */
    uno_seq_init();                     /* UnoSound: PC-speaker voice */
    uno_seq_backend(uno_pc64_snd_note, uno_pc64_snd_quiet);
    unoapp_setup(&g_dirty);             /* wire the legacy-app KernelApi */
    g_studio_present = uno_mod_present("STUDIO.UNO");   /* IDE shipped here? */
    g_photos_present = uno_mod_present("PHOTOS.UNO");   /* viewer shipped?   */
    g_uoword_present = uno_mod_present("UOWORD.UNO");   /* UnoWord shipped?  */
    g_uocalc_present = uno_mod_present("UOCALC.UNO");   /* UnoCalc shipped?  */
    uno_font_set_subpixel(1);           /* subpixel AA for the outline faces  */
    /* Default to the bundled Chicago-style bitmap face (slot 0). It renders at
     * its native px with AA off (crisp 1:1 pixels). If its TTF can't be loaded
     * (e.g. missing from the ESP) uno_font_use falls back to the built-in 8x8
     * bitmap, which the Control Panel picker ("System (mono)") also selects.
     * MUST run before the shell chrome is built: the taskbar, icon grid and
     * launcher are all laid out in the live font's metrics. */
    uno_font_use(0);
    /* Security: register the escalation consent sheet with unosecure, then run
     * the boot login gate.  The gate is a no-op on a fresh machine (no accounts
     * yet), so existing/first boots reach the desktop unchanged; once accounts
     * exist it blocks here until a valid login binds the shell session. */
    pc64_consent_register();
    pc64_login_gate();
    build_desktop();  unoui_ui_add(&UI, &g_desk);   /* bottom: icon layer  */
    build_taskbar();  unoui_ui_add(&UI, &g_task);   /* top: the taskbar    */
    build_launcher();                                /* opened via Start    */
    fmt_clock(0);                                    /* tray clock ready now */
    fmt_batt();                                      /* tray battery (ACPI)  */
    fmt_net();                                       /* tray LAN chip        */
    session_load();                     /* reopen last session (or Control Panel) */

    memset(&tick, 0, sizeof tick); tick.kind = UI_EV_TICK;
    int netboot_done = 0, netboot_frames = 0;   /* one-shot proactive net bring-up */
#ifdef UNO_DEBUG
    /* Prove the shell's main loop was reached and that the debug hooks are
     * live. If the HUD or the stress driver never appear on a machine, the
     * boot log now distinguishes "the loop never ran" from "the loop ran but
     * the hook did nothing". */
    { char hb[96]; int n = uno_dbg_hud(hb, sizeof hb);
      uno_dbg_log("shell: main loop entered, fb=%dx%d, hud_len=%d (%s)",
                  FB_W, FB_H, n, n > 0 ? hb : "HUD DISABLED"); }
    unoui_profile_win = uno_dbg_win_profile;   /* F11: per-window draw timing */
#endif
    for (;;) {
        int la, cursor_only = 0;
        uno_dbg_heartbeat();            /* debug build: the watchdog's liveness */
        pc64_nettest_tick();            /* debug build: network hw test + the
                                           conformance suite, runs once and
                                           blocks this frame while it does */
        /* Proactive network bring-up: once, a few frames AFTER the debug net
           test has had first crack. The net test waits 30 frames then runs its
           one-shot (blocking), so firing at frame 35 guarantees it has run (and
           lets the desktop paint first). pc64_net_boot() no-ops if the net is
           already leased, so when the net test brought a NIC up this does nothing
           and there is no double-net_init; it only fires as a FALLBACK when
           nothing leased - which is what raises the link on a debug box with a
           DEBUG.CFG whose net test does not bring up the wired NIC (the
           ZimaBlade), and on any installed OS with no DEBUG.CFG. Bounded, so a
           dead NIC cannot hang it; `nonet` disables it. In production the net
           test is a no-op, so this is simply the eager boot bring-up. */
        if (!netboot_done && ++netboot_frames >= 35) {
            int pc64_net_boot(void);                    /* pc64_http.c */
            netboot_done = 1;
#ifdef UNO_DEBUG
            {
                int pc64_stress_cfg_flag(const char *key);   /* pc64_stress.c */
                if (pc64_stress_cfg_flag("nonet") <= 0) pc64_net_boot();  /* not `nonet` */
            }
#else
            pc64_net_boot();
#endif
        }
        unoauto_remote_tick();          /* debug build: pump the dev-PC remote
                                           link (armed by unoauto_remote_boot) */
        uno_screen_capture_tick();      /* debug build: server-side screen record
                                           (armed by `screen record start`) */
        netdisc_tick();                 /* debug build: zero-config LAN discovery
                                           (armed by netdisc_boot) */
        /* pc64_stress_tick() REMOVED 2026-07-21 (user request): the continuous
         * fuzz driver ran even when unticked / looped forever. Disconnected here
         * AND hard-disabled in pc64_stress.c so no DEBUG.CFG value can revive
         * it. Conformance + net tests above are unaffected. */
        /* Keep the net stack breathing. Nothing else pumps it from the frame
         * loop - net_poll() only ran inside blocking loops (a fetch, TLS, the
         * join dialog) - so DHCP's retransmit timer froze the moment such a
         * loop ended, and a lease that would have arrived a second later never
         * did. That is why a joined WiFi link could sit with no IP address
         * forever. No-op with no NIC bound (S-NET-02). */
        { int np; for (np = 0; np < 4; np++) net_poll(); }
        /* a lease that lands out here still has to reach the screen: refresh
         * the Control Panel's network pane on the transition */
        { static int last_lease = -1;
          int lease = net_dhcp_done();
          if (lease != last_lease) {
              last_lease = lease;
              if (g_open[APP_CTRL] && g_ctrl_tab == CT_NETWORK) rebuild_ctrl_window();
          } }

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
        { int pr = pump_input();
          if (pr == 1)      { g_dirty = 1; idle = 0; }
          else if (pr == 2) { cursor_only = 1; idle = 0; }   /* moved only: present, no repaint */
          else if (++idle >= 30) {      /* ~0.5 s: caret blink + tray clock tick */
              idle = 0; g_dirty = 1;
              fmt_clock(++halfsecs / 2);
              fmt_batt();               /* AML _BST, self-cached ~2 s */
              net_activity_sample();    /* LAN chip blink: tx/rx delta this tick */
              if ((halfsecs & 3) == 0) fmt_net();  /* LAN chip label, ~2 s cadence */
          }
        }
        /* LAN chip hover -> tooltip (IP + link speed). Repaint only on the
           edge so a still pointer over the chip keeps the last frame's tip. */
        { int mx, my, mb; uno_pc64_mouse(&mx, &my, &mb);
          int over = g_net[0] && mx >= g_net_cx && mx < g_net_cx + g_net_cw &&
                     my >= g_net_cy && my < g_net_cy + g_net_ch;
          if (over != g_net_hover) { g_net_hover = over; g_dirty = 1; } }
        feed(&tick);                    /* advance the caret-blink timebase */
        pc64_write_frame();             /* Editor caret blink / autoscroll */
        if (g_studio && g_open[EX_STUDIO] && g_studio->frame)
            g_studio->frame();          /* Studio caret blink / build pumps */
        if (g_photos && g_open[EX_PHOTOS] && g_photos->frame)
            g_photos->frame();          /* Photos: GIF animation pump */
        if (g_uoword && g_open[EX_UOWORD] && g_uoword->frame)
            g_uoword->frame();          /* UnoWord: caret blink              */
        if (g_uocalc && g_open[EX_UOCALC] && g_uocalc->frame)
            g_uocalc->frame();          /* UnoCalc: per-frame tick           */
        if (g_pyapp && g_open[EX_PYAPP] && g_pyapp->frame)
            g_pyapp->frame();           /* the Python app's per-frame tick   */
        if (g_open[EX_USERAPP]) unoapp_user_tick();  /* the user's app clock */
        uno_seq_tick();                 /* UnoSound: advance music/SFX ~60 Hz */
        if (g_open[APP_CLOCK]) pc64_clock_tick();  /* self-throttling: only
                                        redraws when the second changes */
        if (g_open[APP_MUSIC]) pc64_music_tick();  /* decode ahead into the PCM
                                        stream; bounded by FIFO space, so this
                                        never blocks the frame */
        if (g_open[APP_UNOAMP]) { void unoamp_tick(void); unoamp_tick(); }
                                       /* the same pull, through the plugin
                                        graph: decode -> DSP -> sink */
        if (g_launch_open || g_pop_open) {   /* menu hover highlight + scroll */
            int mx, my, mb; uno_pc64_mouse(&mx, &my, &mb);
            if (g_launch_open) launcher_hover(mx, my);
            if (g_pop_open)    pop_hover(mx, my);
        }
        sw_tick();                      /* Alt-Tab: the release edge + timer   */
        la = active_legacy();           /* drive the focused game/tool clock */
        { int g = (la >= 0) ? app_game(NNATIVE + la) : -1;
          if (g >= 0) { pc64_game_tick(g); g_dirty = 1; unoapp_focus(-1); }  /* native game */
          else if (la >= 0 && app_is_bridge(NNATIVE + la)) { unoapp_focus(la); unoapp_run_tick(la); }
          else unoapp_focus(-1); }                                            /* browser: no tick */
        if (UI.full) g_dirty = 1;       /* fullscreen apps redraw every frame */
        /* Window drag, both flavours, on ONE scene-snapshot fast path: the
           desktop and the other windows are static while a title bar is
           dragged, so render the full scene once when the drag begins,
           snapshot it, and per moved frame restore the snapshot and redraw
           only the thing that moved - instead of re-running the (alpha-blend
           heavy) full-scene painter on every mouse move. That is what made
           dragging smooth, and it is what makes an OPAQUE drag affordable:
           the per-frame work goes from "restore + 3 rects" to "restore + one
           window", not to a whole scene.

           live (UI.live_drag):  the snapshot is taken with the dragged window
                                 REMOVED, and each frame draws the window at
                                 its new rect over it.
           outline:              the snapshot includes every window and each
                                 frame draws the rubber band. */
        dragging = UI.drag_active || (UI.live_drag && UI.cap_mode == UI_CAP_WINDOW);
        if (dragging) {
            unoui_window *dw = (UI.cap_win >= 0 && UI.cap_win < UI.nwin)
                             ? UI.win[UI.cap_win] : 0;
            if (!was_dragging) {                 /* drag just began */
                if (UI.drag_active) {
                    UI.drag_active = 0;          /* render the scene without the outline */
                    unoui_render_ui(&UI);
                    UI.drag_active = 1;
                } else {
                    drag_set_build(dw);          /* the window, or its whole set */
                    drag_scene_without(dw);      /* ...or without the window   */
                }
                uno_pc64_scene_save();
                if (UI.drag_active) unoui_draw_drag_outline(&UI);
                else if (dw)      { /* the fb still holds the scene WITHOUT the
                                       dragged set: exactly the state both
                                       caches want to be taken from. */
                                    drag_top_take(UI.cap_win);
                                    unoui_draw_snap_preview(&UI);
                                    drag_paint_peers(dw);
                                    unoui_render_window(&UI, dw);
                                    drag_cache_take(dw);
                                    drag_top_put_set(); }
                uno_pc64_present();
                g_dirty = 0;
            } else if (g_dirty) {                /* the drag moved */
                unsigned long long t0 = drag_cyc_now(), t1;
                uno_pc64_scene_restore();
                t1 = drag_cyc_now();
                /* the snap target goes UNDER the dragged window: it marks
                   where the window is going, so the window itself must stay
                   the thing you are looking at. */
                if (UI.drag_active) unoui_draw_drag_outline(&UI);
                else if (dw)      {
                    unoui_draw_snap_preview(&UI);
                    drag_paint_peers(dw);          /* a link group moves as one */
                    if (!drag_blit_window(dw)) {   /* cache unusable: the slow
                                                      way, and re-take it (the
                                                      window resized when the
                                                      drag un-snapped it) */
                        unoui_render_window(&UI, dw);
                        drag_cache_take(dw);
                    }
                    drag_top_put_set();
                }
                drag_paint_note(drag_cyc_now() - t1);
                uno_pc64_present();
                drag_cyc_note(drag_cyc_now() - t0);
                g_dirty = 0;
            } else {
                uno_pc64_delay_ms(16);
            }
        } else {
            if (was_dragging) {                  /* drag ended: full repaint to
                                                    commit shadows + occlusion */
                g_ndragset = 0;
                g_dirty = 1;
                drag_cache_drop();               /* content is live again */
                drag_top_drop();
                drag_cyc_report(UI.live_drag ? "live" : "outline");
                session_save();                  /* the new position is session state */
            }
#ifdef UNO_DEBUG
            /* timed render/present + the perf HUD, drawn into the frame the
             * same way any widget is so the present path carries it */
            if (g_dirty) {
                unsigned long long t0, t1;
                uno_dbg_win_frame_reset();
                t0 = uno_native_rdtsc();
                unoui_render_ui(&UI);
                t1 = uno_native_rdtsc();
                uno_dbg_frame_render_cyc(t1 - t0);
                /* Right-align, but NEVER pass a negative x: fb_text ->
                   text_pen() does `x << 6`, which is UB on a negative int and
                   UBSan-traps (crash CR005/CR009 - a long status string on a
                   narrow desktop went to x=-42). Clamp here; the underlying
                   renderer weakness is catalogued as F7. */
                { char hud[96];
                  int n = uno_dbg_hud(hud, sizeof hud);
                  int hx = FB_W - fb_text_w(hud) - 4; if (hx < 0) hx = 0;
                  if (n > 0) fb_text(hx, 3, hud,
                                     FB_RGB(255, 70, 60), FB_RGB(28, 30, 48));
                  /* run state under the HUD: the operator must be able to see
                     at a glance whether a bounded run has FINISHED (safe to
                     shut down) or is merely slow - guessing that is how a run
                     gets powered off mid-pass. */
                  { const char *st = pc64_stress_status();
                    if (st) {
                        int done = (st[7] == 'C' || st[7] == 'S');   /* COMPLETE/STOPPED */
                        int sx = FB_W - fb_text_w(st) - 4; if (sx < 0) sx = 0;
                        fb_text(sx, 3 + fb_text_h() + 2, st,
                                done ? FB_RGB(120, 255, 140) : FB_RGB(160, 200, 255),
                                FB_RGB(28, 30, 48));
                    } } }
                t1 = uno_native_rdtsc();
                uno_pc64_present();
                uno_dbg_frame_present_cyc(uno_native_rdtsc() - t1);
                g_dirty = 0;
                uno_dbg_frame_idle(0);
            } else if (cursor_only) {
                unsigned long long t1 = uno_native_rdtsc();
                uno_pc64_present();
                uno_dbg_frame_present_cyc(uno_native_rdtsc() - t1);
                uno_dbg_frame_idle(0);
            } else { uno_pc64_delay_ms(16); uno_dbg_frame_idle(1); }
#else
            if (g_dirty) { unoui_render_ui(&UI); uno_pc64_present(); g_dirty = 0; }
            else if (cursor_only) uno_pc64_present();  /* cursor moved: recomposite only */
            else uno_pc64_delay_ms(16);
#endif
        }
        was_dragging = dragging;
    }
    return 0;
}

#ifdef UNO_DEBUG
/* ===========================================================================
 * shell hooks the metal stress driver (pc64_stress.c) drives - thin wrappers
 * over the same open_app/close_focused/g_dirty the real UI uses, so a stress
 * run exercises the true window/app machinery, not a private shadow of it.
 * ======================================================================== */
int  pc64_dbg_app_count(void) { return NAPPS; }
int  pc64_dbg_app_hidden(int a) { return (a < 0 || a >= NAPPS) ? 1 : app_hidden(a); }
const char *pc64_dbg_app_name(int a) { return (a >= 0 && a < NAPPS) ? app_name(a) : "?"; }
int  pc64_dbg_app_is_open(int a) { return (a >= 0 && a < NAPPS) ? g_open[a] : 0; }
int  pc64_dbg_open_count(void)
{ int i, n = 0; for (i = 0; i < NAPPS; i++) if (g_open[i]) n++; return n; }
void pc64_dbg_open_app(int a) { if (a >= 0 && a < NAPPS && !app_hidden(a)) open_app(a); g_dirty = 1; }
void pc64_dbg_close_focused(void) { close_focused(); }
void pc64_dbg_focus_next(void)
{ if (UI.nwin > 0) { UI.focus_win = (UI.focus_win + 1) % UI.nwin; g_dirty = 1; } }
void pc64_dbg_mark_dirty(void) { g_dirty = 1; }
/* open a document through the shell's own browser path (the Help deep-link
 * route): the launcher for the malformed text/markup/html fuzz corpus. */
void pc64_dbg_open_path(const char *path)
{ pc64_browser_open_path(path); g_dirty = 1; }
/* window-manager verbs, same rule: the stress driver and the harness drive the
 * REAL switcher / snap / show-desktop machinery, not a private shadow of it. */
void pc64_dbg_wm_switch(int back) { sw_step(back, 0); }
void pc64_dbg_wm_commit(void)     { sw_commit(); }
int  pc64_dbg_wm_switching(void)  { return g_sw_open; }
void pc64_dbg_wm_snap(int snap)   { wm_snap(wm_focused_app(), snap); }
void pc64_dbg_wm_showdesk(void)   { wm_show_desktop(); }
void pc64_dbg_wm_min(void)        { minimize_app(wm_focused_app()); }
void pc64_dbg_wm_restore(int a)   { restore_app(a); }
/* phase F: the stress driver exercises the real layout machinery, not a copy */
void pc64_dbg_wm_tile(void)       { wm_tile(); }
void pc64_dbg_wm_cascade(void)    { wm_cascade(); }
void pc64_dbg_wm_group(int a, int g) { wm_group_join(a, g); }
int  pc64_dbg_wm_parked(int a)    { return (a >= 0 && a < NAPPS) ? g_parked[a] : 0; }
int  pc64_dbg_wm_mods(void)       { return uno_pc64_mods(); }
void pc64_dbg_wm_desk(int n)      { wm_desk_switch(n); }
int  pc64_dbg_wm_curdesk(void)    { return g_cur_desk; }
int  pc64_dbg_wm_deskof(int a)    { return (a >= 0 && a < NAPPS) ? g_desk_of[a] : 0; }
#endif /* UNO_DEBUG */
