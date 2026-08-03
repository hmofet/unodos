/* ===========================================================================
 * uobars_test - the host gate for phase 6d: the status bar, the ruler, the
 * Assistant, and the Open / Save As dialog (OFFICE97-PLAN §5).
 *
 * The file dialog is the interesting one to gate, because it is the first
 * piece of the suite that touches a filesystem - and it does so through a
 * seam, so this harness hands it a FAKE one and never boots the OS.  Same
 * trick as unodoc's ud_src.
 * ======================================================================== */
#include "uobars.h"
#include "uofile.h"
#include "uoicons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_frame, g_fail;
static const char *g_dir = "build";

static void fail(const char *what, const char *d)
{ printf("  FAIL %s: %s\n", what, d); g_fail++; }
static void eq(const char *what, int got, int want)
{ if (got != want) { char b[128]; sprintf(b, "got %d, wanted %d", got, want);
                     fail(what, b); } }
static void streq(const char *what, const char *got, const char *want)
{ if (strcmp(got, want)) { char b[160];
    sprintf(b, "got \"%s\", wanted \"%s\"", got, want); fail(what, b); } }

/* ---- the fake filesystem --------------------------------------------------- */
static const char *const kVolNames[3] = { "C: (hard disk)", "A: (floppy)",
                                          "D: (network)" };
static const char *const kVolC[5] = { "REPORT.DOC", "BUDGET.XLS", "DECK.PPT",
                                      "LETTERS", "NOTES.TXT" };
static const char *const kVolA[2] = { "BACKUP.DOC", "OLD" };
static int  fs_volumes(void) { return 3; }
static const char *fs_vname(int v) { return (v >= 0 && v < 3) ? kVolNames[v] : ""; }
static int  fs_begin(int v) { return v == 0 ? 5 : (v == 1 ? 2 : 0); }
static int  fs_get(int v, int i, char *nm, int cap)
{
    const char *const *t = (v == 0) ? kVolC : kVolA;
    int n = fs_begin(v), j = 0;
    if (i < 0 || i >= n) return 0;
    while (t[i][j] && j < cap - 1) { nm[j] = t[i][j]; j++; }
    nm[j] = 0;
    return 1;
}
static int fs_isdir(int v, const char *nm)
{ (void)v; return strcmp(nm, "LETTERS") == 0 || strcmp(nm, "OLD") == 0; }
static const uof_fs kFakeFs = { fs_volumes, fs_vname, fs_begin, fs_get, fs_isdir };

/* ---- the storyboard -------------------------------------------------------- */
static uob_status  ST;
static uob_ruler   RU;
static uob_assist  AS;
static uod_ui      D;
static int         g_show_dlg;

#define RULER_Y 30
#define STATUS_Y (FB_H - 14 - 20)

static void paint_all(void)
{
    fb_clear(FB_RGB(0x80,0x80,0x80));
    uob_ruler_render(&RU, 0, RULER_Y, FB_W);
    fb_fill_rect(0, RULER_Y + uob_ruler_h(), FB_W,
                 STATUS_Y - RULER_Y - uob_ruler_h(), FB_RGB(0xFF,0xFF,0xFF));
    fb_text(40, RULER_Y + uob_ruler_h() + 12,
            "The document, between the ruler and the status bar.",
            FB_RGB(0,0,0), -1);
    uob_status_render(&ST, 0, STATUS_Y, FB_W);
    if (g_show_dlg) uod_render(&D);
    uob_assist_render(&AS);
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
    sprintf(path, "%s/uob_%02d.ppm", g_dir, g_frame);
    write_ppm(path);
    printf("  %2d. %s\n", g_frame + 1, label);
    g_frame++;
}

/* events, routed to whichever piece is under test */
static void r_down(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y;
  uob_ruler_handle(&RU, &e, 0, RULER_Y, FB_W); }
static void r_move(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_MOVE; e.x=x; e.y=y;
  uob_ruler_handle(&RU, &e, 0, RULER_Y, FB_W); }
static void r_up(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_UP; e.x=x; e.y=y;
  uob_ruler_handle(&RU, &e, 0, RULER_Y, FB_W); }
static void a_down(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y;
  uob_assist_handle(&AS, &e); }
static void a_move(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_MOVE; e.x=x; e.y=y;
  uob_assist_handle(&AS, &e); }
static void a_up(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_UP; e.x=x; e.y=y;
  uob_assist_handle(&AS, &e); }
static void d_down(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_DOWN; e.x=x; e.y=y;
  uod_handle(&D, &e); uof_sync(&D); }
static void d_up(int x, int y)
{ unoui_event e; memset(&e,0,sizeof e); e.kind=UI_EV_MOUSE_UP; e.x=x; e.y=y;
  uod_handle(&D, &e); uof_sync(&D); }
static void d_click(int x, int y) { d_down(x, y); d_up(x, y); }

static const char *const kTopics[4] = {
    "Create a new document",
    "Open an existing document",
    "Change the page margins",
    "See more..."
};
static const char *const kTypes[3] = {
    "Word Document (*.doc)", "Rich Text Format (*.rtf)", "Text Only (*.txt)"
};

int main(int argc, char **argv)
{
    const uoc_look *k = uoc_look_97();
    int tx;

    if (argc >= 2) g_dir = argv[1];
    uoc_icons_install();
    uof_set_fs(&kFakeFs);
    printf("uobars storyboard -> %s\n", g_dir);

    /* ---- the status bar --------------------------------------------------- */
    ST.page = "Page 1    Sec 1     1/1";
    ST.pos  = "At 2.5cm   Ln 3   Col 12";
    ST.rec = 0; ST.trk = 0; ST.ext = 0; ST.ovr = 0;
    uob_ruler_init(&RU, 40, FB_W - 120);
    snap("the ruler and the status bar, all four mode cells greyed");
    {
        int cx = 0, cy = STATUS_Y + uob_status_h() / 2;
        /* walk left from the book until a cell answers, then check the map */
        int found = 0, x;
        for (x = FB_W - 1; x > FB_W / 2; x--)
            if (uob_status_hit(0, STATUS_Y, FB_W, x, cy) == UOB_CELL_OVR)
                { found = 1; cx = x; break; }
        eq("status: the OVR cell is hit-testable", found, 1);
        eq("status: and it really is OVR",
           uob_status_hit(0, STATUS_Y, FB_W, cx, cy), UOB_CELL_OVR);
        eq("status: the middle is not a cell",
           uob_status_hit(0, STATUS_Y, FB_W, 40, cy), UOB_CELL_NONE);
    }
    ST.trk = 1; ST.ovr = 1;
    ST.spell_errors = 1;
    snap("TRK and OVR active, and the spelling book flags an error");

    /* ---- the ruler -------------------------------------------------------- */
    tx = uoc_look_97()->icon_px + 2 + RU.text_x;
    {
        /* drag the first-line indent to the right */
        r_down(tx + 2, RULER_Y + 4);
        r_move(tx + 60, RULER_Y + 4);
        r_up(tx + 60, RULER_Y + 4);
        snap("the ruler: the first-line indent dragged out to 60");
        eq("ruler: first-line indent moved", RU.first, 60);
        eq("ruler: the hanging indent stayed", RU.hang, 0);
    }
    {
        /* the square under the hanging marker moves BOTH */
        int before_gap = RU.first - RU.hang;
        r_down(tx + 2, RULER_Y + uob_ruler_h() - 3);
        r_move(tx + 30, RULER_Y + uob_ruler_h() - 3);
        r_up(tx + 30, RULER_Y + uob_ruler_h() - 3);
        snap("the ruler: the left square drags both markers together");
        eq("ruler: hanging followed", RU.hang, 30);
        eq("ruler: the gap is preserved", RU.first - RU.hang, before_gap);
    }
    {
        /* the selector cycles the tab type, and a click sets a stop */
        r_down(4, RULER_Y + uob_ruler_h() / 2);
        eq("ruler: the selector cycled to centre", RU.pick, UOB_TAB_CENTER);
        r_down(tx + 200, RULER_Y + uob_ruler_h() / 2);
        r_up(tx + 200, RULER_Y + uob_ruler_h() / 2);
        snap("the ruler: a centre tab stop set by clicking the ruler");
        eq("ruler: one tab stop", RU.ntab, 1);
        eq("ruler: at 200", RU.tab[0], 200);
        eq("ruler: of the selected type", RU.tabtype[0], UOB_TAB_CENTER);
    }

    /* ---- the Assistant ---------------------------------------------------- */
    uob_assist_open(&AS, 460, 250);
    snap("the Assistant \"Uno\": our own character, no balloon yet");
    eq("assistant: open", AS.open, 1);
    eq("assistant: quiet", AS.balloon, 0);

    a_down(460 + 20, 250 + 20);
    a_up(460 + 20, 250 + 20);
    uob_assist_ask(&AS, kTopics, 4);
    snap("clicking it opens the balloon: query box and numbered answers");
    eq("assistant: balloon up", AS.balloon, 1);

    {
        /* hovering an answer highlights it, and clicking takes it */
        int bx, by, bw, bh, ty;
        bw = 0; (void)bw;
        bx = AS.x; by = AS.y; bh = 0; (void)bx; (void)by; (void)bh;
        ty = 0;
        /* walk the balloon looking for the second topic's row */
        {
            int found = -1, yy;
            for (yy = 2; yy < FB_H - 20 && found < 0; yy++) {
                unoui_event e;
                memset(&e, 0, sizeof e);
                e.kind = UI_EV_MOUSE_MOVE; e.x = AS.x + 10; e.y = yy;
                uob_assist_handle(&AS, &e);
                if (AS.hot == 1) found = yy;
            }
            eq("assistant: a topic row is hoverable", found >= 0, 1);
            ty = found;
        }
        a_move(AS.x + 10, ty);
        snap("hovering the second answer highlights it");
        eq("assistant: the second topic is hot", uob_assist_taken(&AS), 1);
    }

    /* ---- the Open dialog over a fake filesystem --------------------------- */
    uob_assist_close(&AS);
    g_show_dlg = 1;
    uof_open(&D, 0, kTypes, 3, FB_W, FB_H - 14);
    uof_sync(&D);
    snap("the Open dialog: Look in, the file list, name and type");
    eq("file: it is up", uod_is_open(&D), 1);
    eq("file: volume 0 to start", uof_volume(), 0);

    {
        /* DIRECTORIES SORT FIRST and wear a trailing backslash, so the fake
         * volume's five entries come back as LETTERS\, REPORT.DOC,
         * BUDGET.XLS, DECK.PPT, NOTES.TXT - not in the order the filesystem
         * handed them over.  Both halves of that are asserted here, because
         * the first version of this test quietly assumed raw order. */
        int x = D.x + 3 + 8 + 20;
        int y = D.y + 3 + (fb_text_h() + k->pad + 2) + 26 + 2;
        int lh = fb_text_h() + 2;

        d_click(x, y + 0 * lh);
        eq("file: row 0 is the directory", uod_value(&D, 1002), 0);
        /* ...and because row 0 IS the directory, the field is still empty:
         * a folder is somewhere to go, not a file name to open */
        streq("file: picking a folder does NOT fill the name field",
              uod_text(&D, 1003), "");

        d_click(x, y + 2 * lh);
        snap("picking BUDGET.XLS mirrors it into the File name field");
        eq("file: row 2 selected", uod_value(&D, 1002), 2);
        streq("file: the name field followed", uod_text(&D, 1003), "BUDGET.XLS");
    }

    {
        /* switch volumes: the list must repopulate */
        int cx = D.x + 3 + 70 + 140, cy = D.y + 3 + (fb_text_h() + k->pad + 2) + 8;
        d_click(cx, cy);                                    /* drop it       */
        d_click(D.x + 3 + 80, cy + fb_text_h() + 6 + (fb_text_h() + 2));
        uof_sync(&D);
        snap("switching to A: repopulates the list from the filesystem seam");
        eq("file: volume 1", uof_volume(), 1);
        eq("file: two entries there", uod_value(&D, 1002), 0);
    }

    {
        /* Open reports the chosen name */
        unoui_event e;
        memset(&e, 0, sizeof e);
        e.kind = UI_EV_KEY; e.key = UI_KEY_ENTER;
        uod_handle(&D, &e);
        uof_sync(&D);
        eq("file: OK closed it", uod_result(&D), UOD_ID_OK);
        eq("file: type index reported", uof_type(), 0);
    }

    printf(g_fail ? "\nuobars gate: %d FAILURE(S)\n" : "\nuobars gate: GREEN\n",
           g_fail);
    return g_fail ? 1 : 0;
}
