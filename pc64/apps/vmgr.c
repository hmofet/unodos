/* Appliances - the unovirt manager, as a unoui-class module (APPS\VMGR.UNO).
 *
 * Contract: pc64/UNOVIRT.md and unovirt_mgr.h.  Two views in one window,
 * because they are two halves of one job: a LIST of the appliances this
 * machine has, and the CONSOLE of the one that is running.  Tab switches.
 *
 * The console view is the point of the whole programme so far - it is where a
 * guest stops being a line in a boot log and becomes something a person can
 * type at.  Keys go into the UART's receive FIFO, which is exactly where a
 * real keystroke arrives, so the guest's own driver wakes on IRQ4 and its
 * shell reads a byte with nothing else involved.
 */
#include "uno_uuiapp.h"
#include "uno_appdesc.h"
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "pc64_font.h"
#include "../unovirt_mgr.h"
#include "../unovirt.h"
#include <string.h>

void pc64_shell_dirty(void);
const struct unoui_theme *pc64_shell_theme(void);
static const struct unoui_theme *TH(void) { return pc64_shell_theme(); }

#define ROW_H 16

enum { V_LIST, V_CONSOLE };
static int  g_view = V_LIST;
static int  g_sel;                  /* selected appliance                   */
static int  g_edit = -1;            /* field being edited, -1 = none        */
static int  g_top;                  /* console scroll                       */
static int  g_follow = 1;
static unsigned g_seen;
static char g_msg[80];

/* The definition being edited.  A copy, so an abandoned edit changes nothing
 * - the registry is only written when the user commits. */
static uno_vm_def g_buf;
enum { F_NAME, F_KERNEL, F_INITRD, F_DISK, F_N };
static const char *kFieldName[F_N] = { "Name", "Kernel", "Initrd", "Disk" };

enum { B_NEW, B_DEL, B_START, B_STOP, B_VIEW, B_N };
static unoui_rect g_btn[B_N];
static unoui_rect g_rows_r[UNO_VM_MAX];

static int in_rect(unoui_rect r, int x, int y)
{ return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static char *fieldp(uno_vm_def *d, int f)
{
    switch (f) {
    case F_NAME:   return d->name;
    case F_KERNEL: return d->kernel;
    case F_INITRD: return d->initrd;
    default:       return d->disk;
    }
}
static int fieldcap(int f) { return f == F_NAME ? UNO_VM_NAME : UNO_VM_PATH; }

static void say(const char *s)
{
    int i;
    for (i = 0; i + 1 < (int)sizeof g_msg && s[i]; i++) g_msg[i] = s[i];
    g_msg[i] = 0;
}

static void btn_draw(unoui_rect r, const char *label, int on)
{
    const unoui_palette *p = &TH()->pal;
    fb_fill_rect(r.x, r.y, r.w, r.h, on ? p->accent : p->face);
    fb_frame_rect(r.x, r.y, r.w, r.h, p->dark);
    fb_text(r.x + 8, r.y + (r.h - uno_font_height_px(0, 12)) / 2 + 1,
            label, on ? p->accent_text : p->face_text, -1);
}

/* ---- the list ------------------------------------------------------------ */

static void draw_list(unoui_rect c)
{
    const unoui_palette *p = &TH()->pal;
    int n = uno_vm_count(), i, y = c.y + 4;
    int run = uno_vm_running();

    fb_text(c.x + 6, y, "Appliances on this machine", p->text, -1);
    y += ROW_H + 2;

    if (!n) {
        fb_text(c.x + 10, y, "None yet - New makes one.", p->text_dim, -1);
        fb_text(c.x + 10, y + ROW_H,
                "A new appliance boots whatever is already staged in", p->text_dim, -1);
        fb_text(c.x + 10, y + 2 * ROW_H,
                "EFI\\UNODOS\\VM until you point it somewhere else.", p->text_dim, -1);
        y += 3 * ROW_H;
    }
    for (i = 0; i < n && i < UNO_VM_MAX; i++) {
        const uno_vm_def *d = uno_vm_get(i);
        unoui_rect r;
        r.x = c.x + 4; r.y = y; r.w = c.w - 8; r.h = ROW_H;
        g_rows_r[i] = r;
        if (i == g_sel) fb_fill_rect(r.x, r.y, r.w, r.h, p->accent);
        fb_text(r.x + 6, r.y + 1, d->name,
                i == g_sel ? p->accent_text : p->text, -1);
        fb_text(r.x + 130, r.y + 1, i == run ? "running" : "stopped",
                i == g_sel ? p->accent_text : p->text_dim, -1);
        fb_text(r.x + 210, r.y + 1, d->kernel[0] ? d->kernel : "(staged default)",
                i == g_sel ? p->accent_text : p->text_dim, -1);
        y += ROW_H;
    }

    /* the editor, always visible for the selection: a separate dialog for
     * four fields is a click nobody needs */
    y += 6;
    fb_hline(c.x + 4, c.x + c.w - 4, y, p->dark);
    y += 6;
    if (n) {
        const uno_vm_def *d = (g_edit >= 0) ? &g_buf : uno_vm_get(g_sel);
        int f;
        for (f = 0; f < F_N; f++) {
            char *v = fieldp((uno_vm_def *)d, f);
            fb_text(c.x + 8, y, kFieldName[f], p->text_dim, -1);
            fb_fill_rect(c.x + 74, y - 2, c.w - 90, ROW_H,
                         g_edit == f ? p->field_bg : p->face);
            fb_frame_rect(c.x + 74, y - 2, c.w - 90, ROW_H, p->dark);
            fb_text(c.x + 78, y, v[0] ? v : "(default)",
                    v[0] ? p->text : p->text_dim, -1);
            if (g_edit == f) {
                int tw = uno_font_text_w_styled(v, 0, 12, 0);
                fb_fill_rect(c.x + 78 + tw + 1, y, 1, ROW_H - 4, p->text);
            }
            y += ROW_H + 3;
        }
        fb_text(c.x + 8, y, g_edit >= 0
                ? "Enter commits, Esc abandons"
                : "Click a field to edit it", p->text_dim, -1);
    }
}

/* ---- the console ---------------------------------------------------------- */

static void draw_console(unoui_rect c)
{
    const unoui_palette *p = &TH()->pal;
    int fh = uno_font_height_px(1, 12);
    int rows = (c.h - 4) / (fh + 1), i;
    int n = uno_vm_con_lines();
    int top;

    if (rows < 1) rows = 1;
    top = g_follow ? (n > rows ? n - rows : 0) : g_top;
    if (top > n - 1) top = n > 0 ? n - 1 : 0;
    if (top < 0) top = 0;

    fb_fill_rect(c.x, c.y, c.w, c.h, p->field_bg);
    for (i = 0; i < rows && top + i < n; i++)
        fb_text(c.x + 4, c.y + 2 + i * (fh + 1), uno_vm_con_line(top + i),
                p->text, -1);
    if (!n)
        fb_text(c.x + 6, c.y + 4,
                uno_vm_running() >= 0 ? "waiting for the guest to say something..."
                                      : "no appliance is running", p->text_dim, -1);
}

static void vm_draw(unoui_widget *w, unoui_rect c, void *ctx)
{
    const unoui_palette *p = &TH()->pal;
    unoui_rect body = c;
    int bx = c.x + 6, by, i;
    static const char *kLabel[B_N] = { "New", "Delete", "Start", "Stop", "Console" };
    (void)w; (void)ctx;

    fb_fill_rect(c.x, c.y, c.w, c.h, p->face);

    by = c.y + c.h - 26;
    for (i = 0; i < B_N; i++) {
        g_btn[i].x = bx; g_btn[i].y = by; g_btn[i].w = 76; g_btn[i].h = 20;
        bx += 80;
    }
    body.h -= 52;

    if (g_view == V_LIST) draw_list(body); else draw_console(body);

    for (i = 0; i < B_N; i++)
        btn_draw(g_btn[i], i == B_VIEW
                 ? (g_view == V_LIST ? "Console" : "List") : kLabel[i],
                 i == B_VIEW && g_view == V_CONSOLE);

    /* One status line, always: the manager's own last word, then unovirt's.
     * Two sources because they fail differently - "no appliance selected" is
     * this app, "firmware disabled virtualization" is the machine. */
    fb_text(c.x + 6, c.y + c.h - 44, g_msg[0] ? g_msg : uno_vm_status(),
            p->text_dim, -1);
}

static int vm_event(unoui_widget *w, const unoui_event *e, void *ctx)
{
    int i, n = uno_vm_count();
    (void)w; (void)ctx;
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;

    for (i = 0; i < B_N; i++) {
        if (!in_rect(g_btn[i], e->x, e->y)) continue;
        switch (i) {
        case B_NEW: {
            uno_vm_def d;
            memset(&d, 0, sizeof d);
            d.mem_mb = 512;
            d.net = 1;
            if (uno_vm_add(&d) < 0) say("no room for another appliance");
            else { g_sel = uno_vm_count() - 1; say("created - name it, then Start"); }
            break;
        }
        case B_DEL:
            if (n && uno_vm_del(g_sel)) {
                if (g_sel >= uno_vm_count()) g_sel = uno_vm_count() - 1;
                if (g_sel < 0) g_sel = 0;
                g_edit = -1;
                say("deleted");
            }
            break;
        case B_START:
            if (!n) { say("nothing to start"); break; }
            if (uno_vm_start(g_sel)) { g_view = V_CONSOLE; g_follow = 1; say(""); }
            else say(uno_vm_status());
            break;
        case B_STOP: uno_vm_stop(); say(""); break;
        case B_VIEW: g_view = g_view == V_LIST ? V_CONSOLE : V_LIST; break;
        }
        pc64_shell_dirty();
        return 1;
    }

    if (g_view == V_LIST) {
        for (i = 0; i < n && i < UNO_VM_MAX; i++) {
            if (!in_rect(g_rows_r[i], e->x, e->y)) continue;
            g_sel = i; g_edit = -1;
            pc64_shell_dirty();
            return 1;
        }
        /* a click in the editor block picks a field - the rows are laid out
         * by draw_list, so the hit test uses the same arithmetic */
        if (n) {
            int f = -1, y0 = g_rows_r[0].y + n * ROW_H + 12;
            for (i = 0; i < F_N; i++)
                if (e->y >= y0 + i * (ROW_H + 3) - 2 &&
                    e->y <  y0 + i * (ROW_H + 3) + ROW_H - 2) f = i;
            if (f >= 0) {
                if (g_edit < 0) g_buf = *uno_vm_get(g_sel);
                g_edit = f;
                pc64_shell_dirty();
                return 1;
            }
        }
    }
    return 0;
}

static int vm_key(int uni, int scan, int ctrl)
{
    int n = uno_vm_count();
    (void)ctrl;

    /* EDITING SWALLOWS EVERYTHING, and so does the console: a view where the
     * user is typing must not also treat letters as accelerators, or naming
     * an appliance "s" stops it. */
    if (g_view == V_LIST && g_edit >= 0) {
        char *v = fieldp(&g_buf, g_edit);
        int len = (int)strlen(v);
        if (scan == 0x0E || uni == '\b') { if (len) v[len - 1] = 0; }
        else if (uni == '\n' || uni == '\r') {
            uno_vm_set(g_sel, &g_buf);
            g_edit = -1;
            say("saved");
        } else if (scan == 0x0F || uni == 27) { g_edit = -1; say(""); }
        else if (uni >= 32 && uni < 127 && len + 1 < fieldcap(g_edit)) {
            v[len] = (char)uni; v[len + 1] = 0;
        } else return 0;
        pc64_shell_dirty();
        return 1;
    }

    if (g_view == V_CONSOLE) {
        switch (scan) {
        case 0x09: g_follow = 0; g_top -= 10; if (g_top < 0) g_top = 0; break;
        case 0x0A: g_follow = 0; g_top += 10; break;
        case 0x06: g_follow = 1; break;                  /* End: follow again */
        default:
            if (uni == 27) { g_view = V_LIST; break; }   /* Esc leaves        */
            if (uni > 0) { uno_vm_con_key(uni); return 1; }
            return 0;
        }
        pc64_shell_dirty();
        return 1;
    }

    switch (scan) {
    case 0x01: if (g_sel > 0) g_sel--; break;
    case 0x02: if (g_sel + 1 < n) g_sel++; break;
    default:
        if (uni == '\n' || uni == '\r') {
            if (n && uno_vm_start(g_sel)) { g_view = V_CONSOLE; g_follow = 1; }
            else say(uno_vm_status());
        } else if (uni == 'c' || uni == 'C') g_view = V_CONSOLE;
        else return 0;
    }
    pc64_shell_dirty();
    return 1;
}

/* Repaint when the guest says something, and only while following - a reader
 * who scrolled back is reading, and repainting under them drags the text
 * away mid-sentence (the same rule the log viewer follows). */
static void vm_frame(void)
{
    unsigned s;
    if (g_view != V_CONSOLE || !g_follow) return;
    s = uno_vm_con_seq();
    if (s == g_seen) return;
    g_seen = s;
    pc64_shell_dirty();
}

static unoui_canvas g_canvas = { vm_draw, vm_event, 0 };

static void vm_build(unoui_window *win)
{
    const unoui_metrics *m = &TH()->m;
    int aw = fb_width() - 150, ah = fb_height() - 140;
    if (aw > 720) aw = 720; if (aw < 460) aw = 460;
    if (ah > 460) ah = 460; if (ah < 300) ah = 300;
    unoui_window_init(win, "Appliances", 52, 40,
                      aw + 2 * m->frame_w + 2 * m->pad,
                      ah + m->title_h + 2 * m->pad + m->frame_w);
    unoui_add_canvas(win, 0, 0, aw, ah, &g_canvas);
    win->flags |= UI_WIN_RESIZE;
}

static void vm_opened(void) { g_view = V_LIST; g_edit = -1; g_follow = 1; }
static int  vm_canvas_index(void) { return 0; }

/* what the shell shows for this app, carried in the module (uno_appdesc.h) */
UNO_APP_DESC("id: vmgr\n"
             "name: Appliances\n"
             "icon: file:VMGR.QOI\n"
             "cat: system\n"
             "rank: 60\n"
             "min: 560x380\n");

static const UnoUuiApp kVmgr = {
    UNO_UUIAPP_ABI, "Appliances",
    vm_build, 0, vm_key, vm_frame, vm_opened, 0, vm_canvas_index
};

const UnoUuiApp *uno_app_main(void *reserved) { (void)reserved; return &kVmgr; }
