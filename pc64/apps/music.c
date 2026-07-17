/* Music app module (APP_MUSIC).  Separate artifact -> app04.so.
   Owns the song tables + the playback sequencer; drives the kernel's square-
   wave synth through the KernelApi primitives (music_open_chan/note_on/quiet).
   Same arrangement as the x86 apps/music.asm. */
#include "uno_mod.h"

#define QN 30                       /* quarter note, ticks (60 Hz) */
#define EN 15                       /* eighth note */
#define HN 60                       /* half note */
#define DQ 45                       /* dotted quarter */

/* MIDI: C4=60 D=62 E=64 F=65 G=67 G#=68 A=69 B=71 C5=72 D5=74 E5=76 F5=77 */
static const Note kCanon[] = {
    {72,QN},{71,QN},{69,QN},{67,QN}, {65,QN},{64,QN},{65,QN},{67,QN},
    {72,EN},{76,EN},{71,EN},{74,EN}, {69,EN},{72,EN},{67,EN},{71,EN},
    {65,EN},{69,EN},{64,EN},{67,EN}, {65,EN},{69,EN},{67,EN},{71,EN},
};
static const Note kOde[] = {
    {64,QN},{64,QN},{65,QN},{67,QN},{67,QN},{65,QN},{64,QN},{62,QN},
    {60,QN},{60,QN},{62,QN},{64,QN},{64,DQ},{62,EN},{62,HN},
};
static const Note kTwinkle[] = {
    {60,QN},{60,QN},{67,QN},{67,QN},{69,QN},{69,QN},{67,HN},
    {65,QN},{65,QN},{64,QN},{64,QN},{62,QN},{62,QN},{60,HN},
};
static const Note kGreen[] = {
    {69,QN},{72,QN},{74,QN},{76,DQ},{77,EN},{76,QN},{74,QN},{71,QN},
    {67,DQ},{69,EN},{71,QN},{72,QN},{69,QN},{69,DQ},{68,EN},{69,QN},
    {71,HN},{68,QN},{64,HN},
};
static const Note kJingle[] = {
    {64,QN},{64,QN},{64,HN},{64,QN},{64,QN},{64,HN},
    {64,QN},{67,QN},{60,DQ},{62,EN},{64,HN},
    {65,QN},{65,QN},{65,DQ},{65,EN},{65,QN},{64,QN},{64,QN},{64,EN},{64,EN},
    {67,QN},{67,QN},{65,QN},{62,QN},{60,HN},
};
static const Note kSaints[] = {
    {60,QN},{64,QN},{65,QN},{67,HN},{60,QN},{64,QN},{65,QN},{67,HN},
    {60,QN},{64,QN},{65,QN},{67,QN},{64,QN},{60,QN},{64,QN},{62,HN},
    {64,QN},{64,QN},{62,QN},{60,QN},{67,HN},
};
static const Note kMary[] = {
    {64,QN},{62,QN},{60,QN},{62,QN},{64,QN},{64,QN},{64,HN},
    {62,QN},{62,QN},{62,HN},{64,QN},{67,QN},{67,HN},
    {64,QN},{62,QN},{60,QN},{62,QN},{64,QN},{64,QN},{64,QN},
    {64,QN},{62,QN},{62,QN},{64,QN},{62,QN},{60,HN},
};
static const Note kAmazing[] = {
    {67,QN},{72,HN},{76,QN},{72,QN},{76,HN},{74,QN},{72,HN},{69,QN},
    {67,HN},{72,QN},{76,HN},{74,QN},{72,QN},{69,QN},{67,HN},
};
#define NS(a) (short)(sizeof(a)/sizeof(a[0]))
static const Song kSongs[] = {
    { kCanon,   NS(kCanon),   "Canon in D  (Pachelbel)"    },
    { kOde,     NS(kOde),     "Ode to Joy  (Beethoven)"    },
    { kTwinkle, NS(kTwinkle), "Twinkle Twinkle  (Mozart)"  },
    { kGreen,   NS(kGreen),   "Greensleeves  (Traditional)"},
    { kJingle,  NS(kJingle),  "Jingle Bells  (Pierpont)"   },
    { kSaints,  NS(kSaints),  "When the Saints  (Trad.)"   },
    { kMary,    NS(kMary),    "Mary Had a Little Lamb"     },
    { kAmazing, NS(kAmazing), "Amazing Grace  (Trad.)"     },
};
#define NSONGS (short)(sizeof(kSongs)/sizeof(kSongs[0]))
#define CURNOTES (kSongs[gSong].notes)
#define CURCOUNT (kSongs[gSong].count)

static Boolean gPlaying = false;
static short   gNoteIx = 0;
static long    gNoteEnd = 0;
static short   gSong = 0;

static void music_play(void);
static void music_halt(void);
static void music_select(short s);

/* on-screen toolbar (mouse-reachable Play/Stop + song nav, not just keys) */
enum { MB_PLAY, MB_PREV, MB_NEXT, MB_NBTN };
static UiBtn gMuBtn[MB_NBTN];
static void mu_layout(Rect r)
{
    static const short ws[MB_NBTN] = { 56, 50, 50 };
    short x = (short)(r.left + 8), y = (short)(r.bottom - 24); int i;
    for (i = 0; i < MB_NBTN; i++) { gMuBtn[i].x = x; gMuBtn[i].y = y;
        gMuBtn[i].w = ws[i]; gMuBtn[i].h = 16; x = (short)(x + ws[i] + 4); }
}

static void music_draw(UnoWin *w)
{
    Rect r = w->bounds;
    short x0 = r.left + 12, y;
    short staffTop = r.top + TBAR_H + 28;
    short i;
    Rect ct = r;

    ct.top += TBAR_H; InsetRect(&ct, 1, 1);
    uno_fill(&ct, C_BLUE);
    text_at(r.left + 8, r.top + TBAR_H + 14, kSongs[gSong].title, C_WHITE, C_BLUE, false);

    for (i = 0; i < 5; i++) {
        y = staffTop + 14 + i * 8;
#if UNO_COLOR
        RGBForeColor(&kPalette[C_WHITE]);
#else
        ForeColor(blackColor);
#endif
        MoveTo(x0, y); LineTo(r.right - 12, y);
#if UNO_COLOR
        RGBForeColor(&kBlack);
#endif
    }
    for (i = 0; i < CURCOUNT; i++) {
        Rect nr;
        short nx = x0 + 4 + i * ((r.right - r.left - 32) / CURCOUNT);
        short ny = staffTop + 46 - (CURNOTES[i].midi - 60) * 2;
        SetRect(&nr, nx, ny - 3, nx + 6, ny + 3);
        if (gPlaying && i == gNoteIx) uno_fill(&nr, C_MAG);
        else                          uno_fill(&nr, C_CYAN);
    }
    mu_layout(r);                             /* clickable toolbar */
    ui_button(&gMuBtn[MB_PLAY], gPlaying ? "Stop" : "Play", gPlaying);
    ui_button(&gMuBtn[MB_PREV], "Prev", false);
    ui_button(&gMuBtn[MB_NEXT], "Next", false);
}

static void music_click(UnoWin *w, Point p)
{
    (void)w;
    if      (ui_hit(&gMuBtn[MB_PLAY], p)) { if (gPlaying) music_halt(); else music_play(); }
    else if (ui_hit(&gMuBtn[MB_PREV], p)) music_select((short)(gSong - 1));
    else if (ui_hit(&gMuBtn[MB_NEXT], p)) music_select((short)(gSong + 1));
    else return;
    { UnoWin *ww = find_app_window(APP_MUSIC); if (ww) draw_window(ww); }
}

static void music_play(void)
{
    music_open_chan();
    gPlaying = true;
    gNoteIx = 0;
    gNoteEnd = TickCount() + CURNOTES[0].dur;
    music_note_on(CURNOTES[0].midi, CURNOTES[0].dur);
}

static void music_halt(void)
{
    gPlaying = false;
    music_quiet();
}

static void music_tick(void)
{
    UnoWin *w;
    if (!gPlaying) return;
    if (TickCount() < gNoteEnd) return;
    gNoteIx++;
    if (gNoteIx >= CURCOUNT) gNoteIx = 0;
    gNoteEnd = TickCount() + CURNOTES[gNoteIx].dur;
    music_note_on(CURNOTES[gNoteIx].midi, CURNOTES[gNoteIx].dur);
    w = find_app_window(APP_MUSIC);
    if (w) draw_window(w);
}

static void music_select(short s)
{
    Boolean wasPlaying = gPlaying;
    if (s < 0) s = NSONGS - 1;
    if (s >= NSONGS) s = 0;
    gSong = s;
    music_halt();
    gNoteIx = 0;
    if (wasPlaying) music_play();
}

static Boolean music_key(char ch, short code)
{
    UnoWin *w;
    (void)code;
    if (ch == ' ') { if (gPlaying) music_halt(); else music_play(); }
    else if (ch == ',' || ch == '<') music_select(gSong - 1);
    else if (ch == '.' || ch == '>') music_select(gSong + 1);
    else if (ch >= '1' && ch <= '0' + NSONGS) music_select(ch - '1');
    else return false;
    w = find_app_window(APP_MUSIC);
    if (w) draw_window(w);
    return true;
}

static Boolean music_key_w(char ch, short code, Boolean cmd){ if (cmd) return false; return music_key(ch, code); }
static void music_opened(void){ music_open_chan(); }
static void music_closed(void){ music_halt(); }

static const AppInterface kIface = {
    music_draw, music_key_w, music_click, music_tick, music_opened, music_closed,
    "Music", { 80, 60, 440, 230 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
