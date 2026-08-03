/* ===========================================================================
 * uochrome_test - the host gate for the Office 97 command-bar engine
 * (OFFICE97-PLAN §5 phase 6a).
 *
 * Runs without booting the OS, over the same uochrome.c the app module
 * compiles freestanding, linked against the shared software framebuffer the
 * unoui harness uses.  It drives a SCRIPTED abstract event stream - the same
 * unoui_event stream pc64 feeds - and after each gesture asserts two kinds of
 * thing:
 *
 *   BEHAVIOUR: which menu is open, which item is hot, what command fired.
 *   PIXELS:    that what was drawn matches what the state says.  A model that
 *              is right while the painter is wrong is the failure mode a
 *              behaviour-only test cannot see, so the navy of an open title,
 *              the raised edge of a hovered button and the sunken edge of a
 *              pressed one are all sampled from the framebuffer itself.
 *
 * Plus one property that costs nothing and catches a whole class of bug:
 * rendering the SAME state twice must produce byte-identical frames.  A
 * painter that accumulates (draws over instead of from scratch) passes every
 * single-shot check and drifts in use.
 *
 *   ./uochrome_test <out_dir>      write the storyboard PPMs and assert
 * ======================================================================== */
#include "uochrome.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fb[] comes from ../../ps2/fb.c, which we link. */

static uoc_ui UI;
static int g_frame, g_fail;
static const char *g_dir = "build";

/* ---- assertions ------------------------------------------------------------ */
static void fail(const char *what, const char *detail)
{
    printf("  FAIL %s: %s\n", what, detail);
    g_fail++;
}
static void eq(const char *what, int got, int want)
{
    if (got != want) {
        char b[128];
        sprintf(b, "got %d, wanted %d", got, want);
        fail(what, b);
    }
}
static void px_is(const char *what, int x, int y, fb_px want)
{
    fb_px got = fb[(long)y * FB_W + x];
    if (got != want) {
        char b[160];
        sprintf(b, "pixel (%d,%d) is %06X, wanted %06X",
                x, y, (unsigned)(got & 0xFFFFFF), (unsigned)(want & 0xFFFFFF));
        fail(what, b);
    }
}
/* "this row is not highlighted" cannot be spelled as "it is exactly face":
 * the pixel may legitimately be text, or the white emboss a disabled label
 * draws under itself.  What matters is that the selection colour is absent. */
static void px_not(const char *what, int x, int y, fb_px unwanted)
{
    fb_px got = fb[(long)y * FB_W + x];
    if (got == unwanted) {
        char b[160];
        sprintf(b, "pixel (%d,%d) is %06X, which it must not be",
                x, y, (unsigned)(got & 0xFFFFFF));
        fail(what, b);
    }
}

/* ---- events ---------------------------------------------------------------- */
static int g_cmd;
static void feed(unoui_event *e)
{
    int cmd = 0;
    uoc_handle(&UI, e, &cmd);
    if (cmd) g_cmd = cmd;
}
static void ev_move(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_MOVE; e.x=x; e.y=y; feed(&e); }
static void ev_down(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y; feed(&e); }
static void ev_up(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_UP; e.x=x; e.y=y; feed(&e); }
static void ev_key(int k, int mods)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_KEY; e.key=k; e.mods=mods; feed(&e); }
static void ev_char(int c, int mods)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_CHAR; e.ch=c; e.mods=mods; feed(&e); }
static void click(int x, int y) { ev_move(x,y); ev_down(x,y); ev_up(x,y); }

/* ---- the demo app's menus -------------------------------------------------
 * Word 97's own tree, abridged to what exercises every drawing case: an
 * accelerator column, separators, a submenu, a checked item, a radio item and
 * a disabled item.  It doubles as the first concrete draft of SPEC S-UOW-01,
 * which is why the strings are the real ones. */
enum {
    C_NONE = 0,
    C_NEW = 100, C_OPEN, C_CLOSE, C_SAVE, C_SAVEAS, C_PRINT, C_EXIT,
    C_SEND_MAIL, C_SEND_FAX,
    C_UNDO = 200, C_REDO, C_CUT, C_COPY, C_PASTE,
    C_NORMAL = 300, C_PAGELAYOUT, C_RULER, C_TOOLBARS,
    C_BOLD = 400, C_ITALIC, C_UNDERLINE, C_LEFT, C_CENTER
};

static const uoc_item kSendTo[] = {
    { "&Mail Recipient",    C_SEND_MAIL, 20, 0, 0, 0 },
    { "&Fax Recipient...",  C_SEND_FAX,  21, 0, 0, 0 },
    { "Microsoft &PowerPoint", 0,        -1, UOC_DISABLED, 0, 0 }
};
static const uoc_item kFile[] = {
    { "&New...\tCtrl+N",     C_NEW,    0, 0, 0, 0 },
    { "&Open...\tCtrl+O",    C_OPEN,   1, 0, 0, 0 },
    { "&Close",              C_CLOSE, -1, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Save\tCtrl+S",       C_SAVE,   2, 0, 0, 0 },
    { "Save &As...",         C_SAVEAS,-1, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "Sen&d To",            0,       -1, 0, kSendTo, 3 },
    { "&Print...\tCtrl+P",   C_PRINT,  3, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "E&xit",               C_EXIT,  -1, 0, 0, 0 }
};
static const uoc_item kEdit[] = {
    { "&Undo\tCtrl+Z",  C_UNDO,  4, 0, 0, 0 },
    { "&Redo\tCtrl+Y",  C_REDO,  5, UOC_DISABLED, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "Cu&t\tCtrl+X",   C_CUT,   6, 0, 0, 0 },
    { "&Copy\tCtrl+C",  C_COPY,  7, 0, 0, 0 },
    { "&Paste\tCtrl+V", C_PASTE, 8, 0, 0, 0 }
};
static const uoc_item kView[] = {
    { "&Normal",       C_NORMAL,     -1, UOC_RADIO | UOC_CHECKED, 0, 0 },
    { "&Page Layout",  C_PAGELAYOUT, -1, UOC_RADIO, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Ruler",        C_RULER,      -1, UOC_CHECKED, 0, 0 },
    { "&Toolbars",     C_TOOLBARS,   -1, 0, 0, 0 }
};
static const uoc_item kFormat[] = {
    { "&Font...",       0, -1, 0, 0, 0 },
    { "&Paragraph...",  0, -1, 0, 0, 0 },
    { "&Tabs...",       0, -1, 0, 0, 0 }
};
static const uoc_menu kMenus[] = {
    { "&File",   kFile,   11 },
    { "&Edit",   kEdit,    6 },
    { "&View",   kView,    5 },
    { "F&ormat", kFormat,  3 },
    { "&Help",   kFormat,  3 }
};
#define NMENU 5

static const uoc_tbitem kStd[] = {
    { UOC_TB_BUTTON, C_NEW,   0, "New",   0, 0 },
    { UOC_TB_BUTTON, C_OPEN,  1, "Open",  0, 0 },
    { UOC_TB_BUTTON, C_SAVE,  2, "Save",  0, 0 },
    { UOC_TB_SEP,    0,      -1, 0,       0, 0 },
    { UOC_TB_BUTTON, C_PRINT, 3, "Print", 0, 0 },
    { UOC_TB_BUTTON, C_UNDO,  4, "Undo",  0, 0 },
    { UOC_TB_BUTTON, C_REDO,  5, "Redo",  UOC_DISABLED, 0 },
    { UOC_TB_SEP,    0,      -1, 0,       0, 0 },
    { UOC_TB_BUTTON, C_CUT,   6, "Cut",   0, 0 },
    { UOC_TB_BUTTON, C_COPY,  7, "Copy",  0, 0 }
};
static const uoc_tbitem kFmt[] = {
    { UOC_TB_COMBO,  0,      -1, "Normal", 0, 90 },
    { UOC_TB_COMBO,  0,      -1, "Times",  0, 80 },
    { UOC_TB_SEP,    0,      -1, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_BOLD,      10, "Bold",      0, 0 },
    { UOC_TB_TOGGLE, C_ITALIC,    11, "Italic",    0, 0 },
    { UOC_TB_TOGGLE, C_UNDERLINE, 12, "Underline", 0, 0 },
    { UOC_TB_SEP,    0,      -1, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_LEFT,      13, "Align Left", 0, 0 },
    { UOC_TB_TOGGLE, C_CENTER,    14, "Center",     0, 0 }
};
static const uoc_tbar kBars[] = {
    { "Standard",   kStd, 10 },
    { "Formatting", kFmt,  9 }
};
#define NBAR 2

/* ---- the document behind the chrome, so overlap is visible ---------------- */
static void paint_all(void)
{
    int top = UI.y + uoc_height(&UI);
    fb_clear(FB_RGB(0x80,0x80,0x80));
    fb_fill_rect(40, top + 10, FB_W - 80, FB_H - top - 50, FB_RGB(0xFF,0xFF,0xFF));
    fb_text(52, top + 22, "The document sits under the chrome; a menu overlaps it.",
            FB_RGB(0x00,0x00,0x00), -1);
    uoc_render(&UI);
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i, n = FB_W * FB_H;
    if (!f) { perror(path); exit(2); }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < n; i++) {
        unsigned p = fb[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(p & 0xFF);
        rgb[1] = (unsigned char)((p >> 8) & 0xFF);
        rgb[2] = (unsigned char)((p >> 16) & 0xFF);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* Render, caption, snapshot - and prove the painter is a pure function of the
 * state by rendering twice into two buffers and comparing. */
static fb_px g_first[FB_W * FB_H];
static void snap(const char *label)
{
    char path[256], cap[96];
    paint_all();
    memcpy(g_first, fb, sizeof g_first);
    paint_all();
    if (memcmp(g_first, fb, sizeof g_first) != 0)
        fail("determinism", "rendering the same state twice differs");
    fb_fill_rect(0, FB_H - 14, FB_W, 14, FB_RGB(0x10,0x10,0x10));
    sprintf(cap, "%d. %s", g_frame + 1, label);
    fb_text(6, FB_H - 11, cap, FB_RGB(0xFF,0xFF,0xFF), -1);
    sprintf(path, "%s/uoc_%02d.ppm", g_dir, g_frame);
    write_ppm(path);
    printf("  %2d. %s\n", g_frame + 1, label);
    g_frame++;
}

/* ---- geometry the test needs (mirrors of what the engine computes) --------- */
static int bar_h(void)  { return fb_text_h() + 2 * uoc_look_97()->pad + 2; }
static int item_h(void)
{ int t = fb_text_h() + 6, i = uoc_look_97()->icon_px + 2; return t > i ? t : i; }
/* the centre of top-level menu item `idx` */
static int title_x(int idx)
{
    const uoc_look *k = uoc_look_97();
    int i, x = UI.x + k->pad;
    for (i = 0; i < idx; i++) x += fb_text_w(kMenus[i].title) - 8 + k->pad * 3;
    return x + 6;
}
/* Toolbar `bar` button `idx`: its exact top-left, mirroring what the engine
 * computes.  The pixel assertions need the corner, not an approximation -
 * that is where the bevel lives. */
static void btn_rect(const uoc_tbar *tb, int bar, int idx, int *rx, int *ry)
{
    const uoc_look *k = uoc_look_97();
    int i, x = UI.x + k->pad + 3;
    for (i = 0; i < idx; i++) {
        const uoc_tbitem *b = &tb->item[i];
        x += b->w > 0 ? b->w
           : (b->kind == UOC_TB_SEP ? k->pad + 2 : k->icon_px + 8);
    }
    *rx = x;
    *ry = UI.y + bar_h() + bar * (k->icon_px + 6) + 1;
}
static int btn_x(const uoc_tbar *tb, int idx)
{ int x, y; btn_rect(tb, 0, idx, &x, &y); return x + 6; }
static int row_y(int bar) { return UI.y + bar_h() + bar * (uoc_look_97()->icon_px + 6) + 8; }

int main(int argc, char **argv)
{
    const uoc_look *k = uoc_look_97();
    int y0;

    if (argc >= 2) g_dir = argv[1];
    uoc_init(&UI, kMenus, NMENU, kBars, NBAR, 0, 0, FB_W);

    printf("uochrome storyboard -> %s\n", g_dir);
    y0 = bar_h() / 2;

    /* 1. idle */
    snap("idle: menu bar and two toolbars");
    eq("idle: nothing open", uoc_menu_open(&UI), 0);
    eq("idle: chrome height",
       uoc_height(&UI), bar_h() + 2 * (k->icon_px + 6));

    /* 2. hovering a title highlights it but does NOT open it */
    ev_move(title_x(1), y0);
    snap("hover Edit (no menu opens on hover alone)");
    eq("hover: still closed", uoc_menu_open(&UI), 0);
    eq("hover: hot title", UI.hot, 1);
    px_is("hover: the title sits on the navy bar", title_x(1), y0, k->sel);

    /* 3. click opens it */
    click(title_x(0), y0);
    snap("File menu open");
    eq("open: File", UI.open, 0);
    eq("open: one level", UI.depth, 1);
    px_is("open: the title stays navy", title_x(0), y0, k->sel);
    /* the popup body is face-coloured, below the bar */
    px_is("open: the popup covers the document",
          UI.x + 20, UI.y + bar_h() + 2 * (k->icon_px + 6) + item_h(), k->face);

    /* 4. hovering an item highlights it */
    {
        int py = UI.y + bar_h() + 2 + item_h() + item_h() / 2;   /* item 1 */
        ev_move(UI.x + 40, py);
        snap("hover \"Open...\" - navy bar, icon, accelerator column");
        eq("hover item: index", UI.path[0], 1);
        px_is("hover item: navy behind it", UI.x + 60, py, k->sel);
    }

    /* 5. a disabled item never becomes hot */
    click(title_x(1), y0);                       /* Edit */
    {
        int py = UI.y + bar_h() + 2 + item_h() + item_h() / 2;   /* Redo */
        ev_move(UI.x + 40, py);
        snap("Edit: hovering the disabled \"Redo\" does not highlight it");
        eq("disabled: not hot", UI.path[0], -1);
        px_not("disabled: no navy", UI.x + 60, py, k->sel);
    }

    /* 6. a submenu */
    click(title_x(0), y0);                       /* File again */
    {
        int i, py = UI.y + bar_h() + 2;
        for (i = 0; i < 7; i++) py += kFile[i].text ? item_h() : (fb_text_h()/2 + 3);
        py += item_h() / 2;                      /* "Send To" */
        ev_move(UI.x + 40, py);
        snap("File > Send To: the submenu opens to the right");
        eq("submenu: two levels", UI.depth, 2);
        eq("submenu: parent hot", UI.path[0], 7);
    }

    /* 7. a command fires from the submenu, and the menus close */
    g_cmd = 0;
    {
        int i, py = UI.y + bar_h() + 2;
        for (i = 0; i < 7; i++) py += kFile[i].text ? item_h() : (fb_text_h()/2 + 3);
        py += item_h() / 2;                      /* the "Send To" item's middle */
        ev_move(UI.x + 40, py);                  /* keep the submenu open      */
        /* A submenu's border is aligned to its parent ITEM, so the parent
         * item's own middle is inside the submenu's FIRST entry - which is
         * exactly the slide that makes "hover across into the submenu" feel
         * right, and the one that is off by an item if you reason from the
         * popup's top instead. */
        ev_move(UI.x + 240, py);
        eq("submenu: first entry hot", UI.path[1], 0);
        ev_up(UI.x + 240, py);
        snap("a submenu command fires and everything closes");
        eq("fired: Mail Recipient", g_cmd, C_SEND_MAIL);
        eq("fired: menus closed", uoc_menu_open(&UI), 0);
    }

    /* 8. keyboard: F10 activates the bar, Right walks it, Down opens */
    ev_key(UOC_KEY_F10, 0);
    eq("F10: bar keyed", UI.keyed, 1);
    eq("F10: nothing open yet", uoc_menu_open(&UI), 0);
    ev_key(UI_KEY_RIGHT, 0);
    ev_key(UI_KEY_DOWN, 0);
    snap("keyboard: F10, Right, Down opens Edit with its first item hot");
    eq("kbd: Edit open", UI.open, 1);
    eq("kbd: first item hot", UI.path[0], 0);

    /* Down skips the disabled Redo and the separator that follows it */
    ev_key(UI_KEY_DOWN, 0);
    snap("keyboard: Down skips the disabled item and the separator");
    eq("kbd: skipped to Cut", UI.path[0], 3);

    /* Enter fires it */
    g_cmd = 0;
    ev_key(UI_KEY_ENTER, 0);
    eq("kbd: Enter fired Cut", g_cmd, C_CUT);
    eq("kbd: closed after firing", uoc_menu_open(&UI), 0);

    /* 9. Alt+mnemonic opens a menu by letter */
    ev_char('o', UI_MOD_ALT);                    /* "F&ormat" */
    snap("Alt+O opens Format by its mnemonic");
    eq("mnemonic: Format open", UI.open, 3);
    ev_key(UI_KEY_ESC, 0);
    ev_key(UI_KEY_ESC, 0);
    ev_key(UI_KEY_ESC, 0);
    eq("esc: fully dismissed", uoc_menu_open(&UI), 0);
    eq("esc: bar released", UI.keyed, 0);

    /* 10. toolbar: flat, raised on hover, sunken on press.  This is SPEC
     *     S-OFF-01's central claim about Office 97 command bars, so it is
     *     asserted at the pixel that carries it: the button's top-left
     *     corner, which is face when flat, the bright edge when hovered and
     *     the dark edge when pressed. */
    {
        int bx = btn_x(&kBars[0], 1), by = row_y(0), cx, cy;
        btn_rect(&kBars[0], 0, 1, &cx, &cy);

        ev_move(FB_W / 2, FB_H - 30);           /* nowhere near the toolbar */
        paint_all();
        px_is("tb flat: no bevel at rest", cx, cy, k->face);

        ev_move(bx, by);
        snap("toolbar: hovering \"Open\" raises it");
        eq("tb hover: bar", UI.hot_bar, 0);
        eq("tb hover: button", UI.hot_btn, 1);
        px_is("tb hover: the bright edge appears", cx, cy, k->hilight);

        ev_down(bx, by);
        snap("toolbar: holding it sinks it");
        eq("tb press: button held", UI.down_btn, 1);
        px_is("tb press: the edge inverts to dark", cx, cy, k->shadow);

        g_cmd = 0;
        ev_up(bx, by);
        eq("tb: released fires the command", g_cmd, C_OPEN);
        eq("tb: no longer held", UI.down_btn, -1);
    }

    /* 11. a disabled toolbar button neither hovers nor fires */
    {
        int bx = btn_x(&kBars[0], 6), by = row_y(0);   /* Redo, disabled */
        g_cmd = 0;
        click(bx, by);
        snap("toolbar: the disabled \"Redo\" ignores a click");
        eq("tb disabled: nothing fired", g_cmd, 0);
    }

    /* 12. a toggle stays down after the mouse leaves */
    {
        int bx, by = row_y(1), cx, cy;
        btn_rect(&kBars[1], 1, 3, &cx, &cy);           /* Bold */
        bx = cx + 6;
        g_cmd = 0;
        click(bx, by);
        ev_move(FB_W / 2, FB_H - 30);                  /* leave the toolbar */
        snap("toolbar: Bold toggled ON stays sunken with the mouse away");
        eq("toggle: fired", g_cmd, C_BOLD);
        eq("toggle: on", uoc_toggle(&UI, C_BOLD), 1);
        px_is("toggle: still sunken once the mouse has gone", cx, cy, k->shadow);

        click(bx, by);
        ev_move(FB_W / 2, FB_H - 30);
        snap("toolbar: clicking Bold again turns it off");
        eq("toggle: off again", uoc_toggle(&UI, C_BOLD), 0);
        px_is("toggle: flat again", cx, cy, k->face);
    }

    /* 13. a click on the document closes an open menu and is consumed */
    click(title_x(2), y0);
    eq("view open", UI.open, 2);
    {
        unoui_event e;
        int cmd = 0, used;
        memset(&e, 0, sizeof e);
        e.kind = UI_EV_MOUSE_DOWN; e.x = FB_W / 2; e.y = FB_H - 60;
        used = uoc_handle(&UI, &e, &cmd);
        eq("click-away: consumed by the chrome", used, 1);
        eq("click-away: menu closed", uoc_menu_open(&UI), 0);
    }
    snap("a click on the document dismisses the menu");

    printf(g_fail ? "\nuochrome gate: %d FAILURE(S)\n" : "\nuochrome gate: GREEN\n",
           g_fail);
    return g_fail ? 1 : 0;
}
