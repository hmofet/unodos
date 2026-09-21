/* ===========================================================================
 * plat_x11.c - the desktop shell on Linux (and the BSDs), straight on Xlib.
 *
 *   window + blit   XCreateWindow and XPutImage of our framebuffer, converted
 *                   to the visual's pixel layout (B,G,R,X on every TrueColor
 *                   visual anyone runs).  On a Wayland session this runs
 *                   through XWayland, like every X11 app does.
 *   input           KeyPress through an input method (XIM, so dead keys and
 *                   compose work), ButtonPress/Release, MotionNotify
 *   services        _NET_WM_NAME, _NET_WM_STATE_FULLSCREEN, WM_CLASS (which
 *                   is how the desktop matches us to our .desktop entry and
 *                   its icon), /proc/self/exe
 *   file picker     the XDG desktop portal (org.freedesktop.portal.FileChooser
 *                   over D-Bus): the desktop's own dialog on GNOME, KDE and
 *                   everything else that ships a portal.  If there is no
 *                   portal, zenity or kdialog; if neither, the caller draws
 *                   its own.
 * ======================================================================== */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>
#ifdef UODESK_HAVE_DBUS
#include <dbus/dbus.h>
#endif

#include "uodesk_plat.h"

static Display *g_dpy;
static Window   g_win;
static GC       g_gc;
static Visual  *g_vis;
static int      g_depth;
static XImage  *g_img;
static int      g_iw, g_ih;
static XIM      g_xim;
static XIC      g_xic;
static Atom     A_WM_DELETE, A_NET_WM_STATE, A_NET_WM_STATE_FULLSCREEN,
                A_NET_WM_NAME, A_UTF8_STRING,
                A_CLIPBOARD, A_TARGETS, A_TEXT, A_INCR, A_UOSEL;
/* X has no clipboard STORE: whoever owns the CLIPBOARD selection answers
 * every paste itself, from this copy, for as long as it runs. */
static char    *g_clip;
static long     g_clip_n;
static void     clip_serve(XSelectionRequestEvent *r);
static int      g_full;
static int      g_w, g_h;
static int      g_rs, g_gs, g_bs;     /* the visual's channel shifts          */
static int      g_scale = 100;        /* the desktop's scale, percent         */
static char     g_base[4096];

static plat_event g_q[256];
static int      g_qh, g_qt;

static void push(const plat_event *e)
{
    int n = (g_qt + 1) % 256;
    if (n != g_qh) { g_q[g_qt] = *e; g_qt = n; }
}

static int shift_of(unsigned long mask)
{ int s = 0; if (!mask) return 0; while (!(mask & 1)) { mask >>= 1; s++; } return s; }

static int mods_of(unsigned state)
{
    int m = 0;
    if (state & ShiftMask)   m |= PM_SHIFT;
    if (state & ControlMask) m |= PM_CTRL;
    if (state & Mod1Mask)    m |= PM_ALT;
    if (state & Mod4Mask)    m |= PM_GUI;
    return m;
}

static int keysym_to_key(KeySym k)
{
    switch (k) {
    case XK_Up: case XK_KP_Up: return PK_UP;
    case XK_Down: case XK_KP_Down: return PK_DOWN;
    case XK_Right: case XK_KP_Right: return PK_RIGHT;
    case XK_Left: case XK_KP_Left: return PK_LEFT;
    case XK_Home: case XK_KP_Home: return PK_HOME;
    case XK_End: case XK_KP_End: return PK_END;
    case XK_Insert: case XK_KP_Insert: return PK_INSERT;
    case XK_Delete: case XK_KP_Delete: return PK_DELETE;
    case XK_Page_Up: case XK_KP_Page_Up: return PK_PGUP;
    case XK_Page_Down: case XK_KP_Page_Down: return PK_PGDN;
    case XK_Escape: return PK_ESC;
    case XK_Return: case XK_KP_Enter: return PK_ENTER;
    case XK_BackSpace: return PK_BACKSPACE;
    case XK_Tab: case XK_ISO_Left_Tab: return PK_TAB;
    default: break;
    }
    if (k >= XK_F1 && k <= XK_F12) return PK_F1 + (int)(k - XK_F1);
    return PK_NONE;
}

/* X11 has no per-window scale.  The desktop publishes its choice as
 * GDK_SCALE (an integer, GNOME's) or as Xft.dpi in the resource database
 * (what GNOME's fractional scaling, KDE and xrandr --dpi set); 96 dpi is
 * 100%.  Rounded to a quarter, which is the steps the UI is drawn at. */
static int desktop_scale(void)
{
    const char *g = getenv("GDK_SCALE"), *rm;
    int pct = 100;
    if (g && atoi(g) >= 1) pct = atoi(g) * 100;
    else if ((rm = XResourceManagerString(g_dpy)) != 0) {
        const char *p = strstr(rm, "Xft.dpi:");
        if (p) {
            double dpi = atof(p + 8);
            if (dpi > 0) pct = (int)(dpi * 100.0 / 96.0 + 0.5);
        }
    }
    pct = (pct + 12) / 25 * 25;
    if (pct < 100) pct = 100;
    if (pct > 300) pct = 300;
    return pct;
}

int  plat_scale(void) { return g_scale; }
void plat_size(int *w, int *h) { *w = g_w; *h = g_h; }

int plat_init(const char *title, int w, int h, int hidden)
{
    XSetWindowAttributes wa;
    XClassHint ch;
    int scr;
    char res_name[64];
    int i;

    setlocale(LC_CTYPE, "");            /* the input method needs the locale */
    XSetLocaleModifiers("");
    g_dpy = XOpenDisplay(0);
    if (!g_dpy) { fprintf(stderr, "cannot open the X display (is DISPLAY set?)\n"); return 0; }
    scr = DefaultScreen(g_dpy);
    g_vis = DefaultVisual(g_dpy, scr);
    g_depth = DefaultDepth(g_dpy, scr);
    if (g_vis->class != TrueColor || g_depth < 24) {
        fprintf(stderr, "need a 24-bit TrueColor X visual\n"); return 0;
    }
    /* w x h are points: the window is made in pixels */
    g_scale = hidden ? 100 : desktop_scale();
    w = w * g_scale / 100; h = h * g_scale / 100;
    g_rs = shift_of(g_vis->red_mask);
    g_gs = shift_of(g_vis->green_mask);
    g_bs = shift_of(g_vis->blue_mask);

    memset(&wa, 0, sizeof wa);
    wa.background_pixmap = None;        /* every pixel is ours: no flicker */
    wa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                    ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
                    FocusChangeMask;
    g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, scr), 0, 0,
                          (unsigned)w, (unsigned)h, 0, g_depth, InputOutput, g_vis,
                          CWBackPixmap | CWEventMask, &wa);
    g_w = w; g_h = h;
    g_gc = XCreateGC(g_dpy, g_win, 0, 0);

    A_WM_DELETE = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    A_NET_WM_STATE = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    A_NET_WM_STATE_FULLSCREEN = XInternAtom(g_dpy, "_NET_WM_STATE_FULLSCREEN", False);
    A_NET_WM_NAME = XInternAtom(g_dpy, "_NET_WM_NAME", False);
    A_UTF8_STRING = XInternAtom(g_dpy, "UTF8_STRING", False);
    A_CLIPBOARD = XInternAtom(g_dpy, "CLIPBOARD", False);
    A_TARGETS = XInternAtom(g_dpy, "TARGETS", False);
    A_TEXT = XInternAtom(g_dpy, "TEXT", False);
    A_INCR = XInternAtom(g_dpy, "INCR", False);
    A_UOSEL = XInternAtom(g_dpy, "UODESK_CLIPBOARD", False);
    XSetWMProtocols(g_dpy, g_win, &A_WM_DELETE, 1);

    /* WM_CLASS "unoword", "UnoWord": matches StartupWMClass in our .desktop */
    for (i = 0; title[i] && i < 63; i++)
        res_name[i] = (char)(title[i] >= 'A' && title[i] <= 'Z' ? title[i] + 32 : title[i]);
    res_name[i] = 0;
    ch.res_name = res_name;
    ch.res_class = (char *)title;
    XSetClassHint(g_dpy, g_win, &ch);
    plat_set_title(title);

    g_xim = XOpenIM(g_dpy, 0, 0, 0);
    if (g_xim)
        g_xic = XCreateIC(g_xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                          XNClientWindow, g_win, XNFocusWindow, g_win, (char *)0);

    if (!hidden) XMapWindow(g_dpy, g_win);
    XFlush(g_dpy);
    return 1;
}

void plat_shutdown(void)
{
    if (!g_dpy) return;
    if (g_img) { XDestroyImage(g_img); g_img = 0; }
    if (g_xic) XDestroyIC(g_xic);
    if (g_xim) XCloseIM(g_xim);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
    g_dpy = 0;
}

static void repaint(void)
{
    if (g_img) {
        XPutImage(g_dpy, g_win, g_gc, g_img, 0, 0, 0, 0, (unsigned)g_iw, (unsigned)g_ih);
        XFlush(g_dpy);
    }
}

static void handle(XEvent *x)
{
    plat_event e;
    memset(&e, 0, sizeof e);
    switch (x->type) {
    case ClientMessage:
        if ((Atom)x->xclient.data.l[0] == A_WM_DELETE) { e.type = PE_QUIT; push(&e); }
        break;
    case ConfigureNotify:
        if (x->xconfigure.width != g_w || x->xconfigure.height != g_h) {
            g_w = x->xconfigure.width; g_h = x->xconfigure.height;
            e.type = PE_RESIZE; e.w = g_w; e.h = g_h; push(&e);
        }
        break;
    case Expose:
        if (x->xexpose.count == 0) { repaint(); e.type = PE_EXPOSE; push(&e); }
        break;
    case FocusIn:  if (g_xic) XSetICFocus(g_xic);   break;
    case FocusOut: if (g_xic) XUnsetICFocus(g_xic); break;
    case SelectionRequest: clip_serve(&x->xselectionrequest); break;
    case SelectionClear:                    /* someone else copied: not ours */
        if (x->xselectionclear.selection == A_CLIPBOARD) {
            free(g_clip); g_clip = 0; g_clip_n = 0;
        }
        break;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&x->xkey, 0);
        int k = keysym_to_key(ks), m = mods_of(x->xkey.state);
        char buf[16];
        int n;
        Status st = 0;
        if (k != PK_NONE) { e.type = PE_KEY; e.key = k; e.mods = m; push(&e); break; }
        if ((m & PM_CTRL) && !(m & PM_ALT)) {        /* an accelerator */
            if (ks >= XK_A && ks <= XK_Z) ks += XK_a - XK_A;
            if (ks >= 32 && ks < 127) {
                e.type = PE_KEY; e.key = PK_CHAR; e.ch = (int)ks; e.mods = m; push(&e);
            }
            break;
        }
        if (g_xic) n = Xutf8LookupString(g_xic, &x->xkey, buf, (int)sizeof buf - 1, &ks, &st);
        else       n = XLookupString(&x->xkey, buf, (int)sizeof buf - 1, &ks, 0);
        if (n > 0 && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
            buf[n] = 0;
            memcpy(e.text, buf, (size_t)n + 1);
            e.type = PE_TEXT; e.mods = m; push(&e);
        }
        break;
    }
    case MotionNotify:
        e.type = PE_MOUSE_MOVE; e.x = x->xmotion.x; e.y = x->xmotion.y;
        e.mods = mods_of(x->xmotion.state); push(&e);
        break;
    case ButtonPress: case ButtonRelease: {
        unsigned b = x->xbutton.button;
        if (b == 4 || b == 5) {                 /* the wheel: presses only */
            if (x->type == ButtonPress) {
                e.type = PE_WHEEL; e.wheel = b == 4 ? -1 : 1; push(&e);
            }
            break;
        }
        if (b > 3) break;                       /* 6/7: horizontal, 8/9: back/fwd */
        e.type = x->type == ButtonPress ? PE_MOUSE_DOWN : PE_MOUSE_UP;
        e.x = x->xbutton.x; e.y = x->xbutton.y;
        e.button = b == 1 ? 0 : b == 3 ? 1 : 2;
        e.mods = mods_of(x->xbutton.state);
        push(&e);
        break;
    }
    default: break;
    }
}

int plat_wait(plat_event *e, int ms)
{
    if (g_qh == g_qt && ms > 0 && !XPending(g_dpy)) {
        fd_set fds;
        struct timeval tv;
        int fd = ConnectionNumber(g_dpy);
        FD_ZERO(&fds); FD_SET(fd, &fds);
        tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
        select(fd + 1, &fds, 0, 0, &tv);
    }
    while (g_qh == g_qt && XPending(g_dpy)) {
        XEvent x;
        XNextEvent(g_dpy, &x);
        if (XFilterEvent(&x, None)) continue;  /* the input method took it */
        handle(&x);
    }
    if (g_qh == g_qt) return 0;
    *e = g_q[g_qh];
    g_qh = (g_qh + 1) % 256;
    return 1;
}

void plat_present(const fb_px *px, int w, int h)
{
    int i;
    if (!g_img || g_iw != w || g_ih != h) {
        char *data;
        if (g_img) XDestroyImage(g_img);      /* frees the data too */
        g_img = 0;
        data = (char *)malloc((size_t)w * (size_t)h * 4);
        if (!data) return;
        g_img = XCreateImage(g_dpy, g_vis, (unsigned)g_depth, ZPixmap, 0, data,
                             (unsigned)w, (unsigned)h, 32, 0);
        if (!g_img) { free(data); return; }
        g_iw = w; g_ih = h;
    }
    if (g_rs == 16 && g_gs == 8 && g_bs == 0 && g_img->byte_order == LSBFirst)
        plat_rgba_to_bgrx((unsigned char *)g_img->data, px, w * h);   /* the norm */
    else
        for (i = 0; i < w * h; i++) {
            fb_px p = px[i];
            unsigned long v = ((unsigned long)(p & 0xFF) << g_rs) |
                              ((unsigned long)((p >> 8) & 0xFF) << g_gs) |
                              ((unsigned long)((p >> 16) & 0xFF) << g_bs);
            XPutPixel(g_img, i % w, i / w, v);
        }
    repaint();
}

unsigned plat_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000));
}

const char *plat_base_path(void)
{
    ssize_t n = readlink("/proc/self/exe", g_base, sizeof g_base - 1);
    if (n <= 0) return 0;
    g_base[n] = 0;
    { char *s = strrchr(g_base, '/'); if (!s) return 0; s[1] = 0; }
    return g_base;
}

void plat_set_title(const char *utf8)
{
    XStoreName(g_dpy, g_win, utf8);                               /* legacy WMs */
    XChangeProperty(g_dpy, g_win, A_NET_WM_NAME, A_UTF8_STRING, 8, PropModeReplace,
                    (const unsigned char *)utf8, (int)strlen(utf8));
    XFlush(g_dpy);
}

void plat_set_fullscreen(int on)
{
    XEvent x;
    if (!!on == g_full) return;
    memset(&x, 0, sizeof x);
    x.type = ClientMessage;
    x.xclient.window = g_win;
    x.xclient.message_type = A_NET_WM_STATE;
    x.xclient.format = 32;
    x.xclient.data.l[0] = on ? 1 : 0;          /* _NET_WM_STATE_ADD / REMOVE */
    x.xclient.data.l[1] = (long)A_NET_WM_STATE_FULLSCREEN;
    x.xclient.data.l[3] = 1;                   /* from a normal application  */
    XSendEvent(g_dpy, DefaultRootWindow(g_dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &x);
    XFlush(g_dpy);
    g_full = !!on;
}
int plat_is_fullscreen(void) { return g_full; }

/* ---- file picker ------------------------------------------------------------ */

/* "file:///home/a%20b/x.doc" -> "/home/a b/x.doc" */
static int uri_to_path(const char *uri, char *path, int cap)
{
    int k = 0;
    if (strncmp(uri, "file://", 7)) return 0;
    uri += 7;
    while (*uri && k < cap - 1) {
        if (uri[0] == '%' && uri[1] && uri[2]) {
            char hx[3] = { uri[1], uri[2], 0 };
            path[k++] = (char)strtol(hx, 0, 16);
            uri += 3;
        } else path[k++] = *uri++;
    }
    path[k] = 0;
    return k > 0;
}

/* While a dialog is up, keep our window painted and drop its input. */
static void idle_x(void)
{
    while (XPending(g_dpy)) {
        XEvent x;
        XNextEvent(g_dpy, &x);
        if (x.type == Expose && x.xexpose.count == 0) repaint();
        else if (x.type == ConfigureNotify) handle(&x);   /* keep the size */
    }
}

#ifdef UODESK_HAVE_DBUS
static void add_sv_string(DBusMessageIter *dict, const char *key, const char *val)
{
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, 0, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

/* 1 ok, 0 cancel, -1 no portal */
static int portal_pick(int save, const char *const *types, int ntypes,
                       char *path, int cap, int *type)
{
    DBusError err;
    DBusConnection *c;
    DBusMessage *m, *r;
    DBusMessageIter it, dict, e, v, arr, st, pats, pt;
    char handle[256], token[64], parent[64], sender[128], match[512];
    const char *title = save ? "Save As" : "Open";
    const char *pp = parent, *pt_ = token;
    const char *uname;
    int result = -1, t, i;
    unsigned counter = 0;

    dbus_error_init(&err);
    c = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!c) { dbus_error_free(&err); return -1; }

    uname = dbus_bus_get_unique_name(c);             /* ":1.42" -> "1_42" */
    for (i = 0; uname && uname[i + 1] && i < (int)sizeof sender - 1; i++)
        sender[i] = uname[i + 1] == '.' ? '_' : uname[i + 1];
    sender[i] = 0;
    snprintf(token, sizeof token, "uoffice%d_%u", (int)getpid(), ++counter);
    snprintf(handle, sizeof handle, "/org/freedesktop/portal/desktop/request/%s/%s",
             sender, token);
    snprintf(match, sizeof match,
             "type='signal',interface='org.freedesktop.portal.Request',"
             "member='Response',path='%s'", handle);
    dbus_bus_add_match(c, match, 0);                 /* BEFORE the call: no race */
    snprintf(parent, sizeof parent, "x11:%lx", (unsigned long)g_win);

    m = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                     "/org/freedesktop/portal/desktop",
                                     "org.freedesktop.portal.FileChooser",
                                     save ? "SaveFile" : "OpenFile");
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &pp);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &title);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    add_sv_string(&dict, "handle_token", pt_);
    { /* modal: b true */
        const char *k = "modal";
        dbus_bool_t yes = 1;
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, 0, &e);
        dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &yes);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    }
    if (ntypes) { /* filters: a(sa(us)) - one per uofile type, glob patterns */
        const char *k = "filters";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, 0, &e);
        dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
        dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "a(sa(us))", &v);
        dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "(sa(us))", &arr);
        for (t = 0; t < ntypes; t++) {
            char pat[8][16];
            int n = plat_type_patterns(types[t], pat, 8);
            const char *name = types[t];
            dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, 0, &st);
            dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &name);
            dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "(us)", &pats);
            for (i = 0; i < n; i++) {
                dbus_uint32_t glob = 0;
                const char *g = pat[i];
                dbus_message_iter_open_container(&pats, DBUS_TYPE_STRUCT, 0, &pt);
                dbus_message_iter_append_basic(&pt, DBUS_TYPE_UINT32, &glob);
                dbus_message_iter_append_basic(&pt, DBUS_TYPE_STRING, &g);
                dbus_message_iter_close_container(&pats, &pt);
            }
            dbus_message_iter_close_container(&st, &pats);
            dbus_message_iter_close_container(&arr, &st);
        }
        dbus_message_iter_close_container(&v, &arr);
        dbus_message_iter_close_container(&e, &v);
        dbus_message_iter_close_container(&dict, &e);
    }
    dbus_message_iter_close_container(&it, &dict);

    r = dbus_connection_send_with_reply_and_block(c, m, 5000, &err);
    dbus_message_unref(m);
    if (!r) { dbus_error_free(&err); goto out; }     /* no portal on this bus */
    {   /* an old portal may answer with a different request path */
        const char *got = 0;
        if (dbus_message_get_args(r, 0, DBUS_TYPE_OBJECT_PATH, &got, DBUS_TYPE_INVALID) &&
            got && strcmp(got, handle)) {
            dbus_bus_remove_match(c, match, 0);
            snprintf(handle, sizeof handle, "%s", got);
            snprintf(match, sizeof match,
                     "type='signal',interface='org.freedesktop.portal.Request',"
                     "member='Response',path='%s'", handle);
            dbus_bus_add_match(c, match, 0);
        }
    }
    dbus_message_unref(r);

    /* wait for Request.Response, keeping our own window painted meanwhile */
    for (;;) {
        DBusMessage *s;
        if (!dbus_connection_read_write(c, 50)) break;
        idle_x();
        while ((s = dbus_connection_pop_message(c)) != 0) {
            if (dbus_message_is_signal(s, "org.freedesktop.portal.Request", "Response") &&
                !strcmp(dbus_message_get_path(s), handle)) {
                dbus_uint32_t code = 2;
                DBusMessageIter a, d;
                dbus_message_iter_init(s, &a);
                if (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_UINT32)
                    dbus_message_iter_get_basic(&a, &code);
                result = code == 0 ? 0 : (code == 1 ? 0 : -1);
                if (code == 0 && dbus_message_iter_next(&a) &&
                    dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&a, &d);
                    while (dbus_message_iter_get_arg_type(&d) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter kv, var;
                        const char *key = 0;
                        dbus_message_iter_recurse(&d, &kv);
                        dbus_message_iter_get_basic(&kv, &key);
                        dbus_message_iter_next(&kv);
                        dbus_message_iter_recurse(&kv, &var);
                        if (key && !strcmp(key, "uris") &&
                            dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
                            DBusMessageIter u;
                            const char *uri = 0;
                            dbus_message_iter_recurse(&var, &u);
                            if (dbus_message_iter_get_arg_type(&u) == DBUS_TYPE_STRING) {
                                dbus_message_iter_get_basic(&u, &uri);
                                if (uri && uri_to_path(uri, path, cap)) result = 1;
                            }
                        } else if (key && !strcmp(key, "current_filter") && type &&
                                   dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRUCT) {
                            DBusMessageIter f;
                            const char *fname = 0;
                            dbus_message_iter_recurse(&var, &f);
                            dbus_message_iter_get_basic(&f, &fname);
                            for (t = 0; fname && t < ntypes; t++)
                                if (!strcmp(fname, types[t])) *type = t;
                        }
                        dbus_message_iter_next(&d);
                    }
                }
                dbus_message_unref(s);
                goto done;
            }
            dbus_message_unref(s);
        }
    }
done:
    dbus_bus_remove_match(c, match, 0);
out:
    dbus_connection_unref(c);
    return result;
}
#endif

static int on_path(const char *prog)
{
    const char *p = getenv("PATH");
    char buf[4096];
    while (p && *p) {
        const char *q = strchr(p, ':');
        int n = q ? (int)(q - p) : (int)strlen(p);
        if (n > 0 && n < 3000) {
            snprintf(buf, sizeof buf, "%.*s/%s", n, p, prog);
            if (access(buf, X_OK) == 0) return 1;
        }
        p = q ? q + 1 : 0;
    }
    return 0;
}

/* zenity (GTK) or kdialog (KDE), for desktops without a portal */
static int helper_pick(int save, const char *const *types, int ntypes,
                       char *path, int cap)
{
    char cmd[4096], filt[1024];
    int k = 0, t, i, status;
    FILE *f;
    int zen = on_path("zenity"), kde = !zen && on_path("kdialog");
    if (!zen && !kde) return -1;
    filt[0] = 0;
    for (t = 0; t < ntypes && k < 800; t++) {
        char pat[8][16], name[128];
        int n = plat_type_patterns(types[t], pat, 8), j;
        for (j = 0; types[t][j] && j < 127; j++)          /* no quotes, ever */
            name[j] = (types[t][j] == '\'' || types[t][j] == '|') ? ' ' : types[t][j];
        name[j] = 0;
        if (zen) {
            k += snprintf(filt + k, sizeof filt - (size_t)k, " --file-filter='%s |", name);
            for (i = 0; i < n; i++) k += snprintf(filt + k, sizeof filt - (size_t)k, " %s", pat[i]);
            k += snprintf(filt + k, sizeof filt - (size_t)k, "'");
        } else {
            if (t) k += snprintf(filt + k, sizeof filt - (size_t)k, "\n");
            for (i = 0; i < n; i++) k += snprintf(filt + k, sizeof filt - (size_t)k, "%s%s", i ? " " : "", pat[i]);
            k += snprintf(filt + k, sizeof filt - (size_t)k, "|%s", name);
        }
    }
    if (zen)
        snprintf(cmd, sizeof cmd, "zenity --file-selection --title='%s'%s%s 2>/dev/null",
                 save ? "Save As" : "Open", save ? " --save --confirm-overwrite" : "", filt);
    else
        snprintf(cmd, sizeof cmd, "kdialog --%s . '%s' 2>/dev/null",
                 save ? "getsavefilename" : "getopenfilename", filt);
    f = popen(cmd, "r");
    if (!f) return -1;
    /* blocks until the helper exits; our window repaints once it has */
    if (!fgets(path, cap, f)) path[0] = 0;
    status = pclose(f);
    idle_x();
    { int n = (int)strlen(path); while (n && (path[n - 1] == '\n' || path[n - 1] == '\r')) path[--n] = 0; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !path[0]) return 0;
    return 1;
}

int plat_file_dialog(int save, const char *const *types, int ntypes,
                     char *path, int cap, int *type)
{
    int r = -1;
    if (type) *type = 0;
#ifdef UODESK_HAVE_DBUS
    r = portal_pick(save, types, ntypes, path, cap, type);
    if (r >= 0) {
        if (r > 0 && type && !*type) *type = plat_type_of_path(path, types, ntypes);
        return r;
    }
#endif
    r = helper_pick(save, types, ntypes, path, cap);
    if (r > 0 && type) *type = plat_type_of_path(path, types, ntypes);
    return r;
}

void plat_args(int *argc, char ***argv) { (void)argc; (void)argv; }   /* UTF-8 already */

/* ---- the clipboard: the CLIPBOARD selection, as UTF8_STRING ---------------------
 * Serving: answer TARGETS and the text types from g_clip.  Pasting: our own
 * copy when we own it, otherwise XConvertSelection and wait (bounded) for
 * the owner's SelectionNotify, handling every other event meanwhile so
 * nothing is lost.  INCR (a paste of more than ~256 KB) is not implemented:
 * such a paste comes back empty rather than truncated. */
static void clip_serve(XSelectionRequestEvent *r)
{
    XEvent reply;
    Atom prop = r->property ? r->property : r->target;   /* obsolete clients */
    memset(&reply, 0, sizeof reply);
    reply.xselection.type = SelectionNotify;
    reply.xselection.requestor = r->requestor;
    reply.xselection.selection = r->selection;
    reply.xselection.target = r->target;
    reply.xselection.time = r->time;
    reply.xselection.property = None;                   /* refused, unless... */
    if (r->selection == A_CLIPBOARD && g_clip) {
        if (r->target == A_TARGETS) {
            Atom t[4];
            t[0] = A_TARGETS; t[1] = A_UTF8_STRING; t[2] = XA_STRING; t[3] = A_TEXT;
            XChangeProperty(g_dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace,
                            (const unsigned char *)t, 4);
            reply.xselection.property = prop;
        } else if (r->target == A_UTF8_STRING || r->target == XA_STRING ||
                   r->target == A_TEXT) {
            XChangeProperty(g_dpy, r->requestor, prop,
                            r->target == A_TEXT ? A_UTF8_STRING : r->target, 8,
                            PropModeReplace, (const unsigned char *)g_clip, (int)g_clip_n);
            reply.xselection.property = prop;
        }
    }
    XSendEvent(g_dpy, r->requestor, False, 0, &reply);
    XFlush(g_dpy);
}

int plat_clip_set(const char *utf8)
{
    long n = (long)strlen(utf8 ? utf8 : "");
    char *c = (char *)malloc((size_t)n + 1);
    if (!c) return 0;
    memcpy(c, utf8 ? utf8 : "", (size_t)n + 1);
    free(g_clip);
    g_clip = c; g_clip_n = n;
    XSetSelectionOwner(g_dpy, A_CLIPBOARD, g_win, CurrentTime);
    XFlush(g_dpy);
    return XGetSelectionOwner(g_dpy, A_CLIPBOARD) == g_win;
}

static long copy_out(const char *s, long n, char *buf, long cap)
{
    if (cap > 0) {
        long m = n < cap - 1 ? n : cap - 1;
        memcpy(buf, s, (size_t)m);
        buf[m] = 0;
    }
    return n;
}

long plat_clip_get(char *buf, long cap)
{
    unsigned start;
    int got = 0;
    long n = 0;
    Window owner;
    if (cap > 0) buf[0] = 0;
    owner = XGetSelectionOwner(g_dpy, A_CLIPBOARD);
    if (owner == None) return 0;
    if (owner == g_win) return g_clip ? copy_out(g_clip, g_clip_n, buf, cap) : 0;

    XDeleteProperty(g_dpy, g_win, A_UOSEL);
    XConvertSelection(g_dpy, A_CLIPBOARD, A_UTF8_STRING, A_UOSEL, g_win, CurrentTime);
    XFlush(g_dpy);
    start = plat_ticks();
    while (!got && plat_ticks() - start < 1000) {
        XEvent x;
        if (!XPending(g_dpy)) {
            fd_set fds;
            struct timeval tv = { 0, 20000 };
            int fd = ConnectionNumber(g_dpy);
            FD_ZERO(&fds); FD_SET(fd, &fds);
            select(fd + 1, &fds, 0, 0, &tv);
            continue;
        }
        XNextEvent(g_dpy, &x);
        if (x.type == SelectionNotify && x.xselection.requestor == g_win &&
            x.xselection.selection == A_CLIPBOARD) {
            got = 1;
            if (x.xselection.property != None) {
                Atom type;
                int fmt;
                unsigned long items, left;
                unsigned char *data = 0;
                if (XGetWindowProperty(g_dpy, g_win, A_UOSEL, 0, 0x7FFFFFF, True,
                                       AnyPropertyType, &type, &fmt, &items, &left,
                                       &data) == Success && data) {
                    if (type != A_INCR && fmt == 8) n = copy_out((const char *)data, (long)items, buf, cap);
                    XFree(data);
                }
            }
        } else if (!XFilterEvent(&x, None)) handle(&x);   /* keep everything else */
    }
    return n;
}
void plat_set_modified(int on) { (void)on; }
