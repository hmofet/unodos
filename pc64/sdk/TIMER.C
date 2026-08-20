/* TIMER.C - a kitchen timer and stopwatch for the UnoDOS desktop.
 *
 * Space starts and pauses.  + and - (or the Up/Down arrows) dial the
 * countdown a minute at a time, M switches between countdown and
 * stopwatch, N resets.  When the countdown reaches zero the window
 * flashes and beeps until you press any key.
 *
 * What it demonstrates, beyond SAMPLE.C: keeping time by counting your
 * own tick() calls (the shell ticks you ~60 times a second - and that,
 * not TickCount(), is the platform's timebase: TickCount() counts CALLS,
 * shared by every app, so pace on your own frames), formatting numbers
 * with put2() - there is no printf - playing notes with music_note_on(),
 * and the repaint_all() redraw idiom.  Build with Ctrl-B, run with
 * Ctrl-R.
 */
#include "UNO.H"

#define BAR_W     240
#define MAX_MIN   95
#define START_T   18000              /* 5 minutes, in 60 Hz frames */
#define CAP_T     359940             /* put2 shows 2 digits: 99:59 */

static long  set_frames = START_T;   /* the dialled countdown duration */
static long  left       = START_T;   /* countdown frames remaining     */
static long  elapsed;                /* stopwatch frames accumulated   */
static Boolean running   = false;
static Boolean stopwatch  = false;   /* false = countdown mode         */
static long  ring_frames;            /* nonzero while the alarm rings  */
static long  beep_in;                /* frames until the next beep     */
static long  shown = -1;             /* last second painted            */
static Boolean flash;

static const GameRGB kFace   = {  24,  26,  40, C_BLUE };
static const GameRGB kRing   = { 200,  40,  40, C_MAG  };
static const GameRGB kBarOn  = {  90, 210, 120, C_CYAN };
static const GameRGB kBarOff = {  46,  50,  72, C_BLUE };

static void timer_draw(UnoWin *w)
{
    Rect r;
    char t[8];
    short x0 = w->bounds.left + 14;
    short y0 = w->bounds.top + TBAR_H + 10;
    long frames_now = stopwatch ? elapsed : left;
    /* a countdown rounds up (1 frame left still reads 0:01); a stopwatch
     * rounds down, like every stopwatch you have ever held */
    long secs = stopwatch ? frames_now / 60 : (frames_now + 59) / 60;
    short fill;

    SetRect(&r, x0 - 4, y0 - 4, x0 + BAR_W + 4, y0 + 96);
    fill_rgb(&r, (ring_frames && flash) ? &kRing : &kFace);
    uno_box(&r, C_WHITE);

    text_at(x0, y0 + 6, stopwatch ? "STOPWATCH" : "COUNTDOWN",
            C_CYAN, C_BLUE, false);
    put2(secs / 60, t);
    t[2] = ':';
    put2(secs % 60, t + 3);
    text_at(x0 + 170, y0 + 6, t, C_WHITE, C_BLUE, false);

    /* the bar drains as the countdown runs */
    SetRect(&r, x0, y0 + 28, x0 + BAR_W, y0 + 44);
    fill_rgb(&r, &kBarOff);
    if (!stopwatch && set_frames > 0) {
        fill = (short)((long)BAR_W * (left / 60) / (set_frames / 60));
        if (fill > 0) {
            SetRect(&r, x0, y0 + 28, x0 + fill, y0 + 44);
            fill_rgb(&r, &kBarOn);
        }
    }

    text_at(x0, y0 + 56, running ? "Space: pause" : "Space: start",
            C_WHITE, C_BLUE, false);
    text_at(x0 + 110, y0 + 56, "M: mode   N: reset", C_MAG, C_BLUE, false);
    text_at(x0, y0 + 74, "+ / - or Up / Down: dial the minutes",
            C_CYAN, C_BLUE, false);
}

static void timer_reset(void)
{
    running = false;
    left = set_frames;
    elapsed = 0;
    shown = -1;
}

static Boolean timer_key(char ch, short code, Boolean cmd)
{
    if (cmd) return false;
    if (ring_frames) {                /* any key silences the alarm */
        ring_frames = 0;
        music_quiet();
        repaint_all();
        return true;
    }
    if (ch == ' ') {
        running = !running;
        repaint_all();
        return true;
    }
    if (ch == 'n' || ch == 'N') {
        timer_reset();
        repaint_all();
        return true;
    }
    if (ch == 'm' || ch == 'M') {
        stopwatch = !stopwatch;
        timer_reset();
        repaint_all();
        return true;
    }
    if (ch == '+' || ch == '=' || ch == 0x1E) {        /* Up arrow */
        if (set_frames < (long)MAX_MIN * 60 * 60)
            set_frames += 60 * 60;
        if (!running) left = set_frames;
        repaint_all();
        return true;
    }
    if (ch == '-' || ch == 0x1F) {                     /* Down arrow */
        if (set_frames > 60 * 60)
            set_frames -= 60 * 60;
        if (!running) left = set_frames;
        repaint_all();
        return true;
    }
    (void)code;
    return false;
}

/* tick() arrives once per shell frame, ~60/s: the frame IS the clock.
 * Pausing works by not counting - a paused timer simply lets frames by. */
static void timer_tick(void)
{
    long secs;

    if (ring_frames) {
        ring_frames--;
        if (!ring_frames) {           /* rang long enough on its own */
            music_quiet();
            repaint_all();
            return;
        }
        if (--beep_in <= 0) {
            music_note_on(96, 9);
            beep_in = 24;
        }
        if (((ring_frames / 12) & 1) != (long)flash) {
            flash = (Boolean)((ring_frames / 12) & 1);
            repaint_all();
        }
        return;
    }

    if (!running) return;

    if (stopwatch) {
        if (elapsed < CAP_T) elapsed++;
        secs = elapsed / 60;
    } else {
        left--;
        if (left <= 0) {
            left = 0;
            running = false;
            ring_frames = 300;        /* ring for five seconds */
            beep_in = 1;
            repaint_all();
            return;
        }
        secs = (left + 59) / 60;
    }
    if (secs != shown) {              /* repaint once a second, not 60x */
        shown = secs;
        repaint_all();
    }
}

static void timer_opened(void)
{
    music_open_chan();
}

static const AppInterface kIface = {
    timer_draw, timer_key, 0, timer_tick, timer_opened, 0,
    "Timer", { 40, 40, 328, 194 }
};

const AppInterface *uno_app_main(const KernelApi *k)
{
    gK = k;
    return &kIface;
}
