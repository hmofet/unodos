/* ===========================================================================
 * UnoDOS app loader  -  the generic, app-agnostic loader that replaces the
 * kernel's old switch(proc) dispatch.  This file is part of the KERNEL and
 * contains NO app code: it knows only how to (1) build the KernelApi table of
 * callbacks, (2) ask the platform to load a module by app id from storage,
 * (3) resolve its uno_app_main entry, and (4) keep a per-slot AppInterface so
 * the window manager can dispatch through function pointers.
 *
 * The platform provides ONE hook:
 *
 *     UnoAppEntry uno_load_module(short proc);
 *
 * which loads the app's module image from storage (host: dlopen a .so from a
 * directory; PS2: read the relocatable from mc0:/UnoDOS/Apps/ and register it;
 * DC: read it from the CD romdisk) and returns its entry symbol, or NULL.
 *
 * The kernel #includes this file once (it shares the kernel's static helpers).
 * ===========================================================================
 */

/* The KernelApi instance handed to every module.  Filled by app_loader_init()
   from the kernel's own helpers (all visible because this file is #included
   into unodos.c after those helpers are defined). */
static KernelApi gKApi;

/* per-app-id cached AppInterface (resolved on first launch, reused after) */
static const AppInterface *gAppIface[APP_NAPPS];
static int                 gAppTried[APP_NAPPS];

/* topmost_proc shim for the KernelApi (apps used to read zwin(gZCount-1)) */
static short kapi_topmost_proc(void)
{
    if (gZCount <= 0) return -1;
    return zwin(gZCount - 1)->proc;
}

/* the platform hook (host_modload.c / ee_modload.c / dc_modload.c) */
extern UnoAppEntry uno_load_module(short proc);

static void app_loader_init(void)
{
    int i;
    for (i = 0; i < APP_NAPPS; i++) { gAppIface[i] = NULL; gAppTried[i] = 0; }

    gKApi.abi_version = UNO_ABI_VERSION;
#if UNO_COLOR
    gKApi.palette = kPalette;
    gKApi.black   = &kBlack;
#else
    gKApi.palette = NULL;
    gKApi.black   = NULL;
#endif
    gKApi.tbar_h  = TBAR_H;

    gKApi.uno_fill     = uno_fill;
    gKApi.uno_box      = uno_box;
    gKApi.uno_invert   = uno_invert;
    gKApi.text_at      = text_at;
    gKApi.text_at_max  = text_at_max;
    gKApi.fill_rgb     = fill_rgb;

    gKApi.fmt_u    = fmt_u;
    gKApi.put2     = put2;
    gKApi.now_secs = now_secs;

    gKApi.draw_window     = draw_window;
    gKApi.find_app_window = find_app_window;
    gKApi.launch_app      = launch_app;
    gKApi.repaint_all     = repaint_all;
    gKApi.topmost_proc    = kapi_topmost_proc;

    gKApi.fat12_mount = fat12_mount;
    gKApi.fat12_list  = fat12_list;
    gKApi.fat12_read  = fat12_read;
    gKApi.fat12_write = fat12_write;
    gKApi.fat_count   = &gFatCount;
    gKApi.fat_name    = gFatNames;
    gKApi.fat_sizes   = gFatSizes;

#ifdef UNO_PC64
    gKApi.display_res_count = uno_pc64_res_count;
    gKApi.display_res_get   = uno_pc64_res_get;
    gKApi.display_res_set   = uno_pc64_res_set;
#else
    gKApi.display_res_count = 0;
    gKApi.display_res_get   = 0;
    gKApi.display_res_set   = 0;
#endif

    gKApi.music_open_chan = music_open_chan;
    gKApi.music_note_on   = music_note_on;
    gKApi.music_quiet     = music_quiet;
    gKApi.music_start     = music_start;
    gKApi.music_stop      = music_stop;
    gKApi.gm_start        = gm_start;
    gKApi.gm_stop         = gm_stop;
}

/* resolve (loading on demand) the AppInterface for an app id */
static const AppInterface *app_iface(short proc)
{
    if (proc < 0 || proc >= APP_NAPPS) return NULL;
    if (!gAppTried[proc]) {
        UnoAppEntry entry = uno_load_module(proc);
        gAppTried[proc] = 1;
        if (entry) {
            const AppInterface *ai = entry(&gKApi);
            if (ai) gAppIface[proc] = ai;
        }
    }
    return gAppIface[proc];
}

/* ---- the dispatch the WM calls, now pointer-based (no switch on proc) ---- */
static void draw_app_content(short proc, UnoWin *w)
{
    const AppInterface *ai = app_iface(proc);
    if (ai && ai->draw) ai->draw(w);
}

static Boolean app_key(short proc, char ch, short code, Boolean cmd)
{
    const AppInterface *ai = app_iface(proc);
    if (ai && ai->key) return ai->key(ch, code, cmd);
    return false;
}

static void app_click(short proc, UnoWin *w, Point p)
{
    const AppInterface *ai = app_iface(proc);
    if (ai && ai->click) ai->click(w, p);
}

static void app_opened(short proc)
{
    const AppInterface *ai = app_iface(proc);
    if (ai && ai->opened) ai->opened();
}

static void app_close(short proc)
{
    const AppInterface *ai = app_iface(proc);
    if (ai && ai->closed) ai->closed();
}

/* title + default bounds now come from the loaded module (was kWinTitles[]
   / kWinRect[]).  Falls back gracefully if a module is missing. */
static const char *app_title(short proc)
{
    const AppInterface *ai = app_iface(proc);
    return (ai && ai->win_title) ? ai->win_title : "App";
}

static void app_default_rect(short proc, Rect *r)
{
    const AppInterface *ai = app_iface(proc);
    if (ai) SetRect(r, ai->win_rect[0], ai->win_rect[1],
                       ai->win_rect[2], ai->win_rect[3]);
    else    SetRect(r, 40, 50, 320, 200);
}
