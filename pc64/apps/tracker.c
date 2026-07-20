/* Tracker app module (APP_TRACKER).  Separate artifact -> app08.so.
   32-row x 4-channel pattern editor (Amiga/Genesis-parity format).  The full
   kernel build drives four square-wave Sound Manager channels; this MODULE
   plays through the single KernelApi synth channel (monophonic: the row's
   first active note), which keeps the editor + visuals identical and portable.
   Pattern persists as SONG.TRK on the FAT volume via the KernelApi. */
#include "uno_mod.h"

#define TK_ROWS  32
#define TK_CHANS 4
#define TK_VIEW  14
#define NLINE_H  14
#define TK_PATLEN (TK_ROWS * TK_CHANS * 2)

static unsigned char gTkPat[TK_PATLEN];
static short   gTkRow = 0, gTkCh = 0, gTkTop = 0, gTkPRow = 0;
static Boolean gTkPlaying = false;
static long    gTkLast = 0;

static const char kTkNoteNames[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
static const char kTkInstName[]  = "SSTN";

static const unsigned char kTkDemo[TK_PATLEN] = {
    1,1, 13,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 17,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    1,1, 20,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 17,0, 13,2,  0,0,  0,0, 0,0, 0,0, 0,0,
    8,1, 13,0, 17,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 15,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    8,1, 20,0, 15,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 15,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    6,1, 10,0, 13,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 13,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    6,1, 17,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 13,0, 10,2,  0,0,  0,0, 0,0, 0,0, 0,0,
    8,1, 11,0, 15,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 15,0,  0,0,  0,0,  0,0, 0,0, 0,0, 0,0,
    8,1, 20,0, 19,2, 20,3,  0,0, 0,0, 0,0, 0,0,
    0,0, 23,0,  0,0, 20,3,  0,0, 0,0, 0,0, 0,0
};

static unsigned char *tk_cell(short row, short ch)
{
    return &gTkPat[(row * TK_CHANS + ch) * 2];
}

/* play the row's first active note through the single KernelApi channel */
static void tk_trigger_row(short row)
{
    short ch;
    for (ch = 0; ch < TK_CHANS; ch++) {
        unsigned char *cell = tk_cell(row, ch);
        if (!cell[0]) continue;
        if (ch == 3) music_note_on((short)(36 + (cell[0] % 12)), 4);
        else         music_note_on((short)(59 + cell[0]), 6);
        return;
    }
}

static void tk_fmt_cell(const unsigned char *cell, char *out)
{
    if (!cell[0]) { strcpy(out, "--- -"); return; }
    {
        short n = (short)(cell[0] - 1);
        out[0] = kTkNoteNames[(n % 12) * 2];
        out[1] = kTkNoteNames[(n % 12) * 2 + 1];
        out[2] = (char)('2' + n / 12);
        out[3] = ' ';
        out[4] = kTkInstName[cell[1] & 3];
        out[5] = 0;
    }
}

static void tk_redraw(void);
static void tk_save(void);
static void tk_load(void);
static void tk_toggle_play(void);

/* on-screen toolbar so every action has a mouse-reachable control (not just a
 * key). Rects are laid out in tracker_draw and hit-tested in tracker_click. */
enum { TB_PLAY, TB_CLR, TB_DEMO, TB_SAVE, TB_LOAD, TB_NBTN };
static UiBtn gTkBtn[TB_NBTN];
static void tk_layout(Rect r)
{
    static const short ws[TB_NBTN] = { 56, 50, 56, 50, 50 };
    short x = (short)(r.left + 10), y = (short)(r.bottom - 24); int i;
    for (i = 0; i < TB_NBTN; i++) { gTkBtn[i].x = x; gTkBtn[i].y = y;
        gTkBtn[i].w = ws[i]; gTkBtn[i].h = 16; x = (short)(x + ws[i] + 4); }
}

static void tracker_draw(UnoWin *w)
{
    Rect r = w->bounds, ct = r;
    short x0 = (short)(r.left + 10), y0 = (short)(r.top + TBAR_H + 14), y, i, ch;
    char buf[8];

    ct.top += TBAR_H; InsetRect(&ct, 1, 1); uno_fill(&ct, C_BLUE);

    text_at(x0, y0, "Row", C_CYAN, C_BLUE, false);
    {
        static const char *chn[TK_CHANS] = { "Ch1", "Ch2", "Ch3", "Nz" };
        for (ch = 0; ch < TK_CHANS; ch++)
            text_at((short)(x0 + 44 + ch * 64), y0, chn[ch],
                    (short)(ch == gTkCh ? C_MAG : C_CYAN), C_BLUE, false);
    }
    if (gTkRow < gTkTop) gTkTop = gTkRow;
    if (gTkRow >= gTkTop + TK_VIEW) gTkTop = (short)(gTkRow - TK_VIEW + 1);
    for (i = 0; i < TK_VIEW; i++) {
        short row = (short)(gTkTop + i);
        y = (short)(y0 + 16 + i * NLINE_H);
        if (row == gTkRow) {
            Rect bar; SetRect(&bar, (short)(r.left + 4), (short)(y - 11),
                              (short)(r.right - 4), (short)(y + 3));
            uno_fill(&bar, C_CYAN);
        } else if (gTkPlaying && row == gTkPRow) {
            Rect bar; SetRect(&bar, (short)(r.left + 4), (short)(y - 11),
                              (short)(r.right - 4), (short)(y + 3));
            uno_fill(&bar, C_MAG);
        }
        put2(row, buf);
        text_at(x0, y, buf, (short)(row == gTkRow ? C_BLUE : C_WHITE), C_BLUE, false);
        for (ch = 0; ch < TK_CHANS; ch++) {
            tk_fmt_cell(tk_cell(row, ch), buf);
            text_at((short)(x0 + 44 + ch * 64), y, buf,
                    (short)(row == gTkRow ? C_BLUE : C_WHITE), C_BLUE, false);
        }
    }
    text_at(x0, (short)(r.bottom - 40), "arrows: move   q/w: note   e: inst",
            C_CYAN, C_BLUE, false);
    tk_layout(r);                             /* toolbar: clickable actions */
    ui_button(&gTkBtn[TB_PLAY], gTkPlaying ? "Stop" : "Play", gTkPlaying);
    ui_button(&gTkBtn[TB_CLR],  "Clear", false);
    ui_button(&gTkBtn[TB_DEMO], "Demo",  false);
    ui_button(&gTkBtn[TB_SAVE], "Save",  false);
    ui_button(&gTkBtn[TB_LOAD], "Load",  false);
}

static void tracker_click(UnoWin *w, Point p)
{
    unsigned char *cell = tk_cell(gTkRow, gTkCh); (void)w;
    if      (ui_hit(&gTkBtn[TB_PLAY], p)) tk_toggle_play();
    else if (ui_hit(&gTkBtn[TB_CLR],  p)) { cell[0] = 0; cell[1] = 0; }
    else if (ui_hit(&gTkBtn[TB_DEMO], p)) memcpy(gTkPat, kTkDemo, TK_PATLEN);
    else if (ui_hit(&gTkBtn[TB_SAVE], p)) tk_save();
    else if (ui_hit(&gTkBtn[TB_LOAD], p)) tk_load();
    else return;
    tk_redraw();
}

static void tk_redraw(void)
{
    UnoWin *w = find_app_window(APP_TRACKER);
    if (w) draw_window(w);
}

static void tk_save(void){ fat12_write("SONG.TRK", gTkPat, TK_PATLEN); fat12_list(); }
static void tk_load(void)
{
    int i;
    fat12_read("SONG.TRK", gTkPat, TK_PATLEN);
    /* a corrupt SONG.TRK can hold note bytes far outside the 1..24 range the
       editor enforces; drop out-of-range notes so tk_trigger_row never
       over-drives music_note_on (0 = empty cell) */
    for (i = 0; i < TK_PATLEN; i += 2)
        if (gTkPat[i] > 24) gTkPat[i] = 0;
}
static void tk_stop(void){ gTkPlaying = false; music_quiet(); }
static void tk_toggle_play(void)
{
    if (gTkPlaying) tk_stop();
    else { gm_stop(); music_open_chan();
           gTkPlaying = true; gTkPRow = TK_ROWS - 1; gTkLast = TickCount() - 6; }
}

static void tracker_tick(void)
{
    UnoWin *w;
    if (!gTkPlaying) return;
    if (TickCount() - gTkLast < 6) return;
    gTkLast = TickCount();
    gTkPRow++;
    if (gTkPRow >= TK_ROWS) gTkPRow = 0;
    tk_trigger_row(gTkPRow);
    w = find_app_window(APP_TRACKER);
    if (w) draw_window(w);
}

static Boolean tracker_key(char ch, short code, Boolean cmd)
{
    unsigned char *cell = tk_cell(gTkRow, gTkCh);
    if (cmd) return false;
    if (code == 0x7E || ch == 0x1E) { if (gTkRow > 0) gTkRow--; }
    else if (code == 0x7D || ch == 0x1F) { if (gTkRow < TK_ROWS - 1) gTkRow++; }
    else if (code == 0x7B || ch == 0x1C) { if (gTkCh > 0) gTkCh--; }
    else if (code == 0x7C || ch == 0x1D) { if (gTkCh < TK_CHANS - 1) gTkCh++; }
    else if (ch == 'q') { if (!cell[0]) cell[0] = 1; else if (cell[0] > 1) cell[0]--;
                          music_open_chan(); tk_trigger_row(gTkRow); }
    else if (ch == 'w') { if (!cell[0]) cell[0] = 1; else if (cell[0] < 24) cell[0]++;
                          music_open_chan(); tk_trigger_row(gTkRow); }
    else if (ch == 'e') { if (cell[0]) cell[1] = (unsigned char)((cell[1] + 1) & 3); }
    else if (ch == 'x') { cell[0] = 0; cell[1] = 0; }
    else if (ch == 'd') { memcpy(gTkPat, kTkDemo, TK_PATLEN); }
    else if (ch == 's') { tk_save(); }
    else if (ch == 'l') { tk_load(); }
    else if (ch == ' ') { tk_toggle_play(); }
    else return false;
    tk_redraw();
    return true;
}

static void tracker_closed(void){ tk_stop(); }

static const AppInterface kIface = {
    tracker_draw, tracker_key, tracker_click, tracker_tick, 0, tracker_closed,
    "Tracker", { 26, 30, 486, 326 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
