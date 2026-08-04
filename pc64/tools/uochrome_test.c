/* ===========================================================================
 * uochrome_test - the host gate for the Office 97 command-bar engine
 * (OFFICE97-PLAN §5 phase 6a and 6b).
 *
 * Runs without booting the OS, over the same uochrome.c the app module
 * compiles freestanding, linked against the shared software framebuffer the
 * unoui harness uses.  It drives a SCRIPTED abstract event stream - the same
 * unoui_event stream pc64 feeds - and after each gesture asserts two kinds of
 * thing:
 *
 *   BEHAVIOUR: which menu is open, which item is hot, what command fired,
 *              where a toolbar ended up, what a palette handed back.
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
#include "uoicons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fb[] comes from ../../ps2/fb.c, which we link. */

/* the chrome does not live at the top-left corner of the world */
#define ORIGIN_X 37
#define ORIGIN_Y 23

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
static void ev_tick(int n)
{ unoui_event e; int i; memset(&e,0,sizeof e); e.kind=UI_EV_TICK;
  for (i = 0; i < n; i++) feed(&e); }
static void click(int x, int y) { ev_move(x,y); ev_down(x,y); ev_up(x,y); }
static void drag(int x0, int y0, int x1, int y1)
{ ev_move(x0,y0); ev_down(x0,y0); ev_move((x0+x1)/2,(y0+y1)/2); ev_move(x1,y1); ev_up(x1,y1); }

/* ---- the demo app's menus -------------------------------------------------
 * Word 97's own tree, abridged to what exercises every drawing case: an
 * accelerator column, separators, a submenu, a checked item, a radio item and
 * a disabled item.  It doubles as the first concrete draft of SPEC S-UOW-01,
 * which is why the strings are the real ones. */
enum {
    C_NEW = 100, C_OPEN, C_CLOSE, C_SAVE, C_SAVEAS, C_PRINT, C_EXIT,
    C_SEND_MAIL, C_SEND_FAX,
    C_UNDO = 200, C_REDO, C_CUT, C_COPY, C_PASTE,
    C_NORMAL = 300, C_PAGELAYOUT, C_RULER, C_TOOLBARS,
    C_BOLD = 400, C_ITALIC, C_UNDERLINE, C_LEFT, C_CENTER,
    C_STYLE = 500, C_FONT, C_FONTCOLOR, C_BORDERS
};

static const uoc_item kSendTo[] = {
    { "&Mail Recipient",      C_SEND_MAIL, UOI_LINK, 0, 0, 0 },
    { "&Fax Recipient...",    C_SEND_FAX,  UOI_PRINT, 0, 0, 0 },
    { "Microsoft &PowerPoint", 0,          -1, UOC_DISABLED, 0, 0 }
};
static const uoc_item kFile[] = {
    { "&New...\tCtrl+N",     C_NEW,   UOI_NEW,   0, 0, 0 },
    { "&Open...\tCtrl+O",    C_OPEN,  UOI_OPEN,  0, 0, 0 },
    { "&Close",              C_CLOSE, -1, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "&Save\tCtrl+S",       C_SAVE,  UOI_SAVE,  0, 0, 0 },
    { "Save &As...",         C_SAVEAS,-1, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "Sen&d To",            0,       -1, 0, kSendTo, 3 },
    { "&Print...\tCtrl+P",   C_PRINT, UOI_PRINT, 0, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "E&xit",               C_EXIT,  -1, 0, 0, 0 }
};
static const uoc_item kEdit[] = {
    { "&Undo\tCtrl+Z",  C_UNDO,  UOI_UNDO,  0, 0, 0 },
    { "&Redo\tCtrl+Y",  C_REDO,  UOI_REDO,  UOC_DISABLED, 0, 0 },
    { 0, 0, -1, 0, 0, 0 },
    { "Cu&t\tCtrl+X",   C_CUT,   UOI_CUT,   0, 0, 0 },
    { "&Copy\tCtrl+C",  C_COPY,  UOI_COPY,  0, 0, 0 },
    { "&Paste\tCtrl+V", C_PASTE, UOI_PASTE, 0, 0, 0 }
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

/* the sixteen-colour palette Office 97 dropped from a colour button */
static const fb_px kColors[16] = {
    FB_RGB(0x00,0x00,0x00), FB_RGB(0x80,0x00,0x00), FB_RGB(0x00,0x80,0x00),
    FB_RGB(0x80,0x80,0x00), FB_RGB(0x00,0x00,0x80), FB_RGB(0x80,0x00,0x80),
    FB_RGB(0x00,0x80,0x80), FB_RGB(0xC0,0xC0,0xC0), FB_RGB(0x80,0x80,0x80),
    FB_RGB(0xFF,0x00,0x00), FB_RGB(0x00,0xFF,0x00), FB_RGB(0xFF,0xFF,0x00),
    FB_RGB(0x00,0x00,0xFF), FB_RGB(0xFF,0x00,0xFF), FB_RGB(0x00,0xFF,0xFF),
    FB_RGB(0xFF,0xFF,0xFF)
};
static const uoc_palette kFontColorPal = {
    UOC_PAL_COLOR, kColors, 0, 16, 8, "Font Color"
};
static const int kBorderIcons[8] = {
    UOI_BORDERS, UOI_BORDERS, UOI_BORDERS, UOI_BORDERS,
    UOI_BORDERS, UOI_BORDERS, UOI_BORDERS, UOI_BORDERS
};
static const uoc_palette kBorderPal = {
    UOC_PAL_ICON, 0, kBorderIcons, 8, 4, "Borders"
};

static const char *const kStyles[] = {
    "Normal", "Heading 1", "Heading 2", "Heading 3", "Body Text", "List Bullet"
};
static const char *const kFonts[] = {
    "Times New Roman", "Arial", "Courier New", "Symbol"
};

static const uoc_tbitem kStd[] = {
    { UOC_TB_BUTTON, C_NEW,   UOI_NEW,   "New",   0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_OPEN,  UOI_OPEN,  "Open",  0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_SAVE,  UOI_SAVE,  "Save",  0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0,      -1, 0,               0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_PRINT, UOI_PRINT, "Print", 0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_UNDO,  UOI_UNDO,  "Undo",  0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_REDO,  UOI_REDO,  "Redo",  UOC_DISABLED, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0,      -1, 0,               0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_CUT,   UOI_CUT,   "Cut",   0, 0, 0, 0, 0 },
    { UOC_TB_BUTTON, C_COPY,  UOI_COPY,  "Copy",  0, 0, 0, 0, 0 }
};
static const uoc_tbitem kFmt[] = {
    { UOC_TB_COMBO,  C_STYLE, -1, "Style", 0, 90, kStyles, 6, 0 },
    { UOC_TB_COMBO,  C_FONT,  -1, "Font",  0, 80, kFonts,  4, 0 },
    { UOC_TB_SEP,    0,       -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_BOLD,      UOI_BOLD,      "Bold",      0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_ITALIC,    UOI_ITALIC,    "Italic",    0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_UNDERLINE, UOI_UNDERLINE, "Underline", 0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0,       -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_LEFT,   UOI_ALIGN_L, "Align Left", 0, 0, 0, 0, 0 },
    { UOC_TB_TOGGLE, C_CENTER, UOI_ALIGN_C, "Center",     0, 0, 0, 0, 0 },
    { UOC_TB_SEP,    0,       -1, 0, 0, 0, 0, 0, 0 },
    { UOC_TB_SPLIT,  C_FONTCOLOR, UOI_FONTCOLOR, "Font Color", 0, 0, 0, 0,
      &kFontColorPal },
    { UOC_TB_SPLIT,  C_BORDERS,   UOI_BORDERS,   "Borders",    0, 0, 0, 0,
      &kBorderPal }
};
static const uoc_tbar kBars[] = {
    { "Standard",   kStd, 10 },
    { "Formatting", kFmt, 12 }
};
#define NBAR 2

/* ---- the document behind the chrome, so overlap is visible ---------------- */
static void paint_all(void)
{
    int cx, cy, cw, ch;
    uoc_client_rect(&UI, &cx, &cy, &cw, &ch);
    fb_clear(FB_RGB(0x80,0x80,0x80));
    fb_fill_rect(cx + 8, cy + 8, cw - 16, ch - 24, FB_RGB(0xFF,0xFF,0xFF));
    fb_text(cx + 20, cy + 20, "The document sits under the chrome.",
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
static int tb_h(void)   { return uoc_look_97()->icon_px + 6; }
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
           : (b->kind == UOC_TB_SEP ? k->pad + 2
           : (b->kind == UOC_TB_SPLIT ? k->icon_px + 8 + k->pad + 7
                                      : k->icon_px + 8));
    }
    *rx = x;
    *ry = UI.y + bar_h() + bar * tb_h() + 1;
}
static int row_y(int bar) { return UI.y + bar_h() + bar * tb_h() + tb_h() / 2; }

int main(int argc, char **argv)
{
    const uoc_look *k = uoc_look_97();
    int y0;

    if (argc >= 2) g_dir = argv[1];
    uoc_icons_install();                 /* phase 6b: real artwork */

    /* THE CHROME IS PLACED AT A NON-ZERO ORIGIN, deliberately.
     *
     * Every earlier run of this gate put it at (0,0), where an origin bug is
     * invisible because adding the wrong origin to zero still gives zero.
     * UnoWord found that out on the screen: the app reconstructed its canvas
     * rect from the window FRAME instead of the window's CONTENT origin, was
     * short by the title bar, and every menu click missed while the gate
     * stayed green.  Running the whole storyboard offset makes that class of
     * bug fail here instead. */
    uoc_init(&UI, kMenus, NMENU, kBars, NBAR, ORIGIN_X, ORIGIN_Y,
             FB_W - ORIGIN_X, FB_H - 14 - ORIGIN_Y);

    printf("uochrome storyboard -> %s\n", g_dir);
    y0 = UI.y + bar_h() / 2;   /* the bar is at the ORIGIN, not at zero */

    /* ================= phase 6a: menus, and flat toolbars ================= */

    snap("idle: menu bar, two toolbars, real icons");
    eq("idle: nothing open", uoc_menu_open(&UI), 0);
    eq("idle: top band height", uoc_height(&UI), bar_h() + 2 * tb_h());

    ev_move(title_x(1), y0);
    snap("hover Edit (no menu opens on hover alone)");
    eq("hover: still closed", uoc_menu_open(&UI), 0);
    eq("hover: hot title", UI.hot, 1);
    px_is("hover: the title sits on the navy bar", title_x(1), y0, k->sel);

    click(title_x(0), y0);
    snap("File menu open");
    eq("open: File", UI.open, 0);
    eq("open: one level", UI.depth, 1);
    px_is("open: the title stays navy", title_x(0), y0, k->sel);

    {
        int py = UI.y + bar_h() + 2 + item_h() + item_h() / 2;   /* item 1 */
        ev_move(UI.x + 40, py);
        snap("hover \"Open...\" - navy bar, icon, accelerator column");
        eq("hover item: index", UI.path[0], 1);
        px_is("hover item: navy behind it", UI.x + 60, py, k->sel);
    }

    click(title_x(1), y0);                       /* Edit */
    {
        int py = UI.y + bar_h() + 2 + item_h() + item_h() / 2;   /* Redo */
        ev_move(UI.x + 40, py);
        snap("Edit: hovering the disabled \"Redo\" does not highlight it");
        eq("disabled: not hot", UI.path[0], -1);
        px_not("disabled: no navy", UI.x + 60, py, k->sel);
    }

    click(title_x(0), y0);                       /* File again */
    {
        int i, py = UI.y + bar_h() + 2;
        for (i = 0; i < 7; i++) py += kFile[i].text ? item_h() : (fb_text_h()/2 + 3);
        py += item_h() / 2;
        ev_move(UI.x + 40, py);
        snap("File > Send To: the submenu opens to the right");
        eq("submenu: two levels", UI.depth, 2);
        eq("submenu: parent hot", UI.path[0], 7);
    }

    g_cmd = 0;
    {
        int i, py = UI.y + bar_h() + 2;
        for (i = 0; i < 7; i++) py += kFile[i].text ? item_h() : (fb_text_h()/2 + 3);
        py += item_h() / 2;                      /* the "Send To" item's middle */
        ev_move(UI.x + 40, py);
        /* A submenu's border is aligned to its parent ITEM, so the parent
         * item's own middle is inside the submenu's FIRST entry - the slide
         * that makes "hover across into the submenu" feel right, and the one
         * that is off by an item if you reason from the popup's top. */
        ev_move(UI.x + 240, py);
        eq("submenu: first entry hot", UI.path[1], 0);
        ev_up(UI.x + 240, py);
        snap("a submenu command fires and everything closes");
        eq("fired: Mail Recipient", g_cmd, C_SEND_MAIL);
        eq("fired: menus closed", uoc_menu_open(&UI), 0);
    }

    ev_key(UOC_KEY_F10, 0);
    eq("F10: bar keyed", UI.keyed, 1);
    eq("F10: nothing open yet", uoc_menu_open(&UI), 0);
    ev_key(UI_KEY_RIGHT, 0);
    ev_key(UI_KEY_DOWN, 0);
    snap("keyboard: F10, Right, Down opens Edit with its first item hot");
    eq("kbd: Edit open", UI.open, 1);
    eq("kbd: first item hot", UI.path[0], 0);

    ev_key(UI_KEY_DOWN, 0);
    snap("keyboard: Down skips the disabled item and the separator");
    eq("kbd: skipped to Cut", UI.path[0], 3);

    g_cmd = 0;
    ev_key(UI_KEY_ENTER, 0);
    eq("kbd: Enter fired Cut", g_cmd, C_CUT);
    eq("kbd: closed after firing", uoc_menu_open(&UI), 0);

    ev_char('o', UI_MOD_ALT);                    /* "F&ormat" */
    snap("Alt+O opens Format by its mnemonic");
    eq("mnemonic: Format open", UI.open, 3);
    ev_key(UI_KEY_ESC, 0);
    ev_key(UI_KEY_ESC, 0);
    ev_key(UI_KEY_ESC, 0);
    eq("esc: fully dismissed", uoc_menu_open(&UI), 0);
    eq("esc: bar released", UI.keyed, 0);

    /* flat, raised on hover, sunken on press - SPEC S-OFF-01's central claim,
     * asserted at the pixel that carries it */
    {
        int bx, by = row_y(0), cx, cy;
        btn_rect(&kBars[0], 0, 1, &cx, &cy);
        bx = cx + 6;

        ev_move(FB_W / 2, FB_H - 40);
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

    {
        int bx, by = row_y(0), cx, cy;
        btn_rect(&kBars[0], 0, 6, &cx, &cy);       /* Redo, disabled */
        bx = cx + 6;
        g_cmd = 0;
        click(bx, by);
        snap("toolbar: the disabled \"Redo\" ignores a click");
        eq("tb disabled: nothing fired", g_cmd, 0);
    }

    {
        int bx, by = row_y(1), cx, cy;
        btn_rect(&kBars[1], 1, 3, &cx, &cy);       /* Bold */
        bx = cx + 6;
        g_cmd = 0;
        click(bx, by);
        ev_move(FB_W / 2, FB_H - 40);
        snap("toolbar: Bold toggled ON stays sunken with the mouse away");
        eq("toggle: fired", g_cmd, C_BOLD);
        eq("toggle: on", uoc_toggle(&UI, C_BOLD), 1);
        px_is("toggle: still sunken once the mouse has gone", cx, cy, k->shadow);

        click(bx, by);
        ev_move(FB_W / 2, FB_H - 40);
        paint_all();                 /* sample what THIS state draws, not the
                                      * frame the previous snap left behind */
        eq("toggle: off again", uoc_toggle(&UI, C_BOLD), 0);
        px_is("toggle: flat again", cx, cy, k->face);
    }

    /* ============ phase 6b: combos, palettes, tear-offs, docking ========== */

    /* a ScreenTip after the dwell */
    {
        int bx, by = row_y(0), cx, cy;
        btn_rect(&kBars[0], 0, 2, &cx, &cy);       /* Save */
        bx = cx + 6;
        ev_move(bx, by);
        ev_tick(UOC_TIP_TICKS + 1);
        snap("ScreenTip: \"Save\" appears after the hover dwell");
        eq("tip: armed on the right button", UI.tip_btn, 2);
    }

    /* the Style combo drops a list, and picking from it changes the field */
    {
        int cx, cy, bx, by = row_y(1);
        btn_rect(&kBars[1], 1, 0, &cx, &cy);       /* Style combo */
        bx = cx + 80;                              /* its drop button */
        ev_move(bx, by);
        ev_down(bx, by);
        ev_up(bx, by);
        snap("combo: the Style list drops");
        eq("combo: a list is open", UI.pop_kind, UOC_POP_COMBO);

        {   int lx, ly;
            lx = cx + 10;
            ly = cy + (uoc_look_97()->icon_px + 6 - 2) + 1 + 2 * (fb_text_h() + 2) + 1;
            ev_move(lx, ly);
            eq("combo: third row hot", UI.pop_hot, 2);
            g_cmd = 0;
            ev_down(lx, ly);
            snap("combo: picking \"Heading 2\" closes the list and sets the field");
            eq("combo: fired its id", g_cmd, C_STYLE);
            eq("combo: selection stored", uoc_combo(&UI, C_STYLE), 2);
            eq("combo: list closed", UI.pop_kind, UOC_POP_NONE);
        }
    }

    /* a split button drops a colour palette */
    {
        int cx, cy, ax, by = row_y(1);
        btn_rect(&kBars[1], 1, 10, &cx, &cy);      /* Font Color split */
        ax = cx + k->icon_px + 8 + 3;              /* over the arrow half */
        ev_move(ax, by);
        ev_down(ax, by);
        snap("split button: the Font Color palette drops");
        eq("palette: open", UI.pop_kind, UOC_POP_PALETTE);

        {   int sw = k->icon_px + 4;
            int px = cx, py = cy + (k->icon_px + 6 - 2);
            int hx = px + 2 + sw / 2 + sw, hy = py + 7 + 2 + sw / 2;
            ev_move(hx, hy);
            eq("palette: second swatch hot", UI.pop_hot, 1);
            g_cmd = 0;
            ev_down(hx, hy);
            snap("palette: picking a swatch reports the button and the index");
            eq("palette: fired the split button's id", g_cmd, C_FONTCOLOR);
            eq("palette: the index picked", uoc_pick(&UI), 1);
            eq("palette: closed", UI.pop_kind, UOC_POP_NONE);
        }
    }

    /* ...and the same palette can be TORN OFF by its move bar */
    {
        int cx, cy, ax, by = row_y(1), px, py;
        btn_rect(&kBars[1], 1, 10, &cx, &cy);
        ax = cx + k->icon_px + 8 + 3;
        ev_move(ax, by);
        ev_down(ax, by);
        eq("tear: palette open again", UI.pop_kind, UOC_POP_PALETTE);
        px = cx; py = cy + (k->icon_px + 6 - 2);
        /* grab the move bar across its top and drag it away */
        ev_down(px + 20, py + 2);
        ev_move(240, 220);
        ev_up(240, 220);
        snap("tear-off: the palette is dragged away and floats");
        eq("tear: one palette is floating", UI.tear[0].open, 1);
        eq("tear: the drop-down itself closed", UI.pop_kind, UOC_POP_NONE);

        /* a swatch in the FLOATING palette still picks */
        g_cmd = 0;
        {   int sw = k->icon_px + 4, th = fb_text_h() + k->pad;
            ev_down(UI.tear[0].x + 2 + sw / 2,
                    UI.tear[0].y + th + 7 + 2 + sw / 2);
            eq("tear: a floating swatch still fires", g_cmd, C_FONTCOLOR);
            eq("tear: index 0", uoc_pick(&UI), 0);
        }
    }

    /* dragging a toolbar off its dock makes it float */
    {
        int gx = UI.x + 3, gy = UI.y + bar_h() + tb_h() + tb_h() / 2;
        eq("dock: two top bands to start", uoc_height(&UI), bar_h() + 2 * tb_h());
        drag(gx, gy, 300, 300);
        snap("docking: the Formatting bar dragged into the document floats");
        eq("dock: it is floating", UI.bs[1].dock, UOC_DOCK_FLOAT);
        eq("dock: one top band left", uoc_height(&UI), bar_h() + tb_h());
    }

    /* ...and dragging it to the left edge docks it as a column */
    {
        int tx = UI.bs[1].fx + 20, ty = UI.bs[1].fy + 3;
        int cx0, cy0, cw0, ch0, cx1, cw1;
        uoc_client_rect(&UI, &cx0, &cy0, &cw0, &ch0);
        drag(tx, ty, UI.x + 4, 240);
        snap("docking: dropped at the left edge, it becomes a column");
        eq("dock: docked left", UI.bs[1].dock, UOC_DOCK_LEFT);
        uoc_client_rect(&UI, &cx1, &cy0, &cw1, &ch0);
        eq("dock: the client area moved right", cx1, cx0 + tb_h());
        eq("dock: and got narrower", cw1, cw0 - tb_h());
    }

    /* a floating bar's close box hides it */
    {
        int tx, ty;
        drag(UI.x + 4, UI.y + bar_h() + tb_h() + 4, 320, 260);
        eq("close: floating again", UI.bs[1].dock, UOC_DOCK_FLOAT);
        tx = UI.bs[1].fx; ty = UI.bs[1].fy;
        {   int len = 0, i;
            for (i = 0; i < kBars[1].n; i++) {
                const uoc_tbitem *b = &kBars[1].item[i];
                len += b->w > 0 ? b->w
                     : (b->kind == UOC_TB_SEP ? k->pad + 2
                     : (b->kind == UOC_TB_SPLIT ? k->icon_px + 8 + k->pad + 7
                                                : k->icon_px + 8));
            }
            len += k->pad * 2;
            /* the close box spans [fx+fw-s-2, fx+fw-2): aim at its middle,
             * not its left edge, which is two pixels outside it */
            click(tx + len - 2 - fb_text_h() / 2, ty + 2 + fb_text_h() / 2);
        }
        snap("a floating bar's close box hides it");
        eq("close: hidden", UI.bs[1].hidden, 1);
    }

    /* ---- every icon in the atlas must actually put ink down ---------------
     * Eight of them did not.  `art()` indexes its palette HEX-STYLE - 'a' is
     * ten, not one - and eight icons were written with 'a' meaning "the
     * second colour", so they indexed past a two-entry palette and drew
     * nothing.  Bold, Italic and Underline were blank squares on every
     * Formatting bar in the suite from the day they landed, and it took
     * someone running the OS on a laptop to notice: a toolbar with an
     * invisible button still lays out, still highlights, still fires.
     *
     * The threshold is deliberately not 1.  An icon that puts down a single
     * pixel is as broken as one that puts down none, and would pass. */
    {
        int cell = 0, cols = 0, count = 0, i;
        const fb_px *atlas = uoc_icons_97(&cell, &cols, &count);
        int worst = 1 << 30, worst_i = -1;
        for (i = 0; i < count; i++) {
            int ink = 0, x, y;
            int ox = (i % cols) * cell, oy = (i / cols) * cell;
            for (y = 0; y < cell; y++)
                for (x = 0; x < cell; x++)
                    if (atlas[(long)(oy + y) * cell * cols + ox + x]) ink++;
            if (ink < worst) { worst = ink; worst_i = i; }
            if (ink < 8) {
                char b[120];
                sprintf(b, "icon %d has %d visible pixels", i, ink);
                fail("blank icon", b);
            }
        }
        printf("  %d icons in the atlas, thinnest is #%d at %d pixels\n",
               count, worst_i, worst);
    }

    printf(g_fail ? "\nuochrome gate: %d FAILURE(S)\n" : "\nuochrome gate: GREEN\n",
           g_fail);
    return g_fail ? 1 : 0;
}
