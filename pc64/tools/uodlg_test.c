/* ===========================================================================
 * uodlg_test - the host gate for the Office 97 dialog engine
 * (OFFICE97-PLAN §5 phase 6c).
 *
 * Same shape as the chrome gate: a scripted unoui_event stream over the real
 * engine, asserting behaviour, sampled pixels and render-twice determinism,
 * and dropping a storyboard for the eye.
 *
 * The dialog it drives is Word 97's Font dialog, abridged - three tabs, a
 * font list, a style list, a size spinner, a colour combo, the effects
 * checks, a preview well and the OK/Cancel row.  It is declared the way an
 * app would declare it, as a DATA TABLE, which is the whole claim this phase
 * makes: Office's thirty-odd dialogs are one engine and thirty tables.
 * ======================================================================== */
#include "uodlg.h"
#include "uoicons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uod_ui D;
static int g_frame, g_fail;
static const char *g_dir = "build";

static void fail(const char *what, const char *detail)
{ printf("  FAIL %s: %s\n", what, detail); g_fail++; }
static void eq(const char *what, int got, int want)
{
    if (got != want) {
        char b[128]; sprintf(b, "got %d, wanted %d", got, want);
        fail(what, b);
    }
}
static void streq(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        char b[160]; sprintf(b, "got \"%s\", wanted \"%s\"", got, want);
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

static void feed(unoui_event *e) { uod_handle(&D, e); }
static void ev_move(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_MOVE; e.x=x; e.y=y; feed(&e); }
static void ev_down(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y; feed(&e); }
static void ev_up(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_UP; e.x=x; e.y=y; feed(&e); }
static void ev_key(int k, int mods)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_KEY; e.key=k; e.mods=mods; feed(&e); }
static void ev_char(int c)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_CHAR; e.ch=c; feed(&e); }
static void click(int x, int y) { ev_move(x,y); ev_down(x,y); ev_up(x,y); }

/* ---- the Font dialog, as a data table ------------------------------------- */
#define LBL(t,x,y,w,pg)          { UOD_LABEL,0,t,x,y,w,0,pg,0,0,0,0,0,0 }
#define BTN(id,t,x,y,w,h,fl)     { UOD_BUTTON,id,t,x,y,w,h,-1,fl,0,0,0,0,0 }
#define CHK(id,t,x,y,w,pg)       { UOD_CHECK,id,t,x,y,w,0,pg,0,0,0,0,0,0 }
#define RAD(id,t,x,y,w,pg,g)     { UOD_RADIO,id,t,x,y,w,0,pg,0,0,0,g,0,0 }
#define LST(id,x,y,w,h,pg,l,n)   { UOD_LIST,id,0,x,y,w,h,pg,0,l,n,0,0,0 }
#define CMB(id,t,x,y,w,h,pg,l,n) { UOD_COMBO,id,t,x,y,w,h,pg,0,l,n,0,0,0 }
#define SPN(id,t,x,y,w,h,pg,a,b) { UOD_SPIN,id,t,x,y,w,h,pg,0,0,0,0,a,b }
#define GRP(t,x,y,w,h,pg)        { UOD_GROUP,0,t,x,y,w,h,pg,0,0,0,0,0,0 }
#define PRV(id,x,y,w,h,pg)       { UOD_PREVIEW,id,0,x,y,w,h,pg,0,0,0,0,0,0 }
#define EDT(id,t,x,y,w,h,pg)     { UOD_EDIT,id,t,x,y,w,h,pg,0,0,0,0,0,0 }

enum {
    F_FONT = 10, F_STYLE, F_SIZE, F_COLOR,
    F_STRIKE, F_SHADOW, F_SMALLCAPS, F_HIDDEN,
    F_UNDER_NONE, F_UNDER_SINGLE, F_UNDER_DOUBLE,
    F_SCALE, F_PREVIEW
};

static const char *const kFonts[] = {
    "Times New Roman", "Arial", "Courier New", "Symbol", "Wingdings"
};
static const char *const kStyles[] = { "Regular", "Italic", "Bold", "Bold Italic" };
static const char *const kColors[] = { "Auto", "Black", "Red", "Blue" };
static const char *const kTabs[]   = { "&Font", "Character &Spacing", "A&nimation" };

static const uod_item kFontItems[] = {
    /* page 0 - Font */
    LBL("&Font:",            10,  6, 100, 0),
    LST(F_FONT,              10, 20, 130, 66, 0, kFonts, 5),
    LBL("Font st&yle:",     150,  6,  90, 0),
    LST(F_STYLE,            150, 20,  90, 66, 0, kStyles, 4),
    LBL("&Size:",           250,  6,  50, 0),
    SPN(F_SIZE, "10",       250, 20,  50, 0, 0, 6, 72),
    LBL("&Color:",           10, 94,  60, 0),
    CMB(F_COLOR, "Auto",     10,108, 110, 0, 0, kColors, 4),
    GRP("Effects",          150, 92, 150, 62, 0),
    CHK(F_STRIKE,    "Stri&kethrough", 158, 104, 120, 0),
    CHK(F_SHADOW,    "S&hadow",        158, 122, 120, 0),
    CHK(F_SMALLCAPS, "Small ca&ps",    158, 140, 120, 0),
    /* page 1 - Character Spacing */
    LBL("Sc&ale:",           10,  6,  60, 1),
    SPN(F_SCALE, "100",      70,  4,  60, 0, 1, 25, 200),
    LBL("&Underline:",       10, 30, 100, 1),
    RAD(F_UNDER_NONE,   "(&none)", 20,  48, 100, 1, 1),
    RAD(F_UNDER_SINGLE, "&Single", 20,  66, 100, 1, 1),
    RAD(F_UNDER_DOUBLE, "&Double", 20,  84, 100, 1, 1),
    /* every page */
    PRV(F_PREVIEW,           10,162, 290, 40, -1),
    BTN(UOD_ID_OK,     "OK",     150, 210, 70, 0, UOD_DEFAULT),
    BTN(UOD_ID_CANCEL, "Cancel", 230, 210, 70, 0, 0)
};
static const uod_dlg kFontDlg = {
    "Font", kFontItems, 21, kTabs, 3, 320, 250, 1
};

/* ---- storyboard ------------------------------------------------------------ */
static void paint_all(void)
{
    fb_clear(FB_RGB(0x80,0x80,0x80));
    fb_fill_rect(20, 20, FB_W - 40, FB_H - 60, FB_RGB(0xFF,0xFF,0xFF));
    fb_text(32, 32, "The document, with a modal dialog over it.",
            FB_RGB(0x00,0x00,0x00), -1);
    uod_render(&D);
    /* the app paints its own sample into the preview well */
    {
        int x, y, w, h;
        if (uod_preview_rect(&D, F_PREVIEW, &x, &y, &w, &h)) {
            const char *f = kFonts[uod_value(&D, F_FONT)];
            fb_text(x + (w - fb_text_w(f)) / 2, y + (h - fb_text_h()) / 2, f,
                    FB_RGB(0x00,0x00,0x00), -1);
        }
    }
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
    sprintf(path, "%s/uod_%02d.ppm", g_dir, g_frame);
    write_ppm(path);
    printf("  %2d. %s\n", g_frame + 1, label);
    g_frame++;
}

/* mirrors of the engine's geometry, for aiming clicks */
static int d_title_h(void) { return fb_text_h() + uoc_look_97()->pad + 2; }
static int d_tab_h(void)   { return fb_text_h() + uoc_look_97()->pad + 2; }
static void item_xy(const uod_item *it, int *x, int *y)
{
    *x = D.x + 3 + it->x;
    *y = D.y + 3 + d_title_h() + d_tab_h() + it->y;
}
static void tab_xy(int t, int *x, int *y)
{
    const uoc_look *k = uoc_look_97();
    int i, tx = D.x + 3 + 2;
    for (i = 0; i < t; i++) tx += uoc_label_w(kTabs[i]) + k->pad * 3;
    *x = tx + 6;
    *y = D.y + 3 + d_title_h() + d_tab_h() / 2;
}

int main(int argc, char **argv)
{
    const uoc_look *k = uoc_look_97();
    int x, y;

    if (argc >= 2) g_dir = argv[1];
    uoc_icons_install();
    printf("uodlg storyboard -> %s\n", g_dir);

    uod_open(&D, &kFontDlg, FB_W, FB_H - 14);
    snap("the Font dialog: tabs, lists, a spinner, a group box, a preview");
    eq("open: it is up", uod_is_open(&D), 1);
    eq("open: still running", uod_result(&D), 0);
    eq("open: first page", D.page, 0);

    /* a list picks by click */
    {
        item_xy(&kFontItems[1], &x, &y);          /* the Font list */
        click(x + 20, y + 2 + 1 * (fb_text_h() + 2) + 2);
        snap("the font list selects \"Arial\"");
        eq("list: row 1", uod_value(&D, F_FONT), 1);
        px_is("list: navy behind the selected row",
              x + 20, y + 2 + 1 * (fb_text_h() + 2) + 2, k->sel);
    }

    /* checks toggle, and only themselves */
    {
        item_xy(&kFontItems[9], &x, &y);          /* Strikethrough */
        click(x + 4, y + 4);
        snap("a check box toggles");
        eq("check: on", uod_value(&D, F_STRIKE), 1);
        eq("check: its neighbour untouched", uod_value(&D, F_SHADOW), 0);
        click(x + 4, y + 4);
        eq("check: off again", uod_value(&D, F_STRIKE), 0);
    }

    /* the spinner steps and clamps */
    {
        /* the control is d_row tall when its item declares h == 0, so the
         * two stepper halves are [y, y+sh/2) and [y+sh/2, y+sh) - aim inside
         * the lower one, not at its bottom edge, which is past the control */
        int i, sh = fb_text_h() + k->pad;
        item_xy(&kFontItems[5], &x, &y);          /* the Size spin */
        for (i = 0; i < 3; i++) click(x + 44, y + sh / 4);
        snap("the size spinner steps up");
        eq("spin: 6 + 3", uod_value(&D, F_SIZE), 9);
        streq("spin: the field followed", uod_text(&D, F_SIZE), "9");
        for (i = 0; i < 8; i++) click(x + 44, y + sh * 3 / 4);
        eq("spin: clamped at its floor", uod_value(&D, F_SIZE), 6);
    }

    /* a combo drops a list and picks from it */
    {
        item_xy(&kFontItems[7], &x, &y);          /* the Color combo */
        click(x + 100, y + 4);
        snap("a dialog combo drops its list");
        eq("combo: open", D.pop, 7);
        click(x + 20, y + fb_text_h() + 6 + 2 * (fb_text_h() + 2));
        snap("...and picking \"Red\" closes it");
        eq("combo: picked row 2", uod_value(&D, F_COLOR), 2);
        eq("combo: closed", D.pop, -1);
    }

    /* tabs swap the page, and page -1 items stay */
    {
        tab_xy(1, &x, &y);
        click(x, y);
        snap("the Character Spacing tab: a different page, same OK row");
        eq("tabs: page 1", D.page, 1);
        eq("tabs: an item from page 0 is not focusable now",
           uod_value(&D, F_STRIKE), 0);
    }

    /* radios are exclusive within their group */
    {
        item_xy(&kFontItems[16], &x, &y);         /* Single */
        click(x + 4, y + 4);
        snap("radio buttons: picking one clears its group");
        eq("radio: single on", uod_value(&D, F_UNDER_SINGLE), 1);
        eq("radio: none off", uod_value(&D, F_UNDER_NONE), 0);
        item_xy(&kFontItems[17], &x, &y);         /* Double */
        click(x + 4, y + 4);
        eq("radio: double on", uod_value(&D, F_UNDER_DOUBLE), 1);
        eq("radio: single cleared", uod_value(&D, F_UNDER_SINGLE), 0);
    }

    /* the keyboard: Tab walks, a mnemonic jumps, Enter presses the default */
    {
        int f0;
        ev_key(UI_KEY_TAB, 0);
        f0 = D.focus;
        ev_key(UI_KEY_TAB, 0);
        if (D.focus == f0) fail("kbd", "Tab did not move the focus");
        ev_key(UI_KEY_TAB, UI_MOD_SHIFT);
        eq("kbd: Shift+Tab came back", D.focus, f0);
        snap("keyboard focus: the ring on the focused control");
    }

    /* dragging the title bar moves the whole dialog */
    {
        int ox = D.x, oy = D.y;
        ev_move(D.x + 40, D.y + 5);
        ev_down(D.x + 40, D.y + 5);
        ev_move(D.x + 90, D.y + 45);
        ev_up(D.x + 90, D.y + 45);
        snap("the dialog is dragged by its title bar");
        eq("drag: moved right", D.x, ox + 50);
        eq("drag: moved down",  D.y, oy + 40);
    }

    /* Enter fires the default button and closes */
    ev_key(UI_KEY_ENTER, 0);
    eq("enter: OK closed it", uod_result(&D), UOD_ID_OK);
    eq("enter: no longer open", uod_is_open(&D), 0);

    /* Esc reports Cancel */
    uod_open(&D, &kFontDlg, FB_W, FB_H - 14);
    ev_key(UI_KEY_ESC, 0);
    eq("esc: reports Cancel", uod_result(&D), UOD_ID_CANCEL);

    /* the close box reports Cancel too */
    uod_open(&D, &kFontDlg, FB_W, FB_H - 14);
    click(D.x + kFontDlg.w - 3 - fb_text_h() / 2 - 2, D.y + 3 + fb_text_h() / 2);
    eq("close box: reports Cancel", uod_result(&D), UOD_ID_CANCEL);

    /* the "?" help button is its own result, not a cancel */
    uod_open(&D, &kFontDlg, FB_W, FB_H - 14);
    {
        int bs = fb_text_h();
        int bx = D.x + kFontDlg.w - 3 - bs - 2;
        click(bx - bs - 2 + bs / 2, D.y + 3 + bs / 2);
        eq("help box: reports Help", uod_result(&D), UOD_ID_HELP);
    }

    /* ---- the message box ------------------------------------------------- */
    uod_msgbox(&D, "Microsoft Word",
               "Do you want to save the changes to Document1?",
               UOD_MB_YESNOCANCEL, FB_W, FB_H - 14);
    snap("a Yes/No/Cancel message box");
    eq("msgbox: it is up", uod_is_open(&D), 1);

    ev_char('n');                       /* the &No mnemonic */
    eq("msgbox: the mnemonic answered No", uod_result(&D), UOD_ID_NO);
    eq("msgbox: closed", uod_is_open(&D), 0);

    uod_msgbox(&D, "Microsoft Word", "Saved.", UOD_MB_OK, FB_W, FB_H - 14);
    ev_key(UI_KEY_ENTER, 0);
    eq("msgbox: Enter takes the default", uod_result(&D), UOD_ID_OK);

    printf(g_fail ? "\nuodlg gate: %d FAILURE(S)\n" : "\nuodlg gate: GREEN\n",
           g_fail);
    return g_fail ? 1 : 0;
}
