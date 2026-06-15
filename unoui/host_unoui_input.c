/* ===========================================================================
 * unoui interactive host harness - drive the write-once app with a SCRIPTED
 * abstract event stream and snapshot the resulting states. Because all toolkit
 * behaviour is a pure function of unoui_event, this storyboard is exactly what
 * any port would produce from the same gestures - proof that "write once" also
 * means "behaves the same everywhere". Renders the live UI to PPM frames; the
 * last two frames re-skin the identical live state under other themes to show
 * input + theming compose.
 *
 *   ./host_unoui_input <out_dir>
 * ======================================================================== */
#include "unoui_theme.h"
#include "unoui_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fb is provided by ../ps2/fb.c which we link in. */

static unoui_ui  UI;
static unoui_window ED, PAL;
static int   g_frame = 0;
static char  g_labels[32][48];

/* ---- event helpers ------------------------------------------------------- */
static void ev_move(int x,int y)     { unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_MOVE; e.x=x; e.y=y; unoui_handle(&UI,&e); }
static void ev_down(int x,int y)     { unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y; unoui_handle(&UI,&e); }
static void ev_up(int x,int y)       { unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_UP;   e.x=x; e.y=y; unoui_handle(&UI,&e); }
static void ev_char(int c)           { unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_CHAR; e.ch=c; unoui_handle(&UI,&e); }
static void ev_key(int k,int mods)   { unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_KEY; e.key=k; e.mods=mods; unoui_handle(&UI,&e); }

static void click(int x,int y)       { ev_move(x,y); ev_down(x,y); ev_up(x,y); }
static void type_str(const char *s)  { for (; *s; s++) { if (*s=='\n') ev_key(UI_KEY_ENTER,0); else ev_char((unsigned char)*s); } }
static void drag(int x0,int y0,int x1,int y1)
{ ev_move(x0,y0); ev_down(x0,y0); ev_move((x0+x1)/2,(y0+y1)/2); ev_move(x1,y1); ev_up(x1,y1); }

/* ---- snapshot ------------------------------------------------------------ */
static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb"); int i, n = FB_W*FB_H;
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < n; i++) { unsigned p = fb[i];
        unsigned char rgb[3] = { p & 0xFF, (p>>8) & 0xFF, (p>>16) & 0xFF };
        fwrite(rgb,1,3,f); }
    fclose(f);
}

static void snap(const char *dir, const char *label)
{
    char path[256], cap[64];
    UI.ticks = 0;                          /* keep the caret visible in stills */
    unoui_render_ui(&UI);
    fb_fill_rect(0, 0, FB_W, 13, FB_RGB(0x10,0x10,0x10));
    fb_hline(0, 13, FB_W, FB_RGB(0x80,0x80,0x80));
    sprintf(cap, "%d. %s", g_frame + 1, label);
    fb_text(6, 3, cap, FB_RGB(0xFF,0xFF,0xFF), -1);
    sprintf(path, "%s/in_%02d.ppm", dir, g_frame);
    write_ppm(path);
    strncpy(g_labels[g_frame], label, 47);
    printf("frame %d: %s\n", g_frame + 1, label);
    g_frame++;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "build";

    unoui_ui_init(&UI, &theme_unodos, FB_W, FB_H);
    demo_app_build(&ED, &PAL);
    unoui_ui_add(&UI, &ED);
    unoui_ui_add(&UI, &PAL);          /* PAL on top initially */

    snap(dir, "initial: two windows");

    /* menus: open File, then pick an item */
    ev_down(50, 49);                  /* press the File title -> popup opens   */
    snap(dir, "menubar: File menu open");
    ev_down(50, 75);                  /* click 'Open...' in the popup          */
    ev_up(50, 75);
    snap(dir, "menubar: item chosen, popup closed");

    /* multi-line text entry into the body */
    click(80, 120);                   /* focus the textarea                    */
    type_str("Hello, UnoDOS!\nMulti-line editing\nworks the same everywhere.");
    snap(dir, "textarea: typed multi-line text + caret");

    /* selection via Shift+Left (select the last word) */
    { int i; for (i = 0; i < 11; i++) ev_key(UI_KEY_LEFT, UI_MOD_SHIFT); }
    snap(dir, "textarea: shift-arrow selection");

    /* single-line edit: focus, End, type */
    click(110, 212);
    ev_key(UI_KEY_END, 0);
    type_str("-doc");
    snap(dir, "field: edited single line");

    /* controls: drag slider, toggle checkbox, bump spinner */
    drag(120, 238, 215, 238);         /* slider toward max                     */
    click(153, 261);                  /* toggle 'Dark mode'                    */
    click(385, 233); click(385, 233); /* spinner up x2                         */
    snap(dir, "slider drag + checkbox + spinner");

    /* tabs + dropdown */
    click(110, 72);                   /* select the 'Files' tab                */
    ev_down(340, 212);                /* open the format dropdown              */
    snap(dir, "tabs switched + dropdown open");
    ev_down(330, 244); ev_up(330, 244);   /* pick 'Markdown'                   */
    snap(dir, "dropdown choice committed");

    /* list selection (click 'themes.dat', a few rows down) */
    click(300, 300);
    snap(dir, "list: row selected");

    /* drag the Tools window (also proves z-order: it was on top) */
    drag(500, 79, 360, 150);
    snap(dir, "window dragged to a new position");

    /* button press visual + release */
    ev_move(120+38, 300+52); ev_down(120+38, 300+52);
    snap(dir, "OK button held (pressed state)");
    ev_up(120+38, 300+52);

    /* SAME live state, re-skinned: input + theming compose */
    unoui_ui_theme(&UI, &theme_win31);
    snap(dir, "identical state under Windows 3.1 theme");
    unoui_ui_theme(&UI, &theme_macplus);
    snap(dir, "identical state under Mac Plus theme (1-bit)");

    printf("storyboard: %d frames driven by one scripted event stream.\n", g_frame);
    return 0;
}
